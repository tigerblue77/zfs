/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs.
 * A vdev added after a checkpoint is dropped by the rewind, but keeps its
 * label.  A vdev added later takes the same top-level id.  The pool is then
 * imported by scanning, with the devices renamed so that the paths in the
 * config lead nowhere.  "low": the stale device's new name sorts before the
 * current one's; "high": after.  "inplace": no renaming.
 *
 * Usage: rewind_shadow_probe <scratch-dir> low|high|inplace
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/spa_checkpoint.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static char dir[1024];
static char pool[] = "rws";

static nvlist_t *
mk_file(int i, boolean_t special)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, i);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 128ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	if (special)
		fnvlist_add_string(f, ZPOOL_CONFIG_ALLOCATION_BIAS,
		    VDEV_ALLOC_BIAS_SPECIAL);
	return (f);
}

static nvlist_t *
mk_root(nvlist_t **c, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	return (r);
}

static void
add(nvlist_t *top)
{
	spa_t *spa;
	nvlist_t *rt = mk_root(&top, 1);

	VERIFY0(spa_open(pool, &spa, FTAG));
	VERIFY0(spa_vdev_add(spa, rt, B_FALSE));
	txg_wait_synced(spa_get_dsl(spa), 0);
	spa_close(spa, FTAG);
	fnvlist_free(rt);
}

int
main(int argc, char **argv)
{
	char cache[1200];

	if (argc < 3)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	const char *mode = argv[2];
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *t[2] = { mk_file(0, B_FALSE), mk_file(1, B_FALSE) };
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
	VERIFY0(spa_create(pool, mk_root(t, 2), props, NULL, NULL, NULL));
	fnvlist_free(props);

	VERIFY0(spa_checkpoint(pool));
	add(mk_file(2, B_TRUE));	/* id 2, dropped by the rewind */

	nvlist_t *cfg = NULL;
	VERIFY0(spa_export(pool, &cfg, B_FALSE, B_FALSE));
	VERIFY0(spa_import(pool, cfg, NULL, ZFS_IMPORT_CHECKPOINT));
	fnvlist_free(cfg);
	(void) printf("rewound\n");

	add(mk_file(3, B_FALSE));	/* takes id 2 again */
	spa_t *spa;
	VERIFY0(spa_open(pool, &spa, FTAG));
	uint64_t g2 = spa->spa_root_vdev->vdev_child[2]->vdev_guid;
	(void) printf("vdev 2 is %s, guid %llu\n",
	    spa->spa_root_vdev->vdev_child[2]->vdev_path, (u_longlong_t)g2);
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();

	char scandir[1200];
	(void) strlcpy(scandir, dir, sizeof (scandir));
	if (strcmp(mode, "inplace") != 0) {
		boolean_t low = strcmp(mode, "low") == 0;
		const char *nm[4] = { "m0", "m1", low ? "a2" : "z2", "m3" };

		(void) snprintf(scandir, sizeof (scandir), "%s/moved", dir);
		VERIFY0(mkdir(scandir, 0755));
		for (int i = 0; i < 4; i++) {
			char p[1200], q[1300];

			(void) snprintf(p, sizeof (p), "%s/vd.%d", dir, i);
			(void) snprintf(q, sizeof (q), "%s/%s", scandir, nm[i]);
			VERIFY0(rename(p, q));
		}
	}

	(void) unlink(cache);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	importargs_t args = { 0 };
	char *paths[1] = { scandir };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	int e = zpool_find_config(&lpch, pool, &cfg, &args);
	(void) printf("scan: %d\n", e);
	if (e != 0)
		return (1);
	nvlist_t *nvroot = fnvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE);
	nvlist_t **ch;
	uint_t nch;
	VERIFY0(nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    &ch, &nch));
	uint64_t sg = nch > 2 ? fnvlist_lookup_uint64(ch[2],
	    ZPOOL_CONFIG_GUID) : 0;
	(void) printf("scanned config: vdev 2 guid %llu (%s)\n",
	    (u_longlong_t)sg, sg == g2 ? "current" : "STALE");
	e = spa_import(pool, cfg, NULL, 0);
	(void) printf("import: %d\n", e);
	nvlist_free(cfg);
	if (e == 0) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		(void) printf("root vdev state %llu\n",
		    (u_longlong_t)spa->spa_root_vdev->vdev_state);
		spa_close(spa, FTAG);
		VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	}
	kernel_fini();
	return (e != 0);
}
