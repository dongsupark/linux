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
 *****************************************************************************/

#include "pnfsd.h"
#include "netns.h"

#define NFSDDBG_FACILITY                NFSDDBG_PNFS

static DEFINE_SPINLOCK(layout_lock);

/*
 * Layout state - NFSv4.1 pNFS
 */
static struct kmem_cache *pnfs_layout_slab;
static struct kmem_cache *layout_state_slab;
static struct kmem_cache *pnfs_layoutrecall_slab;

/* hash table for nfsd4_pnfs_deviceid.sbid */
#define SBID_HASH_BITS	8
#define SBID_HASH_SIZE	(1 << SBID_HASH_BITS)
#define SBID_HASH_MASK	(SBID_HASH_SIZE - 1)

struct sbid_tracker {
	u64 id;
	struct super_block *sb;
	struct list_head hash;
};

static u64 current_sbid;
static struct list_head sbid_hashtbl[SBID_HASH_SIZE];

static unsigned long
sbid_hashval(struct super_block *sb)
{
	return hash_ptr(sb, SBID_HASH_BITS);
}

static struct sbid_tracker *
alloc_sbid(void)
{
	return kmalloc(sizeof(struct sbid_tracker), GFP_KERNEL);
}

static void
destroy_sbid(struct sbid_tracker *sbid)
{
	spin_lock(&layout_lock);
	list_del(&sbid->hash);
	spin_unlock(&layout_lock);
	kfree(sbid);
}

void
nfsd4_free_pnfs_slabs(void)
{
	int i;
	struct sbid_tracker *sbid;

	nfsd4_free_slab(&pnfs_layout_slab);
	nfsd4_free_slab(&layout_state_slab);
	nfsd4_free_slab(&pnfs_layoutrecall_slab);

	for (i = 0; i < SBID_HASH_SIZE; i++) {
		while (!list_empty(&sbid_hashtbl[i])) {
			sbid = list_first_entry(&sbid_hashtbl[i],
						struct sbid_tracker,
						hash);
			destroy_sbid(sbid);
		}
	}
}

int
nfsd4_init_pnfs_slabs(void)
{
	int i;

	pnfs_layout_slab = kmem_cache_create("pnfs_layouts",
			sizeof(struct nfs4_layout), 0, 0, NULL);
	if (pnfs_layout_slab == NULL)
		return -ENOMEM;
	pnfs_layoutrecall_slab = kmem_cache_create("pnfs_layoutrecalls",
			sizeof(struct nfs4_layoutrecall), 0, 0, NULL);
	if (pnfs_layoutrecall_slab == NULL)
		return -ENOMEM;

	layout_state_slab = kmem_cache_create("pnfs_layout_states",
			sizeof(struct nfs4_layout_state), 0, 0, NULL);
	if (layout_state_slab == NULL)
		return -ENOMEM;

	for (i = 0; i < SBID_HASH_SIZE; i++)
		INIT_LIST_HEAD(&sbid_hashtbl[i]);

	return 0;
}

/*
 * Note: must be called under the state lock
 */
static struct nfs4_layout_state *
alloc_init_layout_state(struct nfs4_client *clp, struct nfs4_file *fp,
			stateid_t *stateid)
{
	struct nfs4_layout_state *new;

	new = layoutstateid(nfs4_alloc_stid(clp, layout_state_slab));
	if (!new)
		return new;
	kref_init(&new->ls_ref);
	nfsd4_init_stid(&new->ls_stid, clp, NFS4_LAYOUT_STID);
	INIT_LIST_HEAD(&new->ls_perfile);
	spin_lock(&layout_lock);
	list_add(&new->ls_perfile, &fp->fi_layout_states);
	spin_unlock(&layout_lock);
	new->ls_roc = false;
	return new;
}

static void
get_layout_state(struct nfs4_layout_state *ls)
{
	kref_get(&ls->ls_ref);
}

static void
destroy_layout_state(struct kref *kref)
{
	struct nfs4_layout_state *ls =
			container_of(kref, struct nfs4_layout_state, ls_ref);

	nfsd4_unhash_stid(&ls->ls_stid);
	if (!list_empty(&ls->ls_perfile)) {
		spin_lock(&layout_lock);
		list_del(&ls->ls_perfile);
		spin_unlock(&layout_lock);
	}
	kfree(ls);
}

/*
 * Note: caller must not hold layout_lock
 */
static void
put_layout_state(struct nfs4_layout_state *ls)
{
	dprintk("pNFS %s: ls %p ls_ref %d\n", __func__, ls,
		atomic_read(&ls->ls_ref.refcount));
	kref_put(&ls->ls_ref, destroy_layout_state);
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
			    stateid_t *stateid, struct nfs4_layout_state **lsp,
			    bool do_alloc)
{
	struct nfs4_layout_state *ls = NULL;
	__be32 status = 0;
	struct nfs4_stid *stid;

	dprintk("--> %s clp %p fp %p operation stateid=" STATEID_FMT "\n",
		__func__, clp, fp, STATEID_VAL(stateid));

	status = nfsd4_lookup_stateid(stateid,
				      (NFS4_OPEN_STID | NFS4_LOCK_STID |
				       NFS4_DELEG_STID | NFS4_LAYOUT_STID),
				      &stid, true,
				      net_generic(clp->net, nfsd_net_id));
	if (status)
		goto out;

	/* Is this the first use of this layout ? */
	if (stid->sc_type != NFS4_LAYOUT_STID) {
		/* Only alloc layout state on layoutget. */
		if (!do_alloc) {
			dprintk("%s: ERROR: Not layoutget but no layout stateid\n", __func__);
			status = nfserr_bad_stateid;
			goto out;
		}

		ls = alloc_init_layout_state(clp, fp, stateid);
		if (!ls) {
			status = nfserr_jukebox;
			goto out;
		}
	} else {
		ls = container_of(stid, struct nfs4_layout_state, ls_stid);

		/* BAD STATEID */
		if (stateid->si_generation > ls->ls_stid.sc_stateid.si_generation) {
			dprintk("%s bad stateid 1\n", __func__);
			status = nfserr_bad_stateid;
			goto out;
		}
		get_layout_state(ls);
	}
	status = 0;

	*lsp = ls;
	dprintk("%s: layout stateid=" STATEID_FMT " ref=%d\n", __func__,
		STATEID_VAL(&ls->ls_stid.sc_stateid), atomic_read(&ls->ls_ref.refcount));
out:
	dprintk("<-- %s status %d\n", __func__, htonl(status));

	return status;
}

