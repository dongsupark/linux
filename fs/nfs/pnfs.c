/*
 *  linux/fs/nfs/pnfs.c
 *
 *  pNFS functions to call and manage layout drivers.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dean Hildebrand <dhildebz@eecs.umich.edu>
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

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/pnfs_xdr.h>
#include <linux/nfs4_pnfs.h>

#include "internal.h"
#include "nfs4_fs.h"
#include "pnfs.h"

#ifdef CONFIG_PNFS
#define NFSDBG_FACILITY		NFSDBG_PNFS

static int pnfs_initialized;

/* Locking:
 *
 * pnfs_spinlock:
 * 	protects pnfs_modules_tbl.
 */
static spinlock_t pnfs_spinlock = __SPIN_LOCK_UNLOCKED(pnfs_spinlock);

/*
 * pnfs_modules_tbl holds all pnfs modules
 */
static struct list_head	pnfs_modules_tbl;

/*
 * struct pnfs_module - One per pNFS device module.
 */
struct pnfs_module {
	struct pnfs_layoutdriver_type *pnfs_ld_type;
	struct list_head        pnfs_tblid;
};

int
pnfs_initialize(void)
{
	INIT_LIST_HEAD(&pnfs_modules_tbl);

	pnfs_v4_clientops_init();

	pnfs_initialized = 1;
	return 0;
}

void pnfs_uninitialize(void)
{
}

/* search pnfs_modules_tbl for right pnfs module */
static int
find_pnfs(u32 id, struct pnfs_module **module) {
	struct  pnfs_module *local = NULL;

	dprintk("PNFS: %s: Searching for %u\n", __func__, id);
	list_for_each_entry(local, &pnfs_modules_tbl, pnfs_tblid) {
		if (local->pnfs_ld_type->id == id) {
			*module = local;
			return(1);
		}
	}
	return 0;
}

/* Unitialize a mountpoint in a layout driver */
void
unmount_pnfs_layoutdriver(struct super_block *sb)
{
	struct nfs_server *server = NFS_SB(sb);
	if (server->pnfs_curr_ld &&
	    server->pnfs_curr_ld->ld_io_ops &&
	    server->pnfs_curr_ld->ld_io_ops->uninitialize_mountpoint) {
		server->pnfs_curr_ld->ld_io_ops->uninitialize_mountpoint(
			server->pnfs_mountid);
		server->pnfs_mountid = NULL;
	    }
}

/*
 * Set the server pnfs module to the first registered pnfs_type.
 * Only one pNFS layout driver is supported.
 */
void
set_pnfs_layoutdriver(struct super_block *sb, struct nfs_fh *fh, u32 id)
{
	struct pnfs_module *mod;
	struct pnfs_mount_type *mt;
	struct nfs_server *server = NFS_SB(sb);

	if (id > 0 && find_pnfs(id, &mod)) {
		dprintk("%s: Setting pNFS module\n", __func__);
		server->pnfs_curr_ld = mod->pnfs_ld_type;
		mt = server->pnfs_curr_ld->ld_io_ops->initialize_mountpoint(
			sb, fh);
		if (!mt) {
			printk(KERN_ERR "%s: Error initializing mount point "
			       "for layout driver %u. ", __func__, id);
			goto out_err;
		}
		/* Layout driver succeeded in initializing mountpoint */
		server->pnfs_mountid = mt;
		/* Set the rpc_ops */
		server->nfs_client->rpc_ops = &pnfs_v4_clientops;
		return;
	}

	dprintk("%s: No pNFS module found for %u. ", __func__, id);
out_err:
	dprintk("Using NFSv4 I/O\n");
	server->pnfs_curr_ld = NULL;
	server->pnfs_mountid = NULL;
	return;
}

