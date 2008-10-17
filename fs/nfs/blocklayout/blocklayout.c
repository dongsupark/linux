/*
 *  linux/fs/nfs/blocklayout/blocklayout.c
 *
 *  Module for the NFSv4.1 pNFS block layout driver.
 *
 *  Copyright (c) 2006 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson <andros@citi.umich.edu>
 *  Fred Isaman <iisaman@umich.edu>
 *
 * permission is granted to use, copy, create derivative works and
 * redistribute this software and such derivative works for any purpose,
 * so long as the name of the university of michigan is not used in
 * any advertising or publicity pertaining to the use or distribution
 * of this software without specific, written prior authorization.  if
 * the above copyright notice or any other identification of the
 * university of michigan is included in any copy of any portion of
 * this software, then the disclaimer below must also be included.
 *
 * this software is provided as is, without representation from the
 * university of michigan as to its fitness for any purpose, and without
 * warranty by the university of michigan of any kind, either express
 * or implied, including without limitation the implied warranties of
 * merchantability and fitness for a particular purpose.  the regents
 * of the university of michigan shall not be liable for any damages,
 * including special, indirect, incidental, or consequential damages,
 * with respect to any claim arising out or in connection with the use
 * of the software, even if it has been or is hereafter advised of the
 * possibility of such damages.
 */
#include <linux/module.h>
#include <linux/init.h>

#include <linux/buffer_head.h> /* various write calls */
#include <linux/bio.h> /* struct bio */
#include "blocklayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andy Adamson <andros@citi.umich.edu>");
MODULE_DESCRIPTION("The NFSv4.1 pNFS Block layout driver");

/* Callback operations to the pNFS client */
struct pnfs_client_operations *pnfs_callback_ops;

static void print_page(struct page *page)
{
	dprintk("PRINTPAGE page %p\n", page);
	dprintk("        PagePrivate %d\n", PagePrivate(page));
	dprintk("        PageUptodate %d\n", PageUptodate(page));
	dprintk("        PageError %d\n", PageError(page));
	dprintk("        PageDirty %d\n", PageDirty(page));
	dprintk("        PageReferenced %d\n", PageReferenced(page));
	dprintk("        PageLocked %d\n", PageLocked(page));
	dprintk("        PageWriteback %d\n", PageWriteback(page));
	dprintk("        PageMappedToDisk %d\n", PageMappedToDisk(page));
	dprintk("\n");
}

/* Given the be associated with isect, determine if page data needs to be
 * initialized.
 */
static int is_hole(struct pnfs_block_extent *be, sector_t isect)
{
	if (be->be_state == PNFS_BLOCK_NONE_DATA)
		return 1;
	else if (be->be_state != PNFS_BLOCK_INVALID_DATA)
		return 0;
	else
		return !is_sector_initialized(be->be_inval, isect);
}

/* Given the be associated with isect, determine if page data can be
 * written to disk.
 */
static int is_writable(struct pnfs_block_extent *be, sector_t isect)
{
	if (be->be_state == PNFS_BLOCK_READWRITE_DATA)
		return 1;
	else if (be->be_state != PNFS_BLOCK_INVALID_DATA)
		return 0;
	else
		return is_sector_initialized(be->be_inval, isect);
}

static int
dont_like_caller(struct nfs_page *req)
{
	if (atomic_read(&req->wb_complete)) {
		/* Called by _multi */
		return 1;
	} else {
		/* Called by _one */
		return 0;
	}
}

static enum pnfs_try_status
bl_commit(struct pnfs_layout_type *lo,
		int sync,
		struct nfs_write_data *nfs_data)
{
	dprintk("%s enter\n", __func__);
	return PNFS_NOT_ATTEMPTED;
}

/* The data we are handed might be spread across several bios.  We need
 * to track when the last one is finished.
 */
struct parallel_io {
	struct kref refcnt;
	struct rpc_call_ops call_ops;
	void (*pnfs_callback) (void *data);
	void *data;
};

static inline struct parallel_io *alloc_parallel(void *data)
{
	struct parallel_io *rv;

	rv  = kmalloc(sizeof(*rv), GFP_KERNEL);
	if (rv) {
		rv->data = data;
		kref_init(&rv->refcnt);
	}
	return rv;
}

static inline void get_parallel(struct parallel_io *p)
{
	kref_get(&p->refcnt);
}

static void destroy_parallel(struct kref *kref)
{
	struct parallel_io *p = container_of(kref, struct parallel_io, refcnt);

	dprintk("%s enter\n", __func__);
	p->pnfs_callback(p->data);
	kfree(p);
}

static inline void put_parallel(struct parallel_io *p)
{
	kref_put(&p->refcnt, destroy_parallel);
}

static struct bio *
bl_submit_bio(int rw, struct bio *bio)
{
	if (bio) {
		get_parallel(bio->bi_private);
		dprintk("%s submitting %s bio %u@%llu\n", __func__,
			rw == READ ? "read" : "write",
			bio->bi_size, (u64)bio->bi_sector);
		submit_bio(rw, bio);
	}
	return NULL;
}

