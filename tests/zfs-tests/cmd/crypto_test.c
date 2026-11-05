/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, Rob Norris <robn@despairlabs.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * This is a userspace test driver for the ICP. It has two modes:
 *
 * "correctness" (-c <testfile>):
 *   Load a file full of test vectors. For each implementation of the named
 *   algorithm, loop over the tests, and run encrypt and decrypt with the
 *   provided parameters and confirm they either do (result=valid) or do not
 *   (result=invalid) succeed.
 *
 * "performance" (-p <alg>)
 *   For each implementation of the named algorithm, run 1000 rounds of
 *   encrypt() on a range of power-2 sizes of input data from 2^10 (1K) to
 *   2^19 (512K).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <sys/crypto/icp.h>
#include <sys/crypto/api.h>

/* for zfs_nicenum, zfs_nicebytes */
#include <libzutil.h>

/* ========== */

/* types and data for both modes */

/* valid test algorithms */
typedef enum {
	ALG_NONE,
	ALG_AES_GCM,
	ALG_AES_CCM,
} crypto_test_alg_t;

/*
 * Generally the ICP expects zero-length data to still require a valid
 * (non-NULL) pointer, even though it will never read from it. This is a
 * convenient valid item for tjat case.
 */
static uint8_t val_empty[1] = {0};

/* Strings for error returns */
static const char *crypto_errstr[] = {
	[CRYPTO_SUCCESS] =		    "CRYPTO_SUCCESS",
	[CRYPTO_HOST_MEMORY] =		    "CRYPTO_HOST_MEMORY",
	[CRYPTO_FAILED] =		    "CRYPTO_FAILED",
	[CRYPTO_ARGUMENTS_BAD] =	    "CRYPTO_ARGUMENTS_BAD",
	[CRYPTO_DATA_LEN_RANGE] =	    "CRYPTO_DATA_LEN_RANGE",
	[CRYPTO_ENCRYPTED_DATA_LEN_RANGE] = "CRYPTO_ENCRYPTED_DATA_LEN_RANGE",
	[CRYPTO_KEY_SIZE_RANGE] =	    "CRYPTO_KEY_SIZE_RANGE",
	[CRYPTO_KEY_TYPE_INCONSISTENT] =    "CRYPTO_KEY_TYPE_INCONSISTENT",
	[CRYPTO_MECHANISM_INVALID] =	    "CRYPTO_MECHANISM_INVALID",
	[CRYPTO_MECHANISM_PARAM_INVALID] =  "CRYPTO_MECHANISM_PARAM_INVALID",
	[CRYPTO_SIGNATURE_INVALID] =	    "CRYPTO_SIGNATURE_INVALID",
	[CRYPTO_BUFFER_TOO_SMALL] =	    "CRYPTO_BUFFER_TOO_SMALL",
	[CRYPTO_NOT_SUPPORTED] =	    "CRYPTO_NOT_SUPPORTED",
	[CRYPTO_INVALID_CONTEXT] =	    "CRYPTO_INVALID_CONTEXT",
	[CRYPTO_INVALID_MAC] =		    "CRYPTO_INVALID_MAC",
	[CRYPTO_MECH_NOT_SUPPORTED] =	    "CRYPTO_MECH_NOT_SUPPORTED",
	[CRYPTO_INVALID_PROVIDER_ID] =	    "CRYPTO_INVALID_PROVIDER_ID",
	[CRYPTO_BUSY] =			    "CRYPTO_BUSY",
	[CRYPTO_UNKNOWN_PROVIDER] =	    "CRYPTO_UNKNOWN_PROVIDER",
};

/* what to output; driven by -v switch */
typedef enum {
	OUT_SUMMARY,
	OUT_FAIL,
	OUT_ALL,
} crypto_test_outmode_t;


/* ========== */

/* types and data for correctness tests */

/* most ICP inputs are separate val & len */
typedef struct {
	uint8_t		*val;
	size_t		len;
} crypto_test_val_t;

/* tests can be expected to pass (valid) or expected to fail (invalid) */
typedef enum {
	RS_NONE = 0,
	RS_VALID,
	RS_INVALID,
} crypto_test_result_t;

/* a single test, loaded from the test file */
typedef struct crypto_test crypto_test_t;
struct crypto_test {
	crypto_test_t		*next;	    /* ptr to next test */
	char			*fileloc;   /* file:line of test in file */
	crypto_test_alg_t	alg;	    /* alg, for convenience */

	/* id, comment and flags are for output */
	uint64_t		id;
	char			*comment;
	char			*flags;

	/*
	 * raw test params. these are hex strings in the test file, which
	 * we convert on load.
	 */
	crypto_test_val_t	iv;
	crypto_test_val_t	key;
	crypto_test_val_t	msg;
	crypto_test_val_t	ct;
	crypto_test_val_t	aad;
	crypto_test_val_t	tag;

	/* expected result */
	crypto_test_result_t	result;
};

/* ========== */

/* test file loader */

/*
 * helper; split a 'key: value\n' line into separate key and value. original
 * line is modified; \0 will be inserted at end of key and end of value.
 */
static boolean_t
split_kv(char *line, char **kp, char **vp)
{
	char *c = strstr(line, ":");
	if (c == NULL)
		return (B_FALSE);


	*c++ = '\0';
	while (*c == ' ')
		c++;

	char *v = c;
	c = strchr(v, '\n');
	if (c != NULL) {
		*c++ = '\0';
		if (*c != '\0')
			return (B_FALSE);
	}

	*kp = line;
	*vp = v;
	return (B_TRUE);
}

/*
 * helper; parse decimal number to uint64
 */
static boolean_t
parse_num(char *v, uint64_t *np)
{
	char *c = NULL;
	errno = 0;
	uint64_t n = strtoull(v, &c, 10);
	if (*v == '\0' || *c != '\0' || errno != 0 ||
	    n >= UINT32_MAX || n == 0)
		return (B_FALSE);
	*np = n;
	return (B_TRUE);
}

/*
 * load tests from the test file. returns a linked list of tests, and the
 * test algorithm in *algp.
 */
