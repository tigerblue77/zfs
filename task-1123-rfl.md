TASK-1123-RFL (from the coordinator session "main"): functional, about 1 h 45 with its builds, at most 3 h. **One reboot is pre-approved, for step 4 only**, because the test alone is expected to wedge the pool there; nowhere else. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It checks a fix for openzfs/zfs#18894 ("zpool remove of a failing log device hangs unkillably in zil_parse()"): the fix with its test, against master plus the test alone. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rfl/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The bug:** when the ZIL of an unmounted file system still has a block on a log device, and that device has faulted, `zpool remove` of the device resets the logs (`spa_reset_logs()` → `zil_reset()` → `zil_suspend()` → `zil_destroy()` → `zil_destroy_sync()` → `zil_parse()`), which reads that block. The read fails with ENXIO and, with the default `failmode=wait`, suspends the pool. Only `zpool clear` resumes it, and it needs `spa_namespace_lock`, which the removal holds: the removal never returns, every later `zpool`/`zfs` command blocks, and only a reboot gets out of it.
- **The fix:** when the walk that frees a log that was never claimed reaches a block on a log device that is not readable, it frees that block and stops without reading it (`zil_free_log_block()` in `module/zfs/zil.c`).
- **The test** `removal_with_faulted_log` (removal group): pool with a file log vdev, `failmode=wait`; synchronous writes to a file system, then unmount, so its ZIL keeps a block on the log; `zpool offline -f` of the log; `zpool remove` in the background, which must return within 60 s; then the device must be gone, the pool `ONLINE`, `zdb -b` must report "No leaks", and the file system must take synchronous writes again. If the removal has not returned after 60 s the test fails, and its cleanup touches no pool, since nothing short of a reboot can release it.
- Userspace (libzpool, debug, same master, `vdev_fault()` as `zpool offline -f` does): master's removal hangs with the pool suspended in every run (4 of 4, bounded at 60 s); with the fix it returns 0 in 100 ms in every run (5 of 5), the file system's ZIL header is cleared and the pool's allocated space equals that of the healthy control; the controls (healthy log, `failmode=continue`) are unchanged.

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parent |
|---|---|---|---|---|
| zd | zpool-remove-faulted-log | d7942e086d7769d76daee6360a5a3f2d75b7fd6c | 9db13f8bd20a500a06a0c86d90cffd044a5a00e4 | master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (the fix and its test, one commit) |
| zf | zpool-remove-faulted-log-fb | e2df6681bac15d59504e34b8860fd908dc5ddb75 | be88fa488d4c3830603e7ce0f7cda9a982d5f539 | master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (the test alone) |

The test file `tests/zfs-tests/tests/functional/removal/removal_with_faulted_log.ksh` has mode `100755` and sha256 `965b672f1f849ba563e778e37cc00f1e497a82ef71ea0271a411af8ee1150e43` in both trees.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`.
- **Before every ZTS run:**
  - `lsmod | grep -E '^zfs '` shows the module of that step's tree;
  - `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool;
  - `ls -d /var/tmp/testdir /var/tmp/removal_with_faulted_log 2>&1` says neither exists.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs in step 3 (zd).** A build still going after 60 minutes, a single-test run after 20 minutes, or a group after 150 minutes is a hang. Capture into `$L/hang.txt` as described under "Capture" below, then stop and report. **Do not reboot.**
- **Step 4 (zf) is expected to wedge the pool.** Its rules are in step 4; the reboot there is the only one approved.
- **Capture** (any hang, and step 4): no `zpool`, `zfs`, `zdb` or `zinject` command, since each one blocks for good once the pool is wedged. Read kernel files only in the background, so that a blocked read cannot block your shell: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`. Collect:
  - `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zpool|zfs|ksh'`;
  - `/proc/<tid>/stack` of every `zpool` process and every `ksh` process in D state, and of every `txg_sync` and `txg_quiesce` thread (their tids from the `ps` line);
  - `/proc/spl/kstat/zfs/testpool/state` (a lock-free read; it says `SUSPENDED` or not);
  - `tail -n 200 /proc/spl/kstat/zfs/dbgmsg`;
  - `sudo -n dmesg | tail -n 150`.
