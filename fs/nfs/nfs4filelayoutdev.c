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

#ifdef CONFIG_PNFS

#include <linux/completion.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/hash.h>

#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_xdr.h>

#include <asm/div64.h>

#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/pnfs_xdr.h>
#include <linux/nfs41_session_recovery.h>
#include "nfs4filelayout.h"
#include "internal.h"
#include "nfs4_fs.h"

#define NFSDBG_FACILITY		NFSDBG_PNFS_LD

struct nfs4_file_layout_dsaddr *nfs4_file_layout_dsaddr_get(
					struct filelayout_mount_type *mt,
					struct pnfs_deviceid *dev_id);
struct nfs4_file_layout_dsaddr *nfs4_pnfs_device_item_find(
					struct nfs4_pnfs_dev_hlist *hlist,
					struct pnfs_deviceid *dev_id);

void
print_ds_list(struct nfs4_multipath *multipath)
{
	struct nfs4_pnfs_ds *ds;
	int i;

	ds = multipath->ds_list[0];
	if (ds == NULL) {
		dprintk("%s NULL device \n", __func__);
		return;
	}
	for (i = 0; i < multipath->num_ds; i++) {
		dprintk("        ip_addr %x\n", ntohl(ds->ds_ip_addr));
		dprintk("        port %hu\n", ntohs(ds->ds_port));
		dprintk("        client %p\n", ds->ds_clp);
		dprintk("        ref count %d\n", atomic_read(&ds->ds_count));
		if (ds->ds_clp)
			dprintk("        cl_exchange_flags %x\n",
					    ds->ds_clp->cl_exchange_flags);
		dprintk("        ip:port %s\n", ds->r_addr);
		ds++;
	}
}

void
print_multipath_list(struct nfs4_file_layout_dsaddr *dsaddr)
{
	struct nfs4_multipath *multipath;
	int i;

	multipath = &dsaddr->multipath_list[0];
	dprintk("%s dsaddr->multipath_count %d\n", __func__,
		dsaddr->multipath_count);
	for (i = 0; i < dsaddr->multipath_count; i++) {
		dprintk("        num_ds %d\n", multipath->num_ds);
		print_ds_list(multipath);
		multipath++;
	}
}

/* Debugging function assuming a 64bit major/minor split of the deviceid */
char *
deviceid_fmt(const struct pnfs_deviceid *dev_id)
{
	static char buf[17];
	uint32_t *p = (uint32_t *)dev_id->data;
	uint64_t major, minor;

	READ64(major);
	READ64(minor);

	sprintf(buf, "%08llu %08llu", major, minor);
	return buf;
}

unsigned long
_deviceid_hash(const struct pnfs_deviceid *dev_id)
{
	unsigned char *cptr = (unsigned char *)dev_id->data;
	unsigned int nbytes = NFS4_PNFS_DEVICEID4_SIZE;
	u64 x = 0;

	while (nbytes--) {
		x *= 37;
		x += *cptr++;
	}
	return x & NFS4_PNFS_DEV_HASH_MASK;
}

/* Assumes lock is held */
static inline struct nfs4_file_layout_dsaddr *
_device_lookup(struct nfs4_pnfs_dev_hlist *hlist,
	       const struct pnfs_deviceid *dev_id)
{
	unsigned long      hash;
	struct hlist_node *np;

	dprintk("_device_lookup: dev_id=%s\n", deviceid_fmt(dev_id));

	hash = _deviceid_hash(dev_id);

	hlist_for_each(np, &hlist->dev_list[hash]) {
		struct nfs4_file_layout_dsaddr *dsaddr;
		dsaddr = hlist_entry(np, struct nfs4_file_layout_dsaddr,
				  hash_node);
		if (!memcmp(&dsaddr->dev_id, dev_id, NFS4_PNFS_DEVICEID4_SIZE))
			return dsaddr;
	}
	return NULL;
}

