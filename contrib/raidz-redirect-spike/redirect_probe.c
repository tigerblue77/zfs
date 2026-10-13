// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 3 (throwaway, not part of any tree): can a block of an old
 * parity epoch be re-encoded at the new parity into a new extent and served
 * from there, with its block pointer left untouched?
 *
 *   redirect_probe <dir> <ashift>
 *
 * Needs a libzpool built with the SPIKE redirect table in vdev_raidz.c
 * (vdev_raidz_redirect_move()).  Creates a five-wide raidz1 pool of files in
 * <dir>, writes plain, dedup and gang blocks, moves the vdev to a parity-2
 * epoch, and re-encodes some of the old blocks into new extents: the probe
 * reads their raw bytes with the old layout, and the kernel's test-only move
 * allocates N, writes the bytes there with the new layout and adds the
 * persistent redirects, in one txg.  Then it checks:
 *
 *   T1  every redirected block reads back bit-identical through its old,
 *       unchanged block pointer, and only through the redirect
 *   T2  two corrupted columns of a redirected block are reconstructed and
 *       repaired in the new extent (parity 2)
 *   T3  three corrupted columns are not; the control, an old parity-1 block
 *       with two corrupted columns, is not either
 *   T4  a scrub repairs a corrupted column of a plain and of a dedup block
 *   T5  a redirected gang header (and its children) read and self-heal
 *   T6  the old extents are byte for byte what they were before any
 *       redirect existed
 *   T7  with read errors injected on two children a redirected block still
 *       reads back, with three it does not, and an old parity-1 block does
 *       not with two
 *
 * Spike step 4: what may the move do with the old extent [O, O + A) of a
 * moved block?  The policy is applied in the move's own sync task: P1 keeps
 * it whole (the checks above always run with P1), P2 keeps its first sector
 * and frees the rest, P3 frees it whole.
 *
 *   redirect_probe <dir> <ashift> brt <P1|P2|P3> [alone]
 *
 *   T4a a block X cloned into a second object is moved, a new block Y is
 *       steered onto X's old extent (one allocator, best fit), then freed.
 *       Y must not start at O, X must keep its clone reference, Y's extent
 *       must be free again and both objects of X must read back.  A second,
 *       unmoved cloned block keeps the vdev's BRT in use, unless 'alone'.
 *
 *   redirect_probe <dir> <ashift> scan <P1|P2|P3> [nofix]
 *
 *   T4b blocks X are moved, new blocks Y are steered onto the freed part of
 *       their old extents, and the pool is scrubbed with the sorted scan.
 *       The scrub must finish, issue every byte it examined, repair a bad
 *       column of a moved and of a new block and find no error.  'nofix'
 *       turns off the spike's routing of redirected blocks to the unsorted
 *       path (spike_scan_redirect_unsorted in dsl_scan.c).
 *
 * Prints one PASS or FAIL line per check and exits 1 if any check failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/bpobj.h>
#include <sys/brt.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/range_tree.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"rdp"
#define	WIDTH		5
#define	FILE_SIZE	(256ULL << 20)
#define	BLKSZ		(128ULL << 10)
#define	PATH_BUF	512

extern uint64_t metaslab_force_ganging;
extern uint_t metaslab_force_ganging_pct;
/* Weak: absent from a libzpool without the spike's scan routing. */
extern int spike_scan_redirect_unsorted __attribute__((weak));
/* Weak: absent from a libzpool that does not gate P3 behind it. */
extern int spike_redirect_keep_none __attribute__((weak));

/* What the move does with the old extent of a moved block. */
typedef enum policy { P1, P2, P3 } policy_t;
static const char *const policy_names[] = { "P1", "P2", "P3" };

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

/* Reopen the pool, so that no cache holds a block that is read next. */
static spa_t *
cycle_pool(spa_t *spa)
{
	export_pool(spa);
	return (import_pool());
}

/* An object of one incompressible block of <size> bytes. */
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
read_object_size(uint64_t obj, uint64_t seed, uint64_t size,
    boolean_t *match)
{
	objset_t *os;
	uint64_t *buf = malloc(size), *want = malloc(size);
	int err;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	fill_pattern(want, size, seed);
	memset(buf, 0, size);
	err = dmu_read(os, obj, 0, size, buf, DMU_READ_NO_PREFETCH);
	*match = (err == 0 && memcmp(buf, want, size) == 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(buf);
	free(want);
	return (err);
}

static int
read_object(spa_t *spa, uint64_t obj, uint64_t seed, boolean_t *match)
{
	(void) spa;
	return (read_object_size(obj, seed, BLKSZ, match));
}

/*
 * One extent of the vdev as laid out with <parity> parity columns: the map
 * that the I/O path builds for it, with column offsets on the children.
 */
typedef struct extent {
	uint64_t	ex_offset;
	uint64_t	ex_psize;
	uint64_t	ex_parity;
	zio_t		ex_zio;
	raidz_map_t	*ex_rm;
	uint8_t		**ex_saved;	/* every column's bytes */
} extent_t;

static void
col_io(raidz_col_t *rc, void *buf, boolean_t write)
{
	char path[PATH_BUF];
	int fd;
	ssize_t n;

	leaf_path(path, sizeof (path), rc->rc_devidx);
	fd = open(path, write ? O_RDWR : O_RDONLY);
	VERIFY3S(fd, !=, -1);
	if (write)
		n = pwrite(fd, buf, rc->rc_size,
		    rc->rc_offset + VDEV_LABEL_START_SIZE);
	else
		n = pread(fd, buf, rc->rc_size,
		    rc->rc_offset + VDEV_LABEL_START_SIZE);
	VERIFY3S(n, ==, rc->rc_size);
	(void) close(fd);
}

static void
extent_map(extent_t *ex, uint64_t offset, uint64_t psize, uint64_t parity)
{
	memset(ex, 0, sizeof (*ex));
	ex->ex_offset = offset;
	ex->ex_psize = psize;
	ex->ex_parity = parity;
	ex->ex_zio.io_size = P2ROUNDUP_TYPED(psize, 1ULL << ashift, uint64_t);
	ex->ex_zio.io_offset = offset;
	ex->ex_zio.io_type = ZIO_TYPE_READ;
	ex->ex_zio.io_abd = abd_alloc_for_io(ex->ex_zio.io_size, B_FALSE);
	ex->ex_rm = vdev_raidz_map_alloc(&ex->ex_zio, ashift, WIDTH, parity);
	VERIFY3S(ex->ex_rm->rm_nrows, ==, 1);
}

static void
extent_save(extent_t *ex)
{
	raidz_row_t *rr = ex->ex_rm->rm_row[0];

	ex->ex_saved = calloc(rr->rr_cols, sizeof (uint8_t *));
	for (int c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];

		if (rc->rc_size == 0)
			continue;
		ex->ex_saved[c] = malloc(rc->rc_size);
		col_io(rc, ex->ex_saved[c], B_FALSE);
	}
}

