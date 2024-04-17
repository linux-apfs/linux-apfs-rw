// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/mpage.h>
#include <linux/blk_types.h>
#include "apfs.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
#include <linux/sched/mm.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#include <linux/fileattr.h>
#endif

#define MAX_PFK_LEN	512

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)

static int apfs_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, apfs_get_block);
}

#else

static int apfs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, apfs_get_block);
}

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) /* Misses mpage_readpages() */

static void apfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, apfs_get_block);
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) */

static int apfs_readpages(struct file *file, struct address_space *mapping,
			  struct list_head *pages, unsigned int nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, apfs_get_block);
}

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) */

/**
 * apfs_create_dstream_rec - Create a data stream record
 * @dstream: data stream info
 *
 * Does nothing if the record already exists.  TODO: support cloned files.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_dstream_rec(struct apfs_dstream_info *dstream)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_dstream_id_key raw_key;
	struct apfs_dstream_id_val raw_val;
	int ret;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_dstream_id_key(dstream->ds_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret != -ENODATA) /* Either an error, or the record already exists */
		goto out;

	apfs_key_set_hdr(APFS_TYPE_DSTREAM_ID, dstream->ds_id, &raw_key);
	raw_val.refcnt = cpu_to_le32(1);
	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	if (ret) {
		apfs_err(sb, "insertion failed for id 0x%llx", dstream->ds_id);
		goto out;
	}
out:
	apfs_free_query(query);
	return ret;
}
#define APFS_CREATE_DSTREAM_REC_MAXOPS	1

static int apfs_check_dstream_refcnt(struct inode *inode);
static int apfs_put_dstream_rec(struct apfs_dstream_info *dstream);

/**
 * apfs_inode_create_exclusive_dstream - Make an inode's dstream not shared
 * @inode: the vfs inode
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_inode_create_exclusive_dstream(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = &ai->i_dstream;
	u64 new_id;
	int err;

	if (!ai->i_has_dstream || !dstream->ds_shared)
		return 0;

	/*
	 * The ds_shared field is not updated when the other user of the
	 * dstream puts it, so it could be a false positive. Check it again
	 * before actually putting the dstream. The double query is wasteful,
	 * but I don't know if it makes sense to optimize this (TODO).
	 */
	err = apfs_check_dstream_refcnt(inode);
	if (err) {
		apfs_err(sb, "failed to check refcnt for ino 0x%llx", apfs_ino(inode));
		return err;
	}
	if (!dstream->ds_shared)
		return 0;
	err = apfs_put_dstream_rec(dstream);
	if (err) {
		apfs_err(sb, "failed to put dstream for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	new_id = le64_to_cpu(vsb_raw->apfs_next_obj_id);
	le64_add_cpu(&vsb_raw->apfs_next_obj_id, 1);

	err = apfs_clone_extents(dstream, new_id);
	if (err) {
		apfs_err(sb, "failed clone extents for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	dstream->ds_id = new_id;
	err = apfs_create_dstream_rec(dstream);
	if (err) {
		apfs_err(sb, "failed to create dstream for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	dstream->ds_shared = false;
	return 0;
}

/**
 * apfs_inode_create_dstream_rec - Create the data stream record for an inode
 * @inode: the vfs inode
 *
 * Does nothing if the record already exists.  TODO: support cloned files.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_inode_create_dstream_rec(struct inode *inode)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	int err;

	if (ai->i_has_dstream)
		return apfs_inode_create_exclusive_dstream(inode);

	err = apfs_create_dstream_rec(&ai->i_dstream);
	if (err)
		return err;

	ai->i_has_dstream = true;
	return 0;
}

/**
 * apfs_dstream_adj_refcnt - Adjust dstream record refcount
 * @dstream:	data stream info
 * @delta:	desired change in reference count
 *
 * Deletes the record if the reference count goes to zero. Returns 0 on success
 * or a negative error code in case of failure.
 */
int apfs_dstream_adj_refcnt(struct apfs_dstream_info *dstream, u32 delta)
{
	struct super_block *sb = dstream->ds_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_dstream_id_val raw_val;
	void *raw = NULL;
	u32 refcnt;
	int ret;

	ASSERT(APFS_I(dstream->ds_inode)->i_has_dstream);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_dstream_id_key(dstream->ds_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		apfs_err(sb, "query failed for id 0x%llx", dstream->ds_id);
		if (ret == -ENODATA)
			ret = -EFSCORRUPTED;
		goto out;
	}

	if (query->len != sizeof(raw_val)) {
		apfs_err(sb, "bad value length (%d)", query->len);
		ret = -EFSCORRUPTED;
		goto out;
	}
	raw = query->node->object.data;
	raw_val = *(struct apfs_dstream_id_val *)(raw + query->off);
	refcnt = le32_to_cpu(raw_val.refcnt);

	refcnt += delta;
	if (refcnt == 0) {
		ret = apfs_btree_remove(query);
		if (ret)
			apfs_err(sb, "removal failed for id 0x%llx", dstream->ds_id);
		goto out;
	}

	raw_val.refcnt = cpu_to_le32(refcnt);
	ret = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */, &raw_val, sizeof(raw_val));
	if (ret)
		apfs_err(sb, "update failed for id 0x%llx", dstream->ds_id);
out:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_put_dstream_rec - Put a reference for a data stream record
 * @dstream: data stream info
 *
 * Deletes the record if the reference count goes to zero. Returns 0 on success
 * or a negative error code in case of failure.
 */
static int apfs_put_dstream_rec(struct apfs_dstream_info *dstream)
{
	struct apfs_inode_info *ai = APFS_I(dstream->ds_inode);

	if (!ai->i_has_dstream)
		return 0;
	return apfs_dstream_adj_refcnt(dstream, -1);
}

/**
 * apfs_create_crypto_rec - Create the crypto state record for an inode
 * @inode: the vfs inode
 *
 * Does nothing if the record already exists.  TODO: support cloned files.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_crypto_rec(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	struct apfs_query *query;
	struct apfs_crypto_state_key raw_key;
	int ret;

	if (inode->i_size || inode->i_blocks) /* Already has a dstream */
		return 0;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_crypto_state_key(dstream->ds_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret != -ENODATA) /* Either an error, or the record already exists */
		goto out;

	apfs_key_set_hdr(APFS_TYPE_CRYPTO_STATE, dstream->ds_id, &raw_key);
	if (sbi->s_dflt_pfk) {
		struct apfs_crypto_state_val *raw_val = sbi->s_dflt_pfk;
		unsigned int key_len = le16_to_cpu(raw_val->state.key_len);

		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), raw_val, sizeof(*raw_val) + key_len);
		if (ret)
			apfs_err(sb, "insertion failed for id 0x%llx", dstream->ds_id);
	} else {
		struct apfs_crypto_state_val raw_val;

		raw_val.refcnt = cpu_to_le32(1);
		raw_val.state.major_version = cpu_to_le16(APFS_WMCS_MAJOR_VERSION);
		raw_val.state.minor_version = cpu_to_le16(APFS_WMCS_MINOR_VERSION);
		raw_val.state.cpflags = 0;
		raw_val.state.persistent_class = cpu_to_le32(APFS_PROTECTION_CLASS_F);
		raw_val.state.key_os_version = 0;
		raw_val.state.key_revision = cpu_to_le16(1);
		raw_val.state.key_len = cpu_to_le16(0);
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
		if (ret)
			apfs_err(sb, "insertion failed for id 0x%llx", dstream->ds_id);
	}
out:
	apfs_free_query(query);
	return ret;
}
#define APFS_CREATE_CRYPTO_REC_MAXOPS	1

/**
 * apfs_dflt_key_class - Returns default key class for files in volume
 * @sb: volume superblock
 */
static unsigned int apfs_dflt_key_class(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	if (!sbi->s_dflt_pfk)
		return APFS_PROTECTION_CLASS_F;

	return le32_to_cpu(sbi->s_dflt_pfk->state.persistent_class);
}

/**
 * apfs_create_crypto_rec - Adjust crypto state record refcount
 * @sb: volume superblock
 * @crypto_id: crypto_id to adjust
 * @delta: desired change in reference count
 *
 * This function is used when adding or removing extents, as each extent holds
 * a reference to the crypto ID. It should also be used when removing inodes,
 * and in that case it should also remove the crypto record (TODO).
 */
int apfs_crypto_adj_refcnt(struct super_block *sb, u64 crypto_id, int delta)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;

	if (!crypto_id)
		return 0;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_crypto_state_key(crypto_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		apfs_err(sb, "query failed for id 0x%llx", crypto_id);
		goto out;
	}

	ret = apfs_query_join_transaction(query);
	if (ret) {
		apfs_err(sb, "query join failed");
		return ret;
	}
	raw = query->node->object.data;
	raw_val = (void *)raw + query->off;

	le32_add_cpu(&raw_val->refcnt, delta);

out:
	apfs_free_query(query);
	return ret;
}
int APFS_CRYPTO_ADJ_REFCNT_MAXOPS(void)
{
	return 1;
}

/**
 * apfs_crypto_set_key - Modify content of crypto state record
 * @sb: volume superblock
 * @crypto_id: crypto_id to modify
 * @new_val: new crypto state data; new_val->refcnt is overridden
 *
 * This function does not alter the inode's default protection class field.
 * It needs to be done separately if the class changes.
 */
static int apfs_crypto_set_key(struct super_block *sb, u64 crypto_id, struct apfs_crypto_state_val *new_val)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;
	unsigned int pfk_len;

	if (!crypto_id)
		return 0;

	pfk_len = le16_to_cpu(new_val->state.key_len);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_crypto_state_key(crypto_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		apfs_err(sb, "query failed for id 0x%llx", crypto_id);
		goto out;
	}
	raw = query->node->object.data;
	raw_val = (void *)raw + query->off;

	new_val->refcnt = raw_val->refcnt;

	ret = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */, new_val, sizeof(*new_val) + pfk_len);
	if (ret)
		apfs_err(sb, "update failed for id 0x%llx", crypto_id);

