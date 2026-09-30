/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - the datapath
 *
 * Work requests in, completions out. The progress thread scans the rings a
 * queue pair owns, hands each request to the library, and writes what comes
 * back into the guest's completion queue.
 *
 * The rings live in guest memory and are read through the backend on every
 * sweep as the guest could be filling them while this runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uet_dev_priv.h"
#include "uet_dev_data.h"
#include "uet_dev_objs.h"

/* Turn an application address into an offset from a region's start. A work
 * request's segment list carries addresses the application owns. The library
 * holds the region zero-based so every segment has to be rebased before it
 * is handed over. Returns false when the range is not wholly within the
 * region.
 */
static inline bool uet_dev_mr_offset(const struct uet_dev_mr *mr,
				     uint64_t addr,
				     uint64_t len,
				     uint64_t *offset)
{
	if ((addr < mr->base_va) || (len > mr->len))
		return false;

	if ((addr - mr->base_va) > (mr->len - len))
		return false;

	*offset = (addr - mr->base_va);

	return true;
}

/* Get the guest address of byte offset within a ring. The ring is
 * described by a PBL. Find the page holding that offset, then add the
 * offset within it. A level-0 list is one contiguous run and needs no
 * indirection. Level-1 reads the page address out of the root directory.
 * Resolving by byte offset rather than by entry is what lets an entry span
 * two pages. Returns 0 if the ring cannot be walked.
 */
static uint64_t uet_dev_ring_addr(struct uet_dev *dev,
				  struct uet_dev_ring *ring,
				  uint64_t byte_off)
{
	uint64_t slot, within, ent;

	if (ring->desc.page_size == 0)
		return 0;

	switch (ring->desc.level) {
	case UET_DEV_PBL_LEVEL_0:
		return (ring->desc.pbl_root + byte_off);

	case UET_DEV_PBL_LEVEL_1:
		slot = (byte_off / ring->desc.page_size);
		within = (byte_off % ring->desc.page_size);

		if (dev->backend.dma_read == NULL)
			return 0;

		if (dev->backend.dma_read(dev->backend.ctx,
					  (ring->desc.pbl_root +
					   (slot * sizeof(uint64_t))),
					  &ent, sizeof(ent)) != 0)
			return 0;

		if (ent == 0)
			return 0;

		return (ent + within);

	default:
		/* FIXME: a level-2 list would be used for VERY large rings */
		return 0;
	}
}

/* Where a specified entry begins, as a byte offset into the ring. */
static uint64_t uet_dev_ring_entry_off(struct uet_dev_ring *ring,
				       uint32_t index,
				       uint32_t entry_size)
{
	return ((uint64_t)ring->desc.page_offset +
		((uint64_t)index * entry_size));
}

/* How much of an entry may be transferred, starting at done bytes. */
static uint32_t uet_dev_ring_chunk(struct uet_dev_ring *ring,
				   uint64_t off,
				   uint32_t entry_size,
				   uint32_t done)
{
	uint32_t left = (entry_size - done);
	uint32_t room;

	if (ring->desc.level == UET_DEV_PBL_LEVEL_0)
		return left;

	room = (uint32_t)(ring->desc.page_size -
			  (off % ring->desc.page_size));

	return (left < room) ? left : room;
}

/*
 * Move one entry between to/from a ring in guest memory and buf. The two
 * directions differ only in which backend call carries the bytes so they
 * share the page walk. The address is resolved per chunk, which is what
 * lets an entry span two pages.
 */
