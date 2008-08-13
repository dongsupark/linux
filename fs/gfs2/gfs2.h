/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#ifndef __GFS2_DOT_H__
#define __GFS2_DOT_H__

enum {
	NO_CREATE = 0,
	CREATE = 1,
};

enum {
	NO_FORCE = 0,
	FORCE = 1,
};

#define GFS2_FAST_NAME_SIZE 8

#if defined(CONFIG_PNFSD)
/* XXX: revisit; surely there's a better place for this? */
#define XXX_PNFS_DS_LISTSZ 256
extern char pnfs_ds_list[XXX_PNFS_DS_LISTSZ];

extern int gfs2_pnfs_init_layout_cache(void);
extern void gfs2_pnfs_destroy_layout_cache(void);

#else /* !CONFIG_PNFSD */
#define gfs2_pnfs_init_layout_cache()		0
#define gfs2_pnfs_destroy_layout_cache()	do { } while (0)
#endif /* CONFIG_PNFSD */

#endif /* __GFS2_DOT_H__ */

