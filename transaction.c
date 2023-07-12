// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/blkdev.h>
#include <linux/rmap.h>
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	u32 i;

	for (i = 0; i < len; ++i) {
		struct buffer_head *new_bh, *old_bh;
		struct apfs_obj_phys *new_obj;
		u32 new_index = (next + i) % blks;
		u32 old_index = (blks + new_index - len) % blks;
		u32 type;
		int err;

		new_bh = apfs_getblk(sb, base + new_index);
		old_bh = apfs_sb_bread(sb, base + old_index);
		if (!new_bh || !old_bh) {
			apfs_err(sb, "unable to read the checkpoint areas");
			brelse(new_bh);
			brelse(old_bh);
			return -EINVAL;
		}
		memcpy(new_bh->b_data, old_bh->b_data, sb->s_blocksize);
		brelse(old_bh);

		if (nxi->nx_flags & APFS_CHECK_NODES && !apfs_obj_verify_csum(sb, new_bh)) {
			apfs_err(sb, "bad checksum for checkpoint area block");
			brelse(new_bh);
			return -EFSBADCRC;
		}

		new_obj = (struct apfs_obj_phys *)new_bh->b_data;
		type = le32_to_cpu(new_obj->o_type);
		if ((type & APFS_OBJ_STORAGETYPE_MASK) == APFS_OBJ_PHYSICAL)
			new_obj->o_oid = cpu_to_le64(new_bh->b_blocknr);

		ASSERT(nxi->nx_xid == le64_to_cpu(new_obj->o_xid) + 1);
		new_obj->o_xid = cpu_to_le64(nxi->nx_xid);
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	struct buffer_head *new_sb_bh = NULL;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_next = le32_to_cpu(raw_sb->nx_xp_desc_next);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u32 new_sb_index;
	int err;

	if (!desc_blks || !desc_len) {
		apfs_err(sb, "bad checkpoint descriptor area");
		return -EFSCORRUPTED;
	}

	err = apfs_cpoint_init_area(sb, desc_base, desc_blks,
				    desc_next, desc_len);
	if (err) {
		apfs_err(sb, "failed to init area");
		return err;
	}

	/* Now update the superblock with the new checkpoint */

	new_sb_index = (desc_next + desc_len - 1) % desc_blks;
	new_sb_bh = apfs_sb_bread(sb, desc_base + new_sb_index);
	if (!new_sb_bh) {
		apfs_err(sb, "unable to read the new checkpoint superblock");
		brelse(new_sb_bh);
		return -EINVAL;
	}

	brelse(nxi->nx_object.o_bh);
	nxi->nx_object.o_bh = new_sb_bh;
	nxi->nx_object.data = new_sb_bh->b_data;
	raw_sb = (struct apfs_nx_superblock *)new_sb_bh->b_data;
	nxi->nx_raw = raw_sb;
	nxi->nx_object.block_nr = new_sb_bh->b_blocknr;

	ASSERT(nxi->nx_xid == le64_to_cpu(raw_sb->nx_next_xid));
	ASSERT(buffer_trans(new_sb_bh));
	raw_sb->nx_next_xid = cpu_to_le64(nxi->nx_xid + 1);

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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);
	int err;

	if (!data_blks || !data_len) {
		apfs_err(sb, "bad checkpoint data area");
		return -EFSCORRUPTED;
	}

	err = apfs_cpoint_init_area(sb, data_base, data_blks,
				    data_next, data_len);
	if (err) {
		apfs_err(sb, "failed to init area");
		return err;
	}

	/* Apparently the previous checkpoint gets invalidated right away */
	apfs_assert_in_transaction(sb, &raw_sb->nx_o);
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
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

		bh = apfs_sb_bread(sb, desc_base + desc_curr);
		if (!bh) {
			apfs_err(sb, "failed to read cpm block");
			return -EINVAL;
		}
		ASSERT(buffer_trans(bh));
		cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;
		apfs_assert_in_transaction(sb, &cpm->cpm_o);

		map_count = le32_to_cpu(cpm->cpm_count);
		if (map_count > apfs_max_maps_per_block(sb)) {
			apfs_err(sb, "block has too many maps (%d)", map_count);
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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);

	*bno = data_base + data_next;
	data_next = (data_next + 1) % data_blks;
	data_len++;

	apfs_assert_in_transaction(sb, &raw_sb->nx_o);
	raw_sb->nx_xp_data_next = cpu_to_le32(data_next);
	raw_sb->nx_xp_data_len = cpu_to_le32(data_len);
}

