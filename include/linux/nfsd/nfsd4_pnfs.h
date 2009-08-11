/*
 *  Copyright (c) 2006 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson <andros@umich.edu>
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
 *
 */

#ifndef _LINUX_NFSD_NFSD4_PNFS_H
#define _LINUX_NFSD_NFSD4_PNFS_H

#include <linux/exportfs.h>
#include <linux/exp_xdr.h>

struct nfsd4_pnfs_deviceid {
	u64	sbid;			/* per-superblock unique ID */
	u64	devid;			/* filesystem-wide unique device ID */
};

struct nfsd4_pnfs_dev_iter_res {
	u64		gd_cookie;	/* request/repsonse */
	u64		gd_verf;	/* request/repsonse */
	u64		gd_devid;	/* response */
	u32		gd_eof;		/* response */
};

/*
 * pNFS export operations vector.
 *
 * The filesystem must implement the following methods:
 *   layout_type
 *   get_device_info
 *   layout_get
 *
 * All other methods are optional and can be set to NULL if not implemented.
 */
struct pnfs_export_operations {
	/* Returns the supported pnfs_layouttype4. */
	int (*layout_type) (struct super_block *);

	/* Encode device info onto the xdr stream. */
	int (*get_device_info) (struct super_block *,
				struct exp_xdr_stream *,
				u32 layout_type,
				const struct nfsd4_pnfs_deviceid *);

	/* Retrieve all available devices via an iterator.
	 * arg->cookie == 0 indicates the beginning of the list,
	 * otherwise arg->verf is used to verify that the list hasn't changed
	 * while retrieved.
	 *
	 * On output, the filesystem sets the devid based on the current cookie
	 * and sets res->cookie and res->verf corresponding to the next entry.
	 * When the last entry in the list is retrieved, res->eof is set to 1.
	 */
	int (*get_device_iter) (struct super_block *,
				u32 layout_type,
				struct nfsd4_pnfs_dev_iter_res *);
};

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
