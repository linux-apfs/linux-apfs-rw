// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include "apfs.h"
#include "version.h"

#define APFS_MODULE_ID_STRING	"linux-apfs by eafer (" GIT_COMMIT ")"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
#include <linux/iversion.h>
#endif

/* Keep a list of mounted containers, so that their volumes can share them */
static LIST_HEAD(nxs);
/*
 * The main purpose of this mutex is to protect the list of containers and
 * their reference counts, but it also has other uses during mounts/unmounts:
 *   - it prevents new mounts from starting while an unmount is updating the
 *     backup superblock (apfs_attach_nxi vs apfs_make_super_copy)
 *   - it prevents a new container superblock read from starting while another
 *     is taking place, which could cause leaks and other issues if both
 *     containers are the same (apfs_read_main_super vs itself)
 *   - it protects the list of volumes for each container, and keeps it
 *     consistent with the reference count
 *   - it prevents two different snapshots for a single volume from trying to
 *     do the first read of their shared omap at the same time
 *     (apfs_first_read_omap vs itself)
 *   - it protects the reference count for that shared omap, keeping it
 *     consistent with the number of volumes that are set with that omap
 *   - it protects the container mount flags, so that they can only be set by
 *     the first volume mount to attempt it (apfs_set_nx_flags vs itself)
 */
DEFINE_MUTEX(nxs_mutex);

/**
 * apfs_nx_find_by_dev - Search for a device in the list of mounted containers
 * @dev:	device number of block device for the wanted container
 *
 * Returns a pointer to the container structure in the list, or NULL if the
 * container isn't currently mounted.
 */
static struct apfs_nxsb_info *apfs_nx_find_by_dev(dev_t dev)
{
	struct apfs_nxsb_info *curr;

	lockdep_assert_held(&nxs_mutex);
	list_for_each_entry(curr, &nxs, nx_list) {
		struct block_device *curr_bdev = curr->nx_blkdev_info->blki_bdev;

		if (curr_bdev->bd_dev == dev)
			return curr;
	}
	return NULL;
}

/**
 * apfs_blkdev_set_blocksize - Set the blocksize for a block device
 * @info:	info struct for the block device
 * @size:	size to set
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_blkdev_set_blocksize(struct apfs_blkdev_info *info, int size)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	return set_blocksize(info->blki_bdev_file, size);
#else
	return set_blocksize(info->blki_bdev, size);
#endif
}

/**
 * apfs_sb_set_blocksize - Set the block size for the container's device
 * @sb:		superblock structure
 * @size:	size to set
 *
 * This is like sb_set_blocksize(), but it uses the container's device instead
 * of the nonexistent volume device.
 */
static int apfs_sb_set_blocksize(struct super_block *sb, int size)
{
	if (apfs_blkdev_set_blocksize(APFS_NXI(sb)->nx_blkdev_info, size))
		return 0;
	sb->s_blocksize = size;
	sb->s_blocksize_bits = blksize_bits(size);
	return sb->s_blocksize;
}

/**
 * apfs_read_super_copy - Read the copy of the container superblock in block 0
 * @sb: superblock structure
 *
 * Returns a pointer to the buffer head, or an error pointer in case of failure.
 */
static struct buffer_head *apfs_read_super_copy(struct super_block *sb)
{
	struct buffer_head *bh;
	struct apfs_nx_superblock *msb_raw;
	int blocksize;
	int err = -EINVAL;

	/*
	 * For now assume a small blocksize, we only need it so that we can
	 * read the actual blocksize from disk.
	 */
	if (!apfs_sb_set_blocksize(sb, APFS_NX_DEFAULT_BLOCK_SIZE)) {
		apfs_err(sb, "unable to set blocksize");
		return ERR_PTR(err);
	}
	bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "unable to read superblock");
		return ERR_PTR(err);
	}
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	blocksize = le32_to_cpu(msb_raw->nx_block_size);

	sb->s_magic = le32_to_cpu(msb_raw->nx_magic);
	if (sb->s_magic != APFS_NX_MAGIC) {
		apfs_warn(sb, "not an apfs container - are you mounting the right partition?");
		goto fail;
	}

	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!apfs_sb_set_blocksize(sb, blocksize)) {
			apfs_err(sb, "bad blocksize %d", blocksize);
			return ERR_PTR(err);
		}
		bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
		if (!bh) {
			apfs_err(sb, "unable to read superblock 2nd time");
			return ERR_PTR(err);
		}
		msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	}

	if (!apfs_obj_verify_csum(sb, bh))
		apfs_notice(sb, "backup superblock seems corrupted");
	return bh;

fail:
	brelse(bh);
	return ERR_PTR(err);
}

/**
 * apfs_make_super_copy - Write a copy of the checkpoint superblock to block 0
 * @sb:	superblock structure
 */
static void apfs_make_super_copy(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	struct buffer_head *bh;

	if (!(nxi->nx_flags & APFS_READWRITE))
		return;

	/*
	 * Only update the backup when the last volume is getting unmounted.
	 * Of course a new mounter could still come along before we actually
	 * release the nxi.
	 */
	mutex_lock(&nxs_mutex);
	if (nxi->nx_refcnt > 1)
		goto out_unlock;

	bh = apfs_sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "failed to write block zero");
		goto out_unlock;
	}
	memcpy(bh->b_data, nxi->nx_raw, sb->s_blocksize);
	mark_buffer_dirty(bh);
	brelse(bh);
out_unlock:
	mutex_unlock(&nxs_mutex);
}

static int apfs_check_nx_features(struct super_block *sb);
static void apfs_set_trans_buffer_limit(struct super_block *sb);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
static inline void import_uuid(uuid_t *dst, const __u8 *src)
{
	memcpy(dst, src, sizeof(uuid_t));
}
#endif

/**
 * apfs_check_fusion_uuid - Verify that the main and tier 2 devices match
 * @sb: filesystem superblock
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_check_fusion_uuid(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = NULL;
	struct apfs_nx_superblock *main_raw = NULL, *tier2_raw = NULL;
	uuid_t main_uuid, tier2_uuid;
	struct buffer_head *bh = NULL;

	nxi = APFS_NXI(sb);
	main_raw = nxi->nx_raw;
	if (!main_raw) {
		apfs_alert(sb, "fusion uuid checks are misplaced");
		return -EINVAL;
	}
	import_uuid(&main_uuid, main_raw->nx_fusion_uuid);
	main_raw = NULL;

	if (!nxi->nx_tier2_info) {
		/* Not a fusion drive */
		if (!uuid_is_null(&main_uuid)) {
			apfs_err(sb, "fusion uuid on a regular drive");
			return -EFSCORRUPTED;
		}
		return 0;
	}
	if (uuid_is_null(&main_uuid)) {
		apfs_err(sb, "no fusion uuid on fusion drive");
		return -EFSCORRUPTED;
	}

	/* Tier 2 also has a copy of the superblock in block zero */
	bh = apfs_sb_bread(sb, nxi->nx_tier2_bno);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	tier2_raw = (struct apfs_nx_superblock *)bh->b_data;
	import_uuid(&tier2_uuid, tier2_raw->nx_fusion_uuid);
	brelse(bh);
	bh = NULL;
	tier2_raw = NULL;

	/*
	 * The only difference between both superblocks (other than the
	 * checksum) is this one bit here, so it can be used to tell which is
	 * main and which is tier 2. By the way, the reference seems to have
	 * this backwards.
	 */
	if (main_uuid.b[15] & 0x01) {
		apfs_warn(sb, "bad bit on main device - are you mixing up main and tier 2?");
		return -EINVAL;
	}
	if (!(tier2_uuid.b[15] & 0x01)) {
		apfs_warn(sb, "bad bit on tier 2 device - are you mixing up main and tier 2?");
		return -EINVAL;
	}
	tier2_uuid.b[15] &= ~0x01;
	if (!uuid_equal(&main_uuid, &tier2_uuid)) {
		apfs_warn(sb, "the devices are not part of the same fusion drive");
		return -EINVAL;
	}
	return 0;
}

