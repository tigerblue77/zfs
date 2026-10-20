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
 * Copyright (C) 2016 Gvozden Nešković. All rights reserved.
 */

#ifndef _VDEV_RAIDZ_MATH_IMPL_H
#define	_VDEV_RAIDZ_MATH_IMPL_H

#include <sys/types.h>
#include <sys/vdev_raidz_impl.h>

#define	raidz_inline inline __attribute__((always_inline))
#ifndef noinline
#define	noinline __attribute__((noinline))
#endif

/*
 * Functions calculate multiplication constants for data reconstruction.
 * Coefficients depend on RAIDZ geometry, indexes of failed child vdevs, and
 * used parity columns for reconstruction.
 * @rr			RAIDZ row
 * @tgtidx		array of missing data indexes
 * @coeff		output array of coefficients. Array must be provided by
 *         		user and must hold minimum MUL_CNT values.
 */
static noinline void
raidz_rec_q_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];

	coeff[MUL_Q_X] = gf_exp2(255 - (ncols - x - 1));
}

static noinline void
raidz_rec_r_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];

	coeff[MUL_R_X] = gf_exp4(255 - (ncols - x - 1));
}

static noinline void
raidz_rec_pq_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];
	const unsigned y = tgtidx[TARGET_Y];
	gf_t a, b, e;

	a = gf_exp2(x + 255 - y);
	b = gf_exp2(255 - (ncols - x - 1));
	e = a ^ 0x01;

	coeff[MUL_PQ_X] = gf_div(a, e);
	coeff[MUL_PQ_Y] = gf_div(b, e);
}

static noinline void
raidz_rec_pr_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];
	const unsigned y = tgtidx[TARGET_Y];

	gf_t a, b, e;

	a = gf_exp4(x + 255 - y);
	b = gf_exp4(255 - (ncols - x - 1));
	e = a ^ 0x01;

	coeff[MUL_PR_X] = gf_div(a, e);
	coeff[MUL_PR_Y] = gf_div(b, e);
}

static noinline void
raidz_rec_qr_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];
	const unsigned y = tgtidx[TARGET_Y];

	gf_t nx, ny, nxxy, nxyy, d;

	nx = gf_exp2(ncols - x - 1);
	ny = gf_exp2(ncols - y - 1);
	nxxy = gf_mul(gf_mul(nx, nx), ny);
	nxyy = gf_mul(gf_mul(nx, ny), ny);
	d = nxxy ^ nxyy;

	coeff[MUL_QR_XQ] = ny;
	coeff[MUL_QR_X]	= gf_div(ny, d);
	coeff[MUL_QR_YQ] = nx;
	coeff[MUL_QR_Y]	= gf_div(nx, d);
}

static noinline void
raidz_rec_pqr_coeff(const raidz_row_t *rr, const int *tgtidx, unsigned *coeff)
{
	const unsigned ncols = rr->rr_cols;
	const unsigned x = tgtidx[TARGET_X];
	const unsigned y = tgtidx[TARGET_Y];
	const unsigned z = tgtidx[TARGET_Z];

	gf_t nx, ny, nz, nxx, nyy, nzz, nyyz, nyzz, xd, yd;

	nx = gf_exp2(ncols - x - 1);
	ny = gf_exp2(ncols - y - 1);
	nz = gf_exp2(ncols - z - 1);

	nxx = gf_exp4(ncols - x - 1);
	nyy = gf_exp4(ncols - y - 1);
	nzz = gf_exp4(ncols - z - 1);

	nyyz = gf_mul(gf_mul(ny, nz), ny);
	nyzz = gf_mul(nzz, ny);

	xd = gf_mul(nxx, ny) ^ gf_mul(nx, nyy) ^ nyyz ^
	    gf_mul(nxx, nz) ^ gf_mul(nzz, nx) ^  nyzz;

	yd = gf_inv(ny ^ nz);

	coeff[MUL_PQR_XP] = gf_div(nyyz ^ nyzz, xd);
	coeff[MUL_PQR_XQ] = gf_div(nyy ^ nzz, xd);
	coeff[MUL_PQR_XR] = gf_div(ny ^ nz, xd);
	coeff[MUL_PQR_YU] = nx;
	coeff[MUL_PQR_YP] = gf_mul(nz, yd);
	coeff[MUL_PQR_YQ] = yd;
}

