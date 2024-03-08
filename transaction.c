// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/blkdev.h>
#include <linux/rmap.h>
#include "apfs.h"

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
	struct buffer_head *bh = NULL;
	struct inode *bdev_inode = nxi->nx_bdev->bd_inode;
	struct address_space *bdev_map = bdev_inode->i_mapping;
	int err;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	bh = apfs_getblk(sb, nxi->nx_bno);
	if (!bh) {
		apfs_err(sb, "failed to map new checkpoint superblock");
		return -EIO;
	}
	obj->o_xid = cpu_to_le64(nxi->nx_xid);
	apfs_obj_set_csum(sb, obj);
	memcpy(bh->b_data, obj, sb->s_blocksize);

	err = filemap_write_and_wait(bdev_map);
	if (err)
		goto out;

	mark_buffer_dirty(bh);
	err = sync_dirty_buffer(bh);
	if (err)
		goto out;

	err = filemap_write_and_wait(bdev_map);
out:
	brelse(bh);
	bh = NULL;
	return err;
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
 * apfs_read_single_ephemeral_object - Read a single ephemeral object to memory
 * @sb:		filesystem superblock
 * @map:	checkpoint mapping for the object
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_read_single_ephemeral_object(struct super_block *sb, struct apfs_checkpoint_mapping *map)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_ephemeral_object_info *list = NULL;
	struct buffer_head *bh = NULL;
	char *object = NULL;
	int count;
	u32 size;
	u64 bno, oid;
	int err, i;

	list = nxi->nx_eph_list;
	count = nxi->nx_eph_count;
	if (count >= APFS_EPHEMERAL_LIST_LIMIT) {
		apfs_err(sb, "too many ephemeral objects?");
		return -EOPNOTSUPP;
	}

	bno = le64_to_cpu(map->cpm_paddr);
	oid = le64_to_cpu(map->cpm_oid);
	size = le32_to_cpu(map->cpm_size);
	if (size > sb->s_blocksize << 1) {
		/*
		 * No reason not to support bigger objects, but there has to be
		 * a limit somewhere and this is all I've seen so far.
		 */
		apfs_warn(sb, "ephemeral object has more than 2 blocks");
		return -EOPNOTSUPP;
	}
	if (!size || (size & (sb->s_blocksize - 1))) {
		apfs_err(sb, "invalid object size (0x%x)", size);
		return -EFSCORRUPTED;
	}
	object = kmalloc(size, GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	for (i = 0; i < size >> sb->s_blocksize_bits; ++i) {
		bh = apfs_sb_bread(sb, bno + i);
		if (!bh) {
			apfs_err(sb, "failed to read ephemeral block");
			err = -EIO;
			goto fail;
		}
		memcpy(object + (i << sb->s_blocksize_bits), bh->b_data, sb->s_blocksize);
		brelse(bh);
		bh = NULL;
	}

	/*
	 * The official reference requires that we always verify ephemeral
	 * checksums on mount, so do it even if the user didn't ask. We should
	 * actually try to mount an older checkpoint when this fails (TODO),
	 * which I guess means that the official driver writes all checkpoint
	 * blocks at once, instead of leaving the superblock for last like we
	 * do.
	 */
	if (!apfs_multiblock_verify_csum(object, size)) {
		apfs_err(sb, "bad checksum for ephemeral object 0x%llx", oid);
		err = -EFSBADCRC;
		goto fail;
	}

	list[count].oid = oid;
	list[count].size = size;
	list[count].object = object;
	object = NULL;
	nxi->nx_eph_count = count + 1;
	return 0;

fail:
	kfree(object);
	object = NULL;
	return err;
}

/**
 * apfs_read_single_cpm_block - Read all ephemeral objects in a cpm block
 * @sb:		filesystem superblock
 * @cpm_bno:	block number for the cpm block
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_read_single_cpm_block(struct super_block *sb, u64 cpm_bno)
{
	struct buffer_head *bh = NULL;
	struct apfs_checkpoint_map_phys *cpm = NULL;
	u32 map_count;
	int err, i;

	bh = apfs_sb_bread(sb, cpm_bno);
	if (!bh) {
		apfs_err(sb, "failed to read cpm block");
		return -EIO;
	}
	if (!apfs_obj_verify_csum(sb, bh)) {
		/*
		 * The reference seems to imply that we need to check these on
		 * mount, and retry an older checkpoint on failure (TODO).
		 */
		apfs_err(sb, "bad checksum for cpm block at 0x%llx", cpm_bno);
		err = -EFSBADCRC;
		goto out;
	}
	cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;

	map_count = le32_to_cpu(cpm->cpm_count);
	if (map_count > apfs_max_maps_per_block(sb)) {
		apfs_err(sb, "block has too many maps (%d)", map_count);
		err = -EFSCORRUPTED;
		goto out;
	}

	for (i = 0; i < map_count; ++i) {
		err = apfs_read_single_ephemeral_object(sb, &cpm->cpm_map[i]);
		if (err) {
			apfs_err(sb, "failed to read ephemeral object %u", i);
			goto out;
		}
	}

