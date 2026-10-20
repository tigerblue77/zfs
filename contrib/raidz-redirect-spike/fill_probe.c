// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike, accounting defect X2 (throwaway, not part of any tree): after the
 * old blocks of a raidz vdev are moved to a higher parity, does the pool
 * still hold what "available" promised?
 *
 *   fill_probe <dir> <ashift> <move|nomove> [filemb=N] [reck=N] [objs=N]
 *       [objmb=N] [overw=N] [unaligned]
 *
 * Phase 0 builds a five-wide raidz1 pool of five files in <dir>, with
 * compression off: OBJS objects of OBJSZ bytes of incompressible data in
 * SMALL-byte records, a snapshot, then the first OVERW objects written
 * again, so that the snapshot alone holds their old blocks; then a
 * parity-2 epoch (defaults: files of 256 MiB, 10 objects of 20 MiB in 16K
 * records, 6 overwritten).  With 'move', a second phase moves every DVA
 * outside the MOS born before the epoch with the kernel's test-only move,
 * keeping the first sectors of each old extent (P2).  'nomove' is the
 * control: the same pool and epoch, nothing moved.
 *
 * The fill phase then reads the head dataset's "available" (A) and writes
 * incompressible data in BLKSZ records, in 1 MiB transactions until the
 * first ENOSPC and in single records after it, until ENOSPC three times in
 * a row with a synced txg between them.  It reports what the DSL charged
 * for the writes (C, the growth of the root dsl_dir's used bytes) against
 * A, both in the DSL's deflated bytes.  C/A is about 1 when "available"
 * was true; below 1 if the vdev ran out first, which suspends the pool
 * (failmode=wait): a watchdog then reports C so far and the phase exits 3.
 *
 * The move must add more than the slop (at least 128 MiB, on a small pool)
 * for the vdev to run out first.  At ashift 12, a 128K block takes 160K at
 * parity 1 and 216K at parity 2, plus the 4K kept: 48K deflated more per
 * 128K.  A 16K block takes 24K, then 36K plus 4K: 12.8K more per 16K, but
 * P2 frees a 5-sector tail of which a parity-2 allocation, a multiple of 3
 * sectors, can use 3 at most: the free space it leaves is partly stranded,
 * which the probe reports; the kernel now keeps enough sectors for the
 * tail to be a multiple of 3 (X3), and 'unaligned' keeps one sector again.
 *
 * Prints INFO lines, a RESULT line, PASS or FAIL lines, and exits 1 if a
 * check failed.  The pool files are deleted at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/range_tree.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"flp"
#define	WIDTH		5
#define	BLKSZ		(128ULL << 10)
#define	MAXOBJS		64

/* The same binary runs on a libzpool without the charge (X2). */
#pragma weak vdev_raidz_redirect_dspace
#pragma weak spike_redirect_keep_unaligned
#define	FILLOBJ		(64ULL << 20)
#define	BIGTX		(1ULL << 20)
#define	PATH_BUF	1024
#define	MAXMV		(1 << 16)
#define	BATCH		128

typedef struct shared {
	uint64_t	sh_epoch_txg;
	uint64_t	sh_moved;
	uint64_t	sh_refused;
	uint64_t	sh_gang;	/* old gang DVAs, not moved */
	uint64_t	sh_moved_bytes;
	int		sh_errs[8];	/* rdm_error values seen */
	int		sh_errn[8];
	/* The fill. */
	uint64_t	sh_avail;	/* A */
	uint64_t	sh_used0;	/* root dir's used before */
	uint64_t	sh_used;	/* ... at the last synced txg seen */
	uint64_t	sh_written;	/* logical bytes the fill committed */
	uint64_t	sh_enospc;	/* ENOSPC returns */
	int		sh_suspended;
	int		sh_done;
} shared_t;

static shared_t *sh;
static const char *dir;
static uint64_t ashift;
static boolean_t opt_move;
static boolean_t opt_unaligned;
/* Defaults; each can be set with <name>=<value> on the command line. */
static uint64_t FILE_SIZE = 256ULL << 20;	/* filemb */
static uint64_t SMALL = 16ULL << 10;		/* reck */
static int OBJS = 10;				/* objs */
static uint64_t OBJSZ = 20ULL << 20;		/* objmb */
static int OVERW = 6;				/* overw */
static int failures;
static spa_t *wd_spa;

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

static int
phase(const char *name, int (*fn)(void *), void *arg)
{
	pid_t pid;
	int status;

	(void) fflush(stdout);
	pid = fork();
	VERIFY3S(pid, !=, -1);
	if (pid == 0) {
		int rc = fn(arg);

		(void) fflush(stdout);
		_exit(rc);
	}
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

static void
create_pool(void)
{
	nvlist_t *child[WIDTH], *raid, *root, *props;
	char path[PATH_BUF];

	for (int c = 0; c < WIDTH; c++) {
		int fd;

		(void) snprintf(path, sizeof (path), "%s/v%d", dir, c);
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

static int
try_import(const char *from, spa_t **spap)
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
	paths[0] = (char *)from;
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	err = zpool_find_config(&lpch, POOL, &config, &ia);
	if (err != 0)
		return (err);
	err = spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL);
	fnvlist_free(config);
	if (err == 0)
		VERIFY0(spa_open(POOL, spap, FTAG));
	return (err);
}

static void
export_pool(spa_t *spa)
{
	spa_close(spa, FTAG);
	VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
}

/* Write [off, off + size) of obj with the pattern of seed; the error. */
static int
write_range(objset_t *os, uint64_t obj, uint64_t off, uint64_t size,
    uint64_t seed, void *buf)
{
	dmu_tx_t *tx;
	int err;

	fill_pattern(buf, size, seed);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, obj, off, size);
	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}
	dmu_write(os, obj, off, size, buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	return (0);
}

static int
new_object(objset_t *os, uint64_t blksz, uint64_t *objp)
{
	dmu_tx_t *tx = dmu_tx_create(os);
	int err;

	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}
	*objp = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, blksz,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	return (0);
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

/* Let the deferred frees of the last txgs reach the space maps. */
static void
space_settle(spa_t *spa)
{
	for (int i = 0; i < TXG_DEFER_SIZE + 2; i++) {
		dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);

		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_tx_commit(tx);
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
}

static uint64_t
root_used(spa_t *spa)
{
	return (dsl_dir_phys(spa_get_dsl(spa)->dp_root_dir)->dd_used_bytes);
}

static uint64_t
head_avail(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	dsl_dataset_t *ds;
	uint64_t refd, avail, uobj, aobj;

	dsl_pool_config_enter(dp, FTAG);
	VERIFY0(dsl_dataset_hold(dp, POOL, FTAG, &ds));
	dsl_dataset_space(ds, &refd, &avail, &uobj, &aobj);
	dsl_dataset_rele(ds, FTAG);
	dsl_pool_config_exit(dp, FTAG);
	return (avail);
}

/* What the DSL charges against what the raidz holds, and "available". */
static void
print_space(spa_t *spa, const char *when)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	metaslab_class_t *mc = spa_normal_class(spa);
	uint64_t used = root_used(spa);
	uint64_t dalloc = vdev_deflated_space(vd, vd->vdev_stat.vs_alloc);
	int64_t charge = 0;

	if (vdev_raidz_redirect_dspace != NULL)
		charge = vdev_raidz_redirect_dspace(spa);
	(void) printf("INFO space %s: vdev allocated %llu (deflated %llu), "
	    "DSL used %llu, deflated minus used %lld, moved-block charge "
	    "%lld, unaccounted %lld\n", when,
	    (u_longlong_t)vd->vdev_stat.vs_alloc, (u_longlong_t)dalloc,
	    (u_longlong_t)used, (longlong_t)(dalloc - used),
	    (longlong_t)charge, (longlong_t)(dalloc - used - charge));
	(void) printf("INFO space %s: dspace %llu, slop %llu, class free "
	    "(deflated) %llu, head available %llu\n", when,
	    (u_longlong_t)spa_get_dspace(spa),
	    (u_longlong_t)spa_get_slop_space(spa),
	    (u_longlong_t)(metaslab_class_get_dspace(mc) -
	    metaslab_class_get_dalloc(mc)), (u_longlong_t)head_avail(spa));
}

