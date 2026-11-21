TASK-1121-SPB (from the coordinator session "main"): functional, about 1 h builds included. A regression test for our fix "zpool split: hold the new pool until done with it" (M-42, no PR yet), built on our zinject named-point series (N-01, fork branch `zinject-delay-points-v2`): `zpool split` drops the pool namespace lock in `spa_vdev_exit()` and then still logs a history record in the new pool and clears its `spa_is_splitting`, holding no reference on it, so a `zpool export` or `zpool destroy` of the new pool in that window frees the `spa_t` under the split. (A) With the fix, debug build: the new ZTS test `zpool_split_new_pool_busy` three times, `zinject_args` once, then the group `-T zpool_split` once. (B) Fail-before: N-01 plus only the test commit, debug build: the test must FAIL at its export check (the export succeeds), and once its cleanup releases the split, the split uses the freed pool: an oops is expected, which leaves the original pool busy and needs one reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-spb`, `L=/var/log/task-1121-spb`.

## Why

`spa_vdev_split_mirror()` adds the new pool to the namespace (`spa_add()`), loads it, commits the split on the original pool and calls `spa_vdev_exit()`, which drops `spa_namespace_lock`. After that it still calls `spa_history_log_internal(newspa, "split", ...)` and sets `newspa->spa_is_splitting = B_FALSE`, and without `-R` it exports the new pool by name. Nothing holds the new pool meanwhile (the ioctl holds only the original pool), so `spa_export_common()` of the new pool sees no reference, unloads it (`spa_unload()` sets `spa_dsl_pool` to NULL) and `spa_remove()` frees the `spa_t`. The fix takes `spa_open_ref(newspa)` while the namespace lock is still held and drops it with `spa_close()` before the final export; an export or destroy in the window then fails with EBUSY.

- fix (commit 5, `25129f6d749`): the local branch `zpool-split-hold-newspa` `7ef53553244` (built on master by the M-42 check), cherry-picked unchanged onto N-01.
- test (commit 6, `adf7ba0a425`): adds the zinject point `spa_vdev_split_mirror:committed` (on the original pool, right after `zio_handle_panic_injection(spa, FTAG, 3)`: the split is committed, the namespace and config locks are dropped, the history record is next) and `zpool_split_new_pool_busy`: a mirror of two 64 MiB files under `/var/tmp/zpool_split_new_pool_busy`, `zinject -w spa_vdev_split_mirror:committed -s 60 testpool`, `zpool split testpool testpool2` in the background, held at the point; then `zpool export testpool2` and `zpool destroy testpool2` must both fail with "pool is busy" (`log_mustnot_expect busy`); the test releases the split, which must succeed and export `testpool2`, imports `testpool2` and checks that `zpool history -i` holds `] split from pool testpool`.

Expected with the fix: PASS in well under a minute. Without the fix (part B): the test FAILs at `zpool export testpool2`, which succeeds (`ERROR: zpool export testpool2 unexpectedly exited 0`). Its cleanup then runs `zinject -c all`, the split resumes and reads `spa_get_dsl(newspa)->dp_mos_dir` from the freed `spa_t`: expected `BUG: kernel NULL pointer dereference, address: 0000000000000018` (the offset of `dp_mos_dir` in `dsl_pool_t`, as long as the freed memory still holds the NULL that `spa_unload()` wrote), `RIP: ... spa_history_log_internal+0x...`, `spa_vdev_split_mirror` and `zfs_ioc_vdev_split` in the trace, Comm `zpool`. If the memory was reused meanwhile, a `general protection fault` or another crash in the same function is just as much a confirmation; no oops at all is reported as such. The dead ioctl holds no lock but keeps its reference on `testpool`, so the cleanup's `zpool destroy -f testpool` is expected to fail with "pool is busy", and the runner to return with FAIL; the pool then stays until the reboot.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zpool-split-hold-newspa-v2` (main pushes it before this task starts), head `adf7ba0a425e8fda0cc1d5c5fcfdfd1e30ce5004`, tree `af4e968751189a2ead5f3f3e183c25c83511192a`: N-01's four commits (`4a98dfeb57b`, on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, 20/11), then the fix, then the test. The fail-before tree is N-01 plus the test commit only: tree `f01b087a004c42123058b30655d06d95f43df258`, whose `module/zfs/spa.c` is master's plus the one line of the point.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything by hand; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_new_pool_busy 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. In part A, capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: exactly one is pre-approved, in step 5, after the fail-before run. In part A, a hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main. Before the step 5 reboot, arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, and write in $L/progress.md where you are. Use `sync -f $L/progress.md` rather than a global `sync`, then the sysrq reboot through a transient unit, as in DBM4.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_new_pool_busy 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-spb; L=/var/log/task-1121-spb
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zpool-split-hold-newspa-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git rev-parse HEAD~6) $(git status --porcelain | wc -l)"
#   adf7ba0a425e8fda0cc1d5c5fcfdfd1e30ce5004 af4e968751189a2ead5f3f3e183c25c83511192a 4a98dfeb57bddb64160b6c39f111e2e46b971ea6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -7
#   adf7ba0a425 ZTS: export the new pool while zpool split still uses it
#   25129f6d749 zpool split: hold the new pool until done with it
#   4a98dfeb57b ZTS: free a dataset while its upgrade task returns
#   d074b306736 ZTS: leave a fresh vdev file after each upgrade test
#   af8df3d6ebe ZTS: replay a truncate or punch before a write made after it
#   352bd50e775 zinject: hold threads at named points in the code
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c module/zfs/zio_inject.c tests/zfs-tests/tests/functional/cli_root/zpool_split/zpool_split_new_pool_busy.ksh
#   dd052f8ade08d2b55e703cb18b14b89577bc05d00857abc9332d53573755269f  module/zfs/spa.c
#   5197a40c9c6cb1a4262bdfdb3ac9b137b7373466343b98a87dd7e94f95a09acf  module/zfs/zio_inject.c
#   d9287d0cfccd510c511df077487b8a2e0fa08236d8cb45a4df8e3a36b047f9fb  .../zpool_split_new_pool_busy.ksh
grep -c 'spa_open_ref(newspa, FTAG)\|spa_close(newspa, FTAG)' module/zfs/spa.c   # 2
grep -c 'ZIO_INJECT_POINT_SPLIT_COMMITTED' module/zfs/spa.c module/zfs/zio_inject.c include/sys/zio.h   # 1, 1, 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-spb
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q 4a98dfeb57bddb64160b6c39f111e2e46b971ea6
git -c user.name=t -c user.email=t@t cherry-pick adf7ba0a425e8fda0cc1d5c5fcfdfd1e30ce5004
git rev-parse HEAD^{tree}          # f01b087a004c42123058b30655d06d95f43df258
sha256sum module/zfs/spa.c         # cdadc26312b5f39d506360c45538f000eb03196ce05100b3999d125c8eb7a931
grep -c 'spa_open_ref(newspa' module/zfs/spa.c                                  # 0
git diff a1daebfec3f5ca46ac261ef339efdf0dbeae997c -- module/zfs/spa.c | grep -c '^+[^+]'   # 1 (the point)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-spb; L=/var/log/task-1121-spb
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-spb; L=/var/log/task-1121-spb; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_new_pool_busy > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in well under a minute. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the `SUCCESS:` lines of `zpool export testpool2` and `zpool destroy testpool2` with the `cannot export 'testpool2': pool is busy` and `cannot destroy 'testpool2': pool is busy` lines next to them, and the `SUCCESS:` lines of `wait`, `zpool import` and `zpool history`; and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-t zinject_args` once (tag zi), after the pre-run checks: `cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_args > $L/zts-zi.log 2>&1 < /dev/null &`. Expected PASS (its list of points now holds `spa_vdev_split_mirror:committed`). Quote the result line and duration.
3. `-T zpool_split` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_split > $L/zts-az.log 2>&1 < /dev/null &`. Expected: the nine tests of `common.run` PASS, `zpool_split_new_pool_busy` among them; `zpool_split_wholedisk` (`linux.run`) needs the `scsi_debug` module: if it cannot load it, record its result as is. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. If step 0 showed `kernel.panic_on_oops = 1` (VM 107 does), set it to 0 for this run only, `sudo -n sysctl kernel.panic_on_oops=0` (the reboot restores it; say so in the report), so that the oops is printed and the VM stays up, as in DBM3. Start a capture flushed line by line that survives your commands: `L=/var/log/task-1121-spb; setsid nohup sudo -n sh -c "dmesg --follow > $L/oops-follow.txt" > /dev/null 2>&1 < /dev/null &`. Then `touch $L/fb-start`, write $L/progress.md ("step 4 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-spb; L=/var/log/task-1121-spb
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_new_pool_busy > $L/zts-fb.log 2>&1 < /dev/null &
```

Poll dmesg every 10 s for at most 5 minutes. Expected within a minute: the oops. Quote the `BUG:` (or `general protection fault`) line, the `RIP:` line, the line with `Comm:`, and the first 15 frames of the Call Trace. Then wait 2 more minutes, and capture into $L/fb-state.txt: `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|zfs-tests|ksh'`, the kernel stacks of every D task, `timeout 30 sudo -n $D/fb-dbg/zpool list -H -o name,health 2>&1` (testpool expected, testpool2 not), `sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg` and `sudo -n dmesg | tail -n 200`; save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`. From the test's own log quote the `ERROR:` line (expected: `zpool export testpool2 unexpectedly exited 0`) and the lines of its cleanup (`zinject -c all`, `zpool destroy -f testpool`: "pool is busy" expected). Quote any D task (none expected: the dead ioctl holds no lock), and whether the runner returned (expected, with FAIL; quote its result line). No oops within 5 minutes: capture the same, wait for the runner (at most 15 minutes in all) and report its result, the test's own log and whether `zpool split` printed an error: the export result is still the verdict of the test.

## STEP 5: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `find /var/tmp -mindepth 1 -maxdepth 1 -newer $L/fb-start -printf '%M %u %s %TT %p\n'` (quote). The failed cleanup cannot finish, so these are expected among them: `zpool_split_new_pool_busy` (the test's two file vdevs), `file-vdev0` to `file-vdev2`, and possibly `testdir` and `constrained_path.*`. Remove exactly those of them that the listing shows (`sudo -n rm -rf` with their names); anything else it lists is reported and left, `/var/tmp/test_results` stays. `/etc/zfs/zpool.cache` may be above 0 (the run's `testpool`): quote its `ls -l` and the `strings` check of step 0; if it names only `testpool...` with paths under `/var/tmp`, move it to `$L/zpool.cache.fb`; otherwise report it and leave it. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the two "pool is busy" lines seen yes/no, history check SUCCESS yes/no), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the `zinject_args` result; the group result (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: the test's `ERROR:` line, the oops (BUG or GPF line, RIP, Comm, first 15 frames), the cleanup lines, the D tasks, `zpool list` after it, whether the runner returned, the reboot (time, how you resumed), what was removed and where the cache file went. One-line verdict: "SPB PASS 3/3 debug, zinject_args PASS, -T zpool_split PASS; fail-before export succeeded and the split oopsed in spa_history_log_internal" or what differs.
