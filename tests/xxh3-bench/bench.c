// SPDX-License-Identifier: CDDL-1.0
/*
 * Bench only, not for upstream (issue #16503).
 *
 * Times the ZFS block checksums through their zio_checksum_t entry points,
 * as the chksum_bench kstat does (module/zfs/zfs_chksum.c), against XXH3-64
 * and XXH3-128 wired the same way (xxh3_abd.c): same ABDs, same calls,
 * same timing loop.  Every implementation each algorithm offers on this
 * CPU is timed, over linear and scattered ABDs (userland scatter ABDs are
 * built from 4 KiB chunks, so abd_iterate_func() hands the hash one chunk
 * at a time, as in the kernel), at sizes from 4 KiB to 16 MiB.  The whole
 * set is repeated for a number of rounds, and the order in which the
 * implementations run is rotated each round, so that the rounds interleave.
 *
 * Output is one CSV line per measurement:
 *	R,<round>,<algorithm>,<implementation>,<linear|scatter>,<size>,<GB/s>
 * with GB/s = 10^9 bytes per second.
 *
 *	bench [-r rounds] [-t ms] [-s size,...] [-a alg,...]
 *	bench -c		self-check of the XXH3 wiring
 *	bench -w <file>		write the check buffer and print its XXH3
 *				hashes, to compare with xxhsum
 */

#include <sys/zfs_context.h>
#include <sys/abd.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_impl.h>
#include <zfs_fletcher.h>
#include <libzpool.h>
#include <getopt.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#endif

#define	DECL_VARIANT(v)							\
	extern zio_checksum_t abd_checksum_xxh3_64_##v;			\
	extern zio_checksum_t abd_checksum_xxh3_128_##v;		\
	extern void xxh3_oneshot_##v(const void *, size_t, uint64_t[3]); \
	extern const char *xxh3_vector_##v(void);

typedef struct {
	const char *name;
	zio_checksum_t *f64;
	zio_checksum_t *f128;
	void (*oneshot)(const void *, size_t, uint64_t[3]);
	const char *(*vector)(void);
	boolean_t (*supported)(void);
} xxh3_variant_t;

static boolean_t
always(void)
{
	return (B_TRUE);
}

#define	VARIANT(v, sup)	{ #v, abd_checksum_xxh3_64_##v,			\
	abd_checksum_xxh3_128_##v, xxh3_oneshot_##v, xxh3_vector_##v, sup }

#if defined(__x86_64__)
DECL_VARIANT(scalar)
DECL_VARIANT(sse2)
DECL_VARIANT(avx2)
DECL_VARIANT(avx512)
static boolean_t
has_avx2(void)
{
	return (__builtin_cpu_supports("avx2"));
}
static boolean_t
has_avx512(void)
{
	return (__builtin_cpu_supports("avx512f"));
}
static const xxh3_variant_t variants[] = {
	VARIANT(scalar, always),
	VARIANT(sse2, always),
	VARIANT(avx2, has_avx2),
	VARIANT(avx512, has_avx512),
};
#elif defined(__aarch64__)
DECL_VARIANT(scalar)
DECL_VARIANT(neon)
DECL_VARIANT(sve)
static boolean_t
has_sve(void)
{
	return ((getauxval(AT_HWCAP) & (1UL << 22)) != 0);	/* HWCAP_SVE */
}
static const xxh3_variant_t variants[] = {
	VARIANT(scalar, always),
	VARIANT(neon, always),
	VARIANT(sve, has_sve),
};
#else
#error "x86_64 and aarch64 only"
#endif

#define	NVARIANTS	(sizeof (variants) / sizeof (variants[0]))

/* One timed implementation of one algorithm. */
typedef struct {
	const char *alg;
	char impl[32];
	zio_checksum_t *func;
	zio_checksum_tmpl_init_t *init;
	zio_checksum_tmpl_free_t *free;
	const char *fletcher;		/* fletcher_4_impl_set() name */
	const zfs_impl_t *ops;		/* or zfs_impl id */
	uint32_t id;
	void *ctx;
} entry_t;

#define	MAXENTRIES	64
static entry_t entries[MAXENTRIES];
static int nentries;
static zio_cksum_salt_t salt;

static const char *alg_filter;

