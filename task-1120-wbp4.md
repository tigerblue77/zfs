TASK-1120-WBP4 (from the coordinator session "main"): performance plus a short functional part, on one of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB), about 2 h 10 on the VM plus the wait for main's GO, at most 2 h 45 without that wait. Its measured part (steps 6 and 7, about 50 to 80 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 5 are functional and may run while other VMs work. Fork branch `wbp4-pr4` (one commit on upstream master cde5d598eb4): two release builds (master, PR4) for the measurement, a debug build of PR4 for a ztest smoke and the ZTS trim group; four null_blk devices with discard, a manual trim and autotrim next to a sync writer, six to twelve passes. No reboot is pre-approved. Nothing is published on GitHub, no credential files, fstrim at the end. Report: `/var/log/task-1120-wbp4/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1120-wbp4`, `L=/var/log/task-1120-wbp4`; the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a "surprise" in a report table is a finding (then it is reported, not a stop).

## Why

A TRIM zio that `vdev_queue_io()` queues (the TRIM class is at `zfs_vdev_trim_max_active`, 2) is issued later by whichever thread frees a slot: in `vdev_queue_io_done()` that is the completion thread of a TRIM, almost always `z_trim_int`, one thread per pool; `vdev_queue_io()` can also hand a queued TRIM to the thread of an arriving one. Submitting a discard can sleep: blk-wbt throttles every `REQ_OP_DISCARD` at its background limit, whatever the bio flags, so the submitter waits in `wbt_wait()` until the disk's earlier discards complete. While `z_trim_int` sleeps there for one disk, the completions of TRIMs to every other disk of the pool wait behind it, holding their queue slots. Only TRIMs that go through the queue are concerned: with the default vdev property `scheduler=auto`, those to non-rotational disks bypass it; on rotational disks with discard (SMR, some RAID/SAN LUNs, thin-provisioned virtual disks marked rotational) they do not.

PR4 = `1fd3fd8c642` (27 lines, no tunable): such TRIMs are handed to the TRIM issue taskq (`z_trim_iss`, 4 threads per pool) through a new `zio_vdev_io_reissue_async()`, cutting in line as other requeued starts do, instead of running inline on `z_trim_int` (or on the arriving zio's thread). The slot is still taken at selection and released at completion. Expected cost: one context switch per queued TRIM, and the wbt sleep moves to a `z_trim_iss` thread.

Questions: (1) does `z_trim_int` submit discards and sleep in blk-wbt on master, and does it stop with PR4; (2) how long does a `zpool trim` of the same free space take on each; (3) the discards' latency; (4) what a sync writer next to a manual trim and next to autotrim sees (fsync p50/p99).

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
| --- | --- | --- | --- |
| T, PR4 | `wbp4-pr4` | `1fd3fd8c642328f4bb6ebdd481f1e43c226a6fd6` | `661a3c3010463b13998ae10e02f3e6484a8f1b8d` |
| M, upstream master (T~1) | - | `cde5d598eb44b5d6efe8167666d988a496346657` | `d70a147b0775bf4bd216072a8b05738b603d6849` |
| kit | `vm-kit-wbp4` | `e6cf85cfb4306ef9005e18695f7cbdcfb87a7cb5` (no parent) | `a81895f5eaf20aded53b45661d07821e86879ddf` |

cde5d598eb4 is c913da86156 plus upstream PR #19910 (CI only), so M's module source is c913da86156's: ZVH, ROV3 and VQ1 recorded srcversion `65D8C0C24A9B688639F0721` for it. `git diff --numstat` from M to T: `include/sys/zio.h` 1/0, `module/zfs/vdev_queue.c` 14/0, `module/zfs/zio.c` 12/0.

Builds: `$T/M` and `$T/T` release (`./configure` with no option), as VQ1 for its timing, for the measurement; `$T/Td` (the same commit as T) debug (`./configure --enable-debug`), for ztest and ZTS only.

## The rig (`wbp4-bench.sh setup`)

Four memory-backed null_blk devices `wbp4nb0`-`wbp4nb3` (1 GiB each, 4K blocks, discard on, 2 ms per request by timer, 2 tags), mq-deadline, `rotational` 1 (so ZFS queues their TRIMs), `wbt_lat_usec` 75000 (the rotational default, written). A stripe `wbp4p` of the four (ashift 12, compression off, autotrim off, `cachefile=none`, mounted under `$T/mnt`) with `frag` (recordsize 64K: 16384 files of 64K written, then every other one deleted: about 8192 free 64K extents, the same in every run), `churn` (recordsize 64K) and `sync` (recordsize 8K, a 64 MiB file). The VM's test disk is not used. Module parameters at each load: `zfs_arc_max` 1 GiB, `zfs_txg_history` 100, `zfs_trim_txg_batch` 1 (autotrim issues each txg's frees at the next txg instead of every 32 txgs, so that a 40 s window sees it in steady state; the same on both trees). Everything else is the default (`zfs_vdev_trim_max_active` 2, `zfs_trim_queue_limit` 10, extents 32K to 128M), recorded per run. `kernel.sched_schedstats` is set to 1 for the task and restored at teardown.

