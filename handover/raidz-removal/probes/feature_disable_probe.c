/*
 * Scratch probe (not for upstream): userspace libzpool, sparse file vdevs only.
 * Exercises spa_feature_disable() from the local "feature-disable" prototype
 * (openzfs/zfs#19439): disabling, refusals, dependencies, re-enabling, and
 * what survives an export and import.
 *
 * Usage: feature_disable_probe <scratch-root> [scenario ...]
 *
 * Each scenario runs in its own forked process (own kernel_init) in its own
 * sub-directory and leaves its pool exported for zdb; the pools are listed in
 * <scratch-root>/manifest.txt.
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/txg.h>
#include <sys/dsl_pool.h>
#include <sys/zfs_debug.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
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

static const char *root;
static char dir[1024];
static char poolname[64];
static int nfile;
static uint64_t fsize = 256ULL << 20;
static int npass, nfail;

static void
chk(boolean_t ok, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	(void) printf("  %s %s\n", ok ? "PASS" : "FAIL", buf);
	if (ok)
		npass++;
	else
		nfail++;
}

static void
chk_err(const char *what, int got, int exp)
{
	chk(got == exp, "%s: error %d (expected %d)", what, got, exp);
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
mk_top(const char *type, int n, int nparity)
{
	if (strcmp(type, VDEV_TYPE_FILE) == 0)
		return (mk_file());

	nvlist_t **c = malloc(n * sizeof (nvlist_t *));
	nvlist_t *t = fnvlist_alloc();

	for (int i = 0; i < n; i++)
		c[i] = mk_file();
	fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, type);
	fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, nparity);
	if (strcmp(type, VDEV_TYPE_DRAID) == 0) {
		uint64_t ndata = 2, nspares = 1, ngroups = 1;

		while (ngroups * (ndata + nparity) % (n - nspares) != 0)
			ngroups++;
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NDATA, ndata);
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NSPARES, nspares);
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NGROUPS, ngroups);
	}
	fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(t, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	free(c);
	return (t);
}

static nvlist_t *
mk_root1(nvlist_t *top)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)&top, 1);
	fnvlist_free(top);
	return (r);
}

/* All supported features enabled, except those named in skip. */
static void
mkpool(nvlist_t *top, const char *skip)
{
	nvlist_t *props = fnvlist_alloc();
	nvlist_t *rt = mk_root1(top);

	for (int i = 0; i < SPA_FEATURES; i++) {
		char *buf;

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		if (skip != NULL &&
		    strstr(skip, spa_feature_table[i].fi_uname) != NULL)
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
sync_n(int n)
{
	spa_t *spa;

	for (int i = 0; i < n; i++) {
		VERIFY0(spa_open(poolname, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
	}
}

static int
disable(spa_feature_t f)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_feature_disable(spa, f);
	spa_close(spa, FTAG);
	return (e);
}

static int
enable_prop(spa_feature_t f)
{
	spa_t *spa;
	char *name;
	nvlist_t *nvp = fnvlist_alloc();

	VERIFY3S(-1, !=, asprintf(&name, "feature@%s",
	    spa_feature_table[f].fi_uname));
	fnvlist_add_uint64(nvp, name, 0);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_prop_set(spa, nvp);
	spa_close(spa, FTAG);
	fnvlist_free(nvp);
	free(name);
	return (e);
}

static boolean_t
enabled(spa_feature_t f)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	boolean_t r = spa_feature_is_enabled(spa, f);
	spa_close(spa, FTAG);
	return (r);
}

static boolean_t
active(spa_feature_t f)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	boolean_t r = spa_feature_is_active(spa, f);
	spa_close(spa, FTAG);
	return (r);
}

static uint64_t
refcount_of(spa_feature_t f)
{
	spa_t *spa;
	uint64_t rc = UINT64_MAX;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	(void) feature_get_refcount(spa, &spa_feature_table[f], &rc);
	spa_close(spa, FTAG);
	return (rc);
}

/* On-disk entries: 1 bit each for features_for_*, descriptions, txg. */
static int
ondisk(spa_feature_t f, uint64_t *txgp)
{
	spa_t *spa;
	zfeature_info_t *fi = &spa_feature_table[f];
	int r = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	objset_t *mos = spa->spa_meta_objset;
	uint64_t obj = (fi->fi_flags & ZFEATURE_FLAG_READONLY_COMPAT) ?
	    spa->spa_feat_for_write_obj : spa->spa_feat_for_read_obj;
	uint64_t other = (fi->fi_flags & ZFEATURE_FLAG_READONLY_COMPAT) ?
	    spa->spa_feat_for_read_obj : spa->spa_feat_for_write_obj;

	if (zap_contains(mos, obj, fi->fi_guid) == 0)
		r |= 1;
	if (zap_contains(mos, other, fi->fi_guid) == 0)
		r |= 8;
	if (zap_contains(mos, spa->spa_feat_desc_obj, fi->fi_guid) == 0)
		r |= 2;
	if (spa->spa_feat_enabled_txg_obj != 0 &&
	    zap_lookup(mos, spa->spa_feat_enabled_txg_obj, fi->fi_guid,
	    sizeof (uint64_t), 1, txgp) == 0)
		r |= 4;
	spa_close(spa, FTAG);
	return (r);
}

/* Is the feature listed in the stats that "zpool get" reads? */
static boolean_t
in_stats(spa_feature_t f)
{
	nvlist_t *cfg = NULL, *feat;
	boolean_t r;

	VERIFY0(spa_get_stats(poolname, &cfg, NULL, 0));
	feat = fnvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_FEATURE_STATS);
	r = nvlist_exists(feat, spa_feature_table[f].fi_guid);
	fnvlist_free(cfg);
	return (r);
}

