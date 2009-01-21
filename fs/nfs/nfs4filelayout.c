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

#ifdef CONFIG_PNFS

#include <linux/module.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
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

/* Initialize a mountpoint by retrieving the list of
 * available devices for it.
 * Return the pnfs_mount_type structure so the
 * pNFS_client can refer to the mount point later on
 */
struct pnfs_mount_type*
filelayout_initialize_mountpoint(struct super_block *sb, struct nfs_fh *fh)
{
	struct filelayout_mount_type *fl_mt;
	struct pnfs_mount_type *mt;
	struct pnfs_devicelist *dlist;
	int status;

	dlist = kmalloc(sizeof(struct pnfs_devicelist), GFP_KERNEL);
	if (!dlist)
		goto error_ret;

	fl_mt = kmalloc(sizeof(struct filelayout_mount_type), GFP_KERNEL);
	if (!fl_mt)
		goto cleanup_dlist;

	/* Initialize nfs4 file layout specific device list structure */
	fl_mt->hlist = kmalloc(sizeof(struct nfs4_pnfs_dev_hlist), GFP_KERNEL);
	if (!fl_mt->hlist)
		goto cleanup_fl_mt;

	mt = kmalloc(sizeof(struct pnfs_mount_type), GFP_KERNEL);
	if (!mt)
		goto cleanup_fl_mt;

	fl_mt->fl_sb = sb;
	mt->mountid = (void *)fl_mt;

	/* Retrieve device list from server */
	status = pnfs_callback_ops->nfs_getdevicelist(sb, fh, dlist);
	if (status)
		goto cleanup_mt;

	status = nfs4_pnfs_devlist_init(fl_mt->hlist);
	if (status)
		goto cleanup_mt;

	/* Retrieve and add all available devices */
	status = process_deviceid_list(fl_mt, fh, dlist);
	if (status)
		goto cleanup_mt;

	kfree(dlist);
	dprintk("%s: device list has been initialized successfully\n",
		__func__);
	return mt;

cleanup_mt: ;
	kfree(mt);

cleanup_fl_mt: ;
	kfree(fl_mt->hlist);
	kfree(fl_mt);

cleanup_dlist: ;
	kfree(dlist);

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
struct pnfs_layout_type*
filelayout_alloc_layout(struct pnfs_mount_type *mountid, struct inode *inode)
{
	dprintk("NFS_FILELAYOUT: allocating layout\n");
	return kzalloc(sizeof(struct pnfs_layout_type) +
		       sizeof(struct nfs4_filelayout), GFP_KERNEL);
}

/* Free a filelayout layout structure
 */
void
filelayout_free_layout(struct pnfs_layout_type *layoutid)
{
	dprintk("NFS_FILELAYOUT: freeing layout\n");
	kfree(layoutid);
}

/*
 * filelayout_check_layout()
 *
 * Make sure layout segment parameters are sane WRT the device.
 *
 * Notes:
 * 1) current code insists that # stripe index = # multipath devices which
 *    is wrong.
 * 2) pattern_offset is ignored and must == 0 which is wrong;
 * 3) the pattern_offset needs to be a mutliple of the stripe unit.
 * 4) stripe unit is multiple of page size
*/

static int
filelayout_check_layout(struct pnfs_layout_type *lo,
			struct pnfs_layout_segment *lseg)
{
	struct nfs4_filelayout_segment *fl = LSEG_LD_DATA(lseg);
	struct nfs4_file_layout_dsaddr *dsaddr;
	int status = -EINVAL;
	struct nfs_server *nfss = NFS_SERVER(PNFS_INODE(lo));

	dprintk("--> %s\n", __func__);
	dsaddr = nfs4_file_layout_dsaddr_get(FILE_MT(lo->inode), &fl->dev_id);
	if (dsaddr == NULL) {
		dprintk("%s NO device for dev_id %s\n",
				__func__, deviceid_fmt(&fl->dev_id));
		goto out;
	}
	if (fl->first_stripe_index < 0 ||
	    fl->first_stripe_index > dsaddr->stripe_count) {
		dprintk("%s Bad first_stripe_index %d\n",
				__func__, fl->first_stripe_index);
		goto out;
	}

	if (fl->pattern_offset != 0) {
		dprintk("%s Unsupported no-zero pattern_offset %Ld\n",
				__func__, fl->pattern_offset);
		goto out;
	}

	if (fl->stripe_unit % PAGE_SIZE) {
		dprintk("%s Stripe unit (%u) not page aligned\n",
			__func__, fl->stripe_unit);
		goto out;
	}

	/* XXX only support SPARSE packing. Don't support use MDS open fh */
	if (!(fl->num_fh == 1 || fl->num_fh == dsaddr->multipath_count)) {
		dprintk("%s num_fh %u not equal to 1 or multipath_count %u\n",
			__func__, fl->num_fh, dsaddr->multipath_count);
		goto out;
	}

	if (fl->stripe_unit % nfss->ds_rsize || fl->stripe_unit % nfss->ds_wsize) {
		dprintk("%s Stripe unit (%u) not aligned with rsize %u wsize %u\n",
			__func__, fl->stripe_unit, nfss->ds_rsize, nfss->ds_wsize);
	}
	status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
}

static void filelayout_free_lseg(struct pnfs_layout_segment *lseg);

/* Decode layout and store in layoutid.  Overwrite any existing layout
 * information for this file.
 */
static void
filelayout_set_layout(struct nfs4_filelayout *flo,
		      struct nfs4_filelayout_segment *fl,
		      struct nfs4_pnfs_layoutget_res *lgr)
{
	int i;
	uint32_t *p = (uint32_t *)lgr->layout.buf;
	uint32_t nfl_util;

	dprintk("%s: set_layout_map Begin\n", __func__);

	COPYMEM(&fl->dev_id, NFS4_PNFS_DEVICEID4_SIZE);
	READ32(nfl_util);
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

	READ32(fl->first_stripe_index);
	READ64(fl->pattern_offset);
	READ32(fl->num_fh);

	dprintk("%s: nfl_util 0x%X num_fh %u fsi %u po %llu dev_id %s\n",
		__func__, nfl_util, fl->num_fh, fl->first_stripe_index,
		fl->pattern_offset, deviceid_fmt(&fl->dev_id));

	for (i = 0; i < fl->num_fh; i++) {
		/* fh */
		memset(&fl->fh_array[i], 0, sizeof(struct nfs_fh));
		READ32(fl->fh_array[i].size);
		COPYMEM(fl->fh_array[i].data, fl->fh_array[i].size);
		dprintk("DEBUG: %s: fh len %d\n", __func__,
					fl->fh_array[i].size);
	}
}

static struct pnfs_layout_segment *
filelayout_alloc_lseg(struct pnfs_layout_type *layoutid,
		      struct nfs4_pnfs_layoutget_res *lgr)
{
	struct nfs4_filelayout *flo = PNFS_LD_DATA(layoutid);
	struct pnfs_layout_segment *lseg;

	lseg = kzalloc(sizeof(struct pnfs_layout_segment) +
		       sizeof(struct nfs4_filelayout_segment), GFP_KERNEL);
	if (!lseg)
		return NULL;

	filelayout_set_layout(flo, LSEG_LD_DATA(lseg), lgr);
	if (filelayout_check_layout(layoutid, lseg)) {
		filelayout_free_lseg(lseg);
		lseg = NULL;
	}
	return lseg;
}

static void
filelayout_free_lseg(struct pnfs_layout_segment *lseg)
{
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

#endif /* CONFIG_PNFS */
