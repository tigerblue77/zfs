// SPDX-License-Identifier: CDDL-1.0
/*
 * Bench only, not for upstream (issue #16503).
 *
 * XXH3-64 and XXH3-128 as ZFS ABD checksums, in the shape of
 * abd_checksum_blake3_native(): the hash state is fed one ABD chunk at a
 * time through abd_iterate_func(), as every in-tree checksum is, and the
 * result is stored in a zio_cksum_t.  This file is compiled once per
 * xxHash vector path (XXH_VECTOR) with the matching -m flags, and VARIANT
 * names the entry points of that copy.
 */

#define	XXH_INLINE_ALL
#include "xxhash.h"

#include <sys/zfs_context.h>
#include <sys/abd.h>
#include <sys/spa_checksum.h>

#ifndef VARIANT
#error "VARIANT must be defined"
#endif

#define	CAT2(a, b)	a##_##b
#define	CAT(a, b)	CAT2(a, b)
#define	FN(n)		CAT(n, VARIANT)

static int
xxh3_64_incremental(void *buf, size_t len, void *priv)
{
	(void) XXH3_64bits_update(priv, buf, len);
	return (0);
}

static int
xxh3_128_incremental(void *buf, size_t len, void *priv)
{
	(void) XXH3_128bits_update(priv, buf, len);
	return (0);
}

void
FN(abd_checksum_xxh3_64)(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	(void) ctx_template;
	XXH3_state_t st;

	(void) XXH3_64bits_reset(&st);
	(void) abd_iterate_func(abd, 0, size, xxh3_64_incremental, &st);
	ZIO_SET_CHECKSUM(zcp, XXH3_64bits_digest(&st), 0, 0, 0);
}

void
FN(abd_checksum_xxh3_128)(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	(void) ctx_template;
	XXH3_state_t st;
	XXH128_hash_t h;

	(void) XXH3_128bits_reset(&st);
	(void) abd_iterate_func(abd, 0, size, xxh3_128_incremental, &st);
	h = XXH3_128bits_digest(&st);
	ZIO_SET_CHECKSUM(zcp, h.low64, h.high64, 0, 0);
}

/* One-shot hashes of a flat buffer, for the self-check. */
void
FN(xxh3_oneshot)(const void *buf, size_t len, uint64_t out[3])
{
	XXH128_hash_t h = XXH3_128bits(buf, len);

	out[0] = XXH3_64bits(buf, len);
	out[1] = h.low64;
	out[2] = h.high64;
}

/* The vector path this copy was really compiled for. */
const char *
FN(xxh3_vector)(void)
{
	switch (XXH_VECTOR) {
	case XXH_SCALAR:	return ("scalar");
	case XXH_SSE2:		return ("sse2");
	case XXH_AVX2:		return ("avx2");
	case XXH_AVX512:	return ("avx512");
	case XXH_NEON:		return ("neon");
	case XXH_SVE:		return ("sve");
	default:		return ("other");
	}
}