/**
 * apfs_read_main_super - Find the container superblock and read it into memory
 * @sb:	superblock structure
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets the nx_raw and nx_xid fields of APFS_NXI(@sb).
 */
static int apfs_read_main_super(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct buffer_head *bh = NULL;
	struct buffer_head *desc_bh = NULL;
	struct apfs_nx_superblock *msb_raw;
	u64 xid, bno = APFS_NX_BLOCK_NUM;
	u64 desc_base;
	u32 desc_blocks;
	int err = -EINVAL;
	int i;

	mutex_lock(&nxs_mutex);

	if (nxi->nx_blocksize) {
		/* It's already mapped */
		sb->s_blocksize = nxi->nx_blocksize;
		sb->s_blocksize_bits = nxi->nx_blocksize_bits;
		sb->s_magic = le32_to_cpu(nxi->nx_raw->nx_magic);
		err = 0;
		goto out;
	}

	/*
	 * We won't know the block size until we read the backup superblock,
	 * so we can't set this up correctly yet. But we do know that the
	 * backup superblock itself is always in the main device.
	 */
	nxi->nx_tier2_bno = APFS_NX_BLOCK_NUM + 1;

	/* Read the superblock from the last clean unmount */
	bh = apfs_read_super_copy(sb);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
		bh = NULL;
		goto out;
	}
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;

	/*
	 * Now that we confirmed the block size, we can set this up for real.
	 * It's important to do this early because I don't know which mount
	 * objects could get moved to tier 2.
	 */
	nxi->nx_tier2_bno = APFS_FUSION_TIER2_DEVICE_BYTE_ADDR >> sb->s_blocksize_bits;

	/* We want to mount the latest valid checkpoint among the descriptors */
	desc_base = le64_to_cpu(msb_raw->nx_xp_desc_base);
	if (desc_base >> 63 != 0) {
		/* The highest bit is set when checkpoints are not contiguous */
		apfs_err(sb, "checkpoint descriptor tree not yet supported");
		goto out;
	}
	desc_blocks = le32_to_cpu(msb_raw->nx_xp_desc_blocks);
	if (desc_blocks > 10000) { /* Arbitrary loop limit, is it enough? */
		apfs_err(sb, "too many checkpoint descriptors?");
		err = -EFSCORRUPTED;
		goto out;
	}

	/* Now we go through the checkpoints one by one */
	xid = le64_to_cpu(msb_raw->nx_o.o_xid);
	for (i = 0; i < desc_blocks; ++i) {
		struct apfs_nx_superblock *desc_raw;

		brelse(desc_bh);
		desc_bh = apfs_sb_bread(sb, desc_base + i);
		if (!desc_bh) {
			apfs_err(sb, "unable to read checkpoint descriptor");
			goto out;
		}
		desc_raw = (struct apfs_nx_superblock *)desc_bh->b_data;

		if (le32_to_cpu(desc_raw->nx_magic) != APFS_NX_MAGIC)
			continue; /* Not a superblock */
		if (le64_to_cpu(desc_raw->nx_o.o_xid) <= xid)
			continue; /* Old */
		if (!apfs_obj_verify_csum(sb, desc_bh))
			continue; /* Corrupted */

		xid = le64_to_cpu(desc_raw->nx_o.o_xid);
		msb_raw = desc_raw;
		bno = desc_base + i;
		brelse(bh);
		bh = desc_bh;
		desc_bh = NULL;
	}

	nxi->nx_raw = kmalloc(sb->s_blocksize, GFP_KERNEL);
	if (!nxi->nx_raw) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(nxi->nx_raw, bh->b_data, sb->s_blocksize);
	nxi->nx_bno = bno;
	nxi->nx_xid = xid;

	/* For now we only support blocksize < PAGE_SIZE */
	nxi->nx_blocksize = sb->s_blocksize;
	nxi->nx_blocksize_bits = sb->s_blocksize_bits;
	apfs_set_trans_buffer_limit(sb);

	err = apfs_check_nx_features(sb);
	if (err)
		goto out;

	/*
	 * This check is technically too late: if main and tier 2 are backwards
	 * then we have already attempted (and failed) to read the checkpoint
	 * from tier 2. This may lead to a confusing error message if tier 2
	 * is absurdly tiny, not a big deal.
	 */
	err = apfs_check_fusion_uuid(sb);
	if (err)
		goto out;

out:
	brelse(bh);
	mutex_unlock(&nxs_mutex);
	return err;
}

/**
 * apfs_update_software_info - Write the module info to a modified volume
 * @sb: superblock structure
 *
 * Writes this module's information to index zero of the apfs_modified_by
 * array, shifting the rest of the entries to the right.
 */
static void apfs_update_software_info(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *raw = sbi->s_vsb_raw;
	struct apfs_modified_by *mod_by;

	ASSERT(sbi->s_vsb_raw);
	apfs_assert_in_transaction(sb, &raw->apfs_o);
	ASSERT(strlen(APFS_MODULE_ID_STRING) < APFS_MODIFIED_NAMELEN);
	mod_by = raw->apfs_modified_by;

	memmove(mod_by + 1, mod_by, (APFS_MAX_HIST - 1) * sizeof(*mod_by));
	memset(mod_by->id, 0, sizeof(mod_by->id));
	strscpy(mod_by->id, APFS_MODULE_ID_STRING, sizeof(mod_by->id));
	mod_by->timestamp = cpu_to_le64(ktime_get_real_ns());
	mod_by->last_xid = cpu_to_le64(APFS_NXI(sb)->nx_xid);
}

static struct file_system_type apfs_fs_type;

/**
 * apfs_blkdev_cleanup - Clean up after a block device
 * @info:	info struct to clean up
 */
static void apfs_blkdev_cleanup(struct apfs_blkdev_info *info)
{
	if (!info)
		return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0) || RHEL_VERSION_GE(9, 5)
	fput(info->blki_bdev_file);
	info->blki_bdev_file = NULL;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	bdev_release(info->blki_bdev_handle);
	info->blki_bdev_handle = NULL;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0) || RHEL_VERSION_GE(9, 4)
	blkdev_put(info->blki_bdev, &apfs_fs_type);
#else
	blkdev_put(info->blki_bdev, info->blki_mode);
#endif
	info->blki_bdev = NULL;

	kfree(info->blki_path);
	info->blki_path = NULL;
	kfree(info);
}

/**
 * apfs_free_main_super - Clean up apfs_read_main_super()
 * @sbi:	in-memory superblock info
 *
 * It also cleans up after apfs_attach_nxi(), so the name is no longer accurate.
 */
static inline void apfs_free_main_super(struct apfs_sb_info *sbi)
{
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	struct apfs_ephemeral_object_info *eph_list = NULL;
	struct apfs_spaceman *sm = NULL;
	u32 bmap_idx;
	int i;

	mutex_lock(&nxs_mutex);

	list_del(&sbi->list);
	if (--nxi->nx_refcnt)
		goto out;

	/* Clean up all the ephemeral objects in memory */
	eph_list = nxi->nx_eph_list;
	if (eph_list) {
		for (i = 0; i < nxi->nx_eph_count; ++i) {
			kfree(eph_list[i].object);
			eph_list[i].object = NULL;
		}
		kfree(eph_list);
		eph_list = nxi->nx_eph_list = NULL;
		nxi->nx_eph_count = 0;
	}

	kfree(nxi->nx_raw);
	nxi->nx_raw = NULL;

	apfs_blkdev_cleanup(nxi->nx_blkdev_info);
	nxi->nx_blkdev_info = NULL;
	apfs_blkdev_cleanup(nxi->nx_tier2_info);
	nxi->nx_tier2_info = NULL;

	list_del(&nxi->nx_list);
	sm = nxi->nx_spaceman;
	if (sm) {
		for (bmap_idx = 0; bmap_idx < sm->sm_ip_bmaps_count; ++bmap_idx) {
			kfree(sm->sm_ip_bmaps[bmap_idx].block);
			sm->sm_ip_bmaps[bmap_idx].block = NULL;
		}
		kfree(sm);
		nxi->nx_spaceman = sm = NULL;
	}
	kfree(nxi);
out:
	sbi->s_nxi = NULL;
	mutex_unlock(&nxs_mutex);
}

