/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#ifndef _APFS_UNICODE_H
#define _APFS_UNICODE_H

#include <linux/nls.h>

/*
 * This structure helps apfs_normalize_next() to retrieve one normalized
 * (and case-folded) UTF-32 character at a time from a UTF-8 string.
 */
struct apfs_unicursor {
	const char *utf8curr;	/* Start of UTF-8 to decompose and reorder */
	unsigned int total_len;	/* Length of the whole UTF-8 string */
	int length;		/* Length of normalization until next starter */
	int last_pos;           /* Offset in substring of last char returned */
	u8 last_ccc;		/* CCC of the last character returned */
};

extern void apfs_init_unicursor(struct apfs_unicursor *cursor, const char *utf8str, unsigned int total_len);
extern unicode_t apfs_normalize_next(struct apfs_unicursor *cursor,
				     bool case_fold);

#endif	/* _APFS_UNICODE_H */
