/*
 * Scratch probe (not for upstream): drive spa_vdev_remove()/spa_vdev_add()
 * directly through libzpool on file-backed pools, in userspace only, to show
 * what the current tree accepts and refuses for top-level raidz vdevs.
 *
 * Usage: rm_probe <scratch-dir>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/txg.h>
#include <sys/dsl_pool.h>
#include <sys/zfs_debug.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *dir;
static int nfile;

static nvlist_t *
mk_file(void)
{
	char path[1024];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile++);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 512ULL << 20));
	(void) close(fd);

	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	return (f);
}

/* type is "mirror" or "raidz"; nparity only used for raidz */
static nvlist_t *
mk_top(const char *type, int n, int nparity)
{
	if (strcmp(type, VDEV_TYPE_FILE) == 0) {
		nvlist_t *f = mk_file();
		fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
		return (f);
	}
	nvlist_t **c = malloc(n * sizeof (nvlist_t *));
	nvlist_t *t = fnvlist_alloc();

	for (int i = 0; i < n; i++)
		c[i] = mk_file();
	fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, type);
	if (strcmp(type, VDEV_TYPE_RAIDZ) == 0)
		fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, nparity);
	fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(t, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	free(c);
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

static int
try_add(const char *pool, nvlist_t *top, const char *what)
{
	spa_t *spa;
	nvlist_t *tops[1] = { top };
	nvlist_t *root = mk_root(tops, 1);

	VERIFY0(spa_open(pool, &spa, FTAG));
	int err = spa_vdev_add(spa, root, B_FALSE);
	fnvlist_free(root);
	(void) printf("  add %s: error %d\n", what, err);
	spa_close(spa, FTAG);
	return (err);
}

static void
mkpool(const char *pool, nvlist_t *root)
{
	nvlist_t *props = fnvlist_alloc();

	/* Enable every supported feature, as ztest does. */
	for (int i = 0; i < SPA_FEATURES; i++) {
		char *buf;

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		VERIFY3S(-1, !=, asprintf(&buf, "feature@%s",
		    spa_feature_table[i].fi_uname));
		fnvlist_add_uint64(props, buf, 0);
		free(buf);
	}
	VERIFY0(spa_create(pool, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
}

static void
finish(const char *pool)
{
	VERIFY0(spa_destroy(pool));
}


static void
show(const char *pool, uint64_t id, const char *lbl)
{
	spa_t *spa;

	VERIFY0(spa_open(pool, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *vd = vdev_lookup_top(spa, id);
	(void) printf("    %-34s top%llu(%s): alloc=%llu space=%llu noalloc=%llu\n",
	    lbl, (u_longlong_t)id, vd->vdev_ops->vdev_op_type,
	    (u_longlong_t)vd->vdev_stat.vs_alloc,
	    (u_longlong_t)vd->vdev_stat.vs_space,
	    (u_longlong_t)vd->vdev_noalloc);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
}

static uint64_t
top_guid(const char *pool, uint64_t id)
{
	spa_t *spa;
	VERIFY0(spa_open(pool, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	uint64_t g = vdev_lookup_top(spa, id)->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static void
syncs(const char *pool, uint64_t id, int n)
{
	spa_t *spa;
	char lbl[64];

	for (int i = 1; i <= n; i++) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
		(void) snprintf(lbl, sizeof (lbl), "after extra sync #%d", i);
		show(pool, id, lbl);
	}
}

static void
scenario(const char *pool, const char *basetype, int bn, int bp,
    const char *addtype, int an, int ap, boolean_t noalloc_first)
{
	spa_t *spa;
	nvlist_t *t[1] = { mk_top(basetype, bn, bp) };

	(void) printf("[%s] base %s(%d) + add %s(%d), noalloc right after add: %s\n",
	    pool, basetype, bn, addtype, an, noalloc_first ? "yes" : "no");
	mkpool(pool, mk_root(t, 1));
	show(pool, 0, "base after create");
	(void) try_add(pool, mk_top(addtype, an, ap), "new top-level vdev");
	show(pool, 1, "right after spa_vdev_add returned");
	if (noalloc_first) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		int e = spa_vdev_noalloc(spa, top_guid(pool, 1));
		(void) printf("    spa_vdev_noalloc -> %d\n", e);
		spa_close(spa, FTAG);
		show(pool, 1, "right after noalloc");
	}
	syncs(pool, 1, 3);
	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = spa_vdev_remove(spa, top_guid(pool, 1), B_FALSE);
	(void) printf("    spa_vdev_remove of the new vdev (today): error %d\n", e);
	spa_close(spa, FTAG);
	finish(pool);
}

int
main(int argc, char **argv)
{
	char *cache;

	if (argc != 2)
		return (2);
	dir = argv[1];
	if (asprintf(&cache, "%s/zpool.cache", dir) < 0)
		return (2);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	scenario("pA", "raidz", 3, 1, "raidz", 3, 2, B_FALSE);
	scenario("pB", "raidz", 3, 1, "raidz", 3, 2, B_TRUE);
	scenario("pC", "raidz", 3, 1, VDEV_TYPE_FILE, 1, 0, B_FALSE);
	scenario("pD", "mirror", 2, 0, "raidz", 3, 1, B_FALSE);
	kernel_fini();
	return (0);
}
