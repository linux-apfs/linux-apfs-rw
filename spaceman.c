// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include "apfs.h"

/**
 * apfs_spaceman_read_cib_addr - Get the address of a cib from the spaceman
 * @sb:		superblock structure
 * @index:	index of the chunk-info block
 *
 * Returns the block number for the chunk-info block.
 *
 * This is not described in the official documentation; credit for figuring it
 * out should go to Joachim Metz: <https://github.com/libyal/libfsapfs>.
 */
static u64 apfs_spaceman_read_cib_addr(struct super_block *sb, int index)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	u32 offset;
	__le64 *addr_p;

	offset = sm->sm_addr_offset + index * sizeof(*addr_p);
	addr_p = (void *)sm_raw + offset;
	return le64_to_cpup(addr_p);
}

/**
 * apfs_spaceman_write_cib_addr - Store the address of a cib in the spaceman
 * @sb:		superblock structure
 * @index:	index of the chunk-info block
 * @addr:	address of the chunk-info block
 */
static void apfs_spaceman_write_cib_addr(struct super_block *sb,
					 int index, u64 addr)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	u32 offset;
	__le64 *addr_p;

	apfs_assert_in_transaction(sb, &sm_raw->sm_o);

	offset = sm->sm_addr_offset + index * sizeof(*addr_p);
	addr_p = (void *)sm_raw + offset;
	*addr_p = cpu_to_le64(addr);
}

/**
 * apfs_max_chunks_per_cib - Find the maximum chunk count for a chunk-info block
 * @sb: superblock structure
 */
static inline int apfs_max_chunks_per_cib(struct super_block *sb)
{
	return (sb->s_blocksize - sizeof(struct apfs_chunk_info_block)) /
						sizeof(struct apfs_chunk_info);
}

/**
 * apfs_read_spaceman_dev - Read a space manager device structure
 * @sb:		superblock structure
 * @dev:	on-disk device structure
 *
 * Initializes the in-memory spaceman fields related to the main device; fusion
 * drives are not yet supported.  Returns 0 on success, or a negative error code
 * in case of failure.
 */
static int apfs_read_spaceman_dev(struct super_block *sb,
				  struct apfs_spaceman_device *dev)
{
	struct apfs_spaceman *spaceman = APFS_SM(sb);

	if (dev->sm_cab_count) {
		apfs_err(sb, "large devices are not supported");
		return -EINVAL;
	}

	spaceman->sm_block_count = le64_to_cpu(dev->sm_block_count);
	spaceman->sm_chunk_count = le64_to_cpu(dev->sm_chunk_count);
	spaceman->sm_cib_count = le32_to_cpu(dev->sm_cib_count);
	spaceman->sm_free_count = le64_to_cpu(dev->sm_free_count);
	spaceman->sm_addr_offset = le32_to_cpu(dev->sm_addr_offset);

	/* Check that all the cib addresses fit in the spaceman block */
	if ((long long)spaceman->sm_addr_offset +
	    (long long)spaceman->sm_cib_count * sizeof(u64) > sb->s_blocksize) {
		apfs_err(sb, "too many cibs (%u)", spaceman->sm_cib_count);
		return -EFSCORRUPTED;
	}

	return 0;
}

/**
 * apfs_spaceman_get_64 - Get a 64-bit value from an offset in the spaceman
 * @sb:		superblock structure
 * @off:	offset for the value
 *
 * Returns a pointer to the value, or NULL if it doesn't fit.
 */
static __le64 *apfs_spaceman_get_64(struct super_block *sb, size_t off)
{
	struct apfs_spaceman *spaceman = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = spaceman->sm_raw;

	if (off > sb->s_blocksize)
		return NULL;
	if (off + sizeof(__le64) > sb->s_blocksize)
		return NULL;
	return (void *)sm_raw + off;
}

/**
 * apfs_ip_bm_is_free - Check if a given ip bitmap is in the free range
 * @sm:		on-disk spaceman structure
 * @index:	offset in the ring buffer of the bitmap block to check
 */
static bool apfs_ip_bm_is_free(struct apfs_spaceman_phys *sm, u16 index)
{
	u16 free_head = le16_to_cpu(sm->sm_ip_bm_free_head);
	u16 free_tail = le16_to_cpu(sm->sm_ip_bm_free_tail);
	u16 free_len, index_in_free;
	u16 bmap_count = 16;

	free_len = (bmap_count + free_tail - free_head) % bmap_count;
	index_in_free = (bmap_count + index - free_head) % bmap_count;

	return index_in_free < free_len;
}

/**
 * apfs_update_ip_bm_free_next - Update free_next for the internal pool
 * @sb: superblock structure
 *
 * Uses the head and tail reported by the on-disk spaceman structure. Returns 0
 * on success, or -EFSCORRUPTED if corruption is detected.
 */
