/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/gfs2_ondisk.h>
#include <linux/crc32.h>
#include <linux/lm_interface.h>

#include "gfs2.h"
#include "incore.h"
#include "dir.h"
#include "glock.h"
#include "glops.h"
#include "inode.h"
#include "super.h"
#include "rgrp.h"
#include "util.h"

#if defined(CONFIG_PNFSD)
#include <linux/nfs_fs.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/nfsfh.h>
#include <linux/nfsd/pnfsd.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfs4_pnfs.h>
#endif /* CONFIG_PNFSD */

#define GFS2_SMALL_FH_SIZE 4
#define GFS2_LARGE_FH_SIZE 8
#define GFS2_OLD_FH_SIZE 10

static int gfs2_encode_fh(struct dentry *dentry, __u32 *p, int *len,
			  int connectable)
{
	__be32 *fh = (__force __be32 *)p;
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct gfs2_inode *ip = GFS2_I(inode);

	if (*len < GFS2_SMALL_FH_SIZE ||
	    (connectable && *len < GFS2_LARGE_FH_SIZE))
		return 255;

	fh[0] = cpu_to_be32(ip->i_no_formal_ino >> 32);
	fh[1] = cpu_to_be32(ip->i_no_formal_ino & 0xFFFFFFFF);
	fh[2] = cpu_to_be32(ip->i_no_addr >> 32);
	fh[3] = cpu_to_be32(ip->i_no_addr & 0xFFFFFFFF);
	*len = GFS2_SMALL_FH_SIZE;

	if (!connectable || inode == sb->s_root->d_inode)
		return *len;

	spin_lock(&dentry->d_lock);
	inode = dentry->d_parent->d_inode;
	ip = GFS2_I(inode);
	igrab(inode);
	spin_unlock(&dentry->d_lock);

	fh[4] = cpu_to_be32(ip->i_no_formal_ino >> 32);
	fh[5] = cpu_to_be32(ip->i_no_formal_ino & 0xFFFFFFFF);
	fh[6] = cpu_to_be32(ip->i_no_addr >> 32);
	fh[7] = cpu_to_be32(ip->i_no_addr & 0xFFFFFFFF);
	*len = GFS2_LARGE_FH_SIZE;

	iput(inode);

	return *len;
}

struct get_name_filldir {
	struct gfs2_inum_host inum;
	char *name;
};

static int get_name_filldir(void *opaque, const char *name, int length,
			    loff_t offset, u64 inum, unsigned int type)
{
	struct get_name_filldir *gnfd = opaque;

	if (inum != gnfd->inum.no_addr)
		return 0;

	memcpy(gnfd->name, name, length);
	gnfd->name[length] = 0;

	return 1;
}

static int gfs2_get_name(struct dentry *parent, char *name,
			 struct dentry *child)
{
	struct inode *dir = parent->d_inode;
	struct inode *inode = child->d_inode;
	struct gfs2_inode *dip, *ip;
	struct get_name_filldir gnfd;
	struct gfs2_holder gh;
	u64 offset = 0;
	int error;

	if (!dir)
		return -EINVAL;

	if (!S_ISDIR(dir->i_mode) || !inode)
		return -EINVAL;

	dip = GFS2_I(dir);
	ip = GFS2_I(inode);

	*name = 0;
	gnfd.inum.no_addr = ip->i_no_addr;
	gnfd.inum.no_formal_ino = ip->i_no_formal_ino;
	gnfd.name = name;

	error = gfs2_glock_nq_init(dip->i_gl, LM_ST_SHARED, 0, &gh);
	if (error)
		return error;

	error = gfs2_dir_read(dir, &offset, &gnfd, get_name_filldir);

	gfs2_glock_dq_uninit(&gh);

	if (!error && !*name)
		error = -ENOENT;

	return error;
}

static struct dentry *gfs2_get_parent(struct dentry *child)
{
	struct qstr dotdot;
	struct dentry *dentry;

	/*
	 * XXX(hch): it would be a good idea to keep this around as a
	 *	     static variable.
	 */
	gfs2_str2qstr(&dotdot, "..");

	dentry = d_obtain_alias(gfs2_lookupi(child->d_inode, &dotdot, 1));
	if (!IS_ERR(dentry))
		dentry->d_op = &gfs2_dops;
	return dentry;
}

