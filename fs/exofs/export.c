/*
 * export.c - Implementation of the pnfs_export_operations
 *
 * Copyright (C) 2009 Panasas Inc.
 * All rights reserved.
 *
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

#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfsd/nfsd4_pnfs.h>
#include <linux/nfsd/xdr4.h>

#include "../nfs/objlayout/pnfs_osd_xdr.h"
#include "exofs.h"

enum {SINGLE_DEV_ID = 0};

static int exofs_layout_type(struct super_block *sb)
{
	EXOFS_DBGMSG("sb=%p type=%x\n", sb, LAYOUT_OSD2_OBJECTS);

	return LAYOUT_OSD2_OBJECTS;
}

static void set_dev_id(struct pnfs_deviceid *pnfs_devid, u64 fsid, u64 devid)
{
	deviceid_t *dev_id = (deviceid_t *)pnfs_devid;

	dev_id->pnfs_fsid  = fsid;
	dev_id->pnfs_devid = devid;
}

static int exofs_layout_get(
	struct inode *inode,
	struct pnfs_layoutget_arg *lgp)
{
	struct exofs_i_info *oi = exofs_i(inode);
	struct exofs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct pnfs_osd_object_cred cred;
	struct pnfs_osd_layout layout;
	u32 *p, *start, *end;
	int in_recall;
	int err;

	lgp->seg.offset = 0;
	lgp->seg.length = NFS4_MAX_UINT64;
	lgp->seg.iomode = IOMODE_RW;
	lgp->return_on_close = true; /* TODO: unused but will be soon */

	/* Fill in a pnfs_osd_layout struct */
	layout.olo_map.odm_num_comps		= 1;
	layout.olo_map.odm_stripe_unit		= PAGE_SIZE;
	layout.olo_map.odm_group_width		= 0;
	layout.olo_map.odm_group_depth		= 0;
	layout.olo_map.odm_mirror_cnt		= 0;
	layout.olo_map.odm_raid_algorithm	= PNFS_OSD_RAID_0;

	set_dev_id(&cred.oc_object_id.oid_device_id, lgp->fsid, SINGLE_DEV_ID);
	cred.oc_object_id.oid_partition_id	= sbi->s_pid;
	cred.oc_object_id.oid_object_id		= inode->i_ino + EXOFS_OBJ_OFF;
	cred.oc_osd_version = osd_dev_is_ver1(sbi->s_dev) ?
					PNFS_OSD_VERSION_1 :
					PNFS_OSD_VERSION_2;
	cred.oc_cap_key_sec = PNFS_OSD_CAP_KEY_SEC_NONE;

	cred.oc_cap_key.cred_len	= 0;
	cred.oc_cap_key.cred		= NULL;

	cred.oc_cap.cred_len	= OSD_CAP_LEN;
	cred.oc_cap.cred	= oi->i_cred;

	layout.olo_comps_index = 0;
	layout.olo_num_comps = 1;
	layout.olo_comps = &cred;

	/* Now encode the layout */
	p = start = lgp->xdr.resp->p;
	end = start + lgp->xdr.maxcount;

	/* skip opaque size, will be filled-in later */
	if (p + 1 > end) {
		err = -E2BIG;
		goto err;
	}
	p++;

	err = pnfs_osd_xdr_encode_layout(&p, end, &layout);
	if (err)
		goto err;

	lgp->xdr.bytes_written = (p - start) * 4;
	*start = htonl(lgp->xdr.bytes_written - 4);

	spin_lock(&oi->i_layout_lock);
	in_recall = test_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	if (!in_recall)
		__set_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);

	if (in_recall) {
		err = -EAGAIN;
		goto err;
	}

	EXOFS_DBGMSG("(0x%lx) xdr_bytes=%u\n",
		     inode->i_ino, lgp->xdr.bytes_written);
	return 0;

err:
	EXOFS_DBGMSG("Error: (0x%lx) err=%d at_byte=%zu\n",
		     inode->i_ino, err, (p - start) * 4);
	return err;
}

/* NOTE: inode mutex must NOT be held */
static int exofs_layout_commit(
	struct inode *inode,
	struct nfsd4_pnfs_layoutcommit *lcp)
{
	struct exofs_i_info *oi = exofs_i(inode);
	struct timespec mtime;
	loff_t i_size;
	int in_recall;

