// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * SIMD dot products for the generic k+m code prototype in raidz_ec.c,
 * in userspace only.  Two ways of multiplying by a constant c of
 * GF(2^8) mod 0x11d:
 *
 * Split nibbles: c * x = c * (x & 0xf) ^ c * (x & 0xf0), each half looked
 * up 16 bytes at a time with PSHUFB, as the existing AVX2 and AVX-512BW
 * RAID-Z code does.  32 bytes of table per coefficient.
 *
 * GFNI: multiplying by c is linear over GF(2), so GF2P8AFFINEQB applies
 * it as an 8x8 bit matrix.  GF2P8MULB is fixed to the AES polynomial
 * 0x11b and cannot be used.  8 bytes of table per coefficient.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "raidz_ec_simd.h"

#if defined(__x86_64__) && defined(__GNUC__)

#include <immintrin.h>

static uint8_t rzec_nib[256][32] __attribute__((aligned(64)));
static uint64_t rzec_aff[256] __attribute__((aligned(64)));

void
rzec_simd_init(const uint8_t mul[256][256])
{
	__builtin_cpu_init();
	for (int c = 0; c < 256; c++) {
		uint64_t a = 0;

		for (int x = 0; x < 16; x++) {
			rzec_nib[c][x] = mul[c][x];
			rzec_nib[c][16 + x] = mul[c][x << 4];
		}
		/*
		 * Bit b of c * x is the parity of x AND row b, where bit i of
		 * row b is bit b of c * 2^i; GF2P8AFFINEQB reads row b from
		 * byte 7 - b of the matrix.
		 */
		for (int b = 0; b < 8; b++) {
			uint64_t row = 0;
			for (int i = 0; i < 8; i++) {
				uint64_t bit = (mul[c][1 << i] >> b) & 1;
				row |= bit << i;
			}
			a |= row << (8 * (7 - b));
		}
		rzec_aff[c] = a;
	}
}

static int
rzec_avx2_available(void)
{
	return (__builtin_cpu_supports("avx2"));
}

static int
rzec_avx512bw_available(void)
{
	return (__builtin_cpu_supports("avx512f") &&
	    __builtin_cpu_supports("avx512bw"));
}

static int
rzec_gfni_available(void)
{
	return (__builtin_cpu_supports("avx2") &&
	    __builtin_cpu_supports("gfni"));
}

static int
rzec_gfni512_available(void)
{
	return (rzec_avx512bw_available() && __builtin_cpu_supports("gfni"));
}

/* AVX2, split nibbles */
#define	RZ_PFX		rzec_avx2
#define	RZ_TGT		__attribute__((target("avx2")))
#define	RZ_V		__m256i
#define	RZ_W		32
#define	RZ_U		1
#define	RZ_TS		32
#define	RZ_LOAD(p)	_mm256_loadu_si256((const __m256i *)(const void *)(p))
#define	RZ_STORE(p, v)	_mm256_storeu_si256((__m256i *)(void *)(p), (v))
#define	RZ_ZERO()	_mm256_setzero_si256()
#define	RZ_XOR(a, b)	_mm256_xor_si256((a), (b))
#define	RZ_PREP(s, lo, hi)	do {					\
	const __m256i m_ = _mm256_set1_epi8(0x0f);			\
	(lo) = _mm256_and_si256((s), m_);				\
	(hi) = _mm256_and_si256(_mm256_srli_epi16((s), 4), m_);		\
} while (0)
#define	RZ_MAC(acc, t, s, lo, hi)	do {				\
	const __m256i tl_ = _mm256_broadcastsi128_si256(		\
	    _mm_loadu_si128((const __m128i *)(const void *)(t)));	\
	const __m256i th_ = _mm256_broadcastsi128_si256(		\
	    _mm_loadu_si128((const __m128i *)(const void *)((t) + 16)));\
	(acc) = _mm256_xor_si256((acc), _mm256_xor_si256(		\
	    _mm256_shuffle_epi8(tl_, (lo)),				\
	    _mm256_shuffle_epi8(th_, (hi))));				\
} while (0)
#include "raidz_ec_simd_impl.h"

