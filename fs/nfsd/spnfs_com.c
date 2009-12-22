/*
 * fs/nfsd/spnfs_com.c
 *
 * Communcation layer between spNFS kernel and userspace
 * Based heavily on idmap.c
 *
 */

/*
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Marius Aamodt Eriksen <marius@umich.edu>
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
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/rpc_pipe_fs.h>
#include <linux/nfsd/debug.h>

#include <linux/nfsd4_spnfs.h>

#define	NFSDDBG_FACILITY		NFSDDBG_PROC

static ssize_t   spnfs_pipe_upcall(struct file *, struct rpc_pipe_msg *,
		     char __user *, size_t);
static ssize_t   spnfs_pipe_downcall(struct file *, const char __user *,
		     size_t);
static void      spnfs_pipe_destroy_msg(struct rpc_pipe_msg *);

static struct rpc_pipe_ops spnfs_upcall_ops = {
	.upcall		= spnfs_pipe_upcall,
	.downcall	= spnfs_pipe_downcall,
	.destroy_msg	= spnfs_pipe_destroy_msg,
};

/* evil global variable */
struct spnfs *global_spnfs;
struct spnfs_config *spnfs_config;
#ifdef CONFIG_SPNFS_LAYOUTSEGMENTS
int spnfs_use_layoutsegments;
uint64_t layoutsegment_size;
#endif /* CONFIG_SPNFS_LAYOUTSEGMENTS */

/*
 * Used by spnfs_enabled()
 * Tracks if the subsystem has been initialized at some point.  It doesn't
 * matter if it's not currently initialized.
 */
static int spnfs_enabled_at_some_point;

/* call this to start the ball rolling */
/* code it like we're going to avoid the global variable in the future */
int
nfsd_spnfs_new(void)
{
	struct spnfs *spnfs = NULL;
	struct path path;
	struct nameidata nd;
	int rc;

	if (global_spnfs != NULL)
		return -EEXIST;

	path.mnt = rpc_get_mount();
	if (IS_ERR(path.mnt))
		return PTR_ERR(path.mnt);

	/* FIXME: do not abuse rpc_pipefs/nfs */
	rc = vfs_path_lookup(path.mnt->mnt_root, path.mnt, "/nfs", 0, &nd);
	if (rc)
		goto err;

	spnfs = kzalloc(sizeof(*spnfs), GFP_KERNEL);
	if (spnfs == NULL){
		rc = -ENOMEM;
		goto err;
	}

	spnfs->spnfs_dentry = rpc_mkpipe(nd.path.dentry, "spnfs", spnfs,
					 &spnfs_upcall_ops, 0);
	if (IS_ERR(spnfs->spnfs_dentry)) {
		rc = -EPIPE;
		goto err;
	}

	mutex_init(&spnfs->spnfs_lock);
	mutex_init(&spnfs->spnfs_plock);
	init_waitqueue_head(&spnfs->spnfs_wq);

	global_spnfs = spnfs;
	spnfs_enabled_at_some_point = 1;

	return 0;
err:
	rpc_put_mount();
	kfree(spnfs);
	return rc;
}

/* again, code it like we're going to remove the global variable */
void
nfsd_spnfs_delete(void)
{
	struct spnfs *spnfs = global_spnfs;

	if (!spnfs)
		return;
	rpc_unlink(spnfs->spnfs_dentry);
	rpc_put_mount();
	global_spnfs = NULL;
	kfree(spnfs);
}

/* RPC pipefs upcall/downcall routines */
/* looks like this code is invoked by the rpc_pipe code */
/* to handle upcalls on things we've queued elsewhere */
/* See nfs_idmap_id for an exmaple of enqueueing */
static ssize_t
spnfs_pipe_upcall(struct file *filp, struct rpc_pipe_msg *msg,
    char __user *dst, size_t buflen)
{
	char *data = (char *)msg->data + msg->copied;
	ssize_t mlen = msg->len - msg->copied;
	ssize_t left;

	if (mlen > buflen)
		mlen = buflen;

	left = copy_to_user(dst, data, mlen);
	if (left < 0) {
		msg->errno = left;
		return left;
	}
	mlen -= left;
	msg->copied += mlen;
	msg->errno = 0;
	return mlen;
}

