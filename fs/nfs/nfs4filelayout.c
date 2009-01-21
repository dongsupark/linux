/*
 *  linux/fs/nfs/nfs4filelayout.c
 *
 *  Module for the pnfs nfs4 file layout driver.
 *  Defines all I/O and Policy interface operations, plus code
 *  to register itself with the pNFS client.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand <dhildebz@eecs.umich.edu>
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
 *  3. Neither the name of the University nor the names of its
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
#include <linux/init.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/pnfs_xdr.h>
#include <linux/nfs4_pnfs.h>

#include "nfs4filelayout.h"
#include "nfs4_fs.h"
#include "internal.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dean Hildebrand <dhildebz@eecs.umich.edu>");
MODULE_DESCRIPTION("The NFSv4 file layout driver");

/* Callback operations to the pNFS client */
struct pnfs_client_operations *pnfs_callback_ops;

/* Initialize and return the pnfs_mount_type structure so the
 * pNFS_client can refer to the mount point later on
 */
struct pnfs_mount_type*
filelayout_initialize_mountpoint(struct super_block *sb, struct nfs_fh *fh)
{
	struct filelayout_mount_type *fl_mt;
	struct pnfs_mount_type *mt;
	int status;

	fl_mt = kmalloc(sizeof(struct filelayout_mount_type), GFP_KERNEL);
	if (!fl_mt)
		goto error_ret;

	/* Initialize nfs4 file layout specific device list structure */
	fl_mt->hlist = kmalloc(sizeof(struct nfs4_pnfs_dev_hlist), GFP_KERNEL);
	if (!fl_mt->hlist)
		goto cleanup_fl_mt;

	mt = kmalloc(sizeof(struct pnfs_mount_type), GFP_KERNEL);
	if (!mt)
		goto cleanup_fl_mt;

	fl_mt->fl_sb = sb;
	mt->mountid = (void *)fl_mt;

	status = nfs4_pnfs_devlist_init(fl_mt->hlist);
	if (status)
		goto cleanup_mt;

	dprintk("%s: device list has been initialized successfully\n",
		__func__);
	return mt;

cleanup_mt: ;
	kfree(mt);

cleanup_fl_mt: ;
	kfree(fl_mt->hlist);
	kfree(fl_mt);

error_ret: ;
	printk(KERN_WARNING "%s: device list could not be initialized\n",
		__func__);

	return NULL;
}

/* Uninitialize a mountpoint by destroying its device list.
 */
int
filelayout_uninitialize_mountpoint(struct pnfs_mount_type *mountid)
{
	struct filelayout_mount_type *fl_mt = NULL;

	if (mountid) {
		fl_mt = (struct filelayout_mount_type *)mountid->mountid;

		if (fl_mt != NULL) {
			nfs4_pnfs_devlist_destroy(fl_mt->hlist);
			kfree(fl_mt);
		}
		kfree(mountid);
	}
	return 0;
}

/* This function is used by the layout driver to calculate the
 * offset of the file on the dserver based on whether the
 * layout type is STRIPE_DENSE or STRIPE_SPARSE
 */
loff_t
filelayout_get_dserver_offset(loff_t offset,
			      struct nfs4_filelayout_segment *layout)
{
	if (!layout)
		return offset;

	switch (layout->stripe_type) {
	case STRIPE_SPARSE:
		return offset;

	case STRIPE_DENSE:
	{
		u32 stripe_size;
		u32 stripe_unit;
		loff_t off;
		loff_t tmp;
		u32 stripe_unit_idx;

		stripe_size = layout->stripe_unit * layout->num_fh;
		/* XXX I do this because do_div seems to take a 32 bit dividend */
		stripe_unit = layout->stripe_unit;
		tmp = off = offset;

		do_div(off, stripe_size);
		stripe_unit_idx = do_div(tmp, stripe_unit);

		return off * stripe_unit + stripe_unit_idx;
	}

	default:
		BUG();
	}

	/* We should never get here... just to stop the gcc warning */
	return 0;
}

/* Create a filelayout layout structure and return it.  The pNFS client
 * will use the pnfs_layout_type type to refer to the layout for this
 * inode from now on.
 */
