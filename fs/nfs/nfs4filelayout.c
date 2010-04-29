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

/* Forward declaration */
ssize_t filelayout_get_stripesize(struct pnfs_layout_type *);
struct layoutdriver_io_operations filelayout_io_operations;

int
filelayout_initialize_mountpoint(struct nfs_client *clp)
{

	if (nfs4_alloc_init_deviceid_cache(clp,
					   nfs4_fl_free_deviceid_callback)) {
		printk(KERN_WARNING "%s: deviceid cache could not be "
			"initialized\n", __func__);
		return 0;
	}
	dprintk("%s: deviceid cache has been initialized successfully\n",
		__func__);
	return 1;
}

/* Uninitialize a mountpoint by destroying its device list.
 */
int
filelayout_uninitialize_mountpoint(struct nfs_server *nfss)
{
	dprintk("--> %s\n", __func__);

	if (nfss->pnfs_curr_ld && nfss->nfs_client->cl_devid_cache)
		nfs4_put_deviceid_cache(nfss->nfs_client);
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

/* Call ops for the async read/write cases
 * In the case of dense layouts, the offset needs to be reset to its
 * original value.
 */
static void filelayout_read_call_done(struct rpc_task *task, void *data)
{
	struct nfs_read_data *rdata = (struct nfs_read_data *)data;

	if (rdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n",
			__func__, rdata->args.offset, rdata->fldata.orig_offset);
		rdata->args.offset = rdata->fldata.orig_offset;
	}

	pnfs_callback_ops->nfs_readlist_complete(rdata);
}

static void filelayout_write_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	if (wdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n",
			__func__, wdata->args.offset, wdata->fldata.orig_offset);
		wdata->args.offset = wdata->fldata.orig_offset;
	}

	pnfs_callback_ops->nfs_writelist_complete(wdata);
}

struct rpc_call_ops filelayout_read_call_ops = {
	.rpc_call_prepare = nfs_read_prepare,
	.rpc_call_done = filelayout_read_call_done,
};

struct rpc_call_ops filelayout_write_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_write_call_done,
};

/* Perform sync or async reads.
 *
 * An optimization for the NFS file layout driver
 * allows the original read/write data structs to be passed in the
 * last argument.
 *
 * TODO: join with write_pagelist?
 */
static enum pnfs_try_status
filelayout_read_pagelist(struct pnfs_layout_type *layoutid,
			 struct page **pages,
			 unsigned int pgbase,
			 unsigned nr_pages,
			 loff_t offset,
			 size_t count,
			 struct nfs_read_data *data)
{
	struct inode *inode = PNFS_INODE(layoutid);
	struct nfs4_filelayout_segment *flseg;
	struct nfs4_pnfs_dserver dserver;
	int status;

	dprintk("--> %s ino %lu nr_pages %d pgbase %u req %Zu@%llu\n",
		__func__, inode->i_ino, nr_pages, pgbase, count, offset);

	flseg = LSEG_LD_DATA(data->pdata.lseg);

	/* Retrieve the correct rpc_client for the byte range */
	status = nfs4_pnfs_dserver_get(data->pdata.lseg,
				       offset,
				       count,
				       &dserver);
	if (status) {
		printk(KERN_ERR "%s: dserver get failed status %d use MDS\n",
		       __func__, status);
		return PNFS_NOT_ATTEMPTED;
	}

	dprintk("%s USE DS:ip %x %s\n", __func__,
		htonl(dserver.ds->ds_ip_addr), dserver.ds->r_addr);

	/* just try the first data server for the index..*/
	data->fldata.pnfs_client = dserver.ds->ds_clp->cl_rpcclient;
	data->fldata.ds_nfs_client = dserver.ds->ds_clp;
	data->args.fh = dserver.fh;

