// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */
/*
 * Spike (throwaway, not part of any tree): case (d) of the kernel run of
 * the module's mover (kmover_user.c), in userspace.  "create" makes what
 * kh.sh's kpool and case_d make with the kernel: a raidz1 pool "khpool" of
 * five 256 MiB files in <dir>, ashift 12, every feature enabled (as "zpool
 * create" does), a dataset "g" with copies=2 and compression off, eight
 * objects of 1 MiB in 128K blocks written with metaslab_force_ganging=32769
 * and metaslab_force_ganging_pct=100, a snapshot "g@s", the first four
 * objects overwritten, and ganging restored.  With "nodyn",
 * feature@dynamic_gang_header stays disabled, as on the fixtures.  "verify"
 * reads every object of g and of g@s back against its pattern.
 *
 *   kh_gang <dir> create [nodyn] | verify
 *
 * Exit status: 0 on success, 1 on a failed check, 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_prop.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

extern uint64_t metaslab_force_ganging;
extern uint_t metaslab_force_ganging_pct;

#define	POOL	"khpool"
#define	DS	POOL "/g"
#define	WIDTH	5
#define	MEMBER	(256ULL << 20)
#define	NOBJ	8
#define	OSZ	(1ULL << 20)
#define	REC	(128ULL << 10)

static void
pat(uint64_t *b, size_t n, uint64_t seed)
{
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ (seed * 0x2545f4914f6cdd1dULL);

	for (size_t i = 0; i < n / 8; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		b[i] = x;
	}
}

static void
wr(objset_t *os, uint64_t obj, uint64_t seed, void *buf)
{
	for (uint64_t off = 0; off < OSZ; off += REC) {
		dmu_tx_t *tx = dmu_tx_create(os);

		pat(buf, REC, seed * 1000 + off / REC);
		dmu_tx_hold_write(tx, obj, off, REC);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, obj, off, REC, buf, tx, DMU_READ_NO_PREFETCH);
		dmu_tx_commit(tx);
	}
}

static int
ck(const char *name, uint64_t obj, uint64_t seed, void *buf, void *exp)
{
	objset_t *os;
	int bad = 0;

	VERIFY0(dmu_objset_own(name, DMU_OST_ANY, B_TRUE, B_TRUE, FTAG, &os));
	for (uint64_t off = 0; off < OSZ; off += REC) {
		pat(exp, REC, seed * 1000 + off / REC);
		if (dmu_read(os, obj, off, REC, buf,
		    DMU_READ_NO_PREFETCH) != 0 || memcmp(buf, exp, REC) != 0)
			bad++;
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	return (bad);
}

static void
create_pool(const char *dir, boolean_t dyn)
{
	nvlist_t *child[WIDTH];
	nvlist_t *raid = fnvlist_alloc();
	nvlist_t *root = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();
	char path[4096];

	for (int i = 0; i < WIDTH; i++) {
		int fd;

		(void) snprintf(path, sizeof (path), "%s/v%d", dir, i);
		fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		VERIFY3S(fd, >=, 0);
		VERIFY0(ftruncate(fd, MEMBER));
		VERIFY0(close(fd));
		child[i] = fnvlist_alloc();
		fnvlist_add_string(child[i], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[i], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[i], ZPOOL_CONFIG_ASHIFT, 12);
	}
	fnvlist_add_string(raid, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_nvlist_array(raid, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, WIDTH);
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&raid, 1);
	/* As "zpool create": every feature, unless told otherwise. */
	for (spa_feature_t f = 0; f < SPA_FEATURES; f++) {
		if (!dyn && f == SPA_FEATURE_DYNAMIC_GANG_HEADER)
			continue;
		(void) snprintf(path, sizeof (path), "feature@%s",
		    spa_feature_table[f].fi_uname);
		fnvlist_add_uint64(props, path, 0);
	}
	fnvlist_add_string(props, "cachefile", "none");
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raid);
	for (int i = 0; i < WIDTH; i++)
		fnvlist_free(child[i]);
}

