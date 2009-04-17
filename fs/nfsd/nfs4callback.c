/*
 *  linux/fs/nfsd/nfs4callback.c
 *
 *  Copyright (c) 2001 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Kendrick Smith <kmsmith@umich.edu>
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

#include <linux/module.h>
#include <linux/list.h>
#include <linux/inet.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/state.h>
#include <linux/sunrpc/sched.h>
#include <linux/nfs4.h>

#define NFSDDBG_FACILITY                NFSDDBG_PROC

#define NFSPROC4_CB_NULL 0
#define NFSPROC4_CB_COMPOUND 1
#define NFS4_STATEID_SIZE 16

/* Index of predefined Linux callback client operations */

enum {
	NFSPROC4_CLNT_CB_NULL = 0,
	NFSPROC4_CLNT_CB_RECALL,
	NFSPROC4_CLNT_CB_SEQUENCE,
#if defined(CONFIG_PNFSD)
	NFSPROC4_CLNT_CB_LAYOUT,
	NFSPROC4_CLNT_CB_DEVICE,
#endif
};

enum nfs_cb_opnum4 {
	OP_CB_RECALL            = 4,
	OP_CB_LAYOUT            = 5,
	OP_CB_SEQUENCE          = 11,
	OP_CB_DEVICE            = 14,
};

#define NFS4_MAXTAGLEN		20

#define NFS4_enc_cb_null_sz		0
#define NFS4_dec_cb_null_sz		0
#define cb_compound_enc_hdr_sz		4
#define cb_compound_dec_hdr_sz		(3 + (NFS4_MAXTAGLEN >> 2))
#define sessionid_sz			(NFS4_MAX_SESSIONID_LEN >> 2)
#define cb_sequence_enc_sz		(sessionid_sz + 4 +             \
					1 /* no referring calls list yet */)
#define cb_sequence_dec_sz		(op_dec_sz + sessionid_sz + 4)

#define op_enc_sz			1
#define op_dec_sz			2
#define enc_nfs4_fh_sz			(1 + (NFS4_FHSIZE >> 2))
#define enc_stateid_sz			(NFS4_STATEID_SIZE >> 2)
#define NFS4_enc_cb_recall_sz		(cb_compound_enc_hdr_sz +       \
					cb_sequence_enc_sz +            \
					1 + enc_stateid_sz +            \
					enc_nfs4_fh_sz)

#define NFS4_dec_cb_recall_sz		(cb_compound_dec_hdr_sz  +      \
					cb_sequence_dec_sz +            \
					op_dec_sz)
#define NFS4_enc_cb_layout_sz		(cb_compound_enc_hdr_sz +       \
					cb_sequence_enc_sz +            \
					1 + 3 +                         \
					enc_nfs4_fh_sz + 4)
#define NFS4_dec_cb_layout_sz		(cb_compound_dec_hdr_sz  +      \
					cb_sequence_dec_sz +            \
					op_dec_sz)
#define NFS4_enc_cb_device_sz		(cb_compound_enc_hdr_sz +       \
					cb_sequence_enc_sz +            \
					1 + 6)
#define NFS4_dec_cb_device_sz		(cb_compound_dec_hdr_sz  +      \
					cb_sequence_dec_sz +            \
					op_dec_sz)

struct nfs4_rpc_args {
	void                     *args_op;
	struct nfsd4_cb_sequence *args_seq;
};

struct nfs4_rpc_res {
	struct nfsd4_cb_sequence *res_seq;
};

/*
* Generic encode routines from fs/nfs/nfs4xdr.c
*/
static inline __be32 *
xdr_writemem(__be32 *p, const void *ptr, int nbytes)
{
	int tmp = XDR_QUADLEN(nbytes);
	if (!tmp)
		return p;
	p[tmp-1] = 0;
	memcpy(p, ptr, nbytes);
	return p + tmp;
}

#define WRITE32(n)               *p++ = htonl(n)
#define WRITE64(n)               do {				\
	*p++ = htonl((u32)((n) >> 32));				\
	*p++ = htonl((u32)(n));					\
} while (0)
#define WRITEMEM(ptr,nbytes)     do {                           \
	p = xdr_writemem(p, ptr, nbytes);                       \
} while (0)
#define RESERVE_SPACE(nbytes)   do {                            \
	p = xdr_reserve_space(xdr, nbytes);                     \
	if (!p) dprintk("NFSD: RESERVE_SPACE(%d) failed in function %s\n", (int) (nbytes), __func__); \
	BUG_ON(!p);                                             \
} while (0)

/*
 * Generic decode routines from fs/nfs/nfs4xdr.c
 */
