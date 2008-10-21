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

#include "blocklayout.h"
#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

/* Bit numbers */
#define EXTENT_INITIALIZED 0
#define EXTENT_WRITTEN     1
#define EXTENT_IN_COMMIT   2
#define INTERNAL_EXISTS    MY_MAX_TAGS
#define INTERNAL_MASK      ((1 << INTERNAL_EXISTS) - 1)

struct pnfs_inval_tracking {
	struct list_head it_link;
	int		 it_sector;
	int		 it_tags;
};

/* Returns largest t<=s s.t. t%base==0 */
static inline sector_t normalize(sector_t s, int base)
{
	sector_t tmp = s; /* Since do_div modifies its argument */
	return s - do_div(tmp, base);
}

static inline sector_t normalize_up(sector_t s, int base)
{
	return normalize(s + base - 1, base);
}

/* Complete stub using list while determine API wanted */

/* Returns tags, or negative */
static int32_t _find_entry(struct my_tree_t *tree, u64 s)
{
	struct pnfs_inval_tracking *pos;

	dprintk("%s(%llu) enter\n", __func__, s);
	list_for_each_entry(pos, &tree->mtt_stub, it_link) {
		if (pos->it_sector < s)
			continue;
		else if (pos->it_sector == s)
			return pos->it_tags & INTERNAL_MASK;
		else
			break;
	}
	return -ENOENT;
}

static inline
int _has_tag(struct my_tree_t *tree, u64 s, int32_t tag)
{
	int32_t tags;

	dprintk("%s(%llu, %i) enter\n", __func__, s, tag);
	s = normalize(s, tree->mtt_step_size);
	tags = _find_entry(tree, s);
	if ((tags < 0) || !(tags & (1 << tag)))
		return 0;
	else
		return 1;
}

/* Creates entry with tag, or if entry already exists, unions tag to it.
 * If storage is not NULL, newly created entry will use it.
 * Returns number of entries added, or negative on error.
 */
static int _add_entry(struct my_tree_t *tree, u64 s, int32_t tag,
		      struct pnfs_inval_tracking *storage)
{
	int found = 0;
	struct pnfs_inval_tracking *pos;

	dprintk("%s(%llu, %i, %p) enter\n", __func__, s, tag, storage);
	list_for_each_entry(pos, &tree->mtt_stub, it_link) {
		if (pos->it_sector < s)
			continue;
		else if (pos->it_sector == s) {
			found = 1;
			break;
		} else
			break;
	}
	if (found) {
		pos->it_tags |= (1 << tag);
		return 0;
	} else {
		struct pnfs_inval_tracking *new;
		if (storage)
			new = storage;
		else {
			new = kmalloc(sizeof(*new), GFP_KERNEL);
			if (!new)
				return -ENOMEM;
		}
		new->it_sector = s;
		new->it_tags = (1 << tag);
		list_add_tail(&new->it_link, &pos->it_link);
		return 1;
	}
}

/* XXXX Really want option to not create */
/* Over range, unions tag with existing entries, else creates entry with tag */
static int _set_range(struct my_tree_t *tree, int32_t tag, u64 s, u64 length)
{
	u64 i;

	dprintk("%s(%i, %llu, %llu) enter\n", __func__, tag, s, length);
	for (i = normalize(s, tree->mtt_step_size); i < s + length;
	     i += tree->mtt_step_size)
		if (_add_entry(tree, i, tag, NULL))
			return -ENOMEM;
	return 0;
}

/* Ensure that future operations on given range of tree will not malloc */
static int _preload_range(struct my_tree_t *tree, u64 offset, u64 length)
{
	u64 start, end, s;
	int count, i, used = 0, status = -ENOMEM;
	struct pnfs_inval_tracking **storage;

	dprintk("%s(%llu, %llu) enter\n", __func__, offset, length);
	start = normalize(offset, tree->mtt_step_size);
	end = normalize_up(offset + length, tree->mtt_step_size);
	count = (int)(end - start) / (int)tree->mtt_step_size;

	/* Pre-malloc what memory we might need */
	storage = kmalloc(sizeof(*storage) * count, GFP_KERNEL);
	if (!storage)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		storage[i] = kmalloc(sizeof(struct pnfs_inval_tracking),
				     GFP_KERNEL);
		if (!storage[i])
			goto out_cleanup;
	}

	/* Now need lock - HOW??? */

	for (s = start; s < end; s += tree->mtt_step_size)
		used += _add_entry(tree, s, INTERNAL_EXISTS, storage[used]);

	/* Unlock - HOW??? */
	status = 0;

 out_cleanup:
	for (i = used; i < count; i++) {
		if (!storage[i])
			break;
		kfree(storage[i]);
	}
	return status;
}

