// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * An AnyRAID tile covers vd_tile_size bytes on each of vd_width children,
 * so vdev_anyraid_io_start() numbers the offsets of the vdev in tiles of
 * vd_tile_size * vd_width bytes.  This program checks, on an anyraidz1
 * vdev (2 data + 1 parity, width 3) built in userspace on file vdevs, that
 * two other users of tile ids agree with it:
 *
 * - vdev_anyraid_mapped() must report a metaslab as mapped exactly when
 *   the tile holding it is in the tile map (and has been on disk long
 *   enough), and a ZIL allocation must not be satisfied from a metaslab
 *   whose tile is not mapped.
 *
 * - vdev_anyraid_check_contract() must refuse (EDOM) to remove a leaf when
 *   the children that remain cannot back the highest mapped tile, and
 *   allow it when they can.
 *
 * Each case builds a pool of 4 equal children of C tiles each and puts a
 * block in tile H, with every metaslab outside tile H disabled, so the
 * tile map has a hole below H.  Three children of C tiles back C logical
 * tiles (ids 0 to C - 1), so contracting one leaf must fail for H = C and
 * succeed for H = C - 1.
 *
 * Usage: anyraid_tile_id [-v] [-w] <directory>
 * The vdev files (4 x 1 GiB, sparse) are created in <directory>.  -v prints
 * every wrong metaslab.  -w: when a contraction that had to fail is
 * accepted, let it finish and then write into each unmapped tile below
 * the end of the vdev, to show what the accepted plan leads to.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_anyraid.h>
#include <sys/vdev_anyraid_impl.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzpool.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

extern uint64_t zfs_vdev_anyraid_min_tile_size;

#define	NDISKS		4
#define	DISK_SIZE	(1ULL << 30)
#define	TILE_SIZE	(64ULL << 20)
#define	DATA_SIZE	(1ULL << 20)
#define	WAIT_SECONDS	300

static const char *dir;
static int verbose;
static int failures;

static void
fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) printf("FAIL: ");
	(void) vprintf(fmt, ap);
	(void) printf("\n");
	va_end(ap);
	failures++;
}

static nvlist_t *
make_root(const char *pool)
{
	nvlist_t *child[NDISKS];
	char path[MAXPATHLEN];

	for (int c = 0; c < NDISKS; c++) {
		(void) snprintf(path, sizeof (path), "%s/%s.%d", dir, pool, c);
		int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, >=, 0);
		VERIFY0(ftruncate(fd, DISK_SIZE));
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, 12);
	}

	nvlist_t *ar = fnvlist_alloc();
	fnvlist_add_string(ar, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ANYRAIDZ);
	fnvlist_add_uint64(ar, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_uint8(ar, ZPOOL_CONFIG_ANYRAID_NDATA, 2);
	fnvlist_add_uint8(ar, ZPOOL_CONFIG_ANYRAID_PARITY_TYPE, VAP_RAIDZ);
	fnvlist_add_uint64(ar, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(ar, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)child, NDISKS);

	nvlist_t *root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)&ar, 1);

	for (int c = 0; c < NDISKS; c++)
		fnvlist_free(child[c]);
	fnvlist_free(ar);
	return (root);
}

static void
create_pool(const char *pool)
{
	nvlist_t *root = make_root(pool);
	nvlist_t *props = fnvlist_alloc();
	char buf[128];

	for (int i = 0; i < SPA_FEATURES; i++) {
		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		(void) snprintf(buf, sizeof (buf), "feature@%s",
		    spa_feature_table[i].fi_uname);
		fnvlist_add_uint64(props, buf, 0);
	}
	VERIFY0(spa_create(pool, root, props, NULL, NULL, NULL));
	fnvlist_free(root);
	fnvlist_free(props);
}

static vdev_t *
anyraid_vdev(spa_t *spa)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	VERIFY(vdev_is_anyraid(tvd));
	return (tvd);
}

static uint64_t
logical_tile_size(vdev_t *tvd)
{
	vdev_anyraid_t *va = tvd->vdev_tsd;
	return (va->vd_tile_size * va->vd_width);
}

