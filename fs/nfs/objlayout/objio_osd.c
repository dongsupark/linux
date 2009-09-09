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

	/* TODO: per device info (keep in an array) */
	struct osd_dev	*od;
};

struct objio_state {
	struct objio_segment *objio_seg;

	/* TODO: per device info (will be an array of) */
	unsigned expected_pages;
	struct bio *bio;
	unsigned long length;

	/* Generic layer (variable length, keep last) */
	struct objlayout_io_state ol_state;
};

/* Send and wait for a get_device_info of devices in the layout,
   then look them up with the osd_initiator library */
static int objio_devices_lookup(struct pnfs_layout_type *pnfslay,
	struct objio_segment *objio_seg)
{
	struct pnfs_osd_layout *layout = objio_seg->layout;
	struct pnfs_osd_deviceaddr *deviceaddr;
	struct pnfs_deviceid *d_id;
	struct osd_dev_info odi;
	struct objio_mount_type *omt = PNFS_MOUNTID(pnfslay)->mountid;
	int err;

	/* TODO: for_all layout->olo_comps[i] */
	d_id = &layout->olo_comps[0].oc_object_id.oid_device_id;

	objio_seg->od = _dev_list_find(omt, d_id);
	if (objio_seg->od)
		return 0;

	err = objlayout_get_deviceinfo(pnfslay, d_id, &deviceaddr);
	if (err)
		return err;

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

	objio_seg->od = osduld_info_lookup(&odi);
	if (IS_ERR(objio_seg->od)) {
		err = PTR_ERR(objio_seg->od);
		goto out;
	}

	_dev_list_add(omt, d_id, objio_seg->od);

out:
	dprintk("%s: return=%d\n", __func__, err);
	objlayout_put_deviceinfo(deviceaddr);
	return err;
}

int objio_alloc_lseg(void **outp,
	struct pnfs_layout_type *pnfslay,
	struct pnfs_layout_segment *lseg,
	struct pnfs_osd_layout *layout)
{
	struct objio_segment *objio_seg;
	int err;

	objio_seg = kzalloc(sizeof(*objio_seg), GFP_KERNEL);
	if (!objio_seg)
		return -ENOMEM;

	objio_seg->layout = layout;
	err = objio_devices_lookup(pnfslay, objio_seg);
	if (err)
		goto free_seg;

	*outp = objio_seg;
	return 0;

free_seg:
	kfree(objio_seg);
	*outp = NULL;
	return err;
}

void objio_free_lseg(void *p)
{
	struct objio_segment *objio_seg = p;

	kfree(objio_seg);
}

int objio_alloc_io_state(struct objlayout_io_state **outp)
{
	struct objio_state *ios;

	ios = kzalloc(sizeof(*ios), GFP_KERNEL);
	if (unlikely(!ios))
		return -ENOMEM;

	*outp = &ios->ol_state;
	return 0;
}

void objio_free_io_state(struct objlayout_io_state *ol_state)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);

	kfree(ios);
}

static bool _is_osd_security_code(int code)
{
	return	(code == osd_security_audit_value_frozen) ||
		(code == osd_security_working_key_frozen) ||
		(code == osd_nonce_not_unique) ||
		(code == osd_nonce_timestamp_out_of_range) ||
		(code == osd_invalid_dataout_buffer_integrity_check_value);
}

/*
 * _check_io_resid()
 *     Insures consistent results from IO operations. Translate osd
 *     error codes to Linux codes.
 * At input:
 *    *@in_resid and/or *@out_resid contain full requested IO length.
 * At output:
 *    1. If no error @in_resid/@out_resid == Zero.
 *    2. On Error: 0 < resid <= total_length (see input)
 */
static int _check_io_resid(struct osd_request *or,
	u64 *in_resid, u64 *out_resid)
{
	struct osd_sense_info osi = {.key = 0};  /* FIXME: bug in libosd */
	int ret = osd_req_decode_sense(or, &osi);

	if (likely(!ret)) {
		if (in_resid)
			*in_resid = 0;
		if (out_resid)
			*out_resid = 0;

		return 0;
	}

	if (osi.additional_code == scsi_invalid_field_in_cdb) {
		if (osi.cdb_field_offset == OSD_CFO_OBJECT_ID) {
			ret = -ENOENT;
		} else if (osi.cdb_field_offset == OSD_CFO_PERMISSIONS) {
			ret = -EACCES;
		} else {
			ret = -EINVAL;
		}
	} else if (osi.additional_code == osd_quota_error) {
		ret = -ENOSPC;
	} else if (_is_osd_security_code(osi.additional_code)) {
		ret = -EINVAL;
	} else if (!osi.key) {
		ret = or->async_error;
	} else {
		ret = -EIO;
	}

	if (in_resid) {
		unsigned resid = or->in.req ? or->in.req->resid_len : 0;

		if (resid)
			*in_resid = resid;
	}

	if (out_resid) {
		unsigned resid = or->out.req ? or->out.req->resid_len : 0;

		if (resid)
			*out_resid = resid;
	}

	return ret;
}

