/* ICP-glue and Linux-glue AES-256-GCM message functions (benchmark) */
#ifndef GCMGLUE_H
#define GCMGLUE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- ICP side ---------------- */
#ifndef GLUE_EXTERN_MOVBE
int gcm_avx_can_use_movbe = 1;	/* boolean_t, read by aesni-gcm-x86_64.S */
#endif
extern int rijndael_key_setup_enc_intel(uint32_t rk[], const uint32_t key[],
    uint64_t bits);
extern void aes_encrypt_intel(const uint32_t rk[], int nr,
    const uint32_t pt[4], uint32_t ct[4]);
extern void gcm_init_htab_avx(uint64_t *Htable, const uint64_t H[2]);
extern void gcm_ghash_avx(uint64_t ghash[2], const uint64_t *Htable,
    const uint8_t *in, size_t len);
extern size_t aesni_gcm_encrypt(const uint8_t *, uint8_t *, size_t,
    const void *, uint64_t *, uint64_t *);
extern size_t aesni_gcm_decrypt(const uint8_t *, uint8_t *, size_t,
    const void *, uint64_t *, uint64_t *);
extern void gcm_xor_avx(const uint8_t *src, uint8_t *dst);

/* Same layout as module/icp/include/aes/aes_impl.h aes_key_t on amd64. */
typedef struct {
	uint32_t encr_ks[60];
	uint32_t decr_ks[60];
	long double align128;
	const void *ops;
	int nr;
	int type;
} icp_aes_key_t;
_Static_assert(offsetof(icp_aes_key_t, nr) == 504, "ICP nr offset");

/* The part of gcm_ctx_t the asm sees: Htable pointer at ghash + 32. */
typedef struct {
	uint64_t ghash[2];
	uint64_t H[2];
	uint64_t *Htable;
	size_t pad;
	uint64_t cb[2];
	uint64_t J0[2];
	uint64_t len_a_len_c[2];
	uint8_t tmp[16];
	uint8_t rem[16];
	uint64_t htab[2 * 6 * 2] __attribute__((aligned(16)));
} icp_state_t;
_Static_assert(offsetof(icp_state_t, Htable) - offsetof(icp_state_t, ghash)
    == 32, "Htable offset");

#define	ICP_CHUNK	(((32 * 1024) / 96) * 96)	/* gcm_avx_chunk_size */
#define	ICP_MIN_ENC	(16 * 6 * 3)
#define	ICP_MIN_DEC	(16 * 6)

static inline void
icp_incr(uint64_t *cb)
{
	uint64_t mask = __builtin_bswap64(0x00000000ffffffffULL);
	uint64_t c = __builtin_bswap64(cb[1] & mask);
	c = __builtin_bswap64(c + 1) & mask;
	cb[1] = (cb[1] & ~mask) | c;
}

static void
icpg_init(icp_state_t *s, const icp_aes_key_t *k, const uint8_t iv[12])
{
	uint8_t *cb = (uint8_t *)s->cb;

	memset(s->H, 0, 16);
	aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s->H,
	    (uint32_t *)s->H);
	s->Htable = s->htab;
	gcm_init_htab_avx(s->Htable, s->H);
	memcpy(cb, iv, 12);
	cb[12] = cb[13] = cb[14] = 0;
	cb[15] = 1;
	memcpy(s->J0, s->cb, 16);
	memset(s->ghash, 0, 16);
	icp_incr(s->cb);
	s->len_a_len_c[0] = 0;
}

