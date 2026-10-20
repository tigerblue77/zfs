/*
 * Scratch probe (not for upstream): userspace libzpool, sparse file vdevs only.
 * Crash consistency of vdev removal, log removal, allocating=off, attach,
 * detach, expansion and checkpoints.  For each seed a worker process runs
 * random operations and is killed with SIGKILL at a random moment, as ztest
 * does.  Writes it completed survive, as after a crash with an ordered
 * disk.  A verifier then:
 *   - optionally renames every device file, so that the paths in the
 *     pool's config lead nowhere and the labels alone identify devices;
 *   - finds the pool by scanning, without the cache file;
 *   - imports it (failure is a FAIL, with a retry as "import -m" to tell
 *     a lost log apart);
 *   - checks that the root vdev is healthy, invariants I1 and I3, that a
 *     scrub finds no error, and that "zdb -b" finds no leak.
 *
 * Usage: crash_fuzz_probe <scratch-root> <first-seed> <nseeds> <nops>
 *        <max-kill-ms> <zdb-binary>
 */
#include <libzpool.h>
#include <libzutil.h>
#include <signal.h>
#include <dirent.h>
#include <sys/dsl_scan.h>
#include <sys/zfs_debug.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/metaslab_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_dir.h>
#include <sys/zap.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <sys/spa_checkpoint.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>

static char dir[1024];
static char poolname[64];
static int nfile;
static uint64_t fsize = 192ULL << 20;
static int nfail;
static unsigned rs;
static uint64_t objs[64];
static int nobjs;
static boolean_t have_ckpt;
static unsigned opmask;

static unsigned
rnd(unsigned n)
{
	rs = rs * 1103515245 + 12345;
	return ((rs >> 16) % n);
}

static void
bad(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) printf("  FAIL ");
	(void) vprintf(fmt, ap);
	(void) printf("\n");
	va_end(ap);
	nfail++;
}

static nvlist_t *
mk_file(void)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile++);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, fsize));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static nvlist_t *
mk_top(const char *type, int n, const char *bias)
{
	nvlist_t *t;

	if (strcmp(type, VDEV_TYPE_FILE) == 0) {
		t = mk_file();
	} else {
		nvlist_t **c = malloc(n * sizeof (nvlist_t *));

		t = fnvlist_alloc();
		for (int i = 0; i < n; i++)
			c[i] = mk_file();
		fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, type);
		if (strcmp(type, VDEV_TYPE_RAIDZ) == 0)
			fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, 1);
		fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, 0);
		fnvlist_add_nvlist_array(t, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)c, n);
		for (int i = 0; i < n; i++)
			fnvlist_free(c[i]);
		free(c);
	}
	if (bias != NULL)
		fnvlist_add_string(t, ZPOOL_CONFIG_ALLOCATION_BIAS, bias);
	return (t);
}

static nvlist_t *
mk_root(nvlist_t **tops, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)tops, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(tops[i]);
	return (r);
}

static void
mkpool(nvlist_t *rt)
{
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
	VERIFY0(spa_create(poolname, rt, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(rt);
}

static void
sync1(void)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	txg_wait_synced(spa_get_dsl(spa), 0);
	spa_close(spa, FTAG);
}

/* I1 and I3; returns the counter and spa_get_dspace() for I2. */
static void
check(const char *step, uint64_t *nap, uint64_t *dsp)
{
	spa_t *spa;
	uint64_t exp = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	metaslab_class_t *normal = spa_normal_class(spa);
	vdev_t *rvd = spa->spa_root_vdev;
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *vd = rvd->vdev_child[c];

		if (!vdev_is_concrete(vd) || vd->vdev_mg == NULL ||
		    !vd->vdev_noalloc || vd->vdev_mg->mg_class != normal)
			continue;
		uint64_t raw = metaslab_group_get_space(vd->vdev_mg);
		exp += spa_deflate(spa) ? vdev_deflated_space(vd, raw) : raw;
	}
	uint64_t na = spa->spa_nonallocating_dspace;
	/* spa_rdspace: the special classes add what they allocate. */
	uint64_t ds = spa->spa_rdspace;
	if (na != exp)
		bad("I1 after %s: nonallocating %llu, expected %llu", step,
		    (u_longlong_t)na, (u_longlong_t)exp);
	if (metaslab_class_get_alloc(normal) >
	    metaslab_class_get_space(normal))
		bad("I3 after %s: normal alloc %llu > space %llu", step,
		    (u_longlong_t)metaslab_class_get_alloc(normal),
		    (u_longlong_t)metaslab_class_get_space(normal));
	spa_config_exit(spa, SCL_ALL, FTAG);
	spa_close(spa, FTAG);
	if (nap != NULL)
		*nap = na;
	if (dsp != NULL)
		*dsp = ds;
}