static int uet_dev_ring_xfer(struct uet_dev *dev,
			     struct uet_dev_ring *ring,
			     uint32_t index,
			     void *buf,
			     uint32_t entry_size,
			     bool write)
{
	uint64_t base_off, off, gpa;
	uint32_t done = 0;
	uint32_t chunk;
	int rc;

	if ((write) ? (dev->backend.dma_write == NULL)
		    : (dev->backend.dma_read == NULL))
		return -1;

	if ((ring->desc.entries == 0) || (index >= ring->desc.entries))
		return -1;

	base_off = uet_dev_ring_entry_off(ring, index, entry_size);

	while (done < entry_size) {
		off = (base_off + done);

		gpa = uet_dev_ring_addr(dev, ring, off);
		if (gpa == 0)
			return -1;

		chunk = uet_dev_ring_chunk(ring, off, entry_size, done);

		rc = write ? dev->backend.dma_write(dev->backend.ctx, gpa,
						   ((char *)buf + done),
						   chunk)
			   : dev->backend.dma_read(dev->backend.ctx, gpa,
						   ((char *)buf + done),
						   chunk);
		if (rc != 0)
			return -1;

		done += chunk;
	}

	return 0;
}

static int uet_dev_ring_read(struct uet_dev *dev, struct uet_dev_ring *ring,
			     uint32_t index, void *buf, uint32_t entry_size)
{
	return uet_dev_ring_xfer(dev, ring, index, buf, entry_size, false);
}

static int uet_dev_ring_write(struct uet_dev *dev, struct uet_dev_ring *ring,
			      uint32_t index, const void *buf,
			      uint32_t entry_size)
{
	return uet_dev_ring_xfer(dev, ring, index, (void *)(uintptr_t)buf,
				 entry_size, true);
}

/* Post one completion into a guest completion queue. The valid word is
 * written with the entry rather than after it, because the whole 64 bytes
 * reach guest memory in one dma_write. There is no window in which driver
 * could see a half-built entry. On a real bus this would need the payload
 * to land before the valid flag.
 */
static void uet_dev_cq_post(struct uet_dev *dev,
			    struct uet_dev_cq *cq,
			    struct uet_dev_work *work,
			    uint32_t status,
			    uint32_t byte_len,
			    uint64_t imm,
			    uint32_t src_id)
{
	struct uet_dev_cqe cqe;
	uint32_t slot;

	if ((cq == NULL) || !cq->ring.live)
		return;

	/* the ring is full when the next slot is the one the guest is on */
	if ((cq->ring.prod - cq->ring.cons) >= cq->ring.desc.entries) {
		dev->cq_overruns++;

		uet_dev_trace("completion queue full: dropped a completion "
			      "for qp 0x%x, wr_id 0x%llx (%u so far)",
			      work->qpn, (unsigned long long)work->wr_id,
			      dev->cq_overruns);
		return;
	}

	slot = (cq->ring.prod % cq->ring.desc.entries);

	memset(&cqe, 0, sizeof(cqe));

	cqe.wr_id     = work->wr_id;
	cqe.imm       = imm;
	cqe.status    = status;
	cqe.opcode    = work->opcode;
	cqe.byte_len  = byte_len;
	cqe.qpn       = work->qpn;
	cqe.src_id    = src_id;
	cqe.job_id    = work->job_id;
	cqe.ring_cons = work->ring_cons;
	cqe.flags     = work->cqe_flags;
	cqe.valid     = cq->phase;

	if (uet_dev_ring_write(dev, &cq->ring, slot, &cqe,
			       UET_DEV_CQE_SIZE) != 0) {
		dev->cq_errors++;
		return;
	}

	cq->ring.prod++;

	if ((cq->ring.prod % cq->ring.desc.entries) == 0)
		cq->phase ^= 1;

	dev->cq_posted++;

	uet_dev_trace("completion: op %u status %u len %u wr_id 0x%llx",
		      work->opcode, status, byte_len,
		      (unsigned long long)work->wr_id);

	/* Arming is one-shot: the guest asks to be told about the next
	 * completion, gets one interrupt, and must ask again. Anything else
	 * would raise an interrupt per completion for a guest that is
	 * already polling.
	 */
	if (cq->armed) {
		cq->armed = false;

		if (dev->backend.raise_irq != NULL)
			dev->backend.raise_irq(dev->backend.ctx, cq->vector);
	}
}

