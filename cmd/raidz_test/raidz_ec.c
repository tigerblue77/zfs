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
 * Userspace prototype of a systematic erasure code with up to
 * RZEC_MAXPARITY parity columns per RAID-Z row.  Only raidz_test uses it:
 * nothing here is reachable from the module or changes the on-disk
 * format.
 *
 * In a row, columns 0 .. m-1 hold the parities and column m + j holds
 * data column j, 0 <= j < k.  Parity r is P_r = sum_j A[r][j] * D_j over
 * GF(2^8) with the RAID-Z polynomial 0x11d.  Two generators are provided:
 *
 * RZEC_CAUCHY: A[0][j] = 1 and A[r][j] = y_r / (x_j + y_r) for r >= 1,
 * with x_j = j and y_r = 256 - r.  This is a Cauchy matrix extended by a
 * row of ones, each row scaled so that A[r][0] = 1.  Every square
 * submatrix of A is nonsingular, so any m lost columns can be rebuilt,
 * for k <= 256 - (RZEC_MAXPARITY - 1) data columns.  P stays a plain XOR,
 * a row with one data column has every parity equal to its data, and a
 * coefficient depends on (r, j) only, so a zero-length column adds
 * nothing and the first m rows of the code for m + 1 are the code for m.
 *
 * RZEC_VAND: A[r][j] = 2^(r * (k - 1 - j)), the existing P, Q and R for
 * r < 3 continued with the generator 8, 16, ...  It reproduces the
 * on-disk parity for m <= 3, and is kept to show where extending it
 * stops being recoverable.
 */

#include <sys/zfs_context.h>
#include <sys/time.h>
#include <sys/zio.h>
#include <sys/kmem.h>
#include <sys/abd.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/simd.h>
#include <stdio.h>
#include "raidz_test.h"
#include "raidz_ec_simd.h"

#define	RZEC_MAXCOLS	256
#define	RZEC_CHUNK	1024

/* Largest k for which RZEC_CAUCHY keeps x_j and y_r disjoint. */
#define	RZEC_CAUCHY_MAXDATA	(257 - RZEC_MAXPARITY)

/* rzec_mul_lt[c][a] = c * a, built from vdev_raidz_pow2/log2. */
static uint8_t rzec_mul_lt[256][256] __attribute__((aligned(256)));
static uint8_t rzec_cauchy[RZEC_MAXPARITY][RZEC_MAXCOLS];
static struct rzec_row *rzec_rw;

const char *
rzec_code_name(rzec_code_t code)
{
	return (code == RZEC_CAUCHY ? "cauchy" : "vand");
}

int
rzec_max_data(rzec_code_t code)
{
	return (code == RZEC_CAUCHY ? RZEC_CAUCHY_MAXDATA : 255);
}

void
rzec_init(void)
{
	for (int c = 0; c < 256; c++)
		for (int a = 0; a < 256; a++)
			rzec_mul_lt[c][a] = gf_mul(c, a);

	for (int j = 0; j < RZEC_MAXCOLS; j++)
		rzec_cauchy[0][j] = 1;
	for (int r = 1; r < RZEC_MAXPARITY; r++) {
		gf_t y = 256 - r;
		for (int j = 0; j < RZEC_MAXCOLS; j++) {
			gf_t s = (gf_t)j ^ y;
			rzec_cauchy[r][j] = (s == 0) ? 0 : gf_div(y, s);
		}
	}
	rzec_simd_init((const uint8_t (*)[256])rzec_mul_lt);
}

/* Coefficient of data column j (of k) in parity row r. */
static inline uint8_t
rzec_coef(rzec_code_t code, int r, int j, int k)
{
	if (code == RZEC_CAUCHY)
		return (rzec_cauchy[r][j]);
	return (vdev_raidz_pow2[(r * (k - 1 - j)) % 255]);
}

static inline void
rzec_xor_acc(uint8_t *restrict dst, const uint8_t *restrict src, size_t n)
{
	size_t i = 0;

	for (; i + 8 <= n; i += 8) {
		uint64_t d, s;
		memcpy(&d, dst + i, 8);
		memcpy(&s, src + i, 8);
		d ^= s;
		memcpy(dst + i, &d, 8);
	}
	for (; i < n; i++)
		dst[i] ^= src[i];
}

/* dst ^= c * src, eight table lookups per 64-bit store. */
static inline void
rzec_mul_acc(uint8_t *restrict dst, const uint8_t *restrict src, size_t n,
    uint8_t c)
{
	const uint8_t *lt = rzec_mul_lt[c];
	size_t i = 0;

	if (c == 0)
		return;
	if (c == 1) {
		rzec_xor_acc(dst, src, n);
		return;
	}
	for (; i + 8 <= n; i += 8) {
		union {
			uint64_t e;
			uint8_t b[8];
		} v;
		uint64_t d;
		v.b[0] = lt[src[i + 0]];
		v.b[1] = lt[src[i + 1]];
		v.b[2] = lt[src[i + 2]];
		v.b[3] = lt[src[i + 3]];
		v.b[4] = lt[src[i + 4]];
		v.b[5] = lt[src[i + 5]];
		v.b[6] = lt[src[i + 6]];
		v.b[7] = lt[src[i + 7]];
		memcpy(&d, dst + i, 8);
		d ^= v.e;
		memcpy(dst + i, &d, 8);
	}
	for (; i < n; i++)
		dst[i] ^= lt[src[i]];
}

/*
 * Two scalar kernels for dst[d] ^= c[d] * src, d < nd, sharing one source
 * (the SIMD ones, in raidz_ec_simd.c, compute whole dot products instead):
 *
 * RZEC_KERNEL_TABLE looks every byte up in rzec_mul_lt, once per
 * destination.
 *
 * RZEC_KERNEL_SWAR computes src * 2^b for b = 0 .. 7 eight bytes at a
 * time, with the shift-and-reduce the scalar back-end uses for Q and R,
 * then XORs into each destination the multiples selected by the bits of
 * its coefficient.  The doublings are shared by all destinations, but it
 * measured slower than the table for every m up to 6, so the table is
 * the default and SWAR serves as a cross-check.
 */
static rzec_kernel_t rzec_kernel = RZEC_KERNEL_TABLE;

/* The SIMD kernel selected, or NULL for a scalar one. */
static const rzec_simd_t *rzec_simd;

void
rzec_set_kernel(rzec_kernel_t kernel)
{
	VERIFY(rzec_kernel_available(kernel));
	rzec_kernel = kernel;
	rzec_simd = kernel >= RZEC_KERNEL_SIMD ?
	    rzec_simd_impls[kernel - RZEC_KERNEL_SIMD] : NULL;
}

