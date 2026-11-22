TASK-1122-SLD6B (from the coordinator session "main"): functional, about 6 h builds included, no reboot expected. VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. Replaces TASK-1122-SLD6 (never sent), for step 7b v2.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`, `M`, `T`).

## Why

Some vdev changes sync in several steps: `zpool remove` of a log, cache or data device, the completion of a device removal, a RAID-Z expansion (`zpool attach` to a raidz vdev), `zpool set allocating=off` and `zpool split`. Between two steps, the change releases the config lock and waits for the txg of the first step to sync (`spa_vdev_config_exit()`, module/zfs/spa_misc.c), may wait for the initialize, TRIM and autotrim threads of the vdev to stop or for `spa_reset_logs()` to sync the ZIL out, and takes the config lock as writer again (`spa_vdev_config_enter()`), which waits for `spa_sync()` to release it. All of it holds `spa_namespace_lock`. On a pool whose sync is stalled on a device that does not answer, every `spa_open()` on the system (zpool status of any pool, zpool clear, zinject) waits as long as the device. TASK-1120-SLD5 validated the fix of the start and the end of a change (step 7a v1; TASK-1122-SLD5B validates its v2); this branch (step 7b) does the steps in between: the new `spa_vdev_config_pause()` releases the config lock and the namespace lock and waits without either, counted in `spa->spa_vdev_change_holds` (v2: a count instead of a reference on the pool, as in 7a v2, so that a load does not count it in `spa_minref`), and `spa_vdev_config_resume()` takes them again, waiting for the config lock without the namespace lock as step 7a does. The async thread and the end of `zpool add` now take `spa_vdev_top_lock` before `spa_config_update()`, so that the update cannot run in the middle of a change. `zpool split` keeps the namespace lock while it creates and loads the new pool (not changed). Also new in v2: the completion of a removal now waits for its first step without the namespace lock, and `spa_restart_removal()` could then start a second removal thread (seen in a userspace model: at the end of an import whose load restarted the removal, `spa_async_resume()` started one on the vdev just made indirect); `spa->spa_removal_completing`, set under the namespace lock around that wait, keeps it out.

The new test `vdev_change_step_wait` (replacement group) holds a sync at the zinject -w point `spa_sync:config` (two handlers) while the change waits for the config lock, removes the first handler, and the second one holds the next sync, which the change waits for after its first step. While the change waits there, the zinject handler listing (which takes the namespace lock) and `zpool status` of the pool and of another pool must return within 30 s; then the second handler is removed and the change must succeed. It does this for `zpool remove` of a log, a cache and a data device (then `zpool wait -t remove`), `zpool attach` to a raidz1 vdev (then `zpool wait -t raidz_expand`) and `zpool split` of a mirror (then the new pool is imported and destroyed). Without the fix, the change holds the namespace lock while it waits, the listing blocks, the test fails after 30 s in its first case, and its cleanup removes the handlers (removal releases the held sync before it waits for the namespace lock), so the fail-before ends by itself: no reboot.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-lock-detangle-7b-v2`, head `ccde2bb4c1a07a9100881c970a5a52646eb49c17`, tree `999391e415e5b135f0f03fe16ce5cd9889f4b96c`. Its parent `b7728b1d8ed7dd0e49fb613b8f98cce848a98748` is "zpool split: hold the new pool until done with it" (the fix of M-42, the same patch as `zpool-split-hold-newspa-v2` 25129f6d749, which step 7b needs), on `a789c28f33836007763d206c0331a3445f6fce26` (step 7a v2, `spa-lock-detangle-7-v2`, TASK-1122-SLD5B), on step 5 `07797111d7a0230aabdee4e03e28096feccc9a84`, on step 4 `a5b4cc6898f3ed067190d26e2d2044eba3b8815c`, on the zinject -w infrastructure (`zinject-delay-points-v2-rb1121`, 4 commits) on upstream master `a1daebfec3f`. The fail-before (step 4) is the head with the seven kernel files of the change checked out of the parent (include/sys/spa.h, include/sys/spa_impl.h, module/zfs/spa.c, spa_misc.c, vdev_initialize.c, vdev_removal.c, vdev_trim.c; tree `166462d68344676e03f2c4bfbb469d6c55145ca9`); the tests and the zinject documentation stay.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject` says no handlers are registered, and `ls -d /var/tmp/vdev_change_step_wait /var/tmp/vdev_change_load_wait /var/tmp/task-1122-sld6b/demo 2>&1` says none exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A ZTS run still going after 2 hours (a group) or 20 minutes (a single test), or a build after 90 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, `sudo -n cat /proc/<pid>/stack` of each `zpool` and `zinject` process, and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main (the standing reboot approval covers a wedged module only after these captures).
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `cat /proc/sys/kernel/hung_task_timeout_secs` (quote; do not change it), `id zts`, `command -v ksh git`, `git --version`, `ls -d /var/tmp/task-1122-sld6b /var/log/task-1122-sld6b 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/vdev_change_step_wait 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1122-sld6b; L=/var/log/task-1122-sld6b; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 5 $U spa-lock-detangle-7b-v2 && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD^) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git rev-parse HEAD~4) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd ccde2bb4c1a07a9100881c970a5a52646eb49c17 b7728b1d8ed7dd0e49fb613b8f98cce848a98748 a789c28f33836007763d206c0331a3445f6fce26 07797111d7a0230aabdee4e03e28096feccc9a84 a5b4cc6898f3ed067190d26e2d2044eba3b8815c 999391e415e5b135f0f03fe16ce5cd9889f4b96c 0" "zr ccde2bb4c1a07a9100881c970a5a52646eb49c17 b7728b1d8ed7dd0e49fb613b8f98cce848a98748 a789c28f33836007763d206c0331a3445f6fce26 07797111d7a0230aabdee4e03e28096feccc9a84 a5b4cc6898f3ed067190d26e2d2044eba3b8815c 999391e415e5b135f0f03fe16ce5cd9889f4b96c 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
96bd970faa7d0c5b3c1284dc4e229a9d5259711911fa0f640e1a0fd702398776  cmd/zinject/zinject.c
8ac49d68a5713c641d203633e5415331340221e64e4f9cb31f4f8bee0d3961d0  include/sys/spa.h
69d1a301655ac0b74a4f4b1ca21b5ca1efab7a83cb864f5ca24cf2ed771f0d92  include/sys/spa_impl.h
97b9d007ccb98878b9e2d31e35b668dcd5c8f9da81b69179c13b4c6855c1ec1b  man/man8/zinject.8
20108b58926091282bd93921fc33d52bc4273c318fdce7952c6fa5d5edf215f7  module/zfs/spa.c
4fb24daf3579b9800aa2a413cbf2b7e200a89a98a03bde657c34c326580fef16  module/zfs/spa_misc.c
76276ba57e432fc84ac14e58a035f8516185a0cf81397a368515887a86636b27  module/zfs/vdev_initialize.c
fb9bbdedf970413f42a70f75dd2960522ed971a699daf02a62ffc9b87962209c  module/zfs/vdev_removal.c
4653bc0d8c0d96e0ff80fcac2c7c1fd3bb694aa7724ef4a5cc969c19a97a52c9  module/zfs/vdev_trim.c
ac687a6e297ec5a2784cfe7fa27ac78f14931eb52450e96c0e88ddee9e8be8b9  tests/runfiles/common.run
d2a27e2ece643ee1a76b57910ab36b6f9e46b9b821f3e3f1c05ec95a630b521c  tests/zfs-tests/tests/Makefile.am
fb2492b945808992e9e95f14c1f9e297d52cf3c9c37cd1b81f4bda15c02ee264  tests/zfs-tests/tests/functional/replacement/vdev_change_step_wait.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD^ HEAD && ls -l tests/zfs-tests/tests/functional/replacement/vdev_change_step_wait.ksh
```

Expected TREES-OK, two SHA-OK lines, twelve name-status lines (`A` for the new test, `M` for the eleven other files above) and mode `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1122-sld6b; L=/var/log/task-1122-sld6b
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror).

