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
 * SPIKE, not for merge: the hybrid mover of
 * contrib/raidz-redirect-spike/hybrid_probe.c, in the module, so that a
 * kernel can run it, and a test-only trigger for it.
 *
 * The mover is the probe's: a census (traverse_pool()) of every DVA of the
 * raidz vdev 0 outside the MOS born before its last parity epoch, the
 * pre-flight check, then the redirect of every such DVA (P2), or, for the
 * hybrid, the rewrite (dmu_buf_will_rewrite(), as zfs rewrite -P does) of
 * the level-0 blocks of plain files that the head dataset owns and the
 * redirect of the rest; up to three more census rounds redirect what the
 * rewrite did not reach, and a last census must count 0.  Two things are
 * crypt_probe.c's: the census reads with TRAVERSE_NO_DECRYPT, so that a
 * dataset whose key is not loaded is moved too, and it takes gang headers
 * and their children ("nogang" leaves them, as hybrid_probe does).  ZIL
 * blocks are never moved; the census counts the old ones (zil_old).  An
 * error that the probe would have asserted is counted and reported.
 *
 * The trigger is a write to a module parameter, on Linux only:
 *
 *	echo "epoch <pool>" > /sys/module/zfs/parameters/raidz_spike_mover
 *	echo "move <pool> <hybrid|redirect> [head=<dataset>] [force]
 *	    [nogang]" > ...
 *	echo "census <pool>" > ...
 *	cat /sys/module/zfs/parameters/raidz_spike_mover
 *
 * epoch adds hybrid_probe's parity epoch, {0, width, p, T, width, p + 1},
 * to a raidz vdev 0 without one.  A command runs in a thread of its own;
 * the write returns at once, EBUSY while one runs.  Reading the parameter
 * gives "running <command>", "idle", or the last result, one line of
 * key=value pairs, also sent to zfs_dbgmsg().  head defaults to the pool's
 * root dataset; the hybrid needs feature@physical_rewrite enabled and a
 * head that is not mounted, or it redirects everything (head_rw=0).
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dbuf.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zfeature.h>

/* hybrid_probe.c's sizes. */
#define	RZM_SET_BITS	20
#define	RZM_SET_SLOTS	(1ULL << RZM_SET_BITS)
#define	RZM_MAXMV	(1 << 17)
#define	RZM_BATCH	128
#define	RZM_RWCHUNK	(8ULL << 20)
#define	RZM_ENTRY_BYTES	2048
#define	RZM_ROUNDS	3
#define	RZM_GANG_DEPTH	8
#define	RZM_NERR	8
#define	RZM_RESULT	1024

typedef enum rzm_cmd {
	RZM_EPOCH,
	RZM_MOVE,
	RZM_CENSUS,
} rzm_cmd_t;

typedef struct rzm_mv {
	blkptr_t	mv_bp;
	zbookmark_phys_t mv_zb;
	int		mv_dva;
	boolean_t	mv_rewrite;	/* the head owns it */
} rzm_mv_t;

typedef struct rzm {
	rzm_cmd_t	rz_cmd;
	char		rz_pool[ZFS_MAX_DATASET_NAME_LEN];
	char		rz_head[ZFS_MAX_DATASET_NAME_LEN];
	boolean_t	rz_hybrid;
	boolean_t	rz_gang;	/* take gang headers and children */
	boolean_t	rz_force;	/* move though the pre-flight refused */

	spa_t		*rz_spa;
	vdev_t		*rz_vd;
	uint64_t	rz_epoch_txg;
	uint64_t	rz_head_obj;
	uint64_t	rz_prev_snap;
	boolean_t	rz_head_rw;
	uint64_t	rz_gsize;	/* a gang header's psize */
	uint64_t	*rz_key;	/* the census's set of DVAs */
	rzm_mv_t	*rz_mv;		/* NULL: the census only counts */
	int		rz_nmv;

	/* What the last census found besides rz_nmv. */
	int		rz_census_err;
	uint64_t	rz_gang_dvas;	/* old gang headers and children */
	uint64_t	rz_gang_left;	/* of them, not taken */
	uint64_t	rz_zil_old;	/* old ZIL DVAs, never moved */
	uint64_t	rz_live;	/* DVAs with a live redirect */
	uint64_t	rz_kept;	/* their rde_old_reserved */

	uint64_t	rz_moved;
	uint64_t	rz_refused;
	uint64_t	rz_read_err;
	int		rz_errs[RZM_NERR];
	uint64_t	rz_errn[RZM_NERR];
	uint64_t	rz_rewritten;
	int		rz_rewrite_err;
	int		rz_nround[RZM_ROUNDS];
} rzm_t;