int
rzec_nkernels(void)
{
	return (RZEC_KERNEL_SIMD + rzec_simd_nimpls);
}

boolean_t
rzec_kernel_available(rzec_kernel_t kernel)
{
	if (kernel < 0 || kernel >= rzec_nkernels())
		return (B_FALSE);
	if (kernel < RZEC_KERNEL_SIMD)
		return (B_TRUE);
	return (rzec_simd_impls[kernel - RZEC_KERNEL_SIMD]->rs_available() ?
	    B_TRUE : B_FALSE);
}

const char *
rzec_kernel_name(rzec_kernel_t kernel)
{
	if (kernel == RZEC_KERNEL_SWAR)
		return ("swar");
	if (kernel == RZEC_KERNEL_TABLE)
		return ("table");
	if (kernel >= RZEC_KERNEL_SIMD && kernel < rzec_nkernels())
		return (rzec_simd_impls[kernel - RZEC_KERNEL_SIMD]->rs_name);
	return ("?");
}

/* Kernel by name, or "all", or "simd" (all SIMD ones); -3 if unknown. */
rzec_kernel_t
rzec_kernel_lookup(const char *name)
{
	if (strcmp(name, "all") == 0)
		return (RZEC_KERNEL_ALL);
	if (strcmp(name, "simd") == 0)
		return (RZEC_KERNEL_ALLSIMD);
	for (int k = 0; k < rzec_nkernels(); k++)
		if (strcmp(name, rzec_kernel_name(k)) == 0)
			return (k);
	return (-3);
}

/* Whether -k selects kernel k for rebuilding. */
static boolean_t
rzec_kernel_selected(rzec_kernel_t k)
{
	int sel = rto_opts.rto_ec_kernel;

	if (!rzec_kernel_available(k))
		return (B_FALSE);
	if (sel == RZEC_KERNEL_ALL)
		return (B_TRUE);
	if (sel == RZEC_KERNEL_ALLSIMD)
		return (k >= RZEC_KERNEL_SIMD);
	return (k == sel);
}

static inline uint64_t
rzec_mul2(uint64_t a)
{
	uint64_t mask = a & 0x8080808080808080ULL;

	mask = (mask << 1) - (mask >> 7);
	return (((a << 1) & 0xfefefefefefefefeULL) ^
	    (mask & 0x1d1d1d1d1d1d1d1dULL));
}

static void
rzec_mad_swar(uint8_t *const *dst, const uint8_t *c, int nd,
    const uint8_t *restrict src, size_t n)
{
	uint8_t bit[RZEC_MAXPARITY][8];
	int nbit[RZEC_MAXPARITY];
	int top = 0;
	size_t i = 0;

	for (int d = 0; d < nd; d++) {
		nbit[d] = 0;
		for (int b = 0; b < 8; b++) {
			if (c[d] & (1 << b)) {
				bit[d][nbit[d]++] = b;
				top = MAX(top, b);
			}
		}
	}

	for (; i + 8 <= n; i += 8) {
		uint64_t pw[8];
		memcpy(&pw[0], src + i, 8);
		for (int b = 1; b <= top; b++)
			pw[b] = rzec_mul2(pw[b - 1]);
		for (int d = 0; d < nd; d++) {
			uint64_t acc, v;
			if (nbit[d] == 0)
				continue;
			acc = pw[bit[d][0]];
			for (int t = 1; t < nbit[d]; t++)
				acc ^= pw[bit[d][t]];
			memcpy(&v, dst[d] + i, 8);
			v ^= acc;
			memcpy(dst[d] + i, &v, 8);
		}
	}
	for (int d = 0; d < nd; d++) {
		const uint8_t *lt = rzec_mul_lt[c[d]];
		for (size_t t = i; t < n; t++)
			dst[d][t] ^= lt[src[t]];
	}
}

static inline void
rzec_mad(uint8_t *const *dst, const uint8_t *c, int nd, const uint8_t *src,
    size_t n)
{
	if (rzec_kernel == RZEC_KERNEL_SWAR) {
		rzec_mad_swar(dst, c, nd, src, n);
		return;
	}
	for (int d = 0; d < nd; d++)
		rzec_mul_acc(dst[d], src, n, c[d]);
}

/*
 * Inputs and outputs of one dot product, dst[d] = sum_j c[d][j] * src[j],
 * each column with its own length (bytes past it count as zero).
 */
typedef struct rzec_dot {
	int		dt_nd;
	int		dt_ns;
	uint8_t		*dt_dst[RZEC_MAXPARITY];
	size_t		dt_dlen[RZEC_MAXPARITY];
	const uint8_t	*dt_src[RZEC_MAXCOLS];
	size_t		dt_slen[RZEC_MAXCOLS];
	uint8_t		dt_c[RZEC_MAXPARITY][RZEC_MAXCOLS];
	/* scratch of rzec_dot_run() */
	size_t		dt_cut[RZEC_MAXCOLS + RZEC_MAXPARITY];
	const uint8_t	*dt_rsrc[RZEC_MAXCOLS];
	int		dt_ssel[RZEC_MAXCOLS];
} rzec_dot_t;

/*
 * Per-row view of the columns as flat buffers.  Linear ABDs are used in
 * place; any other ABD is borrowed with a copy and returned afterwards.
 */
typedef struct rzec_row {
	int		rw_m;
	int		rw_k;
	size_t		rw_psize;
	uint8_t		*rw_buf[RZEC_MAXCOLS];
	size_t		rw_size[RZEC_MAXCOLS];
	boolean_t	rw_borrowed[RZEC_MAXCOLS];
	boolean_t	rw_written[RZEC_MAXCOLS];
	uint8_t		rw_a[RZEC_MAXPARITY][RZEC_MAXCOLS];
	uint8_t		rw_syn[RZEC_MAXPARITY][RZEC_CHUNK];
	uint8_t		rw_sol[RZEC_MAXPARITY][RZEC_CHUNK];
	boolean_t	rw_lost[RZEC_MAXCOLS];
	rzec_dot_t	rw_dot;
	uint8_t		rw_tab[RZEC_MAXPARITY * RZEC_MAXCOLS * 32]
	    __attribute__((aligned(64)));
} rzec_row_t;

/* raidz_test runs the prototype on one thread, so one context serves. */
static rzec_row_t *
rzec_row_get(void)
{
	if (rzec_rw == NULL)
		rzec_rw = kmem_alloc(sizeof (*rzec_rw), KM_SLEEP);
	return (rzec_rw);
}

