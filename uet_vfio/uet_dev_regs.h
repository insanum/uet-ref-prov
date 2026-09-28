/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - configuration behind the registers
 */

#ifndef _UET_DEV_REGS_H_
#define _UET_DEV_REGS_H_

#include "uet_dev_priv.h"

void uet_dev_read_config(struct uet_dev *dev);
void uet_dev_read_identity(struct uet_dev *dev);

#endif /* _UET_DEV_REGS_H_ */

