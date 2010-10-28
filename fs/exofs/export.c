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

#include <linux/nfsd/nfsd4_pnfs.h>
#include "exofs.h"

static int exofs_layout_type(struct super_block *sb)
{
	return LAYOUT_OSD2_OBJECTS;
}

static void set_dev_id(struct nfs4_deviceid *pnfs_devid, u64 sbid, u64 devid)
{
	struct nfsd4_pnfs_deviceid *dev_id =
		(struct nfsd4_pnfs_deviceid *)pnfs_devid;

	dev_id->sbid  = sbid;
	dev_id->devid = devid;
}

static int cb_layout_recall(struct inode *inode, enum pnfs_iomode iomode,
			    u64 offset, u64 length, void *cookie)
{
	struct nfsd4_pnfs_cb_layout cbl;
	struct pnfsd_cb_ctl cb_ctl;
	int status;

	memset(&cb_ctl, 0, sizeof(cb_ctl));
	status = pnfsd_get_cb_op(&cb_ctl);
	if (unlikely(status)) {
		EXOFS_ERR("%s: nfsd unloaded!! inode (0x%lx) status=%d\n",
			  __func__, inode->i_ino, status);
		goto err;
	}

	memset(&cbl, 0, sizeof(cbl));
	cbl.cbl_recall_type = RETURN_FILE;
	cbl.cbl_seg.layout_type = LAYOUT_OSD2_OBJECTS;
	cbl.cbl_seg.iomode = iomode;
	cbl.cbl_seg.offset = offset;
	cbl.cbl_seg.length = length;
	cbl.cbl_cookie = cookie;

	status = cb_ctl.cb_op->cb_layout_recall(inode->i_sb, inode, &cbl);
	pnfsd_put_cb_op(&cb_ctl);

err:
	return status;
}

static enum nfsstat4 exofs_layout_get(
	struct inode *inode,
	struct exp_xdr_stream *xdr,
	const struct nfsd4_pnfs_layoutget_arg *args,
	struct nfsd4_pnfs_layoutget_res *res)
{
	struct exofs_i_info *oi = exofs_i(inode);
	struct exofs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct exofs_layout *el = &sbi->layout;
	struct pnfs_osd_object_cred *creds = NULL;
	struct pnfs_osd_layout layout;
	__be32 *start;
	bool in_recall;
	int i, err;
	enum nfsstat4 nfserr;

	res->lg_seg.offset = 0;
	res->lg_seg.length = NFS4_MAX_UINT64;
	res->lg_seg.iomode = IOMODE_RW;
	res->lg_return_on_close = true; /* TODO: unused but will be soon */

	/* skip opaque size, will be filled-in later */
	start = exp_xdr_reserve_qwords(xdr, 1);
	if (!start) {
		nfserr = NFS4ERR_TOOSMALL;
		goto out;
	}

	creds = kcalloc(el->s_numdevs, sizeof(*creds), GFP_KERNEL);
	if (!creds) {
		nfserr = NFS4ERR_LAYOUTTRYLATER;
		goto out;
	}

	/* Fill in a pnfs_osd_layout struct */
	layout.olo_map = sbi->data_map;

	for (i = 0; i < el->s_numdevs; i++) {
		struct pnfs_osd_object_cred *cred = &creds[i];
		osd_id id = exofs_oi_objno(oi);
		unsigned dev = exofs_layout_od_id(el, id, i);

		set_dev_id(&cred->oc_object_id.oid_device_id, args->lg_sbid,
			   dev);
		cred->oc_object_id.oid_partition_id = el->s_pid;
		cred->oc_object_id.oid_object_id = id;
		cred->oc_osd_version = osd_dev_is_ver1(el->s_ods[dev]) ?
						PNFS_OSD_VERSION_1 :
						PNFS_OSD_VERSION_2;
		cred->oc_cap_key_sec = PNFS_OSD_CAP_KEY_SEC_NONE;

		cred->oc_cap_key.cred_len	= 0;
		cred->oc_cap_key.cred		= NULL;

		cred->oc_cap.cred_len	= OSD_CAP_LEN;
		cred->oc_cap.cred	= oi->i_cred;
	}

	layout.olo_comps_index = 0;
	layout.olo_num_comps = el->s_numdevs;
	layout.olo_comps = creds;

	err = pnfs_osd_xdr_encode_layout(xdr, &layout);
	if (err) {
		nfserr = NFS4ERR_TOOSMALL; /* FIXME: Change osd_xdr error codes */
		goto out;
	}

	exp_xdr_encode_opaque_len(start, xdr->p);

	spin_lock(&oi->i_layout_lock);
	in_recall = test_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	if (!in_recall) {
		__set_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
		nfserr = NFS4_OK;
	} else {
		nfserr = NFS4ERR_RECALLCONFLICT;
	}
	spin_unlock(&oi->i_layout_lock);

out:
	kfree(creds);
	EXOFS_DBGMSG("(0x%lx) nfserr=%u xdr_bytes=%zu\n",
		     inode->i_ino, nfserr, exp_xdr_qbytes(xdr->p - start));
	return nfserr;
}