typedef struct usable_arg {
	uint64_t	ua_unit;
	uint64_t	ua_free;
	uint64_t	ua_usable;
	uint64_t	ua_segs;
	uint64_t	ua_small;	/* free segments of less than ua_unit */
} usable_arg_t;

static void
usable_cb(void *arg, uint64_t start, uint64_t size)
{
	usable_arg_t *ua = arg;

	(void) start;
	ua->ua_free += size;
	ua->ua_usable += size / ua->ua_unit * ua->ua_unit;
	ua->ua_segs++;
	if (size < ua->ua_unit)
		ua->ua_small++;
}

/*
 * The free space of the raidz that parity-2 allocations can use: each one
 * is a multiple of 3 sectors, so a free segment holds at most the largest
 * multiple of 3 sectors it contains.  An upper bound.
 */
static void
print_usable(spa_t *spa, const char *when)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	usable_arg_t ua = { .ua_unit = 3ULL << vd->vdev_ashift };

	for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
		metaslab_t *msp = vd->vdev_ms[m];

		mutex_enter(&msp->ms_lock);
		VERIFY0(metaslab_load(msp));
		zfs_range_tree_walk(msp->ms_allocatable, usable_cb, &ua);
		mutex_exit(&msp->ms_lock);
	}
	(void) printf("INFO free %s: %llu bytes in %llu segments, %llu of "
	    "them shorter than 3 sectors; at most %llu usable by parity-2 "
	    "allocations, %llu stranded (deflated %lld)\n", when,
	    (u_longlong_t)ua.ua_free, (u_longlong_t)ua.ua_segs,
	    (u_longlong_t)ua.ua_small, (u_longlong_t)ua.ua_usable,
	    (u_longlong_t)(ua.ua_free - ua.ua_usable),
	    (longlong_t)vdev_deflated_space(vd, ua.ua_free - ua.ua_usable));
}

