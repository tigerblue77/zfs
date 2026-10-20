# Feature activation audit: is an asynchronous "disabling" mark safe?

Read-only audit of OpenZFS at `$SCRATCH/pr/wt-fdis2`
(upstream master `1e9b428dc` plus local commit `528146027`, which this audit ignores).
Nothing was built or run. Every claim cites file:line. Anything I worked out
rather than read directly is marked **(inferred)**.

Line numbers:

* Paths are relative to the tree. Lines are from the working tree.
* The local commit changes three cited files: `module/zfs/spa.c`,
  `module/zfs/zfeature.c` and `module/zcommon/zfeature_common.c`.
  * In `spa.c`, upstream lines are about 22 lower before line ~10800 and
    about 26–27 lower after it.
  * For `zfeature.c`, upstream lines are given as "UP:n". They come from
    `git show 1e9b428dc:module/zfs/zfeature.c`, copied to
    `.../scratchpad/pr/upstream/`.

## The scheme being evaluated

1. Mark X "disabling". `spa_feature_is_enabled(X)` then returns false, but
   incr and decr still work.
2. Wait with `txg_wait_synced(N)`, where N is the txg that was open when the
   mark was set.
3. Run a sync task. If refcount(X) is 0, remove X. Otherwise unmark X and fail
   with EBUSY.

The scheme is safe for a feature only if, after step 2, no path can still
activate X without re-checking `spa_feature_is_enabled(X)` and seeing "false".

## 1. Mechanism: refcount cache, `spa_feature_is_enabled()`, where a mark can go

### How the cache works

* **The cache.** It is `spa->spa_feat_refcount_cache[SPA_FEATURES]`
  (`include/sys/spa_impl.h:460`). `SPA_FEATURE_DISABLED` (the value
  UINT64_MAX) means "not enabled". Any other value is the refcount.
* **Writers of the cache:**
  * `spa_add()` sets every entry to DISABLED (`module/zfs/spa_misc.c:936-942`).
  * `spa_ld_check_features()` fills it from disk at import, via
    `feature_get_refcount_from_disk()` (`module/zfs/spa.c:5409-5418`;
    UP:5387-5396).
  * `feature_sync()` updates it with
    `VERIFY3U(old, ==, atomic_swap_64(cache, refcount))`
    (`module/zfs/zfeature.c:305-333`; swap at UP:312-317). Callers:
    * `feature_enable_sync()` (UP:354-356)
    * `feature_do_action()` (UP:429)
    * zhack (`cmd/zhack.c:439,454`)
* **Readers of the cache.** Everything goes through
  `feature_get_refcount()` (UP:224-234), which returns ENOTSUP when the entry
  is DISABLED. Its callers:
  * `spa_feature_is_enabled()` (UP:486-499): `err == 0`
  * `spa_feature_is_active()` (UP:501-514): `err == 0 && refcount > 0`
  * `feature_do_action()`, the path behind `spa_feature_incr()` and
    `spa_feature_decr()`:
    `VERIFY3U(feature_get_refcount(...), !=, ENOTSUP)` at UP:413. **This is
    the panic.**
  * `spa_feature_stats_from_cache()` (`module/zfs/spa.c:6990-7000`), which
    feeds `zpool get feature@` and `zpool status`.
  * zdb, in userspace (`cmd/zdb/zdb.c:1670, 8533, 8536, 9471`).
  * Indirectly, `feature_get_enabled_txg()` (UP:270-288). It calls
    `spa_feature_is_enabled()` and then does
    `VERIFY0(zap_lookup(enabled_txg_obj))`. It is used only by
    `spa_feature_enabled_txg()`, whose only consumer is
    `dmu_traverse.c:652-654` (hole_birth, which is class A).

### Where the mark must and must not go

* **The mark must not go into `feature_get_refcount()` or into the cache
  value.** If it did, `feature_do_action()` would VERIFY-panic on the
  incr/decr that the scheme expects to keep working. `feature_sync()` would
  also VERIFY-fail on the swap.
* **The mark belongs in `spa_feature_is_enabled()` only,** for example a
  separate per-feature flag tested after `feature_get_refcount()`.
  * `spa_feature_is_active()` can stay as it is, since the refcount is 0.
  * `spa_feature_enabled_txg()` inherits the mark through
    `feature_get_enabled_txg()`. That matters only for hole_birth, which
    is class A.

### Memory ordering

* The readers in `feature_get_refcount()` take no lock (UP:228).
* Syncing-context checkers are ordered by the txg handoff (mutex/cv)
  **(inferred)**.
