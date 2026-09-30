// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * The admin queue (slowpath resource management).
 *
 * Consists of a command ring and a separate completion ring. The driver
 * produces commands and consumes completions, the device does the reverse.
 * Neither side writes a ring the other owns.
 *
 * Commands are serialized under one mutex, across all processes. This means
 * one command is outstanding at a time, while the intent it's not a
 * guarantee. A command that times out may still be answered later, and a
 * device that stalls may still hold commands this driver has given up on.
 * Completions are matched by command cookie so when reading completions,
 * anything else waiting in the ring is dropped as a late answer, and a
 * command is not posted until the device has finished every earlier one.
 *
 * Results of one or two words come back in the completion entry. Anything
 * larger is written by the device into a coherent buffer the driver owns
 * and names in the command (shared buffer though safe to access because of
 * the command serialiaation).
 */

#include "uet_ref.h"

/* generous timeout as this path is never hot, stuck device = obvious */
#define UET_REF_ADMIN_TIMEOUT_MS 5000

int uet_ref_admin_init(struct uet_ref_dev *dev)
{
	struct device *d = &dev->pdev->dev;

	mutex_init(&dev->admin_lock);
	init_completion(&dev->admin_done);

	dev->admin_sq = dma_alloc_coherent(d, UET_REF_ADMIN_SQ_BYTES,
					   &dev->admin_sq_dma, GFP_KERNEL);
	if (!dev->admin_sq)
		return -ENOMEM;

	dev->admin_cq = dma_alloc_coherent(d, UET_REF_ADMIN_CQ_BYTES,
					   &dev->admin_cq_dma, GFP_KERNEL);
	if (!dev->admin_cq)
		goto err_sq;

	dev->admin_resp = dma_alloc_coherent(d, UET_REF_ADMIN_RESP_BYTES,
					     &dev->admin_resp_dma, GFP_KERNEL);
	if (!dev->admin_resp)
		goto err_cq;

	dev->admin_sq_prod = 0;
	dev->admin_cq_cons = 0;
	dev->admin_cookie = 0;

	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_CTRL, 0);
	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_CTRL, 0);

	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_BASE_LO,
		    lower_32_bits(dev->admin_sq_dma));
	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_BASE_HI,
		    upper_32_bits(dev->admin_sq_dma));
	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_ENTRIES, UET_REF_ADMIN_ENTRIES);

	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_BASE_LO,
		    lower_32_bits(dev->admin_cq_dma));
	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_BASE_HI,
		    upper_32_bits(dev->admin_cq_dma));
	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_ENTRIES, UET_REF_ADMIN_ENTRIES);

	/* enabled the rings (completion ring first) */
	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_CTRL, UET_DEV_RING_CTRL_ENABLE);
	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_CTRL, UET_DEV_RING_CTRL_ENABLE);

	return 0;

err_cq:
	dma_free_coherent(d, UET_REF_ADMIN_CQ_BYTES, dev->admin_cq,
			  dev->admin_cq_dma);
	dev->admin_cq = NULL;
err_sq:
	dma_free_coherent(d, UET_REF_ADMIN_SQ_BYTES, dev->admin_sq,
			  dev->admin_sq_dma);
	dev->admin_sq = NULL;

	return -ENOMEM;
}

void uet_ref_admin_fini(struct uet_ref_dev *dev)
{
	struct device *d = &dev->pdev->dev;

	if (!dev->admin_sq)
		return;

	/* disable the rings (admin send queue first) */
	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_CTRL, 0);
	uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_CTRL, 0);

	dma_free_coherent(d, UET_REF_ADMIN_RESP_BYTES, dev->admin_resp,
			  dev->admin_resp_dma);
	dma_free_coherent(d, UET_REF_ADMIN_CQ_BYTES, dev->admin_cq,
			  dev->admin_cq_dma);
	dma_free_coherent(d, UET_REF_ADMIN_SQ_BYTES, dev->admin_sq,
			  dev->admin_sq_dma);

	dev->admin_resp = NULL;
	dev->admin_cq = NULL;
	dev->admin_sq = NULL;
}

