// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * Address handles.
 */

#include <rdma/ib_verbs.h>

#include "uet_ref.h"
#include "uet_ref-abi.h"

static void uet_ref_addr_from_rdma_ah(struct uet_dev_admin_addr *out,
				      struct rdma_ah_attr *in,
				      u32 remote_qpn)
{
	const struct ib_global_route *grh;

	memset(out, 0, sizeof(*out));

	if (rdma_ah_get_ah_flags(in) & IB_AH_GRH) {
		grh = rdma_ah_read_grh(in);
		memcpy(out->dgid, grh->dgid.raw, sizeof(out->dgid));
		out->flow_label    = cpu_to_le32(grh->flow_label);
		out->sgid_index    = grh->sgid_index;
		out->hop_limit     = grh->hop_limit;
		out->traffic_class = grh->traffic_class;
		out->is_global     = 1;
	}

	/* RoCE keeps the destination MAC out of the grh */
	if (in->type == RDMA_AH_ATTR_TYPE_ROCE)
		memcpy(out->dmac, in->roce.dmac, sizeof(out->dmac));

	out->sl = rdma_ah_get_sl(in);
	out->port_num = rdma_ah_get_port_num(in);
	out->remote_qpn = cpu_to_le32(remote_qpn);
}

int uet_ref_create_ah(struct ib_ah *ibah,
		      struct rdma_ah_init_attr *init_attr,
		      struct ib_udata *udata)
{
	struct uet_ref_ah *ah = container_of(ibah, struct uet_ref_ah, ibah);
	struct uet_ref_dev *dev = container_of(ibah->device,
					       struct uet_ref_dev, ib_dev);
	struct uet_ref_pd *pd = container_of(ibah->pd, struct uet_ref_pd, ibpd);
	struct uet_dev_admin_addr addr;
	u64 param[1] = { pd->pdn };
	u64 result[2];
	int ret;

	/* A peer on this transport is an address and a queue pair together,
	 * so an address alone is not one: refuse rather than build a handle
	 * that resolves to whoever happens to answer.
	 */
	if (!init_attr->ah_attr_ex)
		return -EINVAL;

	uet_ref_addr_from_rdma_ah(&addr, init_attr->ah_attr,
				  init_attr->ah_attr_ex->remote_qpn);

	ret = uet_ref_admin_cmd(dev, UET_DEV_ADMIN_AH_CREATE, param, 1, &addr,
				sizeof(addr), result);
	if (ret)
		return ret;

	ah->handle = (u32)result[0];

	if (udata && udata->outlen) {
		struct uet_ref_ib_create_ah_resp resp = {
			.ah_handle = ah->handle,
		};

		ret = ib_copy_to_udata(udata, &resp,
				       min(sizeof(resp), udata->outlen));
		if (ret) {
			u64 p[1] = { ah->handle };

			uet_ref_admin_cmd(dev, UET_DEV_ADMIN_AH_DESTROY, p, 1,
					  NULL, 0, NULL);
			return ret;
		}
	}

	return 0;
}

int uet_ref_destroy_ah(struct ib_ah *ibah,
		       u32 flags)
{
	struct uet_ref_ah *ah = container_of(ibah, struct uet_ref_ah, ibah);
	struct uet_ref_dev *dev = container_of(ibah->device,
					       struct uet_ref_dev, ib_dev);
	u64 param[1] = { ah->handle };

	return uet_ref_admin_cmd(dev, UET_DEV_ADMIN_AH_DESTROY, param, 1,
				 NULL, 0, NULL);
}