static void *
filelayout_alloc_layout(struct pnfs_mount_type *mountid, struct inode *inode)
{
	dprintk("NFS_FILELAYOUT: allocating layout\n");
	return kzalloc(sizeof(struct nfs4_filelayout), GFP_KERNEL);
}

/* Free a filelayout layout structure
 */
static void
filelayout_free_layout(void *layoutid)
{
	dprintk("NFS_FILELAYOUT: freeing layout\n");
	kfree(layoutid);
}

static void filelayout_free_lseg(struct pnfs_layout_segment *lseg);
static void filelayout_free_fh_array(struct nfs4_filelayout_segment *fl);

/* Decode layout and store in layoutid.  Overwrite any existing layout
 * information for this file.
 */
static int
filelayout_set_layout(struct nfs4_filelayout *flo,
		      struct nfs4_filelayout_segment *fl,
		      struct nfs4_pnfs_layoutget_res *lgr)
{
	uint32_t *p = (uint32_t *)lgr->layout.buf;
	uint32_t nfl_util;
	int i;

	dprintk("%s: set_layout_map Begin\n", __func__);

	memcpy(&fl->dev_id, p, NFS4_PNFS_DEVICEID4_SIZE);
	p += XDR_QUADLEN(NFS4_PNFS_DEVICEID4_SIZE);
	nfl_util = be32_to_cpup(p++);
	if (nfl_util & NFL4_UFLG_COMMIT_THRU_MDS)
		fl->commit_through_mds = 1;
	if (nfl_util & NFL4_UFLG_DENSE)
		fl->stripe_type = STRIPE_DENSE;
	else
		fl->stripe_type = STRIPE_SPARSE;
	fl->stripe_unit = nfl_util & ~NFL4_UFLG_MASK;

	if (!flo->stripe_unit)
		flo->stripe_unit = fl->stripe_unit;
	else if (flo->stripe_unit != fl->stripe_unit) {
		printk(KERN_NOTICE "%s: updating strip_unit from %u to %u\n",
			__func__, flo->stripe_unit, fl->stripe_unit);
		flo->stripe_unit = fl->stripe_unit;
	}

	fl->first_stripe_index = be32_to_cpup(p++);
	p = xdr_decode_hyper(p, &fl->pattern_offset);
	fl->num_fh = be32_to_cpup(p++);

	dprintk("%s: nfl_util 0x%X num_fh %u fsi %u po %llu dev_id %s\n",
		__func__, nfl_util, fl->num_fh, fl->first_stripe_index,
		fl->pattern_offset, deviceid_fmt(&fl->dev_id));

	if (fl->num_fh * sizeof(struct nfs_fh) > 2*PAGE_SIZE) {
		fl->fh_array = vmalloc(fl->num_fh * sizeof(struct nfs_fh));
		if (fl->fh_array)
			memset(fl->fh_array, 0,
				fl->num_fh * sizeof(struct nfs_fh));
	} else {
		fl->fh_array = kzalloc(fl->num_fh * sizeof(struct nfs_fh),
					GFP_KERNEL);
       }
	if (!fl->fh_array)
		return -ENOMEM;

	for (i = 0; i < fl->num_fh; i++) {
		/* fh */
		fl->fh_array[i].size = be32_to_cpup(p++);
		if (sizeof(struct nfs_fh) < fl->fh_array[i].size) {
			printk(KERN_ERR "Too big fh %d received %d\n",
				i, fl->fh_array[i].size);
			/* Layout is now invalid, so pretend it does not
			   exist */
			filelayout_free_fh_array(fl);
			fl->num_fh = 0;
			break;
		}
		memcpy(fl->fh_array[i].data, p, fl->fh_array[i].size);
		p += XDR_QUADLEN(fl->fh_array[i].size);
		dprintk("DEBUG: %s: fh len %d\n", __func__,
					fl->fh_array[i].size);
	}

	return 0;
}

static struct pnfs_layout_segment *
filelayout_alloc_lseg(struct pnfs_layout_type *layoutid,
		      struct nfs4_pnfs_layoutget_res *lgr)
{
	struct nfs4_filelayout *flo = PNFS_LD_DATA(layoutid);
	struct pnfs_layout_segment *lseg;
	int rc;

	lseg = kzalloc(sizeof(struct pnfs_layout_segment) +
		       sizeof(struct nfs4_filelayout_segment), GFP_KERNEL);
	if (!lseg)
		return NULL;

