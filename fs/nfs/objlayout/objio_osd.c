/*
 *  objio_osd.c
 *
 *  pNFS Objects layout implementation over open-osd initiator library
 *
 *  Copyright (C) 2009 Panasas Inc.
 *  All rights reserved.
 *
 *  Benny Halevy <bharrosh@panasas.com>
 *  Boaz Harrosh <bharrosh@panasas.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  See the file COPYING included with this distribution for more details.
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
 *  3. Neither the name of the Panasas company nor the names of its
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

#include <linux/module.h>
#include <scsi/scsi_device.h>
#include <scsi/osd_attributes.h>
#include <scsi/osd_initiator.h>
#include <scsi/osd_sec.h>
#include <scsi/osd_sense.h>

#include "objlayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS

#define _LLU(x) ((unsigned long long)x)

enum { BIO_MAX_PAGES_KMALLOC =
		(PAGE_SIZE - sizeof(struct bio)) / sizeof(struct bio_vec),
};

/* A per mountpoint struct currently for device cache */
struct objio_mount_type {
	struct list_head dev_list;
	spinlock_t dev_list_lock;
};

struct objio_segment {
	struct pnfs_osd_layout *layout;
};

struct objio_state {
	/* Generic layer */
	struct objlayout_io_state ol_state;

	struct objio_segment *objio_seg;
};

int objio_alloc_lseg(void **outp,
	struct pnfs_layout_type *pnfslay,
	struct pnfs_layout_segment *lseg,
	struct pnfs_osd_layout *layout)
{
	struct objio_segment *objio_seg;

	objio_seg = kzalloc(sizeof(*objio_seg), GFP_KERNEL);
	if (!objio_seg)
		return -ENOMEM;

	objio_seg->layout = layout;

	*outp = objio_seg;
	return 0;
}

void objio_free_lseg(void *p)
{
	struct objio_segment *objio_seg = p;

	kfree(objio_seg);
}

int objio_alloc_io_state(void *seg, struct objlayout_io_state **outp)
{
	struct objio_segment *objio_seg = seg;
	struct objio_state *ios;

	ios = kzalloc(sizeof(*ios), GFP_KERNEL);
	if (unlikely(!ios))
		return -ENOMEM;

	ios->objio_seg = objio_seg;

	*outp = &ios->ol_state;
	return 0;
}

void objio_free_io_state(struct objlayout_io_state *ol_state)
{
	struct objio_state *ios = container_of(ol_state, struct objio_state,
					       ol_state);

	kfree(ios);
}

/*
 * read
 */
ssize_t objio_read_pagelist(struct objlayout_io_state *ol_state)
{
	return -EIO;
}

/*
 * write
 */
ssize_t objio_write_pagelist(struct objlayout_io_state *ol_state, bool stable)
{
	return -EIO;
}

/*
 * Policy Operations
 */

/*
 * Return the stripe size for the specified file
 */
ssize_t
objlayout_get_stripesize(struct pnfs_layout_type *pnfslay)
{
	ssize_t sz, maxsz = -1;
	struct pnfs_layout_segment *lseg;

	list_for_each_entry(lseg, &pnfslay->segs, fi_list) {
		int n;
		struct objlayout_segment *objlseg = LSEG_LD_DATA(lseg);
		struct pnfs_osd_layout *lo =
			(struct pnfs_osd_layout *)objlseg->pnfs_osd_layout;
		struct pnfs_osd_data_map *map = &lo->olo_map;

		n = map->odm_group_width;
		if (n == 0)
			n = map->odm_num_comps / (map->odm_mirror_cnt + 1);

		switch (map->odm_raid_algorithm) {
		case PNFS_OSD_RAID_0:
			break;

		case PNFS_OSD_RAID_4:
		case PNFS_OSD_RAID_5:
			n -= 1;
			break;

		case PNFS_OSD_RAID_PQ:
			n -= 2;
			break;

		default:
			BUG_ON(1);
		}
		sz = map->odm_stripe_unit * n;
		if (sz > maxsz)
			maxsz = sz;
	}
	dprintk("%s: Return %Zx\n", __func__, maxsz);
	return maxsz;
}

/*
 * Get the max [rw]size
 */
static ssize_t
objlayout_get_blocksize(struct pnfs_mount_type *mountid)
{
	ssize_t sz = BIO_MAX_PAGES_KMALLOC * PAGE_SIZE;

	return sz;
}

/*
 * Get the I/O threshold
 */
static ssize_t
objlayout_get_io_threshold(struct pnfs_layout_type *layoutid,
			   struct inode *inode)
{
	ssize_t sz = -1;
	return sz;
}

static struct layoutdriver_policy_operations objlayout_policy_operations = {
/*
 * Don't gather across stripes, but rather gather (coalesce) up to
 * the stripe size.
 *
 * FIXME: change interface to use merge_align, merge_count
 */
	.flags                 = PNFS_LAYOUTGET_ON_OPEN |
				 PNFS_LAYOUTRET_ON_SETATTR,
	.get_stripesize        = objlayout_get_stripesize,
	.get_blocksize         = objlayout_get_blocksize,
	.get_read_threshold    = objlayout_get_io_threshold,
	.get_write_threshold   = objlayout_get_io_threshold,
};

static struct pnfs_layoutdriver_type objlayout_type = {
	.id = LAYOUT_OSD2_OBJECTS,
	.name = "LAYOUT_OSD2_OBJECTS",
	.ld_io_ops = &objlayout_io_operations,
	.ld_policy_ops = &objlayout_policy_operations,
};

void *objio_init_mt(void)
{
	struct objio_mount_type *omt = kzalloc(sizeof(*omt), GFP_KERNEL);

	if (!omt)
		return ERR_PTR(-ENOMEM);

	return omt;
}

void objio_fini_mt(void *mountid)
{
	kfree(mountid);
}

MODULE_DESCRIPTION("pNFS Layout Driver for OSD2 objects");
MODULE_AUTHOR("Benny Halevy <bhalevy@panasas.com>");
MODULE_LICENSE("GPL");

static int __init
objlayout_init(void)
{
	pnfs_client_ops = pnfs_register_layoutdriver(&objlayout_type);
	printk(KERN_INFO "%s: Registered OSD pNFS Layout Driver\n",
	       __func__);
	return 0;
}

static void __exit
objlayout_exit(void)
{
	pnfs_unregister_layoutdriver(&objlayout_type);
	printk(KERN_INFO "%s: Unregistered OSD pNFS Layout Driver\n",
	       __func__);
}

module_init(objlayout_init);
module_exit(objlayout_exit);
