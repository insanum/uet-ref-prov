// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * The RDMA half of the driver: an ib_device associated with the netdev the
 * L2 half registered.
 */

#include <linux/inetdevice.h>
#include <net/addrconf.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_user_verbs.h>

#include "uet_ref.h"
#include "uet_ref-abi.h"

static int uet_ref_query_device(struct ib_device *ibdev,
				struct ib_device_attr *attr,
				struct ib_udata *udata)
{
	struct uet_ref_dev *dev = container_of(ibdev, struct uet_ref_dev,
					       ib_dev);
	u32 devcaps;
	int ret;

	/* There is no udata on the registration path. ib_core queries the
	 * device attributes from ib_register_device(), before any user
	 * context exists, and passes NULL. ib_no_udata_io() takes that, and
	 * zero-fills a response buffer if one is there.
	 */
	ret = ib_no_udata_io(udata);
	if (ret)
		return ret;

	memset(attr, 0, sizeof(*attr));

	attr->vendor_id        = UET_DEV_PCI_VENDOR_ID;
	attr->vendor_part_id   = UET_DEV_PCI_DEVICE_ID;
	attr->hw_ver           = uet_ref_rd0(dev, UET_DEV_REG_ABI_VERSION);
	attr->fw_ver           = 0;
	attr->max_mr_size      = ~0ULL;
	attr->page_size_cap    = PAGE_SIZE;
	attr->max_qp           = UET_DEV_MAX_QPS;
	attr->max_qp_wr        = 1024;
	attr->max_cq           = UET_DEV_MAX_CQS;
	attr->max_cqe          = 4096;
	attr->max_mr           = 1024;
	attr->max_pd           = UET_REF_MAX_PDS;
	attr->max_send_sge     = UET_DEV_MAX_SGE;
	attr->max_recv_sge     = UET_DEV_MAX_SGE;
	attr->max_sge_rd       = UET_DEV_MAX_SGE;
	attr->atomic_cap       = IB_ATOMIC_HCA;
	attr->max_job_ids      = dev->max_job_ids;
	attr->max_job_keys     = dev->max_job_keys;
	attr->max_addr_entries = dev->max_addr_entries;
	attr->device_cap_flags = (IB_DEVICE_MEM_MGT_EXTENSIONS |
				  IB_DEVICE_IMM64 |
				  IB_DEVICE_KEY64 |
				  IB_DEVICE_USER_RKEY);

	devcaps = uet_ref_rd0(dev, UET_DEV_REG_CAPS);
	if (devcaps & UET_DEV_CAP_INSTANCE)
		attr->device_cap_flags |= IB_DEVICE_RU;

	/* Which access restrictions a region may be registered with. Taken
	 * from the running device's own registers rather than from anything
	 * compiled in here.
	 */
	if (devcaps & UET_DEV_CAP_MR_UNRESTRICTED)
		attr->device_cap_flags |= IB_DEVICE_MR_UNRESTRICTED;
	if (devcaps & UET_DEV_CAP_MR_JOB_RESTRICTED)
		attr->device_cap_flags |= IB_DEVICE_MR_JOB_RESTRICTED;
	if (devcaps & UET_DEV_CAP_MR_RI_RESTRICTED)
		attr->device_cap_flags |= IB_DEVICE_MR_QP_RESTRICTED;
	if (devcaps & UET_DEV_CAP_MR_RI_JOB_RESTRICTED)
		attr->device_cap_flags |= IB_DEVICE_MR_QP_JOB_RESTRICTED;

	return 0;
}

static int uet_ref_query_port(struct ib_device *ibdev,
			      u32 port_num,
			      struct ib_port_attr *attr)
{
	struct uet_ref_dev *dev = container_of(ibdev, struct uet_ref_dev,
					       ib_dev);
	struct net_device *ndev = dev->netdev;

	if (port_num != 1)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));

	attr->gid_tbl_len    = 128;
	attr->pkey_tbl_len   = 1;
	attr->max_mtu        = ib_mtu_int_to_enum(
					uet_ref_rd0(dev,
						    UET_DEV_REG_MAX_PAYLOAD));
	attr->active_mtu     = attr->max_mtu;
	attr->max_msg_sz     = uet_ref_rd0(dev, UET_DEV_REG_MAX_MSG_SIZE);
	attr->port_cap_flags = IB_UVERBS_PCF_CM_SUP;

	if (ndev && netif_running(ndev) && netif_carrier_ok(ndev)) {
		attr->state = IB_PORT_ACTIVE;
		attr->phys_state = IB_PORT_PHYS_STATE_LINK_UP;
	} else {
		attr->state = IB_PORT_DOWN;
		attr->phys_state = IB_PORT_PHYS_STATE_DISABLED;
	}

	if (ndev)
		ib_get_eth_speed(ibdev, port_num, &attr->active_speed,
				 &attr->active_width);

	return 0;
}

