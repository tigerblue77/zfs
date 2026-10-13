// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 5, part A (throwaway, not part of any tree): is the move of
 * old-epoch raidz blocks atomic across a crash, does a fresh process find the
 * redirects, and does zdb account for moved blocks?
 *
 *   crash_probe <dir> <ashift> move <point> <P1|P2> [split]
 *   crash_probe <dir> <ashift> guard <mos|zil|stale|checkpoint>
 *
 * The parent never initializes libzpool; every phase runs in a forked child.
 * Phase 0 creates a five-wide raidz1 pool of files in <dir>, writes a plain,
 * a dedup and a gang object and a control at parity 1, adds a parity-2
 * epoch, prints the state and exports.
 *
 * move: child A imports the pool, arms the crash point <point> (TABLE_DIRTY,
 * MS_SYNCED, PRE_UB or POST_UB) for the txg T of the move, and moves the
 * plain, dedup and gang blocks and the gang's children with the kernel's
 * test-only move, keeping their old extents whole (P1) or their first sector
 * (P2).  It must exit with status 99 at that point.  Child B imports the
 * pool read-only and checks the number of redirects loaded, that every
 * object reads back bit-exact, the redirect hits, and, under the metaslab
 * lock, what is allocated: before POST_UB the state before the move (no
 * redirect, no hit, old extents whole, N free), at POST_UB the state after
 * it (every redirect, one hit per moved block, [O, O + kept) and N
 * allocated, the rest of the old extent free).  The parent then runs
 * $ZDB -bcc -d -Y -e -p <dir> on the pool and requires no leaked space, a
 * block sum that matches the space maps, no claim error and no leaked MOS
 * object, since zdb's exit status does not report all of these.  'split'
 * sets the test-only spike_redirect_split_txg, which adds the redirects in
 * a later txg than the allocation and the frees, and arms the first three
 * points in that txg.  zdb's environment may hold
 * SPIKE_ZDB_IGNORE_REDIRECTS (claims, mos or all) to show what zdb reports
 * without its knowledge of moved blocks.
 *
 * On a libzpool without the persistent table (the in-memory one of the
 * previous step), child A moves the blocks with that table and, for
 * POST_UB only, emulates the crash by exiting with 99 as soon as T has
 * synced; the other points cannot be emulated and are skipped.
 *
 * guard: after phase 0, a child checks that the move refuses a MOS block,
 * a block named by a ZIL bookmark (the bookmark only; no ZIL block is
 * made) and a pool with a checkpoint, and skips, without freeing anything,
 * a block whose old extent was freed before the move (under P2).
 *
 * Prints one PASS or FAIL line per check and exits 1 if any check failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/spa_checkpoint.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/range_tree.h>
#include <sys/space_map.h>
#include <sys/spa_log_spacemap.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"crp"
#define	WIDTH		5
#define	FILE_SIZE	(256ULL << 20)
#define	BLKSZ		(128ULL << 10)
#define	PATH_BUF	512
#define	MAXMOVES	16

extern uint64_t metaslab_force_ganging;
extern uint_t metaslab_force_ganging_pct;
extern boolean_t spa_mode_readable_spacemaps;

/* Weak: absent from the libzpool of the previous step. */
extern int vdev_raidz_redirect_move(spa_t *, uint64_t,
    raidz_redirect_move_t *, int, raidz_redirect_keep_t, boolean_t)
    __attribute__((weak));
extern uint64_t vdev_raidz_redirect_count(spa_t *, uint64_t)
    __attribute__((weak));
extern int spike_crash_point __attribute__((weak));
extern int spike_redirect_split_txg __attribute__((weak));
/* Weak: the previous step's in-memory table, absent from this one. */
extern int vdev_raidz_redirect_add(uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t) __attribute__((weak));

typedef enum policy { P1, P2 } policy_t;
static const char *const policy_names[] = { "P1", "P2" };
static const char *const point_names[] = { "NONE", "TABLE_DIRTY",
	"MS_SYNCED", "PRE_UB", "POST_UB" };

enum { OA, OD, OG, OC, NOBJ };
static const char *const obj_names[] = { "plain-A", "dedup-D", "gang-G",
	"control-C" };
static const uint64_t obj_seeds[] = { 1, 2, 3, 4 };

/* What the phases share: the parent maps it before forking any of them. */
typedef struct shared {
	uint64_t		sh_obj[NOBJ];
	uint64_t		sh_obj_e;	/* guard stale: freed object */
	int			sh_nmoves;
	char			sh_name[MAXMOVES][32];
	blkptr_t		sh_bp[MAXMOVES];
	zbookmark_phys_t	sh_zb[MAXMOVES];
	raidz_redirect_move_t	sh_rdm[MAXMOVES];	/* the kernel fills */
} shared_t;

