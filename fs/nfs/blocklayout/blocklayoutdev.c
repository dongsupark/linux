/*
 *  linux/fs/nfs/blocklayout/blocklayoutdev.c
 *
 *  Device operations for the pnfs nfs4 file layout driver.
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
#include <linux/buffer_head.h> /* __bread */

#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hash.h>

#include "blocklayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

uint32_t *blk_overflow(uint32_t *p, uint32_t *end, size_t nbytes)
{
	uint32_t *q = p + XDR_QUADLEN(nbytes);
	if (unlikely(q > end || q < p))
		return NULL;
	return p;
}
EXPORT_SYMBOL(blk_overflow);

/* Open a block_device by device number. */
struct block_device *nfs4_blkdev_get(dev_t dev)
{
	struct block_device *bd;

	dprintk("%s enter\n", __func__);
	bd = open_by_devnum(dev, FMODE_READ);
	if (IS_ERR(bd))
		goto fail;
	return bd;
fail:
	dprintk("%s failed to open device : %ld\n",
			__func__, PTR_ERR(bd));
	return NULL;
}

/*
 * Release the block device
 */
int nfs4_blkdev_put(struct block_device *bdev)
{
	dprintk("%s for device %d:%d\n", __func__, MAJOR(bdev->bd_dev),
			MINOR(bdev->bd_dev));
	bd_release(bdev);
	return blkdev_put(bdev, FMODE_READ);
}

/* Decodes pnfs_block_deviceaddr4 (draft-8) which is XDR encoded
 * in dev->dev_addr_buf.
 */
struct pnfs_block_dev *
nfs4_blk_decode_device(struct nfs_server *server,
		       struct pnfs_device *dev,
		       struct list_head *sdlist)
{
	struct pnfs_block_dev *rv = NULL;
	struct block_device *bd = NULL;
	struct pipefs_hdr *msg = NULL, *reply = NULL;
	uint32_t major, minor;

	dprintk("%s enter\n", __func__);

	if (IS_ERR(bl_device_pipe))
		return NULL;
	dprintk("%s CREATING PIPEFS MESSAGE\n", __func__);
	dprintk("%s: deviceid: %s, mincount: %d\n", __func__, dev->dev_id.data,
		dev->mincount);
	msg = pipefs_alloc_init_msg(0, BL_DEVICE_MOUNT, 0, dev->area,
				    dev->mincount);
	if (IS_ERR(msg)) {
		dprintk("ERROR: couldn't make pipefs message.\n");
		goto out_err;
	}
	msg->msgid = hash_ptr(&msg, sizeof(msg->msgid) * 8);
	msg->status = BL_DEVICE_REQUEST_INIT;

	dprintk("%s CALLING USERSPACE DAEMON\n", __func__);
	reply = pipefs_queue_upcall_waitreply(bl_device_pipe, msg,
					      &bl_device_list, 0, 0);

	if (IS_ERR(reply)) {
		dprintk("ERROR: upcall_waitreply failed\n");
		goto out_err;
	}
	if (reply->status != BL_DEVICE_REQUEST_PROC) {
		dprintk("%s failed to open device: %ld\n",
			__func__, PTR_ERR(bd));
		goto out_err;
	}
	memcpy(&major, (uint32_t *)(payload_of(reply)), sizeof(uint32_t));
	memcpy(&minor, (uint32_t *)(payload_of(reply) + sizeof(uint32_t)),
		sizeof(uint32_t));
	bd = nfs4_blkdev_get(MKDEV(major, minor));
	if (IS_ERR(bd)) {
		dprintk("%s failed to open device : %ld\n",
			__func__, PTR_ERR(bd));
		goto out_err;
	}

	rv = kzalloc(sizeof(*rv), GFP_KERNEL);
	if (!rv)
		goto out_err;

	rv->bm_mdev = bd;
	memcpy(&rv->bm_mdevid, &dev->dev_id, sizeof(struct nfs4_deviceid));
	dprintk("%s Created device %s with bd_block_size %u\n",
		__func__,
		bd->bd_disk->disk_name,
		bd->bd_block_size);
	kfree(reply);
	kfree(msg);
	return rv;

out_err:
	kfree(rv);
	if (!IS_ERR(reply))
		kfree(reply);
	if (!IS_ERR(msg))
		kfree(msg);
	return NULL;
}

/* Map deviceid returned by the server to constructed block_device */
static struct block_device *translate_devid(struct pnfs_layout_hdr *lo,
					    struct nfs4_deviceid *id)
{
	struct block_device *rv = NULL;
	struct block_mount_id *mid;
	struct pnfs_block_dev *dev;

	dprintk("%s enter, lo=%p, id=%p\n", __func__, lo, id);
	mid = BLK_ID(lo);
	spin_lock(&mid->bm_lock);
	list_for_each_entry(dev, &mid->bm_devlist, bm_node) {
		if (memcmp(id->data, dev->bm_mdevid.data,
			   NFS4_DEVICEID4_SIZE) == 0) {
			rv = dev->bm_mdev;
			goto out;
		}
	}
 out:
	spin_unlock(&mid->bm_lock);
	dprintk("%s returning %p\n", __func__, rv);
	return rv;
}

