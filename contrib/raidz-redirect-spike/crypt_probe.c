// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 3A (throwaway, not part of any tree): can the move of
 * old-epoch raidz blocks reach every block of a pool, those of encrypted
 * datasets whose key is not loaded, of encrypted snapshots, of raw-received
 * datasets, dedup, gang and multi-copy blocks included, and does each one
 * read back correct at the new parity afterwards?
 *
 *   crypt_probe <dir> <ashift> [nodva] [noroot] [noraw] [nomos] [keep]
 *
 * Phase 0 builds a five-wide raidz1 pool of files in <dir>: plain, dedup,
 * gang, embedded and copies=2 objects in unencrypted datasets; plain,
 * dedup, gang and copies=2 objects in an encrypted dataset (aes-256-gcm,
 * a raw key), two snapshots of it with blocks that only a snapshot holds,
 * an encrypted clone, and a raw full and incremental receive of its
 * snapshots into a second encryption root; then a parity-2 epoch and a
 * control written after it.  The pool is exported, which unloads every
 * key.
 *
 * Every later phase is a child process that imports the pool and loads no
 * key, except the read checks, which load the keys themselves:
 *
 *   census  traverses the whole pool (MOS, every head, snapshot and clone,
 *           TRAVERSE_NO_DECRYPT) and counts each distinct DVA on the raidz
 *           by class: born in the new epoch, redirected, or still at the
 *           old geometry (the residual); and the old DVAs of pending frees
 *           (the deferred bpobj, the free bpobj), which no traversal sees;
 *   oracle  copies the pool's files, inverts every allocated byte of
 *           children 1 and 3 past their front labels, imports the copy
 *           read-only, loads the keys and reads every expected object of
 *           every dataset and snapshot, then runs zdb -bcc on the copy.  It
 *           proves only that one copy of each block survives: a small
 *           block may have no column on either child;
 *   dva     the same for each of the ten pairs of children, reading every
 *   oracle  DVA alone, raw, against its checksum.  A parity-1 extent has
 *           columns on two children at least, so some pair destroys it,
 *           and a parity-2 extent survives all ten: this counts the
 *           residual without trusting the redirect table;
 *   mos     rewrites the old MOS blocks through the normal write path, by
 *           dirtying them in a sync task (the redirect table lives in the
 *           MOS and cannot redirect it), and repeats until none is left;
 *   move    moves every old DVA outside the MOS with the kernel's
 *           test-only move, keeping the first sector of each old extent
 *           (P2), after checking that every encrypted dataset's key is
 *           unavailable.
 *
 * The run: census, oracle and dva oracle (before), mos, census, oracle
 * (MOS only), move, census, read checks and zdb -bcc on the pool itself, a
 * scrub, census, oracle and dva oracle (after).  'nodva' moves only DVA 0
 * of each block, 'noroot' sets spike_redirect_live_objset to 0, so that
 * the move's liveness check refuses the objsets' root blocks and their
 * meta-dnodes' blocks as it did before this step, 'noraw' has it find a
 * block below an indirect one with dbuf_dnode_findbp() again, which
 * decrypts the dnode block and so fails without the key, and 'nomos'
 * skips the MOS phase: each shows what a residual does.  'keep' keeps the
 * object oracles' copies.
 *
 * Prints one PASS or FAIL line per check, INFO lines with the counts, and
 * exits 1 if any check failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/bpobj.h>
#include <sys/dmu_send.h>
#include <sys/dmu_recv.h>
#include <sys/dnode.h>
#include <sys/dbuf.h>
#include <sys/dsl_crypt.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_file.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"cyp"
#define	WIDTH		5
#define	FILE_SIZE	(512ULL << 20)
#define	BLKSZ		(128ULL << 10)
#define	OBJSZ		(1ULL << 20)
#define	PATH_BUF	1024
#define	MAXEXP		256
#define	MAXLIVE		64
#define	BATCH		128
#define	DSNAME		64
/* Bytes of each child that the oracles leave alone: its front labels. */
#define	FRONT		(4ULL << 20)
#define	DVA_BITS	14
#define	NPAIRS		(WIDTH * (WIDTH - 1) / 2)

static const char *const wkeydata = "abcdefghijklmnopqrstuvwxyz012345";

extern uint64_t metaslab_force_ganging;
extern uint_t metaslab_force_ganging_pct;

/* One object that a dataset or snapshot must hold, and its content. */
typedef struct expect {
	char		e_ds[DSNAME];
	uint64_t	e_obj;
	uint64_t	e_size;
	uint64_t	e_seed;		/* 0: every byte is e_byte */
	uint8_t		e_byte;
} expect_t;

/* The counts of a census, by class and state. */
enum { ST_NEW, ST_REDIR, ST_OLD, NST };
enum {
	C_ALL, C_MOS, C_DATASET, C_ENC_DATA, C_ENC_AUTH, C_ENC_MAC, C_CLEAR,
	C_ROOT, C_METADNODE, C_INDIRECT, C_L0, C_DVA0, C_DVA1, C_DVA2,
	C_GANG_HDR, C_GANG_CHILD, C_DEDUP, C_SNAP, C_RECV, C_CLONE, C_ZIL,
	NCLASS
};
static const char *const class_names[] = {
	"all DVAs", "MOS", "outside the MOS", "encrypted data (L0)",
	"authenticated, not encrypted (L0)", "indirect, with MAC",
	"no encryption", "objset root block", "meta-dnode block",
	"indirect block", "level-0 block", "DVA 0", "DVA 1", "DVA 2",
	"gang header", "gang child", "dedup block", "held by a snapshot",
	"raw-received dataset", "clone", "ZIL",
};

typedef struct census {
	uint64_t	c_n[NCLASS][NST];
	uint64_t	c_embedded;	/* block pointers with no DVA */
	uint64_t	c_bps;		/* block pointers visited */
	int		c_err;		/* traverse_pool()'s */
} census_t;

typedef struct shared {
	uint64_t	sh_epoch_txg;
	int		sh_nexp;
	expect_t	sh_exp[MAXEXP];
	census_t	sh_census;
	uint64_t	sh_pending_old;	/* old DVAs of pending frees */
	int64_t		sh_space_gap;	/* deflated vdev alloc - DSL used */
	int64_t		sh_gap_before;
	/* The DVA oracle: each DVA, and the damaged pairs it failed under. */
	struct {
		uint64_t	dv_offset;	/* + 1, 0: empty */
		uint64_t	dv_birth;
		/* The pairs whose traversal met it, and those it failed in. */
		uint16_t	dv_seen;
		uint16_t	dv_fail;
		uint8_t		dv_mos;
		zbookmark_phys_t dv_zb;
		uint8_t		dv_d, dv_gang, dv_crypt, dv_dedup, dv_ndvas;
		int		dv_err;
	} sh_dva[1 << DVA_BITS];
	/* Each gang header copy's bytes, by SHA-256, from the whole pool. */
	struct {
		uint64_t	gr_offset;
		uint64_t	gr_birth;
		zio_cksum_t	gr_cksum;
	} sh_gref[256];
	int		sh_ngref;
	int		sh_ngref_bad;

	/* What the move phase did. */
	uint64_t	sh_moved[NCLASS];
	uint64_t	sh_refused[NCLASS];
	uint64_t	sh_skipped[NCLASS];	/* by the probe's options */
	int		sh_move_errs[8];	/* rdm_error values seen */
	int		sh_move_errn[8];
	int		sh_nkeys_unavail;
	int		sh_nkeys_checked;
	/* What the MOS phase did. */
	uint64_t	sh_mos_dirtied;
	int		sh_mos_rounds;
} shared_t;

static shared_t *sh;
static const char *dir;
static char odir[PATH_BUF];
static uint64_t ashift;
static int failures;
static boolean_t opt_nodva, opt_noroot, opt_nomos, opt_keep, opt_noraw;

static void
check(boolean_t ok, const char *fmt, ...)
{
	va_list ap;

	(void) printf("%s ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
	(void) printf("\n");
	if (!ok)
		failures++;
}

static void
fill_pattern(uint64_t *buf, size_t size, uint64_t seed)
{
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ (seed * 0x2545f4914f6cdd1dULL);

	for (size_t i = 0; i < size / sizeof (uint64_t); i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = x;
	}
}

static void
fill_expect(void *buf, const expect_t *e)
{
	if (e->e_seed == 0)
		memset(buf, e->e_byte, e->e_size);
	else
		fill_pattern(buf, e->e_size, e->e_seed);
}

/* Run fn(arg) in a child; its exit status, or 128 + its signal. */
static int
phase(const char *name, int (*fn)(void *), void *arg)
{
	pid_t pid;
	int status;

	(void) fflush(stdout);
	pid = fork();
	VERIFY3S(pid, !=, -1);
	if (pid == 0) {
		int rc = fn(arg);

		(void) fflush(stdout);
		_exit(rc);
	}
	VERIFY3S(waitpid(pid, &status, 0), ==, pid);
	if (WIFEXITED(status)) {
		(void) printf("INFO phase %s: exit status %d\n", name,
		    WEXITSTATUS(status));
		return (WEXITSTATUS(status));
	}
	(void) printf("INFO phase %s: killed by signal %d\n", name,
	    WTERMSIG(status));
	return (128 + WTERMSIG(status));
}

/* The pool: five files of FILE_SIZE, raidz1, every feature enabled. */
static void
create_pool(void)
{
	nvlist_t *child[WIDTH], *raid, *root, *props;
	char path[PATH_BUF];

	for (int c = 0; c < WIDTH; c++) {
		int fd;

		(void) snprintf(path, sizeof (path), "%s/v%d", dir, c);
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, !=, -1);
		VERIFY0(ftruncate(fd, FILE_SIZE));
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, ashift);
	}
	raid = fnvlist_alloc();
	fnvlist_add_string(raid, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(raid, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, WIDTH);
	root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&raid, 1);

	props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char buf[256];

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		(void) snprintf(buf, sizeof (buf), "feature@%s",
		    spa_feature_table[i].fi_uname);
		fnvlist_add_uint64(props, buf, 0);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raid);
	for (int c = 0; c < WIDTH; c++)
		fnvlist_free(child[c]);
}