/**
 * apfs_cpoint_data_free - Free a block in the checkpoint data area
 * @sb:		superblock structure
 * @bno:	block number to free
 *
 * Returns 0 on sucess, or a negative error code in case of failure.
 */
int apfs_cpoint_data_free(struct super_block *sb, u64 bno)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	u32 data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u32 data_len = le32_to_cpu(raw_sb->nx_xp_data_len);
	u32 i, bno_i;

	/*
	 * We can't leave a hole in the data area, so we need to shift all
	 * blocks that come after @bno one position back.
	 */
	bno_i = apfs_index_in_data_area(sb, bno);
	for (i = bno_i; i < data_len - 1; ++i) {
		struct buffer_head *old_bh, *new_bh;
		int err;

		new_bh = apfs_getblk(sb, apfs_data_index_to_bno(sb, i));
		old_bh = apfs_sb_bread(sb, apfs_data_index_to_bno(sb, i + 1));
		if (!new_bh || !old_bh) {
			apfs_err(sb, "failed to map blocks in data area");
			brelse(new_bh);
			brelse(old_bh);
			return -EIO;
		}
		/* I could also just remap the buffer heads... */
		memcpy(new_bh->b_data, old_bh->b_data, sb->s_blocksize);

		brelse(old_bh);
		err = apfs_transaction_join(sb, new_bh); /* Not really needed */
		brelse(new_bh);
		if (err)
			return err;
	}

	data_next = (data_blks + data_next - 1) % data_blks;
	data_len--;

	apfs_assert_in_transaction(sb, &raw_sb->nx_o);
	raw_sb->nx_xp_data_next = cpu_to_le32(data_next);
	raw_sb->nx_xp_data_len = cpu_to_le32(data_len);
	return 0;
}

/**
 * apfs_checkpoint_start - Start the checkpoint for a new transaction
 * @sb:	superblock structure
 *
 * Sets the descriptor and data areas for a new checkpoint.  Returns 0 on
 * success, or a negative error code in case of failure.
 */
static int apfs_checkpoint_start(struct super_block *sb)
{
	int err;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	err = apfs_cpoint_init_desc(sb);
	if (err) {
		apfs_err(sb, "failed to init descriptor area");
		return err;
	}
	err = apfs_cpoint_init_data(sb);
	if (err) {
		apfs_err(sb, "failed to init data area");
		return err;
	}
	err = apfs_update_mapping_blocks(sb);
	if (err)
		apfs_err(sb, "failed to update checkpoint mappings");

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
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_obj_phys *obj = &nxi->nx_raw->nx_o;
	struct buffer_head *bh = nxi->nx_object.o_bh;
	struct inode *bdev_inode = nxi->nx_bdev->bd_inode;
	struct address_space *bdev_map = bdev_inode->i_mapping;
	int err;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	err = filemap_write_and_wait(bdev_map);
	if (err)
		return err;

	apfs_obj_set_csum(sb, obj);
	mark_buffer_dirty(bh);
	err = sync_dirty_buffer(bh);
	if (err)
		return err;

	return filemap_write_and_wait(bdev_map);
}

/**
 * apfs_transaction_has_room - Is there enough free space for this transaction?
 * @sb:		superblock structure
 * @maxops:	maximum operations expected
 */
static bool apfs_transaction_has_room(struct super_block *sb, struct apfs_max_ops maxops)
{
	u64 max_cat_blks, max_omap_blks, max_extref_blks, max_blks;
	/* I don't know the actual maximum heights, just guessing */
	const u64 max_cat_height = 8, max_omap_height = 3, max_extref_height = 3;

	/*
	 * On the worst possible case (a tree of max_height), each new insertion
	 * to the catalog may both cow and split every node up to the root. The
	 * root though, is only cowed once.
	 */
	max_cat_blks = 1 + 2 * maxops.cat * max_cat_height;

	/*
	 * Any new catalog node could require a new entry in the object map,
	 * because the original might belong to a snapshot.
	 */
	max_omap_blks = 1 + 2 * max_cat_blks * max_omap_height;

	/* The extent reference tree needs a maximum of one record per block */
	max_extref_blks = 1 + 2 * maxops.blks * max_extref_height;

	/*
	 * Ephemeral allocations shouldn't fail, and neither should those in the
	 * internal pool. So just add the actual file blocks and we are done.
	 */
	max_blks = max_cat_blks + max_omap_blks + max_extref_blks + maxops.blks;

	return max_blks < APFS_SM(sb)->sm_free_count;
}

