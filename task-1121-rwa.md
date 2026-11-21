TASK-1121-RWA (from the coordinator session "main"): functional, about 1 h 10 builds included. A regression test for our open PR #19370 ("Do not take a concurrent write for a physical rewrite"), rebuilt on our zinject named-point series (N-01, fork branch `zinject-delay-points-v2`): a write that dirties a block while `zfs rewrite -P` is between its "not dirty" check and its dirtying is written as a physical rewrite, keeps the old logical birth time, and an incremental stream leaves it out. (A) With the fix, debug build: the new ZTS test `zfs_rewrite_physical_append` three times, `zinject_args` once, then the group `-T zfs_rewrite` once. (B) Fail-before: N-01 plus only the test commit, debug build: the test must FAIL at its last `cmp`, the replica missing the append. No kernel error and no reboot are expected in either part. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-rwa`, `L=/var/log/task-1121-rwa`.

## Why

`dmu_buf_will_rewrite()` (master) takes `db_mtx`, checks that the buffer is not dirty in the txg of the rewrite, drops `db_mtx`, dirties the buffer with `dmu_buf_will_dirty_flags()`, then takes `db_mtx` again and sets `dr_rewrite` on whatever dirty record the txg now has. A write that dirties the buffer in between gets its own dirty record marked: its data is written as a physical rewrite, which keeps the logical birth time of the block it replaces (`zio_dva_allocate()`), and an incremental send from a snapshot taken before leaves the block out. `zfs_rewrite()` range-locks `[off, end of file)`, while an append locks from the end of the file on, so both can dirty the last block of a file at the same time. The fix dirties the buffer and marks the record in one step under `db_mtx` (`dbuf_dirty_impl(db, tx, rewrite)`), and leaves a buffer already dirty in the txg as it is.

- fix (commit 5, `5f89285b5ac`): #19370's commit `694edec3f0a`, cherry-picked unchanged onto N-01 (`git range-diff` identical).
- test (commit 6, `f0a90e690f3`): adds the zinject point `dmu_buf_will_rewrite:clean` (in `dmu_buf_will_rewrite()`, right after the "not dirty" check drops `db_mtx`) and `zfs_rewrite_physical_append`: a file of 192 KiB with recordsize 128K (block 1 holds `[128K, 192K)` and ends the file), snapshot `@a` received into a replica with `-u`; `zfs_txg_timeout` raised to 600 and `zpool sync`, so that no txg starts to quiesce on its own; `zinject -w dmu_buf_will_rewrite:clean -s 60`; `zfs rewrite -P -o 131072 file` in the background, held at the point; then a 4 KiB append (`cat chunk >> file`), which must complete within 10 s while the rewrite is held; the test then fails if the txgs kstat shows a txg in state `Q` (the append would be in a later txg and test nothing); it releases the rewrite, restores the timeout, checks the source file, snapshots `@b`, receives `-i @a @b` into the replica and compares the replica's file with the expected content.

