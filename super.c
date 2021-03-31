// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
#include <linux/iversion.h>
#endif

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
	if (!sb_set_blocksize(sb, APFS_NX_DEFAULT_BLOCK_SIZE)) {
		apfs_err(sb, "unable to set blocksize");
		return ERR_PTR(err);
	}
	bh = sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "unable to read superblock");
		return ERR_PTR(err);
	}
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	blocksize = le32_to_cpu(msb_raw->nx_block_size);

	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, blocksize)) {
			apfs_err(sb, "bad blocksize %d", blocksize);
			return ERR_PTR(err);
		}
		bh = sb_bread(sb, APFS_NX_BLOCK_NUM);
		if (!bh) {
			apfs_err(sb, "unable to read superblock 2nd time");
			return ERR_PTR(err);
		}
		msb_raw = (struct apfs_nx_superblock *)bh->b_data;
	}

	sb->s_magic = le32_to_cpu(msb_raw->nx_magic);
	if (sb->s_magic != APFS_NX_MAGIC) {
		apfs_err(sb, "not an apfs filesystem");
		goto fail;
	}
	if (!apfs_obj_verify_csum(sb, &msb_raw->nx_o)) {
		apfs_err(sb, "inconsistent container superblock");
		err = -EFSBADCRC;
		goto fail;
	}
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
	struct buffer_head *bh;

	if (sb->s_flags & SB_RDONLY)
		return;

	bh = sb_bread(sb, APFS_NX_BLOCK_NUM);
	if (!bh) {
		apfs_err(sb, "failed to write block zero");
		return;
	}
	memcpy(bh->b_data, sbi->s_msb_raw, sb->s_blocksize);
	mark_buffer_dirty(bh);
	brelse(bh);
}

/**
 * apfs_map_main_super - Find the container superblock and map it into memory
 * @sb:	superblock structure
 *
 * Returns a negative error code in case of failure.  On success, returns 0
 * and sets the s_msb_raw, s_mobject and s_xid fields of APFS_SB(@sb).
 */