static void
rzec_row_enter(rzec_row_t *rw, raidz_row_t *rr, rzec_code_t code)
{
	int m = rr->rr_firstdatacol;
	int k = rr->rr_cols - m;

	VERIFY3S(m, >=, 1);
	VERIFY3S(m, <=, RZEC_MAXPARITY);
	VERIFY3S(k, >=, 1);
	VERIFY3S(k, <=, rzec_max_data(code));

	rw->rw_m = m;
	rw->rw_k = k;
	rw->rw_psize = rr->rr_col[0].rc_size;
	for (int c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];

		rw->rw_size[c] = rc->rc_size;
		rw->rw_borrowed[c] = B_FALSE;
		rw->rw_written[c] = B_FALSE;
		if (rc->rc_size == 0 || rc->rc_abd == NULL) {
			rw->rw_size[c] = 0;
			rw->rw_buf[c] = NULL;
		} else if (abd_is_linear(rc->rc_abd)) {
			rw->rw_buf[c] = abd_to_buf(rc->rc_abd);
		} else {
			rw->rw_buf[c] = abd_borrow_buf_copy(rc->rc_abd,
			    rc->rc_size);
			rw->rw_borrowed[c] = B_TRUE;
		}
		ASSERT3U(rw->rw_size[c], <=, rw->rw_psize);
	}
	for (int r = 0; r < m; r++)
		for (int j = 0; j < k; j++)
			rw->rw_a[r][j] = rzec_coef(code, r, j, k);
}

static void
rzec_row_exit(rzec_row_t *rw, raidz_row_t *rr)
{
	for (int c = 0; c < rr->rr_cols; c++) {
		if (!rw->rw_borrowed[c])
			continue;
		if (rw->rw_written[c])
			abd_return_buf_copy(rr->rr_col[c].rc_abd,
			    rw->rw_buf[c], rw->rw_size[c]);
		else
			abd_return_buf(rr->rr_col[c].rc_abd,
			    rw->rw_buf[c], rw->rw_size[c]);
	}
}

static inline size_t
rzec_clip(size_t size, size_t off, size_t len)
{
	if (size <= off)
		return (0);
	return (MIN(size - off, len));
}

static int
rzec_cmp_size(const void *a, const void *b)
{
	size_t x = *(const size_t *)a, y = *(const size_t *)b;

	return (x < y ? -1 : x > y);
}

/*
 * Run a dot product with the selected SIMD kernel.  Columns differ in
 * length by whole sectors, so the offsets are cut at every distinct
 * length into regions over which the same columns take part.  In each
 * region, one output whose coefficients are all 1 (P, or a lone lost data
 * column) is a plain XOR.  A region whose length is not a multiple of the
 * kernel's step, which a RAID-Z map does not produce, is done in scalar.
 */
static void
rzec_dot_run(rzec_dot_t *dt, uint8_t *tab)
{
	const rzec_simd_t *impl = rzec_simd;
	const size_t ts = impl->rs_tabsize;
	size_t *cut = dt->dt_cut;
	int ncut = 0;
	size_t a = 0;

	for (int d = 0; d < dt->dt_nd; d++)
		cut[ncut++] = dt->dt_dlen[d];
	for (int j = 0; j < dt->dt_ns; j++)
		cut[ncut++] = dt->dt_slen[j];
	qsort(cut, ncut, sizeof (size_t), rzec_cmp_size);

	kfpu_begin();
	for (int q = 0; q < ncut; q++) {
		size_t b = cut[q], len = b - a;
		uint8_t *dst[RZEC_MAXPARITY];
		const uint8_t **src = dt->dt_rsrc;
		int dsel[RZEC_MAXPARITY], *ssel = dt->dt_ssel;
		int nd = 0, ns = 0, nx = 0;

		if (b <= a)
			continue;
		for (int d = 0; d < dt->dt_nd; d++)
			if (dt->dt_dlen[d] >= b)
				dsel[nd++] = d;
		for (int j = 0; j < dt->dt_ns; j++)
			if (dt->dt_slen[j] >= b)
				ssel[ns++] = j;
		if (nd == 0)
			break;

		for (int e = 0; e < nd && nx == 0; e++) {
			boolean_t ones = B_TRUE;
			for (int i = 0; i < ns && ones; i++)
				ones = dt->dt_c[dsel[e]][ssel[i]] == 1;
			if (ones) {
				int t = dsel[0];
				dsel[0] = dsel[e];
				dsel[e] = t;
				nx = 1;
			}
		}
		for (int e = 0; e < nd; e++)
			dst[e] = dt->dt_dst[dsel[e]] + a;
		for (int i = 0; i < ns; i++)
			src[i] = dt->dt_src[ssel[i]] + a;

		if (len % impl->rs_step != 0) {
			for (int e = 0; e < nd; e++) {
				memset(dst[e], 0, len);
				for (int i = 0; i < ns; i++)
					rzec_mul_acc(dst[e], src[i], len,
					    dt->dt_c[dsel[e]][ssel[i]]);
			}
			a = b;
			continue;
		}

		for (int i = 0; i < ns; i++)
			for (int e = nx; e < nd; e++)
				memcpy(tab + (i * (nd - nx) + e - nx) * ts,
				    impl->rs_tab +
				    dt->dt_c[dsel[e]][ssel[i]] * ts, ts);
		impl->rs_dot(dst, nx, nd - nx, src, ns, tab, len);
		a = b;
	}
	kfpu_end();
}

/* The parity rows in `rows' from the data columns, in one pass. */
static void
rzec_encode_simd(rzec_row_t *rw, uint64_t rows)
{
	rzec_dot_t *dt = &rw->rw_dot;
	const int m = rw->rw_m, k = rw->rw_k;
	int row[RZEC_MAXPARITY];

	dt->dt_nd = 0;
	dt->dt_ns = 0;
	for (int r = 0; r < m; r++) {
		if (!(rows & (1ULL << r)))
			continue;
		row[dt->dt_nd] = r;
		dt->dt_dst[dt->dt_nd] = rw->rw_buf[r];
		dt->dt_dlen[dt->dt_nd] = rw->rw_size[r];
		dt->dt_nd++;
	}
	for (int j = 0; j < k; j++) {
		int s = dt->dt_ns;
		if (rw->rw_size[m + j] == 0)
			continue;
		dt->dt_src[s] = rw->rw_buf[m + j];
		dt->dt_slen[s] = rw->rw_size[m + j];
		for (int d = 0; d < dt->dt_nd; d++)
			dt->dt_c[d][s] = rw->rw_a[row[d]][j];
		dt->dt_ns++;
	}
	rzec_dot_run(dt, rw->rw_tab);
}

/*
 * The lost data columns straight from the surviving columns: with
 * B = A[use][lost], D_lost = B^-1 * (P_use + A[use][survivors] * D_surv),
 * so the coefficient of P_use[i] is B^-1[l][i] and that of a surviving
 * D_j is sum_i B^-1[l][i] * A[use[i]][j].  One pass, no syndrome buffer.
 */
