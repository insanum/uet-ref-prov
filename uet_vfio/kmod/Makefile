# SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

# Copyright (c) 2026, Broadcom. All rights reserved. The term Broadcom
# refers to Broadcom Inc. and/or its subsidiaries.
#
# Out-of-tree build of the UET reference device driver. Built against a
# kernel carrying the Ultra Ethernet verbs - this driver implements twelve
# ib_device_ops members that a released kernel does not have, so a stock
# tree will not compile it:
#
#     make -C uet_vfio/kmod KERNELDIR=$HOME/rdma-next
#     sudo insmod uet_ref.ko
#     echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers_probe
#
# KERNELDIR defaults to the running kernel's build tree, which is right only
# when the guest runs the patched kernel and that tree is present. ib_core
# and ib_uverbs are built into it, so there is nothing to modprobe. The write
# to drivers_probe re-runs the PCI match, which loading the module does not.
#
# The tree layout matters: uet_dev_abi.h is found one directory up, with the
# device model it describes. Copy uet_vfio/ whole rather than the kmod alone.

obj-m += uet_ref.o

uet_ref-objs := \
        uet_ref_main.o \
        uet_ref_netdev.o \
        uet_ref_verbs.o \
        uet_ref_admin.o \
        uet_ref_job.o \
        uet_ref_mr.o \
        uet_ref_ring.o \
        uet_ref_qp.o \
        uet_ref_cq.o \
        uet_ref_ah.o \
        uet_ref_stats.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

# The device ABI header lives one directory up, with the device model it
# describes, so there is nothing to copy. uet_ref-abi.h (this driver's
# private kernel/user ABI) is authoritative here, and rdma-core keeps a
# mirror the way it does for every other driver's uapi header.
ccflags-y += -I$(src) -I$(src)/..

.PHONY: all clean
