TASK-1121-HRIG2 (from the coordinator session "main"): the resume of TASK-1121-HRIG, calibration of a host-like rig, performance, on VM 105 (CHSONVTST02), which is still booted on 7.0.14-25-pve from TASK-1121-HRIG. TASK-1121-HRIG stopped at step 5 (see "What changed since TASK-1121-HRIG"); this task fetches kit v1.1, which fixes the cause, checks the OpenZFS 2.4.4 build TASK-1121-HRIG left instead of building again, and runs TASK-1121-HRIG's steps 4 to 10: about 1 h 45 on the VM plus the wait for main's GO, at most 2 h 30 without that wait. Its measured part (steps 7 to 9, about 55 min, never more than 75) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 6 are functional and may run while other VMs work. Five emulated hard disks served by `hddsim` (a ublk server from the kit), a raidz2 filled like the host's pool, a guest-like load on a 1M zvol, six device configurations then two repeats of the closest one. No arm comparison: this task only says whether the rig reproduces the host. Two reboots are pre-approved for this task only, in place of TASK-1121-HRIG's (for this task that replaces the standing approval of 18/11): none is needed before GO; one goes back to Debian at step 10; the other is only for clearing a suspended pool (VM rules). Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1121-hrig/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. The final report is sent after the reboot of step 10.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1121-hrig`, `L=/var/log/task-1121-hrig`; the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a "surprise" or a FAIL in a report table is a finding (then it is reported, not a stop).

## Why

The owner's host (Dell T630, kernel 7.0.14-29-pve, OpenZFS 2.4.4) has a raidz2 of five SATA hard disks, no log device, scheduler `none`, blk-wbt at 75 ms, holding only the 1M-volblocksize zvol of a Plex VM. Over ten minutes: PSI io some 8.1 %, disks 22 % busy, ZFS sync-write queue wait p50 0 ms and p99 4295 ms, disk read p99 1074 ms, disk write p99 268 ms, txg sync 0.77 s (p95 2.76 s), 0.9 flushes per second per disk at 55 ms, `z_wr_int` threads asleep in blk-wbt in 7 % of the blocked-task samples. Three earlier rigs could not reproduce that: null_blk serves each request in a fixed time and, with 32 tags, serves 32 at once, so no queue builds (VQ1: "the rig never builds a sync-write queue", writer 11 MB/s against 51); flushes cost one request time; reads never wait behind a write cache. Without a rig that builds the host's sync queue, PR1 (#19981), the M-29 indirect rule and the PR3 window cannot be measured where they matter.

This rig replaces null_blk with `hddsim`, a ublk server in the kit that emulates a 7200 rpm SATA disk: one actuator, seek time growing with distance, rotational latency, a zoned media rate, NCQ with shortest-positioning-first reordering, a volatile write cache destaged in idle time, and SATA flushes (a flush waits for the queue to drain, holds every later command, and completes once the cache is on the media). ublk devices are request-based blk-mq devices, so blk-wbt, scheduler `none` and the disk statistics act on them as on the host's disks, and ZFS's own threads submit to them. The kernel is the host's line (7.0.14-25-pve, on this VM since TASK-1116-KT). Before any arm comparison the rig must match the host on fixed criteria (below); this task measures six device configurations, picks the closest by a fixed rule, and repeats it twice.

## What changed since TASK-1121-HRIG

TASK-1121-HRIG reached step 5 on 7.0.14-25-pve: the five disks came up as expected, then `hr-bench.sh probe G1` stopped because the probe's closing `fdatasync()` on `/dev/ublkb70` returned EINVAL, with the kernel line `invalid error, dev ublkb70, sector 0 op 0x1:(WRITE) flags 0x800 phys_seg 0 prio class 2`. The disks were torn down; `/var/tmp/task-1121-hrig` (the 2.4.4 tree built for 7.0, srcversion `DDF4C2CCA20EC708474D57F`) and `/var/log/task-1121-hrig` were kept.

The cause was in `hddsim`, not in the kernel. The block layer issues a flush with the sector of a freshly initialised request, `(sector_t)-1` (`blk_kick_flush()` calls `blk_rq_init()`); ublk passes it on as the descriptor's `start_sector`; hddsim's range check answered the flush `-EINVAL`, which ublk turns into `BLK_STS_INVAL` for the flush and for the write that waited on it. Every flush, ZFS's included, would have failed the same way, on any kernel. Kit v1.1 changes two files and nothing else:

- `hddsim.c`: a flush has no range, so it is no longer checked against the device. Each statistics line carries `rej N` after `err N`: the requests the server answered with an error since it started. **A non-zero `rej` is a stop, like an `E` line** (VM rules). The model is unchanged: the selftest lines are the ones TASK-1121-HRIG expected.
- `hr-report.py`: reads `rej` and adds the row `disk requests refused, since start` to its tables; it skips a statistics field it does not know (v1.0's parser gives wrong disk figures on a line that has `rej`). The criteria, the score and the selection are unchanged.

`hr-bench.sh`, `hr-load.py`, `hr-sample.py` and `grid/` are byte for byte TASK-1121-HRIG's.

## What you fetch

| What | Source | Commit | Tree |
| --- | --- | --- | --- |
| OpenZFS 2.4.4 | already in `/var/tmp/task-1121-hrig/R`, built by TASK-1121-HRIG from https://github.com/openzfs/zfs tag `zfs-2.4.4` (tag object `f75f3256a0916c5ba090b088e6ea5a9528f6f956`) | `71a9f9578616a90c3c14bb59629fb4d31bfd68d1` | `d2a62ec4527f872ce840ba60f50248e66fcc080b` |
| kit v1.1 | https://github.com/tigerblue77/zfs branch `vm-kit-hrig2` | `9b1c7beb9f8917c1f0f27d2593f9f8d72991f59f` (no parent) | `31c7f64b9b704ffc1f0066589f0288bf2ada8bd2` |

Builds: none if step 2 finds the kept tree as TASK-1121-HRIG built it (release, `./configure` with the 7.0 headers); otherwise that build again, as step 2 says.

## The rig (`hr-bench.sh`)

- **Disks.** Five `hddsim run` servers, `/dev/ublkb70` to `/dev/ublkb74`, 16 GiB each, 4 KiB blocks, rotational, volatile write cache, 64 tags, 1 MiB per request; scheduler `none`, `wbt_lat_usec` 75000, iostats on. Data are kept per 4 KiB page, a page that repeats its first 8 bytes in 8 bytes: the pool has compression off and every writer writes such pages, so five 16 GiB disks hold in a few hundred MiB. The servers are `mlockall`ed and at `oom_score_adj` -1000. Each writes one line per second to `$L/dev/<id>.stats` (requests and latencies by kind, media busy time, destaged bytes, dirty cache, flush execution time, delivery lateness, pages kept in full, store errors `err` and refused requests `rej`, both counted since the server started). Parameters come from `grid/<CONF>.conf`, re-read on SIGHUP; `fast` (every request completes at once) is used for building the pool only.
- **Model knobs.** Fixed: 7200 rpm, seek 0.8 to 16 ms (square root of the distance), 200 to 100 MB/s media rate, NCQ 32, starvation limit 1 s, 100 us per command, SATA link 550 MB/s, destage after 5 ms of idle media or above 75 % of the cache, SATA flushes. Varied: the write cache (`cache_mb` 256, 16 or 0) and the part of the stroke the data span (`stroke` 1.0 or 0.4).
- **ZFS.** 2.4.4, the host's byte thresholds on this VM's RAM: `zfs_arc_max` 3 GiB, `zfs_dirty_data_max` 1.5 GiB, `zfs_dirty_data_sync_percent` 53 (txg forced at about 820 MiB, the host's 20 % of 4 GiB), `zfs_vdev_async_write_active_min_dirty_percent` 80 (1.2 GiB, the host's 30 %), `..._max_dirty_percent` 100, `zfs_delay_min_dirty_percent` 100 (the host saw no write delay), `zfs_txg_history` 200; everything else default.
- **Pool.** `hrp`: raidz2 of the five, ashift 12, compression off, `cachefile=none`. Zvol `hrp/vm` (sparse, 12608 MiB, volblocksize 1M) written once whole: library 8 GiB, write ring 4 GiB, db area 256 MiB, journal 64 MiB. Filesystem `hrp/fill` (recordsize 1M) filled with 1 MiB files to 88 % of the pool, then every fourth file removed (then every fourth from the third, as far as needed) until the pool is at 66 % (the host's cap): most of the free space is then 1.7 MiB holes spread over the filled part (the host's fragmentation is 32 %).
- **Guest** (`hr-load.py guest`, the host's Plex VM as QEMU drives the zvol: O_DIRECT, a guest flush is `fdatasync()`): 50 MB/s of 512 KiB writes, 4 in flight, sequential through the ring and starting 256 KiB into a block, so that half of them straddle two zvol blocks (about 1.5 ZIL records per write, the host's 148 per 104); the ring 256 MiB ahead of the writer discarded (old media deleted with discard on), so no write reads its block first; 6 small writes per second never synced; 0.4 database transactions per second (3 small writes, then `fdatasync()`); a journal commit every 5 s (16 KiB, then `fdatasync()`): about 0.6 ZIL commits per second as on the host; 7 reads per second of 128 KiB to 1 MiB inside one block of the library, 4 in flight. All open loop: a stream that falls 2 s behind skips ahead and counts what it skipped. Every operation's latency is logged.
- **One point** (`hr-bench.sh point PASS CONF`): the grid point's parameters sent and read back from every server; blk-wbt's target written again (1 first, so that it restarts from its initial depth); guest and sampler started; after `RAMP` (60 s) the counters (`/proc/diskstats`, PSI, `/proc/stat`, the zil, dmu_tx, arcstats and dmu_tx_assign kstats, `zpool iostat -w` and `-r` histograms, the txgs kstat); then `MEAS` (300 s) of `zpool iostat -T u -Hpvlq hrp 1` and `zpool iostat -T u -wpH hrp 1`, every line stamped; the counters again; the kernel log checked. The sampler (TASK-1118-VQ1's, unchanged) records every 0.25 s the disks' in-flight requests, blk-wbt's state, disk counters, PSI, CPU and every thread in uninterruptible sleep with whether blk-wbt is on its stack. About 6 min 20 s per point. The pool stays imported for the whole task; nothing is exported between points.

## Passes

| Pass | Points | |
| --- | --- | --- |
| F | `G1` with `RAMP=10 MEAS=30` | functional check at step 5, before GO, not in the verdicts |
| W | `G1` with `RAMP=30 MEAS=60` | warm-up after GO, not in the verdicts |
| G | `G1 G4 G5 G2 G3 G6`, one run each, in that order | the grid; then `hr-report.py calib $L/raw G` prints `BEST <conf>` |
| C1, C2 | the BEST point, once each | confirmation; time rule before them |

| Point | `cache_mb` | `stroke` |
| --- | --- | --- |
| G1 | 256 | 1.0 |
| G2 | 256 | 0.4 |
| G3 | 16 | 1.0 |
| G4 | 16 | 0.4 |
| G5 | 0 (write through) | 1.0 |
| G6 | 0 (write through) | 0.4 |

Projected: quiet check 1 min, W 2 min, G 38 min, C1 and C2 13 min: **about 55 min from GO**.

**Time rule** (keeps the measured part under 75 min). After G, from driver.log: d = the mean duration of G's six points (`point G <conf> start` to `point G <conf> done:`; step 8 gives the command). Run C1 and C2 if now + 2 × d + 2 min is at most GO + 75 min, C1 alone if now + d + 2 min is, otherwise neither (the calibration is then incomplete: say so).

## Written before the run

**Host reference** (DS_HDD_R6_1, blk-wbt on, collector run of 18/11 17:27-17:37Z; percentiles are log2 bucket labels, as `zpool iostat -w` prints them and as the host collector reported them: 1074 means the bucket 537-1074 ms): disk util 22 %; disk_wait read p99 1074 ms; syncq_wait write p99 4295 ms, p50 0; disk_wait write p99 268 ms; PSI io some 8.12 %; txg sync 0.77 s mean, 2.76 s p95; await read 35-38 ms, write 9 ms; 0.9 flushes per second per disk at 55 ms; 36.9 % of leaf writes sync class; `z_wr_int` in blk-wbt 7.2 % of blocked-task samples; guest 51.3 MB/s, 0.6 ZIL commits/s, 148 indirect records per 104 writes, pool writes 2.36 × the guest's, pool reads 9.8 MB/s.

**Criteria** (`hr-report.py`, constants fixed in the kit):

| Id | Metric | Accepted when |
| --- | --- | --- |
| P1 | disk util (diskstats io_ticks), mean of the five | 15 to 30 % |
| P2 | disk_wait read p99 | 537, 1074 or 2147 ms (the host's bucket or a neighbour) |
| P3 | syncq_wait write p99 | 2147, 4295 or 8590 ms |
| P3b | syncq_wait write p50 | at most 1.05 ms |
| P4 | disk_wait write p99 | 134, 268 or 537 ms |
| P5 | PSI io some | 4 to 12 % |
| W1 | guest media MB/s over the offered 50 | at least 0.9 |
| W2 | ZIL commits/s | 0.45 to 0.8 |
| W3 | indirect records per guest write | 1.1 to 1.8 |
| W4 | pool write MB/s over guest write MB/s | 1.8 to 3.0 |
| W5 | pool read MB/s | 5 to 20 |
| Q1 | leaf-seconds with sync writes queued; most queued on a leaf in a second | at least 5 %; at least 10 |
| V1 | disk completions delivered more than 2 ms late; pages the disks could not keep | at most 1 %; 0 |
| S1 | txg sync mean; p95 | 0.4 to 1.5 s; 1.4 to 5.5 s |
| S2 | await read; write | 18 to 60 ms; 4 to 20 ms |
| S3 | flushes per second per disk; mean flush time | 0.5 to 1.5; 25 to 110 ms |
| S4 | sync share of leaf writes | 25 to 50 % |
| S5 | `z_wr_int` in blk-wbt, % of blocked-task samples | 2 to 20 % |

A run is **accepted** when every P, W, Q and V criterion holds and at least 3 of the 5 S criteria do. W failing means the guest is not the host's load (a rig defect, whatever the disks do); V failing means the emulation itself was late or out of memory (the run says nothing about the host).

**Score** (closeness, lower is better): |log2(util / 22)| + |log2(PSI / 8.12)| + the distance in buckets of disk_wait read p99, syncq_wait write p99 and disk_wait write p99 from the host's.

**Selection** (`BEST` line): the accepted point of G with the lowest score; if none is accepted, the lowest score, marked `closest, NOT accepted` (C1 and C2 are still run on it, so that main gets its spread).

**Calibrated** (last line of `calib.md`): the median of each metric over the chosen point's runs (G, C1, C2) is accepted, and the runs agree: the three p99 buckets within one bucket of each other, util and PSI with a standard deviation at most 25 % of their mean.

**Noise**: steal above 1 % in a run is listed with its point; load1 counts the run's own threads, listed, not judged.

**Expected, from the model and the code (report what you see, not this).** The device model alone (step 4, `selftest`, deterministic): G1 QD1 random 4 KiB read mean 13.2 ms, p50 13.2, p99 22.0 ms (76 IOPS); with 0.4 of the stroke 10.2 ms; QD32 144 (G1) and 160 (G2) IOPS; sequential 1 MiB read 5.3 ms. The raw probe through ublk (step 5) should read within about 1 ms of the model's QD1 mean. On ZFS nothing is known: on an open-loop single-disk model of the host's leaf load (no blk-wbt, no ZFS feedback) the six points gave util 23-27 %, read p99 0.3-0.4 s, flush mean 16-180 ms and sync-queue p99 0.1-0.5 s, below the host's tails; whether ZFS's queue and blk-wbt bring them to the host's is what this task measures. A FAIL in the tables, or no point accepted, is a finding, not a stop.

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed (python3, gcc, the kernel headers of 7.0.14-25-pve and the build toolchain are all the kit needs).
- The pool lives on `/dev/ublkb70` to `/dev/ublkb74` only; the driver creates them and the pool and removes them. Touch no other pool, disk or ublk device; never `zpool import -a`; the pool has `cachefile=none` and is never exported.
- The ZTS test `zpool_import_all_001_pos` once imported VM 105's exported measurement pool `tank`, which may still be on this VM's disks, exported. Nothing in this task imports any pool; `hr-bench.sh point` stops (`STOP: a pool this task did not create is imported`) if `zpool list` shows any pool but `hrp`. Such a pool is a stop: report it and leave it as it is (no export, no destroy, no import).
- `/etc/zfs/zpool.cache`: steps 0 and 10 (below). A non-empty one names a pool the module would import at load: never load the module while one exists.
- **The disks are user processes.** If an `hddsim` server dies while the pool is imported, every I/O of the pool fails and the pool suspends; `zpool` commands then hang. Never kill an `hddsim` process yourself, never run `hddsim del`, and never stop `ublk_drv`, except through `hr-bench.sh teardown`. If `ps -C hddsim` shows fewer than five, or `$L/dev/*.out` has a line after `ready`, or the disk check (below) shows anything but its expected output: capture (below), stop and report; a reboot is then allowed to clear the suspended pool (save the diagnostics first; it does not replace step 10's reboot).
- Every build, setup step, point and chain runs detached (`setsid nohup ... &`) and is polled. A hang is: the build still going after 60 minutes; `disks` after 5 minutes; `pool` after 30 minutes; a point after 10 minutes, or `driver.log` unchanged for 10 minutes while a chain runs. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|python3|hddsim|kworker'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`), `tail -n 5 $L/dev/*.stats`, `cat $L/dev/*.out` and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- This is a performance measurement: nothing else on this VM from step 7 to the end of step 9 (no build, ztest, ZTS, second load, bpftrace or blktrace).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured point on your own.
- **The disk check** is the block below, read-only; the steps say when to run it. Expected: `5`; five lines `70 E 0 err 0 rej 0` to `74 E 0 err 0 rej 0`; `0`; `0`. Anything else is a stop (capture as for a hang, then report): fewer than five servers, an `E ` line, a non-zero `err` or `rej`, `rej -` (a server that is not kit v1.1's), a line after `ready` in a `.out` file, or a kernel I/O error (`error, dev ublkb7N`) since this task's start marker.

```
L=/var/log/task-1121-hrig
ps -C hddsim -o pid= | wc -l
for i in 70 71 72 73 74; do awk -v d=$i '/^E / {e++} /^S / {for (k = 6; k < NF; k++) if ($k == "err" || $k == "rej") v[$k] = $(k + 1)} END {printf "%s E %d err %s rej %s\n", d, e, ("err" in v) ? v["err"] : "-", ("rej" in v) ? v["rej"] : "-"}' $L/dev/$i.stats; done
for i in 70 71 72 73 74; do sed -n '/ready/,$p' $L/dev/$i.out | sed 1d; done | wc -l
sudo -n dmesg | tac | sed '/task-1121-hrig: start/q' | grep -c 'error, dev ublkb'
```

## Steps

### 0. State, on 7.0.14-25-pve (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s; swapon --show
lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; ls -l /dev/zfs /dev/ublk-control 2>&1; ls /dev/ublkb* /dev/ublkc* 2>&1; losetup -a; ls -ld /var/tmp/task-1121-hrig /var/log/task-1121-hrig 2>&1
grep -E 'CONFIG_BLK_DEV_UBLK|CONFIG_BLK_WBT=|CONFIG_BLK_WBT_MQ|CONFIG_IO_URING=|CONFIG_PSI=' /boot/config-$(uname -r)
sudo -n grub-editenv list; command -v python3 git make gcc; python3 --version; gcc --version | head -1; pgrep -a -x zed || echo NO-ZED
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|hddsim'
ls -l /etc/zfs/zpool.cache 2>&1; ls -la /var/log/task-1121-hrig; du -sh /var/tmp/task-1121-hrig
```

Stop and report, changing nothing, if: `uname -r` is not `7.0.14-25-pve`; a zfs, spl, ublk_drv or null_blk module is loaded, `/dev/zfs` or a `/dev/ublk*` exists; `losetup -a` is not empty; `$T` or `$L` is missing, or `$L/run1` exists; /var/tmp has less than 10 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; the config lacks `CONFIG_BLK_DEV_UBLK=m` or `=y`, or `CONFIG_BLK_WBT` or `CONFIG_PSI`; `grub-editenv list` shows a `next_entry`; python3 is older than 3.8; a build, test, load or `hddsim` runs. Note, without stopping: swap, zed (do not start or stop it). `/etc/zfs/zpool.cache`: absent is expected; size 0: `L=/var/log/task-1121-hrig; sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0-hrig2` and say so; size above 0: stop and report its `ls -l`, load nothing.

Then set TASK-1121-HRIG's outputs aside in `$L/run1/`, so that every file this task reads or polls is its own; the build logs and any `zpool.cache.*` stay where they are:

```
L=/var/log/task-1121-hrig
sudo -n mkdir $L/run1 && cd $L && for f in *; do case $f in run1|build.log|autogen-R.log|configure-R.log|build-R.log|zpool.cache.*) ;; *) sudo -n mv -- "$f" run1/ ;; esac; done; ls -la $L $L/run1
mkdir $L/bin && ls -ld $L/bin
```

Expected: `$L` holds `run1/`, the four build logs and any `zpool.cache.*`, then the new empty `bin/`, owned by you (TASK-1121-HRIG step 0 gave you `$L`); `run1/` holds the rest (`bin/`, `dev/`, `driver.log`, ...). If `mkdir $L/bin` fails, stop and report.

### 1. Kit v1.1, fetched from the fork branch

```
L=/var/log/task-1121-hrig; T=/var/tmp/task-1121-hrig
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-hrig2
test "$(git rev-parse FETCH_HEAD)" = 9b1c7beb9f8917c1f0f27d2593f9f8d72991f59f && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 31c7f64b9b704ffc1f0066589f0288bf2ada8bd2 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    f79f57a7cec57a602c0c71acad38f1803af1d63ed0a1558c5ca91429142f056b grid/G1.conf \
    eec8a376ff13d234b3c14935f0ec4e25242c4cb4d7ceea5f9509f014daf42256 grid/G2.conf \
    bb601f6e7c4ead34391e2e8d55ffa8a210b250fc298243ce39cd2349bebd537f grid/G3.conf \
    d879b967ac50153f9b52f5ef5cbe6bcc44beb0154c4a173820e35c8084ce0b93 grid/G4.conf \
    0b721c92bb9fc53f4cc257a1a2f2ac591e0c48871c1e06310c31efabd340ad4b grid/G5.conf \
    3ee3cb3731b5323064c03ea239185fa63ad3a157ba2d7a1a185796b39147285b grid/G6.conf \
    ba2267bacf0bf60dc531c2d0431633737017553b17a761fbdfd08844a856b251 grid/fast.conf \
    951f40c41cb93d3e6fb9becbcf90825480365490bd50f8a1940650eaa424dc22 hddsim.c \
    0f55f9097f41f368493f2948eabb14785ff6dd984cf4149a7f0c49757ebad1ae hr-bench.sh \
    b259d0f15f7df96326179564088e6f80433c501eca9723d69770f20e2ce44669 hr-load.py \
    677c09dadd8cdf48ecb41548814f45f890e14c1b06258104670d268ca8b0d7d0 hr-report.py \
    f6df70024895b41f9ddff492ed127e6e2a60d69d0301f6c53f8737535b11aa46 hr-sample.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n hr-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l hr-bench.sh
```

Expected: the three `OK` lines; twelve entries, exactly `100644` for the seven `grid/*.conf`, `hddsim.c`, `hr-load.py`, `hr-report.py`, `hr-sample.py`, and `100755 hr-bench.sh`; twelve `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`. The driver is bash: run it with `bash` or directly.

### 2. The kept 2.4.4 tree, built only if it does not check (about 1 min, or 15 with a build)

```
T=/var/tmp/task-1121-hrig; L=/var/log/task-1121-hrig
cat $L/build.log; git -C $T/R rev-parse HEAD 'HEAD^{tree}'
printf '%s  %s\n' \
    aff082bff393836fd3d415cf6441fee1c034e1f8378a94a4a1a573d21ce64ad6 $T/R/module/zfs/zil.c \
    1cdfb808e2278ba283ca203c158779072b8003e6869978cee757b42fd09ec09a $T/R/module/zfs/vdev_queue.c \
    25f598a1675a2d6af06d3226d8c23e0b689090363e001504ed92b716893638a6 $T/R/module/os/linux/zfs/vdev_disk.c \
    ab365b560991d24b30ddbe3c54432d4ad6f2014ef619020b8dc6cf513044b3f5 $T/R/module/zfs/zvol.c > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
echo "R $(modinfo -F vermagic $T/R/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/R/module/zfs.ko)"; ls -l $T/R/module/zfs.ko $T/R/module/spl.ko $T/R/zpool $T/R/scripts/zfs.sh
```

Expected: `R rc=0 ...` and `build-done`; `71a9f9578616a90c3c14bb59629fb4d31bfd68d1` then `d2a62ec4527f872ce840ba60f50248e66fcc080b`; four `OK`; exactly `R 7.0.14-25-pve DDF4C2CCA20EC708474D57F`; the four files listed. Then nothing is built, and `DDF4C2CCA20EC708474D57F` is the srcversion every load must show.

**Only if one of these differs or is missing**, build again from scratch, as TASK-1121-HRIG did (it runs on 7.0 now; the headers are named, so nothing else changes); a hang is the build still going after 60 minutes:

```
T=/var/tmp/task-1121-hrig; L=/var/log/task-1121-hrig
rm -rf $T/R $T/repo
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

Three `OK` lines and four `OK` from `sha256sum -c`. Poll `cat $L/build.log` until `build-done`. Then `cat $L/build.log; echo "R $(modinfo -F vermagic $T/R/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/R/module/zfs.ko)"; grep -h 'warning:' $L/build-R.log | sort -u | head -20`: `R rc=0`, vermagic `7.0.14-25-pve` and the same srcversion `DDF4C2CCA20EC708474D57F`; another srcversion: stop and report. On a failed build, quote `grep -nE 'error:|Error [0-9]' $L/build-R.log | head -20` and stop.

Then, in every case, the model of kit v1.1 alone:

```
T=/var/tmp/task-1121-hrig; L=/var/log/task-1121-hrig
gcc -O2 -Wall -pthread -o $T/hddsim-st $L/bin/hddsim.c -lm 2>&1; for g in G1 G2; do $T/hddsim-st selftest $L/bin/grid/$g.conf 1800 | sed -n '2,4p'; done
```

Expected: no compiler output. The selftest lines: G1 `QD1 random 4K read: read n 3000 mean 13.2 p50 13.2 p99 22.0 ...`, `QD32 random 4K read: read n 6000 mean 221.5 p50 190.1 p99 673.9 ...`, `QD1 sequential 1M read: read n 2000 mean 5.3 ...`; G2 `mean 10.2 p50 10.2 p99 17.1`, `mean 199.1 p50 169.8 p99 599.1`, `mean 5.3`. Another value, or a compiler warning from hddsim.c: stop and report.

### 3. No boot

TASK-1121-HRIG's step 3 (the boot into 7.0.14-25-pve) is not run: step 0 found the VM on it. Do not change swap or sysctls.

### 4. Disks and the model, offline (about 2 min, functional)

```
L=/var/log/task-1121-hrig
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh disks /var/tmp/task-1121-hrig/R > $L/disks.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `disks done` or `STOP`. Then `cat $L/disks.nohup $L/setup-devices.txt $L/hddsim-build.txt; cat $L/dev/7*.out; ps -o pid,stat,ni,rss,comm -C hddsim`, and:

```
L=/var/log/task-1121-hrig
cd $L/bin && sudo -n bash ./hr-bench.sh selftest; tail -n 1 $L/driver.log; for g in G1 G2 G3 G4 G5 G6; do echo "== $g"; sed -n '2p;6p' $L/selftest-$g.txt; done
```

Expected: `hddsim built: 0 warnings`; five lines `hddsim 7N: /dev/ublkb7N ready pid ...`; five `hddsim` processes; `setup-devices.txt` with kernel 7.0.14-25-pve, the ublk_drv vermagic, `hddsim.c 951f40c41cb93d3e6fb9becbcf90825480365490bd50f8a1940650eaa424dc22` (kit v1.1's), the zfs srcversion of step 2 (`DDF4C2CCA20EC708474D57F`), and for each disk `size_mib 16384 sched ... [none] rot 1 wbt_lat_usec 75000 write_cache write_back ... max_sectors_kb 1024 lbs 4096 iostats 1` and a number after `wbt_debugfs` (if `-`: `sudo -n mount -t debugfs none /sys/kernel/debug`, say so, and rerun nothing). The selftest lines as in step 2 (same program). A disk not as expected: `teardown`, stop and report. Then the disk check (VM rules).

### 5. Probe, pool, functional point (about 25 min, functional)

```
L=/var/log/task-1121-hrig
cd $L/bin && sudo -n bash ./hr-bench.sh probe G1; tail -n 1 $L/driver.log; cat $L/probe-G1.txt
```

Expected (other VMs may be running: a finding if off, not a stop, unless 3 ms or more above the model): `QD1 random 4K read: ... mean` within 1 ms of 13.2; `QD1 sequential 1M write` a few ms (the cache takes them); `flush after them` the time to destage what was written: the line TASK-1121-HRIG never got. On `STOP: probe`, run the disk check, then `teardown`, then report with `cat $L/probe-G1.txt`. Then the disk check, and the pool:

```
L=/var/log/task-1121-hrig
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh pool > $L/pool.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pool done` or `STOP` (about 10 to 20 min). Then `cat $L/pool.nohup $L/setup-zpool-list.txt $L/setup-props.txt` and `grep -c . $L/dev/70.stats`, then the disk check. Expected: `pool done:` with size about 79.5G, cap 60 to 68 %, frag recorded (the host's is 32 %; 20 to 45 % expected, another value is a finding), MemAvailable at least 2500 MiB; `full_pages` per disk below 50000 (200 MiB). Then the functional point:

```
L=/var/log/task-1121-hrig
cd $L/bin && setsid nohup sudo -n env RAMP=10 MEAS=30 bash ./hr-bench.sh point F G1 > $L/point-F.nohup 2>&1 < /dev/null &
```

Poll until `point F G1 done` or `STOP` (about 1 min). Then `python3 -I $L/bin/hr-report.py calib $L/raw F > $L/calib-f.md; echo rc=$?; cat $L/calib-f.md; tail -n 3 $L/raw/F/G1/guest.txt; cat $L/raw/F/G1/guest.err`, then the disk check. **Stop and report** (after `teardown`) if: rc is not 0; the guest's `END` line is missing or there is an `ERROR` line; V1 fails on pages kept (store errors); `models` is not `1`; the `zpool iostat` streams are empty. Every other FAIL in calib-f.md is a finding (30 s is too short to judge): quote the table.

### 6. Note to main, then wait for GO

Steps 7 to 9 must run alone on the host, on a quiet VM. End your turn with the interim results (step 0's kernel lines and what `run1/` holds; step 2's lines, vermagic and srcversion, and whether anything was built; the selftest lines; `setup-devices.txt`; the disk check after `disks`; the probe; `setup-zpool-list.txt`'s last lines and the pool line; calib-f.md's two tables; the disk check after F) and, as the very last line, exactly:

`TASK-1121-HRIG2 READY-TO-MEASURE (about 55 min alone on the host, at most 75). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 7 only on a message from main that contains `GO TASK-1121-HRIG2`; note the time of GO. On `HOLD`, keep waiting. On `ABORT TASK-1121-HRIG2`, go to step 10 without running anything. Nothing runs while you wait (pool imported and idle, disks in G1).

### 7. Quiet check and warm-up (about 3 min)

```
L=/var/log/task-1121-hrig
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n bash ./hr-bench.sh quiet 60; tail -1 $L/driver.log
cd $L/bin && setsid nohup sudo -n env RAMP=30 MEAS=60 bash ./hr-bench.sh point W G1 > $L/point-W.nohup 2>&1 < /dev/null &
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. Poll until `point W G1 done` or `STOP`; then `grep -h '^srcversion' $L/raw/W/G1/layout.txt; grep -o 'wbt_lat_usec [0-9]*' $L/raw/W/G1/layout.txt | sort | uniq -c`: the step 2 srcversion (`DDF4C2CCA20EC708474D57F`); five `wbt_lat_usec 75000`; then the disk check.

### 8. The grid (about 38 min)

```
L=/var/log/task-1121-hrig
cd $L/bin && setsid nohup sudo -n bash -c 'for c in G1 G4 G5 G2 G3 G6; do ./hr-bench.sh point G $c || exit 1; done; echo chain-done' > $L/chain-g.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `point G G6 done` or `STOP`; after each `point G <c> done`, the disk check (its expected output, or stop). Then:

```
L=/var/log/task-1121-hrig
python3 -I $L/bin/hr-report.py calib $L/raw G > $L/calib-g.md; echo rc=$?; tail -n 1 $L/calib-g.md
awk -v go="$(cat $L/go.txt)" -v now="$(date +%s)" '
function sec(t, a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
$2 == "point" && $3 == "G" && $5 == "start" { s[$4] = sec($1) }
$2 == "point" && $3 == "G" && $5 == "done:" { e = sec($1) - s[$4]; if (e < 0) e += 86400; d += e; n++ }
END { d = d / n; k = now + 2 * d + 120 <= go + 4500 ? 2 : now + d + 120 <= go + 4500 ? 1 : 0
    printf "d %.0f s over %d points, now %d, GO+75 %d, k %d: run %s\n", d, n, now, go + 4500, k, k == 2 ? "C1 C2" : k == 1 ? "C1" : "none" }' $L/driver.log | tee $L/time-rule.txt
```

Expected `over 6 points`. The last line of calib-g.md is `BEST <conf> ...`.

### 9. Confirmation (about 13 min)

With `B` the conf named on the BEST line (G1 to G6), run what the time rule leaves, as one detached chain:

```
L=/var/log/task-1121-hrig; B=<conf>
cd $L/bin && setsid nohup sudo -n bash -c './hr-bench.sh point C1 '$B' && ./hr-bench.sh point C2 '$B'; echo chain-done' > $L/chain-c.nohup 2>&1 < /dev/null &
```

(with k = 1, only `./hr-bench.sh point C1 $B`). Poll until the last `done` or `STOP`; run the disk check, then write in your chat the line `TASK-1121-HRIG2 MEASURED <UTC time>: the runs are over, the host is free.` and go on without waiting:

```
L=/var/log/task-1121-hrig
python3 -I $L/bin/hr-report.py calib $L/raw G C1 C2 > $L/calib.md; echo rc=$?; tail -n 1 $L/calib.md
```

(with k = 1, `G C1`; with k = 0, skip it and say so).

### 10. Teardown, back to Debian, end

Run the disk check, then:

```
L=/var/log/task-1121-hrig; cd $L/bin
sudo -n bash ./hr-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|ublk_drv) '; ls /dev/ublk* 2>&1; ps -C hddsim -o pid= | wc -l; tail -n 1 $L/dev/7*.stats
sudo -n dmesg | sed -n '/task-1121-hrig: start/,$p' > $L/kmsg.txt; grep -c 'task-1121-hrig: start' $L/kmsg.txt
tac $L/kmsg.txt | sed '/task-1121-hrig: start/q' | tac > $L/kmsg-hrig2.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|ublk' $L/kmsg-hrig2.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw dev run1 && sudo -n rm -rf /var/tmp/task-1121-hrig && sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: no module loaded, no `/dev/ublk*`, 0 `hddsim` processes, each stats file ending with `I ... hddsim 7N stopped (stop 0 del 0 io_err 0)`; `2` start markers in `kmsg.txt` (TASK-1121-HRIG's and this task's; `1` means the first has left the kernel buffer: say so); in `kmsg-hrig2.txt`, this task's part, no kernel line but `ublk` lines (quote every line, with the step it happened in). TASK-1121-HRIG's `invalid error, dev ublkb70` line sits in `kmsg.txt` before the second marker: quote it once as TASK-1121-HRIG's, not as a finding of this task. `/etc/zfs/zpool.cache`: size 0: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it. Keep `/var/log/task-1121-hrig` (raw data and `run1/` included). Then boot back into the Debian 6.12 kernel by a plain reboot (the default entry), resuming by your own arrangement; then `uname -r`, `sudo -n grub-editenv list` (no `next_entry`), `fstrim -av`, `df -h /`, and send the report.

## Stop conditions (all in one place)

Step 0's list, `$L/run1` existing or `mkdir $L/bin` failing; a fetch failing twice; any commit, tree, tag, parent, file list, mode or hash that differs; the kept tree failing step 2's checks and the build then failing or giving another srcversion than `DDF4C2CCA20EC708474D57F`; selftest lines other than the expected ones, or hddsim warnings; a disk not as expected after `disks`; the disk check giving anything but its expected output, wherever it runs: fewer than five `hddsim` processes, an `E ` line, a non-zero `err` or `rej`, `rej -`, a line after `ready` in a `.out` file, a kernel I/O error on a `ublkb7N` (capture as for a hang, report; reboot only to clear a suspended pool); a `STOP: probe` (the disk check, then `teardown`, then report); step 5's stop list; a check after W that fails; a driver `STOP:` (teardown, then report); a pool this task did not create; a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING` or hung task in the kernel log after this task's start marker (capture, report); an OOM kill; more than 2 h 30 on the VM without the wait for GO (finish the current point, then step 10, and say what did not run). A FAIL in a criteria table, no accepted point, `NOT CALIBRATED`, or a confirmation dropped by the time rule is a finding, not a stop.

## Report

`/var/log/task-1121-hrig/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from calib-g.md and calib.md (recompute nothing by eye):
   a. **Calibrated or not**: the last line of calib.md (or of calib-g.md if no confirmation ran), the chosen point, and which criteria failed.
   b. **The host's numbers against the chosen point's median**: P1 to P5, then S1 to S5, from calib.md's "Chosen point" tables.
   c. **The grid**: per point, the score and the criteria that failed (calib-g.md's "Criteria" table), and which way util, read p99, sync-queue p99, write p99 and PSI moved with `cache_mb` and with `stroke`.
   d. **Does the rig build a sync queue** (Q1: leaf-seconds with sync writes queued, most queued) and **does blk-wbt act** (S5, "blk-wbt background limit < 4"), per point.
   e. **The guest's view** per point: fsync p50, p99 and max, db transaction p99, read p99, media MB/s and skipped operations (the `END` line).
   f. **Emulation health**: V1 per run (late completions, store errors), the `disk requests refused` row per run (0: anything else was a stop), `full_pages` after the pool and the last `full` value per disk, the probe against the model (its `flush after them` line included), and `media busy %` against `disk util %`.
   g. **Noise**: the quiet check; per run the steal and load1; any run with steal above 1 %; the time of GO and what the time rule dropped.
2. calib.md whole (or calib-g.md if no confirmation ran), then calib-f.md's "Criteria" table.
3. Setup: kernel (`uptime -s`: the boot TASK-1121-HRIG made), /proc/cmdline, CPUs, RAM, swap, the 7.0 kernel config lines, kit v1.1 checks and file hashes, step 2's lines (the kept tree, or the build line), code checks, vermagic, srcversion, the selftest lines, setup-devices.txt, the disk checks, the probe, setup-zpool-list.txt, setup-props.txt; per point its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 9, and the time on the VM without the wait.
4. dmesg findings (this task's part, then TASK-1121-HRIG's line once), deviations, what could not be run, end state (modules, devices, processes, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0 and 10, what `run1/` holds, the reboot with its UTC time and how you resumed, then End.
