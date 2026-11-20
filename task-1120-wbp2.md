TASK-1120-WBP2 (from the coordinator session "main"): performance, on one of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB), about 2 h on the VM plus the wait for main's GO, at most 2 h 30 without that wait. Its measured part (steps 5 and 6, about 65 to 85 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 4 are functional and may run while other VMs work. Three release builds fetched from the fork branch `wbp2-pr2` (upstream master cde5d598eb4, its first commit, both commits), one null_blk device, two cgroups, four scenarios per tree, six to eight passes. No reboot is pre-approved. Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1120-wbp2/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1120-wbp2`, `L=/var/log/task-1120-wbp2`; the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a "surprise" in a report table is a finding (then it is reported, not a stop).

## Why

Every bio ZFS submits is built by `vdev_bio_alloc()` (`module/os/linux/zfs/vdev_disk.c`). ZFS issues pool I/O from whatever thread is at hand: the zio taskq threads (kernel threads, root cgroup, no I/O context) submit most of it, but a user thread submits its own reads inline and, through `vdev_queue_io()`, whichever queued I/O the vdev queue releases at that moment (another workload's read, a transaction group write). On 6.12 two things of the submitting thread then reach the bio:

- **I/O priority.** `submit_bio()` gives a bio whose class is NONE the thread's priority: the one `ionice`/`ioprio_set()` gave it, or one derived from its nice value if it has an I/O context of class NONE; a thread without an I/O context (kernel threads, most user threads) leaves the bio NONE (`__get_task_ioprio()`). Then the blkcg's `io.prio.class` policy, if any, is applied (`blkcg_set_ioprio()`, the lower priority wins). mq-deadline serves NONE as best-effort, and serves the idle class only when no best-effort request is queued (up to its 10 s aging).
- **blkcg.** From 5.18, `bio_alloc(bdev, ...)` charges the bio to the submitting thread's cgroup: io.stat counts it there, and `io.max` (blk-throttle), `io.latency` and `io.weight` (iocost) apply to it. From 5.0 to 5.17 ZFS's own `bio_set_dev()` charged every bio to the root blkcg.

The branch has two commits on cde5d598eb4 (upstream master: c913da86156 plus upstream PR #19910, CI only):

- **C1 = 65a157266fb**: `bi_ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_NORM)` (best-effort level 4) on every bio, so that a thread's ionice no longer demotes I/O it merely issues for the pool. A cgroup's `io.prio.class` still applies on top.
- **C2 = 5773193b249**: every bio charged to the root blkcg again, by lending the root blkg to the bio, cloning it with `bio_alloc_clone()` (exported to all modules) and swapping the two pointers; a new configure check `HAVE_BIO_ALLOC_CLONE`. Behaviour change: from 5.18, `io.max`, `io.weight` and `io.latency` stop limiting a container's reads from a pool (its writes were never limited, taskq threads issue them), and `io.prio.class` stops applying.

The owner must decide whether to open the PR with C1 alone or with both. That hangs on what each commit changes in practice on 6.12. This task measures what the ZFS bios actually carry (the ioprio of every request inserted into the device's queue, by inserting task; the blkcg charge of each cgroup) and what it does to a "victim" next to an "aggressor" under four controls.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
| --- | --- | --- | --- |
| C2, both commits | `wbp2-pr2` | `5773193b249b1b9d061c88016512a9a233c3470b` | `55c6c29d3fbeee8ca7e57472d585914df38381ce` |
| C1 (C2~1) | - | `65a157266fb51bd579c6b43c4080b17e0c1342d3` | `70325e99f337bb926ceb27f5756d9c2abf42249d` |
| M, upstream master (C2~2) | - | `cde5d598eb44b5d6efe8167666d988a496346657` | `d70a147b0775bf4bd216072a8b05738b603d6849` |
| kit | `vm-kit-wbp2` | `5611bb673a1371f4798b1d1786ea596902a35092` (no parent) | `6057bf7d35c1b78384650c0f5daf566835addc22` |

`git diff --numstat` from M to C1: `module/os/linux/zfs/vdev_disk.c` 12/0; from C1 to C2: `config/kernel-bio.m4` 37/0, `module/os/linux/zfs/vdev_disk.c` 38/0. M's module source is c913da86156's: ZVH, ROV3 and VQ1 recorded srcversion `65D8C0C24A9B688639F0721` for it.

Builds: release (`./configure` with no option), as VQ1 for its timing. There is no functional run besides step 3's flag check, so no debug build.

## The rig (`wbp2-bench.sh setup`)

One memory-backed null_blk device `wbp2nb0` (1536 MiB, 4K blocks, 8 ms per request by timer, 4 tags, no write cache), mq-deadline with `nr_requests` 64 (so that requests wait in the scheduler, where their class decides the order), `rotational` 1, blk-wbt off (`wbt_lat_usec` 0: it is PR1's subject and would put the same threads to sleep for another reason). A pool `wbp2p` on it (ashift 12, compression off, recordsize 128K, `primarycache=metadata` so that every read reaches the device, `cachefile=none`, mounted under `$T/mnt`), datasets `v` and `a`, each with a 256 MiB file `r` and a 128 MiB file `w`. Cgroups `/sys/fs/cgroup/wbp2/vic` and `/sys/fs/cgroup/wbp2/agg` with the io controller (the kit writes `+io` to the root's `cgroup.subtree_control` only if it is missing, and says so). A tracefs instance `wbp2` traces `block_rq_insert` on the device only (rwbs, bytes, ioprio, inserting task). `zfs_arc_max` 1 GiB.

**One scenario** (`run_one`, 5 s ramp + 30 s measured): in `vic`, a reader of 128K records at random offsets of `v/r`, 40 per second (`wbp2-vr`), and a writer of 128K records to `v/w`, 20 per second, with an `fdatasync()` after every two (`wbp2-vw`); in `agg`, a closed-loop reader of 128K records of `a/r` with 8 threads (`wbp2-ar`) and a writer of 16K into the 128K records of `a/w`, 100 per second, never synced (`wbp2-aw`: each write reads its record first, from the writer's thread). Each thread names itself so (its comm). During the 30 s: the trace; at both ends: io.stat of both cgroups, the txgs kstat, diskstats, /proc/stat. After: the settings cleared, `zpool sync`, the kernel log checked.

| Scenario | Setting on `agg` |
| --- | --- |
| a | its two processes under `ionice -c3` (idle) |
| b | `io.prio.class` `idle` |
| c | `io.max` `riops=40 wiops=40` on the device |
| d | iocost on the device (linear model 150 IOPS, 20 MiB/s; vrate fixed at 100 %), `io.weight` 1 for agg, 10000 for vic |

A scenario whose files the kernel lacks (`io.prio.class`: CONFIG_BLK_CGROUP_IOPRIO; `io.max`: CONFIG_BLK_DEV_THROTTLING; `io.cost.qos`, `io.weight`: CONFIG_BLK_CGROUP_IOCOST) is left out by the kit and listed in `setup-cgroup.txt`; that is a finding, not a stop.

**One pass** (`wbp2-bench.sh pass`): for each arm in the pass's order: the pool exported, the arm's modules loaded if the tree changes, the pool imported (`zpool import -o cachefile=none -d /dev/wbp2nb0 wbp2p`, nothing else), then the scenarios a, b, c, d. About 42 s per scenario and 20 s per arm switch: **about 10 min per pass**.

## Passes

The arm orders form a Williams design for three arms: P1 to P6 put each arm twice in each position and let each arm follow each other arm exactly twice. P7 and P8 repeat P1 and P2.

| Pass | Arm order | |
| --- | --- | --- |
| F | M C1 C2 | step 3's flag check, every scenario, 2 s + 8 s, raw traces kept, not in the verdicts |
| W | M C1 C2 | warm-up after GO, `RAMP=2 MEAS=6`, not reported |
| P1 | M C1 C2 | |
| P2 | C1 C2 M | |
| P3 | C2 M C1 | |
| P4 | C2 C1 M | |
| P5 | M C2 C1 | |
| P6 | C1 M C2 | time rule after it |
| P7 | M C1 C2 | |
| P8 | C1 C2 M | |

Projected: quiet check 1 min, W about 4 min, P1 to P6 about 60 min: **about 65 min from GO** to the end of P6, about 85 min with P7 and P8.

**Time rule** (keeps the measured part under 90 min). After P6, from driver.log: d = the mean duration of P1 to P6; run the largest number k of P7, P8 (in that order, k from 2 down to 0) for which now + k × d + 2 min is at most GO + 90 min. Six passes are the fewest that can give p <= 0.05. Step 6 gives the command; it writes `$L/time-rule.txt`.

## Written before the run

- **Verdict per scenario, pair and metric** (`wbp2-report.py raw P`): over the passes where both arms ran the scenario, ratio = a / b in the same pass; "lower" if the geometric mean of the ratios is 0.97 or less and the exact sign-flip permutation p is 0.05 or less, "higher" if 1.03 or more with the same p, otherwise "no difference shown". The words carry no judgement: a higher agg throughput under a limit means the limit stopped applying. Pairs: C1/M (what commit 1 changes), C2/C1 (what commit 2 adds), C2/M. Metrics: vic read p50 and p99, vic fsync mean and p90, agg read MB/s, agg write ops/s and p99, txg sync time mean.
- **Request classes** ("Request classes by inserting group"): every request inserted into the device's queue during the windows, by inserting group (`vic`: wbp2-vr and wbp2-vw; `agg`: wbp2-ar and wbp2-aw; `zfs`: z_* and txg_* threads; `kworker`; `other`), kind and class (`none`, `be4`, `idle0`, ...). The report sets each group's expected class from the code (below) and writes "as expected" or "surprise: N of M not X".
- **blkcg charge** ("blkcg charge and controller state"): the io.stat deltas of vic and agg per run, the share of agg's read bytes charged to agg, iocost's `cost.wait` for agg; "as expected" or "surprise".
- **Noise**: steal above 1 % in a run is listed with its pass; load1 counts the run's own threads, listed, not judged; trace events lost or lines not parsed are listed per run (0 expected).

**Expected, from reading the code (report what you see, not this).** Requests: on M the zio taskq threads have no I/O context, so their requests are `none` (C1's commit message says they submit at best-effort level 0; the trace decides); the streams' threads are `none` too unless `ionice -p` (recorded per stream in `layout.txt`) says otherwise, and the report then expects that class.

| Scenario | M | C1 | C2 |
| --- | --- | --- | --- |
| a ionice -c3 | agg's requests `idle0`, vic's and zfs's `none`; vic and agg charged (rios above 0) | every request `be4`; charged as M | every request `be4`; vic and agg io.stat 0 |
| b io.prio.class idle | agg `idle0` (the policy), others `none`; charged | agg `idle0` (the policy overrides `be4`), others `be4`; charged | every request `be4` (the root blkcg has no policy); io.stat 0 |
| c io.max | every class `none`; bios blk-throttle held are inserted by a `kworker`; agg's own reads limited, those other threads submit for it not | as M with `be4` | no `kworker` request; io.stat 0; agg read MB/s far above 40 × 128K |
| d iocost | `none`; agg `cost.wait` above 0 | `be4`; agg `cost.wait` above 0 | `be4`; io.stat 0, no `cost.wait` |

Effects (verdicts), the firm ones (the report's "expected" column): a: C2/C1 no difference on every metric (no cgroup control in play), C1/M agg read MB/s higher (its own reads leave the idle class); b, c, d: C1/M no difference on every metric (the cgroup control decides, and mq-deadline serves `none` as `be`), C2/C1 agg read MB/s higher (the control stops applying). Not known before the run: the vic latencies in every scenario. On M and C1 two effects pull in opposite directions: the control protects vic from agg's own I/O, but agg's threads also submit, at agg's priority or charged to agg, vic's queued reads and the transaction group's writes (priority inversion); in c a throttled read also keeps its vdev queue slot while blk-throttle holds it. Which dominates on 6.12 is what the owner needs. This rig cannot show the effect on BFQ (it takes the class from the task, not the bio) or on a real disk's own queue.

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed (python3, util-linux's `ionice` and the build toolchain are all the kit needs).
- The pool lives on `/dev/wbp2nb0` only; the driver creates it and the device and removes them. Touch no other pool, disk or null_blk device; never `zpool import -a`; the driver imports only its own pool, by name, from its own device, with `cachefile=none`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks: during ROV3 it imported VM 105's exported measurement pool `tank`, which may still be there, exported. Nothing in this task runs it, but after setup and after every pass `zpool list` must show no pool but `wbp2p`; the driver checks it at every arm switch and stops (`STOP: a pool this task did not create is imported`). A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
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
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -l /dev/zfs 2>&1; losetup -a; ls -d /dev/wbp2nb0 /var/tmp/task-1120-wbp2 /var/log/task-1120-wbp2 /sys/fs/cgroup/wbp2 2>&1; ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1
command -v python3 git make gcc nm ionice; python3 --version; id zts; pgrep -a -x zed || echo NO-ZED; command -v bpftrace blktrace || echo no-bpftrace-no-blktrace
sudo -n ls -d /sys/kernel/debug/block /sys/kernel/tracing/events/block/block_rq_insert 2>&1; sudo -n grep -c ioprio /sys/kernel/tracing/events/block/block_rq_insert/format
grep -E 'CONFIG_BLK_CGROUP=|CONFIG_BLK_CGROUP_IOPRIO|CONFIG_BLK_CGROUP_IOCOST|CONFIG_BLK_DEV_THROTTLING=|CONFIG_BLK_DEV_NULL_BLK=|CONFIG_MQ_IOSCHED_DEADLINE|CONFIG_BLK_WBT=' /boot/config-$(uname -r)
cat /sys/fs/cgroup/cgroup.controllers /sys/fs/cgroup/cgroup.subtree_control; ls /sys/fs/cgroup/io.cost.qos /sys/fs/cgroup/io.cost.model 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl or null_blk module is loaded, a null_blk device exists or `/dev/zfs` exists; `losetup -a` is not empty; `/dev/wbp2nb0`, `$T`, `$L` or `/sys/fs/cgroup/wbp2` exists; a `/var/tmp/file-vdev*` or `/var/tmp/dev-*` is left; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `CONFIG_BLK_CGROUP` is not `y` or `io` is not in `cgroup.controllers`; `CONFIG_BLK_DEV_NULL_BLK` or `CONFIG_MQ_IOSCHED_DEADLINE` is missing; the `block_rq_insert` format has no `ioprio` (count 0); python3 is older than 3.8; `ionice` or `nm` is missing; a build, test or load runs. Note, without stopping: the three cgroup configs (a missing one drops a scenario), the zed line (do not start or stop zed), bpftrace/blktrace (not used either way: the kit uses tracefs, so that every VM measures the same way). If `/sys/kernel/debug/block` is missing, `sudo -n mount -t debugfs none /sys/kernel/debug` and say so.

`/etc/zfs/zpool.cache`: absent is expected. If it exists with size 0, move it aside and say so: `L=/var/log/task-1120-wbp2; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0`. If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

```
L=/var/log/task-1120-wbp2; T=/var/tmp/task-1120-wbp2
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit, fetched from the fork branch

```
L=/var/log/task-1120-wbp2; T=/var/tmp/task-1120-wbp2
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-wbp2
test "$(git rev-parse FETCH_HEAD)" = 5611bb673a1371f4798b1d1786ea596902a35092 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 6057bf7d35c1b78384650c0f5daf566835addc22 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    476f2daf51d32649e608513c4dca4961823b88a9a1bdbeb889500181b56702a5 wbp2-bench.sh \
    84c89e532aa882ec9ee41df8cc3a7976e97de9a2e6051946cf44641677d2eecb wbp2-load.py \
    b97ed83df4e39c15f431f36fe1a1f81e3b2f9bf73763aaec8c244919b49cae0d wbp2-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
sh -n wbp2-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l wbp2-bench.sh
```

Expected: the three `OK` lines; three entries, exactly `100755 wbp2-bench.sh`, `100644 wbp2-load.py`, `100644 wbp2-report.py`; three `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`. The driver is POSIX sh: run it with `sh` (dash), as below.

### 2. Trees and release builds (about 15 min)

```
T=/var/tmp/task-1120-wbp2; L=/var/log/task-1120-wbp2
git init -q $T/repo && cd $T/repo && git fetch -q --no-tags --depth 3 https://github.com/tigerblue77/zfs wbp2-pr2 && git log --oneline -3 FETCH_HEAD
test "$(git rev-parse FETCH_HEAD)" = 5773193b249b1b9d061c88016512a9a233c3470b && echo C2 commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 55c6c29d3fbeee8ca7e57472d585914df38381ce && echo C2 tree OK
test "$(git rev-parse FETCH_HEAD~1)" = 65a157266fb51bd579c6b43c4080b17e0c1342d3 && echo C1 commit OK
test "$(git rev-parse 'FETCH_HEAD~1^{tree}')" = 70325e99f337bb926ceb27f5756d9c2abf42249d && echo C1 tree OK
test "$(git rev-parse FETCH_HEAD~2)" = cde5d598eb44b5d6efe8167666d988a496346657 && echo M commit OK
test "$(git rev-parse 'FETCH_HEAD~2^{tree}')" = d70a147b0775bf4bd216072a8b05738b603d6849 && echo M tree OK
git diff --numstat FETCH_HEAD~2 FETCH_HEAD~1; git diff --numstat FETCH_HEAD~1 FETCH_HEAD
git worktree add -q --detach $T/M cde5d598eb44b5d6efe8167666d988a496346657
git worktree add -q --detach $T/C1 65a157266fb51bd579c6b43c4080b17e0c1342d3
git worktree add -q --detach $T/C2 5773193b249b1b9d061c88016512a9a233c3470b
printf '%s  %s\n' \
    05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86 $T/M/module/os/linux/zfs/vdev_disk.c \
    3b049aa67134bb7fbad7aced041c9d05e7fbbfec1eb35e6cd49410c861b17d3c $T/C1/module/os/linux/zfs/vdev_disk.c \
    7d312df8204e5a33fbefc805e4dc64d1c16a40c615d28a47a18884ee45f48a31 $T/C2/module/os/linux/zfs/vdev_disk.c \
    deed60b014160a817efcf561b64109c5c0802d8481fef28ba07f1f5e31446a0f $T/C1/config/kernel-bio.m4 \
    fb1ade568dea217140a6cc98642947061b3d178aae80992bebde13cf4b7e0027 $T/C2/config/kernel-bio.m4 > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```

Six `OK` lines, the numstats of the table above (`12 0 module/os/linux/zfs/vdev_disk.c`; then `37 0 config/kernel-bio.m4` and `38 0 module/os/linux/zfs/vdev_disk.c`), and five `OK` from `sha256sum -c`. Then build the three at once, `make -j3` each, detached:

```
T=/var/tmp/task-1120-wbp2; L=/var/log/task-1120-wbp2
setsid nohup env T=$T L=$L sh -c 'for a in M C1 C2; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j3 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Poll `cat $L/build.log` until `build-done`. Then:

```
T=/var/tmp/task-1120-wbp2; L=/var/log/task-1120-wbp2
cat $L/build.log
for a in M C1 C2; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko) clone=$(grep -c '^#define HAVE_BIO_ALLOC_CLONE 1' $T/$a/zfs_config.h) imports=$(nm -u $T/$a/module/zfs.ko | grep -cw bio_alloc_clone)"; done
grep -h 'warning:' $L/build-*.log | grep -E 'vdev_disk\.c|blkdev_compat\.h|kernel-bio' | sort -u; grep -iE 'GPL|undefined' $L/build-C2.log | head -5
```

Expected: rc 0 three times; modules built for `uname -r`; three different srcversions (two equal: stop and report), M's expected to be `65D8C0C24A9B688639F0721` (another value is recorded and reported, not a stop); `clone=0 imports=0` for M and C1, `clone=1 imports=1` for C2 (the configure check found `bio_alloc_clone()` exported to non-GPL modules and the module uses it; anything else: stop and report). Quote any warning in those files and any modpost line about GPL or undefined symbols. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup and flag check (about 12 min, functional)

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh setup > $L/setup.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `setup done` or `STOP`. Then `cat $L/setup.nohup $L/setup-devices.txt $L/setup-cgroup.txt; cat $L/setup-zpool-list.txt`. Expected: `setup done: ...` with the scenarios, and the device line `sched ... [mq-deadline] ...`, `rot 1`, `wbt_lat_usec 0`, `nr_requests 64`, `completion_nsec 8000000`, `hw_queue_depth 4`, `irqmode 2`, `memory_backed 1`; in `setup-cgroup.txt` the root's `subtree_control` before, whether the kit wrote `+io`, the cgroup files, `scenarios: a b c d` (or fewer: a finding), and the trace instance's filter (`dev == <number>`), clock `[mono]` and size. Then `timeout 30 sudo -n /var/tmp/task-1120-wbp2/M/zpool list -H -o name` must print `wbp2p` only. If setup stops, run `teardown` before trying once more.

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh flags > $L/flags.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `flags done` or `STOP` (about 5 min: every scenario for 8 s on each tree). Then `cat $L/flags.nohup $L/flags.txt`, and quote `grep -m 3 'block_rq_insert: .*\[wbp2-a' $L/raw/F/<arm>/a/trace.txt` for M, C1 and C2 (raw lines of agg's requests with their class). **Stop and report** (after `teardown`) if: a `zfs` row shows 0 requests; the `agg` row of M in scenario a shows 0 requests (no user thread submits, so nothing in this task can show an effect); a run lost trace events or has unparsed lines (identity table). Any other "surprise" in flags.txt is a finding: quote it in the note to main, and go on.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's kernel, cmdline, configs, cgroup and tracefs lines; the build table with rc, times, vermagic, srcversions, clone and imports; `setup-devices.txt`, `setup-cgroup.txt` and the setup line; `flags.txt` whole) and, as the very last line, exactly:

`TASK-1120-WBP2 READY-TO-MEASURE (about 65 to 85 min alone on the host, at most 90). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1120-WBP2`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1120-WBP2`, go to step 7 without running anything. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 5 min)

```
L=/var/log/task-1120-wbp2
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n sh ./wbp2-bench.sh quiet 60; tail -1 $L/driver.log
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so.

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n env RAMP=2 MEAS=6 sh ./wbp2-bench.sh pass W "M C1 C2" > $L/pass-W.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass W done` or `STOP`. Then:

```
L=/var/log/task-1120-wbp2
grep -h '^srcversion' $L/raw/W/*/*/layout.txt | sort | uniq -c; grep -h -o 'wbt_lat_usec [0-9]*' $L/raw/W/*/*/layout.txt | sort | uniq -c; timeout 30 sudo -n $(cat /var/tmp/task-1120-wbp2/cur-tree)/zpool list -H -o name
```

Expected: three srcversions, the step 2 ones, each on as many runs as there are scenarios; every `wbt_lat_usec 0`; `wbp2p` only. If one differs, run `teardown` and report.

### 6. The passes (about 60 to 80 min)

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n sh ./wbp2-bench.sh pass P1 "M C1 C2" > $L/pass-P1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P1 done` or `STOP`. Then `python3 -I $L/bin/wbp2-report.py $L/raw P > $L/summary.md; echo rc=$?` and check, before going on, in its "Request classes" and "blkcg charge" tables: no run lost trace events, every `zfs` row has requests (if not: stop, `teardown`, report). Note also, without stopping, per scenario and arm the vic read p99, vic fsync mean and agg read MB/s of P1, and every "surprise". Then P2 to P6 as one detached chain:

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n sh -c 'for p in "P2:C1 C2 M" "P3:C2 M C1" "P4:C2 C1 M" "P5:M C2 C1" "P6:C1 M C2"; do sh ./wbp2-bench.sh pass "${p%%:*}" "${p#*:}" || exit 1; done; echo chain-done' > $L/chain-1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P6 done` or `STOP`; after each `pass Pn done`, `timeout 30 sudo -n $(cat /var/tmp/task-1120-wbp2/cur-tree)/zpool list -H -o name` must print `wbp2p` only. Then the **time rule**:

```
L=/var/log/task-1120-wbp2
awk -v go="$(cat $L/go.txt)" -v now="$(date +%s)" '
function sec(t, a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "start" { s[$3] = sec($1) }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "done" { e = sec($1) - s[$3]; if (e < 0) e += 86400; d += e; n++ }
END { d = d / n; for (k = 2; k > 0; k--) if (now + k * d + 120 <= go + 5400) break
    printf "d %.0f s over %d passes, now %d, GO+90 %d, k %d: run %s\n", d, n, now, go + 5400, k, k == 2 ? "P7 P8" : k == 1 ? "P7" : "none" }' $L/driver.log | tee $L/time-rule.txt
```

Expected `over 6 passes`. Then the passes it leaves, P7 then P8, the same way (detached, polled):

```
L=/var/log/task-1120-wbp2
cd $L/bin && setsid nohup sudo -n sh -c 'sh ./wbp2-bench.sh pass P7 "M C1 C2" && sh ./wbp2-bench.sh pass P8 "C1 C2 M"; echo chain-done' > $L/chain-2.nohup 2>&1 < /dev/null &
```

(with k = 1, only `sh ./wbp2-bench.sh pass P7 "M C1 C2"`). When the last run has ended, write in your chat the line `TASK-1120-WBP2 MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 7 without waiting.

### 7. Report data, teardown, end

```
L=/var/log/task-1120-wbp2; cd $L/bin
python3 -I ./wbp2-report.py $L/raw P > $L/summary.md; echo rc=$?
python3 -I ./wbp2-report.py $L/raw W > $L/summary-w.md; echo rc=$?
sudo -n sh ./wbp2-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /dev/wbp2nb0 2>&1; ls /sys/kernel/config/nullb 2>&1; ls -d /sys/fs/cgroup/wbp2 2>&1; sudo -n ls -d /sys/kernel/tracing/instances/wbp2 2>&1
sudo -n dmesg | sed -n '/task-1120-wbp2: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|percpu ref' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw && sudo -n rm -rf /var/tmp/task-1120-wbp2 && sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: no module loaded, no `/dev/wbp2nb0`, no null_blk device, no `wbp2` cgroup or trace instance (the teardown line says `cgroup removed, device removed`), no kernel line (report any, with the step it happened in; a `percpu ref` or WARNING at the device's or the cgroups' removal would point at commit 2's blkg references). `/etc/zfs/zpool.cache`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Keep `/var/log/task-1120-wbp2` (raw data included). If the kit wrote `+io` to the root's `cgroup.subtree_control`, leave it and say so.

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build, two equal srcversions, or C2 without `clone=1 imports=1`; a setup that fails twice or a device not as expected; step 3's stop list; a failed check after W or P1; a driver `STOP:` (teardown, then report); a pool this task did not create; a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING`, `percpu ref` or hung task in the kernel log (capture, report, no reboot); more than 2 h 30 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict or a class against the expectations, a scenario the kernel lacks, or a pass dropped under the time rule is a finding, not a stop.

## Report

`/var/log/task-1120-wbp2/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from summary.md and flags.txt (recompute nothing by eye):
   a. **What the bios carry**: per scenario and tree, the "Request classes" rows (vic, agg, zfs, kworker), each "as expected" or "surprise"; the raw trace lines quoted at step 3.
   b. **blkcg charge**: the "blkcg charge" rows; agg's charged share of its read bytes on M and C1; `cost.wait` in d.
   c. **Commit 1 alone (C1/M)**, per scenario: the verdicts, with the expected column; in a, what ionice still does to agg and to vic.
   d. **Commit 2 (C2/C1)**, per scenario: the verdicts; in b, c and d, does the control still limit the container's reads and writes to the pool (agg read MB/s and write ops/s, the kworker rows, io.stat), and what vic's latencies do.
   e. **The owner's question**, one table: scenario | what C1 changes against M | what C2 changes against C1 | as expected or surprise.
   f. **Noise**: the quiet check; per run the steal and load1; any run with steal above 1 %; the arm order per pass; the time of GO and what the time rule dropped.
2. summary.md whole, then summary-w.md's identity table only, then flags.txt whole.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, the kernel config lines, cgroup and tracefs lines, kit checks, code checks and file hashes, build times and rc, vermagic, srcversions, clone and imports, setup-devices.txt, setup-cgroup.txt, the setup line; per pass its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, device, cgroups, trace instance, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0 and 7, then End.