static crypto_test_t *
load_tests(const char *filepath, crypto_test_alg_t *algp)
{
	crypto_test_t *tests = NULL, *tail = NULL;
	char *buf = NULL;
	size_t buflen = 0;
	FILE *fh = NULL;

	if ((fh = fopen(filepath, "r")) == NULL) {
		fprintf(stderr, "E: couldn't open %s: %s\n",
		    filepath, strerror(errno));
		goto err;
	}

	/* extract the filename part from the path, for nicer output */
	const char *filename = &filepath[strlen(filepath)-1];
	while (filename != filepath) {
		if (*filename == '/') {
			filename++;
			break;
		}
		filename--;
	}

	int lineno = 0;

	crypto_test_alg_t alg = ALG_NONE;
	uint64_t ntests = 0;
	crypto_test_t *test = NULL;
	uint64_t ncommitted = 0;

	char *k, *v;

	ssize_t nread;
	while ((nread = getline(&buf, &buflen, fh)) != -1 || errno == 0) {
		/* track line number for output and for test->fileloc */
		lineno++;

		if (nread < 2 && test != NULL) {
			/*
			 * blank line or end of file; close out any test in
			 * progress and commit it.
			 */
			if (test->id == 0 ||
			    test->iv.val == NULL ||
			    test->key.val == NULL ||
			    test->msg.val == NULL ||
			    test->ct.val == NULL ||
			    test->aad.val == NULL ||
			    test->tag.val == NULL ||
			    test->result == RS_NONE) {
				fprintf(stderr, "E: incomplete test [%s:%d]\n",
				    filename, lineno);
				goto err;
			}

			/* commit the test, ie, add it to the list */
			if (tail == NULL)
				tests = tail = test;
			else {
				tail->next = test;
				tail = test;
			}
			ncommitted++;

			test = NULL;
		}

		if (nread == -1)
			/* end of file and tests finished, done */
			break;

		if (nread < 2 && ncommitted == 0) {
			/*
			 * blank line after header, make sure the header is
			 * complete.
			 */
			if (alg == ALG_NONE || ntests == 0) {
				fprintf(stderr, "E: incomplete header "
				    "[%s:%d]\n", filename, lineno);
				goto err;
			}
		}

		if (nread < 2) {
			/*
			 * blank line and the header is committed, and no
			 * current test, so the next test will start on the
			 * next line.
			 */
			test = calloc(1, sizeof (crypto_test_t));
			int len = strlen(filename) + 10;
			test->fileloc = calloc(len, 1);
			snprintf(test->fileloc, len, "%s:%d",
			    filename, lineno+1);
			test->alg = alg;
			continue;
		}

		/*
		 * must be a k:v line. if there is a current test, then this
		 * line is part of it, otherwise it's a header line
		 */
		if (!split_kv(buf, &k, &v)) {
			fprintf(stderr, "E: malformed line [%s:%d]\n",
			    filename, lineno);
			goto err;
		}

		if (test == NULL) {
			/* no current test, so a header key */

			/*
			 * typical header:
			 *
			 * algorithm: AES-GCM
			 * tests: 316
			 */
			if (strcmp(k, "algorithm") == 0) {
				if (alg != ALG_NONE)
					goto err_dup_key;
				if (strcmp(v, "AES-GCM") == 0)
					alg = ALG_AES_GCM;
				else if (strcmp(v, "AES-CCM") == 0)
					alg = ALG_AES_CCM;
				else {
					fprintf(stderr,
					    "E: unknown algorithm [%s:%d]: "
					    "%s\n", filename, lineno, v);
					goto err;
				}
			} else if (strcmp(k, "tests") == 0) {
				if (ntests > 0)
					goto err_dup_key;
				if (!parse_num(v, &ntests)) {
					fprintf(stderr,
					    "E: invalid number of tests "
					    "[%s:%d]: %s\n", filename, lineno,
					    v);
					goto err;
				}
			} else {
				fprintf(stderr, "E: unknown header key "
				    "[%s:%d]: %s\n", filename, lineno, k);
				goto err;
			}
			continue;
		}

		/* test key */

		/*
		 * typical test:
		 *
		 * id: 48
		 * comment: Flipped bit 63 in tag
		 * flags: ModifiedTag
		 * iv: 505152535455565758595a5b
		 * key: 000102030405060708090a0b0c0d0e0f
		 * msg: 202122232425262728292a2b2c2d2e2f
		 * ct: eb156d081ed6b6b55f4612f021d87b39
		 * aad:
		 * tag: d8847dbc326a066988c77ad3863e6083
		 * result: invalid
		 */
		if (strcmp(k, "id") == 0) {
			if (test->id > 0)
				goto err_dup_key;
			if (!parse_num(v, &test->id)) {
				fprintf(stderr,
				    "E: invalid test id [%s:%d]: %s\n",
				    filename, lineno, v);
				goto err;
			}
			continue;
		} else if (strcmp(k, "comment") == 0) {
			if (test->comment != NULL)
				goto err_dup_key;
			test->comment = strdup(v);
			continue;
		} else if (strcmp(k, "flags") == 0) {
			if (test->flags != NULL)
				goto err_dup_key;
			test->flags = strdup(v);
			continue;
		} else if (strcmp(k, "result") == 0) {
			if (test->result != RS_NONE)
				goto err_dup_key;
			if (strcmp(v, "valid") == 0)
				test->result = RS_VALID;
			else if (strcmp(v, "invalid") == 0)
				test->result = RS_INVALID;
			else {
				fprintf(stderr,
				    "E: unknown test result [%s:%d]: %s\n",
				    filename, lineno, v);
				goto err;
			}
			continue;
		}

		/*
		 * for the test param keys, we set up a pointer to the right
		 * field in the test struct, and then work through that
		 * pointer.
		 */
		crypto_test_val_t *vp = NULL;
		if (strcmp(buf, "iv") == 0)
			vp = &test->iv;
		else if (strcmp(buf, "key") == 0)
			vp = &test->key;
		else if (strcmp(buf, "msg") == 0)
			vp = &test->msg;
		else if (strcmp(buf, "ct") == 0)
			vp = &test->ct;
		else if (strcmp(buf, "aad") == 0)
			vp = &test->aad;
		else if (strcmp(buf, "tag") == 0)
			vp = &test->tag;
		else {
			fprintf(stderr, "E: unknown key [%s:%d]: %s\n",
			    filename, lineno, buf);
			goto err;
		}

		if (vp->val != NULL)
			goto err_dup_key;

		/* sanity; these are hex bytes so must be two chars per byte. */
		size_t vlen = strlen(v);
		if ((vlen & 1) == 1) {
			fprintf(stderr, "E: value length not even "
			    "[%s:%d]: %s\n", filename, lineno, buf);
			goto err;
		}

		/*
		 * zero-length params are allowed, but ICP requires a non-NULL
		 * value pointer, so we give it one and also use that as
		 * a marker for us to know that we've filled this value.
		 */
		if (vlen == 0) {
			vp->val = val_empty;
			continue;
		}

		/*
		 * convert incoming value from hex to raw. allocate space
		 * half as long as the length, then loop the chars and
		 * convert from ascii to 4-bit values, shifting or or-ing
		 * as appropriate.
		 */
		vp->len = vlen/2;
		vp->val = calloc(vp->len, 1);

		for (int i = 0; i < vlen; i++) {
			char c = v[i];
			if (!((c >= '0' && c <= '9') ||
			    (c >= 'a' && c <= 'f'))) {
				fprintf(stderr, "E: invalid hex char "
				    "[%s:%d]: %c\n", filename, lineno, c);
				goto err;
			}

			uint8_t n = ((c <= '9') ? (c-0x30) : (c-0x57)) & 0xf;
			if ((i & 1) == 0)
				vp->val[i/2] = n << 4;
			else
				vp->val[i/2] |= n;
		}
	}

	if (errno != 0) {
		fprintf(stderr, "E: couldn't read %s: %s\n",
		    filepath, strerror(errno));
		goto err;
	}

	free(buf);
	fclose(fh);

	if (tests == NULL)
		fprintf(stderr, "E: no tests in %s\n", filepath);

	*algp = alg;
	return (tests);

/*
 * jump target for duplicate key error. this is so common that it's easier
 * to just have a single error point.
 */
err_dup_key:
	fprintf(stderr, "E: duplicate key [%s:%d]: %s\n", filename, lineno, k);

err:
	if (buf != NULL)
		free(buf);
	if (fh != NULL)
		fclose(fh);

	/*
	 * XXX we should probably free all the tests here, but the test file
	 *     is generated and this is a one-shot program, so it's really
	 *     not worth the effort today
	 */

	return (NULL);
}

