// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike (throwaway, not part of any tree): hybrid_probe.c's mover, run on
 * the common parity fixtures (contrib/raidz-parity-fixtures of
 * rdmitry0911/zfs, tigerblue77/zfs#2) instead of its own setup.
 *
 *   fixture_adapter <dir> move <hybrid|redirect> [json=FILE]
 *   fixture_adapter <dir> report [json=FILE]
 *   fixture_adapter <dir> fill [json=FILE]
 *
 * <dir> holds the generator's exported pool "jointfixture" (members v0 to
 * v4, found by scanning <dir>) and its objects.txt.
 *
 * move: imports the pool, records the physical inventory of the DVAs born
 * before the epoch (count, allocated and physical bytes; head-owned data,
 * data reached through the snapshot, head-owned metadata, other metadata,
 * the MOS), times a census walk alone and a full scrub, adds hybrid_probe's
 * parity-2 epoch, runs its pre-flight check and its mover (hybrid: the head
 * rewritten, the rest redirected; redirect: everything redirected), ends
 * with its census, which must count 0, and exports.
 * report: a read-only import; the table, its kept sectors and the heap.
 * fill: hybrid_probe's fill (ENOSPC three times in a row, then the gang
 * rate of the new 128K blocks), on a writable copy of an input.
 *
 * Memory: mallinfo2() uordblks (and hblkhd), sampled every 10 ms by a
 * thread: at idle after the import, the peak during the move, at the end;
 * getrusage() maxrss.  The heap includes the census's own arrays
 * (scratch_bytes in the JSON) and every libzpool cache (the ARC included).
 *
 * The census, redirect, pre-flight, epoch, space-settle, fragmentation,
 * fill and gang code is hybrid_probe.c's own, included below.  Its pool
 * name is a compile-time literal ("hyb"), so the few functions that name
 * the pool (import, export, the head's dataset, rewrite_head and the fill
 * loop) are restated here for "jointfixture".
 *
 * Prints INFO and RESULT lines; with json=FILE, writes one JSON object
 * there.  Exit status: 0 done, 1 a check failed (a move refused, or old
 * DVAs left after the census), 2 usage, 4 the pre-flight check refused.
 */

int hybrid_probe_main(int, char **);
#define	main hybrid_probe_main
#include "hybrid_probe.c"
#undef main

#include <malloc.h>
#include <sys/resource.h>
#include <sys/dsl_scan.h>
#include <sys/arc_impl.h>

#define	FX_POOL		"jointfixture"
#define	FX_SNAP		FX_POOL "@retained"

static struct {
	uint64_t	record;
	uint64_t	retained;
	uint64_t	head;
	uint64_t	robj;
	uint64_t	hobj;
	unsigned int	dropped;
} fx;
static const char *jmode = "";

/* ---- JSON ---- */

static FILE *jf;
static int jfields;

static void
jkey(const char *k)
{
	if (jf != NULL)
		(void) fprintf(jf, "%s\"%s\": ", jfields++ ? ", " : "{", k);
}

static void
jnum(const char *k, uint64_t v)
{
	jkey(k);
	if (jf != NULL)
		(void) fprintf(jf, "%llu", (u_longlong_t)v);
	(void) printf("RESULT %s=%llu\n", k, (u_longlong_t)v);
}

static void
jint(const char *k, int64_t v)
{
	jkey(k);
	if (jf != NULL)
		(void) fprintf(jf, "%lld", (longlong_t)v);
	(void) printf("RESULT %s=%lld\n", k, (longlong_t)v);
}

static void
jdbl(const char *k, double v)
{
	jkey(k);
	if (jf != NULL)
		(void) fprintf(jf, "%.6f", v);
	(void) printf("RESULT %s=%.6f\n", k, v);
}

static void
jstr(const char *k, const char *v)
{
	jkey(k);
	if (jf != NULL)
		(void) fprintf(jf, "\"%s\"", v);
	(void) printf("RESULT %s=%s\n", k, v);
}

static void
jclose(void)
{
	if (jf == NULL)
		return;
	(void) fprintf(jf, "%s}\n", jfields ? "" : "{");
	VERIFY0(fclose(jf));
	jf = NULL;
}