static struct nfs4_layout *
alloc_layout(void)
{
	return kmem_cache_alloc(pnfs_layout_slab, GFP_KERNEL);
}

static void
free_layout(struct nfs4_layout *lp)
{
	kmem_cache_free(pnfs_layout_slab, lp);
}

static void update_layout_stateid_locked(struct nfs4_layout_state *ls, stateid_t *sid)
{
	update_stateid(&(ls)->ls_stid.sc_stateid);
	memcpy((sid), &(ls)->ls_stid.sc_stateid, sizeof(stateid_t));
	dprintk("%s Updated ls_stid to %d on layoutstate %p\n",
		__func__, sid->si_generation, ls);
}

static void update_layout_stateid(struct nfs4_layout_state *ls, stateid_t *sid)
{
	spin_lock(&layout_lock);
	update_layout_stateid_locked(ls, sid);
	spin_unlock(&layout_lock);
}

static void update_layout_roc(struct nfs4_layout_state *ls, bool roc)
{
	if (roc) {
		ls->ls_roc = true;
		dprintk("%s: Marked return_on_close on layoutstate %p\n",
			__func__, ls);
	}
}

static void
init_layout(struct nfs4_layout *lp,
	    struct nfs4_layout_state *ls,
	    struct nfs4_file *fp,
	    struct nfs4_client *clp,
	    struct svc_fh *current_fh,
	    struct nfsd4_layout_seg *seg,
	    stateid_t *stateid)
{
	dprintk("pNFS %s: lp %p ls %p clp %p fp %p ino %p\n", __func__,
		lp, ls, clp, fp, fp->fi_inode);

	get_nfs4_file(fp);
	lp->lo_client = clp;
	lp->lo_file = fp;
	memcpy(&lp->lo_seg, seg, sizeof(lp->lo_seg));
	get_layout_state(ls);		/* put on destroy_layout */
	lp->lo_state = ls;
	update_layout_stateid(ls, stateid);
	list_add_tail(&lp->lo_perclnt, &clp->cl_layouts);
	list_add_tail(&lp->lo_perfile, &fp->fi_layouts);
	dprintk("pNFS %s end\n", __func__);
}

/*
 * Note: always called under the layout_lock
 */
static void
dequeue_layout(struct nfs4_layout *lp)
{
	list_del(&lp->lo_perclnt);
	list_del(&lp->lo_perfile);
}

/*
 * Note: caller must not hold layout_lock
 */
static void
destroy_layout(struct nfs4_layout *lp)
{
	struct nfs4_client *clp;
	struct nfs4_file *fp;
	struct nfs4_layout_state *ls;

	clp = lp->lo_client;
	fp = lp->lo_file;
	ls = lp->lo_state;
	dprintk("pNFS %s: lp %p clp %p fp %p ino %p\n",
		__func__, lp, clp, fp, fp->fi_inode);

	kmem_cache_free(pnfs_layout_slab, lp);
	/* release references taken by init_layout */
	put_layout_state(ls);
	put_nfs4_file(fp);
}

static void fs_layout_return(struct inode *ino,
			     struct nfsd4_pnfs_layoutreturn *lrp,
			     int flags, void *recall_cookie)
{
	int ret;
	struct super_block *sb = ino->i_sb;

	if (unlikely(!sb->s_pnfs_op->layout_return))
		return;

	lrp->args.lr_flags = flags;
	lrp->args.lr_cookie = recall_cookie;

	ret = sb->s_pnfs_op->layout_return(ino, &lrp->args);
	dprintk("%s: inode %lu iomode=%d offset=0x%llx length=0x%llx "
		"cookie = %p flags 0x%x status=%d\n",
		__func__, ino->i_ino, lrp->args.lr_seg.iomode,
		lrp->args.lr_seg.offset, lrp->args.lr_seg.length,
		recall_cookie, flags, ret);
}

static u64
alloc_init_sbid(struct super_block *sb)
{
	struct sbid_tracker *sbid;
	struct sbid_tracker *new = alloc_sbid();
	unsigned long hash_idx = sbid_hashval(sb);
	u64 id = 0;

	if (likely(new)) {
		spin_lock(&layout_lock);
		id = ++current_sbid;
		new->id = (id << SBID_HASH_BITS) | (hash_idx & SBID_HASH_MASK);
		id = new->id;
		BUG_ON(id == 0);
		new->sb = sb;

		list_for_each_entry (sbid, &sbid_hashtbl[hash_idx], hash)
			if (sbid->sb == sb) {
				kfree(new);
				id = sbid->id;
				spin_unlock(&layout_lock);
				return id;
			}
		list_add(&new->hash, &sbid_hashtbl[hash_idx]);
		spin_unlock(&layout_lock);
	}
	return id;
}

struct super_block *
find_sbid_id(u64 id)
{
	struct sbid_tracker *sbid;
	struct super_block *sb = NULL;
	unsigned long hash_idx = id & SBID_HASH_MASK;
	int pos = 0;

	spin_lock(&layout_lock);
	list_for_each_entry (sbid, &sbid_hashtbl[hash_idx], hash) {
		pos++;
		if (sbid->id != id)
			continue;
		if (pos > 1)
			list_move(&sbid->hash, &sbid_hashtbl[hash_idx]);
		sb = sbid->sb;
		break;
	}
	spin_unlock(&layout_lock);
	return sb;
}

