/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/*
 * UET reference device model - private state
 */

#ifndef _UET_DEV_PRIV_H_
#define _UET_DEV_PRIV_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "uet_api.h"
#include "uet_addr.h"
#include "uet_dev_abi.h"
#include "uet_dev_core.h"

/* how long the progress thread sleeps when a sweep found nothing to do */
#define UET_DEV_PROGRESS_IDLE_US	100

/* shadow information for a posted work request */
struct uet_dev_work {
	uint64_t wr_id;
	uint32_t qpn;
	uint32_t opcode;
	uint32_t cq; /* device completion queue handle */
	uint32_t job_id;
	     /* Where the device's ring consumer stands once this request is
	      * accounted for. Reported in the completion so the driver can
	      * reuse the slot (see struct uet_dev_cqe).
	      */
	uint32_t ring_cons;
	     /* UET_DEV_CQE_F_* to report with this request's completion */
	uint32_t cqe_flags;
	bool signaled;
	bool busy;
};

/* ring buffer as allocated by the guest (PBL with prod/cons indexes) */
struct uet_dev_ring {
	struct uet_dev_ring_desc desc;
	     /* prod is written by the doorbell and read by the progress
	      * thread that services the ring. Reverse for cons on a
	      * completion ring. Without volatile the progress thread could
	      * keep an index in a CPU register across its whole sweep and
	      * never see the doorbell at all.
	      */
	volatile uint32_t cons;	/* updated by guest for CQ, device for SQ/RQ */
	volatile uint32_t prod;	/* updated by guest for SQ/RQ, device for CQ */
	bool live;
};

/* address entry within a job address table */
struct uet_dev_addr_entry {
	struct uet_dev_admin_addr addr;
	bool valid;
	     /* The library's view of the address. Translation is deferred
	      * until the first work request using this slot it processed.
	      */
	uet_addr_handle_t av;
	     /* uet_av_insert() keeps the pointer it is given rather than
	      * copying, so this has to live as long as the entry does.
	      */
	struct uet_addr ua;
	     /* Removal was asked for while the library still had operations
	      * against this entry. The slot no longer resolves, but its
	      * storage stays until the library lets go.
	      */
	bool retiring;
};

/* A job and its address table. Address entries are indexed by the caller so
 * the table is a flat array with a per-entry valid flag rather than a list.
 */
struct uet_dev_job {
	bool used;
	uint32_t job_id;
	uint32_t max_addr_entries;
	uint32_t flags;
	uint8_t port_num;
	uint8_t sgid_index;
	struct uet_dev_addr_entry *addrs;
	     /* Freed by the guest, but some address entry is still in use.
	      * The job and its table stay until the sweep can finish.
	      */
	bool retiring;
};

/* job key that binds a job to a PD */
struct uet_dev_jkey {
	bool used;
	uint32_t job_handle;
	uint32_t pdn;
	uint32_t flags;
};

/* registered memory region (PBL information is kept by the provider) */
struct uet_dev_mr {
	bool used;
	uet_mr_handle_t mr;
	uint64_t key;
	uint32_t pd;
	uint32_t qp; /* the queue pair it is attached to, or 0 */
	uint32_t access_class; /* UET_DEV_MR_CLASS_* */
	uint32_t job_id; /* for the job restricted classes */
	uint64_t base_va; /* region address as the application sees it */
	uint64_t len;
	bool enabled; /* live in the library's lookup space */
};

/* A guest's completion queue. A library completion queue only exists once
 * bound to an endpoint. This holds the guest's object, its depth, and
 * the queue appears to the library when a queue pair endpoint binds it.
 */
struct uet_dev_cq {
	bool used;
	uint32_t entries;
	uint32_t bound_qps; /* number of QPs referencing this CQ */
	uint32_t db_page;
	uint32_t vector; /* MSI-X to raise when armed */
	struct uet_dev_ring ring;
	uint32_t phase;	/* the valid value entries carry this lap */
	volatile bool armed; /* set by a doorbell, cleared when it fires */
};

/* A guest's queue pair endpoint. */
struct uet_dev_qp {
	bool used;
	uet_ep_handle_t ep;
	uet_cq_handle_t tx_cq;
	uet_cq_handle_t rx_cq;
	struct fid_ep ep_fid;
	struct fid_cq cq_fid;

	uint32_t qpn;
	     /* UET_DEV_QPS_*. Created in RTS! The recovery cycle moves it
	      * to ERROR and back through RESET. The rings are only run in
	      * RTS.
	      */
	uint32_t state;
	uint32_t pd;
	uint32_t send_cq;
	uint32_t recv_cq;
	uint32_t src_id;
	uint32_t jkey_handle;
	uint32_t db_page; /* the doorbell page allowed to ring it */
	     /* Destroy has been asked for but the library still has active
	      * work on the endpoint. The slot stays reserved and the sweep
	      * keeps trying to close it.
	      */
	bool closing;
	bool retire_traced;
	struct uet_dev_ring sq;
	struct uet_dev_ring rq;
	     /* One shadow work request record per ring slot. Handed to the
	      * library as the work request context and handed back on
	      * completion.
	      */
	struct uet_dev_work *sq_work;
	struct uet_dev_work *rq_work;
};