static void
print_tile_map(vdev_t *tvd, const char *when)
{
	vdev_anyraid_t *va = tvd->vdev_tsd;

	(void) printf("  tile map %s:", when);
	rw_enter(&va->vd_lock, RW_READER);
	for (anyraid_tile_t *at = avl_first(&va->vd_tile_map); at != NULL;
	    at = AVL_NEXT(&va->vd_tile_map, at)) {
		(void) printf(" %u[", at->at_tile_id);
		for (anyraid_tile_node_t *atn = list_head(&at->at_list);
		    atn != NULL; atn = list_next(&at->at_list, atn)) {
			(void) printf("%s%u:%u",
			    atn == list_head(&at->at_list) ? "" : " ",
			    atn->atn_disk, atn->atn_tile_idx);
		}
		(void) printf("]");
	}
	rw_exit(&va->vd_lock);
	(void) printf("\n");
}

static boolean_t
tile_is_mapped(vdev_t *tvd, uint32_t tile)
{
	vdev_anyraid_t *va = tvd->vdev_tsd;
	anyraid_tile_t search;

	search.at_tile_id = tile;
	rw_enter(&va->vd_lock, RW_READER);
	boolean_t found = avl_find(&va->vd_tile_map, &search, NULL) != NULL;
	rw_exit(&va->vd_lock);
	return (found);
}

/*
 * Write DATA_SIZE bytes to a new object with every metaslab outside tile
 * "tile" disabled, so that the data (and the metadata synced with it)
 * lands in that tile.
 */
static uint64_t
write_into_tile(spa_t *spa, const char *pool, uint32_t tile)
{
	vdev_t *tvd = anyraid_vdev(spa);
	uint64_t ltsize = logical_tile_size(tvd);
	objset_t *os;
	uint64_t object;
	char *buf = umem_alloc(DATA_SIZE, UMEM_NOFAIL);

	for (uint64_t i = 0; i < DATA_SIZE; i++)
		buf[i] = (char)(i * 7 + 1);

	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		if (tvd->vdev_ms[m]->ms_start / ltsize != tile)
			metaslab_disable_nowait(tvd->vdev_ms[m]);
	}

	VERIFY0(dmu_objset_own(pool, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, DATA_SIZE);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	object = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 0, DMU_OT_NONE, 0,
	    tx);
	dmu_write(os, object, 0, DATA_SIZE, buf, tx, 0);
	dmu_tx_commit(tx);
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);

	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		if (tvd->vdev_ms[m]->ms_start / ltsize != tile)
			metaslab_enable(tvd->vdev_ms[m], B_FALSE, B_FALSE);
	}
	umem_free(buf, DATA_SIZE);
	return (object);
}

