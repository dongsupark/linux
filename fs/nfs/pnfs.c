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
#include <linux/pnfs_xdr.h>
#include <linux/nfs4_pnfs.h>

#include "internal.h"
#include "nfs4_fs.h"
#include "pnfs.h"

#ifdef CONFIG_PNFS
#define NFSDBG_FACILITY		NFSDBG_PNFS

#define MIN_POOL_LC		(4)

static int pnfs_initialized;

static void pnfs_free_layout(struct pnfs_layout_type *lo,
			     struct nfs4_pnfs_layout_segment *range);

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

static inline struct pnfs_layoutcommit_data *pnfs_layoutcommit_alloc(void)
{
	struct pnfs_layoutcommit_data *p =
			mempool_alloc(pnfs_layoutcommit_mempool, GFP_NOFS);
	if (p)
		memset(p, 0, sizeof(*p));

	return p;
}

void pnfs_layoutcommit_free(struct pnfs_layoutcommit_data *p)
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

	pnfs_cachep = kmem_cache_create("pnfs_layoutcommit_data",
					sizeof(struct pnfs_layoutcommit_data),
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

	pnfs_v4_clientops_init();

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

/* Set context to indicate we require a layoutcommit
 * If we don't even have a layout, we don't need to commit it.
 */
void
pnfs_need_layoutcommit(struct nfs_inode *nfsi, struct nfs_open_context *ctx)
{
	dprintk("%s: has_layout=%d layoutcommit_ctx=%p ctx=%p\n", __func__,
		has_layout(nfsi), nfsi->layoutcommit_ctx, ctx);
	spin_lock(&nfsi->lo_lock);
	if (has_layout(nfsi) && !nfsi->layoutcommit_ctx) {
		nfsi->layoutcommit_ctx = get_nfs_open_context(ctx);
		nfsi->change_attr++;
		spin_unlock(&nfsi->lo_lock);
		dprintk("%s: Set layoutcommit_ctx=%p\n", __func__,
			nfsi->layoutcommit_ctx);
		return;
	}
	spin_unlock(&nfsi->lo_lock);
}

/* Unitialize a mountpoint in a layout driver */
void
unmount_pnfs_layoutdriver(struct super_block *sb)
{
	struct nfs_server *server = NFS_SB(sb);
	if (server->pnfs_curr_ld &&
	    server->pnfs_curr_ld->ld_io_ops &&
	    server->pnfs_curr_ld->ld_io_ops->uninitialize_mountpoint) {
		server->pnfs_curr_ld->ld_io_ops->uninitialize_mountpoint(
			server->pnfs_mountid);
		server->pnfs_mountid = NULL;
	    }
}

/*
 * Set the server pnfs module to the first registered pnfs_type.
 * Only one pNFS layout driver is supported.
 */
void
set_pnfs_layoutdriver(struct super_block *sb, struct nfs_fh *fh, u32 id)
{
	struct pnfs_module *mod;
	struct pnfs_mount_type *mt;
	struct nfs_server *server = NFS_SB(sb);

	if (id > 0 && find_pnfs(id, &mod)) {
		dprintk("%s: Setting pNFS module\n", __func__);
		server->pnfs_curr_ld = mod->pnfs_ld_type;
		mt = server->pnfs_curr_ld->ld_io_ops->initialize_mountpoint(
			sb, fh);
		if (!mt) {
			printk(KERN_ERR "%s: Error initializing mount point "
			       "for layout driver %u. ", __func__, id);
			goto out_err;
		}
		/* Layout driver succeeded in initializing mountpoint */
		server->pnfs_mountid = mt;
		/* Set the rpc_ops */
		server->nfs_client->rpc_ops = &pnfs_v4_clientops;
		return;
	}

	dprintk("%s: No pNFS module found for %u. ", __func__, id);
out_err:
	dprintk("Using NFSv4 I/O\n");
	server->pnfs_curr_ld = NULL;
	server->pnfs_mountid = NULL;
	return;
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
#define BUG_ON_UNLOCKED_LO(lo) \
	BUG_ON(!spin_is_locked(&PNFS_NFS_INODE(lo)->lo_lock))
#else /* CONFIG_SMP */
#define BUG_ON_UNLOCKED_LO(lo) do {} while (0)
#endif /* CONFIG_SMP */

/*
 * get and lock nfsi->layout
 */
static inline struct pnfs_layout_type *
get_lock_current_layout(struct nfs_inode *nfsi)
{
	struct pnfs_layout_type *lo;

	lo = &nfsi->layout;
	spin_lock(&nfsi->lo_lock);
	if (!lo->ld_data) {
		spin_unlock(&nfsi->lo_lock);
		return NULL;
	}

	lo->refcount++;
	return lo;
}

/*
 * put and unlock nfs->layout
 */
static inline void
put_unlock_current_layout(struct pnfs_layout_type *lo)
{
	struct nfs_inode *nfsi = PNFS_NFS_INODE(lo);
	struct nfs_client *clp;

	BUG_ON_UNLOCKED_LO(lo);
	BUG_ON(lo->refcount <= 0);

	if (--lo->refcount == 0 && list_empty(&lo->segs)) {
		struct layoutdriver_io_operations *io_ops =
			PNFS_LD_IO_OPS(lo);

		dprintk("%s: freeing layout %p\n", __func__, lo->ld_data);
		io_ops->free_layout(lo->ld_data);
		lo->ld_data = NULL;

		/* Unlist the inode. */
		clp = NFS_SERVER(&nfsi->vfs_inode)->nfs_client;
		spin_lock(&clp->cl_lock);
		list_del_init(&nfsi->lo_inodes);
		spin_unlock(&clp->cl_lock);
	}
	spin_unlock(&nfsi->lo_lock);
}

void
pnfs_layout_release(struct pnfs_layout_type *lo,
		    struct nfs4_pnfs_layout_segment *range)
{
	struct nfs_inode *nfsi = PNFS_NFS_INODE(lo);

	spin_lock(&nfsi->lo_lock);
	if (range)
		pnfs_free_layout(lo, range);
	put_unlock_current_layout(lo);
	wake_up_all(&nfsi->lo_waitq);
}

static inline void
init_lseg(struct pnfs_layout_type *lo, struct pnfs_layout_segment *lseg)
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
	PNFS_LD_IO_OPS(lseg->layout)->free_lseg(lseg);
}

static inline void
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
	kref_put(&lseg->kref, destroy_lseg);
	if (do_wake_up)
		wake_up(&nfsi->lo_waitq);
}

