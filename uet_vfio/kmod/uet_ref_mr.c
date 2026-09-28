// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * Memory regions.
 *
 * A region is the application's own memory, so what crosses to the device is
 * a description of where those pages are, not a copy of them. The driver
 * pins the user's pages and writes their addresses into a page buffer list
 * in memory the device can read; the device hands that list to the reference
 * implementation, which walks it whenever the region is accessed.
 *
 * Only a single level of page list is built. A level-1 list is a flat array
 * of page addresses, which the library requires to be contiguous for the
 * whole region, so the largest region is bounded by the largest contiguous
 * allocation this driver can make. That is a real limit and it is checked
 * rather than assumed - a two-level list is the way past it, and belongs
 * with the first workload that needs one.
 */

#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>
#include <rdma/iter.h>

#include "uet_ref.h"
#include "uet_ref-abi.h"

static u64 uet_ref_mr_access(int ib_access)
{
	u64 access = UET_DEV_MR_ACCESS_LOCAL_READ;

	if (ib_access & IB_ACCESS_LOCAL_WRITE)
		access |= UET_DEV_MR_ACCESS_LOCAL_WRITE;
	if (ib_access & IB_ACCESS_REMOTE_READ)
		access |= UET_DEV_MR_ACCESS_REMOTE_READ;
	if (ib_access & IB_ACCESS_REMOTE_WRITE)
		access |= UET_DEV_MR_ACCESS_REMOTE_WRITE;
	if (ib_access & IB_ACCESS_REMOTE_ATOMIC)
		access |= UET_DEV_MR_ACCESS_REMOTE_ATOMIC;

	return access;
}

static void uet_ref_mr_free_pbl(struct uet_ref_dev *dev,
				struct uet_ref_mr *mr)
{
	size_t i;

	if (mr->pbl_dirs) {
		for (i = 0; i < mr->pbl_ndirs; i++) {
			if (!mr->pbl_dirs[i])
				continue;
			dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
					  mr->pbl_dirs[i], mr->pbl_dir_dma[i]);
		}
		kfree(mr->pbl_dirs);
		kfree(mr->pbl_dir_dma);
		mr->pbl_dirs = NULL;
		mr->pbl_dir_dma = NULL;
		mr->pbl_ndirs = 0;
	}

	if (!mr->pbl)
		return;

	dma_free_coherent(&dev->pdev->dev, mr->pbl_bytes, mr->pbl,
			  mr->pbl_dma);
	mr->pbl = NULL;
}

/* write the region's page addresses where the device can read them */
static int uet_ref_mr_build_pbl(struct uet_ref_dev *dev,
				struct uet_ref_mr *mr)
{
	struct ib_block_iter biter;
	size_t npages, per_dir, ndirs, d, i = 0;
	int ret;

	npages = ib_umem_num_dma_blocks(mr->umem, PAGE_SIZE);
	if (npages == 0)
		return -EINVAL;

	if (npages > UET_REF_MR_MAX_PAGES) {
		dev_err(&dev->pdev->dev,
			"region needs %zu pages, this driver builds at most %u\n",
			npages, UET_REF_MR_MAX_PAGES);
		return -EOPNOTSUPP;
	}

	/*
	 * One level while the flat array is small, two when it is not.
	 *
	 * A one-level list is a single contiguous array of page addresses,
	 * which is the cheapest thing to build and to walk - but it needs
	 * npages * 8 bytes contiguous, and that is what put a ceiling on
	 * how large a region could be. Above a page's worth of entries the
	 * list gets a root of directory addresses instead, and no single
	 * allocation is larger than a page except the root itself.
	 */
	per_dir = PAGE_SIZE / sizeof(u64);

	if (npages <= per_dir) {
		mr->pbl_level = UET_DEV_PBL_LEVEL_1;
		mr->pbl_bytes = npages * sizeof(u64);
		mr->pbl = dma_alloc_coherent(&dev->pdev->dev, mr->pbl_bytes,
					     &mr->pbl_dma, GFP_KERNEL);
		if (!mr->pbl)
			return -ENOMEM;

		rdma_umem_for_each_dma_block(mr->umem, &biter, PAGE_SIZE) {
			if (i >= npages)
				break;
			mr->pbl[i++] =
				cpu_to_le64(rdma_block_iter_dma_address(&biter));
		}

		mr->npages = i;

		return 0;
	}