static inline void
bl_done_with_rpage(struct page *page, const int ok)
{
	if (ok) {
		ClearPagePnfsErr(page);
		SetPageUptodate(page);
	} else {
		ClearPageUptodate(page);
		SetPageError(page);
		SetPagePnfsErr(page);
	}
	/* Page is unlocked via rpc_release.  Should really be done here. */
}

/* This is basically copied from mpage_end_io_read */
static void bl_end_io_read(struct bio *bio, int err)
{
	void *data = bio->bi_private;
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);
		bl_done_with_rpage(page, uptodate);
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
	put_parallel(data);
}

static void bl_read_cleanup(struct work_struct *work)
{
	struct rpc_task *task;
	struct nfs_read_data *rdata;
	dprintk("%s enter\n", __func__);
	task = container_of(work, struct rpc_task, u.tk_work);
	rdata = container_of(task, struct nfs_read_data, task);
	pnfs_callback_ops->nfs_readlist_complete(rdata);
}

static void
bl_end_par_io_read(void *data)
{
	struct nfs_read_data *rdata = data;

	INIT_WORK(&rdata->task.u.tk_work, bl_read_cleanup);
	schedule_work(&rdata->task.u.tk_work);
}

/* We don't want normal .rpc_call_done callback used, so we replace it
 * with this stub.
 */
static void bl_rpc_do_nothing(struct rpc_task *task, void *calldata)
{
	return;
}

static enum pnfs_try_status
bl_read_pagelist(struct pnfs_layout_type *lo,
		struct page **pages,
		unsigned int pgbase,
		unsigned nr_pages,
		loff_t f_offset,
		size_t count,
		struct nfs_read_data *rdata)
{
	int i, hole;
	struct bio *bio = NULL;
	struct pnfs_block_extent *be = NULL, *cow_read = NULL;
	sector_t isect, extent_length = 0;
	struct parallel_io *par;

	dprintk("%s enter nr_pages %u offset %lld count %Zd\n", __func__,
	       nr_pages, f_offset, count);

	if (dont_like_caller(rdata->req)) {
		dprintk("%s dont_like_caller failed\n", __func__);
		goto use_mds;
	}
	if ((nr_pages == 1) && PagePnfsErr(rdata->req->wb_page)) {
		/* We want to fall back to mds in case of read_page
		 * after error on read_pages.
		 */
		dprintk("%s PG_pnfserr set\n", __func__);
		goto use_mds;
	}
	par = alloc_parallel(rdata);
	if (!par)
		goto use_mds;
	par->call_ops = *rdata->pdata.call_ops;
	par->call_ops.rpc_call_done = bl_rpc_do_nothing;
	par->pnfs_callback = bl_end_par_io_read;
	/* At this point, we can no longer jump to use_mds */

	isect = (sector_t) (f_offset >> 9);
	/* Code assumes extents are page-aligned */
	for (i = 0; i < nr_pages; i++) {
		if (!extent_length) {
			/* We've used up the previous extent */
			put_extent(be);
			put_extent(cow_read);
			bio = bl_submit_bio(READ, bio);
			/* Get the next one */
			be = find_get_extent(BLK_LSEG2EXT(rdata->pdata.lseg),
					     isect, &cow_read);
			if (!be) {
				/* Error out this page */
				bl_done_with_rpage(pages[i], 0);
				isect += PAGE_CACHE_SIZE >> 9;
				continue;
			}
			extent_length = be->be_length -
				(isect - be->be_f_offset);
			if (cow_read) {
				sector_t cow_length = cow_read->be_length -
					(isect - cow_read->be_f_offset);
				extent_length = min(extent_length, cow_length);
			}
		}
		hole = is_hole(be, isect);
		if (hole && !cow_read) {
			bio = bl_submit_bio(READ, bio);
			/* Fill hole w/ zeroes w/o accessing device */
			dprintk("%s Zeroing page for hole\n", __func__);
			zero_user(pages[i], 0,
				  min_t(int, PAGE_CACHE_SIZE, count));
			print_page(pages[i]);
			bl_done_with_rpage(pages[i], 1);
		} else {
			struct pnfs_block_extent *be_read;

			be_read = (hole && cow_read) ? cow_read : be;
			for (;;) {
				if (!bio) {
					bio = bio_alloc(GFP_NOIO, nr_pages - i);
					if (!bio) {
						/* Error out this page */
						bl_done_with_rpage(pages[i], 0);
						break;
					}
					bio->bi_sector = isect -
						be_read->be_f_offset +
						be_read->be_v_offset;
					bio->bi_bdev = be_read->be_mdev;
					bio->bi_end_io = bl_end_io_read;
					bio->bi_private = par;
				}
				if (bio_add_page(bio, pages[i], PAGE_SIZE, 0))
					break;
				bio = bl_submit_bio(READ, bio);
			}
		}
		isect += PAGE_CACHE_SIZE >> 9;
		extent_length -= PAGE_CACHE_SIZE >> 9;
	}
	put_extent(be);
	put_extent(cow_read);
	bl_submit_bio(READ, bio);
	put_parallel(par);
	return PNFS_ATTEMPTED;

