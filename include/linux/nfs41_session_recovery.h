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
 */
enum nfs41_session_state {
	NFS41_SESSION_ALLOC = 0,
	NFS41_SESSION_RECOVER,
};

/*
 * Set the session state to alloc
 */
static inline int nfs41_set_session_alloc(struct nfs4_session *session)
{
	return test_and_set_bit(NFS41_SESSION_ALLOC, &session->session_state);
}

static inline int nfs41_test_session_alloc(struct nfs4_session *session)
{
	return test_bit(NFS41_SESSION_ALLOC, &session->session_state);
}

int nfs41_set_session_valid(struct nfs4_session *);
int nfs41_recover_session(struct nfs4_session *);
int nfs41_recover_session_sync(struct nfs4_session *);

#endif	/* CONFIG_NFS_V4_1 */
#endif	/* __NFS41_SESSION_RECOVERY_H__ */