static boolean_t
history_has(const char *needle)
{
	spa_t *spa;
	uint64_t off = 0, len;
	size_t cap = 1 << 20;
	char *buf = malloc(cap);
	boolean_t found = B_FALSE;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (;;) {
		len = cap;
		if (spa_history_get(spa, &off, &len, buf) != 0 || len == 0)
			break;
		if (memmem(buf, len, needle, strlen(needle)) != NULL)
			found = B_TRUE;
	}
	spa_close(spa, FTAG);
	free(buf);
	return (found);
}

static int
reimport(void)
{
	nvlist_t *cfg = NULL;

	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	int e = spa_import(poolname, cfg, NULL, 0);
	fnvlist_free(cfg);
	return (e);
}

static void
leave_for_zdb(void)
{
	nvlist_t *cfg = NULL;
	char path[1200];
	FILE *f;

	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	fnvlist_free(cfg);
	(void) snprintf(path, sizeof (path), "%s/manifest.txt", root);
	f = fopen(path, "a");
	if (f != NULL) {
		(void) fprintf(f, "%s %s\n", dir, poolname);
		(void) fclose(f);
	}
}

static uint64_t
top_guid(uint64_t id)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	uint64_t g = vdev_lookup_top(spa, id)->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static int
attach_disk(uint64_t guid)
{
	spa_t *spa;
	nvlist_t *rt = mk_root1(mk_file());

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_attach(spa, guid, rt, B_FALSE, B_FALSE);
	spa_close(spa, FTAG);
	fnvlist_free(rt);
	return (e);
}

static int
add_top(nvlist_t *top)
{
	spa_t *spa;
	nvlist_t *rt = mk_root1(top);

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_add(spa, rt, B_FALSE);
	spa_close(spa, FTAG);
	fnvlist_free(rt);
	return (e);
}

static void
wait_expansion(void)
{
	spa_t *spa;

	for (int i = 0; i < 600; i++) {
		VERIFY0(spa_open(poolname, &spa, FTAG));
		boolean_t busy = spa->spa_raidz_expand != NULL;
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
		if (!busy)
			return;
		(void) usleep(100000);
	}
	chk(B_FALSE, "raidz expansion finished within 60 s");
}

/*
 * multi_vdev_crash_dump: never activated.  Disable it, check every trace is
 * gone and stays gone across an import, then enable it again.
 */