/* B_TRUE if every column holds what extent_save() saw. */
static boolean_t
extent_same(extent_t *ex)
{
	raidz_row_t *rr = ex->ex_rm->rm_row[0];
	boolean_t same = B_TRUE;

	for (int c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		uint8_t *now;

		if (rc->rc_size == 0)
			continue;
		now = malloc(rc->rc_size);
		col_io(rc, now, B_FALSE);
		if (memcmp(now, ex->ex_saved[c], rc->rc_size) != 0)
			same = B_FALSE;
		free(now);
	}
	return (same);
}

/* Invert the bytes of data column <d>; return its child. */
static int
extent_corrupt(extent_t *ex, int d)
{
	raidz_row_t *rr = ex->ex_rm->rm_row[0];
	raidz_col_t *rc = &rr->rr_col[rr->rr_firstdatacol + d];
	uint8_t *buf = malloc(rc->rc_size);

	VERIFY3S(d, <, rr->rr_cols - rr->rr_firstdatacol);
	col_io(rc, buf, B_FALSE);
	for (uint64_t b = 0; b < rc->rc_size; b++)
		buf[b] = ~buf[b];
	col_io(rc, buf, B_TRUE);
	free(buf);
	return (rc->rc_devidx);
}

/*
 * The raw bytes of a block as the old layout stores them: the same DVA and
 * birth, read with no checksum and no transform, so that a gang header comes
 * back as the header itself.
 */
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

typedef struct move {
	const char	*mv_name;
	blkptr_t	mv_bp;		/* the block pointer, never changed */
	zbookmark_phys_t mv_zb;		/* where it is referenced from */
	uint64_t	mv_psize;	/* bytes at the DVA: psize or header */
	abd_t		*mv_data;	/* their raw contents */
	uint64_t	mv_new_offset;
	uint64_t	mv_new_birth;
	uint64_t	mv_new_asize;
	uint64_t	mv_old_asize;	/* the old extent is [O, O + this) */
	uint64_t	mv_kept;	/* the policy keeps [O, O + this) */
	extent_t	mv_old;
	extent_t	mv_new;
} move_t;

/*
 * Move the blocks with the kernel's test-only move, in one sync task of a txg
 * of the new epoch, which also applies the policy to the old extents.  With
 * <far> not 0, N is claimed from there on instead of allocated.
 */
static void
run_moves(spa_t *spa, move_t *moves, int n, policy_t policy, uint64_t far)
{
	static const raidz_redirect_keep_t keep[] = {
		RAIDZ_REDIRECT_KEEP_ALL, RAIDZ_REDIRECT_KEEP_FIRST,
		RAIDZ_REDIRECT_KEEP_NONE
	};
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	raidz_redirect_move_t *rdm = calloc(n, sizeof (*rdm));

	for (int i = 0; i < n; i++) {
		rdm[i].rdm_bp = &moves[i].mv_bp;
		rdm[i].rdm_zb = &moves[i].mv_zb;
		rdm[i].rdm_psize = moves[i].mv_psize;
		rdm[i].rdm_data = moves[i].mv_data;
		if (far != 0) {
			/* The move's txg is in the last epoch. */
			rdm[i].rdm_place = far;
			far += vdev_psize_to_asize_txg(tvd, moves[i].mv_psize,
			    UINT64_MAX);
		}
	}
	VERIFY0(vdev_raidz_redirect_move(spa, 0, rdm, n, keep[policy],
	    B_FALSE));
	for (int i = 0; i < n; i++) {
		move_t *mv = &moves[i];

		VERIFY0(rdm[i].rdm_error);
		mv->mv_new_offset = rdm[i].rdm_ent.rde_new_offset;
		mv->mv_new_birth = rdm[i].rdm_ent.rde_new_birth;
		mv->mv_new_asize = rdm[i].rdm_ent.rde_new_asize;
		mv->mv_old_asize = rdm[i].rdm_old_asize;
		mv->mv_kept = rdm[i].rdm_ent.rde_old_reserved;
	}
	free(rdm);
	txg_wait_synced(spa_get_dsl(spa), 0);
}

/* Name a plain (not gang) block to move and read its raw bytes. */
static void
move_prepare(spa_t *spa, move_t *mv, const char *name, const blkptr_t *bp,
    objset_t *os, uint64_t obj)
{
	memset(mv, 0, sizeof (*mv));
	VERIFY(!BP_IS_GANG(bp));
	mv->mv_name = name;
	mv->mv_bp = *bp;
	SET_BOOKMARK(&mv->mv_zb, dmu_objset_id(os), obj, 0, 0);
	mv->mv_psize = BP_GET_PSIZE(bp);
	mv->mv_data = read_raw(spa, bp, mv->mv_psize);
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

/* The parity-2 epoch, from the first txg it may start in. */
static uint64_t
add_parity2_epoch(spa_t *spa)
{
	epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

	VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
	    ZFS_SPACE_CHECK_NONE));
	txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
	(void) printf("INFO parity 2 from txg %llu\n",
	    (u_longlong_t)ea.ea_table[3]);
	return (ea.ea_table[3]);
}

/*
 * Wait until the pool's pending frees have been issued and have left the
 * defer trees, so that what they freed is allocatable again.
 */
static void
wait_frees(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);

	for (int i = 0; i < 100 && !bpobj_is_empty(&dp->dp_free_bpobj); i++)
		txg_wait_synced(dp, 0);
	VERIFY(bpobj_is_empty(&dp->dp_free_bpobj));
	txg_wait_synced(dp, 0);
	txg_wait_synced(dp, 0);
}

