v2 of TASK-1122-IRD v2 (task-1122-ird-v2.md): the tests are ported onto wrouesnel's zinject framework (openzfs/zfs#19997 direction); the fixes are unchanged.

TASK-1123-IRD-WR (from the coordinator session "main"): functional, about 3 h 30 builds included, with one pre-approved reboot. A fix for a master deadlock found on 22/11 with the M-05 model: `zpool import -o version=5000` of a pool whose device removal is about to complete, the property being refused by the kernel after the load, leaves `zpool import` and every later pool command hung for good. The fix is tested with wrouesnel's zinject injection-point framework (`zinject -n`, his branch `zinject-points-uplift`, head `189a309d464`), where a point can be set for a pool before its import. (A) With the fix, debug build: the new ZTS test `removal_failed_import` three times, `zinject_args` once, then the groups `-T removal` and `-T zpool_import` once each. (B) Fail-before: his framework plus this branch's test commit, without the fix, debug build: the test must FAIL with the import never returning, and the system then stays deadlocked (spa_namespace_lock held for good): capture it, then the one pre-approved reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1123-ird-wr`, `L=/var/log/task-1123-ird-wr`.

## Why

When `zpool import` fails after the load, `spa_import()` (spa.c, master 7736-7742) unloads the pool with `spa_namespace_lock` held, and `spa_unload()` stops the async tasks: `spa_vdev_remove_suspend()` waits until the removal thread, which the load restarted, clears `svr_thread`. Past its last `svr_thread_exit` check, the thread clears it only in `vdev_remove_complete()`, after `spa_vdev_enter()` has taken the namespace lock. If the import fails while the thread is between those two points: the import waits for the thread with the lock, the thread waits for the lock. `zpool import -o version=5000` fails exactly there: `zpool` accepts any version not below the pool's (5000 for a pool with feature flags), and `spa_prop_validate()` refuses any version above `SPA_VERSION_BEFORE_FEATURES` (EINVAL) once the pool is loaded. The fix stops the removal thread without the namespace lock on that path, as export does, with `spa_load_thread` set meanwhile to keep lookups waiting.

- commit 5 (`d84958c5118`, `zinject -w` for a pool that is not imported) is dropped: wrouesnel's `zinject -n` handlers are matched by pool name, hold no reference on the pool and are removed by ID without the namespace lock.
- commit 6 (`ce428f99046`, the fix, the same patch as `049a635553a`): module/zfs/spa.c, `spa_import()` only.
- commit 7 (`2864fc11b3e`, head, the test): point `vdev_remove_complete:start` (vdev_removal.c, before `spa_vdev_enter()` in `vdev_remove_complete()`), and `functional/removal/removal_failed_import.ksh`: pool `testpool` on two 256 MiB file vdevs in `$TEST_BASE_DIR/removal_failed_import`, 64 MiB written, removal of the second vdev with `zfs_removal_suspend_progress=1`, export; `zinject -P import -s 15 testpool` first, then `zinject -q -n vdev_remove_complete:start -A pause testpool` (the pool is exported: name-based handler), tunable back to 0; `zpool import -d <dir> -o version=5000 testpool &`; it waits until the point's inject count is 1 (the removal held), then until `testpool` leaves the `import_progress` kstat (the load delay is over and the load has retaken the lock), sleeps 2 s, releases the point with `zinject -c <id>`, and requires the import to exit within 60 s with a non-zero status and no pool; then `zinject -c all`, `zpool import -d <dir> testpool`, `wait_for_removal`, vdev1 gone from the pool, export. The import delay is still added before the point, as in v2: master's `zio_handle_pool_delay()` stops its walk of the handlers at the first one of another kind (openzfs/zfs#19994), so there a delay added after the point would never fire; this base already carries wrouesnel's `eaea830c048` ("zinject: find pool delays behind other handlers"), which fixes that walk.

Expected with the fix: PASS in about 40 s; the import fails with EINVAL ("invalid argument") a moment after the release, the removal completes during that failure, and the second import shows it done. Without the fix (part B): the test FAILs after about 80 s with `zpool import did not return in 60s`; its cleanup then logs `zpool import is still running, with spa_namespace_lock held: leaving everything as it is` and touches nothing; the group's `cleanup.ksh` that the runner runs next will hang in `zpool` (spa_namespace_lock), and so would any `zpool`/`zinject` command. Measured with a libzpool model (22/11, user space): master 3/3 deadlock, this tree without the fix 3/3 deadlock, with the fix 3/3 import EINVAL, pool gone, no hang.

What is tested: fork https://github.com/tigerblue77/zfs, branch `import-fail-removal-deadlock-v2-wr` (pushed 22/11), head `2864fc11b3e2f0cc205551785f8f610a9e09725d`, tree `aca709ad4536e0de194adb181ee0f554cbd728c1`: wrouesnel's sixteen commits (`189a309d464`, on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, 20/11), then commits 6 and 7 above. The fail-before tree is branch `import-fail-removal-deadlock-v2-wr-fb`, head `d9b540cacdee3e5a124f0c83b50b409a3fe9b883`, tree `e0949a0780bd5c466b4e76c97b015dcce1b75ae9`: his framework and the test commit, whose `module/zfs/spa.c` is his head's (master's with his points, without the fix). (v2 of this task: the v1 heads `f9dae818410` and `298cc4377fc` added the point before the import delay; only the test file differs.)

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything by hand (the test imports its own pool); never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/removal_failed_import 2>&1` shows nothing, `timeout 30 sudo -n <tree>/zinject 2>&1` lists no handler, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 90 minutes (part B has its own rule, below). Capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|remov'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report. The ported test has no hold time: a thread held at a zinject point (`zio_inject_pause` in its stack) stays held until its handler is removed by ID (`zinject -c <id>`, which takes no namespace lock); say so in the report and leave that to main.
- Reboots: one is pre-approved, in step 4 only, after the fail-before capture: arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, write in $L/progress.md where you are, `sync -f $L/progress.md`, and the sysrq reboot through a transient unit, as in DBM4. In part A, a hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs` (quote; if hung_task_panic is 1, set it to 0 for this task and say so: part B leaves hung tasks on purpose), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/removal_failed_import 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1123-ird-wr; L=/var/log/task-1123-ird-wr
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs import-fail-removal-deadlock-v2-wr && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git rev-parse HEAD~18) $(git status --porcelain | wc -l)"
#   2864fc11b3e2f0cc205551785f8f610a9e09725d aca709ad4536e0de194adb181ee0f554cbd728c1 189a309d464ca51c74d627fdbdd84b5992eade2d a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -19
#   <head>      ZTS: fail an import while the removal it restarted completes
#   ce428f99046 Stop the removal before a failed import unloads the pool
#   189a309d464 zinject: build injection points only into debug builds
#   55d286ee11b zinject: skip action, and a point in each txg's scan work
#   90ed2a0c68c Add a zfs_enter injection point
#   905a0240ba2 zinject: act at an injection point from a value
#   f4f1072e779 zinject: let a point end a pause when its thread must stop
#   cc61c9f76a8 raidz: an injection point before each reflow copy
#   36abb8d711f zinject: document point handlers outliving export and import
#   9a260c62e28 zinject: pause action for injection points
#   11471bec8f8 zinject: delay action for injection points
#   0933c2e7d82 libzfs_core: update the ABI for lzc_wait_inject()
#   86952909f90 zinject: Introduce waiting on injection events
#   95cc8348e45 zinject: add -n flag to allow error returns from anywhere in a function
#   284f2e98e87 zinject: name -p in its incompatible-option error
#   3c0848e2d63 ZTS: zinject -p's error for an incompatible option
#   eaea830c048 zinject: find pool delays behind other handlers
#   90a90232a14 ZTS: zinject pool delays with another handler registered
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c module/zfs/zio_inject.c module/zfs/vdev_removal.c tests/zfs-tests/tests/functional/removal/removal_failed_import.ksh
#   f2f5a5461a29d89e00724bd691462f9bd21c8719c5c6e876c3387745986dc020  module/zfs/spa.c
#   c6c3bbbb4f1c10f4d6a8e9969c18771d8f725181f2558384f8dd315646a3e43a  module/zfs/zio_inject.c
#   ad24555632db01145fb714673b451492410126f31f26388a6b9731f3d72605fa  module/zfs/vdev_removal.c
#   8fd7ae04ccc2b92e6366557c017e1167698ad064c918ad02422626d559d13136  .../removal_failed_import.ksh
grep -c 'spa_vdev_remove_suspend(spa);' module/zfs/spa.c                 # 2
grep -c 'vdev_remove_complete:start' module/zfs/vdev_removal.c   # 1
grep -n -e 'zinject -P import' -e 'zinject -q -n' tests/zfs-tests/tests/functional/removal/removal_failed_import.ksh
#   90:log_must zinject -P import -s 15 $TESTPOOL
#   91:id=$(zinject -q -n $POINT -A pause $TESTPOOL)
```

