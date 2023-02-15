// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include "apfs.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
#include <linux/iversion.h>
#endif

/* Keep a list of mounted containers, so that their volumes can share them */
DEFINE_MUTEX(nxs_mutex);
static LIST_HEAD(nxs);

/**
 * apfs_nx_find_by_dev - Search for a device in the list of mounted containers
 * @dev:	device number of block device for the wanted container
 *
 * Returns a pointer to the container structure in the list, or NULL if the
 * container isn't currently mounted.
 */
static struct apfs_nxsb_info *apfs_nx_find_by_dev(dev_t dev)
{
	struct apfs_nxsb_info *curr;

	lockdep_assert_held(&nxs_mutex);
	list_for_each_entry(curr, &nxs, nx_list) {
		struct block_device *curr_bdev = curr->nx_bdev;

		if (curr_bdev->bd_dev == dev)
			return curr;
	}
	return NULL;
}

/**
 * apfs_sb_set_blocksize - Set the block size for the container's device
 * @sb:		superblock structure
 * @size:	size to set
 *
 * This is like sb_set_blocksize(), but it uses the container's device instead
 * of the nonexistent volume device.
 */
static int apfs_sb_set_blocksize(struct super_block *sb, int size)
{
	if (set_blocksize(APFS_NXI(sb)->nx_bdev, size))
		return 0;
	sb->s_blocksize = size;
	sb->s_blocksize_bits = blksize_bits(size);
	return sb->s_blocksize;
}

/**
 * apfs_read_super_copy - Read the copy of the container superblock in block 0
 * @sb: superblock structure
 *
 * Returns a pointer to the buffer head, or an error pointer in case of failure.
 */
static struct buffer_head *apfs_read_super_copy(struct super_block *sb)
{
	struct buffer_head *bh;
	struct apfs_nx_superblock *msb_raw;
	int blocksize;
	int err = -EINVAL;

	/*
	 * For now assume a small blocksize, we only need it so that we can
	 * read the actual blocksize from disk.
	 */
	if (!apfs_sb_set_blocksize(sb, APFS_NX_DEFAULT_BLOCK_SIZE)) {
		apfs_err(sb, "unable to set blocksize");
		return ERR_PTR(err);
	}
	bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "unable to read superblock");
		return ERR_PTR(err);
	}
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	blocksize = le32_to_cpu(msb_raw->nx_block_size);

	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!apfs_sb_set_blocksize(sb, blocksize)) {
			apfs_err(sb, "bad blocksize %d", blocksize);
			return ERR_PTR(err);
		}
		bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
		if (!bh) {
			apfs_err(sb, "unable to read superblock 2nd time");
			return ERR_PTR(err);
		}
		msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	}

	sb->s_magic = le32_to_cpu(msb_raw->nx_magic);
	if (sb->s_magic != APFS_NX_MAGIC) {
		apfs_err(sb, "not an apfs filesystem");
		goto fail;
	}
	if (!apfs_obj_verify_csum(sb, bh)) {
		apfs_err(sb, "inconsistent container superblock");
		err = -EFSBADCRC;
		goto fail;
	}
	return bh;

fail:
	brelse(bh);
	return ERR_PTR(err);
}

/**
 * apfs_make_super_copy - Write a copy of the checkpoint superblock to block 0
 * @sb:	superblock structure
 */
static void apfs_make_super_copy(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	struct buffer_head *bh;

	if (!(nxi->nx_flags & APFS_READWRITE))
		return;

	/* Only update the backup once all volumes are unmounted */
	mutex_lock(&nxs_mutex);
	if (nxi->nx_refcnt > 1)
		goto out_unlock;

	bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "failed to write block zero");
		goto out_unlock;
	}
	memcpy(bh->b_data, nxi->nx_raw, sb->s_blocksize);
	mark_buffer_dirty(bh);
	brelse(bh);
out_unlock:
	mutex_unlock(&nxs_mutex);
}

/**
 * apfs_map_main_super - Find the container superblock and map it into memory
 * @sb:	superblock structure
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets the nx_raw, nx_object and nx_xid fields of APFS_NXI(@sb).
 */