 use_mds:
	dprintk("Giving up and using normal NFS\n");
	return PNFS_NOT_ATTEMPTED;
}

static void mark_extents_written(struct pnfs_block_layout *bl,
				 __u64 offset, __u32 count)
{
	sector_t isect, end;
	struct pnfs_block_extent *be;

	dprintk("%s(%llu, %u)\n", __func__, offset, count);
	if (count == 0)
		return;
	isect = (offset & (long)(PAGE_CACHE_MASK)) >> 9;
	end = (offset + count + PAGE_CACHE_SIZE - 1) & (long)(PAGE_CACHE_MASK);
	end >>= 9;
	while (isect < end) {
		be = find_get_extent(bl, isect, NULL);
		BUG_ON(!be); /* FIXME */
		if (be->be_state != PNFS_BLOCK_INVALID_DATA)
			isect += be->be_length;
		else {
			sector_t len;
			len = min(end, be->be_f_offset + be->be_length) - isect;
			mark_for_commit(be, isect, len); /* What if fails? */
			isect += len;
		}
		put_extent(be);
	}
}

/* STUB - this needs thought */
static inline void
bl_done_with_wpage(struct page *page, const int ok)
{
	if (!ok) {
		SetPageError(page);
		SetPagePnfsErr(page);
		/* This is an inline copy of nfs_zap_mapping */
		/* This is oh so fishy, and needs deep thought */
		if (page->mapping->nrpages != 0) {
			struct inode *inode = page->mapping->host;
			spin_lock(&inode->i_lock);
			NFS_I(inode)->cache_validity |= NFS_INO_INVALID_DATA;
			spin_unlock(&inode->i_lock);
		}
	}
	/* end_page_writeback called in rpc_release.  Should be done here. */
}

/* This is basically copied from mpage_end_io_read */
static void bl_end_io_write(struct bio *bio, int err)
{
	void *data = bio->bi_private;
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);
		bl_done_with_wpage(page, uptodate);
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
	put_parallel(data);
}

/* Function scheduled for call during bl_end_par_io_write,
 * it marks sectors as written and extends the commitlist.
 */
static void bl_write_cleanup(struct work_struct *work)
{
	struct rpc_task *task;
	struct nfs_write_data *wdata;
	dprintk("%s enter\n", __func__);
	task = container_of(work, struct rpc_task, u.tk_work);
	wdata = container_of(task, struct nfs_write_data, task);
	if (!wdata->task.tk_status) {
		/* Marks for LAYOUTCOMMIT */
		/* BUG - this should be called after each bio, not after
		 * all finish, unless have some way of storing success/failure
		 */
		mark_extents_written(BLK_LSEG2EXT(wdata->pdata.lseg),
				     wdata->args.offset, wdata->args.count);
	}
	pnfs_callback_ops->nfs_writelist_complete(wdata);
}

/* Called when last of bios associated with a bl_write_pagelist call finishes */
static void
bl_end_par_io_write(void *data)
{
	struct nfs_write_data *wdata = data;

	/* STUB - ignoring error handling */
	wdata->task.tk_status = 0;
	wdata->res.count = wdata->args.count;
	wdata->verf.committed = NFS_FILE_SYNC;
	INIT_WORK(&wdata->task.u.tk_work, bl_write_cleanup);
	schedule_work(&wdata->task.u.tk_work);
}

static enum pnfs_try_status
bl_write_pagelist(struct pnfs_layout_type *lo,
		struct page **pages,
		unsigned int pgbase,
		unsigned nr_pages,
		loff_t offset,
		size_t count,
		int sync,
		struct nfs_write_data *wdata)
{
	int i;
	struct bio *bio = NULL;
	struct pnfs_block_extent *be = NULL;
	sector_t isect, extent_length = 0;
	struct parallel_io *par;

	dprintk("%s enter, %Zu@%lld\n", __func__, count, offset);
	if (!test_bit(PG_USE_PNFS, &wdata->req->wb_flags)) {
		dprintk("PG_USE_PNFS not set\n");
		return PNFS_NOT_ATTEMPTED;
	}
	if (dont_like_caller(wdata->req)) {
		dprintk("%s dont_like_caller failed\n", __func__);
		return PNFS_NOT_ATTEMPTED;
	}
	/* At this point, wdata->pages is a (sequential) list of nfs_pages.
	 * We want to write each, and if there is an error remove it from
	 * list and call
	 * nfs_retry_request(req) to have it redone using nfs.
	 * QUEST? Do as block or per req?  Think have to do per block
	 * as part of end_bio
	 */
	par = alloc_parallel(wdata);
	if (!par)
		return PNFS_NOT_ATTEMPTED;
	par->call_ops = *wdata->pdata.call_ops;
	par->call_ops.rpc_call_done = bl_rpc_do_nothing;
	par->pnfs_callback = bl_end_par_io_write;
	/* At this point, have to be more careful with error handling */

