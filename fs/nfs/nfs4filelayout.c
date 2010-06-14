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
#include <linux/nfs4_pnfs.h>

#include "nfs4filelayout.h"
#include "nfs4_fs.h"
#include "internal.h"
#include "pnfs.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dean Hildebrand <dhildebz@eecs.umich.edu>");
MODULE_DESCRIPTION("The NFSv4 file layout driver");

/* Callback operations to the pNFS client */
struct pnfs_client_operations *pnfs_callback_ops;

/* Forward declaration */
struct layoutdriver_io_operations filelayout_io_operations;

int
filelayout_initialize_mountpoint(struct nfs_client *clp)
{
	int status = nfs4_alloc_init_deviceid_cache(clp,
						nfs4_fl_free_deviceid_callback);
	if (status) {
		printk(KERN_WARNING "%s: deviceid cache could not be "
			"initialized\n", __func__);
		return status;
	}
	dprintk("%s: deviceid cache has been initialized successfully\n",
		__func__);
	return 0;
}

/* Uninitialize a mountpoint by destroying its device list */
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
static loff_t
filelayout_get_dserver_offset(struct pnfs_layout_segment *lseg, loff_t offset)
{
	struct nfs4_filelayout_segment *flseg = LSEG_LD_DATA(lseg);

	switch (flseg->stripe_type) {
	case STRIPE_SPARSE:
		return offset;

	case STRIPE_DENSE:
	{
		u32 stripe_width;
		u64 tmp, off;
		u32 unit = flseg->stripe_unit;

		stripe_width = unit * FILE_DSADDR(lseg)->stripe_count;
		tmp = off = offset - flseg->pattern_offset;
		do_div(tmp, stripe_width);
		return tmp * unit + do_div(off, unit);
	}
	default:
		BUG();
	}

	/* We should never get here... just to stop the gcc warning */
	return 0;
}

/*
 * Call ops for the async read/write cases
 * In the case of dense layouts, the offset needs to be reset to its
 * original value.
 */
static void filelayout_read_call_done(struct rpc_task *task, void *data)
{
	struct nfs_read_data *rdata = (struct nfs_read_data *)data;

	if (rdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n", __func__,
			rdata->args.offset, rdata->fldata.orig_offset);
		rdata->args.offset = rdata->fldata.orig_offset;
	}

	/* Note this may cause RPC to be resent */
	rdata->pdata.call_ops->rpc_call_done(task, data);
}

static void filelayout_read_release(void *data)
{
	struct nfs_read_data *rdata = (struct nfs_read_data *)data;

	put_lseg(rdata->pdata.lseg);
	rdata->pdata.lseg = NULL;
	rdata->pdata.call_ops->rpc_release(data);
}

static void filelayout_write_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	if (wdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n", __func__,
			wdata->args.offset, wdata->fldata.orig_offset);
		wdata->args.offset = wdata->fldata.orig_offset;
	}

	/* Note this may cause RPC to be resent */
	wdata->pdata.call_ops->rpc_call_done(task, data);
}

static void filelayout_write_release(void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	put_lseg(wdata->pdata.lseg);
	wdata->pdata.lseg = NULL;
	wdata->pdata.call_ops->rpc_release(data);
}

struct rpc_call_ops filelayout_read_call_ops = {
	.rpc_call_prepare = nfs_read_prepare,
	.rpc_call_done = filelayout_read_call_done,
	.rpc_release = filelayout_read_release,
};

struct rpc_call_ops filelayout_write_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_write_call_done,
	.rpc_release = filelayout_write_release,
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
filelayout_read_pagelist(struct nfs_read_data *data, unsigned nr_pages)
{
	struct pnfs_layout_segment *lseg = data->pdata.lseg;
	struct nfs4_pnfs_ds *ds;
	loff_t offset = data->args.offset;
	u32 idx;
	struct nfs_fh *fh;

	dprintk("--> %s ino %lu nr_pages %d pgbase %u req %Zu@%llu\n",
		__func__, data->inode->i_ino, nr_pages,
		data->args.pgbase, (size_t)data->args.count, offset);

	/* Retrieve the correct rpc_client for the byte range */
	idx = nfs4_fl_calc_ds_index(lseg, offset);
	ds = nfs4_fl_prepare_ds(lseg, idx);
	if (!ds) {
		printk(KERN_ERR "%s: prepare_ds failed, use MDS\n", __func__);
		return PNFS_NOT_ATTEMPTED;
	}
	dprintk("%s USE DS:ip %x %s\n", __func__,
		htonl(ds->ds_ip_addr), ds->r_addr);

	/* just try the first data server for the index..*/
	data->fldata.ds_nfs_client = ds->ds_clp;
	fh = nfs4_fl_select_ds_fh(lseg, offset);
	if (fh)
		data->args.fh = fh;

	/*
	 * Now get the file offset on the dserver
	 * Set the read offset to this offset, and
	 * save the original offset in orig_offset
	 * In the case of aync reads, the offset will be reset in the
	 * call_ops->rpc_call_done() routine.
	 */
	data->args.offset = filelayout_get_dserver_offset(lseg, offset);
	data->fldata.orig_offset = offset;

	/* Perform an asynchronous read */
	nfs_initiate_read(data, ds->ds_clp->cl_rpcclient,
			  &filelayout_read_call_ops);

	data->pdata.pnfs_error = 0;

	return PNFS_ATTEMPTED;
}

