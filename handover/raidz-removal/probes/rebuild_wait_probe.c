/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs in a
 * caller-provided directory.  Replays the loop of ZTS
 * functional/replacement/scrub_cancel.ksh:
 *
 *   suspend scans, scrub, replace file-2 by spare-1 (healing or -s),
 *   wait for the scrub to be cancelled, check a scrub is refused,
 *   unsuspend, "zpool wait" (every activity in turn until none waited),
 *   "zpool replace [-s] -w" back.
 *
 * Every spa_wait() runs in a helper thread with a watchdog.  On a hang the
 * state is dumped, then spa_notify_waiters() is called once: a wait that
 * returns after that is a lost wakeup, one that does not is an activity
 * that really is stuck.
 *
 * Usage: rebuild_wait_probe <dir> <iterations> [healing|sequential|both]
 *        [hang_secs]
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
#include <errno.h>

#define	POOL	"rbw"
#define	VSIZE	(128ULL << 20)	/* SPA_MINDEVSIZE * 2, as replacement.cfg */

static char dir[900];
static int hang_secs = 60;
static spa_t *g_spa;

static const char *act_name[] = {
	"discard", "free", "initialize", "replace", "remove", "resilver",
	"scrub", "trim", "raidz_expand", "condense"
};

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
	uint64_t x = 0x9e3779b97f4a7c15ULL;

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

static void
set_tunable(const char *kv)
{
	VERIFY0(handle_tunable_option(kv, B_TRUE));
}

static void
dump_vdev(vdev_t *vd, int depth)
{
	char dtl[200] = "";

	if (vd->vdev_ops->vdev_op_leaf) {
		(void) snprintf(dtl, sizeof (dtl),
		    " dtl_missing=%s dtl_outage=%s rebuild_txg=%llu "
		    "resilver_txg=%llu",
		    zfs_range_tree_is_empty(vd->vdev_dtl[DTL_MISSING]) ?
		    "empty" : "SET",
		    zfs_range_tree_is_empty(vd->vdev_dtl[DTL_OUTAGE]) ?
		    "empty" : "SET",
		    (u_longlong_t)vd->vdev_rebuild_txg,
		    (u_longlong_t)vd->vdev_resilver_txg);
	}
	(void) fprintf(stderr, "  %*s%s %s state=%llu%s", depth * 2, "",
	    vd->vdev_ops->vdev_op_type,
	    vd->vdev_path ? vd->vdev_path : "", (u_longlong_t)vd->vdev_state, dtl);
	if (vd == vd->vdev_top && vd->vdev_top_zap != 0) {
		vdev_rebuild_phys_t *vrp =
		    &vd->vdev_rebuild_config.vr_rebuild_phys;
		(void) fprintf(stderr, " rebuilding=%d rb_thread=%p "
		    "rb_state=%llu rb_errors=%llu exit/cancel/reset=%d/%d/%d",
		    vd->vdev_rebuilding, (void *)vd->vdev_rebuild_thread,
		    (u_longlong_t)vrp->vrp_rebuild_state,
		    (u_longlong_t)vrp->vrp_errors,
		    vd->vdev_rebuild_exit_wanted,
		    vd->vdev_rebuild_cancel_wanted,
		    vd->vdev_rebuild_reset_wanted);
	}
	(void) fprintf(stderr, "\n");
	for (uint64_t c = 0; c < vd->vdev_children; c++)
		dump_vdev(vd->vdev_child[c], depth + 1);
}

/* Scratch toggles that exist only in the wt-rbwait build. */
extern int rbw_no_sync_notify __attribute__((weak));
extern int rbw_complete_notify __attribute__((weak));
extern int rbw_cancel_fix __attribute__((weak));
extern uint64_t rbw_cancels __attribute__((weak));
extern uint64_t rbw_cancel_pre_state __attribute__((weak));
extern uint64_t rbw_cancel_post_state __attribute__((weak));
extern int rbw_cancel_reopening __attribute__((weak));