static void
sc_crashdump(void)
{
	spa_feature_t f = SPA_FEATURE_MULTI_VDEV_CRASH_DUMP;
	uint64_t txg1 = 0, txg2 = 0, etxg_rc, etxg_after;
	int had_txg;

	mkpool(mk_top("file", 1, 0), NULL);
	sync_n(2);
	chk(enabled(f) && refcount_of(f) == 0, "enabled with refcount 0");
	int od = ondisk(f, &txg1);
	had_txg = (od & 4) != 0;
	chk((od & 3) == 3 && (od & 8) == 0,
	    "for_read and description entries present (enabled_txg entry: "
	    "%s, enabled before enabled_txg at creation)",
	    had_txg ? "yes" : "no");
	etxg_rc = refcount_of(SPA_FEATURE_ENABLED_TXG);
	chk(in_stats(f), "listed in the feature stats");

	chk_err("disable", disable(f), 0);
	chk(!enabled(f), "no longer enabled");
	chk(ondisk(f, &txg2) == 0, "no entry left on disk");
	etxg_after = refcount_of(SPA_FEATURE_ENABLED_TXG);
	chk(etxg_after == etxg_rc - had_txg,
	    "enabled_txg refcount %llu -> %llu", (u_longlong_t)etxg_rc,
	    (u_longlong_t)etxg_after);
	chk(!in_stats(f), "no longer listed in the feature stats");
	chk(history_has("feature@multi_vdev_crash_dump=disabled"),
	    "history records the disable");
	chk_err("disable again (no-op)", disable(f), 0);

	chk(reimport() == 0, "export + import");
	chk(!enabled(f) && ondisk(f, &txg2) == 0 && !in_stats(f),
	    "still disabled after import");
	chk(refcount_of(SPA_FEATURE_ENABLED_TXG) == etxg_after,
	    "enabled_txg refcount kept after import");

	chk_err("enable again", enable_prop(f), 0);
	sync_n(1);
	chk(enabled(f) && refcount_of(f) == 0, "enabled again, refcount 0");
	chk(ondisk(f, &txg2) == 7 && txg2 > txg1,
	    "entries back, now with an enabled txg (%llu)",
	    (u_longlong_t)txg2);
	chk(refcount_of(SPA_FEATURE_ENABLED_TXG) == etxg_after + 1,
	    "enabled_txg refcount %llu -> %llu", (u_longlong_t)etxg_after,
	    (u_longlong_t)refcount_of(SPA_FEATURE_ENABLED_TXG));

	/* Now the enabled_txg entry exists: disabling must give it back. */
	chk_err("disable with an enabled_txg entry", disable(f), 0);
	chk(ondisk(f, &txg2) == 0 &&
	    refcount_of(SPA_FEATURE_ENABLED_TXG) == etxg_after,
	    "entry and enabled_txg reference given back");
	chk_err("enable again", enable_prop(f), 0);
	sync_n(1);
	chk(reimport() == 0, "export + import");
	chk(enabled(f) && in_stats(f), "still enabled after import");
	leave_for_zdb();
}

/*
 * raidz_expansion: disabled, an attach to the raidz is refused; enabled
 * again, the attach expands the vdev, and the feature is then busy.
 */
static void
sc_raidz_expansion(void)
{
	spa_feature_t f = SPA_FEATURE_RAIDZ_EXPANSION;

	mkpool(mk_top("raidz", 3, 1), NULL);
	sync_n(2);
	chk(enabled(f) && !active(f), "enabled, not active");
	chk_err("disable", disable(f), 0);
	chk(!enabled(f), "no longer enabled");
	chk_err("attach to the raidz while disabled",
	    attach_disk(top_guid(0)), ENOTSUP);
	chk(reimport() == 0, "export + import");
	chk(!enabled(f), "still disabled after import");
	chk_err("enable again", enable_prop(f), 0);
	sync_n(1);
	chk_err("attach to the raidz", attach_disk(top_guid(0)), 0);
	wait_expansion();
	chk(active(f), "active after the expansion");
	chk_err("disable while active", disable(f), EBUSY);
	chk(enabled(f), "still enabled");
	chk(reimport() == 0, "export + import");
	chk(active(f), "still active after import");
	leave_for_zdb();
}

/*
 * draid and draid_failure_domains, which depends on it.
 */
