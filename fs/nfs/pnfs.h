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
#include <linux/pnfs_xdr.h>
#include <linux/nfs_iostat.h>
#include "iostat.h"

/* nfs4proc.c */
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
void set_pnfs_layoutdriver(struct nfs_server *, u32 id);
void unmount_pnfs_layoutdriver(struct nfs_server *);
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
enum pnfs_try_status _pnfs_try_to_commit(struct nfs_write_data *,
					 const struct rpc_call_ops *, int);
void pnfs_pageio_init_read(struct nfs_pageio_descriptor *, struct inode *,
			   struct nfs_open_context *, struct list_head *);
void pnfs_pageio_init_write(struct nfs_pageio_descriptor *, struct inode *);
void pnfs_update_layout_commit(struct inode *, struct list_head *, pgoff_t, unsigned int);
void pnfs_free_fsdata(struct pnfs_fsdata *fsdata);
ssize_t pnfs_file_write(struct file *, const char __user *, size_t, loff_t *);
void pnfs_get_layout_done(struct nfs4_pnfs_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp);
void pnfs_layout_release(struct pnfs_layout_type *, atomic_t *,
			 struct nfs4_pnfs_layout_segment *range);
void pnfs_set_layout_stateid(struct pnfs_layout_type *lo,
			     const nfs4_stateid *stateid);
void pnfs_destroy_layout(struct nfs_inode *);
int _pnfs_write_begin(struct inode *inode, struct page *page,
		      loff_t pos, unsigned len,
		      struct pnfs_fsdata **fsdata);
int _pnfs_write_end(struct inode *inode, struct page *page,
		    loff_t pos, unsigned len,
		    unsigned copied, struct pnfs_fsdata *fsdata);
int _pnfs_do_flush(struct inode *inode, struct nfs_page *req,
		   struct pnfs_fsdata *fsdata);
void _pnfs_modify_new_write_request(struct nfs_page *req,
				    struct pnfs_fsdata *fsdata);
void _pnfs_direct_init_io(struct inode *inode, struct nfs_open_context *ctx,
			  size_t count, loff_t loff, int iswrite,
			  size_t *rwsize, size_t *remaining);

#define PNFS_EXISTS_LDIO_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops->opname)
#define PNFS_EXISTS_LDPOLICY_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_policy_ops && \
				     (srv)->pnfs_curr_ld->ld_policy_ops->opname)

static inline int lo_fail_bit(u32 iomode)
{
	return iomode == IOMODE_RW ?
			 NFS_INO_RW_LAYOUT_FAILED : NFS_INO_RO_LAYOUT_FAILED;
}

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
	enum pnfs_try_status ret;

	/* FIXME: read_pagelist should probably be mandated */
	if (PNFS_EXISTS_LDIO_OP(nfss, read_pagelist))
		ret = _pnfs_try_to_read_data(data, call_ops);
	else
		ret = PNFS_NOT_ATTEMPTED;

	if (ret == PNFS_ATTEMPTED)
		nfs_inc_stats(inode, NFSIOS_PNFS_READ);
	return ret;
}

static inline enum pnfs_try_status
pnfs_try_to_write_data(struct nfs_write_data *data,
		       const struct rpc_call_ops *call_ops,
		       int how)
{
	struct inode *inode = data->inode;
	struct nfs_server *nfss = NFS_SERVER(inode);
	enum pnfs_try_status ret;

	/* FIXME: write_pagelist should probably be mandated */
	if (PNFS_EXISTS_LDIO_OP(nfss, write_pagelist))
		ret = _pnfs_try_to_write_data(data, call_ops, how);
	else
		ret = PNFS_NOT_ATTEMPTED;

	if (ret == PNFS_ATTEMPTED)
		nfs_inc_stats(inode, NFSIOS_PNFS_WRITE);
	return ret;
}