static void
dump_state(const char *why)
{
	spa_t *spa = g_spa;
	dsl_scan_t *scn = spa->spa_dsl_pool->dp_scan;

	(void) fprintf(stderr, "STATE (%s):\n", why);
	if (!spa_config_tryenter(spa, SCL_ALL, FTAG, RW_READER)) {
		(void) fprintf(stderr, "  config lock busy (writer); "
		    "scan func=%llu state=%llu waiters=%llu async=0x%x\n",
		    (u_longlong_t)scn->scn_phys.scn_func,
		    (u_longlong_t)scn->scn_phys.scn_state,
		    (u_longlong_t)spa->spa_waiters, spa->spa_async_tasks);
		return;
	}
	(void) fprintf(stderr, "  scan func=%llu state=%llu paused=%d "
	    "finished_txg=%llu last_synced=%llu syncing=%llu "
	    "scrub_started=%d spa_waiters=%llu async_tasks=0x%x "
	    "replace_in_progress=%d rebuild_active=%d\n",
	    (u_longlong_t)scn->scn_phys.scn_func,
	    (u_longlong_t)scn->scn_phys.scn_state,
	    dsl_scan_is_paused_scrub(scn),
	    (u_longlong_t)scn->scn_finished_txg,
	    (u_longlong_t)spa_last_synced_txg(spa),
	    (u_longlong_t)spa_syncing_txg(spa),
	    spa->spa_scrub_started,
	    (u_longlong_t)spa->spa_waiters, spa->spa_async_tasks,
	    vdev_replace_in_progress(spa->spa_root_vdev),
	    vdev_rebuild_active(spa->spa_root_vdev));
	dump_vdev(spa->spa_root_vdev, 0);
	spa_config_exit(spa, SCL_ALL, FTAG);
	if (&rbw_cancels != NULL)
		(void) fprintf(stderr, "  rebuild cancels=%llu; at the last "
		    "one the leaf with DTL_MISSING was state=%llu "
		    "reopening=%d before the check, state=%llu after\n",
		    (u_longlong_t)rbw_cancels,
		    (u_longlong_t)rbw_cancel_pre_state, rbw_cancel_reopening,
		    (u_longlong_t)rbw_cancel_post_state);
}

typedef struct {
	int act;
	int err;
	boolean_t waited;
	volatile int done;
} waitarg_t;

static void *
waiter(void *a)
{
	waitarg_t *w = a;

	w->err = spa_wait(POOL, w->act, &w->waited);
	w->done = 1;
	return (NULL);
}


static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}

/*
 * "zpool wait -t resilver" started while the rebuild is still active: the
 * interleaving of the lead (sleep on vrp_rebuild_state == ACTIVE).
 */
typedef struct {
	volatile int done;
	volatile int stop;
	volatile double t_ret;
	volatile double t_complete;
	volatile double t_synced;
	vdev_t *tvd;
	boolean_t waited;
	int err;
} rwait_t;

static void *
resilver_waiter(void *a)
{
	rwait_t *r = a;

	r->err = spa_wait(POOL, ZPOOL_WAIT_RESILVER, &r->waited);
	r->t_ret = now_ms();
	r->done = 1;
	return (NULL);
}

/* Timestamp the moment the rebuild stops being ACTIVE. */
static void *
rebuild_watch(void *a)
{
	rwait_t *r = a;

	/*
	 * Poll the rebuilding top-level directly: it is detached only after
	 * it has left ACTIVE, and only by a task requested from that point.
	 */
	vdev_t *vd = r->tvd;
	vdev_rebuild_phys_t *vrp = &vd->vdev_rebuild_config.vr_rebuild_phys;
	for (;;) {
		mutex_enter(&vd->vdev_rebuild_lock);
		boolean_t act = (vrp->vrp_rebuild_state == VDEV_REBUILD_ACTIVE);
		mutex_exit(&vd->vdev_rebuild_lock);
		if (r->stop || !act)
			break;
		(void) usleep(100);
	}
	r->t_complete = now_ms();
	uint64_t txg = spa_syncing_txg(g_spa);
	while (!r->stop && spa_last_synced_txg(g_spa) < txg)
		(void) usleep(100);
	r->t_synced = now_ms();
	return (NULL);
}

/*
 * Stress only (not what the test does): keep issuing "zpool scrub", which
 * reopens every vdev before it is refused, while the rebuild runs.
 */
typedef struct {
	vdev_t *tvd;
	int period_ms;
	volatile int stop;
	int tries;
} hammer_t;

static void *
scrub_hammer(void *a)
{
	hammer_t *h = a;
	vdev_rebuild_phys_t *vrp =
	    &h->tvd->vdev_rebuild_config.vr_rebuild_phys;

	while (!h->stop) {
		mutex_enter(&h->tvd->vdev_rebuild_lock);
		boolean_t act = (vrp->vrp_rebuild_state == VDEV_REBUILD_ACTIVE);
		mutex_exit(&h->tvd->vdev_rebuild_lock);
		if (!act)
			break;
		if (spa_scan(g_spa, POOL_SCAN_SCRUB, 0) == 0)
			(void) fprintf(stderr, "hammer: scrub started?\n");
		h->tries++;
		(void) usleep(h->period_ms * 1000);
	}
	return (NULL);
}

