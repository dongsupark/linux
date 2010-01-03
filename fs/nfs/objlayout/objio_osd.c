/*
 *  objio_osd.c
 *
 *  pNFS Objects layout implementation over open-osd initiator library
 *
 *  Copyright (C) 2009 Panasas Inc.
 *  All rights reserved.
 *
 *  Benny Halevy <bharrosh@panasas.com>
 *  Boaz Harrosh <bharrosh@panasas.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  See the file COPYING included with this distribution for more details.
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
 *  3. Neither the name of the Panasas company nor the names of its
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

#include <linux/module.h>
#include <scsi/scsi_device.h>
#include <scsi/osd_attributes.h>
#include <scsi/osd_initiator.h>
#include <scsi/osd_sec.h>
#include <scsi/osd_sense.h>

#include "objlayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS

#define _LLU(x) ((unsigned long long)x)

enum { BIO_MAX_PAGES_KMALLOC =
		(PAGE_SIZE - sizeof(struct bio)) / sizeof(struct bio_vec),
};

/* A per mountpoint struct currently for device cache */
struct objio_mount_type {
	struct list_head dev_list;
	spinlock_t dev_list_lock;
};

struct _dev_ent {
	struct list_head list;
	struct pnfs_deviceid d_id;
	struct osd_dev *od;
};

static void _dev_list_remove_all(struct objio_mount_type *omt)
{
	spin_lock(&omt->dev_list_lock);

	while (!list_empty(&omt->dev_list)) {
		struct _dev_ent *de = list_entry(omt->dev_list.next,
				 struct _dev_ent, list);

		list_del_init(&de->list);
		osduld_put_device(de->od);
		kfree(de);
	}

	spin_unlock(&omt->dev_list_lock);
}

static struct osd_dev *___dev_list_find(struct objio_mount_type *omt,
	struct pnfs_deviceid *d_id)
{
	struct list_head *le;

	list_for_each(le, &omt->dev_list) {
		struct _dev_ent *de = list_entry(le, struct _dev_ent, list);

		if (0 == memcmp(&de->d_id, d_id, sizeof(*d_id)))
			return de->od;
	}

	return NULL;
}

static struct osd_dev *_dev_list_find(struct objio_mount_type *omt,
	struct pnfs_deviceid *d_id)
{
	struct osd_dev *od;

	spin_lock(&omt->dev_list_lock);
	od = ___dev_list_find(omt, d_id);
	spin_unlock(&omt->dev_list_lock);
	return od;
}

static int _dev_list_add(struct objio_mount_type *omt,
	struct pnfs_deviceid *d_id, struct osd_dev *od)
{
	struct _dev_ent *de = kzalloc(sizeof(*de), GFP_KERNEL);

	if (!de)
		return -ENOMEM;

	spin_lock(&omt->dev_list_lock);

	if (___dev_list_find(omt, d_id)) {
		kfree(de);
		goto out;
	}

	de->d_id = *d_id;
	de->od = od;
	list_add(&de->list, &omt->dev_list);

out:
	spin_unlock(&omt->dev_list_lock);
	return 0;
}

struct objio_segment {
	struct pnfs_osd_layout *layout;

	unsigned num_comps;
	/* variable length */
	struct osd_dev	*ods[1];
};

struct objio_state;
typedef ssize_t (*objio_done_fn)(struct objio_state *ios);

struct objio_state {
	/* Generic layer */
	struct objlayout_io_state ol_state;

	struct objio_segment *objio_seg;

	struct kref kref;
	objio_done_fn done;
	void *private;

	unsigned long length;
	unsigned numdevs; /* Actually used devs in this IO */
	/* A per-device variable array of size numdevs */
	struct _objio_per_comp {
		struct bio *bio;
		struct osd_request *or;
	} per_dev[];
};

/* Send and wait for a get_device_info of devices in the layout,
   then look them up with the osd_initiator library */