/* address handle representing one peer destination address */
struct uet_dev_ah {
	bool used;
	uint32_t pd;
	struct uet_dev_admin_addr addr;
	uet_addr_handle_t av;
	     /* uet_av_insert() keeps the pointer it is given rather than
	      * copying, so this has to live as long as the entry does.
	      */
	struct uet_addr ua;
};

/* the device state */
struct uet_dev {
	struct uet_dev_backend backend;

	uet_handle_t uet_handle;
	uet_domain_handle_t domain_handle;
	struct fi_info *info;

	     /* Storage the library borrows, not state this model reads. The
	      * device model owns no libfabric objects. It drives the
	      * reference implementation directly but uet_domain() takes a
	      * fabric and a domain by reference and keeps both pointers for
	      * the life of the domain it creates. Nothing here ever looks at
	      * them again.
	      */
	struct fid_fabric fabric;
	struct fid_domain domain;

	pthread_t progress_thread;
	bool progress_running;
	volatile bool progress_stop;

	volatile uint64_t progress_iters;
	volatile uint32_t bar0_reads;
	volatile uint32_t bar0_writes;
	volatile uint32_t doorbells;
	volatile uint32_t irqs_fired;

	     /* L2 channel is a second raw socket carrying everything that is
	      * not UET, so the guest can run an ordinary netdev against this
	      * device.
	      */
	int l2_fd;
	int l2_ifindex;

	     /* Guards the ring state. The doorbell path (BAR2 writes) and
	      * the progress thread both touch this.
	      */
	pthread_mutex_t ring_lock;
	     /* Guards the datapath objects against the sweep that services
	      * them. The admin queue runs on the vfio-user thread and may
	      * destroy a queue pair while the progress thread is actively
	      * working on it.
	      */
	pthread_mutex_t obj_lock;

	uint64_t tx_ring_base;
	uint32_t tx_ring_entries;
	uint32_t tx_ring_ctrl;
	uint32_t tx_prod;
	volatile uint32_t tx_cons;
	uint64_t rx_ring_base;
	uint32_t rx_ring_entries;
	uint32_t rx_ring_ctrl;
	uint32_t rx_prod;
	volatile uint32_t rx_cons;

	uint64_t admin_sq_base;
	uint32_t admin_sq_entries;
	uint32_t admin_sq_ctrl;
	uint32_t admin_sq_prod;
	volatile uint32_t admin_sq_cons;

	uint64_t admin_cq_base;
	uint32_t admin_cq_entries;
	uint32_t admin_cq_ctrl;
	volatile uint32_t admin_cq_prod;
	uint32_t admin_cq_cons;

	volatile uint32_t admin_cmds;
	volatile uint32_t admin_errors;

	/* objects */
	struct uet_dev_job *jobs;
	struct uet_dev_jkey *jkeys;
	struct uet_dev_ah *ahs;
	struct uet_dev_mr *mrs;
	struct uet_dev_qp *qps;
	struct uet_dev_cq *cqs;
	volatile uint32_t jobs_live;
	volatile uint32_t jkeys_live;
	volatile uint32_t mrs_live;

	/* datapath */
	volatile uint32_t sq_posted;
	volatile uint32_t sq_errors;
	volatile uint32_t rq_posted;
	volatile uint32_t rq_errors;
	volatile uint32_t cq_posted;
	volatile uint32_t cq_overruns;
	volatile uint32_t cq_errors;
	volatile uint32_t flushed; /* abandoned on a retiring pair */
	volatile uint32_t db_rejected; /* doorbell naming an unowned handle */
	volatile uint32_t qps_closing; /* destroyed, waiting on the library */
	volatile uint32_t jobs_retiring;
	volatile uint32_t qps_live;
	volatile uint32_t cqs_live;

	volatile uint32_t tx_frames;
	volatile uint32_t tx_errors;
	volatile uint32_t rx_frames;
	volatile uint32_t rx_dropped;
	volatile uint32_t rx_uet_frames;

	struct timespec started;
	uint32_t state;
	uint32_t caps;

	/* cached configuration, resolved once at bring-up */
	uint32_t pds_mode;
	uint32_t sec_mode;
	uint32_t nic_shim;
	uint32_t max_payload;
	uint32_t max_msg_size;
	uint32_t gid_type;
	uint32_t port_proto;
	uint32_t max_tx_retries;
	uint32_t tx_timeout;
	uint32_t pkt_drop_thresh;
	uint32_t sec_ssi;
	uint32_t iov_limit;
	char ifname[UET_DEV_IFNAME_LEN];
	uint8_t mac[6];
	uint32_t ip_ver;
	uint8_t ipaddr[UET_DEV_IPADDR_LEN];
};

/* log trace to stderr if enabled */
void uet_dev_trace(const char *fmt, ...);

#endif /* _UET_DEV_PRIV_H_ */

