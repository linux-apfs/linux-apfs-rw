// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"

/**
 * apfs_ext_is_hole - Does this extent represent a hole in a sparse file?
 * @extent: the extent to check
 */
static inline bool apfs_ext_is_hole(struct apfs_file_extent *extent)
{
	return extent->phys_block_num == 0;
}

/**
 * apfs_extent_from_query - Read the extent found by a successful query
 * @query:	the query that found the record
 * @extent:	Return parameter.  The extent found.
 *
 * Reads the extent record into @extent and performs some basic sanity checks
 * as a protection against crafted filesystems.  Returns 0 on success or
 * -EFSCORRUPTED otherwise.
 */
int apfs_extent_from_query(struct apfs_query *query,
			   struct apfs_file_extent *extent)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_file_extent_val *ext;
	struct apfs_file_extent_key *ext_key;
	char *raw = query->node->object.data;
	u64 ext_len;

	if (query->len != sizeof(*ext) || query->key_len != sizeof(*ext_key))
		return -EFSCORRUPTED;

	ext = (struct apfs_file_extent_val *)(raw + query->off);
	ext_key = (struct apfs_file_extent_key *)(raw + query->key_off);
	ext_len = le64_to_cpu(ext->len_and_flags) & APFS_FILE_EXTENT_LEN_MASK;

	/* Extent length must be a multiple of the block size */
	if (ext_len & (sb->s_blocksize - 1))
		return -EFSCORRUPTED;

	extent->logical_addr = le64_to_cpu(ext_key->logical_addr);
	extent->phys_block_num = le64_to_cpu(ext->phys_block_num);
	extent->len = ext_len;
	extent->crypto_id = le64_to_cpu(ext->crypto_id);
	return 0;
}

/**
 * apfs_extent_read - Read the extent record that covers a block
 * @dstream:	data stream info
 * @dsblock:	logical number of the wanted block
 * @extent:	Return parameter.  The extent found.
 *
 * Finds and caches the extent record.  On success, returns a pointer to the
 * cache record; on failure, returns an error code.
 */
static int apfs_extent_read(struct apfs_dstream_info *dstream, sector_t dsblock,
			    struct apfs_file_extent *extent)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent *cache = &dstream->ds_cached_ext;
	u64 iaddr = dsblock << sb->s_blocksize_bits;
	int ret = 0;

	spin_lock(&dstream->ds_ext_lock);
	if (iaddr >= cache->logical_addr &&
	    iaddr < cache->logical_addr + cache->len) {
		*extent = *cache;
		spin_unlock(&dstream->ds_ext_lock);
		return 0;
	}
	spin_unlock(&dstream->ds_ext_lock);

	/* We will search for the extent that covers iblock */
	apfs_init_file_extent_key(dstream->ds_id, iaddr, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto done;

	ret = apfs_extent_from_query(query, extent);
	if (ret) {
		apfs_alert(sb, "bad extent record for dstream 0x%llx", dstream->ds_id);
		goto done;
	}

	/*
	 * For now prioritize the deferral of writes.
	 * i_extent_dirty is protected by the read semaphore.
	 */
	if (!dstream->ds_ext_dirty) {
		spin_lock(&dstream->ds_ext_lock);
		*cache = *extent;
		spin_unlock(&dstream->ds_ext_lock);
	}

done:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_logic_to_phys_bno - Find the physical block number for a dstream block
 * @dstream:	data stream info
 * @dsblock:	logical number of the wanted block
 * @bno:	on return, the physical block number (or zero for holes)
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_logic_to_phys_bno(struct apfs_dstream_info *dstream, sector_t dsblock, u64 *bno)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_file_extent ext;
	u64 blk_off;
	int ret;

	ret = apfs_extent_read(dstream, dsblock, &ext);
	if (ret)
		return ret;

	if (apfs_ext_is_hole(&ext)) {
		*bno = 0;
		return 0;
	}

	/* Find the block offset of iblock within the extent */
	blk_off = dsblock - (ext.logical_addr >> sb->s_blocksize_bits);
	*bno = ext.phys_block_num + blk_off;
	return 0;
}

/* This does the same as apfs_get_block(), but without taking any locks */
int __apfs_get_block(struct apfs_dstream_info *dstream, sector_t dsblock,
		     struct buffer_head *bh_result, int create)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_file_extent ext;
	u64 blk_off, bno, map_len;
	int ret;

	ASSERT(!create);

	ret = apfs_extent_read(dstream, dsblock, &ext);
	if (ret)
		return ret;

	/* Find the block offset of iblock within the extent */
	blk_off = dsblock - (ext.logical_addr >> sb->s_blocksize_bits);

	/* Make sure we don't read past the extent boundaries */
	map_len = ext.len - (blk_off << sb->s_blocksize_bits);
	if (bh_result->b_size > map_len)
		bh_result->b_size = map_len;

	/*
	 * Save the requested mapping length as apfs_map_bh() replaces it with
	 * the filesystem block size
	 */
	map_len = bh_result->b_size;
	/* Extents representing holes have block number 0 */
	if (!apfs_ext_is_hole(&ext)) {
		/* Find the block number of iblock within the disk */
		bno = ext.phys_block_num + blk_off;
		apfs_map_bh(bh_result, sb, bno);
	}
	bh_result->b_size = map_len;
	return 0;
}

