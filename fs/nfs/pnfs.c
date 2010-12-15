/*
 *  pNFS functions to call and manage layout drivers.
 *
 *  Copyright (c) 2002 [year of first publication]
 *  The Regents of the University of Michigan
 *  All Rights Reserved
 *
 *  Dean Hildebrand <dhildebz@umich.edu>
 *
 *  Permission is granted to use, copy, create derivative works, and
 *  redistribute this software and such derivative works for any purpose,
 *  so long as the name of the University of Michigan is not used in
 *  any advertising or publicity pertaining to the use or distribution
 *  of this software without specific, written prior authorization. If
 *  the above copyright notice or any other identification of the
 *  University of Michigan is included in any copy of any portion of
 *  this software, then the disclaimer below must also be included.
 *
 *  This software is provided as is, without representation or warranty
 *  of any kind either express or implied, including without limitation
 *  the implied warranties of merchantability, fitness for a particular
 *  purpose, or noninfringement.  The Regents of the University of
 *  Michigan shall not be liable for any damages, including special,
 *  indirect, incidental, or consequential damages, with respect to any
 *  claim arising out of or in connection with the use of the software,
 *  even if it has been or is hereafter advised of the possibility of
 *  such damages.
 */

#include <linux/nfs_fs.h>
#include "internal.h"
#include "pnfs.h"
#include "iostat.h"

#define NFSDBG_FACILITY		NFSDBG_PNFS

/* Locking:
 *
 * pnfs_spinlock:
 *      protects pnfs_modules_tbl.
 */
static DEFINE_SPINLOCK(pnfs_spinlock);

/*
 * pnfs_modules_tbl holds all pnfs modules
 */
static LIST_HEAD(pnfs_modules_tbl);

/* Return the registered pnfs layout driver module matching given id */
static struct pnfs_layoutdriver_type *
find_pnfs_driver_locked(u32 id)
{
	struct pnfs_layoutdriver_type *local;

	list_for_each_entry(local, &pnfs_modules_tbl, pnfs_tblid)
		if (local->id == id)
			goto out;
	local = NULL;
out:
	dprintk("%s: Searching for id %u, found %p\n", __func__, id, local);
	return local;
}

static struct pnfs_layoutdriver_type *
find_pnfs_driver(u32 id)
{
	struct pnfs_layoutdriver_type *local;

	spin_lock(&pnfs_spinlock);
	local = find_pnfs_driver_locked(id);
	spin_unlock(&pnfs_spinlock);
	return local;
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
	    !test_bit(NFS_LAYOUT_NEED_LCOMMIT, &nfsi->layout->plh_flags)) {
		nfsi->layout->cred = get_rpccred(ctx->state->owner->so_cred);
		__set_bit(NFS_LAYOUT_NEED_LCOMMIT,
			  &nfsi->layout->plh_flags);
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

void
unset_pnfs_layoutdriver(struct nfs_server *nfss)
{
	if (nfss->pnfs_curr_ld) {
		nfss->pnfs_curr_ld->clear_layoutdriver(nfss);
		module_put(nfss->pnfs_curr_ld->owner);
	}
	nfss->pnfs_curr_ld = NULL;
}

/*
 * Try to set the server's pnfs module to the pnfs layout type specified by id.
 * Currently only one pNFS layout driver per filesystem is supported.
 *
 * @id layout type. Zero (illegal layout type) indicates pNFS not in use.
 */
void
set_pnfs_layoutdriver(struct nfs_server *server, u32 id)
{
	struct pnfs_layoutdriver_type *ld_type = NULL;

	if (id == 0)
		goto out_no_driver;
	if (!(server->nfs_client->cl_exchange_flags &
		 (EXCHGID4_FLAG_USE_NON_PNFS | EXCHGID4_FLAG_USE_PNFS_MDS))) {
		printk(KERN_ERR "%s: id %u cl_exchange_flags 0x%x\n", __func__,
		       id, server->nfs_client->cl_exchange_flags);
		goto out_no_driver;
	}
	ld_type = find_pnfs_driver(id);
	if (!ld_type) {
		request_module("%s-%u", LAYOUT_NFSV4_1_MODULE_PREFIX, id);
		ld_type = find_pnfs_driver(id);
		if (!ld_type) {
			dprintk("%s: No pNFS module found for %u.\n",
				__func__, id);
			goto out_no_driver;
		}
	}
	if (!try_module_get(ld_type->owner)) {
		dprintk("%s: Could not grab reference on module\n", __func__);
		goto out_no_driver;
	}
	server->pnfs_curr_ld = ld_type;
	if (ld_type->set_layoutdriver(server)) {
		printk(KERN_ERR
		       "%s: Error initializing mount point for layout driver %u.\n",
		       __func__, id);
		module_put(ld_type->owner);
		goto out_no_driver;
	}
	dprintk("%s: pNFS module for %u set\n", __func__, id);
	return;

out_no_driver:
	dprintk("%s: Using NFSv4 I/O\n", __func__);
	server->pnfs_curr_ld = NULL;
}

int
pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	int status = -EINVAL;
	struct pnfs_layoutdriver_type *tmp;

	if (ld_type->id == 0) {
		printk(KERN_ERR "%s id 0 is reserved\n", __func__);
		return status;
	}
	if (!ld_type->alloc_lseg || !ld_type->free_lseg) {
		printk(KERN_ERR "%s Layout driver must provide "
		       "alloc_lseg and free_lseg.\n", __func__);
		return status;
	}

	if (!ld_type->read_pagelist || !ld_type->write_pagelist ||
	    !ld_type->commit) {
		printk(KERN_ERR "%s Layout driver must provide "
		       "read_pagelist, write_pagelist, and commit.\n",
		       __func__);
		return status;
	}

	spin_lock(&pnfs_spinlock);
	tmp = find_pnfs_driver_locked(ld_type->id);
	if (!tmp) {
		list_add(&ld_type->pnfs_tblid, &pnfs_modules_tbl);
		status = 0;
		dprintk("%s Registering id:%u name:%s\n", __func__, ld_type->id,
			ld_type->name);
	} else {
		printk(KERN_ERR "%s Module with id %d already loaded!\n",
			__func__, ld_type->id);
	}
	spin_unlock(&pnfs_spinlock);

	return status;
}
EXPORT_SYMBOL_GPL(pnfs_register_layoutdriver);

