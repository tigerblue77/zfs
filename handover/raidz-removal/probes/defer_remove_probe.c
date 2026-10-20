/*
 * Scratch probe (not for upstream): "zpool remove" of a vdev that is already
 * allocating=off, right after its last block was freed.  The free has synced,
 * so vs_alloc is 0 and the empty path is taken, but the freed range is still
 * deferred for TXG_DEFER_SIZE txgs.  vdev_metaslab_fini() then destroys a
 * non-empty ms_defer tree unless the removal waited for the deferral.
 *
 * Vdev 1 is a dedup-class vdev, so that nothing but the probe's own block is
 * allocated on it.
 *
 * Usage: defer_remove_probe <scratch-dir>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/metaslab_impl.h>
#include <sys/dsl_synctask.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *pool = "deferpool";
static blkptr_t blk;

static nvlist_t *
mk_file(const char *dir, int i)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, i);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 256ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static nvlist_t *
mk_dedup(nvlist_t *f)
{
	fnvlist_add_string(f, ZPOOL_CONFIG_ALLOCATION_BIAS,
	    VDEV_ALLOC_BIAS_DEDUP);
	return (f);
}

static nvlist_t *
all_features(void)
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
	return (props);
}

static nvlist_t *
mk_root(nvlist_t *top)
{
	nvlist_t *rt = fnvlist_alloc();

	fnvlist_add_string(rt, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(rt, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)&top, 1);
	fnvlist_free(top);
	return (rt);
}

static uint64_t
vd1_alloc(uint64_t *defer)
{
	spa_t *spa;

	VERIFY0(spa_open(pool, &spa, FTAG));
	vdev_t *vd = spa->spa_root_vdev->vdev_child[1];
	uint64_t a = vd->vdev_stat.vs_alloc;
	uint64_t d = 0;
	for (uint64_t m = 0; m < vd->vdev_ms_count; m++)
		d += vd->vdev_ms[m]->ms_deferspace;
	spa_close(spa, FTAG);
	if (defer != NULL)
		*defer = d;
	return (a);
}

/* Allocate one 128K block on vdev 1, in syncing context. */
static void
alloc_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	blkptr_t hint = { 0 };
	zio_alloc_list_t zal;

	DVA_SET_VDEV(&hint.blk_dva[0], 1);
	DVA_SET_ASIZE(&hint.blk_dva[0], SPA_MINBLOCKSIZE);
	metaslab_trace_init(&zal);
	VERIFY0(metaslab_alloc(spa, spa_dedup_class(spa), 128 << 10, &blk,
	    1, dmu_tx_get_txg(tx), &hint, 0, &zal, 0, FTAG));
	metaslab_trace_fini(&zal);
	BP_SET_BIRTH(&blk, dmu_tx_get_txg(tx), dmu_tx_get_txg(tx));
	BP_SET_LSIZE(&blk, 128 << 10);
	BP_SET_PSIZE(&blk, 128 << 10);
	VERIFY3U(DVA_GET_VDEV(&blk.blk_dva[0]), ==, 1);
}

/* Dirty the MOS, so that its blocks on vdev 1 get rewritten elsewhere. */
static void
churn_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	objset_t *mos = dmu_tx_pool(tx)->dp_meta_objset;
	uint64_t obj = dmu_object_alloc(mos, DMU_OT_UINT64_OTHER, 0,
	    DMU_OT_NONE, 0, tx);
	VERIFY0(dmu_object_free(mos, obj, tx));
}

static void
free_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	metaslab_free(dmu_tx_pool(tx)->dp_spa, &blk, dmu_tx_get_txg(tx),
	    B_FALSE);
}

int
main(int argc, char **argv)
{
	spa_t *spa;
	uint64_t a, d, guid;

	if (argc < 2)
		return (2);
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *props = all_features();
	VERIFY0(spa_create(pool, mk_root(mk_file(argv[1], 0)), props, NULL,
	    NULL, NULL));
	fnvlist_free(props);
	VERIFY0(spa_open(pool, &spa, FTAG));
	VERIFY0(spa_vdev_add(spa, mk_root(mk_dedup(mk_file(argv[1], 1))),
	    B_FALSE));
	guid = spa->spa_root_vdev->vdev_child[1]->vdev_guid;
	txg_wait_synced(spa_get_dsl(spa), 0);
	spa_close(spa, FTAG);

	a = vd1_alloc(NULL);
	(void) printf("vdev 1 after add: alloc %llu\n", (u_longlong_t)a);
	if (a != 0)
		return (3);

	VERIFY0(dsl_sync_task(pool, NULL, alloc_sync, NULL, 0,
	    ZFS_SPACE_CHECK_NONE));
	a = vd1_alloc(NULL);
	(void) printf("vdev 1 after the allocation: alloc %llu\n",
	    (u_longlong_t)a);
	if (a < DVA_GET_ASIZE(&blk.blk_dva[0]))
		return (3);

	VERIFY0(spa_open(pool, &spa, FTAG));
	VERIFY0(spa_vdev_noalloc(spa, guid));
	txg_wait_synced(spa_get_dsl(spa), 0);
	spa_close(spa, FTAG);
	for (int i = 0; i < 200; i++) {
		a = vd1_alloc(NULL);
		if (a == DVA_GET_ASIZE(&blk.blk_dva[0]))
			break;
		VERIFY0(dsl_sync_task(pool, NULL, churn_sync, NULL, 0,
		    ZFS_SPACE_CHECK_NONE));
	}
	(void) printf("vdev 1 after the churn: alloc %llu\n",
	    (u_longlong_t)a);
	if (a != DVA_GET_ASIZE(&blk.blk_dva[0]))
		return (3);

	VERIFY0(dsl_sync_task(pool, NULL, free_sync, NULL, 0,
	    ZFS_SPACE_CHECK_NONE));
	a = vd1_alloc(&d);
	(void) printf("vdev 1 after the free: alloc %llu, deferred %llu\n",
	    (u_longlong_t)a, (u_longlong_t)d);
	for (int i = 0; i < 10 && a != 0; i++) {
		VERIFY0(spa_open(pool, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), spa_last_synced_txg(spa) + 1);
		spa_close(spa, FTAG);
		a = vd1_alloc(&d);
		(void) printf("  +1 txg: alloc %llu, deferred %llu\n",
		    (u_longlong_t)a, (u_longlong_t)d);
	}
	if (a != 0 || d != 0)
		return (3);

	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = spa_vdev_remove(spa, guid, B_FALSE);
	(void) printf("zpool remove: %d\n", e);
	if (e == 0) {
		vdev_t *vd = spa->spa_root_vdev->vdev_child[1];
		(void) printf("vdev 1 is now %s\n", vd->vdev_ops->vdev_op_type);
		if (vd->vdev_ops != &vdev_hole_ops)
			e = 4;
	}
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (e);
}
