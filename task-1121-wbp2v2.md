TASK-1121-WBP2V2 (from the coordinator session "main"): performance, on one of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB), about 2 h on the VM plus the wait for main's GO, at most 2 h 30 without that wait. Its measured part (steps 5 and 6, about 75 to 85 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 4 are functional and may run while other VMs work. Two release builds fetched from the fork branch `wbp2v2-pr2` (upstream master a1daebfec3f, and the PR2 v2 commit on it), one null_blk device, two cgroups, five scenarios per arm (four on a kernel without `io.prio.class`), three arms, six to eight passes. This is TASK-1120-WBP2 again, for the second version of the change it measured. No reboot is pre-approved. Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1121-wbp2v2/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1121-wbp2v2`, `L=/var/log/task-1121-wbp2v2`; the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a "surprise" in a report table is a finding (then it is reported, not a stop).

## Why

Every bio ZFS submits for a read or a write is built by `vbio_add_page()` (`module/os/linux/zfs/vdev_disk.c`). ZFS issues pool I/O from whatever thread is at hand: the zio taskq threads (kernel threads, root cgroup) submit most of it, but a user thread submits its own reads inline and, through `vdev_queue_io()`, whichever queued I/O the vdev queue releases at that moment. On 6.12 the submitting thread gives a bio left at class NONE its I/O priority (`submit_bio()`, `bio_set_ioprio()`: the one `ionice`/`ioprio_set()` gave it, or one derived from its nice value), then the blkcg's `io.prio.class`, if any, applies; and from 5.18 `bio_alloc(bdev, ...)` charges the bio to the submitting thread's cgroup, where `io.max`, `io.latency` and `io.weight` (iocost) act on it.

TASK-1120-WBP2 measured the first version of PR2 on this rig: C1 gave every bio best-effort level 4, C2 charged every bio to the root blkcg. Its findings: on master the zio taskq threads submit at best-effort level 0 and the user threads at level 4; C1 removed `ionice -c3`'s protection of the victim (agg's own reads left the idle class: victim read p99 ×9.55, fsync mean ×6.97, txg sync ×9.3, p 0.008) and gained nothing elsewhere; C2 stopped `io.max` and iocost from limiting the aggressor's reads (agg read ×10.7 and ×290, io.stat 0). Both were dropped.

PR2 v2 is one commit (**S = 5ddc6188485**, on upstream master **M = a1daebfec3f**): a module parameter `zfs_vdev_disk_rt_mask` (default 0, writable at run time). Bit 1 gives the bios of synchronous reads (`ZIO_PRIORITY_SYNC_READ`) the real-time class at level 4, bit 2 those of synchronous writes (`ZIO_PRIORITY_SYNC_WRITE`: ZIL blocks, `dmu_sync()` writes, Direct I/O writes, labels), **except** when the submitting thread chose its own priority (an I/O priority class set with `ioprio_set()`/`ionice`, or the SCHED_IDLE, SCHED_FIFO, SCHED_RR or SCHED_DEADLINE policy): that thread's bios stay at class NONE, so the kernel gives them the thread's priority as before. Every other bio stays at class NONE as on master. The bio's blkcg is not touched. Its purpose is the host's disks (scheduler `none`, NCQ priority): a drive with NCQ priority enabled serves real-time requests first; this rig cannot show that (null_blk has no NCQ priority; a separate host-like rig measures it). On this rig mq-deadline serves the real-time class ahead of every other class, which is what scenario e measures.

This task answers three questions on 6.12: does `ionice -c3` still protect the victim (scenario a), do `io.max` and iocost still limit the aggressor (c, d), and what does the real-time class do with no control (e), for synchronous reads alone (S1) and for synchronous reads and writes (S3).

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
| --- | --- | --- | --- |
| S, PR2 v2 | `wbp2v2-pr2` | `5ddc61884852afd62a62754272485dd9dfbeb3d4` | `2b7990879815225c315aeb7950c037a7c859ffae` |
| M, upstream master (S~1) | - | `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` | `0b23c141b417a8542546e1f7a32662bc82bc0f92` |
| kit | `vm-kit-wbp2v2` | `a2a67ffa1b2d1e15f228376773ae226932c46217` (no parent) | `57e3f09f74fbb8df34a8114c5d2309c1fdec3d35` |

