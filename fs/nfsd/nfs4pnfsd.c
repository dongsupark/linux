/******************************************************************************
 *
 * (c) 2007 Network Appliance, Inc.  All Rights Reserved.
 * (c) 2009 NetApp.  All Rights Reserved.
 *
 * NetApp provides this source code under the GPL v2 License.
 * The GPL v2 license is available at
 * http://opensource.org/licenses/gpl-license.php.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ******************************************************************************/

#if defined(CONFIG_PNFSD)

#include <linux/param.h>
#include <linux/slab.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/debug.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfs4.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/xdr4.h>
#include <linux/exportfs.h>
#include <linux/nfsd/pnfsd.h>

#define NFSDDBG_FACILITY                NFSDDBG_PROC

/* Globals */
static u32 current_layoutid = 1;

/*
 * Layout state - NFSv4.1 pNFS
 */
static struct kmem_cache *pnfs_layout_slab;

/*
 * Currently used for manipulating the layout state.
 */
static DEFINE_SPINLOCK(layout_lock);

void
nfsd4_free_pnfs_slabs(void)
{
	nfsd4_free_slab(&pnfs_layout_slab);
}

int
nfsd4_init_pnfs_slabs(void)
{
	pnfs_layout_slab = kmem_cache_create("pnfs_layouts",
			sizeof(struct nfs4_layout), 0, 0, NULL);
	if (pnfs_layout_slab == NULL)
		return -ENOMEM;
	return 0;
}

static struct nfs4_file *
find_alloc_file(struct inode *ino, struct svc_fh *current_fh)
{
	struct nfs4_file *fp;

	fp = find_file(ino);
	if (fp)
		return fp;

	return alloc_init_file(ino, current_fh);
}

static struct nfs4_layout_state *
alloc_init_layout_state(struct nfs4_client *clp, struct nfs4_file *fp,
			stateid_t *stateid)
{
	struct nfs4_layout_state *new;

	/* FIXME: use a kmem_cache */
	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return new;
	get_nfs4_file(fp);
	INIT_LIST_HEAD(&new->ls_perfile);
	INIT_LIST_HEAD(&new->ls_layouts);
	kref_init(&new->ls_ref);
	new->ls_client = clp;
	new->ls_file = fp;
	new->ls_stateid.si_boot = stateid->si_boot;
	new->ls_stateid.si_stateownerid = 0; /* identifies layout stateid */
	new->ls_stateid.si_generation = 1;
	spin_lock(&layout_lock);
	new->ls_stateid.si_fileid = current_layoutid++;
	list_add(&new->ls_perfile, &fp->fi_layout_states);
	spin_unlock(&layout_lock);
	return new;
}

static inline void
get_layout_state(struct nfs4_layout_state *ls)
{
	kref_get(&ls->ls_ref);
}

static void
destroy_layout_state_common(struct nfs4_layout_state *ls)
{
	struct nfs4_file *fp = ls->ls_file;

	dprintk("pNFS %s: ls %p fp %p clp %p\n", __func__, ls, fp,
		ls->ls_client);
	BUG_ON(!list_empty(&ls->ls_layouts));
	kfree(ls);
	put_nfs4_file(fp);
}

static void
destroy_layout_state(struct kref *kref)
{
	struct nfs4_layout_state *ls =
			container_of(kref, struct nfs4_layout_state, ls_ref);

	spin_lock(&layout_lock);
	list_del(&ls->ls_perfile);
	spin_unlock(&layout_lock);
	destroy_layout_state_common(ls);
}

static void
destroy_layout_state_locked(struct kref *kref)
{
	struct nfs4_layout_state *ls =
			container_of(kref, struct nfs4_layout_state, ls_ref);

	list_del(&ls->ls_perfile);
	destroy_layout_state_common(ls);
}

static inline void
put_layout_state(struct nfs4_layout_state *ls)
{
	dprintk("pNFS %s: ls %p ls_ref %d\n", __func__, ls,
		atomic_read(&ls->ls_ref.refcount));
	kref_put(&ls->ls_ref, destroy_layout_state);
}

static inline void
put_layout_state_locked(struct nfs4_layout_state *ls)
{
	dprintk("pNFS %s: ls %p ls_ref %d\n", __func__, ls,
		atomic_read(&ls->ls_ref.refcount));
	kref_put(&ls->ls_ref, destroy_layout_state_locked);
}