	isect = (sector_t) ((offset & (long)PAGE_CACHE_MASK) >> 9);
	for (i = 0; i < nr_pages; i++) {
		if (!extent_length) {
			/* We've used up the previous extent */
			put_extent(be);
			bio = bl_submit_bio(WRITE, bio);
			/* Get the next one */
			be = find_get_extent(BLK_LSEG2EXT(wdata->pdata.lseg),
					     isect, NULL);
			if (!be || !is_writable(be, isect)) {
				/* FIXME */
				bl_done_with_wpage(pages[i], 0);
				isect += PAGE_CACHE_SECTORS;
				continue;
			}
			extent_length = be->be_length -
				(isect - be->be_f_offset);
		}
		for (;;) {
			if (!bio) {
				bio = bio_alloc(GFP_NOIO, nr_pages - i);
				if (!bio) {
					/* Error out this page */
					/* FIXME */
					bl_done_with_wpage(pages[i], 0);
					break;
				}
				bio->bi_sector = isect - be->be_f_offset +
					be->be_v_offset;
				bio->bi_bdev = be->be_mdev;
				bio->bi_end_io = bl_end_io_write;
				bio->bi_private = par;
			}
			if (bio_add_page(bio, pages[i], PAGE_SIZE, 0))
				break;
			bio = bl_submit_bio(WRITE, bio);
		}
		isect += PAGE_CACHE_SIZE >> 9;
		extent_length -= PAGE_CACHE_SIZE >> 9;
	}
	put_extent(be);
	bl_submit_bio(WRITE, bio);
	put_parallel(par);
	return PNFS_ATTEMPTED;
}

/* FIXME - range ignored */
static void
release_extents(struct pnfs_block_layout *bl,
		struct nfs4_pnfs_layout_segment *range)
{
	int i;
	struct pnfs_block_extent *be;

	spin_lock(&bl->bl_ext_lock);
	for (i = 0; i < EXTENT_LISTS; i++) {
		while (!list_empty(&bl->bl_extents[i])) {
			be = list_first_entry(&bl->bl_extents[i],
					      struct pnfs_block_extent,
					      be_node);
			list_del(&be->be_node);
			put_extent(be);
		}
	}
	spin_unlock(&bl->bl_ext_lock);
}

/* STUB */
static void
release_inval_marks(void)
{
	return;
}

/* Note we are relying on caller locking to prevent nasty races. */
static void
bl_free_layout(struct pnfs_layout_type *lo)
{
	struct pnfs_block_layout	*bl;

	dprintk("%s enter\n", __func__);
	bl = BLK_LO2EXT(lo);
	release_extents(bl, NULL);
	release_inval_marks();
	kfree(lo);
	return;
}

static struct pnfs_layout_type *
bl_alloc_layout(struct pnfs_mount_type *mtype, struct inode *inode)
{
	struct pnfs_layout_type		*lo;
	struct pnfs_block_layout	*bl;

	dprintk("%s enter\n", __func__);
	lo = kzalloc(sizeof(*lo) + sizeof(*bl), GFP_KERNEL);
	if (!lo)
		return NULL;
	bl = BLK_LO2EXT(lo);
	spin_lock_init(&bl->bl_ext_lock);
	INIT_LIST_HEAD(&bl->bl_extents[0]);
	INIT_LIST_HEAD(&bl->bl_extents[1]);
	INIT_LIST_HEAD(&bl->bl_commit);
	bl->bl_count = 0;
	bl->bl_blocksize = NFS_SERVER(inode)->pnfs_blksize >> 9;
	INIT_INVAL_MARKS(&bl->bl_inval, bl->bl_blocksize);
	return lo;
}

static void
bl_free_lseg(struct pnfs_layout_segment *lseg)
{
	dprintk("%s enter\n", __func__);
	kfree(lseg);
}

/* Because the generic infrastructure does not correctly merge layouts,
 * we pretty much ignore lseg, and store all data layout wide, so we
 * can correctly merge.  Eventually we should push some correct merge
 * behavior up to the generic code, as the current behavior tends to
 * cause lots of unnecessary overlapping LAYOUTGET requests.
 */
static struct pnfs_layout_segment *
bl_alloc_lseg(struct pnfs_layout_type *lo,
	      struct nfs4_pnfs_layoutget_res *lgr)
{
	struct pnfs_layout_segment *lseg;
	int status;

	dprintk("%s enter\n", __func__);
	lseg = kzalloc(sizeof(*lseg) + 0, GFP_KERNEL);
	if (!lseg)
		return NULL;
	status = nfs4_blk_process_layoutget(BLK_LO2EXT(lo), lgr);
	if (status) {
		/* We don't want to call the full-blown bl_free_lseg,
		 * since on error extents were not touched.
		 */
		/* STUB - we really want to distinguish between 2 error
		 * conditions here.  This lseg failed, but lo data structures
		 * are OK, or we hosed the lo data structures.  The calling
		 * code probably needs to distinguish this too.
		 */
		kfree(lseg);
		return ERR_PTR(status);
	}
	return lseg;
}

