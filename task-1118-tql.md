TASK-1118-TQL (from the coordinator session "main"): performance, VM 105, 106 or 107 (the one main sends it to; Debian 13, kernel 6.12, 8 vCPU), about 1 h 35 on the VM plus the wait for main's GO, at most 2 h 15 without that wait. Its measured part (steps 5 and 6, about 70 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you send main a note and wait for main's GO; you never check the other VMs yourself. Steps 0 to 3 are functional and may run while other VMs work. Two release builds, the arm switched by reloading the spl and zfs modules: upstream master `cde5d598eb4` and "tql", the same plus one commit to the SPL taskq (`module/os/linux/spl/spl-taskq.c` only). Nothing is published on GitHub, no credential files, fstrim at the end. Report: /var/log/task-1118-tql/report.md and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`); the blocks below do.

Reboot when needed, without waiting for main (a module that will not unload, a hung task once its diagnostics are saved): save the diagnostics first, say in the report that you rebooted and why. A measured pass is never resumed after a reboot: go to step 7.

## Background

The SPL taskq's `tq_lock` is a spinlock held with interrupts disabled; every dispatch takes it, and every worker takes it to pick up a task and to complete it. On master, `taskq_dispatch()` and `taskq_dispatch_ent()` call `wake_up()` on the work wait queue under it, and a worker calls `wake_up_all()` on the waiters' queue under it after each task, even when nobody sleeps on that queue (a wake-up on an empty queue still takes and releases the queue's own spinlock). On 5 November, on VM 107 with 16 vCPUs, `perf lock contention` named `tq_lock` of the single `z_wr_iss` taskq the most contended named lock on buffered writes (80 s of total wait at 8 jobs, 98 s at 16).

**tql** (one commit): the dispatchers wake only when `wq_has_sleeper()` finds a worker on the work queue; a worker that completes a task wakes the waiters only if there are any, and after it has dropped `tq_lock` rather than under it. The dispatchers' wake-up of a sleeping worker stays under `tq_lock` (it cannot move without a change to `taskq_destroy()`).

Expected from the code (report what you see, not this): any difference on the write rows w4 and w8, where `z_wr_iss` is busy and its workers rarely sleep, so most dispatches skip the wake-up; little or none on r8, whose reads complete synchronously on brd and wake a `z_rd_int` worker that is usually asleep (that wake-up is unchanged); none on c8 (ARC hits, no taskq) and s8 (control). At 8 vCPU there is one `z_wr_iss` taskq of about 6 threads, against 12 threads on the 16-vCPU VM, so the contention may be too low to show anything: "no gain at 8 vCPU" is a valid outcome.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
|---|---|---|---|
| tql | `spl-taskq-wake-outside-lock` | `b378a6d607869c0ac29f3528113f769ae8ca41fa` (parent master) | `2c28b5391e5350ea26b8496d7a30ca810c82bc43` |
| master (tql~1, upstream) | - | `cde5d598eb44b5d6efe8167666d988a496346657` | `d70a147b0775bf4bd216072a8b05738b603d6849` |
| kit | `vm-kit-tql` | `36c97106f103b0dabecc19542d6de7d085aa86cf` (no parent) | `300ed33e267a7a4538b2939dbb8be3c9fd6156a9` |

The kit holds two files: `tql-bench.sh` (the driver: `setup`, `quiet SECONDS`, `pass NAME ROWS ARMS`, `lock NAME ROWS ARMS`, `teardown`; read its header) and `tql-report.py` (the report: per-pass ratios, verdicts, the decision rules below, the perf lock table, taskq and noise tables).

## Rows

One pool `tqlp` on one 4 GiB brd RAM disk (`/dev/ram0`), ashift 12; the ARC is capped at 1 GiB and `zfs_dirty_data_max` set to 256 MiB at each module load. Each row starts with a `zpool sync` and 2 s of sleep, then fio runs 5 s of ramp and 30 s measured (`psync`, buffered I/O). The CPU window is /proc/stat from 1 s after the ramp to 1 s before the end (28 s).

| Row | Dataset | Workload | Metric |
|---|---|---|---|
| c8 | tqlp/cr (recordsize 4K, primarycache=all) | 4K random reads, 8 jobs, one 256M file (ARC hits): null control | IOPS |
| r8 | tqlp/ur (recordsize 4K, primarycache=metadata) | 4K random reads, 8 jobs, one 512M file: every read goes to brd | IOPS |
| s8 | tqlp/sw (recordsize 4K, primarycache=metadata) | 4K random O_SYNC writes, 8 jobs, one 32M file each: control | IOPS |
| w4 | tqlp/w (recordsize 128K, lz4, primarycache=metadata) | 128K sequential buffered writes of half-compressible data, 4 jobs, one 128M file each | MiB/s |
| w8 | tqlp/w | the same, 8 jobs | MiB/s |

Per row and arm the driver keeps under `raw/<pass>/<arm>/` the fio JSON, the CPU window (`.cpu`), and before and after the row (`.snap0`, `.snap1`) the dmu_tx counters, the ARC demand data hits and misses, and per taskq `tasks_executed`, `thread_wakeups`, `thread_wakeups_nowork` and `thread_sleeps`; per arm, `layout.txt` (tree, spl and zfs srcversions, tunables, brd size, cpuidle, taskq thread counts).

## Passes

Arm order alternates so that each arm runs first in half the passes: P1 to P4 and P5 to P8 each put each arm first twice, and so do P1 to P6.

| Pass | Rows | Arm order | |
|---|---|---|---|
| W1 | r8 w8 | master tql | warm-up, not reported |
| P1 | c8 r8 s8 w4 w8 | master tql | |
| P2 | c8 r8 s8 w4 w8 | tql master | |
| P3 | c8 r8 s8 w4 w8 | tql master | |
| P4 | c8 r8 s8 w4 w8 | master tql | |
| P5 | c8 r8 s8 w4 w8 | master tql | |
| P6 | c8 r8 s8 w4 w8 | tql master | |
| P7 | c8 r8 s8 w4 w8 | tql master | time rule before it |
| P8 | c8 r8 s8 w4 w8 | master tql | |
| K | r8 w4 w8 | master tql | perf lock contention, one 20 s capture per row and arm; not used for throughput |

Projected (a row about 40 s, an arm switch about 25 s): quiet check 1 min, W1 about 3.5 min, each of P1 to P8 about 7.2 min, K about 5 min: **about 67 min from GO** to the end of K.

**Time rule** (keeps the measured part under 90 min). Before P7, from driver.log: d = the mean duration of P1 to P6 (`pass Pn start` to `pass Pn done`), and the projected end = now + 2 × d + 6 min. If it passes GO + 90 min, do not run P7 and P8 (P1 to P6 keep the balanced design), and run K. If now + 6 min passes GO + 1 h 40, do not run K either: go to step 7 (rule B is then "not evaluated"). Say what was dropped and why.

## Written before the run: the rules that decide

The report (`tql-report.py raw P`) applies them; quote its figures, recompute none by eye.

- **Verdict per row:** over the passes, ratio = tql / master of the row's metric in the same pass; "gain" if the geometric mean ≥ 1.03 and the exact sign-flip permutation p ≤ 0.05, "loss" if ≤ 0.97 and p ≤ 0.05, otherwise "no difference shown". **Spread** of a row: (max − min) / (2 × median) of master's metric over the passes.
- **Rule A** (r8, w4, w8, each): tql's ratio exceeds 1 + max(3 %, the row's spread) in every pass.
- **Rule B** (r8, w4, w8, each): CPU-seconds per GiB ratio geo ≤ 0.95 with p ≤ 0.05, throughput geo between 0.97 and 1.03, and in pass K the `tq_lock` wait (sum of the wait totals of the callers named `taskq_*`) of tql at most 0.7 × master's on that row.
- **No row losing:** no row of c8, r8, s8, w4 and w8 has the verdict "loss".
- **Decision:** "gain" if rule A or rule B holds on at least one of r8, w4 and w8 and no row is losing; otherwise "no gain at 8 vCPU". Either is a finding; the report's closing line states it.
- **Noise:** steal above 1 % on a row is listed with its pass; load1 at the end of a row counts the row's own 8 jobs, so it is listed, not judged; dmu_tx_quota, dirty-data delays and the ARC data miss % per row are listed (r8 should miss nearly every read, c8 nearly none).

## Rules for this task

- Trees: /var/tmp/task-1118-tql/repo (one blob-less clone) with the worktrees /var/tmp/task-1118-tql/master and /var/tmp/task-1118-tql/tql (release builds only). Logs and results: /var/log/task-1118-tql/. Work as root through `sudo -n` as usual. The driver puts the arm's tree first in PATH, checks that `zpool` comes from it, and at each arm switch checks that the loaded spl and zfs srcversions are that tree's.
- The pool sits on `/dev/ram0` from the brd module, which the driver loads (`rd_size` 4 GiB) and unloads. Touch no other pool, disk or device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6 (no build, ztest, ZTS, second fio, bpftrace or perf other than the driver's).
- Packages: `fio` and `linux-perf` may be installed if missing; keep the apt output in $L/apt-install.log and at the end purge exactly the packages it lists as NEW. Nothing else is installed.
- If a fetch or the clone fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass, a chain of passes): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes. Several passes may run as one background chain that stops at the first non-zero exit status, as long as the checks after P1 and the time rule before P7 are applied where they stand.
- If the driver stops with an error: run `teardown`, then report; do not retry a measured pass on your own. If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or a hung task: stop, capture `sudo -n dmesg | tail -100` into $L/dmesg-stop.txt, report (reboot first only if the VM is unusable, as said above).

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; lscpu | grep -E '^(Socket|Core|Thread|NUMA node)'; grep -m1 'model name' /proc/cpuinfo; free -m; df -h /var/tmp
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1; ls -d /var/tmp/task-1118-tql /var/log/task-1118-tql 2>&1
ls -d /lib/modules/$(uname -r)/build; modinfo -F filename brd
command -v fio perf python3 git make gcc; fio --version; python3 --version
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|perf'
```
Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; `nproc` is not 8; a zfs, spl or brd module is loaded or `/dev/ram0` exists; one of the two task directories exists; the kernel build directory or the brd module is missing; /var/tmp has less than 20 GiB free; less than 6500 MB available; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; a build, test, fio, bpftrace or perf runs. Then, only if `fio` or `perf` is missing:
```
L=/var/log/task-1118-tql; sudo -n mkdir -p $L && sudo -n chown "$(id -un)" $L
sudo -n apt-get install -y fio linux-perf > $L/apt-install.log 2>&1; echo rc=$?; grep -A3 'NEW packages' $L/apt-install.log
```
And in every case:
```
L=/var/log/task-1118-tql; T=/var/tmp/task-1118-tql
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
perf --version; sudo -n perf lock contention -h 2>&1 | grep -cE -- '--use-bpf|-b,'
printf 'T0=%s\n' "<T0>" > $L/env
```

