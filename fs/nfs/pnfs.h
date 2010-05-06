/*
 *  pNFS client data structures.
 *
 *  Copyright (c) 2002
 *  The Regents of the University of Michigan
 *  All Rights Reserved
 *
 *  Dean Hildebrand <dhildebz@umich.edu>
 *
 *  Permission is granted to use, copy, create derivative works, and
 *  redistribute this software and such derivative works for any purpose,
 *  so long as the name of the University of Michigan is not used in
 *  any advertising or publicity pertaining to the use or distribution
 *  of this software without specific, written prior authorization. If
 *  the above copyright notice or any other identification of the
 *  University of Michigan is included in any copy of any portion of
 *  this software, then the disclaimer below must also be included.
 *
 *  This software is provided as is, without representation or warranty
 *  of any kind either express or implied, including without limitation
 *  the implied warranties of merchantability, fitness for a particular
 *  purpose, or noninfringement.  The Regents of the University of
 *  Michigan shall not be liable for any damages, including special,
 *  indirect, incidental, or consequential damages, with respect to any
 *  claim arising out of or in connection with the use of the software,
 *  even if it has been or is hereafter advised of the possibility of
 *  such damages.
 */

#ifndef FS_NFS_PNFS_H
#define FS_NFS_PNFS_H

#include <linux/nfs_page.h>
#include "callback.h"

enum {
	NFS_LSEG_VALID = 0,	/* cleared when lseg is recalled/returned */
};

struct pnfs_layout_segment {
	struct list_head fi_list;
	struct pnfs_layout_range range;
	atomic_t pls_refcount;
	unsigned long pls_flags;
	struct pnfs_layout_hdr *layout;
	u64 pls_notify_mask;
};

enum pnfs_try_status {
	PNFS_ATTEMPTED     = 0,
	PNFS_NOT_ATTEMPTED = 1,
};

struct pnfs_fsdata {
	struct pnfs_layout_segment *lseg;
};

#ifdef CONFIG_NFS_V4_1

#define LAYOUT_NFSV4_1_MODULE_PREFIX "nfs-layouttype4"

enum {
	NFS_LAYOUT_RO_FAILED = 0,	/* get ro layout failed stop trying */
	NFS_LAYOUT_RW_FAILED,		/* get rw layout failed stop trying */
	NFS_LAYOUT_BULK_RECALL,		/* bulk recall affecting layout */
	NFS_LAYOUT_NEED_LCOMMIT,	/* LAYOUTCOMMIT needed */
};

enum layoutdriver_policy_flags {
	/* Should the full nfs rpc cleanup code be used after io */
	PNFS_USE_RPC_CODE		= 1 << 0,

	/* Should the pNFS client commit and return the layout upon a setattr */
	PNFS_LAYOUTRET_ON_SETATTR	= 1 << 1,
};

/* Per-layout driver specific registration structure */
struct pnfs_layoutdriver_type {
	struct list_head pnfs_tblid;
	const u32 id;
	const char *name;
	struct module *owner;
	unsigned flags;
	int (*set_layoutdriver) (struct nfs_server *, const struct nfs_fh *);
	int (*clear_layoutdriver) (struct nfs_server *);

	struct pnfs_layout_hdr * (*alloc_layout_hdr) (struct inode *inode);
	void (*free_layout_hdr) (struct pnfs_layout_hdr *);

	struct pnfs_layout_segment * (*alloc_lseg) (struct pnfs_layout_hdr *layoutid, struct nfs4_layoutget_res *lgr);
	void (*free_lseg) (struct pnfs_layout_segment *lseg);

	/* test for nfs page cache coalescing */
	int (*pg_test)(struct nfs_pageio_descriptor *, struct nfs_page *, struct nfs_page *);

	/* Retreive the block size of the file system.
	 * If gather_across_stripes == 1, then the file system will gather
	 * requests into the block size.
	 * TODO: Where will the layout driver get this info?  It is hard
	 * coded in PVFS2.
	 */
	ssize_t (*get_blocksize) (void);

/* read and write pagelist should return just 0 (to indicate that
	 * the layout code has taken control) or 1 (to indicate that the
	 * layout code wishes to fall back to normal nfs.)  If 0 is returned,
	 * information can be passed back through nfs_data->res and
	 * nfs_data->task.tk_status, and the appropriate pnfs done function
	 * MUST be called.
	 */
	enum pnfs_try_status
	(*read_pagelist) (struct nfs_read_data *nfs_data, unsigned nr_pages);
	enum pnfs_try_status
	(*write_pagelist) (struct nfs_write_data *nfs_data, unsigned nr_pages, int how);
	int (*write_begin) (struct pnfs_layout_segment *lseg, struct page *page,
			    loff_t pos, unsigned count,
			    struct pnfs_fsdata *fsdata);

