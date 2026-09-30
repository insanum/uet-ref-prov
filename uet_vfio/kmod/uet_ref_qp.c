// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * QPs and attaching an MR to a QP.
 *
 * A UET queue pair is a service at a Resource Index, and its QPN carries the
 * addressing mode, that Resource Index, and the PIDonFEP:
 *
 *     [ ABS/REL 1b | reserved 7b | RI 12b | PIDonFEP 12b ]
 *
 * A protection domain is associated with exactly one PIDonFEP, latched by
 * the first queue pair created under it. Every later queue pair in that
 * domain must carry the same value or be refused - which is this file's
 * main job, since nothing below the driver can enforce it: the device sees
 * a QPN, not the protection domain's history.
 */

#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "uet_ref.h"
#include "uet_ref-abi.h"

/* settle the QPN used for a new QP */
static int uet_ref_qpn_assign(struct uet_ref_dev *dev,
			      struct uet_ref_pd *pd,
			      u32 requested,
			      bool have_requested,
			      bool absolute,
			      u32 *out_qpn)
{
	u32 pidonfep, ri;
	int id;

	if (have_requested) {
		pidonfep = (requested & IB_UVERBS_QPN_UET_PID_ON_FEP_MASK);
		ri = ((requested & IB_UVERBS_QPN_UET_RI_MASK) >>
		      IB_UVERBS_QPN_UET_RI_SHIFT);

		if (pd->pidonfep_latched && (pd->pidonfep != pidonfep))
			return -EINVAL;

		/* claim exactly this Resource Index, or fail if it is taken */
		id = ida_alloc_range(&pd->ri_ida, ri, ri, GFP_KERNEL);
		if (id < 0)
			return id;

		if (!pd->pidonfep_latched) {
			id = ida_alloc_range(&dev->pidonfep_ida, pidonfep,
					     pidonfep, GFP_KERNEL);
			if (id < 0) {
				ida_free(&pd->ri_ida, ri);
				return id;
			}

			pd->pidonfep = pidonfep;
			pd->pidonfep_latched = true;
		}
	} else {
		if (!pd->pidonfep_latched) {
			id = ida_alloc_max(&dev->pidonfep_ida,
					   IB_UVERBS_QPN_UET_PID_ON_FEP_MASK,
					   GFP_KERNEL);
			if (id < 0)
				return id;

			pd->pidonfep = (u32)id;
			pd->pidonfep_latched = true;
		}

		pidonfep = pd->pidonfep;

		id = ida_alloc_max(&pd->ri_ida,
				   (IB_UVERBS_QPN_UET_RI_MASK >>
				    IB_UVERBS_QPN_UET_RI_SHIFT),
				   GFP_KERNEL);
		if (id < 0)
			return id;

		ri = (u32)id;
	}

	*out_qpn = (((absolute) ? IB_UVERBS_QPN_UET_ABSOLUTE_ADDR_BIT : 0) |
		    (ri << IB_UVERBS_QPN_UET_RI_SHIFT) | pidonfep);

	return 0;
}

static void uet_ref_qpn_release(struct uet_ref_pd *pd,
				u32 qpn)
{
	ida_free(&pd->ri_ida,
		 ((qpn & IB_UVERBS_QPN_UET_RI_MASK) >>
		  IB_UVERBS_QPN_UET_RI_SHIFT));
}

/* creation takes the modify attributes as well, see uet_ref_create_qp() */
static int uet_ref_qp_apply_attrs(struct uet_ref_qp *qp,
				  const struct ib_qp_attr *attr,
				  int attr_mask);

