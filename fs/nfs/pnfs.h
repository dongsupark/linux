/*
 *  fs/nfs/pnfs.h
 *
 *  pNFS client data structures.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand   <dhildebz@eecs.umich.edu>
 */

#ifndef FS_NFS_PNFS_H
#define FS_NFS_PNFS_H

#include <linux/nfs4_pnfs.h>

#ifdef CONFIG_NFS_V4_1

#include <linux/nfs_page.h>
#include <linux/nfs_iostat.h>
#include "iostat.h"

/* nfs4proc.c */
extern int nfs4_proc_getdeviceinfo(struct nfs_server *server,
				   struct pnfs_device *dev);
/* pnfs.c */
void set_pnfs_layoutdriver(struct nfs_server *, u32 id);
void unmount_pnfs_layoutdriver(struct nfs_server *);
int pnfs_initialize(void);

#define PNFS_EXISTS_LDIO_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops->opname)

#define LAYOUT_NFSV4_1_MODULE_PREFIX "nfs-layouttype4"

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