	/* Consistency ops */
	/* 2 problems:
	 * 1) the page list contains nfs_pages, NOT pages
	 * 2) currently the NFS code doesn't create a page array (as it does with read/write)
	 */
	enum pnfs_try_status
	(*commit) (struct nfs_write_data *nfs_data, int how);

	int (*setup_layoutcommit) (struct pnfs_layout_hdr *layoutid,
				   struct nfs4_layoutcommit_args *args);

	void (*encode_layoutcommit) (struct pnfs_layout_hdr *layoutid,
				     struct xdr_stream *xdr,
				     const struct nfs4_layoutcommit_args *args);

	void (*cleanup_layoutcommit) (struct pnfs_layout_hdr *layoutid,
				      struct nfs4_layoutcommit_data *data);

	void (*encode_layoutreturn) (struct pnfs_layout_hdr *layoutid,
				     struct xdr_stream *xdr,
				     const struct nfs4_layoutreturn_args *args);
};

struct pnfs_layout_hdr {
	atomic_t		plh_refcount;
	struct list_head	layouts;   /* other client layouts */
	struct list_head	plh_bulk_recall; /* clnt list of bulk recalls */
	struct list_head	segs;      /* layout segments list */
	int			roc_iomode;/* return on close iomode, 0=none */
	nfs4_stateid		stateid;
	atomic_t		plh_outstanding; /* number of RPCs out */
	unsigned long		plh_block_lgets; /* block LAYOUTGET if >0 */
	u32			plh_barrier; /* ignore lower seqids */
	unsigned long		plh_flags;
	struct rpc_cred		*cred;     /* layoutcommit credential */
	/* DH: These vars keep track of the maximum write range
	 * so the values can be used for layoutcommit.
	 */
	loff_t			write_begin_pos;
	loff_t			write_end_pos;
	struct inode		*inode;
};

struct pnfs_device {
	struct nfs4_deviceid dev_id;
	unsigned int  layout_type;
	unsigned int  mincount;
	struct page **pages;
	void          *area;
	unsigned int  pgbase;
	unsigned int  pglen;
};

struct pnfs_cb_lrecall_info {
	struct list_head	pcl_list; /* hook into cl_layoutrecalls list */
	atomic_t		pcl_count;
	int			pcl_notify_bit;
	struct nfs_client	*pcl_clp;
	struct inode		*pcl_ino;
	struct cb_layoutrecallargs pcl_args;
};

#define NFS4_PNFS_GETDEVLIST_MAXNUM 16

struct pnfs_devicelist {
	unsigned int		eof;
	unsigned int		num_devs;
	struct nfs4_deviceid	dev_id[NFS4_PNFS_GETDEVLIST_MAXNUM];
};

/*
 * Device ID RCU cache. A device ID is unique per client ID and layout type.
 */
#define NFS4_DEVICE_ID_HASH_BITS	5
#define NFS4_DEVICE_ID_HASH_SIZE	(1 << NFS4_DEVICE_ID_HASH_BITS)
#define NFS4_DEVICE_ID_HASH_MASK	(NFS4_DEVICE_ID_HASH_SIZE - 1)

static inline u32
nfs4_deviceid_hash(struct nfs4_deviceid *id)
{
	unsigned char *cptr = (unsigned char *)id->data;
	unsigned int nbytes = NFS4_DEVICEID4_SIZE;
	u32 x = 0;

	while (nbytes--) {
		x *= 37;
		x += *cptr++;
	}
	return x & NFS4_DEVICE_ID_HASH_MASK;
}

struct pnfs_deviceid_node {
	struct hlist_node	de_node;
	struct nfs4_deviceid	de_id;
	atomic_t		de_ref;
};

struct pnfs_deviceid_cache {
	spinlock_t		dc_lock;
	atomic_t		dc_ref;
	void			(*dc_free_callback)(struct pnfs_deviceid_node *);
	struct hlist_head	dc_deviceids[NFS4_DEVICE_ID_HASH_SIZE];
};

extern int pnfs_alloc_init_deviceid_cache(struct nfs_client *,
			void (*free_callback)(struct pnfs_deviceid_node *));
extern void pnfs_put_deviceid_cache(struct nfs_client *);
extern struct pnfs_deviceid_node *pnfs_find_get_deviceid(
				struct pnfs_deviceid_cache *,
				struct nfs4_deviceid *);
