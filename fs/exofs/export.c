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

	/* TODO: Takes the inode ref here, add to inode's layouts list */

	EXOFS_DBGMSG("(0x%lx) xdr_bytes=%u\n",
		     inode->i_ino, lgp->xdr.bytes_written);

	return 0;

err:
	EXOFS_DBGMSG("Error: (0x%lx) err=%d at_byte=%zu\n",
		     inode->i_ino, err, (p - start) * 4);
	return err;
}

/* This is called under the inode mutex from nfsd4_layoutcommit. If changed
 * then inode mutex must be held, for the hacks done here.
 */
static int exofs_layout_commit(
	struct inode *inode,
	struct nfsd4_pnfs_layoutcommit *lcp)
{
	struct timespec mtime;
	loff_t i_size;

	if (lcp->lc_mtime.seconds) {
		mtime.tv_sec = lcp->lc_mtime.seconds;
		mtime.tv_nsec = lcp->lc_mtime.nseconds;
	} else {
		mtime = current_fs_time(inode->i_sb);
	}
	/* TODO: Will below work? since mark_inode_dirty has it's own
	 *       Time handling
	 */
	inode->i_atime = inode->i_mtime = mtime;

/* TODO: exofs does not currently use the osd_xdr part of the layout_commit */

	i_size = i_size_read(inode);

	if (lcp->lc_newoffset) {
		loff_t new_size = lcp->lc_last_wr + 1;

		if (i_size < new_size)
			i_size_write(inode, i_size = new_size);
	}
	/* TODO: else { i_size = osd_get_object_length() } */

	lcp->lc_size_chg = true;
	lcp->lc_newsize = i_size;

	mark_inode_dirty_sync(inode);
	/*TODO: We might want to call exofs_write_inode(inode, true);
	 *      directly, and not wait for the write_back threads
	 */

	EXOFS_DBGMSG("(0x%lx) i_size=0x%llx lcp->off=0x%llx\n",
		     inode->i_ino, i_size, lcp->lc_last_wr);
	return 0;
}

static int exofs_layout_return(
	struct inode *inode,
	struct nfsd4_pnfs_layoutreturn *lrp)
{
	/* TODO: Decode the pnfs_osd_ioerr if lrf_body_len > 0 */

	/* TODO: When layout_get takes the inode ref put_ref here */
	return 0;
}

struct pnfs_export_operations exofs_pnfs_ops = {
	.layout_type	= exofs_layout_type,
	.layout_get	= exofs_layout_get,
	.layout_commit	= exofs_layout_commit,
	.layout_return	= exofs_layout_return,
};
