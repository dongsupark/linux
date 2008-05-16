/*
 *  pnfs_nfs4filelayout.h
 *
 *  NFSv4 file layout driver data structures.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand   <dhildebz@eecs.umich.edu>
 */

#ifndef FS_NFS_NFS4FILELAYOUT_H
#define FS_NFS_NFS4FILELAYOUT_H

#include <linux/nfs4_pnfs.h>
#include <linux/pnfs_xdr.h>

#define NFS4_PNFS_MAX_STRIPE_CNT 4096
#define NFS4_PNFS_MAX_MULTI_CNT  64 /* 256 fit into a u8 stripe_index */
#define NFS4_PNFS_MAX_MULTI_DS   2

#define FILE_MT(inode) ((struct filelayout_mount_type *) \
			(NFS_SERVER(inode)->pnfs_mountid->mountid))

enum stripetype4 {
	STRIPE_SPARSE = 1,
	STRIPE_DENSE = 2
};

/* Individual ip address */
struct nfs4_pnfs_ds {
	struct hlist_node 	ds_node;  /* nfs4_pnfs_dev_hlist dev_dslist */
	u32 			ds_ip_addr;
	u32 			ds_port;
	struct nfs_client	*ds_clp;
	atomic_t		ds_count;
	char r_addr[29];
};

struct nfs4_multipath {
	int 			num_ds;
	struct nfs4_pnfs_ds	*ds_list[NFS4_PNFS_MAX_MULTI_DS];
};

struct nfs4_file_layout_dsaddr {
	struct pnfs_deviceid	dev_id;
	u32 			stripe_count;
	u8			*stripe_indices;
	u32			multipath_count;
	struct nfs4_multipath	*multipath_list;
};

struct nfs4_filelayout_segment {
	u32 stripe_type;
	u32 commit_through_mds;
	u32 stripe_unit;
	u32 first_stripe_index;
	u64 pattern_offset;
	struct pnfs_deviceid dev_id;
	unsigned int num_fh;
	struct nfs_fh fh_array[NFS4_PNFS_MAX_STRIPE_CNT];
};

struct nfs4_filelayout {
	int uncommitted_write;
	loff_t last_commit_size;
	u64 layout_id;
	u32 stripe_unit;
};

struct filelayout_mount_type {
	struct super_block *fl_sb;
};

extern struct pnfs_client_operations *pnfs_callback_ops;

char *deviceid_fmt(const struct pnfs_deviceid *dev_id);

#define READ32(x)         (x) = ntohl(*p++)
#define READ64(x)         do {			\
	(x) = (u64)ntohl(*p++) << 32;		\
	(x) |= ntohl(*p++);			\
} while (0)
#define COPYMEM(x,nbytes) do {			\
	memcpy((x), p, nbytes);			\
	p += XDR_QUADLEN(nbytes);		\
} while (0)

struct nfs4_file_layout_dsaddr *
nfs4_file_layout_dsaddr_get(struct filelayout_mount_type *,
			  struct pnfs_deviceid *);

#endif /* FS_NFS_NFS4FILELAYOUT_H */