/**
 * apfs_map_volume_super_bno - Map a block containing a volume superblock
 * @sb:		superblock structure
 * @bno:	block to map
 * @check:	verify the checksum?
 */
int apfs_map_volume_super_bno(struct super_block *sb, u64 bno, bool check)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = NULL;
	struct buffer_head *bh = NULL;
	int err;

	bh = apfs_sb_bread(sb, bno);
	if (!bh) {
		apfs_err(sb, "unable to read volume superblock");
		return -EINVAL;
	}

	vsb_raw = (struct apfs_superblock *)bh->b_data;
	if (le32_to_cpu(vsb_raw->apfs_magic) != APFS_MAGIC) {
		apfs_err(sb, "wrong magic in volume superblock");
		err = -EINVAL;
		goto fail;
	}

	/*
	 * XXX: apfs_omap_lookup_block() only runs this check when write
	 * is true, but it should always do it.
	 */
	if (check && !apfs_obj_verify_csum(sb, bh)) {
		apfs_err(sb, "inconsistent volume superblock");
		err = -EFSBADCRC;
		goto fail;
	}

	sbi->s_vsb_raw = vsb_raw;
	sbi->s_vobject.sb = sb;
	sbi->s_vobject.block_nr = bno;
	sbi->s_vobject.oid = le64_to_cpu(vsb_raw->apfs_o.o_oid);
	brelse(sbi->s_vobject.o_bh);
	sbi->s_vobject.o_bh = bh;
	sbi->s_vobject.data = bh->b_data;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_alloc_omap - Allocate and initialize an object map struct
 *
 * Returns the struct, or NULL in case of allocation failure.
 */
static struct apfs_omap *apfs_alloc_omap(void)
{
	struct apfs_omap *omap = NULL;
	struct apfs_omap_cache *cache = NULL;

	omap = kzalloc(sizeof(*omap), GFP_KERNEL);
	if (!omap)
		return NULL;
	cache = &omap->omap_cache;
	spin_lock_init(&cache->lock);
	return omap;
}

/**
 * apfs_map_volume_super - Find the volume superblock and map it into memory
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets APFS_SB(@sb)->s_vsb_raw and APFS_SB(@sb)->s_vobject.
 */
int apfs_map_volume_super(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *msb_raw = nxi->nx_raw;
	struct apfs_omap_phys *msb_omap_raw;
	struct apfs_omap *omap = NULL;
	struct apfs_node *vnode;
	struct buffer_head *bh;
	u64 vol_id;
	u64 vsb;
	int err;

	ASSERT(msb_raw);

	/* Get the id for the requested volume number */
	if (sbi->s_vol_nr >= APFS_NX_MAX_FILE_SYSTEMS) {
		apfs_err(sb, "volume number out of range");
		return -EINVAL;
	}
	vol_id = le64_to_cpu(msb_raw->nx_fs_oid[sbi->s_vol_nr]);
	if (vol_id == 0) {
		apfs_err(sb, "requested volume does not exist");
		return -EINVAL;
	}

	/* Get the container's object map */
	bh = apfs_read_object_block(sb, le64_to_cpu(msb_raw->nx_omap_oid),
				    write, false /* preserve */);
	if (IS_ERR(bh)) {
		apfs_err(sb, "unable to read container object map");
		return PTR_ERR(bh);
	}
	if (write) {
		ASSERT(buffer_trans(bh));
		msb_raw->nx_omap_oid = cpu_to_le64(bh->b_blocknr);
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the root node for the container's omap */
	vnode = apfs_read_node(sb, le64_to_cpu(msb_omap_raw->om_tree_oid),
			       APFS_OBJ_PHYSICAL, write);
	if (IS_ERR(vnode)) {
		apfs_err(sb, "unable to read volume block");
		err = PTR_ERR(vnode);
		goto fail;
	}
	if (write) {
		ASSERT(buffer_trans(bh));
		msb_omap_raw->om_tree_oid = cpu_to_le64(vnode->object.block_nr);
	}
	msb_omap_raw = NULL;
	brelse(bh);
	bh = NULL;

	omap = apfs_alloc_omap();
	if (!omap) {
		apfs_node_free(vnode);
		return -ENOMEM;
	}
	omap->omap_root = vnode;

	err = apfs_omap_lookup_block(sb, omap, vol_id, &vsb, write);
	apfs_node_free(vnode);
	vnode = NULL;
	kfree(omap);
	omap = NULL;
	if (err) {
		apfs_err(sb, "volume not found, likely corruption");
		return err;
	}

	/*
	 * Snapshots could get mounted during a transaction, so the fletcher
	 * checksum doesn't have to be valid.
	 */
	return apfs_map_volume_super_bno(sb, vsb, !write && !sbi->s_snap_name);

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_unmap_volume_super - Clean up apfs_map_volume_super()
 * @sb:	filesystem superblock
 */
void apfs_unmap_volume_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_object *obj = &sbi->s_vobject;

	obj->data = NULL;
	brelse(obj->o_bh);
	obj->o_bh = NULL;
}

/**
 * apfs_get_omap - Get a reference to the omap, if it's already read
 * @sb:	filesystem superblock
 *
 * Returns the omap struct, or NULL on failure.
 */
static struct apfs_omap *apfs_get_omap(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_sb_info *curr = NULL;
	struct apfs_omap *omap = NULL;
	struct apfs_omap_cache *cache = NULL;

	lockdep_assert_held(&nxs_mutex);

	list_for_each_entry(curr, &nxi->vol_list, list) {
		if (curr == sbi)
			continue;
		if (curr->s_vol_nr == sbi->s_vol_nr) {
			omap = curr->s_omap;
			if (!omap) {
				/*
				 * This volume has already gone through
				 * apfs_attach_nxi(), but its omap is either
				 * not yet read or already put.
				 */
				continue;
			}
			cache = &omap->omap_cache;
			++omap->omap_refcnt;
			/* Right now the cache can't be shared like this */
			cache->disabled = true;
			return omap;
		}
	}
	return NULL;
}

/**
 * apfs_read_omap - Find and read the omap root node
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * On success, returns 0 and sets the fields of APFS_SB(@sb)->s_omap; on failure
 * returns a negative error code.
 */
int apfs_read_omap(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_omap_phys *omap_raw;
	struct apfs_node *omap_root;
	struct apfs_omap *omap = NULL;
	struct buffer_head *bh;
	u64 omap_blk;
	int err;

	ASSERT(sbi->s_vsb_raw);

	ASSERT(sbi->s_omap);
	omap = sbi->s_omap;

	/* Get the block holding the volume omap information */
	omap_blk = le64_to_cpu(vsb_raw->apfs_omap_oid);
	bh = apfs_read_object_block(sb, omap_blk, write, false /* preserve */);
	if (IS_ERR(bh)) {
		apfs_err(sb, "unable to read the volume object map");
		return PTR_ERR(bh);
	}
	if (write) {
		apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
		vsb_raw->apfs_omap_oid = cpu_to_le64(bh->b_blocknr);
	}
	omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the volume's object map */
	omap_root = apfs_read_node(sb, le64_to_cpu(omap_raw->om_tree_oid),
				   APFS_OBJ_PHYSICAL, write);
	if (IS_ERR(omap_root)) {
		apfs_err(sb, "unable to read the omap root node");
		err = PTR_ERR(omap_root);
		goto fail;
	}
	if (write) {
		apfs_assert_in_transaction(sb, &omap_raw->om_o);
		ASSERT(buffer_trans(bh));
		omap_raw->om_tree_oid = cpu_to_le64(omap_root->object.block_nr);
	}
	omap->omap_latest_snap = le64_to_cpu(omap_raw->om_most_recent_snap);
	omap_raw = NULL;
	brelse(bh);

	if (omap->omap_root)
		apfs_node_free(omap->omap_root);
	omap->omap_root = omap_root;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_first_read_omap - Find and read the omap root node during mount
 * @sb:		superblock structure
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_omap; on failure returns a
 * negative error code.
 */
static int apfs_first_read_omap(struct super_block *sb)
{
	struct apfs_sb_info *sbi = NULL;
	struct apfs_omap *omap = NULL;
	int err;

	/*
	 * For each volume, the first mount that gets here is responsible
	 * for reading the omap. Other mounts (for other snapshots) just
	 * go through the container's volume list to retrieve it. This results
	 * in coarse locking as usual: with some thought it would be possible
	 * to allow other volumes to read their own omaps at the same time,
	 * but I don't see the point.
	 */
	mutex_lock(&nxs_mutex);

	sbi = APFS_SB(sb);

	/* The current transaction and all snapshots share a single omap */
	omap = apfs_get_omap(sb);
	if (omap) {
		sbi->s_omap = omap;
		err = 0;
		goto out;
	}

	omap = apfs_alloc_omap();
	if (!omap) {
		err = -ENOMEM;
		goto out;
	}

	sbi->s_omap = omap;
	err = apfs_read_omap(sb, false /* write */);
	if (err) {
		kfree(omap);
		sbi->s_omap = NULL;
		goto out;
	}

	++omap->omap_refcnt;
	err = 0;
out:
	mutex_unlock(&nxs_mutex);
	return err;
}

/**
 * apfs_unset_omap - Unset the object map in a superblock
 * @sb: superblock structure
 *
 * Shrinks the omap reference, frees the omap if needed, and sets the field to
 * NULL atomically in relation to apfs_first_read_omap(). So, no other mount
 * can grab a new reference halfway through.
 */
static void apfs_unset_omap(struct super_block *sb)
{
	struct apfs_omap **omap_p = NULL;
	struct apfs_omap *omap = NULL;

	omap_p = &APFS_SB(sb)->s_omap;
	omap = *omap_p;
	if (!omap)
		return;

	mutex_lock(&nxs_mutex);

	if (--omap->omap_refcnt != 0)
		goto out;

	apfs_node_free(omap->omap_root);
	kfree(omap);
out:
	*omap_p = NULL;
	mutex_unlock(&nxs_mutex);
}

/**
 * apfs_read_catalog - Find and read the catalog root node
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_cat_root; on failure returns
 * a negative error code.
 */
int apfs_read_catalog(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_node *root_node;

	ASSERT(sbi->s_omap && sbi->s_omap->omap_root);

	root_node = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_root_tree_oid),
				   APFS_OBJ_VIRTUAL, write);
	if (IS_ERR(root_node)) {
		apfs_err(sb, "unable to read catalog root node");
		return PTR_ERR(root_node);
	}

	if (sbi->s_cat_root)
		apfs_node_free(sbi->s_cat_root);
	sbi->s_cat_root = root_node;
	return 0;
}

