TASK-1121-PRB (from the coordinator session "main"): functional, about 90 min builds included. Our fix for a cache device whose probe failed never being faulted (fork branch `probe-fault-cache-devices`, no PR yet): when an I/O to a cache device fails and the probe that follows fails too, the kernel wants the device faulted, but its async fault walk only covers the vdev tree, so the cache device stays ONLINE while every L2ARC write to it fails, although a data device in the same case is FAULTED. (A) With the fix, debug build: the new ZTS test `l2arc_probe_fault_pos` three times, then the group `-T l2arc` once, then the test `zinject_probe` once. (B) Fail-before: master plus only the test commit, debug build: the test once; it is expected to FAIL after its 60-second wait, not to hang. No reboot is expected or pre-approved. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-prb`, `L=/var/log/task-1121-prb`.

## Why

Read on master `a1daebfec3f`; no report of it upstream.

- When a leaf I/O fails unexpectedly, `zio_vdev_io_done()` probes the device (module/zfs/zio.c:5374-5375). If the probe fails, `vdev_probe_done()` sets `vdev_fault_wanted` and requests `SPA_ASYNC_FAULT_VDEV` (module/zfs/vdev.c:1897-1917). `spa_async_thread()` then calls `spa_async_fault_vdev()` on the root vdev only (module/zfs/spa.c:10317-10324), while the removal task just above it also walks `spa_l2cache` and `spa_spares` (10293-10305). Cache devices live in `spa_l2cache`, so a cache device whose probe failed stays ONLINE with `vdev_cant_read`/`vdev_cant_write` set: each L2ARC write to it fails at once (module/zfs/zio.c:5292-5295) and is counted as a write error (6214-6233), and the feed thread, which only checks `vdev_is_dead()` (module/zfs/arc.c:10269), keeps trying. On Linux zed does not diagnose probe failures (cmd/zed/agents/zfs_diagnosis.c:1027-1033) and leaves the fault to the kernel.
- A user-space program linked with libzpool showed it: with write and probe errors injected on the cache device, it stayed HEALTHY with `vdev_fault_wanted` set and its write error count growing by about 190 a second, while the second side of the mirror, injected the same way, was FAULTED at once.

- fix (commit 1, `a15cc8dca47`): after the root vdev, `spa_async_thread()` calls `spa_async_fault_vdev()` on each cache device, as the removal task does.
- test (commit 2, `dad16715d6e`): `l2arc_probe_fault_pos` (group `functional/l2arc`, needs fio) creates `testpool` on the group's 1 GiB file with a 280 MiB file cache device, writes 200 MiB with fio, random reads it and waits for `arcstats.l2_write_bytes` to grow, then runs `zinject -d <cache file> -e io -T write` and `-T probe`, rewrites and random reads the files, and waits at most 60 seconds for the cache device to be FAULTED. Then it clears the injection, runs `zpool clear testpool <cache file>` and checks that the device is ONLINE.

Expected with the fix: PASS each time, in about 40 to 90 seconds; the cache device FAULTED within about 5 seconds of the failed write (the fault is applied at the next txg). Without the fix (part B): FAIL after the 60-second wait, with `The cache device is 'ONLINE' 60 seconds after its probe failed, expected 'FAULTED'.`, after a `zpool status -v` that shows the cache device ONLINE with a WRITE count above 0. Deterministic.

What is tested: fork https://github.com/tigerblue77/zfs, branch `probe-fault-cache-devices`, head `dad16715d6eb4c44c6d0ec703bd31b31ad9ed81b`, tree `7d857d7994703a1560777f9669becc3207fd0bc6`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `e330ecfe27909ab1a7c70e884b1a633ba7a4817a`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything by hand; never `zfs-tests.sh -x`; never pass `-d` or set `DISKS`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, `timeout 30 sudo -n <tree>/zinject` lists no handler, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file, a leftover injection handler or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l` or output line, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 10 minutes; the `l2arc` group after 60 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|l2arc|zinject'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id zts`, `command -v fio` (fio is required: without it the test is reported UNSUPPORTED, so stop and report), `pgrep -a -x zed` (quote it; do not stop it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-prb; L=/var/log/task-1121-prb
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs probe-fault-cache-devices && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   dad16715d6eb4c44c6d0ec703bd31b31ad9ed81b 7d857d7994703a1560777f9669becc3207fd0bc6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   dad16715d6e ZTS: Fault a cache device whose probe failed
#   a15cc8dca47 Fault a cache device whose probe failed
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c tests/zfs-tests/tests/functional/l2arc/l2arc_probe_fault_pos.ksh tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am
#   784e74fc0c3e707776c3deadc53bdba84c898d2586a9bd8888e411c4adf8a5d6  module/zfs/spa.c
#   2213574c9d6a1edb4e5379dcb31fa35da3661fec170c13de0067e14ced337e7b  .../l2arc/l2arc_probe_fault_pos.ksh
#   82cbd13c4298a48df9677159c6ff71956edc6cb0566a724d1f0c9d529c921941  tests/runfiles/common.run
#   3053923bcf25dec32afe8633d033e467c9359d5aaa0eaeb4bfba36d11ad1c20f  tests/zfs-tests/tests/Makefile.am
grep -c 'spa_async_fault_vdev(spa->spa_l2cache.sav_vdevs\[i\],' module/zfs/spa.c   # 1
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-prb
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick dad16715d6eb4c44c6d0ec703bd31b31ad9ed81b
git rev-parse HEAD^{tree}          # e330ecfe27909ab1a7c70e884b1a633ba7a4817a
sha256sum module/zfs/spa.c tests/zfs-tests/tests/functional/l2arc/l2arc_probe_fault_pos.ksh
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c  (master's)
#   2213574c9d6a1edb4e5379dcb31fa35da3661fec170c13de0067e14ced337e7b  .../l2arc/l2arc_probe_fault_pos.ksh
grep -c 'spa_async_fault_vdev(spa->spa_l2cache' module/zfs/spa.c   # 0 (1 in fix-dbg)
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-prb; L=/var/log/task-1121-prb
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes and the kbuild "compiler differs" note (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks (tags ac-1, ac-2, ac-3):

   ```sh
   D=/var/tmp/task-1121-prb; L=/var/log/task-1121-prb; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_probe_fault_pos > $L/zts-ac-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time. For each run quote: the result lines (the group's `setup` and `cleanup` included) and the test's duration; from the test's own log under the results directory the runner prints, every line with `zinject`, `fio`, `zpool clear`, `FAULTED`, `ONLINE` or `Nothing was written` (expected: the two `zinject -d ... testpool` SUCCESS, the four fio runs SUCCESS, `zinject -c all` and `zpool clear testpool ...` SUCCESS, no "Nothing was written", no log_fail); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T l2arc` once (tag al), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T l2arc > $L/zts-al.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/l2arc]` in `common.run` PASS, `l2arc_probe_fault_pos` included. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.
3. `-t zinject_probe` once (tag zp), after the pre-run checks: `cd $D/fix-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_probe > $L/zts-zp.log 2>&1 < /dev/null &`. It faults a data device the same way, so it checks that the tree is still walked as before. Expected PASS; quote the result lines and duration.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Run the test ONCE, after the pre-run checks (tag fb):

```sh
D=/var/tmp/task-1121-prb; L=/var/log/task-1121-prb
cd $D/fb-dbg && setsid nohup timeout 600 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_probe_fault_pos > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected: FAIL after about 60 seconds of waiting; in the test's own log, the two `zinject -d ... testpool` SUCCESS, the fio runs SUCCESS, then a `zpool status -v testpool` in which the cache device is ONLINE with a WRITE count above 0, then `The cache device is 'ONLINE' 60 seconds after its probe failed, expected 'FAULTED'.`. Quote: the result line and duration; the lines of the test's own log from the second `zinject` to the end, the `zpool status` output included; dmesg since N1 for the same patterns as in step 3 (none expected). A PASS here means the fail-before was not reproduced: quote the whole test log, then run it once more the same way (tag fb2); two PASSes: report it plainly. A hang (the test after 10 minutes): capture as in the VM rules, stop and report.

Then the pre-run checks once more (the test's cleanup must have cleared the injection and destroyed `testpool` even after the FAIL, and the group's cleanup removed `/var/tmp/testdir`), `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc), `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl) '` empty; `losetup -a` (quote); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/testdir 2>&1` (none); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. `/var/tmp/test_results` and $L stay.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the zinject/fio/clear lines as seen in the test log), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the `l2arc` group result (Results Summary, every non-PASS with its lines) and the `zinject_probe` result; srcversions and load lines. Part B: result and duration, the test log lines from the second `zinject` on with the `zpool status` output, dmesg, and whether the cleanup left anything behind. One-line verdict: "PRB PASS 3/3 debug (cache device FAULTED after its failed probe, ONLINE after zpool clear), -T l2arc PASS, zinject_probe PASS; fail-before: FAIL 'The cache device is 'ONLINE' 60 seconds after its probe failed, expected 'FAULTED'.': confirmed" or what differs.
