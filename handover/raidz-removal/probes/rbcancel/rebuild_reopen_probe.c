/*
 * Scratch probe (not for upstream): replays, in userspace libzpool, the
 * sequence of ZTS functional/replacement/rebuild_reopen.ksh:
 *
 *   zfs_vdev_min_ms_count=64; four 1 GiB file vdevs and a spare, striped;
 *   64 MiB of data; then 3 rounds of
 *     replace -s file-2 -> spare-1, replace -s spare-1 -> file-2
 *   each one: suspend, replace -s, check a scrub is refused, start 4
 *   loops of "zpool scrub" (pool stats + spa_scan, 1-3 ms apart),
 *   unsuspend, poll "zpool status" every second for up to 120 s until
 *   the resilver is no longer in progress, stop the loops, fail if the
 *   pool history has a canceled rebuild, wait up to 60 s for the
 *   replacing vdev to go away.
 *
 * Usage: rebuild_reopen_probe <dir>
 * Prints one RESULT line; exit 0 = the test would pass.
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_rebuild.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define	POOL	"rbr"
#define	VSIZE	(1ULL << 30)

static char dir[900];
static spa_t *g_spa;

static char *
vpath(const char *n)
{
	static char path[8][1000];
	static int slot;
	char *p = path[slot++ & 7];

	(void) snprintf(p, 1000, "%s/%s", dir, n);
	return (p);
}

static nvlist_t *
leaf(const char *n, boolean_t create)
{
	if (create) {
		int fd = open(vpath(n), O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, >=, 0);
		VERIFY0(ftruncate(fd, VSIZE));
		(void) close(fd);
	}
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, vpath(n));
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static nvlist_t *
root_of(nvlist_t **c, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	return (r);
}

static vdev_t *
find_path(vdev_t *vd, const char *path)
{
	if (vd->vdev_path != NULL && strcmp(vd->vdev_path, path) == 0)
		return (vd);
	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		vdev_t *f = find_path(vd->vdev_child[c], path);
		if (f != NULL)
			return (f);
	}
	return (NULL);
}

static uint64_t
guid_of(const char *n)
{
	spa_config_enter(g_spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *vd = find_path(g_spa->spa_root_vdev, vpath(n));
	VERIFY3P(vd, !=, NULL);
	uint64_t g = vd->vdev_guid;
	spa_config_exit(g_spa, SCL_VDEV, FTAG);
	return (g);
}

static void
write_data(void)
{
	objset_t *os;
	uint64_t obj;
	const int chunk = 1 << 20;
	char *buf = kmem_alloc(chunk, KM_SLEEP);
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ (uint64_t)getpid();

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 0, DMU_OT_NONE, 0,
	    tx);
	dmu_tx_commit(tx);
	for (int i = 0; i < 64; i++) {
		for (int j = 0; j < chunk / 8; j++) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			((uint64_t *)buf)[j] = x;
		}
		tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, obj, (uint64_t)i * chunk, chunk);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, obj, (uint64_t)i * chunk, chunk, buf, tx,
		    DMU_READ_PREFETCH);
		dmu_tx_commit(tx);
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	kmem_free(buf, chunk);
	txg_wait_synced(spa_get_dsl(g_spa), 0);
}

/* What "zpool status" and zpool_open() ask the kernel for. */
static void
pool_stats(void)
{
	nvlist_t *cfg = NULL;
	char alt[256];

	if (spa_get_stats(POOL, &cfg, alt, sizeof (alt)) == 0 && cfg != NULL)
		nvlist_free(cfg);
}

typedef struct {
	volatile int *stop;
	uint64_t calls;
	uint64_t accepted;
} loop_t;

/* One "while [[ ! -e $STOP_FILE ]]; do zpool scrub; done" loop. */
static void *
scrub_loop(void *a)
{
	loop_t *l = a;

	while (!*l->stop) {
		(void) usleep(1000 + random() % 2000);
		pool_stats();
		if (spa_scan(g_spa, POOL_SCAN_SCRUB, 0) == 0)
			l->accepted++;
		l->calls++;
	}
	return (NULL);
}

static boolean_t
replacing(void)
{
	spa_config_enter(g_spa, SCL_CONFIG, FTAG, RW_READER);
	boolean_t r = vdev_replace_in_progress(g_spa->spa_root_vdev);
	spa_config_exit(g_spa, SCL_CONFIG, FTAG);
	return (r);
}

/* "zpool history -i | grep -c 'rebuild.*canceled'" */
static int
rebuild_cancels(void)
{
	uint64_t off = 0, len;
	size_t cap = 1 << 20, used = 0;
	char *all = malloc(cap);
	int n = 0;

	for (;;) {
		char buf[65536];
		len = sizeof (buf);
		if (spa_history_get(g_spa, &off, &len, buf) != 0 || len == 0)
			break;
		if (used + len > cap) {
			cap = (used + len) * 2;
			all = realloc(all, cap);
		}
		memcpy(all + used, buf, len);
		used += len;
	}
	for (size_t i = 0; i + 8 <= used; i++) {
		if (memcmp(all + i, "canceled", 8) != 0)
			continue;
		size_t s = i > 40 ? i - 40 : 0;
		if (memmem(all + s, i - s, "vdev_guid=", 10) != NULL)
			n++;
	}
	free(all);
	return (n);
}

