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

#ifndef	RAIDZ_TEST_H
#define	RAIDZ_TEST_H

#include <sys/spa.h>

static const char *const raidz_impl_names[] = {
	"original",
	"scalar",
	"sse2",
	"ssse3",
	"avx2",
	"avx512f",
	"avx512bw",
	"aarch64_neon",
	"aarch64_neonx2",
	"powerpc_altivec",
	NULL
};

enum raidz_verbosity {
	D_ALL,
	D_INFO,
	D_DEBUG,
};

typedef struct raidz_test_opts {
	size_t rto_ashift;
	uint64_t rto_offset;
	size_t rto_dcols;
	size_t rto_dsize;
	enum raidz_verbosity rto_v;
	size_t rto_sweep;
	size_t rto_sweep_timeout;
	size_t rto_benchmark;
	size_t rto_expand;
	uint64_t rto_expand_offset;
	size_t rto_sanity;
	size_t rto_gdb;
	size_t rto_ec;
	size_t rto_ec_parity;
	size_t rto_ec_width;
	size_t rto_ec_vand;
	uint64_t rto_ec_samples;
	unsigned rto_ec_seed;
	int rto_ec_kernel;
	const char *rto_ec_kernel_name;
	const char *rto_cr_mode;
	const char *rto_cr_widths;
	const char *rto_cr_prod;
	int rto_cr_cksum;
	int rto_cr_bits;
	uint64_t rto_cr_budget;

	/* non-user options */
	boolean_t rto_should_stop;

	zio_t *zio_golden;
	raidz_map_t *rm_golden;
} raidz_test_opts_t;

static const raidz_test_opts_t rto_opts_defaults = {
	.rto_ashift = 9,
	.rto_offset = 0,
	.rto_dcols = 8,
	.rto_dsize = 1<<19,
	.rto_v = D_ALL,
	.rto_sweep = 0,
	.rto_benchmark = 0,
	.rto_expand = 0,
	.rto_expand_offset = -1ULL,
	.rto_sanity = 0,
	.rto_gdb = 0,
	.rto_ec = 0,
	.rto_ec_parity = 0,
	.rto_ec_width = 32,
	.rto_ec_vand = 0,
	.rto_ec_samples = 2000,
	.rto_ec_seed = 1,
	.rto_ec_kernel = 1,	/* RZEC_KERNEL_TABLE */
	.rto_cr_bits = 256,
	.rto_should_stop = B_FALSE
};

extern raidz_test_opts_t rto_opts;

static inline size_t ilog2(size_t a)
{
	return (a > 1 ? 1 + ilog2(a >> 1) : 0);
}


#define	LOG(lvl, ...)				\
{						\
	if (rto_opts.rto_v >= lvl)		\
		(void) fprintf(stdout, __VA_ARGS__);	\
}						\

#define	LOG_OPT(lvl, opt, ...)			\
{						\
	if (opt->rto_v >= lvl)			\
		(void) fprintf(stdout, __VA_ARGS__);	\
}						\

#define	ERR(...)	(void) fprintf(stderr, __VA_ARGS__)


#define	DBLSEP "================\n"
#define	SEP    "----------------\n"


#define	raidz_alloc(size)	abd_alloc(size, B_FALSE)
#define	raidz_free(p, size)	abd_free(p)


void init_zio_abd(zio_t *zio);

void run_raidz_benchmark(void);

/* Generic k+m code prototype, see raidz_ec.c */
#define	RZEC_MAXPARITY	6

typedef enum rzec_code {
	RZEC_CAUCHY,
	RZEC_VAND,
} rzec_code_t;

/* The scalar kernels, then one per entry of rzec_simd_impls[]. */
typedef int rzec_kernel_t;
#define	RZEC_KERNEL_ALL		(-1)
#define	RZEC_KERNEL_ALLSIMD	(-2)
#define	RZEC_KERNEL_SWAR	0
#define	RZEC_KERNEL_TABLE	1
#define	RZEC_KERNEL_SIMD	2

void rzec_init(void);
void rzec_set_kernel(rzec_kernel_t);
int rzec_nkernels(void);
boolean_t rzec_kernel_available(rzec_kernel_t);
const char *rzec_kernel_name(rzec_kernel_t);
rzec_kernel_t rzec_kernel_lookup(const char *);
const char *rzec_code_name(rzec_code_t);
int rzec_max_data(rzec_code_t);
void rzec_generate(raidz_map_t *, rzec_code_t);
int rzec_reconstruct(raidz_map_t *, rzec_code_t, const int *, int);
int rzec_reconstruct_data(raidz_map_t *, rzec_code_t, const int *, int);
uint8_t rzec_coefficient(rzec_code_t, int, int, int);
int rzec_matrix_invert(uint8_t [RZEC_MAXPARITY][RZEC_MAXPARITY], int);
void rzec_lincomb(int, uint8_t *const *, const size_t *, int,
    const uint8_t *const *, const size_t *, const uint8_t *);
int run_rzec_check(void);
void run_rzec_benchmark(void);
int run_combrec(void);

#endif /* RAIDZ_TEST_H */
