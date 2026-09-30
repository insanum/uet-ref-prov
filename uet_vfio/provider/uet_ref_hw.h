/* SPDX-License-Identifier: (GPL-2.0 OR Linux-OpenIB) */

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/* Hardware layouts for the UET reference device.
 *
 * These are this device's bytes. The format of a work request, of a
 * completion, and the doorbell registers.
 *
 * The authoritative definition is the device model's own header,
 * uet-ref-prov:uet_vfio/uet_dev_abi.h, which userspace never sees. This is
 * a mirror of the parts the provider must agree on.
 */

#ifndef UET_REF_HW_H
#define UET_REF_HW_H

#include <stdint.h>

/* A work request is exactly one ring entry. Nothing here is short of
 * memory and a fixed entry makes walking a ring easy.
 */
#define UET_REF_HW_WQE_SIZE	256
#define UET_REF_HW_CQE_SIZE	64
#define UET_REF_HW_MAX_SGE	8

enum uet_ref_hw_opcode {
	UET_REF_HW_OP_SEND		= 0,
	UET_REF_HW_OP_SEND_IMM		= 1,
	UET_REF_HW_OP_RECV		= 2,
	UET_REF_HW_OP_RDMA_WRITE	= 3,
	UET_REF_HW_OP_RDMA_WRITE_IMM	= 4,
	UET_REF_HW_OP_RDMA_READ		= 5,
	     /* completions only: an incoming write that carried immediate
	      * data, which no local request asked for
	      */
	UET_REF_HW_OP_RECV_RDMA_IMM	= 6,
};

enum uet_ref_hw_wr_flags {
	UET_REF_HW_WR_SIGNALED	= 1 << 0,
	UET_REF_HW_WR_IMM64	= 1 << 1,
	UET_REF_HW_WR_AH	= 1 << 2, /* addr_index is an ah handle */
};

enum uet_ref_hw_cqe_flags {
	UET_REF_HW_CQE_IMM	= 1 << 0,
};

enum uet_ref_hw_status {
	UET_REF_HW_WC_SUCCESS		= 0,
	UET_REF_HW_WC_GENERAL_ERR	= 1,
	UET_REF_HW_WC_BAD_KEY		= 2,
	UET_REF_HW_WC_BAD_ADDR		= 3,
	UET_REF_HW_WC_TOO_LONG		= 4,
	UET_REF_HW_WC_FLUSHED		= 5,
	UET_REF_HW_WC_BAD_JOB		= 6,
	UET_REF_HW_WC_BAD_LOCAL_ADDR	= 7,
};

/* a segment names a 64-bit lkey, which the device resolves */
struct uet_ref_hw_sge {
	uint64_t addr;
	uint64_t key;
	uint32_t len;
	uint32_t rsvd;
};

struct uet_ref_hw_wqe {
	uint64_t wr_id;			/* handed back in the completion */
	uint32_t opcode;		/* enum uet_ref_hw_opcode */
	uint32_t flags;			/* enum uet_ref_hw_wr_flags */
	uint32_t num_sge;
	     /* Where this is going. An index into the job's address table,
	      * or, with UET_REF_HW_WR_AH set, an address handle.
	      */
	uint32_t addr_index;
	     /* The job key value the application holds, not a JobID. Zero
	      * means the one the queue pair was created with.
	      */
	uint32_t jkey;
	uint32_t rsvd0;
	uint64_t imm;
	uint64_t remote_addr;
	uint64_t remote_key;
	uint64_t rsvd1;
	struct uet_ref_hw_sge sge[UET_REF_HW_MAX_SGE];
};

struct uet_ref_hw_cqe {
	uint64_t wr_id;
	uint64_t imm;
	uint32_t status;		/* enum uet_ref_hw_status */
	uint32_t opcode;
	uint32_t byte_len;
	uint32_t flags;			/* enum uet_ref_hw_cqe_flags */
	uint32_t qpn;
	uint32_t src_id;
	uint32_t job_id;
	     /* How far the device has consumed the ring this request came
	      * from. Without it this provider has no way to know a slot is
	      * free again. The device consumes entries on its own thread,
	      * and every queue is permanently full after as many requests
	      * are posted as it has slots.
	      */
	uint32_t ring_cons;
	     /* Toggles each lap of the ring. A consumer compares it against
	      * the lap it is on, which is how it tells a fresh entry from a
	      * stale one without the device having to clear anything.
	      */
	uint32_t valid;
	uint32_t rsvd1[3];
};

/* Doorbell registers, within the page mmap()'ed at the offset the driver
 * reported. Each is a single 64-bit write, so the handle and the index can
 * never be observed apart.
 */
#define UET_REF_HW_DB_SQ		0x00 /* (qp << 32) | producer */
#define UET_REF_HW_DB_RQ		0x08 /* (qp << 32) | producer */
#define UET_REF_HW_DB_CQ		0x10 /* arm | (cq << 32) | consumer */

#define UET_REF_HW_DB_HANDLE_SHIFT	32
#define UET_REF_HW_DB_CQ_ARM		(1ULL << 63)

#endif /* UET_REF_HW_H */