Expected with the fix: PASS in well under a minute. Without the fix (part B): FAIL at the very last check, `cmp /var/tmp/rewrite_append.expect /testpool/testfs2/file`, with cmp reporting the first difference at byte 196609 (the first byte of the append: the replica's block 1 is the one of `@a`, which reads as zeros past 192 KiB). Everything before it must succeed, the source-file `cmp` included; no kernel message is expected.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zfs-rewrite-write-race-v2` (main pushes it before this task starts; the PR branch of #19370, `zfs-rewrite-write-race`, is not touched), head `f0a90e690f333a6b0517bc62bb7e2681cd1e81da`, tree `230dbd951dcddf89b1ed0813d9ae47a26e9cd3aa`: N-01's four commits (`4a98dfeb57b`, on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, 20/11), then the fix, then the test. The fail-before tree is N-01 plus the test commit only: tree `ed1b85fea34ca6b54e6b3c61bdc13b4117fcf942`, whose `module/zfs/dbuf.c` is master's plus the two lines of the point.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/rewrite_append.* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. Capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: none is expected. One is pre-approved, in step 5 only, and only if after part B the module does not unload or `zpool list` does not return: then arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, write in $L/progress.md where you are, `sync -f $L/progress.md`, and the sysrq reboot through a transient unit, as in DBM4. In part A, a hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/rewrite_append.* 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-rwa; L=/var/log/task-1121-rwa
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zfs-rewrite-write-race-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git rev-parse HEAD~6) $(git status --porcelain | wc -l)"
#   f0a90e690f333a6b0517bc62bb7e2681cd1e81da 230dbd951dcddf89b1ed0813d9ae47a26e9cd3aa 4a98dfeb57bddb64160b6c39f111e2e46b971ea6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -7
#   f0a90e690f3 ZTS: append to a block that zfs rewrite -P is about to dirty
#   5f89285b5ac Do not take a concurrent write for a physical rewrite
#   4a98dfeb57b ZTS: free a dataset while its upgrade task returns
#   d074b306736 ZTS: leave a fresh vdev file after each upgrade test
#   af8df3d6ebe ZTS: replay a truncate or punch before a write made after it
#   352bd50e775 zinject: hold threads at named points in the code
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/dbuf.c module/zfs/zio_inject.c tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/zfs_rewrite_physical_append.ksh
#   b73d3a4622ae1dcd6e3048c6d8313bfddb2933be1133d9d70b43bab26978c4c3  module/zfs/dbuf.c
#   b20186240667c5b34e8d8ef25484d2c3756f9ebf6608e7d664957afb696150bb  module/zfs/zio_inject.c
#   b9b05fc9684015f592cc79e8b0cbeea2023eac54060c6d27cf2afeac42e8a2ce  .../zfs_rewrite_physical_append.ksh
grep -c 'dbuf_dirty_impl(db, tx, db->db_level == 0)' module/zfs/dbuf.c          # 1
grep -c 'ZIO_INJECT_POINT_REWRITE_CLEAN' module/zfs/dbuf.c module/zfs/zio_inject.c include/sys/zio.h   # 1, 1, 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-rwa
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q 4a98dfeb57bddb64160b6c39f111e2e46b971ea6
git -c user.name=t -c user.email=t@t cherry-pick f0a90e690f333a6b0517bc62bb7e2681cd1e81da
git rev-parse HEAD^{tree}          # ed1b85fea34ca6b54e6b3c61bdc13b4117fcf942
sha256sum module/zfs/dbuf.c        # b0a5fee5d168bbc5f97ae5d5a4cbf2373fd2075218b874f3a28f45f2fa18e87c
grep -c 'dbuf_dirty_impl' module/zfs/dbuf.c                                    # 0
git diff a1daebfec3f5ca46ac261ef339efdf0dbeae997c -- module/zfs/dbuf.c | grep -c '^+[^+]'   # 2 (the point)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-rwa; L=/var/log/task-1121-rwa
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Quote `cat /sys/module/zfs/parameters/zfs_txg_timeout` (5 expected) and `cat /sys/module/zfs/parameters/zfs_txg_history` (above 0 expected, 100 by default: the test reads the txgs kstat to prove that the append joined the txg of the held rewrite). Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-rwa; L=/var/log/task-1121-rwa; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zfs_rewrite_physical_append > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in well under a minute. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the lines with `zinject -c`, `wait`, `cmp` and `zfs send` (all `SUCCESS:`), and say whether any line contains `waited` or `quiesce` (none expected); `cat /sys/module/zfs/parameters/zfs_txg_timeout` after it (5: the test restores it); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-t zinject_args` once (tag zi), after the pre-run checks: `cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_args > $L/zts-zi.log 2>&1 < /dev/null &`. Expected PASS (its list of points now holds `dmu_buf_will_rewrite:clean`). Quote the result line and duration.
3. `-T zfs_rewrite` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zfs_rewrite > $L/zts-az.log 2>&1 < /dev/null &`. Expected: the nine tests of `common.run` PASS, `zfs_rewrite_physical_append` among them. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-rwa; L=/var/log/task-1121-rwa
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zfs_rewrite_physical_append > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected within a minute: FAIL. Quote the result line and duration, and from the test's own log: the `ERROR:` line (expected: `cmp /var/tmp/rewrite_append.expect /testpool/testfs2/file` with cmp's own line naming byte 196609 just before or after it), the `SUCCESS:` line of the first `cmp` (the one on `/testpool/testfs1/file`, the source), and say whether any line contains `waited` or `quiesce` (none expected: the failure must be the replica's cmp and nothing before it). Then: `cat /sys/module/zfs/parameters/zfs_txg_timeout` (5: the test restored it before the failing cmp, and its cleanup again); every line of `sudo -n dmesg | tail -n +$((N1+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than` (none expected); the pre-run checks once more (no pool, no leftover file). If the test PASSES instead: quote the test's whole own log and report it as the scenario not reproduced; do not rerun it.

Unload with `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc, 0 expected) and save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

If the module did not unload or `zpool list` hung in step 4: capture as in the VM rules into $L/hang-fb.txt, then the one pre-approved reboot (see the VM rules), and after it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.hung_task_panic` back to its step 0 value; `ls -ld /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/rewrite_append.* 2>&1` (quote; remove only those of them, with `sudo -n rm -rf` and their names, and report anything else); `/etc/zfs/zpool.cache` as in step 0. Otherwise no reboot.

Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`; `ls -l /etc/zfs/zpool.cache 2>&1` (absent or empty expected; above 0: report it and leave it).

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, `zfs_txg_timeout` after the run, any `waited`/`quiesce` line), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the `zinject_args` result; the group result (Results Summary, every non-PASS with its lines); srcversions, load lines, `zfs_txg_timeout` and `zfs_txg_history` at load. Part B: the result, the `ERROR:` line and cmp's byte line, the source cmp's `SUCCESS:` line, the dmesg check, the unload rc, whether a reboot was needed. One-line verdict: "RWA PASS 3/3 debug, zinject_args PASS, -T zfs_rewrite PASS; fail-before FAIL at the replica cmp (byte 196609) confirmed" or what differs.
