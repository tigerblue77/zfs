# Is "refcount(X) == 0" a safe disable condition? (per-feature)

Read-only review of `pool-feature-disable-v2` (commit `625dc041b`) on upstream
master `a0c443a85`, worktree
`$SCRATCH/pr/wt-fdis3`.
Nothing built or run. file:line from the working tree. Inferences marked
**(inferred)**. Companion: `feature_activation_audit.md` (that audit was of the
*async-mark* v1 scheme `528146027`; this PR is a different, synchronous design).

## What v2 actually does (and why it is not the v1 scheme the audit judged)

* `spa_feature_disable()` is a `dsl_sync_task` (`zfeature.c:604-611`).
  `dsl_sync_task` runs `feature_disable_check` once in open context and **again
  in syncing context** right before `feature_disable_sync` (audit G1,
  `dsl_synctask.c:244`). So the `refcount==0` test (`zfeature.c:548-549`) is
  re-evaluated in syncing context — there is no open window with a stale "mark".
* There is **no "disabling" mark**. `spa_feature_is_enabled()` is untouched and
  reads the true state until `feature_disable_sync` removes the zap entries.
  So the v1 hazards H1/H2/H3 ("active while is_enabled==false") and the
  "dependent check must ignore the mark" problem (audit §1) **do not arise**.
* `feature_depends_on()` is now **recursive** (`zfeature.c:526-537`), and the
  dependent check reads the true `spa_feature_is_enabled` (`zfeature.c:551-555`),
  so transitive dependents are refused correctly.
* `feature_disable_sync` ordering: within a txg, `dsl_pool_sync()` (sync tasks)
  runs at `spa.c:11135`, **before** `spa_flush_metaslabs`/`vdev_sync`
  (`spa.c:11163-11168`) each pass. So nothing that activates in `metaslab_sync`
  can run before the disable check inside the same txg.
* **Universal safety net:** every feature *activation* (0→≥1) goes through
  `spa_feature_incr()`, which now, instead of VERIFY-panicking on a not-enabled
  feature, calls `feature_reenable_sync()` → `spa_feature_enable()`
  (`zfeature.c:408-432`, `453-456`), serialized by the new
  `spa_feat_enable_lock`. So any path that disabled-then-activates re-enables
  the feature (a fresh enable, new `enabled_txg`) instead of running a path that
  assumes it is enabled. This is why the 4 `ASSERT(is_enabled)` removals are the
  right shape, not a masking of a real guard.

Lock order is consistent (enable_lock → stats_lock in both
`feature_reenable_sync`/`feature_enable_sync` and `feature_disable_sync`
`zfeature.c:568-583`); the `ASSERT(!MUTEX_HELD(enable_lock))` prevents recursion.

The one state that could make a re-enable *incorrect* is a stale `enabled_txg`
being consulted. The **only** consumer of `spa_feature_enabled_txg()` is
hole_birth (`dmu_traverse.c:652-654`), and hole_birth is ACTIVATE_ON_ENABLE
(initial_refcount 1, `zfeature.c:345`), so it can never reach refcount 0 and can
never be disabled/re-enabled. So a fresh `enabled_txg` corrupts nothing. **(key
cross-feature result).**

## The five features

### 1. OBSOLETE_COUNTS — SAFE WITH REFCOUNT-0 ONLY (removed assert justified)

The refcount tracks *every* piece of obsolete-counts machinery, each site
paired incr/decr with the object it describes:
* mapping allocated *with counts* (`vdev_indirect_mapping.c:313-332` incr /
  `:369-376` decr) — held for the life of an indirect vdev;
* obsolete space map (`vdev_indirect.c:832` incr / condense `:516`, cancel
  `vdev_removal.c:1948` decr);
* `VDEV_TOP_ZAP_OBSOLETE_COUNTS_ARE_PRECISE` during active removal
  (`vdev_removal.c:461` incr / `:1931` decr);
* remap deadlists (`dsl_dataset.c:5276`/`5252`, `dsl_pool.c:459`/`465`).

So **refcount 0 ⟺ no counts-mapping, no obsolete sm, no ARE_PRECISE, no remap
deadlist**. A removal with the feature enabled holds ≥1 (ARE_PRECISE at
`vdev_removal.c:461`) for its whole duration, so disable is refused mid-removal.
The only enabled+refcount-0+indirect-vdev state is a V0 mapping (removal ran
while the feature was off) with no obsolete sm yet — genuinely no machinery, and
already reachable on master (enable after an old removal); disabling just
restores "never enabled", exactly that pre-removal state.

The caller's worry that a removal started while disabled "does not bump it"
(`vdev_removal.c:455`): that path leaves a V0 mapping and holds no ref, so there
is nothing to leak. `vdev_indirect_sync_obsolete` is reached only via
`vdev_sync` when `vdev_obsolete_segments` is non-empty (`vdev.c:4381-4385`), and
segments are added only by `vdev_indirect_mark_obsolete`, **gated on
is_enabled** (`vdev_indirect.c:330`). So while disabled, nothing is marked,
sync_obsolete is not reached, and the removed assert at (old) `:818` was
unreachable while disabled anyway. If a disable lands in the same txg as pending
marks, the unconditional incr at `:832` re-enables before creating the sm — on
master that was the panic the assert documented; here it is correct. The
"mapping-without-counts + obsolete-sm-present + ARE_PRECISE=false" shape is
already reachable and handled on master. No silent imprecision, no leaked sm.