/* ========== */

/* ICP algorithm implementation selection */

/*
 * It's currently not really possible to query the ICP for which
 * implementations it supports. Also, not all GCM implementations work
 * with all AES implementations. For now, we keep a hardcoded list of
 * valid combinations.
 */
static const char *aes_impl[] = {
	"generic",
	"x86_64",
	"aesni",
};

static const char *aes_gcm_impl[][2] = {
	{ "generic", "generic" },
	{ "x86_64",  "generic" },
	{ "aesni",   "generic" },
	{ "generic", "pclmulqdq" },
	{ "x86_64",  "pclmulqdq" },
	{ "aesni",   "pclmulqdq" },
	{ "x86_64",  "avx" },
	{ "aesni",   "avx" },
	{ "x86_64",  "avx2-vaes" },
	{ "aesni",   "avx2-vaes" },
	{ "x86_64",  "avx512-vaes" },
	{ "aesni",   "avx512-vaes" },
};

/* signature of function to call after setting implementation params */
typedef void (*alg_cb_t)(const char *alginfo, void *arg);

/* loop over each AES-CCM implementation */
static void
foreach_aes_ccm(alg_cb_t cb, void *arg, crypto_test_outmode_t outmode)
{
	char alginfo[64];

	for (int i = 0; i < ARRAY_SIZE(aes_impl); i++) {
		snprintf(alginfo, sizeof (alginfo), "AES-CCM [%s]",
		    aes_impl[i]);

		int err = -aes_impl_set(aes_impl[i]);
		if (err != 0 && outmode != OUT_SUMMARY)
			printf("W: %s couldn't enable AES impl '%s': %s\n",
			    alginfo, aes_impl[i], strerror(err));

		cb(alginfo, (err == 0) ? arg : NULL);
	}
}

/* loop over each AES-GCM implementation */
static void
foreach_aes_gcm(alg_cb_t cb, void *arg, crypto_test_outmode_t outmode)
{
	char alginfo[64];

	for (int i = 0; i < ARRAY_SIZE(aes_gcm_impl); i++) {
		const char *aes_impl = aes_gcm_impl[i][0];
		const char *gcm_impl = aes_gcm_impl[i][1];

		snprintf(alginfo, sizeof (alginfo), "AES-GCM [%s+%s]",
		    aes_impl, gcm_impl);

		int err = -aes_impl_set(aes_impl);
		if (err != 0 && outmode != OUT_SUMMARY)
			printf("W: %s couldn't enable AES impl '%s': %s\n",
			    alginfo, aes_impl, strerror(err));

		if (err == 0) {
			err = -gcm_impl_set(gcm_impl);
			if (err != 0 && outmode != OUT_SUMMARY) {
				printf("W: %s couldn't enable "
				    "GCM impl '%s': %s\n",
				    alginfo, gcm_impl, strerror(err));
			}
		}

		cb(alginfo, (err == 0) ? arg : NULL);
	}
}

/* ========== */

/* ICP lowlevel drivers */

/*
 * initialise the mechanism (algorithm description) with the wanted parameters
 * for the next operation.
 *
 * mech must be allocated and mech->cm_params point to space large enough
 * to hold the parameters for the given algorithm.
 *
 * decrypt is true if setting up for decryption, false for encryption.
 */
static void
init_mech(crypto_mechanism_t *mech, crypto_test_alg_t alg,
    uint8_t *iv, size_t ivlen,
    uint8_t *aad, size_t aadlen,
    size_t msglen, size_t taglen,
    boolean_t decrypt)
{
	switch (alg) {
	case ALG_AES_GCM: {
		mech->cm_type = crypto_mech2id(SUN_CKM_AES_GCM);
		mech->cm_param_len = sizeof (CK_AES_GCM_PARAMS);
		CK_AES_GCM_PARAMS *p = (CK_AES_GCM_PARAMS *)mech->cm_param;
		p->pIv = (uchar_t *)iv;
		p->ulIvLen = ivlen;
		p->ulIvBits = ivlen << 3;
		p->pAAD = aad;
		p->ulAADLen = aadlen;
		p->ulTagBits = taglen << 3;
		break;
	}
	case ALG_AES_CCM: {
		mech->cm_type = crypto_mech2id(SUN_CKM_AES_CCM);
		mech->cm_param_len = sizeof (CK_AES_CCM_PARAMS);
		CK_AES_CCM_PARAMS *p = (CK_AES_CCM_PARAMS *)mech->cm_param;
		p->nonce = iv;
		p->ulNonceSize = ivlen;
		p->authData = aad;
		p->ulAuthDataSize = aadlen;
		p->ulMACSize = taglen;
		/*
		 * ICP CCM needs the MAC len in the data size for decrypt,
		 * even if the buffer isn't that big.
		 */
		p->ulDataSize = msglen + (decrypt ? taglen : 0);
		break;
	}
	default:
		abort();
	}
}