/*
 * Search the fp->fi_layout_state list for a layout state with the clientid.
 * If not found, then this is a 'first open/delegation/lock stateid' from
 * the client for this file.
 * Called under the layout_lock.
 */
static struct nfs4_layout_state *
find_get_layout_state(struct nfs4_client *clp, struct nfs4_file *fp)
{
	struct nfs4_layout_state *ls;

	list_for_each_entry(ls, &fp->fi_layout_states, ls_perfile) {
		if (ls->ls_client == clp) {
			dprintk("pNFS %s: before GET ls %p ls_ref %d\n",
				__func__, ls,
				atomic_read(&ls->ls_ref.refcount));
			get_layout_state(ls);
			return ls;
		}
	}
	return NULL;
}

static int
verify_stateid(struct nfs4_file *fp, stateid_t *stateid)
{
	struct nfs4_stateid *local = NULL;
	struct nfs4_delegation *temp = NULL;

	/* check if open or lock stateid */
	local = find_stateid(stateid, RD_STATE);
	if (local)
		return 0;
	temp = find_delegation_stateid(fp->fi_inode, stateid);
	if (temp)
		return 0;
	return nfserr_bad_stateid;
}

/*
 * nfs4_preocess_layout_stateid ()
 *
 * We have looked up the nfs4_file corresponding to the current_fh, and
 * confirmed the clientid. Pull the few tests from nfs4_preprocess_stateid_op()
 * that make sense with a layout stateid.
 *
 * Called with the state_lock held
 * Returns zero and stateid is updated, or error.
 *
 * Note: the struct nfs4_layout_state pointer is only set by layoutget.
 */
static __be32
nfs4_process_layout_stateid(struct nfs4_client *clp, struct nfs4_file *fp,
			    stateid_t *stateid, struct nfs4_layout_state **lsp)
{
	struct nfs4_layout_state *ls = NULL;
	__be32 status = 0;

	dprintk("--> %s clp %p fp %p \n", __func__, clp, fp);

	dprintk("%s: operation stateid=" STATEID_FMT "\n", __func__,
		STATEID_VAL(stateid));

	status = nfs4_check_stateid(stateid);
	if (status)
		goto out;

	/* Is this the first use of this layout ? */
	spin_lock(&layout_lock);
	ls = find_get_layout_state(clp, fp);
	spin_unlock(&layout_lock);
	if (!ls) {
		/* Only alloc layout state on layoutget (which sets lsp). */
		if (!lsp) {
			dprintk("%s ERROR: Not layoutget & no layout stateid\n",
				__func__);
			status = nfserr_bad_stateid;
			goto out;
		}
		dprintk("%s Initial stateid for layout: file %p client %p\n",
			__func__, fp, clp);

		/* verify input stateid */
		status = verify_stateid(fp, stateid);
		if (status < 0) {
			dprintk("%s ERROR: invalid open/deleg/lock stateid\n",
				__func__);
			goto out;
		}
		ls = alloc_init_layout_state(clp, fp, stateid);
		if (!ls) {
			dprintk("%s pNFS ERROR: no memory for layout state\n",
				__func__);
			status = nfserr_resource;
			goto out;
		}
	} else {
		dprintk("%s Not initial stateid. Layout state %p file %p\n",
			__func__, ls, fp);

		/* BAD STATEID */
		status = nfserr_bad_stateid;
		if (memcmp(&ls->ls_stateid.si_opaque, &stateid->si_opaque,
			sizeof(stateid_opaque_t)) != 0) {

			/* if a LAYOUTGET operation and stateid is a valid
			 * open/deleg/lock stateid, accept it as a parallel
			 * initial layout stateid
			 */
			if (lsp && ((verify_stateid(fp, stateid)) == 0)) {
				dprintk("%s parallel initial layout state\n",
					__func__);
				goto update;
			}

			dprintk("%s ERROR bad opaque in stateid 1\n", __func__);
			goto out_put;
		}

		/* stateid is a valid layout stateid for this file. */
		if (stateid->si_generation > ls->ls_stateid.si_generation) {
			dprintk("%s bad stateid 1\n", __func__);
			goto out_put;
		}
update:
		update_stateid(&ls->ls_stateid);
		dprintk("%s Updated ls_stateid to %d on layoutstate %p\n",
			__func__, ls->ls_stateid.si_generation, ls);
	}
	status = 0;
	/* Set the stateid to be encoded */
	memcpy(stateid, &ls->ls_stateid, sizeof(stateid_t));

	/* Return the layout state if requested */
	if (lsp) {
		get_layout_state(ls);
		*lsp = ls;
	}
	dprintk("%s: layout stateid=" STATEID_FMT "\n", __func__,
		STATEID_VAL(&ls->ls_stateid));
out_put:
	dprintk("%s PUT LO STATE:\n", __func__);
	put_layout_state(ls);
out:
	dprintk("<-- %s status %d\n", __func__, htonl(status));

	return status;
}