irqreturn_t uet_ref_admin_isr(int irq,
			      void *data)
{
	struct uet_ref_dev *dev = data;

	complete(&dev->admin_done);

	return IRQ_HANDLED;
}

static int uet_ref_admin_status_to_errno(u32 status)
{
	switch (status) {
	case UET_DEV_ADMIN_ST_OK:
		return 0;
	case UET_DEV_ADMIN_ST_ENOSPC:
		return -ENOSPC;
	case UET_DEV_ADMIN_ST_EOPNOTSUPP:
		return -EOPNOTSUPP;
	case UET_DEV_ADMIN_ST_EBUSY:
		return -EBUSY;
	case UET_DEV_ADMIN_ST_ETOOSMALL:
		return -EMSGSIZE;
	case UET_DEV_ADMIN_ST_EEXIST:
		return -EEXIST;
	case UET_DEV_ADMIN_ST_ENOENT:
		return -ENOENT;
	case UET_DEV_ADMIN_ST_EINVAL:
	default:
		return -EINVAL;
	}
}

/* Consume completions in ring order, up to and including the one carrying
 * the 'cookie' (which is copied to 'out'). Any other valid entry ahead of
 * it is the late answer to a command this driver already gave up on. It is
 * logged and dropped. A 'cookie' of 0, which no command carries, drops every
 * entry waiting. Returns true if the 'cookie' completion was found.
 */
static bool uet_ref_admin_reap(struct uet_ref_dev *dev,
			       u64 cookie,
			       struct uet_dev_admin_cqe *out)
{
	struct uet_dev_admin_cqe *cqe;
	bool found = false;
	bool reaped = false;

	while (!found) {
		cqe = &dev->admin_cq[dev->admin_cq_cons % UET_REF_ADMIN_ENTRIES];

		if (!(le16_to_cpu(READ_ONCE(cqe->flags)) &
		      UET_DEV_ADMIN_CQE_VALID))
			break;

		dma_rmb(); /* ensure the entry is ready to read */

		if (cookie && (le64_to_cpu(cqe->cookie) == cookie)) {
			*out = *cqe;
			found = true;
		} else {
			dev_warn(&dev->pdev->dev,
				 "admin: dropped a late completion, "
				 "opcode 0x%04x cookie %llu\n",
				 le16_to_cpu(cqe->opcode),
				 le64_to_cpu(cqe->cookie));
		}

		WRITE_ONCE(cqe->flags, 0);

		dev->admin_cq_cons++;
		reaped = true;
	}

	if (reaped) {
		/* The updated entries (cleared flags) must reach memory
		 * before the doorbell updating the consumer index that tells
		 * the device those slots are free.
		 */
		wmb();
		uet_ref_wr2(dev, UET_DEV_ADMIN_CQ_CONS, dev->admin_cq_cons);
	}

	return found;
}

/* Issue one admin command and wait for its completion. 'params' are copied
 * into the command structure and if a 'resp' buffer is given it is set as the
 * 'buf_addr' in the command. This buffer is bidirectional in that addtional
 * command data (beyond 'params') is placed there and read by the device.
 * Likewise, reponse data (beyond just a status) is written there by the
 * device.
 */
