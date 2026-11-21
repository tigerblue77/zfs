TASK-1121-ZLT (from the coordinator session "main"): functional, about 1 h 15 min builds included, two debug builds, no reboot. The regression test of our zed fix "create the task queue that enables late pools' datasets" (fork branch `zed-slm-handle-taskq`, no PR yet). (A) With the fix, debug build: the new ZTS test `zed_late_pool` three times, then the group `-T events` once. (B) Fail-before: upstream master plus only the test commit, debug build: the test once, expected to FAIL because master's zed dies with SIGSEGV. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB). Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-zlt`, `L=/var/log/task-1121-zlt`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. Never start or stop a zed by hand: the test starts and stops its own, and a zed started by hand with the module loaded would act on the test pools (the one exception is the reaper case under the VM rules). If a result surprises you, stop and report it with the outputs.

## Why

At startup, zed's "enum-pools" thread keeps in `g_pool_list` every imported pool whose root vdev state is below DEGRADED (6). Later, each SLM event that visits the pools with `zfs_iter_pool()` (a udev disk add, or a `vdev_check` event) looks the visited pool up in that list, and if it is now DEGRADED or better calls `taskq_dispatch(g_taskq, zfs_enable_ds, ...)` (master `cmd/zed/agents/zfs_mod.c` line 742) to mount its datasets. Nothing creates `g_taskq` on master (since 4e9b1569605, 2017), so `taskq_dispatch()` locks the mutex of a NULL `taskq_t` and zed dies with SIGSEGV in its "agents" thread. The fix (commit `3a95cbc4c68`) creates the task queue in `zfs_slm_init()`; the commit under it (`48b4351543c`) puts a lock around the SLM's shared libzfs handle.

