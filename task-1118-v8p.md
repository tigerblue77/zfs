TASK-1118-V8P (from the coordinator session "main"): performance, VM 107, about 1 h 40 on the VM plus the wait for main's GO, at most 2 h 15 without that wait. Its measured part (steps 5 and 6, about 80 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you send main a note and wait for main's GO; you never check the other VMs yourself. Steps 0 to 3 are functional and may run while other VMs work. Three release builds, the arm switched by reloading the modules: upstream master c913da86156, "v6", and "v8", over TASK-1118-V7P's rows n8, n8e, r1, x1, n32, w4 and w8, in Williams-square arm orders (6 passes on the disk rows, 12 on the null_blk rows). Nothing is published on GitHub, no credential files, fstrim at the end. Report: /var/log/task-1118-v8p/report.md and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`, `D`, `R`); the blocks below do.

## Background

TASK-1118-V7P measured "v6" (ZIL block writes issued by the thread that writes them out, with the write issue taskq's bio identity) against master on this VM: n8 +14.3 %, n8e +12.1 %, r1 +13.3 %, w4 (4 writers on one zvol) +22.1 % and w8 (8 writers) +15.1 %, each a gain; x1 +2.0 %, not significant; and n32 (32 writers on one zvol) 0.966 of master, not significant over 9 passes (V6P: 0.944, a loss). On n32, v6 wrote more and smaller ZIL blocks than master: `lwbs/op` 0.48 against 0.39, `KiB/lwb` 65.5 against 79.7. Single passes of n32 on v6 with `zfs_commit_timeout_pct` at 10, 15 and 20 gave `lwbs/op` 0.46, 0.41 and 0.35 and 1.02, 1.06 and 1.13 times master's mean IOPS.

Read from the code: while several threads commit to one ZIL, an open lwb waits for more records for `zfs_commit_timeout_pct` (10 %) of a moving average of the lwb latency. Master times that latency from just before the lwb's write enters the zio pipeline to its flush, so it includes the wait for a `z_wr_iss` thread; v6 issues the write in the committing thread, so that wait is gone from the latency and open lwbs wait less. That this shorter wait causes the n32 difference is inferred, not proven.

**v8** is v6 plus one commit that times each lwb from its close (when it stops taking records) to its flush, on every path: the filling of the lwb, the allocation of the next one and any wait for a thread to issue it are now counted, whichever thread issues it. Nothing else changes; inline issue is as in v6 on every row. Expected from the code (report what you see, not this): on n32, v8's `lwbs/op` and `KiB/lwb` nearer master's than v6's; whether its IOPS reach master's depends on whether the time from close to issue is as long as the taskq wait it replaces, which is not known; r1 and x1 like v6 (one writer: its lwb is closed at once, no wait); `tasks/op iss` like v6 on every row.

"v7b" (V7P's variant) is not measured: a fourth arm would cut the null_blk rows from 12 passes to 8 within the 90-minute limit, and n32's decision needs the passes.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
|---|---|---|---|
| v8 | `zil-issue-v8` | `e05de821688499b2c3d346dd144b8ecd42ac9610` (parent v6 `c77b28efff06a4bb276d797f5e72a06c12421386`) | `a6e4c4abf09556b9437d6d4d8986d710eeb31a8d` |
| v6 (v8~1) | `zil-issue-v6` | `c77b28efff06a4bb276d797f5e72a06c12421386` | `0bd4ed63e56e1baaa66271a95ac961ee91fee51e` |
| master (v8~3, upstream) | - | `c913da86156eba3243cc1ef515e98d69887654a7` | `7e32938edf1a54c02519e2dc0104e9325b5a6f4d` |
| kit | `vm-kit-v8p` | `6629e46b62df397a2e89ffb79cd979ad71da3081` (parent `6cbb1780764c116a248b059c9fa84dafbbece635`) | `20f6522bb19b73c8253c307a5b74edc5e91cfbdf` |

The kit's files for this task: `v8p-bench.sh` (the driver: V7P's `v7p-bench.sh` with the three arms master, v6 and v8, same rows, pools and per-pass `raw/<pass>/order.txt`; `layout.txt` no longer has a `zil_issue_inline_committers` line), `v8p-report.py` (V7P's report with the pairs v6/master, v8/master and v8/v6, V7P's v6/master ratios beside this run's, the decision rules below and an n32 mechanism line; no M1 table), and V5P's `v5p-snap.py` and `v5p-sample.py`, unchanged (same sha256 as in V7P). The other files of the kit belong to earlier tasks.

## Rows

| Row | Pool | Workload | Measured |
|---|---|---|---|
| x1 | c1q: mirror of 2 × 3 GiB files, filled before the pool, on direct-I/O loop devices | 4K random writes on c1q/fs (recordsize 4K), each followed by an fsync, 1 job; plus 1M sequential writes at 50 MiB/s into c1b/bulk (gzip-9; c1b is one sparse 3 GiB file), started 4 s before | 10 s |
| r1 | c1r: the test disk alone | the same 4K write + fsync job on c1r/fs, no bulk writer | 12 s |
| n8 | c1n: raidz1 of 5 × 512 MiB memory-backed null_blk | 16K random `O_SYNC` direct writes, 2 jobs on each of 4 zvols (128M, volblocksize 16K), no slog | 10 s |
| n8e | c1n | as n8, on the 4 zvols of c1n/enc (aes-256-gcm) | 10 s |
| n32 | c1n | 32 jobs of 16K random `O_SYNC` direct writes on the zvol c1n/v0 | 10 s |
| w4 | c1n | as n32 with 4 jobs | 10 s |
| w8 | c1n | as n32 with 8 jobs | 10 s |

x1 and r1 are the disk rows; the five others are the null_blk rows. Each row starts with a `zpool sync` of all pools and a 3 s fio ramp, and records under `raw/<pass>/<arm>/` the fio JSON and per-second IOPS logs, a snapshot before and after (CPU, steal, load1, disk writes, flushes, CPU and run-queue wait per thread class, taskq and ZIL counters, txg history), the write taskqs sampled 10 times a second and `zpool iostat -lq` every second.

## Passes

The arm orders form a Williams design for three arms: P1 to P6 put each arm twice in each position and let each arm follow each other arm exactly twice, and P1 to P3 and P4 to P6 are each a Latin square (each arm once per position). P7 to P12 repeat P1 to P6's orders on the null_blk rows only. The disk rows run in P1 to P6.

| Pass | Rows | Arm order | |
|---|---|---|---|
| W | x1 r1 n8 | master v6 v8 | warm-up, not reported |
| P1 | x1 r1 n8 n8e n32 w4 w8 | master v6 v8 | |
| P2 | x1 r1 n8 n8e n32 w4 w8 | v6 v8 master | |
| P3 | x1 r1 n8 n8e n32 w4 w8 | v8 master v6 | |
| P4 | x1 r1 n8 n8e n32 w4 w8 | v8 v6 master | |
| P5 | x1 r1 n8 n8e n32 w4 w8 | master v8 v6 | time rule before it |
| P6 | x1 r1 n8 n8e n32 w4 w8 | v6 master v8 | |
| P7 | n8 n8e n32 w4 w8 | master v6 v8 | |
| P8 | n8 n8e n32 w4 w8 | v6 v8 master | |
| P9 | n8 n8e n32 w4 w8 | v8 master v6 | |
| P10 | n8 n8e n32 w4 w8 | v8 v6 master | |
| P11 | n8 n8e n32 w4 w8 | master v8 v6 | |
| P12 | n8 n8e n32 w4 w8 | v6 master v8 | |

Projected, from V7P's driver.log (an arm switch about 18.5 s, a null_blk row about 16.5 s, x1 plus r1 about 40.5 s per arm): quiet check 1 min, W about 4 min, P1 to P6 about 7.1 min each, P7 to P12 about 5.1 min each, the checks between passes about 2 min: **about 80 min from GO** to the end of P12.

**Time rule** (keeps the measured part under 90 min). Before P5, from driver.log: d = the mean duration of P1 to P4 (`pass Pn start` to `pass Pn done`), and the projected end = now + 2 × d + 6 × 0.71 × d + 2 min. If it passes GO + 90 min, drop in this order, recomputing after each (a null_blk row of one arm takes about 16.5 s, a pass of three arms about 50 s per row plus 56 s of arm switches), until it does not: (1) w4 and w8 from P7 to P12 (they keep P1 to P6, the whole design; about 10 min less); (2) P10 to P12 whole (P7 to P9 stay, so the null_blk rows keep 9 passes with each arm three times in each position; about 10 min less after (1)); (3) n8e from P7 to P9 (about 2.5 min). Never drop an arm; never drop a row from P1 to P6, nor n8 or n32 from P7 to P9. If after (3) the projection still passes GO + 1 h 40, run no further pass after P6: go to step 7 and say so. Say what was dropped and why.

## Written before the run: the rules that decide

- **Verdict per row and pair** (`v8p-report.py raw P`, as V7P): for a pair a/b, over the passes where both arms ran the row, ratio = a IOPS / b IOPS in the same pass; "gain" if the geometric mean ≥ 1.03 and the exact sign-flip permutation p ≤ 0.05, "loss" if ≤ 0.97 and p ≤ 0.05, otherwise "no difference shown". **"Significantly below"**: geometric mean < 1 and p perm ≤ 0.05, whatever the size of the drop. Pairs: v6/master, v8/master, v8/v6.
- **Tail rule** (fsync rows r1, x1, per pair): with t = a fsync p99.9 / b fsync p99.9 per pass, "tail cost" if geo(t) ≥ 1.20 and p perm ≤ 0.05, "tail gain" if geo(t) ≤ 0.83 and p perm ≤ 0.05, otherwise "no tail difference shown".
- **Decision rules for v8**; means are over the passes:
  1. **n32 (remove the loss):** v8/master not "significantly below", and v8/v6 "gain".
  2. **n8, n8e, r1, w4 and w8, each (keep v6's gains):** v8/v6 not "significantly below".
  3. **Tails and x1:** on r1 and x1, v8/master and v8/v6 not "tail cost"; on x1, v8/master not "loss".
  4. **Every row:** v8's `busy CPU us/op` ≤ 1.03 × max(master's, v6's).

  v8 failing any rule, or with a rule it lacks the data for ("not evaluated"), is reported, not recommended. The report prints each rule's figures and result and a closing line; quote them, and recompute none by eye. The **n32 mechanism line** under the rules (v8's `lwbs/op`, `KiB/lwb`, `writer passes/op` and `fast path %`, each against master's and v6's, and which one it is nearer) decides nothing: quote it.
- **Noise:** steal above 1 % on a row is listed with its pass; load1 at the end of a row counts the row's own jobs (n32 runs 32), so it is listed per pass, not judged.

## Rules for this task

- Trees: /var/tmp/task-1118-v8p/repo (one blob-less clone) with the worktrees /var/tmp/task-1118-v8p/master, /var/tmp/task-1118-v8p/v6 and /var/tmp/task-1118-v8p/v8 (release builds only). Logs and results: /var/log/task-1118-v8p/. Work as root through `sudo -n` as usual. The driver puts the arm's tree first in PATH and checks that `zpool` comes from it.
- The pools sit on direct-I/O loop devices over files in /var/tmp/task-1118-v8p/vdev on the VM disk, on null_blk devices `c1nb0`-`c1nb4` (2.5 GiB of RAM at most), and on the one test disk chosen in step 0. The driver creates and destroys them. Touch no other pool, disk or null_blk device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6 (no build, ztest, ZTS, second fio or bpftrace).
- Disk cap, as in V7P: the driver logs the GB written to the VM disk and the test disk since setup, and refuses a pass or a disk row that would pass 95 GB (a `CAP:` line, exit status 3). Never raise `CAP_GB`. About 41 GB are expected (V7P: 11.4 GB at setup, about 1.42 GB per arm and pass with the disk rows). After a `CAP:` exit, run no further pass holding x1 or r1; P7 to P12 may still run.
- Packages: `fio` may be installed if missing; keep the apt output in $L/apt-install.log and at the end purge exactly the packages it lists as NEW. Nothing else is installed.
- If a fetch or the clone fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass, a chain of passes): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes. Several passes may run as one background chain that stops at the first non-zero exit status, as long as the checks after P1 and the time rule before P5 are applied where they stand.
- If the driver stops with an error other than the cap: run `teardown`, then report; do not retry a measured pass on your own. If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or a hung task: stop, capture `sudo -n dmesg | tail -100` into $L/dmesg-stop.txt, report, and wait for main (no reboot).

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; findmnt -no FSTYPE --target /var/tmp
lsmod | grep -E '^(zfs|spl|null_blk) '; ls /sys/kernel/config/nullb 2>&1; ls -d /var/tmp/task-1118-v8p /var/log/task-1118-v8p 2>&1
command -v fio python3 git make gcc; fio --version; python3 --version
cat /sys/fs/cgroup/cgroup.subtree_control; c=$(sed -n 's/^0:://p' /proc/$$/cgroup); echo "my cgroup $c io.stat=$([ -e "/sys/fs/cgroup$c/io.stat" ] && echo yes || echo no)"
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace'
DK=$(lsblk -no PKNAME "$(findmnt -no SOURCE --target /var/tmp)"); [ -n "$DK" ] || DK=$(basename "$(findmnt -no SOURCE --target /var/tmp)"); echo "DISK=$DK"; awk -v d="$DK" '$3 == d' /proc/diskstats | wc -l
```
Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a ZFS or null_blk module is loaded or a null_blk device exists; one of the two task directories exists; /var/tmp has less than 30 GiB free; less than 6000 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `io` is in the root `cgroup.subtree_control` or your cgroup has an `io.stat` (fio would not be in the root io cgroup); a build, test, fio or bpftrace runs; DISK does not name exactly one line of /proc/diskstats.

The test disk R, as in V7P (one of this VM's test disks, by its by-id link):
```
RD=$(lsblk -no PKNAME "$(findmnt -no SOURCE /)"); echo "root disk $RD"
for l in $(ls /dev/disk/by-id/ | grep -E '^scsi-0QEMU_QEMU_HARDDISK_drive-scsi[0-9]+$' | sort -V); do d=$(basename "$(readlink -f /dev/disk/by-id/$l)"); printf '%s %s lines=%s sigs=%s mounts=%s bytes=%s rot=%s\n' "$l" "$d" "$(lsblk -n -o NAME /dev/$d | wc -l)" "$(sudo -n wipefs --noheadings /dev/$d | wc -l)" "$(lsblk -n -o MOUNTPOINTS /dev/$d | grep -c .)" "$(lsblk -bdno SIZE /dev/$d)" "$(cat /sys/block/$d/queue/rotational)"; done
```
R is `/dev/disk/by-id/<link>` of the line V7P used, `scsi-0QEMU_QEMU_HARDDISK_drive-scsi3`, if it qualifies (sdX neither the root disk nor DISK, `lines=1`, `sigs=0`, `mounts=0`, `bytes` at least 4294967296); otherwise the first qualifying line in that order (say so: r1 is then on another disk than V7P's). If none qualifies, stop and report. Then:
```
L=/var/log/task-1118-v8p; T=/var/tmp/task-1118-v8p
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
printf 'D=%s\nR=%s\nT0=%s\n' "<DISK>" "<R>" "<T0>" > $L/env
```

### 1. Kit, fetched from the fork branch
```
L=/var/log/task-1118-v8p; T=/var/tmp/task-1118-v8p
cd $L/bin && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs vm-kit-v8p
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}' 'FETCH_HEAD~1'
test "$(git rev-parse FETCH_HEAD)" = 6629e46b62df397a2e89ffb79cd979ad71da3081 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 20f6522bb19b73c8253c307a5b74edc5e91cfbdf && echo kit tree OK
test "$(git rev-parse 'FETCH_HEAD~1')" = 6cbb1780764c116a248b059c9fa84dafbbece635 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    1f15cc8a9f0afb1b8712a8d1f336e2522272b71922766ee9ed6601a9d3db4230 v8p-bench.sh \
    381cc356ad0737f3d40dc728bb78de0d3245a236c73231c72722d160e0b87951 v8p-report.py \
    249a702d1c5c6814fcced3bcc3124328ff9a4c882f51edde0084f9cbe3bedeb6 v5p-snap.py \
    ba3a52f7e8884bf3726d79e8a060e86404edb61faef0c1eeb53a880d5cd288fd v5p-sample.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n v8p-bench.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l v8p-bench.sh
