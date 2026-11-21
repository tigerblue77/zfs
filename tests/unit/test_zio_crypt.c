// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

#include <sys/zfs_context.h>
#include <sys/crypto/icp.h>
#include <sys/dnode.h>
#include <sys/mman.h>
#include <sys/zil.h>
#include <sys/zio_crypt.h>
#include <libspl.h>

#include "unit.h"

/* ========== */

/*
 * zio_do_crypt_data() walks ZIL and dnode blocks to find the parts to
 * encrypt and the parts to authenticate.  On decryption, the lengths it
 * follows have not been authenticated yet.  Each test encrypts a valid
 * block, changes fields that are stored in plaintext so that the layout no
 * longer fits the block, and expects ECKSUM from the decryption.  Every
 * buffer ends where an inaccessible page begins, so that a read or a write
 * past the block faults even without ASAN.
 */

#define	ZIL_BLKSZ	4096
/* The last record is shorter than a TX_WRITE or TX_CLONE_RANGE header. */
#define	ZIL_LAST_RECLEN	48
#define	ZIL_NRECORDS	6

typedef struct {
	zio_crypt_key_t		cb_key;
	dmu_object_type_t	cb_ot;
	uint_t			cb_len;
	uint8_t			*cb_plain;
	uint8_t			*cb_cipher;
	uint8_t			*cb_out;
	uint8_t			cb_salt[ZIO_DATA_SALT_LEN];
	uint8_t			cb_iv[ZIO_DATA_IV_LEN];
	uint8_t			cb_mac[ZIO_DATA_MAC_LEN];
	uint_t			cb_rec[ZIL_NRECORDS];	/* ZIL record offsets */
} crypt_block_t;

static size_t
guarded_maplen(size_t len)
{
	size_t pgsz = sysconf(_SC_PAGESIZE);

	return (P2ROUNDUP(len, pgsz) + pgsz);
}