static int apfs_map_main_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct buffer_head *bh;
	struct buffer_head *desc_bh = NULL;
	struct apfs_nx_superblock *msb_raw;
	u64 xid, bno = APFS_NX_BLOCK_NUM;
	u64 desc_base;
	u32 desc_blocks;
	int err = -EINVAL;
	int i;

	/* Read the superblock from the last clean unmount */
	bh = apfs_read_super_copy(sb);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	msb_raw = (struct apfs_nx_superblock *)bh->b_data;

	/* We want to mount the latest valid checkpoint among the descriptors */
	desc_base = le64_to_cpu(msb_raw->nx_xp_desc_base);
	if (desc_base >> 63 != 0) {
		/* The highest bit is set when checkpoints are not contiguous */
		apfs_err(sb, "checkpoint descriptor tree not yet supported");
		goto fail;
	}
	desc_blocks = le32_to_cpu(msb_raw->nx_xp_desc_blocks);
	if (desc_blocks > 10000) { /* Arbitrary loop limit, is it enough? */
		apfs_err(sb, "too many checkpoint descriptors?");
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Now we go through the checkpoints one by one */
	xid = le64_to_cpu(msb_raw->nx_o.o_xid);
	for (i = 0; i < desc_blocks; ++i) {
		struct apfs_nx_superblock *desc_raw;

		brelse(desc_bh);
		desc_bh = sb_bread(sb, desc_base + i);
		if (!desc_bh) {
			apfs_err(sb, "unable to read checkpoint descriptor");
			goto fail;
		}
		desc_raw = (struct apfs_nx_superblock *)desc_bh->b_data;

		if (le32_to_cpu(desc_raw->nx_magic) != APFS_NX_MAGIC)
			continue; /* Not a superblock */
		if (le64_to_cpu(desc_raw->nx_o.o_xid) <= xid)
			continue; /* Old */
		if (!apfs_obj_verify_csum(sb, &desc_raw->nx_o))
			continue; /* Corrupted */

		xid = le64_to_cpu(desc_raw->nx_o.o_xid);
		msb_raw = desc_raw;
		bno = desc_base + i;
		brelse(bh);
		bh = desc_bh;
		desc_bh = NULL;
	}

	sbi->s_xid = xid;
	sbi->s_msb_raw = msb_raw;
	sbi->s_mobject.sb = sb;
	sbi->s_mobject.block_nr = bno;
	sbi->s_mobject.oid = le64_to_cpu(msb_raw->nx_o.o_oid);
	sbi->s_mobject.bh = bh;
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_update_software_info - Write the module info to a modified volume
 * @sb: superblock structure
 *
 * Does nothing if the module information is already present at index zero of
 * the apfs_modified_by array.  Otherwise, writes it there after shifting the
 * rest of the entries to the right.
 */
static void apfs_update_software_info(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *raw = sbi->s_vsb_raw;
	struct apfs_modified_by *mod_by;

	ASSERT(sbi->s_vsb_raw);
	ASSERT(sbi->s_xid == le64_to_cpu(raw->apfs_o.o_xid));
	ASSERT(strlen(APFS_MODULE_ID_STRING) < APFS_MODIFIED_NAMELEN);
	mod_by = raw->apfs_modified_by;

	/* This check could be optimized away, but does it matter? */
	if (!strcmp(mod_by->id, APFS_MODULE_ID_STRING))
		return;
	memmove(mod_by + 1, mod_by, (APFS_MAX_HIST - 1) * sizeof(*mod_by));

	memset(mod_by->id, 0, sizeof(mod_by->id));
	strcpy(mod_by->id, APFS_MODULE_ID_STRING);
	mod_by->timestamp = cpu_to_le64(ktime_get_real_ns());
	mod_by->last_xid = cpu_to_le64(sbi->s_xid);
}

/**
 * apfs_unmap_main_super - Clean up apfs_map_main_super()
 * @sb:	filesystem superblock
 */
static inline void apfs_unmap_main_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	brelse(sbi->s_mobject.bh);
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
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_superblock *vsb_raw;
	struct apfs_omap_phys *msb_omap_raw;
	struct apfs_node *vnode;
	struct buffer_head *bh;
	struct apfs_transaction *trans = &sbi->s_transaction;
	u64 vol_id;
	u64 vsb;
	int err;

	ASSERT(sbi->s_msb_raw);
	ASSERT(trans->t_old_vsb == sbi->s_vobject.bh);

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
				    write);
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

	err = apfs_omap_lookup_block(sb, vnode, vol_id, &vsb, write);
	apfs_node_put(vnode);
	if (err) {
		apfs_err(sb, "volume not found, likely corruption");
		return err;
	}

	bh = sb_bread(sb, vsb);
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
	if (!write && !apfs_obj_verify_csum(sb, &vsb_raw->apfs_o)) {
		apfs_err(sb, "inconsistent volume superblock");
		err = -EFSBADCRC;
		goto fail;
	}

	sbi->s_vsb_raw = vsb_raw;
	sbi->s_vobject.sb = sb;
	sbi->s_vobject.block_nr = vsb;
	sbi->s_vobject.oid = le64_to_cpu(vsb_raw->apfs_o.o_oid);
	brelse(sbi->s_vobject.bh);
	sbi->s_vobject.bh = bh;

	if (write)
		apfs_update_software_info(sb);
	return 0;

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_unmap_volume_super - Clean up apfs_map_volume_super()
 * @sb:	filesystem superblock
 */
static inline void apfs_unmap_volume_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	brelse(sbi->s_vobject.bh);
}

/**
 * apfs_read_omap - Find and read the omap root node
 * @sb:		superblock structure
 * @write:	request write access?
 *
 * On success, returns 0 and sets APFS_SB(@sb)->s_omap_root; on failure returns
 * a negative error code.
 */