/*
 * call crypto_encrypt() with the given inputs.
 *
 *        mech: previously initialised by init_mech
 * key, keylen: raw data and length of key
 * msg, msglen: raw data and length of message
 * out, outlen: buffer to write output to (min msglen+taglen)
 *       usecp: if not NULL, recieves microseconds in crypto_encrypt()
 */
static int
encrypt_one(crypto_mechanism_t *mech,
    const uint8_t *key, size_t keylen,
    const uint8_t *msg, size_t msglen,
    uint8_t *out, size_t outlen,
    uint64_t *usecp)
{
	crypto_key_t k = {
		.ck_data = (uint8_t *)key,
		.ck_length = keylen << 3,
	};

	crypto_data_t i = {
		.cd_format = CRYPTO_DATA_RAW,
		.cd_offset = 0,
		.cd_length = msglen,
		.cd_raw = {
			.iov_base = (char *)msg,
			.iov_len = msglen,
		},
	};

	crypto_data_t o = {
		.cd_format = CRYPTO_DATA_RAW,
		.cd_offset = 0,
		.cd_length = outlen,
		.cd_raw = {
			.iov_base = (char *)out,
			.iov_len = outlen,
		},
	};

	struct timeval start, end, diff;
	if (usecp != NULL)
		gettimeofday(&start, NULL);

	int rv = crypto_encrypt(mech, &i, &k, NULL, &o);

	if (usecp != NULL) {
		gettimeofday(&end, NULL);
		timersub(&end, &start, &diff);
		*usecp =
		    ((uint64_t)diff.tv_sec) * 1000000 + (uint64_t)diff.tv_usec;
	}

	return (rv);
}

/*
 * call crypto_decrypt() with the given inputs.
 *
 *        mech: previously initialised by init_mech
 * key, keylen: raw data and length of key
 *   ct, ctlen: raw data and length of ciphertext
 * tag, taglen: raw data and length of tag (MAC)
 * out, outlen: buffer to write output to (min ctlen)
 *       usecp: if not NULL, recieves microseconds in crypto_decrypt()
 */
static int
decrypt_one(crypto_mechanism_t *mech,
    const uint8_t *key, size_t keylen,
    const uint8_t *ct, size_t ctlen,
    const uint8_t *tag, size_t taglen,
    uint8_t *out, size_t outlen,
    uint64_t *usecp)
{
	uint8_t inbuf[1024];

	crypto_key_t k = {
		.ck_data = (uint8_t *)key,
		.ck_length = keylen << 3,
	};

	memcpy(inbuf, ct, ctlen);
	memcpy(inbuf + ctlen, tag, taglen);
	crypto_data_t i = {
		.cd_format = CRYPTO_DATA_RAW,
		.cd_offset = 0,
		.cd_length = ctlen + taglen,
		.cd_raw = {
			.iov_base = (char *)inbuf,
			.iov_len = ctlen + taglen,
		},
	};

	crypto_data_t o = {
		.cd_format = CRYPTO_DATA_RAW,
		.cd_offset = 0,
		.cd_length = outlen,
		.cd_raw = {
			.iov_base = (char *)out,
			.iov_len = outlen
		},
	};

	struct timeval start, end, diff;
	if (usecp != NULL)
		gettimeofday(&start, NULL);

	int rv = crypto_decrypt(mech, &i, &k, NULL, &o);

	if (usecp != NULL) {
		gettimeofday(&end, NULL);
		timersub(&start, &end, &diff);
		*usecp =
		    ((uint64_t)diff.tv_sec) * 1000000 + (uint64_t)diff.tv_usec;
	}

	return (rv);
}

/* ========== */

/* correctness tests */

/*
 * helper; dump the provided data as hex, with a string prefix
 */
static void
hexdump(const char *str, const uint8_t *src, uint_t len)
{
	printf("%12s:", str);
	int i = 0;
	while (i < len) {
		if (i % 4 == 0)
			printf(" ");
		printf("%02x", src[i]);
		i++;
		if (i % 16 == 0 && i < len) {
			printf("\n");
			if (i < len)
				printf("             ");
		}
	}
	printf("\n");
}

/*
 * analyse test result and on failure, print useful output for debugging.
 *
 * test: the test we ran
 * encrypt_rv: return value from crypto_encrypt()
 * encrypt_buf: the output buffer from crypto_encrypt()
 * decrypt_rv: return value from crypto_decrypt()
 * decrypt_buf: the output buffer from crypto_decrypt()
 * outmode: output mode (summary, fail, all)
 */
