// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * Descriptor rings.
 *
 * A send queue, a receive queue and a completion queue are all the same
 * thing to the device. Memory the process allocated, pinned here, and
 * handed over as a page list. Only the entry size and who owns the ring
 * differ, so the pinning lives in one place.
 */

#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/iter.h>

#include "uet_ref.h"

int uet_ref_uring_pin(struct uet_ref_dev *dev,
		      struct ib_device *ibdev,
		      u64 addr,
		      u32 entries,
		      u32 entry_size,
		      struct uet_ref_uring *ring)
{
	struct ib_block_iter biter;
	size_t npages, i = 0;
	u64 len;

	if (entries == 0)
		return 0;		/* a ring is optional */

	/* the device wraps the index by masking */
	if (entries & (entries - 1))
		return -EINVAL;

	len = (u64)entries * entry_size;

	ring->umem = ib_umem_get_va(ibdev, addr, len, IB_ACCESS_LOCAL_WRITE);
	if (IS_ERR(ring->umem)) {
		int ret = PTR_ERR(ring->umem);

		ring->umem = NULL;
		return ret;
	}

	npages = ib_umem_num_dma_blocks(ring->umem, PAGE_SIZE);
	if ((npages == 0) || (npages > UET_REF_MR_MAX_PAGES)) {
		ib_umem_release(ring->umem);
		ring->umem = NULL;
		return -EINVAL;
	}

	ring->pbl_bytes = npages * sizeof(u64);
	ring->pbl = dma_alloc_coherent(&dev->pdev->dev, ring->pbl_bytes,
				       &ring->pbl_dma, GFP_KERNEL);
	if (!ring->pbl) {
		ib_umem_release(ring->umem);
		ring->umem = NULL;
		return -ENOMEM;
	}

	rdma_umem_for_each_dma_block(ring->umem, &biter, PAGE_SIZE) {
		if (i >= npages)
			break;

		ring->pbl[i++] =
			cpu_to_le64(rdma_block_iter_dma_address(&biter));
	}

	ring->npages = i;
	ring->entries = entries;

	return 0;
}

void uet_ref_uring_unpin(struct uet_ref_dev *dev,
			 struct uet_ref_uring *ring)
{
	if (ring->pbl) {
		dma_free_coherent(&dev->pdev->dev, ring->pbl_bytes, ring->pbl,
				  ring->pbl_dma);
		ring->pbl = NULL;
	}

	if (ring->umem) {
		ib_umem_release(ring->umem);
		ring->umem = NULL;
	}

	ring->entries = 0;
}

/* describe a pinned ring in the terms the device ABI uses */
void uet_ref_uring_desc(struct uet_ref_uring *ring,
			struct uet_dev_ring_desc *desc)
{
	memset(desc, 0, sizeof(*desc));

	if (ring->entries == 0)
		return;

	desc->pbl_root    = ring->pbl_dma;
	desc->page_size   = PAGE_SIZE;
	desc->level       = UET_DEV_PBL_LEVEL_1;
	     /* where the ring starts inside its first page, a ring the
	      * process allocated need not be page aligned
	      */
	desc->page_offset = ib_umem_offset(ring->umem);
	desc->entries     = ring->entries;
}