* For checks in open context made under an already-assigned tx, the setter
  should take N *after* the mark is globally visible. For example: set the
  mark under `spa_feat_stats_lock` (or with an atomic plus a membar), then
  read or assign the open txg.
* The step-3 sync task must be dispatched after step 2 returns, so that it
  lands in a txg > N. Inside txg N, `dsl_dataset_sync_done()` activations
  (`dsl_dataset.c:2391,2416-2425`) can run after the sync tasks of that same
  txg **(inferred from the `dsl_pool_sync` ordering; not verified line by
  line)**.

### Hazard: `spa_feature_is_enabled(X)` is also used as a behaviour switch

The refcount is 0 at step 1. But activations decided before the mark land in
txgs ≤ N, *during* the window. After that, X is active while
`is_enabled(X) == false`. Code that switches behaviour on `is_enabled` then
misbehaves. Every case below is **inferred** from reading the code; none was
reproduced.

* **H1. A decrement gated by `is_enabled`.**
  * `space_map_free_obj()` decrements spacemap_histogram only if it is
    enabled (`space_map.c:924-937`). During the window this leaks the
    refcount.
  * `dsl_scan_done()` clears deferred resilvers only if resilver_defer is
    enabled (`dsl_scan.c:1336-1342`). During the window the refcount stays
    up and the deferred resilver is never started.
* **H2. `ASSERT(spa_feature_is_enabled(X))` on paths that only run once X is
  active.** These are debug-build panics.
  * LIVELIST: `dsl_dataset.c:212`, `dsl_dataset.c:299`, `dbuf.c:5321`
  * OBSOLETE_COUNTS: `vdev_indirect.c:818`
  * ALLOCATION_CLASSES: `spa_misc.c:1833`
  * DEVICE_REBUILD: `vdev_rebuild.c:1029`
  * LOG_SPACEMAP: `metaslab.c:2642`, `metaslab.c:4420`
  * BLOCK_CLONING: `zfs_vnops.c:2924`, `zvol.c:573`
  * BOOKMARKS: `dsl_bookmark.c:601`
* **H3. On-disk behaviour of an already-active feature changes.**
  * LOG_SPACEMAP: `spa_generate_syncing_log_sm()` returns without a log
    spacemap when the feature reads as not enabled
    (`spa_log_spacemap.c:1136`). `metaslab_sync()` then writes straight to
    the metaslab spacemaps while unflushed log entries from earlier txgs
    still exist (`metaslab.c:4418-4448`). **This is a potential spacemap
    inconsistency (inferred, severe).**
  * OBSOLETE_COUNTS: `vdev_indirect_mark_obsolete()` stops recording obsolete
    segments (`vdev_indirect.c:331`), and the removal thread stops filling
    `vime_obsolete_count` (`vdev_removal.c:1244`). This happens while
    `VDEV_TOP_ZAP_OBSOLETE_COUNTS_ARE_PRECISE` is set, so the counts become
    imprecise.
  * USEROBJ_ACCOUNTING and PROJECT_QUOTA: per-object accounting stops
    (`dmu_objset.c:1798-1811`, used at 1876, 1929, 1954, 2111).

**A suggested mitigation (design idea, not verified).** If `feature_do_action()`
increments a feature marked "disabling", set a "spoiled" bit on the mark.
From then on `spa_feature_is_enabled()` returns true again, and step 3 fails
with EBUSY regardless of the refcount. This removes H1-H3 and keeps every
refusal decision made before the spoil.

### `feature_enable_sync()` and `spa_feature_enable()` of Y, which depends on a "disabling" X

* `feature_enable_sync(Y)` calls `spa_feature_enable(X)` for each dependency
  (UP:347-348). That reaches `feature_enable_sync(X)`, which returns at once
  because `zap_contains()` finds X **on disk** (UP:344-345).
* Nothing misbehaves on disk. The mark on X survives, and Y becomes enabled
  with a dependency on X.
* Y's paths often activate X without checking X. These incrementing calls
  work, because X is still enabled on disk:
  * `dmu_object_zapify()` increments EXTENSIBLE_DATASET
    (`dmu_object.c:441-478`).
  * `dsl_bookmark_create_redacted_check()` checks REDACTION_BOOKMARKS only
    (`dsl_bookmark.c:650-652`). `dsl_bookmark_node_add()` then increments
    BOOKMARKS (`dsl_bookmark.c:395-400`).
* Step 3 must re-check for enabled dependents *in the sync task*. The local
  commit does this at `zfeature.c:527`.