	/* Now get the file offset on the dserver
	 * Set the read offset to this offset, and
	 * save the original offset in orig_offset
	 * In the case of aync reads, the offset will be reset in the
	 * call_ops->rpc_call_done() routine.
	 */
	data->args.offset = filelayout_get_dserver_offset(offset,
							  flseg);
	data->fldata.orig_offset = offset;

	/* Perform an asynchronous read */
	nfs_initiate_read(data, data->fldata.pnfs_client,
			  &filelayout_read_call_ops);

	data->pdata.pnfs_error = 0;

	return PNFS_ATTEMPTED;
}

/* Perform async writes. */
static enum pnfs_try_status
filelayout_write_pagelist(struct pnfs_layout_type *layoutid,
			  struct page **pages,
			  unsigned int pgbase,
			  unsigned nr_pages,
			  loff_t offset,
			  size_t count,
			  int sync,
			  struct nfs_write_data *data)
{
	struct inode *inode = PNFS_INODE(layoutid);
	struct nfs4_filelayout_segment *flseg = LSEG_LD_DATA(data->pdata.lseg);
	struct nfs4_pnfs_dserver dserver;
	int status;

	dprintk("--> %s ino %lu nr_pages %d pgbase %u req %Zu@%llu sync %d\n",
		__func__, inode->i_ino, nr_pages, pgbase, count, offset, sync);

	/* Retrieve the correct rpc_client for the byte range */
	status = nfs4_pnfs_dserver_get(data->pdata.lseg,
				       offset,
				       count,
				       &dserver);

	if (status) {
		printk(KERN_ERR "%s: dserver get failed status %d use MDS\n",
		       __func__, status);
		return PNFS_NOT_ATTEMPTED;
	}

	dprintk("%s ino %lu %Zu@%llu DS:%x:%hu %s\n",
		__func__, inode->i_ino, count, offset,
		htonl(dserver.ds->ds_ip_addr), ntohs(dserver.ds->ds_port),
		dserver.ds->r_addr);

	data->fldata.pnfs_client = dserver.ds->ds_clp->cl_rpcclient;
	data->fldata.ds_nfs_client = dserver.ds->ds_clp;
	data->args.fh = dserver.fh;

	/* Get the file offset on the dserver. Set the write offset to
	 * this offset and save the original offset.
	 */
	data->args.offset = filelayout_get_dserver_offset(offset, flseg);
	data->fldata.orig_offset = offset;

	/* Perform an asynchronous write The offset will be reset in the
	 * call_ops->rpc_call_done() routine
	 */
	nfs_initiate_write(data, data->fldata.pnfs_client,
			   &filelayout_write_call_ops, sync);

	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;
}

/* Create a filelayout layout structure and return it.  The pNFS client
 * will use the pnfs_layout_type type to refer to the layout for this
 * inode from now on.
 */
static void *
filelayout_alloc_layout(struct inode *inode)
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

/*
 * filelayout_check_layout()
 *
 * Make sure layout segment parameters are sane WRT the device.
 *
 * Notes:
 * 1) current code insists that # stripe index = # data servers in ds_list
 *    which is wrong.
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
	dsaddr = nfs4_pnfs_device_item_find(nfss->nfs_client, &fl->dev_id);
	if (dsaddr == NULL) {
		dsaddr = get_device_info(PNFS_INODE(lo), &fl->dev_id);
		if (dsaddr == NULL) {
			dprintk("%s NO device for dev_id %s\n",
				__func__, deviceid_fmt(&fl->dev_id));
			goto out;
		}
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
	if (!(fl->num_fh == 1 || fl->num_fh == dsaddr->ds_num)) {
		dprintk("%s num_fh %u not equal to 1 or ds_num %u\n",
			__func__, fl->num_fh, dsaddr->ds_num);
		goto out;
	}

	if (fl->stripe_unit % nfss->rsize || fl->stripe_unit % nfss->wsize) {
		dprintk("%s Stripe unit (%u) not aligned with rsize %u "
			"wsize %u\n", __func__, fl->stripe_unit, nfss->rsize,
			nfss->wsize);
	}

	/* reference the device */
	nfs4_set_layout_deviceid(lseg, &dsaddr->deviceid);

	status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
}