/* Perform async writes. */
static enum pnfs_try_status
filelayout_write_pagelist(struct nfs_write_data *data, unsigned nr_pages, int sync)
{
	struct pnfs_layout_segment *lseg = data->pdata.lseg;
	struct nfs4_pnfs_ds *ds;
	loff_t offset = data->args.offset;
	u32 idx;
	struct nfs_fh *fh;

	/* Retrieve the correct rpc_client for the byte range */
	idx = nfs4_fl_calc_ds_index(lseg, offset);
	ds = nfs4_fl_prepare_ds(lseg, idx);
	if (!ds) {
		printk(KERN_ERR "%s: prepare_ds failed, use MDS\n", __func__);
		return PNFS_NOT_ATTEMPTED;
	}
	dprintk("%s ino %lu sync %d req %Zu@%llu DS:%x:%hu %s\n", __func__,
		data->inode->i_ino, sync, (size_t) data->args.count, offset,
		htonl(ds->ds_ip_addr), ntohs(ds->ds_port), ds->r_addr);

	data->fldata.ds_nfs_client = ds->ds_clp;
	fh = nfs4_fl_select_ds_fh(lseg, offset);
	if (fh)
		data->args.fh = fh;
	/*
	 * Get the file offset on the dserver. Set the write offset to
	 * this offset and save the original offset.
	 */
	data->args.offset = filelayout_get_dserver_offset(lseg, offset);
	data->fldata.orig_offset = offset;

	/*
	 * Perform an asynchronous write The offset will be reset in the
	 * call_ops->rpc_call_done() routine
	 */
	nfs_initiate_write(data, ds->ds_clp->cl_rpcclient,
			   &filelayout_write_call_ops, sync);

	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;
}

/*
 * Create a filelayout layout structure and return it.  The pNFS client
 * will use the pnfs_layout_hdr type to refer to the layout for this
 * inode from now on.
 */
static struct pnfs_layout_hdr *
filelayout_alloc_layout(struct inode *inode)
{
	struct nfs4_filelayout *flp;

	dprintk("NFS_FILELAYOUT: allocating layout\n");
	flp =  kzalloc(sizeof(struct nfs4_filelayout), GFP_KERNEL);
	return flp ? &flp->fl_layout : NULL;
}

/* Free a filelayout layout structure */
static void
filelayout_free_layout(struct pnfs_layout_hdr *lo)
{
	dprintk("NFS_FILELAYOUT: freeing layout\n");
	kfree(FILE_LO(lo));
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
filelayout_check_layout(struct pnfs_layout_hdr *lo,
			struct pnfs_layout_segment *lseg)
{
	struct nfs4_filelayout_segment *fl = LSEG_LD_DATA(lseg);
	struct nfs4_file_layout_dsaddr *dsaddr;
	int status = -EINVAL;
	struct nfs_server *nfss = NFS_SERVER(PNFS_INODE(lo));

	dprintk("--> %s\n", __func__);
	/* find in list or get from server and reference the deviceid */
	dsaddr = nfs4_fl_find_get_deviceid(nfss->nfs_client, &fl->dev_id);
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
		goto out_put;
	}

	if (fl->pattern_offset != 0) {
		dprintk("%s Unsupported no-zero pattern_offset %Ld\n",
				__func__, fl->pattern_offset);
		goto out_put;
	}

	if (fl->stripe_unit % PAGE_SIZE) {
		dprintk("%s Stripe unit (%u) not page aligned\n",
			__func__, fl->stripe_unit);
		goto out_put;
	}

	/* XXX only support SPARSE packing. Don't support use MDS open fh */
	if (!(fl->num_fh == 1 || fl->num_fh == dsaddr->ds_num)) {
		dprintk("%s num_fh %u not equal to 1 or ds_num %u\n",
			__func__, fl->num_fh, dsaddr->ds_num);
		goto out_put;
	}

	if (fl->stripe_unit % nfss->rsize || fl->stripe_unit % nfss->wsize) {
		dprintk("%s Stripe unit (%u) not aligned with rsize %u "
			"wsize %u\n", __func__, fl->stripe_unit, nfss->rsize,
			nfss->wsize);
	}

	nfs4_set_layout_deviceid(lseg, &dsaddr->deviceid);

	status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
out_put:
	nfs4_put_unset_layout_deviceid(lseg, &dsaddr->deviceid,
				       nfs4_fl_free_deviceid_callback);
	goto out;
}

