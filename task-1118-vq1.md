TASK-1118-VQ1 (from the coordinator session "main"): performance, on this Debian 13 test VM (kernel 6.12, 8 vCPU, 7.8 GB RAM), about 1 h 50 on the VM plus the wait for main's GO, at most 2 h 20 without that wait. Its measured part (steps 5 and 6, about 60 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 4 are functional and may run while other VMs work. Three release builds fetched by commit SHA from the fork (upstream master, the block-layer flags commit, and the age guard on top of it), four arms (M, P1, P1A, P1I) on a raidz2 of five null_blk devices made to look like SATA HDDs, with blk-wbt's read target set below the devices' read time so that blk-wbt scales down; eight passes in a balanced arm order. Nothing is published on GitHub, no credential files, fstrim at the end. Reboot when needed without waiting for main (save the diagnostics first: dmesg, stacks, logs under /var/log/task-1118-vq1/), arrange your own resumption, and report the reboot. Report: `/var/log/task-1118-vq1/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`); the blocks below do.

## Why

On the owner's host (kernel 7.0, OpenZFS 2.4.4), a raidz2 of five SATA HDDs without a log device takes a guest writing 51 MB/s to a zvol of volblocksize 1M with frequent flushes (148 WR_INDIRECT itx/s, 0.6 commits/s). Over ten minutes: ZFS sync write queue wait p50 0 ms and p99 4295 ms, total write p99 2147 ms, disk write p99 268 ms, disk read p99 1074 ms, while each disk was about 22 % busy. Three causes are read in the code (vdev_queue study of 18/11):

1. **Held slots.** A sync read or write takes its vdev queue slot when it is selected for issue and gives it back only once its completion has been processed. Whatever delays it below the queue keeps the slot: blk-wbt putting the submitting thread to sleep, the device's queue, or a completion waiting for a `z_wr_int`/`z_rd_int` thread that is itself asleep in blk-wbt while issuing the next queued txg write. With 10 sync writes held (`zfs_vdev_sync_write_max_active`), the sync writes queued behind them wait whether or not the disk is busy.
2. **blk-wbt.** Commit `846b9194b4a` (P1) submits sync writes and flushes REQ_SYNC|REQ_IDLE, so blk-wbt no longer sleeps on them; txg writes stay throttled, so completion threads can still sleep while they issue them. On TASK-1118-WBT's rig, arm S marked the same writes on a pool without a log device and halved fsync latency at no read or throughput cost, but that rig never scaled blk-wbt down (its 8 ms reads were below the 10 ms target), and its writer reached only 10-16 MB/s.
3. **Indirect bursts.** Since 2.4.0 a zvol write of 32K or more that is not FUA is logged WR_INDIRECT, so at each flush `dmu_sync()` writes the whole 1M block of every record as sync writes.

The age guard (`zfs_vdev_queue_age_ms`, off by default) tests cause 1: sync reads and writes that have held their slot for longer than that many milliseconds stop counting against their class's limits (at most `*_max_active` more per class). Arm P1I tests cause 3 without code.

Questions: (1) does blk-wbt scale down on this rig, and do the threads sleep in it on every arm; (2) P1A against P1: does the age guard fire (more than 10 sync writes active on a leaf), and do the sync write queue tail and fsync latency fall while throughput and disk wait hold, or does the wait only move from the queue to the disk; (3) P1I against P1: what logging copies instead of indirect records does to sync-class leaf bytes, the sync queue tail, fsync latency and total device bytes; (4) in the seconds where a leaf has sync writes pending, how busy is that device.

## Arms

| Arm | Tree | Setting |
| --- | --- | --- |
| M | `$T/M`, master `c913da86156` | defaults |
| P1 | `$T/P`, `846b9194b4a` | `zfs_vdev_disk_sync_writes=1` |
| P1A | `$T/A`, the age guard (`c9053179c6708aa0231f024a4e294a2a4f1026e8`, child of `846b9194b4a`) | `zfs_vdev_disk_sync_writes=1`, `zfs_vdev_queue_age_ms=50` |
| P1I | `$T/P`, `846b9194b4a` | `zfs_vdev_disk_sync_writes=1`, `zfs_immediate_write_sz=2097152` |

