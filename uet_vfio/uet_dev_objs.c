/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - the object model
 *
 * Jobs, job keys, address table entries, address handles, memory regions,
 * completion queues and queue pairs. Everything a guest creates and destroys
 * via the slowpath admin queue.
 *
 * Note: All resource handles are one-based so zero means "none".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uet_dev_priv.h"
#include "uet_dev_objs.h"
#include "uet_dev_data.h"

struct uet_dev_job *uet_dev_job_get(struct uet_dev *dev,
				    uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_JOB_IDS) ||
	    (dev->jobs == NULL))
		return NULL;

	return dev->jobs[handle - 1].used ? &dev->jobs[handle - 1] : NULL;
}

/* a JobID identifies one unique job object */
static bool uet_dev_job_id_live(struct uet_dev *dev,
				uint32_t job_id)
{
	size_t i;

	for (i = 0; i < UET_DEV_MAX_JOB_IDS; i++) {
		if (dev->jobs[i].used && (dev->jobs[i].job_id == job_id))
			return true;
	}

	return false;
}

uint32_t uet_dev_job_alloc(struct uet_dev *dev,
			   uint32_t job_id,
			   uint32_t max_addrs,
			   uint8_t port_num,
			   uint8_t sgid_index,
			   uint32_t flags,
			   uint32_t *status)
{
	struct uet_dev_job *job;
	size_t i;

	if (max_addrs > UET_DEV_MAX_ADDR_ENTRIES) {
		*status = UET_DEV_ADMIN_ST_EINVAL;
		return 0;
	}

	if (uet_dev_job_id_live(dev, job_id)) {
		*status = UET_DEV_ADMIN_ST_EEXIST;
		return 0;
	}

	for (i = 0; i < UET_DEV_MAX_JOB_IDS; i++) {
		if (!dev->jobs[i].used)
			break;
	}

	if (i == UET_DEV_MAX_JOB_IDS) {
		*status = UET_DEV_ADMIN_ST_ENOSPC;
		return 0;
	}

	job = &dev->jobs[i];
	memset(job, 0, sizeof(*job));

	/* asking for none means results in the default */
	if (max_addrs == 0)
		max_addrs = UET_DEV_DEF_ADDR_ENTRIES;

	job->addrs = calloc(max_addrs, sizeof(*job->addrs));
	if (job->addrs == NULL) {
		*status = UET_DEV_ADMIN_ST_ENOSPC;
		return 0;
	}

	job->used             = true;
	job->job_id           = job_id;
	job->max_addr_entries = max_addrs;
	job->flags            = flags;
	job->port_num         = port_num;
	job->sgid_index       = sgid_index;

	dev->jobs_live++;

	uet_dev_trace("job create: handle %u id %u addrs %u (%u live)",
		      (unsigned)(i + 1), job->job_id,
		      (unsigned)job->max_addr_entries, dev->jobs_live);

	*status = UET_DEV_ADMIN_ST_OK;

	return (uint32_t)(i + 1);
}

/* Remove every address vector in the job's address table. Returns true when
 * the table is clear and may be freed. An entry the library still has
 * operations against stays, and is retried later (retiring state).
 */
bool uet_dev_job_addrs_release(struct uet_dev_job *job)
{
	struct uet_dev_addr_entry *slot;
	bool clear = true;
	size_t i;

	if (job->addrs == NULL)
		return true;

	for (i = 0; i < job->max_addr_entries; i++) {
		slot = &job->addrs[i];

		if (slot->av == NULL)
			continue;

		if (uet_av_remove(slot->av) == FI_SUCCESS) {
			slot->av = NULL;
			slot->valid = false;
			slot->retiring = false;
		} else {
			slot->valid = false;
			slot->retiring = true;
			clear = false;
		}
	}

	return clear;
}

