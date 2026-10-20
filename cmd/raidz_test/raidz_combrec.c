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
 * Userspace model of combinatorial reconstruction for the k+m code of
 * raidz_ec.c.  Only raidz_test uses it.
 *
 * When a block fails its checksum and no child reported an error,
 * vdev_raidz_combrec() assumes every set of 1 .. m children bad in turn,
 * rebuilds them and checksums the block, up to sum_s C(n, s) attempts.
 * cr_combrec() reproduces that order, the cost of each attempt and the
 * attribution of errors on a single-row map.
 *
 * cr_search() finds the same answers from the syndromes instead.  With H
 * the m x n parity check matrix [I | A] and w the columns as read (those
 * that reported an error counted as zero), S = H w = H_X x, where X holds
 * the bad columns and x their errors.  Every m columns of H are
 * independent, so when |X| < m and the errors span |X| dimensions, the
 * bad columns are exactly those c with h_c in the span V of the syndromes
 * at all offsets: one decode and one checksum.  Otherwise the hypotheses
 * left are tested on V alone, and the sets of m columns, which V cannot
 * tell apart, are tried last.  Every answer is still accepted only on the
 * block checksum, so the search order changes the cost and never the
 * result.  A zero syndrome outside the reported errors proves that no
 * set of m columns can repair the block, without any checksum.
 */

#include <sys/zfs_context.h>
#include <sys/time.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/abd.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/sha2.h>
#include <sys/blake3.h>
#include <zfs_fletcher.h>
#include <stdio.h>
#include <math.h>
#include "raidz_test.h"

#define	CR_MAXCOLS	256
#define	CR_WORDS	(CR_MAXCOLS / 64)
#define	CR_SCAN		4096	/* bytes of residual per pass */
#define	CR_ALIGN	128	/* a multiple of every SIMD step */
#define	CR_MAXTRIED	64	/* failed consistent sets kept for pruning */
#define	CR_SAMPLE_NSEC	(150ULL * MICROSEC)

typedef enum cr_cksum {
	CR_FLETCHER4,
	CR_SHA256,
	CR_BLAKE3,
} cr_cksum_t;

typedef enum cr_result {
	CR_RECOVERED,
	CR_UNRECOVERABLE,	/* every hypothesis failed, or proof */
	CR_BUDGET,		/* stopped by the checksum budget */
} cr_result_t;

typedef enum cr_damage {
	CR_D_WHOLE,	/* every byte of the column random */
	CR_D_SECTOR,	/* one to three random sectors */
	CR_D_FLIP,	/* one random bit */
	CR_D_SAMEOFF,	/* the same bit of the same byte in every column */
	CR_D_DUP,	/* the same random pattern XORed into every column */
	CR_D_NKINDS,
} cr_damage_t;

static const char *const cr_damage_names[CR_D_NKINDS] = {
	"whole", "sector", "flip", "sameoff", "dup",
};

static const char *const cr_result_names[] = {
	"recovered", "unrecoverable", "budget",
};

typedef struct cr_set {
	uint64_t	w[CR_WORDS];
} cr_set_t;

typedef struct cr_stats {
	uint64_t	cs_cksums;	/* candidate checksums */
	uint64_t	cs_verify;	/* full checksums confirming one */
	uint64_t	cs_decodes;	/* candidate reconstructions */
	uint64_t	cs_tests;	/* hypotheses tested on V alone */
	int		cs_rank;	/* of V plus the reported errors */
	int		cs_located;	/* columns located from V */
	int		cs_suspects;	/* columns located sector by sector */
	hrtime_t	cs_prep_ns;	/* before the first candidate */
	hrtime_t	cs_ns;
} cr_stats_t;

typedef struct cr_blk {
	zio_t		cb_zio;
	raidz_map_t	*cb_rm;
	raidz_row_t	*cb_rr;
	rzec_code_t	cb_code;
	boolean_t	cb_prod;	/* baseline rebuilds with the module */
	int		cb_m;
	int		cb_k;
	int		cb_n;		/* columns of the row */
	int		cb_width;	/* children of the vdev */
	int		cb_ashift;
	size_t		cb_psize;
	uint8_t		*cb_block;
	uint8_t		*cb_buf[CR_MAXCOLS];
	size_t		cb_size[CR_MAXCOLS];
	uint8_t		*cb_gold[CR_MAXCOLS];
	uint8_t		*cb_read[CR_MAXCOLS];	/* as read, damage included */
	boolean_t	cb_known[CR_MAXCOLS];	/* reported an error */
	boolean_t	cb_hint[CR_MAXCOLS];	/* e.g. DTL, error history */
	cr_set_t	cb_truth;
	size_t		cb_dmin[CR_MAXCOLS];	/* first damaged byte */
	cr_set_t	cb_bad;		/* attributed by the last success */
	uint8_t		cb_h[RZEC_MAXPARITY][CR_MAXCOLS];
	zio_cksum_t	cb_want;
	cr_cksum_t	cb_ck;
	int		cb_bits;	/* bits compared, 256 = all */
	uint64_t	cb_budget;	/* candidate checksums, 0 = none */
	uint64_t	cb_test_budget;
	uint8_t		*cb_syn[RZEC_MAXPARITY];
	uint8_t		*cb_res[RZEC_MAXPARITY];
	uint8_t		*cb_cand[RZEC_MAXPARITY];
	boolean_t	cb_prepared;
	zio_cksum_t	cb_part[CR_MAXCOLS];	/* fletcher4 of each column */
	uint8_t		*cb_prefix;	/* hash state before each column */
	size_t		cb_ctxsize;
	uint64_t	cb_rng;
	const uint8_t	*cb_src[CR_MAXCOLS];	/* rzec_lincomb() scratch */
	size_t		cb_slen[CR_MAXCOLS];
} cr_blk_t;

static void *cr_blake3_tmpl;

/*
 * Small helpers
 */

static inline uint64_t
cr_rand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return (x);
}

static void
cr_fill(uint64_t *rng, uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i += 8) {
		uint64_t v = cr_rand(rng);
		memcpy(p + i, &v, MIN(8, n - i));
	}
}

static inline void
cr_set_add(cr_set_t *s, int c)
{
	s->w[c / 64] |= 1ULL << (c % 64);
}

static inline boolean_t
cr_set_has(const cr_set_t *s, int c)
{
	return ((s->w[c / 64] >> (c % 64)) & 1);
}

static inline boolean_t
cr_set_eq(const cr_set_t *a, const cr_set_t *b)
{
	return (memcmp(a, b, sizeof (*a)) == 0);
}

/* Whether a is a subset of b. */
static inline boolean_t
cr_set_sub(const cr_set_t *a, const cr_set_t *b)
{
	for (int i = 0; i < CR_WORDS; i++)
		if (a->w[i] & ~b->w[i])
			return (B_FALSE);
	return (B_TRUE);
}

static uint64_t
cr_binom(int n, int k)
{
	double r = 1;

	if (k < 0 || k > n)
		return (0);
	for (int i = 1; i <= k; i++)
		r = r * (n - k + i) / i;
	return ((uint64_t)(r + 0.5));
}

/* First combination of k of n in lexicographic order. */
static void
cr_comb_first(int *t, int k)
{
	for (int i = 0; i < k; i++)
		t[i] = i;
}

static boolean_t
cr_comb_next(int *t, int k, int n)
{
	int i = k - 1;

	while (i >= 0 && t[i] == n - k + i)
		i--;
	if (i < 0)
		return (B_FALSE);
	t[i]++;
	for (int j = i + 1; j < k; j++)
		t[j] = t[j - 1] + 1;
	return (B_TRUE);
}

/*
 * GF(2^8) linear algebra on vectors of m bytes
 */

/* Rank of the m x nc matrix a (by columns), which is destroyed. */
static int
cr_rank(uint8_t a[][RZEC_MAXPARITY], int nc, int m)
{
	int r = 0;

	for (int row = 0; row < m && r < nc; row++) {
		int p = r;
		while (p < nc && a[p][row] == 0)
			p++;
		if (p == nc)
			continue;
		for (int i = 0; i < m; i++) {
			uint8_t t = a[r][i];
			a[r][i] = a[p][i];
			a[p][i] = t;
		}
		uint8_t inv = gf_inv(a[r][row]);
		for (int c = r + 1; c < nc; c++) {
			uint8_t f = gf_mul(a[c][row], inv);
			if (f == 0)
				continue;
			for (int i = 0; i < m; i++)
				a[c][i] ^= gf_mul(f, a[r][i]);
		}
		r++;
	}
	return (r);
}

/*
 * Rows y of n (m - r of them) with y . b = 0 for the r independent
 * vectors b of basis, i.e. the equations of their span.
 */
static void
cr_nullspace(const uint8_t basis[][RZEC_MAXPARITY], int r, int m,
    uint8_t n[][RZEC_MAXPARITY])
{
	uint8_t a[RZEC_MAXPARITY][RZEC_MAXPARITY];
	int piv[RZEC_MAXPARITY], isp[RZEC_MAXPARITY] = { 0 };
	int rows = 0;

	memcpy(a, basis, r * sizeof (a[0]));
	for (int col = 0; col < m && rows < r; col++) {
		int p = rows;
		while (p < r && a[p][col] == 0)
			p++;
		if (p == r)
			continue;
		for (int i = 0; i < m; i++) {
			uint8_t t = a[rows][i];
			a[rows][i] = a[p][i];
			a[p][i] = t;
		}
		uint8_t inv = gf_inv(a[rows][col]);
		for (int i = 0; i < m; i++)
			a[rows][i] = gf_mul(a[rows][i], inv);
		for (int q = 0; q < r; q++) {
			uint8_t f = a[q][col];
			if (q == rows || f == 0)
				continue;
			for (int i = 0; i < m; i++)
				a[q][i] ^= gf_mul(f, a[rows][i]);
		}
		piv[rows++] = col;
		isp[col] = 1;
	}
	VERIFY3S(rows, ==, r);

	int nn = 0;
	for (int f = 0; f < m; f++) {
		if (isp[f])
			continue;
		memset(n[nn], 0, m);
		n[nn][f] = 1;
		for (int q = 0; q < r; q++)
			n[nn][piv[q]] = a[q][f];
		nn++;
	}
	VERIFY3S(nn, ==, m - r);
}

