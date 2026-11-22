TASK-1121-HRA1 (from the coordinator session "main"): performance, on VM 105 (CHSONVTST02) booted once into 7.0.14-25-pve, pass 1 of 6 of the host rig's arms, about 3 h 45 on the VM plus the wait for main's GO, at most 4 h 50 without that wait (the first task builds seven trees, about 1 h 50). Its measured part (steps 7 and 8, about 70 min, never more than 85) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 5 are functional and may run while other VMs work. The rig is TASK-1121-HRIG's (five emulated hard disks served by `hddsim`, a raidz2 filled like the host's pool, the host's guest on a 1M zvol), at the device configuration calibration chose, and is switched before every run to one of 15 arms: a code tree, module parameters and device settings each. If TASK-1122-HRIG3's verdict is NOT CALIBRATED, the rig is labelled "not calibrated" (the owner's decision of 21/11): the series runs on the closest point measured, its comparisons between arms are valid, and its absolute values are not the host's. This task runs each arm once, in a fixed order; six such tasks make the six passes the verdicts need. Two reboots are pre-approved for this task only (into 7.0 at step 3, back to Debian at step 10); for this task that replaces the standing approval of 18/11. Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1121-hra1/report.md` and the same text as your final message.

**Filled by main (22/11):** kit v2.1 commit `cca50b0943d674176aa10f98fdcfc271ef392bae` (fork branch `vm-kit-hra21`, no parent, tree `b50c3f0a74fd73077b596376dd793013cfa71765`); the rig's point `grid/G7.conf` (sha256 `538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8`) with the guest at its defaults (`-`), from TASK-1122-HRIG3's verdict line `TASK-1122-HRIG3 VERDICT: NOT CALIBRATED g7-50mbs: P1; closest: grid/G7.conf 538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8 GUEST=-`. The rig is therefore labelled **not calibrated**: every result of this task says so.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. The final report is sent after the reboot of step 10.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1121-hra1`, `L=/var/log/task-1121-hra1`, `R=/var/tmp/hra-trees` (the trees, kept from one arms task to the next); the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a value is a finding (then it is reported, not a stop).

## Why

The owner's host (Dell T630, kernel 7.0.14-29-pve, OpenZFS 2.4.4, five SATA hard disks behind mpt3sas in a raidz2, scheduler `none`, blk-wbt at 75 ms on all five, NCQ priority supported and off) shows, under its Plex VM: sync-write queue wait p99 4295 ms (p50 0), disk read p99 1074 ms, disks 22 % busy, PSI io 8 %, and 62 % of the disks' bios submitted by `z_wr_int` threads (then `z_wr_iss`, `zvol_tq`, `z_null_iss`, `txg_sync`), which blk-wbt can put to sleep. TASK-1122-HRIG3 found no point of the rig that reproduces all of this: the closest, G7 (2 MiB of write cache) at the guest's defaults, meets every criterion but disk busy (38 % against the host's 22 %), so the rig is labelled not calibrated (the owner's decision of 21/11): comparisons between arms are valid, absolute values are not the host's. Every remaining lead is measured on it here, each to a verdict, whatever it is:

| Lead | Arms | Question |
| --- | --- | --- |
| (a) the host's code | R0 | the reference: OpenZFS 2.4.4 as the host runs it |
| (b) upstream master | M0 | does master (c913da86156, the base of PR1 and the age guard) behave as 2.4.4 |
| (c) PR1 (#19981) | P1, P0 | sync writes and flushes submitted REQ_SYNC\|REQ_IDLE, out of blk-wbt (mode 1), against the same tree with the flag off (mode 0) |
| (d) M-29 | P1I | PR1 with `zfs_immediate_write_sz=2097152`: the guest's 512 KiB records logged as copies instead of indirect (whole-1M `dmu_sync()` bursts) |
| (e) age guard | P1A | PR1 plus `zfs_vdev_queue_age_ms=50` (fork commit c9053179c67): sync I/O that held its slot 50 ms stops counting against its class limit |
| (f) blk-wbt itself | R0W, P1W | blk-wbt off (`wbt_lat_usec=0`) on 2.4.4 and on PR1: what the host's blk-wbt costs, since `z_wr_int` submits most bios and PR1 only takes sync writes and flushes out of it |
| (g) NCQ priority | QM, QR, QR2, PQR, PQ3 | PR2 v2 (`zfs_vdev_disk_rt_mask`, the kit's `patches/`): bit 1 gives synchronous-read bios, bit 2 synchronous-write bios, the real-time I/O class (level 4), which libata and mpt3sas turn into the NCQ high-priority bit when `ncq_prio_enable` is set; a thread that chose its own I/O priority or scheduling class keeps it (never the case on the rig: only kernel threads submit). The emulated disks honour the mark or not |
| (h) PR3 | pr3, pr3o | PR3 on 2.4.4 (tree P3W: PR1 and PR3's in-flight cap, read-latency window and its trace, backported; a test-only commit with which `zfs_vdev_disk_sync_writes=2` marks every write REQ_SYNC\|REQ_IDLE, 2.4 having no queue bypass; no age guard), window on (pr3) against the same tree with the window off (pr3o) |

## What you fetch

| What | Source | Commit | Tree |
| --- | --- | --- | --- |
| kit v2.1 | https://github.com/tigerblue77/zfs, by commit (branch `vm-kit-hra21`) | `cca50b0943d674176aa10f98fdcfc271ef392bae` (no parent) | `b50c3f0a74fd73077b596376dd793013cfa71765` |
| R, OpenZFS 2.4.4 | https://github.com/openzfs/zfs tag `zfs-2.4.4` (tag object `f75f3256a0916c5ba090b088e6ea5a9528f6f956`) | `71a9f9578616a90c3c14bb59629fb4d31bfd68d1` | `d2a62ec4527f872ce840ba60f50248e66fcc080b` |
| M, master | the fork, by commit (A~3) | `c913da86156eba3243cc1ef515e98d69887654a7` | `7e32938edf1a54c02519e2dc0104e9325b5a6f4d` |
| P, PR1 | the fork, by commit (branch `vdev-disk-bio-sync-flags`) | `846b9194b4a3eb54f601c1bda0382b68d785b0ef` | `9a4214cf8f0887e110763ef53c666c8527c2ca9e` |
| A, age guard on PR1 | the fork, by commit (branch `vdev-queue-age-guard`) | `c9053179c6708aa0231f024a4e294a2a4f1026e8` | `836581447f1c5a7b2c431e798770c200351896f8` |
| RQ, R + the kit's `patches/r244-rt` | made here (`git am`) | `d14d9bebae01bc853919777c731cac4d416aefd1` | `e4620bb6a674b72ae1391f946e8c521b7eec7e0c` |
| PQ, P + the kit's `patches/pr1-rt` | made here (`git am`) | `f8c8df27e4a729b99c2f0ebf7c517a39fb6a3861` | `0ee3a37d70b16485259adf6139684279a4b1666c` |
| P3W, R + the kit's `patches/p3w-244` (six commits) | made here (`git am`) | `2738e2883136b191618647b2cf1191df745b0e04` | `e9321e519235f9d63f8a3dd9cc5218a100734279` |

`hr-trees.sh` (in the kit) fetches, checks and builds these; every value above is compared by the program and printed `OK` or `FAIL`. P3W's patch set is the 2.4.4 backport of PR1 (two commits) and PR3 (`vdev_queue: add zfs_vdev_queue_window, capping active I/Os`, `... adjust zfs_vdev_queue_window to read latency`, `... trace the window of zfs_vdev_queue_window`), then a test-only commit (`vdev_disk: 2.4 test backport: zfs_vdev_disk_sync_writes=2 marks all`). RQ's and PQ's are two commits each: the change proposed upstream (`vdev_disk: add zfs_vdev_disk_rt_mask`, PR2 v2), and a rig-only one (`RIG ONLY: carry the real-time class to ublk as REQ_META`): ublk does not pass a request's I/O priority to its server, so on the rig the real-time bios also carry REQ_META, which ublk passes and hddsim reads as the high-priority bit. The marker sets it on a real-time read, and on a real-time write only when the write already carries REQ_SYNC|REQ_IDLE, which blk-wbt leaves alone (so REQ_META cannot change blk-wbt's limit): on 2.4.4 no write is marked, on PR1 with `zfs_vdev_disk_sync_writes=1` the synchronous writes are. Nothing else reads REQ_META on this rig (scheduler none, no blk-cgroup I/O controller).

The rig's point (`grid/G7.conf`, the closest measured, not calibrated): the kit's grid files, G1 to G6 the same as TASK-1121-HRIG's, G7 and G8 TASK-1122-HRIG3's (G5 with 2 MiB and 4 MiB of write cache):

| File | sha256 |
| --- | --- |
| `grid/G1.conf` | `f79f57a7cec57a602c0c71acad38f1803af1d63ed0a1558c5ca91429142f056b` |
| `grid/G2.conf` | `eec8a376ff13d234b3c14935f0ec4e25242c4cb4d7ceea5f9509f014daf42256` |
| `grid/G3.conf` | `bb601f6e7c4ead34391e2e8d55ffa8a210b250fc298243ce39cd2349bebd537f` |
| `grid/G4.conf` | `d879b967ac50153f9b52f5ef5cbe6bcc44beb0154c4a173820e35c8084ce0b93` |
| `grid/G5.conf` | `0b721c92bb9fc53f4cc257a1a2f2ac591e0c48871c1e06310c31efabd340ad4b` |
| `grid/G6.conf` | `3ee3cb3731b5323064c03ea239185fa63ad3a157ba2d7a1a185796b39147285b` |
| `grid/G7.conf` | `538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8` |
| `grid/G8.conf` | `fa7ec5a6b920725373c83e78321824294366859058633fa35a892fc213dfb875` |

## The rig (`hr-bench.sh`, kit v2.1)

As TASK-1121-HRIG (disks `/dev/ublkb70`-`74`, 16 GiB, 4K, rotational, volatile write cache, scheduler `none`, iostats; pool `hrp` raidz2, ashift 12, compression off, `cachefile=none`, zvol `hrp/vm` 1M filled whole, filler to 66 %; the same ZFS settings after every module load; the same guest, sampler and counters; the guest runs with TASK-1122-HRIG3's guest settings (`-`) when they are not `-`), with these differences:

- **The model** is `$L/best.conf`, the calibrated point, for every arm, plus the arm's `ncq_prio`. hddsim v2 adds NCQ priority (`ncq_prio`, off by default; with it off the model is TASK-1121-HRIG's, checked at step 4): a request that arrives with UBLK_IO_F_META is marked; with `ncq_prio=1` a marked command waiting for the media is served before every normal one in the NCQ (shortest positioning first among the marked), the oldest command past `age_ms` (1 s) still going first whatever its mark, the commands waiting above the NCQ staying in arrival order, a media operation in progress never interrupted, flushes and the destaging the drive forces (cache above 75 %, a write waiting for space) unchanged; with `ncq_prio=2` marked commands also go before that forced destaging, until a write has waited `age_ms` for space. ATA leaves how a drive serves high-priority commands to its firmware: 1 is the reading that assumes least, 2 the most. Each disk's statistics line counts the requests of the second that arrived marked (`mark r w f o`), and `rej` the requests answered with an error since the start (kit v1.1's counter; v1.1's flush fix is in too).
- **Every run switches the rig to its arm**, on fast disks (every request completes at once): the pool is exported, the arm's tree loaded (`scripts/zfs.sh -u`, then `scripts/zfs.sh`, srcversion checked against the build), the common settings and the arm's parameters written and read back, the pool imported (`-d` on the five disks only, `-o cachefile=none`), the guest's database and journal areas (320 MiB) read once through the zvol so that their partial-block writes find their blocks in the ARC, then the disks get the model and blk-wbt its target (1 first, so that it restarts from its initial depth). `arm.txt` in the run's directory records what is loaded: the tree's stamp, srcversion loaded and built, every zfs and spl module parameter, every disk's sysfs queue values, every server's parameters.
- **One run** is 45 s of ramp and 180 s measured (TASK-1121-HRIG's measures otherwise), about 4 min 25 s with the switch. A warm-up run (30 s + 60 s, pass `W<pass>`, not in the verdicts) of the pass's last arm comes first, so that the first arm also follows another.

| Arm | Tree | blk-wbt | `ncq_prio` | Module parameters written |
| --- | --- | --- | --- | --- |
| R0 | R | 75000 | 0 | - |
| M0 | M | 75000 | 0 | - |
| P0 | P | 75000 | 0 | `zfs_vdev_disk_sync_writes=0` |
| P1 | P | 75000 | 0 | `zfs_vdev_disk_sync_writes=1` |
| P1I | P | 75000 | 0 | `zfs_vdev_disk_sync_writes=1 zfs_immediate_write_sz=2097152` |
| P1A | A | 75000 | 0 | `zfs_vdev_disk_sync_writes=1 zfs_vdev_queue_age_ms=50` |
| R0W | R | 0 | 0 | - |
| P1W | P | 0 | 0 | `zfs_vdev_disk_sync_writes=1` |
| QM | RQ | 75000 | 0 | `zfs_vdev_disk_rt_mask=3` (control: marked, the disks ignore marks, the host with the patch and NCQ priority off; must equal R0) |
| QR | RQ | 75000 | 1 | `zfs_vdev_disk_rt_mask=1` |
| QR2 | RQ | 75000 | 2 | `zfs_vdev_disk_rt_mask=1` |
| PQR | PQ | 75000 | 1 | `zfs_vdev_disk_sync_writes=1 zfs_vdev_disk_rt_mask=1` |
| PQ3 | PQ | 75000 | 1 | `zfs_vdev_disk_sync_writes=1 zfs_vdev_disk_rt_mask=3` (synchronous writes marked too) |
| pr3 | P3W | 75000 | 0 | `zfs_vdev_queue_window=1 zfs_vdev_queue_window_max=32 zfs_vdev_queue_window_rotating_us=75000 zfs_vdev_disk_sync_writes=2` |
| pr3o | P3W | 75000 | 0 | `zfs_vdev_queue_window=0 zfs_vdev_queue_window_max=32 zfs_vdev_queue_window_rotating_us=75000 zfs_vdev_disk_sync_writes=2` (pr3 with the window off: only the window switch differs) |

Every other parameter is at its tree's default (the modules are reloaded for every run). On pr3 runs (window on) the driver also records the window's trace during the measured window, best effort and never a stop: `window_trace` in `layout.txt` says how (`event`: the tracepoint `zfs:zfs_vdev__queue__window`, with each leaf's window; `kprobe`: the module was built without tracepoints, and a kprobe on the stub `trace_zfs_vdev__queue__window` records only the vdev pointer and the cap per adjustment; `none`), and `window.txt` holds the lines. PQ3 with `ncq_prio=2` is not in this series: if PQ3 shows an effect, it gets a pass of its own.

## The plan

Six tasks, one pass each; the arm orders are rows 0, 1 and 2 of a Williams design for 15 arms, each followed by its reverse, which an odd number of arms needs for carry-over balance (`plan.txt`): every arm sits in mean position 7 of 0-14 over the six passes and follows six different arms. Comparisons pair the runs of the same pass, so each pair was measured within the same hour.

| Task | Pass | Row | Order (warm-up: the last arm) |
| --- | --- | --- | --- |
| HRA1 | A1 | 1 | R0 M0 pr3o P0 pr3 P1 PQ3 P1I PQR P1A QR2 R0W QR P1W QM |
| HRA2 | A2 | 2 | QM P1W QR R0W QR2 P1A PQR P1I PQ3 P1 pr3 P0 pr3o M0 R0 |
| HRA3 | A3 | 3 | M0 P0 R0 P1 pr3o P1I pr3 P1A PQ3 R0W PQR P1W QR2 QM QR |
| HRA4 | A4 | 4 | QR QM QR2 P1W PQR R0W PQ3 P1A pr3 P1I pr3o P1 R0 P0 M0 |
| HRA5 | A5 | 5 | P0 P1 M0 P1I R0 P1A pr3o R0W pr3 P1W PQ3 QM PQR QR QR2 |
| HRA6 | A6 | 6 | QR2 QR PQR QM PQ3 P1W pr3 R0W pr3o P1A R0 P1I M0 P1 P0 |

This task: pass **A1**, order `R0 M0 pr3o P0 pr3 P1 PQ3 P1I PQR P1A QR2 R0W QR P1W QM`, warm-up `QM`. Projected: quiet check 1 min, warm-up 2 min 10, 15 runs of about 4 min 25: **about 70 min from GO**; the last run starts about 65 min after GO, and the time rule would refuse to start a run after GO + 79 min 45 s (about 15 min of margin).

**Time rule** (keeps the measured part under 85 min; the driver applies it): `hr-bench.sh pass` starts no run that could end after GO + 85 min (a run is counted 45 + 180 + 90 s), and drops it and every later arm of the pass, saying so in driver.log. A dropped arm is a finding, not a stop.

## Written before the run

**Validity of a run** (`hr-report.py arms`): the guest ended (`END`, no `ERROR`); the disks were in model (models 1); completions more than 2 ms late at most 1 % and no store error (V1); no request refused during the window; the run's `arm.txt` shows the arm's tree (srcversion loaded = built = the stamp's), every parameter the arm writes, blk-wbt at the arm's value and `ncq_prio` at the arm's on all five disks; the disks saw marked reads exactly when the loaded `zfs_vdev_disk_rt_mask` has bit 1, and marked writes exactly when it has bit 2 and `zfs_vdev_disk_sync_writes` is 1 or more. Only valid runs count.

**Fixed metrics** (per arm: median, lowest-highest, number of valid runs): guest fsync p50, p90, p99, max; guest read p99; syncq_wait write p50 and p99 and disk_wait read p99 (zpool iostat -w, log2 buckets); disk busy (diskstats); txg sync mean and p95; `z_wr_int` asleep in blk-wbt (% of the blocked-task samples); guest media MB/s; pool write MB/s; and, shown but not compared, PSI io some (87.4 to 87.9 % in every calibration run on this rig, whatever the disks did), completion lateness, marked reads per second, sync-write queue occupancy, ZIL commits, needcopy records, skipped operations, steal, load1.

**Comparison X/REF** (VQ1's rule): in each pass where both ran validly, ratio = X / REF (each raised to the metric's floor: 1 ms, 1 % busy or wbt share, 0.05 s txg, 1 MB/s), inverted for a throughput so that below 1 is better; over the n pairs g = geometric mean, p = exact two-sided sign-flip permutation p. **better** if g <= 0.97 and p <= 0.05, **worse** if g >= 1.031 and p <= 0.05, otherwise **no difference shown** (disk busy and pool write MB/s: **lower**/**higher**). With six pairs p <= 0.05 needs all six on the same side; below six pairs nothing can be shown (pr3 if PR3 arrives late).

**Verdict per lead**: **HELPS** when a primary metric is better by 10 % or more (g <= 0.90), no primary is worse and no guard is worse; **TRADE-OFF** when a primary is better by 10 % and something is worse; **HURTS** when no primary is better by 10 % and a primary or guard is worse; otherwise **NO EFFECT SHOWN**. A control **FAILS** when a metric moved by 10 % or more, else is OK (smaller shifts named).

| Id | X / REF | Kind | Primary | Guards |
| --- | --- | --- | --- | --- |
| C1 | P1 / P0 | lead: PR1 | fsync p99, p50, syncq w p99, z_wr_int in blk-wbt | read p99, disk read p99, guest MB/s, txg mean |
| C2 | P0 / M0 | control | every metric | |
| C3 | M0 / R0 | info: master | every metric | |
| C4 | P1 / R0 | lead: PR1 against the host's code | as C1 | as C1 |
| C5 | P1I / P1 | lead: M-29 | fsync p99, p50, syncq w p99 | read p99, disk read p99, guest MB/s, txg mean |
| C6 | P1A / P1 | lead: age guard | syncq w p99, fsync p99 | as C5 |
| C7 | R0W / R0 | lead: blk-wbt off (its cost) on 2.4.4 | fsync p99, p50, syncq w p99, z_wr_int in blk-wbt | read p99, disk read p99, guest MB/s |
| C8 | P1W / P1 | lead: blk-wbt off on PR1 | as C7 | as C7 |
| C9 | QM / R0 | control: PR2 v2 on, marks the disks ignore | every metric | |
| C10 | QR / QM | lead: NCQ priority for sync reads, ncq_prio 1 | read p99, disk read p99 | fsync p99, syncq w p99, guest MB/s, txg mean |
| C11 | QR2 / QM | lead: NCQ priority for sync reads, ncq_prio 2 | as C10 | as C10 |
| C12 | QR / R0 | lead: PR2 v2 and ncq_prio, against the host's code | as C10 | as C10 |
| C13 | PQR / P1 | lead: NCQ priority for sync reads, on PR1 | as C10 | as C10 |
| C14 | PQ3 / PQR | lead: sync writes high priority too (rt_mask 3 against 1), on PR1 | fsync p99, p50, syncq w p99 | read p99, disk read p99, guest MB/s, txg mean |
| C15 | pr3 / pr3o | lead: PR3's window, on against off (same tree) | read p99, disk read p99 | fsync p99, syncq w p99, guest MB/s, txg mean |
| C16 | pr3o / R0 | info: PR1's 2.4.4 backport with sync_writes=2, window off, against 2.4.4 (no verdict) | every metric | |

**C7 and C8 compare `z_wr_int` asleep in blk-wbt where they compared PSI io some**, because on the host what blk-wbt off removed from PSI was ZFS's issue threads asleep in blk-wbt (owner's A/B of 18/11: PSI io some 5.0 to 6.2 % with blk-wbt on, 2.6 to 4.6 % off), which the rig samples directly, while the rig's PSI sat at 87.4 to 87.9 % in every calibration run. It is 0 by construction with blk-wbt off, so their verdict turns on what moves with it: fsync, the sync queue and the guards. PSI is still reported per arm.

Each task's report gives these over the passes done so far; only the sixth gives the verdicts the plan was made for.

**Expected, from the model (report what you see, not this).** `hddsim priotest` (step 4), reads marked, mean / p99 ms of the reads; "mixed": 4 random 128K reads and 28 random 342K writes outstanding, write through; "leaf": TASK-1121-HRIG's open-loop host-like leaf:

| Point | mixed, ncq_prio 0 | mixed, 1 | leaf, 0 | leaf, 1 | leaf, 2 |
| --- | --- | --- | --- | --- | --- |
| G1 | 277.4 / 790.5 | 65.1 / 438.8 | 32.1 / 333.6 | 32.1 / 333.6 | 32.1 / 333.6 |
| G2 | 252.4 / 730.6 | 48.7 / 309.6 | 28.1 / 326.2 | 28.1 / 326.2 | 28.1 / 326.2 |
| G3 | 277.4 / 790.5 | 65.1 / 438.8 | 34.2 / 339.4 | 34.2 / 339.4 | 19.6 / 111.5 |
| G4 | 252.4 / 730.6 | 48.7 / 309.6 | 28.2 / 312.4 | 28.2 / 312.4 | 15.8 / 98.3 |
| G5 | 277.4 / 790.5 | 65.1 / 438.8 | 41.1 / 414.1 | 15.3 / 33.1 | 15.3 / 33.1 |
| G6 | 252.4 / 730.6 | 48.7 / 309.6 | 37.6 / 414.1 | 12.2 / 26.1 | 12.2 / 26.1 |
| G7 | 277.4 / 790.5 | 65.1 / 438.8 | 44.6 / 432.2 | 44.6 / 432.2 | 15.4 / 34.9 |
| G8 | 277.4 / 790.5 | 65.1 / 438.8 | 40.3 / 410.4 | 40.3 / 410.4 | 15.6 / 37.8 |

With a large write cache (G1, G2) priority changes nothing in the model: a leaf's read tail is set by flushes, which drain the NCQ and hold every later command; with 16 MiB (G3, G4), 2 MiB (G7) or 4 MiB (G8) only `ncq_prio=2` helps; write through (G5, G6) both do. What ZFS's queue, blk-wbt and the guest make of it is what this measures. On the other leads nothing is known on this rig; VQ1's null_blk rig (no sync queue built) showed P1 halving fsync, P1A never firing, P1I cutting fsync p90.

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed.
- The pool lives on `/dev/ublkb70` to `/dev/ublkb74` only; the driver creates them and the pool and removes them. Touch no other pool, disk or ublk device; never `zpool import -a` (the driver imports `hrp` by name from the five disks only); the pool has `cachefile=none`.
- The ZTS test `zpool_import_all_001_pos` once imported VM 105's exported measurement pool `tank`, which may still be on this VM's disks, exported. Nothing in this task imports it; the driver stops (`STOP: a pool this task did not create is imported`) if `zpool list` shows any pool but `hrp`. Such a pool is a stop: report it and leave it as it is.
- `/etc/zfs/zpool.cache`: steps 0, 3 and 10. A non-empty one names a pool the module would import at load: never load the module while one exists.
- **The disks are user processes.** If an `hddsim` server dies while the pool is imported, every I/O of the pool fails and the pool suspends; `zpool` commands then hang. Never kill an `hddsim` process yourself, never run `hddsim del`, and never stop `ublk_drv`, except through `hr-bench.sh teardown`. If `ps -C hddsim` shows fewer than five, or `$L/dev/*.out` has a line after `ready`, or the disk check (below) shows anything but its expected output: capture (below), stop and report; a reboot is then allowed to clear the suspended pool (save the diagnostics first; it does not replace step 10's reboot).
- **The disk check** is the block below, read-only, TASK-1121-HRIG2's; the steps say when to run it. Expected: `5`; five lines `70 E 0 err 0 rej 0` to `74 E 0 err 0 rej 0`; `0`; `0`. Anything else is a stop (capture as for a hang, then report): fewer than five servers, an `E ` line, a non-zero `err` or `rej`, `rej -`, a line after `ready` in a `.out` file, or a kernel I/O error (`error, dev ublkb7N`) since this task's start marker.

```
L=/var/log/task-1121-hra1
ps -C hddsim -o pid= | wc -l
for i in 70 71 72 73 74; do awk -v d=$i '/^E / {e++} /^S / {for (k = 6; k < NF; k++) if ($k == "err" || $k == "rej") v[$k] = $(k + 1)} END {printf "%s E %d err %s rej %s\n", d, e, ("err" in v) ? v["err"] : "-", ("rej" in v) ? v["rej"] : "-"}' $L/dev/$i.stats; done
for i in 70 71 72 73 74; do sed -n '/ready/,$p' $L/dev/$i.out | sed 1d; done | wc -l
sudo -n dmesg | tac | sed '/task-1121-hra1: start/q' | grep -c 'error, dev ublkb'
```
- **The trees** stay in `/var/tmp/hra-trees` from one arms task to the next; only TASK-1121-HRA6 removes them. Never edit a file in them; a tree that fails `hr-trees.sh check` is a stop.
- Every build, setup step, run and chain runs detached (`setsid nohup ... &`) and is polled. A hang is: the trees' build still going after 3 hours; `disks` after 5 minutes; `pool` after 30 minutes; `armcheck` after 25 minutes; a run after 10 minutes, or `driver.log` unchanged for 10 minutes while a chain runs. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|python3|hddsim|kworker'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`), `tail -n 5 $L/dev/*.stats`, `cat $L/dev/*.out` and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- This is a performance measurement: nothing else on this VM from step 7 to the end of step 8 (no build, ztest, ZTS, second load, bpftrace or blktrace).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured run on your own.

## Steps

### 0. State, on Debian 6.12 (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s; swapon --show
lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; ls -l /dev/zfs /dev/ublk-control 2>&1; ls /dev/ublkb* /dev/ublkc* 2>&1; losetup -a; ls -d /var/tmp/task-1121-hra1 /var/log/task-1121-hra1 2>&1
ls -d /var/tmp/hra-trees /var/log/task-1121-hra? 2>&1; ls /var/tmp/hra-trees 2>&1; cut -d' ' -f1-3,6-9 /var/tmp/hra-trees/*.stamp 2>&1
grep -E 'CONFIG_BLK_DEV_UBLK|CONFIG_BLK_WBT=|CONFIG_BLK_WBT_MQ|CONFIG_IO_URING=|CONFIG_PSI=' /boot/config-7.0.14-25-pve
ls -d /usr/src/linux-headers-7.0.14-25-pve /lib/modules/7.0.14-25-pve/kernel/drivers/block/ublk_drv.ko* 2>&1; grep -c "7.0.14-25-pve" /boot/grub/grub.cfg; sudo -n grub-editenv list
command -v python3 git make gcc modinfo; python3 --version; gcc --version | head -1; git --version; ls /usr/include/linux/io_uring.h; pgrep -a -x zed || echo NO-ZED
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|hddsim'
ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl, ublk_drv or null_blk module is loaded, `/dev/zfs` or a `/dev/ublk*` exists; `losetup -a` is not empty; `$T` or `$L` exists; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `/boot/config-7.0.14-25-pve` lacks `CONFIG_BLK_DEV_UBLK=m` or `=y`, or `CONFIG_BLK_WBT` or `CONFIG_PSI`; the 7.0 headers or the 7.0 `ublk_drv` module are missing; no grub entry names 7.0.14-25-pve; `grub-editenv list` shows a `next_entry`; python3 is older than 3.8; a build, test or load runs.
Also stop if `/var/tmp/hra-trees` exists (trees of an earlier series: report its listing), or /var/tmp has less than 25 GiB free (seven built trees take about 10 GiB).
Note, without stopping: swap, zed (do not start or stop it). `/etc/zfs/zpool.cache`: absent is expected; size 0: `L=/var/log/task-1121-hra1; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0` and say so; size above 0: stop and report its `ls -l`, load nothing.

```
L=/var/log/task-1121-hra1; T=/var/tmp/task-1121-hra1; R=/var/tmp/hra-trees
sudo -n mkdir -p $T $L/bin $R && sudo -n chown "$(id -un)" $T $L $L/bin $R
```

### 1. Kit v2.1, fetched by commit from the fork

```
L=/var/log/task-1121-hra1; T=/var/tmp/task-1121-hra1
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs cca50b0943d674176aa10f98fdcfc271ef392bae
test "$(git rev-parse FETCH_HEAD)" = cca50b0943d674176aa10f98fdcfc271ef392bae && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = b50c3f0a74fd73077b596376dd793013cfa71765 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    e60b3d3de307803d9ee842dab806c1878d80a174355b300274728053b0cbc349 arms.txt \
    f79f57a7cec57a602c0c71acad38f1803af1d63ed0a1558c5ca91429142f056b grid/G1.conf \
    eec8a376ff13d234b3c14935f0ec4e25242c4cb4d7ceea5f9509f014daf42256 grid/G2.conf \
    bb601f6e7c4ead34391e2e8d55ffa8a210b250fc298243ce39cd2349bebd537f grid/G3.conf \
    d879b967ac50153f9b52f5ef5cbe6bcc44beb0154c4a173820e35c8084ce0b93 grid/G4.conf \
    0b721c92bb9fc53f4cc257a1a2f2ac591e0c48871c1e06310c31efabd340ad4b grid/G5.conf \
    3ee3cb3731b5323064c03ea239185fa63ad3a157ba2d7a1a185796b39147285b grid/G6.conf \
    538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8 grid/G7.conf \
    fa7ec5a6b920725373c83e78321824294366859058633fa35a892fc213dfb875 grid/G8.conf \
    ba2267bacf0bf60dc531c2d0431633737017553b17a761fbdfd08844a856b251 grid/fast.conf \
    f4f0f861093d80b1ca08a3e52966dd056b12d7ef61f5eec831d1c0c8294381f9 hddsim.c \
    1de5d21aaadc484978ec4b65fdbd4824071a47b79b2e4833ebe959a7d9f08cbc hr-bench.sh \
    b259d0f15f7df96326179564088e6f80433c501eca9723d69770f20e2ce44669 hr-load.py \
    1bef26029e0a3cb455e96cc697cf0e209371242a121a728f2d0e230108bc7fd3 hr-report.py \
    f6df70024895b41f9ddff492ed127e6e2a60d69d0301f6c53f8737535b11aa46 hr-sample.py \
    86c60b1db5b6f5c09f0b816d48925faccdd6f1e6a4317656078662c2eb56011a hr-trees.sh \
    b9e80d514452154be77af2246c5f2ae9343e94f6d3dac68ed8a66fd7e6db1cdc patches/p3w-244/0001-vdev_disk-keep-blk-wbt-off-sync-writes-and-flushes.patch \
    72e52afa7b42f2ba8568111e7ef1a8c048a38e2d522a192043dc31de8c1fb5af patches/p3w-244/0002-vdev_disk-add-zfs_vdev_disk_sync_writes.patch \
    298eb484a36e3570f495f927f8bf0d4811382cd9e5b97664e17562d9f68781a2 patches/p3w-244/0003-vdev_queue-add-zfs_vdev_queue_window-capping-active-.patch \
    5b54875c268b72d913be43074f192c3f4b6ec43a09af2948f3a8dedd8b9297cc patches/p3w-244/0004-vdev_queue-adjust-zfs_vdev_queue_window-to-read-late.patch \
    c41836711d2aac9d4dd734e4101c85df87ded0f920443a49a842124273785ebe patches/p3w-244/0005-vdev_queue-trace-the-window-of-zfs_vdev_queue_window.patch \
    e5a2957ab1c02c544fea28c33af277cba6ba3442e875c0214113685e1f60d0a5 patches/p3w-244/0006-vdev_disk-2.4-test-backport-zfs_vdev_disk_sync_write.patch \
    69915cc83e61254f1ac9e663a71cdeda947a7e1ebc3f9ad21dd2d660bf03d48b patches/pr1-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch \
    f62b946632a92b2dc58c21ce9809c4df5892d5ef290da1046ffef6c0e5a80504 patches/pr1-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch \
    5b00170ec7d4e69b6e226d752a3ad1038a4c7930ecc44e44d1bfce0e229472df patches/r244-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch \
    87a68f49a3ea7e54cbf56cd9583f20d618627cbe3cad5ca0c9c2bd98805df095 patches/r244-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch \
    81d573ba59644ce1a9381cb3f345f5950d73a99c4ade4f37de731327dc45d776 plan.txt > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n hr-bench.sh && bash -n hr-trees.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l hr-bench.sh hr-trees.sh
cp grid/G7.conf $L/best.conf && echo "538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8  $L/best.conf" | sha256sum -c
echo '-' > $L/best.guest && cat $L/best.guest
```

Expected: the three `OK` lines; 27 entries, exactly `100644 arms.txt`, `100644 grid/G1.conf`, `100644 grid/G2.conf`, `100644 grid/G3.conf`, `100644 grid/G4.conf`, `100644 grid/G5.conf`, `100644 grid/G6.conf`, `100644 grid/G7.conf`, `100644 grid/G8.conf`, `100644 grid/fast.conf`, `100644 hddsim.c`, `100755 hr-bench.sh`, `100644 hr-load.py`, `100644 hr-report.py`, `100644 hr-sample.py`, `100755 hr-trees.sh`, `100644 patches/p3w-244/0001-vdev_disk-keep-blk-wbt-off-sync-writes-and-flushes.patch`, `100644 patches/p3w-244/0002-vdev_disk-add-zfs_vdev_disk_sync_writes.patch`, `100644 patches/p3w-244/0003-vdev_queue-add-zfs_vdev_queue_window-capping-active-.patch`, `100644 patches/p3w-244/0004-vdev_queue-adjust-zfs_vdev_queue_window-to-read-late.patch`, `100644 patches/p3w-244/0005-vdev_queue-trace-the-window-of-zfs_vdev_queue_window.patch`, `100644 patches/p3w-244/0006-vdev_disk-2.4-test-backport-zfs_vdev_disk_sync_write.patch`, `100644 patches/pr1-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch`, `100644 patches/pr1-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch`, `100644 patches/r244-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch`, `100644 patches/r244-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch`, `100644 plan.txt`; 27 `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x` twice; `best.conf: OK`; `-` (`hr-bench.sh` reads the first line of `$L/best.guest`, `-` for the guest's defaults or `media_mbps=N` / `media_qd=N`, and stops without the file). The driver is bash: run it with `bash` or directly.

### 2. Trees, on 6.12

```
L=/var/log/task-1121-hra1; R=/var/tmp/hra-trees
cd $L/bin && bash ./hr-trees.sh fetch $R > $L/trees-fetch.txt 2>&1; echo rc=$?; grep -c ' OK$' $L/trees-fetch.txt; grep -v ' OK$' $L/trees-fetch.txt
cd $L/bin && setsid nohup bash -c './hr-trees.sh build '$R' /usr/src/linux-headers-7.0.14-25-pve; echo "build rc=$?"; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Expected: rc=0, 32 `OK` lines and nothing else. Poll `cat $L/build.log` until `build-done` (seven trees, about 15 min each, one after the other with `make -j$(nproc)`). Expected: one line `<name> build rc=0 <s> s, <k> warning lines` per tree, then `build rc=0`. On a failed build, quote `grep -nE 'error:|Error [0-9]' $R/build-<name>.log | head -20` and stop.
Then:

```
L=/var/log/task-1121-hra1; R=/var/tmp/hra-trees
cd $L/bin && bash ./hr-trees.sh check $R > $L/trees-check.txt 2>&1; echo rc=$?; grep -v ' OK$' $L/trees-check.txt
for n in R M P A RQ PQ P3W; do test -d $R/$n && echo "$n: $(modinfo -p $R/$n/module/zfs.ko | grep -oE '^(zfs_vdev_disk_sync_writes|zfs_vdev_queue_age_ms|zfs_vdev_disk_rt_mask|zfs_immediate_write_sz|zfs_vdev_queue_window):' | tr -d : | sort | tr '\n' ' ')"; done
grep -h 'warning:' $R/build-*.log | grep -E 'vdev_disk\.c|vdev_queue\.c|zil\.c|zio\.h' | sort -u | head
```

Expected: rc=0; one line `<name> <name> commit ... tree ... srcversion ... vermagic 7.0.14-25-pve kdir /usr/src/linux-headers-7.0.14-25-pve` per tree, all srcversions different (the program checks); `R: zfs_immediate_write_sz`, `M: zfs_immediate_write_sz`, `P: zfs_immediate_write_sz zfs_vdev_disk_sync_writes`, `A: zfs_immediate_write_sz zfs_vdev_disk_sync_writes zfs_vdev_queue_age_ms`, `RQ: zfs_immediate_write_sz zfs_vdev_disk_rt_mask`, `PQ: zfs_immediate_write_sz zfs_vdev_disk_rt_mask zfs_vdev_disk_sync_writes`, `P3W: zfs_immediate_write_sz zfs_vdev_disk_sync_writes zfs_vdev_queue_window`; quote any warning in those files.

### 3. Boot 7.0.14-25-pve once

As in TASK-1121-HRIG: write `$L/RESUME.md` ("TASK-1121-HRA1: steps 0-2 done, continue at step 3 after the boot"), boot 7.0.14-25-pve once with `grub-reboot` and the menu entry K35 used (Debian stays the default), and resume by your own arrangement. After the boot: `uname -r` is `7.0.14-25-pve`, `sudo -n grub-editenv list` shows no `next_entry`, `cat /proc/cmdline` (same stop list as step 0), `lsmod | grep -E '^(zfs|spl|ublk_drv) '` empty, `ls -l /etc/zfs/zpool.cache` (as step 0), `free -m`, `swapon --show`, `ls -d /sys/kernel/debug/block`. Do not change swap or sysctls.

### 4. Disks and the model, offline (about 2 min, functional)

```
L=/var/log/task-1121-hra1
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh disks > $L/disks.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `disks done` or `STOP`. Then `cat $L/disks.nohup $L/setup-devices.txt $L/hddsim-build.txt; cat $L/dev/7*.out; ps -o pid,stat,ni,rss,comm -C hddsim`, the disk check, and:

```
L=/var/log/task-1121-hra1
cd $L/bin && sudo -n bash ./hr-bench.sh selftest; tail -n 1 $L/driver.log; sed -n '2,6p' $L/selftest.txt; sed 1d $L/priotest.txt
G=$(basename grid/G7.conf .conf); test -r /var/log/task-1122-hrig3/selftest-$G.txt && { sed 1d $L/selftest.txt | diff - <(sed 1d /var/log/task-1122-hrig3/selftest-$G.txt) && echo model as calibrated; }
```

Expected: `hddsim built: 0 warnings`; five lines `hddsim 7N: /dev/ublkb7N ready pid ...`; five `hddsim` processes; `setup-devices.txt` with kernel 7.0.14-25-pve, the ublk_drv vermagic, `best.conf <its sha256> ... ncq_prio=0`, `best.guest -`, R's srcversion from step 2, and for each disk `size_mib 16384 sched ... [none] rot 1 wbt_lat_usec 75000 write_cache write_back ... max_sectors_kb 1024 lbs 4096 iostats 1` and a number after `wbt_debugfs` (if `-`: `sudo -n mount -t debugfs none /sys/kernel/debug`, say so, and rerun nothing). `model as calibrated` (when the point is a grid file and calibration's `selftest-<G>.txt` is on this VM: the model is the calibrated one; a difference is a stop). The priotest lines: the point's row of the table under "Expected" (mean / p99 of the reads); another value is a stop. A disk not as expected: `teardown`, stop and report.

### 5. Probe, pool, functional run, arms check (about 35 min, functional)

```
L=/var/log/task-1121-hra1
cd $L/bin && sudo -n bash ./hr-bench.sh probe; tail -n 1 $L/driver.log; cat $L/probe.txt
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh pool > $L/pool.nohup 2>&1 < /dev/null &
```

Probe, expected as TASK-1121-HRIG step 5 for the point (a finding if off, not a stop, unless 3 ms or more above the model's QD1 mean). Poll `tail -n 2 $L/driver.log` until `pool done` or `STOP` (about 10 to 20 min). Then `cat $L/pool.nohup $L/setup-zpool-list.txt $L/setup-props.txt`, then the disk check. Expected: size about 79.5G, cap 60 to 68 %, frag recorded (20 to 45 % expected, another value is a finding), MemAvailable at least 2500 MiB; `full_pages` per disk below 50000. Then the functional run (arm R0, which switches the rig once: export, reload, import, prewarm):

```
L=/var/log/task-1121-hra1
cd $L/bin && setsid nohup sudo -n env RAMP=10 MEAS=30 bash ./hr-bench.sh point F R0 > $L/point-F.nohup 2>&1 < /dev/null &
```

Poll until `point F R0 done` or `STOP` (about 1.5 min). Then `python3 -I $L/bin/hr-report.py calib $L/raw F > $L/calib-f.md; echo rc=$?; sed -n '/^## Criteria/,/^- F/p' $L/calib-f.md; tail -n 3 $L/raw/F/R0/guest.txt; cat $L/raw/F/R0/guest.err; head -12 $L/raw/F/R0/arm.txt`, then the disk check. **Stop and report** (after `teardown`) if: rc is not 0; the guest's `END` line is missing or there is an `ERROR` line; V1 fails on store errors; `models` is not `1`; the `zpool iostat` streams are empty; `arm.txt`'s `pool` line does not end with `cachefile none` or its srcversion is not R's. Every other FAIL in calib-f.md is a finding (30 s is too short to judge). Then every arm once, unmeasured (each switch, then 256 MiB of library reads and 16 synchronous 64 KiB writes to the journal area, whose marks the disks count):

```
L=/var/log/task-1121-hra1
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh armcheck > $L/armcheck.nohup 2>&1 < /dev/null &
```

Poll until `armcheck done` or `STOP` (about 10 min). Then `cat $L/armcheck.txt`, then the disk check. Expected: one line per arm, `ok`, its srcversion the stamp's of its tree, `5 wbt_lat_usec 75000;` (`0` on R0W and P1W), `5 ncq_prio=K;` with the arm's K, its parameters as in the arms table, `rt_mask` and `sync_writes` as loaded; marked reads above 0 on QM, QR, QR2, PQR and PQ3 and 0 elsewhere; marked writes above 0 on PQ3 only (QM's rt_mask has bit 2, but 2.4.4 submits no write REQ_SYNC|REQ_IDLE, so the marker marks none); pr3 and pr3o mark nothing (P3W has no rt_mask: `rt_mask 0 sync_writes 2`). A `FAIL` stops the driver: `teardown`, report.

### 6. Note to main, then wait for GO

Steps 7 and 8 must run alone on the host, on a quiet VM. End your turn with the interim results (step 0's kernel lines; the trees' check lines and their modinfo lines; the build times; the selftest/priotest lines and `model as calibrated`; `setup-devices.txt`; the probe; `setup-zpool-list.txt`'s last lines and the pool line; calib-f.md's criteria; `armcheck.txt`) and, as the very last line, exactly:

`TASK-1121-HRA1 READY-TO-MEASURE (about 70 min alone on the host, at most 85). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 7 only on a message from main that contains `GO TASK-1121-HRA1`; note the time of GO. On `HOLD`, keep waiting. On `ABORT TASK-1121-HRA1`, go to step 10 without running anything. Nothing runs while you wait (pool imported and idle, disks in the last armcheck arm's state).

### 7. Quiet check (about 1 min)

```
L=/var/log/task-1121-hra1
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n bash ./hr-bench.sh quiet 60; tail -1 $L/driver.log
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so.

### 8. The pass (about 69 min)

```
L=/var/log/task-1121-hra1
cd $L/bin && setsid nohup sudo -n bash -c './hr-bench.sh pass A1 1 '$(( $(cat $L/go.txt) + 5100 ))'; echo chain-done' > $L/chain.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass A1 done` or `STOP`; after each `point A1 <arm> done` (and `point WA1 ...`), the disk check (its expected output, or stop). Then write in your chat the line `TASK-1121-HRA1 MEASURED <UTC time>: the runs are over, the host is free.` and go on without waiting.

### 9. Report data

```
L=/var/log/task-1121-hra1
grep -E 'pass A1|point A1|point WA1|dropped|skipped' $L/driver.log
python3 -I $L/bin/hr-report.py arms $(ls -d /var/log/task-1121-hra1 2>/dev/null) > $L/arms.md; echo rc=$?
sed -n '/^## Runs/,/^## Per arm/p' $L/arms.md | grep -c '^| A1 .* | yes |'; grep '| no |' $L/arms.md; sed -n '/^## Verdicts/,$p' $L/arms.md
```

Expected: rc 0; 15 valid runs of A1 (fewer if the time rule dropped some), every run of the earlier tasks counted as their reports said. A run that is not valid is a finding: quote its line. The verdicts are final only in TASK-1121-HRA6.

### 10. Teardown, back to Debian, end

Run the disk check, then:

```
L=/var/log/task-1121-hra1; cd $L/bin
sudo -n bash ./hr-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|ublk_drv) '; ls /dev/ublk* 2>&1; ps -C hddsim -o pid= | wc -l; tail -n 1 $L/dev/7*.stats
sudo -n dmesg | sed -n '/task-1121-hra1: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|ublk' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw dev armcheck && sudo -n rm -rf /var/tmp/task-1121-hra1
sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: no module loaded, no `/dev/ublk*`, 0 `hddsim` processes, each stats file ending with `I ... hddsim 7N stopped (stop 0 del 0 io_err 0)`, no kernel line but `ublk` lines (quote every line, with the step it happened in). `/etc/zfs/zpool.cache`: size 0: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it.
Keep `/var/log/task-1121-hra1` (raw data included: the next arms tasks read it) and `/var/tmp/hra-trees` (the next arms task reuses it).
Then boot back into the Debian 6.12 kernel by a plain reboot (the default entry), resuming by your own arrangement; then `uname -r`, `sudo -n grub-editenv list` (no `next_entry`), `fstrim -av`, `df -h /`, and send the report.

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, tag, parent, file list, mode or hash that differs, or a `FAIL` from `hr-trees.sh`; a failed build; selftest lines other than calibration's or priotest values other than the expected ones, or hddsim warnings; a disk not as expected after `disks`; step 5's stop list; an `armcheck` FAIL; a driver `STOP:` (teardown, then report); a pool this task did not create; a disk check other than expected (capture as for a hang, report; reboot only to clear a suspended pool); a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING` or hung task in the kernel log (capture, report); an OOM kill; more time on the VM than the header's maximum without the wait for GO (finish the current run, then step 10, and say what did not run). A run that is not valid, an arm dropped by the time rule, or any verdict is a finding, not a stop.

## Report

`/var/log/task-1121-hra1/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from arms.md (recompute nothing by eye):
   a. **The verdicts** (arms.md's "Verdicts", all sixteen lines), with the number of pairs each rests on; in TASK-1121-HRA1 to HRA5 say that they are interim.
   b. **Per arm**, from "Per arm": guest fsync p50, p99 and max, read p99, syncq_wait w p99, disk_wait r p99, PSI, disk busy, txg sync mean, `z_wr_int` in blk-wbt, guest MB/s; next to the host's where arms.md gives it.
   c. **This pass's runs**: each run's line of "Runs" (valid or why not), the warm-up, what the time rule dropped or what was skipped.
   d. **NCQ priority on the disks**: marked reads per second on QM, QR, QR2, PQR, PQ3 and marked writes per second on PQ3 (and 0 elsewhere), with armcheck's lines.
   e. **Emulation health**: per run the lateness; every disk check; `full_pages` after the pool and the last `full` value per disk; the probe against the model.
   f. **Noise**: the quiet check; per run the steal and load1; any run with steal above 1 %; the time of GO.
2. arms.md whole, then calib-f.md's "Criteria" table and armcheck.txt.
3. Setup: kernel (both boots), /proc/cmdline, CPUs, RAM, swap, the kernel config lines, kit checks and file hashes, `best.conf`'s name and sha256, trees-fetch.txt's non-OK lines (none expected), trees-check.txt, the modinfo lines, build times (from build.log), the selftest and priotest lines, setup-devices.txt, the probe, setup-zpool-list.txt, setup-props.txt; per run its start and end time (driver.log); the measured time from GO to the end of step 8, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, devices, processes, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0, 3 and 10, the reboots with their UTC times and how you resumed, then End.