### 2. LOG_SPACEMAP — SAFE WITH REFCOUNT-0 ONLY

Exactly one incr (`spa_log_spacemap.c:1151`, at first creation of the
`DMU_POOL_LOG_SPACEMAP_ZAP`) and **no decr anywhere in the tree** (confirmed by
grep). The zap persists for the pool's life. Therefore **refcount ≥ 1 from the
instant any log-spacemap state exists, forever**; refcount 0 ⟺ no log-sm zap ⟺
no log spacemaps ⟺ no unflushed log entries. The audit's severe H3 (writing
straight to metaslab spacemaps while unflushed log entries exist) required
disabling at refcount ≥ 1, which this scheme refuses outright.

No log_spacemap assert is among the four removed. The remaining
`ASSERT(is_enabled)` at `metaslab.c:2642`/`:4420` gate on
`spa_syncing_log_sm(spa) != NULL`, which is only non-NULL after
`spa_generate_syncing_log_sm` created a log sm (⟹ incr ⟹ refcount ≥ 1), so they
are unreachable at refcount 0. The behaviour switches at
`spa_log_spacemap.c:1136` and the `else` at `metaslab.c:4442-4448` only take the
legacy path when there is genuinely no log state — correct. Ordering
(dsl_pool_sync before metaslab_sync) means a disable check in a txg always
precedes any log-sm generation in that txg.

### 3. LIVELIST — SAFE WITH REFCOUNT-0 ONLY

Incr at clone creation exactly when the on-disk `DD_FIELD_LIVELIST` deadlist is
created (`dsl_dataset.c:1357-1364`); decr exactly when it is freed
(`dsl_dir.c:2433-2440`, async delete `spa.c:3391-3394`). So **refcount 0 ⟺ no
on-disk livelist ⟺ no dataset has `dd_livelist` open**. All livelist code,
including the three *un-removed* asserts (`dsl_dataset.c:212`, `:299`,
`dbuf.c:5321`), is gated on `dsl_deadlist_is_open(&dd->dd_livelist)`, so those
asserts are unreachable at refcount 0 and were correctly left in place.
Asynchronous livelist deletion holds the ref until the deadlist is actually
freed, so the window is covered.

### 4. BLOCK_CLONING — SAFE WITH REFCOUNT-0 ONLY (removed asserts stale)