static void
rzec_rebuild_simd(rzec_row_t *rw, const int *use, const int *lost,
    int nlost, uint8_t binv[RZEC_MAXPARITY][RZEC_MAXPARITY])
{
	rzec_dot_t *dt = &rw->rw_dot;
	const int m = rw->rw_m, k = rw->rw_k;

	dt->dt_nd = nlost;
	dt->dt_ns = 0;
	for (int l = 0; l < nlost; l++) {
		dt->dt_dst[l] = rw->rw_buf[m + lost[l]];
		dt->dt_dlen[l] = rw->rw_size[m + lost[l]];
	}
	for (int i = 0; i < nlost; i++) {
		int s = dt->dt_ns++;
		dt->dt_src[s] = rw->rw_buf[use[i]];
		dt->dt_slen[s] = rw->rw_size[use[i]];
		for (int l = 0; l < nlost; l++)
			dt->dt_c[l][s] = binv[l][i];
	}
	for (int j = 0; j < k; j++) {
		int s = dt->dt_ns;
		if (rw->rw_lost[m + j] || rw->rw_size[m + j] == 0)
			continue;
		dt->dt_src[s] = rw->rw_buf[m + j];
		dt->dt_slen[s] = rw->rw_size[m + j];
		for (int l = 0; l < nlost; l++) {
			uint8_t c = 0;
			for (int i = 0; i < nlost; i++)
				c ^= rzec_mul_lt[binv[l][i]]
				    [rw->rw_a[use[i]][j]];
			dt->dt_c[l][s] = c;
		}
		dt->dt_ns++;
	}
	rzec_dot_run(dt, rw->rw_tab);
}

/*
 * Compute the parity rows in `rows' (a bit mask) from the data columns.
 * Each chunk of the parities stays in cache while every data column is
 * read once.
 */
static void
rzec_encode(rzec_row_t *rw, uint64_t rows)
{
	const int m = rw->rw_m, k = rw->rw_k;
	uint8_t *dst[RZEC_MAXPARITY];
	uint8_t c[RZEC_MAXPARITY];
	int row[RZEC_MAXPARITY];
	int nd = 0;

	if (rzec_simd != NULL) {
		rzec_encode_simd(rw, rows);
		return;
	}
	for (int r = 0; r < m; r++)
		if (rows & (1ULL << r))
			row[nd++] = r;

	for (size_t off = 0; off < rw->rw_psize; off += RZEC_CHUNK) {
		size_t len = MIN(RZEC_CHUNK, rw->rw_psize - off);

		for (int d = 0; d < nd; d++) {
			dst[d] = rw->rw_buf[row[d]] + off;
			memset(dst[d], 0, len);
		}
		for (int j = 0; j < k; j++) {
			size_t dlen = rzec_clip(rw->rw_size[m + j], off, len);
			if (dlen == 0)
				continue;
			for (int d = 0; d < nd; d++)
				c[d] = rw->rw_a[row[d]][j];
			rzec_mad(dst, c, nd, rw->rw_buf[m + j] + off, dlen);
		}
	}
}

void
rzec_generate(raidz_map_t *rm, rzec_code_t code)
{
	rzec_row_t *rw = rzec_row_get();

	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];

		rzec_row_enter(rw, rr, code);
		rzec_encode(rw, (1ULL << rw->rw_m) - 1);
		for (int c = 0; c < rw->rw_m; c++)
			rw->rw_written[c] = B_TRUE;
		rzec_row_exit(rw, rr);
	}
}

/*
 * Invert the n x n matrix a in place by Gauss-Jordan elimination with
 * row pivoting.  Returns nonzero if a is singular.
 */
static int
rzec_invert(uint8_t a[RZEC_MAXPARITY][RZEC_MAXPARITY], int n)
{
	uint8_t inv[RZEC_MAXPARITY][RZEC_MAXPARITY] = { { 0 } };

	for (int i = 0; i < n; i++)
		inv[i][i] = 1;

	for (int c = 0; c < n; c++) {
		int p = c;
		while (p < n && a[p][c] == 0)
			p++;
		if (p == n)
			return (EINVAL);
		if (p != c) {
			for (int j = 0; j < n; j++) {
				uint8_t t = a[c][j];
				a[c][j] = a[p][j];
				a[p][j] = t;
				t = inv[c][j];
				inv[c][j] = inv[p][j];
				inv[p][j] = t;
			}
		}
		uint8_t s = gf_inv(a[c][c]);
		for (int j = 0; j < n; j++) {
			a[c][j] = gf_mul(a[c][j], s);
			inv[c][j] = gf_mul(inv[c][j], s);
		}
		for (int i = 0; i < n; i++) {
			uint8_t f = a[i][c];
			if (i == c || f == 0)
				continue;
			for (int j = 0; j < n; j++) {
				a[i][j] ^= gf_mul(f, a[c][j]);
				inv[i][j] ^= gf_mul(f, inv[c][j]);
			}
		}
	}
	memcpy(a, inv, sizeof (inv));
	return (0);
}