/* ---- memory ---- */

enum { HS_IDLE, HS_MOVE, HS_END, HS_N };
static volatile int hs_stop;
static volatile int hs_phase;
static uint64_t hs_peak[HS_N];
static uint64_t hs_peak_total[HS_N];
static uint64_t hs_samples;
static pthread_t hs_tid;

static uint64_t
heap_uord(void)
{
	return (mallinfo2().uordblks);
}

static void *
heap_thread(void *arg)
{
	(void) arg;
	while (!hs_stop) {
		struct mallinfo2 mi = mallinfo2();
		int p = hs_phase;

		if (mi.uordblks > hs_peak[p])
			hs_peak[p] = mi.uordblks;
		if (mi.uordblks + mi.hblkhd > hs_peak_total[p])
			hs_peak_total[p] = mi.uordblks + mi.hblkhd;
		hs_samples++;
		(void) usleep(10000);
	}
	return (NULL);
}

static void
heap_start(void)
{
	VERIFY0(pthread_create(&hs_tid, NULL, heap_thread, NULL));
}

static void
heap_stop(void)
{
	hs_stop = 1;
	VERIFY0(pthread_join(hs_tid, NULL));
	jnum("heap_samples", hs_samples);
}

/* Let the sampler see the current phase at least once. */
static void
heap_phase(int p)
{
	hs_phase = p;
	(void) usleep(30000);
}

static uint64_t
maxrss_kib(void)
{
	struct rusage ru;

	VERIFY0(getrusage(RUSAGE_SELF, &ru));
	return ((uint64_t)ru.ru_maxrss);
}

/* ---- the input ---- */

static void
load_objects(void)
{
	char path[PATH_BUF];
	unsigned int version;
	u_longlong_t r, s, h, ro, ho;
	FILE *f;

	(void) snprintf(path, sizeof (path), "%s/objects.txt", dir);
	f = fopen(path, "r");
	VERIFY3P(f, !=, NULL);
	VERIFY3S(fscanf(f, "%u %llu %llu %llu %llu %llu %u", &version, &r, &s,
	    &h, &ro, &ho, &fx.dropped), ==, 7);
	VERIFY3U(version, ==, 1);
	VERIFY0(fclose(f));
	fx.record = r;
	fx.retained = s;
	fx.head = h;
	fx.robj = ro;
	fx.hobj = ho;
	jnum("record", fx.record);
	jnum("retained_bytes", fx.retained);
	jnum("head_bytes", fx.head);
	jnum("retained_object", fx.robj);
	jnum("head_object", fx.hobj);
	jnum("dropped", fx.dropped);
}