/*
 * Method for copying two buffers (can be implemented using SIMD).
 * This method is used by multiple for gen/rec functions.
 *
 * @dc		Destination buffer
 * @sc		Source buffer
 * @dsize	Destination buffer size
 * @ssize	Source buffer size
 * @private	Unused
 */
static int
raidz_copy_abd_cb(void *dc, void *sc, size_t size, void *private)
{
	v_t *dst = (v_t *)dc;
	const v_t *src = (v_t *)sc;
	size_t i;

	COPY_DEFINE();

	(void) private; /* unused */

	for (i = 0; i < size / sizeof (v_t); i += (2 * COPY_STRIDE)) {
		LOAD(src + i, COPY_D);
		STORE(dst + i, COPY_D);

		LOAD(src + i + COPY_STRIDE, COPY_D);
		STORE(dst + i + COPY_STRIDE, COPY_D);
	}

	return (0);
}


#define	raidz_copy(dabd, sabd, off, size)				\
{									\
	abd_iterate_func2(dabd, sabd, off, off, size, raidz_copy_abd_cb, \
	    NULL);							\
}

/*
 * PARITY CALCULATION
 *
 * Parity is computed over groups of up to RAIDZ_GRP_DCOLS data columns at a
 * time.  The parity of each stride stays in registers while the stride is
 * read from every data column of the group, and is stored once per group:
 * data is read once and parity is written once for every RAIDZ_GRP_DCOLS
 * data columns.  Q and R are the Horner sums Q = 2 * Q + D and R = 4 * R + D
 * over the data columns in order; a column that has ended adds 0.
 *
 * For CPU L2 cache blocking we process 64KB at a time.
 */
#define	CHUNK		65536

/*
 * Data columns per group: each pass also maps up to 3 results and, for
 * reconstruction, up to 3 parity columns.
 */
#define	RAIDZ_GRP_DCOLS	(ABD_RAIDZ_MAXCOLS - 2 * PARITY_PQR)

typedef struct raidz_cols_arg {
	boolean_t	rca_first;	/* first group: the sums start at 0 */
	boolean_t	rca_last;	/* last group: parity columns follow */
	const unsigned	*rca_mul;	/* reconstruction coefficients */
} raidz_cols_arg_t;

#define	RAIDZ_COL(c, i)	((const v_t *)(c) + (i))

/*
 * Index of the first data column to read in @c, from @j: in the first
 * group, the sums start at the first column that holds data, since the
 * columns before it add nothing.
 */
static inline unsigned
raidz_cols_start(void **c, unsigned j, const unsigned dend,
    const boolean_t first)
{
	while (first && j < dend && c[j] == NULL)
		j++;
	return (j);
}

/*
 * Generate P parity (RAIDZ1) over a group of data columns
 *
 * @c		P, then the data columns (NULL once ended)
 * @ncols	number of columns in @c
 * @len		length of every column in @c
 * @private	raidz_cols_arg_t
 */
static void
raidz_gen_p_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const boolean_t first = ((raidz_cols_arg_t *)private)->rca_first;
	const unsigned js = raidz_cols_start(c, 1, ncols, first);
	v_t *p = (v_t *)c[CODE_P];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	GEN_P_DEFINE();

	for (i = 0; i < n; i += GEN_P_STRIDE) {
		j = js;
		if (!first) {
			LOAD(p + i, GEN_P_P);
		} else if (js < ncols) {
			LOAD(RAIDZ_COL(c[js], i), GEN_P_P);
			j = js + 1;
		} else {
			ZERO(GEN_P_P);
		}
		for (; j < ncols; j++) {
			if (c[j] != NULL)
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_P_P);
		}
		STORE(p + i, GEN_P_P);
	}
}

/*
 * Generate PQ parity (RAIDZ2) over a group of data columns
 *
 * @c		P, Q, then the data columns (NULL once ended)
 * @ncols	number of columns in @c
 * @len		length of every column in @c
 * @private	raidz_cols_arg_t
 */