static int uet_ref_port_immutable(struct ib_device *ibdev,
				  u32 port_num,
				  struct ib_port_immutable *immutable)
{
	struct uet_ref_dev *dev = container_of(ibdev, struct uet_ref_dev,
					       ib_dev);
	struct ib_port_attr attr = {};
	int err;

	if (port_num != 1)
		return -EINVAL;

	err = ib_query_port(ibdev, port_num, &attr);
	if (err)
		return err;

	/* UET names its encapsulation by GID type, and an application selects
	 * the transport protocol by selecting a GID entry. The port capability
	 * is how ib_core knows which type of GID to derive from our netdev.
	 */
	switch (uet_ref_rd0(dev, UET_DEV_REG_GID_TYPE)) {
	case UET_DEV_GID_TYPE_UDP:
		immutable->core_cap_flags = RDMA_CORE_PORT_UET_UDP;
		break;
	case UET_DEV_GID_TYPE_UFH:
		immutable->core_cap_flags = RDMA_CORE_PORT_UET_UFH;
		break;
	case UET_DEV_GID_TYPE_IP:
	default:
		immutable->core_cap_flags = RDMA_CORE_PORT_UET_IP;
		break;
	}

	immutable->pkey_tbl_len   = attr.pkey_tbl_len;
	immutable->gid_tbl_len    = attr.gid_tbl_len;
	immutable->max_mad_size   = 0;

	return 0;
}