int apfs_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(inode->i_sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	int ret;

	down_read(&nxi->nx_big_sem);
	ret = __apfs_get_block(&ai->i_dstream, iblock, bh_result, create);
	up_read(&nxi->nx_big_sem);
	return ret;
}

static int apfs_delete_phys_extent(struct super_block *sb, const struct apfs_file_extent *extent);

/**
 * apfs_set_extent_length - Set a new length in an extent record's value
 * @ext: the extent record's value
 * @len: the new length
 *
 * Preserves the flags, though none are defined yet and I don't know if that
 * will ever be important.
 */
static inline void apfs_set_extent_length(struct apfs_file_extent_val *ext, u64 len)
{
	u64 len_and_flags = le64_to_cpu(ext->len_and_flags);
	u64 flags = len_and_flags & APFS_FILE_EXTENT_FLAG_MASK;

	ext->len_and_flags = cpu_to_le64(flags | len);
}

/**
 * apfs_shrink_extent_head - Shrink an extent record in its head
 * @query:	the query that found the record
 * @dstream:	data stream info
 * @start:	new logical start for the extent
 *
 * Also deletes the physical extent records for the head. Returns 0 on success
 * or a negative error code in case of failure.
 */
static int apfs_shrink_extent_head(struct apfs_query *query, struct apfs_dstream_info *dstream, u64 start)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_file_extent_key key;
	struct apfs_file_extent_val val;
	struct apfs_file_extent extent;
	u64 new_len, head_len;
	void *raw = NULL;
	int err = 0;

	err = apfs_extent_from_query(query, &extent);
	if (err)
		return err;
	raw = query->node->object.data;
	key = *(struct apfs_file_extent_key *)(raw + query->key_off);
	val = *(struct apfs_file_extent_val *)(raw + query->off);

	new_len = extent.logical_addr + extent.len - start;
	head_len = extent.len - new_len;

	/* Delete the physical records for the blocks lost in the shrinkage */
	if (!apfs_ext_is_hole(&extent)) {
		struct apfs_file_extent head = {0};

		head.phys_block_num = extent.phys_block_num;
		head.len = head_len;
		err = apfs_delete_phys_extent(sb, &head);
		if (err)
			return err;
	} else {
		dstream->ds_sparse_bytes -= head_len;
	}

	/* This is the actual shrinkage of the logical extent */
	key.logical_addr = cpu_to_le64(start);
	apfs_set_extent_length(&val, new_len);
	if (!apfs_ext_is_hole(&extent))
		le64_add_cpu(&val.phys_block_num, head_len >> sb->s_blocksize_bits);
	return apfs_btree_replace(query, &key, sizeof(key), &val, sizeof(val));
}

/**
 * apfs_shrink_extent_tail - Shrink an extent record in its tail
 * @query:	the query that found the record
 * @dstream:	data stream info
 * @end:	new logical end for the extent
 *
 * Also deletes the physical extent records for the tail. Returns 0 on success
 * or a negative error code in case of failure.
 */
static int apfs_shrink_extent_tail(struct apfs_query *query, struct apfs_dstream_info *dstream, u64 end)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_file_extent_val *val;
	struct apfs_file_extent extent;
	u64 new_len, new_blkcount, tail_len;
	void *raw;
	int err = 0;

	ASSERT((end & (sb->s_blocksize - 1)) == 0);

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.data;

	err = apfs_extent_from_query(query, &extent);
	if (err)
		return err;
	val = raw + query->off;

	new_len = end - extent.logical_addr;
	new_blkcount = new_len >> sb->s_blocksize_bits;
	tail_len = extent.len - new_len;

	/* Delete the physical records for the blocks lost in the shrinkage */
	if (!apfs_ext_is_hole(&extent)) {
		struct apfs_file_extent tail = {0};

		tail.phys_block_num = extent.phys_block_num + new_blkcount;
		tail.len = tail_len;
		err = apfs_delete_phys_extent(sb, &tail);
		if (err)
			return err;
	} else {
		dstream->ds_sparse_bytes -= tail_len;
	}

	/* This is the actual shrinkage of the logical extent */
	apfs_set_extent_length(val, new_len);
	return err;
}

/**
 * apfs_query_found_extent - Is this query pointing to an extent record?
 * @query: the (successful) query that found the record
 */
static inline bool apfs_query_found_extent(struct apfs_query *query)
{
	void *raw = query->node->object.data;
	struct apfs_key_header *hdr;

	if (query->key_len < sizeof(*hdr))
		return false;
	hdr = raw + query->key_off;
	return apfs_cat_type(hdr) == APFS_TYPE_FILE_EXTENT;
}

/**
 * apfs_update_tail_extent - Grow the tail extent for a data stream
 * @dstream:	data stream info
 * @extent:	new in-memory extent
 *
 * Also takes care of any needed changes to the physical extent records. Returns
 * 0 on success or a negative error code in case of failure.
 */