static void
raidz_gen_pq_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const boolean_t first = ((raidz_cols_arg_t *)private)->rca_first;
	const unsigned js = raidz_cols_start(c, 2, ncols, first);
	v_t *p = (v_t *)c[CODE_P];
	v_t *q = (v_t *)c[CODE_Q];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	GEN_PQ_DEFINE();

	MUL2_SETUP();

	for (i = 0; i < n; i += GEN_PQ_STRIDE) {
		j = js;
		if (!first) {
			LOAD(p + i, GEN_PQ_P);
			LOAD(q + i, GEN_PQ_Q);
		} else if (js < ncols) {
			LOAD(RAIDZ_COL(c[js], i), GEN_PQ_P);
			LOAD(RAIDZ_COL(c[js], i), GEN_PQ_Q);
			j = js + 1;
		} else {
			ZERO(GEN_PQ_P);
			ZERO(GEN_PQ_Q);
		}
		for (; j < ncols; j++) {
			MUL2(GEN_PQ_Q);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_PQ_P);
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_PQ_Q);
			}
		}
		STORE(p + i, GEN_PQ_P);
		STORE(q + i, GEN_PQ_Q);
	}
}

/*
 * Generate PQR parity (RAIDZ3) over a group of data columns
 *
 * @c		P, Q, R, then the data columns (NULL once ended)
 * @ncols	number of columns in @c
 * @len		length of every column in @c
 * @private	raidz_cols_arg_t
 */
static void
raidz_gen_pqr_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const boolean_t first = ((raidz_cols_arg_t *)private)->rca_first;
	const unsigned js = raidz_cols_start(c, 3, ncols, first);
	v_t *p = (v_t *)c[CODE_P];
	v_t *q = (v_t *)c[CODE_Q];
	v_t *r = (v_t *)c[CODE_R];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	GEN_PQR_DEFINE();

	MUL2_SETUP();

	for (i = 0; i < n; i += GEN_PQR_STRIDE) {
		j = js;
		if (!first) {
			LOAD(p + i, GEN_PQR_P);
			LOAD(q + i, GEN_PQR_Q);
			LOAD(r + i, GEN_PQR_R);
		} else if (js < ncols) {
			LOAD(RAIDZ_COL(c[js], i), GEN_PQR_P);
			LOAD(RAIDZ_COL(c[js], i), GEN_PQR_Q);
			LOAD(RAIDZ_COL(c[js], i), GEN_PQR_R);
			j = js + 1;
		} else {
			ZERO(GEN_PQR_P);
			ZERO(GEN_PQR_Q);
			ZERO(GEN_PQR_R);
		}
		for (; j < ncols; j++) {
			MUL2(GEN_PQR_Q);
			MUL4(GEN_PQR_R);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_PQR_P);
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_PQR_Q);
				XOR_ACC(RAIDZ_COL(c[j], i), GEN_PQR_R);
			}
		}
		STORE(p + i, GEN_PQR_P);
		STORE(q + i, GEN_PQR_Q);
		STORE(r + i, GEN_PQR_R);
	}
}

/*
 * Generate parity over the data columns of a row, a group at a time
 *
 * @rr		RAIDZ row
 * @func	raidz_gen_[p|pq|pqr]_cols()
 */
static raidz_inline void
raidz_generate_cols_impl(raidz_row_t * const rr,
    void (*func)(void **, const unsigned, const size_t, void *))
{
	const size_t firstdc = rr->rr_firstdatacol;
	const size_t ncols = rr->rr_cols;
	const size_t csize = rr->rr_col[CODE_P].rc_size;
	abd_t *abds[ABD_RAIDZ_MAXCOLS];
	size_t sizes[ABD_RAIDZ_MAXCOLS];
	raidz_cols_arg_t arg = { 0 };
	size_t off, size, c, n, j;

	for (j = 0; j < firstdc; j++) {
		abds[j] = rr->rr_col[j].rc_abd;
		sizes[j] = csize;
	}

	raidz_math_begin();

	for (off = 0; off < csize; off += CHUNK) {
		size = MIN(CHUNK, csize - off);
		for (c = firstdc; c < ncols; c += n) {
			n = MIN(ncols - c, RAIDZ_GRP_DCOLS);
			for (j = 0; j < n; j++) {
				abds[firstdc + j] = rr->rr_col[c + j].rc_abd;
				sizes[firstdc + j] = rr->rr_col[c + j].rc_size;
			}
			arg.rca_first = (c == firstdc);
			abd_raidz_cols_iterate(abds, sizes, firstdc + n, off,
			    size, func, &arg);
		}
	}

	raidz_math_end();
}