u64
find_create_sbid(struct super_block *sb)
{
	struct sbid_tracker *sbid;
	unsigned long hash_idx = sbid_hashval(sb);
	int pos = 0;
	u64 id = 0;

	spin_lock(&layout_lock);
	list_for_each_entry (sbid, &sbid_hashtbl[hash_idx], hash) {
		pos++;
		if (sbid->sb != sb)
			continue;
		if (pos > 1)
			list_move(&sbid->hash, &sbid_hashtbl[hash_idx]);
		id = sbid->id;
		break;
	}
	spin_unlock(&layout_lock);

	if (!id)
		id = alloc_init_sbid(sb);

	return id;
}

/*
 * Create a layoutrecall structure
 * An optional layoutrecall can be cloned (except for the layoutrecall lists)
 */
static struct nfs4_layoutrecall *
alloc_init_layoutrecall(struct nfsd4_pnfs_cb_layout *cbl,
			struct nfs4_client *clp,
			struct nfs4_file *lrfile)
{
	struct nfs4_layoutrecall *clr;

	dprintk("NFSD %s\n", __func__);
	clr = kmem_cache_alloc(pnfs_layoutrecall_slab, GFP_KERNEL);
	if (clr == NULL)
		return clr;

	dprintk("NFSD %s -->\n", __func__);

	memset(clr, 0, sizeof(*clr));
	if (lrfile)
		get_nfs4_file(lrfile);
	clr->clr_client = clp;
	clr->clr_file = lrfile;
	clr->cb = *cbl;

	kref_init(&clr->clr_ref);
	INIT_LIST_HEAD(&clr->clr_perclnt);
	nfsd4_init_callback(&clr->clr_recall);

	dprintk("NFSD %s return %p\n", __func__, clr);
	return clr;
}

static void
get_layoutrecall(struct nfs4_layoutrecall *clr)
{
	dprintk("pNFS %s: clr %p clr_ref %d\n", __func__, clr,
		atomic_read(&clr->clr_ref.refcount));
	kref_get(&clr->clr_ref);
}

static void
destroy_layoutrecall(struct kref *kref)
{
	struct nfs4_layoutrecall *clr =
			container_of(kref, struct nfs4_layoutrecall, clr_ref);
	dprintk("pNFS %s: clr %p fp %p clp %p\n", __func__, clr,
		clr->clr_file, clr->clr_client);
	BUG_ON(!list_empty(&clr->clr_perclnt));
	if (clr->clr_file)
		put_nfs4_file(clr->clr_file);
	kmem_cache_free(pnfs_layoutrecall_slab, clr);
}

int
put_layoutrecall(struct nfs4_layoutrecall *clr)
{
	dprintk("pNFS %s: clr %p clr_ref %d\n", __func__, clr,
		atomic_read(&clr->clr_ref.refcount));
	return kref_put(&clr->clr_ref, destroy_layoutrecall);
}

void *
layoutrecall_done(struct nfs4_layoutrecall *clr)
{
	void *recall_cookie = clr->cb.cbl_cookie;
	struct nfs4_layoutrecall *parent = clr->parent;

	dprintk("pNFS %s: clr %p clr_ref %d\n", __func__, clr,
		atomic_read(&clr->clr_ref.refcount));
	list_del_init(&clr->clr_perclnt);
	put_layoutrecall(clr);

	if (parent && !put_layoutrecall(parent))
		recall_cookie = NULL;

	return recall_cookie;
}

/*
 * are two octet ranges overlapping?
 * start1            last1
 *   |-----------------|
 *                start2            last2
 *                  |----------------|
 */
static int
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

static int
same_fsid_major(struct nfs4_fsid *fsid, u64 major)
{
	return fsid->major == major;
}

static int
same_fsid(struct nfs4_fsid *fsid, struct svc_fh *current_fh)
{
	return same_fsid_major(fsid, current_fh->fh_export->ex_fsid);
}

/*
 * find a layout recall conflicting with the specified layoutget
 */
static int
is_layout_recalled(struct nfs4_client *clp,
		   struct svc_fh *current_fh,
		   struct nfsd4_layout_seg *seg)
{
	struct nfs4_layoutrecall *clr;

	spin_lock(&layout_lock);
	list_for_each_entry (clr, &clp->cl_layoutrecalls, clr_perclnt) {
		if (clr->cb.cbl_seg.layout_type != seg->layout_type)
			continue;
		if (clr->cb.cbl_recall_type == RETURN_ALL)
			goto found;
		if (clr->cb.cbl_recall_type == RETURN_FSID) {
			if (same_fsid(&clr->cb.cbl_fsid, current_fh))
				goto found;
			else
				continue;
		}
		BUG_ON(clr->cb.cbl_recall_type != RETURN_FILE);
		if (clr->cb.cbl_seg.clientid == seg->clientid &&
		    lo_seg_overlapping(&clr->cb.cbl_seg, seg))
			goto found;
	}
	spin_unlock(&layout_lock);
	return 0;
found:
	spin_unlock(&layout_lock);
	return 1;
}

/*
 * are two octet ranges overlapping or adjacent?
 */
static int
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

	list_for_each_entry (lp, &fp->fi_layouts, lo_perfile)
		if (lp->lo_seg.layout_type == seg->layout_type &&
		    lp->lo_seg.clientid == seg->clientid &&
		    lp->lo_seg.iomode == seg->iomode &&
		    lo_seg_mergeable(&lp->lo_seg, seg)) {
			extend_layout(&lp->lo_seg, seg);
			break;
		}

	return lp;
}

__be32
nfs4_pnfs_get_layout(struct svc_rqst *rqstp,
		     struct nfsd4_pnfs_layoutget *lgp,
		     struct exp_xdr_stream *xdr)
{
	u32 status;
	__be32 nfserr;
	struct inode *ino = lgp->lg_fhp->fh_dentry->d_inode;
	struct super_block *sb = ino->i_sb;
	int can_merge;
	struct nfs4_file *fp;
	struct nfs4_client *clp;
	struct nfs4_layout *lp = NULL;
	struct nfs4_layout_state *ls = NULL;
	struct nfsd4_pnfs_layoutget_arg args = {
		.lg_minlength = lgp->lg_minlength,
		.lg_fh = &lgp->lg_fhp->fh_handle,
	};
	struct nfsd4_pnfs_layoutget_res res = {
		.lg_seg = lgp->lg_seg,
	};