/* hybrid_probe's try_import(), by scanning dir, for FX_POOL. */
static spa_t *
fx_import(boolean_t readonly)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL, *props = NULL;
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
	VERIFY0(zpool_find_config(&lpch, FX_POOL, &config, &ia));
	if (!readonly) {
		/* As the generator: no cache file written anywhere. */
		props = fnvlist_alloc();
		fnvlist_add_string(props, "cachefile", "none");
	}
	VERIFY0(spa_import((char *)FX_POOL, config, props, readonly ?
	    ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	if (props != NULL)
		fnvlist_free(props);
	VERIFY0(spa_open(FX_POOL, &spa, FTAG));
	return (spa);
}

static void
fx_export(spa_t *spa)
{
	spa_close(spa, FTAG);
	VERIFY0(spa_export(FX_POOL, NULL, B_FALSE, B_FALSE));
}

/* The head dataset and its previous snapshot, as child_move() reads them. */
static void
fx_head(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	dsl_dataset_t *ds;

	dsl_pool_config_enter(dp, FTAG);
	VERIFY0(dsl_dataset_hold(dp, FX_POOL, FTAG, &ds));
	sh->sh_head = ds->ds_object;
	sh->sh_prev_snap = dsl_dataset_phys(ds)->ds_prev_snap_txg;
	dsl_dataset_rele(ds, FTAG);
	dsl_pool_config_exit(dp, FTAG);
}

static uint64_t
fx_head_avail(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	dsl_dataset_t *ds;
	uint64_t refd, avail, uobj, aobj;

	dsl_pool_config_enter(dp, FTAG);
	VERIFY0(dsl_dataset_hold(dp, FX_POOL, FTAG, &ds));
	dsl_dataset_space(ds, &refd, &avail, &uobj, &aobj);
	dsl_dataset_rele(ds, FTAG);
	dsl_pool_config_exit(dp, FTAG);
	return (avail);
}

/* ---- the inventory before the move ---- */

enum { IV_HEAD, IV_SNAP, IV_MHEAD, IV_MOTHER, IV_MOS, IV_N };
static const char *iv_name[IV_N] = { "head_data", "snapshot_data",
	"head_metadata", "other_metadata", "mos" };

typedef struct inv {
	uint64_t	*iv_key;
	uint64_t	iv_n[IV_N];
	uint64_t	iv_asize[IV_N];
	uint64_t	iv_psize[IV_N];
	uint64_t	iv_gang;
} inv_t;

/*
 * Every DVA born before sh_epoch_txg, once: head-owned is census_cb()'s
 * rewrite rule (the head's objset, logical birth after its previous
 * snapshot); data is a level-0 DMU_OT_PLAIN_FILE_CONTENTS block.
 */
static int
inv_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	inv_t *iv = arg;
	uint64_t birth;
	int c;

	(void) spa, (void) zilog, (void) dnp;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    zb->zb_level == ZB_ZIL_LEVEL)
		return (0);
	birth = BP_GET_PHYSICAL_BIRTH(bp);
	if (birth >= sh->sh_epoch_txg)
		return (0);
	if (zb->zb_objset == DMU_META_OBJSET) {
		c = IV_MOS;
	} else {
		boolean_t head = zb->zb_objset == sh->sh_head &&
		    BP_GET_BIRTH(bp) > sh->sh_prev_snap;
		boolean_t data = zb->zb_level == 0 &&
		    BP_GET_TYPE(bp) == DMU_OT_PLAIN_FILE_CONTENTS;

		c = data ? (head ? IV_HEAD : IV_SNAP) :
		    (head ? IV_MHEAD : IV_MOTHER);
	}
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];

		if (!set_add(iv->iv_key, DVA_GET_OFFSET(dva), birth))
			continue;
		iv->iv_n[c]++;
		iv->iv_asize[c] += DVA_GET_ASIZE(dva);
		iv->iv_psize[c] += BP_GET_PSIZE(bp);
		if (DVA_GET_GANG(dva))
			iv->iv_gang++;
	}
	return (0);
}

/* ---- the table: live entries reached from block pointers ---- */

typedef struct twalk {
	uint64_t	*tw_key;
	vdev_t		*tw_vd;
	uint64_t	tw_n;
	uint64_t	tw_kept;	/* rde_old_reserved */
	uint64_t	tw_old;		/* rde_old_asize */
	uint64_t	tw_new;		/* rde_new_asize */
} twalk_t;

static int
twalk_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	twalk_t *tw = arg;
	raidz_redirect_ent_t e;

	(void) spa, (void) zilog, (void) dnp;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    zb->zb_level == ZB_ZIL_LEVEL)
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);

		if (!set_add(tw->tw_key, DVA_GET_OFFSET(dva), birth))
			continue;
		if (!vdev_raidz_redirect_get_live(tw->tw_vd,
		    DVA_GET_OFFSET(dva), birth, &e))
			continue;
		tw->tw_n++;
		tw->tw_kept += e.rde_old_reserved;
		tw->tw_old += e.rde_old_asize;
		tw->tw_new += e.rde_new_asize;
	}
	return (0);
}

static void
table_report(spa_t *spa, uint64_t *key, const char *pfx)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	twalk_t tw = { .tw_key = key, .tw_vd = vd };
	uint64_t lb = obj_bytes(spa, vdrz->vd_redirect_log);
	uint64_t zb = obj_bytes(spa, vdrz->vd_redirect_obj);
	char k[64];
	hrtime_t t0;

	memset(key, 0, (2ULL << SET_BITS) * sizeof (uint64_t));
	t0 = gethrtime();
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD, twalk_cb,
	    &tw));
