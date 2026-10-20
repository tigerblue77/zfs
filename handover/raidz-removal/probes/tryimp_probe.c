/*
 * Scratch probe (not for upstream): import as "zpool import -d <dir>" does
 * with the kernel module: the scan runs with can_be_active = B_FALSE, so
 * get_configs() refreshes each config with a tryimport (spa_tryimport()),
 * and the pool is imported with the config the tryimport returned.
 *
 * Usage: tryimp_probe <device-dir> <guid>   (guid 0: only list)
 * Prints the leaf paths of the refreshed config, then as guidimp2_probe.
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_debug.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_pool.h>
#include <sys/txg.h>
#include <unistd.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nvlist_t *
refresh(void *unused, nvlist_t *tryconfig)
{
	(void) unused;
	return (spa_tryimport(tryconfig));
}

static int
active(void *unused, const char *name, uint64_t guid, boolean_t *isactive)
{
	(void) unused, (void) name, (void) guid;
	*isactive = B_FALSE;
	return (0);
}

static pool_config_ops_t ops = {
	.pco_refresh_config = refresh,
	.pco_pool_active = active,
};

static void
print_paths(nvlist_t *nv)
{
	nvlist_t **child;
	uint_t c, n;
	const char *path;

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN, &child,
	    &n) == 0) {
		for (c = 0; c < n; c++)
			print_paths(child[c]);
		return;
	}
	if (nvlist_lookup_string(nv, ZPOOL_CONFIG_PATH, &path) == 0)
		(void) printf("tryconfig: %llu %s\n", (u_longlong_t)
		    fnvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID), path);
}

static void
print_tree(vdev_t *vd, int indent)
{
	(void) printf("tree:%*s%s %llu state %llu aux %llu %s\n", indent, "",
	    vd->vdev_ops->vdev_op_type, (u_longlong_t)vd->vdev_guid,
	    (u_longlong_t)vd->vdev_state, (u_longlong_t)vd->vdev_stat.vs_aux,
	    vd->vdev_path ? vd->vdev_path : "");
	for (uint64_t c = 0; c < vd->vdev_children; c++)
		print_tree(vd->vdev_child[c], indent + 2);
}

int
main(int argc, char **argv)
{
	char cache[1200];

	if (argc < 3)
		return (2);
	(void) snprintf(cache, sizeof (cache), "%s/tryimp.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	uint64_t want = strtoull(argv[2], NULL, 0);
	importargs_t args = { 0 };
	char *paths[1] = { argv[1] };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &ops, .lpc_printerr = B_TRUE };
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_FALSE;
	nvlist_t *pools = zpool_search_import(&lpch, &args);
	nvlist_t *cfg = NULL;
	for (nvpair_t *e = nvlist_next_nvpair(pools, NULL); e != NULL;
	    e = nvlist_next_nvpair(pools, e)) {
		nvlist_t *c = fnvpair_value_nvlist(e);
		uint64_t g = fnvlist_lookup_uint64(c, ZPOOL_CONFIG_POOL_GUID);
		(void) printf("found pool guid %llu, leaf_paths %d\n",
		    (u_longlong_t)g, nvlist_exists(c, "leaf_paths"));
		if (g == want) {
			print_paths(fnvlist_lookup_nvlist(c,
			    ZPOOL_CONFIG_VDEV_TREE));
			cfg = fnvlist_dup(c);
		}
	}
	nvlist_free(pools);
	if (cfg == NULL)
		return (want != 0);
	int err = spa_import("rg", cfg, NULL, 0);
	(void) printf("import of %llu: %d\n", (u_longlong_t)want, err);
	if (err != 0) {
		zfs_dbgmsg_print(STDOUT_FILENO, "tryimp");
	} else {
		spa_t *spa;
		VERIFY0(spa_open("rg", &spa, FTAG));
		(void) printf("imported: guid %llu, root state %llu\n",
		    (u_longlong_t)spa_guid(spa),
		    (u_longlong_t)spa->spa_root_vdev->vdev_state);
		print_tree(spa->spa_root_vdev, 1);
		if (getenv("GUIDIMP_SCRUB") != NULL) {
			VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
			do {
				txg_wait_synced(spa_get_dsl(spa), 0);
				(void) usleep(100000);
			} while (dsl_scan_scrubbing(spa_get_dsl(spa)));
			(void) printf("scrub: errlog %llu cksum %llu read %llu\n",
			    (u_longlong_t)spa_approx_errlog_size(spa),
			    (u_longlong_t)spa->spa_root_vdev->vdev_stat.
			    vs_checksum_errors,
			    (u_longlong_t)spa->spa_root_vdev->vdev_stat.
			    vs_read_errors);
		}
		spa_close(spa, FTAG);
		VERIFY0(spa_export("rg", NULL, B_FALSE, B_FALSE));
	}
	nvlist_free(cfg);
	kernel_fini();
	return (err != 0);
}
