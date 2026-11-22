TASK-1122-MRH (from the coordinator session "main"): functional, about 2 h 30 builds included. A fix for a master bug found on 22/11 with the M-05 model: the hold that `spa_vdev_exit()` takes on a suspended pool since #19709 is counted in `spa_minref` when a load of the pool ends meanwhile, and the pool can then never be exported. Its test runs a libzpool program from ZTS. (A) With the fix, debug build: the new ZTS test `removal_suspended_load` three times, then the groups `-T removal` and `-T failmode` once each. (B) Fail-before: master plus only the test commit, debug build: the test must FAIL, the export of the program's own pool returning 16 (EBUSY). The test's pool lives in user space (libzpool): no kernel pool, no kernel error and no reboot are expected in either part. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1122-mrh`, `L=/var/log/task-1122-mrh`.

## Why

`spa_load_impl()` drops `spa_namespace_lock` for the second half of a load and restarts a device removal in progress there (`spa_restart_removal()`), then takes the lock back at its end; `spa_load()` then sets `spa_minref` from the reference count. If the removal completes in between and the pool is suspended during the sync of its last step, `spa_vdev_exit()` (#19709) takes a hold, drops the lock and waits; the load counts that hold in `spa_minref`. Once the pool resumes and the hold is dropped, the idle pool's count stays one below `spa_minref`, and every export fails with EBUSY until a reboot. The fix counts these holds in `spa->spa_vdev_exit_holds` and leaves them out of `spa_minref`.

- fix (commit 1, `547c1794623`): include/sys/spa_impl.h, module/zfs/spa.c (`spa_load()`), module/zfs/spa_misc.c (`spa_vdev_config_exit_impl()`).
- test (commit 2, head): `tests/zfs-tests/cmd/removal_suspended_load.c`, a libzpool program, and `functional/removal/removal_suspended_load.ksh`, which runs it on two 256 MiB file vdevs in `$TEST_BASE_DIR/removal_suspended_load`. The kernel cannot be driven into this window with zinject (the removal holds the namespace lock between its two steps, and no handler can be added while a pool loads), so the program does it in libzpool: removal paused, export, import with a 10 s load delay, config lock held as reader so that the restarted removal stops in `spa_vdev_enter()` with the namespace lock, a sync started that waits for the config lock, pool marked suspended without failing I/O, config lock released; the removal's last step then drops the namespace lock while the load waits for it. The program prints its steps with timestamps; it ends `PASS` (exit 0), or `export returned 16` with `FAIL: the reference count is below spa_minref` (exit 1), or `TIMEOUT:`/`ERROR:` (exit 3/2: the sequence itself went wrong).

Expected with the fix: PASS in about 20 s, the program's log showing `load ended: refcount 12, spa_minref 11`, then `removal done: refcount 11, spa_minref 11` and `export returned 0`. Without the fix (part B): FAIL in about 20 s, with `load ended: refcount 12, spa_minref 12`, `removal done: refcount 11, spa_minref 12`, `FAIL: the reference count is below spa_minref`, `export returned 16`. Measured in libzpool (user space, debug, 22/11): 3/3 each way.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-minref-suspended-hold` (main pushes it before this task starts), head `88cd308a9c0d4786b8252b4d30f02e013b7e8007`, tree `d5a2be6011fa9c54b558ee81a0e80d2ee3b688c8`: upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11), then the fix, then the test. The fail-before tree is branch `spa-minref-suspended-hold-fb`, head `f04a596238e440b5ca17ce6263d5664dff5090f6`, tree `16a3a68fc0fa002716ac0740b9336be037acc3b6`: master plus the test commit only, whose three kernel files are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/removal_suspended_load 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 90 minutes. Capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|removal'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: none is expected and none is pre-approved. A hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/removal_suspended_load 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1122-mrh; L=/var/log/task-1122-mrh
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs spa-minref-suspended-hold && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   88cd308a9c0d4786b8252b4d30f02e013b7e8007 d5a2be6011fa9c54b558ee81a0e80d2ee3b688c8 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   <head> ZTS: complete a removal on a suspended pool during its load
#   <fix>  Leave the hold of a suspended vdev change out of spa_minref
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c module/zfs/spa_misc.c tests/zfs-tests/cmd/removal_suspended_load.c
#   37a2837d62aa1015dcd0b7a1ebf1fe932baff8fb7f7c1b0a3d963304efa1edc0  module/zfs/spa.c
#   905b5e6b07802805a1bc521f65249bd01d24fb5082a9b11bb1372b1c3ae7350b  module/zfs/spa_misc.c
#   88210fcdd08226cab1a69542798742f9ac82f7b01e20787b3e185b585dac7179  tests/zfs-tests/cmd/removal_suspended_load.c
grep -c 'spa_vdev_exit_holds' module/zfs/spa.c module/zfs/spa_misc.c include/sys/spa_impl.h   # 1, 2, 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1122-mrh
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git fetch -q https://github.com/tigerblue77/zfs spa-minref-suspended-hold-fb && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~1)"
#   f04a596238e440b5ca17ce6263d5664dff5090f6 16a3a68fc0fa002716ac0740b9336be037acc3b6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c
sha256sum module/zfs/spa.c module/zfs/spa_misc.c
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c      (master's)
#   85d49ddbb567e460411a446504213d2275fbf46ce099cf1f8fc48c5b399bbf35  module/zfs/spa_misc.c (master's)
grep -c 'spa_vdev_exit_holds' module/zfs/spa.c module/zfs/spa_misc.c include/sys/spa_impl.h   # 0, 0, 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If a fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1122-mrh; L=/var/log/task-1122-mrh
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and kbuild's "compiler differs" note (none expected). Check `ls -l $D/*/tests/zfs-tests/cmd/removal_suspended_load` (both trees built the program). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1122-mrh; L=/var/log/task-1122-mrh; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t removal_suspended_load > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in about 20 s. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the program's lines `load ended:`, `removal done:` and `export returned` (expected `spa_minref 11`, `refcount 11, spa_minref 11`, `0`), and any `TIMEOUT:` or `ERROR:` line (none expected); from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected: the program is user space). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T removal` once (tag rm), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T removal > $L/zts-rm.log 2>&1 < /dev/null &` (up to an hour). Expected: every test of the group in `common.run` PASS, `removal_suspended_load` among them. Quote every `Test:` line that is not PASS with the 20 lines before it, and the Results Summary. A FAIL is reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
3. `-T failmode` once (tag fm), after the pre-run checks, the same way (`$L/zts-fm.log`): its `failmode_vdev_detach` is #19709's own test, the path the fix touches. Expected all PASS; quote the Results Summary and any non-PASS as in 2.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1122-mrh; L=/var/log/task-1122-mrh
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t removal_suspended_load > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected within a minute: FAIL. Quote the result line and duration, and from the test's own log: the program's lines `load ended:` (expected `refcount 12, spa_minref 12`), `removal done:` (expected `refcount 11, spa_minref 12`), `FAIL: the reference count is below spa_minref` and `export returned 16`, and the `ERROR:` line of `log_must removal_suspended_load` with its exit status (1 expected). A `TIMEOUT:` or `ERROR:` line of the program instead means the sequence itself went wrong, not the bug: quote the whole program output. Then: every line of `sudo -n dmesg | tail -n +$((N1+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than` (none expected); the pre-run checks once more (no pool, no leftover `/var/tmp/removal_suspended_load`). If the test PASSES instead: quote the test's whole own log and report it as the scenario not reproduced; do not rerun it.

Unload with `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc, 0 expected) and save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`; `ls -l /etc/zfs/zpool.cache 2>&1` (absent or empty expected; above 0: report it and leave it); `sysctl kernel.hung_task_panic` back to its step 0 value if you changed it.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the three program lines, any `TIMEOUT:`/`ERROR:`), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the `-T removal` and `-T failmode` results (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: the result, the four program lines and the `ERROR:` line, the dmesg check, the unload rc. One-line verdict: "MRH PASS 3/3 debug, -T removal PASS, -T failmode PASS; fail-before FAIL (spa_minref 12 for 11 references, export 16) confirmed" or what differs.