The fail-before tree:

```sh
D=/var/tmp/task-1123-ird-wr
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git fetch -q https://github.com/tigerblue77/zfs import-fail-removal-deadlock-v2-wr-fb && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~1) $(git rev-parse HEAD~17)"
#   d9b540cacdee3e5a124f0c83b50b409a3fe9b883 e0949a0780bd5c466b4e76c97b015dcce1b75ae9 189a309d464ca51c74d627fdbdd84b5992eade2d a1daebfec3f5ca46ac261ef339efdf0dbeae997c
sha256sum module/zfs/spa.c
#   6884528a1b85744188e96ae653c56c13cd7b79a2ade703b116c2d2805e56a823  module/zfs/spa.c (wrouesnel head's, without the fix)
grep -c 'spa_vdev_remove_suspend(spa);' module/zfs/spa.c                 # 1
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If a fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1123-ird-wr; L=/var/log/task-1123-ird-wr
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and kbuild's "compiler differs" note (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1123-ird-wr; L=/var/log/task-1123-ird-wr; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t removal_failed_import > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in about a minute. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the `NOTE: the removal is held at vdev_remove_complete:start` line, the `zinject -c` line, the lines of the import's exit (the `cannot import` message of zpool, if logged) and the `SUCCESS:` lines of `zpool import -d`, `wait_for_removal`, `vdevs_in_pool` and the last export; from the runner's timestamped `log` file in the same results directory, the times of `SUCCESS: zinject -P import -s 15 testpool` and `SUCCESS: sleep 2`, at least 17 s apart (the 15 s load delay, then the 2 s sleep; a gap of a few seconds means the import delay did not fire: report it); and from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log; if `zpool list` now hangs, this is a hang (capture, stop, report); otherwise go on to the next run; two FAILs in a row: stop part A and report.
2. `-t zinject_args` once (tag zi), after the pre-run checks: `cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_args > $L/zts-zi.log 2>&1 < /dev/null &`. Expected PASS (wrouesnel's version, which this branch does not change: his framework has no point list, and a handler for a pool that is not imported needs nothing new). Quote the result line and duration.
3. `-T removal` once (tag rm), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T removal > $L/zts-rm.log 2>&1 < /dev/null &` (up to an hour). Expected: every test of the group PASS, `removal_failed_import` among them. Quote every `Test:` line that is not PASS with the 20 lines before it, and the Results Summary. A FAIL is reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
4. `-T zpool_import` once (tag zi2), after the pre-run checks, the same way (`$L/zts-zpi.log`, up to 90 minutes): the fix is on the failure path of `spa_import()`. Expected all PASS (or the SKIPs the report lists as known); quote the Results Summary and any non-PASS as in 3.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