out:
	apfs_free_query(query);
	return ret;
}
#define APFS_CRYPTO_SET_KEY_MAXOPS	1

/**
 * apfs_crypto_get_key - Retrieve content of crypto state record
 * @sb: volume superblock
 * @crypto_id: crypto_id to modify
 * @val: result crypto state data
 * @max_len: maximum allowed value of val->state.key_len
 */
static int apfs_crypto_get_key(struct super_block *sb, u64 crypto_id, struct apfs_crypto_state_val *val,
			       unsigned int max_len)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;
	unsigned int pfk_len;

	if (!crypto_id)
		return -ENOENT;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_crypto_state_key(crypto_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto out;
	raw = query->node->object.data;
	raw_val = (void *)raw + query->off;

	pfk_len = le16_to_cpu(raw_val->state.key_len);
	if (pfk_len > max_len) {
		ret = -ENOSPC;
		goto out;
	}

	memcpy(val, raw_val, sizeof(*val) + pfk_len);

out:
	apfs_free_query(query);
	return ret;
}

int __apfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int flags, struct page **pagep, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	struct super_block *sb = inode->i_sb;
	struct page *page;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	struct folio *folio;
#endif
	struct buffer_head *bh, *head;
	unsigned int blocksize, block_start, block_end, from, to;
	pgoff_t index = pos >> PAGE_SHIFT;
	sector_t iblock = (sector_t)index << (PAGE_SHIFT - inode->i_blkbits);
	loff_t i_blks_end;
	int err;

	apfs_inode_join_transaction(sb, inode);

	err = apfs_inode_create_dstream_rec(inode);
	if (err) {
		apfs_err(sb, "failed to create dstream for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	if (apfs_vol_is_encrypted(sb)) {
		err = apfs_create_crypto_rec(inode);
		if (err) {
			apfs_err(sb, "crypto creation failed for ino 0x%llx", apfs_ino(inode));
			return err;
		}
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	flags = memalloc_nofs_save();
	page = grab_cache_page_write_begin(mapping, index);
	memalloc_nofs_restore(flags);
#else
	page = grab_cache_page_write_begin(mapping, index, flags | AOP_FLAG_NOFS);
#endif
	if (!page)
		return -ENOMEM;
	if (!page_has_buffers(page)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
		create_empty_buffers(page, sb->s_blocksize, 0);
#else
		folio = page_folio(page);
		bh = folio_buffers(folio);
		if (!bh)
			bh = create_empty_buffers(folio, sb->s_blocksize, 0);
#endif
	}

	/* CoW moves existing blocks, so read them but mark them as unmapped */
	head = page_buffers(page);
	blocksize = head->b_size;
	i_blks_end = (inode->i_size + sb->s_blocksize - 1) >> inode->i_blkbits;
	i_blks_end <<= inode->i_blkbits;
	if (i_blks_end >= pos) {
		from = pos & (PAGE_SIZE - 1);
		to = from + min(i_blks_end - pos, (loff_t)len);
	} else {
		/* TODO: deal with preallocated tail blocks */
		from = UINT_MAX;
		to = 0;
	}
	for (bh = head, block_start = 0; bh != head || !block_start;
	     block_start = block_end, bh = bh->b_this_page, ++iblock) {
		block_end = block_start + blocksize;
		if (to > block_start && from < block_end) {
			if (buffer_trans(bh))
				continue;
			if (!buffer_mapped(bh)) {
				err = __apfs_get_block(dstream, iblock, bh,
						       false /* create */);
				if (err) {
					apfs_err(sb, "failed to map block for ino 0x%llx", apfs_ino(inode));
					goto out_put_page;
				}
			}
			if (buffer_mapped(bh) && !buffer_uptodate(bh)) {
				get_bh(bh);
				lock_buffer(bh);
				bh->b_end_io = end_buffer_read_sync;
				apfs_submit_bh(REQ_OP_READ, 0, bh);
				wait_on_buffer(bh);
				if (!buffer_uptodate(bh)) {
					apfs_err(sb, "failed to read block for ino 0x%llx", apfs_ino(inode));
					err = -EIO;
					goto out_put_page;
				}
			}
			clear_buffer_mapped(bh);
		}
	}

	err = __block_write_begin(page, pos, len, apfs_get_new_block);
	if (err) {
		apfs_err(sb, "CoW failed in inode 0x%llx", apfs_ino(inode));
		goto out_put_page;
	}

	*pagep = page;
	return 0;

out_put_page:
	unlock_page(page);
	put_page(page);
	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static int apfs_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned int len,
			    struct page **pagep, void **fsdata)
#else
static int apfs_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned int len, unsigned int flags,
			    struct page **pagep, void **fsdata)
#endif
{
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	int blkcount = (len + sb->s_blocksize - 1) >> inode->i_blkbits;
	struct apfs_max_ops maxops;
	int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	unsigned int flags = 0;
#endif

	if (unlikely(pos >= APFS_MAX_FILE_SIZE))
		return -EFBIG;

	maxops.cat = APFS_CREATE_DSTREAM_REC_MAXOPS +
		     APFS_CREATE_CRYPTO_REC_MAXOPS +
		     APFS_UPDATE_INODE_MAXOPS() +
		     blkcount * APFS_GET_NEW_BLOCK_MAXOPS();
	maxops.blks = blkcount;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	err = __apfs_write_begin(file, mapping, pos, len, flags, pagep, fsdata);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

int __apfs_write_end(struct file *file, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied, struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	int ret, err;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	dstream->ds_size = i_size_read(inode);
	if (ret < len && pos + len > inode->i_size) {
		truncate_pagecache(inode, inode->i_size);
		err = apfs_truncate(dstream, inode->i_size);
		if (err) {
			apfs_err(inode->i_sb, "truncation failed for ino 0x%llx", apfs_ino(inode));
			return err;
		}
	}
	return ret;
}

static int apfs_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned int len, unsigned int copied,
			  struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct apfs_nx_transaction *trans = &APFS_NXI(sb)->nx_transaction;
	int ret, err;

	ret = __apfs_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < 0) {
		err = ret;
		goto fail;
	}

	if ((pos + ret) & (sb->s_blocksize - 1))
		trans->t_state |= APFS_NX_TRANS_INCOMPLETE_BLOCK;
	else
		trans->t_state &= ~APFS_NX_TRANS_INCOMPLETE_BLOCK;

	err = apfs_transaction_commit(sb);
	if (!err)
		return ret;

fail:
	apfs_transaction_abort(sb);
	return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
static void apfs_noop_invalidatepage(struct page *page, unsigned int offset, unsigned int length)
#else
static void apfs_noop_invalidate_folio(struct folio *folio, size_t offset, size_t length)
#endif
{
}

/* bmap is not implemented to avoid issues with CoW on swapfiles */
static const struct address_space_operations apfs_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	.dirty_folio	= block_dirty_folio,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	.set_page_dirty	= __set_page_dirty_buffers,
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	.read_folio	= apfs_read_folio,
#else
	.readpage	= apfs_readpage,
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	.readahead      = apfs_readahead,
#else
	.readpages      = apfs_readpages,
#endif

	.write_begin	= apfs_write_begin,
	.write_end	= apfs_write_end,

	/* The intention is to keep bhs around until the transaction is over */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	.invalidatepage	= apfs_noop_invalidatepage,
#else
	.invalidate_folio = apfs_noop_invalidate_folio,
#endif
};

