/*
 *  linux/fs/nfs/nfs4filelayoutdev.c
 *
 *  Device operations for the pnfs nfs4 file layout driver.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand <dhildebz@eecs.umich.edu>
 *  Garth Goodson   <Garth.Goodson@netapp.com>
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

#include <linux/hash.h>

#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_xdr.h>

#include <asm/div64.h>

#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/nfs4_pnfs.h>
#include "nfs4filelayout.h"
#include "internal.h"
#include "nfs4_fs.h"

#define NFSDBG_FACILITY		NFSDBG_PNFS_LD

DEFINE_SPINLOCK(nfs4_ds_cache_lock);
static LIST_HEAD(nfs4_data_server_cache);

void
print_ds(struct nfs4_pnfs_ds *ds)
{
	if (ds == NULL) {
		dprintk("%s NULL device \n", __func__);
		return;
	}
	dprintk("        ip_addr %x\n", ntohl(ds->ds_ip_addr));
	dprintk("        port %hu\n", ntohs(ds->ds_port));
	dprintk("        client %p\n", ds->ds_clp);
	dprintk("        ref count %d\n", atomic_read(&ds->ds_count));
	if (ds->ds_clp)
		dprintk("        cl_exchange_flags %x\n",
					    ds->ds_clp->cl_exchange_flags);
	dprintk("        ip:port %s\n", ds->r_addr);
}

void
print_ds_list(struct nfs4_file_layout_dsaddr *dsaddr)
{
	int i;

	dprintk("%s dsaddr->ds_num %d\n", __func__,
		dsaddr->ds_num);
	for (i = 0; i < dsaddr->ds_num; i++)
		print_ds(dsaddr->ds_list[i]);
}

/* Debugging function assuming a 64bit major/minor split of the deviceid */
char *
deviceid_fmt(const struct pnfs_deviceid *dev_id)
{
	static char buf[17];
	uint32_t *p = (uint32_t *)dev_id->data;
	uint64_t major, minor;

	p = xdr_decode_hyper(p, &major);
	p = xdr_decode_hyper(p, &minor);

	sprintf(buf, "%08llu %08llu", major, minor);
	return buf;
}

/* nfs4_ds_cache_lock is held */
static inline struct nfs4_pnfs_ds *
_data_server_lookup(u32 ip_addr, u32 port)
{
	struct nfs4_pnfs_ds *ds;

	dprintk("_data_server_lookup: ip_addr=%x port=%hu\n",
			ntohl(ip_addr), ntohs(port));

	list_for_each_entry(ds, &nfs4_data_server_cache, ds_node) {
		if (ds->ds_ip_addr == ip_addr &&
		    ds->ds_port == port) {
			return ds;
		}
	}
	return NULL;
}

static void
destroy_ds(struct nfs4_pnfs_ds *ds)
{
	dprintk("--> %s\n", __func__);
	print_ds(ds);

	if (ds->ds_clp)
		nfs_put_client(ds->ds_clp);
	kfree(ds);
}

static void
nfs4_fl_free_deviceid(struct nfs4_file_layout_dsaddr *dsaddr)
{
	struct nfs4_pnfs_ds *ds;
	int i;

	dprintk("%s: device id=%s\n", __func__,
		deviceid_fmt(&dsaddr->deviceid.de_id));

	for (i = 0; i < dsaddr->ds_num; i++) {
		ds = dsaddr->ds_list[i];
		if (ds != NULL) {
			if (atomic_dec_and_lock(&ds->ds_count,
						&nfs4_ds_cache_lock)) {
				list_del_init(&ds->ds_node);
				spin_unlock(&nfs4_ds_cache_lock);
				destroy_ds(ds);
			}
		}
	}
	kfree(dsaddr->stripe_indices);
	kfree(dsaddr);
}

void
nfs4_fl_free_deviceid_callback(struct kref *kref)
{
	struct nfs4_deviceid *device =
		container_of(kref, struct nfs4_deviceid, de_kref);
	struct nfs4_file_layout_dsaddr *dsaddr =
		container_of(device, struct nfs4_file_layout_dsaddr, deviceid);

	nfs4_fl_free_deviceid(dsaddr);
}

static void
nfs4_pnfs_ds_add(struct inode *inode, struct nfs4_pnfs_ds **dsp,
		 u32 ip_addr, u32 port, char *r_addr, int len)
{
	struct nfs4_pnfs_ds *tmp_ds, *ds;

	*dsp = NULL;

	ds = kzalloc(sizeof(*tmp_ds), GFP_KERNEL);
	if (!ds)
		return;

	spin_lock(&nfs4_ds_cache_lock);
	tmp_ds = _data_server_lookup(ip_addr, port);
	if (tmp_ds == NULL) {
		ds->ds_ip_addr = ip_addr;
		ds->ds_port = port;
		strncpy(ds->r_addr, r_addr, len);
		atomic_set(&ds->ds_count, 1);
		INIT_LIST_HEAD(&ds->ds_node);
		ds->ds_clp = NULL;
		list_add(&ds->ds_node, &nfs4_data_server_cache);
		*dsp = ds;
		dprintk("%s add new data server ip 0x%x\n", __func__,
				ds->ds_ip_addr);
		spin_unlock(&nfs4_ds_cache_lock);
	} else {
		atomic_inc(&tmp_ds->ds_count);
		*dsp = tmp_ds;
		dprintk("%s data server found ip 0x%x, inc'ed ds_count to %d\n",
				__func__, tmp_ds->ds_ip_addr,
				atomic_read(&tmp_ds->ds_count));
		spin_unlock(&nfs4_ds_cache_lock);
		kfree(ds);
	}
}

struct nfs4_file_layout_dsaddr *
nfs4_pnfs_device_item_find(struct nfs_client *clp, struct pnfs_deviceid *id)
{
	struct nfs4_deviceid *d;

	d = nfs4_find_deviceid(clp->cl_devid_cache, id);
	dprintk("%s device id (%s) nfs4_deviceid %p\n", __func__,
		deviceid_fmt(id), d);
	return (d == NULL) ? NULL :
		container_of(d, struct nfs4_file_layout_dsaddr, deviceid);
}