static ssize_t
spnfs_pipe_downcall(struct file *filp, const char __user *src, size_t mlen)
{
	struct rpc_inode *rpci = RPC_I(filp->f_dentry->d_inode);
	struct spnfs *spnfs = (struct spnfs *)rpci->private;
	struct spnfs_msg *im_in = NULL, *im = &spnfs->spnfs_im;
	int ret;

	if (mlen != sizeof(struct spnfs_msg))
		return -ENOSPC;

	im_in = kmalloc(sizeof(struct spnfs_msg), GFP_KERNEL);
	if (im_in == NULL)
		return -ENOMEM;

	if (copy_from_user(im_in, src, mlen) != 0)
		return -EFAULT;

	mutex_lock(&spnfs->spnfs_plock);

	ret = mlen;
	im->im_status = im_in->im_status;
	/* If we got an error, terminate now, and wake up pending upcalls */
	if (!(im_in->im_status & SPNFS_STATUS_SUCCESS)) {
		wake_up(&spnfs->spnfs_wq);
		goto out;
	}

	ret = -EINVAL;
	/* Did we match the current upcall? */
	/* DMXXX: do not understand the comment above, from original code */
	/* DMXXX: when do we _not_ match the current upcall? */
	/* DMXXX: anyway, let's to a simplistic check */
	if (im_in->im_type == im->im_type) {
		/* copy the response into the spnfs struct */
		memcpy(&im->im_res, &im_in->im_res, sizeof(im->im_res));
		ret = mlen;
	} else
		dprintk("spnfs: downcall type != upcall type\n");


	wake_up(&spnfs->spnfs_wq);
/* DMXXX handle rval processing */
out:
	mutex_unlock(&spnfs->spnfs_plock);
	kfree(im_in);
	return ret;
}

static void
spnfs_pipe_destroy_msg(struct rpc_pipe_msg *msg)
{
	struct spnfs_msg *im = msg->data;
	struct spnfs *spnfs = container_of(im, struct spnfs, spnfs_im);

	if (msg->errno >= 0)
		return;
	mutex_lock(&spnfs->spnfs_plock);
	im->im_status = SPNFS_STATUS_FAIL;  /* DMXXX */
	wake_up(&spnfs->spnfs_wq);
	mutex_unlock(&spnfs->spnfs_plock);
}

/* generic upcall.  called by functions in spnfs_ops.c  */
int
spnfs_upcall(struct spnfs *spnfs, struct spnfs_msg *upmsg,
		union spnfs_msg_res *res)
{
	struct rpc_pipe_msg msg;
	struct spnfs_msg *im;
	DECLARE_WAITQUEUE(wq, current);
	int ret = -EIO;
	int rval;

	im = &spnfs->spnfs_im;

	mutex_lock(&spnfs->spnfs_lock);
	mutex_lock(&spnfs->spnfs_plock);

	memset(im, 0, sizeof(*im));
	memcpy(im, upmsg, sizeof(*upmsg));

	memset(&msg, 0, sizeof(msg));
	msg.data = im;
	msg.len = sizeof(*im);

	add_wait_queue(&spnfs->spnfs_wq, &wq);
	rval = rpc_queue_upcall(spnfs->spnfs_dentry->d_inode, &msg);
	if (rval < 0) {
		remove_wait_queue(&spnfs->spnfs_wq, &wq);
		goto out;
	}

	set_current_state(TASK_UNINTERRUPTIBLE);
	mutex_unlock(&spnfs->spnfs_plock);
	schedule();
	current->state = TASK_RUNNING;
	remove_wait_queue(&spnfs->spnfs_wq, &wq);
	mutex_lock(&spnfs->spnfs_plock);

	if (im->im_status & SPNFS_STATUS_SUCCESS) {
		/* copy our result from the upcall */
		memcpy(res, &im->im_res, sizeof(*res));
		ret = 0;
	}

out:
	memset(im, 0, sizeof(*im));
	mutex_unlock(&spnfs->spnfs_plock);
	mutex_unlock(&spnfs->spnfs_lock);
	return(ret);
}

/*
 * This is used to determine if the spnfsd daemon has been started at
 * least once since the system came up.  This is used to by the export
 * mechanism to decide if spnfs is in use.
 *
 * Returns non-zero if the spnfsd has initialized the communication pipe
 * at least once.
 */
int spnfs_enabled(void)
{
	return spnfs_enabled_at_some_point;
}

#ifdef CONFIG_PROC_FS

/*
 * procfs virtual files for user/kernel space communication:
 *
 * ctl - currently just an on/off switch...can be expanded
 * getfh - fd to fh conversion
 * recall - recall a layout from the command line, for example:
 *		echo <path> > /proc/fs/spnfs/recall
 * config - configuration info, e.g., stripe size, num ds, etc.
 */

/*-------------- start ctl -------------------------*/
static ssize_t ctl_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *offset)
{
	int cmd, rc;

	if (copy_from_user((int *)&cmd, (int *)buf, sizeof(int)))
		return -EFAULT;
	if (cmd) {
		rc = nfsd_spnfs_new();
		if (rc != 0)
			return rc;
	} else
		nfsd_spnfs_delete();

	return count;
}

static const struct file_operations ctl_ops = {
	.write		= ctl_write,
};
/*-------------- end ctl ---------------------------*/

