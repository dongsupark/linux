/*
 * linux/fs/nfs/callback_proc.c
 *
 * Copyright (C) 2004 Trond Myklebust
 *
 * NFSv4 callback procedures
 */
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/slab.h>
#include "nfs4_fs.h"
#include "callback.h"
#include "delegation.h"
#include "internal.h"
#include "pnfs.h"

#ifdef NFS_DEBUG
#define NFSDBG_FACILITY NFSDBG_CALLBACK
#endif

__be32 nfs4_callback_getattr(struct cb_getattrargs *args,
			     struct cb_getattrres *res,
			     struct cb_process_state *cps)
{
	struct nfs_delegation *delegation;
	struct nfs_inode *nfsi;
	struct inode *inode;

	res->status = htonl(NFS4ERR_OP_NOT_IN_SESSION);
	if (!cps->clp) /* Always set for v4.0. Set in cb_sequence for v4.1 */
		goto out;

	res->bitmap[0] = res->bitmap[1] = 0;
	res->status = htonl(NFS4ERR_BADHANDLE);

	dprintk("NFS: GETATTR callback request from %s\n",
		rpc_peeraddr2str(cps->clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	inode = nfs_delegation_find_inode(cps->clp, &args->fh);
	if (inode == NULL)
		goto out;
	nfsi = NFS_I(inode);
	rcu_read_lock();
	delegation = rcu_dereference(nfsi->delegation);
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
	rcu_read_unlock();
	iput(inode);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(res->status));
	return res->status;
}

__be32 nfs4_callback_recall(struct cb_recallargs *args, void *dummy,
			    struct cb_process_state *cps)
{
	struct inode *inode;
	__be32 res;
	
	res = htonl(NFS4ERR_OP_NOT_IN_SESSION);
	if (!cps->clp) /* Always set for v4.0. Set in cb_sequence for v4.1 */
		goto out;