### 1. Kit, fetched from the fork branch
```
L=/var/log/task-1118-tql; T=/var/tmp/task-1118-tql
cd $L/bin && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs vm-kit-tql
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}'; git rev-parse -q --verify 'FETCH_HEAD~1' || echo "no parent"
test "$(git rev-parse FETCH_HEAD)" = 36c97106f103b0dabecc19542d6de7d085aa86cf && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 300ed33e267a7a4538b2939dbb8be3c9fd6156a9 && echo kit tree OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    ebe1d64e8b94ab1a2654f25f9fa94346b7f32ed1a70c47d23fe3a292c3afd34b tql-bench.sh \
    59eed5b0ad79c1147d15c360a4ca29f1b0ded6efa6d8eee2abb52fac16be1fcd tql-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n tql-bench.sh && echo syntax-ok; python3 -I -c 'import ast; ast.parse(open("tql-report.py").read())' && echo py-ok; git status --porcelain | wc -l; ls -l tql-bench.sh
```
Expected: `no parent`; the two `OK` lines; exactly two entries, `100755 tql-bench.sh` and `100644 tql-report.py`; two `OK` from `sha256sum -c`; `syntax-ok`; `py-ok`; 0; `-rwxr-xr-x`.

### 2. Trees and release builds (about 15 min)
```
T=/var/tmp/task-1118-tql; L=/var/log/task-1118-tql
git clone -q --filter=blob:none --no-checkout --single-branch --branch spl-taskq-wake-outside-lock https://github.com/tigerblue77/zfs.git $T/repo
cd $T/repo && git log --oneline -2 HEAD
test "$(git rev-parse HEAD)" = b378a6d607869c0ac29f3528113f769ae8ca41fa && echo tql commit OK
test "$(git rev-parse 'HEAD^{tree}')" = 2c28b5391e5350ea26b8496d7a30ca810c82bc43 && echo tql tree OK
test "$(git rev-parse HEAD~1)" = cde5d598eb44b5d6efe8167666d988a496346657 && echo master commit OK
test "$(git rev-parse 'HEAD~1^{tree}')" = d70a147b0775bf4bd216072a8b05738b603d6849 && echo master tree OK
git diff --stat HEAD~1 HEAD
git worktree add -q --detach $T/master cde5d598eb44b5d6efe8167666d988a496346657
git worktree add -q --detach $T/tql b378a6d607869c0ac29f3528113f769ae8ca41fa
printf '%s  %s\n' \
    b8b0089493527d63dc1769efae43b92a69561a533cd50e081dd23b794f79d32d $T/master/module/os/linux/spl/spl-taskq.c \
    25ff60a69ff79b2fbc8b64dcc73ea9c4a49ceeba90c7ba29b12424306f0c8e5d $T/tql/module/os/linux/spl/spl-taskq.c > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
grep -c wq_has_sleeper $T/master/module/os/linux/spl/spl-taskq.c $T/tql/module/os/linux/spl/spl-taskq.c
```
Expected: four `OK` lines; the diff stat names only `module/os/linux/spl/spl-taskq.c` (35 insertions, 3 deletions); two `OK` from `sha256sum -c`; `wq_has_sleeper` counted 0 in master and 3 in tql. Then build the two at once, release (no `--enable-debug`), `make -j4` each:
```
T=/var/tmp/task-1118-tql; L=/var/log/task-1118-tql
for a in master tql; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j4 > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
for a in master tql; do for m in spl zfs; do echo "$a $m $(modinfo -F vermagic $T/$a/module/$m.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/$m.ko)"; done; done
grep -h 'warning:' $L/build-*.log | grep -E 'spl-taskq\.c' | sort -u
```
Expected: rc 0 twice; modules built for `uname -r`; the two spl srcversions different (equal: stop and report); the zfs srcversions are information only. Quote any warning in spl-taskq.c. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop.