static int apfs_update_ip_bm_free_next(struct super_block *sb)
{
	struct apfs_spaceman *spaceman = APFS_SM(sb);
	struct apfs_spaceman_phys *raw = spaceman->sm_raw;
	u32 free_next_off = le32_to_cpu(raw->sm_ip_bm_free_next_offset);
	int bmap_count = 16;
	__le16 *free_next;
	int i;

	if (free_next_off > sb->s_blocksize) {
		apfs_err(sb, "offset out of bounds (%u)", free_next_off);
		return -EFSCORRUPTED;
	}
	if (free_next_off + bmap_count * sizeof(*free_next) > sb->s_blocksize) {
		apfs_err(sb, "free next out of bounds (%u-%u)", free_next_off, bmap_count * (u32)sizeof(*free_next));
		return -EFSCORRUPTED;
	}
	free_next = (void *)raw + free_next_off;

	for (i = 0; i < bmap_count; ++i) {
		if (apfs_ip_bm_is_free(raw, i))
			free_next[i] = cpu_to_le16((1 + i) % bmap_count);
		else
			free_next[i] = cpu_to_le16(0xFFFF);
	}
	return 0;
}

/**
 * apfs_rotate_ip_bitmaps - Allocate a new ip bitmap from the circular buffer
 * @sb: superblock structure
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_rotate_ip_bitmaps(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_spaceman *spaceman = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = spaceman->sm_raw;
	u64 bmap_base = le64_to_cpu(sm_raw->sm_ip_bm_base);
	u32 bmap_length = le32_to_cpu(sm_raw->sm_ip_bm_block_count);
	u16 free_head, free_tail;
	__le64 *curr_bmap_off, *xid;
	struct buffer_head *old_bh = NULL, *new_bh = NULL;
	int err = 0;

	apfs_assert_in_transaction(sb, &sm_raw->sm_o);

	if (le32_to_cpu(sm_raw->sm_ip_bm_size_in_blocks) != 1) {
		apfs_warn(sb, "Multiblock ip bitmaps not supported");
		return -EOPNOTSUPP;
	}

	xid = apfs_spaceman_get_64(sb, le32_to_cpu(sm_raw->sm_ip_bm_xid_offset));
	if (!xid) {
		apfs_err(sb, "xid out of bounds (%u)", le32_to_cpu(sm_raw->sm_ip_bm_xid_offset));
		return -EFSCORRUPTED;
	}
	*xid = cpu_to_le64(nxi->nx_xid);

	free_head = le16_to_cpu(sm_raw->sm_ip_bm_free_head);
	free_tail = le16_to_cpu(sm_raw->sm_ip_bm_free_tail);

	curr_bmap_off = apfs_spaceman_get_64(sb, le32_to_cpu(sm_raw->sm_ip_bitmap_offset));
	if (!curr_bmap_off) {
		apfs_err(sb, "bmap offset out of bounds (%u)", le32_to_cpu(sm_raw->sm_ip_bitmap_offset));
		return -EFSCORRUPTED;
	}
	old_bh = apfs_sb_bread(sb, bmap_base + le64_to_cpup(curr_bmap_off));
	if (!old_bh) {
		apfs_err(sb, "failed to read current ip bitmap (0x%llx)", bmap_base + le64_to_cpup(curr_bmap_off));
		return -EIO;
	}

	*curr_bmap_off = cpu_to_le64(free_head);
	free_head = (free_head + 1) % bmap_length;
	free_tail = (free_tail + 1) % bmap_length;
	sm_raw->sm_ip_bm_free_head = cpu_to_le16(free_head);
	sm_raw->sm_ip_bm_free_tail = cpu_to_le16(free_tail);
	err = apfs_update_ip_bm_free_next(sb);
	if (err) {
		apfs_err(sb, "failed to update bitmap ring");
		goto out;
	}

	new_bh = apfs_getblk(sb, bmap_base + le64_to_cpup(curr_bmap_off));
	if (!new_bh) {
		apfs_err(sb, "failed to map block for CoW (0x%llx)", bmap_base + le64_to_cpup(curr_bmap_off));
		err = -EIO;
		goto out;
	}
	memcpy(new_bh->b_data, old_bh->b_data, sb->s_blocksize);
	err = apfs_transaction_join(sb, new_bh);
	if (err)
		goto out;
	spaceman->sm_ip = new_bh;

out:
	brelse(old_bh);
	if (err)
		brelse(new_bh);
	return err;
}

/*
 * Free queue record data
 */
struct apfs_fq_rec {
	u64 xid;
	u64 bno;
	u64 len;
};

/**
 * apfs_fq_rec_from_query - Read the free queue record found by a query
 * @query:	the query that found the record
 * @fqrec:	on return, the free queue record
 *
 * Reads the free queue record into @fqrec and performs some basic sanity
 * checks as a protection against crafted filesystems. Returns 0 on success
 * or -EFSCORRUPTED otherwise.
 */
