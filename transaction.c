// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/blkdev.h>
#include "apfs.h"

/**
 * apfs_cpoint_init_area - Initialize the new blocks of a checkpoint area
 * @sb:		superblock structure
 * @base:	first block of the area
 * @blks:	block count for the area
 * @next:	first block for the new checkpoint
 * @len:	block count for the new checkpoint
 *
 * Copies the blocks belonging to the last checkpoint to the area assigned for
 * the new one, updating their xids, oids and checksums.  Returns 0 on success,
 * or a negative error code in case of failure.
 */
static int apfs_cpoint_init_area(struct super_block *sb, u64 base, u32 blks,
				 u32 next, u32 len)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	u32 i;

	for (i = 0; i < len; ++i) {
		struct buffer_head *new_bh, *old_bh;
		struct apfs_obj_phys *new_obj;
		u32 new_index = (next + i) % blks;
		u32 old_index = (blks + new_index - len) % blks;
		u32 type;
		int err;

		new_bh = sb_bread(sb, base + new_index);
		old_bh = sb_bread(sb, base + old_index);
		if (!new_bh || !old_bh) {
			apfs_err(sb, "unable to read the checkpoint areas");
			brelse(new_bh);
			brelse(old_bh);
			return -EINVAL;
		}
		memcpy(new_bh->b_data, old_bh->b_data, sb->s_blocksize);
		brelse(old_bh);

		new_obj = (struct apfs_obj_phys *)new_bh->b_data;
		type = le32_to_cpu(new_obj->o_type);
		if ((type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL)
			new_obj->o_oid = cpu_to_le64(new_bh->b_blocknr);

		ASSERT(sbi->s_xid == le64_to_cpu(new_obj->o_xid) + 1);
		new_obj->o_xid = cpu_to_le64(sbi->s_xid);
		err = apfs_transaction_join(sb, new_bh);
		if (err) {
			brelse(new_bh);
			return err;
		}

		/* The transaction isn't over, don't commit the superblock */
		if ((type & APFS_OBJECT_TYPE_MASK) !=
		    APFS_OBJECT_TYPE_NX_SUPERBLOCK)
			set_buffer_csum(new_bh);

		new_obj = NULL;
		brelse(new_bh);
	}
	return 0;
}

/**
 * apfs_cpoint_init_desc - Initialize the descriptor area for a new checkpoint
 * @sb: superblock structure
 *
 * Copies the latest descriptor and mapping blocks into the new checkpoint;
 * updates both the on-disk and in-memory superblocks to refer to the new
 * transaction.  Returns 0 on success, or a negative error code in case of
 * failure.
 */
static int apfs_cpoint_init_desc(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	struct buffer_head *new_sb_bh = NULL;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_next = le32_to_cpu(raw_sb->nx_xp_desc_next);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u32 new_sb_index;
	int err;

	if (!desc_blks || !desc_len)
		return -EFSCORRUPTED;

	err = apfs_cpoint_init_area(sb, desc_base, desc_blks,
				    desc_next, desc_len);
	if (err)
		return err;

	/* Now update the superblock with the new checkpoint */

	new_sb_index = (desc_next + desc_len - 1) % desc_blks;
	new_sb_bh = sb_bread(sb, desc_base + new_sb_index);
	if (!new_sb_bh) {
		apfs_err(sb, "unable to read the new checkpoint superblock");
		brelse(new_sb_bh);
		return -EINVAL;
	}

	brelse(sbi->s_mobject.bh);
	sbi->s_mobject.bh = new_sb_bh;
	raw_sb = (struct apfs_nx_superblock *)new_sb_bh->b_data;
	sbi->s_msb_raw = raw_sb;
	sbi->s_mobject.block_nr = new_sb_bh->b_blocknr;

	ASSERT(sbi->s_xid == le64_to_cpu(raw_sb->nx_next_xid));
	ASSERT(buffer_trans(new_sb_bh));
	raw_sb->nx_next_xid = cpu_to_le64(sbi->s_xid + 1);

	/* Apparently the previous checkpoint gets invalidated right away */
	raw_sb->nx_xp_desc_index = cpu_to_le32(desc_next);
	desc_next = (new_sb_index + 1) % desc_blks;
	raw_sb->nx_xp_desc_next = cpu_to_le32(desc_next);
	return 0;
}