int uet_ref_create_qp(struct ib_qp *ibqp,
		      struct ib_qp_init_attr *init_attr,
		      struct ib_udata *udata)
{
	struct uet_ref_qp *qp = container_of(ibqp, struct uet_ref_qp, ibqp);
	struct uet_ref_dev *dev = container_of(ibqp->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_pd *pd = container_of(ibqp->pd, struct uet_ref_pd,
					     ibpd);
	struct uet_dev_admin_qp_create args = {};
	struct uet_ref_cq *scq, *rcq;
	u64 result[2];
	u32 qpn;
	int ret;

	/* Reliable unconnected is a QP type of its own in verbs. The QP
	 * either is one or it is not. Which delivery mode an operation
	 * travels by is decided per operation, not here.
	 */
	if (ibqp->qp_type != IB_QPT_RU)
		return -EOPNOTSUPP;

	if (!init_attr->send_cq || !init_attr->recv_cq)
		return -EINVAL;

	scq = container_of(init_attr->send_cq, struct uet_ref_cq, ibcq);
	rcq = container_of(init_attr->recv_cq, struct uet_ref_cq, ibcq);

	qp->src_id = init_attr->src_id;
	qp->absolute = true;

	/* a QP number the application chose */
	if (init_attr->create_flags & IB_QP_CREATE_SOURCE_QPN) {
		qp->requested_qpn = init_attr->source_qpn;
		qp->qpn_requested = true;
		qp->absolute = !!(init_attr->source_qpn &
				  IB_UVERBS_QPN_UET_ABSOLUTE_ADDR_BIT);
	}

	ret = uet_ref_qpn_assign(dev, pd, qp->requested_qpn,
				 qp->qpn_requested, qp->absolute, &qpn);
	if (ret)
		return ret;

	args.qpn = qpn;
	args.pd = pd->pdn;
	args.send_cq = scq->handle;
	args.recv_cq = rcq->handle;
	args.src_id = qp->src_id;
	args.max_send_wr = init_attr->cap.max_send_wr;
	args.max_recv_wr = init_attr->cap.max_recv_wr;
	args.max_send_sge = init_attr->cap.max_send_sge;
	args.max_recv_sge = init_attr->cap.max_recv_sge;

	/* The job if the application named one. Relative addressing requires
	 * it as a relative QPN identifies a service within a job.
	 */
	if (init_attr->jkey)
		args.jkey_handle = to_uet_jkey(init_attr->jkey)->handle;

	/* The attributes a modify would otherwise carry. This queue pair is
	 * returned in RTS after creation. There is no modify to carry these
	 * attributes and creation takes the whole set.
	 */
	if (init_attr->qp_attr_mask) {
		int mask = init_attr->qp_attr_mask;

		if (mask & IB_QP_PATH_MTU) {
			int bytes;

			bytes = ib_mtu_enum_to_int(init_attr->qp_attr->path_mtu);
			if (bytes <= 0) {
				ret = -EINVAL;
				goto err_qpn;
			}

			args.path_mtu = (u32)bytes;
		}

		if ((mask & IB_QP_STATE) &&
		    init_attr->qp_attr->qp_state != IB_QPS_RTS) {
			ret = -EOPNOTSUPP;
			goto err_qpn;
		}

		mask &= ~(IB_QP_STATE | IB_QP_CUR_STATE | IB_QP_PATH_MTU);

		mask = uet_ref_qp_apply_attrs(qp, init_attr->qp_attr, mask);
		if (mask) {
			ibdev_dbg(&dev->ib_dev,
				  "create_qp: unsupported qp_attr_mask 0x%x\n",
				  mask);
			ret = -EOPNOTSUPP;
			goto err_qpn;
		}
	}

	/*
	 * The rings, if the process supplied them. A queue pair without them
	 * is still a queue pair - it simply has no datapath, which is what
	 * every queue pair was before this slice.
	 */
	{
		struct uet_ref_ucontext *uctx;
		struct uet_ref_ib_create_qp cmd = {};

		uctx = rdma_udata_to_drv_context(udata,
						 struct uet_ref_ucontext,
						 ibucontext);
		args.db_page = uctx ? uctx->db_page : 0;

		if (udata && udata->inlen >= sizeof(cmd)) {
			ret = ib_copy_from_udata(&cmd, udata, sizeof(cmd));
			if (ret)
				goto err_qpn;

			ret = uet_ref_uring_pin(dev, ibqp->device, cmd.sq_addr,
						cmd.sq_entries,
						UET_DEV_WQE_SIZE, &qp->sq);
			if (ret)
				goto err_qpn;

			ret = uet_ref_uring_pin(dev, ibqp->device, cmd.rq_addr,
						cmd.rq_entries,
						UET_DEV_WQE_SIZE, &qp->rq);
			if (ret) {
				uet_ref_uring_unpin(dev, &qp->sq);
				goto err_qpn;
			}

			uet_ref_uring_desc(&qp->sq, &args.sq);
			uet_ref_uring_desc(&qp->rq, &args.rq);
		}
	}

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_QP_CREATE, NULL, 0, &args,
				sizeof(args), result);
	if (ret) {
		uet_ref_uring_unpin(dev, &qp->sq);
		uet_ref_uring_unpin(dev, &qp->rq);
		goto err_qpn;
	}

	qp->handle = (u32)result[0];
	qp->qpn = qpn;
	qp->path_mtu = args.path_mtu;
	qp->state = IB_QPS_RTS;		/* created ready to send */
	ibqp->qp_num = qpn;

	/* ib_core's resource tracker insists a queue pair name a port */
	if (!qp->port_num)
		qp->port_num = init_attr->port_num ? init_attr->port_num : 1;

	ibqp->port = qp->port_num;