### 3. Setup (about 2 min, functional)
```
L=/var/log/task-1118-tql; cd $L/bin
echo "task-1118-tql: start" | sudo -n tee /dev/kmsg > /dev/null
t=$(date +%s); sudo -n ./tql-bench.sh setup; echo "rc=$? $(( $(date +%s) - t )) s"; tail -2 $L/driver.log
cat $L/setup-zpool-list.txt $L/setup-zfs-list.txt; free -m
```
`setup` loads master's modules, loads brd, creates the pool and its four datasets and writes the files. If it stops, run `teardown` before trying once more. Record the setup duration, the pool's allocated size and the datasets' compress ratios (tqlp/w near 2), and MemAvailable after setup.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's cmdline, nproc and lscpu lines, cpuidle lines; the build table with rc, times and srcversions; setup duration, pool size, MemAvailable) and, as the very last line, exactly:

`TASK-1118-TQL READY-TO-MEASURE (about 70 min alone on the host, at most 90). Waiting for GO.`

Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1118-TQL`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1118-TQL`, go to step 7. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 5 min)
```
L=/var/log/task-1118-tql; cd $L/bin
date -u; sudo -n ./tql-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n ./tql-bench.sh pass W1 "r8 w8" "master tql"; echo rc=$?
grep -H -E '^(arm|spl srcversion|zfs srcversion|nproc|zfs_arc_max|zfs_dirty_data_max|brd)' $L/raw/W1/*/layout.txt
python3 -I $L/bin/tql-report.py $L/raw W > $L/warmup.md; echo rc=$?; grep -A6 '^## Per row and arm' $L/warmup.md
```
If steal is above 1 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W1, check: every row `rc=0` in driver.log; each arm's `layout.txt` names its own tree, its spl srcversion of step 2, `nproc 8`, `zfs_arc_max=1073741824`, `zfs_dirty_data_max=268435456` and `brd 8388608 sectors`; in warmup.md, r8's data miss % above 95 for both arms. If one of these differs, run `teardown` and report.