/* B_TRUE if [offset, offset + size) of vdev 0 is free and allocatable. */
static boolean_t
range_allocatable(spa_t *spa, uint64_t offset, uint64_t size)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	metaslab_t *msp = tvd->vdev_ms[offset >> tvd->vdev_ms_shift];
	boolean_t ok;

	mutex_enter(&msp->ms_lock);
	VERIFY0(metaslab_load(msp));
	ok = zfs_range_tree_contains(msp->ms_allocatable, offset, size);
	mutex_exit(&msp->ms_lock);
	return (ok);
}

/* The free segments of vdev 0's metaslab <m> below <limit>. */
static void
print_free(spa_t *spa, uint64_t m, uint64_t limit)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	metaslab_t *msp = tvd->vdev_ms[m];
	zfs_range_tree_t *rt = msp->ms_allocatable;
	zfs_btree_index_t where;

	mutex_enter(&msp->ms_lock);
	VERIFY0(metaslab_load(msp));
	(void) printf("INFO free below %llx in metaslab %llu:",
	    (u_longlong_t)limit, (u_longlong_t)m);
	for (zfs_range_seg_t *rs = zfs_btree_first(&rt->rt_root, &where);
	    rs != NULL && zfs_rs_get_start(rs, rt) < limit;
	    rs = zfs_btree_next(&rt->rt_root, &where, &where)) {
		(void) printf(" [%llx,+%llu)",
		    (u_longlong_t)zfs_rs_get_start(rs, rt),
		    (u_longlong_t)(zfs_rs_get_end(rs, rt) -
		    zfs_rs_get_start(rs, rt)));
	}
	(void) printf("\n");
	mutex_exit(&msp->ms_lock);
}

static void
print_move(spa_t *spa, const move_t *mv)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];

	(void) printf("INFO %s: O=%llx t1=%llu old asize=%llu kept=%llu "
	    "-> N=%llx t2=%llu new asize=%llu (metaslabs %llu -> %llu)\n",
	    mv->mv_name, (u_longlong_t)DVA_GET_OFFSET(&mv->mv_bp.blk_dva[0]),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&mv->mv_bp),
	    (u_longlong_t)mv->mv_old_asize, (u_longlong_t)mv->mv_kept,
	    (u_longlong_t)mv->mv_new_offset, (u_longlong_t)mv->mv_new_birth,
	    (u_longlong_t)mv->mv_new_asize,
	    (u_longlong_t)(DVA_GET_OFFSET(&mv->mv_bp.blk_dva[0]) >>
	    tvd->vdev_ms_shift),
	    (u_longlong_t)(mv->mv_new_offset >> tvd->vdev_ms_shift));
}

static uint64_t
child_cksum_errors(spa_t *spa, int c)
{
	vdev_t *cvd = spa->spa_root_vdev->vdev_child[0]->vdev_child[c];

	return (cvd->vdev_stat.vs_checksum_errors);
}

static void
reset_errors(spa_t *spa)
{
	for (int c = 0; c < WIDTH; c++) {
		vdev_t *cvd = spa->spa_root_vdev->vdev_child[0]->vdev_child[c];

		cvd->vdev_stat.vs_checksum_errors = 0;
		cvd->vdev_stat.vs_read_errors = 0;
	}
}


/* Fail every read of the given children; return the injection ids. */
static void
fail_reads(const int *children, int n, int *ids, spa_t *spa)
{
	for (int i = 0; i < n; i++) {
		zinject_record_t rec;

		memset(&rec, 0, sizeof (rec));
		rec.zi_cmd = ZINJECT_DEVICE_FAULT;
		rec.zi_guid = spa->spa_root_vdev->vdev_child[0]->
		    vdev_child[children[i]]->vdev_guid;
		/* fail before the read, so that no data arrives */
		rec.zi_error = EFAULT;
		rec.zi_iotype = ZINJECT_IOTYPE_READ;
		VERIFY0(zio_inject_fault((char *)POOL, 0, &ids[i], &rec));
	}
}

static void
clear_faults(const int *ids, int n)
{
	for (int i = 0; i < n; i++)
		VERIFY0(zio_clear_fault(ids[i]));
}

/*
 * Read an object whose dnode has been loaded first, so that the only block
 * read while the faults are in place is the object's data block.
 */