/* Assumes lock is held */
static inline struct nfs4_pnfs_ds *
_data_server_lookup(struct nfs4_pnfs_dev_hlist *hlist, u32 ip_addr, u32 port)
{
	unsigned long      hash;
	struct hlist_node *np;

	dprintk("_data_server_lookup: ip_addr=%x port=%hu\n",
			ntohl(ip_addr), ntohs(port));

	hash = hash_long(ip_addr, NFS4_PNFS_DEV_HASH_BITS);

	hlist_for_each(np, &hlist->dev_dslist[hash]) {
		struct nfs4_pnfs_ds *ds;
		ds = hlist_entry(np, struct nfs4_pnfs_ds, ds_node);
		if (ds->ds_ip_addr == ip_addr &&
		    ds->ds_port == port) {
			return ds;
		}
	}
	return NULL;
}


/* Assumes lock is held */
static inline void
_device_add(struct nfs4_pnfs_dev_hlist *hlist,
	    struct nfs4_file_layout_dsaddr *dsaddr)
{
	unsigned long      hash;

	dprintk("_device_add: dev_id=%s\nmultipath_list:\n",
		deviceid_fmt(&dsaddr->dev_id));
	print_multipath_list(dsaddr);

	hash = _deviceid_hash(&dsaddr->dev_id);
	hlist_add_head(&dsaddr->hash_node, &hlist->dev_list[hash]);
}

/* Assumes lock is held */
static inline void
_data_server_add(struct nfs4_pnfs_dev_hlist *hlist, struct nfs4_pnfs_ds *ds)
{
	unsigned long      hash;

	dprintk("_data_server_add: ip_addr=%x port=%hu\n",
			ntohl(ds->ds_ip_addr), ntohs(ds->ds_port));

	hash = hash_long(ds->ds_ip_addr, NFS4_PNFS_DEV_HASH_BITS);
	hlist_add_head(&ds->ds_node, &hlist->dev_dslist[hash]);
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
	struct nfs_client 	*clp;
	char			ip_addr[16];
	int			addrlen;
	int err = 0;

	dprintk("--> %s ip:port %s\n", __func__, ds->r_addr);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ds->ds_ip_addr;
	sin.sin_port = ds->ds_port;

	/* Set timeout to the mds rpc clnt value.
	 * XXX - find the correct authflavor....
	 *
	 * Fake a client ipaddr (used for sessionid) with hostname
	 * Use hostname since it might be more unique than ipaddr (which
	 * could be set to the loopback 127.0.0/1.1
	 *
	 * XXX: Should sessions continue to use the cl_ipaddr field?
	 */
	addrlen = strnlen(utsname()->nodename, sizeof(utsname()->nodename));
	if (addrlen > sizeof(ip_addr))
		addrlen = sizeof(ip_addr);
	memcpy(ip_addr, utsname()->nodename, addrlen);

	/* XXX need a non-nfs_client struct interface to set up
	 * data server sessions
	 *
	 * tmp: nfs4_set_client sets the nfs_server->nfs_client.
	 *
	 * We specify a retrans and timeout interval equual to MDS. ??
	 */
	err = nfs4_set_client(&tmp,
			      mds_srv->nfs_client->cl_hostname,
			      (struct sockaddr *)&sin,
			      sizeof(struct sockaddr),
			      ip_addr,
			      RPC_AUTH_UNIX,
			      IPPROTO_TCP,
			      mds_clnt->cl_xprt->timeout,
			      1 /* minorversion */);
	if (err < 0)
		goto out;

	clp = tmp.nfs_client;

	/* Set DS lease equal to the MDS lease. */
	spin_lock(&mds_srv->nfs_client->cl_lock);
	clp->cl_lease_time = mds_srv->nfs_client->cl_lease_time;
	spin_unlock(&mds_srv->nfs_client->cl_lock);
	clp->cl_last_renewal = jiffies;

	/* Set exchange id and create session flags and setup session */
	dprintk("%s EXCHANGE_ID for clp %p\n", __func__, clp);
	clp->cl_exchange_flags = EXCHGID4_FLAG_USE_PNFS_DS;
	err = nfs4_recover_expired_lease(clp);
	if (err)
		goto out_put;
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
	if (ds->ds_clp)
		nfs_put_client(ds->ds_clp);
	kfree(ds);
}

