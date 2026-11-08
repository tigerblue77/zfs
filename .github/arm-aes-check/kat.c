// SPDX-License-Identifier: CDDL-1.0
/*
 * KAT / differential harness for openzfs/zfs#18743 (ARMv8 CE AES + PMULL
 * GHASH), run natively on arm64 or under qemu-aarch64.  Compares the PR's
 * implementations with the ICP generic C ones, plus FIPS-197 Appendix C
 * vectors.  Bench/test only, not for upstream.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <aes/aes_impl.h>
#include <modes/gcm_impl.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif

extern const aes_impl_ops_t aes_armv8_impl, aes_generic_impl;
extern const gcm_impl_ops_t gcm_pmull_impl, gcm_generic_impl;

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (rs); }
static void rbytes(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = rnd(); }

/* Mirrors aes_init_keysched() and aes_{en,de}crypt_block() in aes_impl.c */
static void
keysched(const aes_impl_ops_t *ops, const uint8_t *key, int bits, aes_key_t *k)
{
	uint32_t ka[8];
	int n = bits / 8;
	k->nr = bits == 128 ? 10 : bits == 192 ? 12 : 14;
	for (int i = 0; i < n / 4; i++) {
		uint32_t w; memcpy(&w, key + 4 * i, 4);
		ka[i] = ops->needs_byteswap ? __builtin_bswap32(w) : w;
	}
	ops->generate(k, ka, bits);
	k->ops = ops;
}
static void
blk(const aes_key_t *k, int enc, const uint8_t *in, uint8_t *out)
{
	uint32_t b[4];
	memcpy(b, in, 16);
	if (k->ops->needs_byteswap)
		for (int i = 0; i < 4; i++) b[i] = __builtin_bswap32(b[i]);
	if (enc) k->ops->encrypt(k->encr_ks.ks32, k->nr, b, b);
	else k->ops->decrypt(k->decr_ks.ks32, k->nr, b, b);
	if (k->ops->needs_byteswap)
		for (int i = 0; i < 4; i++) b[i] = __builtin_bswap32(b[i]);
	memcpy(out, b, 16);
}
static int hex(const char *s, uint8_t *o) { int n = 0; while (s[0] && s[1]) { sscanf(s, "%2hhx", &o[n++]); s += 2; } return (n); }

int
main(void)
{
	int fail = 0;
#if defined(__linux__)
	unsigned long hwcap = getauxval(AT_HWCAP);
	/* HWCAP_AES 0x8, HWCAP_PMULL 0x10 */
	printf("AT_HWCAP 0x%lx: aes %s, pmull %s\n", hwcap,
	    (hwcap & 0x8) ? "yes" : "no", (hwcap & 0x10) ? "yes" : "no");
	if ((hwcap & 0x18) != 0x18) {
		printf("RESULT: SKIP (no AES/PMULL on this CPU)\n");
		return (2);
	}
#endif
	static const struct { const char *k, *pt, *ct; } fips[] = {
	    { "000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a" },
	    { "000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff", "dda97ca4864cdfe06eaf70a0ec0d7191" },
	    { "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff", "8ea2b7ca516745bfeafc49904b496089" },
	};
	const aes_impl_ops_t *impls[] = { &aes_generic_impl, &aes_armv8_impl };
	for (int ii = 0; ii < 2; ii++) for (int v = 0; v < 3; v++) {
		uint8_t key[32], pt[16], ct[16], out[16];
		int kl = hex(fips[v].k, key); hex(fips[v].pt, pt); hex(fips[v].ct, ct);
		aes_key_t k; keysched(impls[ii], key, kl * 8, &k);
		blk(&k, 1, pt, out); if (memcmp(out, ct, 16)) { printf("FAIL fips enc %s %d\n", impls[ii]->name, kl * 8); fail++; }
		blk(&k, 0, ct, out); if (memcmp(out, pt, 16)) { printf("FAIL fips dec %s %d\n", impls[ii]->name, kl * 8); fail++; }
	}
	printf("FIPS-197 C.1-C.3: %s\n", fail ? "FAIL" : "ok (generic and armv8-ce, enc+dec)");

	/* Differential AES + key-schedule bounds (canary) check */
	long naes = 0;
	for (int it = 0; it < 20000; it++) {
		int bits = (int[]){128, 192, 256}[it % 3];
		uint8_t key[32], in[16], o1[16], o2[16];
		rbytes(key, 32); rbytes(in, 16);
		aes_key_t kg, ka;
		memset(&ka, 0xa5, sizeof (ka));
		keysched(&aes_generic_impl, key, bits, &kg);
		keysched(&aes_armv8_impl, key, bits, &ka);
		int used = 4 * (ka.nr + 1);
		for (int w = used; w < 60; w++)
			if (ka.encr_ks.ks32[w] != 0xa5a5a5a5u || ka.decr_ks.ks32[w] != 0xa5a5a5a5u) {
				printf("FAIL keysched writes past %d words (bits %d)\n", used, bits); fail++; break;
			}
		for (int e = 0; e <= 1; e++) {
			blk(&kg, e, in, o1); blk(&ka, e, in, o2); naes++;
			if (memcmp(o1, o2, 16)) { printf("FAIL aes %s bits %d it %d\n", e ? "enc" : "dec", bits, it); fail++; }
		}
	}
	printf("AES differential: %ld blocks, %s\n", naes, fail ? "FAIL" : "ok");

	/* Differential GHASH multiply, random and edge-case operands */
	long ng = 0; int gfail = 0;
	for (int it = 0; it < 200000; it++) {
		uint64_t x[2], y[2], r1[2], r2[2];
		rbytes((uint8_t *)x, 16); rbytes((uint8_t *)y, 16);
		switch (it % 8) {
		case 0: y[0] = y[1] = 0; break;
		case 1: memset(y, 0xff, 16); break;
		case 2: x[0] = x[1] = 0; break;
		case 3: ((uint8_t *)y)[0] |= 0x80; break;	/* MSB of H set */
		case 4: ((uint8_t *)y)[15] |= 0x01; break;	/* LSB of H set */
		case 5: ((uint8_t *)y)[15] &= 0xfe; ((uint8_t *)y)[0] &= 0x7f; break;
		}
		gcm_generic_impl.mul(x, y, r1);
		gcm_pmull_impl.mul(x, y, r2); ng++;
		if (memcmp(r1, r2, 16)) { if (gfail++ < 5) printf("FAIL ghash mul it %d\n", it); }
	}
	fail += gfail;
	printf("GHASH mul differential: %ld products, %d mismatches\n", ng, gfail);
	printf("%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");
	return (fail != 0);
}