static int
read_object_faulted(uint64_t obj, uint64_t seed, const int *children,
    int n, spa_t *spa, boolean_t *match)
{
	objset_t *os;
	dmu_object_info_t doi;
	uint64_t *buf = malloc(BLKSZ), *want = malloc(BLKSZ);
	int ids[WIDTH];
	int err;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	VERIFY0(dmu_object_info(os, obj, &doi));
	fill_pattern(want, BLKSZ, seed);
	memset(buf, 0, BLKSZ);
	fail_reads(children, n, ids, spa);
	err = dmu_read(os, obj, 0, BLKSZ, buf, DMU_READ_NO_PREFETCH);
	for (int i = 0; i < n; i++) {
		int id = ids[i] - 1;
		char name[MAXNAMELEN];
		zinject_record_t rec;

		if (zio_inject_list_next(&id, name, sizeof (name), &rec) == 0)
			(void) printf("INFO fault %d on child %d: "
			    "matched %llu, injected %llu\n", ids[i],
			    children[i],
			    (u_longlong_t)rec.zi_match_count,
			    (u_longlong_t)rec.zi_inject_count);
	}
	clear_faults(ids, n);
	(void) printf("INFO read errors by child:");
	for (int c = 0; c < WIDTH; c++) {
		(void) printf(" %llu", (u_longlong_t)spa->spa_root_vdev->
		    vdev_child[0]->vdev_child[c]->vdev_stat.vs_read_errors);
	}
	(void) printf(" err=%d\n", err);
	*match = (err == 0 && memcmp(buf, want, BLKSZ) == 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(buf);
	free(want);
	return (err);
}

/* The children of the first <n> data columns of an extent. */
static void
data_children(extent_t *ex, int n, int *children)
{
	raidz_row_t *rr = ex->ex_rm->rm_row[0];

	for (int i = 0; i < n; i++)
		children[i] = rr->rr_col[rr->rr_firstdatacol + i].rc_devidx;
}

/* Spike step 3: the read path, with the old extents kept whole (P1). */
static void
run_classic(void)
{
	enum { A, B, S, D, G, E, NMOVED, CTL = NMOVED, CTL2, NOBJ };
	static const char *names[] = { "plain-A", "plain-B", "plain-S",
	    "dedup-D", "gang-G", "plain-E", "control", "control-2" };
	uint64_t obj[NOBJ], seed[NOBJ] = { 1, 2, 4, 5, 6, 7, 3, 8 };
	move_t moves[NMOVED + 64];
	int nmoves = 0;
	extent_t ctl_old, ctl2_old;
	blkptr_t ctl_bp, ctl2_bp;
	objset_t *os;
	spa_t *spa;
	boolean_t match;
	uint64_t hits;
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));	/* spa_create() imported it */

	/* Blocks of the parity-1 epoch. */
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < NOBJ; i++) {
		if (i != D && i != G)
			obj[i] = put_object(os, seed[i]);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);

	VERIFY0(dsl_prop_set_int(POOL, "dedup", ZPROP_SRC_LOCAL,
	    ZIO_CHECKSUM_SHA256));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	obj[D] = put_object(os, seed[D]);
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dsl_prop_set_int(POOL, "dedup", ZPROP_SRC_LOCAL,
	    ZIO_CHECKSUM_OFF));

	/* Gang the 128K block, not its children. */
	metaslab_force_ganging = 100 << 10;
	metaslab_force_ganging_pct = 100;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	obj[G] = put_object(os, seed[G]);
	txg_wait_synced(spa_get_dsl(spa), 0);
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;

	for (int i = 0; i < NMOVED; i++) {
		moves[nmoves].mv_name = names[i];
		moves[nmoves].mv_bp = object_bp(os, obj[i]);
		SET_BOOKMARK(&moves[nmoves].mv_zb, dmu_objset_id(os), obj[i],
		    0, 0);
		nmoves++;
	}
	ctl_bp = object_bp(os, obj[CTL]);
	ctl2_bp = object_bp(os, obj[CTL2]);
	dmu_objset_disown(os, B_TRUE, FTAG);

	check(BP_GET_DEDUP(&moves[D].mv_bp), "dedup-D has a dedup bp");
	check(BP_IS_GANG(&moves[G].mv_bp), "gang-G has a gang bp");
	for (int i = 0; i < nmoves; i++) {
		blkptr_t *bp = &moves[i].mv_bp;

		(void) printf("INFO %s offset=%llx birth=%llu psize=%llu "
		    "asize=%llu%s\n", moves[i].mv_name,
		    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
		    (u_longlong_t)BP_GET_PSIZE(bp),
		    (u_longlong_t)DVA_GET_ASIZE(&bp->blk_dva[0]),
		    BP_IS_GANG(bp) ? " gang" : "");
	}

	/* What the old extents hold, and the gang header and its children. */
	for (int i = 0; i < NMOVED; i++) {
		move_t *mv = &moves[i];

		if (BP_IS_GANG(&mv->mv_bp)) {
			vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];

			mv->mv_psize = spa_feature_is_active(spa,
			    SPA_FEATURE_DYNAMIC_GANG_HEADER) ?
			    vdev_gang_header_psize(tvd) : SPA_OLD_GANGBLOCKSIZE;
		} else {
			mv->mv_psize = BP_GET_PSIZE(&mv->mv_bp);
		}
		mv->mv_data = read_raw(spa, &mv->mv_bp, mv->mv_psize);
	}
	{
		move_t *g = &moves[G];
		zio_gbh_phys_t *gbh = abd_to_buf(g->mv_data);

		for (int k = 0; k < gbh_nblkptrs(g->mv_psize); k++) {
			blkptr_t *cbp = gbh_bp(gbh, k);
			move_t *mv = &moves[nmoves];

			if (BP_IS_HOLE(cbp))
				continue;
			VERIFY(!BP_IS_GANG(cbp));
			mv->mv_name = "gang-G child";
			mv->mv_bp = *cbp;
			mv->mv_zb = g->mv_zb;
			mv->mv_psize = BP_GET_PSIZE(cbp);
			mv->mv_data = read_raw(spa, cbp, mv->mv_psize);
			nmoves++;
		}
		(void) printf("INFO gang-G header %llu bytes, %d children\n",
		    (u_longlong_t)g->mv_psize, nmoves - NMOVED);
	}
	for (int i = 0; i < nmoves; i++) {
		move_t *mv = &moves[i];

		extent_map(&mv->mv_old, DVA_GET_OFFSET(&mv->mv_bp.blk_dva[0]),
		    mv->mv_psize, 1);
		extent_save(&mv->mv_old);
	}
	extent_map(&ctl_old, DVA_GET_OFFSET(&ctl_bp.blk_dva[0]),
	    BP_GET_PSIZE(&ctl_bp), 1);
	extent_map(&ctl2_old, DVA_GET_OFFSET(&ctl2_bp.blk_dva[0]),
	    BP_GET_PSIZE(&ctl2_bp), 1);

	(void) add_parity2_epoch(spa);

	/* Re-encode and redirect, keeping the old extents. */
	run_moves(spa, moves, nmoves, P1, 0);
	for (int i = 0; i < nmoves; i++) {
		move_t *mv = &moves[i];
		vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
		uint64_t parity;

		vdev_raidz_layout_for_birth(tvd, mv->mv_new_birth, NULL,
		    &parity);
		(void) printf("INFO %s -> offset=%llx birth=%llu asize=%llu "
		    "(was %llu) parity=%llu\n", mv->mv_name,
		    (u_longlong_t)mv->mv_new_offset,
		    (u_longlong_t)mv->mv_new_birth,
		    (u_longlong_t)mv->mv_new_asize,
		    (u_longlong_t)vdev_psize_to_asize_txg(tvd, mv->mv_psize,
		    BP_GET_PHYSICAL_BIRTH(&mv->mv_bp)),
		    (u_longlong_t)parity);
		extent_map(&mv->mv_new, mv->mv_new_offset, mv->mv_psize, 2);
		extent_save(&mv->mv_new);
	}

	/* T1: everything reads back through the old block pointers. */
	spa = cycle_pool(spa);
	reset_errors(spa);
	hits = vdev_raidz_redirect_hits();
	for (int i = 0; i < NMOVED; i++) {
		err = read_object(spa, obj[i], seed[i], &match);
		check(err == 0 && match, "T1 %s reads back (err=%d match=%d)",
		    names[i], err, match);
	}
	check(vdev_raidz_redirect_hits() - hits == (uint64_t)nmoves,
	    "T1 each moved block was read once, through the redirect "
	    "(%llu hits for %d blocks)",
	    (u_longlong_t)(vdev_raidz_redirect_hits() - hits), nmoves);
	{
		uint64_t errs = 0;

		for (int c = 0; c < WIDTH; c++)
			errs += child_cksum_errors(spa, c);
		check(errs == 0, "T1 no checksum error on any child (%llu)",
		    (u_longlong_t)errs);
	}

	/* T2: two bad columns of the new extent, at parity 2. */
	(void) extent_corrupt(&moves[A].mv_new, 0);
	(void) extent_corrupt(&moves[A].mv_new, 1);
	spa = cycle_pool(spa);
	err = read_object(spa, obj[A], seed[A], &match);
	check(err == 0 && match, "T2 plain-A with two bad columns reads back "
	    "(err=%d match=%d)", err, match);
	spa = cycle_pool(spa);
	check(extent_same(&moves[A].mv_new),
	    "T2 plain-A's new extent was repaired in place");

	/* T4: a scrub repairs the plain and the dedup block. */
	int sc = extent_corrupt(&moves[S].mv_new, 0);
	int dc = extent_corrupt(&moves[D].mv_new, 2);
	spa = cycle_pool(spa);
	reset_errors(spa);
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	while (dsl_scan_scrubbing(spa_get_dsl(spa)))
		txg_wait_synced(spa_get_dsl(spa), 0);
	{
		uint64_t serrs = spa_get_dsl(spa)->dp_scan->scn_phys.scn_errors;
		uint64_t others = 0;

		(void) printf("INFO scrub: checksum errors by child:");
		for (int c = 0; c < WIDTH; c++) {
			(void) printf(" %llu",
			    (u_longlong_t)child_cksum_errors(spa, c));
			if (c != sc && c != dc)
				others += child_cksum_errors(spa, c);
		}
		(void) printf(" (corrupted columns on children %d and %d)\n",
		    sc, dc);
		check(serrs == 0, "T4 the scrub found no unrecoverable block "
		    "(%llu)", (u_longlong_t)serrs);
		check(others == 0, "T4 no checksum error on the children that "
		    "were not corrupted (%llu)", (u_longlong_t)others);
	}
	spa = cycle_pool(spa);
	check(extent_same(&moves[S].mv_new),
	    "T4 the scrub repaired plain-S's new extent");
	check(extent_same(&moves[D].mv_new),
	    "T4 the scrub repaired dedup-D's new extent");
	err = read_object(spa, obj[D], seed[D], &match);
	check(err == 0 && match, "T4 dedup-D reads back (err=%d match=%d)",
	    err, match);

	/* T5: a bad column in the moved gang header. */
	(void) extent_corrupt(&moves[G].mv_new, 0);
	spa = cycle_pool(spa);
	err = read_object(spa, obj[G], seed[G], &match);
	check(err == 0 && match, "T5 gang-G with a bad header column reads "
	    "back (err=%d match=%d)", err, match);
	spa = cycle_pool(spa);
	check(extent_same(&moves[G].mv_new),
	    "T5 gang-G's new header extent was repaired in place");

	/* T7: missing children rather than bad data. */
	{
		int kids[3];

		data_children(&moves[E].mv_new, 3, kids);
		spa = cycle_pool(spa);
		err = read_object_faulted(obj[E], seed[E], kids, 2, spa,
		    &match);
		check(err == 0 && match, "T7 plain-E with children %d and %d "
		    "failing reads back (err=%d match=%d)", kids[0], kids[1],
		    err, match);
		spa = cycle_pool(spa);
		err = read_object_faulted(obj[E], seed[E], kids, 3, spa,
		    &match);
		check(err != 0, "T7 plain-E with children %d, %d and %d "
		    "failing does not (err=%d)", kids[0], kids[1], kids[2],
		    err);

		{
			int all[WIDTH] = { 0, 1, 2, 3, 4 };

			spa = cycle_pool(spa);
			err = read_object_faulted(obj[CTL2], seed[CTL2], all,
			    WIDTH, spa, &match);
			check(err != 0, "T7 control-2 with every child failing "
			    "does not read (err=%d)", err);
		}
		data_children(&ctl2_old, 2, kids);
		spa = cycle_pool(spa);
		err = read_object_faulted(obj[CTL2], seed[CTL2], kids, 2, spa,
		    &match);
		check(err != 0, "T7 control-2 (parity 1, not moved) with "
		    "children %d and %d failing does not (err=%d)", kids[0],
		    kids[1], err);
		spa = cycle_pool(spa);
		check(extent_same(&moves[E].mv_new),
		    "T7 plain-E's new extent is intact afterwards");
	}

	/* T3: three bad columns are beyond parity 2; control at parity 1. */
	(void) extent_corrupt(&moves[B].mv_new, 0);
	(void) extent_corrupt(&moves[B].mv_new, 1);
	(void) extent_corrupt(&moves[B].mv_new, 2);
	(void) extent_corrupt(&ctl_old, 0);
	(void) extent_corrupt(&ctl_old, 1);
	spa = cycle_pool(spa);
	err = read_object(spa, obj[B], seed[B], &match);
	check(err != 0, "T3 plain-B with three bad columns fails (err=%d)",
	    err);
	err = read_object(spa, obj[CTL], seed[CTL], &match);
	check(err != 0, "T3 control (parity 1, not moved) with two bad "
	    "columns fails (err=%d)", err);

	/* T6: no write ever reached an old extent. */
	for (int i = 0; i < nmoves; i++) {
		check(extent_same(&moves[i].mv_old),
		    "T6 %s's old extent is unchanged", moves[i].mv_name);
	}

	(void) printf("INFO redirect hits: %llu\n",
	    (u_longlong_t)vdev_raidz_redirect_hits());
	export_pool(spa);
	kernel_fini();
}