/**
 * apfs_inode_set_ops - Set up an inode's operations
 * @inode:	vfs inode to set up
 * @rdev:	device id (0 if not a device file)
 * @compressed:	is this a compressed inode?
 *
 * For device files, also sets the device id to @rdev.
 */
static void apfs_inode_set_ops(struct inode *inode, dev_t rdev, bool compressed)
{
	/* A lot of operations still missing, of course */
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &apfs_file_inode_operations;
		if (compressed) {
			inode->i_fop = &apfs_compress_file_operations;
			inode->i_mapping->a_ops = &apfs_compress_aops;
		} else {
			inode->i_fop = &apfs_file_operations;
			inode->i_mapping->a_ops = &apfs_aops;
		}
		break;
	case S_IFDIR:
		inode->i_op = &apfs_dir_inode_operations;
		inode->i_fop = &apfs_dir_operations;
		break;
	case S_IFLNK:
		inode->i_op = &apfs_symlink_inode_operations;
		break;
	default:
		inode->i_op = &apfs_special_inode_operations;
		init_special_inode(inode, inode->i_mode, rdev);
		break;
	}
}

/**
 * apfs_inode_from_query - Read the inode found by a successful query
 * @query:	the query that found the record
 * @inode:	vfs inode to be filled with the read data
 *
 * Reads the inode record into @inode and performs some basic sanity checks,
 * mostly as a protection against crafted filesystems.  Returns 0 on success
 * or a negative error code otherwise.
 */
static int apfs_inode_from_query(struct apfs_query *query, struct inode *inode)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = &ai->i_dstream;
	struct apfs_inode_val *inode_val;
	char *raw = query->node->object.data;
	char *xval = NULL;
	int xlen;
	u32 rdev = 0, bsd_flags;
	bool compressed = false;

	if (query->len < sizeof(*inode_val))
		goto corrupted;

	inode_val = (struct apfs_inode_val *)(raw + query->off);

	ai->i_parent_id = le64_to_cpu(inode_val->parent_id);
	dstream->ds_id = le64_to_cpu(inode_val->private_id);
	inode->i_mode = le16_to_cpu(inode_val->mode);
	ai->i_key_class = le32_to_cpu(inode_val->default_protection_class);
	ai->i_int_flags = le64_to_cpu(inode_val->internal_flags);

	ai->i_saved_uid = le32_to_cpu(inode_val->owner);
	i_uid_write(inode, ai->i_saved_uid);
	ai->i_saved_gid = le32_to_cpu(inode_val->group);
	i_gid_write(inode, ai->i_saved_gid);

	ai->i_bsd_flags = bsd_flags = le32_to_cpu(inode_val->bsd_flags);
	if (bsd_flags & APFS_INOBSD_IMMUTABLE)
		inode->i_flags |= S_IMMUTABLE;
	if (bsd_flags & APFS_INOBSD_APPEND)
		inode->i_flags |= S_APPEND;

	if (!S_ISDIR(inode->i_mode)) {
		/*
		 * Directory inodes don't store their link count, so to provide
		 * it we would have to actually count the subdirectories. The
		 * HFS/HFS+ modules just leave it at 1, and so do we, for now.
		 */
		set_nlink(inode, le32_to_cpu(inode_val->nlink));
	} else {
		ai->i_nchildren = le32_to_cpu(inode_val->nchildren);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	inode->i_ctime = ns_to_timespec64(le64_to_cpu(inode_val->change_time));
#else
	inode_set_ctime_to_ts(inode, ns_to_timespec64(le64_to_cpu(inode_val->change_time)));
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	inode->i_atime = ns_to_timespec64(le64_to_cpu(inode_val->access_time));
	inode->i_mtime = ns_to_timespec64(le64_to_cpu(inode_val->mod_time));
#else
	inode_set_atime_to_ts(inode, ns_to_timespec64(le64_to_cpu(inode_val->access_time)));
	inode_set_mtime_to_ts(inode, ns_to_timespec64(le64_to_cpu(inode_val->mod_time)));
#endif
	ai->i_crtime = ns_to_timespec64(le64_to_cpu(inode_val->create_time));

	dstream->ds_size = inode->i_size = inode->i_blocks = 0;
	ai->i_has_dstream = false;
	if ((bsd_flags & APFS_INOBSD_COMPRESSED) && !S_ISDIR(inode->i_mode)) {
		if (!apfs_compress_get_size(inode, &inode->i_size)) {
			inode->i_blocks = (inode->i_size + 511) >> 9;
			compressed = true;
		}
	} else {
		xlen = apfs_find_xfield(inode_val->xfields,
					query->len - sizeof(*inode_val),
					APFS_INO_EXT_TYPE_DSTREAM, &xval);
		if (xlen >= sizeof(struct apfs_dstream)) {
			struct apfs_dstream *dstream_raw = (struct apfs_dstream *)xval;

			dstream->ds_size = inode->i_size = le64_to_cpu(dstream_raw->size);
			inode->i_blocks = le64_to_cpu(dstream_raw->alloced_size) >> 9;
			ai->i_has_dstream = true;
		}
	}
	xval = NULL;

	/* TODO: move each xfield read to its own function */
	dstream->ds_sparse_bytes = 0;
	xlen = apfs_find_xfield(inode_val->xfields, query->len - sizeof(*inode_val), APFS_INO_EXT_TYPE_SPARSE_BYTES, &xval);
	if (xlen >= sizeof(__le64)) {
		__le64 *sparse_bytes_p = (__le64 *)xval;

		dstream->ds_sparse_bytes = le64_to_cpup(sparse_bytes_p);
	}
	xval = NULL;

	rdev = 0;
	xlen = apfs_find_xfield(inode_val->xfields,
				query->len - sizeof(*inode_val),
				APFS_INO_EXT_TYPE_RDEV, &xval);
	if (xlen >= sizeof(__le32)) {
		__le32 *rdev_p = (__le32 *)xval;

		rdev = le32_to_cpup(rdev_p);
	}

	apfs_inode_set_ops(inode, rdev, compressed);
	return 0;

corrupted:
	apfs_err(inode->i_sb, "bad inode record for inode 0x%llx", apfs_ino(inode));
	return -EFSCORRUPTED;
}

/**
 * apfs_inode_lookup - Lookup an inode record in the catalog b-tree
 * @inode:	vfs inode to lookup
 *
 * Runs a catalog query for the apfs_ino(@inode) inode record; returns a pointer
 * to the query structure on success, or an error pointer in case of failure.
 */
static struct apfs_query *apfs_inode_lookup(const struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	int ret;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return ERR_PTR(-ENOMEM);
	apfs_init_inode_key(apfs_ino(inode), &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (!ret)
		return query;

	/* Don't complain if an orphan is already gone */
	if (!current_work() || ret != -ENODATA)
		apfs_err(sb, "query failed for id 0x%llx", apfs_ino(inode));
	apfs_free_query(query);
	return ERR_PTR(ret);
}

/**
 * apfs_test_inode - Check if the inode matches a 64-bit inode number
 * @inode:	inode to test
 * @cnid:	pointer to the inode number
 */
static int apfs_test_inode(struct inode *inode, void *cnid)
{
	u64 *ino = cnid;

	return apfs_ino(inode) == *ino;
}

/**
 * apfs_set_inode - Set a 64-bit inode number on the given inode
 * @inode:	inode to set
 * @cnid:	pointer to the inode number
 */
static int apfs_set_inode(struct inode *inode, void *cnid)
{
	apfs_set_ino(inode, *(u64 *)cnid);
	return 0;
}

/**
 * apfs_iget_locked - Wrapper for iget5_locked()
 * @sb:		filesystem superblock
 * @cnid:	64-bit inode number
 *
 * Works the same as iget_locked(), but can handle 64-bit inode numbers on
 * 32-bit architectures.
 */
static struct inode *apfs_iget_locked(struct super_block *sb, u64 cnid)
{
	return iget5_locked(sb, cnid, apfs_test_inode, apfs_set_inode, &cnid);
}

/**
 * apfs_check_dstream_refcnt - Check if an inode's dstream is shared
 * @inode:	the inode to check
 *
 * Sets the value of ds_shared for the inode's dstream. Returns 0 on success,
 * or a negative error code in case of failure.
 */
static int apfs_check_dstream_refcnt(struct inode *inode)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = &ai->i_dstream;
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query = NULL;
	struct apfs_dstream_id_val raw_val;
	void *raw = NULL;
	u32 refcnt;
	int ret;

	if (!ai->i_has_dstream) {
		dstream->ds_shared = false;
		return 0;
	}

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_dstream_id_key(dstream->ds_id, &query->key);
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret) {
		apfs_err(sb, "query failed for id 0x%llx", dstream->ds_id);
		if (ret == -ENODATA)
			ret = -EFSCORRUPTED;
		goto fail;
	}

	if (query->len != sizeof(raw_val)) {
		ret = -EFSCORRUPTED;
		goto fail;
	}
	raw = query->node->object.data;
	raw_val = *(struct apfs_dstream_id_val *)(raw + query->off);
	refcnt = le32_to_cpu(raw_val.refcnt);

	dstream->ds_shared = refcnt > 1;
