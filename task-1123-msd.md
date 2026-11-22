TASK-1123-MSD (from the coordinator session "main"): functional, about 1 h 30 with its builds, at most 2 h 15, no reboot expected. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It checks the takeover of upstream PR #19480 by sairex53 ("Allow stopped vdev operations to cancel metaslab waits"): his fix, rebased onto master with one include corrected, together with our two tests, against master plus the tests alone. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-msd/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

At most three metaslabs of a group can be disabled at once. A manual TRIM limited to a low rate keeps its metaslab disabled while it sleeps.

- **The bug:** an initialize, TRIM, autotrim or rebuild thread waiting in `metaslab_disable()` for one of those slots cannot be stopped until a slot is released. A caller that stops it with `spa_namespace_lock` held (an attach, a detach) waits that long too, and so does every `spa_open()` on the system (our issue #19448).
- **The fix (sairex53's commit):** it adds `metaslab_disable_cancellable()`, used in those threads, and wakes a group's waiters when a stop is requested.
- **Our tests:** they hold all three slots with slow TRIMs and require each stop to return within 30 seconds. Without the fix, each step takes about 160 seconds.

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parents down to master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` |
|---|---|---|---|---|
| zd | metaslab-disable-cancellable | 06718170e8f2c2180d04cb99a1d4cd191b4860a1 | 247c9da67345c78d69e94fd49101936b805f2342 | HEAD^ `c5c42d42604932f7934a42f00a95ebbda5043be6` (the fix, author Eduard Zateev), HEAD~2 = master |
| zf | metaslab-disable-cancellable-fb | 5deefa64fe27a6fcc0e09e2735bca01f7fff4a15 | d11cd77d349643b415614ad3bf507aa875995442 | HEAD^ = master (the tests alone) |

Both test files have mode `100755`. Their paths are under `tests/zfs-tests/tests/functional/cli_root/zpool_trim/`:

| test | sha256 |
|---|---|
| zpool_trim_rate_stop_wait.ksh | cff22c4caa2fba80283f2946b088ce0f5c86325641399fad68852eace9bdb61a |
| zpool_trim_rate_stop_rebuild.ksh | 75c748dff2db6feb52a6fb8d08f396b6a131b021be0ad013997fa3b7e53fd13b |

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`.
- **Before every ZTS run:**
  - `lsmod | grep -E '^zfs '` shows the module of that step's tree;
  - `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool;
  - `ls -d /var/tmp/testdir 2>&1` says it does not exist.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs.** A build still going after 60 minutes, a single-test run after 20 minutes, or a group after 90 minutes is a hang.
  - Capture into `$L/hang.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool` and `ksh` process;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
- **Kernel log.** After each step, check the new kernel lines since that step's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock`. None are expected. Pool suspend/resume `WARNING` lines are not a finding: count them.
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 12 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git`;
- `ls -d /var/tmp/task-1123-msd /var/log/task-1123-msd /var/tmp/testdir 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty;
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-msd; L=/var/log/task-1123-msd; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
for t in "zd metaslab-disable-cancellable 2" "zf metaslab-disable-cancellable-fb 1"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 4 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~$3) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
for n in zd zf; do (cd $D/$n && for f in zpool_trim_rate_stop_wait zpool_trim_rate_stop_rebuild; do p=tests/zfs-tests/tests/functional/cli_root/zpool_trim/$f.ksh; echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64) $f"; done); done
cd $D/zd && git log -2 --format='%an | %s' && git diff --stat HEAD~2 HEAD | tail -1
```

Expected:

```
zd 06718170e8f2c2180d04cb99a1d4cd191b4860a1 247c9da67345c78d69e94fd49101936b805f2342 c5c42d42604932f7934a42f00a95ebbda5043be6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
zf 5deefa64fe27a6fcc0e09e2735bca01f7fff4a15 d11cd77d349643b415614ad3bf507aa875995442 a1daebfec3f5ca46ac261ef339efdf0dbeae997c a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
```

Then four lines with mode `100755` and the sha256 of the table. Then `Tigerblue77 | ZTS: test that stopping vdev operations does not wait for a slow TRIM`, `Eduard Zateev | Fix vdev operation stop waiting on metaslab slots`, and `10 files changed, 453 insertions(+), 32 deletions(-)`.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: the two debug builds, at once (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-msd; L=/var/log/task-1123-msd
for n in zd zf; do setsid nohup sh -c "date -u +%T; cd $D/$n && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$n.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-z?.log` until there are two lines; each must read `build-rc=0`. Quote both durations and every `warning:` line outside objtool notes.

## Step 3: the fix (zd), all expected PASS

**Load zd:**
- `sudo -n $D/zd/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zd/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- write the mark: `echo "task-1123-msd: zd loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**Runs,** one at a time, after the pre-run checks:

```sh
cd /var/tmp/task-1123-msd/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-msd/zts-<tag>-<k>.log 2>&1 < /dev/null &
```

Quote for each run:
- every `Test:` line with its result and duration;
- the Results Summary;
- from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`): every `NOTE` line of the two new tests, and every line with `FAIL`, `failed` or `ERROR`.

On a FAIL, copy the merged log to `$L/merged-<tag>-<k>.txt` and quote the failing command, its output and the 20 lines before it.

1. `zpool_trim_rate_stop_wait`, 3 runs (tag `zd-wait`, `<sel>` = `-t zpool_trim_rate_stop_wait`). Expected: PASS each time; every timed step it notes is well under 30 s.
2. `zpool_trim_rate_stop_rebuild`, 3 runs (tag `zd-rebuild`). Expected: the same.
3. Groups, one run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `zpool_trim`, `zpool_initialize`, `trim`, `replacement`. Expected: all PASS. If a group skips or fails a test in the same way on master, say which and why; leave that to main.

After these runs:
- no pool left;
- the kernel-line check since the zd mark;
- unload with zd's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 4: the tests alone (zf), expected FAIL

Load zf as in step 3, with the mark `task-1123-msd: zf loaded`. Then one run of each test: tag `zf-wait` (`-t zpool_trim_rate_stop_wait`) and tag `zf-rebuild` (`-t zpool_trim_rate_stop_rebuild`).

Expected: each FAILs at its first timed step, which takes about 160 s against the 30 s limit. Quote that step's note and the failing line. The test's cleanup must leave no pool. Quote `zpool list` and the group's cleanup result.

A run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

Then:
- the kernel-line check since the zf mark (a `blocked for more than` line is possible here: quote it, it is not a stop);
- unload with zf's `scripts/zfs.sh -u`; `lsmod` must be empty;
- `sudo -n rm -rf /var/tmp/task-1123-msd`, `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-msd/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, hung_task_timeout_secs, `trees.txt`, the test modes and sha256, the log and diffstat lines, the build rc and durations with their warnings, and both srcversions with "(DEBUG mode)".
2. **Table:** tag | tree | run | selection | result per test (or "all N PASS") | duration | key note. Then one verdict line, for example: "fix: zpool_trim_rate_stop_wait PASS 3/3, zpool_trim_rate_stop_rebuild PASS 3/3; groups zpool_trim, zpool_initialize, trim, replacement PASS; tests alone: both FAIL at the first timed step (~160 s)".
3. **Quotes:** the timed NOTE lines of each run (run 1 in full, then what differs), the non-PASS lines, and the kernel-line checks.
4. **Deviations, cleanup,** then `End`.
