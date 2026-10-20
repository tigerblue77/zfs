/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs.
 * Runs one label-writing operation on pool "op", then exports it; in_op is
 * set from the operation to the end of the export, for gdb breakpoint
 * conditions.
 *
 * Usage: ops_probe <scratch-dir> attach|detach|replace|add|expand|remove|
 * expandraidz|spare|cache|log|removemirror|checkpoint
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <sys/vdev_raidz.h>
#include <zfeature_common.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

volatile int in_op;
static char dir[1024];

static char *
vpath(int i)
{
	static char path[8][1100];

	(void) snprintf(path[i], sizeof (path[i]), "%s/vd.%d", dir, i);
	return (path[i]);
}

static nvlist_t *
mk_file(int i, uint64_t size)
{
	int fd = open(vpath(i), O_RDWR | O_CREAT | O_TRUNC, 0666);

	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, size));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, vpath(i));
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

	VERIFY0(dmu_objset_own("op", DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
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

static vdev_t *
find_path(vdev_t *vd, const char *path)
{
	if (vd->vdev_path != NULL && strcmp(vd->vdev_path, path) == 0)
		return (vd);
	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		vdev_t *f = find_path(vd->vdev_child[c], path);
		if (f != NULL)
			return (f);
	}
	return (NULL);
}

static uint64_t
guid_of(spa_t *spa, const char *path)
{
	vdev_t *vd = find_path(spa->spa_root_vdev, path);

	VERIFY3P(vd, !=, NULL);
	return (vd->vdev_guid);
}


static nvlist_t *
mk_log(int i, uint64_t size)
{
	nvlist_t *f = mk_file(i, size);

	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 1);
	return (f);
}

/* root nvlist carrying one aux (spare/l2cache) file vdev */
static nvlist_t *
mk_aux_root(const char *key, int i, uint64_t size)
{
	nvlist_t *r = fnvlist_alloc();
	nvlist_t *a = mk_file(i, size);

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, key, (const nvlist_t **)&a, 1);
	fnvlist_free(a);
	return (r);
}

static uint64_t
aux_guid(spa_aux_vdev_t *sav, const char *path)
{
	for (int i = 0; i < sav->sav_count; i++) {
		vdev_t *vd = sav->sav_vdevs[i];

		if (vd->vdev_path != NULL && strcmp(vd->vdev_path, path) == 0)
			return (vd->vdev_guid);
	}
	VERIFY(0);
	return (0);
}

static vdev_t *
find_raidz(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;

	for (uint64_t c = 0; c < rvd->vdev_children; c++)
		if (rvd->vdev_child[c]->vdev_ops == &vdev_raidz_ops)
			return (rvd->vdev_child[c]);
	VERIFY(0);
	return (NULL);
}

static void
wait_expand(spa_t *spa, vdev_t *rz)
{
	vdev_raidz_t *vdrz = rz->vdev_tsd;

	for (int i = 0; i < 1200; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (spa->spa_raidz_expand == NULL &&
		    vdrz->vn_vre.vre_state == DSS_FINISHED)
			return;
		(void) usleep(50000);
	}
	(void) printf("expandraidz: timeout\n");
}

static void
dump_tree(vdev_t *vd, int ind)
{
	(void) printf("%*s%s %s st=%d\n", ind, "", vd->vdev_ops->vdev_op_type,
	    vd->vdev_path ? vd->vdev_path : "", (int)vd->vdev_state);
	for (uint64_t c = 0; c < vd->vdev_children; c++)
		dump_tree(vd->vdev_child[c], ind + 2);
}