static int
bl_setup_layoutcommit(struct pnfs_layout_type *lo,
		      struct pnfs_layoutcommit_data *data)
{
	struct nfs_server *nfss = PNFS_NFS_SERVER(lo);
	struct pnfs_layoutcommit_arg *arg = &data->args;

	dprintk("%s enter\n", __func__);
	/* Need to ensure commit is block-size aligned */
	if (nfss->pnfs_blksize) {
		u64 mask = nfss->pnfs_blksize - 1;
		arg->lseg.offset &= ~mask;
		arg->lseg.length += mask;
		arg->lseg.length &= ~mask;
	}
	return encode_pnfs_block_layoutupdate4(BLK_LO2EXT(lo), arg);
}

static void
bl_cleanup_layoutcommit(struct pnfs_layout_type *lo,
			struct pnfs_layoutcommit_data *data)
{
	dprintk("%s enter\n", __func__);
}

static void free_blk_mountid(struct block_mount_id *mid)
{
	if (mid) {
		struct pnfs_block_dev *dev;
		spin_lock(&mid->bm_lock);
		while (!list_empty(&mid->bm_devlist)) {
			dev = list_first_entry(&mid->bm_devlist,
					       struct pnfs_block_dev,
					       bm_node);
			list_del(&dev->bm_node);
			free_block_dev(dev);
		}
		spin_unlock(&mid->bm_lock);
		kfree(mid);
	}
}

/* This is mostly copied form the filelayout's get_device_info function.
 * It seems much of this should be at the generic pnfs level.
 */
static struct pnfs_block_dev *
nfs4_blk_get_deviceinfo(struct super_block *sb, struct nfs_fh *fh,
			struct pnfs_deviceid *d_id,
			struct list_head *sdlist)
{
	struct pnfs_device *dev;
	struct pnfs_block_dev *rv = NULL;
	int maxpages = NFS4_GETDEVINFO_MAXSIZE >> PAGE_SHIFT;
	struct page *pages[maxpages];
	int alloced_pages = 0, used_pages = 1;
	int j, rc;

	dprintk("%s enter\n", __func__);
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dprintk("%s kmalloc failed\n", __func__);
		return NULL;
	}
 retry_once:
	dprintk("%s trying used_pages %d\n", __func__, used_pages);
	for (; alloced_pages < used_pages; alloced_pages++) {
		pages[alloced_pages] = alloc_page(GFP_KERNEL);
		if (!pages[alloced_pages])
			goto out_free;
	}
	/* set dev->area */
	if (used_pages == 1)
		dev->area = page_address(pages[0]);
	else {
		dev->area = vmap(pages, used_pages, VM_MAP, PAGE_KERNEL);
		if (!dev->area)
			goto out_free;
	}

	memcpy(&dev->dev_id, d_id, sizeof(*d_id));
	dev->layout_type = LAYOUT_BLOCK_VOLUME;
	dev->dev_notify_types = 0;
	dev->pages = pages;
	dev->pgbase = 0;
	dev->pglen = PAGE_SIZE * used_pages;
	dev->mincount = 0;

	rc = pnfs_callback_ops->nfs_getdeviceinfo(sb, dev);
	dprintk("%s getdevice info returns %d used_pages %d\n", __func__, rc,
		used_pages);
	if (rc == -ETOOSMALL && used_pages == 1) {
		dev->area = NULL;
		used_pages = (dev->mincount + PAGE_SIZE - 1) >> PAGE_SHIFT;
		if (used_pages > 1 && used_pages <= maxpages)
			goto retry_once;
	}
	if (rc)
		goto out_free;

	rv = nfs4_blk_decode_device(sb, dev, sdlist);
 out_free:
	if (used_pages > 1 && dev->area != NULL)
		vunmap(dev->area);
	for (j = 0; j < alloced_pages; j++)
		__free_page(pages[j]);
	kfree(dev);
	return rv;
}


/*
 * Retrieve the list of available devices for the mountpoint.
 */
