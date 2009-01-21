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

struct filelayout_mount_type {
	struct super_block *fl_sb;
};

extern struct pnfs_client_operations *pnfs_callback_ops;

#endif /* FS_NFS_NFS4FILELAYOUT_H */