#define DECODE_TAIL                             \
	status = 0;                             \
out:                                            \
	return status;                          \
xdr_error:                                      \
	dprintk("NFSD: xdr error! (%s:%d)\n", __FILE__, __LINE__); \
	status = -EIO;                          \
	goto out

#define READ32(x)         (x) = ntohl(*p++)
#define READ64(x)         do {                  \
	(x) = (u64)ntohl(*p++) << 32;           \
	(x) |= ntohl(*p++);                     \
} while (0)
#define READTIME(x)       do {                  \
	p++;                                    \
	(x.tv_sec) = ntohl(*p++);               \
	(x.tv_nsec) = ntohl(*p++);              \
} while (0)
#define READ_BUF(nbytes)  do { \
	p = xdr_inline_decode(xdr, nbytes); \
	if (!p) { \
		dprintk("NFSD: %s: reply buffer overflowed in line %d.\n", \
			__func__, __LINE__); \
		return -EIO; \
	} \
} while (0)
#define COPYMEM(x, nbytes) do {                \
	memcpy((x), p, nbytes);                \
	p += XDR_QUADLEN(nbytes);              \
} while (0)

struct nfs4_cb_compound_hdr {
	/* args */
	u32		ident;	/* minorversion 0 only */
	u32		nops;
	__be32		*nops_p;
	u32		minorversion;
	/* res */
	int		status;
	u32		taglen;
	char		*tag;
};

static struct {
int stat;
int errno;
} nfs_cb_errtbl[] = {
	{ NFS4_OK,		0               },
	{ NFS4ERR_PERM,		EPERM           },
	{ NFS4ERR_NOENT,	ENOENT          },
	{ NFS4ERR_IO,		EIO             },
	{ NFS4ERR_NXIO,		ENXIO           },
	{ NFS4ERR_ACCESS,	EACCES          },
	{ NFS4ERR_EXIST,	EEXIST          },
	{ NFS4ERR_XDEV,		EXDEV           },
	{ NFS4ERR_NOTDIR,	ENOTDIR         },
	{ NFS4ERR_ISDIR,	EISDIR          },
	{ NFS4ERR_INVAL,	EINVAL          },
	{ NFS4ERR_FBIG,		EFBIG           },
	{ NFS4ERR_NOSPC,	ENOSPC          },
	{ NFS4ERR_ROFS,		EROFS           },
	{ NFS4ERR_MLINK,	EMLINK          },
	{ NFS4ERR_NAMETOOLONG,	ENAMETOOLONG    },
	{ NFS4ERR_NOTEMPTY,	ENOTEMPTY       },
	{ NFS4ERR_DQUOT,	EDQUOT          },
	{ NFS4ERR_STALE,	ESTALE          },
	{ NFS4ERR_BADHANDLE,	EBADHANDLE      },
	{ NFS4ERR_BAD_COOKIE,	EBADCOOKIE      },
	{ NFS4ERR_NOTSUPP,	ENOTSUPP        },
	{ NFS4ERR_TOOSMALL,	ETOOSMALL       },
	{ NFS4ERR_SERVERFAULT,	ESERVERFAULT    },
	{ NFS4ERR_BADTYPE,	EBADTYPE        },
	{ NFS4ERR_LOCKED,	EAGAIN          },
	{ NFS4ERR_RESOURCE,	EREMOTEIO       },
	{ NFS4ERR_SYMLINK,	ELOOP           },
	{ NFS4ERR_OP_ILLEGAL,	EOPNOTSUPP      },
	{ NFS4ERR_DEADLOCK,	EDEADLK         },
	{ -1,                   EIO             }
};

static int
nfs_cb_stat_to_errno(int stat)
{
	int i;
	for (i = 0; nfs_cb_errtbl[i].stat != -1; i++) {
		if (nfs_cb_errtbl[i].stat == stat)
			return nfs_cb_errtbl[i].errno;
	}
	/* If we cannot translate the error, the recovery routines should
	* handle it.
	* Note: remaining NFSv4 error codes have values > 10000, so should
	* not conflict with native Linux error codes.
	*/
	return stat;
}

/*
 * XDR encode
 */

static void
encode_stateid(struct xdr_stream *xdr, stateid_t *sid)
{
	__be32 *p;

	RESERVE_SPACE(sizeof(stateid_t));
	WRITE32(sid->si_generation);
	WRITEMEM(&sid->si_opaque, sizeof(stateid_opaque_t));
}