/**
 * apfs_cpoint_init_data - Initialize the data area for a new checkpoint
 * @sb: superblock structure
 *
 * Copies the latest data blocks into the new checkpoint, updating the fields
 * related to this change of location.  Returns 0 on success, or a negative
 * error code in case of failure.
 */
static int apfs_cpoint_init_data(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);
	int err;

	if (!data_blks || !data_len)
		return -EFSCORRUPTED;

	err = apfs_cpoint_init_area(sb, data_base, data_blks,
				    data_next, data_len);
	if (err)
		return err;

	/* Apparently the previous checkpoint gets invalidated right away */
	ASSERT(sbi->s_xid == le64_to_cpu(raw_sb->nx_o.o_xid));
	raw_sb->nx_xp_data_index = cpu_to_le32(data_next);
	data_next = (data_next + data_len) % data_blks;
	raw_sb->nx_xp_data_next = cpu_to_le32(data_next);
	return 0;
}

/**
 * apfs_update_mapping - Update a single checkpoint mapping
 * @sb:		superblock structure
 * @map:	checkpoint mapping to update
 *
 * Remaps @map to point to the address of the same ephemeral object in the next
 * checkpoint data area.
 */
static void apfs_update_mapping(struct super_block *sb,
				struct apfs_checkpoint_mapping *map)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);
	u64 old_paddr, new_paddr;
	u32 old_index, new_index;

	old_paddr = le64_to_cpu(map->cpm_paddr);
	old_index = old_paddr - data_base;
	new_index = (old_index + data_len) % data_blks;
	new_paddr = data_base + new_index;

	map->cpm_paddr = cpu_to_le64(new_paddr);
}

/**
 * apfs_update_mapping_blocks - Update all checkpoint mappings
 * @sb: superblock structure
 *
 * Updates the mappings in the checkpoint descriptor area to point to the
 * address of the ephemeral objects for the new checkpoint.  Returns 0 on
 * success, or a negative error code in case of failure.
 */
static int apfs_update_mapping_blocks(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u32 i;

	/* Last block in the area is superblock; the rest are mapping blocks */
	for (i = 0; i < desc_len - 1; ++i) {
		struct buffer_head *bh;
		struct apfs_checkpoint_map_phys *cpm;
		u32 desc_curr = (desc_index + i) % desc_blks;
		u32 map_count;
		int j;

		bh = sb_bread(sb, desc_base + desc_curr);
		if (!bh)
			return -EINVAL;
		ASSERT(buffer_trans(bh));
		cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;
		ASSERT(sbi->s_xid == le64_to_cpu(cpm->cpm_o.o_xid));

		map_count = le32_to_cpu(cpm->cpm_count);
		if (map_count > apfs_max_maps_per_block(sb)) {
			brelse(bh);
			return -EFSCORRUPTED;
		}

		for (j = 0; j < map_count; ++j)
			apfs_update_mapping(sb, &cpm->cpm_map[j]);
		set_buffer_csum(bh);
		brelse(bh);
	}
	return 0;
}

/**
 * apfs_cpoint_data_allocate - Allocate a new block in the checkpoint data area
 * @sb:		superblock structure
 * @bno:	on return, the allocated block number
 */
void apfs_cpoint_data_allocate(struct super_block *sb, u64 *bno)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);

	*bno = data_base + data_next;
	data_next = (data_next + 1) % data_blks;
	data_len++;

	ASSERT(sbi->s_xid == le64_to_cpu(raw_sb->nx_o.o_xid));
	raw_sb->nx_xp_data_next = cpu_to_le32(data_next);
	raw_sb->nx_xp_data_len = cpu_to_le32(data_len);
}