fail:
	apfs_free_query(query);
	return ret;
}

/**
 * apfs_iget - Populate inode structures with metadata from disk
 * @sb:		filesystem superblock
 * @cnid:	inode number
 *
 * Populates the vfs inode and the corresponding apfs_inode_info structure.
 * Returns a pointer to the vfs inode in case of success, or an appropriate
 * error pointer otherwise.
 */
struct inode *apfs_iget(struct super_block *sb, u64 cnid)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct inode *inode;
	struct apfs_query *query;
	int err;

	inode = apfs_iget_locked(sb, cnid);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	down_read(&nxi->nx_big_sem);
	query = apfs_inode_lookup(inode);
	if (IS_ERR(query)) {
		err = PTR_ERR(query);
		/* Don't complain if an orphan is already gone */
		if (!current_work() || err != -ENODATA)
			apfs_err(sb, "lookup failed for ino 0x%llx", cnid);
		goto fail;
	}
	err = apfs_inode_from_query(query, inode);
	apfs_free_query(query);
	if (err)
		goto fail;
	err = apfs_check_dstream_refcnt(inode);
	if (err) {
		apfs_err(sb, "refcnt check failed for ino 0x%llx", cnid);
		goto fail;
	}
	up_read(&nxi->nx_big_sem);

	/* Allow the user to override the ownership */
	if (uid_valid(sbi->s_uid))
		inode->i_uid = sbi->s_uid;
	if (gid_valid(sbi->s_gid))
		inode->i_gid = sbi->s_gid;

	/* Inode flags are not important for now, leave them at 0 */
	unlock_new_inode(inode);
	return inode;

fail:
	up_read(&nxi->nx_big_sem);
	iget_failed(inode);
	return ERR_PTR(err);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) /* No statx yet... */

int apfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		 struct kstat *stat)
{
	struct inode *inode = d_inode(dentry);

	generic_fillattr(inode, stat);
	stat->dev = APFS_SB(inode->i_sb)->s_anon_dev;
	stat->ino = apfs_ino(inode);
	return 0;
}

#else /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) */

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
int apfs_getattr(const struct path *path, struct kstat *stat,
		 u32 request_mask, unsigned int query_flags)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
int apfs_getattr(struct user_namespace *mnt_userns,
		 const struct path *path, struct kstat *stat, u32 request_mask,
		 unsigned int query_flags)
#else
int apfs_getattr(struct mnt_idmap *idmap,
		 const struct path *path, struct kstat *stat, u32 request_mask,
		 unsigned int query_flags)
#endif
{
	struct inode *inode = d_inode(path->dentry);
	struct apfs_inode_info *ai = APFS_I(inode);

	stat->result_mask |= STATX_BTIME;
	stat->btime = ai->i_crtime;

	if (ai->i_bsd_flags & APFS_INOBSD_APPEND)
		stat->attributes |= STATX_ATTR_APPEND;
	if (ai->i_bsd_flags & APFS_INOBSD_IMMUTABLE)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	if (ai->i_bsd_flags & APFS_INOBSD_NODUMP)
		stat->attributes |= STATX_ATTR_NODUMP;
	if (ai->i_bsd_flags & APFS_INOBSD_COMPRESSED)
		stat->attributes |= STATX_ATTR_COMPRESSED;

	stat->attributes_mask |= STATX_ATTR_APPEND;
	stat->attributes_mask |= STATX_ATTR_IMMUTABLE;
	stat->attributes_mask |= STATX_ATTR_NODUMP;
	stat->attributes_mask |= STATX_ATTR_COMPRESSED;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	generic_fillattr(inode, stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	generic_fillattr(mnt_userns, inode, stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	generic_fillattr(idmap, inode, stat);
#else
	generic_fillattr(idmap, request_mask, inode, stat);
#endif

	stat->dev = APFS_SB(inode->i_sb)->s_anon_dev;
	stat->ino = apfs_ino(inode);
	return 0;
}

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) */

/**
 * apfs_build_inode_val - Allocate and initialize the value for an inode record
 * @inode:	vfs inode to record
 * @qname:	filename for primary link
 * @val_p:	on return, a pointer to the new on-disk value structure
 *
 * Returns the length of the value, or a negative error code in case of failure.
 */
static int apfs_build_inode_val(struct inode *inode, struct qstr *qname,
				struct apfs_inode_val **val_p)
{
	struct apfs_inode_val *val;
	struct apfs_x_field xkey;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	struct timespec64 ts;
#endif
	int total_xlen, val_len;
	bool is_device = S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode);
	__le32 rdev;

	/* The only required xfield is the name, and the id if it's a device */
	total_xlen = sizeof(struct apfs_xf_blob);
	total_xlen += sizeof(xkey) + round_up(qname->len + 1, 8);
	if (is_device)
		total_xlen += sizeof(xkey) + round_up(sizeof(rdev), 8);

	val_len = sizeof(*val) + total_xlen;
	val = kzalloc(val_len, GFP_KERNEL);
	if (!val)
		return -ENOMEM;

	val->parent_id = cpu_to_le64(APFS_I(inode)->i_parent_id);
	val->private_id = cpu_to_le64(apfs_ino(inode));

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	val->mod_time = cpu_to_le64(timespec64_to_ns(&inode->i_mtime));
#else
	ts = inode_get_mtime(inode);
	val->mod_time = cpu_to_le64(timespec64_to_ns(&ts));
#endif
	val->create_time = val->change_time = val->access_time = val->mod_time;

	if (S_ISDIR(inode->i_mode))
		val->nchildren = 0;
	else
		val->nlink = cpu_to_le32(1);

	val->owner = cpu_to_le32(i_uid_read(inode));
	val->group = cpu_to_le32(i_gid_read(inode));
	val->mode = cpu_to_le16(inode->i_mode);

	/* The buffer was just allocated: none of these functions should fail */
	apfs_init_xfields(val->xfields, total_xlen);
	xkey.x_type = APFS_INO_EXT_TYPE_NAME;
	xkey.x_flags = APFS_XF_DO_NOT_COPY;
	xkey.x_size = cpu_to_le16(qname->len + 1);
	apfs_insert_xfield(val->xfields, total_xlen, &xkey, qname->name);
	if (is_device) {
		rdev = cpu_to_le32(inode->i_rdev);
		xkey.x_type = APFS_INO_EXT_TYPE_RDEV;
		xkey.x_flags = 0; /* TODO: proper flags here? */
		xkey.x_size = cpu_to_le16(sizeof(rdev));
		apfs_insert_xfield(val->xfields, total_xlen, &xkey, &rdev);
	}

	*val_p = val;
	return val_len;
}

/*
 * apfs_inode_rename - Update the primary name reported in an inode record
 * @inode:	the in-memory inode
 * @new_name:	name of the new primary link (NULL if unchanged)
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_inode_rename(struct inode *inode, char *new_name,
			     struct apfs_query *query)
{
	char *raw = query->node->object.data;
	struct apfs_inode_val *new_val = NULL;
	int buflen, namelen;
	struct apfs_x_field xkey;
	int xlen;
	int err;

	if (!new_name)
		return 0;

	namelen = strlen(new_name) + 1; /* Count the null-termination */
	buflen = query->len;
	buflen += sizeof(struct apfs_x_field) + round_up(namelen, 8);
	new_val = kzalloc(buflen, GFP_KERNEL);
	if (!new_val)
		return -ENOMEM;
	memcpy(new_val, raw + query->off, query->len);

	/* TODO: can we assume that all inode records have an xfield blob? */
	xkey.x_type = APFS_INO_EXT_TYPE_NAME;
	xkey.x_flags = APFS_XF_DO_NOT_COPY;
	xkey.x_size = cpu_to_le16(namelen);
	xlen = apfs_insert_xfield(new_val->xfields, buflen - sizeof(*new_val),
				  &xkey, new_name);
	if (!xlen) {
		/* Buffer has enough space, but the metadata claims otherwise */
		apfs_err(inode->i_sb, "bad xfields on inode 0x%llx", apfs_ino(inode));
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Just remove the old record and create a new one */
	err = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */, new_val, sizeof(*new_val) + xlen);
	if (err)
		apfs_err(inode->i_sb, "update failed for ino 0x%llx", apfs_ino(inode));

fail:
	kfree(new_val);
	return err;
}
#define APFS_INODE_RENAME_MAXOPS	1

