// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"

#define MAX(X, Y)	((X) <= (Y) ? (Y) : (X))

/**
 * apfs_ext_is_hole - Does this extent represent a hole in a sparse file?
 * @extent: the extent to check
 */
static inline bool apfs_ext_is_hole(struct apfs_file_extent *extent)
{
	return extent->phys_block_num == 0;
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
	char *raw = query->node->object.data;
	u64 ext_len;

	if(!apfs_is_sealed(sb)) {
		struct apfs_file_extent_val *ext = NULL;
		struct apfs_file_extent_key *ext_key = NULL;

		if (query->len != sizeof(*ext) || query->key_len != sizeof(*ext_key)) {
			apfs_err(sb, "bad length of key (%d) or value (%d)", query->key_len, query->len);
			return -EFSCORRUPTED;
		}

		ext = (struct apfs_file_extent_val *)(raw + query->off);
		ext_key = (struct apfs_file_extent_key *)(raw + query->key_off);
		ext_len = le64_to_cpu(ext->len_and_flags) & APFS_FILE_EXTENT_LEN_MASK;

		extent->logical_addr = le64_to_cpu(ext_key->logical_addr);
		extent->phys_block_num = le64_to_cpu(ext->phys_block_num);
		extent->crypto_id = le64_to_cpu(ext->crypto_id);
	} else {
		struct apfs_fext_tree_val *fext_val = NULL;
		struct apfs_fext_tree_key *fext_key = NULL;

		if (query->len != sizeof(*fext_val) || query->key_len != sizeof(*fext_key)) {
			apfs_err(sb, "bad length of sealed key (%d) or value (%d)", query->key_len, query->len);
			return -EFSCORRUPTED;
		}

		fext_val = (struct apfs_fext_tree_val *)(raw + query->off);
		fext_key = (struct apfs_fext_tree_key *)(raw + query->key_off);
		ext_len = le64_to_cpu(fext_val->len_and_flags) & APFS_FILE_EXTENT_LEN_MASK;

		extent->logical_addr = le64_to_cpu(fext_key->logical_addr);
		extent->phys_block_num = le64_to_cpu(fext_val->phys_block_num);
		extent->crypto_id = 0;
	}

	/* Extent length must be a multiple of the block size */
	if (ext_len & (sb->s_blocksize - 1)) {
		apfs_err(sb, "invalid length (0x%llx)", ext_len);
		return -EFSCORRUPTED;
	}
	extent->len = ext_len;
	return 0;
}