static boolean_t
rzm_set_add(rzm_t *rz, uint64_t offset, uint64_t birth)
{
	uint64_t mask = RZM_SET_SLOTS - 1;
	uint64_t h = (offset * 0x9e3779b97f4a7c15ULL ^ birth) & mask;

	for (uint64_t n = 0; n < RZM_SET_SLOTS; n++) {
		uint64_t *k = &rz->rz_key[2 * h];

		if (k[0] == 0) {
			k[0] = offset + 1;
			k[1] = birth;
			return (B_TRUE);
		}
		if (k[0] == offset + 1 && k[1] == birth)
			return (B_FALSE);
		h = (h + 1) & mask;
	}
	/* Full: the census can no longer tell what it has seen. */
	if (rz->rz_census_err == 0)
		rz->rz_census_err = SET_ERROR(EOVERFLOW);
	return (B_FALSE);
}

/* The bytes at DVA d of bp as stored: no checksum, no transform. */
static abd_t *
rzm_read_raw(spa_t *spa, const blkptr_t *bp, int d, uint64_t psize,
    boolean_t meta, int *errp)
{
	blkptr_t rbp = *bp;
	abd_t *abd = abd_alloc_linear(psize, meta);

	rbp.blk_dva[0] = bp->blk_dva[d];
	for (int i = 1; i < SPA_DVAS_PER_BP; i++)
		memset(&rbp.blk_dva[i], 0, sizeof (dva_t));
	DVA_SET_GANG(&rbp.blk_dva[0], 0);
	BP_SET_LSIZE(&rbp, psize);
	BP_SET_PSIZE(&rbp, psize);
	BP_SET_COMPRESS(&rbp, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(&rbp, ZIO_CHECKSUM_OFF);
	BP_SET_DEDUP(&rbp, 0);
	BP_SET_CRYPT(&rbp, 0);
	ZIO_SET_CHECKSUM(&rbp.blk_cksum, 0, 0, 0, 0);
	*errp = zio_wait(zio_read(NULL, spa, &rbp, abd, psize, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, NULL));
	return (abd);
}

static void rzm_count_bp(rzm_t *, const blkptr_t *, const zbookmark_phys_t *,
    int);

/*
 * The children of the gang block bp, from its header's first copy, which
 * must match the checksum embedded in it before its block pointers are
 * believed (vdev_raidz_redirect_in_gang() does the same).
 */
static void
rzm_count_gang(rzm_t *rz, const blkptr_t *bp, const zbookmark_phys_t *zb,
    int depth)
{
	uint64_t gsize = rz->rz_gsize;
	abd_t *abd;
	int err;

	if (depth >= RZM_GANG_DEPTH) {
		if (rz->rz_census_err == 0)
			rz->rz_census_err = SET_ERROR(ELOOP);
		return;
	}
	abd = rzm_read_raw(rz->rz_spa, bp, 0, gsize, B_TRUE, &err);
	if (err == 0) {
		err = zio_checksum_error_impl(rz->rz_spa, bp,
		    ZIO_CHECKSUM_GANG_HEADER, abd, gsize, 0, NULL);
	}
	if (err == 0) {
		zio_gbh_phys_t *gbh = abd_to_buf(abd);

		if (BP_SHOULD_BYTESWAP(bp))
			byteswap_uint64_array(gbh, gsize);
		for (int g = 0; g < gbh_nblkptrs(gsize); g++)
			rzm_count_bp(rz, gbh_bp(gbh, g), zb, depth + 1);
	} else if (rz->rz_census_err == 0) {
		rz->rz_census_err = err;
	}
	abd_free(abd);
}

static void
rzm_count_bp(rzm_t *rz, const blkptr_t *bp, const zbookmark_phys_t *zb,
    int depth)
{
	uint64_t birth;

	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return;
	birth = BP_GET_PHYSICAL_BIRTH(bp);
	if (birth >= rz->rz_epoch_txg)
		return;
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		raidz_redirect_ent_t ent;
		rzm_mv_t *m;

		if (DVA_GET_VDEV(dva) != rz->rz_vd->vdev_id ||
		    !rzm_set_add(rz, DVA_GET_OFFSET(dva), birth))
			continue;
		if (vdev_raidz_redirect_get_live(rz->rz_vd,
		    DVA_GET_OFFSET(dva), birth, &ent)) {
			rz->rz_live++;
			rz->rz_kept += ent.rde_old_reserved;
			continue;
		}
		if (DVA_GET_GANG(dva) || depth > 0)
			rz->rz_gang_dvas++;
		if (DVA_GET_GANG(dva) && !rz->rz_gang) {
			rz->rz_gang_left++;
			continue;
		}
		if (rz->rz_mv == NULL) {
			rz->rz_nmv++;
			continue;
		}
		if (rz->rz_nmv >= RZM_MAXMV) {
			if (rz->rz_census_err == 0)
				rz->rz_census_err = SET_ERROR(EOVERFLOW);
			continue;
		}
		m = &rz->rz_mv[rz->rz_nmv++];
		m->mv_bp = *bp;
		m->mv_zb = *zb;
		m->mv_dva = d;
		m->mv_rewrite = rz->rz_hybrid && rz->rz_head_rw &&
		    zb->zb_objset == rz->rz_head_obj &&
		    BP_GET_BIRTH(bp) > rz->rz_prev_snap;
	}
	if (rz->rz_gang && BP_IS_GANG(bp))
		rzm_count_gang(rz, bp, zb, depth);
}

