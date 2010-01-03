/*
 *  objlayout.h
 *
 *  Data types and function declerations for interfacing with the
 *  pNFS standard object layout driver.
 *
 *  Copyright (C) 2007-2009 Panasas Inc.
 *  All rights reserved.
 *
 *  Benny Halevy <bhalevy@panasas.com>
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

#ifndef _OBJLAYOUT_H
#define _OBJLAYOUT_H

#include <linux/nfs_fs.h>
#include <linux/nfs4_pnfs.h>
#include <linux/pnfs_osd_xdr.h>

/*
 * in-core layout segment
 */
struct objlayout_segment {
	void *internal;    /* for provider internal use */
	u8 pnfs_osd_layout[];
};

/*
 * per-inode layout
 */
struct objlayout {
	 /* for layout_return */
	spinlock_t lock;
	struct list_head err_list;
};

/*
 * per-I/O operation state
 * embedded in objects provider io_state data structure
 */
struct objlayout_io_state {
	struct pnfs_layout_segment *lseg;

	struct page **pages;
	unsigned pgbase;
	unsigned nr_pages;
	unsigned long count;
	loff_t offset;
	bool sync;

	void *rpcdata;
	int status;             /* res */
	int eof;                /* res */
	int committed;          /* res */

	/* Error reporting (layout_return) */
	struct list_head err_list;
	unsigned num_comps;
	/* Pointer to array of error descriptors of size num_comps.
	 * It should contain as many entries as devices in the osd_layout
	 * that participate in the I/O. It is up to the io_engine to allocate
	 * needed space and set num_comps.
	 */
	struct pnfs_osd_ioerr *ioerrs;
};

/*
 * Raid engine I/O API
 */
extern void *objio_init_mt(void);
extern void objio_fini_mt(void *mt);

extern int objio_alloc_lseg(void **outp,
	struct pnfs_layout_type *pnfslay,
	struct pnfs_layout_segment *lseg,
	struct pnfs_osd_layout *layout);
extern void objio_free_lseg(void *p);

extern int objio_alloc_io_state(void *seg, struct objlayout_io_state **outp);
extern void objio_free_io_state(struct objlayout_io_state *state);

extern ssize_t objio_read_pagelist(struct objlayout_io_state *ol_state);
extern ssize_t objio_write_pagelist(struct objlayout_io_state *ol_state,
				    bool stable);

/*
 * callback API
 */
extern void objlayout_io_set_result(struct objlayout_io_state *state,
				    unsigned index, int osd_error,
				    u64 offset, u64 length, bool is_write);

extern void objlayout_read_done(struct objlayout_io_state *state,
				ssize_t status, bool sync);
extern void objlayout_write_done(struct objlayout_io_state *state,
				 ssize_t status, bool sync);

extern int objlayout_get_deviceinfo(struct pnfs_layout_type *pnfslay,
	struct pnfs_deviceid *d_id, struct pnfs_osd_deviceaddr **deviceaddr);
extern void objlayout_put_deviceinfo(struct pnfs_osd_deviceaddr *deviceaddr);

/*
 * exported generic objects function vectors
 */
extern struct layoutdriver_io_operations objlayout_io_operations;
extern struct pnfs_client_operations *pnfs_client_ops;

#endif /* _OBJLAYOUT_H */
