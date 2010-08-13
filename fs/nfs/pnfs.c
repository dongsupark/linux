/*
 *  linux/fs/nfs/pnfs.c
 *
 *  pNFS functions to call and manage layout drivers.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand <dhildebz@eecs.umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/nfs_page.h>
#include <linux/nfs4.h>
#include <linux/nfs4_pnfs.h>
#include <linux/rculist.h>

#include "internal.h"
#include "nfs4_fs.h"
#include "pnfs.h"

#define NFSDBG_FACILITY		NFSDBG_PNFS

#define MIN_POOL_LC		(4)

static int pnfs_initialized;

static void pnfs_free_layout(struct pnfs_layout_hdr *lo,
			     struct pnfs_layout_range *range);
static inline void get_layout(struct pnfs_layout_hdr *lo);

/* Locking:
 *
 * pnfs_spinlock:
 * 	protects pnfs_modules_tbl.
 */
static spinlock_t pnfs_spinlock = __SPIN_LOCK_UNLOCKED(pnfs_spinlock);

/*
 * pnfs_modules_tbl holds all pnfs modules
 */
static struct list_head	pnfs_modules_tbl;

/*
 * struct pnfs_module - One per pNFS device module.
 */
struct pnfs_module {
	struct pnfs_layoutdriver_type *pnfs_ld_type;
	struct list_head        pnfs_tblid;
};

int
pnfs_initialize(void)
{
	INIT_LIST_HEAD(&pnfs_modules_tbl);

	pnfs_initialized = 1;
	return 0;
}

/* search pnfs_modules_tbl for right pnfs module */
static int
find_pnfs(u32 id, struct pnfs_module **module) {
	struct  pnfs_module *local = NULL;

	dprintk("PNFS: %s: Searching for %u\n", __func__, id);
	list_for_each_entry(local, &pnfs_modules_tbl, pnfs_tblid) {
		if (local->pnfs_ld_type->id == id) {
			*module = local;
			return(1);
		}
	}
	return 0;
}

/* Unitialize a mountpoint in a layout driver */
void
unmount_pnfs_layoutdriver(struct nfs_server *nfss)
{
	if (PNFS_EXISTS_LDIO_OP(nfss, uninitialize_mountpoint))
		nfss->pnfs_curr_ld->ld_io_ops->uninitialize_mountpoint(nfss);
}

/*
 * Set the server pnfs module to the first registered pnfs_type.
 * Only one pNFS layout driver is supported.
 */
void
set_pnfs_layoutdriver(struct nfs_server *server, u32 id)
{
	struct pnfs_module *mod = NULL;

	if (server->pnfs_curr_ld)
		return;

	if (!find_pnfs(id, &mod)) {
		request_module("%s-%u", LAYOUT_NFSV4_1_MODULE_PREFIX, id);
		find_pnfs(id, &mod);
	}

	if (!mod) {
		dprintk("%s: No pNFS module found for %u. ", __func__, id);
		goto out_err;
	}

	server->pnfs_curr_ld = mod->pnfs_ld_type;
	if (mod->pnfs_ld_type->ld_io_ops->initialize_mountpoint(
							server->nfs_client)) {
		printk(KERN_ERR "%s: Error initializing mount point "
		       "for layout driver %u. ", __func__, id);
		goto out_err;
	}

	dprintk("%s: pNFS module for %u set\n", __func__, id);
	return;

out_err:
	dprintk("Using NFSv4 I/O\n");
	server->pnfs_curr_ld = NULL;
}

/* Allow I/O module to set its functions structure */
struct pnfs_client_operations*
pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	struct pnfs_module *pnfs_mod;
	struct layoutdriver_io_operations *io_ops = ld_type->ld_io_ops;

	if (!pnfs_initialized) {
		printk(KERN_ERR "%s Registration failure. "
		       "pNFS not initialized.\n", __func__);
		return NULL;
	}

	if (!io_ops || !io_ops->alloc_layout || !io_ops->free_layout) {
		printk(KERN_ERR "%s Layout driver must provide "
		       "alloc_layout and free_layout.\n", __func__);
		return NULL;
	}

	pnfs_mod = kmalloc(sizeof(struct pnfs_module), GFP_KERNEL);
	if (pnfs_mod != NULL) {
		dprintk("%s Registering id:%u name:%s\n",
			__func__,
			ld_type->id,
			ld_type->name);
		pnfs_mod->pnfs_ld_type = ld_type;
		INIT_LIST_HEAD(&pnfs_mod->pnfs_tblid);

		spin_lock(&pnfs_spinlock);
		list_add(&pnfs_mod->pnfs_tblid, &pnfs_modules_tbl);
		spin_unlock(&pnfs_spinlock);
	}

	return &pnfs_ops;
}