/* Tracks info needed to ensure extents in layout obey constraints of spec */
struct layout_verification {
	u32 mode;	/* R or RW */
	u64 start;	/* Expected start of next non-COW extent */
	u64 inval;	/* Start of INVAL coverage */
	u64 cowread;	/* End of COW read coverage */
};

/* Verify the extent meets the layout requirements of the pnfs-block draft,
 * section 2.3.1.
 */
static int verify_extent(struct pnfs_block_extent *be,
			 struct layout_verification *lv)
{
	if (lv->mode == IOMODE_READ) {
		if (be->be_state == PNFS_BLOCK_READWRITE_DATA ||
		    be->be_state == PNFS_BLOCK_INVALID_DATA)
			return -EIO;
		if (be->be_f_offset != lv->start)
			return -EIO;
		lv->start += be->be_length;
		return 0;
	}
	/* lv->mode == IOMODE_RW */
	if (be->be_state == PNFS_BLOCK_READWRITE_DATA) {
		if (be->be_f_offset != lv->start)
			return -EIO;
		if (lv->cowread > lv->start)
			return -EIO;
		lv->start += be->be_length;
		lv->inval = lv->start;
		return 0;
	} else if (be->be_state == PNFS_BLOCK_INVALID_DATA) {
		if (be->be_f_offset != lv->start)
			return -EIO;
		lv->start += be->be_length;
		return 0;
	} else if (be->be_state == PNFS_BLOCK_READ_DATA) {
		if (be->be_f_offset > lv->start)
			return -EIO;
		if (be->be_f_offset < lv->inval)
			return -EIO;
		if (be->be_f_offset < lv->cowread)
			return -EIO;
		/* It looks like you might want to min this with lv->start,
		 * but you really don't.
		 */
		lv->inval = lv->inval + be->be_length;
		lv->cowread = be->be_f_offset + be->be_length;
		return 0;
	} else
		return -EIO;
}

/* XDR decode pnfs_block_layout4 structure */
int
nfs4_blk_process_layoutget(struct pnfs_layout_hdr *lo,
			   struct nfs4_layoutget_res *lgr)
{
	struct pnfs_block_layout *bl = BLK_LO2EXT(lo);
	uint32_t *p = (uint32_t *)lgr->layout.buf;
	uint32_t *end = (uint32_t *)((char *)lgr->layout.buf + lgr->layout.len);
	int i, status = -EIO;
	uint32_t count;
	struct pnfs_block_extent *be = NULL, *save;
	uint64_t tmp; /* Used by READSECTOR */
	struct layout_verification lv = {
		.mode = lgr->range.iomode,
		.start = lgr->range.offset >> 9,
		.inval = lgr->range.offset >> 9,
		.cowread = lgr->range.offset >> 9,
	};

	LIST_HEAD(extents);

	BLK_READBUF(p, end, 4);
	READ32(count);

	dprintk("%s enter, number of extents %i\n", __func__, count);
	BLK_READBUF(p, end, (28 + NFS4_DEVICEID4_SIZE) * count);

	/* Decode individual extents, putting them in temporary
	 * staging area until whole layout is decoded to make error
	 * recovery easier.
	 */
	for (i = 0; i < count; i++) {
		be = alloc_extent();
		if (!be) {
			status = -ENOMEM;
			goto out_err;
		}
		READ_DEVID(&be->be_devid);
		be->be_mdev = translate_devid(lo, &be->be_devid);
		if (!be->be_mdev)
			goto out_err;
		/* The next three values are read in as bytes,
		 * but stored as 512-byte sector lengths
		 */
		READ_SECTOR(be->be_f_offset);
		READ_SECTOR(be->be_length);
		READ_SECTOR(be->be_v_offset);
		READ32(be->be_state);
		if (be->be_state == PNFS_BLOCK_INVALID_DATA)
			be->be_inval = &bl->bl_inval;
		if (verify_extent(be, &lv)) {
			dprintk("%s verify failed\n", __func__);
			goto out_err;
		}
		list_add_tail(&be->be_node, &extents);
	}
	if (p != end) {
		dprintk("%s Undecoded cruft at end of opaque\n", __func__);
		be = NULL;
		goto out_err;
	}
	if (lgr->range.offset + lgr->range.length != lv.start << 9) {
		dprintk("%s Final length mismatch\n", __func__);
		be = NULL;
		goto out_err;
	}
	if (lv.start < lv.cowread) {
		dprintk("%s Final uncovered COW extent\n", __func__);
		be = NULL;
		goto out_err;
	}
	/* Extents decoded properly, now try to merge them in to
	 * existing layout extents.
	 */
	spin_lock(&bl->bl_ext_lock);
	list_for_each_entry_safe(be, save, &extents, be_node) {
		list_del(&be->be_node);
		status = add_and_merge_extent(bl, be);
		if (status) {
			spin_unlock(&bl->bl_ext_lock);
			/* This is a fairly catastrophic error, as the
			 * entire layout extent lists are now corrupted.
			 * We should have some way to distinguish this.
			 */
			be = NULL;
			goto out_err;
		}
	}
	spin_unlock(&bl->bl_ext_lock);
	status = 0;
 out:
	dprintk("%s returns %i\n", __func__, status);
	return status;

 out_err:
	put_extent(be);
	while (!list_empty(&extents)) {
		be = list_first_entry(&extents, struct pnfs_block_extent,
				      be_node);
		list_del(&be->be_node);
		put_extent(be);
	}
	goto out;
}