/**
 * apfs_create_dstream_xfield - Create the inode xfield for a new data stream
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_create_dstream_xfield(struct inode *inode,
				      struct apfs_query *query)
{
	char *raw = query->node->object.data;
	struct apfs_inode_val *new_val;
	struct apfs_dstream dstream_raw = {0};
	struct apfs_x_field xkey;
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	int xlen;
	int buflen;
	int err;

	buflen = query->len;
	buflen += sizeof(struct apfs_x_field) + sizeof(dstream_raw);
	new_val = kzalloc(buflen, GFP_KERNEL);
	if (!new_val)
		return -ENOMEM;
	memcpy(new_val, raw + query->off, query->len);

	dstream_raw.size = cpu_to_le64(inode->i_size);
	dstream_raw.alloced_size = cpu_to_le64(apfs_alloced_size(dstream));
	if (apfs_vol_is_encrypted(inode->i_sb))
		dstream_raw.default_crypto_id = cpu_to_le64(dstream->ds_id);

	/* TODO: can we assume that all inode records have an xfield blob? */
	xkey.x_type = APFS_INO_EXT_TYPE_DSTREAM;
	xkey.x_flags = APFS_XF_SYSTEM_FIELD;
	xkey.x_size = cpu_to_le16(sizeof(dstream_raw));
	xlen = apfs_insert_xfield(new_val->xfields, buflen - sizeof(*new_val),
				  &xkey, &dstream_raw);
	if (!xlen) {
		/* Buffer has enough space, but the metadata claims otherwise */
		apfs_err(inode->i_sb, "bad xfields on inode 0x%llx", apfs_ino(inode));
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Just remove the old record and create a new one */
	err = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */, new_val, sizeof(*new_val) + xlen);
	if (err)
		apfs_err(inode->i_sb, "update failed for ino 0x%llx", apfs_ino(inode));

fail:
	kfree(new_val);
	return err;
}
#define APFS_CREATE_DSTREAM_XFIELD_MAXOPS	1

/**
 * apfs_inode_resize - Update the sizes reported in an inode record
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_inode_resize(struct inode *inode, struct apfs_query *query)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	char *raw;
	struct apfs_inode_val *inode_raw;
	char *xval;
	int xlen;
	int err;

	/* All dstream records must have a matching xfield, even if empty */
	if (!ai->i_has_dstream)
		return 0;

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(inode->i_sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;
	inode_raw = (void *)raw + query->off;

	xlen = apfs_find_xfield(inode_raw->xfields,
				query->len - sizeof(*inode_raw),
				APFS_INO_EXT_TYPE_DSTREAM, &xval);

	if (xlen) {
		struct apfs_dstream *dstream;

		if (xlen != sizeof(*dstream)) {
			apfs_err(inode->i_sb, "bad xlen (%d) on inode 0x%llx", xlen, apfs_ino(inode));
			return -EFSCORRUPTED;
		}
		dstream = (struct apfs_dstream *)xval;

		/* TODO: count bytes read and written */
		dstream->size = cpu_to_le64(inode->i_size);
		dstream->alloced_size = cpu_to_le64(apfs_alloced_size(&ai->i_dstream));
		return 0;
	}
	/* This inode has no dstream xfield, so we need to create it */
	return apfs_create_dstream_xfield(inode, query);
}
#define APFS_INODE_RESIZE_MAXOPS	(1 + APFS_CREATE_DSTREAM_XFIELD_MAXOPS)

/**
 * apfs_create_sparse_xfield - Create an inode xfield to count sparse bytes
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_create_sparse_xfield(struct inode *inode, struct apfs_query *query)
{
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	char *raw = query->node->object.data;
	struct apfs_inode_val *new_val;
	__le64 sparse_bytes;
	struct apfs_x_field xkey;
	int xlen;
	int buflen;
	int err;

	buflen = query->len;
	buflen += sizeof(struct apfs_x_field) + sizeof(sparse_bytes);
	new_val = kzalloc(buflen, GFP_KERNEL);
	if (!new_val)
		return -ENOMEM;
	memcpy(new_val, raw + query->off, query->len);

	sparse_bytes = cpu_to_le64(dstream->ds_sparse_bytes);

	/* TODO: can we assume that all inode records have an xfield blob? */
	xkey.x_type = APFS_INO_EXT_TYPE_SPARSE_BYTES;
	xkey.x_flags = APFS_XF_SYSTEM_FIELD | APFS_XF_CHILDREN_INHERIT;
	xkey.x_size = cpu_to_le16(sizeof(sparse_bytes));
	xlen = apfs_insert_xfield(new_val->xfields, buflen - sizeof(*new_val), &xkey, &sparse_bytes);
	if (!xlen) {
		/* Buffer has enough space, but the metadata claims otherwise */
		apfs_err(inode->i_sb, "bad xfields on inode 0x%llx", apfs_ino(inode));
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Just remove the old record and create a new one */
	err = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */, new_val, sizeof(*new_val) + xlen);
	if (err)
		apfs_err(inode->i_sb, "update failed for ino 0x%llx", apfs_ino(inode));

fail:
	kfree(new_val);
	return err;
}

/**
 * apfs_inode_resize_sparse - Update sparse byte count reported in inode record
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 *
 * TODO: should the xfield be removed if the count reaches 0? Should the inode
 * flag change?
 */
static int apfs_inode_resize_sparse(struct inode *inode, struct apfs_query *query)
{
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	char *raw;
	struct apfs_inode_val *inode_raw;
	char *xval;
	int xlen;
	int err;

	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(inode->i_sb, "query join failed");
		return err;
	}
	raw = query->node->object.data;
	inode_raw = (void *)raw + query->off;

	xlen = apfs_find_xfield(inode_raw->xfields,
				query->len - sizeof(*inode_raw),
				APFS_INO_EXT_TYPE_SPARSE_BYTES, &xval);
	if (!xlen && !dstream->ds_sparse_bytes)
		return 0;

	if (xlen) {
		__le64 *sparse_bytes_p;

		if (xlen != sizeof(*sparse_bytes_p)) {
			apfs_err(inode->i_sb, "bad xlen (%d) on inode 0x%llx", xlen, apfs_ino(inode));
			return -EFSCORRUPTED;
		}
		sparse_bytes_p = (__le64 *)xval;

		*sparse_bytes_p = cpu_to_le64(dstream->ds_sparse_bytes);
		return 0;
	}
	return apfs_create_sparse_xfield(inode, query);
}

/**
 * apfs_update_inode - Update an existing inode record
 * @inode:	the modified in-memory inode
 * @new_name:	name of the new primary link (NULL if unchanged)
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_update_inode(struct inode *inode, char *new_name)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = &ai->i_dstream;
	struct apfs_query *query;
	struct apfs_btree_node_phys *node_raw;
	struct apfs_inode_val *inode_raw;
	int err;

	err = apfs_flush_extent_cache(dstream);
	if (err) {
		apfs_err(sb, "extent cache flush failed for inode 0x%llx", apfs_ino(inode));
		return err;
	}

	query = apfs_inode_lookup(inode);
	if (IS_ERR(query)) {
		apfs_err(sb, "lookup failed for ino 0x%llx", apfs_ino(inode));
		return PTR_ERR(query);
	}

	/* TODO: copy the record to memory and make all xfield changes there */
	err = apfs_inode_rename(inode, new_name, query);
	if (err) {
		apfs_err(sb, "rename failed for ino 0x%llx", apfs_ino(inode));
		goto fail;
	}

	err = apfs_inode_resize(inode, query);
	if (err) {
		apfs_err(sb, "resize failed for ino 0x%llx", apfs_ino(inode));
		goto fail;
	}

	err = apfs_inode_resize_sparse(inode, query);
	if (err) {
		apfs_err(sb, "sparse resize failed for ino 0x%llx", apfs_ino(inode));
		goto fail;
	}
	if (dstream->ds_sparse_bytes)
		ai->i_int_flags |= APFS_INODE_IS_SPARSE;

	/* TODO: just use apfs_btree_replace()? */
	err = apfs_query_join_transaction(query);
	if (err) {
		apfs_err(sb, "query join failed");
		goto fail;
	}
	node_raw = (void *)query->node->object.data;
	apfs_assert_in_transaction(sb, &node_raw->btn_o);
	inode_raw = (void *)node_raw + query->off;

	inode_raw->parent_id = cpu_to_le64(ai->i_parent_id);
	inode_raw->private_id = cpu_to_le64(dstream->ds_id);
	inode_raw->mode = cpu_to_le16(inode->i_mode);
	inode_raw->owner = cpu_to_le32(i_uid_read(inode));
	inode_raw->group = cpu_to_le32(i_gid_read(inode));
	inode_raw->default_protection_class = cpu_to_le32(ai->i_key_class);
	inode_raw->internal_flags = cpu_to_le64(ai->i_int_flags);
	inode_raw->bsd_flags = cpu_to_le32(ai->i_bsd_flags);

	/* Don't persist the uid/gid provided by the user on mount */
	if (uid_valid(sbi->s_uid))
		inode_raw->owner = cpu_to_le32(ai->i_saved_uid);
	if (gid_valid(sbi->s_gid))
		inode_raw->group = cpu_to_le32(ai->i_saved_gid);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	inode_raw->change_time = cpu_to_le64(timespec64_to_ns(&inode->i_ctime));
