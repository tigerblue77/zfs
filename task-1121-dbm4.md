TASK-1121-DBM4 (from the coordinator session "main"): functional, about 2 h 30 builds included. Version 4 of our #19856 fix (PR #19927), reworked after a second review. (A) With the fix: the ZTS test `zinject_decompress_spacemap` five times on a debug build and three times on a release build, then the groups `-T zinject`, `-T arc` and `-T cache` once each on the debug build. (B) Fail-before: master plus only the PR's zinject injection point and test commits, debug build: the test must fail, which wedges txg_sync and needs one reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-dbm4`, `L=/var/log/task-1121-dbm4`.

## Why

Version 3 (TASK-1117-DBM3) retried a failed must-succeed `dbuf_read()` once blindly, then suspended the pool. The reviewer rejected the blind retry. Version 4:

- ARC (commit 1): a reader without `ZIO_FLAG_CANFAIL` that joined an in-flight read with it, which then fails with an I/O error, gets the read reissued for it (`arc_read_reissue()`, from `arc_read_done()`), instead of the error.
- dbuf (commit 2): a must-succeed reader that waited for another reader's read and finds the dbuf `DB_UNCACHED` reads the block itself; an error that still leaves the dbuf without data goes straight to the pool's failmode (`zio_suspend()`, then the read is retried after each resume), with the warning `cannot read a block it has to`; `dbuf_read()` asserts it never gets a parent zio without `DB_RF_CANFAIL`.
- zinject (commit 3, unchanged from v3): `zinject -e decompress` also fires in `arc_buf_fill()`.
- test (commit 4): as v3, and it now sets `zfs_compressed_arc_enabled=1` itself (restored at the end), so that the injected error always fails the block in the ARC after the read.

Expected on the test with the fix: PASS in about 10 s, exactly one `cannot read a block it has to`, one `has been suspended` and one `is being resumed` line per run, as in DBM3. Without the fix (part B): the DBM3 failure, `VERIFY(db->db_level != 0 || db->db_state == DB_CACHED || ...) failed` and `PANIC at dbuf.c:2407:dbuf_dirty()` in txg_sync.

What is tested: fork https://github.com/tigerblue77/zfs, branch `dbuf-mustsucceed-v4`, head `454af330ddd34ecd4bfe4f3ab4becdfdceec9a5d`, tree `cfa71d0df65e3e85b3b525ee7ebd10a25ecdc407`, four commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the last two commits (zinject, test) only: tree `b431fbe9f341436c16a5be5475b842708f1f26e2`, whose `module/zfs/dbuf.c` is master's (`8bbb6b4c...`, the same file as DBM3's fail-before tree).

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, or a non-empty cache file, is a stop: report its `ls -l`, touch nothing. A stale `/var/tmp/tunable-*` file is a stop too (in DBM3 one left by a panicked run made `save_tunable` fail): report its `ls -l` and wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. In part A, capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: exactly one is pre-approved, in step 6, after the fail-before run. In part A, a hang, a panic or a module that will not unload: capture as above, stop, report and wait for main. Before the step 6 reboot, arrange your own resumption the way DBM3 did (a scheduled task of your own session), say which, and write in $L/progress.md where you are. A global `sync` may block once txg_sync is wedged: use `sync -f $L/progress.md`, then the sysrq reboot through a transient unit, as in DBM3.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 25 GiB free on /var/tmp: three trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 and say so; above 0: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-dbm4; L=/var/log/task-1121-dbm4
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/v4-dbg && cd $D/v4-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs dbuf-mustsucceed-v4 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~4) $(git status --porcelain | wc -l)"
#   454af330ddd34ecd4bfe4f3ab4becdfdceec9a5d cfa71d0df65e3e85b3b525ee7ebd10a25ecdc407 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -5
#   454af330ddd ZTS: test a space map block that fails to decompress
#   e3cc19afd67 zinject: inject decompression errors in the ARC too
#   5fc9ff221e5 Suspend pool when a must-succeed dbuf read yields no data
#   bd04bea2437 arc: reissue a failed read for a reader that must succeed
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/dbuf.c module/zfs/arc.c module/zfs/zio_inject.c cmd/zinject/zinject.c tests/zfs-tests/tests/functional/cli_root/zinject/zinject_decompress_spacemap.ksh
#   afc342ea04df18db50d0a09e43b558fc33b72aab1edc271e1e5d7e1c5e9ac95d  module/zfs/dbuf.c
#   4a59472ff09bbced04da556dd7326ab8bc64e4435c2469cfd7c63be7f2429bbb  module/zfs/arc.c
#   383d8235b296a19b9b63328bf95acec69fe352e08160d4e7dcb9f66e85316954  module/zfs/zio_inject.c
#   3d748774f1e80b8981afe79ad685c9ba17e3d8b97410ddd4398625ebc31d90cf  cmd/zinject/zinject.c
#   b2afd2843c69640fa570547bf455718f9e23c88bbfb32d1881af609a62036299  .../zinject_decompress_spacemap.ksh
grep -c arc_read_reissue module/zfs/arc.c; grep -c dbuf_read_suspend module/zfs/dbuf.c   # 3 and 2
git clone -q $D/v4-dbg $D/v4-rel && git -C $D/v4-rel checkout -q 454af330ddd34ecd4bfe4f3ab4becdfdceec9a5d && git -C $D/v4-rel rev-parse HEAD^{tree}   # same tree
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-dbm4
git clone -q $D/v4-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick e3cc19afd67e1ecf27b560a7cef6845ddcfb8afb 454af330ddd34ecd4bfe4f3ab4becdfdceec9a5d
git rev-parse HEAD^{tree}          # b431fbe9f341436c16a5be5475b842708f1f26e2
sha256sum module/zfs/dbuf.c module/zfs/arc.c
#   8bbb6b4cd4cc6fa1c968815c5f13839cc31b06d4eb1976936e148c32571988f1  module/zfs/dbuf.c   (master's)
#   18e5bae7f74edbc7d4b7f735051363707fdd4af0fb4b95128d63dff1f21724b8  module/zfs/arc.c    (master's + the injection hook)
grep -c 'arc_read_reissue\|dbuf_read_suspend' module/zfs/arc.c module/zfs/dbuf.c   # 0 and 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the three at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-dbm4; L=/var/log/task-1121-dbm4
setsid nohup sh -c "cd $D/v4-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-v4-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/v4-rel && ./autogen.sh && ./configure && nice make -j3; echo build-rc=\$?" > $L/build-v4-rel.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected: the four commits build without warnings on gcc 13, debug and release, user space and module). Note S1, S2, S3 = `modinfo -F srcversion <tree>/module/zfs.ko` for v4-dbg, v4-rel, fb-dbg. S1 and S2 are expected equal (same source, as in DBM3); S3 must differ from them.