int apfs_read_omap(struct super_block *sb, bool write)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_omap_phys *omap_raw;
	struct apfs_node *omap_root;
	struct buffer_head *bh;
	u64 omap_blk;
	int err;

	ASSERT(sbi->s_vsb_raw);

	/* Get the block holding the volume omap information */
	omap_blk = le64_to_cpu(vsb_raw->apfs_omap_oid);
	bh = apfs_read_object_block(sb, omap_blk, write);
	if (IS_ERR(bh)) {
		apfs_err(sb, "unable to read the volume object map");
		return PTR_ERR(bh);
	}
	if (write) {
		ASSERT(sbi->s_xid == le64_to_cpu(vsb_raw->apfs_o.o_xid));
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
		ASSERT(sbi->s_xid == le64_to_cpu(omap_raw->om_o.o_xid));
		ASSERT(buffer_trans(bh));
		omap_raw->om_tree_oid = cpu_to_le64(omap_root->object.block_nr);
	}
	omap_raw = NULL;
	brelse(bh);

	if (sbi->s_omap_root)
		apfs_node_put(sbi->s_omap_root);
	sbi->s_omap_root = omap_root;
	return 0;

fail:
	brelse(bh);
	return err;
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

	ASSERT(sbi->s_omap_root);

	root_node = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_root_tree_oid),
				   APFS_OBJ_VIRTUAL, write);
	if (IS_ERR(root_node)) {
		apfs_err(sb, "unable to read catalog root node");
		return PTR_ERR(root_node);
	}

	if (sbi->s_cat_root)
		apfs_node_put(sbi->s_cat_root);
	sbi->s_cat_root = root_node;
	return 0;
}

static void apfs_put_super(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	iput(sbi->s_private_dir);
	sbi->s_private_dir = NULL;

	/* Update the volume's unmount time */
	if (!(sb->s_flags & SB_RDONLY)) {
		struct apfs_superblock *vsb_raw;
		struct buffer_head *vsb_bh;

		if (apfs_transaction_start(sb))
			goto fail;
		vsb_raw = sbi->s_vsb_raw;
		vsb_bh = sbi->s_vobject.bh;

		ASSERT(sbi->s_xid == le64_to_cpu(vsb_raw->apfs_o.o_xid));
		ASSERT(buffer_trans(vsb_bh));

		vsb_raw->apfs_unmount_time = cpu_to_le64(ktime_get_real_ns());
		set_buffer_csum(vsb_bh);

		if (apfs_transaction_commit(sb)) {
			apfs_transaction_abort(sb);
			goto fail;
		}
		apfs_make_super_copy(sb);
	}

fail:
	apfs_node_put(sbi->s_cat_root);
	apfs_node_put(sbi->s_omap_root);

	apfs_unmap_volume_super(sb);
	apfs_unmap_main_super(sb);

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static struct kmem_cache *apfs_inode_cachep;

static struct inode *apfs_alloc_inode(struct super_block *sb)
{
	struct apfs_inode_info *ai;

	ai = kmem_cache_alloc(apfs_inode_cachep, GFP_KERNEL);
	if (!ai)
		return NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) /* iversion came in 4.16 */
	inode_set_iversion(&ai->vfs_inode, 1);
#else
	ai->vfs_inode.i_version = 1;
#endif
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

	spin_lock_init(&ai->i_extent_lock);
	ai->i_cached_extent.len = 0;
	ai->i_nchildren = 0;
	inode_init_once(&ai->vfs_inode);
}

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
	int err;

	err = apfs_transaction_start(sb);
	if (err)
		return err;
	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto fail;
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

/**
 * apfs_count_used_blocks - Count the blocks in use across all volumes
 * @sb:		filesystem superblock
 * @count:	on return it will store the block count
 *
 * This function probably belongs in a separate file, but for now it is
 * only called by statfs.
 */
