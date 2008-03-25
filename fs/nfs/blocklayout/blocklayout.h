/*
 *  linux/fs/nfs/blocklayout/blocklayout.h
 *
 *  Module for the NFSv4.1 pNFS block layout driver.
 *
 *  Copyright (c) 2006 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson <andros@citi.umich.edu>
 *  Fred Isaman <iisaman@umich.edu>
 *
 * permission is granted to use, copy, create derivative works and
 * redistribute this software and such derivative works for any purpose,
 * so long as the name of the university of michigan is not used in
 * any advertising or publicity pertaining to the use or distribution
 * of this software without specific, written prior authorization.  if
 * the above copyright notice or any other identification of the
 * university of michigan is included in any copy of any portion of
 * this software, then the disclaimer below must also be included.
 *
 * this software is provided as is, without representation from the
 * university of michigan as to its fitness for any purpose, and without
 * warranty by the university of michigan of any kind, either express
 * or implied, including without limitation the implied warranties of
 * merchantability and fitness for a particular purpose.  the regents
 * of the university of michigan shall not be liable for any damages,
 * including special, indirect, incidental, or consequential damages,
 * with respect to any claim arising out or in connection with the use
 * of the software, even if it has been or is hereafter advised of the
 * possibility of such damages.
 */
#ifndef FS_NFS_NFS4BLOCKLAYOUT_H
#define FS_NFS_NFS4BLOCKLAYOUT_H

#include <linux/nfs_fs.h>
#include <linux/pnfs_xdr.h> /* Needed by nfs4_pnfs.h */
#include <linux/nfs4_pnfs.h>
#include <linux/dm-ioctl.h> /* Needed for struct dm_ioctl*/

#define PG_pnfserr PG_owner_priv_1
#define PagePnfsErr(page)	test_bit(PG_pnfserr, &(page)->flags)
#define SetPagePnfsErr(page)	set_bit(PG_pnfserr, &(page)->flags)
#define ClearPagePnfsErr(page)	clear_bit(PG_pnfserr, &(page)->flags)

extern struct class shost_class; /* exported from drivers/scsi/hosts.c */
extern int dm_dev_create(struct dm_ioctl *param); /* from dm-ioctl.c */
extern int dm_dev_remove(struct dm_ioctl *param); /* from dm-ioctl.c */
extern int dm_do_resume(struct dm_ioctl *param);
extern int dm_table_load(struct dm_ioctl *param, size_t param_size);

struct block_mount_id {
	struct super_block		*bm_sb;     /* back pointer */
	spinlock_t			bm_lock;    /* protects list */
	struct list_head		bm_devlist; /* holds pnfs_block_dev */
};

struct pnfs_block_dev {
	struct list_head		bm_node;
	char				*bm_mdevname; /* meta device name */
	struct pnfs_deviceid		bm_mdevid;    /* associated devid */
	struct block_device		*bm_mdev;     /* meta device itself */
};

/* holds visible disks that can be matched against VOLUME_SIMPLE signatures */
struct visible_block_device {
	struct list_head	vi_node;
	struct block_device	*vi_bdev;
	int			vi_mapped;
};

enum blk_vol_type {
	PNFS_BLOCK_VOLUME_SIMPLE   = 0,	/* maps to a single LU */
	PNFS_BLOCK_VOLUME_SLICE    = 1,	/* slice of another volume */
	PNFS_BLOCK_VOLUME_CONCAT   = 2,	/* concatenation of multiple volumes */
	PNFS_BLOCK_VOLUME_STRIPE   = 3	/* striped across multiple volumes */
};

/* All disk offset/lengths are stored in 512-byte sectors */
struct pnfs_blk_volume {
	uint32_t		bv_type;
	sector_t 		bv_size;
	struct pnfs_blk_volume 	**bv_vols;
	int 			bv_vol_n;
	union {
		dev_t			bv_dev;
		sector_t		bv_stripe_unit;
		sector_t 		bv_offset;
	};
};

/* Since components need not be aligned, cannot use sector_t */
struct pnfs_blk_sig_comp {
	int64_t 	bs_offset;  /* In bytes */
	uint32_t   	bs_length;  /* In bytes */
	char 		*bs_string;
};

/* Maximum number of signatures components in a simple volume */
# define PNFS_BLOCK_MAX_SIG_COMP 16

struct pnfs_blk_sig {
	int 				si_num_comps;
	struct pnfs_blk_sig_comp	si_comps[PNFS_BLOCK_MAX_SIG_COMP];
};

enum exstate4 {
	PNFS_BLOCK_READWRITE_DATA	= 0,
	PNFS_BLOCK_READ_DATA		= 1,
	PNFS_BLOCK_INVALID_DATA		= 2, /* mapped, but data is invalid */
	PNFS_BLOCK_NONE_DATA		= 3  /* unmapped, it's a hole */
};

