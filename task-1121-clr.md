TASK-1121-CLR (from the coordinator session "main"): functional, about 45 min builds included. Our fix for `zpool clear <pool>` not reaching cache devices (fork branch `zpool-clear-cache-devices`, no PR yet): with no device named, `zpool clear` only clears the vdevs under the root vdev, so a faulted cache device stays FAULTED, although `zpool clear <pool> <cache device>` clears it. (A) With the fix, debug build: the new ZTS test `zpool_clear_cache_device` three times, then the group `-T zpool_clear` once. (B) Fail-before: master plus only the test commit, debug build: the test once; it is expected to FAIL within seconds, not to hang. No reboot is expected or pre-approved. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-clr`, `L=/var/log/task-1121-clr`.

## Why

Read on master `a1daebfec3f`; no report of it upstream.

- `zfs_ioc_clear()` passes `vd = NULL` to `vdev_clear()` when no device is named (module/zfs/zfs_ioctl.c:6633-6645). `vdev_clear()` replaces NULL with the root vdev and recurses into its children only (module/zfs/vdev.c:4977-4989). Cache devices live in `spa->spa_l2cache`, outside that tree. A named device is found by `spa_lookup_by_guid(..., B_TRUE)`, which also searches the cache devices (module/zfs/spa.c:11818-11841), so `zpool clear <pool> <cache device>` works.
- Meanwhile `zpool status` reports a pool with a FAULTED cache device as "One or more devices are faulted ... use 'zpool clear' to mark the device repaired" (lib/libzfs/libzfs_status.c:206-216 checks cache devices; cmd/zpool/zpool_main.c:11184-11193), and zpool-clear(8) says "If no arguments are specified, all device errors within the pool are cleared."
- Available hot spares are left out on purpose: zpool refuses to fault them (since b44a3ecf4a2) and to clear one by name.

- fix (commit 1, `0125775dbcd`): `vdev_clear(spa, NULL)` clears the root vdev as before, then each cache device, the way a named cache device is cleared.
- test (commit 2, `b2c12f7065e`): `zpool_clear_cache_device` (group cli_root/zpool_clear) creates `testpool1` on a 256 MiB file with a 256 MiB file cache device under `/var/tmp`, faults the cache device with `zpool offline -f`, runs `zpool clear testpool1`, and checks that the cache device is ONLINE and that `zpool status -x testpool1` says it is healthy; then the same with `zpool offline -tf`.

Expected with the fix: PASS each time, in a few seconds. Without the fix (part B): FAIL right after the first `zpool clear testpool1`, with `The cache device is 'FAULTED', expected 'ONLINE'.` Deterministic.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zpool-clear-cache-devices`, head `b2c12f7065ef7f6f493bd296a20101d0edc76eb4`, tree `2652ad82d39cbfc19cd786e4f953000bf1638446`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `cb064baf9d2bcd95a80294fa6c2b824468f5527e`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_clear_cache_device.* 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 10 minutes; the group after 60 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id zts`, `pgrep -a -x zed` (quote it; a running zed does not change this test's outcome, do not stop it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_clear_cache_device.* 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-clr; L=/var/log/task-1121-clr
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zpool-clear-cache-devices && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   b2c12f7065ef7f6f493bd296a20101d0edc76eb4 2652ad82d39cbfc19cd786e4f953000bf1638446 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   b2c12f7065e ZTS: Clear a faulted cache device with zpool clear <pool>
#   0125775dbcd vdev_clear: Clear cache devices on a pool-wide clear
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/vdev.c tests/zfs-tests/tests/functional/cli_root/zpool_clear/zpool_clear_cache_device.ksh tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am
#   a042a28ad2547abfd6a139c8a3e2e02efb3b6dcc92707ed9e3f6b6e1928312b1  module/zfs/vdev.c
#   b54f36d99397fea82293fa5cac892dff0b6cbd69af767f5858843d49a6106fb8  .../zpool_clear/zpool_clear_cache_device.ksh
#   78205f18fa4a6f6f87c571c1762700cca09d8bf93a4f57211bb56e0de858cfcf  tests/runfiles/common.run
#   f9de57f7ca281ae4f9c701753b7e5a7bf9a792f8b62f2fe4ed409263a2370e66  tests/zfs-tests/tests/Makefile.am
grep -c 'vdev_clear(spa, spa->spa_l2cache.sav_vdevs\[i\]);' module/zfs/vdev.c   # 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-clr
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick b2c12f7065ef7f6f493bd296a20101d0edc76eb4
git rev-parse HEAD^{tree}          # cb064baf9d2bcd95a80294fa6c2b824468f5527e
sha256sum module/zfs/vdev.c tests/zfs-tests/tests/functional/cli_root/zpool_clear/zpool_clear_cache_device.ksh
#   313e903ad7462b027bf868f950d76ac12abb896a8f7b2231646485724b8cfd8b  module/zfs/vdev.c  (master's)
#   b54f36d99397fea82293fa5cac892dff0b6cbd69af767f5858843d49a6106fb8  .../zpool_clear/zpool_clear_cache_device.ksh
sed -n '/^vdev_clear(spa_t/,/^}/p' module/zfs/vdev.c | grep -c spa_l2cache   # 0 (2 in fix-dbg)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-clr; L=/var/log/task-1121-clr
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and the kbuild "compiler differs" note (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks (tags ac-1, ac-2, ac-3):

   ```sh
   D=/var/tmp/task-1121-clr; L=/var/log/task-1121-clr; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_clear_cache_device > $L/zts-ac-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in a few seconds. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, every line with `zpool offline`, `zpool clear` or `pool_is_healthy` (expected: for each of `-f` and `-tf`, `zpool offline` SUCCESS, `pool_is_healthy` failing as expected under log_mustnot, `zpool clear testpool1` SUCCESS, `pool_is_healthy` SUCCESS); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zpool_clear` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_clear > $L/zts-az.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/cli_root/zpool_clear]` in `common.run` PASS (`zpool_clear_001_pos`, `zpool_clear_002_neg`, `zpool_clear_003_neg`, `zpool_clear_cache_device`, `zpool_clear_readonly`). Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks (tag fb):

```sh
D=/var/tmp/task-1121-clr; L=/var/log/task-1121-clr
cd $D/fb-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_clear_cache_device > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected: FAIL within seconds; in the test's own log, `zpool offline -f` SUCCESS, `zpool clear testpool1` SUCCESS, then `The cache device is 'FAULTED', expected 'ONLINE'.`; no `-tf` step (the test stops at the first check). Quote: the result line and duration; the lines of the test's own log from the `zpool create` to the end; dmesg since N1 for the same patterns as in step 3 (none expected). A PASS here means the fail-before was not reproduced: quote the whole test log, then run it once more the same way (tag fb2); two PASSes: report it plainly. A hang (the test after 10 minutes): capture as in the VM rules, stop and report.

Then the pre-run checks once more (the test's cleanup must have destroyed `testpool1` and removed `/var/tmp/zpool_clear_cache_device.*` even after the FAIL), `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc), `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl) '` empty; `losetup -a` (quote); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_clear_cache_device.* 2>&1` (none); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. `/var/tmp/test_results` and $L stay.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the two clear cycles as seen in the test log), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: result and duration, the test log lines from the `zpool create` on, dmesg, and whether the cleanup left anything behind. One-line verdict: "CLR PASS 3/3 debug (cache device ONLINE and pool healthy after zpool clear <pool>, -f and -tf), -T zpool_clear PASS; fail-before: FAIL 'The cache device is 'FAULTED', expected 'ONLINE'.': confirmed" or what differs.
