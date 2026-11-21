TASK-1121-TQL3 (from the coordinator session "main"): performance, wake-up of SPL taskq workers after dropping `tq_lock`. Shape A only: VM 105, 106 or 107 at 8 vCPU (Debian 13, kernel 6.12, 7.8 GB), about 1 h 35 on the VM plus the wait for main's GO, at most 2 h 15 without that wait. Its measured part (steps 5 and 6, about 65 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you send main a note and wait for main's GO; you never check the other VMs yourself. Steps 0 to 3 are functional and may run while other VMs work. Two release builds, the arm switched by reloading the spl and zfs modules: upstream master `a1daebfec3f` and "twu", the same plus one commit to the SPL taskq. Throughput passes give the verdict; separate lock passes record every lock contention event with its call chain, to check the mechanism. Nothing is published on GitHub, no credential files, fstrim at the end. Report: /var/log/task-1121-tql3/report.md and the same text as your final message.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. Every note for main is the end of a turn in your own chat.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`); the blocks below do.

Reboot when needed, without waiting for main (a module that will not unload, a hung task once its diagnostics are saved): save the diagnostics first, say in the report that you rebooted and why. A measured pass is never resumed after a reboot: go to step 7.

## Background

The SPL taskq's `tq_lock` is a spinlock held with interrupts disabled; every dispatch takes it, and every worker takes it to pick up a task and to complete it. On master, `taskq_dispatch()` and `taskq_dispatch_ent()` wake a sleeping worker with `wake_up()` while holding it, so `try_to_wake_up()` (and the IPI it may send) lengthens every hold that finds a worker asleep; the woken worker's first lock after `schedule()` is `tq_lock` again.

TASK-1118-TQL (VM 106, 8 vCPU) measured an earlier change (dispatchers skip the wake-up of an empty queue; workers wake `taskq_wait()` callers after the unlock) and found no gain. Its lock pass printed, on r8, `ret_from_fork+0x34` as the top spinlock "caller" with 2.36 s of wait per 10 s on 8 CPUs, about 3 % of the CPU time, the same for both arms. TASK-1120-TQL2's analysis (not run on a VM) showed that perf without BPF prints as caller the frame 4 entries up the chain, so that figure is a spinlock taken directly in `taskq_thread()` (`taskq_thread` <- `kthread` <- `ret_from_fork`): `tq_lock` is the only spinlock taken there. On r8 each read completes synchronously on brd and dispatches a task to a `z_rd_int` taskq whose workers sleep after nearly every task (0.89 sleeps per task in TQL), so almost every dispatch wakes a worker under `tq_lock`.

**twu** (one commit, `module/os/linux/spl/spl-taskq.c` and `include/os/linux/spl/sys/taskq.h`): the dispatchers (and the delayed-task timer) decide under `tq_lock` whether a worker sleeps on the work queue, and wake it after dropping `tq_lock`; a dispatcher about to wake counts itself in a new `tq_nwaking`, and `taskq_destroy()` waits for that count to be zero before freeing the taskq. A woken worker leaves the wait queue before taking `tq_lock` instead of after. The worker's wake-up of `taskq_wait()` callers is unchanged (still under `tq_lock`).

Expected from the code (report what you see, not this): on r8, master's `tq_lock` wait in `taskq_thread()` near TQL's 3 %, mostly at the site after `schedule()`; twu's much lower, with some wait moving to the work queue's own lock (`__wake_up`, `add_wait_queue_exclusive`). Whether throughput moves at 8 vCPU is open: TQL's run-to-run spread on r8 was 5.6 %. Both "gain", "mechanism only" and "no effect" are findings.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
|---|---|---|---|
| twu | `spl-taskq-wake-after-unlock` | `f5511fd916318658dc40679fa4c0a624451e3e34` (parent master) | `8b5c148dd54c1d68246da65b4a1da634dd027c20` |
| master (twu~1, upstream) | - | `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` | `0b23c141b417a8542546e1f7a32662bc82bc0f92` |
| kit | `vm-kit-tql3` | `87bdde50764caa1ef8133fd1405e22b19ff83c13` (no parent) | `0ed1f9d074a46b67063061497efaf0ceb535ed2c` |

The kit holds three files: `tql3-bench.sh` (the driver: `sites`, `setup`, `quiet SECONDS`, `pass NAME ROWS ARMS`, `lock NAME ROWS ARMS`, `process`, `teardown`; read its header), `tql2-lock.py` (TASK-1120-TQL2's, unchanged: per capture the lock classes, the top takers with their call chains) and `tql3-report.py` (the summary and the rules below).

## Rows and passes

One pool `tqlp` on one 4 GiB brd RAM disk (`/dev/ram0`), ashift 12; the ARC is capped at 1 GiB and `zfs_dirty_data_max` set to 256 MiB at each module load. Each row starts with a `zpool sync` and 2 s of sleep. A throughput row runs fio 5 s of ramp and 30 s measured (`psync`, buffered I/O, latency percentiles 50/99/99.9), with a CPU window from /proc/stat from 1 s after the ramp to 1 s before the end (28 s). A lock row runs fio 5 s of ramp and 18 s more and, 2 s after the ramp, records 10 s of `lock:contention_begin` and `lock:contention_end` on every CPU with 16-frame kernel call chains into `$T/perf`; its fio results are not used.

| Row | Dataset | Workload | Metric |
|---|---|---|---|
| r8 | tqlp/ur (recordsize 4K, primarycache=metadata) | 4K random reads, 8 jobs, one 512M file: every read goes to brd | IOPS |
| s8 | tqlp/sw (recordsize 4K, primarycache=metadata) | 4K random O_SYNC writes, 8 jobs, one 32M file each | IOPS |
| w8 | tqlp/w (recordsize 128K, lz4, primarycache=metadata) | 128K sequential buffered writes of half-compressible data, 8 jobs, one 128M file each | MiB/s |

Per row and arm the driver keeps under `raw/<pass>/<arm>/` the fio JSON, `.snap0`/`.snap1` (dmu_tx counters, ARC demand data, per-taskq `tasks_executed`, `thread_wakeups`, `thread_wakeups_nowork`, `thread_sleeps`), and `.cpu` (throughput rows) or `.rec.log` (lock rows); `process` adds per lock row `.agg.md`, `.agg.tsv`, `.lock0.txt` (`perf lock contention --stack-skip 0`) and `.script.err`; per arm `layout.txt`. `sites` writes `$L/sites-<arm>.txt`: every spin lock, spin unlock and wait-queue call in the SPL taskq functions of that arm's `spl.ko` (an unlock the kernel's configuration inlines is not listed), as `function+0xRET callee landmark`, RET being the return address perf prints for the frame that took the lock and landmark the last call before it among `tsd_set` (thread start), `schedule` or `remove_wait_queue` (after sleep), an indirect call (after the task).

| Pass | Kind | Rows | Arm order | |
|---|---|---|---|---|
| W1 | throughput | r8 w8 | master twu | warm-up, not reported |
| L1 | lock | r8 s8 w8 | master twu | |
| L2 | lock | r8 s8 w8 | twu master | |
| P1 | throughput | r8 s8 w8 | master twu | |
| P2 | throughput | r8 s8 w8 | twu master | |
| P3 | throughput | r8 s8 w8 | twu master | |
| P4 | throughput | r8 s8 w8 | master twu | |
| P5 | throughput | r8 s8 w8 | master twu | |
| P6 | throughput | r8 s8 w8 | twu master | |
| P7 | throughput | r8 s8 w8 | twu master | time rule before it |
| P8 | throughput | r8 s8 w8 | master twu | |

P1 to P4, P5 to P8 and P1 to P6 each put each arm first equally often.

Projected (an arm switch about 25 s; a throughput row about 40 s, a lock row about 30 s): quiet check 1 min, W1 about 3.5 min, each L pass about 3.7 min, each P pass about 4.8 min, `process` about 12 min (12 captures), report 1 min: **about 65 min from GO** to the summary.

**Time rule** (keeps the measured part under 90 min). Before P7, from driver.log: d = the mean duration of P1 to P6 (`pass Pn start` to `pass Pn done`); projected end = now + 2 × d + 13 min. If it passes GO + 90 min, do not run P7 and P8 (P1 to P6 keep the balanced design) and go on with `process`. Say what was dropped and why.

## Written before the run: what the results decide

`tql3-report.py` applies these rules; quote its figures, recompute none by eye. "% of CPU time" = wait / (capture seconds × CPUs); a spinlock's wait is time spent waiting for it with the lock's own interrupts state (`tq_lock`: interrupts off), and in a guest with paravirtual spinlocks it includes time the vCPU was halted in the wait, so it bounds the CPU burnt from above.

- **Throughput verdict per row:** over the P passes, ratio = twu / master of the row's metric in the same pass; "gain" if the geometric mean ≥ 1.03 and the exact sign-flip permutation p ≤ 0.05, "loss" if ≤ 0.97 and p ≤ 0.05, otherwise "no difference shown". **Spread** of a row: (max − min) / (2 × median) of master's metric over the passes.
- **Rule A** (each row): twu's ratio exceeds 1 + max(3 %, the row's spread) in every pass.
- **Rule B** (each row): CPU-seconds per GiB ratio geo ≤ 0.95 with p ≤ 0.05, and throughput geo between 0.97 and 1.03.
- **Latency** (each row): p99 ratio geo ≥ 1.05 with p ≤ 0.05 is "worse", ≤ 0.95 with p ≤ 0.05 "better"; p99.9 is listed, not judged.
- **No row losing:** no row has the throughput verdict "loss" or a p99 "worse".
- **Lock passes** (means over L1 and L2, % of CPU time): TQ = the three `tq_lock` classes; WQ = the classes "taskq wait-queue lock" and "taskq wake-up, scheduler locks".
  - **M0 (the premise):** on r8, master's class `tq_lock: worker (taskq_thread)` ≥ 1.5 %. If it fails, TQL's 3 % was not `tq_lock` in `taskq_thread()`: say what the r8 master top takers are instead.
  - **M1 (the lever acts):** on r8, twu's TQ ≤ 0.5 × master's.
  - **M2 (no shift):** on r8, twu's TQ + WQ ≤ 0.7 × master's.
  - s8 and w8: the same figures are listed, not judged.
- **Decision:** "loss" if a row is losing; else "gain" if rule A or rule B holds on at least one row; else "mechanism only" if M1 and M2 hold (the lock wait goes, the 8-vCPU throughput does not move); else "no effect". The report's closing line states it.
- **Site split:** the report splits master's and twu's worker class by site (start, after sleep, after task) from the sites files; if a sites file does not give exactly one `_raw_spin_lock*` call of `taskq_thread` per site, it says "AMBIGUOUS, not split": then quote that sites file whole and say so, the rules above do not depend on it.
- **Noise:** steal above 1 % on a row is listed with its pass; load1 at the end of a row counts the row's own 8 jobs, so it is listed, not judged; the `.script.err` lines with "lost" and the "end without begin" and "begin without end" counts of each `.agg.md` head line; the arm-position table.

## Rules for this task

- Trees: /var/tmp/task-1121-tql3/repo (one blob-less clone) with the worktrees /var/tmp/task-1121-tql3/master and /var/tmp/task-1121-tql3/twu (release builds only). Captures: /var/tmp/task-1121-tql3/perf, deleted by `process` one by one. Logs and results: /var/log/task-1121-tql3/. Work as root through `sudo -n` as usual. The driver puts the arm's tree first in PATH, checks that `zpool` comes from it, and at each arm switch checks that the loaded spl and zfs srcversions are that tree's.
- The pool sits on `/dev/ram0` from the brd module, which the driver loads and unloads. Touch no other pool, disk or device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6, `process` and the report included (no build, ztest, ZTS, second fio, bpftrace or perf other than the driver's).
- Packages: `fio` and `linux-perf` may be installed if missing; keep the apt output in $L/apt-install.log and at the end purge exactly the packages it lists as NEW. Nothing else is installed.
- If a fetch or the clone fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass, a chain of passes, `process`): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes. Several passes may run as one background chain that stops at the first non-zero exit status, as long as the checks after L1 and P1 and the time rule before P7 are applied where they stand.
- If the driver stops with an error: run `teardown`, then report; do not retry a measured pass on your own (the driver refuses a pass name that already exists). If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING:` or a hung task: stop, capture `sudo -n dmesg | tail -100` into $L/dmesg-stop.txt, report (reboot first only if the VM is unusable, as said above). twu changes when a taskq can be freed: a hang in `zpool export` or a module unload (an arm switch) is a finding to report with `cat /proc/<pid>/stack` of the hung command, not something to work around.

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; lscpu | grep -E '^(Socket|Core|Thread|NUMA node)'; grep -m1 'model name' /proc/cpuinfo; free -m; df -h /var/tmp; gcc --version | head -1
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1; ls -d /var/tmp/task-1121-tql3 /var/log/task-1121-tql3 2>&1
ls -d /lib/modules/$(uname -r)/build; modinfo -F filename brd
command -v fio perf python3 git make gcc objdump; fio --version; python3 --version
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|perf'
```
Stop and report, changing nothing, if: `nproc` is not 8 or lscpu does not say 1 NUMA node; less than 6500 MB available; `uname -r` does not start with `6.12.`; a zfs, spl or brd module is loaded or `/dev/ram0` exists; one of the two task directories exists; the kernel build directory, the brd module or `objdump` is missing; /var/tmp has less than 20 GiB free; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; a build, test, fio, bpftrace or perf runs. Then, only if `fio` or `perf` is missing:
```
L=/var/log/task-1121-tql3; sudo -n mkdir -p $L && sudo -n chown "$(id -un)" $L
sudo -n apt-get install -y fio linux-perf > $L/apt-install.log 2>&1; echo rc=$?; grep -A3 'NEW packages' $L/apt-install.log
```
And in every case (put T0 for `<T0>`):
```
L=/var/log/task-1121-tql3; T=/var/tmp/task-1121-tql3
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
perf --version; perf record -h 2>&1 | grep -c -- '--kernel-callchains'
printf 'T0=%s\n' "<T0>" > $L/env
```

### 1. Kit, fetched from the fork branch
```
L=/var/log/task-1121-tql3; T=/var/tmp/task-1121-tql3
cd $L/bin && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs vm-kit-tql3
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}'; git rev-parse -q --verify 'FETCH_HEAD~1' || echo "no parent"
test "$(git rev-parse FETCH_HEAD)" = 87bdde50764caa1ef8133fd1405e22b19ff83c13 && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 0ed1f9d074a46b67063061497efaf0ceb535ed2c && echo kit tree OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    21698156eb4055e76468156de9fde2dd7239f4b5377d58990afa1817c50230d9 tql3-bench.sh \
    199599f00814980f972d6d18187964ff50d23aac4a0dfe1c19075850bf988978 tql2-lock.py \
    61c0396c643ce57058fd356277d081b0d9501913c6b7d317f39dd1bdfe7f721d tql3-report.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n tql3-bench.sh && echo syntax-ok; for f in tql2-lock.py tql3-report.py; do python3 -I -c "import ast; ast.parse(open('$f').read())" && echo "$f py-ok"; done; git status --porcelain | wc -l; ls -l tql3-bench.sh