This part deadlocks the module on purpose: do it last, after part A is fully reported in $L/progress.md.

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1123-ird-wr; L=/var/log/task-1123-ird-wr
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t removal_failed_import > $L/zts-fb.log 2>&1 < /dev/null &
```

Poll the test's own log (under the results directory printed at the top of $L/zts-fb.log) every 30 s with `tail`; never run `zpool` or `zinject` yourself from now on (they would hang). Expected about 80 s after the start: `ERROR: ... zpool import did not return in 60s` (from `log_fail`), then the cleanup's note `zpool import is still running, with spa_namespace_lock held: leaving everything as it is`; the runner then starts the group's `cleanup.ksh`, which hangs. If instead the test PASSES: quote its whole own log and report it as the scenario not reproduced; unload normally, no reboot.

Once the `ERROR:` line is there (or 5 minutes after the start, whichever first), wait until 3 minutes after the `ERROR:` line (so that the hung task detector has reported), then capture into $L/hang-fb.txt: `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|zpool|remov|ksh|txg'`; `sudo -n cat /proc/<pid>/stack` of the `zpool import` process (expected frames, top down, near: `spa_vdev_remove_suspend` <- `spa_async_suspend` <- `spa_unload` <- `spa_import` <- `zfs_ioc_pool_import`) and of the removal kernel thread (`ps` shows it with `spa_vdev_remove` in its name; expected near: `mutex_lock` or `__mutex_lock` <- `spa_namespace_enter` <- `spa_vdev_enter` <- `vdev_remove_complete` <- `spa_vdev_remove_thread`), and of every other D task; `sudo -n dmesg | tail -n +$((N1+1))` (expected: `blocked for more than` reports for those two, and no VERIFY/ASSERT/PANIC/Oops). Quote in the report: the `ERROR:` line, the two stacks, the `blocked for more than` lines, and from the runner's `log` file the times of `SUCCESS: zinject -P import -s 15 testpool` and `SUCCESS: sleep 2` (at least 17 s apart, as in part A). Save $L/progress.md ("part B captured, rebooting"), `sync -f $L/progress.md`, kill the `zfs-tests.sh` tree if still running (`sudo -n pkill -f zfs-tests.sh`; the hung processes will not die, that is expected), then the pre-approved reboot.

## STEP 5: end

After the reboot: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.hung_task_panic` back to its step 0 value; `ls -ld /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/removal_failed_import 2>&1` (quote; remove only those of them, with `sudo -n rm -rf` and their names, and report anything else); `/etc/zfs/zpool.cache` as in step 0 (the failed import of part B writes no cache file; if it is above 0, report it and leave it).

Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the held note, the gap between the `zinject -P import` and `sleep 2` lines, the import's exit, the second import and removal check), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the `zinject_args` result; the two group results (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: the result and duration, the `ERROR:` line, the cleanup note, the two stacks, the `blocked for more than` lines, the dmesg check, the reboot (which mechanism, time). One-line verdict: "IRD PASS 3/3 debug, zinject_args PASS, -T removal PASS, -T zpool_import PASS; fail-before deadlock (import in spa_vdev_remove_suspend, removal in spa_namespace_enter) confirmed" or what differs.