static void set_needs_init(sector_t *array, sector_t offset)
{
	sector_t *p = array;

	dprintk("%s enter\n", __func__);
	if (!p)
		return;
	while (*p < offset)
		p++;
	if (*p == offset)
		return;
	else if (*p == ~0) {
		*p++ = offset;
		*p = ~0;
		return;
	} else {
		sector_t *save = p;
		dprintk("%s Adding %llu\n", __func__, (u64)offset);
		while (*p != ~0)
			p++;
		p++;
		memmove(save + 1, save, (char *)p - (char *)save);
		*save = offset;
		return;
	}
}

/* We are relying on page lock to serialize this */
int is_sector_initialized(struct pnfs_inval_markings *marks, sector_t isect)
{
	int rv;

	spin_lock(&marks->im_lock);
	rv = _has_tag(&marks->im_tree, isect, EXTENT_INITIALIZED);
	spin_unlock(&marks->im_lock);
	return rv;
}

/* Assume start, end already sector aligned */
static int
_range_has_tag(struct my_tree_t *tree, u64 start, u64 end, int32_t tag)
{
	struct pnfs_inval_tracking *pos;
	u64 expect = 0;

	dprintk("%s(%llu, %llu, %i) enter\n", __func__, start, end, tag);
	list_for_each_entry(pos, &tree->mtt_stub, it_link) {
		if (pos->it_sector < start)
			continue;
		if (!expect) {
			if ((pos->it_sector == start) &&
			    (pos->it_tags & (1 << tag))) {
				expect = start + tree->mtt_step_size;
				if (expect == end)
					return 1;
				continue;
			} else {
				return 0;
			}
		}
		if (pos->it_sector != expect || !(pos->it_tags & (1 << tag)))
			return 0;
		expect += tree->mtt_step_size;
		if (expect == end)
			return 1;
	}
	return 0;
}

static int is_range_written(struct pnfs_inval_markings *marks,
			    sector_t start, sector_t end)
{
	int rv;

	spin_lock(&marks->im_lock);
	rv = _range_has_tag(&marks->im_tree, start, end, EXTENT_WRITTEN);
	spin_unlock(&marks->im_lock);
	return rv;
}

/* Marks sectors in [offest, offset_length) as having been initialized.
 * All lengths are step-aligned, where step is min(pagesize, blocksize).
 * Notes where partial block is initialized, and helps prepare it for
 * complete initialization later.
 */
/* Currently assumes offset is page-aligned */
int mark_initialized_sectors(struct pnfs_inval_markings *marks,
			     sector_t offset, sector_t length,
			     sector_t **pages)
{
	sector_t s, start, end;
	sector_t *array = NULL; /* Pages to mark */

	dprintk("%s(offset=%llu,len=%llu) enter\n",
		__func__, (u64)offset, (u64)length);
	s = max((sector_t) 3,
		2 * (marks->im_block_size / (PAGE_CACHE_SECTORS)));
	dprintk("%s set max=%llu\n", __func__, (u64)s);
	if (pages) {
		array = kmalloc(s * sizeof(sector_t), GFP_KERNEL);
		if (!array)
			goto outerr;
		array[0] = ~0;
	}

	start = normalize(offset, marks->im_block_size);
	end = normalize_up(offset + length, marks->im_block_size);
	if (_preload_range(&marks->im_tree, start, end - start))
		goto outerr;

	spin_lock(&marks->im_lock);

	for (s = normalize_up(start, PAGE_CACHE_SECTORS);
	     s < offset; s += PAGE_CACHE_SECTORS) {
		dprintk("%s pre-area pages\n", __func__);
		/* Portion of used block is not initialized */
		if (!_has_tag(&marks->im_tree, s, EXTENT_INITIALIZED))
			set_needs_init(array, s);
	}
	if (_set_range(&marks->im_tree, EXTENT_INITIALIZED, offset, length))
		goto out_unlock;
	for (s = normalize_up(offset + length, PAGE_CACHE_SECTORS);
	     s < end; s += PAGE_CACHE_SECTORS) {
		dprintk("%s post-area pages\n", __func__);
		if (!_has_tag(&marks->im_tree, s, EXTENT_INITIALIZED))
			set_needs_init(array, s);
	}

	spin_unlock(&marks->im_lock);

	if (pages) {
		if (array[0] == ~0) {
			kfree(array);
			*pages = NULL;
		} else
			*pages = array;
	}
	return 0;

 out_unlock:
	spin_unlock(&marks->im_lock);
 outerr:
	if (pages) {
		kfree(array);
		*pages = NULL;
	}
	return -ENOMEM;
}