/* Import the pool found in <from>; the import's error if it fails. */
static int
try_import(const char *from, spa_t **spap)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL;
	int err;
	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	memset(&ia, 0, sizeof (ia));
	paths[0] = (char *)from;
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	err = zpool_find_config(&lpch, POOL, &config, &ia);
	if (err != 0)
		return (err);
	err = spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL);
	fnvlist_free(config);
	if (err == 0)
		VERIFY0(spa_open(POOL, spap, FTAG));
	return (err);
}

/* A read-only import is not exported: kernel_fini() evicts it. */
static void
export_pool(spa_t *spa)
{
	boolean_t rw = spa_writeable(spa);

	spa_close(spa, FTAG);
	if (rw)
		VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
}

/* ---- phase 0 ---- */

typedef struct live {
	char		l_ds[DSNAME];
	uint64_t	l_obj;
	uint64_t	l_size;
	uint64_t	l_seed;
	uint8_t		l_byte;
	boolean_t	l_alive;
} live_t;

static live_t lives[MAXLIVE];
static int nlives;

static void
add_expect(const char *ds, uint64_t obj, uint64_t size, uint64_t seed,
    uint8_t byte)
{
	expect_t *e = &sh->sh_exp[sh->sh_nexp++];

	VERIFY3S(sh->sh_nexp, <=, MAXEXP);
	(void) strlcpy(e->e_ds, ds, sizeof (e->e_ds));
	e->e_obj = obj;
	e->e_size = size;
	e->e_seed = seed;
	e->e_byte = byte;
}

/* What <ds> holds now becomes what <as> must hold. */
static void
record_live(const char *ds, const char *as)
{
	for (int i = 0; i < nlives; i++) {
		live_t *l = &lives[i];

		if (l->l_alive && strcmp(l->l_ds, ds) == 0)
			add_expect(as, l->l_obj, l->l_size, l->l_seed,
			    l->l_byte);
	}
}

/* What <from> must hold, <as> must hold too. */
static void
copy_expect(const char *from, const char *as)
{
	int n = sh->sh_nexp;

	for (int i = 0; i < n; i++) {
		expect_t *e = &sh->sh_exp[i];

		if (strcmp(e->e_ds, from) == 0)
			add_expect(as, e->e_obj, e->e_size, e->e_seed,
			    e->e_byte);
	}
}

static live_t *
find_live(const char *ds, uint64_t obj)
{
	for (int i = 0; i < nlives; i++) {
		if (lives[i].l_alive && lives[i].l_obj == obj &&
		    strcmp(lives[i].l_ds, ds) == 0)
			return (&lives[i]);
	}
	VERIFY(0);
	return (NULL);
}

static void
write_content(objset_t *os, uint64_t obj, uint64_t size, uint64_t seed,
    uint8_t byte)
{
	expect_t e = { .e_size = size, .e_seed = seed, .e_byte = byte };
	void *buf = malloc(size);
	dmu_tx_t *tx;

	fill_expect(buf, &e);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, obj, 0, size);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	dmu_write(os, obj, 0, size, buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	free(buf);
}

/* A new object of <size> bytes in BLKSZ blocks, recorded as live. */
static uint64_t
put(const char *ds, uint64_t size, uint64_t seed, uint8_t byte)
{
	objset_t *os;
	dmu_tx_t *tx;
	uint64_t obj;
	live_t *l;

	VERIFY0(dmu_objset_own(ds, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS,
	    MIN(size, BLKSZ), DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	write_content(os, obj, size, seed, byte);
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);

	VERIFY3S(nlives, <, MAXLIVE);
	l = &lives[nlives++];
	(void) strlcpy(l->l_ds, ds, sizeof (l->l_ds));
	l->l_obj = obj;
	l->l_size = size;
	l->l_seed = seed;
	l->l_byte = byte;
	l->l_alive = B_TRUE;
	return (obj);
}

static void
overwrite(const char *ds, uint64_t obj, uint64_t seed)
{
	live_t *l = find_live(ds, obj);
	objset_t *os;

	VERIFY0(dmu_objset_own(ds, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	write_content(os, obj, l->l_size, seed, 0);
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	l->l_seed = seed;
}

static void
remove_object(const char *ds, uint64_t obj)
{
	live_t *l = find_live(ds, obj);
	objset_t *os;

	VERIFY0(dmu_objset_own(ds, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	VERIFY0(dmu_free_long_object(os, obj));
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	l->l_alive = B_FALSE;
}

static void
snapshot(const char *ds, const char *snap)
{
	char name[DSNAME];

	VERIFY0(dmu_objset_snapshot_one(ds, snap));
	(void) snprintf(name, sizeof (name), "%s@%s", ds, snap);
	record_live(ds, name);
}

static void
set_prop(const char *ds, zfs_prop_t prop, uint64_t val)
{
	VERIFY0(dsl_prop_set_int(ds, zfs_prop_to_name(prop),
	    ZPROP_SRC_LOCAL, val));
}

static dsl_crypto_params_t *
wkey_params(boolean_t create)
{
	nvlist_t *crypto_args = fnvlist_alloc();
	nvlist_t *props = NULL;
	dsl_crypto_params_t *dcp;

	fnvlist_add_uint8_array(crypto_args, "wkeydata",
	    (uint8_t *)wkeydata, WRAPPING_KEY_LEN);
	if (create) {
		props = fnvlist_alloc();
		fnvlist_add_uint64(props, zfs_prop_to_name(ZFS_PROP_ENCRYPTION),
		    ZIO_CRYPT_AES_256_GCM);
		fnvlist_add_uint64(props, zfs_prop_to_name(ZFS_PROP_KEYFORMAT),
		    ZFS_KEYFORMAT_RAW);
		fnvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_KEYLOCATION), "prompt");
		fnvlist_add_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_PBKDF2_SALT), 0ULL);
		fnvlist_add_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_PBKDF2_ITERS), 0ULL);
	}
	VERIFY0(dsl_crypto_params_create_nvlist(DCP_CMD_NONE, props,
	    crypto_args, &dcp));
	fnvlist_free(crypto_args);
	if (props != NULL)
		fnvlist_free(props);
	return (dcp);
}

static int
send_out(objset_t *os, void *buf, int len, void *arg)
{
	int fd = *(int *)arg;
	char *p = buf;

	(void) os;
	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0)
			return (SET_ERROR(errno));
		p += n;
		len -= n;
	}
	return (0);
}

/* zfs send -w [-i from] snap | zfs recv tofs@tosnap, in one process. */
static void
raw_send_recv(const char *snap, const char *from, const char *tofs,
    const char *tosnap)
{
	char path[PATH_BUF];
	dmu_send_outparams_t dso = { 0 };
	dmu_replay_record_t drr;
	dmu_recv_cookie_t drc;
	zfs_file_t *fp;
	offset_t off = 0;
	ssize_t resid;
	int fd, err;

	(void) snprintf(path, sizeof (path), "%s/stream", dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, !=, -1);
	dso.dso_outfunc = send_out;
	dso.dso_arg = &fd;
	dso.dso_dryrun = B_FALSE;
	zfs_flags |= ZFS_DEBUG_SET_ERROR;
	err = dmu_send(snap, from, B_TRUE, B_TRUE, B_TRUE, B_TRUE, B_FALSE,
	    0, 0, NULL, fd, &off, &dso);
	if (err != 0) {
		(void) printf("INFO send: %d\n", err);
		(void) fflush(stdout);
		zfs_dbgmsg_print(STDOUT_FILENO, "crypt_probe");
	}
	zfs_flags &= ~ZFS_DEBUG_SET_ERROR;
	VERIFY0(err);
	(void) close(fd);
	(void) printf("INFO raw send of %s%s%s: %lld bytes\n", snap,
	    from != NULL ? " from " : "", from != NULL ? from : "",
	    (longlong_t)off);

	VERIFY0(zfs_file_open(path, O_RDONLY, 0, kcred, &fp));
	VERIFY0(zfs_file_read(fp, &drr, sizeof (drr), &resid));
	VERIFY0(resid);
	off = sizeof (drr);
	memset(&drc, 0, sizeof (drc));
	err = dmu_recv_begin(tofs, tosnap, &drr, B_FALSE, B_FALSE, B_FALSE,
	    NULL, NULL, NULL, &drc, fp, &off);
	VERIFY0(err);
	drc.drc_errors = fnvlist_alloc();
	zfs_flags |= ZFS_DEBUG_SET_ERROR;
	err = dmu_recv_stream(&drc, &off);
	if (err != 0)
		(void) printf("INFO recv stream: %d\n", err);
	if (err == 0)
		err = dmu_recv_end(&drc, NULL);
	if (err != 0) {
		(void) fflush(stdout);
		zfs_dbgmsg_print(STDOUT_FILENO, "crypt_probe");
	}
	zfs_flags &= ~ZFS_DEBUG_SET_ERROR;
	VERIFY0(err);
	fnvlist_free(drc.drc_errors);
	zfs_file_close(fp);
	(void) unlink(path);
}

