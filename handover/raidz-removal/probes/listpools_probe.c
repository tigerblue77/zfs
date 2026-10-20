/*
 * Scratch probe (not for upstream): list the pools a scan of <dir> finds,
 * then import the one with pool guid <guid> (0: only list).
 *
 * Usage: guidimp_probe <device-dir> <guid>
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_debug.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	char cache[1200];

	if (argc < 3)
		return (2);
	(void) snprintf(cache, sizeof (cache), "%s/guidimp.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	uint64_t want = strtoull(argv[2], NULL, 0);
	importargs_t args = { 0 };
	char *paths[1] = { argv[1] };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	nvlist_t *pools = zpool_search_import(&lpch, &args);
	nvlist_t *cfg = NULL;
	for (nvpair_t *e = nvlist_next_nvpair(pools, NULL); e != NULL;
	    e = nvlist_next_nvpair(pools, e)) {
		nvlist_t *c = fnvpair_value_nvlist(e);
		uint64_t g = fnvlist_lookup_uint64(c, ZPOOL_CONFIG_POOL_GUID);
		nvlist_t *nvroot = fnvlist_lookup_nvlist(c,
		    ZPOOL_CONFIG_VDEV_TREE);
		nvlist_t **ch;
		uint_t nch;
		VERIFY0(nvlist_lookup_nvlist_array(nvroot,
		    ZPOOL_CONFIG_CHILDREN, &ch, &nch));
		(void) printf("found pool guid %llu, %u top-level vdevs in "
		    "config\n", (u_longlong_t)g, nch);
		nvlist_print(stdout, c);
		if (g == want)
			cfg = fnvlist_dup(c);
	}
	nvlist_free(pools);
	if (cfg == NULL)
		return (want != 0);
	int err = spa_import("rg", cfg, NULL, 0);
	(void) printf("import of %llu: %d\n", (u_longlong_t)want, err);
	if (err != 0) {
		zfs_dbgmsg_print(STDOUT_FILENO, "guidimp");
	} else {
		spa_t *spa;
		VERIFY0(spa_open("rg", &spa, FTAG));
		(void) printf("imported: guid %llu, root state %llu\n",
		    (u_longlong_t)spa_guid(spa),
		    (u_longlong_t)spa->spa_root_vdev->vdev_state);
		spa_close(spa, FTAG);
		VERIFY0(spa_export("rg", NULL, B_FALSE, B_FALSE));
	}
	nvlist_free(cfg);
	kernel_fini();
	return (err != 0);
}