The driver writes every one of these parameters before each import and reads it back; a parameter that a tree must not have (`zfs_vdev_disk_sync_writes` on M, `zfs_vdev_queue_age_ms` on M, P1 and P1I) is checked absent. `zfs_immediate_write_sz` is 32768 (the default) on M, P1 and P1A.

**Why P1I's value works** (read on master `c913da86156`, unchanged in the other two trees): `zvol_log_write()` (`module/zfs/zvol.c` 996) calls `zil_write_state(zilog, size, blocksize, B_FALSE, commit)`; in `module/zfs/zil.c` 2285-2318 a record is indirect when `size >= zfs_immediate_write_sz && (size >= blocksize / 2 || !commit)`, with no log device and no special vdev (this pool has neither) and logbias latency. `commit` is FUA or `sync=always` (`module/os/linux/zfs/zvol_os.c` 235-236); the writer's O_DIRECT writes without O_DSYNC carry no FUA and the zvol has `sync=standard`, so commit is false. The writer's records are 524288 bytes, so with `zfs_immediate_write_sz=2097152` (a `uint`, `ZMOD_RW`, zil.c 2263 and 5310) they are not indirect, and with commit false they are `WR_NEED_COPY`: the data is copied into log blocks at commit instead of `dmu_sync()` writing the whole 1M block. The tunable is global: the bulk file's 1 MiB records become `WR_NEED_COPY` too. They are never committed, but `dsl_pool_wrlog_count()` counts their bytes, which can start txgs earlier (`txg_kick`) and delay transactions (`dmu_tx_wrlog_delay`); the report shows both (txgs per run, dmu_tx counters).