	dprintk("NFS: RECALL callback request from %s\n",
		rpc_peeraddr2str(cps->clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	res = htonl(NFS4ERR_BADHANDLE);
	inode = nfs_delegation_find_inode(cps->clp, &args->fh);
	if (inode == NULL)
		goto out;
	/* Set up a helper thread to actually return the delegation */
	switch (nfs_async_inode_return_delegation(inode, &args->stateid)) {
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
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(res));
	return res;
}

int nfs4_validate_delegation_stateid(struct nfs_delegation *delegation, const nfs4_stateid *stateid)
{
	if (delegation == NULL || memcmp(delegation->stateid.data, stateid->data,
					 sizeof(delegation->stateid.data)) != 0)
		return 0;
	return 1;
}

#if defined(CONFIG_NFS_V4_1)

static bool
_recall_matches_lget(struct pnfs_cb_lrecall_info *cb_info,
		     struct inode *ino, struct pnfs_layout_range *range)
{
	struct cb_layoutrecallargs *cb_args = &cb_info->pcl_args;

	switch (cb_args->cbl_recall_type) {
	case RETURN_ALL:
		return true;
	case RETURN_FSID:
		return !memcmp(&NFS_SERVER(ino)->fsid, &cb_args->cbl_fsid,
			       sizeof(struct nfs_fsid));
	case RETURN_FILE:
		return (ino == cb_info->pcl_ino) &&
			should_free_lseg(range, &cb_args->cbl_range);
	default:
		/* Should never hit here, as decode_layoutrecall_args()
		 * will verify cb_info from server.
		 */
		BUG();
	}
}

bool
matches_outstanding_recall(struct inode *ino, struct pnfs_layout_range *range)
{
	struct nfs_client *clp = NFS_SERVER(ino)->nfs_client;
	struct pnfs_cb_lrecall_info *cb_info;
	bool rv = false;

	assert_spin_locked(&clp->cl_lock);
	list_for_each_entry(cb_info, &clp->cl_layoutrecalls, pcl_list) {
		if (_recall_matches_lget(cb_info, ino, range)) {
			rv = true;
			break;
		}
	}
	return rv;
}

/* Send a synchronous LAYOUTRETURN.  By the time this is called, we know
 * all IO has been drained, any matching lsegs deleted, and that no
 * overlapping LAYOUTGETs will be sent or processed for the duration
 * of this call.
 * Note that it is possible that when this is called, the stateid has
 * been invalidated.  But will not be cleared, so can still use.
 */
static int
pnfs_send_layoutreturn(struct nfs_client *clp,
		       struct pnfs_cb_lrecall_info *cb_info)
{
	struct cb_layoutrecallargs *args = &cb_info->pcl_args;
	struct nfs4_layoutreturn *lrp;

	lrp = kzalloc(sizeof(*lrp), GFP_KERNEL);
	if (!lrp)
		return -ENOMEM;
	lrp->args.reclaim = 0;
	lrp->args.layout_type = args->cbl_layout_type;
	lrp->args.return_type = args->cbl_recall_type;
	lrp->clp = clp;
	if (args->cbl_recall_type == RETURN_FILE) {
		lrp->args.range = args->cbl_range;
		lrp->args.inode = cb_info->pcl_ino;
	} else {
		lrp->args.range.iomode = IOMODE_ANY;
		lrp->args.inode = NULL;
	}
	return nfs4_proc_layoutreturn(lrp, true);
}

/* Called by state manager to finish CB_LAYOUTRECALLS initiated by
 * nfs4_callback_layoutrecall().
 */
void nfs_client_return_layouts(struct nfs_client *clp)
{
	struct pnfs_cb_lrecall_info *cb_info;

	spin_lock(&clp->cl_lock);
	while (true) {
		if (list_empty(&clp->cl_layoutrecalls)) {
			spin_unlock(&clp->cl_lock);
			break;
		}
		cb_info = list_first_entry(&clp->cl_layoutrecalls,
					   struct pnfs_cb_lrecall_info,
					   pcl_list);
		spin_unlock(&clp->cl_lock);
		if (atomic_read(&cb_info->pcl_count) != 0)
			break;
		/* What do on error return?  These layoutreturns are
		 * required by the protocol.  So if do not get
		 * successful reply, probably have to do something
		 * more drastic.
		 */
		pnfs_send_layoutreturn(clp, cb_info);
		spin_lock(&clp->cl_lock);
		/* Removing from the list unblocks LAYOUTGETs */
		list_del(&cb_info->pcl_list);
		clp->cl_cb_lrecall_count--;
		clp->cl_drain_notification[1 << cb_info->pcl_notify_bit] = NULL;
		rpc_wake_up(&clp->cl_rpcwaitq_recall);
		kfree(cb_info);
	}
}

void notify_drained(struct nfs_client *clp, u64 mask)
{
	atomic_t **ptr = clp->cl_drain_notification;
	bool done = false;

	/* clp lock not needed except to remove used up entries */
	/* Should probably use functions defined in bitmap.h */
	while (mask) {
		if ((mask & 1) && (atomic_dec_and_test(*ptr)))
			done = true;
		mask >>= 1;
		ptr++;
	}
	if (done) {
		set_bit(NFS4CLNT_LAYOUT_RECALL, &clp->cl_state);
		nfs4_schedule_state_manager(clp);
	}
}

static int initiate_layout_draining(struct pnfs_cb_lrecall_info *cb_info)
{
	struct nfs_client *clp = cb_info->pcl_clp;
	struct pnfs_layout_hdr *lo;
	int rv = NFS4ERR_NOMATCHING_LAYOUT;
	struct cb_layoutrecallargs *args = &cb_info->pcl_args;

	if (args->cbl_recall_type == RETURN_FILE) {
		LIST_HEAD(free_me_list);

		spin_lock(&clp->cl_lock);
		list_for_each_entry(lo, &clp->cl_layouts, layouts) {
			if (nfs_compare_fh(&args->cbl_fh,
					   &NFS_I(lo->inode)->fh))
				continue;
			if (test_bit(NFS_LAYOUT_BULK_RECALL, &lo->plh_flags))
				rv = NFS4ERR_DELAY;
			else {
				/* FIXME I need to better understand igrab and
				 * does having a layout ref keep ino around?
				 *  It should.
				 */
				/* We need to hold the reference until any
				 * potential LAYOUTRETURN is finished.
				 */
				get_layout_hdr(lo);
				cb_info->pcl_ino = lo->inode;
				rv = NFS4_OK;
			}
			break;
		}
		spin_unlock(&clp->cl_lock);

		spin_lock(&lo->inode->i_lock);
		if (rv == NFS4_OK) {
			lo->plh_block_lgets++;
			nfs4_asynch_forget_layouts(lo, &args->cbl_range,
						   cb_info->pcl_notify_bit,
						   &cb_info->pcl_count,
						   &free_me_list);
		}
		pnfs_set_layout_stateid(lo, &args->cbl_stateid, true);
		spin_unlock(&lo->inode->i_lock);
		pnfs_free_lseg_list(&free_me_list);
	} else {
		struct pnfs_layout_hdr *tmp;
		LIST_HEAD(recall_list);
		LIST_HEAD(free_me_list);
		struct pnfs_layout_range range = {
			.iomode = IOMODE_ANY,
			.offset = 0,
			.length = NFS4_MAX_UINT64,
		};

		spin_lock(&clp->cl_lock);
		/* Per RFC 5661, 12.5.5.2.1.5, bulk recall must be serialized */
		if (!list_is_singular(&clp->cl_layoutrecalls)) {
			spin_unlock(&clp->cl_lock);
			return NFS4ERR_DELAY;
		}
		list_for_each_entry(lo, &clp->cl_layouts, layouts) {
			if ((args->cbl_recall_type == RETURN_FSID) &&
			    memcmp(&NFS_SERVER(lo->inode)->fsid,
				   &args->cbl_fsid, sizeof(struct nfs_fsid)))
				continue;
			get_layout_hdr(lo);
			/* We could list_del(&lo->layouts) here */
			BUG_ON(!list_empty(&lo->plh_bulk_recall));
			list_add(&lo->plh_bulk_recall, &recall_list);
		}
		spin_unlock(&clp->cl_lock);
		list_for_each_entry_safe(lo, tmp,
					 &recall_list, plh_bulk_recall) {
			spin_lock(&lo->inode->i_lock);
			set_bit(NFS_LAYOUT_BULK_RECALL, &lo->plh_flags);
			nfs4_asynch_forget_layouts(lo, &range,
						   cb_info->pcl_notify_bit,
						   &cb_info->pcl_count,
						   &free_me_list);
			list_del_init(&lo->plh_bulk_recall);
			spin_unlock(&lo->inode->i_lock);
			put_layout_hdr(lo);
			rv = NFS4_OK;
		}
		pnfs_free_lseg_list(&free_me_list);
	}
	return rv;
}

static u32 do_callback_layoutrecall(struct nfs_client *clp,
				    struct cb_layoutrecallargs *args)
{
	struct pnfs_cb_lrecall_info *new;
	atomic_t **ptr;
	int bit_num;
	u32 res;

	dprintk("%s enter, type=%i\n", __func__, args->cbl_recall_type);
	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new) {
		res = NFS4ERR_DELAY;
		goto out;
	}
	memcpy(&new->pcl_args, args, sizeof(*args));
	atomic_set(&new->pcl_count, 1);
	new->pcl_clp = clp;
	new->pcl_ino = NULL;
	spin_lock(&clp->cl_lock);
	if (clp->cl_cb_lrecall_count >= PNFS_MAX_CB_LRECALLS) {
		kfree(new);
		res = NFS4ERR_DELAY;
		spin_unlock(&clp->cl_lock);
		goto out;
	}
	clp->cl_cb_lrecall_count++;
	/* Adding to the list will block conflicting LGET activity */
	list_add_tail(&new->pcl_list, &clp->cl_layoutrecalls);
	for (bit_num = 0, ptr = clp->cl_drain_notification; *ptr; ptr++)
		bit_num++;
	*ptr = &new->pcl_count;
	new->pcl_notify_bit = bit_num;
	spin_unlock(&clp->cl_lock);
	res = initiate_layout_draining(new);
	if (res || atomic_dec_and_test(&new->pcl_count)) {
		spin_lock(&clp->cl_lock);
		list_del(&new->pcl_list);
		clp->cl_cb_lrecall_count--;
		clp->cl_drain_notification[1 << bit_num] = NULL;
		rpc_wake_up(&clp->cl_rpcwaitq_recall);
		spin_unlock(&clp->cl_lock);
		if (res == NFS4_OK) {
			if (args->cbl_recall_type == RETURN_FILE) {
				struct pnfs_layout_hdr *lo;

				lo = NFS_I(new->pcl_ino)->layout;
				spin_lock(&lo->inode->i_lock);
				lo->plh_block_lgets--;
				if (!pnfs_layoutgets_blocked(lo, NULL))
					rpc_wake_up(&NFS_I(lo->inode)->lo_rpcwaitq_stateid);
				spin_unlock(&lo->inode->i_lock);
				put_layout_hdr(lo);
			}
			res = NFS4ERR_NOMATCHING_LAYOUT;
		}
		kfree(new);
	}
out:
	dprintk("%s returning %i\n", __func__, res);
	return res;

}

__be32 nfs4_callback_layoutrecall(struct cb_layoutrecallargs *args,
				  void *dummy, struct cb_process_state *cps)
{
	u32 res;

	dprintk("%s: -->\n", __func__);

	if (cps->clp)
		res = do_callback_layoutrecall(cps->clp, args);
	else
		res = NFS4ERR_OP_NOT_IN_SESSION;

	dprintk("%s: exit with status = %d\n", __func__, res);
	return cpu_to_be32(res);
}

static void pnfs_recall_all_layouts(struct nfs_client *clp)
{
	struct cb_layoutrecallargs args;

	/* Pretend we got a CB_LAYOUTRECALL(ALL) */
	memset(&args, 0, sizeof(args));
	args.cbl_recall_type = RETURN_ALL;
	/* FIXME we ignore errors, what should we do? */
	do_callback_layoutrecall(clp, &args);
}

__be32 nfs4_callback_devicenotify(struct cb_devicenotifyargs *args,
				  void *dummy, struct cb_process_state *cps)
{
	int i;
	u32 type, res = 0;

	dprintk("%s: -->\n", __func__);

	if (!cps->clp) {
		res = NFS4ERR_OP_NOT_IN_SESSION;
		goto out;
	}

	for (i = 0; i < args->ndevs; i++) {
		struct cb_devicenotifyitem *dev = &args->devs[i];
		type = dev->cbd_notify_type;
		if (type == NOTIFY_DEVICEID4_DELETE && cps->clp->cl_devid_cache)
			pnfs_delete_deviceid(cps->clp->cl_devid_cache,
					     &dev->cbd_dev_id);
		else if (type == NOTIFY_DEVICEID4_CHANGE)
			printk(KERN_ERR "%s: NOTIFY_DEVICEID4_CHANGE "
					"not supported\n", __func__);
	}

out:
	dprintk("%s: exit with status = %u\n",
		__func__, res);
	return cpu_to_be32(res);
}

int nfs41_validate_delegation_stateid(struct nfs_delegation *delegation, const nfs4_stateid *stateid)
{
	if (delegation == NULL)
		return 0;

	if (stateid->stateid.seqid != 0)
		return 0;
	if (memcmp(&delegation->stateid.stateid.other,
		   &stateid->stateid.other,
		   NFS4_STATEID_OTHER_SIZE))
		return 0;

	return 1;
}

/*
 * Validate the sequenceID sent by the server.
 * Return success if the sequenceID is one more than what we last saw on
 * this slot, accounting for wraparound.  Increments the slot's sequence.
 *
 * We don't yet implement a duplicate request cache, instead we set the
 * back channel ca_maxresponsesize_cached to zero. This is OK for now
 * since we only currently implement idempotent callbacks anyway.
 *
 * We have a single slot backchannel at this time, so we don't bother
 * checking the used_slots bit array on the table.  The lower layer guarantees
 * a single outstanding callback request at a time.
 */
static __be32
validate_seqid(struct nfs4_slot_table *tbl, struct cb_sequenceargs * args)
{
	struct nfs4_slot *slot;

	dprintk("%s enter. slotid %d seqid %d\n",
		__func__, args->csa_slotid, args->csa_sequenceid);

	if (args->csa_slotid > NFS41_BC_MAX_CALLBACKS)
		return htonl(NFS4ERR_BADSLOT);

	slot = tbl->slots + args->csa_slotid;
	dprintk("%s slot table seqid: %d\n", __func__, slot->seq_nr);

	/* Normal */
	if (likely(args->csa_sequenceid == slot->seq_nr + 1)) {
		slot->seq_nr++;
		return htonl(NFS4_OK);
	}

	/* Replay */
	if (args->csa_sequenceid == slot->seq_nr) {
		dprintk("%s seqid %d is a replay\n",
			__func__, args->csa_sequenceid);
		/* Signal process_op to set this error on next op */
		if (args->csa_cachethis == 0)
			return htonl(NFS4ERR_RETRY_UNCACHED_REP);

		/* The ca_maxresponsesize_cached is 0 with no DRC */
		else if (args->csa_cachethis == 1)
			return htonl(NFS4ERR_REP_TOO_BIG_TO_CACHE);
	}

	/* Wraparound */
	if (args->csa_sequenceid == 1 && (slot->seq_nr + 1) == 0) {
		slot->seq_nr = 1;
		return htonl(NFS4_OK);
	}

	/* Misordered request */
	return htonl(NFS4ERR_SEQ_MISORDERED);
}

/*
 * For each referring call triple, check the session's slot table for
 * a match.  If the slot is in use and the sequence numbers match, the
 * client is still waiting for a response to the original request.
 */
static bool referring_call_exists(struct nfs_client *clp,
				  uint32_t nrclists,
				  struct referring_call_list *rclists)
{
	bool status = 0;
	int i, j;
	struct nfs4_session *session;
	struct nfs4_slot_table *tbl;
	struct referring_call_list *rclist;
	struct referring_call *ref;