static shared_t *sh;
static const char *dir;
static uint64_t ashift;
static int failures;

static void
check(boolean_t ok, const char *fmt, ...)
{
	va_list ap;

	(void) printf("%s ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
	(void) printf("\n");
	if (!ok)
		failures++;
}

static void
leaf_path(char *buf, size_t len, int child)
{
	(void) snprintf(buf, len, "%s/v%d", dir, child);
}

static void
fill_pattern(uint64_t *buf, size_t size, uint64_t seed)
{
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ (seed * 0x2545f4914f6cdd1dULL);

	for (size_t i = 0; i < size / sizeof (uint64_t); i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = x;
	}
}

static void
create_pool(void)
{
	nvlist_t *child[WIDTH], *raid, *root, *props;
	char path[PATH_BUF];

	for (int c = 0; c < WIDTH; c++) {
		int fd;

		leaf_path(path, sizeof (path), c);
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, !=, -1);
		VERIFY0(ftruncate(fd, FILE_SIZE));
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, ashift);
	}
	raid = fnvlist_alloc();
	fnvlist_add_string(raid, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(raid, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, WIDTH);
	root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&raid, 1);

	props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char buf[256];

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		(void) snprintf(buf, sizeof (buf), "feature@%s",
		    spa_feature_table[i].fi_uname);
		fnvlist_add_uint64(props, buf, 0);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raid);
	for (int c = 0; c < WIDTH; c++)
		fnvlist_free(child[c]);
}

/* Import the pool found in dir, read-only if libzpool was set up so. */
static spa_t *
import_pool(void)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL;
	spa_t *spa;
	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	memset(&ia, 0, sizeof (ia));
	paths[0] = (char *)dir;
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&lpch, POOL, &config, &ia));
	VERIFY0(spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	return (spa);
}

static void
export_pool(spa_t *spa)
{
	spa_close(spa, FTAG);
	VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
}

static uint64_t
put_object(objset_t *os, uint64_t seed)
{
	uint64_t *buf = malloc(BLKSZ);
	dmu_tx_t *tx;
	uint64_t obj;

	fill_pattern(buf, BLKSZ, seed);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, BLKSZ);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, BLKSZ,
	    DMU_OT_NONE, 0, tx);
	dmu_write(os, obj, 0, BLKSZ, buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	free(buf);
	return (obj);
}

static blkptr_t
object_bp(objset_t *os, uint64_t obj)
{
	dmu_buf_t *db;
	blkptr_t bp;

	VERIFY0(dmu_buf_hold(os, obj, 0, FTAG, &db, DMU_READ_NO_PREFETCH));
	bp = *dmu_buf_get_blkptr(db);
	dmu_buf_rele(db, FTAG);
	return (bp);
}

/* Read an object of the root dataset; 0 and a match, or the error. */
static int
read_object(uint64_t obj, uint64_t seed, boolean_t *match)
{
	objset_t *os;
	uint64_t *buf = malloc(BLKSZ), *want = malloc(BLKSZ);
	int err;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_TRUE, B_TRUE, FTAG, &os));
	fill_pattern(want, BLKSZ, seed);
	memset(buf, 0, BLKSZ);
	err = dmu_read(os, obj, 0, BLKSZ, buf, DMU_READ_NO_PREFETCH);
	*match = (err == 0 && memcmp(buf, want, BLKSZ) == 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(buf);
	free(want);
	return (err);
}

/* The bytes of a block as stored: no checksum, no transform. */
static abd_t *
read_raw(spa_t *spa, const blkptr_t *bp, uint64_t psize)
{
	blkptr_t rbp = *bp;
	abd_t *abd = abd_alloc_linear(psize, B_FALSE);

	for (int d = 1; d < SPA_DVAS_PER_BP; d++)
		memset(&rbp.blk_dva[d], 0, sizeof (dva_t));
	DVA_SET_GANG(&rbp.blk_dva[0], 0);
	BP_SET_LSIZE(&rbp, psize);
	BP_SET_PSIZE(&rbp, psize);
	BP_SET_COMPRESS(&rbp, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(&rbp, ZIO_CHECKSUM_OFF);
	BP_SET_DEDUP(&rbp, 0);
	BP_SET_CRYPT(&rbp, 0);
	ZIO_SET_CHECKSUM(&rbp.blk_cksum, 0, 0, 0, 0);
	VERIFY0(zio_wait(zio_read(NULL, spa, &rbp, abd, psize, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, NULL)));
	return (abd);
}

typedef struct epoch_arg {
	uint64_t	ea_table[6];
} epoch_arg_t;

static int
epoch_check(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	if (!dmu_tx_is_syncing(tx))
		return (0);
	ea->ea_table[3] = vdev_raidz_parity_epochs_first_start(tx);
	return (vdev_raidz_parity_epochs_set_check_tx(vd, ea->ea_table, 2, tx,
	    NULL));
}

static void
epoch_sync(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	VERIFY0(vdev_raidz_parity_epochs_set(vd, ea->ea_table, 2, tx));
}

static void
add_parity2_epoch(spa_t *spa)
{
	epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

	VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
	    ZFS_SPACE_CHECK_NONE));
	txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
	(void) printf("INFO parity 2 from txg %llu\n",
	    (u_longlong_t)ea.ea_table[3]);
}

