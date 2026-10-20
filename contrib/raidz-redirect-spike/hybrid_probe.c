// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike (throwaway, not part of any tree): the hybrid mover, its
 * pre-flight check, and what a migration leaves of the free space.
 *
 *   hybrid_probe <dir> <ashift> <redirect|hybrid> [filemb=N] [reck=N]
 *       [objs=N] [overw=N] [new=N] [objmb=N] [steer] [force] [nofill]
 *       [destroy] [expect=refused]
 *
 * Setup: a five-wide raidz1 of five files, compression off; OBJS objects
 * of OBJSZ bytes in SMALL-byte records, snapshot @s1, the first OVERW of
 * them written again (the snapshot alone holds their old blocks), then NEW
 * more objects (head only); then a parity-2 epoch.  The default, objs=3
 * overw=3 new=4, makes 70 % of the old data the head's and 30 % the
 * snapshot's alone.
 *
 * Move: a census (traverse_pool) of every DVA outside the MOS born before
 * the epoch, the pre-flight check (vdev_raidz_migrate_preflight(), which
 * refuses unless the vdev can hold every block at the new parity and keep
 * the slop free; 'force' moves anyway), then:
 *  - redirect: every DVA is redirected, keeping the first sectors (P2);
 *  - hybrid: DVAs the head owns (its objset, logical birth after its
 *    previous snapshot) are rewritten through the DMU with
 *    dmu_buf_will_rewrite(), as zfs rewrite -P does, logical birth kept;
 *    the rest is redirected.  What the rewrite did not reach (metadata of
 *    the head with no rewritten child) is redirected by a second census.
 * A last census must count 0 old DVAs that no live redirect covers.
 * 'steer' places each redirect's new extent itself, metaslab by metaslab
 * in the order of the old extents, carved back to back from free segments
 * of the densest metaslab that has room.
 *
 * Fill: writes 128K records until ENOSPC three times in a row, as
 * fill_probe does, then counts the gang blocks among them; with
 * 'destroy', @s1 is destroyed first, which frees what was redirected.
 * Prints INFO, RESULT, PASS and FAIL lines; exits 1 if a check failed.
 * The pool files are deleted at the end.
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

#include <sys/dbuf.h>
#include <sys/dsl_destroy.h>

#define	POOL		"hyb"
#define	WIDTH		5
#define	BLKSZ		(128ULL << 10)
#define	MAXOBJS		64
#define	FILLOBJ		(64ULL << 20)
#define	BIGTX		(1ULL << 20)
#define	PATH_BUF	1024
#define	MAXMV		(1 << 17)
#define	BATCH		128
#define	RWCHUNK		(8ULL << 20)
/* Table metadata per redirect, raw: about 1.5K measured at ashift 12. */
#define	ENTRY_BYTES	2048

typedef struct shared {
	uint64_t	sh_epoch_txg;
	uint64_t	sh_head;	/* the head dataset's object */
	uint64_t	sh_prev_snap;	/* its previous snapshot's txg */
	int		sh_head_rw;	/* readable: not keyless */
	int		sh_refused_up_front;
	uint64_t	sh_moved;
	uint64_t	sh_refused;
	uint64_t	sh_gang;
	uint64_t	sh_rewritten;
	uint64_t	sh_left;	/* old DVAs no redirect covers, after */
	uint64_t	sh_entries;
	/* The fill. */
	uint64_t	sh_avail;
	uint64_t	sh_used0;
	uint64_t	sh_used;
	uint64_t	sh_written;
	uint64_t	sh_enospc;
	uint64_t	sh_fill_l0;
	uint64_t	sh_fill_gang;
	int		sh_suspended;
	int		sh_done;
} shared_t;