/*  Allow I/O module to set its functions structure */
void
pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	struct pnfs_module *pnfs_mod;

	if (find_pnfs(ld_type->id, &pnfs_mod)) {
		dprintk("%s Deregistering id:%u\n", __func__, ld_type->id);
		spin_lock(&pnfs_spinlock);
		list_del(&pnfs_mod->pnfs_tblid);
		spin_unlock(&pnfs_spinlock);
		kfree(pnfs_mod);
	}
}

/*
 * pNFS client layout cache
 */
#if defined(CONFIG_SMP)
#define BUG_ON_UNLOCKED_INO(ino) \
	BUG_ON(!spin_is_locked(&ino->i_lock))
#define BUG_ON_UNLOCKED_LO(lo) \
	BUG_ON_UNLOCKED_INO(PNFS_INODE(lo))
#else /* CONFIG_SMP */
#define BUG_ON_UNLOCKED_INO(lo) do {} while (0)
#define BUG_ON_UNLOCKED_LO(lo) do {} while (0)
#endif /* CONFIG_SMP */

static inline void
get_layout(struct pnfs_layout_hdr *lo)
{
	BUG_ON_UNLOCKED_LO(lo);
	lo->refcount++;
}

static inline void
put_layout_locked(struct pnfs_layout_hdr *lo)
{
	BUG_ON_UNLOCKED_LO(lo);
	BUG_ON(lo->refcount <= 0);

	lo->refcount--;
	if (!lo->refcount) {
		struct layoutdriver_io_operations *io_ops = PNFS_LD_IO_OPS(lo);
		struct nfs_inode *nfsi = PNFS_NFS_INODE(lo);

		dprintk("%s: freeing layout cache %p\n", __func__, lo);
		WARN_ON(!list_empty(&lo->layouts));
		io_ops->free_layout(lo);
		nfsi->layout = NULL;
	}
}

void
put_layout(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	put_layout_locked(NFS_I(inode)->layout);
	spin_unlock(&inode->i_lock);

}

void
pnfs_destroy_layout(struct nfs_inode *nfsi)
{
	struct pnfs_layout_hdr *lo;
	struct pnfs_layout_range range = {
		.iomode = IOMODE_ANY,
		.offset = 0,
		.length = NFS4_MAX_UINT64,
	};

	spin_lock(&nfsi->vfs_inode.i_lock);
	lo = nfsi->layout;
	if (lo) {
		pnfs_free_layout(lo, &range);
		WARN_ON(!list_empty(&nfsi->layout->segs));
		WARN_ON(!list_empty(&nfsi->layout->layouts));

		if (nfsi->layout->refcount != 1)
			printk(KERN_WARNING "%s: layout refcount not=1 %d\n",
				__func__, nfsi->layout->refcount);
		WARN_ON(nfsi->layout->refcount != 1);

		/* Matched by refcount set to 1 in alloc_init_layout */
		put_layout_locked(lo);
	}
	spin_unlock(&nfsi->vfs_inode.i_lock);
}

/*
 * Called by the state manger to remove all layouts established under an
 * expired lease.
 */
void
pnfs_destroy_all_layouts(struct nfs_client *clp)
{
	struct pnfs_layout_hdr *lo;

	while (!list_empty(&clp->cl_layouts)) {
		lo = list_entry(clp->cl_layouts.next, struct pnfs_layout_hdr,
				layouts);
		dprintk("%s freeing layout for inode %lu\n", __func__,
			lo->inode->i_ino);
		pnfs_destroy_layout(NFS_I(lo->inode));
	}
}

void
pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo,
			const nfs4_stateid *stateid)
{
	write_seqlock(&lo->seqlock);
	memcpy(lo->stateid.u.data, stateid->u.data, sizeof(lo->stateid.u.data));
	write_sequnlock(&lo->seqlock);
}

void
pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_hdr *lo)
{
	int seq;

	dprintk("--> %s\n", __func__);

	do {
		seq = read_seqbegin(&lo->seqlock);
		memcpy(dst->u.data, lo->stateid.u.data,
		       sizeof(lo->stateid.u.data));
	} while (read_seqretry(&lo->seqlock, seq));

	dprintk("<-- %s\n", __func__);
}