	rc = filelayout_set_layout(flo, LSEG_LD_DATA(lseg), lgr);

	if (rc != 0) {
		filelayout_free_lseg(lseg);
		lseg = NULL;
	}
	return lseg;
}

static void filelayout_free_fh_array(struct nfs4_filelayout_segment *fl)
{
	if (fl->num_fh * sizeof(struct nfs_fh) > 2*PAGE_SIZE)
		vfree(fl->fh_array);
	else
		kfree(fl->fh_array);

	fl->fh_array = NULL;
}

static void
filelayout_free_lseg(struct pnfs_layout_segment *lseg)
{
	filelayout_free_fh_array(LSEG_LD_DATA(lseg));
	kfree(lseg);
}

/* Return the stripesize for the specified file.
 */
ssize_t
filelayout_get_stripesize(struct pnfs_layout_type *layoutid)
{
	struct nfs4_filelayout *flo = PNFS_LD_DATA(layoutid);

	return flo->stripe_unit;
}

/*
 * filelayout_pg_test(). Called by nfs_can_coalesce_requests()
 *
 * For writes which come from the flush daemon, set the bsize on the fly.
 * reads set the bsize in pnfs_pageio_init_read.
 *
 * return 1 :  coalesce page
 * return 0 :  don't coalesce page
 */
int
filelayout_pg_test(struct nfs_pageio_descriptor *pgio, struct nfs_page *prev,
		   struct nfs_page *req)
{
	u64 p_stripe, r_stripe;

	if (!pgio->pg_iswrite)
		goto boundary;

	if (pgio->pg_bsize != NFS_SERVER(pgio->pg_inode)->ds_wsize &&
	    pgio->pg_count > pgio->pg_threshold)
		pgio->pg_bsize = NFS_SERVER(pgio->pg_inode)->ds_wsize;

boundary:
	if (pgio->pg_boundary == 0)
		return 1;
	p_stripe = (u64)prev->wb_index << PAGE_CACHE_SHIFT;
	r_stripe = (u64)req->wb_index << PAGE_CACHE_SHIFT;

#if 0
	dprintk("%s p %llu r %llu \n", __func__, p_stripe, r_stripe);
#endif

	do_div(p_stripe, pgio->pg_boundary);
	do_div(r_stripe, pgio->pg_boundary);

	return (p_stripe == r_stripe);
}

ssize_t
filelayout_get_io_threshold(struct pnfs_layout_type *layoutid,
			    struct inode *inode)
{
	return -1;
}

struct layoutdriver_io_operations filelayout_io_operations = {
	.alloc_layout            = filelayout_alloc_layout,
	.free_layout             = filelayout_free_layout,
	.alloc_lseg              = filelayout_alloc_lseg,
	.free_lseg               = filelayout_free_lseg,
	.initialize_mountpoint   = filelayout_initialize_mountpoint,
	.uninitialize_mountpoint = filelayout_uninitialize_mountpoint,
};

struct layoutdriver_policy_operations filelayout_policy_operations = {
	.flags                 = PNFS_USE_RPC_CODE |
	                         PNFS_LAYOUTGET_ON_OPEN,
	.get_stripesize        = filelayout_get_stripesize,
	.pg_test               = filelayout_pg_test,
	.get_read_threshold    = filelayout_get_io_threshold,
	.get_write_threshold   = filelayout_get_io_threshold,
};

struct pnfs_layoutdriver_type filelayout_type = {
	.id = LAYOUT_NFSV4_FILES,
	.name = "LAYOUT_NFSV4_FILES",
	.ld_io_ops = &filelayout_io_operations,
	.ld_policy_ops = &filelayout_policy_operations,
};

static int __init nfs4filelayout_init(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Registering...\n",
	       __func__);

	/* Need to register file_operations struct with global list to indicate
	* that NFS4 file layout is a possible pNFS I/O module
	*/
	pnfs_callback_ops = pnfs_register_layoutdriver(&filelayout_type);

	return 0;
}

static void __exit nfs4filelayout_exit(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Unregistering...\n",
	       __func__);

	/* Unregister NFS4 file layout driver with pNFS client*/
	pnfs_unregister_layoutdriver(&filelayout_type);
}

module_init(nfs4filelayout_init);
module_exit(nfs4filelayout_exit);
