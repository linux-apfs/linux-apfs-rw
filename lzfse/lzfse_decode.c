// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2015-2016, Apple Inc. All rights reserved.
 *
 */

/*  LZFSE decode API
 */

#include <linux/slab.h>
#include "lzfse.h"
#include "lzfse_internal.h"

size_t lzfse_decode_scratch_size(void)
{
	return sizeof(lzfse_decoder_state);
}

static size_t lzfse_decode_buffer_with_scratch(
	uint8_t *__restrict dst_buffer, size_t dst_size,
	const uint8_t *__restrict src_buffer, size_t src_size,
	void *__restrict scratch_buffer)
{
	int status;
	lzfse_decoder_state *s = (lzfse_decoder_state *)scratch_buffer;
	memset(s, 0x00, sizeof(*s));

	// Initialize state
	s->src = src_buffer;
	s->src_begin = src_buffer;
	s->src_end = s->src + src_size;
	s->dst = dst_buffer;
	s->dst_begin = dst_buffer;
	s->dst_end = dst_buffer + dst_size;

	// Decode
	status = lzfse_decode(s);
	if (status == LZFSE_STATUS_DST_FULL)
		return dst_size;
	if (status != LZFSE_STATUS_OK)
		return 0; // failed
	return (size_t)(s->dst - dst_buffer); // bytes written
}

size_t lzfse_decode_buffer(uint8_t *__restrict dst_buffer, size_t dst_size,
			   const uint8_t *__restrict src_buffer,
			   size_t src_size, void *__restrict scratch_buffer)
{
	int has_malloc = 0;
	size_t ret = 0;

	// Deal with the possible NULL pointer
	if (scratch_buffer == NULL) {
		// +1 in case scratch size could be zero
		scratch_buffer =
			kmalloc(lzfse_decode_scratch_size() + 1, GFP_KERNEL);
		has_malloc = 1;
	}
	if (scratch_buffer == NULL)
		return 0;
	ret = lzfse_decode_buffer_with_scratch(dst_buffer, dst_size, src_buffer,
					       src_size, scratch_buffer);
	if (has_malloc)
		kfree(scratch_buffer);
	return ret;
}
