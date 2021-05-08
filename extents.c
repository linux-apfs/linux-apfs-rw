// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "apfs.h"

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
	char *raw = query->node->object.bh->b_data;
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
 * @inode:	inode that owns the record
 * @iblock:	logical number of the wanted block
 * @extent:	Return parameter.  The extent found.
 *
 * Finds and caches the extent record.  On success, returns a pointer to the
 * cache record; on failure, returns an error code.
 */
static int apfs_extent_read(struct inode *inode, sector_t iblock,
			    struct apfs_file_extent *extent)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent *cache = &ai->i_cached_extent;
	u64 iaddr = iblock << inode->i_blkbits;
	int ret = 0;

	spin_lock(&ai->i_extent_lock);
	if (iaddr >= cache->logical_addr &&
	    iaddr < cache->logical_addr + cache->len) {
		*extent = *cache;
		spin_unlock(&ai->i_extent_lock);
		return 0;
	}
	spin_unlock(&ai->i_extent_lock);

	/* We will search for the extent that covers iblock */
	apfs_init_file_extent_key(ai->i_extent_id, iaddr, &key);

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
		apfs_alert(sb, "bad extent record for inode 0x%llx",
			   apfs_ino(inode));
		goto done;
	}

	spin_lock(&ai->i_extent_lock);
	*cache = *extent;
	spin_unlock(&ai->i_extent_lock);

done:
	apfs_free_query(sb, query);
	return ret;
}

/* This does the same as apfs_get_block(), but without taking any locks */
int __apfs_get_block(struct inode *inode, sector_t iblock,
		     struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_file_extent ext;
	u64 blk_off, bno, map_len;
	int ret;

	ASSERT(!create);

	ret = apfs_extent_read(inode, iblock, &ext);
	if (ret)
		return ret;

	/* Find the block offset of iblock within the extent */
	blk_off = iblock - (ext.logical_addr >> inode->i_blkbits);

	/* Make sure we don't read past the extent boundaries */
	map_len = ext.len - (blk_off << inode->i_blkbits);
	if (bh_result->b_size > map_len)
		bh_result->b_size = map_len;

	/*
	 * Save the requested mapping length as apfs_map_bh() replaces it with
	 * the filesystem block size
	 */
	map_len = bh_result->b_size;
	/* Extents representing holes have block number 0 */
	if (ext.phys_block_num != 0) {
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
	int ret;

	down_read(&nxi->nx_big_sem);
	ret = __apfs_get_block(inode, iblock, bh_result, create);
	up_read(&nxi->nx_big_sem);
	return ret;
}

/**
 * apfs_update_extent - Create or update the extent record for an extent
 * @inode:	the vfs inode
 * @extent:	new in-memory file extent; the old physical location is set
 *		here on return so that the caller can release the blocks
 *
 * For now only creates or updates the record for @extent, and it won't work
 * for writes to the middle of it (TODO). Returns 0 on success or a negative
 * error code in case of failure.
 */
static int apfs_update_extent(struct inode *inode,
			      struct apfs_file_extent *extent)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_file_extent_key raw_key;
	struct apfs_file_extent_val raw_val;
	u64 extent_id = ai->i_extent_id;
	int ret;
	u64 prev_crypto, new_crypto;

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

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	/* The query is exact for now because we won't break up extents */
	query->flags = APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto fail;

	extent->phys_block_num = 0;
	extent->len = 0;
	if (!ret) {
		/* TODO: preserve the flags, although none are defined yet */
		if (apfs_extent_from_query(query, extent)) {
			ret = -EFSCORRUPTED;
			goto fail;
		}
		prev_crypto = extent->crypto_id;
	} else
		prev_crypto = 0;

	if (ret)
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
					&raw_val, sizeof(raw_val));
	else
		ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key),
					 &raw_val, sizeof(raw_val));

	if(prev_crypto != new_crypto) {
		ret = apfs_crypto_adj_refcnt(sb, prev_crypto, -1);
		if(ret)
			goto fail;
		ret = apfs_crypto_adj_refcnt(sb, new_crypto, 1);
		if(ret)
			goto fail;
	}

fail:
	apfs_free_query(sb, query);
	return ret;
}
#define APFS_UPDATE_EXTENTS_MAXOPS	(1 + 2 * APFS_CRYPTO_ADJ_REFCNT_MAXOPS())

