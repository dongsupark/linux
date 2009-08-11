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

#ifndef NFSD_NFS4LAYOUTXDR_H
#define NFSD_NFS4LAYOUTXDR_H

#include <linux/sunrpc/xdr.h>

/* the nfsd4_pnfs_devlist dev_addr for the file layout type */
struct pnfs_filelayout_devaddr {
	struct xdr_netobj	r_netid;
	struct xdr_netobj	r_addr;
};

/* list of multipath servers */
struct pnfs_filelayout_multipath {
	u32				fl_multipath_length;
	struct pnfs_filelayout_devaddr	*fl_multipath_list;
};

struct pnfs_filelayout_device {
	u32					fl_stripeindices_length;
	u32					*fl_stripeindices_list;
	u32					fl_device_length;
	struct pnfs_filelayout_multipath	*fl_device_list;
};

#endif /* NFSD_NFS4LAYOUTXDR_H */
