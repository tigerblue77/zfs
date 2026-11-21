TASK-1121-SPM (from the coordinator session "main"): functional, about 100 min builds included. Our fix for a hot spare that was missing at import not being a hot spare any more (fork branch `spare-register-missing-at-import`, no PR yet): a spare that cannot be opened when the pool is imported is left unregistered, so once it is back, `zpool replace <pool> <dev> <spare>`, or the replacement zed issues for a faulted device, attaches it as a plain replacement: the replaced device is detached after the resilver and the spare stays in the pool for good, still listed as a spare. (A) With the fix, debug build: the new ZTS test `import_spare_missing` three times, then the group `-T zpool_import` once, then the group `-T fault` once if no zed is running and scsi_debug is available. (B) Fail-before: master plus only the test commit, debug build: the test once; it is expected to FAIL within a minute, not to hang. No reboot is expected or pre-approved. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-spm`, `L=/var/log/task-1121-spm`.

## Why

Read on master `a1daebfec3f`; no report of it upstream.

- `spa_load_spares()` registers a spare with `spa_spare_add()` only if `vdev_open()` succeeded (module/zfs/spa.c:2583-2587), while `spa_load_l2cache()` registers a cache device "even if it fails to open" (2685-2689). Nothing registers the spare later: `vdev_reopen()` and `vdev_online()` do not call `spa_spare_add()`.
- An unregistered spare has `vdev_isspare` unset, so the config has no `is_spare` for it (module/zfs/vdev_label.c:546-547) and zed does not pass `ZFS_ONLINE_SPARE` when its disk returns (cmd/zed/agents/zfs_mod.c:239, 330-331; libzfs then refuses, lib/libzfs/libzfs_pool.c:3618-3619). When a device is replaced with it, `vdev_label_init()` marks the new vdev as a spare only if `spa_spare_exists()` (module/zfs/vdev_label.c:1370-1373), so `spa_vdev_attach()` creates a `replacing` vdev instead of a `spare` vdev (module/zfs/spa.c:8722-8725).
- A user-space program linked with libzpool showed it: with the spare missing at import, then back, replacing the first side of a mirror with it gave `replacing`, and after the resilver the first side was gone and the spare a plain member of the mirror, still an available spare; replacing the second side with the same spare was then accepted, so the same file was in the mirror twice. With the spare present at import: `spare`, and the second replacement refused with EBUSY.

- fix (commit 1, `1f2d180ff24`): `spa_load_spares()` registers a spare that cannot be opened too; a spare that opens but whose label does not match is still left out.
- test (commit 2, `65f3f6f7ea8`): `import_spare_missing` (group `cli_root/zpool_import`) creates `testpool1` as a mirror of the group's `disk0` and `disk1` with `disk2` as a spare, exports it, moves `disk2` out of the device directory, imports with `zpool import -d <device directory>` (the spare is UNAVAIL), moves `disk2` back, runs `zpool replace testpool1 <disk0> <disk2>` and `zpool wait -t resilver testpool1`, then checks that `spare-0` is in `zpool status`, that `disk0` is still ONLINE in the pool, and that the spare is INUSE.

Expected with the fix: PASS each time, in about 10 to 30 seconds. Without the fix (part B): FAIL right after the resilver, with `The spare did not go in as a hot spare.`, after a `zpool status -v` that shows no `spare-0`, `disk2` in the mirror (or under `replacing-0`), and `disk2` UNAVAIL under `spares`. Deterministic.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spare-register-missing-at-import`, head `65f3f6f7ea8e4bb366078bdb900864ec1baceef8`, tree `b36398b0c6f042ce84f6ffb024d5cfa80890ba8c`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `0f814ad9ecba38068f646543760b18e7311c4908`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs and, for `fault`, its own scsi_debug disk; touch no other block device of the VM and no pool this task did not create; never `zpool import` anything by hand; never `zfs-tests.sh -x`; never pass `-d` or set `DISKS`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir /var/tmp/dev_import-test /var/tmp/bakdev_import-test 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, `lsmod | grep -E '^scsi_debug '` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file, a loaded scsi_debug or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l` or `lsmod` line, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 10 minutes; the `zpool_import` group after 60 minutes; the `fault` group after 90 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|zed'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl|scsi_debug) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id zts`, `command -v lsscsi` and `modinfo -F filename scsi_debug` (part A3 only), `pgrep -a -x zed` (quote it; do not stop it; a running zed skips part A3, whose setup refuses to start a second one), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir /var/tmp/dev_import-test /var/tmp/bakdev_import-test 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-spm; L=/var/log/task-1121-spm
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs spare-register-missing-at-import && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   65f3f6f7ea8e4bb366078bdb900864ec1baceef8 b36398b0c6f042ce84f6ffb024d5cfa80890ba8c a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   65f3f6f7ea8 ZTS: Use a hot spare that was missing at import
#   1f2d180ff24 Register a hot spare that was missing at import
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c tests/zfs-tests/tests/functional/cli_root/zpool_import/import_spare_missing.ksh tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am
#   a49367311ac0745600cbe6913d47922407d82667bb4cc94e1be4dcf3aa931ebe  module/zfs/spa.c
#   e889742fa83acdb70b96340594031cd2030cb765af70ae7923d0655046569533  .../zpool_import/import_spare_missing.ksh
#   34a04461efa38e9a7ddd0f3e706415affb42ff68ca1ab03fd50697b7b1c0f7a1  tests/runfiles/common.run
#   8381f0ba2e5b543fd5faa7ccefacfb0292c41643a7f786b49ccb21bcd707fb00  tests/zfs-tests/tests/Makefile.am
grep -c 'if (vdev_open(vd, CRED()) != 0 || vdev_validate_aux(vd) == 0)' module/zfs/spa.c   # 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-spm
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick 65f3f6f7ea8e4bb366078bdb900864ec1baceef8
git rev-parse HEAD^{tree}          # 0f814ad9ecba38068f646543760b18e7311c4908
sha256sum module/zfs/spa.c tests/zfs-tests/tests/functional/cli_root/zpool_import/import_spare_missing.ksh
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c  (master's)
#   e889742fa83acdb70b96340594031cd2030cb765af70ae7923d0655046569533  .../zpool_import/import_spare_missing.ksh
grep -c 'vdev_open(vd, CRED()) != 0 ||' module/zfs/spa.c   # 0 (1 in fix-dbg)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-spm; L=/var/log/task-1121-spm
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and the kbuild "compiler differs" note (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks (tags ac-1, ac-2, ac-3):

   ```sh
   D=/var/tmp/task-1121-spm; L=/var/log/task-1121-spm; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t import_spare_missing > $L/zts-ac-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time. For each run quote: the result lines (the group's `setup` and `cleanup` included) and the test's duration; from the test's own log under the results directory the runner prints, every line with `zpool create`, `zpool import`, `zpool replace`, `zpool wait`, `spare-0`, `INUSE`, `UNAVAIL` or `did not go in` (expected: `zpool import -d ... testpool1` SUCCESS, `zpool replace testpool1 ...disk0 ...disk2` SUCCESS, `zpool wait -t resilver testpool1` SUCCESS, no "did not go in", no log_fail); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zpool_import` once (tag ai), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_import > $L/zts-ai.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/cli_root/zpool_import]` in `common.run` and of `[tests/functional/cli_root/zpool_import:Linux]` in `linux.run` PASS or SKIP as `zts-report.py.in` expects, `import_spare_missing` included. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known. Afterwards `lsmod | grep -E '^scsi_debug '` must be empty (`zpool_import_aux_paths` unloads it); if not, quote it and stop.
3. `-T fault` once (tag af), after the pre-run checks, only if step 0 found no running zed, `lsscsi` and the scsi_debug module (otherwise say which and skip): `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T fault > $L/zts-af.log 2>&1 < /dev/null &`. Its `auto_spare_*` tests have zed replace faulted devices with spares that were present at import, so they check that spares still go in as before. Expected: every test of `[tests/functional/fault:Linux]` in `linux.run` PASS or as `zts-report.py.in` expects. Quote as in 2. Afterwards `pgrep -a -x zed` must be empty (the group's cleanup stops its zed) and `lsmod | grep -E '^scsi_debug '` empty; if not, quote them and stop.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks (tag fb):

```sh
D=/var/tmp/task-1121-spm; L=/var/log/task-1121-spm
cd $D/fb-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t import_spare_missing > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected: FAIL within a minute; in the test's own log, `zpool import -d ... testpool1` SUCCESS, `zpool replace ...` SUCCESS, `zpool wait -t resilver testpool1` SUCCESS, then a `zpool status -v testpool1` with no `spare-0` and `disk2` under `spares` UNAVAIL, then `The spare did not go in as a hot spare.`. Quote: the result line and duration; the lines of the test's own log from the `zpool export` to the end, the `zpool status` output included; dmesg since N1 for the same patterns as in step 3 (none expected). A PASS here means the fail-before was not reproduced: quote the whole test log, then run it once more the same way (tag fb2); two PASSes: report it plainly. A hang (the test after 10 minutes): capture as in the VM rules, stop and report.

Then the pre-run checks once more (the test's cleanup must have destroyed `testpool1` even after the FAIL, and the group's cleanup removed its directories), `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc), `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl|scsi_debug) '` empty; `losetup -a` (quote); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir /var/tmp/dev_import-test /var/tmp/bakdev_import-test 2>&1` (none); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. `/var/tmp/test_results` and $L stay.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the import/replace/wait lines as seen in the test log), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the two group results (Results Summary, every non-PASS with its lines, or why `fault` was skipped); srcversions and load lines. Part B: result and duration, the test log lines from the `zpool export` on with the `zpool status` output, dmesg, and whether the cleanup left anything behind. One-line verdict: "SPM PASS 3/3 debug (spare missing at import goes in as a hot spare once back, replaced device kept, spare INUSE), -T zpool_import PASS, -T fault PASS or skipped (why); fail-before: FAIL 'The spare did not go in as a hot spare.': confirmed" or what differs.