static inline u64
end_offset(u64 start, u64 len)
{
	u64 end;

	end = start + len;
	return end >= start ? end: NFS4_MAX_UINT64;
}

/* last octet in a range */
static inline u64
last_byte_offset(u64 start, u64 len)
{
	u64 end;

	BUG_ON(!len);
	end = start + len;
	return end > start ? end - 1: NFS4_MAX_UINT64;
}

/*
 * is l2 fully contained in l1?
 *   start1                             end1
 *   [----------------------------------)
 *           start2           end2
 *           [----------------)
 */
static inline int
lo_seg_contained(struct nfs4_pnfs_layout_segment *l1,
		 struct nfs4_pnfs_layout_segment *l2)
{
	u64 start1 = l1->offset;
	u64 end1 = end_offset(start1, l1->length);
	u64 start2 = l2->offset;
	u64 end2 = end_offset(start2, l2->length);

	return (start1 <= start2) && (end1 >= end2);
}

/*
 * is l1 and l2 intersecting?
 *   start1                             end1
 *   [----------------------------------)
 *                              start2           end2
 *                              [----------------)
 */
static inline int
lo_seg_intersecting(struct nfs4_pnfs_layout_segment *l1,
		    struct nfs4_pnfs_layout_segment *l2)
{
	u64 start1 = l1->offset;
	u64 end1 = end_offset(start1, l1->length);
	u64 start2 = l2->offset;
	u64 end2 = end_offset(start2, l2->length);

	return (end1 == NFS4_MAX_UINT64 || end1 > start2) &&
	       (end2 == NFS4_MAX_UINT64 || end2 > start1);
}

void
pnfs_set_layout_stateid(struct pnfs_layout_type *lo,
			const nfs4_stateid *stateid)
{
	write_seqlock(&lo->seqlock);
	memcpy(lo->stateid.data, stateid->data, sizeof(lo->stateid.data));
	write_sequnlock(&lo->seqlock);
}

static void
pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_type *lo)
{
	int seq;

	dprintk("--> %s\n", __func__);

	do {
		seq = read_seqbegin(&lo->seqlock);
		memcpy(dst->data, lo->stateid.data, sizeof(lo->stateid.data));
	} while (read_seqretry(&lo->seqlock, seq));

	dprintk("<-- %s\n", __func__);
}

static void
pnfs_layout_from_open_stateid(nfs4_stateid *dst, struct nfs4_state *state)
{
	int seq;

	dprintk("--> %s\n", __func__);

	do {
		seq = read_seqbegin(&state->seqlock);
		memcpy(dst->data, state->stateid.data,
				sizeof(state->stateid.data));
	} while (read_seqretry(&state->seqlock, seq));

	dprintk("<-- %s\n", __func__);
}

/*
* Get layout from server.
*    for now, assume that whole file layouts are requested.
*    arg->offset: 0
*    arg->length: all ones
*
*    for now, assume the LAYOUTGET operation is triggered by an I/O request.
*    the count field is the count in the I/O request, and will be used
*    as the minlength. for the file operation that piggy-backs
*    the LAYOUTGET operation with an OPEN, s
*    arg->minlength = count.
*/
static int
get_layout(struct inode *ino,
	   struct nfs_open_context *ctx,
	   struct nfs4_pnfs_layout_segment *range,
	   struct pnfs_layout_segment **lsegpp,
	   struct pnfs_layout_type *lo)
{
	int status;
	struct nfs_server *server = NFS_SERVER(ino);
	struct nfs4_pnfs_layoutget *lgp;