static boolean_t
data_intact(const char *pool, uint64_t object)
{
	objset_t *os;
	char *buf = umem_alloc(DATA_SIZE, UMEM_NOFAIL);
	boolean_t ok = B_TRUE;

	VERIFY0(dmu_objset_own(pool, DMU_OST_ANY, B_TRUE, B_TRUE, FTAG, &os));
	if (dmu_read(os, object, 0, DATA_SIZE, buf, 0) != 0)
		ok = B_FALSE;
	for (uint64_t i = 0; ok && i < DATA_SIZE; i++) {
		if (buf[i] != (char)(i * 7 + 1))
			ok = B_FALSE;
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	umem_free(buf, DATA_SIZE);
	return (ok);
}

static void
export_import(spa_t **spap, const char *pool)
{
	nvlist_t *config;

	spa_close(*spap, FTAG);
	VERIFY0(spa_get_stats(pool, &config, NULL, 0));
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	VERIFY0(spa_import((char *)pool, config, NULL, 0));
	nvlist_free(config);
	VERIFY0(spa_open(pool, spap, FTAG));
}

/*
 * After an import, every tile in the map was read from disk.  Once enough
 * txgs have synced, a metaslab must be reported mapped exactly when the
 * tile vdev_anyraid_io_start() would use for its offset is in the map.
 */
static void
check_mapped(spa_t *spa, uint32_t hole)
{
	vdev_t *tvd = anyraid_vdev(spa);
	vdev_anyraid_t *va = tvd->vdev_tsd;
	uint64_t ltsize = logical_tile_size(tvd);

	for (int i = 0; i < VDEV_ANYRAID_MAP_COPIES; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	uint64_t txg = spa_last_synced_txg(spa) + 1;

	int wrong = 0;
	int64_t accepted_unmapped = -1;
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		uint64_t start = tvd->vdev_ms[m]->ms_start;
		uint32_t tile = start / ltsize;
		anyraid_tile_t search;
		anyraid_tile_t *at;

		search.at_tile_id = tile;
		rw_enter(&va->vd_lock, RW_READER);
		at = avl_find(&va->vd_tile_map, &search, NULL);
		boolean_t expect = at != NULL &&
		    at->at_synced + VDEV_ANYRAID_MAP_COPIES <= txg;
		rw_exit(&va->vd_lock);

		boolean_t got = vdev_anyraid_mapped(tvd, start, txg);
		if (got != expect) {
			if (verbose || wrong < 8) {
				(void) printf("  metaslab %llu (offset %llu "
				    "MiB, tile %u): vdev_anyraid_mapped() = %d,"
				    " tile %s\n", (u_longlong_t)m,
				    (u_longlong_t)(start >> 20), tile, got,
				    at != NULL ? "mapped" : "not mapped");
			}
			wrong++;
			if (got && at == NULL && accepted_unmapped == -1)
				accepted_unmapped = m;
		}
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	if (wrong != 0) {
		fail("vdev_anyraid_mapped() wrong for %d of %llu metaslabs",
		    wrong, (u_longlong_t)tvd->vdev_ms_count);
	} else {
		(void) printf("  vdev_anyraid_mapped() right for all %llu "
		    "metaslabs\n", (u_longlong_t)tvd->vdev_ms_count);
	}

	/*
	 * The metaslab that starts at hole * vd_tile_size lies in tile
	 * hole / vd_width, which is not mapped.  Leave it the only
	 * allocatable metaslab and ask for a ZIL block: it must not be
	 * placed there.
	 */
	uint64_t m = (hole * va->vd_tile_size) >> tvd->vdev_ms_shift;
	if (accepted_unmapped != -1)
		m = accepted_unmapped;
	uint32_t tile = tvd->vdev_ms[m]->ms_start / ltsize;
	if (tile_is_mapped(tvd, tile)) {
		(void) printf("  tile %u of metaslab %llu is mapped, ZIL "
		    "check skipped\n", tile, (u_longlong_t)m);
		return;
	}
	for (uint64_t i = 0; i < tvd->vdev_ms_count; i++) {
		if (i != m)
			metaslab_disable_nowait(tvd->vdev_ms[i]);
	}
	objset_t *os;
	VERIFY0(dmu_objset_own(spa_name(spa), DMU_OST_ANY, B_FALSE, B_TRUE,
	    FTAG, &os));
	dmu_tx_t *tx = dmu_tx_create(os);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	blkptr_t bp = { 0 };
	boolean_t slog;
	int err = zio_alloc_zil(spa, os, dmu_tx_get_txg(tx), &bp,
	    SPA_MINBLOCKSIZE << 3, SPA_MINBLOCKSIZE << 3, &slog, B_FALSE);
	if (err == 0) {
		uint64_t off = DVA_GET_OFFSET(&bp.blk_dva[0]);
		BP_SET_BIRTH(&bp, dmu_tx_get_txg(tx), dmu_tx_get_txg(tx));
		fail("ZIL block allocated at offset %llu MiB, in tile %llu, "
		    "which is not mapped", (u_longlong_t)(off >> 20),
		    (u_longlong_t)(off / ltsize));
		metaslab_free(spa, &bp, dmu_tx_get_txg(tx), B_TRUE);
	} else {
		(void) printf("  ZIL allocation refused metaslab %llu (tile %u,"
		    " not mapped): error %d\n", (u_longlong_t)m, tile, err);
	}
	dmu_tx_commit(tx);
	dmu_objset_disown(os, B_FALSE, FTAG);
	for (uint64_t i = 0; i < tvd->vdev_ms_count; i++) {
		if (i != m)
			metaslab_enable(tvd->vdev_ms[i], B_FALSE, B_FALSE);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
}

static int
contract(spa_t *spa, uint32_t hole)
{
	vdev_t *tvd = anyraid_vdev(spa);
	vdev_anyraid_t *va = tvd->vdev_tsd;
	anyraid_tile_t search;
	anyraid_tile_t *at;
	uint64_t leaf;

	/* Remove a child that holds a column of the hole tile. */
	search.at_tile_id = hole;
	rw_enter(&va->vd_lock, RW_READER);
	at = avl_find(&va->vd_tile_map, &search, NULL);
	VERIFY3P(at, !=, NULL);
	leaf = ((anyraid_tile_node_t *)list_head(&at->at_list))->atn_disk;
	rw_exit(&va->vd_lock);

	(void) printf("  contracting leaf %llu\n", (u_longlong_t)leaf);
	return (spa_contract_vdev(spa, tvd->vdev_guid,
	    tvd->vdev_child[leaf]->vdev_guid));
}

static boolean_t
wait_contraction(spa_t *spa)
{
	for (int i = 0; i < WAIT_SECONDS * 10; i++) {
		spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
		uint64_t children = anyraid_vdev(spa)->vdev_children;
		spa_config_exit(spa, SCL_ALL, FTAG);
		if (children == NDISKS - 1)
			return (B_TRUE);
		if (spa_suspended(spa))
			return (B_FALSE);
		(void) poll(NULL, 0, 100);
	}
	return (B_FALSE);
}

static void
print_children(spa_t *spa)
{
	vdev_t *tvd = anyraid_vdev(spa);
	vdev_anyraid_t *va = tvd->vdev_tsd;
	uint64_t nfree = 0;

	(void) printf("  children (tiles used/capacity):");
	rw_enter(&va->vd_lock, RW_READER);
	for (int c = 0; c < tvd->vdev_children; c++) {
		vdev_anyraid_node_t *van = va->vd_children[c];
		uint16_t used = anyraid_freelist_alloc(&van->van_freelist);
		(void) printf(" %d:%u/%u", c, used, van->van_capacity);
		nfree += van->van_capacity - used;
	}
	rw_exit(&va->vd_lock);
	(void) printf("; %llu free tiles on the children, min_asize %llu "
	    "MiB\n", (u_longlong_t)nfree,
	    (u_longlong_t)(tvd->vdev_min_asize >> 20));
}

static void
print_vdev(spa_t *spa)
{
	vdev_t *tvd = anyraid_vdev(spa);

	(void) printf("  anyraid vdev: %llu children, state %llu, aux %llu, "
	    "asize %llu MiB (%llu tiles), ms_count %llu, pool %s\n",
	    (u_longlong_t)tvd->vdev_children,
	    (u_longlong_t)tvd->vdev_state,
	    (u_longlong_t)tvd->vdev_stat.vs_aux,
	    (u_longlong_t)(tvd->vdev_asize >> 20),
	    (u_longlong_t)(tvd->vdev_asize / logical_tile_size(tvd)),
	    (u_longlong_t)tvd->vdev_ms_count,
	    spa_suspended(spa) ? "suspended" : "not suspended");
}

/*
 * Write into each tile below the end of the vdev that is not mapped yet,
 * one at a time.
 */
static void
fill_tiles(spa_t *spa, const char *pool)
{
	vdev_t *tvd = anyraid_vdev(spa);
	uint64_t tiles = tvd->vdev_asize / logical_tile_size(tvd);

	for (uint32_t t = 0; t < tiles; t++) {
		if (tile_is_mapped(anyraid_vdev(spa), t))
			continue;
		(void) printf("  writing into tile %u\n", t);
		(void) fflush(stdout);
		(void) write_into_tile(spa, pool, t);
		print_children(spa);
	}
	(void) printf("  every tile below the end of the vdev is mapped\n");
}

/*
 * expect_ok: whether the remaining children can back tile "hole".
 */
static void
run_case(const char *pool, boolean_t expect_ok, int wait_bad)
{
	spa_t *spa;

	(void) printf("%s: ", pool);
	create_pool(pool);
	VERIFY0(spa_open(pool, &spa, FTAG));
	vdev_t *tvd = anyraid_vdev(spa);
	vdev_anyraid_t *va = tvd->vdev_tsd;
	uint64_t c = va->vd_children[0]->van_capacity;
	for (int i = 1; i < NDISKS; i++)
		VERIFY3U(va->vd_children[i]->van_capacity, ==, c);
	uint32_t hole = expect_ok ? c - 1 : c;

	(void) printf("anyraidz1:2, %d children of %llu tiles, tile size "
	    "%llu MiB, width %u, metaslab size %llu MiB, asize %llu tiles;"
	    " block in tile %u, contraction must %s\n", NDISKS,
	    (u_longlong_t)c, (u_longlong_t)(va->vd_tile_size >> 20),
	    va->vd_width, (u_longlong_t)(1ULL << (tvd->vdev_ms_shift - 20)),
	    (u_longlong_t)(tvd->vdev_asize / logical_tile_size(tvd)), hole,
	    expect_ok ? "succeed" : "fail with EDOM");
	VERIFY3U(tvd->vdev_asize / logical_tile_size(tvd), >, hole);

	uint64_t object = write_into_tile(spa, pool, hole);
	print_tile_map(anyraid_vdev(spa), "after write");
	VERIFY(tile_is_mapped(anyraid_vdev(spa), hole));

	export_import(&spa, pool);
	check_mapped(spa, hole);

	int err = contract(spa, hole);
	if (expect_ok) {
		if (err != 0) {
			fail("contraction refused with error %d", err);
		} else if (!wait_contraction(spa)) {
			fail("contraction did not finish");
			print_vdev(spa);
		} else {
			txg_wait_synced(spa_get_dsl(spa), 0);
			print_vdev(spa);
			if (anyraid_vdev(spa)->vdev_state !=
			    VDEV_STATE_HEALTHY)
				fail("vdev not healthy after contraction");
			if (!data_intact(pool, object))
				fail("data in tile %u lost", hole);
		}
	} else {
		if (err == EDOM) {
			(void) printf("  contraction refused with EDOM\n");
		} else {
			fail("contraction returned %d, expected EDOM (%d)",
			    err, EDOM);
			if (err == 0 && wait_bad) {
				(void) printf("  waiting for the contraction "
				    "to finish\n");
				(void) wait_contraction(spa);
				print_vdev(spa);
				print_tile_map(anyraid_vdev(spa), "after");
				print_children(spa);
				fill_tiles(spa, pool);
			}
			/*
			 * The pool may be unusable now; do not try to
			 * export it.
			 */
			(void) printf("%d failure(s)\n", failures);
			exit(1);
		}
	}
	spa_close(spa, FTAG);
	VERIFY0(spa_destroy(pool));
	for (int i = 0; i < NDISKS; i++) {
		char path[MAXPATHLEN];
		(void) snprintf(path, sizeof (path), "%s/%s.%d", dir, pool, i);
		(void) unlink(path);
	}
}

int
main(int argc, char **argv)
{
	int wait_bad = 0;
	int c;

	while ((c = getopt(argc, argv, "vw")) != -1) {
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		case 'w':
			wait_bad = 1;
			break;
		default:
			(void) fprintf(stderr,
			    "usage: %s [-v] [-w] <directory>\n", argv[0]);
			return (2);
		}
	}
	if (optind != argc - 1) {
		(void) fprintf(stderr, "usage: %s [-v] [-w] <directory>\n",
		    argv[0]);
		return (2);
	}
	dir = argv[optind];

	VERIFY3S(asprintf(&spa_config_path, "%s/zpool.cache", dir), !=, -1);
	zfs_vdev_anyraid_min_tile_size = TILE_SIZE;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	run_case("anyraid_tile_ok", B_TRUE, wait_bad);
	run_case("anyraid_tile_edom", B_FALSE, wait_bad);

	kernel_fini();
	(void) printf("%d failure(s)\n", failures);
	return (failures == 0 ? 0 : 1);
}