static boolean_t
test_result(const crypto_test_t *test, int encrypt_rv, uint8_t *encrypt_buf,
    int decrypt_rv, uint8_t *decrypt_buf, crypto_test_outmode_t outmode)
{
	boolean_t ct_match = B_FALSE, tag_match = B_FALSE, msg_match = B_FALSE;
	boolean_t encrypt_pass = B_FALSE, decrypt_pass = B_FALSE;
	boolean_t pass = B_FALSE;

	/* check if the encrypt output matches the expected ciphertext */
	if (memcmp(encrypt_buf, test->ct.val, test->msg.len) == 0)
		ct_match = B_TRUE;

	/*
	 * check if the tag at the end of the encrypt output matches the
	 * expected tag
	 */
	if (memcmp(encrypt_buf + test->msg.len, test->tag.val,
	    test->tag.len) == 0)
		tag_match = B_TRUE;

	/* check if the decrypt output matches the expected plaintext */
	if (memcmp(decrypt_buf, test->msg.val, test->msg.len) == 0)
		msg_match = B_TRUE;

	if (test->result == RS_VALID) {
		/*
		 * a "valid" test is where the params describe an
		 * encrypt/decrypt cycle that should succeed. we consider
		 * these to have passed the test if crypto_encrypt() and
		 * crypto_decrypt() return success, and the output data
		 * matches the expected values from the test params.
		 */
		if (encrypt_rv == CRYPTO_SUCCESS) {
			if (ct_match && tag_match)
				encrypt_pass = B_TRUE;
		}
		if (decrypt_rv == CRYPTO_SUCCESS) {
			if (msg_match)
				decrypt_pass = B_TRUE;
		}
	} else {
		/*
		 * an "invalid" test is where the params describe an
		 * encrypt/decrypt cycle that should _not_ succeed.
		 *
		 * for decrypt, we only need to check the result from
		 * crypto_decrypt(), because decrypt checks the the tag (MAC)
		 * as part of its operation.
		 *
		 * for encrypt, the tag (MAC) is an output of the encryption
		 * function, so if encryption succeeds, we have to check that
		 * the returned tag matches the expected tag.
		 */
		if (encrypt_rv != CRYPTO_SUCCESS || !tag_match)
			encrypt_pass = B_TRUE;
		if (decrypt_rv != CRYPTO_SUCCESS)
			decrypt_pass = B_TRUE;
	}

	/* the test as a whole passed if both encrypt and decrypt passed */
	pass = (encrypt_pass && decrypt_pass);

	/* if the test passed we may not have to output anything */
	if (outmode == OUT_SUMMARY || (outmode == OUT_FAIL && pass))
		return (pass);

	/* print summary of test result */
	printf("%s[%ju]: encrypt=%s decrypt=%s\n", test->fileloc,
	    (uintmax_t)test->id,
	    encrypt_pass ? "PASS" : "FAIL",
	    decrypt_pass ? "PASS" : "FAIL");

	if (!pass) {
		/*
		 * if the test didn't pass, print any comment or flags field
		 * from the test params, which if present can help
		 * understanding what the ICP did wrong
		 */
		if (test->comment != NULL)
			printf("  comment: %s\n", test->comment);
		if (test->flags != NULL)
			printf("  flags: %s\n", test->flags);
	}

	if (!encrypt_pass) {
		/* encrypt failed */

		/* print return value from crypto_encrypt() */
		printf("  encrypt rv = 0x%02x [%s]\n", encrypt_rv,
		    crypto_errstr[encrypt_rv] ?
		    crypto_errstr[encrypt_rv] : "???");

		/* print mismatched ciphertext */
		if (!ct_match) {
			printf("  ciphertexts don't match:\n");
			hexdump("got", encrypt_buf, test->msg.len);
			hexdump("expected", test->ct.val, test->msg.len);
		}

		/* print mistmatched tag (MAC) */
		if (!tag_match) {
			printf("  tags don't match:\n");
			hexdump("got", encrypt_buf + test->msg.len,
			    test->tag.len);
			hexdump("expected", test->tag.val, test->tag.len);
		}
	}

	if (!decrypt_pass) {
		/* decrypt failed */

		/* print return value from crypto_decrypt() */
		printf("  decrypt rv = 0x%02x [%s]\n", decrypt_rv,
		    crypto_errstr[decrypt_rv] ?
		    crypto_errstr[decrypt_rv] : "???");

		/* print mismatched plaintext */
		if (!msg_match) {
			printf("  plaintexts don't match:\n");
			hexdump("got", decrypt_buf, test->msg.len);
			hexdump("expected", test->msg.val, test->msg.len);
		}
	}

	if (!pass)
		printf("\n");

	return (pass);
}

/*
 * run the given list of tests.
 *
 * alginfo: a prefix for the test summary, showing the ICP algo implementation
 *          in use for this run.
 *   tests: first test in test list
 * outmode: output mode, passed to test_result()
 */
static int
run_tests(const char *alginfo, const crypto_test_t *tests,
    crypto_test_outmode_t outmode)
{
	int ntests = 0, npass = 0;

	/*
	 * allocate space for the mechanism description, and alg-specific
	 * params, and hook them up.
	 */
	crypto_mechanism_t mech = {};
	union {
		CK_AES_GCM_PARAMS gcm;
		CK_AES_CCM_PARAMS ccm;
	} params = {};
	mech.cm_param = (caddr_t)&params;

	/* space for encrypt/decrypt output */
	uint8_t encrypt_buf[1024];
	uint8_t decrypt_buf[1024];

	for (const crypto_test_t *test = tests; test != NULL;
	    test = test->next) {
		ntests++;

		/* setup mechanism description for encrypt, then encrypt */
		init_mech(&mech, test->alg, test->iv.val, test->iv.len,
		    test->aad.val, test->aad.len, test->msg.len, test->tag.len,
		    B_FALSE);
		int encrypt_rv = encrypt_one(&mech,
		    test->key.val, test->key.len,
		    test->msg.val, test->msg.len,
		    encrypt_buf, test->msg.len + test->tag.len, NULL);

		/* setup mechanism description for decrypt, then decrypt */
		init_mech(&mech, test->alg, test->iv.val, test->iv.len,
		    test->aad.val, test->aad.len, test->msg.len, test->tag.len,
		    B_TRUE);
		int decrypt_rv = decrypt_one(&mech,
		    test->key.val, test->key.len,
		    test->ct.val, test->ct.len,
		    test->tag.val, test->tag.len,
		    decrypt_buf, test->ct.len, NULL);

		/* consider results and if it passed, count it */
		if (test_result(test, encrypt_rv, encrypt_buf,
		    decrypt_rv, decrypt_buf, outmode))
			npass++;
	}

	printf("%s: tests=%d: passed=%d failed=%d\n",
	    alginfo, ntests, npass, ntests-npass);

	return (ntests != npass);
}

/* args for run_test_alg_cb */
typedef struct {
	crypto_test_t		*tests;
	crypto_test_outmode_t	outmode;
	int			failed;
} run_test_alg_args_t;

/* per-alg-impl function for correctness test runs */
static void
run_test_alg_cb(const char *alginfo, void *arg)
{
	if (arg == NULL) {
		printf("%s: [not supported on this platform]\n", alginfo);
		return;
	}
	run_test_alg_args_t *args = arg;
	args->failed += run_tests(alginfo, args->tests, args->outmode);
}

/* main function for correctness tests */
static int
runtests_main(const char *filename, crypto_test_outmode_t outmode)
{
	crypto_test_alg_t alg = ALG_NONE;
	crypto_test_t *tests = load_tests(filename, &alg);
	if (tests == NULL)
		return (1);

	icp_init();

	run_test_alg_args_t args = {
		.tests = tests,
		.outmode = outmode,
		.failed = 0,
	};

	switch (alg) {
	case ALG_AES_CCM:
		foreach_aes_ccm(run_test_alg_cb, &args, outmode);
		break;
	case ALG_AES_GCM:
		foreach_aes_gcm(run_test_alg_cb, &args, outmode);
		break;
	default:
		abort();
	}

	icp_fini();

	return (args.failed);
}

/* ========== */

/* performance tests */

/* helper; fill the given buffer with random data */
static int
fill_random(uint8_t *v, size_t sz)
{
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return (errno);

	while (sz > 0) {
		ssize_t r = read(fd, v, sz);
		if (r < 0) {
			close(fd);
			return (errno);
		}
		v += r;
		sz -= r;
	}

	close(fd);

	return (0);
}

