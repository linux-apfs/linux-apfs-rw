// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2015-2016, Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the distribution.
 *
 * 3.  Neither the name of the copyright holder(s) nor the names of any contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "lzfse_internal.h"

/*
 * Initialize decoder table T[NSTATES].
 * NSTATES = sum FREQ[i] is the number of states (a power of 2)
 * NSYMBOLS is the number of symbols.
 * FREQ[NSYMBOLS] is a normalized histogram of symbol frequencies, with FREQ[i]
 * >= 0.
 * Some symbols may have a 0 frequency. In that case, they should not be
 * present in the data.
 */
int fse_init_decoder_table(int nstates, int nsymbols, const uint16_t *__restrict freq,
			   int32_t *__restrict t)
{
	int n_clz = __builtin_clz(nstates);
	int sum_of_freq = 0;
	int i, j0, j;

	for (i = 0; i < nsymbols; i++) {
		int f = (int)freq[i];
		int k;

		if (f == 0)
			continue; /* skip this symbol, no occurrences */

		sum_of_freq += f;

		if (sum_of_freq > nstates)
			return -1;

		k = __builtin_clz(f) - n_clz; /* shift needed to ensure N <= (F<<K) < 2*N */
		j0 = ((2 * nstates) >> k) - f;

		/* Initialize all states S reached by this symbol: OFFSET <= S < OFFSET + F */
		for (j = 0; j < f; j++) {
			fse_decoder_entry e;

			e.symbol = (uint8_t)i;
			if (j < j0) {
				e.k = (int8_t)k;
				e.delta = (int16_t)(((f + j) << k) - nstates);
			} else {
				e.k = (int8_t)(k - 1);
				e.delta = (int16_t)((j - j0) << (k - 1));
			}

			memcpy(t, &e, sizeof(e));
			t++;
		}
	}

	return 0; /* OK */
}

/*
 * Initialize value decoder table T[NSTATES].
 * NSTATES = sum FREQ[i] is the number of states (a power of 2)
 * NSYMBOLS is the number of symbols.
 * FREQ[NSYMBOLS] is a normalized histogram of symbol frequencies, with FREQ[i]
 * >= 0.
 * SYMBOL_VBITS[NSYMBOLS] and SYMBOLS_VBASE[NSYMBOLS] are the number of value
 * bits to read and the base value for each symbol.
 * Some symbols may have a 0 frequency. In that case, they should not be
 * present in the data.
 */
void fse_init_value_decoder_table(int nstates, int nsymbols, const uint16_t *__restrict freq,
				  const uint8_t *__restrict symbol_vbits,
				  const int32_t *__restrict symbol_vbase,
				  struct fse_value_decoder_entry *__restrict t)
{
	int n_clz = __builtin_clz(nstates);
	int i;

	for (i = 0; i < nsymbols; i++) {
		struct fse_value_decoder_entry ei = { 0 };
		int f = (int)freq[i];
		int k, j0, j;

		if (f == 0)
			continue; /* skip this symbol, no occurrences */

		k = __builtin_clz(f) - n_clz; /* shift needed to ensure N <= (F<<K) < 2*N */
		j0 = ((2 * nstates) >> k) - f;

		ei.value_bits = symbol_vbits[i];
		ei.vbase = symbol_vbase[i];

		/* Initialize all states S reached by this symbol: OFFSET <= S < OFFSET + F */
		for (j = 0; j < f; j++) {
			struct fse_value_decoder_entry e = ei;

			if (j < j0) {
				e.total_bits = (uint8_t)k + e.value_bits;
				e.delta = (int16_t)(((f + j) << k) - nstates);
			} else {
				e.total_bits = (uint8_t)(k - 1) + e.value_bits;
				e.delta = (int16_t)((j - j0) << (k - 1));
			}

			memcpy(t, &e, 8);
			t++;
		}
	}
}
