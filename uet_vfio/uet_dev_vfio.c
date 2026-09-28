/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - libvfio-user glue
 *
 * Presents the device core over the vfio-user protocol so a hypervisor can
 * attach to it as a PCI device. This file interacts with the device
 * implementation that lives in uet_dev_core.c.
 *
 * usage: uet_dev_vfio <socket-path>
 */

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alloca.h>
#include <pthread.h>
#include <sys/mman.h>

#include <libvfio-user.h>
#include <pci_caps/msix.h>
#include <pci_caps/px.h>

#include "uet_dev_core.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void uet_dev_vfio_log(vfu_ctx_t *vfu_ctx,
			     int level,
			     const char *msg)
{
	(void)vfu_ctx;
	fprintf(stderr, "uet_dev[%d]: %s\n", level, msg);
}

static vfu_ctx_t *dma_vfu_ctx = NULL;
static vfu_ctx_t *irq_vfu_ctx = NULL;

/*
 * Guest memory regions, recorded as the client registers them.
 *
 * Keeping our own table is what lets a memory region be the guest's actual
 * memory. Translation becomes a lookup plus an offset, with no mapping
 * reference to acquire per access.
 */
struct dma_region {
	uint64_t iova;
	size_t len;
	void *vaddr;
};

/* maximum number of guest memory regions the client may register with us */
#define UET_DEV_DMA_MAX_REGIONS		64

static struct dma_region dma_regions[UET_DEV_DMA_MAX_REGIONS];
static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;

static void uet_dev_dma_register(vfu_ctx_t *vfu_ctx,
				 vfu_dma_info_t *info)
{
	size_t i;

	(void)vfu_ctx;

	if (info->vaddr == NULL)
		return;	/* not mappable */

	pthread_mutex_lock(&dma_lock);

	for (i = 0; i < UET_DEV_DMA_MAX_REGIONS; i++) {
		if (dma_regions[i].vaddr == NULL) {
			dma_regions[i].iova =
				(uint64_t)(uintptr_t)info->iova.iov_base;
			dma_regions[i].len = info->iova.iov_len;
			dma_regions[i].vaddr = info->vaddr;
			break;
		}
	}

	pthread_mutex_unlock(&dma_lock);
}

static void uet_dev_dma_unregister(vfu_ctx_t *vfu_ctx,
				   vfu_dma_info_t *info)
{
	uint64_t iova = (uint64_t)(uintptr_t)info->iova.iov_base;
	size_t i;

	(void)vfu_ctx;

	pthread_mutex_lock(&dma_lock);

	for (i = 0; i < UET_DEV_DMA_MAX_REGIONS; i++) {
		if (dma_regions[i].iova == iova) {
			memset(&dma_regions[i], 0, sizeof(dma_regions[i]));
			break;
		}
	}

	pthread_mutex_unlock(&dma_lock);
}

/* DMA callbacks. libvfio-user resolves a guest address to a scatter/gather
 * list, then copies through it. The copy form means no mapping outlives the
 * call, so the device model never holds a reference into guest memory.
 */
static int uet_dev_dma_xfer(uint64_t addr,
			    void *buf,
			    size_t len,
			    bool write)
{
	dma_sg_t *sgl;
	int nr, ret;

	if (dma_vfu_ctx == NULL)
		return -1;

	sgl = alloca(dma_sg_size());

	nr = vfu_addr_to_sgl(dma_vfu_ctx, (vfu_dma_addr_t)addr, len, sgl, 1,
			     write ? PROT_WRITE : PROT_READ);
	if (nr != 1)
		return -1;

	/* DIRECT_ACCESS uses the client's fd mapping instead of an IPC
	 * round trip. Guest RAM shared via memfd is always supported.
	 */
	ret = write ? vfu_sgl_write(dma_vfu_ctx, sgl, 1, buf,
				    VFU_SGL_DIRECT_ACCESS)
		    : vfu_sgl_read(dma_vfu_ctx, sgl, 1, buf,
				   VFU_SGL_DIRECT_ACCESS);

	return (ret == 0) ? 0 : -1;
}

static int uet_dev_dma_read(void *ctx,
			    uint64_t addr,
			    void *buf,
			    size_t len)
{
	(void)ctx;

	return uet_dev_dma_xfer(addr, buf, len, false);
}

static int uet_dev_dma_write(void *ctx,
			     uint64_t addr,
			     const void *buf,
			     size_t len)
{
	(void)ctx;

	return uet_dev_dma_xfer(addr, (void *)(uintptr_t)buf, len, true);
}

static void *uet_dev_dma_map(void *ctx,
			     uint64_t addr,
			     size_t len)
{
	void *p = NULL;
	size_t i;

	(void)ctx;

	pthread_mutex_lock(&dma_lock);

	for (i = 0; i < UET_DEV_DMA_MAX_REGIONS; i++) {
		struct dma_region *r = &dma_regions[i];

		if ((r->vaddr != NULL) &&
		    (addr >= r->iova) &&
		    ((addr - r->iova) < r->len) &&
		    (len <= (r->len - (addr - r->iova)))) {
			p = ((uint8_t *)r->vaddr + (addr - r->iova));
			break;
		}
	}

	pthread_mutex_unlock(&dma_lock);

	return p;
}