static int
rzec_reconstruct_row(raidz_row_t *rr, rzec_code_t code, const int *tgts,
    int ntgts)
{
	rzec_row_t *rw = rzec_row_get();
	uint8_t b[RZEC_MAXPARITY][RZEC_MAXPARITY];
	uint8_t (*syn)[RZEC_CHUNK] = rw->rw_syn;
	uint8_t (*sol)[RZEC_CHUNK] = rw->rw_sol;
	boolean_t *islost = rw->rw_lost;
	int lost[RZEC_MAXPARITY], use[RZEC_MAXPARITY];
	uint64_t badparity = 0;
	int nlost = 0, nuse = 0, err = 0;
	int m, k;

	rzec_row_enter(rw, rr, code);
	m = rw->rw_m;
	k = rw->rw_k;
	memset(islost, 0, sizeof (rw->rw_lost));

	/* Columns past the row or of length zero have nothing to rebuild. */
	for (int t = 0; t < ntgts; t++) {
		int c = tgts[t];
		if (c < 0 || c >= rr->rr_cols || rw->rw_size[c] == 0 ||
		    islost[c])
			continue;
		if (c >= m && nlost == m) {
			err = EINVAL;
			goto out;
		}
		islost[c] = B_TRUE;
		if (c < m)
			badparity |= 1ULL << c;
		else
			lost[nlost++] = c - m;
	}

	for (int r = 0; r < m && nuse < nlost; r++)
		if (!(badparity & (1ULL << r)))
			use[nuse++] = r;
	if (nuse < nlost) {
		err = EINVAL;
		goto out;
	}

	if (nlost > 0) {
		for (int i = 0; i < nlost; i++)
			for (int l = 0; l < nlost; l++)
				b[i][l] = rw->rw_a[use[i]][lost[l]];
		if ((err = rzec_invert(b, nlost)) != 0)
			goto out;
	}

	if (nlost > 0 && rzec_simd != NULL) {
		rzec_rebuild_simd(rw, use, lost, nlost, b);
	} else if (nlost > 0) {
		/*
		 * Syndrome S_i = P_use[i] + sum over surviving data of
		 * A[use[i]][j] * D_j, then D_lost[l] = sum_i Binv[l][i] * S_i.
		 */
		for (size_t off = 0; off < rw->rw_psize; off += RZEC_CHUNK) {
			size_t len = MIN(RZEC_CHUNK, rw->rw_psize - off);
			uint8_t *sp[RZEC_MAXPARITY], *dp[RZEC_MAXPARITY];
			uint8_t c[RZEC_MAXPARITY];

			for (int i = 0; i < nlost; i++) {
				sp[i] = syn[i];
				dp[i] = sol[i];
				memcpy(syn[i], rw->rw_buf[use[i]] + off, len);
				memset(sol[i], 0, len);
			}
			for (int j = 0; j < k; j++) {
				size_t dlen = rzec_clip(rw->rw_size[m + j],
				    off, len);
				if (islost[m + j] || dlen == 0)
					continue;
				for (int i = 0; i < nlost; i++)
					c[i] = rw->rw_a[use[i]][j];
				rzec_mad(sp, c, nlost, rw->rw_buf[m + j] + off,
				    dlen);
			}
			for (int i = 0; i < nlost; i++) {
				for (int l = 0; l < nlost; l++)
					c[l] = b[l][i];
				rzec_mad(dp, c, nlost, syn[i], len);
			}
			for (int l = 0; l < nlost; l++) {
				int col = m + lost[l];
				size_t dlen = rzec_clip(rw->rw_size[col],
				    off, len);
				if (dlen != 0)
					memcpy(rw->rw_buf[col] + off, sol[l],
					    dlen);
			}
		}
	}

	if (badparity != 0)
		rzec_encode(rw, badparity);
	for (int c = 0; c < rr->rr_cols; c++)
		rw->rw_written[c] = islost[c];
out:
	rzec_row_exit(rw, rr);
	return (err);
}

int
rzec_reconstruct(raidz_map_t *rm, rzec_code_t code, const int *tgts,
    int ntgts)
{
	int err = 0;

	for (int i = 0; i < rm->rm_nrows && err == 0; i++)
		err = rzec_reconstruct_row(rm->rm_row[i], code, tgts, ntgts);
	return (err);
}

/*
 * Correctness checks
 */

typedef struct rzec_golden {
	int		rg_nrows;
	int		rg_ncols;
	uint8_t		**rg_copy;	/* [row * ncols + col] */
} rzec_golden_t;

static void
rzec_golden_save(rzec_golden_t *g, raidz_map_t *rm)
{
	g->rg_nrows = rm->rm_nrows;
	g->rg_ncols = rm->rm_row[0]->rr_cols;
	g->rg_copy = kmem_zalloc(sizeof (uint8_t *) * g->rg_nrows *
	    g->rg_ncols, KM_SLEEP);
	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];
		VERIFY3S(rr->rr_cols, ==, g->rg_ncols);
		for (int c = 0; c < rr->rr_cols; c++) {
			raidz_col_t *rc = &rr->rr_col[c];
			if (rc->rc_size == 0 || rc->rc_abd == NULL)
				continue;
			uint8_t *p = kmem_alloc(rc->rc_size, KM_SLEEP);
			abd_copy_to_buf(p, rc->rc_abd, rc->rc_size);
			g->rg_copy[i * g->rg_ncols + c] = p;
		}
	}
}

static void
rzec_golden_free(rzec_golden_t *g, raidz_map_t *rm)
{
	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];
		for (int c = 0; c < rr->rr_cols; c++) {
			uint8_t *p = g->rg_copy[i * g->rg_ncols + c];
			if (p != NULL)
				kmem_free(p, rr->rr_col[c].rc_size);
		}
	}
	kmem_free(g->rg_copy, sizeof (uint8_t *) * g->rg_nrows * g->rg_ncols);
}

/* Number of columns that differ from the golden copy; restores them. */
static int
rzec_golden_cmp(rzec_golden_t *g, raidz_map_t *rm)
{
	int bad = 0;

	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];
		for (int c = 0; c < rr->rr_cols; c++) {
			raidz_col_t *rc = &rr->rr_col[c];
			uint8_t *p = g->rg_copy[i * g->rg_ncols + c];
			if (p == NULL)
				continue;
			if (abd_cmp_buf(rc->rc_abd, p, rc->rc_size) != 0) {
				bad++;
				abd_copy_from_buf(rc->rc_abd, p, rc->rc_size);
			}
		}
	}
	return (bad);
}

static int
rzec_fill_aa(void *buf, size_t size, void *private)
{
	(void) private;
	memset(buf, 0xAA, size);
	return (0);
}

static void
rzec_corrupt(raidz_map_t *rm, const int *tgts, int ntgts)
{
	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];
		for (int t = 0; t < ntgts; t++) {
			if (tgts[t] >= rr->rr_cols)
				continue;
			raidz_col_t *rc = &rr->rr_col[tgts[t]];
			if (rc->rc_size == 0 || rc->rc_abd == NULL)
				continue;
			abd_iterate_func(rc->rc_abd, 0, rc->rc_size,
			    rzec_fill_aa, NULL);
		}
	}
}

typedef struct rzec_stats {
	uint64_t	rs_sets;
	uint64_t	rs_fail;
	uint64_t	rs_singular;
	int		rs_first_fail[RZEC_MAXPARITY];
	int		rs_first_nfail;
	rzec_kernel_t	rs_first_kernel;
} rzec_stats_t;

static void
rzec_try_kernel(raidz_map_t *rm, rzec_golden_t *g, rzec_code_t code,
    const int *tgts, int ntgts, rzec_stats_t *st)
{
	int err = 0, bad;

	rzec_corrupt(rm, tgts, ntgts);
	if (!rto_opts.rto_sanity)
		err = rzec_reconstruct(rm, code, tgts, ntgts);
	bad = rzec_golden_cmp(g, rm);
	st->rs_sets++;
	if (err != 0)
		st->rs_singular++;
	if (err != 0 || bad != 0) {
		if (st->rs_fail == 0) {
			memcpy(st->rs_first_fail, tgts,
			    ntgts * sizeof (int));
			st->rs_first_nfail = ntgts;
			st->rs_first_kernel = rzec_kernel;
		}
		st->rs_fail++;
	}
}