#else
	struct timespec64 ictime = inode_get_ctime(inode);
	inode_raw->change_time = cpu_to_le64(timespec64_to_ns(&ictime));
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	inode_raw->access_time = cpu_to_le64(timespec64_to_ns(&inode->i_atime));
	inode_raw->mod_time = cpu_to_le64(timespec64_to_ns(&inode->i_mtime));
#else
	struct timespec64 ts = inode_get_mtime(inode);
	inode_raw->mod_time = cpu_to_le64(timespec64_to_ns(&ts));
	ts = inode_get_atime(inode);
	inode_raw->access_time = cpu_to_le64(timespec64_to_ns(&ts));
#endif
	inode_raw->create_time = cpu_to_le64(timespec64_to_ns(&ai->i_crtime));

	if (S_ISDIR(inode->i_mode)) {
		inode_raw->nchildren = cpu_to_le32(ai->i_nchildren);
	} else {
		/* The remaining link for orphan inodes is not counted */
		inode_raw->nlink = cpu_to_le32(inode->i_nlink);
	}

fail:
	apfs_free_query(query);
	return err;
}
int APFS_UPDATE_INODE_MAXOPS(void)
{
	return APFS_INODE_RENAME_MAXOPS + APFS_INODE_RESIZE_MAXOPS + 1;
}

/**
 * apfs_delete_inode - Delete an inode record
 * @inode: the vfs inode to delete
 *
 * Returns 0 on success or a negative error code in case of failure, which may
 * be -EAGAIN if the inode was not deleted in full.
 */
static int apfs_delete_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = NULL;
	struct apfs_query *query;
	u64 old_dstream_id;
	int ret;

	ret = apfs_delete_all_xattrs(inode);
	if (ret) {
		apfs_err(sb, "xattr deletion failed for ino 0x%llx", apfs_ino(inode));
		return ret;
	}

	dstream = &ai->i_dstream;
	old_dstream_id = dstream->ds_id;

	/*
	 * This is very wasteful since all the new extents and references will
	 * get deleted right away, but it only affects clones, so I don't see a
	 * big reason to improve it (TODO)
	 */
	ret = apfs_inode_create_exclusive_dstream(inode);
	if (ret) {
		apfs_err(sb, "dstream creation failed for ino 0x%llx", apfs_ino(inode));
		return ret;
	}

	/* TODO: what about partial deletion of xattrs? Is that allowed? */
	ret = apfs_inode_delete_front(inode);
	if (ret) {
		/*
		 * If the inode had too many extents, only the first few get
		 * deleted and the inode remains in the orphan list for now.
		 * I don't know why the deletion starts at the front, but it
		 * seems to be what the official driver does.
		 */
		if (ret != -EAGAIN) {
			apfs_err(sb, "head deletion failed for ino 0x%llx", apfs_ino(inode));
			return ret;
		}
		if (dstream->ds_id != old_dstream_id) {
			ret = apfs_update_inode(inode, NULL /* new_name */);
			if (ret) {
				apfs_err(sb, "dstream id update failed for orphan 0x%llx", apfs_ino(inode));
				return ret;
			}
		}
		return -EAGAIN;
	}

	ret = apfs_put_dstream_rec(dstream);
	if (ret) {
		apfs_err(sb, "failed to put dstream for ino 0x%llx", apfs_ino(inode));
		return ret;
	}
	dstream = NULL;
	ai->i_has_dstream = false;

	query = apfs_inode_lookup(inode);
	if (IS_ERR(query)) {
		apfs_err(sb, "lookup failed for ino 0x%llx", apfs_ino(inode));
		return PTR_ERR(query);
	}
	ret = apfs_btree_remove(query);
	apfs_free_query(query);
	if (ret) {
		apfs_err(sb, "removal failed for ino 0x%llx", apfs_ino(inode));
		return ret;
	}

	ai->i_cleaned = true;
	return ret;
}
#define APFS_DELETE_INODE_MAXOPS	1

/**
 * apfs_clean_single_orphan - Clean the given orphan file
 * @inode:	inode for the file to clean
 *
 * Returns 0 on success or a negative error code in case of failure, which may
 * be -EAGAIN if the file could not be deleted in full.
 */
static int apfs_clean_single_orphan(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops = {0}; /* TODO: rethink this stuff */
	u64 ino = apfs_ino(inode);
	bool eagain = false;
	int err;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	err = apfs_delete_inode(inode);
	if (err) {
		if (err != -EAGAIN) {
			apfs_err(sb, "failed to delete orphan 0x%llx", ino);
			goto fail;
		}
		eagain = true;
	} else {
		err = apfs_delete_orphan_link(inode);
		if (err) {
			apfs_err(sb, "failed to unlink orphan 0x%llx", ino);
			goto fail;
		}
	}
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return eagain ? -EAGAIN : 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_clean_any_orphan - Pick an orphan and delete as much as reasonable
 * @sb:		filesystem superblock
 *
 * Returns 0 on success, or a negative error code in case of failure, which may
 * be -ENODATA if there are no more orphan files or -EAGAIN if a file could not
 * be deleted in full.
 */
static int apfs_clean_any_orphan(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct inode *inode = NULL;
	int err;
	u64 ino;

	down_read(&nxi->nx_big_sem);
	err = apfs_any_orphan_ino(sb, &ino);
	up_read(&nxi->nx_big_sem);
	if (err) {
		if (err == -ENODATA)
			return -ENODATA;
		apfs_err(sb, "failed to find orphan inode numbers");
		return err;
	}

	inode = apfs_iget(sb, ino);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		if (err != -ENODATA) {
			apfs_err(sb, "iget failed for orphan 0x%llx", ino);
			return err;
		}
		/*
		 * This happens rarely for files with no extents, if we hit a
		 * race with ->evict_inode(). Not a problem: the file is gone.
		 */
		apfs_notice(sb, "orphan 0x%llx not found", ino);
		return 0;
	}

	if (atomic_read(&inode->i_count) > 1)
		goto out;
	err = apfs_clean_single_orphan(inode);
	if (err && err != -EAGAIN) {
		apfs_err(sb, "failed to clean orphan 0x%llx", ino);
		goto out;
	}
out:
	iput(inode);
	return err;
}

/**
 * apfs_clean_orphans - Delete as many orphan files as is reasonable
 * @sb: filesystem superblock
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_clean_orphans(struct super_block *sb)
{
	int ret, i;

	for (i = 0; i < 100; ++i) {
		ret = apfs_clean_any_orphan(sb);
		if (ret) {
			if (ret == -ENODATA)
				return 0;
			if (ret == -EAGAIN)
				break;
			apfs_err(sb, "failed to delete an orphan file");
			return ret;
		}
	}

	/*
	 * If a file is too big, or if there are too many files, take a break
	 * and continue later.
	 */
	if (atomic_read(&sb->s_active) != 0)
		schedule_work(&APFS_SB(sb)->s_orphan_cleanup_work);
	return 0;
}

void apfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_inode_info *ai = APFS_I(inode);
	int err;

	if (is_bad_inode(inode) || inode->i_nlink || ai->i_cleaned)
		goto out;

	if (!ai->i_has_dstream || ai->i_dstream.ds_size == 0) {
		/* For files with no extents, scheduled cleanup wastes time */
		err = apfs_clean_single_orphan(inode);
		if (err)
			apfs_err(sb, "failed to clean orphan 0x%llx (err:%d)", apfs_ino(inode), err);
		goto out;
	}

	/*
	 * If the inode still has extents then schedule cleanup for the rest
	 * of it. Not during unmount though: completing all cleanup could take
	 * a while so just leave future mounts to handle the orphans.
	 */
	if (atomic_read(&sb->s_active))
		schedule_work(&APFS_SB(sb)->s_orphan_cleanup_work);
out:
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