	dprintk("--> %s\n", __func__);

	lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
	if (lgp == NULL)
		return -ENOMEM;
	lgp->lo = lo;
	lgp->args.minlength = PAGE_CACHE_SIZE;
	lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;
	lgp->args.lseg.iomode = range->iomode;
	lgp->args.lseg.offset = range->offset;
	lgp->args.lseg.length = max(range->length, lgp->args.minlength);
	lgp->args.type = server->pnfs_curr_ld->id;
	lgp->args.inode = ino;
	lgp->lsegpp = lsegpp;

	if (!memcmp(lo->stateid.data, &zero_stateid, NFS4_STATEID_SIZE)) {
		struct nfs_open_context *oldctx = ctx;

		if (!oldctx) {
			ctx = nfs_find_open_context(ino, NULL,
					(range->iomode == IOMODE_READ) ?
					FMODE_READ: FMODE_WRITE);
			BUG_ON(!ctx);
		}
		pnfs_layout_from_open_stateid(&lgp->args.stateid, ctx->state);
		if (!oldctx)
			put_nfs_open_context(ctx);
	} else
		pnfs_get_layout_stateid(&lgp->args.stateid, lo);

	/* Retrieve layout information from server */
	status = pnfs4_proc_layoutget(lgp);

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
		   struct nfs4_pnfs_layout_segment *range)
{
	return (range->iomode == IOMODE_ANY ||
		lseg->range.iomode == range->iomode) &&
	       lo_seg_intersecting(&lseg->range, range);
}

static struct pnfs_layout_segment *
has_layout_to_return(struct pnfs_layout_type *lo,
		     struct nfs4_pnfs_layout_segment *range)
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

static void
pnfs_free_layout(struct pnfs_layout_type *lo,
		 struct nfs4_pnfs_layout_segment *range)
{
	struct pnfs_layout_segment *lseg, *next;
	dprintk("%s:Begin lo %p offset %llu length %llu iomode %d\n",
		__func__, lo, range->offset, range->length, range->iomode);

	BUG_ON_UNLOCKED_LO(lo);
	list_for_each_entry_safe (lseg, next, &lo->segs, fi_list) {
		if (!should_free_lseg(lseg, range))
			continue;
		dprintk("%s: freeing lseg %p iomode %d "
			"offset %llu length %llu\n", __func__,
			lseg, lseg->range.iomode, lseg->range.offset,
			lseg->range.length);
		list_del(&lseg->fi_list);
		put_lseg(lseg);
	}

	dprintk("%s:Return\n", __func__);
}

static inline bool
_pnfs_can_return_lseg(struct pnfs_layout_segment *lseg)
{
	return atomic_read(&lseg->kref.refcount) == 1;
}

static bool
pnfs_return_layout_barrier(struct nfs_inode *nfsi,
			   struct nfs4_pnfs_layout_segment *range)
{
	struct pnfs_layout_segment *lseg;
	bool ret = false;

	spin_lock(&nfsi->lo_lock);
	list_for_each_entry (lseg, &nfsi->layout.segs, fi_list) {
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
	spin_unlock(&nfsi->lo_lock);

	dprintk("%s:Return %d\n", __func__, ret);
	return ret;
}

static int
return_layout(struct inode *ino, struct nfs4_pnfs_layout_segment *range,
	      const nfs4_stateid *stateid, /* optional */
	      enum pnfs_layoutreturn_type type, struct pnfs_layout_type *lo)
{
	struct nfs4_pnfs_layoutreturn *lrp;
	struct nfs_server *server = NFS_SERVER(ino);
	int status = -ENOMEM;

	dprintk("--> %s\n", __func__);

	lrp = kzalloc(sizeof(*lrp), GFP_KERNEL);
	if (lrp == NULL)
		goto out;
	lrp->args.reclaim = 0;
	lrp->args.layout_type = server->pnfs_curr_ld->id;
	lrp->args.return_type = type;
	lrp->args.lseg = *range;
	lrp->args.inode = ino;
	lrp->lo = lo;
	if (stateid)
		lrp->args.stateid = *stateid;
	else if (lo)
		pnfs_get_layout_stateid(&lrp->args.stateid, lo);

	status = pnfs4_proc_layoutreturn(lrp);
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
}

int
_pnfs_return_layout(struct inode *ino, struct nfs4_pnfs_layout_segment *range,
		    const nfs4_stateid *stateid, /* optional */
		    enum pnfs_layoutreturn_type type)
{
	struct pnfs_layout_type *lo = NULL;
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs4_pnfs_layout_segment arg;
	int status = 0;

	dprintk("--> %s type %d\n", __func__, type);