static int apfs_map_main_super(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct buffer_head *bh;
	struct buffer_head *desc_bh = NULL;
	struct apfs_nx_superblock *msb_raw;
	u64 xid, bno = APFS_NX_BLOCK_NUM;
	u64 desc_base;
	u32 desc_blocks;
	int err = -EINVAL;
	int i;

	lockdep_assert_held(&nxs_mutex);
	if (nxi->nx_refcnt > 1) {
		/* It's already mapped */
		sb->s_blocksize = nxi->nx_blocksize;
		sb->s_blocksize_bits = nxi->nx_blocksize_bits;
		sb->s_magic = le32_to_cpu(nxi->nx_raw->nx_magic);
		return 0;
	}

	/* Read the superblock from the last clean unmount */
	bh = apfs_read_super_copy(sb);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;

	/* We want to mount the latest valid checkpoint among the descriptors */
	desc_base = le64_to_cpu(msb_raw->nx_xp_desc_base);
	if (desc_base >> 63 != 0) {
		/* The highest bit is set when checkpoints are not contiguous */
		apfs_err(sb, "checkpoint descriptor tree not yet supported");
		goto fail;
	}
	desc_blocks = le32_to_cpu(msb_raw->nx_xp_desc_blocks);
	if (desc_blocks > 10000) { /* Arbitrary loop limit, is it enough? */
		apfs_err(sb, "too many checkpoint descriptors?");
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Now we go through the checkpoints one by one */
	xid = le64_to_cpu(msb_raw->nx_o.o_xid);
	for (i = 0; i < desc_blocks; ++i) {
		struct apfs_nx_superblock *desc_raw;

		brelse(desc_bh);
		desc_bh = apfs_sb_bread(sb, desc_base + i);
		if (!desc_bh) {
			apfs_err(sb, "unable to read checkpoint descriptor");
			goto fail;
		}
		desc_raw = (struct apfs_nx_superblock *)desc_bh->b_data;

		if (le32_to_cpu(desc_raw->nx_magic) != APFS_NX_MAGIC)
			continue; /* Not a superblock */
		if (le64_to_cpu(desc_raw->nx_o.o_xid) <= xid)
			continue; /* Old */
		if (!apfs_obj_verify_csum(sb, desc_bh))
			continue; /* Corrupted */

		xid = le64_to_cpu(desc_raw->nx_o.o_xid);
		msb_raw = desc_raw;
		bno = desc_base + i;
		brelse(bh);
		bh = desc_bh;
		desc_bh = NULL;
	}

	nxi->nx_xid = xid;
	nxi->nx_raw = msb_raw;
	nxi->nx_object.sb = sb; /* XXX: these "objects" never made any sense */
	nxi->nx_object.block_nr = bno;
	nxi->nx_object.oid = le64_to_cpu(msb_raw->nx_o.o_oid);
	nxi->nx_object.o_bh = bh;
	nxi->nx_object.data = bh->b_data;

	/* For now we only support blocksize < PAGE_SIZE */
	nxi->nx_blocksize = sb->s_blocksize;
	nxi->nx_blocksize_bits = sb->s_blocksize_bits;

	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_update_software_info - Write the module info to a modified volume
 * @sb: superblock structure
 *
 * Does nothing if the module information is already present at index zero of
 * the apfs_modified_by array.  Otherwise, writes it there after shifting the
 * rest of the entries to the right.
 */
static void apfs_update_software_info(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *raw = sbi->s_vsb_raw;
	struct apfs_modified_by *mod_by;

	ASSERT(sbi->s_vsb_raw);
	apfs_assert_in_transaction(sb, &raw->apfs_o);
	ASSERT(strlen(APFS_MODULE_ID_STRING) < APFS_MODIFIED_NAMELEN);
	mod_by = raw->apfs_modified_by;

	/* This check could be optimized away, but does it matter? */
	if (!strcmp(mod_by->id, APFS_MODULE_ID_STRING))
		return;
	memmove(mod_by + 1, mod_by, (APFS_MAX_HIST - 1) * sizeof(*mod_by));

	memset(mod_by->id, 0, sizeof(mod_by->id));
	strcpy(mod_by->id, APFS_MODULE_ID_STRING);
	mod_by->timestamp = cpu_to_le64(ktime_get_real_ns());
	mod_by->last_xid = cpu_to_le64(APFS_NXI(sb)->nx_xid);
}

/**
 * apfs_unmap_main_super - Clean up apfs_map_main_super()
 * @sbi:	in-memory superblock info
 *
 * It also cleans up after apfs_attach_nxi(), so the name is no longer accurate.
 */
static inline void apfs_unmap_main_super(struct apfs_sb_info *sbi)
{
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	fmode_t mode = FMODE_READ | FMODE_EXCL;
	struct apfs_object *obj = NULL;

	if (nxi->nx_flags & APFS_READWRITE)
		mode |= FMODE_WRITE;

	lockdep_assert_held(&nxs_mutex);

	list_del(&sbi->list);
	if (--nxi->nx_refcnt)
		goto out;

	obj = &nxi->nx_object;
	obj->data = NULL;
	brelse(obj->o_bh);
	obj->o_bh = NULL;
	obj = NULL;

	blkdev_put(nxi->nx_bdev, mode);
	list_del(&nxi->nx_list);
	kfree(nxi);
out:
	sbi->s_nxi = NULL;
}

/**
 * apfs_map_volume_super_bno - Map a block containing a volume superblock
 * @sb:		superblock structure
 * @bno:	block to map
 * @check:	verify the checksum?
 */
int apfs_map_volume_super_bno(struct super_block *sb, u64 bno, bool check)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = NULL;
	struct buffer_head *bh = NULL;
	int err;

	bh = apfs_sb_bread(sb, bno);
	if (!bh) {
		apfs_err(sb, "unable to read volume superblock");
		return -EINVAL;
	}

	vsb_raw = (struct apfs_superblock *)bh->b_data;
	if (le32_to_cpu(vsb_raw->apfs_magic) != APFS_MAGIC) {
		apfs_err(sb, "wrong magic in volume superblock");
		err = -EINVAL;
		goto fail;
	}

	/*
	 * XXX: apfs_omap_lookup_block() only runs this check when write
	 * is true, but it should always do it.
	 */
	if (check && !apfs_obj_verify_csum(sb, bh)) {
		apfs_err(sb, "inconsistent volume superblock");
		err = -EFSBADCRC;
		goto fail;
	}

	sbi->s_vsb_raw = vsb_raw;
	sbi->s_vobject.sb = sb;
	sbi->s_vobject.block_nr = bno;
	sbi->s_vobject.oid = le64_to_cpu(vsb_raw->apfs_o.o_oid);
	brelse(sbi->s_vobject.o_bh);
	sbi->s_vobject.o_bh = bh;
	sbi->s_vobject.data = bh->b_data;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_map_volume_super - Find the volume superblock and map it into memory
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets APFS_SB(@sb)->s_vsb_raw and APFS_SB(@sb)->s_vobject.
 */
int apfs_map_volume_super(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *msb_raw = nxi->nx_raw;
	struct apfs_omap_phys *msb_omap_raw;
	struct apfs_omap *omap = NULL;
	struct apfs_node *vnode;
	struct buffer_head *bh;
	struct apfs_vol_transaction *trans = &sbi->s_transaction;
	u64 vol_id;
	u64 vsb;
	int err;

	ASSERT(msb_raw);
	ASSERT(trans->t_old_vsb == sbi->s_vobject.o_bh);
	(void)trans;

	/* Get the id for the requested volume number */
	if (sbi->s_vol_nr >= APFS_NX_MAX_FILE_SYSTEMS) {
		apfs_err(sb, "volume number out of range");
		return -EINVAL;
	}
	vol_id = le64_to_cpu(msb_raw->nx_fs_oid[sbi->s_vol_nr]);
	if (vol_id == 0) {
		apfs_err(sb, "requested volume does not exist");
		return -EINVAL;
	}

	/* Get the container's object map */
	bh = apfs_read_object_block(sb, le64_to_cpu(msb_raw->nx_omap_oid),
				    write, false /* preserve */);
	if (IS_ERR(bh)) {
		apfs_err(sb, "unable to read container object map");
		return PTR_ERR(bh);
	}
	if (write) {
		ASSERT(buffer_trans(bh));
		msb_raw->nx_omap_oid = cpu_to_le64(bh->b_blocknr);
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the root node for the container's omap */
	vnode = apfs_read_node(sb, le64_to_cpu(msb_omap_raw->om_tree_oid),
			       APFS_OBJ_PHYSICAL, write);
	if (IS_ERR(vnode)) {
		apfs_err(sb, "unable to read volume block");
		err = PTR_ERR(vnode);
		goto fail;
	}
	if (write) {
		ASSERT(buffer_trans(bh));
		msb_omap_raw->om_tree_oid = cpu_to_le64(vnode->object.block_nr);
	}
	msb_omap_raw = NULL;
	brelse(bh);
	bh = NULL;

	omap = kzalloc(sizeof(*omap), GFP_KERNEL);
	if (!omap) {
		apfs_node_free(vnode);
		return -ENOMEM;
	}
	omap->omap_root = vnode;

	err = apfs_omap_lookup_block(sb, omap, vol_id, &vsb, write);
	apfs_node_free(vnode);
	vnode = NULL;
	kfree(omap);
	omap = NULL;
	if (err) {
		apfs_err(sb, "volume not found, likely corruption");
		return err;
	}

	/*
	 * Snapshots could get mounted during a transaction, so the fletcher
	 * checksum doesn't have to be valid.
	 */
	err = apfs_map_volume_super_bno(sb, vsb, !write && !sbi->s_snap_name);
	if (err)
		return err;

	if (write)
		apfs_update_software_info(sb);
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_unmap_volume_super - Clean up apfs_map_volume_super()
 * @sb:	filesystem superblock
 */
void apfs_unmap_volume_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_object *obj = &sbi->s_vobject;

	obj->data = NULL;
	brelse(obj->o_bh);
	obj->o_bh = NULL;
}

/**
 * apfs_get_omap - Get a reference to the omap, if it's already read
 * @sb:	filesystem superblock
 *
 * Returns the omap struct, or NULL on failure.
 */
static struct apfs_omap *apfs_get_omap(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_sb_info *curr = NULL;
	struct apfs_omap *omap = NULL;
	struct apfs_omap_cache *cache = NULL;

	lockdep_assert_held(&nxs_mutex);

	list_for_each_entry(curr, &nxi->vol_list, list) {
		if (curr == sbi)
			continue;
		if (curr->s_vol_nr == sbi->s_vol_nr) {
			omap = curr->s_omap;
			cache = &omap->omap_cache;
			++omap->omap_refcnt;
			/* Right now the cache can't be shared like this */
			cache->disabled = true;
			return omap;
		}
	}
	return NULL;
}

/**
 * apfs_read_omap - Find and read the omap root node
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * On success, returns 0 and sets the fields of APFS_SB(@sb)->s_omap; on failure
 * returns a negative error code.
 */
int apfs_read_omap(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_omap_phys *omap_raw;
	struct apfs_node *omap_root;
	struct apfs_omap *omap = NULL;
	struct buffer_head *bh;
	u64 omap_blk;
	int err;

	ASSERT(sbi->s_vsb_raw);

	ASSERT(sbi->s_omap);
	omap = sbi->s_omap;

	/* Get the block holding the volume omap information */
	omap_blk = le64_to_cpu(vsb_raw->apfs_omap_oid);
	bh = apfs_read_object_block(sb, omap_blk, write, false /* preserve */);
	if (IS_ERR(bh)) {
		apfs_err(sb, "unable to read the volume object map");
		return PTR_ERR(bh);
	}
	if (write) {
		apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
		vsb_raw->apfs_omap_oid = cpu_to_le64(bh->b_blocknr);
	}
	omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the volume's object map */
	omap_root = apfs_read_node(sb, le64_to_cpu(omap_raw->om_tree_oid),
				   APFS_OBJ_PHYSICAL, write);
	if (IS_ERR(omap_root)) {
		apfs_err(sb, "unable to read the omap root node");
		err = PTR_ERR(omap_root);
		goto fail;
	}
	if (write) {
		apfs_assert_in_transaction(sb, &omap_raw->om_o);
		ASSERT(buffer_trans(bh));
		omap_raw->om_tree_oid = cpu_to_le64(omap_root->object.block_nr);
	}
	omap->omap_latest_snap = le64_to_cpu(omap_raw->om_most_recent_snap);
	omap_raw = NULL;
	brelse(bh);

	if (omap->omap_root)
		apfs_node_free(omap->omap_root);
	omap->omap_root = omap_root;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_first_read_omap - Find and read the omap root node during mount
 * @sb:		superblock structure
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_omap; on failure returns a
 * negative error code.
 */
static int apfs_first_read_omap(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_omap *omap = NULL;
	int err;

	lockdep_assert_held(&nxs_mutex);

	/* The current transaction and all snapshots share a single omap */
	omap = apfs_get_omap(sb);
	if (omap) {
		sbi->s_omap = omap;
		return 0;
	}

	omap = kzalloc(sizeof(*omap), GFP_KERNEL);
	if (!omap)
		return -ENOMEM;

	sbi->s_omap = omap;
	err = apfs_read_omap(sb, false /* write */);
	if (err) {
		kfree(omap);
		sbi->s_omap = NULL;
		return err;
	}

	++omap->omap_refcnt;
	return 0;
}

/**
 * apfs_put_omap - Release a reference to an object map
 * @omap: the object map
 */
static void apfs_put_omap(struct apfs_omap *omap)
{
	lockdep_assert_held(&nxs_mutex);

	if (!omap)
		return;

	if (--omap->omap_refcnt != 0)
		return;

	apfs_node_free(omap->omap_root);
	kfree(omap);
}

/**
 * apfs_read_catalog - Find and read the catalog root node
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_cat_root; on failure returns
 * a negative error code.
 */
int apfs_read_catalog(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_node *root_node;

	ASSERT(sbi->s_omap && sbi->s_omap->omap_root);

	root_node = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_root_tree_oid),
				   APFS_OBJ_VIRTUAL, write);
	if (IS_ERR(root_node)) {
		apfs_err(sb, "unable to read catalog root node");
		return PTR_ERR(root_node);
	}

	if (sbi->s_cat_root)
		apfs_node_free(sbi->s_cat_root);
	sbi->s_cat_root = root_node;
	return 0;
}

static void apfs_put_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	/* Update the volume's unmount time */
	if (!(sb->s_flags & SB_RDONLY)) {
		struct apfs_superblock *vsb_raw;
		struct buffer_head *vsb_bh;
		struct apfs_max_ops maxops = {0};

		if (apfs_transaction_start(sb, maxops))
			goto fail;
		vsb_raw = sbi->s_vsb_raw;
		vsb_bh = sbi->s_vobject.o_bh;

		apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
		ASSERT(buffer_trans(vsb_bh));

		vsb_raw->apfs_unmount_time = cpu_to_le64(ktime_get_real_ns());
		set_buffer_csum(vsb_bh);

		/* Guarantee commit */
		sbi->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
		if (apfs_transaction_commit(sb)) {
			apfs_transaction_abort(sb);
			goto fail;
		}
	}

	/*
	 * Even if this particular volume/snapshot was read-only, the container
	 * may have changed and need an update here.
	 */
	apfs_make_super_copy(sb);

fail:
	iput(sbi->s_private_dir);
	sbi->s_private_dir = NULL;

	apfs_node_free(sbi->s_cat_root);
	apfs_unmap_volume_super(sb);

	mutex_lock(&nxs_mutex);
	apfs_put_omap(sbi->s_omap);
	sbi->s_omap = NULL;
	apfs_unmap_main_super(sbi);
	mutex_unlock(&nxs_mutex);

	sb->s_fs_info = NULL;

	kfree(sbi->s_snap_name);
	sbi->s_snap_name = NULL;
	if (sbi->s_dflt_pfk)
		kfree(sbi->s_dflt_pfk);
	kfree(sbi);
}

static struct kmem_cache *apfs_inode_cachep;

static struct inode *apfs_alloc_inode(struct super_block *sb)
{
	struct apfs_inode_info *ai;
	struct apfs_dstream_info *dstream;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	ai = alloc_inode_sb(sb, apfs_inode_cachep, GFP_KERNEL);
#else
	ai = kmem_cache_alloc(apfs_inode_cachep, GFP_KERNEL);
#endif
	if (!ai)
		return NULL;
	dstream = &ai->i_dstream;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
	inode_set_iversion(&ai->vfs_inode, 1);
#else
	ai->vfs_inode.i_version = 1;
#endif
	dstream->ds_sb = sb;
	dstream->ds_inode = &ai->vfs_inode;
	dstream->ds_cached_ext.len = 0;
	dstream->ds_ext_dirty = false;
	ai->i_nchildren = 0;
	INIT_LIST_HEAD(&ai->i_list);
	return &ai->vfs_inode;
}

static void apfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(apfs_inode_cachep, APFS_I(inode));
}