/* Rebuild with every kernel -k selects. */
static void
rzec_try(raidz_map_t *rm, rzec_golden_t *g, rzec_code_t code,
    const int *tgts, int ntgts, rzec_stats_t *st)
{
	for (int k = 0; k < rzec_nkernels(); k++) {
		if (!rzec_kernel_selected(k))
			continue;
		rzec_set_kernel(k);
		rzec_try_kernel(rm, g, code, tgts, ntgts, st);
	}
	rzec_set_kernel(RZEC_KERNEL_TABLE);
}

/* Every set of 1 .. maxlost columns among ncols, in lexicographic order. */
static void
rzec_try_all(raidz_map_t *rm, rzec_golden_t *g, rzec_code_t code,
    int ncols, int maxlost, rzec_stats_t *st)
{
	int t[RZEC_MAXPARITY];

	for (int n = 1; n <= maxlost && n <= ncols; n++) {
		for (int i = 0; i < n; i++)
			t[i] = i;
		for (;;) {
			if (rto_opts.rto_should_stop)
				return;
			rzec_try(rm, g, code, t, n, st);
			int i = n - 1;
			while (i >= 0 && t[i] == ncols - n + i)
				i--;
			if (i < 0)
				break;
			t[i]++;
			for (int j = i + 1; j < n; j++)
				t[j] = t[j - 1] + 1;
		}
	}
}

/* Every single column, then `samples' random sets of exactly maxlost. */
static void
rzec_try_sample(raidz_map_t *rm, rzec_golden_t *g, rzec_code_t code,
    int ncols, int maxlost, uint64_t samples, unsigned *seed,
    rzec_stats_t *st)
{
	int t[RZEC_MAXPARITY];
	int n = MIN(maxlost, ncols);

	for (int c = 0; c < ncols; c++) {
		t[0] = c;
		rzec_try(rm, g, code, t, 1, st);
	}
	for (uint64_t s = 0; s < samples; s++) {
		for (int i = 0; i < n; i++) {
			boolean_t dup;
			do {
				t[i] = rand_r(seed) % ncols;
				dup = B_FALSE;
				for (int j = 0; j < i; j++)
					if (t[j] == t[i])
						dup = B_TRUE;
			} while (dup);
		}
		rzec_try(rm, g, code, t, n, st);
	}
}

/* Parity must match vdev_raidz_generate_parity() where the code exists. */
static int
rzec_check_golden_parity(raidz_map_t *rm, zio_t *zio, int ashift, int n,
    int m)
{
	zio_t z2 = *zio;
	raidz_map_t *rm2;
	int bad = 0;

	z2.io_abd = abd_alloc_linear(zio->io_size, B_FALSE);
	abd_copy(z2.io_abd, zio->io_abd, zio->io_size);
	if (rto_opts.rto_expand)
		rm2 = vdev_raidz_map_alloc_expanded(&z2, ashift, n + 1, n, m,
		    rto_opts.rto_expand_offset, 0, B_FALSE);
	else
		rm2 = vdev_raidz_map_alloc(&z2, ashift, n, m);
	VERIFY0(vdev_raidz_impl_set("original"));
	vdev_raidz_generate_parity(rm2);
	for (int i = 0; i < rm->rm_nrows; i++)
		for (int c = 0; c < m; c++)
			if (abd_cmp(rm->rm_row[i]->rr_col[c].rc_abd,
			    rm2->rm_row[i]->rr_col[c].rc_abd) != 0)
				bad++;
	vdev_raidz_map_free(rm2);
	abd_free(z2.io_abd);
	return (bad);
}

/* One data column: every parity column must equal it. */
static int
rzec_check_single_column(raidz_map_t *rm)
{
	int bad = 0;

	for (int i = 0; i < rm->rm_nrows; i++) {
		raidz_row_t *rr = rm->rm_row[i];
		if (rr->rr_cols != rr->rr_firstdatacol + 1)
			continue;
		abd_t *d = rr->rr_col[rr->rr_firstdatacol].rc_abd;
		for (int c = 0; c < rr->rr_firstdatacol; c++)
			if (abd_cmp(rr->rr_col[c].rc_abd, d) != 0)
				bad++;
	}
	return (bad);
}

typedef struct rzec_case {
	int		rc_ashift;
	uint64_t	rc_sectors;
	boolean_t	rc_exhaustive;
	boolean_t	rc_scatter;
} rzec_case_t;

static int
rzec_run_case(rzec_code_t code, int m, int n, const rzec_case_t *cs,
    unsigned *seed, rzec_stats_t *total)
{
	zio_t zio = { 0 };
	raidz_map_t *rm;
	rzec_golden_t g;
	rzec_stats_t st = { 0 };
	int k = n - m;
	int extra = 0;
	size_t size = cs->rc_sectors << cs->rc_ashift;

	if (size > SPA_MAXBLOCKSIZE)
		return (0);

	zio.io_offset = 0;
	zio.io_size = size;
	zio.io_abd = cs->rc_scatter ? abd_alloc(size, B_FALSE) :
	    abd_alloc_linear(size, B_FALSE);
	init_zio_abd(&zio);

	if (rto_opts.rto_expand)
		rm = vdev_raidz_map_alloc_expanded(&zio, cs->rc_ashift, n + 1,
		    n, m, rto_opts.rto_expand_offset, 0, B_FALSE);
	else
		rm = vdev_raidz_map_alloc(&zio, cs->rc_ashift, n, m);

	rzec_generate(rm, code);
	if (code == RZEC_VAND && m <= PARITY_PQR)
		extra += rzec_check_golden_parity(rm, &zio, cs->rc_ashift,
		    n, m);
	if (code == RZEC_CAUCHY)
		extra += rzec_check_single_column(rm);

	rzec_golden_save(&g, rm);
	/* Every other kernel must produce the same parity, byte for byte. */
	for (int kn = 0; kn < rzec_nkernels(); kn++) {
		int bad;
		if (kn == RZEC_KERNEL_TABLE || !rzec_kernel_available(kn))
			continue;
		rzec_set_kernel(kn);
		rzec_generate(rm, code);
		if ((bad = rzec_golden_cmp(&g, rm)) != 0)
			LOG(D_ALL, "parity of kernel %s differs: %d columns\n",
			    rzec_kernel_name(kn), bad);
		extra += bad;
	}
	rzec_set_kernel(RZEC_KERNEL_TABLE);
	int ncols = rm->rm_row[0]->rr_cols;
	if (cs->rc_exhaustive)
		rzec_try_all(rm, &g, code, ncols, m, &st);
	else
		rzec_try_sample(rm, &g, code, ncols, m,
		    rto_opts.rto_ec_samples, seed, &st);
	rzec_golden_free(&g, rm);

	LOG(D_ALL, "%-6s m=%d n=%3d k=%3d ashift=%2d sectors=%5llu "
	    "rows=%d cols=%3d %-6s %-5s sets=%8llu fail=%llu singular=%llu"
	    " invariant_fail=%d",
	    rzec_code_name(code), m, n, k, cs->rc_ashift,
	    (u_longlong_t)cs->rc_sectors, rm->rm_nrows, ncols,
	    cs->rc_exhaustive ? "all" : "sample",
	    cs->rc_scatter ? "scat" : "lin",
	    (u_longlong_t)st.rs_sets, (u_longlong_t)st.rs_fail,
	    (u_longlong_t)st.rs_singular, extra);
	if (st.rs_fail != 0) {
		LOG(D_ALL, " first_fail={");
		for (int i = 0; i < st.rs_first_nfail; i++)
			LOG(D_ALL, "%s%d", i ? "," : "", st.rs_first_fail[i]);
		LOG(D_ALL, "} kernel=%s", rzec_kernel_name(st.rs_first_kernel));
	}
	LOG(D_ALL, "\n");

	vdev_raidz_map_free(rm);
	abd_free(zio.io_abd);

	total->rs_sets += st.rs_sets;
	total->rs_fail += st.rs_fail;
	total->rs_singular += st.rs_singular;
	return (st.rs_fail != 0 || extra != 0);
}

