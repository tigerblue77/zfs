TASK-1121-L2R (from the coordinator session "main"): functional, about 1 h 15 builds included. Our fix for cache devices that are never written to again after they were offline or faulted (fork branch `l2arc-feed-skip-dead-device`, no PR yet): since the L2ARC feed threads are per device (openzfs#18093), the feed thread of a cache device exits once the device is dead, and `zpool online` / `zpool clear` never create it again. (A) With the fix, debug build: the new ZTS test `l2arc_online_feed_pos` three times, then the group `-T l2arc` once. (B) Fail-before: master plus only the test commit, debug build: the test once; it is expected to FAIL in about two minutes, not to hang. No reboot is expected or pre-approved. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-l2r`, `L=/var/log/task-1121-l2r`.

## Why

Read on master `a1daebfec3f`; no report of it upstream.

- `l2arc_feed_thread()` leaves its loop when the device is dead (offline, faulted or removed): `if (dev->l2ad_vdev == NULL || vdev_is_dead(dev->l2ad_vdev)) break;` (module/zfs/arc.c:10269-10270), then sets `l2ad_feed_thread = NULL` and exits (arc.c:10330-10333). The device stays in the L2ARC list.
- `zpool online` (vdev.c:4768), `zpool clear <pool> <cache device>` (vdev.c:5017) and the online that zed issues when a removed disk reappears all go through `vdev_reopen()`, which finds the device still present and only calls `l2arc_rebuild_vdev()` (vdev.c:3026-3027); that only marks the device for rebuild (arc.c:10586-10610). The one place that creates a feed thread is `l2arc_add_vdev()` (arc.c:10563-10569), reached only for a device not in the list (vdev.c:3029, spa.c:2701-2702). So the device shows ONLINE and nothing is written to it again until export and import, or `zpool remove` and `zpool add`.
- Before #18093 the single feed thread skipped dead devices (`l2arc_dev_invalid()`) instead.

- fix (commit 1, `d93e8bc812a`): the feed thread skips a dead device (`continue` instead of `break`), as before #18093; it only exits when `l2arc_remove_vdev()` tells it to, which it already does.
- test (commit 2, `5a75fa94d3f`): `l2arc_online_feed_pos` (group l2arc) creates a pool on the l2arc group's 1 GiB file with a 280 MiB cache file, writes 200 MiB with fio and reads it randomly for 10 s, and waits for `l2_write_bytes` to grow. Then: `zpool offline` the cache device, 5 s for its feed thread to see it (it wakes at least once a second), `zpool online`, note `l2_write_bytes`, rewrite the files with fio and read them for 10 s, and wait at most 60 s for `l2_write_bytes` to grow; then the same with `zpool offline -f` and `zpool clear`. It logs `after zpool online: l2_write_bytes=N l2_feeds=M` after the device is back, and `l2_write_bytes=N l2_feeds=M` once it was written to.

Expected with the fix: PASS each time, in about 1 to 2 minutes; after each of the two cycles, `l2_write_bytes` grows within seconds, and the feed thread `l2arc_testpool_` (named `l2arc_<pool>_<vdev id>`, cut to 15 characters) exists for the whole life of the pool. Without the fix (part B): the test FAILs after the first cycle with `Nothing was written to the cache device within 60 seconds after zpool online.`, `l2_write_bytes` and `l2_feeds` do not move after the online, and the feed thread is gone from about 1 s after `zpool offline` until the end. Deterministic, unless the VM is so loaded that the thread does not wake within the 5 s.

What is tested: fork https://github.com/tigerblue77/zfs, branch `l2arc-feed-skip-dead-device`, head `5a75fa94d3fc5685528a16901abc38dc5ce78448`, tree `d7f3157b8da0ecee2f3fc8cf46364f882a5bd636`, two commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11). The fail-before tree is that master plus the test commit only: tree `b57edd9e39dfb9a2e38cf6dc65512b4159e15260`, whose `module/` and `include/` are master's.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows nothing, `losetup -a` shows no loop device backed by `/var/tmp/file-vdev*`, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 20 minutes; a group after 90 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|l2arc|txg|zfs|zpool|ksh'`, the kernel stacks of every D task and of every `l2arc_*` thread (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id zts`, `command -v fio` (the test is skipped without it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` (none), `losetup -a` (quote), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 in step 1 once $L exists, and say so; above 0: quote `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only `testpool...` with paths under `/var/tmp` or `/dev/loop`, move it to `$L/zpool.cache.step0` and say so; anything else: stop and report).

## STEP 1: the trees

```sh
D=/var/tmp/task-1121-l2r; L=/var/log/task-1121-l2r
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs l2arc-feed-skip-dead-device && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)"
#   5a75fa94d3fc5685528a16901abc38dc5ce78448 d7f3157b8da0ecee2f3fc8cf46364f882a5bd636 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -3
#   5a75fa94d3f ZTS: Write to a cache device after zpool online and clear
#   d93e8bc812a L2ARC: Keep the feed thread while the cache device is dead
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/zfs/arc.c tests/zfs-tests/tests/functional/l2arc/l2arc_online_feed_pos.ksh tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am
#   6e80bc23be1b8edf4f9246566129831cf5621aff066635f8b53f83bf35f4cd2e  module/zfs/arc.c
#   42d8f819eabceff36747e413d79a31f4715a7f67c06000d1d82d2684e8a19504  .../l2arc/l2arc_online_feed_pos.ksh
#   3a9c4612f0dae726b210857243f8b9084fad54bc2977714a7922c955d538d833  tests/runfiles/common.run
#   4ccb7a8da17c335a8341294bc0cc41ff67bd91ead6842b72ab3d323457d6d805  tests/zfs-tests/tests/Makefile.am
grep -c 'vdev_is_dead(dev->l2ad_vdev))$' module/zfs/arc.c; grep -A1 'vdev_is_dead(dev->l2ad_vdev))$' module/zfs/arc.c | tail -1   # 1, then a line with "continue;"
```

The fail-before tree:

```sh
D=/var/tmp/task-1121-l2r
git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git -c user.name=t -c user.email=t@t cherry-pick 5a75fa94d3fc5685528a16901abc38dc5ce78448
git rev-parse HEAD^{tree}          # b57edd9e39dfb9a2e38cf6dc65512b4159e15260
sha256sum module/zfs/arc.c tests/zfs-tests/tests/functional/l2arc/l2arc_online_feed_pos.ksh
#   091f206e6a12f598184e3a070f3a6edebd6f93e1ff5bde9ab601002b43ad041a  module/zfs/arc.c  (master's)
#   42d8f819eabceff36747e413d79a31f4715a7f67c06000d1d82d2684e8a19504  .../l2arc/l2arc_online_feed_pos.ksh
grep -A1 'vdev_is_dead(dev->l2ad_vdev))$' module/zfs/arc.c | tail -1   # a line with "break;"
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the two at once, about 25 minutes)

```sh
D=/var/tmp/task-1121-l2r; L=/var/log/task-1121-l2r
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg and fb-dbg; they must differ.

## A thread watcher, used in steps 3 and 4

The feed thread of the test's cache device is the kernel thread `l2arc_testpool_`. While a run is going, log once a second how many `l2arc_*` threads exist, next to the test's commands:

```sh
L=/var/log/task-1121-l2r; tag=ad-1   # the run's tag
setsid nohup sh -c "echo \$\$ > $L/threads-$tag.pid; while :; do echo \"\$(date +%T) l2arc=\$(ps -e -o comm= | grep -c '^l2arc_') \$(pgrep -a -x zpool | grep -oE 'zpool (create|offline|online|clear|destroy).*' | head -1)\"; sleep 1; done" > $L/threads-$tag.txt 2>&1 < /dev/null &
```

Stop it after the run with `kill $(cat $L/threads-$tag.pid)`. From it, quote only the transitions: each line where the `l2arc=` count changes, and the lines that name `zpool offline`, `zpool online` or `zpool clear` (the commands are short: they may not be caught; say so).

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Quote `for p in l2arc_noprefetch l2arc_feed_secs l2arc_write_max l2arc_headroom l2arc_rebuild_enabled l2arc_trim_ahead l2arc_dwpd_limit; do echo "$p=$(cat /sys/module/zfs/parameters/$p)"; done` (P0; the test sets `l2arc_noprefetch` to 0 and restores it). Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks and with the thread watcher (tags ad-1, ad-2, ad-3):

   ```sh
   D=/var/tmp/task-1121-l2r; L=/var/log/task-1121-l2r; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 1200 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_online_feed_pos > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in about 1 to 2 minutes. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, every line with `l2_write_bytes=` (four: `after zpool online: ...`, then `l2_write_bytes=...` larger, `after zpool clear: ...`, then `l2_write_bytes=...` larger); the thread watcher's transitions (expected: `l2arc=1` from the `zpool create` until the `zpool destroy`, never 0 in between); P0 after the run (equal: the test restores `l2arc_noprefetch`); and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 60 lines of its log and of the test's own log and the whole thread watcher file, then go on to the next run; two FAILs in a row: stop part A and report. A run that SKIPs: quote why (`fio missing` skips the test) and stop.
2. `-T l2arc` once (tag az), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T l2arc > $L/zts-az.log 2>&1 < /dev/null &`. Expected: every test of `[tests/functional/l2arc]` in `common.run` PASS, `l2arc_online_feed_pos` among them. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known (an `l2arc_dwpd_ratelimit_pos` KILLED at its timeout is a known CI flake, openzfs#19341).

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, fail-before, debug build without the fix

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; P0 as in step 3 (equal); note N1. Run the test ONCE, after the pre-run checks, with the thread watcher (tag fb):

```sh
D=/var/tmp/task-1121-l2r; L=/var/log/task-1121-l2r
cd $D/fb-dbg && setsid nohup timeout 1200 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t l2arc_online_feed_pos > $L/zts-fb.log 2>&1 < /dev/null &
```

Expected: FAIL in about 2 minutes, from the test's own log: `after zpool online: l2_write_bytes=N l2_feeds=M`, then `Nothing was written to the cache device within 60 seconds after zpool online.`; no `after zpool clear` line (the test stops at the first cycle). The thread watcher: `l2arc=1` from the `zpool create`, `l2arc=0` from about 1 s after the `zpool offline` until the end, never 1 again after the `zpool online`. Quote: the result line and duration; every `l2_write_bytes=` line and the FAIL line of the test's own log; `kstat` values are global, so also quote, right after the run, `grep -E '^(l2_feeds|l2_write_bytes) ' /proc/spl/kstat/zfs/arcstats`; the thread watcher's transitions; P0 after the run; dmesg since N1 for the same patterns as in step 3 (none expected). A PASS here means the fail-before was not reproduced: quote the same lines and the whole thread watcher file, then run it once more the same way (tag fb2); two PASSes: report it plainly. A hang (a single test after 20 minutes): capture as in the VM rules, stop and report.

Then the pre-run checks once more, `sudo -n $D/fb-dbg/scripts/zfs.sh -u` (quote rc), `sudo -n dmesg > $L/dmesg-fb-dbg.txt`.

## STEP 5: end

`lsmod | grep -E '^(zfs|spl) '` empty; `losetup -a` (quote); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` (none); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. `/var/tmp/test_results` and $L stay.

## Report

Write $L/report.md and give the same text as your final message. Part A: a table (run, result, duration, the four `l2_write_bytes` values, whether the thread watcher ever showed `l2arc=0` while the pool existed, P0 after the run), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen; the group result (Results Summary, every non-PASS with its lines); srcversions and load lines. Part B: result and duration, the `l2_write_bytes` line and the FAIL line, `l2_feeds`/`l2_write_bytes` after the run, the thread watcher's transitions with times relative to the `zpool offline` and `zpool online` lines, dmesg. One-line verdict: "L2R PASS 3/3 debug (feed thread alive through offline/online and fault/clear, l2_write_bytes grew after both), -T l2arc PASS; fail-before: FAIL 'Nothing was written ... after zpool online', feed thread gone from the offline on: confirmed" or what differs.
