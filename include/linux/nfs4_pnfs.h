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

#define NFS4_PNFS_GETDEVLIST_MAXNUM 16

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
	/* Layout information. For each inode, alloc_layout is executed once to retrieve an
	 * inode specific layout structure.  Each subsequent layoutget operation results in
	 * a set_layout call to set the opaque layout in the layout driver.*/
	void * (*alloc_layout) (struct pnfs_mount_type *mountid, struct inode *inode);
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
	struct pnfs_mount_type * (*initialize_mountpoint) (struct super_block *, struct nfs_fh *fh);
	int (*uninitialize_mountpoint) (struct pnfs_mount_type *mountid);
};

struct layoutdriver_policy_operations {
	unsigned flags;
};

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
	void (*nfs_return_layout) (struct inode *);
};

extern struct pnfs_client_operations pnfs_ops;

extern struct pnfs_client_operations *pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *);
extern void pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *);

#define NFS4_PNFS_MAX_LAYOUTS 4
#define NFS4_PNFS_PRIVATE_LAYOUT 0x80000000

#endif /* LINUX_NFS4_PNFS_H */
