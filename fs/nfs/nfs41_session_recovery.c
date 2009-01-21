/*
 * NFSv4.1 session recovery code
 *
 * Author: Rahul Iyer <iyer@netapp.com>
 *
 * This code is released under GPL. For details see Documentation/COPYING
 */

#if defined(CONFIG_NFS_V4_1)

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs3.h>
#include <linux/nfs.h>
#include <linux/nfs_fs.h>
#include <linux/namei.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_xdr.h>
#include <linux/nfs41_session_recovery.h>
#include "nfs4_fs.h"
#include "internal.h"

#define NFSDBG_FACILITY		NFSDBG_PROC

static int nfs41_start_session_recovery(struct nfs4_session *session)
{
	int ret;
	ret = test_and_set_bit(NFS41_SESSION_RECOVER, &session->session_state);

	return ret;
}

/*
 * Session reset
 */

int nfs41_wait_session_reset(struct nfs4_session *session)
{
	might_sleep();
	return wait_on_bit(&session->session_state, NFS41_SESSION_RESET,
			   nfs4_wait_bit_killable, TASK_KILLABLE);
}

struct reclaimer_arg {
	struct nfs4_session *session;
};

static int nfs41_end_session_recovery(struct nfs4_session *session)
{
	smp_mb__before_clear_bit();
	clear_bit(NFS41_SESSION_RECOVER, &session->session_state);
	smp_mb__after_clear_bit();

	/*
	 * Wake up sync tasks
	 */
	wake_up_bit(&session->session_state, NFS41_SESSION_RECOVER);
	return 0;
}

static int nfs41_wait_session_recover_sync(struct nfs4_session *session)
{
	might_sleep();
	return wait_on_bit(&session->session_state, NFS41_SESSION_RECOVER,
			   nfs4_wait_bit_killable, TASK_KILLABLE);
}

static int session_reclaimer(void *arg)
{
	int ret, reset;
	struct reclaimer_arg *rec = (struct reclaimer_arg *)arg;
	struct nfs_client *clp = rec->session->clp;

	dprintk("--> %s session %p\n", __func__, rec->session);
	allow_signal(SIGKILL);

	reset = nfs41_test_session_reset(rec->session);
	if (reset) {
		dprintk("%s Session Reset\n", __func__);
		/* Reset is called only when all slots are clear.
		 *
		 * Bail on the reset if destroy session op fails or if
		 * the session ref_count is not 1
		 *
		 * Of course since we are resetting the session,
		 * it's OK if the session is already destroyed
		 * on the server.
		 */
		ret = nfs4_proc_destroy_session(rec->session);
		switch (ret) {
		case 0:
		case -NFS4ERR_BADSESSION:
		case -NFS4ERR_DEADSESSION:
			break;
		default:
			goto out_error;
		}
		memset(rec->session->sess_id.data, 0, NFS4_MAX_SESSIONID_LEN);
	}
create:
	ret = nfs4_proc_create_session(clp, reset);
	if (ret)
		goto out_error;

out:
	if (reset) {
		struct nfs4_slot_table *tbl;

		tbl = &rec->session->fc_slot_table;
		/* Need to clear reset bit and wake up the next rpc task
		 * even on error */
		nfs41_clear_session_reset(rec->session);
		rpc_wake_up_next(&tbl->slot_tbl_waitq);
	}
	nfs41_end_session_recovery(rec->session);
	kfree(rec);
	module_put_and_exit(0);
	dprintk("<-- %s: status=%d\n", __func__, ret);
	return ret;
out_error:
	printk(KERN_WARNING "Error: session recovery failed on "
		"NFSv4.1 server with error %d\n", ret);

	switch (ret) {
	case -NFS4ERR_STALE_CLIENTID:
		if (exchgid_is_ds_only(clp)) {
			dprintk("%s DS Clientid Reset\n", __func__);
			ret = nfs4_proc_exchange_id(clp, clp->cl_machine_cred);
			if (ret) {
				nfs_put_client(clp);
				goto out;
			}
			goto create;
		} else {
			dprintk("%s SET NFS4CLNT_LEASE_EXPIRED\n", __func__);
			set_bit(NFS4CLNT_LEASE_EXPIRED,
				&rec->session->clp->cl_state);
			goto out;
		}
	}
	goto out;
}

static int nfs41_schedule_session_recovery(struct reclaimer_arg *rec)
{
	struct task_struct *task;

	dprintk("--> %s: spawning session_reclaimer\n", __func__);
	__module_get(THIS_MODULE);
	task = kthread_run(session_reclaimer, rec, "%llx-session-reclaim",
			   *(unsigned long long *)&rec->session->sess_id);

	if (!IS_ERR(task)) {
		dprintk("<-- %s\n", __func__);
		return 0;
	}

	module_put(THIS_MODULE);
	dprintk("--> %s: failed spawning session_reclaimer: error=%ld\n",
		__func__, PTR_ERR(task));
	return PTR_ERR(task);
}

/*
 * Session recovery
 * Called when an op receives a session related error
 */
int nfs41_recover_session(struct nfs4_session *session)
{
	struct reclaimer_arg *rec = NULL;
	int ret;

	dprintk("--> %s: clp=%p session=%p\n", __func__, session->clp, session);

	ret = nfs41_start_session_recovery(session);

	/*
	 * If we get 1, it means some other thread beat us to us here, so we
	 * just sit back and wait for completion of the recovery process
	 */
	if (ret) {
		dprintk("%s: session_recovery already started\n", __func__);
		ret = 0;
		goto out;
	}

	ret = -ENOMEM;
	rec = kmalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		goto err;
	rec->session = session;

	ret = nfs41_schedule_session_recovery(rec);
	/*
	 * We got an error creating the reclaiming thread, so end the recovery
	 * and bail out
	 */
	if (ret)
		goto err;
out:
	dprintk("<-- %s status=%d\n", __func__, ret);
	return ret;
err:
	nfs41_end_session_recovery(session);
	kfree(rec);
	goto out;
}

int nfs41_recover_session_sync(struct nfs4_session *session)
{
	int ret;

	dprintk("--> %s\n", __func__);

	ret = nfs41_recover_session(session);
	if (!ret)
		ret = nfs41_wait_session_recover_sync(session);

	dprintk("<-- %s: status=%d\n", __func__, ret);
	return ret;
}

#endif /* CONFIG_NFS_V4_1 */