	mr->pbl_level = UET_DEV_PBL_LEVEL_2;
	ndirs = DIV_ROUND_UP(npages, per_dir);

	mr->pbl_dirs = kcalloc(ndirs, sizeof(*mr->pbl_dirs), GFP_KERNEL);
	mr->pbl_dir_dma = kcalloc(ndirs, sizeof(*mr->pbl_dir_dma), GFP_KERNEL);
	if (!mr->pbl_dirs || !mr->pbl_dir_dma) {
		ret = -ENOMEM;
		goto err;
	}
	mr->pbl_ndirs = ndirs;

	mr->pbl_bytes = ndirs * sizeof(u64);
	mr->pbl = dma_alloc_coherent(&dev->pdev->dev, mr->pbl_bytes,
				     &mr->pbl_dma, GFP_KERNEL);
	if (!mr->pbl) {
		ret = -ENOMEM;
		goto err;
	}

	for (d = 0; d < ndirs; d++) {
		mr->pbl_dirs[d] = dma_alloc_coherent(&dev->pdev->dev, PAGE_SIZE,
						     &mr->pbl_dir_dma[d],
						     GFP_KERNEL);
		if (!mr->pbl_dirs[d]) {
			ret = -ENOMEM;
			goto err;
		}
		mr->pbl[d] = cpu_to_le64(mr->pbl_dir_dma[d]);
	}

	rdma_umem_for_each_dma_block(mr->umem, &biter, PAGE_SIZE) {
		if (i >= npages)
			break;
		mr->pbl_dirs[i / per_dir][i % per_dir] =
			cpu_to_le64(rdma_block_iter_dma_address(&biter));
		i++;
	}

	mr->npages = i;

	return 0;

err:
	uet_ref_mr_free_pbl(dev, mr);
	return ret;
}

/* Pin a range, describe it to the device, and register it.
 *
 * The part both registration paths share. The caller has already decided
 * everything UET-specific - the access class, the job, a requested key -
 * and passes it in; what happens here is the same either way, and having
 * it in one place is what keeps the ioctl method and the legacy write
 * command from drifting into two subtly different registrations.
 */
static int uet_ref_mr_register(struct ib_pd *ibpd,
			       u64 start,
			       u64 length,
			       u64 virt_addr,
			       int access_flags,
			       int fd,
			       u64 fd_offset,
			       struct uet_dev_admin_mr_reg *args,
			       struct uet_ref_mr **out)
{
	struct uet_ref_dev *dev = container_of(ibpd->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_pd *pd = container_of(ibpd, struct uet_ref_pd, ibpd);
	struct uet_ref_mr *mr;
	u64 param[4];
	u64 result[2];
	int ret;

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return -ENOMEM;

	/* Pages the process owns, or pages somebody else owns and exported
	 * through a dma-buf. Everything below is the same either way. A
	 * dma-buf umem is walked with the same block iterator and released
	 * by the same ib_umem_release().
	 */
	if (fd >= 0) {
		struct ib_umem_dmabuf *umem_dmabuf;

		umem_dmabuf = ib_umem_dmabuf_get_pinned(ibpd->device,
							fd_offset, length, fd,
							access_flags);
		if (IS_ERR(umem_dmabuf)) {
			ret = PTR_ERR(umem_dmabuf);
			goto err_free;
		}

		mr->umem = &umem_dmabuf->umem;
	} else {
		mr->umem = ib_umem_get_va(ibpd->device, start, length,
					  access_flags);
		if (IS_ERR(mr->umem)) {
			ret = PTR_ERR(mr->umem);
			mr->umem = NULL;
			goto err_free;
		}
	}

	ret = uet_ref_mr_build_pbl(dev, mr);
	if (ret)
		goto err_umem;

	args->access = uet_ref_mr_access(access_flags);
	args->page_offset = (u32)((fd >= 0 ? fd_offset : start) & ~PAGE_MASK);
	args->pd = pd->pdn;

	param[0] = mr->pbl_dma;
	param[1] = (u64)PAGE_SIZE | ((u64)mr->pbl_level << 32);
	/*
	 * base_va is the address by which the region is named, and zero
	 * selects the convention rather than being a degenerate address: a
	 * region registered at zero is named by offsets from its start, one
	 * registered at an address is named by virtual addresses.
	 *
	 * UET regions are zero-based on the wire. That is the convention the
	 * UET verbs ABI defines - ibv_ru_rma names every remote buffer as an
	 * offset - so a region a peer can reach must be registered that way,
	 * and one registered at its iova reads back as "Invalid Buffer
	 * Offset" at the responder for every RMA request.
	 *
	 * The iova still travels: the device keeps it so it can turn the
	 * application's own addresses, which is what a work request's
	 * segment list carries, into offsets from the region's start.
	 */
	param[2] = virt_addr;
	param[3] = length;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_MR_REG, param,
				ARRAY_SIZE(param), args, sizeof(*args),
				result);
	if (ret)
		goto err_pbl;