static int apfs_update_tail_extent(struct apfs_dstream_info *dstream, const struct apfs_file_extent *extent)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent_key raw_key;
	struct apfs_file_extent_val raw_val;
	u64 extent_id = dstream->ds_id;
	int ret;
	u64 new_crypto;

	apfs_key_set_hdr(APFS_TYPE_FILE_EXTENT, extent_id, &raw_key);
	raw_key.logical_addr = cpu_to_le64(extent->logical_addr);
	raw_val.len_and_flags = cpu_to_le64(extent->len);
	raw_val.phys_block_num = cpu_to_le64(extent->phys_block_num);
	if (apfs_vol_is_encrypted(sb))
		new_crypto = extent_id;
	else
		new_crypto = 0;
	raw_val.crypto_id = cpu_to_le64(new_crypto);

	/* We want the last extent record */
	apfs_init_file_extent_key(extent_id, -1, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto out;

	if (ret == -ENODATA || !apfs_query_found_extent(query)) {
		/* We are creting the first extent for the file */
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret)
			goto out;
	} else {
		struct apfs_file_extent tail;

		ret = apfs_extent_from_query(query, &tail);
		if (ret)
			goto out;

		if (tail.logical_addr > extent->logical_addr) {
			ret = -EOPNOTSUPP;
			goto out;
		} else if (tail.logical_addr == extent->logical_addr) {
			ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
			if (ret)
				goto out;
			if (apfs_ext_is_hole(&tail)) {
				dstream->ds_sparse_bytes -= tail.len;
			} else if (tail.phys_block_num != extent->phys_block_num) {
				ret = apfs_delete_phys_extent(sb, &tail);
				if (ret)
					goto out;
			}
			if (new_crypto == tail.crypto_id)
				goto out;
			ret = apfs_crypto_adj_refcnt(sb, tail.crypto_id, -1);
			if (ret)
				goto out;
		} else {
			/*
			 * TODO: we could actually also continue the tail extent
			 * if it's right next to the new one (both logically and
			 * physically), even if they don't overlap. Or maybe we
			 * should always make sure that the tail extent is in
			 * the cache before a write...
			 */
			if (extent->logical_addr < tail.logical_addr + tail.len) {
				ret = apfs_shrink_extent_tail(query, dstream, extent->logical_addr);
				if (ret)
					goto out;
			}
			ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
			if (ret)
				goto out;
		}
	}

	ret = apfs_crypto_adj_refcnt(sb, new_crypto, 1);

out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_split_extent - Break an extent in two
 * @query:	query pointing to the extent
 * @div:	logical address for the division
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_split_extent(struct apfs_query *query, u64 div)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_file_extent_val *val1;
	struct apfs_file_extent_key key2;
	struct apfs_file_extent_val val2;
	struct apfs_file_extent extent;
	u64 len1, len2, blkcount1;
	void *raw;
	int err = 0;

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.data;

	err = apfs_extent_from_query(query, &extent);
	if (err)
		return err;
	val1 = raw + query->off;
	val2 = *(struct apfs_file_extent_val *)(raw + query->off);
	key2 = *(struct apfs_file_extent_key *)(raw + query->key_off);

	len1 = div - extent.logical_addr;
	blkcount1 = len1 >> sb->s_blocksize_bits;
	len2 = extent.len - len1;

	/* Modify the current extent in place to become the first half */
	apfs_set_extent_length(val1, len1);

	/* Insert the second half right after the first */
	key2.logical_addr = cpu_to_le64(div);
	if (!apfs_ext_is_hole(&extent))
		val2.phys_block_num = cpu_to_le64(extent.phys_block_num + blkcount1);
	apfs_set_extent_length(&val2, len2);
	err = apfs_btree_insert(query, &key2, sizeof(key2), &val2, sizeof(val2));
	if (err)
		return err;

	return apfs_crypto_adj_refcnt(sb, extent.crypto_id, 1);
}

/**
 * apfs_update_mid_extent - Create or update a non-tail extent for a dstream
 * @dstream:	data stream info
 * @extent:	new in-memory extent
 *
 * Also takes care of any needed changes to the physical extent records. Returns
 * 0 on success or a negative error code in case of failure.
 */
static int apfs_update_mid_extent(struct apfs_dstream_info *dstream, const struct apfs_file_extent *extent)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent_key raw_key;
	struct apfs_file_extent_val raw_val;
	struct apfs_file_extent prev_ext;
	u64 extent_id = dstream->ds_id;
	u64 prev_crypto, new_crypto;
	u64 prev_start, prev_end;
	bool second_run = false;
	int ret;

	apfs_key_set_hdr(APFS_TYPE_FILE_EXTENT, extent_id, &raw_key);
	raw_key.logical_addr = cpu_to_le64(extent->logical_addr);
	raw_val.len_and_flags = cpu_to_le64(extent->len);
	raw_val.phys_block_num = cpu_to_le64(extent->phys_block_num);
	if(apfs_vol_is_encrypted(sb))
		new_crypto = extent_id;
	else
		new_crypto = 0;
	raw_val.crypto_id = cpu_to_le64(new_crypto);

	apfs_init_file_extent_key(extent_id, extent->logical_addr, &key);