## STEP 3: debug runs (all expected PASS), then DEMO-F

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1122-sld6b/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1122-sld6b/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it.

1. The new test, 3 runs (tag zd, `<sel>` = `-t vdev_change_step_wait`). Expected per run: the replacement group's `setup` PASS, `vdev_change_step_wait` PASS in under 3 minutes, `cleanup` PASS. From the merged log of each run quote the five `NOTE: zpool remove ... returned 0`, `NOTE: zpool attach ... returned 0` and `NOTE: zpool split ... returned 0` lines; from one run also, in order, the `bounded` SUCCESS lines of the first case (`zinject -c <id>`, `zpool status testpool3`, `zpool status testpool1`, `zinject -c <id>`).
2. Groups, 1 run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `replacement`, `removal`, `zpool_remove`, `zpool_split`, `zpool_set`, `zpool_add`, `zpool_attach`, `cache`, `l2arc`, `zinject`, `failmode`, `fault`, `trim`, `zpool_export`, `raidz`. `raidz` (RAID-Z expansion, whose attach this branch changes) is the longest, about an hour on debug: run it last. Expected: all PASS (or SKIP where the same group skips on master: say which, with the skip reason). Known on master, not from this branch: `zinject_counts` in `zinject` may FAIL with `check_count_freq: freq=100 match=98 inject=97` (counter race, M-31); `auto_spare_002_pos` in `fault` may FAIL in its anymirror cases at `wait_vdev_state ... DEGRADED 60` (TASK-1120-ASP); the scsi_debug tests skip. `replacement` includes `vdev_change_step_wait`, `vdev_change_load_wait` and `vdev_change_sync_wait` (step 7a v2) and `vdev_change_thread_wait` (step 5); `vdev_change_load_wait` holds a removal completed during a load at `spa_vdev_exit:unlocked`, which in this branch is its first pause (`vdev_remove_complete()`), so it also checks that the pause is counted, not a reference, and that the pool exports afterwards; `removal` and `zpool_remove` remove log, cache and data devices (`spa_vdev_remove_log()`, `vdev_passivate()`, `vdev_remove_complete()`, all changed here); `zpool_set` includes `vdev_set_allocating` (`zpool set allocating=off`: `vdev_passivate()`); `zpool_split` runs the split's pauses and the new pool phase that keeps the lock. For any other result, quote it as above and say whether its log involves `spa_vdev_config_pause`, `spa_vdev_config_resume`, `spa_vdev_config_enter`, `spa_vdev_enter`, `spa_vdev_exit`, `spa_config_update`, the config lock, the namespace lock, `spa_vdev_top_lock` or `spa_sync`; do not rerun.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|possible circular|deadlock` (expected none; a suspended-pool message from `failmode` is expected: count them, quote one). Keep zd loaded.

