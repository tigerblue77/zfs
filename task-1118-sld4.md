TASK-1118-SLD4 (from the coordinator session "main"): functional, about 4 h builds included, one reboot at the end (step 5). VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`, `M`, `G`).

## Why

`zpool reguid` runs `spa_change_guid()` (module/zfs/spa.c). On master it holds `spa_namespace_lock` across its `dsl_sync_task()`, i.e. across the txg sync that writes the new guid. If the pool suspends in that sync, the sync completes only once `zpool clear` resumes the pool, but `zpool clear` needs the namespace lock first; so do `zinject`, `zinject -c` and every `spa_open()` on the system: `zpool status` of every pool blocks, and only a reboot ends it. The fix checks the new guid and reserves it under the lock (new field `spa_pending_guid`, which `spa_by_guid()`, and so `spa_guid_exists()` and the in-use check of a device, match like the pool's guid until the change has synced), drops the lock for the sync task, and takes it again to release the reservation and write the cache file.

The new test `failmode_reguid` (failmode group): a reguid of a degraded mirror to the guid G=7208386112437612741 must fail (and give G back); then every write and probe of a pool's only vdev fails and `zpool reguid -g G testpool` runs in the background until the pool suspends. While it waits, the zinject handler listing (which takes the namespace lock), `zpool status` of testpool1 and of testpool must return within 60 s, and `zpool reguid -g G testpool1` must fail; then `zinject -c all` and `zpool clear testpool` must return, the reguid must succeed, testpool must have G (also after an export and import) and testpool1 its old guid. Without the fix, the listing blocks for good: the test fails after 60 s and the namespace lock stays held, so step 5 ends with a reboot.

What is tested: fork https://github.com/tigerblue77/zfs, branch `spa-lock-detangle-4`, head `c3e4b6778f87ca2ffe8a54cea649c6a8237bf3de`, tree `1ad4e5877eee79776e7398e288d97e04d741bd0f`, one commit on upstream master `c913da86156eba3243cc1ef515e98d69887654a7`. The fail-before is the head with only `module/zfs/spa.c` taken from master (tree `0753658118378d67b75d550f34afe7d38811e71b`): the field, the lookup and the test stay.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject` says no handlers are registered, and `ls -d /var/tmp/failmode_reguid /var/tmp/task-1118-sld4/demo 2>&1` says neither exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A ZTS run still going after 2 hours (a group) or 20 minutes (a single test), or a build after 90 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, `sudo -n cat /proc/<pid>/stack` of each `zpool` and `zinject` process, and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main. Step 5 is the exception: its blocked commands are expected, and its reboot is allowed.
- Rebooting this VM is allowed for this task, once, in step 5. Before it, arrange your own resumption the way VM 105 did in TASK-1117-DB (a scheduled task of your own session), say which, and write in /var/log/task-1118-sld4/progress.md where you are, so that you resume at step 6. `sudo -n systemctl reboot`; the stuck `zpool` processes are in D state and may hold the shutdown: if the VM is still up 5 minutes later, `echo s | sudo -n tee /proc/sysrq-trigger; sleep 5; echo b | sudo -n tee /proc/sysrq-trigger`, and say that you had to.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules not loaded at the end (after the reboot, they are not).

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `cat /proc/sys/kernel/hung_task_timeout_secs` (quote; do not change it), `id zts`, `command -v ksh git`, `git --version`, `ls -d /var/tmp/task-1118-sld4 /var/log/task-1118-sld4 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/failmode_reguid 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1118-sld4; L=/var/log/task-1118-sld4; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 2 $U spa-lock-detangle-4 && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD^) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd c3e4b6778f87ca2ffe8a54cea649c6a8237bf3de c913da86156eba3243cc1ef515e98d69887654a7 1ad4e5877eee79776e7398e288d97e04d741bd0f 0" "zr c3e4b6778f87ca2ffe8a54cea649c6a8237bf3de c913da86156eba3243cc1ef515e98d69887654a7 1ad4e5877eee79776e7398e288d97e04d741bd0f 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
f8fa778756e930fa0f328f66e1a22d0d35d0ddf412c6c61534c84a39f7901b4a  include/sys/spa_impl.h
496e0bb886e02b72f25ea3e15123e6a85c688db05e0a7006b834cd914ebca481  module/zfs/spa.c
e2f62ea4029ea1960126bf4b81bcfb168876dd47f4ca3e90f2b2b2a3456a2373  module/zfs/spa_misc.c
09ebf04530a2036a3c3eba1c111a7e70c5c3ec91fab2e5d5617f8636ea9d70cb  tests/runfiles/common.run
6d9d0a3635b54788d5d56b87ebad100dc98da7adf9ce7d2138490e954cc8905a  tests/zfs-tests/tests/Makefile.am
5870931e155ca603b6756a52e1a37baf78d6d5365e967192ac2a9b08c130db9e  tests/zfs-tests/tests/functional/failmode/failmode_reguid.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD^ HEAD && ls -l tests/zfs-tests/tests/functional/failmode/failmode_reguid.ksh
```