/**
 * apfs_extent_read - Read the extent record that covers a block
 * @dstream:	data stream info
 * @dsblock:	logical number of the wanted block (must be in range)
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
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent *cache = &dstream->ds_cached_ext;
	u64 iaddr = dsblock << sb->s_blocksize_bits;
	struct apfs_node *root = NULL;
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
	if (!apfs_is_sealed(sb)) {
		apfs_init_file_extent_key(dstream->ds_id, iaddr, &key);
		root = sbi->s_cat_root;
	} else {
		apfs_init_fext_key(dstream->ds_id, iaddr, &key);
		root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_fext_tree_oid), APFS_OBJ_PHYSICAL, false /* write */);
		if (IS_ERR(root)) {
			apfs_err(sb, "failed to read fext root 0x%llx", le64_to_cpu(vsb_raw->apfs_fext_tree_oid));
			return PTR_ERR(root);
		}
	}

	query = apfs_alloc_query(root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto done;
	}
	query->key = &key;
	query->flags = apfs_is_sealed(sb) ? APFS_QUERY_FEXT : APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		apfs_err(sb, "query failed for id 0x%llx, addr 0x%llx", dstream->ds_id, iaddr);
		if (ret == -ENODATA)
			ret = -EFSCORRUPTED;
		goto done;
	}

	ret = apfs_extent_from_query(query, extent);
	if (ret) {
		apfs_err(sb, "bad extent record for dstream 0x%llx", dstream->ds_id);
		goto done;
	}
	if (iaddr < extent->logical_addr || iaddr >= extent->logical_addr + extent->len) {
		apfs_err(sb, "no extent for addr 0x%llx in dstream 0x%llx", iaddr, dstream->ds_id);
		ret = -EFSCORRUPTED;
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
	if (apfs_is_sealed(sb))
		apfs_node_free(root);
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

	if (dsblock >= apfs_size_to_blocks(sb, dstream->ds_size))
		return 0;

	ret = apfs_extent_read(dstream, dsblock, &ext);
	if (ret) {
		apfs_err(sb, "extent read failed");
		return ret;
	}

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

static int apfs_range_put_reference(struct super_block *sb, u64 paddr, u64 length);

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
	if (err) {
		apfs_err(sb, "bad extent record for dstream 0x%llx", dstream->ds_id);
		return err;
	}
	raw = query->node->object.data;
	key = *(struct apfs_file_extent_key *)(raw + query->key_off);
	val = *(struct apfs_file_extent_val *)(raw + query->off);

	new_len = extent.logical_addr + extent.len - start;
	head_len = extent.len - new_len;

	/* Delete the physical records for the blocks lost in the shrinkage */
	if (!apfs_ext_is_hole(&extent)) {
		err = apfs_range_put_reference(sb, extent.phys_block_num, head_len);
		if (err) {
			apfs_err(sb, "failed to put range 0x%llx-0x%llx", extent.phys_block_num, head_len);
			return err;
		}
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
 * Also puts the physical extent records for the tail. Returns 0 on success or
 * a negative error code in case of failure.
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
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;

	err = apfs_extent_from_query(query, &extent);
	if (err) {
		apfs_err(sb, "bad extent record for dstream 0x%llx", dstream->ds_id);
		return err;
	}
	val = raw + query->off;

	new_len = end - extent.logical_addr;
	new_blkcount = new_len >> sb->s_blocksize_bits;
	tail_len = extent.len - new_len;

	/* Delete the physical records for the blocks lost in the shrinkage */
	if (!apfs_ext_is_hole(&extent)) {
		err = apfs_range_put_reference(sb, extent.phys_block_num + new_blkcount, tail_len);
		if (err) {
			apfs_err(sb, "failed to put range 0x%llx-0x%llx", extent.phys_block_num + new_blkcount, tail_len);
			return err;
		}
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
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for last extent of id 0x%llx", extent_id);
		goto out;
	}

	if (ret == -ENODATA || !apfs_query_found_extent(query)) {
		/* We are creating the first extent for the file */
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret) {
			apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
			goto out;
		}
	} else {
		struct apfs_file_extent tail;

		ret = apfs_extent_from_query(query, &tail);
		if (ret) {
			apfs_err(sb, "bad extent record for dstream 0x%llx", dstream->ds_id);
			goto out;
		}

		if (tail.logical_addr > extent->logical_addr) {
			apfs_alert(sb, "extent is not tail - bug!");
			ret = -EOPNOTSUPP;
			goto out;
		} else if (tail.logical_addr == extent->logical_addr) {
			ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
			if (ret) {
				apfs_err(sb, "update failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
				goto out;
			}
			if (apfs_ext_is_hole(&tail)) {
				dstream->ds_sparse_bytes -= tail.len;
			} else if (tail.phys_block_num != extent->phys_block_num) {
				ret = apfs_range_put_reference(sb, tail.phys_block_num, tail.len);
				if (ret) {
					apfs_err(sb, "failed to put range 0x%llx-0x%llx", tail.phys_block_num, tail.len);
					goto out;
				}
			}
			if (new_crypto == tail.crypto_id)
				goto out;
			ret = apfs_crypto_adj_refcnt(sb, tail.crypto_id, -1);
			if (ret) {
				apfs_err(sb, "failed to put crypto id 0x%llx", tail.crypto_id);
				goto out;
			}
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
				if (ret) {
					apfs_err(sb, "failed to shrink tail of dstream 0x%llx", extent_id);
					goto out;
				}
			}
			ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
			if (ret) {
				apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
				goto out;
			}
		}
	}

	ret = apfs_crypto_adj_refcnt(sb, new_crypto, 1);
	if (ret)
		apfs_err(sb, "failed to take crypto id 0x%llx", new_crypto);

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
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;

	err = apfs_extent_from_query(query, &extent);
	if (err) {
		apfs_err(sb, "bad extent record");
		return err;
	}
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
	if (err) {
		apfs_err(sb, "insertion failed in division 0x%llx", div);
		return err;
	}

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
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
		goto out;
	}

	if (ret == -ENODATA || !apfs_query_found_extent(query)) {
		/*
		 * The new extent goes in a hole we just made, right at the
		 * beginning of the file.
		 */
		if (!second_run) {
			apfs_err(sb, "missing extent on dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
		} else {
			ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
			if (ret)
				apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
		}
		goto out;
	}

	if (apfs_extent_from_query(query, &prev_ext)) {
		apfs_err(sb, "bad mid extent record on dstream 0x%llx", extent_id);
		ret = -EFSCORRUPTED;
		goto out;
	}
	prev_crypto = prev_ext.crypto_id;
	prev_start = prev_ext.logical_addr;
	prev_end = prev_ext.logical_addr + prev_ext.len;

	if (prev_end == extent->logical_addr && second_run) {
		/* The new extent goes in the hole we just made */
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret) {
			apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
			goto out;
		}
	} else if (prev_start == extent->logical_addr && prev_ext.len == extent->len) {
		/* The old and new extents are the same logical block */
		ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret) {
			apfs_err(sb, "update failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
			goto out;
		}
		if (apfs_ext_is_hole(&prev_ext)) {
			dstream->ds_sparse_bytes -= prev_ext.len;
		} else if (prev_ext.phys_block_num != extent->phys_block_num) {
			ret = apfs_range_put_reference(sb, prev_ext.phys_block_num, prev_ext.len);
			if (ret) {
				apfs_err(sb, "failed to put range 0x%llx-0x%llx", prev_ext.phys_block_num, prev_ext.len);
				goto out;
			}
		}
		ret = apfs_crypto_adj_refcnt(sb, prev_crypto, -1);
		if (ret) {
			apfs_err(sb, "failed to put crypto id 0x%llx", prev_crypto);
			goto out;
		}
	} else if (prev_start == extent->logical_addr) {
		/* The new extent is the first logical block of the old one */
		if (second_run) {
			/* I don't know if this is possible, but be safe */
			apfs_alert(sb, "recursion shrinking extent head for dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
			goto out;
		}
		ret = apfs_shrink_extent_head(query, dstream, extent->logical_addr + extent->len);
		if (ret) {
			apfs_err(sb, "failed to shrink extent in dstream 0x%llx", extent_id);
			goto out;
		}
		/* The query should point to the previous record, start again */
		apfs_free_query(query);
		second_run = true;
		goto search_and_insert;
	} else if (prev_end == extent->logical_addr + extent->len) {
		/* The new extent is the last logical block of the old one */
		ret = apfs_shrink_extent_tail(query, dstream, extent->logical_addr);
		if (ret) {
			apfs_err(sb, "failed to shrink extent in dstream 0x%llx", extent_id);
			goto out;
		}
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret) {
			apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, extent->logical_addr);
			goto out;
		}
	} else if (prev_start < extent->logical_addr && prev_end > extent->logical_addr + extent->len) {
		/* The new extent is logically in the middle of the old one */
		if (second_run) {
			/* I don't know if this is possible, but be safe */
			apfs_alert(sb, "recursion when splitting extents for dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
			goto out;
		}
		ret = apfs_split_extent(query, extent->logical_addr + extent->len);
		if (ret) {
			apfs_err(sb, "failed to split extent in dstream 0x%llx", extent_id);
			goto out;
		}
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
	if (ret)
		apfs_err(sb, "failed to take crypto id 0x%llx", new_crypto);

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
	if (extent->len > sb->s_blocksize) {
		apfs_err(sb, "can't create mid extents of length 0x%llx", extent->len);
		return -EOPNOTSUPP;
	}
	return apfs_update_mid_extent(dstream, extent);
}
#define APFS_UPDATE_EXTENTS_MAXOPS	(1 + 2 * APFS_CRYPTO_ADJ_REFCNT_MAXOPS())

static int apfs_extend_phys_extent(struct apfs_query *query, u64 bno, u64 blkcnt, u64 dstream_id)
{
	struct apfs_phys_ext_key raw_key;
	struct apfs_phys_ext_val raw_val;
	u64 kind = (u64)APFS_KIND_NEW << APFS_PEXT_KIND_SHIFT;

	apfs_key_set_hdr(APFS_TYPE_EXTENT, bno, &raw_key);
	raw_val.len_and_kind = cpu_to_le64(kind | blkcnt);
	raw_val.owning_obj_id = cpu_to_le64(dstream_id);
	raw_val.refcnt = cpu_to_le32(1);
	return apfs_btree_replace(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
}

static int apfs_insert_new_phys_extent(struct apfs_query *query, u64 bno, u64 blkcnt, u64 dstream_id)
{
	struct apfs_phys_ext_key raw_key;
	struct apfs_phys_ext_val raw_val;
	u64 kind = (u64)APFS_KIND_NEW << APFS_PEXT_KIND_SHIFT;

	apfs_key_set_hdr(APFS_TYPE_EXTENT, bno, &raw_key);
	raw_val.len_and_kind = cpu_to_le64(kind | blkcnt);
	raw_val.owning_obj_id = cpu_to_le64(dstream_id);
	raw_val.refcnt = cpu_to_le32(1);
	return apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
}

static int apfs_phys_ext_from_query(struct apfs_query *query, struct apfs_phys_extent *pext);

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
	struct apfs_phys_extent pext;
	u64 blkcnt = extent->len >> sb->s_blocksize_bits;
	u64 last_bno, new_base, new_blkcnt;
	int ret;

	extref_root = apfs_read_node(sb,
				le64_to_cpu(vsb_raw->apfs_extentref_tree_oid),
				APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(extref_root)) {
		apfs_err(sb, "failed to read extref root 0x%llx", le64_to_cpu(vsb_raw->apfs_extentref_tree_oid));
		return PTR_ERR(extref_root);
	}
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(extref_root->object.oid);

	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * The cached logical extent may have been split into multiple physical
	 * extents because of clones. If that happens, we want to grow the last
	 * one.
	 */
	last_bno = extent->phys_block_num + blkcnt - 1;
	apfs_init_extent_key(last_bno, &key);
	query->key = &key;
	query->flags = APFS_QUERY_EXTENTREF;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for paddr 0x%llx", last_bno);
		goto out;
	}

	if (ret == -ENODATA) {
		/* This is a fresh new physical extent */
		ret = apfs_insert_new_phys_extent(query, extent->phys_block_num, blkcnt, dstream->ds_id);
		if (ret)
			apfs_err(sb, "insertion failed for paddr 0x%llx", extent->phys_block_num);
		goto out;
	}

	ret = apfs_phys_ext_from_query(query, &pext);
	if (ret) {
		apfs_err(sb, "bad pext record for bno 0x%llx", last_bno);
		goto out;
	}
	if (pext.bno + pext.blkcount <= extent->phys_block_num) {
		/* Also a fresh new physical extent */
		ret = apfs_insert_new_phys_extent(query, extent->phys_block_num, blkcnt, dstream->ds_id);
		if (ret)
			apfs_err(sb, "insertion failed for paddr 0x%llx", extent->phys_block_num);
		goto out;
	}

	/*
	 * There is an existing physical extent that overlaps the new one. The
	 * cache was dirty, so the existing extent can't cover the whole tail.
	 */
	if (pext.bno + pext.blkcount >= extent->phys_block_num + blkcnt) {
		apfs_err(sb, "dirty cache tail covered by existing physical extent 0x%llx-0x%llx", pext.bno, pext.blkcount);
		ret = -EFSCORRUPTED;
		goto out;
	}
	if (pext.refcnt == 1) {
		new_base = pext.bno;
		new_blkcnt = extent->phys_block_num + blkcnt - new_base;
		ret = apfs_extend_phys_extent(query, new_base, new_blkcnt, dstream->ds_id);
		if (ret)
			apfs_err(sb, "update failed for paddr 0x%llx", new_base);
	} else {
		/*
		 * We can't extend this one, because it would extend the other
		 * references as well.
		 */
		new_base = pext.bno + pext.blkcount;
		new_blkcnt = extent->phys_block_num + blkcnt - new_base;
		ret = apfs_insert_new_phys_extent(query, new_base, new_blkcnt, dstream->ds_id);
		if (ret)
			apfs_err(sb, "insertion failed for paddr 0x%llx", new_base);
	}

out:
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

	if (query->len != sizeof(*val) || query->key_len != sizeof(*key)) {
		apfs_err(sb, "bad length of key (%d) or value (%d)", query->key_len, query->len);
		return -EFSCORRUPTED;
	}

	key = (struct apfs_phys_ext_key *)(raw + query->key_off);
	val = (struct apfs_phys_ext_val *)(raw + query->off);

	pext->bno = apfs_cat_cnid(&key->hdr);
	pext->blkcount = le64_to_cpu(val->len_and_kind) & APFS_PEXT_LEN_MASK;
	pext->len = pext->blkcount << sb->s_blocksize_bits;
	pext->refcnt = le32_to_cpu(val->refcnt);
	pext->kind = le64_to_cpu(val->len_and_kind) >> APFS_PEXT_KIND_SHIFT;
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
	le64_add_cpu(&vsb_raw->apfs_total_blocks_freed, pext->blkcount);

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
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_val *val;
	void *raw;
	int err;

	if (--pext->refcnt == 0) {
		err = apfs_btree_remove(query);
		if (err) {
			apfs_err(sb, "removal failed for paddr 0x%llx", pext->bno);
			return err;
		}
		return pext->kind == APFS_KIND_NEW ? apfs_free_phys_ext(sb, pext) : 0;
	}

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;
	val = raw + query->off;
	val->refcnt = cpu_to_le32(pext->refcnt);
	return 0;
}