/* A buffer of len bytes, followed by an inaccessible page. */
static uint8_t *
guarded_alloc(size_t len)
{
	size_t pgsz = sysconf(_SC_PAGESIZE);
	size_t maplen = guarded_maplen(len);
	uint8_t *map = mmap(NULL, maplen, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	munit_assert_ptr_not_equal(map, MAP_FAILED);
	unit_ok(mprotect(map + maplen - pgsz, pgsz, PROT_NONE));
	return (map + maplen - pgsz - len);
}

static void
guarded_free(uint8_t *buf, size_t len)
{
	size_t pgsz = sysconf(_SC_PAGESIZE);
	size_t maplen = guarded_maplen(len);

	unit_ok(munmap(buf + len + pgsz - maplen, maplen));
}

static void
block_init(crypt_block_t *cb, dmu_object_type_t ot, uint_t len)
{
	memset(cb, 0, sizeof (*cb));
	unit_ok(zio_crypt_key_init(ZIO_CRYPT_AES_256_GCM, &cb->cb_key));
	cb->cb_ot = ot;
	cb->cb_len = len;
	cb->cb_plain = guarded_alloc(len);
	cb->cb_cipher = guarded_alloc(len);
	cb->cb_out = guarded_alloc(len);
}

static void
block_fini(crypt_block_t *cb)
{
	guarded_free(cb->cb_plain, cb->cb_len);
	guarded_free(cb->cb_cipher, cb->cb_len);
	guarded_free(cb->cb_out, cb->cb_len);
	zio_crypt_key_destroy(&cb->cb_key);
}

static void
block_encrypt(crypt_block_t *cb)
{
	boolean_t no_crypt = B_TRUE;

	unit_ok(zio_crypt_key_get_salt(&cb->cb_key, cb->cb_salt));
	unit_ok(zio_crypt_generate_iv(cb->cb_iv));
	unit_ok(zio_do_crypt_data(B_TRUE, &cb->cb_key, cb->cb_ot, B_FALSE,
	    cb->cb_salt, cb->cb_iv, cb->cb_mac, cb->cb_len, cb->cb_plain,
	    cb->cb_cipher, &no_crypt));
	unit_false(no_crypt);
}

static int
block_decrypt(crypt_block_t *cb)
{
	boolean_t no_crypt = B_TRUE;

	return (zio_do_crypt_data(B_FALSE, &cb->cb_key, cb->cb_ot, B_FALSE,
	    cb->cb_salt, cb->cb_iv, cb->cb_mac, cb->cb_len, cb->cb_out,
	    cb->cb_cipher, &no_crypt));
}

static void
fill_pattern(uint8_t *p, size_t len, uint64_t seed)
{
	for (size_t i = 0; i < len; i++)
		p[i] = (uint8_t)(seed * 131 + i * 7);
}

/* ========== */

/* ZIL blocks */

static lr_t *
zil_add_record(crypt_block_t *cb, uint_t *off, uint_t n, uint64_t txtype,
    uint64_t reclen)
{
	lr_t *lr = (lr_t *)(cb->cb_plain + *off);

	fill_pattern((uint8_t *)lr, reclen, n);
	lr->lrc_txtype = txtype;
	lr->lrc_reclen = reclen;
	lr->lrc_txg = 4;
	lr->lrc_seq = n + 1;
	cb->cb_rec[n] = *off;
	*off += reclen;

	return (lr);
}

/*
 * A log block: a TX_CREATE, a TX_WRITE with data, a TX_WRITE without, and a
 * TX_CLONE_RANGE with one block pointer.  If full, a TX_SETATTR then fills
 * the space up to the last record: a TX_REMOVE of ZIL_LAST_RECLEN bytes
 * that ends the block.
 */
static void
zil_block_build(crypt_block_t *cb, boolean_t full)
{
	zil_chain_t *zilc = (zil_chain_t *)cb->cb_plain;
	uint_t off = sizeof (zil_chain_t);
	lr_clone_range_t *lrc;

	memset(cb->cb_plain, 0, cb->cb_len);
	(void) zil_add_record(cb, &off, 0, TX_CREATE, 200);
	(void) zil_add_record(cb, &off, 1, TX_WRITE,
	    sizeof (lr_write_t) + 64);
	(void) zil_add_record(cb, &off, 2, TX_WRITE, sizeof (lr_write_t));
	lrc = (lr_clone_range_t *)zil_add_record(cb, &off, 3, TX_CLONE_RANGE,
	    sizeof (lr_clone_range_t) + sizeof (blkptr_t));
	lrc->lr_nbps = 1;
	if (full) {
		(void) zil_add_record(cb, &off, 4, TX_SETATTR,
		    cb->cb_len - off - ZIL_LAST_RECLEN);
		(void) zil_add_record(cb, &off, 5, TX_REMOVE, ZIL_LAST_RECLEN);
		unit_eq(off, cb->cb_len);
	}

	zilc->zc_nused = off;
	zilc->zc_eck.zec_magic = ZEC_MAGIC;
}

static lr_t *
zil_record(crypt_block_t *cb, uint_t n)
{
	return ((lr_t *)(cb->cb_cipher + cb->cb_rec[n]));
}

static MunitResult
test_zil_valid(const MunitParameter params[], void *data)
{
	(void) data;
	boolean_t full = strcmp(munit_parameters_get(params, "nused"),
	    "full") == 0;
	crypt_block_t cb;

	block_init(&cb, DMU_OT_INTENT_LOG, ZIL_BLKSZ);
	zil_block_build(&cb, full);
	block_encrypt(&cb);

	unit_ok(block_decrypt(&cb));
	unit_ok(memcmp(cb.cb_out, cb.cb_plain, cb.cb_len));

	block_fini(&cb);
	return (MUNIT_OK);
}

static MunitResult
test_zil_malformed(const MunitParameter params[], void *data)
{
	(void) data;
	const char *layout = munit_parameters_get(params, "layout");
	crypt_block_t cb;
	zil_chain_t *zilc;
	lr_t *last;

	/* Without ASSERTs, a record length of 0 used to loop forever. */
	(void) alarm(30);

	block_init(&cb, DMU_OT_INTENT_LOG, ZIL_BLKSZ);
	zil_block_build(&cb, B_TRUE);
	block_encrypt(&cb);

	zilc = (zil_chain_t *)cb.cb_cipher;
	last = zil_record(&cb, ZIL_NRECORDS - 1);
	if (strcmp(layout, "nused_below_header") == 0) {
		zilc->zc_nused = sizeof (zil_chain_t) - sizeof (uint64_t);
	} else if (strcmp(layout, "nused_past_block") == 0) {
		zilc->zc_nused = cb.cb_len + sizeof (lr_t);
	} else if (strcmp(layout, "header_past_nused") == 0) {
		zilc->zc_nused = cb.cb_rec[ZIL_NRECORDS - 1] +
		    sizeof (lr_t) / 2;
	} else if (strcmp(layout, "reclen_zero") == 0) {
		zil_record(&cb, 1)->lrc_reclen = 0;
	} else if (strcmp(layout, "reclen_past_nused") == 0) {
		last->lrc_reclen = ZIL_LAST_RECLEN + 64;
	} else if (strcmp(layout, "write_short") == 0) {
		last->lrc_txtype = TX_WRITE;
	} else if (strcmp(layout, "clone_range_short") == 0) {
		last->lrc_txtype = TX_CLONE_RANGE;
	} else {
		unit_unreachable();
	}

	unit_err(block_decrypt(&cb), ECKSUM);

	block_fini(&cb);
	return (MUNIT_OK);
}

/* ========== */

/* dnode blocks */

static void
dnode_set(dnode_phys_t *dnp, uint_t n, dmu_object_type_t bonustype,
    uint8_t nblkptr, uint8_t extra_slots, uint16_t bonuslen, uint8_t flags)
{
	dnp += n;
	dnp->dn_type = DMU_OT_PLAIN_FILE_CONTENTS;
	dnp->dn_indblkshift = 17;
	dnp->dn_nlevels = 1;
	dnp->dn_nblkptr = nblkptr;
	dnp->dn_bonustype = bonustype;
	dnp->dn_checksum = ZIO_CHECKSUM_INHERIT;
	dnp->dn_compress = ZIO_COMPRESS_INHERIT;
	dnp->dn_flags = flags;
	dnp->dn_datablkszsec = 1;
	dnp->dn_bonuslen = bonuslen;
	dnp->dn_extra_slots = extra_slots;
	fill_pattern(DN_BONUS(dnp), bonuslen, n);
}

/*
 * Encrypted and plaintext bonus buffers, a spill block pointer, a dnode
 * with two slots and one with three block pointers, followed by free
 * dnodes up to the end of the block.  The bonus types are new-style, so
 * that DMU_OT_IS_ENCRYPTED() does not look them up in dmu_ot[], which the
 * mock fills in only for the ZIL and dnode block types.
 */
static void
dnode_block_build(crypt_block_t *cb)
{
	dnode_phys_t *dnp = (dnode_phys_t *)cb->cb_plain;

	memset(cb->cb_plain, 0, cb->cb_len);
	dnode_set(dnp, 1, DMU_OTN_UINT8_ENC_DATA, 1, 0, 160, 0);
	dnode_set(dnp, 2, DMU_OTN_UINT8_ENC_DATA, 1, 0, 192,
	    DNODE_FLAG_SPILL_BLKPTR);
	dnode_set(dnp, 3, DMU_OTN_UINT8_ENC_DATA, 1, 1, 600, 0);
	dnode_set(dnp, 5, DMU_OTN_UINT8_DATA, DN_MAX_NBLKPTR, 0, 64, 0);
}

static MunitResult
test_dnode_valid(const MunitParameter params[], void *data)
{
	(void) params, (void) data;
	crypt_block_t cb;

	block_init(&cb, DMU_OT_DNODE, DNODE_BLOCK_SIZE);
	dnode_block_build(&cb);
	block_encrypt(&cb);

	unit_ok(block_decrypt(&cb));
	unit_ok(memcmp(cb.cb_out, cb.cb_plain, cb.cb_len));

	block_fini(&cb);
	return (MUNIT_OK);
}

static MunitResult
test_dnode_malformed(const MunitParameter params[], void *data)
{
	(void) data;
	const char *layout = munit_parameters_get(params, "layout");
	crypt_block_t cb;
	dnode_phys_t *last;

	block_init(&cb, DMU_OT_DNODE, DNODE_BLOCK_SIZE);
	dnode_block_build(&cb);
	block_encrypt(&cb);

	last = (dnode_phys_t *)(cb.cb_cipher + cb.cb_len) - 1;
	if (strcmp(layout, "extra_slots_past_block") == 0) {
		last->dn_extra_slots = 1;
	} else if (strcmp(layout, "nblkptr_too_large") == 0) {
		last->dn_nblkptr = DN_MAX_NBLKPTR + 1;
	} else if (strcmp(layout, "spill_over_blkptrs") == 0) {
		last->dn_nblkptr = DN_MAX_NBLKPTR;
		last->dn_flags |= DNODE_FLAG_SPILL_BLKPTR;
	} else {
		unit_unreachable();
	}

	unit_err(block_decrypt(&cb), ECKSUM);

	block_fini(&cb);
	return (MUNIT_OK);
}

/* ========== */

/*
 * zio_buf_*() on top of kmem, and stubs for the abd calls of zio_crypt.c,
 * which the tests do not reach: they pass linear buffers.
 */

void *
zio_buf_alloc(size_t size)
{
	return (kmem_alloc(size, KM_SLEEP));
}

void
zio_buf_free(void *buf, size_t size)
{
	kmem_free(buf, size);
}

void *
abd_borrow_buf(abd_t *abd, size_t n)
{
	(void) abd, (void) n;
	unit_unreachable();
}

void *
abd_borrow_buf_copy(abd_t *abd, size_t n)
{
	(void) abd, (void) n;
	unit_unreachable();
}

void
abd_return_buf(abd_t *abd, void *buf, size_t n)
{
	(void) abd, (void) buf, (void) n;
	unit_unreachable();
}

void
abd_return_buf_copy(abd_t *abd, void *buf, size_t n)
{
	(void) abd, (void) buf, (void) n;
	unit_unreachable();
}

/* ========== */

static const MunitParameterEnum zil_nused_params[] = {
	UNIT_PARAM("nused", "full", "partial"),
	{ NULL, NULL },
};

static const MunitParameterEnum zil_layout_params[] = {
	UNIT_PARAM("layout", "nused_below_header", "nused_past_block",
	    "header_past_nused", "reclen_zero", "reclen_past_nused",
	    "write_short", "clone_range_short"),
	{ NULL, NULL },
};

static const MunitParameterEnum dnode_layout_params[] = {
	UNIT_PARAM("layout", "extra_slots_past_block", "nblkptr_too_large",
	    "spill_over_blkptrs"),
	{ NULL, NULL },
};

static const MunitTest zio_crypt_tests[] = {
	UNIT_TEST("zil_valid", test_zil_valid, zil_nused_params),
	UNIT_TEST("zil_malformed", test_zil_malformed, zil_layout_params),
	UNIT_TEST("dnode_valid", test_dnode_valid),
	UNIT_TEST("dnode_malformed", test_dnode_malformed,
	    dnode_layout_params),
	{ 0 },
};

static const MunitSuite zio_crypt_test_suite = {
	"zio_crypt.",
	zio_crypt_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE,
};

int
main(int argc, char **argv)
{
	libspl_init();
	VERIFY0(icp_init());

	int rc = munit_suite_main(&zio_crypt_test_suite, NULL, argc, argv);

	icp_fini();
	libspl_fini();

	return (rc);
}
