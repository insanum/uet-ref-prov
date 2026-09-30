// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * Completion queues.
 *
 * No ->poll_cq() or ->req_notify_cq(): the device writes completions into
 * the ring and the provider reads them, so ibv_poll_cq() never enters the
 * kernel.
 */

#include <linux/interrupt.h>

#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "uet_ref.h"
#include "uet_ref-abi.h"

/*
 * A data completion arrived on a queue that had been ARM'ed.
 *
 * The device raises one vector for every completion queue, so which CQ is
 * ready is not known here. All CQs are searched for new entries.
 */
irqreturn_t uet_ref_cq_isr(int irq,
			   void *data)
{
	struct uet_ref_dev *dev = data;
	struct uet_ref_cq *cq;
	unsigned long flags;

	spin_lock_irqsave(&dev->cq_lock, flags);

	list_for_each_entry(cq, &dev->cq_list, entry) {
		if (cq->ibcq.comp_handler)
			cq->ibcq.comp_handler(&cq->ibcq, cq->ibcq.cq_context);
	}

	spin_unlock_irqrestore(&dev->cq_lock, flags);

	return IRQ_HANDLED;
}

int uet_ref_create_cq(struct ib_cq *ibcq,
		      const struct ib_cq_init_attr *attr,
		      struct uverbs_attr_bundle *attrs)
{
	struct uet_ref_cq *cq = container_of(ibcq, struct uet_ref_cq, ibcq);
	struct uet_ref_dev *dev = container_of(ibcq->device,
					       struct uet_ref_dev, ib_dev);
	struct ib_udata *udata = attrs ? &attrs->driver_udata : NULL;
	struct uet_ref_ucontext *ctx;
	struct uet_dev_admin_cq_create args = {};
	struct uet_ref_ib_create_cq cmd = {};
	unsigned long flags;
	u64 result[2];
	int ret;

	if (attr->cqe == 0)
		return -EINVAL;

	ctx = rdma_udata_to_drv_context(udata, struct uet_ref_ucontext,
					ibucontext);

	if (udata && udata->inlen >= sizeof(cmd)) {
		ret = ib_copy_from_udata(&cmd, udata, sizeof(cmd));
		if (ret)
			return ret;

		ret = uet_ref_uring_pin(dev, ibcq->device, cmd.ring_addr,
					cmd.ring_entries, UET_DEV_CQE_SIZE,
					&cq->ring);
		if (ret)
			return ret;

		uet_ref_uring_desc(&cq->ring, &args.ring);
	}

	if (args.ring.entries == 0)
		args.ring.entries = attr->cqe;

	args.db_page = ctx ? ctx->db_page : 0;
	args.vector = UET_DEV_CQ_VECTOR_BASE;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_CQ_CREATE, NULL, 0, &args,
				sizeof(args), result);
	if (ret) {
		uet_ref_uring_unpin(dev, &cq->ring);
		return ret;
	}

	cq->handle = (u32)result[0];
	ibcq->cqe = attr->cqe;

	spin_lock_irqsave(&dev->cq_lock, flags);
	list_add_tail(&cq->entry, &dev->cq_list);
	spin_unlock_irqrestore(&dev->cq_lock, flags);

	if (udata && udata->outlen) {
		struct uet_ref_ib_create_cq_resp resp = {
			.cq_handle = cq->handle,
		};

		ret = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (ret) {
			u64 p[1] = { cq->handle };

			uet_ref_admin_cmd(dev, UET_DEV_ADMIN_CQ_DESTROY, p, 1,
					  NULL, 0, NULL);
			uet_ref_uring_unpin(dev, &cq->ring);
			return ret;
		}
	}

	return 0;
}

int uet_ref_destroy_cq(struct ib_cq *ibcq,
		       struct ib_udata *udata)
{
	struct uet_ref_cq *cq = container_of(ibcq, struct uet_ref_cq, ibcq);
	struct uet_ref_dev *dev = container_of(ibcq->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { cq->handle };
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->cq_lock, flags);
	list_del(&cq->entry);
	spin_unlock_irqrestore(&dev->cq_lock, flags);

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_CQ_DESTROY, param, 1,
				NULL, 0, NULL);

	/* release the ring even if the device refused, or it leaks with the
	 * process
	 */
	uet_ref_uring_unpin(dev, &cq->ring);

	return ret;
}

