/*
 *  objlayout.c
 *
 *  pNFS layout driver for Panasas OSDs
 *
 *  Copyright (C) 2007-2009 Panasas Inc.
 *  All rights reserved.
 *
 *  Benny Halevy <bhalevy@panasas.com>
 *  Boaz Harrosh <bharrosh@panasas.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  See the file COPYING included with this distribution for more details.
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
 *  3. Neither the name of the Panasas company nor the names of its
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

#include "objlayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS

#define _DEVID_LO(oid_device_id) \
	be64_to_cpup((__be64 *)oid_device_id.data)

#define _DEVID_HI(oid_device_id) \
	be64_to_cpup(((__be64 *)oid_device_id.data) + 1)

struct pnfs_client_operations *pnfs_client_ops;

/*
 * Create a objlayout layout structure for the given inode and return it.
 */
static void *
objlayout_alloc_layout(struct pnfs_mount_type *mountid, struct inode *inode)
{
	struct objlayout *objlay;

	objlay = kzalloc(sizeof(struct objlayout), GFP_KERNEL);
	dprintk("%s: Return %p\n", __func__, objlay);
	return objlay;
}

/*
 * Free an objlayout layout structure
 */
static void
objlayout_free_layout(void *p)
{
	struct objlayout *objlay = p;

	dprintk("%s: objlay %p\n", __func__, objlay);

	kfree(objlay);
}

/*
 * Unmarshall layout and store it in pnfslay.
 */
static struct pnfs_layout_segment *
objlayout_alloc_lseg(struct pnfs_layout_type *pnfslay,
		     struct nfs4_pnfs_layoutget_res *lgr)
{
	int status;
	void *layout = lgr->layout.buf;
	struct pnfs_layout_segment *lseg;
	struct objlayout_segment *objlseg;
	struct pnfs_osd_layout *pnfs_osd_layout;

	dprintk("%s: Begin pnfslay %p layout %p\n", __func__, pnfslay, layout);

	BUG_ON(!layout);

	status = -ENOMEM;
	lseg = kzalloc(sizeof(*lseg) + sizeof(*objlseg) +
		       pnfs_osd_layout_incore_sz(layout), GFP_KERNEL);
	if (!lseg)
		goto err;

	objlseg = LSEG_LD_DATA(lseg);
	pnfs_osd_layout = (struct pnfs_osd_layout *)objlseg->pnfs_osd_layout;
	pnfs_osd_xdr_decode_layout(pnfs_osd_layout, layout);

	status = objio_alloc_lseg(&objlseg->internal, pnfslay, lseg,
				  pnfs_osd_layout);
	if (status)
		goto err;

	dprintk("%s: Return %p\n", __func__, lseg);
	return lseg;

 err:
	kfree(lseg);
	return ERR_PTR(status);
}

/*
 * Free a layout segement
 */
static void
objlayout_free_lseg(struct pnfs_layout_segment *lseg)
{
	struct objlayout_segment *objlseg;

	dprintk("%s: freeing layout segment %p\n", __func__, lseg);

	if (unlikely(!lseg))
		return;

	objlseg = LSEG_LD_DATA(lseg);
	objio_free_lseg(objlseg->internal);
	kfree(lseg);
}

/*
 * I/O Operations
 */
static inline u64
end_offset(u64 start, u64 len)
{
	u64 end;

	end = start + len;
	return end >= start ? end : NFS4_MAX_UINT64;
}

/* last octet in a range */
static inline u64
last_byte_offset(u64 start, u64 len)
{
	u64 end;

	BUG_ON(!len);
	end = start + len;
	return end > start ? end - 1 : NFS4_MAX_UINT64;
}

static struct objlayout_io_state *
objlayout_alloc_io_state(struct pnfs_layout_type *pnfs_layout_type,
			struct page **pages,
			unsigned pgbase,
			unsigned nr_pages,
			loff_t offset,
			size_t count,
			struct pnfs_layout_segment *lseg,
			void *rpcdata)
{
	struct objlayout_segment *objlseg = LSEG_LD_DATA(lseg);
	struct objlayout_io_state *state;
	u64 lseg_end_offset;
	size_t size_nr_pages;

	dprintk("%s: allocating io_state\n", __func__);
	if (objio_alloc_io_state(objlseg->internal, &state))
		return NULL;

	BUG_ON(offset < lseg->range.offset);
	lseg_end_offset = end_offset(lseg->range.offset, lseg->range.length);
	BUG_ON(offset >= lseg_end_offset);
	if (offset + count > lseg_end_offset) {
		count = lseg->range.length - (offset - lseg->range.offset);
		dprintk("%s: truncated count %Zd\n", __func__, count);
	}

	if (pgbase > PAGE_SIZE) {
		unsigned n = pgbase >> PAGE_SHIFT;

		pgbase &= ~PAGE_MASK;
		pages += n;
		nr_pages -= n;
	}

	size_nr_pages = (pgbase + count + PAGE_SIZE - 1) >> PAGE_SHIFT;
	BUG_ON(nr_pages < size_nr_pages);
	if (nr_pages > size_nr_pages)
		nr_pages = size_nr_pages;

	state->lseg = lseg;
	state->rpcdata = rpcdata;
	state->pages = pages;
	state->pgbase = pgbase;
	state->nr_pages = nr_pages;
	state->offset = offset;
	state->count = count;
	state->sync = 0;

	return state;
}

