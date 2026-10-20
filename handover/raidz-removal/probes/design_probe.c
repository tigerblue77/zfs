/*
 * Scratch design probe (not for upstream).  Userspace libzpool on sparse
 * file vdevs only.  Scenarios:
 *   create2  pool created with two top-level vdevs at once (disk + raidz),
 *            as in remove_raidz.ksh: per-vdev alloc and today's removal
 *            results.
 *   churn    new raidz added to a raidz pool, then pool metadata churn
 *            (spa_prop_set) with and without spa_vdev_noalloc(): does the
 *            new vdev stay empty?  Uses 40 GiB sparse files so that the
 *            vdev has more than zfs_embedded_slog_min_ms metaslabs.
 *   midhole  [A B] + log L + normal C, remove L: hole between normal vdevs;
 *            export, import, print the tree, add another vdev (hole slot
 *            reuse?).
 *
 * Usage: design_probe <scratch-dir> <scenario>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/metaslab_impl.h>
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
static uint64_t fsize = 512ULL << 20;

static nvlist_t *
mk_file(void)
{
	char path[1024];
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
	return (f);
}

static nvlist_t *
mk_top(const char *type, int n, int nparity, int islog)
{
	if (strcmp(type, VDEV_TYPE_FILE) == 0) {
		nvlist_t *f = mk_file();
		fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, islog);
		return (f);
	}
	nvlist_t **c = malloc(n * sizeof (nvlist_t *));
	nvlist_t *t = fnvlist_alloc();

	for (int i = 0; i < n; i++)
		c[i] = mk_file();
	fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, type);
	if (strcmp(type, VDEV_TYPE_RAIDZ) == 0)
		fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, nparity);
	fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, islog);
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

static void
mkpool(const char *pool, nvlist_t *root)
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
	VERIFY0(spa_create(pool, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
}

static int
add_top(const char *pool, nvlist_t *top)
{
	spa_t *spa;
	nvlist_t *tops[1] = { top };
	nvlist_t *root = mk_root(tops, 1);

	VERIFY0(spa_open(pool, &spa, FTAG));
	int err = spa_vdev_add(spa, root, B_FALSE);
	fnvlist_free(root);
	spa_close(spa, FTAG);
	return (err);
}

static void
tree(const char *pool, const char *lbl)
{
	spa_t *spa;

	VERIFY0(spa_open(pool, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	(void) printf("  tree %s (children=%llu):\n", lbl,
	    (u_longlong_t)rvd->vdev_children);
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *vd = rvd->vdev_child[c];
		(void) printf("    id %llu type=%-8s hole=%d log=%llu "
		    "alloc=%llu space=%llu noalloc=%llu ms=%llu log_mg=%s "
		    "guid=%llu\n", (u_longlong_t)vd->vdev_id,
		    vd->vdev_ops->vdev_op_type, (int)vd->vdev_ishole,
		    (u_longlong_t)vd->vdev_islog,
		    (u_longlong_t)vd->vdev_stat.vs_alloc,
		    (u_longlong_t)vd->vdev_stat.vs_space,
		    (u_longlong_t)vd->vdev_noalloc,
		    (u_longlong_t)vd->vdev_ms_count,
		    vd->vdev_log_mg != NULL ? "yes" : "no",
		    (u_longlong_t)vd->vdev_guid);
	}
	(void) printf("    root: alloc=%llu space=%llu nonalloc_dspace=%llu\n",
	    (u_longlong_t)rvd->vdev_stat.vs_alloc,
	    (u_longlong_t)rvd->vdev_stat.vs_space,
	    (u_longlong_t)spa->spa_nonallocating_dspace);
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

static uint64_t
leaf_guid(const char *pool, uint64_t id, uint64_t leaf)
{
	spa_t *spa;

	VERIFY0(spa_open(pool, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	uint64_t g = vdev_lookup_top(spa, id)->vdev_child[leaf]->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static int
do_remove(const char *pool, uint64_t guid)
{
	spa_t *spa;

	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = spa_vdev_remove(spa, guid, B_FALSE);
	spa_close(spa, FTAG);
	return (e);
}

static void
sync_n(const char *pool, int n)
{
	spa_t *spa;

	for (int i = 0; i < n; i++) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
	}
}

static void
set_comment(const char *pool, int i)
{
	spa_t *spa;
	char buf[64];
	nvlist_t *nvp = fnvlist_alloc();

	(void) snprintf(buf, sizeof (buf), "probe comment %d", i);
	fnvlist_add_string(nvp, "comment", buf);
	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = spa_prop_set(spa, nvp);
	if (e != 0)
		(void) printf("    spa_prop_set error %d\n", e);
	spa_close(spa, FTAG);
	fnvlist_free(nvp);
}

static void
create2(void)
{
	nvlist_t *t[2];
	const char *pool = "pcreate";

	(void) printf("[create2] pool created with [file, raidz1(2 files)]"
	    " (remove_raidz.ksh layout)\n");
	t[0] = mk_top(VDEV_TYPE_FILE, 1, 0, 0);
	t[1] = mk_top(VDEV_TYPE_RAIDZ, 2, 1, 0);
	mkpool(pool, mk_root(t, 2));
	tree(pool, "right after spa_create");
	sync_n(pool, 2);
	tree(pool, "after 2 syncs");
	(void) printf("  remove disk top (id0):  error %d\n",
	    do_remove(pool, top_guid(pool, 0)));
	(void) printf("  remove raidz leaf:      error %d\n",
	    do_remove(pool, leaf_guid(pool, 1, 0)));
	(void) printf("  remove raidz top (id1): error %d\n",
	    do_remove(pool, top_guid(pool, 1)));
	VERIFY0(spa_destroy(pool));
}

static void
churn(boolean_t noalloc)
{
	spa_t *spa;
	const char *pool = noalloc ? "pchurnB" : "pchurnA";
	nvlist_t *t[1];

	fsize = 40ULL << 30;
	(void) printf("[churn] base raidz1(3) + add raidz2(3), 40GiB sparse "
	    "files, noalloc right after add: %s\n", noalloc ? "yes" : "no");
	t[0] = mk_top(VDEV_TYPE_RAIDZ, 3, 1, 0);
	mkpool(pool, mk_root(t, 1));
	(void) printf("  add: error %d\n",
	    add_top(pool, mk_top(VDEV_TYPE_RAIDZ, 3, 2, 0)));
	if (noalloc) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		(void) printf("  spa_vdev_noalloc: %d\n",
		    spa_vdev_noalloc(spa, top_guid(pool, 1)));
		spa_close(spa, FTAG);
	}
	tree(pool, "right after add");
	for (int i = 1; i <= 6; i++) {
		set_comment(pool, i);
		sync_n(pool, 1);
	}
	tree(pool, "after 6 x (spa_prop_set comment + sync)");
	(void) printf("  remove new raidz (today): error %d\n",
	    do_remove(pool, top_guid(pool, 1)));
	VERIFY0(spa_destroy(pool));
	fsize = 512ULL << 20;
}

static void
midhole(void)
{
	const char *pool = "pmid";
	nvlist_t *t[2], *cfg = NULL;
	spa_t *spa;

	(void) printf("[midhole] [A B] + log L + normal C; remove L\n");
	t[0] = mk_top(VDEV_TYPE_FILE, 1, 0, 0);
	t[1] = mk_top(VDEV_TYPE_FILE, 1, 0, 0);
	mkpool(pool, mk_root(t, 2));
	(void) printf("  add log: %d\n",
	    add_top(pool, mk_top(VDEV_TYPE_FILE, 1, 0, 1)));
	(void) printf("  add normal: %d\n",
	    add_top(pool, mk_top(VDEV_TYPE_FILE, 1, 0, 0)));
	tree(pool, "before log removal");
	(void) printf("  remove log (id2): error %d\n",
	    do_remove(pool, top_guid(pool, 2)));
	tree(pool, "after log removal (hole between normal vdevs)");
	(void) printf("  add another normal: %d\n",
	    add_top(pool, mk_top(VDEV_TYPE_FILE, 1, 0, 0)));
	tree(pool, "after one more add (hole slot reused?)");
	VERIFY0(spa_open(pool, &spa, FTAG));
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, &cfg, B_FALSE, B_FALSE));
	(void) printf("  exported; config has hole_array/children: ");
	{
		uint64_t *holes, nch = 0;
		uint_t nh = 0;

		(void) nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_VDEV_CHILDREN,
		    &nch);
		if (nvlist_lookup_uint64_array(cfg, ZPOOL_CONFIG_HOLE_ARRAY,
		    &holes, &nh) == 0)
			(void) printf("children=%llu holes=%u first=%llu\n",
			    (u_longlong_t)nch, nh, (u_longlong_t)holes[0]);
		else
			(void) printf("children=%llu no hole array\n",
			    (u_longlong_t)nch);
	}
	int e = spa_import((char *)pool, cfg, NULL, 0);
	(void) printf("  spa_import: error %d\n", e);
	if (e == 0)
		tree(pool, "after export/import");
	fnvlist_free(cfg);
	if (e == 0)
		VERIFY0(spa_destroy(pool));
}

int
main(int argc, char **argv)
{
	char *cache;

	if (argc != 3)
		return (2);
	dir = argv[1];
	if (asprintf(&cache, "%s/zpool.cache", dir) < 0)
		return (2);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (strcmp(argv[2], "create2") == 0)
		create2();
	else if (strcmp(argv[2], "churn") == 0) {
		churn(B_FALSE);
		churn(B_TRUE);
	} else if (strcmp(argv[2], "midhole") == 0)
		midhole();
	kernel_fini();
	return (0);
}
