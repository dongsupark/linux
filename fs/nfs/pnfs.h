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

#include <linux/nfs_page.h>
#include <linux/nfs4_pnfs.h>
#include <linux/pnfs_xdr.h>

/* nfs4proc.c */
extern int nfs4_pnfs_getdevicelist(struct super_block *sb,
				   struct nfs_fh *fh,
				   struct pnfs_devicelist *devlist);
extern int nfs4_pnfs_getdeviceinfo(struct super_block *sb,
				   struct pnfs_device *dev);

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
int pnfs_use_read(struct inode *inode, ssize_t count);
int pnfs_use_ds_io(struct list_head *, struct inode *, int);
int pnfs_use_write(struct inode *inode, ssize_t count);
enum pnfs_try_status _pnfs_try_to_write_data(struct nfs_write_data *,
					     const struct rpc_call_ops *, int);
enum pnfs_try_status _pnfs_try_to_read_data(struct nfs_read_data *,
					    const struct rpc_call_ops *);
int pnfs_initialize(void);
void pnfs_uninitialize(void);
void pnfs_layoutcommit_done(struct pnfs_layoutcommit_data *data);
void pnfs_layoutcommit_free(struct pnfs_layoutcommit_data *data);
int pnfs_layoutcommit_inode(struct inode *inode, int sync);
void pnfs_update_last_write(struct nfs_inode *nfsi, loff_t offset, size_t extent);
void pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx);
unsigned int pnfs_getiosize(struct nfs_server *server);
void pnfs_set_ds_iosize(struct nfs_server *server);
void pnfs_pageio_init_read(struct nfs_pageio_descriptor *, struct inode *, struct nfs_open_context *, struct list_head *, size_t *);
void pnfs_pageio_init_write(struct nfs_pageio_descriptor *, struct inode *);
ssize_t pnfs_file_write(struct file *, const char __user *, size_t, loff_t *);
void pnfs_get_layout_done(struct nfs4_pnfs_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp);
void pnfs_layout_release(struct pnfs_layout_type *);
int _pnfs_do_flush(struct inode *inode, struct nfs_page *req,
		   struct pnfs_fsdata *fsdata);

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

static inline enum pnfs_try_status
pnfs_try_to_read_data(struct nfs_read_data *data,
		      const struct rpc_call_ops *call_ops)
{
	struct inode *inode = data->inode;
	struct nfs_server *nfss = NFS_SERVER(inode);

	/* FIXME: read_pagelist should probably be mandated */
	if (PNFS_EXISTS_LDIO_OP(nfss, read_pagelist))
		return _pnfs_try_to_read_data(data, call_ops);

	return PNFS_NOT_ATTEMPTED;
}

static inline enum pnfs_try_status
pnfs_try_to_write_data(struct nfs_write_data *data,
		       const struct rpc_call_ops *call_ops,
		       int how)
{
	struct inode *inode = data->inode;
	struct nfs_server *nfss = NFS_SERVER(inode);

	/* FIXME: write_pagelist should probably be mandated */
	if (PNFS_EXISTS_LDIO_OP(nfss, write_pagelist))
		return _pnfs_try_to_write_data(data, call_ops, how);

	return PNFS_NOT_ATTEMPTED;
}

/* req may not be locked, so we have to be prepared for req->wb_page being
 * set to NULL at any time.
 */
static inline int pnfs_do_flush(struct nfs_page *req, void *fsdata)
{
	struct page *page = req->wb_page;
	struct inode *inode;

	if (!page)
		return 1;
	inode = page->mapping->host;

	if (PNFS_EXISTS_LDPOLICY_OP(NFS_SERVER(inode), do_flush))
		return _pnfs_do_flush(inode, req, fsdata);
	else
		return 0;
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

static inline int pnfs_get_write_status(struct nfs_write_data *data)
{
	return data->pdata.pnfs_error;
}

static inline int pnfs_get_read_status(struct nfs_read_data *data)
{
	return data->pdata.pnfs_error;
}

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	if (pnfs_enabled_sb(nfss))
		return pnfs_ld_use_rpc_code(nfss->pnfs_curr_ld);

	return 1;
}

#else  /* CONFIG_PNFS */

static inline enum pnfs_try_status
pnfs_try_to_read_data(struct nfs_read_data *data,
		      const struct rpc_call_ops *call_ops)
{
	return PNFS_NOT_ATTEMPTED;
}

static inline enum pnfs_try_status
pnfs_try_to_write_data(struct nfs_write_data *data,
		       const struct rpc_call_ops *call_ops, int how)
{
	return PNFS_NOT_ATTEMPTED;
}

static inline int pnfs_do_flush(struct nfs_page *req, void *fsdata)
{
	return 0;
}

static inline int pnfs_get_write_status(struct nfs_write_data *data)
{
	return 0;
}

static inline int pnfs_get_read_status(struct nfs_read_data *data)
{
	return 0;
}

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	return 1;
}

#endif /* CONFIG_PNFS */

#endif /* FS_NFS_PNFS_H */