search_and_insert:
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto out;

	if (ret == -ENODATA || !apfs_query_found_extent(query)) {
		/*
		 * The new extent goes in a hole we just made, right at the
		 * beginning of the file.
		 */
		if (!second_run) {
			apfs_alert(sb, "missing extent on dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
		} else {
			ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		}
		goto out;
	}

	if (apfs_extent_from_query(query, &prev_ext)) {
		apfs_alert(sb, "bad mid extent record on dstream 0x%llx", extent_id);
		ret = -EFSCORRUPTED;
		goto out;
	}
	prev_crypto = prev_ext.crypto_id;
	prev_start = prev_ext.logical_addr;
	prev_end = prev_ext.logical_addr + prev_ext.len;

	if (prev_end == extent->logical_addr && second_run) {
		/* The new extent goes in the hole we just made */
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret)
			goto out;
	} else if (prev_start == extent->logical_addr && prev_ext.len == extent->len) {
		/* The old and new extents are the same logical block */
		ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret)
			goto out;
		if (apfs_ext_is_hole(&prev_ext)) {
			dstream->ds_sparse_bytes -= prev_ext.len;
		} else if (prev_ext.phys_block_num != extent->phys_block_num) {
			ret = apfs_delete_phys_extent(sb, &prev_ext);
			if (ret)
				goto out;
		}
		ret = apfs_crypto_adj_refcnt(sb, prev_crypto, -1);
		if (ret)
			goto out;
	} else if (prev_start == extent->logical_addr) {
		/* The new extent is the first logical block of the old one */
		if (second_run) {
			/* I don't know if this is possible, but be safe */
			apfs_alert(sb, "recursion shrinking extent head for dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
			goto out;
		}
		ret = apfs_shrink_extent_head(query, dstream, extent->logical_addr + extent->len);
		if (ret)
			goto out;
		/* The query should point to the previous record, start again */
		apfs_free_query(query);
		second_run = true;
		goto search_and_insert;
	} else if (prev_end == extent->logical_addr + extent->len) {
		/* The new extent is the last logical block of the old one */
		ret = apfs_shrink_extent_tail(query, dstream, extent->logical_addr);
		if (ret)
			goto out;
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret)
			goto out;
	} else if (prev_start < extent->logical_addr && prev_end > extent->logical_addr + extent->len) {
		/* The new extent is logically in the middle of the old one */
		if (second_run) {
			/* I don't know if this is possible, but be safe */
			apfs_alert(sb, "recursion when splitting extents for dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
			goto out;
		}
		ret = apfs_split_extent(query, extent->logical_addr + extent->len);
		if (ret)
			goto out;
		/* The split may make the query invalid */
		apfs_free_query(query);
		second_run = true;
		goto search_and_insert;
	} else {
		/* I don't know what this is, be safe */
		apfs_alert(sb, "strange extents for dstream 0x%llx", extent_id);
		ret = -EFSCORRUPTED;
		goto out;
	}

	ret = apfs_crypto_adj_refcnt(sb, new_crypto, 1);

out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_update_extent - Create or update the extent record for an extent
 * @dstream:	data stream info
 * @extent:	new in-memory file extent
 *
 * The @extent must either be a new tail for the dstream, or a single block.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_update_extent(struct apfs_dstream_info *dstream, const struct apfs_file_extent *extent)
{
	struct super_block *sb = dstream->ds_sb;

	if (extent->logical_addr + extent->len >= dstream->ds_size)
		return apfs_update_tail_extent(dstream, extent);
	if (extent->len > sb->s_blocksize)
		return -EOPNOTSUPP;
	return apfs_update_mid_extent(dstream, extent);
}
#define APFS_UPDATE_EXTENTS_MAXOPS	(1 + 2 * APFS_CRYPTO_ADJ_REFCNT_MAXOPS())

/**
 * apfs_insert_phys_extent - Create or grow the physical record for an extent
 * @dstream:	data stream info for the extent
 * @extent:	new in-memory file extent
 *
 * Only works for appending to extents, for now. TODO: reference counting.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_insert_phys_extent(struct apfs_dstream_info *dstream, const struct apfs_file_extent *extent)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root;
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_phys_ext_key raw_key;
	struct apfs_phys_ext_val raw_val;
	u64 kind = (u64)APFS_KIND_NEW << APFS_PEXT_KIND_SHIFT;
	u64 blkcnt = extent->len >> sb->s_blocksize_bits;
	int ret;

	extref_root = apfs_read_node(sb,
				le64_to_cpu(vsb_raw->apfs_extentref_tree_oid),
				APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(extref_root))
		return PTR_ERR(extref_root);
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(extref_root->object.oid);

	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto fail;
	}

	apfs_init_extent_key(extent->phys_block_num, &key);
	query->key = &key;
	/* The query is exact for now because we assume single-block extents */
	query->flags = APFS_QUERY_EXTENTREF | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto fail;

	apfs_key_set_hdr(APFS_TYPE_EXTENT, extent->phys_block_num, &raw_key);
	raw_val.len_and_kind = cpu_to_le64(kind | blkcnt);
	raw_val.owning_obj_id = cpu_to_le64(dstream->ds_id);
	raw_val.refcnt = cpu_to_le32(1);

	if (ret)
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
					&raw_val, sizeof(raw_val));
	else
		ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key),
					 &raw_val, sizeof(raw_val));

fail:
	apfs_free_query(query);
	apfs_node_free(extref_root);
	return ret;
}

/**
 * apfs_phys_ext_from_query - Read the physical extent record found by a query
 * @query:	the (successful) query that found the record
 * @pext:	on return, the physical extent read
 *
 * Reads the physical extent record into @pext and performs some basic sanity
 * checks as a protection against crafted filesystems. Returns 0 on success or
 * -EFSCORRUPTED otherwise.
 */
