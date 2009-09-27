/*
 * Copyright (C) 2005, 2006
 * Avishay Traeger (avishay@gmail.com)
 * Copyright (C) 2008, 2009
 * Boaz Harrosh <bharrosh@panasas.com>
 *
 * This file is part of exofs.
 *
 * exofs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.  Since it is based on ext2, and the only
 * valid version of GPL for the Linux kernel is version 2, the only valid
 * version of GPL for exofs is version 2.
 *
 * exofs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with exofs; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <scsi/scsi_device.h>
#include <scsi/osd_sense.h>

#include "../nfs/objlayout/pnfs_osd_xdr.h"
#include "exofs.h"

void exofs_make_credential(u8 cred_a[OSD_CAP_LEN], const struct osd_obj_id *obj)
{
	osd_sec_init_nosec_doall_caps(cred_a, obj, false, true);
}

/*
 * Perform a synchronous OSD operation.
 */
int exofs_sync_op(struct osd_request *or, int timeout, uint8_t *credential)
{
	int ret;

	if (timeout)
		or->timeout = timeout;
	ret = osd_finalize_request(or, 0, credential, NULL);
	if (unlikely(ret)) {
		EXOFS_DBGMSG("Faild to osd_finalize_request() => %d\n", ret);
		return ret;
	}

	ret = osd_execute_request(or);

	if (unlikely(ret))
		EXOFS_DBGMSG("osd_execute_request() => %d\n", ret);
	/* osd_req_decode_sense(or, ret); */
	return ret;
}

int extract_attr_from_req(struct osd_request *or, struct osd_attr *attr)
{
	struct osd_attr cur_attr = {.attr_page = 0}; /* start with zeros */
	void *iter = NULL;
	int nelem;

	do {
		nelem = 1;
		osd_req_decode_get_attr_list(or, &cur_attr, &nelem, &iter);
		if ((cur_attr.attr_page == attr->attr_page) &&
		    (cur_attr.attr_id == attr->attr_id)) {
			attr->len = cur_attr.len;
			attr->val_ptr = cur_attr.val_ptr;
			return 0;
		}
	} while (iter);

	return -EIO;
}

int exofs_read_kern(struct osd_dev *od, u8 *cred, struct osd_obj_id *obj,
		    u64 offset, void *p, unsigned length)
{
	struct osd_request *or = osd_start_request(od, GFP_KERNEL);
/*	struct osd_sense_info osi = {.key = 0};*/
	int ret;

	if (unlikely(!or)) {
		EXOFS_DBGMSG("%s: osd_start_request failed.\n", __func__);
		return -ENOMEM;
	}
	ret = osd_req_read_kern(or, obj, offset, p, length);
	if (unlikely(ret)) {
		EXOFS_DBGMSG("%s: osd_req_read_kern failed.\n", __func__);
		goto out;
	}

	ret = exofs_sync_op(or, 0, cred);
/*	ret = osd_req_decode_sense(or, &osi);*/
	if (unlikely(ret))
		EXOFS_DBGMSG("%s: exofs_sync_op failed.\n", __func__);

out:
	osd_end_request(or);
	return ret;
}

int exofs_get_io_state(struct exofs_sb_info *sbi, struct exofs_io_state** pios)
{
	struct exofs_io_state *ios;

	/*TODO: Maybe use kmem_cach per sbi of size
	 * exofs_io_state_size(sbi->s_numdevs)
	 */
	ios = kzalloc(exofs_io_state_size(sbi->s_numdevs), GFP_KERNEL);
	if (unlikely(!ios)) {
		*pios = NULL;
		return -ENOMEM;
	}

	ios->sbi = sbi;
	ios->obj.partition = sbi->s_pid;
	*pios = ios;
	return 0;
}

void exofs_put_io_state(struct exofs_io_state *ios)
{
	if (ios) {
		unsigned i;

		for (i = 0; i < ios->numdevs; i++)
			if (ios->per_dev[i].or)
				osd_end_request(ios->per_dev[i].or);

		kfree(ios);
	}
}

static void _sync_done(struct exofs_io_state *ios, void *p)
{
	struct completion *waiting = p;

	complete(waiting);
}