static void
encode_cb_compound_hdr(struct xdr_stream *xdr, struct nfs4_cb_compound_hdr *hdr)
{
	__be32 * p;

	RESERVE_SPACE(16);
	WRITE32(0);            /* tag length is always 0 */
	WRITE32(hdr->minorversion);
	WRITE32(hdr->ident);
	hdr->nops_p = p;
	WRITE32(hdr->nops);
}

static void encode_cb_nops(struct nfs4_cb_compound_hdr *hdr)
{
	*hdr->nops_p = htonl(hdr->nops);
}

static void
encode_cb_recall(struct xdr_stream *xdr, struct nfs4_cb_recall *cb_rec,
		 struct nfs4_cb_compound_hdr *hdr)
{
	__be32 *p;
	int len = cb_rec->cbr_fhlen;

	RESERVE_SPACE(4);
	WRITE32(OP_CB_RECALL);
	encode_stateid(xdr, &cb_rec->cbr_stateid);
	RESERVE_SPACE(8 + len);
	WRITE32(cb_rec->cbr_trunc);
	WRITE32(len);
	WRITEMEM(cb_rec->cbr_fhval, len);
	hdr->nops++;
}

static void
encode_cb_sequence(struct xdr_stream *xdr, struct nfsd4_cb_sequence *args,
		   struct nfs4_cb_compound_hdr *hdr)
{
	__be32 *p;

	if (hdr->minorversion == 0)
		return;

	RESERVE_SPACE(1 + NFS4_MAX_SESSIONID_LEN + 20);

	WRITE32(OP_CB_SEQUENCE);
#ifdef CONFIG_NFSD_V4_1
	WRITEMEM(args->cbs_clp->cl_sessionid.data, NFS4_MAX_SESSIONID_LEN);
	WRITE32(args->cbs_clp->cl_cb_seq_nr);
#endif /* CONFIG_NFSD_V4_1 */
	WRITE32(0);		/* slotid, always 0 */
	WRITE32(0);		/* highest slotid always 0 */
	WRITE32(0);		/* cachethis always 0 */
	WRITE32(0); /* FIXME: support referring_call_lists */
	hdr->nops++;
}

#if defined(CONFIG_PNFSD)
static void
encode_cb_layout(struct xdr_stream *xdr, struct nfs4_layoutrecall *clr,
		 struct nfs4_cb_compound_hdr *hdr)
{
	u32 *p;

	BUG_ON(hdr->minorversion == 0);

	RESERVE_SPACE(20);
	WRITE32(OP_CB_LAYOUT);
	WRITE32(clr->cb.cbl_seg.layout_type);
	WRITE32(clr->cb.cbl_seg.iomode);
	WRITE32(clr->cb.cbl_layoutchanged);
	WRITE32(clr->cb.cbl_recall_type);
	if (unlikely(clr->cb.cbl_recall_type == RECALL_FSID)) {
		struct nfs4_fsid fsid = clr->cb.cbl_fsid;

		RESERVE_SPACE(16);
		WRITE64(fsid.major);
		WRITE64(fsid.minor);
		dprintk("%s: type %x iomode %d changed %d recall_type %d "
			"fsid 0x%llx-0x%llx\n",
			__func__, clr->cb.cbl_seg.layout_type,
			clr->cb.cbl_seg.iomode, clr->cb.cbl_layoutchanged,
			clr->cb.cbl_recall_type, fsid.major, fsid.minor);
	} else if (clr->cb.cbl_recall_type == RECALL_FILE) {
		int len = clr->clr_file->fi_fhlen;

		RESERVE_SPACE(20 + len);
		WRITE32(len);
		WRITEMEM(clr->clr_file->fi_fhval, len);
		WRITE64(clr->cb.cbl_seg.offset);
		WRITE64(clr->cb.cbl_seg.length);
		encode_stateid(xdr, &clr->cb.cbl_sid);
		dprintk("%s: type %x iomode %d changed %d recall_type %d "
			"offset %lld length %lld stateid " STATEID_FMT "\n",
			__func__, clr->cb.cbl_seg.layout_type,
			clr->cb.cbl_seg.iomode, clr->cb.cbl_layoutchanged,
			clr->cb.cbl_recall_type,
			clr->cb.cbl_seg.offset, clr->cb.cbl_seg.length,
			STATEID_VAL(&clr->cb.cbl_sid));
	} else
		dprintk("%s: type %x iomode %d changed %d recall_type %d\n",
			__func__, clr->cb.cbl_seg.layout_type,
			clr->cb.cbl_seg.iomode, clr->cb.cbl_layoutchanged,
			clr->cb.cbl_recall_type);
	hdr->nops++;
}

