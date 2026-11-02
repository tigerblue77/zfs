// SPDX-License-Identifier: CDDL-1.0
/*
 * Userspace (libzpool) reproduction: a vdev configuration change whose own
 * txg sync suspends the pool, and what that does to spa_namespace_lock.
 *
 * Usage: vdev_exit_suspend <scratch dir> [three|two|all] [timeout s]
 *
 *   three   (default) zpool detach of m3 from mirror(m1, m2, m3), with
 *           every write and every probe of m1 and m2 failing ("zinject -d
 *           mN -e io -T write" and "-e nxio -T probe").  m3 has no
 *           handler, so vdev_dtl_required(m3) lets the detach through.
 *   two     zpool detach of m2 from mirror(m1, m2), handlers on m1 only.
 *           The mirror collapses and m1 takes over the mirror's guid
 *           (vdev_remove_parent()), so its handlers stop matching and the
 *           sync does not fail.
 *   all     the recipe of the earlier VM script: handlers on every child.
 *           The ENXIO probe handler on the detached child makes
 *           vdev_dtl_required() true, so the detach is refused with EBUSY
 *           ("no valid replicas") before anything is synced.
 *
 * Steps (each wait is bounded by the timeout):
 *   1. create pool "vxs" and a second healthy pool "vxo", raise
 *      zfs_txg_timeout so that no background sync runs;
 *   2. register the zinject-equivalent handlers;
 *   3. run the vdev operation in a thread (as the ioctl does: spa_open,
 *      operation, spa_close);
 *   4. wait for the pool to suspend;
 *   5. probe spa_namespace_lock with spa_namespace_tryenter();
 *   6. spa_open() the other pool (what zpool status / zpool list do);
 *   7. zinject -c (zio_clear_fault) and zpool clear (zfs_ioc_clear's
 *      sequence) in a thread;
 *   8. wait for the vdev operation to return.
 * VXS_SKIP_ZINJECT_CLEAR=1 in the environment skips step 7's zinject -c,
 * to see where zpool clear alone stops (the handlers then stay, so the
 * clear cannot repair the pool).
 * A step that does not finish is reported as BLOCKED; the process then
 * dumps its threads with gdb (if present) and exits 2, since a blocked
 * kernel-style thread cannot be recovered in-process.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/zfs_ioctl.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <sys/dsl_pool.h>
#include <sys/zfs_debug.h>
#include <libzpool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define	POOL	"vxs"
#define	OTHER	"vxo"
#define	VSIZE	(128ULL << 20)

static int timeout_s = 20;
static char path[3][MAXPATHLEN], ov[MAXPATHLEN];
static const char *dir;
static uint64_t guid[3], dguid;
static int ids[8];
static int nids;
static const char *mode = "three";

static volatile int op_done, op_rc = -1;
static volatile int other_done, other_rc = -1;
static volatile int clear_done, clear_rc = -1;
static volatile int clear_phase;

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

static double t0;

#define	LOG(...) do { \
	(void) printf("[%7.2f] ", now() - t0); \
	(void) printf(__VA_ARGS__); \
	(void) printf("\n"); \
	(void) fflush(stdout); \
} while (0)

static nvlist_t *
mkleaf(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, VSIZE) != 0) {
		perror(path);
		exit(1);
	}
	(void) close(fd);
	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_ASHIFT, 9);
	return (nv);
}

static nvlist_t *
mkparent(const char *type, nvlist_t **child, int n)
{
	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, type);
	fnvlist_add_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, n);
	return (nv);
}

static void
inject(uint64_t guid, int iotype, int error)
{
	zinject_record_t rec = { 0 };
	int id;

	rec.zi_cmd = ZINJECT_DEVICE_FAULT;
	rec.zi_guid = guid;
	rec.zi_iotype = iotype;
	rec.zi_error = error;
	VERIFY0(zio_inject_fault((char *)POOL, 0, &id, &rec));
	ids[nids++] = id;
	LOG("handler %d: guid %llx iotype %d error %d", id,
	    (u_longlong_t)guid, iotype, error);
}

static void *
op_thread(void *arg)
{
	(void) arg;
	spa_t *spa;

	VERIFY0(spa_open(POOL, &spa, FTAG));
	op_rc = spa_vdev_detach(spa, dguid, 0, B_FALSE);
	spa_close(spa, FTAG);
	op_done = 1;
	return (NULL);
}

static void *
other_thread(void *arg)
{
	(void) arg;
	spa_t *spa;

	other_rc = spa_open(OTHER, &spa, FTAG);
	if (other_rc == 0)
		spa_close(spa, FTAG);
	other_done = 1;
	return (NULL);
}

/* zinject -c <id> for every handler, then zfs_ioc_clear()'s sequence. */
static void *
clear_thread(void *arg)
{
	(void) arg;
	spa_t *spa;

	clear_phase = 1;
	if (getenv("VXS_SKIP_ZINJECT_CLEAR") == NULL) {
		for (int i = 0; i < nids; i++)
			(void) zio_clear_fault(ids[i]);
	}
	clear_phase = 2;
	/* pool_status_check() */
	if ((clear_rc = spa_open(POOL, &spa, FTAG)) != 0)
		goto out;
	spa_close(spa, FTAG);
	clear_phase = 3;
	if ((clear_rc = spa_open(POOL, &spa, FTAG)) != 0)
		goto out;
	clear_phase = 4;
	spa_vdev_state_enter(spa, SCL_NONE);
	vdev_clear(spa, NULL);
	(void) spa_vdev_state_exit(spa, spa_suspended(spa) ?
	    NULL : spa->spa_root_vdev, 0);
	clear_phase = 5;
	clear_rc = zio_resume(spa);
	spa_close(spa, FTAG);
out:
	clear_done = 1;
	return (NULL);
}