/* gcm_mode_encrypt_contiguous_blocks_avx + gcm_encrypt_final_avx */
static void
icp_encrypt_msg(const icp_aes_key_t *k, const uint8_t iv[12],
    const uint8_t *in, uint8_t *out, size_t len, uint8_t tag[16])
{
	icp_state_t s;
	size_t bleft = len, done;

	icpg_init(&s, k, iv);
	for (; bleft >= ICP_CHUNK; bleft -= ICP_CHUNK) {
		done = aesni_gcm_encrypt(in, out, ICP_CHUNK, k, s.cb, s.ghash);
		if (done != ICP_CHUNK)
			abort();
		in += done;
		out += done;
	}
	if (bleft >= ICP_MIN_ENC) {
		done = aesni_gcm_encrypt(in, out, bleft, k, s.cb, s.ghash);
		in += done;
		out += done;
		bleft -= done;
	}
	while (bleft >= 16) {
		aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.cb,
		    (uint32_t *)s.tmp);
		gcm_xor_avx(in, s.tmp);
		gcm_ghash_avx(s.ghash, s.Htable, s.tmp, 16);
		memcpy(out, s.tmp, 16);
		icp_incr(s.cb);
		in += 16;
		out += 16;
		bleft -= 16;
	}
	if (bleft > 0) {
		aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.cb,
		    (uint32_t *)s.tmp);
		memset(s.rem, 0, 16);
		memcpy(s.rem, in, bleft);
		for (size_t i = 0; i < bleft; i++)
			s.rem[i] ^= s.tmp[i];
		memset(s.rem + bleft, 0, 16 - bleft);
		gcm_ghash_avx(s.ghash, s.Htable, s.rem, 16);
		memcpy(out, s.rem, bleft);
	}
	s.len_a_len_c[1] = __builtin_bswap64((uint64_t)len * 8);
	gcm_ghash_avx(s.ghash, s.Htable, (uint8_t *)s.len_a_len_c, 16);
	aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.J0,
	    (uint32_t *)s.J0);
	gcm_xor_avx((uint8_t *)s.J0, (uint8_t *)s.ghash);
	memcpy(tag, s.ghash, 16);
}

/* gcm_decrypt_final_avx (in place, as the ICP does on gcm_pt_buf) */
static bool
icp_decrypt_msg(const icp_aes_key_t *k, const uint8_t iv[12],
    uint8_t *buf, size_t len, const uint8_t tag[16])
{
	icp_state_t s;
	uint8_t *datap = buf;
	size_t bleft, done;

	icpg_init(&s, k, iv);
	for (bleft = len; bleft >= ICP_CHUNK; bleft -= ICP_CHUNK) {
		done = aesni_gcm_decrypt(datap, datap, ICP_CHUNK, k, s.cb,
		    s.ghash);
		if (done != ICP_CHUNK)
			abort();
		datap += done;
	}
	if (bleft >= ICP_MIN_DEC) {
		done = aesni_gcm_decrypt(datap, datap, bleft, k, s.cb, s.ghash);
		datap += done;
		bleft -= done;
	}
	while (bleft > 0) {
		if (bleft < 16) {
			memset(s.rem, 0, 16);
			memcpy(s.rem, datap, bleft);
			gcm_ghash_avx(s.ghash, s.Htable, s.rem, 16);
			aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.cb,
			    (uint32_t *)s.tmp);
			for (size_t i = 0; i < bleft; i++)
				datap[i] = s.rem[i] ^ s.tmp[i];
			break;
		}
		gcm_ghash_avx(s.ghash, s.Htable, datap, 16);
		aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.cb,
		    (uint32_t *)s.tmp);
		gcm_xor_avx(s.tmp, datap);
		icp_incr(s.cb);
		datap += 16;
		bleft -= 16;
	}
	s.len_a_len_c[1] = __builtin_bswap64((uint64_t)len * 8);
	gcm_ghash_avx(s.ghash, s.Htable, (uint8_t *)s.len_a_len_c, 16);
	aes_encrypt_intel(k->encr_ks, k->nr, (uint32_t *)s.J0,
	    (uint32_t *)s.J0);
	gcm_xor_avx((uint8_t *)s.J0, (uint8_t *)s.ghash);
	return (memcmp(s.ghash, tag, 16) == 0);
}

/* ---------------- Linux (Biggers) side ---------------- */
/* struct aes_gcm_key_aesni, offsets as in the .S file */
typedef struct {
	uint8_t b[480];
} __attribute__((aligned(16))) lx_key_t;

extern void icp_aes_gcm_precompute_aesni_avx(lx_key_t *);
extern void icp_aes_gcm_enc_update_aesni_avx(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], const uint8_t *src,
    uint8_t *dst, int len);
extern void icp_aes_gcm_dec_update_aesni_avx(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], const uint8_t *src,
    uint8_t *dst, int len);
extern void icp_aes_gcm_enc_final_aesni_avx(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], uint64_t aadlen,
    uint64_t datalen);
extern bool icp_aes_gcm_dec_final_aesni_avx(const lx_key_t *,
    const uint32_t le_ctr[4], const uint8_t acc[16], uint64_t aadlen,
    uint64_t datalen, const uint8_t tag[16], int taglen);
