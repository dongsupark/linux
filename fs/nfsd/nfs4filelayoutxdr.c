/*
*  linux/fs/nfsd/nfs4filelayout_xdr.c
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
*
*/
#if defined(CONFIG_PNFSD)

#include <linux/module.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/cache.h>
#include <linux/nfs4.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/xdr4.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfsd/nfsd4_pnfs.h>

#define NFSDDBG_FACILITY	NFSDDBG_PNFS

/* Calculate the XDR length of the GETDEVICEINFO4resok structure
 * excluding the gdir_notification and the gdir_device_addr da_layout_type.
 */
static int fl_devinfo_xdr_length(struct pnfs_filelayout_device *fdev)
{
	struct pnfs_filelayout_devaddr *fl_addr;
	struct pnfs_filelayout_multipath *mp;
	int i, j, length;

	/* da_addr_body length, indice length, indices,
	 * multipath_list4 length */
	length = 4 + 4 + (fdev->fl_stripeindices_length * 4) + 4;
	for (i = 0; i < fdev->fl_device_length; i++) {
		mp = &fdev->fl_device_list[i];
		length += 4; /* multipath list length */
		for (j = 0; j < mp->fl_multipath_length; j++) {
			fl_addr = mp->fl_multipath_list;
			length += 4 + XDR_QUADLEN(fl_addr->r_netid.len);
			length += 4 + XDR_QUADLEN(fl_addr->r_addr.len);
		}
	}
	dprintk("<-- %s length %d\n", __func__, length);
	return length;
}

/* Encodes the nfsv4_1_file_layout_ds_addr4 structure from draft 13
 * on the response stream.
 * Use linux error codes (not nfs) since these values are being
 * returned to the file system.
 */
int
filelayout_encode_devinfo(struct pnfs_xdr_info *info, void *device)
{
	unsigned int i, j, len, opaque_len;
	struct nfsd4_compoundres *resp = info->resp;
	u32 *p_in = resp->p;
	struct pnfs_filelayout_device *fdev = device;
	struct xdr_buf *xb = &resp->rqstp->rq_res;
	u32 index_count = fdev->fl_stripeindices_length;
	u32 dev_count = fdev->fl_device_length;
	int error = 0;
	int maxcount = info->maxcount;

	ENCODE_HEAD;

	info->bytes_written = 0; /* in case there is an error */

	opaque_len = fl_devinfo_xdr_length(fdev);
	dprintk("%s: Begin indx_cnt: %u dev_cnt: %u total size %u\n",
		__func__,
		index_count,
		dev_count,
		opaque_len);

	maxcount -= opaque_len; /* da_layout_type and notification bitmap
				 * already subtracted from maxcount in
				 * nfs4xdr.c:encode_getdevinfo(). */
	if (maxcount < 0 && info->maxcount != 0) {
		info->bytes_written = opaque_len;
		error =  -ETOOSMALL;
		goto out;
	}

	if (fdev->fl_enc_stripe_indices) {
		/* Multi-page getdeviceinfo places the index into a page */
		error = fdev->fl_enc_stripe_indices(info, device);
		if (error)
			goto out;
		p = resp->p;
	} else {
		/* check space for opaque length */
		RESERVE_SPACE(opaque_len);

		/* Fill in length later */
		p++;

		/* encode device list indices */
		WRITE32(index_count);

		for (i = 0; i < index_count; i++)
			WRITE32(fdev->fl_stripeindices_list[i]);
	}
	/* encode device list */
	WRITE32(dev_count);
	ADJUST_ARGS();
	for (i = 0; i < dev_count; i++) {
		struct pnfs_filelayout_multipath *mp = &fdev->fl_device_list[i];

		WRITE32(mp->fl_multipath_length);
		for (j = 0; j < mp->fl_multipath_length; j++) {
			struct pnfs_filelayout_devaddr *da =
						&mp->fl_multipath_list[j];

			/* Encode device info */
			WRITE32(da->r_netid.len);
			WRITEMEM(da->r_netid.data, da->r_netid.len);
			WRITE32(da->r_addr.len);
			WRITEMEM(da->r_addr.data, da->r_addr.len);
		}
		ADJUST_ARGS();
	}

	/* backfill in length. Subtract 4 for da_addr_body size */
	len = (char *)p - (char *)p_in + xb->page_len;
	*p_in = htonl(len - 4);

	/* update num bytes written */
	info->bytes_written = len;

	error = 0;
out:
	dprintk("%s: End err %d xdrlen %d\n",
		__func__, error, info->bytes_written);
	return error;
}
EXPORT_SYMBOL(filelayout_encode_devinfo);

#endif /* CONFIG_PNFSD */