/* Freeing a job releases the job keys that are associated with it. */
uint32_t uet_dev_job_free(struct uet_dev *dev,
			  uint64_t handle)
{
	struct uet_dev_job *job = uet_dev_job_get(dev, handle);
	size_t i;

	if (job == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	for (i = 0; i < UET_DEV_MAX_JOB_KEYS; i++) {
		if (dev->jkeys[i].used &&
		    (dev->jkeys[i].job_handle == (uint32_t)handle)) {
			dev->jkeys[i].used = false;
			dev->jkeys_live--;
		}
	}

	/* Release all entries in this job's address table. An address the
	 * library is still using cannot be released yet so the job is marked
	 * as retiring and the progress thread will finish it.
	 */
	if (!uet_dev_job_addrs_release(job)) {
		job->retiring = true;
		dev->jobs_retiring++;
		return UET_DEV_ADMIN_ST_OK;
	}

	uet_dev_trace("job free: id %u (%u live)", job->job_id,
		      (dev->jobs_live - 1));

	free(job->addrs);
	memset(job, 0, sizeof(*job));
	dev->jobs_live--;

	return UET_DEV_ADMIN_ST_OK;
}

/* resolve an address table slot based on a job object and index */
struct uet_dev_addr_entry *uet_dev_addr_slot_job(struct uet_dev_job *job,
						 uint64_t index)
{
	if ((job == NULL) || (job->addrs == NULL) ||
	    (index >= job->max_addr_entries))
		return NULL;

	return &job->addrs[index];
}

/* resolve an address table slot based on a job handle and index */
struct uet_dev_addr_entry *uet_dev_addr_slot(struct uet_dev *dev,
					     uint64_t job_handle,
					     uint64_t index)
{
	struct uet_dev_job *job = uet_dev_job_get(dev, job_handle);

	if ((job == NULL) || (job->addrs == NULL) ||
	    (index >= job->max_addr_entries))
		return NULL;

	return &job->addrs[index];
}

/* Translate an address into the uet-ref-prof library form. */
static void uet_dev_addr_build(struct uet_dev *dev,
			       struct uet_dev_admin_addr *addr,
			       struct uet_addr *outp)
{
	struct uet_addr ua;
	uint32_t v4;

	memset(&ua, 0, sizeof(ua));
	ua.ver = UET_ADDR_VERSION;

	ua.flags = (UET_ADDR_FEP_CAP_V | UET_ADDR_FA_V |
		    UET_ADDR_PID_ON_FEP_V | UET_ADDR_INDEX_V |
		    UET_ADDR_INITIATOR_V | UET_ADDR_BIG_MSG_SIZE);
	ua.flags |= ((addr->remote_qpn & UET_DEV_QPN_ABSOLUTE) ?
		     UET_ADDR_ABSOLUTE_MODE : UET_ADDR_RELATIVE_MODE);

	if (dev->ip_ver == 6) {
		ua.flags |= UET_ADDR_IPV6;
		memcpy(ua.fa.v6, addr->dgid, sizeof(ua.fa.v6));
	} else {
		ua.flags |= UET_ADDR_IPV4;
		memcpy(&v4, &addr->dgid[12], sizeof(v4));
		ua.fa.v4 = ntohl(v4);
	}

	ua.fep_cap = (UET_FEP_CAP_AI_FULL | UET_FEP_CAP_HPC);

	ua.pid_on_fep = (uint16_t)(addr->remote_qpn &
				   UET_DEV_QPN_PIDONFEP_MASK);

	ua.start_index = (uint16_t)((addr->remote_qpn &
				     UET_DEV_QPN_RI_MASK) >>
				    UET_DEV_QPN_RI_SHIFT);

	ua.num_indices = 1;

	ua.initiator_id = addr->flow_label;

	*outp = ua;
}

/* Translate an address table entry into the uet-ref-prof library form and
 * register the address. The caller owns the storage of the UET address and
 * it must outlive the address vector entry in the library, so it is stored
 * in the address table entry.
 */
uet_addr_handle_t uet_dev_addr_av(struct uet_dev *dev,
				  struct uet_dev_addr_entry *slot)
{
	int rc;

	if (slot->av != NULL)
		return slot->av;

	/* build the uet_addr from the address table entry */
	uet_dev_addr_build(dev, &slot->addr, &slot->ua);

	rc = uet_av_insert(dev->domain_handle, &slot->ua, &slot->av);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_av_insert failed (%d)\n", rc);
		slot->av = NULL;
	}

	return slot->av;
}

/* The value a job key is known by outside the device and by the application.
 * A tagging scheme is used here placing 'J' in the upper byte to keep it
 * distinct from the handle.
 */
uint32_t uet_dev_jkey_value(uint32_t handle)
{
	return 0x4a000000u | handle;
}

/* Find the job from a job key (must be associated the specified PD). */
struct uet_dev_job *uet_dev_job_by_jkey(struct uet_dev *dev,
					uint32_t jkey,
					uint32_t pdn)
{
	size_t i;

	for (i = 0; i < UET_DEV_MAX_JOB_KEYS; i++) {
		if (!dev->jkeys[i].used)
			continue;

		if (uet_dev_jkey_value((uint32_t)(i + 1)) != jkey)
			continue;

		if (dev->jkeys[i].pdn != pdn)
			continue;

		return uet_dev_job_get(dev, dev->jkeys[i].job_handle);
	}

	return NULL;
}

uint32_t uet_dev_jkey_create(struct uet_dev *dev,
			     uint64_t job_handle,
			     uint32_t pdn,
			     uint32_t flags,
			     uint32_t *out_handle,
			     uint32_t *out_jkey)
{
	size_t i;

	if (uet_dev_job_get(dev, job_handle) == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	for (i = 0; i < UET_DEV_MAX_JOB_KEYS; i++) {
		if (!dev->jkeys[i].used)
			break;
	}

	if (i == UET_DEV_MAX_JOB_KEYS)
		return UET_DEV_ADMIN_ST_ENOSPC;

	dev->jkeys[i].used       = true;
	dev->jkeys[i].job_handle = (uint32_t)job_handle;
	dev->jkeys[i].pdn        = pdn;
	dev->jkeys[i].flags      = flags;

	dev->jkeys_live++;

	*out_handle = (uint32_t)(i + 1);
	*out_jkey = uet_dev_jkey_value((uint32_t)(i + 1));

	uet_dev_trace("jkey create: handle %u jkey %u pd %u (%u live)",
		      (unsigned)(i + 1), *out_jkey, pdn,
		      dev->jkeys_live);

	return UET_DEV_ADMIN_ST_OK;
}

uint32_t uet_dev_jkey_destroy(struct uet_dev *dev,
			      uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_JOB_KEYS))
		return UET_DEV_ADMIN_ST_ENOENT;

	if (!dev->jkeys[handle - 1].used)
		return UET_DEV_ADMIN_ST_ENOENT;

	dev->jkeys[handle - 1].used = false;
	dev->jkeys_live--;

	uet_dev_trace("jkey destroy: handle %u jkey %u (%u live)",
		      handle, uet_dev_jkey_value((uint32_t)(handle + 1)),
		      dev->jkeys_live);

	return UET_DEV_ADMIN_ST_OK;
}

