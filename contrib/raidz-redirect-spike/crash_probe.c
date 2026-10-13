// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike steps 5 and 6 (throwaway, not part of any tree): is the move of
 * old-epoch raidz blocks atomic across a crash, does a fresh process find
 * the redirects, does zdb account for moved blocks (part A), does freeing a
 * moved block release what it holds, in one txg with its redirect (part B),
 * and do resilvers and txg-ranged scrubs rebuild moved blocks (step 6)?
 *
 *   crash_probe <dir> <ashift> move <point> <P1|P2> [split|condense]
 *   crash_probe <dir> <ashift> guard <mos|zil|stale|checkpoint|wrongzb>
 *   crash_probe <dir> <ashift> guard <spill|spillblk|xbook>
 *   crash_probe <dir> <ashift> free <plain|dedup|clone|gang|snap> <P1|P2>
 *   crash_probe <dir> <ashift> free plain P3 [nocheck]
 *   crash_probe <dir> <ashift> fcrash <point> <P1|P2>
 *       [late|condense|condensebad]
 *   crash_probe <dir> <ashift> converge <P1|P2> [nolog]
 *   crash_probe <dir> <ashift> resilver <P1|P2>
 *       <offline|offlinescrub|control|replace> [nofix|nowiden|nocols]
 *   crash_probe <dir> <ashift> stalebp <P1|P2> [same] [nolive]
 *   crash_probe <dir> <ashift> corrupt <kind> [P1|P2] [lax]
 *   crash_probe <dir> <ashift> tomb <read|repair>
 *   crash_probe <dir> <ashift> rpass <R1|R2|R3|R4|R5|R6> <P1|P2>
 *       [none|stopgap|pass|both|noverify]
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
 * (ESTALE).  'spill' names plain-A by its object's spill block, which it
 * does not have; 'xbook' by bookmarks of every other shape that names no
 * block of it (bonus, out-of-range or negative blkid and level, an object
 * beyond the dnode count or freed, an objset that is not a dataset), then
 * by its right bookmark; 'spillblk' moves the spill block of spill-S, an
 * object phase 0 gives one, by its bookmark and plain-A by the same.  Each
 * wrong bookmark must be refused (ESTALE), each right one must move, and
 * the spill block must read back through its redirect after an import.
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
 * rpass: the scan's walk of moved blocks by their new birth (step 6).
 * Phase 0 is the resilver mode's plus bulk-B, 64 MiB written after every
 * block to move (late-L too for R4, and a mirror of m0 and m1 beside the
 * raidz, before bulk-B, for R5).  R1: child 2 offline, every block moved,
 * child 2 back and resilvered; zdb; a scrub must then repair nothing, and
 * a copy of the pool's files, with one column of each new extent damaged
 * on a child other than 2 and RP_X and RP_X offline, must read back every
 * object.  Each resilver prints MEASURE lines: its window, the bytes it
 * examined and issued, the block pointers the traversal visited, what the
 * leaves read, and what the walk did.  R2: every block moved with every
 * child online, one data column of each new extent damaged, then a scrub
 * of the txgs from the parity-2 epoch on must rebuild each one.  R3: R1
 * with the walk limited to a block per txg (spike_scan_redirect_batch),
 * exported once it has read two blocks, imported again: the walk must
 * resume from its cursor and read the others.  R4: R1 with the walk held
 * (zfs_scan_suspend_progress) after its first block, late-L moved, the
 * moved object of plain-A and small-Z that the walk reaches first freed
 * with a watch on its extents (spike_scan_watch), then the walk let go:
 * it must skip the tombstone, read nothing of the freed block, and leave
 * late-L, moved after the window, alone.  R5: R1 beside the mirror (N
 * placed on the raidz by making the mirror noalloc during the move), which
 * the resilver must not repair.  R6: plain-A's stored block pointer names
 * vdev 99 on disk; the resilver must refuse it as one scan error and read
 * every other moved block.  The last argument sets
 * spike_resilver_redirects: none (2, no walk and no stopgap), stopgap (3),
 * pass (6, the default), both (7); noverify clears
 * spike_scan_redirect_verify.
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
#include <sys/dbuf.h>
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
#define	SPILL_SIZE	(16ULL << 10)
#define	TOMB_MARKER	0xa5

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
/* Weak: the entry checks, absent from the previous step's. */
extern int spike_redirect_keep_none __attribute__((weak));
extern uint64_t vdev_raidz_redirect_damaged(spa_t *, uint64_t)
    __attribute__((weak));
/* Weak: the tombstones of freed entries, absent from the previous step's. */
extern uint64_t vdev_raidz_redirect_tombstones(spa_t *, uint64_t)
    __attribute__((weak));
extern boolean_t vdev_raidz_redirect_get_live(vdev_t *, uint64_t, uint64_t,
    raidz_redirect_ent_t *) __attribute__((weak));
/* Weak: the scan's walk of moved blocks, absent from the previous step's. */
extern spike_scan_pass_stat_t spike_scan_pass_stats __attribute__((weak));
extern uint64_t spike_scan_watch[2][3] __attribute__((weak));
extern int spike_scan_redirect_batch __attribute__((weak));
extern int spike_scan_redirect_verify __attribute__((weak));
/* Weak: the log, its structure checks and its condense, before step 7. */
extern int spike_redirect_load_lax __attribute__((weak));
extern int spike_redirect_condense __attribute__((weak));
extern int spike_redirect_condense_tombs __attribute__((weak));
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
	int			sh_condense;	/* log condensed in it */
	boolean_t		sh_small;	/* setup adds small-Z */
	int			sh_watch;	/* resilver: child watched */
	int			sh_replaced;	/* resilver: replaced + 1 */
	blkptr_t		sh_stale_bp;	/* stalebp: E's freed bp */
	zbookmark_phys_t	sh_stale_zb;
	boolean_t		sh_with_spill;	/* setup adds spill-S */
	uint64_t		sh_obj_s;
	uint64_t		sh_epoch_txg;	/* start of parity 2 */
	/* rpass */
	uint64_t		sh_bulk;	/* bytes of bulk-B, 0: none */
	boolean_t		sh_mirror;	/* setup adds a mirror */
	boolean_t		sh_late;	/* setup adds late-L */
	int			sh_col_child[MAXMOVES];	/* R2: damaged */
	uint64_t		sh_col_off[MAXMOVES];
	uint64_t		sh_col_size[MAXMOVES];
	uint64_t		sh_col_hash[MAXMOVES];	/* its bytes before */
	uint64_t		sh_rp_window;	/* R3: the first process's */
	int			sh_rp_freed;	/* R4: the freed move */
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

/*
 * Import the pool found in dir, read-only if libzpool was set up so, and
 * open it; the import's error if it fails.
 */
static int
try_import_pool(spa_t **spap)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL;
	int err;
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
	err = spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL);
	fnvlist_free(config);
	if (err == 0)
		VERIFY0(spa_open(POOL, spap, FTAG));
	return (err);
}

static spa_t *
import_pool(void)
{
	spa_t *spa;

	VERIFY0(try_import_pool(&spa));
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

/*
 * A new object with no data and a spill block of SPILL_SIZE bytes of
 * pattern <seed>.  As the SA layer does, it is made with the old maximum
 * bonus buffer, so that it has one block pointer, and the bonus buffer is
 * then shortened to leave room for the spill block pointer.
 */
static uint64_t
put_spill_object(objset_t *os, uint64_t seed)
{
	dmu_buf_t *bonus, *db;
	dnode_t *dn;
	dmu_tx_t *tx;
	uint64_t obj;

	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS,
	    SPA_MINBLOCKSIZE, DMU_OT_PLAIN_OTHER, DN_OLD_MAX_BONUSLEN, tx);
	dmu_tx_commit(tx);

	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, obj);
	dmu_tx_hold_spill(tx, obj);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY0(dmu_bonus_hold(os, obj, FTAG, &bonus));
	VERIFY0(dmu_set_bonus(bonus, DN_OLD_MAX_BONUSLEN - sizeof (blkptr_t),
	    tx));
	dmu_buf_rele(bonus, FTAG);
	VERIFY0(dnode_hold(os, obj, FTAG, &dn));
	VERIFY0(dmu_spill_hold_by_dnode(dn, DB_RF_MUST_SUCCEED, FTAG, &db));
	VERIFY0(dbuf_spill_set_blksz(db, SPILL_SIZE, tx));
	dmu_buf_will_dirty(db, tx);
	VERIFY3U(db->db_size, ==, SPILL_SIZE);
	fill_pattern(db->db_data, SPILL_SIZE, seed);
	dmu_buf_rele(db, FTAG);
	dnode_rele(dn, FTAG);
	dmu_tx_commit(tx);
	return (obj);
}

/* The spill block pointer of an object, as the last synced txg has it. */
static blkptr_t
spill_bp(objset_t *os, uint64_t obj)
{
	dnode_t *dn;
	blkptr_t bp;

	VERIFY0(dnode_hold(os, obj, FTAG, &dn));
	mutex_enter(&dn->dn_mtx);
	VERIFY(dn->dn_phys->dn_flags & DNODE_FLAG_SPILL_BLKPTR);
	bp = *DN_SPILL_BLKPTR(dn->dn_phys);
	mutex_exit(&dn->dn_mtx);
	dnode_rele(dn, FTAG);
	return (bp);
}