/**
 * apfs_transaction_start - Begin a new transaction
 * @sb:		superblock structure
 * @maxops:	maximum operations expected
 *
 * Also locks the filesystem for writing; returns 0 on success or a negative
 * error code in case of failure.
 */
int apfs_transaction_start(struct super_block *sb, struct apfs_max_ops maxops)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_vol_transaction *vol_trans = &sbi->s_transaction;
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	int err;

	down_write(&nxi->nx_big_sem);
	mutex_lock(&nxs_mutex); /* Don't mount during a transaction */

	if (sb->s_flags & SB_RDONLY) {
		/* A previous transaction has failed; this should be rare */
		mutex_unlock(&nxs_mutex);
		up_write(&nxi->nx_big_sem);
		return -EROFS;
	}

	/* TODO: rethink this now that transactions shouldn't fail */
	if (!nx_trans->t_old_msb) {
		/* Backup the old superblock buffers in case the transaction fails */
		nx_trans->t_old_msb = nxi->nx_object.o_bh;
		get_bh(nx_trans->t_old_msb);

		++nxi->nx_xid;
		INIT_LIST_HEAD(&nx_trans->t_inodes);
		INIT_LIST_HEAD(&nx_trans->t_buffers);
		nx_trans->t_buffers_count = 0;
		nx_trans->t_starts_count = 0;

		err = apfs_checkpoint_start(sb);
		if (err) {
			apfs_err(sb, "failed to start a new checkpoint");
			goto fail;
		}

		err = apfs_read_spaceman(sb);
		if (err) {
			apfs_err(sb, "failed to read the spaceman");
			goto fail;
		}
	}

	/* Don't start transactions unless we are sure they fit in disk */
	if (!apfs_transaction_has_room(sb, maxops)) {
		/* Commit what we have so far to flush the queues */
		nx_trans->t_state |= APFS_NX_TRANS_FORCE_COMMIT;
		err = apfs_transaction_commit(sb);
		if (err) {
			apfs_err(sb, "commit failed");
			goto fail;
		}
		return -ENOSPC;
	}

	if (!vol_trans->t_old_vsb) {
		vol_trans->t_old_vsb = sbi->s_vobject.o_bh;
		get_bh(vol_trans->t_old_vsb);

		/* Backup the old tree roots; the node struct issues make this ugly */
		vol_trans->t_old_cat_root = *sbi->s_cat_root;
		get_bh(vol_trans->t_old_cat_root.object.o_bh);
		vol_trans->t_old_omap_root = *sbi->s_omap->omap_root;
		get_bh(vol_trans->t_old_omap_root.object.o_bh);

		err = apfs_map_volume_super(sb, true /* write */);
		if (err) {
			apfs_err(sb, "CoW failed for volume super");
			goto fail;
		}

		/* TODO: don't copy these nodes for transactions that don't use them */
		err = apfs_read_omap(sb, true /* write */);
		if (err) {
			apfs_err(sb, "CoW failed for omap");
			goto fail;
		}
		err = apfs_read_catalog(sb, true /* write */);
		if (err) {
			apfs_err(sb, "Cow failed for catalog");
			goto fail;
		}
	}

	nx_trans->t_starts_count++;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_end_buffer_write_sync - Clean up a buffer head just synced to disk
 * @bh:		the buffer head to clean
 * @uptodate:	has the write succeeded?
 */
static void apfs_end_buffer_write_sync(struct buffer_head *bh, int uptodate)
{
	struct page *page = NULL;
	bool must_unlock, is_metadata;

	page = bh->b_page;
	get_page(page);

	is_metadata = buffer_csum(bh);
	clear_buffer_csum(bh);
	end_buffer_write_sync(bh, uptodate);
	bh = NULL;

	/* Future writes to mmapped areas should fault for CoW */
	must_unlock = trylock_page(page);
	page_mkclean(page);

	/* XXX: otherwise, the page cache fills up and crashes the machine */
	if (!is_metadata) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
		try_to_free_buffers(page_folio(page));
#else
		try_to_free_buffers(page);
#endif
	}

	if (must_unlock)
		unlock_page(page);
	put_page(page);
}

/**
 * apfs_transaction_flush_all_inodes - Flush inode metadata to the buffer heads
 * @sb: superblock structure
 *
 * This messes a lot with the disk layout, so it must be called ahead of time
 * if we need it to be stable for the rest or the transaction (for example, if
 * we are setting up a snapshot).
 */