/*
 * The size of Y: the largest psize whose asize in the new epoch fits in what
 * P2 frees of an old extent, so that best fit finds that hole an exact match
 * under P2 and a near one under P3.  The same size is used under P1.
 */
static uint64_t
steered_psize(spa_t *spa, const move_t *mv)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	uint64_t hole = mv->mv_old_asize - (1ULL << tvd->vdev_ashift);
	uint64_t psize = 1ULL << tvd->vdev_ashift;

	while (vdev_psize_to_asize_txg(tvd, psize + (1ULL << tvd->vdev_ashift),
	    mv->mv_new_birth) <= hole)
		psize += 1ULL << tvd->vdev_ashift;
	return (psize);
}

static void
set_tunable(const char *arg)
{
	VERIFY0(handle_tunable_option(arg, B_TRUE));
}

/* An empty object with 128K blocks holding a clone of <src>'s block. */
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

/*
 * Write one block per object, in one txg, with a single allocator and
 * best-fit allocation, so that each lands in the smallest free segment that
 * holds it: the freed part of an old extent when the policy freed one.
 */
static void
put_steered(spa_t *spa, uint64_t *objs, blkptr_t *bps, const uint64_t *seeds,
    int n, uint64_t size)
{
	objset_t *os;

	set_tunable("metaslab_df_free_pct=100");
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < n; i++)
		objs[i] = put_object_size(os, seeds[i], size);
	txg_wait_synced(spa_get_dsl(spa), 0);
	for (int i = 0; i < n; i++)
		bps[i] = object_bp(os, objs[i]);
	dmu_objset_disown(os, B_TRUE, FTAG);
	set_tunable("metaslab_df_free_pct=4");
}