static int apfs_fq_rec_from_query(struct apfs_query *query, struct apfs_fq_rec *fqrec)
{
	char *raw = query->node->object.data;
	struct apfs_spaceman_free_queue_key *key;

	if (query->key_len != sizeof(*key)) {
		apfs_err(query->node->object.sb, "bad key length (%d)", query->key_len);
		return -EFSCORRUPTED;
	}
	key = (struct apfs_spaceman_free_queue_key *)(raw + query->key_off);

	fqrec->xid = le64_to_cpu(key->sfqk_xid);
	fqrec->bno = le64_to_cpu(key->sfqk_paddr);

	if (query->len == 0) {
		fqrec->len = 1; /* Ghost record */
		return 0;
	} else if (query->len == sizeof(__le64)) {
		fqrec->len = le64_to_cpup((__le64 *)(raw + query->off));
		return 0;
	}
	apfs_err(query->node->object.sb, "bad value length (%d)", query->len);
	return -EFSCORRUPTED;
}

/**
 * apfs_block_in_ip - Does this block belong to the internal pool?
 * @sm:		in-memory spaceman structure
 * @bno:	block number to check
 */
static inline bool apfs_block_in_ip(struct apfs_spaceman *sm, u64 bno)
{
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	u64 start = le64_to_cpu(sm_raw->sm_ip_base);
	u64 end = start + le64_to_cpu(sm_raw->sm_ip_block_count);

	return bno >= start && bno < end;
}

/**
 * apfs_ip_mark_free - Mark a block in the internal pool as free
 * @sb:		superblock structure
 * @bno:	block number (must belong to the ip)
 */
static int apfs_ip_mark_free(struct super_block *sb, u64 bno)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	char *bitmap = sm->sm_ip->b_data;

	bno -= le64_to_cpu(sm_raw->sm_ip_base);
	__clear_bit_le(bno, bitmap);

	return 0;
}

/*
 * apfs_main_free - Mark a regular block as free
 */
static int apfs_main_free(struct super_block *sb, u64 bno);

/**
 * apfs_flush_fq_rec - Delete a single fq record and mark its blocks as free
 * @root:	free queue root node
 * @xid:	transaction to target
 * @len:	on return, the number of freed blocks
 *
 * Returns 0 on success, or a negative error code in case of failure. -ENODATA
 * in particular means that there are no matching records left.
 */
static int apfs_flush_fq_rec(struct apfs_node *root, u64 xid, u64 *len)
{
	struct super_block *sb = root->object.sb;
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_query *query = NULL;
	struct apfs_fq_rec fqrec = {0};
	struct apfs_key key;
	u64 bno;
	int err;

	query = apfs_alloc_query(root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	apfs_init_free_queue_key(xid, 0 /* paddr */, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_FREE_QUEUE | APFS_QUERY_ANY_NUMBER | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err) {
		if (err != -ENODATA)
			apfs_err(sb, "query failed for xid 0x%llx, paddr 0x%llx", xid, 0ULL);
		goto fail;
	}
	err = apfs_fq_rec_from_query(query, &fqrec);
	if (err) {
		apfs_err(sb, "bad free queue rec for xid 0x%llx", xid);
		goto fail;
	}

	for (bno = fqrec.bno; bno < fqrec.bno + fqrec.len; ++bno) {
		if(apfs_block_in_ip(sm, bno))
			err = apfs_ip_mark_free(sb, bno);
		else
			err = apfs_main_free(sb, bno);
		if (err) {
			apfs_err(sb, "freeing block 0x%llx failed (%d)", (unsigned long long)bno, err);
			goto fail;
		}
	}
	err = apfs_btree_remove(query);
	if (err) {
		apfs_err(sb, "removal failed for xid 0x%llx", xid);
		goto fail;
	}
	*len = fqrec.len;

fail:
	apfs_free_query(query);
	return err;
}

/**
 * apfs_free_queue_oldest_xid - Find the oldest xid among the free queue records
 * @root: free queue root node
 */
static u64 apfs_free_queue_oldest_xid(struct apfs_node *root)
{
	struct apfs_spaceman_free_queue_key *key;
	char *raw = root->object.data;
	int len, off;

	len = apfs_node_locate_key(root, 0, &off);
	if (len != sizeof(*key)) /* No records in queue (or corruption) */
		return 0;
	key = (struct apfs_spaceman_free_queue_key *)(raw + off);
	return le64_to_cpu(key->sfqk_xid);
}

/**
 * apfs_flush_free_queue - Free ip blocks queued by old transactions
 * @sb:		superblock structure
 * @qid:	queue to be freed
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_flush_free_queue(struct super_block *sb, unsigned qid)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	struct apfs_spaceman_free_queue *fq = &sm_raw->sm_fq[qid];
	struct apfs_node *fq_root;
	u64 oldest = le64_to_cpu(fq->sfq_oldest_xid);
	int err;

	fq_root = apfs_read_node(sb, le64_to_cpu(fq->sfq_tree_oid),
				 APFS_OBJ_EPHEMERAL, true /* write */);
	if (IS_ERR(fq_root)) {
		apfs_err(sb, "failed to read fq root 0x%llx", le64_to_cpu(fq->sfq_tree_oid));
		return PTR_ERR(fq_root);
	}

	/* Preserve a few transactions */
	while (oldest + 4 < nxi->nx_xid) {
		u64 sfq_count;

		while (true) {
			u64 count = 0;

			/* Probably not very efficient... */
			err = apfs_flush_fq_rec(fq_root, oldest, &count);
			if (err == -ENODATA) {
				err = 0;
				break;
			} else if (err) {
				apfs_err(sb, "failed to flush fq");
				goto fail;
			} else {
				le64_add_cpu(&fq->sfq_count, -count);
			}
		}
		oldest = apfs_free_queue_oldest_xid(fq_root);
		fq->sfq_oldest_xid = cpu_to_le64(oldest);

		/*
		 * Flushing a single transaction may not be enough to avoid
		 * running out of space in the ip, but it's probably best not
		 * to flush all the old transactions at once either. We use a
		 * harsher version of the apfs_transaction_need_commit() check,
		 * to make sure we won't be forced to commit again right away.
		 */
		sfq_count = le64_to_cpu(fq->sfq_count);
		if (qid == APFS_SFQ_IP && sfq_count * 6 <= le64_to_cpu(sm_raw->sm_ip_block_count))
			break;
		if (qid == APFS_SFQ_MAIN && sfq_count <= TRANSACTION_MAIN_QUEUE_MAX - 200)
			break;
	}

	set_buffer_csum(sm->sm_bh);

