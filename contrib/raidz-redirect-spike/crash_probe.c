// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 5 (throwaway, not part of any tree): is the move of old-epoch
 * raidz blocks atomic across a crash, does a fresh process find the
 * redirects, does zdb account for moved blocks (part A), and does freeing a
 * moved block release what it holds, in one txg with its redirect (part B)?
 *
 *   crash_probe <dir> <ashift> move <point> <P1|P2> [split]
 *   crash_probe <dir> <ashift> guard <mos|zil|stale|checkpoint|wrongzb>
 *   crash_probe <dir> <ashift> free <plain|dedup|clone|gang|snap> <P1|P2>
 *   crash_probe <dir> <ashift> free plain P3 [nocheck]
 *   crash_probe <dir> <ashift> fcrash <point> <P1|P2> [late]
 *   crash_probe <dir> <ashift> converge <P1|P2> [nolog]
 *   crash_probe <dir> <ashift> resilver <P1|P2>
 *       <offline|offlinescrub|control|replace> [nofix|nowiden|nocols]
 *   crash_probe <dir> <ashift> stalebp <P1|P2> [same] [nolive]
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
 * 'wrongzb' moves gang-G's children with plain-A's bookmark, and plain-A's
 * block with gang-G's and at level -1: the move must refuse each one
 * (ESTALE).
 *
 * free: phase 0 writes, at parity 1, a block that only snapshot s1 holds, a
 * control, then a plain object, a clone pair (one block, one BRT reference),
 * a dedup pair (one block, refcount 2) and a gang object, and adds the
 * parity-2 epoch.  Phase 1 moves every block but the control's.  Phase 2
 * frees one case: plain-A; dedup-D1 then dedup-D2; clone-K then clone-K2;
 * gang-G; or destroys s1.  The first free of a pair must change nothing.
 * After the free that converts, its txg and TXG_DEFER_SIZE more, then again
 * after an export and import: each freed moved DVA's entry is gone from
 * memory and from the table, [O, O + kept) and N are in ms_allocatable, the
 * bytes those extents held are all released, vs_alloc agrees with the
 * metaslab trees, and every other object reads back; zdb must find no leak.
 * P3 (keep nothing) moves and frees in the same child, while the whole old
 * extent is free: metaslab_check_free() must check N instead of it, and
 * 'nocheck' clears spike_redirect_check_enable to show it otherwise.
 *
 * fcrash: after phases 0 and 1, child A frees plain-A with the crash point
 * armed for the txg of the free (spike_crash_arm_free) and must exit 99;
 * child B, a read-only import, finds plain-A, its entry, [O, O + kept) and
 * N as before the free at the first three points, and all of them gone at
 * POST_UB; then zdb.  'late' sets spike_redirect_late_remove, which removes
 * the table entry a txg after the frees.  Without the free of moved blocks
 * in libzpool, POST_UB is emulated by exiting once the free's txg synced.
 *
 * converge: frees, in one tx, many moved plain, dedup and gang objects next
 * to a kept clone pair (so that their frees can go through the free list
 * and the DDT), with or without the log space map, and prints, from the
 * test-only spike_redirect_stats, the frees and table removals of each sync
 * pass: every key must leave the table in the txg of its free.
 *
 * resilver: phase 0 is the free modes' plus small-Z, one sector of data,
 * whose new extent has one column more than its old one.  'offline' takes
 * child 2 offline, moves every block (the new extents miss its column, and
 * its DTL holds the move's txg), brings it back and waits for the resilver
 * to finish; 'offlinescrub' then also scrubs; 'control' keeps the child
 * online.  'replace' moves with every child online, then replaces with a
 * new file a child that holds a column of small-Z's new extent and none of
 * its old one, and waits for the resilver and the detach.  The child must
 * hold a non-zero column of the target's new extent (plain-A, or small-Z
 * for replace); two other columns of that extent are then damaged and the
 * target must still read back, which parity 2 allows only if the resilver
 * rebuilt the child's column.  zdb runs after the resilver.  'nofix',
 * 'nowiden' and 'nocols' clear both or one of the bits of the test-only
 * spike_resilver_redirects.
 *
 * stalebp: phase 0 writes an object E at parity 1, frees it, refills its
 * old extent (one allocator, best fit) with live objects, with E's content
 * under 'same', and adds the parity-2 epoch.  The move of E's saved block
 * pointer must be refused, leave nothing allocated and E's old extent
 * whole; small writes then try to reuse anything it freed, every live object
 * must read back after an export and import, and zdb must find no leak and
 * no claim error.  'nolive' clears the test-only spike_redirect_move_live.
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
#include <sys/dnode.h>
#include <sys/brt.h>
#include <sys/dsl_destroy.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
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
#define	SNAP		"crp@s1"
#define	WIDTH		5
#define	FILE_SIZE	(256ULL << 20)
#define	BLKSZ		(128ULL << 10)
#define	PATH_BUF	512
#define	MAXMOVES	64
#define	MAXOBJS		96
#define	POLL_TXGS	40

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
/* Weak: the free of moved blocks, absent from the previous step's. */
extern int spike_crash_arm_free __attribute__((weak));
extern int spike_redirect_late_remove __attribute__((weak));
extern int spike_redirect_check_enable __attribute__((weak));
extern spike_redirect_stat_t spike_redirect_stats[SPIKE_REDIRECT_TXGS]
    __attribute__((weak));
/* Weak: the resilver and liveness fixes, absent from the previous step's. */
extern int spike_resilver_redirects __attribute__((weak));
extern int spike_redirect_move_live __attribute__((weak));
/* Weak: the previous step's in-memory table, absent from this one. */
extern int vdev_raidz_redirect_add(uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t) __attribute__((weak));

typedef enum policy { P1, P2, P3 } policy_t;
static const char *const policy_names[] = { "P1", "P2", "P3" };
static const char *const point_names[] = { "NONE", "TABLE_DIRTY",
	"MS_SYNCED", "PRE_UB", "POST_UB" };

enum { OA, OD, OG, OC, NOBJ };
static const char *const obj_names[] = { "plain-A", "dedup-D", "gang-G",
	"control-C" };
static const uint64_t obj_seeds[] = { 1, 2, 3, 4 };

/* An object of the free and converge modes. */
typedef struct fobj {
	char		fo_name[24];
	uint64_t	fo_seed;
	uint64_t	fo_obj;
	int		fo_shares;	/* fobj whose block it shares, or -1 */
	boolean_t	fo_snap;	/* lives only in the snapshot */
	boolean_t	fo_moved;
	boolean_t	fo_gone;	/* freed by the test */
	uint64_t	fo_size;	/* its one block's size, 0 for BLKSZ */
} fobj_t;

