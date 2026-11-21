TASK-1122-HRIG3 (from the coordinator session "main"): one more calibration pass of the host-like rig, performance, on VM 105 (CHSONVTST02) booted once into 7.0.14-25-pve, before the six passes of the arms series (TASK-1121-HRA1 to HRA6) run on it. TASK-1121-HRIG2 found no device configuration that matches the host; this task measures six points around the closest one (grid point G5: write cache off, data across the whole stroke): the control (TASK-1121-HRIG2's G5 and load, exactly), two lighter loads on G5, and three points with a small write cache (2 or 4 MiB). About 1 h 45 on the VM plus the wait for main's GO, at most 2 h 45 without that wait (at most 70 min before the READY line, 75 measured, 20 after). Its measured part (steps 7 to 9, about 55 min, never more than 75) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never start, check or stop the other VMs yourself. Steps 0 to 6 are functional and may run while other VMs work. One release build (OpenZFS 2.4.4, the host's version, built as TASK-1121-HRIG built it: TASK-1121-HRIG2's step 10 removed that tree), five emulated hard disks served by `hddsim` (a ublk server from the kit), a raidz2 filled like the host's pool, the guest on a 1M zvol, six points then two repeats of the closest one. No arm comparison: this task only says whether the rig reproduces the host, and with which disks and load. Two reboots are pre-approved for this task only (into 7.0 at step 3, back to Debian at step 10); for this task they replace the standing approval of 18/11; a third is allowed only to clear a suspended pool (VM rules). Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1122-hrig3/report.md` and the same text as your final message.

**Kit:** commit `7348c61de867aa1c55ebe05f85b58e20f43140df` (kit-hrig3 published on the fork as the orphan branch `vm-kit-hrig3`, no parent, tree `c2a0ee60e422fb9dcf0def94961414668b84546e`).

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. The final report is sent after the reboot of step 10.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1122-hrig3`, `L=/var/log/task-1122-hrig3`; the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a "surprise" or a FAIL in a report table is a finding (then it is reported, not a stop).

## Why

The owner's host (Dell T630, kernel 7.0.14-29-pve, OpenZFS 2.4.4) has a raidz2 of five SATA hard disks, no log device, scheduler `none`, blk-wbt at 75 ms, holding only the 1M-volblocksize zvol of a Plex VM. Over ten minutes: disks 22 % busy, write await 9 ms, read await 35-38 ms, ZFS sync-write queue wait p50 0 ms and p99 4295 ms, disk read p99 1074 ms, disk write p99 268 ms, txg sync 0.77 s (p95 2.76 s), 0.9 flushes per second per disk at 55 ms, the VM writing 51.3 MB/s. The arms series needs a rig that does the same, so that what it measures on the rig says something about the host.

**What TASK-1121-HRIG2 found** (its report of 21/11 17:45Z): no grid point met the criteria. The closest, G5 (write cache off, whole stroke), median of three runs: `NOT CALIBRATED G5: P1 P3b P5 S(2 of 5)`. Only the two write-through points built the host's tails: sync-queue p99 2147 ms (host 4295, one bucket below), disk write p99 268 ms (host 268), sync writes queued on 12 to 17 % of the leaf-seconds, `z_wr_int` asleep in blk-wbt in 15.5 to 18.4 % of the blocked-task samples (host 7.2). But they also added what the host does not have: a sync-queue p50 of 67 ms (host 0), write await 32 ms (host 9), disks 46 % busy (host 22), and flushes of 5 ms (host 55). The points with a write cache (16 or 256 MiB) had the host's p50 (0 ms) and write await (6 to 8 ms), but sync-queue tails of 134 to 537 ms. The guest fell behind at G5 only: 293, 825 and 1232 media writes skipped in its three runs (49.5, 48.6 and 47.8 MB/s delivered of 50 offered), none at G1 to G4. PSI io some was 87.4 to 87.9 % in every run, whatever the disks did.

**This task changes four things**, all fixed here before any measurement:

1. **The load on G5.** The guest's media writes are a knob (`hr-load.py`: `media_mbps`, the offered rate; its other streams keep their rates; the write size, 512 KiB, and its offset into the zvol's blocks, 256 KiB, are the host's and stay). Beside the control at 50 MB/s, two lighter rates on G5 show how util, write await and the sync queue follow the load toward P1's 30 % ceiling: 32 and 22 MB/s.
2. **A small write cache.** HRIG2 jumped from 16 MiB of write cache to none. In between, the cache absorbs the first writes of each burst (low p50, low write await) and the rest go at media speed (long tails), and a flush has a few MiB to destage. Three points: 2 MiB (G7) and 4 MiB (G8) at the control's load, and 4 MiB at 32 MB/s. The reasons for every level are in the points table below.
3. **PSI io some is no longer a criterion.** It cannot measure the disks on this rig: 87.4 to 87.9 % at every one of HRIG2's eight runs, the four where the guest never fell behind included, and HRIG2's quiet check found 44 % iowait with the pool idle and no load running. The rig's own threads wait as I/O: the guest's threads block in their O_DIRECT calls (HRIG2's reading of its samples), and the five disk servers wait in io_uring with requests outstanding, which kernels of this line count as I/O wait (likely, not checked on 7.0). The host's 8.12 % is a whole-host figure (88 CPUs, three pools, every VM; `qemu.slice` 3.68 %), which no disk-only figure here reproduces either. P5 becomes the disks' own **write await** (diskstats, the mean of the five; host 9 ms, accepted 4 to 20 ms, as the write half of HRIG2's S2): the time each write spends in the disk, the mismatch HRIG2 showed (32 ms). S2 keeps the read await alone. The score and the stability check use the write await where they used PSI. PSI stays in the tables, shown, not judged. Applied to HRIG2's numbers, this rule still picks G5 (score 4.90; G6 5.26, G3 5.80) and still says `NOT CALIBRATED G5: P1 P3b P5`.
4. **The flush time follows the cache, not a new knob.** In `hddsim` (kit v2's, and `fix-flush`'s v1.1) a flush has no time of its own: it waits for the disk's command queue to drain, destages whatever the write cache holds, then costs the 0.1 ms command overhead (`overhead_us`, which every command pays). With the cache off (G5) there is nothing to destage: HRIG2 measured 5 ms in diskstats and 0.10 ms inside the disk. `cache_mb` is the model's only route to the host's 55 ms, and the small-cache points take it: in the model's host-like leaf, flushes average 42 ms with 2 MiB and 54 ms with 4 MiB (16 MiB: 116 ms; HRIG2 measured 97 to 130 ms there). The simulator is not changed: a fixed flush delay on top of write-through would charge the media twice for the same bytes and make S3 hold by construction. On the G5 points S3 is expected to fail, and S4 (sync share of leaf writes, 67 to 70 % at every HRIG2 point against the host's 37 %) everywhere: a G5 point is then accepted only if S1, S2 and S5 all hold.

**P3 and P3b together: what the model says, before the run.** P3 wants the sync-queue p99 in the 2147-ms bucket or above (a p99 above 1074 ms); P3b wants its p50 at or below 1.05 ms. HRIG2 measured both sides of that pair at every point and never both at once. Set against the offline model of one leaf (selftest's "host-like leaf": open loop, without ZFS or blk-wbt; main ran it on a scratch copy with the leaf's write load scaled, nothing to rerun here), HRIG2's rig gave a p50 of 0 wherever the model's was 8.1 to 8.8 ms (all four cached points) and 67 or 34 ms where the model's was 46 or 39 ms (write through); its p99 was 0.5 to 1.1 times the model's at G1, G2 and G4 and 4.4 to 4.7 times at G3, G5 and G6. So the rig's p99 can pass 1074 ms only where the model's passes about 230 ms. In the model that happens only with the cache off and at least about 0.64 of the load (p99 246 ms at 32 MB/s, 459 at 50), where its p50 is 20 to 46 ms. With any cache from 1 to 16 MiB, even at the full load, the model's p99 is at most 191 ms (1 MiB 191, 2 MiB 188, 4 MiB 138, 8 MiB 135, 16 MiB 122), at most about 900 ms on the rig; and with the cache off its p50 falls below 1 ms only at 22 MB/s of the loads tried (0 ms), where its p99 is 107 ms; with a cache it falls to 0.7 ms at 0.52 of the load, where its p99 is at most 64 ms. **By the model, no point of this task, nor any cache size from 0 to 16 MiB at 0.52 to 1.0 of the load, meets P3 and P3b together: NOT CALIBRATED is the expected verdict.** The points are placed where the model is least sure and the rig could still depart from it: 2 and 4 MiB at the full load, between the two regimes HRIG2 measured. A NOT CALIBRATED verdict is then a finding, and its closest point (the lowest score) is what the arms series runs on; a CALIBRATED one means the rig's tails with a small cache are longer than the model's.

**What a lighter point means.** Only the media writes get lighter: reads (7 per second), small writes, database transactions and journal commits keep the host's rates, and so do the ZIL commits (about 0.6 per second); each commit carries fewer bytes. A point chosen at a lower rate reproduces the host's disk regime at that fraction of the host's 51.3 MB/s, the rig's disks then being that much slower than the host's for this load; the arms series would then run at that rate, and its report must say so.

## What changed since TASK-1121-HRIG2

- **The VM.** TASK-1121-HRIG2's step 10 removed `/var/tmp/task-1121-hrig` (the 2.4.4 tree) and rebooted into Debian; TASK-1121-AR3 then ran in the night window and its END rebooted into Debian again; the night's restore may have installed the ZFS udev rules `69-vdev.rules` and `90-zfs.rules` (without their `modprobe zfs` lines) next to `60-zvol.rules`. `/var/log/task-1121-hrig` stays as it is; this task writes nothing there.
- **`/dev/zfs` on 7.0.** TASK-1121-HRIG2 stopped at its step 0 on a `/dev/zfs` node with no module loaded: a static node (character 10, 249) that `kmod-static-nodes` makes at every boot of the 7.0 kernel from its `modules.devname`. Main cleared it on three conditions, which hold for this task too (step 3): no zfs or spl module loaded, nothing holding `/dev/zfs` open, and the srcversion of step 2 at every load.
- **The kit (kit-hrig3).** `hddsim.c`, `hr-load.py`, `hr-sample.py` and `grid/G5.conf` are kit v2's, the files the arms series runs (`hddsim.c` v2 adds NCQ priority, off here since no grid file of this task sets it: with it off the model is v1.1's, and its selftest lines are v1.1's for every grid point; `grid/G5.conf` is byte for byte HRIG2's). `grid/G7.conf` and `grid/G8.conf` are new: G5 with `cache_mb=2` and `cache_mb=4`, nothing else changed. `hr-bench.sh` is kit v1.1's driver (the pool stays imported, nothing is reloaded between points, windows of 60 + 300 s), so that the control repeats HRIG2's procedure, with three changes: a point is a line of the new `points.txt` (its disks' grid file and its guest settings, passed to `hr-load.py` and written to `layout.txt`), `selftest` runs G5, G7 and G8, and the task's paths and kernel-log marker. `hr-report.py` is kit v2's with the rule above: P5, S2, the score and the stability check as said, W1 against the rate the point offered (read from the guest's header) instead of 50, rows for the offered rate, the writes in flight, the media writes skipped and their p99 and maximum latency, a table setting the control against HRIG2's three G5 runs, and a result line that names the point's grid file, its sha256 and its guest settings (its arms part, which this task does not run, is the one prepared for the arms series).

## What you fetch

| What | Source | Commit | Tree |
| --- | --- | --- | --- |
| OpenZFS 2.4.4 | https://github.com/openzfs/zfs tag `zfs-2.4.4` (tag object `f75f3256a0916c5ba090b088e6ea5a9528f6f956`) | `71a9f9578616a90c3c14bb59629fb4d31bfd68d1` | `d2a62ec4527f872ce840ba60f50248e66fcc080b` |
| kit-hrig3 | https://github.com/tigerblue77/zfs branch `vm-kit-hrig3` | `7348c61de867aa1c55ebe05f85b58e20f43140df` (no parent) | `c2a0ee60e422fb9dcf0def94961414668b84546e` |

Build: one, release, `./configure` with the 7.0 headers, on Debian before the boot, as TASK-1121-HRIG's step 2.

## The rig (`hr-bench.sh`)

- **Disks.** Five `hddsim run` servers, `/dev/ublkb70` to `/dev/ublkb74`, 16 GiB each, 4 KiB blocks, rotational, 64 tags, 1 MiB per request; scheduler `none`, `wbt_lat_usec` 75000, iostats on. Data are kept per 4 KiB page, a page that repeats its first 8 bytes in 8 bytes (the pool has compression off and every writer writes such pages). The servers are `mlockall`ed and at `oom_score_adj` -1000. Each writes one line per second to `$L/dev/<id>.stats` (requests and latencies by kind, media busy time, destaged bytes, dirty cache, flush execution time, delivery lateness, pages kept in full, store errors `err` and refused requests `rej`, both since the server started, and `mark`, the requests of the second that arrived marked high priority: none in this task). Parameters come from `grid/<CONF>.conf`, re-read on SIGHUP; `fast` (every request completes at once) is used for building the pool only.
- **The model.** 7200 rpm, seek 0.8 to 16 ms (square root of the distance), 200 to 100 MB/s media rate, NCQ 32, starvation limit 1 s, 100 us per command, SATA link 550 MB/s, data across the whole stroke, destage after 5 ms of idle media or above 75 % of the cache, SATA flushes (a flush waits for the queue to drain, holds every later command, and completes once the cache is on the media), NCQ priority off. The write cache varies: G5 none (`cache_mb=0`: a write completes once on the media), G7 2 MiB, G8 4 MiB.
- **ZFS.** 2.4.4, the host's byte thresholds on this VM's RAM: `zfs_arc_max` 3 GiB, `zfs_dirty_data_max` 1.5 GiB, `zfs_dirty_data_sync_percent` 53, `zfs_vdev_async_write_active_min_dirty_percent` 80, `..._max_dirty_percent` 100, `zfs_delay_min_dirty_percent` 100, `zfs_txg_history` 200; everything else default.
- **Pool.** `hrp`: raidz2 of the five, ashift 12, compression off, `cachefile=none`. Zvol `hrp/vm` (sparse, 12608 MiB, volblocksize 1M) written once whole: library 8 GiB, write ring 4 GiB, db area 256 MiB, journal 64 MiB. Filesystem `hrp/fill` (recordsize 1M) filled with 1 MiB files to 88 % of the pool, then every fourth file removed (then every fourth from the third, as far as needed) until the pool is at 66 % (the host's cap).
- **Guest** (`hr-load.py guest`, the host's Plex VM as QEMU drives the zvol: O_DIRECT, a guest flush is `fdatasync()`). Defaults (the control): 50 MB/s of 512 KiB media writes, 4 in flight, sequential through the ring and starting 256 KiB into a block; the ring ahead of the writer discarded; 6 small writes per second never synced; 0.4 database transactions per second (3 small writes, then `fdatasync()`); a journal commit every 5 s; 7 reads per second of 128 KiB to 1 MiB inside one block of the library, 4 in flight. All open loop: a stream that falls 2 s behind skips ahead and counts what it skipped. A point of `points.txt` changes `media_mbps` only; `guest.txt`'s first line records every setting the guest ran with.
- **One point** (`hr-bench.sh point PASS NAME`): the point's disks sent and read back from every server; blk-wbt's target written again (1 first); guest and sampler started; after `RAMP` (60 s) the counters (`/proc/diskstats`, PSI, `/proc/stat`, the zil, dmu_tx, arcstats and dmu_tx_assign kstats, `zpool iostat -w` and `-r` histograms, the txgs kstat); then `MEAS` (300 s) of `zpool iostat -T u -Hpvlq hrp 1` and `zpool iostat -T u -wpH hrp 1`, every line stamped; the counters again; the kernel log checked. The sampler records every 0.25 s the disks' in-flight requests, blk-wbt's state, disk counters, PSI, CPU and every thread in uninterruptible sleep with whether blk-wbt is on its stack. About 6 min 5 s per point. The pool stays imported for the whole task; nothing is exported or reloaded between points.

## Points and passes

| Point | Disks | Guest media writes | Why this point |
| --- | --- | --- | --- |
| `g5-50mbs` | G5, no cache | 50 MB/s (the defaults) | The control: exactly HRIG2's G5 and load, so that this task is set against HRIG2's three G5 runs (util 44.5 to 46.2 %, sync-queue p50 67 ms, write await 31.8 to 32.8 ms, 293 to 1232 media writes skipped). |
| `g5-32mbs` | G5, no cache | 32 MB/s | The highest rate at which util can reach P1's 30 % ceiling (29.7 % if it falls in proportion to the media writes from 45.96 % at 49.5 MB/s, 33 % if the reads and small writes keep a fixed share), and the lowest at which the model still gives a tail long enough for P3 (p99 246 ms, against about 230 needed). |
| `g5-22mbs` | G5, no cache | 22 MB/s | The deepest cut the workload checks allow by estimate (indirect records per guest write about 1.14 against the 1.1 floor; pool writes per guest write about 2.88 against the 3.0 ceiling) and util about 20 to 26 %, around the host's 22 %: the one load where the model's p50 reaches 0 without a cache. |
| `g7-50mbs` | G7, 2 MiB | 50 MB/s | Nearly the longest tail the model gives with a cache (p99 188 ms; 191 with 1 MiB, whose p50 is worse), flushes 42 ms, write mean 23 ms, at the control's load: the point most likely to keep the 2147-ms tail while the cache takes the first writes of each burst. |
| `g8-50mbs` | G8, 4 MiB | 50 MB/s | The smallest cache whose model p50 (9.6 ms) is close to the 8.1 to 8.8 ms at which HRIG2's rig showed 0 (G1 to G4), with the host's flush time (model 54 ms, host 55) and a write mean of 19 ms (P5 needs at most 20). |
| `g8-32mbs` | G8, 4 MiB | 32 MB/s | 4 MiB at the lighter load: util about 25 to 30 % (model 19.7 % at 0.64 of the load, times HRIG2's 1.3), flushes 44 ms, write mean 15 ms, p50 2.2 ms: every primary but the tail by the model, so likely the lowest score if nothing meets P3. |

| Pass | Points | |
| --- | --- | --- |
| F | `g8-32mbs` then `g7-50mbs`, with `RAMP=10 MEAS=30` | functional check at step 5, before GO (both new grid files and a guest setting exercised), not in the verdicts |
| W | `g5-50mbs` with `RAMP=30 MEAS=60` | warm-up after GO, not in the verdicts |
| G | `g5-50mbs g8-32mbs g7-50mbs g5-22mbs g8-50mbs g5-32mbs`, one run each, in that order (heavy and light, cache and none alternate, so that a drift over the pass lines up with neither) | the grid; then `hr-report.py calib $L/raw G` prints `BEST <point>` |
| C1, C2 | the BEST point, once each | confirmation; time rule before them |

Projected: quiet check 1 min, W 2 min, G 37 min, C1 and C2 13 min: **about 55 min from GO**.

**Time rule** (keeps the measured part under 75 min). After G, from driver.log: d = the mean duration of G's six points (`point G <point> start` to `point G <point> done:`; step 8 gives the command). Run C1 and C2 if now + 2 × d + 2 min is at most GO + 75 min, C1 alone if now + d + 2 min is, otherwise neither (the calibration is then incomplete: say so).

## Written before the run

**Host reference** (DS_HDD_R6_1, blk-wbt on, collector run of 18/11 17:27-17:37Z; percentiles are log2 bucket labels, as `zpool iostat -w` prints them: 1074 means the bucket 537-1074 ms): disk util 22 %; disk_wait read p99 1074 ms; syncq_wait write p99 4295 ms, p50 0; disk_wait write p99 268 ms; await read 35-38 ms, write 9 ms; PSI io some 8.12 % (whole host); txg sync 0.77 s mean, 2.76 s p95; 0.9 flushes per second per disk at 55 ms; 36.9 % of leaf writes sync class; `z_wr_int` in blk-wbt 7.2 % of blocked-task samples; guest 51.3 MB/s, 0.6 ZIL commits/s, 148 indirect records per 104 writes, pool writes 2.36 × the guest's, pool reads 9.8 MB/s.

**Criteria** (`hr-report.py`, constants fixed in the kit; the rows that differ from TASK-1121-HRIG2's are marked "new"):

| Id | Metric | Accepted when |
| --- | --- | --- |
| P1 | disk util (diskstats io_ticks), mean of the five | 15 to 30 % |
| P2 | disk_wait read p99 | 537, 1074 or 2147 ms (the host's bucket or a neighbour) |
| P3 | syncq_wait write p99 | 2147, 4295 or 8590 ms |
| P3b | syncq_wait write p50 | at most 1.05 ms |
| P4 | disk_wait write p99 | 134, 268 or 537 ms |
| P5 (new) | disk write await (diskstats), mean of the five | 4 to 20 ms (host 9; was PSI io some 4 to 12 %) |
| W1 (new) | guest media MB/s over the rate the point offered | at least 0.9 (was over 50 for every point) |
| W2 | ZIL commits/s | 0.45 to 0.8 |
| W3 | indirect records per guest write | 1.1 to 1.8 |
| W4 | pool write MB/s over guest write MB/s | 1.8 to 3.0 |
| W5 | pool read MB/s | 5 to 20 |
| Q1 | leaf-seconds with sync writes queued; most queued on a leaf in a second | at least 5 %; at least 10 |
| V1 | disk completions delivered more than 2 ms late; pages the disks could not keep | at most 1 %; 0 |
| S1 | txg sync mean; p95 | 0.4 to 1.5 s; 1.4 to 5.5 s |
| S2 (new) | disk read await (diskstats) | 18 to 60 ms (the write half moved to P5) |
| S3 | flushes per second per disk; mean flush time | 0.5 to 1.5; 25 to 110 ms |
| S4 | sync share of leaf writes | 25 to 50 % |
| S5 | `z_wr_int` in blk-wbt, % of blocked-task samples | 2 to 20 % |

A run is **accepted** when every P, W, Q and V criterion holds and at least 3 of the 5 S criteria do. W failing means the guest is not the host's load (a rig defect, whatever the disks do); V failing means the emulation itself was late or out of memory (the run says nothing about the host).

**Score** (closeness, lower is better; new): |log2(util / 22)| + |log2(write await / 9)| + the distance in buckets of disk_wait read p99, syncq_wait write p99 and disk_wait write p99 from the host's.

**Selection** (`BEST` line): the accepted point of G with the lowest score; if none is accepted, the lowest score, marked `closest, NOT accepted` (C1 and C2 are still run on it, so that main gets its spread).

**Calibrated** (last line of `calib.md`): the median of each metric over the chosen point's runs (G, C1, C2) is accepted, and the runs agree: the three p99 buckets within one bucket of each other, util and write await (new; was PSI) with a standard deviation at most 25 % of their mean.

**The result line** (the last line of `calib.md`, printed by `hr-report.py`; it is the verdict line of the report and what main puts in the arms series' drafts): `CALIBRATED <point>: BEST_CONF=grid/<G>.conf BEST_SHA256=<its sha256> GUEST=<settings>`, or `NOT CALIBRATED <point>: <criteria that failed>; closest: grid/<G>.conf <its sha256> GUEST=<settings>`. `<G>` is G5, G7 or G8 (sha256 in step 1); `GUEST=-` means hr-load.py's defaults (50 MB/s), otherwise `media_mbps=32` or `media_mbps=22`. If no confirmation ran, there is no such line: the verdict is `NOT CALIBRATED (no confirmation ran)` with calib-g.md's BEST line.

**Control against TASK-1121-HRIG2** (printed by `hr-report.py` under its own heading, a finding either way): `as TASK-1121-HRIG2` when the control's util and write await are within 25 % of HRIG2's G5 median (45.96 %, 32.05 ms) and its three p99 buckets within one bucket of HRIG2's (537, 2147, 268 ms); otherwise `differs from TASK-1121-HRIG2:` and the metrics that moved.

**Noise**: steal above 1 % in a run is listed with its point; load1 counts the run's own threads, listed, not judged.

**Expected, from the model, HRIG2 and estimates (report what you see, not this).** The device model (steps 2 and 4, `selftest`, deterministic), lines 2 to 6 of each file:

| Line | G5 (no cache) | G7 (2 MiB) | G8 (4 MiB) |
| --- | --- | --- | --- |
| `QD1 random 4K read` | n 3000 mean 13.2 p50 13.2 p99 22.0 max 24.0, IOPS 76 | same | same |
| `QD32 random 4K read` | n 6000 mean 221.5 p50 190.1 p99 673.9 max 1018.6, IOPS 144 | same | same |
| `QD1 sequential 1M read` | n 2000 mean 5.3 p50 5.3 p99 5.3 max 5.3, IOPS 187 | same | same |
| `QD4 random 342K write, then nothing` | n 3000 mean 52.2 p50 41.3 p99 168.2 max 278.4, IOPS 77 | n 3000 mean 50.3 p50 49.9 p99 69.2 max 76.1, IOPS 80 | n 3000 mean 43.2 p50 43.0 p99 60.3 max 73.2, IOPS 93 |
| `host-like leaf`: read | n 11827 mean 41.1 p50 15.3 p99 414.1 | n 12103 mean 44.6 p50 15.4 p99 432.2 | n 11835 mean 40.3 p50 15.4 p99 410.4 |
| `host-like leaf`: write; flush | n 68382 mean 26.0; n 1346 mean 21.2 | n 67926 mean 23.0; n 1365 mean 41.9 | n 68496 mean 19.0; n 1395 mean 53.8 |
| `host-like leaf`: sync-write queue wait; util | n 41382 mean 64.4 p50 46.2 p99 458.7; 25.2 % | n 40926 mean 38.3 p50 29.8 p99 188.1; 26.2 % | n 41496 mean 25.0 p50 9.6 p99 138.0; 25.7 % |

The probes through ublk (step 5): the read within about 1 ms of 13.2 on both; sequential 1 MiB writes about 7 ms each (the media rate at mid-disk); `flush after them` below 2 ms with G5 (nothing to destage; HRIG2's G1 probe, 256 MiB of cache: 1982.5 ms) and about 20 to 30 ms with G8 (the 3 to 4 MiB its cache holds, destaged at about 150 MB/s). The pool as HRIG2's: about 79.5G, cap 60 to 68 %, frag 20 to 45 %. The control: near HRIG2's G5 runs (above). The G5 points: util, write await and the sync-queue p50 fall with the rate, and so does the p99 (see "P3 and P3b together"). The cache points: write await, p50 and flush time toward the host's, util about as HRIG2's 16-MiB point at the same load (34 %), tails between HRIG2's 16-MiB and write-through points. P2 (read p99 at 537 ms or above, already at its floor at the control) and S1 (txg p95 at 1.4 s or above) may fail at the light points. A FAIL in the tables, or no point accepted, is a finding, not a stop.

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed (python3, gcc, git, the kernel headers of 7.0.14-25-pve and the build toolchain are all the task needs).
- The pool lives on `/dev/ublkb70` to `/dev/ublkb74` only; the driver creates them and the pool and removes them. Touch no other pool, disk or ublk device; never `zpool import -a`; the pool has `cachefile=none` and is never exported.
- VM 105's measurement pool `tank` (TASK-1121-AR3's, exported) is on this VM's disks and stays as it is. Nothing in this task imports any pool; `hr-bench.sh point` stops (`STOP: a pool this task did not create is imported`) if `zpool list` shows any pool but `hrp`. Such a pool is a stop: report it and leave it as it is (no export, no destroy, no import). The trees under `~/work` are not touched.
- `/etc/zfs/zpool.cache`: steps 0, 3 and 10 (below). A non-empty one names a pool the module would import at load: never load the module while one exists.
- **The disks are user processes.** If an `hddsim` server dies while the pool is imported, every I/O of the pool fails and the pool suspends; `zpool` commands then hang. Never kill an `hddsim` process yourself, never run `hddsim del`, and never stop `ublk_drv`, except through `hr-bench.sh teardown`. If `ps -C hddsim` shows fewer than five, or `$L/dev/*.out` has a line after `ready`, or the disk check (below) shows anything but its expected output: capture (below), stop and report; a reboot is then allowed to clear the suspended pool (save the diagnostics first; it does not replace step 10's reboot).
- Every build, setup step, point and chain runs detached (`setsid nohup ... &`) and is polled. A hang is: the build still going after 60 minutes; `disks` after 5 minutes; `pool` after 30 minutes; a point after 10 minutes, or `driver.log` unchanged for 10 minutes while a chain runs. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|python3|hddsim|kworker'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`), `tail -n 5 $L/dev/*.stats`, `cat $L/dev/*.out` and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- This is a performance measurement: nothing else on this VM from step 7 to the end of step 9 (no build, ztest, ZTS, second load, bpftrace, blktrace, apt or fstrim).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured point on your own.
- **Reboots**: the two of the steps (into 7.0.14-25-pve at step 3, back into Debian at step 10), and a third only to clear a suspended pool, after its diagnostics are saved under `$L`. Before each, a one-shot scheduled task of your own session 5 minutes ahead to resume, as in your earlier tasks; report each reboot with its UTC time.
- **fstrim** after the deletions of step 10, then again after its reboot.
- **The disk check** is the block below, read-only; the steps say when to run it. Expected: `5`; five lines `70 E 0 err 0 rej 0` to `74 E 0 err 0 rej 0`; `0`; `0`. Anything else is a stop (capture as for a hang, then report): fewer than five servers, an `E ` line, a non-zero `err` or `rej`, `rej -` (a server that is not this kit's), a line after `ready` in a `.out` file, or a kernel I/O error (`error, dev ublkb7N`) since this task's start marker.

```
L=/var/log/task-1122-hrig3
ps -C hddsim -o pid= | wc -l
for i in 70 71 72 73 74; do awk -v d=$i '/^E / {e++} /^S / {for (k = 6; k < NF; k++) if ($k == "err" || $k == "rej") v[$k] = $(k + 1)} END {printf "%s E %d err %s rej %s\n", d, e, ("err" in v) ? v["err"] : "-", ("rej" in v) ? v["rej"] : "-"}' $L/dev/$i.stats; done
for i in 70 71 72 73 74; do sed -n '/ready/,$p' $L/dev/$i.out | sed 1d; done | wc -l
sudo -n dmesg | tac | sed '/task-1122-hrig3: start/q' | grep -c 'error, dev ublkb'
```

## Steps

### 0. State, on Debian 6.12 (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s; swapon --show
lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; ls -l /dev/zfs /dev/ublk-control 2>&1; ls /dev/ublkb* /dev/ublkc* 2>&1; losetup -a; ls -d /var/tmp/task-1122-hrig3 /var/log/task-1122-hrig3 2>&1
grep -E 'CONFIG_BLK_DEV_UBLK|CONFIG_BLK_WBT=|CONFIG_BLK_WBT_MQ|CONFIG_IO_URING=|CONFIG_PSI=' /boot/config-7.0.14-25-pve
ls -d /usr/src/linux-headers-7.0.14-25-pve /boot/vmlinuz-7.0.14-25-pve /lib/modules/7.0.14-25-pve/kernel/drivers/block/ublk_drv.ko* 2>&1; grep -c "7.0.14-25-pve" /boot/grub/grub.cfg; sudo -n grub-editenv list
command -v python3 git make gcc; python3 --version; gcc --version | head -1; ls /usr/include/linux/io_uring.h; pgrep -a -x zed || echo NO-ZED
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|hddsim|apt|apt-get|dpkg|unattended-upgr'
ls -l /etc/zfs/zpool.cache 2>&1; ls /etc/udev/rules.d/ | grep -E 'zvol|vdev|zfs'
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl, ublk_drv or null_blk module is loaded, `/dev/zfs` or a `/dev/ublk*` exists; `losetup -a` is not empty; `$T` or `$L` exists; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `/boot/config-7.0.14-25-pve` lacks `CONFIG_BLK_DEV_UBLK=m` or `=y`, or `CONFIG_BLK_WBT` or `CONFIG_PSI`; the 7.0 headers, kernel or `ublk_drv` module are missing; no grub entry names 7.0.14-25-pve; `grub-editenv list` shows a `next_entry`; python3 is older than 3.8; a build, test, load or `hddsim` runs. An `apt`, `apt-get`, `dpkg` or `unattended-upgr` process (the timers unmasked at 10:05Z may run one): wait, polling every 2 minutes, up to 30 minutes, then go on and say so; still running after 30 minutes: stop and report. Note, without stopping: swap, zed (do not start or stop it), the udev rules listed. `/etc/zfs/zpool.cache`: absent is expected; size 0: `L=/var/log/task-1122-hrig3; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0` and say so; size above 0: stop and report its `ls -l`, load nothing.

```
L=/var/log/task-1122-hrig3; T=/var/tmp/task-1122-hrig3
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin && ls -ld $T $L $L/bin
```

### 1. kit-hrig3, fetched from the fork branch

```
L=/var/log/task-1122-hrig3; T=/var/tmp/task-1122-hrig3
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-hrig3
test "$(git rev-parse FETCH_HEAD)" = 7348c61de867aa1c55ebe05f85b58e20f43140df && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = c2a0ee60e422fb9dcf0def94961414668b84546e && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    0b721c92bb9fc53f4cc257a1a2f2ac591e0c48871c1e06310c31efabd340ad4b grid/G5.conf \
    538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8 grid/G7.conf \
    fa7ec5a6b920725373c83e78321824294366859058633fa35a892fc213dfb875 grid/G8.conf \
    ba2267bacf0bf60dc531c2d0431633737017553b17a761fbdfd08844a856b251 grid/fast.conf \
    f4f0f861093d80b1ca08a3e52966dd056b12d7ef61f5eec831d1c0c8294381f9 hddsim.c \
    5a9c79dd46423e9c930a5fe130781d9cd996a985a1e3dd7e36db31d94ef799cf hr-bench.sh \
    b259d0f15f7df96326179564088e6f80433c501eca9723d69770f20e2ce44669 hr-load.py \
    1bef26029e0a3cb455e96cc697cf0e209371242a121a728f2d0e230108bc7fd3 hr-report.py \
    f6df70024895b41f9ddff492ed127e6e2a60d69d0301f6c53f8737535b11aa46 hr-sample.py \
    2682cda26347c733b4b5d9969909416d06ab4ea3d723b33640e9e052fbb90493 points.txt > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n hr-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l hr-bench.sh; grep -v '^#' points.txt; diff grid/G5.conf grid/G7.conf; diff grid/G5.conf grid/G8.conf
```

Expected: the three `OK` lines; ten entries, exactly `100644 grid/G5.conf`, `100644 grid/G7.conf`, `100644 grid/G8.conf`, `100644 grid/fast.conf`, `100644 hddsim.c`, `100755 hr-bench.sh`, `100644 hr-load.py`, `100644 hr-report.py`, `100644 hr-sample.py`, `100644 points.txt`; ten `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`; the six points of the points table with their grid file and guest setting (`-` for the defaults); each `diff` showing only the comment line naming the point and `cache_mb=0` against `cache_mb=2` (G7) or `cache_mb=4` (G8). The driver is bash: run it with `bash` or directly.

### 2. Tree and release build for 7.0, on 6.12 (about 5 to 15 min)

```
T=/var/tmp/task-1122-hrig3; L=/var/log/task-1122-hrig3
git init -q $T/repo && cd $T/repo && git fetch -q --no-tags --depth 1 https://github.com/openzfs/zfs refs/tags/zfs-2.4.4
test "$(git rev-parse FETCH_HEAD)" = f75f3256a0916c5ba090b088e6ea5a9528f6f956 && echo tag OK
test "$(git rev-parse 'FETCH_HEAD^{commit}')" = 71a9f9578616a90c3c14bb59629fb4d31bfd68d1 && echo R commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = d2a62ec4527f872ce840ba60f50248e66fcc080b && echo R tree OK
git worktree add -q --detach $T/R 71a9f9578616a90c3c14bb59629fb4d31bfd68d1
printf '%s  %s\n' \
    aff082bff393836fd3d415cf6441fee1c034e1f8378a94a4a1a573d21ce64ad6 $T/R/module/zfs/zil.c \
    1cdfb808e2278ba283ca203c158779072b8003e6869978cee757b42fd09ec09a $T/R/module/zfs/vdev_queue.c \
    25f598a1675a2d6af06d3226d8c23e0b689090363e001504ed92b716893638a6 $T/R/module/os/linux/zfs/vdev_disk.c \
    ab365b560991d24b30ddbe3c54432d4ad6f2014ef619020b8dc6cf513044b3f5 $T/R/module/zfs/zvol.c > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
cd $T/R && setsid nohup sh -c 't=$(date +%s); ./autogen.sh > '$L'/autogen-R.log 2>&1 && ./configure --with-linux=/usr/src/linux-headers-7.0.14-25-pve --with-linux-obj=/usr/src/linux-headers-7.0.14-25-pve > '$L'/configure-R.log 2>&1 && nice -n 10 make -j8 > '$L'/build-R.log 2>&1; echo "R rc=$? $(( $(date +%s) - t )) s"; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Three `OK` lines and four `OK` from `sha256sum -c`. Poll `cat $L/build.log` until `build-done`. Then:

```
T=/var/tmp/task-1122-hrig3; L=/var/log/task-1122-hrig3
cat $L/build.log; echo "R $(modinfo -F vermagic $T/R/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/R/module/zfs.ko)"
grep -h 'warning:' $L/build-R.log | sort -u | head -20
gcc -O2 -Wall -pthread -o $T/hddsim-st $L/bin/hddsim.c -lm 2>&1; for g in G5 G7 G8; do $T/hddsim-st selftest $L/bin/grid/$g.conf 1800 > $L/selftest-step2-$g.txt; echo "== $g rc=$?"; sed -n '2,6p' $L/selftest-step2-$g.txt; done
```

Expected: `R rc=0`; vermagic `7.0.14-25-pve`; srcversion `DDF4C2CCA20EC708474D57F` (TASK-1121-HRIG's build of the same tag against the same headers; another value is a finding, not a stop: say so). The srcversion printed is the one every load must show. Quote any warning. No compiler output, three `rc=0`, and for each grid file the five selftest lines of the table under "Expected". Another value, or a compiler warning from hddsim.c: stop and report. On a failed build, quote `grep -nE 'error:|Error [0-9]' $L/build-R.log | head -20` and stop.

### 3. Boot 7.0.14-25-pve once

As TASK-1121-HRIG's step 3 and TASK-1121-AR3's step 4: write `$L/RESUME.md` ("TASK-1122-HRIG3: steps 0-2 done, continue with step 3's checks after the boot"), create a one-shot scheduled task of your own session 5 minutes ahead to resume, set the one-time boot with `sudo -n grub-reboot` and the 7.0.14-25-pve menu entry those tasks used (Debian stays the default), check that `sudo -n grub-editenv list` shows a `next_entry` naming 7.0.14-25-pve, `sync -f` your logs, and reboot as in those tasks. After the boot, delete the scheduled task if it has not fired, then:

```
L=/var/log/task-1122-hrig3
date -u; uname -r; uptime -s; cat /proc/cmdline; sudo -n grub-editenv list; lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; free -m; swapon --show
ls -l /dev/zfs 2>&1; sudo -n find /proc/[0-9]*/fd -lname /dev/zfs 2>/dev/null | wc -l; ls /dev/ublk* 2>&1; ls -l /etc/zfs/zpool.cache 2>&1; pgrep -a -x zed || echo NO-ZED
```

Expected: `7.0.14-25-pve`; no `next_entry`; the same /proc/cmdline stop list as step 0; no module; `/dev/zfs` absent, or the static node `crw... 10, 249` (with no module loaded: expected, as in "What changed"); `0` from `find` (nothing holds `/dev/zfs` open); no `/dev/ublk*`; `/etc/zfs/zpool.cache` as step 0 (size 0: move it to `$L/zpool.cache.step3` and say so). Stop and report if the VM did not come up on 7.0.14-25-pve (do not try again), a `next_entry` is left, a module is loaded, `/dev/zfs` is another node or something holds it open, or a cache file above 0 bytes exists. Do not change swap or sysctls.

### 4. Disks and the model, offline (about 2 min, functional)

```
L=/var/log/task-1122-hrig3
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh disks /var/tmp/task-1122-hrig3/R > $L/disks.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `disks done` or `STOP`. Then `cat $L/disks.nohup $L/setup-devices.txt $L/hddsim-build.txt; cat $L/dev/7*.out; ps -o pid,stat,ni,rss,comm -C hddsim`, and:

```
L=/var/log/task-1122-hrig3
cd $L/bin && sudo -n bash ./hr-bench.sh selftest; tail -n 1 $L/driver.log; for g in G5 G7 G8; do sed 1d $L/selftest-$g.txt | diff - <(sed 1d $L/selftest-step2-$g.txt) && echo "$g model as step 2"; done
```

Expected: `hddsim built: 0 warnings`; five lines `hddsim 7N: /dev/ublkb7N ready pid ...`; five `hddsim` processes; `setup-devices.txt` with kernel 7.0.14-25-pve, the ublk_drv vermagic, `hddsim.c f4f0f861093d80b1ca08a3e52966dd056b12d7ef61f5eec831d1c0c8294381f9`, `points.txt 2682cda26347c733b4b5d9969909416d06ab4ea3d723b33640e9e052fbb90493`, the zfs srcversion of step 2, and for each disk `size_mib 16384 sched ... [none] rot 1 wbt_lat_usec 75000 write_cache write_back ... max_sectors_kb 1024 lbs 4096 iostats 1` and a number after `wbt_debugfs` (if `-`: `sudo -n mount -t debugfs none /sys/kernel/debug`, say so, and rerun nothing); `selftest done`; three `model as step 2` lines. A disk not as expected, or a selftest that differs: `teardown`, stop and report. Then the disk check (VM rules).

### 5. Probes, pool, functional points (about 25 min, functional)

```
L=/var/log/task-1122-hrig3
cd $L/bin && sudo -n bash ./hr-bench.sh probe G5 && sudo -n bash ./hr-bench.sh probe G8; tail -n 2 $L/driver.log; cat $L/probe-G5.txt $L/probe-G8.txt
```

Expected (other VMs may be running: a finding if off, not a stop, unless a read is 3 ms or more above the model): `QD1 random 4K read: ... mean` within 1 ms of 13.2 in both; `QD1 sequential 1M write` about 7 ms in both; `flush after them` below 2 ms with G5 and about 20 to 30 ms with G8 (the flush time of "Why" in two lines: nothing to destage with the cache off, the few MiB of the cache with it on). On `STOP: probe`, run the disk check, then `teardown`, then report with both probe files. Then the disk check, and the pool:

```
L=/var/log/task-1122-hrig3
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh pool > $L/pool.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pool done` or `STOP` (about 10 to 20 min). Then `cat $L/pool.nohup $L/setup-zpool-list.txt $L/setup-props.txt` and `grep -c . $L/dev/70.stats`, then the disk check. Expected: `pool done:` with size about 79.5G, cap 60 to 68 %, frag recorded (the host's is 32 %; 20 to 45 % expected, another value is a finding), MemAvailable at least 2500 MiB; `full_pages` per disk below 50000 (200 MiB). Then the two functional points, one chain:

```
L=/var/log/task-1122-hrig3
cd $L/bin && setsid nohup sudo -n env RAMP=10 MEAS=30 bash -c './hr-bench.sh point F g8-32mbs && ./hr-bench.sh point F g7-50mbs; echo chain-done' > $L/point-F.nohup 2>&1 < /dev/null &
```

Poll until `point F g7-50mbs done` or `STOP` (about 2 min). Then:

```
L=/var/log/task-1122-hrig3
python3 -I $L/bin/hr-report.py calib $L/raw F > $L/calib-f.md; echo rc=$?; cat $L/calib-f.md; grep 'point F' $L/driver.log
for p in g8-32mbs g7-50mbs; do echo "== $p"; grep -o 'media_mbps=[0-9.]* media_qd=[0-9]*' $L/raw/F/$p/guest.txt | head -1; grep -E '^(hddsim|disks|guest) ' $L/raw/F/$p/layout.txt | sed 's/^hddsim .*cache_mb=\([0-9.]*\) .*/hddsim cache_mb=\1/'; tail -n 1 $L/raw/F/$p/guest.txt; cat $L/raw/F/$p/guest.err; done
```

Then the disk check. Expected: `rc=0`; the two `done:` lines ending `of 32 offered, W skipped ...` and `of 50.0 offered, W skipped ...`; for `g8-32mbs`: `media_mbps=32 media_qd=4`, `hddsim cache_mb=4`, `disks G8 grid/G8.conf fa7ec5a6b920725373c83e78321824294366859058633fa35a892fc213dfb875`, `guest media_mbps=32`; for `g7-50mbs`: `media_mbps=50.0 media_qd=4`, `hddsim cache_mb=2`, `disks G7 grid/G7.conf 538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8`, `guest -`; each guest's `END` line; empty `guest.err`. **Stop and report** (after `teardown`) if: rc is not 0; a guest's `END` line is missing or there is an `ERROR` line; a guest's settings or a point's disks are not the ones above; V1 fails on pages kept (store errors); `models` is not `1`; the `zpool iostat` streams are empty. Every other FAIL in calib-f.md is a finding (30 s is too short to judge): quote the table.

### 6. Note to main, then wait for GO

Steps 7 to 9 must run alone on the host, on a quiet VM. End your turn with the interim results (step 0's kernel lines, the apt wait if any, and the udev rules; step 1's checks; step 2's build line, vermagic, srcversion and warnings; the selftest lines; step 3's boot time and checks, `/dev/zfs` included; `setup-devices.txt`; the disk check after `disks`; the two probes; `setup-zpool-list.txt`'s last lines and the pool line; calib-f.md's two tables and the two points' settings; the disk check after F) and, as the very last line, exactly:

`TASK-1122-HRIG3 READY-TO-MEASURE (about 55 min alone on the host, at most 75). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 7 only on a message from main that contains `GO TASK-1122-HRIG3`; note the time of GO. On `HOLD`, keep waiting. On `ABORT TASK-1122-HRIG3`, go to step 10 without running anything. Nothing runs while you wait (pool imported and idle, disks in G7 from the last functional point).

### 7. Quiet check and warm-up (about 3 min)

```
L=/var/log/task-1122-hrig3
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n bash ./hr-bench.sh quiet 60; tail -1 $L/driver.log
cd $L/bin && setsid nohup sudo -n env RAMP=30 MEAS=60 bash ./hr-bench.sh point W g5-50mbs > $L/point-W.nohup 2>&1 < /dev/null &
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so (iowait around 44 % with nothing running is what HRIG2 saw: report it, it is not noise from elsewhere). Poll until `point W g5-50mbs done` or `STOP`; then `grep -h '^srcversion' $L/raw/W/g5-50mbs/layout.txt; grep -o 'wbt_lat_usec [0-9]*' $L/raw/W/g5-50mbs/layout.txt | sort | uniq -c; grep -E '^(disks|guest) ' $L/raw/W/g5-50mbs/layout.txt`: the step 2 srcversion; five `wbt_lat_usec 75000`; the G5 `disks` line and `guest -`; then the disk check.

### 8. The grid (about 37 min)

```
L=/var/log/task-1122-hrig3
cd $L/bin && setsid nohup sudo -n bash -c 'for c in g5-50mbs g8-32mbs g7-50mbs g5-22mbs g8-50mbs g5-32mbs; do ./hr-bench.sh point G $c || exit 1; done; echo chain-done' > $L/chain-g.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `point G g5-32mbs done` or `STOP`; after each `point G <point> done`, the disk check (its expected output, or stop). Then:

```
L=/var/log/task-1122-hrig3
python3 -I $L/bin/hr-report.py calib $L/raw G > $L/calib-g.md; echo rc=$?; tail -n 1 $L/calib-g.md
awk -v go="$(cat $L/go.txt)" -v now="$(date +%s)" '
function sec(t, a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
$2 == "point" && $3 == "G" && $5 == "start" { s[$4] = sec($1) }
$2 == "point" && $3 == "G" && $5 == "done:" { e = sec($1) - s[$4]; if (e < 0) e += 86400; d += e; n++ }
END { d = d / n; k = now + 2 * d + 120 <= go + 4500 ? 2 : now + d + 120 <= go + 4500 ? 1 : 0
    printf "d %.0f s over %d points, now %d, GO+75 %d, k %d: run %s\n", d, n, now, go + 4500, k, k == 2 ? "C1 C2" : k == 1 ? "C1" : "none" }' $L/driver.log | tee $L/time-rule.txt
```

Expected `over 6 points`. The last line of calib-g.md is `BEST <point> ...`.

### 9. Confirmation (about 13 min)

With `B` the point named on the BEST line (one of the six), run what the time rule leaves, as one detached chain:

```
L=/var/log/task-1122-hrig3; B=<point>
cd $L/bin && setsid nohup sudo -n bash -c './hr-bench.sh point C1 '$B' && ./hr-bench.sh point C2 '$B'; echo chain-done' > $L/chain-c.nohup 2>&1 < /dev/null &
```

(with k = 1, only `./hr-bench.sh point C1 $B`). Poll until the last `done` or `STOP`; run the disk check, then write in your chat the line `TASK-1122-HRIG3 MEASURED <UTC time>: the runs are over, the host is free.` and go on without waiting:

```
L=/var/log/task-1122-hrig3
python3 -I $L/bin/hr-report.py calib $L/raw G C1 C2 > $L/calib.md; echo rc=$?; tail -n 1 $L/calib.md
```

(with k = 1, `G C1`; with k = 0, skip it and say so: the verdict is then `NOT CALIBRATED (no confirmation ran)` with calib-g.md's BEST line).

### 10. Teardown, back to Debian, end

Run the disk check, then:

```
L=/var/log/task-1122-hrig3; cd $L/bin
sudo -n bash ./hr-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|ublk_drv) '; ls /dev/ublk* 2>&1; ps -C hddsim -o pid= | wc -l; tail -n 1 $L/dev/7*.stats
sudo -n dmesg | sed -n '/task-1122-hrig3: start/,$p' > $L/kmsg.txt; grep -c 'task-1122-hrig3: start' $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|ublk|error, dev' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw dev && sudo -n rm -rf /var/tmp/task-1122-hrig3 && sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: `teardown done: modules , devices`; no module loaded, no `/dev/ublk*`, 0 `hddsim` processes, each stats file ending with `I ... hddsim 7N stopped (stop 0 del 0 io_err 0)`; `1` start marker in `kmsg.txt` (this boot's only); no kernel line but `ublk` lines (quote every line, with the step it happened in). `/etc/zfs/zpool.cache`: size 0: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it. Keep `/var/log/task-1122-hrig3` (raw data and the selftest files included: the arms series compares its model with them). Then boot back into the Debian 6.12 kernel by a plain reboot (the default entry), resuming by your own arrangement; then `uname -r`, `sudo -n grub-editenv list` (no `next_entry`), `lsmod | grep -E '^(zfs|spl) '` (nothing), `sudo -n fstrim -av`, `df -h /`, and send the report.

## Stop conditions (all in one place)

Step 0's list (an apt or dpkg process still running after 30 minutes included); `mkdir` of `$T` or `$L` failing; a fetch failing twice; any commit, tree, tag, parent, file list, mode or hash that differs; a failed build; selftest lines other than the expected ones, or hddsim warnings; after the boot, a kernel other than 7.0.14-25-pve, a `next_entry` left, a module loaded, another `/dev/zfs` node or one held open, a cache file above 0 bytes; a disk not as expected after `disks`, or a selftest that differs from step 2's; the disk check giving anything but its expected output, wherever it runs: fewer than five `hddsim` processes, an `E ` line, a non-zero `err` or `rej`, `rej -`, a line after `ready` in a `.out` file, a kernel I/O error on a `ublkb7N` (capture as for a hang, report; reboot only to clear a suspended pool); a `STOP: probe` (the disk check, then `teardown`, then report); step 5's stop list; a check after W that fails; a driver `STOP:` (teardown, then report); a pool this task did not create; a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING` or hung task in the kernel log after this task's start marker (capture, report); an OOM kill; more than 2 h 45 on the VM without the wait for GO (finish the current point, then step 10, and say what did not run). A FAIL in a criteria table, no accepted point, `NOT CALIBRATED` (the expected verdict, see "Why"), `control: differs`, another srcversion than `DDF4C2CCA20EC708474D57F`, or a confirmation dropped by the time rule is a finding, not a stop.

## Report

`/var/log/task-1122-hrig3/report.md`, and the same text as your final message. Its first line is the verdict line, exactly: `TASK-1122-HRIG3 VERDICT: ` followed by the last line of calib.md (or, with no confirmation, `NOT CALIBRATED (no confirmation ran): ` followed by the last line of calib-g.md). Then:

1. **Answers first**, each with the figures it rests on, quoted from calib-g.md and calib.md (recompute nothing by eye):
   a. **Calibrated or not**: the verdict line again, the chosen point, its grid file and guest settings, and which criteria failed.
   b. **The host's numbers against the chosen point's median**: P1 to P5, then S1 to S5, from calib.md's "Chosen point" tables.
   c. **The six points**: per point, the score and the criteria that failed (calib-g.md's "Criteria" table); then how util, write await, read await, the sync-queue p50 and p99, read p99, write p99, txg sync mean and p95 and the flush time moved with the rate on G5 (50, 32, 22 MB/s), with the cache at 50 MB/s (none, 2, 4 MiB), and with the cache at 32 MB/s (none, 4 MiB).
   d. **P3 and P3b together**: per point, the sync-queue p50 and p99 side by side, and whether any point held both (the model said none would).
   e. **The generator**: per point, the rate offered and delivered, the media writes skipped, and the media write p99 and maximum; at which points the skips stopped.
   f. **The control against TASK-1121-HRIG2**: calib-g.md's "Control" table and its last line.
   g. **Does the rig build a sync queue** (Q1) and **does blk-wbt act** (S5, "blk-wbt background limit < 4"), per point.
   h. **The guest's view** per point: fsync p50, p99 and max, db transaction p99, read p99, and the `END` line.
   i. **Emulation health**: V1 per run, the `disk requests refused` row per run (0: anything else was a stop), `full_pages` after the pool and the last `full` value per disk, the probes against the model (their `flush after them` lines included), and `media busy %` against `disk util %`.
   j. **Noise**: the quiet check; per run the steal and load1; any run with steal above 1 %; PSI per run (shown, not judged); the time of GO and what the time rule dropped.
   k. **For the arms series' drafts**: from the verdict line, `BEST_CONF`, `BEST_SHA256` and `GUEST` (or, if not calibrated, the closest point's), copied as printed.
2. calib.md whole (or calib-g.md if no confirmation ran), then calib-f.md's "Criteria" table.
3. Setup: kernel (`uptime -s` of both boots), /proc/cmdline, CPUs, RAM, swap, the 7.0 kernel config lines, the udev rules, kit-hrig3's checks and file hashes, step 2's lines (build, vermagic, srcversion, warnings), code checks, the selftest lines, step 3's `/dev/zfs` lines, setup-devices.txt, the disk checks, the probes, setup-zpool-list.txt, setup-props.txt; per point its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 9, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, devices, processes, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0, 3 and 10, the two reboots with their UTC times and how you resumed, then End.