DEMO-F: the log removal by hand on the branch's module, with 20 s holds (Z is the tree whose module is loaded; T names the demo). The first handler holds a sync while `zpool remove` waits for the config lock; once it is removed, the second one holds the next sync, which the removal waits for after its first step:

```sh
D=/var/tmp/task-1122-sld6b; L=/var/log/task-1122-sld6b; Z=$D/zd; M=$D/demo; T=F
mkdir $M && truncate -s 256M $M/a $M/l $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld6o $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld6a $M/a log $M/l &&
h1=$(sudo -n $Z/zinject -q -w spa_sync:config -s 20 sld6a) && h2=$(sudo -n $Z/zinject -q -w spa_sync:config -s 20 sld6a) && echo "DEMO-$T handlers $h1 $h2" &&
{ sudo -n $Z/zpool sync sld6a & } &&
for i in $(seq 50); do sudo -n $Z/zinject | awk -v id=$h1 '$1 == id && $NF > 0 { f = 1 } END { exit !f }' && break; sleep 0.2; done && echo "DEMO-$T first sync held" &&
s0=$(date +%s%N) && { sudo -n $Z/zpool remove sld6a $M/l; echo "DEMO-$T remove rc=$? after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
pr=$!
sleep 2
{ sudo -n $Z/zinject -c $h1 > /dev/null; echo "DEMO-$T handler $h1 removed after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
sleep 2
s1=$(date +%s%N); { sudo -n $Z/zpool status sld6o > /dev/null; echo "DEMO-$T status of sld6o rc=$? in $(( ($(date +%s%N) - s1) / 1000000 )) ms"; } &
ps1=$!
sleep 2
for p in $(pgrep -f 'zpool (remove sld6a|status sld6o)'); do echo "== $p $(ps -o stat=,args= -p $p)"; sudo -n cat /proc/$p/stack; done
{ sudo -n $Z/zinject -c $h2 > /dev/null; echo "DEMO-$T handler $h2 removed after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
wait $ps1
wait $pr
sudo -n $Z/zpool status -P sld6a | grep -c "$M/l"
wait
sudo -n $Z/zinject -c all; sudo -n $Z/zpool destroy sld6a; sudo -n $Z/zpool destroy sld6o; sudo -n rm -rf $M && sudo -n fstrim -av
```

