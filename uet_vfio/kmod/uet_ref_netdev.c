// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * The L2 half of the driver. A basic Ethernet netdev over the device's
 * two descriptor rings.
 *
 * This path is deliberately simple rather than fast. Frames are copied into
 * per-slot coherent buffers instead of mapping skb pages, and receive runs
 * straight out of the interrupt handler with no NAPI.
 *
 * Index discipline from the device ABI. Driver owns the producer index and
 * publishes it by writing the ring's doorbell. The device owns the consumer
 * index and publishes it in BAR0. A slot belongs to the device while
 * cons != prod.
 */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>

#include "uet_ref.h"

#define UET_REF_RING_BYTES \
	(UET_REF_RING_ENTRIES * sizeof(struct uet_dev_desc))

#define UET_REF_BUFS_BYTES \
	(UET_REF_RING_ENTRIES * UET_DEV_MAX_FRAME)

static inline struct uet_ref_dev *uet_ref_from_netdev(struct net_device *nd)
{
	return *(struct uet_ref_dev **)netdev_priv(nd);
}

static void uet_ref_ring_free(struct uet_ref_dev *dev,
			      struct uet_ref_ring *ring)
{
	struct device *d = &dev->pdev->dev;

	if (ring->buf[0])
		dma_free_coherent(d, UET_REF_BUFS_BYTES, ring->buf[0],
				  ring->buf_dma[0]);

	if (ring->desc)
		dma_free_coherent(d, UET_REF_RING_BYTES, ring->desc,
				  ring->desc_dma);

	memset(ring, 0, sizeof(*ring));
}

/* One coherent block for the descriptors and one for every frame buffer,
 * carved into slots. Two allocations per ring.
 */
static int uet_ref_ring_alloc(struct uet_ref_dev *dev,
			      struct uet_ref_ring *ring)
{
	struct device *d = &dev->pdev->dev;
	dma_addr_t bufs_dma;
	void *bufs;
	int i;

	memset(ring, 0, sizeof(*ring));

	ring->desc = dma_alloc_coherent(d, UET_REF_RING_BYTES,
					&ring->desc_dma, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	bufs = dma_alloc_coherent(d, UET_REF_BUFS_BYTES, &bufs_dma,
				  GFP_KERNEL);
	if (!bufs) {
		dma_free_coherent(d, UET_REF_RING_BYTES, ring->desc,
				  ring->desc_dma);
		ring->desc = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < UET_REF_RING_ENTRIES; i++) {
		ring->buf[i] = (bufs + (i * UET_DEV_MAX_FRAME));
		ring->buf_dma[i] = (bufs_dma + (i * UET_DEV_MAX_FRAME));
	}

	return 0;
}

/* program a ring's base and size, then enable it */
static void uet_ref_ring_program(struct uet_ref_dev *dev,
				 struct uet_ref_ring *ring,
				 u32 base_lo,
				 u32 base_hi,
				 u32 entries_reg,
				 u32 ctrl_reg)
{
	uet_ref_wr2(dev, base_lo, lower_32_bits(ring->desc_dma));
	uet_ref_wr2(dev, base_hi, upper_32_bits(ring->desc_dma));
	uet_ref_wr2(dev, entries_reg, UET_REF_RING_ENTRIES);
	uet_ref_wr2(dev, ctrl_reg, UET_DEV_RING_CTRL_ENABLE);
}

/* post every receive slot to the device */
static void uet_ref_rx_post_all(struct uet_ref_dev *dev)
{
	struct uet_dev_desc *desc;
	int i;

	for (i = 0; i < UET_REF_RING_ENTRIES; i++) {
		desc = &dev->rx.desc[i];

		desc->addr  = cpu_to_le64(dev->rx.buf_dma[i]);
		desc->len   = cpu_to_le32(UET_DEV_MAX_FRAME);
		desc->flags = 0;
	}

	dev->rx.cons = 0;
	dev->rx.prod = UET_REF_RING_ENTRIES;

	wmb(); /* ensure visible before the doorbell that publishes them */
	uet_ref_wr2(dev, UET_DEV_RX_PROD, dev->rx.prod);
}

static int uet_ref_open(struct net_device *netdev)
{
	struct uet_ref_dev *dev = uet_ref_from_netdev(netdev);
	int ret;

	ret = uet_ref_ring_alloc(dev, &dev->tx);
	if (ret)
		return ret;

	ret = uet_ref_ring_alloc(dev, &dev->rx);
	if (ret) {
		uet_ref_ring_free(dev, &dev->tx);
		return ret;
	}

	uet_ref_ring_program(dev, &dev->tx, UET_DEV_TX_RING_BASE_LO,
			     UET_DEV_TX_RING_BASE_HI, UET_DEV_TX_RING_ENTRIES,
			     UET_DEV_TX_RING_CTRL);

	uet_ref_ring_program(dev, &dev->rx, UET_DEV_RX_RING_BASE_LO,
			     UET_DEV_RX_RING_BASE_HI, UET_DEV_RX_RING_ENTRIES,
			     UET_DEV_RX_RING_CTRL);

	uet_ref_rx_post_all(dev);

	netif_start_queue(netdev);
	netif_carrier_on(netdev);

	return 0;
}

static int uet_ref_stop(struct net_device *netdev)
{
	struct uet_ref_dev *dev = uet_ref_from_netdev(netdev);

	netif_carrier_off(netdev);
	netif_stop_queue(netdev);

	/* clearing enable takes the ring memory back from the device */
	uet_ref_wr2(dev, UET_DEV_TX_RING_CTRL, 0);
	uet_ref_wr2(dev, UET_DEV_RX_RING_CTRL, 0);

	uet_ref_ring_free(dev, &dev->tx);
	uet_ref_ring_free(dev, &dev->rx);

	return 0;
}

/* number of tx slots the device has not yet consumed */
static u32 uet_ref_tx_inflight(struct uet_ref_dev *dev)
{
	return (dev->tx.prod - uet_ref_rd0(dev, UET_DEV_REG_TX_CONS));
}

static netdev_tx_t uet_ref_xmit(struct sk_buff *skb,
				struct net_device *netdev)
{
	struct uet_ref_dev *dev = uet_ref_from_netdev(netdev);
	u32 slot = (dev->tx.prod % UET_REF_RING_ENTRIES);
	struct uet_dev_desc *desc = &dev->tx.desc[slot];
	unsigned int len = skb->len;