static inline enum pnfs_try_status
pnfs_try_to_commit(struct nfs_write_data *data,
		   const struct rpc_call_ops *call_ops,
		   int how)
{
	struct inode *inode = data->inode;
	struct nfs_server *nfss = NFS_SERVER(inode);
	enum pnfs_try_status ret;

	/* Note that we check for "write_pagelist" and not for "commit"
	   since if async writes were done and pages weren't marked as stable
	   the commit method MUST be defined by the LD */
	/* FIXME: write_pagelist should probably be mandated */
	if (PNFS_EXISTS_LDIO_OP(nfss, write_pagelist))
		ret = _pnfs_try_to_commit(data, call_ops, how);
	else
		ret = PNFS_NOT_ATTEMPTED;

	if (ret == PNFS_ATTEMPTED)
		nfs_inc_stats(inode, NFSIOS_PNFS_COMMIT);
	return ret;
}

static inline int pnfs_write_begin(struct file *filp, struct page *page,
				   loff_t pos, unsigned len, void **fsdata)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct nfs_server *nfss = NFS_SERVER(inode);
	int status = 0;

	*fsdata = NULL;
	if (PNFS_EXISTS_LDIO_OP(nfss, write_begin))
		status = _pnfs_write_begin(inode, page, pos, len,
					   (struct pnfs_fsdata **) fsdata);
	return status;
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

static inline int pnfs_write_end(struct file *filp, struct page *page,
				 loff_t pos, unsigned len, unsigned copied,
				 void *fsdata)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct nfs_server *nfss = NFS_SERVER(inode);

	if (PNFS_EXISTS_LDIO_OP(nfss, write_end))
		return _pnfs_write_end(inode, page, pos, len, copied, fsdata);
	else
		return 0;
}

static inline void pnfs_write_end_cleanup(void *fsdata)
{
	pnfs_free_fsdata(fsdata);
}

static inline void pnfs_redirty_request(struct nfs_page *req)
{
	clear_bit(PG_USE_PNFS, &req->wb_flags);
}

static inline void pnfs_modify_new_request(struct nfs_page *req,
					   void *fsdata)
{
	if (fsdata)
		_pnfs_modify_new_write_request(req, fsdata);
	/* Should we do something (like set PG_USE_PNFS) if !fsdata ? */
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

static inline int pnfs_get_write_status(struct nfs_write_data *data)
{
	return data->pdata.pnfs_error;
}

static inline int pnfs_get_read_status(struct nfs_read_data *data)
{
	return data->pdata.pnfs_error;
}

static inline void pnfs_direct_init_io(struct inode *inode,
				       struct nfs_open_context *ctx,
				       size_t count, loff_t loff, int iswrite,
				       size_t *iosize, size_t *remaining)
{
	struct nfs_server *nfss = NFS_SERVER(inode);

	if (pnfs_enabled_sb(nfss))
		return _pnfs_direct_init_io(inode, ctx, count, loff, iswrite,
					    iosize, remaining);

	return;
}

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	if (pnfs_enabled_sb(nfss))
		return pnfs_ld_use_rpc_code(nfss->pnfs_curr_ld);

	return 1;
}

#else  /* CONFIG_NFS_V4_1 */

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

static inline enum pnfs_try_status
pnfs_try_to_commit(struct nfs_write_data *data,
		   const struct rpc_call_ops *call_ops, int how)
{
	return PNFS_NOT_ATTEMPTED;
}

static inline int pnfs_do_flush(struct nfs_page *req, void *fsdata)
{
	return 0;
}

static inline int pnfs_write_begin(struct file *filp, struct page *page,
				   loff_t pos, unsigned len, void **fsdata)
{
	return 0;
}

static inline int pnfs_write_end(struct file *filp, struct page *page,
				 loff_t pos, unsigned len, unsigned copied,
				 void *fsdata)
{
	return 0;
}

static inline void pnfs_write_end_cleanup(void *fsdata)
{
}

static inline void pnfs_redirty_request(struct nfs_page *req)
{
}

static inline void pnfs_modify_new_request(struct nfs_page *req,
					   void *fsdata)
{
}

static inline int pnfs_get_write_status(struct nfs_write_data *data)
{
	return 0;
}

static inline int pnfs_get_read_status(struct nfs_read_data *data)
{
	return 0;
}

/* Set num of remaining bytes, which is everything */
static inline void pnfs_direct_init_io(struct inode *inode,
				       struct nfs_open_context *ctx,
				       size_t count, loff_t loff, int iswrite,
				       size_t *iosize, size_t *remaining)
{
}

static inline int pnfs_use_rpc(struct nfs_server *nfss)
{
	return 1;
}

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
