#ifndef NFSD4_BLOCK
#define NFSD4_BLOCK

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcauth.h>
#include <linux/nfsd/nfsfh.h>
#include <linux/nfsd/nfsd4_pnfs.h>

#define PNFS_BLOCK_SUCCESS		1
#define PNFS_BLOCK_FAILURE		0

#define PNFS_BLOCK_CTL_START		1
#define PNFS_BLOCK_CTL_STOP		2
#define PNFS_BLOCK_CTL_VERS		3 /* Allows daemon to request current
					   * version from kernel via an upcall.
					   */

#define PNFS_UPCALL_MSG_STOP	0
#define PNFS_UPCALL_MSG_GETSIG	1
#define PNFS_UPCALL_MSG_GETSLICE	2
#define PNFS_UPCALL_MSG_DMCHK	3	// See if dev_t is a DM volume
#define PNFS_UPCALL_MSG_DMGET	4
#define PNFS_UPCALL_MSG_VERS	5

#define PNFS_UPCALL_VERS		8

typedef struct stripe_dev {
	int	major,
		minor,
		offset;
} stripe_dev_t;

typedef struct bl_comm_res {
	int				res_status;
	union {
		struct {
			long long	start,
					length;
		} slice;
		struct {
			int		num_stripes,
					stripe_size;
			stripe_dev_t	devs[];
		} stripe;
		struct {
			long long	sector;
			int		offset,
					len;
			char		sig[];
		} sig;
		int			vers,
					dm_vol;
	} u;
} bl_comm_res_t;

typedef struct bl_comm_msg {
	int		msg_type,
			msg_status;
	union {
		dev_t	msg_dev;
		int	msg_vers;
	} u;
	bl_comm_res_t	*msg_res;
} bl_comm_msg_t;

#ifdef __KERNEL__

typedef struct bl_comm {
	/* ---- protects access to this structure ---- */
	struct mutex		lock;
	/* ---- protects access to rpc pipe ---- */
	struct mutex		pipe_lock;
	struct dentry		*pipe_dentry;
	wait_queue_head_t	pipe_wq;
	bl_comm_msg_t		msg;
} bl_comm_t;

int pnfs_block_enabled(struct inode *, int);
int bl_layout_type(struct super_block *sb);
int bl_getdeviceiter(struct super_block *, u32 layout_type,
		     struct nfsd4_pnfs_dev_iter_res *);
int bl_getdeviceinfo(struct super_block *, struct exp_xdr_stream *,
		     u32 layout_type,
		     const struct nfsd4_pnfs_deviceid *);
u32 bl_layoutget(struct inode *, struct exp_xdr_stream *,
		 const struct nfsd4_pnfs_layoutget_arg *,
		 struct nfsd4_pnfs_layoutget_res *);
int bl_layoutcommit(struct inode *,
		    const struct nfsd4_pnfs_layoutcommit_arg *,
		    struct nfsd4_pnfs_layoutcommit_res *);
int bl_layoutreturn(struct inode *,
		    const struct nfsd4_pnfs_layoutreturn_arg *);
int bl_layoutrecall(struct inode *inode, int type, u64 offset, u64 len);
int bl_init_proc(void);
int bl_upcall(bl_comm_t *, bl_comm_msg_t *, bl_comm_res_t **);

extern bl_comm_t	*bl_comm_global;	// Ugly...
#endif /* __KERNEL__ */

#endif /* NFSD4_BLOCK */