typedef struct epoch_arg {
	uint64_t	ea_table[6];
} epoch_arg_t;

static int
epoch_check(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	if (!dmu_tx_is_syncing(tx))
		return (0);
	ea->ea_table[3] = vdev_raidz_parity_epochs_first_start(tx);
	return (vdev_raidz_parity_epochs_set_check_tx(vd, ea->ea_table, 2, tx,
	    NULL));
}

static void
epoch_sync(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	VERIFY0(vdev_raidz_parity_epochs_set(vd, ea->ea_table, 2, tx));
}

/*
 * The ZPL's master node, with nothing but its version: a send of a ZFS
 * objset reads it, and a raw receive takes nothing but a ZFS objset or a
 * zvol.  Version 4 predates system attributes, so no stream needs them.
 */
static void
master_node(const char *ds)
{
	objset_t *os;
	dmu_tx_t *tx;
	uint64_t version = 4;

	VERIFY0(dmu_objset_own(ds, DMU_OST_ZFS, B_FALSE, B_TRUE, FTAG, &os));
	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY0(zap_create_claim(os, MASTER_NODE_OBJ, DMU_OT_MASTER_NODE,
	    DMU_OT_NONE, 0, tx));
	VERIFY0(zap_update(os, MASTER_NODE_OBJ, ZPL_VERSION_STR, 8, 1,
	    &version, tx));
	dmu_tx_commit(tx);
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
}

static int
child_setup(void *arg)
{
	dsl_crypto_params_t *dcp;
	uint64_t x[5], p[4];
	spa_t *spa;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	set_prop(POOL, ZFS_PROP_COMPRESSION, ZIO_COMPRESS_LZ4);

	/* Unencrypted: plain, dedup, gang, embedded, snapshot-only. */
	for (int i = 0; i < 4; i++)
		p[i] = put(POOL, OBJSZ, 10 + i, 0);
	set_prop(POOL, ZFS_PROP_DEDUP, ZIO_CHECKSUM_SHA256);
	(void) put(POOL, BLKSZ, 20, 0);
	(void) put(POOL, BLKSZ, 20, 0);
	set_prop(POOL, ZFS_PROP_DEDUP, ZIO_CHECKSUM_OFF);
	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	(void) put(POOL, BLKSZ, 30, 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
	(void) put(POOL, 4096, 0, 'E');		/* compresses into its bp */
	snapshot(POOL, "s1");
	overwrite(POOL, p[2], 112);
	remove_object(POOL, p[3]);

	VERIFY0(dmu_objset_create(POOL "/c2", DMU_OST_ZFS, 0, NULL, NULL,
	    NULL));
	set_prop(POOL "/c2", ZFS_PROP_COPIES, 2);
	(void) put(POOL "/c2", OBJSZ, 40, 0);
	(void) put(POOL "/c2", OBJSZ, 41, 0);

	/* Encrypted: the same shapes, two snapshots, a clone. */
	dcp = wkey_params(B_TRUE);
	VERIFY0(dmu_objset_create(POOL "/enc", DMU_OST_ZFS, 0, dcp, NULL,
	    NULL));
	dsl_crypto_params_free(dcp, B_FALSE);
	master_node(POOL "/enc");
	for (int i = 0; i < 4; i++)
		x[i] = put(POOL "/enc", OBJSZ, 50 + i, 0);
	set_prop(POOL "/enc", ZFS_PROP_DEDUP, ZIO_CHECKSUM_SHA256);
	(void) put(POOL "/enc", BLKSZ, 60, 0);
	(void) put(POOL "/enc", BLKSZ, 60, 0);
	set_prop(POOL "/enc", ZFS_PROP_DEDUP, ZIO_CHECKSUM_OFF);
	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	(void) put(POOL "/enc", BLKSZ, 70, 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
	(void) put(POOL "/enc", 512, 71, 0);
	snapshot(POOL "/enc", "s1");
	overwrite(POOL "/enc", x[2], 152);
	remove_object(POOL "/enc", x[3]);
	x[4] = put(POOL "/enc", OBJSZ, 54, 0);
	snapshot(POOL "/enc", "s2");
	remove_object(POOL "/enc", x[1]);

	/*
	 * Two copies of an encrypted block: DVA 1 is the last one it can
	 * have, since the third holds the IV and salt.
	 */
	set_prop(POOL "/enc", ZFS_PROP_COPIES, 2);
	(void) put(POOL "/enc", OBJSZ, 80, 0);
	set_prop(POOL "/enc", ZFS_PROP_COPIES, 1);

	/* The clone starts as what the snapshot holds. */
	VERIFY0(dsl_dataset_clone(POOL "/encclone", POOL "/enc@s1"));
	for (int i = 0; i < sh->sh_nexp; i++) {
		expect_t *e = &sh->sh_exp[i];
		live_t *l;

		if (strcmp(e->e_ds, POOL "/enc@s1") != 0)
			continue;
		VERIFY3S(nlives, <, MAXLIVE);
		l = &lives[nlives++];
		(void) strlcpy(l->l_ds, POOL "/encclone", sizeof (l->l_ds));
		l->l_obj = e->e_obj;
		l->l_size = e->e_size;
		l->l_seed = e->e_seed;
		l->l_byte = e->e_byte;
		l->l_alive = B_TRUE;
	}
	overwrite(POOL "/encclone", x[0], 150);

	/* Raw receive into a second encryption root, full then -i. */
	raw_send_recv(POOL "/enc@s1", NULL, POOL "/rcv", "s1");
	raw_send_recv(POOL "/enc@s2", POOL "/enc@s1", POOL "/rcv", "s2");
	copy_expect(POOL "/enc@s1", POOL "/rcv@s1");
	copy_expect(POOL "/enc@s2", POOL "/rcv@s2");
	copy_expect(POOL "/enc@s2", POOL "/rcv");

	/* The parity-2 epoch, then a control in each dataset kind. */
	{
		epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

		VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
		sh->sh_epoch_txg = ea.ea_table[3];
		(void) printf("INFO parity 2 from txg %llu\n",
		    (u_longlong_t)sh->sh_epoch_txg);
	}
	(void) put(POOL, OBJSZ, 90, 0);
	(void) put(POOL "/enc", OBJSZ, 91, 0);

	record_live(POOL, POOL);
	record_live(POOL "/c2", POOL "/c2");
	record_live(POOL "/enc", POOL "/enc");
	record_live(POOL "/encclone", POOL "/encclone");
	(void) printf("INFO %d objects to check across the datasets and "
	    "snapshots\n", sh->sh_nexp);

	export_pool(spa);
	kernel_fini();
	return (0);
}

/* ---- census ---- */

/* Distinct DVAs: an open-addressing set of {offset, birth}. */
#define	SET_BITS	20
typedef struct dvaset {
	uint64_t	*ds_key;	/* 2 per slot; offset + 1, 0: empty */
} dvaset_t;

static boolean_t
dvaset_add(dvaset_t *s, uint64_t offset, uint64_t birth)
{
	uint64_t mask = (1ULL << SET_BITS) - 1;
	uint64_t h = (offset * 0x9e3779b97f4a7c15ULL ^ birth) & mask;

	for (;;) {
		uint64_t *k = &s->ds_key[2 * h];

		if (k[0] == 0) {
			k[0] = offset + 1;
			k[1] = birth;
			return (B_TRUE);
		}
		if (k[0] == offset + 1 && k[1] == birth)
			return (B_FALSE);
		h = (h + 1) & mask;
	}
}

/* One DVA to move, kept by the census for the move phase. */
typedef struct mv {
	blkptr_t	mv_bp;
	zbookmark_phys_t mv_zb;
	int		mv_dva;
	uint64_t	mv_psize;
	uint8_t		mv_class[NCLASS];
} mv_t;

typedef struct census_arg {
	census_t	*ca_c;
	dvaset_t	ca_set;
	spa_t		*ca_spa;
	/* The datasets met so far, by object: snapshot, recv, clone. */
	uint64_t	ca_dsobj[64];
	uint8_t		ca_dsflags[64];
	int		ca_nds;
	/* The old DVAs, for the move and MOS phases, or NULL. */
	mv_t		*ca_mv;
	int		ca_nmv;
	int		ca_maxmv;
} census_arg_t;

#define	DSF_SNAP	1
#define	DSF_RECV	2
#define	DSF_CLONE	4

static uint8_t
ds_flags(census_arg_t *ca, uint64_t dsobj)
{
	dsl_pool_t *dp = spa_get_dsl(ca->ca_spa);
	dsl_dataset_t *ds;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	uint8_t f = 0;

	for (int i = 0; i < ca->ca_nds; i++) {
		if (ca->ca_dsobj[i] == dsobj)
			return (ca->ca_dsflags[i]);
	}
	dsl_pool_config_enter(dp, FTAG);
	if (dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds) == 0) {
		dsl_dataset_name(ds, name);
		if (ds->ds_is_snapshot)
			f |= DSF_SNAP;
		if (strstr(name, "/rcv") != NULL)
			f |= DSF_RECV;
		if (dsl_dir_is_clone(ds->ds_dir))
			f |= DSF_CLONE;
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);
	VERIFY3S(ca->ca_nds, <, 64);
	ca->ca_dsobj[ca->ca_nds] = dsobj;
	ca->ca_dsflags[ca->ca_nds++] = f;
	return (f);
}

static uint64_t
gang_header_psize(spa_t *spa)
{
	return (spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER) ?
	    vdev_gang_header_psize(spa->spa_root_vdev->vdev_child[0]) :
	    SPA_OLD_GANGBLOCKSIZE);
}