	if (udata && udata->outlen) {
		struct uet_ref_ib_create_qp_resp resp = {
			.qp_handle = qp->handle,
			.sq_entries = qp->sq.entries,
			.rq_entries = qp->rq.entries,
		};

		ret = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (ret) {
			u64 p[1] = { qp->handle };

			uet_ref_admin_cmd(dev, UET_DEV_ADMIN_QP_DESTROY, p, 1,
					  NULL, 0, NULL);
			uet_ref_uring_unpin(dev, &qp->sq);
			uet_ref_uring_unpin(dev, &qp->rq);
			goto err_qpn;
		}
	}

	return 0;

err_qpn:
	uet_ref_qpn_release(pd, qpn);
	return ret;
}

int uet_ref_destroy_qp(struct ib_qp *ibqp,
		       struct ib_udata *udata)
{
	struct uet_ref_qp *qp = container_of(ibqp, struct uet_ref_qp, ibqp);
	struct uet_ref_dev *dev = container_of(ibqp->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_pd *pd = container_of(ibqp->pd, struct uet_ref_pd,
					     ibpd);
	u64 param[1] = { qp->handle };
	int ret;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_QP_DESTROY, param, 1, NULL,
				0, NULL);
	if (ret)
		return ret;

	uet_ref_uring_unpin(dev, &qp->sq);
	uet_ref_uring_unpin(dev, &qp->rq);
	uet_ref_qpn_release(pd, qp->qpn);

	return 0;
}

/* Record the attributes this device does not act on, and say what is left.
 *
 * They are kept rather than dropped so query_qp can answer with what the
 * application asked for: a caller that sets an attribute and reads it back
 * expecting its own value is not doing anything unreasonable. This mirrors
 * uprot's uprot_qp_apply_attrs(), deliberately - the two providers front
 * the same transport and should not disagree about which attributes a UET
 * queue pair accepts.
 *
 * The path MTU is the one place they must differ, and it is handled by the
 * caller rather than here. uprot records it and never uses it; this device
 * segments against it, so a value that differs from the one the endpoint
 * was built with has to be refused rather than remembered.
 */
static int uet_ref_qp_apply_attrs(struct uet_ref_qp *qp,
				  const struct ib_qp_attr *attr,
				  int attr_mask)
{
	if (attr_mask & IB_QP_PKEY_INDEX) {
		qp->pkey_index = attr->pkey_index;
		attr_mask &= ~IB_QP_PKEY_INDEX;
	}

	if (attr_mask & IB_QP_PORT) {
		qp->port_num = attr->port_num;
		attr_mask &= ~IB_QP_PORT;
	}

	if (attr_mask & IB_QP_ACCESS_FLAGS) {
		qp->qp_access_flags = attr->qp_access_flags;
		attr_mask &= ~IB_QP_ACCESS_FLAGS;
	}

	return attr_mask;
}

/* The queue pair state machine, which has three states and no more.
 *
 *     create ------> RTS <----------------.
 *                     | (modify in place)  \
 *                     |                     \
 *                     v                      \
 *                    ERR -----------------> RESET
 *
 * A UET queue pair is created ready to send. INIT and RTR exist in verbs
 * to hold a connected queue pair still while its peer is negotiated, and
 * there is nothing here to negotiate, so they are refused rather than
 * tolerated as a no-op overlay. A caller walking the full connected
 * sequence is doing something this device cannot honour, and saying so at
 * the first step beats accepting three transitions that mean nothing.
 *
 * What is kept is the recovery cycle. A queue pair that has failed can go
 * to ERROR, be reset, and come back to RTS without being destroyed - which
 * matters because everything hanging off it, the regions attached to it
 * and the Resource Index it holds, survives the round trip.
 *
 * RTS -> RTS is legal and is how attributes are modified in place.
 */
int uet_ref_modify_qp(struct ib_qp *ibqp,
		      struct ib_qp_attr *attr,
		      int attr_mask,
		      struct ib_udata *udata)
{
	struct uet_ref_qp *qp = container_of(ibqp, struct uet_ref_qp, ibqp);
	struct uet_ref_dev *dev = container_of(ibqp->device,
					       struct uet_ref_dev, ib_dev);
	enum ib_qp_state cur = qp->state, next;
	u64 param[2];
	u32 dev_state;
	int left, ret;

	if (!(attr_mask & IB_QP_STATE)) {
		ibdev_dbg(&dev->ib_dev,
			  "modify_qp: IB_QP_STATE must be set (mask 0x%x)\n",
			  attr_mask);
		return -EINVAL;
	}

	next = attr->qp_state;

	if (!((cur == IB_QPS_RTS   && next == IB_QPS_RTS) ||
	      (cur == IB_QPS_RESET && next == IB_QPS_RTS) ||
	      (cur != IB_QPS_ERR   && next == IB_QPS_ERR) ||
	      (cur == IB_QPS_ERR   && next == IB_QPS_RESET) ||
	      (cur == IB_QPS_ERR   && next == IB_QPS_ERR) ||
	      (cur == IB_QPS_RESET && next == IB_QPS_RESET))) {
		ibdev_dbg(&dev->ib_dev,
			  "modify_qp: %d -> %d refused; this device has RTS, ERR and RESET only\n",
			  cur, next);
		return -EOPNOTSUPP;
	}

