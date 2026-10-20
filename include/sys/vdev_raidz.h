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
 * Copyright (C) 2016 Gvozden Neskovic <neskovic@compeng.uni-frankfurt.de>.
 */

#ifndef _SYS_VDEV_RAIDZ_H
#define	_SYS_VDEV_RAIDZ_H

#include <sys/types.h>
#include <sys/zfs_rlock.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct zio;
struct raidz_col;
struct raidz_row;
struct raidz_map;
struct vdev_raidz;
struct uberblock;
#if !defined(_KERNEL)
struct kernel_param {};
#endif

/*
 * vdev_raidz interface
 */
struct raidz_map *vdev_raidz_map_alloc(struct zio *, uint64_t, uint64_t,
    uint64_t);
struct raidz_map *vdev_raidz_map_alloc_expanded(struct zio *,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, boolean_t);
void vdev_raidz_map_free(struct raidz_map *);
void vdev_raidz_free(struct vdev_raidz *);
void vdev_raidz_generate_parity_row(struct raidz_map *, struct raidz_row *);
void vdev_raidz_generate_parity(struct raidz_map *);
void vdev_raidz_reconstruct(struct raidz_map *, const int *, int);
void vdev_raidz_child_done(zio_t *);
void vdev_raidz_io_done(zio_t *);
void vdev_raidz_checksum_error(zio_t *, struct raidz_col *, abd_t *);
struct raidz_row *vdev_raidz_row_alloc(int, zio_t *);
void vdev_raidz_reflow_copy_scratch(spa_t *);
void raidz_dtl_reassessed(vdev_t *);
boolean_t vdev_sit_out_reads(vdev_t *, zio_flag_t);
void vdev_raidz_sit_child(vdev_t *, uint64_t);
void vdev_raidz_unsit_child(vdev_t *);
boolean_t vdev_raidz_same_logical_width(vdev_t *, uint64_t, uint64_t);

extern const zio_vsd_ops_t vdev_raidz_vsd_ops;

/*
 * vdev_raidz_math interface
 */
/* Required, but not used, by ZFS_MODULE_PARAM_CALL */
extern uint32_t zfs_vdev_raidz_impl;
void vdev_raidz_math_init(void);
void vdev_raidz_math_fini(void);
const struct raidz_impl_ops *vdev_raidz_math_get_ops(void);
int vdev_raidz_math_generate(struct raidz_map *, struct raidz_row *);
int vdev_raidz_math_reconstruct(struct raidz_map *, struct raidz_row *,
    const int *, const int *, const int);
int vdev_raidz_impl_set(const char *);
int vdev_raidz_impl_get(char *buffer, size_t size);

typedef struct vdev_raidz_expand {
	uint64_t vre_vdev_id;

	kmutex_t vre_lock;
	kcondvar_t vre_cv;

	/*
	 * How much i/o is outstanding (issued and not completed).
	 */
	uint64_t vre_outstanding_bytes;

	/*
	 * Next offset to issue i/o for.
	 */
	uint64_t vre_offset;

	/*
	 * Lowest offset of a failed expansion i/o.  The expansion will retry
	 * from here.  Once the expansion thread notices the failure and exits,
	 * vre_failed_offset is reset back to UINT64_MAX, and
	 * vre_waiting_for_resilver will be set.
	 */
	uint64_t vre_failed_offset;
	boolean_t vre_waiting_for_resilver;

	/*
	 * Offset that is completing each txg
	 */
	uint64_t vre_offset_pertxg[TXG_SIZE];

	/*
	 * Bytes copied in each txg.
	 */
	uint64_t vre_bytes_copied_pertxg[TXG_SIZE];

	/*
	 * The rangelock prevents normal read/write zio's from happening while
	 * there are expansion (reflow) i/os in progress to the same offsets.
	 */
	zfs_rangelock_t vre_rangelock;

	/*
	 * These fields are stored on-disk in the vdev_top_zap:
	 */
	dsl_scan_state_t vre_state;
	uint64_t vre_start_time;
	uint64_t vre_end_time;
	uint64_t vre_bytes_copied;
} vdev_raidz_expand_t;