/* Translate provider library error completions into guest completions. */
static void uet_dev_cq_drain_err(struct uet_dev *dev,
				 struct uet_dev_qp *qp,
				 uet_cq_handle_t src)
{
	struct fi_cq_err_entry err;
	struct uet_dev_work *work;
	uint32_t status;
	size_t i;

	for (i = 0; i < UET_DEV_ERRS_PER_SWEEP; i++) {
		memset(&err, 0, sizeof(err));

		if (uet_cq_readerr(src, &err) != 1)
			return;

		work = err.op_context;
		if ((work == NULL) || !work->busy)
			continue;

		/* A request the guest asked to be abandoned completes as
		 * flushed, anything else is a genuine failure. The
		 * distinction matters to an application. One it caused, the
		 * other it did not.
		 */
		status = (err.err == FI_ECANCELED) ? UET_DEV_WC_FLUSHED
						   : UET_DEV_WC_GENERAL_ERR;

		uet_dev_cq_post(dev, uet_dev_cq_get(dev, work->cq), work,
				status, 0, 0, 0);

		work->busy = false;
	}
}

/* Translate provider library completions into a guest completions. The
 * op_context corresponds to the work requres the device handed the library
 * when it was posted. This is how a completion finds its way back to the
 * ring slot and the completion queue that should receive it.
 */
void uet_dev_cq_drain(struct uet_dev *dev,
		      struct uet_dev_qp *qp,
		      uet_cq_handle_t src)
{
	struct fi_cq_data_entry ents[8];
	struct uet_dev_work *work;
	struct uet_dev_cq *cq;
	struct uet_dev_job *ujob;
	struct uet_dev_work u; /* unsolicited */
	ssize_t n, i;

	if (src == NULL)
		return;

	/* An error completion is not returned with the successful ones. The
	 * library reports -FI_EAVAIL when the entry at the head of the queue
	 * is an error, and it stays there until it is read with
	 * uet_cq_readerr().
	 */
	n = uet_cq_read(src, ents, 8);
	if (n == -FI_EAVAIL) {
		uet_dev_cq_drain_err(dev, qp, src);
		return;
	}

	if (n <= 0)
		return;

	uet_dev_trace("library completions: %zd from %s queue", n,
		      (src == qp->tx_cq) ? "transmit" : "receive");

	for (i = 0; i < n; i++) {
		work = ents[i].op_context;

		/* A completion with no context is unsolicited. For example,
		 * an RDMA write that carried immediate data placed its
		 * own payload and the immediate is reported at the target
		 * without consuming a receive. Report it against the queue
		 * pair.
		 */
		if ((work == NULL) && (src == qp->rx_cq)) {
			ujob = NULL;

			if (qp->jkey_handle != 0) {
				ujob = uet_dev_job_by_jkey(dev,
					uet_dev_jkey_value(qp->jkey_handle),
					qp->pd);
			}

			memset(&u, 0, sizeof(u));

			u.qpn       = qp->qpn;
			u.opcode    = UET_DEV_WR_RECV_RDMA_IMM;
			u.cq        = qp->recv_cq;
			u.job_id    = (ujob != NULL) ? ujob->job_id : 0;
			u.ring_cons = qp->rq.cons;
			u.cqe_flags = UET_DEV_CQE_F_IMM;
			u.signaled  = true;

			uet_dev_cq_post(dev,
					uet_dev_cq_get(dev, qp->recv_cq),
					&u,
					UET_DEV_WC_SUCCESS,
					(uint32_t)ents[i].len,
					ents[i].data,
					uet_cq_read_src_id(src));
			continue;
		}

		if ((work == NULL) || !work->busy) {
			uet_dev_trace("  dropped: context %p%s",
				      ents[i].op_context,
				      (work == NULL) ? " (none)"
						     : " (not busy)");
			continue;
		}

		if (ents[i].flags & FI_REMOTE_CQ_DATA)
			work->cqe_flags |= UET_DEV_CQE_F_IMM;

		cq = uet_dev_cq_get(dev, work->cq);
		if (work->signaled) {
			uet_dev_cq_post(dev, cq, work, UET_DEV_WC_SUCCESS,
					(uint32_t)ents[i].len, ents[i].data,
					uet_cq_read_src_id(src));
		}

		work->busy = false;
	}
}

