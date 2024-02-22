// SPDX-License-Identifier: GPL-2.0-only
/*
 * Checksum routines for an APFS object
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include "apfs.h"

/*
 * Note that this is not a generic implementation of fletcher64, as it assumes
 * a message length that doesn't overflow sum1 and sum2.  This constraint is ok
 * for apfs, though, since the block size is limited to 2^16.  For a more
 * generic optimized implementation, see Nakassis (1988).
 */
static u64 apfs_fletcher64(void *addr, size_t len)
{
	__le32 *buff = addr;
	u64 sum1 = 0;
	u64 sum2 = 0;
	u64 c1, c2;
	int i, count_32;

	count_32 = len >> 2;
	for (i = 0; i < count_32; i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - do_div(c1, 0xFFFFFFFF);
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - do_div(c2, 0xFFFFFFFF);

	return (c2 << 32) | c1;
}

int apfs_obj_verify_csum(struct super_block *sb, struct buffer_head *bh)
{
	/* The checksum may be stale until the transaction is committed */
	if (buffer_trans(bh))
		return 1;
	return apfs_multiblock_verify_csum(bh->b_data, sb->s_blocksize);
}

/**
 * apfs_multiblock_verify_csum - Verify an object's checksum
 * @object:	the object to verify
 * @size:	size of the object in bytes (may be multiple blocks)
 *
 * Returns 1 on success, 0 on failure.
 */
int apfs_multiblock_verify_csum(char *object, u32 size)
{
	struct apfs_obj_phys *obj = (struct apfs_obj_phys *)object;
	u64 actual_csum, header_csum;

	header_csum = le64_to_cpu(obj->o_cksum);
	actual_csum = apfs_fletcher64(object + APFS_MAX_CKSUM_SIZE, size - APFS_MAX_CKSUM_SIZE);
	return header_csum == actual_csum;
}

/**
 * apfs_obj_set_csum - Set the fletcher checksum in an object header
 * @sb:		superblock structure
 * @obj:	the object header
 *
 * The object must have a length of a single block.
 */
void apfs_obj_set_csum(struct super_block *sb, struct apfs_obj_phys *obj)
{
	apfs_multiblock_set_csum((char *)obj, sb->s_blocksize);
}

/**
 * apfs_multiblock_set_csum - Set an object's checksum
 * @object:	the object to checksum
 * @size:	size of the object in bytes (may be multiple blocks)
 */
void apfs_multiblock_set_csum(char *object, u32 size)
{
	struct apfs_obj_phys *obj = (struct apfs_obj_phys *)object;
	u64 cksum;

	cksum = apfs_fletcher64(object + APFS_MAX_CKSUM_SIZE, size - APFS_MAX_CKSUM_SIZE);
	obj->o_cksum = cpu_to_le64(cksum);
}

/**
 * apfs_create_cpm_block - Create a new checkpoint-mapping block
 * @sb:		filesystem superblock
 * @bno:	block number to use
 * @bh_p:	on return, the buffer head for the block
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_create_cpm_block(struct super_block *sb, u64 bno, struct buffer_head **bh_p)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_checkpoint_map_phys *cpm = NULL;
	struct buffer_head *bh = NULL;
	int err;

	bh = apfs_getblk(sb, bno);
	if (!bh) {
		apfs_err(sb, "failed to map cpm block");
		return -EIO;
	}
	err = apfs_transaction_join(sb, bh);
	if (err) {
		brelse(bh);
		return err;
	}
	set_buffer_csum(bh);

	cpm = (void *)bh->b_data;
	memset(cpm, 0, sb->s_blocksize);
	cpm->cpm_o.o_oid = cpu_to_le64(bno);
	cpm->cpm_o.o_xid = cpu_to_le64(nxi->nx_xid);
	cpm->cpm_o.o_type = cpu_to_le32(APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CHECKPOINT_MAP);
	cpm->cpm_o.o_subtype = cpu_to_le32(APFS_OBJECT_TYPE_INVALID);

	/* For now: the caller will have to update these fields */
	cpm->cpm_flags = cpu_to_le32(APFS_CHECKPOINT_MAP_LAST);
	cpm->cpm_count = 0;

	*bh_p = bh;
	return 0;
}

/**
 * apfs_create_cpoint_map - Create a checkpoint mapping for an object
 * @sb:		filesystem superblock
 * @cpm:	checkpoint mapping block to use
 * @obj:	header for the ephemeral object
 * @bno:	block number for the ephemeral object
 * @size:	size of the ephemeral object in bytes
 *
 * Returns 0 on success or a negative error code in case of failure, which may
 * be -ENOSPC if @cpm is full.
 */
int apfs_create_cpoint_map(struct super_block *sb, struct apfs_checkpoint_map_phys *cpm, struct apfs_obj_phys *obj, u64 bno, u32 size)
{
	struct apfs_checkpoint_mapping *map = NULL;
	u32 cpm_count;

	apfs_assert_in_transaction(sb, &cpm->cpm_o);

	cpm_count = le32_to_cpu(cpm->cpm_count);
	if (cpm_count >= apfs_max_maps_per_block(sb))
		return -ENOSPC;
	map = &cpm->cpm_map[cpm_count];
	le32_add_cpu(&cpm->cpm_count, 1);

	map->cpm_type = obj->o_type;
	map->cpm_subtype = obj->o_subtype;
	map->cpm_size = cpu_to_le32(size);
	map->cpm_pad = 0;
	map->cpm_fs_oid = 0;
	map->cpm_oid = obj->o_oid;
	map->cpm_paddr = cpu_to_le64(bno);
	return 0;
}

