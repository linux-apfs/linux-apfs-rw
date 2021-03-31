// SPDX-License-Identifier: GPL-2.0
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
	int i;

	for (i = 0; i < len/sizeof(u32); i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - do_div(c1, 0xFFFFFFFF);
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - do_div(c2, 0xFFFFFFFF);

	return (c2 << 32) | c1;
}

int apfs_obj_verify_csum(struct super_block *sb, struct apfs_obj_phys *obj)
{
	return  (le64_to_cpu(obj->o_cksum) ==
		 apfs_fletcher64((char *) obj + APFS_MAX_CKSUM_SIZE,
				 sb->s_blocksize - APFS_MAX_CKSUM_SIZE));
}

/**
 * apfs_obj_set_csum - Set the fletcher checksum in an object header
 * @sb:		superblock structure
 * @obj:	the object header
 */
void apfs_obj_set_csum(struct super_block *sb, struct apfs_obj_phys *obj)
{
	u64 cksum = apfs_fletcher64((char *)obj + APFS_MAX_CKSUM_SIZE,
				    sb->s_blocksize - APFS_MAX_CKSUM_SIZE);

	obj->o_cksum = cpu_to_le64(cksum);
}

/**
 * apfs_cpm_lookup_oid - Search a checkpoint-mapping block for a given oid
 * @sb:		superblock structure
 * @cpm:	checkpoint-mapping block (on disk)
 * @oid:	the ephemeral object id to look up
 * @bno:	on return, the block number for the object
 *
 * Returns -EFSCORRUPTED in case of corruption, or -EAGAIN if @oid is not
 * listed in @cpm; returns 0 on success.
 */
static int apfs_cpm_lookup_oid(struct super_block *sb,
			       struct apfs_checkpoint_map_phys *cpm,
			       u64 oid, u64 *bno)
{
	u32 map_count = le32_to_cpu(cpm->cpm_count);
	int i;

	if (map_count > apfs_max_maps_per_block(sb))
		return -EFSCORRUPTED;

	for (i = 0; i < map_count; ++i) {
		struct apfs_checkpoint_mapping *map = &cpm->cpm_map[i];

		if (le64_to_cpu(map->cpm_oid) == oid) {
			*bno = le64_to_cpu(map->cpm_paddr);
			return 0;
		}
	}
	return -EAGAIN; /* The mapping may still be in the next block */
}

/**
 * apfs_create_cpoint_map - Create a checkpoint mapping
 * @sb:		filesystem superblock
 * @oid:	ephemeral object id
 * @bno:	block number
 *
 * Only mappings for free queue nodes are supported for now.  Returns 0 on
 * success or a negative error code in case of failure.
 */
int apfs_create_cpoint_map(struct super_block *sb, u64 oid, u64 bno)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	struct buffer_head *bh;
	struct apfs_checkpoint_map_phys *cpm;
	struct apfs_checkpoint_mapping *map;
	u64 cpm_bno;
	u32 cpm_count;
	int err = 0;

	if (!desc_blks || desc_len < 2)
		return -EFSCORRUPTED;

	/* Last block in area is superblock; we want the last mapping block */
	cpm_bno = desc_base + (desc_index + desc_len - 2) % desc_blks;
	bh = sb_bread(sb, cpm_bno);
	if (!bh)
		return -EIO;
	cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;
	ASSERT(sbi->s_xid == le64_to_cpu(cpm->cpm_o.o_xid));

	cpm_count = le32_to_cpu(cpm->cpm_count);
	if (cpm_count >= apfs_max_maps_per_block(sb)) { /* TODO */
		apfs_warn(sb, "creation of cpm blocks not yet supported");
		err = -EOPNOTSUPP;
		goto fail;
	}
	map = &cpm->cpm_map[cpm_count];
	le32_add_cpu(&cpm->cpm_count, 1);

	map->cpm_type = cpu_to_le32(APFS_OBJ_EPHEMERAL |
				    APFS_OBJECT_TYPE_BTREE_NODE);
	map->cpm_subtype = cpu_to_le32(APFS_OBJECT_TYPE_SPACEMAN_FREE_QUEUE);
	map->cpm_size = cpu_to_le32(sb->s_blocksize);
	map->cpm_pad = 0;
	map->cpm_fs_oid = 0;
	map->cpm_oid = cpu_to_le64(oid);
	map->cpm_paddr = cpu_to_le64(bno);

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_read_ephemeral_object - Find and map an ephemeral object
 * @sb:		superblock structure
 * @oid:	ephemeral object id
 *
 * Returns the mapped buffer head for the object, or an error pointer in case
 * of failure.
 */
