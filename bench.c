/*
 * AES-256-GCM benchmark: OpenZFS ICP "avx" path (OpenSSL aesni-gcm-x86_64.S
 * + ghash-x86_64.S, as built in the OpenZFS tree) against Eric Biggers'
 * Linux aes-gcm-aesni-x86_64.S (AES-NI + AVX variant), in userspace.
 *
 * Modes, per size:
 *   asm  - one call of the bulk routine over the buffer
 *          (ICP: aesni_gcm_{en,de}crypt, which only takes multiples of 96
 *           bytes; throughput is computed on the bytes it processed)
 *   msg  - whole message as the ICP glue does it in gcm.c: H/Htable init,
 *          bulk in gcm_avx_chunk_size (32736 B) pieces, per-block tail with
 *          aes_encrypt_intel + gcm_ghash_avx, length block, tag.
 *          Linux side: precompute, update in the same pieces, final.
 *          Both exclude the crypto framework (key schedule, allocations,
 *          the ICP's bounce-buffer copy), which a port would not change.
 *
 * Correctness: both implementations are checked against OpenSSL EVP
 * (libcrypto) on many lengths, ciphertext and tag, before timing.
 */
#define	_GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
#include <openssl/evp.h>

#include "gcmglue.h"

/* ---------------- reference ---------------- */
static void
ossl_encrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *in,
    uint8_t *out, size_t len, uint8_t tag[16])
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int l;

	EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, iv);
	if (len > 0)
		EVP_EncryptUpdate(c, out, &l, in, (int)len);
	EVP_EncryptFinal_ex(c, out + len, &l);
	EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
	EVP_CIPHER_CTX_free(c);
}

/* ---------------- timing ---------------- */
static inline uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

enum { ICP_ASM_ENC, LX_ASM_ENC, ICP_ASM_DEC, LX_ASM_DEC,
    ICP_MSG_ENC, LX_MSG_ENC, ICP_MSG_DEC, LX_MSG_DEC, NCASE };
static const char *casename[NCASE] = {
	"icp-asm-enc", "lx-asm-enc", "icp-asm-dec", "lx-asm-dec",
	"icp-msg-enc", "lx-msg-enc", "icp-msg-dec", "lx-msg-dec",
};

static icp_aes_key_t ik;
static lx_key_t lk;
static uint8_t key[32], iv[12];
static uint8_t *src, *dst;

/* returns bytes processed per op */
static size_t
run_once(int which, size_t len)
{
	icp_state_t s;
	uint32_t le_ctr[4];
	uint8_t acc[16] = { 0 };
	uint8_t tag[16] = { 0 };

	switch (which) {
	case ICP_ASM_ENC:
		icpg_init(&s, &ik, iv);
		return (aesni_gcm_encrypt(src, dst, len, &ik, s.cb, s.ghash));
	case ICP_ASM_DEC:
		icpg_init(&s, &ik, iv);
		return (aesni_gcm_decrypt(src, dst, len, &ik, s.cb, s.ghash));
	case LX_ASM_ENC:
		lx_ctr(le_ctr, iv);
		lx_enc_update(&lk, le_ctr, acc, src, dst,
		    len);
		return (len);
	case LX_ASM_DEC:
		lx_ctr(le_ctr, iv);
		lx_dec_update(&lk, le_ctr, acc, src, dst,
		    len);
		return (len);
	case ICP_MSG_ENC:
		icp_encrypt_msg(&ik, iv, src, dst, len, tag);
		return (len);
	case LX_MSG_ENC:
		lx_encrypt_msg(&lk, iv, src, dst, len, tag);
		return (len);
	case ICP_MSG_DEC:
		/*
		 * Decrypt dst in place again and again: the code is constant
		 * time, so its contents (and the failing tag) change nothing.
		 */
		(void) icp_decrypt_msg(&ik, iv, dst, len, tag);
		return (len);
	case LX_MSG_DEC:
		(void) lx_decrypt_msg(&lk, iv, dst, len, tag);
		return (len);
	}
	abort();
}

static int
cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return ((x > y) - (x < y));
}

/*
 * ns per byte for one measurement of ~target_ns: the median over batches
 * of ~0.5 ms, so that preemption by other load inflates only a few batches.
 */
