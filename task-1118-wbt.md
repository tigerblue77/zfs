TASK-1118-WBT (from the coordinator session "main"): performance, VM 107 (CHSONVTST04) on its Debian 13 kernel 6.12, about 1 h 40 on the VM plus the wait for main's GO, at most 2 h 10 without that wait. Its measured part (steps 5 and 6, about 75 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 4 are functional and may run while other VMs work. Two release builds, upstream master c913da86156 and fork branch `vdev-disk-bio-sync-flags`, four arms (M, M0, S, A) over two rows (R75, R10) on a raidz2 of five null_blk devices made to look like SATA HDDs, six passes in a balanced arm order. Nothing is published on GitHub, no credential files, fstrim at the end. Reboot when needed without waiting for main (save the diagnostics first: dmesg, stacks, logs under /var/log/task-1118-wbt/), arrange your own resumption, and report the reboot. Report: `/var/log/task-1118-wbt/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`); the blocks below do.

## Why

On the owner's Proxmox host (kernel 7.0, OpenZFS 2.4.4), a raidz2 of five SATA HDDs without a log device takes a guest writing 51 MB/s to a zvol of volblocksize 1M with frequent flushes (148 WR_INDIRECT itx/s). Over ten minutes: ZFS sync write queue wait p50 0 ms and p99 4295 ms, total write p99 2147 ms, disk write p99 268 ms, disk read p99 1074 ms, while each disk was only about 22 % busy (w_await about 9 ms). 7.2 % of the blocked-task stack samples were `z_wr_int` threads in `rq_qos_wait < wbt_wait < __rq_qos_throttle < blk_mq_submit_bio < __submit_bio < submit_bio < vdev_disk_io_start < zio_vdev_io_start < zio_nowait < vdev_queue_io_done < zio_vdev_io_done < zio_execute < taskq_thread`: the block layer's writeback throttle (blk-wbt) puts ZFS's write completion threads to sleep while they issue the next queued write.

From the kernel sources (6.12 and 7.0 read, same logic): blk-wbt, on by default on blk-mq disks unless BFQ or iocost turns it off, throttles every write that is not both REQ_SYNC and REQ_IDLE. Its limit is a quarter of min(16, queue depth) when a read was seen in the last 100 ms, shrinks further after each window in which every read took longer than `wbt_lat_usec` (75 ms on rotational disks), and on a disk with a volatile write cache the sleepers are woken only once every tracked write has completed. On master, `vdev_disk_io_rw()` submits every ZFS write as a plain WRITE and flushes as WRITE|PREFLUSH, so all of them are throttled, ZIL blocks and the flushes `zil_commit()` waits for included.

The branch has two commits: `b55aa658caa` marks writes of the sync write class (ZIL blocks, `dmu_sync()`, labels, uberblocks) and flushes REQ_SYNC|REQ_IDLE; `bc213d2730c` marks all writes so by default and adds `zfs_vdev_disk_sync_writes` (0 none, as master; 1 sync writes and flushes, as the first commit; 2 all writes and flushes, the default).

Questions: (1) does blk-wbt put ZFS threads to sleep on this rig with master, and do arms A and M0 remove that entirely; (2) how much do fsync latency, read latency and throughput change with S (sync writes only) and with A (all writes), against master and against M0 (blk-wbt off, the ceiling); (3) does A cost read latency, the risk of the second commit.

## Arms and rows

| Arm | Tree | Setting |
| --- | --- | --- |
| M | `$T/M`, master | blk-wbt at the row's target |
| M0 | `$T/M`, master | blk-wbt off (`wbt_lat_usec` 0) |
| S | `$T/B`, branch | `zfs_vdev_disk_sync_writes=1`, blk-wbt at the row's target |
| A | `$T/B`, branch | `zfs_vdev_disk_sync_writes=2` (the default), blk-wbt at the row's target |

| Row | `wbt_lat_usec` | Meaning |
| --- | --- | --- |
| R75 | 75000 | the rotational default |
| R10 | 10000 | the devices' reads (8 ms or more) exceed it, which keeps blk-wbt scaled down, the state slow reads cause on real disks (the host's disk read p99 was 1074 ms against 75 ms) |

