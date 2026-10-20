/*
 * Scratch probe (not for upstream): import by scanning the devices, without
 * the cache file, after an interrupted removal of an empty top-level vdev.
 *
 *   crash  The removal panics in spa_vdev_exit(), before the txg that it
 *          waits for has synced.
 *   stale  The removal completes, then the leaves of the removed vdev get
 *          back the labels they had before it: the state of a crash after
 *          the hole is on disk and before the labels are erased.
 *
 * Usage: label_window_probe <scratch-dir> crash|stale [moved] [log]
 * "log": vdev 1 is a log disk, removed by spa_vdev_remove_log().
 * Exit 0 when the pool imports by scanning with vdev 1 a hole or the
 * untouched raidz, 1 otherwise.
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/zio.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

static const char *pool = "lwpool";
static char dir[1024];
static char cache[1100];
static int nfile;
static boolean_t logmode;

static nvlist_t *
mk_file(void)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile++);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 256ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	return (f);
}

static nvlist_t *
mk_raidz_root(int nparity)
{
	nvlist_t *c[3], *t = fnvlist_alloc(), *r = fnvlist_alloc();

	for (int i = 0; i < 3; i++)
		c[i] = mk_file();
	fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, nparity);
	fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(t, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, 3);
	for (int i = 0; i < 3; i++)
		fnvlist_free(c[i]);
	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)&t, 1);
	fnvlist_free(t);
	return (r);
}

static int64_t
label_txg(const char *path)
{
	vdev_label_t *vl = malloc(sizeof (vdev_label_t));
	nvlist_t *nv = NULL;
	uint64_t txg = 0;
	int fd = open(path, O_RDONLY);
	int64_t r = -1;

	if (fd >= 0 && pread(fd, vl, sizeof (*vl), 0) == sizeof (*vl) &&
	    nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    sizeof (vl->vl_vdev_phys.vp_nvlist), &nv, 0) == 0) {
		if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_POOL_TXG, &txg) == 0)
			r = (int64_t)txg;
		nvlist_free(nv);
	}
	if (fd >= 0)
		(void) close(fd);
	free(vl);
	return (r);
}

/* Copy the label areas, the first and the last MB of a 256M file. */
static void
copy_file(const char *from, const char *to)
{
	char *buf = malloc(1 << 20);
	int in = open(from, O_RDONLY);
	int out = open(to, O_WRONLY | O_CREAT, 0666);
	off_t offs[2] = { 0, (256 << 20) - (1 << 20) };

	VERIFY3S(in, >=, 0);
	VERIFY3S(out, >=, 0);
	VERIFY0(ftruncate(out, 256ULL << 20));
	for (int i = 0; i < 2; i++) {
		VERIFY3S(pread(in, buf, 1 << 20, offs[i]), ==, 1 << 20);
		VERIFY3S(pwrite(out, buf, 1 << 20, offs[i]), ==, 1 << 20);
	}
	(void) close(in);
	(void) close(out);
	free(buf);
}

/* Pool with raidz1-0 and an empty, non-allocating raidz2-1. */
static uint64_t
setup(void)
{
	spa_t *spa;
	uint64_t guid;

	VERIFY0(spa_create(pool, mk_raidz_root(1), NULL, NULL, NULL, NULL));
	VERIFY0(spa_open(pool, &spa, FTAG));
	txg_wait_synced(spa_get_dsl(spa), 0);
	nvlist_t *r;
	if (logmode) {
		nvlist_t *f = mk_file();
		fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 1);
		r = fnvlist_alloc();
		fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
		fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)&f, 1);
		fnvlist_free(f);
		nfile += 2;	/* keep vd.3 to vd.5 as the vdev 1 files */
	} else {
		r = mk_raidz_root(2);
	}
	VERIFY0(spa_vdev_add(spa, r, B_FALSE));
	fnvlist_free(r);
	guid = spa->spa_root_vdev->vdev_child[1]->vdev_guid;
	if (!logmode)
		VERIFY0(spa_vdev_noalloc(spa, guid));
	txg_wait_synced(spa_get_dsl(spa), 0);
	spa_close(spa, FTAG);
	return (guid);
}

