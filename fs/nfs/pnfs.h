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
void put_lseg(struct pnfs_layout_segment *lseg);
void set_pnfs_layoutdriver(struct nfs_server *, u32 id);
void unmount_pnfs_layoutdriver(struct nfs_server *);
int pnfs_initialize(void);
void pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo,
			     const nfs4_stateid *stateid);
void pnfs_destroy_layout(struct nfs_inode *);
void pnfs_destroy_all_layouts(struct nfs_client *);
void put_layout(struct inode *inode);
void pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_hdr *lo);

#define PNFS_EXISTS_LDIO_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops->opname)

#define LAYOUT_NFSV4_1_MODULE_PREFIX "nfs-layouttype4"

static inline void get_lseg(struct pnfs_layout_segment *lseg)
{
	kref_get(&lseg->kref);
}

/* Return true if a layout driver is being used for this mountpoint */
static inline int pnfs_enabled_sb(struct nfs_server *nfss)
{
	return nfss->pnfs_curr_ld != NULL;
}

#else  /* CONFIG_NFS_V4_1 */

static inline void pnfs_destroy_all_layouts(struct nfs_client *clp)
{
}

static inline void pnfs_destroy_layout(struct nfs_inode *nfsi)
{
}

static inline void get_lseg(struct pnfs_layout_segment *lseg)
{
}

static inline void put_lseg(struct pnfs_layout_segment *lseg)
{
}

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