static int rw_runs, rw_hangs;
static double rw_lat_sum, rw_lat_max;

static int hangs, lost_wakeups, stuck;
static long last_ms;

/* One lzc_wait() with a watchdog.  Returns waited, or -1 on a real hang. */
static int
watched_wait(int act, const char *ctx)
{
	waitarg_t w = { .act = act };
	pthread_t t;
	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	VERIFY0(pthread_create(&t, NULL, waiter, &w));
	for (int i = 0; i < hang_secs * 10 && !w.done; i++)
		(void) usleep(100000);
	if (!w.done) {
		hangs++;
		(void) fprintf(stderr, "HANG: %s: wait for '%s' still "
		    "blocked after %d s\n", ctx, act_name[act], hang_secs);
		dump_state("at hang");
		spa_notify_waiters(g_spa);
		for (int i = 0; i < 50 && !w.done; i++)
			(void) usleep(100000);
		if (w.done) {
			lost_wakeups++;
			(void) fprintf(stderr, "HANG: returned after one "
			    "spa_notify_waiters(): LOST WAKEUP (waited=%d)\n",
			    w.waited);
		} else {
			stuck++;
			(void) fprintf(stderr, "HANG: still blocked after "
			    "spa_notify_waiters(): activity really stuck\n");
			for (int i = 0; i < 600 && !w.done; i++)
				(void) usleep(100000);
			if (!w.done) {
				dump_state("after 60 more s");
				return (-1);
			}
			(void) fprintf(stderr, "HANG: finally returned\n");
		}
	}
	VERIFY0(pthread_join(t, NULL));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	last_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000000;
	if (w.err != 0)
		(void) fprintf(stderr, "%s: wait '%s' error %d\n", ctx,
		    act_name[act], w.err);
	return (w.waited);
}