static int apfs_count_used_blocks(struct super_block *sb, u64 *count)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_node *vnode;
	struct apfs_omap_phys *msb_omap_raw;
	struct buffer_head *bh;
	u64 msb_omap, vb;
	int i;
	int err = 0;

	/* Get the container's object map */
	msb_omap = le64_to_cpu(msb_raw->nx_omap_oid);
	bh = sb_bread(sb, msb_omap);
	if (!bh) {
		apfs_err(sb, "unable to read container object map");
		return -EIO;
	}
	msb_omap_raw = (struct apfs_omap_phys *)bh->b_data;

	/* Get the Volume Block */
	vb = le64_to_cpu(msb_omap_raw->om_tree_oid);
	msb_omap_raw = NULL;
	brelse(bh);
	bh = NULL;
	vnode = apfs_read_node(sb, vb, APFS_OBJ_PHYSICAL, false /* write */);
	if (IS_ERR(vnode)) {
		apfs_err(sb, "unable to read volume block");
		return PTR_ERR(vnode);
	}

	/* Iterate through the checkpoint superblocks and add the used blocks */
	*count = 0;
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_superblock *vsb_raw;
		u64 vol_id;
		u64 vol_bno;

		vol_id = le64_to_cpu(msb_raw->nx_fs_oid[i]);
		if (vol_id == 0) /* All volumes have been checked */
			break;
		err = apfs_omap_lookup_block(sb, vnode, vol_id, &vol_bno,
					     false /* write */);
		if (err)
			break;

		bh = sb_bread(sb, vol_bno);
		if (!bh) {
			err = -EIO;
			apfs_err(sb, "unable to read volume superblock");
			break;
		}
		vsb_raw = (struct apfs_superblock *)bh->b_data;
		*count += le64_to_cpu(vsb_raw->apfs_fs_alloc_count);
		brelse(bh);
	}

	apfs_node_put(vnode);
	return err;
}

static int apfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw;
	struct apfs_superblock *vol;
	u64 fsid, used_blocks = 0;
	int err;

	down_read(&sbi->s_big_sem);
	msb_raw = sbi->s_msb_raw;
	vol = sbi->s_vsb_raw;

	buf->f_type = APFS_SUPER_MAGIC;
	/* Nodes are assumed to fit in a page, for now */
	buf->f_bsize = sb->s_blocksize;

	/* Volumes share the whole disk space */
	buf->f_blocks = le64_to_cpu(msb_raw->nx_block_count);
	err = apfs_count_used_blocks(sb, &used_blocks);
	if (err)
		goto fail;
	buf->f_bfree = buf->f_blocks - used_blocks;
	buf->f_bavail = buf->f_bfree; /* I don't know any better */

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
	up_read(&sbi->s_big_sem);
	return err;
}

static int apfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct apfs_sb_info *sbi = APFS_SB(root->d_sb);

	if (sbi->s_vol_nr != 0)
		seq_printf(seq, ",vol=%u", sbi->s_vol_nr);
	if (uid_valid(sbi->s_uid))
		seq_printf(seq, ",uid=%u", from_kuid(&init_user_ns,
						     sbi->s_uid));
	if (gid_valid(sbi->s_gid))
		seq_printf(seq, ",gid=%u", from_kgid(&init_user_ns,
						     sbi->s_gid));
	if (sbi->s_flags & APFS_CHECK_NODES)
		seq_puts(seq, ",cknodes");

	return 0;
}

static const struct super_operations apfs_sops = {
	.alloc_inode	= apfs_alloc_inode,
	.destroy_inode	= apfs_destroy_inode,
	.write_inode	= apfs_write_inode,
	.evict_inode	= apfs_evict_inode,
	.put_super	= apfs_put_super,
	.statfs		= apfs_statfs,
	.show_options	= apfs_show_options,
};

enum {
	Opt_readwrite, Opt_cknodes, Opt_uid, Opt_gid, Opt_vol, Opt_err,
};

static const match_table_t tokens = {
	{Opt_readwrite, "readwrite"},
	{Opt_cknodes, "cknodes"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_vol, "vol=%u"},
	{Opt_err, NULL}
};

/*
 * Many of the parse_options() functions in other file systems return 0
 * on error. This one returns an error code, and 0 on success.
 */
static int parse_options(struct super_block *sb, char *options)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	int err = 0;
	bool readwrite;

	/* Set default values before parsing */
	sbi->s_vol_nr = 0;
	sbi->s_flags = 0;
	readwrite = false;

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
			readwrite = true;
			break;
		case Opt_cknodes:
			/*
			 * Right now, node checksums are too costly to enable
			 * by default.  TODO: try to improve this.
			 */
			sbi->s_flags |= APFS_CHECK_NODES;
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
			err = match_int(&args[0], &sbi->s_vol_nr);
			if (err)
				return err;
			break;
		default:
			return -EINVAL;
		}
	}

