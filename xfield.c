// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include "apfs.h"

/**
 * apfs_find_xfield - Find an extended field value in an inode or dentry record
 * @xfields:	pointer to the on-disk xfield collection for the record
 * @len:	length of the collection
 * @xtype:	type of the xfield to retrieve
 * @xval:	on return, a pointer to the wanted on-disk xfield value
 *
 * Returns the length of @xval on success, or 0 if no matching xfield was found;
 * the caller must check that the expected structures fit before casting @xval.
 */
int apfs_find_xfield(u8 *xfields, int len, u8 xtype, char **xval)
{
	struct apfs_xf_blob *blob;
	struct apfs_x_field *xfield;
	int count;
	int rest = len;
	int i;

	if (!len)
		return 0; /* No xfield data */

	rest -= sizeof(*blob);
	if (rest < 0)
		return 0; /* Corruption */
	blob = (struct apfs_xf_blob *)xfields;

	count = le16_to_cpu(blob->xf_num_exts);
	rest -= count * sizeof(*xfield);
	if (rest < 0)
		return 0; /* Corruption */
	xfield = (struct apfs_x_field *)blob->xf_data;

	for (i = 0; i < count; ++i) {
		int xlen;

		/* Attribute length is padded to a multiple of 8 */
		xlen = round_up(le16_to_cpu(xfield[i].x_size), 8);
		if (xlen > rest)
			return 0; /* Corruption */

		if (xfield[i].x_type == xtype) {
			*xval = (char *)xfields + len - rest;
			return xlen;
		}
		rest -= xlen;
	}
	return 0;
}

/**
 * apfs_init_xfields - Set an empty collection of xfields in a buffer
 * @buffer:	buffer to hold the xfields
 * @buflen:	length of the buffer; should be enough to fit an xfield blob
 *
 * Returns 0 on success, or -1 if the buffer isn't long enough.
 */
int apfs_init_xfields(u8 *buffer, int buflen)
{
	struct apfs_xf_blob *blob;

	if (buflen < sizeof(*blob))
		return -1;
	blob = (struct apfs_xf_blob *)buffer;

	blob->xf_num_exts = 0;
	blob->xf_used_data = 0;
	return 0;
}

/**
 * apfs_insert_xfield - Add a new xfield to an in-memory collection
 * @buffer:	buffer holding the collection of xfields
 * @buflen:	length of the buffer; should be enough to fit the new xfield
 * @xkey:	metadata for the new xfield
 * @xval:	value for the new xfield
 *
 * Returns the new length of the collection, or 0 if it the allocation would
 * overflow @buffer.
 */
int apfs_insert_xfield(u8 *buffer, int buflen, const struct apfs_x_field *xkey,
		       const void *xval)
{
	struct apfs_xf_blob *blob;
	struct apfs_x_field *curr_xkey;
	void *curr_xval;
	int count;
	int rest = buflen;
	u16 used_data;
	int xlen, padded_xlen;
	int meta_len, total_len;
	int i;

	xlen = le16_to_cpu(xkey->x_size);
	padded_xlen = round_up(xlen, 8);

	if (!buflen)
		return 0;

	rest -= sizeof(*blob);
	if (rest < 0)
		return 0;
	blob = (struct apfs_xf_blob *)buffer;
	used_data = le16_to_cpu(blob->xf_used_data);

	count = le16_to_cpu(blob->xf_num_exts);
	rest -= count * sizeof(*curr_xkey);
	if (rest < 0)
		return 0;
	meta_len = buflen - rest;
	curr_xkey = (struct apfs_x_field *)blob->xf_data;

	for (i = 0; i < count; ++i, ++curr_xkey) {
		int curr_xlen;

		/* Attribute length is padded to a multiple of 8 */
		curr_xlen = round_up(le16_to_cpu(curr_xkey->x_size), 8);
		if (curr_xlen > rest)
			return 0;
		if (curr_xkey->x_type != xkey->x_type) {
			rest -= curr_xlen;
			continue;
		}

		/* The xfield already exists, so just resize it and set it */
		memcpy(curr_xkey, xkey, sizeof(*curr_xkey));
		if (padded_xlen > rest)
			return 0;
		curr_xval = buffer + buflen - rest;
		rest -= max(padded_xlen, curr_xlen);
		memmove(curr_xval + padded_xlen, curr_xval + curr_xlen, rest);
		memcpy(curr_xval, xval, xlen);
		memset(curr_xval + xlen, 0, padded_xlen - xlen);
		used_data += padded_xlen - curr_xlen;

		goto done;
	}

	/* Create a metadata entry for the new xfield */
	rest -= sizeof(*curr_xkey);
	if (rest < 0)
		return 0;
	meta_len += sizeof(*curr_xkey);
	memmove(curr_xkey + 1, curr_xkey, buflen - meta_len);
	memcpy(curr_xkey, xkey, sizeof(*curr_xkey));
	++count;

	/* Now set the xfield value */
	if (padded_xlen > rest)
		return 0;
	curr_xval = buffer + buflen - rest;
	memcpy(curr_xval, xval, xlen);
	memset(curr_xval + xlen, 0, padded_xlen - xlen);
	used_data += padded_xlen;

done:
	total_len = used_data + meta_len;
	if (total_len > buflen)
		return 0;
	blob->xf_num_exts = cpu_to_le16(count);
	blob->xf_used_data = cpu_to_le16(used_data);
	return total_len;
}
