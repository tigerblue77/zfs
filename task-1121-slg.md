TASK-1121-SLG (from the coordinator session "main"): functional, about 1 h builds included. A regression test for our fix "Skip log space map entries for vdevs a split pool lacks" (fork branch `split-log-sm-missing-vdev`, no PR yet): `zpool split` of a pool whose last top-level vdev is a log, while the log space maps hold entries for that log, makes the load of the new pool dereference NULL. (A) With the fix, debug build: the new ZTS test `zpool_split_log_spacemap` three times, then the group `-T zpool_split` once, then a two-minute check by hand of a split pool with zdb. (B) Fail-before: master plus only the test commit, debug build: the split must oops in the kernel, which leaves ZFS wedged and needs one reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-slg`, `L=/var/log/task-1121-slg`.

## Why

`zpool split` leaves the log vdevs out of the new pool. A log that is the last top-level vdev is dropped altogether, so the new pool's vdev tree ends before the log's vdev id. The new pool inherits the MOS of the original pool, log space maps included, and their entries name the vdev by id: `spa_ld_log_sm_cb()` looks each entry's vdev up with `vdev_lookup_top()`, which returns NULL past the end of the tree, and hands it to `vdev_is_concrete()`. One ZIL block allocated or freed on the log while its log space map still exists is enough, and `zpool split` itself frees the ZIL chain on the log (`spa_reset_logs()`) right before it loads the new pool. ztest -m 3 hit this in user space (crash 2 of the M-49 sweep); this task checks the kernel.

- fix (commit 1, `c919a38b8e4`): `spa_ld_log_sm_cb()` and four zdb callbacks skip an entry whose vdev is NULL, as they already did for a hole or an indirect vdev.
- test (commit 2, `e98801c078c`): `zpool_split_log_spacemap` creates `mirror data1 data2 log mirror log1 log2` (the log is vdev 1, the last one) under `/var/tmp/zpool_split_log_spacemap` with `sync=always`, writes 4 MiB, checks with `zdb -mmmm` that the log space maps hold entries for `vdev: 1` (it logs `The log space maps hold N entries for the log`), sets `zfs_keep_log_spacemaps_at_export=1` (restored at the end), runs `zpool split` (the new pool is exported), imports the new pool with `-R`, compares the file's xxh128 and scrubs the new pool.

Expected with the fix: PASS in well under a minute, N above 0. Without the fix (part B): `zpool split` dies and dmesg shows `BUG: kernel NULL pointer dereference` (a small address, the offset of `vdev_ops` in `vdev_t`), `RIP: ... vdev_is_concrete+0x...` and, in the trace, `spa_ld_log_sm_cb`, `space_map_iterate`, `spa_vdev_split_mirror` and `zfs_ioc_vdev_split`, Comm `zpool`. The dead ioctl keeps the pool namespace lock, so the test's cleanup (`zpool list`, `zpool destroy`) then blocks in D state: the runner is not expected to return, and you do not wait for it.

What is tested: fork https://github.com/tigerblue77/zfs, branch `split-log-sm-missing-vdev`, head `e98801c078c266d7d2f77b503be789013d7dfec2`, tree `3f9145d24823b8473f3c05ce232e4de071894c59`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `b1e0a86472fd54affc0ef5f0272b8458c300e374`, whose `module/zfs/spa_log_spacemap.c` and `cmd/zdb/zdb.c` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_log_spacemap 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. In part A, capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: exactly one is pre-approved, in step 5, after the fail-before run. In part A, a hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main. Before the step 5 reboot, arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, and write in $L/progress.md where you are. Use `sync -f $L/progress.md` rather than a global `sync`, then the sysrq reboot through a transient unit, as in DBM4.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_log_spacemap 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent is expected. TASK-1121-DBM4 left one of 2364 bytes on VM 107, from its panicked fail-before run. If it is there: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it is empty, or names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-slg; L=/var/log/task-1121-slg
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs split-log-sm-missing-vdev && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   e98801c078c266d7d2f77b503be789013d7dfec2 3f9145d24823b8473f3c05ce232e4de071894c59 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   e98801c078c ZTS: split a pool whose log space maps name its trailing log
#   c919a38b8e4 Skip log space map entries for vdevs a split pool lacks
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa_log_spacemap.c cmd/zdb/zdb.c tests/zfs-tests/tests/functional/cli_root/zpool_split/zpool_split_log_spacemap.ksh
#   fc15cb0e49c71eeedf57e7ae9d8d402217369d26e327338023e2bd717628a8ef  module/zfs/spa_log_spacemap.c
#   524fd4ab161231f338a7ce9fe11b92b9b9cc531bf91971d1e9bcb152f72d7ae9  cmd/zdb/zdb.c
#   f8ffff8d016b09de08d4dadcd9d04a4cf605e87dcbdf10a034631df0cd3c3fef  .../zpool_split_log_spacemap.ksh
grep -c 'vd == NULL || !vdev_is_concrete' module/zfs/spa_log_spacemap.c cmd/zdb/zdb.c   # 1 and 4
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-slg
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick e98801c078c266d7d2f77b503be789013d7dfec2
git rev-parse HEAD^{tree}          # b1e0a86472fd54affc0ef5f0272b8458c300e374
sha256sum module/zfs/spa_log_spacemap.c cmd/zdb/zdb.c
#   f1e9be445949ebf99dbc334e9cde621f194aace4de711a04f42936bc2613ac80  module/zfs/spa_log_spacemap.c   (master's)
#   6e24b12886705dbde777c0a0602b02a279117a32c3805e44ec45df8edad00fb3  cmd/zdb/zdb.c                    (master's)
grep -c 'vd == NULL || !vdev_is_concrete' module/zfs/spa_log_spacemap.c cmd/zdb/zdb.c   # 0 and 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-slg; L=/var/log/task-1121-slg
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Quote `cat /sys/module/zfs/parameters/zfs_keep_log_spacemaps_at_export` (0 expected, the default). Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-slg; L=/var/log/task-1121-slg; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_log_spacemap > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in well under a minute. For each run quote: the result line and duration; the line `The log space maps hold N entries for the log` from the test's own log under the results directory the runner prints (N above 0 expected); the lines of `sudo -n grep -E 'spa_load\(testpool2.*Read [0-9]+ log space maps' /proc/spl/kstat/zfs/dbgmsg` (zfs-tests.sh clears the debug log when it starts, so they belong to this run: two are expected, the load in the split, which must read at least one log space map, then the import, which may read none; if there is no such line, say so and quote `cat /sys/module/zfs/parameters/zfs_dbgmsg_enable`); `cat /sys/module/zfs/parameters/zfs_keep_log_spacemaps_at_export` after it (0: the test restores it); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zpool_split` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_split > $L/zts-az.log 2>&1 < /dev/null &`. Expected: the nine tests of `common.run` PASS, `zpool_split_log_spacemap` among them; `zpool_split_wholedisk` (`linux.run`) needs the `scsi_debug` module: if it cannot load it, record its result as is. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
3. A check by hand, not a test (about two minutes): my reading of the code is that a split pool keeps, unreferenced, the metaslab space maps of the log it left out, so that zdb reports a space map refcount mismatch on it. This is why the test runs no zdb on the new pool. After the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-slg; L=/var/log/task-1121-slg; Z=$D/fix-dbg; M=$D/diag
   mkdir -p $M/mnt && for f in d1 d2 l1 l2; do truncate -s 256M $M/$f; done
   sudo -n $Z/zpool create -o cachefile=none -O sync=always -O mountpoint=$M/mnt slgdiag mirror $M/d1 $M/d2 log mirror $M/l1 $M/l2; echo create-rc=$?
   sudo -n dd if=/dev/urandom of=$M/mnt/f bs=128k count=32 status=none; sudo -n $Z/zpool sync slgdiag
   sudo -n $Z/zpool split slgdiag slgdiag2; echo split-rc=$?
   sudo -n $Z/zdb -e -p $M -MC slgdiag2 > $L/diag-zdb-MC.txt 2>&1; echo zdb-MC-rc=$?
   sudo -n $Z/zdb -e -p $M -d slgdiag2 > $L/diag-zdb-d.txt 2>&1; echo zdb-d-rc=$?
   grep -E 'refcount mismatch|DTL:|unreferenced|leaked' $L/diag-zdb-MC.txt $L/diag-zdb-d.txt
   sudo -n $Z/zpool destroy slgdiag; echo destroy-rc=$?
   rm -rf $M; ls -d $M 2>&1
   ```

   `slgdiag2` stays exported, and its files go with `rm -rf $M`: it is not imported. Expected by that reading: create, split and destroy rc 0; `zdb -MC` rc 2 with `space map refcount mismatch: expected X != actual Y` and its detail lines; `zdb -d` listing `MOS object ... (space map) leaked` lines and probably an `(object array)` one. Quote the rc lines and the grep output (the first 20 lines if longer). If both zdb runs exit 0 without such lines, say so plainly: the reading is wrong. A crash or an assertion of zdb: quote its last 20 lines.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. If step 0 showed `kernel.panic_on_oops = 1` (VM 107 does), set it to 0 for this run only, `sudo -n sysctl kernel.panic_on_oops=0` (the reboot restores it; say so in the report), so that the oops is printed and the VM stays up, as in DBM3. Start a capture flushed line by line that survives your commands: `L=/var/log/task-1121-slg; setsid nohup sudo -n sh -c "dmesg --follow > $L/oops-follow.txt" > /dev/null 2>&1 < /dev/null &`. Then `touch $L/fb-start`, write $L/progress.md ("step 4 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-slg; L=/var/log/task-1121-slg
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_log_spacemap > $L/zts-fb.log 2>&1 < /dev/null &
```

Poll dmesg every 10 s for at most 5 minutes. Expected within a minute: the oops. Quote the `BUG:` line, the `RIP:` line, the line with `Comm:`, and the first 15 frames of the Call Trace. Then wait 2 more minutes, and capture into $L/fb-state.txt: `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|zfs-tests|ksh'`, the kernel stacks of every D task, `sudo -n tail -n 80 /proc/spl/kstat/zfs/dbgmsg` (its last `spa_load(testpool2, ...)` lines show where the load of the new pool stopped) and `sudo -n dmesg | tail -n 200`; save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`. Quote the D tasks (command and wait channel; `zpool` commands of the cleanup are expected there) and the last `spa_load(testpool2` line. Do not wait for the runner; if it has returned, quote its result line. No oops within 5 minutes: capture the same, wait for the runner (at most 15 minutes in all) and report its result and the test's own log: the scenario did not occur.

## STEP 5: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `find /var/tmp -mindepth 1 -maxdepth 1 -newer $L/fb-start -printf '%M %u %s %TT %p\n'` (quote). The hung run cannot clean up, so these are expected among them: `tunable-KEEP_LOG_SPACEMAPS_AT_EXPORT`, `zpool_split_log_spacemap` (the test's four file vdevs), `file-vdev0` to `file-vdev2`, and possibly `testdir` and `constrained_path.*`. Remove exactly those of them that the listing shows (`sudo -n rm -rf` with their names); anything else it lists is reported and left, `/var/tmp/test_results` stays. `/etc/zfs/zpool.cache` is expected above 0 (the run's `testpool`): quote its `ls -l` and the `strings` check of step 0; if it names only `testpool...` with paths under `/var/tmp`, move it to `$L/zpool.cache.fb`; otherwise report it and leave it. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, N of `The log space maps hold N entries`, `Read K log space maps` for the split and for the import, `zfs_keep_log_spacemaps_at_export` after the run), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); the check by hand (rc lines, grep output, your one-line reading); srcversions and load lines. Part B: the oops (BUG line, RIP, Comm, first 15 frames), the last `spa_load(testpool2` line, the D tasks, whether the runner returned, the reboot (time, how you resumed), what was removed and where the cache file went. One-line verdict: "SLG PASS 3/3 debug, -T zpool_split PASS; fail-before oops in vdev_is_concrete via spa_ld_log_sm_cb confirmed; split pool zdb refcount mismatch: yes/no" or what differs.
