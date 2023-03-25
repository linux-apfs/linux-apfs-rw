// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/crc32c.h>
#include "apfs.h"
#include "unicode.h"

/**
 * apfs_filename_cmp - Normalize and compare two APFS filenames
 * @sb:		filesystem superblock
 * @name1:	first name to compare
 * @len1:	length of @name1
 * @name2:	second name to compare
 * @len2:	length of the @name2
 *
 * Returns 0 if @name1 and @name2 are equal, or non-zero otherwise.
 */
int apfs_filename_cmp(struct super_block *sb,
		      const char *name1, unsigned int len1,
		      const char *name2, unsigned int len2)
{
	struct apfs_unicursor cursor1, cursor2;
	bool case_fold = apfs_is_case_insensitive(sb);

	if (!apfs_is_normalization_insensitive(sb)) {
		if (len1 != len2)
			return -1;
		return memcmp(name1, name2, len1);
	}

	apfs_init_unicursor(&cursor1, name1, len1);
	apfs_init_unicursor(&cursor2, name2, len2);

	while (1) {
		unicode_t uni1, uni2;

		uni1 = apfs_normalize_next(&cursor1, case_fold);
		uni2 = apfs_normalize_next(&cursor2, case_fold);

		if (uni1 != uni2)
			return uni1 < uni2 ? -1 : 1;
		if (!uni1)
			return 0;
	}
}

/**
 * apfs_keycmp - Compare two keys
 * @k1:	first key to compare
 * @k2:	second key to compare
 *
 * returns   0 if @k1 and @k2 are equal
 *	   < 0 if @k1 comes before @k2 in the btree
 *	   > 0 if @k1 comes after @k2 in the btree
 */
int apfs_keycmp(struct apfs_key *k1, struct apfs_key *k2)
{
	if (k1->id != k2->id)
		return k1->id < k2->id ? -1 : 1;
	if (k1->type != k2->type)
		return k1->type < k2->type ? -1 : 1;
	if (k1->number != k2->number)
		return k1->number < k2->number ? -1 : 1;
	if (!k1->name || !k2->name)
		return 0;

	/* Normalization seems to be ignored here, even for directory records */
	return strcmp(k1->name, k2->name);
}

/**
 * apfs_read_cat_key - Parse an on-disk catalog key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	apfs_key structure to store the result
 * @hashed:	are the directory records hashed?
 *
 * Returns 0 on success, or a negative error code otherwise.
 */
