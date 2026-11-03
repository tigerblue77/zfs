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
/*
 * Spike (throwaway, not part of any tree): the module's test-only mover
 * (module/zfs/vdev_raidz_mover.c), run in userspace through the same entry
 * points as the kernel's raidz_spike_mover parameter, on an exported pool
 * of file vdevs, such as the common fixtures of contrib/raidz-parity-fixtures.
 * Its results are the reference a kernel run of the same input is compared
 * with.
 *
 *   kmover_user <dir> <pool> <rw|ro> <command> [<command> ...]
 *
 * <dir> holds the pool's members, found by scanning it; no cache file is
 * written outside it.  Each command is one argument: "rewrite-feature"
 * enables feature@physical_rewrite, as "zpool set
 * feature@physical_rewrite=enabled" does; "fill <MiB> [zil]" creates the
 * sparse volume <pool>/fill (volblocksize 128K, compression off) and writes
 * that much pseudo-random data into it, as "zfs create -s -V" and dd do,
 * with zil through the volume's intent log as the kernel's volume writes
 * and an fsync commit it; "volume-minor" does with the log of <pool>/fill
 * what the kernel does when it creates the volume's minor on an import;
 * "destroy <dataset>" destroys <pool>/<dataset>, as "zfs destroy" does;
 * "sync" waits for a txg to sync.  Any other command is written to the
 * mover with the pool name inserted after its first word ("epoch", "move
 * hybrid", "census") and its result line printed.  A rw pool is exported
 * at the end.  Exit status: 0 if every command returned rc=0, 1 if one did
 * not, 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zfeature.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_destroy.h>
#include <sys/dsl_prop.h>
#include <sys/zap.h>
#include <sys/zvol.h>
#include <libzutil.h>
#include <libzpool.h>

static int
rw_feature_check(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	if (spa_feature_is_enabled(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE))
		return (SET_ERROR(EEXIST));
	return (0);
}

static void
rw_feature_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_feature_enable(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE, tx);
}

#define	FILL_BLOCK	(128ULL << 10)
#define	FILL_CHUNK	(1ULL << 20)
#define	FILL_VOLSIZE	(1ULL << 30)

/* zvol_create_cb(): the volume's data object and its "size". */
static void
fill_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	uint64_t volsize = FILL_VOLSIZE;

	(void) arg, (void) cr;
	VERIFY0(dmu_object_claim(os, ZVOL_OBJ, DMU_OT_ZVOL, FILL_BLOCK,
	    DMU_OT_NONE, 0, tx));
	VERIFY0(zap_create_claim(os, ZVOL_ZAP_OBJ, DMU_OT_ZVOL_PROP,
	    DMU_OT_NONE, 0, tx));
	VERIFY0(zap_update(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize, tx));
}

/* zvol_get_done() */
static void
fill_get_done(zgd_t *zgd, int error)
{
	(void) error;
	if (zgd->zgd_db != NULL)
		dmu_buf_rele(zgd->zgd_db, zgd);
	kmem_free(zgd, sizeof (zgd_t));
}

/* zvol_get_data(), without the range lock: one writer, done by then. */
static int
fill_get_data(void *arg, uint64_t arg2, lr_write_t *lr, char *buf,
    struct lwb *lwb, zio_t *zio)
{
	objset_t *os = arg;
	zgd_t *zgd = kmem_zalloc(sizeof (zgd_t), KM_SLEEP);
	uint64_t offset = lr->lr_offset;
	dmu_buf_t *db;
	int error;

	(void) arg2;
	zgd->zgd_lwb = lwb;
	if (buf != NULL) {
		error = dmu_read(os, ZVOL_OBJ, offset, lr->lr_length, buf,
		    DMU_READ_NO_PREFETCH);
	} else {
		offset = P2ALIGN_TYPED(offset, FILL_BLOCK, uint64_t);
		error = dmu_buf_hold_noread(os, ZVOL_OBJ, offset, zgd, &db);
		if (error == 0) {
			zgd->zgd_db = db;
			zgd->zgd_bp = &lr->lr_blkptr;
			error = dmu_sync(zio, lr->lr_common.lrc_txg,
			    fill_get_done, zgd);
			if (error == 0)
				return (0);
		}
	}
	fill_get_done(zgd, error);
	return (error);
}

