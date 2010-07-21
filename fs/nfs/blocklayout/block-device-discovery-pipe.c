#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include "blocklayout.h"

#define NFSDBG_FACILITY NFSDBG_PNFS_LD

struct pipefs_list bl_device_list;
struct dentry *bl_device_pipe;

ssize_t bl_pipe_downcall(struct file *filp, const char __user *src, size_t len)
{
	int err;
	struct pipefs_hdr *msg;

	dprintk("Entering %s...\n", __func__);

	msg = pipefs_readmsg(filp, src, len);
	if (IS_ERR(msg)) {
		dprintk("ERROR: unable to read pipefs message.\n");
		return PTR_ERR(msg);
	}

	/* now assign the result, which wakes the blocked thread */
	err = pipefs_assign_upcall_reply(msg, &bl_device_list);
	if (err) {
		dprintk("ERROR: failed to assign upcall with id %u\n",
			msg->msgid);
		kfree(msg);
	}
	return len;
}

static const struct rpc_pipe_ops bl_pipe_ops = {
	.upcall         = pipefs_generic_upcall,
	.downcall       = bl_pipe_downcall,
	.destroy_msg    = pipefs_generic_destroy_msg,
};

int bl_pipe_init(void)
{
	dprintk("%s: block_device pipefs registering...\n", __func__);
	bl_device_pipe = pipefs_mkpipe("bl_device_pipe", &bl_pipe_ops, 1);
	if (IS_ERR(bl_device_pipe))
		dprintk("ERROR, unable to make block_device pipe\n");

	if (!bl_device_pipe)
		dprintk("bl_device_pipe is NULL!\n");
	else
	dprintk("bl_device_pipe created!\n");
	pipefs_init_list(&bl_device_list);
	return 0;
}

void bl_pipe_exit(void)
{
	dprintk("%s: block_device pipefs unregistering...\n", __func__);
	if (IS_ERR(bl_device_pipe))
		return ;
	pipefs_closepipe(bl_device_pipe);
	return;
}