/* Assumes lock is NOT held */
static void
device_destroy(struct nfs4_file_layout_dsaddr *dsaddr,
	       struct nfs4_pnfs_dev_hlist *hlist)
{
	struct nfs4_multipath *multipath;
	struct nfs4_pnfs_ds *ds;
	HLIST_HEAD(release);
	int i, j;

	if (!dsaddr)
		return;

	dprintk("device_destroy: dev_id=%s\ndev_list:\n",
		deviceid_fmt(&dsaddr->dev_id));
	print_multipath_list(dsaddr);

	write_lock(&hlist->dev_lock);
	hlist_del_init(&dsaddr->hash_node);

	multipath = &dsaddr->multipath_list[0];
	for (i = 0; i < dsaddr->multipath_count; i++) {
		for (j = 0; j < multipath->num_ds; j++) {
			ds = multipath->ds_list[j];
			if (ds != NULL) {
				/* if we are last user - move to release list */
				if (atomic_dec_and_test(&ds->ds_count)) {
					hlist_del_init(&ds->ds_node);
					hlist_add_head(&ds->ds_node, &release);
				}
			}
		}
		multipath++;
	}
	write_unlock(&hlist->dev_lock);
	while (!hlist_empty(&release)) {
		ds = hlist_entry(release.first, struct nfs4_pnfs_ds, ds_node);
		hlist_del(&ds->ds_node);
		destroy_ds(ds);
	}
	kfree(dsaddr->multipath_list);
	kfree(dsaddr->stripe_indices);
	kfree(dsaddr);
}

int
nfs4_pnfs_devlist_init(struct nfs4_pnfs_dev_hlist *hlist)
{
	int i;

	rwlock_init(&hlist->dev_lock);

	for (i = 0; i < NFS4_PNFS_DEV_HASH_SIZE; i++) {
		INIT_HLIST_HEAD(&hlist->dev_list[i]);
		INIT_HLIST_HEAD(&hlist->dev_dslist[i]);
	}

	return 0;
}

/* De-alloc all devices for a mount point.  This is called in
 * nfs4_kill_super.
 */
void
nfs4_pnfs_devlist_destroy(struct nfs4_pnfs_dev_hlist *hlist)
{
	int i;

	if (hlist == NULL)
		return;

	/* No lock held, as synchronization should occur at upper levels */
	for (i = 0; i < NFS4_PNFS_DEV_HASH_SIZE; i++) {
		struct hlist_node *np, *next;

		hlist_for_each_safe(np, next, &hlist->dev_list[i]) {
			struct nfs4_file_layout_dsaddr *dsaddr;
			dsaddr = hlist_entry(np,
					     struct nfs4_file_layout_dsaddr,
					     hash_node);
			/* device_destroy grabs hlist->dev_lock */
			device_destroy(dsaddr, hlist);
		}
	}
}

/* De-alloc a devices for a mount point. */
void
nfs4_pnfs_dev_destroy(struct nfs4_pnfs_dev_hlist *hlist,
			struct pnfs_deviceid *dev_id)
{
	struct nfs4_file_layout_dsaddr *dsaddr;

	if (hlist == NULL)
		return;

	dprintk("%s: dev_id=%s\n", __func__, deviceid_fmt(dev_id));

	dsaddr = nfs4_pnfs_device_item_find(hlist, dev_id);
	if (dsaddr)
		/* device_destroy grabs hlist->dev_lock */
		device_destroy(dsaddr, hlist);
}

/*
 * Add the device to the list of available devices for this mount point.
 * The * rpc client is created during first I/O.
 */
static int
nfs4_pnfs_device_add(struct filelayout_mount_type *mt,
		     struct nfs4_file_layout_dsaddr *dsaddr)
{
	struct nfs4_file_layout_dsaddr *tmp_dsaddr;
	struct nfs4_pnfs_dev_hlist *hlist = mt->hlist;