out:
	brelse(bh);
	cpm = NULL;
	return err;
}

/**
 * apfs_read_ephemeral_objects - Read all ephemeral objects to memory
 * @sb:	superblock structure
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_read_ephemeral_objects(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	u64 desc_base;
	u32 desc_index, desc_blks, desc_len, i;
	int err;

	if (nxi->nx_eph_list) {
		apfs_alert(sb, "attempt to reread ephemeral object list");
		return -EFSCORRUPTED;
	}
	nxi->nx_eph_list = kzalloc(APFS_EPHEMERAL_LIST_SIZE, GFP_KERNEL);
	if (!nxi->nx_eph_list)
		return -ENOMEM;
	nxi->nx_eph_count = 0;

	desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);

	/* Last block in the area is superblock; the rest are mapping blocks */
	for (i = 0; i < desc_len - 1; ++i) {
		u64 cpm_bno = desc_base + (desc_index + i) % desc_blks;

		err = apfs_read_single_cpm_block(sb, cpm_bno);
		if (err) {
			apfs_err(sb, "failed to read cpm block %u", i);
			return err;
		}
	}
	return 0;
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

	/*
	 * Ephemeral objects are read only once, kept in memory, and committed
	 * to disk along with each transaction.
	 */
	if (!nxi->nx_eph_list) {
		err = apfs_read_ephemeral_objects(sb);
		if (err) {
			mutex_unlock(&nxs_mutex);
			up_write(&nxi->nx_big_sem);
			apfs_err(sb, "failed to read the ephemeral objects");
			return err;
		}
	}

	if (nx_trans->t_starts_count == 0) {
		++nxi->nx_xid;
		nxi->nx_raw->nx_next_xid = cpu_to_le64(nxi->nx_xid + 1);

		INIT_LIST_HEAD(&nx_trans->t_inodes);
		INIT_LIST_HEAD(&nx_trans->t_buffers);

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

		/* Unlocked, so it may call evict() and wait for writeback */
		iput(inode);

		down_write(&nxi->nx_big_sem);
		mutex_lock(&nxs_mutex);
		nx_trans->t_state = 0;

		/* Transaction aborted during writeback, error code is lost */
		if (sb->s_flags & SB_RDONLY) {
			apfs_err(sb, "abort during inode writeback");
			return -EROFS;
		}
	}

	return err;
}

/**
 * apfs_write_single_ephemeral_object - Write a single ephemeral object to bh's
 * @sb:		filesystem superblock
 * @obj_raw:	contents of the object
 * @map:	checkpoint mapping for the object, already updated
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_write_single_ephemeral_object(struct super_block *sb, struct apfs_obj_phys *obj_raw, const struct apfs_checkpoint_mapping *map)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct buffer_head *bh = NULL;
	u64 bno;
	u32 size;
	int err, i;

	bno = le64_to_cpu(map->cpm_paddr);
	size = le32_to_cpu(map->cpm_size);
	obj_raw->o_xid = cpu_to_le64(nxi->nx_xid);
	apfs_multiblock_set_csum((char *)obj_raw, size);

	for (i = 0; i < size >> sb->s_blocksize_bits; ++i) {
		bh = apfs_getblk(sb, bno + i);
		if (!bh) {
			apfs_err(sb, "failed to map ephemeral block");
			return -EIO;
		}
		err = apfs_transaction_join(sb, bh);
		if (err) {
			brelse(bh);
			bh = NULL;
			return err;
		}
		memcpy(bh->b_data, (char *)obj_raw + (i << sb->s_blocksize_bits), sb->s_blocksize);
		brelse(bh);
		bh = NULL;
	}
	return 0;
}

/**
 * apfs_write_ephemeral_objects - Write all ephemeral objects to bh's
 * @sb: filesystem superblock
 *
 * Returns 0 on sucess, or a negative error code in case of failure.
 */