static int apfs_phys_ext_from_query(struct apfs_query *query, struct apfs_phys_extent *pext)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_key *key;
	struct apfs_phys_ext_val *val;
	char *raw = query->node->object.data;

	if (query->len != sizeof(*val) || query->key_len != sizeof(*key))
		return -EFSCORRUPTED;

	key = (struct apfs_phys_ext_key *)(raw + query->key_off);
	val = (struct apfs_phys_ext_val *)(raw + query->off);

	pext->bno = apfs_cat_cnid(&key->hdr);
	pext->blkcount = le64_to_cpu(val->len_and_kind) & APFS_PEXT_LEN_MASK;
	pext->len = pext->blkcount << sb->s_blocksize_bits;
	pext->refcnt = le32_to_cpu(val->refcnt);
	return 0;
}

/**
 * apfs_free_phys_ext - Add all blocks in a physical extent to the free queue
 * @sb:		superblock structure
 * @pext:	physical range to free
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_free_phys_ext(struct super_block *sb, struct apfs_phys_extent *pext)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;

	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, -pext->blkcount);

	return apfs_free_queue_insert(sb, pext->bno, pext->blkcount);
}

/**
 * apfs_put_phys_extent - Reduce the reference count for a physical extent
 * @pext:	physical extent data, already read
 * @query:	query that found the extent
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_put_phys_extent(struct apfs_phys_extent *pext, struct apfs_query *query)
{
	struct apfs_phys_ext_val *val;
	void *raw;
	int err;

	if (--pext->refcnt == 0) {
		struct super_block *sb = query->node->object.sb;

		err = apfs_btree_remove(query);
		if (err)
			return err;
		return apfs_free_phys_ext(sb, pext);
	}

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.data;
	val = raw + query->off;
	val->refcnt = cpu_to_le32(pext->refcnt);
	return 0;
}

/**
 * apfs_set_phys_ext_length - Set new length in a physical extent record's value
 * @pext:	the physical extent record's value
 * @len:	the new length (in blocks)
 *
 * Preserves the kind, though I doubt that's the right thing to do in general.
 */
static inline void apfs_set_phys_ext_length(struct apfs_phys_ext_val *pext, u64 len)
{
	u64 len_and_kind = le64_to_cpu(pext->len_and_kind);
	u64 kind = len_and_kind & APFS_PEXT_KIND_MASK;

	pext->len_and_kind = cpu_to_le64(kind | len);
}

/**
 * apfs_shrink_phys_ext_head - Shrink a physical extent record in its head
 * @query:	the query that found the record
 * @start:	new first physical block for the extent
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_shrink_phys_ext_head(struct apfs_query *query, u64 start)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_key key;
	struct apfs_phys_ext_val val;
	struct apfs_phys_extent pextent;
	struct apfs_phys_extent head = {0};
	u64 new_blkcount;
	void *raw = NULL;
	int err = 0;

	err = apfs_phys_ext_from_query(query, &pextent);
	if (err)
		return err;
	raw = query->node->object.data;
	key = *(struct apfs_phys_ext_key *)(raw + query->key_off);
	val = *(struct apfs_phys_ext_val *)(raw + query->off);

	new_blkcount = pextent.bno + pextent.blkcount - start;

	/* Free the blocks lost in the shrinkage */
	head.bno = pextent.bno;
	head.blkcount = pextent.blkcount - new_blkcount;
	head.len = head.blkcount << sb->s_blocksize_bits;
	err = apfs_free_phys_ext(sb, &head);
	if (err)
		return err;

	/* This is the actual shrinkage of the physical extent */
	apfs_key_set_hdr(APFS_TYPE_EXTENT, start, &key);
	apfs_set_phys_ext_length(&val, new_blkcount);
	return apfs_btree_replace(query, &key, sizeof(key), &val, sizeof(val));
}

/**
 * apfs_shrink_phys_ext_tail - Shrink a physical extent record in its tail
 * @query:	the query that found the record
 * @end:	new physical block to end the extent
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_shrink_phys_ext_tail(struct apfs_query *query, u64 end)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_val *val;
	struct apfs_phys_extent pextent;
	struct apfs_phys_extent tail = {0};
	u64 new_blkcount;
	void *raw;
	int err = 0;

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.data;

	err = apfs_phys_ext_from_query(query, &pextent);
	if (err)
		return err;
	val = raw + query->off;

	new_blkcount = end - pextent.bno;

	/* Free the blocks lost in the shrinkage */
	tail.bno = end;
	tail.blkcount = pextent.blkcount - new_blkcount;
	tail.len = tail.blkcount << sb->s_blocksize_bits;
	err = apfs_free_phys_ext(sb, &tail);
	if (err)
		return err;

	/* This is the actual shrinkage of the logical extent */
	apfs_set_phys_ext_length(val, new_blkcount);
	return err;
}