/* Allow I/O module to set its functions structure */
struct pnfs_client_operations*
pnfs_register_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	struct pnfs_module *pnfs_mod;

	if (!pnfs_initialized) {
		printk(KERN_ERR "%s Registration failure. "
		       "pNFS not initialized.\n", __func__);
		return NULL;
	}

	pnfs_mod = kmalloc(sizeof(struct pnfs_module), GFP_KERNEL);
	if (pnfs_mod != NULL) {
		dprintk("%s Registering id:%u name:%s\n",
			__func__,
			ld_type->id,
			ld_type->name);
		pnfs_mod->pnfs_ld_type = ld_type;
		INIT_LIST_HEAD(&pnfs_mod->pnfs_tblid);

		spin_lock(&pnfs_spinlock);
		list_add(&pnfs_mod->pnfs_tblid, &pnfs_modules_tbl);
		spin_unlock(&pnfs_spinlock);
	}

	return &pnfs_ops;
}

/*  Allow I/O module to set its functions structure */
void
pnfs_unregister_layoutdriver(struct pnfs_layoutdriver_type *ld_type)
{
	struct pnfs_module *pnfs_mod;

	if (find_pnfs(ld_type->id, &pnfs_mod)) {
		dprintk("%s Deregistering id:%u\n", __func__, ld_type->id);
		spin_lock(&pnfs_spinlock);
		list_del(&pnfs_mod->pnfs_tblid);
		spin_unlock(&pnfs_spinlock);
		kfree(pnfs_mod);
	}
}

static void
pnfs_get_layout_stateid(nfs4_stateid *dst, struct pnfs_layout_type *lo)
{
	int seq;

	dprintk("--> %s\n", __func__);

	do {
		seq = read_seqbegin(&lo->seqlock);
		memcpy(dst->data, lo->stateid.data, sizeof(lo->stateid.data));
	} while (read_seqretry(&lo->seqlock, seq));

	dprintk("<-- %s\n", __func__);
}

static void
pnfs_layout_from_open_stateid(nfs4_stateid *dst, struct nfs4_state *state)
{
	int seq;

	dprintk("--> %s\n", __func__);

	do {
		seq = read_seqbegin(&state->seqlock);
		memcpy(dst->data, state->stateid.data,
				sizeof(state->stateid.data));
	} while (read_seqretry(&state->seqlock, seq));

	dprintk("<-- %s\n", __func__);
}

/*
* Get layout from server.
*    for now, assume that whole file layouts are requested.
*    arg->offset: 0
*    arg->length: all ones
*
*    for now, assume the LAYOUTGET operation is triggered by an I/O request.
*    the count field is the count in the I/O request, and will be used
*    as the minlength. for the file operation that piggy-backs
*    the LAYOUTGET operation with an OPEN, s
*    arg->minlength = count.
*/
static int
get_layout(struct inode *ino,
	   struct nfs_open_context *ctx,
	   struct nfs4_pnfs_layout_segment *range,
	   struct pnfs_layout_segment **lsegpp,
	   struct pnfs_layout_type *lo)
{
	int status;
	struct nfs_server *server = NFS_SERVER(ino);
	struct nfs4_pnfs_layoutget *lgp;

	dprintk("--> %s\n", __func__);

	lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
	if (lgp == NULL)
		return -ENOMEM;
	lgp->lo = lo;
	lgp->args.lseg.iomode = range->iomode;
	lgp->args.lseg.offset = range->offset;
	lgp->args.lseg.length = range->length;
	lgp->args.type = server->pnfs_curr_ld->id;
	lgp->args.minlength = PAGE_CACHE_SIZE;
	lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;
	lgp->args.inode = ino;
	lgp->args.ctx = ctx;
	lgp->lsegpp = lsegpp;

	if (!memcmp(lo->stateid.data, &zero_stateid, NFS4_STATEID_SIZE))
		pnfs_layout_from_open_stateid(&lgp->args.stateid, ctx->state);
	else
		pnfs_get_layout_stateid(&lgp->args.stateid, lo);

	/* Retrieve layout information from server */
	status = NFS_PROTO(ino)->pnfs_layoutget(lgp);

	dprintk("<-- %s status %d\n", __func__, status);
	return status;
}

/* Callback operations for layout drivers.
 */
struct pnfs_client_operations pnfs_ops = {
};

EXPORT_SYMBOL(pnfs_unregister_layoutdriver);
EXPORT_SYMBOL(pnfs_register_layoutdriver);

#endif /* CONFIG_PNFS */