int apfs_read_cat_key(void *raw, int size, struct apfs_key *key, bool hashed)
{
	if (size < sizeof(struct apfs_key_header)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	key->id = apfs_cat_cnid((struct apfs_key_header *)raw);
	key->type = apfs_cat_type((struct apfs_key_header *)raw);

	switch (key->type) {
	case APFS_TYPE_DIR_REC:
		if (hashed) {
			if (size < sizeof(struct apfs_drec_hashed_key) + 1 ||
			    *((char *)raw + size - 1) != 0) {
				/* Filename must have NULL-termination */
				apfs_err(NULL, "invalid drec key (%d)", size);
				return -EFSCORRUPTED;
			}
			/* Name length is not used in key comparisons, only the hash */
			key->number = le32_to_cpu(
			      ((struct apfs_drec_hashed_key *)raw)->name_len_and_hash) &
								    APFS_DREC_HASH_MASK;
			key->name = ((struct apfs_drec_hashed_key *)raw)->name;
		} else {
			if (size < sizeof(struct apfs_drec_key) + 1 ||
			    *((char *)raw + size - 1) != 0) {
				/* Filename must have NULL-termination */
				apfs_err(NULL, "invalid drec key (%d)", size);
				return -EFSCORRUPTED;
			}
			/* There's no hash */
			key->number = 0;
			key->name = ((struct apfs_drec_key *)raw)->name;
		}
		break;
	case APFS_TYPE_XATTR:
		if (size < sizeof(struct apfs_xattr_key) + 1 ||
		    *((char *)raw + size - 1) != 0) {
			/* xattr name must have NULL-termination */
			apfs_err(NULL, "invalid xattr key (%d)", size);
			return -EFSCORRUPTED;
		}
		key->number = 0;
		key->name = ((struct apfs_xattr_key *)raw)->name;
		break;
	case APFS_TYPE_FILE_EXTENT:
		if (size != sizeof(struct apfs_file_extent_key)) {
			apfs_err(NULL, "bad key length (%d)", size);
			return -EFSCORRUPTED;
		}
		key->number = le64_to_cpu(
			((struct apfs_file_extent_key *)raw)->logical_addr);
		key->name = NULL;
		break;
	case APFS_TYPE_SIBLING_LINK:
		if (size != sizeof(struct apfs_sibling_link_key)) {
			apfs_err(NULL, "bad key length (%d)", size);
			return -EFSCORRUPTED;
		}
		key->number = le64_to_cpu(
			((struct apfs_sibling_link_key *)raw)->sibling_id);
		key->name = NULL;
		break;
	default:
		key->number = 0;
		key->name = NULL;
		break;
	}

	return 0;
}

int apfs_read_fext_key(void *raw, int size, struct apfs_key *key)
{
	struct apfs_fext_tree_key *raw_key;

	if (size != sizeof(*raw_key)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	raw_key = raw;

	key->id = le64_to_cpu(raw_key->private_id);
	key->type = 0;
	key->number = le64_to_cpu(raw_key->logical_addr);
	key->name = NULL;
	return 0;
}

/**
 * apfs_read_free_queue_key - Parse an on-disk free queue key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	apfs_key structure to store the result
 *
 * Returns 0 on success, or a negative error code otherwise.
 */
int apfs_read_free_queue_key(void *raw, int size, struct apfs_key *key)
{
	struct apfs_spaceman_free_queue_key *raw_key;

	if (size < sizeof(struct apfs_spaceman_free_queue_key)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	raw_key = raw;

	key->id = le64_to_cpu(raw_key->sfqk_xid);
	key->type = 0;
	key->number = le64_to_cpu(raw_key->sfqk_paddr);
	key->name = NULL;

	return 0;
}

/**
 * apfs_read_omap_key - Parse an on-disk object map key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	apfs_key structure to store the result
 *
 * Returns 0 on success, or a negative error code otherwise.
 */
int apfs_read_omap_key(void *raw, int size, struct apfs_key *key)
{
	if (size < sizeof(struct apfs_omap_key)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}

	key->id = le64_to_cpu(((struct apfs_omap_key *)raw)->ok_oid);
	key->type = 0;
	key->number = le64_to_cpu(((struct apfs_omap_key *)raw)->ok_xid);
	key->name = NULL;

	return 0;
}

/**
 * apfs_read_extentref_key - Parse an on-disk extent reference tree key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	apfs_key structure to store the result
 *
 * Returns 0 on success, or a negative error code otherwise.
 */
int apfs_read_extentref_key(void *raw, int size, struct apfs_key *key)
{
	if (size != sizeof(struct apfs_phys_ext_key)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	key->id = apfs_cat_cnid((struct apfs_key_header *)raw);
	key->type = apfs_cat_type((struct apfs_key_header *)raw);
	key->number = 0;
	key->name = NULL;
	return 0;
}

int apfs_read_snap_meta_key(void *raw, int size, struct apfs_key *key)
{
	if (size < sizeof(struct apfs_key_header)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	key->id = apfs_cat_cnid((struct apfs_key_header *)raw);
	key->type = apfs_cat_type((struct apfs_key_header *)raw);
	key->number = 0;

	switch (key->type) {
	case APFS_TYPE_SNAP_METADATA:
		if (size != sizeof(struct apfs_snap_metadata_key)) {
			apfs_err(NULL, "bad key length (%d)", size);
			return -EFSCORRUPTED;
		}
		key->name = NULL;
		return 0;
	case APFS_TYPE_SNAP_NAME:
		if (size < sizeof(struct apfs_snap_name_key) + 1 || *((char *)raw + size - 1) != 0) {
			/* snapshot name must have NULL-termination */
			apfs_err(NULL, "invalid snap name key (%d)", size);
			return -EFSCORRUPTED;
		}
		key->name = ((struct apfs_snap_name_key *)raw)->name;
		return 0;
	default:
		return -EFSCORRUPTED;
	}
}

int apfs_read_omap_snap_key(void *raw, int size, struct apfs_key *key)
{
	__le64 *xid = NULL;

	if (size != sizeof(*xid)) {
		apfs_err(NULL, "bad key length (%d)", size);
		return -EFSCORRUPTED;
	}
	xid = raw;

	key->id = le64_to_cpup(xid);
	key->number = 0;
	key->name = NULL;
	key->type = 0;
	return 0;
}

/**
 * apfs_init_drec_key - Initialize an in-memory key for a dentry query
 * @sb:		filesystem superblock
 * @ino:	inode number of the parent directory
 * @name:	filename (NULL for a multiple query)
 * @name_len:	filename length (0 if NULL)
 * @key:	apfs_key structure to initialize
 */
void apfs_init_drec_key(struct super_block *sb, u64 ino, const char *name,
			unsigned int name_len, struct apfs_key *key)
{
	struct apfs_unicursor cursor;
	bool case_fold = apfs_is_case_insensitive(sb);
	u32 hash = 0xFFFFFFFF;

	key->id = ino;
	key->type = APFS_TYPE_DIR_REC;
	if (!apfs_is_normalization_insensitive(sb)) {
		key->name = name;
		key->number = 0;
		return;
	}

	/* To respect normalization, queries can only consider the hash */
	key->name = NULL;

	if (!name) {
		key->number = 0;
		return;
	}

	apfs_init_unicursor(&cursor, name, name_len);

	while (1) {
		unicode_t utf32;

		utf32 = apfs_normalize_next(&cursor, case_fold);
		if (!utf32)
			break;

		hash = crc32c(hash, &utf32, sizeof(utf32));
	}

	/* The filename length doesn't matter, so it's left as zero */
	key->number = hash << APFS_DREC_HASH_SHIFT;
}