static int
fill_replay_none(void *arg1, void *arg2, boolean_t byteswap)
{
	(void) arg1, (void) arg2, (void) byteswap;
	return (SET_ERROR(ENOTSUP));
}

static zil_replay_func_t *const fill_replay_vector[TX_MAX_TYPE] = {
	[0 ... TX_MAX_TYPE - 1] = fill_replay_none,
};

/*
 * What the kernel does with a volume's intent log when it creates its
 * minor, on "zfs create -V" and on every import: zvol_os_create_minor()
 * opens the log and replays it, which destroys a clean one but its first
 * block.  B_FALSE if <pool>/fill does not exist.
 */
static boolean_t
fill_minor(const char *pool)
{
	char name[ZFS_MAX_DATASET_NAME_LEN];
	zilog_t *zilog;
	objset_t *os;

	(void) snprintf(name, sizeof (name), "%s/fill", pool);
	if (dmu_objset_own(name, DMU_OST_ZVOL, B_TRUE, B_TRUE, FTAG,
	    &os) != 0)
		return (B_FALSE);
	zilog = zil_open(os, fill_get_data, NULL);
	if (zil_replay(os, os, fill_replay_vector))
		zil_close(zilog);
	dmu_objset_disown(os, B_TRUE, FTAG);
	return (B_TRUE);
}

/*
 * dd if=/dev/urandom bs=1M count=<mib> conv=fsync into a new volume.  With
 * zil, as the kernel's volume does: each write is logged (zvol_log_write())
 * and the fsync commits the log, whose large records are indirect, their
 * blocks written by dmu_sync() (zvol_get_data()).
 */
static int
fill(const char *pool, uint64_t mib, boolean_t zil)
{
	char name[ZFS_MAX_DATASET_NAME_LEN];
	uint64_t *buf = kmem_alloc(FILL_CHUNK, KM_SLEEP);
	uint64_t x = 0x9e3779b97f4a7c15ULL, off = 0;
	zilog_t *zilog = NULL;
	objset_t *os;
	int err;

	(void) snprintf(name, sizeof (name), "%s/fill", pool);
	err = dmu_objset_create(name, DMU_OST_ZVOL, 0, NULL, fill_create_cb,
	    NULL);
	if (err == 0) {
		err = dsl_prop_set_int(name, "compression", ZPROP_SRC_LOCAL,
		    ZIO_COMPRESS_OFF);
	}
	if (err == 0 && zil)
		VERIFY(fill_minor(pool));
	if (err == 0)
		err = dmu_objset_own(name, DMU_OST_ZVOL, B_FALSE, B_TRUE,
		    FTAG, &os);
	if (err != 0) {
		kmem_free(buf, FILL_CHUNK);
		(void) printf("RESULT fill bytes=0 rc=%d\n", err);
		return (1);
	}
	if (zil)
		zilog = zil_open(os, fill_get_data, NULL);
	for (; err == 0 && off < MIN(mib << 20, FILL_VOLSIZE);
	    off += FILL_CHUNK) {
		dmu_tx_t *tx = dmu_tx_create(os);

		for (size_t i = 0; i < FILL_CHUNK / sizeof (*buf); i++) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			buf[i] = x;
		}
		dmu_tx_hold_write(tx, ZVOL_OBJ, off, FILL_CHUNK);
		if ((err = dmu_tx_assign(tx, DMU_TX_WAIT)) != 0) {
			dmu_tx_abort(tx);
			break;
		}
		dmu_write(os, ZVOL_OBJ, off, FILL_CHUNK, buf, tx,
		    DMU_READ_NO_PREFETCH);
		for (uint64_t o = off; zil && o < off + FILL_CHUNK; ) {
			itx_wr_state_t ws = zil_write_state(zilog,
			    off + FILL_CHUNK - o, FILL_BLOCK, B_FALSE, B_FALSE);
			uint64_t len = off + FILL_CHUNK - o;
			itx_t *itx;
			lr_write_t *lr;

			if (ws == WR_COPIED)
				ws = WR_NEED_COPY;
			else if (ws == WR_INDIRECT)
				len = MIN(len, FILL_BLOCK - P2PHASE(o,
				    FILL_BLOCK));
			itx = zil_itx_create(TX_WRITE, sizeof (*lr));
			lr = (lr_write_t *)&itx->itx_lr;
			itx->itx_wr_state = ws;
			lr->lr_foid = ZVOL_OBJ;
			lr->lr_offset = o;
			lr->lr_length = len;
			lr->lr_blkoff = 0;
			BP_ZERO(&lr->lr_blkptr);
			itx->itx_private = os;
			zil_itx_assign(zilog, itx, tx);
			o += len;
		}
		dmu_tx_commit(tx);
	}
	if (zil) {
		int cerr = zil_commit(zilog, ZVOL_OBJ);

		(void) printf("RESULT fill zil_commit rc=%d\n", cerr);
		zil_close(zilog);
	}
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	kmem_free(buf, FILL_CHUNK);
	(void) printf("RESULT fill bytes=%llu rc=%d\n", (u_longlong_t)off,
	    err);
	return (0);
}