static void
encode_cb_device(struct xdr_stream *xdr, struct nfs4_notify_device *nd,
		 struct nfs4_cb_compound_hdr *hdr)
{
	u32 *p;
	int i;
	int len					= nd->nd_list->cbd_len;
	struct nfsd4_pnfs_cb_dev_item *cbd	= nd->nd_list->cbd_list;

	dprintk("NFSD %s: --> num %d\n", __func__, len);

	BUG_ON(hdr->minorversion == 0);

	RESERVE_SPACE(8);
	WRITE32(OP_CB_DEVICE);

	/* notify4 cnda_changes<>; */
	WRITE32(len);
	for (i = 0; i < len; i++) {
		dprintk("%s: nt %d lt %d devid x%llx-x%llx im %d i %d\n",
			__func__, cbd[i].cbd_notify_type,
			cbd[i].cbd_layout_type,
			cbd[i].cbd_devid.pnfs_fsid,
			cbd[i].cbd_devid.pnfs_devid,
			cbd[i].cbd_immediate, i);

		BUG_ON(cbd[i].cbd_notify_type != NOTIFY_DEVICEID4_CHANGE &&
		       cbd[i].cbd_notify_type != NOTIFY_DEVICEID4_DELETE);
		RESERVE_SPACE(28);
		/* bitmap4         notify_mask; */
		WRITE32(cbd[i].cbd_notify_type);
		/* opaque     notify_vals<>; */
		if (cbd[i].cbd_notify_type == NOTIFY_DEVICEID4_CHANGE)
			WRITE32(24);
		else
			WRITE32(20);
		WRITE32(cbd[i].cbd_layout_type);
		WRITE64(cbd[i].cbd_devid.pnfs_fsid);
		WRITE64(cbd[i].cbd_devid.pnfs_devid);

		if (cbd[i].cbd_notify_type == NOTIFY_DEVICEID4_CHANGE) {
			RESERVE_SPACE(4);
			WRITE32(cbd[i].cbd_immediate);
		}
	}
	hdr->nops++;
}
#endif /* CONFIG_PNFSD */

static int
nfs4_xdr_enc_cb_null(struct rpc_rqst *req, __be32 *p)
{
	struct xdr_stream xdrs, *xdr = &xdrs;

	xdr_init_encode(&xdrs, &req->rq_snd_buf, p);
        RESERVE_SPACE(0);
	return 0;
}

static int
nfs4_xdr_enc_cb_recall(struct rpc_rqst *req, __be32 *p,
		       struct nfs4_rpc_args *rpc_args)
{
	struct xdr_stream xdr;
	struct nfs4_cb_recall *args = rpc_args->args_op;
	struct nfs4_callback *cb =
		(struct nfs4_callback *)req->rq_task->tk_client->cl_private;
	struct nfs4_cb_compound_hdr hdr = {
		.ident = args->cbr_ident,
		.minorversion = cb->cb_minorversion,
	};

	xdr_init_encode(&xdr, &req->rq_snd_buf, p);
	encode_cb_compound_hdr(&xdr, &hdr);
	encode_cb_sequence(&xdr, rpc_args->args_seq, &hdr);
	encode_cb_recall(&xdr, args, &hdr);
	encode_cb_nops(&hdr);
	return 0;
}

#if defined(CONFIG_PNFSD)
static int
nfs4_xdr_enc_cb_layout(struct rpc_rqst *req, u32 *p,
		       struct nfs4_rpc_args *rpc_args)
{
	struct xdr_stream xdr;
	struct nfs4_layoutrecall *args = rpc_args->args_op;
	struct nfs4_callback *cb =
		(struct nfs4_callback *)req->rq_task->tk_client->cl_private;
	struct nfs4_cb_compound_hdr hdr = {
		.ident = 0,
		.minorversion = cb->cb_minorversion,
	};

	xdr_init_encode(&xdr, &req->rq_snd_buf, p);
	encode_cb_compound_hdr(&xdr, &hdr);
	encode_cb_sequence(&xdr, rpc_args->args_seq, &hdr);
	encode_cb_layout(&xdr, args, &hdr);
	encode_cb_nops(&hdr);
	return 0;
}

static int
nfs4_xdr_enc_cb_device(struct rpc_rqst *req, u32 *p,
		       struct nfs4_rpc_args *rpc_args)
{
	struct xdr_stream xdr;
	struct nfs4_notify_device *args = rpc_args->args_op;
	struct nfs4_callback *cb =
		(struct nfs4_callback *)req->rq_task->tk_client->cl_private;
	struct nfs4_cb_compound_hdr hdr = {
		.ident = 0,
		.minorversion = cb->cb_minorversion,
	};

