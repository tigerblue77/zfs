#include <sys/zfs_context.h>
#include <sys/zio_crypt.h>
#include <sys/hkdf.h>
#include <libzpool.h>
#include <time.h>

extern int gcm_impl_set(const char *);
extern int aes_impl_set(const char *);

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

int
main(int argc, char **argv)
{
	const char *gimpl = argc > 1 ? argv[1] : "avx";
	kernel_init(SPA_MODE_READ);
	VERIFY0(gcm_impl_set(gimpl));
	VERIFY0(aes_impl_set("aesni"));
	zio_crypt_key_t key;
	VERIFY0(zio_crypt_key_init(ZIO_CRYPT_AES_256_GCM, &key));
	size_t sizes[] = { 4096, 16384, 131072 };
	uint8_t *plain = malloc(131072), *cipher = malloc(131072);
	for (int i = 0; i < 131072; i++)
		plain[i] = (uint8_t)(i * 2654435761u >> 13);
	uint8_t iv[ZIO_DATA_IV_LEN] = { 1 }, mac[ZIO_DATA_MAC_LEN];
	uint8_t other[ZIO_DATA_SALT_LEN];
	memcpy(other, key.zk_salt, sizeof (other));
	other[0] ^= 0xff;
	boolean_t nc;

	/* HKDF alone */
	uint8_t out[32];
	int n = 20000;
	double t0 = now();
	for (int i = 0; i < n; i++)
		VERIFY0(hkdf_sha512(key.zk_master_keydata, 32, NULL, 0,
		    other, ZIO_DATA_SALT_LEN, out, 32));
	double t = (now() - t0) / n;
	printf("gcm=%s hkdf_sha512: %.2f us/call\n", gimpl, t * 1e6);

	for (int s = 0; s < 3; s++) {
		size_t len = sizes[s];
		int iters = (int)(400 * 1024 * 1024 / len);
		/* decrypt, current salt */
		VERIFY0(zio_do_crypt_data(B_TRUE, &key, DMU_OT_PLAIN_FILE_CONTENTS,
		    B_FALSE, key.zk_salt, iv, mac, len, plain, cipher, &nc));
		t0 = now();
		for (int i = 0; i < iters; i++)
			VERIFY0(zio_do_crypt_data(B_FALSE, &key,
			    DMU_OT_PLAIN_FILE_CONTENTS, B_FALSE, key.zk_salt, iv,
			    mac, len, plain, cipher, &nc));
		double tc = (now() - t0) / iters;
		/* decrypt, older salt (any block written before this key load) */
		VERIFY0(zio_do_crypt_data(B_TRUE, &key, DMU_OT_PLAIN_FILE_CONTENTS,
		    B_FALSE, other, iv, mac, len, plain, cipher, &nc));
		t0 = now();
		for (int i = 0; i < iters; i++)
			VERIFY0(zio_do_crypt_data(B_FALSE, &key,
			    DMU_OT_PLAIN_FILE_CONTENTS, B_FALSE, other, iv, mac,
			    len, plain, cipher, &nc));
		double to = (now() - t0) / iters;
		/* dedup IV/salt derivation: HMAC-SHA512 over the plaintext */
		uint8_t dsalt[ZIO_DATA_SALT_LEN], div[ZIO_DATA_IV_LEN];
		int hi = iters / 4 + 1;
		t0 = now();
		for (int i = 0; i < hi; i++)
			VERIFY0(zio_crypt_generate_iv_salt_dedup(&key, plain, len,
			    div, dsalt));
		double th = (now() - t0) / hi;
		printf("%6zu B: decrypt current salt %.2f us (%.2f GB/s), "
		    "older salt %.2f us (+%.0f%%), dedup HMAC-SHA512 %.2f us "
		    "(%.2f GB/s)\n", len, tc * 1e6, len / tc / 1e9, to * 1e6,
		    (to / tc - 1) * 100, th * 1e6, len / th / 1e9);
	}
	zio_crypt_key_destroy(&key);
	kernel_fini();
	return (0);
}