static void _filelayout_free_lseg(struct pnfs_layout_segment *lseg);
static void filelayout_free_fh_array(struct nfs4_filelayout_segment *fl);

/* Decode layout and store in layoutid.  Overwrite any existing layout
 * information for this file.
 */
static int
filelayout_set_layout(struct nfs4_filelayout *flo,
		      struct nfs4_filelayout_segment *fl,
		      struct nfs4_layoutget_res *lgr)
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
			/* Layout is now invalid, pretend it doesn't exist */
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
filelayout_alloc_lseg(struct pnfs_layout_hdr *layoutid,
		      struct nfs4_layoutget_res *lgr)
{
	struct nfs4_filelayout *flo = FILE_LO(layoutid);
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
	nfs4_put_unset_layout_deviceid(lseg, lseg->deviceid,
				   nfs4_fl_free_deviceid_callback);
	_filelayout_free_lseg(lseg);
}

/* Allocate a new nfs_write_data struct and initialize */
static struct nfs_write_data *
filelayout_clone_write_data(struct nfs_write_data *old)
{
	static struct nfs_write_data *new;

	new = nfs_commitdata_alloc();
	if (!new)
		goto out;
	kref_init(&new->refcount);
	new->parent      = old;
	kref_get(&old->refcount);
	new->inode       = old->inode;
	new->cred        = old->cred;
	new->args.offset = 0;
	new->args.count  = 0;
	new->res.count   = 0;
	new->res.fattr   = &new->fattr;
	nfs_fattr_init(&new->fattr);
	new->res.verf    = &new->verf;
	new->args.context = get_nfs_open_context(old->args.context);
	new->pdata.lseg = NULL;
	new->pdata.call_ops = old->pdata.call_ops;
	new->pdata.how = old->pdata.how;
out:
	return new;
}

static void filelayout_commit_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	wdata->pdata.call_ops->rpc_call_done(task, data);
}

static struct rpc_call_ops filelayout_commit_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_commit_call_done,
	.rpc_release = filelayout_write_release,
};

/*
 * Execute a COMMIT op to the MDS or to each data server on which a page
 * in 'pages' exists.
 * Invoke the pnfs_commit_complete callback.
 */