static struct osd_dev *_device_lookup(struct pnfs_layout_type *pnfslay,
			       struct objio_segment *objio_seg, unsigned comp)
{
	struct pnfs_osd_layout *layout = objio_seg->layout;
	struct pnfs_osd_deviceaddr *deviceaddr;
	struct pnfs_deviceid *d_id;
	struct osd_dev *od;
	struct osd_dev_info odi;
	struct objio_mount_type *omt = PNFS_MOUNTID(pnfslay)->mountid;
	int err;

	d_id = &layout->olo_comps[comp].oc_object_id.oid_device_id;

	od = _dev_list_find(omt, d_id);
	if (od)
		return od;

	err = objlayout_get_deviceinfo(pnfslay, d_id, &deviceaddr);
	if (unlikely(err)) {
		dprintk("%s: objlayout_get_deviceinfo=>%d\n", __func__, err);
		return ERR_PTR(err);
	}

	odi.systemid_len = deviceaddr->oda_systemid.len;
	if (odi.systemid_len > sizeof(odi.systemid)) {
		err = -EINVAL;
		goto out;
	} else if (odi.systemid_len)
		memcpy(odi.systemid, deviceaddr->oda_systemid.data,
		       odi.systemid_len);
	odi.osdname_len	 = deviceaddr->oda_osdname.len;
	odi.osdname	 = (u8 *)deviceaddr->oda_osdname.data;

	if (!odi.osdname_len && !odi.systemid_len) {
		err = -ENODEV;
		goto out;
	}

	od = osduld_info_lookup(&odi);
	if (unlikely(IS_ERR(od))) {
		err = PTR_ERR(od);
		goto out;
	}

	_dev_list_add(omt, d_id, od);

out:
	dprintk("%s: return=%d\n", __func__, err);
	objlayout_put_deviceinfo(deviceaddr);
	return err ? ERR_PTR(err) : od;
}

static int objio_devices_lookup(struct pnfs_layout_type *pnfslay,
	struct objio_segment *objio_seg)
{
	struct pnfs_osd_layout *layout = objio_seg->layout;
	unsigned i, num_comps = layout->olo_num_comps;
	int err;

	/* lookup all devices */
	for (i = 0; i < num_comps; i++) {
		struct osd_dev *od;

		od = _device_lookup(pnfslay, objio_seg, i);
		if (unlikely(IS_ERR(od))) {
			err = PTR_ERR(od);
			goto out;
		}
		objio_seg->ods[i] = od;
	}
	objio_seg->num_comps = num_comps;
	err = 0;

out:
	dprintk("%s: return=%d\n", __func__, err);
	return err;
}

static int _verify_data_map(struct pnfs_osd_layout *layout)
{
	struct pnfs_osd_data_map *data_map = &layout->olo_map;

/* FIXME: Only Mirror arangment for now. if not so, do not mount */
	if (data_map->odm_group_width || data_map->odm_group_depth) {
		printk(KERN_ERR "Group width/depth not supported\n");
		return -ENOTSUPP;
	}
	if (data_map->odm_num_comps != layout->olo_num_comps) {
		printk(KERN_ERR "odm_num_comps(%u) != olo_num_comps(%u)\n",
			  data_map->odm_num_comps, layout->olo_num_comps);
		return -ENOTSUPP;
	}
	if (data_map->odm_raid_algorithm != PNFS_OSD_RAID_0) {
		printk(KERN_ERR "Only RAID_0 for now\n");
		return -ENOTSUPP;
	}
	if (data_map->odm_num_comps != data_map->odm_mirror_cnt + 1) {
		printk(KERN_ERR "Mirror only!, num_comps=%u mirrors=%u\n",
			  data_map->odm_num_comps, data_map->odm_mirror_cnt);
		return -ENOTSUPP;
	}

	if (data_map->odm_stripe_unit != PAGE_SIZE) {
		printk(KERN_ERR "Stripe Unit != PAGE_SIZE not supported\n");
		return -ENOTSUPP;
	}

	return 0;
}

int objio_alloc_lseg(void **outp,
	struct pnfs_layout_type *pnfslay,
	struct pnfs_layout_segment *lseg,
	struct pnfs_osd_layout *layout)
{
	struct objio_segment *objio_seg;
	int err;

	err = _verify_data_map(layout);
	if (unlikely(err))
		return err;