static void
wait_scan(spa_t *spa)
{
	for (int i = 0; i < 600; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (!dsl_scan_resilvering(spa_get_dsl(spa)) &&
		    !vdev_resilver_needed(spa->spa_root_vdev, NULL, NULL) &&
		    spa->spa_async_tasks == 0)
			break;
		(void) usleep(50000);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
}

int
main(int argc, char **argv)
{
	char cache[1200];
	spa_t *spa;
	nvlist_t *t[4], *m[2], *rt;
	const char *op;
	int nt = 0, e = 0;

	if (argc < 3)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	op = argv[2];
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	if (strcmp(op, "detach") == 0) {
		m[0] = mk_file(0, 128 << 20);
		m[1] = mk_file(1, 128 << 20);
		t[nt++] = mk_vdev(VDEV_TYPE_MIRROR, m, 2);
		t[nt++] = mk_file(2, 128 << 20);
	} else if (strcmp(op, "add") == 0) {
		t[nt++] = mk_file(0, 128 << 20);
	} else if (strcmp(op, "expand") == 0) {
		t[nt++] = mk_file(0, 64 << 20);
		t[nt++] = mk_file(1, 128 << 20);
	} else if (strcmp(op, "expandraidz") == 0) {
		nvlist_t *rz[3];
		for (int i = 0; i < 3; i++)
			rz[i] = mk_file(i, 128 << 20);
		t[nt] = mk_vdev(VDEV_TYPE_RAIDZ, rz, 3);
		fnvlist_add_uint64(t[nt], ZPOOL_CONFIG_NPARITY, 1);
		nt++;
		t[nt++] = mk_file(3, 128 << 20);
	} else if (strcmp(op, "spare") == 0) {
		m[0] = mk_file(0, 128 << 20);
		m[1] = mk_file(1, 128 << 20);
		t[nt++] = mk_vdev(VDEV_TYPE_MIRROR, m, 2);
		t[nt++] = mk_file(2, 128 << 20);
	} else if (strcmp(op, "removemirror") == 0) {
		m[0] = mk_file(0, 128 << 20);
		m[1] = mk_file(1, 128 << 20);
		t[nt++] = mk_vdev(VDEV_TYPE_MIRROR, m, 2);
		t[nt++] = mk_file(2, 128 << 20);
		t[nt++] = mk_file(3, 128 << 20);
	} else if (strcmp(op, "remove") == 0) {
		t[nt++] = mk_file(0, 128 << 20);
		t[nt++] = mk_file(1, 128 << 20);
		t[nt++] = mk_file(2, 128 << 20);
	} else {
		t[nt++] = mk_file(0, 128 << 20);
		t[nt++] = mk_file(1, 128 << 20);
	}
	rt = mk_vdev(VDEV_TYPE_ROOT, t, nt);
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
	VERIFY0(spa_create("op", rt, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(rt);
	VERIFY0(spa_open("op", &spa, FTAG));
	write_some(spa);

	in_op = 1;
	if (strcmp(op, "attach") == 0 || strcmp(op, "replace") == 0) {
		boolean_t repl = strcmp(op, "replace") == 0;
		nvlist_t *n = mk_file(repl ? 3 : 2, 128 << 20);
		nvlist_t *r = mk_vdev(VDEV_TYPE_ROOT, &n, 1);
		e = spa_vdev_attach(spa, guid_of(spa, vpath(0)), r, repl,
		    B_FALSE);
		fnvlist_free(r);
		wait_scan(spa);
	} else if (strcmp(op, "detach") == 0) {
		e = spa_vdev_detach(spa, guid_of(spa, vpath(1)), 0, B_FALSE);
	} else if (strcmp(op, "add") == 0) {
		nvlist_t *n = mk_file(1, 128 << 20);
		nvlist_t *r = mk_vdev(VDEV_TYPE_ROOT, &n, 1);
		e = spa_vdev_add(spa, r, B_FALSE);
		fnvlist_free(r);
	} else if (strcmp(op, "expand") == 0) {
		vdev_state_t ns;
		VERIFY0(truncate(vpath(0), 128 << 20));
		e = vdev_online(spa, guid_of(spa, vpath(0)),
		    ZFS_ONLINE_EXPAND, &ns);
	} else if (strcmp(op, "expandraidz") == 0) {
		vdev_t *rz = find_raidz(spa);
		nvlist_t *n = mk_file(4, 128 << 20);
		nvlist_t *r = mk_vdev(VDEV_TYPE_ROOT, &n, 1);
		e = spa_vdev_attach(spa, rz->vdev_guid, r, B_FALSE, B_FALSE);
		fnvlist_free(r);
		if (e == 0)
			wait_expand(spa, rz);
	} else if (strcmp(op, "spare") == 0) {
		/*
		 * Add a hot spare, replace mirror leaf vd.1 with it (spare
		 * in), wait for the resilver, then detach the ORIGINAL leaf so
		 * the spare becomes a permanent member.
		 */
		nvlist_t *r = mk_aux_root(ZPOOL_CONFIG_SPARES, 3, 128 << 20);
		e = spa_vdev_add(spa, r, B_FALSE);
		fnvlist_free(r);
		if (getenv("OPS_DBG"))
			(void) printf("spare add: %d\n", e);
		if (e == 0) {
			uint64_t og = guid_of(spa, vpath(1));
			nvlist_t *n = mk_file(3, 128 << 20);
			r = mk_vdev(VDEV_TYPE_ROOT, &n, 1);
			e = spa_vdev_attach(spa, og, r, B_TRUE, B_FALSE);
			fnvlist_free(r);
			if (getenv("OPS_DBG"))
				(void) printf("spare attach: %d\n", e);
			if (e == 0) {
				wait_scan(spa);
				if (getenv("OPS_DBG"))
					dump_tree(spa->spa_root_vdev, 0);
				/* resilver_done normally detaches og by itself */
				if (spa_lookup_by_guid(spa, og, B_FALSE) != NULL)
					e = spa_vdev_detach(spa, og, 0, B_FALSE);
			}
		}
	} else if (strcmp(op, "cache") == 0) {
		nvlist_t *r = mk_aux_root(ZPOOL_CONFIG_L2CACHE, 2, 128 << 20);
		e = spa_vdev_add(spa, r, B_FALSE);
		fnvlist_free(r);
		if (e == 0) {
			txg_wait_synced(spa_get_dsl(spa), 0);
			e = spa_vdev_remove(spa, aux_guid(&spa->spa_l2cache,
			    vpath(2)), B_FALSE);
		}
	} else if (strcmp(op, "log") == 0) {
		nvlist_t *n = mk_log(2, 128 << 20);
		nvlist_t *r = mk_vdev(VDEV_TYPE_ROOT, &n, 1);
		e = spa_vdev_add(spa, r, B_FALSE);
		fnvlist_free(r);
		if (e == 0) {
			txg_wait_synced(spa_get_dsl(spa), 0);
			e = spa_vdev_remove(spa, guid_of(spa, vpath(2)),
			    B_FALSE);
			for (int i = 0; e == 0 && i < 1200 &&
			    spa->spa_vdev_removal != NULL; i++)
				(void) usleep(50000);
		}
	} else if (strcmp(op, "removemirror") == 0) {
		e = spa_vdev_remove(spa,
		    spa->spa_root_vdev->vdev_child[0]->vdev_guid, B_FALSE);
		for (int i = 0; e == 0 && i < 1200 &&
		    spa->spa_vdev_removal != NULL; i++)
			(void) usleep(50000);
	} else if (strcmp(op, "checkpoint") == 0) {
		e = spa_checkpoint("op");
		if (e == 0) {
			write_some(spa);
			e = spa_checkpoint_discard("op");
		}
		for (int i = 0; e == 0 && i < 1200; i++) {
			txg_wait_synced(spa_get_dsl(spa), 0);
			if (!spa_feature_is_active(spa,
			    SPA_FEATURE_POOL_CHECKPOINT))
				break;
			(void) usleep(50000);
		}
	} else if (strcmp(op, "remove") == 0) {
		e = spa_vdev_remove(spa, guid_of(spa, vpath(1)), B_FALSE);
		for (int i = 0; e == 0 && i < 1200 &&
		    spa->spa_vdev_removal != NULL; i++)
			(void) usleep(50000);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("%s: %d\n", op, e);
	spa_close(spa, FTAG);
	VERIFY0(spa_export("op", NULL, B_FALSE, B_FALSE));
	in_op = 0;
	kernel_fini();
	return (e != 0);
}
