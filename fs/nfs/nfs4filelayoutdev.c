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
#include <linux/pnfs_xdr.h>
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

/* Create an rpc to the data server defined in 'dev_list' */
static int
nfs4_pnfs_ds_create(struct nfs_server *mds_srv, struct nfs4_pnfs_ds *ds)
{
	struct nfs_server	tmp = {
		.nfs_client = NULL,
	};
	struct sockaddr_in	sin;
	struct rpc_clnt 	*mds_clnt = mds_srv->client;
	struct nfs_client	*clp = mds_srv->nfs_client;
	struct sockaddr		*mds_addr;
	int err = 0;

	dprintk("--> %s ip:port %s au_flavor %d\n", __func__,
		ds->r_addr, mds_clnt->cl_auth->au_flavor);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ds->ds_ip_addr;
	sin.sin_port = ds->ds_port;

	/*
	 * If this DS is also the MDS, use the MDS session only if the
	 * MDS exchangeid flags show the EXCHGID4_FLAG_USE_PNFS_DS pNFS role.
	 */
	mds_addr = (struct sockaddr *)&clp->cl_addr;
	if (nfs_sockaddr_cmp((struct sockaddr *)&sin, mds_addr)) {
		if (!(clp->cl_exchange_flags & EXCHGID4_FLAG_USE_PNFS_DS)) {
			printk(KERN_INFO "ip:port %s is not a pNFS Data "
				"Server\n", ds->r_addr);
			err = -ENODEV;
		} else {
			atomic_inc(&clp->cl_count);
			ds->ds_clp = clp;
			dprintk("%s Using MDS Session for DS\n", __func__);
		}
		goto out;
	}
	/*
	 * Set a retrans, timeout interval, and authflavor equual to the MDS
	 * values. Use the MDS nfs_client cl_ipaddr field so as to use the
	 * same co_ownerid as the MDS.
	 */
	err = nfs4_set_client(&tmp,
			      mds_srv->nfs_client->cl_hostname,
			      (struct sockaddr *)&sin,
			      sizeof(struct sockaddr),
			      mds_srv->nfs_client->cl_ipaddr,
			      mds_clnt->cl_auth->au_flavor,
			      IPPROTO_TCP,
			      mds_clnt->cl_xprt->timeout,
			      1 /* minorversion */);
	if (err < 0)
		goto out;

	clp = tmp.nfs_client;

	/* Ask for only the EXCHGID4_FLAG_USE_PNFS_DS pNFS role */
	dprintk("%s EXCHANGE_ID for clp %p\n", __func__, clp);
	clp->cl_exchange_flags = EXCHGID4_FLAG_USE_PNFS_DS;

	err = nfs4_recover_expired_lease(clp);
	if (!err)
		err = nfs4_check_client_ready(clp);
	if (err)
		goto out_put;

	if (!(clp->cl_exchange_flags & EXCHGID4_FLAG_USE_PNFS_DS)) {
		printk(KERN_INFO "ip:port %s is not a pNFS Data Server\n",
			ds->r_addr);
		err = -ENODEV;
		goto out_put;
	}

	/*
	 * Mask the (possibly) returned EXCHGID4_FLAG_USE_PNFS_MDS pNFS role
	 * The is_ds_only_session depends on this.
	 */
	clp->cl_exchange_flags &= ~EXCHGID4_FLAG_USE_PNFS_MDS;

	/*
	 * Set DS lease equal to the MDS lease, renewal is scheduled in
	 * create_session
	 */
	spin_lock(&mds_srv->nfs_client->cl_lock);
	clp->cl_lease_time = mds_srv->nfs_client->cl_lease_time;
	spin_unlock(&mds_srv->nfs_client->cl_lock);
	clp->cl_last_renewal = jiffies;

	clear_bit(NFS4CLNT_SESSION_RESET, &clp->cl_state);
	ds->ds_clp = clp;

	dprintk("%s: ip=%x, port=%hu, rpcclient %p\n", __func__,
				ntohl(ds->ds_ip_addr), ntohs(ds->ds_port),
				clp->cl_rpcclient);
out:
	dprintk("%s Returns %d\n", __func__, err);
	return err;
out_put:
	nfs_put_client(clp);
	goto out;
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

static struct nfs4_pnfs_ds *
decode_and_add_ds(uint32_t **pp, struct inode *inode)
{
	struct nfs4_pnfs_ds *ds = NULL;
	char r_addr[29]; /* max size of ip/port string */
	int len;
	u32 ip_addr, port;
	int tmp[6];
	uint32_t *p = *pp;

	dprintk("%s enter\n", __func__);
	/* check and skip r_netid */
	len = be32_to_cpup(p++);
	/* "tcp" */
	if (len != 3) {
		printk("%s: ERROR: non TCP r_netid len %d\n",
			__func__, len);
		goto out_err;
	}
	/* Read the bytes into a temporary buffer */
	/* XXX: should probably sanity check them */
	tmp[0] = be32_to_cpup(p++);

	len = be32_to_cpup(p++);
	if (len > 29) {
		printk("%s: ERROR: Device ip/port too long (%d)\n",
			__func__, len);
		goto out_err;
	}
	memcpy(r_addr, p, len);
	p += XDR_QUADLEN(len);
	*pp = p;
	r_addr[len] = '\0';
	sscanf(r_addr, "%d.%d.%d.%d.%d.%d", &tmp[0], &tmp[1],
	       &tmp[2], &tmp[3], &tmp[4], &tmp[5]);
	ip_addr = htonl((tmp[0]<<24) | (tmp[1]<<16) | (tmp[2]<<8) | (tmp[3]));
	port = htons((tmp[4] << 8) | (tmp[5]));

	nfs4_pnfs_ds_add(inode, &ds, ip_addr, port, r_addr, len);

	dprintk("%s: addr:port string = %s\n", __func__, r_addr);
	return ds;
out_err:
	dprintk("%s returned NULL\n", __func__);
	return NULL;
}

/* Decode opaque device data and return the result
 */
static struct nfs4_file_layout_dsaddr*
decode_device(struct inode *ino, struct pnfs_device *pdev)
{
	int i, dummy;
	u32 cnt, num;
	u8 *indexp;
	uint32_t *p = (u32 *)pdev->area, *indicesp;
	struct nfs4_file_layout_dsaddr *dsaddr;

	/* Get the stripe count (number of stripe index) */
	cnt = be32_to_cpup(p++);
	dprintk("%s stripe count  %d\n", __func__, cnt);
	if (cnt > NFS4_PNFS_MAX_STRIPE_CNT) {
		printk(KERN_WARNING "%s: stripe count %d greater than "
		       "supported maximum %d\n", __func__,
			cnt, NFS4_PNFS_MAX_STRIPE_CNT);
		goto out_err;
	}

	/* Check the multipath list count */
	indicesp = p;
	p += XDR_QUADLEN(cnt << 2);
	num = be32_to_cpup(p++);
	dprintk("%s ds_num %u\n", __func__, num);
	if (num > NFS4_PNFS_MAX_MULTI_CNT) {
		printk(KERN_WARNING "%s: multipath count %d greater than "
			"supported maximum %d\n", __func__,
			num, NFS4_PNFS_MAX_MULTI_CNT);
		goto out_err;
	}
	dsaddr = kzalloc(sizeof(*dsaddr) +
			(sizeof(struct nfs4_pnfs_ds *) * (num - 1)),
			GFP_KERNEL);
	if (!dsaddr)
		goto out_err;

	dsaddr->stripe_indices = kzalloc(sizeof(u8) * cnt, GFP_KERNEL);
	if (!dsaddr->stripe_indices)
		goto out_err_free;

	dsaddr->stripe_count = cnt;
	dsaddr->ds_num = num;

	memcpy(&dsaddr->deviceid.de_id, &pdev->dev_id,
	       NFS4_PNFS_DEVICEID4_SIZE);

	/* Go back an read stripe indices */
	p = indicesp;
	indexp = &dsaddr->stripe_indices[0];
	for (i = 0; i < dsaddr->stripe_count; i++) {
		dummy = be32_to_cpup(p++);
		*indexp = dummy; /* bound by NFS4_PNFS_MAX_MULTI_CNT */
		indexp++;
	}
	/* Skip already read multipath list count */
	p++;

	for (i = 0; i < dsaddr->ds_num; i++) {
		int j;

		dummy = be32_to_cpup(p++); /* multipath count */
		if (dummy > 1) {
			printk(KERN_WARNING
			       "%s: Multipath count %d not supported, "
			       "skipping all greater than 1\n", __func__,
				dummy);
		}
		for (j = 0; j < dummy; j++) {
			if (j == 0) {
				dsaddr->ds_list[i] = decode_and_add_ds(&p, ino);
				if (dsaddr->ds_list[i] == NULL)
					goto out_err_free;
			} else {
				u32 len;
				/* skip extra multipath */
				len = be32_to_cpup(p++);
				p += XDR_QUADLEN(len);
				len = be32_to_cpup(p++);
				p += XDR_QUADLEN(len);
				continue;
			}
		}
	}
	nfs4_init_deviceid_node(&dsaddr->deviceid);

	return dsaddr;

out_err_free:
	nfs4_fl_free_deviceid(dsaddr);
out_err:
	dprintk("%s ERROR: returning NULL\n", __func__);
	return NULL;
}

/*
 * Decode the opaque device specified in 'dev'
 * and add it to the list of available devices.
 * If the deviceid is already cached, nfs4_add_deviceid will return
 * a pointer to the cached struct and throw away the new.
 */
static struct nfs4_file_layout_dsaddr*
decode_and_add_device(struct inode *inode, struct pnfs_device *dev)
{
	struct nfs4_file_layout_dsaddr *dsaddr;
	struct nfs4_deviceid *d;

	dsaddr = decode_device(inode, dev);
	if (!dsaddr) {
		printk(KERN_WARNING "%s: Could not decode or add device\n",
			__func__);
		return NULL;
	}

	d = nfs4_add_deviceid(NFS_SERVER(inode)->nfs_client->cl_devid_cache,
			      &dsaddr->deviceid);

	return container_of(d, struct nfs4_file_layout_dsaddr, deviceid);
}

/* Retrieve the information for dev_id, add it to the list
 * of available devices, and return it.
 */
struct nfs4_file_layout_dsaddr *
get_device_info(struct inode *inode, struct pnfs_deviceid *dev_id)
{
	struct pnfs_device *pdev = NULL;
	u32 max_resp_sz;
	int max_pages;
	struct page **pages = NULL;
	struct nfs4_file_layout_dsaddr *dsaddr = NULL;
	int rc, i;
	struct nfs_server *server = NFS_SERVER(inode);

	/*
	 * Use the session max response size as the basis for setting
	 * GETDEVICEINFO's maxcount
	 */
	max_resp_sz = server->nfs_client->cl_session->fc_attrs.max_resp_sz;
	max_pages = max_resp_sz >> PAGE_SHIFT;
	dprintk("%s inode %p max_resp_sz %u max_pages %d\n",
		__func__, inode, max_resp_sz, max_pages);

	pdev = kzalloc(sizeof(struct pnfs_device), GFP_KERNEL);
	if (pdev == NULL)
		return NULL;

	pages = kzalloc(max_pages * sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL) {
		kfree(pdev);
		return NULL;
	}
	for (i = 0; i < max_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			goto out_free;
	}

	/* set pdev->area */
	pdev->area = vmap(pages, max_pages, VM_MAP, PAGE_KERNEL);
	if (!pdev->area)
		goto out_free;

	memcpy(&pdev->dev_id, dev_id, NFS4_PNFS_DEVICEID4_SIZE);
	pdev->layout_type = LAYOUT_NFSV4_1_FILES;
	pdev->pages = pages;
	pdev->pgbase = 0;
	pdev->pglen = PAGE_SIZE * max_pages;
	pdev->mincount = 0;
	/* TODO: Update types when CB_NOTIFY_DEVICEID is available */
	pdev->dev_notify_types = 0;

	rc = pnfs_callback_ops->nfs_getdeviceinfo(server, pdev);
	dprintk("%s getdevice info returns %d\n", __func__, rc);
	if (rc)
		goto out_free;

	/* Found new device, need to decode it and then add it to the
	 * list of known devices for this mountpoint.
	 */
	dsaddr = decode_and_add_device(inode, pdev);
out_free:
	if (pdev->area != NULL)
		vunmap(pdev->area);
	for (i = 0; i < max_pages; i++)
		__free_page(pages[i]);
	kfree(pages);
	kfree(pdev);
	dprintk("<-- %s dsaddr %p\n", __func__, dsaddr);
	return dsaddr;
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

/* Want res = ((offset / layout->stripe_unit) % dsaddr->stripe_count)
 * Then: ((res + fsi) % dsaddr->stripe_count)
 */
u32
filelayout_dserver_get_index(loff_t offset,
			     struct nfs4_file_layout_dsaddr *dsaddr,
			     struct nfs4_filelayout_segment *layout)
{
	u64 tmp, tmp2;

	tmp = offset;
	do_div(tmp, layout->stripe_unit);
	tmp2 = do_div(tmp, dsaddr->stripe_count) + layout->first_stripe_index;
	return do_div(tmp2, dsaddr->stripe_count);
}

/* Retrieve the rpc client for a specified byte range
 * in 'inode' by filling in the contents of 'dserver'.
 */
int
nfs4_pnfs_dserver_get(struct pnfs_layout_segment *lseg,
		      loff_t offset,
		      size_t count,
		      struct nfs4_pnfs_dserver *dserver)
{
	struct nfs4_filelayout_segment *layout = LSEG_LD_DATA(lseg);
	struct inode *inode = PNFS_INODE(lseg->layout);
	struct nfs_server *mds_srv = NFS_SERVER(inode);
	struct nfs4_file_layout_dsaddr *dsaddr;
	u64 tmp, tmp2;
	u32 stripe_idx, end_idx, ds_idx;

	if (!layout)
		return 1;

	dsaddr = container_of(lseg->deviceid, struct nfs4_file_layout_dsaddr,
			      deviceid);

	stripe_idx = filelayout_dserver_get_index(offset, dsaddr, layout);

	/* For debugging, ensure entire requested range is in this dserver */
	tmp = offset + count - 1;
	do_div(tmp, layout->stripe_unit);
	tmp2 = do_div(tmp, dsaddr->stripe_count) + layout->first_stripe_index;
	end_idx = do_div(tmp2, dsaddr->stripe_count);

	dprintk("%s: offset=%Lu, count=%Zu, si=%u, dsi=%u, "
		"stripe_count=%u, stripe_unit=%u first_stripe_index %u\n",
		__func__,
		offset, count, stripe_idx, end_idx, dsaddr->stripe_count,
		layout->stripe_unit, layout->first_stripe_index);

	BUG_ON(end_idx != stripe_idx);
	BUG_ON(stripe_idx >= dsaddr->stripe_count);

	ds_idx = dsaddr->stripe_indices[stripe_idx];
	if (dsaddr->ds_list[ds_idx] == NULL) {
		printk(KERN_ERR "%s: No data server for device id (%s)!! \n",
			__func__, deviceid_fmt(&layout->dev_id));
		return 1;
	}

	if (!dsaddr->ds_list[ds_idx]->ds_clp) {
		int err;

		err = nfs4_pnfs_ds_create(mds_srv, dsaddr->ds_list[ds_idx]);
		if (err) {
			printk(KERN_ERR "%s nfs4_pnfs_ds_create error %d\n",
			       __func__, err);
			return 1;
		}
	}
	dserver->ds = dsaddr->ds_list[ds_idx];

	if (layout->num_fh == 1)
		dserver->fh = &layout->fh_array[0];
	else
		dserver->fh = &layout->fh_array[ds_idx];

	dprintk("%s: dev_id=%s, ip:port=%s, ds_idx=%u stripe_idx=%u, "
		"offset=%llu, count=%Zu\n",
		__func__, deviceid_fmt(&layout->dev_id),
		dserver->ds->r_addr,
		ds_idx, stripe_idx, offset, count);

	return 0;
}