static void
crash_child(void)
{
	spa_t *spa;
	zinject_record_t rec = { 0 };
	int id;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	uint64_t guid = setup();
	rec.zi_cmd = ZINJECT_PANIC;
	(void) strlcpy(rec.zi_func, "spa_vdev_exit", sizeof (rec.zi_func));
	VERIFY0(zio_inject_fault((char *)pool, 0, &id, &rec));
	VERIFY0(spa_open(pool, &spa, FTAG));
	(void) printf("child: removing vdev 1, panic in spa_vdev_exit\n");
	(void) fflush(stdout);
	(void) spa_vdev_remove(spa, guid, B_FALSE);
	(void) printf("child: removal returned\n");
	_exit(3);
}

static void
stale(void)
{
	spa_t *spa;
	char p[1200], s[1200];

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	uint64_t guid = setup();
	/* Nothing is dirty and vdev 1 is idle: its files are stable. */
	for (int i = 3; i < 6; i++) {
		(void) snprintf(p, sizeof (p), "%s/vd.%d", dir, i);
		(void) snprintf(s, sizeof (s), "%s/saved.%d", dir, i);
		if (access(p, F_OK) == 0)
			copy_file(p, s);
	}
	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = spa_vdev_remove(spa, guid, B_FALSE);
	(void) printf("removal: %d, vdev 1 is %s\n", e,
	    spa->spa_root_vdev->vdev_child[1]->vdev_ops->vdev_op_type);
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	for (int i = 3; i < 6; i++) {
		(void) snprintf(p, sizeof (p), "%s/vd.%d", dir, i);
		(void) snprintf(s, sizeof (s), "%s/saved.%d", dir, i);
		if (access(s, F_OK) == 0)
			copy_file(s, p);
		(void) unlink(s);
	}
	kernel_fini();
}

int
main(int argc, char **argv)
{
	spa_t *spa;
	int st;

	if (argc < 3)
		return (2);
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	for (int i = 3; i < argc; i++)
		if (strcmp(argv[i], "log") == 0)
			logmode = B_TRUE;
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;

	pid_t pid = fork();
	if (pid == 0) {
		if (strcmp(argv[2], "crash") == 0)
			crash_child();
		stale();
		_exit(0);
	}
	VERIFY3S(waitpid(pid, &st, 0), ==, pid);
	(void) printf("child status %#x%s\n", st,
	    WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT ? " (panic)" : "");

	for (int i = 0; i < 6; i++) {
		char p[1200];

		(void) snprintf(p, sizeof (p), "%s/vd.%d", dir, i);
		(void) printf("label txg of vd.%d: %lld\n", i,
		    (long long)label_txg(p));
	}

	/*
	 * "moved": the devices are found under other names, as after they
	 * were renumbered or moved to another host, so that the paths in the
	 * pool's own config no longer lead to them.
	 */
	char scandir[1200];
	(void) strlcpy(scandir, dir, sizeof (scandir));
	if ((argc > 3 && strcmp(argv[3], "moved") == 0) ||
	    (argc > 4 && strcmp(argv[4], "moved") == 0)) {
		(void) snprintf(scandir, sizeof (scandir), "%s/moved", dir);
		VERIFY0(mkdir(scandir, 0755));
		for (int i = 0; i < 6; i++) {
			char p[1200], q[1300];

			(void) snprintf(p, sizeof (p), "%s/vd.%d", dir, i);
			(void) snprintf(q, sizeof (q), "%s/disk%d", scandir, i);
			if (access(p, F_OK) == 0)
				VERIFY0(rename(p, q));
		}
	}

	/* No cache file: find the pool by scanning, as "zpool import -d". */
	(void) unlink(cache);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	importargs_t args = { 0 };
	char *paths[1] = { scandir };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	nvlist_t *cfg;
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	int e = zpool_find_config(&lpch, pool, &cfg, &args);
	(void) printf("scan: %d\n", e);
	if (e != 0)
		return (1);
	e = spa_import((char *)pool, cfg, NULL, 0);
	(void) printf("import: %d\n", e);
	if (e != 0) {
		/* As "zpool import -m". */
		e = spa_import((char *)pool, cfg, NULL, ZFS_IMPORT_MISSING_LOG);
		(void) printf("import -m: %d\n", e);
	}
	nvlist_free(cfg);
	if (e != 0)
		return (1);
	VERIFY0(spa_open(pool, &spa, FTAG));
	vdev_t *rvd = spa->spa_root_vdev;
	(void) printf("vdev 1 after import: %s, state %llu\n",
	    rvd->vdev_child[1]->vdev_ops->vdev_op_type,
	    (u_longlong_t)rvd->vdev_child[1]->vdev_state);
	boolean_t ok = (rvd->vdev_state == VDEV_STATE_HEALTHY);
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (ok ? 0 : 1);
}