	dprintk("nfs4_pnfs_device_add\n");

	/* Write lock, do lookup again, and then add device */
	write_lock(&hlist->dev_lock);
	tmp_dsaddr = _device_lookup(hlist, &dsaddr->dev_id);
	if (tmp_dsaddr == NULL)
		_device_add(hlist, dsaddr);
	write_unlock(&hlist->dev_lock);

	/* Cleanup, if device was recently added */
	if (tmp_dsaddr != NULL) {
		dprintk(" device found, not adding (after creation)\n");
		device_destroy(dsaddr, hlist);
	}

	return 0;
}

static void
nfs4_pnfs_ds_add(struct filelayout_mount_type *mt, struct nfs4_pnfs_ds **dsp,
		 u32 ip_addr, u32 port, char *r_addr, int len)
{
	struct nfs4_pnfs_ds *tmp_ds, *ds;
	struct nfs4_pnfs_dev_hlist *hlist = mt->hlist;

	*dsp = NULL;

	ds = kzalloc(sizeof(*tmp_ds), GFP_KERNEL);
	if (!ds)
		return;

	/* Initialize ds */
	ds->ds_ip_addr = ip_addr;
	ds->ds_port = port;
	strncpy(ds->r_addr, r_addr, len);
	atomic_set(&ds->ds_count, 1);
	INIT_HLIST_NODE(&ds->ds_node);
	ds->ds_clp = NULL;

	write_lock(&hlist->dev_lock);
	tmp_ds = _data_server_lookup(hlist, ip_addr, port);
	if (tmp_ds == NULL) {
		dprintk("%s add new data server ip 0x%x\n", __func__,
				ds->ds_ip_addr);
		_data_server_add(hlist, ds);
		*dsp = ds;
	}
	if (tmp_ds != NULL) {
		destroy_ds(ds);
		atomic_inc(&tmp_ds->ds_count);
		dprintk("%s data server found ip 0x%x, inc'ed ds_count to %d\n",
				__func__, tmp_ds->ds_ip_addr,
				atomic_read(&tmp_ds->ds_count));
		*dsp = tmp_ds;
	}
	write_unlock(&hlist->dev_lock);
}

static struct nfs4_pnfs_ds *
decode_and_add_ds(uint32_t **pp, struct filelayout_mount_type *mt)
{
	struct nfs_server *mds_srv = NFS_SB(mt->fl_sb);
	struct nfs4_pnfs_ds *ds = NULL;
	char r_addr[29]; /* max size of ip/port string */
	int len, err;
	u32 ip_addr, port;
	int tmp[6];
	uint32_t *p = *pp;

	dprintk("%s enter\n", __func__);
	/* check and skip r_netid */
	READ32(len);
	/* "tcp" */
	if (len != 3) {
		printk("%s: ERROR: non TCP r_netid len %d\n",
			__func__, len);
		goto out_err;
	}
	/* Read the bytes into a temporary buffer */
	/* XXX: should probably sanity check them */
	READ32(tmp[0]);

	READ32(len);
	if (len > 29) {
		printk("%s: ERROR: Device ip/port too long (%d)\n",
			__func__, len);
		goto out_err;
	}
	COPYMEM(r_addr, len);
	*pp = p;
	r_addr[len] = '\0';
	sscanf(r_addr, "%d.%d.%d.%d.%d.%d", &tmp[0], &tmp[1],
	       &tmp[2], &tmp[3], &tmp[4], &tmp[5]);
	ip_addr = htonl((tmp[0]<<24) | (tmp[1]<<16) | (tmp[2]<<8) | (tmp[3]));
	port = htons((tmp[4] << 8) | (tmp[5]));

	nfs4_pnfs_ds_add(mt, &ds, ip_addr, port, r_addr, len);

	/* XXX: Don't connect to data servers here, because we
	 * don't want any un-used (never used!) connections.
	 * We should wait until I/O demands use of the data server.
	 */
	if (!ds->ds_clp) {
		err = nfs4_pnfs_ds_create(mds_srv, ds);
		if (err) {
			printk(KERN_ERR "%s nfs4_pnfs_ds_create error %d\n",
			       __func__, err);
			goto out_err;
		}
	}

	dprintk("%s: addr:port string = %s\n", __func__, r_addr);
	return ds;
out_err:
	dprintk("%s returned NULL\n", __func__);
	return NULL;
}