static shared_t *sh;
static const char *dir;
static uint64_t ashift;
static boolean_t opt_hybrid;
static boolean_t opt_steer;
static boolean_t opt_force;
static boolean_t opt_nofill;
static boolean_t opt_destroy;
static boolean_t opt_expect_refused;
static uint64_t FILE_SIZE = 320ULL << 20;	/* filemb */
static uint64_t SMALL = 128ULL << 10;		/* reck */
static int OBJS = 3;				/* objs */
static uint64_t OBJSZ = 32ULL << 20;		/* objmb */
static int OVERW = 3;				/* overw */
static int NEWOBJS = 4;				/* new */
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
	int64_t charge = vdev_raidz_redirect_dspace(spa);

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

/* A 128K block's asize at the new parity, and what fits it. */
typedef struct seg_arg {
	uint64_t	sa_big;		/* a 128K block's asize, new parity */
	uint64_t	sa_free;
	uint64_t	sa_segs;
	uint64_t	sa_big_free;	/* in segments that fit one */
} seg_arg_t;

static void
seg_cb(void *arg, uint64_t start, uint64_t size)
{
	seg_arg_t *sa = arg;

	(void) start;
	sa->sa_free += size;
	sa->sa_segs++;
	if (size >= sa->sa_big)
		sa->sa_big_free += size;
}

/* The class fragmentation, and the share of free space a 128K fits. */
static void
print_frag(spa_t *spa, const char *when)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	seg_arg_t sa = { 0 };

	sa.sa_big = vdev_psize_to_asize_txg(vd, BLKSZ,
	    spa_last_synced_txg(spa) + 1);
	for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
		metaslab_t *msp = vd->vdev_ms[m];

		mutex_enter(&msp->ms_lock);
		VERIFY0(metaslab_load(msp));
		zfs_range_tree_walk(msp->ms_allocatable, seg_cb, &sa);
		mutex_exit(&msp->ms_lock);
	}
	(void) printf("INFO frag %s: class fragmentation %llu%%, %llu free "
	    "bytes in %llu segments, %.1f%% of them in segments that hold a "
	    "128K block at the new parity (%llu bytes)\n", when,
	    (u_longlong_t)metaslab_class_fragmentation(spa_normal_class(spa)),
	    (u_longlong_t)sa.sa_free, (u_longlong_t)sa.sa_segs,
	    sa.sa_free ? 100.0 * sa.sa_big_free / sa.sa_free : 0.0,
	    (u_longlong_t)sa.sa_big);
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
	for (int i = 0; i < NEWOBJS; i++) {
		uint64_t o;

		VERIFY0(new_object(os, SMALL, &o));
		VERIFY0(write_range(os, o, 0, OBJSZ, 300 + i, buf));
	}
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
	    "@s1, %d overwritten, %d new; parity 2 from txg %llu\n", OBJS,
	    (u_longlong_t)(OBJSZ >> 20), (u_longlong_t)(SMALL >> 10), OVERW,
	    NEWOBJS, (u_longlong_t)sh->sh_epoch_txg);
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
	boolean_t	mv_rewrite;	/* the head owns it */
} mv_t;

#define	SET_BITS	20
typedef struct census_arg {
	vdev_t		*ca_vd;
	uint64_t	*ca_key;
	mv_t		*ca_mv;
	int		ca_nmv;
	uint64_t	ca_gang;
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

/* Old DVAs outside the MOS that no live redirect covers. */
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
		if (vdev_raidz_redirect_get_live(ca->ca_vd,
		    DVA_GET_OFFSET(dva), birth, NULL))
			continue;
		if (DVA_GET_GANG(dva)) {
			ca->ca_gang++;
			continue;
		}
		VERIFY3S(ca->ca_nmv, <, MAXMV);
		mv_t *m = &ca->ca_mv[ca->ca_nmv++];
		m->mv_bp = *bp;
		m->mv_zb = *zb;
		m->mv_dva = d;
		m->mv_rewrite = opt_hybrid && sh->sh_head_rw &&
		    zb->zb_objset == sh->sh_head &&
		    BP_GET_BIRTH(bp) > sh->sh_prev_snap;
	}
	return (0);
}

