/*
 *  Copyright (c) 2005 The Regents of the University of Michigan.
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

#ifndef LINUX_NFSD_PNFSD_H
#define LINUX_NFSD_PNFSD_H

#include <linux/list.h>
#include <linux/nfsd/nfsd4_pnfs.h>

#include "state.h"
#include "xdr4.h"

/* outstanding layout stateid */
struct nfs4_layout_state {
	struct nfs4_stid	ls_stid; /* must be first field */
	struct kref		ls_ref;
	struct list_head	ls_perfile;
	bool			ls_roc;
};

/* outstanding layout */
struct nfs4_layout {
	struct list_head		lo_perfile;	/* hash by f_id */
	struct list_head		lo_perclnt;	/* hash by clientid */
	struct nfs4_file		*lo_file;	/* backpointer */
	struct nfs4_client		*lo_client;
	struct nfs4_layout_state	*lo_state;
	struct nfsd4_layout_seg		lo_seg;
};

u64 find_create_sbid(struct super_block *);
struct super_block *find_sbid_id(u64);
__be32 nfs4_pnfs_get_layout(struct svc_rqst *, struct nfsd4_pnfs_layoutget *, struct exp_xdr_stream *);
int nfs4_pnfs_return_layout(struct super_block *, struct svc_fh *,
					struct nfsd4_pnfs_layoutreturn *);
int put_layoutrecall(struct nfs4_layoutrecall *);
void nomatching_layout(struct nfs4_layoutrecall *);
void *layoutrecall_done(struct nfs4_layoutrecall *);
void nfsd4_cb_layout(struct nfs4_layoutrecall *);
int _nfsd_layout_recall_cb(struct super_block *, struct inode *,
			  struct nfsd4_pnfs_cb_layout *,
			  bool with_nfs4_state_lock);
int nfsd_layout_recall_cb(struct super_block *, struct inode *,
			  struct nfsd4_pnfs_cb_layout *);

static inline struct nfs4_layout_state *layoutstateid(struct nfs4_stid *s)
{
	return container_of(s, struct nfs4_layout_state, ls_stid);
}

#if defined(CONFIG_PNFSD_LOCAL_EXPORT)
extern struct sockaddr_storage pnfsd_lexp_addr;
extern size_t pnfs_lexp_addr_len;

extern void pnfsd_lexp_init(struct inode *);
extern bool is_inode_pnfsd_lexp(struct inode *);
extern int pnfsd_lexp_recall_layout(struct inode *, bool with_nfs4_state_lock);
#endif /* CONFIG_PNFSD_LOCAL_EXPORT */

#endif /* LINUX_NFSD_PNFSD_H */