	xdr_init_encode(&xdr, &req->rq_snd_buf, p);
	encode_cb_compound_hdr(&xdr, &hdr);
	encode_cb_sequence(&xdr, rpc_args->args_seq, &hdr);
	encode_cb_device(&xdr, args, &hdr);
	encode_cb_nops(&hdr);
	return 0;
}
#endif /* CONFIG_PNFSD */

static int
decode_cb_compound_hdr(struct xdr_stream *xdr, struct nfs4_cb_compound_hdr *hdr){
        __be32 *p;

        READ_BUF(8);
        READ32(hdr->status);
        READ32(hdr->taglen);
        READ_BUF(hdr->taglen + 4);
        hdr->tag = (char *)p;
        p += XDR_QUADLEN(hdr->taglen);
        READ32(hdr->nops);
        return 0;
}

static int
decode_cb_op_hdr(struct xdr_stream *xdr, enum nfs_opnum4 expected)
{
	__be32 *p;
	u32 op;
	int32_t nfserr;

	READ_BUF(8);
	READ32(op);
	if (op != expected) {
		dprintk("NFSD: decode_cb_op_hdr: Callback server returned "
		         " operation %d but we issued a request for %d\n",
		         op, expected);
		return -EIO;
	}
	READ32(nfserr);
	if (nfserr != NFS_OK)
		return -nfs_cb_stat_to_errno(nfserr);
	return 0;
}

/*
 * Our current back channel implmentation supports a single backchannel
 * with a single slot.
 */
static int
decode_cb_sequence(struct xdr_stream *xdr, struct nfsd4_cb_sequence *res,
		   struct rpc_rqst *rqstp)
{
	struct nfs4_callback *cb =
		(struct nfs4_callback *)rqstp->rq_task->tk_client->cl_private;
	struct nfs4_sessionid id;
	int status;
	u32 dummy;
	__be32 *p;

	if (cb->cb_minorversion == 0)
		return 0;

	status = decode_cb_op_hdr(xdr, OP_CB_SEQUENCE);
	if (status)
		return status;

	/*
	 * If the server returns different values for sessionID, slotID or
	 * sequence number, the server is looney tunes.
	 */
	status = -ESERVERFAULT;

	READ_BUF(NFS4_MAX_SESSIONID_LEN + 16);
	COPYMEM(id.data, NFS4_MAX_SESSIONID_LEN);
#ifdef CONFIG_NFSD_V4_1
	if (memcmp(id.data, res->cbs_clp->cl_sessionid.data,
		   NFS4_MAX_SESSIONID_LEN)) {
		dprintk("%s Invalid session id\n", __func__);
		goto out;
	}
	READ32(dummy);
	if (dummy != res->cbs_clp->cl_cb_seq_nr) {
		dprintk("%s Invalid sequence number\n", __func__);
		goto out;
	}
#endif /* CONFIG_NFSD_V4_1 */
	READ32(dummy); 	/* slotid must be 0 */
	if (dummy != 0) {
		dprintk("%s Invalid slotid\n", __func__);
		goto out;
	}
	READ32(dummy); 	/* highest slotid */
	READ32(dummy); 	/* target highest slotid */
	status = 0;
out:
	return status;
}


static int
nfs4_xdr_dec_cb_null(struct rpc_rqst *req, __be32 *p)
{
	return 0;
}

static int
nfs4_xdr_dec_cb_recall(struct rpc_rqst *rqstp, __be32 *p,
		       struct nfs4_rpc_res *rpc_res)
{
	struct xdr_stream xdr;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	xdr_init_decode(&xdr, &rqstp->rq_rcv_buf, p);
	status = decode_cb_compound_hdr(&xdr, &hdr);
	if (status)
		goto out;
	status = decode_cb_sequence(&xdr, rpc_res->res_seq, rqstp);
	if (status)
		goto out;
	status = decode_cb_op_hdr(&xdr, OP_CB_RECALL);
out:
	return status;
}

#if defined(CONFIG_PNFSD)
static int
nfs4_xdr_dec_cb_layout(struct rpc_rqst *rqstp, u32 *p,
		       struct nfs4_rpc_res *rpc_res)
{
	struct xdr_stream xdr;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	xdr_init_decode(&xdr, &rqstp->rq_rcv_buf, p);
	status = decode_cb_compound_hdr(&xdr, &hdr);
	if (status)
		goto out;
	status = decode_cb_sequence(&xdr, rpc_res->res_seq, rqstp);
	if (status)
		goto out;
	status = decode_cb_op_hdr(&xdr, OP_CB_LAYOUT);
out:
	return status;
}

