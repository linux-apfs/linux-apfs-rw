/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2015-2016, Apple Inc. All rights reserved.
 *
 */

#ifndef LZFSE_TUNABLES_H
#define LZFSE_TUNABLES_H

/*   Parameters controlling details of the LZ-style match search. These values
 *   may be modified to fine tune compression ratio vs. encoding speed, while
 *   keeping the compressed format compatible with LZFSE. Note that
 *   modifying them will also change the amount of work space required by
 *   the encoder. The values here are those used in the compression library
 *   on iOS and OS X.
 */

/*   Number of bits for hash function to produce. Should be in the range
 *   [10, 16]. Larger values reduce the number of false-positive found during
 *   the match search, and expand the history table, which may allow additional
 *   matches to be found, generally improving the achieved compression ratio.
 *   Larger values also increase the workspace size, and make it less likely
 *   that the history table will be present in cache, which reduces performance.
 */
#define LZFSE_ENCODE_HASH_BITS 14

/*   Number of positions to store for each line in the history table. May
 *   be either 4 or 8. Using 8 doubles the size of the history table, which
 *   increases the chance of finding matches (thus improving compression ratio),
 *   but also increases the workspace size.
 */
#define LZFSE_ENCODE_HASH_WIDTH 4

/*   Match length in bytes to cause immediate emission. Generally speaking,
 *   LZFSE maintains multiple candidate matches and waits to decide which match
 *   to emit until more information is available. When a match exceeds this
 *   threshold, it is emitted immediately. Thus, smaller values may give
 *   somewhat better performance, and larger values may give somewhat better
 *   compression ratios.
 */
#define LZFSE_ENCODE_GOOD_MATCH 40

/*   When the source buffer is very small, LZFSE doesn't compress as well as
 *   some simpler algorithms. To maintain reasonable compression for these
 *   cases, we transition to use LZVN instead if the size of the source buffer
 *   is below this threshold.
 */
#define LZFSE_ENCODE_LZVN_THRESHOLD 4096

#endif // LZFSE_TUNABLES_H
