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

/* Layout driver I/O operations.
 * Either the pagecache or non-pagecache read/write operations must be implemented
 */
struct layoutdriver_io_operations {
	/* Registration information for a new mounted file system
	 */
	int (*initialize_mountpoint) (struct nfs_client *);
	int (*uninitialize_mountpoint) (struct nfs_server *server);
};

struct layoutdriver_policy_operations {
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

#endif /* LINUX_NFS4_PNFS_H */
