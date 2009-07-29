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
	ENCODE_HEAD;
	
	RESERVE_SPACE(16 + (XDR_QUADLEN(bld->u.simple.bld_sig_len) << 2));

	*len += 16 + (XDR_QUADLEN(bld->u.simple.bld_sig_len) << 2);

	WRITE32(1);
	WRITE64(bld->u.simple.bld_offset);
	WRITE32(bld->u.simple.bld_sig_len);
	WRITEMEM(bld->u.simple.bld_sig, bld->u.simple.bld_sig_len);
	ADJUST_ARGS();
	return 0;
}

static int
bl_encode_slice(struct nfsd4_compoundres *resp, pnfs_blocklayout_devinfo_t *bld,
    u32 *len)
{
	ENCODE_HEAD;
	
	RESERVE_SPACE(32);
	
	WRITE64(bld->u.slice.bld_start);
	WRITE64(bld->u.slice.bld_len);

	WRITE32(bld->u.slice.bld_index);
	*len += 20;

	ADJUST_ARGS();
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
	int	i;
	ENCODE_HEAD;

	RESERVE_SPACE(12 + (4 * bld->u.stripe.bld_stripes));
	*len += 12 + (4 * bld->u.stripe.bld_stripes);

	WRITE64(bld->u.stripe.bld_chunk_size);
	WRITE32(bld->u.stripe.bld_stripes);
	for (i = 0; i < bld->u.stripe.bld_stripes; i++)
		WRITE32(bld->u.stripe.bld_stripe_indexs[i]);

	ADJUST_ARGS();
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
	ENCODE_HEAD;
	
	info->bytes_written = 0;
	p = resp->p;
	p += 2;
	len += 8;
	ADJUST_ARGS();
	
	/*
	 * All simple volumes with their signature are required to be listed
	 * first.
	 */
	list_for_each_entry(bld, volumes, bld_list) {
		num_vols++;
		RESERVE_SPACE(20);
		WRITE32(bld->bld_type);
		len += 4;
	
		ADJUST_ARGS();
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
		p = resp->p;
		if (status)
			goto error;
	}
	ADJUST_ARGS();

	/* ---- Fill in the overall length and number of volumes ---- */
	p = layoutlen_p;
	WRITE32(len - 4);
	WRITE32(num_vols);

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
	ENCODE_HEAD;
	
	/*
	 * Save spot for opaque block layout length and number of extents,
	 * fill-in later.
	 */
	RESERVE_SPACE(8);
	p += 2;
	len += 8;
	ADJUST_ARGS();
	
	list_for_each_entry(b, bl_head, bll_list) {
		extents++;
		ADJUST_ARGS();
		RESERVE_SPACE(44);
		WRITE64(b->bll_vol_id.pnfs_fsid);
		WRITE64(b->bll_vol_id.pnfs_devid);
		len += sizeof (deviceid_t);
		
		WRITE64(b->bll_foff);
		len += sizeof (b->bll_foff);
		
		WRITE64(b->bll_len);
		len += sizeof (b->bll_len);
		
		WRITE64(b->bll_soff);
		len += sizeof (b->bll_soff);
		
		WRITE32(b->bll_es);
		len += sizeof (b->bll_es);
	}
	
	ADJUST_ARGS();
	
	/* ---- Fill in the overall length and number of extents ---- */
	p = layoutlen_p;
	WRITE32(len - 4);
	WRITE32(extents);
	
	if (len > info->maxcount)
		return -ETOOSMALL;
	
	/* ---- update number of bytes written ---- */
	info->bytes_written = len;

	return 0;
}
EXPORT_SYMBOL(blocklayout_encode_layout);

#endif /* CONFIG_PNFSD_BLOCK */
