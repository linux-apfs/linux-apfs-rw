/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2015-2016, Apple Inc. All rights reserved.
 *
 */

/*  LZVN low-level decoder (v2)
 *  Functions in the low-level API should switch to these at some point.
 *  Apr 2014
 */

#ifndef LZVN_DECODE_BASE_H
#define LZVN_DECODE_BASE_H

#include "lzfse_internal.h"

/*! @abstract Base decoder state. */
typedef struct {
	// Decoder I/O

	// Next byte to read in source buffer
	const unsigned char *src;
	// Next byte after source buffer
	const unsigned char *src_end;

	// Next byte to write in destination buffer (by decoder)
	unsigned char *dst;
	// Valid range for destination buffer is [dst_begin, dst_end - 1]
	unsigned char *dst_begin;
	unsigned char *dst_end;
	// Next byte to read in destination buffer (modified by caller)
	unsigned char *dst_current;

	// Decoder state

	// Partially expanded match, or 0,0,0.
	// In that case, src points to the next literal to copy, or the next op-code
	// if L==0.
	size_t L, M, D;

	// Distance for last emitted match, or 0
	lzvn_offset d_prev;

	// Did we decode end-of-stream?
	int end_of_stream;

} lzvn_decoder_state;

/*! @abstract Decode source to destination.
 *  Updates \p state (src,dst,d_prev). */
void lzvn_decode(lzvn_decoder_state *state);

#endif // LZVN_DECODE_BASE_H