/* args for perf_alg_cb */
typedef struct {
	crypto_test_alg_t	alg;
	uint8_t			*msg;
	uint8_t			*out;
	uint8_t			key[32];
	uint8_t			iv[12];
} perf_alg_args_t;

#define	PERF_MSG_SHIFT_MIN	(10)	/* min test size 2^10 == 1K */
#define	PERF_MSG_SHIFT_MAX	(19)	/* max test size 2^19 == 512K */
#define	PERF_ROUNDS		(1000)	/* 1000 rounds per test */

/*
 * Bench only: CRYPTO_TEST_NS prints the average in whole nanoseconds instead
 * of zfs_nicetime()'s whole units, and CRYPTO_TEST_IMPL skips every
 * implementation whose name does not contain it. The timing loop is the same.
 */
static boolean_t perf_ns = B_FALSE;
static const char *perf_impl_filter = NULL;

/* per-alg-impl function for performance test runs */
static void
perf_alg_cb(const char *alginfo, void *arg)
{
	char buf[10];

	if (perf_impl_filter != NULL &&
	    strstr(alginfo, perf_impl_filter) == NULL)
		return;
	printf("%-28s", alginfo);

	if (arg == NULL) {
		printf("[not supported on this platform]\n");
		return;
	}

	perf_alg_args_t *args = arg;

	/* space for mechanism description */
	crypto_mechanism_t mech = {};
	union {
		CK_AES_GCM_PARAMS gcm;
		CK_AES_CCM_PARAMS ccm;
	} params = {};
	mech.cm_param = (caddr_t)&params;

	/* loop for each power-2 input size */
	for (int i = PERF_MSG_SHIFT_MIN; i <= PERF_MSG_SHIFT_MAX; i++) {
		/* size of input */
		size_t sz = 1<<i;

		/* initialise mechanism */
		init_mech(&mech, args->alg, args->iv, sizeof (args->iv),
		    val_empty, 0, sz, 16, B_FALSE);

		/* run N rounds and accumulate total time */
		uint64_t total = 0;
		for (int round = 0; round < PERF_ROUNDS; round++) {
			uint64_t usec;
			encrypt_one(&mech, args->key, sizeof (args->key),
			    args->msg, sz, args->out, sz+16, &usec);
			total += usec;
		}

		/*
		 * print avg time per round. zfs_nicetime expects nanoseconds,
		 * so we multiply first
		 */
		if (perf_ns) {
			printf("  %7llu",
			    (unsigned long long)((total*1000)/PERF_ROUNDS));
			continue;
		}
		zfs_nicetime((total*1000)/PERF_ROUNDS, buf, sizeof (buf));
		printf("  %5s", buf);
	}

	printf("\n");
}

/* main function for performance tests */
static int
perf_main(const char *algname, crypto_test_outmode_t outmode)
{
	perf_alg_args_t args;

	if (strcmp(algname, "AES-CCM") == 0)
		args.alg = ALG_AES_CCM;
	else if (strcmp(algname, "AES-GCM") == 0)
		args.alg = ALG_AES_GCM;
	else {
		fprintf(stderr, "E: unknown algorithm: %s\n", algname);
		return (1);
	}

	/*
	 * test runs are often slow, but the very first ones won't be. by
	 * disabling buffering, we can display results immediately, and
	 * the user quickly gets an idea of what to expect
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* allocate random data for encrypt input */
	size_t maxsz = (1<<PERF_MSG_SHIFT_MAX);
	args.msg = malloc(maxsz);
	VERIFY0(fill_random(args.msg, maxsz));

	/* allocate space for output, +16 bytes for tag */
	args.out = malloc(maxsz+16);

	/* fill key and iv */
	VERIFY0(fill_random(args.key, sizeof (args.key)));
	VERIFY0(fill_random(args.iv, sizeof (args.iv)));

	icp_init();

	perf_ns = (getenv("CRYPTO_TEST_NS") != NULL);
	perf_impl_filter = getenv("CRYPTO_TEST_IMPL");

	/* print header */
	char buf[10];
	printf("avg encrypt (%4d rounds)   ", PERF_ROUNDS);
	for (int i = PERF_MSG_SHIFT_MIN; i <= PERF_MSG_SHIFT_MAX; i++) {
		zfs_nicebytes(1<<i, buf, sizeof (buf));
		printf(perf_ns ? "  %7s" : "  %5s", buf);
	}
	if (perf_ns)
		printf("  (ns)");
	printf("\n");

	/* loop over all implementations of the wanted algorithm */
	switch (args.alg) {
	case ALG_AES_CCM:
		foreach_aes_ccm(perf_alg_cb, &args, outmode);
		break;
	case ALG_AES_GCM:
		foreach_aes_gcm(perf_alg_cb, &args, outmode);
		break;
	default:
		abort();
	}

	icp_fini();

	return (0);
}

/* ========== */

/*
 * Prototype self-check (-s, bench only): encrypt random messages with every
 * AES-GCM implementation and compare the ciphertext and tag, byte for byte,
 * with generic+generic. Each message is written through five output layouts:
 * a raw buffer at a non-zero offset, a UIO with one data iovec and a tag
 * iovec (the shape zio_crypt uses), a UIO split exactly like its UIO input
 * (the ZIL and dnode shape), a UIO split at unrelated points, both with
 * pieces of at least one block, and input and output split into pieces of
 * any size, empty ones included. Every output iovec sits in one guarded
 * arena, so a write outside the range the ICP was given is caught too.
 *
 * The last layout is skipped for the generic and pclmulqdq GCM code:
 * crypto_get_ptrs() assumes a block spans at most two iovecs, so a block
 * straddling an iovec shorter than 16 bytes is written wrongly there, with
 * or without the prototype.
 */

#define	SC_MAXLEN	(70000)
#define	SC_MAXOFF	(48)
#define	SC_MAXIOV	(8)
#define	SC_GAP		(16)
#define	SC_GUARD	(0xa5)
#define	SC_ARENA	(SC_MAXOFF + SC_MAXLEN + 16 + (SC_MAXIOV + 3) * SC_GAP)

typedef enum {
	SC_RAW,
	SC_UIO_ONE,
	SC_UIO_SAME,
	SC_UIO_OTHER,
	SC_UIO_TINY,
	SC_NLAYOUTS,
} sc_layout_t;

static const char *sc_layout_name[SC_NLAYOUTS] = {
	"raw+offset", "uio-data+tag", "uio-same-split", "uio-other-split",
	"uio-tiny-split",
};

static uint64_t sc_rng_state;