static inline struct nfs4_layout *
alloc_layout(void)
{
	return kmem_cache_alloc(pnfs_layout_slab, GFP_KERNEL);
}

static inline void
free_layout(struct nfs4_layout *lp)
{
	kmem_cache_free(pnfs_layout_slab, lp);
}

static void
init_layout(struct nfs4_layout_state *ls,
	    struct nfs4_layout *lp,
	    struct nfs4_file *fp,
	    struct nfs4_client *clp,
	    struct svc_fh *current_fh,
	    struct nfsd4_layout_seg *seg)
{
	dprintk("pNFS %s: ls %p lp %p clp %p fp %p ino %p\n", __func__,
		ls, lp, clp, fp, fp->fi_inode);

	get_nfs4_file(fp);
	lp->lo_client = clp;
	lp->lo_file = fp;
	get_layout_state(ls);
	lp->lo_state = ls;
	memcpy(&lp->lo_seg, seg, sizeof(lp->lo_seg));
	spin_lock(&layout_lock);
	list_add_tail(&lp->lo_perstate, &ls->ls_layouts);
	list_add_tail(&lp->lo_perclnt, &clp->cl_layouts);
	list_add_tail(&lp->lo_perfile, &fp->fi_layouts);
	spin_unlock(&layout_lock);
	dprintk("pNFS %s end\n", __func__);
}

static void
dequeue_layout(struct nfs4_layout *lp)
{
	list_del(&lp->lo_perclnt);
	list_del(&lp->lo_perfile);
	list_del(&lp->lo_perstate);
}

static void
destroy_layout(struct nfs4_layout *lp)
{
	struct nfs4_client *clp;
	struct nfs4_file *fp;
	struct nfs4_layout_state *ls;

	dequeue_layout(lp);
	clp = lp->lo_client;
	fp = lp->lo_file;
	ls = lp->lo_state;
	dprintk("pNFS %s: lp %p clp %p fp %p ino %p ls_layouts empty %d\n",
		__func__, lp, clp, fp, fp->fi_inode,
		list_empty(&ls->ls_layouts));

	kmem_cache_free(pnfs_layout_slab, lp);
	/* release references taken by init_layout */
	put_layout_state_locked(ls);
	put_nfs4_file(fp);
}
/*
 * get_state() and cb_get_state() are
 */
void
release_pnfs_ds_dev_list(struct nfs4_stateid *stp)
{
	struct pnfs_ds_dev_entry *ddp;

	while (!list_empty(&stp->st_pnfs_ds_id)) {
		ddp = list_entry(stp->st_pnfs_ds_id.next,
				 struct pnfs_ds_dev_entry, dd_dev_entry);
		list_del(&ddp->dd_dev_entry);
		kfree(ddp);
	}
}

static int
nfs4_add_pnfs_ds_dev(struct nfs4_stateid *stp, u32 dsid)
{
	struct pnfs_ds_dev_entry *ddp;

	ddp = kmalloc(sizeof(*ddp), GFP_KERNEL);
	if (!ddp)
		return -ENOMEM;

	INIT_LIST_HEAD(&ddp->dd_dev_entry);
	list_add(&ddp->dd_dev_entry, &stp->st_pnfs_ds_id);
	ddp->dd_dsid = dsid;
	return 0;
}

/*
 * are two octet ranges overlapping?
 * start1            last1
 *   |-----------------|
 *                start2            last2
 *                  |----------------|
 */