static void
census(spa_t *spa, census_arg_t *ca)
{
	memset(ca->ca_key, 0, (2ULL << SET_BITS) * sizeof (uint64_t));
	ca->ca_nmv = 0;
	ca->ca_gang = 0;
	ca->ca_vd = spa->spa_root_vdev->vdev_child[0];
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD,
	    census_cb, ca));
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

/* ---- steering: where to put N, carved from a dense metaslab ---- */

typedef struct run {
	uint64_t	r_cur;
	uint64_t	r_end;
} run_t;

typedef struct first_arg {
	uint64_t	fa_want;
	uint64_t	fa_start;
	uint64_t	fa_size;
} first_arg_t;

static void
first_cb(void *arg, uint64_t start, uint64_t size)
{
	first_arg_t *fa = arg;

	if (fa->fa_size == 0 && size >= fa->fa_want) {
		fa->fa_start = start;
		fa->fa_size = size;
	}
}

/* A free segment of at least want bytes in the densest metaslab. */
static boolean_t
run_pick(vdev_t *vd, uint64_t want, run_t *r)
{
	boolean_t *tried = calloc(vd->vdev_ms_count, sizeof (boolean_t));
	boolean_t found = B_FALSE;

	for (;;) {
		metaslab_t *best = NULL;
		first_arg_t fa = { .fa_want = want };

		for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *msp = vd->vdev_ms[m];

			if (tried[m] || msp->ms_size - msp->ms_allocated_space <
			    want)
				continue;
			if (best == NULL || msp->ms_allocated_space >
			    best->ms_allocated_space)
				best = msp;
		}
		if (best == NULL)
			break;
		tried[best->ms_id] = B_TRUE;
		mutex_enter(&best->ms_lock);
		VERIFY0(metaslab_load(best));
		zfs_range_tree_walk(best->ms_allocatable, first_cb, &fa);
		mutex_exit(&best->ms_lock);
		if (fa.fa_size != 0) {
			r->r_cur = fa.fa_start;
			r->r_end = fa.fa_start + fa.fa_size;
			found = B_TRUE;
			break;
		}
	}
	free(tried);
	return (found);
}

static int
mv_offset_cmp(const void *a, const void *b)
{
	const mv_t *x = a, *y = b;
	uint64_t ox = DVA_GET_OFFSET(&x->mv_bp.blk_dva[x->mv_dva]);
	uint64_t oy = DVA_GET_OFFSET(&y->mv_bp.blk_dva[y->mv_dva]);

	return (TREE_CMP(ox, oy));
}

typedef struct move_stat {
	uint64_t	ms_moved;
	uint64_t	ms_refused;
	uint64_t	ms_placed;
	uint64_t	ms_place_failed;
	int		ms_errs[8];
	int		ms_errn[8];
} move_stat_t;