/* What the phases share: the parent maps it before forking any of them. */
typedef struct shared {
	uint64_t		sh_obj[NOBJ];
	uint64_t		sh_obj_e;	/* guard stale: freed object */
	int			sh_nmoves;
	char			sh_name[MAXMOVES][32];
	blkptr_t		sh_bp[MAXMOVES];
	zbookmark_phys_t	sh_zb[MAXMOVES];
	raidz_redirect_move_t	sh_rdm[MAXMOVES];	/* the kernel fills */
	int			sh_owner[MAXMOVES];	/* fobj, or -1 */
	int			sh_nfobj;
	fobj_t			sh_fobj[MAXOBJS];
	blkptr_t		sh_snap_bp;	/* the snapshot-only block */
	uint64_t		sh_free_txg;	/* txg of the armed free */
	boolean_t		sh_small;	/* setup adds small-Z */
	int			sh_watch;	/* resilver: child watched */
	int			sh_replaced;	/* resilver: replaced + 1 */
	blkptr_t		sh_stale_bp;	/* stalebp: E's freed bp */
	zbookmark_phys_t	sh_stale_zb;
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

/* Features create_pool() leaves disabled: converge's variants. */
static const char *skip_features[2];

static boolean_t
feature_skipped(const char *name)
{
	for (int i = 0; i < 2; i++) {
		if (skip_features[i] != NULL &&
		    strcmp(skip_features[i], name) == 0)
			return (B_TRUE);
	}
	return (B_FALSE);
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

		if (!spa_feature_table[i].fi_zfs_mod_supported ||
		    feature_skipped(spa_feature_table[i].fi_uname))
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

/* A new object of one block of <size> bytes of pattern <seed>. */
static uint64_t
put_object_size(objset_t *os, uint64_t seed, uint64_t size)
{
	uint64_t *buf = malloc(size);
	dmu_tx_t *tx;
	uint64_t obj;

	fill_pattern(buf, size, seed);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, size);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, size,
	    DMU_OT_NONE, 0, tx);
	dmu_write(os, obj, 0, size, buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	free(buf);
	return (obj);
}

static uint64_t
put_object(objset_t *os, uint64_t seed)
{
	return (put_object_size(os, seed, BLKSZ));
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
		    policy == P3 ? RAIDZ_REDIRECT_KEEP_NONE :
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
	} else if (strcmp(which, "wrongzb") == 0) {
		/* Live blocks, each named by the other object's bookmark. */
		zbookmark_phys_t zba, zbg;

		add_object_moves(spa, os, OG);
		add_object_moves(spa, os, OA);
		SET_BOOKMARK(&zba, dmu_objset_id(os), sh->sh_obj[OA], 0, 0);
		SET_BOOKMARK(&zbg, dmu_objset_id(os), sh->sh_obj[OG], 0, 0);
		/* Drop gang-G's header: its children follow, then plain-A. */
		for (int i = 1; i < sh->sh_nmoves; i++) {
			(void) memcpy(sh->sh_name[i - 1], sh->sh_name[i],
			    sizeof (sh->sh_name[i]));
			sh->sh_bp[i - 1] = sh->sh_bp[i];
			sh->sh_rdm[i - 1] = sh->sh_rdm[i];
			sh->sh_rdm[i - 1].rdm_bp = &sh->sh_bp[i - 1];
			sh->sh_rdm[i - 1].rdm_zb = &sh->sh_zb[i - 1];
			sh->sh_zb[i - 1] = (i == sh->sh_nmoves - 1) ? zbg :
			    zba;
		}
		sh->sh_nmoves--;
		/* And plain-A named at a level no object block has. */
		SET_BOOKMARK(&zba, dmu_objset_id(os), sh->sh_obj[OA], -1, 0);
		add_move(spa, "plain-A at level -1", &sh->sh_bp[sh->sh_nmoves -
		    1], &zba, sh->sh_rdm[sh->sh_nmoves - 1].rdm_psize);
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
	for (int i = 0; i < sh->sh_nmoves; i++) {
		(void) printf("INFO move of %s: returned %d, block error %d, "
		    "N=%llx\n", sh->sh_name[i], err, sh->sh_rdm[i].rdm_error,
		    (u_longlong_t)sh->sh_rdm[i].rdm_ent.rde_new_offset);
	}
	if (strcmp(which, "wrongzb") == 0) {
		int refused = 0;

		for (int i = 0; i < sh->sh_nmoves; i++) {
			if (sh->sh_rdm[i].rdm_error == ESTALE &&
			    sh->sh_rdm[i].rdm_ent.rde_new_asize == 0)
				refused++;
		}
		check(err == 0 && refused == sh->sh_nmoves, "the move refuses "
		    "every live block named by another object's bookmark, "
		    "gang children included (%d of %d with ESTALE)", refused,
		    sh->sh_nmoves);
	} else if (strcmp(which, "stale") == 0) {
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

/*
 * The free, fcrash and converge modes: freeing moved blocks.
 */

static int
add_fobj(const char *name, uint64_t seed, boolean_t move)
{
	int i = sh->sh_nfobj++;
	fobj_t *fo = &sh->sh_fobj[i];

	VERIFY3S(i, <, MAXOBJS);
	memset(fo, 0, sizeof (*fo));
	(void) strlcpy(fo->fo_name, name, sizeof (fo->fo_name));
	fo->fo_seed = seed;
	fo->fo_shares = -1;
	fo->fo_moved = move;
	return (i);
}

static int
find_fobj(const char *name)
{
	for (int i = 0; i < sh->sh_nfobj; i++) {
		if (strcmp(sh->sh_fobj[i].fo_name, name) == 0)
			return (i);
	}
	VERIFY(B_FALSE);
	return (-1);
}

/* An object holding a clone of src's only block, made with the BRT. */
static uint64_t
clone_object(spa_t *spa, objset_t *os, uint64_t src)
{
	blkptr_t bps[1];
	size_t nbps = 1;
	dnode_t *dn;
	dmu_tx_t *tx;
	uint64_t obj;

	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, BLKSZ,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	txg_wait_synced(spa_get_dsl(spa), 0);

	VERIFY0(dmu_read_l0_bps(os, src, 0, BLKSZ, bps, &nbps));
	VERIFY3U(nbps, ==, 1);
	VERIFY0(dnode_hold(os, obj, FTAG, &dn));
	tx = dmu_tx_create(os);
	dmu_tx_hold_clone_by_dnode(tx, dn, 0, BLKSZ, BLKSZ);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY0(dmu_brt_clone(os, obj, 0, BLKSZ, tx, bps, nbps));
	dmu_tx_commit(tx);
	dnode_rele(dn, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
	return (obj);
}

static blkptr_t
fobj_bp(objset_t *os, int f)
{
	return (sh->sh_fobj[f].fo_snap ? sh->sh_snap_bp :
	    object_bp(os, sh->sh_fobj[f].fo_obj));
}

static void
print_fobjs(objset_t *os)
{
	for (int i = 0; i < sh->sh_nfobj; i++) {
		blkptr_t bp = fobj_bp(os, i);

		(void) printf("INFO state: %s object %llu seed %llu "
		    "offset=%llx birth=%llu asize=%llu%s%s%s%s\n",
		    sh->sh_fobj[i].fo_name,
		    (u_longlong_t)sh->sh_fobj[i].fo_obj,
		    (u_longlong_t)sh->sh_fobj[i].fo_seed,
		    (u_longlong_t)DVA_GET_OFFSET(&bp.blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&bp),
		    (u_longlong_t)DVA_GET_ASIZE(&bp.blk_dva[0]),
		    BP_GET_DEDUP(&bp) ? " dedup" : "",
		    BP_IS_GANG(&bp) ? " gang" : "",
		    sh->sh_fobj[i].fo_snap ? " snapshot-only" : "",
		    sh->sh_fobj[i].fo_moved ? " to-move" : "");
	}
}

static void
set_dedup(uint64_t checksum)
{
	VERIFY0(dsl_prop_set_int(POOL, "dedup", ZPROP_SRC_LOCAL, checksum));
}

/*
 * Phase 0 of the free modes: a plain object, a dedup pair (one block, two
 * references), a clone pair (one block, one BRT reference), a gang object, a
 * block that only snapshot s1 holds, and a control; then the parity-2 epoch.
 */
static int
child_setup_free(void)
{
	objset_t *os;
	spa_t *spa;
	int a, k, k2, s, d1, d2, g;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	sh->sh_nfobj = 0;

	/*
	 * S lives on in snapshot s1 only.  Everything the test frees in the
	 * head is written after s1, which would otherwise hold it too.
	 */
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	s = add_fobj("snap-S", 16, B_TRUE);
	sh->sh_fobj[s].fo_obj = put_object(os, 16);
	sh->sh_fobj[add_fobj("control-C", 14, B_FALSE)].fo_obj =
	    put_object(os, 14);
	txg_wait_synced(spa_get_dsl(spa), 0);
	sh->sh_snap_bp = object_bp(os, sh->sh_fobj[s].fo_obj);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dmu_objset_snapshot_one(POOL, "s1"));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	VERIFY0(dmu_free_long_object(os, sh->sh_fobj[s].fo_obj));
	sh->sh_fobj[s].fo_snap = B_TRUE;
	txg_wait_synced(spa_get_dsl(spa), 0);

	a = add_fobj("plain-A", 11, B_TRUE);
	sh->sh_fobj[a].fo_obj = put_object(os, 11);
	k = add_fobj("clone-K", 15, B_TRUE);
	sh->sh_fobj[k].fo_obj = put_object(os, 15);
	txg_wait_synced(spa_get_dsl(spa), 0);
	k2 = add_fobj("clone-K2", 15, B_FALSE);
	sh->sh_fobj[k2].fo_obj = clone_object(spa, os, sh->sh_fobj[k].fo_obj);
	sh->sh_fobj[k2].fo_shares = k;
	dmu_objset_disown(os, B_TRUE, FTAG);

	set_dedup(ZIO_CHECKSUM_SHA256);
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	d1 = add_fobj("dedup-D1", 12, B_TRUE);
	sh->sh_fobj[d1].fo_obj = put_object(os, 12);
	txg_wait_synced(spa_get_dsl(spa), 0);
	d2 = add_fobj("dedup-D2", 12, B_FALSE);
	sh->sh_fobj[d2].fo_obj = put_object(os, 12);
	sh->sh_fobj[d2].fo_shares = d1;
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	set_dedup(ZIO_CHECKSUM_OFF);

	/* Gang the 128K block, not its children. */
	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	g = add_fobj("gang-G", 13, B_TRUE);
	sh->sh_fobj[g].fo_obj = put_object(os, 13);
	txg_wait_synced(spa_get_dsl(spa), 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
	if (sh->sh_small) {
		/* One sector of data: 2 columns at parity 1, 3 at parity 2. */
		int z = add_fobj("small-Z", 17, B_TRUE);

		sh->sh_fobj[z].fo_size = 1ULL << ashift;
		sh->sh_fobj[z].fo_obj = put_object_size(os, 17,
		    sh->sh_fobj[z].fo_size);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	print_fobjs(os);
	{
		blkptr_t b1 = object_bp(os, sh->sh_fobj[d1].fo_obj);
		blkptr_t b2 = object_bp(os, sh->sh_fobj[d2].fo_obj);
		blkptr_t bk = object_bp(os, sh->sh_fobj[k].fo_obj);

		VERIFY(DVA_EQUAL(&b1.blk_dva[0], &b2.blk_dva[0]));
		VERIFY3U(BP_GET_PHYSICAL_BIRTH(&b1), ==,
		    BP_GET_PHYSICAL_BIRTH(&b2));
		(void) printf("INFO dedup pair shares one DVA; BRT refcount of "
		    "clone-K's block: %llu\n",
		    (u_longlong_t)brt_entry_get_refcount(spa, &bk));
	}
	dmu_objset_disown(os, B_TRUE, FTAG);

	add_parity2_epoch(spa);
	export_pool(spa);
	kernel_fini();
	return (0);
}

#define	CONV_PLAIN	24
#define	CONV_DEDUP	8
#define	CONV_GANG	2

/*
 * Phase 0 of converge: many plain, dedup and gang objects to free in one
 * txg, a clone pair that is kept (it makes brt_maybe_exists() true for the
 * blocks around it, which sends their frees through the free list) and a
 * control.
 */
static int
child_setup_converge(void)
{
	objset_t *os;
	spa_t *spa;
	char name[24];
	int k;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	sh->sh_nfobj = 0;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	k = add_fobj("clone-K", 400, B_FALSE);
	sh->sh_fobj[k].fo_obj = put_object(os, 400);
	for (int i = 0; i < CONV_PLAIN; i++) {
		(void) snprintf(name, sizeof (name), "plain-%d", i);
		sh->sh_fobj[add_fobj(name, 100 + i, B_TRUE)].fo_obj =
		    put_object(os, 100 + i);
	}
	sh->sh_fobj[add_fobj("control-C", 401, B_FALSE)].fo_obj =
	    put_object(os, 401);
	txg_wait_synced(spa_get_dsl(spa), 0);
	sh->sh_fobj[add_fobj("clone-K2", 400, B_FALSE)].fo_obj =
	    clone_object(spa, os, sh->sh_fobj[k].fo_obj);
	dmu_objset_disown(os, B_TRUE, FTAG);

	set_dedup(ZIO_CHECKSUM_SHA256);
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < CONV_DEDUP; i++) {
		(void) snprintf(name, sizeof (name), "dedup-%d", i);
		sh->sh_fobj[add_fobj(name, 200 + i, B_TRUE)].fo_obj =
		    put_object(os, 200 + i);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	set_dedup(ZIO_CHECKSUM_OFF);

	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < CONV_GANG; i++) {
		(void) snprintf(name, sizeof (name), "gang-%d", i);
		sh->sh_fobj[add_fobj(name, 300 + i, B_TRUE)].fo_obj =
		    put_object(os, 300 + i);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
	print_fobjs(os);
	dmu_objset_disown(os, B_TRUE, FTAG);

	add_parity2_epoch(spa);
	export_pool(spa);
	kernel_fini();
	return (0);
}

/*
 * The objset id of snapshot s1, where its snapshot-only block lives, or 0
 * if there is none.  Not with a pool hold: this takes one.
 */
static uint64_t
snap_objset_id(void)
{
	objset_t *sos;
	uint64_t id;

	if (dmu_objset_hold(SNAP, FTAG, &sos) != 0)
		return (0);
	id = dmu_objset_id(sos);
	dmu_objset_rele(sos, FTAG);
	return (id);
}

/*
 * Name the blocks of a free-mode object, a gang header's children too,
 * with the bookmark of where each lives: s1, objset <snapid>, for the
 * snapshot-only block.
 */
static void
add_fobj_moves(spa_t *spa, objset_t *os, int f, uint64_t snapid)
{
	fobj_t *fo = &sh->sh_fobj[f];
	blkptr_t bp = fobj_bp(os, f);
	zbookmark_phys_t zb;
	char name[32];
	int first = sh->sh_nmoves;

	VERIFY(!fo->fo_snap || snapid != 0);
	SET_BOOKMARK(&zb, fo->fo_snap ? snapid : dmu_objset_id(os),
	    fo->fo_obj, 0, 0);
	if (!BP_IS_GANG(&bp)) {
		add_move(spa, fo->fo_name, &bp, &zb, BP_GET_PSIZE(&bp));
	} else {
		add_move(spa, fo->fo_name, &bp, &zb, gang_header_psize(spa));
		zio_gbh_phys_t *gbh = abd_to_buf(sh->sh_rdm[sh->sh_nmoves -
		    1].rdm_data);
		(void) snprintf(name, sizeof (name), "%s child", fo->fo_name);
		for (int k = 0; k < gbh_nblkptrs(gang_header_psize(spa));
		    k++) {
			blkptr_t *cbp = gbh_bp(gbh, k);

			if (BP_IS_HOLE(cbp))
				continue;
			VERIFY(!BP_IS_GANG(cbp));
			add_move(spa, name, cbp, &zb, BP_GET_PSIZE(cbp));
		}
	}
	for (int i = first; i < sh->sh_nmoves; i++)
		sh->sh_owner[i] = f;
}

static void
print_move(int i)
{
	const raidz_redirect_move_t *rdm = &sh->sh_rdm[i];
	const raidz_redirect_ent_t *e = &rdm->rdm_ent;

	(void) printf("INFO move %d %s: O=%llx t1=%llu asize %llu kept %llu "
	    "-> N=%llx asize %llu t2=%llu (error %d)\n", i, sh->sh_name[i],
	    (u_longlong_t)DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]),
	    (u_longlong_t)rdm->rdm_old_asize,
	    (u_longlong_t)e->rde_old_reserved,
	    (u_longlong_t)e->rde_new_offset, (u_longlong_t)e->rde_new_asize,
	    (u_longlong_t)e->rde_new_birth, rdm->rdm_error);
}

/* Move every block of the objects marked to move, and sync the move. */
static void
move_fobjs(spa_t *spa, policy_t policy)
{
	uint64_t snapid = snap_objset_id();
	objset_t *os;
	int err;

	VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
	sh->sh_nmoves = 0;
	for (int f = 0; f < sh->sh_nfobj; f++) {
		if (sh->sh_fobj[f].fo_moved)
			add_fobj_moves(spa, os, f, snapid);
	}
	dmu_objset_rele(os, FTAG);
	/* dsl_sync_task() returns once the move's txg has synced. */
	err = do_move(spa, policy, B_FALSE);
	(void) printf("INFO the move synced in txg %llu\n",
	    (u_longlong_t)spa_last_synced_txg(spa));
	check(err == 0, "the move of %d blocks under %s returns 0 (%d)",
	    sh->sh_nmoves, policy_names[policy], err);
	for (int i = 0; i < sh->sh_nmoves; i++) {
		print_move(i);
		check(sh->sh_rdm[i].rdm_error == 0 &&
		    sh->sh_rdm[i].rdm_ent.rde_new_asize != 0,
		    "%s moved", sh->sh_name[i]);
	}
}

/* The phase after setup: move, let the move's frees settle, export. */
static int
child_move_fobjs(void *arg)
{
	policy_t policy = *(policy_t *)arg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	move_fobjs(spa, policy);
	txg_wait_synced(spa_get_dsl(spa), spa_last_synced_txg(spa) +
	    TXG_DEFER_SIZE + 1);
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/*
 * Free some objects of the head in one tx; its txg.  A block that may be
 * cloned goes to the pool's free bpobj, which dsl_scan_sync() leaves alone
 * for zfs_import_defer_txgs (5, static) txgs after an import: free after
 * that, so that the blocks are freed in the txg that frees the objects.
 */
static uint64_t
free_fobjs(spa_t *spa, const int *f, int n)
{
	objset_t *os;
	dmu_tx_t *tx;
	uint64_t txg;

	txg_wait_synced(spa_get_dsl(spa), spa_first_txg(spa) + 5);
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	tx = dmu_tx_create(os);
	for (int i = 0; i < n; i++) {
		dmu_tx_hold_free(tx, sh->sh_fobj[f[i]].fo_obj, 0,
		    DMU_OBJECT_END);
	}
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	txg = dmu_tx_get_txg(tx);
	for (int i = 0; i < n; i++) {
		VERIFY0(dmu_object_free(os, sh->sh_fobj[f[i]].fo_obj, tx));
		sh->sh_fobj[f[i]].fo_gone = B_TRUE;
	}
	dmu_tx_commit(tx);
	dmu_objset_disown(os, B_TRUE, FTAG);
	return (txg);
}

/* Read a free-mode object, from snapshot s1 if it lives only there. */
static int
read_fobj(int f, boolean_t *match)
{
	fobj_t *fo = &sh->sh_fobj[f];
	uint64_t size = fo->fo_size != 0 ? fo->fo_size : BLKSZ;
	objset_t *os;
	uint64_t *buf = malloc(size), *want = malloc(size);
	int err;

	VERIFY0(dmu_objset_hold(fo->fo_snap ? SNAP : POOL, FTAG, &os));
	fill_pattern(want, size, fo->fo_seed);
	memset(buf, 0, size);
	err = dmu_read(os, fo->fo_obj, 0, size, buf, DMU_READ_NO_PREFETCH);
	*match = (err == 0 && memcmp(buf, want, size) == 0);
	dmu_objset_rele(os, FTAG);
	free(buf);
	free(want);
	return (err);
}

static void
check_others_read(void)
{
	for (int f = 0; f < sh->sh_nfobj; f++) {
		boolean_t match;
		int err;

		if (sh->sh_fobj[f].fo_gone)
			continue;
		err = read_fobj(f, &match);
		check(err == 0 && match, "%s reads back bit-exact (err=%d "
		    "match=%d)", sh->sh_fobj[f].fo_name, err, match);
	}
}

/* Bytes of [off, off + len) that rt holds. */
static uint64_t
tree_overlap(zfs_range_tree_t *rt, uint64_t off, uint64_t len)
{
	uint64_t end = off + len;
	uint64_t total = 0;
	uint64_t s;
	uint64_t l;

	while (off < end && zfs_range_tree_find_in(rt, off, end - off, &s,
	    &l)) {
		total += l;
		off = s + l;
	}
	return (total);
}

static metaslab_t *
ms_of(spa_t *spa, uint64_t off, uint64_t len)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];

	VERIFY3U(off >> tvd->vdev_ms_shift, ==,
	    (off + len - 1) >> tvd->vdev_ms_shift);
	return (tvd->vdev_ms[off >> tvd->vdev_ms_shift]);
}

/*
 * Bytes of [off, off + len) of vdev 0 that a writeable pool holds
 * allocated: in neither ms_allocatable nor any tree on the way to it.
 */
static uint64_t
live_allocated(spa_t *spa, uint64_t off, uint64_t len)
{
	metaslab_t *msp = ms_of(spa, off, len);
	uint64_t nfree;

	mutex_enter(&msp->ms_lock);
	VERIFY0(metaslab_load(msp));
	nfree = tree_overlap(msp->ms_allocatable, off, len) +
	    tree_overlap(msp->ms_freeing, off, len) +
	    tree_overlap(msp->ms_freed, off, len) +
	    tree_overlap(msp->ms_checkpointing, off, len);
	for (int t = 0; t < TXG_DEFER_SIZE; t++)
		nfree += tree_overlap(msp->ms_defer[t], off, len);
	mutex_exit(&msp->ms_lock);
	return (len - nfree);
}

static boolean_t
in_allocatable(spa_t *spa, uint64_t off, uint64_t len)
{
	metaslab_t *msp = ms_of(spa, off, len);
	boolean_t in;

	mutex_enter(&msp->ms_lock);
	VERIFY0(metaslab_load(msp));
	in = zfs_range_tree_contains(msp->ms_allocatable, off, len);
	mutex_exit(&msp->ms_lock);
	return (in);
}

/* What the trees of vdev 0 hold allocated, deferred frees included. */
static uint64_t
trees_allocated(spa_t *spa)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	uint64_t total = 0;

	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		metaslab_t *msp = tvd->vdev_ms[m];

		mutex_enter(&msp->ms_lock);
		VERIFY0(metaslab_load(msp));
		total += msp->ms_size -
		    zfs_range_tree_space(msp->ms_allocatable);
		mutex_exit(&msp->ms_lock);
	}
	return (total);
}