/* "zpool wait <pool>": every activity in turn until none was waited for. */
static int
zpool_wait_all(const char *ctx, char *trace, size_t tlen)
{
	trace[0] = '\0';
	for (;;) {
		boolean_t any = B_FALSE;
		for (int a = 0; a < ZPOOL_WAIT_NUM_ACTIVITIES; a++) {
			int r = watched_wait(a, ctx);
			if (r < 0)
				return (-1);
			if (r) {
				char b[48];
				any = B_TRUE;
				(void) snprintf(b, sizeof (b), "%s(%ldms) ",
				    act_name[a], last_ms);
				(void) strlcat(trace, b, tlen);
			}
		}
		if (!any)
			break;
		(void) strlcat(trace, "| ", tlen);
	}
	return (0);
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

static int
replace(const char *oldn, const char *newn, boolean_t rebuild)
{
	nvlist_t *n = leaf(newn, B_FALSE);
	nvlist_t *r = root_of(&n, 1);
	int e = spa_vdev_attach(g_spa, guid_of(oldn), r, B_TRUE, rebuild);
	fnvlist_free(r);
	return (e);
}

static int
one_mode(int iter, boolean_t seq)
{
	dsl_pool_t *dp = spa_get_dsl(g_spa);
	char ctx[64], trace[512];
	int e;

	(void) snprintf(ctx, sizeof (ctx), "iter %d %s", iter,
	    seq ? "sequential" : "healing");

	set_tunable("zfs_resilver_min_time_ms=50");
	zfs_scan_suspend_progress = 1;

	pool_stats();
	if ((e = spa_scan(g_spa, POOL_SCAN_SCRUB, 0)) != 0)
		(void) fprintf(stderr, "%s: scrub failed %d\n", ctx, e);
	pool_stats();
	if ((e = replace("file-2", "spare-1", seq)) != 0) {
		(void) fprintf(stderr, "%s: replace failed %d\n", ctx, e);
		dump_state("replace failed");
		return (-1);
	}
	/*
	 * PROBE_REFUSED_DELAY_US=<max>: poll the cancel every 1 ms and run
	 * the refused scrub a random 0..max us after it, with no status
	 * calls in between, to sample the moment the rebuild thread leaves
	 * the cancel and checks its first metaslab.
	 */
	const char *rd = getenv("PROBE_REFUSED_DELAY_US");
	const char *ra = getenv("PROBE_REFUSED_AFTER_SYNC_US");
	if (ra != NULL) {
		/*
		 * Land the refused scrub just after the txg that cancelled
		 * the scrub has synced, i.e. when the rebuild thread returns
		 * from dsl_scan_cancel() and checks its first metaslab.
		 */
		while (dsl_scan_scrubbing(dp))
			(void) usleep(50);
		uint64_t y = spa_syncing_txg(g_spa);
		while (spa_last_synced_txg(g_spa) < y)
			(void) usleep(20);
		(void) usleep(random() % (atoi(ra) + 1));
		rd = ra;
	} else if (rd != NULL) {
		for (int i = 0; i < 60000 && dsl_scan_scrubbing(dp); i++)
			(void) usleep(1000);
		(void) usleep(random() % (atoi(rd) + 1));
	} else {
		for (int i = 0; i < 600; i++) {
			pool_stats();
			if (!dsl_scan_scrubbing(dp))
				break;
			(void) usleep(100000);
		}
		pool_stats();
	}
	if (dsl_scan_scrubbing(dp))
		(void) fprintf(stderr, "%s: scrub not cancelled\n", ctx);
	if (!dsl_scan_resilvering(dp) &&
	    !vdev_rebuild_active(g_spa->spa_root_vdev))
		(void) fprintf(stderr, "%s: not resilvering\n", ctx);
	if (rd == NULL)
		pool_stats();
	if (spa_scan(g_spa, POOL_SCAN_SCRUB, 0) == 0)
		(void) fprintf(stderr, "%s: scrub was allowed\n", ctx);

	rwait_t rw = { 0 };
	pthread_t rwt, rwm;
	boolean_t use_rw = seq && getenv("PROBE_RW_WAITER") != NULL &&
	    vdev_rebuild_active(g_spa->spa_root_vdev);
	if (use_rw) {
		spa_config_enter(g_spa, SCL_CONFIG, FTAG, RW_READER);
		vdev_t *rvd = g_spa->spa_root_vdev;
		for (uint64_t c = 0; c < rvd->vdev_children; c++)
			if (rvd->vdev_child[c]->vdev_ops == &vdev_replacing_ops)
				rw.tvd = rvd->vdev_child[c];
		spa_config_exit(g_spa, SCL_CONFIG, FTAG);
		VERIFY3P(rw.tvd, !=, NULL);
		VERIFY0(pthread_create(&rwt, NULL, resilver_waiter, &rw));
		/* let it check and go to sleep on the active rebuild */
		(void) usleep(300000);
		VERIFY0(pthread_create(&rwm, NULL, rebuild_watch, &rw));
	}

	hammer_t hm = { 0 };
	pthread_t hmt;
	boolean_t use_hm = seq && getenv("PROBE_HAMMER_SCRUB_MS") != NULL;
	if (use_hm) {
		spa_config_enter(g_spa, SCL_CONFIG, FTAG, RW_READER);
		vdev_t *rvd = g_spa->spa_root_vdev;
		for (uint64_t c = 0; c < rvd->vdev_children; c++)
			if (rvd->vdev_child[c]->vdev_ops == &vdev_replacing_ops)
				hm.tvd = rvd->vdev_child[c];
		spa_config_exit(g_spa, SCL_CONFIG, FTAG);
		VERIFY3P(hm.tvd, !=, NULL);
		hm.period_ms = atoi(getenv("PROBE_HAMMER_SCRUB_MS"));
	}

	zfs_scan_suspend_progress = 0;
	set_tunable("zfs_resilver_min_time_ms=3000");
	if (use_hm)
		VERIFY0(pthread_create(&hmt, NULL, scrub_hammer, &hm));

	if (use_rw) {
		for (int i = 0; i < hang_secs * 100 && !rw.done; i++)
			(void) usleep(10000);
		rw_runs++;
		if (!rw.done) {
			rw_hangs++;
			(void) fprintf(stderr, "%s: RW-HANG: wait -t resilver "
			    "still blocked %d s after unsuspend\n", ctx,
			    hang_secs);
			dump_state("rw hang");
			spa_notify_waiters(g_spa);
			for (int i = 0; i < 500 && !rw.done; i++)
				(void) usleep(10000);
			(void) fprintf(stderr, "%s: RW-HANG: %s after one "
			    "notify\n", ctx, rw.done ? "returned" :
			    "still blocked");
			if (!rw.done)
				return (-1);
		}
		rw.stop = 1;
		VERIFY0(pthread_join(rwt, NULL));
		VERIFY0(pthread_join(rwm, NULL));
		double lat = rw.t_ret - rw.t_complete;
		rw_lat_sum += lat;
		if (lat > rw_lat_max)
			rw_lat_max = lat;
		(void) fprintf(stderr, "%s: wait -t resilver waited=%d err=%d "
		    "returned %.1f ms after rebuild left ACTIVE (that txg "
		    "synced at +%.1f ms)\n", ctx, rw.waited, rw.err, lat,
		    rw.t_synced - rw.t_complete);
	}

	if (getenv("PROBE_DUMP") != NULL)
		dump_state("before zpool wait");
	pool_stats();
	int zw = zpool_wait_all(ctx, trace, sizeof (trace));
	if (use_hm) {
		hm.stop = 1;
		if (zw == 0)
			VERIFY0(pthread_join(hmt, NULL));
		(void) fprintf(stderr, "%s: hammer issued %d refused "
		    "scrubs\n", ctx, hm.tries);
	}
	if (zw != 0)
		return (-1);
	if (getenv("PROBE_DUMP") != NULL)
		dump_state("after zpool wait");

	pool_stats();
	if ((e = replace("spare-1", "file-2", seq)) != 0) {
		(void) fprintf(stderr, "%s: replace back failed %d\n", ctx, e);
		dump_state("replace back failed");
		return (-1);
	}
	char c2[80];
	(void) snprintf(c2, sizeof (c2), "%s replace -w", ctx);
	if (watched_wait(ZPOOL_WAIT_REPLACE, c2) < 0)
		return (-1);
	(void) fprintf(stderr, "%s: zpool wait waited: %s\n", ctx, trace);
	return (0);
}

int
main(int argc, char **argv)
{
	char cache[1000];
	int iters;
	const char *modes = "both";

	if (argc < 3)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	iters = atoi(argv[2]);
	if (argc > 3)
		modes = argv[3];
	if (argc > 4)
		hang_secs = atoi(argv[4]);

	if (&rbw_no_sync_notify != NULL && getenv("RBW_NO_SYNC_NOTIFY"))
		rbw_no_sync_notify = atoi(getenv("RBW_NO_SYNC_NOTIFY"));
	if (&rbw_complete_notify != NULL && getenv("RBW_COMPLETE_NOTIFY"))
		rbw_complete_notify = atoi(getenv("RBW_COMPLETE_NOTIFY"));
	if (&rbw_cancel_fix != NULL && getenv("RBW_CANCEL_FIX"))
		rbw_cancel_fix = atoi(getenv("RBW_CANCEL_FIX"));
	(void) printf("toggles: no_sync_notify=%d complete_notify=%d "
	    "cancel_fix=%d\n",
	    &rbw_no_sync_notify != NULL ? rbw_no_sync_notify : -1,
	    &rbw_complete_notify != NULL ? rbw_complete_notify : -1,
	    &rbw_cancel_fix != NULL ? rbw_cancel_fix : -1);
	(void) fflush(stdout);
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	if (getenv("PROBE_NO_REBUILD_SCRUB") != NULL)
		set_tunable("zfs_rebuild_scrub_enabled=0");
	spa_config_path = cache;
	srandom(getpid());
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *t[4];
	t[0] = leaf("file-1", B_TRUE);
	t[1] = leaf("file-2", B_TRUE);
	t[2] = leaf("file-3", B_TRUE);
	t[3] = leaf("file-4", B_TRUE);
	fnvlist_free(leaf("spare-1", B_TRUE));
	nvlist_t *rt = root_of(t, 4);
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

	int rc = 0, done = 0;
	for (int i = 0; i < iters && rc == 0; i++) {
		if (strcmp(modes, "sequential") != 0 &&
		    (rc = one_mode(i, B_FALSE)) != 0)
			break;
		if (strcmp(modes, "healing") != 0 &&
		    (rc = one_mode(i, B_TRUE)) != 0)
			break;
		/*
		 * The test runs the loop once; settle (e.g. the scrub a
		 * finished rebuild starts) before replaying it again.
		 */
		char trace[512];
		if ((rc = zpool_wait_all("settle", trace,
		    sizeof (trace))) != 0)
			break;
		done++;
	}
	(void) printf("RESULT iterations=%d hangs=%d lost_wakeups=%d "
	    "stuck=%d rc=%d rw_runs=%d rw_hangs=%d rw_lat_avg=%.1fms "
	    "rw_lat_max=%.1fms\n", done, hangs, lost_wakeups, stuck, rc,
	    rw_runs, rw_hangs, rw_runs ? rw_lat_sum / rw_runs : 0.0,
	    rw_lat_max);
	if (&rbw_cancels != NULL)
		(void) printf("rebuild cancels=%llu\n",
		    (u_longlong_t)rbw_cancels);
	(void) fflush(stdout);
	if (rc != 0)
		_exit(3);

	spa_close(g_spa, FTAG);
	VERIFY0(spa_destroy(POOL));
	kernel_fini();
	return (0);
}
