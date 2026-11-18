TASK-1118-SLD3 (from the coordinator session "main"): functional, about 3 h 30 min builds included, no reboot expected. VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`, `M`, `T`).

## Why

Every vdev change (zpool add, attach, replace, detach, remove, split) starts in `spa_vdev_enter()` or, for a detach, `spa_vdev_detach_enter()` (module/zfs/spa_misc.c). On master both take `spa_vdev_top_lock`, then `spa_namespace_lock`, and then wait, with both held, for the threads they stop: the autotrim thread of each top-level vdev and, in a detach, the rebuild thread. A thread asked to stop waits for its I/O, for txg syncs and for a config lock reader on its way out; meanwhile every `spa_open()` on the system (zpool status of any pool, zpool clear, zinject) waits for the namespace lock. The fix stops the threads with only `spa_vdev_top_lock` held and takes the namespace lock afterwards; `vdev_rebuild_restart()` now asserts `spa_vdev_top_lock` and `vdev_rebuild_stop_wait()` accepts it.

Two new zinject -w points, `vdev_autotrim_thread:exit` and `vdev_rebuild_thread:exit`, hold a stopping thread after its last I/O. The new test `vdev_change_thread_wait` (replacement group) holds the autotrim thread while `zpool add` waits for it, then the autotrim and the rebuild threads while `zpool detach` waits for them (rebuild progress stopped with `zfs_scan_suspend_progress`, so only the detach ends the rebuild). Each time, the zinject handler listing (which takes the namespace lock) and `zpool status` of the pool and of another pool must return within 30 s while the command still waits; then the command must succeed. The hold time is 100 s: without the fix the listing returns only when it runs out, the test fails, and its cleanup waits for the hold to end, so the fail-before needs no reboot.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-lock-detangle-3`, head `411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59`, tree `8c09c8ea6c1e86ee6bbfb6460b6f35ff1be78cc2`. Its parent `af7f0c6e8d3f780f71db8be71472d4edc4bb744d` is step 4 ("Start and stop initialize and TRIM under spa_vdev_top_lock", the same patch as `30dc2819f2d`), on `77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d` (branch `zinject-delay-points-v2`: the zinject -w infrastructure, 4 commits on upstream master `c913da86156`). The fail-before is the head with only `module/zfs/spa_misc.c` taken from the parent (tree `50ed09abbe5f6fe932a15aebf87653b98cf32e2b`): the points and the test stay.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject` says no handlers are registered, and `ls -d /var/tmp/vdev_change_thread_wait /var/tmp/task-1118-sld3/demo 2>&1` says neither exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A ZTS run still going after 2 hours (a group) or 20 minutes (a single test), or a build after 90 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, `sudo -n cat /proc/<pid>/stack` of each `zpool` and `zinject` process, and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `cat /proc/sys/kernel/hung_task_timeout_secs` (quote; do not change it), `id zts`, `command -v ksh git`, `git --version`, `ls -d /var/tmp/task-1118-sld3 /var/log/task-1118-sld3 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/vdev_change_thread_wait 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1118-sld3; L=/var/log/task-1118-sld3; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 3 $U spa-lock-detangle-3 && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD^) $(git rev-parse HEAD~2) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd 411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59 af7f0c6e8d3f780f71db8be71472d4edc4bb744d 77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d 8c09c8ea6c1e86ee6bbfb6460b6f35ff1be78cc2 0" "zr 411f3ff06d4534e7fc7a6ec64b2bf8b957af8c59 af7f0c6e8d3f780f71db8be71472d4edc4bb744d 77c5e77d5efdf4dedd83452cf8cc7b798a5ea02d 8c09c8ea6c1e86ee6bbfb6460b6f35ff1be78cc2 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
4d098af076c8a62eb8bd6973c04c940e06792d64b9518dd613e389bde8f0aacc  module/zfs/spa_misc.c
50e2097dfec87ecec42e78b4ac59658ada7458be084ce06b8cd91746bc72951b  module/zfs/vdev_rebuild.c
111b602b959e2f9e7d2cffead0c137e9f75447729008f4ae0bddaec4552fcfc0  module/zfs/vdev_trim.c
570e2e29cd4123846a874b525b5e0deb7e0c55855a0f1d12007a9c94eadd81af  module/zfs/zio_inject.c
e0f741cba7ff571d694d21539032242ce81261bf5560a833a24baa9241b40c75  include/sys/zio.h
18e63d62739a2cfc9f319b0dc88604928a4e3e16a4d06307b5aee618a7d6b3bc  man/man8/zinject.8
03c7019b05b5f8265cf4111e7fdfe1700d2cafde0c27461055218d0bd1fad195  tests/runfiles/common.run
19f6bcb01fb8de9d42bdf8efb1c53da3fa2c1a1b198a1b0b39db136d6aee10c3  tests/zfs-tests/tests/Makefile.am
f9a90314c8d875698f3983090b72cbcb50a2dada9ff3555f863b302836a8cfa7  tests/zfs-tests/tests/functional/cli_root/zinject/zinject_args.ksh
637ca93dae1e175e6e23ef8b7fc113efae267e42a6a0ecad580b770ad8e35520  tests/zfs-tests/tests/functional/replacement/vdev_change_thread_wait.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD^ HEAD && ls -l tests/zfs-tests/tests/functional/replacement/vdev_change_thread_wait.ksh
```

Expected TREES-OK, two SHA-OK lines, ten name-status lines (`A` for the new test, `M` for the nine other files above) and mode `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1118-sld3; L=/var/log/task-1118-sld3
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror).