#define	J(name, v)	do { \
	(void) snprintf(k, sizeof (k), "%s_%s", pfx, name); \
	jnum(k, v); \
} while (0)
	J("table_entries", vdev_raidz_redirect_count(spa, 0));
	J("table_tombstones", vdev_raidz_redirect_tombstones(spa, 0));
	J("table_damaged", vdev_raidz_redirect_damaged(spa, 0));
	J("table_log_bytes", lb);
	J("table_bp_zap_bytes", zb);
	J("table_bytes", lb + zb);
	J("table_walk_entries", tw.tw_n);
	J("kept_bytes", tw.tw_kept);
	J("kept_sectors", tw.tw_kept >> vd->vdev_ashift);
	J("redirect_old_asize", tw.tw_old);
	J("redirect_new_asize", tw.tw_new);
#undef J
	(void) snprintf(k, sizeof (k), "%s_table_extra", pfx);
	jint(k, vdev_raidz_redirect_extra(spa, 0));
	(void) snprintf(k, sizeof (k), "%s_moved_charge", pfx);
	jint(k, vdev_raidz_redirect_dspace(spa));
	(void) snprintf(k, sizeof (k), "%s_table_walk_s", pfx);
	jdbl(k, (double)(gethrtime() - t0) / 1e9);
}

static void
root_stats(spa_t *spa, vdev_stat_t *vs)
{
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_get_stats(spa->spa_root_vdev, vs);
	spa_config_exit(spa, SCL_ALL, FTAG);
}

