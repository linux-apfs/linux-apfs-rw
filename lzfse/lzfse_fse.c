// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2015-2016, Apple Inc. All rights reserved.
 *
 */

#include "lzfse_internal.h"

/*  Initialize encoder table T[NSYMBOLS].
 *  NSTATES = sum FREQ[i] is the number of states (a power of 2)
 *  NSYMBOLS is the number of symbols.
 *  FREQ[NSYMBOLS] is a normalized histogram of symbol frequencies, with FREQ[i]
 *  >= 0.
 *  Some symbols may have a 0 frequency.  In that case, they should not be
 *  present in the data.
 */
void fse_init_encoder_table(int nstates, int nsymbols,
			    const uint16_t *__restrict freq,
			    fse_encoder_entry *__restrict t)
{
	int offset = 0; // current offset
	int n_clz = __builtin_clz(nstates);
	int i;
	for (i = 0; i < nsymbols; i++) {
		int f = (int)freq[i];
		int k;
		if (f == 0)
			continue; // skip this symbol, no occurrences
		k = __builtin_clz(f) -
		    n_clz; // shift needed to ensure N <= (F<<K) < 2*N
		t[i].s0 = (int16_t)((f << k) - nstates);
		t[i].k = (int16_t)k;
		t[i].delta0 = (int16_t)(offset - f + (nstates >> k));
		t[i].delta1 = (int16_t)(offset - f + (nstates >> (k - 1)));
		offset += f;
	}
}

/*  Initialize decoder table T[NSTATES].
 *  NSTATES = sum FREQ[i] is the number of states (a power of 2)
 *  NSYMBOLS is the number of symbols.
 *  FREQ[NSYMBOLS] is a normalized histogram of symbol frequencies, with FREQ[i]
 *  >= 0.
 *  Some symbols may have a 0 frequency.  In that case, they should not be
 *  present in the data.
 */
int fse_init_decoder_table(int nstates, int nsymbols,
			   const uint16_t *__restrict freq,
			   int32_t *__restrict t)
{
	int n_clz = __builtin_clz(nstates);
	int sum_of_freq = 0;
	int i, j0, j;
	for (i = 0; i < nsymbols; i++) {
		int f = (int)freq[i];
		int k;
		if (f == 0)
			continue; // skip this symbol, no occurrences

		sum_of_freq += f;

		if (sum_of_freq > nstates) {
			return -1;
		}

		k = __builtin_clz(f) -
		    n_clz; // shift needed to ensure N <= (F<<K) < 2*N
		j0 = ((2 * nstates) >> k) - f;

		// Initialize all states S reached by this symbol: OFFSET <= S < OFFSET + F
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

	return 0; // OK
}

/*  Initialize value decoder table T[NSTATES].
 *  NSTATES = sum FREQ[i] is the number of states (a power of 2)
 *  NSYMBOLS is the number of symbols.
 *  FREQ[NSYMBOLS] is a normalized histogram of symbol frequencies, with FREQ[i]
 *  >= 0.
 *  SYMBOL_VBITS[NSYMBOLS] and SYMBOLS_VBASE[NSYMBOLS] are the number of value
 *  bits to read and the base value for each symbol.
 *  Some symbols may have a 0 frequency.  In that case, they should not be
 *  present in the data.
 */
void fse_init_value_decoder_table(int nstates, int nsymbols,
				  const uint16_t *__restrict freq,
				  const uint8_t *__restrict symbol_vbits,
				  const int32_t *__restrict symbol_vbase,
				  fse_value_decoder_entry *__restrict t)
{
	int n_clz = __builtin_clz(nstates);
	int i;
	for (i = 0; i < nsymbols; i++) {
		fse_value_decoder_entry ei = { 0 };
		int f = (int)freq[i];
		int k, j0, j;
		if (f == 0)
			continue; // skip this symbol, no occurrences

		k = __builtin_clz(f) -
		    n_clz; // shift needed to ensure N <= (F<<K) < 2*N
		j0 = ((2 * nstates) >> k) - f;

		ei.value_bits = symbol_vbits[i];
		ei.vbase = symbol_vbase[i];

		// Initialize all states S reached by this symbol: OFFSET <= S < OFFSET + F
		for (j = 0; j < f; j++) {
			fse_value_decoder_entry e = ei;

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

/*  Remove states from symbols until the correct number of states is used.
 */
static void fse_adjust_freqs(uint16_t *freq, int overrun, int nsymbols)
{
	int shift;
	for (shift = 3; overrun != 0; shift--) {
		int sym;
		for (sym = 0; sym < nsymbols; sym++) {
			if (freq[sym] > 1) {
				int n = (freq[sym] - 1) >> shift;
				if (n > overrun)
					n = overrun;
				freq[sym] -= n;
				overrun -= n;
				if (overrun == 0)
					break;
			}
		}
	}
}

/*  Normalize a table T[NSYMBOLS] of occurrences to FREQ[NSYMBOLS].
 */
void fse_normalize_freq(int nstates, int nsymbols, const uint32_t *__restrict t,
			uint16_t *__restrict freq)
{
	uint32_t s_count = 0;
	int remaining = nstates; // must be signed; this may become < 0
	int max_freq = 0;
	int max_freq_sym = 0;
	int shift = __builtin_clz(nstates) - 1;
	uint32_t highprec_step;
	int i;

	// Compute the total number of symbol occurrences
	for (i = 0; i < nsymbols; i++)
		s_count += t[i];

	if (s_count == 0)
		highprec_step = 0; // no symbols used
	else
		highprec_step = ((uint32_t)1 << 31) / s_count;

	for (i = 0; i < nsymbols; i++) {
		// Rescale the occurrence count to get the normalized frequency.
		// Round up if the fractional part is >= 0.5; otherwise round down.
		// For efficiency, we do this calculation using integer arithmetic.
		int f = (((t[i] * highprec_step) >> shift) + 1) >> 1;

		// If a symbol was used, it must be given a nonzero normalized frequency.
		if (f == 0 && t[i] != 0)
			f = 1;

		freq[i] = f;
		remaining -= f;

		// Remember the maximum frequency and which symbol had it.
		if (f > max_freq) {
			max_freq = f;
			max_freq_sym = i;
		}
	}

	// If there remain states to be assigned, then just assign them to the most
	// frequent symbol.  Alternatively, if we assigned more states than were
	// actually available, then either remove states from the most frequent symbol
	// (for minor overruns) or use the slower adjustment algorithm (for major
	// overruns).
	if (-remaining < (max_freq >> 2)) {
		freq[max_freq_sym] += remaining;
	} else {
		fse_adjust_freqs(freq, -remaining, nsymbols);
	}
}
