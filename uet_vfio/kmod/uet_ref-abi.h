/* SPDX-License-Identifier: ((GPL-2.0-only WITH Linux-syscall-note) OR Linux-OpenIB) */

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * uet_ref: the kernel/user ABI for this driver.
 *
 * The UET/RU verbs ABI contains the objects, methods, and attributes any
 * UET device exposes. These are what a second vendor would implement and is
 * in ib_core. See rdma/ib_user_ioctl_cmds.h and rdma/ib_user_ioctl_verbs.h.
 *
 * Everything here contains definition specific uet_ref driver and its
 * associated provider driver required for resource management.
 */

#ifndef UET_REF_ABI_USER_H
#define UET_REF_ABI_USER_H

/* Version of the private command and response structures below. Bumped
 * when one of them changes shape. ibv_get_device_list() refuses a provider
 * whose number does not match the driver's.
 */
#define UET_REF_UVERBS_ABI_VERSION	1

#include <linux/types.h>

/* The rings live in the process's own memory. It allocates them, the kernel
 * pins them and hands the device a page list. The process then writes
 * descriptors into memory it already owns. Nothing is mapped back to
 * userspace except the doorbell page.
 *
 * The doorbell page is one page of device memory per ucontext, obtained with
 * mmap() at the offset reported below. The device refuses a doorbell whose
 * handle is not owned by the context the page belongs to, so a process cannot
 * ring another's queue.
 */
struct uet_ref_ib_alloc_ucontext_resp {
	__aligned_u64 db_mmap_offset;	/* pass to mmap() for the doorbell */
	__u32 db_page;			/* which page it is, for diagnostics */
	__u32 max_sge;			/* segments accepted per request */
	__u32 wqe_size;			/* bytes per send/receive ring entry */
	__u32 cqe_size;			/* bytes per completion ring entry */
};

struct uet_ref_ib_create_cq {
	__aligned_u64 ring_addr;	/* the process's completion ring */
	__u32 ring_entries;		/* a power of two */
	__u32 rsvd;
};

struct uet_ref_ib_create_cq_resp {
	__u32 cq_handle;
	__u32 rsvd;
};

struct uet_ref_ib_create_qp {
	__aligned_u64 sq_addr;
	__aligned_u64 rq_addr;
	__u32 sq_entries;		/* powers of two; 0 for no ring */
	__u32 rq_entries;
};

struct uet_ref_ib_create_qp_resp {
	__u32 qp_handle;
	__u32 sq_entries;		/* what the device actually took */
	__u32 rq_entries;
	__u32 rsvd;
};

struct uet_ref_ib_create_ah_resp {
	__u32 ah_handle;	/* the device's; a work request names it */
	__u32 rsvd;
};

/* The region's 64-bit key, returned as driver data on the legacy
 * ibv_reg_mr() path. The ib_core response carries only the 32-bit lkey/rkey
 * which on this device holds the region's handle. ibv_reg_mr_ex() does not
 * come through here. Tt instead asks for the wide keys with the REG_MR
 * RESP_LKEY64/RESP_RKEY64 attributes.
 *
 * Two 32-bit halves and not one __aligned_u64. struct ib_uverbs_reg_mr_resp
 * is three __u32, so the kernel writes driver data at offset 12. An 8-byte
 * member would give this struct 8-byte alignment, and the compiler would pad
 * the userspace view out to offset 16 while the kernel kept writing at 12.
 * uet_ref_key64() rejoins the halves.
 */
struct uet_ref_ib_reg_mr_resp {
	__u32 key64_lo;
	__u32 key64_hi;
};

static inline __u64 uet_ref_key64(const struct uet_ref_ib_reg_mr_resp *r)
{
	return (((__u64)r->key64_hi << 32) | r->key64_lo);
}

static inline void uet_ref_key64_set(struct uet_ref_ib_reg_mr_resp *r,
				     __u64 key)
{
	r->key64_lo = (__u32)key;
	r->key64_hi = (__u32)(key >> 32);
}

#endif /* UET_REF_ABI_USER_H */