struct uet_dev_ah *uet_dev_ah_get(struct uet_dev *dev,
				  uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_AHS) ||
	    (dev->ahs == NULL))
		return NULL;

	return dev->ahs[handle - 1].used ? &dev->ahs[handle - 1] : NULL;
}

/* Translate an address handle into the uet-ref-prof library form and
 * register the address. The caller owns the storage of the UET address and
 * it must outlive the address vector entry in the library, so it is stored
 * in the address handle.
 */
uet_addr_handle_t uet_dev_ah_av(struct uet_dev *dev,
				struct uet_dev_ah *ah)
{
	int rc;

	if (ah->av != NULL)
		return ah->av;

	uet_dev_addr_build(dev, &ah->addr, &ah->ua);

	rc = uet_av_insert(dev->domain_handle, &ah->ua, &ah->av);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_av_insert failed (%d)\n", rc);
		ah->av = NULL;
	}

	return ah->av;
}

uint32_t uet_dev_ah_create(struct uet_dev *dev,
			   uint32_t pd,
			   struct uet_dev_admin_addr *addr,
			   uint32_t *out_handle)
{
	size_t i;

	for (i = 0; i < UET_DEV_MAX_AHS; i++) {
		if (!dev->ahs[i].used)
			break;
	}

	if (i == UET_DEV_MAX_AHS)
		return UET_DEV_ADMIN_ST_ENOSPC;

	memset(&dev->ahs[i], 0, sizeof(dev->ahs[i]));

	dev->ahs[i].used = true;
	dev->ahs[i].pd   = pd;
	dev->ahs[i].addr = *addr;

	*out_handle = (uint32_t)(i + 1);

	uet_dev_trace("ah create: handle %u pd %u", *out_handle, pd);

	return UET_DEV_ADMIN_ST_OK;
}

uint32_t uet_dev_ah_destroy(struct uet_dev *dev,
			    uint64_t handle)
{
	struct uet_dev_ah *ah = uet_dev_ah_get(dev, handle);

	if (ah == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	if (ah->av != NULL)
		uet_av_remove(ah->av);

	memset(ah, 0, sizeof(*ah));

	uet_dev_trace("ah destroy: handle %llu", (unsigned long long)handle);

	return UET_DEV_ADMIN_ST_OK;
}

struct uet_dev_cq *uet_dev_cq_get(struct uet_dev *dev,
				  uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_CQS) ||
	    (dev->cqs == NULL))
		return NULL;

	return dev->cqs[handle - 1].used ? &dev->cqs[handle - 1] : NULL;
}

uint32_t uet_dev_cq_create(struct uet_dev *dev,
			   struct uet_dev_admin_cq_create *args,
			   uint32_t *out_handle)
{
	uint32_t entries = args->ring.entries;
	size_t i;

	if ((entries == 0) || (entries > UET_DEV_CQ_MAX_ENTRIES))
		return UET_DEV_ADMIN_ST_EINVAL;

	/* the ring index wraps by masking, so depth must be a power of two */
	if (entries & (entries - 1))
		return UET_DEV_ADMIN_ST_EINVAL;

	if (args->db_page >= UET_DEV_MAX_DB_PAGES)
		return UET_DEV_ADMIN_ST_EINVAL;

	if (args->vector >= UET_DEV_NUM_VECTORS)
		return UET_DEV_ADMIN_ST_EINVAL;

	for (i = 0; i < UET_DEV_MAX_CQS; i++) {
		if (!dev->cqs[i].used)
			break;
	}

	if (i == UET_DEV_MAX_CQS)
		return UET_DEV_ADMIN_ST_ENOSPC;

	memset(&dev->cqs[i], 0, sizeof(dev->cqs[i]));

	dev->cqs[i].used      = true;
	dev->cqs[i].entries   = entries;
	dev->cqs[i].db_page   = args->db_page;
	dev->cqs[i].vector    = args->vector;
	dev->cqs[i].ring.desc = args->ring;
	dev->cqs[i].ring.live = (args->ring.pbl_root != 0);
	dev->cqs[i].phase     = 1; /* first lap marks entries valid */

	dev->cqs_live++;

	uet_dev_trace("cq create: handle %u entries %u (%u live)",
		      (unsigned)(i + 1), dev->cqs[i].entries, dev->cqs_live);

	*out_handle = (uint32_t)(i + 1);

	return UET_DEV_ADMIN_ST_OK;
}