static enum rdma_link_layer uet_ref_get_link_layer(struct ib_device *ibdev,
						   u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int uet_ref_query_pkey(struct ib_device *ibdev,
			      u32 port_num,
			      u16 index,
			      u16 *pkey)
{
	if (index > 0)
		return -EINVAL;

	*pkey = 0xffff;	/* full-membership default pkey */

	return 0;
}

static int uet_ref_query_gid(struct ib_device *ibdev,
			     u32 port_num,
			     int index,
			     union ib_gid *gid)
{
	/* ib_core owns the RoCE GID cache, populated from our netdev */
	return -EINVAL;
}

/* The doorbell page is device memory, so it is exposed through the rdma
 * mmap machinery rather than by handing out a physical address. ib_core
 * hands the process an opaque offset, checks on mmap() that the offset
 * belongs to this context, and removes the mapping when the context dies.
 */
struct uet_ref_db_mmap {
	struct rdma_user_mmap_entry rdma_entry;
	phys_addr_t address;
};

static void uet_ref_mmap_free(struct rdma_user_mmap_entry *entry)
{
	kfree(container_of(entry, struct uet_ref_db_mmap, rdma_entry));
}

static int uet_ref_mmap(struct ib_ucontext *uctx,
			struct vm_area_struct *vma)
{
	struct rdma_user_mmap_entry *rdma_entry;
	struct uet_ref_db_mmap *db;
	int ret;

	rdma_entry = rdma_user_mmap_entry_get(uctx, vma);
	if (!rdma_entry)
		return -EINVAL;

	db = container_of(rdma_entry, struct uet_ref_db_mmap, rdma_entry);

	/* device memory: uncached, and never speculatively read */
	ret = rdma_user_mmap_io(uctx, vma, db->address >> PAGE_SHIFT,
				PAGE_SIZE, pgprot_noncached(vma->vm_page_prot),
				rdma_entry);

	rdma_user_mmap_entry_put(rdma_entry);

	return ret;
}

static int uet_ref_alloc_ucontext(struct ib_ucontext *uctx,
				  struct ib_udata *udata)
{
	struct uet_ref_ucontext *ctx = container_of(uctx,
						    struct uet_ref_ucontext,
						    ibucontext);
	struct uet_ref_dev *dev = container_of(uctx->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_ib_alloc_ucontext_resp resp = {};
	struct uet_ref_db_mmap *db;
	int page, ret;

	page = ida_alloc_max(&dev->db_ida, UET_DEV_MAX_DB_PAGES - 1,
			     GFP_KERNEL);
	if (page < 0)
		return -ENOSPC;

	db = kzalloc(sizeof(*db), GFP_KERNEL);
	if (!db) {
		ret = -ENOMEM;
		goto err_ida;
	}

	db->address = (dev->bar3_phys +
		       ((phys_addr_t)page * UET_DEV_DB_PAGE_SIZE));

	ret = rdma_user_mmap_entry_insert(uctx, &db->rdma_entry, PAGE_SIZE);
	if (ret) {
		kfree(db);
		goto err_ida;
	}

	ctx->db_page  = page;
	ctx->db_entry = &db->rdma_entry;

	resp.db_mmap_offset = rdma_user_mmap_get_offset(&db->rdma_entry);
	resp.db_page        = page;
	resp.max_sge        = UET_DEV_MAX_SGE;
	resp.wqe_size       = UET_DEV_WQE_SIZE;
	resp.cqe_size       = UET_DEV_CQE_SIZE;

	ret = ib_copy_to_udata(udata, &resp,
			       min(sizeof(resp), udata->outlen));
	if (ret)
		goto err_entry;

	return 0;

err_entry:
	rdma_user_mmap_entry_remove(&db->rdma_entry);
	ctx->db_entry = NULL;
err_ida:
	ida_free(&dev->db_ida, page);
	return ret;
}

static void uet_ref_dealloc_ucontext(struct ib_ucontext *uctx)
{
	struct uet_ref_ucontext *ctx = container_of(uctx,
						    struct uet_ref_ucontext,
						    ibucontext);
	struct uet_ref_dev *dev = container_of(uctx->device,
					       struct uet_ref_dev, ib_dev);

	if (ctx->db_entry) {
		rdma_user_mmap_entry_remove(ctx->db_entry);
		ctx->db_entry = NULL;
	}

	ida_free(&dev->db_ida, ctx->db_page);
}

static int uet_ref_alloc_pd(struct ib_pd *ibpd,
			    struct ib_udata *udata)
{
	struct uet_ref_pd *pd = container_of(ibpd, struct uet_ref_pd, ibpd);
	struct uet_ref_dev *dev = container_of(ibpd->device,
					       struct uet_ref_dev, ib_dev);
	int id;

	id = ida_alloc_max(&dev->pd_ida, UET_REF_MAX_PDS - 1, GFP_KERNEL);
	if (id < 0)
		return id;

	ida_init(&pd->ri_ida);

	pd->pdn              = (u32)id;
	pd->pidonfep         = 0;
	pd->pidonfep_latched = false;

	return 0;
}

static int uet_ref_dealloc_pd(struct ib_pd *ibpd,
			      struct ib_udata *udata)
{
	struct uet_ref_pd *pd = container_of(ibpd, struct uet_ref_pd, ibpd);
	struct uet_ref_dev *dev = container_of(ibpd->device,
					       struct uet_ref_dev, ib_dev);

	ida_destroy(&pd->ri_ida);

	if (pd->pidonfep_latched)
		ida_free(&dev->pidonfep_ida, pd->pidonfep);

	ida_free(&dev->pd_ida, pd->pdn);

	return 0;
}

/* What a queue pair of the named type guarantees over this device.
 *
 * No ordering is guaranteed between operations. This implementation
 * chooses a packet delivery mode per message rather than per queue pair,
 * so two operations on one pair may travel by reliable-ordered and
 * reliable-unordered delivery respectively. There is therefore no ordering
 * that holds for every pair of operations, and reporting the strongest
 * mode's guarantees would be reporting something an application cannot
 * rely on. An application that needs ordering must wait for the earlier
 * completion.
 */
static int uet_ref_query_qp_semantics(struct ib_device *ibdev,
				      enum ib_qp_type qp_type,
				      struct ib_qp_semantics *sem)
{
	struct uet_ref_dev *dev = container_of(ibdev, struct uet_ref_dev,
					       ib_dev);

	if (qp_type != IB_QPT_RU)
		return -EOPNOTSUPP;

	sem->comp_mask         = (IB_UVERBS_QP_SEMANTICS_MASK_MSG_ORDER |
				  IB_UVERBS_QP_SEMANTICS_MASK_RAW |
				  IB_UVERBS_QP_SEMANTICS_MASK_WAR |
				  IB_UVERBS_QP_SEMANTICS_MASK_WAW |
				  IB_UVERBS_QP_SEMANTICS_MASK_PDU |
				  IB_UVERBS_QP_SEMANTICS_MASK_IMM |
				  IB_UVERBS_QP_SEMANTICS_MASK_USAGE);
	sem->msg_order         = 0;
	sem->max_rdma_raw_size = 0;
	sem->max_rdma_war_size = 0;
	sem->max_rdma_waw_size = 0;
	sem->max_pdu           = uet_ref_rd0(dev, UET_DEV_REG_MAX_PAYLOAD);
	sem->imm_data_size     = 64;
	sem->usage_flags       = IB_UVERBS_QP_USAGE_ATTACH_MR;

	return 0;
}

static const struct ib_device_ops uet_ref_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_UET_REF,
	.uverbs_abi_ver = UET_REF_UVERBS_ABI_VERSION,

	.alloc_pd = uet_ref_alloc_pd,
	.dealloc_pd = uet_ref_dealloc_pd,
	.alloc_ucontext = uet_ref_alloc_ucontext,
	.mmap = uet_ref_mmap,
	.mmap_free = uet_ref_mmap_free,
	.dealloc_ucontext = uet_ref_dealloc_ucontext,
	.get_link_layer = uet_ref_get_link_layer,
	.get_port_immutable = uet_ref_port_immutable,
	.query_device = uet_ref_query_device,
	.query_gid = uet_ref_query_gid,
	.query_pkey = uet_ref_query_pkey,
	.query_port = uet_ref_query_port,
	.reg_user_mr = uet_ref_reg_user_mr,
	.reg_user_mr_dmabuf = uet_ref_reg_user_mr_dmabuf,
	.reg_user_mr_ex = uet_ref_reg_user_mr_ex,
	.qp_attach_mr = uet_ref_qp_attach_mr,
	.qp_detach_mr = uet_ref_qp_detach_mr,
	.query_qp_semantics = uet_ref_query_qp_semantics,
	.dereg_mr = uet_ref_dereg_mr,
	.create_user_ah = uet_ref_create_ah,
	.destroy_ah = uet_ref_destroy_ah,
	.create_cq = uet_ref_create_cq,
	.destroy_cq = uet_ref_destroy_cq,
	.create_qp = uet_ref_create_qp,
	.destroy_qp = uet_ref_destroy_qp,
	.modify_qp = uet_ref_modify_qp,
	.query_qp = uet_ref_query_qp,
	.alloc_job = uet_ref_alloc_job,
	.dealloc_job = uet_ref_dealloc_job,
	.query_job = uet_ref_query_job,
	.create_jkey = uet_ref_create_jkey,
	.destroy_jkey = uet_ref_destroy_jkey,
	.job_addr_insert = uet_ref_job_addr_insert,
	.job_addr_remove = uet_ref_job_addr_remove,
	.job_addr_query = uet_ref_job_addr_query,

	INIT_RDMA_OBJ_SIZE(ib_pd, uet_ref_pd, ibpd),
	INIT_RDMA_OBJ_SIZE(ib_ah, uet_ref_ah, ibah),
	INIT_RDMA_OBJ_SIZE(ib_cq, uet_ref_cq, ibcq),
	INIT_RDMA_OBJ_SIZE(ib_qp, uet_ref_qp, ibqp),
	INIT_RDMA_OBJ_SIZE(ib_ucontext, uet_ref_ucontext, ibucontext),
	INIT_RDMA_OBJ_SIZE(ib_job, uet_ref_job, ibjob),
	INIT_RDMA_OBJ_SIZE(ib_jkey, uet_ref_jkey, ibjkey),
};

int uet_ref_verbs_probe(struct uet_ref_dev *dev)
{
	struct ib_device *ibdev = &dev->ib_dev;
	int ret;

	ibdev->node_type = RDMA_NODE_UNSPECIFIED;
	ibdev->phys_port_cnt = 1;
	ibdev->num_comp_vectors = 1;
	ibdev->dev.parent = &dev->pdev->dev;

	strscpy(ibdev->node_desc, "UET reference device",
		sizeof(ibdev->node_desc));

	/* node GUID derived from the MAC */
	addrconf_addr_eui48((u8 *)&ibdev->node_guid, dev->netdev->dev_addr);

	/* set the device ops and stats ops */
	ib_set_device_ops(ibdev, &uet_ref_dev_ops);
	ib_set_device_ops(ibdev, &uet_ref_stats_ops);

	/* The netdev must be associated before registration: ib_core walks
	 * it during registration to build the initial GID table.
	 */
	ret = ib_device_set_netdev(ibdev, dev->netdev, 1);
	if (ret) {
		dev_err(&dev->pdev->dev, "ib_device_set_netdev: %d\n", ret);
		return ret;
	}

	ret = ib_register_device(ibdev, "uet_ref%d", &dev->pdev->dev);
	if (ret) {
		dev_err(&dev->pdev->dev, "ib_register_device: %d\n", ret);
		return ret;
	}

	dev->ib_registered = true;

	ret = uet_ref_stats_add_dev_attrs(dev);
	if (ret)
		dev_warn(&dev->pdev->dev,
			 "device attributes unavailable: %d\n", ret);

	dev_info(&dev->pdev->dev, "ib device %s registered on %s\n",
		 dev_name(&ibdev->dev), dev->netdev->name);

	return 0;
}

void uet_ref_verbs_remove(struct uet_ref_dev *dev)
{
	if (!dev->ib_registered)
		return;

	ib_unregister_device(&dev->ib_dev);
	dev->ib_registered = false;
}

