// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 *
 * CDDL HEADER END
 */

/*
 * LZ4 HC compressor glue.
 *
 * LZ4 HC emits standard LZ4 blocks, so its output is framed exactly as
 * zfs_lz4_compress_buf() frames the fast compressor's output, stored as
 * ZIO_COMPRESS_LZ4, and read back by the existing decoder.
 */

#include <sys/zfs_context.h>
#include <sys/zio_compress.h>

/*
 * Build the upstream library in this translation unit only, with
 * internal linkage for all of it, so that it cannot clash with another
 * copy of LZ4 HC linked into the same kernel or process.
 */
#define	LZ4LIB_VISIBILITY	static
#define	LZ4_PUBLISH_STATIC_FUNCTIONS
#define	LZ4_USER_MEMORY_FUNCTIONS
/* lz4.c and lz4hc.c also define these outside of LZ4LIB_API. */
#define	LZ4_compress_destSize_extState	zfs_LZ4_compress_destSize_extState
#define	LZ4_compress_forceExtDict	zfs_LZ4_compress_forceExtDict
#define	LZ4_decompress_safe_forceExtDict	\
	zfs_LZ4_decompress_safe_forceExtDict
#define	LZ4_decompress_safe_partial_forceExtDict	\
	zfs_LZ4_decompress_safe_partial_forceExtDict
#define	LZ4_loadDict_internal	zfs_LZ4_loadDict_internal
#define	LZ4HC_searchExtDict	zfs_LZ4HC_searchExtDict

static void *LZ4_malloc(size_t s);
static void *LZ4_calloc(size_t n, size_t s);
static void LZ4_free(void *p);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/*
 * The library's assert()s are for its own development and the upstream
 * release builds them out; keep them out whatever <assert.h> said here.
 */
#undef assert
/* lz4hc.c defines its own, equivalent, MIN and MAX. */
#undef MIN
#undef MAX
/* All of lz4.c, so that every function lz4hc.c calls is defined here. */
#include "lib/lz4.c"
#include "lib/lz4hc.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static kmem_cache_t *lz4hc_cache;

/*
 * With the state supplied by the caller, the only allocation the library
 * makes is the ~64 KiB match table of the optimal parser (levels 10 to
 * 12). It fails gracefully: the block is then written uncompressed.
 */
static void *
LZ4_malloc(size_t s)
{
	uint64_t *p = vmem_alloc(s + sizeof (uint64_t), KM_NOSLEEP);

	if (p == NULL)
		return (NULL);
	*p = s + sizeof (uint64_t);
	return (p + 1);
}

static void *
LZ4_calloc(size_t n, size_t s)
{
	uint64_t *p;

	if (s != 0 && n > (SIZE_MAX - sizeof (uint64_t)) / s)
		return (NULL);
	p = vmem_zalloc(n * s + sizeof (uint64_t), KM_NOSLEEP);
	if (p == NULL)
		return (NULL);
	*p = n * s + sizeof (uint64_t);
	return (p + 1);
}

static void
LZ4_free(void *p)
{
	uint64_t *h = p;

	if (h == NULL)
		return;
	h--;
	vmem_free(h, *h);
}

size_t
zfs_lz4hc_compress_buf(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int level)
{
	uint32_t bufsiz;
	char *dest = d_start;
	void *state;

	ASSERT3U(d_len, >=, sizeof (bufsiz));
	ASSERT3S(level, >=, ZIO_LZ4HC_LEVEL_MIN);
	ASSERT3S(level, <=, ZIO_LZ4HC_LEVEL_MAX);

	state = kmem_cache_alloc(lz4hc_cache, KM_SLEEP);
	bufsiz = LZ4_compress_HC_extStateHC(state, s_start,
	    &dest[sizeof (bufsiz)], s_len, d_len - sizeof (bufsiz), level);
	kmem_cache_free(lz4hc_cache, state);

	if (bufsiz == 0)
		return (s_len);

	/* Same framing as zfs_lz4_compress_buf(): big-endian length. */
	*(uint32_t *)dest = BE_32(bufsiz);

	return (bufsiz + sizeof (bufsiz));
}

void
lz4hc_init(void)
{
	lz4hc_cache = kmem_cache_create("lz4hc_cache",
	    sizeof (LZ4_streamHC_t), 0, NULL, NULL, NULL, NULL, NULL,
	    KMC_RECLAIMABLE);
}

void
lz4hc_fini(void)
{
	if (lz4hc_cache) {
		kmem_cache_destroy(lz4hc_cache);
		lz4hc_cache = NULL;
	}
}