	/*
	 * XXX When client trunking is implemented, this becomes
	 * a session lookup from within the loop
	 */
	session = clp->cl_session;
	tbl = &session->fc_slot_table;

	for (i = 0; i < nrclists; i++) {
		rclist = &rclists[i];
		if (memcmp(session->sess_id.data,
			   rclist->rcl_sessionid.data,
			   NFS4_MAX_SESSIONID_LEN) != 0)
			continue;

		for (j = 0; j < rclist->rcl_nrefcalls; j++) {
			ref = &rclist->rcl_refcalls[j];

			dprintk("%s: sessionid %x:%x:%x:%x sequenceid %u "
				"slotid %u\n", __func__,
				((u32 *)&rclist->rcl_sessionid.data)[0],
				((u32 *)&rclist->rcl_sessionid.data)[1],
				((u32 *)&rclist->rcl_sessionid.data)[2],
				((u32 *)&rclist->rcl_sessionid.data)[3],
				ref->rc_sequenceid, ref->rc_slotid);

			spin_lock(&tbl->slot_tbl_lock);
			status = (test_bit(ref->rc_slotid, tbl->used_slots) &&
				  tbl->slots[ref->rc_slotid].seq_nr ==
					ref->rc_sequenceid);
			spin_unlock(&tbl->slot_tbl_lock);
			if (status)
				goto out;
		}
	}

out:
	return status;
}

__be32 nfs4_callback_sequence(struct cb_sequenceargs *args,
			      struct cb_sequenceres *res,
			      struct cb_process_state *cps)
{
	struct nfs_client *clp;
	int i;
	__be32 status;