## STEP 3: part A, debug build with the fix

`sudo -n $D/v4-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Quote `cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled` (1 expected, the default). Note N0=`sudo -n dmesg | wc -l`.

1. The new test, five times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-dbm4; L=/var/log/task-1121-dbm4; n=1   # then 2 .. 5
   cd $D/v4-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_decompress_spacemap > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in about 10 s. For each run quote the result line and duration, `cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled` after it (1: the test restores it), and from dmesg since the previous count: the number of lines `cannot read a block it has to` (quote the first in full: it names objset 0 and a space map object, error 5), `has been suspended` and `is being resumed` (1, 1, 1 expected), and every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of its `<result dir>/log`, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zinject` once (tag az), after the pre-run checks: `cd $D/v4-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zinject > $L/zts-az.log 2>&1 < /dev/null &`. Expected PASS: `zinject_args`, `zinject_counts`, `zinject_decompress_spacemap`, `zinject_probe`. `zinject_counts` has a known intermittent failure on master (a freq=100 `match=<m> inject=<m-1>`, the counter race of TASK-1120-ZIC, not this change): if it fails that way, quote its `check_count_freq` lines and record it as that race, without rerun.
3. `-T arc` once (tag aa), then `-T cache` once (tag ac), each after the pre-run checks, same command form. Expected all PASS. Quote every `Test:` line with result and duration and the Results Summary of each group. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected); the counts of `cannot read a block it has to`, `has been suspended` and `is being resumed` over the whole step (expected 6 each: five runs and the one inside `-T zinject`). Unload with `sudo -n $D/v4-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-v4-dbg.txt`.

## STEP 4: part A, release build with the fix

`sudo -n $D/v4-rel/scripts/zfs.sh`; srcversion equal to S2 and a load line without "(DEBUG mode)". Note N1. The new test three times (tags ar-1 .. ar-3), exactly as in step 3.1. Then the dmesg check since N1, unload, `sudo -n dmesg > $L/dmesg-v4-rel.txt`.

## STEP 5: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S3 and "(DEBUG mode)"; note N2. Write $L/progress.md ("step 5 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached under `timeout 900` (`$L/zts-fb.log`), and poll its log and dmesg every 10 s for at most 15 minutes. Expected: FAIL, with `VERIFY(db->db_level != 0 || db->db_state == DB_CACHED || db->db_state == DB_FILL || db->db_state == DB_NOFILL) failed` and `PANIC at dbuf.c:2407:dbuf_dirty()` in txg_sync, and the runner returning on its own (about 100 s in DBM3). Quote the result line and duration, the assertion line, the first 15 frames of the txg_sync stack, and whether a `cannot read a block it has to` line appeared (none expected: the fix is absent). `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

The release fail-before (the NULL pointer dereference in `arc_release()`) is not repeated: DBM3 captured it with the same zinject commit and test, and the only change to the test, turning on the compressed ARC, sets the default value.

## STEP 6: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `ls -l /var/tmp/tunable-* 2>&1`: the panicked run cannot restore its tunables, so `tunable-COMPRESSED_ARC_ENABLED`, `tunable-KEEP_LOG_SPACEMAPS_AT_EXPORT` and `tunable-METASLAB_PRELOAD_ENABLED` are expected: quote their `ls -l`, then remove exactly those three (`sudo -n rm -f` with the three names; any other `tunable-*` file is left and reported); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (quote; files left by the panicked run under `/var/tmp/zinject_decompress_spacemap` or the ZTS directories are listed with `ls -l` and removed only if this task created them); `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`; `ls -l /etc/zfs/zpool.cache 2>&1` (size 0: move it to $L/zpool.cache.end; above 0: report it and leave it).

## Report

Write $L/report.md and give the same text as your final message. Part A: one table per build (run, result, duration, cannot-read / suspended / resumed counts, `zfs_compressed_arc_enabled` after the run), the first `cannot read a block it has to` line, then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the three group results (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: the result, the assertion (first line, file:line), the first 15 frames of txg_sync, whether the runner returned, the reboot (time, how you resumed), the tunable files removed. One-line verdict: "v4 PASS 5/5 debug + 3/3 release, -T zinject/arc/cache PASS; fail-before confirmed on debug" or what differs.
