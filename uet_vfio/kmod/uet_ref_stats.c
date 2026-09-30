// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * The device's counters, readable from the guest.
 *
 * Everything numeric is published through ib_core's hardware statistics
 * mechanism, which puts one file per counter under:
 *
 *     ls /sys/class/infiniband/uet_ref0/hw_counters/
 *     rdma statistic show
 */

#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/sysfs.h>

#include <rdma/ib_verbs.h>

#include "uet_ref.h"

/* Where a value comes from. Most are BAR0 registers. BAR0_64 is the one
 * value the device reports as a pair (hi/lo). The device's limits are not
 * registers at all and they arrive once in the DEV_INFO admin response which
 * the driver cached.
 */
enum uet_ref_stat_src {
	UET_REF_STAT_BAR0,
	UET_REF_STAT_BAR0_64,
	UET_REF_STAT_CACHED,
};

/* which cached field, for UET_REF_STAT_CACHED */
enum uet_ref_stat_field {
	UET_REF_FIELD_MAX_JOBS,
	UET_REF_FIELD_MAX_JKEYS,
	UET_REF_FIELD_MAX_ADDR_ENTRIES,
	UET_REF_FIELD_MAX_IMM_SIZE,
	UET_REF_FIELD_CAPS,
};

struct uet_ref_stat_reg {
	const char *name;
	u32 off; /* BAR0 offset, or a field id */
	enum uet_ref_stat_src src;
};

/* ib_core needs an array of descriptions and this file needs an array of
 * sources. The two must stay in the same order for a value to land under
 * the right name.
 */
#define UET_REF_STAT_LIST(X)                                                  \
	/* configuration, as the instance was actually ran */                 \
	X("cfg_state",		UET_DEV_REG_STATE,		BAR0)         \
	X("cfg_pds_mode",	UET_DEV_REG_PDS_MODE,		BAR0)         \
	X("cfg_sec_mode",	UET_DEV_REG_SEC_MODE,		BAR0)         \
	X("cfg_nic_shim",	UET_DEV_REG_NIC_SHIM,		BAR0)         \
	X("cfg_max_payload",	UET_DEV_REG_MAX_PAYLOAD,	BAR0)         \
	X("cfg_port_proto",	UET_DEV_REG_PORT_PROTO,		BAR0)         \
	X("cfg_max_tx_retries",	UET_DEV_REG_MAX_TX_RETRIES,	BAR0)         \
	X("cfg_tx_timeout_ms",	UET_DEV_REG_TX_TIMEOUT,		BAR0)         \
	X("cfg_iov_limit",	UET_DEV_REG_IOV_LIMIT,		BAR0)         \
	X("cfg_ip_ver",		UET_DEV_REG_IP_VER,		BAR0)         \
	/* the device's own liveness */                                       \
	X("progress_alive",	UET_DEV_REG_PROGRESS_ALIVE,	BAR0)         \
	X("progress_iters",	UET_DEV_REG_PROGRESS_ITERS_LO,	BAR0_64)      \
	X("uptime_ms",		UET_DEV_REG_UPTIME_MS,		BAR0)         \
	/* what the guest has asked */                                        \
	X("bar0_reads",		UET_DEV_REG_BAR0_READS,		BAR0)         \
	X("bar0_writes",	UET_DEV_REG_BAR0_WRITES,	BAR0)         \
	X("doorbells",		UET_DEV_REG_DOORBELLS,		BAR0)         \
	X("irqs_fired",		UET_DEV_REG_IRQS_FIRED,		BAR0)         \
	/* the L2 rings */                                                    \
	X("tx_frames",		UET_DEV_REG_TX_FRAMES,		BAR0)         \
	X("tx_errors",		UET_DEV_REG_TX_ERRORS,		BAR0)         \
	X("rx_frames",		UET_DEV_REG_RX_FRAMES,		BAR0)         \
	X("rx_dropped",		UET_DEV_REG_RX_DROPPED,		BAR0)         \
	X("rx_uet_frames",	UET_DEV_REG_RX_UET_FRAMES,	BAR0)         \
	/* the admin queue */                                                 \
	X("admin_cmds",		UET_DEV_REG_ADMIN_CMDS,		BAR0)         \
	X("admin_errors",	UET_DEV_REG_ADMIN_ERRORS,	BAR0)         \
	/* live object counts, gauges rather than counters */                 \
	X("jobs_live",		UET_DEV_REG_JOBS_LIVE,		BAR0)         \
	X("jkeys_live",		UET_DEV_REG_JKEYS_LIVE,		BAR0)         \
	X("mrs_live",		UET_DEV_REG_MRS_LIVE,		BAR0)         \
	X("qps_live",		UET_DEV_REG_QPS_LIVE,		BAR0)         \
	X("cqs_live",		UET_DEV_REG_CQS_LIVE,		BAR0)         \
	X("qps_closing",	UET_DEV_REG_QPS_CLOSING,	BAR0)         \
	/* the datapath */                                                    \
	X("sq_posted",		UET_DEV_REG_SQ_POSTED,		BAR0)         \
	X("rq_posted",		UET_DEV_REG_RQ_POSTED,		BAR0)         \
	X("cq_posted",		UET_DEV_REG_CQ_POSTED,		BAR0)         \
	X("cq_overruns",	UET_DEV_REG_CQ_OVERRUNS,	BAR0)         \
	X("db_rejected",	UET_DEV_REG_DB_REJECTED,	BAR0)         \
	X("sq_errors",		UET_DEV_REG_SQ_ERRORS,		BAR0)         \
	X("rq_errors",		UET_DEV_REG_RQ_ERRORS,		BAR0)         \
	X("cq_errors",		UET_DEV_REG_CQ_ERRORS,		BAR0)         \
	X("flushed",		UET_DEV_REG_FLUSHED,		BAR0)         \
	/* device capabilities and limits */                                  \
	X("max_jobs",		UET_REF_FIELD_MAX_JOBS,		CACHED)       \
	X("max_jkeys",		UET_REF_FIELD_MAX_JKEYS,	CACHED)       \
	X("max_addr_entries",	UET_REF_FIELD_MAX_ADDR_ENTRIES,	CACHED)       \
	X("max_imm_size",	UET_REF_FIELD_MAX_IMM_SIZE,	CACHED)       \
	X("dev_caps",		UET_REF_FIELD_CAPS,		CACHED)