static void
import_pool(const char *dir)
{
	importargs_t ia = { 0 };
	char *paths[1] = { (char *)dir };
	nvlist_t *config = NULL, *props = fnvlist_alloc();
	libpc_handle_t lpch = { .lpc_ops = &libzpool_config_ops,
	    .lpc_printerr = B_TRUE };

	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&lpch, POOL, &config, &ia));
	fnvlist_add_string(props, "cachefile", "none");
	VERIFY0(spa_import((char *)POOL, config, props, ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	fnvlist_free(props);
}

static void
setup(const char *dir, void *buf)
{
	uint64_t obj[NOBJ], g0 = metaslab_force_ganging;
	uint_t g1 = metaslab_force_ganging_pct;
	char path[4096];
	objset_t *os;
	spa_t *spa;
	FILE *f;

	VERIFY0(spa_open(POOL, &spa, FTAG));
	VERIFY0(dmu_objset_create(DS, DMU_OST_OTHER, 0, NULL, NULL, NULL));
	VERIFY0(dsl_prop_set_int(DS, "copies", ZPROP_SRC_LOCAL, 2));
	VERIFY0(dsl_prop_set_int(DS, "compression", ZPROP_SRC_LOCAL,
	    ZIO_COMPRESS_OFF));
	metaslab_force_ganging = 32769;
	metaslab_force_ganging_pct = 100;
	VERIFY0(dmu_objset_own(DS, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < NOBJ; i++) {
		dmu_tx_t *tx = dmu_tx_create(os);

		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		obj[i] = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, REC,
		    DMU_OT_NONE, 0, tx);
		dmu_tx_commit(tx);
		wr(os, obj[i], 10 + i, buf);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dmu_objset_snapshot_one(DS, "s"));
	VERIFY0(dmu_objset_own(DS, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < NOBJ / 2; i++)
		wr(os, obj[i], 20 + i, buf);
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	metaslab_force_ganging = g0;
	metaslab_force_ganging_pct = g1;
	(void) printf("GANG create: dynamic_gang_header enabled=%d active=%d\n",
	    spa_feature_is_enabled(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER),
	    spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER));
	spa_close(spa, FTAG);
	(void) snprintf(path, sizeof (path), "%s/g.txt", dir);
	f = fopen(path, "w");
	VERIFY3P(f, !=, NULL);
	for (int i = 0; i < NOBJ; i++)
		(void) fprintf(f, "%llu\n", (u_longlong_t)obj[i]);
	VERIFY0(fclose(f));
}

static int
gang_verify(const char *dir, void *buf, void *exp)
{
	uint64_t obj[NOBJ];
	char path[4096];
	int bad = 0;
	FILE *f;

	(void) snprintf(path, sizeof (path), "%s/g.txt", dir);
	f = fopen(path, "r");
	VERIFY3P(f, !=, NULL);
	for (int i = 0; i < NOBJ; i++) {
		u_longlong_t o;

		VERIFY3S(fscanf(f, "%llu", &o), ==, 1);
		obj[i] = o;
	}
	VERIFY0(fclose(f));
	for (int i = 0; i < NOBJ; i++) {
		bad += ck(DS, obj[i], i < NOBJ / 2 ? 20 + i : 10 + i, buf, exp);
		bad += ck(DS "@s", obj[i], 10 + i, buf, exp);
	}
	(void) printf("GANG verify: %d bad records %s\n", bad,
	    bad == 0 ? "PASS" : "FAIL");
	return (bad != 0);
}

int
main(int argc, char **argv)
{
	char cache[4096];
	void *buf, *exp;
	int rc = 0;

	if (argc < 3 || (strcmp(argv[2], "create") != 0 &&
	    strcmp(argv[2], "verify") != 0)) {
		(void) fprintf(stderr, "usage: %s <dir> create [nodyn] | "
		    "verify\n", argv[0]);
		return (2);
	}
	buf = malloc(REC);
	exp = malloc(REC);
	VERIFY(buf != NULL && exp != NULL);
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (strcmp(argv[2], "create") == 0) {
		create_pool(argv[1], argc < 4 || strcmp(argv[3], "nodyn") != 0);
		setup(argv[1], buf);
	} else {
		import_pool(argv[1]);
		rc = gang_verify(argv[1], buf, exp);
	}
	VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	free(buf);
	free(exp);
	return (rc);
}