* **That dependency check must not use the marked `spa_feature_is_enabled()`.**
  The local `spa_feature_disable_check()` does use it
  (`zfeature.c:504-507`). With an async mark, a dependent Y that is itself
  "disabling" would read as not enabled, so X could be removed while Y is
  still on disk.
* `zfeature_depends_on()` is not recursive (`zfeature_common.c`). It is still
  sufficient, by induction, while every enabled feature has all its direct
  dependencies enabled.
* An explicit `zpool set feature@X=enabled` during the window is a no-op on
  disk (`spa.c:10857-10866` → `feature_enable_sync`), and the mark persists.
  The enable reports success, yet X still reads disabled. The set should
  clear the mark, or fail with EBUSY.

## 2. Activation mechanisms that recur below

* **G1. Sync task.** `dsl_sync_task_common()` assigns the tx first
  (`dsl_synctask.c:51`) and then runs the check in open context (`:64`).
  `dsl_sync_task_sync()` runs the check *again* in syncing context (`:244`).
  An activation in the sync function is therefore class B by construction,
  **provided the check function holds the feature check**.
  `dsl_sync_task_nowait()` has no check (`:178`).
* **G2. `spa_vdev_enter()`.** It takes `spa_namespace_lock` and
  `SCL_ALL` as writer (`spa_misc.c:1352`, `1396-1402`), and its txg is
  `spa_last_synced_txg()+1`. `spa_sync()` holds `SCL_CONFIG` as reader for
  the whole sync (`spa.c:11350` … `~11510`). So a check made under
  `spa_vdev_enter()`, with a `dsl_sync_task_nowait()` or config-dirty in that
  txg, activates in a txg ≤ N whenever the check came before the mark. That
  is B **(inferred from the lock span)**.
* **G3. Per-dataset deferral.** `ds->ds_feature_activation[f]` is set in
  open or syncing context. `dsl_dataset_sync_done()` turns it into
  `dsl_dataset_activate_feature()` → `spa_feature_incr()` with **no
  enabled re-check** (`dsl_dataset.c:2416-2425`, `1135-1144`). It runs
  whenever that dataset is next synced, so arming it is equivalent to
  activating it.
* **G4. Block birth.** `dsl_dataset_block_born()` (syncing context) arms
  three things from the block pointer alone, with no enabled check
  (`dsl_dataset.c:184-202`):
  * LARGE_BLOCKS, when LSIZE > 128K
  * the checksum features (`zio_checksum_to_feature`: SHA512, SKEIN, EDONR,
    BLAKE3)
  * ZSTD (`zio_compress_to_feature`)
* **G5. Propagation of features already active.**
  * Snapshot creation (`dsl_dataset.c:1798-1803`) and clone creation
    (`1247-1251`) copy them.
  * Clone swap (`4374-4385`) moves them.
  * A pool-level refcount of 0 means no dataset has the feature active
    **(inferred from the refcount invariant)**, so G5 cannot activate a
    feature whose refcount is 0.

## 3. Per-feature classification (48 features)

Notation: "chk" is where enabled is checked; "act" is where the refcount is
incremented.

### Class A: activate-on-enable, or never activated