typedef struct wr { uint64_t obj; uint64_t len; boolean_t do_free; } wr_t;

static void
write_sync(void *arg, dmu_tx_t *tx)
{
	wr_t *w = arg;
	objset_t *mos = dmu_tx_pool(tx)->dp_meta_objset;

	if (w->do_free) {
		VERIFY0(dmu_object_free(mos, w->obj, tx));
		return;
	}
	w->obj = dmu_object_alloc(mos, DMU_OT_UINT64_OTHER, 128 << 10,
	    DMU_OT_NONE, 0, tx);
	/* Incompressible: the MOS is accounted as uncompressed metadata. */
	uint64_t *buf = malloc(w->len), x = w->obj * 0x9E3779B97F4A7C15ULL + 1;
	for (uint64_t i = 0; i < w->len / 8; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = x;
	}
	dmu_write(mos, w->obj, 0, w->len, buf, tx, DMU_READ_NO_PREFETCH);
	free(buf);
}

static int
do_write(void)
{
	wr_t w = { 0, (1 + rnd(6)) << 20, B_FALSE };

	if (nobjs == 64)
		return (0);
	int e = dsl_sync_task(poolname, NULL, write_sync, &w, 0,
	    ZFS_SPACE_CHECK_NORMAL);
	if (e == 0)
		objs[nobjs++] = w.obj;
	return (e);
}

static int
do_free(void)
{
	if (nobjs == 0)
		return (0);
	int i = rnd(nobjs);
	wr_t w = { objs[i], 0, B_TRUE };
	int e = dsl_sync_task(poolname, NULL, write_sync, &w, 0,
	    ZFS_SPACE_CHECK_NONE);
	if (e == 0)
		objs[i] = objs[--nobjs];
	return (e);
}

/* A random concrete top-level vdev guid, or 0. */
static int want_noalloc = -1;	/* -1 any, 0 allocating, 1 not allocating */

static uint64_t
pick_top(void)
{
	spa_t *spa;
	uint64_t g = 0, cand[32];
	int n = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	for (uint64_t c = 0; c < rvd->vdev_children && n < 32; c++) {
		vdev_t *vd = rvd->vdev_child[c];
		if (vdev_is_concrete(vd) && !vd->vdev_islog &&
		    vd->vdev_mg != NULL &&
		    (want_noalloc < 0 || vd->vdev_noalloc == want_noalloc))
			cand[n++] = vd->vdev_guid;
	}
	if (n > 0)
		g = cand[rnd(n)];
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static void
wait_removal(void)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (int i = 0; i < 900; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (spa->spa_vdev_removal == NULL)
			break;
		(void) usleep(50000);
	}
	spa_close(spa, FTAG);
}

static boolean_t
removing(void)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	boolean_t r = spa->spa_vdev_removal != NULL;
	spa_close(spa, FTAG);
	return (r);
}