static void apfs_put_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_transaction *trans = NULL;

	/* Cleanups won't reschedule themselves during unmount */
	flush_work(&sbi->s_orphan_cleanup_work);

	/* We are about to commit anyway */
	trans = &APFS_NXI(sb)->nx_transaction;
	cancel_delayed_work_sync(&trans->t_work);

	/* Stop flushing orphans and update the volume as needed */
	if (!(sb->s_flags & SB_RDONLY)) {
		struct apfs_superblock *vsb_raw;
		struct buffer_head *vsb_bh;
		int err;

		err = apfs_transaction_start(sb, APFS_TRANS_SYNC);
		if (err) {
			apfs_err(sb, "unmount transaction start failed (err:%d)", err);
			goto fail;
		}
		vsb_raw = sbi->s_vsb_raw;
		vsb_bh = sbi->s_vobject.o_bh;

		apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
		ASSERT(buffer_trans(vsb_bh));

		apfs_update_software_info(sb);
		vsb_raw->apfs_unmount_time = cpu_to_le64(ktime_get_real_ns());
		set_buffer_csum(vsb_bh);

		/* Guarantee commit */
		sbi->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
		err = apfs_transaction_commit(sb);
		if (err) {
			apfs_err(sb, "unmount transaction commit failed (err:%d)", err);
			apfs_transaction_abort(sb);
			goto fail;
		}
	}

	/*
	 * Even if this particular volume/snapshot was read-only, the container
	 * may have changed and need an update here.
	 */
	apfs_make_super_copy(sb);

fail:
	/*
	 * This is essentially the cleanup for apfs_fill_super(). It goes here
	 * because generic_shutdown_super() only calls ->put_super() when the
	 * root dentry has been set, that is, when apfs_fill_super() succeeded.
	 * The rest of the mount cleanup is done directly by ->kill_sb().
	 */
	iput(sbi->s_private_dir);
	sbi->s_private_dir = NULL;
	apfs_node_free(sbi->s_cat_root);
	sbi->s_cat_root = NULL;
	apfs_unset_omap(sb);
	apfs_unmap_volume_super(sb);
}

static struct kmem_cache *apfs_inode_cachep;

static struct inode *apfs_alloc_inode(struct super_block *sb)
{
	struct apfs_inode_info *ai;
	struct apfs_dstream_info *dstream;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0) || RHEL_VERSION_GE(9, 1)
	ai = alloc_inode_sb(sb, apfs_inode_cachep, GFP_KERNEL);
#else
	ai = kmem_cache_alloc(apfs_inode_cachep, GFP_KERNEL);
#endif
	if (!ai)
		return NULL;
	dstream = &ai->i_dstream;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
	inode_set_iversion(&ai->vfs_inode, 1);
#else
	ai->vfs_inode.i_version = 1;
#endif
	dstream->ds_sb = sb;
	dstream->ds_inode = &ai->vfs_inode;
	dstream->ds_cached_ext.len = 0;
	dstream->ds_ext_dirty = false;
	ai->i_nchildren = 0;
	INIT_LIST_HEAD(&ai->i_list);
	ai->i_cleaned = false;
	return &ai->vfs_inode;
}

static void apfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(apfs_inode_cachep, APFS_I(inode));
}

static void apfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, apfs_i_callback);
}