	if (range)
		arg = *range;
	else {
		arg.iomode = IOMODE_ANY;
		arg.offset = 0;
		arg.length = ~0;
	}
	if (type == RETURN_FILE) {
		if (nfsi->layoutcommit_ctx) {
			status = pnfs_layoutcommit_inode(ino, 1);
			if (status) {
				dprintk("%s: layoutcommit failed, status=%d. "
					"Returning layout anyway\n",
					__func__, status);
				status = 0;
			}
		}

		lo = get_lock_current_layout(nfsi);
		if (lo && !has_layout_to_return(lo, &arg)) {
			put_unlock_current_layout(lo);
			lo = NULL;
		}
		if (!lo) {
			dprintk("%s: no layout segments to return\n", __func__);
			/* must send the LAYOUTRETURN in response to recall */
			if (stateid)
				goto send_return;
			else
				goto out;
		}

		/* unlock w/o put rebalanced by eventual call to
		 * pnfs_layout_release
		 */
		spin_unlock(&nfsi->lo_lock);

		if (pnfs_return_layout_barrier(nfsi, &arg)) {
			dprintk("%s: waiting\n", __func__);
			wait_event(nfsi->lo_waitq,
				!pnfs_return_layout_barrier(nfsi, &arg));
		}
	}
send_return:
	status = return_layout(ino, &arg, stateid, type, lo);
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
}

void
pnfs_return_layout_done(struct pnfs_layout_type *lo,
		     struct nfs4_pnfs_layoutreturn *lrp,
		     int rpc_status)
{
	dprintk("--> %s\n", __func__);

	/* FIX-ME: If layoutreturn failed, we have already removed the
	 * lseg from the cache...
	 */
	pnfs_set_layout_stateid(lo, &lrp->res.stateid);

	dprintk("<-- %s\n", __func__);
}

/*
 * cmp two layout segments for sorting into layout cache
 */
static inline s64
cmp_layout(struct nfs4_pnfs_layout_segment *l1,
	   struct nfs4_pnfs_layout_segment *l2)
{
	s64 d;

	/* higher offset > lower offset */
	d = l1->offset - l2->offset;
	if (d)
		return d;

	/* longer length > shorter length */
	d = l1->length - l2->length;
	if (d)
		return d;

	/* read > read/write */
	return (int)(l1->iomode == IOMODE_READ) -
	       (int)(l2->iomode == IOMODE_READ);
}

static void
pnfs_insert_layout(struct pnfs_layout_type *lo,
		   struct pnfs_layout_segment *lseg)
{
	struct pnfs_layout_segment *lp;
	int found = 0;

	dprintk("%s:Begin\n", __func__);

	BUG_ON_UNLOCKED_LO(lo);
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

	dprintk("%s:Return\n", __func__);
}

static struct pnfs_layout_type *
alloc_init_layout(struct inode *ino)
{
	void *ld_data;
	struct pnfs_layout_type *lo;
	struct layoutdriver_io_operations *io_ops;

	io_ops = NFS_SERVER(ino)->pnfs_curr_ld->ld_io_ops;
	lo = &NFS_I(ino)->layout;
	ld_data = io_ops->alloc_layout(NFS_SERVER(ino)->pnfs_mountid, ino);
	if (!ld_data) {
		printk(KERN_ERR
			"%s: out of memory: io_ops->alloc_layout failed\n",
			__func__);
		return NULL;
	}

	BUG_ON(lo->ld_data != NULL);
	lo->ld_data = ld_data;
	seqlock_init(&lo->seqlock);
	memset(&lo->stateid, 0, NFS4_STATEID_SIZE);
	lo->refcount = 1;
	INIT_LIST_HEAD(&lo->segs);
	lo->roc_iomode = 0;
	return lo;
}

static int pnfs_wait_schedule(void *word)
{
	if (signal_pending(current))
		return -ERESTARTSYS;
	schedule();
	return 0;
}

/*
 * get, possibly allocate, and lock current_layout
 *
 * Note: If successful, nfsi->lo_lock is taken and the caller
 * must put and unlock current_layout by using put_unlock_current_layout()
 * when the returned layout is released.
 */
static struct pnfs_layout_type *
get_lock_alloc_layout(struct inode *ino)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_type *lo;
	int res;

	dprintk("%s Begin\n", __func__);