typedef struct vdev_raidz {
	/*
	 * Number of child vdevs when this raidz vdev was created (i.e. before
	 * any raidz expansions).
	 */
	int vd_original_width;

	/*
	 * The current number of child vdevs, which may be more than the
	 * original width if an expansion is in progress or has completed.
	 */
	int vd_physical_width;

	int vd_nparity;

	/*
	 * Tree of reflow_node_t's.  The lock protects the avl tree only.
	 * The reflow_node_t's describe completed expansions, and are used
	 * to determine the logical width given a block's birth time.
	 */
	avl_tree_t vd_expand_txgs;
	kmutex_t vd_expand_lock;

	/*
	 * If this vdev is being expanded, spa_raidz_expand is set to this
	 */
	vdev_raidz_expand_t vn_vre;

	/*
	 * Parity-epoch table (org.openzfs:raidz_parity_epochs): triplets of
	 * {start physical-birth txg, logical width, parity}, sorted by start;
	 * the count is in triplets.  vdev_raidz_init copies it from the label
	 * or MOS config (needed to read the MOS before vdev_load),
	 * vdev_raidz_load replaces that copy with the vdev ZAP's, the only
	 * authoritative one, or drops it if the ZAP has none, and
	 * vdev_raidz_fini frees the current one.  Once the vdev is in use
	 * only vdev_raidz_parity_epochs_install() replaces it.  The pointer
	 * and the count are protected by vd_expand_lock: take it to read
	 * either.
	 */
	uint64_t *vd_parity_epochs;
	uint64_t vd_parity_epoch_count;

	/*
	 * SPIKE, not for merge: blocks moved to a new extent.  In memory, a
	 * table keyed by {old offset, physical birth} (vdev_raidz.c), which
	 * the I/O path reads without a lock; on disk, the log of its records
	 * and the ZAP of the block pointers a scan reads (0 until the first
	 * move).  The entry of a freed block stays in the table as a
	 * tombstone, which I/O still follows, until its extents leave the
	 * defer trees.
	 */
	struct raidz_redirect_table *vd_rdt;
	uint64_t vd_redirect_obj;	/* the ZAP of block pointers */
	uint64_t vd_redirect_log;	/* the log */
	/* Damaged records a read-only import left out of the table. */
	uint64_t vd_redirect_damaged;
} vdev_raidz_t;

extern int vdev_raidz_attach_check(vdev_t *);
extern boolean_t vdev_raidz_has_parity_epochs(vdev_t *);
extern void vdev_raidz_layout_for_birth(vdev_t *, uint64_t, uint64_t *,
    uint64_t *);
extern void vdev_raidz_parity_epochs_install(vdev_t *, const uint64_t *,
    uint64_t);
extern uint64_t vdev_raidz_parity_epochs_first_start(dmu_tx_t *);
extern int vdev_raidz_parity_epochs_set_check(vdev_t *, const uint64_t *,
    uint64_t, const char **);
extern int vdev_raidz_parity_epochs_set_check_tx(vdev_t *, const uint64_t *,
    uint64_t, dmu_tx_t *, const char **);
extern int vdev_raidz_parity_epochs_set(vdev_t *, const uint64_t *, uint64_t,
    dmu_tx_t *);
extern void vdev_raidz_attach_sync(void *, dmu_tx_t *);
extern void spa_start_raidz_expansion_thread(spa_t *);
extern int spa_raidz_expand_get_stats(spa_t *, pool_raidz_expand_stat_t *);
extern int vdev_raidz_load(vdev_t *);

/*
 * SPIKE, not for merge: persistent block redirects of a raidz vdev.  An
 * entry says that the block whose DVA starts at rde_offset, born in txg
 * rde_birth, is now stored at rde_new_offset with the layout of txg
 * rde_new_birth, in rde_new_asize bytes, and that [rde_offset, rde_offset +
 * rde_old_reserved) of its old extent, rde_old_asize bytes long, is still
 * allocated.
 *
 * On disk, the vdev's top-level ZAP names two MOS objects.  The log holds
 * raidz_redirect_rec_t records: an entry added, or a key removed, in the
 * order they happened; its bonus holds {records, how many of the first are
 * sorted by key, one per entry}, and the load replays it.  The ZAP of block
 * pointers, keyed by {rde_offset, rde_birth}, holds the block pointer the
 * move found live and the bookmark it was named by: what a scan needs to
 * read the block and verify it without finding its block pointer.  A table
 * of the earlier spike format, one ZAP of both, is refused.
 */