/*-------------- start config -------------------------*/
static ssize_t config_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *offset)
{
	static struct spnfs_config cfg;

	if (copy_from_user(&cfg, buf, count))
		return -EFAULT;

	spnfs_config = &cfg;
	return 0;
}

static const struct file_operations config_ops = {
	.write		= config_write,
};
/*-------------- end config ---------------------------*/

/*-------------- start getfh -----------------------*/
static int getfh_open(struct inode *inode, struct file *file)
{
	file->private_data = kmalloc(sizeof(struct nfs_fh), GFP_KERNEL);
	if (file->private_data == NULL)
		return -ENOMEM;

	return 0;
}

static ssize_t getfh_read(struct file *file, char __user *buf, size_t count,
			  loff_t *offset)
{
	if (copy_to_user(buf, file->private_data, sizeof(struct nfs_fh)))
		return -EFAULT;

	return count;
}

static ssize_t getfh_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *offset)
{
	int fd;

	if (copy_from_user((int *)&fd, (int *)buf, sizeof(int)))
		return -EFAULT;
	if (spnfs_getfh(fd, file->private_data) != 0)
		return -EIO;

	return count;
}

static int getfh_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static const struct file_operations getfh_ops = {
	.open		= getfh_open,
	.read		= getfh_read,
	.write		= getfh_write,
	.release	= getfh_release,
};
/*-------------- end getfh ------------------------*/


/*-------------- start recall layout --------------*/
static ssize_t recall_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *offset)
{
	char input[128];
	char *path, *str, *p;
	int rc;
	u64 off = 0, len = 0;

	if (count > 128)
		return -EINVAL;

	if (copy_from_user(input, buf, count))
		return -EFAULT;

	/* assumes newline-terminated path */
	p = memchr(input, '\n', count);
	if (p == NULL)
		return -EINVAL;
	*p = '\0';

	/*
	 * Scan for path and, optionally, an offset and length
	 * of a layout segment to be recalled; if there are two
	 * fields, they're assumed to be path and offset.
	 */
	p = input;
	path = strsep(&p, " ");
	if (path == NULL)
		return -EINVAL;

	str = strsep(&p, " ");
	if (str != NULL) {
		rc = strict_strtoull(str, 10, &off);
		if (rc != 0)
			return -EINVAL;

		str = strsep(&p, " ");
		if (str != NULL) {
			rc = strict_strtoull(str, 10, &len);
			if (rc != 0)
				return -EINVAL;
		}
	}

	rc = spnfs_test_layoutrecall(path, off, len);
	if (rc != 0)
		return rc;

	return count;
}

static const struct file_operations recall_ops = {
	.write		= recall_write,
};
/*-------------- end recall layout --------------*/


#ifdef CONFIG_SPNFS_LAYOUTSEGMENTS
/*-------------- start layoutseg -------------------------*/
static ssize_t layoutseg_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *offset)
{
	char cmd[3];

	if (copy_from_user(cmd, buf, 1))
		return -EFAULT;
	if (cmd[0] == '0')
		spnfs_use_layoutsegments = 0;
	else
		spnfs_use_layoutsegments = 1;

	return count;
}

static const struct file_operations layoutseg_ops = {
	.write		= layoutseg_write,
};
/*-------------- end layoutseg ---------------------------*/

/*-------------- start layoutsegsize -------------------------*/
static ssize_t layoutsegsize_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *offset)
{
	char cmd[50];

	if (copy_from_user(cmd, buf, 49))
		return -EFAULT;
	layoutsegment_size = simple_strtoull(cmd, NULL, 10);

	return count;
}

static const struct file_operations layoutsegsize_ops = {
	.write		= layoutsegsize_write,
};
/*-------------- end layoutsegsize ---------------------------*/
#endif /* CONFIG_SPNFS_LAYOUTSEGMENTS */

int
spnfs_init_proc(void)
{
	struct proc_dir_entry *entry;

	entry = proc_mkdir("fs/spnfs", NULL);
	if (!entry)
		return -ENOMEM;

	entry = create_proc_entry("fs/spnfs/ctl", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &ctl_ops;

	entry = create_proc_entry("fs/spnfs/config", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &config_ops;

	entry = create_proc_entry("fs/spnfs/getfh", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &getfh_ops;

	entry = create_proc_entry("fs/spnfs/recall", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &recall_ops;

#ifdef CONFIG_SPNFS_LAYOUTSEGMENTS
	entry = create_proc_entry("fs/spnfs/layoutseg", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &layoutseg_ops;

	entry = create_proc_entry("fs/spnfs/layoutsegsize", 0, NULL);
	if (!entry)
		return -ENOMEM;
	entry->proc_fops = &layoutsegsize_ops;
#endif /* CONFIG_SPNFS_LAYOUTSEGMENTS */

	return 0;
}
#endif /* CONFIG_PROC_FS */
