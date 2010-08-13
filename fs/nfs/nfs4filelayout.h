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

#include <linux/kref.h>
#include <linux/nfs4_pnfs.h>

#define NFS4_PNFS_DEV_HASH_BITS 5
#define NFS4_PNFS_DEV_HASH_SIZE (1 << NFS4_PNFS_DEV_HASH_BITS)
#define NFS4_PNFS_DEV_HASH_MASK (NFS4_PNFS_DEV_HASH_SIZE - 1)

/* Individual ip address */
struct nfs4_pnfs_ds {
	struct list_head	ds_node;  /* nfs4_pnfs_dev_hlist dev_dslist */
	u32 			ds_ip_addr;
	u32 			ds_port;
	struct nfs_client	*ds_clp;
	atomic_t		ds_count;
	char r_addr[29];
};

struct nfs4_file_layout_dsaddr {
	struct nfs4_deviceid	deviceid;
	u32 			stripe_count;
	u8			*stripe_indices;
	u32			ds_num;
	struct nfs4_pnfs_ds	*ds_list[1];
};


extern struct pnfs_client_operations *pnfs_callback_ops;

extern void nfs4_fl_free_deviceid_callback(struct kref *);
extern void print_ds(struct nfs4_pnfs_ds *ds);
#endif /* FS_NFS_NFS4FILELAYOUT_H */