/*
 * Generate P parity (RAIDZ1)
 *
 * @rr	RAIDZ row
 */
static raidz_inline void
raidz_generate_p_impl(raidz_row_t * const rr)
{
	raidz_generate_cols_impl(rr, raidz_gen_p_cols);
}

/*
 * Generate PQ parity (RAIDZ2)
 *
 * @rr	RAIDZ row
 */
static raidz_inline void
raidz_generate_pq_impl(raidz_row_t * const rr)
{
	raidz_generate_cols_impl(rr, raidz_gen_pq_cols);
}

/*
 * Generate PQR parity (RAIDZ3)
 *
 * @rr	RAIDZ row
 */
static raidz_inline void
raidz_generate_pqr_impl(raidz_row_t * const rr)
{
	raidz_generate_cols_impl(rr, raidz_gen_pqr_cols);
}


/*
 * DATA RECONSTRUCTION
 *
 * Data reconstruction process consists of two phases:
 * 	- Syndrome calculation
 * 	- Data reconstruction
 *
 * Syndrome is calculated by generating parity using available data columns
 * and zeros in places of erasure. Existing parity is added to corresponding
 * syndrome value to obtain the [P|Q|R]syn values from equation:
 * 	P = Psyn + Dx + Dy + Dz
 * 	Q = Qsyn + 2^x * Dx + 2^y * Dy + 2^z * Dz
 * 	R = Rsyn + 4^x * Dx + 4^y * Dy + 4^z * Dz
 *
 * For data reconstruction phase, the corresponding equations are solved
 * for missing data (Dx, Dy, Dz). This generally involves multiplying known
 * symbols by an coefficient and adding them together. The multiplication
 * constant coefficients are calculated ahead of the operation in
 * raidz_rec_[q|r|pq|pq|qr|pqr]_coeff() functions.
 *
 * Both phases run in one pass over each stride, as for parity generation:
 * the syndromes stay in registers while the stride is read from the data
 * columns of a group, and the last group, which also reads the parity
 * columns, solves the equations before storing the stride of each target.
 * Earlier groups store the syndromes in the targets.
 *
 * IMPLEMENTATION NOTE: RAID-Z block can have complex geometry, with "big"
 * and "short" columns.  A data column that is shorter than the first target
 * adds 0 to the syndromes past its end.  A target that is shorter than the
 * first one is replaced by a buffer of the first target's size, copied back
 * at the end.
 *
 *    parity          data
 *    columns         columns
 * <----------> <------------------>
 *                   x       y  <----+ missing columns (x, y)
 *                   |       |
 * +---+---+---+---+-v-+---+-v-+---+   ^ 0
 * |   |   |   |   |   |   |   |   |   |
 * |   |   |   |   |   |   |   |   |   |
 * | P | Q | R | D | D | D | D | D |   |
 * |   |   |   | 0 | 1 | 2 | 3 | 4 |   |
 * |   |   |   |   |   |   |   |   |   v
 * |   |   |   |   |   +---+---+---+   ^ short_size
 * |   |   |   |   |   |               |
 * +---+---+---+---+---+               v big_size
 * <------------------> <---------->
 *      big columns     short columns
 *
 */

/*
 * Reconstruct data over a group of data columns
 *
 * Each function below takes the targets, the data columns of the group
 * (NULL for targets and for columns that have ended) and, in the last
 * group, the parity columns it uses, all of @len bytes.
 *
 * rec_p is gen_p over the data columns and P.
 */
