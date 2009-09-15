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

#include <linux/exportfs.h>

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
};

struct nfsd4_pnfs_layoutget {
	struct nfsd4_layout_seg	lg_seg;		/* request */
	u32			lg_signal;	/* request */
	u64			lg_minlength;	/* request */
	u32			lg_maxcount;	/* request */
	struct svc_fh		*lg_fhp;	/* response */
	stateid_t		lg_sid;		/* request/response */
};

#endif /* CONFIG_PNFSD */

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