/* Decode opaque device data and return the result
 */
static struct nfs4_file_layout_dsaddr*
decode_device(struct filelayout_mount_type *mt, struct pnfs_device *pdev)
{
	int i, dummy;
	u8 *indexp;
	uint32_t *p = (u32 *)pdev->area, *indicesp;
	struct nfs4_file_layout_dsaddr *dsaddr;
	struct nfs4_multipath *multipath;

	dsaddr = kzalloc(sizeof(*dsaddr), GFP_KERNEL);
	if (!dsaddr)
		goto out_err;

	/* Get the stripe count (number of stripe index) */
	READ32(dsaddr->stripe_count);
	dprintk("%s stripe count  %d\n", __func__, dsaddr->stripe_count);
	if (dsaddr->stripe_count > NFS4_PNFS_MAX_STRIPE_CNT) {
		printk(KERN_WARNING "%s: stripe count %d greater than "
		       "supported maximum %d\n", __func__,
			dsaddr->stripe_count, NFS4_PNFS_MAX_STRIPE_CNT);
		dsaddr->stripe_count = 0;
		goto out_err_free;
	}

	/* Check the multipath list count */
	indicesp = p;
	p += XDR_QUADLEN(dsaddr->stripe_count << 2);
	READ32(dsaddr->multipath_count);
	dprintk("%s multipath_count %u\n", __func__, dsaddr->multipath_count);
	if (dsaddr->multipath_count > NFS4_PNFS_MAX_MULTI_CNT) {
		printk(KERN_WARNING "%s: multipath count %d greater than "
			"supported maximum %d\n", __func__,
			dsaddr->multipath_count, NFS4_PNFS_MAX_MULTI_CNT);
		dsaddr->multipath_count = 0;
		goto out_err_free;
	}
	dsaddr->stripe_indices = kzalloc(sizeof(u8) * dsaddr->stripe_count,
					 GFP_KERNEL);
	if (!dsaddr->stripe_indices)
		goto out_err_free;

	dsaddr->multipath_list = kzalloc(sizeof(struct nfs4_multipath) *
					 dsaddr->multipath_count, GFP_KERNEL);
	if (!dsaddr->multipath_list)
		goto out_err_free;

	memcpy(&dsaddr->dev_id, &pdev->dev_id, NFS4_PNFS_DEVICEID4_SIZE);

	/* Go back an read stripe indices */
	p = indicesp;
	indexp = &dsaddr->stripe_indices[0];
	for (i = 0; i < dsaddr->stripe_count; i++) {
		READ32(dummy);
		*indexp = dummy; /* bound by NFS4_PNFS_MAX_MULTI_CNT */
		indexp++;
	}
	/* Skip already read multipath list count */
	p++;

	multipath = &dsaddr->multipath_list[0];
	for (i = 0; i < dsaddr->multipath_count; i++) {
		int j;

		READ32(multipath->num_ds);
		if (multipath->num_ds > NFS4_PNFS_MAX_MULTI_DS) {
			printk(KERN_WARNING
			       "%s: Multipath count %d not supported, "
			       "skipping all greater than %d\n", __func__,
				multipath->num_ds, NFS4_PNFS_MAX_MULTI_DS);
		}
		for (j = 0; j < multipath->num_ds; j++) {
			if (j >= NFS4_PNFS_MAX_MULTI_DS) {
				u32 len;
				/* skip extra multipath */
				READ32(len);
				p += XDR_QUADLEN(len);
				READ32(len);
				p += XDR_QUADLEN(len);
				continue;
			}
			multipath->ds_list[j] = decode_and_add_ds(&p, mt);
			if (multipath->ds_list[j] == NULL)
				goto out_err_free;
		}
		multipath++;
	}
	return dsaddr;

out_err_free:
	device_destroy(dsaddr, mt->hlist);
out_err:
	dprintk("%s ERROR: returning NULL\n", __func__);
	return NULL;
}