static int
nfs4_xdr_dec_cb_device(struct rpc_rqst *rqstp, u32 *p,
		       struct nfs4_rpc_res *rpc_res)
{
	struct xdr_stream xdr;
	struct nfs4_cb_compound_hdr hdr;
	int status;

	xdr_init_decode(&xdr, &rqstp->rq_rcv_buf, p);
	status = decode_cb_compound_hdr(&xdr, &hdr);
	if (status)
		goto out;
	status = decode_cb_sequence(&xdr, rpc_res->res_seq, rqstp);
	if (status)
		goto out;
	status = decode_cb_op_hdr(&xdr, OP_CB_DEVICE);
out:
	return status;
}
#endif /* CONFIG_PNFSD */

/*
 * RPC procedure tables
 */
#define PROC(proc, call, argtype, restype)                              \
[NFSPROC4_CLNT_##proc] = {                                      	\
        .p_proc   = NFSPROC4_CB_##call,					\
        .p_encode = (kxdrproc_t) nfs4_xdr_##argtype,                    \
        .p_decode = (kxdrproc_t) nfs4_xdr_##restype,                    \
        .p_arglen = NFS4_##argtype##_sz,                                \
        .p_replen = NFS4_##restype##_sz,                                \
        .p_statidx = NFSPROC4_CB_##call,				\
	.p_name   = #proc,                                              \
}

static struct rpc_procinfo     nfs4_cb_procedures[] = {
    PROC(CB_NULL,      NULL,     enc_cb_null,     dec_cb_null),
    PROC(CB_RECALL,    COMPOUND,   enc_cb_recall,      dec_cb_recall),
#if defined(CONFIG_PNFSD)
    PROC(CB_LAYOUT,    COMPOUND,   enc_cb_layout,      dec_cb_layout),
    PROC(CB_DEVICE,    COMPOUND,   enc_cb_device,      dec_cb_device),
#endif
};

static struct rpc_version       nfs_cb_version4 = {
        .number                 = 1,
        .nrprocs                = ARRAY_SIZE(nfs4_cb_procedures),
        .procs                  = nfs4_cb_procedures
};

static struct rpc_version *	nfs_cb_version[] = {
	NULL,
	&nfs_cb_version4,
};

static struct rpc_program cb_program;

static struct rpc_stat cb_stats = {
		.program	= &cb_program
};

#define NFS4_CALLBACK 0x40000000
static struct rpc_program cb_program = {
		.name 		= "nfs4_cb",
		.number		= NFS4_CALLBACK,
		.nrvers		= ARRAY_SIZE(nfs_cb_version),
		.version	= nfs_cb_version,
		.stats		= &cb_stats,
		.pipe_dir_name  = "/nfsd4_cb",
};

/* Reference counting, callback cleanup, etc., all look racy as heck.
 * And why is cb_set an atomic? */

static int do_probe_callback(void *data)
{
	struct nfs4_client *clp = data;
	struct sockaddr_in	addr;
	struct nfs4_callback    *cb = &clp->cl_callback;
	struct rpc_timeout	timeparms = {
		.to_initval	= (NFSD_LEASE_TIME/4) * HZ,
		.to_retries	= 5,
		.to_maxval	= (NFSD_LEASE_TIME/2) * HZ,
		.to_exponential	= 1,
	};
	struct rpc_create_args args = {
		.protocol	= IPPROTO_TCP,
		.address	= (struct sockaddr *)&addr,
		.addrsize	= sizeof(addr),
		.timeout	= &timeparms,
		.program	= &cb_program,
		.prognumber	= cb->cb_prog,
		.version	= nfs_cb_version[1]->number,
		.authflavor	= clp->cl_flavor,
		.flags		= (RPC_CLNT_CREATE_NOPING | RPC_CLNT_CREATE_QUIET),
		.client_name    = clp->cl_principal,
	};
	struct rpc_message msg = {
		.rpc_proc       = &nfs4_cb_procedures[NFSPROC4_CLNT_CB_NULL],
		.rpc_argp       = clp,
	};
	struct rpc_clnt *client;
	int status;

	if (!clp->cl_principal && (clp->cl_flavor >= RPC_AUTH_GSS_KRB5)) {
		status = nfserr_cb_path_down;
		goto out_err;
	}

	/* Initialize address */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(cb->cb_port);
	addr.sin_addr.s_addr = htonl(cb->cb_addr);
#if defined(CONFIG_NFSD_V4_1)
	if (cb->cb_minorversion) {
		BUG_ON(cb->cb_minorversion != 1);
		args.bc_sock = container_of(clp->cl_cb_xprt, struct svc_sock,
					    sk_xprt);
	}
#endif /* CONFIG_NFSD_V4_1 */

	dprintk("%s: program %s 0x%x nrvers %u version %u minorversion %u\n",
		__func__, args.program->name, args.prognumber,
		args.program->nrvers, args.version, cb->cb_minorversion);

	/* Create RPC client */
	client = rpc_create(&args);
	if (IS_ERR(client)) {
		dprintk("NFSD: couldn't create callback client\n");
		status = PTR_ERR(client);
		goto out_err;
	}

	status = rpc_call_sync(client, &msg, RPC_TASK_SOFT);

	if (status)
		goto out_release_client;

	cb->cb_client = client;
	client->cl_private = cb;
	atomic_set(&cb->cb_set, 1);
	put_nfs4_client(clp);
	return 0;
out_release_client:
	dprintk("NFSD: synchronous CB_NULL failed. status=%d\n", status);
	rpc_shutdown_client(client);
out_err:
	dprintk("NFSD: warning: no callback path to client %.*s\n",
		(int)clp->cl_name.len, clp->cl_name.data);
	put_nfs4_client(clp);
	return status;
}