static int apfs_write_ephemeral_objects(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	struct apfs_checkpoint_map_phys *cpm = NULL;
	struct buffer_head *cpm_bh = NULL;
	struct apfs_ephemeral_object_info *eph_info = NULL;
	u64 cpm_bno;
	u64 desc_base, data_base;
	u32 desc_index, desc_blks, desc_len, desc_next;
	u32 data_index, data_blks, data_len, data_next;
	u32 desc_limit, data_limit;
	u32 obj_blkcnt;
	int err, i, cpm_start;

	if (!nxi->nx_eph_list) {
		apfs_alert(sb, "missing ephemeral object list");
		return -EFSCORRUPTED;
	}

	desc_next = le32_to_cpu(raw_sb->nx_xp_desc_next);
	desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	desc_index = desc_next; /* New checkpoint */
	desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	desc_len = 0; /* For now */

	data_next = le32_to_cpu(raw_sb->nx_xp_data_next);
	data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	data_index = data_next; /* New checkpoint */
	data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	data_len = 0; /* For now */

	/*
	 * The reference doesn't mention anything about this, but I need to
	 * put some sort of a limit or else the rings could wrap around and
	 * corrupt themselves.
	 */
	desc_limit = desc_blks >> 2;
	data_limit = data_blks >> 2;

	for (i = 0; i < nxi->nx_eph_count; ++i) {
		if (data_len == data_limit) {
			apfs_err(sb, "too many checkpoint data blocks");
			return -EFSCORRUPTED;
		}

		if (!cpm) {
			cpm_start = i;
			if (desc_len == desc_limit) {
				apfs_err(sb, "too many checkpoint descriptor blocks");
				return -EFSCORRUPTED;
			}
			cpm_bno = desc_base + (desc_index + desc_len) % desc_blks;
			err = apfs_create_cpm_block(sb, cpm_bno, &cpm_bh);
			if (err) {
				apfs_err(sb, "failed to create cpm block");
				return err;
			}
			cpm = (void *)cpm_bh->b_data;
			desc_len += 1;
		}

		eph_info = &nxi->nx_eph_list[i];
		data_next = (data_index + data_len) % data_blks;
		obj_blkcnt = eph_info->size >> sb->s_blocksize_bits;
		if (obj_blkcnt > data_blks - data_next) {
			/*
			 * This multiblock object does not fit in what's left
			 * of the ring buffer, so move it to the beginning and
			 * leave some empty blocks.
			 */
			data_len += data_blks - data_next;
			data_next = 0;
		}

		err = apfs_create_cpoint_map(sb, cpm, eph_info->object, data_base + data_next, eph_info->size);
		if (err) {
			if (err == -ENOSPC)
				cpm->cpm_flags = 0; /* No longer the last */
			brelse(cpm_bh);
			cpm = NULL;
			cpm_bh = NULL;
			if (err == -ENOSPC) {
				--i;
				continue;
			}
			apfs_err(sb, "failed to create cpm map %d", i);
			return err;
		}
		err = apfs_write_single_ephemeral_object(sb, eph_info->object, &cpm->cpm_map[i - cpm_start]);
		if (err) {
			brelse(cpm_bh);
			cpm = NULL;
			cpm_bh = NULL;
			apfs_err(sb, "failed to write ephemeral object %d", i);
			return err;
		}
		data_len += obj_blkcnt;
	}

	/*
	 * The checkpoint superblock can't be set until the very end of the
	 * transaction commit, but allocate its block here already.
	 */
	nxi->nx_bno = desc_base + (desc_index + desc_len) % desc_blks;
	desc_len += 1;

	desc_next = (desc_index + desc_len) % desc_blks;
	data_next = (data_index + data_len) % data_blks;

	raw_sb->nx_xp_desc_next = cpu_to_le32(desc_next);
	raw_sb->nx_xp_desc_index = cpu_to_le32(desc_index);
	raw_sb->nx_xp_desc_len = cpu_to_le32(desc_len);

	raw_sb->nx_xp_data_next = cpu_to_le32(data_next);
	raw_sb->nx_xp_data_index = cpu_to_le32(data_index);
	raw_sb->nx_xp_data_len = cpu_to_le32(data_len);

	return 0;
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
	u32 bmap_idx;

	ASSERT(!(sb->s_flags & SB_RDONLY));

	/* Before committing the bhs, write all inode metadata to them */
	err = apfs_transaction_flush_all_inodes(sb);
	if (err) {
		apfs_err(sb, "failed to flush all inodes");
		return err;
	}

	/*
	 * Now that nothing else will be freed, flush the last update to the
	 * free queues so that it can be committed to disk along with all the
	 * ephemeral objects.
	 */
	if (sm->sm_free_cache_base) {
		err = apfs_free_queue_insert_nocache(sb, sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
		if (err) {
			apfs_err(sb, "fq cache flush failed (0x%llx-0x%llx)", sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
			return err;
		}
		sm->sm_free_cache_base = sm->sm_free_cache_blkcnt = 0;
	}
	err = apfs_write_ephemeral_objects(sb);
	if (err)
		return err;

	list_for_each_entry(bhi, &nx_trans->t_buffers, list) {
		struct buffer_head *bh = bhi->bh;

		ASSERT(buffer_trans(bh));

		if (buffer_csum(bh))
			apfs_obj_set_csum(sb, (void *)bh->b_data);

		clear_buffer_dirty(bh);
		get_bh(bh);
		lock_buffer(bh);
		bh->b_end_io = end_buffer_write_sync;
		apfs_submit_bh(REQ_OP_WRITE, REQ_SYNC, bh);
	}
	list_for_each_entry_safe(bhi, tmp, &nx_trans->t_buffers, list) {
		struct buffer_head *bh = bhi->bh;
		struct page *page = NULL;
		bool is_metadata;

		ASSERT(buffer_trans(bh));

		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			apfs_err(sb, "failed to write some blocks");
			return -EIO;
		}

		list_del(&bhi->list);
		clear_buffer_trans(bh);
		nx_trans->t_buffers_count--;

		bh->b_private = NULL;
		bhi->bh = NULL;
		kfree(bhi);
		bhi = NULL;

		page = bh->b_page;
		get_page(page);

		is_metadata = buffer_csum(bh);
		clear_buffer_csum(bh);
		put_bh(bh);
		bh = NULL;

		/* Future writes to mmapped areas should fault for CoW */
		lock_page(page);
		page_mkclean(page);
		/* XXX: otherwise, the page cache fills up and crashes the machine */
		if (!is_metadata) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
			try_to_free_buffers(page_folio(page));
#else
			try_to_free_buffers(page);
#endif
		}
		unlock_page(page);
		put_page(page);
	}
	err = apfs_checkpoint_end(sb);
	if (err) {
		apfs_err(sb, "failed to end the checkpoint");
		return err;
	}

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

	for (bmap_idx = 0; bmap_idx < APFS_SM(sb)->sm_ip_bmaps_count; ++bmap_idx) {
		brelse(APFS_SM(sb)->sm_ip_bmaps[bmap_idx]);
		APFS_SM(sb)->sm_ip_bmaps[bmap_idx] = NULL;
	}
	APFS_SM(sb)->sm_raw = NULL;

	nx_trans->t_starts_count = 0;
	nx_trans->t_buffers_count = 0;
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

	/* Avoid nested commits on inode writeback */
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
		int buffers_max = APFS_SB(sb)->s_trans_buffers_max;
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

		if (nx_trans->t_buffers_count > buffers_max)
			return true;
		if (nx_trans->t_starts_count > starts_max)
			return true;

		/*
		 * The internal pool has enough blocks to map the container
		 * exactly 3 times. Don't allow large transactions if we can't
		 * be sure the bitmap changes will all fit.
		 */
		if (le64_to_cpu(fq_ip->sfq_count) * 3 > le64_to_cpu(sm_raw->sm_ip_block_count))
			return true;

		/* Don't let the main queue get too full either */
		if (le64_to_cpu(fq_main->sfq_count) > mq_max)
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
	nx_trans->t_buffers_count++;

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
	struct apfs_spaceman *sm = NULL;
	u32 bmap_idx;

	if (sb->s_flags & SB_RDONLY) {
		/* Transaction already aborted, do nothing */
		ASSERT(list_empty(&nx_trans->t_inodes));
		ASSERT(list_empty(&nx_trans->t_buffers));
		mutex_unlock(&nxs_mutex);
		up_write(&nxi->nx_big_sem);
		return;
	}

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

	/*
	 * TODO: get rid of all this stuff, it makes little sense. Maybe do an
	 * actual read-only remount?
	 */
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

	sm = APFS_SM(sb);
	if (sm) {
		for (bmap_idx = 0; bmap_idx < sm->sm_ip_bmaps_count; ++bmap_idx) {
			brelse(sm->sm_ip_bmaps[bmap_idx]);
			sm->sm_ip_bmaps[bmap_idx] = NULL;
		}
		APFS_SM(sb)->sm_raw = NULL;
	}

	/*
	 * It's not possible to undo in-memory changes from old operations in
	 * the aborted transaction. To avoid corruption, never write again.
	 */
	apfs_force_readonly(nxi);

	mutex_unlock(&nxs_mutex);
	up_write(&nxi->nx_big_sem);

	list_for_each_entry_safe(ai, ai_tmp, &nx_trans->t_inodes, i_list) {
		list_del_init(&ai->i_list);
		iput(&ai->vfs_inode);
	}
}