/*
 * Phase 0: the pool, its objects at parity 1 and the parity-2 epoch.  With
 * <extra>, one more plain object for the stale guard.
 */
static int
child_setup(boolean_t extra)
{
	objset_t *os;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	sh->sh_obj[OA] = put_object(os, obj_seeds[OA]);
	sh->sh_obj[OC] = put_object(os, obj_seeds[OC]);
	if (extra)
		sh->sh_obj_e = put_object(os, 99);
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);

	VERIFY0(dsl_prop_set_int(POOL, "dedup", ZPROP_SRC_LOCAL,
	    ZIO_CHECKSUM_SHA256));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	sh->sh_obj[OD] = put_object(os, obj_seeds[OD]);
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dsl_prop_set_int(POOL, "dedup", ZPROP_SRC_LOCAL,
	    ZIO_CHECKSUM_OFF));

	/* Gang the 128K block, not its children. */
	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	sh->sh_obj[OG] = put_object(os, obj_seeds[OG]);
	txg_wait_synced(spa_get_dsl(spa), 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
	for (int i = 0; i < NOBJ; i++) {
		blkptr_t bp = object_bp(os, sh->sh_obj[i]);

		(void) printf("INFO state: %s object %llu seed %llu "
		    "offset=%llx birth=%llu asize=%llu%s%s\n", obj_names[i],
		    (u_longlong_t)sh->sh_obj[i], (u_longlong_t)obj_seeds[i],
		    (u_longlong_t)DVA_GET_OFFSET(&bp.blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&bp),
		    (u_longlong_t)DVA_GET_ASIZE(&bp.blk_dva[0]),
		    BP_GET_DEDUP(&bp) ? " dedup" : "",
		    BP_IS_GANG(&bp) ? " gang" : "");
	}
	dmu_objset_disown(os, B_TRUE, FTAG);

	add_parity2_epoch(spa);
	export_pool(spa);
	kernel_fini();
	return (0);
}

static uint64_t
gang_header_psize(spa_t *spa)
{
	return (spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER) ?
	    vdev_gang_header_psize(spa->spa_root_vdev->vdev_child[0]) :
	    SPA_OLD_GANGBLOCKSIZE);
}

static void
add_move(spa_t *spa, const char *name, const blkptr_t *bp,
    const zbookmark_phys_t *zb, uint64_t psize)
{
	int i = sh->sh_nmoves++;
	raidz_redirect_move_t *rdm = &sh->sh_rdm[i];

	VERIFY3S(i, <, MAXMOVES);
	(void) strlcpy(sh->sh_name[i], name, sizeof (sh->sh_name[i]));
	sh->sh_bp[i] = *bp;
	sh->sh_zb[i] = *zb;
	memset(rdm, 0, sizeof (*rdm));
	rdm->rdm_bp = &sh->sh_bp[i];
	rdm->rdm_zb = &sh->sh_zb[i];
	rdm->rdm_psize = psize;
	rdm->rdm_data = read_raw(spa, bp, psize);
}

/* Name the blocks of an object, a gang header's children included. */
static void
add_object_moves(spa_t *spa, objset_t *os, int o)
{
	blkptr_t bp = object_bp(os, sh->sh_obj[o]);
	zbookmark_phys_t zb;

	SET_BOOKMARK(&zb, dmu_objset_id(os), sh->sh_obj[o], 0, 0);
	if (!BP_IS_GANG(&bp)) {
		add_move(spa, obj_names[o], &bp, &zb, BP_GET_PSIZE(&bp));
		return;
	}
	add_move(spa, obj_names[o], &bp, &zb, gang_header_psize(spa));
	zio_gbh_phys_t *gbh = abd_to_buf(sh->sh_rdm[sh->sh_nmoves - 1].
	    rdm_data);
	for (int k = 0; k < gbh_nblkptrs(gang_header_psize(spa)); k++) {
		blkptr_t *cbp = gbh_bp(gbh, k);

		if (BP_IS_HOLE(cbp))
			continue;
		VERIFY(!BP_IS_GANG(cbp));
		add_move(spa, "gang-G child", cbp, &zb, BP_GET_PSIZE(cbp));
	}
}

