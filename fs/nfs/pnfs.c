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

static void
get_layout_hdr_locked(struct pnfs_layout_hdr *lo)
{
	assert_spin_locked(&lo->inode->i_lock);
	lo->refcount++;
}

static void
put_layout_hdr_locked(struct pnfs_layout_hdr *lo)
{
	assert_spin_locked(&lo->inode->i_lock);
	BUG_ON(lo->refcount == 0);

	lo->refcount--;
	if (!lo->refcount) {
		dprintk("%s: freeing layout cache %p\n", __func__, lo);
		BUG_ON(!list_empty(&lo->layouts));
		NFS_I(lo->inode)->layout = NULL;
		kfree(lo);
	}
}

void
put_layout_hdr(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	put_layout_hdr_locked(NFS_I(inode)->layout);
	spin_unlock(&inode->i_lock);
}

static void
init_lseg(struct pnfs_layout_hdr *lo, struct pnfs_layout_segment *lseg)
{
	INIT_LIST_HEAD(&lseg->fi_list);
	atomic_set(&lseg->pls_refcount, 1);
	smp_mb();
	lseg->valid = true;
	lseg->layout = lo;
}

static void
put_lseg(struct pnfs_layout_segment *lseg)
{
	bool do_wake_up;
	struct inode *ino;

	if (!lseg)
		return;

	dprintk("%s: lseg %p ref %d valid %d\n", __func__, lseg,
		atomic_read(&lseg->pls_refcount), lseg->valid);
	do_wake_up = !lseg->valid;
	ino = lseg->layout->inode;
	if (atomic_dec_and_test(&lseg->pls_refcount)) {
		NFS_SERVER(ino)->pnfs_curr_ld->free_lseg(lseg);
		/* Matched by get_layout_hdr_locked in pnfs_insert_layout */
		put_layout_hdr(ino);
	}
	if (do_wake_up)
		rpc_wake_up(&NFS_I(ino)->lo_rpcwaitq);
}

static int
should_free_lseg(struct pnfs_layout_range *lseg_range, u32 iomode)
{
	return (iomode == IOMODE_ANY ||
		lseg_range->iomode == iomode);
}

static bool
_pnfs_can_return_lseg(struct pnfs_layout_segment *lseg)
{
	return atomic_read(&lseg->pls_refcount) == 1;
}

static void
pnfs_clear_lseg_list(struct pnfs_layout_hdr *lo, struct list_head *tmp_list,
		     u32 iomode)
{
	struct pnfs_layout_segment *lseg, *next;

	dprintk("%s:Begin lo %p\n", __func__, lo);

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry_safe(lseg, next, &lo->segs, fi_list) {
		if (!should_free_lseg(&lseg->range, iomode) ||
		    !_pnfs_can_return_lseg(lseg))
			continue;
		dprintk("%s: freeing lseg %p iomode %d\n", __func__,
			lseg, lseg->range.iomode);
		list_move(&lseg->fi_list, tmp_list);
	}
	if (list_empty(&lo->segs)) {
		struct nfs_client *clp;

		clp = NFS_SERVER(lo->inode)->nfs_client;
		spin_lock(&clp->cl_lock);
		/* List does not take a reference, so no need for put here */
		list_del_init(&lo->layouts);
		spin_unlock(&clp->cl_lock);
		pnfs_invalidate_layout_stateid(lo);
	}

	dprintk("%s:Return\n", __func__);
}

static void
pnfs_free_lseg_list(struct list_head *tmp_list)
{
	struct pnfs_layout_segment *lseg;

	while (!list_empty(tmp_list)) {
		lseg = list_entry(tmp_list->next, struct pnfs_layout_segment,
				fi_list);
		dprintk("%s calling put_lseg on %p\n", __func__, lseg);
		list_del(&lseg->fi_list);
		put_lseg(lseg);
	}
}

