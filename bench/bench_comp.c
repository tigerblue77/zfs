#include <sys/zfs_context.h>
#include <sys/zio_compress.h>
#include <sys/abd.h>
#include <sys/spa.h>
#include <libzpool.h>
#include <time.h>
#include <fcntl.h>

extern int lz4_110_compress(const char *, char *, int, int, int);
extern int lz4_110_decompress(const char *, char *, int, int);
extern int LZ4_uncompress_unknownOutputSize(const char *, char *, int, int);

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

enum { R85, L110, HC, ZSTD };

static void
run(const char *corpus, uint8_t *data, size_t total, size_t bs, int kind,
    int level, const char *label)
{
	size_t nblk = total / bs;
	uint8_t *out = malloc(nblk * bs), *back = malloc(bs);
	size_t *clen = calloc(nblk, sizeof (size_t));
	size_t sum = 0;
	double t0 = now();
	for (size_t b = 0; b < nblk; b++) {
		uint8_t *src = data + b * bs, *dst = out + b * bs;
		size_t c;
		if (kind == R85 || kind == ZSTD) {
			abd_t *sa = abd_get_from_buf(src, bs);
			abd_t *da = abd_get_from_buf(dst, bs);
			c = zio_compress_data(kind == R85 ? ZIO_COMPRESS_LZ4 :
			    ZIO_COMPRESS_ZSTD, sa, &da, bs, bs - bs / 8, level);
			abd_free(sa);
			abd_free(da);
		} else {
			int r = lz4_110_compress((char *)src, (char *)dst + 4,
			    bs, bs - bs / 8 - 4, kind == HC ? level : 0);
			c = r > 0 ? (size_t)r + 4 : bs;
			if (r > 0) {
				uint32_t be = htonl(r);
				memcpy(dst, &be, 4);
			}
		}
		clen[b] = c;
		sum += (c >= bs - bs / 8) ? bs : c;
	}
	double tc = now() - t0;
	int reps = 3;
	t0 = now();
	for (int r = 0; r < reps; r++) {
		for (size_t b = 0; b < nblk; b++) {
			uint8_t *dst = out + b * bs;
			if (clen[b] >= bs - bs / 8)
				continue;
			if (kind == ZSTD) {
				abd_t *sa = abd_get_from_buf(dst, clen[b]);
				abd_t *da = abd_get_from_buf(back, bs);
				uint8_t lvl;
				VERIFY0(zio_decompress_data(ZIO_COMPRESS_ZSTD,
				    sa, da, clen[b], bs, &lvl));
				abd_free(sa);
				abd_free(da);
			} else {
				uint32_t be;
				memcpy(&be, dst, 4);
				int n = LZ4_uncompress_unknownOutputSize(
				    (char *)dst + 4, (char *)back, ntohl(be), bs);
				VERIFY3S(n, ==, (int)bs);
			}
		}
	}
	double td = (now() - t0) / reps;
	printf("%-8s %4zuK %-10s ratio %.3f  comp %7.1f MB/s  decomp %7.1f MB/s\n",
	    corpus, bs / 1024, label, (double)(nblk * bs) / sum,
	    nblk * bs / tc / 1e6, nblk * bs / td / 1e6);
	free(out);
	free(back);
	free(clen);
}

int
main(int argc, char **argv)
{
	kernel_init(SPA_MODE_READ);
	for (int f = 1; f < argc; f++) {
		int fd = open(argv[f], O_RDONLY);
		VERIFY3S(fd, >=, 0);
		size_t total = 64 << 20;
		uint8_t *data = malloc(total);
		ssize_t n = read(fd, data, total);
		close(fd);
		const char *name = strrchr(argv[f], '/') + 1;
		size_t bss[] = { 16384, 131072 };
		for (int i = 0; i < 2; i++) {
			size_t bs = bss[i];
			run(name, data, n, bs, R85, ZIO_COMPLEVEL_DEFAULT,
			    "lz4-r85");
			run(name, data, n, bs, L110, 0, "lz4-1.10");
			run(name, data, n, bs, HC, 4, "lz4hc-4");
			run(name, data, n, bs, HC, 9, "lz4hc-9");
			run(name, data, n, bs, HC, 12, "lz4hc-12");
			run(name, data, n, bs, ZSTD, ZIO_ZSTD_LEVEL_1, "zstd-1");
			run(name, data, n, bs, ZSTD, ZIO_ZSTD_LEVEL_3, "zstd-3");
		}
		free(data);
	}
	kernel_fini();
	return (0);
}