/* The bytes at DVA <d> of bp as stored: no checksum, no transform. */
static abd_t *
read_raw(spa_t *spa, const blkptr_t *bp, int d, uint64_t psize, int *errp)
{
	blkptr_t rbp = *bp;
	abd_t *abd = abd_alloc_linear(psize, B_FALSE);

	rbp.blk_dva[0] = bp->blk_dva[d];
	for (int i = 1; i < SPA_DVAS_PER_BP; i++)
		memset(&rbp.blk_dva[i], 0, sizeof (dva_t));
	DVA_SET_GANG(&rbp.blk_dva[0], 0);
	BP_SET_LSIZE(&rbp, psize);
	BP_SET_PSIZE(&rbp, psize);
	BP_SET_COMPRESS(&rbp, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(&rbp, ZIO_CHECKSUM_OFF);
	BP_SET_DEDUP(&rbp, 0);
	BP_SET_CRYPT(&rbp, 0);
	ZIO_SET_CHECKSUM(&rbp.blk_cksum, 0, 0, 0, 0);
	*errp = zio_wait(zio_read(NULL, spa, &rbp, abd, psize, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, NULL));
	return (abd);
}

static void
count_bp(census_arg_t *ca, const blkptr_t *bp, const zbookmark_phys_t *zb,
    boolean_t child, int depth);

static void
count_dva(census_arg_t *ca, const blkptr_t *bp, const zbookmark_phys_t *zb,
    int d, boolean_t child, uint8_t *cls)
{
	spa_t *spa = ca->ca_spa;
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	const dva_t *dva = &bp->blk_dva[d];
	uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);
	uint64_t offset = DVA_GET_OFFSET(dva);
	raidz_redirect_ent_t ent;
	census_t *c = ca->ca_c;
	int st;

	if (DVA_GET_VDEV(dva) != 0 || !dvaset_add(&ca->ca_set, offset, birth))
		return;
	if (birth >= sh->sh_epoch_txg)
		st = ST_NEW;
	else if (vdev_raidz_redirect_get_live(vd, offset, birth, &ent))
		st = ST_REDIR;
	else
		st = ST_OLD;

	memset(cls, 0, NCLASS);
	cls[C_ALL] = 1;
	if (zb->zb_objset == DMU_META_OBJSET) {
		cls[C_MOS] = 1;
	} else {
		uint8_t f = ds_flags(ca, zb->zb_objset);

		cls[C_DATASET] = 1;
		cls[C_SNAP] = !!(f & DSF_SNAP);
		cls[C_RECV] = !!(f & DSF_RECV);
		cls[C_CLONE] = !!(f & DSF_CLONE);
	}
	if (BP_IS_ENCRYPTED(bp))
		cls[C_ENC_DATA] = 1;
	else if (BP_IS_AUTHENTICATED(bp))
		cls[C_ENC_AUTH] = 1;
	else if (BP_HAS_INDIRECT_MAC_CKSUM(bp))
		cls[C_ENC_MAC] = 1;
	else
		cls[C_CLEAR] = 1;
	/* ZB_ZIL_OBJECT is 0, the meta-dnode's: the level tells them apart. */
	if (zb->zb_level == ZB_ZIL_LEVEL ||
	    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG)
		cls[C_ZIL] = 1;
	else if (zb->zb_object == ZB_ROOT_OBJECT &&
	    zb->zb_level == ZB_ROOT_LEVEL)
		cls[C_ROOT] = 1;
	else if (zb->zb_object == DMU_META_DNODE_OBJECT)
		cls[C_METADNODE] = 1;
	if (BP_GET_LEVEL(bp) > 0)
		cls[C_INDIRECT] = 1;
	else
		cls[C_L0] = 1;
	cls[C_DVA0 + d] = 1;
	if (DVA_GET_GANG(dva))
		cls[C_GANG_HDR] = 1;
	if (child)
		cls[C_GANG_CHILD] = 1;
	if (BP_GET_DEDUP(bp))
		cls[C_DEDUP] = 1;
	for (int i = 0; i < NCLASS; i++)
		c->c_n[i][st] += cls[i];

	if (st == ST_OLD && ca->ca_mv != NULL) {
		mv_t *m = &ca->ca_mv[ca->ca_nmv++];

		VERIFY3S(ca->ca_nmv, <=, ca->ca_maxmv);
		m->mv_bp = *bp;
		m->mv_zb = *zb;
		m->mv_dva = d;
		m->mv_psize = DVA_GET_GANG(dva) ? gang_header_psize(spa) :
		    BP_GET_PSIZE(bp);
		memcpy(m->mv_class, cls, NCLASS);
	}
}

/*
 * Count every DVA of bp; a gang block's header copies, then the DVAs of
 * its children, read from the header's first DVA.
 */
static void
count_bp(census_arg_t *ca, const blkptr_t *bp, const zbookmark_phys_t *zb,
    boolean_t child, int depth)
{
	uint8_t cls[NCLASS];

	if (BP_IS_HOLE(bp))
		return;
	if (BP_IS_EMBEDDED(bp)) {
		ca->ca_c->c_embedded++;
		return;
	}
	for (int d = 0; d < BP_GET_NDVAS(bp); d++)
		count_dva(ca, bp, zb, d, child, cls);
	if (BP_IS_GANG(bp) && depth < 8) {
		uint64_t gsize = gang_header_psize(ca->ca_spa);
		int err;
		abd_t *abd = read_raw(ca->ca_spa, bp, 0, gsize, &err);

		if (err == 0) {
			zio_gbh_phys_t *gbh = abd_to_buf(abd);

			for (int g = 0; g < gbh_nblkptrs(gsize); g++)
				count_bp(ca, gbh_bp(gbh, g), zb, B_TRUE,
				    depth + 1);
		} else {
			(void) printf("INFO census: gang header at %llx "
			    "unreadable (%d)\n", (u_longlong_t)
			    DVA_GET_OFFSET(&bp->blk_dva[0]), err);
			ca->ca_c->c_err = err;
		}
		abd_free(abd);
	}
}

static int
census_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	census_arg_t *ca = arg;

	(void) spa, (void) zilog, (void) dnp;
	if (bp == NULL)
		return (0);
	ca->ca_c->c_bps++;
	count_bp(ca, bp, zb, B_FALSE, 0);
	return (0);
}

static void
run_census(spa_t *spa, census_t *c, mv_t *mv, int maxmv, int *nmv)
{
	census_arg_t *ca = calloc(1, sizeof (*ca));

	memset(c, 0, sizeof (*c));
	ca->ca_c = c;
	ca->ca_spa = spa;
	ca->ca_set.ds_key = calloc(2ULL << SET_BITS, sizeof (uint64_t));
	ca->ca_mv = mv;
	ca->ca_maxmv = maxmv;
	c->c_err = traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD |
	    TRAVERSE_NO_DECRYPT, census_cb, ca);
	if (nmv != NULL)
		*nmv = ca->ca_nmv;
	free(ca->ca_set.ds_key);
	free(ca);
}

static void
print_census(const char *when, const census_t *c)
{
	(void) printf("INFO census %s: %llu block pointers visited, %llu "
	    "embedded (no DVA), traverse_pool() returned %d\n", when,
	    (u_longlong_t)c->c_bps, (u_longlong_t)c->c_embedded, c->c_err);
	(void) printf("INFO census %s: %-36s %8s %8s %8s\n", when, "class",
	    "new", "moved", "OLD");
	for (int i = 0; i < NCLASS; i++) {
		if (c->c_n[i][0] + c->c_n[i][1] + c->c_n[i][2] == 0)
			continue;
		(void) printf("INFO census %s: %-36s %8llu %8llu %8llu\n",
		    when, class_names[i], (u_longlong_t)c->c_n[i][ST_NEW],
		    (u_longlong_t)c->c_n[i][ST_REDIR],
		    (u_longlong_t)c->c_n[i][ST_OLD]);
	}
}

/*
 * Blocks that no traversal reaches but that are still allocated: frees
 * deferred to a later txg and the pool's free bpobj (an async destroy).
 */
typedef struct pend_arg {
	vdev_t		*pa_vd;
	uint64_t	pa_old;
	uint64_t	pa_all;
} pend_arg_t;

static int
pend_cb(void *arg, const blkptr_t *bp, boolean_t bp_freed, dmu_tx_t *tx)
{
	pend_arg_t *pa = arg;

	(void) bp_freed, (void) tx;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);
		raidz_redirect_ent_t ent;

		if (DVA_GET_VDEV(&bp->blk_dva[d]) != 0)
			continue;
		pa->pa_all++;
		if (birth < sh->sh_epoch_txg &&
		    !vdev_raidz_redirect_get_live(pa->pa_vd,
		    DVA_GET_OFFSET(&bp->blk_dva[d]), birth, &ent))
			pa->pa_old++;
	}
	return (0);
}