static const struct uet_ref_stat_reg uet_ref_stat_regs[] = {
#define X(nm, o, s) { .name = nm, .off = (o), .src = UET_REF_STAT_##s },
	UET_REF_STAT_LIST(X)
#undef X
};

/* ib_core keeps this pointer, so it has to outlive every stats struct */
static const struct rdma_stat_desc uet_ref_stat_descs[] = {
#define X(nm, o, s) { .name = nm },
	UET_REF_STAT_LIST(X)
#undef X
};

static u64 uet_ref_stat_cached(const struct uet_ref_dev *dev,
			       u32 field)
{
	switch (field) {
	case UET_REF_FIELD_MAX_JOBS:		return dev->max_job_ids;
	case UET_REF_FIELD_MAX_JKEYS:		return dev->max_job_keys;
	case UET_REF_FIELD_MAX_ADDR_ENTRIES:	return dev->max_addr_entries;
	case UET_REF_FIELD_MAX_IMM_SIZE:	return dev->max_imm_size;
	case UET_REF_FIELD_CAPS:		return dev->dev_caps;
	default:				return 0;
	}
}

static struct rdma_hw_stats *uet_ref_alloc_hw_device_stats(
	struct ib_device *ibdev)
{
	return rdma_alloc_hw_stats_struct(uet_ref_stat_descs,
					  ARRAY_SIZE(uet_ref_stat_descs),
					  RDMA_HW_STATS_DEFAULT_LIFESPAN);
}

/* read every counter as a snapshot, not just the one asked for */
static int uet_ref_get_hw_stats(struct ib_device *ibdev,
				struct rdma_hw_stats *stats,
				u32 port,
				int index)
{
	struct uet_ref_dev *dev = container_of(ibdev, struct uet_ref_dev,
					       ib_dev);
	const struct uet_ref_stat_reg *r;
	size_t i;
	u64 v;

	/* device-level statistics: ib_core passes port 0 */
	if (port)
		return 0;

	for (i = 0; i < ARRAY_SIZE(uet_ref_stat_regs); i++) {
		r = &uet_ref_stat_regs[i];

		switch (r->src) {
		case UET_REF_STAT_BAR0_64:
			v = uet_ref_rd0(dev, r->off);
			v |= ((u64)uet_ref_rd0(dev, r->off + 4) << 32);
			break;
		case UET_REF_STAT_CACHED:
			v = uet_ref_stat_cached(dev, r->off);
			break;
		default:
			v = uet_ref_rd0(dev, r->off);
			break;
		}

		stats->value[i] = v;
	}

	return ARRAY_SIZE(uet_ref_stat_regs);
}

const struct ib_device_ops uet_ref_stats_ops = {
	.alloc_hw_device_stats = uet_ref_alloc_hw_device_stats,
	.get_hw_stats = uet_ref_get_hw_stats,
};

static ssize_t mac_show(struct device *d,
			struct device_attribute *a,
			char *buf)
{
	struct uet_ref_dev *dev = dev_get_drvdata(d);
	u8 mac[ETH_ALEN];

	memcpy_fromio(mac, (dev->bar0 + UET_DEV_REG_MAC_LO), ETH_ALEN);

	return sysfs_emit(buf, "%pM\n", mac);
}
static DEVICE_ATTR_RO(mac);

static ssize_t ip_addr_show(struct device *d,
			    struct device_attribute *a,
			    char *buf)
{
	struct uet_ref_dev *dev = dev_get_drvdata(d);
	u8 ip[UET_DEV_IPADDR_LEN];

	memcpy_fromio(ip, (dev->bar0 + UET_DEV_REG_IPADDR), sizeof(ip));

	if (uet_ref_rd0(dev, UET_DEV_REG_IP_VER) == 6)
		return sysfs_emit(buf, "%pI6\n", ip);

	return sysfs_emit(buf, "%pI4\n", ip);
}
static DEVICE_ATTR_RO(ip_addr);

static ssize_t ifname_show(struct device *d,
			   struct device_attribute *a,
			   char *buf)
{
	struct uet_ref_dev *dev = dev_get_drvdata(d);
	char name[UET_DEV_IFNAME_LEN + 1];

	memcpy_fromio(name, (dev->bar0 + UET_DEV_REG_IFNAME),
		      UET_DEV_IFNAME_LEN);
	name[UET_DEV_IFNAME_LEN] = '\0';

	/* the interface the device model itself is bound to, on the host */
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(ifname);

static struct attribute *uet_ref_dev_attrs[] = {
	&dev_attr_mac.attr,
	&dev_attr_ip_addr.attr,
	&dev_attr_ifname.attr,
	NULL,
};

static const struct attribute_group uet_ref_dev_group = {
	.name = "uet",
	.attrs = uet_ref_dev_attrs,
};

int uet_ref_stats_add_dev_attrs(struct uet_ref_dev *dev)
{
	return devm_device_add_group(&dev->pdev->dev, &uet_ref_dev_group);
}