TASK-1120-ZEDR (part 2, VM 106, 20/11) reproduced it outside ZTS, with a scenario built from the code: master's zed SIGSEGV 2/2 (gdb: `taskq_dispatch(tq=0x0)` called from `zfs_iter_pool` at zfs_mod.c:742, under `zpool_iter` and `zfs_deliver_check`), the fix's zed alive 2/2 with the late pool's dataset mounted by `zfs_enable_ds()`. The new test `tests/zfs-tests/tests/functional/events/zed_late_pool.ksh` (commit `c9f742c1cf4`, registered in `tests/runfiles/linux.run`, events section, last) is that scenario under ZTS, with ZTS's own zed (`zed_start`, the events group's ZEDLETs `all-debug.sh` and `all-syslog.sh`). It:

1. creates `testpool1` on `/var/tmp/vdev1` (`cachefile=none`, `-m none`) with `testpool1/testfs1` mounted on `/var/tmp/testdir1`, then unmounts it; creates `testpool2`, a mirror of `/var/tmp/vdev2` and `/var/tmp/vdev3` with `autoreplace=on`, exports it and deletes `vdev3`;
2. `zinject -d /var/tmp/vdev1 -e nxio testpool1` and `zpool reopen testpool1`: the only leaf cannot reopen and the pool is unavailable (it logs `NOTE: testpool1 is SUSPENDED`, or `UNAVAIL`);
3. clears the events, starts zed, and waits (60 s at most) for zed's line `zfs_unavail_pool: examining 'testpool1' (state <n>)` with n from 0 to 5;
4. `zinject -c all` and `zpool clear testpool1`: the pool is ONLINE again; it logs `NOTE: testpool1/testfs1 mounted before the import: no` (only zed can mount it);
5. `zpool import -d /var/tmp/vdev2 -o cachefile=none testpool2`: with `autoreplace=on` and a dead leaf, `spa_load()` posts `sysevent.fs.zfs.vdev_check`, and zed's agents thread runs `zfs_deliver_check()`, which visits the pools in name order, `testpool1` before `testpool2`;
6. waits (120 s at most) for `testpool1/testfs1` to be mounted, checking each second that its zed is still there. PASS: mounted and zed alive; it then logs zed's SLM lines (`zfs_unavail_pool`, `zfs_slm_event`, `zfs_deliver_check`, `zfs_iter_pool`). A failure logs `NOTE: SLM lines of /var/tmp/zed/zed.log:` and the last 20 of them, `NOTE: Last lines of /var/tmp/zed/zed.log:` and the last 20 lines, `NOTE: Segfaults in dmesg:` with the last 3 `segfault`/`traps:` lines of dmesg, `NOTE: vdev_check events: <n>`, then the reason (`the ZED died after the import` is the expected one on master). The cleanup stops zed, clears the injections, destroys both pools and removes the three files and `/var/tmp/testdir1`.

Expected with the fix (part A): PASS in about a minute, `examining 'testpool1' (state 4)`, and among the SLM lines `zfs_slm_event: EC_zfs.vdev_check`, `zfs_deliver_check: pool '<guid>', vdev <guid>` and `zfs_iter_pool: evaluating vdevs on testpool1 (by GUID)`. In ZEDR's fix rounds no SLM line about the imported pool followed, so a line about `testpool2` may or may not be there. Without the fix (part B): FAIL in about a minute with `the ZED died after the import`, the last SLM line `zfs_iter_pool: evaluating vdevs on testpool1 (by GUID)`, and in dmesg one `agents[<pid>]: segfault at 30 ...` line (in ZEDR: in libc, `pthread_mutex_lock` called by `taskq_dispatch`). The crash is in user space: the test's cleanup and the runner finish normally, the module is not affected, and no reboot is needed.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-zlt`, head `8738e4d144b0a1afd86778433388ca5bc530ecdc`: an empty transport commit (`[skip ci]`, so that its push starts no CI) on the test `c9f742c1cf42fdbfee480a0816f5d9deb7e4abfc` (tree `eac1eb1c7921d92a8e48d8affb67e5ad7e2f7b6b`), on the fix `3a95cbc4c6835482eb5a796834265f5fe637d09d` and `48b4351543cd1bb9e78084525b638c0b00c7e779`, on upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` (20/11). The fail-before tree is that master plus the test commit only: tree `84f3867516208b60ece1ac3502febb217ce2011a`, whose `cmd/zed/agents/zfs_mod.c` is master's. The two trees differ in that one file only. Main pushes `vm-kit-zlt` before sending this task.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs and loop devices under /var/tmp; touch no block device of the VM and no pool this task did not create; never `zpool import` anything yourself (`-a` least of all); never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `sudo -n <tree>/zinject | head -n 1` prints `No handlers registered.` (only the first line is compared), `ls -d /var/tmp/file-vdev* /var/tmp/vdev* /var/tmp/zed /var/tmp/testdir1 /etc/zfs/vdev_id.conf 2>&1` shows nothing, `pgrep -a -x zed; pgrep -a -x lt-zed` shows nothing (the test fails if a zed runs), `losetup -a` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a running zed, a non-empty cache file or a file left by a previous run is a stop: report its `ls -l` (or the `pgrep` line), touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; the events group after 45 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zed|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<tid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report. Run no zpool, zfs or zdb command, no plain `sync` and no `df` once a hang is suspected.
- The reaper case: master's `cmd/zed/zed_exec.c`, which both trees have, can leave a zed that logged `Exiting` without ever exiting (a separate bug, branch `zed-reaper-lost-wakeup`, not in these trees); ZTS's `zed_stop` then sends it SIGTERM once a second until the test times out. If a run stays on one test for more than 5 minutes while `pgrep -a -x zed; pgrep -a -x lt-zed` shows a zed whose log ends with `Exiting` (`sudo -n tail -n 3 /proc/<pid>/fd/2`), capture `ps -L -o pid,tid,stat,wchan:32,comm -p <pid>` and `sudo -n sh -c 'for t in /proc/<pid>/task/*; do echo "== $t"; grep -E "^(State|SigBlk)" $t/status; cat $t/stack; done'` into $L/reaper-<tag>.txt, then `sudo -n kill -9 <pid>` (that zed only) so that ZTS goes on, and report it. Kill nothing else.
- Reboots: none pre-approved; for this task that replaces the standing approval. A hang, an oops, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/vdev* /var/tmp/zed /var/tmp/testdir1 /etc/zfs/vdev_id.conf 2>&1` (none), `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop one), `cat /proc/sys/kernel/core_pattern`, `ulimit -c`, and `command -v coredumpctl gdb` (any answer is fine: say which; install nothing, change no core setting).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside in step 1 once $L exists (`sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0`) and say so. If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module. Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/fix-dbg && cd $D/fix-dbg && git init -q . && git fetch -q --depth 5 https://github.com/tigerblue77/zfs vm-kit-zlt && git checkout -q FETCH_HEAD~1
echo "$(git rev-parse FETCH_HEAD HEAD HEAD~1 HEAD~2 HEAD~3 'HEAD^{tree}' | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree-fix.txt
echo "8738e4d144b0a1afd86778433388ca5bc530ecdc c9f742c1cf42fdbfee480a0816f5d9deb7e4abfc 3a95cbc4c6835482eb5a796834265f5fe637d09d 48b4351543cd1bb9e78084525b638c0b00c7e779 1b1aa0e91b694798085a2771c4eef40e93b9b2db eac1eb1c7921d92a8e48d8affb67e5ad7e2f7b6b 0" > $L/tree-fix.expected
diff $L/tree-fix.expected $L/tree-fix.txt && echo TREE-FIX-OK
git diff --stat=80 FETCH_HEAD~1 FETCH_HEAD | wc -l; git diff --stat=80 HEAD~1 HEAD; git log --oneline -4
```

Expected TREE-FIX-OK, `0` (the transport commit is empty), the diffstat

```
 tests/runfiles/linux.run                           |   2 +-
 tests/zfs-tests/tests/Makefile.am                  |   1 +
 .../tests/functional/events/zed_late_pool.ksh      | 157 +++++++++++++++++++++
 3 files changed, 159 insertions(+), 1 deletion(-)
