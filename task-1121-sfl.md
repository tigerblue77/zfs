TASK-1121-SFL (from the coordinator session "main"): functional, about 1 h builds included. A regression test for our fix "Free the MOS objects of the logs that zpool split leaves out" (fork branch `split-free-dropped-log-objects`, no PR yet, register item M-50): the pool that `zpool split` creates keeps, unreferenced, the metaslab array, the metaslab space maps and the unflushed-txgs object of every log vdev it leaves out, so that zdb reports them leaked and a space map refcount mismatch on it. (A) With the fix, debug build: the new ZTS test `zpool_split_log_objects` three times, then the group `-T zpool_split` once, then a check by hand of a pool with two logs. (B) Fail-before: master plus our NULL-deref fix and its test, plus only the new test commit, debug build: the new test must FAIL on zdb's leak lines, and the same check by hand must show them for both logs. Nothing is expected to panic: no reboot is planned. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-sfl`, `L=/var/log/task-1121-sfl`.

## Why

`zpool split` leaves the log vdevs out of the new pool: a log among the other top-level vdevs becomes a hole, and trailing logs are dropped. The new pool's MOS is a copy of the original pool's, so it still holds what each log owned there: its metaslab array (an `object array`), the space maps that array names (`SPA space map`, still counted by the `spacemap_histogram` feature) and the object of its metaslabs' unflushed txgs (`uint64`), which the log's top-level ZAP names. TASK-1121-SLG's check by hand saw exactly this on VM 107: `MOS object 136 (object array) leaked`, `MOS object 258 (SPA space map) leaked`, `MOS object 259 (uint64) leaked`, `space map refcount mismatch: expected 12 != actual 11`.

- fix (commit 3, `c5bcddb3b37`): `spa_vdev_split_mirror()` records the metaslab array of every log (with its number of entries) where it already records the DTL objects (6d4ff3b4f3a), and a sync task of the new pool frees each array and the space maps it names, with the code a log removal uses (`vdev_destroy_spacemaps()` is split into `vdev_destroy_ms_array()` and `vdev_destroy_ms_flush_data()`). The unflushed-txgs object is freed with the log's top-level ZAP, which the new pool destroys when it rebuilds its all-vdev ZAP in `spa_sync_config_object()`.
- test (commit 4, `089d8bca81a`): `zpool_split_log_objects` covers two layouts in one run: a log that is the last top-level vdev (`trailing`), and one followed by another mirror (`middle`, a hole in the new pool). Each time: files under `/var/tmp/zpool_split_log_objects`, `zpool create -O sync=always`, 4 MiB written, `zdb -m` must show that the log (vdev 1) has an `ms_unflushed_phys` object and at least one metaslab space map (it logs `Log unflushed txgs object, metaslab space maps: yes N`), `zpool split` (the new pool is exported), `zpool export` of the original, then `zdb -e -p <dir> -d` and `-MC` on both pools must exit 0 with no `leaked` or `refcount mismatch` line (it logs `zdb ...: no leak` for each of the four), then the new pool is imported with `-R` and the file's xxh128 compared.
- commits 1 and 2 are our NULL-deref fix and its test (TASK-1121-SLG, `c919a38b8e4`, `e98801c078c`): without them the `trailing` split crashes the kernel first, in `spa_ld_log_sm_cb()`. They are in both trees.

Expected with the fix: PASS in well under a minute. Without it (part B): the test FAILs at the `trailing` layout's first zdb, `zdb -e -p ... -d testpool2`, and its log shows the `MOS object ... (object array) leaked`, `(SPA space map) leaked` and `(uint64) leaked` lines; no kernel message.