static void apfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, apfs_i_callback);
}

static void init_once(void *p)
{
	struct apfs_inode_info *ai = (struct apfs_inode_info *)p;
	struct apfs_dstream_info *dstream = &ai->i_dstream;

	spin_lock_init(&dstream->ds_ext_lock);
	inode_init_once(&ai->vfs_inode);
}

static int __init init_inodecache(void)
{
	apfs_inode_cachep = kmem_cache_create("apfs_inode_cache",
					     sizeof(struct apfs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD|SLAB_ACCOUNT),
					     init_once);
	if (apfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static int apfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_nxsb_info *nxi = APFS_SB(sb)->s_nxi;
	struct apfs_max_ops maxops;
	int err;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto fail;
	/* Don't commit yet, or the inode will get flushed again and lock up */
	nxi->nx_transaction.t_state |= APFS_NX_TRANS_DEFER_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static void destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(apfs_inode_cachep);
}

/**
 * apfs_count_used_blocks - Count the blocks in use across all volumes
 * @sb:		filesystem superblock
 * @count:	on return it will store the block count
 *
 * This function probably belongs in a separate file, but for now it is
 * only called by statfs.
 */
static int apfs_count_used_blocks(struct super_block *sb, u64 *count)
{
	struct apfs_nx_superblock *msb_raw = APFS_NXI(sb)->nx_raw;
	struct apfs_node *vnode;
	struct apfs_omap_phys *msb_omap_raw;
	struct buffer_head *bh;
	struct apfs_omap *omap = NULL;
	u64 msb_omap, vb;
	int i;
	int err = 0;

	/* Get the container's object map */
	msb_omap = le64_to_cpu(msb_raw->nx_omap_oid);
	bh = apfs_sb_bread(sb, msb_omap);
	if (!bh) {
		apfs_err(sb, "unable to read container object map");
		return -EIO;
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the Volume Block */
	vb = le64_to_cpu(msb_omap_raw->om_tree_oid);
	msb_omap_raw = NULL;
	brelse(bh);
	bh = NULL;
	vnode = apfs_read_node(sb, vb, APFS_OBJ_PHYSICAL, false /* write */);
	if (IS_ERR(vnode)) {
		apfs_err(sb, "unable to read volume block");
		return PTR_ERR(vnode);
	}

	omap = kzalloc(sizeof(*omap), GFP_KERNEL);
	if (!omap) {
		err = -ENOMEM;
		goto fail;
	}
	omap->omap_root = vnode;

	/* Iterate through the checkpoint superblocks and add the used blocks */
	*count = 0;
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_superblock *vsb_raw;
		u64 vol_id;
		u64 vol_bno;

		vol_id = le64_to_cpu(msb_raw->nx_fs_oid[i]);
		if (vol_id == 0) /* All volumes have been checked */
			break;
		err = apfs_omap_lookup_block(sb, omap, vol_id, &vol_bno,
					     false /* write */);
		if (err)
			break;

		bh = apfs_sb_bread(sb, vol_bno);
		if (!bh) {
			err = -EIO;
			apfs_err(sb, "unable to read volume superblock");
			break;
		}
		vsb_raw = (struct apfs_superblock *)bh->b_data;
		*count += le64_to_cpu(vsb_raw->apfs_fs_alloc_count);
		brelse(bh);
	}

fail:
	kfree(omap);
	apfs_node_free(vnode);
	return err;
}

static int apfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *msb_raw;
	struct apfs_superblock *vol;
	u64 fsid, used_blocks = 0;
	int err;

	down_read(&nxi->nx_big_sem);
	msb_raw = nxi->nx_raw;
	vol = sbi->s_vsb_raw;

	buf->f_type = APFS_NX_MAGIC;
	/* Nodes are assumed to fit in a page, for now */
	buf->f_bsize = sb->s_blocksize;

	/* Volumes share the whole disk space */
	buf->f_blocks = le64_to_cpu(msb_raw->nx_block_count);
	err = apfs_count_used_blocks(sb, &used_blocks);
	if (err)
		goto fail;
	buf->f_bfree = buf->f_blocks - used_blocks;
	buf->f_bavail = buf->f_bfree; /* I don't know any better */

	/* The file count is only for the mounted volume */
	buf->f_files = le64_to_cpu(vol->apfs_num_files) +
		       le64_to_cpu(vol->apfs_num_directories) +
		       le64_to_cpu(vol->apfs_num_symlinks) +
		       le64_to_cpu(vol->apfs_num_other_fsobjects);

	/*
	 * buf->f_ffree is left undefined for now. Maybe it should report the
	 * number of available cnids, like hfsplus attempts to do.
	 */

	buf->f_namelen = APFS_NAME_LEN;

	/* There are no clear rules for the fsid, so we follow ext2 here */
	fsid = le64_to_cpup((void *)vol->apfs_vol_uuid) ^
	       le64_to_cpup((void *)vol->apfs_vol_uuid + sizeof(u64));
	buf->f_fsid.val[0] = fsid & 0xFFFFFFFFUL;
	buf->f_fsid.val[1] = (fsid >> 32) & 0xFFFFFFFFUL;

fail:
	up_read(&nxi->nx_big_sem);
	return err;
}