/**
 * apfs_take_phys_extent - Increase the reference count for a physical extent
 * @pext:	physical extent data, already read
 * @query:	query that found the extent
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_take_phys_extent(struct apfs_phys_extent *pext, struct apfs_query *query)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_val *val;
	void *raw;
	int err;

	/* An update extent may be dropped when a reference is taken */
	if (++pext->refcnt == 0)
		return apfs_btree_remove(query);

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}
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
 * apfs_split_phys_ext - Break a physical extent in two
 * @query:	query pointing to the extent
 * @div:	first physical block number to come after the division
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_split_phys_ext(struct apfs_query *query, u64 div)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_val *val1;
	struct apfs_phys_ext_key key2;
	struct apfs_phys_ext_val val2;
	struct apfs_phys_extent pextent;
	u64 blkcount1, blkcount2;
	void *raw;
	int err = 0;

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;

	err = apfs_phys_ext_from_query(query, &pextent);
	if (err) {
		apfs_err(sb, "bad pext record over div 0x%llx", div);
		return err;
	}
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
 * apfs_create_update_pext - Create a reference update physical extent record
 * @query:	query that searched for the physical extent
 * @extent:	range of physical blocks to update
 * @diff:	reference count change
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_update_pext(struct apfs_query *query, const struct apfs_file_extent *extent, u32 diff)
{
	struct super_block *sb = query->node->object.sb;
	struct apfs_phys_ext_key key = {0};
	struct apfs_phys_ext_val val = {0};

	apfs_key_set_hdr(APFS_TYPE_EXTENT, extent->phys_block_num, &key);
	val.len_and_kind = cpu_to_le64((u64)APFS_KIND_UPDATE << APFS_PEXT_KIND_SHIFT | extent->len >> sb->s_blocksize_bits);
	val.owning_obj_id = cpu_to_le64(APFS_OWNING_OBJ_ID_INVALID);
	val.refcnt = cpu_to_le32(diff);
	return apfs_btree_insert(query, &key, sizeof(key), &val, sizeof(val));
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
	struct super_block *sb = dstream->ds_sb;
	struct apfs_file_extent *ext = &dstream->ds_cached_ext;
	int err;

	if (!dstream->ds_ext_dirty)
		return 0;
	ASSERT(ext->len > 0);

	err = apfs_update_extent(dstream, ext);
	if (err) {
		apfs_err(sb, "extent update failed");
		return err;
	}
	err = apfs_insert_phys_extent(dstream, ext);
	if (err) {
		apfs_err(sb, "pext insertion failed");
		return err;
	}

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
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for id 0x%llx, addr 0x%llx", extent_id, start);
		goto out;
	}

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	if (ret)
		apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", extent_id, start);
	dstream->ds_sparse_bytes += end - start;