	while ((lo = get_lock_current_layout(nfsi)) == NULL) {
		/* Compete against other threads on who's doing the allocation,
		 * wait until bit is cleared if we lost this race.
		 */
		res = wait_on_bit_lock(
			&nfsi->pnfs_layout_state, NFS_INO_LAYOUT_ALLOC,
			pnfs_wait_schedule, TASK_KILLABLE);
		if (res) {
			lo = ERR_PTR(res);
			break;
		}

		/* Was current_layout already allocated while we slept?
		 * If so, retry get_lock'ing it. Otherwise, allocate it.
		 */
		if (nfsi->layout.ld_data)
			continue;

		lo = alloc_init_layout(ino);
		if (lo) {
			struct nfs_client *clp = NFS_SERVER(ino)->nfs_client;

			/* must grab the layout lock before the client lock */
			spin_lock(&nfsi->lo_lock);

			spin_lock(&clp->cl_lock);
			if (list_empty(&nfsi->lo_inodes))
				list_add_tail(&nfsi->lo_inodes, &clp->cl_lo_inodes);
			spin_unlock(&clp->cl_lock);
		} else
			lo = ERR_PTR(-ENOMEM);

		/* release the NFS_INO_LAYOUT_ALLOC bit and wake up waiters */
		clear_bit_unlock(NFS_INO_LAYOUT_ALLOC, &nfsi->pnfs_layout_state);
		wake_up_bit(&nfsi->pnfs_layout_state, NFS_INO_LAYOUT_ALLOC);
		break;
	}

#ifdef NFS_DEBUG
	if (!IS_ERR(lo))
		dprintk("%s Return %p\n", __func__, lo);
	else
		dprintk("%s Return error %ld\n", __func__, PTR_ERR(lo));
#endif
	return lo;
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
		  struct nfs4_pnfs_layout_segment *range)
{
	struct nfs4_pnfs_layout_segment range1;

	if ((range->iomode == IOMODE_RW && lseg->range.iomode != IOMODE_RW) ||
	    !lo_seg_intersecting(&lseg->range, range))
		return 0;

	/* range1 covers only the first byte in the range */
	range1 = *range;
	range1.length = 1;
	return lo_seg_contained(&lseg->range, &range1);
}

/*
 * lookup range in layout
 */
static struct pnfs_layout_segment *
pnfs_has_layout(struct pnfs_layout_type *lo,
		struct nfs4_pnfs_layout_segment *range,
		bool take_ref,
		bool only_valid)
{
	struct pnfs_layout_segment *lseg, *ret = NULL;

	dprintk("%s:Begin\n", __func__);

	BUG_ON_UNLOCKED_LO(lo);
	list_for_each_entry (lseg, &lo->segs, fi_list) {
		if (has_matching_lseg(lseg, range) &&
		    (lseg->valid || !only_valid)) {
			ret = lseg;
			if (take_ref)
				kref_get(&ret->kref);
			break;
		}
	}

	dprintk("%s:Return lseg %p take_ref %d ref %d valid %d\n",
		__func__, ret, take_ref,
		ret ? atomic_read(&ret->kref.refcount) : 0,
		ret ? ret->valid : 0);
	return ret;
}

/* Update the file's layout for the given range and iomode.
 * Layout is retreived from the server if needed.
 * If lsegpp is given, the appropriate layout segment is referenced and
 * returned to the caller.
 */
int
pnfs_update_layout(struct inode *ino,
		   struct nfs_open_context *ctx,
		   u64 count,
		   loff_t pos,
		   enum pnfs_iomode iomode,
		   struct pnfs_layout_segment **lsegpp)
{
	struct nfs4_pnfs_layout_segment arg = {
		.iomode = iomode,
		.offset = pos,
		.length = count
	};
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_type *lo;
	struct pnfs_layout_segment *lseg = NULL;
	bool take_ref = (lsegpp != NULL);
	DEFINE_WAIT(__wait);
	int result = 0;

	lo = get_lock_alloc_layout(ino);
	if (IS_ERR(lo)) {
		dprintk("%s ERROR: can't get pnfs_layout_type\n", __func__);
		result = PTR_ERR(lo);
		goto out;
	}

	/* Check to see if the layout for the given range already exists */
	lseg = pnfs_has_layout(lo, &arg, take_ref, !take_ref);
	if (lseg && !lseg->valid) {
		spin_unlock(&nfsi->lo_lock);
		if (take_ref)
			put_lseg(lseg);
		for (;;) {
			prepare_to_wait(&nfsi->lo_waitq, &__wait,
					TASK_KILLABLE);
			spin_lock(&nfsi->lo_lock);
			lseg = pnfs_has_layout(lo, &arg, take_ref, !take_ref);
			if (!lseg || lseg->valid)
				break;
			dprintk("%s: invalid lseg %p ref %d\n", __func__,
				lseg, atomic_read(&lseg->kref.refcount)-1);
			if (take_ref)
				put_lseg(lseg);
			if (signal_pending(current)) {
				lseg = NULL;
				result = -ERESTARTSYS;
				break;
			}
			spin_unlock(&nfsi->lo_lock);
			schedule();
		}
		finish_wait(&nfsi->lo_waitq, &__wait);
		if (result)
			goto out_put;
	}

	if (lseg) {
		dprintk("%s: Using cached lseg %p for %llu@%llu iomode %d)\n",
			__func__,
			lseg,
			arg.length,
			arg.offset,
			arg.iomode);

		goto out_put;
	}