static int apfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct apfs_sb_info *sbi = APFS_SB(root->d_sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(root->d_sb);

	if (sbi->s_vol_nr != 0)
		seq_printf(seq, ",vol=%u", sbi->s_vol_nr);
	if (uid_valid(sbi->s_uid))
		seq_printf(seq, ",uid=%u", from_kuid(&init_user_ns,
						     sbi->s_uid));
	if (gid_valid(sbi->s_gid))
		seq_printf(seq, ",gid=%u", from_kgid(&init_user_ns,
						     sbi->s_gid));
	if (nxi->nx_flags & APFS_CHECK_NODES)
		seq_puts(seq, ",cknodes");

	return 0;
}

/* TODO: don't ignore @wait */
int apfs_sync_fs(struct super_block *sb, int wait)
{
	struct apfs_max_ops maxops = {0};
	int err;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	APFS_SB(sb)->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

/* Only supports read-only remounts, everything else is silently ignored */
static int apfs_remount(struct super_block *sb, int *flags, char *data)
{
	int err = 0;

	err = sync_filesystem(sb);
	if (err)
		return err;

	/* TODO: race? Could a new transaction have started already? */
	if (*flags & SB_RDONLY)
		sb->s_flags |= SB_RDONLY;

	/*
	 * TODO: readwrite remounts seem simple enough, but I worry about
	 * remounting aborted transactions. I would probably also need a
	 * dry-run version of parse_options().
	 */
	apfs_notice(sb, "all remounts can do is turn a volume read-only");
	return 0;
}

static const struct super_operations apfs_sops = {
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_destroy_inode,
	.write_inode	= apfs_write_inode,
	.evict_inode	= apfs_evict_inode,
	.put_super	= apfs_put_super,
	.sync_fs	= apfs_sync_fs,
	.statfs		= apfs_statfs,
	.remount_fs	= apfs_remount,
	.show_options	= apfs_show_options,
};

enum {
	Opt_readwrite, Opt_cknodes, Opt_uid, Opt_gid, Opt_vol, Opt_snap, Opt_err,
};

static const match_table_t tokens = {
	{Opt_readwrite, "readwrite"},
	{Opt_cknodes, "cknodes"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_vol, "vol=%u"},
	{Opt_snap, "snap=%s"},
	{Opt_err, NULL}
};

/**
 * apfs_set_nx_flags - Set the mount flags for the container, if allowed
 * @sb:		superblock structure
 * @flags:	flags to set
 */
static void apfs_set_nx_flags(struct super_block *sb, unsigned int flags)
{
	struct apfs_nxsb_info *nxi = APFS_SB(sb)->s_nxi;

	lockdep_assert_held(&nxs_mutex);

	/* The mount flags can only be set when the container is first mounted */
	if (nxi->nx_refcnt == 1)
		nxi->nx_flags = flags;
	else if (flags != nxi->nx_flags)
		apfs_warn(sb, "ignoring mount flags - container already mounted");
}

/**
 * apfs_get_vol_number - Retrieve the volume number from the mount options
 * @options:	string of mount options
 *
 * On error, it will just return the default volume 0.
 */
static unsigned int apfs_get_vol_number(char *options)
{
	char needle[] = "vol=";
	char *volstr;
	long vol;

	if (!options)
		return 0;

	/* TODO: just parse all the options once... */
	volstr = strstr(options, needle);
	if (!volstr)
		return 0;
	volstr += sizeof(needle) - 1;

	/* TODO: other bases? */
	if (kstrtol(volstr, 10, &vol) < 0)
		return 0;
	return vol;
}

/**
 * apfs_get_snap_name - Duplicate the snapshot label from the mount options
 * @options:	string of mount options
 *
 * On error, it will just return the default NULL snapshot name. TODO: this is
 * actually a bit dangerous because a memory allocation failure might get the
 * same snapshot mounted twice, without a shared superblock.
 */
static char *apfs_get_snap_name(char *options)
{
	char needle[] = "snap=";
	char *name = NULL, *end = NULL;

	if (!options)
		return NULL;

	name = strstr(options, needle);
	if (!name)
		return NULL;

	name += sizeof(needle) - 1;
	end = strchrnul(name, ',');

	return kmemdup_nul(name, end - name, GFP_KERNEL);
}

/*
 * Many of the parse_options() functions in other file systems return 0
 * on error. This one returns an error code, and 0 on success.
 */
static int parse_options(struct super_block *sb, char *options)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	int err = 0;
	unsigned int nx_flags;

	lockdep_assert_held(&nxs_mutex);

	/* Set default values before parsing */
	sbi->s_vol_nr = 0;
	nx_flags = 0;

	if (!options)
		goto out;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_readwrite:
			/*
			 * Write support is not safe yet, so keep it disabled
			 * unless the user requests it explicitly.
			 */
			nx_flags |= APFS_READWRITE;
			break;
		case Opt_cknodes:
			/*
			 * Right now, node checksums are too costly to enable
			 * by default.  TODO: try to improve this.
			 */
			nx_flags |= APFS_CHECK_NODES;
			break;
		case Opt_uid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(sbi->s_uid)) {
				apfs_err(sb, "invalid uid");
				return -EINVAL;
			}
			break;
		case Opt_gid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(sbi->s_gid)) {
				apfs_err(sb, "invalid gid");
				return -EINVAL;
			}
			break;
		case Opt_vol:
			err = match_int(&args[0], &sbi->s_vol_nr);
			if (err)
				return err;
			break;
		case Opt_snap:
			kfree(sbi->s_snap_name);
			sbi->s_snap_name = match_strdup(&args[0]);
			if (!sbi->s_snap_name)
				return -ENOMEM;
			break;
		default:
			return -EINVAL;
		}
	}