fail:
	apfs_node_free(fq_root);
	return err;
}

/**
 * apfs_read_spaceman - Find and read the space manager
 * @sb: superblock structure
 *
 * Reads the space manager structure from disk and initializes its in-memory
 * counterpart; returns 0 on success, or a negative error code in case of
 * failure.
 */
int apfs_read_spaceman(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	struct apfs_spaceman *spaceman = APFS_SM(sb);
	struct buffer_head *sm_bh;
	struct apfs_spaceman_phys *sm_raw;
	u32 sm_flags;
	u64 oid = le64_to_cpu(raw_sb->nx_spaceman_oid);
	int err;

	if (sb->s_flags & SB_RDONLY) /* The space manager won't be needed */
		return 0;

	spaceman->sm_free_cache_base = spaceman->sm_free_cache_blkcnt = 0;

	sm_bh = apfs_read_ephemeral_object(sb, oid);
	if (IS_ERR(sm_bh)) {
		apfs_err(sb, "ephemeral read failed for oid 0x%llx", oid);
		return PTR_ERR(sm_bh);
	}
	sm_raw = (struct apfs_spaceman_phys *)sm_bh->b_data;

	sm_flags = le32_to_cpu(sm_raw->sm_flags);
	/* Undocumented feature, but it's too common to refuse to mount */
	if (sm_flags & APFS_SM_FLAG_VERSIONED)
		pr_warn_once("APFS: space manager is versioned\n");

	/* Only read the main device; fusion drives are not yet supported */
	err = apfs_read_spaceman_dev(sb, &sm_raw->sm_dev[APFS_SD_MAIN]);
	if (err) {
		apfs_err(sb, "failed to read main device");
		goto fail;
	}

	spaceman->sm_blocks_per_chunk =
				le32_to_cpu(sm_raw->sm_blocks_per_chunk);
	spaceman->sm_chunks_per_cib = le32_to_cpu(sm_raw->sm_chunks_per_cib);
	if (spaceman->sm_chunks_per_cib > apfs_max_chunks_per_cib(sb)) {
		apfs_err(sb, "too many chunks per cib (%u)", spaceman->sm_chunks_per_cib);
		err = -EFSCORRUPTED;
		goto fail;
	}

	spaceman->sm_bh = sm_bh;
	spaceman->sm_raw = sm_raw;
	err = apfs_rotate_ip_bitmaps(sb);
	if (err) {
		apfs_err(sb, "failed to rotate ip bitmaps");
		goto fail;
	}
	err = apfs_flush_free_queue(sb, APFS_SFQ_IP);
	if (err) {
		apfs_err(sb, "failed to flush ip fq");
		goto fail;
	}
	err = apfs_flush_free_queue(sb, APFS_SFQ_MAIN);
	if (err) {
		apfs_err(sb, "failed to flush main fq");
		goto fail;
	}
	return 0;

fail:
	brelse(sm_bh);
	spaceman->sm_bh = sm_bh = NULL;
	spaceman->sm_raw = NULL;
	return err;
}

/**
 * apfs_write_spaceman - Write the in-memory spaceman fields to the disk buffer
 * @sm: in-memory spaceman structure
 *
 * Copies the updated in-memory fields of the space manager into the on-disk
 * structure; the buffer is not dirtied.
 */
static void apfs_write_spaceman(struct apfs_spaceman *sm)
{
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	struct apfs_spaceman_device *dev_raw = &sm_raw->sm_dev[APFS_SD_MAIN];
	struct apfs_nxsb_info *nxi;

	nxi = container_of(sm, struct apfs_nxsb_info, nx_spaceman);
	ASSERT(le64_to_cpu(sm_raw->sm_o.o_xid) == nxi->nx_xid);

	dev_raw->sm_free_count = cpu_to_le64(sm->sm_free_count);
}

/**
 * apfs_ip_find_free - Find a free block inside the internal pool
 * @sb:		superblock structure
 *
 * Returns the block number for a free block, or 0 in case of corruption.
 */