static void
census_pending(spa_t *spa, const char *when)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	pend_arg_t def = { 0 }, fre = { 0 };
	uint64_t bptree = 0;

	def.pa_vd = fre.pa_vd = spa->spa_root_vdev->vdev_child[0];

	(void) bpobj_iterate_nofree(&spa->spa_deferred_bpobj, pend_cb, &def,
	    NULL);
	if (dp->dp_free_bpobj.bpo_object != 0)
		(void) bpobj_iterate_nofree(&dp->dp_free_bpobj, pend_cb, &fre,
		    NULL);
	bptree = dp->dp_bptree_obj;
	(void) printf("INFO census %s: pending frees, not traversed: deferred "
	    "%llu DVAs (%llu old), free bpobj %llu (%llu old), async destroy "
	    "bptree %s\n", when, (u_longlong_t)def.pa_all,
	    (u_longlong_t)def.pa_old, (u_longlong_t)fre.pa_all,
	    (u_longlong_t)fre.pa_old, bptree != 0 ? "present" : "none");
	sh->sh_pending_old = def.pa_old + fre.pa_old;
}

/* Let the deferred frees of the last txgs reach the space maps. */
static void
space_settle(spa_t *spa)
{
	for (int i = 0; i < TXG_DEFER_SIZE + 2; i++) {
		dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);

		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_tx_commit(tx);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
}

/*
 * What the DSL charges against what the raidz holds.  Datasets are charged
 * a block's dsize from its DVA's asize at birth, and a move keeps the DVA:
 * the gap is what "available" would overstate (accounting defect X2).
 */
static void
print_space(spa_t *spa, const char *when)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	uint64_t used = dsl_dir_phys(dp->dp_root_dir)->dd_used_bytes;
	uint64_t alloc = vd->vdev_stat.vs_alloc;
	uint64_t dalloc = vdev_deflated_space(vd, alloc);
	int64_t charge = 0;

#ifdef	RAIDZ_REDIRECT_DSPACE
	/* What the kernel takes out of dspace for moved blocks. */
	charge = vdev_raidz_redirect_dspace(spa);
#endif
	(void) printf("INFO space %s: vdev allocated %llu (deflated %llu), "
	    "DSL used %llu, deflated minus used %lld, moved-block charge "
	    "%lld, dspace %llu\n", when, (u_longlong_t)alloc,
	    (u_longlong_t)dalloc, (u_longlong_t)used,
	    (longlong_t)(dalloc - used), (longlong_t)charge,
	    (u_longlong_t)spa_get_dspace(spa));
	sh->sh_space_gap = (int64_t)(dalloc - used) - charge;
}

static int
child_census(void *arg)
{
	const char *when = arg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ);
	VERIFY0(try_import(dir, &spa));
	run_census(spa, &sh->sh_census, NULL, 0, NULL);
	print_census(when, &sh->sh_census);
	census_pending(spa, when);
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* ---- reading everything back ---- */

static int
load_keys(void)
{
	const char *roots[] = { POOL "/enc", POOL "/rcv" };
	int bad = 0;

	for (int i = 0; i < 2; i++) {
		dsl_crypto_params_t *dcp = wkey_params(B_FALSE);
		int err = spa_keystore_load_wkey(roots[i], dcp, B_FALSE);

		dsl_crypto_params_free(dcp, err != 0);
		if (err != 0) {
			(void) printf("INFO load-key %s: error %d\n", roots[i],
			    err);
			bad++;
		}
	}
	return (bad);
}

/* Read every expected object; the number that read back right. */
static int
read_all(const char *what, int *nerr)
{
	int good = 0;

	*nerr = 0;
	for (int i = 0; i < sh->sh_nexp; i++) {
		expect_t *e = &sh->sh_exp[i];
		void *buf = malloc(e->e_size), *want = malloc(e->e_size);
		objset_t *os;
		int err;

		fill_expect(want, e);
		err = dmu_objset_hold_flags(e->e_ds, B_TRUE, FTAG, &os);
		if (err == 0) {
			err = dmu_read(os, e->e_obj, 0, e->e_size, buf,
			    DMU_READ_NO_PREFETCH);
			dmu_objset_rele_flags(os, B_TRUE, FTAG);
		}
		if (err == 0 && memcmp(buf, want, e->e_size) == 0) {
			good++;
		} else {
			(*nerr)++;
			(void) printf("INFO %s: %s object %llu: %s (%d)\n",
			    what, e->e_ds, (u_longlong_t)e->e_obj,
			    err == 0 ? "wrong bytes" : "unreadable", err);
		}
		free(buf);
		free(want);
	}
	return (good);
}

/* Read back on the pool itself: the keys load and every byte is right. */
static int
child_read(void *arg)
{
	spa_t *spa;
	int good, nerr;

	(void) arg;
	kernel_init(SPA_MODE_READ);
	VERIFY0(try_import(dir, &spa));
	check(load_keys() == 0, "both encryption roots' keys load");
	good = read_all("read", &nerr);
	check(nerr == 0, "%d of %d objects read back bit-exact, keys loaded",
	    good, sh->sh_nexp);
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* ---- the oracle ---- */

static void
copy_pool(void)
{
	char cmd[PATH_BUF * 3];

	(void) snprintf(odir, sizeof (odir), "%s/oracle", dir);
	(void) snprintf(cmd, sizeof (cmd), "rm -rf %s && mkdir %s && "
	    "for c in 0 1 2 3 4; do cp --sparse=always %s/v$c %s/v$c; done",
	    odir, odir, dir, odir);
	VERIFY0(system(cmd));
}

/* Invert every allocated byte of a copied child past its front labels. */
static uint64_t
damage_child(int c)
{
	char path[PATH_BUF];
	uint64_t done = 0;
	size_t bufsz = 1 << 20;
	uint8_t *buf = malloc(bufsz);
	off_t pos = FRONT, end = FILE_SIZE - VDEV_LABEL_END_SIZE;
	int fd;

	(void) snprintf(path, sizeof (path), "%s/v%d", odir, c);
	fd = open(path, O_RDWR);
	VERIFY3S(fd, !=, -1);
	while (pos < end) {
		off_t data = lseek(fd, pos, SEEK_DATA);
		off_t hole;

		if (data < 0 || data >= end)
			break;
		hole = lseek(fd, data, SEEK_HOLE);
		if (hole > end)
			hole = end;
		for (off_t o = data; o < hole; o += bufsz) {
			size_t n = MIN(bufsz, hole - o);

			VERIFY3S(pread(fd, buf, n, o), ==, n);
			for (size_t i = 0; i < n; i++)
				buf[i] ^= 0xff;
			VERIFY3S(pwrite(fd, buf, n, o), ==, n);
			done += n;
		}
		pos = hole;
	}
	(void) close(fd);
	free(buf);
	return (done);
}

typedef struct oracle_res {
	int	or_import;
	int	or_keys;
	int	or_good;
	int	or_bad;
} oracle_res_t;

static int
child_oracle_read(void *arg)
{
	oracle_res_t *r = arg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ);
	r->or_import = try_import(odir, &spa);
	if (r->or_import != 0) {
		kernel_fini();
		return (0);
	}
	r->or_keys = load_keys();
	r->or_good = read_all("oracle", &r->or_bad);
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* zdb -bcc on the copy: block pointers it could not read. */
static int
run_zdb(const char *on, const char *what, int *leaks)
{
	const char *zdb = getenv("ZDB") != NULL ? getenv("ZDB") : "zdb";
	char cmd[PATH_BUF * 2];
	char line[4096];
	int errs = 0, status, noleaks = 0, shown = 0;
	FILE *fp;

	(void) snprintf(cmd, sizeof (cmd), "%s -bcc -d -Y -e -p %s %s 2>&1",
	    zdb, on, POOL);
	(void) printf("INFO %s: running %s\n", what, cmd);
	(void) fflush(stdout);
	fp = popen(cmd, "r");
	VERIFY3P(fp, !=, NULL);
	while (fgets(line, sizeof (line), fp) != NULL) {
		char *p = line, *cr;

		while ((cr = strchr(p, '\r')) != NULL)
			p = cr + 1;
		if (strstr(p, "Got error") != NULL) {
			errs++;
			if (shown++ < 6)
				(void) printf("zdb| %s", p);
			continue;
		}
		if (strstr(p, "No leaks (block sum matches space maps "
		    "exactly)") != NULL)
			noleaks++;
		if (strstr(p, "leaked") != NULL || strstr(p, "error") != NULL ||
		    strstr(p, "bp count") != NULL ||
		    strstr(p, "No leaks") != NULL ||
		    strstr(p, "loading") != NULL ||
		    strstr(p, "can't open") != NULL ||
		    strstr(p, "cannot") != NULL)
			(void) printf("zdb| %s%s", p,
			    strchr(p, '\n') ? "" : "\n");
	}
	status = pclose(fp);
	(void) printf("INFO %s: zdb exit status %d, %d block pointers it "
	    "could not read\n", what, WIFEXITED(status) ?
	    WEXITSTATUS(status) : 128 + WTERMSIG(status), errs);
	if (leaks != NULL)
		*leaks = noleaks;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0 ? errs :
	    errs + 1000000);
}