static void _last_io(struct kref *kref)
{
	struct exofs_io_state *ios = container_of(
					kref, struct exofs_io_state, kref);

	ios->done(ios, ios->private);
}

static void _done_io(struct osd_request *or, void *p)
{
	struct exofs_io_state *ios = p;

	kref_put(&ios->kref, _last_io);
}

static int exofs_io_execute(struct exofs_io_state *ios)
{
	int i, ret;

	for (i = 0; i < ios->numdevs; i++) {
		struct osd_request *or = ios->per_dev[i].or;
		if (unlikely(!or))
			continue;

		ret = osd_finalize_request(or, 0, ios->cred, NULL);
		if (unlikely(ret)) {
			EXOFS_DBGMSG("Faild to osd_finalize_request() => %d\n",
				     ret);
			return ret;
		}
	}

	for (i = 0; i < ios->numdevs; i++) {
		struct osd_request *or = ios->per_dev[i].or;
		if (unlikely(!or))
			continue;

		kref_get(&ios->kref);
		osd_execute_request_async(or, _done_io, ios);
	}

	kref_put(&ios->kref, _last_io);
	return 0;
}

static bool _is_osd_security_code(int code)
{
	return	(code == osd_security_audit_value_frozen) ||
		(code == osd_security_working_key_frozen) ||
		(code == osd_nonce_not_unique) ||
		(code == osd_nonce_timestamp_out_of_range) ||
		(code == osd_invalid_dataout_buffer_integrity_check_value);
}

static int err_prio(u32 oer_errno)
{
	static u8 error_priority[] = {
		[PNFS_OSD_ERR_EIO] = 7,
		[PNFS_OSD_ERR_NO_SPACE] = 6,
		[PNFS_OSD_ERR_NOT_FOUND] = 5,
		[PNFS_OSD_ERR_UNREACHABLE] = 4,
		[PNFS_OSD_ERR_NO_ACCESS] = 3,
		[PNFS_OSD_ERR_BAD_CRED] = 2,
		[PNFS_OSD_ERR_RESOURCE] = 1,
		[0] = 0,
	};

	BUG_ON(oer_errno >= ARRAY_SIZE(error_priority));

	return error_priority[oer_errno];
}

struct acumulated_err_desc {
	unsigned osd_error;
	int lin_ret;
};

static int _check_or(struct osd_request *or, struct acumulated_err_desc *err)
{
	struct osd_sense_info osi = {.key = 0};  /* FIXME: bug in libosd */
	int ret = osd_req_decode_sense(or, &osi);
	int osd_error;

	if (likely(!ret))
		return 0;

	if (osi.additional_code == scsi_invalid_field_in_cdb) {
		if (osi.cdb_field_offset == OSD_CFO_STARTING_BYTE) {
			osd_error = 0;
			ret = -EFAULT; /* we will recover from this */
		} else if (osi.cdb_field_offset == OSD_CFO_OBJECT_ID) {
			osd_error = PNFS_OSD_ERR_NOT_FOUND;
			ret = -ENOENT;
		} else if (osi.cdb_field_offset == OSD_CFO_PERMISSIONS) {
			osd_error = PNFS_OSD_ERR_NO_ACCESS;
			ret = -EACCES;
		} else {
			osd_error = PNFS_OSD_ERR_BAD_CRED;
			ret = -EINVAL;
		}
	} else if (osi.additional_code == osd_quota_error) {
		osd_error = PNFS_OSD_ERR_NO_SPACE;
		ret = -ENOSPC;
	} else if (_is_osd_security_code(osi.additional_code)) {
		osd_error = PNFS_OSD_ERR_BAD_CRED;
		ret = -EINVAL;
	} else if (!osi.key) {
		/* scsi sense is Empty, we currently cannot know if it
		 * is an out-of-memory or communication error. But try
		 * anyway
		 */
		if (or->async_error == -ENOMEM)
			osd_error = PNFS_OSD_ERR_RESOURCE;
		else
			osd_error = PNFS_OSD_ERR_UNREACHABLE;
		ret = or->async_error;
	} else {
		osd_error = PNFS_OSD_ERR_EIO;
		ret = -EIO;
	}

	if (err_prio(osd_error) >= err_prio(err->osd_error)) {
		err->osd_error = osd_error;
		err->lin_ret = ret;
	}

	return ret;
}