static u64 apfs_ip_find_free(struct super_block *sb)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	u64 bitcount = le64_to_cpu(sm_raw->sm_ip_block_count);
	char *bitmap = sm->sm_ip->b_data;
	u64 offset;

	if (bitcount > sb->s_blocksize * 8)
		return 0;
	offset = find_next_zero_bit_le(bitmap, bitcount, 0 /* offset */);
	if (offset >= bitcount) {
		apfs_warn(sb, "internal pool seems full");
		return 0;
	}
	return le64_to_cpu(sm_raw->sm_ip_base) + offset;
}

/**
 * apfs_chunk_find_free - Find a free block inside a chunk
 * @sb:		superblock structure
 * @bitmap:	allocation bitmap for the chunk, which should have free blocks
 * @addr:	number of the first block in the chunk
 *
 * Returns the block number for a free block, or 0 in case of corruption.
 */
static u64 apfs_chunk_find_free(struct super_block *sb, char *bitmap, u64 addr)
{
	int bitcount = sb->s_blocksize * 8;
	u64 bno;

	bno = find_next_zero_bit_le(bitmap, bitcount, 0 /* offset */);
	if (bno >= bitcount)
		return 0;
	return addr + bno;
}

/**
 * apfs_ip_mark_used - Mark a block in the internal pool as used
 * @sb:		superblock strucuture
 * @bno:	block number (must belong to the ip)
 */
static void apfs_ip_mark_used(struct super_block *sb, u64 bno)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	char *bitmap = sm->sm_ip->b_data;

	bno -= le64_to_cpu(sm_raw->sm_ip_base);
	__set_bit_le(bno, bitmap);
}

/**
 * apfs_chunk_mark_used - Mark a block inside a chunk as used
 * @sb:		superblock structure
 * @bitmap:	allocation bitmap for the chunk
 * @bno:	block number (must belong to the chunk)
 */
static inline void apfs_chunk_mark_used(struct super_block *sb, char *bitmap,
					u64 bno)
{
	int bitcount = sb->s_blocksize * 8;

	__set_bit_le(bno & (bitcount - 1), bitmap);
}

/**
 * apfs_chunk_mark_free - Mark a block inside a chunk as free
 * @sb:		superblock structure
 * @bitmap:	allocation bitmap for the chunk
 * @bno:	block number (must belong to the chunk)
 */
static inline int apfs_chunk_mark_free(struct super_block *sb, char *bitmap,
					u64 bno)
{
	int bitcount = sb->s_blocksize * 8;

	return __test_and_clear_bit_le(bno & (bitcount - 1), bitmap);
}

/**
 * apfs_free_queue_insert_nocache - Add a block range to its free queue
 * @sb:		superblock structure
 * @bno:	first block number to free
 * @count:	number of consecutive blocks to free
 *
 * Same as apfs_free_queue_insert(), but writes to the free queue directly,
 * bypassing the cache of the latest freed block range.
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_free_queue_insert_nocache(struct super_block *sb, u64 bno, u64 count)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	struct apfs_spaceman_free_queue *fq;
	struct apfs_node *fq_root = NULL;
	struct apfs_btree_info *fq_info = NULL;
	struct apfs_query *query = NULL;
	struct apfs_spaceman_free_queue_key raw_key;
	__le64 raw_val;
	struct apfs_key key;
	u64 node_count;
	u16 node_limit;
	int err;

	if (apfs_block_in_ip(sm, bno))
		fq = &sm_raw->sm_fq[APFS_SFQ_IP];
	else
		fq = &sm_raw->sm_fq[APFS_SFQ_MAIN];

	fq_root = apfs_read_node(sb, le64_to_cpu(fq->sfq_tree_oid),
				 APFS_OBJ_EPHEMERAL, true /* write */);
	if (IS_ERR(fq_root)) {
		apfs_err(sb, "failed to read fq root 0x%llx", le64_to_cpu(fq->sfq_tree_oid));
		return PTR_ERR(fq_root);
	}

	query = apfs_alloc_query(fq_root, NULL /* parent */);
	if (!query) {
		err = -ENOMEM;
		goto fail;
	}

	apfs_init_free_queue_key(nxi->nx_xid, bno, &key);
	query->key = &key;
	query->flags |= APFS_QUERY_FREE_QUEUE;

	err = apfs_btree_query(sb, &query);
	if (err && err != -ENODATA) {
		apfs_err(sb, "query failed for xid 0x%llx, paddr 0x%llx", nxi->nx_xid, bno);
		goto fail;
	}

	raw_key.sfqk_xid = cpu_to_le64(nxi->nx_xid);
	raw_key.sfqk_paddr = cpu_to_le64(bno);
	if (count == 1) {
		/* A lack of value (ghost record) means single-block extent */
		err = apfs_btree_insert(query, &raw_key, sizeof(raw_key), NULL /* val */, 0 /* val_len */);
	} else {
		raw_val = cpu_to_le64(count);
		err = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	}
	if (err) {
		apfs_err(sb, "insertion failed for xid 0x%llx, paddr 0x%llx", nxi->nx_xid, bno);
		goto fail;
	}

	fq_info = (void *)fq_root->object.data + sb->s_blocksize - sizeof(*fq_info);
	node_count = le64_to_cpu(fq_info->bt_node_count);
	node_limit = le16_to_cpu(fq->sfq_tree_node_limit);
	if (node_count > node_limit) {
		/*
		 * Ideally this should never happen, but at this point I can't
		 * be certain of that (TODO). If it does happen, it's best to
		 * abort and avoid corruption.
		 */
		apfs_alert(sb, "free queue has too many nodes (%llu > %u)", node_count, node_limit);
		err = -EFSCORRUPTED;
		goto fail;
	}

	if (!fq->sfq_oldest_xid)
		fq->sfq_oldest_xid = cpu_to_le64(nxi->nx_xid);
	le64_add_cpu(&fq->sfq_count, count);
	set_buffer_csum(sm->sm_bh);