/*
 * Perform a synchronous OSD operation.
 */
static int _sync_op(struct osd_request *or, uint8_t *credential)
{
	int ret;

	ret = osd_finalize_request(or, 0, credential, NULL);
	if (ret) {
		dprintk("Faild to osd_finalize_request() => %d\n", ret);
		return ret;
	}

	ret = osd_execute_request(or);

	if (ret)
		dprintk("osd_execute_request() => %d\n", ret);
	/* osd_req_decode_sense(or, ret); */
	return ret;
}

/*
 * Perform an asynchronous OSD operation.
 */
static int _async_op(struct osd_request *or, osd_req_done_fn *async_done,
		   void *caller_context, u8 *cred)
{
	int ret;

	ret = osd_finalize_request(or, 0, cred, NULL);
	if (ret) {
		dprintk("Faild to osd_finalize_request() => %d\n", ret);
		return ret;
	}

	ret = osd_execute_request_async(or, async_done, caller_context);

	if (ret)
		dprintk("osd_execute_request_async() => %d\n", ret);
	return ret;
}


/*
 * Common IO state helpers.
 */
static int _io_try_alloc(struct objio_state *ios, bool do_write)
{
	int pages = min_t(unsigned, ios->expected_pages, BIO_MAX_PAGES_KMALLOC);

	for (; pages; pages >>= 1) {
		ios->bio = bio_kmalloc(GFP_KERNEL, pages);
		if (likely(ios->bio)) {
			if (do_write) {
				/* FIXME: bio_set_dir() */
				ios->bio->bi_rw |= (1 << BIO_RW);
			}
			return 0;
		}
	}

	return -ENOMEM;
}

static void _io_free(struct objio_state *ios)
{
	bio_put(ios->bio);
	ios->bio = NULL;
}

static int _io_add_page(struct objio_state *ios, struct page *page,
			unsigned len, unsigned start)
{
	int added_len = bio_add_pc_page(osd_request_queue(ios->objio_seg->od),
					ios->bio, page, len, start);
	if (unlikely(len != added_len))
		return -ENOMEM;

	ios->length += len;
	return 0;
}

static int _io_rw_pagelist(struct objio_state *ios, bool do_write)
{
	struct objlayout_segment *lseg = LSEG_LD_DATA(ios->ol_state.lseg);
	struct objio_segment *objio_seg = lseg->internal;
	unsigned long count = ios->ol_state.count;
	unsigned pgbase = ios->ol_state.pgbase;
	unsigned nr_pages = ios->ol_state.nr_pages;
	struct page **pages = ios->ol_state.pages;
	int ret;

	ios->objio_seg = objio_seg;
	ios->expected_pages = nr_pages;

	ret = _io_try_alloc(ios, do_write);
	if (ret)
		return ret;

	while (count) {
		unsigned this_count;

		this_count = min(count, PAGE_SIZE - pgbase);
		ret = _io_add_page(ios, *pages, this_count, pgbase);
		if (unlikely(ret))
			break;

		pgbase = 0;
		++pages;
		count -= this_count;
	}

	return ios->length ? 0 : ret;
}

/*
 * read
 */
static ssize_t _read_done_ret(struct osd_request *or, struct objio_state *ios)
{
	u64 resid = ios->length;
	ssize_t status;
	int ret = _check_io_resid(or, &resid, NULL);

	osd_end_request(or);
	_io_free(ios);

	if (likely(!ret))
		status = ios->length;
	else
		status = ret;

	objlayout_read_done(&ios->ol_state, ios->ol_state.sync);
	return status;
}

static void _read_done(struct osd_request *or, void *p)
{
	struct objio_state *ios = p;

	_read_done_ret(or, ios);
}