static int
rzm_census_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	rzm_t *rz = arg;

	(void) spa, (void) zilog, (void) dnp;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    zb->zb_objset == DMU_META_OBJSET)
		return (0);
	if (zb->zb_level == ZB_ZIL_LEVEL) {
		if (BP_GET_PHYSICAL_BIRTH(bp) < rz->rz_epoch_txg)
			rz->rz_zil_old += BP_GET_NDVAS(bp);
		return (0);
	}
	rzm_count_bp(rz, bp, zb, 0);
	return (0);
}

static int
rzm_census(rzm_t *rz)
{
	int err;

	memset(rz->rz_key, 0, 2 * RZM_SET_SLOTS * sizeof (uint64_t));
	rz->rz_nmv = 0;
	rz->rz_census_err = 0;
	rz->rz_gang_dvas = rz->rz_gang_left = rz->rz_zil_old = 0;
	rz->rz_live = rz->rz_kept = 0;
	err = traverse_pool(rz->rz_spa, 0, TRAVERSE_PRE | TRAVERSE_HARD |
	    TRAVERSE_NO_DECRYPT, rzm_census_cb, rz);
	if (err != 0 && rz->rz_census_err == 0)
		rz->rz_census_err = err;
	return (rz->rz_census_err);
}

static void
rzm_count_err(rzm_t *rz, int e)
{
	for (int k = 0; k < RZM_NERR; k++) {
		if (rz->rz_errn[k] == 0 || rz->rz_errs[k] == e) {
			rz->rz_errs[k] = e;
			rz->rz_errn[k]++;
			return;
		}
	}
}

/* hybrid_probe's redirect(), without steering: P2, batches of 128. */
static void
rzm_redirect(rzm_t *rz, rzm_mv_t **list, int n)
{
	spa_t *spa = rz->rz_spa;
	raidz_redirect_move_t *rdm = kmem_zalloc(RZM_BATCH * sizeof (*rdm),
	    KM_SLEEP);

	for (int i = 0; i < n; ) {
		int b = 0;
		int err;

		while (i < n && b < RZM_BATCH) {
			rzm_mv_t *m = list[i++];
			boolean_t gang =
			    DVA_GET_GANG(&m->mv_bp.blk_dva[m->mv_dva]);
			uint64_t psize = gang ? rz->rz_gsize :
			    BP_GET_PSIZE(&m->mv_bp);
			abd_t *abd = rzm_read_raw(spa, &m->mv_bp, m->mv_dva,
			    psize, gang, &err);

			if (err != 0) {
				abd_free(abd);
				rz->rz_read_err++;
				rz->rz_refused++;
				rzm_count_err(rz, err);
				continue;
			}
			memset(&rdm[b], 0, sizeof (rdm[b]));
			rdm[b].rdm_bp = &m->mv_bp;
			rdm[b].rdm_zb = &m->mv_zb;
			rdm[b].rdm_psize = psize;
			rdm[b].rdm_dva = m->mv_dva;
			rdm[b].rdm_data = abd;
			b++;
		}
		if (b == 0)
			continue;
		err = vdev_raidz_redirect_move(spa, rz->rz_vd->vdev_id, rdm, b,
		    RAIDZ_REDIRECT_KEEP_FIRST, B_FALSE);
		for (int j = 0; j < b; j++) {
			int e = err != 0 ? err : rdm[j].rdm_error;

			if (e == 0) {
				rz->rz_moved++;
			} else {
				rz->rz_refused++;
				rzm_count_err(rz, e);
			}
			abd_free(rdm[j].rdm_data);
		}
	}
	kmem_free(rdm, RZM_BATCH * sizeof (*rdm));
}