static int
child_setup(void *arg)
{
	void *buf = malloc(OBJSZ);
	uint64_t obj[MAXOBJS];
	objset_t *os;
	spa_t *spa;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool();
	VERIFY0(spa_open(POOL, &spa, FTAG));
	VERIFY0(dsl_prop_set_int(POOL, zfs_prop_to_name(ZFS_PROP_COMPRESSION),
	    ZPROP_SRC_LOCAL, ZIO_COMPRESS_OFF));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < OBJS; i++) {
		VERIFY0(new_object(os, SMALL, &obj[i]));
		VERIFY0(write_range(os, obj[i], 0, OBJSZ, 100 + i, buf));
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dmu_objset_snapshot_one(POOL, "s1"));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < OVERW; i++)
		VERIFY0(write_range(os, obj[i], 0, OBJSZ, 200 + i, buf));
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	{
		epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

		VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
		sh->sh_epoch_txg = ea.ea_table[3];
	}
	(void) printf("INFO setup: %d objects of %llu MiB in %llu K records, "
	    "@s1, %d overwritten; parity 2 from txg %llu\n", OBJS,
	    (u_longlong_t)(OBJSZ >> 20), (u_longlong_t)(SMALL >> 10), OVERW,
	    (u_longlong_t)sh->sh_epoch_txg);
	space_settle(spa);
	print_space(spa, "after setup");
	export_pool(spa);
	kernel_fini();
	free(buf);
	return (0);
}

/* ---- the move ---- */

typedef struct mv {
	blkptr_t	mv_bp;
	zbookmark_phys_t mv_zb;
	int		mv_dva;
} mv_t;

#define	SET_BITS	20
typedef struct census_arg {
	uint64_t	*ca_key;	/* {offset + 1, birth} per slot */
	mv_t		*ca_mv;
	int		ca_nmv;
} census_arg_t;

static boolean_t
set_add(uint64_t *key, uint64_t offset, uint64_t birth)
{
	uint64_t mask = (1ULL << SET_BITS) - 1;
	uint64_t h = (offset * 0x9e3779b97f4a7c15ULL ^ birth) & mask;

	for (;;) {
		uint64_t *k = &key[2 * h];

		if (k[0] == 0) {
			k[0] = offset + 1;
			k[1] = birth;
			return (B_TRUE);
		}
		if (k[0] == offset + 1 && k[1] == birth)
			return (B_FALSE);
		h = (h + 1) & mask;
	}
}

