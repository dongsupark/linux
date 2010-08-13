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

	if (!pnfs_initialized) {
		printk(KERN_ERR "%s Registration failure. "
		       "pNFS not initialized.\n", __func__);
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