/*
 * Set up the callback client and put a NFSPROC4_CB_NULL on the wire...
 */
void
nfsd4_probe_callback(struct nfs4_client *clp)
{
	struct task_struct *t;

	BUG_ON(atomic_read(&clp->cl_callback.cb_set));

	/* the task holds a reference to the nfs4_client struct */
	atomic_inc(&clp->cl_count);

	t = kthread_run(do_probe_callback, clp, "nfs4_cb_probe");

	if (IS_ERR(t))
		atomic_dec(&clp->cl_count);

	return;
}

static int _nfsd4_cb_sync(struct nfs4_client *clp,
			  const struct rpc_message *msg, int flags)
{
	return rpc_call_sync(clp->cl_callback.cb_client, msg, RPC_TASK_SOFT);
}

#if defined(CONFIG_NFSD_V4_1)
/*
 * FIXME: cb_sequence should support referring call lists, cachethis, and
 * multiple slots
 */
static int
nfs41_cb_sequence_setup(struct nfs4_client *clp, struct nfsd4_cb_sequence *args)
{
	u32 *ptr = (u32 *)clp->cl_sessionid.data;

	dprintk("%s: %u:%u:%u:%u\n", __func__,
		ptr[0], ptr[1], ptr[2], ptr[3]);

	mutex_lock(&clp->cl_cb_mutex);
	args->cbs_clp = clp;
	clp->cl_cb_seq_nr++;
	return 0;
}

static void
nfs41_cb_sequence_done(struct nfs4_client *clp, struct nfsd4_cb_sequence *res)
{
	u32 *ptr = (u32 *)clp->cl_sessionid.data;

	dprintk("%s: %u:%u:%u:%u\n", __func__,
		ptr[0], ptr[1], ptr[2], ptr[3]);

	/* FIXME: support multiple callback slots */
	mutex_unlock(&clp->cl_cb_mutex);
}

static int _nfsd41_cb_sync(struct nfs4_client *clp,
			   struct rpc_message *msg, int flags)
{
	struct nfsd4_cb_sequence seq;
	struct nfs4_rpc_args *args;
	struct nfs4_rpc_res res;
	int status;

	args = msg->rpc_argp;
	args->args_seq = &seq;

	res.res_seq = &seq;
	msg->rpc_resp = &res;

	nfs41_cb_sequence_setup(clp, &seq);
	status = _nfsd4_cb_sync(clp, msg, flags);
	nfs41_cb_sequence_done(clp, &seq);

	return status;
}

static int nfsd4_cb_sync(struct nfs4_client *clp,
			 struct rpc_message *msg, int flags)
{
	return clp->cl_callback.cb_minorversion ?
		_nfsd41_cb_sync(clp, msg, flags) :
		_nfsd4_cb_sync(clp, msg, flags);
}
#else  /* CONFIG_NFSD_V4_1 */
static int nfsd4_cb_sync(struct nfs4_client *clp,
			 struct rpc_message *msg, int flags)
{
	return _nfsd4_cb_sync(clp, msg, flags);
}
#endif /* CONFIG_NFSD_V4_1 */

/*
 * called with dp->dl_count inc'ed.
 */