void
pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	dprintk("%s Deregistering id:%u\n", __func__, ld_type->id);
	spin_lock(&pnfs_spinlock);
	list_del(&ld_type->pnfs_tblid);
	spin_unlock(&pnfs_spinlock);
}
EXPORT_SYMBOL_GPL(pnfs_unregister_layoutdriver);

/*
 * pNFS client layout cache
 */

/* Need to hold i_lock if caller does not already hold reference */
void
get_layout_hdr(struct pnfs_layout_hdr *lo)
{
	atomic_inc(&lo->plh_refcount);
}

static void
destroy_layout_hdr(struct pnfs_layout_hdr *lo)
{
	dprintk("%s: freeing layout cache %p\n", __func__, lo);
	BUG_ON(!list_empty(&lo->layouts));
	NFS_I(lo->inode)->layout = NULL;
	kfree(lo);
}

static void
put_layout_hdr_locked(struct pnfs_layout_hdr *lo)
{
	assert_spin_locked(&lo->inode->i_lock);
	BUG_ON(atomic_read(&lo->plh_refcount) == 0);
	if (atomic_dec_and_test(&lo->plh_refcount))
		destroy_layout_hdr(lo);
}

void
put_layout_hdr(struct pnfs_layout_hdr *lo)
{
	struct inode *inode = lo->inode;

	BUG_ON(atomic_read(&lo->plh_refcount) == 0);
	if (atomic_dec_and_lock(&lo->plh_refcount, &inode->i_lock)) {
		destroy_layout_hdr(lo);
		spin_unlock(&inode->i_lock);
	}
}

static void
init_lseg(struct pnfs_layout_hdr *lo, struct pnfs_layout_segment *lseg)
{
	INIT_LIST_HEAD(&lseg->fi_list);
	atomic_set(&lseg->pls_refcount, 1);
	smp_mb();
	set_bit(NFS_LSEG_VALID, &lseg->pls_flags);
	lseg->layout = lo;
	lseg->pls_notify_mask = 0;
}

static void free_lseg(struct pnfs_layout_segment *lseg)
{
	struct inode *ino = lseg->layout->inode;
	u64 mask = lseg->pls_notify_mask;

	BUG_ON(atomic_read(&lseg->pls_refcount) != 0);
	NFS_SERVER(ino)->pnfs_curr_ld->free_lseg(lseg);
	notify_drained(NFS_SERVER(ino)->nfs_client, mask);
	/* Matched by get_layout_hdr_locked in pnfs_insert_layout */
	put_layout_hdr(NFS_I(ino)->layout);
}

static void
_put_lseg_common(struct pnfs_layout_segment *lseg)
{
	struct inode *ino = lseg->layout->inode;

	BUG_ON(test_bit(NFS_LSEG_VALID, &lseg->pls_flags));
	list_del(&lseg->fi_list);
	if (list_empty(&lseg->layout->segs)) {
		struct nfs_client *clp;

		clp = NFS_SERVER(ino)->nfs_client;
		spin_lock(&clp->cl_lock);
		/* List does not take a reference, so no need for put here */
		list_del_init(&lseg->layout->layouts);
		spin_unlock(&clp->cl_lock);
		clear_bit(NFS_LAYOUT_BULK_RECALL, &lseg->layout->plh_flags);
		if (!pnfs_layoutgets_blocked(lseg->layout, NULL))
			rpc_wake_up(&NFS_I(ino)->lo_rpcwaitq_stateid);
	}
	rpc_wake_up(&NFS_I(ino)->lo_rpcwaitq);
}