**Why 2 tags, not the 4 of the starting plan.** blk-wbt takes its depth from the queue's `nr_requests` when the disk registers (6.12 `wbt_init()`, `blk_queue_depth()`), which mq-deadline sets to twice the tag count: with 4 tags that is 8, and blk-wbt's discard limit `wb_background` is (8 + 3) / 4 = 2, equal to `zfs_vdev_trim_max_active`, so no discard would ever wait. With 2 tags, `nr_requests` is 4 and `wb_background` (4 + 3) / 4 = 1: the second TRIM ZFS issues to a disk waits for the first, as on a real disk once slow reads have scaled blk-wbt down (a SATA disk with 32 tags starts at 4). The driver reads `wb_background` from debugfs and stops if it is not 1.

**Two rows per arm and pass** (`wbp4-bench.sh pass`; the pool exported, the arm's modules loaded if the tree changes, the pool imported from its four devices only, `cachefile=none`):

| Row | What runs | Window |
| --- | --- | --- |
| mt | a sync writer (8K `pwrite` + `fdatasync()`, 50 per second) on `sync/f`; after 5 s, `zpool trim -w wbp4p`, timed, at most 120 s (then `zpool trim -c`, and the run is marked CAPPED) | the trim |
| at | `autotrim=on`; the sync writer; churn: 64K files created in `churn/d`, 400 per second, and once 4000 exist a random one deleted after each creation (scattered frees, about 400 extents per second); 10 s ramp | 40 s |

During each window a tracefs instance `wbp4` traces the discards to the four devices (`block_bio_queue` with the submitting task, `block_getrq`, `block_rq_issue`, `block_rq_complete`; filtered on the devices and on `rwbs ~ "*D*"`, or the devices only if the kernel refuses the glob, recorded in `setup-trace.txt`); the `z_trim_*` threads are sampled every 20 ms (state, and `wbt_wait`/`rq_qos_wait` on their kernel stack); their schedstat `sum_block_runtime` is read at both ends. Per discard: g - q is the time its submitter slept in `submit_bio()` before getting a request (blk-wbt, or a tag), c - q its latency as ZFS submitted it. After `at`: autotrim off, the churn files removed, `zpool sync`. About 20 s per arm switch, 15 to 30 s for mt, 65 s for at: **about 4 min per pass** if master's trim takes about 15 s (unknown before the run).

## Passes

Two arms in ABBA order: P1 to P4 are M T, T M, T M, M T; P5 to P8 and P9 to P12 repeat them.

| Pass | Arm order | |
| --- | --- | --- |
| F | M T | step 4's rig check, at with `RAMP_AT=5 MEAS_AT=15`, raw traces kept, not in the verdicts |
| W | M T | warm-up after GO, at with `RAMP_AT=5 MEAS_AT=10`, not reported |
| P1 | M T | |
| P2 | T M | |
| P3 | T M | |
| P4 | M T | |
| P5 | M T | |
| P6 | T M | time rule after it |
| P7 | T M | |
| P8 | M T | |
| P9 | M T | |
| P10 | T M | |
| P11 | T M | |
| P12 | M T | |

Projected: quiet check 1 min, W about 4 min, P1 to P6 about 25 min: **about 30 min from GO** to the end of P6, about 55 min with P7 to P12; longer if master's trim is slow (each mt is capped at 120 s).

**Time rule** (keeps the measured part under 90 min). After P6, from driver.log: d = the mean duration of P1 to P6; run the largest number k of P7 to P12 (in that order, k from 6 down to 0) for which now + k × d + 2 min is at most GO + 90 min. Six passes are the fewest that can give p <= 0.05. Step 7 gives the command; it writes `$L/time-rule.txt`.

## Written before the run

- **Verdict per row and metric** (`wbp4-report.py raw P`): over the passes where both arms ran, ratio = T / M in the same pass; "lower" if the geometric mean of the ratios is 0.97 or less and the exact sign-flip permutation p is 0.05 or less, "higher" if 1.03 or more with the same p, otherwise "no difference shown". mt: trim seconds, discards per second, the device discard-busy fraction (each disk's time with a discard issued and not completed, over the span from the first discard to the last), fsync p50 and p99 during the trim, discard latency p99. at: discards per second, fsync p50 and p99, discard latency p99.
- **Who submits and who waits** ("Who submits the discards, and who waits"): per row and arm, the share of discards submitted by `z_trim_int`, `z_trim_iss` and any other thread; per thread class the wbt wait summed per run (from the trace) and its p99 per discard, the share of 20 ms samples asleep in `wbt_wait`/`rq_qos_wait`, and schedstat's D time per run; "as expected" or "surprise" for `z_trim_int`.
- **Noise**: steal and load1 per run are in the driver's quiet line and not judged per run here; trace events lost, lines not parsed, discards with no completion and completions with no submission are listed per run (all 0 expected).

**Expected, from reading the code (report what you see, not this).**

| | M | T |
| --- | --- | --- |
| submitters (mt and at) | most discards by `z_trim_int` (released at a completion), the rest by `z_trim_iss` (issued on arrival, at most 2 per disk at a time) | none by `z_trim_int`; all by `z_trim_iss` |
| blk-wbt wait | `z_trim_int`'s discards wait (about one device time, 2 ms, each); `z_trim_int` asleep in wbt in a large share of its samples and of its schedstat D time | `z_trim_int` never in wbt; `z_trim_iss` waits instead |
| mt | a disk whose discard completed idles until `z_trim_int` ends its sleep on another disk: busy fraction well below 1 | trim seconds lower (by up to the number of disks, 4, if `z_trim_int`'s sleeps were the bottleneck), discards per second and busy fraction higher: the report's "expected" column |
| discard latency (c - q) | about 2 ms device time plus the wbt wait | about the same per discard: the wait moves, it does not shrink |
| at | discards per second equal to T's if both keep up with the churn, lower if M falls behind | |
| fsync (mt and at) | | not known: T keeps more discards in flight at once, and they share each disk's 2 tags with the sync writer's writes, so fsync may rise with T; the sync writer's own completions run on other taskqs, so neither tree blocks them directly |

This rig cannot show: secure TRIM (`zpool trim -d`, not used); FreeBSD; how often blk-wbt is down to one discard per disk on real disks (here it is by construction).

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed.
- The pool lives on `/dev/wbp4nb0`-`/dev/wbp4nb3` only; the driver creates it and the devices and removes them. ztest uses its own files under `$T/zt`, ZTS its own file vdevs and loop devices; touch no other pool, disk or null_blk device; never `zpool import -a`; never `zfs-tests.sh -x`. The driver imports only its own pool, by name, from its own four devices, with `cachefile=none`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks: during ROV3 it imported VM 105's exported measurement pool `tank`, which may still be there, exported. No run of this task includes it (the trim tag runs `functional/trim` only), but after every ZTS run, after setup and after every pass `zpool list` must show no pool (ZTS) or no pool but `wbp4p` (the measurement); the driver checks it at every arm switch and stops (`STOP: a pool this task did not create is imported`). A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Before the ZTS run and after it: `lsmod | grep -E '^zfs '` shows the module of that step only, `timeout 30 sudo -n $T/Td/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test, `losetup -a` is empty, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`.
- Every build, ztest, the ZTS run, the setup, the rig check and every pass run detached (`setsid nohup ... &`) and are polled. A hang is: a build still going after 90 minutes; ztest after 15 minutes; a single ZTS test after 30 minutes, the group after 60; the setup after 15 minutes; the rig check after 15 minutes; a pass after 20 minutes, or `driver.log` unchanged for 6 minutes while one runs (each row logs a line, at most about 2.5 min apart). Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|ztest|ksh|python3'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- Reboots: one is pre-approved only where a task says so, and this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or test files. Modules unloaded at the end of steps 3 and 8.
- This is a performance measurement: nothing else on this VM from step 6 to the end of step 7 (no build, ztest, ZTS, second load, bpftrace or blktrace).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured pass on your own.

## Steps

### 0. State (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -l /dev/zfs 2>&1; losetup -a; ls -d /dev/wbp4nb* /var/tmp/task-1120-wbp4 /var/log/task-1120-wbp4 2>&1; ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1
command -v python3 git make gcc timeout; python3 --version; id zts; pgrep -a -x zed || echo NO-ZED; command -v bpftrace blktrace || echo no-bpftrace-no-blktrace
sudo -n ls -d /sys/kernel/debug/block /sys/kernel/tracing/events/block/block_bio_queue /sys/kernel/tracing/events/block/block_getrq 2>&1; ls -l /proc/sys/kernel/sched_schedstats 2>&1; cat /proc/sys/kernel/sched_schedstats 2>&1
grep -E 'CONFIG_BLK_WBT|CONFIG_BLK_DEBUG_FS|CONFIG_BLK_DEV_NULL_BLK=|CONFIG_MQ_IOSCHED_DEADLINE|CONFIG_SCHEDSTATS' /boot/config-$(uname -r)
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl or null_blk module is loaded, a null_blk device exists or `/dev/zfs` exists; `losetup -a` is not empty; `/dev/wbp4nb*`, `$T` or `$L` exists; a `/var/tmp/file-vdev*` or `/var/tmp/dev-*` is left; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `CONFIG_BLK_WBT` or `CONFIG_BLK_WBT_MQ` is not `y`; `CONFIG_BLK_DEV_NULL_BLK` or `CONFIG_MQ_IOSCHED_DEADLINE` is missing; the block tracepoints are missing; python3 is older than 3.8; `id zts` fails; a build, test or load runs. Note, without stopping: `CONFIG_SCHEDSTATS` and the sysctl (without them the D times are "-"), the zed line (do not start or stop zed), bpftrace/blktrace (not used either way: the kit uses tracefs and /proc sampling, so that every VM measures the same way). If `/sys/kernel/debug/block` is missing, `sudo -n mount -t debugfs none /sys/kernel/debug` and say so (the setup needs it to read `wb_background`).

`/etc/zfs/zpool.cache`: absent is expected. If it exists with size 0, move it aside and say so: `L=/var/log/task-1120-wbp4; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0`. If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

```
L=/var/log/task-1120-wbp4; T=/var/tmp/task-1120-wbp4
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit, fetched from the fork branch

```
L=/var/log/task-1120-wbp4; T=/var/tmp/task-1120-wbp4
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-wbp4
test "$(git rev-parse FETCH_HEAD)" = e6cf85cfb4306ef9005e18695f7cbdcfb87a7cb5 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = a81895f5eaf20aded53b45661d07821e86879ddf && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    90e091c735f181549153926b0a1342651d5547e9d8276ec8c0d3355786ec1e63 wbp4-bench.sh \
    f7bd7ee2c3a37148bc642390201327bcb6d4d323739eb2a04d7c1da94ac6585e wbp4-load.py \
    07decc4a72406f116ddfa10abed1f9638ad06b994f4f56795e0a33919a822ef3 wbp4-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
sh -n wbp4-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l wbp4-bench.sh
```

Expected: the three `OK` lines; three entries, exactly `100755 wbp4-bench.sh`, `100644 wbp4-load.py`, `100644 wbp4-report.py`; three `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`. The driver is POSIX sh: run it with `sh` (dash), as below.

### 2. Trees and builds (about 18 min)

```
T=/var/tmp/task-1120-wbp4; L=/var/log/task-1120-wbp4
git init -q $T/repo && cd $T/repo && git fetch -q --no-tags --depth 2 https://github.com/tigerblue77/zfs wbp4-pr4 && git log --oneline -2 FETCH_HEAD
test "$(git rev-parse FETCH_HEAD)" = 1fd3fd8c642328f4bb6ebdd481f1e43c226a6fd6 && echo T commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 661a3c3010463b13998ae10e02f3e6484a8f1b8d && echo T tree OK
test "$(git rev-parse FETCH_HEAD~1)" = cde5d598eb44b5d6efe8167666d988a496346657 && echo M commit OK
test "$(git rev-parse 'FETCH_HEAD~1^{tree}')" = d70a147b0775bf4bd216072a8b05738b603d6849 && echo M tree OK
git diff --numstat FETCH_HEAD~1 FETCH_HEAD
git worktree add -q --detach $T/M cde5d598eb44b5d6efe8167666d988a496346657
git worktree add -q --detach $T/T 1fd3fd8c642328f4bb6ebdd481f1e43c226a6fd6
git worktree add -q --detach $T/Td 1fd3fd8c642328f4bb6ebdd481f1e43c226a6fd6
printf '%s  %s\n' \
    c6b7c5c0c3d9a7639b646a940352ad62d5b512de4ab6944d67139e9dcba98315 $T/M/module/zfs/vdev_queue.c \
    be31510828342e582cdc11c93ed51d175c177e3d10b7ffe7d285ad1d5876e32d $T/M/module/zfs/zio.c \
    1ee7c5a34580dfd3db7fcaee0f97785b7420db70d5c5ee44a1bd43cb19269f1e $T/M/include/sys/zio.h \
    78795f09d1ede8bab3120717b462ab4169d482e3f03dd93d349e62806e872256 $T/T/module/zfs/vdev_queue.c \
    3b6383f4ae15189de9e10e2af3307508685092128d9245540833653deb1f9ca7 $T/T/module/zfs/zio.c \
    3cea472fcdc0ed089e25bcfa8b074b515fa0eaa3454aa9976763690b6707e09c $T/T/include/sys/zio.h \
    78795f09d1ede8bab3120717b462ab4169d482e3f03dd93d349e62806e872256 $T/Td/module/zfs/vdev_queue.c \
    3b6383f4ae15189de9e10e2af3307508685092128d9245540833653deb1f9ca7 $T/Td/module/zfs/zio.c > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```

Four `OK` lines, the numstat of the table above, and eight `OK` from `sha256sum -c`. Then build the three at once, `make -j3` each, detached:

```
T=/var/tmp/task-1120-wbp4; L=/var/log/task-1120-wbp4
setsid nohup env T=$T L=$L sh -c 'for a in M T Td; do (o=; [ $a = Td ] && o=--enable-debug; cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure $o > $L/configure-$a.log 2>&1 && nice -n 10 make -j3 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Poll `cat $L/build.log` until `build-done`. Then:

```
T=/var/tmp/task-1120-wbp4; L=/var/log/task-1120-wbp4
cat $L/build.log
for a in M T Td; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko) debug=$(grep -c -- '--enable-debug' $T/$a/config.log)"; done
modinfo -p $T/M/module/zfs.ko | grep -cE '^zfs_(trim_txg_batch|vdev_trim_max_active):'; ls -l $T/Td/ztest $T/Td/zpool
grep -h 'warning:' $L/build-*.log | grep -E 'vdev_queue\.c|zio\.c|zio\.h' | sort -u
```

Expected: rc 0 three times; modules built for `uname -r`; M's srcversion differs from T's, and is expected to be `65D8C0C24A9B688639F0721` (another value is recorded and reported, not a stop); Td's is expected to equal T's (the same sources; recorded either way); `debug=0` for M and T, above 0 for Td; `2`; `ztest` and `zpool` present in Td. Quote any warning in those files. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Functional, debug PR4 (about 25 min)

**ztest smoke** (userspace, no module needed):

```
T=/var/tmp/task-1120-wbp4; L=/var/log/task-1120-wbp4
mkdir -p $T/zt && cd $T/zt && setsid nohup env T=$T sh -c '$T/Td/ztest -VV -T 300 -s 128M -f $T/zt; echo ztest-rc=$?' > $L/ztest.log 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/ztest.log` until `ztest-rc=`. Expected `ztest-rc=0`. Quote `tail -n 15 $L/ztest.log` and `grep -ci trim $L/ztest.log` (whether its trims reached the new path is not known: file vdevs, trims at random). A non-zero rc: quote `grep -nE 'ASSERT|VERIFY|panic|failed|error' $L/ztest.log | head -20`, keep `$T/zt` as it is, stop and report. Then `rm -rf $T/zt && sudo -n fstrim -av`.

**ZTS trim group** (`-T trim`: `functional/trim`, six tests: autotrim_integrity, autotrim_config, autotrim_trim_integrity, trim_integrity, trim_config, trim_l2arc, about 12 min on CI). Load the debug modules: `sudo -n /var/tmp/task-1120-wbp4/Td/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (Td's) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks of the rules, then:

```
cd /var/tmp/task-1120-wbp4/Td && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T trim > /var/log/task-1120-wbp4/zts-trim.log 2>&1 < /dev/null &
```

Poll `tail -n 3 /var/log/task-1120-wbp4/zts-trim.log` until the Results Summary. Expected all PASS (setup, the six tests, cleanup). Quote every `Test:` line with result and duration and the Results Summary. A SKIP of setup ("DISKS do not support discard") means the group did not run: quote it, a finding, go on. A FAIL is quoted with the 20 lines before it in `<log directory>/log` and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known; it does not stop the task.

After it: the pre-run checks once more (no pool, CACHE-OK, no file-vdev, `losetup -a` empty); every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected: one is a stop). Unload with `sudo -n /var/tmp/task-1120-wbp4/Td/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

### 4. Setup and rig check (about 10 min, release trees)

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n sh ./wbp4-bench.sh setup > $L/setup.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `setup done` or `STOP` (writing and deleting the frag files takes a minute or two). Then `cat $L/setup.nohup $L/setup-devices.txt $L/setup-trace.txt $L/setup-vdev-scheduler.txt $L/setup-zpool-list.txt` and `sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1`. Expected: `setup done: ..., deleted 8192, MemAvailable ...`; for each of `wbp4nb0`-`wbp4nb3`: `sched ... [mq-deadline] ...`, `rot 1`, `wbt_lat_usec 75000`, `nr_requests 4`, `discard_max_bytes` above 0, `wb_background 1`, `wb_normal 2`, `completion_nsec 2000000`, `hw_queue_depth 2`, `discard 1`; schedstats `0 -> 1` (or `absent`); the four trace filters (with `rwbs ~ "*D*"`, or the devices only: say which); `scheduler` `auto` (or `-`, the default) on the leaves; the last load line without "DEBUG mode" (the release M tree). Then `timeout 30 sudo -n /var/tmp/task-1120-wbp4/M/zpool list -H -o name` must print `wbp4p` only. If setup stops, run `teardown` before trying once more; a STOP on `wb_background` is a finding: report it (the rig cannot make a discard wait), do not retry.

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n sh ./wbp4-bench.sh flags > $L/flags.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `flags done` or `STOP` (mt and a 15 s at on both trees, about 4 min). Then `cat $L/flags.nohup $L/flags.txt`, and quote `grep -m 4 -E 'block_(bio_queue|getrq): .*\[z_trim_int\]' $L/raw/F/M/mt/trace.txt` (raw lines: the same discard queued and then given a request later). **Stop and report** (after `teardown`) if: in flags.txt, M's mt `z_trim_int` row is a "surprise" (no discard submitted by `z_trim_int`, or none of them waited: the rig does not reproduce what PR4 is about); T's mt or at `z_trim_int` row is a "surprise" (PR4 does not do what it says: the finding that matters most); a run lost trace events or has unparsed lines, open discards or orphan completions above 1 % of its discards; M's mt is CAPPED (the trim took more than 120 s: the passes would not fit). Anything else is noted for main.

### 5. Note to main, then wait for GO

Steps 6 and 7 must run alone on the host, on a quiet VM: the test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's kernel, cmdline, configs and tracefs lines; the build table with rc, times, vermagic, srcversions and debug; the ztest and ZTS results; `setup-devices.txt`, `setup-trace.txt` and the setup line; `flags.txt` whole and the trim seconds of both mt runs) and, as the very last line, exactly:

`TASK-1120-WBP4 READY-TO-MEASURE (about 30 to 60 min alone on the host, at most 90). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 6 only on a message from main that contains `GO TASK-1120-WBP4`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1120-WBP4`, go to step 8 without running anything. Nothing runs while you wait (pool imported and idle).

### 6. Quiet check and warm-up (about 5 min)

```
L=/var/log/task-1120-wbp4
date +%s > $L/go.txt; date -u; cd $L/bin && sudo -n sh ./wbp4-bench.sh quiet 60; tail -1 $L/driver.log
```

If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so.

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n env RAMP_AT=5 MEAS_AT=10 sh ./wbp4-bench.sh pass W "M T" > $L/pass-W.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass W done` or `STOP`. Then:

```
L=/var/log/task-1120-wbp4
grep -h '^srcversion' $L/raw/W/*/*/layout.txt | sort | uniq -c; grep -h -E '^zfs_(trim_txg_batch|vdev_trim_max_active)=' $L/raw/W/*/*/layout.txt | sort | uniq -c; grep -h -o 'wb_background [0-9-]*' $L/raw/W/*/*/layout.txt | sort | uniq -c; timeout 30 sudo -n $(cat /var/tmp/task-1120-wbp4/cur-tree)/zpool list -H -o name
```

Expected: M's and T's srcversions (step 2) on 2 runs each; `zfs_trim_txg_batch=1` and `zfs_vdev_trim_max_active=2` on 4 runs; `16 wb_background 1`; `wbp4p` only. If one differs, run `teardown` and report.

### 7. The passes (about 25 to 75 min)

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n sh ./wbp4-bench.sh pass P1 "M T" > $L/pass-P1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P1 done` or `STOP`. Then `python3 -I $L/bin/wbp4-report.py $L/raw P > $L/summary.md; echo rc=$?` and check, before going on, in its "Who submits" table: M's `z_trim_int` rows "as expected" and T's `z_trim_int` rows "as expected"; no run lost trace events (Per run table). If not: stop, `teardown`, report. Note also, without stopping, the trim seconds and fsync p99 of P1 per arm. Then P2 to P6 as one detached chain:

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n sh -c 'for p in "P2:T M" "P3:T M" "P4:M T" "P5:M T" "P6:T M"; do sh ./wbp4-bench.sh pass "${p%%:*}" "${p#*:}" || exit 1; done; echo chain-done' > $L/chain-1.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pass P6 done` or `STOP`; after each `pass Pn done`, `timeout 30 sudo -n $(cat /var/tmp/task-1120-wbp4/cur-tree)/zpool list -H -o name` must print `wbp4p` only. Then the **time rule**:

```
L=/var/log/task-1120-wbp4
awk -v go="$(cat $L/go.txt)" -v now="$(date +%s)" '
function sec(t, a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "start" { s[$3] = sec($1) }
$2 == "pass" && $3 ~ /^P[1-6]$/ && $4 == "done" { e = sec($1) - s[$3]; if (e < 0) e += 86400; d += e; n++ }
END { d = d / n; for (k = 6; k > 0; k--) if (now + k * d + 120 <= go + 5400) break
    split("P7 P8 P9 P10 P11 P12", p, " "); r = ""; for (i = 1; i <= k; i++) r = r " " p[i]
    printf "d %.0f s over %d passes, now %d, GO+90 %d, k %d: run%s\n", d, n, now, go + 5400, k, k ? r : " none" }' $L/driver.log | tee $L/time-rule.txt
```

Expected `over 6 passes`. Then the k passes it leaves, in order, as one detached chain; for k = 6:

```
L=/var/log/task-1120-wbp4
cd $L/bin && setsid nohup sudo -n sh -c 'for p in "P7:T M" "P8:M T" "P9:M T" "P10:T M" "P11:T M" "P12:M T"; do sh ./wbp4-bench.sh pass "${p%%:*}" "${p#*:}" || exit 1; done; echo chain-done' > $L/chain-2.nohup 2>&1 < /dev/null &
```

(for a smaller k, the same command with only the first k entries of the list). When the last run has ended, write in your chat the line `TASK-1120-WBP4 MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 8 without waiting.

### 8. Report data, teardown, end

```
L=/var/log/task-1120-wbp4; cd $L/bin
python3 -I ./wbp4-report.py $L/raw P > $L/summary.md; echo rc=$?
python3 -I ./wbp4-report.py $L/raw W > $L/summary-w.md; echo rc=$?
sudo -n sh ./wbp4-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /dev/wbp4nb* 2>&1; ls /sys/kernel/config/nullb 2>&1; sudo -n ls -d /sys/kernel/tracing/instances/wbp4 2>&1; cat /proc/sys/kernel/sched_schedstats 2>&1
sudo -n dmesg | sed -n '/task-1120-wbp4: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw && sudo -n rm -rf /var/tmp/task-1120-wbp4 && sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1; ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1; losetup -a
```

Expected: no module loaded, no `/dev/wbp4nb*`, no null_blk device, no `wbp4` trace instance, `sched_schedstats` back to its step 0 value, no kernel line (report any, with the step it happened in), no file vdev or loop device. `/etc/zfs/zpool.cache`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Keep `/var/log/task-1120-wbp4` (raw data included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build or equal M and T srcversions; ztest failing; a kernel line after the ZTS run; a setup that fails twice, a device not as expected or `wb_background` not 1; step 4's stop list; a failed check after W or P1; a driver `STOP:` (teardown, then report); a pool this task did not create; a hang (capture first); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING` or hung task in the kernel log (capture, report, no reboot); more than 2 h 45 on the VM without the wait for GO (finish the current pass, then step 8, and say what did not run). A ZTS FAIL or SKIP, a verdict against the expectations, a CAPPED run of T or a pass dropped under the time rule is a finding, not a stop.

## Report

`/var/log/task-1120-wbp4/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from summary.md and flags.txt (recompute nothing by eye):
   a. **The rig**: `wb_background`, `nr_requests` and the filters (setup); the frag layout (`deleted 8192`, discards per mt run); flags.txt's raw trace lines.
   b. **Who submits and who waits**: the "Who submits" table for mt and at, M against T: `z_trim_int`'s share of discards, its wbt wait per run, its wbt sample share and D time; the same for `z_trim_iss`. Each "as expected" or "surprise".
   c. **zpool trim**: trim seconds per arm (means and per run), the verdicts on trim seconds, discards per second and busy fraction; any CAPPED run.
   d. **TRIM latency**: discard latency p50 and p99 per arm and row, the verdicts, and the wait p99 per thread class.
   e. **The sync writer**: fsync p50, p99 and max during mt and during at per arm, and the verdicts.
   f. **Autotrim**: discards per second and the churn rates per arm.
   g. **Functional**: ztest rc and its tail; the ZTS trim group per test; the dmesg check.
   h. **Noise**: the quiet check; the arm order per pass; the time of GO and what the time rule dropped.
2. summary.md whole, then summary-w.md's "Per run" table only, then flags.txt whole.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, the kernel config lines, debugfs, tracefs and schedstats lines, kit checks, code checks and file hashes, build times and rc, vermagic, srcversions, setup-devices.txt, setup-trace.txt, the setup line; per pass its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 7, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, devices, trace instance, schedstats, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0 and 8, then End.
