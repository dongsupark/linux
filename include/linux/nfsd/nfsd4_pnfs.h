/*
*  include/linux/nfsd4_pnfs.h
*
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

#if defined(CONFIG_PNFSD)

#include <linux/nfs_xdr.h>
#include <linux/exportfs.h>
#include <linux/nfsd/nfsfh.h>

typedef struct {
	uint64_t	pnfs_fsid;	/* fsid */
	uint64_t	pnfs_devid;	/* deviceid */
} deviceid_t;

/* XDR stream arguments and results.  Exported file system uses this
 * struct to encode information and return how many bytes were encoded.
 */
struct pnfs_xdr_info {
	struct nfsd4_compoundres *resp;
	u32 maxcount;		/* in */
	u32 bytes_written;	/* out */
};

/* Used by get_device_info to encode a device (da_addr_body in spec)
 * Args:
 * xdr - xdr stream
 * device - pointer to device to be encoded
*/
typedef int (*pnfs_encodedev_t)(struct pnfs_xdr_info *xdr, void *device);

/* Arguments for get_device_info */
struct pnfs_devinfo_arg {
	u32 type;			/* request */
	deviceid_t devid;		/* request */
	u32 notify_types;		/* request/response */
	struct pnfs_xdr_info xdr;	/* request/response */
	pnfs_encodedev_t func;		/* request */
};

/* Used by get_device_iter to retrieve all available devices.
 * Args:
 * type - layout type
 * cookie/verf - index and verifier of current list item
 * export_id - Minor part of deviceid_t
 * eof - end of file?
 */
struct pnfs_deviter_arg {
	u32 type;	/* request */
	u64 cookie;	/* request/response */
	u64 verf;	/* request/response */
	u64 devid;	/* response */
	u32 eof;	/* response */
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
 * func - per layout encoding function
 * export_id - Major part of deviceid_t.  File system uses this
 * to build the deviceid returned in the layout.
 * fh - fs can modify the file handle for use on data servers
 * seg - layout info requested and layout info returned
 * xdr - xdr info
 * return_on_close - true if layout to be returned on file close
 * TODO: use common func with dev?
 */
typedef int (*pnfs_encodelayout_t)(struct pnfs_xdr_info *xdr, void *layout);

/* Arguments for layoutget */
struct pnfs_layoutget_arg {
	u64			minlength;	/* request */
	pnfs_encodelayout_t 	func;		/* request */
	u64			fsid;		/* request */
	struct knfsd_fh		*fh;		/* request/response */
	struct nfsd4_layout_seg	seg;		/* request/response */
	struct pnfs_xdr_info	xdr;		/* request/response */
	u32			return_on_close;/* response */
};

/* pNFS structs */

struct nfsd4_pnfs_getdevlist {
	u32             gd_type;	/* request */
	u32		gd_maxnum;	/* request */
	u64		gd_cookie;	/* request - response */
	u64		gd_verf;	/* request - response */
	struct svc_fh 	*gd_fhp;	/* response */
	u32		gd_eof;		/* response */
};

struct nfsd4_pnfs_getdevinfo {
	u32		gd_type;	/* request */
	deviceid_t	gd_devid;	/* request */
	u32		gd_maxcount;	/* request */
	u32		gd_notify_types; /* request */
	struct super_block *gd_sb;
};

struct nfsd4_pnfs_layoutget {
	struct nfsd4_layout_seg	lg_seg;		/* request */
	u32			lg_signal;	/* request */
	u64			lg_minlength;	/* request */
	u32			lg_maxcount;	/* request */
	struct svc_fh		*lg_fhp;	/* response */
	stateid_t		lg_sid;		/* request/response */
};

struct nfsd4_pnfs_layoutcommit {
	struct nfsd4_layout_seg	lc_seg;		/* request */
	u32			lc_reclaim;	/* request */
	u32			lc_newoffset;	/* request */
	u64			lc_last_wr;	/* request */
	struct nfstime4		lc_mtime;	/* request */
	stateid_t		lc_sid;		/* request */
	u32			lc_up_len;	/* layout length */
	void			*lc_up_layout;	/* decoded by callback */
	u32			lc_size_chg;	/* boolean for response */
	u64			lc_newsize;	/* response */
};

enum layoutreturn_flags {
	LR_FLAG_INTERN = 1 << 0,	/* internal return */
	LR_FLAG_EXPIRE = 1 << 1,	/* return on client expiration */
};

struct nfsd4_pnfs_layoutreturn {
	u32			lr_return_type;	/* request */
	struct nfsd4_layout_seg	lr_seg;		/* request */
	u32			lr_reclaim;	/* request */
	u32			lr_flags;
	stateid_t		lr_sid;		/* request/resopnse */
	u32			lrf_body_len;	/* request */
	void			*lrf_body;	/* request */
	u32			lrs_present;	/* response */
};

struct nfsd4_pnfs_cb_layout {
	u32			cbl_recall_type;	/* request */
	struct nfsd4_layout_seg cbl_seg;		/* request */
	u32			cbl_layoutchanged;	/* request */
	stateid_t		cbl_sid;		/* request */
	struct nfs4_fsid	cbl_fsid;
};

/* pNFS Metadata to Data server state communication */
struct pnfs_get_state {
	u32			dsid;    /* request */
	u64			ino;      /* request */
	stateid_t		stid;     /* request;response */
	clientid_t		clid;     /* response */
	u32			access;    /* response */
	u32			stid_gen;    /* response */
	u32			verifier[2]; /* response */
};

/*
 * callbacks provided by the nfsd
 */
struct pnfsd_cb_operations {
	/* Generic callbacks */
	int (*cb_layout_recall) (struct super_block *, struct inode *,
				 struct nfsd4_pnfs_cb_layout *);
};

struct pnfs_export_operations {
	/* Returns the supported pnfs_layouttype4. */
	int (*layout_type) (struct super_block *);

	/* Retrieve and encode a device onto the xdr stream.
	 * Args:
	 * sb - superblock
	 * arg - layout type, device id, maxcount
	 * arg.xdr - xdr stream for encoding
	 * arg.func - Optional function called by file system to encode
	 * device on xdr stream.
	 */
	int (*get_device_info) (struct super_block *, struct pnfs_devinfo_arg *);
	/* Retrieve all available devices via an iterator */
	int (*get_device_iter) (struct super_block *, struct pnfs_deviter_arg *);

	/* Retrieve and encode a layout onto the xdr stream.
	 * Args:
	 * inode - inode for which to retrieve layout
	 * arg.xdr - xdr stream for encoding
	 * arg.func - Optional function called by file system to encode
	 * layout on xdr stream.
	 */
	int (*layout_get) (struct inode *, struct pnfs_layoutget_arg *);
	/* Commit changes to layout */
	int (*layout_commit) (struct inode *, struct nfsd4_pnfs_layoutcommit *);
	/* Returns the layout */
	int (*layout_return) (struct inode *, struct nfsd4_pnfs_layoutreturn *);
	/* Can layout segments be merged for this layout type? */
	int (*can_merge_layouts) (u32 layout_type);

	/* pNFS Files layout specific operations */

	/* Call fs on DS only */
	int (*get_state) (struct inode *, struct knfsd_fh *,
			  struct pnfs_get_state *);
};

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
#endif /* CONFIG_PNFSD */

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