/**
 * apfs_split_phys_ext - Break a physical extent in two
 * @query:	query pointing to the extent
 * @div:	first physical block number to come after the division
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_split_phys_ext(struct apfs_query *query, u64 div)
{
	struct apfs_phys_ext_val *val1;
	struct apfs_phys_ext_key key2;
	struct apfs_phys_ext_val val2;
	struct apfs_phys_extent pextent;
	u64 blkcount1, blkcount2;
	void *raw;
	int err = 0;

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.data;

	err = apfs_phys_ext_from_query(query, &pextent);
	if (err)
		return err;
	val1 = raw + query->off;
	val2 = *(struct apfs_phys_ext_val *)(raw + query->off);
	key2 = *(struct apfs_phys_ext_key *)(raw + query->key_off);

	blkcount1 = div - pextent.bno;
	blkcount2 = pextent.blkcount - blkcount1;

	/* Modify the current extent in place to become the first half */
	apfs_set_phys_ext_length(val1, blkcount1);

	/* Insert the second half right after the first */
	apfs_key_set_hdr(APFS_TYPE_EXTENT, div, &key2);
	apfs_set_phys_ext_length(&val2, blkcount2);
	return apfs_btree_insert(query, &key2, sizeof(key2), &val2, sizeof(val2));
}

/**
 * apfs_delete_phys_extent - Delete (or modify) physical extent records in range
 * @sb:		superblock structure
 * @extent:	range of physical blocks to delete
 *
 * The range to delete must either be a whole extent, the tail of an extent, or
 * a single block. Returns 0 on success or a negative error code in case of
 * failure.
 */
static int apfs_delete_phys_extent(struct super_block *sb, const struct apfs_file_extent *extent)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root;
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_phys_extent prev_ext;
	u64 del_start, del_end, prev_start, prev_end;
	bool second_run = false;
	int ret;

	if (extent->len == 0)
		return 0;

	extref_root = apfs_read_node(sb,
				le64_to_cpu(vsb_raw->apfs_extentref_tree_oid),
				APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(extref_root))
		return PTR_ERR(extref_root);
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(extref_root->object.oid);

	apfs_init_extent_key(extent->phys_block_num, &key);

search_and_insert:
	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto fail;
	}
	query->key = &key;
	query->flags = APFS_QUERY_EXTENTREF;

	ret = apfs_btree_query(sb, &query);
	if (ret == -ENODATA)
		ret = -EFSCORRUPTED;
	if (ret)
		goto fail;

	ret = apfs_phys_ext_from_query(query, &prev_ext);
	if (ret)
		goto fail;
	del_start = extent->phys_block_num;
	del_end = del_start + (extent->len >> sb->s_blocksize_bits);
	prev_start = prev_ext.bno;
	prev_end = prev_ext.bno + prev_ext.blkcount;

	if (prev_ext.len == extent->len) {
		/* The range to free is the whole extent */
		ret = apfs_put_phys_extent(&prev_ext, query);
	} else if (prev_ext.refcnt > 1) {
		ret = -EOPNOTSUPP; /* TODO */
	} else if (prev_start == del_start) {
		/* The range to free is the first block of the extent */
		if (extent->len != sb->s_blocksize) {
			apfs_alert(sb, "deleting a large physical extent's head at 0x%llx", del_start);
			ret = -EFSCORRUPTED;
		} else {
			ret = apfs_shrink_phys_ext_head(query, del_end);
		}
	} else if (prev_end == del_end) {
		/* The range to free is the tail of the extent */
		ret = apfs_shrink_phys_ext_tail(query, del_start);
	} else if (prev_start < del_start && prev_end > del_end) {
		/* The range to free is a block in the middle of the extent */
		if (second_run) {
			/* I don't know if this is possible, but be safe */
			apfs_alert(sb, "recursion splitting physical extents at block 0x%llx", del_start);
			ret = -EFSCORRUPTED;
			goto fail;
		}
		ret = apfs_split_phys_ext(query, del_end);
		if (ret)
			goto fail;
		/* The split may make the query invalid */
		apfs_free_query(query);
		second_run = true;
		goto search_and_insert;
	} else {
		/* I don't know how we got here, be safe */
		apfs_alert(sb, "strange physical extents at block 0x%llx", del_start);
		ret = -EFSCORRUPTED;
	}

fail:
	apfs_free_query(query);
	apfs_node_free(extref_root);
	return ret;
}

/**
 * apfs_dstream_cache_is_tail - Is the tail of this dstream in its extent cache?
 * @dstream: dstream to check
 */
static inline bool apfs_dstream_cache_is_tail(struct apfs_dstream_info *dstream)
{
	struct apfs_file_extent *cache = &dstream->ds_cached_ext;

	/* nx_big_sem provides the locking for the cache here */
	return cache->len && (dstream->ds_size <= cache->logical_addr + cache->len);
}

/**
 * apfs_flush_extent_cache - Write the cached extent to the catalog, if dirty
 * @dstream: data stream to flush
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_flush_extent_cache(struct apfs_dstream_info *dstream)
{
	struct apfs_file_extent *ext = &dstream->ds_cached_ext;
	int err;

	if (!dstream->ds_ext_dirty)
		return 0;
	ASSERT(ext->len > 0);

	err = apfs_update_extent(dstream, ext);
	if (err)
		return err;
	err = apfs_insert_phys_extent(dstream, ext);
	if (err)
		return err;

	/*
	 * TODO: keep track of the byte and block count through the use of
	 * inode_add_bytes() and inode_set_bytes(). This hasn't been done with
	 * care in the rest of the module and it doesn't seem to matter beyond
	 * stat(), so I'm ignoring it for now.
	 */

	dstream->ds_ext_dirty = false;
	return 0;
}
#define APFS_FLUSH_EXTENT_CACHE	APFS_UPDATE_EXTENTS_MAXOPS