static int
rzec_run_width(rzec_code_t code, int m, int n, boolean_t exhaustive,
    unsigned *seed, rzec_stats_t *total)
{
	int k = n - m;
	int err = 0;
	/* A size that leaves a partial last row: k/2 + 1 big columns. */
	uint64_t irregular = 3 * (uint64_t)k + k / 2 + 1;
	rzec_case_t cases[] = {
		{ 9, irregular, exhaustive, B_FALSE },
		{ 9, 1, n <= 12, B_FALSE },
		{ 9, MAX(1, k - 1), n <= 12, B_FALSE },
		{ 9, k, n <= 12, B_FALSE },
		{ 9, 2 * (uint64_t)k + 1, n <= 12, B_FALSE },
		{ 12, 5 * (uint64_t)k + 3, B_FALSE, B_FALSE },
		{ 12, 32, B_FALSE, B_TRUE },
	};

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		if (rto_opts.rto_should_stop)
			break;
		err |= rzec_run_case(code, m, n, &cases[i], seed, total);
	}
	return (err);
}

int
run_rzec_check(void)
{
	static const int sample_widths[] = { 48, 64, 96, 128, 192, 255 };
	rzec_code_t code = rto_opts.rto_ec_vand ? RZEC_VAND : RZEC_CAUCHY;
	int mlo = rto_opts.rto_ec_parity ? rto_opts.rto_ec_parity : 4;
	int mhi = rto_opts.rto_ec_parity ? rto_opts.rto_ec_parity : 6;
	unsigned seed = rto_opts.rto_ec_seed;
	rzec_stats_t total = { 0 };
	int err = 0, fails = 0;
	hrtime_t start = gethrtime();

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	LOG(D_ALL, "generic code check: code=%s m=%d..%d exhaustive up to "
	    "n=%zu, %llu random sets otherwise, seed=%u, %s map%s\n",
	    rzec_code_name(code), mlo, mhi, rto_opts.rto_ec_width,
	    (u_longlong_t)rto_opts.rto_ec_samples, seed,
	    rto_opts.rto_expand ? "expanded" : "plain",
	    rto_opts.rto_sanity ? " (sanity: reconstruction skipped)" : "");
	LOG(D_ALL, "parity cross-checked against kernel table with:");
	for (int kn = 0; kn < rzec_nkernels(); kn++)
		if (kn != RZEC_KERNEL_TABLE && rzec_kernel_available(kn))
			LOG(D_ALL, " %s", rzec_kernel_name(kn));
	LOG(D_ALL, "\nrebuilt with:");
	for (int kn = 0; kn < rzec_nkernels(); kn++)
		if (rzec_kernel_selected(kn))
			LOG(D_ALL, " %s", rzec_kernel_name(kn));
	LOG(D_ALL, "\n");

	for (int m = mlo; m <= mhi; m++) {
		int nmax = MIN(255, m + rzec_max_data(code));
		for (int n = m + 1; n <= MIN(nmax,
		    (int)rto_opts.rto_ec_width); n++) {
			if (rzec_run_width(code, m, n, B_TRUE, &seed,
			    &total) != 0) {
				err = 1;
				if (++fails >= 3 && code == RZEC_VAND)
					break;
			}
		}
		if (code == RZEC_VAND && fails >= 3)
			continue;
		for (int i = 0; i < ARRAY_SIZE(sample_widths); i++) {
			int n = sample_widths[i];
			if (n <= (int)rto_opts.rto_ec_width || n > nmax)
				continue;
			err |= rzec_run_width(code, m, n, B_FALSE, &seed,
			    &total);
		}
		fails = 0;
	}

	LOG(D_ALL, "total: sets=%llu fail=%llu singular=%llu in %.1fs: %s\n",
	    (u_longlong_t)total.rs_sets, (u_longlong_t)total.rs_fail,
	    (u_longlong_t)total.rs_singular,
	    NSEC2SEC((double)(gethrtime() - start)),
	    err ? "FAIL" : "PASS");
	return (err);
}

/*
 * Benchmark: the existing P, PQ and PQR implementation against the
 * generic code, on the same linear buffers, map geometry and block size.
 */

#define	RZEC_BENCH_ASHIFT	12
#define	RZEC_BENCH_ROUNDS	9
#define	RZEC_BENCH_NSEC		(200ULL * MICROSEC)
#define	RZEC_BENCH_MAXPOINTS	256

typedef void (*rzec_bench_f)(raidz_map_t *, const int *, int);

static void
rzec_bench_gen_existing(raidz_map_t *rm, const int *t, int nt)
{
	(void) t, (void) nt;
	vdev_raidz_generate_parity(rm);
}

static void
rzec_bench_rec_existing(raidz_map_t *rm, const int *t, int nt)
{
	(void) vdev_raidz_reconstruct(rm, t, nt);
}

static void
rzec_bench_gen_cauchy(raidz_map_t *rm, const int *t, int nt)
{
	(void) t, (void) nt;
	rzec_generate(rm, RZEC_CAUCHY);
}

static void
rzec_bench_rec_cauchy(raidz_map_t *rm, const int *t, int nt)
{
	VERIFY0(rzec_reconstruct(rm, RZEC_CAUCHY, t, nt));
}