static int rebuilds, calls_total, accepted_total;

static int
replace_while_scrubbing(const char *oldn, const char *newn)
{
	zfs_scan_suspend_progress = 1;
	nvlist_t *n = leaf(newn, B_FALSE);
	nvlist_t *r = root_of(&n, 1);
	int e = spa_vdev_attach(g_spa, guid_of(oldn), r, B_TRUE, B_TRUE);
	fnvlist_free(r);
	if (e != 0) {
		(void) printf("FAIL replace -s %s %s: %d\n", oldn, newn, e);
		return (-1);
	}
	pool_stats();
	if (!vdev_rebuild_active(g_spa->spa_root_vdev)) {
		(void) printf("FAIL not resilvering after replace\n");
		return (-1);
	}
	pool_stats();
	if (spa_scan(g_spa, POOL_SCAN_SCRUB, 0) == 0) {
		(void) printf("FAIL scrub accepted while resilvering\n");
		return (-1);
	}

	volatile int stop = 0;
	loop_t l[4];
	pthread_t t[4];
	for (int i = 0; i < 4; i++) {
		l[i].stop = &stop;
		l[i].calls = l[i].accepted = 0;
		VERIFY0(pthread_create(&t[i], NULL, scrub_loop, &l[i]));
	}
	zfs_scan_suspend_progress = 0;
	for (int i = 0; i < 120; i++) {
		pool_stats();
		if (!vdev_rebuild_active(g_spa->spa_root_vdev))
			break;
		(void) sleep(1);
	}
	stop = 1;
	for (int i = 0; i < 4; i++) {
		VERIFY0(pthread_join(t[i], NULL));
		calls_total += l[i].calls;
		accepted_total += l[i].accepted;
	}
	rebuilds++;
	if (vdev_rebuild_active(g_spa->spa_root_vdev)) {
		(void) printf("FAIL still resilvering after 120 s\n");
		return (-1);
	}
	int c = rebuild_cancels();
	if (c != 0) {
		(void) printf("FAIL rebuild canceled (%d in history)\n", c);
		return (-1);
	}
	for (int i = 0; i < 60 && replacing(); i++) {
		pool_stats();
		(void) sleep(1);
	}
	if (replacing()) {
		(void) printf("FAIL still replacing after 60 s\n");
		return (-1);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	char cache[1000];

	if (argc < 2)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	srandom(getpid());
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(handle_tunable_option("zfs_vdev_min_ms_count=64", B_TRUE));

	nvlist_t *c[4];
	c[0] = leaf("file-1", B_TRUE);
	c[1] = leaf("file-2", B_TRUE);
	c[2] = leaf("file-3", B_TRUE);
	c[3] = leaf("file-4", B_TRUE);
	fnvlist_free(leaf("spare-1", B_TRUE));
	nvlist_t *rt = root_of(c, 4);
	nvlist_t *props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char *buf;

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		VERIFY3S(-1, !=, asprintf(&buf, "feature@%s",
		    spa_feature_table[i].fi_uname));
		fnvlist_add_uint64(props, buf, 0);
		free(buf);
	}
	VERIFY0(spa_create(POOL, rt, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(rt);
	VERIFY0(spa_open(POOL, &g_spa, FTAG));
	(void) printf("replaced top-level: %llu metaslabs\n",
	    (u_longlong_t)g_spa->spa_root_vdev->vdev_child[1]->vdev_ms_count);
	write_data();

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = 0;
	for (int i = 0; i < 3 && rc == 0; i++) {
		rc = replace_while_scrubbing("file-2", "spare-1");
		if (rc == 0)
			rc = replace_while_scrubbing("spare-1", "file-2");
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc == 0) {
		vdev_t *rvd = g_spa->spa_root_vdev;
		if (rvd->vdev_state != VDEV_STATE_HEALTHY) {
			(void) printf("FAIL pool state %llu\n",
			    (u_longlong_t)rvd->vdev_state);
			rc = -1;
		}
	}
	(void) printf("RESULT %s rebuilds=%d scrub_requests=%d "
	    "accepted=%d seconds=%ld\n", rc == 0 ? "PASS" : "FAIL", rebuilds,
	    calls_total, accepted_total, (long)(t1.tv_sec - t0.tv_sec));
	(void) fflush(stdout);
	if (rc != 0)
		_exit(1);
	spa_close(g_spa, FTAG);
	VERIFY0(spa_destroy(POOL));
	kernel_fini();
	return (0);
}