void
pnfs_destroy_layout(struct nfs_inode *nfsi)
{
	struct pnfs_layout_hdr *lo;
	LIST_HEAD(tmp_list);

	spin_lock(&nfsi->vfs_inode.i_lock);
	lo = nfsi->layout;
	if (lo) {
		pnfs_clear_lseg_list(lo, &tmp_list, IOMODE_ANY);
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

/* update lo->stateid with new if is more recent
 *
 * lo->stateid could be the open stateid, in which case we just use what given.
 */
static void
pnfs_set_layout_stateid(struct pnfs_layout_hdr *lo,
			const nfs4_stateid *new)
{
	nfs4_stateid *old = &lo->stateid;
	bool overwrite = false;

	if (!test_bit(NFS_LAYOUT_STATEID_SET, &lo->state) ||
	    memcmp(old->stateid.other, new->stateid.other, sizeof(new->stateid.other)))
		overwrite = true;
	else {
		u32 oldseq, newseq;

		oldseq = be32_to_cpu(old->stateid.seqid);
		newseq = be32_to_cpu(new->stateid.seqid);
		if ((int)(newseq - oldseq) > 0)
			overwrite = true;
	}
	if (overwrite)
		memcpy(&old->stateid, &new->stateid, sizeof(new->stateid));
}

void
pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_hdr *lo,
			struct nfs4_state *open_state)
{
	dprintk("--> %s\n", __func__);
	spin_lock(&lo->inode->i_lock);
	if (!test_bit(NFS_LAYOUT_STATEID_SET, &lo->state)) {
		int seq;

		do {
			seq = read_seqbegin(&open_state->seqlock);
			memcpy(dst->data, open_state->stateid.data,
			       sizeof(open_state->stateid.data));
		} while (read_seqretry(&open_state->seqlock, seq));
		set_bit(NFS_LAYOUT_STATEID_SET, &lo->state);
	} else
		memcpy(dst->data, lo->stateid.data,
		       sizeof(lo->stateid.data));
	spin_unlock(&lo->inode->i_lock);
	dprintk("<-- %s\n", __func__);
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
	   u32 iomode)
{
	struct inode *ino = lo->inode;
	struct nfs_server *server = NFS_SERVER(ino);
	struct nfs4_layoutget *lgp;
	struct pnfs_layout_segment *lseg = NULL;

	dprintk("--> %s\n", __func__);

	BUG_ON(ctx == NULL);
	lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
	if (lgp == NULL) {
		put_layout_hdr(ino);
		return NULL;
	}
	lgp->args.minlength = NFS4_MAX_UINT64;
	lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;
	lgp->args.range.iomode = iomode;
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
		set_bit(lo_fail_bit(iomode), &lo->state);
	}
	return lseg;
}

static struct pnfs_layout_segment *
has_layout_to_return(struct pnfs_layout_hdr *lo, u32 iomode)
{
	struct pnfs_layout_segment *out = NULL, *lseg;
	dprintk("%s:Begin lo %p iomode %d\n", __func__, lo, iomode);

	list_for_each_entry(lseg, &lo->segs, fi_list)
		if (should_free_lseg(&lseg->range, iomode)) {
			out = lseg;
			break;
		}

	dprintk("%s:Return lseg=%p\n", __func__, out);
	return out;
}

bool
pnfs_return_layout_barrier(struct nfs_inode *nfsi, u32 iomode)
{
	struct pnfs_layout_segment *lseg;
	bool ret = false;

	spin_lock(&nfsi->vfs_inode.i_lock);
	list_for_each_entry(lseg, &nfsi->layout->segs, fi_list) {
		if (!should_free_lseg(&lseg->range, iomode))
			continue;
		lseg->valid = false;
		if (!_pnfs_can_return_lseg(lseg)) {
			dprintk("%s: wait on lseg %p refcount %d\n",
				__func__, lseg,
				atomic_read(&lseg->pls_refcount));
			ret = true;
		}
	}
	spin_unlock(&nfsi->vfs_inode.i_lock);
	dprintk("%s:Return %d\n", __func__, ret);
	return ret;
}

void
pnfs_layoutreturn_release(struct nfs4_layoutreturn *lrp)
{
	struct pnfs_layout_hdr *lo;
	LIST_HEAD(tmp_list);

	if (lrp->args.return_type != RETURN_FILE)
		return;
	lo = NFS_I(lrp->args.inode)->layout;
	spin_lock(&lrp->args.inode->i_lock);
	pnfs_clear_lseg_list(lo, &tmp_list, lrp->args.range.iomode);
	if (!lrp->res.valid)
		;	/* forgetful model internal release */
	else if (!lrp->res.lrs_present)
		pnfs_invalidate_layout_stateid(lo);
	else
		pnfs_set_layout_stateid(lo, &lrp->res.stateid);
	put_layout_hdr_locked(lo); /* Matched in _pnfs_return_layout */
	spin_unlock(&lrp->args.inode->i_lock);
	pnfs_free_lseg_list(&tmp_list);
}

static int
return_layout(struct inode *ino, struct pnfs_layout_range *range,
	      enum pnfs_layoutreturn_type type, struct pnfs_layout_hdr *lo,
	      bool wait, const nfs4_stateid *stateid)
{
	struct nfs4_layoutreturn *lrp;
	struct nfs_server *server = NFS_SERVER(ino);
	int status = -ENOMEM;

	dprintk("--> %s\n", __func__);

	BUG_ON(type != RETURN_FILE);

	lrp = kzalloc(sizeof(*lrp), GFP_KERNEL);
	if (lrp == NULL) {
		if (lo && (type == RETURN_FILE))
			put_layout_hdr(lo->inode);
		goto out;
	}
	lrp->args.reclaim = 0;
	lrp->args.layout_type = server->pnfs_curr_ld->id;
	lrp->args.return_type = type;
	lrp->args.range = *range;
	lrp->args.inode = ino;
	lrp->stateid = stateid;
	lrp->clp = server->nfs_client;

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
		if (lo && !has_layout_to_return(lo, arg.iomode))
			lo = NULL;
		if (!lo) {
			spin_unlock(&ino->i_lock);
			dprintk("%s: no layout segments to return\n", __func__);
			goto out;
		}

		/* Reference matched in pnfs_layoutreturn_release */
		get_layout_hdr_locked(lo);

		spin_unlock(&ino->i_lock);

		status = return_layout(ino, &arg, type, lo, wait, stateid);
	}
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
cmp_layout(u32 iomode1, u32 iomode2)
{
	/* read > read/write */
	return (int)(iomode2 == IOMODE_READ) - (int)(iomode1 == IOMODE_READ);
}

