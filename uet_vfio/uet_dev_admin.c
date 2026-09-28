/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - the slowpath admin queue
 *
 * The driver posts commands into the admin SQ ring in guest memory and
 * rings a doorbell. The doorbell results in the pending commands being
 * drained from the ring. Each command is executed against the object
 * model and a response completion is written back to the admin CQ..
 */

#include <string.h>

#include "uet_dev_priv.h"
#include "uet_dev_admin.h"
#include "uet_dev_objs.h"

/* execute one command and fill in the completion to return */
static void uet_dev_admin_execute(struct uet_dev *dev,
				  const struct uet_dev_admin_cmd *cmd,
				  struct uet_dev_admin_cqe *cqe)
{
	cqe->cookie = cmd->cookie;
	cqe->opcode = cmd->opcode;
	cqe->result[0] = 0;
	cqe->result[1] = 0;

	switch (cmd->opcode) {
	case UET_DEV_ADMIN_NOP:
		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;

	case UET_DEV_ADMIN_DEV_INFO: {
		struct uet_dev_admin_dev_info info;

		if (cmd->buf_len < sizeof(info)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(info);
			break;
		}

		memset(&info, 0, sizeof(info));
		info.caps = dev->caps;
		info.max_job_ids = UET_DEV_MAX_JOB_IDS;
		info.max_job_keys = UET_DEV_MAX_JOB_KEYS;
		info.max_addr_entries = UET_DEV_MAX_ADDR_ENTRIES;
		info.max_imm_size = dev->max_payload;
		info.max_pds = UET_DEV_MAX_PDS_ADVISORY;

		if ((dev->backend.dma_write == NULL) ||
		    (dev->backend.dma_write(dev->backend.ctx, cmd->buf_addr,
					    &info, sizeof(info)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->result[0] = sizeof(info);
		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;
	}

	case UET_DEV_ADMIN_JOB_ALLOC: {
		uint32_t status, handle;

		handle = uet_dev_job_alloc(dev, (uint32_t)cmd->param[0],
					   (uint32_t)cmd->param[1],
					   (uint8_t)(cmd->param[2] & 0xff),
					   (uint8_t)((cmd->param[2] >> 8) &
						     0xff),
					   (uint32_t)cmd->param[3], &status);
		cqe->result[0] = handle;
		cqe->status = status;
		break;
	}

	case UET_DEV_ADMIN_JOB_FREE:
		cqe->status = uet_dev_job_free(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_JOB_QUERY: {
		struct uet_dev_job *job = uet_dev_job_get(dev, cmd->param[0]);
		struct uet_dev_admin_job_info info;

		if (job == NULL) {
			cqe->status = UET_DEV_ADMIN_ST_ENOENT;
			break;
		}

		if (cmd->buf_len < sizeof(info)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(info);
			break;
		}

		memset(&info, 0, sizeof(info));
		info.job_id = job->job_id;
		info.max_addr_entries = job->max_addr_entries;
		info.flags = job->flags;
		info.port_num = job->port_num;
		info.sgid_index = job->sgid_index;

		if ((dev->backend.dma_write == NULL) ||
		    (dev->backend.dma_write(dev->backend.ctx, cmd->buf_addr,
					    &info, sizeof(info)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->result[0] = sizeof(info);
		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;
	}

	case UET_DEV_ADMIN_JKEY_CREATE: {
		uint32_t handle = 0, jkey = 0;

		cqe->status = uet_dev_jkey_create(dev, cmd->param[0],
						  (uint32_t)cmd->param[1],
						  (uint32_t)cmd->param[2],
						  &handle, &jkey);
		cqe->result[0] = handle;
		cqe->result[1] = jkey;
		break;
	}

	case UET_DEV_ADMIN_JKEY_DESTROY:
		cqe->status = uet_dev_jkey_destroy(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_ADDR_INSERT: {
		struct uet_dev_addr_entry *slot;
		struct uet_dev_admin_addr addr;

		slot = uet_dev_addr_slot(dev, cmd->param[0], cmd->param[1]);
		if (slot == NULL) {
			cqe->status = UET_DEV_ADMIN_ST_ENOENT;
			break;
		}

		if (cmd->buf_len < sizeof(addr)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(addr);
			break;
		}

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, cmd->buf_addr,
					   &addr, sizeof(addr)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		/* The library holds a pointer into this slot so release the
		 * old entry before overwriting the entry. */
		if (slot->av != NULL) {
			uet_av_remove(slot->av);
			slot->av = NULL;
		}

		slot->addr = addr;
		slot->valid = true;

		cqe->result[0] = sizeof(addr);
		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;
	}

	case UET_DEV_ADMIN_ADDR_REMOVE: {
		struct uet_dev_addr_entry *slot;

		slot = uet_dev_addr_slot(dev, cmd->param[0], cmd->param[1]);
		if ((slot == NULL) || !slot->valid) {
			cqe->status = UET_DEV_ADMIN_ST_ENOENT;
			break;
		}

		/* Release the address from the library before the address
		 * goes away. If the library still has operations against it,
		 * the entry cannot be removed yet, but the guest's removal
		 * still succeeds. The slot stops resolving immediately and
		 * its storage is held as retiring until the progress thread
		 * can finish the job.
		 */
		if ((slot->av != NULL) && (uet_av_remove(slot->av) != FI_SUCCESS)) {
			slot->valid = false;
			slot->retiring = true;
		} else {
			memset(slot, 0, sizeof(*slot));
		}

		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;
	}

	case UET_DEV_ADMIN_ADDR_QUERY: {
		struct uet_dev_addr_entry *slot;

		slot = uet_dev_addr_slot(dev, cmd->param[0], cmd->param[1]);
		if ((slot == NULL) || !slot->valid) {
			cqe->status = UET_DEV_ADMIN_ST_ENOENT;
			break;
		}

		if (cmd->buf_len < sizeof(slot->addr)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(slot->addr);
			break;
		}

		if ((dev->backend.dma_write == NULL) ||
		    (dev->backend.dma_write(dev->backend.ctx, cmd->buf_addr,
					    &slot->addr,
					    sizeof(slot->addr)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->result[0] = sizeof(slot->addr);
		cqe->status = UET_DEV_ADMIN_ST_OK;
		break;
	}

	case UET_DEV_ADMIN_MR_REG: {
		struct uet_dev_admin_mr_reg args;
		uint32_t handle = 0;
		uint64_t key = 0;

		if (cmd->buf_len < sizeof(args)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(args);
			break;
		}

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, cmd->buf_addr,
					   &args, sizeof(args)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->status = uet_dev_mr_reg(dev, &args, cmd->param[0],
					     (uint32_t)(cmd->param[1] &
							0xffffffffu),
					     (uint32_t)(cmd->param[1] >> 32),
					     cmd->param[2], cmd->param[3],
					     &handle, &key);
		cqe->result[0] = handle;
		cqe->result[1] = key;
		break;
	}

	case UET_DEV_ADMIN_MR_DEREG:
		cqe->status = uet_dev_mr_dereg(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_MR_ATTACH:
		cqe->status = uet_dev_mr_attach(dev, cmd->param[0],
						cmd->param[1]);
		break;

	case UET_DEV_ADMIN_MR_DETACH:
		cqe->status = uet_dev_mr_detach(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_CQ_CREATE: {
		struct uet_dev_admin_cq_create args;
		uint32_t handle = 0;

		if (cmd->buf_len < sizeof(args)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(args);
			break;
		}

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, cmd->buf_addr,
					   &args, sizeof(args)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->status = uet_dev_cq_create(dev, &args, &handle);
		cqe->result[0] = handle;
		break;
	}

	case UET_DEV_ADMIN_CQ_DESTROY:
		cqe->status = uet_dev_cq_destroy(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_QP_CREATE: {
		struct uet_dev_admin_qp_create args;
		uint32_t handle = 0;

		if (cmd->buf_len < sizeof(args)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(args);
			break;
		}

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, cmd->buf_addr,
					   &args, sizeof(args)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->status = uet_dev_qp_create(dev, &args, &handle);
		cqe->result[0] = handle;
		break;
	}

	case UET_DEV_ADMIN_QP_DESTROY:
		cqe->status = uet_dev_qp_destroy(dev, cmd->param[0]);
		break;

	case UET_DEV_ADMIN_QP_MODIFY:
		cqe->status = uet_dev_qp_modify(dev, cmd->param[0],
						(uint32_t)cmd->param[1]);
		break;

	case UET_DEV_ADMIN_AH_CREATE: {
		struct uet_dev_admin_addr addr;
		uint32_t handle = 0;

		if (cmd->buf_len < sizeof(addr)) {
			cqe->status = UET_DEV_ADMIN_ST_ETOOSMALL;
			cqe->result[0] = sizeof(addr);
			break;
		}

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, cmd->buf_addr,
					   &addr, sizeof(addr)) != 0)) {
			cqe->status = UET_DEV_ADMIN_ST_EINVAL;
			break;
		}

		cqe->status = uet_dev_ah_create(dev, (uint32_t)cmd->param[0],
						&addr, &handle);
		cqe->result[0] = handle;
		break;
	}

	case UET_DEV_ADMIN_AH_DESTROY:
		cqe->status = uet_dev_ah_destroy(dev, cmd->param[0]);
		break;

	default:
		cqe->status = UET_DEV_ADMIN_ST_EOPNOTSUPP;
		break;
	}

	cqe->flags = UET_DEV_ADMIN_CQE_VALID;
}

/* Is there room in the completion ring for one entry? */
static bool uet_dev_admin_cq_has_room(const struct uet_dev *dev)
{
	return ((dev->admin_cq_prod - dev->admin_cq_cons) <
		dev->admin_cq_entries);
}

/* Drain all commands the guest has produced and the device has not yet
 * consumed. Called from the doorbell write, with ring_lock held.
 */
void uet_dev_admin_drain_locked(struct uet_dev *dev)
{
	struct uet_dev_admin_cmd cmd;
	struct uet_dev_admin_cqe cqe;
	uint64_t slot;
	bool ran = false;

	if (!(dev->admin_sq_ctrl & UET_DEV_RING_CTRL_ENABLE) ||
	    !(dev->admin_cq_ctrl & UET_DEV_RING_CTRL_ENABLE) ||
	    (dev->admin_sq_entries == 0) || (dev->admin_cq_entries == 0))
		return;

	while (dev->admin_sq_cons != dev->admin_sq_prod) {
		/* Stop rather than overwrite when the driver has not
		 * reclaimed completions. The commands stay queued and are
		 * picked up on the next doorbell.
		 */
		if (!uet_dev_admin_cq_has_room(dev))
			break;

		/* read the command from the admin sq */
		slot = (dev->admin_sq_base +
			((uint64_t)(dev->admin_sq_cons %
				    dev->admin_sq_entries) *
			 sizeof(cmd)));

		if ((dev->backend.dma_read == NULL) ||
		    (dev->backend.dma_read(dev->backend.ctx, slot, &cmd,
					   sizeof(cmd)) != 0)) {
			dev->admin_errors++;
			dev->admin_sq_cons++;
			continue;
		}

		memset(&cqe, 0, sizeof(cqe));

		/* Must take the same lock the progress thread takes for
		 * datapath operations in the uet-ref-prov instance.
		 */
		pthread_mutex_lock(&dev->obj_lock);
		uet_dev_admin_execute(dev, &cmd, &cqe);
		pthread_mutex_unlock(&dev->obj_lock);

		/* write the completion to the admin cq */
		slot = (dev->admin_cq_base +
			((uint64_t)(dev->admin_cq_prod %
				    dev->admin_cq_entries) *
			 sizeof(cqe)));

		if ((dev->backend.dma_write == NULL) ||
		    (dev->backend.dma_write(dev->backend.ctx, slot, &cqe,
					    sizeof(cqe)) != 0)) {
			dev->admin_errors++;
			dev->admin_sq_cons++;
			continue;
		}

		if (cqe.status != UET_DEV_ADMIN_ST_OK)
			dev->admin_errors++;

		dev->admin_sq_cons++;
		dev->admin_cq_prod++;
		dev->admin_cmds++;
		ran = true;
	}

	if (ran && (dev->backend.raise_irq != NULL)) {
		dev->backend.raise_irq(dev->backend.ctx, UET_DEV_ADMIN_VECTOR);
		dev->irqs_fired++;
	}
}