	dprintk("NFSD: %s Begin\n", __func__);

	/* verify minlength and range as per RFC5661:
	 *  o  If loga_length is less than loga_minlength,
	 *     the metadata server MUST return NFS4ERR_INVAL.
	 *  o  If the sum of loga_offset and loga_minlength exceeds
	 *     NFS4_UINT64_MAX, and loga_minlength is not
	 *     NFS4_UINT64_MAX, the error NFS4ERR_INVAL MUST result.
	 *  o  If the sum of loga_offset and loga_length exceeds
	 *     NFS4_UINT64_MAX, and loga_length is not NFS4_UINT64_MAX,
	 *     the error NFS4ERR_INVAL MUST result.
	 */
	if ((lgp->lg_seg.length < lgp->lg_minlength) ||
	    (lgp->lg_minlength != NFS4_MAX_UINT64 &&
	     lgp->lg_minlength > NFS4_MAX_UINT64 - lgp->lg_seg.offset) ||
	    (lgp->lg_seg.length != NFS4_MAX_UINT64 &&
	     lgp->lg_seg.length > NFS4_MAX_UINT64 - lgp->lg_seg.offset)) {
		nfserr = nfserr_inval;
		goto out;
	}

	args.lg_sbid = find_create_sbid(sb);
	if (!args.lg_sbid) {
		nfserr = nfserr_layouttrylater;
		goto out;
	}

	can_merge = sb->s_pnfs_op->can_merge_layouts != NULL &&
		    sb->s_pnfs_op->can_merge_layouts(lgp->lg_seg.layout_type);

	nfs4_lock_state();
	fp = find_alloc_file(ino, lgp->lg_fhp);
	clp = find_confirmed_client((clientid_t *)&lgp->lg_seg.clientid, true,
				    net_generic(SVC_NET(rqstp), nfsd_net_id));
	dprintk("pNFS %s: fp %p clp %p\n", __func__, fp, clp);
	if (!fp || !clp) {
		nfserr = nfserr_inval;
		goto out_unlock;
	}

	/* Check decoded layout stateid */
	nfserr = nfs4_process_layout_stateid(clp, fp, &lgp->lg_sid, &ls, true);
	if (nfserr)
		goto out_unlock;

	if (is_layout_recalled(clp, lgp->lg_fhp, &lgp->lg_seg)) {
		nfserr = nfserr_recallconflict;
		goto out_unlock;
	}

	/* pre-alloc layout in case we can't merge after we call
	 * the file system
	 */
	lp = alloc_layout();
	if (!lp) {
		nfserr = nfserr_layouttrylater;
		goto out_unlock;
	}

	dprintk("pNFS %s: pre-export type 0x%x maxcount %Zd "
		"iomode %u offset %llu length %llu\n",
		__func__, lgp->lg_seg.layout_type,
		exp_xdr_qbytes(xdr->end - xdr->p),
		lgp->lg_seg.iomode, lgp->lg_seg.offset, lgp->lg_seg.length);

	/* FIXME: need to eliminate the use of the state lock */
	nfs4_unlock_state();
	status = sb->s_pnfs_op->layout_get(ino, xdr, &args, &res);
	nfs4_lock_state();

	dprintk("pNFS %s: post-export status %u "
		"iomode %u offset %llu length %llu\n",
		__func__, status, res.lg_seg.iomode,
		res.lg_seg.offset, res.lg_seg.length);

	/*
	 * The allowable error codes for the layout_get pNFS export
	 * operations vector function (from the file system) can be
	 * expanded as needed to include other errors defined for
	 * the RFC 5561 LAYOUTGET operation.
	 */
	switch (status) {
	case 0:
		nfserr = NFS4_OK;
		break;
	case NFS4ERR_ACCESS:
	case NFS4ERR_BADIOMODE:
		/* No support for LAYOUTIOMODE4_RW layouts */
	case NFS4ERR_BADLAYOUT:
		/* No layout matching loga_minlength rules */
	case NFS4ERR_INVAL:
	case NFS4ERR_IO:
	case NFS4ERR_LAYOUTTRYLATER:
	case NFS4ERR_LAYOUTUNAVAILABLE:
	case NFS4ERR_LOCKED:
	case NFS4ERR_NOSPC:
	case NFS4ERR_RECALLCONFLICT:
	case NFS4ERR_SERVERFAULT:
	case NFS4ERR_TOOSMALL:
		/* Requested layout too big for loga_maxcount */
	case NFS4ERR_WRONG_TYPE:
		/* Not a regular file */
		nfserr = cpu_to_be32(status);
		goto out_freelayout;
	default:
		BUG();
		nfserr = nfserr_serverfault;
	}

	lgp->lg_seg = res.lg_seg;
	lgp->lg_roc = res.lg_return_on_close;
	update_layout_roc(ls, res.lg_return_on_close);

	/* SUCCESS!
	 * Can the new layout be merged into an existing one?
	 * If so, free unused layout struct
	 */
	if (can_merge && merge_layout(fp, clp, &res.lg_seg))
		goto out_freelayout;