	/* In case of a recall we ignore the new size and mtime since they
	 * are going to be changed again by truncate, and since we cannot take
	 * the inode lock in that case.
	 */
	spin_lock(&oi->i_layout_lock);
	in_recall = test_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);
	if (in_recall) {
		EXOFS_DBGMSG("(0x%lx) commit was called during recall\n",
			     inode->i_ino);
		return 0;
	}

	/* NOTE: I would love to call inode_setattr here
	 *	 but i cannot since this will cause an eventual vmtruncate,
	 *	 which will cause a layout_recall. So open code the i_size
	 *	 and mtime/atime changes under i_mutex.
	 */
	mutex_lock_nested(&inode->i_mutex, I_MUTEX_NORMAL);

	if (lcp->lc_mtime.seconds) {
		mtime.tv_sec = lcp->lc_mtime.seconds;
		mtime.tv_nsec = lcp->lc_mtime.nseconds;

		/* layout commit may only make time bigger, since there might
		 * be reordering of the notifications and it might arrive after
		 * A local change.
		 * TODO: if mtime > ctime then we know set_attr did an mtime
		 * in the future. and we can let this update through
		 */
		if (0 <= timespec_compare(&mtime, &inode->i_mtime))
			mtime = inode->i_mtime;
	} else {
		mtime = current_fs_time(inode->i_sb);
	}

	/* TODO: Will below work? since mark_inode_dirty has it's own
	 *       Time handling
	 */
	inode->i_atime = inode->i_mtime = mtime;

	i_size = i_size_read(inode);
	if (lcp->lc_newoffset) {
		loff_t new_size = lcp->lc_last_wr + 1;

		if (i_size < new_size)
			i_size_write(inode, i_size = new_size);
	}
	/* TODO: else { i_size = osd_get_object_length() } */

	mark_inode_dirty_sync(inode);

	mutex_unlock(&inode->i_mutex);
	EXOFS_DBGMSG("(0x%lx) i_size=0x%llx lcp->off=0x%llx\n",
		     inode->i_ino, i_size, lcp->lc_last_wr);
	return 0;
}

static void exofs_handle_error(struct pnfs_osd_ioerr *ioerr)
{
	EXOFS_ERR("exofs_handle_error: errno=%d is_write=%d obj=0x%llx "
		  "offset=0x%llx length=0x%llx\n",
		  ioerr->oer_errno, ioerr->oer_iswrite,
		  _LLU(ioerr->oer_component.oid_object_id),
		  _LLU(ioerr->oer_comp_offset),
		  _LLU(ioerr->oer_comp_length));
}

static int exofs_layout_return(
	struct inode *inode,
	struct nfsd4_pnfs_layoutreturn *lrp)
{
	__be32 *p = lrp->lrf_body;
	unsigned len = lrp->lrf_body_len / 4;

	EXOFS_DBGMSG("(0x%lx) cookie %p xdr_len %d\n",
		     inode->i_ino, lrp->lr_cookie, len);

	while (len >= pnfs_osd_ioerr_xdr_sz()) {
		struct pnfs_osd_ioerr ioerr;

		p = pnfs_osd_xdr_decode_ioerr(&ioerr, p);
		len -= pnfs_osd_ioerr_xdr_sz();
		exofs_handle_error(&ioerr);
	}

	if (lrp->lr_cookie) {
		struct exofs_i_info *oi = exofs_i(inode);
		bool in_recall;

		if (lrp->lr_cookie == PNFS_LAST_LAYOUT_NO_RECALLS) {
			spin_lock(&oi->i_layout_lock);
			in_recall = test_bit(OBJ_IN_LAYOUT_RECALL,
						   &oi->i_flags);
			if (!in_recall)
				__clear_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
			spin_unlock(&oi->i_layout_lock);
		} else {
			in_recall = true;
		}

		/* TODO: how to communicate cookie with the waiter */
		if (in_recall)
			wake_up(&oi->i_wq); /* wakeup any recalls */
	}

	return 0;
}

int exofs_get_device_info(struct super_block *sb, struct pnfs_devinfo_arg *arg)
{
	struct exofs_sb_info *sbi = sb->s_fs_info;
	struct pnfs_osd_deviceaddr devaddr;
	const struct osd_dev_info *odi;
	u32 *p, *start, *end;
	int err;

	memset(&devaddr, 0, sizeof(devaddr));

	if (arg->devid.pnfs_devid != SINGLE_DEV_ID)
		return -ENODEV;

	odi = osduld_device_info(sbi->s_dev);

	devaddr.oda_systemid.len = odi->systemid_len;
	devaddr.oda_systemid.data = (void *)odi->systemid; /* !const cast */

	devaddr.oda_osdname.len = odi->osdname_len ;
	devaddr.oda_osdname.data = (void *)odi->osdname;/* !const cast */

	/* Now encode the device info */
	p = start = arg->xdr.resp->p;
	end = start + arg->xdr.maxcount;

	/* skip opaque size, will be filled-in later */
	if (p + 1 > end) {
		err = -E2BIG;
		goto err;
	}
	p++;

	err = pnfs_osd_xdr_encode_deviceaddr(&p, end, &devaddr);
	if (err)
		goto err;

	arg->xdr.bytes_written = (p - start) * 4;
	*start = htonl(arg->xdr.bytes_written - 4);

	EXOFS_DBGMSG("xdr_bytes=%u\n", arg->xdr.bytes_written);
	return 0;

err:
	EXOFS_DBGMSG("Error: err=%d at_byte=%zu\n",
		     err, (p - start) * 4);
	return err;
}