out:
	apfs_set_nx_flags(sb, nx_flags);
	if ((nxi->nx_flags & APFS_READWRITE) && !(sb->s_flags & SB_RDONLY))
		apfs_notice(sb, "experimental write support is enabled");
	else
		sb->s_flags |= SB_RDONLY;
	return 0;
}

/**
 * apfs_check_features - Check for unsupported features in the filesystem
 * @sb: superblock structure
 *
 * Returns -EINVAL if unsupported incompatible features are found, otherwise
 * returns 0.
 */
static int apfs_check_features(struct super_block *sb)
{
	struct apfs_nx_superblock *msb_raw = APFS_NXI(sb)->nx_raw;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	u64 features;

	ASSERT(msb_raw);
	ASSERT(vsb_raw);

	features = le64_to_cpu(msb_raw->nx_incompatible_features);
	if (features & ~APFS_NX_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb,
			  "unknown incompatible container features (0x%llx)",
			  features);
		return -EINVAL;
	}
	if (features & APFS_NX_INCOMPAT_FUSION) {
		apfs_warn(sb, "fusion drives are not supported");
		return -EINVAL;
	}

	features = le64_to_cpu(vsb_raw->apfs_incompatible_features);
	if (features & ~APFS_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb, "unknown incompatible volume features (0x%llx)",
			  features);
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_DATALESS_SNAPS) {
		apfs_warn(sb, "snapshots with no data are not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_ENC_ROLLED) {
		apfs_warn(sb, "encrypted volumes are not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_INCOMPLETE_RESTORE) {
		apfs_warn(sb, "incomplete restore is not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_RESERVED_40) {
		apfs_warn(sb, "reserved incompatible feature flag is set");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_SEALED_VOLUME) {
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "writes to sealed volumes are not yet supported");
			return -EINVAL;
		}
		apfs_info(sb, "volume is sealed");
	}

	features = le64_to_cpu(vsb_raw->apfs_fs_flags);
	/* Some encrypted volumes are readable anyway */
	if (!(features & APFS_FS_UNENCRYPTED))
		apfs_warn(sb, "volume is encrypted, may not be read correctly");

	features = le64_to_cpu(msb_raw->nx_readonly_compatible_features);
	if (features & ~APFS_NX_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb,
		     "unknown read-only compatible container features (0x%llx)",
		     features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "container can't be mounted read-write");
			return -EINVAL;
		}
	}

	features = le64_to_cpu(vsb_raw->apfs_readonly_compatible_features);
	if (features & ~APFS_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb,
			"unknown read-only compatible volume features (0x%llx)",
			features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "volume can't be mounted read-write");
			return -EINVAL;
		}
	}

	/* TODO: add checks for encryption, snapshots? */
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)