static uint64_t
vs_alloc(spa_t *spa)
{
	return (spa->spa_root_vdev->vdev_child[0]->vdev_stat.vs_alloc);
}

/*
 * vs_alloc and the trees' count, read with no txg allocating in between
 * (an import leaves a few txgs of its own work): at most 10 tries.
 */
static void
quiet_alloc(spa_t *spa, uint64_t *vs, uint64_t *trees)
{
	for (int i = 0; i < 10; i++) {
		uint64_t a;

		txg_wait_synced(spa_get_dsl(spa), 0);
		a = vs_alloc(spa);
		*trees = trees_allocated(spa);
		*vs = vs_alloc(spa);
		if (a == *vs && *vs == *trees)
			return;
	}
}

/* The extents a moved block holds: [O, O + kept) and N. */
static uint64_t
held_allocated(spa_t *spa, int i)
{
	const raidz_redirect_ent_t *e = &sh->sh_rdm[i].rdm_ent;
	uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
	uint64_t n = live_allocated(spa, e->rde_new_offset, e->rde_new_asize);

	if (e->rde_old_reserved != 0)
		n += live_allocated(spa, o, e->rde_old_reserved);
	return (n);
}

static int
zap_entry(spa_t *spa, int i)
{
	vdev_raidz_t *vdrz = spa->spa_root_vdev->vdev_child[0]->vdev_tsd;
	uint64_t key[2] = { DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]) };
	uint64_t val[4];

	if (vdrz->vd_redirect_obj == 0)
		return (ENOENT);
	return (zap_lookup_uint64(spa_meta_objset(spa), vdrz->vd_redirect_obj,
	    key, 2, sizeof (uint64_t), 4, val));
}