static void _filelayout_free_lseg(struct pnfs_layout_segment *lseg);
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

	dprintk("--> %s\n", __func__);
	lseg = kzalloc(sizeof(struct pnfs_layout_segment) +
		       sizeof(struct nfs4_filelayout_segment), GFP_KERNEL);
	if (!lseg)
		return NULL;

	rc = filelayout_set_layout(flo, LSEG_LD_DATA(lseg), lgr);

	if (rc != 0 || filelayout_check_layout(layoutid, lseg)) {
		_filelayout_free_lseg(lseg);
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
_filelayout_free_lseg(struct pnfs_layout_segment *lseg)
{
	filelayout_free_fh_array(LSEG_LD_DATA(lseg));
	kfree(lseg);
}

static void
filelayout_free_lseg(struct pnfs_layout_segment *lseg)
{
	dprintk("--> %s\n", __func__);
	nfs4_unset_layout_deviceid(lseg, lseg->deviceid,
				   nfs4_fl_free_deviceid_callback);
	_filelayout_free_lseg(lseg);
}

/*
 * Allocate a new nfs_write_data struct and initialize
 */
static struct nfs_write_data *
filelayout_clone_write_data(struct nfs_write_data *old)
{
	static struct nfs_write_data *new;

	new = nfs_commitdata_alloc();
	if (!new)
		goto out;
	new->inode       = old->inode;
	new->cred        = old->cred;
	new->args.offset = 0;
	new->args.count  = 0;
	new->res.count   = 0;
	new->res.fattr   = &new->fattr;
	nfs_fattr_init(&new->fattr);
	new->res.verf    = &new->verf;
	new->args.context = get_nfs_open_context(old->args.context);
	new->pdata.lseg = old->pdata.lseg;
	kref_get(&new->pdata.lseg->kref);
	new->pdata.call_ops = old->pdata.call_ops;
	new->pdata.how = old->pdata.how;
out:
	return new;
}

static void filelayout_commit_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	pnfs_callback_ops->nfs_commit_complete(wdata);
}

static struct rpc_call_ops filelayout_commit_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_commit_call_done,
};

/*
 * Execute a COMMIT op to the MDS or to each data server on which a page
 * in 'pages' exists.
 * Invoke the pnfs_commit_complete callback.
 */
enum pnfs_try_status
filelayout_commit(struct pnfs_layout_type *layoutid, int sync,
		  struct nfs_write_data *data)
{
	struct nfs4_filelayout_segment *nfslay;
	struct nfs_write_data   *dsdata = NULL;
	struct nfs4_pnfs_dserver dserver;
	struct nfs4_pnfs_ds *ds;
	struct nfs_page *req, *reqt;
	struct list_head *pos, *tmp, head, head2;
	loff_t file_offset, comp_offset;
	size_t stripesz, cbytes;
	int status;
	enum pnfs_try_status trypnfs = PNFS_ATTEMPTED;
	struct nfs4_file_layout_dsaddr *dsaddr;
	u32 idx1, idx2;

	nfslay = LSEG_LD_DATA(data->pdata.lseg);

	dprintk("%s data %p pnfs_client %p nfslay %p sync %d\n",
		__func__, data, data->fldata.pnfs_client, nfslay, sync);

	data->fldata.commit_through_mds = nfslay->commit_through_mds;
	if (nfslay->commit_through_mds) {
		dprintk("%s data %p commit through mds\n", __func__, data);
		return PNFS_NOT_ATTEMPTED;
	}

	stripesz = filelayout_get_stripesize(layoutid);
	dprintk("%s stripesize %Zd\n", __func__, stripesz);

	dsaddr = container_of(data->pdata.lseg->deviceid,
			      struct nfs4_file_layout_dsaddr, deviceid);