	/* if get layout already failed once goto out */
	if (test_bit(NFS_INO_LAYOUT_FAILED, &nfsi->pnfs_layout_state)) {
		if (unlikely(nfsi->pnfs_layout_suspend &&
		    get_seconds() >= nfsi->pnfs_layout_suspend)) {
			dprintk("%s: layout_get resumed\n", __func__);
			clear_bit(NFS_INO_LAYOUT_FAILED,
				  &nfsi->pnfs_layout_state);
			nfsi->pnfs_layout_suspend = 0;
		} else {
			result = 1;
			goto out_put;
		}
	}

	/* Lose lock, but not reference, match this with pnfs_layout_release */
	spin_unlock(&nfsi->lo_lock);

	result = get_layout(ino, ctx, &arg, lsegpp, lo);
out:
	dprintk("%s end (err:%d) state 0x%lx lseg %p\n",
			__func__, result, nfsi->pnfs_layout_state, lseg);
	return result;
out_put:
	if (lsegpp)
		*lsegpp = lseg;
	put_unlock_current_layout(lo);
	goto out;
}

void
pnfs_get_layout_done(struct nfs4_pnfs_layoutget *lgp, int rpc_status)
{
	struct nfs4_pnfs_layoutget_res *res = &lgp->res;
	struct pnfs_layout_segment *lseg = NULL;
	struct nfs_inode *nfsi = PNFS_NFS_INODE(lgp->lo);
	time_t suspend = 0;

	dprintk("-->%s\n", __func__);

	lgp->status = rpc_status;
	if (likely(!rpc_status)) {
		if (unlikely(res->layout.len <= 0)) {
			printk(KERN_ERR
			       "%s: ERROR!  Layout size is ZERO!\n", __func__);
			lgp->status = -EIO;
			goto get_out;
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

get_out:
	/* remember that get layout failed and suspend trying */
	nfsi->pnfs_layout_suspend = suspend;
	set_bit(NFS_INO_LAYOUT_FAILED, &nfsi->pnfs_layout_state);
	dprintk("%s: layout_get suspended until %ld\n",
		__func__, suspend);
out:
	dprintk("%s end (err:%d) state 0x%lx lseg %p\n",
		__func__, lgp->status, nfsi->pnfs_layout_state, lseg);
	return;
}

int
pnfs_layout_process(struct nfs4_pnfs_layoutget *lgp)
{
	struct pnfs_layout_type *lo = lgp->lo;
	struct nfs4_pnfs_layoutget_res *res = &lgp->res;
	struct pnfs_layout_segment *lseg;
	struct nfs_inode *nfsi = PNFS_NFS_INODE(lo);
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

	init_lseg(lo, lseg);
	lseg->range = res->lseg;
	if (lgp->lsegpp) {
		kref_get(&lseg->kref);
		*lgp->lsegpp = lseg;
	}

	spin_lock(&nfsi->lo_lock);
	pnfs_insert_layout(lo, lseg);

	if (res->return_on_close) {
		lo->roc_iomode |= res->lseg.iomode;
		if (!lo->roc_iomode)
			lo->roc_iomode = IOMODE_ANY;
	}

	/* Done processing layoutget. Set the layout stateid */
	pnfs_set_layout_stateid(lo, &res->stateid);
	spin_unlock(&nfsi->lo_lock);
out:
	return status;
}

size_t
pnfs_getthreshold(struct inode *inode, int iswrite)
{
	struct nfs_server *nfss = NFS_SERVER(inode);
	struct nfs_inode *nfsi = NFS_I(inode);
	ssize_t threshold = 0;

	if (!pnfs_enabled_sb(nfss) ||
	    !nfss->pnfs_curr_ld->ld_policy_ops)
		goto out;

	if (iswrite && nfss->pnfs_curr_ld->ld_policy_ops->get_write_threshold) {
		threshold = nfss->pnfs_curr_ld->ld_policy_ops->
				get_write_threshold(&nfsi->layout, inode);
		goto out;
	}

	if (!iswrite && nfss->pnfs_curr_ld->ld_policy_ops->get_read_threshold) {
		threshold = nfss->pnfs_curr_ld->ld_policy_ops->
				get_read_threshold(&nfsi->layout, inode);
	}
out:
	return threshold;
}

/*
 * Ask the layout driver for the request size at which pNFS should be used
 * or standard NFSv4 I/O.  Writing directly to the NFSv4 server can
 * improve performance through its singularity and async behavior to
 * the underlying parallel file system.
 */
static int
below_threshold(struct inode *inode, size_t req_size, int iswrite)
{
	ssize_t threshold;

	threshold = pnfs_getthreshold(inode, iswrite);
	if ((ssize_t)req_size <= threshold)
		return 1;
	else
		return 0;
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
	struct pnfs_layout_type *laytype;
	struct pnfs_layoutdriver_type *ld;

	pgio->pg_test = NULL;

	laytype = &NFS_I(inode)->layout;
	ld = NFS_SERVER(inode)->pnfs_curr_ld;
	if (!pnfs_enabled_sb(NFS_SERVER(inode)) || !laytype)
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
	struct nfs_inode *nfsi;
	struct pnfs_layout_type *lo;

	if (!nfss->pnfs_curr_ld)
		goto out;

	policy_ops = nfss->pnfs_curr_ld->ld_policy_ops;
	if (!policy_ops || !policy_ops->get_stripesize)
		goto out;

	/* The default is to not gather across stripes */
	if (pnfs_ld_gather_across_stripes(nfss->pnfs_curr_ld))
		goto out;

	nfsi = NFS_I(inode);
	lo = get_lock_current_layout(nfsi);;
	if (lo) {
		stripe_size = policy_ops->get_stripesize(lo);
		put_unlock_current_layout(lo);
	}
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
		  struct list_head *pages,
		  size_t *rsize)
{
	struct nfs_server *nfss = NFS_SERVER(inode);
	size_t count = 0;
	loff_t loff;
	int status = 0;

	pgio->pg_boundary = 0;
	pgio->pg_test = NULL;

	if (!pnfs_enabled_sb(nfss))
		return;

	/* Calculate the total read-ahead count */
	readahead_range(inode, pages, &loff, &count);

	if (count > 0 && !below_threshold(inode, count, 0)) {
		status = pnfs_update_layout(inode, ctx, count,
						loff, IOMODE_READ, NULL);
		dprintk("%s *rsize %Zd virt update returned %d\n",
					__func__, *rsize, status);
		if (status != 0)
			return;

		*rsize = NFS_SERVER(inode)->ds_rsize;
		pgio->pg_boundary = pnfs_getboundary(inode);
		if (pgio->pg_boundary)
			pnfs_set_pg_test(inode, pgio);
	}
}

/* This is utilized in the paging system to determine if
 * it should use the NFSv4 or pNFS read path.
 * If count < 0, we do not check the I/O size.
 */
int
pnfs_use_read(struct inode *inode, ssize_t count)
{
	struct nfs_server *nfss = NFS_SERVER(inode);

	/* Use NFSv4 I/O if there is no layout driver OR
	 * count is below the threshold.
	 */
	if (!pnfs_enabled_sb(nfss) ||
	    (count >= 0 && below_threshold(inode, count, 0)))
		return 0;

	return 1; /* use pNFS I/O */
}

/* Called only from pnfs4 nfs_rpc_ops => a layout driver is loaded */
int
pnfs_use_ds_io(struct list_head *head, struct inode *inode, int io)
{
	struct nfs_page	*req;
	struct list_head *pos, *tmp;
	int count = 0;

	list_for_each_safe(pos, tmp, head) {
		req = nfs_list_entry(head->next);
		count += req->wb_bytes;
	}
	if (count >= 0 && below_threshold(inode, count, io))
		return 0;
	return 1; /* use pNFS data server I/O */
}

/* This is utilized in the paging system to determine if
 * it should use the NFSv4 or pNFS write path.
 * If count < 0, we do not check the I/O size.
 */
int
pnfs_use_write(struct inode *inode, ssize_t count)
{
	struct nfs_server *nfss = NFS_SERVER(inode);

	/* Use NFSv4 I/O if there is no layout driver OR
	 * count is below the threshold.
	 */
	if (!pnfs_enabled_sb(nfss) ||
	    (count >= 0 && below_threshold(inode, count, 1)))
		return 0;

	return 1; /* use pNFS I/O */
}

/* Return I/O buffer size for a layout driver
 * This value will determine what size reads and writes
 * will be gathered into and sent to the data servers.
 * blocksize must be a multiple of the page cache size.
 */
unsigned int
pnfs_getiosize(struct nfs_server *server)
{
	struct pnfs_mount_type *mounttype;
	struct pnfs_layoutdriver_type *ld;

	mounttype = server->pnfs_mountid;
	ld = server->pnfs_curr_ld;
	if (!pnfs_enabled_sb(server) ||
	    !mounttype ||
	    !ld->ld_policy_ops ||
	    !ld->ld_policy_ops->get_blocksize)
		return 0;

	return ld->ld_policy_ops->get_blocksize(mounttype);
}

void
pnfs_set_ds_iosize(struct nfs_server *server)
{
	unsigned dssize = pnfs_getiosize(server);

	/* Set buffer size for data servers */
	if (dssize > 0) {
		server->ds_rsize = server->ds_wsize =
			nfs_block_size(dssize, NULL);
		server->ds_rpages = server->ds_wpages =
			(server->ds_rsize + PAGE_CACHE_SIZE - 1) >>
			PAGE_CACHE_SHIFT;
	} else {
		server->ds_wsize = server->wsize;
		server->ds_rsize = server->rsize;
		server->ds_rpages = server->rpages;
		server->ds_wpages = server->wpages;
	}
}

/* Called on completion of layoutcommit */
void
pnfs_layoutcommit_done(struct pnfs_layoutcommit_data *data)
{
	struct nfs_server *nfss = NFS_SERVER(data->args.inode);
	struct nfs_inode *nfsi = NFS_I(data->args.inode);

	dprintk("%s: (status %d)\n", __func__, data->status);

	/* TODO: For now, set an error in the open context (just like
	 * if a commit failed) We may want to do more, much more, like
	 * replay all writes through the NFSv4
	 * server, or something.
	 */
	if (data->status < 0) {
		printk(KERN_ERR "%s, Layoutcommit Failed! = %d\n",
		       __func__, data->status);
		data->ctx->error = data->status;
	}

	/* TODO: Maybe we should avoid this by allowing the layout driver
	 * to directly xdr its layout on the wire.
	 */
	if (nfss->pnfs_curr_ld->ld_io_ops->cleanup_layoutcommit)
		nfss->pnfs_curr_ld->ld_io_ops->cleanup_layoutcommit(
							&nfsi->layout,
							&data->args,
							data->status);

	/* release the open_context acquired in pnfs_writeback_done */
	put_nfs_open_context(data->ctx);
}

/*
 * Set up the argument/result storage required for the RPC call.
 */
static int
pnfs_layoutcommit_setup(struct pnfs_layoutcommit_data *data, int sync)
{
	struct nfs_inode *nfsi = NFS_I(data->args.inode);
	struct nfs_server *nfss = NFS_SERVER(data->args.inode);
	int result = 0;

	dprintk("%s Begin (sync:%d)\n", __func__, sync);
	data->args.fh = NFS_FH(data->args.inode);
	data->args.layout_type = nfss->pnfs_curr_ld->id;

	/* TODO: Need to determine the correct values */
	data->args.time_modify_changed = 0;

	/* Set values from inode so it can be reset
	 */
	data->args.lseg.iomode = IOMODE_RW;
	data->args.lseg.offset = nfsi->pnfs_write_begin_pos;
	data->args.lseg.length = nfsi->pnfs_write_end_pos - nfsi->pnfs_write_begin_pos + 1;
	data->args.lastbytewritten = nfsi->pnfs_write_end_pos;
	data->args.bitmask = nfss->attr_bitmask;
	data->res.server = nfss;

	/* Call layout driver to set the arguments.
	 */
	if (nfss->pnfs_curr_ld->ld_io_ops->setup_layoutcommit) {
		result = nfss->pnfs_curr_ld->ld_io_ops->setup_layoutcommit(
				&nfsi->layout, &data->args);
		if (result)
			goto out;
	}
	pnfs_get_layout_stateid(&data->args.stateid, &nfsi->layout);
	data->res.fattr = &data->fattr;
	nfs_fattr_init(&data->fattr);

out:
	dprintk("%s End Status %d\n", __func__, result);
	return result;
}

/* Issue a async layoutcommit for an inode.
 */
int
pnfs_layoutcommit_inode(struct inode *inode, int sync)
{
	struct pnfs_layoutcommit_data *data;
	struct nfs_inode *nfsi = NFS_I(inode);
	int status = 0;

	dprintk("%s Begin (sync:%d)\n", __func__, sync);

	BUG_ON(!has_layout(nfsi));

	data = pnfs_layoutcommit_alloc();
	if (!data)
		return -ENOMEM;

	spin_lock(&nfsi->lo_lock);
	if (!nfsi->layoutcommit_ctx) {
		pnfs_layoutcommit_free(data);
		goto out_unlock;
	}

	data->args.inode = inode;
	data->cred  = nfsi->layoutcommit_ctx->cred;
	data->ctx = nfsi->layoutcommit_ctx;

	/* Set up layout commit args*/
	status = pnfs_layoutcommit_setup(data, sync);
	if (status)
		goto out_unlock;

	/* Clear layoutcommit properties in the inode so
	 * new lc info can be generated
	 */
	nfsi->pnfs_write_begin_pos = 0;
	nfsi->pnfs_write_end_pos = 0;
	nfsi->layoutcommit_ctx = NULL;

	/* release lock on pnfs layoutcommit attrs */
	spin_unlock(&nfsi->lo_lock);

	data->is_sync = sync;
	status = pnfs4_proc_layoutcommit(data);
out:
	dprintk("%s end (err:%d)\n", __func__, status);
	return status;
out_unlock:
	spin_unlock(&nfsi->lo_lock);
	goto out;
}

/* Callback operations for layout drivers.
 */
struct pnfs_client_operations pnfs_ops = {
	.nfs_getdevicelist = nfs4_pnfs_getdevicelist,
	.nfs_getdeviceinfo = nfs4_pnfs_getdeviceinfo,
};

EXPORT_SYMBOL(pnfs_unregister_layoutdriver);
EXPORT_SYMBOL(pnfs_register_layoutdriver);

#endif /* CONFIG_PNFS */
