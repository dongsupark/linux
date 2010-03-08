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

#ifdef CONFIG_PNFS

#include <linux/pnfs_xdr.h>

/* nfs4proc.c */
extern int pnfs4_proc_layoutget(struct nfs4_pnfs_layoutget *lgp);

/* pnfs.c */
extern const nfs4_stateid zero_stateid;

int pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
	u64 count, loff_t pos, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp);

void set_pnfs_layoutdriver(struct super_block *sb, struct nfs_fh *fh, u32 id);
void unmount_pnfs_layoutdriver(struct super_block *sb);
int pnfs_initialize(void);
void pnfs_uninitialize(void);
void pnfs_get_layout_done(struct nfs4_pnfs_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp);

#define PNFS_EXISTS_LDIO_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops->opname)
#define PNFS_EXISTS_LDPOLICY_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_policy_ops && \
				     (srv)->pnfs_curr_ld->ld_policy_ops->opname)

/* Return true if a layout driver is being used for this mountpoint */
static inline int pnfs_enabled_sb(struct nfs_server *nfss)
{
	return nfss->pnfs_curr_ld != NULL;
}

#endif /* CONFIG_PNFS */

#endif /* FS_NFS_PNFS_H */