static void
sc_draid_deps(void)
{
	spa_feature_t d = SPA_FEATURE_DRAID;
	spa_feature_t fd = SPA_FEATURE_DRAID_FAIL_DOMAINS;

	mkpool(mk_top("file", 1, 0), NULL);
	sync_n(2);
	chk(enabled(d) && enabled(fd) && !active(d), "both enabled, inactive");
	chk_err("disable draid while draid_failure_domains is enabled",
	    disable(d), EBUSY);
	chk(enabled(d), "draid still enabled");
	chk_err("disable draid_failure_domains", disable(fd), 0);
	chk_err("disable draid", disable(d), 0);
	chk(!enabled(d) && !enabled(fd), "both disabled");
	chk_err("add a draid vdev while draid is disabled",
	    add_top(mk_top("draid", 5, 1)), ENOTSUP);
	chk(reimport() == 0, "export + import");
	chk(!enabled(d) && !enabled(fd), "both still disabled after import");
	chk_err("enable draid_failure_domains", enable_prop(fd), 0);
	sync_n(1);
	chk(enabled(d) && enabled(fd),
	    "enabling draid_failure_domains enabled draid too");
	chk_err("add a draid vdev", add_top(mk_top("draid", 5, 1)), 0);
	sync_n(1);
	chk(active(d), "draid active");
	chk_err("disable draid while active", disable(d), EBUSY);
	chk_err("disable draid_failure_domains (no domains, refcount 0)",
	    disable(fd), 0);
	chk(active(d) && !enabled(fd), "draid active, failure domains off");
	chk(reimport() == 0, "export + import");
	chk(active(d) && !enabled(fd), "unchanged after import");
	leave_for_zdb();
}

/*
 * Features without ZFEATURE_FLAG_DISABLE_OK, and features never enabled.
 */
static void
sc_refused(void)
{
	mkpool(mk_top("file", 1, 0), "multi_vdev_crash_dump");
	sync_n(2);
	chk_err("disable zstd_compress", disable(SPA_FEATURE_ZSTD_COMPRESS),
	    ENOTSUP);
	chk_err("disable enabled_txg", disable(SPA_FEATURE_ENABLED_TXG),
	    ENOTSUP);
	chk_err("disable allocation_classes",
	    disable(SPA_FEATURE_ALLOCATION_CLASSES), ENOTSUP);
	chk(enabled(SPA_FEATURE_ZSTD_COMPRESS) &&
	    enabled(SPA_FEATURE_ENABLED_TXG) &&
	    enabled(SPA_FEATURE_ALLOCATION_CLASSES), "all still enabled");
	chk(!enabled(SPA_FEATURE_MULTI_VDEV_CRASH_DUMP),
	    "multi_vdev_crash_dump not enabled at creation");
	chk_err("disable a feature that is not enabled (no-op)",
	    disable(SPA_FEATURE_MULTI_VDEV_CRASH_DUMP), 0);
	leave_for_zdb();
}

/* feature@<name>=disabled through spa_prop_set(), as the ioctl does. */
static int
disable_prop(spa_feature_t f, const char *comment)
{
	spa_t *spa;
	char *name;
	nvlist_t *nvp = fnvlist_alloc();

	VERIFY3S(-1, !=, asprintf(&name, "feature@%s",
	    spa_feature_table[f].fi_uname));
	fnvlist_add_uint64(nvp, name, ZPOOL_FEATURE_DISABLE);
	if (comment != NULL)
		fnvlist_add_string(nvp, "comment", comment);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_prop_set(spa, nvp);
	spa_close(spa, FTAG);
	fnvlist_free(nvp);
	free(name);
	return (e);
}

static boolean_t
comment_is(const char *want)
{
	spa_t *spa;
	boolean_t r;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	r = spa->spa_comment != NULL && strcmp(spa->spa_comment, want) == 0;
	spa_close(spa, FTAG);
	return (r);
}

/*
 * Through spa_prop_set(), alone and mixed with another property in one
 * request, and the refusals it returns.
 */