static void
objlayout_free_io_state(struct objlayout_io_state *state)
{
	dprintk("%s: freeing io_state\n", __func__);
	if (unlikely(!state))
		return;

	objio_free_io_state(state);
}

/*
 * I/O done common code
 */
static void
objlayout_iodone(struct objlayout_io_state *state)
{
	dprintk("%s: state %p status\n", __func__, state);

	objlayout_free_io_state(state);
}

/*
 * Commit data remotely on OSDs
 */
enum pnfs_try_status
objlayout_commit(struct pnfs_layout_type *pnfslay,
		 int sync,
		 struct nfs_write_data *data)
{
	int status = PNFS_ATTEMPTED;
	dprintk("%s: Return %d\n", __func__, status);
	return status;
}

/* Function scheduled on rpc workqueue to call ->nfs_readlist_complete().
 * This is because the osd completion is called with ints-off from
 * the block layer
 */
static void _rpc_read_complete(struct work_struct *work)
{
	struct rpc_task *task;
	struct nfs_read_data *rdata;

	dprintk("%s enter\n", __func__);
	task = container_of(work, struct rpc_task, u.tk_work);
	rdata = container_of(task, struct nfs_read_data, task);

	pnfs_client_ops->nfs_readlist_complete(rdata);
}

void
objlayout_read_done(struct objlayout_io_state *state, ssize_t status, bool sync)
{
	int eof = state->eof;
	struct nfs_read_data *rdata;

	state->status = status;
	dprintk("%s: Begin status=%ld eof=%d\n", __func__, status, eof);
	rdata = state->rpcdata;
	rdata->task.tk_status = status;
	if (status >= 0) {
		rdata->res.count = status;
		rdata->res.eof = eof;
	}
	objlayout_iodone(state);
	/* must not use state after this point */

	if (sync)
		pnfs_client_ops->nfs_readlist_complete(rdata);
	else {
		INIT_WORK(&rdata->task.u.tk_work, _rpc_read_complete);
		schedule_work(&rdata->task.u.tk_work);
	}
}

/*
 * Perform sync or async reads.
 */
enum pnfs_try_status
objlayout_read_pagelist(struct pnfs_layout_type *pnfs_layout_type,
			struct page **pages,
			unsigned pgbase,
			unsigned nr_pages,
			loff_t offset,
			size_t count,
			struct nfs_read_data *rdata)
{
	struct inode *inode = PNFS_INODE(pnfs_layout_type);
	struct objlayout_io_state *state;
	ssize_t status = 0;
	loff_t eof;

	dprintk("%s: Begin inode %p offset %llu count %d\n",
		__func__, inode, offset, (int)count);

	eof = i_size_read(inode);
	if (unlikely(offset + count > eof)) {
		if (offset >= eof) {
			status = 0;
			rdata->res.count = 0;
			rdata->res.eof = 1;
			goto out;
		}
		count = eof - offset;
	}

	state = objlayout_alloc_io_state(pnfs_layout_type, pages, pgbase,
					 nr_pages, offset, count,
					 rdata->pdata.lseg, rdata);
	if (unlikely(!state)) {
		status = -ENOMEM;
		goto out;
	}

	state->eof = state->offset + state->count >= eof;

	status = objio_read_pagelist(state);
 out:
	dprintk("%s: Return status %Zd\n", __func__, status);
	rdata->pdata.pnfs_error = status;
	return PNFS_ATTEMPTED;
}

/* Function scheduled on rpc workqueue to call ->nfs_writelist_complete().
 * This is because the osd completion is called with ints-off from
 * the block layer
 */
static void _rpc_write_complete(struct work_struct *work)
{
	struct rpc_task *task;
	struct nfs_write_data *wdata;

	dprintk("%s enter\n", __func__);
	task = container_of(work, struct rpc_task, u.tk_work);
	wdata = container_of(task, struct nfs_write_data, task);

	pnfs_client_ops->nfs_writelist_complete(wdata);
}

void
objlayout_write_done(struct objlayout_io_state *state, ssize_t status,
		     bool sync)
{
	struct nfs_write_data *wdata;

	dprintk("%s: Begin\n", __func__);
	wdata = state->rpcdata;
	state->status = status;
	wdata->task.tk_status = status;
	if (status >= 0) {
		wdata->res.count = status;
		wdata->verf.committed = state->committed;
		dprintk("%s: Return status %d committed %d\n",
			__func__, wdata->task.tk_status,
			wdata->verf.committed);
	} else
		dprintk("%s: Return status %d\n",
			__func__, wdata->task.tk_status);
	objlayout_iodone(state);
	/* must not use state after this point */

	if (sync)
		pnfs_client_ops->nfs_writelist_complete(wdata);
	else {
		INIT_WORK(&wdata->task.u.tk_work, _rpc_write_complete);
		schedule_work(&wdata->task.u.tk_work);
	}
}

