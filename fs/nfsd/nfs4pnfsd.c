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

#define NFSDDBG_FACILITY                NFSDDBG_PROC

/* Globals */
static u32 current_layoutid = 1;

/*
 * Currently used for manipulating the layout state.
 */
static DEFINE_SPINLOCK(layout_lock);

/*
 * Layout state - NFSv4.1 pNFS
 */
static struct kmem_cache *pnfs_layout_slab;

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

static inline unsigned long
sbid_hashval(struct super_block *sb)
{
	return hash_ptr(sb, SBID_HASH_BITS);
}

static inline struct sbid_tracker *
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

	for (i = 0; i < SBID_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&sbid_hashtbl[i]);
	}

	return 0;
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

__be32
nfs4_pnfs_get_layout(struct nfsd4_pnfs_layoutget *lgp,
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

	args.lg_sbid = find_create_sbid(sb);
	if (!args.lg_sbid) {
		nfserr = nfserr_layouttrylater;
		goto out;
	}

	can_merge = sb->s_pnfs_op->can_merge_layouts != NULL &&
		    sb->s_pnfs_op->can_merge_layouts(lgp->lg_seg.layout_type);

	nfs4_lock_state();
	fp = find_alloc_file(ino, lgp->lg_fhp);
	clp = find_confirmed_client((clientid_t *)&lgp->lg_seg.clientid);
	dprintk("pNFS %s: fp %p clp %p \n", __func__, fp, clp);
	if (!fp || !clp) {
		nfserr = nfserr_inval;
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

	/* SUCCESS!
	 * Can the new layout be merged into an existing one?
	 * If so, free unused layout struct
	 */
	if (can_merge && merge_layout(fp, clp, &res.lg_seg))
		goto out_freelayout;

	/* Can't merge, so let's initialize this new layout */
	init_layout(ls, lp, fp, clp, lgp->lg_fhp, &res.lg_seg);
out_unlock:
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