/* consume work requests the guest has produced on the RQ */
static void uet_dev_rq_run(struct uet_dev *dev,
			   struct uet_dev_qp *qp)
{
	uint32_t slot;
	struct uet_dev_work *work;
	struct uet_mr_seg segs[UET_DEV_MAX_SGE];
	struct uet_dev_job *rjob;
	struct uet_dev_wqe wqe;
	struct uet_dev_mr *mr;
	uint32_t jkey;
	uint32_t qp_jobid;
	uint32_t i;
	ssize_t rc;

	while (qp->rq.cons != qp->rq.prod) {
		slot = (qp->rq.cons % qp->rq.desc.entries);
		work = &qp->rq_work[slot];

		if (work->busy)
			return;

		if (uet_dev_ring_read(dev, &qp->rq, slot, &wqe,
				      UET_DEV_WQE_SIZE) != 0) {
			dev->rq_errors++;
			return;
		}

		if (wqe.num_sge > UET_DEV_MAX_SGE) {
			qp->rq.cons++;
			continue;
		}

		/* A receive names no peer, but it does belong to a job: an
		 * incoming packet carries a JobID and the buffer only matches
		 * if the two agree. The job comes from the request's key, or
		 * from the queue pair's when it names none, exactly as for a
		 * send.
		 */
		jkey = wqe.jkey;

		if ((jkey == 0) && (qp->jkey_handle != 0))
			jkey = uet_dev_jkey_value(qp->jkey_handle);

		rjob = uet_dev_job_by_jkey(dev, jkey, qp->pd);
		if (rjob == NULL) {
			qp->rq.cons++;
			continue;
		}

		memset(work, 0, sizeof(*work));
		qp_jobid = uet_dev_qp_job_id(dev, qp);

		work->wr_id     = wqe.wr_id;
		work->qpn       = qp->qpn;
		work->opcode    = UET_DEV_WR_RECV;
		work->cq        = qp->recv_cq;
		work->job_id    = (qp_jobid) ? qp_jobid : rjob->job_id;
		work->ring_cons = (qp->rq.cons + 1);
		work->signaled  = true;	/* a receive always completes */

		/* verify the sge list and prep for the provider library */
		for (i = 0; i < wqe.num_sge; i++) {
			mr = uet_dev_mr_by_key(dev, wqe.sge[i].key);
			if ((mr == NULL) || !mr->enabled) {
				uet_dev_trace("recv: key 0x%llx %s",
					      (unsigned long long)wqe.sge[i].key,
					      (mr == NULL) ? "invalid region"
							   : "region not enabled");

				uet_dev_cq_post(dev,
						uet_dev_cq_get(dev, work->cq),
						work,
						UET_DEV_WC_BAD_KEY,
						0, 0, 0);
				goto next;
			}

			if (!uet_dev_mr_offset(mr, wqe.sge[i].addr,
					       wqe.sge[i].len,
					       &segs[i].addr)) {
				uet_dev_trace("recv: segment [0x%llx, +%u) is "
					      "not within region [0x%llx, +%llu)",
					      (unsigned long long)wqe.sge[i].addr,
					      wqe.sge[i].len,
					      (unsigned long long)mr->base_va,
					      (unsigned long long)mr->len);

				uet_dev_cq_post(dev,
						uet_dev_cq_get(dev, work->cq),
						work,
						UET_DEV_WC_BAD_LOCAL_ADDR,
						0, 0, 0);
				goto next;
			}

			segs[i].mr = mr->mr;
			segs[i].len = wqe.sge[i].len;
		}

		work->busy = true;

		uet_dev_trace("recv posted: job %u sge %u wr_id 0x%llx",
			      rjob->job_id, wqe.num_sge,
			      (unsigned long long)wqe.wr_id);

		rc = uet_recvseg(qp->ep, rjob->job_id, segs, wqe.num_sge,
				 NULL, work);
		if (rc < 0) {
			work->busy = false;

			if (rc == -FI_EAGAIN)
				return;

			uet_dev_cq_post(dev, uet_dev_cq_get(dev, work->cq),
					work, UET_DEV_WC_GENERAL_ERR,
					0, 0, 0);
		} else {
			dev->rq_posted++;
		}

next:
		qp->rq.cons++;
	}
}