/*
 * hybrid_probe's rewrite_head(): rewrite, as zfs rewrite -P does, every
 * level-0 block of a plain file of the head that the head owns and that is
 * older than the epoch.
 */
static int
rzm_rewrite_head(rzm_t *rz)
{
	objset_t *os;
	uint64_t obj = 0;
	int err;

	err = dmu_objset_own(rz->rz_head, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os);
	if (err != 0)
		return (err);
	while (err == 0 && dmu_object_next(os, &obj, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		uint64_t bs, end;

		if (dmu_object_info(os, obj, &doi) != 0 ||
		    doi.doi_type != DMU_OT_PLAIN_FILE_CONTENTS)
			continue;
		bs = doi.doi_data_block_size;
		end = doi.doi_max_offset;
		for (uint64_t off = 0; err == 0 && off < end;
		    off += RZM_RWCHUNK) {
			uint64_t len = MIN(RZM_RWCHUNK, end - off);
			dmu_tx_t *tx = dmu_tx_create(os);

			dmu_tx_hold_write(tx, obj, off, len);
			err = dmu_tx_assign(tx, DMU_TX_WAIT);
			if (err != 0) {
				dmu_tx_abort(tx);
				break;
			}
			for (uint64_t o = off; o < off + len; o += bs) {
				dmu_buf_t *db;
				const blkptr_t *bp;

				err = dmu_buf_hold(os, obj, o, FTAG, &db,
				    DMU_READ_PREFETCH);
				if (err != 0)
					break;
				bp = ((dmu_buf_impl_t *)db)->db_blkptr;
				if (bp != NULL && !BP_IS_HOLE(bp) &&
				    BP_GET_PHYSICAL_BIRTH(bp) <
				    rz->rz_epoch_txg &&
				    BP_GET_BIRTH(bp) > rz->rz_prev_snap) {
					dmu_buf_will_rewrite(db, tx);
					rz->rz_rewritten++;
				}
				dmu_buf_rele(db, FTAG);
			}
			dmu_tx_commit(tx);
		}
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(rz->rz_spa), 0);
	return (err);
}

/* hybrid_probe's space_settle(): the deferred frees reach the space maps. */
static void
rzm_settle(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);

	for (int i = 0; i < TXG_DEFER_SIZE + 2; i++) {
		dmu_tx_t *tx = dmu_tx_create_dd(dp->dp_mos_dir);

		if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
			dmu_tx_abort(tx);
			return;
		}
		dmu_tx_commit(tx);
		txg_wait_synced(dp, 0);
	}
}

/* The start of the last parity epoch of vd, 0 if it has fewer than two. */
static uint64_t
rzm_last_epoch(vdev_t *vd)
{
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	uint64_t start = 0;

	mutex_enter(&vdrz->vd_expand_lock);
	if (vdrz->vd_parity_epochs != NULL && vdrz->vd_parity_epoch_count > 1) {
		start = vdrz->vd_parity_epochs[3 *
		    (vdrz->vd_parity_epoch_count - 1)];
	}
	mutex_exit(&vdrz->vd_expand_lock);
	return (start);
}

typedef struct rzm_epoch_arg {
	uint64_t	ea_table[6];
} rzm_epoch_arg_t;

static int
rzm_epoch_check(void *arg, dmu_tx_t *tx)
{
	rzm_epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	if (!dmu_tx_is_syncing(tx))
		return (0);
	ea->ea_table[3] = vdev_raidz_parity_epochs_first_start(tx);
	return (vdev_raidz_parity_epochs_set_check_tx(vd, ea->ea_table, 2, tx,
	    NULL));
}

static void
rzm_epoch_sync(void *arg, dmu_tx_t *tx)
{
	rzm_epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	VERIFY0(vdev_raidz_parity_epochs_set(vd, ea->ea_table, 2, tx));
}

