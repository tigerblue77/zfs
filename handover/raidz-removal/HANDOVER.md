# Handover: RAIDZ vdev removal work (2026-10-20)

Technical notes for whoever continues this work. Branch heads are on
tigerblue77/zfs unless stated. Upstream master at writing: a0c443a85.

## Upstream items

| Item | What | Head | State on 2026-10-20 |
| --- | --- | --- | --- |
| PR #19373 | zpool remove of an empty top-level vdev of any type -> hole (Part of #9013) | zpool-remove-empty-vdev-v2 5ae071e4e | no review; 2 red qemu jobs = trim/autotrim_config flake |
| PR #19452 | Closes #19417: account a growing non-allocating vdev once | noalloc-expand-accounting f8756736b | no review |
| PR #19470 | activate a vdev only if it is not allocating yet (race) | vdev-alloc-recheck ceede6e48 | no review |
| PR #19471 | ZTS detach: pass the pool to the detach that must fail | zts-detach-pool-arg c25ca08a7 | approved |
| PR #19493 | checkpoint rewind writes uberblock to checkpointed vdevs | checkpoint-rewind-uberblock dc480498f | opened 10-20 |
| PR #19494 | log removal: erase labels once the hole is on disk | log-remove-label-order ec1a6be19 | opened 10-20 |
| PR #19496 | libzutil: keep the latest label when two vdevs claim one id | import-stale-top-label b5f8ef9c7 | opened 10-20 |
| Issue #19439 | disable an inactive pool feature | pool-feature-disable-v2 625dc041b | PR not opened; fork CI never ran |
| Issue #9013 | raidz removal | - | waiting for amotin since 10-15 20:44Z; do not ping |
| Issue #19492 | CI: no linux-modules-extra for 7.0 azure kernels | ci-modules-extra 04517b6e6 | own PR dropped for #19498 (wrouesnel, same fix) |

## Candidate PRs not opened yet (fork branches)

| Branch | SHA | Base | Draft body | Blocker |
| --- | --- | --- | --- | --- |
| rebuild-cancel-state-lock | 6b6fe6f09 | 1e9b428dc | drafts/rbcancel_pr_body_draft.md | fork CI: run 39248215556 had 3 red qemu jobs to triage, smatch/arm hit #19492 |
| validate-label-txg | 523ad30cd | 073e32604 | drafts/validate_txg_pr_body_draft.md | 2 red jobs, unrelated flakes (zfs_copies_006, removal_with_errors hang); placeholders #REWIND_PR=#19493, #SCAN_PR=#19496, #LOG_PR=#19494, #ZTEST_PR = PR of ztest-import-after-kill when it exists |
| detach-renamed-import | b7695349c | 073e32604 | drafts/detach_path_pr_body_draft.md | 1 red job (dbufstats_002 flake); #ZTEST_PR as above |
| import-erased-leaf-paths | a1842b396 | b7695349c (stacked) | drafts/leafpaths_pr_body_draft.md | open after detach-renamed-import; #DETACH_PR, #VTXG_PR |
| pool-feature-disable-v2 | 625dc041b | a0c443a85 | drafts/fdis2_pr_body_draft.md | fork CI never ran; RESULT placeholder |
| remove-not-empty-message | 44220e5a7 | on 5ae071e4e | drafts/not_empty_pr_body_draft.md | after #19373 merges, then rebase (conflicts in zfs.h and pyzfs constants, trivial) |
| ztest-import-active | ebc8c4861 | 073e32604 | - | abandoned in favour of ztest-import-after-kill e9da0006f |

All of them need a rebase on master once #19498 lands, then a fully green
fork CI on the exact SHA before a PR is opened.

## Commits that existed only locally

The head commit of this branch has these as extra parents, so every commit is
on the fork. Create a branch from any of them if needed.

| SHA | What |
| --- | --- |
| a0547407d | scratch-all3: all fixes stacked (fuzz tree, 0 failures over 300 seeds) |
| dda9bed3a | scratch-all2: same minus variant B |
| ee7af386a | scratch wt-all: #19373 + log + scan + rewind combined |
| 80e6a653c | prototype tree for the detach DEGRADED variants (with wip-diffs/wt-proto.uncommitted.diff) |
| a377414e0 | validate-label-txg on top of import-erased-leaf-paths |
| a0c0e1062 | first #19439 prototype (flag design, superseded) |
| 72c49ee87, 916305f0f | older versions of #19452 |
| 4a6caca4c | older version of #19373 |
| 0a38796f7, 79b56b613, 897495489, b64b68132, c3c4723f4 | local conflict resolutions of other PRs (#19367, #19389, #19375, #19377, #19365); superseded by their own rebases |

## #9013 design

design/raidz_removal_facts.md holds the facts. Agreed order: A then B.
- A: raw-segment removal of a raidz vdev onto raidz vdevs of the same width,
  the door amotin opened on #9013 (indirect vdev keeps the source geometry,
  split segments keep column alignment). Waiting for amotin.
- B: remove a disk from a raidz (N -> N-1) or the last raidz: rewrite live
  blocks at the new width (zfs rewrite work, #19419) and map only the blocks
  that snapshots, clones or dedup still hold. Measured on 10-15: rewrite moves
  all live data; snapshot-held blocks remain, and 22 metadata blocks remained
  after snapshot destroy.

## Probes and scripts

probes/ holds the userspace libzpool probes (build against a debug
--with-config=user tree) and the VM scripts. $SCRATCH stands for the old
scratch directory. Notable: crash_fuzz_probe_v3*.c (crash fuzzer used to find
the import bugs), rebuild_wait_probe.c and rbcancel/ (rebuild race),
feature_disable_probe_v2.c (#19439 v2), empty_remove_probe.c (#19373).

## Known open questions (not pursued, waiting for a decision)

- removal/removal_with_errors hang seen on 5 branches in CI; 100/100 pass in
  a VM loop; no stacks captured.
- Kernel Oops strnlen NULL in spa_lookup from zpl_get_tree (ubuntu26, 7.0),
  seen once in mount/mount_null_source.
- Partial reguid (new guid on some devices only) and states written by old
  code that still import DEGRADED after renaming.