/* The use of tmp_list is necessary because pnfs_curr_ld->free_lseg
 * could sleep, so must be called outside of the lock.
 */
static void
put_lseg_locked(struct pnfs_layout_segment *lseg,
		struct list_head *tmp_list)
{
	dprintk("%s: lseg %p ref %d valid %d\n", __func__, lseg,
		atomic_read(&lseg->pls_refcount),
		test_bit(NFS_LSEG_VALID, &lseg->pls_flags));
	if (atomic_dec_and_test(&lseg->pls_refcount)) {
		_put_lseg_common(lseg);
		list_add(&lseg->fi_list, tmp_list);
	}
}

void
put_lseg(struct pnfs_layout_segment *lseg)
{
	struct inode *ino;

	if (!lseg)
		return;

	dprintk("%s: lseg %p ref %d valid %d\n", __func__, lseg,
		atomic_read(&lseg->pls_refcount),
		test_bit(NFS_LSEG_VALID, &lseg->pls_flags));
	ino = lseg->layout->inode;
	if (atomic_dec_and_lock(&lseg->pls_refcount, &ino->i_lock)) {
		_put_lseg_common(lseg);
		spin_unlock(&ino->i_lock);
		free_lseg(lseg);
	}
}
EXPORT_SYMBOL_GPL(put_lseg);

bool
should_free_lseg(struct pnfs_layout_range *lseg_range,
		 struct pnfs_layout_range *recall_range)
{
	return (recall_range->iomode == IOMODE_ANY ||
		lseg_range->iomode == recall_range->iomode);
}

static void mark_lseg_invalid(struct pnfs_layout_segment *lseg,
			      struct list_head *tmp_list)
{
	assert_spin_locked(&lseg->layout->inode->i_lock);
	if (test_and_clear_bit(NFS_LSEG_VALID, &lseg->pls_flags)) {
		/* Remove the reference keeping the lseg in the
		 * list.  It will now be removed when all
		 * outstanding io is finished.
		 */
		put_lseg_locked(lseg, tmp_list);
	}
}

/* Returns false if there was nothing to do, true otherwise */
static bool
pnfs_clear_lseg_list(struct pnfs_layout_hdr *lo, struct list_head *tmp_list,
		     struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg, *next;
	bool rv = false;

	dprintk("%s:Begin lo %p offset %llu length %llu iomode %d\n",
		__func__, lo, range->offset, range->length, range->iomode);

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry_safe(lseg, next, &lo->segs, fi_list)
		if (should_free_lseg(&lseg->range, range)) {
			dprintk("%s: freeing lseg %p iomode %d "
				"offset %llu length %llu\n", __func__,
				lseg, lseg->range.iomode, lseg->range.offset,
				lseg->range.length);
			mark_lseg_invalid(lseg, tmp_list);
			rv = true;
		}
	dprintk("%s:Return\n", __func__);
	return rv;
}

void
pnfs_free_lseg_list(struct list_head *free_me)
{
	struct pnfs_layout_segment *lseg, *tmp;

	list_for_each_entry_safe(lseg, tmp, free_me, fi_list)
		free_lseg(lseg);
	INIT_LIST_HEAD(free_me);
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
	LIST_HEAD(tmp_list);

	spin_lock(&nfsi->vfs_inode.i_lock);
	lo = nfsi->layout;
	if (lo) {
		pnfs_clear_lseg_list(lo, &tmp_list, &range);
		WARN_ON(!list_empty(&nfsi->layout->segs));
		WARN_ON(!list_empty(&nfsi->layout->layouts));
		WARN_ON(atomic_read(&nfsi->layout->plh_refcount) != 1);

		/* Matched by refcount set to 1 in alloc_init_layout_hdr */
		put_layout_hdr_locked(lo);
	}
	spin_unlock(&nfsi->vfs_inode.i_lock);
	pnfs_free_lseg_list(&tmp_list);
}

/*
 * Called by the state manger to remove all layouts established under an
 * expired lease.
 */
void
pnfs_destroy_all_layouts(struct nfs_client *clp)
{
	struct pnfs_layout_hdr *lo;
	LIST_HEAD(tmp_list);

	spin_lock(&clp->cl_lock);
	list_splice_init(&clp->cl_layouts, &tmp_list);
	spin_unlock(&clp->cl_lock);

	while (!list_empty(&tmp_list)) {
		lo = list_entry(tmp_list.next, struct pnfs_layout_hdr,
				layouts);
		dprintk("%s freeing layout for inode %lu\n", __func__,
			lo->inode->i_ino);
		pnfs_destroy_layout(NFS_I(lo->inode));
	}
}