/* Where Y lies against the old extent of X. */
static const char *
y_place(const move_t *mv, const blkptr_t *ybp)
{
	uint64_t o = DVA_GET_OFFSET(&mv->mv_bp.blk_dva[0]);
	uint64_t y = DVA_GET_OFFSET(&ybp->blk_dva[0]);
	uint64_t yend = y + DVA_GET_ASIZE(&ybp->blk_dva[0]);

	if (y == o)
		return ("starts at O");
	if (y < o + mv->mv_old_asize && yend > o)
		return ("overlaps the old extent");
	return ("is outside the old extent");
}

/* Blocks written in one txg, which the allocator lays out back to back. */
#define	NRUN	8

/* Write NRUN one-block objects in one txg, seeds from <seed0> on. */
static void
put_run(spa_t *spa, objset_t *os, uint64_t seed0, uint64_t *objs,
    blkptr_t *bps)
{
	for (int i = 0; i < NRUN; i++)
		objs[i] = put_object(os, seed0 + i);
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("INFO run of %d blocks:", NRUN);
	for (int i = 0; i < NRUN; i++) {
		bps[i] = object_bp(os, objs[i]);
		(void) printf(" %llx", (u_longlong_t)DVA_GET_OFFSET(
		    &bps[i].blk_dva[0]));
	}
	(void) printf("\n");
}