Run the whole block as one command. Expected: `DEMO-F first sync held`; `DEMO-F handler <h1> removed after` about 2000 ms; `DEMO-F status of sld6o rc=0 in N ms` with N below 1000, printed before the second handler is removed; the stack of the `zpool remove` process (not its `sudo`) waiting for its txg: `txg_wait_synced_flags` (or `cv_wait`) <- `txg_wait_synced` <- `spa_vdev_config_exit_impl` <- `spa_vdev_config_pause` <- `spa_vdev_remove_log` (any of the last two may be inlined) <- `spa_vdev_remove` <- `zfs_ioc_vdev_remove`; `DEMO-F handler <h2> removed after` about 6000 ms; `DEMO-F remove rc=0` shortly after; the grep count `0` (the log device is gone). Quote all of it.

## STEP 4: the parent's kernel files in the debug tree, fail-before, DEMO-B (about 25 minutes)

Check the seven kernel files of the change out of the parent in zd, check the tree, rebuild in place (zd's module stays loaded meanwhile):

```sh
cd /var/tmp/task-1122-sld6b/zd && git checkout b7728b1d8ed7dd0e49fb613b8f98cce848a98748 -- include/sys/spa.h include/sys/spa_impl.h module/zfs/spa.c module/zfs/spa_misc.c module/zfs/vdev_initialize.c module/zfs/vdev_removal.c module/zfs/vdev_trim.c &&
test "$(git write-tree)" = 166462d68344676e03f2c4bfbb469d6c55145ca9 && echo TREE-OK &&
printf '%s\n' "4cf656f11c367ecd1d798eacef8c50e334cc29159748ca23df168122f426956d  module/zfs/spa_misc.c" "8ec28441beebe4c135878d08df51efb583ac3f14914965c06c9b6d48de5f0587  module/zfs/vdev_removal.c" | sha256sum -c --quiet && echo SHA-OK &&
git diff --cached --name-status HEAD
```

Expected TREE-OK, SHA-OK (the spa_misc.c of step 7a v2, the one TASK-1122-SLD5B runs) and seven `M` lines: include/sys/spa.h, include/sys/spa_impl.h, module/zfs/spa.c, module/zfs/spa_misc.c, module/zfs/vdev_initialize.c, module/zfs/vdev_removal.c, module/zfs/vdev_trim.c. Then, detached and polled like a build: `setsid nohup sh -c "cd /var/tmp/task-1122-sld6b/zd && nice make -j4; echo build-rc=\$?" > /var/log/task-1122-sld6b/build-fb.log 2>&1 < /dev/null &`. It must end with `build-rc=0`; quote `grep -E 'spa_misc|vdev_removal|zfs\.ko' $L/build-fb.log` and every `warning:` line outside objtool notes. Then `cat /sys/module/zfs/srcversion` must still give zd's value of step 3, and `modinfo -F srcversion $D/zd/module/zfs.ko` must now differ from it. Unload with zd's `scripts/zfs.sh -u` and load zd again: the srcversion must now equal the new `modinfo` value, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

Fail-before fb: the pre-run checks, then one run (tag fb, in zd, `<sel>` = `-t vdev_change_step_wait`). Expected: FAIL in the first case (`zpool remove testpool1 .../log`), about 30 s after its first handler is removed, with `zinject still running after 30s` (the listing that waits for the second handler) or `zinject -c <id> still running after 30s` (the removal of the first handler, which takes the namespace lock once it has released the sync, if the removal of the log already holds it); the test's cleanup then removes the handlers in the background, the commands return within seconds (no `commands are blocked: pools left` note), the pools are destroyed, and the group's cleanup PASSes. The run takes about 2 minutes. Quote the failing lines, the 20 lines before them and the cleanup that follows. A fb run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so. If the runner is still going after 20 minutes, it is a hang (rules).

DEMO-B: the DEMO block of step 3 with `T=B` (same Z, same commands). Expected: `DEMO-B status of sld6o rc=0 in N ms` with N about 2000 or more: it returns only once the second handler is removed (its line may come before or after `DEMO-B handler <h2> removed after`, which also waits for the namespace lock; so may `DEMO-B handler <h1> removed after`); the stack of the `zpool status` process waiting for the namespace mutex in `spa_open_common` (via `spa_get_stats`) or `spa_all_configs`; the `zpool remove` stack in `txg_wait_synced` <- `spa_vdev_config_exit_impl` <- `spa_vdev_config_exit` <- `spa_vdev_remove_log` <- `spa_vdev_remove`; `DEMO-B remove rc=0`; the grep count `0`. Quote all of it.

After DEMO-B: no pool left, quote the new dmesg lines since N1 matching the pattern of step 3 (a `blocked for more than` line is not expected: every wait is shorter than the hung task timeout of step 0; quote any), unload with zd's `scripts/zfs.sh -u`, `sudo -n rm -rf $D/zd`, `sudo -n fstrim -av`.

## STEP 5: release runs (expected PASS)

Load zr ("(DEBUG mode)" must be absent). The new test 3 times (tag zr, in zr, `<sel>` = `-t vdev_change_step_wait`), with the same expectations and quotes as each zd run of step 3, item 1. After them: no pool left, the dmesg check since zr's N0, unload with zr's `scripts/zfs.sh -u`, `sudo -n rm -rf /var/tmp/task-1122-sld6b`, `ls -d /var/tmp/vdev_change_step_wait /var/tmp/vdev_change_load_wait 2>&1` (absent), `sudo -n fstrim -av`, `df -h /var/tmp`. Logs stay in /var/log/task-1122-sld6b.

## Report

Write /var/log/task-1122-sld6b/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, hung_task_timeout_secs, git version; TREES-OK, the two SHA-OK, the name-status lines and the test's mode; build rc and warnings of zd and zr; srcversions and DEBUG mode; step 4's TREE-OK, SHA-OK, name-status lines, build-fb rc and quoted lines, and the srcversion checks.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note. Then one verdict line, e.g. "spa-lock-detangle-7b-v2: vdev_change_step_wait PASS debug 3/3, release 3/3; groups replacement, removal, zpool_remove, zpool_split, zpool_set, zpool_add, zpool_attach, cache, l2arc, zinject, failmode, fault, trim, zpool_export, raidz PASS; DEMO-F status 12 ms; parent's kernel files: vdev_change_step_wait FAIL 1/1 (listing blocked 30 s in the log removal), DEMO-B status after the handler removal, no reboot".
3. Quotes: the NOTE and SUCCESS lines of step 3 item 1; the non-PASS lines of the groups; DEMO-F and DEMO-B outputs with the stacks; fb's failing lines and cleanup; the dmesg checks.
4. Deviations, cleanup, then End.