static struct pnfs_mount_type *
bl_initialize_mountpoint(struct super_block *sb, struct nfs_fh *fh)
{
	struct block_mount_id *b_mt_id = NULL;
	struct pnfs_mount_type *mtype = NULL;
	struct pnfs_devicelist *dlist = NULL;
	struct pnfs_block_dev *bdev;
	LIST_HEAD(scsi_disklist);
	int status, i;

	dprintk("%s enter\n", __func__);

	if (NFS_SB(sb)->pnfs_blksize == 0) {
		dprintk("%s Server did not return blksize\n", __func__);
		return NULL;
	}
	b_mt_id = kzalloc(sizeof(struct block_mount_id), GFP_KERNEL);
	if (!b_mt_id)
		goto out_error;
	/* Initialize nfs4 block layout mount id */
	b_mt_id->bm_sb = sb; /* back pointer to retrieve nfs_server struct */
	spin_lock_init(&b_mt_id->bm_lock);
	INIT_LIST_HEAD(&b_mt_id->bm_devlist);
	mtype = kzalloc(sizeof(struct pnfs_mount_type), GFP_KERNEL);
	if (!mtype)
		goto out_error;
	mtype->mountid = (void *)b_mt_id;

	/* Construct a list of all visible scsi disks that have not been
	 * claimed.
	 */
	status =  nfs4_blk_create_scsi_disk_list(&scsi_disklist);
	if (status < 0)
		goto out_error;

	dlist = kmalloc(sizeof(struct pnfs_devicelist), GFP_KERNEL);
	if (!dlist)
		goto out_error;
	dlist->eof = 0;
	while (!dlist->eof) {
		status = pnfs_callback_ops->nfs_getdevicelist(sb, fh, dlist);
		if (status)
			goto out_error;
		dprintk("%s GETDEVICELIST numdevs=%i, eof=%i\n",
			__func__, dlist->num_devs, dlist->eof);
		/* For each device returned in dlist, call GETDEVICEINFO, and
		 * decode the opaque topology encoding to create a flat
		 * volume topology, matching VOLUME_SIMPLE disk signatures
		 * to disks in the visible scsi disk list.
		 * Construct an LVM meta device from the flat volume topology.
		 */
		for (i = 0; i < dlist->num_devs; i++) {
			bdev = nfs4_blk_get_deviceinfo(sb, fh,
						     &dlist->dev_id[i],
						     &scsi_disklist);
			if (!bdev)
				goto out_error;
			spin_lock(&b_mt_id->bm_lock);
			list_add(&bdev->bm_node, &b_mt_id->bm_devlist);
			spin_unlock(&b_mt_id->bm_lock);
		}
	}
	dprintk("%s SUCCESS\n", __func__);

 out_return:
	kfree(dlist);
	nfs4_blk_destroy_disk_list(&scsi_disklist);
	return mtype;

 out_error:
	free_blk_mountid(b_mt_id);
	kfree(mtype);
	mtype = NULL;
	goto out_return;
}

static int
bl_uninitialize_mountpoint(struct pnfs_mount_type *mtype)
{
	struct block_mount_id *b_mt_id = NULL;

	dprintk("%s enter\n", __func__);
	if (!mtype)
		return 0;
	b_mt_id = (struct block_mount_id *)mtype->mountid;
	free_blk_mountid(b_mt_id);
	kfree(mtype);
	dprintk("%s RETURNS\n", __func__);
	return 0;
}

/* STUB - mark intersection of layout and page as bad, so is not
 * used again.
 */
static void mark_bad_read(void)
{
	return;
}

/* Copied from buffer.c */
static void __end_buffer_read_notouch(struct buffer_head *bh, int uptodate)
{
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		/* This happens, due to failed READA attempts. */
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
}

/* Copied from buffer.c */
static void end_buffer_read_nobh(struct buffer_head *bh, int uptodate)
{
	__end_buffer_read_notouch(bh, uptodate);
}

/*
 * map_block:  map a requested I/0 block (isect) into an offset in the LVM
 * meta block_device
 */
static void
map_block(sector_t isect, struct pnfs_block_extent *be, struct buffer_head *bh)
{
	dprintk("%s enter be=%p\n", __func__, be);

	set_buffer_mapped(bh);
	bh->b_bdev = be->be_mdev;
	bh->b_blocknr = (isect - be->be_f_offset + be->be_v_offset) >>
		(be->be_mdev->bd_inode->i_blkbits - 9);

	dprintk("%s isect %ld, bh->b_blocknr %ld, using bsize %Zd\n",
				__func__, (long)isect,
				(long)bh->b_blocknr,
				bh->b_size);
	return;
}

/* Given an unmapped page, zero it (or read in page for COW),
 * and set appropriate flags/markings, but it is safe to not initialize
 * the range given in [from, to).
 */