	/* Can't merge, so let's initialize this new layout */
	init_layout(lp, ls, fp, clp, lgp->lg_fhp, &res.lg_seg, &lgp->lg_sid);
out_unlock:
	if (ls)
		put_layout_state(ls);
	if (fp)
		put_nfs4_file(fp);
	nfs4_unlock_state();
out:
	dprintk("pNFS %s: lp %p exit nfserr %u\n", __func__, lp,
		be32_to_cpu(nfserr));
	return nfserr;
out_freelayout:
	free_layout(lp);
	goto out_unlock;
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

static void
pnfsd_return_lo_list(struct list_head *lo_destroy_list, struct inode *ino_orig,
		     struct nfsd4_pnfs_layoutreturn *lr_orig, int flags,
		     void *cb_cookie)
{
	struct nfs4_layout *lo, *nextlp;

	if (list_empty(lo_destroy_list) && cb_cookie) {
		/* This is a rare race case where at the time of recall there
		 * were some layouts, which got freed before the recall-done.
		 * and the recall was left without any actual layouts to free.
		 * If the FS gave us a cb_cookie it is waiting for it so we
		 * report back about it here.
		 */

		/* Any caller with a cb_cookie must pass a none null
		 * ino_orig and an lr_orig.
		 */
		struct inode *inode = igrab(ino_orig);

		/* Probably a BUG_ON but just in case. Caller of cb_recall must
		 * take care of this. Please report to ml */
		if (WARN_ON(!inode))
			return;

		fs_layout_return(inode, lr_orig, flags, cb_cookie);
		iput(inode);
		return;
	}

	list_for_each_entry_safe(lo, nextlp, lo_destroy_list, lo_perfile) {
		struct inode *inode = lo->lo_file->fi_inode;
		struct nfsd4_pnfs_layoutreturn lr;
		bool empty;
		int lr_flags = flags;

		memset(&lr, 0, sizeof(lr));
		lr.args.lr_return_type = RETURN_FILE;
		lr.args.lr_seg = lo->lo_seg;

		spin_lock(&layout_lock);
		if (list_empty(&lo->lo_file->fi_layouts))
			lr_flags |= LR_FLAG_EMPTY;
		spin_unlock(&layout_lock);

		list_del(&lo->lo_perfile);
		empty = list_empty(lo_destroy_list);

		fs_layout_return(inode, &lr, lr_flags, empty ? cb_cookie : NULL);

		destroy_layout(lo); /* this will put the lo_file */
	}
}

static int
pnfs_return_file_layouts(struct nfs4_client *clp, struct nfs4_file *fp,
			 struct nfsd4_pnfs_layoutreturn *lrp,
			 struct nfs4_layout_state *ls,
			 struct list_head *lo_destroy_list)
{
	int layouts_found = 0;
	struct nfs4_layout *lp, *nextlp;

	dprintk("%s: clp %p fp %p\n", __func__, clp, fp);
	lrp->lrs_present = 0;
	spin_lock(&layout_lock);
	list_for_each_entry_safe (lp, nextlp, &fp->fi_layouts, lo_perfile) {
		dprintk("%s: lp %p client %p,%p lo_type %x,%x iomode %d,%d\n",
			__func__, lp,
			lp->lo_client, clp,
			lp->lo_seg.layout_type, lrp->args.lr_seg.layout_type,
			lp->lo_seg.iomode, lrp->args.lr_seg.iomode);
		if (lp->lo_client != clp)
			continue;
		if (lp->lo_seg.layout_type != lrp->args.lr_seg.layout_type ||
		    (lp->lo_seg.iomode != lrp->args.lr_seg.iomode &&
		     lrp->args.lr_seg.iomode != IOMODE_ANY) ||
		     !lo_seg_overlapping(&lp->lo_seg, &lrp->args.lr_seg)) {
			lrp->lrs_present = 1;
			continue;
		}
		layouts_found++;
		trim_layout(&lp->lo_seg, &lrp->args.lr_seg);
		if (!lp->lo_seg.length) {
			dequeue_layout(lp);
			list_add_tail(&lp->lo_perfile, lo_destroy_list);
		} else
			lrp->lrs_present = 1;
	}
	if (ls && layouts_found && lrp->lrs_present)
		update_layout_stateid_locked(ls, &lrp->lr_sid);
	spin_unlock(&layout_lock);

	return layouts_found;
}

static int
pnfs_return_client_layouts(struct nfs4_client *clp,
			   struct nfsd4_pnfs_layoutreturn *lrp, u64 ex_fsid,
			   struct list_head *lo_destroy_list)
{
	int layouts_found = 0;
	struct nfs4_layout *lp, *nextlp;

	spin_lock(&layout_lock);
	list_for_each_entry_safe (lp, nextlp, &clp->cl_layouts, lo_perclnt) {
		if (lrp->args.lr_seg.layout_type != lp->lo_seg.layout_type ||
		   (lrp->args.lr_seg.iomode != lp->lo_seg.iomode &&
		    lrp->args.lr_seg.iomode != IOMODE_ANY))
			continue;

		if (lrp->args.lr_return_type == RETURN_FSID &&
		    !same_fsid_major(&lp->lo_file->fi_fsid, ex_fsid))
			continue;

		layouts_found++;
		dequeue_layout(lp);
		list_add_tail(&lp->lo_perfile, lo_destroy_list);
	}
	spin_unlock(&layout_lock);

	return layouts_found;
}

static int
recall_return_perfect_match(struct nfs4_layoutrecall *clr,
			    struct nfsd4_pnfs_layoutreturn *lrp,
			    struct nfs4_file *fp,
			    struct svc_fh *current_fh)
{
	if (clr->cb.cbl_seg.iomode != lrp->args.lr_seg.iomode ||
	    clr->cb.cbl_recall_type != lrp->args.lr_return_type)
		return 0;

	return (clr->cb.cbl_recall_type == RETURN_FILE &&
		clr->clr_file == fp &&
		clr->cb.cbl_seg.offset == lrp->args.lr_seg.offset &&
		clr->cb.cbl_seg.length == lrp->args.lr_seg.length) ||

		(clr->cb.cbl_recall_type == RETURN_FSID &&
		 same_fsid(&clr->cb.cbl_fsid, current_fh)) ||

		clr->cb.cbl_recall_type == RETURN_ALL;
}

static int
recall_return_partial_match(struct nfs4_layoutrecall *clr,
			    struct nfsd4_pnfs_layoutreturn *lrp,
			    struct nfs4_file *fp,
			    struct svc_fh *current_fh)
{
	/* iomode matching? */
	if (clr->cb.cbl_seg.iomode != lrp->args.lr_seg.iomode &&
	    clr->cb.cbl_seg.iomode != IOMODE_ANY &&
	    lrp->args.lr_seg.iomode != IOMODE_ANY)
		return 0;