	INIT_LIST_HEAD(&head);
	INIT_LIST_HEAD(&head2);
	list_add(&head, &data->pages);
	list_del_init(&data->pages);

	/* COMMIT to each Data Server */
	while (!list_empty(&head)) {
		cbytes = 0;
		req = nfs_list_entry(head.next);

		file_offset = (loff_t)req->wb_index << PAGE_CACHE_SHIFT;

		/* Get dserver for the current page */
		status = nfs4_pnfs_dserver_get(data->pdata.lseg,
					       file_offset,
					       req->wb_bytes,
					       &dserver);
		if (status) {
			data->pdata.pnfs_error = -EIO;
			goto err_rewind;
		}

		/* Get its index */
		idx1 = filelayout_dserver_get_index(file_offset, dsaddr,
						    nfslay);

		/* Gather all pages going to the current data server by
		 * comparing their indices.
		 * XXX: This recalculates the indices unecessarily.
		 *      One idea would be to calc the index for every page
		 *      and then compare if they are the same. */
		list_for_each_safe(pos, tmp, &head) {
			reqt = nfs_list_entry(pos);
			comp_offset = (loff_t)reqt->wb_index << PAGE_CACHE_SHIFT;
			idx2 = filelayout_dserver_get_index(comp_offset,
							    dsaddr, nfslay);
			if (idx1 == idx2) {
				nfs_list_remove_request(reqt);
				nfs_list_add_request(reqt, &head2);
				cbytes += reqt->wb_bytes;
			}
		}

		if (!list_empty(&head)) {
			dsdata = filelayout_clone_write_data(data);
			if (!dsdata) {
				/* return pages back to head */
				list_splice(&head2, &head);
				INIT_LIST_HEAD(&head2);
				data->pdata.pnfs_error = -ENOMEM;
				goto err_rewind;
			}
		} else {
			dsdata = data;
		}

		list_add(&dsdata->pages, &head2);
		list_del_init(&head2);

		ds = dserver.ds;
		dsdata->fldata.pnfs_client = ds->ds_clp->cl_rpcclient;
		dsdata->fldata.ds_nfs_client = ds->ds_clp;
		dsdata->args.fh = dserver.fh;

		dprintk("%s: Initiating commit: %Zu@%llu USE DS:\n",
			__func__, cbytes, file_offset);
		print_ds(ds);

		/* Send COMMIT to data server */
		nfs_initiate_commit(dsdata, dsdata->fldata.pnfs_client,
				    &filelayout_commit_call_ops, sync);
	}

out:
	if (data->pdata.pnfs_error)
		printk(KERN_ERR "%s: ERROR %d\n", __func__,
		       data->pdata.pnfs_error);

	/* XXX should we send COMMIT to MDS e.g. not free data and return 1 ? */
	return trypnfs;
err_rewind:
	/* put remaining pages back onto the original data->pages */
	list_add(&data->pages, &head);
	list_del_init(&head);
	trypnfs = PNFS_NOT_ATTEMPTED;
	goto out;
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

	if (pgio->pg_bsize != NFS_SERVER(pgio->pg_inode)->wsize &&
	    pgio->pg_count > pgio->pg_threshold)
		pgio->pg_bsize = NFS_SERVER(pgio->pg_inode)->wsize;

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

#if 0
	dprintk("%s p %llu r %llu bnd %d bsize %Zu\n", __func__,
		p_stripe, r_stripe, pgio->pg_boundary, pgio->pg_bsize);
#endif

	return (p_stripe == r_stripe);
}

ssize_t
filelayout_get_io_threshold(struct pnfs_layout_type *layoutid,
			    struct inode *inode)
{
	return -1;
}

struct layoutdriver_io_operations filelayout_io_operations = {
	.commit                  = filelayout_commit,
	.read_pagelist           = filelayout_read_pagelist,
	.write_pagelist          = filelayout_write_pagelist,
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