static uint64_t
sc_rand(void)
{
	/* xorshift64*, reproducible from the printed seed */
	sc_rng_state ^= sc_rng_state >> 12;
	sc_rng_state ^= sc_rng_state << 25;
	sc_rng_state ^= sc_rng_state >> 27;
	return (sc_rng_state * 0x2545F4914F6CDD1DULL);
}

static void
sc_fill(uint8_t *p, size_t len)
{
	for (size_t i = 0; i < len; i++)
		p[i] = (uint8_t)(sc_rand() >> 56);
}

/*
 * split len into up to maxn pieces at random points, each at least minp
 * bytes long (minp == 0 allows empty pieces)
 */
static int
sc_split(size_t len, size_t *pieces, int maxn, size_t minp)
{
	if (minp > 0 && len / minp < (size_t)maxn)
		maxn = MAX(1, (int)(len / minp));
	int n = 1 + (int)(sc_rand() % maxn);
	if (n == 1) {
		pieces[0] = len;
		return (1);
	}
	size_t spare = len - n * minp;
	size_t cuts[SC_MAXIOV + 2];

	cuts[0] = 0;
	for (int i = 1; i < n; i++)
		cuts[i] = sc_rand() % (spare + 1);
	cuts[n] = spare;
	/* insertion sort, n is tiny */
	for (int i = 2; i < n; i++) {
		size_t v = cuts[i];
		int j = i - 1;
		while (j >= 1 && cuts[j] > v) {
			cuts[j + 1] = cuts[j];
			j--;
		}
		cuts[j + 1] = v;
	}
	for (int i = 0; i < n; i++)
		pieces[i] = cuts[i + 1] - cuts[i] + minp;
	return (n);
}

/* lay out iovecs of the given lengths in the arena, SC_GAP apart */
static void
sc_place(uint8_t *arena, iovec_t *iov, const size_t *lens, int n)
{
	size_t pos = SC_GAP;

	for (int i = 0; i < n; i++) {
		iov[i].iov_base = arena + pos;
		iov[i].iov_len = lens[i];
		pos += lens[i] + SC_GAP;
	}
	VERIFY3U(pos, <=, SC_ARENA);
}

/* copy len bytes of src into the iovecs, starting off bytes in */
static void
sc_scatter(const iovec_t *iov, int n, size_t off, const uint8_t *src,
    size_t len)
{
	for (int i = 0; i < n && len > 0; i++) {
		if (off >= iov[i].iov_len) {
			off -= iov[i].iov_len;
			continue;
		}
		size_t c = MIN(iov[i].iov_len - off, len);
		memcpy((uint8_t *)iov[i].iov_base + off, src, c);
		src += c;
		len -= c;
		off = 0;
	}
	VERIFY0(len);
}

typedef struct {
	size_t		msglen;
	const uint8_t	*msg;
	const uint8_t	*key;
	size_t		keylen;
	const uint8_t	*iv;
	const uint8_t	*aad;
	size_t		aadlen;
	const uint8_t	*ref;		/* generic+generic ciphertext and tag */
	uint8_t		*arena;
	uint8_t		*expect;
} sc_case_t;

/* run one layout through the currently selected implementation */
static boolean_t
sc_run_layout(const sc_case_t *c, sc_layout_t layout, int *rvp)
{
	size_t len = c->msglen;
	size_t outlen = len + 16;
	size_t off = 1 + sc_rand() % (SC_MAXOFF - 1);
	size_t inlens[SC_MAXIOV], outlens[SC_MAXIOV + 1];
	iovec_t iniov[SC_MAXIOV], outiov[SC_MAXIOV + 1];
	int nin = 1, nout;
	zfs_uio_t inuio = { 0 }, outuio = { 0 };
	crypto_data_t i = { 0 }, o = { 0 };

	inlens[0] = len;
	switch (layout) {
	case SC_RAW:
		outlens[0] = off + outlen;
		nout = 1;
		break;
	case SC_UIO_ONE:
		outlens[0] = off + len;
		outlens[1] = 16;
		nout = 2;
		break;
	case SC_UIO_SAME:
		nin = sc_split(len, inlens, SC_MAXIOV, 16);
		memcpy(outlens, inlens, nin * sizeof (size_t));
		outlens[0] += off;
		outlens[nin] = 16;
		nout = nin + 1;
		break;
	case SC_UIO_OTHER:
		nin = sc_split(len, inlens, SC_MAXIOV, 16);
		nout = sc_split(outlen, outlens, SC_MAXIOV + 1, 16);
		outlens[0] += off;
		break;
	case SC_UIO_TINY:
		nin = sc_split(len, inlens, SC_MAXIOV, 0);
		nout = sc_split(outlen, outlens, SC_MAXIOV + 1, 0);
		outlens[0] += off;
		break;
	default:
		abort();
	}

	/* input iovecs point into the message itself */
	size_t pos = 0;
	for (int k = 0; k < nin; k++) {
		iniov[k].iov_base = (len == 0) ? (void *)val_empty :
		    (void *)(c->msg + pos);
		iniov[k].iov_len = inlens[k];
		pos += inlens[k];
	}

	memset(c->arena, SC_GUARD, SC_ARENA);
	memset(c->expect, SC_GUARD, SC_ARENA);
	sc_place(c->arena, outiov, outlens, nout);
	sc_place(c->expect, outiov, outlens, nout);
	sc_scatter(outiov, nout, off, c->ref, outlen);
	/* point the output iovecs back at the arena */
	sc_place(c->arena, outiov, outlens, nout);

	if (layout == SC_RAW) {
		i.cd_format = CRYPTO_DATA_RAW;
		i.cd_raw = iniov[0];
		o.cd_format = CRYPTO_DATA_RAW;
		o.cd_raw = outiov[0];
	} else {
		inuio.uio_iov = iniov;
		inuio.uio_iovcnt = nin;
		inuio.uio_segflg = UIO_SYSSPACE;
		outuio.uio_iov = outiov;
		outuio.uio_iovcnt = nout;
		outuio.uio_segflg = UIO_SYSSPACE;
		i.cd_format = CRYPTO_DATA_UIO;
		i.cd_uio = &inuio;
		o.cd_format = CRYPTO_DATA_UIO;
		o.cd_uio = &outuio;
	}
	i.cd_offset = 0;
	i.cd_length = len;
	o.cd_offset = off;
	o.cd_length = outlen;

	crypto_mechanism_t mech = { 0 };
	CK_AES_GCM_PARAMS params = { 0 };
	mech.cm_param = (caddr_t)&params;
	init_mech(&mech, ALG_AES_GCM, (uint8_t *)c->iv, 12,
	    (uint8_t *)c->aad, c->aadlen, len, 16, B_FALSE);

	crypto_key_t k = {
		.ck_data = (uint8_t *)c->key,
		.ck_length = c->keylen << 3,
	};

	*rvp = crypto_encrypt(&mech, &i, &k, NULL, &o);
	return (*rvp == CRYPTO_SUCCESS && o.cd_length == outlen &&
	    memcmp(c->arena, c->expect, SC_ARENA) == 0);
}