/**
 * apfs_create_hole - Create and insert a hole extent for the dstream
 * @dstream:	data stream info
 * @start:	first logical block number for the hole
 * @end:	first logical block number right after the hole
 *
 * Returns 0 on success, or a negative error code in case of failure.
 * TODO: what happens to the crypto refcount?
 */
static int apfs_create_hole(struct apfs_dstream_info *dstream, u64 start, u64 end)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent_key raw_key;
	struct apfs_file_extent_val raw_val;
	u64 extent_id = dstream->ds_id;
	int ret;

	if (start == end)
		return 0;

	/* File extent records use addresses, not block numbers */
	start <<= sb->s_blocksize_bits;
	end <<= sb->s_blocksize_bits;

	apfs_key_set_hdr(APFS_TYPE_FILE_EXTENT, extent_id, &raw_key);
	raw_key.logical_addr = cpu_to_le64(start);
	raw_val.len_and_flags = cpu_to_le64(end - start);
	raw_val.phys_block_num = cpu_to_le64(0); /* It's a hole... */
	raw_val.crypto_id = cpu_to_le64(apfs_vol_is_encrypted(sb) ? extent_id : 0);

	apfs_init_file_extent_key(extent_id, start, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto out;

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	dstream->ds_sparse_bytes += end - start;

out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_size_to_blocks - Return the block count for a given size, rounded up
 * @sb:		filesystem superblock
 * @size:	size in bytes
 *
 * TODO: reuse for inode.c
 */
static inline u64 apfs_size_to_blocks(struct super_block *sb, u64 size)
{
	return (size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
}

/**
 * apfs_zero_dstream_tail - Zero out stale bytes in a data stream's last block
 * @dstream: data stream info
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_zero_dstream_tail(struct apfs_dstream_info *dstream)
{
	struct super_block *sb = dstream->ds_sb;
	struct buffer_head *bh;
	u64 bno = 0, dstream_blks;
	int valid_length;
	int err;

	/* No stale bytes if no actual content */
	if (dstream->ds_size <= dstream->ds_sparse_bytes)
		return 0;

	/* No stale tail if the last block is fully used */
	valid_length = dstream->ds_size & (sb->s_blocksize - 1);
	if (valid_length == 0)
		return 0;

	dstream_blks = apfs_size_to_blocks(sb, dstream->ds_size);

	err = apfs_logic_to_phys_bno(dstream, dstream_blks - 1, &bno);
	if (err)
		return err;
	if (bno == 0) /* No stale bytes in holes */
		return 0;

	bh = apfs_sb_bread(sb, bno);
	if (!bh)
		return -EIO;

	/*
	 * We are only modifying stale data, so no need to join the
	 * transaction. TODO: snapshots?
	 */
	memset(bh->b_data + valid_length, 0, sb->s_blocksize - valid_length);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh); /* TODO: add to trans but mark as no-CoWed? */
	brelse(bh);
	return 0;
}

/**
 * apfs_zero_bh_tail - Zero out stale bytes in a buffer head
 * @sb:		filesystem superblock
 * @bh:		buffer head to zero
 * @length:	length of valid bytes to be left alone
 */
static void apfs_zero_bh_tail(struct super_block *sb, struct buffer_head *bh, u64 length)
{
	ASSERT(buffer_trans(bh));
	if (length < sb->s_blocksize)
		memset(bh->b_data + length, 0, sb->s_blocksize - length);
}

/**
 * apfs_dstream_get_new_block - Like the get_block_t function, but for dstreams
 * @dstream:	data stream info
 * @dsblock:	logical dstream block to map
 * @bh_result:	buffer head to map (NULL if none)
 * @bno:	if not NULL, the new block number is returned here
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_dstream_get_new_block(struct apfs_dstream_info *dstream, u64 dsblock, struct buffer_head *bh_result, u64 *bno)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_file_extent *cache = &dstream->ds_cached_ext;
	u64 phys_bno, logical_addr, cache_blks, dstream_blks;
	int err;

	cache_blks = apfs_size_to_blocks(sb, cache->len);

	/* TODO: preallocate tail blocks */
	logical_addr = dsblock << sb->s_blocksize_bits;

	err = apfs_spaceman_allocate_block(sb, &phys_bno, false /* backwards */);
	if (err)
		return err;
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, 1);
	if (bno)
		*bno = phys_bno;

	if (bh_result) {
		apfs_map_bh(bh_result, sb, phys_bno);
		err = apfs_transaction_join(sb, bh_result);
		if (err)
			return err;

		if (!buffer_uptodate(bh_result)) {
			/*
			 * Truly new buffers need to be marked as such, to get
			 * zeroed; this also takes care of holes in sparse files
			 */
			set_buffer_new(bh_result);
		} else if (dstream->ds_size > logical_addr) {
			/*
			 * The last block may have stale data left from a
			 * truncation
			 */
			apfs_zero_bh_tail(sb, bh_result, dstream->ds_size - logical_addr);
		}
	}

	if (apfs_dstream_cache_is_tail(dstream) &&
	    logical_addr == cache->logical_addr + cache->len &&
	    phys_bno == cache->phys_block_num + cache_blks) {
		cache->len += sb->s_blocksize;
		dstream->ds_ext_dirty = true;
		return 0;
	}

	err = apfs_flush_extent_cache(dstream);
	if (err)
		return err;

	dstream_blks = apfs_size_to_blocks(sb, dstream->ds_size);
	if (dstream_blks < dsblock) {
		err = apfs_zero_dstream_tail(dstream);
		if (err)
			return err;
		err = apfs_create_hole(dstream, dstream_blks, dsblock);
		if (err)
			return err;
	}

	cache->logical_addr = logical_addr;
	cache->phys_block_num = phys_bno;
	cache->len = sb->s_blocksize;
	dstream->ds_ext_dirty = true;
	return 0;
}
int APFS_GET_NEW_BLOCK_MAXOPS(void)
{
	return APFS_FLUSH_EXTENT_CACHE;
}