/* consume work requests the guest has produced on the SQ */
static void uet_dev_sq_run(struct uet_dev *dev,
			   struct uet_dev_qp *qp)
{
	uint16_t ri = (uint16_t)((qp->qpn & UET_DEV_QPN_RI_MASK) >>
				 UET_DEV_QPN_RI_SHIFT);
	uint32_t qp_jobid = uet_dev_qp_job_id(dev, qp);
	uint32_t slot;
	struct uet_dev_mr *mr;
	struct uet_dev_work *work;
	struct uet_mr_seg segs[UET_DEV_MAX_SGE];
	struct uet_dev_addr_entry *aslot;
	struct uet_dev_ah *ah;
	struct uet_dev_job *job;
	uet_addr_handle_t av;
	struct uet_dev_wqe wqe;
	uint32_t status;
	uint64_t imm;
	uint64_t *imm_p;
	size_t total;
	uint32_t i;
	ssize_t rc;

	while (qp->sq.cons != qp->sq.prod) {
		slot = (qp->sq.cons % qp->sq.desc.entries);
		work = &qp->sq_work[slot];
		aslot = NULL;
		ah = NULL;
		job = NULL;
		status = UET_DEV_WC_SUCCESS;
		imm = 0;
		total = 0;

		/* A ring we cannot read is not going to become readable by
		 * being left alone, and returning here silently retries it
		 * for ever. The request never completes and never errors,
		 * which from the guest is indistinguishable from a lost one.
		 * Complete it with error and move on.
		 */
		if (uet_dev_ring_read(dev, &qp->sq, slot, &wqe,
				      UET_DEV_WQE_SIZE) != 0) {
			uet_dev_trace("send ring unreadable: qp %u slot %u "
				      "root 0x%llx level %u page %u entries %u",
				      qp->qpn, slot,
				      (unsigned long long)qp->sq.desc.pbl_root,
				      qp->sq.desc.level, qp->sq.desc.page_size,
				      qp->sq.desc.entries);

			dev->sq_errors++;

			memset(work, 0, sizeof(*work));

			work->qpn       = qp->qpn;
			work->cq        = qp->send_cq;
			work->ring_cons = (qp->sq.cons + 1);
			work->signaled  = true;

			uet_dev_cq_post(dev, uet_dev_cq_get(dev, work->cq),
					work, UET_DEV_WC_GENERAL_ERR,
					0, 0, 0);

			qp->sq.cons++;
			continue;
		}

		if (work->busy) /* the slot has not completed yet */
			return;

		memset(work, 0, sizeof(*work));

		work->wr_id     = wqe.wr_id;
		work->qpn       = qp->qpn;
		work->opcode    = wqe.opcode;
		work->cq        = qp->send_cq;
		work->ring_cons = (qp->sq.cons + 1);
		work->signaled  = ((wqe.flags & UET_DEV_WR_F_SIGNALED) != 0);

		if (wqe.num_sge > UET_DEV_MAX_SGE)
			status = UET_DEV_WC_TOO_LONG;

		/* verify the sge list and prep for the provider library */
		for (i = 0;
		     (status == UET_DEV_WC_SUCCESS) && (i < wqe.num_sge);
		     i++) {
			mr = uet_dev_mr_by_key(dev, wqe.sge[i].key);
			if ((mr == NULL) || !mr->enabled) {
				uet_dev_trace("send: key 0x%llx %s",
					      (unsigned long long)wqe.sge[i].key,
					      (mr == NULL) ? "invalid region"
							   : "region not enabled");

				status = UET_DEV_WC_BAD_KEY;
				break;
			}

			if (!uet_dev_mr_offset(mr, wqe.sge[i].addr,
					       wqe.sge[i].len,
					       &segs[i].addr)) {
				uet_dev_trace("send: segment [0x%llx, +%u) is "
					      "not within region [0x%llx, +%llu)",
					      (unsigned long long)wqe.sge[i].addr,
					      wqe.sge[i].len,
					      (unsigned long long)mr->base_va,
					      (unsigned long long)mr->len);

				status = UET_DEV_WC_BAD_LOCAL_ADDR;
				break;
			}

			segs[i].mr = mr->mr;
			segs[i].len = wqe.sge[i].len;

			total += wqe.sge[i].len;
		}

		/* The job comes from the key the request names, or from the
		 * one the queue pair was created with when it names none.
		 */
		if (status == UET_DEV_WC_SUCCESS) {
			uint32_t jkey = wqe.jkey;

			if ((jkey == 0) && (qp->jkey_handle != 0))
				jkey = uet_dev_jkey_value(qp->jkey_handle);

			job = uet_dev_job_by_jkey(dev, jkey, qp->pd);
			if (job == NULL) {
				uet_dev_trace("send: no job for key 0x%x "
					      "(request 0x%x, queue pair 0x%x)",
					      jkey, wqe.jkey, qp->jkey_handle);

				status = UET_DEV_WC_BAD_JOB;
			}
		}

		/* The peer is either an address handle or an index into that
		 * job's table. Both end at the same place, a library address
		 * vector entry.
		 */
		if (status == UET_DEV_WC_SUCCESS) {
			if (wqe.flags & UET_DEV_WR_F_AH) {
				ah = uet_dev_ah_get(dev, wqe.addr_index);
				if (ah == NULL) {
					uet_dev_trace("send: no address handle "
						      "%u", wqe.addr_index);

					status = UET_DEV_WC_BAD_ADDR;
				}
			} else {
				aslot = uet_dev_addr_slot_job(job,
							      wqe.addr_index);
				if ((aslot == NULL) || !aslot->valid) {
					uet_dev_trace("send: job %u has no "
						      "address at index %u",
						      job->job_id,
						      wqe.addr_index);

					status = UET_DEV_WC_BAD_ADDR;
				}
			}
		}

		if (status != UET_DEV_WC_SUCCESS) {
			work->busy = false;

			if (work->signaled) {
				uet_dev_cq_post(dev,
						uet_dev_cq_get(dev, work->cq),
						work,
						status,
						0, 0, 0);
			}

			qp->sq.cons++;
			continue;
		}

		av = (ah) ? uet_dev_ah_av(dev, ah)
			  : uet_dev_addr_av(dev, aslot);
		if (av == NULL) {
			if (work->signaled) {
				uet_dev_cq_post(dev,
						uet_dev_cq_get(dev, work->cq),
						work,
						UET_DEV_WC_BAD_ADDR,
						0, 0, 0);
			}

			qp->sq.cons++;
			continue;
		}

		work->busy = true;

		/* the pair's own job, not the one this request addresses */
		work->job_id = (qp_jobid) ? qp_jobid : job->job_id;

		uet_dev_trace("send posted: op %u job %u bytes %zu wr_id 0x%llx",
			      wqe.opcode, job->job_id, total,
			      (unsigned long long)wqe.wr_id);

		/* execute the work request with the provider library */
		if ((wqe.opcode == UET_DEV_WR_RDMA_WRITE) ||
		    (wqe.opcode == UET_DEV_WR_RDMA_WRITE_IMM)) {
			imm_p = NULL;

			if (wqe.opcode == UET_DEV_WR_RDMA_WRITE_IMM) {
				imm = wqe.imm;
				imm_p = &imm;
			}

			rc = uet_writeseg(qp->ep, job->job_id, segs,
					  wqe.num_sge, imm_p, av,
					  wqe.remote_addr, wqe.remote_key,
					  work, ri);
		} else if (wqe.opcode == UET_DEV_WR_RDMA_READ) {
			rc = uet_readseg(qp->ep, job->job_id, segs,
					 wqe.num_sge, av, wqe.remote_addr,
					 wqe.remote_key, work, ri);
		} else if (wqe.opcode == UET_DEV_WR_SEND_IMM) {
			imm = wqe.imm;
			rc = uet_sendseg_imm(qp->ep, job->job_id, segs,
					     wqe.num_sge, &imm, av, work, ri);
		} else {
			rc = uet_sendseg(qp->ep, job->job_id, segs,
					 wqe.num_sge, av, work, ri);
		}

		if (rc < 0) {
			work->busy = false;

			/* Out of resources is not an error the guest caused,
			 * so the entry stays unconsumed and is retried on the
			 * next sweep. Anything else completes in error.
			 */
			if (rc == -FI_EAGAIN)
				return;

			if (work->signaled) {
				uet_dev_cq_post(dev,
						uet_dev_cq_get(dev, work->cq),
						work,
						UET_DEV_WC_GENERAL_ERR,
						0, 0, 0);
			}
		}

		qp->sq.cons++;
		dev->sq_posted++;
	}
}

