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

#include <linux/nfs_page.h>
#include <linux/pnfs_xdr.h>

/* nfs4proc.c */
extern int nfs4_pnfs_getdevicelist(struct super_block *sb,
				   struct nfs_fh *fh,
				   struct pnfs_devicelist *devlist);
extern int nfs4_pnfs_getdeviceinfo(struct super_block *sb,
				   struct pnfs_device *dev);
extern int pnfs4_proc_layoutget(struct nfs4_pnfs_layoutget *lgp);
extern int pnfs4_proc_layoutcommit(struct pnfs_layoutcommit_data *data);
extern int pnfs4_proc_layoutreturn(struct nfs4_pnfs_layoutreturn *lrp);

/* pnfs.c */
extern const nfs4_stateid zero_stateid;

int pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
	u64 count, loff_t pos, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp);

int _pnfs_return_layout(struct inode *, struct nfs4_pnfs_layout_segment *,
			const nfs4_stateid *stateid, /* optional */
			enum pnfs_layoutreturn_type);
void set_pnfs_layoutdriver(struct super_block *sb, struct nfs_fh *fh, u32 id);
void unmount_pnfs_layoutdriver(struct super_block *sb);
int pnfs_use_read(struct inode *inode, ssize_t count);
int pnfs_use_ds_io(struct list_head *, struct inode *, int);
int pnfs_use_write(struct inode *inode, ssize_t count);
int pnfs_initialize(void);
void pnfs_uninitialize(void);
void pnfs_layoutcommit_done(struct pnfs_layoutcommit_data *data);
void pnfs_layoutcommit_free(struct pnfs_layoutcommit_data *data);
int pnfs_layoutcommit_inode(struct inode *inode, int sync);
void pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx);
unsigned int pnfs_getiosize(struct nfs_server *server);
void pnfs_set_ds_iosize(struct nfs_server *server);
void pnfs_pageio_init_read(struct nfs_pageio_descriptor *, struct inode *, struct nfs_open_context *, struct list_head *, size_t *);
void pnfs_get_layout_done(struct nfs4_pnfs_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp);
void pnfs_layout_release(struct pnfs_layout_type *,
			 struct nfs4_pnfs_layout_segment *range);
void pnfs_set_layout_stateid(struct pnfs_layout_type *lo,
			     const nfs4_stateid *stateid);

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
				     enum pnfs_layoutreturn_type type)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs_server *nfss = NFS_SERVER(ino);

	if (pnfs_enabled_sb(nfss) &&
	    (type != RETURN_FILE || has_layout(nfsi)))
		return _pnfs_return_layout(ino, lseg, stateid, type);

	return 0;
}

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	if (pnfs_enabled_sb(nfss))
		return pnfs_ld_use_rpc_code(nfss->pnfs_curr_ld);

	return 1;
}

#else  /* CONFIG_PNFS */

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	return 1;
}

#endif /* CONFIG_PNFS */

#endif /* FS_NFS_PNFS_H */