/* hybrid_probe's rewrite_head(), for FX_POOL. */
static uint64_t
fx_rewrite_head(spa_t *spa)
{
	objset_t *os;
	uint64_t obj = 0, n = 0;

	VERIFY0(dmu_objset_own(FX_POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
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

/*
 * dmu_buf_will_rewrite() activates feature@physical_rewrite, which the
 * generator does not enable (hybrid_probe's own pools enable every
 * feature): enable it, and what it depends on, as "zpool set
 * feature@physical_rewrite=enabled" would.  Without it the first sync of a
 * rewritten block fails a VERIFY in feature_do_action().
 */
static int
rewrite_feature_check(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	if (spa_feature_is_enabled(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE))
		return (SET_ERROR(EEXIST));
	return (0);
}

static void
rewrite_feature_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_feature_enable(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE, tx);
}

/* ---- move ---- */

static int
do_move(void)
{
	census_arg_t ca = { 0 };
	raidz_migrate_need_t rmn = { 0 };
	move_stat_t st = { 0 };
	inv_t iv = { 0 };
	mv_t **red, **rw;
	int nred = 0, nrw = 0, err, round, rc = 0;
	uint64_t txg0, scratch;
	vdev_stat_t vs0, vs1;
	pool_scan_stat_t ps;
	dsl_pool_t *dp;
	vdev_t *vd;
	spa_t *spa;
	hrtime_t t0, t1, t2;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	heap_start();
	jnum("arc_c_max", arc_stats.arcstat_c_max.value.ui64);
	t0 = gethrtime();
	spa = fx_import(B_FALSE);
	jdbl("import_s", (double)(gethrtime() - t0) / 1e9);
	dp = spa_get_dsl(spa);
	vd = spa->spa_root_vdev->vdev_child[0];
	space_settle(spa);
	heap_phase(HS_IDLE);
	jnum("heap_idle", heap_uord());
	fx_head(spa);
	jnum("head_dsobj", sh->sh_head);
	jnum("head_prev_snap_txg", sh->sh_prev_snap);

	/* (b) Before any move: the inventory, a census walk, a scrub. */
	ca.ca_key = calloc(2ULL << SET_BITS, sizeof (uint64_t));
	ca.ca_mv = malloc(MAXMV * sizeof (mv_t));
	red = malloc(MAXMV * sizeof (mv_t *));
	rw = malloc(MAXMV * sizeof (mv_t *));
	VERIFY(ca.ca_key != NULL && ca.ca_mv != NULL && red != NULL &&
	    rw != NULL);
	scratch = (2ULL << SET_BITS) * sizeof (uint64_t) +
	    MAXMV * (sizeof (mv_t) + 2 * sizeof (mv_t *));
	jnum("scratch_bytes", scratch);
	sh->sh_epoch_txg = UINT64_MAX;
	iv.iv_key = ca.ca_key;
	memset(iv.iv_key, 0, (2ULL << SET_BITS) * sizeof (uint64_t));
	t0 = gethrtime();
	VERIFY0(traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_HARD, inv_cb,
	    &iv));
	jdbl("inventory_walk_s", (double)(gethrtime() - t0) / 1e9);
	for (int c = 0; c < IV_N; c++) {
		char k[64];

		(void) snprintf(k, sizeof (k), "inv_%s_dvas", iv_name[c]);
		jnum(k, iv.iv_n[c]);
		(void) snprintf(k, sizeof (k), "inv_%s_asize", iv_name[c]);
		jnum(k, iv.iv_asize[c]);
		(void) snprintf(k, sizeof (k), "inv_%s_psize", iv_name[c]);
		jnum(k, iv.iv_psize[c]);
	}
	jnum("inv_gang_dvas", iv.iv_gang);
	opt_hybrid = B_FALSE;
	t0 = gethrtime();
	census(spa, &ca);
	jdbl("census_walk_s", (double)(gethrtime() - t0) / 1e9);
	jnum("census_walk_dvas", ca.ca_nmv);
	root_stats(spa, &vs0);
	t0 = gethrtime();
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	while (dsl_scan_scrubbing(dp))
		txg_wait_synced(dp, 0);
	jdbl("scrub_s", (double)(gethrtime() - t0) / 1e9);
	root_stats(spa, &vs1);
	VERIFY0(spa_scan_get_stats(spa, &ps));
	jnum("scrub_finished", ps.pss_state == DSS_FINISHED);
	jnum("scrub_errors", ps.pss_errors);
	jnum("scrub_examined", ps.pss_examined);
	jnum("scrub_read_bytes", vs1.vs_bytes[ZIO_TYPE_READ] -
	    vs0.vs_bytes[ZIO_TYPE_READ]);

	/* (c) The parity-2 epoch, as child_setup() adds it. */
	{
		epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };

		t0 = gethrtime();
		VERIFY0(dsl_sync_task(FX_POOL, epoch_check, epoch_sync, &ea, 0,
		    ZFS_SPACE_CHECK_NONE));
		txg_wait_synced(dp, ea.ea_table[3]);
		sh->sh_epoch_txg = ea.ea_table[3];
		jnum("epoch_txg", sh->sh_epoch_txg);
		jdbl("epoch_s", (double)(gethrtime() - t0) / 1e9);
	}
	opt_hybrid = strcmp(jmode, "hybrid") == 0;
	jnum("physical_rewrite_enabled_before", spa_feature_is_enabled(spa,
	    SPA_FEATURE_PHYSICAL_REWRITE));
	if (opt_hybrid) {
		objset_t *os;

		err = dsl_sync_task(FX_POOL, rewrite_feature_check,
		    rewrite_feature_sync, NULL, 0, ZFS_SPACE_CHECK_NONE);
		VERIFY(err == 0 || err == EEXIST);
		jnum("physical_rewrite_enabled_by_adapter", err == 0);
		if (dmu_objset_own(FX_POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
		    &os) == 0) {
			sh->sh_head_rw = 1;
			dmu_objset_disown(os, B_TRUE, FTAG);
		}
	}
	jnum("head_rewritable", sh->sh_head_rw);
	space_settle(spa);
	print_frag(spa, "before the move");

	/* The mover, as child_move() runs it. */
	root_stats(spa, &vs0);
	txg0 = spa_last_synced_txg(spa);
	heap_phase(HS_MOVE);
	t0 = gethrtime();
	census(spa, &ca);
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
	jnum("census_dvas", ca.ca_nmv);
	jnum("census_to_redirect", nred);
	jnum("census_to_rewrite", nrw);
	jnum("census_gang", ca.ca_gang);
	jint("preflight_error", err);
	jnum("preflight_redirect_keep", rmn.rmn_redirect_keep);
	jnum("preflight_need_end", rmn.rmn_need_end);
	jnum("preflight_free", rmn.rmn_free);
	jnum("preflight_left", rmn.rmn_left);
	jnum("preflight_need_left", rmn.rmn_need_left);
	if (err != 0) {
		jstr("outcome", "refused by the pre-flight check");
		heap_phase(HS_END);
		fx_export(spa);
		rc = 4;
		goto out;
	}
	t1 = gethrtime();
	redirect(spa, red, nred, &st);
	jdbl("redirect_s", (double)(gethrtime() - t1) / 1e9);
	t1 = gethrtime();
	if (nrw > 0)
		sh->sh_rewritten = fx_rewrite_head(spa);
	jdbl("rewrite_s", (double)(gethrtime() - t1) / 1e9);
	jnum("rewritten_blocks", sh->sh_rewritten);
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
		jnum(round == 0 ? "round1_dvas" : round == 1 ? "round2_dvas" :
		    "round3_dvas", nl);
		redirect(spa, red, nl, &st);
	}
	census(spa, &ca);
	txg_wait_synced(dp, 0);
	t2 = gethrtime();
	root_stats(spa, &vs1);
	heap_phase(HS_END);
	sh->sh_left = ca.ca_nmv + ca.ca_gang;
	jdbl("mover_s", (double)(t2 - t0) / 1e9);
	jnum("move_read_bytes", vs1.vs_bytes[ZIO_TYPE_READ] -
	    vs0.vs_bytes[ZIO_TYPE_READ]);
	jnum("move_write_bytes", vs1.vs_bytes[ZIO_TYPE_WRITE] -
	    vs0.vs_bytes[ZIO_TYPE_WRITE]);
	jnum("move_read_ops", vs1.vs_ops[ZIO_TYPE_READ] -
	    vs0.vs_ops[ZIO_TYPE_READ]);
	jnum("move_write_ops", vs1.vs_ops[ZIO_TYPE_WRITE] -
	    vs0.vs_ops[ZIO_TYPE_WRITE]);
	jnum("move_txgs", spa_last_synced_txg(spa) - txg0);
	jnum("redirected_dvas", st.ms_moved);
	jnum("refused_dvas", st.ms_refused);
	jnum("census_left", sh->sh_left);
	space_settle(spa);
	table_report(spa, ca.ca_key, "after");
	print_frag(spa, "after the move");
	jnum("heap_end", heap_uord());
	if (st.ms_refused != 0 || sh->sh_left != 0)
		rc = 1;
	jstr("outcome", rc == 0 ? "moved, census 0" : "census not 0 or a "
	    "move refused");
	fx_export(spa);
	jnum("heap_after_export", heap_uord());
