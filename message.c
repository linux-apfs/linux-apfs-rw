// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/fs.h>
#include "apfs.h"

void apfs_msg(struct super_block *sb, const char *prefix, const char *func, int line, const char *fmt, ...)
{
	char *sb_id = NULL;
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	/* The superblock is not available to all callers */
	sb_id = sb ? sb->s_id : "?";

	if (func)
		printk("%sAPFS (%s): %pV (%s:%d)\n", prefix, sb_id, &vaf, func, line);
	else
		printk("%sAPFS (%s): %pV\n", prefix, sb_id, &vaf);

	va_end(args);
}