	cps->clp = NULL;

	status = htonl(NFS4ERR_BADSESSION);
	/* Incoming session must match the callback session */
	if (memcmp(&args->csa_sessionid, cps->svc_sid, NFS4_MAX_SESSIONID_LEN))
		goto out;

	clp = nfs4_find_client_sessionid(args->csa_addr,
					 &args->csa_sessionid, 1);
	if (clp == NULL)
		goto out;

	/* state manager is resetting the session */
	if (test_bit(NFS4_SESSION_DRAINING, &clp->cl_session->session_state)) {
		status = NFS4ERR_DELAY;
		goto out;
	}

	status = validate_seqid(&clp->cl_session->bc_slot_table, args);
	if (status)
		goto out;

	/*
	 * Check for pending referring calls.  If a match is found, a
	 * related callback was received before the response to the original
	 * call.
	 */
	if (referring_call_exists(clp, args->csa_nrclists, args->csa_rclists)) {
		status = htonl(NFS4ERR_DELAY);
		goto out;
	}

	memcpy(&res->csr_sessionid, &args->csa_sessionid,
	       sizeof(res->csr_sessionid));
	res->csr_sequenceid = args->csa_sequenceid;
	res->csr_slotid = args->csa_slotid;
	res->csr_highestslotid = NFS41_BC_MAX_CALLBACKS - 1;
	res->csr_target_highestslotid = NFS41_BC_MAX_CALLBACKS - 1;
	cps->clp = clp; /* put in nfs4_callback_compound */

out:
	for (i = 0; i < args->csa_nrclists; i++)
		kfree(args->csa_rclists[i].rcl_refcalls);
	kfree(args->csa_rclists);