/* This is loosely based on nobh_write_begin */
static int
init_page_for_write(struct pnfs_block_layout *bl, struct page *page,
		    unsigned from, unsigned to, sector_t **pages_to_mark)
{
	struct buffer_head *bh;
	int inval, ret = -EIO;
	struct pnfs_block_extent *be = NULL, *cow_read = NULL;
	sector_t isect;

	dprintk("%s enter, %p\n", __func__, page);
	bh = alloc_page_buffers(page, PAGE_CACHE_SIZE, 0);
	if (!bh) {
		ret = -ENOMEM;
		goto cleanup;
	}

	isect = (sector_t)page->index << (PAGE_CACHE_SHIFT - 9);
	be = find_get_extent(bl, isect, &cow_read);
	if (!be)
		goto cleanup;
	inval = is_hole(be, isect);
	dprintk("%s inval=%i, from=%u, to=%u\n", __func__, inval, from, to);
	if (inval) {
		if (be->be_state == PNFS_BLOCK_NONE_DATA) {
			dprintk("%s PANIC - got NONE_DATA extent %p\n",
				__func__, be);
			goto cleanup;
		}
		map_block(isect, be, bh);
		unmap_underlying_metadata(bh->b_bdev, bh->b_blocknr);
	}
	if (PageUptodate(page)) {
		/* Do nothing */
	} else if (inval & !cow_read) {
		zero_user_segments(page, 0, from, to, PAGE_CACHE_SIZE);
	} else if (0 < from || PAGE_CACHE_SIZE > to) {
		struct pnfs_block_extent *read_extent;

		read_extent = (inval && cow_read) ? cow_read : be;
		map_block(isect, read_extent, bh);
		lock_buffer(bh);
		bh->b_end_io = end_buffer_read_nobh;
		submit_bh(READ, bh);
		dprintk("%s: Waiting for buffer read\n", __func__);
		/* XXX Don't really want to hold layout lock here */
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			goto cleanup;
	}
	if (be->be_state == PNFS_BLOCK_INVALID_DATA) {
		/* There is a BUG here if is a short copy after write_begin,
		 * but I think this is a generic fs bug.  The problem is that
		 * we have marked the page as initialized, but it is possible
		 * that the section not copied may never get copied.
		 */
		ret = mark_initialized_sectors(be->be_inval, isect,
					       PAGE_CACHE_SECTORS,
					       pages_to_mark);
		/* Want to preallocate mem so above can't fail */
		if (ret)
			goto cleanup;
	}
	SetPageMappedToDisk(page);
	ret = 0;

cleanup:
	free_buffer_head(bh);
	put_extent(be);
	put_extent(cow_read);
	if (ret) {
		/* Need to mark layout with bad read...should now
		 * just use nfs4 for reads and writes.
		 */
		mark_bad_read();
	}
	return ret;
}

static int
bl_write_begin(struct pnfs_layout_segment *lseg, struct page *page, loff_t pos,
	       unsigned count, struct pnfs_fsdata *fsdata)
{
	unsigned from, to;
	int ret;
	sector_t *pages_to_mark = NULL;
	struct pnfs_block_layout *bl = BLK_LSEG2EXT(lseg);

	dprintk("%s enter, %u@%lld\n", __func__, count, pos);
	print_page(page);
	/* The following code assumes blocksize >= PAGE_CACHE_SIZE */
	if (bl->bl_blocksize < (PAGE_CACHE_SIZE >> 9)) {
		dprintk("%s Can't handle blocksize %llu\n", __func__,
			(u64)bl->bl_blocksize);
		fsdata->ok_to_use_pnfs = 0;
		return 0;
	}
	fsdata->ok_to_use_pnfs = 1;
	if (PageMappedToDisk(page)) {
		/* Basically, this is a flag that says we have
		 * successfully called write_begin already on this page.
		 */
		/* NOTE - there are cache consistency issues here.
		 * For example, what if the layout is recalled, then regained?
		 * If the file is closed and reopened, will the page flags
		 * be reset?  If not, we'll have to use layout info instead of
		 * the page flag.
		 */
		return 0;
	}
	from = pos & (PAGE_CACHE_SIZE - 1);
	to = from + count;
	ret = init_page_for_write(bl, page, from, to, &pages_to_mark);
	if (ret) {
		dprintk("%s init page failed with %i", __func__, ret);
		/* Revert back to plain NFS and just continue on with
		 * write.  This assumes there is no request attached, which
		 * should be true if we get here.
		 */
		BUG_ON(PagePrivate(page));
		fsdata->ok_to_use_pnfs = 0;
		kfree(pages_to_mark);
		ret = 0;
	} else {
		fsdata->private = pages_to_mark;
	}
	return ret;
}

/* CAREFUL - what happens if copied < count??? */
static int
bl_write_end(struct inode *inode, struct page *page, loff_t pos,
	     unsigned count, unsigned copied, struct pnfs_fsdata *fsdata)
{
	dprintk("%s enter, %u@%lld, %i\n", __func__, count, pos,
		fsdata ? fsdata->ok_to_use_pnfs : -1);
	print_page(page);
	if (fsdata) {
		if (fsdata->ok_to_use_pnfs) {
			dprintk("%s using pnfs\n", __func__);
			SetPageUptodate(page);
		}
	}
	return 0;
}

/* Return any memory allocated to fsdata->private, and take advantage
 * of no page locks to mark pages noted in write_begin as needing
 * initialization.
 */