fail:
	apfs_free_query(query);
	apfs_node_free(fq_root);
	return err;
}

/**
 * apfs_free_queue_insert - Add a block range to its free queue
 * @sb:		superblock structure
 * @bno:	first block number to free
 * @count:	number of consecutive blocks to free
 *
 * Uses a cache to delay the actual tree operations as much as possible.
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_free_queue_insert(struct super_block *sb, u64 bno, u64 count)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	int err;

	if (sm->sm_free_cache_base == 0) {
		/* Nothing yet cached */
		sm->sm_free_cache_base = bno;
		sm->sm_free_cache_blkcnt = count;
		return 0;
	}

	/*
	 * First attempt to extend the cache of freed blocks, but never cache
	 * a range that doesn't belong to a single free queue.
	 */
	if (apfs_block_in_ip(sm, bno) == apfs_block_in_ip(sm, sm->sm_free_cache_base)) {
		if (bno == sm->sm_free_cache_base + sm->sm_free_cache_blkcnt) {
			sm->sm_free_cache_blkcnt += count;
			return 0;
		}
		if (bno + count == sm->sm_free_cache_base) {
			sm->sm_free_cache_base -= count;
			sm->sm_free_cache_blkcnt += count;
			return 0;
		}
	}

	/* Failed to extend the cache, so flush it and replace it */
	err = apfs_free_queue_insert_nocache(sb, sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
	if (err) {
		apfs_err(sb, "fq cache flush failed (0x%llx-0x%llx)", sm->sm_free_cache_base, sm->sm_free_cache_blkcnt);
		return err;
	}
	sm->sm_free_cache_base = bno;
	sm->sm_free_cache_blkcnt = count;
	return 0;
}

/**
 * apfs_chunk_alloc_free - Allocate or free block in given CIB and chunk
 * @sb:		superblock structure
 * @cib_bh:	buffer head for the chunk-info block
 * @index:	index of this chunk's info structure inside @cib
 * @bno:	block number
 * @is_alloc:	true to allocate, false to free
 */