/* update lo->stateid with new if is more recent */
void
pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo, const nfs4_stateid *new,
			bool update_barrier)
{
	u32 oldseq, newseq;

	assert_spin_locked(&lo->inode->i_lock);
	oldseq = be32_to_cpu(lo->stateid.stateid.seqid);
	newseq = be32_to_cpu(new->stateid.seqid);
	if ((int)(newseq - oldseq) > 0) {
		memcpy(&lo->stateid, &new->stateid, sizeof(new->stateid));
		if (update_barrier)
			lo->plh_barrier = be32_to_cpu(new->stateid.seqid);
		else {
			/* Because of wraparound, we want to keep the barrier
			 * "close" to the current seqids.  It needs to be
			 * within 2**31 to count as "behind", so if it
			 * gets too near that limit, give us a litle leeway
			 * and bring it to within 2**30.
			 * NOTE - and yes, this is all unsigned arithmetic.
			 */
			if (unlikely((newseq - lo->plh_barrier) > (3 << 29)))
				lo->plh_barrier = newseq - (1 << 30);
		}
	}
}

int
pnfs_choose_layoutget_stateid(nfs4_stateid *dst, struct pnfs_layout_hdr *lo,
			      struct nfs4_state *open_state)
{
	int status = 0;

	dprintk("--> %s\n", __func__);
	spin_lock(&lo->inode->i_lock);
	if (lo->plh_block_lgets ||
	    test_bit(NFS_LAYOUT_BULK_RECALL, &lo->plh_flags)) {
		/* We avoid -EAGAIN, as that has special meaning to
		 * some callers.
		 */
		status = -NFS4ERR_LAYOUTTRYLATER;
	} else if (list_empty(&lo->segs)) {
		int seq;

		do {
			seq = read_seqbegin(&open_state->seqlock);
			memcpy(dst->data, open_state->stateid.data,
			       sizeof(open_state->stateid.data));
		} while (read_seqretry(&open_state->seqlock, seq));
	} else
		memcpy(dst->data, lo->stateid.data, sizeof(lo->stateid.data));
	spin_unlock(&lo->inode->i_lock);
	dprintk("<-- %s\n", __func__);
	return status;
}

/*
* Get layout from server.
*    for now, assume that whole file layouts are requested.
*    arg->offset: 0
*    arg->length: all ones
*/
static struct pnfs_layout_segment *
send_layoutget(struct pnfs_layout_hdr *lo,
	   struct nfs_open_context *ctx,
	   struct pnfs_layout_range *range)
{
	struct inode *ino = lo->inode;
	struct nfs_server *server = NFS_SERVER(ino);
	struct nfs4_layoutget *lgp;
	struct pnfs_layout_segment *lseg = NULL;

	dprintk("--> %s\n", __func__);

	BUG_ON(ctx == NULL);
	lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
	if (lgp == NULL) {
		put_layout_hdr(lo);
		return NULL;
	}
	lgp->args.minlength = NFS4_MAX_UINT64;
	lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;
	lgp->args.range.iomode = range->iomode;
	lgp->args.range.offset = 0;
	lgp->args.range.length = NFS4_MAX_UINT64;
	lgp->args.type = server->pnfs_curr_ld->id;
	lgp->args.inode = ino;
	lgp->args.ctx = get_nfs_open_context(ctx);
	lgp->lsegpp = &lseg;

	/* Synchronously retrieve layout information from server and
	 * store in lseg.
	 */
	nfs4_proc_layoutget(lgp);
	if (!lseg) {
		/* remember that LAYOUTGET failed and suspend trying */
		set_bit(lo_fail_bit(range->iomode), &lo->plh_flags);
	}
	return lseg;
}

void nfs4_asynch_forget_layouts(struct pnfs_layout_hdr *lo,
				struct pnfs_layout_range *range,
				int notify_bit, atomic_t *notify_count,
				struct list_head *tmp_list)
{
	struct pnfs_layout_segment *lseg, *tmp;

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry_safe(lseg, tmp, &lo->segs, fi_list)
		if (should_free_lseg(&lseg->range, range)) {
			lseg->pls_notify_mask |= (1 << notify_bit);
			atomic_inc(notify_count);
			mark_lseg_invalid(lseg, tmp_list);
		}
}

/* Return true if there is layout based io in progress in the given range.
 * Assumes range has already been marked invalid, and layout marked to
 * prevent any new lseg from being inserted.
 */
bool
pnfs_return_layout_barrier(struct nfs_inode *nfsi,
			   struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg;
	bool ret = false;

	spin_lock(&nfsi->vfs_inode.i_lock);
	list_for_each_entry(lseg, &nfsi->layout->segs, fi_list)
		if (should_free_lseg(&lseg->range, range)) {
			ret = true;
			break;
		}
	spin_unlock(&nfsi->vfs_inode.i_lock);
	dprintk("%s:Return %d\n", __func__, ret);
	return ret;
}

