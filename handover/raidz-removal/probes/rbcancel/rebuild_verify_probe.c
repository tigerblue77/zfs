/*
 * Scratch probe (not for upstream): can a concurrent vdev_reopen() make
 * the VERIFY(vdev_resilver_needed()) of vdev_rebuild_initiate_sync() or
 * vdev_rebuild_reset_sync() fail?
 *
 * Pool: 2-way mirror of file vdevs.  Each iteration, with rebuilds
 * parked (zfs_scan_suspend_progress=1) and H hammer threads reopening
 * every vdev (refused "zpool scrub" = spa_scan(), or "zpool reopen"):
 *   attach -s a-1 new-1          -> vdev_rebuild_initiate_sync()
 *   attach -s a-1 new-2          -> reset wanted -> reset_sync()
 *   unsuspend, wait for the rebuild, detach new-1 and new-2.
 *
 * Linked against the scratch tree whose toggles (weak symbols below)
 * select the fixes and make the VERIFY count instead of abort.
 *
 * Usage: rebuild_verify_probe <dir> <iterations> <scrub|sreopen>
 *        <threads> <period_us>
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

#define	POOL	"rbv"
#define	VSIZE	(256ULL << 20)

static char dir[900];
static spa_t *g_spa;
static int resets_seen;

extern int rbw_thread_fix __attribute__((weak));
extern int rbw_sync_fix __attribute__((weak));
extern int rbw_verify_soft __attribute__((weak));
extern uint64_t rbw_cancels __attribute__((weak));
extern uint64_t rbw_initiates __attribute__((weak));
extern uint64_t rbw_resets __attribute__((weak));
extern uint64_t rbw_initiate_unneeded __attribute__((weak));
extern uint64_t rbw_reset_unneeded __attribute__((weak));
extern uint64_t rbw_unneeded_closed __attribute__((weak));
extern uint64_t rbw_unneeded_other __attribute__((weak));
extern hrtime_t rbw_fail_ns __attribute__((weak));
extern hrtime_t rbw_task_start_ns __attribute__((weak));

#define	RING	64
static volatile hrtime_t ev_req[RING], ev_got[RING], ev_done[RING];
static volatile int ev_n;

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
wrap(const char *type, nvlist_t **c, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, type);
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
	uint64_t x = 0x9e3779b97f4a7c15ULL;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 0, DMU_OT_NONE, 0,
	    tx);
	dmu_tx_commit(tx);
	for (int i = 0; i < 32; i++) {
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

typedef struct {
	int scrub;
	int period_us;
	volatile int stop;
	uint64_t calls;
	uint64_t started;
} hammer_t;

static void *
hammer(void *a)
{
	hammer_t *h = a;

	while (!h->stop) {
		if (h->scrub) {
			if (spa_scan(g_spa, POOL_SCAN_SCRUB, 0) == 0) {
				h->started++;
				(void) spa_scan_stop(g_spa);
			}
		} else {
			/*
			 * The reopen that dsl_scan() does before a scrub is
			 * refused, without its setup-check sync task.
			 */
			int k = ev_n % RING;
			ev_req[k] = gethrtime();
			spa_vdev_state_enter(g_spa, SCL_NONE);
			ev_got[k] = gethrtime();
			g_spa->spa_scrub_reopen = B_TRUE;
			vdev_reopen(g_spa->spa_root_vdev);
			ev_done[k] = gethrtime();
			ev_n++;
			g_spa->spa_scrub_reopen = B_FALSE;
			(void) spa_vdev_state_exit(g_spa, NULL, 0);
		}
		h->calls++;
		if (h->period_us > 0)
			(void) usleep(random() % (h->period_us + 1));
	}
	return (NULL);
}

static int
attach_s(const char *oldn, const char *newn)
{
	nvlist_t *n = leaf(newn, B_FALSE);
	nvlist_t *r = wrap(VDEV_TYPE_ROOT, &n, 1);
	int e = spa_vdev_attach(g_spa, guid_of(oldn), r, B_FALSE, B_TRUE);
	fnvlist_free(r);
	return (e);
}

static uint64_t
cnt(uint64_t *p)
{
	return (p != NULL ? *p : 0);
}