extern void icp_aes_gcm_precompute_aesni(lx_key_t *);
extern void icp_aes_gcm_enc_update_aesni(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], const uint8_t *src,
    uint8_t *dst, int len);
extern void icp_aes_gcm_dec_update_aesni(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], const uint8_t *src,
    uint8_t *dst, int len);
extern void icp_aes_gcm_enc_final_aesni(const lx_key_t *,
    const uint32_t le_ctr[4], uint8_t acc[16], uint64_t aadlen,
    uint64_t datalen);
extern bool icp_aes_gcm_dec_final_aesni(const lx_key_t *,
    const uint32_t le_ctr[4], const uint8_t acc[16], uint64_t aadlen,
    uint64_t datalen, const uint8_t tag[16], int taglen);

/* Linux variant in use: AES-NI + AVX (default) or AES-NI + SSE4.1 only */
static void (*lx_precompute)(lx_key_t *) = icp_aes_gcm_precompute_aesni_avx;
static void (*lx_enc_update)(const lx_key_t *, const uint32_t *, uint8_t *,
    const uint8_t *, uint8_t *, int) = icp_aes_gcm_enc_update_aesni_avx;
static void (*lx_dec_update)(const lx_key_t *, const uint32_t *, uint8_t *,
    const uint8_t *, uint8_t *, int) = icp_aes_gcm_dec_update_aesni_avx;
static void (*lx_enc_final)(const lx_key_t *, const uint32_t *, uint8_t *,
    uint64_t, uint64_t) = icp_aes_gcm_enc_final_aesni_avx;
static bool (*lx_dec_final)(const lx_key_t *, const uint32_t *,
    const uint8_t *, uint64_t, uint64_t, const uint8_t *, int) =
    icp_aes_gcm_dec_final_aesni_avx;

static void
lx_key_from_icp(lx_key_t *lk, const icp_aes_key_t *k, uint32_t keylen)
{
	memset(lk, 0, sizeof (*lk));
	memcpy(lk->b + 0, &keylen, 4);
	memcpy(lk->b + 16, k->encr_ks, (k->nr + 1) * 16);
}

static inline void
lx_ctr(uint32_t le_ctr[4], const uint8_t iv[12])
{
	le_ctr[0] = 2;
	le_ctr[1] = __builtin_bswap32(*(const uint32_t *)(iv + 8));
	le_ctr[2] = __builtin_bswap32(*(const uint32_t *)(iv + 4));
	le_ctr[3] = __builtin_bswap32(*(const uint32_t *)(iv + 0));
}

static void
lx_encrypt_msg(lx_key_t *lk, const uint8_t iv[12], const uint8_t *in,
    uint8_t *out, size_t len, uint8_t tag[16])
{
	uint32_t le_ctr[4];
	uint8_t acc[16] = { 0 };
	size_t bleft = len;

	lx_precompute(lk);
	lx_ctr(le_ctr, iv);
	for (; bleft > ICP_CHUNK; bleft -= ICP_CHUNK) {
		lx_enc_update(lk, le_ctr, acc, in, out,
		    ICP_CHUNK);
		le_ctr[0] += ICP_CHUNK / 16;
		in += ICP_CHUNK;
		out += ICP_CHUNK;
	}
	lx_enc_update(lk, le_ctr, acc, in, out, bleft);
	lx_enc_final(lk, le_ctr, acc, 0, len);
	memcpy(tag, acc, 16);
}

static bool
lx_decrypt_msg(lx_key_t *lk, const uint8_t iv[12], uint8_t *buf, size_t len,
    const uint8_t tag[16])
{
	uint32_t le_ctr[4];
	uint8_t acc[16] = { 0 };
	size_t bleft = len;

	lx_precompute(lk);
	lx_ctr(le_ctr, iv);
	for (; bleft > ICP_CHUNK; bleft -= ICP_CHUNK) {
		lx_dec_update(lk, le_ctr, acc, buf, buf,
		    ICP_CHUNK);
		le_ctr[0] += ICP_CHUNK / 16;
		buf += ICP_CHUNK;
	}
	lx_dec_update(lk, le_ctr, acc, buf, buf, bleft);
	return (lx_dec_final(lk, le_ctr, acc, 0, len, tag,
	    16));
}

#endif