static struct dentry *gfs2_get_dentry(struct super_block *sb,
		struct gfs2_inum_host *inum)
{
	struct gfs2_sbd *sdp = sb->s_fs_info;
	struct gfs2_holder i_gh, ri_gh, rgd_gh;
	struct gfs2_rgrpd *rgd;
	struct inode *inode;
	struct dentry *dentry;
	int error;

	/* System files? */

	inode = gfs2_ilookup(sb, inum->no_addr);
	if (inode) {
		if (GFS2_I(inode)->i_no_formal_ino != inum->no_formal_ino) {
			iput(inode);
			return ERR_PTR(-ESTALE);
		}
		goto out_inode;
	}

	error = gfs2_glock_nq_num(sdp, inum->no_addr, &gfs2_inode_glops,
				  LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
	if (error)
		return ERR_PTR(error);

	error = gfs2_rindex_hold(sdp, &ri_gh);
	if (error)
		goto fail;

	error = -EINVAL;
	rgd = gfs2_blk2rgrpd(sdp, inum->no_addr);
	if (!rgd)
		goto fail_rindex;

	error = gfs2_glock_nq_init(rgd->rd_gl, LM_ST_SHARED, 0, &rgd_gh);
	if (error)
		goto fail_rindex;

	error = -ESTALE;
	if (gfs2_get_block_type(rgd, inum->no_addr) != GFS2_BLKST_DINODE)
		goto fail_rgd;

	gfs2_glock_dq_uninit(&rgd_gh);
	gfs2_glock_dq_uninit(&ri_gh);

	inode = gfs2_inode_lookup(sb, DT_UNKNOWN,
					inum->no_addr,
					0, 0);
	if (IS_ERR(inode)) {
		error = PTR_ERR(inode);
		goto fail;
	}

	error = gfs2_inode_refresh(GFS2_I(inode));
	if (error) {
		iput(inode);
		goto fail;
	}

	/* Pick up the works we bypass in gfs2_inode_lookup */
	if (inode->i_state & I_NEW) 
		gfs2_set_iop(inode);

	if (GFS2_I(inode)->i_no_formal_ino != inum->no_formal_ino) {
		iput(inode);
		goto fail;
	}

	error = -EIO;
	if (GFS2_I(inode)->i_diskflags & GFS2_DIF_SYSTEM) {
		iput(inode);
		goto fail;
	}

	gfs2_glock_dq_uninit(&i_gh);

out_inode:
	dentry = d_obtain_alias(inode);
	if (!IS_ERR(dentry))
		dentry->d_op = &gfs2_dops;
	return dentry;

fail_rgd:
	gfs2_glock_dq_uninit(&rgd_gh);

fail_rindex:
	gfs2_glock_dq_uninit(&ri_gh);

fail:
	gfs2_glock_dq_uninit(&i_gh);
	return ERR_PTR(error);
}

static struct dentry *gfs2_fh_to_dentry(struct super_block *sb, struct fid *fid,
		int fh_len, int fh_type)
{
	struct gfs2_inum_host this;
	__be32 *fh = (__force __be32 *)fid->raw;

	switch (fh_type) {
	case GFS2_SMALL_FH_SIZE:
	case GFS2_LARGE_FH_SIZE:
	case GFS2_OLD_FH_SIZE:
		this.no_formal_ino = ((u64)be32_to_cpu(fh[0])) << 32;
		this.no_formal_ino |= be32_to_cpu(fh[1]);
		this.no_addr = ((u64)be32_to_cpu(fh[2])) << 32;
		this.no_addr |= be32_to_cpu(fh[3]);
		return gfs2_get_dentry(sb, &this);
	default:
		return NULL;
	}
}

static struct dentry *gfs2_fh_to_parent(struct super_block *sb, struct fid *fid,
		int fh_len, int fh_type)
{
	struct gfs2_inum_host parent;
	__be32 *fh = (__force __be32 *)fid->raw;

	switch (fh_type) {
	case GFS2_LARGE_FH_SIZE:
	case GFS2_OLD_FH_SIZE:
		parent.no_formal_ino = ((u64)be32_to_cpu(fh[4])) << 32;
		parent.no_formal_ino |= be32_to_cpu(fh[5]);
		parent.no_addr = ((u64)be32_to_cpu(fh[6])) << 32;
		parent.no_addr |= be32_to_cpu(fh[7]);
		return gfs2_get_dentry(sb, &parent);
	default:
		return NULL;
	}
}

#if defined(CONFIG_PNFSD)
static int gfs2_layout_type(void)
{
	return LAYOUT_NFSV4_FILES;
}

static int get_stripe_unit(int blocksize)
{
	if (blocksize >= NFSSVC_MAXBLKSIZE)
		return blocksize;

	return NFSSVC_MAXBLKSIZE - (NFSSVC_MAXBLKSIZE % blocksize);
}

/*
 * Retrieve and encode a file layout onto the xdr stream.
 * @inode: inode for which to retrieve layout
 * @arg->xdr: xdr stream for encoding
 * @arg->func: a call into file system to encode the layout on xdr stream.
 */
static int gfs2_layout_get(struct inode *inode, struct pnfs_layoutget_arg *arg)
{
	int rc = 0;
	struct pnfs_filelayout_layout *layout = NULL;
	struct knfsd_fh *fhp = NULL;

	printk(KERN_DEBUG "%s: LAYOUT_GET\n", __func__);

	/* Set layout indept response args */
	arg->seg.layout_type = LAYOUT_NFSV4_FILES;
	arg->seg.offset = 0;
	arg->seg.length = inode->i_sb->s_maxbytes; /* The maximum file size */

	layout = kzalloc(sizeof(*layout), GFP_KERNEL);
	if (layout == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	/* Set file layout response args */
	layout->lg_layout_type = LAYOUT_NFSV4_FILES;
	layout->lg_stripe_type = STRIPE_SPARSE;
	layout->lg_commit_through_mds = true;
	layout->lg_stripe_unit = get_stripe_unit(inode->i_sb->s_blocksize);
	layout->lg_fh_length = 1;
	layout->device_id.pnfs_fsid = arg->fsid;
	layout->device_id.pnfs_devid = 1;			/*FSFTEMP*/
	layout->lg_first_stripe_index = 0;			/*FSFTEMP*/
	layout->lg_pattern_offset = 0;

	fhp = kmalloc(sizeof(*fhp), GFP_KERNEL);
	if (fhp == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	memcpy(fhp, arg->fh, sizeof(*fhp));
	pnfs_fh_mark_ds(fhp);
	layout->lg_fh_list = fhp;

	/* Call nfsd to encode layout */
	rc = arg->func(&arg->xdr, layout);
exit:
	kfree(layout);
	kfree(fhp);
	return rc;

error:
	arg->seg.length = 0;
	goto exit;
}

static int gfs2_layout_commit(struct inode *inode,
			      struct nfsd4_pnfs_layoutcommit *p)
{
	printk(KERN_DEBUG "%s: LAYOUT_COMMIT (unimplemented)\n", __func__);

	return 0;
}

static int gfs2_layout_return(struct inode *inode,
			      struct nfsd4_pnfs_layoutreturn *p)
{
	printk(KERN_DEBUG "%s: LAYOUT_RETURN (unimplemented)\n", __func__);

	return 0;
}

static int gfs2_get_device_iter(struct super_block *sb,
				struct pnfs_deviter_arg *arg)
{
	if (arg->type != LAYOUT_NFSV4_FILES) {
		printk(KERN_ERR "%s: ERROR: layout type isn't 'file' "
			"(type: %x)\n", __func__, arg->type);
		return -ENOTSUPP;
	}

	if (arg->cookie == 0) {
		arg->cookie = 1;
		arg->verf = 1;
		arg->devid = 1;
	} else
		arg->eof = 1;

	return 0;
}

static int gfs2_get_device_info(struct super_block *sb,
				struct pnfs_devinfo_arg *arg)
{
	int err, len, i = 0;
	struct pnfs_filelayout_device fdev;
	struct pnfs_filelayout_devaddr *daddr;
	char *ds_buf, *bufp, *bufp2;

	memset(&fdev, '\0', sizeof(fdev));
	if (arg->type != LAYOUT_NFSV4_FILES) {
		printk(KERN_ERR "%s: ERROR: layout type isn't 'file' "
			"(type: %x)\n", __func__, arg->type);
		err = -ENOTSUPP;
		goto out;
	}

	if (arg->devid.pnfs_devid != 1) {
		printk(KERN_DEBUG "%s: WARNING: didn't receive a deviceid of "
			"1 (got: 0x%llx)\n", __func__, arg->devid.pnfs_devid);
		err = -EINVAL;
		goto out;
	}

	/* XXX: no notifications yet */
	arg->notify_types = 0;

	printk(KERN_DEBUG "%s: DEBUG: current entire DS list is |%s|\n",
		__func__, pnfs_ds_list);
	if (!*pnfs_ds_list) {
		printk(KERN_ERR "%s: ERROR: pnfs_ds_list has no entries!\n",
			__func__);
		err = -EIO;
		goto out;
	}

	err = -ENOMEM;
	len = strlen(pnfs_ds_list) + 1;
	ds_buf = kmalloc(len, GFP_KERNEL);
	if (!ds_buf)
		goto out;
	memcpy(ds_buf, pnfs_ds_list, len);
	bufp = bufp2 = ds_buf;

	/* count the number of comma-delimited DS IPs */
	fdev.fl_device_length = 1;
	while ((bufp = strchr(bufp, ',')) != NULL) {
		fdev.fl_device_length++;
		bufp++;
	}

	len = sizeof(*fdev.fl_device_list) * fdev.fl_device_length;
	fdev.fl_device_list = kzalloc(len, GFP_KERNEL);
	if (!fdev.fl_device_list) {
		printk(KERN_ERR "%s: ERROR: unable to kmalloc a device list "
			"buffer for %d DSes.\n", __func__, i);
		goto out;
	}

	fdev.fl_stripeindices_length = fdev.fl_device_length;
	fdev.fl_stripeindices_list =
		kzalloc(sizeof(u32) * fdev.fl_stripeindices_length, GFP_KERNEL);

	if (!fdev.fl_stripeindices_list) {
		printk(KERN_ERR "%s: ERROR: unable to kmalloc a stripeindices "
			"list buffer for %d DSes.\n", __func__, i);
		goto out;
	}
	for (i = 0; i < fdev.fl_stripeindices_length; i++)
		fdev.fl_stripeindices_list[i] = i;

	for (i = 0; (bufp = strsep(&bufp2, ",")) != NULL; i++) {
		printk(KERN_DEBUG "%s: DEBUG: encoding DS |%s|\n", __func__,
			bufp);

		daddr = kmalloc(sizeof(*daddr), GFP_KERNEL);
		if (!daddr) {
			printk(KERN_ERR "%s: ERROR: unable to kmalloc a device "
				"addr buffer.\n", __func__);
			goto out;
		}

		daddr->r_netid.data = "tcp";
		daddr->r_netid.len = 3;
		len = strlen(bufp);
		daddr->r_addr.data = kmalloc(len + 4, GFP_KERNEL);
		memcpy(daddr->r_addr.data, bufp, len);
		/*
		 * append the port number.  interpreted as two more bytes
		 * beyond the quad: ".8.1" -> 0x08.0x01 -> 0x0801 = port 2049.
		 */
		memcpy(daddr->r_addr.data + len, ".8.1", 4);
		daddr->r_addr.len = len + 4;

		fdev.fl_device_list[i].fl_multipath_length = 1;
		fdev.fl_device_list[i].fl_multipath_list = daddr;
	}

	/* have nfsd encode the device info */
	err = arg->func(&arg->xdr, &fdev);
out:
	for (i = 0; i < fdev.fl_device_length; i++)
		kfree(fdev.fl_device_list[i].fl_multipath_list);
	kfree(fdev.fl_device_list);
	kfree(fdev.fl_stripeindices_list);
	return err;
}

#endif /* CONFIG_PNFSD */

#if defined(CONFIG_PNFSD)
const struct pnfs_export_operations gfs2_pnfs_ops = {
	.layout_type = gfs2_layout_type,
	.layout_get = gfs2_layout_get,
	.layout_commit = gfs2_layout_commit,
	.layout_return = gfs2_layout_return,
	.get_device_iter = gfs2_get_device_iter,
	.get_device_info = gfs2_get_device_info,
};
#endif /* CONFIG_PNFSD */

const struct export_operations gfs2_export_ops = {
	.encode_fh = gfs2_encode_fh,
	.fh_to_dentry = gfs2_fh_to_dentry,
	.fh_to_parent = gfs2_fh_to_parent,
	.get_name = gfs2_get_name,
	.get_parent = gfs2_get_parent,
};