#define	VDEV_TOP_ZAP_RAIDZ_REDIRECT_LOG	"org.openzfs:spike_raidz_redirect_log"
#define	VDEV_TOP_ZAP_RAIDZ_REDIRECT_BPS	"org.openzfs:spike_raidz_redirect_bps"
#define	VDEV_TOP_ZAP_RAIDZ_REDIRECTS_OLD	\
	"org.openzfs:spike_raidz_redirects"
#define	RAIDZ_REDIRECT_BP_VALUES	(sizeof (blkptr_t) / sizeof (uint64_t))
#define	RAIDZ_REDIRECT_ZB_VALUES	4
#define	RAIDZ_REDIRECT_BPS_VALUES	(RAIDZ_REDIRECT_BP_VALUES + \
	RAIDZ_REDIRECT_ZB_VALUES)

typedef struct raidz_redirect_ent {
	uint64_t rde_offset;
	uint64_t rde_birth;
	uint64_t rde_new_offset;
	uint64_t rde_new_birth;
	uint64_t rde_new_asize;
	uint64_t rde_old_reserved;
	uint64_t rde_old_asize;
} raidz_redirect_ent_t;

typedef struct raidz_redirect_table raidz_redirect_table_t;

/*
 * A record of the log, 40 bytes: the key, its type in the birth's top byte,
 * then for an ADD the new offset and birth, and the new asize, old asize
 * and old_reserved in 512-byte units, 21 bits each.  A REMOVE has only the
 * key.
 */
typedef struct raidz_redirect_rec {
	uint64_t rr_offset;
	uint64_t rr_birth;
	uint64_t rr_new_offset;
	uint64_t rr_new_birth;
	uint64_t rr_sizes;
} raidz_redirect_rec_t;

#define	RAIDZ_REDIRECT_REC_ADD		1ULL
#define	RAIDZ_REDIRECT_REC_REMOVE	2ULL
#define	RAIDZ_REDIRECT_REC_TYPE_SHIFT	56
#define	RAIDZ_REDIRECT_REC_BIRTH_MAX	((1ULL << 56) - 1)
#define	RAIDZ_REDIRECT_REC_SIZE_BITS	21
#define	RAIDZ_REDIRECT_REC_SIZE_MAX	((1ULL << 21) - 1)

static inline void
raidz_redirect_rec_make(raidz_redirect_rec_t *r, uint64_t type,
    const raidz_redirect_ent_t *e)
{
	r->rr_offset = e->rde_offset;
	r->rr_birth = (e->rde_birth & RAIDZ_REDIRECT_REC_BIRTH_MAX) |
	    (type << RAIDZ_REDIRECT_REC_TYPE_SHIFT);
	r->rr_new_offset = r->rr_new_birth = r->rr_sizes = 0;
	if (type != RAIDZ_REDIRECT_REC_ADD)
		return;
	r->rr_new_offset = e->rde_new_offset;
	r->rr_new_birth = e->rde_new_birth;
	r->rr_sizes = (e->rde_new_asize >> 9) |
	    ((e->rde_old_asize >> 9) << RAIDZ_REDIRECT_REC_SIZE_BITS) |
	    ((e->rde_old_reserved >> 9) << (2 * RAIDZ_REDIRECT_REC_SIZE_BITS));
}

