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

struct nfsd4_layout_seg {
	u64	clientid;
	u32	layout_type;
	u32	iomode;
	u64	offset;
	u64	length;
};

/* Used by layout_get to encode layout (loc_body var in spec)
 * Args:
 * minlength - min number of accessible bytes given by layout
 * fsid - Major part of struct pnfs_deviceid.  File system uses this
 * to build the deviceid returned in the layout.
 * fh - fs can modify the file handle for use on data servers
 * seg - layout info requested and layout info returned
 * xdr - xdr info
 * return_on_close - true if layout to be returned on file close
 */

struct nfsd4_pnfs_layoutget_arg {
	u64			lg_minlength;
	u64			lg_sbid;
	const struct knfsd_fh	*lg_fh;
};

struct nfsd4_pnfs_layoutget_res {
	struct nfsd4_layout_seg	lg_seg;	/* request/resopnse */
	u32			lg_return_on_close;
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

	/* Retrieve and encode a layout for inode onto the xdr stream.
	 * arg->minlength is the minimum number of accessible bytes required
	 *   by the client.
	 * The maximum number of bytes to encode the layout is given by
	 *   the xdr stream end pointer.
	 * arg->fsid contains the major part of struct pnfs_deviceid.
	 *   The file system uses this to build the deviceid returned
	 *   in the layout.
	 * res->seg - layout segment requested and layout info returned.
	 * res->fh can be modified the file handle for use on data servers
	 * res->return_on_close - true if layout to be returned on file close
	 *
	 * return one of the following nfs errors:
	 * NFS_OK			Success
	 * NFS4ERR_ACCESS		Permission error
	 * NFS4ERR_BADIOMODE		Server does not support requested iomode
	 * NFS4ERR_BADLAYOUT		No layout matching loga_minlength rules
	 * NFS4ERR_INVAL		Parameter other than layout is invalid
	 * NFS4ERR_IO			I/O error
	 * NFS4ERR_LAYOUTTRYLATER	Layout may be retrieved later
	 * NFS4ERR_LAYOUTUNAVAILABLE	Layout unavailable for this file
	 * NFS4ERR_LOCKED		Lock conflict
	 * NFS4ERR_NOSPC		Out-of-space error occured
	 * NFS4ERR_RECALLCONFLICT	Layout currently unavialable due to
	 *				a conflicting CB_LAYOUTRECALL
	 * NFS4ERR_SERVERFAULT		Server went bezerk
	 * NFS4ERR_TOOSMALL		loga_maxcount too small to fit layout
	 * NFS4ERR_WRONG_TYPE		Wrong file type (not a regular file)
	 */
	u32 (*layout_get) (struct inode *,
			   struct exp_xdr_stream *xdr,
			   const struct nfsd4_pnfs_layoutget_arg *,
			   struct nfsd4_pnfs_layoutget_res *);

	/* Can layout segments be merged for this layout type? */
	int (*can_merge_layouts) (u32 layout_type);
};

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
