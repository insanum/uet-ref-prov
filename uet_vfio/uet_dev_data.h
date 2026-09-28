/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - the datapath
 *
 * Driven by the progress thread. Completions are also drained on behalf of
 * a queue pair the object model is retiring.
 */

#ifndef _UET_DEV_DATA_H_
#define _UET_DEV_DATA_H_

#include "uet_dev_priv.h"

void uet_dev_cq_drain(struct uet_dev *dev, struct uet_dev_qp *qp,
		      uet_cq_handle_t src);
void uet_dev_datapath_poll(struct uet_dev *dev);

#endif /* _UET_DEV_DATA_H_ */