/* The record's entry in *e (only the key for a REMOVE); its type. */
static inline uint64_t
raidz_redirect_rec_read(const raidz_redirect_rec_t *r, raidz_redirect_ent_t *e)
{
	uint64_t m = RAIDZ_REDIRECT_REC_SIZE_MAX;

	e->rde_offset = r->rr_offset;
	e->rde_birth = r->rr_birth & RAIDZ_REDIRECT_REC_BIRTH_MAX;
	e->rde_new_offset = r->rr_new_offset;
	e->rde_new_birth = r->rr_new_birth;
	e->rde_new_asize = (r->rr_sizes & m) << 9;
	e->rde_old_asize = ((r->rr_sizes >> RAIDZ_REDIRECT_REC_SIZE_BITS) &
	    m) << 9;
	e->rde_old_reserved = ((r->rr_sizes >>
	    (2 * RAIDZ_REDIRECT_REC_SIZE_BITS)) & m) << 9;
	return (r->rr_birth >> RAIDZ_REDIRECT_REC_TYPE_SHIFT);
}

/* What a move keeps allocated of the old extent [O, O + asize). */
typedef enum raidz_redirect_keep {
	RAIDZ_REDIRECT_KEEP_ALL,	/* P1: all of it */
	RAIDZ_REDIRECT_KEEP_FIRST,	/* P2: its first sector */
	RAIDZ_REDIRECT_KEEP_NONE,	/* P3: nothing */
} raidz_redirect_keep_t;

struct blkptr;
struct zbookmark_phys;

/* One block to move, for vdev_raidz_redirect_move() (test-only). */
typedef struct raidz_redirect_move {
	const struct blkptr *rdm_bp;	/* never changed */
	/* Where rdm_bp lives; for a gang child, where its gang block does. */
	const struct zbookmark_phys *rdm_zb;
	uint64_t rdm_psize;		/* bytes at the DVA */
	abd_t *rdm_data;		/* those bytes, as stored */
	uint64_t rdm_place;		/* if not 0, claim N there */
	int rdm_dva;			/* the DVA of rdm_bp that moves */
	/* Set by the move, as soon as each is known. */
	uint64_t rdm_old_asize;
	raidz_redirect_ent_t rdm_ent;
	int rdm_error;
} raidz_redirect_move_t;

/* SPIKE: the move takes rdm_dva (step 3A). */
#define	RAIDZ_REDIRECT_MOVE_DVA	1

extern int vdev_raidz_redirect_move(spa_t *, uint64_t,
    raidz_redirect_move_t *, int, raidz_redirect_keep_t, boolean_t);
extern boolean_t vdev_raidz_redirect_get(vdev_t *, uint64_t, uint64_t,
    raidz_redirect_ent_t *);
extern boolean_t vdev_raidz_redirect_get_live(vdev_t *, uint64_t, uint64_t,
    raidz_redirect_ent_t *);
extern uint64_t vdev_raidz_redirect_tombstones(spa_t *, uint64_t);
extern void vdev_raidz_redirect_expire(vdev_t *, uint64_t);
extern boolean_t vdev_raidz_redirect_lookup(spa_t *, uint64_t, uint64_t,
    uint64_t);
extern uint64_t vdev_raidz_redirect_count(spa_t *, uint64_t);
extern uint64_t vdev_raidz_redirect_damaged(spa_t *, uint64_t);
extern boolean_t vdev_raidz_redirect_fits(vdev_t *,
    const raidz_redirect_ent_t *, uint64_t);
extern uint64_t vdev_raidz_redirect_hits(void);
extern boolean_t vdev_raidz_redirect_claim(vdev_t *, uint64_t, uint64_t,
    uint64_t, uint64_t, int *);
extern boolean_t vdev_raidz_redirect_remove(vdev_t *, uint64_t, uint64_t,
    raidz_redirect_ent_t *);
extern void vdev_raidz_redirect_sync(vdev_t *, dmu_tx_t *);
extern uint64_t vdev_raidz_redirect_scan_min_txg(spa_t *, uint64_t,
    uint64_t);

/*
 * SPIKE: test-only, what the scan's walk of moved blocks did since the last
 * scan started (dsl_scan_redirect_pass()), and the keys of its last reads.
 * Written by the sync thread only.
 */
