TASK-1121-ROP (from the coordinator session "main"): functional, about 75 min builds included. Our fix for `zpool reopen` not reaching cache devices (fork branch `zpool-reopen-cache-devices`, no PR yet): `zpool reopen` reopens the vdevs under the root vdev only, so a cache device that was missing when the pool was imported stays UNAVAIL after `zpool reopen`, once it is back, although `zpool online <pool> <cache device>` brings it back. (A) With the fix, debug build: the new ZTS test `l2arc_reopen_pos` three times, then the group `-T l2arc` once, then the group `-T zpool_reopen` once (Linux, scsi_debug). (B) Fail-before: master plus only the test commit, debug build: the test once; it is expected to FAIL within a minute, not to hang. No reboot is expected or pre-approved. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-rop`, `L=/var/log/task-1121-rop`.

## Why

Read on master `a1daebfec3f`; no report of it upstream.

- `zfs_ioc_pool_reopen()` calls `vdev_reopen(spa->spa_root_vdev)` only (module/zfs/zfs_ioctl.c:6705), although the comment above it says "Reopen all the vdevs associated with the pool." (6662) and zpool-reopen(8) says "Reopen all vdevs associated with the specified pools" (man/man8/zpool-reopen.8:34). Cache devices live in `spa->spa_l2cache`, outside the tree. `zpool online` of a cache device runs `vdev_reopen()` on it (module/zfs/vdev.c:4768), and `vdev_reopen()` has the code for cache devices (module/zfs/vdev.c:3017-3033): that is why `zpool online` works and `zpool reopen` does not.
- Available hot spares are left out on purpose: they do no I/O, and zpool(8) refuses to online one (lib/libzfs/libzfs_pool.c:3618).

- fix (commit 1, `0b0fe815b08`): after `vdev_reopen()` of the root vdev, `zfs_ioc_pool_reopen()` calls `vdev_reopen()` on each cache device.
- test (commit 2, `91b3b1f7722`): `l2arc_reopen_pos` (group `functional/l2arc`, needs fio) creates `testpool` on the group's 1 GiB file with a 280 MiB file cache device, writes 200 MiB with fio, exports the pool, moves the cache file out of the group's vdev directory, imports with `zpool import -d <that directory>` (the cache device is UNAVAIL, `zpool status -x` reports the pool), moves the file back, runs `zpool reopen testpool`, and checks that the cache device is ONLINE, that `zpool status -x` says the pool is healthy, and that `arcstats.l2_write_bytes` grows within 60 seconds of fio random reads.

Expected with the fix: PASS each time, in about 30 to 60 seconds. Without the fix (part B): FAIL right after `zpool reopen testpool`, with `The cache device is 'UNAVAIL', expected 'ONLINE'.` Deterministic.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zpool-reopen-cache-devices`, head `91b3b1f77226306daf29de001b098953820a4878`, tree `26d85736ad077acfb6eb35c502585eeeada79010`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `337bc472afa43068b049dc758fd86b09a6d39d95`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs and, for `zpool_reopen`, its own scsi_debug disk; touch no other block device of the VM and no pool this task did not create; never `zpool import` anything by hand; never `zfs-tests.sh -x`; never pass `-d` or set `DISKS`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, `lsmod | grep -E '^scsi_debug '` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file, a loaded scsi_debug or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l` or `lsmod` line, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 10 minutes; the `l2arc` group after 60 minutes; the `zpool_reopen` group after 30 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|l2arc'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl|scsi_debug) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id zts`, `command -v fio lsscsi` (fio is required: without it the test is reported UNSUPPORTED, so stop and report; lsscsi is needed for part A3 only), `modinfo -F filename scsi_debug` (part A3 only), `pgrep -a -x zed` (quote it; do not stop it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-rop; L=/var/log/task-1121-rop
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zpool-reopen-cache-devices && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   91b3b1f77226306daf29de001b098953820a4878 26d85736ad077acfb6eb35c502585eeeada79010 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   91b3b1f7722 ZTS: Reopen a cache device that was missing at import
#   0b0fe815b08 zpool reopen: Reopen the cache devices of the pool
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/zfs_ioctl.c tests/zfs-tests/tests/functional/l2arc/l2arc_reopen_pos.ksh tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am
#   e842cb6158c0a026f8cd9dc8782a0837b0f148b78d02be912dacac33df28b8bb  module/zfs/zfs_ioctl.c
#   60af455e3f600b52bada458f4f2895384bca78c6ad2b1dbf9a53fd5161d4aae9  .../l2arc/l2arc_reopen_pos.ksh
#   d6bbec057e397418eb66c35fac01b4eafb43e6ef9f690614484973c975604d3c  tests/runfiles/common.run
#   565247c053495d8edadf13cd65887450214c610650adcf363f974b4c0e2b3f8d  tests/zfs-tests/tests/Makefile.am
grep -c 'vdev_reopen(spa->spa_l2cache.sav_vdevs\[i\]);' module/zfs/zfs_ioctl.c   # 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-rop
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick 91b3b1f77226306daf29de001b098953820a4878
git rev-parse HEAD^{tree}          # 337bc472afa43068b049dc758fd86b09a6d39d95
sha256sum module/zfs/zfs_ioctl.c tests/zfs-tests/tests/functional/l2arc/l2arc_reopen_pos.ksh
#   da8ee7862d88d5dee0e80f7be936b1ff25121d7c1c13521068bb2152bd566f98  module/zfs/zfs_ioctl.c  (master's)
#   60af455e3f600b52bada458f4f2895384bca78c6ad2b1dbf9a53fd5161d4aae9  .../l2arc/l2arc_reopen_pos.ksh
grep -c spa_l2cache module/zfs/zfs_ioctl.c   # 0 (1 in fix-dbg)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-rop; L=/var/log/task-1121-rop
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and the kbuild "compiler differs" note (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks (tags ac-1, ac-2, ac-3):

   ```sh
   D=/var/tmp/task-1121-rop; L=/var/log/task-1121-rop; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_reopen_pos > $L/zts-ac-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time. For each run quote: the result lines (the group's `setup` and `cleanup` included) and the test's duration; from the test's own log under the results directory the runner prints, every line with `zpool import`, `zpool reopen`, `pool_is_healthy`, `fio` or `Nothing was written` (expected: `zpool import -d ... testpool` SUCCESS, `pool_is_healthy` failing as expected under log_mustnot, `zpool reopen testpool` SUCCESS, `pool_is_healthy` SUCCESS, the two fio runs SUCCESS, no "Nothing was written"); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T l2arc` once (tag al), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T l2arc > $L/zts-al.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/l2arc]` in `common.run` PASS, `l2arc_reopen_pos` included. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
3. `-T zpool_reopen` once (tag ar), after the pre-run checks, only if step 0 found `lsscsi` and the scsi_debug module (otherwise say so and skip): `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_reopen > $L/zts-ar.log 2>&1 < /dev/null &`. These tests reopen pools without cache devices, so they check that the data devices are still reopened as before. Expected: every test of `[tests/functional/cli_root/zpool_reopen:Linux]` in `linux.run` PASS. Quote as in 2. Afterwards `lsmod | grep -E '^scsi_debug '` must be empty (the group's cleanup unloads it); if not, quote it and stop.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks (tag fb):

```sh
D=/var/tmp/task-1121-rop; L=/var/log/task-1121-rop
cd $D/fb-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_reopen_pos > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected: FAIL within a minute; in the test's own log, `zpool import -d ... testpool` SUCCESS, `zpool reopen testpool` SUCCESS, then `The cache device is 'UNAVAIL', expected 'ONLINE'.`; no fio random reads (the test stops at that check). Quote: the result line and duration; the lines of the test's own log from the `zpool export` to the end; dmesg since N1 for the same patterns as in step 3 (none expected). A PASS here means the fail-before was not reproduced: quote the whole test log, then run it once more the same way (tag fb2); two PASSes: report it plainly. A hang (the test after 10 minutes): capture as in the VM rules, stop and report.

Then the pre-run checks once more (the test's cleanup must have destroyed `testpool` even after the FAIL, and the group's cleanup removed `/var/tmp/testdir`), `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc), `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl|scsi_debug) '` empty; `losetup -a` (quote); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` (none); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. `/var/tmp/test_results` and $L stay.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the import/reopen/healthy/fio lines as seen in the test log), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the two group results (Results Summary, every non-PASS with its lines, or why `zpool_reopen` was skipped); srcversions and load lines. Part B: result and duration, the test log lines from the `zpool export` on, dmesg, and whether the cleanup left anything behind. One-line verdict: "ROP PASS 3/3 debug (cache device missing at import ONLINE, pool healthy and L2ARC writing after zpool reopen <pool>), -T l2arc PASS, -T zpool_reopen PASS; fail-before: FAIL 'The cache device is 'UNAVAIL', expected 'ONLINE'.': confirmed" or what differs.