static int
run(const char *pool, const char *cmd)
{
	char req[1024], res[4096];
	const char *sp = strchr(cmd, ' ');
	size_t w = sp == NULL ? strlen(cmd) : (size_t)(sp - cmd);
	int err;

	(void) snprintf(req, sizeof (req), "%.*s %s%s", (int)w, cmd, pool,
	    sp == NULL ? "" : sp);
	if ((err = vdev_raidz_spike_mover_start(req)) != 0) {
		(void) printf("START %s: error %d\n", req, err);
		return (1);
	}
	for (;;) {
		(void) vdev_raidz_spike_mover_status(res, sizeof (res));
		if (strncmp(res, "running ", 8) != 0)
			break;
		(void) usleep(100000);
	}
	(void) printf("RESULT %s", res);
	return (strstr(res, " rc=0\n") != NULL ? 0 : 1);
}

int
main(int argc, char **argv)
{
	importargs_t ia = { 0 };
	char *paths[1];
	char cache[4096];
	nvlist_t *config = NULL, *props = NULL;
	libpc_handle_t lpch = {
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	boolean_t ro;
	int rc = 0;

	if (argc < 5 || (strcmp(argv[3], "rw") != 0 &&
	    strcmp(argv[3], "ro") != 0)) {
		(void) fprintf(stderr, "usage: %s <dir> <pool> <rw|ro> "
		    "<command> [<command> ...]\n", argv[0]);
		return (2);
	}
	ro = strcmp(argv[3], "ro") == 0;
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(ro ? SPA_MODE_READ : SPA_MODE_READ | SPA_MODE_WRITE);
	paths[0] = argv[1];
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&lpch, argv[2], &config, &ia));
	if (!ro) {
		props = fnvlist_alloc();
		fnvlist_add_string(props, "cachefile", "none");
	}
	VERIFY0(spa_import(argv[2], config, props, ro ?
	    ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	if (props != NULL)
		fnvlist_free(props);
	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "rewrite-feature") == 0) {
			int err = dsl_sync_task(argv[2], rw_feature_check,
			    rw_feature_sync, NULL, 0, ZFS_SPACE_CHECK_NONE);

			(void) printf("RESULT rewrite-feature rc=%d\n",
			    err == EEXIST ? 0 : err);
			if (err != 0 && err != EEXIST)
				rc = 1;
			continue;
		}
		if (strncmp(argv[i], "fill ", 5) == 0) {
			char *end;
			uint64_t mib = strtoull(argv[i] + 5, &end, 10);

			rc |= fill(argv[2], mib, strcmp(end, " zil") == 0);
			continue;
		}
		if (strcmp(argv[i], "sync") == 0) {
			spa_t *spa;

			VERIFY0(spa_open(argv[2], &spa, FTAG));
			txg_wait_synced(spa_get_dsl(spa), 0);
			spa_close(spa, FTAG);
			continue;
		}
		if (strcmp(argv[i], "volume-minor") == 0) {
			(void) printf("RESULT volume-minor found=%d\n",
			    fill_minor(argv[2]));
			continue;
		}
		if (strncmp(argv[i], "destroy ", 8) == 0) {
			char name[ZFS_MAX_DATASET_NAME_LEN];
			int err;

			(void) snprintf(name, sizeof (name), "%s/%s", argv[2],
			    argv[i] + 8);
			err = dsl_destroy_head(name);
			(void) printf("RESULT destroy %s rc=%d\n", name, err);
			rc |= err != 0;
			continue;
		}
		rc |= run(argv[2], argv[i]);
	}
	/* As the generator: a read-only import is left to kernel_fini(). */
	if (!ro)
		VERIFY0(spa_export(argv[2], NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (rc);
}
