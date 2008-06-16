/*
 * linux/fs/nfsd/pnfs_lexp.c
 *
 * pNFS export of local filesystems.
 *
 * Export local file systems over the files layout type.
 * The MDS (metadata server) functions also as a single DS (data server).
 * This is mostly useful for development and debugging purposes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Copyright (C) 2008 Benny Halevy, <bhalevy@panasas.com>
 *
 * Initial implementation was based on the pnfs-gfs2 patches done
 * by David M. Richter <richterd@citi.umich.edu>
 */

#include <linux/sunrpc/svc_xprt.h>
#include <linux/nfsd/nfs4layoutxdr.h>

#include "pnfsd.h"

#define NFSDDBG_FACILITY NFSDDBG_PNFS

struct sockaddr pnfsd_lexp_addr;
size_t pnfs_lexp_addr_len;

static int
pnfsd_lexp_layout_type(struct super_block *sb)
{
	int ret = LAYOUT_NFSV4_1_FILES;
	dprintk("<-- %s: return %d\n", __func__, ret);
	return ret;
}

static int
pnfsd_lexp_get_device_iter(struct super_block *sb,
			   u32 layout_type,
			   struct nfsd4_pnfs_dev_iter_res *res)
{
	dprintk("--> %s: sb=%p\n", __func__, sb);

	BUG_ON(layout_type != LAYOUT_NFSV4_1_FILES);

	res->gd_eof = 1;
	if (res->gd_cookie)
		return -ENOENT;
	res->gd_cookie = 1;
	res->gd_verf = 1;
	res->gd_devid = 1;

	dprintk("<-- %s: return 0\n", __func__);
	return 0;
}

static int
pnfsd_lexp_get_device_info(struct super_block *sb,
			   struct exp_xdr_stream *xdr,
			   u32 layout_type,
			   const struct nfsd4_pnfs_deviceid *devid)
{
	int err;
	struct pnfs_filelayout_device fdev;
	struct pnfs_filelayout_multipath fl_devices[1];
	u32 fl_stripe_indices[1] = { 0 };
	struct pnfs_filelayout_devaddr daddr;
	/* %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x.%03u.%03u */
	char daddr_buf[8*4 + 2*3 + 10];

	dprintk("--> %s: sb=%p\n", __func__, sb);

	BUG_ON(layout_type != LAYOUT_NFSV4_1_FILES);

	memset(&fdev, '\0', sizeof(fdev));

	if (devid->devid != 1) {
		printk(KERN_ERR "%s: WARNING: didn't receive a deviceid of 1 "
			"(got: 0x%llx)\n", __func__, devid->devid);
		err = -EINVAL;
		goto out;
	}

	/* count the number of comma-delimited DS IPs */
	fdev.fl_device_length = 1;
	fdev.fl_device_list = fl_devices;

	fdev.fl_stripeindices_length = fdev.fl_device_length;
	fdev.fl_stripeindices_list = fl_stripe_indices;

	daddr.r_addr.data = daddr_buf;
	daddr.r_addr.len = sizeof(daddr_buf);
	err = __svc_print_netaddr(&pnfsd_lexp_addr, &daddr.r_addr);
	if (err < 0)
		goto out;
	daddr.r_addr.len = err;
	switch (pnfsd_lexp_addr.sa_family) {
	case AF_INET:
		daddr.r_netid.data = "tcp";
		daddr.r_netid.len = 3;
		break;
	case AF_INET6:
		daddr.r_netid.data = "tcp6";
		daddr.r_netid.len = 4;
		break;
	default:
		BUG();
	}
	fdev.fl_device_list[0].fl_multipath_length = 1;
	fdev.fl_device_list[0].fl_multipath_list = &daddr;

	/* have nfsd encode the device info */
	err = filelayout_encode_devinfo(xdr, &fdev);
out:
	dprintk("<-- %s: return %d\n", __func__, err);
	return err;
}

static int get_stripe_unit(int blocksize)
{
	if (blocksize < NFSSVC_MAXBLKSIZE)
		blocksize = NFSSVC_MAXBLKSIZE - (NFSSVC_MAXBLKSIZE % blocksize);
	dprintk("%s: return %d\n", __func__, blocksize);
	return blocksize;
}

static enum nfsstat4
pnfsd_lexp_layout_get(struct inode *inode,
		      struct exp_xdr_stream *xdr,
		      const struct nfsd4_pnfs_layoutget_arg *arg,
		      struct nfsd4_pnfs_layoutget_res *res)
{
	enum nfsstat4 rc = NFS4_OK;
	struct pnfs_filelayout_layout *layout = NULL;
	struct knfsd_fh *fhp = NULL;

	dprintk("--> %s: inode=%p\n", __func__, inode);

	res->lg_seg.layout_type = LAYOUT_NFSV4_1_FILES;
	res->lg_seg.offset = 0;
	res->lg_seg.length = NFS4_MAX_UINT64;

	layout = kzalloc(sizeof(*layout), GFP_KERNEL);
	if (layout == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	/* Set file layout response args */
	layout->lg_layout_type = LAYOUT_NFSV4_1_FILES;
	layout->lg_stripe_type = STRIPE_SPARSE;
	layout->lg_commit_through_mds = true;
	layout->lg_stripe_unit = get_stripe_unit(inode->i_sb->s_blocksize);
	layout->lg_fh_length = 1;
	layout->device_id.sbid = arg->lg_sbid;
	layout->device_id.devid = 1;				/*FSFTEMP*/
	layout->lg_first_stripe_index = 0;			/*FSFTEMP*/
	layout->lg_pattern_offset = 0;

	fhp = kmalloc(sizeof(*fhp), GFP_KERNEL);
	if (fhp == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	memcpy(fhp, arg->lg_fh, sizeof(*fhp));
	pnfs_fh_mark_ds(fhp);
	layout->lg_fh_list = fhp;

	/* Call nfsd to encode layout */
	rc = filelayout_encode_layout(xdr, layout);
exit:
	kfree(layout);
	kfree(fhp);
	dprintk("<-- %s: return %d\n", __func__, rc);
	return rc;

error:
	res->lg_seg.length = 0;
	goto exit;
}