### 6. The A/B passes (about 65 min)
```
L=/var/log/task-1118-tql; cd $L/bin
sudo -n ./tql-bench.sh pass P1 "c8 r8 s8 w4 w8" "master tql"; echo rc=$?
python3 -I $L/bin/tql-report.py $L/raw P > $L/summary.md; echo rc=$?; grep -A12 '^## Per row and arm' $L/summary.md
```
After P1, check before going on: each arm's `layout.txt` as after W1; in summary.md's per-row table, r8's data miss % above 95 and c8's below 5 for both arms. If one of these fails, run `teardown` and report what you see. Note also, without stopping: the dmu_tx quota and dirty_delay columns of w4 and w8 (quota should be near 0), and the taskq table's `sleeps/task` of the `z_wr_iss` and `z_rd_int` rows. Then, one command each or one chain:
```
L=/var/log/task-1118-tql; cd $L/bin
sudo -n ./tql-bench.sh pass P2 "c8 r8 s8 w4 w8" "tql master"; echo rc=$?
sudo -n ./tql-bench.sh pass P3 "c8 r8 s8 w4 w8" "tql master"; echo rc=$?
sudo -n ./tql-bench.sh pass P4 "c8 r8 s8 w4 w8" "master tql"; echo rc=$?
sudo -n ./tql-bench.sh pass P5 "c8 r8 s8 w4 w8" "master tql"; echo rc=$?
sudo -n ./tql-bench.sh pass P6 "c8 r8 s8 w4 w8" "tql master"; echo rc=$?
```
Apply the **time rule** now (section Passes), and write its figures (d, now, projected end, GO + 90 min, what was dropped if anything) into $L/time-rule.txt. Then, unless the rule drops them:
```
L=/var/log/task-1118-tql; cd $L/bin
sudo -n ./tql-bench.sh pass P7 "c8 r8 s8 w4 w8" "tql master"; echo rc=$?
sudo -n ./tql-bench.sh pass P8 "c8 r8 s8 w4 w8" "master tql"; echo rc=$?
sudo -n ./tql-bench.sh lock K "r8 w4 w8" "master tql"; echo rc=$?
ls $L/raw/K/*/; head -12 $L/raw/K/master/w8.lock.txt $L/raw/K/tql/w8.lock.txt
```
If a `.lock-b.txt` file exists, `perf lock contention -b` failed and the driver fell back to `perf lock record` for 10 s: quote that file's first 5 lines. After the last pass, check that every layout of P1 to P8 names `zfs_arc_max=1073741824` (`grep -h zfs_arc_max $L/raw/P*/*/layout.txt | sort | uniq -c`).

