/*
 *  include/linux/nfs4_pnfs.h
 *
 *  Common data structures needed by the pnfs client and pnfs layout driver.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand   <dhildebz@eecs.umich.edu>
 */

#ifndef LINUX_NFS4_PNFS_H
#define LINUX_NFS4_PNFS_H

enum pnfs_try_status {
	PNFS_ATTEMPTED     = 0,
	PNFS_NOT_ATTEMPTED = 1,
};

#if defined(CONFIG_PNFS)

#include <linux/pnfs_xdr.h>
#include <linux/nfs_page.h>

#define NFS4_PNFS_GETDEVLIST_MAXNUM 16

/* NFS4_GETDEVINFO_MAXSIZE is based on file layoutdriver with 4096 stripe
 * indices and 64 multipath data server each with 2 data servers per
 * multipath with ipv4 addresses r_addr and "tcp" r_netid.
 */
#define NFS4_GETDEVINFO_MAXSIZE (4096 * 6)

/* Per-layout driver specific registration structure */
struct pnfs_layoutdriver_type {
	const u32 id;
	const char *name;
	struct layoutdriver_io_operations *ld_io_ops;
	struct layoutdriver_policy_operations *ld_policy_ops;
};

/* Layout driver specific identifier for a mount point.  For each mountpoint
 * a reference is stored in the nfs_server structure.
 */
struct pnfs_mount_type {
	void *mountid;
};

/* Layout driver specific identifier for layout information for a file.
 * Each inode has a specific layout type structure.
 * A reference is stored in the nfs_inode structure.
 */
struct pnfs_layout_type {
	int refcount;
	struct list_head segs;		/* layout segments list */
	int roc_iomode;			/* iomode to return on close, 0=none */
	struct inode *inode;
	seqlock_t seqlock;		/* Protects the stateid */
	nfs4_stateid stateid;
	u8 ld_data[];			/* layout driver private data */
};

struct pnfs_fsdata {
	int ok_to_use_pnfs;
	struct pnfs_layout_segment *lseg;
	void *private;
};


static inline struct inode *
PNFS_INODE(struct pnfs_layout_type *lo)
{
	return lo->inode;
}

static inline struct nfs_inode *
PNFS_NFS_INODE(struct pnfs_layout_type *lo)
{
	return NFS_I(PNFS_INODE(lo));
}

static inline struct nfs_server *
PNFS_NFS_SERVER(struct pnfs_layout_type *lo)
{
	return NFS_SERVER(PNFS_INODE(lo));
}

static inline struct pnfs_mount_type *
PNFS_MOUNTID(struct pnfs_layout_type *lo)
{
	return NFS_SERVER(PNFS_INODE(lo))->pnfs_mountid;
}

static inline void *
PNFS_LD_DATA(struct pnfs_layout_type *lo)
{
	return lo->ld_data;
}

static inline struct pnfs_layoutdriver_type *
PNFS_LD(struct pnfs_layout_type *lo)
{
	return NFS_SERVER(PNFS_INODE(lo))->pnfs_curr_ld;
}

static inline struct layoutdriver_io_operations *
PNFS_LD_IO_OPS(struct pnfs_layout_type *lo)
{
	return PNFS_LD(lo)->ld_io_ops;
}

static inline struct layoutdriver_policy_operations *
PNFS_LD_POLICY_OPS(struct pnfs_layout_type *lo)
{
	return PNFS_LD(lo)->ld_policy_ops;
}

struct pnfs_layout_segment {
	struct list_head fi_list;
	struct nfs4_pnfs_layout_segment range;
	struct kref kref;
	struct pnfs_layout_type *layout;
	u8 ld_data[];			/* layout driver private data */
};

static inline void *
LSEG_LD_DATA(struct pnfs_layout_segment *lseg)
{
	return lseg->ld_data;
}

/* Layout driver I/O operations.
 * Either the pagecache or non-pagecache read/write operations must be implemented
 */