out:
	apfs_free_query(query);
	return ret;
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
	struct inode *inode = NULL;
	struct page *page = NULL;
	void *fsdata = NULL;
	int valid_length;
	int err;

	/* No stale bytes if no actual content */
	if (dstream->ds_size <= dstream->ds_sparse_bytes)
		return 0;

	/* No stale tail if the last block is fully used */
	valid_length = dstream->ds_size & (sb->s_blocksize - 1);
	if (valid_length == 0)
		return 0;

	inode = dstream->ds_inode;
	if (!inode) {
		/* This should never happen, but be safe */
		apfs_alert(sb, "attempt to zero the tail of xattr dstream 0x%llx", dstream->ds_id);
		return -EFSCORRUPTED;
	}

	/* This will take care of the CoW and zeroing */
	err = __apfs_write_begin(NULL, inode->i_mapping, inode->i_size, 0, 0, &page, &fsdata);
	if (err)
		return err;
	return __apfs_write_end(NULL, inode->i_mapping, inode->i_size, 0, 0, page, fsdata);
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
 * apfs_range_in_snap - Check if a given block range overlaps a snapshot
 * @sb:		filesystem superblock
 * @bno:	first block in the range
 * @blkcnt:	block count for the range
 * @in_snap:	on return, the result
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_range_in_snap(struct super_block *sb, u64 bno, u64 blkcnt, bool *in_snap)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root = NULL;
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_phys_extent pext = {0};
	int ret;

	/* Avoid the tree queries when we don't even have snapshots */
	if (vsb_raw->apfs_num_snapshots == 0) {
		*in_snap = false;
		return 0;
	}

	/*
	 * Now check if the current physical extent tree has an entry for
	 * these blocks
	 */
	extref_root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_extentref_tree_oid), APFS_OBJ_PHYSICAL, false /* write */);
	if (IS_ERR(extref_root)) {
		apfs_err(sb, "failed to read extref root 0x%llx", le64_to_cpu(vsb_raw->apfs_extentref_tree_oid));
		return PTR_ERR(extref_root);
	}

	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto out;
	}

	apfs_init_extent_key(bno, &key);
	query->key = &key;
	query->flags = APFS_QUERY_EXTENTREF;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for paddr 0x%llx", bno);
		goto out;
	}
	if (ret == -ENODATA) {
		*in_snap = false;
		ret = 0;
		goto out;
	}

	ret = apfs_phys_ext_from_query(query, &pext);
	if (ret) {
		apfs_err(sb, "bad pext record for paddr 0x%llx", bno);
		goto out;
	}

	if (pext.bno <= bno && pext.bno + pext.blkcount >= bno + blkcnt) {
		if (pext.kind == APFS_KIND_NEW) {
			*in_snap = false;
			goto out;
		}
	}

	/*
	 * I think the file extent could still be covered by two different
	 * physical extents from the current tree, but it's easier to just
	 * assume the worst here.
	 */
	*in_snap = true;

