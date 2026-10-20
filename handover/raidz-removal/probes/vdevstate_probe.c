/*
 * Scratch probe: scan-import <pool> from <dir> (optionally by guid), print the
 * vdev tree states, online every offline leaf as the fuzzer's verifier does,
 * sync, and print the states again.  Usage: vdevstate_probe <dir> <pool> [guid]
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/txg.h>
#include <sys/zfs_debug.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
dump(vdev_t *vd, int depth)
{
	(void) printf("%*s%s guid %llu state %d aux %d off %d flt %d deg %d "
	    "rem %d noalloc %d spare %d path %s rerr %llu werr %llu cerr %llu\n",
	    depth * 2, "", vd->vdev_ops->vdev_op_type,
	    (u_longlong_t)vd->vdev_guid, vd->vdev_state, vd->vdev_stat.vs_aux,
	    vd->vdev_offline, vd->vdev_faulted, vd->vdev_degraded,
	    vd->vdev_removed, vd->vdev_noalloc, vd->vdev_isspare,
	    vd->vdev_path ? vd->vdev_path : "-",
	    (u_longlong_t)vd->vdev_stat.vs_read_errors,
	    (u_longlong_t)vd->vdev_stat.vs_write_errors,
	    (u_longlong_t)vd->vdev_stat.vs_checksum_errors);
	for (uint64_t c = 0; c < vd->vdev_children; c++)
		dump(vd->vdev_child[c], depth + 1);
}

static void
leaves(vdev_t *vd, uint64_t *l, int *n)
{
	if (vd->vdev_ops->vdev_op_leaf) {
		l[(*n)++] = vd->vdev_guid;
		return;
	}
	for (uint64_t c = 0; c < vd->vdev_children; c++)
		leaves(vd->vdev_child[c], l, n);
}

int
main(int argc, char **argv)
{
	char cache[1200];
	uint64_t want = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;

	(void) snprintf(cache, sizeof (cache), "%s/vs.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	importargs_t args = { 0 };
	char *paths[1] = { argv[1] };
	libpc_handle_t lpch = { .lpc_lib_handle = NULL,
	    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_TRUE;
	nvlist_t *pools = zpool_search_import(&lpch, &args), *cfg = NULL;
	for (nvpair_t *e = nvlist_next_nvpair(pools, NULL); e != NULL;
	    e = nvlist_next_nvpair(pools, e)) {
		nvlist_t *c = fnvpair_value_nvlist(e);
		uint64_t g = fnvlist_lookup_uint64(c, ZPOOL_CONFIG_POOL_GUID);
		if (want == 0 || g == want)
			cfg = fnvlist_dup(c);
	}
	int err = spa_import(argv[2], cfg, NULL, 0);
	(void) printf("import: %d\n", err);
	if (err != 0)
		return (1);
	spa_t *spa;
	VERIFY0(spa_open(argv[2], &spa, FTAG));
	(void) printf("== after import\n");
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	dump(spa->spa_root_vdev, 0);
	uint64_t lv[64];
	int nl = 0;
	leaves(spa->spa_root_vdev, lv, &nl);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	for (int i = 0; i < nl; i++) {
		vdev_state_t ns;
		vdev_t *lvd;

		spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
		lvd = spa_lookup_by_guid(spa, lv[i], B_TRUE);
		boolean_t off = lvd != NULL && lvd->vdev_offline;
		spa_config_exit(spa, SCL_VDEV, FTAG);
		if (off) {
			int oe = vdev_online(spa, lv[i], 0, &ns);
			(void) printf("online %llu: %d newstate %d\n",
			    (u_longlong_t)lv[i], oe, ns);
		}
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("== after online+sync\n");
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	dump(spa->spa_root_vdev, 0);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	zfs_dbgmsg_print(STDOUT_FILENO, "vs");
	spa_close(spa, FTAG);
	VERIFY0(spa_export(argv[2], NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (0);
}