int exofs_check_io(struct exofs_io_state *ios)
{
	struct acumulated_err_desc err = {.osd_error = 0};
	int i;

	for (i = 0; i < ios->numdevs; i++) {
		_check_or(ios->per_dev[i].or, &err);
/*TODO:		if (unlikely(ret == -EFAULT)) {
			if (ios->per_dev[i].offset < ios->need_zero_offset)
				ios->need_zero_offset = ios->per_dev[i].offset;
			if (ios->per_dev[i].end > ios->need_zero_end)
				ios->need_zero_end = ios->per_dev[i].end;
		}*/
	}

	return err.lin_ret;
}

int exofs_sbi_create(struct exofs_sb_info *sbi, struct exofs_io_state *ios)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	bool sync = (ios->done == NULL);
	int i, ret = 0;

	if (sync) {
		ios->done = _sync_done;
		ios->private = &wait;
	}

	kref_init(&ios->kref);

	for (i = 0; i < sbi->s_numdevs; i++) {
		struct osd_request *or;

		or = osd_start_request(sbi->s_ods[i], GFP_KERNEL);
		if (unlikely(!or)) {
			EXOFS_ERR("%s: osd_start_request failed\n", __func__);
			ret = -ENOMEM;
			goto out;
		}
		ios->per_dev[i].or = or;
		ios->numdevs++;

		osd_req_create_object(or, &ios->obj);
	}

	ret = exofs_io_execute(ios);
	if (sync /*&& likely(!ret)*/) {
		wait_for_completion(&wait);
/*		ret = exofs_check_io(ios);*/
	}

out:
	return ret;
}

int exofs_sbi_remove(struct exofs_sb_info *sbi, struct exofs_io_state *ios)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	bool sync = (ios->done == NULL);
	int i, ret = 0;

	if (sync) {
		ios->done = _sync_done;
		ios->private = &wait;
	}

	kref_init(&ios->kref);

	for (i = 0; i < sbi->s_numdevs; i++) {
		struct osd_request *or;

		or = osd_start_request(sbi->s_ods[i], GFP_KERNEL);
		if (unlikely(!or)) {
			EXOFS_ERR("%s: osd_start_request failed\n", __func__);
			ret = -ENOMEM;
			goto out;
		}
		ios->per_dev[i].or = or;
		ios->numdevs++;

		osd_req_remove_object(or, &ios->obj);
	}
	ret = exofs_io_execute(ios);
	if (sync /*&& likely(!ret)*/) {
		wait_for_completion(&wait);
/*		ret = exofs_check_io(ios);*/
	}

out:
	return ret;
}

int exofs_sbi_write(struct exofs_sb_info *sbi, struct exofs_io_state *ios)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	bool sync = (ios->done == NULL);
	int i, ret = 0;

	if (sync) {
		ios->done = _sync_done;
		ios->private = &wait;
	}

	kref_init(&ios->kref);

	for (i = 0; i < sbi->s_numdevs; i++) {
		struct osd_request *or;

		or = osd_start_request(sbi->s_ods[i], GFP_KERNEL);
		if (unlikely(!or)) {
			EXOFS_ERR("%s: osd_start_request failed\n", __func__);
			ret = -ENOMEM;
			goto out;
		}
		ios->per_dev[i].or = or;
		ios->numdevs++;

		if (ios->bio) {
			struct bio *bio;

			if (i != 0) {
				bio = bio_kmalloc(GFP_KERNEL,
						  ios->bio->bi_max_vecs);
				if (!bio)
					goto out;

				__bio_clone(bio, ios->bio);
				bio->bi_bdev = NULL;
				bio->bi_next = NULL;
			} else {
				bio = ios->bio;
			}

			osd_req_write(or, &ios->obj, ios->offset, bio,
				      ios->length);
			EXOFS_DBGMSG("osd_req_write sync=%d\n", sync);
		} else if (ios->kern_buff) {
			osd_req_write_kern(or, &ios->obj, ios->offset,
					   ios->kern_buff, ios->length);
			EXOFS_DBGMSG("osd_req_write_kern sync=%d\n", sync);
		} else {
			osd_req_set_attributes(or, &ios->obj);
			EXOFS_DBGMSG("osd_req_set_attributes sync=%d\n", sync);
		}

		if (ios->out_attr)
			osd_req_add_set_attr_list(or, ios->out_attr,
						  ios->out_attr_len);

		if (ios->in_attr)
			osd_req_add_get_attr_list(or, ios->in_attr,
						  ios->in_attr_len);
	}
	ret = exofs_io_execute(ios);
	if (sync && likely(!ret)) {
		wait_for_completion(&wait);
		ret = exofs_check_io(ios);
	}