/* Describe a random top-level vdev: guid, type, first leaf guid and path. */
static int
pick_desc(uint64_t *tg, char *type, size_t tl, uint64_t *lg, char *path,
    size_t pl, int *nch)
{
	spa_t *spa;
	int n = 0;
	vdev_t *cand[32];

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	for (uint64_t c = 0; c < rvd->vdev_children && n < 32; c++) {
		vdev_t *vd = rvd->vdev_child[c];
		if (vdev_is_concrete(vd) && !vd->vdev_islog &&
		    vd->vdev_mg != NULL && !vd->vdev_removing)
			cand[n++] = vd;
	}
	int r = -1;
	if (n > 0) {
		vdev_t *vd = cand[rnd(n)];
		vdev_t *lf = vd->vdev_ops->vdev_op_leaf ? vd :
		    vd->vdev_child[vd->vdev_children - 1];
		*tg = vd->vdev_guid;
		(void) strlcpy(type, vd->vdev_ops->vdev_op_type, tl);
		*lg = lf->vdev_guid;
		(void) strlcpy(path, lf->vdev_path ? lf->vdev_path : "", pl);
		*nch = vd->vdev_children;
		r = 0;
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (r);
}

static void
wait_expand(void)
{
	spa_t *spa;

	for (int i = 0; i < 900; i++) {
		VERIFY0(spa_open(poolname, &spa, FTAG));
		boolean_t busy = spa->spa_raidz_expand != NULL;
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
		if (!busy)
			return;
		(void) usleep(50000);
	}
}

static int
reimport(int flags)
{
	nvlist_t *cfg = NULL;

	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	int e = spa_import(poolname, cfg, NULL, flags);
	fnvlist_free(cfg);
	return (e);
}


static const char *const opname[] = {
	"write", "free", "allocating=off", "allocating=on", "remove",
	"remove+cancel", "add disk", "add mirror", "add special",
	"checkpoint", "checkpoint discard", "rewind to checkpoint",
	"export+import", "attach", "detach", "add log", "remove log",
	"add+off+remove"
};
#define	NOPS	(sizeof (opname) / sizeof (opname[0]))

static int tracefd = -1;

static void
trace(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	if (tracefd >= 0 && n > 0)
		(void) write(tracefd, buf, MIN(n, sizeof (buf) - 1));
}

static uint64_t
pick_log(void)
{
	spa_t *spa;
	uint64_t g = 0, cand[32];
	int n = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	for (uint64_t c = 0; c < rvd->vdev_children && n < 32; c++) {
		vdev_t *vd = rvd->vdev_child[c];
		if (vdev_is_concrete(vd) && vd->vdev_islog)
			cand[n++] = vd->vdev_guid;
	}
	if (n > 0)
		g = cand[rnd(n)];
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static int
add_top(nvlist_t *top)
{
	spa_t *spa;
	nvlist_t *rt = mk_root(&top, 1);

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_add(spa, rt, B_FALSE);
	spa_close(spa, FTAG);
	fnvlist_free(rt);
	return (e);
}

static uint64_t
last_top_guid(void)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t g = rvd->vdev_child[rvd->vdev_children - 1]->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

/* The worker: random operations until killed. */
static void
worker(int nops)
{
	nvlist_t *t[3];
	int nt = 0;
	char path[1200];

	(void) snprintf(path, sizeof (path), "%s/trace", dir);
	tracefd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	t[nt++] = mk_top("mirror", 2, NULL);
	t[nt++] = mk_top(rnd(2) ? "raidz" : "file", 3, NULL);
	if (rnd(2))
		t[nt++] = mk_top("file", 1, NULL);
	mkpool(mk_root(t, nt));
	sync1();
	for (int i = 0; i < 3; i++)
		(void) do_write();
	sync1();
	(void) snprintf(path, sizeof (path), "%s/ready", dir);
	(void) close(open(path, O_WRONLY | O_CREAT, 0644));

	for (int step = 0; step < nops; step++) {
		unsigned op = rnd(NOPS);
		int e = 0;
		spa_t *spa;
		uint64_t g;

		trace(" [%s", opname[op]);
		switch (op) {
		case 0: e = do_write(); break;
		case 1: e = do_free(); break;
		case 2: case 3:
			want_noalloc = op == 2 ? 0 : 1;
			g = pick_top();
			want_noalloc = -1;
			if (g == 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = op == 2 ? spa_vdev_noalloc(spa, g) :
			    spa_vdev_alloc(spa, g);
			spa_close(spa, FTAG);
			break;
		case 4: case 5:
			if ((g = pick_top()) == 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = spa_vdev_remove(spa, g, B_FALSE);
			spa_close(spa, FTAG);
			if (e == 0 && op == 5 && removing()) {
				VERIFY0(spa_open(poolname, &spa, FTAG));
				e = spa_vdev_remove_cancel(spa);
				spa_close(spa, FTAG);
			}
			break;
		case 6: e = add_top(mk_top("file", 1, NULL)); break;
		case 7: e = add_top(mk_top("mirror", 2, NULL)); break;
		case 8:
			e = add_top(mk_top("file", 1, VDEV_ALLOC_BIAS_SPECIAL));
			break;
		case 9:
			e = spa_checkpoint(poolname);
			if (e == 0)
				have_ckpt = B_TRUE;
			break;
		case 10:
			e = spa_checkpoint_discard(poolname);
			if (e == 0)
				have_ckpt = B_FALSE;
			break;
		case 11:
			if (!have_ckpt || removing())
				break;
			e = reimport(ZFS_IMPORT_CHECKPOINT);
			have_ckpt = B_FALSE;
			nobjs = 0;
			if (e != 0 && getenv("FZ_NOKILL") != NULL)
				zfs_dbgmsg_print(STDOUT_FILENO, "rewind");
			break;
		case 12:
			e = reimport(0);
			break;
		case 13: case 14: {
			uint64_t tg, lg;
			char type[32], lp[1100];
			int nch;

			if (removing() || pick_desc(&tg, type, sizeof (type),
			    &lg, lp, sizeof (lp), &nch) != 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			if (op == 13) {
				nvlist_t *nl = mk_file();
				nvlist_t *rt = mk_root(&nl, 1);
				boolean_t rz = strcmp(type, VDEV_TYPE_RAIDZ) == 0;
				e = spa_vdev_attach(spa, rz ? tg : lg, rt,
				    B_FALSE, B_FALSE);
				fnvlist_free(rt);
			} else if (strcmp(type, VDEV_TYPE_MIRROR) == 0 &&
			    nch > 2) {
				e = spa_vdev_detach(spa, lg, tg, B_FALSE);
			}
			spa_close(spa, FTAG);
			break;
		}
		case 15: {
			nvlist_t *f = mk_file();
			fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 1);
			e = add_top(f);
			break;
		}
		case 16:
			if ((g = pick_log()) == 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = spa_vdev_remove(spa, g, B_FALSE);
			spa_close(spa, FTAG);
			break;
		case 17:
			if ((e = add_top(mk_top("file", 1, NULL))) != 0)
				break;
			g = last_top_guid();
			VERIFY0(spa_open(poolname, &spa, FTAG));
			(void) spa_vdev_noalloc(spa, g);
			e = spa_vdev_remove(spa, g, B_FALSE);
			spa_close(spa, FTAG);
			break;
		}
		trace("=%d]", e);
	}
	trace(" [done]");
	_exit(0);
}

static boolean_t
scrub_clean(spa_t *spa)
{
	dsl_scan_t *scn = spa->spa_dsl_pool->dp_scan;
	int e = 0;

	for (int i = 0; i < 100; i++) {
		e = spa_scan(spa, POOL_SCAN_SCRUB, 0);
		if (e != EBUSY)
			break;
		txg_wait_synced(spa_get_dsl(spa), 0);
		(void) usleep(100000);
	}
	if (e != 0) {
		bad("scrub did not start: %d", e);
		return (B_FALSE);
	}
	for (int i = 0; i < 3000; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (scn->scn_phys.scn_state != DSS_SCANNING)
			break;
		(void) usleep(20000);
	}
	if (scn->scn_phys.scn_state != DSS_FINISHED)
		bad("scrub state %llu", (u_longlong_t)scn->scn_phys.scn_state);
	if (scn->scn_phys.scn_errors != 0)
		bad("scrub found %llu errors",
		    (u_longlong_t)scn->scn_phys.scn_errors);
	return (scn->scn_phys.scn_errors == 0);
}

/* The verifier: what an administrator would see after the crash. */
static int
verify_crash(unsigned seed, const char *zdb)
{
	char scandir[1200], cache[1200];
	boolean_t moved = seed % 2;

	(void) strlcpy(scandir, dir, sizeof (scandir));
	if (moved) {
		(void) snprintf(scandir, sizeof (scandir), "%s/moved", dir);
		VERIFY0(mkdir(scandir, 0755));
		DIR *d = opendir(dir);
		struct dirent *de;
		unsigned k = 0;

		VERIFY3P(d, !=, NULL);
		while ((de = readdir(d)) != NULL) {
			char p[1200], q[1300];

			if (strncmp(de->d_name, "vd.", 3) != 0)
				continue;
			(void) snprintf(p, sizeof (p), "%s/%s", dir,
			    de->d_name);
			(void) snprintf(q, sizeof (q), "%s/d%03u", scandir,
			    (k++ * 37 + seed) % 997);
			VERIFY0(rename(p, q));
		}
		(void) closedir(d);
	}
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	(void) unlink(cache);
	(void) snprintf(cache, sizeof (cache), "%s/verify.cache", dir);
	spa_config_path = strdup(cache);
	(void) printf("  seed %u: %s\n", seed, moved ? "moved" : "in place");

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	importargs_t args = { 0 };
	char *paths[1] = { scandir };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	nvlist_t *cfg = NULL;
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	int e = zpool_find_config(&lpch, poolname, &cfg, &args);
	if (e != 0) {
		bad("scan found no pool: %d", e);
		kernel_fini();
		return (1);
	}
	e = spa_import(poolname, cfg, NULL, 0);
	if (e != 0) {
		int e2 = spa_import(poolname, cfg, NULL,
		    ZFS_IMPORT_MISSING_LOG);
		bad("import after crash: %d (import -m: %d)", e, e2);
		nvlist_free(cfg);
		if (e2 == 0)
			VERIFY0(spa_export(poolname, NULL, B_FALSE, B_FALSE));
		kernel_fini();
		return (1);
	}
	nvlist_free(cfg);

	spa_t *spa;
	VERIFY0(spa_open(poolname, &spa, FTAG));
	vdev_t *rvd = spa->spa_root_vdev;
	if (rvd->vdev_state != VDEV_STATE_HEALTHY) {
		bad("root vdev state %llu", (u_longlong_t)rvd->vdev_state);
		for (uint64_t c = 0; c < rvd->vdev_children; c++)
			(void) printf("    child %llu %s state %llu\n",
			    (u_longlong_t)c,
			    rvd->vdev_child[c]->vdev_ops->vdev_op_type,
			    (u_longlong_t)rvd->vdev_child[c]->vdev_state);
	}
	spa_close(spa, FTAG);
	wait_removal();
	wait_expand();
	sync1();
	check("crash import", NULL, NULL);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	(void) scrub_clean(spa);
	spa_close(spa, FTAG);
	VERIFY0(spa_export(poolname, NULL, B_FALSE, B_FALSE));
	kernel_fini();

	if (zdb != NULL && nfail == 0) {
		char cmd[4000];

		(void) snprintf(cmd, sizeof (cmd),
		    "%s -e -p %s -b %s > %s/zdb.txt 2>&1", zdb, scandir,
		    poolname, dir);
		int st = system(cmd);
		if (st != 0)
			bad("zdb -b: status %#x (see %s/zdb.txt)", st, dir);
	}
	return (nfail != 0);
}

int
main(int argc, char **argv)
{
	if (argc < 7)
		return (2);
	const char *root = argv[1];
	unsigned first = atoi(argv[2]);
	int nseeds = atoi(argv[3]), nops = atoi(argv[4]);
	int maxkill = atoi(argv[5]);
	const char *zdb = strcmp(argv[6], "-") == 0 ? NULL : argv[6];
	int fails = 0, aborts = 0, killed = 0;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < nseeds; i++) {
		unsigned seed = first + i;
		struct rlimit rl = { 0, 0 };

		(void) setrlimit(RLIMIT_CORE, &rl);
		rs = seed;
		(void) snprintf(poolname, sizeof (poolname), "cf%u", seed);
		(void) snprintf(dir, sizeof (dir), "%s/s%u", root, seed);
		(void) mkdir(dir, 0755);
		char cache[1200], ready[1200];
		(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
		(void) snprintf(ready, sizeof (ready), "%s/ready", dir);
		spa_config_path = cache;

		(void) fflush(stdout);
		pid_t w = fork();
		if (w == 0)
			worker(nops);
		for (int t = 0; t < 600 && access(ready, F_OK) != 0; t++)
			(void) usleep(100000);
		unsigned delay = (unsigned)(((uint64_t)seed * 2654435761u) %
		    (unsigned)maxkill);
		int st;
		if (getenv("FZ_NOKILL") == NULL) {
			(void) usleep(delay * 1000);
			if (kill(w, SIGKILL) == 0)
				killed++;
		}
		VERIFY3S(waitpid(w, &st, 0), ==, w);

		/* The verifier draws from the same stream, after the worker. */
		rs = seed * 7919 + 1;
		nfile = 0;
		(void) fflush(stdout);
		pid_t v = fork();
		if (v == 0) {
			(void) printf("  seed %u: killed after %u ms\n", seed,
			    delay);
			_exit(verify_crash(seed, zdb));
		}
		VERIFY3S(waitpid(v, &st, 0), ==, v);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
		    getenv("FZ_TRACE") != NULL) {
			char tp[1200], tb[4096] = "";
			(void) snprintf(tp, sizeof (tp), "%s/trace", dir);
			int fd = open(tp, O_RDONLY);
			if (fd >= 0) {
				ssize_t n = read(fd, tb, sizeof (tb) - 1);
				tb[n > 0 ? n : 0] = '\0';
				(void) close(fd);
			}
			(void) printf("  seed %u ops:%s\n", seed, tb);
		}
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
			char cmd[1200];
			(void) snprintf(cmd, sizeof (cmd), "rm -rf %s", dir);
			if (system(cmd) != 0)
				(void) printf("cleanup of %s failed\n", dir);
			continue;
		}
		char tp[1200], tb[4096] = "";
		(void) snprintf(tp, sizeof (tp), "%s/trace", dir);
		int fd = open(tp, O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, tb, sizeof (tb) - 1);
			tb[n > 0 ? n : 0] = '\0';
			(void) close(fd);
		}
		size_t tl = strlen(tb);
		(void) printf("  seed %u ops (last 600 chars):%s\n", seed,
		    tl > 600 ? tb + tl - 600 : tb);
		if (WIFEXITED(st)) {
			fails++;
			(void) printf("seed %u: FAIL (kept %s)\n", seed, dir);
		} else {
			aborts++;
			(void) printf("seed %u: ABORT (status %#x, kept %s)\n",
			    seed, st, dir);
		}
	}
	(void) printf("SUMMARY seeds %d, killed %d, failures %d, aborts %d\n",
	    nseeds, killed, fails, aborts);
	return (0);
}