/**
 * apfs_setup_bdi - Set up the bdi for the superblock
 * @sb: superblock structure
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_setup_bdi(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct backing_dev_info *bdi_dev = NULL, *bdi_sb = NULL;
	int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) || (defined(RHEL_RELEASE) && LINUX_VERSION_CODE == KERNEL_VERSION(5, 14, 0))
	bdi_dev = nxi->nx_bdev->bd_disk->bdi;
#else
	bdi_dev = nxi->nx_bdev->bd_bdi;
#endif

	err = super_setup_bdi(sb);
	if (err)
		return err;
	bdi_sb = sb->s_bdi;

	bdi_sb->ra_pages = bdi_dev->ra_pages;
	bdi_sb->io_pages = bdi_dev->io_pages;

	bdi_sb->capabilities = bdi_dev->capabilities;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	bdi_sb->capabilities &= ~BDI_CAP_WRITEBACK;
#else
	bdi_sb->capabilities |= BDI_CAP_NO_WRITEBACK | BDI_CAP_NO_ACCT_DIRTY;
#endif

	return 0;
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0) */

/* This is needed for readahead, so old kernels will be slower */
static int apfs_setup_bdi(struct super_block *sb)
{
	return 0;
}

#endif

static int apfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct inode *root;
	int err;

	ASSERT(sbi);
	lockdep_assert_held(&nxs_mutex);

	err = apfs_setup_bdi(sb);
	if (err)
		return err;

	sbi->s_uid = INVALID_UID;
	sbi->s_gid = INVALID_GID;
	err = parse_options(sb, data);
	if (err)
		return err;

	err = apfs_map_volume_super(sb, false /* write */);
	if (err)
		return err;

	err = apfs_check_features(sb);
	if (err)
		goto failed_omap;

	/*
	 * The omap needs to be set before the call to apfs_read_catalog().
	 * It's also shared with all the snapshots, so it needs to be read
	 * before we switch to the old superblock.
	 */
	err = apfs_first_read_omap(sb);
	if (err)
		goto failed_omap;

	if (sbi->s_snap_name) {
		err = apfs_switch_to_snapshot(sb);
		if (err)
			goto failed_cat;
	}

	err = apfs_read_catalog(sb, false /* write */);
	if (err)
		goto failed_cat;

	sb->s_op = &apfs_sops;
	sb->s_d_op = &apfs_dentry_operations;
	sb->s_xattr = apfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1; /* Nanosecond granularity */

	sbi->s_private_dir = apfs_iget(sb, APFS_PRIV_DIR_INO_NUM);
	if (IS_ERR(sbi->s_private_dir)) {
		apfs_err(sb, "unable to get private-dir inode");
		err = PTR_ERR(sbi->s_private_dir);
		goto failed_private_dir;
	}

	root = apfs_iget(sb, APFS_ROOT_DIR_INO_NUM);
	if (IS_ERR(root)) {
		apfs_err(sb, "unable to get root inode");
		err = PTR_ERR(root);
		goto failed_mount;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		apfs_err(sb, "unable to get root dentry");
		err = -ENOMEM;
		goto failed_mount;
	}
	return 0;

