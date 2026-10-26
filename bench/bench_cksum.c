#include <sys/zfs_context.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_impl.h>
#include <sys/abd.h>
#include <libzpool.h>
#include <time.h>

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

static void
run(enum zio_checksum c, const char *label)
{
	size_t sizes[] = { 4096, 16384, 131072 };
	zio_checksum_info_t *ci = &zio_checksum_table[c];
	zio_cksum_salt_t salt = { { 0 } };
	void *tmpl = ci->ci_tmpl_init ? ci->ci_tmpl_init(&salt) : NULL;
	for (int s = 0; s < 3; s++) {
		size_t len = sizes[s];
		void *buf = malloc(len);
		for (size_t i = 0; i < len; i++)
			((uint8_t *)buf)[i] = (uint8_t)(i * 2654435761u >> 13);
		abd_t *abd = abd_get_from_buf(buf, len);
		zio_cksum_t zc;
		int iters = (int)(200 * 1024 * 1024 / len);
		double t0 = now();
		for (int i = 0; i < iters; i++)
			ci->ci_func[0](abd, len, tmpl, &zc);
		double t = (now() - t0) / iters;
		printf("%-22s %6zu B: %.2f GB/s\n", label, len, len / t / 1e9);
		abd_free(abd);
		free(buf);
	}
	if (tmpl)
		ci->ci_tmpl_free(tmpl);
}

int
main(void)
{
	kernel_init(SPA_MODE_READ);
	const zfs_impl_t *b3 = zfs_impl_get_ops("blake3");
	const zfs_impl_t *s256 = zfs_impl_get_ops("sha256");
	const zfs_impl_t *s512 = zfs_impl_get_ops("sha512");
	VERIFY0(b3->setname("avx2"));
	VERIFY0(s256->setname("avx2"));
	VERIFY0(s512->setname("avx2"));
	printf("impls: blake3=%s sha256=%s sha512=%s\n", b3->getname(),
	    s256->getname(), s512->getname());
	run(ZIO_CHECKSUM_BLAKE3, "blake3 (avx2)");
	run(ZIO_CHECKSUM_SHA256, "sha256 (avx2)");
	run(ZIO_CHECKSUM_SHA512, "sha512/256 (avx2)");
	kernel_fini();
	return (0);
}
