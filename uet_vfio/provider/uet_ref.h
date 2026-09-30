/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

#ifndef _UET_REF_H_
#define _UET_REF_H_

#include <assert.h>
#include <stddef.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

/* Out-of-tree driver id, matching the kernel driver's. A real upstream
 * submission would add RDMA_DRIVER_UET_REF to the enum.
 */
#define RDMA_DRIVER_UET_REF	((enum rdma_driver_id)0x43)

/* The device is a PCI function, so matching is by PCI id. */
#define UET_REF_PCI_VENDOR_ID	0x14e4	/* Broadcom */
#define UET_REF_PCI_DEVICE_ID	0x4242

struct uet_ref_device {
	struct verbs_device vdev;
	uint32_t abi_version;
};

/* command and response buffers: the core structure, then this driver's */

struct uet_ref_create_cq_cmd {
	struct ibv_create_cq ibv_cmd;
	struct uet_ref_ib_create_cq uet;
};

struct uet_ref_create_cq_resp {
	struct ib_uverbs_create_cq_resp ibv_resp;
	struct uet_ref_ib_create_cq_resp uet;
};

struct uet_ref_create_cq_ex_cmd {
	struct ibv_create_cq_ex ibv_cmd;
	struct uet_ref_ib_create_cq uet;
};

struct uet_ref_create_cq_ex_resp {
	struct ib_uverbs_ex_create_cq_resp ibv_resp;
	struct uet_ref_ib_create_cq_resp uet;
};

struct uet_ref_create_qp_cmd {
	struct ibv_create_qp_ex ibv_cmd;
	struct uet_ref_ib_create_qp uet;
};

struct uet_ref_create_qp_resp {
	struct ib_uverbs_ex_create_qp_resp ibv_resp;
	struct uet_ref_ib_create_qp_resp uet;
};

struct uet_ref_reg_mr_resp {
	struct ib_uverbs_reg_mr_resp ibv_resp;
	struct uet_ref_ib_reg_mr_resp uet;
};

struct uet_ref_create_ah_resp {
	struct ib_uverbs_create_ah_resp ibv_resp;
	struct uet_ref_ib_create_ah_resp uet;
};

struct uet_ref_get_context_resp {
	struct ib_uverbs_get_context_resp ibv_resp;
	struct uet_ref_ib_alloc_ucontext_resp uet;
};

/* The kernel appends driver data immediately after the core response, at
 * offset sizeof(core) of the same buffer. Every pair above must therefore
 * place its uet member at exactly that offset, and nothing in the
 * language guarantees it will. A driver member that needs wider alignment
 * than the core structure's size makes the compiler insert padding here
 * that the kernel does not write, and every field then reads short by the
 * width of the hole. These assertions turn the issue into a build failure.
 */
static_assert(offsetof(struct uet_ref_create_cq_resp, uet) ==
	      sizeof(struct ib_uverbs_create_cq_resp),
	      "create_cq driver response is not where the kernel writes it");
static_assert(offsetof(struct uet_ref_create_cq_ex_resp, uet) ==
	      sizeof(struct ib_uverbs_ex_create_cq_resp),
	      "create_cq_ex driver response is not where the kernel writes it");
static_assert(offsetof(struct uet_ref_create_qp_resp, uet) ==
	      sizeof(struct ib_uverbs_ex_create_qp_resp),
	      "create_qp driver response is not where the kernel writes it");
static_assert(offsetof(struct uet_ref_reg_mr_resp, uet) ==
	      sizeof(struct ib_uverbs_reg_mr_resp),
	      "reg_mr driver response is not where the kernel writes it");
static_assert(offsetof(struct uet_ref_create_ah_resp, uet) ==
	      sizeof(struct ib_uverbs_create_ah_resp),
	      "create_ah driver response is not where the kernel writes it");
static_assert(offsetof(struct uet_ref_get_context_resp, uet) ==
	      sizeof(struct ib_uverbs_get_context_resp),
	      "get_context driver response is not where the kernel writes it");

/* A ring the process owns. The kernel pinned it and the device walks its
 * page list.
 */
struct uet_ref_ring {
	void *buf;
	uint32_t entries;	/* a power of two */
	uint32_t entry_size;
	uint32_t prod;		/* next slot this process will write */
	uint32_t cons;		/* completion rings only: what we have read */
	uint32_t phase;		/* completion rings only: the lap we are on */
};

struct uet_ref_ah {
	struct ibv_ah_ex ah_ex;	/* ah_base.handle is the kernel object id */
	uint32_t handle;	/* the device's, a work request names it */
};

struct uet_ref_qp;

struct uet_ref_cq {
	struct verbs_cq vcq;
	uint32_t handle;	/* the device's, which a doorbell names */
	struct uet_ref_ring ring;
	     /* The queue pairs this queue reports for. A completion names
	      * its queue pair by QPN and carries how far the device has
	      * consumed that pair's ring, so reaping one is what frees the
	      * slot the request occupied. One completion queue may serve
	      * several pairs, so the mapping has to be kept here.
	      */
	struct uet_ref_qp **qps;
	uint32_t nqps;
	     /* The extended reader's cursor. cur is the entry the read_*
	      * accessors describe. cur_valid says whether one is held, so
	      * end_poll knows whether it still has to consume it.
	      */
	struct uet_ref_hw_cqe *cur;
	bool cur_valid;
};

struct uet_ref_qp {
	struct verbs_qp vqp;
	uint32_t handle;
	struct uet_ref_ring sq;
	struct uet_ref_ring rq;
	struct uet_ref_cq *send_cq;
	struct uet_ref_cq *recv_cq;
	     /* The work request under construction between ibv_wr_start()
	      * and ibv_wr_complete(). NULL when none is open. err records
	      * the first failure so the whole batch is reported once, which
	      * is what the builder API asks for.
	      */
	struct uet_ref_hw_wqe *cur;
	int err;
	bool in_batch;
};

static inline struct uet_ref_cq *to_uet_ref_cq(struct ibv_cq *ibcq)
{
	return container_of(container_of(ibcq, struct verbs_cq, cq),
			    struct uet_ref_cq, vcq);
}

static inline struct uet_ref_ah *to_uet_ref_ah(struct ibv_ah *ibah)
{
	return container_of(container_of(ibah, struct ibv_ah_ex, ah_base),
			    struct uet_ref_ah, ah_ex);
}

static inline struct uet_ref_qp *to_uet_ref_qp(struct ibv_qp *ibqp)
{
	return container_of(container_of(ibqp, struct verbs_qp, qp),
			    struct uet_ref_qp, vqp);
}

/* command and response buffers: the core structure, then ours */
struct uet_ref_context {
	struct verbs_context vctx;
	/* The doorbell page, mapped once per context. Rings live in ordinary
	 * process memory. This is the only piece of the device mapped in,
	 * and every queue pair and completion queue of this context rings
	 * through it.
	 */
	void *db;
	uint32_t db_page;
	uint32_t max_sge;
	uint32_t wqe_size;
	uint32_t cqe_size;
};

/* our ibv_job / ibv_job_key, each carrying the kernel object handle */
struct uet_ref_job {
	struct ibv_job vjob;
};

struct uet_ref_jkey {
	struct ibv_job_key vjkey;
};

static inline struct uet_ref_device *to_uet_ref_dev(struct ibv_device *ibdev)
{
	return container_of(ibdev, struct uet_ref_device, vdev.device);
}

static inline struct uet_ref_context *to_uet_ref_ctx(
	struct ibv_context *ibctx)
{
	return container_of(ibctx, struct uet_ref_context, vctx.context);
}

#endif /* _UET_REF_H_ */