out:
	kernel_fini();
	heap_stop();
	jnum("heap_peak_idle", hs_peak[HS_IDLE]);
	jnum("heap_peak_move", hs_peak[HS_MOVE]);
	jnum("heap_peak_end", hs_peak[HS_END]);
	jnum("heap_total_peak_move", hs_peak_total[HS_MOVE]);
	jnum("maxrss_kib", maxrss_kib());
	free(ca.ca_key);
	free(ca.ca_mv);
	free(red);
	free(rw);
	return (rc);
}

/* ---- report: a cold read-only import ---- */

static int
do_report(void)
{
	uint64_t *key;
	spa_t *spa;
	hrtime_t t0;

	kernel_init(SPA_MODE_READ);
	heap_start();
	jnum("heap_before_import", heap_uord());
	t0 = gethrtime();
	spa = fx_import(B_TRUE);
	jdbl("import_s", (double)(gethrtime() - t0) / 1e9);
	heap_phase(HS_IDLE);
	jnum("heap_after_import", heap_uord());
	key = calloc(2ULL << SET_BITS, sizeof (uint64_t));
	VERIFY3P(key, !=, NULL);
	table_report(spa, key, "cold");
	free(key);
	heap_phase(HS_END);
	spa_close(spa, FTAG);
	kernel_fini();
	heap_stop();
	jnum("heap_peak_import", hs_peak[HS_IDLE]);
	jnum("maxrss_kib", maxrss_kib());
	return (0);
}