failed_mount:
	iput(sbi->s_private_dir);
failed_private_dir:
	sbi->s_private_dir = NULL;
	apfs_node_free(sbi->s_cat_root);
failed_cat:
	apfs_put_omap(sbi->s_omap);
	sbi->s_omap = NULL;
failed_omap:
	apfs_unmap_volume_super(sb);
	return err;
}

/**
 * apfs_strings_are_equal - Compare two possible NULL strings
 * @str1: the first string
 * @str2: the second string
 */
static bool apfs_strings_are_equal(const char *str1, const char *str2)
{
	if (str1 == str2) /* Both are NULL */
		return true;
	if (!str1 || !str2) /* One is NULL */
		return false;
	return strcmp(str1, str2) == 0;
}

/**
 * apfs_test_super - Check if two volume superblocks are for the same volume
 * @sb:		superblock structure for a currently mounted volume
 * @data:	superblock info for the volume being mounted
 */
static int apfs_test_super(struct super_block *sb, void *data)
{
	struct apfs_sb_info *sbi_1 = data;
	struct apfs_sb_info *sbi_2 = APFS_SB(sb);

	if (sbi_1->s_nxi != sbi_2->s_nxi)
		return false;
	if (sbi_1->s_vol_nr != sbi_2->s_vol_nr)
		return false;
	return apfs_strings_are_equal(sbi_1->s_snap_name, sbi_2->s_snap_name);
}