static void
pnfs_layout_from_open_stateid(struct pnfs_layout_hdr *lo,
			      struct nfs4_state *state)
{
	int seq;

	dprintk("--> %s\n", __func__);

	write_seqlock(&lo->seqlock);
	if (!memcmp(lo->stateid.u.data, &zero_stateid, NFS4_STATEID_SIZE))
		do {
			seq = read_seqbegin(&state->seqlock);
			memcpy(lo->stateid.u.data, state->stateid.u.data,
					sizeof(state->stateid.u.data));
		} while (read_seqretry(&state->seqlock, seq));
	write_sequnlock(&lo->seqlock);
	dprintk("<-- %s\n", __func__);
}

static void
pnfs_free_layout(struct pnfs_layout_hdr *lo,
		 struct pnfs_layout_range *range)
{
	dprintk("%s:Begin lo %p offset %llu length %llu iomode %d\n",
		__func__, lo, range->offset, range->length, range->iomode);

	if (list_empty(&lo->segs)) {
		struct nfs_client *clp;

		clp = PNFS_NFS_SERVER(lo)->nfs_client;
		spin_lock(&clp->cl_lock);
		list_del_init(&lo->layouts);
		spin_unlock(&clp->cl_lock);
		pnfs_set_layout_stateid(lo, &zero_stateid);
	}

	dprintk("%s:Return\n", __func__);
}

/*
 * Each layoutdriver embeds pnfs_layout_hdr as the first field in it's
 * per-layout type layout cache structure and returns it ZEROed
 * from layoutdriver_io_ops->alloc_layout
 */
static struct pnfs_layout_hdr *
alloc_init_layout(struct inode *ino)
{
	struct pnfs_layout_hdr *lo;
	struct layoutdriver_io_operations *io_ops;

	io_ops = NFS_SERVER(ino)->pnfs_curr_ld->ld_io_ops;
	lo = io_ops->alloc_layout(ino);
	if (!lo) {
		printk(KERN_ERR
			"%s: out of memory: io_ops->alloc_layout failed\n",
			__func__);
		return NULL;
	}
	lo->refcount = 1;
	INIT_LIST_HEAD(&lo->layouts);
	INIT_LIST_HEAD(&lo->segs);
	seqlock_init(&lo->seqlock);
	lo->inode = ino;
	return lo;
}

/*
 * Retrieve and possibly allocate the inode layout
 *
 * ino->i_lock must be taken by the caller.
 */
static struct pnfs_layout_hdr *
pnfs_alloc_layout(struct inode *ino)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_hdr *new = NULL;

	dprintk("%s Begin ino=%p layout=%p\n", __func__, ino, nfsi->layout);

	BUG_ON_UNLOCKED_INO(ino);
	if (likely(nfsi->layout))
		return nfsi->layout;

	spin_unlock(&ino->i_lock);
	new = alloc_init_layout(ino);
	spin_lock(&ino->i_lock);

	if (likely(nfsi->layout == NULL)) {	/* Won the race? */
		nfsi->layout = new;
	} else if (new) {
		/* Reference the layout accross i_lock release and grab */
		get_layout(nfsi->layout);
		spin_unlock(&ino->i_lock);
		NFS_SERVER(ino)->pnfs_curr_ld->ld_io_ops->free_layout(new);
		spin_lock(&ino->i_lock);
		put_layout_locked(nfsi->layout);
	}
	return nfsi->layout;
}

/* Callback operations for layout drivers.
 */
struct pnfs_client_operations pnfs_ops = {
	.nfs_getdeviceinfo = nfs4_proc_getdeviceinfo,
};

EXPORT_SYMBOL(pnfs_unregister_layoutdriver);
EXPORT_SYMBOL(pnfs_register_layoutdriver);