/* Decode the opaque device specified in 'dev'
 * and add it to the list of available devices for this
 * mount point.
 * Must at some point be followed up with device_destroy
 */
static struct nfs4_file_layout_dsaddr*
decode_and_add_device(struct filelayout_mount_type *mt, struct pnfs_device *dev)
{
	struct nfs4_file_layout_dsaddr *dsaddr;

	dsaddr = decode_device(mt, dev);
	if (!dsaddr) {
		printk(KERN_WARNING "%s: Could not decode device\n",
			__func__);
		device_destroy(dsaddr, mt->hlist);
		return NULL;
	}

	if (nfs4_pnfs_device_add(mt, dsaddr))
		return NULL;

	return dsaddr;
}

/* For each deviceid, if not already in the cache,
 * call getdeviceinfo and add the devices associated with
 * the deviceid to the list of available devices for this
 * mount point.
 * Must at some point be followed up with device_destroy.
 */
int
process_deviceid_list(struct filelayout_mount_type *mt,
		      struct nfs_fh *fh,
		      struct pnfs_devicelist *devlist)
{
	int i;

	dprintk("--> %s: num_devs=%d\n", __func__, devlist->num_devs);

	for (i = 0; i < devlist->num_devs; i++) {
		if (!nfs4_file_layout_dsaddr_get(mt, &devlist->dev_id[i])) {
			printk(KERN_WARNING
			       "<-- %s: Error retrieving device %d\n",
			       __func__, i);
			return 1;
		}
	}
	dprintk("<-- %s: success\n", __func__);
	return 0;
}

/* Retrieve the information for dev_id, add it to the list
 * of available devices, and return it.
 */
static struct nfs4_file_layout_dsaddr *
get_device_info(struct filelayout_mount_type *mt,
		struct pnfs_deviceid *dev_id)
{
	struct pnfs_device *pdev = NULL;
	int maxpages = NFS4_GETDEVINFO_MAXSIZE >> PAGE_SHIFT;
	struct page *pages[maxpages];
	struct nfs4_file_layout_dsaddr *dsaddr = NULL;
	int rc, i, j, minpages = 1;

	dprintk("%s mt %p\n", __func__, mt);
	pdev = kzalloc(sizeof(struct pnfs_device), GFP_KERNEL);
	if (pdev == NULL)
		return NULL;

	/* First try with 1 page */
retry_once:
	dprintk("%s trying minpages %d\n", __func__, minpages);
	for (i = 0; i < minpages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			goto out_free;
	}

	/* set pdev->area */
	if (minpages == 1)
		pdev->area = page_address(pages[0]);
	else if (minpages > 1) {
		pdev->area = vmap(pages, minpages, VM_MAP, PAGE_KERNEL);
		if (!pdev->area)
			goto out_free;
	}

	memcpy(&pdev->dev_id, dev_id, NFS4_PNFS_DEVICEID4_SIZE);
	pdev->layout_type = LAYOUT_NFSV4_FILES;
	pdev->pages = pages;
	pdev->pgbase = 0;
	pdev->pglen = PAGE_SIZE * minpages;
	pdev->mincount = 0;
	/* TODO: Update types when CB_NOTIFY_DEVICEID is available */
	pdev->dev_notify_types = 0;

	rc = pnfs_callback_ops->nfs_getdeviceinfo(mt->fl_sb, pdev);
	/* Retry once with the returned mincount if a page was too small */
	dprintk("%s getdevice info returns %d minpages %d\n", __func__, rc,
		minpages);
	if (rc == -ETOOSMALL && minpages == 1) {
		pdev->area = NULL;
		minpages = (pdev->mincount + PAGE_SIZE - 1) >> PAGE_SHIFT;
		if (minpages > 1 && minpages <= maxpages) {
			__free_page(pages[0]);
			goto retry_once;
		}
	}
	if (rc)
		goto out_free;