/*
 * The previous step's move, for a libzpool with only the in-memory table:
 * allocate and write N, add the in-memory redirect, free what P2 does not
 * keep, all in one sync task.
 */
static policy_t legacy_policy;

static void
legacy_move_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	uint64_t txg = dmu_tx_get_txg(tx);

	for (int i = 0; i < sh->sh_nmoves; i++) {
		raidz_redirect_move_t *rdm = &sh->sh_rdm[i];
		const blkptr_t *bp = rdm->rdm_bp;
		uint64_t o = DVA_GET_OFFSET(&bp->blk_dva[0]);
		uint64_t t1 = BP_GET_PHYSICAL_BIRTH(bp);
		uint64_t asize = DVA_GET_GANG(&bp->blk_dva[0]) ?
		    vdev_gang_header_asize(tvd, t1) :
		    DVA_GET_ASIZE(&bp->blk_dva[0]);
		uint64_t keep = legacy_policy == P2 ?
		    1ULL << tvd->vdev_ashift : asize;
		zio_alloc_list_t zal;
		blkptr_t wbp;

		memset(&wbp, 0, sizeof (wbp));
		BP_SET_LSIZE(&wbp, rdm->rdm_psize);
		BP_SET_PSIZE(&wbp, rdm->rdm_psize);
		BP_SET_COMPRESS(&wbp, ZIO_COMPRESS_OFF);
		BP_SET_CHECKSUM(&wbp, ZIO_CHECKSUM_OFF);
		BP_SET_TYPE(&wbp, DMU_OT_PLAIN_FILE_CONTENTS);
		BP_SET_LEVEL(&wbp, 0);
		BP_SET_BYTEORDER(&wbp, ZFS_HOST_BYTEORDER);
		metaslab_trace_init(&zal);
		VERIFY0(metaslab_alloc(spa, spa_normal_class(spa),
		    rdm->rdm_psize, &wbp, 1, txg, NULL, 0, &zal, 0, FTAG));
		metaslab_trace_fini(&zal);
		BP_SET_BIRTH(&wbp, txg, txg);
		rdm->rdm_old_asize = asize;
		rdm->rdm_ent = (raidz_redirect_ent_t) {
			.rde_offset = o, .rde_birth = t1,
			.rde_new_offset = DVA_GET_OFFSET(&wbp.blk_dva[0]),
			.rde_new_birth = txg,
			.rde_new_asize = DVA_GET_ASIZE(&wbp.blk_dva[0]),
			.rde_old_reserved = keep,
		};
		VERIFY0(zio_wait(zio_rewrite(NULL, spa, txg, &wbp,
		    rdm->rdm_data, rdm->rdm_psize, NULL, NULL,
		    ZIO_PRIORITY_SYNC_WRITE, ZIO_FLAG_CANFAIL, NULL)));
		VERIFY0(vdev_raidz_redirect_add(0, o, t1,
		    rdm->rdm_ent.rde_new_offset, txg,
		    rdm->rdm_ent.rde_new_asize));
		if (keep < asize) {
			VERIFY(!spa_has_checkpoint(spa));
			spa_config_enter(spa, SCL_FREE, FTAG, RW_READER);
			metaslab_free_concrete(tvd, o + keep, asize - keep,
			    B_FALSE);
			spa_config_exit(spa, SCL_FREE, FTAG);
		}
	}
}

/* Move the named blocks with whichever table this libzpool has. */
static int
do_move(spa_t *spa, policy_t policy, boolean_t arm)
{
	if (vdev_raidz_redirect_move != NULL) {
		return (vdev_raidz_redirect_move(spa, 0, sh->sh_rdm,
		    sh->sh_nmoves, policy == P2 ? RAIDZ_REDIRECT_KEEP_FIRST :
		    RAIDZ_REDIRECT_KEEP_ALL, arm));
	}
	legacy_policy = policy;
	VERIFY0(dsl_sync_task(POOL, NULL, legacy_move_sync, NULL, 0,
	    ZFS_SPACE_CHECK_NONE));
	return (0);
}