/* Marks sectors in [offest, offset+length) as having been written to disk.
 * All lengths should be block aligned.
 */
int mark_written_sectors(struct pnfs_inval_markings *marks,
			 sector_t offset, sector_t length)
{
	int status;

	dprintk("%s(offset=%llu,len=%llu) enter\n", __func__,
		(u64)offset, (u64)length);
	spin_lock(&marks->im_lock);
	status = _set_range(&marks->im_tree, EXTENT_WRITTEN, offset, length);
	spin_unlock(&marks->im_lock);
	return status;
}

static void print_short_extent(struct pnfs_block_short_extent *be)
{
	dprintk("PRINT SHORT EXTENT extent %p\n", be);
	if (be) {
		dprintk("        be_f_offset %llu\n", (u64)be->bse_f_offset);
		dprintk("        be_length   %llu\n", (u64)be->bse_length);
	}
}

void print_clist(struct list_head *list, unsigned int count)
{
	struct pnfs_block_short_extent *be;
	unsigned int i = 0;

	dprintk("****************\n");
	dprintk("Extent list looks like:\n");
	list_for_each_entry(be, list, bse_node) {
		i++;
		print_short_extent(be);
	}
	if (i != count)
		dprintk("\n\nExpected %u entries\n\n\n", count);
	dprintk("****************\n");
}

/* Note: In theory, we should do more checking that devid's match between
 * old and new, but if they don't, the lists are too corrupt to salvage anyway.
 */
/* Note this is very similar to add_and_merge_extent */
static void add_to_commitlist(struct pnfs_block_layout *bl,
			      struct pnfs_block_short_extent *new)
{
	struct list_head *clist = &bl->bl_commit;
	struct pnfs_block_short_extent *old, *save;
	sector_t end = new->bse_f_offset + new->bse_length;

	dprintk("%s enter\n", __func__);
	print_short_extent(new);
	print_clist(clist, bl->bl_count);
	bl->bl_count++;
	/* Scan for proper place to insert, extending new to the left
	 * as much as possible.
	 */
	list_for_each_entry_safe(old, save, clist, bse_node) {
		if (new->bse_f_offset < old->bse_f_offset)
			break;
		if (end <= old->bse_f_offset + old->bse_length) {
			/* Range is already in list */
			bl->bl_count--;
			kfree(new);
			return;
		} else if (new->bse_f_offset <=
				old->bse_f_offset + old->bse_length) {
			/* new overlaps or abuts existing be */
			if (new->bse_mdev == old->bse_mdev) {
				/* extend new to fully replace old */
				new->bse_length += new->bse_f_offset -
						old->bse_f_offset;
				new->bse_f_offset = old->bse_f_offset;
				list_del(&old->bse_node);
				bl->bl_count--;
				kfree(old);
			}
		}
	}
	/* Note that if we never hit the above break, old will not point to a
	 * valid extent.  However, in that case &old->bse_node==list.
	 */
	list_add_tail(&new->bse_node, &old->bse_node);
	/* Scan forward for overlaps.  If we find any, extend new and
	 * remove the overlapped extent.
	 */
	old = list_prepare_entry(new, clist, bse_node);
	list_for_each_entry_safe_continue(old, save, clist, bse_node) {
		if (end < old->bse_f_offset)
			break;
		/* new overlaps or abuts old */
		if (new->bse_mdev == old->bse_mdev) {
			if (end < old->bse_f_offset + old->bse_length) {
				/* extend new to fully cover old */
				end = old->bse_f_offset + old->bse_length;
				new->bse_length = end - new->bse_f_offset;
			}
			list_del(&old->bse_node);
			bl->bl_count--;
			kfree(old);
		}
	}
	dprintk("%s: after merging\n", __func__);
	print_clist(clist, bl->bl_count);
}