typedef struct rzec_point {
	const char	*bp_impl;	/* vdev_raidz_impl_set() name or NULL */
	rzec_kernel_t	bp_kernel;
	const char	*bp_op;
	int		bp_m;
	int		bp_nlost;
	uint64_t	bp_size;
	rzec_bench_f	bp_f;
	double		bp_mibs[RZEC_BENCH_ROUNDS];
} rzec_point_t;

static int
rzec_cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x < y ? -1 : x > y);
}

/* Data throughput in MiB/s of one point, timed for RZEC_BENCH_NSEC. */
static double
rzec_bench_measure(rzec_point_t *bp)
{
	int k = rto_opts.rto_dcols;
	int tgts[RZEC_MAXPARITY];
	zio_t zio = { 0 };
	raidz_map_t *rm;
	hrtime_t start, now;
	uint64_t iters = 0;

	if (bp->bp_impl != NULL)
		VERIFY0(vdev_raidz_impl_set(bp->bp_impl));
	rzec_set_kernel(bp->bp_kernel);

	zio.io_size = bp->bp_size;
	zio.io_abd = abd_alloc_linear(bp->bp_size, B_FALSE);
	init_zio_abd(&zio);
	rm = vdev_raidz_map_alloc(&zio, RZEC_BENCH_ASHIFT, k + bp->bp_m,
	    bp->bp_m);
	if (bp->bp_impl != NULL)
		vdev_raidz_generate_parity(rm);
	else
		rzec_generate(rm, RZEC_CAUCHY);
	/* Lose the first nlost data columns. */
	for (int i = 0; i < bp->bp_nlost; i++)
		tgts[i] = bp->bp_m + i;

	bp->bp_f(rm, tgts, bp->bp_nlost);
	start = gethrtime();
	do {
		for (int i = 0; i < 8; i++)
			bp->bp_f(rm, tgts, bp->bp_nlost);
		iters += 8;
		now = gethrtime();
	} while (now - start < RZEC_BENCH_NSEC);

	vdev_raidz_map_free(rm);
	abd_free(zio.io_abd);
	rzec_set_kernel(RZEC_KERNEL_TABLE);

	return ((double)iters * (double)bp->bp_size /
	    (1024.0 * 1024.0 * NSEC2SEC((double)(now - start))));
}

static void
rzec_bench_add(rzec_point_t *bp, int *np, const char *impl,
    rzec_kernel_t kernel, const char *op, int m, int nlost, uint64_t size,
    rzec_bench_f f)
{
	if (nlost > (int)rto_opts.rto_dcols)
		return;
	VERIFY3S(*np, <, RZEC_BENCH_MAXPOINTS);
	rzec_point_t *p = &bp[(*np)++];
	p->bp_impl = impl;
	p->bp_kernel = kernel;
	p->bp_op = op;
	p->bp_m = m;
	p->bp_nlost = nlost;
	p->bp_size = size;
	p->bp_f = f;
}

void
run_rzec_benchmark(void)
{
	static const uint64_t sizes[] = { 1ULL << 17, 1ULL << 20 };
	static const char *const impls[] = { "scalar", "avx2", "avx512bw" };
	rzec_point_t *bp = kmem_zalloc(sizeof (*bp) * RZEC_BENCH_MAXPOINTS,
	    KM_SLEEP);
	int np = 0;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);

	/*
	 * The existing raidz3 code at its own parity count, against the
	 * generic code from 3 (not the on-disk code there) to 6 parities.
	 */
	for (int s = 0; s < ARRAY_SIZE(sizes); s++) {
		uint64_t sz = sizes[s];
		for (int i = 0; i < ARRAY_SIZE(impls); i++) {
			if (vdev_raidz_impl_set(impls[i]) != 0)
				continue;
			rzec_bench_add(bp, &np, impls[i], 0, "gen_pqr",
			    PARITY_PQR, 0, sz, rzec_bench_gen_existing);
			rzec_bench_add(bp, &np, impls[i], 0, "rec_pqr",
			    PARITY_PQR, PARITY_PQR, sz,
			    rzec_bench_rec_existing);
			rzec_bench_add(bp, &np, impls[i], 0, "rec_p",
			    PARITY_PQR, 1, sz, rzec_bench_rec_existing);
		}
		for (int kn = RZEC_KERNEL_TABLE; kn < rzec_nkernels(); kn++) {
			if (!rzec_kernel_available(kn))
				continue;
			for (int m = PARITY_PQR; m <= RZEC_MAXPARITY; m++)
				rzec_bench_add(bp, &np, NULL, kn, "gen", m, 0,
				    sz, rzec_bench_gen_cauchy);
			for (int m = PARITY_PQR; m <= RZEC_MAXPARITY; m++)
				rzec_bench_add(bp, &np, NULL, kn, "rec", m, m,
				    sz, rzec_bench_rec_cauchy);
			for (int m = 4; m <= RZEC_MAXPARITY; m += 2)
				rzec_bench_add(bp, &np, NULL, kn, "rec", m, 1,
				    sz, rzec_bench_rec_cauchy);
		}
	}

/* Rounds alternate every point, so that drift hits all of them. */
	for (int r = 0; r < RZEC_BENCH_ROUNDS; r++) {
		LOG(D_INFO, "round %d/%d\n", r + 1, RZEC_BENCH_ROUNDS);
		for (int i = 0; i < np; i++)
			bp[i].bp_mibs[r] = rzec_bench_measure(&bp[i]);
	}

	LOG(D_ALL, "impl, op, parity, lost_data_cols, data_cols, iosize, "
	    "median_MiB/s, min_MiB/s, max_MiB/s, rounds\n");
	for (int i = 0; i < np; i++) {
		double v[RZEC_BENCH_ROUNDS];
		memcpy(v, bp[i].bp_mibs, sizeof (v));
		qsort(v, RZEC_BENCH_ROUNDS, sizeof (double), rzec_cmp_double);
		char name[32];
		if (bp[i].bp_impl == NULL)
			(void) snprintf(name, sizeof (name), "cauchy-%s",
			    rzec_kernel_name(bp[i].bp_kernel));
		else
			(void) snprintf(name, sizeof (name), "%s",
			    bp[i].bp_impl);
		LOG(D_ALL, "%-16s, %-7s, %d, %d, %3zu, %8llu, %9.1f, %9.1f, "
		    "%9.1f, %d\n", name,
		    bp[i].bp_op, bp[i].bp_m, bp[i].bp_nlost, rto_opts.rto_dcols,
		    (u_longlong_t)bp[i].bp_size, v[RZEC_BENCH_ROUNDS / 2],
		    v[0], v[RZEC_BENCH_ROUNDS - 1], RZEC_BENCH_ROUNDS);
	}
	kmem_free(bp, sizeof (*bp) * RZEC_BENCH_MAXPOINTS);
}
