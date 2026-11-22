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
 * Copyright (c) 2026 by the OpenZFS project.  All rights reserved.
 */

/*
 * A device removal whose last step waits for its suspended pool while the
 * pool is being loaded must not leave its hold in spa_minref.
 *
 * spa_load_impl() restarts a removal in progress after it has dropped
 * spa_namespace_lock, and retakes the lock at its end; spa_load() then
 * sets spa_minref from the reference count.  If the removal completes in
 * between and the pool is suspended, spa_vdev_exit() holds the pool,
 * drops the lock and waits for the sync.  The load counts that hold in
 * spa_minref; once it is dropped, the reference count of the idle pool
 * stays below spa_minref, and every export fails with EBUSY.
 *
 * The program does this in libzpool, on two file vdevs in the directory
 * it is given: it pauses the removal of the second vdev, exports the pool
 * and imports it with a load delay (zinject -P import).  While the load
 * sleeps in its unlocked window, it holds SCL_CONFIG as reader, lets the
 * restarted removal run, which stops in spa_vdev_enter() with the
 * namespace lock held, waiting for the config lock as writer; it has a
 * sync started, which waits for the config lock too, and waits for the
 * delay to end, so that the load waits for the namespace lock.  It then
 * marks the pool suspended, without failing any I/O, and releases the
 * reader: the removal's first step syncs in that sync, its last step
 * finds the pool suspended and drops the namespace lock, and the load
 * takes it.  Once spa_minref is set, the program resumes the pool; once
 * the import has returned and the removal is done, it checks that the
 * reference count never goes below spa_minref and that the pool exports.
 *
 * Exit status: 0 pass, 1 wrong spa_minref or export failed, 2 setup or
 * sequence error, 3 something did not return in time.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/txg.h>
#include <sys/txg_impl.h>
#include <sys/dsl_pool.h>
#include <sys/zio.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzpool.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define	POOL	"rsl_pool"
#define	DELAY	10

static char dir[MAXPATHLEN];
static double t0;
static spa_t *lspa;
static nvlist_t *config;
static volatile int import_done;
static volatile int import_err = -1;

static double
now(void)
{
	struct timeval tv;

	(void) gettimeofday(&tv, NULL);
	return (tv.tv_sec + tv.tv_usec / 1e6);
}

static void
say(const char *fmt, ...)
{
	va_list ap;

	(void) printf("[%7.3f] ", now() - t0);
	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
	(void) printf("\n");
	(void) fflush(stdout);
}

static void
bail(int rc, const char *what)
{
	say("%s: %s", rc == 3 ? "TIMEOUT" : "ERROR", what);
	_exit(rc);
}

static nvlist_t *
make_leaf(const char *name)
{
	nvlist_t *leaf = fnvlist_alloc();
	char *path;
	int fd;

	VERIFY3S(asprintf(&path, "%s/%s", dir, name), >, 0);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0 ||
	    ftruncate(fd, 256ULL << 20) != 0)
		bail(2, "cannot create the vdev files");
	(void) close(fd);
	fnvlist_add_string(leaf, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(leaf, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(leaf, ZPOOL_CONFIG_ASHIFT, 12);
	free(path);
	return (leaf);
}

static void
create_pool(void)
{
	nvlist_t *root = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();
	nvlist_t *child[2];

	child[0] = make_leaf("vdev0");
	child[1] = make_leaf("vdev1");
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, 2);
	for (int i = 0; i < SPA_FEATURES; i++) {
		char *name;

		VERIFY3S(asprintf(&name, "feature@%s",
		    spa_feature_table[i].fi_uname), >, 0);
		fnvlist_add_uint64(props, name, 0);
		free(name);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(child[0]);
	fnvlist_free(child[1]);
	fnvlist_free(root);
	fnvlist_free(props);
}

/* 32 MiB in 128 KiB blocks, so that both vdevs hold some of it. */
static void
write_data(void)
{
	char fs[ZFS_MAX_DATASET_NAME_LEN];
	size_t bs = 128 << 10;
	char *buf = malloc(bs);
	objset_t *os;
	dmu_tx_t *tx;

	VERIFY3P(buf, !=, NULL);
	for (size_t i = 0; i < bs; i++)
		buf[i] = (char)(i * 7 + 3);
	(void) snprintf(fs, sizeof (fs), "%s/fs", POOL);
	VERIFY0(dmu_objset_create(fs, DMU_OST_OTHER, 0, NULL, NULL, NULL));
	VERIFY0(dmu_objset_own(fs, DMU_OST_OTHER, B_FALSE, B_TRUE, FTAG,
	    &os));
	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	uint64_t obj = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, bs,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	for (uint64_t off = 0; off < (32ULL << 20); off += bs) {
		tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, obj, off, bs);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, obj, off, bs, buf, tx, 0);
		dmu_tx_commit(tx);
	}
	txg_wait_synced(dmu_objset_pool(os), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(buf);
}