static boolean_t
mem_entry(spa_t *spa, int i)
{
	return (vdev_raidz_redirect_get(spa->spa_root_vdev->vdev_child[0],
	    DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]), NULL));
}

/* Whether move i belongs to an object in the set (by fobj index). */
static boolean_t
move_in(int i, const boolean_t *owners)
{
	return (sh->sh_owner[i] >= 0 && owners[sh->sh_owner[i]]);
}

/*
 * Dirty txg <txg> or a later one with a small write to a scratch object:
 * spa_sync() skips the deferred frees of a txg that changes nothing.
 */
static void
dirty_txg(spa_t *spa)
{
	static uint64_t churn;
	char buf[512];
	objset_t *os;
	dmu_tx_t *tx;

	(void) spa;
	memset(buf, 0x5a, sizeof (buf));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, churn == 0 ? DMU_NEW_OBJECT : churn, 0,
	    sizeof (buf));
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	if (churn == 0) {
		churn = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 0,
		    DMU_OT_NONE, 0, tx);
	}
	dmu_write(os, churn, 0, sizeof (buf), buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	dmu_objset_disown(os, B_TRUE, FTAG);
}

/*
 * Wait for the frees of the moves in <owners> to leave memory, from txg
 * <start> on, at most POLL_TXGS txgs, each one dirtied; the txg of the
 * last of them as the statistics give it (or the txg the poll saw it in),
 * 0 if they never did.
 */
static uint64_t
wait_conversions(spa_t *spa, const boolean_t *owners, uint64_t start)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	uint64_t seen = 0;

	for (uint64_t t = start; t <= start + POLL_TXGS; t++) {
		boolean_t any = B_FALSE;

		if (t > start)
			dirty_txg(spa);
		txg_wait_synced(dp, t);
		for (int i = 0; i < sh->sh_nmoves; i++) {
			if (move_in(i, owners) && mem_entry(spa, i))
				any = B_TRUE;
		}
		if (!any) {
			seen = t;
			break;
		}
	}
	if (seen == 0) {
		(void) printf("INFO the redirects never left memory within %d "
		    "txgs of txg %llu\n", POLL_TXGS, (u_longlong_t)start);
		return (0);
	}
	if (spike_redirect_stats == NULL)
		return (seen);
	uint64_t last = 0;
	for (uint64_t t = start; t <= seen; t++) {
		spike_redirect_stat_t *rst =
		    &spike_redirect_stats[t % SPIKE_REDIRECT_TXGS];

		for (int p = 0; rst->rst_txg == t &&
		    p < SPIKE_REDIRECT_PASSES; p++) {
			if (rst->rst_freed[p] != 0)
				last = t;
		}
	}
	(void) printf("INFO the redirects left memory by txg %llu; the last "
	    "free of a moved block was in txg %llu\n", (u_longlong_t)seen,
	    (u_longlong_t)last);
	return (last != 0 ? last : seen);
}

/* Print what the frees of moved blocks did in txgs [from, to]. */
static void
print_stats(uint64_t from, uint64_t to, uint64_t *freed, uint64_t *removed,
    boolean_t *same_txg)
{
	*freed = *removed = 0;
	*same_txg = B_TRUE;
	if (spike_redirect_stats == NULL) {
		(void) printf("INFO this libzpool keeps no statistics of frees "
		    "of moved blocks\n");
		return;
	}
	for (uint64_t t = from; t <= to; t++) {
		spike_redirect_stat_t *rst =
		    &spike_redirect_stats[t % SPIKE_REDIRECT_TXGS];
		uint64_t f = 0, r = 0;
		char line[256];
		int len;

		if (rst->rst_txg != t)
			continue;
		len = snprintf(line, sizeof (line), "INFO txg %llu: %llu sync "
		    "passes; frees by pass", (u_longlong_t)t,
		    (u_longlong_t)rst->rst_passes);
		for (int p = 0; p < SPIKE_REDIRECT_PASSES; p++) {
			len += snprintf(line + len, sizeof (line) - len,
			    " %llu", (u_longlong_t)rst->rst_freed[p]);
			f += rst->rst_freed[p];
		}
		len += snprintf(line + len, sizeof (line) - len,
		    "; removals by pass");
		for (int p = 0; p < SPIKE_REDIRECT_PASSES; p++) {
			len += snprintf(line + len, sizeof (line) - len,
			    " %llu", (u_longlong_t)rst->rst_removed[p]);
			r += rst->rst_removed[p];
		}
		(void) printf("%s\n", line);
		if (f != r)
			*same_txg = B_FALSE;
		*freed += f;
		*removed += r;
	}
}

/*
 * The checks after the frees of the moves in <owners> settled: each entry
 * gone from memory and from the table, [O, O + kept) and N back in
 * ms_allocatable, the bytes those extents held all freed, vs_alloc in step
 * with the trees, and every other object read back.
 */
static void
check_freed(spa_t *spa, const boolean_t *owners, const char *when,
    uint64_t vs0, uint64_t trees0, uint64_t held0)
{
	uint64_t held1 = 0, want = 0;
	uint64_t vs1, trees1;
	int remaining = 0;

	for (int i = 0; i < sh->sh_nmoves; i++) {
		const raidz_redirect_ent_t *e = &sh->sh_rdm[i].rdm_ent;
		uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
		int zerr;

		if (!move_in(i, owners)) {
			remaining++;
			continue;
		}
		zerr = zap_entry(spa, i);
		check(!mem_entry(spa, i) && zerr == ENOENT, "%s: %s: the "
		    "entry is gone from memory (%d) and the table (zap %d)",
		    when, sh->sh_name[i], mem_entry(spa, i), zerr);
		if (e->rde_old_reserved != 0) {
			check(in_allocatable(spa, o, e->rde_old_reserved),
			    "%s: %s: kept [%llx, +%llu) is in ms_allocatable",
			    when, sh->sh_name[i], (u_longlong_t)o,
			    (u_longlong_t)e->rde_old_reserved);
		}
		check(in_allocatable(spa, e->rde_new_offset, e->rde_new_asize),
		    "%s: %s: N [%llx, +%llu) is in ms_allocatable", when,
		    sh->sh_name[i], (u_longlong_t)e->rde_new_offset,
		    (u_longlong_t)e->rde_new_asize);
		held1 += held_allocated(spa, i);
		want += e->rde_old_reserved + e->rde_new_asize;
	}
	if (vdev_raidz_redirect_count != NULL) {
		uint64_t n = vdev_raidz_redirect_count(spa, 0);

		check(n == remaining, "%s: %llu redirects left (expected %d)",
		    when, (u_longlong_t)n, remaining);
	}
	quiet_alloc(spa, &vs1, &trees1);
	check(held0 - held1 == want, "%s: the moved extents released %llu "
	    "bytes, old_reserved + new_asize summed over the freed moved DVAs "
	    "is %llu", when, (u_longlong_t)(held0 - held1),
	    (u_longlong_t)want);
	check(vs0 == trees0 && vs1 == trees1, "%s: vs_alloc matches the "
	    "trees before (%llu, %llu) and after (%llu, %llu)", when,
	    (u_longlong_t)vs0, (u_longlong_t)trees0, (u_longlong_t)vs1,
	    (u_longlong_t)trees1);
	(void) printf("INFO %s: vs_alloc dropped by %lld: %llu in the moved "
	    "extents, %lld elsewhere (metadata)\n", when,
	    (longlong_t)(vs0 - vs1), (u_longlong_t)(held0 - held1),
	    (longlong_t)((vs0 - vs1) - (held0 - held1)));
}

typedef struct free_args {
	const char	*fa_case;
	policy_t	fa_policy;
	boolean_t	fa_nocheck;	/* spike_redirect_check_enable = 0 */
} free_args_t;

/*
 * The free test of one case.  The first free of a dedup or clone pair must
 * change nothing; the free that drops the last reference converts.  Under
 * P3 the move happens in this child, right before the free, so that the
 * old extent is still in a defer tree when the free checks it.
 */