	mr->handle = (u32)result[0];
	mr->key = result[1];

	/*
	 * The device keys each region off its handle, so the handle is the
	 * raw memory key and the 64-bit value is its UET-formatted form.
	 * Verbs carries the raw key in its 32-bit fields; the formatted key
	 * is what goes on the wire.
	 */
	mr->ibmr.lkey = mr->handle;
	mr->ibmr.rkey = mr->handle;
	mr->ibmr.lkey64 = mr->key;
	mr->ibmr.rkey64 = mr->key;
	mr->ibmr.length = length;
	mr->ibmr.iova = virt_addr;

	*out = mr;

	return 0;

err_pbl:
	uet_ref_mr_free_pbl(dev, mr);
err_umem:
	ib_umem_release(mr->umem);
err_free:
	kfree(mr);

	return ret;
}

/* undo a successful uet_ref_mr_register() */
static void uet_ref_mr_unregister(struct uet_ref_dev *dev,
				  struct uet_ref_mr *mr)
{
	u64 param[1] = { mr->handle };

	uet_ref_admin_cmd(dev, UET_DEV_ADMIN_MR_DEREG, param, 1, NULL, 0,
			  NULL);
	uet_ref_mr_free_pbl(dev, mr);
	ib_umem_release(mr->umem);
	kfree(mr);
}

struct ib_mr *uet_ref_reg_user_mr(struct ib_pd *ibpd,
				  u64 start,
				  u64 length,
				  u64 virt_addr,
				  int access_flags,
				  struct ib_dmah *dmah,
				  struct ib_udata *udata)
{
	struct uet_ref_dev *dev = container_of(ibpd->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_dev_admin_mr_reg args = {};
	struct uet_ref_mr *mr;
	int ret;

	/* no DMA handle here: refuse one rather than register without it */
	if (dmah)
		return ERR_PTR(-EOPNOTSUPP);

	/*
	 * ibv_reg_mr() has no way to say which queue pair a region is for, so
	 * the only sensible default is the least restrictive class that is
	 * still scoped to the protection domain: unrestricted, {Device, PD}.
	 * That is how a region behaves everywhere else in verbs, and an
	 * application that never calls ibv_attach_mr() - which is most of
	 * them, including ibv_ru_pingpong - expects exactly that.
	 *
	 * Narrowing to a queue pair is what ibv_attach_mr() is for. Naming a
	 * job, choosing a key, or carving a region out of another is what
	 * ibv_reg_mr_ex() is for, and that reaches ->reg_user_mr_ex() rather
	 * than here.
	 */
	args.access_class = UET_DEV_MR_CLASS_UNRESTRICTED;

	ret = uet_ref_mr_register(ibpd, start, length, virt_addr, access_flags,
				  -1, 0, &args, &mr);
	if (ret)
		return ERR_PTR(ret);

	/* the wide key has no room in the core response */
	if (udata && udata->outlen) {
		struct uet_ref_ib_reg_mr_resp resp = {};

		uet_ref_key64_set(&resp, mr->key);

		ret = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (ret) {
			uet_ref_mr_unregister(dev, mr);
			return ERR_PTR(ret);
		}
	}

	return &mr->ibmr;
}

/* Registration that ibv_reg_mr() cannot express. A key the application
 * chose, a region carved out of another, a region bound to a job. ib_core
 * parses all of it and resolves the objects, so what arrives here is a
 * job key and a parent region rather than numbers userspace supplied.
 */
struct ib_mr *uet_ref_reg_user_mr_ex(struct ib_pd *ibpd,
				     struct ib_mr_ex_attr *attr,
				     struct ib_udata *udata)
{
	struct uet_dev_admin_mr_reg args = {};
	struct uet_ref_mr *mr;
	u64 rkey64;
	int ret;