/* The device core rings through this callback. Here it becomes a vfio-user
 * interrupt. vfu_irq_trigger writes the eventfd the client registered for
 * the vector.
 */
static void uet_dev_raise_irq(void *ctx,
			      unsigned int vector)
{
	(void)ctx;

	if (irq_vfu_ctx != NULL)
		vfu_irq_trigger(irq_vfu_ctx, vector);
}

/* BAR0 region access. This read-only region is trapped rather than mmap'd,
 * so every guest access arrives here as a message. Writes are ignored though
 * counted.
 */
static ssize_t uet_dev_bar0_access(vfu_ctx_t *vfu_ctx,
				   char *buf,
				   size_t count,
				   loff_t offset,
				   bool is_write)
{
	struct uet_dev *dev = vfu_get_private(vfu_ctx);

	if (is_write)
		return uet_dev_bar0_write(dev, (uint64_t)offset, buf, count);

	return uet_dev_bar0_read(dev, (uint64_t)offset, buf, count);
}

/* BAR1 backs the MSI-X table and PBA. The client virtualizes the table for
 * the guest and delivers interrupts through eventfds it registers with
 * SET_IRQS, so plain storage served over the protocol is all the device
 * needs to provide here.
 */
static uint8_t bar1[UET_DEV_BAR1_SIZE];

static ssize_t uet_dev_bar1_access(vfu_ctx_t *vfu_ctx,
				   char *buf,
				   size_t count,
				   loff_t offset,
				   bool is_write)
{
	(void)vfu_ctx;

	if (((size_t)offset > sizeof(bar1)) ||
	    (count > (sizeof(bar1) - (size_t)offset))) {
		errno = EINVAL;
		return -1;
	}

	if (is_write)
		memcpy(&bar1[offset], buf, count);
	else
		memcpy(buf, &bar1[offset], count);

	return (ssize_t)count;
}

/* BAR2 region access. This write-only region is trapped rather than mmap'd,
 * so every guest access arrives here as a message. Reads are accepted though
 * return 0 as all status is located in BAR0 registers.
 */
static ssize_t uet_dev_bar2_access(vfu_ctx_t *vfu_ctx,
				   char *buf,
				   size_t count,
				   loff_t offset,
				   bool is_write)
{
	struct uet_dev *dev = vfu_get_private(vfu_ctx);

	if (is_write)
		return uet_dev_bar2_write(dev, (uint64_t)offset, buf, count);

	return uet_dev_bar2_read(dev, (uint64_t)offset, buf, count);
}

/* BAR3 region access. The write-only user doorbell pages. Which page a write
 * lands on is the only evidence of who wrote it, so the offset is passed
 * through unchanged and the device model does the ownership check. Reads
 * are accepted though return 0.
 */
static ssize_t uet_dev_bar3_access(vfu_ctx_t *vfu_ctx,
				   char *buf,
				   size_t count,
				   loff_t offset,
				   bool is_write)
{
	struct uet_dev *dev = vfu_get_private(vfu_ctx);

	if (is_write)
		return uet_dev_bar3_write(dev, (uint64_t)offset, buf, count);

	return uet_dev_bar3_read(dev, (uint64_t)offset, buf, count);
}