static void init_once(void *p)
{
	struct apfs_inode_info *ai = (struct apfs_inode_info *)p;
	struct apfs_dstream_info *dstream = &ai->i_dstream;

	spin_lock_init(&dstream->ds_ext_lock);
	inode_init_once(&ai->vfs_inode);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#define SLAB_MEM_SPREAD	0
#endif

static int __init init_inodecache(void)
{
	apfs_inode_cachep = kmem_cache_create("apfs_inode_cache",
					     sizeof(struct apfs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD|SLAB_ACCOUNT),
					     init_once);
	if (apfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static int apfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_nxsb_info *nxi = APFS_SB(sb)->s_nxi;
	int err;

	err = apfs_transaction_start(sb, APFS_TRANS_REG);
	if (err)
		return err;
	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto fail;
	/* Don't commit yet, or the inode will get flushed again and lock up */
	nxi->nx_transaction.t_state |= APFS_NX_TRANS_DEFER_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static void destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(apfs_inode_cachep);
}

static int apfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *msb_raw;
	struct apfs_superblock *vol;
	u64 fsid, free_blocks;
	int err;

	down_read(&nxi->nx_big_sem);
	msb_raw = nxi->nx_raw;
	vol = sbi->s_vsb_raw;

	buf->f_type = APFS_NX_MAGIC;
	/* Nodes are assumed to fit in a page, for now */
	buf->f_bsize = sb->s_blocksize;

	/* Volumes share the whole disk space */
	buf->f_blocks = le64_to_cpu(msb_raw->nx_block_count);
	/*
	 * It takes some work to retrieve the free block count because we
	 * can't assume that the spaceman has been read yet. It would be
	 * cleaner if we always did that on first mount (TODO).
	 */
	err = apfs_spaceman_get_free_blkcnt(sb, &free_blocks);
	if (err)
		goto fail;
	buf->f_bfree = free_blocks;
	buf->f_bavail = free_blocks;

	/* The file count is only for the mounted volume */
	buf->f_files = le64_to_cpu(vol->apfs_num_files) +
		       le64_to_cpu(vol->apfs_num_directories) +
		       le64_to_cpu(vol->apfs_num_symlinks) +
		       le64_to_cpu(vol->apfs_num_other_fsobjects);

	/*
	 * buf->f_ffree is left undefined for now. Maybe it should report the
	 * number of available cnids, like hfsplus attempts to do.
	 */

	buf->f_namelen = APFS_NAME_LEN;

	/* There are no clear rules for the fsid, so we follow ext2 here */
	fsid = le64_to_cpup((void *)vol->apfs_vol_uuid) ^
	       le64_to_cpup((void *)vol->apfs_vol_uuid + sizeof(u64));
	buf->f_fsid.val[0] = fsid & 0xFFFFFFFFUL;
	buf->f_fsid.val[1] = (fsid >> 32) & 0xFFFFFFFFUL;

fail:
	up_read(&nxi->nx_big_sem);
	return err;
}

static int apfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct apfs_sb_info *sbi = APFS_SB(root->d_sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(root->d_sb);

	if (sbi->s_vol_nr != 0)
		seq_printf(seq, ",vol=%u", sbi->s_vol_nr);
	if (sbi->s_snap_name)
		seq_printf(seq, ",snap=%s", sbi->s_snap_name);
	if (uid_valid(sbi->s_uid))
		seq_printf(seq, ",uid=%u", from_kuid(&init_user_ns,
						     sbi->s_uid));
	if (gid_valid(sbi->s_gid))
		seq_printf(seq, ",gid=%u", from_kgid(&init_user_ns,
						     sbi->s_gid));
	if (nxi->nx_flags & APFS_CHECK_NODES)
		seq_puts(seq, ",cknodes");
	if (nxi->nx_tier2_info)
		seq_printf(seq, ",tier2=%s", nxi->nx_tier2_info->blki_path);

	return 0;
}

int apfs_sync_fs(struct super_block *sb, int wait)
{
	int err;

	/* TODO: actually start the commit and return without waiting? */
	if (wait == 0)
		return 0;

	err = apfs_transaction_start(sb, APFS_TRANS_SYNC);
	if (err)
		return err;
	APFS_SB(sb)->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		apfs_transaction_abort(sb);
	return err;
}

/* Only supports read-only remounts, everything else is silently ignored */
static int apfs_remount(struct super_block *sb, int *flags, char *data)
{
	int err = 0;

	err = sync_filesystem(sb);
	if (err)
		return err;

	/* TODO: race? Could a new transaction have started already? */
	if (*flags & SB_RDONLY)
		sb->s_flags |= SB_RDONLY;

	/*
	 * TODO: readwrite remounts seem simple enough, but I worry about
	 * remounting aborted transactions. I would probably also need a
	 * dry-run version of parse_options().
	 */
	apfs_notice(sb, "all remounts can do is turn a volume read-only");
	return 0;
}

static const struct super_operations apfs_sops = {
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_destroy_inode,
	.write_inode	= apfs_write_inode,
	.evict_inode	= apfs_evict_inode,
	.put_super	= apfs_put_super,
	.sync_fs	= apfs_sync_fs,
	.statfs		= apfs_statfs,
	.remount_fs	= apfs_remount,
	.show_options	= apfs_show_options,
};

enum {
	Opt_readwrite, Opt_cknodes, Opt_uid, Opt_gid, Opt_vol, Opt_snap, Opt_tier2, Opt_err,
};

static const match_table_t tokens = {
	{Opt_readwrite, "readwrite"},
	{Opt_cknodes, "cknodes"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_vol, "vol=%u"},
	{Opt_snap, "snap=%s"},
	{Opt_tier2, "tier2=%s"},
	{Opt_err, NULL}
};

/**
 * apfs_set_nx_flags - Set the mount flags for the container, if allowed
 * @sb:		superblock structure
 * @flags:	flags to set
 */
static void apfs_set_nx_flags(struct super_block *sb, unsigned int flags)
{
	struct apfs_nxsb_info *nxi = APFS_SB(sb)->s_nxi;

	mutex_lock(&nxs_mutex);

	/* The first mount thet gets here decides the flags for its container */
	flags |= APFS_FLAGS_SET;
	if (!(nxi->nx_flags & APFS_FLAGS_SET))
		nxi->nx_flags = flags;
	else if (flags != nxi->nx_flags)
		apfs_warn(sb, "ignoring mount flags - container already mounted");

	mutex_unlock(&nxs_mutex);
}

/*
 * Many of the parse_options() functions in other file systems return 0
 * on error. This one returns an error code, and 0 on success.
 */
static int parse_options(struct super_block *sb, char *options)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	int err = 0;
	unsigned int nx_flags;

	/* Set default values before parsing */
	nx_flags = 0;

#ifdef CONFIG_APFS_RW_ALWAYS
	/* Still risky, but some packagers want writable mounts by default */
	nx_flags |= APFS_READWRITE;
#endif

	if (!options)
		goto out;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_readwrite:
			/*
			 * Write support is not safe yet, so keep it disabled
			 * unless the user requests it explicitly.
			 */
			nx_flags |= APFS_READWRITE;
			break;
		case Opt_cknodes:
			/*
			 * Right now, node checksums are too costly to enable
			 * by default.  TODO: try to improve this.
			 */
			nx_flags |= APFS_CHECK_NODES;
			break;
		case Opt_uid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(sbi->s_uid)) {
				apfs_err(sb, "invalid uid");
				return -EINVAL;
			}
			break;
		case Opt_gid:
			err = match_int(&args[0], &option);
			if (err)
				return err;
			sbi->s_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(sbi->s_gid)) {
				apfs_err(sb, "invalid gid");
				return -EINVAL;
			}
			break;
		case Opt_vol:
		case Opt_snap:
		case Opt_tier2:
			/* Already read early on mount */
			break;
		default:
			/*
			 * We should have already checked the mount options in
			 * apfs_preparse_options(), so this is a bug.
			 */
			apfs_alert(sb, "invalid mount option %s", p);
			return -EINVAL;
		}
	}

out:
	apfs_set_nx_flags(sb, nx_flags);
	if (!(sb->s_flags & SB_RDONLY)) {
		if (nxi->nx_flags & APFS_READWRITE) {
			apfs_notice(sb, "experimental write support is enabled");
		} else {
			apfs_warn(sb, "experimental writes disabled to avoid data loss");
			apfs_warn(sb, "if you really want them, check the README");
			sb->s_flags |= SB_RDONLY;
		}
	}
	return 0;
}

/**
 * apfs_check_nx_features - Check for unsupported features in the container
 * @sb: superblock structure
 *
 * Returns -EINVAL if unsupported incompatible features are found, otherwise
 * returns 0.
 */
