/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs.
 * A pool with one top-level vdev takes a checkpoint, then <k> more disks are
 * added, and the pool is exported and rewound to the checkpoint.  Repeated
 * <trials> times on fresh pools; prints how many rewinds failed, and for the
 * failed ones whether the pool can still be imported at all.
 *
 * Usage: rewind_probe <scratch-dir> <k> <trials>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/spa_checkpoint.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static char dir[1024];
static char pool[] = "rwp";
static int nfile;

static nvlist_t *
mk_file(void)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile++);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 128ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static nvlist_t *
mk_root(nvlist_t *c)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)&c, 1);
	fnvlist_free(c);
	return (r);
}

int
main(int argc, char **argv)
{
	if (argc < 4)
		return (2);
	int k = atoi(argv[2]), trials = atoi(argv[3]);
	int bad = 0, lost = 0;
	char cache[1200];

	for (int t = 0; t < trials; t++) {
		(void) snprintf(dir, sizeof (dir), "%s/t%d", argv[1], t);
		(void) mkdir(dir, 0755);
		nfile = 0;
		(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
		spa_config_path = cache;
		kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

		nvlist_t *props = fnvlist_alloc();
		fnvlist_add_uint64(props, "feature@zpool_checkpoint", 0);
		VERIFY0(spa_create(pool, mk_root(mk_file()), props, NULL,
		    NULL, NULL));
		fnvlist_free(props);
		VERIFY0(spa_checkpoint(pool));
		spa_t *spa;
		for (int i = 0; i < k; i++) {
			nvlist_t *rt = mk_root(mk_file());
			VERIFY0(spa_open(pool, &spa, FTAG));
			VERIFY0(spa_vdev_add(spa, rt, B_FALSE));
			txg_wait_synced(spa_get_dsl(spa), 0);
			spa_close(spa, FTAG);
			fnvlist_free(rt);
		}
		nvlist_t *cfg = NULL;
		VERIFY0(spa_export(pool, &cfg, B_FALSE, B_FALSE));
		int e = spa_import(pool, cfg, NULL, ZFS_IMPORT_CHECKPOINT);
		if (e == 0) {
			VERIFY0(spa_open(pool, &spa, FTAG));
			uint64_t n = spa->spa_root_vdev->vdev_children;
			spa_close(spa, FTAG);
			if (n != 1) {
				(void) printf("trial %d: rewound, %llu vdevs\n",
				    t, (u_longlong_t)n);
				bad++;
			}
			VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
		} else {
			bad++;
			int e2 = spa_import(pool, cfg, NULL, 0);
			(void) printf("trial %d: rewind %d, plain import %d\n",
			    t, e, e2);
			if (e2 == 0) {
				VERIFY0(spa_export(pool, NULL, B_FALSE,
				    B_FALSE));
			} else {
				lost++;
				/* As "zpool import -F", then "-FX". */
				uint32_t pol[2] = { ZPOOL_DO_REWIND,
				    ZPOOL_DO_REWIND | ZPOOL_EXTREME_REWIND };
				for (int i = 0; i < 2; i++) {
					nvlist_t *c2 = fnvlist_dup(cfg);
					nvlist_t *lp = fnvlist_alloc();
					fnvlist_add_uint32(lp,
					    ZPOOL_LOAD_REWIND_POLICY, pol[i]);
					fnvlist_add_nvlist(c2,
					    ZPOOL_LOAD_POLICY, lp);
					int e3 = spa_import(pool, c2, NULL, 0);
					(void) printf("  %s: %d\n", i == 0 ?
					    "import -F" : "import -FX", e3);
					fnvlist_free(lp);
					fnvlist_free(c2);
					if (e3 == 0) {
						VERIFY0(spa_export(pool, NULL,
						    B_FALSE, B_FALSE));
						break;
					}
				}
			}
		}
		fnvlist_free(cfg);
		kernel_fini();
		char cmd[1200];
		(void) snprintf(cmd, sizeof (cmd), "rm -rf %s", dir);
		if (system(cmd) != 0)
			return (1);
	}
	(void) printf("SUMMARY k=%d trials %d, rewind failures %d, "
	    "pool no longer importable %d\n", k, trials, bad, lost);
	return (bad != 0);
}