static int
return_layout(struct inode *ino, struct pnfs_layout_range *range, bool wait)
{
	struct nfs4_layoutreturn *lrp;
	struct nfs_server *server = NFS_SERVER(ino);
	int status = -ENOMEM;

	dprintk("--> %s\n", __func__);

	lrp = kzalloc(sizeof(*lrp), GFP_KERNEL);
	if (lrp == NULL) {
		put_layout_hdr(NFS_I(ino)->layout);
		goto out;
	}
	lrp->args.reclaim = 0;
	lrp->args.layout_type = server->pnfs_curr_ld->id;
	lrp->args.return_type = RETURN_FILE;
	lrp->args.range = *range;
	lrp->args.inode = ino;
	lrp->clp = server->nfs_client;

	status = nfs4_proc_layoutreturn(lrp, wait);
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
}

/* Initiates a LAYOUTRETURN(FILE) */
int
_pnfs_return_layout(struct inode *ino, struct pnfs_layout_range *range,
		    bool wait)
{
	struct pnfs_layout_hdr *lo = NULL;
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_range arg;
	LIST_HEAD(tmp_list);
	int status = 0;

	dprintk("--> %s\n", __func__);

	arg.iomode = range ? range->iomode : IOMODE_ANY;
	arg.offset = 0;
	arg.length = NFS4_MAX_UINT64;

	spin_lock(&ino->i_lock);
	lo = nfsi->layout;
	if (!lo || !pnfs_clear_lseg_list(lo, &tmp_list, &arg)) {
		spin_unlock(&ino->i_lock);
		dprintk("%s: no layout segments to return\n", __func__);
		goto out;
	}
	lo->plh_block_lgets++;
	/* Reference matched in nfs4_layoutreturn_release */
	get_layout_hdr(lo);
	spin_unlock(&ino->i_lock);
	pnfs_free_lseg_list(&tmp_list);

	if (layoutcommit_needed(nfsi)) {
		status = pnfs_layoutcommit_inode(ino, wait);
		if (status) {
			/* Return layout even if layoutcommit fails */
			dprintk("%s: layoutcommit failed, status=%d. "
				"Returning layout anyway\n",
				__func__, status);
		}
	}
	status = return_layout(ino, &arg, wait);
out:
	dprintk("<-- %s status: %d\n", __func__, status);
	return status;
}

/*
 * Compare two layout segments for sorting into layout cache.
 * We want to preferentially return RW over RO layouts, so ensure those
 * are seen first.
 */
static s64
cmp_layout(struct pnfs_layout_range *l1,
	   struct pnfs_layout_range *l2)
{
	/* read > read/write */
	return (int)(l2->iomode == IOMODE_READ) -
		(int)(l1->iomode == IOMODE_READ);
}

static void
pnfs_insert_layout(struct pnfs_layout_hdr *lo,
		   struct pnfs_layout_segment *lseg)
{
	struct pnfs_layout_segment *lp;
	int found = 0;

	dprintk("%s:Begin\n", __func__);

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry(lp, &lo->segs, fi_list) {
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
		if (list_is_singular(&lo->segs) &&
		    !pnfs_layoutgets_blocked(lo, NULL))
			rpc_wake_up(&NFS_I(lo->inode)->lo_rpcwaitq_stateid);
		dprintk("%s: inserted lseg %p "
			"iomode %d offset %llu length %llu at tail\n",
			__func__, lseg, lseg->range.iomode,
			lseg->range.offset, lseg->range.length);
	}
	get_layout_hdr(lo);

	dprintk("%s:Return\n", __func__);
}

static struct pnfs_layout_hdr *
alloc_init_layout_hdr(struct inode *ino)
{
	struct pnfs_layout_hdr *lo;

	lo = kzalloc(sizeof(struct pnfs_layout_hdr), GFP_KERNEL);
	if (!lo)
		return NULL;
	atomic_set(&lo->plh_refcount, 1);
	INIT_LIST_HEAD(&lo->layouts);
	INIT_LIST_HEAD(&lo->segs);
	INIT_LIST_HEAD(&lo->plh_bulk_recall);
	lo->inode = ino;
	return lo;
}

