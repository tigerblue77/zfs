/*
 * Scratch probe (not for upstream): scan a directory for a pool, print the
 * assembled top-level vdevs, import it, and dump zfs_dbgmsg on failure.
 * Works on a copy: the caller passes a scratch copy of the devices.
 *
 * Usage: scanimp_probe <device-dir> <pool>
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
#include <unistd.h>

int
main(int argc, char **argv)
{
	if (argc < 3)
		return (2);
	char cache[1200];
	boolean_t usecache = argc > 3 && strcmp(argv[3], "cache") == 0;
	(void) snprintf(cache, sizeof (cache), "%s/%s", argv[1],
	    usecache ? "zpool.cache" : "scanimp.cache");
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (usecache) {
		spa_t *spa;
		int oe = spa_open(argv[2], &spa, FTAG);
		(void) printf("cache open: %d\n", oe);
		if (oe != 0) {
			zfs_dbgmsg_print(STDOUT_FILENO, "scanimp");
			return (1);
		}
		(void) printf("opened: guid %llu, root state %llu\n",
		    (u_longlong_t)spa_guid(spa),
		    (u_longlong_t)spa->spa_root_vdev->vdev_state);
		spa_close(spa, FTAG);
		VERIFY0(spa_export(argv[2], NULL, B_FALSE, B_FALSE));
		kernel_fini();
		return (0);
	}
	importargs_t args = { 0 };
	char *paths[1] = { argv[1] };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	nvlist_t *cfg = NULL;
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	int e = zpool_find_config(&lpch, argv[2], &cfg, &args);
	(void) printf("scan: %d\n", e);
	if (e != 0)
		return (1);
	(void) printf("scanned pool guid %llu\n", (u_longlong_t)
	    fnvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_GUID));
	nvlist_t *nvroot = fnvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE);
	nvlist_t **ch;
	uint_t nch;
	VERIFY0(nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    &ch, &nch));
	for (uint_t c = 0; c < nch; c++) {
		uint64_t g = 0;
		const char *t = "?";
		(void) nvlist_lookup_uint64(ch[c], ZPOOL_CONFIG_GUID, &g);
		(void) nvlist_lookup_string(ch[c], ZPOOL_CONFIG_TYPE, &t);
		(void) printf("  top %u: %s guid %llu\n", c, t,
		    (u_longlong_t)g);
	}
	if (argc > 3 && argv[3][0] == 'F') {
		nvlist_t *pol = fnvlist_alloc();
		fnvlist_add_uint32(pol, ZPOOL_LOAD_REWIND_POLICY,
		    strcmp(argv[3], "FX") == 0 ?
		    ZPOOL_DO_REWIND | ZPOOL_EXTREME_REWIND : ZPOOL_DO_REWIND);
		fnvlist_add_nvlist(cfg, ZPOOL_LOAD_POLICY, pol);
		fnvlist_free(pol);
		(void) printf("rewind policy %s\n", argv[3]);
	}
	if (argc > 4)
		fnvlist_add_uint64(cfg, ZPOOL_CONFIG_POOL_GUID,
		    strtoull(argv[4], NULL, 0));
	e = spa_import(argv[2], cfg, NULL, 0);
	(void) printf("import: %d\n", e);
	if (e != 0)
		zfs_dbgmsg_print(STDOUT_FILENO, "scanimp");
	else
	{
		spa_t *spa;
		VERIFY0(spa_open(argv[2], &spa, FTAG));
		(void) printf("imported: guid %llu, root state %llu\n",
		    (u_longlong_t)spa_guid(spa),
		    (u_longlong_t)spa->spa_root_vdev->vdev_state);
		spa_close(spa, FTAG);
		VERIFY0(spa_export(argv[2], NULL, B_FALSE, B_FALSE));
	}
	nvlist_free(cfg);
	kernel_fini();
	return (e != 0);
}