static inline int
lo_seg_overlapping(struct nfsd4_layout_seg *l1, struct nfsd4_layout_seg *l2)
{
	u64 start1 = l1->offset;
	u64 last1 = last_byte_offset(start1, l1->length);
	u64 start2 = l2->offset;
	u64 last2 = last_byte_offset(start2, l2->length);
	int ret;

	/* if last1 == start2 there's a single byte overlap */
	ret = (last2 >= start1) && (last1 >= start2);
	dprintk("%s: l1 %llu:%lld l2 %llu:%lld ret=%d\n", __func__,
		l1->offset, l1->length, l2->offset, l2->length, ret);
	return ret;
}

static inline int
same_fsid_major(struct nfs4_fsid *fsid, u64 major)
{
	return fsid->major == major;
}

static inline int
same_fsid(struct nfs4_fsid *fsid, struct svc_fh *current_fh)
{
	return same_fsid_major(fsid, current_fh->fh_export->ex_fsid);
}

/*
 * are two octet ranges overlapping or adjacent?
 */
static inline int
lo_seg_mergeable(struct nfsd4_layout_seg *l1, struct nfsd4_layout_seg *l2)
{
	u64 start1 = l1->offset;
	u64 end1 = end_offset(start1, l1->length);
	u64 start2 = l2->offset;
	u64 end2 = end_offset(start2, l2->length);

	/* is end1 == start2 ranges are adjacent */
	return (end2 >= start1) && (end1 >= start2);
}

static void
extend_layout(struct nfsd4_layout_seg *lo, struct nfsd4_layout_seg *lg)
{
	u64 lo_start = lo->offset;
	u64 lo_end = end_offset(lo_start, lo->length);
	u64 lg_start = lg->offset;
	u64 lg_end = end_offset(lg_start, lg->length);

	/* lo already covers lg? */
	if (lo_start <= lg_start && lg_end <= lo_end)
		return;

	/* extend start offset */
	if (lo_start > lg_start)
		lo_start = lg_start;

	/* extend end offset */
	if (lo_end < lg_end)
		lo_end = lg_end;

	lo->offset = lo_start;
	lo->length = (lo_end == NFS4_MAX_UINT64) ?
		      lo_end : lo_end - lo_start;
}

static struct nfs4_layout *
merge_layout(struct nfs4_file *fp,
	     struct nfs4_client *clp,
	     struct nfsd4_layout_seg *seg)
{
	struct nfs4_layout *lp = NULL;

	spin_lock(&layout_lock);
	list_for_each_entry (lp, &fp->fi_layouts, lo_perfile)
		if (lp->lo_seg.layout_type == seg->layout_type &&
		    lp->lo_seg.clientid == seg->clientid &&
		    lp->lo_seg.iomode == seg->iomode &&
		    lo_seg_mergeable(&lp->lo_seg, seg)) {
			extend_layout(&lp->lo_seg, seg);
			break;
		}
	spin_unlock(&layout_lock);

	return lp;
}

int
nfs4_pnfs_get_layout(struct svc_fh *current_fh,
		     struct pnfs_layoutget_arg *args,
		     stateid_t *stateid)
{
	int status = nfserr_layouttrylater;
	struct inode *ino = current_fh->fh_dentry->d_inode;
	struct nfs4_file *fp;
	struct nfs4_client *clp;
	struct nfs4_layout *lp = NULL;
	struct nfs4_layout_state *ls = NULL;

	dprintk("NFSD: %s Begin\n", __func__);

	nfs4_lock_state();
	fp = find_alloc_file(ino, current_fh);
	clp = find_confirmed_client((clientid_t *)&args->seg.clientid);
	dprintk("pNFS %s: fp %p clp %p \n", __func__, fp, clp);
	if (!fp || !clp)
		goto out;

	/* Check decoded layout stateid */
	status = nfs4_process_layout_stateid(clp, fp, stateid, &ls);
	if (status)
		goto out;

	/* pre-alloc layout in case we can't merge after we call
	 * the file system
	 */
	lp = alloc_layout();
	if (!lp)
		goto out;

	dprintk("pNFS %s: pre-export type 0x%x maxcount %d "
		"iomode %u offset %llu length %llu\n",
		__func__, args->seg.layout_type, args->xdr.maxcount,
		args->seg.iomode, args->seg.offset, args->seg.length);

	status = nfsd4_pnfs_fl_layoutget(ino, args);

	dprintk("pNFS %s: post-export status %d "
		"iomode %u offset %llu length %llu\n",
		__func__, status, args->seg.iomode,
		args->seg.offset, args->seg.length);

	if (status) {
		switch (status) {
		case -ENOMEM:
		case -EAGAIN:
		case -EINTR:
			status = nfserr_layouttrylater;
			break;
		case -ENOENT:
			status = nfserr_badlayout;
			break;
		case -E2BIG:
			status = nfserr_toosmall;
			break;
		default:
			status = nfserr_layoutunavailable;
		}
		goto out_freelayout;
	}

	init_layout(ls, lp, fp, clp, current_fh, &args->seg);
out:
	if (ls)
		put_layout_state(ls);
	if (fp)
		put_nfs4_file(fp);
	nfs4_unlock_state();
	dprintk("pNFS %s: lp %p exit status %d\n", __func__, lp, status);
	return status;
out_freelayout:
	free_layout(lp);
	goto out;
}

