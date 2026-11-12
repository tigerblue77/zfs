// SPDX-License-Identifier: CDDL-1.0
/*
 * Bench only, never for upstream.
 *
 * Times every fletcher4 implementation this CPU supports, native and
 * byteswap, through the zio_checksum_t entry points zio uses
 * (abd_fletcher_4_native/byteswap), over linear and scattered ABDs
 * (userland scatter ABDs are 4 KiB chunks), at several sizes.  Each
 * invocation runs one or more rounds; the order of the implementations
 * is rotated by round, starting at -o, so that rounds interleave.
 *
 *	R,<label>,<round>,<impl>,<order>,<layout>,<size>,<GB/s>
 *
 * with order native|byteswap, layout linear|scatter, and GB/s = 10^9
 * bytes per second.
 *
 * -S n repeats n times the selection the kernel makes at module load in
 * fletcher_4_benchmark() (userland skips it and takes the last supported
 * implementation): byteswap pass then native pass, 128 KiB buffer, each
 * implementation timed for at least 1 ms in batches of 32 calls through
 * fletcher_4_native()/fletcher_4_byteswap(), fastest B/s wins.
 *
 *	S,<label>,<iter>,<impl>,<native|byteswap>,<B/s>
 *	W,<label>,<iter>,<native winner>,<byteswap winner>
 *
 *	fl4bench [-L label] [-r rounds] [-o first round] [-t ms] [-s size,...]
 *	fl4bench [-L label] -S n
 */

#include <sys/zfs_context.h>
#include <sys/abd.h>
#include <sys/zio_checksum.h>
#include <zfs_fletcher.h>
#include <libzpool.h>
#include <getopt.h>

static const char *const all_impls[] = {
	"scalar", "superscalar", "superscalar4", "sse2", "ssse3",
	"avx2", "avx512f", "avx512bw", "aarch64_neon",
};

static const char *impls[ARRAY_SIZE(all_impls)];
static int nimpls;
static const char *label = "build";

static void
probe(void)
{
	for (int i = 0; i < ARRAY_SIZE(all_impls); i++) {
		if (fletcher_4_impl_set(all_impls[i]) == 0)
			impls[nimpls++] = all_impls[i];
	}
	VERIFY3S(nimpls, >, 0);
	for (int i = 0; i < nimpls; i++)
		printf("# %s supported %s\n", label, impls[i]);
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

	if (size >= 4096)
		VERIFY3U(abd_is_linear(abd), ==, linear);
	abd_copy_from_buf(abd, buf, size);
	return (abd);
}

/* Bytes per nanosecond, i.e. GB/s, over at least min_ns. */
static double
time_one(zio_checksum_t *func, abd_t *abd, uint64_t size, hrtime_t min_ns)
{
	zio_cksum_t zc;
	uint64_t batch = size >= (1 << 20) ? 1 : (1 << 20) / size;
	uint64_t n = 0;
	hrtime_t start, elapsed;

	func(abd, size, NULL, &zc);	/* warm up */
	start = gethrtime();
	do {
		for (uint64_t i = 0; i < batch; i++)
			func(abd, size, NULL, &zc);
		n += batch;
		elapsed = gethrtime() - start;
	} while (elapsed < min_ns);
	return ((double)size * n / elapsed);
}

/* The kernel's fletcher_4_benchmark_impl(), timed the same way. */
static int
select_pass(boolean_t native, const char *data, uint64_t size, int iter)
{
	uint64_t best = 0;
	int besti = 0;

	for (int i = 0; i < nimpls; i++) {
		uint64_t run_count = 0, bw;
		hrtime_t start, ns;
		zio_cksum_t zc;

		VERIFY0(fletcher_4_impl_set(impls[i]));
		start = gethrtime();
		do {
			for (int l = 0; l < 32; l++, run_count++) {
				if (native)
					fletcher_4_native(data, size, NULL,
					    &zc);
				else
					fletcher_4_byteswap(data, size, NULL,
					    &zc);
			}
			ns = gethrtime() - start;
		} while (ns < MSEC2NSEC(1));
		bw = size * run_count * NANOSEC / ns;
		printf("S,%s,%d,%s,%s,%llu\n", label, iter, impls[i],
		    native ? "native" : "byteswap", (u_longlong_t)bw);
		if (bw > best) {
			best = bw;
			besti = i;
		}
	}
	return (besti);
}

static void
selection(int n)
{
	const uint64_t size = 1 << SPA_OLD_MAXBLOCKSHIFT;	/* 128 KiB */
	char *data = malloc(size);

	for (uint64_t i = 0; i < size / sizeof (uint64_t); i++)
		((uint64_t *)data)[i] = (uintptr_t)(data + i);
	for (int it = 0; it < n; it++) {
		int bs = select_pass(B_FALSE, data, size, it);
		int nat = select_pass(B_TRUE, data, size, it);
		printf("W,%s,%d,%s,%s\n", label, it, impls[nat], impls[bs]);
		fflush(stdout);
	}
	free(data);
}

int
main(int argc, char **argv)
{
	int rounds = 1, first = 0, nsel = 0, c;
	hrtime_t min_ns = MSEC2NSEC(50);
	uint64_t sizes[16];
	int nsizes = 0;
	char *sizearg = NULL, *tok, *p;

	while ((c = getopt(argc, argv, "L:o:r:s:S:t:")) != -1) {
		switch (c) {
		case 'L':
			label = optarg;
			break;
		case 'o':
			first = atoi(optarg);
			break;
		case 'r':
			rounds = atoi(optarg);
			break;
		case 's':
			sizearg = optarg;
			break;
		case 'S':
			nsel = atoi(optarg);
			break;
		case 't':
			min_ns = MSEC2NSEC(atoi(optarg));
			break;
		default:
			return (2);
		}
	}
	if (sizearg == NULL)
		sizearg = strdup("4096,16384,131072,1048576,16777216");
	for (tok = strtok_r(sizearg, ",", &p); tok != NULL && nsizes < 16;
	    tok = strtok_r(NULL, ",", &p))
		sizes[nsizes++] = strtoull(tok, NULL, 0);

	kernel_init(SPA_MODE_READ);
	probe();

	if (nsel > 0) {
		selection(nsel);
		kernel_fini();
		return (0);
	}

	uint64_t maxsize = 0;
	for (int s = 0; s < nsizes; s++)
		maxsize = MAX(maxsize, sizes[s]);
	uint8_t *buf = malloc(maxsize);
	fill(buf, maxsize);

	abd_t *abds[16][2];
	for (int s = 0; s < nsizes; s++)
		for (int l = 0; l <= 1; l++)
			abds[s][l] = make_abd(buf, sizes[s], l);

	/* Every (implementation, byte order) pair, rotated by round. */
	int npairs = nimpls * 2;
	for (int r = first; r < first + rounds; r++) {
		for (int s = 0; s < nsizes; s++) {
			for (int l = 1; l >= 0; l--) {
				for (int k = 0; k < npairs; k++) {
					int e = (k + r) % npairs;
					const char *impl = impls[e / 2];
					boolean_t bswap = e % 2;
					VERIFY0(fletcher_4_impl_set(impl));
					double gbs = time_one(bswap ?
					    abd_fletcher_4_byteswap :
					    abd_fletcher_4_native,
					    abds[s][l], sizes[s], min_ns);
					printf("R,%s,%d,%s,%s,%s,%llu,%.3f\n",
					    label, r, impl,
					    bswap ? "byteswap" : "native",
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
	free(buf);
	kernel_fini();
	return (0);
}