static void
raidz_rec_q_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 1 : ncols;
	const unsigned js = raidz_cols_start(c, 1, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_Q_DEFINE();

	for (i = 0; i < n; i += REC_Q_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_Q_X);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_Q_X);
			j = js + 1;
		} else {
			ZERO(REC_Q_X);
		}
		for (; j < dend; j++) {
			MUL2(REC_Q_X);
			if (c[j] != NULL)
				XOR_ACC(RAIDZ_COL(c[j], i), REC_Q_X);
		}
		if (arg->rca_last) {
			XOR_ACC(RAIDZ_COL(c[dend], i), REC_Q_X);
			MUL(mul[MUL_Q_X], REC_Q_X);
		}
		STORE(x + i, REC_Q_X);
	}
}

static void
raidz_rec_r_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 1 : ncols;
	const unsigned js = raidz_cols_start(c, 1, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_R_DEFINE();

	for (i = 0; i < n; i += REC_R_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_R_X);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_R_X);
			j = js + 1;
		} else {
			ZERO(REC_R_X);
		}
		for (; j < dend; j++) {
			MUL4(REC_R_X);
			if (c[j] != NULL)
				XOR_ACC(RAIDZ_COL(c[j], i), REC_R_X);
		}
		if (arg->rca_last) {
			XOR_ACC(RAIDZ_COL(c[dend], i), REC_R_X);
			MUL(mul[MUL_R_X], REC_R_X);
		}
		STORE(x + i, REC_R_X);
	}
}

static void
raidz_rec_pq_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 2 : ncols;
	const unsigned js = raidz_cols_start(c, 2, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	v_t *y = (v_t *)c[TARGET_Y];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_PQ_DEFINE();

	for (i = 0; i < n; i += REC_PQ_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_PQ_X);
			LOAD(y + i, REC_PQ_Y);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_PQ_X);
			LOAD(RAIDZ_COL(c[js], i), REC_PQ_Y);
			j = js + 1;
		} else {
			ZERO(REC_PQ_X);
			ZERO(REC_PQ_Y);
		}
		for (; j < dend; j++) {
			MUL2(REC_PQ_Y);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PQ_X);
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PQ_Y);
			}
		}
		if (!arg->rca_last) {
			STORE(x + i, REC_PQ_X);
			STORE(y + i, REC_PQ_Y);
			continue;
		}
		XOR_ACC(RAIDZ_COL(c[dend + CODE_P], i), REC_PQ_X);
		XOR_ACC(RAIDZ_COL(c[dend + CODE_Q], i), REC_PQ_Y);

		/* Save Pxy */
		COPY(REC_PQ_X,  REC_PQ_T);

		/* Calc X */
		MUL(mul[MUL_PQ_X], REC_PQ_X);
		MUL(mul[MUL_PQ_Y], REC_PQ_Y);
		XOR(REC_PQ_Y,  REC_PQ_X);
		STORE(x + i, REC_PQ_X);

		/* Calc Y */
		XOR(REC_PQ_T,  REC_PQ_X);
		STORE(y + i, REC_PQ_X);
	}
}

static void
raidz_rec_pr_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 2 : ncols;
	const unsigned js = raidz_cols_start(c, 2, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	v_t *y = (v_t *)c[TARGET_Y];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_PR_DEFINE();

	for (i = 0; i < n; i += REC_PR_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_PR_X);
			LOAD(y + i, REC_PR_Y);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_PR_X);
			LOAD(RAIDZ_COL(c[js], i), REC_PR_Y);
			j = js + 1;
		} else {
			ZERO(REC_PR_X);
			ZERO(REC_PR_Y);
		}
		for (; j < dend; j++) {
			MUL4(REC_PR_Y);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PR_X);
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PR_Y);
			}
		}
		if (!arg->rca_last) {
			STORE(x + i, REC_PR_X);
			STORE(y + i, REC_PR_Y);
			continue;
		}
		XOR_ACC(RAIDZ_COL(c[dend], i), REC_PR_X);
		XOR_ACC(RAIDZ_COL(c[dend + 1], i), REC_PR_Y);

		/* Save Pxy */
		COPY(REC_PR_X,  REC_PR_T);

		/* Calc X */
		MUL(mul[MUL_PR_X], REC_PR_X);
		MUL(mul[MUL_PR_Y], REC_PR_Y);
		XOR(REC_PR_Y,  REC_PR_X);
		STORE(x + i, REC_PR_X);

		/* Calc Y */
		XOR(REC_PR_T,  REC_PR_X);
		STORE(y + i, REC_PR_X);
	}
}