/* B_TRUE if other blocks of the run end at bps[i]'s start / start at end. */
static boolean_t
run_neighbour(const blkptr_t *bps, int i, boolean_t after)
{
	uint64_t o = DVA_GET_OFFSET(&bps[i].blk_dva[0]);
	uint64_t e = o + DVA_GET_ASIZE(&bps[i].blk_dva[0]);

	for (int j = 0; j < NRUN; j++) {
		uint64_t oj = DVA_GET_OFFSET(&bps[j].blk_dva[0]);

		if (j == i)
			continue;
		if (after ? oj == e :
		    oj + DVA_GET_ASIZE(&bps[j].blk_dva[0]) == o)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * T4a: BRT reference theft through an alias at O.  X is a block of a run
 * written in one txg with run blocks on both sides, so that what the
 * policy frees of its old extent is a hole of exactly that size.  Unless
 * <alone>, another block K of the run is cloned too, so that the vdev's BRT
 * stays in use whatever happens to X's entry.
 */
static void
run_brt(policy_t policy, boolean_t alone)
{
	uint64_t seed_y = 12, seed0 = 20;
	uint64_t objs[NRUN], a, b, c, o, refs, ysize;
	blkptr_t bps[NRUN];
	blkptr_t xbp, bbp, ybp;
	objset_t *os;
	spa_t *spa;
	move_t mv;
	boolean_t match;
	int err, x = -1, k;

	set_tunable("spa_num_allocators=1");
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	(void) printf("INFO T4a policy %s%s, ashift %llu, %llu metaslabs of "
	    "%llu bytes\n", policy_names[policy],
	    alone ? ", X the only cloned block" : "", (u_longlong_t)ashift,
	    (u_longlong_t)spa->spa_root_vdev->vdev_child[0]->vdev_ms_count,
	    1ULL << spa->spa_root_vdev->vdev_child[0]->vdev_ms_shift);

	/* X in the parity-1 epoch, cloned into a second object. */
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	put_run(spa, os, seed0, objs, bps);
	for (int i = 0; i < NRUN && x == -1; i++) {
		if (run_neighbour(bps, i, B_FALSE) &&
		    run_neighbour(bps, i, B_TRUE))
			x = i;
	}
	VERIFY3S(x, !=, -1);
	k = (x + 1) % NRUN;
	a = objs[x];
	b = clone_object(spa, os, a);
	if (!alone)
		(void) clone_object(spa, os, objs[k]);
	(void) printf("INFO X is run block %d; K, run block %d, is %s\n", x, k,
	    alone ? "not cloned" : "cloned");
	xbp = object_bp(os, a);
	move_prepare(spa, &mv, "X", &xbp, os, a);
	bbp = object_bp(os, b);
	dmu_objset_disown(os, B_TRUE, FTAG);
	o = DVA_GET_OFFSET(&mv.mv_bp.blk_dva[0]);
	refs = brt_entry_get_refcount(spa, &mv.mv_bp);
	check(DVA_EQUAL(&bbp.blk_dva[0], &mv.mv_bp.blk_dva[0]) &&
	    BP_GET_PHYSICAL_BIRTH(&bbp) == BP_GET_PHYSICAL_BIRTH(&mv.mv_bp),
	    "T4a the clone names X's DVA and physical birth");
	check(refs == 1, "T4a X has one BRT reference before the move (%llu)",
	    (u_longlong_t)refs);

	/* Move X, apply the policy, let the freed part become allocatable. */
	(void) add_parity2_epoch(spa);
	run_moves(spa, &mv, 1, policy, 0);
	wait_frees(spa);
	print_move(spa, &mv);
	if (mv.mv_kept < mv.mv_old_asize) {
		(void) printf("INFO X's freed part [%llx, %llx) allocatable: "
		    "%d\n", (u_longlong_t)(o + mv.mv_kept),
		    (u_longlong_t)(o + mv.mv_old_asize),
		    range_allocatable(spa, o + mv.mv_kept,
		    mv.mv_old_asize - mv.mv_kept));
	}

	/* Y, steered onto the old extent if the policy freed any of it. */
	print_free(spa, 0, mv.mv_new_offset + mv.mv_new_asize + (1 << 20));
	ysize = steered_psize(spa, &mv);
	put_steered(spa, &c, &ybp, &seed_y, 1, ysize);
	(void) printf("INFO Y: offset=%llx psize=%llu asize=%llu birth=%llu, "
	    "Y - O = %lld: Y %s\n",
	    (u_longlong_t)DVA_GET_OFFSET(&ybp.blk_dva[0]),
	    (u_longlong_t)BP_GET_PSIZE(&ybp),
	    (u_longlong_t)DVA_GET_ASIZE(&ybp.blk_dva[0]),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&ybp),
	    (longlong_t)(DVA_GET_OFFSET(&ybp.blk_dva[0]) - o),
	    y_place(&mv, &ybp));
	(void) printf("INFO BRT refcount at Y's offset before Y is freed: "
	    "%llu\n", (u_longlong_t)brt_entry_get_refcount(spa, &ybp));

	/* Free Y. */
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	VERIFY0(dmu_free_long_object(os, c));
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
	wait_frees(spa);

	check(DVA_GET_OFFSET(&ybp.blk_dva[0]) != o,
	    "T4a Y does not start at X's old offset (Y at %llx, O at %llx)",
	    (u_longlong_t)DVA_GET_OFFSET(&ybp.blk_dva[0]), (u_longlong_t)o);
	refs = brt_entry_get_refcount(spa, &mv.mv_bp);
	check(refs == 1, "T4a X keeps its one BRT reference after Y is freed "
	    "(%llu)", (u_longlong_t)refs);
	check(range_allocatable(spa, DVA_GET_OFFSET(&ybp.blk_dva[0]),
	    DVA_GET_ASIZE(&ybp.blk_dva[0])),
	    "T4a Y's extent [%llx, +%llu) is allocatable again",
	    (u_longlong_t)DVA_GET_OFFSET(&ybp.blk_dva[0]),
	    (u_longlong_t)DVA_GET_ASIZE(&ybp.blk_dva[0]));

	spa = cycle_pool(spa);
	(void) printf("INFO after reimport: X refcount %llu, Y's extent "
	    "allocatable %d\n",
	    (u_longlong_t)brt_entry_get_refcount(spa, &mv.mv_bp),
	    range_allocatable(spa, DVA_GET_OFFSET(&ybp.blk_dva[0]),
	    DVA_GET_ASIZE(&ybp.blk_dva[0])));
	err = read_object(spa, a, seed0 + x, &match);
	check(err == 0 && match, "T4a X's object reads back (err=%d match=%d)",
	    err, match);
	err = read_object(spa, b, seed0 + x, &match);
	check(err == 0 && match, "T4a X's clone reads back (err=%d match=%d)",
	    err, match);
	(void) printf("INFO redirect hits: %llu\n",
	    (u_longlong_t)vdev_raidz_redirect_hits());
	export_pool(spa);
	kernel_fini();
}

/*
 * T4b: a sorted scrub with blocks Y overlapping the freed part of old
 * extents.  The blocks X moved are those of a run written in one txg that
 * have another run block right after them, so that what P2 frees of each is
 * a hole of exactly that size.
 */
static void
run_scan(policy_t policy, boolean_t fix)
{
	uint64_t robj[NRUN], xobj[NRUN], yobj[NRUN], xseed[NRUN], yseed[NRUN];
	blkptr_t rbp[NRUN], ybp[NRUN];
	move_t moves[NRUN];
	extent_t xnew, ynew;
	pool_scan_stat_t ps;
	objset_t *os;
	spa_t *spa;
	int npairs = 0, overlaps = 0, cx, cy;
	uint64_t hits, others = 0, ysize;
	boolean_t match;
	int err;

	set_tunable("spa_num_allocators=1");
	set_tunable("zfs_scan_legacy=0");
	if (&spike_scan_redirect_unsorted != NULL)
		spike_scan_redirect_unsorted = fix;
	(void) printf("INFO T4b policy %s, ashift %llu, routing of "
	    "redirected blocks: %s\n", policy_names[policy],
	    (u_longlong_t)ashift, &spike_scan_redirect_unsorted == NULL ?
	    "absent from this libzpool" :
	    spike_scan_redirect_unsorted ? "on" : "off");
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	(void) printf("INFO %llu metaslabs of %llu bytes\n",
	    (u_longlong_t)spa->spa_root_vdev->vdev_child[0]->vdev_ms_count,
	    1ULL << spa->spa_root_vdev->vdev_child[0]->vdev_ms_shift);

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	put_run(spa, os, 100, robj, rbp);
	for (int i = 0; i < NRUN; i++) {
		if (!run_neighbour(rbp, i, B_TRUE))
			continue;
		xobj[npairs] = robj[i];
		xseed[npairs] = 100 + i;
		yseed[npairs] = 200 + i;
		move_prepare(spa, &moves[npairs], "X", &rbp[i], os, robj[i]);
		npairs++;
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY3S(npairs, >, 0);

	/*
	 * The new extents go to the last metaslab, far from the run, so that
	 * the scan's extent tree gets no unqueued gap from them near X.
	 */
	(void) add_parity2_epoch(spa);
	{
		vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];

		run_moves(spa, moves, npairs, policy,
		    tvd->vdev_ms[tvd->vdev_ms_count - 1]->ms_start);
	}
	wait_frees(spa);
	ysize = steered_psize(spa, &moves[0]);
	put_steered(spa, yobj, ybp, yseed, npairs, ysize);
	for (int i = 0; i < npairs; i++) {
		const char *where = "is outside every old extent";

		print_move(spa, &moves[i]);
		for (int j = 0; j < npairs; j++) {
			if (strcmp(y_place(&moves[j], &ybp[i]),
			    "is outside the old extent") != 0) {
				where = y_place(&moves[j], &ybp[i]);
				overlaps++;
				break;
			}
		}
		(void) printf("INFO Y%d: offset=%llx psize=%llu asize=%llu: "
		    "%s\n", i, (u_longlong_t)DVA_GET_OFFSET(&ybp[i].blk_dva[0]),
		    (u_longlong_t)BP_GET_PSIZE(&ybp[i]),
		    (u_longlong_t)DVA_GET_ASIZE(&ybp[i].blk_dva[0]), where);
	}
	(void) printf("INFO %d of %d Y blocks overlap an old extent\n",
	    overlaps, npairs);

	/* A bad column in X0's new extent and in Y0, for the scrub to fix. */
	extent_map(&xnew, moves[0].mv_new_offset, moves[0].mv_psize, 2);
	extent_save(&xnew);
	extent_map(&ynew, DVA_GET_OFFSET(&ybp[0].blk_dva[0]), ysize, 2);
	extent_save(&ynew);
	cx = extent_corrupt(&xnew, 0);
	cy = extent_corrupt(&ynew, 0);

	spa = cycle_pool(spa);
	reset_errors(spa);
	hits = vdev_raidz_redirect_hits();
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	while (dsl_scan_scrubbing(spa_get_dsl(spa)))
		txg_wait_synced(spa_get_dsl(spa), 0);
	VERIFY0(spa_scan_get_stats(spa, &ps));
	(void) printf("INFO scrub: state=%llu examined=%llu skipped=%llu "
	    "issued=%llu to_examine=%llu errors=%llu, redirect hits %llu\n",
	    (u_longlong_t)ps.pss_state, (u_longlong_t)ps.pss_examined,
	    (u_longlong_t)ps.pss_skipped, (u_longlong_t)ps.pss_issued,
	    (u_longlong_t)ps.pss_to_examine, (u_longlong_t)ps.pss_errors,
	    (u_longlong_t)(vdev_raidz_redirect_hits() - hits));
	(void) printf("INFO scrub: checksum errors by child:");
	for (int c = 0; c < WIDTH; c++) {
		uint64_t errs = child_cksum_errors(spa, c);

		(void) printf(" %llu", (u_longlong_t)errs);
		if (c != cx && c != cy)
			others += errs;
	}
	(void) printf(" (bad columns on children %d and %d)\n", cx, cy);

	check(ps.pss_state == DSS_FINISHED, "T4b the scrub finished (%llu)",
	    (u_longlong_t)ps.pss_state);
	check(ps.pss_errors == 0, "T4b the scrub found no unrecoverable "
	    "block (%llu)", (u_longlong_t)ps.pss_errors);
	check(ps.pss_issued == ps.pss_examined - ps.pss_skipped, "T4b the "
	    "scrub issued every byte it examined and did not skip "
	    "(examined %llu, skipped %llu, issued %llu)",
	    (u_longlong_t)ps.pss_examined, (u_longlong_t)ps.pss_skipped,
	    (u_longlong_t)ps.pss_issued);
	check(vdev_raidz_redirect_hits() - hits >= npairs, "T4b the scrub "
	    "read every moved block through its redirect (%llu hits, %d "
	    "blocks)", (u_longlong_t)(vdev_raidz_redirect_hits() - hits),
	    npairs);
	check(others == 0, "T4b no checksum error on the children without a "
	    "bad column (%llu)", (u_longlong_t)others);
	spa = cycle_pool(spa);
	check(extent_same(&xnew), "T4b the scrub repaired X0's new extent");
	check(extent_same(&ynew), "T4b the scrub repaired Y0's extent");
	for (int i = 0; i < npairs; i++) {
		boolean_t ymatch;
		int yerr;

		err = read_object(spa, xobj[i], xseed[i], &match);
		yerr = read_object_size(yobj[i], yseed[i], ysize, &ymatch);
		check(err == 0 && match && yerr == 0 && ymatch, "T4b X%d and "
		    "Y%d read back (err=%d/%d match=%d/%d)", i, i, err, yerr,
		    match, ymatch);
	}
	export_pool(spa);
	kernel_fini();
}

static int
parse_policy(const char *s, policy_t *policy)
{
	for (int p = P1; p <= P3; p++) {
		if (strcmp(s, policy_names[p]) == 0) {
			*policy = p;
			return (0);
		}
	}
	return (-1);
}

int
main(int argc, char **argv)
{
	policy_t policy = P1;

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 3 || (argc > 3 && (argc < 5 || argc > 6 ||
	    parse_policy(argv[4], &policy) != 0 ||
	    (strcmp(argv[3], "brt") != 0 && strcmp(argv[3], "scan") != 0) ||
	    (argc == 6 && strcmp(argv[5],
	    strcmp(argv[3], "brt") == 0 ? "alone" : "nofix") != 0)))) {
		(void) fprintf(stderr, "usage: redirect_probe <dir> <ashift>\n"
		    "       redirect_probe <dir> <ashift> brt <P1|P2|P3> "
		    "[alone]\n"
		    "       redirect_probe <dir> <ashift> scan <P1|P2|P3> "
		    "[nofix]\n");
		return (2);
	}
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 10);
	/* P3 is kept only to show it failing: allow it where it is gated. */
	if (policy == P3 && &spike_redirect_keep_none != NULL)
		spike_redirect_keep_none = 1;

	if (argc == 3)
		run_classic();
	else if (strcmp(argv[3], "brt") == 0)
		run_brt(policy, argc == 6);
	else
		run_scan(policy, argc == 5);
	(void) printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures);
	return (failures == 0 ? 0 : 1);
}