/* Note the range described by offset, length is guaranteed to be contained
 * within be.
 */
int mark_for_commit(struct pnfs_block_extent *be,
		    sector_t offset, sector_t length)
{
	sector_t new_end, end = offset + length;
	struct pnfs_block_short_extent *new;
	struct pnfs_block_layout *bl = container_of(be->be_inval,
						    struct pnfs_block_layout,
						    bl_inval);

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	mark_written_sectors(be->be_inval, offset, length);
	/* We want to add the range to commit list, but it must be
	 * block-normalized, and verified that the normalized range has
	 * been entirely written to disk.
	 */
	new->bse_f_offset = offset;
	offset = normalize(offset, bl->bl_blocksize);
	if (offset < new->bse_f_offset) {
		if (is_range_written(be->be_inval, offset, new->bse_f_offset))
			new->bse_f_offset = offset;
		else
			new->bse_f_offset = offset + bl->bl_blocksize;
	}
	new_end = normalize_up(end, bl->bl_blocksize);
	if (end < new_end) {
		if (is_range_written(be->be_inval, end, new_end))
			end = new_end;
		else
			end = new_end - bl->bl_blocksize;
	}
	if (end <= new->bse_f_offset) {
		kfree(new);
		return 0;
	}
	new->bse_length = end - new->bse_f_offset;
	new->bse_devid = be->be_devid;
	new->bse_mdev = be->be_mdev;

	spin_lock(&bl->bl_ext_lock);
	/* new will be freed, either by add_to_commitlist if it decides not
	 * to use it, or after LAYOUTCOMMIT uses it in the commitlist.
	 */
	add_to_commitlist(bl, new);
	spin_unlock(&bl->bl_ext_lock);
	return 0;
}

static void print_bl_extent(struct pnfs_block_extent *be)
{
	dprintk("PRINT EXTENT extent %p\n", be);
	if (be) {
		dprintk("        be_f_offset %llu\n", (u64)be->be_f_offset);
		dprintk("        be_length   %llu\n", (u64)be->be_length);
		dprintk("        be_v_offset %llu\n", (u64)be->be_v_offset);
		dprintk("        be_state    %d\n", be->be_state);
	}
}

static void
destroy_extent(struct kref *kref)
{
	struct pnfs_block_extent *be;

	be = container_of(kref, struct pnfs_block_extent, be_refcnt);
	dprintk("%s be=%p\n", __func__, be);
	kfree(be);
}

void
put_extent(struct pnfs_block_extent *be)
{
	if (be) {
		dprintk("%s enter %p (%i)\n", __func__, be,
			atomic_read(&be->be_refcnt.refcount));
		kref_put(&be->be_refcnt, destroy_extent);
	}
}

struct pnfs_block_extent *alloc_extent(void)
{
	struct pnfs_block_extent *be;

	be = kmalloc(sizeof(struct pnfs_block_extent), GFP_KERNEL);
	if (!be)
		return NULL;
	INIT_LIST_HEAD(&be->be_node);
	kref_init(&be->be_refcnt);
	be->be_inval = NULL;
	return be;
}

struct pnfs_block_extent *
get_extent(struct pnfs_block_extent *be)
{
	if (be)
		kref_get(&be->be_refcnt);
	return be;
}

void print_elist(struct list_head *list)
{
	struct pnfs_block_extent *be;
	dprintk("****************\n");
	dprintk("Extent list looks like:\n");
	list_for_each_entry(be, list, be_node) {
		print_bl_extent(be);
	}
	dprintk("****************\n");
}

static inline int
extents_consistent(struct pnfs_block_extent *old, struct pnfs_block_extent *new)
{
	/* Note this assumes new->be_f_offset >= old->be_f_offset */
	return (new->be_state == old->be_state) &&
		((new->be_state == PNFS_BLOCK_NONE_DATA) ||
		 ((new->be_v_offset - old->be_v_offset ==
		   new->be_f_offset - old->be_f_offset) &&
		  new->be_mdev == old->be_mdev));
}

/* Adds new to appropriate list in bl, modifying new and removing existing
 * extents as appropriate to deal with overlaps.
 *
 * See find_get_extent for list constraints.
 *
 * Refcount on new is already set.  If end up not using it, or error out,
 * need to put the reference.
 *
 * Lock is held by caller.
 */