/* Redirect the given DVAs, KEEP_FIRST; steered with opt_steer. */
static void
redirect(spa_t *spa, mv_t **list, int n, move_stat_t *st)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	raidz_redirect_move_t *rdm = calloc(BATCH, sizeof (*rdm));
	mv_t **retry = calloc(n + 1, sizeof (mv_t *));
	int nretry = 0;
	run_t run = { 0 };

	for (int pass = 0; pass < 2; pass++) {
		mv_t **l = pass == 0 ? list : retry;
		int ln = pass == 0 ? n : nretry;
		boolean_t steer = opt_steer && pass == 0;

		for (int i = 0; i < ln; ) {
			int b = 0;

			while (i < ln && b < BATCH) {
				mv_t *m = l[i++];
				uint64_t psize = BP_GET_PSIZE(&m->mv_bp);

				memset(&rdm[b], 0, sizeof (rdm[b]));
				rdm[b].rdm_bp = &m->mv_bp;
				rdm[b].rdm_zb = &m->mv_zb;
				rdm[b].rdm_psize = psize;
				rdm[b].rdm_dva = m->mv_dva;
				if (steer) {
					uint64_t a = vdev_psize_to_asize_txg(
					    vd, psize,
					    spa_last_synced_txg(spa) + 1);

					/*
					 * The tree does not show this batch's
					 * claims yet: pick anew in the next.
					 */
					if (run.r_end - run.r_cur < a &&
					    b > 0) {
						i--;
						break;
					}
					if (run.r_end - run.r_cur < a &&
					    !run_pick(vd, a, &run))
						run.r_cur = run.r_end = 0;
					if (run.r_end - run.r_cur >= a) {
						rdm[b].rdm_place = run.r_cur;
						run.r_cur += a;
					}
				}
				rdm[b].rdm_data = read_raw(spa, &m->mv_bp,
				    m->mv_dva, psize);
				b++;
			}
			VERIFY0(vdev_raidz_redirect_move(spa, 0, rdm, b,
			    RAIDZ_REDIRECT_KEEP_FIRST, B_FALSE));
			/* The txg's own metadata may have taken the rest. */
			run.r_cur = run.r_end = 0;
			for (int j = 0; j < b; j++) {
				int e = rdm[j].rdm_error;

				if (e == 0) {
					st->ms_moved++;
					if (rdm[j].rdm_place != 0)
						st->ms_placed++;
				} else if (rdm[j].rdm_place != 0) {
					/* Taken meanwhile: unplaced, later. */
					st->ms_place_failed++;
					retry[nretry++] = l[i - b + j];
					run.r_cur = run.r_end = 0;
				} else {
					st->ms_refused++;
					for (int k = 0; k < 8; k++) {
						if (st->ms_errn[k] == 0 ||
						    st->ms_errs[k] == e) {
							st->ms_errs[k] = e;
							st->ms_errn[k]++;
							break;
						}
					}
				}
				abd_free(rdm[j].rdm_data);
			}
		}
	}
	free(retry);
	free(rdm);
}

/*
 * Rewrite, as zfs rewrite -P does, every level-0 block of a plain file of
 * the head that the head owns and that is older than the epoch.
 */