enum pnfs_try_status
filelayout_commit(struct nfs_write_data *data, int sync)
{
	LIST_HEAD(head);
	struct nfs_page *req;
	loff_t file_offset = 0;
	u16 idx, i;
	struct list_head **ds_page_list = NULL;
	u16 *indices_used;
	int num_indices_seen = 0;
	const struct rpc_call_ops *call_ops;
	struct rpc_clnt *clnt;
	struct nfs_write_data **clone_list = NULL;
	struct nfs_write_data *dsdata;
	struct nfs4_pnfs_ds *ds;

	dprintk("%s data %p sync %d\n", __func__, data, sync);

	/* Alloc room for both in one go */
	ds_page_list = kzalloc((NFS4_PNFS_MAX_MULTI_CNT + 1) *
			       (sizeof(u16) + sizeof(struct list_head *)),
			       GFP_KERNEL);
	if (!ds_page_list)
		goto mem_error;
	indices_used = (u16 *) (ds_page_list + NFS4_PNFS_MAX_MULTI_CNT + 1);
	/*
	 * Sort pages based on which ds to send to.
	 * MDS is given index equal to NFS4_PNFS_MAX_MULTI_CNT.
	 * Note we are assuming there is only a single lseg in play.
	 * When that is not true, we could first sort on lseg, then
	 * sort within each as we do here.
	 */
	while (!list_empty(&data->pages)) {
		req = nfs_list_entry(data->pages.next);
		nfs_list_remove_request(req);
		if (!req->wb_lseg ||
		    ((struct nfs4_filelayout_segment *)
		     LSEG_LD_DATA(req->wb_lseg))->commit_through_mds)
			idx = NFS4_PNFS_MAX_MULTI_CNT;
		else {
			file_offset = (loff_t)req->wb_index << PAGE_CACHE_SHIFT;
			idx = nfs4_fl_calc_ds_index(req->wb_lseg, file_offset);
		}
		if (ds_page_list[idx]) {
			/* Already seen this idx */
			list_add(&req->wb_list, ds_page_list[idx]);
		} else {
			/* New idx not seen so far */
			list_add_tail(&req->wb_list, &head);
			indices_used[num_indices_seen++] = idx;
		}
		ds_page_list[idx] = &req->wb_list;
	}
	/* Once created, clone must be released via call_op */
	clone_list = kzalloc(num_indices_seen *
			     sizeof(struct nfs_write_data *), GFP_KERNEL);
	if (!clone_list)
		goto mem_error;
	for (i = 0; i < num_indices_seen - 1; i++) {
		clone_list[i] = filelayout_clone_write_data(data);
		if (!clone_list[i])
			goto mem_error;
	}
	clone_list[i] = data;
	/*
	 * Now send off the RPCs to each ds.  Note that it is important
	 * that any RPC to the MDS be sent last (or at least after all
	 * clones have been made.)
	 */
	for (i = 0; i < num_indices_seen; i++) {
		dsdata = clone_list[i];
		idx = indices_used[i];
		list_cut_position(&dsdata->pages, &head, ds_page_list[idx]);
		if (idx == NFS4_PNFS_MAX_MULTI_CNT) {
			call_ops = data->pdata.call_ops;;
			clnt = NFS_CLIENT(dsdata->inode);
			ds = NULL;
		} else {
			struct nfs_fh *fh;

			call_ops = &filelayout_commit_call_ops;
			req = nfs_list_entry(dsdata->pages.next);
			ds = nfs4_fl_prepare_ds(req->wb_lseg, idx);
			if (!ds) {
				/* Trigger retry of this chunk through MDS */
				dsdata->task.tk_status = -EIO;
				data->pdata.call_ops->rpc_release(dsdata);
				continue;
			}
			clnt = ds->ds_clp->cl_rpcclient;
			dsdata->fldata.ds_nfs_client = ds->ds_clp;
			file_offset = (loff_t)req->wb_index << PAGE_CACHE_SHIFT;
			fh = nfs4_fl_select_ds_fh(req->wb_lseg, file_offset);
			if (fh)
				dsdata->args.fh = fh;
		}
		dprintk("%s: Initiating commit: %llu USE DS:\n",
			__func__, file_offset);
		print_ds(ds);

		/* Send COMMIT to data server */
		nfs_initiate_commit(dsdata, clnt, call_ops, sync);
	}
	kfree(clone_list);
	kfree(ds_page_list);
	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;

 mem_error:
	if (clone_list) {
		for (i = 0; i < num_indices_seen - 1; i++) {
			if (!clone_list[i])
				break;
			data->pdata.call_ops->rpc_release(clone_list[i]);
		}
		kfree(clone_list);
	}
	kfree(ds_page_list);
	/* One of these will be empty, but doesn't hurt to do both */
	nfs_mark_list_commit(&head);
	nfs_mark_list_commit(&data->pages);
	data->pdata.call_ops->rpc_release(data);
	return PNFS_ATTEMPTED;
}

/* Return the stripesize for the specified file */
ssize_t
filelayout_get_stripesize(struct pnfs_layout_hdr *lo)
{
	struct nfs4_filelayout *flo = FILE_LO(lo);

	return flo->stripe_unit;
}

/*
 * filelayout_pg_test(). Called by nfs_can_coalesce_requests()
 *
 * return 1 :  coalesce page
 * return 0 :  don't coalesce page
 */
int
filelayout_pg_test(struct nfs_pageio_descriptor *pgio, struct nfs_page *prev,
		   struct nfs_page *req)
{
	u64 p_stripe, r_stripe;

	if (pgio->pg_boundary == 0)
		return 1;
	p_stripe = (u64)prev->wb_index << PAGE_CACHE_SHIFT;
	r_stripe = (u64)req->wb_index << PAGE_CACHE_SHIFT;

	do_div(p_stripe, pgio->pg_boundary);
	do_div(r_stripe, pgio->pg_boundary);

	return (p_stripe == r_stripe);
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
	.get_stripesize        = filelayout_get_stripesize,
	.pg_test               = filelayout_pg_test,
};

struct pnfs_layoutdriver_type filelayout_type = {
	.id = LAYOUT_NFSV4_1_FILES,
	.name = "LAYOUT_NFSV4_1_FILES",
	.ld_io_ops = &filelayout_io_operations,
	.ld_policy_ops = &filelayout_policy_operations,
};

static int __init nfs4filelayout_init(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Registering...\n",
	       __func__);

	/*
	 * Need to register file_operations struct with global list to indicate
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
