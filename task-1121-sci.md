TASK-1121-SCI (from the coordinator session "main"): functional, about 1 h builds included. A regression test for our fix "Check the split config entry of an indirect vdev" (fork branch `split-check-indirect-config`, no PR yet): `zpool split` builds the new pool's config from the config it read before its ioctl; if a `zpool remove` completes in between, the config still holds the removed mirror where the pool now has an indirect vdev, and the kernel used to take the mirror's leaf in its place. (A) With the fix, debug build: the new ZTS test `zpool_split_indirect_stale` three times, then the group `-T zpool_split` once. (B) Fail-before: master plus only the test commit, debug build: loading the new pool must trip the ASSERT in `spa_remove_init()`, which leaves the ioctl hung with the pool namespace lock held and needs one reboot. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-sci`, `L=/var/log/task-1121-sci`.

## Why

`zpool_vdev_split()` builds the split config from the config that `zpool_open()` cached in the pool handle, and `spa_vdev_split_mirror()` used to skip, without looking at it, the entry at the position of an indirect vdev. A removal of a mirror that completes between the two leaves a leaf of that mirror there: the new pool gets a leaf vdev where its MOS records an indirect one, and `spa_remove_init()` asserts, while the split loads the new pool, that the vdev on the chain of previous indirect vdevs is indirect. ztest -m 3 hit this in user space (one of the M-49 sweep's crashes, `VERIFY3P(vd->vdev_ops, ==, &vdev_indirect_ops)` at `vdev_removal.c:632`); this task checks the kernel.

- fix (commit 1, `32604f14918`): the entry at the position of an indirect vdev must be an indirect vdev with the same guid, else the split fails with EINVAL before it changes anything.
- test (commit 2, `803043c1796`): a new helper, `split_on_signal <pool> <newpool>` (`tests/zfs-tests/cmd/split_on_signal.c`, libzfs), opens the pool, prints `ready`, waits for SIGUSR1, then calls `zpool_vdev_split()` (the new pool exported) and exits 0 on success, 1 on failure (libzfs prints why), 2 if it did not try. `zpool_split_indirect_stale` creates `mirror a1 a2 mirror b1 b2` (64 MiB files under `/var/tmp/zpool_split_indirect_stale`), starts the helper, waits (at most 60 s) for `ready`, runs `zpool remove testpool mirror-0` and `wait_for_removal`, sends SIGUSR1 and waits for the helper. It expects exit 1 with `invalid argument` in the helper's output (libzfs's wording for EINVAL), no testpool2, testpool ONLINE with b1 and b2 ONLINE, then a fresh `zpool split -R` that succeeds with testpool2 ONLINE.

Expected with the fix: PASS in well under a minute; the test logs `split_on_signal exited with 1: ready` followed by `Unable to split testpool: invalid argument for this pool operation`. Without the fix (part B): the helper's split never returns. dmesg shows `VERIFY3P(vd->vdev_ops, ==, &vdev_indirect_ops) failed (<a> == <b>)` (a: `vdev_file_ops`, b: `vdev_indirect_ops`), `PANIC at vdev_removal.c:632:spa_remove_init()`, `Showing stack for process <pid>`, Comm `split_on_signal`, and a trace through `spl_panic`, `spa_remove_init`, `spa_load` (possibly with `spa_load_impl` or `spa_ld_open_indirect_vdev_metadata`), `spa_vdev_split_mirror`, `zfs_ioc_vdev_split` and `zfsdev_ioctl`. This is an SPL assertion, not an oops: the thread parks itself in TASK_UNINTERRUPTIBLE forever, holding the pool namespace lock, so `kernel.panic_on_oops` plays no part, but the hung-task detector reports it after 120 s, and every later `zpool` command blocks in D state behind it. The test itself sits in `wait` until the runner's 600 s timeout, after which the runner's own cleanup blocks the same way: the runner is not expected to return, and you do not wait for it.

What is tested: fork https://github.com/tigerblue77/zfs, branch `split-check-indirect-config`, head `803043c1796ef6cda0fa53063009a6cd5103104c`, tree `e772e81bce9bc0435c0b2ff4c75be293e475f22b`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `931b9bef5c8a738e0437805600702bbfc44d4941`, whose `module/zfs/spa.c` is master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_indirect_stale 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. In part A, capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|split_on'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: exactly one is pre-approved, in step 5, after the fail-before run. In part A, a hang, an oops, a panic, a `PANIC at` line or a module that will not unload: capture as above, stop, report and wait for main. Before the step 5 reboot, arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, and write in $L/progress.md where you are. Use `sync -f $L/progress.md` rather than a global `sync`, then the sysrq reboot through a transient unit, as in DBM4.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs` (quote; if hung_task_panic is 1, set it to 0 for this task with `sudo -n sysctl kernel.hung_task_panic=0` and say so: part B leaves tasks in D state for minutes, and a hung-task panic would reboot the VM before the capture), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zpool_split_indirect_stale 2>&1` (none), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent is expected. If it is there: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it is empty, or names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-sci; L=/var/log/task-1121-sci
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs split-check-indirect-config && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   803043c1796ef6cda0fa53063009a6cd5103104c e772e81bce9bc0435c0b2ff4c75be293e475f22b a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   803043c1796 ZTS: split a pool from its config before a removal
#   32604f14918 Check the split config entry of an indirect vdev
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/spa.c tests/zfs-tests/cmd/split_on_signal.c tests/zfs-tests/tests/functional/cli_root/zpool_split/zpool_split_indirect_stale.ksh
#   007607703e34cc227b96aa3a782d7fd8db29c21cd79a3495d533688df943930b  module/zfs/spa.c
#   12f957fdcad1e2baaf86d7042d494268d420f477d3531051588fc99b52ffa621  tests/zfs-tests/cmd/split_on_signal.c
#   cc55c02509c7d2634ff2a29df4ee010065ca69e81fa0aee56c8189554cc6a86b  .../zpool_split_indirect_stale.ksh
grep -c 'strcmp(type, VDEV_TYPE_INDIRECT) != 0' module/zfs/spa.c   # 1
sed -n 632p module/zfs/vdev_removal.c                                # ASSERT3P(vd->vdev_ops, ==, &vdev_indirect_ops);
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-sci
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick 803043c1796ef6cda0fa53063009a6cd5103104c
git rev-parse HEAD^{tree}          # 931b9bef5c8a738e0437805600702bbfc44d4941
sha256sum module/zfs/spa.c tests/zfs-tests/cmd/split_on_signal.c
#   ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c   (master's)
#   12f957fdcad1e2baaf86d7042d494268d420f477d3531051588fc99b52ffa621  tests/zfs-tests/cmd/split_on_signal.c
grep -c 'strcmp(type, VDEV_TYPE_INDIRECT) != 0' module/zfs/spa.c   # 0
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-sci; L=/var/log/task-1121-sci
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Check that the helper was built and linked into the in-tree path: `ls -l <tree>/tests/zfs-tests/bin/split_on_signal` for both trees (a symlink to `tests/zfs-tests/cmd/split_on_signal`). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-sci; L=/var/log/task-1121-sci; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_indirect_stale > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in well under a minute. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the `split_on_signal exited with` note and the line after it (expected: exit 1, `ready`, `Unable to split testpool: invalid argument for this pool operation`); the lines of `sudo -n grep -E 'finishing device removal for vdev 0|spa_load\(testpool2, [^)]*\): (LOADING|LOADED|FAILED)' /proc/spl/kstat/zfs/dbgmsg` (zfs-tests.sh clears the debug log when it starts, so they belong to this run: expected exactly one `finishing device removal for vdev 0`, then exactly one LOADING and one LOADED for testpool2, both from the fresh split at the end; a second LOADING, or any FAILED, would mean that the stale split got as far as building testpool2, and is reported as such; if there is no line at all, say so and quote `cat /sys/module/zfs/parameters/zfs_dbgmsg_enable`); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-T zpool_split` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zpool_split > $L/zts-az.log 2>&1 < /dev/null &`. Expected: the nine tests of `common.run` PASS, `zpool_split_indirect_stale` and `zpool_split_indirect` among them; `zpool_split_wholedisk` (`linux.run`) needs the `scsi_debug` module: if it cannot load it, record its result as is. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N1. Quote `cat /sys/module/spl/parameters/spl_panic_halt`: 0 expected, so that the assertion parks the thread instead of panicking the VM; if it is 1, set it to 0 for this run (`echo 0 | sudo -n tee /sys/module/spl/parameters/spl_panic_halt`; the reboot restores it) and say so. `kernel.panic_on_oops` is left as it is: no oops is expected. Start a capture flushed line by line that survives your commands: `L=/var/log/task-1121-sci; setsid nohup sudo -n sh -c "dmesg --follow > $L/panic-follow.txt" > /dev/null 2>&1 < /dev/null &`. Then `touch $L/fb-start`, write $L/progress.md ("step 4 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-sci; L=/var/log/task-1121-sci
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zpool_split_indirect_stale > $L/zts-fb.log 2>&1 < /dev/null &
```