Refcount = number of per-vdev BRT ZAP objects on disk (incr `brt.c:487`, decr
`brt.c:668`). **refcount 0 ⟺ no BRT state on disk.** A pending ZIL
`TX_CLONE_RANGE` is transient *pre-sync* state not reflected in the pool
refcount (if the clone's txg had synced, the BRT entries would be on disk and
refcount ≥ 1). So the admin-reachable scenario holds: enabled + refcount 0 +
a dataset whose ZIL carries unreplayed clone records (e.g. after a crash, before
that dataset's first mount); disable succeeds; mount replays.

`zfs_clone_range_replay`/`zvol_replay_clone_range` → `dmu_brt_clone` →
`brt_pending_add` → `brt_vdev_create` → `spa_feature_incr(BLOCK_CLONING)`
(`brt.c:487`). The replay does **not** read is_enabled and bail, so there is no
silent clone-drop; the incr re-enables the feature, and the BRT is built
correctly (legacy or endian-fixed per `brt.c:479-487`, consistent with whatever
of BLOCK_CLONING_ENDIAN is still enabled). On master this replay would hit the
removed `ASSERT(is_enabled)` (panic); here it is correct. A fresh `enabled_txg`
is harmless (BRT format does not depend on it). The two removed asserts were
stale debug invariants ("enabled because we are replaying a clone"), an
invariant the design deliberately relaxes and the incr repairs.

### 5. ALLOCATION_CLASSES — SAFE WITH REFCOUNT-0 ONLY (assert removal *required*)

One incr (`spa_misc.c:1835`), **never decremented** (confirmed). So once any
special/dedup vdev exists refcount ≥ 1 forever ⟹ disable refused whenever the
machinery exists. refcount 0 ⟺ no allocation-classes vdev. Adding one is gated:
`vdev_alloc` re-checks `is_enabled` (`vdev.c:891-899`) and the alloc-bias prop
re-checks it (`vdev.c:6632-6636`), both refuse while disabled.

The removed assert (`spa_misc.c:1832`) is not merely stale — it is *required*
for the one racy path the audit flagged (class D): `vdev_prop_set` ALLOC_BIAS
checks is_enabled in open context (`vdev.c:6633`) then runs
`dsl_sync_task(NULL, vdev_props_set_sync, ...)` with **no check function**
(`vdev.c:6683`). A disable landing between the check and that sync task leaves
`spa_activate_allocation_classes` → `spa_feature_incr` running against a
disabled feature. On master: panic at the removed assert. Here: the incr
re-enables, the special bias is applied, refcount → 1 — the correct end state.
If the two sync tasks share a txg the outcome is order-independent (either the
bias task incr's first and the disable then sees refcount 1 → EBUSY, or the
disable runs first and the bias task re-enables).

## Other features (scan for machinery outliving the refcount)

The incr-re-enable net means a feature is unsafe only if on-disk/in-flight state
exists while refcount is 0 **and** a path touches it assuming-enabled *without*
going through `spa_feature_incr* — i.e. a surviving `ASSERT(is_enabled)` or a
destructive behaviour-switch reachable at refcount 0.

Remaining `ASSERT(spa_feature_is_enabled)` sites (grep), all gated by
refcount-holding machinery, hence unreachable at refcount 0:
* `space_map.c:1054` SPACEMAP_V2 — ACTIVATE_ON_ENABLE, initial_refcount 1
  (`zfeature.c:345`), undisablable. Same for lz4, hole_birth, embedded_data,
  head_errlog, enabled_txg.
* `dsl_bookmark.c:601` BOOKMARKS — its sync task's check
  (`dsl_bookmark.c:298-299`) re-runs in syncing context and returns ENOTSUP if
  disabled, so the sync (and the assert) never runs; refcount = #bookmarks.
* `vdev_rebuild.c:1040` DEVICE_REBUILD — `vdev_rebuild()` resumes only ACTIVE
  rebuilds, which hold the ref (`:221` incr / `:322`,`:374` decr) for their
  duration.
* LIVELIST / LOG_SPACEMAP asserts — covered above.

Behaviour switches with their own incr/decr tracking the state:
* **spacemap_histogram** — incr/decr both gated on is_enabled and paired with
  the histogram-bearing space map (`space_map.c:909-910`/`:927-932`); refcount 0
  ⟺ no histogram sm, so the H1 "skipped decr" cannot be triggered via disable.
  In practice refcount is never near 0 on a live pool. SAFE.
* Per-dataset armed features (large_blocks, sha512/skein/edonr/blake3, zstd,
  large_dnode, longname, large_microzap, physical_rewrite, redacted_datasets,
  userobj_accounting, project_quota, encryption, zilsaxattr) all activate via
  `dsl_dataset_activate_feature` → `spa_feature_incr` (`dsl_dataset.c:1144`), so
  a deferred activation after a disable re-enables. A block written with e.g. a
  sha512 checksum carries the checksum in its blkptr regardless of the feature
  flag, so the momentary disable corrupts no data; the next born block re-arms
  and the incr restores the marker. SAFE under this scheme.

One residual, **UNSURE (leaning safe)** — **resilver_defer**. Its arming is
in-memory (`vdev_defer_resilver` sets `spa->spa_resilver_deferred` and the leaf
`vdev_resilver_deferred`, `vdev.c:6090-6092`) and the feature incr happens later
in `dsl_scan_sync` (`dsl_scan.c:4513-4514`), decr in
`vdev_clear_resilver_deferred` (`vdev.c:6110-6113`). So a deferred-resilver
intent can exist (in memory, and persisted to the vdev label independently of
the feature zap) while RESILVER_DEFER refcount is still 0 — unlike the
structural features, the refcount does not atomically track the armed state.
A disable in that window succeeds; `dsl_scan_sync:4514` then re-incr's
(re-enables), and `dsl_scan_done`'s is_enabled-gated start
(`dsl_scan.c:1336-1341`) works once re-enabled; a crash in the window is
recovered by import re-arming from the label (the comment at
`dsl_scan.c:1330-1334`). So the outcome is a feature that bounces off and comes
straight back, with no lost resilver — but this is the one spot not proven by a
clean "refcount ⟺ machinery" invariant.
* Cheap test for the disable to additionally refuse (belt-and-braces):
  `spa->spa_resilver_deferred` (one bool), i.e. refuse RESILVER_DEFER while a
  deferred resilver is pending.
* Experiment to settle it: on a debug build, attach a device to defer a
  resilver, and race `zpool set feature@resilver_defer=disabled` against the
  scan-done/scan-sync txg (zinject a delay); confirm no assert fires and the
  deferred resilver still runs.

## Bottom line

All five named features: **SAFE WITH REFCOUNT-0 ONLY**; the four assert removals
are justified (ALLOCATION_CLASSES' is required by the racy ALLOC_BIAS sync task).
For each, the refcount tracks the on-disk machinery (OBSOLETE_COUNTS, LIVELIST,
BLOCK_CLONING, ALLOCATION_CLASSES) or is monotonic and pinned by it
(LOG_SPACEMAP, ALLOCATION_CLASSES), so refcount 0 ⟺ no machinery; the one
genuinely refcount-0-with-pending case (BLOCK_CLONING ZIL replay, and the same
shape for all per-dataset armed features) is made correct by `spa_feature_incr`
re-enabling rather than panicking. No extra disable guard is needed for the five.
The single soft spot across the whole feature set is **resilver_defer**, whose
arming is not atomic with its refcount; it is harmless under the re-enable net
but is the one place worth the cheap `spa->spa_resilver_deferred` guard.