	if (clr->cb.cbl_recall_type == RETURN_ALL ||
	    lrp->args.lr_return_type == RETURN_ALL)
		return 1;

	/* fsid matches? */
	if (clr->cb.cbl_recall_type == RETURN_FSID ||
	    lrp->args.lr_return_type == RETURN_FSID)
		return same_fsid(&clr->cb.cbl_fsid, current_fh);

	/* file matches, range overlapping? */
	return clr->clr_file == fp &&
	       lo_seg_overlapping(&clr->cb.cbl_seg, &lrp->args.lr_seg);
}

int nfs4_pnfs_return_layout(struct svc_rqst *rqstp,
			    struct super_block *sb,
			    struct svc_fh *current_fh,
			    struct nfsd4_pnfs_layoutreturn *lrp)
{
	int status = 0;
	int layouts_found = 0;
	struct inode *ino = current_fh->fh_dentry->d_inode;
	struct nfs4_file *fp = NULL;
	struct nfs4_client *clp;
	struct nfs4_layoutrecall *clr, *nextclr;
	u64 ex_fsid = current_fh->fh_export->ex_fsid;
	void *recall_cookie = NULL;
	LIST_HEAD(lo_destroy_list);
	int lr_flags = 0;

	dprintk("NFSD: %s\n", __func__);

	nfs4_lock_state();
	clp = find_confirmed_client((clientid_t *)&lrp->args.lr_seg.clientid,
				    true, net_generic(SVC_NET(rqstp), nfsd_net_id));
	if (!clp)
		goto out;

	if (lrp->args.lr_return_type == RETURN_FILE) {
		struct nfs4_layout_state *ls = NULL;

		fp = find_file(ino);
		if (!fp) {
			nfs4_unlock_state();
			dprintk("%s: RETURN_FILE: no nfs4_file for ino %p:%lu\n",
				__func__, ino, ino ? ino->i_ino : 0L);
			/* If we had a layout on the file the nfs4_file would
			 * be referenced and we should have found it. Since we
			 * don't then it means all layouts were ROC and at this
			 * point we returned all of them on file close.
			 */
			goto out_no_fs_call;
		}

		/* Check the stateid */
		dprintk("%s PROCESS LO_STATEID inode %p\n", __func__, ino);
		status = nfs4_process_layout_stateid(clp, fp, &lrp->lr_sid, &ls, false);
		if (status)
			goto out_put_file;

		/* update layouts */
		layouts_found = pnfs_return_file_layouts(clp, fp, lrp, ls,
							 &lo_destroy_list);
		put_layout_state(ls);
		if (!lrp->lrs_present)
			lr_flags |= LR_FLAG_CL_EMPTY;
	} else {
		layouts_found = pnfs_return_client_layouts(clp, lrp, ex_fsid,
							   &lo_destroy_list);
		lr_flags |= LR_FLAG_CL_EMPTY;
	}

	dprintk("pNFS %s: clp %p fp %p layout_type 0x%x iomode %d "
		"return_type %d fsid 0x%llx offset %llu length %llu: "
		"layouts_found %d\n",
		__func__, clp, fp, lrp->args.lr_seg.layout_type,
		lrp->args.lr_seg.iomode, lrp->args.lr_return_type,
		ex_fsid,
		lrp->args.lr_seg.offset, lrp->args.lr_seg.length, layouts_found);

	/* update layoutrecalls
	 * note: for RETURN_{FSID,ALL}, fp may be NULL
	 */
	spin_lock(&layout_lock);
	list_for_each_entry_safe (clr, nextclr, &clp->cl_layoutrecalls,
				  clr_perclnt) {
		if (clr->cb.cbl_seg.layout_type != lrp->args.lr_seg.layout_type)
			continue;

		if (recall_return_perfect_match(clr, lrp, fp, current_fh))
			recall_cookie = layoutrecall_done(clr);
		else if (layouts_found &&
			 recall_return_partial_match(clr, lrp, fp, current_fh))
			clr->clr_time = CURRENT_TIME;
	}
	spin_unlock(&layout_lock);

out_put_file:
	if (fp)
		put_nfs4_file(fp);

out:
	nfs4_unlock_state();