static struct pnfs_layout_hdr *
pnfs_find_alloc_layout(struct inode *ino)
{
	struct nfs_inode *nfsi = NFS_I(ino);
	struct pnfs_layout_hdr *new = NULL;

	dprintk("%s Begin ino=%p layout=%p\n", __func__, ino, nfsi->layout);

	assert_spin_locked(&ino->i_lock);
	if (nfsi->layout)
		return nfsi->layout;

	spin_unlock(&ino->i_lock);
	new = alloc_init_layout_hdr(ino);
	spin_lock(&ino->i_lock);

	if (likely(nfsi->layout == NULL))	/* Won the race? */
		nfsi->layout = new;
	else
		kfree(new);
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
static int
is_matching_lseg(struct pnfs_layout_segment *lseg,
		 struct pnfs_layout_range *range)
{
	return (range->iomode != IOMODE_RW || lseg->range.iomode == IOMODE_RW);
}

/*
 * lookup range in layout
 */
static struct pnfs_layout_segment *
pnfs_find_lseg(struct pnfs_layout_hdr *lo,
		struct pnfs_layout_range *range)
{
	struct pnfs_layout_segment *lseg, *ret = NULL;

	dprintk("%s:Begin\n", __func__);

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry(lseg, &lo->segs, fi_list) {
		if (test_bit(NFS_LSEG_VALID, &lseg->pls_flags) &&
		    is_matching_lseg(lseg, range)) {
			get_lseg(lseg);
			ret = lseg;
			break;
		}
		if (cmp_layout(range, &lseg->range) > 0)
			break;
	}

	dprintk("%s:Return lseg %p ref %d valid %d\n",
		__func__, ret, ret ? atomic_read(&ret->pls_refcount) : 0,
		ret ? test_bit(NFS_LSEG_VALID, &ret->pls_flags) : 0);
	return ret;
}

/*
 * Layout segment is retreived from the server if not cached.
 * The appropriate layout segment is referenced and returned to the caller.
 */
struct pnfs_layout_segment *
pnfs_update_layout(struct inode *ino,
		   struct nfs_open_context *ctx,
		   enum pnfs_iomode iomode)
{
	struct pnfs_layout_range arg = {
		.iomode = iomode,
		.offset = 0,
		.length = NFS4_MAX_UINT64,
	};
	struct nfs_inode *nfsi = NFS_I(ino);
	struct nfs_client *clp = NFS_SERVER(ino)->nfs_client;
	struct pnfs_layout_hdr *lo;
	struct pnfs_layout_segment *lseg = NULL;

	if (!pnfs_enabled_sb(NFS_SERVER(ino)))
		return NULL;
	spin_lock(&ino->i_lock);
	lo = pnfs_find_alloc_layout(ino);
	if (lo == NULL) {
		dprintk("%s ERROR: can't get pnfs_layout_hdr\n", __func__);
		goto out_unlock;
	}

	/* Check to see if the layout for the given range already exists */
	lseg = pnfs_find_lseg(lo, &arg);
	if (lseg)
		goto out_unlock;

	/* if LAYOUTGET already failed once we don't try again */
	if (test_bit(lo_fail_bit(iomode), &nfsi->layout->plh_flags))
		goto out_unlock;

	get_layout_hdr(lo); /* Matched in pnfs_layoutget_release */
	if (list_empty(&lo->segs)) {
		/* The lo must be on the clp list if there is any
		 * chance of a CB_LAYOUTRECALL(FILE) coming in.
		 */
		spin_lock(&clp->cl_lock);
		BUG_ON(!list_empty(&lo->layouts));
		list_add_tail(&lo->layouts, &clp->cl_layouts);
		spin_unlock(&clp->cl_lock);
	}
	spin_unlock(&ino->i_lock);

	lseg = send_layoutget(lo, ctx, &arg);
	if (!lseg) {
		spin_lock(&ino->i_lock);
		if (list_empty(&lo->segs)) {
			spin_lock(&clp->cl_lock);
			list_del_init(&lo->layouts);
			spin_unlock(&clp->cl_lock);
			clear_bit(NFS_LAYOUT_BULK_RECALL, &lo->plh_flags);
		}
		spin_unlock(&ino->i_lock);
	}
out:
	dprintk("%s end, state 0x%lx lseg %p\n", __func__,
		nfsi->layout->plh_flags, lseg);
	return lseg;
out_unlock:
	spin_unlock(&ino->i_lock);
	goto out;
}

bool
pnfs_layoutgets_blocked(struct pnfs_layout_hdr *lo, nfs4_stateid *stateid)
{
	assert_spin_locked(&lo->inode->i_lock);
	if ((stateid) &&
	    (int)(lo->plh_barrier - be32_to_cpu(stateid->stateid.seqid)) >= 0)
		return true;
	return lo->plh_block_lgets ||
		test_bit(NFS_LAYOUT_BULK_RECALL, &lo->plh_flags) ||
		(list_empty(&lo->segs) &&
		 (atomic_read(&lo->plh_outstanding) != 0));
}

int
pnfs_layout_process(struct nfs4_layoutget *lgp)
{
	struct pnfs_layout_hdr *lo = NFS_I(lgp->args.inode)->layout;
	struct nfs4_layoutget_res *res = &lgp->res;
	struct pnfs_layout_segment *lseg;
	struct inode *ino = lo->inode;
	struct nfs_client *clp = NFS_SERVER(ino)->nfs_client;
	int status = 0;

	/* Inject layout blob into I/O device driver */
	lseg = NFS_SERVER(ino)->pnfs_curr_ld->alloc_lseg(lo, res);
	if (!lseg || IS_ERR(lseg)) {
		if (!lseg)
			status = -ENOMEM;
		else
			status = PTR_ERR(lseg);
		dprintk("%s: Could not allocate layout: error %d\n",
		       __func__, status);
		spin_lock(&ino->i_lock);
		goto out;
	}

	spin_lock(&ino->i_lock);
	/* decrement needs to be done before call to pnfs_layoutget_blocked */
	atomic_dec(&lo->plh_outstanding);
	spin_lock(&clp->cl_lock);
	if (matches_outstanding_recall(ino, &res->range)) {
		spin_unlock(&clp->cl_lock);
		dprintk("%s forget reply due to recall\n", __func__);
		goto out_forget_reply;
	}
	spin_unlock(&clp->cl_lock);

	if (pnfs_layoutgets_blocked(lo, &res->stateid)) {
		dprintk("%s forget reply due to state\n", __func__);
		goto out_forget_reply;
	}
	init_lseg(lo, lseg);
	lseg->range = res->range;
	get_lseg(lseg);
	*lgp->lsegpp = lseg;
	pnfs_insert_layout(lo, lseg);

	if (res->return_on_close) {
		/* FI: This needs to be re-examined.  At lo level,
		 * all it needs is a bit indicating whether any of
		 * the lsegs in the list have the flags set.
		 */
		lo->roc_iomode |= res->range.iomode;
	}

	/* Done processing layoutget. Set the layout stateid */
	pnfs_set_layout_stateid(lo, &res->stateid, false);
out:
	if (!pnfs_layoutgets_blocked(lo, NULL))
		rpc_wake_up(&NFS_I(ino)->lo_rpcwaitq_stateid);
	spin_unlock(&ino->i_lock);
	return status;

out_forget_reply:
	spin_unlock(&ino->i_lock);
	lseg->layout = lo;
	NFS_SERVER(ino)->pnfs_curr_ld->free_lseg(lseg);
	spin_lock(&ino->i_lock);
	goto out;
}

void
pnfs_set_pg_test(struct inode *inode, struct nfs_pageio_descriptor *pgio)
{
	struct pnfs_layout_hdr *lo;
	struct pnfs_layoutdriver_type *ld;

	pgio->pg_test = NULL;

	lo = NFS_I(inode)->layout;
	ld = NFS_SERVER(inode)->pnfs_curr_ld;
	if (!ld || !lo)
		return;

	pgio->pg_test = ld->pg_test;
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

	pgio->pg_iswrite = 0;
	pgio->pg_test = NULL;
	pgio->pg_lseg = NULL;

	if (!pnfs_enabled_sb(nfss))
		return;

	pgio->pg_lseg = pnfs_update_layout(inode, ctx, IOMODE_READ);
	if (!pgio->pg_lseg)
		return;

	pnfs_set_pg_test(inode, pgio);
}

void
pnfs_pageio_init_write(struct nfs_pageio_descriptor *pgio, struct inode *inode)
{
	struct nfs_server *server = NFS_SERVER(inode);

	pgio->pg_iswrite = 1;
	if (!pnfs_enabled_sb(server))
		pgio->pg_test = NULL;
	else
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
	trypnfs = nfss->pnfs_curr_ld->write_pagelist(wdata,
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
	trypnfs = nfss->pnfs_curr_ld->read_pagelist(rdata,
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
	trypnfs = nfss->pnfs_curr_ld->commit(data, sync);
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

	data = kzalloc(sizeof(*data), GFP_NOFS);
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
	__clear_bit(NFS_LAYOUT_NEED_LCOMMIT, &nfsi->layout->plh_flags);
	memcpy(data->args.stateid.data, nfsi->layout->stateid.data,
	       NFS4_STATEID_SIZE);

	/* Reference for layoutcommit matched in pnfs_layoutcommit_release */
	get_layout_hdr(NFS_I(inode)->layout);

	spin_unlock(&inode->i_lock);

	/* Set up layout commit args */
	status = pnfs_layoutcommit_setup(inode, data, write_begin_pos,
					 write_end_pos);
	if (status) {
		/* The layout driver failed to setup the layoutcommit */
		put_rpccred(data->cred);
		put_layout_hdr(NFS_I(inode)->layout);
		goto out_free;
	}
	status = nfs4_proc_layoutcommit(data, sync);
out:
	dprintk("%s end (err:%d)\n", __func__, status);
	return status;
out_free:
	kfree(data);
	goto out;
}

/*
 * Device ID cache. Currently supports one layout type per struct nfs_client.
 * Add layout type to the lookup key to expand to support multiple types.
 */
int
pnfs_alloc_init_deviceid_cache(struct nfs_client *clp,
			 void (*free_callback)(struct pnfs_deviceid_node *))
{
	struct pnfs_deviceid_cache *c;

	c = kzalloc(sizeof(struct pnfs_deviceid_cache), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	spin_lock(&clp->cl_lock);
	if (clp->cl_devid_cache != NULL) {
		atomic_inc(&clp->cl_devid_cache->dc_ref);
		dprintk("%s [kref [%d]]\n", __func__,
			atomic_read(&clp->cl_devid_cache->dc_ref));
		kfree(c);
	} else {
		/* kzalloc initializes hlists */
		spin_lock_init(&c->dc_lock);
		atomic_set(&c->dc_ref, 1);
		c->dc_free_callback = free_callback;
		clp->cl_devid_cache = c;
		dprintk("%s [new]\n", __func__);
	}
	spin_unlock(&clp->cl_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(pnfs_alloc_init_deviceid_cache);

/*
 * Called from pnfs_layoutdriver_type->free_lseg
 * last layout segment reference frees deviceid
 */
void
pnfs_put_deviceid(struct pnfs_deviceid_cache *c,
		  struct pnfs_deviceid_node *devid)
{
	struct nfs4_deviceid *id = &devid->de_id;
	struct pnfs_deviceid_node *d;
	struct hlist_node *n;
	long h = nfs4_deviceid_hash(id);

	dprintk("%s [%d]\n", __func__, atomic_read(&devid->de_ref));
	if (!atomic_dec_and_lock(&devid->de_ref, &c->dc_lock))
		return;

	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[h], de_node)
		if (!memcmp(&d->de_id, id, sizeof(*id))) {
			hlist_del_rcu(&d->de_node);
			spin_unlock(&c->dc_lock);
			synchronize_rcu();
			c->dc_free_callback(devid);
			return;
		}
	spin_unlock(&c->dc_lock);
	/* Why wasn't it found in  the list? */
	BUG();
}
EXPORT_SYMBOL_GPL(pnfs_put_deviceid);

/* Find and reference a deviceid */
struct pnfs_deviceid_node *
pnfs_find_get_deviceid(struct pnfs_deviceid_cache *c, struct nfs4_deviceid *id)
{
	struct pnfs_deviceid_node *d;
	struct hlist_node *n;
	long hash = nfs4_deviceid_hash(id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	rcu_read_lock();
	hlist_for_each_entry_rcu(d, n, &c->dc_deviceids[hash], de_node) {
		if (!memcmp(&d->de_id, id, sizeof(*id))) {
			if (!atomic_inc_not_zero(&d->de_ref)) {
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
EXPORT_SYMBOL_GPL(pnfs_find_get_deviceid);

/*
 * Add a deviceid to the cache.
 * GETDEVICEINFOs for same deviceid can race. If deviceid is found, discard new
 */
struct pnfs_deviceid_node *
pnfs_add_deviceid(struct pnfs_deviceid_cache *c, struct pnfs_deviceid_node *new)
{
	struct pnfs_deviceid_node *d;
	long hash = nfs4_deviceid_hash(&new->de_id);

	dprintk("--> %s hash %ld\n", __func__, hash);
	spin_lock(&c->dc_lock);
	d = pnfs_find_get_deviceid(c, &new->de_id);
	if (d) {
		spin_unlock(&c->dc_lock);
		dprintk("%s [discard]\n", __func__);
		c->dc_free_callback(new);
		return d;
	}
	INIT_HLIST_NODE(&new->de_node);
	atomic_set(&new->de_ref, 1);
	hlist_add_head_rcu(&new->de_node, &c->dc_deviceids[hash]);
	spin_unlock(&c->dc_lock);
	dprintk("%s [new]\n", __func__);
	return new;
}
EXPORT_SYMBOL_GPL(pnfs_add_deviceid);

void
pnfs_put_deviceid_cache(struct nfs_client *clp)
{
	struct pnfs_deviceid_cache *local = clp->cl_devid_cache;

	dprintk("--> %s cl_devid_cache %p\n", __func__, clp->cl_devid_cache);
	if (atomic_dec_and_lock(&local->dc_ref, &clp->cl_lock)) {
		int i;
		/* Verify cache is empty */
		for (i = 0; i < NFS4_DEVICE_ID_HASH_SIZE; i++)
			BUG_ON(!hlist_empty(&local->dc_deviceids[i]));
		clp->cl_devid_cache = NULL;
		spin_unlock(&clp->cl_lock);
		kfree(local);
	}
}
EXPORT_SYMBOL_GPL(pnfs_put_deviceid_cache);
