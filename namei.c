// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/namei.h>
#include "apfs.h"
#include "unicode.h"

static struct dentry *apfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct inode *inode = NULL;
	u64 ino = 0;
	int err;

	if (dentry->d_name.len > APFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	err = apfs_inode_by_name(dir, &dentry->d_name, &ino);
	if (err && err != -ENODATA) {
		apfs_err(dir->i_sb, "inode lookup by name failed");
		return ERR_PTR(err);
	}

	if (!err) {
		inode = apfs_iget(dir->i_sb, ino);
		if (IS_ERR(inode)) {
			apfs_err(dir->i_sb, "iget failed");
			return ERR_CAST(inode);
		}
	}

	return d_splice_alias(inode, dentry);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
static int apfs_symlink(struct inode *dir, struct dentry *dentry,
			const char *symname)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static int apfs_symlink(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, const char *symname)
#else
static int apfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
#endif
{
	/* Symlink permissions don't mean anything and their value is fixed */
	return apfs_mkany(dir, dentry, S_IFLNK | 0x1ed, 0 /* rdev */, symname);
}

const struct inode_operations apfs_dir_inode_operations = {
	.create		= apfs_create,
	.lookup		= apfs_lookup,
	.link		= apfs_link,
	.unlink		= apfs_unlink,
	.symlink	= apfs_symlink,
	.mkdir		= apfs_mkdir,
	.rmdir		= apfs_rmdir,
	.mknod		= apfs_mknod,
	.rename		= apfs_rename,
	.getattr	= apfs_getattr,
	.listxattr      = apfs_listxattr,
	.setattr	= apfs_setattr,
	.update_time	= apfs_update_time,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	.fileattr_get	= apfs_fileattr_get,
	.fileattr_set	= apfs_fileattr_set,
#endif
};

const struct inode_operations apfs_special_inode_operations = {
	.getattr	= apfs_getattr,
	.listxattr      = apfs_listxattr,
	.setattr	= apfs_setattr,
	.update_time	= apfs_update_time,
};

static int apfs_dentry_hash(const struct dentry *dir, struct qstr *child)
{
	struct apfs_unicursor cursor;
	unsigned long hash;
	bool case_fold = apfs_is_case_insensitive(dir->d_sb);

	if (!apfs_is_normalization_insensitive(dir->d_sb))
		return 0;

	apfs_init_unicursor(&cursor, child->name, child->len);
	hash = init_name_hash(dir);

	while (1) {
		int i;
		unicode_t utf32;

		utf32 = apfs_normalize_next(&cursor, case_fold);
		if (!utf32)
			break;

		/* Hash the unicode character one byte at a time */
		for (i = 0; i < 4; ++i) {
			hash = partial_name_hash((u8)utf32, hash);
			utf32 = utf32 >> 8;
		}
	}
	child->hash = end_name_hash(hash);

	/* TODO: return error instead of truncating invalid UTF-8? */
	return 0;
}

static int apfs_dentry_compare(const struct dentry *dentry, unsigned int len,
			       const char *str, const struct qstr *name)
{
	return apfs_filename_cmp(dentry->d_sb, name->name, name->len, str, len);
}

static int apfs_dentry_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct super_block *sb = dentry->d_sb;

	if (flags & LOOKUP_RCU)
		return -ECHILD;

	/*
	 * If we want to create a link with a name that normalizes to the same
	 * as an existing negative dentry, then we first need to invalidate the
	 * dentry; otherwise it would keep the existing name.
	 */
	if (d_really_is_positive(dentry))
		return 1;
	if (!apfs_is_case_insensitive(sb) && !apfs_is_normalization_insensitive(sb))
		return 1;
	if (flags & (LOOKUP_CREATE | LOOKUP_RENAME_TARGET))
		return 0;
	return 1;
}

const struct dentry_operations apfs_dentry_operations = {
	.d_revalidate	= apfs_dentry_revalidate,
	.d_hash		= apfs_dentry_hash,
	.d_compare	= apfs_dentry_compare,
};