static int apfs_chunk_alloc_free(struct super_block *sb,
				 struct buffer_head **cib_bh,
				 int index, u64 *bno, bool is_alloc)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_chunk_info_block *cib;
	struct apfs_chunk_info *ci;
	struct buffer_head *bmap_bh = NULL;
	char *bmap = NULL;
	bool old_cib = false;
	bool old_bmap = false;
	int err = 0;

	cib = (struct apfs_chunk_info_block *)(*cib_bh)->b_data;
	ci = &cib->cib_chunk_info[index];

	/* Cibs and bitmaps from old transactions can't be modified in place */
	if (le64_to_cpu(cib->cib_o.o_xid) < nxi->nx_xid)
		old_cib = true;
	if (le64_to_cpu(ci->ci_xid) < nxi->nx_xid)
		old_bmap = true;
	if (is_alloc && le32_to_cpu(ci->ci_free_count) < 1)
		return -ENOSPC;

	/* Read the current bitmap, or allocate it if necessary */
	if (!ci->ci_bitmap_addr) {
		u64 bmap_bno;

		if(!is_alloc) {
			apfs_err(sb, "attempt to free block in all-free chunk");
			return -EFSCORRUPTED;
		}

		/* All blocks in this chunk are free */
		bmap_bno = apfs_ip_find_free(sb);
		if (!bmap_bno) {
			apfs_err(sb, "no free blocks in ip");
			return -EFSCORRUPTED;
		}
		bmap_bh = apfs_sb_bread(sb, bmap_bno);
	} else {
		bmap_bh = apfs_sb_bread(sb, le64_to_cpu(ci->ci_bitmap_addr));
	}
	if (!bmap_bh) {
		apfs_err(sb, "failed to read bitmap block");
		return -EIO;
	}
	bmap = bmap_bh->b_data;
	if (!ci->ci_bitmap_addr) {
		memset(bmap, 0, sb->s_blocksize);
		old_bmap = false;
	}

	/* Write the bitmap to its location for the next transaction */
	if (old_bmap) {
		struct buffer_head *new_bmap_bh;
		u64 new_bmap_bno;

		new_bmap_bno = apfs_ip_find_free(sb);
		if (!new_bmap_bno) {
			apfs_err(sb, "no free blocks in ip");
			err = -EFSCORRUPTED;
			goto fail;
		}

		new_bmap_bh = apfs_getblk(sb, new_bmap_bno);
		if (!new_bmap_bh) {
			apfs_err(sb, "failed to map new bmap block (0x%llx)", new_bmap_bno);
			err = -EIO;
			goto fail;
		}
		memcpy(new_bmap_bh->b_data, bmap, sb->s_blocksize);
		err = apfs_free_queue_insert(sb, bmap_bh->b_blocknr, 1);
		brelse(bmap_bh);
		bmap_bh = new_bmap_bh;
		if (err) {
			apfs_err(sb, "free queue insertion failed");
			goto fail;
		}
		bmap = bmap_bh->b_data;
	}
	apfs_ip_mark_used(sb, bmap_bh->b_blocknr);

	/* Write the cib to its location for the next transaction */
	if (old_cib) {
		struct buffer_head *new_cib_bh;
		u64 new_cib_bno;

		new_cib_bno = apfs_ip_find_free(sb);
		if (!new_cib_bno) {
			apfs_err(sb, "no free blocks in ip");
			err = -EFSCORRUPTED;
			goto fail;
		}

		new_cib_bh = apfs_getblk(sb, new_cib_bno);
		if (!new_cib_bh) {
			apfs_err(sb, "failed to map new cib block (0x%llx)", new_cib_bno);
			err = -EIO;
			goto fail;
		}
		memcpy(new_cib_bh->b_data, (*cib_bh)->b_data, sb->s_blocksize);
		err = apfs_free_queue_insert(sb, (*cib_bh)->b_blocknr, 1);
		brelse(*cib_bh);
		*cib_bh = new_cib_bh;
		if (err) {
			apfs_err(sb, "free queue insertion failed");
			goto fail;
		}

		err = apfs_transaction_join(sb, *cib_bh);
		if (err)
			goto fail;

		cib = (struct apfs_chunk_info_block *)(*cib_bh)->b_data;
		ci = &cib->cib_chunk_info[index];
		cib->cib_o.o_oid = cpu_to_le64(new_cib_bno);
		cib->cib_o.o_xid = cpu_to_le64(nxi->nx_xid);

		apfs_ip_mark_used(sb, new_cib_bno);
	}

	/* The chunk info can be updated now */
	apfs_assert_in_transaction(sb, &cib->cib_o);
	ci->ci_xid = cpu_to_le64(nxi->nx_xid);
	le32_add_cpu(&ci->ci_free_count, is_alloc ? -1 : 1);
	ci->ci_bitmap_addr = cpu_to_le64(bmap_bh->b_blocknr);
	ASSERT(buffer_trans(*cib_bh));
	set_buffer_csum(*cib_bh);
	mark_buffer_dirty(*cib_bh);

	/* Finally, allocate / free the actual block that was requested */
	if(is_alloc) {
		*bno = apfs_chunk_find_free(sb, bmap, le64_to_cpu(ci->ci_addr));
		if (!*bno) {
			apfs_err(sb, "no free blocks in chunk");
			err = -EFSCORRUPTED;
			goto fail;
		}
		apfs_chunk_mark_used(sb, bmap, *bno);
		sm->sm_free_count -= 1;
	} else {
		if(!apfs_chunk_mark_free(sb, bmap, *bno)) {
			apfs_err(sb, "block already marked as free (0x%llx)", *bno);
			le32_add_cpu(&ci->ci_free_count, -1);
			set_buffer_csum(*cib_bh);
			mark_buffer_dirty(*cib_bh);
			err = -EFSCORRUPTED;
		} else
			sm->sm_free_count += 1;
	}
	mark_buffer_dirty(bmap_bh);

fail:
	brelse(bmap_bh);
	return err;
}

/**
 * apfs_chunk_allocate_block - Allocate a single block from a chunk
 * @sb:		superblock structure
 * @cib_bh:	buffer head for the chunk-info block
 * @index:	index of this chunk's info structure inside @cib
 * @bno:	on return, the allocated block number
 *
 * Finds a free block in the chunk and marks it as used; the buffer at @cib_bh
 * may be replaced if needed for copy-on-write.  Returns 0 on success, or a
 * negative error code in case of failure.
 */
static int apfs_chunk_allocate_block(struct super_block *sb,
				     struct buffer_head **cib_bh,
				     int index, u64 *bno)
{
	return apfs_chunk_alloc_free(sb, cib_bh, index, bno, true);
}

/**
 * apfs_cib_allocate_block - Allocate a single block from a cib
 * @sb:		superblock structure
 * @cib_bh:	buffer head for the chunk-info block
 * @bno:	on return, the allocated block number
 * @backwards:	start the search on the last chunk
 *
 * Finds a free block among all the chunks in the cib and marks it as used; the
 * buffer at @cib_bh may be replaced if needed for copy-on-write.  Returns 0 on
 * success, or a negative error code in case of failure.
 */