static void
trim_layout(struct nfsd4_layout_seg *lo, struct nfsd4_layout_seg *lr)
{
	u64 lo_start = lo->offset;
	u64 lo_end = end_offset(lo_start, lo->length);
	u64 lr_start = lr->offset;
	u64 lr_end = end_offset(lr_start, lr->length);

	dprintk("%s:Begin lo %llu:%lld lr %llu:%lld\n", __func__,
		lo->offset, lo->length, lr->offset, lr->length);

	/* lr fully covers lo? */
	if (lr_start <= lo_start && lo_end <= lr_end) {
		lo->length = 0;
		goto out;
	}

	/*
	 * split not supported yet. retain layout segment.
	 * remains must be returned by the client
	 * on the final layout return.
	 */
	if (lo_start < lr_start && lr_end < lo_end) {
		dprintk("%s: split not supported\n", __func__);
		goto out;
	}

	if (lo_start < lr_start)
		lo_end = lr_start - 1;
	else /* lr_end < lo_end */
		lo_start = lr_end + 1;

	lo->offset = lo_start;
	lo->length = (lo_end == NFS4_MAX_UINT64) ? lo_end : lo_end - lo_start;
out:
	dprintk("%s:End lo %llu:%lld\n", __func__, lo->offset, lo->length);
}

static int
pnfs_return_file_layouts(struct nfs4_client *clp, struct nfs4_file *fp,
			 struct nfsd4_pnfs_layoutreturn *lrp)
{
	int layouts_found = 0;
	struct nfs4_layout *lp, *nextlp;

	dprintk("%s: clp %p fp %p\n", __func__, clp, fp);
	spin_lock(&layout_lock);
	list_for_each_entry_safe (lp, nextlp, &fp->fi_layouts, lo_perfile) {
		dprintk("%s: lp %p client %p,%p lo_type %x,%x iomode %d,%d\n",
			__func__, lp,
			lp->lo_client, clp,
			lp->lo_seg.layout_type, lrp->lr_seg.layout_type,
			lp->lo_seg.iomode, lrp->lr_seg.iomode);
		if (lp->lo_client != clp ||
		    lp->lo_seg.layout_type != lrp->lr_seg.layout_type ||
		    (lp->lo_seg.iomode != lrp->lr_seg.iomode &&
		     lrp->lr_seg.iomode != IOMODE_ANY) ||
		     !lo_seg_overlapping(&lp->lo_seg, &lrp->lr_seg))
			continue;
		layouts_found++;
		trim_layout(&lp->lo_seg, &lrp->lr_seg);
		if (!lp->lo_seg.length) {
			lrp->lrs_present = 0;
			destroy_layout(lp);
		}
	}
	spin_unlock(&layout_lock);

	return layouts_found;
}

static int
pnfs_return_client_layouts(struct nfs4_client *clp,
			   struct nfsd4_pnfs_layoutreturn *lrp, u64 ex_fsid)
{
	int layouts_found = 0;
	struct nfs4_layout *lp, *nextlp;

	spin_lock(&layout_lock);
	list_for_each_entry_safe (lp, nextlp, &clp->cl_layouts, lo_perclnt) {
		if (lrp->lr_seg.layout_type != lp->lo_seg.layout_type ||
		   (lrp->lr_seg.iomode != lp->lo_seg.iomode &&
		    lrp->lr_seg.iomode != IOMODE_ANY))
			continue;

		if (lrp->lr_return_type == RETURN_FSID &&
		    !same_fsid_major(&lp->lo_file->fi_fsid, ex_fsid))
			continue;

		layouts_found++;
		destroy_layout(lp);
	}
	spin_unlock(&layout_lock);