/* AVX-512BW, split nibbles, two vectors per output */
#define	RZ_PFX		rzec_avx512bw
#define	RZ_TGT		__attribute__((target("avx512f,avx512bw")))
#define	RZ_V		__m512i
#define	RZ_W		64
#define	RZ_U		2
#define	RZ_TS		32
#define	RZ_LOAD(p)	_mm512_loadu_si512((const void *)(p))
#define	RZ_STORE(p, v)	_mm512_storeu_si512((void *)(p), (v))
#define	RZ_ZERO()	_mm512_setzero_si512()
#define	RZ_XOR(a, b)	_mm512_xor_si512((a), (b))
#define	RZ_PREP(s, lo, hi)	do {					\
	const __m512i m_ = _mm512_set1_epi8(0x0f);			\
	(lo) = _mm512_and_si512((s), m_);				\
	(hi) = _mm512_and_si512(_mm512_srli_epi16((s), 4), m_);		\
} while (0)
#define	RZ_MAC(acc, t, s, lo, hi)	do {				\
	const __m512i tl_ = _mm512_broadcast_i32x4(			\
	    _mm_loadu_si128((const __m128i *)(const void *)(t)));	\
	const __m512i th_ = _mm512_broadcast_i32x4(			\
	    _mm_loadu_si128((const __m128i *)(const void *)((t) + 16)));\
	(acc) = _mm512_ternarylogic_epi64((acc),			\
	    _mm512_shuffle_epi8(tl_, (lo)),				\
	    _mm512_shuffle_epi8(th_, (hi)), 0x96);			\
} while (0)
#include "raidz_ec_simd_impl.h"

/* GFNI with VEX encoding on 256-bit vectors */
#define	RZ_PFX		rzec_gfni
#define	RZ_TGT		__attribute__((target("avx2,gfni")))
#define	RZ_V		__m256i
#define	RZ_W		32
#define	RZ_U		1
#define	RZ_TS		8
#define	RZ_LOAD(p)	_mm256_loadu_si256((const __m256i *)(const void *)(p))
#define	RZ_STORE(p, v)	_mm256_storeu_si256((__m256i *)(void *)(p), (v))
#define	RZ_ZERO()	_mm256_setzero_si256()
#define	RZ_XOR(a, b)	_mm256_xor_si256((a), (b))
#define	RZ_PREP(s, lo, hi)	((lo) = (hi) = (s))
#define	RZ_MAC(acc, t, s, lo, hi)	do {				\
	uint64_t a_;							\
	(void) (lo), (void) (hi);					\
	memcpy(&a_, (t), sizeof (a_));					\
	(acc) = _mm256_xor_si256((acc), _mm256_gf2p8affine_epi64_epi8(	\
	    (s), _mm256_set1_epi64x((long long)a_), 0));		\
} while (0)
#include "raidz_ec_simd_impl.h"

/* GFNI with EVEX encoding on 512-bit vectors, two vectors per output */
#define	RZ_PFX		rzec_gfni512
#define	RZ_TGT		__attribute__((target("avx512f,avx512bw,gfni")))
#define	RZ_V		__m512i
#define	RZ_W		64
#define	RZ_U		2
#define	RZ_TS		8
#define	RZ_LOAD(p)	_mm512_loadu_si512((const void *)(p))
#define	RZ_STORE(p, v)	_mm512_storeu_si512((void *)(p), (v))
#define	RZ_ZERO()	_mm512_setzero_si512()
#define	RZ_XOR(a, b)	_mm512_xor_si512((a), (b))
#define	RZ_PREP(s, lo, hi)	((lo) = (hi) = (s))
#define	RZ_MAC(acc, t, s, lo, hi)	do {				\
	uint64_t a_;							\
	(void) (lo), (void) (hi);					\
	memcpy(&a_, (t), sizeof (a_));					\
	(acc) = _mm512_xor_si512((acc), _mm512_gf2p8affine_epi64_epi8(	\
	    (s), _mm512_set1_epi64((long long)a_), 0));		\
} while (0)
#include "raidz_ec_simd_impl.h"

static const rzec_simd_t rzec_simd_avx2 = {
	"avx2", rzec_avx2_available, &rzec_nib[0][0], 32, 32, rzec_avx2_dot
};
static const rzec_simd_t rzec_simd_avx512bw = {
	"avx512bw", rzec_avx512bw_available, &rzec_nib[0][0], 32, 128,
	rzec_avx512bw_dot
};
static const rzec_simd_t rzec_simd_gfni = {
	"gfni", rzec_gfni_available, (const uint8_t *)rzec_aff, 8, 32,
	rzec_gfni_dot
};
static const rzec_simd_t rzec_simd_gfni512 = {
	"gfni512", rzec_gfni512_available, (const uint8_t *)rzec_aff, 8, 128,
	rzec_gfni512_dot
};

const rzec_simd_t *const rzec_simd_impls[] = {
	&rzec_simd_avx2,
	&rzec_simd_avx512bw,
	&rzec_simd_gfni,
	&rzec_simd_gfni512,
};
const int rzec_simd_nimpls =
    sizeof (rzec_simd_impls) / sizeof (rzec_simd_impls[0]);

#else

void
rzec_simd_init(const uint8_t mul[256][256])
{
	(void) mul;
}

const rzec_simd_t *const rzec_simd_impls[] = { NULL };
const int rzec_simd_nimpls = 0;

#endif