out:
	apfs_free_query(query);
	apfs_node_free(extref_root);
	return ret;
}

/**
 * apfs_dstream_cache_in_snap - Check if the cached extent overlaps a snapshot
 * @dstream:	the data stream to check
 * @in_snap:	on return, the result
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_dstream_cache_in_snap(struct apfs_dstream_info *dstream, bool *in_snap)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_file_extent *cache = NULL;

	/* All changes to extents get flushed when a snaphot is created */
	if (dstream->ds_ext_dirty) {
		*in_snap = false;
		return 0;
	}

	cache = &dstream->ds_cached_ext;
	return apfs_range_in_snap(sb, cache->phys_block_num, cache->len >> sb->s_blocksize_bits, in_snap);
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
	struct apfs_file_extent *cache = NULL;
	u64 phys_bno, logical_addr, cache_blks, dstream_blks;
	bool in_snap = true;
	int err;

	/* TODO: preallocate tail blocks */
	logical_addr = dsblock << sb->s_blocksize_bits;

	err = apfs_spaceman_allocate_block(sb, &phys_bno, false /* backwards */);
	if (err) {
		apfs_err(sb, "block allocation failed");
		return err;
	}
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, 1);
	le64_add_cpu(&vsb_raw->apfs_total_blocks_alloced, 1);
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

	dstream_blks = apfs_size_to_blocks(sb, dstream->ds_size);
	if (dstream_blks < dsblock) {
		/*
		 * This recurses into apfs_dstream_get_new_block() and dirties
		 * the extent cache, so it must happen before flushing it.
		 */
		err = apfs_zero_dstream_tail(dstream);
		if (err) {
			apfs_err(sb, "failed to zero tail for dstream 0x%llx", dstream->ds_id);
			return err;
		}
	}

	err = apfs_dstream_cache_in_snap(dstream, &in_snap);
	if (err)
		return err;

	cache = &dstream->ds_cached_ext;
	cache_blks = apfs_size_to_blocks(sb, cache->len);

	/* TODO: allow dirty caches of several blocks in the middle of a file */
	if (!in_snap && apfs_dstream_cache_is_tail(dstream) &&
	    logical_addr == cache->logical_addr + cache->len &&
	    phys_bno == cache->phys_block_num + cache_blks) {
		cache->len += sb->s_blocksize;
		dstream->ds_ext_dirty = true;
		return 0;
	}

	err = apfs_flush_extent_cache(dstream);
	if (err) {
		apfs_err(sb, "extent cache flush failed for dstream 0x%llx", dstream->ds_id);
		return err;
	}

	if (dstream_blks < dsblock) {
		/*
		 * This puts new extents after the reported end of the file, so
		 * it must happen after the flush to avoid conflict with those
		 * extent operations.
		 */
		err = apfs_create_hole(dstream, dstream_blks, dsblock);
		if (err) {
			apfs_err(sb, "hole creation failed for dstream 0x%llx", dstream->ds_id);
			return err;
		}
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
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for last extent of id 0x%llx", extent_id);
		goto out;
	}

	if (!apfs_query_found_extent(query)) {
		/* No more extents, we deleted the whole file already? */
		if (new_size) {
			apfs_err(sb, "missing extent for dstream 0x%llx", extent_id);
			ret = -EFSCORRUPTED;
		} else {
			ret = 0;
		}
		goto out;
	}

	ret = apfs_extent_from_query(query, &tail);
	if (ret) {
		apfs_err(sb, "bad tail extent record on dstream 0x%llx", extent_id);
		goto out;
	}

	if (tail.logical_addr + tail.len < new_size) {
		apfs_err(sb, "missing extent for dstream 0x%llx", extent_id);
		ret = -EFSCORRUPTED; /* Tail extent missing */
	} else if (tail.logical_addr + tail.len == new_size) {
		ret = 0; /* Nothing more to be done */
	} else if (tail.logical_addr >= new_size) {
		/* This whole extent needs to go */
		ret = apfs_btree_remove(query);
		if (ret) {
			apfs_err(sb, "removal failed for id 0x%llx, addr 0x%llx", dstream->ds_id, tail.logical_addr);
			goto out;
		}
		if (apfs_ext_is_hole(&tail)) {
			dstream->ds_sparse_bytes -= tail.len;
		} else {
			ret = apfs_range_put_reference(sb, tail.phys_block_num, tail.len);
			if (ret) {
				apfs_err(sb, "failed to put range 0x%llx-0x%llx", tail.phys_block_num, tail.len);
				goto out;
			}
		}
		ret = apfs_crypto_adj_refcnt(sb, tail.crypto_id, -1);
		if (ret) {
			apfs_err(sb, "failed to take crypto id 0x%llx", tail.crypto_id);
			goto out;
		}
		ret = tail.logical_addr == new_size ? 0 : -EAGAIN;
	} else {
		/*
		 * The file is being truncated in the middle of this extent.
		 * TODO: preserve the physical tail to be overwritten later.
		 */
		new_size = apfs_size_to_blocks(sb, new_size) << sb->s_blocksize_bits;
		ret = apfs_shrink_extent_tail(query, dstream, new_size);
		if (ret)
			apfs_err(sb, "failed to shrink tail of dstream 0x%llx", extent_id);
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
	if (err) {
		apfs_err(sb, "extent cache flush failed for dstream 0x%llx", dstream->ds_id);
		return err;
	}
	dstream->ds_ext_dirty = false;

	/* TODO: keep the cache valid on truncation */
	cache->len = 0;

	/* "<=", because a partial write may have left extents beyond the end */
	if (new_size <= dstream->ds_size)
		return apfs_shrink_dstream(dstream, new_size);

	err = apfs_zero_dstream_tail(dstream);
	if (err) {
		apfs_err(sb, "failed to zero tail for dstream 0x%llx", dstream->ds_id);
		return err;
	}
	new_blks = apfs_size_to_blocks(sb, new_size);
	old_blks = apfs_size_to_blocks(sb, dstream->ds_size);
	return apfs_create_hole(dstream, old_blks, new_blks);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
loff_t apfs_remap_file_range(struct file *src_file, loff_t off, struct file *dst_file, loff_t destoff, loff_t len, unsigned int remap_flags)
#else
int apfs_clone_file_range(struct file *src_file, loff_t off, struct file *dst_file, loff_t destoff, u64 len)
#endif
{
	struct inode *src_inode = file_inode(src_file);
	struct inode *dst_inode = file_inode(dst_file);
	struct apfs_inode_info *src_ai = APFS_I(src_inode);
	struct apfs_inode_info *dst_ai = APFS_I(dst_inode);
	struct apfs_dstream_info *src_ds = &src_ai->i_dstream;
	struct apfs_dstream_info *dst_ds = &dst_ai->i_dstream;
	struct super_block *sb = src_inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	/* TODO: remember to update the maxops in the future */
	struct apfs_max_ops maxops = {0};
	const u64 xfield_flags = APFS_INODE_MAINTAIN_DIR_STATS | APFS_INODE_IS_SPARSE | APFS_INODE_HAS_PURGEABLE_FLAGS;
	int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	if (remap_flags & ~(REMAP_FILE_ADVISORY))
		return -EINVAL;
#endif
	if (src_inode == dst_inode)
		return -EINVAL;

	/* We only want to clone whole files, like in the official driver */
	if (off != 0 || destoff != 0 || len != 0)
		return -EINVAL;

	/*
	 * Clones here work in two steps: first the user creates an empty target
	 * file, and then the user calls the ioctl, which replaces the file with
	 * a clone. This is not atomic, of course.
	 */
	if (dst_ai->i_has_dstream || dst_ai->i_bsd_flags & APFS_INOBSD_COMPRESSED) {
		apfs_warn(sb, "clones can only replace freshly created files");
		return -EOPNOTSUPP;
	}
	if (dst_ai->i_int_flags & xfield_flags) {
		apfs_warn(sb, "clone can't replace a file that has xfields");
		return -EOPNOTSUPP;
	}

	if (!src_ai->i_has_dstream) {
		apfs_warn(sb, "can't clone a file with no dstream");
		return -EOPNOTSUPP;
	}

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	apfs_inode_join_transaction(sb, src_inode);
	apfs_inode_join_transaction(sb, dst_inode);

	err = apfs_flush_extent_cache(src_ds);
	if (err) {
		apfs_err(sb, "extent cache flush failed for dstream 0x%llx", src_ds->ds_id);
		goto fail;
	}
	err = apfs_dstream_adj_refcnt(src_ds, +1);
	if (err) {
		apfs_err(sb, "failed to take dstream id 0x%llx", src_ds->ds_id);
		goto fail;
	}
	src_ds->ds_shared = true;

	dst_inode->i_mtime = dst_inode->i_ctime = current_time(dst_inode);
	dst_inode->i_size = src_inode->i_size;
	dst_ai->i_key_class = src_ai->i_key_class;
	dst_ai->i_int_flags = src_ai->i_int_flags;
	dst_ai->i_bsd_flags = src_ai->i_bsd_flags;
	dst_ai->i_has_dstream = true;

	dst_ds->ds_sb = src_ds->ds_sb;
	dst_ds->ds_inode = dst_inode;
	dst_ds->ds_id = src_ds->ds_id;
	dst_ds->ds_size = src_ds->ds_size;
	dst_ds->ds_sparse_bytes = src_ds->ds_sparse_bytes;
	dst_ds->ds_cached_ext = src_ds->ds_cached_ext;
	dst_ds->ds_ext_dirty = false;
	dst_ds->ds_shared = true;

	dst_ai->i_int_flags |= APFS_INODE_WAS_EVER_CLONED | APFS_INODE_WAS_CLONED;
	src_ai->i_int_flags |= APFS_INODE_WAS_EVER_CLONED;

	/*
	 * The sparse flag is the important one here: if we need it, it will get
	 * set later by apfs_update_inode(), after the xfield gets created.
	 */
	dst_ai->i_int_flags &= ~xfield_flags;

	/*
	 * Commit the transaction to make sure all buffers in the source inode
	 * go through copy-on-write. This is a bit excessive, but I don't expect
	 * clones to be created often enough for it to matter.
	 */
	sbi->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return dst_ds->ds_size;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_extent_create_record - Create a logical extent record for a dstream id
 * @sb:		filesystem superblock
 * @dstream_id:	the dstream id
 * @extent:	extent info for the record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_extent_create_record(struct super_block *sb, u64 dstream_id, struct apfs_file_extent *extent)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_file_extent_val raw_val;
	struct apfs_file_extent_key raw_key;
	int ret = 0;

	apfs_init_file_extent_key(dstream_id, extent->logical_addr, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for id 0x%llx, addr 0x%llx", dstream_id, extent->logical_addr);
		goto out;
	}

	apfs_key_set_hdr(APFS_TYPE_FILE_EXTENT, dstream_id, &raw_key);
	raw_key.logical_addr = cpu_to_le64(extent->logical_addr);
	raw_val.len_and_flags = cpu_to_le64(extent->len);
	raw_val.phys_block_num = cpu_to_le64(extent->phys_block_num);
	raw_val.crypto_id = cpu_to_le64(apfs_vol_is_encrypted(sb) ? dstream_id : 0); /* TODO */

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	if (ret)
		apfs_err(sb, "insertion failed for id 0x%llx, addr 0x%llx", dstream_id, extent->logical_addr);
out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_put_single_extent - Put a reference to a single extent
 * @sb:		filesystem superblock
 * @paddr_end:	first block after the extent to put
 * @paddr_min:	don't put references before this block
 *
 * Puts a reference to the physical extent range that ends in paddr. Sets
 * @paddr_end to the beginning of the extent, so that the caller can continue
 * with the previous one. Returns 0 on success, or a negative error code in
 * case of failure.
 *
 * TODO: unify this with apfs_take_single_extent(), they are almost the same.
 */
static int apfs_put_single_extent(struct super_block *sb, u64 *paddr_end, u64 paddr_min)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root = NULL;
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_phys_extent prev_ext;
	u64 prev_start, prev_end;
	bool cropped_head = false, cropped_tail = false;
	struct apfs_file_extent tmp = {0}; /* TODO: clean up all the fake extent interfaces? */
	int ret;

	extref_root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_extentref_tree_oid), APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(extref_root)) {
		apfs_err(sb, "failed to read extref root 0x%llx", le64_to_cpu(vsb_raw->apfs_extentref_tree_oid));
		return PTR_ERR(extref_root);
	}
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(extref_root->object.oid);

	apfs_init_extent_key(*paddr_end - 1, &key);

