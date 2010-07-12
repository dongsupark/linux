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
static struct kmem_cache *pnfs_cachep;
static mempool_t *pnfs_layoutcommit_mempool;

static inline struct nfs4_layoutcommit_data *pnfs_layoutcommit_alloc(void)
{
	struct nfs4_layoutcommit_data *p =
			mempool_alloc(pnfs_layoutcommit_mempool, GFP_NOFS);
	if (p)
		memset(p, 0, sizeof(*p));

	return p;
}

void pnfs_layoutcommit_free(struct nfs4_layoutcommit_data *p)
{
	mempool_free(p, pnfs_layoutcommit_mempool);
}

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

	pnfs_cachep = kmem_cache_create("nfs4_layoutcommit_data",
					sizeof(struct nfs4_layoutcommit_data),
					0, SLAB_HWCACHE_ALIGN, NULL);
	if (pnfs_cachep == NULL)
		return -ENOMEM;

	pnfs_layoutcommit_mempool = mempool_create(MIN_POOL_LC,
						   mempool_alloc_slab,
						   mempool_free_slab,
						   pnfs_cachep);
	if (pnfs_layoutcommit_mempool == NULL) {
		kmem_cache_destroy(pnfs_cachep);
		return -ENOMEM;
	}

	pnfs_initialized = 1;
	return 0;
}

void pnfs_uninitialize(void)
{
	mempool_destroy(pnfs_layoutcommit_mempool);
	kmem_cache_destroy(pnfs_cachep);
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

/* Set cred to indicate we require a layoutcommit
 * If we don't even have a layout, we don't need to commit it.
 */
void
pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx)
{
	dprintk("%s: has_layout=%d ctx=%p\n", __func__, has_layout(nfsi), ctx);
	spin_lock(&nfsi->vfs_inode.i_lock);
	if (has_layout(nfsi) &&
	    !test_bit(NFS_INO_LAYOUTCOMMIT, &nfsi->layout->state)) {
		nfsi->layout->cred = get_rpccred(ctx->state->owner->so_cred);
		__set_bit(NFS_INO_LAYOUTCOMMIT,
			  &nfsi->layout->state);
		nfsi->change_attr++;
		spin_unlock(&nfsi->vfs_inode.i_lock);
		dprintk("%s: Set layoutcommit\n", __func__);
		return;
	}
	spin_unlock(&nfsi->vfs_inode.i_lock);
}

/* Update last_write_offset for layoutcommit.
 * TODO: We should only use commited extents, but the current nfs
 * implementation does not calculate the written range in nfs_commit_done.
 * We therefore update this field in writeback_done.
 */
