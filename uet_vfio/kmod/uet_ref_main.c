// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/* PCI attach for the UET reference device: BARs, interrupts, netdev. */

#include <linux/module.h>
#include <linux/pci.h>

#include <rdma/ib_verbs.h>

#include "uet_ref.h"

static const struct pci_device_id uet_ref_pci_ids[] = {
	{ PCI_DEVICE(UET_DEV_PCI_VENDOR_ID, UET_DEV_PCI_DEVICE_ID) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, uet_ref_pci_ids);

/* refuse a device whose ABI we were not built against */
static int uet_ref_check_abi(struct uet_ref_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	u32 magic, abi;

	magic = uet_ref_rd0(dev, UET_DEV_REG_MAGIC);

	if (magic != UET_DEV_MAGIC) {
		dev_err(&pdev->dev, "bad magic 0x%08x, expected 0x%08x\n",
			magic, UET_DEV_MAGIC);
		return -ENODEV;
	}

	abi = uet_ref_rd0(dev, UET_DEV_REG_ABI_VERSION);

	if (((abi >> 16) != UET_DEV_ABI_MAJOR) ||
	    ((abi & 0xffff) < UET_DEV_ABI_MINOR)) {
		dev_err(&pdev->dev,
			"device abi %u.%u, driver needs %u.%u or later\n",
			(abi >> 16), (abi & 0xffff), UET_DEV_ABI_MAJOR,
			UET_DEV_ABI_MINOR);
		return -ENODEV;
	}

	dev_info(&pdev->dev, "UET reference device, abi %u.%u, state %u\n",
		 (abi >> 16), (abi & 0xffff),
		 uet_ref_rd0(dev, UET_DEV_REG_STATE));

	return 0;
}

static int uet_ref_setup_irqs(struct uet_ref_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	int nvec, ret;

	nvec = pci_alloc_irq_vectors(pdev, UET_REF_NUM_VECTORS,
				     UET_DEV_MSIX_NUM_VECTORS,
				     PCI_IRQ_MSIX);
	if (nvec < 0) {
		dev_err(&pdev->dev, "no MSI-X vectors: %d\n", nvec);
		return nvec;
	}

	dev->tx_irq = pci_irq_vector(pdev, UET_DEV_TX_VECTOR);
	dev->rx_irq = pci_irq_vector(pdev, UET_DEV_RX_VECTOR);

	ret = request_irq(dev->tx_irq, uet_ref_tx_isr, 0, "uet_ref-tx", dev);
	if (ret) {
		dev_err(&pdev->dev, "request tx irq: %d\n", ret);
		goto err_vectors;
	}

	ret = request_irq(dev->rx_irq, uet_ref_rx_isr, 0, "uet_ref-rx", dev);
	if (ret) {
		dev_err(&pdev->dev, "request rx irq: %d\n", ret);
		goto err_tx;
	}

	dev->admin_irq = pci_irq_vector(pdev, UET_DEV_ADMIN_VECTOR);

	ret = request_irq(dev->admin_irq, uet_ref_admin_isr, 0,
			  "uet_ref-admin", dev);
	if (ret) {
		dev_err(&pdev->dev, "request admin irq: %d\n", ret);
		goto err_rx;
	}

	dev->cq_irq = pci_irq_vector(pdev, UET_DEV_CQ_VECTOR_BASE);

	ret = request_irq(dev->cq_irq, uet_ref_cq_isr, 0, "uet_ref-cq", dev);
	if (ret) {
		dev_err(&pdev->dev, "request cq irq: %d\n", ret);
		goto err_admin;
	}

	dev->irqs_ready = true;

	return 0;

err_admin:
	free_irq(dev->cq_irq, dev);
err_rx:
	free_irq(dev->rx_irq, dev);
err_tx:
	free_irq(dev->tx_irq, dev);
err_vectors:
	pci_free_irq_vectors(pdev);

	return ret;
}

static void uet_ref_free_irqs(struct uet_ref_dev *dev)
{
	if (!dev->irqs_ready)
		return;

	free_irq(dev->cq_irq, dev);
	free_irq(dev->admin_irq, dev);
	free_irq(dev->rx_irq, dev);
	free_irq(dev->tx_irq, dev);
	pci_free_irq_vectors(dev->pdev);
	dev->irqs_ready = false;
}

static int uet_ref_probe(struct pci_dev *pdev,
			 const struct pci_device_id *id)
{
	struct uet_ref_dev *dev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	/* The rdma core requires ib_device to be allocated by it, so the
	 * whole containing structure comes from ib_alloc_device rather than
	 * devm.
	 */
	dev = ib_alloc_device(uet_ref_dev, ib_dev);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	ret = pcim_iomap_regions(pdev, (BIT(0) | BIT(2)), UET_REF_DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "cannot map BAR0/BAR2: %d\n", ret);
		goto err_dealloc;
	}

	INIT_LIST_HEAD(&dev->cq_list);
	spin_lock_init(&dev->cq_lock);

	dev->bar0 = pcim_iomap_table(pdev)[0];
	dev->bar2 = pcim_iomap_table(pdev)[2];

	/* BAR3 is never mapped in the kernel. It exists only to be handed to
	 * processes a page at a time, so the driver keeps its physical
	 * address.
	 */
	dev->bar3_phys = pci_resource_start(pdev, 3);
	if (!dev->bar3_phys ||
	    (pci_resource_len(pdev, 3) < UET_DEV_BAR3_SIZE)) {
		dev_err(&pdev->dev, "BAR3 missing or too small for doorbells\n");
		return -ENODEV;
	}

	ida_init(&dev->db_ida);

	ret = uet_ref_check_abi(dev);
	if (ret)
		goto err_dealloc;

	/* the rings live in ordinary system memory the device reads */
	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "no 64-bit dma: %d\n", ret);
		goto err_dealloc;
	}

	ret = uet_ref_setup_irqs(dev);
	if (ret)
		goto err_dealloc;

	/* the admin queue must exist before verbs can create anything */
	ida_init(&dev->pd_ida);
	ida_init(&dev->pidonfep_ida);

	ret = uet_ref_admin_init(dev);
	if (ret)
		goto err_irqs;

	/* first admin command: learn the device's caps/limits */
	ret = uet_ref_admin_query_dev_info(dev);
	if (ret) {
		dev_err(&pdev->dev, "device info query failed: %d\n", ret);
		goto err_admin;
	}

	ret = uet_ref_netdev_probe(dev);
	if (ret)
		goto err_admin;

	/* the ib_device is associated with the netdev, so it comes second */
	ret = uet_ref_verbs_probe(dev);
	if (ret)
		goto err_netdev;

	return 0;