restart:
	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto out;
	}
	query->key = &key;
	query->flags = APFS_QUERY_EXTENTREF;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for paddr 0x%llx", *paddr_end - 1);
		goto out;
	}

	if (ret == -ENODATA) {
		/* The whole range to put is part of a snapshot */
		tmp.phys_block_num = paddr_min;
		tmp.len = (*paddr_end - paddr_min) << sb->s_blocksize_bits;
		ret = apfs_create_update_pext(query, &tmp, -1);
		*paddr_end = paddr_min;
		goto out;
	}

	ret = apfs_phys_ext_from_query(query, &prev_ext);
	if (ret) {
		apfs_err(sb, "bad pext record over paddr 0x%llx", *paddr_end - 1);
		goto out;
	}
	prev_start = prev_ext.bno;
	prev_end = prev_ext.bno + prev_ext.blkcount;
	if (prev_end < *paddr_end) {
		/* The extent to put is part of a snapshot */
		tmp.phys_block_num = MAX(prev_end, paddr_min);
		tmp.len = (*paddr_end - tmp.phys_block_num) << sb->s_blocksize_bits;
		ret = apfs_create_update_pext(query, &tmp, -1);
		*paddr_end = tmp.phys_block_num;
		goto out;
	}

	if ((cropped_tail && prev_end > *paddr_end) || (cropped_head && prev_start < paddr_min)) {
		/* This should never happen, but be safe */
		apfs_alert(sb, "recursion cropping physical extent 0x%llx-0x%llx", prev_start, prev_end);
		ret = -EFSCORRUPTED;
		goto out;
	}

	if (prev_end > *paddr_end) {
		ret = apfs_split_phys_ext(query, *paddr_end);
		if (ret) {
			apfs_err(sb, "failed to split pext at 0x%llx", *paddr_end);
			goto out;
		}
		/* The split may make the query invalid */
		apfs_free_query(query);
		cropped_tail = true;
		goto restart;
	}

	if (prev_start < paddr_min) {
		ret = apfs_split_phys_ext(query, paddr_min);
		if (ret) {
			apfs_err(sb, "failed to split pext at 0x%llx", paddr_min);
			goto out;
		}
		/* The split may make the query invalid */
		apfs_free_query(query);
		cropped_head = true;
		goto restart;
	}

	/* The extent to put already exists */
	ret = apfs_put_phys_extent(&prev_ext, query);
	if (ret)
		apfs_err(sb, "failed to put pext at 0x%llx", prev_ext.bno);
	*paddr_end = prev_start;