int apfs_transaction_flush_all_inodes(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	int err = 0, curr_err;

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(nx_trans->t_old_msb);

	while (!list_empty(&nx_trans->t_inodes)) {
		struct apfs_inode_info *ai = NULL;
		struct inode *inode = NULL;

		ai = list_first_entry(&nx_trans->t_inodes, struct apfs_inode_info, i_list);
		inode = &ai->vfs_inode;

		/* This is a bit wasteful if the inode will get deleted */
		curr_err = apfs_update_inode(inode, NULL /* new_name */);
		if (curr_err)
			err = curr_err;
		inode->i_state &= ~I_DIRTY_ALL;

		/*
		 * The same inode may get dirtied again as soon as we release
		 * the lock, and we don't want to miss that.
		 */
		list_del_init(&ai->i_list);

		nx_trans->t_state |= APFS_NX_TRANS_COMMITTING;
		mutex_unlock(&nxs_mutex);
		up_write(&nxi->nx_big_sem);

		/* Unlocked, so it may call ->evict_inode() */
		iput(inode);

		down_write(&nxi->nx_big_sem);
		mutex_lock(&nxs_mutex);
		nx_trans->t_state = 0;

		/* Transaction aborted by ->evict_inode(), error code is lost */
		if (sb->s_flags & SB_RDONLY) {
			apfs_err(sb, "abort during inode eviction");
			return -EROFS;
		}
	}

	return err;
}

/**
 * apfs_transaction_commit_nx - Definitely commit the current transaction
 * @sb: superblock structure
 */
static int apfs_transaction_commit_nx(struct super_block *sb)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_sb_info *sbi;
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	struct apfs_bh_info *bhi, *tmp;
	int err = 0;

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(nx_trans->t_old_msb);

	/* Before committing the bhs, write all inode metadata to them */
	err = apfs_transaction_flush_all_inodes(sb);
	if (err) {
		apfs_err(sb, "failed to flush all inodes");
		return err;
	}

	/*
	 * Now that nothing else will be freed, flush the last update to the
	 * free queues so that it can be committed to disk with the other bhs
	 */
	if (sm->sm_free_cache_base) {
		err = apfs_free_queue_insert_nocache(sb, sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
		if (err) {
			apfs_err(sb, "fq cache flush failed (0x%llx-0x%llx)", sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
			return err;
		}
		sm->sm_free_cache_base = sm->sm_free_cache_blkcnt = 0;
	}

	list_for_each_entry_safe(bhi, tmp, &nx_trans->t_buffers, list) {
		struct buffer_head *bh = bhi->bh;

		ASSERT(buffer_trans(bh));

		if (buffer_csum(bh))
			apfs_obj_set_csum(sb, (void *)bh->b_data);

		list_del(&bhi->list);
		clear_buffer_trans(bh);
		nx_trans->t_buffers_count--;

		bh->b_private = NULL;
		bhi->bh = NULL;
		kfree(bhi);
		bhi = NULL;

		clear_buffer_dirty(bh);
		lock_buffer(bh);
		bh->b_end_io = apfs_end_buffer_write_sync;
		apfs_submit_bh(REQ_OP_WRITE, REQ_SYNC, bh);
	}
	err = apfs_checkpoint_end(sb);
	if (err) {
		apfs_err(sb, "failed to end the checkpoint");
		return err;
	}

	/* Success: forget the old container and volume superblocks */
	brelse(nx_trans->t_old_msb);
	nx_trans->t_old_msb = NULL;

	list_for_each_entry(sbi, &nxi->vol_list, list) {
		struct apfs_vol_transaction *vol_trans = &sbi->s_transaction;
		if (!vol_trans->t_old_vsb)
			continue;

		brelse(vol_trans->t_old_vsb);
		vol_trans->t_old_vsb = NULL;

		/* XXX: forget the buffers for the b-tree roots */
		vol_trans->t_old_omap_root.object.data = NULL;
		brelse(vol_trans->t_old_omap_root.object.o_bh);
		vol_trans->t_old_omap_root.object.o_bh = NULL;
		vol_trans->t_old_cat_root.object.data = NULL;
		brelse(vol_trans->t_old_cat_root.object.o_bh);
		vol_trans->t_old_cat_root.object.o_bh = NULL;
	}

	brelse(APFS_SM(sb)->sm_ip);
	APFS_SM(sb)->sm_ip = NULL;
	brelse(APFS_SM(sb)->sm_bh);
	APFS_SM(sb)->sm_bh = NULL;
	return 0;
}

/**
 * apfs_transaction_need_commit - Evaluate if a commit is required
 * @sb: superblock structure
 */
static bool apfs_transaction_need_commit(struct super_block *sb)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;

	if (nx_trans->t_state & APFS_NX_TRANS_DEFER_COMMIT) {
		nx_trans->t_state &= ~APFS_NX_TRANS_DEFER_COMMIT;
		return false;
	}

	/* Avoid nested commits on ->evict_inode() */
	if (nx_trans->t_state & APFS_NX_TRANS_COMMITTING)
		return false;

	if (nx_trans->t_state & APFS_NX_TRANS_FORCE_COMMIT) {
		nx_trans->t_state = 0;
		return true;
	}

	if (sm) {
		struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
		struct apfs_spaceman_free_queue *fq_ip = &sm_raw->sm_fq[APFS_SFQ_IP];
		struct apfs_spaceman_free_queue *fq_main = &sm_raw->sm_fq[APFS_SFQ_MAIN];
		int buffers_max = TRANSACTION_BUFFERS_MAX;
		int starts_max = TRANSACTION_STARTS_MAX;
		int mq_max = TRANSACTION_MAIN_QUEUE_MAX;

		/*
		 * Try to avoid committing halfway through a data block write,
		 * otherwise the block will be put through copy-on-write again,
		 * causing unnecessary fragmentation.
		 */
		if (nx_trans->t_state & APFS_NX_TRANS_INCOMPLETE_BLOCK) {
			buffers_max += 50;
			starts_max += 50;
			mq_max += 20;
		}

		if(nx_trans->t_buffers_count > buffers_max)
			return true;
		if (nx_trans->t_starts_count > starts_max)
			return true;

		/*
		 * The internal pool has enough blocks to map the container
		 * exactly 3 times. Don't allow large transactions if we can't
		 * be sure the bitmap changes will all fit.
		 */
		if(le64_to_cpu(fq_ip->sfq_count) * 3 > le64_to_cpu(sm_raw->sm_ip_block_count))
			return true;

		/* Don't let the main queue get too full either */
		if(le64_to_cpu(fq_main->sfq_count) > mq_max)
			return true;
	}

	return false;
}