	/* A placement handle is a hint about where the region should live,
	 * and this device has no notion of one. A dma-buf it can take: the
	 * pages are walked the same way whoever owns them.
	 */
	if (attr->dmah)
		return ERR_PTR(-EOPNOTSUPP);

	if (attr->jkey) {
		args.jkey = attr->jkey->jkey;
		args.access_class = UET_DEV_MR_CLASS_JOB_RESTRICTED;
	} else {
		args.access_class = UET_DEV_MR_CLASS_UNRESTRICTED;
	}

	/* Idempotent-safe is a bit inside the key rather than a property
	 * beside it. ib_core has already refused the combination that asks
	 * for it without a caller-chosen key, so setting it here is safe.
	 */
	if (attr->flags & IB_UVERBS_REG_MR_USER_RKEY) {
		rkey64 = attr->req_rkey64;

		if (attr->flags & IB_UVERBS_REG_MR_IDEMPOTENT_SAFE)
			rkey64 |= UET_DEV_MR_KEY_IDEMPOTENT_SAFE;

		args.requested_key = rkey64;
		args.flags |= UET_DEV_MR_FLAG_USER_KEY;
	}

	ret = uet_ref_mr_register(ibpd, attr->addr, attr->length, attr->iova,
				  (int)attr->access_flags, attr->fd,
				  attr->fd_offset, &args, &mr);
	if (ret)
		return ERR_PTR(ret);

	return &mr->ibmr;
}

struct ib_mr *uet_ref_reg_user_mr_dmabuf(struct ib_pd *ibpd,
					 u64 offset,
					 u64 length,
					 u64 virt_addr,
					 int fd,
					 int access_flags,
					 struct ib_dmah *dmah,
					 struct uverbs_attr_bundle *attrs)
{
	struct uet_dev_admin_mr_reg args = {};
	struct uet_ref_mr *mr;
	int ret;

	if (dmah)
		return ERR_PTR(-EOPNOTSUPP);

	/* The same default an ordinary ibv_reg_mr() gets, see above. A
	 * dma-buf that also names a job or chooses a key does not come
	 * through here. That reaches ->reg_user_mr_ex() which takes an fd of
	 * its own.
	 */
	args.access_class = UET_DEV_MR_CLASS_UNRESTRICTED;

	ret = uet_ref_mr_register(ibpd, 0, length, virt_addr, access_flags, fd,
				  offset, &args, &mr);
	if (ret)
		return ERR_PTR(ret);

	return &mr->ibmr;
}

int uet_ref_dereg_mr(struct ib_mr *ibmr,
		     struct ib_udata *udata)
{
	struct uet_ref_mr *mr = container_of(ibmr, struct uet_ref_mr, ibmr);
	struct uet_ref_dev *dev = container_of(ibmr->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { mr->handle };
	int ret;

	/* Release on the device first. The pages must stay pinned and the
	 * page list readable until it has stopped referring to them.
	 */
	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_MR_DEREG, param, 1, NULL,
				0, NULL);
	if (ret)
		return ret;

	uet_ref_mr_free_pbl(dev, mr);
	ib_umem_release(mr->umem);
	kfree(mr);

	return 0;
}