struct layoutdriver_io_operations {
	/* Functions that use the pagecache.
	 * If use_pagecache == 1, then these functions must be implemented.
	 */
	/* read and write pagelist should return just 0 (to indicate that
	 * the layout code has taken control) or 1 (to indicate that the
	 * layout code wishes to fall back to normal nfs.)  If 0 is returned,
	 * information can be passed back through nfs_data->res and
	 * nfs_data->task.tk_status, and the appropriate pnfs done function
	 * MUST be called.
	 */
	enum pnfs_try_status
	(*read_pagelist) (struct pnfs_layout_type *layoutid,
			  struct page **pages, unsigned int pgbase,
			  unsigned nr_pages, loff_t offset, size_t count,
			  struct nfs_read_data *nfs_data);
	enum pnfs_try_status
	(*write_pagelist) (struct pnfs_layout_type *layoutid,
			   struct page **pages, unsigned int pgbase,
			   unsigned nr_pages, loff_t offset, size_t count,
			   int sync, struct nfs_write_data *nfs_data);
	int (*write_begin) (struct pnfs_layout_segment *lseg, struct page *page,
			    loff_t pos, unsigned count,
			    struct pnfs_fsdata *fsdata);
	int (*write_end)(struct inode *inode, struct page *page, loff_t pos,
			 unsigned count, unsigned copied,
			 struct pnfs_fsdata *fsdata);
	void (*write_end_cleanup)(struct file *filp,
				  struct pnfs_fsdata *fsdata);
	void (*new_request)(struct pnfs_layout_segment *lseg,
			    struct nfs_page *req, loff_t pos, unsigned count,
			    struct pnfs_fsdata *fsdata);

	/* Consistency ops */
	/* 2 problems:
	 * 1) the page list contains nfs_pages, NOT pages
	 * 2) currently the NFS code doesn't create a page array (as it does with read/write)
	 */
	enum pnfs_try_status
	(*commit) (struct pnfs_layout_type *layoutid,
		   int sync, struct nfs_write_data *nfs_data);

	/* Layout information. For each inode, alloc_layout is executed once to retrieve an
	 * inode specific layout structure.  Each subsequent layoutget operation results in
	 * a set_layout call to set the opaque layout in the layout driver.*/
	struct pnfs_layout_type * (*alloc_layout) (struct pnfs_mount_type *mountid, struct inode *inode);
	void (*free_layout) (struct pnfs_layout_type *layoutid);
	struct pnfs_layout_segment * (*alloc_lseg) (struct pnfs_layout_type *layoutid, struct nfs4_pnfs_layoutget_res *lgr);
	void (*free_lseg) (struct pnfs_layout_segment *lseg);

	int (*setup_layoutcommit) (struct pnfs_layout_type *layoutid,
				   struct pnfs_layoutcommit_data *data);
	void (*cleanup_layoutcommit) (struct pnfs_layout_type *layoutid,
				      struct pnfs_layoutcommit_data *data);

	/* Registration information for a new mounted file system
	 */
	struct pnfs_mount_type * (*initialize_mountpoint) (struct super_block *, struct nfs_fh *fh);
	int (*uninitialize_mountpoint) (struct pnfs_mount_type *mountid);
	int (*device_delete) (struct pnfs_mount_type *mountid, struct pnfs_deviceid *dev_id);
};

enum layoutdriver_policy_flags {
	/* Should the full nfs rpc cleanup code be used after io */
	PNFS_USE_RPC_CODE		= 1 << 0,

	/* Should the NFS req. gather algorithm cross stripe boundaries? */
	PNFS_GATHER_ACROSS_STRIPES	= 1 << 1,

	/* Should the pNFS client issue a layoutget call in the
	 * same compound as the OPEN operation?
	 */
	PNFS_LAYOUTGET_ON_OPEN		= 1 << 2,

	/* Should the pNFS client commit and return the layout upon a setattr */
	PNFS_LAYOUTRET_ON_SETATTR	= 1 << 3,
};

struct layoutdriver_policy_operations {
	unsigned flags;