static void
pnfs_insert_layout(struct pnfs_layout_hdr *lo,
		   struct pnfs_layout_segment *lseg)
{
	struct pnfs_layout_segment *lp;
	int found = 0;

	dprintk("%s:Begin\n", __func__);

	assert_spin_locked(&lo->inode->i_lock);
	if (list_empty(&lo->segs)) {
		struct nfs_client *clp = NFS_SERVER(lo->inode)->nfs_client;

		spin_lock(&clp->cl_lock);
		BUG_ON(!list_empty(&lo->layouts));
		list_add_tail(&lo->layouts, &clp->cl_layouts);
		spin_unlock(&clp->cl_lock);
	}
	list_for_each_entry(lp, &lo->segs, fi_list) {
		if (cmp_layout(lp->range.iomode, lseg->range.iomode) > 0)
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
	get_layout_hdr_locked(lo);

	dprintk("%s:Return\n", __func__);
}

static struct pnfs_layout_hdr *
alloc_init_layout_hdr(struct inode *ino)
{
	struct pnfs_layout_hdr *lo;

	lo = kzalloc(sizeof(struct pnfs_layout_hdr), GFP_KERNEL);
	if (!lo)
		return NULL;
	lo->refcount = 1;
	INIT_LIST_HEAD(&lo->layouts);
	INIT_LIST_HEAD(&lo->segs);
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
 * iomode	lseg	match
 * -----	-----	-----
 * ANY		READ	true
 * ANY		RW	true
 * RW		READ	false
 * RW		RW	true
 * READ		READ	true
 * READ		RW	true
 */
static int
is_matching_lseg(struct pnfs_layout_segment *lseg, u32 iomode)
{
	return (iomode != IOMODE_RW || lseg->range.iomode == IOMODE_RW);
}

/*
 * lookup range in layout
 */
struct pnfs_layout_segment *
pnfs_has_layout(struct pnfs_layout_hdr *lo, u32 iomode)
{
	struct pnfs_layout_segment *lseg, *ret = NULL;

	dprintk("%s:Begin\n", __func__);

	assert_spin_locked(&lo->inode->i_lock);
	list_for_each_entry(lseg, &lo->segs, fi_list) {
		if (is_matching_lseg(lseg, iomode)) {
			ret = lseg;
			break;
		}
		if (cmp_layout(iomode, lseg->range.iomode) > 0)
			break;
	}

	dprintk("%s:Return lseg %p ref %d valid %d\n",
		__func__, ret, ret ? atomic_read(&ret->pls_refcount) : 0,
		ret ? ret->valid : 0);
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
	struct nfs_inode *nfsi = NFS_I(ino);
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
	lseg = pnfs_has_layout(lo, iomode);
	if (lseg) {
		if (lseg->valid) {
			dprintk("%s: Using cached lseg %p "
				"iomode %d)\n",
				__func__,
				lseg,
				iomode);
			get_lseg(lseg);
			goto out_unlock;
		}
		/* someone is cleaning the layout */
		lseg = NULL;
	}

	/* if LAYOUTGET already failed once we don't try again */
	if (test_bit(lo_fail_bit(iomode), &nfsi->layout->state))
		goto out_unlock;

	get_layout_hdr_locked(lo); /* Matched in pnfs_layoutget_release */
	spin_unlock(&ino->i_lock);

	lseg = send_layoutget(lo, ctx, iomode);
out:
	dprintk("%s end, state 0x%lx lseg %p\n", __func__,
		nfsi->layout->state, lseg);
	put_lseg(lseg); /* STUB - callers currently ignore return value */
	return lseg;
out_unlock:
	spin_unlock(&ino->i_lock);
	goto out;
}

int
pnfs_layout_process(struct nfs4_layoutget *lgp)
{
	struct pnfs_layout_hdr *lo = NFS_I(lgp->args.inode)->layout;
	struct nfs4_layoutget_res *res = &lgp->res;
	struct pnfs_layout_segment *lseg;
	struct inode *ino = lo->inode;
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
		goto out;
	}

	spin_lock(&ino->i_lock);
	init_lseg(lo, lseg);
	lseg->range = res->range;
	get_lseg(lseg);
	*lgp->lsegpp = lseg;
	pnfs_insert_layout(lo, lseg);

	if (res->return_on_close) {
		/* FI: This needs to be re-examined.  At lo level,
		 * all it needs is a bit indicating whether any of
		 * the lsegs in the list have the flags set.
		 *
		 * The IOMODE_ANY line just seems nonsensical.
		 */
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