/* Read an object's spill block; 0 and a match, or the error. */
static int
read_spill(uint64_t obj, uint64_t seed, boolean_t *match)
{
	uint64_t *want = malloc(SPILL_SIZE);
	dmu_buf_t *bonus, *db;
	objset_t *os;
	int err;

	*match = B_FALSE;
	fill_pattern(want, SPILL_SIZE, seed);
	VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
	VERIFY0(dmu_bonus_hold(os, obj, FTAG, &bonus));
	err = dmu_spill_hold_existing(bonus, FTAG, &db);
	if (err == 0) {
		*match = (db->db_size == SPILL_SIZE &&
		    memcmp(db->db_data, want, SPILL_SIZE) == 0);
		dmu_buf_rele(db, FTAG);
	}
	dmu_buf_rele(bonus, FTAG);
	dmu_objset_rele(os, FTAG);
	free(want);
	return (err);
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
	sh->sh_epoch_txg = ea.ea_table[3];
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
	if (sh->sh_with_spill)
		sh->sh_obj_s = put_spill_object(os, 98);
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
/*
 * The redirect log as the imported pool has it: its object, records and
 * sorted records (a condensed log has as many of each as entries).  0 if
 * this libzpool has no log.
 */
static uint64_t
log_report(spa_t *spa, uint64_t *records, uint64_t *sorted)
{
	vdev_raidz_t *vdrz = spa->spa_root_vdev->vdev_child[0]->vdev_tsd;
	uint64_t hdr[2] = { 0, 0 };
	dmu_buf_t *db;

	if (&spike_redirect_condense == NULL || vdrz->vd_redirect_log == 0)
		return (0);
	VERIFY0(dmu_bonus_hold(spa_meta_objset(spa), vdrz->vd_redirect_log,
	    FTAG, &db));
	memcpy(hdr, db->db_data, sizeof (hdr));
	dmu_buf_rele(db, FTAG);
	(void) printf("INFO the redirect log is object %llu: %llu records, "
	    "the first %llu sorted\n", (u_longlong_t)vdrz->vd_redirect_log,
	    (u_longlong_t)hdr[0], (u_longlong_t)hdr[1]);
	*records = hdr[0];
	*sorted = hdr[1];
	return (vdrz->vd_redirect_log);
}

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
	{
		uint64_t rec, srt;

		if (log_report(spa, &rec, &srt) != 0 && committed &&
		    sh->sh_condense) {
			check(rec == srt && rec == vdev_raidz_redirect_count(
			    spa, 0), "B the log was condensed in the move's "
			    "txg: %llu records, all sorted, one per entry",
			    (u_longlong_t)rec);
		}
	}

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

	/* zdb must accept P3's entries where this process does. */
	(void) snprintf(cmd, sizeof (cmd), "%s%s -bcc -d -Y -e -p %s %s 2>&1",
	    &spike_redirect_keep_none != NULL && spike_redirect_keep_none ?
	    "SPIKE_ZDB_KEEP_NONE=1 " : "", zdb, dir, POOL);
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
	} else if (strcmp(which, "spill") == 0) {
		/* plain-A named by its object's spill block, which it lacks */
		blkptr_t bp = object_bp(os, sh->sh_obj[OA]);
		zbookmark_phys_t zb;

		SET_BOOKMARK(&zb, dmu_objset_id(os), sh->sh_obj[OA], 0,
		    DMU_SPILL_BLKID);
		add_move(spa, "plain-A at the spill blkid", &bp, &zb,
		    BP_GET_PSIZE(&bp));
	} else if (strcmp(which, "spillblk") == 0) {
		/* spill-S's spill block by its bookmark, plain-A by S's. */
		blkptr_t sbp = spill_bp(os, sh->sh_obj_s);
		blkptr_t bp = object_bp(os, sh->sh_obj[OA]);
		zbookmark_phys_t zb;

		SET_BOOKMARK(&zb, dmu_objset_id(os), sh->sh_obj_s, 0,
		    DMU_SPILL_BLKID);
		add_move(spa, "spill-S's spill block", &sbp, &zb,
		    BP_GET_PSIZE(&sbp));
		add_move(spa, "plain-A at S's spill blkid", &bp, &zb,
		    BP_GET_PSIZE(&bp));
	} else if (strcmp(which, "xbook") == 0) {
		/*
		 * plain-A named by bookmarks that name no block of it: every
		 * shape that reaches the liveness check, then the right one.
		 */
		blkptr_t bp = object_bp(os, sh->sh_obj[OA]);
		uint64_t id = dmu_objset_id(os);
		uint64_t a = sh->sh_obj[OA];
		const struct {
			const char	*xb_name;
			uint64_t	xb_objset;
			uint64_t	xb_object;
			int64_t		xb_level;
			uint64_t	xb_blkid;
		} xb[] = {
			{ "blkid BONUS", id, a, 0, DMU_BONUS_BLKID },
			{ "blkid -3", id, a, 0, -3ULL },
			{ "blkid 2^40", id, a, 0, 1ULL << 40 },
			{ "blkid 1, a hole", id, a, 0, 1 },
			{ "level -1", id, a, -1, 0 },
			{ "level 1 = nlevels", id, a, 1, 0 },
			{ "level 50", id, a, 50, 0 },
			{ "level 2^32", id, a, 1LL << 32, 0 },
			{ "level INT64_MIN", id, a, INT64_MIN, 0 },
			{ "object 2^40", id, 1ULL << 40, 0, 0 },
			{ "object 2^50", id, 1ULL << 50, 0, 0 },
			{ "object -10", id, -10ULL, 0, 0 },
			{ "object USERUSED", id, DMU_USERUSED_OBJECT, 0, 0 },
			{ "freed object E", id, sh->sh_obj_e, 0, 0 },
			{ "objset 1", DMU_POOL_DIRECTORY_OBJECT, a, 0, 0 },
			{ "objset 2^40", 1ULL << 40, a, 0, 0 },
			{ "objset -1", -1ULL, a, 0, 0 },
			{ "right bookmark", id, a, 0, 0 },
		};

		/* E is freed, in a txg that has synced. */
		VERIFY0(dmu_free_long_object(os, sh->sh_obj_e));
		txg_wait_synced(spa_get_dsl(spa), 0);
		for (int i = 0; i < sizeof (xb) / sizeof (xb[0]); i++) {
			zbookmark_phys_t zb;
			char name[32];

			SET_BOOKMARK(&zb, xb[i].xb_objset, xb[i].xb_object,
			    xb[i].xb_level, xb[i].xb_blkid);
			(void) snprintf(name, sizeof (name), "plain-A, %s",
			    xb[i].xb_name);
			add_move(spa, name, &bp, &zb, BP_GET_PSIZE(&bp));
		}
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
	if (strcmp(which, "spill") == 0 || strcmp(which, "spillblk") == 0 ||
	    strcmp(which, "xbook") == 0) {
		/* The right bookmark: first for spillblk, last for xbook. */
		int good = strcmp(which, "spillblk") == 0 ? 0 :
		    strcmp(which, "xbook") == 0 ? sh->sh_nmoves - 1 : -1;
		int refused = 0;

		for (int i = 0; i < sh->sh_nmoves; i++) {
			if (i != good && sh->sh_rdm[i].rdm_error == ESTALE &&
			    sh->sh_rdm[i].rdm_ent.rde_new_asize == 0)
				refused++;
		}
		check(err == 0 && refused == sh->sh_nmoves - (good >= 0),
		    "the move refuses every bookmark that names no block of "
		    "the block it is given (%d of %d with ESTALE)", refused,
		    sh->sh_nmoves - (good >= 0));
		if (good >= 0) {
			check(sh->sh_rdm[good].rdm_error == 0 &&
			    sh->sh_rdm[good].rdm_ent.rde_new_asize != 0,
			    "%s, named by its right bookmark, moves (block "
			    "error %d)", sh->sh_name[good],
			    sh->sh_rdm[good].rdm_error);
		}
	} else if (strcmp(which, "wrongzb") == 0) {
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
		uint64_t want = (strcmp(which, "spillblk") == 0 ||
		    strcmp(which, "xbook") == 0) ? 1 : 0;

		check(vdev_raidz_redirect_count(spa, 0) == want,
		    "%llu redirect(s) added (expected %llu)",
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0),
		    (u_longlong_t)want);
	}
	if (strcmp(which, "spillblk") == 0) {
		boolean_t match;
		uint64_t hits;

		/* Read spill-S's spill block from a fresh import. */
		export_pool(spa);
		spa = import_pool();
		hits = vdev_raidz_redirect_hits();
		err = read_spill(sh->sh_obj_s, 98, &match);
		hits = vdev_raidz_redirect_hits() - hits;
		check(err == 0 && match && hits == 1, "spill-S's spill block "
		    "reads back bit-exact through its redirect after an export "
		    "and import (err=%d match=%d hits=%llu)", err, match,
		    (u_longlong_t)hits);
	}
	if (strcmp(which, "spill") == 0 || strcmp(which, "spillblk") == 0 ||
	    strcmp(which, "xbook") == 0) {
		boolean_t match;
		int rerr = read_object(sh->sh_obj[OA], obj_seeds[OA], &match);

		check(rerr == 0 && match, "plain-A reads back (err=%d "
		    "match=%d)", rerr, match);
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
	sh->sh_with_spill = (strcmp(which, "spillblk") == 0);
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

/* A new object of <size> bytes of pattern <seed>, in 128K blocks. */
static uint64_t
put_bulk(objset_t *os, uint64_t seed, uint64_t size)
{
	const uint64_t chunk = 8ULL << 20;
	char *buf = malloc(size);
	dmu_tx_t *tx;
	uint64_t obj;

	fill_pattern((uint64_t *)buf, size, seed);
	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, BLKSZ,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	for (uint64_t off = 0; off < size; off += chunk) {
		uint64_t len = MIN(chunk, size - off);

		tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, obj, off, len);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, obj, off, len, buf + off, tx,
		    DMU_READ_NO_PREFETCH);
		dmu_tx_commit(tx);
	}
	free(buf);
	return (obj);
}

/* A second top-level vdev beside the raidz: a mirror of m0 and m1. */
static void
add_mirror(spa_t *spa)
{
	nvlist_t *child[2], *mirror, *root;
	char path[PATH_BUF];

	for (int c = 0; c < 2; c++) {
		int fd;

		(void) snprintf(path, sizeof (path), "%s/m%d", dir, c);
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, !=, -1);
		VERIFY0(ftruncate(fd, FILE_SIZE));
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, ashift);
	}
	mirror = fnvlist_alloc();
	fnvlist_add_string(mirror, ZPOOL_CONFIG_TYPE, VDEV_TYPE_MIRROR);
	fnvlist_add_uint64(mirror, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(mirror, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, 2);
	root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&mirror, 1);
	VERIFY0(spa_vdev_add(spa, root, B_FALSE));
	fnvlist_free(root);
	fnvlist_free(mirror);
	for (int c = 0; c < 2; c++)
		fnvlist_free(child[c]);
	(void) printf("INFO added a mirror of m0 and m1 as top-level vdev 1\n");
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
	if (sh->sh_late) {
		/* Moved by rpass R4 while a resilver runs. */
		sh->sh_fobj[add_fobj("late-L", 18, B_FALSE)].fo_obj =
		    put_object(os, 18);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	if (sh->sh_mirror) {
		dmu_objset_disown(os, B_TRUE, FTAG);
		add_mirror(spa);
		VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE,
		    FTAG, &os));
	}
	if (sh->sh_bulk != 0) {
		/* Born after every block to move: the stopgap reaches it. */
		int b = add_fobj("bulk-B", 19, B_FALSE);

		sh->sh_fobj[b].fo_size = sh->sh_bulk;
		sh->sh_fobj[b].fo_obj = put_bulk(os, 19, sh->sh_bulk);
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

/*
 * The vdev's redirect log as the pool holds it (the open txg's records
 * included): its records, in an array to kmem_free(), and their count.
 */
static uint64_t
log_read(spa_t *spa, raidz_redirect_rec_t **recs)
{
	vdev_raidz_t *vdrz = spa->spa_root_vdev->vdev_child[0]->vdev_tsd;
	objset_t *mos = spa_meta_objset(spa);
	uint64_t hdr[2];
	dmu_buf_t *db;

	*recs = NULL;
	if (vdrz->vd_redirect_log == 0)
		return (0);
	VERIFY0(dmu_bonus_hold(mos, vdrz->vd_redirect_log, FTAG, &db));
	memcpy(hdr, db->db_data, sizeof (hdr));
	dmu_buf_rele(db, FTAG);
	if (hdr[0] == 0)
		return (0);
	*recs = kmem_alloc(hdr[0] * sizeof (**recs), KM_SLEEP);
	VERIFY0(dmu_read(mos, vdrz->vd_redirect_log, 0,
	    hdr[0] * sizeof (**recs), *recs, DMU_READ_NO_PREFETCH));
	return (hdr[0]);
}

/* Whether the log, replayed, holds the key {o, b}. */
static boolean_t
log_holds(spa_t *spa, uint64_t o, uint64_t b)
{
	raidz_redirect_rec_t *recs;
	uint64_t n = log_read(spa, &recs);
	boolean_t live = B_FALSE;

	for (uint64_t r = 0; r < n; r++) {
		raidz_redirect_ent_t e;
		uint64_t type = raidz_redirect_rec_read(&recs[r], &e);

		if (e.rde_offset == o && e.rde_birth == b)
			live = (type == RAIDZ_REDIRECT_REC_ADD);
	}
	if (recs != NULL)
		kmem_free(recs, n * sizeof (*recs));
	return (live);
}

/*
 * 0 if move i has its entry in the table on disk, its key live in the log
 * and its block pointer in the ZAP; ENOENT if it has neither; EIO if it
 * has one only.
 */
static int
zap_entry(spa_t *spa, int i)
{
	vdev_raidz_t *vdrz = spa->spa_root_vdev->vdev_child[0]->vdev_tsd;
	uint64_t key[2] = { DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]) };
	boolean_t live = log_holds(spa, key[0], key[1]);
	boolean_t bp = vdrz->vd_redirect_obj != 0 &&
	    zap_length_uint64(spa_meta_objset(spa), vdrz->vd_redirect_obj,
	    key, 2, NULL, NULL) == 0;

	if (live != bp) {
		(void) printf("INFO %s: the log %s its key, the ZAP %s its "
		    "block pointer\n", sh->sh_name[i], live ? "holds" :
		    "lacks", bp ? "holds" : "lacks");
	}
	return (live && bp ? 0 : !live && !bp ? ENOENT : EIO);
}