static boolean_t
wanted(const char *alg)
{
	char buf[256], *tok, *p;

	if (alg_filter == NULL)
		return (B_TRUE);
	(void) strlcpy(buf, alg_filter, sizeof (buf));
	for (tok = strtok_r(buf, ",", &p); tok != NULL;
	    tok = strtok_r(NULL, ",", &p)) {
		if (strcmp(tok, alg) == 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static entry_t *
add(const char *alg, const char *impl, zio_checksum_t *func,
    zio_checksum_tmpl_init_t *init, zio_checksum_tmpl_free_t *free)
{
	entry_t *e;

	VERIFY3S(nentries, <, MAXENTRIES);
	e = &entries[nentries++];
	memset(e, 0, sizeof (*e));
	e->alg = alg;
	(void) strlcpy(e->impl, impl, sizeof (e->impl));
	e->func = func;
	e->init = init;
	e->free = free;
	if (init != NULL)
		e->ctx = init(&salt);
	return (e);
}

static void
add_impls(const char *alg, const char *implname, zio_checksum_t *func,
    zio_checksum_tmpl_init_t *init, zio_checksum_tmpl_free_t *free)
{
	const zfs_impl_t *ops = zfs_impl_get_ops(implname);
	uint32_t save = ops->getid();

	for (uint32_t id = 0; id < ops->getcnt(); id++) {
		ops->setid(id);
		entry_t *e = add(alg, ops->getname(), func, init, free);
		e->ops = ops;
		e->id = id;
	}
	ops->setid(save);
}

static void
setup(void)
{
	static const char *const fl[] = {
		"scalar", "superscalar", "superscalar4", "sse2", "ssse3",
		"avx2", "avx512f", "avx512bw", "aarch64_neon",
	};

	if (wanted("fletcher4")) {
		for (int i = 0; i < ARRAY_SIZE(fl); i++) {
			if (fletcher_4_impl_set(fl[i]) != 0)
				continue;
			entry_t *e = add("fletcher4", fl[i],
			    abd_fletcher_4_native, NULL, NULL);
			e->fletcher = fl[i];
		}
	}
	if (wanted("sha256"))
		add_impls("sha256", "sha256", abd_checksum_sha256, NULL, NULL);
	if (wanted("blake3"))
		add_impls("blake3", "blake3", abd_checksum_blake3_native,
		    abd_checksum_blake3_tmpl_init,
		    abd_checksum_blake3_tmpl_free);
	if (wanted("edonr"))
		(void) add("edonr", "generic", abd_checksum_edonr_native,
		    abd_checksum_edonr_tmpl_init,
		    abd_checksum_edonr_tmpl_free);
	if (wanted("skein"))
		(void) add("skein", "generic", abd_checksum_skein_native,
		    abd_checksum_skein_tmpl_init,
		    abd_checksum_skein_tmpl_free);
	for (int i = 0; i < NVARIANTS; i++) {
		const xxh3_variant_t *v = &variants[i];

		if (!v->supported())
			continue;
		/* The -m flags must really have selected this path. */
		VERIFY0(strcmp(v->name, v->vector()));
		if (wanted("xxh3-64"))
			(void) add("xxh3-64", v->name, v->f64, NULL, NULL);
		if (wanted("xxh3-128"))
			(void) add("xxh3-128", v->name, v->f128, NULL, NULL);
	}
}

static void
select_impl(const entry_t *e)
{
	if (e->fletcher != NULL)
		VERIFY0(fletcher_4_impl_set(e->fletcher));
	else if (e->ops != NULL)
		e->ops->setid(e->id);
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static void
fill(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		rng ^= rng << 13;
		rng ^= rng >> 7;
		rng ^= rng << 17;
		p[i] = (uint8_t)rng;
	}
}

static abd_t *
make_abd(const uint8_t *buf, size_t size, boolean_t linear)
{
	abd_t *abd = linear ? abd_alloc_linear(size, B_FALSE) :
	    abd_alloc(size, B_FALSE);

	/* abd_alloc() returns a linear ABD below 1536 bytes. */
	if (size >= 4096)
		VERIFY3U(abd_is_linear(abd), ==, linear);
	abd_copy_from_buf(abd, buf, size);
	return (abd);
}

/* Bytes per nanosecond, i.e. GB/s, over at least min_ns. */
static double
time_one(const entry_t *e, abd_t *abd, uint64_t size, hrtime_t min_ns)
{
	zio_cksum_t zc;
	uint64_t batch = size >= (1 << 20) ? 1 : (1 << 20) / size;
	uint64_t n = 0;
	hrtime_t start, elapsed;

	e->func(abd, size, e->ctx, &zc);	/* warm up */
	start = gethrtime();
	do {
		for (uint64_t i = 0; i < batch; i++)
			e->func(abd, size, e->ctx, &zc);
		n += batch;
		elapsed = gethrtime() - start;
	} while (elapsed < min_ns);
	return ((double)size * n / elapsed);
}

/*
 * Self-check: through every variant, the XXH3 of a buffer computed over
 * linear and scattered ABDs, one chunk at a time, must equal the one-shot
 * hash of the flat buffer by the scalar code.
 */
static int
self_check(void)
{
	static const size_t sizes[] = {
		512, 1536, 4096, 4608, 12288, 65536, 131072 + 512,
		1 << 20, (1 << 20) + 3584, 1 << 24,
	};
	uint8_t *buf = malloc(1 << 24);
	int checks = 0, bad = 0;

	fill(buf, 1 << 24);
	for (int s = 0; s < ARRAY_SIZE(sizes); s++) {
		size_t size = sizes[s];
		uint64_t ref[3];

		variants[0].oneshot(buf, size, ref);
		for (int l = 0; l <= 1; l++) {
			abd_t *abd = make_abd(buf, size, l);
			for (int i = 0; i < NVARIANTS; i++) {
				const xxh3_variant_t *v = &variants[i];
				uint64_t one[3];
				zio_cksum_t z64, z128;

				if (!v->supported())
					continue;
				v->oneshot(buf, size, one);
				v->f64(abd, size, NULL, &z64);
				v->f128(abd, size, NULL, &z128);
				checks += 4;
				if (one[0] != ref[0] || one[1] != ref[1] ||
				    one[2] != ref[2]) {
					printf("MISMATCH one-shot %s size %zu\n",
					    v->name, size);
					bad++;
				}
				if (z64.zc_word[0] != ref[0] ||
				    z64.zc_word[1] != 0) {
					printf("MISMATCH xxh3-64 %s %s %zu\n",
					    v->name, l ? "linear" : "scatter",
					    size);
					bad++;
				}
				if (z128.zc_word[0] != ref[1] ||
				    z128.zc_word[1] != ref[2]) {
					printf("MISMATCH xxh3-128 %s %s %zu\n",
					    v->name, l ? "linear" : "scatter",
					    size);
					bad++;
				}
			}
			abd_free(abd);
		}
	}
	printf("xxh3 self-check: %d comparisons, %d mismatches\n", checks, bad);
	free(buf);
	return (bad != 0);
}

/* Write the check buffer and print its hashes as xxhsum does. */
static int
write_check(const char *path)
{
	size_t size = (1 << 20) + 3584;
	uint8_t *buf = malloc(size);
	FILE *f;
	abd_t *abd;

	fill(buf, size);
	f = fopen(path, "w");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fwrite(buf, 1, size, f), ==, size);
	VERIFY0(fclose(f));
	abd = make_abd(buf, size, B_FALSE);
	for (int i = 0; i < NVARIANTS; i++) {
		zio_cksum_t z64, z128;

		if (!variants[i].supported())
			continue;
		variants[i].f64(abd, size, NULL, &z64);
		variants[i].f128(abd, size, NULL, &z128);
		printf("%s xxh3-64 %016llx xxh3-128 %016llx%016llx\n",
		    variants[i].name, (u_longlong_t)z64.zc_word[0],
		    (u_longlong_t)z128.zc_word[1],
		    (u_longlong_t)z128.zc_word[0]);
	}
	abd_free(abd);
	free(buf);
	return (0);
}

int
main(int argc, char **argv)
{
	int rounds = 5, c, rv = 0;
	hrtime_t min_ns = MSEC2NSEC(50);
	uint64_t sizes[16];
	int nsizes = 0;
	char *sizearg = NULL, *tok, *p;
	boolean_t check = B_FALSE;
	const char *wpath = NULL;

	while ((c = getopt(argc, argv, "a:cr:s:t:w:")) != -1) {
		switch (c) {
		case 'a':
			alg_filter = optarg;
			break;
		case 'c':
			check = B_TRUE;
			break;
		case 'r':
			rounds = atoi(optarg);
			break;
		case 's':
			sizearg = optarg;
			break;
		case 't':
			min_ns = MSEC2NSEC(atoi(optarg));
			break;
		case 'w':
			wpath = optarg;
			break;
		default:
			return (2);
		}
	}
	if (sizearg == NULL)
		sizearg = strdup("4096,16384,65536,131072,1048576,4194304,"
		    "16777216");
	for (tok = strtok_r(sizearg, ",", &p); tok != NULL && nsizes < 16;
	    tok = strtok_r(NULL, ",", &p))
		sizes[nsizes++] = strtoull(tok, NULL, 0);

	kernel_init(SPA_MODE_READ);

	if (check || wpath != NULL) {
		if (check)
			rv |= self_check();
		if (wpath != NULL)
			rv |= write_check(wpath);
		kernel_fini();
		return (rv);
	}

	setup();
	for (int i = 0; i < nentries; i++)
		printf("# %s %s\n", entries[i].alg, entries[i].impl);

	uint64_t maxsize = 0;
	for (int s = 0; s < nsizes; s++)
		maxsize = MAX(maxsize, sizes[s]);
	uint8_t *buf = malloc(maxsize);
	fill(buf, maxsize);

	abd_t *abds[16][2];
	for (int s = 0; s < nsizes; s++)
		for (int l = 0; l <= 1; l++)
			abds[s][l] = make_abd(buf, sizes[s], l);

	for (int r = 0; r < rounds; r++) {
		for (int s = 0; s < nsizes; s++) {
			for (int l = 1; l >= 0; l--) {
				for (int k = 0; k < nentries; k++) {
					entry_t *e =
					    &entries[(k + r) % nentries];
					select_impl(e);
					double gbs = time_one(e, abds[s][l],
					    sizes[s], min_ns);
					printf("R,%d,%s,%s,%s,%llu,%.3f\n", r,
					    e->alg, e->impl,
					    l ? "linear" : "scatter",
					    (u_longlong_t)sizes[s], gbs);
					fflush(stdout);
				}
			}
		}
	}

	for (int s = 0; s < nsizes; s++)
		for (int l = 0; l <= 1; l++)
			abd_free(abds[s][l]);
	for (int i = 0; i < nentries; i++)
		if (entries[i].free != NULL)
			entries[i].free(entries[i].ctx);
	free(buf);
	kernel_fini();
	return (0);
}