out:
	apfs_free_query(query);
	apfs_node_free(extref_root);
	return ret;
}

/**
 * apfs_range_put_reference - Put a reference to a physical range
 * @sb:		filesystem superblock
 * @paddr:	first block of the range
 * @length:	length of the range (in bytes)
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_range_put_reference(struct super_block *sb, u64 paddr, u64 length)
{
	u64 extent_end;
	int err;

	ASSERT(paddr);

	extent_end = paddr + (length >> sb->s_blocksize_bits);
	while (extent_end > paddr) {
		err = apfs_put_single_extent(sb, &extent_end, paddr);
		if (err)
			return err;
	}
	return 0;
}

/**
 * apfs_take_single_extent - Take a reference to a single extent
 * @sb:		filesystem superblock
 * @paddr_end:	first block after the extent to take
 * @paddr_min:	don't take references before this block
 *
 * Takes a reference to the physical extent range that ends in paddr. Sets
 * @paddr_end to the beginning of the extent, so that the caller can continue
 * with the previous one. Returns 0 on success, or a negative error code in
 * case of failure.
 */
static int apfs_take_single_extent(struct super_block *sb, u64 *paddr_end, u64 paddr_min)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root = NULL;
	struct apfs_key key;
	struct apfs_query *query = NULL;
	struct apfs_phys_extent prev_ext;
	u64 prev_start, prev_end;
	bool cropped_head = false, cropped_tail = false;
	struct apfs_file_extent tmp = {0}; /* TODO: clean up all the fake extent interfaces? */
	int ret;

	extref_root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_extentref_tree_oid), APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(extref_root)) {
		apfs_err(sb, "failed to read extref root 0x%llx", le64_to_cpu(vsb_raw->apfs_extentref_tree_oid));
		return PTR_ERR(extref_root);
	}
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(extref_root->object.oid);

	apfs_init_extent_key(*paddr_end - 1, &key);

