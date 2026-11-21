TASK-1120-SLD5 (from the coordinator session "main"): functional, about 4 h 30 min builds included, no reboot expected. VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`, `M`, `T`).

## Why

Every vdev change (zpool add, attach, replace, detach, remove, split) starts in `spa_vdev_enter()` or, for a detach, `spa_vdev_detach_enter()` (module/zfs/spa_misc.c), which take `spa_namespace_lock` and then the config lock as writer, and ends in `spa_vdev_exit()`, which waits for the change's txg to sync before it releases the namespace lock. The config lock writer waits for `spa_sync()`, which holds the config lock as reader for a whole txg, and the final wait lasts as long as the sync. On a pool whose sync is stalled on a device that does not answer, both waits hold the namespace lock for as long as the device hangs, so every `spa_open()` on the system (zpool status of any pool, zpool clear, zinject) waits too. The fix waits for the config lock without the namespace lock (then only tries to take the namespace lock, and starts over if it is busy), and waits for the final txg and frees the detached vdev without it.

A new zinject -w point, `spa_sync:config`, holds the sync of a txg while it holds the config lock as reader, as a sync stalled on a hung device does. The new test `vdev_change_sync_wait` (replacement group) registers two handlers there, makes the pool sync (the first holds it), runs `zpool attach` in the background, waits until `zpool status` of that pool blocks (the attach then waits for the config lock as writer, and new readers of that pool wait behind it: expected, not changed), and checks that `zpool status` of another pool and the zinject handler listing (which takes the namespace lock) return within 30 s. It then removes the first handler; the second holds the next sync while the attach waits for its txg, and `zpool status` of both pools and the listing must return within 30 s; then it removes the second handler and the attach must succeed. The same again with `zpool detach` of the new vdev. Without the fix, the attach holds the namespace lock, the status of the other pool blocks, the test fails after 30 s, and its cleanup removes the handlers (removal releases the held sync before it waits for the namespace lock), so the fail-before ends by itself: no reboot.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-lock-detangle-7`, head `a021401b969a707bea960531ae6335340dadbe20`, tree `bcf1d0347f01c9064a85251057a5dbc65e60b56c`. Its parent `411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59` is step 5 ("Stop autotrim and rebuild threads before the namespace lock", TASK-1118-SLD3), on step 4 `af7f0c6e8d3f780f71db8be71472d4edc4bb744d` (same patch as #19968), on `77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d` (branch `zinject-delay-points-v2`: the zinject -w infrastructure, 4 commits on upstream master `c913da86156`). The fail-before is the head with only `module/zfs/spa_misc.c` taken from the parent (tree `2f15f903cf171dc1aa913f1f5a84556562770158`): the point and the test stay.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject` says no handlers are registered, and `ls -d /var/tmp/vdev_change_sync_wait /var/tmp/task-1120-sld5/demo 2>&1` says neither exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A ZTS run still going after 2 hours (a group) or 20 minutes (a single test), or a build after 90 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, `sudo -n cat /proc/<pid>/stack` of each `zpool` and `zinject` process, and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main (the standing reboot approval covers a wedged module only after these captures).
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `cat /proc/sys/kernel/hung_task_timeout_secs` (quote; do not change it), `id zts`, `command -v ksh git`, `git --version`, `ls -d /var/tmp/task-1120-sld5 /var/log/task-1120-sld5 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/vdev_change_sync_wait 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1120-sld5; L=/var/log/task-1120-sld5; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 4 $U spa-lock-detangle-7 && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD^) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd a021401b969a707bea960531ae6335340dadbe20 411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59 af7f0c6e8d3f780f71db8be71472d4edc4bb744d 77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d bcf1d0347f01c9064a85251057a5dbc65e60b56c 0" "zr a021401b969a707bea960531ae6335340dadbe20 411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59 af7f0c6e8d3f780f71db8be71472d4edc4bb744d 77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d bcf1d0347f01c9064a85251057a5dbc65e60b56c 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
2eba4b734a59fbd73c7bac921c09f89ec0bbda999c862cc2023c08a1907f27d5  include/sys/zio.h
56c2120de4868df2a358bf66ae7445b40d6372fede871794dbc30c37035737f2  man/man8/zinject.8
f9d8bccea323c246345c635337f264b2ac5d46b27910b54afbd4ede3aa887e7c  module/zfs/spa.c
7fea37fd2deed1f1284802ea795c49494d24f41528451072928d28813e697382  module/zfs/spa_misc.c
d4c37e54adf406c0349f7241c6e3f2b49c8832b01631cc21a437f7b978824039  module/zfs/zio_inject.c
18240a2381f32851a186d30bcbc3ffe1d619d4371bf0f825b5b2b5cea9f5b77d  tests/runfiles/common.run
fe83c7b729437190cabfe4ff4418a1c065bf94a90977fd01d8165d84b825fb3e  tests/zfs-tests/tests/Makefile.am
986a30d43c1f6a3d3be106d6daf537ff8e51099d60c58c84922bf8ca7d0b25c1  tests/zfs-tests/tests/functional/cli_root/zinject/zinject_args.ksh
989fd7cee500e424c626ce5d81536578c9475ca863b40b4391abc4a7556260aa  tests/zfs-tests/tests/functional/replacement/vdev_change_sync_wait.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD^ HEAD && ls -l tests/zfs-tests/tests/functional/replacement/vdev_change_sync_wait.ksh
```

Expected TREES-OK, two SHA-OK lines, nine name-status lines (`A` for the new test, `M` for the eight other files above) and mode `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1120-sld5; L=/var/log/task-1120-sld5
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror).

## STEP 3: debug runs (all expected PASS), then DEMO-F

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1120-sld5/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1120-sld5/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it.

1. The new test, 3 runs (tag zd, `<sel>` = `-t vdev_change_sync_wait`). Expected per run: the replacement group's `setup` PASS, `vdev_change_sync_wait` PASS in under a minute, `cleanup` PASS. From the merged log of each run quote the two `NOTE: zpool attach ... returned 0: ...` and `NOTE: zpool detach ... returned 0: ...` lines; from one run also, in order, the `bounded` SUCCESS lines of the attach part (`zpool status testpool3`, `zinject -c <id>`, `zpool status testpool3`, `zpool status testpool1`, `zinject -c <id>`) and the five of the detach part.
2. Groups, 1 run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `replacement`, `zinject`, `failmode`, `zpool_add`, `zpool_attach`, `zpool_detach`, `zpool_replace`, `zpool_split`, `zpool_remove`, `removal`, `cache`, `fault`, `raidz`. `raidz` (RAID-Z expansion, an attach) is the longest, about an hour on debug: run it last. Expected: all PASS (or SKIP where the same group skips on master: say which, with the skip reason). Known on master, not from this branch: `zinject_counts` in `zinject` may FAIL with `check_count_freq: freq=100 match=98 inject=97` (counter race, M-31); `auto_spare_002_pos` in `fault` may FAIL in its anymirror cases at `wait_vdev_state ... DEGRADED 60` (TASK-1120-ASP). `replacement` includes both `vdev_change_sync_wait` and `vdev_change_thread_wait` (step 5); `zinject` runs `zinject_args`, which now registers the new point; `failmode` includes `failmode_vdev_detach`, whose path (the final wait of a detach on a suspended pool) this branch changes. For any other result, quote it as above and say whether its log involves `spa_vdev_enter`, `spa_vdev_exit`, the config lock, the namespace lock, `spa_vdev_top_lock` or `spa_sync`; do not rerun.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|possible circular|deadlock` (expected none; a suspended-pool message from `failmode` is expected: count them, quote one). Keep zd loaded.

DEMO-F: the attach case by hand on the branch's module, with a 20 s hold (Z is the tree whose module is loaded; T names the demo):

```sh
D=/var/tmp/task-1120-sld5; L=/var/log/task-1120-sld5; Z=$D/zd; M=$D/demo; T=F
mkdir $M && truncate -s 256M $M/a $M/b $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld5o $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld5a $M/a &&
id=$(sudo -n $Z/zinject -q -w spa_sync:config -s 20 sld5a) && echo "DEMO-$T handler $id" &&
{ sudo -n $Z/zpool sync sld5a & } &&
for i in $(seq 50); do sudo -n $Z/zinject | awk -v id=$id '$1 == id && $NF > 0 { f = 1 } END { exit !f }' && break; sleep 0.2; done && echo "DEMO-$T sync held" &&
s0=$(date +%s%N) && { sudo -n $Z/zpool attach sld5a $M/a $M/b; echo "DEMO-$T attach rc=$? after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
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

Run the whole block as one command. Expected: `DEMO-F sync held`; `DEMO-F status of sld5o rc=0 in N ms` with N below 1000, printed before the handler is removed; the stack of the `zpool attach` process (not its `sudo`) waiting for the config lock: `spa_config_enter_impl` (or `cv_wait`) <- `spa_config_enter` <- `spa_vdev_enter_locks` (may be inlined) <- `spa_vdev_enter` <- `spa_vdev_attach` <- `zfs_ioc_vdev_attach`; `DEMO-F handler removed after` about 4000 ms; `DEMO-F attach rc=0` shortly after; the grep count `1`. Quote all of it.

## STEP 4: the parent's spa_misc.c in the debug tree, fail-before, DEMO-B (about 25 minutes)

Check the file out of the parent in zd, check the tree, rebuild in place (zd's module stays loaded meanwhile):

```sh
cd /var/tmp/task-1120-sld5/zd && git checkout 411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59 -- module/zfs/spa_misc.c &&
test "$(git write-tree)" = 2f15f903cf171dc1aa913f1f5a84556562770158 && echo TREE-OK &&
echo "4d098af076c8a62eb8bd6973c04c940e06792d64b9518dd613e389bde8f0aacc  module/zfs/spa_misc.c" | sha256sum -c --quiet && echo SHA-OK &&
git diff --cached --name-status HEAD
```

Expected TREE-OK, SHA-OK and the single line `M	module/zfs/spa_misc.c`. Then, detached and polled like a build: `setsid nohup sh -c "cd /var/tmp/task-1120-sld5/zd && nice make -j4; echo build-rc=\$?" > /var/log/task-1120-sld5/build-fb.log 2>&1 < /dev/null &`. It must end with `build-rc=0`; quote `grep -E 'spa_misc|zfs\.ko' $L/build-fb.log` and every `warning:` line outside objtool notes. Then `cat /sys/module/zfs/srcversion` must still give zd's value of step 3, and `modinfo -F srcversion $D/zd/module/zfs.ko` must now differ from it. Unload with zd's `scripts/zfs.sh -u` and load zd again: the srcversion must now equal the new `modinfo` value, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

Fail-before fb: the pre-run checks, then one run (tag fb, in zd, `<sel>` = `-t vdev_change_sync_wait`). Expected: FAIL in the attach part, about 35 s after `zpool attach` starts, with `zpool status testpool3 still running after 30s`; the test's cleanup then removes the two handlers in the background, the commands return within seconds (no `commands are blocked: pools left` note), the pools are destroyed, and the group's cleanup PASSes. The run takes about 2 minutes. Quote the failing lines, the 20 lines before them and the cleanup that follows. A fb run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so. If the runner is still going after 20 minutes, it is a hang (rules).

DEMO-B: the DEMO block of step 3 with `T=B` (same Z, same commands). Expected: `DEMO-B status of sld5o rc=0 in N ms` with N about 2000 or more: it returns only once the handler is removed and the attach has finished (its line may come before or after `DEMO-B handler removed after`, which also waits for the namespace lock); the stack of the `zpool status` process waiting for the namespace mutex in `spa_open_common` (via `spa_get_stats`) or `spa_all_configs`; the `zpool attach` stack in `spa_config_enter` <- `spa_vdev_config_enter` <- `spa_vdev_enter` <- `spa_vdev_attach`; `DEMO-B attach rc=0`; the grep count `1`. Quote all of it.

After DEMO-B: no pool left, quote the new dmesg lines since N1 matching the pattern of step 3 (a `blocked for more than` line is not expected: every wait is shorter than the hung task timeout of step 0; quote any), unload with zd's `scripts/zfs.sh -u`, `sudo -n rm -rf $D/zd`, `sudo -n fstrim -av`.

## STEP 5: release runs (expected PASS)

Load zr ("(DEBUG mode)" must be absent). The new test 3 times (tag zr, in zr, `<sel>` = `-t vdev_change_sync_wait`), with the same expectations and quotes as each zd run of step 3, item 1. After them: no pool left, the dmesg check since zr's N0, unload with zr's `scripts/zfs.sh -u`, `sudo -n rm -rf /var/tmp/task-1120-sld5`, `ls -d /var/tmp/vdev_change_sync_wait 2>&1` (absent), `sudo -n fstrim -av`, `df -h /var/tmp`. Logs stay in /var/log/task-1120-sld5.

## Report

Write /var/log/task-1120-sld5/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, hung_task_timeout_secs, git version; TREES-OK, the two SHA-OK, the name-status lines and the test's mode; build rc and warnings of zd and zr; srcversions and DEBUG mode; step 4's TREE-OK, SHA-OK, name-status line, build-fb rc and quoted lines, and the srcversion checks.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note. Then one verdict line, e.g. "spa-lock-detangle-7: vdev_change_sync_wait PASS debug 3/3, release 3/3; groups replacement, zinject, failmode, zpool_add, zpool_attach, zpool_detach, zpool_replace, zpool_split, zpool_remove, removal, cache, fault, raidz PASS; DEMO-F status 12 ms; parent's spa_misc.c: vdev_change_sync_wait FAIL 1/1 (status of another pool blocked 30 s), DEMO-B status after the handler removal, no reboot".
3. Quotes: the NOTE and SUCCESS lines of step 3 item 1; the non-PASS lines of the groups; DEMO-F and DEMO-B outputs with the stacks; fb's failing lines and cleanup; the dmesg checks.
4. Deviations, cleanup, then End.