```
Expected: the three `OK` lines; fourteen entries, exactly `100644 c5g-sum.py`, `100755 c5g.sh`, `100644 c6g-clone.bt`, `100644 c6g-sum.py`, `100755 c6g.sh`, `100644 v5p-sample.py`, `100644 v5p-snap.py`, `100755 v6p-bench.sh`, `100644 v6p-hop.bt`, `100644 v6p-report.py`, `100755 v7p-bench.sh`, `100644 v7p-report.py`, `100755 v8p-bench.sh`, `100644 v8p-report.py`; four `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x`.

### 2. Trees and release builds (about 11 min)
```
T=/var/tmp/task-1118-v8p; L=/var/log/task-1118-v8p
git clone -q --filter=blob:none --no-checkout --single-branch --branch zil-issue-v8 https://github.com/tigerblue77/zfs.git $T/repo
cd $T/repo && git log --oneline -4 HEAD
test "$(git rev-parse HEAD)" = e05de821688499b2c3d346dd144b8ecd42ac9610 && echo v8 commit OK
test "$(git rev-parse 'HEAD^{tree}')" = a6e4c4abf09556b9437d6d4d8986d710eeb31a8d && echo v8 tree OK
test "$(git rev-parse HEAD~1)" = c77b28efff06a4bb276d797f5e72a06c12421386 && echo v6 commit OK
test "$(git rev-parse 'HEAD~1^{tree}')" = 0bd4ed63e56e1baaa66271a95ac961ee91fee51e && echo v6 tree OK
test "$(git rev-parse HEAD~3)" = c913da86156eba3243cc1ef515e98d69887654a7 && echo master commit OK
test "$(git rev-parse 'HEAD~3^{tree}')" = 7e32938edf1a54c02519e2dc0104e9325b5a6f4d && echo master tree OK
git worktree add -q --detach $T/master c913da86156eba3243cc1ef515e98d69887654a7
git worktree add -q --detach $T/v6 c77b28efff06a4bb276d797f5e72a06c12421386
git worktree add -q --detach $T/v8 e05de821688499b2c3d346dd144b8ecd42ac9610
printf '%s  %s\n' \
    be31510828342e582cdc11c93ed51d175c177e3d10b7ffe7d285ad1d5876e32d $T/master/module/zfs/zio.c \
    a4b2910a3331bfba09cae55d8710a61eb9ded2165dac0792992febe0c9618c85 $T/master/module/zfs/zil.c \
    15e2e279dd809102b98b1dfc3e7dab4cf5b93a2aa99b35b84c9975c4de87d034 $T/v6/module/zfs/zio.c \
    9b7fb749440de683e04b2248cf6242f039a12fc0c6f24139f313b5cddb2a22ba $T/v6/module/zfs/zil.c \
    15e2e279dd809102b98b1dfc3e7dab4cf5b93a2aa99b35b84c9975c4de87d034 $T/v8/module/zfs/zio.c \
    30507e4c2c58a22e846c0654e7fd5a59a1aef6f8f06586fc89b206eb6f60d467 $T/v8/module/zfs/zil.c \
    fdde60bc55338f4fd837579e66e435881b880453a17dcd439a91ac50534aaae2 $T/v6/include/sys/zil_impl.h \
    dbb13fd02fd0458891863bdcbaabb20830c2659493470edbb8dc3894226dbfdb $T/v8/include/sys/zil_impl.h > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```
Six `OK` lines and eight `OK` from `sha256sum -c` (v6 and v8 share one zio.c). Then build the three at once, `make -j2` each:
```
T=/var/tmp/task-1118-v8p; L=/var/log/task-1118-v8p
for a in master v6 v8; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j2 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
grep -H -E 'define HAVE_BIO_ALLOC_4ARG' $T/v6/zfs_config.h $T/v8/zfs_config.h
for a in master v6 v8; do echo "$a $(modinfo -F vermagic $T/$a/module/zfs.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/zfs.ko)"; done
grep -h 'warning:' $L/build-*.log | grep -E 'zio\.c|vdev_disk\.c|vdev_queue\.c|zil\.c|zil_impl\.h|zfs_context_os\.h' | sort -u
```
Expected: rc 0 three times; `#define HAVE_BIO_ALLOC_4ARG 1` in the v6 and v8 trees (they issue ZIL blocks inline on this kernel; if it is absent from either, stop and report); modules built for `uname -r`; three different srcversions (two equal: stop and report). Say whether master's and v6's srcversions are V7P's, `65D8C0C24A9B688639F0721` and `5F1377A007E20F4482C7B2D` (information only). Quote any warning in those files. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup (about 1 min, functional)
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
echo "task-1118-v8p: start" | sudo -n tee /dev/kmsg > /dev/null
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh setup; echo rc=$?; tail -2 $L/driver.log
```
`setup` loads the master modules, fills the c1q files, creates the four pools, the zvols and the encrypted dataset, and prefills them. If it stops, run `teardown` before trying once more. Record the setup duration and the GB the driver logs.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's cmdline, test disk, cpuidle lines; the build table with rc, times and srcversions; setup duration and GB) and, as the very last line, exactly:

`TASK-1118-V8P READY-TO-MEASURE (about 80 min alone on the host, at most 90). Waiting for GO.`

Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1118-V8P`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1118-V8P`, go to step 7. Nothing runs while you wait (pools imported and idle).

### 5. Quiet check and warm-up (about 5 min)
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
date -u; sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass W "x1 r1 n8" "master v6 v8"; echo rc=$?
grep -H -E '^(arm|srcversion|zio_taskq_write|zfs_commit_timeout_pct|root subtree_control)' $L/raw/W/*/layout.txt
```
If steal is above 2 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W, check: every row `rc=0` in driver.log; each arm's `layout.txt` names its own tree and the srcversion of step 2, `zio_taskq_write=sync null scale null`, `zfs_commit_timeout_pct=10`, and a root `subtree_control` without `io`. If one of these differs, run `teardown` and report.

