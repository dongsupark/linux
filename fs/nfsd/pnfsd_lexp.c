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