```

and the subjects `ZTS: zed mounts a pool that was unavailable at its start`, `zed: create the task queue that enables late pools' datasets`, `zed: do not share the SLM's libzfs handle between threads`, `FreeBSD: Fix append-only semantics`. The fail-before tree:

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt
mkdir $D/fb-dbg && cd $D/fb-dbg && git init -q . && git fetch -q --depth 5 https://github.com/tigerblue77/zfs vm-kit-zlt && git checkout -q FETCH_HEAD~4
git -c user.name=t -c user.email=t@t -c commit.gpgsign=false cherry-pick FETCH_HEAD~1 > /dev/null; echo cherry-pick-rc=$?
echo "$(git rev-parse HEAD~1 'HEAD^{tree}' | tr '\n' ' ')$(git status --porcelain | wc -l)"
#   1b1aa0e91b694798085a2771c4eef40e93b9b2db 84f3867516208b60ece1ac3502febb217ce2011a 0
printf '%s\n' "1e94a3886faf27fcfb43c1a88526e1f89e79969833b1eb13101f037c01a3b526  $D/fix-dbg/cmd/zed/agents/zfs_mod.c" "9ae6cfc9d7b22cbe85f8c49535372e67410d6d6e61e513a54f0267e44911cf6b  $D/fb-dbg/cmd/zed/agents/zfs_mod.c" "62183c847393980f04a773ae2a74272192eee488c25bb9cb679c3be613a5f00a  $D/fix-dbg/tests/zfs-tests/tests/functional/events/zed_late_pool.ksh" "62183c847393980f04a773ae2a74272192eee488c25bb9cb679c3be613a5f00a  $D/fb-dbg/tests/zfs-tests/tests/functional/events/zed_late_pool.ksh" "080da1671e349a3b4af5e0df89e7c36bbe77dfad9869831ccce6109e955a501c  $D/fix-dbg/tests/runfiles/linux.run" "080da1671e349a3b4af5e0df89e7c36bbe77dfad9869831ccce6109e955a501c  $D/fb-dbg/tests/runfiles/linux.run" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
grep -c 'taskq_create("zfs_enable_ds"' $D/fix-dbg/cmd/zed/agents/zfs_mod.c $D/fb-dbg/cmd/zed/agents/zfs_mod.c
ls -l $D/fix-dbg/tests/zfs-tests/tests/functional/events/zed_late_pool.ksh $D/fb-dbg/tests/zfs-tests/tests/functional/events/zed_late_pool.ksh | awk '{print $1, $5}'
```

