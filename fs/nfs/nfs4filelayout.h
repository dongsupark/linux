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

enum stripetype4 {
	STRIPE_SPARSE = 1,
	STRIPE_DENSE = 2
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

#endif /* FS_NFS_NFS4FILELAYOUT_H */