`git diff --numstat` from M to S: `man/man4/zfs.4` 47/1, `module/os/linux/zfs/vdev_disk.c` 46/0. M's `vdev_disk.c` is byte for byte TASK-1120-WBP2's M (sha256 `05a9989e...`).

Builds: release (`./configure` with no option), as WBP2. No functional run besides step 3's flag check, so no debug build.

## The rig (`wbp2-bench.sh setup`)

TASK-1120-WBP2's rig unchanged: one memory-backed null_blk device `wbp2nb0` (1536 MiB, 4K blocks, 8 ms per request by timer, 4 tags, no write cache), mq-deadline with `nr_requests` 64, `rotational` 1, blk-wbt off (`wbt_lat_usec` 0). A pool `wbp2p` on it (ashift 12, compression off, recordsize 128K, `primarycache=metadata`, `cachefile=none`, mounted under `$T/mnt`), datasets `v` and `a`, each with a 256 MiB file `r` and a 128 MiB file `w`. Cgroups `/sys/fs/cgroup/wbp2/vic` and `/sys/fs/cgroup/wbp2/agg` with the io controller (the kit writes `+io` to the root's `cgroup.subtree_control` only if it is missing, and says so). A tracefs instance `wbp2` traces `block_rq_insert` on the device only (rwbs, bytes, ioprio, inserting task). `zfs_arc_max` 1 GiB.

**One scenario** (`run_one`, 5 s ramp + 30 s measured), as WBP2: in `vic`, a reader of 128K records at random offsets of `v/r`, 40 per second (`wbp2-vr`), and a writer of 128K records to `v/w`, 20 per second, with an `fdatasync()` after every two (`wbp2-vw`); in `agg`, a closed-loop reader of 128K records of `a/r` with 8 threads (`wbp2-ar`) and a writer of 16K into the 128K records of `a/w`, 100 per second, never synced (`wbp2-aw`). During the 30 s: the trace; at both ends: io.stat of both cgroups, the txgs kstat, diskstats, /proc/stat. After: the settings cleared, `zpool sync`, the kernel log checked.

| Scenario | Setting on `agg` |
| --- | --- |
| a | its two processes under `ionice -c3` (idle) |
| b | `io.prio.class` `idle` |
| c | `io.max` `riops=40 wiops=40` on the device |
| d | iocost on the device (linear model 150 IOPS, 20 MiB/s; vrate fixed at 100 %), `io.weight` 1 for agg, 10000 for vic |
| e | nothing: no ionice, no cgroup control (new in this task) |