### 6. The A/B passes (about 75 min)
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P1 "x1 r1 n8 n8e n32 w4 w8" "master v6 v8"; echo rc=$?
python3 -I $L/bin/v8p-report.py $L/raw P > $L/summary.md; echo rc=$?
```
After P1, check before going on: each arm's `layout.txt` as after W; in summary.md's Diagnostics, `tasks/op iss` of n8 lower for v6 **and for v8** than for master (V7P: 1.5 to 1.9 for master against about 0.9 inline); and `bulk MiB/s` near 50 in x1's latency table. If one of these checks fails, run `teardown` and report what you see. Note also, without stopping: n32's `lwbs/op` and `KiB/lwb` per arm, and `itxs/op` per row. Then, one command each or one chain, reading the last `pass ... done, N GB written since setup began (cap 95)` line of driver.log after each and noting N:
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P2 "x1 r1 n8 n8e n32 w4 w8" "v6 v8 master"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P3 "x1 r1 n8 n8e n32 w4 w8" "v8 master v6"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P4 "x1 r1 n8 n8e n32 w4 w8" "v8 v6 master"; echo rc=$?
```
Apply the **time rule** now (section Passes), and write its figures (d, now, projected end, GO + 90 min, what was dropped if anything) into $L/time-rule.txt. Then, with w4 and w8 removed from the rows of P7 to P12, P10 to P12 left out, or n8e removed from P7 to P9 if the rule says so:
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P5 "x1 r1 n8 n8e n32 w4 w8" "master v8 v6"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P6 "x1 r1 n8 n8e n32 w4 w8" "v6 master v8"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P7 "n8 n8e n32 w4 w8" "master v6 v8"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P8 "n8 n8e n32 w4 w8" "v6 v8 master"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P9 "n8 n8e n32 w4 w8" "v8 master v6"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P10 "n8 n8e n32 w4 w8" "v8 v6 master"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P11 "n8 n8e n32 w4 w8" "master v8 v6"; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh pass P12 "n8 n8e n32 w4 w8" "v6 master v8"; echo rc=$?
```
An exit with status 3 is the cap (a `CAP:` line): see the rules. After the last pass, check that every layout of P1 to P12 reads `zfs_commit_timeout_pct=10` (`grep -h zfs_commit_timeout_pct $L/raw/P*/*/layout.txt | sort | uniq -c`).

### 7. Report data, teardown, end
```
L=/var/log/task-1118-v8p; . $L/env; cd $L/bin
python3 -I ./v8p-report.py $L/raw P > $L/summary.md; echo rc=$?
sudo -n env DISK="$D" RDISK="$R" ./v8p-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|null_blk) '; sudo -n wipefs --noheadings "$R" | wc -l
sudo -n dmesg | sed -n '/task-1118-v8p: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
sudo -n rm -rf /var/tmp/task-1118-v8p && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, 0 signatures on R, no kernel line (report any, with the step it happened in). Purge the packages $L/apt-install.log lists as NEW, if any. Keep /var/log/task-1118-v8p (raw data included).

## Stop conditions (all in one place)

Step 0's list; no qualifying test disk; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build, `HAVE_BIO_ALLOC_4ARG` absent from the v6 or v8 tree, or equal srcversions; a failed check after W or P1; a driver error other than the cap (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or hung task in the kernel log (capture, no reboot, wait for main); more than 2 h 15 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict against expectations, v8 failing a rule or a pass dropped under the time rule is a finding, not a stop.

## Report

/var/log/task-1118-v8p/report.md, and the same text as your final message:

1. **Answers first**, each with the figures it rests on:
   a. **Decision**: the decision-rule table of summary.md for v8 (rule, row, figures, result), its closing line, and the n32 mechanism line. Say plainly whether v8 is recommended under the rules; v8 failing a rule is not.
   b. The verdict table (summary.md) for the pairs v6/master, v8/master and v8/v6 per row: geo ratio, passes won, p perm, verdict, and "significantly below" where the rules print it; beside v6/master, V7P's ratio. Does v6 reproduce V7P (n8, n8e, r1, w4, w8 gains; n32 below 1)?
   c. n32, w4 and w8 per arm: IOPS, `lwbs/op`, `KiB/lwb`, `zil_commit/op`, `writer passes/op`, `fast path %`, `tasks/op iss`, write p99 and p99.9. On n32, is v8's `lwbs/op` nearer master's or v6's?
   d. Tail rule: r1 and x1 fsync p99.9 per pair (v8/master, v8/v6, v6/master) and the verdicts; r1's fsync p50 and p99 per arm.
   e. CPU: `busy CPU us/op` per arm and row, `cpu z_wr_iss`, `tasks/op iss`. Does v8 spend more CPU per operation than max(master, v6) + 3 % anywhere?
   f. `itxs/op` per row and arm: is it near 1 (the per-operation normalization holds) or not, and by how much on the multi-job rows?
   g. Noise: the quiet check, per pass the highest steal and the load1 range (from the IOPS-per-pass tables), any row with steal above 1 %; the arm-position table of summary.md.
2. summary.md whole.
3. Setup: kernel, /proc/cmdline, CPUs, RAM, DISK and the file system of /var/tmp, the test disk (link, sdX, size, rotational), cpuidle driver and governor, kit checks, code checks and file hashes, build times and rc, srcversions, setup duration and GB; per pass its start and end time, arm order and GB written since setup (driver.log); the time of GO, the time rule's figures ($L/time-rule.txt) and what it dropped; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, packages installed and purged, end state (modules, R's signatures, `fstrim -av`, `df -h /var/tmp`).