/**
 * apfs_transaction_commit - Possibly commit the current transaction
 * @sb: superblock structure
 *
 * On success returns 0 and releases the big filesystem lock. On failure,
 * returns a negative error code, and the caller is responsibly for aborting
 * the transaction.
 */
int apfs_transaction_commit(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	int err = 0;

	if (apfs_transaction_need_commit(sb)) {
		err = apfs_transaction_commit_nx(sb);
		if (err) {
			apfs_err(sb, "transaction commit failed");
			return err;
		}
	}

	mutex_unlock(&nxs_mutex);
	up_write(&nxi->nx_big_sem);
	return 0;
}

/**
 * apfs_inode_join_transaction - Add an inode to the current transaction
 * @sb:		superblock structure
 * @inode:	vfs inode to add
 */
void apfs_inode_join_transaction(struct super_block *sb, struct inode *inode)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	struct apfs_inode_info *ai = APFS_I(inode);

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(nx_trans->t_old_msb);
	lockdep_assert_held_write(&nxi->nx_big_sem);

	if (!list_empty(&ai->i_list)) /* Already in the transaction */
		return;

	ihold(inode);
	list_add(&ai->i_list, &nx_trans->t_inodes);
}

/**
 * apfs_transaction_join - Add a buffer head to the current transaction
 * @sb:	superblock structure
 * @bh:	the buffer head
 */
int apfs_transaction_join(struct super_block *sb, struct buffer_head *bh)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	struct apfs_bh_info *bhi;

	ASSERT(!(sb->s_flags & SB_RDONLY));
	ASSERT(nx_trans->t_old_msb);
	lockdep_assert_held_write(&nxi->nx_big_sem);

	if (buffer_trans(bh)) /* Already part of the only transaction */
		return 0;

	/* TODO: use a slab cache */
	bhi = kzalloc(sizeof(*bhi), GFP_NOFS);
	if (!bhi)
		return -ENOMEM;
	get_bh(bh);
	bhi->bh = bh;
	list_add(&bhi->list, &nx_trans->t_buffers);
	nx_trans->t_buffers_count ++;

	set_buffer_trans(bh);
	bh->b_private = bhi;
	return 0;
}