static int
census_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	census_arg_t *ca = arg;
	uint64_t birth;

	(void) spa, (void) zilog, (void) dnp;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    zb->zb_objset == DMU_META_OBJSET || zb->zb_level == ZB_ZIL_LEVEL)
		return (0);
	birth = BP_GET_PHYSICAL_BIRTH(bp);
	if (birth >= sh->sh_epoch_txg)
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];

		if (!set_add(ca->ca_key, DVA_GET_OFFSET(dva), birth))
			continue;
		if (DVA_GET_GANG(dva)) {
			sh->sh_gang++;
			continue;
		}
		VERIFY3S(ca->ca_nmv, <, MAXMV);
		ca->ca_mv[ca->ca_nmv].mv_bp = *bp;
		ca->ca_mv[ca->ca_nmv].mv_zb = *zb;
		ca->ca_mv[ca->ca_nmv].mv_dva = d;
		ca->ca_nmv++;
	}
	return (0);
}

static abd_t *
read_raw(spa_t *spa, const blkptr_t *bp, int d, uint64_t psize)
{
	blkptr_t rbp = *bp;
	abd_t *abd = abd_alloc_linear(psize, B_FALSE);

	rbp.blk_dva[0] = bp->blk_dva[d];
	for (int i = 1; i < SPA_DVAS_PER_BP; i++)
		memset(&rbp.blk_dva[i], 0, sizeof (dva_t));
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

static int
child_move(void *arg)
{
	census_arg_t ca = { 0 };
	raidz_redirect_move_t *rdm = calloc(BATCH, sizeof (*rdm));
	spa_t *spa;
	hrtime_t t0;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (opt_unaligned)
		spike_redirect_keep_unaligned = 1;
	VERIFY0(try_import(dir, &spa));
	space_settle(spa);
	print_space(spa, "before the move");
	ca.ca_key = calloc(2ULL << SET_BITS, sizeof (uint64_t));
	ca.ca_mv = malloc(MAXMV * sizeof (mv_t));
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD,
	    census_cb, &ca));
	(void) printf("INFO move: %d old DVAs outside the MOS, %llu gang\n",
	    ca.ca_nmv, (u_longlong_t)sh->sh_gang);

	t0 = gethrtime();
	for (int i = 0; i < ca.ca_nmv; ) {
		int n = 0;

		while (i < ca.ca_nmv && n < BATCH) {
			mv_t *m = &ca.ca_mv[i++];

			memset(&rdm[n], 0, sizeof (rdm[n]));
			rdm[n].rdm_bp = &m->mv_bp;
			rdm[n].rdm_zb = &m->mv_zb;
			rdm[n].rdm_psize = BP_GET_PSIZE(&m->mv_bp);
			rdm[n].rdm_dva = m->mv_dva;
			rdm[n].rdm_data = read_raw(spa, &m->mv_bp, m->mv_dva,
			    rdm[n].rdm_psize);
			sh->sh_moved_bytes += rdm[n].rdm_psize;
			n++;
		}
		VERIFY0(vdev_raidz_redirect_move(spa, 0, rdm, n,
		    RAIDZ_REDIRECT_KEEP_FIRST, B_FALSE));
		for (int j = 0; j < n; j++) {
			int e = rdm[j].rdm_error;

			if (e == 0) {
				sh->sh_moved++;
			} else {
				sh->sh_refused++;
				for (int k = 0; k < 8; k++) {
					if (sh->sh_errn[k] == 0 ||
					    sh->sh_errs[k] == e) {
						sh->sh_errs[k] = e;
						sh->sh_errn[k]++;
						break;
					}
				}
			}
			abd_free(rdm[j].rdm_data);
		}
	}
	(void) printf("INFO move: %llu DVAs moved, %llu refused, %llu bytes "
	    "as stored, in %.1f s\n", (u_longlong_t)sh->sh_moved,
	    (u_longlong_t)sh->sh_refused, (u_longlong_t)sh->sh_moved_bytes,
	    (double)(gethrtime() - t0) / 1e9);
	for (int k = 0; k < 8 && sh->sh_errn[k] != 0; k++) {
		(void) printf("INFO move: error %d (%s) for %d DVAs\n",
		    sh->sh_errs[k], strerror(sh->sh_errs[k]), sh->sh_errn[k]);
	}
	space_settle(spa);
	print_space(spa, "after the move");
	print_usable(spa, "after the move");
	export_pool(spa);
	kernel_fini();
	free(ca.ca_key);
	free(ca.ca_mv);
	free(rdm);
	return (0);
}