#define	SPIKE_SCAN_TRACE	64
typedef struct spike_scan_pass_stat {
	uint64_t sps_walked;	/* entries looked at */
	uint64_t sps_tombs;	/* of them, tombstones */
	uint64_t sps_in_window;	/* live, pruned by the traversal, t2 inside */
	uint64_t sps_issued;	/* of them, read */
	uint64_t sps_issued_bytes;
	uint64_t sps_damaged;	/* of them, whose stored bp is unusable */
	uint64_t sps_txgs;	/* txgs the walk ran in */
	uint64_t sps_resumed;	/* walks resumed from a cursor read at load */
	uint64_t sps_done_txg;	/* txg the walk finished in */
	uint64_t sps_visited;	/* blocks the traversal visited */
	uint64_t sps_watch_hits; /* scan reads at a watched extent */
	uint64_t sps_ntrace;
	uint64_t sps_trace[SPIKE_SCAN_TRACE][4]; /* vdev, offset, birth, txg */
} spike_scan_pass_stat_t;
extern spike_scan_pass_stat_t spike_scan_pass_stats;
/* SPIKE: test-only, scan reads intersecting these extents are counted. */
extern uint64_t spike_scan_watch[2][3];	/* vdev, offset, size */
extern int spike_scan_redirect_batch;
extern int spike_scan_redirect_verify;
/* SPIKE: test-only, a sorted scan issues a moved DVA alone (dsl_scan.c). */
extern int spike_scan_redirect_split;

extern int vdev_raidz_redirect_scan_next(spa_t *, uint64_t *, uint64_t,
    uint64_t, raidz_redirect_ent_t *, struct blkptr *,
    struct zbookmark_phys *);
extern uint64_t spike_redirect_claim_delta;
extern int spike_redirect_claim_enable;
extern int spike_redirect_split_txg;
extern int spike_redirect_check_enable;
extern int spike_redirect_late_remove;
extern int spike_crash_arm_free;

/* SPIKE: test-only, what spike_resilver_redirects turns on. */
#define	SPIKE_RESILVER_WIDEN		1	/* scan window to old births */
#define	SPIKE_RESILVER_NEW_EXTENT	2	/* need_resilver at N, t2 */
#define	SPIKE_RESILVER_TABLE_PASS	4	/* walk the table by t2 */
extern int spike_resilver_redirects;
/* SPIKE: test-only, a root mirror's DTL checks at a moved copy's txg. */
extern int spike_mirror_redirect_txg;
extern uint64_t spike_mirror_redirect_repairs[2];
extern int spike_redirect_move_live;
extern int spike_redirect_live_objset;
extern int spike_redirect_zil_object_check;
extern int spike_redirect_findbp_raw;
extern int spike_redirect_keep_none;

/*
 * SPIKE: test-only, what the frees of moved blocks did in a recent txg: how
 * many were freed and how many keys left the table in each sync pass (the
 * last slot takes every later pass), and how many passes spa_sync() took.
 * Indexed by txg % SPIKE_REDIRECT_TXGS; written by the sync thread only.
 */
#define	SPIKE_REDIRECT_PASSES	8
#define	SPIKE_REDIRECT_TXGS	32
typedef struct spike_redirect_stat {
	uint64_t rst_txg;
	uint64_t rst_passes;
	uint64_t rst_freed[SPIKE_REDIRECT_PASSES];
	uint64_t rst_removed[SPIKE_REDIRECT_PASSES];
} spike_redirect_stat_t;
extern spike_redirect_stat_t spike_redirect_stats[SPIKE_REDIRECT_TXGS];