/**
 * apfs_dstream_get_new_bno - Allocate a new block inside a dstream
 * @dstream:	data stream info
 * @dsblock:	logical dstream block to allocate
 * @bno:	on return, the new block number
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_dstream_get_new_bno(struct apfs_dstream_info *dstream, u64 dsblock, u64 *bno)
{
	return apfs_dstream_get_new_block(dstream, dsblock, NULL /* bh_result */, bno);
}

int apfs_get_new_block(struct inode *inode, sector_t iblock,
		       struct buffer_head *bh_result, int create)
{
	struct apfs_inode_info *ai = APFS_I(inode);

	ASSERT(create);
	return apfs_dstream_get_new_block(&ai->i_dstream, iblock, bh_result, NULL /* bno */);
}

/**
 * apfs_shrink_dstream_last_extent - Shrink last extent of dstream being resized
 * @dstream:	data stream info
 * @new_size:	new size for the whole data stream
 *
 * Deletes, shrinks or zeroes the last extent, as needed for the truncation of
 * the data stream.
 *
 * Only works with the last extent, so it needs to be called repeatedly to
 * complete the truncation. Returns -EAGAIN in that case, or 0 when the process
 * is complete. Returns other negative error codes in case of failure.
 */
static int apfs_shrink_dstream_last_extent(struct apfs_dstream_info *dstream, loff_t new_size)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent tail;
	u64 extent_id = dstream->ds_id;
	int ret = 0;

	apfs_init_file_extent_key(extent_id, -1, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto out;

	if (!apfs_query_found_extent(query)) {
		/* No more extents, we deleted the whole file already? */
		if (new_size)
			ret = -EFSCORRUPTED;
		else
			ret = 0;
		goto out;
	}

	ret = apfs_extent_from_query(query, &tail);
	if (ret)
		goto out;

	if (tail.logical_addr + tail.len < new_size) {
		ret = -EFSCORRUPTED; /* Tail extent missing */
	} else if (tail.logical_addr + tail.len == new_size) {
		ret = 0; /* Nothing more to be done */
	} else if (tail.logical_addr >= new_size) {
		/* This whole extent needs to go */
		ret = apfs_btree_remove(query);
		if (ret)
			goto out;
		if (apfs_ext_is_hole(&tail)) {
			dstream->ds_sparse_bytes -= tail.len;
		} else {
			ret = apfs_delete_phys_extent(sb, &tail);
			if (ret)
				goto out;
		}
		ret = apfs_crypto_adj_refcnt(sb, tail.crypto_id, -1);
		if (ret)
			goto out;
		ret = tail.logical_addr == new_size ? 0 : -EAGAIN;
	} else {
		/*
		 * The file is being truncated in the middle of this extent.
		 * TODO: preserve the physical tail to be overwritten later.
		 */
		new_size = apfs_size_to_blocks(sb, new_size) << sb->s_blocksize_bits;
		ret = apfs_shrink_extent_tail(query, dstream, new_size);
	}

out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_shrink_dstream - Shrink a data stream's extents to a new length
 * @dstream:	data stream info
 * @new_size:	the new size
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_shrink_dstream(struct apfs_dstream_info *dstream, loff_t new_size)
{
	int ret;

	do {
		ret = apfs_shrink_dstream_last_extent(dstream, new_size);
	} while (ret == -EAGAIN);

	return ret;
}

/**
 * apfs_truncate - Truncate a data stream's content
 * @dstream:	data stream info
 * @new_size:	the new size
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_truncate(struct apfs_dstream_info *dstream, loff_t new_size)
{
	struct super_block *sb = dstream->ds_sb;
	u64 old_blks, new_blks;
	struct apfs_file_extent *cache = &dstream->ds_cached_ext;
	int err;

	/* TODO: don't write the cached extent if it will be deleted */
	err = apfs_flush_extent_cache(dstream);
	if (err)
		return err;
	dstream->ds_ext_dirty = false;

	/* TODO: keep the cache valid on truncation */
	cache->len = 0;

	/* "<=", because a partial write may have left extents beyond the end */
	if (new_size <= dstream->ds_size)
		return apfs_shrink_dstream(dstream, new_size);

	err = apfs_zero_dstream_tail(dstream);
	if (err)
		return err;
	new_blks = apfs_size_to_blocks(sb, new_size);
	old_blks = apfs_size_to_blocks(sb, dstream->ds_size);
	return apfs_create_hole(dstream, old_blks, new_blks);
}