static int apfs_check_nx_features(struct super_block *sb)
{
	struct apfs_nx_superblock *msb_raw = NULL;
	u64 features;
	bool fusion;

	msb_raw = APFS_NXI(sb)->nx_raw;
	if (!msb_raw) {
		apfs_alert(sb, "feature checks are misplaced");
		return -EINVAL;
	}

	features = le64_to_cpu(msb_raw->nx_incompatible_features);
	if (features & ~APFS_NX_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb, "unknown incompatible container features (0x%llx)", features);
		return -EINVAL;
	}

	fusion = features & APFS_NX_INCOMPAT_FUSION;
	if (fusion && !APFS_NXI(sb)->nx_tier2_info) {
		apfs_warn(sb, "fusion drive - please use the \"tier2\" mount option");
		return -EINVAL;
	}
	if (!fusion && APFS_NXI(sb)->nx_tier2_info) {
		apfs_warn(sb, "not a fusion drive - what's the second disk for?");
		return -EINVAL;
	}
	if (fusion) {
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "writes to fusion drives not yet supported");
			sb->s_flags |= SB_RDONLY;
		}
	}

	features = le64_to_cpu(msb_raw->nx_readonly_compatible_features);
	if (features & ~APFS_NX_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb, "unknown read-only compatible container features (0x%llx)", features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "container can't be mounted read-write");
			return -EINVAL;
		}
	}
	return 0;
}

/**
 * apfs_check_vol_features - Check for unsupported features in the volume
 * @sb: superblock structure
 *
 * Returns -EINVAL if unsupported incompatible features are found, otherwise
 * returns 0.
 */
static int apfs_check_vol_features(struct super_block *sb)
{
	struct apfs_superblock *vsb_raw = NULL;
	u64 features;

	vsb_raw = APFS_SB(sb)->s_vsb_raw;
	if (!vsb_raw) {
		apfs_alert(sb, "feature checks are misplaced");
		return -EINVAL;
	}

	features = le64_to_cpu(vsb_raw->apfs_incompatible_features);
	if (features & ~APFS_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb, "unknown incompatible volume features (0x%llx)", features);
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_DATALESS_SNAPS) {
		/*
		 * I haven't encountered dataless snapshots myself yet (TODO).
		 * I'm not even sure what they are, so be safe.
		 */
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "writes to volumes with dataless snapshots not yet supported");
			return -EINVAL;
		}
		apfs_warn(sb, "volume has dataless snapshots");
	}
	if (features & APFS_INCOMPAT_ENC_ROLLED) {
		apfs_warn(sb, "encrypted volumes are not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_INCOMPLETE_RESTORE) {
		apfs_warn(sb, "incomplete restore is not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_PFK) {
		apfs_warn(sb, "PFK is not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_SECONDARY_FSROOT) {
		apfs_warn(sb, "secondary fsroot is not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_SEALED_VOLUME) {
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "writes to sealed volumes are not yet supported");
			return -EINVAL;
		}
		apfs_info(sb, "volume is sealed");
	}
	/*
	 * As far as I can see, all this feature seems to do is define a new
	 * flag (which I call APFS_FILE_EXTENT_PREALLOCATED) for extents that
	 * are fully after the end of their file. I don't get why this change
	 * is incompatible instead of read-only compatible, so I fear I might
	 * be missing something. I will never be certain though, so for now
	 * allow the mount and hope for the best.
	 */
	if (features & APFS_INCOMPAT_EXTENT_PREALLOC_FLAG)
		apfs_warn(sb, "extent prealloc flag is set");

	features = le64_to_cpu(vsb_raw->apfs_fs_flags);
	/* Some encrypted volumes are readable anyway */
	if (!(features & APFS_FS_UNENCRYPTED))
		apfs_warn(sb, "volume is encrypted, may not be read correctly");

	features = le64_to_cpu(vsb_raw->apfs_readonly_compatible_features);
	if (features & ~APFS_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb, "unknown read-only compatible volume features (0x%llx)", features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "volume can't be mounted read-write");
			return -EINVAL;
		}
	}
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)

/**
 * apfs_setup_bdi - Set up the bdi for the superblock
 * @sb: superblock structure
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_setup_bdi(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_blkdev_info *bd_info = NULL;
	struct backing_dev_info *bdi_dev = NULL, *bdi_sb = NULL;
	int err;

	bd_info = nxi->nx_blkdev_info;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) || RHEL_VERSION_GE(9, 0)
	bdi_dev = bd_info->blki_bdev->bd_disk->bdi;
#else
	bdi_dev = bd_info->blki_bdev->bd_bdi;
#endif

	err = super_setup_bdi(sb);
	if (err)
		return err;
	bdi_sb = sb->s_bdi;

	bdi_sb->ra_pages = bdi_dev->ra_pages;
	bdi_sb->io_pages = bdi_dev->io_pages;

	bdi_sb->capabilities = bdi_dev->capabilities;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	bdi_sb->capabilities &= ~BDI_CAP_WRITEBACK;
#else
	bdi_sb->capabilities |= BDI_CAP_NO_WRITEBACK | BDI_CAP_NO_ACCT_DIRTY;
#endif

	return 0;
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0) */

/* This is needed for readahead, so old kernels will be slower */
static int apfs_setup_bdi(struct super_block *sb)
{
	return 0;
}

#endif

static void apfs_set_trans_buffer_limit(struct super_block *sb)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	unsigned long memsize_in_blocks;
	struct sysinfo info = {0};

	si_meminfo(&info);
	memsize_in_blocks = info.totalram << (PAGE_SHIFT - sb->s_blocksize_bits);

	/*
	 * Buffer heads are not reclaimed while they are part of the current
	 * transaction, so systems with little memory will crash if we don't
	 * commit often enough. This hack should make that happen in general,
	 * but I still need to get the reclaim to work eventually (TODO).
	 */
	if (memsize_in_blocks >= 16 * APFS_TRANS_BUFFERS_MAX)
		nxi->nx_trans_buffers_max = APFS_TRANS_BUFFERS_MAX;
	else
		nxi->nx_trans_buffers_max = memsize_in_blocks / 16;
}

static int apfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct inode *root = NULL, *priv = NULL;
	int err;

	/*
	 * This function doesn't write anything to disk, that happens later
	 * when an actual transaction begins. So, it's not generally a problem
	 * if other mounts for the same container fill their own supers at the
	 * same time (the few critical sections will be protected by
	 * nxs_mutex), nor is it a problem if other mounted volumes want to
	 * make reads while the mount is taking place. But we definitely don't
	 * want any writes, or else we could find ourselves reading stale
	 * blocks after CoW, among other issues.
	 */
	down_read(&APFS_NXI(sb)->nx_big_sem);

	err = apfs_setup_bdi(sb);
	if (err)
		goto failed_volume;

	sbi->s_uid = INVALID_UID;
	sbi->s_gid = INVALID_GID;
	err = parse_options(sb, data);
	if (err)
		goto failed_volume;

	err = apfs_map_volume_super(sb, false /* write */);
	if (err)
		goto failed_volume;

	err = apfs_check_vol_features(sb);
	if (err)
		goto failed_omap;

	/*
	 * The omap needs to be set before the call to apfs_read_catalog().
	 * It's also shared with all the snapshots, so it needs to be read
	 * before we switch to the old superblock.
	 */
	err = apfs_first_read_omap(sb);
	if (err)
		goto failed_omap;

	if (sbi->s_snap_name) {
		err = apfs_switch_to_snapshot(sb);
		if (err)
			goto failed_cat;
	}

	err = apfs_read_catalog(sb, false /* write */);
	if (err)
		goto failed_cat;

	sb->s_op = &apfs_sops;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	sb->__s_d_op = &apfs_dentry_operations;
#else
	sb->s_d_op = &apfs_dentry_operations;