/**
 * apfs_index_in_data_area - Get position of block in current checkpoint's data
 * @sb:		superblock structure
 * @bno:	block number
 */
u32 apfs_index_in_data_area(struct super_block *sb, u64 bno)
{
	struct apfs_nx_superblock *raw_sb = APFS_NXI(sb)->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_index = le32_to_cpu(raw_sb->nx_xp_data_index);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);
	u64 tmp;

	tmp = bno - data_base + data_blks - data_index;
	return do_div(tmp, data_blks);
}

/**
 * apfs_data_index_to_bno - Convert index in data area to block number
 * @sb:		superblock structure
 * @index:	index of the block in the current checkpoint's data area
 */
u64 apfs_data_index_to_bno(struct super_block *sb, u32 index)
{
	struct apfs_nx_superblock *raw_sb = APFS_NXI(sb)->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_index = le32_to_cpu(raw_sb->nx_xp_data_index);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);

	return data_base + (index + data_index) % data_blks;
}

/**
 * apfs_ephemeral_object_lookup - Find an ephemeral object info in memory
 * @sb:		superblock structure
 * @oid:	ephemeral object id
 *
 * Returns a pointer to the object info on success, or an error pointer in case
 * of failure.
 */
struct apfs_ephemeral_object_info *apfs_ephemeral_object_lookup(struct super_block *sb, u64 oid)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_ephemeral_object_info *list = NULL;
	int i;

	list = nxi->nx_eph_list;
	for (i = 0; i < nxi->nx_eph_count; ++i) {
		if (list[i].oid == oid)
			return &list[i];
	}
	apfs_err(sb, "no mapping for oid 0x%llx", oid);
	return ERR_PTR(-EFSCORRUPTED);
}

/**
 * apfs_read_object_block - Map a non-ephemeral object block
 * @sb:		superblock structure
 * @bno:	block number for the object
 * @write:	request write access?
 * @preserve:	preserve the old block?
 *
 * On success returns the mapped buffer head for the object, which may now be
 * in a new location if write access was requested.  Returns an error pointer
 * in case of failure.
 */
struct buffer_head *apfs_read_object_block(struct super_block *sb, u64 bno, bool write, bool preserve)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_superblock *vsb_raw = NULL;
	struct buffer_head *bh, *new_bh;
	struct apfs_obj_phys *obj;
	u32 type;
	u64 new_bno;
	int err;

	ASSERT(write || !preserve);

	bh = apfs_sb_bread(sb, bno);
	if (!bh) {
		apfs_err(sb, "failed to read object block 0x%llx", bno);
		return ERR_PTR(-EIO);
	}

	obj = (struct apfs_obj_phys *)bh->b_data;
	type = le32_to_cpu(obj->o_type);
	ASSERT(!(type & APFS_OBJ_EPHEMERAL));
	if (nxi->nx_flags & APFS_CHECK_NODES && !apfs_obj_verify_csum(sb, bh)) {
		apfs_err(sb, "bad checksum for object in block 0x%llx", bno);
		err = -EFSBADCRC;
		goto fail;
	}

	if (!write)
		return bh;
	ASSERT(!(sb->s_flags & SB_RDONLY));

	/* Is the object already part of the current transaction? */
	if (obj->o_xid == cpu_to_le64(nxi->nx_xid))
		return bh;

	err = apfs_spaceman_allocate_block(sb, &new_bno, true /* backwards */);
	if (err) {
		apfs_err(sb, "block allocation failed");
		goto fail;
	}
	new_bh = apfs_getblk(sb, new_bno);
	if (!new_bh) {
		apfs_err(sb, "failed to map block for CoW (0x%llx)", new_bno);
		err = -EIO;
		goto fail;
	}
	memcpy(new_bh->b_data, bh->b_data, sb->s_blocksize);

	/*
	 * Don't free the old copy of the object if it's part of a snapshot.
	 * Also increase the allocation count, except for the volume superblock
	 * which is never counted there.
	 */
	if (!preserve) {
		err = apfs_free_queue_insert(sb, bh->b_blocknr, 1);
		if (err)
			apfs_err(sb, "free queue insertion failed for 0x%llx", (unsigned long long)bh->b_blocknr);
	} else if ((type & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_FS) {
		vsb_raw = APFS_SB(sb)->s_vsb_raw;
		apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
		le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, 1);
		le64_add_cpu(&vsb_raw->apfs_total_blocks_alloced, 1);
	}

	brelse(bh);
	bh = new_bh;
	new_bh = NULL;
	if (err)
		goto fail;
	obj = (struct apfs_obj_phys *)bh->b_data;

	if (type & APFS_OBJ_PHYSICAL)
		obj->o_oid = cpu_to_le64(new_bno);
	obj->o_xid = cpu_to_le64(nxi->nx_xid);
	err = apfs_transaction_join(sb, bh);
	if (err)
		goto fail;

	set_buffer_csum(bh);
	return bh;

fail:
	brelse(bh);
	return ERR_PTR(err);
}