/**
 * apfs_checkpoint_start - Start the checkpoint for a new transaction
 * @sb:		superblock structure
 * @trans:	the transaction
 *
 * Sets the descriptor and data areas for a new checkpoint.  Returns 0 on
 * success, or a negative error code in case of failure.
 */
static int apfs_checkpoint_start(struct super_block *sb,
				 struct apfs_transaction *trans)
{
	int err;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	err = apfs_cpoint_init_desc(sb);
	if (err)
		return err;
	err = apfs_cpoint_init_data(sb);
	if (err)
		return err;
	err = apfs_update_mapping_blocks(sb);

	return err;
}

/**
 * apfs_checkpoint_end - End the new checkpoint
 * @sb:	filesystem superblock
 *
 * Flushes all changes to disk, and commits the new checkpoint by setting the
 * fletcher checksum on its superblock.  Returns 0 on success, or a negative
 * error code in case of failure.
 */
static int apfs_checkpoint_end(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_obj_phys *obj = &sbi->s_msb_raw->nx_o;
	struct buffer_head *bh = sbi->s_mobject.bh;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	apfs_obj_set_csum(sb, obj);
	mark_buffer_dirty(bh);
	return sync_dirty_buffer(bh);
}

/**
 * apfs_transaction_start - Begin a new transaction
 * @sb: superblock structure
 *
 * Also locks the filesystem for writing; returns 0 on success or a negative
 * error code in case of failure.
 */
int apfs_transaction_start(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_transaction *trans = &sbi->s_transaction;
	int err;

	down_write(&sbi->s_big_sem);

	ASSERT(!trans->t_old_msb && !trans->t_old_vsb);

	/* Backup the old superblock buffers in case the transaction fails */
	trans->t_old_msb = sbi->s_mobject.bh;
	get_bh(trans->t_old_msb);
	trans->t_old_vsb = sbi->s_vobject.bh;
	get_bh(trans->t_old_vsb);
	/* Backup the old tree roots; the node struct issues make this ugly */
	trans->t_old_cat_root = *sbi->s_cat_root;
	get_bh(trans->t_old_cat_root.object.bh);
	trans->t_old_omap_root = *sbi->s_omap_root;
	get_bh(trans->t_old_omap_root.object.bh);

	++sbi->s_xid;
	INIT_LIST_HEAD(&trans->t_buffers);

	if (sb->s_flags & SB_RDONLY) {
		/* A previous commit has failed; this should be rare */
		err = -EROFS;
		goto fail;
	}

	err = apfs_checkpoint_start(sb, trans);
	if (err)
		goto fail;

	/*
	 * If the last transaction was aborted, the current spaceman structure
	 * could be incorrect; just reread the whole thing, for now.
	 */
	err = apfs_read_spaceman(sb);
	if (err)
		goto fail;

	err = apfs_map_volume_super(sb, true /* write */);
	if (err)
		goto fail;

	/* TODO: don't copy these nodes for transactions that don't use them */
	err = apfs_read_omap(sb, true /* write */);
	if (err)
		goto fail;
	err = apfs_read_catalog(sb, true /* write */);
	if (err)
		goto fail;

	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_transaction_commit - Commit the current transaction
 * @sb: superblock structure
 *
 * Also releases the big filesystem lock; returns 0 on success or a negative
 * error code in case of failure.
 */
int apfs_transaction_commit(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_transaction *trans = &sbi->s_transaction;
	struct apfs_bh_info *bhi, *tmp;
	int err = 0;

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(trans->t_old_msb && trans->t_old_vsb);

	list_for_each_entry_safe(bhi, tmp, &trans->t_buffers, list) {
		struct buffer_head *bh = bhi->bh;
		int curr_err;

		ASSERT(buffer_trans(bh));

		if (buffer_csum(bh))
			apfs_obj_set_csum(sb, (void *)bh->b_data);
		mark_buffer_dirty(bh);
		curr_err = sync_dirty_buffer(bh);
		if (curr_err)
			err = curr_err;

		bh->b_private = NULL;
		clear_buffer_trans(bh);
		clear_buffer_csum(bh);
		brelse(bh);
		bhi->bh = NULL;
		list_del(&bhi->list);
		kfree(bhi);
	}
	if (err)
		goto fail;
	err = apfs_checkpoint_end(sb);
	if (err)
		goto fail;

	/* Success: forget the old container and volume superblocks */
	brelse(trans->t_old_msb);
	trans->t_old_msb = NULL;
	brelse(trans->t_old_vsb);
	trans->t_old_vsb = NULL;
	/* XXX: forget the buffers for the b-tree roots */
	brelse(trans->t_old_omap_root.object.bh);
	trans->t_old_omap_root.object.bh = NULL;
	brelse(trans->t_old_cat_root.object.bh);
	trans->t_old_cat_root.object.bh = NULL;

	up_write(&sbi->s_big_sem);
	return 0;

fail:
	/*
	 * This won't happen on ENOSPC, so it should be rare.  Set the
	 * filesystem read-only to simplify cleanup for the callers and
	 * avoid deciding if the transaction was completed.
	 */
	apfs_info(sb, "transaction commit failed, forcing read-only");
	sb->s_flags |= SB_RDONLY;
	return err;
}

/**
 * apfs_transaction_join - Add a buffer head to the current transaction
 * @sb:	superblock structure
 * @bh:	the buffer head
 */
int apfs_transaction_join(struct super_block *sb, struct buffer_head *bh)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_transaction *trans = &sbi->s_transaction;
	struct apfs_bh_info *bhi;

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(trans->t_old_msb && trans->t_old_vsb);

	if (buffer_trans(bh)) /* Already part of the only transaction */
		return 0;

	/* TODO: use a slab cache */
	bhi = kzalloc(sizeof(*bhi), GFP_NOFS);
	if (!bhi)
		return -ENOMEM;
	get_bh(bh);
	bhi->bh = bh;
	list_add(&bhi->list, &trans->t_buffers);

	set_buffer_trans(bh);
	bh->b_private = bhi;
	return 0;
}