#endif

	sb->s_xattr = apfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1; /* Nanosecond granularity */

	/*
	 * At this point everything is already set up for the inode reads,
	 * which take care of their own locking as always.
	 */
	up_read(&APFS_NXI(sb)->nx_big_sem);

	sbi->s_private_dir = apfs_iget(sb, APFS_PRIV_DIR_INO_NUM);
	if (IS_ERR(sbi->s_private_dir)) {
		apfs_err(sb, "unable to get private-dir inode");
		err = PTR_ERR(sbi->s_private_dir);
		goto failed_private_dir;
	}

	root = apfs_iget(sb, APFS_ROOT_DIR_INO_NUM);
	if (IS_ERR(root)) {
		apfs_err(sb, "unable to get root inode");
		err = PTR_ERR(root);
		goto failed_mount;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		apfs_err(sb, "unable to get root dentry");
		err = -ENOMEM;
		goto failed_mount;
	}

	INIT_WORK(&sbi->s_orphan_cleanup_work, apfs_orphan_cleanup_work);
	if (!(sb->s_flags & SB_RDONLY)) {
		priv = sbi->s_private_dir;
		if (APFS_I(priv)->i_nchildren)
			schedule_work(&sbi->s_orphan_cleanup_work);
	}
	return 0;

failed_mount:
	iput(sbi->s_private_dir);
failed_private_dir:
	sbi->s_private_dir = NULL;
	down_read(&APFS_NXI(sb)->nx_big_sem);
	apfs_node_free(sbi->s_cat_root);
failed_cat:
	apfs_unset_omap(sb);
failed_omap:
	apfs_unmap_volume_super(sb);
failed_volume:
	up_read(&APFS_NXI(sb)->nx_big_sem);
	return err;
}

/**
 * apfs_strings_are_equal - Compare two possible NULL strings
 * @str1: the first string
 * @str2: the second string
 */
static bool apfs_strings_are_equal(const char *str1, const char *str2)
{
	if (str1 == str2) /* Both are NULL */
		return true;
	if (!str1 || !str2) /* One is NULL */
		return false;
	return strcmp(str1, str2) == 0;
}

/**
 * apfs_test_super - Check if two volume superblocks are for the same volume
 * @sb:		superblock structure for a currently mounted volume
 * @data:	superblock info for the volume being mounted
 */
static int apfs_test_super(struct super_block *sb, void *data)
{
	struct apfs_sb_info *sbi_1 = data;
	struct apfs_sb_info *sbi_2 = APFS_SB(sb);

	if (sbi_1->s_nxi != sbi_2->s_nxi)
		return false;
	if (sbi_1->s_vol_nr != sbi_2->s_vol_nr)
		return false;
	return apfs_strings_are_equal(sbi_1->s_snap_name, sbi_2->s_snap_name);
}

/**
 * apfs_set_super - Assign the device and an info struct to a superblock
 * @sb:		superblock structure to set
 * @data:	superblock info for the volume being mounted
 */
static int apfs_set_super(struct super_block *sb, void *data)
{
	struct apfs_sb_info *sbi = data;
	struct apfs_nxsb_info *nxi = sbi->s_nxi;
	int err;

	/*
	 * This fake device number will be unique to this volume-snapshot
	 * combination. It gets reported by stat(), so that userland tools can
	 * use it to tell different mountpoints apart.
	 */
	err = get_anon_bdev(&sbi->s_anon_dev);
	if (err)
		return err;

	/*
	 * This is the actual device number, shared by all volumes and
	 * snapshots. It gets reported by the mountinfo file, and it seems that
	 * udisks uses it to decide if a device is mounted, so it must be set.
	 *
	 * TODO: does this work for fusion drives?
	 */
	sb->s_dev = nxi->nx_blkdev_info->blki_bdev->bd_dev;

	sb->s_fs_info = sbi;
	return 0;
}

/*
 * Wrapper for lookup_bdev() that supports older kernels.
 */
static int apfs_lookup_bdev(const char *pathname, dev_t *dev)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	struct block_device *bdev;

	bdev = lookup_bdev(pathname);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	*dev = bdev->bd_dev;
	bdput(bdev);
	return 0;
#else
	return lookup_bdev(pathname, dev);
#endif
}

/**
 * apfs_blkdev_setup - Open a block device and set its info struct
 * @info_p:	info struct to set
 * @dev_name:	path name for the block device to open
 * @mode:	FMODE_* mask
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
static int apfs_blkdev_setup(struct apfs_blkdev_info **info_p, const char *dev_name, blk_mode_t mode)
#else
static int apfs_blkdev_setup(struct apfs_blkdev_info **info_p, const char *dev_name, fmode_t mode)
#endif
{
	struct apfs_blkdev_info *info = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0) || RHEL_VERSION_GE(9, 5)
	struct file *file = NULL;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	struct bdev_handle *handle = NULL;
#endif
	struct block_device *bdev = NULL;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->blki_path = kstrdup(dev_name, GFP_KERNEL);
	if (!info->blki_path) {
		ret = -ENOMEM;
		goto fail;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0) || RHEL_VERSION_GE(9, 5)
	file = bdev_file_open_by_path(dev_name, mode, &apfs_fs_type, NULL);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto fail;
	}
	info->blki_bdev_file = file;
	bdev = file_bdev(file);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	handle = bdev_open_by_path(dev_name, mode, &apfs_fs_type, NULL);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto fail;
	}
	info->blki_bdev_handle = handle;
	bdev = handle->bdev;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0) || RHEL_VERSION_GE(9, 4)
	bdev = blkdev_get_by_path(dev_name, mode, &apfs_fs_type, NULL);
#else
	bdev = blkdev_get_by_path(dev_name, mode, &apfs_fs_type);
#endif

	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		goto fail;
	}
	info->blki_bdev = bdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0) && !RHEL_VERSION_GE(9, 4)
	info->blki_mode = mode;
#endif
	*info_p = info;
	return 0;

fail:
	kfree(info->blki_path);
	info->blki_path = NULL;
	kfree(info);
	info = NULL;
	return ret;
}

/**
 * apfs_attach_nxi - Attach container sb info to a volume's sb info
 * @sbi:	new superblock info structure for the volume to be mounted
 * @dev_name:	path name for the container's block device
 * @mode:	FMODE_* mask
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
static int apfs_attach_nxi(struct apfs_sb_info *sbi, const char *dev_name, blk_mode_t mode)
#else
static int apfs_attach_nxi(struct apfs_sb_info *sbi, const char *dev_name, fmode_t mode)
#endif
{
	struct apfs_nxsb_info *nxi = NULL;
	dev_t dev = 0;
	int ret;

	mutex_lock(&nxs_mutex);

	ret = apfs_lookup_bdev(dev_name, &dev);
	if (ret)
		goto out;

	nxi = apfs_nx_find_by_dev(dev);
	if (!nxi) {
		nxi = kzalloc(sizeof(*nxi), GFP_KERNEL);
		if (!nxi) {
			ret = -ENOMEM;
			goto out;
		}

		ret = apfs_blkdev_setup(&nxi->nx_blkdev_info, dev_name, mode);
		if (ret)
			goto out;

		if (sbi->s_tier2_path) {
			ret = apfs_blkdev_setup(&nxi->nx_tier2_info, sbi->s_tier2_path, mode);
			if (ret) {
				apfs_blkdev_cleanup(nxi->nx_blkdev_info);
				nxi->nx_blkdev_info = NULL;
				goto out;
			}
			/* We won't need this anymore, so why waste memory? */
			kfree(sbi->s_tier2_path);
			sbi->s_tier2_path = NULL;
		}

		init_rwsem(&nxi->nx_big_sem);
		list_add(&nxi->nx_list, &nxs);
		INIT_LIST_HEAD(&nxi->vol_list);
		apfs_transaction_init(&nxi->nx_transaction);
	}

	list_add(&sbi->list, &nxi->vol_list);
	sbi->s_nxi = nxi;
	++nxi->nx_refcnt;
	ret = 0;
out:
	if (ret) {
		kfree(nxi);
		nxi = NULL;
	}
	mutex_unlock(&nxs_mutex);
	return ret;
}