	objio_seg = kzalloc(sizeof(*objio_seg) +
			(layout->olo_num_comps - 1) * sizeof(objio_seg->ods[0]),
			GFP_KERNEL);
	if (!objio_seg)
		return -ENOMEM;

	objio_seg->layout = layout;
	err = objio_devices_lookup(pnfslay, objio_seg);
	if (err)
		goto free_seg;

	*outp = objio_seg;
	return 0;

free_seg:
	dprintk("%s: Error: return %d\n", __func__, err);
	kfree(objio_seg);
	*outp = NULL;
	return err;
}

void objio_free_lseg(void *p)
{
	struct objio_segment *objio_seg = p;

	kfree(objio_seg);
}

int objio_alloc_io_state(void *seg, struct objlayout_io_state **outp)
{
	struct objio_segment *objio_seg = seg;
	struct objio_state *ios;
	const unsigned first_size = sizeof(*ios) +
				objio_seg->num_comps * sizeof(ios->per_dev[0]);
	const unsigned sec_size = objio_seg->num_comps *
						sizeof(ios->ol_state.ioerrs[0]);

	dprintk("%s: num_comps=%d\n", __func__, objio_seg->num_comps);
	ios = kzalloc(first_size + sec_size, GFP_KERNEL);
	if (unlikely(!ios))
		return -ENOMEM;

	ios->objio_seg = objio_seg;
	ios->ol_state.ioerrs = ((void *)ios) + first_size;
	ios->ol_state.num_comps = objio_seg->num_comps;

	*outp = &ios->ol_state;
	return 0;
}

void objio_free_io_state(struct objlayout_io_state *ol_state)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);

	kfree(ios);
}

enum pnfs_osd_errno osd_pri_2_pnfs_err(enum osd_err_priority oep)
{
	switch (oep) {
	case OSD_ERR_PRI_NO_ERROR:
		return (enum pnfs_osd_errno)0;

	case OSD_ERR_PRI_CLEAR_PAGES:
		BUG_ON(1);
		return 0;

	case OSD_ERR_PRI_RESOURCE:
		return PNFS_OSD_ERR_RESOURCE;
	case OSD_ERR_PRI_BAD_CRED:
		return PNFS_OSD_ERR_BAD_CRED;
	case OSD_ERR_PRI_NO_ACCESS:
		return PNFS_OSD_ERR_NO_ACCESS;
	case OSD_ERR_PRI_UNREACHABLE:
		return PNFS_OSD_ERR_UNREACHABLE;
	case OSD_ERR_PRI_NOT_FOUND:
		return PNFS_OSD_ERR_NOT_FOUND;
	case OSD_ERR_PRI_NO_SPACE:
		return PNFS_OSD_ERR_NO_SPACE;
	default:
		WARN_ON(1);
		/* fallthrough */
	case OSD_ERR_PRI_EIO:
		return PNFS_OSD_ERR_EIO;
	}
}

static void _clear_bio(struct bio *bio)
{
	struct bio_vec *bv;
	unsigned i;

	__bio_for_each_segment(bv, bio, i, 0) {
		unsigned this_count = bv->bv_len;

		if (likely(PAGE_SIZE == this_count))
			clear_highpage(bv->bv_page);
		else
			zero_user(bv->bv_page, bv->bv_offset, this_count);
	}
}

static int _io_check(struct objio_state *ios, bool is_write)
{
	enum osd_err_priority oep = OSD_ERR_PRI_NO_ERROR;
	int lin_ret = 0;
	int i;

	for (i = 0; i <  ios->numdevs; i++) {
		struct osd_sense_info osi;
		struct osd_request *or = ios->per_dev[i].or;
		int ret;

		if (!or)
			continue;

		ret = osd_req_decode_sense(or, &osi);
		if (likely(!ret))
			continue;

		if (OSD_ERR_PRI_CLEAR_PAGES == osi.osd_err_pri) {
			/* start read offset passed endof file */
			BUG_ON(is_write);
			_clear_bio(ios->per_dev[i].bio);
			dprintk("%s: start read offset passed end of file "
				"offset=0x%llx, length=0x%lx\n", __func__,
				_LLU(ios->ol_state.offset), ios->length);

			continue; /* we recovered */
		}
		objlayout_io_set_result(&ios->ol_state, i,
					osd_pri_2_pnfs_err(osi.osd_err_pri),
					ios->ol_state.offset, ios->length,
					is_write);

		if (osi.osd_err_pri >= oep) {
			oep = osi.osd_err_pri;
			lin_ret = ret;
		}
	}

	return lin_ret;
}

