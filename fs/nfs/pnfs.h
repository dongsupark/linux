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

#ifdef CONFIG_PNFS

#include <linux/nfs4_pnfs.h>
#include <linux/pnfs_xdr.h>

/* pnfs.c */
extern const nfs4_stateid zero_stateid;

int pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
	size_t count, loff_t pos, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp);

int _pnfs_return_layout(struct inode *, struct nfs4_pnfs_layout_segment *,
			const nfs4_stateid *stateid, /* optional */
			enum pnfs_layoutrecall_type);
void set_pnfs_layoutdriver(struct super_block *sb, struct nfs_fh *fh, u32 id);
void unmount_pnfs_layoutdriver(struct super_block *sb);
int pnfs_initialize(void);
void pnfs_uninitialize(void);
void pnfs_layoutcommit_done(struct pnfs_layoutcommit_data *data);
void pnfs_layoutcommit_free(struct pnfs_layoutcommit_data *data);
int pnfs_layoutcommit_inode(struct inode *inode, int sync);
void pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx);
void pnfs_get_layout_done(struct nfs4_pnfs_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp);
void pnfs_layout_release(struct pnfs_layout_type *);

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

static inline int pnfs_return_layout(struct inode *ino,
				     struct nfs4_pnfs_layout_segment *lseg,
				     const nfs4_stateid *stateid, /* optional */
				     enum pnfs_layoutrecall_type type)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs_server *nfss = NFS_SERVER(ino);

	if (pnfs_enabled_sb(nfss) &&
	    (nfsi->current_layout || type != RECALL_FILE))
		return _pnfs_return_layout(ino, lseg, stateid, type);

	return 0;
}

#endif /* CONFIG_PNFS */

#endif /* FS_NFS_PNFS_H */