```
Expected: `no parent`; the two `OK` lines; exactly three entries, `100644 tql2-lock.py`, `100755 tql3-bench.sh` and `100644 tql3-report.py`; three `OK` from `sha256sum -c`; `syntax-ok`; two `py-ok`; 0; `-rwxr-xr-x`.

### 2. Trees, release builds and call sites (about 17 min)
```
T=/var/tmp/task-1121-tql3; L=/var/log/task-1121-tql3
git clone -q --filter=blob:none --no-checkout --single-branch --branch spl-taskq-wake-after-unlock https://github.com/tigerblue77/zfs.git $T/repo
cd $T/repo && git log --oneline -2 HEAD
test "$(git rev-parse HEAD)" = f5511fd916318658dc40679fa4c0a624451e3e34 && echo twu commit OK
test "$(git rev-parse 'HEAD^{tree}')" = 8b5c148dd54c1d68246da65b4a1da634dd027c20 && echo twu tree OK
test "$(git rev-parse HEAD~1)" = a1daebfec3f5ca46ac261ef339efdf0dbeae997c && echo master commit OK
test "$(git rev-parse 'HEAD~1^{tree}')" = 0b23c141b417a8542546e1f7a32662bc82bc0f92 && echo master tree OK
git diff --stat HEAD~1 HEAD
git worktree add -q --detach $T/master a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git worktree add -q --detach $T/twu f5511fd916318658dc40679fa4c0a624451e3e34
printf '%s  %s\n' \
    b8b0089493527d63dc1769efae43b92a69561a533cd50e081dd23b794f79d32d $T/master/module/os/linux/spl/spl-taskq.c \
    f809ac2e5ed004971bc5694d1cc8dd741cc7fe9c54d6c045ae51cd4b1a1058a5 $T/twu/module/os/linux/spl/spl-taskq.c \
    5e829da48d65ee3a8c4a94d3859400e8393f823e7479366d7249c443667ff7c0 $T/master/include/os/linux/spl/sys/taskq.h \
    06841c62b15d4a7ca811a4b3c4125263015acf632f4b400e3d93d7f2cfbf63f6 $T/twu/include/os/linux/spl/sys/taskq.h > $T/code.sha256
