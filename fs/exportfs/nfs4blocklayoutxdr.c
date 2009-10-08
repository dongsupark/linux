/*
 *  linux/fs/nfsd/nfs4blocklayoutxdr.c
 *
 *
 *  Created by Rick McNeal on 3/31/08.
 *  Copyright 2008 __MyCompanyName__. All rights reserved.
 *
 */
#if defined(CONFIG_SPNFS_BLOCK)

#include <linux/module.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/cache.h>
#include <linux/nfs4.h>
#include <linux/nfsd/state.h>
#include <linux/nfsd/xdr4.h>
#include <linux/nfsd/nfs4layoutxdr.h>
#include <linux/nfsd/debug.h>

#define NFSDDBG_FACILITY	NFSDDBG_PNFS

static int
bl_encode_simple(struct nfsd4_compoundres *resp, pnfs_blocklayout_devinfo_t *bld,
    u32 *len)
{
	int bytes = 16 + (XDR_QUADLEN(bld->u.simple.bld_sig_len) << 2);
	__be32 *p = nfsd4_xdr_reserve_space(resp, bytes);

	*len += bytes;

	*p++ = cpu_to_be32(1);
	p = xdr_encode_hyper(p, bld->u.simple.bld_offset);
	*p++ = cpu_to_be32(bld->u.simple.bld_sig_len);
	resp->p = xdr_encode_opaque_fixed(p, bld->u.simple.bld_sig, bld->u.simple.bld_sig_len);
	return 0;
}

static int
bl_encode_slice(struct nfsd4_compoundres *resp, pnfs_blocklayout_devinfo_t *bld,
    u32 *len)
{
	__be32 *p = nfsd4_xdr_reserve_space(resp, 20);

	p = xdr_encode_hyper(p, bld->u.slice.bld_start);
	p = xdr_encode_hyper(p, bld->u.slice.bld_len);

	*p++ = cpu_to_be32(bld->u.slice.bld_index);
	*len += 20;

	resp->p = p;
	return 0;
}

static int
bl_encode_concat(struct nfsd4_compoundres *resp, pnfs_blocklayout_devinfo_t *bld,
    u32 *len)
{
	return -1;
}

static int
bl_encode_stripe(struct nfsd4_compoundres *resp, pnfs_blocklayout_devinfo_t *bld,
    u32 *len)
{
	int	i, bytes = 12 + (4 * bld->u.stripe.bld_stripes);
	__be32 *p = nfsd4_xdr_reserve_space(resp, bytes);

	*len += bytes;

	p = xdr_encode_hyper(p, bld->u.stripe.bld_chunk_size);
	*p++ = cpu_to_be32(bld->u.stripe.bld_stripes);
	for (i = 0; i < bld->u.stripe.bld_stripes; i++)
		*p++ = cpu_to_be32(bld->u.stripe.bld_stripe_indexs[i]);

	resp->p = p;
	return 0;
}

int
blocklayout_encode_devinfo(struct pnfs_xdr_info *info, void *v)
{
	struct nfsd4_compoundres	*resp		= info->resp;
	u32				len		= 0,
					num_vols	= 0,
					*layoutlen_p	= resp->p;
	struct list_head		*volumes	= (struct list_head *)v;
	pnfs_blocklayout_devinfo_t	*bld;
	int				status		= 0;
	__be32 *p;

	info->bytes_written = 0;
	p = nfsd4_xdr_reserve_space(resp, 8);
	p += 2;
	len += 8;
	resp->p = p;

	/*
	 * All simple volumes with their signature are required to be listed
	 * first.
	 */
	list_for_each_entry(bld, volumes, bld_list) {
		num_vols++;
		p = nfsd4_xdr_reserve_space(resp, 4);
		*p++ = cpu_to_be32(bld->bld_type);
		len += 4;
		resp->p = p;
		switch (bld->bld_type) {
			case PNFS_BLOCK_VOLUME_SIMPLE:
				status = bl_encode_simple(resp, bld, &len);
				break;
			case PNFS_BLOCK_VOLUME_SLICE:
				status = bl_encode_slice(resp, bld, &len);
				break;
			case PNFS_BLOCK_VOLUME_CONCAT:
				status = bl_encode_concat(resp, bld, &len);
				break;
			case PNFS_BLOCK_VOLUME_STRIPE:
				status = bl_encode_stripe(resp, bld, &len);
				break;
			default:
				BUG();
		}
		if (status)
			goto error;
	}

	/* ---- Fill in the overall length and number of volumes ---- */
	p = layoutlen_p;
	*p++ = cpu_to_be32(len - 4);
	*p++ = cpu_to_be32(num_vols);

	if (len > info->maxcount)
		return -ETOOSMALL;
	info->bytes_written = len;
error:
	return status;
}
EXPORT_SYMBOL(blocklayout_encode_devinfo);

int
blocklayout_encode_layout(struct pnfs_xdr_info *info, void *l)
{
	struct list_head		*bl_head	= (struct list_head *)l;
	struct nfsd4_compoundres	*resp		= info->resp;
	struct pnfs_blocklayout_layout	*b;
	u32				*layoutlen_p	= resp->p,
					len		= 0,
					extents		= 0;
	__be32 *p;

	/*
	 * Save spot for opaque block layout length and number of extents,
	 * fill-in later.
	 */
	p = nfsd4_xdr_reserve_space(resp, 8);
	p += 2;
	len += 8;
	resp->p = p;

	list_for_each_entry(b, bl_head, bll_list) {
		extents++;
		p = nfsd4_xdr_reserve_space(resp, 44);
		p = xdr_encode_hyper(p, b->bll_vol_id.pnfs_fsid);
		p = xdr_encode_hyper(p, b->bll_vol_id.pnfs_devid);
		len += sizeof (deviceid_t);

		p = xdr_encode_hyper(p, b->bll_foff);
		len += sizeof (b->bll_foff);

		p = xdr_encode_hyper(p, b->bll_len);
		len += sizeof (b->bll_len);

		p = xdr_encode_hyper(p, b->bll_soff);
		len += sizeof (b->bll_soff);

		*p++ = cpu_to_be32(b->bll_es);
		len += sizeof (b->bll_es);
		resp->p = p;
	}

	/* ---- Fill in the overall length and number of extents ---- */
	p = layoutlen_p;
	*p++ = cpu_to_be32(len - 4);
	*p++ = cpu_to_be32(extents);

	if (len > info->maxcount)
		return -ETOOSMALL;

	/* ---- update number of bytes written ---- */
	info->bytes_written = len;

	return 0;
}
EXPORT_SYMBOL(blocklayout_encode_layout);

#endif /* CONFIG_PNFSD_BLOCK */
