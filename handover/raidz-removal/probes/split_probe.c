/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs.
 * Creates pool "sp" with two two-way mirrors (vd.0 vd.1, vd.2 vd.3), writes
 * a few objects, then splits vd.1 and vd.3 off into pool "sp2".  in_split
 * is set during the split, for gdb breakpoint conditions.
 *
 * Usage: split_probe <scratch-dir>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

volatile int in_split;
static char dir[1024];

static nvlist_t *
mk_file(int i)
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
	return (f);
}

static nvlist_t *
mk_vdev(const char *type, nvlist_t **c, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, type);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	return (r);
}

static void
write_some(spa_t *spa)
{
	objset_t *os;

	VERIFY0(dmu_objset_own("sp", DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
	for (int i = 0; i < 8; i++) {
		dmu_tx_t *tx = dmu_tx_create(os);
		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, 1 << 20);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		uint64_t obj = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 0,
		    DMU_OT_NONE, 0, tx);
		char *buf = kmem_alloc(1 << 20, KM_SLEEP);
		memset(buf, 'a' + i, 1 << 20);
		dmu_write(os, obj, 0, 1 << 20, buf, tx, DMU_READ_PREFETCH);
		kmem_free(buf, 1 << 20);
		dmu_tx_commit(tx);
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
}

int
main(int argc, char **argv)
{
	char cache[1200];
	spa_t *spa;

	if (argc < 2)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *m0[2] = { mk_file(0), mk_file(1) };
	nvlist_t *m1[2] = { mk_file(2), mk_file(3) };
	nvlist_t *t[2] = { mk_vdev(VDEV_TYPE_MIRROR, m0, 2),
	    mk_vdev(VDEV_TYPE_MIRROR, m1, 2) };
	nvlist_t *rt = mk_vdev(VDEV_TYPE_ROOT, t, 2);
	VERIFY0(spa_create("sp", rt, NULL, NULL, NULL, NULL));
	fnvlist_free(rt);
	VERIFY0(spa_open("sp", &spa, FTAG));
	write_some(spa);

	/* the new pool's vdev tree: the second leaf of each mirror */
	nvlist_t *l[2];
	for (int c = 0; c < 2; c++) {
		vdev_t *leaf = spa->spa_root_vdev->vdev_child[c]->vdev_child[1];
		l[c] = fnvlist_alloc();
		fnvlist_add_string(l[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(l[c], ZPOOL_CONFIG_PATH, leaf->vdev_path);
		fnvlist_add_uint64(l[c], ZPOOL_CONFIG_GUID, leaf->vdev_guid);
	}
	nvlist_t *nr = mk_vdev(VDEV_TYPE_ROOT, l, 2);
	nvlist_t *cfg = fnvlist_alloc();
	fnvlist_add_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE, nr);
	fnvlist_free(nr);
	in_split = 1;
	int e = spa_vdev_split_mirror(spa, "sp2", cfg, NULL, B_TRUE);
	in_split = 0;
	(void) printf("split: %d\n", e);
	fnvlist_free(cfg);
	spa_close(spa, FTAG);
	VERIFY0(spa_export("sp", NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (e != 0);
}