void apfs_orphan_cleanup_work(struct work_struct *work)
{
	struct super_block *sb = NULL;
	struct apfs_sb_info *sbi = NULL;
	struct inode *priv = NULL;
	int err;

	sbi = container_of(work, struct apfs_sb_info, s_orphan_cleanup_work);
	priv = sbi->s_private_dir;
	sb = priv->i_sb;

	if (sb->s_flags & SB_RDONLY) {
		apfs_alert(sb, "attempt to flush orphans in read-only mount");
		return;
	}

	err = apfs_clean_orphans(sb);
	if (err)
		apfs_err(sb, "orphan cleanup failed (err:%d)", err);
}

/**
 * apfs_insert_inode_locked - Wrapper for insert_inode_locked4()
 * @inode: vfs inode to insert in cache
 *
 * Works the same as insert_inode_locked(), but can handle 64-bit inode numbers
 * on 32-bit architectures.
 */
static int apfs_insert_inode_locked(struct inode *inode)
{
	u64 cnid = apfs_ino(inode);

	return insert_inode_locked4(inode, cnid, apfs_test_inode, &cnid);
}

/**
 * apfs_new_inode - Create a new in-memory inode
 * @dir:	parent inode
 * @mode:	mode bits for the new inode
 * @rdev:	device id (0 if not a device file)
 *
 * Returns a pointer to the new vfs inode on success, or an error pointer in
 * case of failure.
 */
struct inode *apfs_new_inode(struct inode *dir, umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct inode *inode;
	struct apfs_inode_info *ai;
	struct apfs_dstream_info *dstream;
	u64 cnid;
	struct timespec64 now;

	/* Updating on-disk structures here is odd, but it works for now */
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	ai = APFS_I(inode);
	dstream = &ai->i_dstream;

	cnid = le64_to_cpu(vsb_raw->apfs_next_obj_id);
	le64_add_cpu(&vsb_raw->apfs_next_obj_id, 1);
	apfs_set_ino(inode, cnid);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	inode_init_owner(inode, dir, mode);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	inode_init_owner(&init_user_ns, inode, dir, mode);
#else
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#endif

	ai->i_saved_uid = i_uid_read(inode);
	ai->i_saved_gid = i_gid_read(inode);
	ai->i_parent_id = apfs_ino(dir);
	set_nlink(inode, 1);
	ai->i_nchildren = 0;
	if (apfs_vol_is_encrypted(sb) && S_ISREG(mode))
		ai->i_key_class = apfs_dflt_key_class(sb);
	else
		ai->i_key_class = 0;
	ai->i_int_flags = APFS_INODE_NO_RSRC_FORK;
	ai->i_bsd_flags = 0;

	ai->i_has_dstream = false;
	dstream->ds_id = cnid;
	dstream->ds_size = 0;
	dstream->ds_sparse_bytes = 0;
	dstream->ds_shared = false;

	now = current_time(inode);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	inode->i_atime = inode->i_mtime = inode->i_ctime = ai->i_crtime = now;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	inode_set_ctime_to_ts(inode, now);
	inode->i_atime = inode->i_mtime = ai->i_crtime = now;
#else
	ai->i_crtime = simple_inode_init_ts(inode);
#endif
	vsb_raw->apfs_last_mod_time = cpu_to_le64(timespec64_to_ns(&now));

	if (S_ISREG(mode))
		le64_add_cpu(&vsb_raw->apfs_num_files, 1);
	else if (S_ISDIR(mode))
		le64_add_cpu(&vsb_raw->apfs_num_directories, 1);
	else if (S_ISLNK(mode))
		le64_add_cpu(&vsb_raw->apfs_num_symlinks, 1);
	else
		le64_add_cpu(&vsb_raw->apfs_num_other_fsobjects, 1);

	if (apfs_insert_inode_locked(inode)) {
		/* The inode number should have been free, but wasn't */
		apfs_err(sb, "next obj_id (0x%llx) not free", cnid);
		make_bad_inode(inode);
		iput(inode);
		return ERR_PTR(-EFSCORRUPTED);
	}

	/* No need to dirty the inode, we'll write it to disk right away */
	apfs_inode_set_ops(inode, rdev, false /* compressed */);
	return inode;
}

/**
 * apfs_create_inode_rec - Create an inode record in the catalog b-tree
 * @sb:		filesystem superblock
 * @inode:	vfs inode to record
 * @dentry:	dentry for primary link
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_create_inode_rec(struct super_block *sb, struct inode *inode,
			  struct dentry *dentry)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_query *query;
	struct apfs_inode_key raw_key;
	struct apfs_inode_val *raw_val;
	int val_len;
	int ret;

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_inode_key(apfs_ino(inode), &query->key);
	query->flags |= APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA) {
		apfs_err(sb, "query failed for ino 0x%llx", apfs_ino(inode));
		goto fail;
	}

	apfs_key_set_hdr(APFS_TYPE_INODE, apfs_ino(inode), &raw_key);

	val_len = apfs_build_inode_val(inode, &dentry->d_name, &raw_val);
	if (val_len < 0) {
		ret = val_len;
		goto fail;
	}

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key), raw_val, val_len);
	if (ret)
		apfs_err(sb, "insertion failed for ino 0x%llx", apfs_ino(inode));
	kfree(raw_val);

fail:
	apfs_free_query(query);
	return ret;
}
int APFS_CREATE_INODE_REC_MAXOPS(void)
{
	return 1;
}

/**
 * apfs_setsize - Change the size of a regular file
 * @inode:	the vfs inode
 * @new_size:	the new size
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_setsize(struct inode *inode, loff_t new_size)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	int err;

	if (new_size == inode->i_size)
		return 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	inode->i_mtime = inode->i_ctime = current_time(inode);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
	inode->i_mtime = inode_set_ctime_current(inode);
#else
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
#endif

	err = apfs_inode_create_dstream_rec(inode);
	if (err) {
		apfs_err(sb, "failed to create dstream for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	/* Must be called before i_size is changed */
	err = apfs_truncate(dstream, new_size);
	if (err) {
		apfs_err(sb, "truncation failed for ino 0x%llx", apfs_ino(inode));
		return err;
	}

	truncate_setsize(inode, new_size);
	dstream->ds_size = i_size_read(inode);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
int apfs_setattr(struct dentry *dentry, struct iattr *iattr)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
int apfs_setattr(struct user_namespace *mnt_userns,
		 struct dentry *dentry, struct iattr *iattr)
#else
int apfs_setattr(struct mnt_idmap *idmap,
		 struct dentry *dentry, struct iattr *iattr)
#endif
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	bool resizing = S_ISREG(inode->i_mode) && (iattr->ia_valid & ATTR_SIZE);
	int err;

	if (resizing && iattr->ia_size > APFS_MAX_FILE_SIZE)
		return -EFBIG;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	err = setattr_prepare(dentry, iattr);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	err = setattr_prepare(&init_user_ns, dentry, iattr);
#else
	err = setattr_prepare(&nop_mnt_idmap, dentry, iattr);
