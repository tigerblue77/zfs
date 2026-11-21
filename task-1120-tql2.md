TASK-1120-TQL2 (from the coordinator session "main"): performance diagnosis, lock attribution. Shape A: VM 105, 106 or 107 at 8 vCPU (Debian 13, kernel 6.12, 7.8 GB), about 1 h 25 on the VM plus the wait for main's GO, at most 2 h 10 without that wait. Shape B, only when main sends it there: VM 107 reshaped to 32 vCPUs in two NUMA nodes of 16 and 32 GiB (the shape of 8-9/11), about 1 h 50, at most 2 h 45. Its measured part (steps 5 and 6: shape A about 40 min, never more than 60; shape B about 60 min, never more than 90) runs ALONE on the host and on a quiet VM: before it you send main a note and wait for main's GO; you never check the other VMs yourself. Steps 0 to 3 are functional and may run while other VMs work. Two release builds, the arm switched by reloading the spl and zfs modules: upstream master `cde5d598eb4` and "tql", the same plus one commit to the SPL taskq (the same two trees as TASK-1118-TQL). No throughput verdict: every row records its lock contention events with their call chains. Nothing is published on GitHub, no credential files, fstrim at the end. Report: /var/log/task-1120-tql2/report.md and the same text as your final message.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. Every note for main is the end of a turn in your own chat.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`T`, `L`); the blocks below do.

Reboot when needed, without waiting for main (a module that will not unload, a hung task once its diagnostics are saved): save the diagnostics first, say in the report that you rebooted and why. A lock pass is never resumed after a reboot: go to step 7.

## Background

