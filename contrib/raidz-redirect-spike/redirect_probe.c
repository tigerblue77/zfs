// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 3 (throwaway, not part of any tree): can a block of an old
 * parity epoch be re-encoded at the new parity into a new extent and served
 * from there, with its block pointer left untouched?
 *
 *   redirect_probe <dir> <ashift>
 *
 * Needs a libzpool built with the SPIKE redirect hook in vdev_raidz.c
 * (vdev_raidz_redirect_add()).  Creates a five-wide raidz1 pool of files in
 * <dir>, writes plain, dedup and gang blocks, moves the vdev to a parity-2
 * epoch, re-encodes some of the old blocks into new extents (raw read with
 * the old layout, metaslab_alloc() and zio_rewrite() with the new one),
 * registers redirects, then checks:
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
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
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
read_object(spa_t *spa, uint64_t obj, uint64_t seed, boolean_t *match)
{
	objset_t *os;
	uint64_t *buf = malloc(BLKSZ), *want = malloc(BLKSZ);
	int err;

	(void) spa;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	fill_pattern(want, BLKSZ, seed);
	memset(buf, 0, BLKSZ);
	err = dmu_read(os, obj, 0, BLKSZ, buf, DMU_READ_NO_PREFETCH);
	*match = (err == 0 && memcmp(buf, want, BLKSZ) == 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(buf);
	free(want);
	return (err);
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
	uint64_t	mv_psize;	/* bytes at the DVA: psize or header */
	abd_t		*mv_data;	/* their raw contents */
	uint64_t	mv_new_offset;
	uint64_t	mv_new_birth;
	uint64_t	mv_new_asize;
	extent_t	mv_old;
	extent_t	mv_new;
} move_t;

typedef struct move_arg {
	move_t	*ma_moves;
	int	ma_n;
} move_arg_t;

/*
 * Re-encode every block in a sync task of a txg of the new epoch: allocate
 * an extent sized for its layout, write the raw bytes there through the
 * raidz vdev with that layout, and register the redirect.
 */
static void
move_sync(void *arg, dmu_tx_t *tx)
{
	move_arg_t *ma = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	uint64_t txg = dmu_tx_get_txg(tx);

	for (int i = 0; i < ma->ma_n; i++) {
		move_t *mv = &ma->ma_moves[i];
		zio_alloc_list_t zal;
		blkptr_t wbp;

		memset(&wbp, 0, sizeof (wbp));
		BP_SET_LSIZE(&wbp, mv->mv_psize);
		BP_SET_PSIZE(&wbp, mv->mv_psize);
		BP_SET_COMPRESS(&wbp, ZIO_COMPRESS_OFF);
		BP_SET_CHECKSUM(&wbp, ZIO_CHECKSUM_OFF);
		BP_SET_TYPE(&wbp, DMU_OT_PLAIN_FILE_CONTENTS);
		BP_SET_LEVEL(&wbp, 0);
		BP_SET_BYTEORDER(&wbp, ZFS_HOST_BYTEORDER);

		metaslab_trace_init(&zal);
		VERIFY0(metaslab_alloc(spa, spa_normal_class(spa),
		    mv->mv_psize, &wbp, 1, txg, NULL, 0, &zal, 0, FTAG));
		metaslab_trace_fini(&zal);
		BP_SET_BIRTH(&wbp, txg, txg);

		VERIFY0(zio_wait(zio_rewrite(NULL, spa, txg, &wbp,
		    mv->mv_data, mv->mv_psize, NULL, NULL,
		    ZIO_PRIORITY_SYNC_WRITE, ZIO_FLAG_CANFAIL, NULL)));

		mv->mv_new_offset = DVA_GET_OFFSET(&wbp.blk_dva[0]);
		mv->mv_new_birth = txg;
		mv->mv_new_asize = DVA_GET_ASIZE(&wbp.blk_dva[0]);
		VERIFY0(vdev_raidz_redirect_add(0,
		    DVA_GET_OFFSET(&mv->mv_bp.blk_dva[0]),
		    BP_GET_PHYSICAL_BIRTH(&mv->mv_bp), mv->mv_new_offset,
		    mv->mv_new_birth, mv->mv_new_asize));
	}
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

int
main(int argc, char **argv)
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

	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 3) {
		(void) fprintf(stderr,
		    "usage: redirect_probe <dir> <ashift>\n");
		return (2);
	}
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 10);

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

	/* The parity-2 epoch, from the first txg it may start in. */
	{
		epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

		VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
		(void) printf("INFO parity 2 from txg %llu\n",
		    (u_longlong_t)ea.ea_table[3]);
	}

	/* Re-encode and redirect. */
	{
		move_arg_t ma = { moves, nmoves };

		VERIFY0(dsl_sync_task(POOL, NULL, move_sync, &ma, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
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
	(void) printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
	    failures);
	return (failures == 0 ? 0 : 1);
}
