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

#include <linux/pnfs_xdr.h>
#include <linux/nfs_page.h>

enum pnfs_try_status {
	PNFS_ATTEMPTED     = 0,
	PNFS_NOT_ATTEMPTED = 1,
};

#define NFS4_PNFS_GETDEVLIST_MAXNUM 16

/* Per-layout driver specific registration structure */
struct pnfs_layoutdriver_type {
	const u32 id;
	const char *name;
	struct layoutdriver_io_operations *ld_io_ops;
	struct layoutdriver_policy_operations *ld_policy_ops;
};

struct pnfs_fsdata {
	int ok_to_use_pnfs;
	struct pnfs_layout_segment *lseg;
};

#if defined(CONFIG_NFS_V4_1)

static inline struct nfs_inode *
PNFS_NFS_INODE(struct pnfs_layout_type *lo)
{
	return container_of(lo, struct nfs_inode, layout);
}

static inline struct inode *
PNFS_INODE(struct pnfs_layout_type *lo)
{
	return &PNFS_NFS_INODE(lo)->vfs_inode;
}

static inline struct nfs_server *
PNFS_NFS_SERVER(struct pnfs_layout_type *lo)
{
	return NFS_SERVER(PNFS_INODE(lo));
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

static inline bool
has_layout(struct nfs_inode *nfsi)
{
	return nfsi->layout.ld_data != NULL;
}

#endif /* CONFIG_NFS_V4_1 */

struct pnfs_layout_segment {
	struct list_head fi_list;
	struct nfs4_pnfs_layout_segment range;
	struct kref kref;
	bool valid;
	struct pnfs_layout_type *layout;
	struct nfs4_deviceid *deviceid;
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
	void * (*alloc_layout) (struct inode *inode);
	void (*free_layout) (void *layoutid);
	struct pnfs_layout_segment * (*alloc_lseg) (struct pnfs_layout_type *layoutid, struct nfs4_pnfs_layoutget_res *lgr);
	void (*free_lseg) (struct pnfs_layout_segment *lseg);

	int (*setup_layoutcommit) (struct pnfs_layout_type *layoutid,
				    struct pnfs_layoutcommit_arg *args);
	void (*encode_layoutcommit) (struct pnfs_layout_type *layoutid,
				     struct xdr_stream *xdr,
				     const struct pnfs_layoutcommit_arg *args);
	void (*cleanup_layoutcommit) (struct pnfs_layout_type *layoutid,
				      struct pnfs_layoutcommit_arg *args,
				      int status);
	void (*encode_layoutreturn) (struct pnfs_layout_type *layoutid,
				struct xdr_stream *xdr,
				const struct nfs4_pnfs_layoutreturn_arg *args);

	/* Registration information for a new mounted file system
	 */
	int (*initialize_mountpoint) (struct super_block *, struct nfs_fh *fh);
	int (*uninitialize_mountpoint) (struct nfs_server *server);
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

/*
 * Device ID RCU cache. A device ID is unique per client ID and layout type.
 */
#define NFS4_DEVICE_ID_HASH_BITS	5
#define NFS4_DEVICE_ID_HASH_SIZE	(1 << NFS4_DEVICE_ID_HASH_BITS)
#define NFS4_DEVICE_ID_HASH_MASK	(NFS4_DEVICE_ID_HASH_SIZE - 1)

static inline u32
nfs4_deviceid_hash(struct pnfs_deviceid *id)
{
	unsigned char *cptr = (unsigned char *)id->data;
	unsigned int nbytes = NFS4_PNFS_DEVICEID4_SIZE;
	u32 x = 0;

	while (nbytes--) {
		x *= 37;
		x += *cptr++;
	}
	return x & NFS4_DEVICE_ID_HASH_MASK;
}

struct nfs4_deviceid_cache {
	spinlock_t		dc_lock;
	struct kref		dc_kref;
	void			(*dc_free_callback)(struct kref *);
	struct hlist_head	dc_deviceids[NFS4_DEVICE_ID_HASH_SIZE];
};

/* Device ID cache node */
struct nfs4_deviceid {
	struct hlist_node	de_node;
	struct pnfs_deviceid	de_id;
	struct kref		de_kref;
};

extern int nfs4_alloc_init_deviceid_cache(struct nfs_client *,
				void (*free_callback)(struct kref *));
extern void nfs4_put_deviceid_cache(struct nfs_client *);
extern void nfs4_init_deviceid_node(struct nfs4_deviceid *);
extern struct nfs4_deviceid *nfs4_find_deviceid(struct nfs4_deviceid_cache *,
				struct pnfs_deviceid *);
extern struct nfs4_deviceid *nfs4_add_deviceid(struct nfs4_deviceid_cache *,
				struct nfs4_deviceid *);
extern void nfs4_set_layout_deviceid(struct pnfs_layout_segment *,
				struct nfs4_deviceid *);
extern void nfs4_unset_layout_deviceid(struct pnfs_layout_segment *,
				struct nfs4_deviceid *,
				void (*free_callback)(struct kref *));

/* pNFS client callback functions.
 * These operations allow the layout driver to access pNFS client
 * specific information or call pNFS client->server operations.
 * E.g., getdeviceinfo, I/O callbacks, etc
 */
struct pnfs_client_operations {
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

#endif /* LINUX_NFS4_PNFS_H */