TASK-1118-TQL (VM 106, 20/11) found no gain for tql at 8 vCPU. Its lock pass could not use BPF (Debian's perf has no BPF skeleton) and fell back to `perf lock record` and `perf lock contention`. Without BPF, perf prints as "caller" the first non-lock function of the call chain *after skipping its first 4 entries* (`--stack-skip`, default 4, sized for BPF stacks). On a kernel whose mutex chain starts with the one lock frame `__mutex_lock.constprop.0` (the 6.18 kernel where this was checked, perf 6.8), a pipe mutex taken in `anon_pipe_write` is printed as `do_syscall_64+0x71`, three frames up, and a futex spinlock taken in `futex_wait_setup` as `futex_wait+0x67`, two frames up; with `--stack-skip 0` both are printed at the function that took the lock. TQL's own data fit this: its top spinlock caller, `ret_from_fork+0x34` (2.36 s of wait per 10 s on r8), is the frame two above `taskq_thread`. So TQL's "tq_lock (callers taskq_*)" column did not measure tq_lock, and its top mutex on w8, "`dmu_tx_assign+0x2bd`" (0.433 s per 10 s), was not taken in `dmu_tx_assign()`.

Expected from the code (report what you see, not this): `dmu_tx_assign+0x2bd` is the return address of `dmu_tx_assign()`'s call to `dmu_tx_wait()`, and the mutex is `tx_sync_lock`, re-taken by `cv_wait_common()` when a writer wakes in `txg_wait_synced_flags()` (`__cv_wait_io()` tail-calls `cv_wait_common()`, so it has no frame). Writers wait there after an ERESTART counted as `dmu_tx_quota` (12022 per w8 row in TQL): on the 4 GiB pool, the root dataset's space check counts writes in flight at `spa_asize_inflation` (24) times their size, so at about 90 MiB in flight it refuses them; each refused writer waits for the syncing txg, and the end of each sync wakes all of them at once. The wi rows set `spa_asize_inflation=4`, which should remove that throttle and with it the mutex. tql (unchanged from TQL): dispatchers wake only when `wq_has_sleeper()` finds a worker on the work queue; a worker wakes `taskq_wait()` waiters only if there are any, after dropping `tq_lock`. The dispatchers' wake-up of a sleeping worker stays under `tq_lock`.

## What you fetch (fork https://github.com/tigerblue77/zfs)

| What | Branch | Commit | Tree |
|---|---|---|---|
| tql | `spl-taskq-wake-outside-lock` | `b378a6d607869c0ac29f3528113f769ae8ca41fa` (parent master) | `2c28b5391e5350ea26b8496d7a30ca810c82bc43` |
| master (tql~1, upstream) | - | `cde5d598eb44b5d6efe8167666d988a496346657` | `d70a147b0775bf4bd216072a8b05738b603d6849` |
| kit | `vm-kit-tql2` | `0932f58bd7b9b6b321f447b310efc85084b6e27a` (no parent) | `6813794630d899b99f90f00936f73b10831a1093` |

The kit holds two files: `tql2-bench.sh` (the driver: `setup`, `quiet SECONDS`, `lock NAME ROWS ARMS`, `process`, `teardown`; read its header) and `tql2-lock.py` (`agg`, called by `process`: per capture the lock classes, the top takers with their call chains, and TQL's callers mapped to the takers behind them; `summary RAW`: the means over the passes).

## Shapes, rows and passes

| Shape | VM | brd | Jobs (JOBS_MAX) | Rows |
|---|---|---|---|---|
| A | 8 vCPU, one NUMA node, at least 6500 MB available | 4 GiB | 8 | r8 s8 w8 wi8 |
| B | 32 vCPU, two NUMA nodes, at least 24000 MB available | 8 GiB | 32 | r32 s32 w32 wi32 wi8 wi16 |

One pool `tqlp` on one brd RAM disk (`/dev/ram0`), ashift 12; the ARC is capped at 1 GiB and `zfs_dirty_data_max` set to 256 MiB at each module load. Each row starts with a `zpool sync` and 2 s of sleep; fio runs 5 s of ramp and 18 s more; 2 s after the ramp the driver records 10 s of `lock:contention_begin` and `lock:contention_end` on every CPU with 16-frame kernel call chains into `$T/perf`.

| Row | Dataset | Workload |
|---|---|---|
| rJ | tqlp/ur (recordsize 4K, primarycache=metadata) | 4K random reads, J jobs, one 512M file: every read goes to brd |
| sJ | tqlp/sw (recordsize 4K, primarycache=metadata) | 4K random O_SYNC writes, J jobs, one 32M file each |
| wJ | tqlp/w (recordsize 128K, lz4, primarycache=metadata) | 128K sequential buffered writes of half-compressible data, J jobs, one file each (128M in shape A, 32M in shape B) |
| wiJ | tqlp/w | wJ with `spa_asize_inflation=4` during the row (24 again after it) |

Per row and arm the driver keeps under `raw/<pass>/<arm>/` the fio JSON, `.snap0`/`.snap1` (dmu_tx counters, the pool's `dmu_tx_assign` histogram, ARC demand data, taskq counters, `spa_asize_inflation`), `.rec.log`; `process` adds `.agg.md`, `.agg.tsv`, `.lock4.txt` (perf's default, as TQL printed), `.lock0.txt` (`--stack-skip 0`) and `.script.err`; per arm `layout.txt`.

| Pass | Rows | Arm order | |
|---|---|---|---|
| W1 | shape A: r8 wi8; shape B: r32 wi32 | master tql | warm-up, not reported |
| L1 | the shape's rows | master tql | |
| L2 | the shape's rows | tql master | |
| L3 | the shape's rows | master tql | time rule before it |

Projected (a row about 30 s, an arm switch about 25 s): shape A: quiet check 1 min, W1 about 3 min, each L pass about 5 min, `process` about 15 min: **about 35 min from GO** to the summary. Shape B: W1 about 3 min, each L pass about 7 min, `process` about 25 min: **about 55 min**.

**Time rule** (keeps the measured part under its limit). Before L3, from driver.log: d = the mean duration of L1 and L2; projected end = now + d + (shape A 15 min, shape B 25 min). If it passes GO + 60 min (shape A) or GO + 90 min (shape B), do not run L3 (L1 and L2 keep each arm first once) and go on with `process`. Say what was dropped and why.

## Written before the run: what the results decide

`tql2-lock.py summary` and the `.agg.md` files give the figures; quote them, recompute none by eye. "% of CPU time" = wait / (capture seconds × CPUs); a spinlock's wait is CPU spent spinning (tq_lock with interrupts off), a mutex's includes sleeping.

- **F (frame model):** in the `.agg.md` of w8 (shape A) or w32 (shape B), master, pass L1, the call chain of the top mutex taker, quoted. Expected `__mutex_lock.constprop.0` then the taker; say how many lock frames precede the taker for mutexes and for spinlocks. The objdump lines of step 2 say what `dmu_tx_assign+0x2bd` calls and whether `__cv_wait_io` ends in `jmp cv_wait_common`.
- **H1 (the hot mutex):** on the w row (w8 or w32), master: the TQL caller with the largest mutex wait in "TQL callers resolved" and the takers behind it (from the `.agg.md` "TQL callers" table). Confirmed "tx_sync_lock re-taken after a txg wait" if at least 80 % of that caller's wait is the class `tx_sync_lock: re-acquired after a txg wait (cv_wait_common)` in every pass; otherwise name the classes behind it.
- **H2 (a by-product of the pool size):** on the wi row of the same J, master: `dmu_tx_quota` at most 1 % of the w row's, and the two `tx_sync_lock` classes' wait at most 20 % of the w row's. Both: "the hot mutex comes from the space-check throttle of the small pool". Otherwise say which part fails.
- **T1 (room for tql):** S = master's tq_lock classes plus the taskq wait-queue class, in % of CPU time, the largest over the w and wi rows (and separately over the r rows). S below 2: "the tq_lock lead cannot show a throughput effect on this shape"; 2 to 5: "marginal"; 5 or more: "a throughput A/B on this shape can conclude".
- **T2 (tql does what it says):** on the w and wi rows, tql's taskq wait-queue class wait at most 0.5 × master's, and tql's `tq_lock: dispatch` wait not above 1.2 × master's.
- **What it takes to conclude:** shape A is expected to give S below 2, i.e. 8 vCPU cannot conclude. The tq_lock lead can then only be concluded on shape B, and only if T1 there is "can conclude" (a throughput task, TQL3, would follow); if shape B gives S below 2, no host available here can show it, and the lead is parked.
- **Noise:** the `.script.err` lines with "lost", and the "end without begin" and "begin without end" counts in each `.agg.md` head line; steal from the quiet check.

## Rules for this task

- Trees: /var/tmp/task-1120-tql2/repo (one blob-less clone) with the worktrees /var/tmp/task-1120-tql2/master and /var/tmp/task-1120-tql2/tql (release builds only). Captures: /var/tmp/task-1120-tql2/perf, deleted by `process` one by one. Logs and results: /var/log/task-1120-tql2/. Work as root through `sudo -n` as usual. The driver puts the arm's tree first in PATH, checks that `zpool` comes from it, and at each arm switch checks that the loaded spl and zfs srcversions are that tree's.
- The pool sits on `/dev/ram0` from the brd module, which the driver loads and unloads. Touch no other pool, disk or device; never `zpool import -a`.
- This is a performance measurement: nothing else on this VM from step 5 to the end of step 6, `process` and `summary` included (no build, ztest, ZTS, second fio, bpftrace or perf other than the driver's).
- Packages: `fio` and `linux-perf` may be installed if missing; keep the apt output in $L/apt-install.log and at the end purge exactly the packages it lists as NEW. Nothing else is installed.
- If a fetch or the clone fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- A long command (a build, a pass, `process`): if your shell tool cannot wait for it in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`), reading `tail -3 $L/driver.log` every few minutes. Several passes may run as one background chain that stops at the first non-zero exit status, as long as the checks after L1 and the time rule before L3 are applied where they stand.
- If the driver stops with an error: run `teardown`, then report; do not retry a lock pass on your own. If the kernel log shows `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or a hung task: stop, capture `sudo -n dmesg | tail -100` into $L/dmesg-stop.txt, report (reboot first only if the VM is unusable, as said above).

## Steps

### 0. State (T0 = when you start this step)
```
echo "T0=$(date +%s)"; date -u; uname -r; cat /proc/cmdline; nproc; lscpu | grep -E '^(Socket|Core|Thread|NUMA node)'; grep -m1 'model name' /proc/cpuinfo; free -m; df -h /var/tmp; gcc --version | head -1
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1; ls -d /var/tmp/task-1120-tql2 /var/log/task-1120-tql2 2>&1
ls -d /lib/modules/$(uname -r)/build; modinfo -F filename brd
command -v fio perf python3 git make gcc objdump nm; fio --version; python3 --version
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|perf'
```
The shape: **A** if `nproc` is 8, lscpu says 1 NUMA node and at least 6500 MB are available; **B** if `nproc` is 32, lscpu says 2 NUMA nodes and at least 24000 MB are available. Stop and report, changing nothing, if: neither shape; `uname -r` does not start with `6.12.`; a zfs, spl or brd module is loaded or `/dev/ram0` exists; one of the two task directories exists; the kernel build directory, the brd module, `objdump` or `nm` is missing; /var/tmp has less than 20 GiB free (shape A) or 40 GiB (shape B); /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; a build, test, fio, bpftrace or perf runs. Then, only if `fio` or `perf` is missing:
```
L=/var/log/task-1120-tql2; sudo -n mkdir -p $L && sudo -n chown "$(id -un)" $L
sudo -n apt-get install -y fio linux-perf > $L/apt-install.log 2>&1; echo rc=$?; grep -A3 'NEW packages' $L/apt-install.log
```
And in every case (put the shape letter for `<S>` and T0 for `<T0>`):
```
L=/var/log/task-1120-tql2; T=/var/tmp/task-1120-tql2
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
perf --version; perf record -h 2>&1 | grep -c -- '--kernel-callchains'
printf 'T0=%s\nSHAPE=%s\n' "<T0>" "<S>" > $L/env
```

### 1. Kit, fetched from the fork branch
```
L=/var/log/task-1120-tql2; T=/var/tmp/task-1120-tql2
cd $L/bin && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs vm-kit-tql2
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}'; git rev-parse -q --verify 'FETCH_HEAD~1' || echo "no parent"
test "$(git rev-parse FETCH_HEAD)" = 0932f58bd7b9b6b321f447b310efc85084b6e27a && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 6813794630d899b99f90f00936f73b10831a1093 && echo kit tree OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    82aad759e7bb78862416a2b27434054ad2b4f972e8accbdba551cb8b59375534 tql2-bench.sh \
    199599f00814980f972d6d18187964ff50d23aac4a0dfe1c19075850bf988978 tql2-lock.py > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n tql2-bench.sh && echo syntax-ok; python3 -I -c 'import ast; ast.parse(open("tql2-lock.py").read())' && echo py-ok; git status --porcelain | wc -l; ls -l tql2-bench.sh
```
Expected: `no parent`; the two `OK` lines; exactly two entries, `100755 tql2-bench.sh` and `100644 tql2-lock.py`; two `OK` from `sha256sum -c`; `syntax-ok`; `py-ok`; 0; `-rwxr-xr-x`.

### 2. Trees, release builds and TQL's callers (about 17 min)
```
T=/var/tmp/task-1120-tql2; L=/var/log/task-1120-tql2
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
```
Expected: four `OK` lines; the diff stat names only `module/os/linux/spl/spl-taskq.c` (35 insertions, 3 deletions); two `OK` from `sha256sum -c`. Then build the two at once, release (no `--enable-debug`), `make -j4` each in shape A and `make -j12` each in shape B (`J` below):
```
T=/var/tmp/task-1120-tql2; L=/var/log/task-1120-tql2; J=4
for a in master tql; do (cd $T/$a && t=$(date +%s) && ./autogen.sh > $L/autogen-$a.log 2>&1 && ./configure > $L/configure-$a.log 2>&1 && nice -n 10 make -j$J > $L/build-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s") & done; wait
for a in master tql; do for m in spl zfs; do echo "$a $m $(modinfo -F vermagic $T/$a/module/$m.ko | cut -d' ' -f1) $(modinfo -F srcversion $T/$a/module/$m.ko)"; done; done
```
Expected: rc 0 twice; modules built for `uname -r`; the two spl srcversions different (equal: stop and report). Say whether they and the zfs srcversion equal TQL's (master spl `BA40B6EA1E022CA48AB235F`, tql spl `2A8459650F16BDB07476261`, zfs `65D8C0C24A9B688639F0721`) and whether `uname -r` is TQL's `6.12.117+deb13-cloud-amd64`: if all are, the code below is the code TQL measured. On a failed build, quote `grep -nE 'error:|Error [0-9]' <its log> | head -20` and stop. Then what TQL's callers call, and the SPL wait wrappers (reads only):
```
T=/var/tmp/task-1120-tql2; L=/var/log/task-1120-tql2; K=$T/master/module/zfs.ko
for so in dmu_tx_assign+0x2bd zpl_iter_write+0x165 zio_nowait+0xed abd_alloc_chunks+0x185 arc_buf_alloc_impl.isra.0+0x1f2 zio_dva_allocate+0x143; do s=${so%+*}; o=${so##*+}; a=$(nm "$K" | awk -v s="$s" '$3 == s && ($2 == "t" || $2 == "T") {print $1; exit}'); if [ -z "$a" ]; then echo "== $so: no symbol"; continue; fi; e=$(printf '%x' $(( 0x$a + o ))); echo "== $so (ends at $e)"; objdump -dr --no-show-raw-insn --disassemble="$s" "$K" | awk -v e="$e" '$1 == e ":" {print prev; if (rel != "") print rel; found = 1; exit} /^ +[0-9a-f]+:/ {prev = $0; rel = ""; next} /R_X86_64/ {rel = $0} END {if (!found) print "   no instruction starts at " e}'; done 2>&1 | tee $L/tql-callers.txt
for f in __cv_wait_io __cv_wait; do echo "== $f"; objdump -d --no-show-raw-insn --disassemble=$f $T/master/module/spl.ko | grep -E 'call|jmp'; done 2>&1 | tee -a $L/tql-callers.txt
```
Quote $L/tql-callers.txt whole. Expected (report what you see): `dmu_tx_assign+0x2bd` ends a call whose relocation names `dmu_tx_wait`, and both wrappers end in `jmp` to `cv_wait_common`.

### 3. Setup (about 2 min, functional)
Shape A:
```
L=/var/log/task-1120-tql2; cd $L/bin
echo "task-1120-tql2: start" | sudo -n tee /dev/kmsg > /dev/null
t=$(date +%s); sudo -n ./tql2-bench.sh setup; echo "rc=$? $(( $(date +%s) - t )) s"; tail -2 $L/driver.log
cat $L/setup-zpool-list.txt $L/setup-zfs-list.txt; free -m
```
Shape B: the same with `sudo -n env BRD_KB=8388608 JOBS_MAX=32 ./tql2-bench.sh setup`. `setup` loads master's modules, loads brd, creates the pool and its four datasets and writes the files. If it stops, run `teardown` before trying once more. Record the setup duration, the pool's size and allocated space, the datasets' compress ratios (tqlp/w near 2), and MemAvailable after setup.

### 4. Note to main, then wait for GO

Steps 5 and 6 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load. You do not check the other VMs yourself; main does. End your turn with the interim results (the shape; step 0's cmdline, nproc, lscpu and gcc lines, cpuidle lines; the build table with rc, times and srcversions and whether they equal TQL's; $L/tql-callers.txt; setup duration, pool size, MemAvailable) and, as the very last line, exactly (with A or B):

`TASK-1120-TQL2 READY-TO-MEASURE (shape <S>: about 40 min alone on the host for A, 60 for B; at most 60 for A, 90 for B). Waiting for GO.`

Then do nothing until main writes. Go on to step 5 only on a message from main that contains `GO TASK-1120-TQL2`; note the time of GO (the time rule counts from it). On `HOLD`, keep waiting. On `ABORT TASK-1120-TQL2`, go to step 7. Nothing runs while you wait (pool imported and idle).

### 5. Quiet check and warm-up (about 4 min)
```
L=/var/log/task-1120-tql2; cd $L/bin
date -u; sudo -n ./tql2-bench.sh quiet 60; tail -1 $L/driver.log
sudo -n ./tql2-bench.sh lock W1 "r8 wi8" "master tql"; echo rc=$?
grep -H -E '^(arm|spl srcversion|zfs srcversion|nproc|numa nodes|zfs_arc_max|zfs_dirty_data_max|spa_asize_inflation|brd|JOBS_MAX)' $L/raw/W1/*/layout.txt
grep -h spa_asize_inflation $L/raw/W1/*/*.snap0; ls -la /var/tmp/task-1120-tql2/perf; grep -il -E 'error|invalid|not supported|lost' $L/raw/W1/*/*.rec.log
```
Shape B: `"r32 wi32"` instead of `"r8 wi8"`. If steal is above 1 % or load1 above 2 at the end of the quiet check, wait 2 minutes, run it once more, and go on either way, saying so. After W1, check: every row `rc=0` in driver.log; each arm's `layout.txt` names its own tree, its spl srcversion of step 2, the shape's nproc and NUMA nodes, `zfs_arc_max=1073741824`, `zfs_dirty_data_max=268435456`, `spa_asize_inflation=24`, the shape's brd sectors (A 8388608, B 16777216) and JOBS_MAX; the wi row's `.snap0` says `spa_asize_inflation 4` and the r row's 24; four non-empty captures in `$T/perf`. Quote any `.rec.log` that the last grep lists. If one of these checks fails, run `teardown` and report. The W1 captures stay in `$T/perf` and are processed with the others (W1 is not in the summary).

### 6. The lock passes, then processing (shape A about 35 min, shape B about 55 min)
Shape A rows `"r8 s8 w8 wi8"`; shape B rows `"r32 s32 w32 wi32 wi8 wi16"`:
```
L=/var/log/task-1120-tql2; cd $L/bin
sudo -n ./tql2-bench.sh lock L1 "r8 s8 w8 wi8" "master tql"; echo rc=$?
sudo -n ./tql2-bench.sh lock L2 "r8 s8 w8 wi8" "tql master"; echo rc=$?
```
After L1, check before going on: every row `rc=0`; each arm's `layout.txt` as after W1; no `.rec.log` of L1 names an error or lost events (quote any that does). Apply the **time rule** now (section Shapes, rows and passes) and write its figures (d, now, projected end, the limit, what was dropped if anything) into $L/time-rule.txt. Then, unless the rule drops it:
```
L=/var/log/task-1120-tql2; cd $L/bin
sudo -n ./tql2-bench.sh lock L3 "r8 s8 w8 wi8" "master tql"; echo rc=$?
```
Then, in every case:
```
L=/var/log/task-1120-tql2; cd $L/bin
t=$(date +%s); sudo -n ./tql2-bench.sh process; echo "rc=$? $(( $(date +%s) - t )) s"; ls /var/tmp/task-1120-tql2/perf | wc -l
python3 -I $L/bin/tql2-lock.py summary $L/raw > $L/summary.md; echo rc=$?
grep -h -i lost $L/raw/L*/*/*.script.err | sort | uniq -c; head -1 $L/raw/L*/*/*.agg.md
```
Expected: `process` rc 0 and no capture left. If `process` stops, quote the last 20 lines of its output and of driver.log, run `teardown` and report.

### 7. Teardown, end
```
L=/var/log/task-1120-tql2; cd $L/bin
sudo -n ./tql2-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|brd) '; ls /dev/ram0 2>&1
sudo -n dmesg | sed -n '/task-1120-tql2: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops' $L/kmsg.txt | tail -40
sudo -n rm -rf /var/tmp/task-1120-tql2 && sudo -n fstrim -av && df -h /var/tmp
```
Expected: no module loaded, no `/dev/ram0`, no kernel line (report any, with the step it happened in). Purge the packages $L/apt-install.log lists as NEW, if any. Keep /var/log/task-1120-tql2 (raw data, `.agg.md` and `.agg.tsv` included).

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a failed build or equal spl srcversions; a failed check after W1 or L1; a driver error (teardown, then report); any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace` or hung task in the kernel log (capture, report); more than the shape's limit on the VM without the wait for GO (2 h 10 for A, 2 h 45 for B: finish the current pass, run `process`, then step 7, and say what did not run). A result against expectations, or a pass dropped under the time rule, is a finding, not a stop.

## Report

/var/log/task-1120-tql2/report.md, and the same text as your final message:

1. **Answers first**, each with the figures it rests on, per the rules written before the run:
   a. **F**: the call chain quoted, the number of lock frames before the taker for mutexes and spinlocks, and $L/tql-callers.txt whole.
   b. **H1**: the "TQL callers resolved" rows of the w row for master and tql, and per pass the "TQL callers" line of `dmu_tx_assign+0x2bd` (or the top mutex caller) from the w row's `.agg.md`. Confirmed or not.
   c. **H2**: the dmu_tx counter table of summary.md for the w and wi rows, and the two `tx_sync_lock` class lines of each. Holds or not.
   d. **T1 and T2**: the "tq_lock and taskq wake-up totals" table of summary.md whole, S for the w/wi rows and for the r rows, and the verdicts. Compare the last column (TQL's metric) with the tq_lock columns.
   e. **What it takes to conclude**, in one sentence, from T1 on this shape.
   f. The class table of summary.md whole, and for r8 or r32 master the top 10 takers of pass L1 (from its `.agg.md`).
   g. Noise: the quiet check, lost events, the begin/end counts of the `.agg.md` head lines.
2. summary.md whole.
3. Setup: the shape, kernel, /proc/cmdline, CPUs and lscpu lines, gcc, RAM, cpuidle driver and governor, kit checks, code checks and file hashes, build times and rc, srcversions and whether they equal TQL's, setup duration, pool size and MemAvailable; per pass its start and end time and arm order (driver.log); the time of GO, the time rule's figures ($L/time-rule.txt) and what it dropped; `process` duration; the measured time from GO to the end of step 6, and the time on the VM without the wait.
4. dmesg findings, deviations, what could not be run, packages installed and purged, any reboot, end state (modules, `/dev/ram0`, `fstrim -av`, `df -h /var/tmp`).