static int
rzm_epoch(rzm_t *rz, char *buf, size_t size)
{
	vdev_t *vd = rz->rz_vd;
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	rzm_epoch_arg_t ea = { { 0 } };
	int err;

	if (vdev_raidz_has_parity_epochs(vd))
		return (SET_ERROR(EEXIST));
	ea.ea_table[1] = ea.ea_table[4] = vd->vdev_children;
	ea.ea_table[2] = vdrz->vd_nparity;
	ea.ea_table[5] = vdrz->vd_nparity + 1;
	err = dsl_sync_task(rz->rz_pool, rzm_epoch_check, rzm_epoch_sync, &ea,
	    0, ZFS_SPACE_CHECK_NONE);
	if (err == 0)
		txg_wait_synced(spa_get_dsl(rz->rz_spa), ea.ea_table[3]);
	(void) snprintf(buf, size, " epoch_txg=%llu width=%llu parity=%llu",
	    (u_longlong_t)ea.ea_table[3], (u_longlong_t)ea.ea_table[1],
	    (u_longlong_t)ea.ea_table[5]);
	return (err);
}

/* What the last census found, and the table. */
static void
rzm_print_census(rzm_t *rz, char *buf, size_t size)
{
	spa_t *spa = rz->rz_spa;
	uint64_t v = rz->rz_vd->vdev_id;

	(void) snprintf(buf, size, " census_err=%d census_left=%llu "
	    "gang_dvas=%llu gang_left=%llu zil_old=%llu live=%llu kept=%llu "
	    "entries=%llu tombstones=%llu damaged=%llu extra=%lld",
	    rz->rz_census_err, (u_longlong_t)(rz->rz_nmv + rz->rz_gang_left),
	    (u_longlong_t)rz->rz_gang_dvas, (u_longlong_t)rz->rz_gang_left,
	    (u_longlong_t)rz->rz_zil_old, (u_longlong_t)rz->rz_live,
	    (u_longlong_t)rz->rz_kept,
	    (u_longlong_t)vdev_raidz_redirect_count(spa, v),
	    (u_longlong_t)vdev_raidz_redirect_tombstones(spa, v),
	    (u_longlong_t)vdev_raidz_redirect_damaged(spa, v),
	    (longlong_t)vdev_raidz_redirect_extra(spa, v));
}

#define	RZM_ADD(...)	do {						\
	size_t _n = strlen(buf);					\
	(void) snprintf(buf + _n, size - _n, __VA_ARGS__);		\
} while (0)