void
pnfs_update_last_write(struct nfs_inode *nfsi, loff_t offset, size_t extent)
{
	loff_t end_pos;

	spin_lock(&nfsi->vfs_inode.i_lock);
	if (offset < nfsi->layout->write_begin_pos)
		nfsi->layout->write_begin_pos = offset;
	end_pos = offset + extent - 1; /* I'm being inclusive */
	if (end_pos > nfsi->layout->write_end_pos)
		nfsi->layout->write_end_pos = end_pos;
	dprintk("%s: Wrote %lu@%lu bpos %lu, epos: %lu\n",
		__func__,
		(unsigned long) extent,
		(unsigned long) offset ,
		(unsigned long) nfsi->layout->write_begin_pos,
		(unsigned long) nfsi->layout->write_end_pos);
	spin_unlock(&nfsi->vfs_inode.i_lock);
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

	if (!io_ops->alloc_lseg || !io_ops->free_lseg) {
		printk(KERN_ERR "%s Layout driver must provide "
		       "alloc_lseg and free_lseg.\n", __func__);
		return NULL;
	}

	if (!io_ops->read_pagelist || !io_ops->write_pagelist ||
	    !io_ops->commit) {
		printk(KERN_ERR "%s Layout driver must provide "
		       "read_pagelist, write_pagelist, and commit.\n",
		       __func__);
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
pnfs_layout_release(struct pnfs_layout_hdr *lo,
		    struct pnfs_layout_range *range)
{
	struct nfs_inode *nfsi = PNFS_NFS_INODE(lo);

	spin_lock(&nfsi->vfs_inode.i_lock);
	if (range)
		pnfs_free_layout(lo, range);
	/*
	 * Matched in _pnfs_update_layout for layoutget
	 * and by get_layout in _pnfs_return_layout for layoutreturn
	 */
	put_layout_locked(lo);
	spin_unlock(&nfsi->vfs_inode.i_lock);
	wake_up_all(&nfsi->lo_waitq);
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

static inline void
init_lseg(struct pnfs_layout_hdr *lo, struct pnfs_layout_segment *lseg)
{
	INIT_LIST_HEAD(&lseg->fi_list);
	kref_init(&lseg->kref);
	lseg->valid = true;
	lseg->layout = lo;
}

static void
destroy_lseg(struct kref *kref)
{
	struct pnfs_layout_segment *lseg =
		container_of(kref, struct pnfs_layout_segment, kref);

	dprintk("--> %s\n", __func__);
	/* Matched by get_layout in pnfs_insert_layout */
	put_layout_locked(lseg->layout);
	PNFS_LD_IO_OPS(lseg->layout)->free_lseg(lseg);
}

static void
put_lseg_locked(struct pnfs_layout_segment *lseg)
{
	bool do_wake_up;
	struct nfs_inode *nfsi;

	if (!lseg)
		return;

	dprintk("%s: lseg %p ref %d valid %d\n", __func__, lseg,
		atomic_read(&lseg->kref.refcount), lseg->valid);
	do_wake_up = !lseg->valid;
	nfsi = PNFS_NFS_INODE(lseg->layout);
	kref_put(&lseg->kref, destroy_lseg);
	if (do_wake_up)
		wake_up(&nfsi->lo_waitq);
}

void
put_lseg(struct pnfs_layout_segment *lseg)
{
	bool do_wake_up;
	struct nfs_inode *nfsi;

	if (!lseg)
		return;

	dprintk("%s: lseg %p ref %d valid %d\n", __func__, lseg,
		atomic_read(&lseg->kref.refcount), lseg->valid);
	do_wake_up = !lseg->valid;
	nfsi = PNFS_NFS_INODE(lseg->layout);
	spin_lock(&nfsi->vfs_inode.i_lock);
	kref_put(&lseg->kref, destroy_lseg);
	spin_unlock(&nfsi->vfs_inode.i_lock);
	if (do_wake_up)
		wake_up(&nfsi->lo_waitq);
}
EXPORT_SYMBOL(put_lseg);

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

/*
* Get layout from server.
*    for now, assume that whole file layouts are requested.
*    arg->offset: 0
*    arg->length: all ones
*/
static int
send_layoutget(struct inode *ino,
	   struct nfs_open_context *ctx,
	   struct pnfs_layout_range *range,
	   struct pnfs_layout_segment **lsegpp,
	   struct pnfs_layout_hdr *lo)
{
	int status;
	struct nfs_server *server = NFS_SERVER(ino);
	struct nfs4_layoutget *lgp;

	dprintk("--> %s\n", __func__);

	lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
	if (lgp == NULL) {
		pnfs_layout_release(lo, NULL);
		return -ENOMEM;
	}
	lgp->args.minlength = NFS4_MAX_UINT64;
	lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;
	lgp->args.range.iomode = range->iomode;
	lgp->args.range.offset = 0;
	lgp->args.range.length = NFS4_MAX_UINT64;
	lgp->args.type = server->pnfs_curr_ld->id;
	lgp->args.inode = ino;
	lgp->lsegpp = lsegpp;

	if (!memcmp(lo->stateid.u.data, &zero_stateid, NFS4_STATEID_SIZE)) {
		struct nfs_open_context *oldctx = ctx;

		if (!oldctx) {
			ctx = nfs_find_open_context(ino, NULL,
					(range->iomode == IOMODE_READ) ?
					FMODE_READ: FMODE_WRITE);
			BUG_ON(!ctx);
		}
		/* Set the layout stateid from the open stateid */
		pnfs_layout_from_open_stateid(NFS_I(ino)->layout, ctx->state);
		if (!oldctx)
			put_nfs_open_context(ctx);
	}

	/* Retrieve layout information from server */
	status = nfs4_proc_layoutget(lgp);

	dprintk("<-- %s status %d\n", __func__, status);
	return status;
}

/*
 * iomode matching rules:
 * range	lseg	match
 * -----	-----	-----
 * ANY		READ	true
 * ANY		RW	true
 * RW		READ	false
 * RW		RW	true
 * READ		READ	true
 * READ		RW	false
 */
static inline int
should_free_lseg(struct pnfs_layout_segment *lseg,
		   struct pnfs_layout_range *range)
{
	return (range->iomode == IOMODE_ANY ||
		lseg->range.iomode == range->iomode);
}

static struct pnfs_layout_segment *
has_layout_to_return(struct pnfs_layout_hdr *lo,
		     struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *out = NULL, *lseg;
	dprintk("%s:Begin lo %p offset %llu length %llu iomode %d\n",
		__func__, lo, range->offset, range->length, range->iomode);

	BUG_ON_UNLOCKED_LO(lo);
	list_for_each_entry (lseg, &lo->segs, fi_list)
		if (should_free_lseg(lseg, range)) {
			out = lseg;
			break;
		}

	dprintk("%s:Return lseg=%p\n", __func__, out);
	return out;
}

static inline bool
_pnfs_can_return_lseg(struct pnfs_layout_segment *lseg)
{
	return atomic_read(&lseg->kref.refcount) == 1;
}


static void
pnfs_free_layout(struct pnfs_layout_hdr *lo,
		 struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg, *next;
	dprintk("%s:Begin lo %p offset %llu length %llu iomode %d\n",
		__func__, lo, range->offset, range->length, range->iomode);

	BUG_ON_UNLOCKED_LO(lo);
	list_for_each_entry_safe (lseg, next, &lo->segs, fi_list) {
		if (!should_free_lseg(lseg, range) ||
		    !_pnfs_can_return_lseg(lseg))
			continue;
		dprintk("%s: freeing lseg %p iomode %d "
			"offset %llu length %llu\n", __func__,
			lseg, lseg->range.iomode, lseg->range.offset,
			lseg->range.length);
		list_del(&lseg->fi_list);
		put_lseg_locked(lseg);
	}
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

static bool
pnfs_return_layout_barrier(struct nfs_inode *nfsi,
			   struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg;
	bool ret = false;

	spin_lock(&nfsi->vfs_inode.i_lock);
	list_for_each_entry(lseg, &nfsi->layout->segs, fi_list) {
		if (!should_free_lseg(lseg, range))
			continue;
		lseg->valid = false;
		if (!_pnfs_can_return_lseg(lseg)) {
			dprintk("%s: wait on lseg %p refcount %d\n",
				__func__, lseg,
				atomic_read(&lseg->kref.refcount));
			ret = true;
		}
	}
	spin_unlock(&nfsi->vfs_inode.i_lock);
	dprintk("%s:Return %d\n", __func__, ret);
	return ret;
}

static int
return_layout(struct inode *ino, struct pnfs_layout_range *range,
	      enum pnfs_layoutreturn_type type, struct pnfs_layout_hdr *lo,
	      bool wait)
{
	struct nfs4_layoutreturn *lrp;
	struct nfs_server *server = NFS_SERVER(ino);
	int status = -ENOMEM;

	dprintk("--> %s\n", __func__);

	BUG_ON(type != RETURN_FILE);

	lrp = kzalloc(sizeof(*lrp), GFP_KERNEL);
	if (lrp == NULL) {
		if (lo && (type == RETURN_FILE))
			pnfs_layout_release(lo, NULL);
		goto out;
	}
	lrp->args.reclaim = 0;
	lrp->args.layout_type = server->pnfs_curr_ld->id;
	lrp->args.return_type = type;
	lrp->args.range = *range;
	lrp->args.inode = ino;

	status = nfs4_proc_layoutreturn(lrp, wait);
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
}

int
_pnfs_return_layout(struct inode *ino, struct pnfs_layout_range *range,
		    const nfs4_stateid *stateid, /* optional */
		    enum pnfs_layoutreturn_type type,
		    bool wait)
{
	struct pnfs_layout_hdr *lo = NULL;
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_range arg;
	int status = 0;

	dprintk("--> %s type %d\n", __func__, type);


	arg.iomode = range ? range->iomode : IOMODE_ANY;
	arg.offset = 0;
	arg.length = NFS4_MAX_UINT64;

	if (type == RETURN_FILE) {
		spin_lock(&ino->i_lock);
		lo = nfsi->layout;
		if (lo && !has_layout_to_return(lo, &arg)) {
			lo = NULL;
		}
		if (!lo) {
			spin_unlock(&ino->i_lock);
			dprintk("%s: no layout segments to return\n", __func__);
			goto out;
		}

		/* Reference for layoutreturn matched in pnfs_layout_release */
		get_layout(lo);

		spin_unlock(&ino->i_lock);

		if (pnfs_return_layout_barrier(nfsi, &arg)) {
			if (stateid) { /* callback */
				status = -EAGAIN;
				goto out_put;
			}
			dprintk("%s: waiting\n", __func__);
			wait_event(nfsi->lo_waitq,
				   !pnfs_return_layout_barrier(nfsi, &arg));
		}

		if (layoutcommit_needed(nfsi)) {
			if (stateid && !wait) { /* callback */
				dprintk("%s: layoutcommit pending\n", __func__);
				status = -EAGAIN;
				goto out_put;
			}
			status = pnfs_layoutcommit_inode(ino, wait);
			if (status) {
				/* Return layout even if layoutcommit fails */
				dprintk("%s: layoutcommit failed, status=%d. "
					"Returning layout anyway\n",
					__func__, status);
			}
		}

		if (!stateid)
			status = return_layout(ino, &arg, type, lo, wait);
		else
			pnfs_layout_release(lo, &arg);
	}
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
out_put:
	put_layout(ino);
	goto out;
}

/*
 * cmp two layout segments for sorting into layout cache
 */
static inline s64
cmp_layout(struct pnfs_layout_range *l1,
	   struct pnfs_layout_range *l2)
{
	/* read > read/write */
	return (int)(l1->iomode == IOMODE_READ) -
	(int)(l2->iomode == IOMODE_READ);
}

static void
pnfs_insert_layout(struct pnfs_layout_hdr *lo,
		   struct pnfs_layout_segment *lseg)
{
	struct pnfs_layout_segment *lp;
	int found = 0;

	dprintk("%s:Begin\n", __func__);

	BUG_ON_UNLOCKED_LO(lo);
	if (list_empty(&lo->segs)) {
		struct nfs_client *clp = PNFS_NFS_SERVER(lo)->nfs_client;

		spin_lock(&clp->cl_lock);
		BUG_ON(!list_empty(&lo->layouts));
		list_add_tail(&lo->layouts, &clp->cl_layouts);
		spin_unlock(&clp->cl_lock);
	}
	list_for_each_entry (lp, &lo->segs, fi_list) {
		if (cmp_layout(&lp->range, &lseg->range) > 0)
			continue;
		list_add_tail(&lseg->fi_list, &lp->fi_list);
		dprintk("%s: inserted lseg %p "
			"iomode %d offset %llu length %llu before "
			"lp %p iomode %d offset %llu length %llu\n",
			__func__, lseg, lseg->range.iomode,
			lseg->range.offset, lseg->range.length,
			lp, lp->range.iomode, lp->range.offset,
			lp->range.length);
		found = 1;
		break;
	}
	if (!found) {
		list_add_tail(&lseg->fi_list, &lo->segs);
		dprintk("%s: inserted lseg %p "
			"iomode %d offset %llu length %llu at tail\n",
			__func__, lseg, lseg->range.iomode,
			lseg->range.offset, lseg->range.length);
	}
	get_layout(lo);

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

/*
 * iomode matching rules:
 * range	lseg	match
 * -----	-----	-----
 * ANY		READ	true
 * ANY		RW	true
 * RW		READ	false
 * RW		RW	true
 * READ		READ	true
 * READ		RW	true
 */
static inline int
has_matching_lseg(struct pnfs_layout_segment *lseg,
		  struct pnfs_layout_range *range)
{
	return (range->iomode != IOMODE_RW || lseg->range.iomode == IOMODE_RW);
}

/*
 * lookup range in layout
 */
static struct pnfs_layout_segment *
pnfs_has_layout(struct pnfs_layout_hdr *lo,
		struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg, *ret = NULL;

	dprintk("%s:Begin\n", __func__);

	BUG_ON_UNLOCKED_LO(lo);
	list_for_each_entry (lseg, &lo->segs, fi_list) {
		if (has_matching_lseg(lseg, range)) {
			ret = lseg;
			get_lseg(ret);
			break;
		}
		if (cmp_layout(range, &lseg->range) > 0)
			break;
	}

	dprintk("%s:Return lseg %p ref %d valid %d\n",
		__func__, ret, ret ? atomic_read(&ret->kref.refcount) : 0,
		ret ? ret->valid : 0);
	return ret;
}

/* Update the file's layout for the given range and iomode.
 * Layout is retreived from the server if needed.
 * The appropriate layout segment is referenced and returned to the caller.
 */
void
_pnfs_update_layout(struct inode *ino,
		   struct nfs_open_context *ctx,
		   loff_t pos,
		   u64 count,
		   enum pnfs_iomode iomode,
		   struct pnfs_layout_segment **lsegpp)
{
	struct pnfs_layout_range arg = {
		.iomode = iomode,
		.offset = 0,
		.length = NFS4_MAX_UINT64,
	};
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_hdr *lo;
	struct pnfs_layout_segment *lseg = NULL;

	*lsegpp = NULL;
	spin_lock(&ino->i_lock);
	lo = pnfs_alloc_layout(ino);
	if (lo == NULL) {
		dprintk("%s ERROR: can't get pnfs_layout_hdr\n", __func__);
		goto out_unlock;
	}

	/* Check to see if the layout for the given range already exists */
	lseg = pnfs_has_layout(lo, &arg);
	if (lseg && !lseg->valid) {
		put_lseg_locked(lseg);
		/* someone is cleaning the layout */
		lseg = NULL;
		goto out_unlock;
	}

	if (lseg) {
		dprintk("%s: Using cached lseg %p for %llu@%llu iomode %d)\n",
			__func__,
			lseg,
			arg.length,
			arg.offset,
			arg.iomode);

		goto out_unlock;
	}

	/* if get layout already failed once goto out */
	if (test_bit(lo_fail_bit(iomode), &nfsi->layout->state)) {
		if (unlikely(nfsi->pnfs_layout_suspend &&
		    get_seconds() >= nfsi->pnfs_layout_suspend)) {
			dprintk("%s: layout_get resumed\n", __func__);
			clear_bit(lo_fail_bit(iomode),
				  &nfsi->layout->state);
			nfsi->pnfs_layout_suspend = 0;
		} else
			goto out_unlock;
	}

	/* Reference the layout for layoutget matched in pnfs_layout_release */
	get_layout(lo);
	spin_unlock(&ino->i_lock);

	send_layoutget(ino, ctx, &arg, lsegpp, lo);
out:
	dprintk("%s end, state 0x%lx lseg %p\n", __func__,
		nfsi->layout->state, lseg);
	return;
out_unlock:
	*lsegpp = lseg;
	spin_unlock(&ino->i_lock);
	goto out;
}

void
pnfs_get_layout_done(struct nfs4_layoutget *lgp, int rpc_status)
{
	struct pnfs_layout_segment *lseg = NULL;
	struct nfs_inode *nfsi = NFS_I(lgp->args.inode);
	time_t suspend = 0;

	dprintk("-->%s\n", __func__);

	lgp->status = rpc_status;
	if (likely(!rpc_status)) {
		if (unlikely(lgp->res.layout.len < 0)) {
			printk(KERN_ERR
			       "%s: ERROR Returned layout size is ZERO\n", __func__);
			lgp->status = -EIO;
		}
		goto out;
	}

	dprintk("%s: ERROR retrieving layout %d\n", __func__, rpc_status);
	switch (rpc_status) {
	case -NFS4ERR_BADLAYOUT:
		lgp->status = -ENOENT;
		/* FALLTHROUGH */
	case -EACCES:	/* NFS4ERR_ACCESS */
		/* transient error, don't mark with NFS_INO_LAYOUT_FAILED */
		goto out;

	case -NFS4ERR_LAYOUTTRYLATER:
	case -NFS4ERR_RECALLCONFLICT:
	case -NFS4ERR_OLD_STATEID:
	case -EAGAIN:	/* NFS4ERR_LOCKED */
		lgp->status = -NFS4ERR_DELAY;	/* for nfs4_handle_exception */
		/* FALLTHROUGH */
	case -NFS4ERR_GRACE:
	case -NFS4ERR_DELAY:
		goto out;

	case -NFS4ERR_ADMIN_REVOKED:
	case -NFS4ERR_DELEG_REVOKED:
		/* The layout is expected to be returned at this point.
		 * This should clear the layout stateid as well */
		suspend = get_seconds() + 1;
		break;

	case -NFS4ERR_LAYOUTUNAVAILABLE:
		lgp->status = -ENOTSUPP;
		break;

	case -NFS4ERR_REP_TOO_BIG:
	case -NFS4ERR_REP_TOO_BIG_TO_CACHE:
		lgp->status = -E2BIG;
		break;

	/* Leave the following errors untranslated */
	case -NFS4ERR_DEADSESSION:
	case -NFS4ERR_DQUOT:
	case -EINVAL:		/* NFS4ERR_INVAL */
	case -EIO:		/* NFS4ERR_IO */
	case -NFS4ERR_FHEXPIRED:
	case -NFS4ERR_MOVED:
	case -NFS4ERR_NOSPC:
	case -ESERVERFAULT:	/* NFS4ERR_SERVERFAULT */
	case -ESTALE:		/* NFS4ERR_STALE */
	case -ETOOSMALL:	/* NFS4ERR_TOOSMALL */
		break;

	/* The following errors are our fault and should never happen */
	case -NFS4ERR_BADIOMODE:
	case -NFS4ERR_BADXDR:
	case -NFS4ERR_REQ_TOO_BIG:
	case -NFS4ERR_UNKNOWN_LAYOUTTYPE:
	case -NFS4ERR_WRONG_TYPE:
		lgp->status = -EINVAL;
		/* FALLTHROUGH */
	case -NFS4ERR_BAD_STATEID:
	case -NFS4ERR_NOFILEHANDLE:
	case -ENOTSUPP:	/* NFS4ERR_NOTSUPP */
	case -NFS4ERR_OPENMODE:
	case -NFS4ERR_OP_NOT_IN_SESSION:
	case -NFS4ERR_TOO_MANY_OPS:
		dprintk("%s: error %d: should never happen\n", __func__,
			rpc_status);
		break;

	/* The following errors are the server's fault */
	default:
		dprintk("%s: illegal error %d\n", __func__, rpc_status);
		lgp->status = -EIO;
		break;
	}

	/* remember that get layout failed and suspend trying */
	nfsi->pnfs_layout_suspend = suspend;
	set_bit(lo_fail_bit(lgp->args.range.iomode),
		&nfsi->layout->state);
	dprintk("%s: layout_get suspended until %ld\n",
		__func__, suspend);
out:
	dprintk("%s end (err:%d) state 0x%lx lseg %p\n",
		__func__, lgp->status, nfsi->layout->state, lseg);
	return;
}

int
pnfs_layout_process(struct nfs4_layoutget *lgp)
{
	struct pnfs_layout_hdr *lo = NFS_I(lgp->args.inode)->layout;
	struct nfs4_layoutget_res *res = &lgp->res;
	struct pnfs_layout_segment *lseg;
	struct inode *ino = PNFS_INODE(lo);
	int status = 0;

	/* Inject layout blob into I/O device driver */
	lseg = PNFS_LD_IO_OPS(lo)->alloc_lseg(lo, res);
	if (!lseg || IS_ERR(lseg)) {
		if (!lseg)
			status = -ENOMEM;
		else
			status = PTR_ERR(lseg);
		dprintk("%s: Could not allocate layout: error %d\n",
		       __func__, status);
		goto out;
	}

	spin_lock(&ino->i_lock);
	init_lseg(lo, lseg);
	lseg->range = res->range;
	if (lgp->lsegpp) {
		get_lseg(lseg);
		*lgp->lsegpp = lseg;
	}
	pnfs_insert_layout(lo, lseg);

	if (res->return_on_close) {
		lo->roc_iomode |= res->range.iomode;
		if (!lo->roc_iomode)
			lo->roc_iomode = IOMODE_ANY;
	}

	/* Done processing layoutget. Set the layout stateid */
	pnfs_set_layout_stateid(lo, &res->stateid);
	spin_unlock(&ino->i_lock);
out:
	return status;
}

void
readahead_range(struct inode *inode, struct list_head *pages, loff_t *offset,
		size_t *count)
{
	struct page *first, *last;
	loff_t foff, i_size = i_size_read(inode);
	pgoff_t end_index = (i_size - 1) >> PAGE_CACHE_SHIFT;
	size_t range;


	first = list_entry((pages)->prev, struct page, lru);
	last = list_entry((pages)->next, struct page, lru);

	foff = (loff_t)first->index << PAGE_CACHE_SHIFT;

	range = (last->index - first->index) * PAGE_CACHE_SIZE;
	if (last->index == end_index)
		range += ((i_size - 1) & ~PAGE_CACHE_MASK) + 1;
	else
		range += PAGE_CACHE_SIZE;
	dprintk("%s foff %lu, range %Zu\n", __func__, (unsigned long)foff,
		range);
	*offset = foff;
	*count = range;
}

void
pnfs_set_pg_test(struct inode *inode, struct nfs_pageio_descriptor *pgio)
{
	struct pnfs_layout_hdr *lo;
	struct pnfs_layoutdriver_type *ld;

	pgio->pg_test = NULL;

	lo = NFS_I(inode)->layout;
	ld = NFS_SERVER(inode)->pnfs_curr_ld;
	if (!pnfs_enabled_sb(NFS_SERVER(inode)) || !lo)
		return;

	if (ld->ld_policy_ops)
		pgio->pg_test = ld->ld_policy_ops->pg_test;
}

static u32
pnfs_getboundary(struct inode *inode)
{
	u32 stripe_size = 0;
	struct nfs_server *nfss = NFS_SERVER(inode);
	struct layoutdriver_policy_operations *policy_ops;

	if (!nfss->pnfs_curr_ld)
		goto out;

	policy_ops = nfss->pnfs_curr_ld->ld_policy_ops;
	if (!policy_ops || !policy_ops->get_stripesize)
		goto out;

	/* The default is to not gather across stripes */
	if (pnfs_ld_gather_across_stripes(nfss->pnfs_curr_ld))
		goto out;

	spin_lock(&inode->i_lock);
	if (NFS_I(inode)->layout)
		stripe_size = policy_ops->get_stripesize(NFS_I(inode)->layout);
	spin_unlock(&inode->i_lock);
out:
	return stripe_size;
}

/*
 * rsize is already set by caller to MDS rsize.
 */
void
pnfs_pageio_init_read(struct nfs_pageio_descriptor *pgio,
		  struct inode *inode,
		  struct nfs_open_context *ctx,
		  struct list_head *pages)
{
	struct nfs_server *nfss = NFS_SERVER(inode);
	size_t count = 0;
	loff_t loff;

	pgio->pg_iswrite = 0;
	pgio->pg_boundary = 0;
	pgio->pg_test = NULL;
	pgio->pg_lseg = NULL;

	if (!pnfs_enabled_sb(nfss))
		return;

	/* Calculate the total read-ahead count */
	readahead_range(inode, pages, &loff, &count);

	if (count > 0) {
		_pnfs_update_layout(inode, ctx, loff, count, IOMODE_READ,
				    &pgio->pg_lseg);
		if (!pgio->pg_lseg)
			return;

		pgio->pg_boundary = pnfs_getboundary(inode);
		if (pgio->pg_boundary)
			pnfs_set_pg_test(inode, pgio);
	}
}

void
pnfs_pageio_init_write(struct nfs_pageio_descriptor *pgio, struct inode *inode)
{
	struct nfs_server *server = NFS_SERVER(inode);

	pgio->pg_iswrite = 1;
	if (!pnfs_enabled_sb(server)) {
		pgio->pg_boundary = 0;
		pgio->pg_test = NULL;
		return;
	}
	pgio->pg_boundary = pnfs_getboundary(inode);
	pnfs_set_pg_test(inode, pgio);
}

static void _pnfs_clear_lseg_from_pages(struct list_head *head)
{
	struct nfs_page *req;

	list_for_each_entry(req, head, wb_list) {
		put_lseg(req->wb_lseg);
		req->wb_lseg = NULL;
	}
}

/*
 * Call the appropriate parallel I/O subsystem write function.
 * If no I/O device driver exists, or one does match the returned
 * fstype, then return a positive status for regular NFS processing.
 *
 * TODO: Is wdata->how and wdata->args.stable always the same value?
 * TODO: It seems in NFS, the server may not do a stable write even
 * though it was requested (and vice-versa?).  To check, it looks
 * in data->res.verf->committed.  Do we need this ability
 * for non-file layout drivers?
 */
enum pnfs_try_status
pnfs_try_to_write_data(struct nfs_write_data *wdata,
			const struct rpc_call_ops *call_ops, int how)
{
	struct inode *inode = wdata->inode;
	enum pnfs_try_status trypnfs;
	struct nfs_server *nfss = NFS_SERVER(inode);
	struct pnfs_layout_segment *lseg = wdata->req->wb_lseg;

	wdata->pdata.call_ops = call_ops;
	wdata->pdata.how = how;

	dprintk("%s: Writing ino:%lu %u@%llu (how %d)\n", __func__,
		inode->i_ino, wdata->args.count, wdata->args.offset, how);

	get_lseg(lseg);

	wdata->pdata.lseg = lseg;
	trypnfs = nfss->pnfs_curr_ld->ld_io_ops->write_pagelist(wdata,
		nfs_page_array_len(wdata->args.pgbase, wdata->args.count),
								how);

	if (trypnfs == PNFS_NOT_ATTEMPTED) {
		wdata->pdata.lseg = NULL;
		put_lseg(lseg);
		_pnfs_clear_lseg_from_pages(&wdata->pages);
	} else {
		nfs_inc_stats(inode, NFSIOS_PNFS_WRITE);
	}
	dprintk("%s End (trypnfs:%d)\n", __func__, trypnfs);
	return trypnfs;
}

/*
 * Call the appropriate parallel I/O subsystem read function.
 * If no I/O device driver exists, or one does match the returned
 * fstype, then return a positive status for regular NFS processing.
 */
enum pnfs_try_status
pnfs_try_to_read_data(struct nfs_read_data *rdata,
		       const struct rpc_call_ops *call_ops)
{
	struct inode *inode = rdata->inode;
	struct nfs_server *nfss = NFS_SERVER(inode);
	struct pnfs_layout_segment *lseg = rdata->req->wb_lseg;
	enum pnfs_try_status trypnfs;

	rdata->pdata.call_ops = call_ops;

	dprintk("%s: Reading ino:%lu %u@%llu\n",
		__func__, inode->i_ino, rdata->args.count, rdata->args.offset);

	get_lseg(lseg);

	rdata->pdata.lseg = lseg;
	trypnfs = nfss->pnfs_curr_ld->ld_io_ops->read_pagelist(rdata,
		nfs_page_array_len(rdata->args.pgbase, rdata->args.count));
	if (trypnfs == PNFS_NOT_ATTEMPTED) {
		rdata->pdata.lseg = NULL;
		put_lseg(lseg);
		_pnfs_clear_lseg_from_pages(&rdata->pages);
	} else {
		nfs_inc_stats(inode, NFSIOS_PNFS_READ);
	}
	dprintk("%s End (trypnfs:%d)\n", __func__, trypnfs);
	return trypnfs;
}

enum pnfs_try_status
pnfs_try_to_commit(struct nfs_write_data *data,
		    const struct rpc_call_ops *call_ops, int sync)
{
	struct inode *inode = data->inode;
	struct nfs_server *nfss = NFS_SERVER(data->inode);
	enum pnfs_try_status trypnfs;

	dprintk("%s: Begin\n", __func__);

	/* We need to account for possibility that
	 * each nfs_page can point to a different lseg (or be NULL).
	 * For the immediate case of whole-file-only layouts, we at
	 * least know there can be only a single lseg.
	 * We still have to account for the possibility of some being NULL.
	 * This will be done by passing the buck to the layout driver.
	 */
	data->pdata.call_ops = call_ops;
	data->pdata.how = sync;
	data->pdata.lseg = NULL;
	trypnfs = nfss->pnfs_curr_ld->ld_io_ops->commit(data, sync);
	if (trypnfs == PNFS_NOT_ATTEMPTED)
		_pnfs_clear_lseg_from_pages(&data->pages);
	else
		nfs_inc_stats(inode, NFSIOS_PNFS_COMMIT);
	dprintk("%s End (trypnfs:%d)\n", __func__, trypnfs);
	return trypnfs;
}

/*
 * Set up the argument/result storage required for the RPC call.
 */
static int
pnfs_layoutcommit_setup(struct inode *inode,
			struct nfs4_layoutcommit_data *data,
			loff_t write_begin_pos, loff_t write_end_pos)
{
	struct nfs_server *nfss = NFS_SERVER(inode);
	int result = 0;

	dprintk("--> %s\n", __func__);

	data->args.inode = inode;
	data->args.fh = NFS_FH(inode);
	data->args.layout_type = nfss->pnfs_curr_ld->id;
	data->res.fattr = &data->fattr;
	nfs_fattr_init(&data->fattr);

	/* TODO: Need to determine the correct values */
	data->args.time_modify_changed = 0;

	/* Set values from inode so it can be reset
	 */
	data->args.range.iomode = IOMODE_RW;
	data->args.range.offset = write_begin_pos;
	data->args.range.length = write_end_pos - write_begin_pos + 1;
	data->args.lastbytewritten =  min(write_end_pos,
					  i_size_read(inode) - 1);
	data->args.bitmask = nfss->attr_bitmask;
	data->res.server = nfss;

	dprintk("<-- %s Status %d\n", __func__, result);
	return result;
}

/* Issue a async layoutcommit for an inode.
 */
int
pnfs_layoutcommit_inode(struct inode *inode, int sync)
{
	struct nfs4_layoutcommit_data *data;
	struct nfs_inode *nfsi = NFS_I(inode);
	loff_t write_begin_pos;
	loff_t write_end_pos;

	int status = 0;

	dprintk("%s Begin (sync:%d)\n", __func__, sync);

	BUG_ON(!has_layout(nfsi));

	data = pnfs_layoutcommit_alloc();
	if (!data)
		return -ENOMEM;

	spin_lock(&inode->i_lock);
	if (!layoutcommit_needed(nfsi)) {
		spin_unlock(&inode->i_lock);
		goto out_free;
	}

	/* Clear layoutcommit properties in the inode so
	 * new lc info can be generated
	 */
	write_begin_pos = nfsi->layout->write_begin_pos;
	write_end_pos = nfsi->layout->write_end_pos;
	data->cred = nfsi->layout->cred;
	nfsi->layout->write_begin_pos = 0;
	nfsi->layout->write_end_pos = 0;
	nfsi->layout->cred = NULL;
	__clear_bit(NFS_INO_LAYOUTCOMMIT, &nfsi->layout->state);
	pnfs_get_layout_stateid(&data->args.stateid, nfsi->layout);

	/* Reference for layoutcommit matched in pnfs_layoutcommit_release */
	get_layout(NFS_I(inode)->layout);

	spin_unlock(&inode->i_lock);

	/* Set up layout commit args */
	status = pnfs_layoutcommit_setup(inode, data, write_begin_pos,
					 write_end_pos);
	if (status) {
		/* The layout driver failed to setup the layoutcommit */
		put_rpccred(data->cred);
		put_layout(inode);
		goto out_free;
	}
	status = nfs4_proc_layoutcommit(data, sync);
out:
	dprintk("%s end (err:%d)\n", __func__, status);
	return status;
out_free:
	pnfs_layoutcommit_free(data);
	goto out;
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

/* Called from layoutdriver_io_operations->alloc_lseg */
void
nfs4_set_layout_deviceid(struct pnfs_layout_segment *l, struct nfs4_deviceid *d)
{
	dprintk("%s [%d]\n", __func__, atomic_read(&d->de_kref.refcount));
	l->deviceid = d;
}
EXPORT_SYMBOL(nfs4_set_layout_deviceid);

/* Called from layoutdriver_io_operations->free_lseg */
void
nfs4_put_unset_layout_deviceid(struct pnfs_layout_segment *l,
			   struct nfs4_deviceid *d,
			   void (*free_callback)(struct kref *))
{
	dprintk("%s [%d]\n", __func__, atomic_read(&d->de_kref.refcount));
	l->deviceid = NULL;
	kref_put(&d->de_kref, free_callback);
}
EXPORT_SYMBOL(nfs4_put_unset_layout_deviceid);

/* Find and reference a deviceid */
struct nfs4_deviceid *
nfs4_find_get_deviceid(struct nfs4_deviceid_cache *c, struct pnfs_deviceid *id)
{
	struct nfs4_deviceid *d;
	struct hlist_node *n;
	long hash = nfs4_deviceid_hash(id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	rcu_read_lock();
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		if (!memcmp(&d->de_id, id, NFS4_PNFS_DEVICEID4_SIZE)) {
			if (!atomic_inc_not_zero(&d->de_kref.refcount)) {
				goto fail;
			} else {
				rcu_read_unlock();
				return d;
			}
		}
	}
fail:
	rcu_read_unlock();
	return NULL;
}
EXPORT_SYMBOL(nfs4_find_get_deviceid);

/*
 * Add and kref_get a deviceid.
 * GETDEVICEINFOs for same deviceid can race. If deviceid is found, discard new
 */
struct nfs4_deviceid *
nfs4_add_get_deviceid(struct nfs4_deviceid_cache *c, struct nfs4_deviceid *new)
{
	struct nfs4_deviceid *d;
	struct hlist_node *n;
	long hash = nfs4_deviceid_hash(&new->de_id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	spin_lock(&c->dc_lock);
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		if (!memcmp(&d->de_id, &new->de_id, NFS4_PNFS_DEVICEID4_SIZE)) {
			kref_get(&d->de_kref);
			spin_unlock(&c->dc_lock);
			dprintk("%s [discard]\n", __func__);
			c->dc_free_callback(&new->de_kref);
			return d;
		}
	}
	hlist_add_head_rcu(&new->de_node, &c->dc_deviceids[hash]);
	kref_get(&new->de_kref);
	spin_unlock(&c->dc_lock);
	dprintk("%s [new]\n", __func__);
	return new;
}
EXPORT_SYMBOL(nfs4_add_get_deviceid);

/*
 * Remove the first deviceid from a hash bucket, or return 0 if bucket list
 * is empty.
 */
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