Expected `cherry-pick-rc=0`, the line in the comment, SHA-OK (the fix's zfs_mod.c is TASK-1120-ZEDR's fix one, the fail-before's is master's; the test and the runfile are the same in both), the counts `1` (fix) and `0` (fail-before), and `-rwxr-xr-x 5185` twice. Stop and report if a hash, a count or a subject differs (the branch may have moved). If a fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg (they differ: the git revision is compiled in).

## STEP 3: part A, the fix (tags a1 to a3, ae)

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

A run, one at a time, after the pre-run checks of the rules; note Nb=`sudo -n dmesg | wc -l` before it:

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt; tag=a1   # then a2, a3
cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zed_late_pool > $L/zts-$tag.log 2>&1 < /dev/null &
```

For each run, quote the `Test:` lines with result and duration (a `-t` run prints three: the events setup, the test, the events cleanup) and the Results Summary, then:

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt; tag=a1; Nb=<Nb>
R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag.log)
f=$(sudo -n find "$R" -type f -path '*/zed_late_pool/merged' | head -n 1); echo "$f"; sudo -n cp "$f" $L/merged-$tag.txt
sudo -n grep -E 'testpool1 is |examining|mounted before the import|zfs_slm_event|zfs_deliver_check|zfs_iter_pool|SUCCESS: zpool (reopen|clear|import)|NOTE: (SLM|Last|Segfaults|vdev_check)|the ZED|ERROR' "$f"
sudo -n dmesg | tail -n +$((Nb + 1)) | grep -E 'segfault|traps:|VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than'
```