static int
wait_flag(volatile int *flag, double secs)
{
	double end = now() + secs;
	while (!*flag && now() < end)
		(void) usleep(50000);
	return (*flag);
}

static void
die_blocked(const char *what)
{
	char cmd[256];

	LOG("BLOCKED: %s (clear phase %d, op done %d, other done %d)",
	    what, clear_phase, op_done, other_done);
	if (access("/usr/bin/gdb", X_OK) == 0) {
		(void) snprintf(cmd, sizeof (cmd), "gdb -p %d -batch -nx "
		    "-ex 'thread apply all bt' 2>/dev/null | "
		    "grep -E '^Thread|^#[0-9]+ ' > %s/stacks.txt",
		    (int)getpid(), dir);
		if (system(cmd) != 0)
			LOG("gdb failed");
		LOG("thread stacks in %s/stacks.txt", dir);
	}
	_exit(2);
}

int
main(int argc, char **argv)
{
	spa_t *spa, *ospa;
	nvlist_t *l[3], *mir, *root;
	int n, nfail;

	if (argc < 2) {
		(void) fprintf(stderr,
		    "usage: %s <dir> [three|two|all] [timeout]\n", argv[0]);
		return (1);
	}
	if (argc > 2)
		mode = argv[2];
	if (argc > 3)
		timeout_s = atoi(argv[3]);
	t0 = now();
	dir = argv[1];
	n = strcmp(mode, "two") == 0 ? 2 : 3;
	nfail = strcmp(mode, "all") == 0 ? n : n - 1;
	for (int c = 0; c < n; c++)
		(void) snprintf(path[c], sizeof (path[c]), "%s/m%d", dir,
		    c + 1);
	(void) snprintf(ov, sizeof (ov), "%s/other", argv[1]);

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	zfs_txg_timeout = 600;

	for (int c = 0; c < n; c++)
		l[c] = mkleaf(path[c]);
	mir = mkparent(VDEV_TYPE_MIRROR, l, n);
	root = mkparent(VDEV_TYPE_ROOT, &mir, 1);
	VERIFY0(spa_create(POOL, root, NULL, NULL, NULL, NULL));
	l[0] = mkleaf(ov);
	root = mkparent(VDEV_TYPE_ROOT, l, 1);
	VERIFY0(spa_create(OTHER, root, NULL, NULL, NULL, NULL));

	VERIFY0(spa_open(POOL, &spa, FTAG));
	VERIFY0(spa_open(OTHER, &ospa, FTAG));
	txg_wait_synced(spa_get_dsl(spa), 0);
	txg_wait_synced(spa_get_dsl(ospa), 0);
	spa_close(ospa, FTAG);

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	for (int c = 0; c < n; c++)
		guid[c] = tvd->vdev_child[c]->vdev_guid;
	dguid = guid[n - 1];
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	LOG("mode %s: pool %s = mirror of %d, detaching m%d (%llx), "
	    "synced txg %llu", mode, POOL, n, n, (u_longlong_t)dguid,
	    (u_longlong_t)spa_last_synced_txg(spa));

	for (int c = 0; c < nfail; c++) {
		inject(guid[c], ZINJECT_IOTYPE_WRITE, EIO);
		inject(guid[c], ZINJECT_IOTYPE_PROBE, ENXIO);
	}
	VERIFY(!spa_suspended(spa));

	pthread_t opt, ot, ct;
	VERIFY0(pthread_create(&opt, NULL, op_thread, NULL));

	/* 4. the pool suspends during the operation's own sync */
	double end = now() + timeout_s;
	while (!spa_suspended(spa) && !op_done && now() < end)
		(void) usleep(50000);
	LOG("pool suspended %d, operation returned %d (rc %d)",
	    spa_suspended(spa), op_done, op_rc);
	if (op_done && !spa_suspended(spa)) {
		LOG("RESULT: operation returned %d before any suspend", op_rc);
		goto cleanup;
	}

	/* 5. is spa_namespace_lock held? */
	boolean_t got = B_FALSE;
	end = now() + 3;
	while (!got && now() < end) {
		got = spa_namespace_tryenter(FTAG);
		if (!got)
			(void) usleep(50000);
	}
	if (got)
		spa_namespace_exit(FTAG);
	LOG("spa_namespace_lock %s (3 s of tryenter)",
	    got ? "free" : "HELD");

	/* 6. a command on the other pool */
	VERIFY0(pthread_create(&ot, NULL, other_thread, NULL));
	if (!wait_flag(&other_done, timeout_s)) {
		LOG("RESULT: spa_open(%s) blocked for %d s", OTHER, timeout_s);
		VERIFY0(pthread_create(&ct, NULL, clear_thread, NULL));
		if (!wait_flag(&clear_done, timeout_s))
			LOG("zinject -c / zpool clear blocked for %d s "
			    "(phase %d)", timeout_s, clear_phase);
		die_blocked("namespace held by the suspended operation");
	}
	LOG("spa_open(%s) returned %d", OTHER, other_rc);

	/* 7. recovery */
	VERIFY0(pthread_create(&ct, NULL, clear_thread, NULL));
	if (!wait_flag(&clear_done, timeout_s))
		die_blocked("zinject -c / zpool clear");
	LOG("zpool clear returned %d, suspended %d", clear_rc,
	    spa_suspended(spa));

	/* 8. the operation completes once the pool resumes */
	if (!wait_flag(&op_done, timeout_s))
		die_blocked("vdev operation after zpool clear");
	LOG("RESULT: operation returned %d after zpool clear", op_rc);
	VERIFY0(pthread_join(ot, NULL));
	VERIFY0(pthread_join(ct, NULL));

cleanup:
	VERIFY0(pthread_join(opt, NULL));
	for (int i = 0; i < nids; i++)
		(void) zio_clear_fault(ids[i]);
	if (spa_suspended(spa)) {
		LOG("pool still suspended, not destroying");
		_exit(3);
	}
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	LOG("detached child still in pool: %d, mirror children %llu, "
	    "synced txg %llu", spa_lookup_by_guid(spa, dguid, B_FALSE) != NULL,
	    (u_longlong_t)spa->spa_root_vdev->vdev_child[0]->vdev_children,
	    (u_longlong_t)spa_last_synced_txg(spa));
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	spa_close(spa, FTAG);
	VERIFY0(spa_destroy(POOL));
	VERIFY0(spa_destroy(OTHER));
	kernel_fini();
	LOG("done");
	return (0);
}