/* ---- fill: hybrid_probe's child_fill(), for FX_POOL ---- */

static int
do_fill(void)
{
	void *buf = malloc(BIGTX);
	uint64_t obj = 0, off = FILLOBJ, seed = 1000;
	boolean_t big = B_TRUE;
	int in_a_row = 0;
	pthread_t wd;
	objset_t *os;
	spa_t *spa;
	hrtime_t t0;
	double r;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	spa = fx_import(B_FALSE);
	space_settle(spa);
	print_frag(spa, "before the fill");
	fill_txg0 = spa_last_synced_txg(spa) + 1;
	sh->sh_avail = fx_head_avail(spa);
	sh->sh_used0 = sh->sh_used = root_used(spa);
	fx_head(spa);
	jnum("fill_table_entries", vdev_raidz_redirect_count(spa, 0));
	wd_spa = spa;
	VERIFY0(pthread_create(&wd, NULL, watchdog, NULL));
	t0 = gethrtime();
	VERIFY0(dmu_objset_own(FX_POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
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
	jdbl("fill_s", (double)(gethrtime() - t0) / 1e9);
	print_frag(spa, "after the fill");
	report_fill("ENOSPC");
	r = (double)(sh->sh_used - sh->sh_used0) / (double)sh->sh_avail;
	jnum("fill_available", sh->sh_avail);
	jnum("fill_charged", sh->sh_used - sh->sh_used0);
	jdbl("fill_charged_ratio", r);
	jnum("fill_ratio_ok", r > 0.97 && r < 1.03);
	jnum("fill_written", sh->sh_written);
	jnum("fill_enospc", sh->sh_enospc);
	jnum("fill_l0_blocks", sh->sh_fill_l0);
	jnum("fill_gang_blocks", sh->sh_fill_gang);
	jdbl("fill_gang_pct", sh->sh_fill_l0 ?
	    100.0 * sh->sh_fill_gang / sh->sh_fill_l0 : 0.0);
	fx_export(spa);
	kernel_fini();
	free(buf);
	return (0);
}

int
main(int argc, char **argv)
{
	char cache[PATH_BUF];
	const char *json = NULL;
	int argi, rc;

	if (argc < 3 || argv[1][0] != '/')
		goto usage;
	dir = argv[1];
	if (strcmp(argv[2], "move") == 0) {
		if (argc < 4 || (strcmp(argv[3], "hybrid") != 0 &&
		    strcmp(argv[3], "redirect") != 0))
			goto usage;
		jmode = argv[3];
		argi = 4;
	} else if (strcmp(argv[2], "report") == 0 ||
	    strcmp(argv[2], "fill") == 0) {
		argi = 3;
	} else {
		goto usage;
	}
	for (; argi < argc; argi++) {
		if (strncmp(argv[argi], "json=", 5) != 0)
			goto usage;
		json = argv[argi] + 5;
	}
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	sh = calloc(1, sizeof (*sh));
	VERIFY3P(sh, !=, NULL);
	ashift = 12;
	if (json != NULL) {
		jf = fopen(json, "w");
		VERIFY3P(jf, !=, NULL);
	}
	jstr("adapter_mode", argv[2]);
	jstr("mover", jmode);
	jstr("dir", dir);
	load_objects();
	/* As the generator: the cache file, if any, lives in the input. */
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	if (strcmp(argv[2], "move") == 0)
		rc = do_move();
	else if (strcmp(argv[2], "report") == 0)
		rc = do_report();
	else
		rc = do_fill();
	jint("exit", rc);
	jclose();
	return (rc);
usage:
	(void) fprintf(stderr, "usage: fixture_adapter <dir> move "
	    "<hybrid|redirect> [json=FILE]\n"
	    "       fixture_adapter <dir> report [json=FILE]\n"
	    "       fixture_adapter <dir> fill [json=FILE]\n");
	return (2);
}