A scenario whose files the kernel lacks (`io.prio.class`: CONFIG_BLK_CGROUP_IOPRIO, missing on WBP2's VM; `io.max`: CONFIG_BLK_DEV_THROTTLING; `io.cost.qos`, `io.weight`: CONFIG_BLK_CGROUP_IOCOST) is left out by the kit and listed in `setup-cgroup.txt`; that is a finding, not a stop.

| Arm | Tree | `zfs_vdev_disk_rt_mask` |
| --- | --- | --- |
| M | `$T/M` | absent (the kit checks that the parameter does not exist) |
| S1 | `$T/S` | 1 (synchronous reads) |
| S3 | `$T/S` | 3 (synchronous reads and writes) |

The kit writes the parameter after loading the tree and reads it back (`STOP` if it reads otherwise), and records it in every run's `layout.txt`. S1 and S3 share one build: switching between them reimports the pool without reloading the modules.

**One pass** (`wbp2-bench.sh pass`): for each arm in the pass's order: the pool exported, the arm's tree loaded if it changes, the parameter set, the pool imported (`zpool import -o cachefile=none -d /dev/wbp2nb0 wbp2p`, nothing else), then the scenarios. About 42 s per scenario, 20 s per tree switch: **about 9 min per pass** with four scenarios, 11 with five.

## Passes

The arm orders form a Williams design for three arms, as WBP2: P1 to P6 put each arm twice in each position and let each arm follow each other arm exactly twice. P7 and P8 repeat P1 and P2.

| Pass | Arm order | |
| --- | --- | --- |
| F | M S1 S3 | step 3's flag check, every scenario, 2 s + 8 s, raw traces kept, not in the verdicts |
| W | M S1 S3 | warm-up after GO, `RAMP=2 MEAS=6`, not reported |
| P1 | M S1 S3 | |
| P2 | S1 S3 M | |
| P3 | S3 M S1 | |
| P4 | S3 S1 M | |
| P5 | M S3 S1 | |
| P6 | S1 M S3 | time rule after it |
| P7 | M S1 S3 | |
| P8 | S1 S3 M | |

Projected with four scenarios: quiet check 1 min, W about 4 min, P1 to P6 about 54 min: **about 60 min from GO** to the end of P6, about 78 min with P7 and P8. With five scenarios the time rule drops P8 or both.

**Time rule** (keeps the measured part under 90 min). After P6, from driver.log: d = the mean duration of P1 to P6; run the largest number k of P7, P8 (in that order, k from 2 down to 0) for which now + k × d + 2 min is at most GO + 90 min. Six passes are the fewest that can give p <= 0.05. Step 6 gives the command; it writes `$L/time-rule.txt`.

## Written before the run

- **Verdict per scenario, pair and metric** (`wbp2-report.py raw P`), as WBP2: over the passes where both arms ran the scenario, ratio = a / b in the same pass; "lower" if the geometric mean of the ratios is 0.97 or less and the exact sign-flip permutation p is 0.05 or less, "higher" if 1.03 or more with the same p, otherwise "no difference shown". Pairs: S1/M (synchronous reads real-time), S3/M (reads and writes), S3/S1 (what the write bit adds). Metrics: vic read p50 and p99, vic fsync mean and p90, agg read MB/s, agg write ops/s and p99, txg sync time mean.
- **Firm expectations** (the report's "expected" column): the two harms WBP2 measured must be gone. a: S1/M and S3/M agg read MB/s "no difference shown" (agg's own reads stay idle), vic read p99, vic fsync mean and txg sync mean "not higher" (it holds on "lower" or "no difference shown"). b, c, d: S1/M and S3/M agg read MB/s "not higher" (the control still limits agg's reads). A "surprise" there is the finding this task exists for: reported, not a stop.
- **Request classes** ("Request classes by inserting group"): every request inserted into the device's queue during the windows, by inserting group (`vic`: wbp2-vr and wbp2-vw; `agg`: wbp2-ar and wbp2-aw; `zfs`: z_* and txg_* threads; `kworker`; `other`), reads and writes apart, by class (`none`, `be0`, `be4`, `idle0`, `rt4`, ...). Listed, not judged; then "Class checks" judges the facts the code fixes: on every arm and scenario the zfs threads insert requests; on M no real-time request; in a, every request an agg thread inserts is idle; on S1 no real-time write; on S1 and S3 at least half of vic's reads real-time; on S3 some real-time writes.
- **blkcg charge** ("blkcg charge and controller state"): the io.stat deltas of vic and agg per run, the share of agg's read bytes charged to agg; every arm must charge both cgroups (rios above 0); iocost's `cost.usage` for agg in d, listed (WBP2's `cost.wait` needs `blkcg_debug_stats` and read 0; this kit reads `cost.usage` instead).
- **Noise**: steal above 1 % in a run is listed with its pass; load1 counts the run's own threads, listed, not judged; trace events lost or lines not parsed are listed per run (0 expected).

**Expected, from reading the code (report what you see, not this).** On M, as WBP2 found: zfs threads `be0`, user threads `be4` (or `none`), agg `idle` in a; no `rt` anywhere. On S1 and S3, a synchronous read submitted by a thread without a priority of its own is `rt4`: vic's reads in every scenario, agg's own reads in b, c, d and e (not in a: agg's threads run under `ionice -c3`, so theirs stay idle, the protection WBP2's C1 lost), and the queued synchronous reads that a zfs completion thread releases. On S3 the synchronous writes are `rt4` too: the ZIL blocks of vic's `fdatasync()` and the blocks written for them, submitted by `z_wr_iss*` threads. Asynchronous reads and the transaction group's writes keep the submitting thread's class (`be0` from zfs threads). In b the cgroup's `io.prio.class idle` turns agg's `rt4` into idle. In c, blk-throttle dispatches the bios it held from a `kworker`, with the class they carried.

Effects: a, b, c, d: the firm expectations above. Not known before the run: everything in e, and the vic latencies under the controls. With mq-deadline, real-time requests are dispatched before every best-effort and idle one, up to its 10 s aging: in e, S1 should lower vic's read latency, and agg's (its reads are synchronous too); the transaction group's writes, best-effort, wait behind 8 closed-loop real-time readers, so the txg sync time, the agg writer and vic's fsync may get worse; S3 lifts vic's ZIL writes out of that queue. Which dominates is what the owner needs. This rig cannot show NCQ priority (null_blk has none) or BFQ.

**Lead "user threads submit I/O they did not issue" (not a verdict).** A user thread never issues a write of its own: transaction group, ZIL and `dmu_sync()` writes are submitted by zfs threads. Every write (`W`) a `vic` or `agg` row shows on M is therefore I/O the vdev queue released to that thread for someone else, which then carries the thread's priority and cgroup. The report quotes those counts.

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed (python3, util-linux's `ionice` and the build toolchain are all the kit needs).
- The pool lives on `/dev/wbp2nb0` only; the driver creates it and the device and removes them. Touch no other pool, disk or null_blk device; never `zpool import -a`; the driver imports only its own pool, by name, from its own device, with `cachefile=none`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks. Nothing in this task runs it, but after setup and after every pass `zpool list` must show no pool but `wbp2p`; the driver checks it at every arm switch and stops (`STOP: a pool this task did not create is imported`). A pool this task did not create is a stop: report it, and leave it as it is (no export, no destroy, no import).
- `/etc/zfs/zpool.cache`: step 0 and step 7 (below). A non-empty one names a pool the module would import at load: never load the module while one exists.
- Every build, the setup, the flag check and every pass run detached (`setsid nohup ... &`) and are polled. A hang is: a build still going after 90 minutes; the setup after 15 minutes; the flag check after 25 minutes; a pass after 20 minutes, or `driver.log` unchanged for 8 minutes while one runs (a scenario logs a line about every 42 s). Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|python3|kworker'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- Reboots: one is pre-approved only where a task says so, and this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees. Modules unloaded at the end (`teardown`).
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6 (no build, ztest, ZTS, second load, bpftrace or blktrace).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured pass on your own.

## Steps

### 0. State (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -l /dev/zfs 2>&1; losetup -a; ls -d /dev/wbp2nb0 /var/tmp/task-1121-wbp2v2 /var/log/task-1121-wbp2v2 /sys/fs/cgroup/wbp2 2>&1; ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1
command -v python3 git make gcc nm ionice modinfo; python3 --version; id zts; pgrep -a -x zed || echo NO-ZED; command -v bpftrace blktrace || echo no-bpftrace-no-blktrace
sudo -n ls -d /sys/kernel/debug/block /sys/kernel/tracing/events/block/block_rq_insert /sys/kernel/tracing/instances/wbp2 2>&1; sudo -n grep -c ioprio /sys/kernel/tracing/events/block/block_rq_insert/format
grep -E 'CONFIG_BLK_CGROUP=|CONFIG_BLK_CGROUP_IOPRIO|CONFIG_BLK_CGROUP_IOCOST|CONFIG_BLK_DEV_THROTTLING=|CONFIG_BLK_DEV_NULL_BLK=|CONFIG_MQ_IOSCHED_DEADLINE|CONFIG_BLK_WBT=' /boot/config-$(uname -r)
cat /sys/fs/cgroup/cgroup.controllers /sys/fs/cgroup/cgroup.subtree_control; ls /sys/fs/cgroup/io.cost.qos /sys/fs/cgroup/io.cost.model 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl or null_blk module is loaded, a null_blk device exists or `/dev/zfs` exists; `losetup -a` is not empty; `/dev/wbp2nb0`, `$T`, `$L`, `/sys/fs/cgroup/wbp2` or the trace instance `wbp2` exists (TASK-1120-WBP2's teardown removed them); a `/var/tmp/file-vdev*` or `/var/tmp/dev-*` is left; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `CONFIG_BLK_CGROUP` is not `y` or `io` is not in `cgroup.controllers`; `CONFIG_BLK_DEV_NULL_BLK` or `CONFIG_MQ_IOSCHED_DEADLINE` is missing; the `block_rq_insert` format has no `ioprio` (count 0); python3 is older than 3.8; `ionice`, `nm` or `modinfo` is missing; a build, test or load runs. Note, without stopping: the three cgroup configs (a missing one drops a scenario), the zed line (do not start or stop zed), bpftrace/blktrace (not used either way). If `/sys/kernel/debug/block` is missing, `sudo -n mount -t debugfs none /sys/kernel/debug` and say so.

`/etc/zfs/zpool.cache`: absent is expected. If it exists with size 0, move it aside and say so: `L=/var/log/task-1121-wbp2v2; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0`. If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

```
L=/var/log/task-1121-wbp2v2; T=/var/tmp/task-1121-wbp2v2
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit, fetched from the fork branch

```
L=/var/log/task-1121-wbp2v2; T=/var/tmp/task-1121-wbp2v2
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-wbp2v2
test "$(git rev-parse FETCH_HEAD)" = a2a67ffa1b2d1e15f228376773ae226932c46217 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 57e3f09f74fbb8df34a8114c5d2309c1fdec3d35 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    74fbaad0389b104624b935512e5ea4316bb19c6dd5b6cf0f99ae63213db8e9ba wbp2-bench.sh \
    84c89e532aa882ec9ee41df8cc3a7976e97de9a2e6051946cf44641677d2eecb wbp2-load.py \
    70f71f98406f9f70d40441cb1936ae8ea69fc30b994d99e552859b71ce5333ef wbp2-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
sh -n wbp2-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l wbp2-bench.sh
```

Expected: the three `OK` lines; three entries, exactly `100755 wbp2-bench.sh`, `100644 wbp2-load.py`, `100644 wbp2-report.py`; three `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`. `wbp2-load.py` is WBP2's, unchanged (same sha256). The driver is POSIX sh: run it with `sh` (dash), as below.

### 2. Trees and release builds (about 15 min)

```
T=/var/tmp/task-1121-wbp2v2; L=/var/log/task-1121-wbp2v2
git init -q $T/repo && cd $T/repo && git fetch -q --no-tags --depth 2 https://github.com/tigerblue77/zfs wbp2v2-pr2 && git log --oneline -2 FETCH_HEAD
test "$(git rev-parse FETCH_HEAD)" = 5ddc61884852afd62a62754272485dd9dfbeb3d4 && echo S commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 2b7990879815225c315aeb7950c037a7c859ffae && echo S tree OK
test "$(git rev-parse FETCH_HEAD~1)" = a1daebfec3f5ca46ac261ef339efdf0dbeae997c && echo M commit OK
test "$(git rev-parse 'FETCH_HEAD~1^{tree}')" = 0b23c141b417a8542546e1f7a32662bc82bc0f92 && echo M tree OK
git diff --numstat FETCH_HEAD~1 FETCH_HEAD
git worktree add -q --detach $T/M a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git worktree add -q --detach $T/S 5ddc61884852afd62a62754272485dd9dfbeb3d4
printf '%s  %s\n' \
    05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86 $T/M/module/os/linux/zfs/vdev_disk.c \
    fcdb2ff654a9bef0237c1404f6846de2cc795e3ff1c3ff3b910b3e23bd1fb85c $T/S/module/os/linux/zfs/vdev_disk.c \
    2deee3008a535d6ffe3c645fed8b68ae18bb0609f6617772ed05c1c40439310e $T/M/man/man4/zfs.4 \
    e451ea1ccc5145df608e5220887d6aff30ed852c4e07a32d1973069dd08cb184 $T/S/man/man4/zfs.4 > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```

Four `OK` lines, the numstat `47 1 man/man4/zfs.4` and `46 0 module/os/linux/zfs/vdev_disk.c`, and four `OK` from `sha256sum -c`. Then build the two at once, `make -j4` each, detached:

```
T=/var/tmp/task-1121-wbp2v2; L=/var/log/task-1121-wbp2v2
setsid nohup env T=$T L=$L sh -c 'for a in M S; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j4 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Poll `cat $L/build.log` until `build-done`. Then:

```
T=/var/tmp/task-1121-wbp2v2; L=/var/log/task-1121-wbp2v2
cat $L/build.log
for a in M S; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko) rt_mask=$(modinfo -F parm $T/$a/module/zfs.ko | grep -c '^zfs_vdev_disk_rt_mask:')"; done
grep -h 'warning:' $L/build-*.log | grep -E 'vdev_disk\.c|blkdev_compat\.h' | sort -u; grep -iE 'GPL|undefined' $L/build-S.log | head -5
```

Expected: rc 0 twice; modules built for `uname -r`; two different srcversions (equal: stop and report), recorded (M's is not WBP2's `65D8C0C24A9B688639F0721`: `module/zfs/zfs_vnops.c` changed since cde5d598eb4); `rt_mask=0` for M and `rt_mask=1` for S (anything else: stop and report). Quote any warning in those files and any modpost line about GPL or undefined symbols (none expected: the change uses no exported symbol). On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup and flag check (about 15 min, functional)

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh setup > $L/setup.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `setup done` or `STOP`. Then `cat $L/setup.nohup $L/setup-devices.txt $L/setup-cgroup.txt; cat $L/setup-zpool-list.txt`. Expected: `setup done: ...` with the scenarios (`a c d e` on WBP2's VM, `b` too where the kernel has `io.prio.class`), and the device line `sched ... [mq-deadline] ...`, `rot 1`, `wbt_lat_usec 0`, `nr_requests 64`, `completion_nsec 8000000`, `hw_queue_depth 4`, `irqmode 2`, `memory_backed 1`; in `setup-cgroup.txt` the root's `subtree_control` before, whether the kit wrote `+io`, the cgroup files, the scenarios, and the trace instance's filter (`dev == <number>`), clock `[mono]` and size. Then `timeout 30 sudo -n /var/tmp/task-1121-wbp2v2/M/zpool list -H -o name` must print `wbp2p` only. If setup stops, run `teardown` before trying once more.

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh flags > $L/flags.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `flags done` or `STOP` (about 6 min: every scenario for 8 s on each arm). Then `cat $L/flags.nohup $L/flags.txt`, `grep -h '^zfs_vdev_disk_rt_mask' $L/raw/F/*/*/layout.txt | sort | uniq -c`, and quote `grep -m 3 'block_rq_insert: .*\[wbp2-vr' $L/raw/F/<arm>/e/trace.txt` and `grep -m 3 'block_rq_insert: .*\[wbp2-a' $L/raw/F/<arm>/a/trace.txt` for M, S1 and S3 (raw lines with their class). **Stop and report** (after `teardown`) if: a "zfs threads insert requests" check is a surprise; M's "agg (ionice -c3): every request idle" shows 0 requests (no user thread submits, so nothing here can show an effect); M shows a real-time request; S1 or S3 fail "vic's reads mostly real-time" in scenario e (the parameter does nothing: the arms would measure nothing); the layout lines do not read `absent` for M, `1` for S1 and `3` for S3; a run lost trace events or has unparsed lines (identity table). Any other "surprise" in flags.txt is a finding (an agg request that is not idle in a on S1 or S3 is the first harm coming back): quote it in the note to main, and go on.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's kernel, cmdline, configs, cgroup and tracefs lines; the build table with rc, times, vermagic, srcversions and `rt_mask`; `setup-devices.txt`, `setup-cgroup.txt` and the setup line; `flags.txt` whole and the quoted trace lines) and, as the very last line, exactly:

`TASK-1121-WBP2V2 READY-TO-MEASURE (about 75 to 85 min alone on the host, at most 90). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1121-WBP2V2`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1121-WBP2V2`, go to step 7 without running anything. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 5 min)

```
L=/var/log/task-1121-wbp2v2
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n sh ./wbp2-bench.sh quiet 60; tail -1 $L/driver.log
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so.

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n env RAMP=2 MEAS=6 sh ./wbp2-bench.sh pass W "M S1 S3" > $L/pass-W.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass W done` or `STOP`. Then:

```
L=/var/log/task-1121-wbp2v2
grep -h '^srcversion' $L/raw/W/*/*/layout.txt | sort | uniq -c; grep -h '^zfs_vdev_disk_rt_mask' $L/raw/W/*/*/layout.txt | sort | uniq -c; grep -h -o 'wbt_lat_usec [0-9]*' $L/raw/W/*/*/layout.txt | sort | uniq -c; timeout 30 sudo -n $(cat /var/tmp/task-1121-wbp2v2/cur-tree)/zpool list -H -o name
```

Expected: two srcversions, the step 2 ones, M's on as many runs as there are scenarios and S's on twice as many; `zfs_vdev_disk_rt_mask=absent`, `=1` and `=3` each on as many runs as there are scenarios; every `wbt_lat_usec 0`; `wbp2p` only. If one differs, run `teardown` and report.

### 6. The passes (about 55 to 75 min)

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh pass P1 "M S1 S3" > $L/pass-P1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P1 done` or `STOP`. Then `python3 -I $L/bin/wbp2-report.py $L/raw P > $L/summary.md; echo rc=$?` and check, before going on, in its "Class checks" and identity tables: no run lost trace events, every "zfs threads insert requests" is as expected (if not: stop, `teardown`, report). Note also, without stopping, per scenario and arm the vic read p99, vic fsync mean and agg read MB/s of P1, and every "surprise". Then P2 to P6 as one detached chain:

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n sh -c 'for p in "P2:S1 S3 M" "P3:S3 M S1" "P4:S3 S1 M" "P5:M S3 S1" "P6:S1 M S3"; do sh ./wbp2-bench.sh pass "${p%%:*}" "${p#*:}" || exit 1; done; echo chain-done' > $L/chain-1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P6 done` or `STOP`; after each `pass Pn done`, `timeout 30 sudo -n $(cat /var/tmp/task-1121-wbp2v2/cur-tree)/zpool list -H -o name` must print `wbp2p` only. Then the **time rule**:

```
L=/var/log/task-1121-wbp2v2
awk -v go="$(cat $L/go.txt)" -v now="$(date +%s)" '
function sec(t, a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "start" { s[$3] = sec($1) }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "done" { e = sec($1) - s[$3]; if (e < 0) e += 86400; d += e; n++ }
END { d = d / n; for (k = 2; k > 0; k--) if (now + k * d + 120 <= go + 5400) break
    printf "d %.0f s over %d passes, now %d, GO+90 %d, k %d: run %s\n", d, n, now, go + 5400, k, k == 2 ? "P7 P8" : k == 1 ? "P7" : "none" }' $L/driver.log | tee $L/time-rule.txt
```

Expected `over 6 passes`. Then the passes it leaves, P7 then P8, the same way (detached, polled):

```
L=/var/log/task-1121-wbp2v2
cd $L/bin && setsid nohup sudo -n sh -c 'sh ./wbp2-bench.sh pass P7 "M S1 S3" && sh ./wbp2-bench.sh pass P8 "S1 S3 M"; echo chain-done' > $L/chain-2.nohup 2>&1 < /dev/null &
```

(with k = 1, only `sh ./wbp2-bench.sh pass P7 "M S1 S3"`). When the last run has ended, write in your chat the line `TASK-1121-WBP2V2 MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 7 without waiting.

### 7. Report data, teardown, end

```
L=/var/log/task-1121-wbp2v2; cd $L/bin
python3 -I ./wbp2-report.py $L/raw P > $L/summary.md; echo rc=$?
python3 -I ./wbp2-report.py $L/raw W > $L/summary-w.md; echo rc=$?
sudo -n sh ./wbp2-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /dev/wbp2nb0 2>&1; ls /sys/kernel/config/nullb 2>&1; ls -d /sys/fs/cgroup/wbp2 2>&1; sudo -n ls -d /sys/kernel/tracing/instances/wbp2 2>&1
sudo -n dmesg | sed -n '/task-1121-wbp2v2: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|percpu ref' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw && sudo -n rm -rf /var/tmp/task-1121-wbp2v2 && sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: no module loaded, no `/dev/wbp2nb0`, no null_blk device, no `wbp2` cgroup or trace instance (the teardown line says `cgroup removed, device removed`), no kernel line (report any, with the step it happened in). `/etc/zfs/zpool.cache`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Keep `/var/log/task-1121-wbp2v2` (raw data included). If the kit wrote `+io` to the root's `cgroup.subtree_control`, leave it and say so.

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build, two equal srcversions, or `rt_mask` other than 0 for M and 1 for S; a setup that fails twice or a device not as expected; step 3's stop list; a failed check after W or P1; a driver `STOP:` (teardown, then report); a pool this task did not create; a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING`, `percpu ref` or hung task in the kernel log (capture, report, no reboot); more than 2 h 30 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict or a class check against the expectations, a scenario the kernel lacks, or a pass dropped under the time rule is a finding, not a stop.

## Report

`/var/log/task-1121-wbp2v2/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from summary.md and flags.txt (recompute nothing by eye):
   a. **What the bios carry**: the "Class checks" rows, each "as expected" or "surprise", and per scenario and arm the vic, agg and zfs rows of "Request classes"; the raw trace lines quoted at step 3.
   b. **First harm, ionice (scenario a)**: S1/M and S3/M verdicts on every metric with the expected column; whether every agg request stayed idle on S1 and S3.
   c. **Second harm, cgroup control (b, c, d)**: S1/M and S3/M verdicts on agg read MB/s and agg write ops/s with the expected column; the blkcg rows (both cgroups charged on every arm, agg's charged share); the kworker rows in c; agg's `cost.usage` in d; vic's latencies.
   d. **No control (scenario e)**: S1/M, S3/M and S3/S1 verdicts on every metric (none is expected: this is the measurement), with the classes of vic's, agg's and zfs's reads and writes.
   e. **The owner's question**, one table: scenario | what S1 changes against M | what S3 changes against M | what the write bit adds (S3/S1) | harms as expected or surprise.
   f. **Lead "user threads submit I/O they did not issue"**: per scenario on M, the W column of the vic and agg rows of "Request classes", against the W column of the zfs row.
   g. **Noise**: the quiet check; per run the steal and load1; any run with steal above 1 %; the arm order per pass; the time of GO and what the time rule dropped.
2. summary.md whole, then summary-w.md's identity table only, then flags.txt whole.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, the kernel config lines, cgroup and tracefs lines, kit checks, code checks and file hashes, build times and rc, vermagic, srcversions, `rt_mask`, setup-devices.txt, setup-cgroup.txt, the setup line; per pass its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, device, cgroups, trace instance, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0 and 7, then End.