/* Child A: move every block of the plain, dedup and gang objects. */
static int
child_move(int point, policy_t policy, boolean_t split)
{
	objset_t *os;
	spa_t *spa;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	sh->sh_nmoves = 0;
	add_object_moves(spa, os, OA);
	add_object_moves(spa, os, OD);
	add_object_moves(spa, os, OG);
	dmu_objset_disown(os, B_TRUE, FTAG);
	(void) printf("INFO moving %d blocks under %s, crash at %s%s, "
	    "after txg %llu\n", sh->sh_nmoves, policy_names[policy],
	    point_names[point], split ? ", redirects a txg later" : "",
	    (u_longlong_t)spa_last_synced_txg(spa));

	if (vdev_raidz_redirect_move == NULL) {
		(void) printf("INFO this libzpool has the in-memory table "
		    "only\n");
		if (point != SPIKE_CRASH_POST_UB || split) {
			(void) printf("SKIP %s cannot be emulated here\n",
			    point_names[point]);
			return (2);
		}
		VERIFY0(do_move(spa, policy, B_FALSE));
		/* T has synced: its uberblock is on disk. */
		(void) printf("INFO the move's txg has synced; exiting as a "
		    "crash after its uberblock would\n");
		_exit(99);
	}
	spike_crash_point = point;
	spike_redirect_split_txg = split;
	err = do_move(spa, policy, B_TRUE);
	(void) printf("FAIL the move returned %d without reaching %s\n",
	    err, point_names[point]);
	return (1);
}

/*
 * The free space of each metaslab of vdev 0 as the pool on disk has it.  A
 * read-only import reads no log space map, so a loaded metaslab would miss
 * what the txgs not yet flushed to it changed, the move among them: build
 * private trees from each metaslab's space map, under its lock, and apply
 * the log space maps from its unflushed txg on, as zdb does.
 */
static zfs_range_tree_t **free_trees;

typedef struct log_arg {
	spa_t		*la_spa;
	uint64_t	la_txg;
} log_arg_t;

static int
apply_log_entry(space_map_entry_t *sme, void *arg)
{
	log_arg_t *la = arg;
	vdev_t *tvd = la->la_spa->spa_root_vdev->vdev_child[0];
	uint64_t m = sme->sme_offset >> tvd->vdev_ms_shift;

	if (sme->sme_vdev != 0 ||
	    la->la_txg < metaslab_unflushed_txg(tvd->vdev_ms[m]))
		return (0);
	if (sme->sme_type == SM_FREE)
		zfs_range_tree_add(free_trees[m], sme->sme_offset,
		    sme->sme_run);
	else
		zfs_range_tree_remove(free_trees[m], sme->sme_offset,
		    sme->sme_run);
	return (0);
}

static void
build_free_trees(spa_t *spa)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];

	free_trees = calloc(tvd->vdev_ms_count, sizeof (*free_trees));
	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		metaslab_t *msp = tvd->vdev_ms[m];

		free_trees[m] = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
		    NULL, 0, 0);
		mutex_enter(&msp->ms_lock);
		if (msp->ms_sm != NULL) {
			VERIFY0(space_map_load(msp->ms_sm, free_trees[m],
			    SM_FREE));
		} else {
			zfs_range_tree_add(free_trees[m], msp->ms_start,
			    msp->ms_size);
		}
		mutex_exit(&msp->ms_lock);
	}
	if (!spa_feature_is_active(spa, SPA_FEATURE_LOG_SPACEMAP))
		return;
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	for (spa_log_sm_t *sls = avl_first(&spa->spa_sm_logs_by_txg);
	    sls != NULL; sls = AVL_NEXT(&spa->spa_sm_logs_by_txg, sls)) {
		space_map_t *sm = NULL;
		log_arg_t la = { spa, sls->sls_txg };

		VERIFY0(space_map_open(&sm, spa_meta_objset(spa),
		    sls->sls_sm_obj, 0, UINT64_MAX, SPA_MINBLOCKSHIFT));
		VERIFY0(space_map_iterate(sm, space_map_length(sm),
		    apply_log_entry, &la));
		space_map_close(sm);
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
}

/*
 * Where [offset, offset + size) of vdev 0 stands, checked under its
 * metaslab's lock: 1 wholly free, 0 wholly allocated, -1 partly each.
 */
static int
range_state(spa_t *spa, uint64_t offset, uint64_t size)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	uint64_t m = offset >> tvd->vdev_ms_shift;
	metaslab_t *msp = tvd->vdev_ms[m];
	uint64_t s;
	uint64_t l;
	int state;

	mutex_enter(&msp->ms_lock);
	if (zfs_range_tree_contains(free_trees[m], offset, size))
		state = 1;
	else if (!zfs_range_tree_find_in(free_trees[m], offset, size, &s, &l))
		state = 0;
	else
		state = -1;
	mutex_exit(&msp->ms_lock);
	return (state);
}

static const char *
state_name(int state)
{
	return (state == 1 ? "free" : state == 0 ? "allocated" : "mixed");
}