struct pnfs_inval_markings {
	/* STUB */
};

/* sector_t fields are all in 512-byte sectors */
struct pnfs_block_extent {
	struct kref	be_refcnt;
	struct list_head be_node;	/* link into lseg list */
	struct pnfs_deviceid be_devid;  /* STUB - remevable??? */
	struct block_device *be_mdev;
	sector_t	be_f_offset;	/* the starting offset in the file */
	sector_t	be_length;	/* the size of the extent */
	sector_t	be_v_offset;	/* the starting offset in the volume */
	enum exstate4	be_state;	/* the state of this extent */
	struct pnfs_inval_markings *be_inval; /* tracks INVAL->RW transition */
};

static inline void
INIT_INVAL_MARKS(struct pnfs_inval_markings *marks, sector_t blocksize)
{
	/* STUB */
}

enum extentclass4 {
	RW_EXTENT	= 0, /* READWRTE and INVAL */
	RO_EXTENT	= 1, /* READ and NONE */
	EXTENT_LISTS	= 2,
};

static inline int choose_list(enum exstate4 state)
{
	if (state == PNFS_BLOCK_READ_DATA || state == PNFS_BLOCK_NONE_DATA)
		return RO_EXTENT;
	else
		return RW_EXTENT;
}

struct pnfs_block_layout {
	struct pnfs_inval_markings bl_inval; /* tracks INVAL->RW transition */
	spinlock_t		bl_ext_lock;   /* Protects list manipulation */
	struct list_head	bl_extents[EXTENT_LISTS]; /* R and RW extents */
	sector_t		bl_blocksize;  /* Server blocksize in sectors */
};

#define BLK_ID(lo)     ((struct block_mount_id *)(PNFS_MOUNTID(lo)->mountid))
#define BLK_LSEG2EXT(lseg) ((struct pnfs_block_layout *)lseg->layout->ld_data)
#define BLK_LO2EXT(lo) ((struct pnfs_block_layout *)lo->ld_data)

uint32_t *blk_overflow(uint32_t *p, uint32_t *end, size_t nbytes);

#define BLK_READBUF(p, e, nbytes)  do { \
	p = blk_overflow(p, e, nbytes); \
	if (!p) { \
		printk(KERN_WARNING \
			"%s: reply buffer overflowed in line %d.\n", \
			__func__, __LINE__); \
		goto out_err; \
	} \
} while (0)

#define READ32(x)         (x) = ntohl(*p++)
#define READ64(x)         do {                  \
	(x) = (uint64_t)ntohl(*p++) << 32;           \
	(x) |= ntohl(*p++);                     \
} while (0)
#define COPYMEM(x, nbytes) do {                 \
	memcpy((x), p, nbytes);                 \
	p += XDR_QUADLEN(nbytes);               \
} while (0)
#define READ_DEVID(x)	COPYMEM((x)->data, NFS4_PNFS_DEVICEID4_SIZE)
#define READ_SECTOR(x)     do { \
	READ64(tmp); \
	if (tmp & 0x1ff) { \
		printk(KERN_WARNING \
		       "%s Value not 512-byte aligned at line %d\n", \
		       __func__, __LINE__);			     \
		goto out_err; \
	} \
	(x) = tmp >> 9; \
} while (0)

/* blocklayoutdev.c */
struct block_device *nfs4_blkdev_get(dev_t dev);
int nfs4_blkdev_put(struct block_device *bdev);
struct pnfs_block_dev *nfs4_blk_decode_device(struct super_block *sb,
					      struct pnfs_device *dev,
					      struct list_head *sdlist);
int nfs4_blk_process_layoutget(struct pnfs_block_layout *bl,
			       struct nfs4_pnfs_layoutget_res *lgr);
int nfs4_blk_create_scsi_disk_list(struct list_head *);
void nfs4_blk_destroy_disk_list(struct list_head *);
/* blocklayoutdm.c */
struct pnfs_block_dev *nfs4_blk_init_metadev(struct super_block *sb,
					     struct pnfs_device *dev);
int nfs4_blk_flatten(struct pnfs_blk_volume *, int, struct pnfs_block_dev *);
void free_block_dev(struct pnfs_block_dev *bdev);
/* extents.c */
struct pnfs_block_extent *
find_get_extent(struct pnfs_block_layout *bl, sector_t isect,
		struct pnfs_block_extent **cow_read);
void put_extent(struct pnfs_block_extent *be);
struct pnfs_block_extent *alloc_extent(void);
int is_sector_initialized(struct pnfs_inval_markings *marks, sector_t isect);
int add_and_merge_extent(struct pnfs_block_layout *bl,
			 struct pnfs_block_extent *new);
#endif /* FS_NFS_NFS4BLOCKLAYOUT_H */