static void
raidz_rec_qr_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 2 : ncols;
	const unsigned js = raidz_cols_start(c, 2, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	v_t *y = (v_t *)c[TARGET_Y];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_QR_DEFINE();

	for (i = 0; i < n; i += REC_QR_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_QR_X);
			LOAD(y + i, REC_QR_Y);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_QR_X);
			LOAD(RAIDZ_COL(c[js], i), REC_QR_Y);
			j = js + 1;
		} else {
			ZERO(REC_QR_X);
			ZERO(REC_QR_Y);
		}
		for (; j < dend; j++) {
			MUL2(REC_QR_X);
			MUL4(REC_QR_Y);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), REC_QR_X);
				XOR_ACC(RAIDZ_COL(c[j], i), REC_QR_Y);
			}
		}
		if (!arg->rca_last) {
			STORE(x + i, REC_QR_X);
			STORE(y + i, REC_QR_Y);
			continue;
		}
		XOR_ACC(RAIDZ_COL(c[dend], i), REC_QR_X);
		XOR_ACC(RAIDZ_COL(c[dend + 1], i), REC_QR_Y);

		/* Save Pxy */
		COPY(REC_QR_X,  REC_QR_T);

		/* Calc X */
		MUL(mul[MUL_QR_XQ], REC_QR_X);	/* X = Q * xqm */
		XOR(REC_QR_Y, REC_QR_X);	/* X = R ^ X   */
		MUL(mul[MUL_QR_X], REC_QR_X);	/* X = X * xm  */
		STORE(x + i, REC_QR_X);

		/* Calc Y */
		MUL(mul[MUL_QR_YQ], REC_QR_T);	/* X = Q * xqm */
		XOR(REC_QR_Y, REC_QR_T);	/* X = R ^ X   */
		MUL(mul[MUL_QR_Y], REC_QR_T);	/* X = X * xm  */
		STORE(y + i, REC_QR_T);
	}
}

