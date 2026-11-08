/* Harness stub: layout copied from module/icp/include/aes/aes_impl.h. */
#include <sys/types.h>
#include <sys/asm_linkage.h>
#define	AES_BLOCK_LEN	16
#define	RC_LENGTH	(5 * ((AES_BLOCK_LEN) / 4 - 2))
#define	MAX_AES_NR	14
#define	MAX_AES_NB	4
typedef union {
	uint32_t ks32[((MAX_AES_NR) + 1) * (MAX_AES_NB)];
} aes_ks_t;
typedef struct aes_impl_ops aes_impl_ops_t;
typedef struct aes_key aes_key_t;
struct aes_key {
	aes_ks_t encr_ks;
	aes_ks_t decr_ks;
	const aes_impl_ops_t *ops;
	int nr;
	int type;
};
typedef void (*aes_generate_f)(aes_key_t *, const uint32_t *, int);
typedef void (*aes_encrypt_f)(const uint32_t[], int, const uint32_t[4],
    uint32_t[4]);
typedef void (*aes_decrypt_f)(const uint32_t[], int, const uint32_t[4],
    uint32_t[4]);
typedef boolean_t (*aes_will_work_f)(void);
struct aes_impl_ops {
	aes_generate_f generate;
	aes_encrypt_f encrypt;
	aes_decrypt_f decrypt;
	aes_will_work_f is_supported;
	boolean_t needs_byteswap;
	char name[16];
};