int uet_ref_admin_cmd(struct uet_ref_dev *dev,
		      u16 opcode,
		      const u64 *params,
		      unsigned int nparams,
		      void *resp,
		      size_t resp_len,
		      u64 *result)
{
	struct uet_dev_admin_cmd *cmd;
	struct uet_dev_admin_cqe cqe;
	unsigned long deadline, left;
	unsigned int i;
	u64 cookie;
	int ret = 0;

	if ((nparams > UET_REF_ADMIN_MAX_PARAMS) ||
	    (resp_len > UET_REF_ADMIN_RESP_BYTES))
		return -EINVAL;

	if (!dev->admin_sq)
		return -ENODEV;

	mutex_lock(&dev->admin_lock);

	/* drop answers to commands that timed out */
	uet_ref_admin_reap(dev, 0, NULL);

	/* The device advances ADMIN_SQ_CONS only once a command has run and
	 * its completion is written, so anything short of our producer means
	 * it still holds an earlier command. Posting now would risk
	 * overwriting that previous command's response buffer.
	 */
	if (uet_ref_rd0(dev, UET_DEV_REG_ADMIN_SQ_CONS) != dev->admin_sq_prod) {
		dev_err_ratelimited(&dev->pdev->dev,
				    "admin: device has not finished an "
				    "earlier command\n");
		ret = -EIO;
		goto out;
	}

	cookie = ++dev->admin_cookie;

	cmd = &dev->admin_sq[dev->admin_sq_prod % UET_REF_ADMIN_ENTRIES];
	memset(cmd, 0, sizeof(*cmd));

	cmd->opcode = cpu_to_le16(opcode);
	cmd->cookie = cpu_to_le64(cookie);

	for (i = 0; i < nparams; i++)
		cmd->param[i] = cpu_to_le64(params[i]);

	if (resp && resp_len) {
		memcpy(dev->admin_resp, resp, resp_len);
		cmd->buf_addr = cpu_to_le64(dev->admin_resp_dma);
		cmd->buf_len  = cpu_to_le32(resp_len);
	}

	reinit_completion(&dev->admin_done);

	dev->admin_sq_prod++;

	wmb(); /* ensure command visible before hitting the doorebell */
	uet_ref_wr2(dev, UET_DEV_ADMIN_SQ_PROD, dev->admin_sq_prod);

	/* an interrupt is a hint to look, not proof of an answer */
	deadline = (jiffies + msecs_to_jiffies(UET_REF_ADMIN_TIMEOUT_MS));

	while (!uet_ref_admin_reap(dev, cookie, &cqe)) {
		left = time_before(jiffies, deadline)
			? (deadline - jiffies) : 0;

		if (left && wait_for_completion_timeout(&dev->admin_done, left))
			continue;

		/* the entry is in memory even when its interrupt is lost */
		if (uet_ref_admin_reap(dev, cookie, &cqe)) {
			dev_warn(&dev->pdev->dev,
				 "admin opcode 0x%04x completed without "
				 "an interrupt\n",
				 opcode);
			break;
		}

		dev_err(&dev->pdev->dev, "admin opcode 0x%04x timed out\n",
			opcode);

		ret = -ETIMEDOUT;
		goto out;
	}

	ret = uet_ref_admin_status_to_errno(le32_to_cpu(cqe.status));

	if (result) {
		result[0] = le64_to_cpu(cqe.result[0]);
		result[1] = le64_to_cpu(cqe.result[1]);
	}

	if (!ret && resp && resp_len)
		memcpy(resp, dev->admin_resp, resp_len);

out:
	mutex_unlock(&dev->admin_lock);

	return ret;
}

/* query device caps/limits and cache in device state */
int uet_ref_admin_query_dev_info(struct uet_ref_dev *dev)
{
	struct uet_dev_admin_dev_info info;
	int ret;

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_DEV_INFO, NULL, 0, &info,
				sizeof(info), NULL);
	if (ret)
		return ret;

	dev->dev_caps         = le64_to_cpu(info.caps);
	dev->max_job_ids      = le32_to_cpu(info.max_job_ids);
	dev->max_job_keys     = le32_to_cpu(info.max_job_keys);
	dev->max_addr_entries = le32_to_cpu(info.max_addr_entries);
	dev->max_imm_size     = le32_to_cpu(info.max_imm_size);

	dev_info(&dev->pdev->dev,
		 "device info: jobs %u jkeys %u addrs %u imm %u\n",
		 dev->max_job_ids, dev->max_job_keys, dev->max_addr_entries,
		 dev->max_imm_size);

	return 0;
}