static void
raidz_rec_pqr_cols(void **c, const unsigned ncols, const size_t len,
    void *private)
{
	const raidz_cols_arg_t *arg = private;
	const unsigned * const mul = arg->rca_mul;
	const unsigned dend = arg->rca_last ? ncols - 3 : ncols;
	const unsigned js = raidz_cols_start(c, 3, dend,
	    arg->rca_first);
	v_t *x = (v_t *)c[TARGET_X];
	v_t *y = (v_t *)c[TARGET_Y];
	v_t *z = (v_t *)c[TARGET_Z];
	const size_t n = len / sizeof (v_t);
	size_t i;
	unsigned j;

	REC_PQR_DEFINE();

	for (i = 0; i < n; i += REC_PQR_STRIDE) {
		MUL2_SETUP();
		j = js;
		if (!arg->rca_first) {
			LOAD(x + i, REC_PQR_X);
			LOAD(y + i, REC_PQR_Y);
			LOAD(z + i, REC_PQR_Z);
		} else if (js < dend) {
			LOAD(RAIDZ_COL(c[js], i), REC_PQR_X);
			LOAD(RAIDZ_COL(c[js], i), REC_PQR_Y);
			LOAD(RAIDZ_COL(c[js], i), REC_PQR_Z);
			j = js + 1;
		} else {
			ZERO(REC_PQR_X);
			ZERO(REC_PQR_Y);
			ZERO(REC_PQR_Z);
		}
		for (; j < dend; j++) {
			MUL2(REC_PQR_Y);
			MUL4(REC_PQR_Z);
			if (c[j] != NULL) {
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PQR_X);
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PQR_Y);
				XOR_ACC(RAIDZ_COL(c[j], i), REC_PQR_Z);
			}
		}
		if (!arg->rca_last) {
			STORE(x + i, REC_PQR_X);
			STORE(y + i, REC_PQR_Y);
			STORE(z + i, REC_PQR_Z);
			continue;
		}
		XOR_ACC(RAIDZ_COL(c[dend + CODE_P], i), REC_PQR_X);
		XOR_ACC(RAIDZ_COL(c[dend + CODE_Q], i), REC_PQR_Y);
		XOR_ACC(RAIDZ_COL(c[dend + CODE_R], i), REC_PQR_Z);

		/* Save Pxyz and Qxyz */
		COPY(REC_PQR_X, REC_PQR_XS);
		COPY(REC_PQR_Y, REC_PQR_YS);

		/* Calc X */
		MUL(mul[MUL_PQR_XP], REC_PQR_X);	/* Xp = Pxyz * xp   */
		MUL(mul[MUL_PQR_XQ], REC_PQR_Y);	/* Xq = Qxyz * xq   */
		XOR(REC_PQR_Y, REC_PQR_X);
		MUL(mul[MUL_PQR_XR], REC_PQR_Z);	/* Xr = Rxyz * xr   */
		XOR(REC_PQR_Z, REC_PQR_X);		/* X = Xp + Xq + Xr */
		STORE(x + i, REC_PQR_X);

		/* Calc Y */
		XOR(REC_PQR_X, REC_PQR_XS); 		/* Pyz = Pxyz + X */
		MUL(mul[MUL_PQR_YU], REC_PQR_X);  	/* Xq = X * upd_q */
		XOR(REC_PQR_X, REC_PQR_YS); 		/* Qyz = Qxyz + Xq */
		COPY(REC_PQR_XS, REC_PQR_X);		/* restore Pyz */
		MUL(mul[MUL_PQR_YP], REC_PQR_X);	/* Yp = Pyz * yp */
		MUL(mul[MUL_PQR_YQ], REC_PQR_YS);	/* Yq = Qyz * yq */
		XOR(REC_PQR_X, REC_PQR_YS); 		/* Y = Yp + Yq */
		STORE(y + i, REC_PQR_YS);

		/* Calc Z */
		XOR(REC_PQR_XS, REC_PQR_YS);		/* Z = Pz = Pyz + Y */
		STORE(z + i, REC_PQR_YS);
	}
}

/*
 * Reconstruct the targets of a row, a group of data columns at a time
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 * @ntgt	number of targets
 * @pidx	parity columns used, in order
 * @func	raidz_rec_*_cols() or raidz_gen_p_cols()
 * @coeff	multiplication coefficients
 */
static raidz_inline void
raidz_reconstruct_cols_impl(raidz_row_t *rr, const int *tgtidx,
    const unsigned ntgt, const unsigned *pidx,
    void (*func)(void **, const unsigned, const size_t, void *),
    const unsigned *coeff)
{
	const size_t firstdc = rr->rr_firstdatacol;
	const size_t ncols = rr->rr_cols;
	const size_t xsize = rr->rr_col[tgtidx[TARGET_X]].rc_size;
	abd_t *tabds[PARITY_PQR];
	abd_t *abds[ABD_RAIDZ_MAXCOLS];
	size_t sizes[ABD_RAIDZ_MAXCOLS];
	raidz_cols_arg_t arg = { .rca_mul = coeff };
	size_t off, size, c, n, j, k;
	unsigned t;

	/*
	 * Check if some of targets is shorter then others
	 * In this case, shorter target needs to be replaced with
	 * new buffer so that syndrome can be calculated.
	 */
	for (j = 0; j < ntgt; j++) {
		raidz_col_t *rc = &rr->rr_col[tgtidx[j]];

		tabds[j] = (rc->rc_size < xsize) ?
		    abd_alloc(xsize, B_FALSE) : rc->rc_abd;
		abds[j] = tabds[j];
		sizes[j] = xsize;
	}

	raidz_math_begin();

	for (off = 0; off < xsize; off += CHUNK) {
		size = MIN(CHUNK, xsize - off);
		for (c = firstdc; c < ncols; c += n) {
			n = MIN(ncols - c, RAIDZ_GRP_DCOLS);
			k = ntgt;
			for (j = c; j < c + n; j++, k++) {
				abds[k] = rr->rr_col[j].rc_abd;
				sizes[k] = rr->rr_col[j].rc_size;
				for (t = 0; t < ntgt; t++) {
					if (j == (size_t)tgtidx[t])
						abds[k] = NULL;
				}
			}
			arg.rca_first = (c == firstdc);
			arg.rca_last = (c + n == ncols);
			for (j = 0; arg.rca_last && j < ntgt; j++) {
				abds[k] = rr->rr_col[pidx[j]].rc_abd;
				sizes[k] = xsize;
				k++;
			}
			abd_raidz_cols_iterate(abds, sizes, k, off, size,
			    func, &arg);
		}
	}

	/*
	 * Copy shorter targets back to the original abd buffer
	 */
	for (j = 1; j < ntgt; j++) {
		raidz_col_t *rc = &rr->rr_col[tgtidx[j]];

		if (tabds[j] != rc->rc_abd)
			raidz_copy(rc->rc_abd, tabds[j], 0, rc->rc_size);
	}

	raidz_math_end();

	for (j = 1; j < ntgt; j++) {
		if (tabds[j] != rr->rr_col[tgtidx[j]].rc_abd)
			abd_free(tabds[j]);
	}
}

