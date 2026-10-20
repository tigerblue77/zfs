/*
 * Scratch probe (not for upstream): userspace libzpool, sparse file vdevs only.
 * Random sequences of allocating=off/on, device removal (waited for or
 * canceled), checkpoints (discarded or rewound to), vdev additions, data
 * writes and frees, and export/import.  After every step it checks:
 *   I1  spa_nonallocating_dspace == sum of the deflated space of the
 *       non-allocating concrete normal-class top-level vdevs;
 *   I2  the same counter and spa_get_dspace() are unchanged by an
 *       export and import (in-core state matches what import rebuilds);
 *   I3  the normal class never has more allocated than space.
 * Every seed runs in its own process; a crash or assertion shows as an abort.
 *
 * Usage: noalloc_fuzz_probe <scratch-root> <first-seed> <nseeds> <nops>
 */
#include <libzpool.h>
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
	"write", "free", "allocating=off", "allocating=on", "remove+wait",
	"remove+cancel", "add disk", "add mirror", "add special",
	"checkpoint", "checkpoint discard", "rewind to checkpoint",
	"export+import", "attach", "detach", "grow+online -e"
};
#define	NOPS	(sizeof (opname) / sizeof (opname[0]))

static int
run_seed(const char *root, unsigned seed, int nops)
{
	char *cache;
	struct rlimit rl = { 0, 0 };
	char trace[65536] = "";
	size_t tl = 0;

	(void) setrlimit(RLIMIT_CORE, &rl);
	rs = seed;
	(void) snprintf(poolname, sizeof (poolname), "fz%u", seed);
	(void) snprintf(dir, sizeof (dir), "%s/s%u", root, seed);
	(void) mkdir(dir, 0755);
	VERIFY3S(asprintf(&cache, "%s/zpool.cache", dir), !=, -1);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *t[3];
	int nt = 0;
	t[nt++] = mk_top("mirror", 2, NULL);
	t[nt++] = mk_top(rnd(2) ? "raidz" : "file", 3, NULL);
	if (rnd(2))
		t[nt++] = mk_top("file", 1, NULL);
	mkpool(mk_root(t, nt));
	sync1();
	for (int i = 0; i < 3; i++)
		(void) do_write();
	check("create", NULL, NULL);

	for (int step = 0; step < nops && nfail == 0; step++) {
		unsigned op = rnd(NOPS);
		if (opmask != 0 && !(opmask & (1u << op)))
			continue;
		int e = 0;
		spa_t *spa;
		uint64_t g;

		switch (op) {
		case 0: e = do_write(); break;
		case 1: e = do_free(); break;
		case 2:
			want_noalloc = 0;
			g = pick_top();
			want_noalloc = -1;
			if (g == 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = spa_vdev_noalloc(spa, g);
			spa_close(spa, FTAG);
			break;
		case 3:
			want_noalloc = 1;
			g = pick_top();
			want_noalloc = -1;
			if (g == 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = spa_vdev_alloc(spa, g);
			spa_close(spa, FTAG);
			break;
		case 4:
		case 5:
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
			if (e == 0)
				wait_removal();
			break;
		case 6: case 7: case 8: {
			nvlist_t *nt1 = op == 6 ? mk_top("file", 1, NULL) :
			    op == 7 ? mk_top("mirror", 2, NULL) :
			    mk_top("file", 1, VDEV_ALLOC_BIAS_SPECIAL);
			nvlist_t *rt = mk_root(&nt1, 1);
			VERIFY0(spa_open(poolname, &spa, FTAG));
			e = spa_vdev_add(spa, rt, B_FALSE);
			spa_close(spa, FTAG);
			fnvlist_free(rt);
			break;
		}
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
			/* objects written after the checkpoint are gone */
			nobjs = 0;
			break;
		case 13: case 14: case 15: {
			uint64_t tg, lg;
			char type[32], path[1100];
			int nch;

			if (removing() ||
			    pick_desc(&tg, type, sizeof (type), &lg, path,
			    sizeof (path), &nch) != 0)
				break;
			VERIFY0(spa_open(poolname, &spa, FTAG));
			if (op == 13) {
				nvlist_t *nl = mk_file();
				nvlist_t *rt = mk_root(&nl, 1);
				boolean_t rz = strcmp(type, VDEV_TYPE_RAIDZ) == 0;
				e = spa_vdev_attach(spa, rz ? tg : lg, rt,
				    B_FALSE, B_FALSE);
				fnvlist_free(rt);
				spa_close(spa, FTAG);
				if (e == 0 && rz)
					wait_expand();
				sync1();
				break;
			} else if (op == 14) {
				if (strcmp(type, VDEV_TYPE_MIRROR) == 0 &&
				    nch > 2)
					e = spa_vdev_detach(spa, lg, tg, B_FALSE);
				spa_close(spa, FTAG);
				break;
			}
			/* grow one leaf and online -e it */
			if (path[0] != '\0') {
				struct stat st;
				if (stat(path, &st) == 0 &&
				    truncate(path, st.st_size + (64 << 20)) == 0) {
					vdev_state_t ns;
					e = vdev_online(spa, lg, ZFS_ONLINE_EXPAND,
					    &ns);
				}
			}
			spa_close(spa, FTAG);
			break;
		}
		case 12: {
			uint64_t na0, ds0, na1, ds1;

			if (removing())
				wait_removal();
			sync1();
			check("before export", &na0, &ds0);
			e = reimport(0);
			if (e == 0) {
				check("export+import", &na1, &ds1);
				if (na1 != na0 || ds1 != ds0)
					bad("I2: nonallocating %llu -> %llu, "
					    "dspace %llu -> %llu",
					    (u_longlong_t)na0,
					    (u_longlong_t)na1,
					    (u_longlong_t)ds0,
					    (u_longlong_t)ds1);
			}
			break;
		}
		}
		sync1();
		tl += snprintf(trace + tl, sizeof (trace) - tl, " %s=%d",
		    opname[op], e);
		if (tl > sizeof (trace) - 64)
			tl = 0;
		check(opname[op], NULL, NULL);
		if (op == 11 && e != 0)
			bad("rewind to checkpoint failed: %d", e);
		if (nfail != 0)
			(void) printf("  seed %u trace:%s\n", seed, trace);
	}
	/* Final I2 check. */
	if (nfail == 0) {
		uint64_t na0, ds0, na1, ds1;

		if (removing())
			wait_removal();
		sync1();
		check("final", &na0, &ds0);
		if (reimport(0) == 0) {
			check("final import", &na1, &ds1);
			if (na1 != na0 || ds1 != ds0)
				bad("I2 final: nonallocating %llu -> %llu, "
				    "dspace %llu -> %llu", (u_longlong_t)na0,
				    (u_longlong_t)na1, (u_longlong_t)ds0,
				    (u_longlong_t)ds1);
		}
		if (nfail != 0)
			(void) printf("  seed %u trace:%s\n", seed, trace);
	}
	if (getenv("FZ_TRACE") != NULL)
		(void) printf("  seed %u ops:%s\n", seed, trace);
	VERIFY0(spa_export(poolname, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (nfail != 0);
}

int
main(int argc, char **argv)
{
	if (argc < 5)
		return (2);
	const char *root = argv[1];
	unsigned first = atoi(argv[2]);
	int nseeds = atoi(argv[3]), nops = atoi(argv[4]);
	int fails = 0, aborts = 0;

	if (argc > 5)
		opmask = strtoul(argv[5], NULL, 0);

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < nseeds; i++) {
		unsigned seed = first + i;
		(void) fflush(stdout);
		pid_t p = fork();
		if (p == 0)
			_exit(run_seed(root, seed, nops));
		int st;
		VERIFY3S(waitpid(p, &st, 0), ==, p);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
			char cmd[1200];
			(void) snprintf(cmd, sizeof (cmd), "rm -rf %s/s%u",
			    root, seed);
			if (system(cmd) != 0)
				(void) printf("cleanup of %s failed\n", cmd);
			continue;
		}
		if (WIFEXITED(st)) {
			fails++;
			(void) printf("seed %u: invariant failure\n", seed);
		} else {
			aborts++;
			(void) printf("seed %u: ABORT (status %#x)\n", seed,
			    st);
		}
	}
	(void) printf("SUMMARY seeds %d, invariant failures %d, aborts %d\n",
	    nseeds, fails, aborts);
	return (0);
}
