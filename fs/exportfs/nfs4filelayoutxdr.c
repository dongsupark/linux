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
 */
#include <linux/exp_xdr.h>
#include <linux/module.h>
#include <linux/nfsd/nfs4layoutxdr.h>

/* We do our-own dprintk so filesystems are not dependent on sunrpc */
#ifdef dprintk
#undef dprintk
#endif
#define dprintk(fmt, args, ...)	do { } while (0)

/* Calculate the XDR length of the GETDEVICEINFO4resok structure
 * excluding the gdir_notification and the gdir_device_addr da_layout_type.
 */
static int fl_devinfo_xdr_words(const struct pnfs_filelayout_device *fdev)
{
	struct pnfs_filelayout_devaddr *fl_addr;
	struct pnfs_filelayout_multipath *mp;
	int i, j, nwords;

	/* da_addr_body length, indice length, indices,
	 * multipath_list4 length */
	nwords = 1 + 1 + fdev->fl_stripeindices_length + 1;
	for (i = 0; i < fdev->fl_device_length; i++) {
		mp = &fdev->fl_device_list[i];
		nwords++; /* multipath list length */
		for (j = 0; j < mp->fl_multipath_length; j++) {
			fl_addr = mp->fl_multipath_list;
			nwords += 1 + exp_xdr_qwords(fl_addr->r_netid.len);
			nwords += 1 + exp_xdr_qwords(fl_addr->r_addr.len);
		}
	}
	dprintk("<-- %s nwords %d\n", __func__, nwords);
	return nwords;
}

/* Encodes the nfsv4_1_file_layout_ds_addr4 structure from draft 13
 * on the response stream.
 * Use linux error codes (not nfs) since these values are being
 * returned to the file system.
 */
int
filelayout_encode_devinfo(struct exp_xdr_stream *xdr,
			  const struct pnfs_filelayout_device *fdev)
{
	unsigned int i, j, len = 0, opaque_words;
	u32 *p_in;
	u32 index_count = fdev->fl_stripeindices_length;
	u32 dev_count = fdev->fl_device_length;
	int error = 0;
	__be32 *p;

	opaque_words = fl_devinfo_xdr_words(fdev);
	dprintk("%s: Begin indx_cnt: %u dev_cnt: %u total size %u\n",
		__func__,
		index_count,
		dev_count,
		opaque_words*4);

	/* check space for opaque length */
	p = p_in = exp_xdr_reserve_qwords(xdr, opaque_words);
	if (!p) {
		error =  -ETOOSMALL;
		goto out;
	}

	/* Fill in length later */
	p++;

	/* encode device list indices */
	p = exp_xdr_encode_u32(p, index_count);
	for (i = 0; i < index_count; i++)
		p = exp_xdr_encode_u32(p, fdev->fl_stripeindices_list[i]);

	/* encode device list */
	p = exp_xdr_encode_u32(p, dev_count);
	for (i = 0; i < dev_count; i++) {
		struct pnfs_filelayout_multipath *mp = &fdev->fl_device_list[i];

		p = exp_xdr_encode_u32(p, mp->fl_multipath_length);
		for (j = 0; j < mp->fl_multipath_length; j++) {
			struct pnfs_filelayout_devaddr *da =
						&mp->fl_multipath_list[j];

			/* Encode device info */
			p = exp_xdr_encode_opaque(p, da->r_netid.data,
						     da->r_netid.len);
			p = exp_xdr_encode_opaque(p, da->r_addr.data,
						     da->r_addr.len);
		}
	}

	/* backfill in length. Subtract 4 for da_addr_body size */
	len = (char *)p - (char *)p_in;
	exp_xdr_encode_u32(p_in, len - 4);

	error = 0;
out:
	dprintk("%s: End err %d xdrlen %d\n",
		__func__, error, len);
	return error;
}
EXPORT_SYMBOL(filelayout_encode_devinfo);