	/* Found new device, need to decode it and then add it to the
	 * list of known devices for this mountpoint.
	 */
	dsaddr = decode_and_add_device(mt, pdev);
out_free:
	if (minpages > 1 && pdev->area != NULL)
		vunmap(pdev->area);
	for (j = 0; j < i; j++)
		__free_page(pages[j]);
	kfree(pdev);
	dprintk("<-- %s dsaddr %p\n", __func__, dsaddr);
	return dsaddr;
}

struct nfs4_file_layout_dsaddr *
nfs4_file_layout_dsaddr_get(struct filelayout_mount_type *mt,
			    struct pnfs_deviceid *dev_id)
{
	struct nfs4_file_layout_dsaddr *dsaddr;

	read_lock(&mt->hlist->dev_lock);
	dsaddr = _device_lookup(mt->hlist, dev_id);
	read_unlock(&mt->hlist->dev_lock);

	if (dsaddr == NULL)
		dsaddr = get_device_info(mt, dev_id);
	return dsaddr;
}

struct nfs4_file_layout_dsaddr *
nfs4_pnfs_device_item_find(struct nfs4_pnfs_dev_hlist *hlist,
			   struct pnfs_deviceid *dev_id)
{
	struct nfs4_file_layout_dsaddr *dsaddr;

	read_lock(&hlist->dev_lock);
	dsaddr = _device_lookup(hlist, dev_id);
	read_unlock(&hlist->dev_lock);

	return dsaddr;
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
	struct nfs4_file_layout_dsaddr *dsaddr;
	u64 tmp, tmp2;
	u32 stripe_idx, end_idx, multipath_idx;

	if (!layout)
		return 1;

	dsaddr = nfs4_file_layout_dsaddr_get(FILE_MT(inode), &layout->dev_id);
	if (dsaddr == NULL)
		return 1;

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

	multipath_idx = dsaddr->stripe_indices[stripe_idx];
	dserver->multipath = &dsaddr->multipath_list[multipath_idx];

	if (dserver->multipath == NULL) {
		printk(KERN_ERR "%s: No data server for device id (%s)!! \n",
			__func__, deviceid_fmt(&layout->dev_id));
		return 1;
	}
	if (layout->num_fh == 1)
		dserver->fh = &layout->fh_array[0];
	else
		dserver->fh = &layout->fh_array[multipath_idx];

	dprintk("%s: dev_id=%s, ip:port=%s, multipath_idx=%u stripe_idx=%u, "
		"offset=%llu, count=%Zu\n",
		__func__, deviceid_fmt(&layout->dev_id),
		dserver->multipath->ds_list[0]->r_addr,
		multipath_idx, stripe_idx, offset, count);

	return 0;
}

/* Currently not used.
 * I have disabled checking the device count until we can think of a good way
 * to call nfs4_pnfs_device_put in a generic way from the pNFS client.
 * The only way I think think of is to put the nfs4_file_layout_dsaddr directly
 * in the nfs4_write/read_data structure, which breaks the clear line between
 * the pNFS client and layout drivers.  If I did do this, then I could call
 * an ioctl on the NFSv4 file layout driver to decrement the device count.
 */
#if 0
static void
nfs4_pnfs_device_put(struct nfs_server *server,
		     struct nfs4_pnfs_dev_hlist *hlist,
		     struct nfs4_file_layout_dsaddr *dsaddr)
{
	dprintk("nfs4_pnfs_device_put: dev_id=%u\n", dsaddr->dev_id);
	/* XXX Do we need to invoke this put_client? */
	/* server->rpc_ops->put_client(dsaddr->clp); */
	atomic_dec(&dsaddr->count);
}
#endif

#endif /* CONFIG_PNFS */