	pnfsd_return_lo_list(&lo_destroy_list, ino ? ino : sb->s_root->d_inode,
			     lrp, lr_flags, recall_cookie);

out_no_fs_call:
	dprintk("pNFS %s: exit status %d\n", __func__, status);
	return status;
}

static bool
cl_has_file_layout(struct nfs4_client *clp, struct nfs4_file *fp,
		   stateid_t *lsid, struct nfsd4_pnfs_cb_layout *cbl)
{
	struct nfs4_layout *lo;
	bool ret = false;

	spin_lock(&layout_lock);
	list_for_each_entry(lo, &fp->fi_layouts, lo_perfile) {
		if (same_clid(&lo->lo_client->cl_clientid, &clp->cl_clientid) &&
		    lo_seg_overlapping(&cbl->cbl_seg, &lo->lo_seg) &&
		    (cbl->cbl_seg.iomode & lo->lo_seg.iomode))
			goto found;
	}
	goto unlock;
found:
	/* Im going to send a recall on this latout update state */
	update_layout_stateid_locked(lo->lo_state, lsid);
	ret = true;
unlock:
	spin_unlock(&layout_lock);
	return ret;
}

static int
cl_has_fsid_layout(struct nfs4_client *clp, struct nfs4_fsid *fsid)
{
	int found = 0;
	struct nfs4_layout *lp;

	/* note: minor version unused */
	spin_lock(&layout_lock);
	list_for_each_entry(lp, &clp->cl_layouts, lo_perclnt)
		if (lp->lo_file->fi_fsid.major == fsid->major) {
			found = 1;
			break;
		}
	spin_unlock(&layout_lock);
	return found;
}

static int
cl_has_any_layout(struct nfs4_client *clp)
{
	return !list_empty(&clp->cl_layouts);
}

static int
cl_has_layout(struct nfs4_client *clp, struct nfsd4_pnfs_cb_layout *cbl,
	      struct nfs4_file *lrfile, stateid_t *lsid)
{
	switch (cbl->cbl_recall_type) {
	case RETURN_FILE:
		return cl_has_file_layout(clp, lrfile, lsid, cbl);
	case RETURN_FSID:
		return cl_has_fsid_layout(clp, &cbl->cbl_fsid);
	default:
		return cl_has_any_layout(clp);
	}
}

/*
 * Called without the layout_lock.
 */
void
nomatching_layout(struct nfs4_layoutrecall *clr)
{
	struct nfsd4_pnfs_layoutreturn lr = {
		.args.lr_return_type = clr->cb.cbl_recall_type,
		.args.lr_seg = clr->cb.cbl_seg,
	};
	struct inode *inode;
	void *recall_cookie;
	LIST_HEAD(lo_destroy_list);
	int lr_flags = LR_FLAG_INTERN;

	dprintk("%s: clp %p fp %p: simulating layout_return\n", __func__,
		clr->clr_client, clr->clr_file);

	if (clr->cb.cbl_recall_type == RETURN_FILE) {
		pnfs_return_file_layouts(clr->clr_client, clr->clr_file, &lr,
					 NULL, &lo_destroy_list);
		if (!lr.lrs_present)
			lr_flags |= LR_FLAG_CL_EMPTY;
	} else {
		pnfs_return_client_layouts(clr->clr_client, &lr,
					   clr->cb.cbl_fsid.major,
					   &lo_destroy_list);
		lr_flags |= LR_FLAG_CL_EMPTY;
	}

	spin_lock(&layout_lock);
	recall_cookie = layoutrecall_done(clr);
	spin_unlock(&layout_lock);

	inode = clr->clr_file->fi_inode ?: clr->clr_sb->s_root->d_inode;
	pnfsd_return_lo_list(&lo_destroy_list, inode, &lr, lr_flags,
			     recall_cookie);
}

/* Return On Close:
 *   Look for all layouts of @fp that belong to @clp, if ROC is set, remove
 *   the layout and simulate a layout_return. Surly the client has forgotten
 *   these layouts or it would return them before the close.
 */
void pnfsd_roc(struct nfs4_client *clp, struct nfs4_file *fp)
{
	struct nfs4_layout *lo, *nextlp;
	LIST_HEAD(lo_destroy_list);

	/* TODO: We need to also free layout recalls like pnfs_expire_client */
	dprintk("%s: fp=%p clp=%p", __func__, fp, clp);
	spin_lock(&layout_lock);
	list_for_each_entry_safe (lo, nextlp, &fp->fi_layouts, lo_perfile) {
		/* Check for a match */
		if (lo->lo_client != clp)
			continue;

		/* Return the layout */
		list_del_init(&lo->lo_state->ls_perfile);	/* just to be on the safe side */
		dequeue_layout(lo);
		list_add_tail(&lo->lo_perfile, &lo_destroy_list);
	}
	spin_unlock(&layout_lock);

	pnfsd_return_lo_list(&lo_destroy_list, NULL, NULL, LR_FLAG_INTERN, NULL);
}

void pnfs_expire_client(struct nfs4_client *clp)
{
	struct nfs4_layout *lo, *nextlo;
	LIST_HEAD(lo_destroy_list);

	for (;;) {
		struct nfs4_layoutrecall *lrp = NULL;

		spin_lock(&layout_lock);
		if (!list_empty(&clp->cl_layoutrecalls)) {
			lrp = list_entry(clp->cl_layoutrecalls.next,
					 struct nfs4_layoutrecall, clr_perclnt);
			get_layoutrecall(lrp);
		}
		spin_unlock(&layout_lock);
		if (!lrp)
			break;

		dprintk("%s: lrp %p, fp %p\n", __func__, lrp, lrp->clr_file);
		BUG_ON(lrp->clr_client != clp);
		nomatching_layout(lrp);
		put_layoutrecall(lrp);
	}

	spin_lock(&layout_lock);
	list_for_each_entry_safe (lo, nextlo, &clp->cl_layouts, lo_perclnt) {
		BUG_ON(lo->lo_client != clp);
		dequeue_layout(lo);
		list_add_tail(&lo->lo_perfile, &lo_destroy_list);
		dprintk("%s: inode %lu lp %p clp %p\n", __func__,
			lo->lo_file->fi_inode->i_ino, lo, clp);
	}
	spin_unlock(&layout_lock);

	pnfsd_return_lo_list(&lo_destroy_list, NULL, NULL, LR_FLAG_EXPIRE, NULL);
}

struct create_recall_list_arg {
	struct nfsd4_pnfs_cb_layout *cbl;
	struct nfs4_file *lrfile;
	struct list_head *todolist;
	unsigned todo_count;
};

/*
 * look for matching layout for the given client
 * and add a pending layout recall to the todo list
 * if found any.
 * returns:
 *   0 if layouts found or negative error.
 */
static int
lo_recall_per_client(struct nfs4_client *clp, void *p)
{
	stateid_t lsid;
	struct nfs4_layoutrecall *pending;
	struct create_recall_list_arg *arg = p;

	memset(&lsid, 0, sizeof(lsid));
	if (!cl_has_layout(clp, arg->cbl, arg->lrfile, &lsid))
		return 0;

	/* Matching put done by layoutreturn */
	pending = alloc_init_layoutrecall(arg->cbl, clp, arg->lrfile);
	/* out of memory, drain todo queue */
	if (!pending)
		return -ENOMEM;

	*(stateid_t *)&pending->cb.cbl_sid = lsid;
	list_add(&pending->clr_perclnt, arg->todolist);
	arg->todo_count++;
	return 0;
}

/* Create a layoutrecall structure for each client based on the
 * original structure. */
int
create_layout_recall_list(struct list_head *todolist, unsigned *todo_len,
			  struct nfsd4_pnfs_cb_layout *cbl,
			  struct nfs4_file *lrfile)
{
	struct nfs4_client *clp;
	struct create_recall_list_arg arg = {
		.cbl = cbl,
		.lrfile = lrfile,
		.todolist = todolist,
	};
	int status = 0;

#if 0
	dprintk("%s: -->\n", __func__);