uint32_t uet_dev_cq_destroy(struct uet_dev *dev,
			    uint64_t handle)
{
	struct uet_dev_cq *cq = uet_dev_cq_get(dev, handle);

	if (cq == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	/* no queue pair can be referencing this cq */
	if (cq->bound_qps != 0)
		return UET_DEV_ADMIN_ST_EINVAL;

	memset(cq, 0, sizeof(*cq));
	dev->cqs_live--;

	uet_dev_trace("cq destroy: handle %u (%u live)",
		      (unsigned)handle, dev->cqs_live);

	return UET_DEV_ADMIN_ST_OK;
}

struct uet_dev_qp *uet_dev_qp_get(struct uet_dev *dev,
				  uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_QPS) ||
	    (dev->qps == NULL))
		return NULL;

	return dev->qps[handle - 1].used ? &dev->qps[handle - 1] : NULL;
}

/* Create a queue pair endpoint. The library's sequence is endpoint, bind
 * completion queues, enable. The guest's completion queues become the library
 * queues here.
 */
uint32_t uet_dev_qp_create(struct uet_dev *dev,
			   struct uet_dev_admin_qp_create *args,
			   uint32_t *out_handle)
{
	struct fi_cq_attr cq_attr;
	struct uet_dev_cq *scq, *rcq;
	struct uet_dev_qp *qp;
	struct uet_dev_job *ep_job = NULL;
	bool absolute = ((args->qpn & UET_DEV_QPN_ABSOLUTE) != 0);
	struct uet_dev_ring_desc *rd[2] = { &args->sq, &args->rq };
	size_t i, k;
	int rc;

	scq = uet_dev_cq_get(dev, args->send_cq);
	rcq = uet_dev_cq_get(dev, args->recv_cq);
	if ((scq == NULL) || (rcq == NULL))
		return UET_DEV_ADMIN_ST_ENOENT;

	/* Relative addressing names a job as well as a service, so the queue
	 * pair must carry a job key, absolute addressing must not.
	 */
	if (!absolute) {
		if ((args->jkey_handle == 0) ||
		    (args->jkey_handle > UET_DEV_MAX_JOB_KEYS) ||
		    !dev->jkeys[args->jkey_handle - 1].used)
			return UET_DEV_ADMIN_ST_EINVAL;
	}

	/* A queue pair still being retired keeps its QPN. The guest released
	 * the Resource Index when its destroy returned, so it may legitimately
	 * ask for the same one again, but the library still has an endpoint
	 * under that identity, and a second one would collide with it. Refuse
	 * until the first has gone; the caller can retry.
	 */
	for (i = 0; i < UET_DEV_MAX_QPS; i++) {
		if (dev->qps[i].used && dev->qps[i].closing &&
		    (dev->qps[i].qpn == args->qpn))
			return UET_DEV_ADMIN_ST_EBUSY;
	}

	for (i = 0; i < UET_DEV_MAX_QPS; i++) {
		if (!dev->qps[i].used)
			break;
	}

	if (i == UET_DEV_MAX_QPS)
		return UET_DEV_ADMIN_ST_ENOSPC;

	qp = &dev->qps[i];
	memset(qp, 0, sizeof(*qp));

	/* Set the descriptor pool sizes that the library latches on when
	 * creating the endpoint.
	 */
	dev->info->tx_attr->size = (args->max_send_wr > UET_DEV_DEF_QUEUE_DEPTH)
					? args->max_send_wr
					: UET_DEV_DEF_QUEUE_DEPTH;
	dev->info->rx_attr->size = (args->max_recv_wr > UET_DEV_DEF_QUEUE_DEPTH)
					? args->max_recv_wr
					: UET_DEV_DEF_QUEUE_DEPTH;

	if (args->jkey_handle != 0) {
		ep_job = uet_dev_job_get(dev,
				dev->jkeys[args->jkey_handle - 1].job_handle);
	}

	rc = uet_endpoint(dev->domain_handle, dev->info, &qp->ep_fid,
			  dev, &qp->ep,
			  (uint16_t)(args->qpn & UET_DEV_QPN_PIDONFEP_MASK),
			  (uint16_t)((args->qpn & UET_DEV_QPN_RI_MASK) >>
				     UET_DEV_QPN_RI_SHIFT),
			  args->src_id,
			  (ep_job != NULL) ? ep_job->job_id : 0,
			  absolute, (dev->ip_ver == 6),
			  args->path_mtu);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_endpoint failed (%d)\n", rc);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_DATA;
	cq_attr.size = scq->entries;

	rc = uet_ep_bind_cq(qp->ep, &cq_attr, &qp->cq_fid, FI_TRANSMIT, dev,
			    &qp->tx_cq);
	if (rc != FI_SUCCESS)
		goto err_out;

	cq_attr.size = rcq->entries;

	rc = uet_ep_bind_cq(qp->ep, &cq_attr, &qp->cq_fid, FI_RECV, dev,
			    &qp->rx_cq);
	if (rc != FI_SUCCESS)
		goto err_out;

	rc = uet_ep_enable(qp->ep);
	if (rc != FI_SUCCESS)
		goto err_out;

	/* A ring that is present must have a power-of-two depth, because the
	 * index wraps by masking.
	 */
	for (k = 0; k < 2; k++) {
		if (rd[k]->entries == 0)
			continue;

		if ((rd[k]->entries > UET_DEV_RING_MAX_ENTRIES) ||
		    (rd[k]->entries & (rd[k]->entries - 1)) ||
		    (rd[k]->page_size == 0)) {
			goto err_out;
		}
	}

	if (args->db_page >= UET_DEV_MAX_DB_PAGES)
		goto err_out;

	qp->db_page = args->db_page;
	qp->sq.desc = args->sq;
	qp->rq.desc = args->rq;
	qp->sq.live = (args->sq.entries != 0);
	qp->rq.live = (args->rq.entries != 0);

	if (qp->sq.live) {
		qp->sq_work = calloc(args->sq.entries, sizeof(*qp->sq_work));
		if (qp->sq_work == NULL)
			goto err_out;
	}

	if (qp->rq.live) {
		qp->rq_work = calloc(args->rq.entries, sizeof(*qp->rq_work));
		if (qp->rq_work == NULL)
			goto err_out;
	}

	qp->used        = true;
	qp->state       = UET_DEV_QPS_RTS; /* created directly in RTS */
	qp->qpn         = args->qpn;
	qp->pd          = args->pd;
	qp->send_cq     = args->send_cq;
	qp->recv_cq     = args->recv_cq;
	qp->src_id      = args->src_id;
	qp->jkey_handle = args->jkey_handle;

	*out_handle = (uint32_t)(i + 1);

	/* one queue pair = one reference when the same cq used for sq/rq */
	scq->bound_qps++;
	if (rcq != scq)
		rcq->bound_qps++;

	dev->qps_live++;

	uet_dev_trace("qp create: handle %u qpn 0x%x pd %u mtu %u (%u live)",
		      *out_handle, qp->qpn, qp->pd, args->path_mtu,
		      dev->qps_live);

	return UET_DEV_ADMIN_ST_OK;

err_out:
	fprintf(stderr, "uet_dev: queue pair bring-up failed (%d)\n", rc);
	free(qp->sq_work);
	free(qp->rq_work);
	qp->sq_work = NULL;
	qp->rq_work = NULL;
	uet_ep_close(qp->ep);

	return UET_DEV_ADMIN_ST_EINVAL;
}