extern struct pnfs_deviceid_node *pnfs_add_deviceid(
				struct pnfs_deviceid_cache *,
				struct pnfs_deviceid_node *);
extern void pnfs_put_deviceid(struct pnfs_deviceid_cache *c,
			      struct pnfs_deviceid_node *devid);
extern void pnfs_delete_deviceid(struct pnfs_deviceid_cache *,
				 struct nfs4_deviceid *);

extern int pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *);
extern void pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *);

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
void get_layout_hdr(struct pnfs_layout_hdr *lo);
void put_lseg(struct pnfs_layout_segment *lseg);
bool should_free_lseg(struct pnfs_layout_range *lseg_range,
		      struct pnfs_layout_range *recall_range);
struct pnfs_layout_segment *
pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
		   loff_t pos, u64 count, enum pnfs_iomode access_type);
bool pnfs_return_layout_barrier(struct nfs_inode *, struct pnfs_layout_range *);
int _pnfs_return_layout(struct inode *, struct pnfs_layout_range *, bool wait);
void set_pnfs_layoutdriver(struct nfs_server *, const struct nfs_fh *mntfh, u32 id);
void unset_pnfs_layoutdriver(struct nfs_server *);
enum pnfs_try_status pnfs_try_to_write_data(struct nfs_write_data *,
					     const struct rpc_call_ops *, int);
enum pnfs_try_status pnfs_try_to_read_data(struct nfs_read_data *,
					    const struct rpc_call_ops *);
void pnfs_cleanup_layoutcommit(struct inode *,
			       struct nfs4_layoutcommit_data *);
int pnfs_layoutcommit_inode(struct inode *inode, int sync);
void pnfs_update_last_write(struct nfs_inode *nfsi, loff_t offset, size_t extent);
void pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx);
void pnfs_set_ds_iosize(struct nfs_server *server);
enum pnfs_try_status pnfs_try_to_commit(struct nfs_write_data *,
					 const struct rpc_call_ops *, int);
void pnfs_pageio_init_read(struct nfs_pageio_descriptor *, struct inode *,
			   struct nfs_open_context *, struct list_head *,
			   size_t *);
void pnfs_pageio_init_write(struct nfs_pageio_descriptor *, struct inode *,
			    size_t *);
void pnfs_free_fsdata(struct pnfs_fsdata *fsdata);
bool pnfs_layoutgets_blocked(struct pnfs_layout_hdr *lo, nfs4_stateid *stateid);
int pnfs_layout_process(struct nfs4_layoutget *lgp);
void pnfs_free_lseg_list(struct list_head *tmp_list);
void pnfs_destroy_layout(struct nfs_inode *);
void pnfs_destroy_all_layouts(struct nfs_client *);
void put_layout_hdr(struct pnfs_layout_hdr *lo);
void pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo,
			     const nfs4_stateid *new,
			     bool update_barrier);
int pnfs_choose_layoutget_stateid(nfs4_stateid *dst,
				  struct pnfs_layout_hdr *lo,
				  struct nfs4_state *open_state);
void nfs4_asynch_forget_layouts(struct pnfs_layout_hdr *lo,
				struct pnfs_layout_range *range,
				int notify_bit, atomic_t *notify_count,
				struct list_head *tmp_list);
void pnfs_read_done(struct nfs_read_data *);
void pnfs_writeback_done(struct nfs_write_data *);
void pnfs_commit_done(struct nfs_write_data *);
int _pnfs_write_begin(struct inode *inode, struct page *page,
		      loff_t pos, unsigned len,
		      struct pnfs_layout_segment *lseg,
		      struct pnfs_fsdata **fsdata);

static inline bool
has_layout(struct nfs_inode *nfsi)
{
	return nfsi->layout != NULL;
}

static inline int lo_fail_bit(u32 iomode)
{
	return iomode == IOMODE_RW ?
			 NFS_LAYOUT_RW_FAILED : NFS_LAYOUT_RO_FAILED;
}

static inline void get_lseg(struct pnfs_layout_segment *lseg)
{
	atomic_inc(&lseg->pls_refcount);
	smp_mb__after_atomic_inc();
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
	return NFS_SERVER(inode)->pnfs_curr_ld->flags &
		PNFS_LAYOUTRET_ON_SETATTR;
}

static inline bool pnfs_use_rpc(struct nfs_server *nfss)
{
	if (pnfs_enabled_sb(nfss))
		return nfss->pnfs_curr_ld->flags & PNFS_USE_RPC_CODE;

	return true;
}

/* Should the pNFS client commit and return the layout on close
 */