/**
 * apfs_force_readonly - Set the whole container as read-only
 * @nxi: container superblock info
 */
static void apfs_force_readonly(struct apfs_nxsb_info *nxi)
{
	struct apfs_sb_info *sbi = NULL;
	struct super_block *sb = NULL;

	list_for_each_entry(sbi, &nxi->vol_list, list) {
		sb = sbi->s_vobject.sb;
		sb->s_flags |= SB_RDONLY;
	}
	nxi->nx_flags &= ~APFS_READWRITE;
}

/**
 * apfs_transaction_abort - Abort the current transaction
 * @sb: superblock structure
 *
 * Releases the big filesystem lock and clears the in-memory transaction data;
 * the on-disk changes are irrelevant because the superblock checksum hasn't
 * been written yet. Leaves the filesystem in read-only state.
 */
void apfs_transaction_abort(struct super_block *sb)
{
	struct apfs_sb_info *sbi;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_transaction *nx_trans = &nxi->nx_transaction;
	struct apfs_bh_info *bhi, *tmp;
	struct apfs_inode_info *ai, *ai_tmp;

	if (sb->s_flags & SB_RDONLY) {
		/* Transaction already aborted, do nothing */
		ASSERT(!nx_trans->t_old_msb);
		ASSERT(list_empty(&nx_trans->t_inodes));
		ASSERT(list_empty(&nx_trans->t_buffers));
		mutex_unlock(&nxs_mutex);
		up_write(&nxi->nx_big_sem);
		return;
	}

	ASSERT(nx_trans->t_old_msb);
	nx_trans->t_state = 0;
	apfs_err(sb, "aborting transaction");

	--nxi->nx_xid;
	list_for_each_entry_safe(bhi, tmp, &nx_trans->t_buffers, list) {
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

	/* Restore the old container superblock */
	brelse(nxi->nx_object.o_bh);
	nxi->nx_object.o_bh = nx_trans->t_old_msb;
	nxi->nx_object.data = nxi->nx_object.o_bh->b_data;
	nxi->nx_object.block_nr = nx_trans->t_old_msb->b_blocknr;
	nxi->nx_raw = (void *)nx_trans->t_old_msb->b_data;
	nx_trans->t_old_msb = NULL;

	list_for_each_entry(sbi, &nxi->vol_list, list) {
		struct apfs_vol_transaction *vol_trans = &sbi->s_transaction;
		if (!vol_trans->t_old_vsb)
			continue;

		/* Restore volume state for all volumes */
		brelse(sbi->s_vobject.o_bh);
		sbi->s_vobject.o_bh = vol_trans->t_old_vsb;
		sbi->s_vobject.data = sbi->s_vobject.o_bh->b_data;
		sbi->s_vobject.block_nr = vol_trans->t_old_vsb->b_blocknr;
		sbi->s_vsb_raw = (void *)vol_trans->t_old_vsb->b_data;
		vol_trans->t_old_vsb = NULL;

		/* XXX: restore the old b-tree root nodes */
		brelse(sbi->s_omap->omap_root->object.o_bh);
		*(sbi->s_omap->omap_root) = vol_trans->t_old_omap_root;
		vol_trans->t_old_omap_root.object.o_bh = NULL;
		vol_trans->t_old_omap_root.object.data = NULL;
		brelse(sbi->s_cat_root->object.o_bh);
		*(sbi->s_cat_root) = vol_trans->t_old_cat_root;
		vol_trans->t_old_cat_root.object.o_bh = NULL;
		vol_trans->t_old_cat_root.object.data = NULL;
	}

	brelse(APFS_SM(sb)->sm_ip);
	APFS_SM(sb)->sm_ip = NULL;
	brelse(APFS_SM(sb)->sm_bh);
	APFS_SM(sb)->sm_bh = NULL;

	/*
	 * It's not possible to undo in-memory changes from old operations in
	 * the aborted transaction. To avoid corruption, never write again.
	 */
	apfs_force_readonly(nxi);

	mutex_unlock(&nxs_mutex);
	up_write(&nxi->nx_big_sem);

	/* ->evict_inode() will just fail if it starts a new transaction */
	list_for_each_entry_safe(ai, ai_tmp, &nx_trans->t_inodes, i_list) {
		list_del_init(&ai->i_list);
		iput(&ai->vfs_inode);
	}
}