out:
	if (readwrite && !(sb->s_flags & SB_RDONLY))
		apfs_notice(sb, "experimental write support is enabled");
	else
		sb->s_flags |= SB_RDONLY;
	return 0;
}

/**
 * apfs_check_features - Check for unsupported features in the filesystem
 * @sb: superblock structure
 *
 * Returns -EINVAL if unsupported incompatible features are found, otherwise
 * returns 0.
 */
static int apfs_check_features(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nx_superblock *msb_raw = sbi->s_msb_raw;
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	u64 features;

	ASSERT(sbi->s_msb_raw);
	ASSERT(sbi->s_vsb_raw);

	features = le64_to_cpu(msb_raw->nx_incompatible_features);
	if (features & ~APFS_NX_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb,
			  "unknown incompatible container features (0x%llx)",
			  features);
		return -EINVAL;
	}
	if (features & APFS_NX_INCOMPAT_FUSION) {
		apfs_warn(sb, "fusion drives are not supported");
		return -EINVAL;
	}

	features = le64_to_cpu(vsb_raw->apfs_incompatible_features);
	if (features & ~APFS_SUPPORTED_INCOMPAT_MASK) {
		apfs_warn(sb, "unknown incompatible volume features (0x%llx)",
			  features);
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_DATALESS_SNAPS) {
		apfs_warn(sb, "snapshots with no data are not supported");
		return -EINVAL;
	}
	if (features & APFS_INCOMPAT_ENC_ROLLED) {
		apfs_warn(sb, "encrypted volumes are not supported");
		return -EINVAL;
	}

	features = le64_to_cpu(msb_raw->nx_readonly_compatible_features);
	if (features & ~APFS_NX_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb,
		     "unknown read-only compatible container features (0x%llx)",
		     features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "container can't be mounted read-write");
			return -EINVAL;
		}
	}

	features = le64_to_cpu(vsb_raw->apfs_readonly_compatible_features);
	if (features & ~APFS_SUPPORTED_ROCOMPAT_MASK) {
		apfs_warn(sb,
			"unknown read-only compatible volume features (0x%llx)",
			features);
		if (!sb_rdonly(sb)) {
			apfs_warn(sb, "volume can't be mounted read-write");
			return -EINVAL;
		}
	}

	/* TODO: add checks for encryption, snapshots? */
	return 0;
}

static int apfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct apfs_sb_info *sbi;
	struct inode *root;
	int err;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	init_rwsem(&sbi->s_big_sem);

	err = apfs_map_main_super(sb);
	if (err)
		goto failed_main_super;

	/* For now we only support blocksize < PAGE_SIZE */
	sbi->s_blocksize = sb->s_blocksize;
	sbi->s_blocksize_bits = sb->s_blocksize_bits;

	sbi->s_uid = INVALID_UID;
	sbi->s_gid = INVALID_GID;
	err = parse_options(sb, data);
	if (err)
		goto failed_volume_super;

	err = apfs_map_volume_super(sb, false /* write */);
	if (err)
		goto failed_volume_super;

	err = apfs_check_features(sb);
	if (err)
		goto failed_omap;

	/* The omap needs to be set before the call to apfs_read_catalog() */
	err = apfs_read_omap(sb, false /* write */);
	if (err)
		goto failed_omap;

	err = apfs_read_catalog(sb, false /* write */);
	if (err)
		goto failed_cat;

	sb->s_op = &apfs_sops;
	sb->s_d_op = &apfs_dentry_operations;
	sb->s_xattr = apfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1; /* Nanosecond granularity */

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
	return 0;

failed_mount:
	iput(sbi->s_private_dir);
failed_private_dir:
	sbi->s_private_dir = NULL;
	apfs_node_put(sbi->s_cat_root);
failed_cat:
	apfs_node_put(sbi->s_omap_root);
failed_omap:
	apfs_unmap_volume_super(sb);
failed_volume_super:
	apfs_unmap_main_super(sb);
failed_main_super:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return err;
}

static struct dentry *apfs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, apfs_fill_super);
}

static struct file_system_type apfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "apfs",
	.mount		= apfs_mount,
	.kill_sb	= kill_block_super,
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
MODULE_LICENSE("GPL");
module_init(init_apfs_fs)
module_exit(exit_apfs_fs)