#define	MAXB	8192
static double
measure(int which, size_t len, uint64_t target_ns, double *gbps)
{
	static double b[MAXB];
	size_t per = 0, nb = 0;
	uint64_t t0, ts, te;
	int k;

	for (int i = 0; i < 3; i++)	/* warm-up */
		per = run_once(which, len);
	k = (int)(500000 / (len / 4 + 1000));	/* ~0.5 ms at ~4 B/ns */
	if (k < 1)
		k = 1;
	t0 = now_ns();
	do {
		ts = now_ns();
		for (int i = 0; i < k; i++)
			(void) run_once(which, len);
		te = now_ns();
		b[nb++] = (double)(te - ts) / ((double)k * per);
	} while (te - t0 < target_ns && nb < MAXB);
	qsort(b, nb, sizeof (double), cmpd);
	*gbps = 1 / b[nb / 2];
	return (b[nb / 2]);
}

/*
 * Paired measurement: alternate ~0.5 ms batches of case a and case b
 * (order flipped every pair) for ~target_ns.  Returns the median ns/byte of
 * each and the median of the per-pair ratios ta/tb, which cancels clock
 * and load drift that hits both members of a pair alike.
 */
static void
measure_pair(int ca, int cb, size_t len, uint64_t target_ns,
    double *ma, double *mb, double *mr)
{
	static double ba[MAXB], bb[MAXB], br[MAXB];
	size_t pa = 0, pb = 0, n = 0;
	uint64_t t0, ts, te;
	int k;

	for (int i = 0; i < 3; i++) {
		pa = run_once(ca, len);
		pb = run_once(cb, len);
	}
	k = (int)(500000 / (len / 4 + 1000));
	if (k < 1)
		k = 1;
	t0 = now_ns();
	do {
		double ta = 0, tb = 1;
		for (int o = 0; o < 2; o++) {
			int c = ((n & 1) ^ o) ? cb : ca;
			ts = now_ns();
			for (int i = 0; i < k; i++)
				(void) run_once(c, len);
			te = now_ns();
			double t = (double)(te - ts) / k;
			if (c == ca)
				ta = t / pa;
			else
				tb = t / pb;
		}
		ba[n] = ta;
		bb[n] = tb;
		br[n] = ta / tb;
		n++;
	} while (te - t0 < target_ns && n < MAXB);
	qsort(ba, n, sizeof (double), cmpd);
	qsort(bb, n, sizeof (double), cmpd);
	qsort(br, n, sizeof (double), cmpd);
	*ma = ba[n / 2];
	*mb = bb[n / 2];
	*mr = br[n / 2];
}

static int
selftest(void)
{
	static const size_t lens[] = { 0, 1, 15, 16, 17, 63, 64, 95, 96, 97,
	    127, 128, 129, 255, 287, 288, 289, 300, 1000, 4095, 4096, 4097,
	    16384, 32736, 32737, 65536, 131072, 131075, 1048576, 1048577 };
	uint8_t *r = malloc(2 << 20), *a = malloc(2 << 20);
	uint8_t rt[16], at[16];
	int bad = 0;

	for (size_t i = 0; i < sizeof (lens) / sizeof (lens[0]); i++) {
		size_t n = lens[i];
		ossl_encrypt(key, iv, src, r, n, rt);

		icp_encrypt_msg(&ik, iv, src, a, n, at);
		if (memcmp(a, r, n) || memcmp(at, rt, 16)) {
			printf("ICP enc mismatch len %zu\n", n);
			bad++;
		}
		memcpy(a, r, n);
		if (!icp_decrypt_msg(&ik, iv, a, n, rt) || memcmp(a, src, n)) {
			printf("ICP dec mismatch len %zu\n", n);
			bad++;
		}
		lx_encrypt_msg(&lk, iv, src, a, n, at);
		if (memcmp(a, r, n) || memcmp(at, rt, 16)) {
			printf("LX enc mismatch len %zu\n", n);
			bad++;
		}
		memcpy(a, r, n);
		if (!lx_decrypt_msg(&lk, iv, a, n, rt) || memcmp(a, src, n)) {
			printf("LX dec mismatch len %zu\n", n);
			bad++;
		}
		rt[0] ^= 1;
		memcpy(a, r, n);
		if (lx_decrypt_msg(&lk, iv, a, n, rt) ||
		    icp_decrypt_msg(&ik, iv, a, n, rt)) {
			printf("bad tag accepted len %zu\n", n);
			bad++;
		}
	}
	printf("selftest vs OpenSSL EVP: %zu lengths, %d failures\n",
	    sizeof (lens) / sizeof (lens[0]), bad);
	free(r);
	free(a);
	return (bad);
}