#endif
	if (err)
		return err;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	/* TODO: figure out why ->write_inode() isn't firing */
	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	apfs_inode_join_transaction(sb, inode);

	if (resizing) {
		err = apfs_setsize(inode, iattr->ia_size);
		if (err) {
			apfs_err(sb, "setsize failed for ino 0x%llx", apfs_ino(inode));
			goto fail;
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	setattr_copy(inode, iattr);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	setattr_copy(&init_user_ns, inode, iattr);
#else
	setattr_copy(&nop_mnt_idmap, inode, iattr);
#endif

	mark_inode_dirty(inode);
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/* TODO: this only seems to be necessary because ->write_inode() isn't firing */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
int apfs_update_time(struct inode *inode, struct timespec64 *time, int flags)
#else
int apfs_update_time(struct inode *inode, int flags)
#endif
{
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	int err;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	apfs_inode_join_transaction(sb, inode);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	generic_update_time(inode, time, flags);
#else
	generic_update_time(inode, flags);
#endif

	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static int apfs_ioc_set_dflt_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	unsigned int key_len;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	key_len = le16_to_cpu(pfk_hdr.key_len);
	if (key_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + key_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;
	if (__copy_from_user(&pfk->state, user_pfk, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}
	pfk->refcnt = cpu_to_le32(1);

	down_write(&nxi->nx_big_sem);

	if (sbi->s_dflt_pfk)
		kfree(sbi->s_dflt_pfk);
	sbi->s_dflt_pfk = pfk;

	up_write(&nxi->nx_big_sem);

	return 0;
}

static int apfs_ioc_set_dir_class(struct file *file, u32 __user *user_class)
{
	struct inode *inode = file_inode(file);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	u32 class;
	int err;

	if (get_user(class, user_class))
		return -EFAULT;

	ai->i_key_class = class;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	apfs_inode_join_transaction(sb, inode);
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static int apfs_ioc_set_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_dstream_info *dstream = &ai->i_dstream;
	struct apfs_max_ops maxops;
	unsigned int key_len, key_class;
	int err;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	key_len = le16_to_cpu(pfk_hdr.key_len);
	if (key_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + key_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;
	if (__copy_from_user(&pfk->state, user_pfk, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}
	pfk->refcnt = cpu_to_le32(1);

	maxops.cat = APFS_CRYPTO_SET_KEY_MAXOPS + APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err) {
		kfree(pfk);
		return err;
	}

	err = apfs_crypto_set_key(sb, dstream->ds_id, pfk);
	if (err)
		goto fail;

	key_class = le32_to_cpu(pfk_hdr.persistent_class);
	if (ai->i_key_class != key_class) {
		ai->i_key_class = key_class;
		apfs_inode_join_transaction(sb, inode);
	}

	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	kfree(pfk);
	return 0;

fail:
	apfs_transaction_abort(sb);
	kfree(pfk);
	return err;
}

static int apfs_ioc_get_class(struct file *file, u32 __user *user_class)
{
	struct inode *inode = file_inode(file);
	struct apfs_inode_info *ai = APFS_I(inode);
	u32 class;

	class = ai->i_key_class;
	if (put_user(class, user_class))
		return -EFAULT;
	return 0;
}

static int apfs_ioc_get_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	unsigned int max_len, key_len;
	struct apfs_dstream_info *dstream = &APFS_I(inode)->i_dstream;
	int err;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	max_len = le16_to_cpu(pfk_hdr.key_len);
	if (max_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + max_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;

	down_read(&nxi->nx_big_sem);

	err = apfs_crypto_get_key(sb, dstream->ds_id, pfk, max_len);
	if (err)
		goto fail;

	up_read(&nxi->nx_big_sem);

	key_len = le16_to_cpu(pfk->state.key_len);
	if (__copy_to_user(user_pfk, &pfk->state, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}

	kfree(pfk);
	return 0;

fail:
	up_read(&nxi->nx_big_sem);
	kfree(pfk);
	return err;
}

/*
 * Older kernels have no vfs_ioc_setflags_prepare(), so don't implement the
 * SETFLAGS/GETFLAGS ioctls there. It should be easy to fix, but it's not
 * really needed at all. Be careful with this macro check, because it nests
 * over a few others.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)

/**
 * apfs_getflags - Read an inode's bsd flags in FS_IOC_GETFLAGS format
 * @inode: the vfs inode
 */
static unsigned int apfs_getflags(struct inode *inode)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	unsigned int flags = 0;

	if (ai->i_bsd_flags & APFS_INOBSD_APPEND)
		flags |= FS_APPEND_FL;
	if (ai->i_bsd_flags & APFS_INOBSD_IMMUTABLE)
		flags |= FS_IMMUTABLE_FL;
	if (ai->i_bsd_flags & APFS_INOBSD_NODUMP)
		flags |= FS_NODUMP_FL;
	return flags;
}

/**
 * apfs_setflags - Set an inode's bsd flags
 * @inode: the vfs inode
 * @flags: flags to set, in FS_IOC_SETFLAGS format
 */
static void apfs_setflags(struct inode *inode, unsigned int flags)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	unsigned int i_flags = 0;

	if (flags & FS_APPEND_FL) {
		ai->i_bsd_flags |= APFS_INOBSD_APPEND;
		i_flags |= S_APPEND;
	} else {
		ai->i_bsd_flags &= ~APFS_INOBSD_APPEND;
	}

	if (flags & FS_IMMUTABLE_FL) {
		ai->i_bsd_flags |= APFS_INOBSD_IMMUTABLE;
		i_flags |= S_IMMUTABLE;
	} else {
		ai->i_bsd_flags &= ~APFS_INOBSD_IMMUTABLE;
	}

	if (flags & FS_NODUMP_FL)
		ai->i_bsd_flags |= APFS_INOBSD_NODUMP;
	else
		ai->i_bsd_flags &= ~APFS_INOBSD_NODUMP;

	inode_set_flags(inode, i_flags, S_IMMUTABLE | S_APPEND);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0)

/**
 * apfs_ioc_getflags - Ioctl handler for FS_IOC_GETFLAGS
 * @file:	affected file
 * @arg:	ioctl argument
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_ioc_getflags(struct file *file, int __user *arg)
{
	unsigned int flags = apfs_getflags(file_inode(file));

	return put_user(flags, arg);
}

/**
 * apfs_do_ioc_setflags - Actual work for apfs_ioc_setflags(), after preparation
 * @inode:	affected vfs inode
 * @newflags:	inode flags to set, in FS_IOC_SETFLAGS format
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_do_ioc_setflags(struct inode *inode, unsigned int newflags)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	unsigned int oldflags;
	int err;

	lockdep_assert_held_write(&inode->i_rwsem);

	oldflags = apfs_getflags(inode);
	err = vfs_ioc_setflags_prepare(inode, oldflags, newflags);
	if (err)
		return err;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;
	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	apfs_inode_join_transaction(sb, inode);
	apfs_setflags(inode, newflags);
	inode->i_ctime = current_time(inode);

	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_ioc_setflags - Ioctl handler for FS_IOC_SETFLAGS
 * @file:	affected file
 * @arg:	ioctl argument
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_ioc_setflags(struct file *file, int __user *arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	unsigned int newflags;
	int err;

	if (sb->s_flags & SB_RDONLY)
		return -EROFS;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	if (!inode_owner_or_capable(inode))
#else
	if (!inode_owner_or_capable(&init_user_ns, inode))
#endif
		return -EPERM;

	if (get_user(newflags, arg))
		return -EFAULT;

	if (newflags & ~(FS_APPEND_FL | FS_IMMUTABLE_FL | FS_NODUMP_FL))
		return -EOPNOTSUPP;

	err = mnt_want_write_file(file);
	if (err)
		return err;

	inode_lock(inode);
	err = apfs_do_ioc_setflags(inode, newflags);
	inode_unlock(inode);

	mnt_drop_write_file(file);
	return err;
}

#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)

int apfs_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	unsigned int flags = apfs_getflags(d_inode(dentry));

	fileattr_fill_flags(fa, flags);
	return 0;
}

int apfs_fileattr_set(struct user_namespace *mnt_userns, struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	int err;

	if (sb->s_flags & SB_RDONLY)
		return -EROFS;

	if (fa->flags & ~(FS_APPEND_FL | FS_IMMUTABLE_FL | FS_NODUMP_FL))
		return -EOPNOTSUPP;
	if (fileattr_has_fsx(fa))
		return -EOPNOTSUPP;

	lockdep_assert_held_write(&inode->i_rwsem);

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;
	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	apfs_inode_join_transaction(sb, inode);
	apfs_setflags(inode, fa->flags);
	inode->i_ctime = current_time(inode);

	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0) */

int apfs_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	unsigned int flags = apfs_getflags(d_inode(dentry));

	fileattr_fill_flags(fa, flags);
	return 0;
}

int apfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	int err;

	if (sb->s_flags & SB_RDONLY)
		return -EROFS;

	if (fa->flags & ~(FS_APPEND_FL | FS_IMMUTABLE_FL | FS_NODUMP_FL))
		return -EOPNOTSUPP;
	if (fileattr_has_fsx(fa))
		return -EOPNOTSUPP;

	lockdep_assert_held_write(&inode->i_rwsem);

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;
	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	apfs_inode_join_transaction(sb, inode);
	apfs_setflags(inode, fa->flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	inode->i_ctime = current_time(inode);
#else
	inode_set_ctime_current(inode);
#endif

	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0) */

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0) */

long apfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	case FS_IOC_GETFLAGS:
		return apfs_ioc_getflags(file, argp);
	case FS_IOC_SETFLAGS:
		return apfs_ioc_setflags(file, argp);
#endif
	case APFS_IOC_SET_DFLT_PFK:
		return apfs_ioc_set_dflt_pfk(file, argp);
	case APFS_IOC_SET_DIR_CLASS:
		return apfs_ioc_set_dir_class(file, argp);
	case APFS_IOC_GET_CLASS:
		return apfs_ioc_get_class(file, argp);
	case APFS_IOC_TAKE_SNAPSHOT:
		return apfs_ioc_take_snapshot(file, argp);
	default:
		return -ENOTTY;
	}
}

long apfs_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	case FS_IOC_GETFLAGS:
		return apfs_ioc_getflags(file, argp);
	case FS_IOC_SETFLAGS:
		return apfs_ioc_setflags(file, argp);
#endif
	case APFS_IOC_SET_PFK:
		return apfs_ioc_set_pfk(file, argp);
	case APFS_IOC_GET_CLASS:
		return apfs_ioc_get_class(file, argp);
	case APFS_IOC_GET_PFK:
		return apfs_ioc_get_pfk(file, argp);
	default:
		return -ENOTTY;
	}
}