static void
oracle(const char *when, boolean_t expect_all)
{
	oracle_res_t *r = mmap(NULL, sizeof (*r), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	uint64_t bytes = 0;
	char what[64];
	int z;

	(void) snprintf(what, sizeof (what), "oracle %s", when);
	copy_pool();
	bytes += damage_child(1);
	bytes += damage_child(3);
	(void) printf("INFO %s: children 1 and 3 of the copy inverted past "
	    "their front labels, %llu bytes\n", what, (u_longlong_t)bytes);
	memset(r, 0, sizeof (*r));
	(void) phase(what, child_oracle_read, r);
	(void) printf("INFO %s: import %d, keys failing %d, objects right %d, "
	    "wrong or unreadable %d of %d\n", what, r->or_import, r->or_keys,
	    r->or_good, r->or_bad, sh->sh_nexp);
	z = r->or_import == 0 ? run_zdb(odir, what, NULL) : -1;
	if (expect_all) {
		check(r->or_import == 0 && r->or_keys == 0 &&
		    r->or_good == sh->sh_nexp, "%s: with two of five children "
		    "damaged, every object reads back (%d of %d)", what,
		    r->or_good, sh->sh_nexp);
		check(z == 0, "%s: zdb -bcc reads every block pointer of the "
		    "damaged copy (%d not read)", what, z);
	} else {
		check(r->or_import != 0 || r->or_good < sh->sh_nexp,
		    "%s: the damaged copy loses data while old blocks remain "
		    "(import %d, %d of %d objects right)", what, r->or_import,
		    r->or_good, sh->sh_nexp);
	}
	if (!opt_keep) {
		char cmd[PATH_BUF * 2];

		(void) snprintf(cmd, sizeof (cmd), "rm -rf %s", odir);
		VERIFY0(system(cmd));
	}
	(void) munmap(r, sizeof (*r));
}

/*
 * ---- the DVA oracle ----
 *
 * The object oracle proves that one copy of each block reads back, and a
 * small block can miss both damaged children.  This one reads every DVA on
 * its own, raw (neither decompressed nor decrypted, but checked against
 * its checksum, which an encrypted block computes over its ciphertext), on
 * ten copies of the pool, each with another pair of children inverted.  A
 * parity-1 extent spans at least two children, so some pair destroys it;
 * a parity-2 extent survives every pair.  A DVA that reads under all ten
 * is at the new parity, whatever the redirect table says.
 */
static int dv_pair;

static void
dv_note(const blkptr_t *bp, int d, const zbookmark_phys_t *zb, int err)
{
	const dva_t *dva = &bp->blk_dva[d];
	uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);
	boolean_t mos = zb->zb_objset == DMU_META_OBJSET;
	boolean_t failed = err != 0;
	uint64_t mask = (1ULL << DVA_BITS) - 1;
	uint64_t off = DVA_GET_OFFSET(dva);
	uint64_t h = (off * 0x9e3779b97f4a7c15ULL ^ birth) & mask;

	for (;;) {
		if (sh->sh_dva[h].dv_offset == 0) {
			sh->sh_dva[h].dv_offset = off + 1;
			sh->sh_dva[h].dv_birth = birth;
			sh->sh_dva[h].dv_mos = mos;
			sh->sh_dva[h].dv_zb = *zb;
			sh->sh_dva[h].dv_d = d;
			sh->sh_dva[h].dv_gang = DVA_GET_GANG(dva);
			sh->sh_dva[h].dv_crypt = BP_USES_CRYPT(bp);
			sh->sh_dva[h].dv_dedup = BP_GET_DEDUP(bp);
			sh->sh_dva[h].dv_ndvas = BP_GET_NDVAS(bp);
			break;
		}
		if (sh->sh_dva[h].dv_offset == off + 1 &&
		    sh->sh_dva[h].dv_birth == birth)
			break;
		h = (h + 1) & mask;
	}
	sh->sh_dva[h].dv_seen |= 1 << dv_pair;
	if (failed) {
		sh->sh_dva[h].dv_fail |= 1 << dv_pair;
		sh->sh_dva[h].dv_err = err;
	}
}

/*
 * A gang header's checksum is embedded in it, with a verifier made of the
 * block pointer's DVA 0: a copy cannot be read on its own under another
 * DVA.  So each copy's bytes, checked against the header's own checksum
 * on the undamaged pool, give a SHA-256 that a plain read of that copy
 * alone is checked against, with the reconstruction that it brings.
 */
static int
gref_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) zilog, (void) dnp, (void) arg, (void) zb;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    !BP_IS_GANG(bp))
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		uint64_t gsize = gang_header_psize(spa);
		abd_t *abd;
		int err;

		if (DVA_GET_VDEV(&bp->blk_dva[d]) != 0)
			continue;
		abd = read_raw(spa, bp, d, gsize, &err);
		if (err == 0)
			err = zio_checksum_error_impl(spa, bp,
			    ZIO_CHECKSUM_GANG_HEADER, abd, gsize, 0, NULL);
		if (err == 0) {
			int n = sh->sh_ngref++;

			VERIFY3S(n, <, 256);
			sh->sh_gref[n].gr_offset = DVA_GET_OFFSET(
			    &bp->blk_dva[d]);
			sh->sh_gref[n].gr_birth = BP_GET_PHYSICAL_BIRTH(bp);
			zio_checksum_table[ZIO_CHECKSUM_SHA256].ci_func[0](abd,
			    gsize, NULL, &sh->sh_gref[n].gr_cksum);
		} else {
			sh->sh_ngref_bad++;
		}
		abd_free(abd);
	}
	return (0);
}

static int
child_gang_ref(void *arg)
{
	spa_t *spa;

	(void) arg;
	kernel_init(SPA_MODE_READ);
	VERIFY0(try_import(dir, &spa));
	sh->sh_ngref = sh->sh_ngref_bad = 0;
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_NO_DECRYPT,
	    gref_cb, NULL));
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* Read the gang header copy at DVA d alone, against its SHA-256. */
static int
read_gang_copy(spa_t *spa, const blkptr_t *bp, int d)
{
	uint64_t off = DVA_GET_OFFSET(&bp->blk_dva[d]);
	uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);
	uint64_t gsize = gang_header_psize(spa);
	blkptr_t rbp = *bp;
	abd_t *abd;
	int i, err;

	for (i = 0; i < sh->sh_ngref; i++) {
		if (sh->sh_gref[i].gr_offset == off &&
		    sh->sh_gref[i].gr_birth == birth)
			break;
	}
	if (i == sh->sh_ngref)
		return (SET_ERROR(ENOENT));
	rbp.blk_dva[0] = bp->blk_dva[d];
	for (int k = 1; k < SPA_DVAS_PER_BP; k++)
		memset(&rbp.blk_dva[k], 0, sizeof (dva_t));
	DVA_SET_GANG(&rbp.blk_dva[0], 0);
	BP_SET_LSIZE(&rbp, gsize);
	BP_SET_PSIZE(&rbp, gsize);
	BP_SET_COMPRESS(&rbp, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(&rbp, ZIO_CHECKSUM_SHA256);
	BP_SET_DEDUP(&rbp, 0);
	BP_SET_CRYPT(&rbp, 0);
	rbp.blk_cksum = sh->sh_gref[i].gr_cksum;
	abd = abd_alloc_linear(gsize, B_FALSE);
	err = zio_wait(zio_read(NULL, spa, &rbp, abd, gsize, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_RETRY,
	    NULL));
	abd_free(abd);
	return (err);
}

static int
dv_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) zilog, (void) dnp, (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		blkptr_t one = *bp;
		uint64_t psize = BP_GET_PSIZE(bp);
		abd_t *abd;
		int err;

		if (DVA_GET_VDEV(&bp->blk_dva[d]) != 0)
			continue;
		one.blk_dva[0] = bp->blk_dva[d];
		memset(&one.blk_dva[1], 0, sizeof (dva_t));
		if (!BP_IS_ENCRYPTED(bp))
			memset(&one.blk_dva[2], 0, sizeof (dva_t));
		/* DVA 0 of a gang block: its header, then its children. */
		err = DVA_GET_GANG(&bp->blk_dva[d]) ?
		    read_gang_copy(spa, bp, d) : 0;
		if (err == 0 && (d == 0 || !DVA_GET_GANG(&bp->blk_dva[d]))) {
			abd = abd_alloc_linear(psize, B_FALSE);
			err = zio_wait(zio_read(NULL, spa, &one, abd, psize,
			    NULL, NULL, ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW |
			    ZIO_FLAG_DONT_RETRY, zb));
			abd_free(abd);
		}
		dv_note(bp, d, zb, err);
	}
	return (0);
}

