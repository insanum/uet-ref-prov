/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - device core
 *
 * The device's behavior, with no knowledge of how it is being driven.
 * uet_dev_vfio.c drives it over the vfio-user protocol.
 */

#ifndef _UET_DEV_CORE_H_
#define _UET_DEV_CORE_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>	/* ssize_t */

#include "uet_dev_abi.h"

struct uet_dev;

/* callbacks for how the device reaches the address space it is serving */
struct uet_dev_backend {
	/* Copy len bytes in/out of guest memory at addr. Copying rather than
	 * resolving a pointer keeps the mapping lifetime entirely inside the
	 * backend. 0 on success, -1 on failure.
	 */
	int (*dma_read)(void *ctx, uint64_t addr, void *buf, size_t len);
	int (*dma_write)(void *ctx, uint64_t addr, const void *buf,
			 size_t len);

	/* Resolve a guest address to a pointer valid for len bytes, or NULL.
	 * Unlike the DMA calls this hands back the guest's own memory.
	 */
	void *(*dma_map)(void *ctx, uint64_t addr, size_t len);

	/* raise an interrupt on the given vector */
	void (*raise_irq)(void *ctx, unsigned int vector);

	void *ctx;
};

/* Bring the device up: create a uet instance, bind the nic shim, create a
 * domain and start the progress thread. Configuration comes from the
 * environment, exactly as it does for the standalone app, because the
 * device model is the process that owns that environment.
 */
struct uet_dev *uet_dev_create(const struct uet_dev_backend *backend);

/* stop the progress thread and tear the instance down */
void uet_dev_destroy(struct uet_dev *dev);

/* log if tracing is on */
void uet_dev_trace_announce(void);

/* Note there is nothing here for BAR1 as that is completely handled in the
 * vfio-user wrapper managing MSI-X/PBA with the PCIe interface in the guest
 * kernel.
 */

/* BAR0 access. Reads are served from live device state. Writes are ignored
 * but are counted, so a driver poking at it is visible rather than silent.
 */
ssize_t uet_dev_bar0_read(struct uet_dev *dev, uint64_t offset,
			  void *buf, size_t len);
ssize_t uet_dev_bar0_write(struct uet_dev *dev, uint64_t offset,
			   const void *buf, size_t len);

/* BAR2 access, the control region. A control doorbell write is a ring, the
 * device records the value, counts it, and raises the backend. Reads return
 * zero.
 */
ssize_t uet_dev_bar2_read(struct uet_dev *dev, uint64_t offset,
			  void *buf, size_t len);
ssize_t uet_dev_bar2_write(struct uet_dev *dev, uint64_t offset,
			   const void *buf, size_t len);

/* BAR3 access, userspace doorbell region. A doorbell write is a ring, the
 * device records the value, counts it, and raises the backend. Device
 * verifies doorbell page mapping to resource assignment. Reads return
 * zero.
 */
ssize_t uet_dev_bar3_read(struct uet_dev *dev, uint64_t offset,
			  void *buf, size_t count);
ssize_t uet_dev_bar3_write(struct uet_dev *dev, uint64_t offset,
			   const void *buf, size_t count);

#endif /* _UET_DEV_CORE_H_ */

