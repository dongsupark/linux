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

#if defined(CONFIG_PNFSD_LOCAL_EXPORT)

#include <linux/nfs_fs.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/nfsfh.h>
#include <linux/nfsd/pnfsd.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfs4_pnfs.h>

#define NFSDDBG_FACILITY NFSDDBG_PNFS

struct sockaddr pnfsd_lexp_addr;
size_t pnfs_lexp_addr_len;

static int
pnfsd_lexp_layout_type(struct super_block *sb)
{
	int ret = LAYOUT_NFSV4_FILES;
	dprintk("<-- %s: return %d\n", __func__, ret);
	return ret;
}

static int
pnfsd_lexp_get_device_iter(struct super_block *sb,
			   struct pnfs_deviter_arg *arg)
{
	dprintk("--> %s: sb=%p\n", __func__, sb);

	BUG_ON(arg->type != LAYOUT_NFSV4_FILES);

	if (arg->cookie == 0) {
		arg->cookie = 1;
		arg->verf = 1;
		arg->devid = 1;
	} else
		arg->eof = 1;

	dprintk("<-- %s: return 0\n", __func__);
	return 0;
}

#endif /* CONFIG_PNFSD_LOCAL_EXPORT */