	return layouts_found;
}

int nfs4_pnfs_return_layout(struct svc_fh *current_fh,
			    struct nfsd4_pnfs_layoutreturn *lrp)
{
	int status = 0;
	int layouts_found = 0;
	struct inode *ino = current_fh->fh_dentry->d_inode;
	struct nfs4_file *fp = NULL;
	struct nfs4_client *clp;
	u64 ex_fsid = current_fh->fh_export->ex_fsid;

	dprintk("NFSD: %s\n", __func__);

	nfs4_lock_state();
	clp = find_confirmed_client((clientid_t *)&lrp->lr_seg.clientid);
	if (!clp)
		goto out;

	fp = find_file(ino);
	if (lrp->lr_return_type == RETURN_FILE) {
		if (!fp) {
			printk(KERN_ERR "%s: RETURN_FILE: no nfs4_file for "
				"ino %p:%lu\n",
				__func__, ino, ino ? ino->i_ino : 0L);
			goto out;
		}

		/* Check the stateid */
		dprintk("%s PROCESS LO_STATEID inode %p\n", __func__, ino);
		status = nfs4_process_layout_stateid(clp, fp, &lrp->lr_sid,
						     NULL);
		if (status)
			goto out_put_file;

		/* update layouts */
		layouts_found = pnfs_return_file_layouts(clp, fp, lrp);
	} else {
		layouts_found = pnfs_return_client_layouts(clp, lrp, ex_fsid);
	}

	dprintk("pNFS %s: clp %p fp %p layout_type 0x%x iomode %d "
		"return_type %d fsid 0x%llx offset %llu length %llu: "
		"layouts_found %d\n",
		__func__, clp, fp, lrp->lr_seg.layout_type,
		lrp->lr_seg.iomode, lrp->lr_return_type,
		ex_fsid,
		lrp->lr_seg.offset, lrp->lr_seg.length, layouts_found);

out_put_file:
	if (fp)
		put_nfs4_file(fp);
out:
	nfs4_unlock_state();

	dprintk("pNFS %s: exit status %d \n", __func__, status);
	return status;
}

/*
 * PNFS Metadata server export operations callback for get_state
 *
 * called by the cluster fs when it receives a get_state() from a data
 * server.
 * returns status, or pnfs_get_state* with pnfs_get_state->status set.
 *
 */
int
nfs4_pnfs_cb_get_state(struct super_block *sb, struct pnfs_get_state *arg)
{
	struct nfs4_stateid *stp;
	int flags = LOCK_STATE | OPEN_STATE; /* search both hash tables */
	int status = -EINVAL;
	struct inode *ino;
	struct nfs4_delegation *dl;

	dprintk("NFSD: %s sid=" STATEID_FMT " ino %llu\n", __func__,
		STATEID_VAL(&arg->stid), arg->ino);

	nfs4_lock_state();
	stp = find_stateid(&arg->stid, flags);
	if (!stp) {
		ino = iget_locked(sb, arg->ino);
		if (!ino)
			goto out;

		if (ino->i_state & I_NEW) {
			iget_failed(ino);
			goto out;
		}

		dl = find_delegation_stateid(ino, &arg->stid);
		if (dl)
			status = 0;

		iput(ino);
	} else {
		/* XXX ANDROS: marc removed nfs4_check_fh - how come? */

		/* arg->devid is the Data server id, set by the cluster fs */
		status = nfs4_add_pnfs_ds_dev(stp, arg->dsid);
		if (status)
			goto out;

		arg->access = stp->st_access_bmap;
		arg->clid = stp->st_stateowner->so_client->cl_clientid;
	}
out:
	nfs4_unlock_state();
	return status;
}

void pnfs_expire_client(struct nfs4_client *clp)
{
	struct nfs4_layout *lp;

	spin_lock(&layout_lock);
	while (!list_empty(&clp->cl_layouts)) {
		lp = list_entry(clp->cl_layouts.next, struct nfs4_layout,
				lo_perclnt);
		dprintk("NFSD: expire client. lp %p, fp %p\n", lp,
			lp->lo_file);
		BUG_ON(lp->lo_client != clp);
		destroy_layout(lp);
	}
	spin_unlock(&layout_lock);
}
#endif /* CONFIG_PNFSD */