struct pnfs_export_operations exofs_pnfs_ops = {
	.layout_type	= exofs_layout_type,
	.layout_get	= exofs_layout_get,
	.layout_commit	= exofs_layout_commit,
	.layout_return	= exofs_layout_return,
	.get_device_info = exofs_get_device_info,
};

static int is_layout_returned(struct exofs_i_info *oi)
{
	struct inode *inode = &oi->vfs_inode;
	struct nfsd4_pnfs_cb_layout cbl;
	struct pnfsd_cb_ctl cb_ctl;
	int layout_given;
	int status;

	layout_given = test_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
	if (!layout_given)
		return 1;

	/* We most probably have finished the recall, unless there was an out-
	 * of-memory condition when sending the recall. For forward progress
	 * some recalls where sent and some not, resend these that where lost
	 * before. If cb_layout_recall returns with -ENOENT we know we are
	 * done for sure.
	 */
	memset(&cb_ctl, 0, sizeof(cb_ctl));
	status = pnfsd_get_cb_op(&cb_ctl);
	if (unlikely(status)) {
		EXOFS_ERR("exofs_layout_return: nfsd unloaded!!"
			  " inode (0x%lx) status=%d\n", inode->i_ino, status);
		goto err;
	}

	memset(&cbl, 0, sizeof(cbl));
	cbl.cbl_recall_type = RECALL_FILE;
	cbl.cbl_seg.layout_type = LAYOUT_OSD2_OBJECTS;
	cbl.cbl_seg.iomode = IOMODE_RW;
	cbl.cbl_seg.length = NFS4_MAX_UINT64;
	cbl.cbl_cookie = oi;

	status = cb_ctl.cb_op->cb_layout_recall(inode->i_sb, inode, &cbl);
	pnfsd_put_cb_op(&cb_ctl);

	if (likely(status == -ENOENT)) {
		spin_lock(&oi->i_layout_lock);
		__clear_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
		spin_unlock(&oi->i_layout_lock);

		layout_given = 0;
	} else if (unlikely(status)) {
		/* Double fault in cb_layout_recall for now I would like to know
		   and bailout. Perhaps I need retry counts */
		EXOFS_ERR("exofs_layout_return: Double fault in "
			  "cb_layout_recall inode (0x%lx) status=%d\n",
			  inode->i_ino, status);
		goto err;
	}

	return !layout_given;

err:
	/* this will cause wait_event_interruptible below to break with
	 * an -ERESTARTSY return. If nfsd is able to unload we probably
	 * should break out of any nfs loops
	 */
	restart_syscall();
	return 0;
}

int exofs_inode_recall_layout(struct inode *inode, exofs_recall_fn todo)
{
	struct exofs_i_info *oi = exofs_i(inode);
	struct nfsd4_pnfs_cb_layout cbl;
	struct pnfsd_cb_ctl cb_ctl;
	int layout_given;
	int error = 0;

	spin_lock(&oi->i_layout_lock);
	layout_given = test_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
	__set_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);

	if (!layout_given)
		goto exec;

	EXOFS_DBGMSG("(0x%lx) has_layout issue a recall\n", inode->i_ino);
	memset(&cb_ctl, 0, sizeof(cb_ctl));
	error = pnfsd_get_cb_op(&cb_ctl);
	if (error)
		goto err;

	memset(&cbl, 0, sizeof(cbl));
	cbl.cbl_recall_type = RECALL_FILE;
	cbl.cbl_seg.layout_type = LAYOUT_OSD2_OBJECTS;
	cbl.cbl_seg.iomode = IOMODE_ANY;
	cbl.cbl_seg.length = NFS4_MAX_UINT64;
	cbl.cbl_cookie = todo;

	error = cb_ctl.cb_op->cb_layout_recall(inode->i_sb, inode, &cbl);
	pnfsd_put_cb_op(&cb_ctl);

	switch (error) {
	case 0:
	case -EAGAIN:
		break;
	case -ENOENT:
		goto exec;
	default:
		goto err;
	}

	error = wait_event_interruptible(oi->i_wq, is_layout_returned(oi));
	if (error)
		goto err;

exec:
	EXOFS_DBGMSG("(0x%lx) executing todo\n", inode->i_ino);
	error = todo(inode);

err:
	spin_lock(&oi->i_layout_lock);
	__clear_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);
	EXOFS_DBGMSG("(0x%lx) return=>%d\n", inode->i_ino, error);
	return error;
}