/**
 * apfs_transaction_abort - Abort the current transaction
 * @sb: superblock structure
 *
 * Releases the big filesystem lock and clears the in-memory transaction data;
 * the on-disk changes are irrelevant because the superblock checksum hasn't
 * been written yet.
 */
void apfs_transaction_abort(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_transaction *trans = &sbi->s_transaction;
	struct apfs_bh_info *bhi, *tmp;

	ASSERT(trans->t_old_msb && trans->t_old_vsb);

	--sbi->s_xid;
	list_for_each_entry_safe(bhi, tmp, &trans->t_buffers, list) {
		struct buffer_head *bh = bhi->bh;

		bh->b_private = NULL;
		clear_buffer_dirty(bh);
		clear_buffer_trans(bh);
		clear_buffer_csum(bh);
		brelse(bh);
		bhi->bh = NULL;

		list_del(&bhi->list);
		kfree(bhi);
	}

	/* Restore the old container and volume superblocks */
	brelse(sbi->s_mobject.bh);
	sbi->s_mobject.bh = trans->t_old_msb;
	sbi->s_mobject.block_nr = trans->t_old_msb->b_blocknr;
	sbi->s_msb_raw = (void *)trans->t_old_msb->b_data;
	trans->t_old_msb = NULL;
	brelse(sbi->s_vobject.bh);
	sbi->s_vobject.bh = trans->t_old_vsb;
	sbi->s_vobject.block_nr = trans->t_old_vsb->b_blocknr;
	sbi->s_vsb_raw = (void *)trans->t_old_vsb->b_data;
	trans->t_old_vsb = NULL;

	/* XXX: restore the old b-tree root nodes */
	brelse(sbi->s_omap_root->object.bh);
	*(sbi->s_omap_root) = trans->t_old_omap_root;
	trans->t_old_omap_root.object.bh = NULL;
	brelse(sbi->s_cat_root->object.bh);
	*(sbi->s_cat_root) = trans->t_old_cat_root;
	trans->t_old_cat_root.object.bh = NULL;

	up_write(&sbi->s_big_sem);
}
