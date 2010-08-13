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

/* Per-layout driver specific registration structure */
struct pnfs_layoutdriver_type {
	const u32 id;
	const char *name;
	struct layoutdriver_io_operations *ld_io_ops;
	struct layoutdriver_policy_operations *ld_policy_ops;
};

#if defined(CONFIG_NFS_V4_1)

static inline struct nfs_inode *
PNFS_NFS_INODE(struct pnfs_layout_hdr *lo)
{
	return NFS_I(lo->inode);
}

static inline struct inode *
PNFS_INODE(struct pnfs_layout_hdr *lo)
{
	return lo->inode;
}

static inline struct nfs_server *
PNFS_NFS_SERVER(struct pnfs_layout_hdr *lo)
{
	return NFS_SERVER(PNFS_INODE(lo));
}

static inline struct pnfs_layoutdriver_type *
PNFS_LD(struct pnfs_layout_hdr *lo)
{
	return NFS_SERVER(PNFS_INODE(lo))->pnfs_curr_ld;
}

static inline struct layoutdriver_io_operations *
PNFS_LD_IO_OPS(struct pnfs_layout_hdr *lo)
{
	return PNFS_LD(lo)->ld_io_ops;
}


#endif /* CONFIG_NFS_V4_1 */

struct pnfs_layout_segment {
	struct list_head fi_list;
	struct pnfs_layout_range range;
	struct kref kref;
	bool valid;
	struct pnfs_layout_hdr *layout;
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
	/* Layout information. For each inode, alloc_layout is executed once to retrieve an
	 * inode specific layout structure.  Each subsequent layoutget operation results in
	 * a set_layout call to set the opaque layout in the layout driver.*/
	struct pnfs_layout_hdr * (*alloc_layout) (struct inode *inode);
	void (*free_layout) (struct pnfs_layout_hdr *);
	struct pnfs_layout_segment * (*alloc_lseg) (struct pnfs_layout_hdr *layoutid, struct nfs4_layoutget_res *lgr);
	void (*free_lseg) (struct pnfs_layout_segment *lseg);

	/* Registration information for a new mounted file system
	 */
	int (*initialize_mountpoint) (struct nfs_client *);
	int (*uninitialize_mountpoint) (struct nfs_server *server);
};

struct layoutdriver_policy_operations {
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
	int (*nfs_getdeviceinfo) (struct nfs_server *,
				  struct pnfs_device *dev);
};

extern struct pnfs_client_operations pnfs_ops;

extern struct pnfs_client_operations *pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *);
extern void pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *);

#endif /* LINUX_NFS4_PNFS_H */
