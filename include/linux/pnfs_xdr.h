/*
 *  include/linux/pnfs_xdr.h
 *
 *  Common xdr data structures needed by pnfs client.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 * Dean Hildebrand   <dhildebz@eecs.umich.edu>
 */

#ifndef LINUX_PNFS_XDR_H
#define LINUX_PNFS_XDR_H

#if defined(CONFIG_PNFS)

#define PNFS_LAYOUT_MAXSIZE 4096

struct nfs4_pnfs_layout {
	__u32 len;
	void *buf;
};

struct nfs4_pnfs_layout_segment {
	u32 iomode;
	u64 offset;
	u64 length;
};

struct nfs4_pnfs_layoutget_arg {
	__u32 type;
	struct nfs4_pnfs_layout_segment lseg;
	__u64 minlength;
	__u32 maxcount;
	struct nfs_open_context *ctx;
	nfs4_stateid stateid;
	struct inode *inode;
	struct nfs4_sequence_args seq_args;
};

struct nfs4_pnfs_layoutget_res {
	__u32 return_on_close;
	struct nfs4_pnfs_layout_segment lseg;
	__u32 type;
	nfs4_stateid stateid;
	struct nfs4_pnfs_layout layout;
	struct nfs4_sequence_res seq_res;
};

struct nfs4_pnfs_layoutget {
	struct pnfs_layout_type *lo;
	struct nfs4_pnfs_layoutget_arg args;
	struct nfs4_pnfs_layoutget_res res;
	struct pnfs_layout_segment **lsegpp;
	int status;
};

struct pnfs_layoutcommit_arg {
	nfs4_stateid stateid;
	__u64 lastbytewritten;
	__u32 time_modify_changed;
	struct timespec time_modify;
	const u32 *bitmask;
	struct nfs_fh *fh;

	/* Values set by layout driver */
	struct nfs4_pnfs_layout_segment lseg;
	__u32 layout_type;
	__u32 new_layout_size;
	void *new_layout;
	struct nfs4_sequence_args seq_args;
};

struct pnfs_layoutcommit_res {
	__u32 sizechanged;
	__u64 newsize;
	struct nfs_fattr *fattr;
	const struct nfs_server *server;
	struct nfs4_sequence_res seq_res;
};

struct pnfs_layoutcommit_data {
	struct rpc_task task;
	bool is_sync;
	struct inode *inode;
	struct rpc_cred *cred;
	struct nfs_fattr fattr;
	struct nfs_open_context *ctx;
	struct pnfs_layoutcommit_arg args;
	struct pnfs_layoutcommit_res res;
	int status;
};

struct nfs4_pnfs_layoutreturn_arg {
	__u32	reclaim;
	__u32	layout_type;
	__u32	return_type;
	struct nfs4_pnfs_layout_segment lseg;
	nfs4_stateid stateid;
	struct inode *inode;
	struct nfs4_sequence_args seq_args;
};

struct nfs4_pnfs_layoutreturn_res {
	struct nfs4_sequence_res seq_res;
	u32 lrs_present;
	nfs4_stateid stateid;
};

struct nfs4_pnfs_layoutreturn {
	struct pnfs_layout_type *lo;
	struct nfs4_pnfs_layoutreturn_arg args;
	struct nfs4_pnfs_layoutreturn_res res;
	struct rpc_cred *cred;
	int rpc_status;
};

#endif /* CONFIG_PNFS */

#endif /* LINUX_PNFS_XDR_H */
