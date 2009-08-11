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

#include <linux/nfs_xdr.h>
#include <linux/exportfs.h>
#include <linux/exp_xdr.h>

struct nfsd4_pnfs_deviceid {
	u64	fsid;			/* filesystem ID */
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
	u64			lg_fsid;
	const struct knfsd_fh	*lg_fh;
};

struct nfsd4_pnfs_layoutget_res {
	struct nfsd4_layout_seg	lg_seg;	/* request/resopnse */
	u32			lg_return_on_close;
};

struct nfsd4_pnfs_layoutcommit_arg {
	struct nfsd4_layout_seg	lc_seg;		/* request */
	u32			lc_reclaim;	/* request */
	u32			lc_newoffset;	/* request */
	u64			lc_last_wr;	/* request */
	struct nfstime4		lc_mtime;	/* request */
	u32			lc_up_len;	/* layout length */
	void			*lc_up_layout;	/* decoded by callback */
};

struct nfsd4_pnfs_layoutcommit_res {
	u32			lc_size_chg;	/* boolean for response */
	u64			lc_newsize;	/* response */
};

#define PNFS_LAST_LAYOUT_NO_RECALLS ((void *)-1) /* used with lr_cookie below */

struct nfsd4_pnfs_layoutreturn_arg {
	u32			lr_return_type;	/* request */
	struct nfsd4_layout_seg	lr_seg;		/* request */
	u32			lr_reclaim;	/* request */
	u32			lrf_body_len;	/* request */
	void			*lrf_body;	/* request */
	void			*lr_cookie;	/* fs private */
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
	 */
	int (*layout_get) (struct inode *,
			   struct exp_xdr_stream *xdr,
			   const struct nfsd4_pnfs_layoutget_arg *,
			   struct nfsd4_pnfs_layoutget_res *);

	/* Commit changes to layout */
	int (*layout_commit) (struct inode *,
			      const struct nfsd4_pnfs_layoutcommit_arg *,
			      struct nfsd4_pnfs_layoutcommit_res *);

	/* Returns the layout */
	int (*layout_return) (struct inode *,
			      const struct nfsd4_pnfs_layoutreturn_arg *);

	/* Can layout segments be merged for this layout type? */
	int (*can_merge_layouts) (u32 layout_type);
};

#if defined(CONFIG_PNFSD)

/*
 * fh_fsid_type is overloaded to indicate whether a filehandle was one supplied
 * to a DS by LAYOUTGET.  nfs4_preprocess_stateid_op() uses this to decide how
 * to handle a given stateid.
 */
static inline int pnfs_fh_is_ds(struct knfsd_fh *fh)
{
	return fh->fh_fsid_type >= FSID_MAX;
}

static inline void pnfs_fh_mark_ds(struct knfsd_fh *fh)
{
	BUG_ON(fh->fh_version != 1);
	BUG_ON(pnfs_fh_is_ds(fh));
	fh->fh_fsid_type += FSID_MAX;
}

/* allows fh_verify() to check the real fsid_type (i.e., not overloaded). */
static inline int pnfs_fh_fsid_type(struct knfsd_fh *fh)
{
	int fsid_type = fh->fh_fsid_type;

	if (pnfs_fh_is_ds(fh))
		return fsid_type - FSID_MAX;
	return fsid_type;
}

#else  /* CONFIG_PNFSD */

static inline int pnfs_fh_is_ds(struct knfsd_fh *fh)
{
	return 0;
}

#endif /* CONFIG_PNFSD */

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