/* Child B: what a fresh, read-only import of the crashed pool sees. */
static int
child_verify(boolean_t committed)
{
	spa_t *spa;
	uint64_t entries = 0;
	uint64_t hits;
	int st;

	/* A read-only import opens no space map unless asked to. */
	spa_mode_readable_spacemaps = B_TRUE;
	kernel_init(SPA_MODE_READ);
	spa = import_pool();
	VERIFY(!spa_writeable(spa));
	(void) printf("INFO read-only import at txg %llu; expecting the "
	    "state %s the move\n", (u_longlong_t)spa_last_synced_txg(spa),
	    committed ? "after" : "before");

	/* The move's txg T, from the first block it allocated N for. */
	for (int i = 0; i < sh->sh_nmoves; i++) {
		uint64_t t = sh->sh_rdm[i].rdm_ent.rde_new_birth;

		if (t == 0)
			continue;
		check(committed ? spa_last_synced_txg(spa) >= t :
		    spa_last_synced_txg(spa) < t, "B the newest uberblock, txg "
		    "%llu, is %s the move's txg %llu",
		    (u_longlong_t)spa_last_synced_txg(spa),
		    committed ? "at or after" : "before", (u_longlong_t)t);
		break;
	}

	if (vdev_raidz_redirect_count != NULL)
		entries = vdev_raidz_redirect_count(spa, 0);
	else
		(void) printf("INFO this libzpool keeps no redirect across "
		    "processes\n");
	check(entries == (committed ? sh->sh_nmoves : 0),
	    "B %llu redirects loaded (expected %d)", (u_longlong_t)entries,
	    committed ? sh->sh_nmoves : 0);

	build_free_trees(spa);
	hits = vdev_raidz_redirect_hits();
	for (int i = 0; i < NOBJ; i++) {
		boolean_t match;
		int err = read_object(sh->sh_obj[i], obj_seeds[i], &match);

		check(err == 0 && match, "B %s reads back bit-exact "
		    "(err=%d match=%d)", obj_names[i], err, match);
	}
	hits = vdev_raidz_redirect_hits() - hits;
	check(hits == (committed ? sh->sh_nmoves : 0), "B %llu redirect "
	    "hits for %d moved blocks (expected %d)", (u_longlong_t)hits,
	    sh->sh_nmoves, committed ? sh->sh_nmoves : 0);

	for (int i = 0; i < sh->sh_nmoves; i++) {
		const raidz_redirect_move_t *rdm = &sh->sh_rdm[i];
		const raidz_redirect_ent_t *e = &rdm->rdm_ent;
		uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
		uint64_t a = rdm->rdm_old_asize;
		uint64_t r = e->rde_old_reserved;

		(void) printf("INFO %s: O=%llx asize %llu kept %llu -> "
		    "N=%llx asize %llu birth %llu (error %d)\n", sh->sh_name[i],
		    (u_longlong_t)o, (u_longlong_t)a, (u_longlong_t)r,
		    (u_longlong_t)e->rde_new_offset,
		    (u_longlong_t)e->rde_new_asize,
		    (u_longlong_t)e->rde_new_birth, rdm->rdm_error);
		if (e->rde_new_asize == 0) {
			check(B_FALSE, "B %s: the move never allocated N",
			    sh->sh_name[i]);
			continue;
		}
		if (!committed) {
			st = range_state(spa, o, a);
			check(st == 0, "B %s: old extent [%llx, +%llu) %s "
			    "(expected allocated)", sh->sh_name[i],
			    (u_longlong_t)o, (u_longlong_t)a, state_name(st));
			st = range_state(spa, e->rde_new_offset,
			    e->rde_new_asize);
			check(st == 1, "B %s: N [%llx, +%llu) %s (expected "
			    "free)", sh->sh_name[i],
			    (u_longlong_t)e->rde_new_offset,
			    (u_longlong_t)e->rde_new_asize, state_name(st));
			continue;
		}
		if (r != 0) {
			st = range_state(spa, o, r);
			check(st == 0, "B %s: kept [%llx, +%llu) %s (expected "
			    "allocated)", sh->sh_name[i], (u_longlong_t)o,
			    (u_longlong_t)r, state_name(st));
		}
		if (r < a) {
			st = range_state(spa, o + r, a - r);
			check(st == 1, "B %s: tail [%llx, +%llu) %s (expected "
			    "free)", sh->sh_name[i], (u_longlong_t)(o + r),
			    (u_longlong_t)(a - r), state_name(st));
		}
		st = range_state(spa, e->rde_new_offset, e->rde_new_asize);
		check(st == 0, "B %s: N [%llx, +%llu) %s (expected "
		    "allocated)", sh->sh_name[i],
		    (u_longlong_t)e->rde_new_offset,
		    (u_longlong_t)e->rde_new_asize, state_name(st));
	}
	/* A read-only pool cannot be exported; kernel_fini() unloads it. */
	spa_close(spa, FTAG);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* Run fn(arg) in a child; its exit status, or 128 + its signal. */
static int
phase(const char *name, int (*fn)(void *), void *arg)
{
	pid_t pid;
	int status;

	(void) fflush(stdout);
	pid = fork();
	VERIFY3S(pid, !=, -1);
	if (pid == 0)
		_exit(fn(arg));
	VERIFY3S(waitpid(pid, &status, 0), ==, pid);
	if (WIFEXITED(status)) {
		(void) printf("INFO phase %s: exit status %d\n", name,
		    WEXITSTATUS(status));
		return (WEXITSTATUS(status));
	}
	(void) printf("INFO phase %s: killed by signal %d\n", name,
	    WTERMSIG(status));
	return (128 + WTERMSIG(status));
}

typedef struct move_args {
	int		ma_point;
	policy_t	ma_policy;
	boolean_t	ma_split;
} move_args_t;

static int
setup_fn(void *arg)
{
	return (child_setup(arg != NULL));
}

static int
move_fn(void *arg)
{
	move_args_t *ma = arg;

	return (child_move(ma->ma_point, ma->ma_policy, ma->ma_split));
}

static int
verify_fn(void *arg)
{
	return (child_verify(*(boolean_t *)arg));
}

/*
 * Run zdb on the pool and parse what its exit status does not say: leaked
 * space, the block sum against the space maps, claim errors, MOS objects.
 */
static void
run_zdb(void)
{
	const char *zdb = getenv("ZDB") != NULL ? getenv("ZDB") : "zdb";
	char cmd[PATH_BUF * 2];
	char line[4096];
	int leaked = 0, claims = 0, mos = 0, noleaks = 0, status;
	FILE *fp;

	(void) snprintf(cmd, sizeof (cmd), "%s -bcc -d -Y -e -p %s %s 2>&1",
	    zdb, dir, POOL);
	if (getenv("SPIKE_ZDB_IGNORE_REDIRECTS") != NULL) {
		(void) printf("INFO SPIKE_ZDB_IGNORE_REDIRECTS=%s\n",
		    getenv("SPIKE_ZDB_IGNORE_REDIRECTS"));
	}
	(void) printf("INFO running %s\n", cmd);
	(void) fflush(stdout);
	fp = popen(cmd, "r");
	VERIFY3P(fp, !=, NULL);
	while (fgets(line, sizeof (line), fp) != NULL) {
		char *p = line;
		char *cr;

		/* zdb separates its progress reports with a CR */
		while ((cr = strchr(p, '\r')) != NULL)
			p = cr + 1;
		(void) printf("zdb| %s%s", p, strchr(p, '\n') ? "" : "\n");
		if (strstr(p, "leaked space") != NULL)
			leaked++;
		if (strstr(p, "block claim error") != NULL)
			claims++;
		if (strstr(p, "MOS object") != NULL)
			mos++;
		if (strstr(p, "No leaks (block sum matches space maps "
		    "exactly)") != NULL)
			noleaks++;
	}
	status = pclose(fp);
	check(leaked == 0, "zdb reports no leaked space (%d lines)", leaked);
	check(noleaks == 1, "zdb finds the block sum equal to the space maps");
	check(claims == 0, "zdb reports no block claim error (%d)", claims);
	check(mos == 0, "zdb reports no leaked or missing MOS object (%d)",
	    mos);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "zdb exits 0 (status %d)", WIFEXITED(status) ?
	    WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

static int
run_move(int point, policy_t policy, boolean_t split)
{
	move_args_t ma = { point, policy, split };
	boolean_t committed = (point == SPIKE_CRASH_POST_UB);
	int st;

	(void) printf("INFO move, crash at %s, policy %s, ashift %llu%s\n",
	    point_names[point], policy_names[policy], (u_longlong_t)ashift,
	    split ? ", split txgs" : "");
	VERIFY0(phase("0 setup", setup_fn, NULL));
	st = phase("A move", move_fn, &ma);
	if (st == 2)
		return (2);
	check(st == 99, "A exits at the crash point (status %d)", st);
	st = phase("B verify", verify_fn, &committed);
	check(st == 0, "B finds the state %s the move (status %d)",
	    committed ? "after" : "before", st);
	run_zdb();
	return (0);
}

/* The guards, each in a child of its own after phase 0. */
static int
guard_fn(void *arg)
{
	const char *which = arg;
	objset_t *os;
	spa_t *spa;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	sh->sh_nmoves = 0;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	if (strcmp(which, "mos") == 0) {
		objset_t *mos = spa->spa_meta_objset;
		dmu_buf_t *db;
		blkptr_t bp;
		zbookmark_phys_t zb;

		VERIFY0(dmu_buf_hold(mos, DMU_POOL_DIRECTORY_OBJECT, 0, FTAG,
		    &db, DMU_READ_NO_PREFETCH));
		bp = *dmu_buf_get_blkptr(db);
		dmu_buf_rele(db, FTAG);
		SET_BOOKMARK(&zb, DMU_META_OBJSET, DMU_POOL_DIRECTORY_OBJECT,
		    0, 0);
		add_move(spa, "MOS directory", &bp, &zb, BP_GET_PSIZE(&bp));
	} else if (strcmp(which, "zil") == 0) {
		blkptr_t bp = object_bp(os, sh->sh_obj[OA]);
		zbookmark_phys_t zb;

		SET_BOOKMARK(&zb, dmu_objset_id(os), ZB_ZIL_OBJECT,
		    ZB_ZIL_LEVEL, 0);
		add_move(spa, "plain-A as a ZIL block", &bp, &zb,
		    BP_GET_PSIZE(&bp));
	} else if (strcmp(which, "checkpoint") == 0) {
		add_object_moves(spa, os, OA);
	} else {
		blkptr_t bp = object_bp(os, sh->sh_obj_e);
		zbookmark_phys_t zb;

		SET_BOOKMARK(&zb, dmu_objset_id(os), sh->sh_obj_e, 0, 0);
		add_move(spa, "freed plain-E", &bp, &zb, BP_GET_PSIZE(&bp));
		VERIFY0(dmu_free_long_object(os, sh->sh_obj_e));
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
	if (strcmp(which, "checkpoint") == 0) {
		VERIFY0(spa_checkpoint(POOL));
		check(spa_has_checkpoint(spa), "the pool has a checkpoint");
	}

	err = do_move(spa, P2, B_FALSE);
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("INFO move of %s: returned %d, block error %d, N=%llx\n",
	    sh->sh_name[0], err, sh->sh_rdm[0].rdm_error,
	    (u_longlong_t)sh->sh_rdm[0].rdm_ent.rde_new_offset);
	if (strcmp(which, "stale") == 0) {
		check(err == 0 && sh->sh_rdm[0].rdm_error == ENOENT &&
		    sh->sh_rdm[0].rdm_ent.rde_new_asize == 0,
		    "the move skips a block whose extent was freed (error %d, "
		    "block error %d)", err, sh->sh_rdm[0].rdm_error);
	} else if (strcmp(which, "checkpoint") == 0) {
		check(err == ZFS_ERR_CHECKPOINT_EXISTS, "the move refuses a "
		    "pool with a checkpoint (error %d)", err);
	} else {
		check(err == EINVAL, "the move refuses %s (error %d)",
		    sh->sh_name[0], err);
	}
	if (vdev_raidz_redirect_count != NULL) {
		check(vdev_raidz_redirect_count(spa, 0) == 0,
		    "no redirect was added (%llu)",
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	}
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

static int
run_guard(const char *which)
{
	int st;

	(void) printf("INFO guard %s, ashift %llu\n", which,
	    (u_longlong_t)ashift);
	VERIFY0(phase("0 setup", setup_fn, (void *)1));
	st = phase("guard", guard_fn, (void *)which);
	check(st == 0, "the guard child passes (status %d)", st);
	return (0);
}

static int
parse_point(const char *s)
{
	for (int p = SPIKE_CRASH_TABLE_DIRTY; p <= SPIKE_CRASH_POST_UB; p++) {
		if (strcmp(s, point_names[p]) == 0)
			return (p);
	}
	return (-1);
}

int
main(int argc, char **argv)
{
	int point = -1;
	policy_t policy = P1;
	int st;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 6 && strcmp(argv[3], "move") == 0) {
		point = parse_point(argv[4]);
		if (strcmp(argv[5], "P2") == 0)
			policy = P2;
		else if (strcmp(argv[5], "P1") != 0)
			point = -1;
		if (argc == 7 && strcmp(argv[6], "split") != 0)
			point = -1;
	}
	if (!((point != -1 && argc <= 7) || (argc == 5 &&
	    strcmp(argv[3], "guard") == 0 && (strcmp(argv[4], "mos") == 0 ||
	    strcmp(argv[4], "zil") == 0 || strcmp(argv[4], "stale") == 0 ||
	    strcmp(argv[4], "checkpoint") == 0)))) {
		(void) fprintf(stderr, "usage: crash_probe <dir> <ashift> move "
		    "<TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [split]\n"
		    "       crash_probe <dir> <ashift> guard "
		    "<mos|zil|stale|checkpoint>\n");
		return (2);
	}
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 10);
	sh = mmap(NULL, sizeof (*sh), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	VERIFY3P(sh, !=, MAP_FAILED);

	if (point != -1)
		st = run_move(point, policy, argc == 7);
	else
		st = run_guard(argv[4]);
	if (st == 2) {
		(void) printf("SKIP: not run on this libzpool\n");
		return (2);
	}
	(void) printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures);
	return (failures == 0 ? 0 : 1);
}