static void
bl_write_end_cleanup(struct file *filp, struct pnfs_fsdata *fsdata)
{
	struct page *page;
	pgoff_t index;
	sector_t *pos;
	struct address_space *mapping = filp->f_mapping;
	struct pnfs_fsdata *fake_data;

	if (!fsdata)
		return;
	pos = fsdata->private;
	if (!pos)
		return;
	dprintk("%s enter with pos=%llu\n", __func__, (u64)(*pos));
	for (; *pos != ~0; pos++) {
		index = *pos >> (PAGE_CACHE_SHIFT - 9);
		/* XXX How do we properly deal with failures here??? */
		page = grab_cache_page_write_begin(mapping, index, 0);
		if (!page) {
			printk(KERN_ERR "%s BUG BUG BUG NoMem\n", __func__);
			continue;
		}
		dprintk("%s: Examining block page\n", __func__);
		print_page(page);
		if (!PageMappedToDisk(page)) {
			/* XXX How do we properly deal with failures here??? */
			dprintk("%s Marking block page\n", __func__);
			init_page_for_write(BLK_LSEG2EXT(fsdata->lseg), page,
					    PAGE_CACHE_SIZE, PAGE_CACHE_SIZE,
					    NULL);
			print_page(page);
			fake_data = kzalloc(sizeof(*fake_data), GFP_KERNEL);
			if (!fake_data) {
				printk(KERN_ERR "%s BUG BUG BUG NoMem\n",
				       __func__);
				unlock_page(page);
				continue;
			}
			fake_data->ok_to_use_pnfs = 1;
			fake_data->bypass_eof = 1;
			mapping->a_ops->write_end(filp, mapping,
						  index << PAGE_CACHE_SHIFT,
						  PAGE_CACHE_SIZE,
						  PAGE_CACHE_SIZE,
						  page, fake_data);
			/* Note fake_data is freed by nfs_write_end */
		} else
			unlock_page(page);
	}
	kfree(fsdata->private);
	fsdata->private = NULL;
}

static ssize_t
bl_get_stripesize(struct pnfs_layout_type *lo)
{
	dprintk("%s enter\n", __func__);
	return 0;
}

static ssize_t
bl_get_io_threshold(struct pnfs_layout_type *lo, struct inode *inode)
{
	dprintk("%s enter\n", __func__);
	return 0;
}

/* This is called by nfs_can_coalesce_requests via nfs_pageio_do_add_request.
 * Should return False if there is a reason requests can not be coalesced,
 * otherwise, should default to returning True.
 */
static int
bl_pg_test(struct nfs_pageio_descriptor *pgio, struct nfs_page *prev,
	   struct nfs_page *req)
{
	dprintk("%s enter\n", __func__);
	if (pgio->pg_iswrite) {
		return test_bit(PG_USE_PNFS, &prev->wb_flags) ==
			test_bit(PG_USE_PNFS, &req->wb_flags);
	} else {
		return 1;
	}
}

/* This checks if old req will likely use same io method as soon
 * to be created request, and returns False if they are the same.
 */
static int
bl_do_flush(struct pnfs_layout_segment *lseg, struct nfs_page *req,
	    struct pnfs_fsdata *fsdata)
{
	int will_try_pnfs;
	dprintk("%s enter\n", __func__);
	will_try_pnfs = fsdata ? (fsdata->ok_to_use_pnfs) : (lseg != NULL);
	return will_try_pnfs != test_bit(PG_USE_PNFS, &req->wb_flags);
}

static struct layoutdriver_io_operations blocklayout_io_operations = {
	.commit				= bl_commit,
	.read_pagelist			= bl_read_pagelist,
	.write_pagelist			= bl_write_pagelist,
	.write_begin			= bl_write_begin,
	.write_end			= bl_write_end,
	.write_end_cleanup		= bl_write_end_cleanup,
	.alloc_layout			= bl_alloc_layout,
	.free_layout			= bl_free_layout,
	.alloc_lseg			= bl_alloc_lseg,
	.free_lseg			= bl_free_lseg,
	.setup_layoutcommit		= bl_setup_layoutcommit,
	.cleanup_layoutcommit		= bl_cleanup_layoutcommit,
	.initialize_mountpoint		= bl_initialize_mountpoint,
	.uninitialize_mountpoint	= bl_uninitialize_mountpoint,
};

static struct layoutdriver_policy_operations blocklayout_policy_operations = {
	.get_stripesize			= bl_get_stripesize,
	.get_read_threshold		= bl_get_io_threshold,
	.get_write_threshold		= bl_get_io_threshold,
	.pg_test			= bl_pg_test,
	.do_flush			= bl_do_flush,
};

static struct pnfs_layoutdriver_type blocklayout_type = {
	.id = LAYOUT_BLOCK_VOLUME,
	.name = "LAYOUT_BLOCK_VOLUME",
	.ld_io_ops = &blocklayout_io_operations,
	.ld_policy_ops = &blocklayout_policy_operations,
};

static int __init nfs4blocklayout_init(void)
{
	dprintk("%s: NFSv4 Block Layout Driver Registering...\n", __func__);

	pnfs_callback_ops = pnfs_register_layoutdriver(&blocklayout_type);
	return 0;
}

static void __exit nfs4blocklayout_exit(void)
{
	dprintk("%s: NFSv4 Block Layout Driver Unregistering...\n",
	       __func__);

	pnfs_unregister_layoutdriver(&blocklayout_type);
}

module_init(nfs4blocklayout_init);
module_exit(nfs4blocklayout_exit);