/* Finish pending resource teardowns that the library was not ready for. A
 * guest's destroy always succeeds. What it could not complete lands here
 * and is retried until the library lets go.
 */
static void uet_dev_retire_poll(struct uet_dev *dev)
{
	struct uet_dev_job *job;
	struct uet_dev_addr_entry *slot;
	size_t i, j;

	/* retire pending jobs */
	for (i = 0; i < UET_DEV_MAX_JOB_IDS; i++) {
		job = &dev->jobs[i];

		if (!job->used || !job->retiring)
			continue;

		if (!uet_dev_job_addrs_release(job))
			continue;

		uet_dev_trace("job free: id %u (%u live, was retiring)",
			      job->job_id, (dev->jobs_live - 1));

		free(job->addrs);

		memset(job, 0, sizeof(*job));

		dev->jobs_retiring--;
		dev->jobs_live--;
	}

	/* retire pending job address table entries */
	for (i = 0; i < UET_DEV_MAX_JOB_IDS; i++) {
		job = &dev->jobs[i];

		if (!job->used || job->retiring || (job->addrs == NULL))
			continue;

		for (j = 0; j < job->max_addr_entries; j++) {
			slot = &job->addrs[j];

			if (!slot->retiring || (slot->av == NULL))
				continue;

			if (uet_av_remove(slot->av) == FI_SUCCESS)
				memset(slot, 0, sizeof(*slot));
		}
	}
}