/* NOTE: inode mutex must NOT be held */
static int exofs_layout_commit(
	struct inode *inode,
	const struct nfsd4_pnfs_layoutcommit_arg *args,
	struct nfsd4_pnfs_layoutcommit_res *res)
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

	if (args->lc_mtime.seconds) {
		mtime.tv_sec = args->lc_mtime.seconds;
		mtime.tv_nsec = args->lc_mtime.nseconds;

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
	if (args->lc_newoffset) {
		loff_t new_size = args->lc_last_wr + 1;

		if (i_size < new_size) {
			i_size_write(inode, i_size = new_size);
			res->lc_size_chg = 1;
			res->lc_newsize = new_size;
		}
	}
	/* TODO: else { i_size = osd_get_object_length() } */

/* TODO: exofs does not currently use the osd_xdr part of the layout_commit */

	mark_inode_dirty_sync(inode);

	mutex_unlock(&inode->i_mutex);
	EXOFS_DBGMSG("(0x%lx) i_size=0x%llx lcp->off=0x%llx\n",
		     inode->i_ino, i_size, args->lc_last_wr);
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
	const struct nfsd4_pnfs_layoutreturn_arg *args)
{
	__be32 *p = args->lrf_body;
	unsigned len = exp_xdr_qwords(args->lrf_body_len);

	EXOFS_DBGMSG("(0x%lx) cookie %p xdr_len %d\n",
		     inode->i_ino, args->lr_cookie, len);

	while (len >= pnfs_osd_ioerr_xdr_sz()) {
		struct pnfs_osd_ioerr ioerr;

		p = pnfs_osd_xdr_decode_ioerr(&ioerr, p);
		len -= pnfs_osd_ioerr_xdr_sz();
		exofs_handle_error(&ioerr);
	}

	if (args->lr_cookie) {
		struct exofs_i_info *oi = exofs_i(inode);
		bool in_recall;

		spin_lock(&oi->i_layout_lock);
		in_recall = test_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
		__clear_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
		spin_unlock(&oi->i_layout_lock);

		/* TODO: how to communicate cookie with the waiter */
		if (in_recall)
			wake_up(&oi->i_wq); /* wakeup any recalls */
	}

	return 0;
}

int exofs_get_device_info(struct super_block *sb, struct exp_xdr_stream *xdr,
			  u32 layout_type,
			  const struct nfsd4_pnfs_deviceid *devid)
{
	struct exofs_sb_info *sbi = sb->s_fs_info;
	struct pnfs_osd_deviceaddr devaddr;
	const struct osd_dev_info *odi;
	u64 devno = devid->devid;
	__be32 *start;
	int err;

	memset(&devaddr, 0, sizeof(devaddr));

	if (unlikely(devno >= sbi->layout.s_numdevs))
		return -ENODEV;

	odi = osduld_device_info(sbi->layout.s_ods[devno]);

	devaddr.oda_systemid.len = odi->systemid_len;
	devaddr.oda_systemid.data = (void *)odi->systemid; /* !const cast */

	devaddr.oda_osdname.len = odi->osdname_len ;
	devaddr.oda_osdname.data = (void *)odi->osdname;/* !const cast */

	/* skip opaque size, will be filled-in later */
	start = exp_xdr_reserve_qwords(xdr, 1);
	if (!start) {
		err = -E2BIG;
		goto err;
	}

	err = pnfs_osd_xdr_encode_deviceaddr(xdr, &devaddr);
	if (err)
		goto err;

	exp_xdr_encode_opaque_len(start, xdr->p);

	EXOFS_DBGMSG("xdr_bytes=%Zu devno=%lld osdname-%s\n",
		     exp_xdr_qbytes(xdr->p - start), devno, odi->osdname);
	return 0;

err:
	EXOFS_DBGMSG("Error: err=%d at_byte=%zu\n",
		     err, exp_xdr_qbytes(xdr->p - start));
	return err;
}

struct pnfs_export_operations exofs_pnfs_ops = {
	.layout_type	= exofs_layout_type,
	.layout_get	= exofs_layout_get,
	.layout_commit	= exofs_layout_commit,
	.layout_return	= exofs_layout_return,
	.get_device_info = exofs_get_device_info,
};

static bool is_layout_returned(struct exofs_i_info *oi)
{
	bool layout_given;

	spin_lock(&oi->i_layout_lock);
	layout_given = test_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);

	return !layout_given;
}

int exofs_inode_recall_layout(struct inode *inode, enum pnfs_iomode iomode,
			      exofs_recall_fn todo, u64 todo_data)
{
	struct exofs_i_info *oi = exofs_i(inode);
	int layout_given;
	int error = 0;

	spin_lock(&oi->i_layout_lock);
	layout_given = test_bit(OBJ_LAYOUT_IS_GIVEN, &oi->i_flags);
	__set_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);

	if (!layout_given)
		goto exec;

	for (;;) {
		EXOFS_DBGMSG("(0x%lx) has_layout issue a recall\n",
			     inode->i_ino);
		error = cb_layout_recall(inode, iomode, 0, NFS4_MAX_UINT64,
					 &oi->i_wq);
		switch (error) {
		case 0:
		case -EAGAIN:
			break;
		case -ENOENT:
			goto exec;
		default:
			goto err;
		}

		error = wait_event_interruptible(oi->i_wq,
						 is_layout_returned(oi));
		if (error)
			goto err;
	}

exec:
	error = todo(inode, todo_data);

err:
	spin_lock(&oi->i_layout_lock);
	__clear_bit(OBJ_IN_LAYOUT_RECALL, &oi->i_flags);
	spin_unlock(&oi->i_layout_lock);
	EXOFS_DBGMSG("(0x%lx) return=>%d\n", inode->i_ino, error);
	return error;
}

void exofs_init_export(struct super_block *sb)
{
	sb->s_pnfs_op = &exofs_pnfs_ops;
}
