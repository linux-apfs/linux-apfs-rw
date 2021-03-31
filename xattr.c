// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/buffer_head.h>
#include <linux/xattr.h>
#include "apfs.h"

/**
 * apfs_xattr_from_query - Read the xattr record found by a successful query
 * @query:	the query that found the record
 * @xattr:	Return parameter.  The xattr record found.
 *
 * Reads the xattr record into @xattr and performs some basic sanity checks
 * as a protection against crafted filesystems.  Returns 0 on success or
 * -EFSCORRUPTED otherwise.
 *
 * The caller must not free @query while @xattr is in use, because @xattr->name
 * and @xattr->xdata point to data on disk.
 */
static int apfs_xattr_from_query(struct apfs_query *query,
				 struct apfs_xattr *xattr)
{
	struct apfs_xattr_val *xattr_val;
	struct apfs_xattr_key *xattr_key;
	char *raw = query->node->object.bh->b_data;
	int datalen = query->len - sizeof(*xattr_val);
	int namelen = query->key_len - sizeof(*xattr_key);

	if (namelen < 1 || datalen < 0)
		return -EFSCORRUPTED;

	xattr_val = (struct apfs_xattr_val *)(raw + query->off);
	xattr_key = (struct apfs_xattr_key *)(raw + query->key_off);

	if (namelen != le16_to_cpu(xattr_key->name_len))
		return -EFSCORRUPTED;

	/* The xattr name must be NULL-terminated */
	if (xattr_key->name[namelen - 1] != 0)
		return -EFSCORRUPTED;

	xattr->has_dstream = le16_to_cpu(xattr_val->flags) &
			     APFS_XATTR_DATA_STREAM;

	if (xattr->has_dstream && datalen != sizeof(struct apfs_xattr_dstream))
		return -EFSCORRUPTED;
	if (!xattr->has_dstream && datalen != le16_to_cpu(xattr_val->xdata_len))
		return -EFSCORRUPTED;

	xattr->name = xattr_key->name;
	xattr->name_len = namelen - 1; /* Don't count the NULL termination */
	xattr->xdata = xattr_val->xdata;
	xattr->xdata_len = datalen;
	return 0;
}

/**
 * apfs_xattr_extents_read - Read the value of a xattr from its extents
 * @parent:	inode the attribute belongs to
 * @xattr:	the xattr structure
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * Copies the value of @xattr to @buffer, if provided. If @buffer is NULL, just
 * computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
static int apfs_xattr_extents_read(struct inode *parent,
				   struct apfs_xattr *xattr,
				   void *buffer, size_t size)
{
	struct super_block *sb = parent->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_xattr_dstream *xdata;
	u64 extent_id;
	int length;
	int ret;
	int i;

	xdata = (struct apfs_xattr_dstream *) xattr->xdata;
	length = le64_to_cpu(xdata->dstream.size);
	if (length < 0 || length < le64_to_cpu(xdata->dstream.size))
		return -E2BIG;

	if (!buffer) /* All we want is the length */
		return length;
	if (length > size) /* xattr won't fit in the buffer */
		return -ERANGE;

	extent_id = le64_to_cpu(xdata->xattr_obj_id);
	/* We will read all the extents, starting with the last one */
	apfs_init_file_extent_key(extent_id, 0 /* offset */, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_MULTIPLE | APFS_QUERY_EXACT;

	/*
	 * The logic in this loop would allow a crafted filesystem with a large
	 * number of redundant extents to become stuck for a long time. We use
	 * the xattr length to put a limit on the number of iterations.
	 */
	ret = -EFSCORRUPTED;
	for (i = 0; i < (length >> parent->i_blkbits) + 2; i++) {
		struct apfs_file_extent ext;
		u64 block_count, file_off;
		int err;
		int j;

		err = apfs_btree_query(sb, &query);
		if (err == -ENODATA) { /* No more records to search */
			ret = length;
			goto done;
		}
		if (err) {
			ret = err;
			goto done;
		}

		err = apfs_extent_from_query(query, &ext);
		if (err) {
			apfs_alert(sb, "bad extent for xattr in inode 0x%llx",
				   apfs_ino(parent));
			ret = err;
			goto done;
		}

		block_count = ext.len >> sb->s_blocksize_bits;
		file_off = ext.logical_addr;
		for (j = 0; j < block_count; ++j) {
			struct buffer_head *bh;
			int bytes;

			if (length <= file_off) /* Read the whole extent */
				break;
			bytes = min(sb->s_blocksize,
				    (unsigned long)(length - file_off));

			bh = sb_bread(sb, ext.phys_block_num + j);
			if (!bh) {
				ret = -EIO;
				goto done;
			}
			memcpy(buffer + file_off, bh->b_data, bytes);
			brelse(bh);
			file_off = file_off + bytes;
		}
	}

done:
	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_xattr_inline_read - Read the value of an inline xattr
 * @parent:	inode the attribute belongs to
 * @xattr:	the xattr structure
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * Copies the inline value of @xattr to @buffer, if provided. If @buffer is
 * NULL, just computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
static int apfs_xattr_inline_read(struct inode *parent,
				  struct apfs_xattr *xattr,
				  void *buffer, size_t size)
{
	int length = xattr->xdata_len;

	if (!buffer) /* All we want is the length */
		return length;
	if (length > size) /* xattr won't fit in the buffer */
		return -ERANGE;
	memcpy(buffer, xattr->xdata, length);
	return length;
}

/**
 * __apfs_xattr_get - Find and read a named attribute
 * @inode:	inode the attribute belongs to
 * @name:	name of the attribute
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * This does the same as apfs_xattr_get(), but without taking any locks.
 */
int __apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
		     size_t size)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_xattr xattr;
	u64 cnid = apfs_ino(inode);
	int ret;

	apfs_init_xattr_key(cnid, name, &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto done;

	ret = apfs_xattr_from_query(query, &xattr);
	if (ret) {
		apfs_alert(sb, "bad xattr record in inode 0x%llx", cnid);
		goto done;
	}

	if (xattr.has_dstream)
		ret = apfs_xattr_extents_read(inode, &xattr, buffer, size);
	else
		ret = apfs_xattr_inline_read(inode, &xattr, buffer, size);

done:
	apfs_free_query(sb, query);
	return ret;
}