	/* If client given by fs, just do single client */
	if (cbl->cbl_seg.clientid) {
		clp = find_confirmed_client((clientid_t *)&cbl->cbl_seg.clientid, true);
		if (!clp) {
			status = -ENOENT;
			dprintk("%s: clientid %llx not found\n", __func__,
				(unsigned long long)cbl->cbl_seg.clientid);
			goto out;
		}

		status = lo_recall_per_client(clp, &arg);
	} else {
		/* Check all clients for layout matches */
		status = filter_confirmed_clients(lo_recall_per_client, &arg);
	}

out:
	*todo_len = arg.todo_count;
	dprintk("%s: <-- list len %u status %d\n", __func__, *todo_len, status);
#endif
	return status;
}

/*
 * Recall layouts asynchronously
 * Called with state lock.
 */
static int
spawn_layout_recall(struct super_block *sb, struct list_head *todolist,
		    unsigned todo_len)
{
	struct nfs4_layoutrecall *pending;
	struct nfs4_layoutrecall *parent = NULL;
	int status = 0;

	dprintk("%s: -->\n", __func__);

	if (todo_len > 1) {
		pending = list_entry(todolist->next, struct nfs4_layoutrecall,
				     clr_perclnt);

		parent = alloc_init_layoutrecall(&pending->cb, NULL,
						 pending->clr_file);
		if (unlikely(!parent)) {
			/* We want forward progress. If parent cannot be
			 * allocated take the first one as parent but don't
			 * execute it.  Caller must check for -EAGAIN, if so
			 * When the partial recalls return,
			 * nfsd_layout_recall_cb should be called again.
			 */
			list_del_init(&pending->clr_perclnt);
			if (todo_len > 2) {
				parent = pending;
			} else {
				parent = NULL;
				put_layoutrecall(pending);
			}
			--todo_len;
				status = -ENOMEM;
		}
	}

	while (!list_empty(todolist)) {
		pending = list_entry(todolist->next, struct nfs4_layoutrecall,
				     clr_perclnt);
		list_del_init(&pending->clr_perclnt);
		dprintk("%s: clp %p cb_client %p fp %p\n", __func__,
			pending->clr_client,
			pending->clr_client->cl_cb_client,
			pending->clr_file);
		if (unlikely(!pending->clr_client->cl_cb_client)) {
			printk(KERN_INFO
				"%s: clientid %08x/%08x has no callback path\n",
				__func__,
				pending->clr_client->cl_clientid.cl_boot,
				pending->clr_client->cl_clientid.cl_id);
			put_layoutrecall(pending);
			continue;
		}

		pending->clr_time = CURRENT_TIME;
		pending->clr_sb = sb;
		if (parent) {
			/* If we created a parent its initial ref count is 1.
			 * We will need to de-ref it eventually. So we just
			 * don't increment on behalf of the last one.
			 */
			if (todo_len != 1)
				get_layoutrecall(parent);
		}
		pending->parent = parent;
		get_layoutrecall(pending);
		/* Add to list so corresponding layoutreturn can find req */
		list_add(&pending->clr_perclnt,
			 &pending->clr_client->cl_layoutrecalls);

		nfsd4_cb_layout(pending);
		--todo_len;
	}

	return status;
}

/*
 * Spawn a thread to perform a recall layout
 *
 */
int
_nfsd_layout_recall_cb(struct super_block *sb, struct inode *inode,
		       struct nfsd4_pnfs_cb_layout *cbl, bool with_nfs4_state_lock)
{
	int status;
	struct nfs4_file *lrfile = NULL;
	struct list_head todolist;
	unsigned todo_len = 0;

	dprintk("NFSD nfsd_layout_recall_cb: inode %p cbl %p\n", inode, cbl);
	BUG_ON(!cbl);
	BUG_ON(cbl->cbl_recall_type != RETURN_FILE &&
	       cbl->cbl_recall_type != RETURN_FSID &&
	       cbl->cbl_recall_type != RETURN_ALL);
	BUG_ON(cbl->cbl_recall_type == RETURN_FILE && !inode);
	BUG_ON(cbl->cbl_seg.iomode != IOMODE_READ &&
	       cbl->cbl_seg.iomode != IOMODE_RW &&
	       cbl->cbl_seg.iomode != IOMODE_ANY);

	if (nfsd_serv == NULL) {
		dprintk("NFSD nfsd_layout_recall_cb: nfsd_serv == NULL\n");
		return -ENOENT;
	}

	if (!with_nfs4_state_lock)
		nfs4_lock_state();
	status = -ENOENT;
	if (inode) {
		lrfile = find_file(inode);
		if (!lrfile) {
			dprintk("NFSD nfsd_layout_recall_cb: "
				"nfs4_file not found\n");
			goto err;
		}
		if (cbl->cbl_recall_type == RETURN_FSID)
			cbl->cbl_fsid = lrfile->fi_fsid;
	}

	INIT_LIST_HEAD(&todolist);

	status = create_layout_recall_list(&todolist, &todo_len, cbl, lrfile);
	if (list_empty(&todolist)) {
		status = -ENOENT;
	} else {
		/* process todolist even if create_layout_recall_list
		 * returned an error */
		int status2 = spawn_layout_recall(sb, &todolist, todo_len);
		if (status2)
			status = status2;
	}

err:
	if (!with_nfs4_state_lock)
		nfs4_unlock_state();
	if (lrfile)
		put_nfs4_file(lrfile);
	return (todo_len && status) ? -EAGAIN : status;
}

int
nfsd_layout_recall_cb(struct super_block *sb, struct inode *inode,
		      struct nfsd4_pnfs_cb_layout *cbl)
{
	return _nfsd_layout_recall_cb(sb, inode, cbl, false);
}
