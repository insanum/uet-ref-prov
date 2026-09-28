/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - the object model
 */

#ifndef _UET_DEV_OBJS_H_
#define _UET_DEV_OBJS_H_

#include "uet_dev_priv.h"

/* jobs, job keys, and address tables */
struct uet_dev_job *uet_dev_job_get(struct uet_dev *dev, uint64_t handle);
uint32_t uet_dev_job_alloc(struct uet_dev *dev, uint32_t job_id,
			   uint32_t max_addrs, uint8_t port_num,
			   uint8_t sgid_index, uint32_t flags,
			   uint32_t *status);
uint32_t uet_dev_job_free(struct uet_dev *dev, uint64_t handle);
bool uet_dev_job_addrs_release(struct uet_dev_job *job);
struct uet_dev_addr_entry *uet_dev_addr_slot_job(struct uet_dev_job *job,
						 uint64_t index);
struct uet_dev_addr_entry *uet_dev_addr_slot(struct uet_dev *dev,
					     uint64_t job_handle,
					     uint64_t index);
uet_addr_handle_t uet_dev_addr_av(struct uet_dev *dev,
				  struct uet_dev_addr_entry *slot);
uint32_t uet_dev_jkey_value(uint32_t handle);
struct uet_dev_job *uet_dev_job_by_jkey(struct uet_dev *dev,
					uint32_t jkey, uint32_t pdn);
uint32_t uet_dev_jkey_create(struct uet_dev *dev, uint64_t job_handle,
			     uint32_t pdn, uint32_t flags,
			     uint32_t *out_handle, uint32_t *out_jkey);
uint32_t uet_dev_jkey_destroy(struct uet_dev *dev, uint64_t handle);

/* address handles */
struct uet_dev_ah *uet_dev_ah_get(struct uet_dev *dev, uint64_t handle);
uet_addr_handle_t uet_dev_ah_av(struct uet_dev *dev, struct uet_dev_ah *ah);
uint32_t uet_dev_ah_create(struct uet_dev *dev, uint32_t pd,
			   struct uet_dev_admin_addr *addr,
			   uint32_t *out_handle);
uint32_t uet_dev_ah_destroy(struct uet_dev *dev, uint64_t handle);

/* completion queues */
struct uet_dev_cq *uet_dev_cq_get(struct uet_dev *dev, uint64_t handle);
uint32_t uet_dev_cq_create(struct uet_dev *dev,
			   struct uet_dev_admin_cq_create *args,
			   uint32_t *out_handle);
uint32_t uet_dev_cq_destroy(struct uet_dev *dev, uint64_t handle);

/* queue pairs */
struct uet_dev_qp *uet_dev_qp_get(struct uet_dev *dev, uint64_t handle);
uint32_t uet_dev_qp_create(struct uet_dev *dev,
			   struct uet_dev_admin_qp_create *args,
			   uint32_t *out_handle);
uint32_t uet_dev_qp_destroy(struct uet_dev *dev, uint64_t handle);
uint32_t uet_dev_qp_modify(struct uet_dev *dev, uint64_t handle,
			   uint32_t next_state);
void uet_dev_qp_retire(struct uet_dev *dev, struct uet_dev_qp *qp);
uint32_t uet_dev_qp_job_id(struct uet_dev *dev, struct uet_dev_qp *qp);

/* memory regions */
struct uet_dev_mr *uet_dev_mr_by_key(struct uet_dev *dev, uint64_t key);
uint32_t uet_dev_mr_reg(struct uet_dev *dev,
			struct uet_dev_admin_mr_reg *args,
			uint64_t pbl_root, uint32_t page_size,
			uint32_t pbl_level, uint64_t base_va, uint64_t len,
			uint32_t *out_handle, uint64_t *out_key);
uint32_t uet_dev_mr_dereg(struct uet_dev *dev, uint64_t handle);
uint32_t uet_dev_mr_attach(struct uet_dev *dev, uint64_t mr_handle,
			   uint64_t qp_handle);
uint32_t uet_dev_mr_detach(struct uet_dev *dev, uint64_t mr_handle);

#endif /* _UET_DEV_OBJS_H_ */