int
main(int argc, char **argv)
{
	static const size_t sizes[] = { 4096, 16384, 131072, 1048576 };
	enum { NS = 4, NRUN = 5 };
	double res[NCASE][NS][NRUN];
	uint64_t target = (argc > 1 ? strtoull(argv[1], NULL, 0) : 150) *
	    1000000ULL;
	const double tsc_ghz = argc > 2 ? strtod(argv[2], NULL) : 2.1;

	if (argc > 3 && strcmp(argv[3], "sse") == 0) {
		lx_precompute = icp_aes_gcm_precompute_aesni;
		lx_enc_update = icp_aes_gcm_enc_update_aesni;
		lx_dec_update = icp_aes_gcm_dec_update_aesni;
		lx_enc_final = icp_aes_gcm_enc_final_aesni;
		lx_dec_final = icp_aes_gcm_dec_final_aesni;
		printf("Linux column: AES-NI + SSE4.1 variant (*_aesni)\n");
	} else {
		printf("Linux column: AES-NI + AVX variant (*_aesni_avx)\n");
	}

	src = aligned_alloc(64, 2 << 20);
	dst = aligned_alloc(64, (2 << 20) + 64);
	for (size_t i = 0; i < (2 << 20); i++)
		src[i] = (uint8_t)(i * 131 + 7 + (i >> 9));
	for (int i = 0; i < 32; i++)
		key[i] = (uint8_t)(0x11 * i + 3);
	for (int i = 0; i < 12; i++)
		iv[i] = (uint8_t)(0xa5 ^ (i * 29));

	memset(&ik, 0, sizeof (ik));
	ik.nr = rijndael_key_setup_enc_intel(ik.encr_ks,
	    (const uint32_t *)key, 256);
	if (ik.nr != 14)
		abort();
	lx_key_from_icp(&lk, &ik, 32);
	lx_precompute(&lk);

	if (selftest() != 0)
		return (1);

	static double rat[NCASE][NS][NRUN];
	(void) measure;

	/* run r does every case and size, ICP/Linux paired batch by batch */
	for (int r = 0; r < NRUN; r++) {
		for (int si = 0; si < NS; si++) {
			for (int c = 0; c < NCASE; c += 2) {
				measure_pair(c, c + 1, sizes[si], target,
				    &res[c][si][r], &res[c + 1][si][r],
				    &rat[c][si][r]);
			}
		}
		fprintf(stderr, "run %d done\n", r + 1);
	}

	printf("\nAES-256-GCM, median of %d interleaved runs "
	    "(GB/s ; TSC cycles/byte @ %.2f GHz ; min-max GB/s over runs)\n"
	    "ratio = Linux speed / ICP speed, median of per-run paired "
	    "ratios [min-max]\n", NRUN, tsc_ghz);
	printf("%-8s %-8s %28s %28s %20s\n", "mode", "size", "ICP (OpenSSL)",
	    "Linux (Biggers)", "ratio");
	for (int c = 0; c < NCASE; c += 2) {
		for (int si = 0; si < NS; si++) {
			double a[NRUN], b[NRUN], q[NRUN];
			memcpy(a, res[c][si], sizeof (a));
			memcpy(b, res[c + 1][si], sizeof (b));
			memcpy(q, rat[c][si], sizeof (q));
			qsort(a, NRUN, sizeof (double), cmpd);
			qsort(b, NRUN, sizeof (double), cmpd);
			qsort(q, NRUN, sizeof (double), cmpd);
			double ma = a[NRUN / 2], mb = b[NRUN / 2];
			char sa[64], sb[64];
			snprintf(sa, sizeof (sa), "%5.2f ; %4.2f ; %4.2f-%4.2f",
			    1 / ma, ma * tsc_ghz, 1 / a[NRUN - 1], 1 / a[0]);
			snprintf(sb, sizeof (sb), "%5.2f ; %4.2f ; %4.2f-%4.2f",
			    1 / mb, mb * tsc_ghz, 1 / b[NRUN - 1], 1 / b[0]);
			printf("%-8.7s %-8zu %28s %28s  %5.3fx [%5.3f-%5.3f]\n",
			    casename[c] + 4, sizes[si], sa, sb, q[NRUN / 2],
			    q[0], q[NRUN - 1]);
		}
	}
	return (0);
}