/**
 * apfs_xattr_get - Find and read a named attribute
 * @inode:	inode the attribute belongs to
 * @name:	name of the attribute
 * @buffer:	where to copy the attribute value
 * @size:	size of @buffer
 *
 * Finds an extended attribute and copies its value to @buffer, if provided. If
 * @buffer is NULL, just computes the size of the buffer required.
 *
 * Returns the number of bytes used/required, or a negative error code in case
 * of failure.
 */
int apfs_xattr_get(struct inode *inode, const char *name, void *buffer,
		   size_t size)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	int ret;

	down_read(&sbi->s_big_sem);
	ret = __apfs_xattr_get(inode, name, buffer, size);
	up_read(&sbi->s_big_sem);
	return ret;
}

static int apfs_xattr_osx_get(const struct xattr_handler *handler,
				struct dentry *unused, struct inode *inode,
				const char *name, void *buffer, size_t size)
{
	/* Ignore the fake 'osx' prefix */
	return apfs_xattr_get(inode, name, buffer, size);
}

static const struct xattr_handler apfs_xattr_osx_handler = {
	.prefix	= XATTR_MAC_OSX_PREFIX,
	.get	= apfs_xattr_osx_get,
};

/* On-disk xattrs have no namespace; use a fake 'osx' prefix in the kernel */
const struct xattr_handler *apfs_xattr_handlers[] = {
	&apfs_xattr_osx_handler,
	NULL
};

ssize_t apfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	u64 cnid = apfs_ino(inode);
	size_t free = size;
	ssize_t ret;

	down_read(&sbi->s_big_sem);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query) {
		ret = -ENOMEM;
		goto fail;
	}

	/* We want all the xattrs for the cnid, regardless of the name */
	apfs_init_xattr_key(cnid, NULL /* name */, &key);
	query->key = &key;
	query->flags = APFS_QUERY_CAT | APFS_QUERY_MULTIPLE | APFS_QUERY_EXACT;

	while (1) {
		struct apfs_xattr xattr;

		ret = apfs_btree_query(sb, &query);
		if (ret == -ENODATA) { /* Got all the xattrs */
			ret = size - free;
			break;
		}
		if (ret)
			break;

		ret = apfs_xattr_from_query(query, &xattr);
		if (ret) {
			apfs_alert(sb, "bad xattr key in inode %llx", cnid);
			break;
		}

		if (buffer) {
			/* Prepend the fake 'osx' prefix before listing */
			if (xattr.name_len + XATTR_MAC_OSX_PREFIX_LEN + 1 >
									free) {
				ret = -ERANGE;
				break;
			}
			memcpy(buffer, XATTR_MAC_OSX_PREFIX,
			       XATTR_MAC_OSX_PREFIX_LEN);
			buffer += XATTR_MAC_OSX_PREFIX_LEN;
			memcpy(buffer, xattr.name, xattr.name_len + 1);
			buffer += xattr.name_len + 1;
		}
		free -= xattr.name_len + XATTR_MAC_OSX_PREFIX_LEN + 1;
	}

fail:
	apfs_free_query(sb, query);
	up_read(&sbi->s_big_sem);
	return ret;
}