1. `a1`, `a2`, `a3`: expected PASS each, in about a minute, with the lines described under Why: `testpool1 is SUSPENDED` (or `UNAVAIL`), `examining 'testpool1' (state 4)` (any state from 0 to 5 is right), `mounted before the import: no`, the import's `SUCCESS` line, then `zfs_slm_event: EC_zfs.vdev_check`, `zfs_deliver_check: ...` and `zfs_iter_pool: evaluating vdevs on testpool1 (by GUID)`; no `the ZED ...` line, no `ERROR`; the dmesg grep prints nothing. A FAIL: quote `$L/merged-$tag.txt` from its `NOTE: SLM lines` line to the end, and the dmesg lines since Nb matching `segfault|traps:`, then go on with the next run; two FAILs in a row: stop part A and report.
2. `ae`: `-T events` once, after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T events > $L/zts-ae.log 2>&1 < /dev/null &` (about 15 minutes; TASK-1120-ZEDR measured 13 to 16 for the group without the new test). Expected: the 16 tests of linux.run's events section PASS, `zed_late_pool` last, with the setup and the cleanup. Quote every `Test:` line with result and duration and the Results Summary, then the commands above with `tag=ae` (the same lines expected from `zed_late_pool`, here run after the 15 other tests). A FAIL is quoted with the 20 lines before its first `ERROR` (from its own `merged` file: `sudo -n find "$R" -type f -path '*/<test>/merged'`) and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0 + 1))` matching `segfault|traps:|VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and `lsmod | grep -E '^(zfs|spl) '` (empty).

## STEP 4: part B, fail-before (tag b1)

`sudo -n $D/fb-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S2) and the "(DEBUG mode)" load line. Note N1=`sudo -n dmesg | wc -l` and T0=`date '+%Y-%m-%d %H:%M:%S'`. After the pre-run checks, ONCE:

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zed_late_pool > $L/zts-b1.log 2>&1 < /dev/null &
```

Expected: the test FAILs in about a minute and the runner returns (the setup and the cleanup PASS). Quote the `Test:` lines and the Results Summary, then:

```sh
D=/var/tmp/task-1121-zlt; L=/var/log/task-1121-zlt; N1=<N1>
R=$(awk '/^Log directory/ { print $3 }' $L/zts-b1.log)
f=$(sudo -n find "$R" -type f -path '*/zed_late_pool/merged' | head -n 1); echo "$f"; sudo -n cp "$f" $L/merged-b1.txt
sudo -n grep -E 'testpool1 is |examining|mounted before the import|SUCCESS: zpool (reopen|clear|import)' "$f"
sudo -n sed -n '/NOTE: SLM lines of/,/the ZED /p' "$f"
sudo -n dmesg | tail -n +$((N1 + 1)) | grep -E 'segfault|traps:|VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than'
```

Expected: `testpool1 is SUSPENDED` (or `UNAVAIL`), `examining 'testpool1' (state 4)`, `mounted before the import: no`, the import's `SUCCESS` line; then the failure block: SLM lines whose last is `zfs_iter_pool: evaluating vdevs on testpool1 (by GUID)`, the last lines of zed.log, `NOTE: Segfaults in dmesg:` with an `agents[<pid>]: segfault at 30 ...` line, `NOTE: vdev_check events: 1` (1 or more), and `the ZED died after the import`. dmesg since N1: that one `segfault` line and nothing else.

The core of the dead zed, as this VM keeps it, changing nothing: if step 0 found `coredumpctl`, quote `sudo -n coredumpctl --no-pager list --since "<T0>"` and, for a `zed` or `lt-zed` entry, the first 60 lines of `sudo -n coredumpctl --no-pager info <PID>` (with its stack trace, if systemd-coredump made one). If its `Storage:` line says the core is present and step 0 found `gdb`, also `sudo -n coredumpctl --no-pager debug <PID> --debugger-arguments='-batch -ex bt'` and quote the first 12 frames (expected: `taskq_dispatch (tq=0x0, ...)` called from `zfs_iter_pool` at zfs_mod.c:742, under `zpool_iter` and `zfs_deliver_check`). No core stored (for instance `Resource limits disable core dumping`, a `core_pattern` that is not systemd-coredump, or no entry at all): say so; the test's own log and dmesg then carry the evidence.

A PASS here, or a FAIL with another reason than `the ZED died after the import`, contradicts the prediction and matters most: quote `$L/merged-b1.txt` in full and the dmesg lines since N1. After the run: the pre-run checks once more; unload with `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc) and `lsmod | grep -E '^(zfs|spl) '` (empty).

## STEP 5: cleanup

`pgrep -a -x zed; pgrep -a -x lt-zed` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-vdev* /var/tmp/vdev* /var/tmp/zed /var/tmp/testdir1 /etc/zfs/vdev_id.conf 2>&1` (none), `losetup -a` (empty), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and the copied `merged-*.txt` stay in $L, the ZTS results under /var/tmp/test_results.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, `core_pattern`, `ulimit -c`, coredumpctl and gdb present or not; TREE-FIX-OK, the empty transport, the diffstat, the cherry-pick rc and tree, SHA-OK, the counts, modes and sizes; both builds' rc and warnings; S1, S2 and the DEBUG load lines; what happened to /etc/zfs/zpool.cache at steps 0 and 5.
2. Part A, a table: tag | result | duration | health before zed | enumerated state | mounted before the import | `zfs_iter_pool` on testpool1 | dmesg. Rows a1 to a3, then ae for `zed_late_pool`; then every `Test:` line of `ae` and its Results Summary.
3. Part B: the result and duration, the grep lines, the failure block in full, the dmesg segfault line, and what coredumpctl and gdb gave (or that no core was stored).
4. One verdict line, e.g. "zed-slm-handle-taskq + test c9f742c1cf4 on 6.12 debug: zed_late_pool PASS 3/3, -T events PASS 16/16; fail-before (master 1b1aa0e91b6 + test): FAIL, zed SIGSEGV in agents after visiting testpool1", or what differs.
5. Quotes: every non-PASS run with its lines, the dmesg checks, any reaper capture.
6. Deviations, cleanup, then End.