/* one loop over every live qp, reap what's done, post what's pending */
void uet_dev_datapath_poll(struct uet_dev *dev)
{
	size_t i;

	/* The object lock is taken per queue pair rather than around the
	 * whole sweep. Holding it across all of them can starve the admin
	 * queue. Receives first, across every queue pair, before any
	 * transmit.
	 */
	for (i = 0; i < UET_DEV_MAX_QPS; i++) {
		struct uet_dev_qp *qp = &dev->qps[i];

		if (!qp->used)
			continue;

		pthread_mutex_lock(&dev->obj_lock);

		if (qp->used && !qp->closing &&
		    (qp->state == UET_DEV_QPS_RTS) && qp->rq.live)
			uet_dev_rq_run(dev, qp);

		pthread_mutex_unlock(&dev->obj_lock);
	}

	for (i = 0; i < UET_DEV_MAX_QPS; i++) {
		struct uet_dev_qp *qp = &dev->qps[i];

		if (!qp->used)
			continue;

		pthread_mutex_lock(&dev->obj_lock);

		/* it may have gone while we were not holding the lock */
		if (qp->used && qp->closing) {
			uet_dev_qp_retire(dev, qp);
		} else if (qp->used && (qp->state == UET_DEV_QPS_RTS)) {
			if (qp->sq.live)
				uet_dev_sq_run(dev, qp);

			uet_dev_cq_drain(dev, qp, qp->tx_cq);
			uet_dev_cq_drain(dev, qp, qp->rx_cq);
		}

		pthread_mutex_unlock(&dev->obj_lock);
	}

	pthread_mutex_lock(&dev->obj_lock);
	uet_dev_retire_poll(dev);
	pthread_mutex_unlock(&dev->obj_lock);
}

