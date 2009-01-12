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

#if defined(CONFIG_PNFS)

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
	struct inode *inode;
	nfs4_stateid stateid;
	u8 ld_data[];			/* layout driver private data */
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

/* Layout driver I/O operations.
 * Either the pagecache or non-pagecache read/write operations must be implemented
 */
struct layoutdriver_io_operations {
	/* Registration information for a new mounted file system
	 */
	struct pnfs_mount_type * (*initialize_mountpoint) (struct super_block *, struct nfs_fh *fh);
	int (*uninitialize_mountpoint) (struct pnfs_mount_type *mountid);
};

struct layoutdriver_policy_operations {
};

/* pNFS client callback functions.
 * These operations allow the layout driver to access pNFS client
 * specific information or call pNFS client->server operations.
 * E.g., getdeviceinfo, I/O callbacks, etc
 */
struct pnfs_client_operations {
};

extern struct pnfs_client_operations pnfs_ops;

extern struct pnfs_client_operations *pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *);
extern void pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *);

#define NFS4_PNFS_MAX_LAYOUTS 4
#define NFS4_PNFS_PRIVATE_LAYOUT 0x80000000

#endif /* CONFIG_PNFS */

#endif /* LINUX_NFS4_PNFS_H */