static int apfs_cib_allocate_block(struct super_block *sb,
				   struct buffer_head **cib_bh, u64 *bno, bool backwards)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_chunk_info_block *cib;
	u32 chunk_count;
	int i;

	cib = (struct apfs_chunk_info_block *)(*cib_bh)->b_data;
	if (nxi->nx_flags & APFS_CHECK_NODES && !apfs_obj_verify_csum(sb, *cib_bh)) {
		apfs_err(sb, "bad checksum for chunk-info block");
		return -EFSBADCRC;
	}

	/* Avoid out-of-bounds operations on corrupted cibs */
	chunk_count = le32_to_cpu(cib->cib_chunk_info_count);
	if (chunk_count > sm->sm_chunks_per_cib) {
		apfs_err(sb, "too many chunks in cib (%u)", chunk_count);
		return -EFSCORRUPTED;
	}

	for (i = 0; i < chunk_count; ++i) {
		int index;
		int err;

		index = backwards ? chunk_count - 1 - i : i;

		err = apfs_chunk_allocate_block(sb, cib_bh, index, bno);
		if (err == -ENOSPC) /* This chunk is full */
			continue;
		if (err)
			apfs_err(sb, "error during allocation");
		return err;
	}
	return -ENOSPC;
}

/**
 * apfs_spaceman_allocate_block - Allocate a single on-disk block
 * @sb:		superblock structure
 * @bno:	on return, the allocated block number
 * @backwards:	start the search on the last chunk
 *
 * Finds a free block among the spaceman bitmaps and marks it as used.  Returns
 * 0 on success, or a negative error code in case of failure.
 */
int apfs_spaceman_allocate_block(struct super_block *sb, u64 *bno, bool backwards)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	int i;

	for (i = 0; i < sm->sm_cib_count; ++i) {
		struct buffer_head *cib_bh;
		u64 cib_bno;
		int index;
		int err;

		/* Keep extents and metadata separate to limit fragmentation */
		index = backwards ? sm->sm_cib_count - 1 - i : i;

		cib_bno = apfs_spaceman_read_cib_addr(sb, index);
		cib_bh = apfs_sb_bread(sb, cib_bno);
		if (!cib_bh) {
			apfs_err(sb, "failed to read cib");
			return -EIO;
		}

		err = apfs_cib_allocate_block(sb, &cib_bh, bno, backwards);
		if (!err) {
			/* The cib may have been moved */
			apfs_spaceman_write_cib_addr(sb, index, cib_bh->b_blocknr);
			/* The free block count has changed */
			apfs_write_spaceman(sm);
			set_buffer_csum(sm->sm_bh);
		}
		brelse(cib_bh);
		if (err == -ENOSPC) /* This cib is full */
			continue;
		if (err)
			apfs_err(sb, "error during allocation");
		return err;
	}
	return -ENOSPC;
}

/**
 * apfs_chunk_free - Mark a regular block as free given CIB and chunk
 * @sb:		superblock structure
 * @cib_bh:	buffer head for the chunk-info block
 * @index:	index of this chunk's info structure inside @cib
 * @bno:	block number (must not belong to the ip)
 */
static int apfs_chunk_free(struct super_block *sb,
				struct buffer_head **cib_bh,
				int index, u64 bno)
{
	return apfs_chunk_alloc_free(sb, cib_bh, index, &bno, false);
}

/**
 * apfs_main_free - Mark a regular block as free
 * @sb:		superblock structure
 * @bno:	block number (must not belong to the ip)
 */
static int apfs_main_free(struct super_block *sb, u64 bno)
{
	struct apfs_spaceman *sm = APFS_SM(sb);
	struct apfs_spaceman_phys *sm_raw = sm->sm_raw;
	u64 cib_idx, chunk_idx;
	struct buffer_head *cib_bh;
	u64 cib_bno;
	int err;

	if(!sm_raw->sm_blocks_per_chunk || !sm_raw->sm_chunks_per_cib) {
		apfs_err(sb, "block or chunk count not set");
		return -EINVAL;
	}
	/* TODO: use bitshifts instead of do_div() */
	chunk_idx = bno;
	do_div(chunk_idx, sm->sm_blocks_per_chunk);
	cib_idx = chunk_idx;
	chunk_idx = do_div(cib_idx, sm->sm_chunks_per_cib);

	cib_bno = apfs_spaceman_read_cib_addr(sb, cib_idx);
	cib_bh = apfs_sb_bread(sb, cib_bno);
	if (!cib_bh) {
		apfs_err(sb, "failed to read cib");
		return -EIO;
	}

	err = apfs_chunk_free(sb, &cib_bh, chunk_idx, bno);
	if (!err) {
		/* The cib may have been moved */
		apfs_spaceman_write_cib_addr(sb, cib_idx, cib_bh->b_blocknr);
		/* The free block count has changed */
		apfs_write_spaceman(sm);
		set_buffer_csum(sm->sm_bh);
	}
	brelse(cib_bh);
	if (err)
		apfs_err(sb, "error during free");

	return err;
}