static uint64_t
rewrite_head(spa_t *spa)
{
	objset_t *os;
	uint64_t obj = 0, n = 0;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	while (dmu_object_next(os, &obj, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		uint64_t bs, end;

		VERIFY0(dmu_object_info(os, obj, &doi));
		if (doi.doi_type != DMU_OT_PLAIN_FILE_CONTENTS)
			continue;
		bs = doi.doi_data_block_size;
		end = doi.doi_max_offset;
		for (uint64_t off = 0; off < end; off += RWCHUNK) {
			uint64_t len = MIN(RWCHUNK, end - off);
			dmu_tx_t *tx = dmu_tx_create(os);

			dmu_tx_hold_write(tx, obj, off, len);
			VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
			for (uint64_t o = off; o < off + len; o += bs) {
				dmu_buf_t *db;
				const blkptr_t *bp;

				VERIFY0(dmu_buf_hold(os, obj, o, FTAG, &db,
				    DMU_READ_PREFETCH));
				bp = ((dmu_buf_impl_t *)db)->db_blkptr;
				if (bp != NULL && !BP_IS_HOLE(bp) &&
				    BP_GET_PHYSICAL_BIRTH(bp) <
				    sh->sh_epoch_txg &&
				    BP_GET_BIRTH(bp) > sh->sh_prev_snap) {
					dmu_buf_will_rewrite(db, tx);
					n++;
				}
				dmu_buf_rele(db, FTAG);
			}
			dmu_tx_commit(tx);
		}
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
	return (n);
}

static uint64_t
obj_bytes(spa_t *spa, uint64_t obj)
{
	dmu_object_info_t doi;

	if (obj == 0 || dmu_object_info(spa->spa_meta_objset, obj, &doi) != 0)
		return (0);
	return (doi.doi_physical_blocks_512 << 9);
}

static void
print_need(const raidz_migrate_need_t *r)
{
	(void) printf("INFO preflight: %llu DVAs to redirect (old %llu, new "
	    "%llu, kept %llu), %llu to rewrite (old %llu, new %llu); table "
	    "metadata %llu per entry\n", (u_longlong_t)r->rmn_nredirect,
	    (u_longlong_t)r->rmn_redirect_old,
	    (u_longlong_t)r->rmn_redirect_new,
	    (u_longlong_t)r->rmn_redirect_keep,
	    (u_longlong_t)r->rmn_nrewrite, (u_longlong_t)r->rmn_rewrite_old,
	    (u_longlong_t)r->rmn_rewrite_new,
	    (u_longlong_t)r->rmn_entry_bytes);
	(void) printf("INFO preflight: free %llu, slop %llu (raw); the end "
	    "state needs %llu more; first fit packs %llu of the %llu new "
	    "redirect extents, leaving %llu usable for %llu\n",
	    (u_longlong_t)r->rmn_free, (u_longlong_t)r->rmn_slop,
	    (u_longlong_t)r->rmn_need_end, (u_longlong_t)r->rmn_packed,
	    (u_longlong_t)r->rmn_redirect_new, (u_longlong_t)r->rmn_left,
	    (u_longlong_t)r->rmn_need_left);
}

static int
child_move(void *arg)
{
	census_arg_t ca = { 0 };
	raidz_migrate_need_t rmn = { 0 };
	move_stat_t st = { 0 };
	mv_t **red, **rw;
	int nred = 0, nrw = 0, err, round;
	uint64_t alloc0;
	vdev_t *vd;
	spa_t *spa;
	hrtime_t t0, t1, t2;

	(void) arg;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(try_import(dir, &spa));
	vd = spa->spa_root_vdev->vdev_child[0];
	space_settle(spa);
	print_space(spa, "before the move");
	print_frag(spa, "before the move");
	{
		dsl_pool_t *dp = spa_get_dsl(spa);
		dsl_dataset_t *ds;

		dsl_pool_config_enter(dp, FTAG);
		VERIFY0(dsl_dataset_hold(dp, POOL, FTAG, &ds));
		sh->sh_head = ds->ds_object;
		sh->sh_prev_snap = dsl_dataset_phys(ds)->ds_prev_snap_txg;
		dsl_dataset_rele(ds, FTAG);
		dsl_pool_config_exit(dp, FTAG);
	}
	/* A keyless encrypted head cannot be rewritten: redirect it all. */
	if (opt_hybrid) {
		objset_t *os;

		if (dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
		    &os) == 0) {
			sh->sh_head_rw = 1;
			dmu_objset_disown(os, B_TRUE, FTAG);
		}
	}
	ca.ca_key = calloc(2ULL << SET_BITS, sizeof (uint64_t));
	ca.ca_mv = malloc(MAXMV * sizeof (mv_t));
	red = malloc(MAXMV * sizeof (mv_t *));
	rw = malloc(MAXMV * sizeof (mv_t *));

	t0 = gethrtime();
	census(spa, &ca);
	if (opt_steer)
		qsort(ca.ca_mv, ca.ca_nmv, sizeof (mv_t), mv_offset_cmp);
	rmn.rmn_txg = spa_last_synced_txg(spa) + 1;
	rmn.rmn_entry_bytes = ENTRY_BYTES;
	for (int i = 0; i < ca.ca_nmv; i++) {
		mv_t *m = &ca.ca_mv[i];

		vdev_raidz_migrate_need_add(vd, &rmn, &m->mv_bp, m->mv_dva,
		    !m->mv_rewrite);
		if (m->mv_rewrite)
			rw[nrw++] = m;
		else
			red[nred++] = m;
	}
	sh->sh_gang = ca.ca_gang;
	err = vdev_raidz_migrate_preflight(vd, &rmn);
	print_need(&rmn);
	(void) printf("INFO census: %d old DVAs outside the MOS (%d to "
	    "redirect, %d the head owns), %llu gang, in %.2f s\n", ca.ca_nmv,
	    nred, nrw, (u_longlong_t)ca.ca_gang,
	    (double)(gethrtime() - t0) / 1e9);
	if (err != 0) {
		uint64_t room = rmn.rmn_free - MIN(rmn.rmn_free, rmn.rmn_slop);

		(void) printf("REFUSED parity 2 does not fit on this vdev with "
		    "the slop kept free (error %d): the end state needs %llu "
		    "bytes, %llu are free above the slop (%s); %llu of the "
		    "%llu bytes of new extents fit in the free segments as "
		    "they stand (%s); after them %llu bytes stay usable, "
		    "%llu are needed for the slop, the table and the "
		    "rewrites (%s)\n", err, (u_longlong_t)rmn.rmn_need_end,
		    (u_longlong_t)room,
		    rmn.rmn_need_end <= room ? "fits" : "short",
		    (u_longlong_t)rmn.rmn_packed,
		    (u_longlong_t)rmn.rmn_redirect_new,
		    rmn.rmn_packed == rmn.rmn_redirect_new ? "fits" :
		    "short", (u_longlong_t)rmn.rmn_left,
		    (u_longlong_t)rmn.rmn_need_left,
		    rmn.rmn_left >= rmn.rmn_need_left ? "fits" : "short");
		if (!opt_force) {
			sh->sh_refused_up_front = 1;
			export_pool(spa);
			kernel_fini();
			return (4);
		}
		(void) printf("INFO force: moving anyway\n");
	}
	alloc0 = vd->vdev_stat.vs_alloc;

	t1 = gethrtime();
	redirect(spa, red, nred, &st);
	t2 = gethrtime();
	(void) printf("INFO redirect: %llu DVAs moved, %llu refused, %llu "
	    "placed by steering (%llu retried unplaced), in %.2f s\n",
	    (u_longlong_t)st.ms_moved, (u_longlong_t)st.ms_refused,
	    (u_longlong_t)st.ms_placed, (u_longlong_t)st.ms_place_failed,
	    (double)(t2 - t1) / 1e9);
	if (nrw > 0) {
		sh->sh_rewritten = rewrite_head(spa);
		(void) printf("INFO rewrite: %llu level-0 blocks rewritten "
		    "(of %d head DVAs counted), in %.2f s\n",
		    (u_longlong_t)sh->sh_rewritten, nrw,
		    (double)(gethrtime() - t2) / 1e9);
	}
	/* What neither reached: head metadata with nothing rewritten. */
	for (round = 0; round < 3; round++) {
		int nl = 0;

		census(spa, &ca);
		if (ca.ca_nmv == 0)
			break;
		for (int i = 0; i < ca.ca_nmv; i++)
			red[nl++] = &ca.ca_mv[i];
		(void) printf("INFO round %d: %d old DVAs left, redirected\n",
		    round + 1, nl);
		redirect(spa, red, nl, &st);
	}
	t2 = gethrtime();
	census(spa, &ca);
	sh->sh_left = ca.ca_nmv + ca.ca_gang;
	sh->sh_moved = st.ms_moved;
	sh->sh_refused = st.ms_refused;
	sh->sh_entries = vdev_raidz_redirect_count(spa, 0);
	(void) printf("INFO move: %llu DVAs redirected, %llu refused, %llu "
	    "blocks rewritten, migration %.2f s (census included)\n",
	    (u_longlong_t)st.ms_moved, (u_longlong_t)st.ms_refused,
	    (u_longlong_t)sh->sh_rewritten, (double)(t2 - t0) / 1e9);
	for (int k = 0; k < 8 && st.ms_errn[k] != 0; k++) {
		(void) printf("INFO move: error %d (%s) for %d DVAs\n",
		    st.ms_errs[k], strerror(st.ms_errs[k]), st.ms_errn[k]);
	}
	space_settle(spa);
	{
		vdev_raidz_t *vdrz = vd->vdev_tsd;
		uint64_t lb = obj_bytes(spa, vdrz->vd_redirect_log);
		uint64_t zb = obj_bytes(spa, vdrz->vd_redirect_obj);

		(void) printf("RESULT table: %llu entries, log %llu bytes, "
		    "block-pointer ZAP %llu bytes, %.0f bytes per entry\n",
		    (u_longlong_t)sh->sh_entries, (u_longlong_t)lb,
		    (u_longlong_t)zb, sh->sh_entries ?
		    (double)(lb + zb) / sh->sh_entries : 0.0);
	}
	(void) printf("RESULT census after the move: %llu old DVAs that no "
	    "redirect covers\n", (u_longlong_t)sh->sh_left);
	(void) printf("INFO preflight: the vdev's allocation grew by %lld, "
	    "the check predicted %llu\n",
	    (longlong_t)(vd->vdev_stat.vs_alloc - alloc0),
	    (u_longlong_t)rmn.rmn_need_end);
	print_space(spa, "after the move");
	print_frag(spa, "after the move");
	export_pool(spa);
	kernel_fini();
	free(ca.ca_key);
	free(ca.ca_mv);
	free(red);
	free(rw);
	return (0);
}

/* ---- the fill ---- */

static void
report_fill(const char *how)
{
	double c = (double)(sh->sh_used - sh->sh_used0);

	(void) printf("RESULT %s %s: available before %llu, charged %llu, "
	    "charged/available %.4f, logical written %llu, ENOSPC returns "
	    "%llu\n", opt_hybrid ? "hybrid" : "redirect", how,
	    (u_longlong_t)sh->sh_avail, (u_longlong_t)(sh->sh_used -
	    sh->sh_used0), c / (double)sh->sh_avail,
	    (u_longlong_t)sh->sh_written, (u_longlong_t)sh->sh_enospc);
}

static void *
watchdog(void *arg)
{
	(void) arg;
	while (!sh->sh_done) {
		if (spa_suspended(wd_spa)) {
			sh->sh_suspended = 1;
			(void) printf("INFO fill: the pool is suspended after "
			    "%llu logical bytes\n",
			    (u_longlong_t)sh->sh_written);
			report_fill("suspended");
			(void) fflush(stdout);
			_exit(3);
		}
		sh->sh_used = root_used(wd_spa);
		(void) usleep(50000);
	}
	return (NULL);
}

static uint64_t fill_txg0;

static int
gang_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) zilog, (void) dnp, (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    zb->zb_objset != sh->sh_head || zb->zb_level != 0 ||
	    BP_GET_TYPE(bp) != DMU_OT_PLAIN_FILE_CONTENTS ||
	    BP_GET_PHYSICAL_BIRTH(bp) < fill_txg0)
		return (0);
	sh->sh_fill_l0++;
	if (DVA_GET_GANG(&bp->blk_dva[0]))
		sh->sh_fill_gang++;
	return (0);
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
	if (opt_destroy) {
		VERIFY0(dsl_destroy_snapshot(POOL "@s1", B_FALSE));
		space_settle(spa);
		(void) printf("INFO destroyed @s1: %llu redirect entries "
		    "live\n", (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	}
	print_frag(spa, "before the fill");
	fill_txg0 = spa_last_synced_txg(spa) + 1;
	sh->sh_avail = head_avail(spa);
	sh->sh_used0 = sh->sh_used = root_used(spa);
	{
		dsl_pool_t *dp = spa_get_dsl(spa);
		dsl_dataset_t *ds;

		dsl_pool_config_enter(dp, FTAG);
		VERIFY0(dsl_dataset_hold(dp, POOL, FTAG, &ds));
		sh->sh_head = ds->ds_object;
		dsl_dataset_rele(ds, FTAG);
		dsl_pool_config_exit(dp, FTAG);
	}
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
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE, gang_cb, NULL));
	(void) printf("RESULT fill gang: %llu of %llu new 128K blocks are "
	    "gang blocks (%.2f%%)\n", (u_longlong_t)sh->sh_fill_gang,
	    (u_longlong_t)sh->sh_fill_l0, sh->sh_fill_l0 ?
	    100.0 * sh->sh_fill_gang / sh->sh_fill_l0 : 0.0);
	print_space(spa, "after the fill");
	print_frag(spa, "after the fill");
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

	if (argc < 4 || (strcmp(argv[3], "redirect") != 0 &&
	    strcmp(argv[3], "hybrid") != 0)) {
		(void) fprintf(stderr, "usage: hybrid_probe <dir> <ashift> "
		    "<redirect|hybrid> [filemb=N] [reck=N] [objs=N] "
		    "[overw=N] [new=N] [objmb=N] [steer] [force] [nofill] "
		    "[destroy] [expect=refused]\n");
		return (2);
	}
	for (int i = 4; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		uint64_t v;

		if (strcmp(argv[i], "steer") == 0) {
			opt_steer = B_TRUE;
			continue;
		}
		if (strcmp(argv[i], "force") == 0) {
			opt_force = B_TRUE;
			continue;
		}
		if (strcmp(argv[i], "destroy") == 0) {
			opt_destroy = B_TRUE;
			continue;
		}
		if (strcmp(argv[i], "nofill") == 0) {
			opt_nofill = B_TRUE;
			continue;
		}
		if (strcmp(argv[i], "expect=refused") == 0) {
			opt_expect_refused = B_TRUE;
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
		else if (strncmp(argv[i], "new=", 4) == 0 && v <= MAXOBJS)
			NEWOBJS = (int)v;
		else
			return (2);
	}
	if (OVERW > OBJS)
		return (2);
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 0);
	opt_hybrid = strcmp(argv[3], "hybrid") == 0;
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	sh = mmap(NULL, sizeof (*sh), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	VERIFY3P(sh, !=, MAP_FAILED);
	memset(sh, 0, sizeof (*sh));
	(void) printf("INFO ashift %llu, %s%s%s, files of %llu MiB, %d "
	    "objects of %llu MiB in %llu K records, %d overwritten, %d new\n",
	    (u_longlong_t)ashift, argv[3], opt_steer ? ", steered" : "",
	    opt_force ? ", forced" : "", (u_longlong_t)(FILE_SIZE >> 20),
	    OBJS, (u_longlong_t)(OBJSZ >> 20), (u_longlong_t)(SMALL >> 10),
	    OVERW, NEWOBJS);

	VERIFY0(phase("0 setup", child_setup, NULL));
	rc = phase("move", child_move, NULL);
	if (opt_expect_refused) {
		check(rc == 4 && sh->sh_refused_up_front, "the pre-flight "
		    "check refuses the migration before moving anything");
		goto out;
	}
	check(rc == 0, "the pre-flight check accepts the migration, or it "
	    "was forced");
	if (rc != 0)
		goto out;
	check(sh->sh_refused == 0, "no move refused (%llu)",
	    (u_longlong_t)sh->sh_refused);
	check(sh->sh_left == 0, "the census counts 0 old DVAs left (%llu)",
	    (u_longlong_t)sh->sh_left);
	if (opt_nofill)
		goto out;
	rc = phase("fill", child_fill, NULL);
	check(rc == 0 && !sh->sh_suspended, "the fill ends in ENOSPC, the "
	    "pool not suspended");
	if (rc == 0) {
		double r = (double)(sh->sh_used - sh->sh_used0) /
		    (double)sh->sh_avail;

		check(r > 0.97 && r < 1.03, "the writes were charged "
		    "%.4f of what \"available\" said", r);
	}
out:
	if (rc == 0 || rc == 4)
		check(run_zdb(), "zdb -b: no leak");
	for (int c = 0; c < WIDTH; c++) {
		(void) snprintf(path, sizeof (path), "%s/v%d", dir, c);
		(void) unlink(path);
	}
	(void) printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS",
	    failures);
	return (failures ? 1 : 0);
}