/* ---- the fill ---- */

static void
report_fill(const char *how)
{
	double c = (double)(sh->sh_used - sh->sh_used0);

	(void) printf("RESULT %s %s: available before %llu, charged %llu, "
	    "charged/available %.4f, logical written %llu, ENOSPC returns "
	    "%llu\n", opt_move ? "move" : "nomove", how,
	    (u_longlong_t)sh->sh_avail, (u_longlong_t)(sh->sh_used -
	    sh->sh_used0), c / (double)sh->sh_avail,
	    (u_longlong_t)sh->sh_written, (u_longlong_t)sh->sh_enospc);
}

/* Report a suspended pool, which never resumes on these files, and exit. */
static void *
watchdog(void *arg)
{
	(void) arg;
	while (!sh->sh_done) {
		if (spa_suspended(wd_spa)) {
			sh->sh_suspended = 1;
			metaslab_class_t *mc = spa_normal_class(wd_spa);

			(void) printf("INFO fill: the pool is suspended "
			    "(failmode=wait) after %llu logical bytes "
			    "committed; class free (deflated) %llu, "
			    "fragmentation %llu%%\n",
			    (u_longlong_t)sh->sh_written,
			    (u_longlong_t)(metaslab_class_get_dspace(mc) -
			    metaslab_class_get_dalloc(mc)),
			    (u_longlong_t)metaslab_class_fragmentation(mc));
			report_fill("suspended");
			(void) fflush(stdout);
			_exit(3);
		}
		sh->sh_used = root_used(wd_spa);
		(void) usleep(50000);
	}
	return (NULL);
}