/*
 * SPIKE: test-only, for the scale measurements (scale_probe.c).
 * vdev_raidz_redirect_synth() adds <n> synthetic entries to raidz vdev
 * <vdev>'s table in one sync task, persisted (ADD) or in memory only
 * (ADD_MEM), or removes them as frees of their blocks would (REMOVE), and
 * says in done[0] how many it did and in done[1] in which txg.  ADD_HOT
 * persists them without the block pointers the scan reads, so that a
 * large table fits a small disk (no test scans such a pool).  An
 * added entry gets the task's txg as its new birth, must then pass the
 * load's checks and must have a new key.  Nothing is allocated or freed:
 * a synthetic entry owns no space, and no real block has its key.
 * unload drops a vdev's table from memory and reload reads it again (an
 * idle pool only), so that a test can measure what the table takes;
 * spike_redirect_load_skip makes every load keep nothing, the table of a
 * pool that has none.  spike_redirect_condense condenses every log at its
 * vdev's next sync (1), in the txg of the next free of a moved block (2),
 * or never (-1), instead of when it is due (0);
 * spike_redirect_condense_tombs makes a condense keep the entries of freed
 * blocks, as one that forgot the frees of its txg would.
 * spike_redirect_load_lax makes the load skip a record of unknown type and
 * take a second ADD of a key, as a load without those checks would.
 * spike_redirect_unsafe_readers makes a writer of the in-memory table move
 * slots and free arrays without waiting for its lock-free readers, as a
 * table without that protocol would.
 */
#define	SPIKE_SYNTH_ADD		0
#define	SPIKE_SYNTH_ADD_MEM	1
#define	SPIKE_SYNTH_REMOVE	2
#define	SPIKE_SYNTH_ADD_HOT	3
extern int vdev_raidz_redirect_synth(spa_t *, uint64_t,
    raidz_redirect_ent_t *, uint64_t, int, uint64_t *);
extern void vdev_raidz_redirect_unload(spa_t *, uint64_t);
extern int vdev_raidz_redirect_reload(spa_t *, uint64_t);
extern int spike_redirect_load_skip;
extern int spike_redirect_condense;
extern int spike_redirect_condense_tombs;
extern int spike_redirect_load_lax;
extern int spike_redirect_unsafe_readers;

typedef struct spike_table_stat {
	uint64_t sts_load_calls;	/* loads, tryimport's included */
	uint64_t sts_load_ns;		/* the last one's wall time */
	uint64_t sts_load_total_ns;
	uint64_t sts_load_entries;	/* entries the last one kept */
	uint64_t sts_load_records;	/* records it read */
	/* What spa_sync() of a recent txg took, by txg % SPIKE_REDIRECT_TXGS */
	uint64_t sts_sync_txg[SPIKE_REDIRECT_TXGS];
	uint64_t sts_sync_ns[SPIKE_REDIRECT_TXGS];
	uint64_t sts_load_read_ns;	/* of the last load, in dmu_read() */
	uint64_t sts_condense_calls;
	uint64_t sts_condense_txg;	/* the last one's */
	uint64_t sts_condense_records;
	uint64_t sts_condense_ns;
} spike_table_stat_t;
extern spike_table_stat_t spike_table_stats;

/* SPIKE: test-only crash points; spike_crash_check() calls _exit(99). */
#define	SPIKE_CRASH_NONE	0
#define	SPIKE_CRASH_TABLE_DIRTY	1	/* the move's table updates */
#define	SPIKE_CRASH_MS_SYNCED	2	/* after vdev_sync() in spa_sync */
#define	SPIKE_CRASH_PRE_UB	3	/* even labels, no uberblock yet */
#define	SPIKE_CRASH_POST_UB	4	/* uberblocks and labels written */
extern uint64_t spike_crash_txg;
extern int spike_crash_point;
extern void spike_crash_check(spa_t *, uint64_t, int);

/* RAIDZ scratch area pause points (for testing) */
#define	RAIDZ_EXPAND_PAUSE_NONE	0
#define	RAIDZ_EXPAND_PAUSE_PRE_SCRATCH_1 1
#define	RAIDZ_EXPAND_PAUSE_PRE_SCRATCH_2 2
#define	RAIDZ_EXPAND_PAUSE_PRE_SCRATCH_3 3
#define	RAIDZ_EXPAND_PAUSE_SCRATCH_VALID 4
#define	RAIDZ_EXPAND_PAUSE_SCRATCH_REFLOWED 5
#define	RAIDZ_EXPAND_PAUSE_SCRATCH_POST_REFLOW_1 6
#define	RAIDZ_EXPAND_PAUSE_SCRATCH_POST_REFLOW_2 7

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_VDEV_RAIDZ_H */