## STEP 3: debug runs (all expected PASS), then DEMO-F

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1118-sld3/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1118-sld3/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it.

1. The new test, 3 runs (tag zd, `<sel>` = `-t vdev_change_thread_wait`). Expected per run: the replacement group's `setup` PASS, `vdev_change_thread_wait` PASS in under a minute, `cleanup` PASS. From the merged log of each run quote the two `NOTE: the vdev change returned 0: ...` lines; from one run also the SUCCESS lines of `bounded zpool status testpool3`, `bounded zpool status testpool1`, `bounded zinject -c <id>` (add part) and the six `bounded` SUCCESS lines of the detach part (status of testpool3, status of testpool2, `zinject -c`, once for each held thread), in order.
2. Groups, 1 run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `replacement`, `zinject`, `failmode`, `zpool_add`, `zpool_attach`, `zpool_detach`, `zpool_split`, `removal`, `zpool_initialize`, `zpool_trim`, `trim`. Expected: all PASS (or SKIP where the same group skips on master: say which, with the skip reason). `replacement` includes `vdev_change_thread_wait`; `zinject` runs `zinject_args`, which now registers the two new points; `failmode` includes `failmode_initialize_trim` (step 4). For any other result, quote it as above and say whether its log involves `spa_vdev_enter`, `spa_vdev_detach_enter`, autotrim, rebuild, the namespace lock or `spa_vdev_top_lock`; do not rerun.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|possible circular|deadlock` (expected none). Keep zd loaded.

DEMO-F: the add case by hand on the branch's module, with a 20 s hold (Z is the tree whose module is loaded; T names the demo):

```sh
D=/var/tmp/task-1118-sld3; L=/var/log/task-1118-sld3; Z=$D/zd; M=$D/demo; T=F
mkdir $M && truncate -s 256M $M/a $M/b $M/o &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld3o $M/o &&
sudo -n $Z/zpool create -o cachefile=none -o autotrim=on -O mountpoint=none sld3a $M/a &&
sudo -n $Z/zpool export sld3a && sudo -n $Z/zpool import -o cachefile=none -d $M sld3a &&
id=$(sudo -n $Z/zinject -q -w vdev_autotrim_thread:exit -s 20 sld3a) && echo "DEMO-$T handler $id" &&
s0=$(date +%s%N) && { sudo -n $Z/zpool add -f sld3a $M/b; echo "DEMO-$T add rc=$? after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
pa=$!
sleep 2
s1=$(date +%s%N); { sudo -n $Z/zpool status sld3o > /dev/null; echo "DEMO-$T status of sld3o rc=$? in $(( ($(date +%s%N) - s1) / 1000000 )) ms"; } &
ps1=$!
sleep 2
for p in $(pgrep -f 'zpool (add -f sld3a|status sld3o)'); do echo "== $p $(ps -o stat=,args= -p $p)"; sudo -n cat /proc/$p/stack; done
wait $ps1
sudo -n $Z/zinject -c $id
wait $pa
sudo -n $Z/zpool status -P sld3a | grep -c "$M/b"
sudo -n $Z/zinject -c all; sudo -n $Z/zpool destroy sld3a; sudo -n $Z/zpool destroy sld3o; sudo -n rm -rf $M && sudo -n fstrim -av
```

Run the whole block as one command. Expected: `DEMO-F status of sld3o rc=0 in N ms` with N below 1000; the stack of the `zpool add` process (not its `sudo`) in `vdev_autotrim_stop_wait` <- `vdev_autotrim_stop_all` <- `spa_vdev_enter` <- `spa_vdev_add`; `DEMO-F add rc=0` once the handler is removed, a little over 4 s after it started; the grep count `1`. Quote all of it.

## STEP 4: the parent's spa_misc.c in the debug tree, fail-before, DEMO-B (about 25 minutes)

Check the file out of the parent in zd, check the tree, rebuild in place (zd's module stays loaded meanwhile):

```sh
cd /var/tmp/task-1118-sld3/zd && git checkout af7f0c6e8d3f780f71db8be71472d4edc4bb744d -- module/zfs/spa_misc.c &&
test "$(git write-tree)" = 50ed09abbe5f6fe932a15aebf87653b98cf32e2b && echo TREE-OK &&
echo "85d49ddbb567e460411a446504213d2275fbf46ce099cf1f8fc48c5b399bbf35  module/zfs/spa_misc.c" | sha256sum -c --quiet && echo SHA-OK &&
git diff --cached --name-status HEAD
```

Expected TREE-OK, SHA-OK and the single line `M	module/zfs/spa_misc.c`. Then, detached and polled like a build: `setsid nohup sh -c "cd /var/tmp/task-1118-sld3/zd && nice make -j4; echo build-rc=\$?" > /var/log/task-1118-sld3/build-fb.log 2>&1 < /dev/null &`. It must end with `build-rc=0`; quote `grep -E 'spa_misc|zfs\.ko' $L/build-fb.log` and every `warning:` line outside objtool notes. Then `cat /sys/module/zfs/srcversion` must still give zd's value of step 3, and `modinfo -F srcversion $D/zd/module/zfs.ko` must now differ from it. Unload with zd's `scripts/zfs.sh -u` and load zd again: the srcversion must now equal the new `modinfo` value, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

Fail-before fb: the pre-run checks, then one run (tag fb, in zd, `<sel>` = `-t vdev_change_thread_wait`). Expected: FAIL in the add part, about 30 s after `zpool add` starts, with `zinject still running after 30s`; the test's cleanup then waits until the 100 s hold has run out (no `commands are blocked: pools left` note), clears the handlers and destroys the pools, and the group's cleanup PASSes. The run takes about 2 to 3 minutes. Quote the failing lines, the 20 lines before them and the cleanup that follows. A fb run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so. If the runner is still going after 20 minutes, it is a hang (rules).

DEMO-B: the DEMO block of step 3 with `T=B` (same Z, same commands). Expected: `DEMO-B status of sld3o rc=0 in N ms` with N about 18000 (the rest of the 20 s hold); the stack of the `zpool status` process waiting for the namespace mutex in `spa_open_common` (via `spa_get_stats`, or `spa_all_configs`); the `zpool add` stack in `vdev_autotrim_stop_wait` <- `vdev_autotrim_stop_all` <- `spa_vdev_enter`, as in DEMO-F; `DEMO-B add rc=0` about 20 s after it started; the grep count `1`. Quote all of it.

After DEMO-B: no pool left, quote the new dmesg lines since N1 matching the pattern of step 3 (a `blocked for more than` line is not expected: every wait is shorter than the hung task timeout of step 0; quote any), unload with zd's `scripts/zfs.sh -u`, `sudo -n rm -rf $D/zd`, `sudo -n fstrim -av`.

## STEP 5: release run (expected PASS)

Load zr ("(DEBUG mode)" must be absent). One run (tag zr, in zr, `<sel>` = `-t vdev_change_thread_wait`), with the same expectations and quotes as each zd run of step 3, item 1. After it: no pool left, the dmesg check since zr's N0, unload with zr's `scripts/zfs.sh -u`, `sudo -n rm -rf /var/tmp/task-1118-sld3`, `ls -d /var/tmp/vdev_change_thread_wait 2>&1` (absent), `sudo -n fstrim -av`, `df -h /var/tmp`. Logs stay in /var/log/task-1118-sld3.

## Report

Write /var/log/task-1118-sld3/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, hung_task_timeout_secs, git version; TREES-OK, the two SHA-OK, the name-status lines and the test's mode; build rc and warnings of zd and zr; srcversions and DEBUG mode; step 4's TREE-OK, SHA-OK, name-status line, build-fb rc and quoted lines, and the srcversion checks.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note. Then one verdict line, e.g. "spa-lock-detangle-3: vdev_change_thread_wait PASS debug 3/3, release 1/1; groups replacement, zinject, failmode, zpool_add, zpool_attach, zpool_detach, zpool_split, removal, zpool_initialize, zpool_trim, trim PASS; DEMO-F status 12 ms; parent's spa_misc.c: vdev_change_thread_wait FAIL 1/1 (zinject listing blocked 30 s), DEMO-B status 18 s, no reboot".
3. Quotes: the NOTE and SUCCESS lines of step 3 item 1; the non-PASS lines of the groups; DEMO-F and DEMO-B outputs with the stacks; fb's failing lines and cleanup; the dmesg checks.
4. Deviations, cleanup, then End.
