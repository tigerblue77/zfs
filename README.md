# Handover notes: zfs rewrite for RAID-Z restriping (state on 2026-10-20)

Notes for whoever continues this work. Branches named here live on this fork.

## Goal

After a RAID-Z expansion the existing blocks keep their old width (RAIDZ2 5 -> 6
disks: 40 % parity instead of 33 %). Goal: put them at the new geometry, in
place, volumes and snapshot-held blocks included, without send/recv, and in the
end automatically in the background. Agreed path, in three stages:

1. a working tool, merged upstream: `zfs rewrite` (volumes, -W, -P race, nopwrite);
2. only once that is merged, propose automation to the maintainers;
3. then a background engine, shared with the block-redirection mechanism of the
   parity-change work (it also serves special vdev evacuation and RAID-Z disk
   removal). Nothing about automation upstream before stage 1 is merged.

## Upstream state

| PR / issue | Subject | State |
|---|---|---|
| #19374 | ZTS: zfs rewrite on an expanded RAID-Z vdev, plus doc cross-references | merged |
| #19376 | zfs rewrite -W (skip blocks already at the current width) | merged |
| #19377, #19398 | zvol rename: EPERM (#19371), ENOENT and rollback crash (#19397) | merged |
| #19367 | zfs rewrite: support volumes | open, no review |
| #19370 | -P: a concurrent write loses its birth (#19368) | open, no review |
| #19375 | do not nopwrite rewrite blocks (#19372) | open, no review |
| #19495 | -W skips only blocks a rewrite would keep (compression, checksum, copies, dedup, allocation class) | open since 2026-10-20 |
| #19366 | rewrite -P breaks raw incremental replicas of encrypted datasets | waiting for amotin on proposal D; the thread ends on our comment, no new comment |
| #3111 | our proposal of a `zhack metaslab rebuild` command | no answer |

## Candidate PRs (fork branches; open only once fork CI is green on the exact SHA, 5 PRs/day for the whole account)

| Branch | SHA | Subject | Fork CI (zfs-qemu run) |
|---|---|---|---|
| vdev-add-event-details | 8c3784644 | #7107 vdev_add event details | 39156050299 attempt 2 queued; only failure was events/zed_io_config (unrelated) |
| redact-delegation | 7ed118dd3 | #10296 delegatable redact permission | 39157881670 queued |
| zfs-destroy-type | 339708354 | #9522 zfs destroy -t | 39180242526 queued |
| zfs-dirsync | 886e5d787 | #4366 dirsync property | 39161892519 attempt 2 queued (autotrim_config flake, runner-image failures, cancelled jobs) |
| zil-commit-error-release | d6ccf2588 | znode leak when zil_commit fails | 39195174957 queued |
| zvol-logical-block-size | 8ce6c9148 | #4203 volsectorsize | 39201474685 in progress |
| raidz-gfni | e3e74feba | GFNI RAID-Z parity math | 39152867071 cancelled (CI capacity); re-run when possible |
| aes-gcm-vaes-avx512 | ce7e33699 | VAES AVX-512 AES-GCM | 39147391434 cancelled; rebase after #19411 merges |

PR descriptions, following the upstream template: `pr-bodies/<branch>.md`. All
of these branches merge cleanly on master a0c443a85. Measurements for GFNI and
AES are in their descriptions: i7-1165G7, AMD EPYC 9V74 (Zen 4) and 9V45 (Zen 5)
from the `bench-simd` branch.

## After merges

- `rewrite-integration-1017` merges the open rewrite branches together. See
  `integration/RECIPE-NOTES.md` for the conflict resolutions to replay when
  rebasing (volumes x -W, volumes x nopwrite, nopwrite x race).
- Once #19367 and #19495 are both merged: apply the -W check to volumes in
  `dmu_rewrite_skip()`.
- Once #19375 is merged: add a sha256 (nopwrite) case to raidz_expand_rewrite.
- Once #19495 is merged: discuss with the maintainers whether skipping
  unchanged blocks should become the default, with an option to force a full
  rewrite. It was kept off by default because of 2.4 compatibility and the
  cases a block pointer cannot reveal: zstd level, incompressible block, rebalancing.

## Deferred or set aside

- Temperature-based tiering through zfs rewrite: after the OpenZFS Summit
  (2026-10-28) and the rewrite merges.
- Local Reconstruction Codes: closed by the maintainer, because they would
  hurt fully heterogeneous disks (5 x 22 TB + 12 x 6 TB). Study in `lrc/`.
- Other accelerations reviewed on 2026-10-17/18 and not retained:
  - ARM kernel SIMD: overlaps #16601 and #18743;
  - NEON fletcher4 (#14219): untestable without ARM hardware;
  - zstd assembly in the kernel: disabled on purpose by #18089;
  - SVE, RISC-V, QAT, IAA: no usable kernel API;
  - Cauchy RS with more than 3 parities (#558): no design upstream;
  - xxh3 (#16503), LZ4 HC (#1900): blocked upstream;
  - CRUSH and AMX: not applicable.
- #6824 and #11524: they need a design decision first.

## Noted, not investigated

- events/zed_io_config failed once on ubuntu26 (vdev not FAULTED within 60 s, io_n=1 case).
- cli_root/zinject/zinject_args failed in 3 of 6 reruns on a slow test VM.
- txg_sync and zpool hung-task warnings (>120 s) on slow VMs.

## Test environment

The full ZTS of zfs-rewrite-skip-unchanged (6dbd356) was run on the test VM
CHSONVTST02: no test fails on the branch while passing on master. Its logs are
in ~/logs/rewrite-skip-unchanged/ on that VM. The VM was released on 2026-10-20
with its modules unloaded.