sha256sum -c $T/code.sha256 && rm $T/code.sha256
```
Expected: four `OK` lines; the diff stat names only `include/os/linux/spl/sys/taskq.h` (1 insertion) and `module/os/linux/spl/spl-taskq.c` (45 insertions, 9 deletions); four `OK` from `sha256sum -c`. (master's `spl-taskq.c` is byte for byte TQL's and TQL2's.) Then build the two at once, release (no `--enable-debug`), `make -j4` each:
```
T=/var/tmp/task-1121-tql3; L=/var/log/task-1121-tql3; J=4
for a in master twu; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j$J > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
for a in master twu; do for m in spl zfs; do echo "$a $m $(modinfo -F vermagic $T/$a/module/$m.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/$m.ko)"; done; done
```
Expected: rc 0 twice; modules built for `uname -r`; the two spl srcversions different (equal: stop and report). The two zfs srcversions are expected equal, as in TQL: srcversion hashes only the sources in each object's own directory, so the changed `taskq.h` does not show in it, and zfs.ko reads no field of the taskq structure. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop. Then the call sites (reads only):
```
L=/var/log/task-1121-tql3; cd $L/bin
./tql3-bench.sh sites; tail -2 $L/driver.log
for a in master twu; do echo "== $a"; cat $L/sites-$a.txt; done
```
Quote both files whole. Expected (report what you see): in each, `taskq_thread` has exactly three `_raw_spin_lock_irqsave` calls, one with landmark `tsd_set`, one with `indirect`, one with `schedule` (master) or `remove_wait_queue` (twu); in twu the `remove_wait_queue` call comes before that third lock call, in master after it. In `taskq_dispatch_ent` (and `taskq_dispatch`), master's `__wake_up` comes before the `_raw_spin_unlock_irqrestore` that follows it, twu's after; if no unlock call is listed (inlined), say so: the order cannot be read then.

### 3. Setup (about 2 min, functional)
```
L=/var/log/task-1121-tql3; cd $L/bin
echo "task-1121-tql3: start" | sudo -n tee /dev/kmsg > /dev/null
t=$(date +%s); sudo -n ./tql3-bench.sh setup; echo "rc=$? $(( $(date +%s) - t )) s"; tail -2 $L/driver.log
cat $L/setup-zpool-list.txt $L/setup-zfs-list.txt; free -m
```
`setup` loads master's modules, loads brd, creates the pool and its four datasets and writes the files. If it stops, run `teardown` before trying once more. Record the setup duration, the pool's size and allocated space, the datasets' compress ratios (tqlp/w near 2), and MemAvailable after setup.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (step 0's cmdline, nproc, lscpu and gcc lines, cpuidle lines; the build table with rc, times and srcversions; both sites files; setup duration, pool size, MemAvailable) and, as the very last line, exactly:

`TASK-1121-TQL3 READY-TO-MEASURE (shape A: about 65 min alone on the host, at most 90). Waiting for GO.`

Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1121-TQL3`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1121-TQL3`, go to step 7. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 5 min)
```
L=/var/log/task-1121-tql3; cd $L/bin
date -u; sudo -n ./tql3-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n ./tql3-bench.sh pass W1 "r8 w8" "master twu"; echo rc=$?
grep -H -E '^(arm|spl srcversion|zfs srcversion|nproc|numa nodes|zfs_arc_max|zfs_dirty_data_max|spa_asize_inflation|brd|cpuidle|JOBS_MAX)' $L/raw/W1/*/layout.txt
```
If steal is above 1 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W1, check: every row `rc=0` in driver.log; each arm's `layout.txt` names its own tree and its spl and zfs srcversions of step 2, nproc 8, 1 NUMA node, `zfs_arc_max=1073741824`, `zfs_dirty_data_max=268435456`, `spa_asize_inflation=24`, `brd 8388608 sectors` and `JOBS_MAX=8`. If one of these checks fails, run `teardown` and report.

### 6. The passes, then processing and the report (about 60 min)
```
L=/var/log/task-1121-tql3; cd $L/bin
sudo -n ./tql3-bench.sh lock L1 "r8 s8 w8" "master twu"; echo rc=$?
```
After L1, check before going on: every row `rc=0`; each arm's `layout.txt` as after W1; `ls -la /var/tmp/task-1121-tql3/perf` shows six non-empty captures; no `.rec.log` of L1 names an error or lost events (`grep -il -E 'error|invalid|not supported|lost' $L/raw/L1/*/*.rec.log`; quote any it lists). Then:
```
L=/var/log/task-1121-tql3; cd $L/bin
sudo -n ./tql3-bench.sh lock L2 "r8 s8 w8" "twu master"; echo rc=$?
sudo -n ./tql3-bench.sh pass P1 "r8 s8 w8" "master twu"; echo rc=$?
```
After P1, check: every row `rc=0`; each arm's `layout.txt` as after W1; `ls $L/raw/P1/*/*.cpu | wc -l` is 6. Then:
```
L=/var/log/task-1121-tql3; cd $L/bin
for p in "P2|twu master" "P3|twu master" "P4|master twu" "P5|master twu" "P6|twu master"; do sudo -n ./tql3-bench.sh pass "${p%%|*}" "r8 s8 w8" "${p#*|}" || { echo "rc=$? at ${p%%|*}"; break; }; done
```
Apply the **time rule** now (section Rows and passes) and write its figures (d, now, projected end, the limit, what was dropped if anything) into $L/time-rule.txt. Then, unless the rule drops them:
```
L=/var/log/task-1121-tql3; cd $L/bin
sudo -n ./tql3-bench.sh pass P7 "r8 s8 w8" "twu master" && sudo -n ./tql3-bench.sh pass P8 "r8 s8 w8" "master twu"; echo rc=$?
```
Then, in every case:
```
L=/var/log/task-1121-tql3; cd $L/bin
t=$(date +%s); sudo -n ./tql3-bench.sh process; echo "rc=$? $(( $(date +%s) - t )) s"; ls /var/tmp/task-1121-tql3/perf | wc -l
python3 -I $L/bin/tql3-report.py $L > $L/summary.md; echo rc=$?
python3 -I $L/bin/tql2-lock.py summary $L/raw > $L/lock-summary.md; echo rc=$?
grep -h -i lost $L/raw/L*/*/*.script.err | sort | uniq -c; head -1 $L/raw/L*/*/*.agg.md
```
Expected: `process` rc 0 and no capture left; both summaries rc 0. If `process` stops, quote the last 20 lines of its output and of driver.log, run `teardown` and report.

### 7. Teardown, end
```
L=/var/log/task-1121-tql3; cd $L/bin
sudo -n ./tql3-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1
sudo -n dmesg | sed -n '/task-1121-tql3: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
sudo -n rm -rf /var/tmp/task-1121-tql3 && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, no `/dev/ram0`, no kernel line (report any, with the step it happened in). The arm switches unloaded and reloaded spl and zfs about 20 times, each destroying every taskq of the pool and of the SPL: say how many arm switches ran (`grep -c 'arm ' $L/driver.log`) and that none hung. Purge the packages $L/apt-install.log lists as NEW, if any. Keep /var/log/task-1121-tql3 (raw data, `.agg.md` and `.agg.tsv` included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build or equal spl srcversions; a failed check after W1, L1 or P1; a driver error (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING:` or hung task in the kernel log (capture, report); more than 2 h 15 on the VM without the wait for GO (finish the current pass, run `process` and the report, then step 7, and say what did not run). A result against expectations, a sites file that is ambiguous, or a pass dropped under the time rule, is a finding, not a stop.

## Report

/var/log/task-1121-tql3/report.md, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, per the rules written before the run:
   a. **Decision**: the "Decision rules" table and the closing line of summary.md.
   b. **Verdicts**: the "Verdicts (twu/master)" table whole, and per row one sentence on throughput, CPU per GiB and p99.
   c. **Mechanism**: the "Lock passes" table whole (with the two site-map lines above it), M0, M1 and M2 with their figures, and the "Taskq wait-queue takers" rows of r8. For r8 master and twu, pass L1: the top 10 takers from the `.agg.md` and the call chains of the top 3.
   d. **Taskqs**: the taskq table of summary.md for r8 (sleeps and wakeups per task, nowork %), master against twu.
   e. **Noise**: the quiet check, the noise table and the arm-position table of summary.md, lost events, the begin/end counts of the `.agg.md` head lines.
2. summary.md whole, then lock-summary.md's "tq_lock and taskq wake-up totals" table.
3. Setup: kernel, /proc/cmdline, CPUs and lscpu lines, gcc, RAM, cpuidle driver and governor, kit checks, code checks and file hashes, build times and rc, srcversions; both sites files; setup duration, pool size and MemAvailable; per pass its start and end time and arm order (driver.log); the time of GO, the time rule's figures ($L/time-rule.txt) and what it dropped; `process` duration; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, the number of arm switches and whether any hung, deviations, what could not be run, packages installed and purged, any reboot, end state (modules, `/dev/ram0`, `fstrim -av`, `df -h /var/tmp`).