/* release a queue pair the library has finished with */
static void uet_dev_qp_free(struct uet_dev *dev,
			    struct uet_dev_qp *qp)
{
	free(qp->sq_work);
	free(qp->rq_work);

	uet_dev_trace("qp free: qpn 0x%x (%u live)", qp->qpn,
		      dev->qps_live - 1);

	memset(qp, 0, sizeof(*qp));
	dev->qps_live--;
}

uint32_t uet_dev_qp_destroy(struct uet_dev *dev,
			    uint64_t handle)
{
	struct uet_dev_qp *qp = uet_dev_qp_get(dev, handle);
	struct uet_dev_cq *scq;
	struct uet_dev_cq *rcq;
	size_t i;
	int rc;

	if (qp == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	/* detach all MRs that are still attached */
	for (i = 0; i < UET_DEV_MAX_MRS; i++) {
		if (dev->mrs[i].used &&
		    (dev->mrs[i].qp == (uint32_t)handle)) {
			uet_mr_disable(dev->mrs[i].mr);
			dev->mrs[i].qp = 0;
			dev->mrs[i].enabled = false;
		}
	}

	/* drop the cq references */
	scq = uet_dev_cq_get(dev, qp->send_cq);
	rcq = uet_dev_cq_get(dev, qp->recv_cq);

	if ((scq != NULL) && (scq->bound_qps > 0))
		scq->bound_qps--;

	if ((rcq != NULL) && (rcq != scq) && (rcq->bound_qps > 0))
		rcq->bound_qps--;

	/* Reap anything the library has already finished. A guest that polls
	 * its last completion and immediately destroys is the common case,
	 * and the endpoint is often closeable the moment its completions are
	 * drained.
	 */
	uet_dev_cq_drain(dev, qp, qp->tx_cq);
	uet_dev_cq_drain(dev, qp, qp->rx_cq);

	/* The library refuses to close an endpoint with transfers still in
	 * flight as resources continue to be in use. So the destroy always
	 * succeeds and the queue pair stops carrying traffic immediately.
	 * Its slot stays reserved and the progress thread keeps trying to
	 * close the endpoint until the library lets go.
	 */
	rc = uet_ep_close(qp->ep);
	if (rc != FI_SUCCESS) {
		qp->closing = true;
		qp->sq.live = false;
		qp->rq.live = false;
		dev->qps_closing++;
		return UET_DEV_ADMIN_ST_OK;
	}

	uet_dev_qp_free(dev, qp);

	return UET_DEV_ADMIN_ST_OK;
}

/* Move a queue pair between the three states this device implements.
 * RTS -> ERROR abandons whatever was in flight.
 * ERROR -> RESET returns the endpoint to its pre-enable state.
 * RESET -> RTS brings it back up.
 * RTS -> RTS is legal and is not considered a no-op (though othing done).
 * INIT and RTR are not states supported by the device. The driver refuses
 * them before the device is reached. If they are seen then it's an error.
 */
uint32_t uet_dev_qp_modify(struct uet_dev *dev,
			   uint64_t handle,
			   uint32_t next_state)
{
	struct uet_dev_qp *qp = uet_dev_qp_get(dev, handle);
	uint32_t cur_state;
	ssize_t flushed;

	if (qp == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	if (qp->closing)
		return UET_DEV_ADMIN_ST_EINVAL;

	cur_state = qp->state;

	switch (next_state) {
	case UET_DEV_QPS_RTS:
		if (cur_state == UET_DEV_QPS_RTS)
			break; /* modify in place */

		if (cur_state != UET_DEV_QPS_RESET)
			return UET_DEV_ADMIN_ST_EINVAL;

		if (uet_ep_enable(qp->ep) != FI_SUCCESS)
			return UET_DEV_ADMIN_ST_EINVAL;

		uet_dev_trace("qp 0x%x -> RTS", qp->qpn);
		break;

	case UET_DEV_QPS_ERR:
		if (cur_state == UET_DEV_QPS_ERR)
			break;

		/* Abandon what was outstanding a batch at a time to avoid
		 * overrunning the completion ring (no overflow protection).
		 */
		do {
			flushed = uet_ep_flush(qp->ep,
					       UET_DEV_FLUSH_PER_SWEEP);
			if (flushed > 0) {
				dev->flushed += (uint32_t)flushed;
				uet_dev_cq_drain(dev, qp, qp->tx_cq);
				uet_dev_cq_drain(dev, qp, qp->rx_cq);
			}
		} while (flushed > 0);

		uet_dev_trace("qp 0x%x -> ERROR", qp->qpn);
		break;

	case UET_DEV_QPS_RESET:
		if (cur_state == UET_DEV_QPS_RESET)
			break;

		if (cur_state != UET_DEV_QPS_ERR)
			return UET_DEV_ADMIN_ST_EINVAL;

		if (uet_ep_reset(qp->ep) != FI_SUCCESS)
			return UET_DEV_ADMIN_ST_EINVAL;

		uet_dev_trace("qp 0x%x -> RESET", qp->qpn);
		break;

	default:
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	qp->state = next_state;

	return UET_DEV_ADMIN_ST_OK;
}

/* Finish closing a queue pair the guest has already destroyed. The library
 * refuses to close an endpoint with transmits outstanding. Abandon what was
 * outstanding a batch at a time to avoid overrunning the completion ring
 * (no overflow protection).
 */
void uet_dev_qp_retire(struct uet_dev *dev,
		       struct uet_dev_qp *qp)
{
	ssize_t flushed;

	flushed = uet_ep_flush(qp->ep, UET_DEV_FLUSH_PER_SWEEP);
	if (flushed > 0) {
		dev->flushed += (uint32_t)flushed;
		uet_dev_trace("flushed %zd outstanding on qp 0x%x", flushed,
			      qp->qpn);
		uet_dev_cq_drain(dev, qp, qp->tx_cq);
		uet_dev_cq_drain(dev, qp, qp->rx_cq);
		return; /* could still be more, try again later */
	}

	if (uet_ep_close(qp->ep) != FI_SUCCESS) {
		if (!qp->retire_traced) {
			qp->retire_traced = true;
			uet_dev_trace("qp 0x%x: flush found nothing to abandon "
				      "but the endpoint will not close",
				      qp->qpn);
		}

		return; /* try again later */
	}

	dev->qps_closing--;

	uet_dev_qp_free(dev, qp);
}

uint32_t uet_dev_qp_job_id(struct uet_dev *dev,
			   struct uet_dev_qp *qp)
{
	struct uet_dev_job *job;

	if (qp->jkey_handle == 0)
		return 0;

	job = uet_dev_job_by_jkey(dev, uet_dev_jkey_value(qp->jkey_handle),
				  qp->pd);

	return (job != NULL) ? job->job_id : 0;
}

static struct uet_dev_mr *uet_dev_mr_get(struct uet_dev *dev,
					 uint64_t handle)
{
	if ((handle == 0) || (handle > UET_DEV_MAX_MRS) ||
	    (dev->mrs == NULL))
		return NULL;

	return dev->mrs[handle - 1].used ? &dev->mrs[handle - 1] : NULL;
}

/* search using 64b rkey (UET key) or 32b lkey (handle) */
struct uet_dev_mr *uet_dev_mr_by_key(struct uet_dev *dev,
				     uint64_t key)
{
	size_t i;

	if (key == 0)
		return NULL;

	for (i = 0; i < UET_DEV_MAX_MRS; i++) {
		if (!dev->mrs[i].used)
			continue;

		if ((dev->mrs[i].key == key) || ((uint64_t)(i + 1) == key))
			return &dev->mrs[i];
	}

	return NULL;
}

/* Register a guest region with the reference implementation. */
uint32_t uet_dev_mr_reg(struct uet_dev *dev,
			struct uet_dev_admin_mr_reg *args,
			uint64_t pbl_root,
			uint32_t page_size,
			uint32_t pbl_level,
			uint64_t base_va,
			uint64_t len,
			uint32_t *out_handle,
			uint64_t *out_key)
{
	uet_mr_handle_t mr = NULL;
	uet_pbl_level_t lib_level;
	uet_mr_access_class_t lib_class;
	uint64_t lib_flags = 0;
	uint32_t jkey_handle = args->jkey_handle;
	struct uet_dev_job *job;
	uint32_t job_id = 0;
	uint64_t access = 0;
	uint64_t requested;
	size_t i, k;
	int rc;

	/* translate the ABI's level names to the library's names */
	switch (pbl_level) {
	case UET_DEV_PBL_LEVEL_0:
		lib_level = UET_PBL_LEVEL_0;
		break;
	case UET_DEV_PBL_LEVEL_1:
		lib_level = UET_PBL_LEVEL_1;
		break;
	case UET_DEV_PBL_LEVEL_2:
		lib_level = UET_PBL_LEVEL_2;
		break;
	default:
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	for (i = 0; i < UET_DEV_MAX_MRS; i++) {
		if (!dev->mrs[i].used)
			break;
	}

	if (i == UET_DEV_MAX_MRS)
		return UET_DEV_ADMIN_ST_ENOSPC;

	/* translate the ABI's access rights into the library's */
	if (args->access & UET_DEV_MR_ACCESS_LOCAL_READ)
		access |= (FI_SEND | FI_READ);
	if (args->access & UET_DEV_MR_ACCESS_LOCAL_WRITE)
		access |= (FI_RECV | FI_WRITE);
	if (args->access & UET_DEV_MR_ACCESS_REMOTE_READ)
		access |= FI_REMOTE_READ;
	if (args->access & UET_DEV_MR_ACCESS_REMOTE_WRITE)
		access |= FI_REMOTE_WRITE;
	if (args->access & UET_DEV_MR_ACCESS_REMOTE_ATOMIC)
		access |= FI_ATOMIC;

	/* ask for a specific key rather than the provider assign one */
	if (args->requested_key == 0)
		requested = uet_mr_format_key((uint64_t)(i + 1), false);
	else
		requested = args->requested_key;

	/* Transate the ABI's access classes to the library's names. A job
	 * class takes its JobID from the job key it names, which must exist
	 * and belong to the same protection domain as the region.
	 */
	switch (args->access_class) {
	case UET_DEV_MR_CLASS_RI_RESTRICTED:
		lib_class = UET_MR_ACCESS_RI_RESTRICTED;
		break;
	case UET_DEV_MR_CLASS_UNRESTRICTED:
		lib_class = UET_MR_ACCESS_UNRESTRICTED;
		break;
	case UET_DEV_MR_CLASS_JOB_RESTRICTED:
		lib_class = UET_MR_ACCESS_JOB_RESTRICTED;
		break;
	case UET_DEV_MR_CLASS_RI_JOB_RESTRICTED:
		lib_class = UET_MR_ACCESS_RI_JOB_RESTRICTED;
		break;
	default:
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	if ((lib_class == UET_MR_ACCESS_JOB_RESTRICTED) ||
	    (lib_class == UET_MR_ACCESS_RI_JOB_RESTRICTED)) {
		if ((jkey_handle == 0) && (args->jkey != 0)) {
			for (k = 0; k < UET_DEV_MAX_JOB_KEYS; k++) {
				if (dev->jkeys[k].used &&
				    (uet_dev_jkey_value((uint32_t)(k + 1)) ==
				     args->jkey)) {
					jkey_handle = (uint32_t)(k + 1);
					break;
				}
			}
		}

		if ((jkey_handle == 0) ||
		    (jkey_handle > UET_DEV_MAX_JOB_KEYS) ||
		    !dev->jkeys[jkey_handle - 1].used)
			return UET_DEV_ADMIN_ST_EINVAL;

		if (dev->jkeys[jkey_handle - 1].pdn != args->pd)
			return UET_DEV_ADMIN_ST_EINVAL;

		job = uet_dev_job_get(dev,
				      dev->jkeys[jkey_handle - 1].job_handle);
		if (job == NULL)
			return UET_DEV_ADMIN_ST_ENOENT;

		job_id = job->job_id;
	}

	if (args->flags & UET_DEV_MR_FLAG_USER_KEY)
		lib_flags |= UET_MR_FLAG_USER_KEY;

	rc = uet_mr_reg_pbl(dev->domain_handle, pbl_root, page_size,
			    lib_level, args->page_offset,
			    0, (size_t)len, access,
			    requested, lib_flags, NULL, &mr);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_mr_reg_pbl failed (%d)\n", rc);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	rc = uet_mr_set_access(mr, lib_class, job_id);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_mr_set_access failed (%d)\n", rc);
		uet_mr_close(mr);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	/* Enabled the MR except for an RI-restricted region which is not
	 * enabled and should be attached later by the application.
	 */
	if ((args->access_class != UET_DEV_MR_CLASS_RI_RESTRICTED) &&
	    (args->access_class != UET_DEV_MR_CLASS_RI_JOB_RESTRICTED)) {
		rc = uet_mr_enable(mr);
		if (rc != FI_SUCCESS) {
			fprintf(stderr, "uet_dev: uet_mr_enable failed (%d)\n",
				rc);
			uet_mr_close(mr);
			return UET_DEV_ADMIN_ST_EINVAL;
		}

		dev->mrs[i].enabled = true;
	}

	dev->mrs[i].used         = true;
	dev->mrs[i].mr           = mr;
	dev->mrs[i].key          = uet_mr_key(mr);
	dev->mrs[i].pd           = args->pd;
	dev->mrs[i].access_class = args->access_class;
	dev->mrs[i].job_id       = job_id;
	dev->mrs[i].base_va      = base_va;
	dev->mrs[i].len          = len;

	dev->mrs_live++;

	*out_handle = (uint32_t)(i + 1);
	*out_key = dev->mrs[i].key;

	uet_dev_trace("mr reg: handle %u key 0x%llx class %u pd %u %s",
		      *out_handle, (unsigned long long)dev->mrs[i].key,
		      args->access_class, args->pd,
		      dev->mrs[i].enabled ? "enabled" : "awaiting attach");

	return UET_DEV_ADMIN_ST_OK;
}

uint32_t uet_dev_mr_dereg(struct uet_dev *dev,
			  uint64_t handle)
{
	struct uet_dev_mr *mr = uet_dev_mr_get(dev, handle);
	int rc;

	if (mr == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	/* the library refuses to close a region that is still enabled */
	if (mr->enabled) {
		rc = uet_mr_disable(mr->mr);
		if (rc != FI_SUCCESS) {
			fprintf(stderr, "uet_dev: uet_mr_disable failed (%d)\n",
				rc);
			return UET_DEV_ADMIN_ST_EINVAL;
		}

		mr->enabled = false;
		mr->qp = 0;
	}

	rc = uet_mr_close(mr->mr);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: uet_mr_close failed (%d)\n", rc);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	memset(mr, 0, sizeof(*mr));
	dev->mrs_live--;

	return UET_DEV_ADMIN_ST_OK;
}

/* attach a region to a queue pair endpoint (resource index) */
uint32_t uet_dev_mr_attach(struct uet_dev *dev,
			   uint64_t mr_handle,
			   uint64_t qp_handle)
{
	struct uet_dev_mr *mr = uet_dev_mr_get(dev, mr_handle);
	struct uet_dev_qp *qp = uet_dev_qp_get(dev, qp_handle);
	int rc;

	if ((mr == NULL) || (qp == NULL))
		return UET_DEV_ADMIN_ST_ENOENT;

	if (mr->qp != 0)
		return UET_DEV_ADMIN_ST_EEXIST;

	if (mr->pd != qp->pd)
		return UET_DEV_ADMIN_ST_EINVAL;

	/* Attach means "restrict this region to this queue pair", so a region
	 * that is currently reachable through any of them is narrowed rather
	 * than refused. It has to be taken out of the domain's lookup space
	 * before proceeding.
	 */
	if (mr->enabled) {
		rc = uet_mr_disable(mr->mr);
		if (rc != FI_SUCCESS)
			return UET_DEV_ADMIN_ST_EBUSY;

		mr->enabled = false;
	}

	/* The bind is what narrows the class turning an unrestricted region
	 * into an RI-restricted one and a job-restricted region into an
	 * RI-job-restricted one.
	 */
	rc = uet_ep_bind_mr(qp->ep, mr->mr, UET_FLAGS_NONE);
	if (rc == FI_SUCCESS)
		rc = uet_mr_enable(mr->mr);

	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: attach failed (%d)\n", rc);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	/* narrow the access class based on previous scope */
	if (mr->access_class == UET_DEV_MR_CLASS_JOB_RESTRICTED)
		mr->access_class = UET_DEV_MR_CLASS_RI_JOB_RESTRICTED;
	else if (mr->access_class == UET_DEV_MR_CLASS_UNRESTRICTED)
		mr->access_class = UET_DEV_MR_CLASS_RI_RESTRICTED;

	mr->qp = (uint32_t)qp_handle;
	mr->enabled = true;

	return UET_DEV_ADMIN_ST_OK;
}

uint32_t uet_dev_mr_detach(struct uet_dev *dev,
			   uint64_t mr_handle)
{
	struct uet_dev_mr *mr = uet_dev_mr_get(dev, mr_handle);
	int rc;

	if (mr == NULL)
		return UET_DEV_ADMIN_ST_ENOENT;

	if (mr->qp == 0)
		return UET_DEV_ADMIN_ST_EINVAL;

	rc = uet_mr_disable(mr->mr);
	if (rc != FI_SUCCESS) {
		fprintf(stderr, "uet_dev: detach failed (%d)\n", rc);
		return UET_DEV_ADMIN_ST_EINVAL;
	}

	mr->qp = 0;
	mr->enabled = false;

	return UET_DEV_ADMIN_ST_OK;
}