/*
 * Common IO state helpers.
 */
static void _io_free(struct objio_state *ios)
{
	unsigned i;

	for (i = 0; i < ios->numdevs; i++) {
		struct _objio_per_comp *per_dev = &ios->per_dev[i];

		if (per_dev->or) {
			osd_end_request(per_dev->or);
			per_dev->or = NULL;
		}

		if (per_dev->bio) {
			bio_put(per_dev->bio);
			per_dev->bio = NULL;
		}
	}
}

static int _io_rw_pagelist(struct objio_state *ios)
{
	u64 length = ios->ol_state.count;
	unsigned pgbase = ios->ol_state.pgbase;
	unsigned nr_pages = ios->ol_state.nr_pages;
	struct page **pages = ios->ol_state.pages;
	struct bio *master_bio;
	unsigned bio_size = min_t(unsigned, nr_pages, BIO_MAX_PAGES_KMALLOC);

	master_bio = bio_kmalloc(GFP_KERNEL, bio_size);
	if (unlikely(!master_bio)) {
		dprintk("%s: Faild to alloc bio pages=%d\n",
			__func__, bio_size);
		return -ENOMEM;
	}

	ios->per_dev[0].bio = master_bio;

	while (length) {
		unsigned cur_len, added_len;

		cur_len = min_t(u64, length, PAGE_SIZE - pgbase);

		added_len = bio_add_pc_page(
			osd_request_queue(ios->objio_seg->ods[0]),
			master_bio, *pages, cur_len, pgbase);
		if (unlikely(cur_len != added_len))
			break;

		pgbase = 0;
		++pages;
		length -= cur_len;
		ios->length += cur_len;
	}

	/* this should never happen */
	WARN_ON(!ios->length);

	return 0;
}

static ssize_t _sync_done(struct objio_state *ios)
{
	struct completion *waiting = ios->private;

	complete(waiting);
	return 0;
}

static void _last_io(struct kref *kref)
{
	struct objio_state *ios = container_of(kref, struct objio_state, kref);

	ios->done(ios);
}

static void _done_io(struct osd_request *or, void *p)
{
	struct objio_state *ios = p;

	kref_put(&ios->kref, _last_io);
}

static ssize_t _io_exec(struct objio_state *ios)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	ssize_t status = 0; /* sync status */
	unsigned i;
	objio_done_fn saved_done_fn = ios->done;
	bool sync = ios->ol_state.sync;

	if (sync) {
		ios->done = _sync_done;
		ios->private = &wait;
	}

	kref_init(&ios->kref);

	for (i = 0; i < ios->numdevs; i++) {
		struct osd_request *or = ios->per_dev[i].or;

		if (!or)
			continue;

		kref_get(&ios->kref);
		osd_execute_request_async(or, _done_io, ios);
	}

	kref_put(&ios->kref, _last_io);

	if (sync) {
		wait_for_completion(&wait);
		status = saved_done_fn(ios);
	}

	return status;
}

/*
 * read
 */
static ssize_t _read_done(struct objio_state *ios)
{
	ssize_t status;
	int ret = _io_check(ios, false);

	_io_free(ios);

	if (likely(!ret))
		status = ios->length;
	else
		status = ret;

	objlayout_read_done(&ios->ol_state, status, ios->ol_state.sync);
	return status;
}

