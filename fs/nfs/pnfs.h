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
extern int nfs4_proc_getdevicelist(struct nfs_server *server,
				   const struct nfs_fh *fh,
				   struct pnfs_devicelist *devlist);
extern int nfs4_proc_getdeviceinfo(struct nfs_server *server,
				   struct pnfs_device *dev);
extern int nfs4_proc_layoutget(struct nfs4_layoutget *lgp);
extern int nfs4_proc_layoutcommit(struct nfs4_layoutcommit_data *data,
				   int issync);
extern int nfs4_proc_layoutreturn(struct nfs4_layoutreturn *lrp, bool wait);

/* pnfs.c */
extern const nfs4_stateid zero_stateid;

void put_lseg(struct pnfs_layout_segment *lseg);
void _pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
	loff_t pos, u64 count, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp);

int _pnfs_return_layout(struct inode *, struct pnfs_layout_range *,
			const nfs4_stateid *stateid, /* optional */
			enum pnfs_layoutreturn_type, bool wait);
void set_pnfs_layoutdriver(struct nfs_server *, u32 id);
void unmount_pnfs_layoutdriver(struct nfs_server *);
enum pnfs_try_status pnfs_try_to_write_data(struct nfs_write_data *,
					     const struct rpc_call_ops *, int);
enum pnfs_try_status pnfs_try_to_read_data(struct nfs_read_data *,
					    const struct rpc_call_ops *);
int pnfs_initialize(void);
void pnfs_uninitialize(void);
void pnfs_layoutcommit_free(struct nfs4_layoutcommit_data *data);
void pnfs_cleanup_layoutcommit(struct nfs4_layoutcommit_data *data);
int pnfs_layoutcommit_inode(struct inode *inode, int sync);
void pnfs_update_last_write(struct nfs_inode *nfsi, loff_t offset, size_t extent);
void pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx);
unsigned int pnfs_getiosize(struct nfs_server *server);
void pnfs_set_ds_iosize(struct nfs_server *server);
enum pnfs_try_status pnfs_try_to_commit(struct nfs_write_data *,
					 const struct rpc_call_ops *, int);
void pnfs_pageio_init_read(struct nfs_pageio_descriptor *, struct inode *,
			   struct nfs_open_context *, struct list_head *,
			   size_t *);
void pnfs_pageio_init_write(struct nfs_pageio_descriptor *, struct inode *,
			    size_t *);
void pnfs_get_layout_done(struct nfs4_layoutget *, int rpc_status);
int pnfs_layout_process(struct nfs4_layoutget *lgp);
void pnfs_layout_release(struct pnfs_layout_hdr *, struct pnfs_layout_range *range);
void pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo,
			     const nfs4_stateid *stateid);
void pnfs_destroy_layout(struct nfs_inode *);
void pnfs_destroy_all_layouts(struct nfs_client *);
void put_layout(struct inode *inode);
void pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_hdr *lo);

#define PNFS_EXISTS_LDIO_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops &&	\
				     (srv)->pnfs_curr_ld->ld_io_ops->opname)
#define PNFS_EXISTS_LDPOLICY_OP(srv, opname) ((srv)->pnfs_curr_ld &&	\
				     (srv)->pnfs_curr_ld->ld_policy_ops && \
				     (srv)->pnfs_curr_ld->ld_policy_ops->opname)

#define LAYOUT_NFSV4_1_MODULE_PREFIX "nfs-layouttype4"

static inline int lo_fail_bit(u32 iomode)
{
	return iomode == IOMODE_RW ?
			 NFS_INO_RW_LAYOUT_FAILED : NFS_INO_RO_LAYOUT_FAILED;
}

static inline void get_lseg(struct pnfs_layout_segment *lseg)
{
	kref_get(&lseg->kref);
}

/* Return true if a layout driver is being used for this mountpoint */
static inline int pnfs_enabled_sb(struct nfs_server *nfss)
{
	return nfss->pnfs_curr_ld != NULL;
}

/* Should the pNFS client commit and return the layout upon a setattr */
static inline bool
pnfs_ld_layoutret_on_setattr(struct inode *inode)
{
	if (!pnfs_enabled_sb(NFS_SERVER(inode)))
		return false;
	return NFS_SERVER(inode)->pnfs_curr_ld->ld_policy_ops->flags &
		PNFS_LAYOUTRET_ON_SETATTR;
}

/* Should the pNFS client commit and return the layout on close
 */
static inline int
pnfs_layout_roc_iomode(struct nfs_inode *nfsi)
{
	return nfsi->layout->roc_iomode;
}

static inline int pnfs_return_layout(struct inode *ino,
				     struct pnfs_layout_range *range,
				     const nfs4_stateid *stateid, /* optional */
				     enum pnfs_layoutreturn_type type,
				     bool wait)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs_server *nfss = NFS_SERVER(ino);

	if (pnfs_enabled_sb(nfss) &&
	    (type != RETURN_FILE || has_layout(nfsi)))
		return _pnfs_return_layout(ino, range, stateid, type, wait);

	return 0;
}

static inline void pnfs_update_layout(struct inode *ino,
	struct nfs_open_context *ctx,
	loff_t pos, u64 count, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp)
{
	struct nfs_server *nfss = NFS_SERVER(ino);

	if (pnfs_enabled_sb(nfss))
		_pnfs_update_layout(ino, ctx, pos, count, access_type, lsegpp);
	else {
		if (lsegpp)
			*lsegpp = NULL;
	}
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

static inline void
pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
	loff_t pos, u64 count, enum pnfs_iomode access_type,
	struct pnfs_layout_segment **lsegpp)
{
	if (lsegpp)
		*lsegpp = NULL;
}

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

static inline int pnfs_layoutcommit_inode(struct inode *inode, int sync)
{
	return 0;
}

static inline bool
pnfs_ld_layoutret_on_setattr(struct inode *inode)
{
	return false;
}

static inline int
pnfs_layout_roc_iomode(struct nfs_inode *nfsi)
{
	return 0;
}

static inline int pnfs_return_layout(struct inode *ino,
				     struct pnfs_layout_range *range,
				     const nfs4_stateid *stateid, /* optional */
				     enum pnfs_layoutreturn_type type,
				     bool wait)
{
	return 0;
}

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