/**
 * apfs_set_super - Assign a fake bdev and an info struct to a superblock
 * @sb:		superblock structure to set
 * @data:	superblock info for the volume being mounted
 */
static int apfs_set_super(struct super_block *sb, void *data)
{
	int err = set_anon_super(sb, data);
	if (!err)
		sb->s_fs_info = data;
	return err;
}

static struct file_system_type apfs_fs_type;

/*
 * Wrapper for lookup_bdev() that supports older kernels.
 */
static int apfs_lookup_bdev(const char *pathname, dev_t *dev)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	struct block_device *bdev;

	bdev = lookup_bdev(pathname);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	*dev = bdev->bd_dev;
	bdput(bdev);
	return 0;
#else
	return lookup_bdev(pathname, dev);
#endif
}

/**
 * apfs_attach_nxi - Attach container sb info to a volume's sb info
 * @sbi:	new superblock info structure for the volume to be mounted
 * @dev_name:	path name for the container's block device
 * @mode:	FMODE_* mask
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_attach_nxi(struct apfs_sb_info *sbi, const char *dev_name, fmode_t mode)
{
	struct apfs_nxsb_info *nxi;
	dev_t dev = 0;
	int ret;

	lockdep_assert_held(&nxs_mutex);

	ret = apfs_lookup_bdev(dev_name, &dev);
	if (ret)
		return ret;

	nxi = apfs_nx_find_by_dev(dev);
	if (!nxi) {
		struct block_device *bdev;

		nxi = kzalloc(sizeof(*nxi), GFP_KERNEL);
		if (!nxi)
			return -ENOMEM;

		bdev = blkdev_get_by_path(dev_name, mode, &apfs_fs_type);
		if (IS_ERR(bdev)) {
			kfree(nxi);
			return PTR_ERR(bdev);
		}

		nxi->nx_bdev = bdev;
		init_rwsem(&nxi->nx_big_sem);
		list_add(&nxi->nx_list, &nxs);
		INIT_LIST_HEAD(&nxi->vol_list);
	}

	list_add(&sbi->list, &nxi->vol_list);
	sbi->s_nxi = nxi;
	++nxi->nx_refcnt;
	return 0;
}

/*
 * This function is a copy of mount_bdev() that allows multiple mounts.
 */
static struct dentry *apfs_mount(struct file_system_type *fs_type, int flags,
				 const char *dev_name, void *data)
{
	struct apfs_nxsb_info *nxi;
	struct super_block *sb;
	struct apfs_sb_info *sbi;
	fmode_t mode = FMODE_READ | FMODE_EXCL;
	int error = 0;

	mutex_lock(&nxs_mutex);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		error = -ENOMEM;
		goto out_unlock;
	}
	sbi->s_vol_nr = apfs_get_vol_number(data);
	sbi->s_snap_name = apfs_get_snap_name(data);

	/* Make sure that snapshots are mounted read-only */
	if (sbi->s_snap_name)
		flags |= SB_RDONLY;

	if (!(flags & SB_RDONLY))
		mode |= FMODE_WRITE;

	error = apfs_attach_nxi(sbi, dev_name, mode);
	if (error)
		goto out_free_sbi;
	nxi = sbi->s_nxi;

	/* TODO: lockfs stuff? Btrfs doesn't seem to care */
	sb = sget(fs_type, apfs_test_super, apfs_set_super, flags | SB_NOSEC, sbi);
	if (IS_ERR(sb))
		goto out_unmap_super;

	if (sb->s_root) {
		if ((flags ^ sb->s_flags) & SB_RDONLY) {
			error = -EBUSY;
			goto out_deactivate_super;
		}
		/* Only one superblock per volume */
		apfs_unmap_main_super(sbi);
		kfree(sbi->s_snap_name);
		sbi->s_snap_name = NULL;
		kfree(sbi);
		sbi = NULL;
	} else {
		error = apfs_map_main_super(sb);
		if (error)
			goto out_deactivate_super;
		sb->s_mode = mode;
		snprintf(sb->s_id, sizeof(sb->s_id), "%xg", sb->s_dev);
		error = apfs_fill_super(sb, data, flags & SB_SILENT ? 1 : 0);
		if (error)
			goto out_deactivate_super;
		sb->s_flags |= SB_ACTIVE;
	}

	mutex_unlock(&nxs_mutex);
	return dget(sb->s_root);

out_deactivate_super:
	deactivate_locked_super(sb);
out_unmap_super:
	apfs_unmap_main_super(sbi);
out_free_sbi:
	kfree(sbi->s_snap_name);
	kfree(sbi);
out_unlock:
	mutex_unlock(&nxs_mutex);
	return ERR_PTR(error);
}

static struct file_system_type apfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "apfs",
	.mount		= apfs_mount,
	.kill_sb	= kill_anon_super,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("apfs");

static int __init init_apfs_fs(void)
{
	int err = 0;

	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&apfs_fs_type);
	if (err)
		destroy_inodecache();
	return err;
}

static void __exit exit_apfs_fs(void)
{
	unregister_filesystem(&apfs_fs_type);
	destroy_inodecache();
}

MODULE_AUTHOR("Ernesto A. Fernández");
MODULE_DESCRIPTION("Apple File System");
MODULE_LICENSE("GPL");
module_init(init_apfs_fs)
module_exit(exit_apfs_fs)
