/*
 *  Copyright (c) 2008 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  David M. Richter <richterd@citi.umich.edu>
 *
 *  Drawing on work done by Andy Adamson <andros@citi.umich.edu> and
 *  Marius Eriksen <marius@monkey.org>.  Thanks for the help over the
 *  years, guys.
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
 *  With thanks to CITI's project sponsor and partner, IBM.
 */

#ifndef _SIMPLE_RPC_PIPEFS_H_
#define _SIMPLE_RPC_PIPEFS_H_

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/rpc_pipe_fs.h>


#define payload_of(headerp)  ((void *)(headerp + 1))

/*
 * struct pipefs_hdr -- the generic message format for simple_rpc_pipefs.
 * Messages may simply be the header itself, although having an optional
 * data payload follow the header allows much more flexibility.
 *
 * Messages are created using pipefs_alloc_init_msg() and
 * pipefs_alloc_init_msg_padded(), both of which accept a pointer to an
 * (optional) data payload.
 *
 * Given a struct pipefs_hdr *msg that has a struct foo payload, the data
 * can be accessed using: struct foo *foop = payload_of(msg)
 */
struct pipefs_hdr {
	u32 msgid;
	u8  type;
	u8  flags;
	u16 totallen; /* length of entire message, including hdr itself */
	u32 status;
};

/*
 * struct pipefs_list -- a type of list used for tracking callers who've made an
 * upcall and are blocked waiting for a reply.
 *
 * See pipefs_queue_upcall_waitreply() and pipefs_assign_upcall_reply().
 */
struct pipefs_list {
	struct list_head list;
	spinlock_t list_lock;
};


/* See net/sunrpc/simple_rpc_pipefs.c for more info on using these functions. */
extern struct dentry *pipefs_mkpipe(const char *name,
				    const struct rpc_pipe_ops *ops,
				    int wait_for_open);
extern void pipefs_closepipe(struct dentry *pipe);
extern void pipefs_init_list(struct pipefs_list *list);
extern struct pipefs_hdr *pipefs_alloc_init_msg(u32 msgid, u8 type, u8 flags,
						void *data, u16 datalen);
extern struct pipefs_hdr *pipefs_alloc_init_msg_padded(u32 msgid, u8 type,
						       u8 flags, void *data,
						       u16 datalen, u16 padlen);
extern struct pipefs_hdr *pipefs_queue_upcall_waitreply(struct dentry *pipe,
							struct pipefs_hdr *msg,
							struct pipefs_list
							*uplist, u8 upflags,
							u32 timeout);
extern int pipefs_queue_upcall_noreply(struct dentry *pipe,
				       struct pipefs_hdr *msg, u8 upflags);
extern int pipefs_assign_upcall_reply(struct pipefs_hdr *reply,
				      struct pipefs_list *uplist);
extern struct pipefs_hdr *pipefs_readmsg(struct file *filp,
					 const char __user *src, size_t len);
extern ssize_t pipefs_generic_upcall(struct file *filp,
				     struct rpc_pipe_msg *rpcmsg,
				     char __user *dst, size_t buflen);
extern void pipefs_generic_destroy_msg(struct rpc_pipe_msg *rpcmsg);

#endif /* _SIMPLE_RPC_PIPEFS_H_ */
