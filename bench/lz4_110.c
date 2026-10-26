/* Plain-libc build of the LZ4 1.10.0 sources vendored on lz4hc-poc. */
#include "lz4.c"
#include "lz4hc.c"
int
lz4_110_compress(const char *s, char *d, int sl, int dl, int level)
{
	if (level == 0)
		return (LZ4_compress_default(s, d, sl, dl));
	return (LZ4_compress_HC(s, d, sl, dl, level));
}
int
lz4_110_decompress(const char *s, char *d, int sl, int dl)
{
	return (LZ4_decompress_safe(s, d, sl, dl));
}