/**
 * apfs_preparse_options - Parse the options used to identify a superblock
 * @sbi:	superblock info
 * @options:	options string to parse
 *
 * Returns 0 on success, or a negative error code in case of failure. Even on
 * failure, the caller is responsible for freeing all superblock fields.
 */
static int apfs_preparse_options(struct apfs_sb_info *sbi, char *options)
{
	char *tofree = NULL;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int err = 0;

	/* Set default values before parsing */
	sbi->s_vol_nr = 0;
	sbi->s_snap_name = NULL;
	sbi->s_tier2_path = NULL;

	if (!options)
		return 0;

	/* Later parse_options() will need the unmodified options string */
	options = kstrdup(options, GFP_KERNEL);
	if (!options)
		return -ENOMEM;
	tofree = options;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_vol:
			err = match_int(&args[0], &sbi->s_vol_nr);
			if (err)
				goto out;
			break;
		case Opt_snap:
			kfree(sbi->s_snap_name);
			sbi->s_snap_name = match_strdup(&args[0]);
			if (!sbi->s_snap_name) {
				err = -ENOMEM;
				goto out;
			}
			break;
		case Opt_tier2:
			kfree(sbi->s_tier2_path);
			sbi->s_tier2_path = match_strdup(&args[0]);
			if (!sbi->s_tier2_path) {
				err = -ENOMEM;
				goto out;
			}
			break;
		case Opt_readwrite:
		case Opt_cknodes:
		case Opt_uid:
		case Opt_gid:
			/* Not needed for sget(), will be read later */
			break;
		default:
			apfs_warn(NULL, "invalid mount option %s", p);
			err = -EINVAL;
			goto out;
		}
	}
	err = 0;
out:
	kfree(tofree);
	return err;
}

/*
 * This function is a copy of mount_bdev() that allows multiple mounts.
 */
static struct dentry *apfs_mount(struct file_system_type *fs_type, int flags,
				 const char *dev_name, void *data)
{
	struct super_block *sb;
	struct apfs_sb_info *sbi;
	struct apfs_blkdev_info *bd_info = NULL, *tier2_info = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0) || RHEL_VERSION_GE(9, 4)
	blk_mode_t mode = sb_open_mode(flags);
#else
	fmode_t mode = FMODE_READ | FMODE_EXCL;
#endif
	int error = 0;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return ERR_PTR(-ENOMEM);
	/* Set up the fields that sget() will need to id the superblock */
	error = apfs_preparse_options(sbi, data);
	if (error)
		goto out_free_sbi;

	/* Make sure that snapshots are mounted read-only */
	if (sbi->s_snap_name)
		flags |= SB_RDONLY;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0) && !RHEL_VERSION_GE(9, 4)
	if (!(flags & SB_RDONLY))
		mode |= FMODE_WRITE;
#endif

	error = apfs_attach_nxi(sbi, dev_name, mode);
	if (error)
		goto out_free_sbi;

	/* TODO: lockfs stuff? Btrfs doesn't seem to care */
	sb = sget(fs_type, apfs_test_super, apfs_set_super, flags | SB_NOSEC, sbi);
	if (IS_ERR(sb)) {
		error = PTR_ERR(sb);
		goto out_unmap_super;
	}

	bd_info = APFS_NXI(sb)->nx_blkdev_info;
	tier2_info = APFS_NXI(sb)->nx_tier2_info;

	/*
	 * I'm doing something hacky with s_dev inside ->kill_sb(), so I want
	 * to find out as soon as possible if I messed it up.
	 */
	WARN_ON(sb->s_dev != bd_info->blki_bdev->bd_dev);

	if (sb->s_root) {
		if ((flags ^ sb->s_flags) & SB_RDONLY) {
			error = -EBUSY;
			deactivate_locked_super(sb);
			goto out_unmap_super;
		}
		/* Only one superblock per volume */
		apfs_free_main_super(sbi);
		kfree(sbi->s_snap_name);
		sbi->s_snap_name = NULL;
		kfree(sbi->s_tier2_path);
		sbi->s_tier2_path = NULL;
		kfree(sbi);
		sbi = NULL;
	} else {
		if (!sbi->s_snap_name && !tier2_info)
			snprintf(sb->s_id, sizeof(sb->s_id), "%pg:%u", bd_info->blki_bdev, sbi->s_vol_nr);
		else if (!tier2_info)
			snprintf(sb->s_id, sizeof(sb->s_id), "%pg:%u:%s", bd_info->blki_bdev, sbi->s_vol_nr, sbi->s_snap_name);
		else if (!sbi->s_snap_name)
			snprintf(sb->s_id, sizeof(sb->s_id), "%pg+%pg:%u", bd_info->blki_bdev, tier2_info->blki_bdev, sbi->s_vol_nr);
		else
			snprintf(sb->s_id, sizeof(sb->s_id), "%pg+%pg:%u:%s", bd_info->blki_bdev, tier2_info->blki_bdev, sbi->s_vol_nr, sbi->s_snap_name);
		error = apfs_read_main_super(sb);
		if (error) {
			deactivate_locked_super(sb);
			return ERR_PTR(error);
		}
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0) && !RHEL_VERSION_GE(9, 4)
		sb->s_mode = mode;
#endif
		error = apfs_fill_super(sb, data, flags & SB_SILENT ? 1 : 0);
		if (error) {
			deactivate_locked_super(sb);
			return ERR_PTR(error);
		}
		sb->s_flags |= SB_ACTIVE;
	}

	return dget(sb->s_root);

out_unmap_super:
	apfs_free_main_super(sbi);
out_free_sbi:
	kfree(sbi->s_snap_name);
	kfree(sbi->s_tier2_path);
	kfree(sbi);
	return ERR_PTR(error);
}

/**
 * apfs_free_sb_info - Free the sb info and release all remaining fields
 * @sb: superblock structure
 *
 * This function does not include the cleanup for apfs_fill_super(), which
 * already took place inside ->put_super() (or maybe inside apfs_fill_super()
 * itself if the mount failed).
 */
static void apfs_free_sb_info(struct super_block *sb)
{
	struct apfs_sb_info *sbi = NULL;

	sbi = APFS_SB(sb);
	apfs_free_main_super(sbi);
	sb->s_fs_info = NULL;

	kfree(sbi->s_snap_name);
	sbi->s_snap_name = NULL;
	kfree(sbi->s_tier2_path);
	sbi->s_tier2_path = NULL;
	if (sbi->s_dflt_pfk)
		kfree(sbi->s_dflt_pfk);
	kfree(sbi);
	sbi = NULL;
}

static void apfs_kill_sb(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	/*
	 * We need to delist the superblock before freeing its info to avoid a
	 * race with apfs_test_super(), but we can't call kill_super_notify()
	 * from the driver. The available wrapper is kill_anon_super(), but our
	 * s_dev is set to the actual device (that gets freed later along with
	 * the container), not to the anon device that we keep on the sbi. So,
	 * we change that before the call; this is safe because other mounters
	 * won't revive this super, even if apfs_test_super() succeeds.
	 */
	sb->s_dev = sbi->s_anon_dev;
	kill_anon_super(sb);

	apfs_free_sb_info(sb);
}

static struct file_system_type apfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "apfs",
	.mount		= apfs_mount,
	.kill_sb	= apfs_kill_sb,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("apfs");

static int __init init_apfs_fs(void)
{
	int err = 0;

	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&apfs_fs_type);
	if (err)
		destroy_inodecache();
	return err;
}

static void __exit exit_apfs_fs(void)
{
	unregister_filesystem(&apfs_fs_type);
	destroy_inodecache();
}

MODULE_AUTHOR("Ernesto A. Fernández");
MODULE_DESCRIPTION("Apple File System");
MODULE_VERSION(GIT_COMMIT);
MODULE_LICENSE("GPL");
module_init(init_apfs_fs)
module_exit(exit_apfs_fs)