- **Kernel log.** After each step, check the new kernel lines since that step's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock`. In step 3 none are expected. Pool suspend `WARNING` lines (`has encountered an uncorrectable I/O failure and has been suspended`): count them, and say whether any falls between the `ZTS run .../removal_with_faulted_log` kmsg line and the next `ZTS run` line (none expected in step 3). In step 4 a suspend line and `blocked for more than` lines are expected: quote them.
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end of step 3; step 4 ends with the reboot.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 12 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git`;
- `ls -d /var/tmp/task-1123-rfl /var/log/task-1123-rfl /var/tmp/testdir /var/tmp/removal_with_faulted_log 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic`: quote them; do not change them. **If `hung_task_panic` is not 0, do steps 1 to 3 and skip step 4**: say so in the report;
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service 2>&1`: quote it (none of them should import or mount anything after the reboot of step 4);
- then, once `$L` exists (step 1), `ls -A /var/tmp > $L/vartmp-0.txt`.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-rfl; L=/var/log/task-1123-rfl; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
for t in "zd zpool-remove-faulted-log" "zf zpool-remove-faulted-log-fb"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 2 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
p=tests/zfs-tests/tests/functional/removal/removal_with_faulted_log.ksh
for n in zd zf; do (cd $D/$n && echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64)"); done
for n in zd zf; do (cd $D/$n && git log -1 --format='%an | %s' && git diff --stat HEAD^ HEAD | tail -1); done
```

Expected:

```
zd d7942e086d7769d76daee6360a5a3f2d75b7fd6c 9db13f8bd20a500a06a0c86d90cffd044a5a00e4 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
zf e2df6681bac15d59504e34b8860fd908dc5ddb75 be88fa488d4c3830603e7ce0f7cda9a982d5f539 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
```

Then two lines with mode `100755` and sha256 `965b672f1f849ba563e778e37cc00f1e497a82ef71ea0271a411af8ee1150e43`. Then `Tigerblue77 | ZIL: Don't read a dead log device when destroying a log`, `4 files changed, 131 insertions(+), 4 deletions(-)`, `Tigerblue77 | ZTS: Add removal_with_faulted_log` and `3 files changed, 110 insertions(+), 1 deletion(-)`.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: the two debug builds, at once (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-rfl; L=/var/log/task-1123-rfl
for n in zd zf; do setsid nohup sh -c "date -u +%T; cd $D/$n && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$n.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-z?.log` until there are two lines; each must read `build-rc=0`. Quote both durations and every `warning:` line outside objtool notes.

## Step 3: the fix (zd), all expected PASS

**Load zd:**
- `sudo -n $D/zd/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zd/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- write the mark: `echo "task-1123-rfl: zd loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**Runs,** one at a time, after the pre-run checks:

```sh
cd /var/tmp/task-1123-rfl/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-rfl/zts-<tag>-<k>.log 2>&1 < /dev/null &
```

Quote for each run:
- every `Test:` line with its result and duration;
- the Results Summary;
- from the run's timestamped log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); ls "$R/log"`; each test's own `merged` or `stdout` file is under `$R` too): every `NOTE` line of `removal_with_faulted_log`, and every line with `FAIL`, `failed` or `ERROR`.

On a FAIL, copy `$R/log` to `$L/log-<tag>-<k>.txt` and quote the failing command, its output and the 20 lines before it.

1. `removal_with_faulted_log`, 3 runs (tag `zd-rfl`, `<sel>` = `-t removal_with_faulted_log`). Expected: PASS each time, in about a minute or less. Its notes: `N bytes allocated on the log device` with N above 0, then `zpool remove returned 0 within N seconds:` with N well under 60 (nothing follows the colon: the command prints nothing).
2. Groups, one run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `removal`, then `slog`. Expected: all PASS, `removal_with_faulted_log` included. For any test that fails or is skipped, quote it; step 4 reruns it on zf before the wedge, to tell a master failure from one of the fix.

After these runs:
- no pool left;
- the kernel-line check since the zd mark;
- unload with zd's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 4: the test alone (zf), expected FAIL and a wedged pool, then the reboot

Load zf as in step 3, with the mark `task-1123-rfl: zf loaded`.

**4a. Only if a test failed or was skipped in step 3's groups:** run each such test once on zf first (tag `zf-recheck-<test>`, `<sel>` = `-t <test>`), with the usual quotes, and say for each whether master fails it the same way. Then the pre-run checks again.