### 7. Report data, teardown, end
```
L=/var/log/task-1118-tql; cd $L/bin
python3 -I ./tql-report.py $L/raw P > $L/summary.md; echo rc=$?
sudo -n ./tql-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1
sudo -n dmesg | sed -n '/task-1118-tql: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
sudo -n rm -rf /var/tmp/task-1118-tql && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, no `/dev/ram0`, no kernel line (report any, with the step it happened in). Purge the packages $L/apt-install.log lists as NEW, if any. Keep /var/log/task-1118-tql (raw data included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build or equal spl srcversions; a failed check after W1 or P1; a driver error (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or hung task in the kernel log (capture, report); more than 2 h 15 on the VM without the wait for GO (finish the current pass, then step 7, and say what did not run). A verdict against expectations, "no gain at 8 vCPU", or a pass dropped under the time rule is a finding, not a stop.

## Report

/var/log/task-1118-tql/report.md, and the same text as your final message:

1. **Answers first**, each with the figures it rests on:
   a. **Decision**: the decision-rule table of summary.md (rule, row, figures, result) and its closing line, quoted. Say plainly whether the result is "gain" or "no gain at 8 vCPU".
   b. The verdict table of summary.md (per row: passes, geo ratio, passes won, p perm, verdict, spread, min ratio, CPU-s/GiB geo and p, p99 geo and p).
   c. The per-row table (means per arm: metric, CPU-s/GiB, p99, p99.9, data miss %, dmu_tx columns). Was dmu_tx_quota near 0 on w4 and w8?
   d. The perf lock table of pass K, and the first 12 lines of `raw/K/master/w8.lock.txt` and `raw/K/tql/w8.lock.txt`. Is `tq_lock` (callers `taskq_*`) the top named lock on w8 for master at 8 vCPU, and how does tql's wait compare?
   e. The taskq table for r8, w4 and w8: `sleeps/task` and `nowork %` of `z_wr_iss`, `z_wr_int` and `z_rd_int` per arm (how often a dispatch found a sleeping worker).
   f. Noise: the quiet check, the noise table (highest steal, load1 range, rows with steal above 1 %), and the arm-position table.
2. summary.md whole.
3. Setup: kernel, /proc/cmdline, CPUs and lscpu lines, RAM, cpuidle driver and governor, kit checks, code checks and file hashes, build times and rc, srcversions, setup duration, pool size and MemAvailable; per pass its start and end time and arm order (driver.log); the time of GO, the time rule's figures ($L/time-rule.txt) and what it dropped; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, packages installed and purged, any reboot, end state (modules, `/dev/ram0`, `fstrim -av`, `df -h /var/tmp`).