int
add_and_merge_extent(struct pnfs_block_layout *bl,
		     struct pnfs_block_extent *new)
{
	struct pnfs_block_extent *be, *tmp;
	sector_t end = new->be_f_offset + new->be_length;
	struct list_head *list;

	dprintk("%s enter with be=%p\n", __func__, new);
	print_bl_extent(new);
	list = &bl->bl_extents[choose_list(new->be_state)];
	print_elist(list);

	/* Scan for proper place to insert, extending new to the left
	 * as much as possible.
	 */
	list_for_each_entry_safe(be, tmp, list, be_node) {
		if (new->be_f_offset < be->be_f_offset)
			break;
		if (end <= be->be_f_offset + be->be_length) {
			/* new is a subset of existing be*/
			if (extents_consistent(be, new)) {
				dprintk("%s: new is subset, ignoring\n",
					__func__);
				put_extent(new);
				return 0;
			} else
				goto out_err;
		} else if (new->be_f_offset <=
				be->be_f_offset + be->be_length) {
			/* new overlaps or abuts existing be */
			if (extents_consistent(be, new)) {
				/* extend new to fully replace be */
				new->be_length += new->be_f_offset -
						  be->be_f_offset;
				new->be_f_offset = be->be_f_offset;
				new->be_v_offset = be->be_v_offset;
				dprintk("%s: removing %p\n", __func__, be);
				list_del(&be->be_node);
				put_extent(be);
			} else if (new->be_f_offset !=
				   be->be_f_offset + be->be_length)
				goto out_err;
		}
	}
	/* Note that if we never hit the above break, be will not point to a
	 * valid extent.  However, in that case &be->be_node==list.
	 */
	list_add_tail(&new->be_node, &be->be_node);
	dprintk("%s: inserting new\n", __func__);
	print_elist(list);
	/* Scan forward for overlaps.  If we find any, extend new and
	 * remove the overlapped extent.
	 */
	be = list_prepare_entry(new, list, be_node);
	list_for_each_entry_safe_continue(be, tmp, list, be_node) {
		if (end < be->be_f_offset)
			break;
		/* new overlaps or abuts existing be */
		if (extents_consistent(be, new)) {
			if (end < be->be_f_offset + be->be_length) {
				/* extend new to fully cover be */
				end = be->be_f_offset + be->be_length;
				new->be_length = end - new->be_f_offset;
			}
			dprintk("%s: removing %p\n", __func__, be);
			list_del(&be->be_node);
			put_extent(be);
		} else if (end != be->be_f_offset)
			goto out_err;
	}
	dprintk("%s: after merging\n", __func__);
	print_elist(list);
	/* STUB - The per-list consistency checks have all been done,
	 * should now check cross-list consistency.
	 */
	return 0;

 out_err:
	put_extent(new);
	return -EIO;
}

/* Returns extent, or NULL.  If a second READ extent exists, it is returned
 * in cow_read, if given.
 *
 * The extents are kept in two seperate ordered lists, one for READ and NONE,
 * one for READWRITE and INVALID.  Within each list, we assume:
 * 1. Extents are ordered by file offset.
 * 2. For any given isect, there is at most one extents that matches.
 */
struct pnfs_block_extent *
find_get_extent(struct pnfs_block_layout *bl, sector_t isect,
	    struct pnfs_block_extent **cow_read)
{
	struct pnfs_block_extent *be, *cow, *ret;
	int i;

	dprintk("%s enter with isect %llu\n", __func__, (u64)isect);
	cow = ret = NULL;
	spin_lock(&bl->bl_ext_lock);
	for (i = 0; i < EXTENT_LISTS; i++) {
		if (ret &&
		    (!cow_read || ret->be_state != PNFS_BLOCK_INVALID_DATA))
			break;
		list_for_each_entry(be, &bl->bl_extents[i], be_node) {
			if (isect < be->be_f_offset)
				break;
			if (isect < be->be_f_offset + be->be_length) {
				/* We have found an extent */
				dprintk("%s Get %p (%i)\n", __func__, be,
					atomic_read(&be->be_refcnt.refcount));
				kref_get(&be->be_refcnt);
				if (!ret)
					ret = be;
				else if (be->be_state != PNFS_BLOCK_READ_DATA)
					put_extent(be);
				else
					cow = be;
				break;
			}
		}
	}
	spin_unlock(&bl->bl_ext_lock);
	if (cow_read)
		*cow_read = cow;
	print_bl_extent(ret);
	return ret;
}