	/* The stripe size of the file system */
	ssize_t (*get_stripesize) (struct pnfs_layout_type *layoutid);

	/* test for nfs page cache coalescing */
	int (*pg_test)(struct nfs_pageio_descriptor *, struct nfs_page *, struct nfs_page *);

	/* Test for pre-write request flushing */
	int (*do_flush)(struct pnfs_layout_segment *lseg, struct nfs_page *req,
			struct pnfs_fsdata *fsdata);

	/* Retreive the block size of the file system.  If gather_across_stripes == 1,
	 * then the file system will gather requests into the block size.
	 * TODO: Where will the layout driver get this info?  It is hard coded in PVFS2.
	 */
	ssize_t (*get_blocksize) (struct pnfs_mount_type *);

	/* Read requests under this value are sent to the NFSv4 server */
	ssize_t (*get_read_threshold) (struct pnfs_layout_type *, struct inode *);

	/* Write requests under this value are sent to the NFSv4 server */
	ssize_t (*get_write_threshold) (struct pnfs_layout_type *, struct inode *);
};

/* Should the full nfs rpc cleanup code be used after io */
static inline int
pnfs_ld_use_rpc_code(struct pnfs_layoutdriver_type *ld)
{
	return ld->ld_policy_ops->flags & PNFS_USE_RPC_CODE;
}

/* Should the NFS req. gather algorithm cross stripe boundaries? */
static inline int
pnfs_ld_gather_across_stripes(struct pnfs_layoutdriver_type *ld)
{
	return ld->ld_policy_ops->flags & PNFS_GATHER_ACROSS_STRIPES;
}

/* Should the pNFS client issue a layoutget call in the
 * same compound as the OPEN operation?
 */
static inline int
pnfs_ld_layoutget_on_open(struct pnfs_layoutdriver_type *ld)
{
	return ld->ld_policy_ops->flags & PNFS_LAYOUTGET_ON_OPEN;
}

/* Should the pNFS client commit and return the layout upon a setattr
 */
static inline int
pnfs_ld_layoutret_on_setattr(struct pnfs_layoutdriver_type *ld)
{
	return ld->ld_policy_ops->flags & PNFS_LAYOUTRET_ON_SETATTR;
}

struct pnfs_device {
	struct pnfs_deviceid dev_id;
	unsigned int  layout_type;
	unsigned int  mincount;
	struct page **pages;
	void          *area;
	unsigned int  pgbase;
	unsigned int  pglen;
	unsigned int  dev_notify_types;
};

struct pnfs_devicelist {
	unsigned int		eof;
	unsigned int		num_devs;
	struct pnfs_deviceid	dev_id[NFS4_PNFS_GETDEVLIST_MAXNUM];
};

/* pNFS client callback functions.
 * These operations allow the layout driver to access pNFS client
 * specific information or call pNFS client->server operations.
 * E.g., getdeviceinfo, I/O callbacks, etc
 */
struct pnfs_client_operations {
	int (*nfs_getdevicelist) (struct super_block *sb, struct nfs_fh *fh,
				  struct pnfs_devicelist *devlist);
	int (*nfs_getdeviceinfo) (struct super_block *sb,
				  struct pnfs_device *dev);

	/* Post read callback. */
	void (*nfs_readlist_complete) (struct nfs_read_data *nfs_data);

	/* Post write callback. */
	void (*nfs_writelist_complete) (struct nfs_write_data *nfs_data);

	/* Post commit callback. */
	void (*nfs_commit_complete) (struct nfs_write_data *nfs_data);
	void (*nfs_return_layout) (struct inode *);
};

extern struct pnfs_client_operations pnfs_ops;

extern struct pnfs_client_operations *pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *);
extern void pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *);

#define NFS4_PNFS_MAX_LAYOUTS 4
#define NFS4_PNFS_PRIVATE_LAYOUT 0x80000000

#endif /* CONFIG_PNFS */

#endif /* LINUX_NFS4_PNFS_H */
