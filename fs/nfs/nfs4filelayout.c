/*
 *  linux/fs/nfs/nfs4filelayout.c
 *
 *  Module for the pnfs nfs4 file layout driver.
 *  Defines all I/O and Policy interface operations, plus code
 *  to register itself with the pNFS client.
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

#ifdef CONFIG_PNFS

#include <linux/module.h>
#include <linux/init.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/nfs4_pnfs.h>

#include "nfs4filelayout.h"
#include "nfs4_fs.h"
#include "internal.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dean Hildebrand <dhildebz@eecs.umich.edu>");
MODULE_DESCRIPTION("The NFSv4 file layout driver");

/* Callback operations to the pNFS client */
struct pnfs_client_operations *pnfs_callback_ops;

/* Initialize a mountpoint by retrieving the list of
 * available devices for it.
 * Return the pnfs_mount_type structure so the
 * pNFS_client can refer to the mount point later on
 */
struct pnfs_mount_type*
filelayout_initialize_mountpoint(struct super_block *sb, struct nfs_fh *fh)
{
	struct filelayout_mount_type *fl_mt;
	struct pnfs_mount_type *mt;

	fl_mt = kmalloc(sizeof(struct filelayout_mount_type), GFP_KERNEL);
	if (!fl_mt)
		goto error_ret;

	mt = kmalloc(sizeof(struct pnfs_mount_type), GFP_KERNEL);
	if (!mt)
		goto cleanup_fl_mt;

	fl_mt->fl_sb = sb;
	mt->mountid = (void *)fl_mt;

	return mt;

cleanup_fl_mt: ;
	kfree(fl_mt);

error_ret: ;
	return NULL;
}

/* Uninitialize a mountpoint by destroying its device list.
 */
int
filelayout_uninitialize_mountpoint(struct pnfs_mount_type *mountid)
{
	struct filelayout_mount_type *fl_mt = NULL;

	if (mountid) {
		fl_mt = (struct filelayout_mount_type *)mountid->mountid;

		if (fl_mt != NULL)
			kfree(fl_mt);
		kfree(mountid);
	}
	return 0;
}

struct layoutdriver_io_operations filelayout_io_operations = {
	.initialize_mountpoint   = filelayout_initialize_mountpoint,
	.uninitialize_mountpoint = filelayout_uninitialize_mountpoint,
};

struct layoutdriver_policy_operations filelayout_policy_operations = {
};

struct pnfs_layoutdriver_type filelayout_type = {
	.id = LAYOUT_NFSV4_FILES,
	.name = "LAYOUT_NFSV4_FILES",
	.ld_io_ops = &filelayout_io_operations,
	.ld_policy_ops = &filelayout_policy_operations,
};

static int __init nfs4filelayout_init(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Registering...\n",
	       __func__);

	/* Need to register file_operations struct with global list to indicate
	* that NFS4 file layout is a possible pNFS I/O module
	*/
	pnfs_callback_ops = pnfs_register_layoutdriver(&filelayout_type);

	return 0;
}

static void __exit nfs4filelayout_exit(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Unregistering...\n",
	       __func__);

	/* Unregister NFS4 file layout driver with pNFS client*/
	pnfs_unregister_layoutdriver(&filelayout_type);
}

module_init(nfs4filelayout_init);
module_exit(nfs4filelayout_exit);

#endif /* CONFIG_PNFS */