	if (status == htonl(NFS4ERR_RETRY_UNCACHED_REP)) {
		cps->drc_status = status;
		status = 0;
	} else
		res->csr_status = status;

	dprintk("%s: exit with status = %d res->csr_status %d\n", __func__,
		ntohl(status), ntohl(res->csr_status));
	return status;
}

static bool
validate_bitmap_values(unsigned long mask)
{
	return (mask & ~RCA4_TYPE_MASK_ALL) == 0;
}

__be32 nfs4_callback_recallany(struct cb_recallanyargs *args, void *dummy,
			       struct cb_process_state *cps)
{
	__be32 status;
	fmode_t flags = 0;

	status = cpu_to_be32(NFS4ERR_OP_NOT_IN_SESSION);
	if (!cps->clp) /* set in cb_sequence */
		goto out;

	dprintk("NFS: RECALL_ANY callback request from %s\n",
		rpc_peeraddr2str(cps->clp->cl_rpcclient, RPC_DISPLAY_ADDR));

	status = cpu_to_be32(NFS4ERR_INVAL);
	if (!validate_bitmap_values(args->craa_type_mask))
		goto out;

	status = cpu_to_be32(NFS4_OK);
	if (test_bit(RCA4_TYPE_MASK_RDATA_DLG, (const unsigned long *)
		     &args->craa_type_mask))
		flags = FMODE_READ;
	if (test_bit(RCA4_TYPE_MASK_WDATA_DLG, (const unsigned long *)
		     &args->craa_type_mask))
		flags |= FMODE_WRITE;
	if (test_bit(RCA4_TYPE_MASK_FILE_LAYOUT, (const unsigned long *)
		     &args->craa_type_mask))
		pnfs_recall_all_layouts(cps->clp);
	if (flags)
		nfs_expire_all_delegation_types(cps->clp, flags);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(status));
	return status;
}

/* Reduce the fore channel's max_slots to the target value */
__be32 nfs4_callback_recallslot(struct cb_recallslotargs *args, void *dummy,
				struct cb_process_state *cps)
{
	struct nfs4_slot_table *fc_tbl;
	__be32 status;

	status = htonl(NFS4ERR_OP_NOT_IN_SESSION);
	if (!cps->clp) /* set in cb_sequence */
		goto out;

	dprintk("NFS: CB_RECALL_SLOT request from %s target max slots %d\n",
		rpc_peeraddr2str(cps->clp->cl_rpcclient, RPC_DISPLAY_ADDR),
		args->crsa_target_max_slots);

	fc_tbl = &cps->clp->cl_session->fc_slot_table;

	status = htonl(NFS4ERR_BAD_HIGH_SLOT);
	if (args->crsa_target_max_slots > fc_tbl->max_slots ||
	    args->crsa_target_max_slots < 1)
		goto out;

	status = htonl(NFS4_OK);
	if (args->crsa_target_max_slots == fc_tbl->max_slots)
		goto out;

	fc_tbl->target_max_slots = args->crsa_target_max_slots;
	nfs41_handle_recall_slot(cps->clp);
out:
	dprintk("%s: exit with status = %d\n", __func__, ntohl(status));
	return status;
}
#endif /* CONFIG_NFS_V4_1 */
