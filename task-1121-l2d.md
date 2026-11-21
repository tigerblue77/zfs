TASK-1121-L2D (from the coordinator session "main"): functional, about 1 h 45 builds included. Our fix for the L2ARC feed-thread deadlock that hung `trim_l2arc` in CI (register M-51, fork branch `l2arc-feed-stop-before-unload`, no PR yet): with `l2arc_trim_ahead` set, `zpool destroy`, `zpool export` or `zpool remove` of a cache device takes the config lock as writer while the per-device L2ARC feed thread trims, then waits for that thread, which waits for the lock. (A) With the fix, debug build: the new ZTS test `trim_l2arc_export` three times, then the groups `-T trim,l2arc` once. (B) Fail-before: master plus only the test commit, debug build: the test once; its first command is expected to hang for good in the kernel, which needs one reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-l2d`, `L=/var/log/task-1121-l2d`.

## Why

CI job 136636990524 (ubuntu24) stopped in `functional/trim/trim_l2arc` right before its `log_must zpool destroy $TESTPOOL`; the console showed two hung tasks, `l2arc_testpool_` (the feed thread of the cache device, named `l2arc_<pool>_<vdev id>` and cut to 15 characters) and `zpool`, and every later test hung. 1 run in 458. No stacks were captured: the cycle is from the code (master `a1daebfec3f`):

- `spa_unload()` takes `SCL_ALL` as writer (spa.c:2407) and, holding it, drops the cache devices: `spa_l2cache_drop()` (spa.c:2432) calls `l2arc_remove_vdev()`, which takes `l2ad_feed_thr_lock` and waits for the feed thread to exit (arc.c:10697-10702). `zpool remove` of a cache device does the same through `spa_load_l2cache()` (spa.c:2747), also holding `spa_namespace_lock`.
- The feed thread holds `l2ad_feed_thr_lock` for its whole pass. With `l2arc_trim_ahead > 0`, once its write hand has wrapped, `l2arc_evict()` drops `SCL_L2ARC` (arc.c:9368), calls `vdev_trim_simple()`, which takes `SCL_STATE_ALL` as reader for each TRIM (vdev_trim.c:523), then takes `SCL_L2ARC` again (arc.c:9372). Either blocks behind the writer.

- fix (commit 1, `f09245139b3`): `l2arc_spa_feed_stop()` stops the feed threads of a pool before the config lock is taken, in `spa_export_common()` and `spa_unload()` next to `l2arc_spa_rebuild_stop()`; `l2arc_vdev_feed_stop()` stops the thread of a cache device in `spa_vdev_remove()`, where the config lock is already dropped to stop its TRIM. `l2arc_remove_vdev()` keeps its wait, a no-op then.
- test (commit 2, `aba3b50c9be`): `trim_l2arc_export` (group trim) creates a pool on `/var/tmp/trim-vdev1` (1 GiB) with the cache device `/var/tmp/trim-vdev2` (256 MiB), sets `l2arc_trim_ahead=1`, `l2arc_write_max=64M`, `l2arc_dwpd_limit=0`, writes 768 MiB with fio and reads it randomly until the feed thread trims (the pool kstat `simple_trim_extents_written` grows). It then sets `zfs_trim_extent_bytes_max=512` and `zfs_trim_queue_limit=1`, so that a feed pass trims up to 64 MiB in 512-byte TRIMs issued one at a time (seconds), waits until at least 1000 TRIMs a second are issued, and runs `zpool remove testpool /var/tmp/trim-vdev2` in the background with a 300 s limit; then the defaults back, `zpool add` of the device, the same for `zpool export`, `zpool import`, and the same for `zpool destroy`. A command still running after 300 s: the test prints its kernel stack and those of the `l2arc_*` threads, FAILs, and leaves the pool alone. It adds `TRIM_EXTENT_BYTES_MAX` and `TRIM_QUEUE_LIMIT` to `tunables.cfg`.

Expected with the fix: PASS each time, in a few minutes; each of the three commands returns after waiting at most for the end of one feed pass (some seconds; the test logs `... returned after about N seconds`). Without the fix (part B): the first command whose config lock lands inside a slow TRIM hangs, most likely the first, `zpool remove`: its kernel stack in `l2arc_remove_vdev` (mutex) from `spa_load_l2cache` / `spa_vdev_remove`, the feed thread's in `spa_config_enter` from `vdev_trim_range` / `vdev_trim_simple` / `l2arc_evict` (or from `l2arc_evict` directly), then the hung-task warnings for both after 120 s. The window is wide but not certain: a command that lands between two TRIM passes returns, and the test goes on to the next command.

What is tested: fork https://github.com/tigerblue77/zfs, branch `l2arc-feed-stop-before-unload`, head `aba3b50c9bebf4799ba8685f5c79d30c0e7b4dd7`, tree `4f8a560c92a6ba304d4dee6583b6eeafc4b23aec`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `7165b51681237f9cf1fb0dd16bc6f08d9f33c19d`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs and the loop devices zfs-tests.sh makes of them (the trim group needs block devices that support discard: do not pass `-f`); touch no other block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/trim-vdev* 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 20 minutes; a group after 90 minutes. In part A, capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|l2arc|txg|zfs|zpool|ksh'`, the kernel stacks of every D task and of every `l2arc_*` thread (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: exactly one is pre-approved, in step 5, after the fail-before run. In part A, a hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main. Before the step 5 reboot, arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, and write in $L/progress.md where you are. A global `sync` may block once a pool is wedged: use `sync -f $L/progress.md`, then the sysrq reboot through a transient unit, as in DBM4.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs` (quote; if hung_task_panic is 1, set it to 0 for this task and say so: part B must hang, not panic), `id zts`, `command -v fio` (the test is skipped without it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/trim-vdev* 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-l2d; L=/var/log/task-1121-l2d
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs l2arc-feed-stop-before-unload && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   aba3b50c9bebf4799ba8685f5c79d30c0e7b4dd7 4f8a560c92a6ba304d4dee6583b6eeafc4b23aec a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   aba3b50c9be ZTS: Remove, export and destroy while the L2ARC trims
#   f09245139b3 L2ARC: Stop feed threads before taking the config lock
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/arc.c module/zfs/spa.c module/zfs/vdev_removal.c include/sys/arc.h tests/zfs-tests/tests/functional/trim/trim_l2arc_export.ksh tests/zfs-tests/include/tunables.cfg
#   7499b68a5bdb47e11547a287ad45a3d307354bd40178e6acb19b2895b7c0b4ba  module/zfs/arc.c
#   cfca51340eaf25aa7686cb5c6201ad3e2f81cbf07a5a40f34cc09be925fb5067  module/zfs/spa.c
#   6f7ca257b9ef9799a53d2eff9c1d2ba50dcc33a9502acdce100111347f716341  module/zfs/vdev_removal.c
#   00c1f6cbbe768195d1787c1f490a24567f93c63f5278c3b7e9ab664cad5344e9  include/sys/arc.h
#   451d3a73e174c36a2b15f223e8316d230333675079582309655e95788a3434ed  .../trim/trim_l2arc_export.ksh
#   a44f40e4223c28a8b6354b5588aa43704bb44d632f2972342422a8054dc2c89f  tests/zfs-tests/include/tunables.cfg
grep -c 'l2arc_spa_feed_stop\|l2arc_vdev_feed_stop' module/zfs/arc.c module/zfs/spa.c module/zfs/vdev_removal.c   # 4, 2 and 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-l2d
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick aba3b50c9bebf4799ba8685f5c79d30c0e7b4dd7
git rev-parse HEAD^{tree}          # 7165b51681237f9cf1fb0dd16bc6f08d9f33c19d
sha256sum module/zfs/arc.c module/zfs/spa.c module/zfs/vdev_removal.c include/sys/arc.h
#   091f206e6a12f598184e3a070f3a6edebd6f93e1ff5bde9ab601002b43ad041a  module/zfs/arc.c           (master's)
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c           (master's)
#   8ec28441beebe4c135878d08df51efb583ac3f14914965c06c9b6d48de5f0587  module/zfs/vdev_removal.c  (master's)
#   db5591eca2c1e3e8d13748cd953bbdff86c390f69815f485dcfb341d05dd34ee  include/sys/arc.h          (master's)
grep -c 'l2arc_spa_feed_stop\|l2arc_vdev_feed_stop' module/zfs/arc.c module/zfs/spa.c module/zfs/vdev_removal.c   # 0, 0 and 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-l2d; L=/var/log/task-1121-l2d
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Quote the defaults the test changes and must restore, `for p in l2arc_trim_ahead l2arc_write_max l2arc_dwpd_limit zfs_trim_extent_bytes_max zfs_trim_queue_limit; do echo "$p=$(cat /sys/module/zfs/parameters/$p)"; done` (P0). Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-l2d; L=/var/log/task-1121-l2d; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 1500 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t trim_l2arc_export > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in a few minutes (no measurement yet: say how long). For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, every line with `TRIMs during the read`, `TRIMs in a second` and `returned after about` (three of the last: `zpool remove`, `zpool export`, `zpool destroy`; the `TRIMs in a second` line just before each is the one that let it start: 1000 or more); the five parameters after it (equal to P0: the test restores them); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log (a FAIL that says `did not return in 300 seconds` is a hang: capture as in the VM rules, stop and report), then go on to the next run; two FAILs in a row: stop part A and report. A run that SKIPs: quote why (the trim group's setup skips when its disks do not support discard; `fio` missing skips the test) and stop.
2. `-T trim,l2arc` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T trim,l2arc > $L/zts-az.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/trim]` and `[tests/functional/l2arc]` in `common.run` PASS, `trim_l2arc` and `trim_l2arc_export` among them. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known (an `l2arc_dwpd_ratelimit_pos` KILLED at its timeout is a known CI flake, openzfs#19341).

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Start a capture flushed line by line that survives your commands: `L=/var/log/task-1121-l2d; setsid nohup sudo -n sh -c "dmesg --follow > $L/fb-follow.txt" > /dev/null 2>&1 < /dev/null &`. Then `touch $L/fb-start`, write $L/progress.md ("step 4 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-l2d; L=/var/log/task-1121-l2d
cd $D/fb-dbg && setsid nohup timeout 1500 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t trim_l2arc_export > $L/zts-fb.log 2>&1 < /dev/null &
```

Poll every 10 s, for at most 20 minutes: `pgrep -a -x zpool` (the three commands are `zpool remove testpool /var/tmp/trim-vdev2`, `zpool export testpool`, `zpool destroy testpool`) and, for each of them, `ps -o etimes=,stat=,wchan:32= -p <pid>`. When one has run for 60 s, capture into $L/fb-state.txt, in this order:

```sh
L=/var/log/task-1121-l2d
{ date; ps -eLo pid,tid,stat,etimes,comm,wchan:32,args | grep -E ' D|l2arc|zpool|zfs-tests|ksh';
  for p in $(pgrep -x zpool) $(pgrep '^l2arc_'); do echo "== $p $(cat /proc/$p/comm) $(ps -o args= -p $p)"; sudo -n cat /proc/$p/stack; done;
  sudo -n grep -E '^(simple_)?trim_extents_written' /proc/spl/kstat/zfs/testpool/iostats;
  for t in zfs_trim_extent_bytes_max zfs_trim_queue_limit l2arc_trim_ahead; do echo "$t=$(cat /sys/module/zfs/parameters/$t)"; done;
  sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg; } > $L/fb-state.txt 2>&1
echo w | sudo -n tee /proc/sysrq-trigger > /dev/null
sleep 5; sudo -n dmesg | tail -n 400 > $L/fb-sysrq-w.txt
```

Take the stacks twice, 30 s apart (fb-state.txt, then fb-state2.txt with the same command): in a deadlock they do not move. Quote: which command hung and after which `TRIMs in a second` value (from the test's own log if it is already written, else say it is not); the stack of the `zpool` process and of every `l2arc_*` thread (all frames); whether the two captures match; the `simple_trim_extents_written` value in both (it must not move); from fb-follow.txt, the `blocked for more than` lines and the task names they give (expected after 120 s: `l2arc_testpool_` and `zpool`). Then wait until 6 minutes after the command started: the test itself prints the stacks and FAILs with `... did not return in 300 seconds`; quote that line from `$L/zts-fb.log` or the test's own log if it appears. The runner then runs the trim group's cleanup, which blocks on the pool: it is not expected to return, and you do not wait for it. Save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

No command hung (the test PASSes on master within 20 minutes): the window was missed; quote its `TRIMs in a second` and `returned after about` lines, then run it once more, the same way (at most two more runs in all). Three PASSes: report it plainly, the fail-before was not reproduced, and skip the reboot in step 5 (unload instead, quoting rc).

## STEP 5: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `losetup -a` (quote); `find /var/tmp -mindepth 1 -maxdepth 1 -newer $L/fb-start -printf '%M %u %s %TT %p\n'` (quote). The hung run cannot clean up, so these are expected among them: `trim-vdev1` and `trim-vdev2` (the test's vdevs), `tunable-ANYRAID_MIN_TILE_SIZE` (saved by the trim group's setup), `file-vdev0` to `file-vdev2`, and possibly `testdir` and `constrained_path.*`. Remove exactly those of them that the listing shows (`sudo -n rm -rf` with their names); anything else it lists is reported and left, `/var/tmp/test_results` stays. `/etc/zfs/zpool.cache` is expected above 0 (the run's `testpool`): quote its `ls -l` and the `strings` check of step 0; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.fb`; otherwise report it and leave it. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the three `returned after about N seconds` values, the `TRIMs in a second` value before each command, parameters equal to P0 after the run), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: which command hung, after how many seconds it was captured, the `zpool` and `l2arc_*` stacks (all frames), whether the second capture matched, `simple_trim_extents_written` in both, the hung-task lines, the test's own `did not return` line if it came, whether the runner returned, the reboot (time, how you resumed), what was removed and where the cache file went. One-line verdict: "L2D PASS 3/3 debug (remove/export/destroy returned after A/B/C s at most), -T trim,l2arc PASS; fail-before: <command> hung with zpool in l2arc_remove_vdev and the feed thread in spa_config_enter under vdev_trim_simple: confirmed" or what differs.