static int
child_dva_oracle(void *arg)
{
	spa_t *spa;
	int err;

	(void) arg;
	kernel_init(SPA_MODE_READ);
	err = try_import(odir, &spa);
	if (err != 0) {
		kernel_fini();
		return (1);
	}
	(void) traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD |
	    TRAVERSE_NO_DECRYPT, dv_cb, NULL);
	if (dv_pair == NPAIRS - 1) {
		census_t *c = calloc(1, sizeof (*c));

		run_census(spa, c, NULL, 0, NULL);
		(void) printf("INFO dva oracle: the census of this copy counts "
		    "%llu DVAs, %llu of them gang children\n",
		    (u_longlong_t)(c->c_n[C_ALL][0] + c->c_n[C_ALL][1] +
		    c->c_n[C_ALL][2]), (u_longlong_t)(c->c_n[C_GANG_CHILD][0] +
		    c->c_n[C_GANG_CHILD][1] + c->c_n[C_GANG_CHILD][2]));
		free(c);
	}
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* Run the DVA oracle; the number of DVAs that failed under some pair. */
static uint64_t
dva_oracle(const char *when)
{
	char what[64];
	int pair = 0, noimport = 0;
	uint64_t seen = 0, failed = 0, failed_mos = 0, all = 0;

	memset(sh->sh_dva, 0, sizeof (sh->sh_dva));
	(void) phase("gang header references", child_gang_ref, NULL);
	(void) printf("INFO dva oracle %s: %d gang header copies checked "
	    "against their header checksum, %d failed\n", when,
	    sh->sh_ngref, sh->sh_ngref_bad);
	for (int a = 0; a < WIDTH; a++) {
		for (int b = a + 1; b < WIDTH; b++, pair++) {
			char cmd[PATH_BUF * 2];

			copy_pool();
			(void) damage_child(a);
			(void) damage_child(b);
			dv_pair = pair;
			(void) snprintf(what, sizeof (what), "dva oracle %s, "
			    "children %d+%d", when, a, b);
			if (phase(what, child_dva_oracle, NULL) != 0) {
				noimport++;
				(void) printf("INFO %s: the copy does not "
				    "import\n", what);
			}
			(void) snprintf(cmd, sizeof (cmd), "rm -rf %s", odir);
			VERIFY0(system(cmd));
		}
	}
	for (int i = 0; i < (1 << DVA_BITS); i++) {
		if (sh->sh_dva[i].dv_offset == 0)
			continue;
		all++;
		if (sh->sh_dva[i].dv_seen == (1 << NPAIRS) - 1)
			seen++;
		if (sh->sh_dva[i].dv_fail != 0) {
			const zbookmark_phys_t *zb = &sh->sh_dva[i].dv_zb;

			failed++;
			failed_mos += sh->sh_dva[i].dv_mos;
			if (failed > 12)
				continue;
			(void) printf("INFO dva oracle %s: DVA %u of %u at "
			    "%llx birth %llu <%llu, %lld, %lld, %llu>%s%s%s "
			    "failed (error %d) under pairs %#x\n",
			    when, sh->sh_dva[i].dv_d,
			    sh->sh_dva[i].dv_ndvas, (u_longlong_t)
			    (sh->sh_dva[i].dv_offset - 1),
			    (u_longlong_t)sh->sh_dva[i].dv_birth,
			    (u_longlong_t)zb->zb_objset,
			    (longlong_t)zb->zb_object,
			    (longlong_t)zb->zb_level,
			    (u_longlong_t)zb->zb_blkid,
			    sh->sh_dva[i].dv_gang ? " gang" : "",
			    sh->sh_dva[i].dv_crypt ? " crypt" : "",
			    sh->sh_dva[i].dv_dedup ? " dedup" : "",
			    sh->sh_dva[i].dv_err,
			    sh->sh_dva[i].dv_fail);
		}
	}
	(void) printf("INFO dva oracle %s: %llu DVAs met, %llu of them under "
	    "all %d pairs; %llu failed under some pair (%llu in the MOS); %d "
	    "copies did not import\n", when, (u_longlong_t)all,
	    (u_longlong_t)seen, NPAIRS, (u_longlong_t)failed,
	    (u_longlong_t)failed_mos, noimport);
	return (failed + noimport + (all - seen) + sh->sh_ngref_bad);
}

/* ---- the MOS phase ---- */

typedef struct mos_arg {
	mv_t		*ma_mv;
	int		ma_n;
	uint64_t	ma_dirtied;
} mos_arg_t;

/*
 * Dirty each old MOS block in syncing context so that the MOS sync writes
 * it again, at this txg's parity: a level-0 block through its dbuf, a
 * block of the meta-dnode through an allocated dnode it holds.  Indirect
 * blocks and the root block are written again with their children.
 */
static void
mos_sync(void *arg, dmu_tx_t *tx)
{
	mos_arg_t *ma = arg;
	objset_t *mos = dmu_tx_pool(tx)->dp_meta_objset;

	for (int i = 0; i < ma->ma_n; i++) {
		const zbookmark_phys_t *zb = &ma->ma_mv[i].mv_zb;
		dnode_t *dn;

		if (zb->zb_level != 0)
			continue;
		if (zb->zb_object == DMU_META_DNODE_OBJECT) {
			uint64_t epb = DNODES_PER_BLOCK;

			for (uint64_t o = MAX(zb->zb_blkid * epb, 1);
			    o < (zb->zb_blkid + 1) * epb; o++) {
				if (dnode_hold(mos, o, FTAG, &dn) != 0)
					continue;
				dnode_setdirty(dn, tx);
				dnode_rele(dn, FTAG);
				ma->ma_dirtied++;
				break;
			}
			continue;
		}
		if (dnode_hold(mos, zb->zb_object, FTAG, &dn) != 0)
			continue;
		rw_enter(&dn->dn_struct_rwlock, RW_READER);
		dmu_buf_impl_t *db = dbuf_hold(dn, zb->zb_blkid, FTAG);
		rw_exit(&dn->dn_struct_rwlock);
		if (db != NULL) {
			dmu_buf_will_dirty(&db->db, tx);
			dbuf_rele(db, FTAG);
			ma->ma_dirtied++;
		}
		dnode_rele(dn, FTAG);
	}
}

#define	MAXMV	(1 << 16)