Expected TREES-OK, two SHA-OK lines, six name-status lines (`A` for the new test, `M` for the five other files above) and mode `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1118-sld4; L=/var/log/task-1118-sld4
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror).

## STEP 3: debug runs (all expected PASS), then DEMO-F

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1118-sld4/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1118-sld4/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it.

1. The new test, 3 runs (tag zd, `<sel>` = `-t failmode_reguid`). Expected per run: the failmode group's `setup` PASS, `failmode_reguid` PASS in under a minute, `cleanup` PASS. From the merged log of each run quote the `NOTE: zpool reguid returned 0: ...` line; from one run also, in order, the SUCCESS lines of `zpool reguid -g 7208386112437612741 testpool2` (exited 1), `bounded zinject`, `bounded zpool status testpool1`, `bounded zpool status testpool`, `bounded zpool reguid -g 7208386112437612741 testpool1` (exited 1), `bounded zinject -c all`, `bounded zpool clear testpool`, and the guid checks after it.
2. Groups, 1 run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `failmode`, `zpool_reguid`, `zpool_export`, `zpool_import`, `cachefile`, `inuse`, `zpool_add`, `zpool_split`, `pool_checkpoint`, `getfsuuid`. Expected: all PASS (or SKIP where the same group skips on master: say which, with the skip reason). `failmode` includes `failmode_reguid`; `zpool_import` includes `zpool_import_reguid_stale`, `zpool_export` includes `zpool_export_parallel_admin` (a reguid racing an export), `getfsuuid` includes `getfsuuid_002_pos` (a reguid), `inuse` and `zpool_add` exercise the in-use check of a device, which now goes through the changed `spa_by_guid()`. For any other result, quote it as above and say whether its log involves reguid, a pool guid, `spa_guid_exists`, the namespace lock or `spa_vdev_top_lock`; do not rerun.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|possible circular|deadlock` (the test's own "has been suspended" and "is being resumed" pool messages are expected: count them, quote one of each). Keep zd loaded.

DEMO-F: the test's middle part by hand on the branch's module, with the stack of the waiting reguid:

```sh
D=/var/tmp/task-1118-sld4; L=/var/log/task-1118-sld4; Z=$D/zd; M=$D/demo; G=7208386112437612741
mkdir $M && truncate -s 256M $M/a $M/o &&
sudo -n $Z/zpool create -o cachefile=none -o failmode=wait -O mountpoint=none sld4a $M/a &&
sudo -n $Z/zpool create -o cachefile=none -O mountpoint=none sld4o $M/o &&
o=$(sudo -n $Z/zpool get -Hpo value guid sld4o) && sudo -n $Z/zpool sync sld4a sld4o &&
sudo -n $Z/zinject -d $M/a -e io -T write sld4a && sudo -n $Z/zinject -d $M/a -e nxio -T probe sld4a &&
s0=$(date +%s%N) && { sudo -n $Z/zpool reguid -g $G sld4a; echo "DEMO-F reguid rc=$? after $(( ($(date +%s%N) - s0) / 1000000 )) ms"; } &
pr=$!
for i in $(seq 60); do [ "$(cat /proc/spl/kstat/zfs/sld4a/state)" = SUSPENDED ] && break; sleep 1; done; echo "DEMO-F state $(cat /proc/spl/kstat/zfs/sld4a/state) after ${i} s"
sleep 2
s1=$(date +%s%N); sudo -n $Z/zpool status sld4o > /dev/null; echo "DEMO-F status of sld4o rc=$? in $(( ($(date +%s%N) - s1) / 1000000 )) ms"
s1=$(date +%s%N); n=$(sudo -n $Z/zinject | grep -c sld4a); echo "DEMO-F zinject listing: $n handlers in $(( ($(date +%s%N) - s1) / 1000000 )) ms"
s1=$(date +%s%N); sudo -n $Z/zpool reguid -g $G sld4o; echo "DEMO-F reguid of sld4o to the same guid rc=$? in $(( ($(date +%s%N) - s1) / 1000000 )) ms"
for p in $(pgrep -f "zpool reguid -g $G sld4a"); do echo "== $p $(ps -o stat=,args= -p $p)"; sudo -n cat /proc/$p/stack; done
sudo -n $Z/zinject -c all; sudo -n $Z/zpool clear sld4a
wait $pr
echo "DEMO-F guid $(sudo -n $Z/zpool get -Hpo value guid sld4a) other $(sudo -n $Z/zpool get -Hpo value guid sld4o) (was $o) state $(cat /proc/spl/kstat/zfs/sld4a/state)"
sudo -n $Z/zpool destroy sld4a; sudo -n $Z/zpool destroy sld4o; sudo -n rm -rf $M && sudo -n fstrim -av
```

Run the whole block as one command. Expected: `DEMO-F state SUSPENDED after 1 s` (or 2); `DEMO-F status of sld4o rc=0` and the listing (2 handlers) each in well under 1000 ms; the second reguid `cannot reguid 'sld4o': ...` with rc=1, also at once; the stack of the `zpool reguid` process (not its `sudo`) in `txg_wait_synced_flags` <- `dsl_sync_task_common` <- `dsl_sync_task` <- `spa_change_guid` <- `zfs_ioc_pool_reguid`; `DEMO-F reguid rc=0` once the pool is cleared; then the guid line with sld4a at 7208386112437612741, sld4o at its old value and state ONLINE. Quote all of it.

Unload zd with its `scripts/zfs.sh -u`.

## STEP 4: release runs (expected PASS), with the fail-before build of zd meanwhile

Check the master file out in zd and rebuild it in place, detached (zr's runs go on meanwhile):

```sh
cd /var/tmp/task-1118-sld4/zd && git checkout c913da86156eba3243cc1ef515e98d69887654a7 -- module/zfs/spa.c &&
test "$(git write-tree)" = 0753658118378d67b75d550f34afe7d38811e71b && echo TREE-OK &&
echo "ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c" | sha256sum -c --quiet && echo SHA-OK &&
git diff --cached --name-status HEAD &&
setsid nohup sh -c "cd /var/tmp/task-1118-sld4/zd && nice make -j4; echo build-rc=\$?" > /var/log/task-1118-sld4/build-fb.log 2>&1 < /dev/null &
```

Expected TREE-OK, SHA-OK and the single line `M	module/zfs/spa.c`. The build must end with `build-rc=0`; quote `grep -E 'spa\.c|zfs\.ko' $L/build-fb.log` and every `warning:` line outside objtool notes.

Load zr ("(DEBUG mode)" must be absent). The new test 3 times (tag zr, in zr, `<sel>` = `-t failmode_reguid`), with the same expectations and quotes as step 3 item 1. After them: no pool left, the dmesg check since zr's N0, unload with zr's `scripts/zfs.sh -u`.

## STEP 5: fail-before (expected FAIL, then commands blocked for good, then the reboot)

Once build-fb has ended: load zd; its srcversion must equal the new `modinfo -F srcversion $D/zd/module/zfs.ko` (and differ from zd's value of step 3), "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`. The pre-run checks, then ONCE, in zd:

```sh
cd /var/tmp/task-1118-sld4/zd
setsid nohup sh -c "sudo -n -u zts timeout 1500 ./scripts/zfs-tests.sh -vKO -t failmode_reguid; echo runner-rc=\$?" > /var/log/task-1118-sld4/zts-fb-1.log 2>&1 < /dev/null &
```

Expected: `failmode_reguid` FAIL about 60 s after `zpool reguid -g 7208386112437612741 testpool` started, on `zinject still running after 60s`, the test's cleanup noting `commands are blocked: leaving the pools`, then the group's cleanup blocks in a `zpool` command until the runner's own timeout, or `timeout 1500`, ends it (say which, and when `runner-rc=` appeared; poll every 2 minutes, up to 30 minutes, and go on with what follows as soon as the test's FAIL line is there).

Quote: the result line and its duration; the last 30 lines of the test's log (under the runner's results directory; the log names it); `cat /proc/spl/kstat/zfs/testpool/state` (expected SUSPENDED); for every `zpool` and `zinject` process, `ps -o pid=,stat=,wchan=,args= -p <pid>` and `sudo -n cat /proc/<pid>/stack`. Expected: `zpool reguid -g 7208386112437612741 testpool` in `txg_wait_synced_flags` <- `dsl_sync_task_common` <- `dsl_sync_task` <- `spa_change_guid` <- `zfs_ioc_pool_reguid`; the `zinject` listing waiting for the mutex in `zio_inject_list_next`; the cleanup's `zpool` commands waiting for it in `spa_all_configs`, `spa_open_common` or `spa_namespace_enter`. Then from dmesg after N1, every line matching the pattern of step 3 (hung-task reports are expected after `hung_task_timeout_secs`), with the first stack after the first `blocked for more than` line (up to 30 lines). `timeout 30 zpool status -x` with zd's zpool (quote; expected to block: say so).

Save `sudo -n dmesg > $L/dmesg-fb.txt`. Then reboot (see the rules).

## STEP 6: end

After the reboot: `lsmod | grep -E '^(zfs|spl)'` empty; `sudo -n rm -rf /var/tmp/task-1118-sld4 /var/tmp/failmode_reguid`; `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1; losetup -a` (ZTS scratch files the fail-before left with no loop device on them: delete them and say so); `sudo -n fstrim -av`; `df -h /var/tmp`. Logs stay in /var/log/task-1118-sld4.

## Report

Write /var/log/task-1118-sld4/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, hung_task_timeout_secs, git version; TREES-OK, the two SHA-OK, the name-status lines and the test's mode; build rc and warnings of zd and zr; srcversions and DEBUG mode; step 4's TREE-OK, SHA-OK, name-status line, build-fb rc and quoted lines, and step 5's srcversion check.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note. Then one verdict line, e.g. "spa-lock-detangle-4: failmode_reguid PASS debug 3/3, release 3/3; groups failmode, zpool_reguid, zpool_export, zpool_import, cachefile, inuse, zpool_add, zpool_split, pool_checkpoint, getfsuuid PASS; DEMO-F status 15 ms, listing 10 ms, second reguid EEXIST; master spa.c: failmode_reguid FAIL 1/1 (zinject listing blocked 60 s), namespace lock held until the reboot".
3. Quotes: the NOTE and SUCCESS lines of step 3 item 1; the non-PASS lines of the groups; DEMO-F's output with the stack; the fail-before's failing lines, stacks, pool state and dmesg; the dmesg checks.
4. The reboot (time, how you resumed, whether sysrq was needed), deviations, cleanup, then End.