/* Whether move i has a live entry in memory; a tombstone is not one. */
static boolean_t
mem_entry(spa_t *spa, int i)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
	uint64_t b = BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]);

	if (vdev_raidz_redirect_get_live != NULL)
		return (vdev_raidz_redirect_get_live(vd, o, b, NULL));
	return (vdev_raidz_redirect_get(vd, o, b, NULL));
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
	int		fc_condense;	/* 1: in the free's txg, 2: badly */
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
	if (fc->fc_condense != 0 && &spike_redirect_condense == NULL) {
		(void) printf("SKIP this libzpool has no log to condense\n");
		return (2);
	}
	sh->sh_condense = fc->fc_condense;
	if (fc->fc_condense != 0) {
		/* The log is rewritten in the txg of the free. */
		spike_redirect_condense = 2;
		spike_redirect_condense_tombs = (fc->fc_condense == 2);
	}
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
	(void) printf("INFO freeing plain-A in txg %llu, crash at %s%s%s\n",
	    (u_longlong_t)txg, point_names[fc->fc_point],
	    fc->fc_late ? ", table updated a txg late" : "",
	    fc->fc_condense == 1 ? ", log condensed in that txg" :
	    fc->fc_condense == 2 ? ", log condensed in that txg, keeping "
	    "freed entries" : "");
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
	{
		uint64_t rec, srt;

		if (log_report(spa, &rec, &srt) != 0 && committed &&
		    sh->sh_condense) {
			check(rec == srt && rec == vdev_raidz_redirect_count(
			    spa, 0) + (sh->sh_condense == 2), "B the log was "
			    "condensed in the free's txg: %llu records, all "
			    "sorted, one per entry", (u_longlong_t)rec);
		}
	}
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
	/* P3 is kept only to show it failing: allow it where it is gated. */
	if (policy == P3 && &spike_redirect_keep_none != NULL)
		spike_redirect_keep_none = 1;
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
run_fcrash(int point, policy_t policy, boolean_t late, int condense)
{
	fcrash_args_t fc = { point, late, condense };
	boolean_t committed = (point == SPIKE_CRASH_POST_UB);
	int st;

	(void) printf("INFO fcrash, crash at %s, policy %s, ashift %llu%s%s\n",
	    point_names[point], policy_names[policy], (u_longlong_t)ashift,
	    late ? ", late table removal" : "", condense == 1 ?
	    ", condense" : condense == 2 ? ", condense keeping freed entries" :
	    "");
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

/*
 * The corrupt mode: plain-A's record in the log, rewritten as a damaged
 * table could hold it.  Each kind but oasize and nsmall breaks one rule
 * that every record the move writes keeps.  Those two keep them all, but
 * oasize claims a larger old extent than plain-A's DVA has, which only the
 * free and the claim can see, and nsmall a new extent of one sector, too
 * small for plain-A, which only its I/O can see.  rtype gives the record
 * an unknown type, and rdup appends a second ADD of plain-A's key; the
 * other kinds rewrite one field of its ADD.  The log stores sizes in
 * 512-byte units: at ashift 9, nasize and rmisalign cannot be written.
 */
static const char *const corrupt_kinds[] = {
	"misalign", "nasize", "nout", "nspan", "nzero", "noverlap",
	"reserved0", "rmisalign", "overreserve", "birth", "epoch", "future",
	"koffset", "kout", "rtype", "rdup", "oasize", "nsmall", NULL
};

/* The kinds the load cannot see, which their use must refuse. */
static boolean_t
corrupt_at_use(const char *how)
{
	return (strcmp(how, "oasize") == 0 || strcmp(how, "nsmall") == 0);
}

/* The kinds after which the load still holds plain-A's sound entry. */
static boolean_t
corrupt_keeps_entry(const char *how)
{
	return (strcmp(how, "rdup") == 0);
}

static boolean_t
corrupt_kind_ok(const char *how)
{
	for (int k = 0; corrupt_kinds[k] != NULL; k++) {
		if (strcmp(how, corrupt_kinds[k]) == 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static boolean_t
corrupt_expressible(const char *how)
{
	return (ashift > SPA_MINBLOCKSHIFT || (strcmp(how, "nasize") != 0 &&
	    strcmp(how, "rmisalign") != 0));
}

static void
corrupt_sync(void *arg, dmu_tx_t *tx)
{
	const char *how = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	objset_t *mos = spa_meta_objset(spa);
	int i = find_move("plain-A");
	uint64_t sector = 1ULL << vd->vdev_ashift;
	uint64_t oasize = sh->sh_rdm[i].rdm_old_asize;
	uint64_t key[2] = { DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]) };
	raidz_redirect_rec_t *recs, rec;
	raidz_redirect_ent_t e;
	uint64_t n = log_read(spa, &recs);
	uint64_t r = n, at, type = RAIDZ_REDIRECT_REC_ADD;

	for (uint64_t k = 0; k < n; k++) {
		if (raidz_redirect_rec_read(&recs[k], &e) ==
		    RAIDZ_REDIRECT_REC_ADD && e.rde_offset == key[0] &&
		    e.rde_birth == key[1])
			r = k;
	}
	VERIFY3U(r, <, n);
	(void) raidz_redirect_rec_read(&recs[r], &e);
	(void) printf("INFO plain-A's record %llu of %llu in log %llu "
	    "{%llx, %llu}: N=%llx t2=%llu new asize %llu kept %llu old asize "
	    "%llu\n", (u_longlong_t)r, (u_longlong_t)n,
	    (u_longlong_t)vdrz->vd_redirect_log, (u_longlong_t)key[0],
	    (u_longlong_t)key[1], (u_longlong_t)e.rde_new_offset,
	    (u_longlong_t)e.rde_new_birth, (u_longlong_t)e.rde_new_asize,
	    (u_longlong_t)e.rde_old_reserved, (u_longlong_t)e.rde_old_asize);
	at = r;
	if (strcmp(how, "misalign") == 0) {
		e.rde_new_offset += sector / 2;
	} else if (strcmp(how, "nasize") == 0) {
		e.rde_new_asize += sector / 2;
	} else if (strcmp(how, "nout") == 0) {
		e.rde_new_offset = P2ALIGN_TYPED(vd->vdev_asize, sector,
		    uint64_t);
	} else if (strcmp(how, "nspan") == 0) {
		uint64_t m = e.rde_new_offset >> vd->vdev_ms_shift;

		if (m + 1 >= vd->vdev_ms_count)
			m--;
		e.rde_new_offset = ((m + 1) << vd->vdev_ms_shift) - sector;
	} else if (strcmp(how, "nzero") == 0) {
		e.rde_new_asize = 0;
	} else if (strcmp(how, "noverlap") == 0) {
		e.rde_new_offset = key[0];
	} else if (strcmp(how, "reserved0") == 0) {
		e.rde_old_reserved = 0;
	} else if (strcmp(how, "rmisalign") == 0) {
		e.rde_old_reserved += sector / 2;
	} else if (strcmp(how, "overreserve") == 0) {
		e.rde_old_reserved = oasize + 65536;
	} else if (strcmp(how, "birth") == 0) {
		e.rde_new_birth = key[1];
	} else if (strcmp(how, "epoch") == 0) {
		e.rde_new_birth = sh->sh_epoch_txg - 1;
		VERIFY3U(e.rde_new_birth, >, key[1]);
	} else if (strcmp(how, "future") == 0) {
		e.rde_new_birth = dmu_tx_get_txg(tx) + 1000;
	} else if (strcmp(how, "koffset") == 0) {
		e.rde_offset += sector / 2;
	} else if (strcmp(how, "kout") == 0) {
		e.rde_offset = P2ALIGN_TYPED(vd->vdev_asize, sector, uint64_t) +
		    sector;
	} else if (strcmp(how, "rtype") == 0) {
		type = 3;
	} else if (strcmp(how, "rdup") == 0) {
		at = n;
	} else if (strcmp(how, "nsmall") == 0) {
		e.rde_new_asize = sector;
	} else {
		/* oasize: kept and old asize, consistent, but too large. */
		e.rde_old_reserved = oasize + 65536;
		e.rde_old_asize = oasize + 65536;
	}
	raidz_redirect_rec_make(&rec, type, &e);
	dmu_write(mos, vdrz->vd_redirect_log, at * sizeof (rec), sizeof (rec),
	    &rec, tx, DMU_READ_NO_PREFETCH);
	if (at == n) {
		dmu_buf_t *db;
		uint64_t hdr[2];

		VERIFY0(dmu_bonus_hold(mos, vdrz->vd_redirect_log, FTAG, &db));
		dmu_buf_will_dirty(db, tx);
		memcpy(hdr, db->db_data, sizeof (hdr));
		hdr[0]++;
		memcpy(db->db_data, hdr, sizeof (hdr));
		dmu_buf_rele(db, FTAG);
	}
	(void) printf("INFO wrote it (%s) as record %llu, type %llu: key "
	    "{%llx, %llu}: N=%llx t2=%llu new asize %llu kept %llu old asize "
	    "%llu\n", how, (u_longlong_t)at, (u_longlong_t)type,
	    (u_longlong_t)e.rde_offset, (u_longlong_t)e.rde_birth,
	    (u_longlong_t)e.rde_new_offset, (u_longlong_t)e.rde_new_birth,
	    (u_longlong_t)e.rde_new_asize, (u_longlong_t)e.rde_old_reserved,
	    (u_longlong_t)e.rde_old_asize);
	kmem_free(recs, n * sizeof (*recs));
}

/* Phase 2: rewrite plain-A's entry on disk. */
static int
child_corrupt_write(void *arg)
{
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	VERIFY0(dsl_sync_task(POOL, NULL, corrupt_sync, arg, 0,
	    ZFS_SPACE_CHECK_NONE));
	export_pool(spa);
	kernel_fini();
	return (0);
}

/*
 * Phase 3: a read-write import.  It must refuse a damaged entry; if it does
 * not, what freeing plain-A then does is shown.  An oasize entry passes the
 * load: reading plain-A must go to N, and freeing it must leak what the
 * entry holds, with nothing freed twice.
 */
static int
child_corrupt_rw(void *arg)
{
	const char *how = arg;
	boolean_t use = corrupt_at_use(how);
	boolean_t small = (strcmp(how, "nsmall") == 0);
	int a = find_fobj("plain-A");
	int i = find_move("plain-A");
	const raidz_redirect_ent_t *e = &sh->sh_rdm[i].rdm_ent;
	uint64_t o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
	uint64_t vs, trees, hits, t;
	boolean_t owners[MAXOBJS] = { 0 };
	boolean_t match;
	spa_t *spa;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	err = try_import_pool(&spa);
	if (!use) {
		check(err == EROFS, "the read-write import refuses the damaged "
		    "entry with EROFS (error %d)", err);
	} else {
		check(err == 0, "the read-write import accepts the entry, "
		    "sound on its own (error %d)", err);
	}
	if (err != 0) {
		kernel_fini();
		return (failures == 0 ? 0 : 1);
	}
	(void) printf("INFO imported read-write with %llu redirects\n",
	    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	hits = vdev_raidz_redirect_hits();
	err = read_fobj(a, &match);
	hits = vdev_raidz_redirect_hits() - hits;
	(void) printf("INFO plain-A read: err=%d match=%d hits=%llu\n", err,
	    match, (u_longlong_t)hits);
	if (small) {
		check(err == EIO && hits >= 1, "plain-A's read through its "
		    "too small redirect fails with EIO (err=%d hits=%llu)", err,
		    (u_longlong_t)hits);
	} else if (use) {
		check(err == 0 && match && hits == 1, "plain-A reads back "
		    "through its redirect (err=%d match=%d hits=%llu)", err,
		    match, (u_longlong_t)hits);
	}
	(void) printf("INFO freeing plain-A\n");
	t = free_fobjs(spa, &a, 1);
	owners[a] = B_TRUE;
	t = wait_conversions(spa, owners, t);
	txg_wait_synced(spa_get_dsl(spa), t + TXG_DEFER_SIZE + 1);
	(void) printf("INFO the free synced\n");
	if (small) {
		uint64_t sector = 1ULL << ashift;

		check(!mem_entry(spa, i) && zap_entry(spa, i) == ENOENT,
		    "the entry is gone from memory and the table");
		/* The entry's one sector of N is freed, N's rest leaks. */
		check(live_allocated(spa, e->rde_new_offset + sector,
		    e->rde_new_asize - sector) == e->rde_new_asize - sector,
		    "the free leaked the rest of N [%llx, +%llu)",
		    (u_longlong_t)(e->rde_new_offset + sector),
		    (u_longlong_t)(e->rde_new_asize - sector));
		quiet_alloc(spa, &vs, &trees);
		check(vs == trees, "vs_alloc matches the trees (%llu, %llu)",
		    (u_longlong_t)vs, (u_longlong_t)trees);
	} else if (use) {
		check(!mem_entry(spa, i) && zap_entry(spa, i) == ENOENT,
		    "the entry is gone from memory and the table");
		/* What the move kept and wrote, before the damage. */
		check(live_allocated(spa, o, e->rde_old_reserved) ==
		    e->rde_old_reserved && live_allocated(spa,
		    e->rde_new_offset, e->rde_new_asize) == e->rde_new_asize,
		    "the free leaked the kept [%llx, +%llu) and N [%llx, "
		    "+%llu) rather than free what the entry claims",
		    (u_longlong_t)o, (u_longlong_t)e->rde_old_reserved,
		    (u_longlong_t)e->rde_new_offset,
		    (u_longlong_t)e->rde_new_asize);
		quiet_alloc(spa, &vs, &trees);
		check(vs == trees, "vs_alloc matches the trees (%llu, %llu)",
		    (u_longlong_t)vs, (u_longlong_t)trees);
	}
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/*
 * Phase 4: a read-only import.  It must load every sound entry and leave the
 * damaged one out; every other object must read back, and plain-A, read at
 * its old extent, must read back under P1 and never return wrong bytes.
 */
static int
child_corrupt_ro(void *arg)
{
	const char *how = arg;
	boolean_t use = corrupt_at_use(how);
	boolean_t keeps = corrupt_keeps_entry(how);
	int a = find_fobj("plain-A");
	uint64_t want = sh->sh_nmoves - (keeps ? 0 : 1);
	boolean_t match;
	spa_t *spa;
	int err;

	spa_mode_readable_spacemaps = B_TRUE;
	kernel_init(SPA_MODE_READ);
	err = try_import_pool(&spa);
	check(err == 0, "a read-only import succeeds (error %d)", err);
	if (err != 0) {
		kernel_fini();
		return (1);
	}
	check(vdev_raidz_redirect_count(spa, 0) == want, "it loads the %llu "
	    "sound entries (%llu)", (u_longlong_t)want,
	    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	if (vdev_raidz_redirect_damaged != NULL) {
		uint64_t d = vdev_raidz_redirect_damaged(spa, 0);

		check(d == (use ? 0 : 1), "it counts %llu damaged entries "
		    "(expected %d)", (u_longlong_t)d, use ? 0 : 1);
	} else {
		(void) printf("INFO this libzpool counts no damaged entry\n");
	}
	if (keeps) {
		err = read_fobj(a, &match);
		check(err == 0 && match, "plain-A reads back through its sound "
		    "entry (err=%d match=%d)", err, match);
	} else if (!use) {
		err = read_fobj(a, &match);
		(void) printf("INFO plain-A, read at its old extent: err=%d "
		    "match=%d\n", err, match);
		check(sh->sh_rdm[find_move("plain-A")].rdm_ent.
		    rde_old_reserved == sh->sh_rdm[find_move("plain-A")].
		    rdm_old_asize ? (err == 0 && match) : (err != 0 || match),
		    "plain-A reads back%s (err=%d match=%d)",
		    sh->sh_rdm[find_move("plain-A")].rdm_ent.rde_old_reserved ==
		    sh->sh_rdm[find_move("plain-A")].rdm_old_asize ? "" :
		    " or fails, never with wrong bytes", err, match);
	}
	sh->sh_fobj[a].fo_gone = B_TRUE;
	check_others_read();
	spa_close(spa, FTAG);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/*
 * zdb on the damaged pool must run to its end and name the damaged entry;
 * after the free of an oasize entry, it must report what that free leaked.
 */
static void
run_zdb_damaged(const char *how)
{
	const char *zdb = getenv("ZDB") != NULL ? getenv("ZDB") : "zdb";
	char cmd[PATH_BUF * 2];
	char line[4096];
	int named = 0, leaked = 0, status;
	FILE *fp;

	(void) snprintf(cmd, sizeof (cmd), "%s -bcc -d -Y -e -p %s %s 2>&1",
	    zdb, dir, POOL);
	(void) printf("INFO running %s\n", cmd);
	(void) fflush(stdout);
	fp = popen(cmd, "r");
	VERIFY3P(fp, !=, NULL);
	while (fgets(line, sizeof (line), fp) != NULL) {
		char *p = line;
		char *cr;

		while ((cr = strchr(p, '\r')) != NULL)
			p = cr + 1;
		(void) printf("zdb| %s%s", p, strchr(p, '\n') ? "" : "\n");
		if (strstr(p, "damaged redirect record") != NULL)
			named++;
		if (strstr(p, "leaked space") != NULL)
			leaked++;
	}
	status = pclose(fp);
	check(WIFEXITED(status) && WEXITSTATUS(status) < 128, "zdb runs to "
	    "its end (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) :
	    128 + WTERMSIG(status));
	if (!corrupt_at_use(how)) {
		check(named >= 1, "zdb names the damaged entry (%d lines)",
		    named);
	} else {
		check(leaked >= 1, "zdb reports the space the free leaked (%d "
		    "lines)", leaked);
	}
}

static int
run_corrupt(const char *how, policy_t policy)
{
	int st;

	(void) printf("INFO corrupt %s, policy %s, ashift %llu\n", how,
	    policy_names[policy], (u_longlong_t)ashift);
	if (!corrupt_expressible(how)) {
		(void) printf("SKIP %s cannot be written at ashift %llu: the "
		    "log stores sizes in 512-byte units\n", how,
		    (u_longlong_t)ashift);
		return (2);
	}
	VERIFY0(phase("0 setup", setup_free_fn, NULL));
	st = phase("1 move", child_move_fobjs, &policy);
	check(st == 0, "the move phase passes (status %d)", st);
	VERIFY0(phase("2 damage the entry", child_corrupt_write, (void *)how));
	st = phase("3 read-write import", child_corrupt_rw, (void *)how);
	check(st == 0, "the read-write phase passes (status %d)", st);
	st = phase("4 read-only import", child_corrupt_ro, (void *)how);
	check(st == 0, "the read-only phase passes (status %d)", st);
	run_zdb_damaged(how);
	return (0);
}

/*
 * The tomb mode.  A holder of plain-A's block pointer reads it right after
 * plain-A was freed, while [O, O + kept) and N sit in the defer trees, under
 * P2.  'read': small-Z's new extent is first placed in plain-A's freed
 * tail, so that O holds another live block.  'repair': one sector of a
 * data column of plain-A's freed tail is overwritten on disk, as a block
 * written there would, which leaves O one column from plain-A's bytes.
 */
typedef struct tomb_arg {
	uint64_t	ta_offset;
	uint64_t	ta_birth;
	boolean_t	ta_removed;
	uint64_t	ta_txg;
} tomb_arg_t;

/* The free path, called again for plain-A's key, must not convert it. */
static void
tomb_remove_sync(void *arg, dmu_tx_t *tx)
{
	tomb_arg_t *ta = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];
	raidz_redirect_ent_t ent;

	ta->ta_txg = dmu_tx_get_txg(tx);
	ta->ta_removed = vdev_raidz_redirect_remove(vd, ta->ta_offset,
	    ta->ta_birth, &ent);
}

static boolean_t
tomb_lookup(spa_t *spa, uint64_t offset, uint64_t birth)
{
	return (vdev_raidz_redirect_get(spa->spa_root_vdev->vdev_child[0],
	    offset, birth, NULL));
}

/*
 * What holds [off, off + len): allocatable, still freeing or deferred, or
 * none of those, that is allocated again.
 */
static void
space_report(spa_t *spa, const char *what, uint64_t off, uint64_t len)
{
	metaslab_t *msp = ms_of(spa, off, len);
	uint64_t s, l;
	boolean_t held = B_FALSE;

	mutex_enter(&msp->ms_lock);
	VERIFY0(metaslab_load(msp));
	held = zfs_range_tree_find_in(msp->ms_freeing, off, len, &s, &l) ||
	    zfs_range_tree_find_in(msp->ms_freed, off, len, &s, &l);
	for (int t = 0; t < TXG_DEFER_SIZE; t++) {
		held = held || zfs_range_tree_find_in(msp->ms_defer[t], off,
		    len, &s, &l);
	}
	(void) printf("INFO %s [%llx, +%llu): %s allocatable, %s, %llu of its "
	    "bytes allocatable\n", what, (u_longlong_t)off, (u_longlong_t)len,
	    zfs_range_tree_contains(msp->ms_allocatable, off, len) ? "all" :
	    "not all", held ? "partly in a freeing or defer tree" :
	    "in no freeing or defer tree",
	    (u_longlong_t)tree_overlap(msp->ms_allocatable, off, len));
	mutex_exit(&msp->ms_lock);
}

static void
tomb_space_report(spa_t *spa, uint64_t o, uint64_t kept,
    const raidz_redirect_ent_t *e)
{
	space_report(spa, "kept", o, kept);
	space_report(spa, "N", e->rde_new_offset, e->rde_new_asize);
}

static int
child_tomb(void *arg)
{
	const char *how = arg;
	boolean_t repair = (strcmp(how, "repair") == 0);
	int a = find_fobj("plain-A");
	int i = find_move("plain-A");
	const raidz_redirect_ent_t *e = &sh->sh_rdm[i].rdm_ent;
	blkptr_t bp = sh->sh_bp[i];
	zbookmark_phys_t zb = sh->sh_zb[i];
	uint64_t o = DVA_GET_OFFSET(&bp.blk_dva[0]);
	uint64_t t1 = BP_GET_PHYSICAL_BIRTH(&bp);
	uint64_t kept = e->rde_old_reserved;
	uint64_t lsize = BP_GET_LSIZE(&bp);
	uint64_t sector = 1ULL << ashift;
	uint64_t *want = malloc(lsize);
	uint8_t *cols[WIDTH] = { NULL };
	uint64_t f, synced, hits, tombs;
	int changed = 0, err;
	boolean_t match;
	tomb_arg_t ta;
	raidz_row_t *rr;
	raidz_map_t *rm;
	abd_t *abd;
	spa_t *spa;
	zio_t zio;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	VERIFY3U(kept, <, sh->sh_rdm[i].rdm_old_asize);
	if (!repair) {
		/* small-Z's N, claimed at the start of plain-A's tail. */
		int z = find_fobj("small-Z");
		int k = sh->sh_nmoves;
		objset_t *os;

		VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
		add_fobj_moves(spa, os, z, 0);
		dmu_objset_rele(os, FTAG);
		sh->sh_rdm[k].rdm_place = o + kept;
		err = vdev_raidz_redirect_move(spa, 0, &sh->sh_rdm[k], 1,
		    RAIDZ_REDIRECT_KEEP_FIRST, B_FALSE);
		print_move(k);
		check(err == 0 && sh->sh_rdm[k].rdm_error == 0 &&
		    sh->sh_rdm[k].rdm_ent.rde_new_offset == o + kept,
		    "small-Z's new extent lies in plain-A's freed tail at %llx "
		    "(error %d, block error %d)", (u_longlong_t)(o + kept),
		    err, sh->sh_rdm[k].rdm_error);
		sh->sh_fobj[z].fo_moved = B_TRUE;
	}

	f = free_fobjs(spa, &a, 1);
	txg_wait_synced(spa_get_dsl(spa), f);
	(void) printf("INFO freed plain-A in txg %llu; O=%llx kept %llu, "
	    "N=%llx asize %llu\n", (u_longlong_t)f, (u_longlong_t)o,
	    (u_longlong_t)kept, (u_longlong_t)e->rde_new_offset,
	    (u_longlong_t)e->rde_new_asize);

	/* plain-A's old extent, column by column, as its old layout has it. */
	rm = rs_map(&zio, o, BP_GET_PSIZE(&bp), 1);
	rr = rm->rm_row[0];
	if (repair) {
		/* A sector of the second data column, in the freed tail. */
		const raidz_col_t *rc = &rr->rr_col[rr->rr_firstdatacol + 1];
		uint8_t *mark = malloc(sector);
		char path[PATH_BUF];
		int fd;

		memset(mark, TOMB_MARKER, sector);
		child_file(path, sizeof (path), rc->rc_devidx);
		fd = open(path, O_RDWR);
		VERIFY3S(fd, !=, -1);
		VERIFY3S(pwrite(fd, mark, sector, rc->rc_offset +
		    VDEV_LABEL_START_SIZE), ==, sector);
		(void) close(fd);
		free(mark);
		(void) printf("INFO overwrote child %d at %llx, a sector of "
		    "plain-A's data column %d\n", (int)rc->rc_devidx,
		    (u_longlong_t)rc->rc_offset, rr->rr_firstdatacol + 1);
	}
	for (int c = 0; c < rr->rr_cols; c++) {
		if (rr->rr_col[c].rc_size == 0)
			continue;
		cols[c] = malloc(rr->rr_col[c].rc_size);
		rs_col_io(&rr->rr_col[c], cols[c], B_FALSE);
	}

	/* The stale read. */
	abd = abd_alloc_linear(lsize, B_FALSE);
	fill_pattern(want, lsize, sh->sh_fobj[a].fo_seed);
	hits = vdev_raidz_redirect_hits();
	err = zio_wait(zio_read(NULL, spa, &bp, abd, lsize, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, &zb));
	hits = vdev_raidz_redirect_hits() - hits;
	match = (err == 0 && memcmp(abd_to_buf(abd), want, lsize) == 0);
	synced = spa_last_synced_txg(spa);
	tombs = vdev_raidz_redirect_tombstones != NULL ?
	    vdev_raidz_redirect_tombstones(spa, 0) : 0;
	(void) printf("INFO stale read of plain-A after txg %llu synced: "
	    "err=%d match=%d redirect hits %llu, %llu tombstone(s)\n",
	    (u_longlong_t)synced, err, match, (u_longlong_t)hits,
	    (u_longlong_t)tombs);
	for (int c = 0; c < rr->rr_cols; c++) {
		uint8_t *now;

		if (cols[c] == NULL)
			continue;
		now = malloc(rr->rr_col[c].rc_size);
		rs_col_io(&rr->rr_col[c], now, B_FALSE);
		if (memcmp(now, cols[c], rr->rr_col[c].rc_size) != 0) {
			(void) printf("INFO column %d of O, on child %d, was "
			    "written by the read\n", c,
			    (int)rr->rr_col[c].rc_devidx);
			changed++;
		}
		free(now);
		free(cols[c]);
	}
	rs_map_free(&zio, rm);
	abd_free(abd);
	free(want);
	check(synced < f + TXG_DEFER_SIZE, "the read came before the defer "
	    "window of the free closed (txg %llu synced, freed in %llu)",
	    (u_longlong_t)synced, (u_longlong_t)f);
	check(err == 0 && match && hits == 1, "the stale read of plain-A goes "
	    "to N and returns its bytes (err=%d match=%d hits=%llu)", err,
	    match, (u_longlong_t)hits);
	check(changed == 0, "nothing of plain-A's old extent was written (%d "
	    "columns changed)", changed);

	/* What the tombstone does and does not do. */
	if (vdev_raidz_redirect_tombstones != NULL) {
		check(tombs == 1, "plain-A's entry stands as a tombstone "
		    "(%llu)", (u_longlong_t)tombs);
		check(tomb_lookup(spa, o, t1) && !mem_entry(spa, i), "I/O "
		    "finds it, the live lookups do not");
		check(!tomb_lookup(spa, o, f + TXG_DEFER_SIZE + 1),
		    "a block born at O after the window would not find it");
	}
	check(!in_allocatable(spa, o, kept) && !in_allocatable(spa,
	    e->rde_new_offset, e->rde_new_asize), "neither [O, O + kept) nor "
	    "N can be allocated yet");
	ta.ta_offset = o;
	ta.ta_birth = t1;
	ta.ta_removed = B_FALSE;
	VERIFY0(dsl_sync_task(POOL, NULL, tomb_remove_sync, &ta, 0,
	    ZFS_SPACE_CHECK_NONE));
	check(!ta.ta_removed, "the free path, asked again for plain-A's key "
	    "in txg %llu, does not convert it", (u_longlong_t)ta.ta_txg);

	/* The tombstone stands until the sync of txg f + TXG_DEFER_SIZE. */
	if (vdev_raidz_redirect_tombstones != NULL &&
	    spa_last_synced_txg(spa) < f + TXG_DEFER_SIZE) {
		check(vdev_raidz_redirect_tombstones(spa, 0) == 1, "with txg "
		    "%llu synced, it still stands",
		    (u_longlong_t)spa_last_synced_txg(spa));
		txg_wait_synced(spa_get_dsl(spa), f + TXG_DEFER_SIZE);
		check(vdev_raidz_redirect_tombstones(spa, 0) == 0, "with txg "
		    "%llu synced, it is gone", (u_longlong_t)(f +
		    TXG_DEFER_SIZE));
	}

	/* Past the window: nothing is left of the entry. */
	for (int k = 0; k <= TXG_DEFER_SIZE + 1; k++) {
		dirty_txg(spa);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	if (vdev_raidz_redirect_tombstones != NULL) {
		check(vdev_raidz_redirect_tombstones(spa, 0) == 0 &&
		    !tomb_lookup(spa, o, t1), "after txg %llu, the tombstone "
		    "is gone (%llu)", (u_longlong_t)spa_last_synced_txg(spa),
		    (u_longlong_t)vdev_raidz_redirect_tombstones(spa, 0));
	}
	check(in_allocatable(spa, o, kept) && in_allocatable(spa,
	    e->rde_new_offset, e->rde_new_asize), "[O, O + kept) and N are "
	    "allocatable again");
	if (!in_allocatable(spa, o, kept) || !in_allocatable(spa,
	    e->rde_new_offset, e->rde_new_asize))
		tomb_space_report(spa, o, kept, e);
	sh->sh_fobj[a].fo_gone = B_TRUE;
	check_others_read();
	export_pool(spa);
	spa = import_pool();
	check(vdev_raidz_redirect_tombstones == NULL ||
	    vdev_raidz_redirect_tombstones(spa, 0) == 0, "an import finds no "
	    "tombstone");
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* Phase 1 of tomb: move every block but small-Z's, under P2. */
static int
child_tomb_move(void *arg)
{
	policy_t policy = P2;

	(void) arg;
	sh->sh_fobj[find_fobj("small-Z")].fo_moved = B_FALSE;
	return (child_move_fobjs(&policy));
}

static int
run_tomb(const char *how)
{
	int st;

	(void) printf("INFO tomb %s, policy P2, ashift %llu\n", how,
	    (u_longlong_t)ashift);
	sh->sh_small = B_TRUE;
	VERIFY0(phase("0 setup", setup_free_fn, NULL));
	st = phase("1 move", child_tomb_move, NULL);
	check(st == 0, "the move phase passes (status %d)", st);
	st = phase("2 stale read", child_tomb, (void *)how);
	check(st == 0, "the stale-read phase passes (status %d)", st);
	run_zdb();
	return (0);
}

/*
 * The rpass mode: the scan's walk of moved blocks by their new birth.
 */
#define	RP_X		0	/* the second child, offline for the reads */
#define	RP_BULK		(64ULL << 20)

typedef struct rp_args {
	const char	*rp_test;	/* R1 ... R5 */
	policy_t	rp_policy;
	int		rp_fix;		/* spike_resilver_redirects, or -1 */
} rp_args_t;

static const char *rp_copy_dir;	/* rpass: the copy of the pool's files */

/* What the leaves under vd did, summed. */
typedef struct rp_leafstat {
	uint64_t	rl_rd_bytes;
	uint64_t	rl_rd_ops;
	uint64_t	rl_wr_bytes;
	uint64_t	rl_cksum;
	uint64_t	rl_rd_errors;
	uint64_t	rl_processed;
	uint64_t	rl_healed;
} rp_leafstat_t;

static void
rp_leaves(vdev_t *vd, rp_leafstat_t *ls)
{
	if (vd->vdev_ops->vdev_op_leaf) {
		vdev_stat_t *vs = &vd->vdev_stat;

		mutex_enter(&vd->vdev_stat_lock);
		ls->rl_rd_bytes += vs->vs_bytes[ZIO_TYPE_READ];
		ls->rl_rd_ops += vs->vs_ops[ZIO_TYPE_READ];
		ls->rl_wr_bytes += vs->vs_bytes[ZIO_TYPE_WRITE];
		ls->rl_cksum += vs->vs_checksum_errors;
		ls->rl_rd_errors += vs->vs_read_errors;
		ls->rl_processed += vs->vs_scan_processed;
		ls->rl_healed += vs->vs_self_healed;
		mutex_exit(&vd->vdev_stat_lock);
		return;
	}
	for (int c = 0; c < vd->vdev_children; c++)
		rp_leaves(vd->vdev_child[c], ls);
}

/* The leaves of top-level vdev <top>, or of the whole pool if -1. */
static rp_leafstat_t
rp_stat(spa_t *spa, int top)
{
	rp_leafstat_t ls = { 0 };

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	rp_leaves(top < 0 ? spa->spa_root_vdev :
	    spa->spa_root_vdev->vdev_child[top], &ls);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	return (ls);
}

static spike_scan_pass_stat_t *
rp_walk(void)
{
	return (&spike_scan_pass_stats != NULL ? &spike_scan_pass_stats :
	    NULL);
}

static void
rp_print_walk(const char *when)
{
	spike_scan_pass_stat_t *st = rp_walk();

	if (st == NULL) {
		(void) printf("INFO %s: this libzpool has no walk of moved "
		    "blocks\n", when);
		return;
	}
	(void) printf("MEASURE %s walk: walked=%llu tombs=%llu in_window=%llu "
	    "issued=%llu issued_bytes=%llu damaged=%llu txgs=%llu "
	    "resumed=%llu done_txg=%llu traversal_visited=%llu "
	    "watch_hits=%llu\n", when, (u_longlong_t)st->sps_walked,
	    (u_longlong_t)st->sps_tombs, (u_longlong_t)st->sps_in_window,
	    (u_longlong_t)st->sps_issued, (u_longlong_t)st->sps_issued_bytes,
	    (u_longlong_t)st->sps_damaged, (u_longlong_t)st->sps_txgs,
	    (u_longlong_t)st->sps_resumed, (u_longlong_t)st->sps_done_txg,
	    (u_longlong_t)st->sps_visited, (u_longlong_t)st->sps_watch_hits);
	for (uint64_t t = 0; t < MIN(st->sps_ntrace, SPIKE_SCAN_TRACE); t++) {
		(void) printf("INFO walk read {%llu, %llx, %llu} in txg %llu\n",
		    (u_longlong_t)st->sps_trace[t][0],
		    (u_longlong_t)st->sps_trace[t][1],
		    (u_longlong_t)st->sps_trace[t][2],
		    (u_longlong_t)st->sps_trace[t][3]);
	}
}

/* Whether the walk read move i's block (its key is in the trace). */
static boolean_t
rp_walk_read(int i)
{
	spike_scan_pass_stat_t *st = rp_walk();

	for (uint64_t t = 0; st != NULL &&
	    t < MIN(st->sps_ntrace, SPIKE_SCAN_TRACE); t++) {
		if (st->sps_trace[t][1] ==
		    DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]) &&
		    st->sps_trace[t][2] == BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]))
			return (B_TRUE);
	}
	return (B_FALSE);
}

static void
rp_set_batch(int batch)
{
	if (&spike_scan_redirect_batch == NULL) {
		(void) printf("INFO this libzpool has no "
		    "spike_scan_redirect_batch\n");
		return;
	}
	spike_scan_redirect_batch = batch;
	(void) printf("INFO spike_scan_redirect_batch=%d\n", batch);
}

/* What the resilver, or scrub, that just finished cost. */
static void
rp_measure(spa_t *spa, const char *what, const pool_scan_stat_t *ps,
    const rp_leafstat_t *b, const rp_leafstat_t *a, uint64_t txgs)
{
	dsl_scan_t *scn = spa_get_dsl(spa)->dp_scan;

	(void) printf("MEASURE %s: window (%llu, %llu) to_examine=%llu "
	    "examined=%llu skipped=%llu issued=%llu processed=%llu "
	    "errors=%llu txgs=%llu leaf_read_bytes=%llu leaf_read_ops=%llu "
	    "leaf_cksum=%llu\n", what,
	    (u_longlong_t)scn->scn_phys.scn_min_txg,
	    (u_longlong_t)scn->scn_phys.scn_max_txg,
	    (u_longlong_t)ps->pss_to_examine, (u_longlong_t)ps->pss_examined,
	    (u_longlong_t)ps->pss_skipped, (u_longlong_t)ps->pss_issued,
	    (u_longlong_t)ps->pss_processed, (u_longlong_t)ps->pss_errors,
	    (u_longlong_t)txgs, (u_longlong_t)(a->rl_rd_bytes - b->rl_rd_bytes),
	    (u_longlong_t)(a->rl_rd_ops - b->rl_rd_ops),
	    (u_longlong_t)(a->rl_cksum - b->rl_cksum));
}

/* The objset of the moves' bookmarks, and the moves of fobj f appended. */
static int
rp_move_one(spa_t *spa, int f, policy_t policy)
{
	uint64_t snapid = snap_objset_id();
	int first = sh->sh_nmoves;
	objset_t *os;
	int err;

	VERIFY0(dmu_objset_hold(POOL, FTAG, &os));
	add_fobj_moves(spa, os, f, snapid);
	dmu_objset_rele(os, FTAG);
	err = vdev_raidz_redirect_move(spa, 0, &sh->sh_rdm[first],
	    sh->sh_nmoves - first, policy == P2 ? RAIDZ_REDIRECT_KEEP_FIRST :
	    RAIDZ_REDIRECT_KEEP_ALL, B_FALSE);
	for (int i = first; i < sh->sh_nmoves; i++) {
		print_move(i);
		if (sh->sh_rdm[i].rdm_error != 0)
			err = sh->sh_rdm[i].rdm_error;
	}
	sh->sh_fobj[f].fo_moved = B_TRUE;
	return (err);
}

/*
 * R4, while the resilver runs: hold the walk (zfs_scan_suspend_progress),
 * move late-L, then free whichever of plain-A and small-Z the walk reaches
 * first, with a watch on the extents the free releases, and let the walk
 * go on, through all that is left, while the freed block's tombstone
 * stands.
 */
static void
rp_free_and_move(spa_t *spa, policy_t policy, boolean_t walk)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	dsl_scan_t *scn = dp->dp_scan;
	int ia = find_move("plain-A"), iz = find_move("small-Z");
	uint64_t oa = DVA_GET_OFFSET(&sh->sh_bp[ia].blk_dva[0]);
	uint64_t oz = DVA_GET_OFFSET(&sh->sh_bp[iz].blk_dva[0]);
	uint64_t c = walk ? scn->scn_spike_rdc[2] : 0;
	const raidz_redirect_ent_t *e;
	uint64_t o, txg;
	int i, f, err;

	zfs_scan_suspend_progress = 1;
	txg_wait_synced(dp, 0);
	txg_wait_synced(dp, 0);
	err = rp_move_one(spa, find_fobj("late-L"), policy);
	check(err == 0, "late-L moves while the resilver runs (%d), new "
	    "birth %llu", err, (u_longlong_t)sh->sh_rdm[sh->sh_nmoves - 1].
	    rdm_ent.rde_new_birth);

	if (walk)
		c = scn->scn_spike_rdc[2];
	i = (oa > c && (oz <= c || oa < oz)) ? ia : iz;
	e = &sh->sh_rdm[i].rdm_ent;
	o = DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]);
	f = sh->sh_owner[i];
	(void) printf("INFO freeing %s, key {0, %llx, %llu}\n", sh->sh_name[i],
	    (u_longlong_t)o,
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]));
	if (walk) {
		(void) printf("INFO walk held at cursor {%llu, %llx, %llu} "
		    "state %llu\n", (u_longlong_t)scn->scn_spike_rdc[1],
		    (u_longlong_t)scn->scn_spike_rdc[2],
		    (u_longlong_t)scn->scn_spike_rdc[3],
		    (u_longlong_t)scn->scn_spike_rdc[0]);
		check(scn->scn_spike_rdc[0] == SPIKE_RDC_WALKING && c < o &&
		    !rp_walk_read(i), "the walk has not reached %s's entry "
		    "when it is freed", sh->sh_name[i]);
	}
	if (rp_walk() != NULL) {
		spike_scan_watch[0][0] = 0;
		spike_scan_watch[0][1] = o;
		spike_scan_watch[0][2] = e->rde_old_reserved;
		spike_scan_watch[1][0] = 0;
		spike_scan_watch[1][1] = e->rde_new_offset;
		spike_scan_watch[1][2] = e->rde_new_asize;
		spike_scan_pass_stats.sps_watch_hits = 0;
	}
	txg = free_fobjs(spa, &f, 1);
	txg_wait_synced(dp, txg);
	sh->sh_rp_freed = i;
	(void) printf("INFO freed %s in txg %llu: watching [%llx, +%llu) and "
	    "[%llx, +%llu) for scan reads\n", sh->sh_fobj[f].fo_name,
	    (u_longlong_t)txg, (u_longlong_t)o,
	    (u_longlong_t)e->rde_old_reserved,
	    (u_longlong_t)e->rde_new_offset, (u_longlong_t)e->rde_new_asize);
	if (walk) {
		check(vdev_raidz_redirect_tombstones(spa, 0) == 1, "its "
		    "tombstone stands once txg %llu synced",
		    (u_longlong_t)txg);
		rp_set_batch(0);
	}
	zfs_scan_suspend_progress = 0;
}