**4b. The test, once** (tag `zf-rfl`, `<sel>` = `-t removal_with_faulted_log`), started as in step 3. Then, instead of waiting for the run to end (the group's cleanup script blocks on the wedged pool; at best the runner kills it at its 600 s timeout):
- note the start time; poll `grep -E 'removal_with_faulted_log .*\[(PASS|FAIL|KILLED|SKIP)' $L/zts-zf-rfl-1.log` every 30 s, for at most 10 minutes;
- expected: `[FAIL]` after about 60 to 70 s. Then read `$R/log` as in step 3 (the test's lines are written when it ends) and quote from it: the `NOTE: N bytes allocated on the log device` line (N above 0), the failing line, which must end with `zpool remove of the faulted log device still running after 60 seconds`, and the cleanup's `NOTE: zpool remove (pid N) is still blocked; only a reboot releases testpool`;
- a PASS here, or a FAIL at any other line, is not the expected fail-before: quote it and say so. If the run passed and nothing is blocked (`ps` shows no `zpool` process in D state), do not reboot: unload zf with its `scripts/zfs.sh -u` and go to the cleanup below.

**4c. Capture** (the "Capture" list of the VM rules) into `$L/wedge.txt`, about 3 minutes after the FAIL line so the hung-task watchdog has spoken. Expected, and to quote:
- `zpool remove` in D state, its stack through `zio_wait`, `arc_read`, `zil_parse`, `zil_destroy`, `zil_suspend`, `zil_reset`, `dmu_objset_find`, `spa_reset_logs`, `spa_vdev_remove`;
- the group cleanup script's `zpool` (or `zfs`) command blocked on `spa_namespace_lock`: in D state through `spa_namespace_enter`, or in S state through `spa_all_configs` (the interruptible enter that `zpool list` uses). The runner kills that script at its 600 s timeout and the run may then end on its own: if it does, quote its Results Summary; it changes nothing below;
- the pool state `SUSPENDED`;
- in the kernel log since the zf mark: `WARNING: Pool 'testpool' has encountered an uncorrectable I/O failure and has been suspended.`, and the `blocked for more than` reports (none if `hung_task_timeout_secs` is 0);
- `txg_sync` and `txg_quiesce` stacks, as they are (master may show either idle or blocked: quote, do not judge).

Copy `$R/log` and the test's own directory under `$R` into `$L/` as well, then make sure the logs are on disk: `sync -f $L/wedge.txt` (this syncs only the file system of `/var/log`, never ZFS).

**4d. Reboot (pre-approved, this one only).** A graceful reboot can hang on the suspended pool's mount and leave the VM unreachable, so reboot at once: `sudo -n systemctl reboot -ff`. Wait for the VM, then check and quote: `uptime`, `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), and `ls -l $L/wedge.txt` (must be there).

**Cleanup after the reboot:**
- `ls -A /var/tmp | diff $L/vartmp-0.txt - `: quote it. Remove only what this task created there: `task-1123-rfl`, `removal_with_faulted_log`, `testdir`, `file-vdev*`, `zfs-tests.*.run`, `constrained_path.*`, and the `test_results` runs of this task (their logs are in `$L`). Anything else new: quote it and leave it;
- `/etc/zfs/zpool.cache`: if step 0 found it absent or empty and it now lists `testpool`, remove it (`sudo -n rm -f /etc/zfs/zpool.cache`); otherwise quote it and leave it;
- `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-rfl/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, the three kernel sysctls of step 0, the zfs service states, `trees.txt`, the test's mode and sha256 in both trees, the log and diffstat lines, the build rc and durations with their warnings, and both srcversions with "(DEBUG mode)".
2. **Table:** tag | tree | run | selection | result per test (or "all N PASS") | duration | key note. Then one verdict line, for example: "fix: removal_with_faulted_log PASS 3/3 (remove returns in N s); groups removal and slog PASS; test alone: FAIL at the 60 s bound, pool SUSPENDED, zpool remove stuck in zil_parse(), reboot done".
3. **Quotes:** the NOTE lines of each run of the test (run 1 in full, then what differs), the non-PASS lines, the kernel-line checks with the suspend counts, and from `wedge.txt` the `ps` lines, the stacks, the pool state and the kernel lines.
4. **Deviations, cleanup,** then `End`.