/*
 * Reconstruct single data column using P parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_p_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_P };

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return (1 << CODE_P);

	raidz_reconstruct_cols_impl(rr, tgtidx, 1, pidx, raidz_gen_p_cols,
	    NULL);

	return (1 << CODE_P);
}

/*
 * Reconstruct single data column using Q parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_q_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_Q };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return (1 << CODE_Q);

	raidz_rec_q_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 1, pidx, raidz_rec_q_cols,
	    coeff);

	return (1 << CODE_Q);
}

/*
 * Reconstruct single data column using R parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_r_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_R };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return (1 << CODE_R);

	raidz_rec_r_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 1, pidx, raidz_rec_r_cols,
	    coeff);

	return (1 << CODE_R);
}

/*
 * Reconstruct two data columns using PQ parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_pq_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_P, CODE_Q };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return ((1 << CODE_P) | (1 << CODE_Q));

	raidz_rec_pq_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 2, pidx, raidz_rec_pq_cols,
	    coeff);

	return ((1 << CODE_P) | (1 << CODE_Q));
}

/*
 * Reconstruct two data columns using PR parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_pr_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_P, CODE_R };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return ((1 << CODE_P) | (1 << CODE_R));

	raidz_rec_pr_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 2, pidx, raidz_rec_pr_cols,
	    coeff);

	return ((1 << CODE_P) | (1 << CODE_R));
}

/*
 * Reconstruct two data columns using QR parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_qr_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_Q, CODE_R };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return ((1 << CODE_Q) | (1 << CODE_R));

	raidz_rec_qr_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 2, pidx, raidz_rec_qr_cols,
	    coeff);

	return ((1 << CODE_Q) | (1 << CODE_R));
}

/*
 * Reconstruct three data columns using PQR parity
 *
 * @rr		RAIDZ row
 * @tgtidx	array of missing data indexes
 */
static raidz_inline int
raidz_reconstruct_pqr_impl(raidz_row_t *rr, const int *tgtidx)
{
	static const unsigned pidx[] = { CODE_P, CODE_Q, CODE_R };
	unsigned coeff[MUL_CNT];

	if (rr->rr_col[tgtidx[TARGET_X]].rc_abd == NULL)
		return ((1 << CODE_P) | (1 << CODE_Q) | (1 << CODE_R));

	raidz_rec_pqr_coeff(rr, tgtidx, coeff);
	raidz_reconstruct_cols_impl(rr, tgtidx, 3, pidx, raidz_rec_pqr_cols,
	    coeff);

	return ((1 << CODE_P) | (1 << CODE_Q) | (1 << CODE_R));
}

#endif /* _VDEV_RAIDZ_MATH_IMPL_H */
