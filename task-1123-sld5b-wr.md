v2 of TASK-1122-SLD5B (task-1122-sld5b.md): the tests are ported onto wrouesnel's zinject framework (openzfs/zfs#19997 direction); the fixes are unchanged.

TASK-1123-SLD5B-WR (from the coordinator session "main"): functional, about 6 h builds included, no reboot expected. VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. Successor of TASK-1120-SLD5 for step 7a v2.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`, `M`, `T`).

## Why

Every vdev change (zpool add, attach, replace, detach, remove, split) starts in `spa_vdev_enter()` or `spa_vdev_detach_enter()` (module/zfs/spa_misc.c), which take `spa_namespace_lock` and then the config lock as writer, and ends in `spa_vdev_exit()`, which waits for the change's txg to sync. On a pool whose sync is stalled on a device that does not answer, both waits used to hold the namespace lock, so every `spa_open()` on the system waited too. Step 7a (TASK-1120-SLD5, all PASS) waits for the config lock and for the final txg without the namespace lock.

Version 2 changes how such a change keeps the pool while it waits without the namespace lock. Version 1 took a reference on the pool. The removal thread completes a removal through `spa_vdev_enter()`/`spa_vdev_exit()`, and a load restarts it before the load takes the namespace lock back; `spa_load()` then sets `spa_minref` from the reference count, so a reference held by the thread at that moment made every later export of the pool fail with EBUSY (and, on a debug build, trips an ASSERT in a later `spa_close()`), and an import that failed after the load freed the pool under the thread (userspace model: wrong `spa_minref`, export EBUSY, use-after-free under ASAN). v2 counts such changes in `spa->spa_vdev_change_holds`, under the namespace lock: export fails with EBUSY while the count is not zero, `spa_unload()` waits for the count to drain, and `spa_vdev_exit()` releases `spa_vdev_top_lock` before the namespace lock.

Two new zinject points (`zinject -n <point> -A pause`) make the window testable: `spa_load_impl:end` (end of the unlocked part of a load, after the removal thread is restarted) and `spa_vdev_exit:unlocked` (a change that dropped the namespace lock, before its last txg wait). A handler is matched by pool name and can be added whether the pool is loaded or not; the test adds its handlers once `zinject -u` has unloaded the pool. The new test `vdev_change_load_wait` (replacement group) starts a removal with `zfs_removal_suspend_progress=1`, unloads the pool with `zinject -u`, adds handlers at both points, lets the removal go on, opens the pool (`zpool status`), waits until the load is held at its end and the completed removal at its last wait, removes the first handler (the load ends), then the second, and the pool must export (with up to 5 retries while busy) and import again. With the reference of v1, the export stays busy: that is the fail-before (branch `spa-lock-detangle-7-v2-wr-fb`, release build only, configured with `--enable-inject-points`: on a debug build the wrong `spa_minref` trips the `spa_close()` ASSERT and would panic the VM). `vdev_change_sync_wait` (SLD5's test) is unchanged; its fail-before (the parent's spa_misc.c, the same file as in SLD5) is not repeated.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-lock-detangle-7-v2-wr`, head `bfebf20f54db200387256432a634b27a46ab5920`, tree `97385d14a6c9889d9bb2a35ff63e6adf1c8e06cf`. Its parent `c763dacf340d05230c1e328cf31a9f7c9a225a6a` is step 5 ("Stop autotrim and rebuild threads before the namespace lock"), on step 4 `5c40f9b9a86535259a8f1ab03d5a42e5f5659e5a`, on `189a309d464ca51c74d627fdbdd84b5992eade2d` (wrouesnel's zinject injection-point framework, his branch `zinject-points-uplift`, 16 commits) on upstream master `a1daebfec3f`. The fail-before branch `spa-lock-detangle-7-v2-wr-fb`, head `62f01ac9d3cca2a96c57f4477d686387860e9e68`, tree `5c15f4a424c1495461c4fc23045a72c625cb6425`: one commit on the head (the patch of `6157f616ed8`, unchanged) that takes module/zfs/spa_misc.c, module/zfs/spa.c and include/sys/spa_impl.h back to step 7a v1 (the reference), keeping the two new points and the tests.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject` says no handlers are registered, and `ls -d /var/tmp/vdev_change_sync_wait /var/tmp/vdev_change_load_wait /var/tmp/task-1123-sld5b-wr/demo 2>&1` says none exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A ZTS run still going after 2 hours (a group) or 20 minutes (a single test), or a build after 90 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, `sudo -n cat /proc/<pid>/stack` of each `zpool` and `zinject` process, and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main (the standing reboot approval covers a wedged module only after these captures). The ported tests have no hold time: a thread held at a zinject point (`zio_inject_pause` in its stack) stays held until its handler is removed by ID (`zinject -c <id>`, which takes no namespace lock); say so in the report and leave that to main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `cat /proc/sys/kernel/hung_task_timeout_secs` (quote; do not change it), `id zts`, `command -v ksh git`, `git --version`, `ls -d /var/tmp/task-1123-sld5b-wr /var/log/task-1123-sld5b-wr 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/vdev_change_sync_wait /var/tmp/vdev_change_load_wait 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1123-sld5b-wr; L=/var/log/task-1123-sld5b-wr; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 4 $U spa-lock-detangle-7-v2-wr && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD^) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd bfebf20f54db200387256432a634b27a46ab5920 c763dacf340d05230c1e328cf31a9f7c9a225a6a 5c40f9b9a86535259a8f1ab03d5a42e5f5659e5a 189a309d464ca51c74d627fdbdd84b5992eade2d 97385d14a6c9889d9bb2a35ff63e6adf1c8e06cf 0" "zr bfebf20f54db200387256432a634b27a46ab5920 c763dacf340d05230c1e328cf31a9f7c9a225a6a 5c40f9b9a86535259a8f1ab03d5a42e5f5659e5a 189a309d464ca51c74d627fdbdd84b5992eade2d 97385d14a6c9889d9bb2a35ff63e6adf1c8e06cf 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
36a12122c36b4f98914fb51ae0ef634ed7abd5ee12ff4105628d7a93f922e9d3  include/sys/spa_impl.h
a9d92881221b83a3f57da9d44bcfd7d671e2691f7cbcc9f7102b14c2680e2dbf  module/zfs/spa.c
ebcaf13356fc69d41ba04af08786b58aa7dad4cb526a0b6f35efe9e727775e0f  module/zfs/spa_misc.c
02efe9e943c0d9987ef1f07b5ebf9a393adff736b257a530cd4aa08b3f8cb293  tests/runfiles/common.run
29513d72a99996a96983ffbca8501ff1c68eb18ae6bbfc8f36b5b470b2eb5910  tests/zfs-tests/tests/Makefile.am
eb50c30d3114bca445fceec38fb14ad7487c7d21995f5bded1085f4ce45ccd4b  tests/zfs-tests/tests/functional/replacement/vdev_change_load_wait.ksh
66d463e3110c40c14ae11461b8872c24fbdbcfb4bef5e6a30313030c0f37993d  tests/zfs-tests/tests/functional/replacement/vdev_change_sync_wait.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD^ HEAD && ls -l tests/zfs-tests/tests/functional/replacement/vdev_change_*_wait.ksh
```

Expected TREES-OK, two SHA-OK lines, seven name-status lines (`A` for the two tests, `M` for the five other files above) and mode `-rwxr-xr-x` for both tests. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1123-sld5b-wr; L=/var/log/task-1123-sld5b-wr
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure --enable-inject-points && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror). Quote `grep -H 'injection points will be built' $L/build-z?.log`: both lines must end `yes` (the ported tests report UNSUPPORTED on a module without injection points); a `no` is a stop: report it.

## STEP 3: debug runs (all expected PASS), then DEMO-F

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1123-sld5b-wr/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-sld5b-wr/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it.

1. The new test, 3 runs (tag zd-load, `<sel>` = `-t vdev_change_load_wait`). Expected per run: the replacement group's `setup` PASS, `vdev_change_load_wait` PASS in under 2 minutes, `cleanup` PASS. From the merged log of each run quote the two `held by zinject handler` NOTE lines and the SUCCESS lines of `zinject -u`, `zinject -c` (two), `zpool export` and `zpool import`.
2. `vdev_change_sync_wait`, 3 runs (tag zd-sync, `<sel>` = `-t vdev_change_sync_wait`), with SLD5's expectations: PASS in under a minute; quote the two `NOTE: zpool attach ... returned 0: ...` and `NOTE: zpool detach ... returned 0: ...` lines of each run.
3. Groups, 1 run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `replacement`, `zinject`, `failmode`, `zpool_add`, `zpool_attach`, `zpool_detach`, `zpool_replace`, `zpool_split`, `zpool_remove`, `removal`, `cache`, `fault`, `zpool_export`, `zpool_import`, `raidz`. `raidz` (RAID-Z expansion, an attach) is the longest, about an hour on debug: run it last; `zpool_import` takes about 45 minutes. Expected: all PASS (or SKIP where the same group skips on master: say which, with the skip reason). Known on master, not from this branch: `zinject_counts` in `zinject` may FAIL with `check_count_freq: freq=100 match=98 inject=97` (counter race, M-31); `auto_spare_002_pos` in `fault` may FAIL in its anymirror cases at `wait_vdev_state ... DEGRADED 60` (TASK-1120-ASP). `replacement` includes both new-series tests and `vdev_change_thread_wait` (step 5); `zinject` runs wrouesnel's own tests of the framework (`zinject_args` and the `zinject_point_*` tests; this branch changes none of them); `removal` includes `removal_with_export`, `removal_resume_export` and `removal_condense_export` (a removal restarted by a load); `zpool_export` and `zpool_import` exercise the export check and `spa_unload()`, both changed. For any other result, quote it as above and say whether its log involves `spa_vdev_enter`, `spa_vdev_exit`, `spa_unload`, `spa_export_common`, "pool is busy", the config lock, the namespace lock, `spa_vdev_top_lock` or `spa_sync`; do not rerun.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|possible circular|deadlock` (expected none; a suspended-pool message from `failmode` is expected: count them, quote one). Keep zd loaded.

DEMO-F: the attach case of SLD5 by hand on the branch's module, with a 20 s bound (the handler is removed by its ID 20 s after it is added: `-A pause` has no hold time like the former `-s 20`) (Z is the tree whose module is loaded; T names the demo):

```sh
D=/var/tmp/task-1123-sld5b-wr; L=/var/log/task-1123-sld5b-wr; Z=$D/zd; M=$D/demo; T=F
mkdir $M && truncate -s 256M $M/a $M/b $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld5o $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld5a $M/a &&
id=$(sudo -n $Z/zinject -q -n spa_sync:config -A pause sld5a) && echo "DEMO-$T handler $id" &&
{ { sleep 20; sudo -n $Z/zinject -c $id; } > /dev/null 2>&1 & } &&
{ sudo -n $Z/zpool sync sld5a & } &&
for i in $(seq 50); do sudo -n $Z/zinject | awk -v id=$id '$1 == id && $6 > 0 { f = 1 } END { exit !f }' && break; sleep 0.2; done && echo "DEMO-$T sync held" &&
s0=$(date +%s%N) && { { sudo -n $Z/zpool attach sld5a $M/a $M/b; echo "DEMO-$T attach rc=$? after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } & }
pa=$!
sleep 2
s1=$(date +%s%N); { sudo -n $Z/zpool status sld5o > /dev/null; echo "DEMO-$T status of sld5o rc=$? in $(( ($(date +%s%N) - s1) / 1000000 )) ms"; } &
ps1=$!
sleep 2
for p in $(pgrep -f 'zpool (attach sld5a|status sld5o)'); do echo "== $p $(ps -o stat=,args= -p $p)"; sudo -n cat /proc/$p/stack; done
{ sudo -n $Z/zinject -c $id; echo "DEMO-$T handler removed after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
wait $ps1
wait $pa
sudo -n $Z/zpool status -P sld5a | grep -c "$M/b"
wait
sudo -n $Z/zinject -c all; sudo -n $Z/zpool destroy sld5a; sudo -n $Z/zpool destroy sld5o; sudo -n rm -rf $M && sudo -n fstrim -av
```

Run the whole block as one command. Only the attach runs in the background (in SLD5's block the trailing `&` ran the whole chain in a subshell, so `$id` and `s0` never reached the later lines; the hold time covered that, nothing does now); `$6` is the INJECT column of the listing, which now ends with FROM and AT; the last `wait` also waits for the 20 s bound. Expected: `DEMO-F sync held`; `DEMO-F status of sld5o rc=0 in N ms` with N below 1000, printed before the handler is removed; the stack of the `zpool attach` process (not its `sudo`) waiting for the config lock: `spa_config_enter_impl` (or `cv_wait`) <- `spa_config_enter` <- `spa_vdev_enter_locks` (may be inlined) <- `spa_vdev_enter` <- `spa_vdev_attach` <- `zfs_ioc_vdev_attach`; `DEMO-F handler removed after` about 4000 ms; `DEMO-F attach rc=0` shortly after; the grep count `1`. Quote all of it. Then unload with zd's `scripts/zfs.sh -u`, `sudo -n rm -rf $D/zd`, `sudo -n fstrim -av`.

## STEP 4: release runs (expected PASS)

Load zr ("(DEBUG mode)" must be absent); note N1. `vdev_change_load_wait` 3 times (tag zr-load) and `vdev_change_sync_wait` 3 times (tag zr-sync), with the expectations and quotes of step 3 items 1 and 2; then the group `removal` once (tag zr-removal), expected all PASS. After them: no pool left, the dmesg check since N1. Keep zr loaded.

## STEP 5: the fail-before, release build only (about 30 minutes)

Fetch the fail-before branch into zr, check it, rebuild in place (zr's module stays loaded meanwhile):

```sh
cd /var/tmp/task-1123-sld5b-wr/zr && git fetch -q --depth 2 https://github.com/tigerblue77/zfs spa-lock-detangle-7-v2-wr-fb && git checkout -q FETCH_HEAD &&
test "$(git rev-parse HEAD)" = 62f01ac9d3cca2a96c57f4477d686387860e9e68 && test "$(git rev-parse HEAD^)" = bfebf20f54db200387256432a634b27a46ab5920 && echo HEADS-OK &&
test "$(git write-tree)" = 5c15f4a424c1495461c4fc23045a72c625cb6425 && echo TREE-OK &&
printf '%s\n' "3d10ecf66cc2cedc82709b04123f04640b66d76c2c7dda5296b94cdd644c091d  module/zfs/spa_misc.c" "ab5aa6b3299a2ff79b6af8428def15cf2bbaca3bbea7a4cc51ed8ba2252c1d4b  module/zfs/spa.c" "e8e1e3eb2639143efdd15f1d5dfa76274fdcfc2af700596e59534038afbe57af  include/sys/spa_impl.h" | sha256sum -c --quiet && echo SHA-OK &&
git diff --name-status HEAD^ HEAD
```

Expected HEADS-OK, TREE-OK, SHA-OK and three `M` lines (include/sys/spa_impl.h, module/zfs/spa.c, module/zfs/spa_misc.c). Then, detached and polled like a build: `setsid nohup sh -c "cd /var/tmp/task-1123-sld5b-wr/zr && nice make -j4; echo build-rc=\$?" > /var/log/task-1123-sld5b-wr/build-fb.log 2>&1 < /dev/null &`. It must end with `build-rc=0`; quote `grep -E 'spa_misc|spa\.o|zfs\.ko' $L/build-fb.log` and every `warning:` line outside objtool notes. `cat /sys/module/zfs/srcversion` must still give zr's value, and `modinfo -F srcversion $D/zr/module/zfs.ko` must now differ. Unload with zr's `scripts/zfs.sh -u` and load zr again: the srcversion must equal the new `modinfo` value, no "(DEBUG mode)"; note N2.

Do not load this code as a debug build: with the reference, `spa_minref` is one too high and the next `spa_close()` that does not hold the namespace lock trips its ASSERT, which panics a debug kernel.

Fail-before fb: the pre-run checks, then one run (tag fb, in zr, `<sel>` = `-t vdev_change_load_wait`). Expected: FAIL at the end, `zpool export testpool1` failing with "pool is busy" on each of its retries (`log_must_busy`), after the two `held by zinject handler` notes; the cleanup notes `testpool1 cannot be exported: left busy` and the group's cleanup may then fail or PASS: quote it. The pool stays imported and busy: `timeout 30 sudo -n $D/zr/zpool list -H -o name` shows it, and `sudo -n $D/zr/zpool export testpool1` must still fail with "pool is busy" (quote it). Then unload with zr's `scripts/zfs.sh -u`, which evicts the pool (quote its output and `lsmod | grep -E '^zfs '`, expected empty). If the unload fails, capture as for a hang and report; do not reboot without main. A fb run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

After it: the dmesg check since N2 (a `blocked for more than` line is not expected), `sudo -n rm -rf /var/tmp/task-1123-sld5b-wr /var/tmp/vdev_change_load_wait`, `ls -d /var/tmp/vdev_change_sync_wait /var/tmp/vdev_change_load_wait 2>&1` (absent), `sudo -n fstrim -av`, `df -h /var/tmp`. Logs stay in /var/log/task-1123-sld5b-wr.

## Report

Write /var/log/task-1123-sld5b-wr/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, hung_task_timeout_secs, git version; TREES-OK, the two SHA-OK, the name-status lines and the tests' modes; build rc, warnings and injection-points lines of zd and zr; srcversions and DEBUG mode; step 5's HEADS-OK, TREE-OK, SHA-OK, name-status lines, build-fb rc and quoted lines, and the srcversion checks.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note. Then one verdict line, e.g. "spa-lock-detangle-7-v2-wr: vdev_change_load_wait PASS debug 3/3, release 3/3; vdev_change_sync_wait PASS debug 3/3, release 3/3; groups replacement, zinject, failmode, zpool_add, zpool_attach, zpool_detach, zpool_replace, zpool_split, zpool_remove, removal, cache, fault, zpool_export, zpool_import, raidz PASS (debug), removal PASS (release); DEMO-F status 12 ms; fail-before (release): vdev_change_load_wait FAIL 1/1 (export busy), pool evicted by the unload, no reboot".
3. Quotes: the NOTE and SUCCESS lines of step 3 items 1-2 and step 4; the non-PASS lines of the groups; DEMO-F output with the stacks; fb's failing lines, cleanup, the export after it and the unload; the dmesg checks.
4. Deviations, cleanup, then End.
