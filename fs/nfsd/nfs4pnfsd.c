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

/*
 * Layout state - NFSv4.1 pNFS
 */
static struct kmem_cache *pnfs_layout_slab;

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
init_layout(struct nfs4_layout *lp,
	    struct nfs4_file *fp,
	    struct nfs4_client *clp,
	    struct svc_fh *current_fh,
	    struct nfsd4_layout_seg *seg)
{
	dprintk("pNFS %s: lp %p clp %p fp %p ino %p\n", __func__,
		lp, clp, fp, fp->fi_inode);

	get_nfs4_file(fp);
	lp->lo_client = clp;
	lp->lo_file = fp;
	memcpy(&lp->lo_seg, seg, sizeof(lp->lo_seg));
	list_add_tail(&lp->lo_perclnt, &clp->cl_layouts);
	list_add_tail(&lp->lo_perfile, &fp->fi_layouts);
	dprintk("pNFS %s end\n", __func__);
}

static void
dequeue_layout(struct nfs4_layout *lp)
{
	list_del(&lp->lo_perclnt);
	list_del(&lp->lo_perfile);
}

static void
destroy_layout(struct nfs4_layout *lp)
{
	struct nfs4_client *clp;
	struct nfs4_file *fp;

	dequeue_layout(lp);
	clp = lp->lo_client;
	fp = lp->lo_file;
	dprintk("pNFS %s: lp %p clp %p fp %p ino %p\n", __func__,
		lp, clp, fp, fp->fi_inode);

	kmem_cache_free(pnfs_layout_slab, lp);
	put_nfs4_file(fp);
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
	struct nfs4_layout *lp;

	list_for_each_entry (lp, &fp->fi_layouts, lo_perfile)
	if (lp->lo_seg.layout_type == seg->layout_type &&
	    lp->lo_seg.clientid == seg->clientid &&
	    lp->lo_seg.iomode == seg->iomode &&
	    lo_seg_mergeable(&lp->lo_seg, seg)) {
		extend_layout(&lp->lo_seg, seg);
		return lp;
	}

	return NULL;
}

int
nfs4_pnfs_get_layout(struct svc_fh *current_fh,
		     struct pnfs_layoutget_arg *args)
{
	int status = nfserr_layouttrylater;
	struct inode *ino = current_fh->fh_dentry->d_inode;
	struct nfs4_file *fp;
	struct nfs4_client *clp;
	struct nfs4_layout *lp = NULL;

	dprintk("NFSD: %s Begin\n", __func__);

	nfs4_lock_state();
	fp = find_alloc_file(ino, current_fh);
	clp = find_confirmed_client((clientid_t *)&args->seg.clientid);
	dprintk("pNFS %s: fp %p clp %p \n", __func__, fp, clp);
	if (!fp || !clp)
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

	init_layout(lp, fp, clp, current_fh, &args->seg);
out:
	if (fp)
		put_nfs4_file(fp);
	nfs4_unlock_state();
	dprintk("pNFS %s: lp %p exit status %d\n", __func__, lp, status);
	return status;
out_freelayout:
	free_layout(lp);
	goto out;
}

#endif /* CONFIG_PNFSD */