struct buffer_head *apfs_read_ephemeral_object(struct super_block *sb, u64 oid)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *raw_sb = sbi->s_msb_raw;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u32 i;

	if (!desc_blks || !desc_len)
		return ERR_PTR(-EFSCORRUPTED);

	/* Last block in the area is superblock; the rest are mapping blocks */
	for (i = 0; i < desc_len - 1; ++i) {
		struct buffer_head *bh;
		struct apfs_checkpoint_map_phys *cpm;
		u64 cpm_bno = desc_base + (desc_index + i) % desc_blks;
		u64 obj_bno;
		int err;

		bh = sb_bread(sb, cpm_bno);
		if (!bh)
			return ERR_PTR(-EIO);
		cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;

		err = apfs_cpm_lookup_oid(sb, cpm, oid, &obj_bno);
		brelse(bh);
		cpm = NULL;
		if (err == -EAGAIN) /* Search the next mapping block */
			continue;
		if (err)
			return ERR_PTR(err);

		bh = sb_bread(sb, obj_bno);
		if (!bh)
			return ERR_PTR(-EIO);
		return bh;
	}
	return ERR_PTR(-EFSCORRUPTED); /* The mapping is missing */
}

/**
 * apfs_read_object_block - Map a non-ephemeral object block
 * @sb:		superblock structure
 * @bno:	block number for the object
 * @write:	request write access?
 *
 * On success returns the mapped buffer head for the object, which may now be
 * in a new location if write access was requested.  Returns an error pointer
 * in case of failure.
 */
struct buffer_head *apfs_read_object_block(struct super_block *sb, u64 bno,
					   bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct buffer_head *bh, *new_bh;
	struct apfs_obj_phys *obj;
	u32 type;
	u64 new_bno;
	int err;

	bh = sb_bread(sb, bno);
	if (!bh)
		return ERR_PTR(-EIO);

	obj = (struct apfs_obj_phys *)bh->b_data;
	type = le32_to_cpu(obj->o_type);
	ASSERT(!(type & APFS_OBJ_EPHEMERAL));
	if (sbi->s_flags & APFS_CHECK_NODES && !apfs_obj_verify_csum(sb, obj)) {
		err = -EFSBADCRC;
		goto fail;
	}

	if (!write)
		return bh;
	ASSERT(!(sb->s_flags & SB_RDONLY));

	/* Is the object already part of the current transaction? */
	if (obj->o_xid == cpu_to_le64(sbi->s_xid))
		return bh;

	err = apfs_spaceman_allocate_block(sb, &new_bno);
	if (err)
		goto fail;
	new_bh = sb_bread(sb, new_bno);
	if (!new_bh) {
		err = -EIO;
		goto fail;
	}
	memcpy(new_bh->b_data, bh->b_data, sb->s_blocksize);

	err = apfs_free_queue_insert(sb, bh->b_blocknr);
	brelse(bh);
	bh = new_bh;
	new_bh = NULL;
	if (err)
		goto fail;
	obj = (struct apfs_obj_phys *)bh->b_data;

	if (type & APFS_OBJ_PHYSICAL)
		obj->o_oid = cpu_to_le64(new_bno);
	obj->o_xid = cpu_to_le64(sbi->s_xid);
	err = apfs_transaction_join(sb, bh);
	if (err)
		goto fail;

	set_buffer_csum(bh);
	return bh;

fail:
	brelse(bh);
	return ERR_PTR(err);
}
