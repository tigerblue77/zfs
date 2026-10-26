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
 * Unit tests for the LZJB decompressor (module/zfs/lzjb.c), which must stop
 * reading at the end of the source buffer rather than at the point where it
 * has produced d_len output bytes.
 */

#include <sys/zfs_context.h>
#include <sys/abd.h>
#include <sys/zio_compress.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "unit.h"

/* ========== */

/*
 * The compression wrappers only borrow and return linear buffers, so a
 * mock ABD is a buffer and its size.
 */
typedef struct mock_abd {
	void	*ma_buf;
	size_t	ma_size;
} mock_abd_t;

void *
abd_borrow_buf(abd_t *abd, size_t n)
{
	mock_abd_t *ma = (mock_abd_t *)abd;
	unit_le(n, ma->ma_size);
	return (ma->ma_buf);
}

void *
abd_borrow_buf_copy(abd_t *abd, size_t n)
{
	return (abd_borrow_buf(abd, n));
}

void
abd_return_buf(abd_t *abd, void *buf, size_t n)
{
	(void) abd, (void) buf, (void) n;
}

void
abd_return_buf_copy(abd_t *abd, void *buf, size_t n)
{
	(void) abd, (void) buf, (void) n;
}

/*
 * Place len bytes at the very end of a page whose successor is unmapped,
 * so that any read past them faults.
 */
static uint8_t *
guarded_alloc(size_t len)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unit_true(p != MAP_FAILED);
	unit_ok(mprotect(p + pg, pg, PROT_NONE));
	return (p + pg - len);
}

static void
guarded_free(uint8_t *buf, size_t len)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	unit_ok(munmap(buf + len - pg, 2 * pg));
}

static int
decompress(uint8_t *src, size_t s_len, uint8_t *dst, size_t d_len)
{
	mock_abd_t sabd = { src, s_len };
	mock_abd_t dabd = { dst, d_len };
	return (zfs_lzjb_decompress((abd_t *)&sabd, (abd_t *)&dabd,
	    s_len, d_len, 0));
}

#define	BLKSZ	4096

static uint8_t lbuf[BLKSZ], cbuf[BLKSZ], obuf[BLKSZ];

static size_t
compress_block(void)
{
	/* Compressible, with runs so the output uses match tokens. */
	for (size_t i = 0; i < BLKSZ; i++)
		lbuf[i] = (uint8_t)((i / 17) & 0x0f);
	mock_abd_t sabd = { lbuf, BLKSZ };
	mock_abd_t dabd = { cbuf, BLKSZ };
	abd_t *dp = (abd_t *)&dabd;
	size_t c_len = zfs_lzjb_compress((abd_t *)&sabd, dp, BLKSZ,
	    BLKSZ, 0);
	unit_lt(c_len, BLKSZ);
	return (c_len);
}

/* ========== */

/*
 * A source that runs out before d_len output bytes have been produced is
 * rejected without reading past its end.  A source of all-literal groups
 * (copymap bytes 0) encodes 8 output bytes per 9 input bytes, so any source
 * far shorter than d_len cannot fill the output.
 */
static MunitResult
test_lzjb_truncated_source(const MunitParameter params[], void *data)
{
	(void) data;
	size_t s_len = strtoul(munit_parameters_get(params, "s_len"),
	    NULL, 10);

	uint8_t *src = guarded_alloc(s_len);
	memset(src, 0, s_len);		/* copymap 0: literals only */
	unit_ne(decompress(src, s_len, obuf, BLKSZ), 0);
	guarded_free(src, s_len);

	return (MUNIT_OK);
}

/*
 * A match token at the very end of the source whose second byte is missing
 * must be rejected, not read past the end.
 */
static MunitResult
test_lzjb_truncated_match(const MunitParameter params[], void *data)
{
	(void) params, (void) data;

	/* copymap with bit 0 set (a match), then a single match byte. */
	uint8_t *src = guarded_alloc(2);
	src[0] = 0x01;
	src[1] = 0x00;
	unit_ne(decompress(src, 2, obuf, BLKSZ), 0);
	guarded_free(src, 2);

	return (MUNIT_OK);
}

/* A block decompresses from exactly the bytes the compressor produced. */
static MunitResult
test_lzjb_roundtrip(const MunitParameter params[], void *data)
{
	(void) params, (void) data;
	size_t c_len = compress_block();

	uint8_t *src = guarded_alloc(c_len);
	memcpy(src, cbuf, c_len);
	memset(obuf, 0xa5, BLKSZ);
	unit_ok(decompress(src, c_len, obuf, BLKSZ));
	unit_ok(memcmp(obuf, lbuf, BLKSZ));
	guarded_free(src, c_len);

	return (MUNIT_OK);
}

/* ========== */

static const MunitParameterEnum truncated_params[] = {
	UNIT_PARAM("s_len", "1", "2", "9", "64"),
	{ 0 },
};

static const MunitTest lzjb_tests[] = {
	UNIT_TEST("truncated_source",	test_lzjb_truncated_source,
	    truncated_params),
	UNIT_TEST("truncated_match",	test_lzjb_truncated_match),
	UNIT_TEST("roundtrip",		test_lzjb_roundtrip),
	{ 0 },
};

static const MunitSuite lzjb_test_suite = {
	"lzjb.",
	lzjb_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE,
};

int
main(int argc, char **argv)
{
	return (munit_suite_main(&lzjb_test_suite, NULL, argc, argv));
}