static int
child_fill(void *arg)
{
	void *buf = malloc(BIGTX);
	uint64_t obj = 0, off = FILLOBJ, seed = 1000;
	boolean_t big = B_TRUE;
	int in_a_row = 0;
	pthread_t wd;
	objset_t *os;
	spa_t *spa;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(try_import(dir, &spa));
	space_settle(spa);
	print_space(spa, "before the fill");
	print_usable(spa, "before the fill");
	sh->sh_avail = head_avail(spa);
	sh->sh_used0 = sh->sh_used = root_used(spa);
	wd_spa = spa;
	VERIFY0(pthread_create(&wd, NULL, watchdog, NULL));

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	while (in_a_row < 3) {
		uint64_t size = big ? BIGTX : BLKSZ;
		int err = 0;

		if (off + size > FILLOBJ) {
			err = new_object(os, BLKSZ, &obj);
			if (err == 0)
				off = 0;
		}
		if (err == 0)
			err = write_range(os, obj, off, size, seed++, buf);
		if (err == 0) {
			off += size;
			sh->sh_written += size;
			in_a_row = 0;
			continue;
		}
		VERIFY3S(err, ==, ENOSPC);
		sh->sh_enospc++;
		if (big)
			big = B_FALSE;
		else
			in_a_row++;
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	space_settle(spa);
	sh->sh_done = 1;
	VERIFY0(pthread_join(wd, NULL));
	sh->sh_used = root_used(spa);
	print_space(spa, "after the fill");
	print_usable(spa, "after the fill");
	report_fill("ENOSPC");
	export_pool(spa);
	kernel_fini();
	free(buf);
	return (0);
}

/*
 * zdb -b on the exported pool: B_TRUE if it finds no leak, which includes
 * its check that the redirect tables charge what the claims add.
 */
static boolean_t
run_zdb(void)
{
	const char *zdb = getenv("ZDB") != NULL ? getenv("ZDB") : "zdb";
	char cmd[PATH_BUF * 2];
	char line[4096];
	boolean_t noleaks = B_FALSE;
	int status;
	FILE *fp;

	(void) snprintf(cmd, sizeof (cmd), "%s -b -e -p %s %s 2>&1", zdb, dir,
	    POOL);
	(void) printf("INFO running %s\n", cmd);
	(void) fflush(stdout);
	fp = popen(cmd, "r");
	VERIFY3P(fp, !=, NULL);
	while (fgets(line, sizeof (line), fp) != NULL) {
		if (strstr(line, "No leaks (block sum matches space maps "
		    "exactly)") != NULL)
			noleaks = B_TRUE;
		if (strstr(line, "leak") != NULL ||
		    strstr(line, "SPIKE") != NULL ||
		    strstr(line, "error") != NULL)
			(void) printf("zdb| %s", line);
	}
	status = pclose(fp);
	(void) printf("INFO zdb exit status %d\n", WIFEXITED(status) ?
	    WEXITSTATUS(status) : 128 + WTERMSIG(status));
	return (noleaks && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int
main(int argc, char **argv)
{
	char path[PATH_BUF];
	int rc;

	if (argc < 4 || (strcmp(argv[3], "move") != 0 &&
	    strcmp(argv[3], "nomove") != 0)) {
		(void) fprintf(stderr, "usage: fill_probe <dir> <ashift> "
		    "<move|nomove> [filemb=N] [reck=N] [objs=N] [objmb=N] "
		    "[overw=N] [unaligned]\n");
		return (2);
	}
	for (int i = 4; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		uint64_t v;

		if (strcmp(argv[i], "unaligned") == 0 &&
		    &spike_redirect_keep_unaligned != NULL) {
			opt_unaligned = B_TRUE;
			continue;
		}
		if (eq == NULL)
			return (2);
		v = strtoull(eq + 1, NULL, 0);
		if (strncmp(argv[i], "filemb=", 7) == 0)
			FILE_SIZE = v << 20;
		else if (strncmp(argv[i], "reck=", 5) == 0)
			SMALL = v << 10;
		else if (strncmp(argv[i], "objs=", 5) == 0 && v <= MAXOBJS)
			OBJS = (int)v;
		else if (strncmp(argv[i], "objmb=", 6) == 0)
			OBJSZ = v << 20;
		else if (strncmp(argv[i], "overw=", 6) == 0)
			OVERW = (int)v;
		else
			return (2);
	}
	if (OVERW > OBJS)
		return (2);
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 0);
	opt_move = strcmp(argv[3], "move") == 0;
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	sh = mmap(NULL, sizeof (*sh), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	VERIFY3P(sh, !=, MAP_FAILED);
	memset(sh, 0, sizeof (*sh));
	(void) printf("INFO the kernel %s moved blocks to dspace\n",
	    vdev_raidz_redirect_dspace != NULL ? "charges" :
	    "does not charge");
	if (opt_unaligned)
		(void) printf("INFO P2 keeps one sector (unaligned)\n");
	(void) printf("INFO ashift %llu, %s, files of %llu MiB, %d objects "
	    "of %llu MiB in %llu K records, %d overwritten\n",
	    (u_longlong_t)ashift, argv[3], (u_longlong_t)(FILE_SIZE >> 20),
	    OBJS, (u_longlong_t)(OBJSZ >> 20), (u_longlong_t)(SMALL >> 10),
	    OVERW);

	VERIFY0(phase("0 setup", child_setup, NULL));
	if (opt_move) {
		VERIFY0(phase("move", child_move, NULL));
		check(sh->sh_moved > 0 && sh->sh_refused == 0 &&
		    sh->sh_gang == 0, "every old DVA outside the MOS moved "
		    "(%llu, %llu refused, %llu gang)",
		    (u_longlong_t)sh->sh_moved, (u_longlong_t)sh->sh_refused,
		    (u_longlong_t)sh->sh_gang);
	}
	rc = phase("fill", child_fill, NULL);
	check(rc == 0 && !sh->sh_suspended, "the fill ends in ENOSPC, the "
	    "pool not suspended");
	if (rc == 0) {
		double r = (double)(sh->sh_used - sh->sh_used0) /
		    (double)sh->sh_avail;

		check(r > 0.97 && r < 1.03, "the writes were charged "
		    "%.4f of what \"available\" said", r);
		check(run_zdb(), "zdb -b on the full pool: no leak");
	}
	for (int c = 0; c < WIDTH; c++) {
		(void) snprintf(path, sizeof (path), "%s/v%d", dir, c);
		(void) unlink(path);
	}
	(void) printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS",
	    failures);
	return (failures ? 1 : 0);
}
