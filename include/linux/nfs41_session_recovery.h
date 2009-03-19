/*
 * Session Recovery header file
 *
 * Author: Rahul Iyer <iyer@netapp.com>
 *
 * This code is released under GPL. For details see Documentation/COPYING
 */

#ifndef __NFS41_SESSION_RECOVERY_H__
#define __NFS41_SESSION_RECOVERY_H__

#if defined(CONFIG_NFS_V4_1)

/*
 * Session state bits
 *
 * State machine.
 * NFS41_SESSION_ALLOC bit set: session is ready for create_session call.
 * NFS41_SESSION_RECOVER bit set: session is being reset/recovered
 * NFS41_SESSION_ALLOC bit unset: valid session in use.
 * NFS41_SESSION_RESET bit set: session is being reset.
 */
enum nfs41_session_state {
	NFS41_SESSION_RECOVER = 0,
	NFS41_SESSION_RESET,
};

/*
 * Set, test, and clear the session reset state
 */
static inline int nfs41_set_session_reset(struct nfs4_session *session)
{
	return test_and_set_bit(NFS41_SESSION_RESET, &session->session_state);
}

static inline int nfs41_test_session_reset(struct nfs4_session *session)
{
	return test_bit(NFS41_SESSION_RESET, &session->session_state);
}

static inline void nfs41_clear_session_reset(struct nfs4_session *session)
{
	smp_mb__before_clear_bit();
	clear_bit(NFS41_SESSION_RESET,
			   &session->session_state);
	smp_mb__after_clear_bit();
	wake_up_bit(&session->session_state, NFS41_SESSION_RESET);
}

int nfs41_wait_session_reset(struct nfs4_session *session);
int nfs41_recover_session(struct nfs4_session *);
int nfs41_recover_session_sync(struct nfs4_session *);

#endif	/* CONFIG_NFS_V4_1 */
#endif	/* __NFS41_SESSION_RECOVERY_H__ */
