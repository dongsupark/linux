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

#endif /* CONFIG_PNFSD */

#endif /* _LINUX_NFSD_NFSD4_PNFS_H */