**The rig** (`wbt-bench.sh setup`): five memory-backed null_blk devices `wbt0`-`wbt4` (640 MiB each, 4K blocks, 8 ms per request by timer, 150 MiB/s, 32 tags, a 32 MiB volatile write cache, mq-deadline, `rotational` set to 1 so that blk-wbt's target becomes 75 ms), about 3.3 GiB of RAM at most; a raidz2 pool `wbtp` (ashift 12, compression off) with a zvol `wbtp/w` (320M, volblocksize 1M), a zvol `wbtp/r` (320M, volblocksize 1M, primarycache=metadata, so that its reads reach the disks) and a file system `wbtp/bulk` (recordsize 1M); `zfs_arc_max` 512 MiB. The VM's test disk is not used.

**One run** (`wbt-bench.sh pass`): the pool is exported, the arm's modules loaded if the tree changes, the pool imported, the arm's settings written, then for 10 s of ramp plus 60 s measured: a writer of 512 KiB O_DIRECT writes to `wbtp/w` at random aligned offsets, 100 per second, with an `fdatasync()` after every 150 (the host's guest: about 50 MB/s, a flush about every 1.5 s, WR_INDIRECT records); a reader of 32K to 256K O_DIRECT reads of `wbtp/r`, 10 per second; 60 MiB/s of buffered writes to `/wbtp/bulk/f` (transaction group writes); a sampler that every 0.25 s records each device's requests in flight, blk-wbt's tracked writes and limits (debugfs), and every thread in uninterruptible sleep with whether `wbt_wait`/`rq_qos_wait` is on its kernel stack (the host collector's method). Every operation's latency is logged; `zpool iostat -w` histograms, /proc/stat, PSI, diskstats and the ZIL kstats are taken around the measured 60 s. About 85 s per run, 8 runs per pass.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
| --- | --- | --- | --- |
| branch head (B) | `vdev-disk-bio-sync-flags` | `bc213d2730c69af7ffabdf1e60a6701c3e5ad8eb` | `1fff3e3f3c80a4809ed58a5b82461d56dbf01d03` |
| its parent | - | `b55aa658caa8961288f41177b34860b39859c386` | `a4445f23f3308530a6182e820715f1bac08d1315` |
| master (M, B~2) | - | `c913da86156eba3243cc1ef515e98d69887654a7` | `7e32938edf1a54c02519e2dc0104e9325b5a6f4d` |
| kit | `vm-kit-wbt` | `bfff869ea0561bce08fb62285396c5432cbe1b93` (no parent) | `a272c50c3dc5212137256d0336beb22cec87f242` |

`git diff --numstat` from master to B: `man/man4/zfs.4` 36/1, `module/os/linux/zfs/vdev_disk.c` 26/0.

## Passes

The four arm orders form a balanced Latin square (each arm once in each position, each arm followed once by each other arm); P5 and P6 repeat P1 and P2. Within a pass each arm runs both rows before the next arm; the row order alternates.

| Pass | Rows | Arm order | |
| --- | --- | --- | --- |
| W | R10 | M S A M0 | warm-up, `RAMP=5 MEAS=20`, not reported |
| P1 | R75 R10 | M S A M0 | |
| P2 | R10 R75 | S M0 M A | |
| P3 | R75 R10 | A M M0 S | |
| P4 | R10 R75 | M0 A S M | time rule after it |
| P5 | R75 R10 | M S A M0 | |
| P6 | R10 R75 | S M0 M A | |

Projected: quiet check 1 min, W 3 min, each P pass about 11-12 min (8 runs of about 85 s, two module reloads), checks about 1 min each: **about 75 min from GO** to the end of P6.

**Time rule** (keeps the measured part under 90 min). After P4, from driver.log: d = the mean duration of P1 to P4 (`pass Pn start` to `pass Pn done`), projected end = now + 2 × d + 2 min. If it passes GO + 90 min, run P5 and P6 with the row `R10` only (R75 keeps its four passes, then an indication only, since six passes are the fewest that can give p <= 0.05) and recompute with d / 2; if it still passes GO + 90 min, run P5 with `R10` only and no P6, and say so. Write the figures into `$L/time-rule.txt`.

## Written before the run

- **Verdict per row, pair and metric** (`wbt-report.py raw P`): over the passes where both arms ran the row, ratio = a / b in the same pass; "better" if the geometric mean of the ratios is 3 % or more on the good side (below 0.97 for a latency or PSI, above 1.03 for a throughput) and the exact sign-flip permutation p is 0.05 or less, "worse" if the same holds on the bad side, otherwise "no difference shown". Metrics: fsync mean, fsync p90, read p50, read p99, writer MB/s, bulk MB/s, PSI io some. Pairs: A/M, S/M, A/S, M0/M, A/M0.
- **Where the threads slept**: the mean number of threads per class (z_wr_int, z_wr_iss, z_rd_int, ..., zvol, python3 for the load) asleep in blk-wbt and in any uninterruptible sleep, per sample; times 60 s it is the thread-seconds per run. Arms A and M0 must show 0 in blk-wbt (no write is tracked, no discard is issued).
- **Noise**: steal above 1 % in a run is listed with its pass; load1 counts the run's own threads, listed, not judged.

**Expected, from reading the code (report what you see, not this):** on M, z_wr_int (also z_wr_iss and z_rd_int) threads in blk-wbt in both rows, more in R10, with `wb_background` at 1 most of the time in R10; none on A and M0; S in between (its async writes are still throttled). fsync latency A about M0, both below S, below M. Read latency: no large change expected here, because null_blk serves up to 32 requests at once in a fixed 8 ms and mq-deadline dispatches reads first, so reads do not wait behind writes inside the "disk" as they can on a real drive: this rig cannot show the read-latency cost of A on real HDDs, and the report must say so. Writer MB/s may fall below 52 on M if fsyncs stall.

## Rules for this task

- Trees: `/var/tmp/task-1118-wbt/repo` (one blob-less clone) with the worktrees `/var/tmp/task-1118-wbt/M` and `/var/tmp/task-1118-wbt/B` (release builds only). Logs and results: `/var/log/task-1118-wbt/`. Work as root through `sudo -n`. The driver puts the arm's tree first in PATH and checks that `zpool` comes from it.
- The pool lives on the five null_blk devices `wbt0`-`wbt4` only; the driver creates and destroys them. Touch no other pool, disk or null_blk device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6 (no build, ztest, ZTS, second load or bpftrace).
- No package is installed (python3 is the only tool the kit needs besides the trees).
- If a fetch or the clone fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes.
- If the driver stops with an error (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured pass on your own. If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or a hung task: save `sudo -n dmesg | tail -150` into `$L/dmesg-stop.txt` and the stacks of the blocked threads, report, and reboot only if the VM is wedged.

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -d /dev/wbt* /var/tmp/task-1118-wbt /var/log/task-1118-wbt 2>&1
command -v python3 git make gcc; python3 --version
ls -d /sys/kernel/debug/block /sys/kernel/tracing/events/block/block_bio_queue 2>&1
grep -E 'CONFIG_BLK_WBT|CONFIG_BLK_DEBUG_FS|CONFIG_BLK_DEV_NULL_BLK=|CONFIG_MQ_IOSCHED_DEADLINE' /boot/config-$(uname -r)
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
```
Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl or null_blk module is loaded or a null_blk device exists; `/dev/wbt0`-`/dev/wbt4`, `/var/tmp/task-1118-wbt` or `/var/log/task-1118-wbt` exists; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `CONFIG_BLK_WBT` or `CONFIG_BLK_WBT_MQ` is not `y`; python3 is older than 3.8; a build, test or load runs. If `/sys/kernel/debug/block` is missing, `sudo -n mount -t debugfs none /sys/kernel/debug` and say so (without it the sampler records no blk-wbt state; the rest works). If `/sys/kernel/tracing/events/block/block_bio_queue` is missing, say so (step 3's flag check then looks under `/sys/kernel/debug/tracing`).
```
L=/var/log/task-1118-wbt; T=/var/tmp/task-1118-wbt
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit, fetched from the fork branch
```
L=/var/log/task-1118-wbt; T=/var/tmp/task-1118-wbt
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-wbt
test "$(git rev-parse FETCH_HEAD)" = bfff869ea0561bce08fb62285396c5432cbe1b93 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = a272c50c3dc5212137256d0336beb22cec87f242 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    d6258995e7bea44fc73a9560450a9315d2c8cb04c4502e48c4acc2f64be1f2dc wbt-bench.sh \
    413f2a3861dcc6a06f2a33616c2082cf54f5046b74caf62b3077b4a2ee0b3a7b wbt-load.py \
    cfb7e2d0b60a2266bd8c38a65d100a92ee4f90c65c373f54deab15c301720ac1 wbt-sample.py \
    1a45016f292565b044a8ab378a072a3568a8700c54d2a437996df52f0b22ab1e wbt-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n wbt-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l wbt-bench.sh
```
Expected: the three `OK` lines; four entries, exactly `100755 wbt-bench.sh`, `100644 wbt-load.py`, `100644 wbt-report.py`, `100644 wbt-sample.py`; four `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`.

### 2. Trees and release builds (about 10 min)
```
T=/var/tmp/task-1118-wbt; L=/var/log/task-1118-wbt
git clone -q --filter=blob:none --no-checkout --single-branch --branch vdev-disk-bio-sync-flags https://github.com/tigerblue77/zfs.git $T/repo
cd $T/repo && git log --oneline -3 HEAD
test "$(git rev-parse HEAD)" = bc213d2730c69af7ffabdf1e60a6701c3e5ad8eb && echo B commit OK
test "$(git rev-parse 'HEAD^{tree}')" = 1fff3e3f3c80a4809ed58a5b82461d56dbf01d03 && echo B tree OK
test "$(git rev-parse HEAD~1)" = b55aa658caa8961288f41177b34860b39859c386 && echo B~1 commit OK
test "$(git rev-parse 'HEAD~1^{tree}')" = a4445f23f3308530a6182e820715f1bac08d1315 && echo B~1 tree OK
test "$(git rev-parse HEAD~2)" = c913da86156eba3243cc1ef515e98d69887654a7 && echo master commit OK
test "$(git rev-parse 'HEAD~2^{tree}')" = 7e32938edf1a54c02519e2dc0104e9325b5a6f4d && echo master tree OK
git diff --numstat HEAD~2 HEAD
git worktree add -q --detach $T/M c913da86156eba3243cc1ef515e98d69887654a7
git worktree add -q --detach $T/B bc213d2730c69af7ffabdf1e60a6701c3e5ad8eb
printf '%s  %s\n' \
    05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86 $T/M/module/os/linux/zfs/vdev_disk.c \
    84f544a1e7329c8b5c131ac042a643be8b687eee551a1f13cec97d057fdf6dd7 $T/B/module/os/linux/zfs/vdev_disk.c \
    41619304df6fbdf82ea82340a0a2b440f56b092eee2f889c18868fded3b58b05 $T/B/man/man4/zfs.4 > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```
Six `OK` lines, the numstat of the table above, and three `OK` from `sha256sum -c`. Then build both at once, `make -j4` each:
```
T=/var/tmp/task-1118-wbt; L=/var/log/task-1118-wbt
for a in M B; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j4 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
for a in M B; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko)"; done
modinfo -p $T/B/module/zfs.ko | grep '^zfs_vdev_disk_sync_writes:'; modinfo -p $T/M/module/zfs.ko | grep -c '^zfs_vdev_disk_sync_writes:'
grep -h 'warning:' $L/build-*.log | grep -E 'vdev_disk\.c|blkdev_compat\.h' | sort -u
```
Expected: rc 0 twice; modules built for `uname -r`; two different srcversions (equal: stop and report); for B one line starting `zfs_vdev_disk_sync_writes:Writes submitted REQ_SYNC|REQ_IDLE, out of blk-wbt: 0 none, 1 sync writes and flushes, 2 all writes and flushes`, then `0` for M. Quote any warning in those files. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup and flag check (about 4 min, functional)
```
L=/var/log/task-1118-wbt; cd $L/bin
sudo -n ./wbt-bench.sh setup; echo rc=$?; tail -2 $L/driver.log; cat $L/setup-devices.txt
```
Expected: rc 0; for each of `wbt0`-`wbt4`: `sched ... [mq-deadline] ...`, `rot 1`, `wbt_lat_usec 75000`, `write_cache write_back`, `mbps 150`, `completion_nsec 8000000`, `hw_queue_depth 32`, `cache_size 32`, and a number after `wbt_debugfs`. Record `nr_requests` and `max_sectors_kb`, the setup line (size, alloc, cap, MemAvailable). If setup stops, run `teardown` before trying once more.
```
L=/var/log/task-1118-wbt; cd $L/bin
sudo -n ./wbt-bench.sh flags; echo rc=$?
```
It runs the load for 8 s on master and on the branch at `zfs_vdev_disk_sync_writes` 0, 1 and 2, and counts the bios queued to the five devices by their rwbs (`W` write, `WS` write with REQ_SYNC, `FW` flush, `FWS` flush with REQ_SYNC, `R` read). Expected (from reading): `M:-` and `B:0`: writes `W` only and flushes `FW` only, no `WS` or `FWS`; `B:1`: both `WS` (ZIL blocks, `dmu_sync()` writes, labels) and `W` (transaction group writes), flushes `FWS` only; `B:2`: writes `WS` only, flushes `FWS` only; `R` in all four. Any other result (an `S` on M or B:0, a plain `W` or `FW` on B:2, `B:1` without both kinds): stop and report `$L/flags.txt` (the flags are what the branch is about). Leave the pool imported and idle (the driver leaves the branch loaded).

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's cmdline, kernel configs, debugfs and tracefs lines; the build table with rc, times, vermagic and srcversions; `setup-devices.txt` and the setup line; `flags.txt`) and, as the very last line, exactly:

`TASK-1118-WBT READY-TO-MEASURE (about 75 min alone on the host, at most 90). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1118-WBT`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1118-WBT`, go to step 7 without running anything. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 4 min)
```
L=/var/log/task-1118-wbt; cd $L/bin
date -u; sudo -n ./wbt-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n env RAMP=5 MEAS=20 ./wbt-bench.sh pass W "R10" "M S A M0"; echo rc=$?
grep -H -E '^(arm|srcversion|zfs_vdev_disk_sync_writes)' $L/raw/W/R10/*/layout.txt; grep -h -o 'wbt_lat_usec [0-9]*' $L/raw/W/R10/*/layout.txt | sort | uniq -c
```
If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W, check: rc 0; M and M0 show master's srcversion and `zfs_vdev_disk_sync_writes=absent`, S and A the branch's srcversion and `=1`, `=2`; `wbt_lat_usec` 10000 on M, S and A (15 lines) and 0 on M0 (5 lines). If one differs, run `teardown` and report.

### 6. The six passes (about 70 min)
```
L=/var/log/task-1118-wbt; cd $L/bin
sudo -n ./wbt-bench.sh pass P1 "R75 R10" "M S A M0"; echo rc=$?
python3 -I ./wbt-report.py $L/raw P > $L/summary.md; echo rc=$?
```
After P1, check in summary.md's "Threads asleep" tables, before going on: A and M0 show `0.000` in blk-wbt for every class and in "all in wbt", in both rows (if not: stop, `teardown`, report with the stacks of `samples.txt` lines `D ... 1 ...` of those runs); and M shows more than 0 "all in wbt" in at least one row (if M shows 0 in both rows, the rig does not reproduce the throttling: stop, `teardown`, report what you see, a finding). Note also, without stopping: per arm and row the fsync mean and read p99 of P1. Then, one command each or one chain:
```
L=/var/log/task-1118-wbt; cd $L/bin
sudo -n ./wbt-bench.sh pass P2 "R10 R75" "S M0 M A"; echo rc=$?
sudo -n ./wbt-bench.sh pass P3 "R75 R10" "A M M0 S"; echo rc=$?
sudo -n ./wbt-bench.sh pass P4 "R10 R75" "M0 A S M"; echo rc=$?
```
Apply the **time rule** now (section Passes) and write its figures (d, now, projected end, GO + 90 min, what was dropped) into `$L/time-rule.txt`. Then, with the rows the rule leaves:
```
L=/var/log/task-1118-wbt; cd $L/bin
sudo -n ./wbt-bench.sh pass P5 "R75 R10" "M S A M0"; echo rc=$?
sudo -n ./wbt-bench.sh pass P6 "R10 R75" "S M0 M A"; echo rc=$?
```
When the last run has ended, write in your chat the line `TASK-1118-WBT MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 7 without waiting.

### 7. Report data, teardown, end
```
L=/var/log/task-1118-wbt; cd $L/bin
python3 -I ./wbt-report.py $L/raw P > $L/summary.md; echo rc=$?
python3 -I ./wbt-report.py $L/raw W > $L/summary-w.md; echo rc=$?
sudo -n ./wbt-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /dev/wbt* 2>&1; ls /sys/kernel/config/nullb 2>&1
sudo -n dmesg | sed -n '/task-1118-wbt: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw flags && sudo -n rm -rf /var/tmp/task-1118-wbt && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, no `/dev/wbt*`, no null_blk device, no kernel line (report any, with the step it happened in). Keep `/var/log/task-1118-wbt` (raw data included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build, equal srcversions, or the parameter not exactly in B; a setup that fails twice or a device not as expected; a flag check against the expected result; a failed check after W or P1; a driver `STOP:` (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or hung task in the kernel log (capture, report, reboot only if wedged); more than 2 h 10 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict against the expectations or a row dropped under the time rule is a finding, not a stop.

## Report

`/var/log/task-1118-wbt/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from summary.md (recompute nothing by eye):
   a. **Flags**: `flags.txt` against the expected result of step 3.
   b. **Does blk-wbt put ZFS threads to sleep, per arm and row**: the "Threads asleep" tables (z_wr_int, z_wr_iss, z_rd_int, zvol, python3 and "all in wbt"), the top z_wr_int stacks, and the devices table (writes in flight, blk-wbt tracked, `wb_background` median and % at 1 or less). Do A and M0 show none? Does M's stack match the host's (`rq_qos_wait < wbt_wait < __rq_qos_throttle < blk_mq_submit_bio < ... < vdev_queue_io_done < zio_vdev_io_done`)?
   c. **fsync**: per row the arms' fsync mean, p50, p90, max and the pooled p50/p90/p99/max, and the verdicts of A/M, S/M, A/S, M0/M and A/M0 on fsync mean and p90.
   d. **Reads**: read p50, p99 and the pooled p99.9 per arm and row, `disk_wait r` p99, and the verdicts on read p50 and p99, with the rig's limit for this question (Expected) stated.
   e. **Throughput and pressure**: writer MB/s, bulk MB/s, PSI io some and full, busy CPU, with the verdicts.
   f. **A against M0**: does marking every write give what turning blk-wbt off gives? Any metric where A/M0 is not "no difference shown".
   g. **S against A**: what marking the async writes adds over the sync writes and flushes alone.
   h. **ZFS's own view**: `zpool iostat -w` percentiles (total_wait w, disk_wait w and r, syncq_wait w, asyncq_wait w) and the ZIL counters per second, per arm and row.
   i. **Noise**: the quiet check; per run the steal and load1 (the identity table); any run with steal above 1 %; the arm order per pass; the time of GO and what the time rule dropped.
2. summary.md whole, then summary-w.md's identity table only.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, the kernel config lines, debugfs and tracefs, kit checks, code checks and file hashes, build times and rc, vermagic and srcversions, setup-devices.txt, the setup line; per pass its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, devices, `fstrim -av`, `df -h /var/tmp`), and any reboot with its UTC time and how you resumed.