static ssize_t _read_exec(struct objio_state *ios)
{
	struct osd_request *or = NULL;
	struct _objio_per_comp *per_dev = &ios->per_dev[0];
	unsigned dev = 0;
	struct pnfs_osd_object_cred *cred =
			&ios->objio_seg->layout->olo_comps[dev];
	struct osd_obj_id obj = {
		.partition = cred->oc_object_id.oid_partition_id,
		.id = cred->oc_object_id.oid_object_id,
	};
	int ret;

	or = osd_start_request(ios->objio_seg->ods[dev], GFP_KERNEL);
	if (unlikely(!or)) {
		ret = -ENOMEM;
		goto err;
	}
	per_dev->or = or;
	ios->numdevs++;

	osd_req_read(or, &obj, ios->ol_state.offset, per_dev->bio, ios->length);

	ret = osd_finalize_request(or, 0, cred->oc_cap.cred, NULL);
	if (ret) {
		dprintk("%s: Faild to osd_finalize_request() => %d\n",
			__func__, ret);
		goto err;
	}

	dprintk("%s: obj=0x%llx start=0x%llx length=0x%lx\n",
		__func__, obj.id, _LLU(ios->ol_state.offset), ios->length);
	ios->done = _read_done;
	return _io_exec(ios); /* In sync mode exec returns the io status */

err:
	_io_free(ios);
	return ret;
}

ssize_t objio_read_pagelist(struct objlayout_io_state *ol_state)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);
	int ret;

	ret = _io_rw_pagelist(ios);
	if (unlikely(ret))
		return ret;

	return _read_exec(ios);
}

/*
 * write
 */
static ssize_t _write_done(struct objio_state *ios)
{
	ssize_t status;
	int ret = _io_check(ios, true);

	_io_free(ios);

	if (likely(!ret)) {
		/* FIXME: should be based on the OSD's persistence model
		 * See OSD2r05 Section 4.13 Data persistence model */
		ios->ol_state.committed = NFS_FILE_SYNC;
		status = ios->length;
	} else {
		status = ret;
	}

	objlayout_write_done(&ios->ol_state, status, ios->ol_state.sync);
	return status;
}

static int _write_exec(struct objio_state *ios)
{
	int i, ret;
	struct bio *master_bio = ios->per_dev[0].bio;

	for (i = 0; i < ios->objio_seg->num_comps; i++) {
		struct osd_request *or = NULL;
		struct pnfs_osd_object_cred *cred =
					&ios->objio_seg->layout->olo_comps[i];
		struct osd_obj_id obj = {cred->oc_object_id.oid_partition_id,
					 cred->oc_object_id.oid_object_id};
		struct _objio_per_comp *per_dev = &ios->per_dev[i];
		struct bio *bio;

		or = osd_start_request(ios->objio_seg->ods[i], GFP_KERNEL);
		if (unlikely(!or)) {
			ret = -ENOMEM;
			goto err;
		}
		per_dev->or = or;
		ios->numdevs++;

		if (i != 0) {
			bio = bio_kmalloc(GFP_KERNEL, master_bio->bi_max_vecs);
			if (unlikely(!bio)) {
				dprintk("Faild to allocate BIO size=%u\n",
					master_bio->bi_max_vecs);
				ret = -ENOMEM;
				goto err;
			}

			__bio_clone(bio, master_bio);
			bio->bi_bdev = NULL;
			bio->bi_next = NULL;
			per_dev->bio = bio;
		} else {
			/* FIXME: bio_set_dir() */
			master_bio->bi_rw |= (1 << BIO_RW);
			bio = master_bio;
		}

		osd_req_write(or, &obj, ios->ol_state.offset, bio, ios->length);

		ret = osd_finalize_request(or, 0, cred->oc_cap.cred, NULL);
		if (ret) {
			dprintk("%s: Faild to osd_finalize_request() => %d\n",
				__func__, ret);
			goto err;
		}

		dprintk("%s: [%d] obj=0x%llx start=0x%llx length=0x%lx\n",
			__func__, i, obj.id, _LLU(ios->ol_state.offset),
			ios->length);
	}

	ios->done = _write_done;
	return _io_exec(ios); /* In sync mode exec returns the io->status */

err:
	_io_free(ios);
	return ret;
}