/* Similar to find_get_extent, but called with lock held, and ignores cow */
static struct pnfs_block_extent *
find_get_extent_locked(struct pnfs_block_layout *bl, sector_t isect)
{
	struct pnfs_block_extent *be, *ret = NULL;
	int i;

	dprintk("%s enter with isect %llu\n", __func__, (u64)isect);
	for (i = 0; i < EXTENT_LISTS; i++) {
		if (ret)
			break;
		list_for_each_entry(be, &bl->bl_extents[i], be_node) {
			if (isect < be->be_f_offset)
				break;
			if (isect < be->be_f_offset + be->be_length) {
				/* We have found an extent */
				dprintk("%s Get %p (%i)\n", __func__, be,
					atomic_read(&be->be_refcnt.refcount));
				kref_get(&be->be_refcnt);
				ret = be;
				break;
			}
		}
	}
	print_bl_extent(ret);
	return ret;
}

int
encode_pnfs_block_layoutupdate4(struct pnfs_block_layout *bl,
				struct pnfs_layoutcommit_arg *arg)
{
	sector_t start, end;
	struct pnfs_block_short_extent *lce, *save;
	LIST_HEAD(ranges);
	unsigned int count;
	int size;
	__be32 *p;

	dprintk("%s enter\n", __func__);
	start = arg->lseg.offset >> 9;
	end = start + (arg->lseg.length >> 9);
	dprintk("%s set start=%llu, end=%llu\n",
		__func__, (u64)start, (u64)end);

	/* BUG - creation of bl_commit is buggy - need to wait for
	 * entire block to be marked WRITTEN before it can be added.
	 */
	spin_lock(&bl->bl_ext_lock);
	list_splice_init(&bl->bl_commit, &ranges);
	count = bl->bl_count;
	bl->bl_count = 0;
	/* Want to adjust for possible truncate */
	/* We now want to adjust argument range */
	spin_unlock(&bl->bl_ext_lock);

	dprintk("%s found %i ranges\n", __func__, count);
	if (count == 0)
		return 0;
	/* XDR encode the ranges found */
	size = (NFS4_PNFS_DEVICEID4_SIZE + 28) * count + 4;
	arg->new_layout = kmalloc(size, GFP_KERNEL);
	if (!arg->new_layout)
		goto out_err;
	arg->new_layout_size = size;

	p = arg->new_layout;
	WRITE32(count);
	list_for_each_entry_safe(lce, save, &ranges, bse_node) {
		WRITE_DEVID(&lce->bse_devid);
		WRITE64(lce->bse_f_offset << 9);
		WRITE64(lce->bse_length << 9);
		WRITE64(0LL);
		WRITE32(PNFS_BLOCK_READWRITE_DATA);
		kfree(lce);
	}
	return 0;

 out_err:
	/* We need to put ranges back into commit list, but it
	 * might have changed in the meantime.
	 */
	/* We need to remove the IN_COMMIT marks */
	spin_lock(&bl->bl_ext_lock);
	/* Optimize for the simple case */
	if (!bl->bl_count) {
		/* The commit list is still empty, things are simple */
		list_splice(&ranges, &bl->bl_commit);
		bl->bl_count = count;
	} else {
		/* Reversing the (sorted) range list causes each entry to
		 * immediately placed at the beginning of bl_commit
		 */
		list_for_each_entry_safe_reverse(lce, save, &ranges, bse_node)
			add_to_commitlist(bl, lce);
	}
	spin_unlock(&bl->bl_ext_lock);
	return -ENOMEM;
}

/* Helper function to set_to_rw that initialize a new extent */
static void
_prep_new_extent(struct pnfs_block_extent *new,
		 struct pnfs_block_extent *orig,
		 sector_t offset, sector_t length, int state)
{
	kref_init(&new->be_refcnt);
	/* don't need to INIT_LIST_HEAD(&new->be_node) */
	memcpy(&new->be_devid, &orig->be_devid, sizeof(struct pnfs_deviceid));
	new->be_mdev = orig->be_mdev;
	new->be_f_offset = offset;
	new->be_length = length;
	new->be_v_offset = orig->be_v_offset - orig->be_f_offset + offset;
	new->be_state = state;
	new->be_inval = orig->be_inval;
}

/* Tries to merge be with extent in front of it in list.
 * Frees storage if not used.
 */