void
nfsd4_cb_recall(struct nfs4_delegation *dp)
{
	struct nfs4_client *clp = dp->dl_client;
	struct nfs4_cb_recall *cbr = &dp->dl_recall;
	struct nfs4_rpc_args args = {
		.args_op = cbr,
	};
	struct rpc_message msg = {
		.rpc_proc = &nfs4_cb_procedures[NFSPROC4_CLNT_CB_RECALL],
		.rpc_argp = &args,
	};
	int retries = 1;
	int status = 0;

	dprintk("%s: dp %p\n", __func__, dp);

	cbr->cbr_trunc = 0; /* XXX need to implement truncate optimization */
	cbr->cbr_dp = dp;

	status = nfsd4_cb_sync(clp, &msg, RPC_TASK_SOFT);
	while (retries--) {
		switch (status) {
			case -EIO:
				/* Network partition? */
				atomic_set(&clp->cl_callback.cb_set, 0);
			case -EBADHANDLE:
			case -NFS4ERR_BAD_STATEID:
				/* Race: client probably got cb_recall
				 * before open reply granting delegation */
				break;
			default:
				goto out_put_cred;
		}
		ssleep(2);
		status = nfsd4_cb_sync(clp, &msg, RPC_TASK_SOFT);
	}
out_put_cred:
	/*
	 * Success or failure, now we're either waiting for lease expiration
	 * or deleg_return.
	 */
	dprintk("%s: dp %p dl_flock %p dl_count %d\n",
		__func__, dp, dp->dl_flock, atomic_read(&dp->dl_count));
	nfs4_lock_state();
	put_nfs4_client(clp);
	nfs4_put_delegation(dp);
	nfs4_unlock_state();
	return;
}

#if defined(CONFIG_PNFSD)
/*
 * called with dp->dl_count inc'ed.
 * nfs4_lock_state() may or may not have been called.
 */
int
nfsd4_cb_layout(struct nfs4_layoutrecall *clr)
{
	struct nfs4_client *clp = clr->clr_client;
	struct rpc_clnt *clnt = NULL;
	struct nfsd4_cb_sequence seq;
	struct nfs4_rpc_args args = {
		.args_op = clr,
		.args_seq = &seq
	};
	struct nfs4_rpc_res res = {
		.res_seq = &seq
	};
	struct rpc_message msg = {
		.rpc_proc = &nfs4_cb_procedures[NFSPROC4_CLNT_CB_LAYOUT],
		.rpc_argp = &args,
		.rpc_resp = &res,
	};
	int status = -EIO;

	if (clp)
		clnt = clp->cl_callback.cb_client;

	if ((!atomic_read(&clp->cl_callback.cb_set)) || !clnt)
		goto out;

	nfs41_cb_sequence_setup(clp, &seq);
	status = rpc_call_sync(clnt, &msg, RPC_TASK_SOFT);
	nfs41_cb_sequence_done(clp, &seq);

	if (status == -EIO)
		atomic_set(&clp->cl_callback.cb_set, 0);
out:
	/* Success or failure, now we're either waiting for lease expiration
	   or layout_return. */
	dprintk("NFSD: nfsd4_cb_layout: status %d\n", status);
	return status;
}

/*
 * called with dp->dl_count inc'ed.
 * nfs4_lock_state() may or may not have been called.
 */
int
nfsd4_cb_notify_device(struct nfs4_notify_device *cbnd)
{
	struct nfs4_client *clp = cbnd->nd_client;
	struct rpc_clnt *clnt = NULL;
	struct nfsd4_cb_sequence seq;
	struct nfs4_rpc_args args = {
		.args_op = cbnd,
		.args_seq = &seq
	};
	struct nfs4_rpc_res res = {
		.res_seq = &seq
	};
	struct rpc_message msg = {
		.rpc_proc = &nfs4_cb_procedures[NFSPROC4_CLNT_CB_DEVICE],
		.rpc_argp = &args,
		.rpc_resp = &res,
	};
	int status = -EIO;

	dprintk("NFSD:%s --> clp %p\n", __func__, clp);

	dprintk("NFSD %s: -->\n", __func__);

	if (clp)
		clnt = clp->cl_callback.cb_client;

	if ((!atomic_read(&clp->cl_callback.cb_set)) || !clnt)
		goto out;

	dprintk("NFSD %s: Calling rpc device notify (clnt %p)\n",
		__func__, clp);
	nfs41_cb_sequence_setup(clp, &seq);
	status = rpc_call_sync(clnt, &msg, RPC_TASK_SOFT);
	nfs41_cb_sequence_done(clp, &seq);

	if (status == -EIO)
		atomic_set(&clp->cl_callback.cb_set, 0);
out:
	dprintk("NFSD %s: status %d\n", __func__, status);
	return status;
}
#endif /* CONFIG_PNFSD */