/* child_move() of hybrid_probe.c, and do_move() of fixture_adapter.c. */
static int
rzm_move(rzm_t *rz, char *buf, size_t size)
{
	spa_t *spa = rz->rz_spa;
	dsl_pool_t *dp = spa_get_dsl(spa);
	vdev_t *vd = rz->rz_vd;
	raidz_migrate_need_t *rmn = NULL;
	rzm_mv_t **red = NULL, **rw = NULL;
	int nred = 0, nrw = 0, pf, err = 0;
	uint64_t txg0;
	hrtime_t t0;
	dsl_dataset_t *ds;

	if (rz->rz_hybrid &&
	    !spa_feature_is_enabled(spa, SPA_FEATURE_PHYSICAL_REWRITE)) {
		RZM_ADD(" why=physical_rewrite_not_enabled");
		return (SET_ERROR(ENOTSUP));
	}
	dsl_pool_config_enter(dp, FTAG);
	err = dsl_dataset_hold(dp, rz->rz_head, FTAG, &ds);
	if (err == 0) {
		rz->rz_head_obj = ds->ds_object;
		rz->rz_prev_snap = dsl_dataset_phys(ds)->ds_prev_snap_txg;
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_config_exit(dp, FTAG);
	if (err != 0) {
		RZM_ADD(" why=head_not_found");
		return (err);
	}
	/* A keyless or mounted head cannot be rewritten: redirect it all. */
	if (rz->rz_hybrid) {
		objset_t *os;

		if (dmu_objset_own(rz->rz_head, DMU_OST_ANY, B_FALSE, B_TRUE,
		    FTAG, &os) == 0) {
			rz->rz_head_rw = B_TRUE;
			dmu_objset_disown(os, B_TRUE, FTAG);
		}
	}
	RZM_ADD(" head_rw=%d", rz->rz_head_rw);
	rz->rz_mv = vmem_alloc(RZM_MAXMV * sizeof (rzm_mv_t), KM_SLEEP);
	red = vmem_alloc(RZM_MAXMV * sizeof (rzm_mv_t *), KM_SLEEP);
	rw = vmem_alloc(RZM_MAXMV * sizeof (rzm_mv_t *), KM_SLEEP);
	rmn = kmem_zalloc(sizeof (*rmn), KM_SLEEP);
	rzm_settle(spa);

	txg0 = spa_last_synced_txg(spa);
	t0 = gethrtime();
	if ((err = rzm_census(rz)) != 0) {
		RZM_ADD(" why=census_failed census_err=%d", err);
		goto out;
	}
	rmn->rmn_txg = spa_last_synced_txg(spa) + 1;
	rmn->rmn_entry_bytes = RZM_ENTRY_BYTES;
	for (int i = 0; i < rz->rz_nmv; i++) {
		rzm_mv_t *m = &rz->rz_mv[i];

		vdev_raidz_migrate_need_add(vd, rmn, &m->mv_bp, m->mv_dva,
		    !m->mv_rewrite);
		if (m->mv_rewrite)
			rw[nrw++] = m;
		else
			red[nred++] = m;
	}
	pf = vdev_raidz_migrate_preflight(vd, rmn);
	RZM_ADD(" census_dvas=%d to_redirect=%d to_rewrite=%d gang_dvas=%llu "
	    "zil_old=%llu preflight=%d pf_free=%llu pf_slop=%llu "
	    "pf_need_end=%llu pf_new=%llu pf_packed=%llu pf_left=%llu "
	    "pf_need_left=%llu pf_keep=%llu", rz->rz_nmv, nred, nrw,
	    (u_longlong_t)rz->rz_gang_dvas, (u_longlong_t)rz->rz_zil_old, pf,
	    (u_longlong_t)rmn->rmn_free, (u_longlong_t)rmn->rmn_slop,
	    (u_longlong_t)rmn->rmn_need_end,
	    (u_longlong_t)rmn->rmn_redirect_new,
	    (u_longlong_t)rmn->rmn_packed, (u_longlong_t)rmn->rmn_left,
	    (u_longlong_t)rmn->rmn_need_left,
	    (u_longlong_t)rmn->rmn_redirect_keep);
	if (pf != 0 && !rz->rz_force) {
		RZM_ADD(" why=refused_by_preflight");
		err = pf;
		goto out;
	}

	rzm_redirect(rz, red, nred);
	if (nrw > 0)
		rz->rz_rewrite_err = rzm_rewrite_head(rz);
	/* What neither reached: head metadata with nothing rewritten. */
	for (int round = 0; round < RZM_ROUNDS; round++) {
		int nl = 0;

		if (rzm_census(rz) != 0 || rz->rz_nmv == 0)
			break;
		for (int i = 0; i < rz->rz_nmv; i++)
			red[nl++] = &rz->rz_mv[i];
		rz->rz_nround[round] = nl;
		rzm_redirect(rz, red, nl);
	}
	(void) rzm_census(rz);
	txg_wait_synced(dp, 0);
	RZM_ADD(" redirected=%llu refused=%llu read_errs=%llu errs=",
	    (u_longlong_t)rz->rz_moved, (u_longlong_t)rz->rz_refused,
	    (u_longlong_t)rz->rz_read_err);
	for (int k = 0; k < RZM_NERR && rz->rz_errn[k] != 0; k++) {
		RZM_ADD("%s%d:%llu", k == 0 ? "" : ",", rz->rz_errs[k],
		    (u_longlong_t)rz->rz_errn[k]);
	}
	RZM_ADD(" rewritten=%llu rewrite_err=%d round1=%d round2=%d "
	    "round3=%d mover_ms=%llu move_txgs=%llu",
	    (u_longlong_t)rz->rz_rewritten, rz->rz_rewrite_err,
	    rz->rz_nround[0], rz->rz_nround[1], rz->rz_nround[2],
	    (u_longlong_t)((gethrtime() - t0) / 1000000),
	    (u_longlong_t)(spa_last_synced_txg(spa) - txg0));
	rzm_settle(spa);
	rzm_print_census(rz, buf + strlen(buf), size - strlen(buf));
	if (rz->rz_census_err != 0)
		err = rz->rz_census_err;
	else if (rz->rz_refused != 0 || rz->rz_nmv + rz->rz_gang_left != 0)
		err = SET_ERROR(EAGAIN);
out:
	kmem_free(rmn, sizeof (*rmn));
	vmem_free(rw, RZM_MAXMV * sizeof (rzm_mv_t *));
	vmem_free(red, RZM_MAXMV * sizeof (rzm_mv_t *));
	vmem_free(rz->rz_mv, RZM_MAXMV * sizeof (rzm_mv_t));
	rz->rz_mv = NULL;
	return (err);
}

static int
rzm_run(rzm_t *rz, char *buf, size_t size)
{
	static const char *names[] = { "epoch", "move", "census" };
	vdev_t *rvd;
	hrtime_t t0 = gethrtime();
	int err;

	(void) snprintf(buf, size, "cmd=%s pool=%s", names[rz->rz_cmd],
	    rz->rz_pool);
	if (rz->rz_cmd == RZM_MOVE) {
		RZM_ADD(" mode=%s head=%s gang=%d force=%d",
		    rz->rz_hybrid ? "hybrid" : "redirect", rz->rz_head,
		    rz->rz_gang, rz->rz_force);
	}
	if ((err = spa_open(rz->rz_pool, &rz->rz_spa, FTAG)) != 0) {
		RZM_ADD(" why=spa_open");
		goto done;
	}
	rvd = rz->rz_spa->spa_root_vdev;
	rz->rz_vd = rvd->vdev_child[0];
	if (rvd->vdev_children == 0 ||
	    rz->rz_vd->vdev_ops != &vdev_raidz_ops) {
		RZM_ADD(" why=vdev0_not_raidz");
		err = SET_ERROR(EINVAL);
		goto close;
	}
	if (rz->rz_cmd == RZM_EPOCH) {
		err = rzm_epoch(rz, buf + strlen(buf), size - strlen(buf));
		goto close;
	}
	rz->rz_epoch_txg = rzm_last_epoch(rz->rz_vd);
	RZM_ADD(" epoch_txg=%llu", (u_longlong_t)rz->rz_epoch_txg);
	if (rz->rz_epoch_txg == 0) {
		RZM_ADD(" why=no_parity_epoch");
		err = SET_ERROR(EINVAL);
		goto close;
	}
	rz->rz_gsize = spa_feature_is_active(rz->rz_spa,
	    SPA_FEATURE_DYNAMIC_GANG_HEADER) ?
	    vdev_gang_header_psize(rz->rz_vd) : SPA_OLD_GANGBLOCKSIZE;
	rz->rz_key = vmem_alloc(2 * RZM_SET_SLOTS * sizeof (uint64_t),
	    KM_SLEEP);
	if (rz->rz_cmd == RZM_MOVE) {
		err = rzm_move(rz, buf, size);
	} else {
		err = rzm_census(rz);
		rzm_print_census(rz, buf + strlen(buf), size - strlen(buf));
	}
	vmem_free(rz->rz_key, 2 * RZM_SET_SLOTS * sizeof (uint64_t));
close:
	spa_close(rz->rz_spa, FTAG);
done:
	RZM_ADD(" ms=%llu rc=%d", (u_longlong_t)((gethrtime() - t0) / 1000000),
	    err);
	return (err);
}

/*
 * One command at a time.  rzm_running is written by the trigger before the
 * thread starts and rzm_result by the thread before it clears rzm_busy, so
 * the status never reads what is being written.
 */
static volatile uint32_t rzm_busy;
static uint64_t rzm_runs;
static char rzm_running[2 * ZFS_MAX_DATASET_NAME_LEN + 64];
static char rzm_result[RZM_RESULT];

static __attribute__((noreturn)) void
rzm_thread(void *arg)
{
	rzm_t *rz = arg;
	char *buf = kmem_zalloc(RZM_RESULT, KM_SLEEP);

	(void) rzm_run(rz, buf, RZM_RESULT);
	zfs_dbgmsg("spike mover: %s", buf);
	(void) strlcpy(rzm_result, buf, sizeof (rzm_result));
	rzm_runs++;
	kmem_free(buf, RZM_RESULT);
	kmem_free(rz, sizeof (*rz));
	membar_producer();
	rzm_busy = 0;
	thread_exit();
}

/* The next word of *sp, NUL-terminated in place; NULL at the end. */
static char *
rzm_word(char **sp)
{
	char *s = *sp, *w;

	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	if (*s == '\0')
		return (NULL);
	w = s;
	while (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\n')
		s++;
	if (*s != '\0')
		*s++ = '\0';
	*sp = s;
	return (w);
}

static int
rzm_parse(rzm_t *rz, char *s)
{
	char *w = rzm_word(&s);
	char *pool;

	if (w == NULL)
		return (SET_ERROR(EINVAL));
	if (strcmp(w, "epoch") == 0)
		rz->rz_cmd = RZM_EPOCH;
	else if (strcmp(w, "move") == 0)
		rz->rz_cmd = RZM_MOVE;
	else if (strcmp(w, "census") == 0)
		rz->rz_cmd = RZM_CENSUS;
	else
		return (SET_ERROR(EINVAL));
	pool = rzm_word(&s);
	if (pool == NULL || strchr(pool, '/') != NULL ||
	    strchr(pool, '@') != NULL ||
	    strlcpy(rz->rz_pool, pool, sizeof (rz->rz_pool)) >=
	    sizeof (rz->rz_pool))
		return (SET_ERROR(EINVAL));
	(void) strlcpy(rz->rz_head, pool, sizeof (rz->rz_head));
	rz->rz_gang = B_TRUE;
	if (rz->rz_cmd != RZM_MOVE)
		return (rzm_word(&s) == NULL ? 0 : SET_ERROR(EINVAL));
	if ((w = rzm_word(&s)) == NULL)
		return (SET_ERROR(EINVAL));
	if (strcmp(w, "hybrid") == 0)
		rz->rz_hybrid = B_TRUE;
	else if (strcmp(w, "redirect") != 0)
		return (SET_ERROR(EINVAL));
	while ((w = rzm_word(&s)) != NULL) {
		if (strcmp(w, "force") == 0) {
			rz->rz_force = B_TRUE;
		} else if (strcmp(w, "nogang") == 0) {
			rz->rz_gang = B_FALSE;
		} else if (strncmp(w, "head=", 5) == 0 &&
		    strncmp(w + 5, pool, strlen(pool)) == 0 &&
		    (w[5 + strlen(pool)] == '\0' ||
		    w[5 + strlen(pool)] == '/') && strchr(w, '@') == NULL) {
			if (strlcpy(rz->rz_head, w + 5, sizeof (rz->rz_head)) >=
			    sizeof (rz->rz_head))
				return (SET_ERROR(ENAMETOOLONG));
		} else {
			return (SET_ERROR(EINVAL));
		}
	}
	return (0);
}

/* SPIKE: start a command of the test-only mover; a positive errno. */
int
vdev_raidz_spike_mover_start(const char *cmd)
{
	char *s;
	rzm_t *rz;
	size_t n;
	int err;

	if (spa_mode_global == SPA_MODE_UNINIT)
		return (SET_ERROR(EAGAIN));
	if (atomic_cas_32(&rzm_busy, 0, 1) != 0)
		return (SET_ERROR(EBUSY));
	rz = kmem_zalloc(sizeof (*rz), KM_SLEEP);
	s = kmem_strdup(cmd);
	(void) strlcpy(rzm_running, cmd, sizeof (rzm_running));
	n = strlen(rzm_running);
	while (n > 0 && (rzm_running[n - 1] == '\n' ||
	    rzm_running[n - 1] == ' '))
		rzm_running[--n] = '\0';
	err = rzm_parse(rz, s);
	kmem_strfree(s);
	if (err != 0) {
		kmem_free(rz, sizeof (*rz));
		membar_producer();
		rzm_busy = 0;
		return (err);
	}
	(void) thread_create(NULL, 0, rzm_thread, rz, 0, &p0, TS_RUN,
	    defclsyspri);
	return (0);
}

/* SPIKE: "running <command>", "idle" or the last result, and a newline. */
int
vdev_raidz_spike_mover_status(char *buf, size_t size)
{
	int n;

	if (rzm_busy != 0) {
		n = snprintf(buf, size, "running %s\n", rzm_running);
	} else {
		membar_consumer();
		n = snprintf(buf, size, "%s\n", rzm_runs == 0 ? "idle" :
		    rzm_result);
	}
	if (n < 0)
		return (0);
	return (MIN((size_t)n, size - 1));
}

#if defined(_KERNEL) && defined(__linux__)
static int
param_set_raidz_spike_mover(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	return (-vdev_raidz_spike_mover_start(val));
}

static int
param_get_raidz_spike_mover(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	return (vdev_raidz_spike_mover_status(buf, PAGE_SIZE));
}

ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs_vdev, raidz_, spike_mover,
	param_set_raidz_spike_mover, param_get_raidz_spike_mover, ZMOD_RW,
	"SPIKE, test-only: run the parity mover (see vdev_raidz_mover.c)");
#endif