static int
child_free(void *arg)
{
	free_args_t *fa = arg;
	boolean_t owners[MAXOBJS] = { 0 };
	int first = -1, last;
	uint64_t vs0, trees0, held0 = 0, start, f_txg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	if (fa->fa_policy == P3) {
		/* Free right after the move: past free_fobjs()'s wait. */
		txg_wait_synced(spa_get_dsl(spa), spa_first_txg(spa) + 5);
		move_fobjs(spa, P3);
	}
	if (fa->fa_nocheck) {
		VERIFY3P(&spike_redirect_check_enable, !=, NULL);
		spike_redirect_check_enable = 0;
		(void) printf("INFO metaslab_check_free() checks the old "
		    "extents of moved blocks (spike_redirect_check_enable=0)"
		    "\n");
	}

	if (strcmp(fa->fa_case, "plain") == 0) {
		last = find_fobj("plain-A");
	} else if (strcmp(fa->fa_case, "dedup") == 0) {
		first = find_fobj("dedup-D1");
		last = find_fobj("dedup-D2");
	} else if (strcmp(fa->fa_case, "clone") == 0) {
		first = find_fobj("clone-K");
		last = find_fobj("clone-K2");
	} else if (strcmp(fa->fa_case, "gang") == 0) {
		last = find_fobj("gang-G");
	} else {
		last = find_fobj("snap-S");
	}
	owners[sh->sh_fobj[last].fo_shares >= 0 ?
	    sh->sh_fobj[last].fo_shares : last] = B_TRUE;

	if (fa->fa_policy != P3) {
		quiet_alloc(spa, &vs0, &trees0);
	} else {
		/* No txg may pass: the old extent is still deferred. */
		vs0 = vs_alloc(spa);
		trees0 = trees_allocated(spa);
	}
	for (int i = 0; i < sh->sh_nmoves; i++) {
		if (!move_in(i, owners))
			continue;
		check(mem_entry(spa, i) && zap_entry(spa, i) == 0, "before: "
		    "%s has its entry in memory and in the table",
		    sh->sh_name[i]);
		held0 += held_allocated(spa, i);
		if (fa->fa_policy == P3) {
			uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
			uint64_t a = sh->sh_rdm[i].rdm_old_asize;

			check(live_allocated(spa, o, a) == 0, "before: %s: "
			    "the old extent [%llx, +%llu) is wholly free",
			    sh->sh_name[i], (u_longlong_t)o, (u_longlong_t)a);
		}
	}
	(void) printf("INFO before: vs_alloc %llu, trees %llu, the moved "
	    "extents to free hold %llu bytes\n", (u_longlong_t)vs0,
	    (u_longlong_t)trees0, (u_longlong_t)held0);

	if (first != -1) {
		uint64_t t = free_fobjs(spa, &first, 1);
		uint64_t held = 0;

		(void) printf("INFO freed %s in txg %llu\n",
		    sh->sh_fobj[first].fo_name, (u_longlong_t)t);
		txg_wait_synced(spa_get_dsl(spa), t + 8);
		for (int i = 0; i < sh->sh_nmoves; i++) {
			if (!move_in(i, owners))
				continue;
			check(mem_entry(spa, i) && zap_entry(spa, i) == 0,
			    "after the first free: %s keeps its entry",
			    sh->sh_name[i]);
			held += held_allocated(spa, i);
		}
		check(held == held0, "after the first free: the moved extents "
		    "still hold %llu bytes (%llu before)", (u_longlong_t)held,
		    (u_longlong_t)held0);
	}

	if (strcmp(fa->fa_case, "snap") == 0) {
		start = spa_last_synced_txg(spa) + 1;
		VERIFY0(dsl_destroy_snapshot(SNAP, B_FALSE));
		sh->sh_fobj[last].fo_gone = B_TRUE;
		(void) printf("INFO destroyed %s from txg %llu on\n", SNAP,
		    (u_longlong_t)start);
	} else {
		start = free_fobjs(spa, &last, 1);
		(void) printf("INFO freed %s in txg %llu\n",
		    sh->sh_fobj[last].fo_name, (u_longlong_t)start);
	}
	f_txg = wait_conversions(spa, owners, start);
	if (f_txg == 0)
		f_txg = start;
	txg_wait_synced(spa_get_dsl(spa), f_txg + TXG_DEFER_SIZE);
	(void) printf("INFO synced txg %llu, TXG_DEFER_SIZE (%d) txgs after "
	    "%llu\n", (u_longlong_t)(f_txg + TXG_DEFER_SIZE), TXG_DEFER_SIZE,
	    (u_longlong_t)f_txg);
	{
		uint64_t fr, rm;
		boolean_t same;

		print_stats(start, f_txg, &fr, &rm, &same);
	}
	check_freed(spa, owners, "in-process", vs0, trees0, held0);

	export_pool(spa);
	spa = import_pool();
	check_freed(spa, owners, "after export and import", vs0, trees0,
	    held0);
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/*
 * Converge: free every moved object in one tx.  Plain blocks are freed in
 * pass 1; gang and dedup blocks, whose frees go through the free list,
 * the DDT or (without the log space map) the deferred bpobj of the next
 * txg, in later passes.  spa_sync() must converge, every key must leave
 * the table in the txg of its free, and nothing may leak.
 */
static int
child_converge(void *arg)
{
	boolean_t owners[MAXOBJS] = { 0 };
	int f[MAXOBJS], n = 0;
	uint64_t vs0, trees0, held0 = 0, start, last, fr, rm;
	boolean_t same;
	spa_t *spa;
	objset_t *os;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	(void) printf("INFO log space map %s, fast dedup %s\n",
	    spa_feature_is_active(spa, SPA_FEATURE_LOG_SPACEMAP) ?
	    "active" : "inactive",
	    spa_feature_is_enabled(spa, SPA_FEATURE_FAST_DEDUP) ?
	    "enabled" : "disabled");
	VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
	for (int i = 0; i < sh->sh_nfobj; i++) {
		if (!sh->sh_fobj[i].fo_moved)
			continue;
		f[n++] = i;
		owners[i] = B_TRUE;
		if (strncmp(sh->sh_fobj[i].fo_name, "dedup-0", 7) == 0) {
			blkptr_t bp = object_bp(os, sh->sh_fobj[i].fo_obj);

			(void) printf("INFO brt_maybe_exists(%s) = %d\n",
			    sh->sh_fobj[i].fo_name,
			    brt_maybe_exists(spa, &bp));
		}
	}
	dmu_objset_rele(os, FTAG);
	quiet_alloc(spa, &vs0, &trees0);
	for (int i = 0; i < sh->sh_nmoves; i++)
		held0 += held_allocated(spa, i);

	start = free_fobjs(spa, f, n);
	(void) printf("INFO freed %d objects, %d moved blocks, in txg %llu\n",
	    n, sh->sh_nmoves, (u_longlong_t)start);
	last = wait_conversions(spa, owners, start);
	check(last != 0, "every moved block was freed within %d txgs",
	    POLL_TXGS);
	if (last == 0)
		last = start;
	txg_wait_synced(spa_get_dsl(spa), last + TXG_DEFER_SIZE);
	print_stats(start, last + 1, &fr, &rm, &same);
	if (spike_redirect_stats != NULL) {
		check(fr == sh->sh_nmoves && rm == sh->sh_nmoves, "%llu frees "
		    "of moved blocks and %llu table removals for %d moved "
		    "blocks", (u_longlong_t)fr, (u_longlong_t)rm,
		    sh->sh_nmoves);
		check(same, "each txg removed from the table exactly the keys "
		    "of the moved blocks it freed");
	}
	check_freed(spa, owners, "in-process", vs0, trees0, held0);

	export_pool(spa);
	spa = import_pool();
	check_freed(spa, owners, "after export and import", vs0, trees0,
	    held0);
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

typedef struct fcrash_args {
	int		fc_point;
	boolean_t	fc_late;
} fcrash_args_t;

/* Child A of fcrash: free plain-A with the crash armed for its txg. */
static int
child_fcrash(void *arg)
{
	fcrash_args_t *fc = arg;
	int a = find_fobj("plain-A");
	uint64_t txg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	if (&spike_crash_arm_free != NULL) {
		spike_crash_point = fc->fc_point;
		spike_crash_arm_free = 1;
		if (fc->fc_late)
			spike_redirect_late_remove = 1;
	} else if (fc->fc_point != SPIKE_CRASH_POST_UB || fc->fc_late) {
		(void) printf("SKIP %s cannot be emulated without the free "
		    "of moved blocks\n", point_names[fc->fc_point]);
		return (2);
	}
	txg = free_fobjs(spa, &a, 1);
	sh->sh_free_txg = txg;
	(void) printf("INFO freeing plain-A in txg %llu, crash at %s%s\n",
	    (u_longlong_t)txg, point_names[fc->fc_point],
	    fc->fc_late ? ", table updated a txg late" : "");
	txg_wait_synced(spa_get_dsl(spa), txg);
	if (&spike_crash_arm_free == NULL) {
		(void) printf("INFO the free's txg has synced; exiting as a "
		    "crash after its uberblock would\n");
		_exit(99);
	}
	txg_wait_synced(spa_get_dsl(spa), txg + TXG_DEFER_SIZE + 1);
	(void) printf("FAIL the free returned without reaching %s\n",
	    point_names[fc->fc_point]);
	return (1);
}

/* Child B of fcrash: a fresh read-only import of the crashed pool. */
static int
child_fverify(void *arg)
{
	boolean_t committed = *(boolean_t *)arg;
	int a = find_fobj("plain-A");
	uint64_t f_txg = sh->sh_free_txg;
	uint64_t hits;
	objset_t *os;
	spa_t *spa;
	int err, st;

	spa_mode_readable_spacemaps = B_TRUE;
	kernel_init(SPA_MODE_READ);
	spa = import_pool();
	VERIFY(!spa_writeable(spa));
	(void) printf("INFO read-only import at txg %llu; expecting the "
	    "state %s the free\n", (u_longlong_t)spa_last_synced_txg(spa),
	    committed ? "after" : "before");
	check(committed ? spa_last_synced_txg(spa) >= f_txg :
	    spa_last_synced_txg(spa) < f_txg, "B the newest uberblock, txg "
	    "%llu, is %s the free's txg %llu",
	    (u_longlong_t)spa_last_synced_txg(spa),
	    committed ? "at or after" : "before", (u_longlong_t)f_txg);

	VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
	err = dmu_object_info(os, sh->sh_fobj[a].fo_obj, NULL);
	dmu_objset_rele(os, FTAG);
	check(committed ? err == ENOENT : err == 0, "B plain-A %s (%d)",
	    committed ? "is gone" : "still exists", err);

	if (vdev_raidz_redirect_count != NULL) {
		int nmine = 0;
		uint64_t n = vdev_raidz_redirect_count(spa, 0);

		for (int i = 0; i < sh->sh_nmoves; i++)
			nmine += (sh->sh_owner[i] == a);
		check(n == sh->sh_nmoves - (committed ? nmine : 0), "B %llu "
		    "redirects loaded (expected %d)", (u_longlong_t)n,
		    sh->sh_nmoves - (committed ? nmine : 0));
	}
	build_free_trees(spa);
	for (int i = 0; i < sh->sh_nmoves; i++) {
		const raidz_redirect_ent_t *e = &sh->sh_rdm[i].rdm_ent;
		uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
		int want = committed ? 1 : 0;

		if (sh->sh_owner[i] != a)
			continue;
		check(mem_entry(spa, i) == !committed, "B %s: entry %s",
		    sh->sh_name[i], committed ? "gone" : "present");
		if (e->rde_old_reserved != 0) {
			st = range_state(spa, o, e->rde_old_reserved);
			check(st == want, "B %s: kept [%llx, +%llu) %s",
			    sh->sh_name[i], (u_longlong_t)o,
			    (u_longlong_t)e->rde_old_reserved, state_name(st));
		}
		st = range_state(spa, e->rde_new_offset, e->rde_new_asize);
		check(st == want, "B %s: N [%llx, +%llu) %s", sh->sh_name[i],
		    (u_longlong_t)e->rde_new_offset,
		    (u_longlong_t)e->rde_new_asize, state_name(st));
	}
	if (!committed) {
		boolean_t match;

		hits = vdev_raidz_redirect_hits();
		err = read_fobj(a, &match);
		hits = vdev_raidz_redirect_hits() - hits;
		check(err == 0 && match && hits == 1, "B plain-A reads back "
		    "bit-exact through its redirect (err=%d match=%d "
		    "hits=%llu)", err, match, (u_longlong_t)hits);
	}
	sh->sh_fobj[a].fo_gone = B_TRUE;
	check_others_read();
	spa_close(spa, FTAG);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

static int
setup_free_fn(void *arg)
{
	(void) arg;
	return (child_setup_free());
}

static int
setup_converge_fn(void *arg)
{
	(void) arg;
	return (child_setup_converge());
}

static int
run_free(const char *which, policy_t policy, boolean_t nocheck)
{
	free_args_t fa = { which, policy, nocheck };
	int st;

	(void) printf("INFO free %s, policy %s, ashift %llu%s\n", which,
	    policy_names[policy], (u_longlong_t)ashift,
	    nocheck ? ", nocheck" : "");
	VERIFY0(phase("0 setup", setup_free_fn, NULL));
	if (policy != P3) {
		st = phase("1 move", child_move_fobjs, &policy);
		check(st == 0, "the move phase passes (status %d)", st);
	}
	st = phase("2 free", child_free, &fa);
	check(st == 0, "the free phase passes (status %d)", st);
	run_zdb();
	return (0);
}

static int
run_fcrash(int point, policy_t policy, boolean_t late)
{
	fcrash_args_t fc = { point, late };
	boolean_t committed = (point == SPIKE_CRASH_POST_UB);
	int st;

	(void) printf("INFO fcrash, crash at %s, policy %s, ashift %llu%s\n",
	    point_names[point], policy_names[policy], (u_longlong_t)ashift,
	    late ? ", late table removal" : "");
	VERIFY0(phase("0 setup", setup_free_fn, NULL));
	st = phase("1 move", child_move_fobjs, &policy);
	check(st == 0, "the move phase passes (status %d)", st);
	st = phase("A free", child_fcrash, &fc);
	if (st == 2)
		return (2);
	check(st == 99, "A exits at the crash point (status %d)", st);
	st = phase("B verify", child_fverify, &committed);
	check(st == 0, "B finds the state %s the free (status %d)",
	    committed ? "after" : "before", st);
	run_zdb();
	return (0);
}

static int
run_converge(policy_t policy, boolean_t nolog)
{
	int st;

	(void) printf("INFO converge, policy %s, ashift %llu, %s\n",
	    policy_names[policy], (u_longlong_t)ashift,
	    nolog ? "no log space map" : "log space map");
	skip_features[0] = "fast_dedup";
	skip_features[1] = nolog ? "log_spacemap" : NULL;
	VERIFY0(phase("0 setup", setup_converge_fn, NULL));
	st = phase("1 move", child_move_fobjs, &policy);
	check(st == 0, "the move phase passes (status %d)", st);
	st = phase("2 free", child_converge, NULL);
	check(st == 0, "the free phase passes (status %d)", st);
	run_zdb();
	return (0);
}

/*
 * The resilver mode.
 */
#define	RS_CHILD	2
#define	RS_TXGS		400

typedef struct rs_args {
	policy_t	ra_policy;
	const char	*ra_how;	/* offline, control, replace... */
	int		ra_fix;		/* spike_resilver_redirects, or -1 */
} rs_args_t;

/* A child's file: the replacement, once the test replaced that child. */
static void
child_file(char *buf, size_t len, int child)
{
	if (sh->sh_replaced == child + 1)
		(void) snprintf(buf, len, "%s/n%d", dir, child);
	else
		leaf_path(buf, len, child);
}

static int
find_move(const char *name)
{
	for (int i = 0; i < sh->sh_nmoves; i++) {
		if (strcmp(sh->sh_name[i], name) == 0)
			return (i);
	}
	VERIFY(B_FALSE);
	return (-1);
}

/* The one-row raidz map of <psize> bytes at <offset> with <nparity>. */
static raidz_map_t *
rs_map(zio_t *zio, uint64_t offset, uint64_t psize, uint64_t nparity)
{
	raidz_map_t *rm;

	memset(zio, 0, sizeof (*zio));
	zio->io_size = P2ROUNDUP_TYPED(psize, 1ULL << ashift, uint64_t);
	zio->io_offset = offset;
	zio->io_type = ZIO_TYPE_READ;
	zio->io_abd = abd_alloc_for_io(zio->io_size, B_FALSE);
	rm = vdev_raidz_map_alloc(zio, ashift, WIDTH, nparity);
	VERIFY3S(rm->rm_nrows, ==, 1);
	return (rm);
}

static void
rs_map_free(zio_t *zio, raidz_map_t *rm)
{
	vdev_raidz_map_free(rm);
	abd_free(zio->io_abd);
}

/* Read or write a column where its child's file holds it. */
static void
rs_col_io(const raidz_col_t *rc, void *buf, boolean_t write)
{
	char path[PATH_BUF];
	ssize_t n;
	int fd;

	child_file(path, sizeof (path), rc->rc_devidx);
	fd = open(path, write ? O_RDWR : O_RDONLY);
	VERIFY3S(fd, !=, -1);
	if (write) {
		n = pwrite(fd, buf, rc->rc_size,
		    rc->rc_offset + VDEV_LABEL_START_SIZE);
	} else {
		n = pread(fd, buf, rc->rc_size,
		    rc->rc_offset + VDEV_LABEL_START_SIZE);
	}
	VERIFY3S(n, ==, rc->rc_size);
	(void) close(fd);
}

static boolean_t
rs_col_zero(const raidz_col_t *rc)
{
	uint8_t *buf = malloc(rc->rc_size);
	boolean_t zero = B_TRUE;

	rs_col_io(rc, buf, B_FALSE);
	for (uint64_t b = 0; b < rc->rc_size && zero; b++)
		zero = (buf[b] == 0);
	free(buf);
	return (zero);
}

/*
 * A child that holds a column of move i's new extent (parity 2) and none
 * of its old extent (parity 1), or -1.
 */
static int
rs_new_only_child(int i)
{
	zio_t zo, zn;
	raidz_map_t *ro = rs_map(&zo, DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    sh->sh_rdm[i].rdm_psize, 1);
	raidz_map_t *rn = rs_map(&zn, sh->sh_rdm[i].rdm_ent.rde_new_offset,
	    sh->sh_rdm[i].rdm_psize, 2);
	boolean_t old[WIDTH] = { B_FALSE };
	int child = -1;

	for (int c = 0; c < ro->rm_row[0]->rr_cols; c++) {
		if (ro->rm_row[0]->rr_col[c].rc_size != 0)
			old[ro->rm_row[0]->rr_col[c].rc_devidx] = B_TRUE;
	}
	for (int c = 0; c < rn->rm_row[0]->rr_cols && child == -1; c++) {
		const raidz_col_t *rc = &rn->rm_row[0]->rr_col[c];

		if (rc->rc_size != 0 && !old[rc->rc_devidx])
			child = rc->rc_devidx;
	}
	(void) printf("INFO %s: old extent on %d children, new extent on "
	    "%d\n", sh->sh_name[i], (int)ro->rm_row[0]->rr_cols,
	    (int)rn->rm_row[0]->rr_cols);
	rs_map_free(&zo, ro);
	rs_map_free(&zn, rn);
	return (child);
}

static boolean_t
rs_offline_done(spa_t *spa)
{
	return (vdev_dtl_empty(spa->spa_root_vdev->vdev_child[0]->
	    vdev_child[sh->sh_watch], DTL_MISSING));
}

/* The replacement is the child, the old one gone, and its DTL empty. */
static boolean_t
rs_replace_done(spa_t *spa)
{
	char path[PATH_BUF];
	vdev_t *cvd;
	boolean_t done;

	(void) snprintf(path, sizeof (path), "%s/n%d", dir, sh->sh_watch);
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	cvd = spa->spa_root_vdev->vdev_child[0]->vdev_child[sh->sh_watch];
	done = cvd->vdev_ops->vdev_op_leaf && cvd->vdev_path != NULL &&
	    strcmp(cvd->vdev_path, path) == 0 &&
	    vdev_dtl_empty(cvd, DTL_MISSING);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	return (done);
}

/* Wait, at most RS_TXGS txgs, for no resilver to be left and done(). */
static void
rs_wait(spa_t *spa, boolean_t (*done)(spa_t *), pool_scan_stat_t *ps)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	int n;

	for (n = 0; n < RS_TXGS; n++) {
		txg_wait_synced(dp, 0);
		if (n > 3 && !dsl_scan_resilvering(dp) &&
		    !dsl_scan_resilver_scheduled(dp) && done(spa))
			break;
		if (n % 8 == 7)
			dirty_txg(spa);
	}
	VERIFY0(spa_scan_get_stats(spa, ps));
	(void) printf("INFO after %d txgs: scan func %llu state %llu min txg "
	    "%llu max txg %llu examined %llu issued %llu errors %llu\n", n,
	    (u_longlong_t)ps->pss_func, (u_longlong_t)ps->pss_state,
	    (u_longlong_t)spa_get_dsl(spa)->dp_scan->scn_phys.scn_min_txg,
	    (u_longlong_t)spa_get_dsl(spa)->dp_scan->scn_phys.scn_max_txg,
	    (u_longlong_t)ps->pss_examined, (u_longlong_t)ps->pss_issued,
	    (u_longlong_t)ps->pss_errors);
}

/* Replace child r with a new file of the same size. */
static int
rs_replace(spa_t *spa, int r)
{
	char path[PATH_BUF];
	nvlist_t *file, *root;
	uint64_t guid;
	int fd, err;

	(void) snprintf(path, sizeof (path), "%s/n%d", dir, r);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, !=, -1);
	VERIFY0(ftruncate(fd, FILE_SIZE));
	(void) close(fd);
	file = fnvlist_alloc();
	fnvlist_add_string(file, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(file, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(file, ZPOOL_CONFIG_ASHIFT, ashift);
	root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&file, 1);
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	guid = spa->spa_root_vdev->vdev_child[0]->vdev_child[r]->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	err = spa_vdev_attach(spa, guid, root, B_TRUE, B_FALSE);
	fnvlist_free(root);
	fnvlist_free(file);
	return (err);
}

static void
rs_set_fix(int fix)
{
	if (fix < 0)
		return;
	if (&spike_resilver_redirects == NULL) {
		(void) printf("INFO this libzpool has no "
		    "spike_resilver_redirects\n");
		return;
	}
	spike_resilver_redirects = fix;
	(void) printf("INFO spike_resilver_redirects=%d\n", fix);
}

/* Phase 1: offline or not, move, then resilver, scrub or replace. */
static int
child_rs_move(void *arg)
{
	rs_args_t *ra = arg;
	boolean_t offline = (strncmp(ra->ra_how, "offline", 7) == 0);
	boolean_t replace = (strcmp(ra->ra_how, "replace") == 0);
	pool_scan_stat_t ps;
	vdev_t *tvd;
	spa_t *spa;
	int r = -1;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	rs_set_fix(ra->ra_fix);
	spa = import_pool();
	tvd = spa->spa_root_vdev->vdev_child[0];
	sh->sh_watch = RS_CHILD;
	if (offline) {
		VERIFY0(vdev_offline(spa, tvd->vdev_child[RS_CHILD]->vdev_guid,
		    0));
		(void) printf("INFO child %d offline (state %d)\n", RS_CHILD,
		    (int)tvd->vdev_child[RS_CHILD]->vdev_state);
	}
	move_fobjs(spa, ra->ra_policy);
	for (int k = 0; k < 3; k++) {
		dirty_txg(spa);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	if (offline) {
		vdev_t *cvd = tvd->vdev_child[RS_CHILD];
		uint64_t t2 = sh->sh_rdm[0].rdm_ent.rde_new_birth;
		vdev_state_t ns;

		(void) printf("INFO child %d DTL_MISSING holds the move's txg "
		    "%llu: %d\n", RS_CHILD, (u_longlong_t)t2,
		    vdev_dtl_contains(cvd, DTL_MISSING, t2, 1));
		VERIFY0(vdev_online(spa, cvd->vdev_guid, 0, &ns));
		(void) printf("INFO child %d online (state %d)\n", RS_CHILD,
		    (int)ns);
		rs_wait(spa, rs_offline_done, &ps);
		check(ps.pss_func == POOL_SCAN_RESILVER &&
		    ps.pss_state == DSS_FINISHED && ps.pss_errors == 0 &&
		    rs_offline_done(spa), "the resilver of child %d finished "
		    "with no error and cleared its DTL", RS_CHILD);
		if (strcmp(ra->ra_how, "offlinescrub") == 0) {
			VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
			while (dsl_scan_scrubbing(spa_get_dsl(spa)))
				txg_wait_synced(spa_get_dsl(spa), 0);
			VERIFY0(spa_scan_get_stats(spa, &ps));
			(void) printf("INFO scrub after the resilver: state "
			    "%llu examined %llu errors %llu\n",
			    (u_longlong_t)ps.pss_state,
			    (u_longlong_t)ps.pss_examined,
			    (u_longlong_t)ps.pss_errors);
		}
	} else if (replace) {
		int err;

		r = rs_new_only_child(find_move("small-Z"));
		check(r != -1, "a child holds a column of small-Z's new extent "
		    "and none of its old one (child %d)", r);
		if (r == -1) {
			export_pool(spa);
			kernel_fini();
			return (1);
		}
		sh->sh_watch = r;
		err = rs_replace(spa, r);
		check(err == 0, "child %d is being replaced (%d)", r, err);
		rs_wait(spa, rs_replace_done, &ps);
		check(ps.pss_func == POOL_SCAN_RESILVER &&
		    ps.pss_state == DSS_FINISHED && ps.pss_errors == 0 &&
		    rs_replace_done(spa), "the replacement of child %d "
		    "finished with no error and the old file was detached", r);
	}
	export_pool(spa);
	kernel_fini();
	if (r != -1) {
		char path[PATH_BUF];

		/* The detached file still has labels: keep it from import. */
		leaf_path(path, sizeof (path), r);
		VERIFY0(unlink(path));
		sh->sh_replaced = r + 1;
	}
	return (failures == 0 ? 0 : 1);
}

/*
 * Phase 2: look at each new extent's column on the watched child, then
 * damage two other columns of every new extent.
 */
static int
child_rs_damage(void *arg)
{
	const char *target = arg;
	int t = find_move(target);
	int zeros = -1;

	kernel_init(SPA_MODE_READ);
	for (int i = 0; i < sh->sh_nmoves; i++) {
		zio_t zio;
		raidz_map_t *rm = rs_map(&zio,
		    sh->sh_rdm[i].rdm_ent.rde_new_offset,
		    sh->sh_rdm[i].rdm_psize, 2);
		raidz_row_t *rr = rm->rm_row[0];
		int done = 0;

		for (int c = 0; c < rr->rr_cols; c++) {
			const raidz_col_t *rc = &rr->rr_col[c];
			boolean_t zero;

			if (rc->rc_devidx != sh->sh_watch || rc->rc_size == 0)
				continue;
			zero = rs_col_zero(rc);
			(void) printf("INFO %s: new extent column %d (%s) on "
			    "child %d, %llu bytes, all zero %d\n",
			    sh->sh_name[i], c, c < rr->rr_firstdatacol ?
			    "parity" : "data", sh->sh_watch,
			    (u_longlong_t)rc->rc_size, zero);
			if (i == t)
				zeros = zero;
		}
		/* Data columns first, then parity. */
		for (int pass = 0; pass < 2; pass++) {
			for (int c = 0; c < rr->rr_cols && done < 2; c++) {
				const raidz_col_t *rc = &rr->rr_col[c];
				uint8_t *buf;

				if (rc->rc_devidx == sh->sh_watch ||
				    rc->rc_size == 0 ||
				    (c >= rr->rr_firstdatacol) != (pass == 0))
					continue;
				buf = malloc(rc->rc_size);
				rs_col_io(rc, buf, B_FALSE);
				for (uint64_t b = 0; b < rc->rc_size; b++)
					buf[b] = ~buf[b];
				rs_col_io(rc, buf, B_TRUE);
				free(buf);
				done++;
			}
		}
		VERIFY3S(done, ==, 2);
		rs_map_free(&zio, rm);
	}
	check(zeros == 0, "%s's new extent has a column on child %d that is "
	    "not all zero (all zero %d)", target, sh->sh_watch, zeros);
	(void) printf("INFO damaged two columns, not on child %d, of each of "
	    "the %d new extents\n", sh->sh_watch, sh->sh_nmoves);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* Phase 3: every object must read back, its new extent damaged. */
static int
child_rs_read(void *arg)
{
	const char *target = arg;
	boolean_t match;
	spa_t *spa;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	err = read_fobj(find_fobj(target), &match);
	check(err == 0 && match, "%s reads back with two damaged columns of "
	    "its parity-2 new extent (err=%d match=%d)", target, err, match);
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

static int
run_resilver(policy_t policy, const char *how, int fix)
{
	rs_args_t ra = { policy, how, fix };
	const char *target = strcmp(how, "replace") == 0 ? "small-Z" :
	    "plain-A";
	int st;

	(void) printf("INFO resilver %s, policy %s, ashift %llu%s\n", how,
	    policy_names[policy], (u_longlong_t)ashift,
	    fix == 0 ? ", nofix" : fix == 1 ? ", nocols" :
	    fix == 2 ? ", nowiden" : "");
	sh->sh_small = B_TRUE;
	VERIFY0(phase("0 setup", setup_free_fn, NULL));
	st = phase("1 move and resilver", child_rs_move, &ra);
	check(st == 0, "the move and resilver phase passes (status %d)", st);
	run_zdb();
	st = phase("2 damage", child_rs_damage, (void *)target);
	check(st == 0, "the damage phase passes (status %d)", st);
	st = phase("3 read", child_rs_read, (void *)target);
	check(st == 0, "the read phase passes (status %d)", st);
	return (0);
}

/*
 * The stalebp mode.
 */
#define	STALE_FILL	64

typedef struct stale_args {
	policy_t	sa_policy;
	boolean_t	sa_same;
	boolean_t	sa_nolive;
} stale_args_t;

static void
probe_tunable(const char *s)
{
	VERIFY0(handle_tunable_option(s, B_TRUE));
}

/*
 * Phase 0: E at parity 1, freed; its old extent refilled, best fit, by
 * live objects (with E's content under 'same'); then the parity-2 epoch.
 */
static int
child_setup_stale(void *arg)
{
	const stale_args_t *sa = arg;
	objset_t *os;
	spa_t *spa;
	uint64_t e, o, a;
	dmu_tx_t *tx;
	int nfill = 0;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	probe_tunable("spa_num_allocators=1");
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	sh->sh_nfobj = 0;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	sh->sh_fobj[add_fobj("control-C", 14, B_FALSE)].fo_obj =
	    put_object(os, 14);
	e = put_object(os, 99);
	sh->sh_fobj[add_fobj("after-E", 15, B_FALSE)].fo_obj =
	    put_object(os, 15);
	txg_wait_synced(spa_get_dsl(spa), 0);
	sh->sh_stale_bp = object_bp(os, e);
	SET_BOOKMARK(&sh->sh_stale_zb, dmu_objset_id(os), e, 0, 0);
	o = DVA_GET_OFFSET(&sh->sh_stale_bp.blk_dva[0]);
	a = DVA_GET_ASIZE(&sh->sh_stale_bp.blk_dva[0]);
	(void) printf("INFO E: object %llu O=%llx asize %llu birth %llu\n",
	    (u_longlong_t)e, (u_longlong_t)o, (u_longlong_t)a,
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&sh->sh_stale_bp));
	tx = dmu_tx_create(os);
	dmu_tx_hold_free(tx, e, 0, DMU_OBJECT_END);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY0(dmu_object_free(os, e, tx));
	dmu_tx_commit(tx);
	for (int k = 0; k < 4; k++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	probe_tunable("metaslab_df_free_pct=100");
	while (nfill < STALE_FILL && live_allocated(spa, o, a) != a) {
		uint64_t seed = sa->sa_same ? 99 : 500 + nfill;
		char name[24];
		int f;

		(void) snprintf(name, sizeof (name), "fill-%d", nfill);
		f = add_fobj(name, seed, B_FALSE);
		sh->sh_fobj[f].fo_obj = put_object(os, seed);
		nfill++;
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	probe_tunable("metaslab_df_free_pct=4");
	(void) printf("INFO wrote %d fill objects; E's old extent live %llu "
	    "of %llu\n", nfill, (u_longlong_t)live_allocated(spa, o, a),
	    (u_longlong_t)a);
	for (int f = 0; f < sh->sh_nfobj; f++) {
		blkptr_t b = object_bp(os, sh->sh_fobj[f].fo_obj);
		uint64_t bo = DVA_GET_OFFSET(&b.blk_dva[0]);
		uint64_t ba = DVA_GET_ASIZE(&b.blk_dva[0]);

		if (bo < o + a && o < bo + ba) {
			(void) printf("INFO live %s (object %llu, seed %llu) "
			    "at [%llx,+%llu) birth %llu overlaps E's old "
			    "extent\n", sh->sh_fobj[f].fo_name,
			    (u_longlong_t)sh->sh_fobj[f].fo_obj,
			    (u_longlong_t)sh->sh_fobj[f].fo_seed,
			    (u_longlong_t)bo, (u_longlong_t)ba,
			    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&b));
		}
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	add_parity2_epoch(spa);
	export_pool(spa);
	kernel_fini();
	return (0);
}

static int
setup_stale_fn(void *arg)
{
	return (child_setup_stale(arg));
}

/* Phase 1: move E's stale bp, then try to reuse what that freed. */
static int
child_stalebp(void *arg)
{
	const stale_args_t *sa = arg;
	blkptr_t ebp = sh->sh_stale_bp;
	uint64_t o = DVA_GET_OFFSET(&ebp.blk_dva[0]);
	uint64_t a = DVA_GET_ASIZE(&ebp.blk_dva[0]);
	uint64_t tsz = ashift == 12 ? 90112 : 97280;
	objset_t *os;
	spa_t *spa;
	abd_t *abd;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	probe_tunable("spa_num_allocators=1");
	if (sa->sa_nolive) {
		VERIFY3P(&spike_redirect_move_live, !=, NULL);
		spike_redirect_move_live = 0;
		(void) printf("INFO spike_redirect_move_live=0\n");
	}
	spa = import_pool();
	(void) printf("INFO before the move: E's old extent live %llu of "
	    "%llu\n", (u_longlong_t)live_allocated(spa, o, a),
	    (u_longlong_t)a);
	if (live_allocated(spa, o, a) != a) {
		(void) printf("SKIP the old extent is not wholly reused\n");
		export_pool(spa);
		kernel_fini();
		return (2);
	}
	abd = abd_alloc_linear(BP_GET_PSIZE(&ebp), B_FALSE);
	err = zio_wait(zio_read(NULL, spa, &ebp, abd, BP_GET_PSIZE(&ebp),
	    NULL, NULL, ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL |
	    ZIO_FLAG_SPECULATIVE | ZIO_FLAG_RAW, NULL));
	abd_free(abd);
	(void) printf("INFO E's freed bp %s against what its old extent "
	    "holds now (read error %d)\n", err == 0 ? "verifies" :
	    "does not verify", err);
	sh->sh_nmoves = 0;
	add_move(spa, "stale-E", &ebp, &sh->sh_stale_zb, BP_GET_PSIZE(&ebp));
	err = do_move(spa, sa->sa_policy, B_FALSE);
	print_move(0);
	check((err != 0 || sh->sh_rdm[0].rdm_error != 0) &&
	    sh->sh_rdm[0].rdm_ent.rde_new_asize == 0, "the move refuses E's "
	    "stale bp, whose old extent live objects now hold (returned %d, "
	    "block error %d)", err, sh->sh_rdm[0].rdm_error);
	if (vdev_raidz_redirect_count != NULL) {
		check(vdev_raidz_redirect_count(spa, 0) == 0,
		    "no redirect was added (%llu)",
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	}
	for (int k = 0; k < 4; k++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	check(live_allocated(spa, o, a) == a, "E's old extent is still wholly "
	    "allocated after the move (%llu of %llu)",
	    (u_longlong_t)live_allocated(spa, o, a), (u_longlong_t)a);

	/* Blocks sized to what P2 would free of E's old extent (raidz2). */
	probe_tunable("metaslab_df_free_pct=100");
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int k = 0; k < 64 && live_allocated(spa, o, a) != a; k++) {
		uint64_t obj = put_object_size(os, 900 + k, tsz);
		blkptr_t b;

		txg_wait_synced(spa_get_dsl(spa), 0);
		b = object_bp(os, obj);
		if (DVA_GET_OFFSET(&b.blk_dva[0]) < o + a &&
		    o < DVA_GET_OFFSET(&b.blk_dva[0]) +
		    DVA_GET_ASIZE(&b.blk_dva[0])) {
			(void) printf("INFO new object %llu at [%llx,+%llu) "
			    "lands in E's old extent, on top of a live "
			    "block\n", (u_longlong_t)obj,
			    (u_longlong_t)DVA_GET_OFFSET(&b.blk_dva[0]),
			    (u_longlong_t)DVA_GET_ASIZE(&b.blk_dva[0]));
		}
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	probe_tunable("metaslab_df_free_pct=4");
	export_pool(spa);
	spa = import_pool();
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

static int
run_stalebp(policy_t policy, boolean_t same, boolean_t nolive)
{
	stale_args_t sa = { policy, same, nolive };
	int st;

	(void) printf("INFO stalebp, policy %s, ashift %llu%s%s\n",
	    policy_names[policy], (u_longlong_t)ashift,
	    same ? ", refilled with E's content" : "",
	    nolive ? ", nolive" : "");
	VERIFY0(phase("0 setup", setup_stale_fn, &sa));
	st = phase("1 stale move", child_stalebp, &sa);
	if (st == 2)
		return (2);
	check(st == 0, "the stale-bp phase passes (status %d)", st);
	run_zdb();
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

static int
parse_policy(const char *s, boolean_t p3)
{
	if (strcmp(s, "P1") == 0)
		return (P1);
	if (strcmp(s, "P2") == 0)
		return (P2);
	if (p3 && strcmp(s, "P3") == 0)
		return (P3);
	return (-1);
}

static int
usage(void)
{
	(void) fprintf(stderr, "usage: crash_probe <dir> <ashift> move "
	    "<TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [split]\n"
	    "       crash_probe <dir> <ashift> guard "
	    "<mos|zil|stale|checkpoint|wrongzb>\n"
	    "       crash_probe <dir> <ashift> free "
	    "<plain|dedup|clone|gang|snap> <P1|P2> | free plain P3 "
	    "[nocheck]\n"
	    "       crash_probe <dir> <ashift> fcrash "
	    "<TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [late]\n"
	    "       crash_probe <dir> <ashift> converge <P1|P2> [nolog]\n"
	    "       crash_probe <dir> <ashift> resilver <P1|P2> "
	    "<offline|offlinescrub|control|replace> [nofix|nowiden|nocols]\n"
	    "       crash_probe <dir> <ashift> stalebp <P1|P2> [same] "
	    "[nolive]\n");
	return (2);
}

int
main(int argc, char **argv)
{
	const char *mode = argc >= 4 ? argv[3] : "";
	const char *opt = argc >= 7 ? argv[6] : NULL;
	int point = -1;
	int policy = -1;
	int st;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 5 || argc > 7)
		return (usage());
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 10);
	sh = mmap(NULL, sizeof (*sh), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	VERIFY3P(sh, !=, MAP_FAILED);

	if (strcmp(mode, "move") == 0 && argc >= 6) {
		point = parse_point(argv[4]);
		policy = parse_policy(argv[5], B_FALSE);
		if (point == -1 || policy == -1 ||
		    (opt != NULL && strcmp(opt, "split") != 0))
			return (usage());
		st = run_move(point, policy, opt != NULL);
	} else if (strcmp(mode, "guard") == 0 && argc == 5 &&
	    (strcmp(argv[4], "mos") == 0 || strcmp(argv[4], "zil") == 0 ||
	    strcmp(argv[4], "stale") == 0 ||
	    strcmp(argv[4], "checkpoint") == 0 ||
	    strcmp(argv[4], "wrongzb") == 0)) {
		st = run_guard(argv[4]);
	} else if (strcmp(mode, "free") == 0 && argc >= 6 &&
	    (strcmp(argv[4], "plain") == 0 || strcmp(argv[4], "dedup") == 0 ||
	    strcmp(argv[4], "clone") == 0 || strcmp(argv[4], "gang") == 0 ||
	    strcmp(argv[4], "snap") == 0)) {
		policy = parse_policy(argv[5], strcmp(argv[4], "plain") == 0);
		if (policy == -1 ||
		    (opt != NULL && strcmp(opt, "nocheck") != 0))
			return (usage());
		st = run_free(argv[4], policy, opt != NULL);
	} else if (strcmp(mode, "fcrash") == 0 && argc >= 6) {
		point = parse_point(argv[4]);
		policy = parse_policy(argv[5], B_FALSE);
		if (point == -1 || policy == -1 ||
		    (opt != NULL && strcmp(opt, "late") != 0))
			return (usage());
		st = run_fcrash(point, policy, opt != NULL);
	} else if (strcmp(mode, "converge") == 0 && argc <= 6) {
		policy = parse_policy(argv[4], B_FALSE);
		if (policy == -1 ||
		    (argc == 6 && strcmp(argv[5], "nolog") != 0))
			return (usage());
		st = run_converge(policy, argc == 6);
	} else if (strcmp(mode, "resilver") == 0 && argc >= 6) {
		const char *how = argv[5];
		int fix = -1;

		policy = parse_policy(argv[4], B_FALSE);
		if (policy == -1 || (strcmp(how, "offline") != 0 &&
		    strcmp(how, "offlinescrub") != 0 &&
		    strcmp(how, "control") != 0 &&
		    strcmp(how, "replace") != 0))
			return (usage());
		if (opt != NULL && strcmp(opt, "nofix") == 0)
			fix = 0;
		else if (opt != NULL && strcmp(opt, "nocols") == 0)
			fix = 1;
		else if (opt != NULL && strcmp(opt, "nowiden") == 0)
			fix = 2;
		else if (opt != NULL)
			return (usage());
		st = run_resilver(policy, how, fix);
	} else if (strcmp(mode, "stalebp") == 0) {
		boolean_t same = B_FALSE, nolive = B_FALSE;

		policy = parse_policy(argv[4], B_FALSE);
		if (policy == -1)
			return (usage());
		for (int i = 5; i < argc; i++) {
			if (strcmp(argv[i], "same") == 0)
				same = B_TRUE;
			else if (strcmp(argv[i], "nolive") == 0)
				nolive = B_TRUE;
			else
				return (usage());
		}
		st = run_stalebp(policy, same, nolive);
	} else {
		return (usage());
	}
	if (st == 2) {
		(void) printf("SKIP: not run on this libzpool\n");
		return (2);
	}
	(void) printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures);
	return (failures == 0 ? 0 : 1);
}