| Feature | Evidence |
|---|---|
| lz4_compress | ACTIVATE_ON_ENABLE (`zfeature_common.c` registration). `spa_sync_upgrades()` also increments it every txg if it is enabled but not active (`spa.c:11028-11036`, reached on every pass-1 sync, `spa.c:10973`). No decrement anywhere. |
| multi_vdev_crash_dump | No `SPA_FEATURE_MULTI_VDEV_CRASH_DUMP` reference outside `zfeature_common.[ch]`. Never activated. |
| enabled_txg | Self-activating. Enabling it writes refcount 0, after which `feature_enable_sync()` sees ENABLED_TXG enabled and increments it for its own entry (UP:358-372). Upstream never decrements it (the only decrement is the local commit's, `zfeature.c:535`). The refcount is therefore ≥1 while it is enabled **(inferred, absent zhack)**. |
| hole_birth | ACTIVATE_ON_ENABLE. No decrement. |
| embedded_data | ACTIVATE_ON_ENABLE. No decrement. |
| spacemap_v2 | ACTIVATE_ON_ENABLE. No incr/decr anywhere. |
| head_errlog | ACTIVATE_ON_ENABLE. No decrement (`zfeature.c` UP:391 only upgrades the errlog). |

### Class B: decided in syncing context, or under an assigned tx or `spa_vdev_enter()`

| Feature | chk | act | Notes |
|---|---|---|---|
| async_destroy | `dsl_destroy.c:1141-1143` (syncing, `dsl_destroy_head_sync_impl`) | `dsl_destroy.c:1030-1034` (`dsl_async_dataset_destroy`, same call) | The open-context check at `dsl_destroy.c:1278` only chooses the pre-free strategy. |
| empty_bpobj | `bpobj.c:34` | `bpobj.c:44` | Same function, called only on MOS deadlists in syncing context (`dsl_deadlist.c:530,633,724`). |
| spacemap_histogram | `space_map.c:909` | `space_map.c:910` | `space_map_alloc()` is MOS-only and syncing. Hazard H1 at `space_map.c:927`. |
| extensible_dataset | Own checks are in syncing context: `dsl_dir.c:2328` (snap cmtime) and `dmu_recv.c:687` (resumable recv, a G1 check). | `dmu_object.c:476` (`dmu_object_zapify`; asserts syncing at 452). | Every other zapify caller relies on a *dependent* feature being enabled or active: encryption (`dsl_crypt.c:2042,2075,2417`, `dmu_recv.c:5321,5338`), per-dataset features (`dsl_dataset.c:1145`), livelist (1360), fs limits (1375, `dsl_dir.c:575`), obsolete counts (`dsl_dataset.c:1863,5227`), bookmarks (`dsl_bookmark.c:401`), and ivset under encryption (`dsl_dataset.c:1884-1887`). Safe only while the disable refuses on enabled dependents. |
| bookmarks | `dsl_bookmark.c:298` (G1 check) | `dsl_bookmark.c:399` | Promote moves bookmarks through `dsl_bookmark_node_add` (`dsl_dataset.c:3916`), but only when bookmarks already exist, so the feature is already active. `dsl_bookmark_create_redacted_check()` does not check BOOKMARKS; REDACTION_BOOKMARKS depends on it. |
| filesystem_limits | `dsl_dir.c:657` (G1 check) | `dsl_dir.c:695` | Never decremented. |
| device_removal | `vdev_removal.c:2238`, re-checked at 2403 under `spa_vdev_config_enter` | `vdev_removal.c:454` (sync task added at 2418-2420) | G2. |
| obsolete_counts | Syncing-context checks: `vdev_removal.c:455`, `vdev_indirect_mapping.c:313,322`, `vdev_indirect.c:331` (mark obsolete, then `vdev_dirty` for the syncing txg), `metaslab.c:5727` (`spa_remap_blkptr`, from `dbuf_remap`, which asserts syncing at `dbuf.c:5351`) | `vdev_removal.c:461`, `vdev_indirect_mapping.c:332`, `vdev_indirect.c:833`, `dsl_dataset.c:5276`, `dsl_pool.c:459` | The last two are unguarded but reachable only once a remap deadlist exists, i.e. the feature is already active. Hazards H2 and H3. |
| zpool_checkpoint | `spa_checkpoint.c:450` (G1 check, early sync task at 559) | `spa_checkpoint.c:520` | |
| resilver_defer | `spa.c:8845-8848` (attach, G2); `dsl_scan.c:5491` (`dsl_scan_assess_vdev`, from `vdev_open` at `vdev.c:2454`) | `dsl_scan.c:4509-4514`, every sync (`spa.c:11158`) | The arming is the in-memory `spa->spa_resilver_deferred` (`vdev.c:6087-6092`), consumed with no re-check. A flag set before T is consumed by txg N's sync, which starts after T. A **theoretical** preemption window exists between check and store in the `vdev_open` path, which holds `SCL_STATE_ALL`, not `SCL_CONFIG` **(inferred)**. Import also arms it from the label flag (`vdev.c:1094-1095`), which by design implies the feature is active (`vdev_label.c:645`). Detectable cheaply through `spa->spa_resilver_deferred`. Hazard H1 at `dsl_scan.c:1336`. |
| bookmark_v2 | `dsl_bookmark.c:416` (syncing) | `dsl_bookmark.c:420` | Same function. Also reached by promote (`dsl_dataset.c:3916`). |
| redacted_datasets | `dmu_recv.c:644-645` (G1 check, `dmu_recv_begin_check` at 684) | `dmu_recv.c:1033` → `dsl_dataset.c:5293` (`dmu_recv_begin_sync`) | Otherwise only G5 propagation. Per-dataset but B. |
| bookmark_written | `dsl_bookmark.c:367` (`dsl_bookmark_set_phys`, syncing) | `dsl_bookmark.c:510-512` (same sync task) | A book→book copy (`dsl_bookmark.c:569-571`) needs a source that already has `HAS_FBN`, so the feature is already active **(inferred)**. |
| log_spacemap | `spa_log_spacemap.c:1136` | `spa_log_spacemap.c:1151` | Called from `metaslab_sync` (syncing, `metaslab.c:4351`). **Hazards H2 and H3 are severe.** |
| livelist | `dsl_dataset.c:1357-1358` (syncing, clone creation) | `dsl_dataset.c:1364` | Hazard H2. |
| device_rebuild | `spa.c:8529` (attach, G2) | `vdev_rebuild.c:221` (sync task via `vdev_rebuild_initiate`, `vdev_rebuild.c:283`; called at `spa.c:8841`) | Restart (`vdev_rebuild.c:1051-1090`) only resumes active rebuilds. |
| draid | `vdev.c:904-907` (`vdev_alloc`, under `spa_vdev_add`, G2) | `spa.c:8211-8218` (sync task) | Never decremented anywhere (no `spa_feature_decr(...DRAID...)`). |
| draid_failure_domains | `spa.c:8285-8287` (G2) | `spa.c:8224-8231` | Never decremented. |
| raidz_expansion | `spa.c:8554` (attach, G2) | `vdev_raidz.c:5381` (sync task, `spa.c:8795`) | Never decremented. |
| vdev_zaps_v2 | `vdev.c:3744` (syncing, `spa_sync_config_object` → `vdev_construct_zaps`, `spa.c:10728`) | `vdev.c:3746` | Never decremented. |
| fast_dedup | `ddt.c:1552-1553`, `ddt_configure(new=TRUE)` on the first `ddt_lookup` of an unconfigured DDT (`ddt.c:1222-1229`). Callers run in syncing context: `zio_ddt_write` (`zio.c:4225`; `dmu_sync` disables dedup, `dmu.c:2618-2620`), `zio_ddt_free` (`zio.c:4586`), `ddt_addref` (`ddt.c:2677`), prune (`ddt.c:2772`) **(inferred: all syncing)**. | `ddt.c:1500` (`ddt_create_dir`, from `ddt_sync_table`, `ddt.c:2533-2534`; called for every DDT in every txg, `ddt.c:2565-2569`) | **Not armed by `dedup=on`.** The version choice is made at first lookup, and the dir is created in the same txg. With the mark, a new DDT is created LEGACY, which is valid. |
| large_microzap | `zap_micro.c:65`, via `zap_get_micro_max_size()` at `zap_impl.c:334`, under an assigned tx (zap growth) | `zap_impl.c:347-367` (arms `ds_feature_activation` and calls `dsl_dataset_dirty` in the same tx), then G3 in that txg | Also on receive (`dmu_recv.c:636-637` → 1066-1083, G1). Per-dataset but B. |
| dynamic_gang_header | `zio.c:3678-3679` | `zio.c:3680-3685` (`dsl_sync_task_nowait` in `MAX(txg, syncing+1)`) → `zio.c:3492` | The target txg is ≤ the open txg at check time **(inferred)**. No sleep between check and enqueue. Future gang headers grow only once the feature is active (`zio.c:3563`). |
| block_cloning_endian | `brt.c:479` (syncing, `brt_vdev_create`) | `brt.c:481/484` | Same call. |

### Class C: armed (checked at one time, activated in a later, arbitrary txg)

"C-persistent" means the arming survives in properties or on-disk state.
"C-inflight" means it lives only in a running operation, so it cannot be
detected without new bookkeeping.

| Feature | Check (arming time) | Activation | What arms it | Cheap to detect? |
|---|---|---|---|---|
| large_blocks (PER_DATASET) | `zfs_ioctl.c:5492-5513` (`recordsize`/`volblocksize` > 128K, open context, before the property sync task); `zvol.c:444` (zvol create); `dmu_recv.c:630-631` (stream) | G4 `dsl_dataset.c:184-188`, then G3 | C-persistent: `recordsize`/`volblocksize` on any dataset, and existing dnodes whose block size is > 128K but whose large blocks are not yet born **(inferred)**. C-inflight: receives. | No: needs a walk of every dataset's properties, and misses in-flight receives. |
| large_dnode (PER_DATASET) | `zfs_ioctl.c:5517-5532` (`dnodesize`); `dmu_recv.c:633-634` | `dnode_sync.c:825-830` (any dnode with more than one slot), then G3 | C-persistent: the `dnodesize` property. C-inflight: receives and ZIL replay. | No |
| sha512, skein, edonr, blake3 (PER_DATASET) | `zfs_ioctl.c:5549-5578` (`checksum` and `dedup` properties) | G4 `dsl_dataset.c:190-195`, then G3 | C-persistent: the `checksum`/`dedup` property on any dataset (local, inherited or received). Metadata can also inherit the data checksum (`dmu.c:2526-2530`). **Also the per-object `dn_checksum` set from a received DRR_OBJECT with no feature check** (`dmu_recv.c:3519-3520`, only a range check at 1990; used at `dmu.c:2613-2614`). | No (property walk); per-object values would need a dnode walk. |
| zstd_compress (PER_DATASET) | `zfs_ioctl.c:5470-5481`; `dsl_dataset.c:4863` (G1, activates *that* dataset at 4881-4885); `dmu_recv.c:624-625`; `recv_check_compress_feature` (`dmu_recv.c:1828`) | G4 `dsl_dataset.c:197-202`, then G3 | C-persistent: the `compression` property inherited by children, which are not activated at set time. **Also the per-object `dn_compress` from a received DRR_OBJECT, not feature-checked** (`dmu_recv.c:3521-3522`, range check only at 1999; used at `dmu.c:2599`). C-inflight: receives. | No |
| longname (PER_DATASET) | `zfs_ioctl.c:3070-3100` (`longname` property; **skipped for `ZPROP_SRC_RECEIVED`**, 3077-3084) | `zfs_dir.c:846-852` (Linux) and `freebsd/zfs/zfs_dir.c:622-628`: `ds_feature_activation` in the create tx, then G3 | C-persistent: the `longname` property (`zfsvfs->z_longname`). | No |
| userobj_accounting (PER_DATASET) | `dmu_objset.c:1798-1802`, re-read at 2410 in the upgrade callback (open context) | Creation: `dmu_objset.c:1133-1149` plus `dsl_dataset_dirty` at 1155 (this part is B). Upgrade: `dmu_objset.c:2410-2412`, set *before* `dmu_objset_space_upgrade()` (2319-2357), whose first `dmu_tx_assign` (`DMU_TX_WAIT`) can come arbitrarily later. On EINTR (2340-2345) `ds_feature_activation` stays set in memory and fires at the dataset's next sync. | Owning any ZPL dataset without `OBJSET_FLAG_USEROBJACCOUNTING_COMPLETE` starts the upgrade task (`dmu_objset.c:838-844`). In-memory `ds_feature_activation` on cached `dsl_dataset_t`s. | No |
| project_quota (PER_DATASET) | `dmu_objset.c:1805-1810` | Same as userobj_accounting: `dmu_objset.c:1143-1148`, 2413-2415 | Same | No |
| zilsaxattr (PER_DATASET) | `zfs_sa.c:251-253` (decides to log TX_SETSAXATTR, open context, plus tunable `zfs_zil_saxattr`) | `zil.c:1063-1069` at ZIL creation (under an assigned tx, which is B); `zil.c:966-976` (**check made before `dmu_tx_assign(DMU_TX_WAIT|DMU_TX_SUSPEND)`: unbounded gap**) | C-inflight: queued itxs, written when the ZIL is next created (next fsync). The refcount drops back to 0 whenever a ZIL chain is destroyed (`zil.c:4245-4254`), so 0 is the normal idle state. | No |
| encryption (PER_DATASET) | `dsl_crypt.c:1963-1967` (create, G1); `dmu_recv.c:692-694` (raw recv begin, G1) | Create: `dsl_crypt.c:2092` (B). Raw receive of a new dataset: `dsl_crypt.c:2429-2431`, in a *separate later* sync task (`dsl_crypto_recv_raw`, `dsl_crypt.c:2520`, called from `dmu_recv_stream` at `dmu_recv.c:4919`) whose check `dsl_crypto_recv_key_check` (`dsl_crypt.c:2459-2497`) does **not** re-check the feature | C-inflight: a raw receive between begin and key load (received-property sync tasks run in between). | No |
| block_cloning | `zfs_vnops.c:1643-1646` (`zfs_clone_range_precheck`, called at 2070, **before** the rangelocks at ~2137-2149 and the chunk loop at 1839, which contains `txg_wait_synced` at 1874 and `dmu_tx_assign` at 1897); `zvol.c:649`; Linux `zpl_file_range.c:85,134`; FreeBSD `zfs_vnops_os.c:5815,6931` | `dmu.c:2969` `brt_pending_add` → `brt_pending_apply` (`spa.c:~11343`) → `brt_sync` → `brt_vdev_create` (`brt.c:1570`) → `brt.c:487`, with **no re-check** | C-inflight: a clone syscall in progress (unbounded loop). ZIL TX_CLONE_RANGE replay only asserts enabled (`zfs_vnops.c:2924`, `zvol.c:573`). | No |
| physical_rewrite (PER_DATASET) | `zfs_vnops.c:1181-1185` (once, at `zfs_rewrite` entry) | Loop at 1215-1290 (`dmu_tx_assign` per chunk at 1254, `dmu_buf_will_rewrite` at 1280) → `dbuf.c:5452-5465` (arms `ds_feature_activation` at dbuf write), then G3 | C-inflight: a rewrite syscall in progress over an arbitrary range. | No |

### Class D: other

| Feature | Why |
|---|---|
| allocation_classes | Adding a vdev is B (G2): chk `vdev.c:891-899`, act `vdev.c:3698-3699` from `vdev_construct_zaps` in the config sync. **The `vdev_prop_set` ALLOC_BIAS path is not:** <ul><li>The check is in open context under `SCL_CONFIG` as reader only (`vdev.c:6627-6635`), and the in-memory bias is changed at once (6656).</li><li>`dsl_sync_task(spa, NULL, vdev_props_set_sync)` (6678) then has **no** check function. Its `dmu_tx_assign(DMU_TX_WAIT|DMU_TX_SUSPEND)` (`dsl_synctask.c:51`) can wait without bound.</li><li>`vdev_props_set_sync` increments with only an ASSERT (`vdev.c:6343` → `spa_misc.c:1833-1834`).</li><li>The path takes no `spa_namespace_lock`, so a disable under `spa_vdev_enter` does not serialize with it.</li></ul> Never decremented. |
| redaction_bookmarks | `zfs redact` is B: `dsl_bookmark.c:650-652` (G1) → 472-473. **But a plain `zfs bookmark` of a *redacted snapshot* increments it with no check:** `dsl_bookmark_create_check()` checks only BOOKMARKS (298); `dsl_bookmark_create_sync_impl_snap()` then sees `bookmark_redacted` (456-458) and increments at 472-473. REDACTED_DATASETS does not depend on REDACTION_BOOKMARKS. This is armed by any snapshot with redacted_datasets active, which is cheap to detect as `spa_feature_is_active(REDACTED_DATASETS)` at pool level. It looks like an **upstream panic path too** when redaction_bookmarks was never enabled **(inferred, untested)**. |
| redaction_list_spill | `zfs redact` is B (snaplimit check `dsl_bookmark.c:658-663`, then 474-477). The same unguarded `zfs bookmark` path increments it at 474-477 whenever the redacted snapshot carries more snapshots than fit in the bonus buffer (`dsl_bookmark.c:462-465`). Same arming and detection as redaction_bookmarks. |

Totals: A = 7, B = 24, C = 14, D = 3 (48 features).

## 4. Answers to the specific questions

### PER_DATASET features

There are 15:

* large_blocks
* large_dnode
* sha512
* skein
* edonr
* redacted_datasets
* userobj_accounting
* encryption
* project_quota
* zstd_compress
* zilsaxattr
* blake3
* longname
* large_microzap
* physical_rewrite

All take their flag from the registrations in `zfeature_common.c` (`zpool_feature_init`).

### Does "PER_DATASET ⇒ class C" hold?

**No.**

* **redacted_datasets and large_microzap are B.** Their checks run under the
  same sync task or an assigned tx as the activation.
* **encryption is B except for one C-inflight path**, raw receive key load.
* The other 12 are C.

### Non-per-dataset features that are C or not cleanly B

* **block_cloning** is C-inflight.
* **allocation_classes** is D (the vdev-property gap).
* **resilver_defer** is B, but relies on an in-memory flag that is consumed
  without a re-check. Detect it via `spa_resilver_deferred`.
* **redaction_bookmarks and redaction_list_spill** are D (no guard).

The rest of the features the brief asked about are A or B:

* fast_dedup: B, not armed by `dedup=on`
* log_spacemap: B, but hazard H3
* spacemap_v2: A
* livelist: B
* device_removal: B
* obsolete_counts: B, with hazards H2 and H3
* raidz_expansion: B
* draid: B
* head_errlog: A
* bookmark_v2: B
* bookmarks: B
* bookmark_written: B
* vdev_zaps_v2: B
* dynamic_gang_header: B
* large_microzap: B
* redacted_datasets: B

### B-shaped paths where the gap between check and `dmu_tx_assign` is unbounded

These break the scheme:

1. `zil_commit_activate_saxattr_feature`: `zil.c:966` check, then `zil.c:970`
   `dmu_tx_assign(WAIT|SUSPEND)`.
2. `zfs_clone_range` and `zvol_clone_range`: a single precheck, then
   rangelocks, a loop, `txg_wait_synced` and per-chunk assigns (see the
   block_cloning row).
3. `zfs_rewrite` with `ZFS_REWRITE_PHYSICAL`: check at 1182, loop with an
   assign at 1254.
4. `dmu_objset_id_quota_upgrade_cb`: check at 2410/2413, then
   `dmu_bonus_hold` I/O and `dmu_tx_assign(WAIT)` per object, possibly never
   (EINTR).
5. `vdev_prop_set` ALLOC_BIAS: check at `vdev.c:6633`, then a sync task with
   no check (`dsl_synctask.c:51`).
6. Raw receive: `dmu_recv_begin_check` at `dmu_recv.c:693`, then the key sync
   task (`dsl_crypt.c:2459`) with no re-check.
7. Property-armed paths. These are checks in `zfs_check_settable` with no
   feature re-check in the property sync task:
   * recordsize, volblocksize, dnodesize, checksum, dedup, compression,
     longname.
   * Received props: longname skips its check entirely
     (`zfs_ioctl.c:3077-3084`).

### Upstream issues noticed in passing (all inferred, none reproduced)

* **Unguarded bookmark activation.** `zfs bookmark` of a redacted snapshot
  increments REDACTION_BOOKMARKS (and possibly REDACTION_LIST_SPILL) without
  any enabled check. See the D rows above.
* **Received `longname` property.** A received `longname=on` property is
  applied without checking that LONGNAME is enabled
  (`zfs_ioctl.c:3077-3084`).
  * The comment there relies on `dmu_recv_begin_check()`, but that only fires
    when the stream carries `DMU_BACKUP_FEATURE_LONGNAME`, i.e. when the
    source already had long names.
  * Creating a long name afterwards would increment a feature that is not
    enabled.
* **Per-object checksum and compression from streams.** A received
  `DRR_OBJECT` sets the per-object `dn_checksum`/`dn_compress` with only a
  range check (`dmu_recv.c:1989-2003`, `3519-3522`). A crafted stream naming
  sha512, skein, edonr, blake3 or zstd can make G4 increment a feature that
  is not enabled. Normal senders send INHERIT, since ZPL never calls
  `dmu_object_set_checksum`; only ztest does (`cmd/ztest.c:5342`).
* **Features never decremented.** draid, draid_failure_domains,
  raidz_expansion, allocation_classes, filesystem_limits and vdev_zaps_v2
  have no `spa_feature_decr` anywhere.
  * Once active, they stay active.
  * The local commit message's example ("a pool … whose last dRAID vdev was
    removed") does not seem reachable: no dRAID decrement exists, and
    top-level removal refuses vdevs with parity. `spa_vdev_remove_top_check`
    returns EINVAL when `vdev_get_nparity(cvd) != 0`
    (`vdev_removal.c:2329-2330`), and `vdev_remove_initiate_sync` asserts it
    at `vdev_removal.c:448`.

## 5. Bottom line for the maintainer's scheme

* **Class A (7):** nothing to do. They cannot reach refcount 0.
* **Class B (24):** steps 1–3 work, provided four conditions hold.
  * a. The mark is in `spa_feature_is_enabled()` only.
  * b. The dependent check in step 3 ignores the mark.
  * c. Step 3 is dispatched after step 2.
  * d. H1–H3 are handled, for example with the "spoil on incr" idea, or by
    refusing log_spacemap, obsolete_counts and livelist outright.
* **Class C (14):** not safe with a txg wait alone.
  * Property-armed features would need a dataset-property walk and a
    re-check of the property set.
  * In-flight features would need counters for active operations, or a
    re-check at activation time.
  * USEROBJ_ACCOUNTING and PROJECT_QUOTA would need the upgrade path
    re-checked when `ds_feature_activation` is applied.
* **Class D (3):**
  * allocation_classes could become B by moving the ALLOC_BIAS feature check
    into a sync-task check function.
  * redaction_bookmarks and redaction_list_spill need an enabled check in
    `dsl_bookmark_create_sync_impl_snap`'s redacted-snapshot branch. Until
    then, refuse while redacted_datasets is active.