/*
 * Perform sync or async writes.
 */
enum pnfs_try_status
objlayout_write_pagelist(struct pnfs_layout_type *pnfs_layout_type,
			 struct page **pages,
			 unsigned pgbase,
			 unsigned nr_pages,
			 loff_t offset,
			 size_t count,
			 int how,
			 struct nfs_write_data *wdata)
{
	struct objlayout_io_state *state;
	ssize_t status;

	dprintk("%s: Begin inode %p offset %llu count %d\n",
		__func__, PNFS_INODE(pnfs_layout_type), offset, (int)count);

	state = objlayout_alloc_io_state(pnfs_layout_type, pages, pgbase,
					 nr_pages, offset, count,
					 wdata->pdata.lseg, wdata);
	if (unlikely(!state)) {
		status = -ENOMEM;
		goto out;
	}

	state->sync = how & FLUSH_SYNC;

	status = objio_write_pagelist(state, how & FLUSH_STABLE);
 out:
	dprintk("%s: Return status %Zd\n", __func__, status);
	wdata->pdata.pnfs_error = status;
	return PNFS_ATTEMPTED;
}

struct objlayout_deviceinfo {
	struct page *page;
	struct pnfs_osd_deviceaddr da; /* This must be last */
};

/* Initialize and call nfs_getdeviceinfo, then decode and return a
 * "struct pnfs_osd_deviceaddr *" Eventually objlayout_put_deviceinfo()
 * should be called.
 */
int objlayout_get_deviceinfo(struct pnfs_layout_type *pnfslay,
	struct pnfs_deviceid *d_id, struct pnfs_osd_deviceaddr **deviceaddr)
{
	struct objlayout_deviceinfo *odi;
	struct pnfs_device pd;
	struct super_block *sb;
	struct page *page;
	size_t sz;
	u32 *p;
	int err;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	pd.area = page_address(page);

	memcpy(&pd.dev_id, d_id, sizeof(*d_id));
	pd.layout_type = LAYOUT_OSD2_OBJECTS;
	pd.dev_notify_types = 0;
	pd.pages = &page;
	pd.pgbase = 0;
	pd.pglen = PAGE_SIZE;
	pd.mincount = 0;

	sb = PNFS_INODE(pnfslay)->i_sb;
	err = pnfs_client_ops->nfs_getdeviceinfo(sb, &pd);
	dprintk("%s nfs_getdeviceinfo returned %d\n", __func__, err);
	if (err)
		goto err_out;

	p = pd.area;
	sz = pnfs_osd_xdr_deviceaddr_incore_sz(p);
	odi = kzalloc(sz + (sizeof(*odi) - sizeof(odi->da)), GFP_KERNEL);
	if (!odi) {
		err = -ENOMEM;
		goto err_out;
	}
	pnfs_osd_xdr_decode_deviceaddr(&odi->da, p);
	odi->page = page;
	*deviceaddr = &odi->da;
	return 0;

err_out:
	__free_page(page);
	return err;
}

void objlayout_put_deviceinfo(struct pnfs_osd_deviceaddr *deviceaddr)
{
	struct objlayout_deviceinfo *odi = container_of(deviceaddr,
						struct objlayout_deviceinfo,
						da);

	__free_page(odi->page);
	kfree(odi);
}

/*
 * Initialize a mountpoint by retrieving the list of
 * available devices for it.
 * Return the pnfs_mount_type structure so the
 * pNFS_client can refer to the mount point later on.
 */
static struct pnfs_mount_type *
objlayout_initialize_mountpoint(struct super_block *sb, struct nfs_fh *fh)
{
	struct pnfs_mount_type *mt;

	mt = kzalloc(sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return NULL;

	mt->mountid = objio_init_mt();
	if (IS_ERR(mt->mountid)) {
		printk(KERN_INFO "%s: objlayout lib not ready err=%ld\n",
		       __func__, PTR_ERR(mt->mountid));
		kfree(mt);
		return NULL;
	}

	dprintk("%s: Return %p\n", __func__, mt);
	return mt;
}

/*
 * Uninitialize a mountpoint
 */
static int
objlayout_uninitialize_mountpoint(struct pnfs_mount_type *mt)
{
	dprintk("%s: Begin %p\n", __func__, mt);
	objio_fini_mt(mt->mountid);
	kfree(mt);
	return 0;
}

struct layoutdriver_io_operations objlayout_io_operations = {
	.commit                  = objlayout_commit,
	.read_pagelist           = objlayout_read_pagelist,
	.write_pagelist          = objlayout_write_pagelist,
	.alloc_layout            = objlayout_alloc_layout,
	.free_layout             = objlayout_free_layout,
	.alloc_lseg              = objlayout_alloc_lseg,
	.free_lseg               = objlayout_free_lseg,
	.initialize_mountpoint   = objlayout_initialize_mountpoint,
	.uninitialize_mountpoint = objlayout_uninitialize_mountpoint,
};