/* Device ID cache. Supports one layout type per struct nfs_client */
int
nfs4_alloc_init_deviceid_cache(struct nfs_client *clp,
			 void (*free_callback)(struct kref *))
{
	struct nfs4_deviceid_cache *c;

	c = kzalloc(sizeof(struct nfs4_deviceid_cache), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	spin_lock(&clp->cl_lock);
	if (clp->cl_devid_cache != NULL) {
		kref_get(&clp->cl_devid_cache->dc_kref);
		spin_unlock(&clp->cl_lock);
		dprintk("%s [kref [%d]]\n", __func__,
			atomic_read(&clp->cl_devid_cache->dc_kref.refcount));
		kfree(c);
	} else {
		int i;

		spin_lock_init(&c->dc_lock);
		for (i = 0; i < NFS4_DEVICE_ID_HASH_SIZE ; i++)
			INIT_HLIST_HEAD(&c->dc_deviceids[i]);
		kref_init(&c->dc_kref);
		c->dc_free_callback = free_callback;
		clp->cl_devid_cache = c;
		spin_unlock(&clp->cl_lock);
		dprintk("%s [new]\n", __func__);
	}
	return 0;
}
EXPORT_SYMBOL(nfs4_alloc_init_deviceid_cache);

void
nfs4_init_deviceid_node(struct nfs4_deviceid *d)
{
	INIT_HLIST_NODE(&d->de_node);
	kref_init(&d->de_kref);
}
EXPORT_SYMBOL(nfs4_init_deviceid_node);

struct nfs4_deviceid *
nfs4_find_deviceid(struct nfs4_deviceid_cache *c, struct pnfs_deviceid *id)
{
	struct nfs4_deviceid *d;
	struct hlist_node *n;
	long hash = nfs4_deviceid_hash(id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	rcu_read_lock();
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		if (!memcmp(&d->de_id, id, NFS4_PNFS_DEVICEID4_SIZE)) {
			rcu_read_unlock();
			return d;
		}
	}
	rcu_read_unlock();
	return NULL;
}
EXPORT_SYMBOL(nfs4_find_deviceid);

/*
 * Add or kref_get a deviceid.
 * GETDEVICEINFOs for same deviceid can race. If deviceid is found, discard new
 */
struct nfs4_deviceid *
nfs4_add_deviceid(struct nfs4_deviceid_cache *c, struct nfs4_deviceid *new)
{
	struct nfs4_deviceid *d;
	struct hlist_node *n;
	long hash = nfs4_deviceid_hash(&new->de_id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	spin_lock(&c->dc_lock);
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		if (!memcmp(&d->de_id, &new->de_id, NFS4_PNFS_DEVICEID4_SIZE)) {
			spin_unlock(&c->dc_lock);
			dprintk("%s [discard]\n", __func__);
			c->dc_free_callback(&new->de_kref);
			return d;
		}
	}
	hlist_add_head_rcu(&new->de_node, &c->dc_deviceids[hash]);
	spin_unlock(&c->dc_lock);
	dprintk("%s [new]\n", __func__);
	return new;
}
EXPORT_SYMBOL(nfs4_add_deviceid);

static int
nfs4_remove_deviceid(struct nfs4_deviceid_cache *c, long hash)
{
	struct nfs4_deviceid *d;
	struct hlist_node *n;

	dprintk("--> %s hash %ld\n", __func__, hash);
	spin_lock(&c->dc_lock);
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		hlist_del_rcu(&d->de_node);
		spin_unlock(&c->dc_lock);
		synchronize_rcu();
		dprintk("%s [%d]\n", __func__,
			atomic_read(&d->de_kref.refcount));
		kref_put(&d->de_kref, c->dc_free_callback);
		return 1;
	}
	spin_unlock(&c->dc_lock);
	return 0;
}

static void
nfs4_free_deviceid_cache(struct kref *kref)
{
	struct nfs4_deviceid_cache *cache =
		container_of(kref, struct nfs4_deviceid_cache, dc_kref);
	long i;

	for (i = 0; i < NFS4_DEVICE_ID_HASH_SIZE; i++)
		while (nfs4_remove_deviceid(cache, i))
			;
	kfree(cache);
}

void
nfs4_put_deviceid_cache(struct nfs_client *clp)
{
	struct nfs4_deviceid_cache *tmp = clp->cl_devid_cache;
	int refcount;

	dprintk("--> %s cl_devid_cache %p\n", __func__, clp->cl_devid_cache);
	spin_lock(&clp->cl_lock);
	refcount = atomic_read(&clp->cl_devid_cache->dc_kref.refcount);
	if (refcount == 1)
		clp->cl_devid_cache = NULL;
	spin_unlock(&clp->cl_lock);
	dprintk("%s [%d]\n", __func__, refcount);
	kref_put(&tmp->dc_kref, nfs4_free_deviceid_cache);
}
EXPORT_SYMBOL(nfs4_put_deviceid_cache);