restart:
	query = apfs_alloc_query(extref_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto out;
	}
	query->key = &key;
	query->flags = APFS_QUERY_EXTENTREF;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for paddr 0x%llx", *paddr_end - 1);
		goto out;
	}

	if (ret == -ENODATA) {
		/* The whole range to take is part of a snapshot */
		tmp.phys_block_num = paddr_min;
		tmp.len = (*paddr_end - paddr_min) << sb->s_blocksize_bits;
		ret = apfs_create_update_pext(query, &tmp, +1);
		*paddr_end = paddr_min;
		goto out;
	}

	ret = apfs_phys_ext_from_query(query, &prev_ext);
	if (ret) {
		apfs_err(sb, "bad pext record over paddr 0x%llx", *paddr_end - 1);
		goto out;
	}
	prev_start = prev_ext.bno;
	prev_end = prev_ext.bno + prev_ext.blkcount;
	if (prev_end < *paddr_end) {
		/* The extent to take is part of a snapshot */
		tmp.phys_block_num = MAX(prev_end, paddr_min);
		tmp.len = (*paddr_end - tmp.phys_block_num) << sb->s_blocksize_bits;
		ret = apfs_create_update_pext(query, &tmp, +1);
		*paddr_end = tmp.phys_block_num;
		goto out;
	}

	if ((cropped_tail && prev_end > *paddr_end) || (cropped_head && prev_start < paddr_min)) {
		/* This should never happen, but be safe */
		apfs_alert(sb, "recursion cropping physical extent 0x%llx-0x%llx", prev_start, prev_end);
		ret = -EFSCORRUPTED;
		goto out;
	}

	if (prev_end > *paddr_end) {
		ret = apfs_split_phys_ext(query, *paddr_end);
		if (ret) {
			apfs_err(sb, "failed to split pext at 0x%llx", *paddr_end);
			goto out;
		}
		/* The split may make the query invalid */
		apfs_free_query(query);
		cropped_tail = true;
		goto restart;
	}

	if (prev_start < paddr_min) {
		ret = apfs_split_phys_ext(query, paddr_min);
		if (ret) {
			apfs_err(sb, "failed to split pext at 0x%llx", paddr_min);
			goto out;
		}
		/* The split may make the query invalid */
		apfs_free_query(query);
		cropped_head = true;
		goto restart;
	}

	/* The extent to take already exists */
	ret = apfs_take_phys_extent(&prev_ext, query);
	if (ret)
		apfs_err(sb, "failed to take pext at 0x%llx", prev_ext.bno);
	*paddr_end = prev_start;

out:
	apfs_free_query(query);
	apfs_node_free(extref_root);
	return ret;
}

/**
 * apfs_range_take_reference - Take a reference to a physical range
 * @sb:		filesystem superblock
 * @paddr:	first block of the range
 * @length:	length of the range (in bytes)
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_range_take_reference(struct super_block *sb, u64 paddr, u64 length)
{
	u64 extent_end;
	int err;

	ASSERT(paddr);

	extent_end = paddr + (length >> sb->s_blocksize_bits);
	while (extent_end > paddr) {
		err = apfs_take_single_extent(sb, &extent_end, paddr);
		if (err)
			return err;
	}
	return 0;
}

/**
 * apfs_clone_single_extent - Make a copy of an extent in a dstream to a new one
 * @dstream:	old dstream
 * @new_id:	id of the new dstream
 * @log_addr:	logical address for the extent
 *
 * Duplicates the logical extent, and updates the references to the physical
 * extents as required. Sets @addr to the end of the extent, so that the caller
 * can continue in the same place. Returns 0 on success, or a negative error
 * code in case of failure.
 */
static int apfs_clone_single_extent(struct apfs_dstream_info *dstream, u64 new_id, u64 *log_addr)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_file_extent extent;
	int err;

	err = apfs_extent_read(dstream, *log_addr >> sb->s_blocksize_bits, &extent);
	if (err) {
		apfs_err(sb, "failed to read an extent to clone for dstream 0x%llx", dstream->ds_id);
		return err;
	}
	err = apfs_extent_create_record(sb, new_id, &extent);
	if (err) {
		apfs_err(sb, "failed to create extent record for clone of dstream 0x%llx", dstream->ds_id);
		return err;
	}

	if (!apfs_ext_is_hole(&extent)) {
		err = apfs_range_take_reference(sb, extent.phys_block_num, extent.len);
		if (err) {
			apfs_err(sb, "failed to take a reference to physical range 0x%llx-0x%llx", extent.phys_block_num, extent.len);
			return err;
		}
	}

	*log_addr += extent.len;
	return 0;
}

/**
 * apfs_clone_extents - Make a copy of all extents in a dstream to a new one
 * @dstream:	old dstream
 * @new_id:	id for the new dstream
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_clone_extents(struct apfs_dstream_info *dstream, u64 new_id)
{
	u64 next = 0;
	int err;

	while (next < dstream->ds_size) {
		err = apfs_clone_single_extent(dstream, new_id, &next);
		if (err)
			return err;
	}
	return 0;
}