static int
selfcheck_main(crypto_test_outmode_t outmode)
{
	uint64_t seed;
	int fd = open("/dev/urandom", O_RDONLY);
	VERIFY3S(fd, >=, 0);
	VERIFY3S(read(fd, &seed, sizeof (seed)), ==, sizeof (seed));
	close(fd);
	const char *env = getenv("CRYPTO_TEST_SEED");
	if (env != NULL)
		seed = strtoull(env, NULL, 0);
	sc_rng_state = seed | 1;
	printf("self-check seed 0x%llx\n", (unsigned long long)seed);

	uint8_t *msg = malloc(SC_MAXLEN);
	uint8_t *ref = malloc(SC_MAXLEN + 16);
	uint8_t key[32], iv[12], aad[64];
	sc_case_t c = {
		.msg = msg,
		.key = key,
		.iv = iv,
		.aad = aad,
		.ref = ref,
		.arena = malloc(SC_ARENA),
		.expect = malloc(SC_ARENA),
	};
	uint64_t cases[ARRAY_SIZE(aes_gcm_impl)] = { 0 };
	uint64_t fails[ARRAY_SIZE(aes_gcm_impl)] = { 0 };
	boolean_t usable[ARRAY_SIZE(aes_gcm_impl)];

	icp_init();

	for (int m = 0; m < ARRAY_SIZE(aes_gcm_impl); m++) {
		usable[m] = (aes_impl_set(aes_gcm_impl[m][0]) == 0 &&
		    gcm_impl_set(aes_gcm_impl[m][1]) == 0);
	}

	/*
	 * Every length up to 1100, then every 7th (so all residues mod 16 and
	 * mod 96 are hit), plus every length within 300 bytes of a multiple of
	 * the default 32736-byte chunk.
	 */
	uint64_t nlen = 0;
	for (size_t len = 0; len <= SC_MAXLEN; len++) {
		size_t r = len % 32736;
		if (len > 1100 && len % 7 != 0 && r > 300 && r < 32736 - 300)
			continue;
		nlen++;

		c.msglen = len;
		c.keylen = 16 + 8 * (len % 3);
		c.aadlen = sc_rand() % 40;
		sc_fill(msg, len);
		sc_fill(key, sizeof (key));
		sc_fill(iv, sizeof (iv));
		sc_fill(aad, sizeof (aad));

		/* reference: generic+generic, raw, offset 0 */
		VERIFY0(aes_impl_set("generic"));
		VERIFY0(gcm_impl_set("generic"));
		crypto_mechanism_t mech = { 0 };
		CK_AES_GCM_PARAMS params = { 0 };
		mech.cm_param = (caddr_t)&params;
		init_mech(&mech, ALG_AES_GCM, iv, sizeof (iv), aad, c.aadlen,
		    len, 16, B_FALSE);
		VERIFY0(encrypt_one(&mech, key, c.keylen,
		    len == 0 ? val_empty : msg, len, ref, len + 16, NULL));

		for (int m = 0; m < ARRAY_SIZE(aes_gcm_impl); m++) {
			if (!usable[m])
				continue;
			VERIFY0(aes_impl_set(aes_gcm_impl[m][0]));
			VERIFY0(gcm_impl_set(aes_gcm_impl[m][1]));
			boolean_t generic_gcm =
			    strcmp(aes_gcm_impl[m][1], "generic") == 0 ||
			    strcmp(aes_gcm_impl[m][1], "pclmulqdq") == 0;
			for (int l = 0; l < SC_NLAYOUTS; l++) {
				int rv;
				if (l == SC_UIO_TINY && generic_gcm)
					continue;
				cases[m]++;
				if (sc_run_layout(&c, l, &rv))
					continue;
				fails[m]++;
				if (outmode != OUT_SUMMARY || fails[m] <= 5) {
					printf("FAIL: AES-GCM [%s+%s] len %zu "
					    "key %zu aad %zu layout %s rv %d\n",
					    aes_gcm_impl[m][0],
					    aes_gcm_impl[m][1], len,
					    c.keylen, c.aadlen,
					    sc_layout_name[l], rv);
				}
			}
		}
	}

	int failed = 0;
	printf("self-check: %llu lengths in [0, %d], %d layouts each "
	    "(%d for generic/pclmulqdq GCM)\n", (unsigned long long)nlen,
	    SC_MAXLEN, SC_NLAYOUTS, SC_NLAYOUTS - 1);
	for (int m = 0; m < ARRAY_SIZE(aes_gcm_impl); m++) {
		char alginfo[64];
		snprintf(alginfo, sizeof (alginfo), "AES-GCM [%s+%s]",
		    aes_gcm_impl[m][0], aes_gcm_impl[m][1]);
		if (!usable[m]) {
			printf("%-28s [not supported on this platform]\n",
			    alginfo);
			continue;
		}
		printf("%-28s cases %llu  failed %llu\n", alginfo,
		    (unsigned long long)cases[m], (unsigned long long)fails[m]);
		failed += (fails[m] != 0);
	}

	icp_fini();

	return (failed);
}

/* ========== */

/* main entry */

static void
usage(void)
{
	fprintf(stderr,
	    "usage: crypto_test [-v] < -c <testfile> | -p <alg> | -s >\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	crypto_test_outmode_t outmode = OUT_SUMMARY;
	const char *filename = NULL;
	const char *algname = NULL;
	boolean_t selfcheck = B_FALSE;

	int c;
	while ((c = getopt(argc, argv, "c:p:sv")) != -1) {
		switch (c) {
		case 'c':
			filename = optarg;
			break;
		case 'p':
			algname = optarg;
			break;
		case 's':
			selfcheck = B_TRUE;
			break;
		case 'v':
			outmode = (outmode == OUT_SUMMARY) ? OUT_FAIL : OUT_ALL;
			break;
		case '?':
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (filename != NULL && algname != NULL) {
		fprintf(stderr, "E: can't use -c and -p together\n");
		usage();
	}

	if (argc != 0)
		usage();

	if (selfcheck)
		return (selfcheck_main(outmode));

	if (filename)
		return (runtests_main(filename, outmode));

	return (perf_main(algname, outmode));
}