static inline int
pnfs_layout_roc_iomode(struct nfs_inode *nfsi)
{
	return nfsi->layout->roc_iomode;
}

static inline int pnfs_write_begin(struct file *filp, struct page *page,
				   loff_t pos, unsigned len,
				   struct pnfs_layout_segment *lseg,
				   void **fsdata)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct nfs_server *nfss = NFS_SERVER(inode);
	int status = 0;

	*fsdata = lseg;
	if (lseg && nfss->pnfs_curr_ld->write_begin)
		status = _pnfs_write_begin(inode, page, pos, len, lseg,
					   (struct pnfs_fsdata **) fsdata);
	return status;
}

static inline void pnfs_write_end_cleanup(struct file *filp, void *fsdata)
{
	struct nfs_server *nfss = NFS_SERVER(filp->f_dentry->d_inode);

	if (fsdata && nfss->pnfs_curr_ld) {
		if (nfss->pnfs_curr_ld->write_begin)
			pnfs_free_fsdata(fsdata);
	}
}

static inline int pnfs_return_layout(struct inode *ino,
				     struct pnfs_layout_range *range,
				     bool wait)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs_server *nfss = NFS_SERVER(ino);

	if (pnfs_enabled_sb(nfss) && has_layout(nfsi))
		return _pnfs_return_layout(ino, range, wait);

	return 0;
}

static inline bool
layoutcommit_needed(struct nfs_inode *nfsi)
{
	return has_layout(nfsi) &&
	       test_bit(NFS_LAYOUT_NEED_LCOMMIT, &nfsi->layout->plh_flags);
}

static inline int pnfs_get_write_status(struct nfs_write_data *data)
{
	return data->pdata.pnfs_error;
}

static inline int pnfs_get_read_status(struct nfs_read_data *data)
{
	return data->pdata.pnfs_error;
}

static inline struct pnfs_layout_segment *
nfs4_pull_lseg_from_fsdata(struct file *filp, void *fsdata)
{
	if (fsdata) {
		struct nfs_server *nfss = NFS_SERVER(filp->f_dentry->d_inode);

		if (nfss->pnfs_curr_ld && nfss->pnfs_curr_ld->write_begin)
			return ((struct pnfs_fsdata *) fsdata)->lseg;
		return (struct pnfs_layout_segment *)fsdata;
	}
	return NULL;
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

static inline struct pnfs_layout_segment *
pnfs_update_layout(struct inode *ino, struct nfs_open_context *ctx,
		   loff_t pos, u64 count, enum pnfs_iomode access_type)
{
	return NULL;
}

static inline bool
has_layout(struct nfs_inode *nfsi)
{
	return false;
}

static inline bool
layoutcommit_needed(struct nfs_inode *nfsi)
{
	return 0;
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

static inline int pnfs_layoutcommit_inode(struct inode *inode, int sync)
{
	return 0;
}

static inline bool
pnfs_ld_layoutret_on_setattr(struct inode *inode)
{
	return false;
}

static inline bool pnfs_use_rpc(struct nfs_server *nfss)
{
	return true;
}

static inline int
pnfs_layout_roc_iomode(struct nfs_inode *nfsi)
{
	return 0;
}

static inline int pnfs_return_layout(struct inode *ino,
				     struct pnfs_layout_range *range,
				     bool wait)
{
	return 0;
}

static inline void set_pnfs_layoutdriver(struct nfs_server *s, const struct nfs_fh *mntfh, u32 id)
{
}

static inline void unset_pnfs_layoutdriver(struct nfs_server *s)
{
}

static inline void pnfs_set_ds_iosize(struct nfs_server *server)
{
	server->ds_wsize = server->ds_rsize = -1;
}

static inline int pnfs_write_begin(struct file *filp, struct page *page,
				   loff_t pos, unsigned len,
				   struct pnfs_layout_segment *lseg,
				   void **fsdata)
{
	*fsdata = NULL;
	return 0;
}

static inline void pnfs_write_end_cleanup(struct file *filp, void *fsdata)
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

static inline void
pnfs_pageio_init_read(struct nfs_pageio_descriptor *pgio, struct inode *ino,
		      struct nfs_open_context *ctx, struct list_head *pages,
		      size_t *rsize)
{
	pgio->pg_lseg = NULL;
}

static inline void
pnfs_pageio_init_write(struct nfs_pageio_descriptor *pgio, struct inode *ino,
		       size_t *wsize)
{
	pgio->pg_lseg = NULL;
}

static inline struct pnfs_layout_segment *
nfs4_pull_lseg_from_fsdata(struct file *filp, void *fsdata)
{
	return NULL;
}

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