static void
tunable(const char *opt)
{
	VERIFY0(handle_tunable_option(opt, B_TRUE));
	say("%s", opt);
}

static void *
importer(void *arg)
{
	(void) arg;
	import_err = spa_import((char *)POOL, config, NULL, 0);
	import_done = 1;
	say("import returned %d", import_err);
	return (NULL);
}

/* The pool being imported, found without spa_lookup(), which waits. */
static spa_t *
find_loading(void)
{
	double s = now();

	while (now() - s < 30) {
		spa_t *spa = NULL;

		spa_namespace_enter(FTAG);
		while ((spa = spa_next(spa)) != NULL) {
			if (strcmp(spa_name(spa), POOL) == 0 &&
			    spa->spa_load_thread != NULL)
				break;
		}
		spa_namespace_exit(FTAG);
		if (spa != NULL)
			return (spa);
		(void) usleep(1000);
	}
	return (NULL);
}

/* Asks for a sync, which spa_sync() starts and then waits in. */
static void *
syncer(void *arg)
{
	txg_wait_synced(spa_get_dsl(arg), 0);
	return (NULL);
}

static void
show(const char *when)
{
	say("%s: refcount %lld, spa_minref %d", when,
	    (longlong_t)zfs_refcount_count(&lspa->spa_refcount),
	    lspa->spa_minref);
}