int
main(int argc, char **argv)
{
	char cache[1000];

	if (argc < 6)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	int iters = atoi(argv[2]);
	int scrub = strcmp(argv[3], "scrub") == 0;
	int nthr = atoi(argv[4]);
	int period = atoi(argv[5]);

	/* Without the scratch toggles: the tree's own code, real VERIFY. */
	if (&rbw_thread_fix != NULL) {
		if (getenv("RBW_THREAD_FIX"))
			rbw_thread_fix = atoi(getenv("RBW_THREAD_FIX"));
		if (getenv("RBW_SYNC_FIX"))
			rbw_sync_fix = atoi(getenv("RBW_SYNC_FIX"));
		if (getenv("RBW_VERIFY_SOFT"))
			rbw_verify_soft = atoi(getenv("RBW_VERIFY_SOFT"));
		(void) printf("scratch toggles: thread_fix=%d sync_fix=%d "
		    "verify_soft=%d\n", rbw_thread_fix, rbw_sync_fix,
		    rbw_verify_soft);
	} else {
		(void) printf("no scratch toggles: tree code as built\n");
	}
	(void) printf("mode=%s threads=%d period_us=%d\n", argv[3], nthr,
	    period);
	(void) fflush(stdout);

	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	srandom(getpid());
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(handle_tunable_option("zfs_rebuild_scrub_enabled=0",
	    B_TRUE));

	nvlist_t *m[2];
	m[0] = leaf("a-1", B_TRUE);
	m[1] = leaf("a-2", B_TRUE);
	fnvlist_free(leaf("new-1", B_TRUE));
	fnvlist_free(leaf("new-2", B_TRUE));
	nvlist_t *mir = wrap(VDEV_TYPE_MIRROR, m, 2);
	nvlist_t *rt = wrap(VDEV_TYPE_ROOT, &mir, 1);
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
	write_data();

	hammer_t h[16] = { { 0 } };
	pthread_t ht[16];
	VERIFY3S(nthr, <=, 16);
	const char *loop = getenv("PROBE_LOOP") ? getenv("PROBE_LOOP") : "both";
	int done = 0;

	for (int t = 0; t < nthr; t++) {
		h[t].scrub = scrub;
		h[t].period_us = period;
		VERIFY0(pthread_create(&ht[t], NULL, hammer, &h[t]));
	}
	if (strcmp(loop, "reset") == 0) {
		/* One parked rebuild; each attach of new-2 resets it. */
		zfs_scan_suspend_progress = 1;
		VERIFY0(attach_s("a-1", "new-1"));
		for (int i = 0; i < iters; i++) {
			uint64_t r0 = cnt(&rbw_resets);
			int e = attach_s("a-1", "new-2");
			if (e != 0) {
				(void) fprintf(stderr, "iter %d: attach %d\n",
				    i, e);
				break;
			}
			vdev_t *tv0 = g_spa->spa_root_vdev->vdev_child[0];
			int w;
			for (w = 0; w < 150; w++) {
				(void) usleep(100000);
				txg_wait_synced(spa_get_dsl(g_spa), 0);
				if (&rbw_resets != NULL ?
				    cnt(&rbw_resets) != r0 :
				    !tv0->vdev_rebuild_reset_wanted)
					break;
			}
			resets_seen += (w < 150);
			if (w == 150) {
				vdev_t *tv = g_spa->spa_root_vdev->vdev_child[0];
				(void) fprintf(stderr, "iter %d: no reset "
				    "(cancels=%llu) rebuilding=%d thread=%p "
				    "reset_wanted=%d state=%llu\n", i,
				    (u_longlong_t)cnt(&rbw_cancels),
				    tv->vdev_rebuilding,
				    (void *)tv->vdev_rebuild_thread,
				    tv->vdev_rebuild_reset_wanted,
				    (u_longlong_t)tv->vdev_rebuild_config.
				    vr_rebuild_phys.vrp_rebuild_state);
				if (getenv("PROBE_PAUSE"))
					(void) sleep(60);
				break;
			}
			VERIFY0(spa_vdev_detach(g_spa, guid_of("new-2"), 0,
			    B_FALSE));
			done++;
		}
		zfs_scan_suspend_progress = 0;
	} else if (strcmp(loop, "init") == 0) {
		/* Each attach of new-1 to the idle mirror initiates. */
		for (int i = 0; i < iters; i++) {
			uint64_t f0 = cnt(&rbw_initiate_unneeded);
			hrtime_t a0 = gethrtime();
			int e = attach_s("a-1", "new-1");
			hrtime_t a1 = gethrtime();
			if (cnt(&rbw_initiate_unneeded) != f0) {
				(void) fprintf(stderr, "FAIL iter %d (us from "
				    "attach start): attach_end=%lld task=%lld "
				    "check=%lld\n", i, (a1 - a0) / 1000,
				    (rbw_task_start_ns - a0) / 1000,
				    (rbw_fail_ns - a0) / 1000);
				for (int q = ev_n - 6; q < ev_n; q++) {
					int k = (q + RING) % RING;
					if (q < 0)
						continue;
					(void) fprintf(stderr, "  reopen req=%lld "
					    "got=%lld done=%lld\n",
					    (ev_req[k] - a0) / 1000,
					    (ev_got[k] - a0) / 1000,
					    (ev_done[k] - a0) / 1000);
				}
			}
			if (e != 0) {
				(void) fprintf(stderr, "iter %d: attach %d\n",
				    i, e);
				break;
			}
			for (int w = 0; w < 3000 &&
			    vdev_rebuild_active(g_spa->spa_root_vdev); w++)
				(void) usleep(1000);
			(void) spa_scan_stop(g_spa);
			VERIFY0(spa_vdev_detach(g_spa, guid_of("new-1"), 0,
			    B_FALSE));
			done++;
		}
	} else {
		for (int i = 0; i < iters; i++) {
			int e;

			zfs_scan_suspend_progress = 1;
			uint64_t r0 = cnt(&rbw_resets);
			if ((e = attach_s("a-1", "new-1")) != 0 ||
			    (e = attach_s("a-1", "new-2")) != 0) {
				(void) fprintf(stderr, "iter %d: attach %d\n",
				    i, e);
				break;
			}
			for (int w = 0; w < 300 && cnt(&rbw_resets) == r0; w++)
				(void) usleep(10000);
			zfs_scan_suspend_progress = 0;
			for (int w = 0; w < 3000 &&
			    vdev_rebuild_active(g_spa->spa_root_vdev); w++)
				(void) usleep(10000);
			if (vdev_rebuild_active(g_spa->spa_root_vdev)) {
				(void) fprintf(stderr, "iter %d: rebuild "
				    "still active after 30 s\n", i);
				break;
			}
			(void) spa_scan_stop(g_spa);
			VERIFY0(spa_vdev_detach(g_spa, guid_of("new-1"), 0,
			    B_FALSE));
			VERIFY0(spa_vdev_detach(g_spa, guid_of("new-2"), 0,
			    B_FALSE));
			done++;
		}
	}
	for (int t = 0; t < nthr; t++) {
		h[t].stop = 1;
		VERIFY0(pthread_join(ht[t], NULL));
	}
	uint64_t calls = 0, started = 0;
	for (int t = 0; t < nthr; t++) {
		calls += h[t].calls;
		started += h[t].started;
	}
	(void) printf("RESULT iterations=%d hammer_calls=%llu "
	    "scrubs_started=%llu initiates=%llu resets=%llu "
	    "initiate_unneeded=%llu reset_unneeded=%llu (closed=%llu "
	    "other=%llu) cancels=%llu resets_seen=%d\n", done, (u_longlong_t)calls,
	    (u_longlong_t)started, (u_longlong_t)cnt(&rbw_initiates),
	    (u_longlong_t)cnt(&rbw_resets),
	    (u_longlong_t)cnt(&rbw_initiate_unneeded),
	    (u_longlong_t)cnt(&rbw_reset_unneeded),
	    (u_longlong_t)cnt(&rbw_unneeded_closed),
	    (u_longlong_t)cnt(&rbw_unneeded_other),
	    (u_longlong_t)cnt(&rbw_cancels), resets_seen);
	(void) fflush(stdout);
	if (done != iters)
		_exit(3);
	spa_close(g_spa, FTAG);
	VERIFY0(spa_destroy(POOL));
	kernel_fini();
	return (0);
}