err_netdev:
	uet_ref_netdev_remove(dev);
err_admin:
	uet_ref_admin_fini(dev);
err_irqs:
	uet_ref_free_irqs(dev);
err_dealloc:
	ib_dealloc_device(&dev->ib_dev);

	return ret;
}

static void uet_ref_remove(struct pci_dev *pdev)
{
	struct uet_ref_dev *dev = pci_get_drvdata(pdev);

	if (!dev)
		return;

	uet_ref_verbs_remove(dev);
	uet_ref_netdev_remove(dev);
	uet_ref_admin_fini(dev);
	ida_destroy(&dev->pidonfep_ida);
	ida_destroy(&dev->pd_ida);
	ida_destroy(&dev->db_ida);
	uet_ref_free_irqs(dev);
	ib_dealloc_device(&dev->ib_dev);
}

static struct pci_driver uet_ref_pci_driver = {
	.name = UET_REF_DRV_NAME,
	.id_table = uet_ref_pci_ids,
	.probe = uet_ref_probe,
	.remove = uet_ref_remove,
};

module_pci_driver(uet_ref_pci_driver);

MODULE_AUTHOR("Eric Davis");
MODULE_DESCRIPTION("Ultra Ethernet reference device driver");
MODULE_LICENSE("Dual BSD/GPL");