int
main(int argc, char **argv)
{
	spa_config_lock_t *scl;
	tx_state_t *tx;
	pthread_t tid, sid;
	uint64_t syncing;
	spa_t *spa;
	double s;
	int id, err, rc = 0;

	if (argc != 2) {
		(void) fprintf(stderr, "usage: %s <directory>\n", argv[0]);
		return (2);
	}
	t0 = now();
	(void) strlcpy(dir, argv[1], sizeof (dir));
	VERIFY3S(asprintf((char **)&spa_config_path, "%s/zpool.cache",
	    dir), >, 0);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	create_pool();
	write_data();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	tunable("zfs_removal_suspend_progress=1");
	VERIFY0(spa_vdev_remove(spa,
	    spa->spa_root_vdev->vdev_child[1]->vdev_guid, B_FALSE));
	spa_close(spa, FTAG);
	VERIFY0(spa_export(POOL, &config, B_FALSE, B_FALSE));
	say("pool exported with the removal of vdev 1 in progress");

	zinject_record_t rec;
	(void) memset(&rec, 0, sizeof (rec));
	rec.zi_cmd = ZINJECT_DELAY_IMPORT;
	rec.zi_duration = DELAY;
	VERIFY0(zio_inject_fault((char *)POOL, 0, &id, &rec));

	double t_import = now();
	VERIFY0(pthread_create(&tid, NULL, importer, NULL));
	if ((lspa = find_loading()) == NULL)
		bail(2, "no pool being loaded");
	s = now();
	while (lspa->spa_vdev_removal == NULL ||
	    lspa->spa_vdev_removal->svr_thread == NULL) {
		if (now() - s > 30)
			bail(3, "the load did not restart the removal");
		(void) usleep(1000);
	}
	say("the load restarted the removal (paused)");

	/*
	 * Let the load reach its delay, past the steps that take the config
	 * lock as reader, then keep the removal's spa_vdev_enter() from
	 * taking it as writer.
	 */
	(void) sleep(2);
	scl = &lspa->spa_config_lock[0];
	spa_config_enter(lspa, SCL_CONFIG, FTAG, RW_READER);
	tunable("zfs_removal_suspend_progress=0");
	s = now();
	while (scl->scl_write_wanted == 0) {
		if (now() - s > 30)
			bail(3, "the removal did not reach spa_vdev_enter()");
		(void) usleep(1000);
	}
	if (lspa->spa_load_thread == NULL)
		bail(2, "the load ended before the removal waited");
	say("the removal waits for the config lock with the namespace lock");

	/*
	 * A suspended pool starts no sync.  Have one started now: it waits
	 * for the config lock in spa_sync(), and the removal's first step
	 * joins its txg.  A txg that stays in sync for a second is that one.
	 */
	VERIFY0(pthread_create(&sid, NULL, syncer, lspa));
	tx = &spa_get_dsl(lspa)->dp_tx;
	s = now();
	for (;;) {
		syncing = tx->tx_syncing_txg;
		(void) sleep(1);
		if (syncing != 0 && tx->tx_syncing_txg == syncing)
			break;
		if (now() - s > 30)
			bail(3, "no sync waits for the config lock");
	}
	say("txg %llu waits for the config lock", (u_longlong_t)syncing);

	/* The delay is over: the load waits for the namespace lock. */
	while (now() < t_import + DELAY + 2)
		(void) usleep(10000);
	if (lspa->spa_load_thread == NULL)
		bail(2, "the load ended while the removal held the lock");
	zio_suspend(lspa, NULL, ZIO_SUSPEND_IOERR);
	spa_config_exit(lspa, SCL_CONFIG, FTAG);
	say("pool marked suspended, config lock released");

	/* Once the load has set spa_minref, the pool can resume. */
	s = now();
	while (lspa->spa_load_state != SPA_LOAD_NONE) {
		if (now() - s > 60)
			bail(3, "the load did not end");
		(void) usleep(1000);
	}
	show("load ended");
	zio_resume(lspa);
	say("pool resumed");

	s = now();
	while (!import_done) {
		if (now() - s > 60)
			bail(3, "the import did not return");
		(void) usleep(10000);
	}
	if (import_err != 0)
		bail(2, "the import failed");
	(void) pthread_join(sid, NULL);

	for (s = now(); ; ) {
		boolean_t done = B_FALSE;

		spa_namespace_enter(FTAG);
		if (lspa->spa_vdev_removal == NULL &&
		    mutex_tryenter(&lspa->spa_vdev_top_lock)) {
			mutex_exit(&lspa->spa_vdev_top_lock);
			done = B_TRUE;
		}
		spa_namespace_exit(FTAG);
		if (done)
			break;
		if (now() - s > 60)
			bail(3, "the removal did not end");
		(void) usleep(10000);
	}
	txg_wait_synced(spa_get_dsl(lspa), 0);

	/*
	 * The async tasks of the import may still hold the pool for a
	 * while; nothing can take the count below spa_minref.
	 */
	for (s = now(); ; ) {
		int64_t count;

		spa_evicting_os_wait(lspa);
		spa_namespace_enter(FTAG);
		count = zfs_refcount_count(&lspa->spa_refcount);
		spa_namespace_exit(FTAG);
		if (count <= lspa->spa_minref || now() - s > 30)
			break;
		(void) usleep(10000);
	}
	show("removal done");
	if (zfs_refcount_count(&lspa->spa_refcount) < lspa->spa_minref) {
		say("FAIL: the reference count is below spa_minref");
		rc = 1;
	}
	err = spa_export(POOL, NULL, B_FALSE, B_FALSE);
	say("export returned %d", err);
	if (err != 0)
		rc = 1;
	if (rc != 0)
		_exit(rc);

	(void) pthread_join(tid, NULL);
	kernel_fini();
	say("PASS");
	return (0);
}