	if (len > UET_DEV_MAX_FRAME) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (uet_ref_tx_inflight(dev) >= UET_REF_RING_ENTRIES) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}

	skb_copy_from_linear_data(skb, dev->tx.buf[slot], len);

	desc->addr  = cpu_to_le64(dev->tx.buf_dma[slot]);
	desc->len   = cpu_to_le32(len);
	desc->flags = 0;

	dev->tx.prod++;

	/* The descriptor must be visible to the device before the doorbell
	 * is written that updates the producer index.
	 */
	wmb();
	uet_ref_wr2(dev, UET_DEV_TX_PROD, dev->tx.prod);

	netdev->stats.tx_packets++;
	netdev->stats.tx_bytes += len;

	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

irqreturn_t uet_ref_tx_isr(int irq,
			   void *data)
{
	struct uet_ref_dev *dev = data;

	/* frames are copied at xmit, so completion only frees ring space */
	if (netif_queue_stopped(dev->netdev) &&
	    (uet_ref_tx_inflight(dev) < UET_REF_RING_ENTRIES))
		netif_wake_queue(dev->netdev);

	return IRQ_HANDLED;
}

irqreturn_t uet_ref_rx_isr(int irq,
			   void *data)
{
	struct uet_ref_dev *dev = data;
	struct net_device *netdev = dev->netdev;
	u32 dev_cons = uet_ref_rd0(dev, UET_DEV_REG_RX_CONS);
	bool posted = false;
	struct uet_dev_desc *desc;
	struct sk_buff *skb;
	u32 slot, len;

	while (dev->rx.cons != dev_cons) {
		slot = (dev->rx.cons % UET_REF_RING_ENTRIES);
		desc = &dev->rx.desc[slot];

		rmb(); /* make sure we see the device's writes */

		len = le32_to_cpu(desc->len);

		if (!(le32_to_cpu(desc->flags) & UET_DEV_DESC_DONE) ||
		    (len == 0) || (len > UET_DEV_MAX_FRAME)) {
			netdev->stats.rx_errors++;
			goto repost;
		}

		skb = netdev_alloc_skb_ip_align(netdev, len);
		if (!skb) {
			netdev->stats.rx_dropped++;
			goto repost;
		}

		skb_put_data(skb, dev->rx.buf[slot], len); /* packet copy */
		skb->protocol = eth_type_trans(skb, netdev);

		netdev->stats.rx_packets++;
		netdev->stats.rx_bytes += len;

		netif_rx(skb);

repost:
		desc->addr  = cpu_to_le64(dev->rx.buf_dma[slot]);
		desc->len   = cpu_to_le32(UET_DEV_MAX_FRAME);
		desc->flags = 0;

		dev->rx.cons++;
		dev->rx.prod++;
		posted = true;
	}

	if (posted) {
		/* The descriptors must be visible to the device before the
		 * doorbell is written that updates the producer index.
		 */
		wmb();
		uet_ref_wr2(dev, UET_DEV_RX_PROD, dev->rx.prod);
	}

	return IRQ_HANDLED;
}

static const struct net_device_ops uet_ref_netdev_ops = {
	.ndo_open = uet_ref_open,
	.ndo_stop = uet_ref_stop,
	.ndo_start_xmit = uet_ref_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static void uet_ref_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *info)
{
	struct uet_ref_dev *dev = uet_ref_from_netdev(netdev);
	u32 abi = uet_ref_rd0(dev, UET_DEV_REG_ABI_VERSION);

	strscpy(info->driver, UET_REF_DRV_NAME, sizeof(info->driver));
	snprintf(info->version, sizeof(info->version), "abi %u.%u",
		 abi >> 16, abi & 0xffff);
	strscpy(info->bus_info, pci_name(dev->pdev), sizeof(info->bus_info));
}

/* the ring counters, as ethtool sees them */
static const struct {
	const char *name;
	u32 off;
} uet_ref_eth_stats[] = {
	{ "tx_frames",	   UET_DEV_REG_TX_FRAMES },
	{ "tx_errors",	   UET_DEV_REG_TX_ERRORS },
	{ "rx_frames",	   UET_DEV_REG_RX_FRAMES },
	{ "rx_dropped",	   UET_DEV_REG_RX_DROPPED },
	{ "rx_uet_frames", UET_DEV_REG_RX_UET_FRAMES },
};

static int uet_ref_get_sset_count(struct net_device *netdev,
				  int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return ARRAY_SIZE(uet_ref_eth_stats);
}

static void uet_ref_get_strings(struct net_device *netdev,
				u32 sset,
				u8 *data)
{
	size_t i;

	if (sset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(uet_ref_eth_stats); i++)
		ethtool_puts(&data, uet_ref_eth_stats[i].name);
}

static void uet_ref_get_ethtool_stats(struct net_device *netdev,
				      struct ethtool_stats *stats,
				      u64 *data)
{
	struct uet_ref_dev *dev = uet_ref_from_netdev(netdev);
	size_t i;

	for (i = 0; i < ARRAY_SIZE(uet_ref_eth_stats); i++)
		data[i] = uet_ref_rd0(dev, uet_ref_eth_stats[i].off);
}

static const struct ethtool_ops uet_ref_ethtool_ops = {
	.get_drvinfo = uet_ref_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_sset_count = uet_ref_get_sset_count,
	.get_strings = uet_ref_get_strings,
	.get_ethtool_stats = uet_ref_get_ethtool_stats,
};

int uet_ref_netdev_probe(struct uet_ref_dev *dev)
{
	struct net_device *netdev;
	u8 mac[ETH_ALEN];
	int ret;

	netdev = devm_alloc_etherdev(&dev->pdev->dev,
				     sizeof(struct uet_ref_dev *));
	if (!netdev)
		return -ENOMEM;

	*(struct uet_ref_dev **)netdev_priv(netdev) = dev;
	dev->netdev = netdev;

	SET_NETDEV_DEV(netdev, &dev->pdev->dev);
	netdev->netdev_ops = &uet_ref_netdev_ops;
	netdev->ethtool_ops = &uet_ref_ethtool_ops;
	netdev->mtu = UET_DEV_MTU;

	/* The hardware address comes from the device, which took it from the
	 * interface the reference model is bound to.
	 */
	memcpy_fromio(mac, dev->bar0 + UET_DEV_REG_MAC_LO, ETH_ALEN);
	if (is_valid_ether_addr(mac))
		eth_hw_addr_set(netdev, mac);
	else
		eth_hw_addr_random(netdev);

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(&dev->pdev->dev, "register_netdev: %d\n", ret);
		dev->netdev = NULL;
		return ret;
	}

	netif_carrier_off(netdev);

	dev_info(&dev->pdev->dev, "%s: %pM\n", netdev->name, netdev->dev_addr);

	return 0;
}

void uet_ref_netdev_remove(struct uet_ref_dev *dev)
{
	if (!dev->netdev)
		return;

	unregister_netdev(dev->netdev);
	dev->netdev = NULL;
}