static void
sc_propset(void)
{
	spa_feature_t c = SPA_FEATURE_MULTI_VDEV_CRASH_DUMP;
	spa_feature_t r = SPA_FEATURE_RAIDZ_EXPANSION;

	mkpool(mk_top("file", 1, 0), NULL);
	sync_n(2);
	chk_err("feature@multi_vdev_crash_dump=disabled",
	    disable_prop(c, NULL), 0);
	chk(!enabled(c), "disabled");
	chk_err("feature@raidz_expansion=disabled with a comment in the same "
	    "request", disable_prop(r, "probe one"), 0);
	sync_n(1);
	chk(!enabled(r), "raidz_expansion disabled, not enabled again by "
	    "spa_sync_props()");
	chk(comment_is("probe one"), "the comment was set too");
	chk_err("feature@zstd_compress=disabled",
	    disable_prop(SPA_FEATURE_ZSTD_COMPRESS, "probe two"), ENOTSUP);
	chk(enabled(SPA_FEATURE_ZSTD_COMPRESS) && comment_is("probe one"),
	    "refused request changed nothing, comment included");
	chk_err("feature@draid=disabled while draid_failure_domains is enabled",
	    disable_prop(SPA_FEATURE_DRAID, NULL), EBUSY);
	chk_err("feature@multi_vdev_crash_dump=enabled", enable_prop(c), 0);
	sync_n(1);
	chk(enabled(c), "enabled again");
	chk(reimport() == 0, "export + import");
	chk(enabled(c) && !enabled(r) && comment_is("probe one"),
	    "state kept after import");
	leave_for_zdb();
}

/* A pool imported read-only refuses. */
static void
sc_readonly(void)
{
	nvlist_t *cfg = NULL;

	mkpool(mk_top("file", 1, 0), NULL);
	sync_n(2);
	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	nvlist_t *props = fnvlist_alloc();
	fnvlist_add_uint64(props, zpool_prop_to_name(ZPOOL_PROP_READONLY), 1);
	chk(spa_import(poolname, cfg, props, 0) == 0, "read-only import");
	fnvlist_free(props);
	fnvlist_free(cfg);
	chk_err("disable on a read-only pool",
	    disable(SPA_FEATURE_MULTI_VDEV_CRASH_DUMP), EROFS);
	chk(enabled(SPA_FEATURE_MULTI_VDEV_CRASH_DUMP), "still enabled");
	VERIFY0(spa_export(poolname, NULL, B_FALSE, B_FALSE));
}

typedef struct scenario {
	const char *name;
	void (*fn)(void);
} scenario_t;

static const scenario_t scenarios[] = {
	{ "crashdump", sc_crashdump },
	{ "raidz_expansion", sc_raidz_expansion },
	{ "draid_deps", sc_draid_deps },
	{ "refused", sc_refused },
	{ "readonly", sc_readonly },
	{ "propset", sc_propset },
	{ NULL, NULL }
};

static int
run_one(const scenario_t *sc)
{
	char *cache;
	struct rlimit rl = { 0, 0 };

	(void) setrlimit(RLIMIT_CORE, &rl);
	(void) snprintf(poolname, sizeof (poolname), "fd_%s", sc->name);
	(void) snprintf(dir, sizeof (dir), "%s/%s", root, sc->name);
	(void) mkdir(dir, 0755);
	nfile = 0;
	VERIFY3S(asprintf(&cache, "%s/zpool.cache", dir), !=, -1);
	spa_config_path = cache;
	(void) printf("=== %s\n", sc->name);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	sc->fn();
	kernel_fini();
	(void) printf("=== %s: %d pass, %d FAIL\n", sc->name, npass, nfail);
	return (nfail != 0 ? 1 : 0);
}

int
main(int argc, char **argv)
{
	int total_fail = 0;

	if (argc < 2)
		return (2);
	root = argv[1];
	(void) setvbuf(stdout, NULL, _IOLBF, 0);

	for (const scenario_t *sc = scenarios; sc->name != NULL; sc++) {
		boolean_t selected = argc == 2;

		for (int i = 2; i < argc; i++)
			if (strcmp(argv[i], sc->name) == 0)
				selected = B_TRUE;
		if (!selected)
			continue;
		(void) fflush(stdout);
		pid_t p = fork();
		if (p == 0)
			_exit(run_one(sc));
		int st;
		VERIFY3S(waitpid(p, &st, 0), ==, p);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			(void) printf("=== %s: exit status %#x\n", sc->name,
			    st);
			total_fail++;
		}
	}
	(void) printf("SUMMARY scenarios with failures or aborts: %d\n",
	    total_fail);
	return (total_fail != 0);
}