static struct pnfs_block_extent *
_front_merge(struct pnfs_block_extent *be, struct list_head *head,
	     struct pnfs_block_extent *storage)
{
	struct pnfs_block_extent *prev;

	if (!storage)
		goto no_merge;
	if (&be->be_node == head || be->be_node.prev == head)
		goto no_merge;
	prev = list_entry(be->be_node.prev, struct pnfs_block_extent, be_node);
	if ((prev->be_f_offset + prev->be_length != be->be_f_offset) ||
	    !extents_consistent(prev, be))
		goto no_merge;
	_prep_new_extent(storage, prev, prev->be_f_offset,
			 prev->be_length + be->be_length, prev->be_state);
	list_replace(&prev->be_node, &storage->be_node);
	put_extent(prev);
	list_del(&be->be_node);
	put_extent(be);
	return storage;

 no_merge:
	kfree(storage);
	return be;
}

u64 set_to_rw(struct pnfs_block_layout *bl, u64 offset, u64 length)
{
	u64 rv = offset + length;
	struct pnfs_block_extent *be, *e1, *e2, *e3, *new, *old;
	struct pnfs_block_extent *children[3];
	struct pnfs_block_extent *merge1 = NULL, *merge2 = NULL;
	int i = 0, j;

	dprintk("%s(%llu, %llu)\n", __func__, offset, length);
	/* Create storage for up to three new extents e1, e2, e3 */
	e1 = kmalloc(sizeof(*e1), GFP_KERNEL);
	e2 = kmalloc(sizeof(*e2), GFP_KERNEL);
	e3 = kmalloc(sizeof(*e3), GFP_KERNEL);
	/* BUG - we are ignoring any failure */
	if (!e1 || !e2 || !e3)
		goto out_nosplit;

	spin_lock(&bl->bl_ext_lock);
	be = find_get_extent_locked(bl, offset);
	rv = be->be_f_offset + be->be_length;
	if (be->be_state != PNFS_BLOCK_INVALID_DATA) {
		spin_unlock(&bl->bl_ext_lock);
		goto out_nosplit;
	}
	/* Add e* to children, bumping e*'s krefs */
	if (be->be_f_offset != offset) {
		_prep_new_extent(e1, be, be->be_f_offset,
				 offset - be->be_f_offset,
				 PNFS_BLOCK_INVALID_DATA);
		children[i++] = e1;
		kref_get(&e1->be_refcnt);
		print_bl_extent(e1);
	} else
		merge1 = e1;
	_prep_new_extent(e2, be, offset,
			 min(length, be->be_f_offset + be->be_length - offset),
			 PNFS_BLOCK_READWRITE_DATA);
	children[i++] = e2;
	kref_get(&e2->be_refcnt);
	print_bl_extent(e2);
	if (offset + length < be->be_f_offset + be->be_length) {
		_prep_new_extent(e3, be, e2->be_f_offset + e2->be_length,
				 be->be_f_offset + be->be_length -
				 offset - length,
				 PNFS_BLOCK_INVALID_DATA);
		children[i++] = e3;
		kref_get(&e3->be_refcnt);
		print_bl_extent(e3);
	} else
		merge2 = e3;

	/* Remove be from list, and insert the e* */
	/* We don't get refs on e*, since this list is the base reference
	 * set when init'ed.
	 */
	if (i < 3)
		children[i] = NULL;
	new = children[0];
	list_replace(&be->be_node, &new->be_node);
	put_extent(be);
	new = _front_merge(new, &bl->bl_extents[RW_EXTENT], merge1);
	for (j = 1; j < i; j++) {
		old = new;
		new = children[j];
		list_add(&new->be_node, &old->be_node);
	}
	if (merge2) {
		/* This is a HACK, should just create a _back_merge function */
		new = list_entry(new->be_node.next,
				 struct pnfs_block_extent, be_node);
		new = _front_merge(new, &bl->bl_extents[RW_EXTENT], merge2);
	}
	spin_unlock(&bl->bl_ext_lock);

	/* Since we removed the base reference above, be is now scheduled for
	 * destruction.
	 */
	put_extent(be);
	dprintk("%s returns %llu after split\n", __func__, rv);
	return rv;

 out_nosplit:
	kfree(e1);
	kfree(e2);
	kfree(e3);
	dprintk("%s returns %llu without splitting\n", __func__, rv);
	return rv;
}