**The rig** (`vq1-bench.sh setup`): TASK-1118-WBT's rig with three changes. Five memory-backed null_blk devices `vq0`-`vq4` (640 MiB each, 4K blocks, 8 ms per request by timer, 150 MiB/s, 32 tags, a 32 MiB volatile write cache, mq-deadline, `rotational` 1), about 3.3 GiB of RAM at most; a raidz2 pool `vqp` (ashift 12, compression off) with a zvol `vqp/w` (320M, volblocksize 1M), a zvol `vqp/r` (320M, volblocksize 1M, primarycache=metadata, so that its reads reach the disks) and a file system `vqp/bulk` (recordsize 1M). The changes: (a) `wbt_lat_usec` 4000 on every arm: blk-wbt scales down after a monitoring window (100 ms, shorter once scaled down) in which it saw a read and three writes and the fastest read took longer than its target, or in which no read completed while one had been in flight longer than the target (`latency_exceeded()`, block/blk-wbt.c); every read here takes at least 8 ms. After a window with writes and no read it scales up again; (b) `zfs_arc_max` 1 GiB and, after each import, one read of the whole `vqp/w`, so that the writer's half-block writes find their block in the ARC (the host's ARC hit rate was 96.7 %) instead of reading it first; (c) `zfs_txg_history` 200.

**One run** (`vq1-bench.sh pass`): the pool is exported, the arm's modules loaded if the tree changes, the arm's parameters written, the pool imported, blk-wbt's target written (1 first, so that blk-wbt restarts from its initial depth), `vqp/w` read once, then 10 s of ramp plus 60 s measured: a writer of 512 KiB O_DIRECT writes to `vqp/w` at random aligned offsets, 100 per second, with an `fdatasync()` after every 150; a reader of 32K to 256K O_DIRECT reads of `vqp/r`, 10 per second; 60 MiB/s of buffered writes to `/vqp/bulk/f`; a sampler that every 0.25 s records each device's requests in flight and counters, blk-wbt's tracked writes and limits (debugfs), PSI, CPU, and every thread in uninterruptible sleep with whether `wbt_wait`/`rq_qos_wait` is on its kernel stack; during the 60 s, `zpool iostat -T u -Hpvlq vqp 1` (per-leaf latencies and queue sizes) and `zpool iostat -T u -wpH vqp 1` (pool histograms), each line stamped with its arrival time. At the start and end of the 60 s: `zpool iostat -w` and `-r` histograms, the txgs kstat, the ZIL, dmu_tx, arcstats, dmu_tx_assign and objset kstats, diskstats, PSI and /proc/stat. Every operation's latency is logged. About 90 s per run, 4 runs per pass.

## What you fetch (fork https://github.com/tigerblue77/zfs, by commit)

| What | Commit | Tree |
| --- | --- | --- |
| age guard (A) | `c9053179c6708aa0231f024a4e294a2a4f1026e8` | `836581447f1c5a7b2c431e798770c200351896f8` |
| its parent (P) | `846b9194b4a3eb54f601c1bda0382b68d785b0ef` | `9a4214cf8f0887e110763ef53c666c8527c2ca9e` |
| P's parent | `2e4d79dc74b19ed0a1db0b3b752234cad61c3146` | `2d10b97c5a529ac58652c85aaed78011771f5853` |
| master (M, A~3) | `c913da86156eba3243cc1ef515e98d69887654a7` | `7e32938edf1a54c02519e2dc0104e9325b5a6f4d` |
| kit | `a9a3e04f81678e41d738f703de1e461ec3855630` (no parent) | `9082b438d28c5655dac353d0ecce78df4e918790` |

`git diff --numstat` from M to P: `man/man4/zfs.4` 43/1, `module/os/linux/zfs/vdev_disk.c` 44/0; from P to A: `include/sys/zio.h` 1/0, `man/man4/zfs.4` 15/0, `module/zfs/vdev_queue.c` 64/10.

## Passes

The four arm orders form a balanced Latin square (each arm once in each position, each arm followed once by each other arm); P5 to P8 repeat P1 to P4.

| Pass | Arm order | |
| --- | --- | --- |
| C | M P1 P1A P1I | rig check before GO, `RAMP=5 MEAS=20`, not reported |
| W | M P1 P1I P1A | warm-up after GO, `RAMP=5 MEAS=20`, not reported |
| P1 | M P1 P1I P1A | |
| P2 | P1 P1A M P1I | |
| P3 | P1A P1I P1 M | |
| P4 | P1I M P1A P1 | time rule after it |
| P5 | M P1 P1I P1A | |
| P6 | P1 P1A M P1I | |
| P7 | P1A P1I P1 M | |
| P8 | P1I M P1A P1 | |

Projected: quiet check 1 min, W 3 min, each P pass about 6-7 min (4 runs of about 90 s, two or three module reloads): **about 60 min from GO** to the end of P8.

**Time rule** (keeps the measured part under 90 min). After P4, from driver.log: d = the mean duration of P1 to P4 (`pass Pn start` to `pass Pn done`). Run the largest number k of the passes P5, P6, P7, P8 (in that order, k from 4 down to 1) for which now + k × d + 2 min is at most GO + 90 min, and drop the rest. Six passes are the fewest that can give p <= 0.05; if k is 1, say so. Write d, now, GO + 90 min, k and what was dropped into `$L/time-rule.txt`.

## Written before the run

- **Verdict per pair and metric** (`vq1-report.py raw P`): over the passes where both arms ran, ratio = a / b in the same pass; "better" if the geometric mean of the ratios is 3 % or more on the good side (below 0.97 for a latency, PSI, device bytes or txg time, above 1.03 for a throughput) and the exact sign-flip permutation p is 0.05 or less, "worse" if the same holds on the bad side, otherwise "no difference shown". Pairs: P1/M, P1A/P1, P1I/P1, P1A/M, P1I/M. Metrics: fsync mean, fsync p90, read p50, read p99, writer MB/s, bulk MB/s, PSI io some, and from ZFS's histograms over the measured 60 s (log2 buckets): syncq_wait w p99, total_wait w p99, disk_wait w p99, disk_wait r p99; device write MB/s (diskstats, sum of the five); txg sync time mean.
- **Device write MB/s** is a rate: read it next to writer and bulk MB/s; less device bytes is better only for the same work.
- **The age guard fired** in a run if a leaf showed more sync writes (or sync reads) active than `zfs_vdev_sync_*_max_active` (10) in a per-second `zpool iostat -q` sample: only the guard can do that.
- **Where the time went**: when P1A lowers syncq_wait w but raises disk_wait w by as much and fsync does not improve, the disk, not the slots, was the limit.
- **Noise**: steal above 1 % in a run is listed with its pass; load1 counts the run's own threads, listed, not judged.

**Expected, from reading the code (report what you see, not this):** on M, z_wr_int (also z_wr_iss and z_rd_int) threads asleep in blk-wbt, and `wb_background` below 4 in a large share of the samples; blk-wbt's step falls after each window with a read and rises after each window with writes only, so how often it reaches 1 is unknown. P1: no flush or sync write in blk-wbt, txg writes still are, so z_wr_int threads still sleep there; fsync lower than M (0.47-0.49× on the WBT rig; unknown here). P1A: the guard acts only once a sync I/O has held its slot for 50 ms; a device serves a request in 8 ms plus its share of 150 MiB/s, so that happens only when completions wait behind sleeping completion threads or the device's bandwidth queue is long. Whether it happens on this rig is unknown; if it never fires, P1A should equal P1 within noise. P1I: no whole-block `dmu_sync()` writes at commit (zil_itx_indirect_count 0, needcopy about 100/s); sync-class leaf bytes, device bytes and fsync change in a direction that is unknown. Reads: null_blk serves every request in a fixed 8 ms, so this rig cannot show the drive-side starvation of reads seen on the host (disk read p99 1074 ms); the report must say so.

## Rules for this task

- Trees: `/var/tmp/task-1118-vq1/repo` (one shallow repository, the three commits fetched with `--depth 4`, which reaches master from A) with the worktrees `/var/tmp/task-1118-vq1/M`, `/var/tmp/task-1118-vq1/P` and `/var/tmp/task-1118-vq1/A` (release builds only). Logs and results: `/var/log/task-1118-vq1/`. Work as root through `sudo -n`. The driver puts the arm's tree first in PATH and checks that `zpool` comes from it.
- The pool lives on the five null_blk devices `vq0`-`vq4` only; the driver creates and destroys them. Touch no other pool, disk or null_blk device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6 (no build, ztest, ZTS, second load or bpftrace).
- No package is installed (python3 and bash 5 are the only tools the kit needs besides the trees).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes.
- If the driver stops with an error (`STOP:` in driver.log): run `teardown`, then report; do not retry a measured pass on your own. If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or a hung task: save `sudo -n dmesg | tail -150` into `$L/dmesg-stop.txt` and the stacks of the blocked threads, report, and reboot only if the VM is wedged.

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -d /dev/vq[0-4] /var/tmp/task-1118-vq1 /var/log/task-1118-vq1 2>&1
command -v python3 git make gcc; python3 --version; bash -c 'echo "bash $BASH_VERSION EPOCHREALTIME=${EPOCHREALTIME:-absent}"'
ls -d /sys/kernel/debug/block 2>&1
grep -E 'CONFIG_BLK_WBT|CONFIG_BLK_DEBUG_FS|CONFIG_BLK_DEV_NULL_BLK=|CONFIG_MQ_IOSCHED_DEADLINE' /boot/config-$(uname -r)
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
```
Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl or null_blk module is loaded or a null_blk device exists; `/dev/vq0`-`/dev/vq4`, `/var/tmp/task-1118-vq1` or `/var/log/task-1118-vq1` exists; /var/tmp has less than 20 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `CONFIG_BLK_WBT` or `CONFIG_BLK_WBT_MQ` is not `y`; python3 is older than 3.8; `EPOCHREALTIME` is absent; a build, test or load runs. If `/sys/kernel/debug/block` is missing, `sudo -n mount -t debugfs none /sys/kernel/debug` and say so (without it the sampler records no blk-wbt state and the rig check of step 3 cannot pass).
```
L=/var/log/task-1118-vq1; T=/var/tmp/task-1118-vq1
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit, fetched by commit from the fork
```
L=/var/log/task-1118-vq1; T=/var/tmp/task-1118-vq1
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs a9a3e04f81678e41d738f703de1e461ec3855630
test "$(git rev-parse FETCH_HEAD)" = a9a3e04f81678e41d738f703de1e461ec3855630 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 9082b438d28c5655dac353d0ecce78df4e918790 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    6ee6c4f8e8741f3677d73cd34b79527fc366a9202617e8fd806b82c240dd0ec2 vq1-bench.sh \
    a4edfb85b26a47d41920ca376fd4bd582231ba2d986ad6596c6c8fd343b50876 vq1-report.py \
    378a43d2eaa2f1a05bafead8a4479d2d34509724d7b08f46cd6f97431fde4229 vq1-sample.py \
    413f2a3861dcc6a06f2a33616c2082cf54f5046b74caf62b3077b4a2ee0b3a7b wbt-load.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n vq1-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l vq1-bench.sh
```
Expected: the three `OK` lines; four entries, exactly `100755 vq1-bench.sh`, `100644 vq1-report.py`, `100644 vq1-sample.py`, `100644 wbt-load.py`; four `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`. (`wbt-load.py` is TASK-1118-WBT's load generator, unchanged: same hash.)

### 2. Trees and release builds (about 15 min)
```
T=/var/tmp/task-1118-vq1; L=/var/log/task-1118-vq1
git init -q $T/repo && cd $T/repo && git remote add origin https://github.com/tigerblue77/zfs && git fetch -q --no-tags --depth 4 origin c9053179c6708aa0231f024a4e294a2a4f1026e8 846b9194b4a3eb54f601c1bda0382b68d785b0ef c913da86156eba3243cc1ef515e98d69887654a7
git log --oneline -4 c9053179c6708aa0231f024a4e294a2a4f1026e8
test "$(git rev-parse 'c9053179c6708aa0231f024a4e294a2a4f1026e8^{tree}')" = 836581447f1c5a7b2c431e798770c200351896f8 && echo A tree OK
test "$(git rev-parse c9053179c6708aa0231f024a4e294a2a4f1026e8~1)" = 846b9194b4a3eb54f601c1bda0382b68d785b0ef && echo P commit OK
test "$(git rev-parse '846b9194b4a3eb54f601c1bda0382b68d785b0ef^{tree}')" = 9a4214cf8f0887e110763ef53c666c8527c2ca9e && echo P tree OK
test "$(git rev-parse c9053179c6708aa0231f024a4e294a2a4f1026e8~2)" = 2e4d79dc74b19ed0a1db0b3b752234cad61c3146 && echo P~1 commit OK
test "$(git rev-parse '2e4d79dc74b19ed0a1db0b3b752234cad61c3146^{tree}')" = 2d10b97c5a529ac58652c85aaed78011771f5853 && echo P~1 tree OK
test "$(git rev-parse c9053179c6708aa0231f024a4e294a2a4f1026e8~3)" = c913da86156eba3243cc1ef515e98d69887654a7 && echo master commit OK
test "$(git rev-parse 'c913da86156eba3243cc1ef515e98d69887654a7^{tree}')" = 7e32938edf1a54c02519e2dc0104e9325b5a6f4d && echo master tree OK
git diff --numstat c913da86156eba3243cc1ef515e98d69887654a7 846b9194b4a3eb54f601c1bda0382b68d785b0ef
git diff --numstat 846b9194b4a3eb54f601c1bda0382b68d785b0ef c9053179c6708aa0231f024a4e294a2a4f1026e8
git worktree add -q --detach $T/M c913da86156eba3243cc1ef515e98d69887654a7
git worktree add -q --detach $T/P 846b9194b4a3eb54f601c1bda0382b68d785b0ef
git worktree add -q --detach $T/A c9053179c6708aa0231f024a4e294a2a4f1026e8
printf '%s  %s\n' \
    05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86 $T/M/module/os/linux/zfs/vdev_disk.c \
    c6b7c5c0c3d9a7639b646a940352ad62d5b512de4ab6944d67139e9dcba98315 $T/M/module/zfs/vdev_queue.c \
    6c9fdd374746f4246cb977b17841a289da91cce64a5272707e8dd92b6d1e9d60 $T/P/module/os/linux/zfs/vdev_disk.c \
    c6b7c5c0c3d9a7639b646a940352ad62d5b512de4ab6944d67139e9dcba98315 $T/P/module/zfs/vdev_queue.c \
    6c9fdd374746f4246cb977b17841a289da91cce64a5272707e8dd92b6d1e9d60 $T/A/module/os/linux/zfs/vdev_disk.c \
    bd2aa2002150f3599ff80da2f5d1298e1e0d9238f7a3e1845479487170b619b6 $T/P/man/man4/zfs.4 \
    247b628496815fe4941f8fba00a23ce691afb9c04089e9272d7e417eac08836b $T/A/module/zfs/vdev_queue.c \
    3b5665d8af437b13e04e45e52d4144382833453f805dc067c3fdde3cc0cd83be $T/A/include/sys/zio.h \
    9b4c672be8ef8e2e5c021d1e27ef28d57e9fddb79c90a2a80a8d5e44209128f5 $T/A/man/man4/zfs.4 > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```
Seven `OK` lines (A tree, P commit and tree, P~1 commit and tree, master commit and tree), the two numstats of the table above, and nine `OK` from `sha256sum -c`. Then build the three at once, `make -j3` each:
```
T=/var/tmp/task-1118-vq1; L=/var/log/task-1118-vq1
for a in M P A; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j3 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
for a in M P A; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko)"; done
for a in M P A; do echo "$a: $(modinfo -p $T/$a/module/zfs.ko | grep -cE '^(zfs_vdev_disk_sync_writes|zfs_vdev_queue_age_ms|zfs_immediate_write_sz):')"; done
modinfo -p $T/A/module/zfs.ko | grep -E '^zfs_vdev_(queue_age_ms|disk_sync_writes):'
grep -h 'warning:' $L/build-*.log | grep -E 'vdev_queue\.c|vdev_disk\.c|zio\.h|blkdev_compat\.h' | sort -u
```
Expected: rc 0 three times; modules built for `uname -r`; three different srcversions (two equal: stop and report); counts `M: 1`, `P: 2`, `A: 3`; for A one line starting `zfs_vdev_queue_age_ms:Age in ms after which active sync I/Os stop counting against limits` and one starting `zfs_vdev_disk_sync_writes:Writes submitted REQ_SYNC|REQ_IDLE, out of blk-wbt`. Quote any warning in those files. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup and rig check (about 6 min, functional)
```
L=/var/log/task-1118-vq1; cd $L/bin
sudo -n ./vq1-bench.sh setup; echo rc=$?; tail -2 $L/driver.log; cat $L/setup-devices.txt
```
Expected: rc 0; for each of `vq0`-`vq4`: `sched ... [mq-deadline] ...`, `rot 1`, `wbt_lat_usec 75000` (the rotational default; each run then writes 4000), `write_cache write_back`, `mbps 150`, `completion_nsec 8000000`, `hw_queue_depth 32`, `cache_size 32`, and a number after `wbt_debugfs`. Record `nr_requests` and `max_sectors_kb`, and the setup line (size, alloc, cap, MemAvailable). If setup stops, run `teardown` before trying once more.
```
L=/var/log/task-1118-vq1; cd $L/bin
sudo -n env RAMP=5 MEAS=20 ./vq1-bench.sh pass C "M P1 P1A P1I"; echo rc=$?
python3 -I ./vq1-report.py $L/raw C > $L/summary-c.md; echo rc=$?
sed -n '/^## Rig checks/,/^## Arm order/p' $L/summary-c.md; grep -A6 '^| arm | fsync n | fsync mean' $L/summary-c.md
```
Check, in summary-c.md's "Rig checks" and "Noise and identity" tables: rc 0 twice; M's srcversion is M's from step 2, P1's and P1I's are P's, P1A's is A's; `sync_writes` `absent` on M and `1` on the others; `age_ms` `50` on P1A and `absent` on the others; `immediate_write_sz` `2097152` on P1I and `32768` on the others; `wbt_lat_usec` `4000` on all four. **Stop and report** (after `teardown`) if one differs; if M's `wb_bg < 4 %` is `0.0` or `-` (blk-wbt never scaled down: the rig fails its purpose, a finding); if P1I's `indirect/s` is not `0.0` or its `needcopy/s` is not above 0; if any other arm's `needcopy/s` is not `0.0` or its `indirect/s` is not above 0. Note without stopping: each arm's `wb_bg <= 1 %`, `leaf-s above max` and `sync w activ max` (the age guard firing or not), `ARC dd hit %`, writer MB/s and fsync mean. Leave the pool imported and idle.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's cmdline, kernel configs, bash and debugfs lines; the build table with rc, times, vermagic and srcversions; `setup-devices.txt` and the setup line; summary-c.md's "Rig checks", "Noise and identity" and "Load and pressure" tables) and, as the very last line, exactly:

`TASK-1118-VQ1 READY-TO-MEASURE (about 60 min alone on the host, at most 90). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1118-VQ1`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1118-VQ1`, go to step 7 without running anything. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 4 min)
```
L=/var/log/task-1118-vq1; cd $L/bin
date -u; sudo -n ./vq1-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n env RAMP=5 MEAS=20 ./vq1-bench.sh pass W "M P1 P1I P1A"; echo rc=$?
grep -H -E '^(arm|srcversion|zfs_vdev_disk_sync_writes|zfs_vdev_queue_age_ms|zfs_immediate_write_sz)' $L/raw/W/*/layout.txt; grep -h -o 'wbt_lat_usec [0-9]*' $L/raw/W/*/layout.txt | sort | uniq -c
```
If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W, check: rc 0; the srcversions and parameters as in step 3; `20 wbt_lat_usec 4000`. If one differs, run `teardown` and report.

### 6. The eight passes (about 55 min)
```
L=/var/log/task-1118-vq1; cd $L/bin
sudo -n ./vq1-bench.sh pass P1 "M P1 P1I P1A"; echo rc=$?
python3 -I ./vq1-report.py $L/raw P > $L/summary.md; echo rc=$?
```
After P1, check in summary.md before going on: the "Rig checks" table shows M's `wb_bg < 4 %` above 0, P1I's `indirect/s` `0.0` and the other arms' `needcopy/s` `0.0` (if not: stop, `teardown`, report). Note also, without stopping, per arm the fsync mean, read p99, writer MB/s and `leaf-s above max` of P1. Then, one command each or one chain:
```
L=/var/log/task-1118-vq1; cd $L/bin
sudo -n ./vq1-bench.sh pass P2 "P1 P1A M P1I"; echo rc=$?
sudo -n ./vq1-bench.sh pass P3 "P1A P1I P1 M"; echo rc=$?
sudo -n ./vq1-bench.sh pass P4 "P1I M P1A P1"; echo rc=$?
```
Apply the **time rule** now (section Passes) and write its figures into `$L/time-rule.txt`. Then the passes it leaves, in this order:
```
L=/var/log/task-1118-vq1; cd $L/bin
sudo -n ./vq1-bench.sh pass P5 "M P1 P1I P1A"; echo rc=$?
sudo -n ./vq1-bench.sh pass P6 "P1 P1A M P1I"; echo rc=$?
sudo -n ./vq1-bench.sh pass P7 "P1A P1I P1 M"; echo rc=$?
sudo -n ./vq1-bench.sh pass P8 "P1I M P1A P1"; echo rc=$?
```
When the last run has ended, write in your chat the line `TASK-1118-VQ1 MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 7 without waiting.

### 7. Report data, teardown, end
```
L=/var/log/task-1118-vq1; cd $L/bin
python3 -I ./vq1-report.py $L/raw P > $L/summary.md; echo rc=$?
python3 -I ./vq1-report.py $L/raw W > $L/summary-w.md; echo rc=$?
sudo -n ./vq1-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /dev/vq[0-4] 2>&1; ls /sys/kernel/config/nullb 2>&1
sudo -n dmesg | sed -n '/task-1118-vq1: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L raw && sudo -n rm -rf /var/tmp/task-1118-vq1 && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, no `/dev/vq*`, no null_blk device, no kernel line (report any, with the step it happened in). Keep `/var/log/task-1118-vq1` (raw data included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build, two equal srcversions, or a parameter count other than step 2's; a setup that fails twice or a device not as expected; a failed rig check (step 3) or a failed check after W or P1; a driver `STOP:` (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or hung task in the kernel log (capture, report, reboot only if wedged); more than 2 h 20 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict against the expectations, the age guard never firing, or a pass dropped under the time rule is a finding, not a stop.

## Report

`/var/log/task-1118-vq1/report.md`, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, quoted from summary.md (recompute nothing by eye):
   a. **The rig**: did blk-wbt scale down ("Rig checks" `wb_bg < 4 %` and `<= 1 %`; the devices table's `wb_background` median), the writer's MB/s against the host's 51 MB/s, the ARC hit rate, and the ZIL record kinds per arm.
   b. **Age guard, P1A against P1**: did it fire (`leaf-s above max`, `sync w activ max`, the per-second queue table); the verdicts of P1A/P1 and P1A/M on every metric; syncq_wait w against disk_wait w (did the wait fall or move from the queue to the disk); writer and bulk MB/s.
   c. **Copies instead of indirect, P1I against P1**: the verdicts of P1I/P1 and P1I/M; sync_write ops/s and MB/s, async_write MB/s and device write MB/s (request-size table); the ZIL counters; txgs per run, txg sync time and the dmu_tx counters (`dmu_tx_wrlog_delay` in particular).
   d. **P1 against M**: the verdicts of P1/M; the "Threads asleep" table (z_wr_int, z_wr_iss, z_rd_int, zvol, python3 and "all in wbt") and the top z_wr_int stacks per arm.
   e. **The study's decisive reading**: per arm, the device busy % in leaf-seconds with sync writes pending against those without, the share of pending leaf-seconds below 80 % busy, and syncq_wait w against disk_wait w in those seconds.
   f. **Reads**: read p50, p99 and pooled p99.9, disk_wait r p99 and the per-second disk_wait r p99, with the rig's limit for reads (Expected) stated.
   g. **ZFS's view**: the latency table over the measured window and the per-second pool latency table, per arm.
   h. **Noise**: the quiet check; per run the steal and load1 (the identity table); any run with steal above 1 %; the arm order per pass; the time of GO and what the time rule dropped.
2. summary.md whole, then summary-c.md's and summary-w.md's "Rig checks" and "Noise and identity" tables.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, the kernel config lines, bash version, debugfs, kit checks, code checks and file hashes, build times and rc, vermagic and srcversions, setup-devices.txt, the setup line; per pass its start and end time (driver.log); `$L/time-rule.txt`; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, end state (modules, devices, `fstrim -av`, `df -h /var/tmp`), and any reboot with its UTC time and how you resumed.