/*
 * R6: name another vdev in DVA 0 of the block pointer stored with plain-A's
 * entry on disk, as damage would; the entry in memory is unchanged.
 */
static void
rp_damage_bp_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_raidz_t *vdrz = spa->spa_root_vdev->vdev_child[0]->vdev_tsd;
	objset_t *mos = spa_meta_objset(spa);
	int i = find_move("plain-A");
	uint64_t key[2] = { DVA_GET_OFFSET(&sh->sh_bp[i].blk_dva[0]),
	    BP_GET_PHYSICAL_BIRTH(&sh->sh_bp[i]) };
	uint64_t val[RAIDZ_REDIRECT_BPS_VALUES];
	blkptr_t bp;

	VERIFY0(zap_lookup_uint64(mos, vdrz->vd_redirect_obj, key, 2, 8,
	    RAIDZ_REDIRECT_BPS_VALUES, val));
	memcpy(&bp, val, sizeof (bp));
	VERIFY(DVA_EQUAL(&bp.blk_dva[0], &sh->sh_bp[i].blk_dva[0]));
	DVA_SET_VDEV(&bp.blk_dva[0], 99);
	memcpy(val, &bp, sizeof (bp));
	VERIFY0(zap_update_uint64(mos, vdrz->vd_redirect_obj, key, 2, 8,
	    RAIDZ_REDIRECT_BPS_VALUES, val, tx));
	(void) printf("INFO plain-A's stored block pointer now names vdev 99 "
	    "in DVA 0\n");
}