int main(int argc, char **argv)
{
	struct uet_dev *dev = NULL;
	vfu_ctx_t *vfu_ctx = NULL;
	const char *sock;
	int rc = EXIT_FAILURE;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <socket-path>\n", argv[0]);
		return EXIT_FAILURE;
	}

	sock = argv[1];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* bring the device up before the socket */
	{
		struct uet_dev_backend backend = {
			.dma_read = uet_dev_dma_read,
			.dma_write = uet_dev_dma_write,
			.dma_map = uet_dev_dma_map,
			.raise_irq = uet_dev_raise_irq,
		};

		dev = uet_dev_create(&backend);
	}

	if (dev == NULL) {
		fprintf(stderr, "uet_dev: device did not come up\n");
		return EXIT_FAILURE;
	}

	/* libvfio-user only unlinks its socket on a clean shutdown, so a
	 * previous run that was killed leaves one behind and the next bind
	 * fails with EADDRINUSE. Remove it here so we can create a new one.
	 */
	unlink(sock);

	vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, sock, 0, dev,
				 VFU_DEV_TYPE_PCI);
	if (vfu_ctx == NULL) {
		fprintf(stderr, "uet_dev: vfu_create_ctx: %s\n",
			strerror(errno));
		goto out;
	}

	vfu_setup_log(vfu_ctx, uet_dev_vfio_log, LOG_INFO);

	/* initialize PCI Express */
	if (vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS,
			 PCI_HEADER_TYPE_NORMAL, 0) < 0) {
		fprintf(stderr, "uet_dev: vfu_pci_init: %s\n",
			strerror(errno));
		goto out;
	}

	dma_vfu_ctx = vfu_ctx;
	irq_vfu_ctx = vfu_ctx;

	vfu_pci_set_id(vfu_ctx, UET_DEV_PCI_VENDOR_ID, UET_DEV_PCI_DEVICE_ID,
		       0, 0);
	vfu_pci_set_class(vfu_ctx, UET_DEV_PCI_CLASS, UET_DEV_PCI_SUBCLASS, 0);

	/* VFU_PCI_TYPE_EXPRESS above only sizes the configuration space. The
	 * express capability itself must be added or the guest enumerates a
	 * conventional endpoint. Also MSI-X is an express capability, so it
	 * hangs off this.
	 */
	{
		struct pxcap px = {
			.hdr = { .id = 0x10 }, /* PCI_CAP_ID_EXP */
			.pxcaps = { .ver = 2, .dpt = 0 }, /* v2 endpoint */
		};

		struct msixcap msix = {
			.hdr = { .id = 0x11 }, /* PCI_CAP_ID_MSIX */
			.mxc = { .ts = (UET_DEV_MSIX_NUM_VECTORS - 1) },
			.mtab = { .tbir = 1, /* table in BAR1 */
				  .to = (UET_DEV_MSIX_TABLE_OFF >> 3) },
			.mpba = { .pbir = 1, /* pba in BAR1 too */
				  .pbao = (UET_DEV_MSIX_PBA_OFF >> 3) },
		};

		if (vfu_pci_add_capability(vfu_ctx, 0, 0, &px) < 0) {
			fprintf(stderr, "uet_dev: add PCIe pxcap: %s\n",
				strerror(errno));
			goto out;
		}

		if (vfu_pci_add_capability(vfu_ctx, 0, 0, &msix) < 0) {
			fprintf(stderr, "uet_dev: add PCIe msixcap: %s\n",
				strerror(errno));
			goto out;
		}
	}

	/* VFU_REGION_FLAG_MEM matters, without it the BARs are advertised as
	 * I/O space, the guest assigns it I/O ports, and sysfs resource0
	 * cannot be mapped at all. A status region a driver will mmap has to
	 * be memory.
	 */

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX,
			       UET_DEV_BAR0_SIZE, uet_dev_bar0_access,
			       (VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM),
			       NULL, 0, -1, 0);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: PCIe BAR0: %s\n",
			strerror(errno));
		goto out;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR1_REGION_IDX,
			       UET_DEV_BAR1_SIZE, uet_dev_bar1_access,
			       (VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM),
			       NULL, 0, -1, 0);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: PCIe BAR1: %s\n", strerror(errno));
		goto out;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR2_REGION_IDX,
			       UET_DEV_BAR2_SIZE, uet_dev_bar2_access,
			       (VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM),
			       NULL, 0, -1, 0);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: PCIe BAR2: %s\n", strerror(errno));
		goto out;
	}

	ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR3_REGION_IDX,
			       UET_DEV_BAR3_SIZE, uet_dev_bar3_access,
			       (VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM),
			       NULL, 0, -1, 0);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: PCIe BAR3: %s\n", strerror(errno));
		goto out;
	}

	/* required before any vfu_addr_to_sgl call */
	ret = vfu_setup_device_dma(vfu_ctx, UET_DEV_DMA_MAX_REGIONS,
				   uet_dev_dma_register,
				   uet_dev_dma_unregister);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: setup dma: %s\n", strerror(errno));
		goto out;
	}

	ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ,
				       UET_DEV_MSIX_NUM_VECTORS);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: setup irqs: %s\n", strerror(errno));
		goto out;
	}

	ret = vfu_realize_ctx(vfu_ctx);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: vfu_realize_ctx: %s\n",
			strerror(errno));
		goto out;
	}

	uet_dev_trace_announce();

	printf("uet_dev: listening on %s (%04x:%04x)\n", sock,
	       UET_DEV_PCI_VENDOR_ID, UET_DEV_PCI_DEVICE_ID);
	printf("uet_dev: waiting for a client to attach...\n");

	ret = vfu_attach_ctx(vfu_ctx);
	if (ret < 0) {
		fprintf(stderr, "uet_dev: vfu_attach_ctx: %s\n",
			strerror(errno));
		goto out;
	}

	printf("uet_dev: attached\n");

	while (!stop) {
		ret = vfu_run_ctx(vfu_ctx);
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			if (errno == ENOTCONN) {
				printf("uet_dev: client detached\n");
				break;
			}

			fprintf(stderr, "uet_dev: vfu_run_ctx: %s\n",
				strerror(errno));

			goto out;
		}
	}

	rc = EXIT_SUCCESS;

out:
	if (vfu_ctx != NULL)
		vfu_destroy_ctx(vfu_ctx);

	uet_dev_destroy(dev);
	unlink(sock);

	return rc;
}