Poll dmesg every 10 s for at most 5 minutes. Expected within a minute: the assertion. Quote the `VERIFY3P(` line, the `PANIC at` line, the line with `Comm:` and the first 15 frames of the Call Trace after `Showing stack for process`. Then wait 3 more minutes (the hung-task report of `split_on_signal` comes about 120 s after the assertion), and capture into $L/fb-state.txt: `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|split_on|zpool|zfs-tests|ksh'`, the kernel stacks of every D task, `sudo -n tail -n 80 /proc/spl/kstat/zfs/dbgmsg` (its last lines show where the load of testpool2 stopped: a `spa_load(testpool2, config trusted): LOADING` with no LOADED after it, and, if the import progress notes are kept for a split, `'testpool2' Loading indirect vdev metadata` as the last note for it) and `sudo -n dmesg | tail -n 200`; save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`. Quote the D tasks (command and wait channel: `split_on_signal` is expected there, in `spl_panic`, and possibly `zpool` commands of the test's cleanup or of the runner, blocked on the namespace lock), any `blocked for more than` line with its task name, and the last `spa_load(testpool2` line. Do not wait for the runner; if it has returned, quote its result line. No assertion within 5 minutes: capture the same, wait for the runner (at most 15 minutes in all) and report its result and the test's own log: the scenario did not occur.

## STEP 5: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `find /var/tmp -mindepth 1 -maxdepth 1 -newer $L/fb-start -printf '%M %u %s %TT %p\n'` (quote). The hung run cannot clean up, so these are expected among them: `zpool_split_indirect_stale` (the test's four file vdevs and the helper's output), `file-vdev0` to `file-vdev2`, and possibly `testdir` and `constrained_path.*`. Remove exactly those of them that the listing shows (`sudo -n rm -rf` with their names); anything else it lists is reported and left, `/var/tmp/test_results` stays. `/etc/zfs/zpool.cache` is expected above 0 (the run's `testpool`): quote its `ls -l` and the `strings` check of step 0; if it names only `testpool...` with paths under `/var/tmp`, move it to `$L/zpool.cache.fb`; otherwise report it and leave it. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the helper's exit status and message, the counts of `finishing device removal for vdev 0`, testpool2 LOADING and LOADED), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: the assertion (VERIFY3P line, PANIC line, Comm, first 15 frames), the last `spa_load(testpool2` line, the D tasks and any hung-task lines, whether the runner returned, `spl_panic_halt` and `hung_task_panic` as found, the reboot (time, how you resumed), what was removed and where the cache file went. One-line verdict: "SCI PASS 3/3 debug (EINVAL, pool intact, fresh split OK), -T zpool_split PASS; fail-before spa_remove_init VERIFY3P via spa_vdev_split_mirror confirmed" or what differs.