static ssize_t _read_exec(struct objio_state *ios)
{
	struct osd_request *or = NULL;
	struct pnfs_osd_object_cred *cred =
					&ios->objio_seg->layout->olo_comps[0];
	struct osd_obj_id obj = {cred->oc_object_id.oid_partition_id,
					cred->oc_object_id.oid_object_id};
	u8 *caps;
	int ret;

	or = osd_start_request(ios->objio_seg->od, GFP_KERNEL);
	if (unlikely(!or)) {
		ret = -ENOMEM;
		goto err;
	}

	osd_req_read(or, &obj, ios->ol_state.offset, ios->bio, ios->length);
	caps = cred->oc_cap.cred;

	dprintk("%s obj=0x%llx start=0x%llx length=0x%lx sync=%d\n",
		  __func__, obj.id, _LLU(ios->ol_state.offset),
		  ios->length, ios->ol_state.sync);

	if (ios->ol_state.sync) {
		_sync_op(or, caps);
		return _read_done_ret(or, ios);
	}

	ret = _async_op(or, _read_done, ios, caps);
	if (unlikely(ret))
		goto err;

	return 0;

err:
	_io_free(ios);
	if (or)
		osd_end_request(or);
	return ret;
}

ssize_t objio_read_pagelist(struct objlayout_io_state *ol_state)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);
	int ret;

	ret = _io_rw_pagelist(ios, false);
	if (unlikely(ret))
		return ret;

	return _read_exec(ios);
}

/*
 * write
 */
static ssize_t _write_done_ret(struct osd_request *or, struct objio_state *ios)
{
	u64 resid = ios->length;
	ssize_t status;
	int ret = _check_io_resid(or, NULL, &resid);

	osd_end_request(or);
	_io_free(ios);

	if (likely(!ret)) {
		/* FIXME: should be based on the OSD's persistence model
		 * See OSD2r05 Section 4.13 Data persistence model */
		ios->ol_state.committed = NFS_FILE_SYNC;
		status = ios->length;
	} else {
		status = ret;
	}

	objlayout_write_done(&ios->ol_state, ios->ol_state.sync);
	return status;
}

static void _write_done(struct osd_request *or, void *p)
{
	struct objio_state *ios = p;

	_write_done_ret(or, ios);
}

static int _write_exec(struct objio_state *ios)
{
	struct osd_request *or = NULL;
	struct pnfs_osd_object_cred *cred =
					&ios->objio_seg->layout->olo_comps[0];
	struct osd_obj_id obj = {cred->oc_object_id.oid_partition_id,
					cred->oc_object_id.oid_object_id};
	u8 *caps;
	int ret;

	or = osd_start_request(ios->objio_seg->od, GFP_KERNEL);
	if (unlikely(!or)) {
		ret = -ENOMEM;
		goto err;
	}

	osd_req_write(or, &obj, ios->ol_state.offset, ios->bio, ios->length);
	caps = cred->oc_cap.cred;

	dprintk("%s obj=0x%llx start=0x%llx length=0x%lx sync=%d\n",
		  __func__, obj.id, _LLU(ios->ol_state.offset),
		  ios->length, ios->ol_state.sync);

	if (ios->ol_state.sync) {
		_sync_op(or, caps);
		return _write_done_ret(or, ios);
	}

	ret = _async_op(or, _write_done, ios, caps);
	if (unlikely(ret))
		goto err;

	return 0;

err:
	_io_free(ios);
	if (or)
		osd_end_request(or);
	return ret;
}

ssize_t objio_write_pagelist(struct objlayout_io_state *ol_state, bool stable)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);
	int ret;

	/* TODO: ios->stable = stable; */
	ret = _io_rw_pagelist(ios, true);
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
		int n = 0;
		struct objlayout_segment *objlseg = LSEG_LD_DATA(lseg);
		struct pnfs_osd_layout *lo =
			(struct pnfs_osd_layout *)objlseg->pnfs_osd_layout;
		struct pnfs_osd_data_map *map = &lo->olo_map;

		switch (map->odm_raid_algorithm) {
		case PNFS_OSD_RAID_0:
			n = lo->olo_num_comps;
			break;

		case PNFS_OSD_RAID_4:
		case PNFS_OSD_RAID_5:
			n = map->odm_group_width;
			if (n == 0)
				n = lo->olo_num_comps;
			n -= 1;
			n *= 8;	/* FIXME: until we have 2-D coalescing */
			break;

		case PNFS_OSD_RAID_PQ:
			n = map->odm_group_width;
			if (n == 0)
				n = lo->olo_num_comps;
			n -= 2;
			break;

		default:
			BUG_ON(1);
		}
		sz = map->odm_stripe_unit * n;
		if (sz > maxsz)
			maxsz = sz;
	}
	dprintk("%s: Return %Zd\n", __func__, maxsz);
	return maxsz;
}

/*
 * Get the max [rw]size
 */
static ssize_t
objlayout_get_blocksize(struct pnfs_mount_type *mountid)
{
	ssize_t sz = BIO_MAX_PAGES_KMALLOC * PAGE_SIZE;

	dprintk("%s: Return %Zd\n", __func__, sz);
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
	dprintk("%s: Return %Zd\n", __func__, sz);
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