static boolean_t
cr_in_span(const uint8_t n[][RZEC_MAXPARITY], int nn, int m,
    const uint8_t *v)
{
	for (int d = 0; d < nn; d++) {
		uint8_t s = 0;
		for (int i = 0; i < m; i++)
			s ^= gf_mul(n[d][i], v[i]);
		if (s != 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * Checksums
 */

static void
cr_cksum_full(cr_blk_t *cb, zio_cksum_t *zc)
{
	abd_t *abd = cb->cb_zio.io_abd;
	uint64_t size = cb->cb_zio.io_size;

	switch (cb->cb_ck) {
	case CR_FLETCHER4:
		abd_fletcher_4_native(abd, size, NULL, zc);
		break;
	case CR_SHA256:
		abd_checksum_sha256(abd, size, NULL, zc);
		break;
	case CR_BLAKE3:
		abd_checksum_blake3_native(abd, size, cr_blake3_tmpl, zc);
		break;
	}
}

static boolean_t
cr_cksum_eq(const cr_blk_t *cb, const zio_cksum_t *a)
{
	const zio_cksum_t *b = &cb->cb_want;

	if (cb->cb_bits >= 256)
		return (ZIO_CHECKSUM_EQUAL(*a, *b));
	return (((a->zc_word[0] ^ b->zc_word[0]) &
	    ((1ULL << cb->cb_bits) - 1)) == 0);
}

/* As fletcher_4_incremental_combine() in zfs_fletcher.c. */
static void
cr_fletcher_combine(zio_cksum_t *zcp, uint64_t size, const zio_cksum_t *nzcp)
{
	const uint64_t c1 = size / sizeof (uint32_t);
	const uint64_t c2 = c1 * (c1 + 1) / 2;
	const uint64_t c3 = c2 * (c1 + 2) / 3;

	zcp->zc_word[3] += nzcp->zc_word[3] + c1 * zcp->zc_word[2] +
	    c2 * zcp->zc_word[1] + c3 * zcp->zc_word[0];
	zcp->zc_word[2] += nzcp->zc_word[2] + c1 * zcp->zc_word[1] +
	    c2 * zcp->zc_word[0];
	zcp->zc_word[1] += nzcp->zc_word[1] + c1 * zcp->zc_word[0];
	zcp->zc_word[0] += nzcp->zc_word[0];
}

static size_t
cr_ctx_size(cr_cksum_t ck)
{
	return (ck == CR_SHA256 ? sizeof (SHA2_CTX) :
	    ck == CR_BLAKE3 ? sizeof (BLAKE3_CTX) : 0);
}

static void
cr_ctx_init(cr_blk_t *cb, void *ctx)
{
	if (cb->cb_ck == CR_SHA256)
		SHA2Init(SHA256, ctx);
	else
		memcpy(ctx, cr_blake3_tmpl, sizeof (BLAKE3_CTX));
}

static void
cr_ctx_update(cr_blk_t *cb, void *ctx, const uint8_t *p, size_t n)
{
	if (cb->cb_ck == CR_SHA256)
		SHA2Update(ctx, p, n);
	else
		Blake3_Update(ctx, p, n);
}

static void
cr_ctx_final(cr_blk_t *cb, void *ctx, zio_cksum_t *zc)
{
	if (cb->cb_ck == CR_SHA256) {
		zio_cksum_t tmp;
		SHA2Final(&tmp, ctx);
		for (int i = 0; i < 4; i++)
			zc->zc_word[i] = BE_64(tmp.zc_word[i]);
	} else {
		Blake3_Final(ctx, (uint8_t *)zc);
	}
}

/*
 * Per-column state for checksumming a candidate that differs from the
 * block as read in a few data columns only: the fletcher4 of each column
 * (fletcher4 is linear, so partial sums combine), or for a hash the
 * state before each column, so that only the suffix from the first
 * changed column is hashed again.
 */
static void
cr_prepare(cr_blk_t *cb)
{
	const int m = cb->cb_m;

	if (cb->cb_prepared)
		return;
	cb->cb_prepared = B_TRUE;
	if (cb->cb_ck == CR_FLETCHER4) {
		for (int c = m; c < cb->cb_n; c++)
			fletcher_4_native(cb->cb_buf[c], cb->cb_size[c], NULL,
			    &cb->cb_part[c]);
		return;
	}
	uint8_t *ctx = cb->cb_prefix;
	cr_ctx_init(cb, ctx);
	for (int c = m; c < cb->cb_n; c++) {
		uint8_t *next = cb->cb_prefix + (c - m + 1) * cb->cb_ctxsize;
		memcpy(next, ctx, cb->cb_ctxsize);
		cr_ctx_update(cb, next, cb->cb_buf[c], cb->cb_size[c]);
		ctx = next;
	}
}

/* Checksum of the block with column dc[i] replaced by cand[i]. */
static void
cr_cksum_cand(cr_blk_t *cb, const int *dc, uint8_t *const *cand, int nd,
    zio_cksum_t *zc)
{
	const int m = cb->cb_m;
	int first = cb->cb_n;

	cr_prepare(cb);
	for (int i = 0; i < nd; i++)
		first = MIN(first, dc[i]);

	if (cb->cb_ck == CR_FLETCHER4) {
		ZIO_SET_CHECKSUM(zc, 0, 0, 0, 0);
		for (int c = m; c < cb->cb_n; c++) {
			zio_cksum_t part = cb->cb_part[c];
			for (int i = 0; i < nd; i++)
				if (dc[i] == c)
					fletcher_4_native(cand[i],
					    cb->cb_size[c], NULL, &part);
			cr_fletcher_combine(zc, cb->cb_size[c], &part);
		}
		return;
	}

	uint64_t ctxbuf[sizeof (BLAKE3_CTX) / 8 + 1];
	void *ctx = ctxbuf;
	memcpy(ctx, cb->cb_prefix + (first - m) * cb->cb_ctxsize,
	    cr_ctx_size(cb->cb_ck));
	for (int c = first; c < cb->cb_n; c++) {
		const uint8_t *p = cb->cb_buf[c];
		for (int i = 0; i < nd; i++)
			if (dc[i] == c)
				p = cand[i];
		cr_ctx_update(cb, ctx, p, cb->cb_size[c]);
	}
	cr_ctx_final(cb, ctx, zc);
}

/*
 * The block
 */

static int cr_cksum_bits = 256;
static uint64_t cr_budget;
static uint64_t cr_test_budget = 1ULL << 22;
static cr_cksum_t cr_ck = CR_FLETCHER4;
static const char *cr_prod_impl;

static cr_blk_t *
cr_blk_alloc(rzec_code_t code, int m, int width, size_t size, int ashift,
    uint64_t seed)
{
	cr_blk_t *cb = kmem_zalloc(sizeof (*cb), KM_SLEEP);
	raidz_row_t *rr;

	cb->cb_code = code;
	cb->cb_m = m;
	cb->cb_width = width;
	cb->cb_ashift = ashift;
	cb->cb_ck = cr_ck;
	cb->cb_bits = cr_cksum_bits;
	cb->cb_budget = cr_budget;
	cb->cb_test_budget = cr_test_budget;
	cb->cb_rng = seed * 0x9E3779B97F4A7C15ULL + 1;
	cb->cb_prod = (cr_prod_impl != NULL && code == RZEC_VAND &&
	    m <= PARITY_PQR);

	cb->cb_zio.io_offset = 0;
	cb->cb_zio.io_size = size;
	cb->cb_zio.io_abd = abd_alloc_linear(size, B_FALSE);
	cb->cb_block = abd_to_buf(cb->cb_zio.io_abd);
	cr_fill(&cb->cb_rng, cb->cb_block, size);

	cb->cb_rm = vdev_raidz_map_alloc(&cb->cb_zio, ashift, width, m);
	VERIFY3S(cb->cb_rm->rm_nrows, ==, 1);
	rr = cb->cb_rr = cb->cb_rm->rm_row[0];
	cb->cb_n = rr->rr_cols;
	cb->cb_k = rr->rr_cols - m;
	cb->cb_psize = rr->rr_col[0].rc_size;
	VERIFY3S(rr->rr_firstdatacol, ==, m);
	VERIFY3S(cb->cb_n, <=, width);

	if (cb->cb_prod) {
		VERIFY0(vdev_raidz_impl_set(cr_prod_impl));
		vdev_raidz_generate_parity(cb->cb_rm);
	} else {
		rzec_generate(cb->cb_rm, code);
	}

	size_t off = 0;
	for (int c = 0; c < cb->cb_n; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		VERIFY(abd_is_linear(rc->rc_abd));
		VERIFY3U(rc->rc_size, >, 0);
		cb->cb_buf[c] = abd_to_buf(rc->rc_abd);
		cb->cb_size[c] = rc->rc_size;
		cb->cb_gold[c] = kmem_alloc(rc->rc_size, KM_SLEEP);
		cb->cb_read[c] = kmem_alloc(rc->rc_size, KM_SLEEP);
		memcpy(cb->cb_gold[c], cb->cb_buf[c], rc->rc_size);
		memcpy(cb->cb_read[c], cb->cb_buf[c], rc->rc_size);
		if (c >= m) {
			/* The incremental checksums rely on this layout. */
			VERIFY3P(cb->cb_buf[c], ==, cb->cb_block + off);
			off += rc->rc_size;
		}
		for (int r = 0; r < m; r++)
			cb->cb_h[r][c] = c < m ? (r == c) :
			    rzec_coefficient(code, r, c - m, cb->cb_k);
	}
	VERIFY3U(off, ==, size);

	for (int r = 0; r < m; r++) {
		cb->cb_syn[r] = kmem_alloc(cb->cb_psize, KM_SLEEP);
		cb->cb_cand[r] = kmem_alloc(cb->cb_psize, KM_SLEEP);
		cb->cb_res[r] = kmem_alloc(CR_SCAN, KM_SLEEP);
	}
	cb->cb_ctxsize = P2ROUNDUP(cr_ctx_size(cb->cb_ck), 64);
	if (cb->cb_ctxsize != 0)
		cb->cb_prefix = kmem_alloc((cb->cb_k + 1) * cb->cb_ctxsize,
		    KM_SLEEP);
	cr_cksum_full(cb, &cb->cb_want);
	return (cb);
}

static void
cr_blk_free(cr_blk_t *cb)
{
	for (int c = 0; c < cb->cb_n; c++) {
		kmem_free(cb->cb_gold[c], cb->cb_size[c]);
		kmem_free(cb->cb_read[c], cb->cb_size[c]);
	}
	for (int r = 0; r < cb->cb_m; r++) {
		kmem_free(cb->cb_syn[r], cb->cb_psize);
		kmem_free(cb->cb_cand[r], cb->cb_psize);
		kmem_free(cb->cb_res[r], CR_SCAN);
	}
	if (cb->cb_prefix != NULL)
		kmem_free(cb->cb_prefix, (cb->cb_k + 1) * cb->cb_ctxsize);
	vdev_raidz_map_free(cb->cb_rm);
	abd_free(cb->cb_zio.io_abd);
	kmem_free(cb, sizeof (*cb));
}

/* Back to the golden state: no damage, no reported error. */
static void
cr_blk_reset(cr_blk_t *cb)
{
	for (int c = 0; c < cb->cb_n; c++) {
		memcpy(cb->cb_buf[c], cb->cb_gold[c], cb->cb_size[c]);
		cb->cb_known[c] = B_FALSE;
		cb->cb_hint[c] = B_FALSE;
		cb->cb_rr->rr_col[c].rc_error = 0;
	}
	memset(&cb->cb_truth, 0, sizeof (cb->cb_truth));
}

/* Back to the block as read, before a search. */
static void
cr_blk_reread(cr_blk_t *cb)
{
	for (int c = 0; c < cb->cb_n; c++)
		memcpy(cb->cb_buf[c], cb->cb_read[c], cb->cb_size[c]);
	memset(&cb->cb_bad, 0, sizeof (cb->cb_bad));
	cb->cb_prepared = B_FALSE;
}

/*
 * Damage the columns cols[] silently, and make the columns known[] report
 * an error with garbage in their buffer.  Then record the block as read.
 */
static void
cr_damage(cr_blk_t *cb, const int *cols, int nt, cr_damage_t kind,
    const int *known, int nk)
{
	uint64_t *rng = &cb->cb_rng;
	size_t minsize = cb->cb_psize;
	uint8_t *pat = NULL;

	for (int i = 0; i < nt; i++)
		minsize = MIN(minsize, cb->cb_size[cols[i]]);
	size_t off = (cr_rand(rng) % minsize);
	int bit = cr_rand(rng) % 8;
	if (kind == CR_D_DUP) {
		pat = kmem_alloc(cb->cb_psize, KM_SLEEP);
		do {
			cr_fill(rng, pat, cb->cb_psize);
		} while (pat[0] == 0);
	}

	for (int i = 0; i < nt; i++) {
		int c = cols[i];
		uint8_t *p = cb->cb_buf[c];
		size_t sz = cb->cb_size[c];
		size_t sec = 1ULL << cb->cb_ashift, nsec = sz / sec;

		switch (kind) {
		case CR_D_WHOLE:
			cr_fill(rng, p, sz);
			break;
		case CR_D_SECTOR: {
			int ns = 1 + cr_rand(rng) % MIN(3, nsec);
			for (int s = 0; s < ns; s++)
				cr_fill(rng, p + (cr_rand(rng) % nsec) * sec,
				    sec);
			break;
		}
		case CR_D_FLIP:
			p[cr_rand(rng) % sz] ^= 1 << (cr_rand(rng) % 8);
			break;
		case CR_D_SAMEOFF:
			p[off] ^= 1 << bit;
			break;
		case CR_D_DUP:
			for (size_t b = 0; b < sz; b++)
				p[b] ^= pat[b];
			break;
		default:
			VERIFY(0);
		}
		/* A random fill can, in theory, rewrite the same bytes. */
		if (memcmp(p, cb->cb_gold[c], sz) == 0)
			p[0] ^= 1;
		cr_set_add(&cb->cb_truth, c);
	}
	if (pat != NULL)
		kmem_free(pat, cb->cb_psize);

	for (int i = 0; i < nk; i++) {
		int c = known[i];
		cb->cb_known[c] = B_TRUE;
		cb->cb_rr->rr_col[c].rc_error = SET_ERROR(EIO);
		memset(cb->cb_buf[c], 0xAA, cb->cb_size[c]);
		cr_set_add(&cb->cb_truth, c);
	}
	for (int c = 0; c < cb->cb_n; c++) {
		size_t o = 0;
		while (o < cb->cb_size[c] && cb->cb_buf[c][o] ==
		    cb->cb_gold[c][o])
			o++;
		cb->cb_dmin[c] = o;
		memcpy(cb->cb_read[c], cb->cb_buf[c], cb->cb_size[c]);
	}
}

/* Data columns equal to the golden copy. */
static boolean_t
cr_data_ok(const cr_blk_t *cb)
{
	for (int c = cb->cb_m; c < cb->cb_n; c++)
		if (memcmp(cb->cb_buf[c], cb->cb_gold[c], cb->cb_size[c]) != 0)
			return (B_FALSE);
	return (B_TRUE);
}

/*
 * Regenerate the parity from the data as repaired and blame each parity
 * column that read otherwise (raidz_parity_verify()).
 */
static void
cr_blame_parity(cr_blk_t *cb)
{
	const int m = cb->cb_m;
	const uint8_t **src = cb->cb_src;
	size_t *slen = cb->cb_slen, dlen[RZEC_MAXPARITY];
	uint8_t *coef = kmem_alloc(m * CR_MAXCOLS, KM_SLEEP);
	int ns = 0;

	for (int c = m; c < cb->cb_n; c++) {
		src[ns] = cb->cb_buf[c];
		slen[ns] = cb->cb_size[c];
		for (int r = 0; r < m; r++)
			coef[r * cb->cb_k + ns] = cb->cb_h[r][c];
		ns++;
	}
	for (int r = 0; r < m; r++)
		dlen[r] = cb->cb_psize;
	rzec_lincomb(m, cb->cb_cand, dlen, ns, src, slen, coef);
	for (int r = 0; r < m; r++)
		if (!cb->cb_known[r] &&
		    memcmp(cb->cb_cand[r], cb->cb_buf[r], cb->cb_psize) != 0)
			cr_set_add(&cb->cb_bad, r);
	kmem_free(coef, m * CR_MAXCOLS);
}

/*
 * Baseline: vdev_raidz_combrec() and raidz_reconstruct()
 */

/* 0 on success, EINVAL if too many columns would be lost, else ECKSUM. */
static int
cr_combrec_try(cr_blk_t *cb, const int *ltgts, int ntgts, cr_stats_t *st)
{
	raidz_row_t *rr = cb->cb_rr;
	const int m = cb->cb_m;
	int sim[RZEC_MAXPARITY + 1], nsim = 0;
	int tg[2 * RZEC_MAXPARITY], ntg = 0;
	int dead = 0, dead_data = 0, sim_data = 0;
	zio_cksum_t zc;

	for (int c = 0; c < cb->cb_n; c++) {
		if (cb->cb_known[c]) {
			dead++;
			if (c >= m)
				dead_data++;
			tg[ntg++] = c;
			continue;
		}
		for (int lt = 0; lt < ntgts; lt++) {
			if (rr->rr_col[c].rc_devidx != ltgts[lt])
				continue;
			dead++;
			if (c >= m) {
				dead_data++;
				sim_data++;
			}
			if (nsim < RZEC_MAXPARITY)
				sim[nsim++] = c;
			tg[ntg++] = c;
			break;
		}
	}
	if (dead > m)
		return (EINVAL);

	if (dead_data > 0) {
		if (cb->cb_prod)
			vdev_raidz_reconstruct(cb->cb_rm, sim, nsim);
		else
			VERIFY0(rzec_reconstruct_data(cb->cb_rm, cb->cb_code,
			    tg, ntg));
		st->cs_decodes++;
	}
	cr_cksum_full(cb, &zc);
	st->cs_cksums++;

	if (cr_cksum_eq(cb, &zc)) {
		int parity_errors = 0, data_errors = sim_data;
		memset(&cb->cb_bad, 0, sizeof (cb->cb_bad));
		for (int c = 0; c < cb->cb_n; c++) {
			if (!cb->cb_known[c])
				continue;
			cr_set_add(&cb->cb_bad, c);
			if (c < m)
				parity_errors++;
			else
				data_errors++;
		}
		for (int i = 0; i < nsim; i++)
			if (sim[i] >= m)
				cr_set_add(&cb->cb_bad, sim[i]);
		if (parity_errors < m - data_errors)
			cr_blame_parity(cb);
		return (0);
	}
	for (int i = 0; i < nsim; i++)
		memcpy(cb->cb_buf[sim[i]], cb->cb_read[sim[i]],
		    cb->cb_size[sim[i]]);
	return (ECKSUM);
}

static cr_result_t
cr_combrec(cr_blk_t *cb, cr_stats_t *st)
{
	const int m = cb->cb_m, n = cb->cb_width;
	hrtime_t start = gethrtime();
	cr_result_t res = CR_UNRECOVERABLE;
	int nknown = 0;

	for (int c = 0; c < cb->cb_n; c++)
		nknown += cb->cb_known[c];
	if (nknown > m)
		goto out;

	for (int nf = 1; nf <= m; nf++) {
		int tstore[RZEC_MAXPARITY + 2];
		int *ltgts = &tstore[1];

		ltgts[-1] = -1;
		for (int i = 0; i < nf; i++)
			ltgts[i] = i;
		ltgts[nf] = n;

		for (;;) {
			int err = cr_combrec_try(cb, ltgts, nf, st);
			if (err == EINVAL)
				break;
			if (err == 0) {
				res = CR_RECOVERED;
				goto out;
			}
			for (int t = 0; ; t++) {
				ltgts[t]++;
				if (ltgts[t] == n)
					break;
				if (ltgts[t] != ltgts[t + 1])
					break;
				ltgts[t] = ltgts[t - 1] + 1;
			}
			if (ltgts[nf - 1] == n)
				break;
		}
	}
out:
	st->cs_ns = gethrtime() - start;
	return (res);
}

/*
 * Syndrome-guided search
 */

/*
 * Extend basis[0 .. r-1] with syndrome vectors of offsets [from, to) until
 * it spans all of them, scanning N S for a nonzero, N being the equations
 * of the current span.  Returns the new rank.
 */
static int
cr_scan(cr_blk_t *cb, size_t from, size_t to,
    uint8_t basis[][RZEC_MAXPARITY], int r)
{
	const int m = cb->cb_m;
	uint8_t n[RZEC_MAXPARITY][RZEC_MAXPARITY];
	uint8_t coef[RZEC_MAXPARITY * RZEC_MAXPARITY];
	const uint8_t *src[RZEC_MAXPARITY];
	size_t slen[RZEC_MAXPARITY], dlen[RZEC_MAXPARITY];
	size_t p = from;

	if (r < m)
		cr_nullspace((const uint8_t (*)[RZEC_MAXPARITY])basis, r, m, n);
	while (p < to && r < m) {
		size_t end = MIN(P2ALIGN_TYPED(p, CR_SCAN, size_t) + CR_SCAN,
		    to);
		size_t len = end - p, q = len;
		int nn = m - r;

		for (int d = 0; d < nn; d++) {
			dlen[d] = len;
			memcpy(coef + d * m, n[d], m);
		}
		for (int i = 0; i < m; i++) {
			src[i] = cb->cb_syn[i] + p;
			slen[i] = len;
		}
		rzec_lincomb(nn, cb->cb_res, dlen, m, src, slen, coef);
		for (int d = 0; d < nn; d++) {
			const uint8_t *res = cb->cb_res[d];
			for (size_t i = 0; i < q; i += 8) {
				uint64_t v;
				memcpy(&v, res + i, 8);
				if (v == 0)
					continue;
				while (res[i] == 0)
					i++;
				q = i;
				break;
			}
		}
		if (q == len) {
			p = end;
			continue;
		}
		for (int i = 0; i < m; i++)
			basis[r][i] = cb->cb_syn[i][p + q];
		r++;
		if (r < m)
			cr_nullspace((const uint8_t (*)[RZEC_MAXPARITY])basis,
			    r, m, n);
		p = P2ALIGN_TYPED(p + q, CR_ALIGN, size_t);
	}
	return (r);
}

/* Syndromes of the columns as read, those reporting an error left out. */
static void
cr_syndromes(cr_blk_t *cb)
{
	const int m = cb->cb_m;
	const uint8_t **src = cb->cb_src;
	size_t *slen = cb->cb_slen, dlen[RZEC_MAXPARITY];
	uint8_t *coef = kmem_alloc(m * CR_MAXCOLS, KM_SLEEP);
	int ns = 0;

	for (int c = 0; c < cb->cb_n; c++) {
		if (cb->cb_known[c])
			continue;
		src[ns] = cb->cb_buf[c];
		slen[ns] = cb->cb_size[c];
		ns++;
	}
	for (int r = 0, s = 0; r < m; r++, s = 0)
		for (int c = 0; c < cb->cb_n; c++)
			if (!cb->cb_known[c])
				coef[r * ns + s++] = cb->cb_h[r][c];
	for (int r = 0; r < m; r++)
		dlen[r] = cb->cb_psize;
	rzec_lincomb(m, cb->cb_syn, dlen, ns, src, slen, coef);
	kmem_free(coef, m * CR_MAXCOLS);
}

/* The columns c of cand[] with h_c in the span of basis[0 .. r-1]. */
static int
cr_locate(cr_blk_t *cb, const uint8_t basis[][RZEC_MAXPARITY], int r,
    const int *cand, int ncand, int *loc)
{
	const int m = cb->cb_m;
	uint8_t n[RZEC_MAXPARITY][RZEC_MAXPARITY];
	int nl = 0;

	if (r >= m)
		return (0);
	cr_nullspace(basis, r, m, n);
	for (int i = 0; i < ncand; i++) {
		uint8_t h[RZEC_MAXPARITY];
		for (int j = 0; j < m; j++)
			h[j] = cb->cb_h[j][cand[i]];
		if (cr_in_span((const uint8_t (*)[RZEC_MAXPARITY])n, m - r, m,
		    h))
			loc[nl++] = cand[i];
	}
	return (nl);
}

/* rank([H_x | basis]) */
static int
cr_rank_with(cr_blk_t *cb, const int *x, int nx,
    const uint8_t basis[][RZEC_MAXPARITY], int r)
{
	uint8_t a[2 * RZEC_MAXPARITY][RZEC_MAXPARITY];

	for (int i = 0; i < nx; i++)
		for (int j = 0; j < cb->cb_m; j++)
			a[i][j] = cb->cb_h[j][x[i]];
	memcpy(a[nx], basis, r * sizeof (a[0]));
	return (cr_rank(a, nx + r, cb->cb_m));
}

static boolean_t
cr_has_data(const cr_blk_t *cb, const int *x, int nx)
{
	for (int i = 0; i < nx; i++)
		if (x[i] >= cb->cb_m)
			return (B_TRUE);
	return (B_FALSE);
}

static int
cr_cmp_int(const void *a, const void *b)
{
	return (*(const int *)a - *(const int *)b);
}

/*
 * Assume the columns x[] (at most m, the reported errors included) bad,
 * correct them from the syndromes and checksum the result.  With X' = x
 * completed to m columns by parities, x_X' = H_X'^-1 S, so a data column
 * becomes w_c + sum_r Hinv[c][r] S_r: m sources instead of k, and the
 * block as read is not touched unless the checksum matches.
 */
static int
cr_search_try(cr_blk_t *cb, const int *x, int nx, cr_stats_t *st)
{
	const int m = cb->cb_m;
	int xs[RZEC_MAXPARITY], dc[RZEC_MAXPARITY], dj[RZEC_MAXPARITY];
	uint8_t hm[RZEC_MAXPARITY][RZEC_MAXPARITY];
	const uint8_t *src[2 * RZEC_MAXPARITY];
	size_t slen[2 * RZEC_MAXPARITY], dlen[RZEC_MAXPARITY];
	uint8_t coef[RZEC_MAXPARITY * 2 * RZEC_MAXPARITY];
	int nxs = nx, nd = 0, ns = 0;
	zio_cksum_t zc;

	memcpy(xs, x, nx * sizeof (int));
	for (int p = 0; p < m && nxs < m; p++) {
		boolean_t in = B_FALSE;
		for (int i = 0; i < nx; i++)
			in |= (x[i] == p);
		if (!in)
			xs[nxs++] = p;
	}
	VERIFY3S(nxs, ==, m);
	for (int r = 0; r < m; r++)
		for (int j = 0; j < m; j++)
			hm[r][j] = cb->cb_h[r][xs[j]];
	VERIFY0(rzec_matrix_invert(hm, m));

	for (int j = 0; j < m; j++) {
		if (xs[j] < m)
			continue;
		dj[nd] = j;
		dc[nd] = xs[j];
		dlen[nd] = cb->cb_size[xs[j]];
		nd++;
	}
	if (nd == 0)
		return (ECKSUM);
	for (int r = 0; r < m; r++) {
		src[ns] = cb->cb_syn[r];
		slen[ns] = cb->cb_psize;
		ns++;
	}
	for (int d = 0; d < nd; d++) {
		if (cb->cb_known[dc[d]])
			continue;
		src[ns] = cb->cb_buf[dc[d]];
		slen[ns] = cb->cb_size[dc[d]];
		ns++;
	}
	for (int d = 0; d < nd; d++) {
		int s = m;
		for (int r = 0; r < m; r++)
			coef[d * ns + r] = hm[dj[d]][r];
		for (int e = 0; e < nd; e++) {
			if (cb->cb_known[dc[e]])
				continue;
			coef[d * ns + s++] = (e == d);
		}
	}
	rzec_lincomb(nd, cb->cb_cand, dlen, ns, src, slen, coef);
	st->cs_decodes++;

	cr_cksum_cand(cb, dc, cb->cb_cand, nd, &zc);
	st->cs_cksums++;
	if (!cr_cksum_eq(cb, &zc))
		return (ECKSUM);

	/* Accept only on the checksum of the block itself. */
	for (int d = 0; d < nd; d++)
		memcpy(cb->cb_buf[dc[d]], cb->cb_cand[d], dlen[d]);
	cr_cksum_full(cb, &zc);
	st->cs_verify++;
	if (!cr_cksum_eq(cb, &zc)) {
		for (int d = 0; d < nd; d++)
			memcpy(cb->cb_buf[dc[d]], cb->cb_read[dc[d]], dlen[d]);
		return (ECKSUM);
	}

	memset(&cb->cb_bad, 0, sizeof (cb->cb_bad));
	for (int c = 0; c < cb->cb_n; c++) {
		if (cb->cb_known[c])
			cr_set_add(&cb->cb_bad, c);
		else if (c >= m && memcmp(cb->cb_buf[c], cb->cb_read[c],
		    cb->cb_size[c]) != 0)
			cr_set_add(&cb->cb_bad, c);
	}
	cr_blame_parity(cb);
	return (0);
}

typedef struct cr_search_state {
	cr_set_t	ss_tried[CR_MAXTRIED];
	int		ss_ntried;
	hrtime_t	ss_start;
	boolean_t	ss_first;
} cr_search_state_t;

/* Try x unless it contains a set already shown consistent and wrong. */
static int
cr_search_hyp(cr_blk_t *cb, cr_search_state_t *ss, const int *x, int nx,
    boolean_t consistent, cr_stats_t *st)
{
	cr_set_t s = { { 0 } };
	int xs[RZEC_MAXPARITY];

	if (!cr_has_data(cb, x, nx))
		return (ECKSUM);
	for (int i = 0; i < nx; i++)
		cr_set_add(&s, x[i]);
	for (int i = 0; i < ss->ss_ntried; i++)
		if (cr_set_sub(&ss->ss_tried[i], &s))
			return (ECKSUM);
	if (cb->cb_budget != 0 && st->cs_cksums >= cb->cb_budget)
		return (EAGAIN);
	if (ss->ss_first) {
		ss->ss_first = B_FALSE;
		st->cs_prep_ns = gethrtime() - ss->ss_start;
	}
	memcpy(xs, x, nx * sizeof (int));
	qsort(xs, nx, sizeof (int), cr_cmp_int);
	int err = cr_search_try(cb, xs, nx, st);
	/*
	 * Every superset of a consistent set decodes to the same block, so
	 * none of them needs a checksum.
	 */
	if (err != 0 && consistent && ss->ss_ntried < CR_MAXTRIED)
		ss->ss_tried[ss->ss_ntried++] = s;
	return (err);
}

/* What the hypotheses share during one search. */
typedef struct cr_hyp {
	cr_blk_t	*hy_cb;
	cr_search_state_t *hy_ss;
	cr_stats_t	*hy_st;
	int		hy_known[RZEC_MAXPARITY];
	int		hy_nk;
	int		hy_cand[CR_MAXCOLS];	/* columns not reported */
	int		hy_ncand;
	uint8_t		hy_basis[RZEC_MAXPARITY][RZEC_MAXPARITY];
	int		hy_r;
	int		hy_a[CR_MAXCOLS];	/* scratch */
	int		hy_b[CR_MAXCOLS];
	int		hy_c[CR_MAXCOLS];
} cr_hyp_t;

/*
 * The errors span fewer dimensions than there are bad columns, so the
 * located columns do not explain V.  A consistent set x of fewer than m
 * columns (V within span(H_x)) contains every located column, since any
 * m columns of H are independent.  Enumerate the others it holds by size:
 * for each set of all but the last, x + c is consistent exactly when h_c
 * lies in span(H_x, V), provided that has one dimension more than H_x.
 * Returns 0 if recovered, EAGAIN on the budget, else ENOENT.
 */
static int
cr_search_deficient(cr_hyp_t *hy, const int *loc, int nloc)
{
	cr_blk_t *cb = hy->hy_cb;
	cr_stats_t *st = hy->hy_st;
	const int m = cb->cb_m, nk = hy->hy_nk, r = hy->hy_r;
	const int smax = m - nk;
	int *other = hy->hy_a, *fit = hy->hy_b, nother = 0;
	int x[RZEC_MAXPARITY];
	cr_set_t locset = { { 0 } };

	for (int i = 0; i < nloc; i++)
		cr_set_add(&locset, loc[i]);
	for (int i = 0; i < hy->hy_ncand; i++)
		if (!cr_set_has(&locset, hy->hy_cand[i]))
			other[nother++] = hy->hy_cand[i];

	for (int s = MAX(nloc + 1, r - nk); s < smax; s++) {
		int ne = s - nloc, t[RZEC_MAXPARITY];

		if (ne - 1 > nother)
			break;
		cr_comb_first(t, ne - 1);
		do {
			uint8_t sp[RZEC_MAXPARITY][RZEC_MAXPARITY];
			int nfit, nx = 0, nsp;
			int last = ne > 1 ? t[ne - 2] : -1;

			for (int i = 0; i < nk; i++)
				x[nx++] = hy->hy_known[i];
			for (int i = 0; i < nloc; i++)
				x[nx++] = loc[i];
			for (int i = 0; i < ne - 1; i++)
				x[nx++] = other[t[i]];
			if (st->cs_tests >= cb->cb_test_budget)
				return (ENOENT);
			st->cs_tests++;
			if (cr_rank_with(cb, x, nx, (const uint8_t (*)
			    [RZEC_MAXPARITY])hy->hy_basis, r) != nx + 1)
				continue;

			for (int i = 0; i < nx; i++)
				for (int j = 0; j < m; j++)
					sp[i][j] = cb->cb_h[j][x[i]];
			nsp = nx;
			for (int i = 0; i < r && nsp == nx; i++) {
				uint8_t tmp[RZEC_MAXPARITY][RZEC_MAXPARITY];
				memcpy(sp[nx], hy->hy_basis[i], m);
				memcpy(tmp, sp, (nx + 1) * sizeof (tmp[0]));
				if (cr_rank(tmp, nx + 1, m) == nx + 1)
					nsp++;
			}
			VERIFY3S(nsp, ==, nx + 1);
			nfit = cr_locate(cb, (const uint8_t (*)
			    [RZEC_MAXPARITY])sp, nsp, other, nother, fit);
			for (int f = 0, oi = 0; f < nfit; f++) {
				while (other[oi] != fit[f])
					oi++;
				if (oi <= last)
					continue;
				x[nx] = fit[f];
				int err = cr_search_hyp(cb, hy->hy_ss, x,
				    nx + 1, B_TRUE, st);
				if (err == 0 || err == EAGAIN)
					return (err);
			}
		} while (cr_comb_next(t, ne - 1, nother));
	}
	return (ENOENT);
}

/*
 * Sets of m columns, which V cannot tell apart: only the checksum can.
 * Those holding the most suspects come first: the hinted columns, and
 * those located on slices of the offsets (bad sectors or bits at
 * different offsets in m columns leave each slice with fewer than m bad
 * ones).  A slice is at most 1/64 of a column and at least 512 bytes.
 */
static int
cr_search_msets(cr_hyp_t *hy)
{
	cr_blk_t *cb = hy->hy_cb;
	cr_stats_t *st = hy->hy_st;
	const int m = cb->cb_m, nk = hy->hy_nk, smax = m - nk;
	const size_t sec = MAX(512, P2ROUNDUP(cb->cb_psize / 64, CR_ALIGN));
	int *sus = hy->hy_a, *oth = hy->hy_b, *sl = hy->hy_c;
	int nsus = 0, noth = 0, x[RZEC_MAXPARITY];
	cr_set_t susset = { { 0 } };

	for (size_t o = 0; o < cb->cb_psize; o += sec) {
		uint8_t sb[RZEC_MAXPARITY][RZEC_MAXPARITY];
		int nsl, sr;

		memcpy(sb, hy->hy_basis, nk * sizeof (sb[0]));
		sr = cr_scan(cb, o, MIN(o + sec, cb->cb_psize), sb, nk);
		if (sr == nk || sr == m)
			continue;
		nsl = cr_locate(cb, (const uint8_t (*)[RZEC_MAXPARITY])sb,
		    sr, hy->hy_cand, hy->hy_ncand, sl);
		if (nsl + nk != sr)
			continue;
		for (int i = 0; i < nsl; i++)
			cr_set_add(&susset, sl[i]);
	}
	for (int i = 0; i < hy->hy_ncand; i++) {
		int c = hy->hy_cand[i];
		if (cb->cb_hint[c] || cr_set_has(&susset, c))
			sus[nsus++] = c;
		else
			oth[noth++] = c;
	}
	st->cs_suspects = nsus;

	for (int j = MIN(nsus, smax); j >= 0; j--) {
		int a[RZEC_MAXPARITY], b[RZEC_MAXPARITY], nb = smax - j;

		if (nb > noth)
			continue;
		cr_comb_first(a, j);
		do {
			cr_comb_first(b, nb);
			do {
				int nx = 0;
				for (int i = 0; i < nk; i++)
					x[nx++] = hy->hy_known[i];
				for (int i = 0; i < j; i++)
					x[nx++] = sus[a[i]];
				for (int i = 0; i < nb; i++)
					x[nx++] = oth[b[i]];
				int err = cr_search_hyp(cb, hy->hy_ss, x, nx,
				    B_FALSE, st);
				if (err == 0 || err == EAGAIN)
					return (err);
			} while (cr_comb_next(b, nb, noth));
		} while (cr_comb_next(a, j, nsus));
	}
	return (ENOENT);
}

static cr_result_t
cr_search(cr_blk_t *cb, cr_stats_t *st)
{
	const int m = cb->cb_m;
	cr_hyp_t *hy = kmem_zalloc(sizeof (*hy), KM_SLEEP);
	cr_search_state_t *ss = kmem_zalloc(sizeof (*ss), KM_SLEEP);
	int loc[CR_MAXCOLS], nloc, err = ENOENT;
	int nk = 0;

	hy->hy_cb = cb;
	hy->hy_ss = ss;
	hy->hy_st = st;
	ss->ss_start = gethrtime();
	ss->ss_first = B_TRUE;
	for (int c = 0; c < cb->cb_n; c++) {
		if (!cb->cb_known[c])
			hy->hy_cand[hy->hy_ncand++] = c;
		else if (nk++ < m)
			hy->hy_known[nk - 1] = c;
	}
	if (nk > m)
		goto out;
	hy->hy_nk = nk;

	cr_syndromes(cb);
	for (int i = 0; i < nk; i++)
		for (int j = 0; j < m; j++)
			hy->hy_basis[i][j] = cb->cb_h[j][hy->hy_known[i]];
	hy->hy_r = cr_scan(cb, 0, cb->cb_psize, hy->hy_basis, nk);
	st->cs_rank = hy->hy_r;
	/*
	 * Syndromes explained by the reported errors alone: every set of at
	 * most m columns decodes to the block those errors give, which has
	 * already failed, so more than m columns are bad.
	 */
	if (hy->hy_r == nk)
		goto out;

	if (hy->hy_r < m) {
		nloc = cr_locate(cb,
		    (const uint8_t (*)[RZEC_MAXPARITY])hy->hy_basis, hy->hy_r,
		    hy->hy_cand, hy->hy_ncand, loc);
		st->cs_located = nloc;
		if (nloc + nk == hy->hy_r) {
			/* Usually, the located columns are the bad ones. */
			int x[RZEC_MAXPARITY];
			memcpy(x, hy->hy_known, nk * sizeof (int));
			memcpy(x + nk, loc, nloc * sizeof (int));
			err = cr_search_hyp(cb, ss, x, nk + nloc, B_TRUE, st);
		} else {
			err = cr_search_deficient(hy, loc, nloc);
		}
	}
	if (err != 0 && err != EAGAIN)
		err = cr_search_msets(hy);
out:
	if (ss->ss_first)
		st->cs_prep_ns = gethrtime() - ss->ss_start;
	st->cs_ns = gethrtime() - ss->ss_start;
	kmem_free(ss, sizeof (*ss));
	kmem_free(hy, sizeof (*hy));
	return (err == 0 ? CR_RECOVERED : err == EAGAIN ? CR_BUDGET :
	    CR_UNRECOVERABLE);
}

/*
 * Exact number of attempts cr_combrec() makes on the block as damaged,
 * from its order alone: an attempt succeeds when it rebuilds every bad
 * data column from good parities, the lowest ones not assumed bad.
 */
static uint64_t
cr_combrec_count(const cr_blk_t *cb)
{
	const int m = cb->cb_m, n = cb->cb_width;
	int devcol[CR_MAXCOLS];
	uint64_t count = 0;

	for (int d = 0; d < n; d++)
		devcol[d] = -1;
	for (int c = 0; c < cb->cb_n; c++)
		devcol[cb->cb_rr->rr_col[c].rc_devidx] = c;

	for (int nf = 1; nf <= m; nf++) {
		int tstore[RZEC_MAXPARITY + 2];
		int *ltgts = &tstore[1];

		ltgts[-1] = -1;
		for (int i = 0; i < nf; i++)
			ltgts[i] = i;
		ltgts[nf] = n;
		for (;;) {
			boolean_t lost[CR_MAXCOLS] = { 0 };
			int dead = 0, dead_data = 0;
			for (int c = 0; c < cb->cb_n; c++)
				lost[c] = cb->cb_known[c];
			for (int i = 0; i < nf; i++)
				if (devcol[ltgts[i]] >= 0)
					lost[devcol[ltgts[i]]] = B_TRUE;
			for (int c = 0; c < cb->cb_n; c++) {
				dead += lost[c];
				dead_data += lost[c] && c >= m;
			}
			if (dead > m)
				break;
			count++;
			boolean_t ok = dead_data > 0;
			size_t reach = 0;
			for (int c = m; c < cb->cb_n && ok; c++) {
				if (cr_set_has(&cb->cb_truth, c) && !lost[c])
					ok = B_FALSE;
				if (lost[c])
					reach = MAX(reach, cb->cb_size[c]);
			}
			/* A parity matters only up to the longest rebuilt. */
			for (int r = 0, used = 0; r < m && used < dead_data &&
			    ok; r++) {
				if (lost[r])
					continue;
				used++;
				if (cr_set_has(&cb->cb_truth, r) &&
				    cb->cb_dmin[r] < reach)
					ok = B_FALSE;
			}
			if (ok)
				return (count);
			for (int t = 0; ; t++) {
				ltgts[t]++;
				if (ltgts[t] == n)
					break;
				if (ltgts[t] != ltgts[t + 1])
					break;
				ltgts[t] = ltgts[t - 1] + 1;
			}
			if (ltgts[nf - 1] == n)
				break;
		}
	}
	return (count);
}

/* Rank of the sorted k-set t among those of 0 .. n-1, lexicographic. */
static uint64_t
cr_lex_rank(const int *t, int k, int n)
{
	uint64_t r = 0;

	for (int i = 0, prev = -1; i < k; prev = t[i], i++)
		for (int j = prev + 1; j < t[i]; j++)
			r += cr_binom(n - 1 - j, k - 1 - i);
	return (r);
}

static uint64_t
cr_combrec_total(int width, int m)
{
	uint64_t n = 0;

	for (int s = 1; s <= m; s++)
		n += cr_binom(width, s);
	return (n);
}

/*
 * Modes
 */

static void
cr_random_set(uint64_t *rng, int n, int k, int *t, const int *excl, int ne)
{
	for (int i = 0; i < k; i++) {
		boolean_t dup;
		do {
			t[i] = cr_rand(rng) % n;
			dup = B_FALSE;
			for (int j = 0; j < i; j++)
				dup |= (t[j] == t[i]);
			for (int j = 0; j < ne; j++)
				dup |= (excl[j] == t[i]);
		} while (dup);
	}
}

static boolean_t
cr_set_has_data(const int *t, int nt, int m)
{
	for (int i = 0; i < nt; i++)
		if (t[i] >= m)
			return (B_TRUE);
	return (B_FALSE);
}

typedef struct cr_check {
	uint64_t	ck_runs;
	uint64_t	ck_fail;	/* not recovered, or wrongly */
	uint64_t	ck_false;	/* "recovered" to the wrong data */
	uint64_t	ck_blame;	/* recovered, wrong attribution */
	uint64_t	ck_budget;
	uint64_t	ck_beyond;	/* past m, yet repaired correctly */
	uint64_t	ck_count_mismatch;
	uint64_t	ck_cksums;
	uint64_t	ck_base_cksums;
} cr_check_t;

/* Run both searches (the baseline if asked) on the block as damaged. */
static void
cr_check_one(cr_blk_t *cb, boolean_t recoverable, boolean_t base,
    const int *t, int nt, cr_check_t *ck)
{
	for (int pass = base ? 0 : 1; pass < 2; pass++) {
		cr_stats_t st = { 0 };
		cr_result_t res;

		cr_blk_reread(cb);
		/* -T: claim success without repairing, which must be caught */
		if (rto_opts.rto_sanity)
			res = CR_RECOVERED;
		else
			res = pass == 0 ? cr_combrec(cb, &st) :
			    cr_search(cb, &st);
		ck->ck_runs++;
		if (pass == 0)
			ck->ck_base_cksums += st.cs_cksums;
		else
			ck->ck_cksums += st.cs_cksums;
		boolean_t bad = B_FALSE;
		if (res == CR_RECOVERED && !cr_data_ok(cb)) {
			ck->ck_false++;
			bad = B_TRUE;
		} else if (res == CR_RECOVERED && !recoverable) {
			/*
			 * More than m columns damaged, some only where no
			 * rebuild reads them: a true repair, whose blame the
			 * baseline may leave short (no spare parity to check).
			 */
			ck->ck_beyond++;
		} else if (res == CR_RECOVERED &&
		    !cr_set_eq(&cb->cb_bad, &cb->cb_truth)) {
			ck->ck_blame++;
			bad = B_TRUE;
		} else if (res == CR_BUDGET) {
			ck->ck_budget++;
		} else if (recoverable && res != CR_RECOVERED) {
			bad = B_TRUE;
		}
		if (pass == 0 && !rto_opts.rto_sanity &&
		    st.cs_cksums != cr_combrec_count(cb))
			ck->ck_count_mismatch++;
		if (bad) {
			ck->ck_fail++;
			if (ck->ck_fail <= 10) {
				LOG(D_ALL, "FAIL %s m=%d n=%d res=%s set={",
				    pass ? "search" : "combrec", cb->cb_m,
				    cb->cb_width, cr_result_names[res]);
				for (int i = 0; i < nt; i++)
					LOG(D_ALL, "%s%d", i ? "," : "", t[i]);
				LOG(D_ALL, "} known={");
				for (int c = 0, f = 0; c < cb->cb_n; c++)
					if (cb->cb_known[c])
						LOG(D_ALL, "%s%d", f++ ? "," :
						    "", c);
				LOG(D_ALL, "} rank=%d located=%d\n",
				    st.cs_rank, st.cs_located);
			}
		}
	}
}

/*
 * Every set of 1 .. m silent columns (with a data column) for widths up
 * to -w, each damage kind, alone and with random reported errors; then
 * random sets beyond.  Sets of m + 1 and m + 2 must not be "repaired".
 */
static int
cr_run_check(rzec_code_t code, int mlo, int mhi)
{
	static const int wide[] = { 12, 16, 20, 24, 32, 40, 64, 128, 255 };
	const int wbase = 10;
	cr_check_t ck = { 0 };
	uint64_t seed = rto_opts.rto_ec_seed;
	hrtime_t start = gethrtime();

	for (int m = mlo; m <= mhi; m++) {
		int nmax = MIN(255, m + rzec_max_data(code));
		for (int n = m + 1; n <= nmax; n++) {
			boolean_t exh = n <= (int)rto_opts.rto_ec_width;
			boolean_t listed = B_FALSE;
			for (int i = 0; i < ARRAY_SIZE(wide); i++)
				listed |= (wide[i] == n);
			if (!exh && !listed)
				continue;
			int k = n - m;
			uint64_t sectors = exh ? 3 * (uint64_t)k + k / 2 + 1 :
			    MIN(256, 2 * (uint64_t)k + 1);
			int ashift = exh ? 9 : 12;
			cr_blk_t *cb = cr_blk_alloc(code, m, n,
			    sectors << ashift, ashift, seed++);
			cr_check_t c0 = ck;
			int ncols = cb->cb_n;

			/*
			 * m whole random columns can only be found by trying
			 * the m-sets, about C(n, m) / 2 of them: bounded on
			 * the sampled widths, where -L or 20000 applies.
			 */
			if (!exh && cb->cb_budget == 0)
				cb->cb_budget = 20000;
			int t[RZEC_MAXPARITY + 2], kn[RZEC_MAXPARITY];

			for (int nt = 1; exh && nt <= m; nt++) {
				cr_comb_first(t, nt);
				do {
					if (!cr_set_has_data(t, nt, m))
						continue;
					for (int kd = 0; kd < CR_D_NKINDS;
					    kd++) {
						int nk = 0;
						cr_blk_reset(cb);
						cr_damage(cb, t, nt, kd, NULL,
						    0);
						cr_check_one(cb, B_TRUE,
						    kd == CR_D_WHOLE &&
						    n <= wbase, t, nt, &ck);
						if (nt == m)
							continue;
						nk = 1 + cr_rand(&cb->cb_rng) %
						    (m - nt);
						cr_random_set(&cb->cb_rng,
						    ncols, nk, kn, t, nt);
						cr_blk_reset(cb);
						cr_damage(cb, t, nt, kd, kn,
						    nk);
						cr_check_one(cb, B_TRUE,
						    B_FALSE, t, nt, &ck);
					}
				} while (cr_comb_next(t, nt, ncols));
			}
			for (uint64_t s = 0; !exh && s <
			    rto_opts.rto_ec_samples; s++) {
				int nt = 1 + cr_rand(&cb->cb_rng) % m;
				int kd = cr_rand(&cb->cb_rng) % CR_D_NKINDS;
				int nk = cr_rand(&cb->cb_rng) % 2 ?
				    cr_rand(&cb->cb_rng) % (m - nt + 1) : 0;
				/* bound the rank-deficient searches */
				if (kd >= CR_D_SAMEOFF && nt > 2)
					kd = cr_rand(&cb->cb_rng) %
					    CR_D_SAMEOFF;
				do {
					cr_random_set(&cb->cb_rng, ncols, nt,
					    t, NULL, 0);
				} while (!cr_set_has_data(t, nt, m));
				cr_random_set(&cb->cb_rng, ncols, nk, kn, t,
				    nt);
				cr_blk_reset(cb);
				cr_damage(cb, t, nt, kd, kn, nk);
				cr_check_one(cb, B_TRUE, B_FALSE, t, nt, &ck);
			}
			/* Beyond m: must be reported, never repaired. */
			uint64_t beyond = MIN(rto_opts.rto_ec_samples,
			    n <= wbase ? 200 : 20);
			for (uint64_t s = 0; s < beyond && n > m + 1; s++) {
				int nt = m + 1 + (n > m + 2 ?
				    cr_rand(&cb->cb_rng) % 2 : 0);
				int kd = cr_rand(&cb->cb_rng) % 2;
				if (cr_binom(n, m) > 20000)
					break;
				cr_random_set(&cb->cb_rng, ncols, nt, t, NULL,
				    0);
				cr_blk_reset(cb);
				cr_damage(cb, t, nt, kd, NULL, 0);
				cr_check_one(cb, B_FALSE, n <= wbase, NULL, 0,
				    &ck);
			}
			LOG(D_ALL, "%s m=%d n=%3d k=%3d %s size=%7zu runs=%llu "
			    "fail=%llu false=%llu blame=%llu budget=%llu "
			    "beyond_repaired=%llu count_mismatch=%llu\n",
			    rzec_code_name(code), m, n,
			    k, exh ? "all   " : "sample", cb->cb_zio.io_size,
			    (u_longlong_t)(ck.ck_runs - c0.ck_runs),
			    (u_longlong_t)(ck.ck_fail - c0.ck_fail),
			    (u_longlong_t)(ck.ck_false - c0.ck_false),
			    (u_longlong_t)(ck.ck_blame - c0.ck_blame),
			    (u_longlong_t)(ck.ck_budget - c0.ck_budget),
			    (u_longlong_t)(ck.ck_beyond - c0.ck_beyond),
			    (u_longlong_t)(ck.ck_count_mismatch -
			    c0.ck_count_mismatch));
			cr_blk_free(cb);
		}
	}
	LOG(D_ALL, "total: runs=%llu fail=%llu false=%llu blame=%llu "
	    "budget=%llu beyond_repaired=%llu count_mismatch=%llu "
	    "search_cksums=%llu combrec_cksums=%llu in %.1fs: %s\n",
	    (u_longlong_t)ck.ck_runs, (u_longlong_t)ck.ck_fail,
	    (u_longlong_t)ck.ck_false, (u_longlong_t)ck.ck_blame,
	    (u_longlong_t)ck.ck_budget, (u_longlong_t)ck.ck_beyond,
	    (u_longlong_t)ck.ck_count_mismatch,
	    (u_longlong_t)ck.ck_cksums, (u_longlong_t)ck.ck_base_cksums,
	    NSEC2SEC((double)(gethrtime() - start)),
	    ck.ck_fail || ck.ck_count_mismatch ? "FAIL" : "PASS");
	return (ck.ck_fail != 0 || ck.ck_count_mismatch != 0);
}

/* Mean time of one baseline attempt with s targets, sampled. */
static double
cr_try_cost(cr_blk_t *cb, int s)
{
	cr_stats_t st = { 0 };
	int t[RZEC_MAXPARITY];
	uint64_t tries = 0;
	hrtime_t start = gethrtime(), now;

	cr_blk_reread(cb);
	do {
		cr_random_set(&cb->cb_rng, cb->cb_width, s, t, NULL, 0);
		(void) cr_combrec_try(cb, t, s, &st);
		tries++;
		now = gethrtime();
	} while (now - start < CR_SAMPLE_NSEC || tries < 20);
	return ((double)(now - start) / tries);
}

/*
 * Mean time of one candidate of the search over random m-sets, the
 * order of the search putting the cheap ones (mostly parity) first.
 */
static double
cr_search_cost(cr_blk_t *cb)
{
	cr_stats_t st = { 0 };
	int t[RZEC_MAXPARITY];
	uint64_t tries = 0;
	hrtime_t start, now;

	cr_blk_reread(cb);
	cr_syndromes(cb);
	cr_prepare(cb);
	start = gethrtime();
	do {
		do {
			cr_random_set(&cb->cb_rng, cb->cb_n, cb->cb_m, t, NULL,
			    0);
		} while (!cr_set_has_data(t, cb->cb_m, cb->cb_m));
		qsort(t, cb->cb_m, sizeof (int), cr_cmp_int);
		(void) cr_search_try(cb, t, cb->cb_m, &st);
		tries++;
		now = gethrtime();
	} while (now - start < CR_SAMPLE_NSEC || tries < 20);
	return ((double)(now - start) / tries);
}

static int
cr_parse_list(const char *s, int *v, int max)
{
	int n = 0;

	while (s != NULL && *s != '\0' && n < max) {
		v[n++] = strtol(s, NULL, 0);
		s = strchr(s, ',');
		if (s != NULL)
			s++;
	}
	return (n);
}

static const int cr_default_widths[] = { 8, 12, 16, 20, 24, 32, 40 };

/*
 * Worst case per damaged block: m + 1 whole columns lost, so both
 * searches exhaust their hypotheses.  The baseline's attempts are
 * counted exactly and timed per size of set, sampled; the search's
 * candidates are timed on a budgeted run.  Runs whose estimate is below
 * the -n limit (in milliseconds) are also run in full.
 */
static void
cr_run_worst(rzec_code_t code, int mlo, int mhi, const int *widths, int nw)
{
	size_t size = rto_opts.rto_dsize;
	uint64_t seed = rto_opts.rto_ec_seed;
	double full_ms = (double)rto_opts.rto_ec_samples;

	LOG(D_ALL, "worst: code=%s size=%zu ck=%d bits=%d kernel=%s "
	    "prod=%s full_below_ms=%.0f\n", rzec_code_name(code), size,
	    cr_ck, cr_cksum_bits, rto_opts.rto_ec_kernel_name ?
	    rto_opts.rto_ec_kernel_name : "table",
	    cr_prod_impl ? cr_prod_impl : "-", full_ms);
	for (int m = mlo; m <= mhi; m++) {
		for (int w = 0; w < nw; w++) {
			int n = widths[w], t[RZEC_MAXPARITY + 1];
			if (n <= m + 1)
				continue;
			cr_blk_t *cb = cr_blk_alloc(code, m, n, size, 12,
			    seed++);
			cr_random_set(&cb->cb_rng, cb->cb_n, m + 1, t, NULL, 0);
			cr_damage(cb, t, m + 1, CR_D_WHOLE, NULL, 0);

			uint64_t btries = cr_combrec_total(n, m);
			double bms = 0;
			for (int s = 1; s <= m; s++)
				bms += cr_binom(n, s) * cr_try_cost(cb, s) /
				    1e6;
			double bfull = -1;
			if (bms < full_ms) {
				cr_stats_t st = { 0 };
				cr_blk_reread(cb);
				VERIFY3S(cr_combrec(cb, &st), ==,
				    CR_UNRECOVERABLE);
				VERIFY3U(st.cs_cksums, ==, btries);
				bfull = st.cs_ns / 1e6;
			}

			/* the candidates: every m-set of the n_eff columns */
			uint64_t stries = cr_binom(cb->cb_n, m) - 1;
			cr_stats_t st = { 0 };
			cr_blk_reread(cb);
			cb->cb_budget = MIN(stries, 2000);
			cr_result_t res = cr_search(cb, &st);
			VERIFY3S(res, !=, CR_RECOVERED);
			double per = cr_search_cost(cb);
			double sms = (st.cs_prep_ns + per * stries) / 1e6;
			double sfull = -1;
			if (sms < full_ms) {
				cr_stats_t st2 = { 0 };
				cr_blk_reread(cb);
				cb->cb_budget = 0;
				VERIFY3S(cr_search(cb, &st2), ==,
				    CR_UNRECOVERABLE);
				VERIFY3U(st2.cs_cksums, ==, stries);
				sfull = st2.cs_ns / 1e6;
			}
			LOG(D_ALL, "worst m=%d n=%3d ncols=%3d size=%7zu "
			    "combrec_cksums=%llu combrec_ms=%.1f "
			    "combrec_full_ms=%.1f search_cksums=%llu "
			    "search_prep_ms=%.3f search_ms=%.1f "
			    "search_full_ms=%.1f per_try_us=%.1f "
			    "combrec_per_try_us=%.1f\n", m, n, cb->cb_n, size,
			    (u_longlong_t)btries, bms, bfull,
			    (u_longlong_t)stries, st.cs_prep_ns / 1e6, sms,
			    sfull, per / 1e3, bms * 1e3 / btries);
			cr_blk_free(cb);
		}
	}
}

/*
 * Candidate checksums a full search would make, where a budgeted one
 * stopped: all of them past m columns, or the place of the bad set in
 * the order of the m-sets when nothing reorders them; else 0, unknown.
 */
static uint64_t
cr_estimate(const cr_blk_t *cb, const cr_stats_t *st, cr_result_t res,
    const int *t, int nt)
{
	int s[RZEC_MAXPARITY + 1];

	if (res != CR_BUDGET)
		return (st->cs_cksums);
	if (nt > cb->cb_m)
		return (cr_binom(cb->cb_n, cb->cb_m) - 1);
	if (nt < cb->cb_m || st->cs_rank < cb->cb_m || st->cs_suspects != 0)
		return (0);
	memcpy(s, t, nt * sizeof (int));
	qsort(s, nt, sizeof (int), cr_cmp_int);
	return (cr_lex_rank(s, nt, cb->cb_n));
}

/*
 * Random damage: t = 1 .. m + 1 silent columns of each kind, -n samples
 * each.  The search runs in full up to the -L budget; the baseline's
 * attempts are counted exactly from its order and timed per size of set.
 */
static void
cr_run_random(rzec_code_t code, int mlo, int mhi, const int *widths, int nw)
{
	size_t size = rto_opts.rto_dsize;
	uint64_t seed = rto_opts.rto_ec_seed;

	LOG(D_ALL, "random: code=%s size=%zu ck=%d kernel=%s budget=%llu "
	    "samples=%llu\n", rzec_code_name(code), size, cr_ck,
	    rto_opts.rto_ec_kernel_name ? rto_opts.rto_ec_kernel_name :
	    "table", (u_longlong_t)cr_budget,
	    (u_longlong_t)rto_opts.rto_ec_samples);
	for (int m = mlo; m <= mhi; m++) {
		for (int w = 0; w < nw; w++) {
			int n = widths[w];
			double cost[RZEC_MAXPARITY + 1];
			if (n <= m + 1)
				continue;
			cr_blk_t *cb = cr_blk_alloc(code, m, n, size, 12,
			    seed++);
			cr_blk_reset(cb);
			int t0[1] = { m };
			cr_damage(cb, t0, 1, CR_D_WHOLE, NULL, 0);
			for (int s = 1; s <= m; s++)
				cost[s] = cr_try_cost(cb, s);
			double scost = cr_search_cost(cb);
			for (int kd = 0; kd < CR_D_NKINDS; kd++) {
				for (int nt = 1; nt <= m + 1; nt++) {
					if (kd >= CR_D_SAMEOFF && nt == 1)
						continue;
					for (uint64_t s = 0;
					    s < rto_opts.rto_ec_samples; s++) {
						int t[RZEC_MAXPARITY + 1];
						cr_stats_t st = { 0 };
						do {
							cr_random_set(
							    &cb->cb_rng,
							    cb->cb_n, nt, t,
							    NULL, 0);
						} while (!cr_set_has_data(t,
						    nt, m));
						cr_blk_reset(cb);
						cr_damage(cb, t, nt, kd, NULL,
						    0);
						cr_blk_reread(cb);
						cr_result_t res =
						    cr_search(cb, &st);
						boolean_t ok = res ==
						    CR_RECOVERED &&
						    cr_data_ok(cb) &&
						    cr_set_eq(&cb->cb_bad,
						    &cb->cb_truth);
						uint64_t bt =
						    cr_combrec_count(cb);
						uint64_t est = cr_estimate(cb,
						    &st, res, t, nt);
						double ems = res == CR_BUDGET ?
						    (st.cs_prep_ns + scost *
						    est) / 1e6 : st.cs_ns / 1e6;
						double bms = 0;
						uint64_t left = bt;
						for (int l = 1; l <= m &&
						    left > 0; l++) {
							uint64_t c = MIN(left,
							    cr_binom(n, l));
							bms += c * cost[l];
							left -= c;
						}
						LOG(D_ALL, "rand m=%d n=%d "
						    "kind=%s t=%d res=%s ok=%d "
						    "rank=%d located=%d "
						    "suspects=%d "
						    "cksums=%llu tests=%llu "
						    "ms=%.3f prep_ms=%.3f "
						    "est_cksums=%llu "
						    "est_ms=%.1f "
						    "combrec_cksums=%llu "
						    "combrec_ms=%.1f\n", m, n,
						    cr_damage_names[kd], nt,
						    cr_result_names[res],
						    nt <= m ? ok : !ok ||
						    res != CR_RECOVERED,
						    st.cs_rank, st.cs_located,
						    st.cs_suspects,
						    (u_longlong_t)st.cs_cksums,
						    (u_longlong_t)st.cs_tests,
						    st.cs_ns / 1e6,
						    st.cs_prep_ns / 1e6,
						    (u_longlong_t)est, ems,
						    (u_longlong_t)bt,
						    bms / 1e6);
					}
				}
			}
			cr_blk_free(cb);
		}
	}
}

/*
 * False repairs with a checksum truncated to -b bits: m + 1 whole columns
 * lost, both searches, the rate of "recovered" against the number of
 * wrong candidates checksummed.
 */
static void
cr_run_false(rzec_code_t code, int mlo, int mhi, const int *widths, int nw)
{
	uint64_t seed = rto_opts.rto_ec_seed;

	LOG(D_ALL, "false: code=%s bits=%d ck=%d samples=%llu\n",
	    rzec_code_name(code), cr_cksum_bits, cr_ck,
	    (u_longlong_t)rto_opts.rto_ec_samples);
	for (int m = mlo; m <= mhi; m++) {
		for (int w = 0; w < nw; w++) {
			int n = widths[w];
			uint64_t fr[2] = { 0 }, tries[2] = { 0 };
			if (n <= m + 1)
				continue;
			int k = n - m;
			cr_blk_t *cb = cr_blk_alloc(code, m, n,
			    (uint64_t)k << 9, 9, seed++);
			for (uint64_t s = 0; s < rto_opts.rto_ec_samples;
			    s++) {
				int t[RZEC_MAXPARITY + 1];
				cr_random_set(&cb->cb_rng, cb->cb_n, m + 1, t,
				    NULL, 0);
				cr_blk_reset(cb);
				cr_damage(cb, t, m + 1, CR_D_WHOLE, NULL, 0);
				for (int pass = 0; pass < 2; pass++) {
					cr_stats_t st = { 0 };
					cr_blk_reread(cb);
					cr_result_t res = pass == 0 ?
					    cr_combrec(cb, &st) :
					    cr_search(cb, &st);
					tries[pass] += st.cs_cksums;
					if (res == CR_RECOVERED) {
						VERIFY(!cr_data_ok(cb));
						fr[pass]++;
					}
				}
			}
			double p = ldexp(1.0, -cr_cksum_bits);
			uint64_t ns = rto_opts.rto_ec_samples;
			for (int pass = 0; pass < 2; pass++) {
				double per = (double)tries[pass] / ns;
				LOG(D_ALL, "false m=%d n=%d bits=%d %s "
				    "samples=%llu false=%llu rate=%.4f "
				    "cksums_per_block=%.1f "
				    "expected=%.4f\n", m, n, cr_cksum_bits,
				    pass ? "search " : "combrec",
				    (u_longlong_t)ns, (u_longlong_t)fr[pass],
				    (double)fr[pass] / ns, per,
				    1 - pow(1 - p, per));
			}
			cr_blk_free(cb);
		}
	}
}

int
run_combrec(void)
{
	const char *mode = rto_opts.rto_cr_mode;
	rzec_code_t code = rto_opts.rto_ec_vand ? RZEC_VAND : RZEC_CAUCHY;
	int mlo = rto_opts.rto_ec_parity ? rto_opts.rto_ec_parity :
	    (code == RZEC_VAND ? 3 : 4);
	int mhi = rto_opts.rto_ec_parity ? rto_opts.rto_ec_parity :
	    (code == RZEC_VAND ? 3 : 6);
	int widths[32], nw;
	zio_cksum_salt_t salt;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (code == RZEC_VAND && mhi > PARITY_PQR) {
		ERR("-V only gives a recoverable code for m <= 3\n");
		return (1);
	}
	cr_ck = rto_opts.rto_cr_cksum;
	cr_cksum_bits = rto_opts.rto_cr_bits;
	cr_budget = rto_opts.rto_cr_budget;
	cr_prod_impl = rto_opts.rto_cr_prod;
	memset(&salt, 0x5a, sizeof (salt));
	cr_blake3_tmpl = abd_checksum_blake3_tmpl_init(&salt);
	if (rto_opts.rto_ec_kernel >= 0)
		rzec_set_kernel(rto_opts.rto_ec_kernel);

	nw = cr_parse_list(rto_opts.rto_cr_widths, widths, ARRAY_SIZE(widths));
	if (nw == 0) {
		nw = ARRAY_SIZE(cr_default_widths);
		memcpy(widths, cr_default_widths, sizeof (cr_default_widths));
	}

	int err = 0;
	if (strcmp(mode, "check") == 0)
		err = cr_run_check(code, mlo, mhi);
	else if (strcmp(mode, "worst") == 0)
		cr_run_worst(code, mlo, mhi, widths, nw);
	else if (strcmp(mode, "random") == 0)
		cr_run_random(code, mlo, mhi, widths, nw);
	else if (strcmp(mode, "false") == 0)
		cr_run_false(code, mlo, mhi, widths, nw);
	else {
		ERR("unknown -R mode %s\n", mode);
		err = 1;
	}
	abd_checksum_blake3_tmpl_free(cr_blake3_tmpl);
	return (err);
}