ssize_t objio_write_pagelist(struct objlayout_io_state *ol_state, bool stable)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);
	int ret;

	/* TODO: ios->stable = stable; */
	ret = _io_rw_pagelist(ios);
	if (unlikely(ret))
		return ret;

	return _write_exec(ios);
}

/*
 * Policy Operations
 */

/*
 * Return the stripe size for the specified file
 */
ssize_t
objlayout_get_stripesize(struct pnfs_layout_type *pnfslay)
{
	ssize_t sz, maxsz = -1;
	struct pnfs_layout_segment *lseg;

	list_for_each_entry(lseg, &pnfslay->segs, fi_list) {
		int n;
		struct objlayout_segment *objlseg = LSEG_LD_DATA(lseg);
		struct pnfs_osd_layout *lo =
			(struct pnfs_osd_layout *)objlseg->pnfs_osd_layout;
		struct pnfs_osd_data_map *map = &lo->olo_map;

		n = map->odm_group_width;
		if (n == 0)
			n = map->odm_num_comps / (map->odm_mirror_cnt + 1);

		switch (map->odm_raid_algorithm) {
		case PNFS_OSD_RAID_0:
			break;

		case PNFS_OSD_RAID_4:
		case PNFS_OSD_RAID_5:
			n -= 1;
			break;

		case PNFS_OSD_RAID_PQ:
			n -= 2;
			break;

		default:
			BUG_ON(1);
		}
		sz = map->odm_stripe_unit * n;
		if (sz > maxsz)
			maxsz = sz;
	}
	dprintk("%s: Return %Zx\n", __func__, maxsz);
	return maxsz;
}

/*
 * Get the max [rw]size
 */
static ssize_t
objlayout_get_blocksize(struct pnfs_mount_type *mountid)
{
	ssize_t sz = BIO_MAX_PAGES_KMALLOC * PAGE_SIZE;

	return sz;
}

/*
 * Get the I/O threshold
 */
static ssize_t
objlayout_get_io_threshold(struct pnfs_layout_type *layoutid,
			   struct inode *inode)
{
	ssize_t sz = -1;
	return sz;
}

static struct layoutdriver_policy_operations objlayout_policy_operations = {
/*
 * Don't gather across stripes, but rather gather (coalesce) up to
 * the stripe size.
 *
 * FIXME: change interface to use merge_align, merge_count
 */
	.flags                 = PNFS_LAYOUTGET_ON_OPEN |
				 PNFS_LAYOUTRET_ON_SETATTR,
	.get_stripesize        = objlayout_get_stripesize,
	.get_blocksize         = objlayout_get_blocksize,
	.get_read_threshold    = objlayout_get_io_threshold,
	.get_write_threshold   = objlayout_get_io_threshold,
};

static struct pnfs_layoutdriver_type objlayout_type = {
	.id = LAYOUT_OSD2_OBJECTS,
	.name = "LAYOUT_OSD2_OBJECTS",
	.ld_io_ops = &objlayout_io_operations,
	.ld_policy_ops = &objlayout_policy_operations,
};

void *objio_init_mt(void)
{
	struct objio_mount_type *omt = kzalloc(sizeof(*omt), GFP_KERNEL);

	if (!omt)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&omt->dev_list);
	spin_lock_init(&omt->dev_list_lock);
	return omt;
}

void objio_fini_mt(void *mountid)
{
	_dev_list_remove_all(mountid);
	kfree(mountid);
}

MODULE_DESCRIPTION("pNFS Layout Driver for OSD2 objects");
MODULE_AUTHOR("Benny Halevy <bhalevy@panasas.com>");
MODULE_LICENSE("GPL");

static int __init
objlayout_init(void)
{
	pnfs_client_ops = pnfs_register_layoutdriver(&objlayout_type);
	printk(KERN_INFO "%s: Registered OSD pNFS Layout Driver\n",
	       __func__);
	return 0;
}

static void __exit
objlayout_exit(void)
{
	pnfs_unregister_layoutdriver(&objlayout_type);
	printk(KERN_INFO "%s: Unregistered OSD pNFS Layout Driver\n",
	       __func__);
}

module_init(objlayout_init);
module_exit(objlayout_exit);