What is tested: fork https://github.com/tigerblue77/zfs, branch `split-free-dropped-log-objects`, head `089d8bca81ae6736378dc12746aeeef9d883f42e`, tree `a1e95eded50341092cbd671170f69a6005ddceb3`, four commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus commits 1, 2 and 4 (the test) only: tree `bcdf110dfe7cc2787c360b2a102204fe096032d8`, whose `module/zfs/spa.c`, `module/zfs/vdev.c` and `include/sys/vdev.h` are those of `e98801c078c` (master's, which commits 1 and 2 do not touch).

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything (the checks by hand read their exported pools with `zdb -e` only); never `zfs-tests.sh -x`.
- Before every ZTS run and the checks by hand: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_log_objects* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. Capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved. A hang, an oops, a panic or a module that will not unload, in either part: capture as above, stop, report and wait for main.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_log_objects* 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1` (absent or empty expected; anything else: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`, then stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-sfl; L=/var/log/task-1121-sfl
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs split-free-dropped-log-objects && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~4) $(git status --porcelain | wc -l)"
#   089d8bca81ae6736378dc12746aeeef9d883f42e a1e95eded50341092cbd671170f69a6005ddceb3 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -5
#   089d8bca81a ZTS: check that zpool split leaves no MOS object of its logs
#   c5bcddb3b37 Free the MOS objects of the logs that zpool split leaves out
#   e98801c078c ZTS: split a pool whose log space maps name its trailing log
#   c919a38b8e4 Skip log space map entries for vdevs a split pool lacks
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c module/zfs/vdev.c include/sys/vdev.h tests/zfs-tests/tests/functional/cli_root/zpool_split/zpool_split_log_objects.ksh
#   0cc6821072fa1a2c62caa028ade07e14945d32db8b4ac21fd3d0ad00146108fd  module/zfs/spa.c
#   a16ce5010c8c1512b0bca0dcc8fee0fbd00120d94d2aec532ad251057a4583b4  module/zfs/vdev.c
#   c6557f8fb26aeede1847b449aa48550cd5ad3bf694e3469811cd0cb452ba0f37  include/sys/vdev.h
#   1179f39452298b83558cfcec17258d532b6c5e53b094ef45239fe571f8d2fb91  .../zpool_split_log_objects.ksh
grep -c 'vdev_destroy_ms_flush_data(spa->spa_meta_objset' module/zfs/spa.c   # 1
grep -c 'spa_split_log_free_sync' module/zfs/spa.c                           # 2
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-sfl
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q e98801c078c266d7d2f77b503be789013d7dfec2
git -c user.name=t -c user.email=t@t cherry-pick 089d8bca81ae6736378dc12746aeeef9d883f42e
git rev-parse HEAD^{tree}          # bcdf110dfe7cc2787c360b2a102204fe096032d8
sha256sum module/zfs/spa.c module/zfs/vdev.c include/sys/vdev.h
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c     (master's)
#   313e903ad7462b027bf868f950d76ac12abb896a8f7b2231646485724b8cfd8b  module/zfs/vdev.c    (master's)
#   9d3462449bd4be9310bf73ccaf534c18feebc050eced73f4f0bf3fd67ffa472d  include/sys/vdev.h   (master's)
grep -c 'vdev_destroy_ms_flush_data(spa->spa_meta_objset' module/zfs/spa.c   # 0
grep -c 'spa_split_log_free_sync' module/zfs/spa.c                           # 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-sfl; L=/var/log/task-1121-sfl
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-sfl; L=/var/log/task-1121-sfl; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_log_objects > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in well under a minute. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the two `Log unflushed txgs object, metaslab space maps: yes N` lines (N above 0 expected, one per layout) and the count of `no leak` lines (8 expected: four zdb runs per layout); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zpool_split` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_split > $L/zts-az.log 2>&1 < /dev/null &`. Expected: the ten tests of `common.run` PASS, `zpool_split_log_spacemap` and `zpool_split_log_objects` among them; `zpool_split_wholedisk` (`linux.run`) needs the `scsi_debug` module: if it cannot load it, record its result as is. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
3. A check by hand, not a test (about two minutes): a pool with two logs, one in the middle (a hole in the new pool) and one last (dropped), which the test does not combine. After the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-sfl; L=/var/log/task-1121-sfl; Z=$D/fix-dbg; M=$D/diag; t=fix
   mkdir -p $M/mnt && for f in d1 d2 l1 l2 e1 e2 m1 m2; do truncate -s 256M $M/$f; done
   sudo -n $Z/zpool create -o cachefile=none -O sync=always -O mountpoint=$M/mnt sfldiag mirror $M/d1 $M/d2 log mirror $M/l1 $M/l2; echo create-rc=$?
   sudo -n $Z/zpool add sfldiag mirror $M/e1 $M/e2 log mirror $M/m1 $M/m2; echo add-rc=$?
   sudo -n dd if=/dev/urandom of=$M/mnt/f bs=128k count=32 status=none; sudo -n $Z/zpool sync sfldiag
   sudo -n $Z/zpool split sfldiag sfldiag2; echo split-rc=$?
   sudo -n $Z/zpool export sfldiag; echo export-rc=$?
   sudo -n $Z/zdb -e -p $M -m sfldiag 2>&1 | grep -E '^\s+vdev ' > $L/diag-$t-vdevs.txt; cat $L/diag-$t-vdevs.txt
   for p in sfldiag2 sfldiag; do
       sudo -n $Z/zdb -e -p $M -d $p > $L/diag-$t-$p-d.txt 2>&1; echo $p zdb-d-rc=$?
       sudo -n $Z/zdb -e -p $M -MC $p > $L/diag-$t-$p-MC.txt 2>&1; echo $p zdb-MC-rc=$?
   done
   grep -E 'refcount mismatch|DTL:|unreferenced|smp_|leaked' $L/diag-$t-*-d.txt $L/diag-$t-*-MC.txt
   rm -rf $M; ls -d $M 2>&1
   ```

   Both pools stay exported, and their files go with `rm -rf $M`: neither is imported again. The vdev lines come from the original pool, which keeps its logs: each log's `ms_unflushed_phys object` there is also in the new pool's MOS. Expected with the fix: create, add, split and export rc 0; the vdev lines show `log` vdevs 1 and 3, each with `ms_unflushed_phys object ...`; all four zdb rc 0; the grep prints nothing. Quote the rc lines, the vdev lines and the grep output (the first 20 lines if longer).

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1.

1. The test ONCE, after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-sfl; L=/var/log/task-1121-sfl
   cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_log_objects > $L/zts-fb.log 2>&1 < /dev/null &
   ```

   Expected: FAIL, in well under a minute, during the `trailing` layout (`Log layout: middle` is not reached). Quote the result line and duration, and from the test's own log: the `Log unflushed txgs object, metaslab space maps` line, every line with `leaked`, `refcount mismatch` or `smp_`, and the `log_fail` line (`zdb -e -p ... -d testpool2 exited 2 or reported a leak` expected). Then the pre-run checks: the test's cleanup must leave no pool and no file (if one is left, quote it and stop).
2. The same check by hand as part A step 3, with `Z=$D/fb-dbg` and `t=fb`. Expected: create, add, split and export rc 0; on `sfldiag2`, `zdb -d` rc 2 with two `(object array) leaked`, at least two `(SPA space map) leaked` and two `(uint64) leaked` lines (one set per log), and `zdb -MC` rc 2 with `space map refcount mismatch: expected X != actual Y` where X - Y equals the number of leaked space maps; on `sfldiag` (the original pool, which keeps its logs), both zdb rc 0 with no such line. Quote the rc lines, the vdev lines and the grep output.

After them: dmesg since N1 as in part A (none expected); unload with `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc); save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl) '` empty; `ls -d /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/zpool_split_log_objects* 2>&1` (none expected); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the two N of `Log unflushed txgs object, metaslab space maps: yes N`, count of `no leak` lines), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); the check by hand (rc lines, vdev lines, grep output, your one-line reading); srcversions and load lines. Part B: the test's result line, duration, the leak lines and the log_fail line; the check by hand as in part A. One-line verdict: "SFL PASS 3/3 debug, -T zpool_split PASS, two-log split clean with the fix; fail-before: test FAIL on leaked log objects, two-log split leaks both logs' objects" or what differs.