out:
	return ret;
}

static void _done_read(struct osd_request *or, void *p)
{
	struct exofs_io_state *ios = p;

	ios->done(ios, ios->private);
}

int exofs_sbi_read(struct exofs_sb_info *sbi, struct exofs_io_state *ios)
{
	struct osd_request *or;
	bool sync = (ios->done == NULL);
	int ret;

	or = osd_start_request(sbi->s_ods[0], GFP_KERNEL);
	if (unlikely(!or)) {
		EXOFS_ERR("%s: osd_start_request failed\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	ios->per_dev[0].or = or;
	ios->numdevs = 1;

	if (ios->bio)
		osd_req_read(or, &ios->obj, ios->offset, ios->bio, ios->length);
	else if (ios->kern_buff)
		osd_req_read_kern(or, &ios->obj, ios->offset,
				   ios->kern_buff, ios->length);
	else
		osd_req_get_attributes(or, &ios->obj);

	if (ios->out_attr)
		osd_req_add_set_attr_list(or, ios->out_attr,
					  ios->out_attr_len);

	if (ios->in_attr)
		osd_req_add_set_attr_list(or, ios->in_attr,
					  ios->in_attr_len);

	if (sync)
		ret = exofs_sync_op(or, sbi->s_timeout, ios->cred);
	else {
		ret = osd_finalize_request(or, 0, ios->cred, NULL);
		if (unlikely(ret)) {
			EXOFS_DBGMSG("Faild to osd_finalize_request() => %d\n",
				     ret);
			goto out;
		}

		osd_execute_request_async(or, _done_read, ios);
	}

out:
	return ret;
}

int exofs_oi_write(struct exofs_i_info *oi, struct exofs_io_state *ios)
{
	struct exofs_sb_info *sbi = oi->vfs_inode.i_sb->s_fs_info;

	ios->obj.id = exofs_oi_objno(oi);
	ios->cred = oi->i_cred;
	return exofs_sbi_write(sbi, ios);
}

int exofs_oi_read(struct exofs_i_info *oi, struct exofs_io_state *ios)
{
	struct exofs_sb_info *sbi = oi->vfs_inode.i_sb->s_fs_info;

	ios->obj.id = exofs_oi_objno(oi);
	ios->cred = oi->i_cred;
	return exofs_sbi_read(sbi, ios);
}

int exofs_oi_truncate(struct exofs_i_info *oi, u64 size)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	struct exofs_sb_info *sbi = oi->vfs_inode.i_sb->s_fs_info;
	struct exofs_io_state *ios;
	struct osd_attr attr;
	__be64 newsize;
	int i, ret;

	if (exofs_get_io_state(sbi, &ios))
		return -ENOMEM;

	ios->obj.id = exofs_oi_objno(oi);
	ios->cred = oi->i_cred;

	newsize = cpu_to_be64(size);
	attr = g_attr_logical_length;
	attr.val_ptr = &newsize;

	ios->done = _sync_done;
	ios->private = &wait;

	kref_init(&ios->kref);

	for (i = 0; i < sbi->s_numdevs; i++) {
		struct osd_request *or;

		or = osd_start_request(sbi->s_ods[i], GFP_KERNEL);
		if (unlikely(!or)) {
			EXOFS_ERR("%s: osd_start_request failed\n", __func__);
			ret = -ENOMEM;
			goto out;
		}
		ios->per_dev[i].or = or;
		ios->numdevs++;

		osd_req_set_attributes(or, &ios->obj);
		osd_req_add_set_attr_list(or, &attr, 1);
	}
	ret = exofs_io_execute(ios);
	if (unlikely(ret))
		goto out;

	wait_for_completion(&wait);
	ret = exofs_check_io(ios);

out:
	exofs_put_io_state(ios);
	return ret;
}