/*
 * Phase 1 of R1, R3, R4, R5: child 2 offline, move every block, bring it
 * back and resilver.  R3 exports in the middle of the walk; R4 frees a
 * moved block and moves another while the walk runs.
 */
static int
child_rp_resilver(void *arg)
{
	rp_args_t *ra = arg;
	boolean_t r3 = strcmp(ra->rp_test, "R3") == 0;
	boolean_t r4 = strcmp(ra->rp_test, "R4") == 0;
	boolean_t r5 = strcmp(ra->rp_test, "R5") == 0;
	boolean_t r6 = strcmp(ra->rp_test, "R6") == 0;
	spike_scan_pass_stat_t *st;
	rp_leafstat_t b, a, bm, am;
	pool_scan_stat_t ps;
	vdev_state_t ns;
	vdev_t *tvd, *cvd;
	uint64_t t0, t2, mguid = 0;
	boolean_t walk;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	rs_set_fix(ra->rp_fix);
	st = rp_walk();
	walk = st != NULL && ra->rp_fix != 2 && ra->rp_fix != 3;
	if ((r3 || r4) && walk)
		rp_set_batch(1);
	spa = import_pool();
	tvd = spa->spa_root_vdev->vdev_child[0];
	cvd = tvd->vdev_child[RS_CHILD];
	sh->sh_watch = RS_CHILD;
	VERIFY0(vdev_offline(spa, cvd->vdev_guid, 0));
	(void) printf("INFO child %d offline (state %d)\n", RS_CHILD,
	    (int)cvd->vdev_state);
	if (r5) {
		/* N must be on the raidz vdev. */
		mguid = spa->spa_root_vdev->vdev_child[1]->vdev_guid;
		VERIFY0(spa_vdev_noalloc(spa, mguid));
	}
	move_fobjs(spa, ra->rp_policy);
	if (r5)
		VERIFY0(spa_vdev_alloc(spa, mguid));
	if (r6) {
		VERIFY0(dsl_sync_task(POOL, NULL, rp_damage_bp_sync, NULL, 0,
		    ZFS_SPACE_CHECK_NONE));
	}
	for (int k = 0; k < 3; k++) {
		dirty_txg(spa);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	t2 = sh->sh_rdm[0].rdm_ent.rde_new_birth;
	check(vdev_dtl_contains(cvd, DTL_MISSING, t2, 1), "child %d's "
	    "DTL_MISSING holds the move's txg %llu", RS_CHILD,
	    (u_longlong_t)t2);
	b = rp_stat(spa, -1);
	if (r5)
		bm = rp_stat(spa, 1);
	t0 = spa_last_synced_txg(spa);
	VERIFY0(vdev_online(spa, cvd->vdev_guid, 0, &ns));
	(void) printf("INFO child %d online (state %d)\n", RS_CHILD, (int)ns);

	if ((r3 || r4) && walk) {
		int n;

		/* Until the walk has read a block and has more to read. */
		for (n = 0; n < RS_TXGS; n++) {
			txg_wait_synced(spa_get_dsl(spa), 0);
			if (st->sps_issued >= (r3 ? 2 : 1) ||
			    st->sps_done_txg != 0)
				break;
			if (n % 8 == 7)
				dirty_txg(spa);
		}
		rp_print_walk("before the interruption");
		check(st->sps_issued >= 1 && st->sps_done_txg == 0,
		    "the walk has read a block and is not done after %d txgs",
		    n);
		if (st->sps_done_txg != 0) {
			export_pool(spa);
			kernel_fini();
			return (1);
		}
	} else if (r3 || r4) {
		/* No walk: interrupt the resilver itself. */
		for (int n = 0; n < 6; n++)
			txg_wait_synced(spa_get_dsl(spa), 0);
		(void) printf("INFO no walk in this run; resilvering %d\n",
		    dsl_scan_resilvering(spa_get_dsl(spa)));
	}
	if (r3) {
		sh->sh_rp_window = walk ? st->sps_in_window : 0;
		check(vdev_dtl_contains(cvd, DTL_MISSING, t2, 1) &&
		    dsl_scan_resilvering(spa_get_dsl(spa)), "the resilver "
		    "runs and child %d's DTL_MISSING still holds txg %llu",
		    RS_CHILD, (u_longlong_t)t2);
		(void) printf("INFO exporting in the middle of the resilver\n");
		export_pool(spa);
		kernel_fini();
		return (failures == 0 ? 0 : 1);
	}
	if (r4)
		rp_free_and_move(spa, ra->rp_policy, walk);

	rs_wait(spa, rs_offline_done, &ps);
	check(ps.pss_func == POOL_SCAN_RESILVER &&
	    ps.pss_state == DSS_FINISHED && ps.pss_errors == (r6 ? 1 : 0) &&
	    rs_offline_done(spa), "the resilver of child %d finished with %d "
	    "error and cleared its DTL (%llu)", RS_CHILD, r6 ? 1 : 0,
	    (u_longlong_t)ps.pss_errors);
	a = rp_stat(spa, -1);
	rp_measure(spa, "resilver", &ps, &b, &a,
	    spa_last_synced_txg(spa) - t0);
	rp_print_walk("resilver");
	if (walk) {
		check(st->sps_in_window == sh->sh_nmoves - (r4 ? 2 : 0) &&
		    st->sps_done_txg != 0, "the walk found the %d moved "
		    "blocks the traversal prunes (%llu)",
		    sh->sh_nmoves - (r4 ? 2 : 0),
		    (u_longlong_t)st->sps_in_window);
	}
	if (r6) {
		/* A new extent with no column on child 2 needs no read. */
		check(st != NULL && st->sps_damaged == 1 &&
		    st->sps_in_window == sh->sh_nmoves &&
		    !rp_walk_read(find_move("plain-A")), "the walk refused "
		    "plain-A's damaged block pointer and went on to the %d "
		    "other moved blocks (%llu read)", sh->sh_nmoves - 1,
		    (u_longlong_t)(st != NULL ? st->sps_issued : 0));
	}
	if (r4) {
		dsl_scan_t *scn = spa_get_dsl(spa)->dp_scan;
		const raidz_redirect_ent_t *le =
		    &sh->sh_rdm[sh->sh_nmoves - 1].rdm_ent;

		check(!rp_walk_read(sh->sh_rp_freed), "the walk never read "
		    "the freed %s", sh->sh_name[sh->sh_rp_freed]);
		if (walk) {
			check(st->sps_tombs == 1, "the walk met its "
			    "tombstone and skipped it (%llu)",
			    (u_longlong_t)st->sps_tombs);
		}
		if (st != NULL) {
			check(st->sps_watch_hits == 0, "no scan read touched "
			    "the extents the free released (%llu)",
			    (u_longlong_t)st->sps_watch_hits);
			memset(spike_scan_watch, 0, sizeof (spike_scan_watch));
		}
		check(le->rde_new_birth >= scn->scn_phys.scn_max_txg &&
		    !rp_walk_read(sh->sh_nmoves - 1), "late-L, moved in txg "
		    "%llu, is outside the window (max %llu) and not read",
		    (u_longlong_t)le->rde_new_birth,
		    (u_longlong_t)scn->scn_phys.scn_max_txg);
	}
	if (r5) {
		am = rp_stat(spa, 1);
		(void) printf("MEASURE mirror leaves during the resilver: "
		    "read_bytes=%llu read_ops=%llu processed=%llu healed=%llu "
		    "cksum=%llu\n",
		    (u_longlong_t)(am.rl_rd_bytes - bm.rl_rd_bytes),
		    (u_longlong_t)(am.rl_rd_ops - bm.rl_rd_ops),
		    (u_longlong_t)am.rl_processed, (u_longlong_t)am.rl_healed,
		    (u_longlong_t)(am.rl_cksum - bm.rl_cksum));
		check(am.rl_processed == 0 && am.rl_healed == 0 &&
		    am.rl_cksum == bm.rl_cksum && am.rl_rd_errors == 0,
		    "the resilver repaired nothing on the mirror and found no "
		    "error there");
	}
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* R3, phase 1b: import again; the resilver must resume its walk. */
static int
child_rp_resume(void *arg)
{
	rp_args_t *ra = arg;
	spike_scan_pass_stat_t *st;
	pool_scan_stat_t ps;
	dsl_scan_t *scn;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	rs_set_fix(ra->rp_fix);
	st = rp_walk();
	if (st != NULL)
		rp_set_batch(1);
	spa = import_pool();
	sh->sh_watch = RS_CHILD;
	scn = spa_get_dsl(spa)->dp_scan;
	(void) printf("INFO imported: scan func %llu state %llu\n",
	    (u_longlong_t)scn->scn_phys.scn_func,
	    (u_longlong_t)scn->scn_phys.scn_state);
	if (st != NULL) {
		(void) printf("INFO walk state %llu cursor {%llu, %llx, "
		    "%llu}\n", (u_longlong_t)scn->scn_spike_rdc[0],
		    (u_longlong_t)scn->scn_spike_rdc[1],
		    (u_longlong_t)scn->scn_spike_rdc[2],
		    (u_longlong_t)scn->scn_spike_rdc[3]);
	}
	check(scn->scn_phys.scn_func == POOL_SCAN_RESILVER &&
	    scn->scn_phys.scn_state == DSS_SCANNING &&
	    vdev_dtl_contains(spa->spa_root_vdev->vdev_child[0]->
	    vdev_child[RS_CHILD], DTL_MISSING,
	    sh->sh_rdm[0].rdm_ent.rde_new_birth, 1), "the import finds the "
	    "resilver running and child %d's DTL_MISSING holding the move's "
	    "txg", RS_CHILD);
	rs_wait(spa, rs_offline_done, &ps);
	check(ps.pss_func == POOL_SCAN_RESILVER &&
	    ps.pss_state == DSS_FINISHED && ps.pss_errors == 0 &&
	    rs_offline_done(spa), "the resumed resilver of child %d finished "
	    "with no error and cleared its DTL", RS_CHILD);
	rp_print_walk("resumed resilver");
	check(st != NULL && st->sps_resumed == 1 && st->sps_in_window >= 1 &&
	    sh->sh_rp_window + st->sps_in_window == sh->sh_nmoves,
	    "the walk resumed from its cursor and found the other %llu of "
	    "the %d moved blocks (%llu)", (u_longlong_t)(sh->sh_nmoves -
	    sh->sh_rp_window), sh->sh_nmoves,
	    (u_longlong_t)(st != NULL ? st->sps_in_window : 0));
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* R2, phase 1: move every block with every child online. */
static int
child_rp_move(void *arg)
{
	rp_args_t *ra = arg;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	move_fobjs(spa, ra->rp_policy);
	for (int k = 0; k < 3; k++) {
		dirty_txg(spa);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

static uint64_t
rp_hash(const uint8_t *b, uint64_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	for (uint64_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ULL;
	return (h);
}

/* Read or write <size> bytes at <offset> of child <c>'s file. */
static void
rp_child_io(int c, uint64_t offset, void *buf, uint64_t size,
    boolean_t write)
{
	char path[PATH_BUF];
	ssize_t n;
	int fd;

	child_file(path, sizeof (path), c);
	fd = open(path, write ? O_RDWR : O_RDONLY);
	VERIFY3S(fd, !=, -1);
	n = write ? pwrite(fd, buf, size, offset + VDEV_LABEL_START_SIZE) :
	    pread(fd, buf, size, offset + VDEV_LABEL_START_SIZE);
	VERIFY3S(n, ==, size);
	(void) close(fd);
}

/*
 * R2, phase 2: damage one column of each new extent, on child 2 if it
 * holds one, and keep a hash of what it held.
 */
static int
child_rp_damage1(void *arg)
{
	(void) arg;
	kernel_init(SPA_MODE_READ);
	for (int i = 0; i < sh->sh_nmoves; i++) {
		zio_t zio;
		raidz_map_t *rm = rs_map(&zio,
		    sh->sh_rdm[i].rdm_ent.rde_new_offset,
		    sh->sh_rdm[i].rdm_psize, 2);
		raidz_row_t *rr = rm->rm_row[0];
		const raidz_col_t *rc = NULL;
		uint8_t *buf;

		for (int c = rr->rr_firstdatacol; c < rr->rr_cols; c++) {
			if (rr->rr_col[c].rc_size != 0 && (rc == NULL ||
			    rr->rr_col[c].rc_devidx == RS_CHILD))
				rc = &rr->rr_col[c];
		}
		VERIFY3P(rc, !=, NULL);
		buf = malloc(rc->rc_size);
		rp_child_io(rc->rc_devidx, rc->rc_offset, buf, rc->rc_size,
		    B_FALSE);
		sh->sh_col_child[i] = rc->rc_devidx;
		sh->sh_col_off[i] = rc->rc_offset;
		sh->sh_col_size[i] = rc->rc_size;
		sh->sh_col_hash[i] = rp_hash(buf, rc->rc_size);
		for (uint64_t k = 0; k < rc->rc_size; k++)
			buf[k] = ~buf[k];
		rp_child_io(rc->rc_devidx, rc->rc_offset, buf, rc->rc_size,
		    B_TRUE);
		free(buf);
		(void) printf("INFO %s: damaged its new extent's data column "
		    "on child %d (%llu bytes)\n", sh->sh_name[i],
		    rc->rc_devidx, (u_longlong_t)rc->rc_size);
		rs_map_free(&zio, rm);
	}
	kernel_fini();
	return (0);
}

/*
 * R2, phase 3: a scrub of the txgs from the parity-2 epoch on, which
 * covers every t2 and no t1, must read every new extent and repair it.
 */
static int
child_rp_ranged(void *arg)
{
	rp_args_t *ra = arg;
	spike_scan_pass_stat_t *st;
	rp_leafstat_t b, a;
	pool_scan_stat_t ps;
	uint64_t t0;
	spa_t *spa;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	rs_set_fix(ra->rp_fix);
	st = rp_walk();
	spa = import_pool();
	txg_wait_synced(spa_get_dsl(spa), spa_first_txg(spa) + 5);
	b = rp_stat(spa, -1);
	t0 = spa_last_synced_txg(spa);
	err = spa_scan_range(spa, POOL_SCAN_SCRUB, sh->sh_epoch_txg, 0, 0);
	check(err == 0, "a scrub of txgs (%llu, now) starts (%d)",
	    (u_longlong_t)sh->sh_epoch_txg, err);
	for (int n = 0; n < RS_TXGS && dsl_scan_scrubbing(spa_get_dsl(spa));
	    n++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	VERIFY0(spa_scan_get_stats(spa, &ps));
	a = rp_stat(spa, -1);
	rp_measure(spa, "ranged scrub", &ps, &b, &a,
	    spa_last_synced_txg(spa) - t0);
	rp_print_walk("ranged scrub");
	check(ps.pss_state == DSS_FINISHED && ps.pss_errors == 0,
	    "the ranged scrub finished with no error");
	check(a.rl_cksum - b.rl_cksum >= sh->sh_nmoves, "it found the %d "
	    "damaged columns (%llu checksum errors)", sh->sh_nmoves,
	    (u_longlong_t)(a.rl_cksum - b.rl_cksum));
	if (st != NULL && ra->rp_fix != 2 && ra->rp_fix != 3) {
		check(st->sps_in_window == sh->sh_nmoves &&
		    st->sps_issued == sh->sh_nmoves, "the walk read the %d "
		    "moved blocks (%llu in the window, %llu read)",
		    sh->sh_nmoves, (u_longlong_t)st->sps_in_window,
		    (u_longlong_t)st->sps_issued);
	}
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* R2, phase 4: every damaged column must hold what it held before. */
static int
child_rp_restored(void *arg)
{
	(void) arg;
	int bad = 0;

	for (int i = 0; i < sh->sh_nmoves; i++) {
		uint8_t *buf = malloc(sh->sh_col_size[i]);
		boolean_t ok;

		rp_child_io(sh->sh_col_child[i], sh->sh_col_off[i], buf,
		    sh->sh_col_size[i], B_FALSE);
		ok = rp_hash(buf, sh->sh_col_size[i]) == sh->sh_col_hash[i];
		(void) printf("INFO %s: column on child %d %s\n",
		    sh->sh_name[i], sh->sh_col_child[i],
		    ok ? "restored" : "still damaged");
		bad += !ok;
		free(buf);
	}
	check(bad == 0, "the scrub rebuilt every damaged column of the new "
	    "extents (%d still damaged)", bad);
	return (failures == 0 ? 0 : 1);
}

/* Phase 2 of R1, R3, R4, R5: a scrub must find nothing to repair. */
static int
child_rp_scrub(void *arg)
{
	(void) arg;
	rp_leafstat_t b, a;
	pool_scan_stat_t ps;
	uint64_t t0;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = import_pool();
	b = rp_stat(spa, -1);
	t0 = spa_last_synced_txg(spa);
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	for (int n = 0; n < RS_TXGS && dsl_scan_scrubbing(spa_get_dsl(spa));
	    n++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	VERIFY0(spa_scan_get_stats(spa, &ps));
	a = rp_stat(spa, -1);
	rp_measure(spa, "scrub", &ps, &b, &a, spa_last_synced_txg(spa) - t0);
	check(ps.pss_func == POOL_SCAN_SCRUB && ps.pss_state == DSS_FINISHED &&
	    ps.pss_errors == 0 && ps.pss_processed == 0 &&
	    a.rl_cksum == b.rl_cksum && a.rl_processed == 0,
	    "a scrub after the resilver finds nothing to repair (errors %llu, "
	    "repaired %llu bytes, %llu checksum errors)",
	    (u_longlong_t)ps.pss_errors, (u_longlong_t)ps.pss_processed,
	    (u_longlong_t)(a.rl_cksum - b.rl_cksum));
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/*
 * Phase 3 of R1, R3, R4, R5, on a copy of the pool's files taken after the
 * resilver: damage one column of each live new extent on a child other
 * than 2 and RP_X, take RP_X offline, and read every object.  Parity 2
 * covers two lost columns; with a third, stale on child 2, a block whose
 * new extent has columns on all three would not read back.
 */
static int
child_rp_read(void *arg)
{
	(void) arg;
	int damaged = 0;
	spa_t *spa;
	vdev_t *xvd;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	for (int i = 0; i < sh->sh_nmoves; i++) {
		zio_t zio;
		raidz_map_t *rm;
		raidz_row_t *rr;
		const raidz_col_t *rc = NULL;
		uint8_t *buf;

		if (sh->sh_rdm[i].rdm_error != 0 ||
		    sh->sh_fobj[sh->sh_owner[i]].fo_gone)
			continue;
		rm = rs_map(&zio, sh->sh_rdm[i].rdm_ent.rde_new_offset,
		    sh->sh_rdm[i].rdm_psize, 2);
		rr = rm->rm_row[0];
		for (int c = 0; c < rr->rr_cols && rc == NULL; c++) {
			if (rr->rr_col[c].rc_size != 0 &&
			    rr->rr_col[c].rc_devidx != RS_CHILD &&
			    rr->rr_col[c].rc_devidx != RP_X)
				rc = &rr->rr_col[c];
		}
		if (rc != NULL) {
			buf = malloc(rc->rc_size);
			rs_col_io(rc, buf, B_FALSE);
			for (uint64_t k = 0; k < rc->rc_size; k++)
				buf[k] = ~buf[k];
			rs_col_io(rc, buf, B_TRUE);
			free(buf);
			damaged++;
		}
		rs_map_free(&zio, rm);
	}
	(void) printf("INFO damaged one column, not on child %d or %d, of %d "
	    "new extents of the copy\n", RS_CHILD, RP_X, damaged);
	spa = import_pool();
	xvd = spa->spa_root_vdev->vdev_child[0]->vdev_child[RP_X];
	VERIFY0(vdev_offline(spa, xvd->vdev_guid, 0));
	(void) printf("INFO child %d of the copy offline (state %d)\n", RP_X,
	    (int)xvd->vdev_state);
	check_others_read();
	export_pool(spa);
	kernel_fini();
	return (failures == 0 ? 0 : 1);
}

/* Copy the pool's files to rp_copy_dir, sparse. */
static void
rp_copy(void)
{
	char cmd[PATH_BUF * 3];
	int st;

	(void) snprintf(cmd, sizeof (cmd), "mkdir -p %s && for f in %s/v? "
	    "%s/m?; do [ -e \"$f\" ] && cp --sparse=always \"$f\" %s/; "
	    "done; du -sh %s | sed 's/^/INFO copy: /'", rp_copy_dir, dir, dir,
	    rp_copy_dir, rp_copy_dir);
	(void) fflush(stdout);
	st = system(cmd);
	VERIFY0(st);
}

static int
run_rpass(const char *test, policy_t policy, int fix)
{
	rp_args_t ra = { test, policy, fix };
	char copy[PATH_BUF];
	const char *orig = dir;
	int st;

	(void) printf("INFO rpass %s, policy %s, ashift %llu, "
	    "spike_resilver_redirects %d\n", test, policy_names[policy],
	    (u_longlong_t)ashift, fix);
	if (vdev_raidz_redirect_move == NULL)
		return (2);
	sh->sh_small = B_TRUE;
	sh->sh_bulk = RP_BULK;
	sh->sh_mirror = strcmp(test, "R5") == 0;
	sh->sh_late = strcmp(test, "R4") == 0;
	VERIFY0(phase("0 setup", setup_free_fn, NULL));

	if (strcmp(test, "R2") == 0) {
		st = phase("1 move", child_rp_move, &ra);
		check(st == 0, "the move phase passes (status %d)", st);
		VERIFY0(phase("2 damage", child_rp_damage1, NULL));
		st = phase("3 ranged scrub", child_rp_ranged, &ra);
		check(st == 0, "the ranged scrub phase passes (status %d)", st);
		st = phase("4 restored", child_rp_restored, NULL);
		check(st == 0, "the columns phase passes (status %d)", st);
		run_zdb();
		return (0);
	}
	st = phase("1 move and resilver", child_rp_resilver, &ra);
	check(st == 0, "the move and resilver phase passes (status %d)", st);
	if (strcmp(test, "R6") == 0) {
		/* plain-A's new extent is not rebuilt: no scrub or read. */
		run_zdb();
		return (0);
	}
	if (strcmp(test, "R3") == 0) {
		st = phase("1b resume", child_rp_resume, &ra);
		check(st == 0, "the resume phase passes (status %d)", st);
	}
	run_zdb();
	(void) snprintf(copy, sizeof (copy), "%s/copy", orig);
	rp_copy_dir = copy;
	rp_copy();
	st = phase("2 scrub", child_rp_scrub, &ra);
	check(st == 0, "the scrub phase passes (status %d)", st);
	dir = copy;
	st = phase("3 read with a second child offline", child_rp_read, &ra);
	dir = orig;
	check(st == 0, "the read phase passes (status %d)", st);
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
	    "<TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [split|condense]\n"
	    "       crash_probe <dir> <ashift> guard "
	    "<mos|zil|stale|checkpoint|wrongzb|spill|spillblk|xbook>\n"
	    "       crash_probe <dir> <ashift> free "
	    "<plain|dedup|clone|gang|snap> <P1|P2> | free plain P3 "
	    "[nocheck]\n"
	    "       crash_probe <dir> <ashift> fcrash "
	    "<TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> "
	    "[late|condense|condensebad]\n"
	    "       crash_probe <dir> <ashift> converge <P1|P2> [nolog]\n"
	    "       crash_probe <dir> <ashift> resilver <P1|P2> "
	    "<offline|offlinescrub|control|replace> [nofix|nowiden|nocols]\n"
	    "       crash_probe <dir> <ashift> stalebp <P1|P2> [same] "
	    "[nolive]\n"
	    "       crash_probe <dir> <ashift> corrupt <kind> [P1|P2] [lax]\n"
	    "       crash_probe <dir> <ashift> tomb <read|repair>\n"
	    "       crash_probe <dir> <ashift> rpass <R1|R2|R3|R4|R5|R6> "
	    "<P1|P2> [none|stopgap|pass|both|noverify]\n");
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
		if (point == -1 || policy == -1 || (opt != NULL &&
		    strcmp(opt, "split") != 0 && strcmp(opt, "condense") != 0))
			return (usage());
		if (opt != NULL && strcmp(opt, "condense") == 0) {
			if (&spike_redirect_condense == NULL) {
				(void) printf("SKIP: no log to condense\n");
				return (2);
			}
			/* The log is rewritten in the txg of the move. */
			spike_redirect_condense = 1;
			sh->sh_condense = 1;
			(void) printf("INFO spike_redirect_condense=1\n");
		}
		st = run_move(point, policy, opt != NULL &&
		    strcmp(opt, "split") == 0);
	} else if (strcmp(mode, "guard") == 0 && argc == 5 &&
	    (strcmp(argv[4], "mos") == 0 || strcmp(argv[4], "zil") == 0 ||
	    strcmp(argv[4], "stale") == 0 ||
	    strcmp(argv[4], "checkpoint") == 0 ||
	    strcmp(argv[4], "wrongzb") == 0 ||
	    strcmp(argv[4], "spill") == 0 ||
	    strcmp(argv[4], "spillblk") == 0 ||
	    strcmp(argv[4], "xbook") == 0)) {
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
		int condense = opt == NULL ? 0 :
		    strcmp(opt, "condense") == 0 ? 1 :
		    strcmp(opt, "condensebad") == 0 ? 2 : 0;

		point = parse_point(argv[4]);
		policy = parse_policy(argv[5], B_FALSE);
		if (point == -1 || policy == -1 || (opt != NULL &&
		    strcmp(opt, "late") != 0 && condense == 0))
			return (usage());
		st = run_fcrash(point, policy, opt != NULL && condense == 0,
		    condense);
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
	} else if (strcmp(mode, "corrupt") == 0 && argc <= 7 &&
	    corrupt_kind_ok(argv[4])) {
		policy = argc >= 6 ? parse_policy(argv[5], B_FALSE) : P2;
		if (policy == -1)
			return (usage());
		/* The load without its checks of the log's structure. */
		if (argc == 7 && strcmp(argv[6], "lax") == 0 &&
		    &spike_redirect_load_lax != NULL) {
			spike_redirect_load_lax = 1;
			(void) printf("INFO spike_redirect_load_lax=1\n");
		} else if (argc == 7) {
			return (usage());
		}
		st = run_corrupt(argv[4], policy);
	} else if (strcmp(mode, "tomb") == 0 && argc == 5 &&
	    (strcmp(argv[4], "read") == 0 || strcmp(argv[4], "repair") == 0)) {
		st = run_tomb(argv[4]);
	} else if (strcmp(mode, "rpass") == 0 && argc >= 6 &&
	    (strcmp(argv[4], "R1") == 0 || strcmp(argv[4], "R2") == 0 ||
	    strcmp(argv[4], "R3") == 0 || strcmp(argv[4], "R4") == 0 ||
	    strcmp(argv[4], "R5") == 0 || strcmp(argv[4], "R6") == 0)) {
		/* Bits of spike_resilver_redirects, -1 to keep the default. */
		static const char *const variants[] = { "none", "stopgap",
		    "pass", "both" };
		static const int bits[] = { 2, 3, 6, 7 };
		int fix = -1;

		policy = parse_policy(argv[5], B_FALSE);
		if (policy == -1)
			return (usage());
		for (int v = 0; opt != NULL && v < 4; v++) {
			if (strcmp(opt, variants[v]) == 0)
				fix = bits[v];
		}
		/* R6: no check of the stored block pointer. */
		if (opt != NULL && strcmp(opt, "noverify") == 0 &&
		    &spike_scan_redirect_verify != NULL) {
			spike_scan_redirect_verify = 0;
			(void) printf("INFO spike_scan_redirect_verify=0\n");
		} else if (opt != NULL && fix == -1) {
			return (usage());
		}
		st = run_rpass(argv[4], policy, fix);
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