	/*
	 * Refused before anything is recorded, so a rejected call changes
	 * nothing. The endpoint segments against this value and there is no
	 * way to change that underneath work in flight, so a caller asking
	 * for a different one is asking for something it will not get -
	 * and being told so beats believing it sends 512-byte packets while
	 * the wire carries 1024.
	 */
	if (attr_mask & IB_QP_PATH_MTU) {
		u32 want = ib_mtu_enum_to_int(attr->path_mtu);
		u32 have = qp->path_mtu ? qp->path_mtu :
					  uet_ref_rd0(dev, UET_DEV_REG_MAX_PAYLOAD);

		if ((int)want <= 0)
			return -EINVAL;

		if (want != have) {
			ibdev_dbg(&dev->ib_dev,
				  "modify_qp: path MTU %u refused; this queue pair was created with %u\n",
				  want, have);
			return -EINVAL;
		}

		attr_mask &= ~IB_QP_PATH_MTU;
	}

	attr_mask &= ~(IB_QP_STATE | IB_QP_CUR_STATE);

	left = uet_ref_qp_apply_attrs(qp, attr, attr_mask);
	if (left) {
		ibdev_dbg(&dev->ib_dev,
			  "modify_qp: attributes 0x%x are not accepted by this device\n",
			  left);
		return -EOPNOTSUPP;
	}

	switch (next) {
	case IB_QPS_RTS:
		dev_state = UET_DEV_QPS_RTS;
		break;
	case IB_QPS_ERR:
		dev_state = UET_DEV_QPS_ERR;
		break;
	default:
		dev_state = UET_DEV_QPS_RESET;
		break;
	}

	/*
	 * Told to the device even when the state is not changing. RTS ->
	 * RTS costs one admin command and keeps the two sides' idea of the
	 * state from being able to drift, which is worth more than the
	 * command.
	 */
	param[0] = qp->handle;
	param[1] = dev_state;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_QP_MODIFY, param, 2,
				NULL, 0, NULL);
	if (ret)
		return ret;

	qp->state = next;

	return 0;
}

int uet_ref_query_qp(struct ib_qp *ibqp,
		     struct ib_qp_attr *attr,
		     int attr_mask,
		     struct ib_qp_init_attr *init_attr)
{
	struct uet_ref_qp *qp = container_of(ibqp, struct uet_ref_qp, ibqp);

	memset(attr, 0, sizeof(*attr));
	memset(init_attr, 0, sizeof(*init_attr));

	attr->qp_state = qp->state;
	attr->cur_qp_state = qp->state;
	attr->port_num = qp->port_num ? qp->port_num : 1;
	attr->pkey_index = qp->pkey_index;
	attr->qp_access_flags = qp->qp_access_flags;
	{
		struct uet_ref_dev *dev = container_of(ibqp->device,
						       struct uet_ref_dev,
						       ib_dev);
		u32 bytes = qp->path_mtu ? qp->path_mtu :
					   uet_ref_rd0(dev,
						       UET_DEV_REG_MAX_PAYLOAD);

		attr->path_mtu = ib_mtu_int_to_enum(bytes);
	}

	init_attr->qp_type = ibqp->qp_type;
	init_attr->send_cq = ibqp->send_cq;
	init_attr->recv_cq = ibqp->recv_cq;

	ibqp->qp_num = qp->qpn;

	return 0;
}

int uet_ref_qp_attach_mr(struct ib_qp *ibqp,
			 struct ib_mr *ibmr)
{
	struct uet_ref_qp *qp = container_of(ibqp, struct uet_ref_qp, ibqp);
	struct uet_ref_mr *mr = container_of(ibmr, struct uet_ref_mr, ibmr);
	struct uet_ref_dev *dev = container_of(ibqp->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[2] = { mr->handle, qp->handle };

	return uet_ref_admin_cmd(dev, UET_DEV_ADMIN_MR_ATTACH, param, 2, NULL,
				 0, NULL);
}

int uet_ref_qp_detach_mr(struct ib_qp *ibqp,
			 struct ib_mr *ibmr)
{
	struct uet_ref_mr *mr = container_of(ibmr, struct uet_ref_mr, ibmr);
	struct uet_ref_dev *dev = container_of(ibmr->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { mr->handle };

	return uet_ref_admin_cmd(dev, UET_DEV_ADMIN_MR_DETACH, param, 1, NULL,
				 0, NULL);
}

