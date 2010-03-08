/*
 * linux/fs/nfs/callback_proc.c
 *
 * Copyright (C) 2004 Trond Myklebust
 *
 * NFSv4 callback procedures
 */
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include "nfs4_fs.h"
#include "callback.h"
#include "delegation.h"
#include "internal.h"

#if defined(CONFIG_PNFS)
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/nfs4_pnfs.h>
#endif

#include "pnfs.h"

#ifdef NFS_DEBUG
#define NFSDBG_FACILITY NFSDBG_CALLBACK
#endif
 
__be32 nfs4_callback_getattr(struct cb_getattrargs *args, struct cb_getattrres *res)
{
	struct nfs_client *clp;
	struct nfs_delegation *delegation;
	struct nfs_inode *nfsi;
	struct inode *inode;

	res->bitmap[0] = res->bitmap[1] = 0;
	res->status = htonl(NFS4ERR_BADHANDLE);
	clp = nfs_find_client(args->addr, 4);
	if (clp == NULL)
		goto out;

	dprintk("NFS: GETATTR callback request from %s\n",
		rpc_peeraddr2str(clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	inode = nfs_delegation_find_inode(clp, &args->fh);
	if (inode == NULL)
		goto out_putclient;
	nfsi = NFS_I(inode);
	down_read(&nfsi->rwsem);
	delegation = nfsi->delegation;
	if (delegation == NULL || (delegation->type & FMODE_WRITE) == 0)
		goto out_iput;
	res->size = i_size_read(inode);
	res->change_attr = delegation->change_attr;
	if (nfsi->npages != 0)
		res->change_attr++;
	res->ctime = inode->i_ctime;
	res->mtime = inode->i_mtime;
	res->bitmap[0] = (FATTR4_WORD0_CHANGE|FATTR4_WORD0_SIZE) &
		args->bitmap[0];
	res->bitmap[1] = (FATTR4_WORD1_TIME_METADATA|FATTR4_WORD1_TIME_MODIFY) &
		args->bitmap[1];
	res->status = 0;
out_iput:
	up_read(&nfsi->rwsem);
	iput(inode);
out_putclient:
	nfs_put_client(clp);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(res->status));
	return res->status;
}

static int (*nfs_validate_delegation_stateid(struct nfs_client *clp))(struct nfs_delegation *, const nfs4_stateid *)
{
#if defined(CONFIG_NFS_V4_1)
	if (clp->cl_minorversion > 0)
		return nfs41_validate_delegation_stateid;
#endif
	return nfs4_validate_delegation_stateid;
}


__be32 nfs4_callback_recall(struct cb_recallargs *args, void *dummy)
{
	struct nfs_client *clp;
	struct inode *inode;
	__be32 res;
	
	res = htonl(NFS4ERR_BADHANDLE);
	clp = nfs_find_client(args->addr, 4);
	if (clp == NULL)
		goto out;

	dprintk("NFS: RECALL callback request from %s\n",
		rpc_peeraddr2str(clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	do {
		struct nfs_client *prev = clp;

		inode = nfs_delegation_find_inode(clp, &args->fh);
		if (inode != NULL) {
			/* Set up a helper thread to actually return the delegation */
			switch (nfs_async_inode_return_delegation(inode, &args->stateid,
								  nfs_validate_delegation_stateid(clp))) {
				case 0:
					res = 0;
					break;
				case -ENOENT:
					if (res != 0)
						res = htonl(NFS4ERR_BAD_STATEID);
					break;
				default:
					res = htonl(NFS4ERR_RESOURCE);
			}
			iput(inode);
		}
		clp = nfs_find_client_next(prev);
		nfs_put_client(prev);
	} while (clp != NULL);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(res));
	return res;
}

#if defined(CONFIG_PNFS)

/*
 * Retrieve an inode based on layout recall parameters
 *
 * Note: caller must iput(inode) to dereference the inode.
 */
static struct inode *
nfs_layoutrecall_find_inode(struct nfs_client *clp,
			    const struct cb_pnfs_layoutrecallargs *args)
{
	struct nfs_inode *nfsi;
	struct nfs_server *server;
	struct inode *ino = NULL;

	dprintk("%s: Begin recall_type=%d clp %p\n",
		__func__, args->cbl_recall_type, clp);

	spin_lock(&clp->cl_lock);
	list_for_each_entry(nfsi, &clp->cl_lo_inodes, lo_inodes) {
		dprintk("%s: Searching inode=%lu\n",
			__func__, nfsi->vfs_inode.i_ino);
		if (args->cbl_recall_type == RETURN_FILE) {
		    if (nfs_compare_fh(&args->cbl_fh, &nfsi->fh))
			continue;
		} else if (args->cbl_recall_type == RETURN_FSID) {
			server = NFS_SERVER(&nfsi->vfs_inode);
			if (server->fsid.major != args->cbl_fsid.major ||
			    server->fsid.minor != args->cbl_fsid.minor)
				continue;
		}

		/* Make sure client didn't clean up layout without
		 * telling the server */
		if (!has_layout(nfsi))
			continue;

		ino = igrab(&nfsi->vfs_inode);
		dprintk("%s: Found inode=%p\n", __func__, ino);
		break;
	}
	spin_unlock(&clp->cl_lock);
	return ino;
}

struct recall_layout_threadargs {
	struct inode *inode;
	struct nfs_client *clp;
	struct completion started;
	struct cb_pnfs_layoutrecallargs *rl;
	int result;
};

static int pnfs_recall_layout(void *data)
{
	struct inode *inode, *ino;
	struct nfs_client *clp;
	struct cb_pnfs_layoutrecallargs rl;
	struct recall_layout_threadargs *args =
		(struct recall_layout_threadargs *)data;
	int status;

	daemonize("nfsv4-layoutreturn");

	dprintk("%s: recall_type=%d fsid 0x%llx-0x%llx start\n",
		__func__, args->rl->cbl_recall_type,
		args->rl->cbl_fsid.major, args->rl->cbl_fsid.minor);

	clp = args->clp;
	inode = args->inode;
	rl = *args->rl;
	args->result = 0;
	complete(&args->started);
	args = NULL;
	/* Note: args must not be used after this point!!! */

/* FIXME: need barrier here:
   pause I/O to data servers
   pause layoutgets
   drain all outstanding writes to storage devices
   wait for any outstanding layoutreturns and layoutgets mentioned in
   cb_sequence.
   then return layouts, resume after layoutreturns complete
 */

	if (rl.cbl_recall_type == RETURN_FILE) {
		status = pnfs_return_layout(inode, &rl.cbl_seg, &rl.cbl_stateid,
					    RETURN_FILE);
		if (status)
			dprintk("%s RETURN_FILE error: %d\n", __func__, status);
		goto out;
	}

	rl.cbl_seg.offset = 0;
	rl.cbl_seg.length = NFS4_MAX_UINT64;

	/* FIXME: This loop is inefficient, running in O(|s_inodes|^2) */
	while ((ino = nfs_layoutrecall_find_inode(clp, &rl)) != NULL) {
		/* XXX need to check status on pnfs_return_layout */
		pnfs_return_layout(ino, &rl.cbl_seg, NULL, RETURN_FILE);
		iput(ino);
	}

	/* send final layoutreturn */
	status = pnfs_return_layout(inode, &rl.cbl_seg, NULL, rl.cbl_recall_type);
	if (status)
		printk(KERN_INFO "%s: ignoring pnfs_return_layout status=%d\n",
				__func__, status);
out:
	iput(inode);
	module_put_and_exit(0);
	dprintk("%s: exit status %d\n", __func__, 0);
	return 0;
}

/*
 * Asynchronous layout recall!
 */
static int pnfs_async_return_layout(struct nfs_client *clp, struct inode *inode,
				    struct cb_pnfs_layoutrecallargs *rl)
{
	struct recall_layout_threadargs data = {
		.clp = clp,
		.inode = inode,
		.rl = rl,
	};
	struct task_struct *t;
	int status;

	/* should have returned NFS4ERR_NOMATCHING_LAYOUT... */
	BUG_ON(inode == NULL);

	dprintk("%s: -->\n", __func__);

	init_completion(&data.started);
	__module_get(THIS_MODULE);

	t = kthread_run(pnfs_recall_layout, &data, "%s", "pnfs_recall_layout");
	if (IS_ERR(t)) {
		printk(KERN_INFO "NFS: Layout recall callback thread failed "
			"for client (clientid %08x/%08x)\n",
			(unsigned)(clp->cl_clientid >> 32),
			(unsigned)(clp->cl_clientid));
		status = PTR_ERR(t);
		goto out_module_put;
	}
	wait_for_completion(&data.started);
	return data.result;
out_module_put:
	module_put(THIS_MODULE);
	return status;
}

__be32 pnfs_cb_layoutrecall(struct cb_pnfs_layoutrecallargs *args,
			    void *dummy)
{
	struct nfs_client *clp;
	struct inode *inode = NULL;
	__be32 res;
	unsigned int num_client = 0;

	dprintk("%s: -->\n", __func__);

	res = htonl(NFS4ERR_INVAL);
	clp = nfs_find_client(args->cbl_addr, 4);
	if (clp == NULL) {
		dprintk("%s: no client for addr %u.%u.%u.%u\n",
			__func__, NIPQUAD(args->cbl_addr));
		goto out;
	}

	res = htonl(NFS4ERR_NOMATCHING_LAYOUT);
	do {
		struct nfs_client *prev = clp;
		num_client++;
		inode = nfs_layoutrecall_find_inode(clp, args);
		if (inode != NULL) {
			if (PNFS_LD(&NFS_I(inode)->layout)->id ==
			    args->cbl_layout_type) {
				/* Set up a helper thread to actually
				 * return the delegation */
				res = pnfs_async_return_layout(clp, inode, args);
				if (res != 0)
					res = htonl(NFS4ERR_RESOURCE);
				break;
			}
			iput(inode);
		}
		clp = nfs_find_client_next(prev);
		nfs_put_client(prev);
	} while (clp != NULL);

out:
	dprintk("%s: exit with status = %d numclient %u\n",
		__func__, ntohl(res), num_client);
	return res;
}

/* Remove all devices for each superblock for nfs_client.  This may try
 * to remove the same device multple times if they are
 * shared across superblocks in the layout driver, but the
 * layout drive should be able to handle this */
static __be32 pnfs_devicenotify_client(struct nfs_client *clp,
				       struct cb_pnfs_devicenotifyargs *args)
{
	struct nfs_server *server;
	__be32 res = 0, res2 = 0;
	int i, num_sb = 0;
	struct layoutdriver_io_operations *ops;
	uint32_t type;

	dprintk("%s: --> clp %p\n", __func__, clp);

	list_for_each_entry(server, &clp->cl_superblocks, client_link) {
		ops = server->pnfs_curr_ld->ld_io_ops;
		num_sb++;
		for (i = 0; i < args->ndevs; i++) {
			struct cb_pnfs_devicenotifyitem *dev = &args->devs[i];
			type = dev->cbd_notify_type;
			if (type == NOTIFY_DEVICEID4_DELETE &&
			    PNFS_EXISTS_LDIO_OP(server, device_delete))
				res = ops->device_delete(server->pnfs_mountid,
							 &dev->cbd_dev_id);
			else if (type == NOTIFY_DEVICEID4_CHANGE)
				printk(KERN_ERR "%s: NOTIFY_DEVICEID4_CHANGE "
					"not supported\n", __func__);
			if (res)
				res2 = res;
		}
	}
	dprintk("%s: exit with status = %d numsb %u\n",
		__func__, ntohl(res2), num_sb);
	return res2;
}

__be32 pnfs_cb_devicenotify(struct cb_pnfs_devicenotifyargs *args,
			    void *dummy)
{
	struct nfs_client *clp;
	__be32 res = 0;
	unsigned int num_client = 0;

	dprintk("%s: -->\n", __func__);

	res = __constant_htonl(NFS4ERR_INVAL);
	clp = nfs_find_client(args->addr, 4);
	if (clp == NULL) {
		dprintk("%s: no client for addr %u.%u.%u.%u\n",
			__func__, NIPQUAD(args->addr));
		goto out;
	}

	do {
		struct nfs_client *prev = clp;
		num_client++;
		res = pnfs_devicenotify_client(clp, args);
		clp = nfs_find_client_next(prev);
		nfs_put_client(prev);
	} while (clp != NULL);

out:
	dprintk("%s: exit with status = %d numclient %u\n",
		__func__, ntohl(res), num_client);
	return res;
}

#endif /* defined(CONFIG_PNFS) */

int nfs4_validate_delegation_stateid(struct nfs_delegation *delegation, const nfs4_stateid *stateid)
{
	if (delegation == NULL || memcmp(delegation->stateid.data, stateid->data,
					 sizeof(delegation->stateid.data)) != 0)
		return 0;
	return 1;
}

#if defined(CONFIG_NFS_V4_1)

int nfs41_validate_delegation_stateid(struct nfs_delegation *delegation, const nfs4_stateid *stateid)
{
	if (delegation == NULL)
		return 0;

	/* seqid is 4-bytes long */
	if (((u32 *) &stateid->data)[0] != 0)
		return 0;
	if (memcmp(&delegation->stateid.data[4], &stateid->data[4],
		   sizeof(stateid->data)-4))
		return 0;

	return 1;
}

/*
 * Validate the sequenceID sent by the server.
 * Return success if the sequenceID is one more than what we last saw on
 * this slot, accounting for wraparound.  Increments the slot's sequence.
 *
 * We don't yet implement a duplicate request cache, so at this time
 * we will log replays, and process them as if we had not seen them before,
 * but we don't bump the sequence in the slot.  Not too worried about it,
 * since we only currently implement idempotent callbacks anyway.
 *
 * We have a single slot backchannel at this time, so we don't bother
 * checking the used_slots bit array on the table.  The lower layer guarantees
 * a single outstanding callback request at a time.
 */
static int
validate_seqid(struct nfs4_slot_table *tbl, u32 slotid, u32 seqid)
{
	struct nfs4_slot *slot;

	dprintk("%s enter. slotid %d seqid %d\n",
		__func__, slotid, seqid);

	if (slotid > NFS41_BC_MAX_CALLBACKS)
		return htonl(NFS4ERR_BADSLOT);

	slot = tbl->slots + slotid;
	dprintk("%s slot table seqid: %d\n", __func__, slot->seq_nr);

	/* Normal */
	if (likely(seqid == slot->seq_nr + 1)) {
		slot->seq_nr++;
		return htonl(NFS4_OK);
	}

	/* Replay */
	if (seqid == slot->seq_nr) {
		dprintk("%s seqid %d is a replay - no DRC available\n",
			__func__, seqid);
		return htonl(NFS4_OK);
	}

	/* Wraparound */
	if (seqid == 1 && (slot->seq_nr + 1) == 0) {
		slot->seq_nr = 1;
		return htonl(NFS4_OK);
	}

	/* Misordered request */
	return htonl(NFS4ERR_SEQ_MISORDERED);
}

/*
 * Returns a pointer to a held 'struct nfs_client' that matches the server's
 * address, major version number, and session ID.  It is the caller's
 * responsibility to release the returned reference.
 *
 * Returns NULL if there are no connections with sessions, or if no session
 * matches the one of interest.
 */
 static struct nfs_client *find_client_with_session(
	const struct sockaddr *addr, u32 nfsversion,
	struct nfs4_sessionid *sessionid)
{
	struct nfs_client *clp;

	clp = nfs_find_client(addr, 4);
	if (clp == NULL)
		return NULL;

	do {
		struct nfs_client *prev = clp;

		if (clp->cl_session != NULL) {
			if (memcmp(clp->cl_session->sess_id.data,
					sessionid->data,
					NFS4_MAX_SESSIONID_LEN) == 0) {
				/* Returns a held reference to clp */
				return clp;
			}
		}
		clp = nfs_find_client_next(prev);
		nfs_put_client(prev);
	} while (clp != NULL);

	return NULL;
}

/* FIXME: referring calls should be processed */
unsigned nfs4_callback_sequence(struct cb_sequenceargs *args,
				struct cb_sequenceres *res)
{
	struct nfs_client *clp;
	int i, status;

	for (i = 0; i < args->csa_nrclists; i++)
		kfree(args->csa_rclists[i].rcl_refcalls);
	kfree(args->csa_rclists);

	status = htonl(NFS4ERR_BADSESSION);
	clp = find_client_with_session(args->csa_addr, 4, &args->csa_sessionid);
	if (clp == NULL)
		goto out;

	status = validate_seqid(&clp->cl_session->bc_slot_table,
				args->csa_slotid, args->csa_sequenceid);
	if (status)
		goto out_putclient;

	memcpy(&res->csr_sessionid, &args->csa_sessionid,
	       sizeof(res->csr_sessionid));
	res->csr_sequenceid = args->csa_sequenceid;
	res->csr_slotid = args->csa_slotid;
	res->csr_highestslotid = NFS41_BC_MAX_CALLBACKS - 1;
	res->csr_target_highestslotid = NFS41_BC_MAX_CALLBACKS - 1;

out_putclient:
	nfs_put_client(clp);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(status));
	res->csr_status = status;
	return res->csr_status;
}

unsigned nfs4_callback_recallany(struct cb_recallanyargs *args, void *dummy)
{
	struct nfs_client *clp;
	int status;
	fmode_t flags = 0;

	status = htonl(NFS4ERR_OP_NOT_IN_SESSION);
	clp = nfs_find_client(args->craa_addr, 4);
	if (clp == NULL)
		goto out;

	dprintk("NFS: RECALL_ANY callback request from %s\n",
		rpc_peeraddr2str(clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	if (test_bit(RCA4_TYPE_MASK_RDATA_DLG, (const unsigned long *)
		     &args->craa_type_mask))
		flags = FMODE_READ;
	if (test_bit(RCA4_TYPE_MASK_WDATA_DLG, (const unsigned long *)
		     &args->craa_type_mask))
		flags |= FMODE_WRITE;

	if (flags)
		nfs_expire_all_delegation_types(clp, flags);
	status = htonl(NFS4_OK);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(status));
	return status;
}
#endif /* CONFIG_NFS_V4_1 */