/**
 * apfs_update_phys_extent - Create or update the physical record for an extent
 * @inode:	the vfs inode
 * @extent:	new in-memory file extent
 *
 * Only works for appending to extents, for now.  Returns 0 on success or a
 * negative error code in case of failure.
 */
static int apfs_update_phys_extent(struct inode *inode,
				   struct apfs_file_extent *extent)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_inode_info *ai = APFS_I(inode);
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
	raw_val.owning_obj_id = cpu_to_le64(ai->i_extent_id);
	raw_val.refcnt = cpu_to_le32(1);

	if (ret)
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
					&raw_val, sizeof(raw_val));
	else
		ret = apfs_btree_replace(query, &raw_key, sizeof(raw_key),
					 &raw_val, sizeof(raw_val));

fail:
	apfs_free_query(sb, query);
	apfs_node_put(extref_root);
	return ret;
}

/**
 * apfs_delete_phys_extent - Delete the physical record for an extent
 * @inode:	the vfs inode
 * @extent:	in-memory extent to delete
 *
 * Only works with single-block extents, for now.  Returns 0 on success or a
 * negative error code in case of failure.
 */
static int apfs_delete_phys_extent(struct inode *inode,
				   struct apfs_file_extent *extent)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *extref_root;
	struct apfs_key key;
	struct apfs_query *query = NULL;
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
	if (ret == -ENODATA)
		ret = -EFSCORRUPTED;
	if (!ret)
		ret = apfs_btree_remove(query);

fail:
	apfs_free_query(sb, query);
	apfs_node_put(extref_root);
	return ret;
}

/**
 * apfs_flush_extent_cache - Write the cached extent to the catalog, if dirty
 * @inode: the inode to flush
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_flush_extent_cache(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_file_extent *cache = &ai->i_cached_extent;
	struct apfs_file_extent ext = *cache;
	u64 old_blks, new_blks, byte_change;
	int err;

	if (!ai->i_extent_dirty)
		return 0;

	err = apfs_update_phys_extent(inode, &ext);
	if (err)
		return err;
	err = apfs_update_extent(inode, &ext);
	if (err)
		return err;

	byte_change = cache->len - ext.len;
	inode_add_bytes(inode, byte_change);

	old_blks = (ext.len + sb->s_blocksize - 1) >> inode->i_blkbits;
	new_blks = (cache->len + sb->s_blocksize - 1) >> inode->i_blkbits;
	le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, new_blks - old_blks);

	/* Release the old blocks if the extent is new. TODO: cloned files */
	if (ext.len && ext.phys_block_num != cache->phys_block_num) {
		int i;

		err = apfs_delete_phys_extent(inode, &ext);
		if (err)
			return err;

		for (i = 0; i < old_blks; ++i) {
			err = apfs_free_queue_insert(sb, ext.phys_block_num + i);
			if (err)
				return err;
		}
	}

	ai->i_extent_dirty = false;
	return 0;
}
#define APFS_FLUSH_EXTENT_CACHE	APFS_UPDATE_EXTENTS_MAXOPS

int apfs_get_new_block(struct inode *inode, sector_t iblock,
		       struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_file_extent *cache = &ai->i_cached_extent;
	u64 phys_bno, logical_addr, cache_blks;
	bool cache_is_tail;
	int err;

	ASSERT(create);

	/* nx_big_sem provides the locking for the cache here */
	cache_is_tail = cache->len && (inode->i_size <= cache->logical_addr + cache->len);
	cache_blks = (cache->len + sb->s_blocksize - 1) >> inode->i_blkbits;

	/* TODO: preallocate tail blocks, support sparse files */
	logical_addr = iblock << inode->i_blkbits;

	err = apfs_spaceman_allocate_block(sb, &phys_bno, false /* backwards */);
	if (err)
		return err;

	apfs_map_bh(bh_result, sb, phys_bno);
	get_bh(bh_result);

	if (cache_is_tail &&
	    logical_addr == cache->logical_addr + cache->len &&
	    phys_bno == cache->phys_block_num + cache_blks) {
		cache->len += sb->s_blocksize;
		ai->i_extent_dirty = true;
		return 0;
	}

	err = apfs_flush_extent_cache(inode);
	if (err)
		return err;

	cache->logical_addr = logical_addr;
	cache->phys_block_num = phys_bno;
	cache->len = sb->s_blocksize;
	ai->i_extent_dirty = true;
	return 0;
}
int APFS_GET_NEW_BLOCK_MAXOPS(void)
{
	return APFS_FLUSH_EXTENT_CACHE;
}
