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
	 * SPIKE, not for merge: blocks moved to a new extent, as
	 * raidz_redirect_node_t's keyed by {old offset, physical birth}, and
	 * the MOS ZAP that holds them (0 until the first move).  The lock
	 * protects the tree and the lists; the I/O path takes it as a reader.
	 * The entry of a freed block stays in the tree as a tombstone, which
	 * I/O still follows, until its extents leave the defer trees: on the
	 * pending list until vdev_sync() removes its key from the ZAP in the
	 * txg of the free, then on the tombstone list.
	 */
	avl_tree_t vd_redirects;
	krwlock_t vd_redirect_lock;
	uint64_t vd_redirect_obj;
	list_t vd_redirect_pending;
	list_t vd_redirect_tombs;
	uint64_t vd_redirect_ntombs;	/* pending and tombs */
	/* Damaged entries a read-only import left out of vd_redirects. */
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
 * allocated.  The table is a ZAP in the MOS, keyed by {rde_offset,
 * rde_birth}, named in the vdev's top-level ZAP, whose values are the other
 * four fields in this order.
 */
#define	VDEV_TOP_ZAP_RAIDZ_REDIRECTS	"org.openzfs:spike_raidz_redirects"
#define	RAIDZ_REDIRECT_VALUES		5

typedef struct raidz_redirect_ent {
	uint64_t rde_offset;
	uint64_t rde_birth;
	uint64_t rde_new_offset;
	uint64_t rde_new_birth;
	uint64_t rde_new_asize;
	uint64_t rde_old_reserved;
	uint64_t rde_old_asize;
} raidz_redirect_ent_t;

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
	const struct blkptr *rdm_bp;	/* never changed; DVA 0 moves */
	/* Where rdm_bp lives; for a gang child, where its gang block does. */
	const struct zbookmark_phys *rdm_zb;
	uint64_t rdm_psize;		/* bytes at the DVA */
	abd_t *rdm_data;		/* those bytes, as stored */
	uint64_t rdm_place;		/* if not 0, claim N there */
	/* Set by the move, as soon as each is known. */
	uint64_t rdm_old_asize;
	raidz_redirect_ent_t rdm_ent;
	int rdm_error;
} raidz_redirect_move_t;

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
extern uint64_t spike_redirect_claim_delta;
extern int spike_redirect_claim_enable;
extern int spike_redirect_split_txg;
extern int spike_redirect_check_enable;
extern int spike_redirect_late_remove;
extern int spike_crash_arm_free;

/* SPIKE: test-only, what spike_resilver_redirects turns on. */
#define	SPIKE_RESILVER_WIDEN		1	/* scan window to old births */
#define	SPIKE_RESILVER_NEW_EXTENT	2	/* need_resilver at N, t2 */
extern int spike_resilver_redirects;
extern int spike_redirect_move_live;
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