static int
child_mos(void *arg)
{
	mv_t *mv = malloc(MAXMV * sizeof (mv_t));
	census_t *c = calloc(1, sizeof (*c));
	spa_t *spa;
	int round;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(try_import(dir, &spa));
	for (round = 1; round <= 4; round++) {
		mos_arg_t ma = { 0 };
		int nmv, n = 0;

		run_census(spa, c, mv, MAXMV, &nmv);
		for (int i = 0; i < nmv; i++) {
			if (mv[i].mv_zb.zb_objset == DMU_META_OBJSET)
				mv[n++] = mv[i];
		}
		(void) printf("INFO mos round %d: %d old MOS DVAs\n", round, n);
		if (n == 0)
			break;
		ma.ma_mv = mv;
		ma.ma_n = n;
		VERIFY0(dsl_sync_task(POOL, NULL, mos_sync, &ma, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(spa_get_dsl(spa), 0);
		sh->sh_mos_dirtied += ma.ma_dirtied;
		(void) printf("INFO mos round %d: %llu buffers dirtied\n",
		    round, (u_longlong_t)ma.ma_dirtied);
	}
	sh->sh_mos_rounds = round;
	export_pool(spa);
	kernel_fini();
	free(mv);
	free(c);
	return (0);
}

/* ---- the move phase ---- */

static void
note_error(int err)
{
	for (int i = 0; i < 8; i++) {
		if (sh->sh_move_errn[i] == 0 || sh->sh_move_errs[i] == err) {
			sh->sh_move_errs[i] = err;
			sh->sh_move_errn[i]++;
			return;
		}
	}
}

/* Every encrypted dataset's key must be unavailable. */
static void
check_keys_unloaded(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	const char *names[] = { POOL "/enc", POOL "/enc@s1", POOL "/enc@s2",
	    POOL "/encclone", POOL "/rcv", POOL "/rcv@s1", POOL "/rcv@s2" };

	for (int i = 0; i < 7; i++) {
		dsl_dataset_t *ds;
		objset_t *os;
		int err;

		dsl_pool_config_enter(dp, FTAG);
		VERIFY0(dsl_dataset_hold(dp, names[i], FTAG, &ds));
		sh->sh_nkeys_checked++;
		if (dsl_dataset_get_keystatus(ds->ds_dir) ==
		    ZFS_KEYSTATUS_UNAVAILABLE)
			sh->sh_nkeys_unavail++;
		dsl_dataset_rele(ds, FTAG);
		dsl_pool_config_exit(dp, FTAG);
		/* And nothing of it can be read decrypted. */
		err = dmu_objset_hold_flags(names[i], B_TRUE, FTAG, &os);
		if (err == 0)
			dmu_objset_rele_flags(os, B_TRUE, FTAG);
		(void) printf("INFO key check: %s: decrypting hold returns "
		    "%d\n", names[i], err);
	}
}

static boolean_t
skipped_by_options(const mv_t *m)
{
	if (opt_nodva && m->mv_dva != 0)
		return (B_TRUE);
	return (B_FALSE);
}

static int
child_move(void *arg)
{
	mv_t *mv = malloc(MAXMV * sizeof (mv_t));
	raidz_redirect_move_t *rdm = calloc(BATCH, sizeof (*rdm));
	census_t *c = calloc(1, sizeof (*c));
	int idx[BATCH];
	spa_t *spa;
	int nmv;
	hrtime_t t0;
	uint64_t bytes = 0;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spike_redirect_live_objset = !opt_noroot;
	spike_redirect_findbp_raw = !opt_noraw;
	VERIFY0(try_import(dir, &spa));
	check_keys_unloaded(spa);
	check(sh->sh_nkeys_unavail == sh->sh_nkeys_checked, "every encrypted "
	    "dataset's key is unavailable during the move (%d of %d)",
	    sh->sh_nkeys_unavail, sh->sh_nkeys_checked);

	space_settle(spa);
	print_space(spa, "before the move");
	sh->sh_gap_before = sh->sh_space_gap;

	t0 = gethrtime();
	run_census(spa, c, mv, MAXMV, &nmv);
	(void) printf("INFO move: the census found %d old DVAs in %.2f s\n",
	    nmv, (double)(gethrtime() - t0) / 1e9);

	t0 = gethrtime();
	for (int i = 0; i < nmv; ) {
		int n = 0;

		while (i < nmv && n < BATCH) {
			mv_t *m = &mv[i];
			int err;

			if (m->mv_zb.zb_objset == DMU_META_OBJSET ||
			    m->mv_class[C_ZIL]) {
				i++;
				continue;
			}
			if (skipped_by_options(m)) {
				for (int k = 0; k < NCLASS; k++)
					sh->sh_skipped[k] += m->mv_class[k];
				i++;
				continue;
			}
			memset(&rdm[n], 0, sizeof (rdm[n]));
			rdm[n].rdm_bp = &m->mv_bp;
			rdm[n].rdm_zb = &m->mv_zb;
			rdm[n].rdm_psize = m->mv_psize;
#ifdef	RAIDZ_REDIRECT_MOVE_DVA
			rdm[n].rdm_dva = m->mv_dva;
#else
			if (m->mv_dva != 0) {
				/* This move cannot name any other DVA. */
				for (int k = 0; k < NCLASS; k++)
					sh->sh_refused[k] += m->mv_class[k];
				note_error(-1);
				i++;
				continue;
			}
#endif
			rdm[n].rdm_data = read_raw(spa, &m->mv_bp, m->mv_dva,
			    m->mv_psize, &err);
			VERIFY0(err);
			bytes += m->mv_psize;
			idx[n++] = i++;
		}
		if (n == 0)
			continue;
		VERIFY0(vdev_raidz_redirect_move(spa, 0, rdm, n,
		    RAIDZ_REDIRECT_KEEP_FIRST, B_FALSE));
		for (int j = 0; j < n; j++) {
			mv_t *m = &mv[idx[j]];

			for (int k = 0; k < NCLASS; k++) {
				if (rdm[j].rdm_error == 0)
					sh->sh_moved[k] += m->mv_class[k];
				else
					sh->sh_refused[k] += m->mv_class[k];
			}
			if (rdm[j].rdm_error != 0)
				note_error(rdm[j].rdm_error);
			abd_free(rdm[j].rdm_data);
		}
	}
	space_settle(spa);
	print_space(spa, "after the move");
	(void) printf("INFO space: the move grew the gap between what the vdev "
	    "holds and what the DSL and the moved-block charge account for "
	    "by %lld bytes (deflated), for %llu bytes of moved blocks as "
	    "stored\n", (longlong_t)(sh->sh_space_gap - sh->sh_gap_before),
	    (u_longlong_t)bytes);
	check(sh->sh_space_gap == sh->sh_gap_before, "the move leaves no "
	    "space unaccounted (%lld bytes, deflated)",
	    (longlong_t)(sh->sh_space_gap - sh->sh_gap_before));
	(void) printf("INFO move: %llu bytes moved in %.2f s\n",
	    (u_longlong_t)bytes, (double)(gethrtime() - t0) / 1e9);
	(void) printf("INFO move: %-36s %8s %8s %8s\n", "class", "moved",
	    "refused", "skipped");
	for (int k = 0; k < NCLASS; k++) {
		if (sh->sh_moved[k] + sh->sh_refused[k] + sh->sh_skipped[k] ==
		    0)
			continue;
		(void) printf("INFO move: %-36s %8llu %8llu %8llu\n",
		    class_names[k], (u_longlong_t)sh->sh_moved[k],
		    (u_longlong_t)sh->sh_refused[k],
		    (u_longlong_t)sh->sh_skipped[k]);
	}
	for (int i = 0; i < 8 && sh->sh_move_errn[i] != 0; i++) {
		(void) printf("INFO move: error %d (%s) for %d DVAs\n",
		    sh->sh_move_errs[i], sh->sh_move_errs[i] == -1 ?
		    "DVA other than 0, not expressible" :
		    strerror(sh->sh_move_errs[i]), sh->sh_move_errn[i]);
	}
	export_pool(spa);
	kernel_fini();
	free(mv);
	free(rdm);
	free(c);
	return (0);
}

/* ---- scrub ---- */

static int
child_scrub(void *arg)
{
	spa_t *spa;
	vdev_t *rvd;
	pool_scan_stat_t ps;
	uint64_t cks = 0;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(try_import(dir, &spa));
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	for (;;) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		VERIFY0(spa_scan_get_stats(spa, &ps));
		if (ps.pss_state == DSS_FINISHED ||
		    ps.pss_state == DSS_CANCELED)
			break;
		(void) usleep(100000);
	}
	rvd = spa->spa_root_vdev->vdev_child[0];
	for (int c = 0; c < rvd->vdev_children; c++)
		cks += rvd->vdev_child[c]->vdev_stat.vs_checksum_errors;
	(void) printf("INFO scrub: examined %llu, issued %llu, errors %llu, "
	    "repaired %llu, checksum errors %llu\n",
	    (u_longlong_t)ps.pss_examined, (u_longlong_t)ps.pss_issued,
	    (u_longlong_t)ps.pss_errors, (u_longlong_t)ps.pss_processed,
	    (u_longlong_t)cks);
	check(ps.pss_state == DSS_FINISHED && ps.pss_errors == 0 &&
	    ps.pss_processed == 0 && cks == 0, "a scrub finishes with no "
	    "error and repairs nothing, no key loaded");
	export_pool(spa);
	kernel_fini();
	return (0);
}

/* ---- the run ---- */

static void
report_residual(const char *when, boolean_t want_zero)
{
	const census_t *c = &sh->sh_census;

	if (want_zero) {
		check(c->c_n[C_ALL][ST_OLD] == 0 && c->c_err == 0 &&
		    sh->sh_pending_old == 0, "census %s: no DVA left at the "
		    "old geometry (%llu reachable, %llu pending frees)", when,
		    (u_longlong_t)c->c_n[C_ALL][ST_OLD],
		    (u_longlong_t)sh->sh_pending_old);
	}
}

int
main(int argc, char **argv)
{
	int leaks = 0;

	if (argc < 3) {
		(void) fprintf(stderr, "usage: crypt_probe <dir> <ashift> "
		    "[nodva] [noroot] [noraw] [nomos] [keep]\n");
		return (2);
	}
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 0);
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "nodva") == 0)
			opt_nodva = B_TRUE;
		else if (strcmp(argv[i], "noroot") == 0)
			opt_noroot = B_TRUE;
		else if (strcmp(argv[i], "nomos") == 0)
			opt_nomos = B_TRUE;
		else if (strcmp(argv[i], "keep") == 0)
			opt_keep = B_TRUE;
		else if (strcmp(argv[i], "noraw") == 0)
			opt_noraw = B_TRUE;
		else
			return (2);
	}
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	sh = mmap(NULL, sizeof (*sh), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	VERIFY3P(sh, !=, MAP_FAILED);
	memset(sh, 0, sizeof (*sh));
#ifdef	RAIDZ_REDIRECT_MOVE_DVA
	(void) printf("INFO the move takes a DVA index\n");
#else
	(void) printf("INFO the move only moves DVA 0\n");
#endif
	(void) printf("INFO ashift %llu%s%s%s%s\n", (u_longlong_t)ashift,
	    opt_nodva ? ", nodva" : "", opt_noroot ? ", noroot" : "",
	    opt_nomos ? ", nomos" : "", opt_noraw ? ", noraw" : "");

	VERIFY0(phase("0 setup", child_setup, NULL));
	(void) phase("census before", child_census, (void *)"before");
	check(sh->sh_census.c_n[C_ENC_DATA][ST_OLD] > 0 &&
	    sh->sh_census.c_n[C_RECV][ST_OLD] > 0 &&
	    sh->sh_census.c_n[C_SNAP][ST_OLD] > 0 &&
	    sh->sh_census.c_n[C_DVA1][ST_OLD] > 0 &&
	    sh->sh_census.c_n[C_GANG_HDR][ST_OLD] > 0 &&
	    sh->sh_census.c_n[C_DEDUP][ST_OLD] > 0 &&
	    sh->sh_census.c_embedded > 0,
	    "the pool has old encrypted, raw-received, snapshot, second-copy, "
	    "gang, dedup and embedded blocks");
	oracle("before", B_FALSE);
	check(dva_oracle("before") > 0, "dva oracle before: some DVA does "
	    "not survive two damaged children");

	if (!opt_nomos) {
		(void) phase("mos", child_mos, NULL);
		(void) phase("census after mos", child_census,
		    (void *)"after-mos");
		check(sh->sh_census.c_n[C_MOS][ST_OLD] == 0,
		    "no MOS DVA left at the old geometry (%llu), %llu buffers "
		    "dirtied", (u_longlong_t)sh->sh_census.c_n[C_MOS][ST_OLD],
		    (u_longlong_t)sh->sh_mos_dirtied);
		oracle("mos-only", B_FALSE);
	}

	(void) phase("move", child_move, NULL);
	(void) phase("census after move", child_census, (void *)"after-move");

	report_residual("after the move", B_TRUE);
	(void) phase("read", child_read, NULL);
	check(run_zdb(dir, "zdb on the pool", &leaks) == 0 && leaks == 1,
	    "zdb -bcc on the pool: every block read, no leak");
	(void) phase("scrub", child_scrub, NULL);
	(void) phase("census after scrub", child_census,
	    (void *)"after-scrub");
	report_residual("after the scrub", B_TRUE);
	oracle("after", B_TRUE);
	check(dva_oracle("after") == 0, "dva oracle after: every DVA met, "
	    "under all %d pairs, and every one reads raw under each", NPAIRS);

	(void) printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS",
	    failures);
	return (failures ? 1 : 0);
}
