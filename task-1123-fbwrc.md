TASK-1123-FBWRC (from the coordinator session "main"): functional, on **VM 106** (Debian 13, Linux 6.12.118, 8 CPUs, 7.8 GB), about 1 h with its builds, at most 1 h 45, not next to a performance run. It is the fail-before of our two race tests on wrouesnel's zinject framework (the openzfs/zfs#19997 direction): `slog_replay_fs_freesp_order` and `upgrade_task_unmount`, on one tree, the fork branch `zinject-delay-points-v2-wr` with both fixes it tests undone, built twice: release with `--enable-inject-points` first, then debug. With the fixes, TASK-1122-WRS (debug) and TASK-1123-WRSR3 (release with `--enable-inject-points`) ran each test 3 times on this VM: all PASS. Here `slog_replay_fs_freesp_order` is expected to FAIL on both builds. `upgrade_task_unmount` is expected to PASS on the release build, which cannot see the bug, and to trip an assertion on the debug build, which halts a kernel thread: **one reboot, pre-approved for step 5 only**. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-fbwrc/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `F`, `t`, `n`, `T`).

## Plan and durations

| step | what | about |
|---|---|---|
| 0 | state of this VM | 3 min |
| 1 | two checkouts of the fail-before head, and their checks | 3 min |
| 2 | both builds at once: `ffr` release with `--enable-inject-points`, `ffd` debug | 20 to 25 min |
| 3 | release module: `slog_replay_fs_freesp_order` twice, then `upgrade_task_unmount` once | 6 min |
| 4 | debug module: `slog_replay_fs_freesp_order` twice | 4 min |
| 5 | debug module: `upgrade_task_unmount` once (crash expected), the captures, the reboot | 8 min |
| 6 | after the reboot (the resumption message fires about 5 min after it), then the end | 8 min |

## Why

| test | the bug, in one line | its fix, on upstream master |
|---|---|---|
| `slog/slog_replay_fs_freesp_order` (Linux) | A truncate or punch dropped its range lock before its change was logged (a truncate's size even went into the `TX_SETATTR` logged after it). A write into the freed range, logged in between, came first in the log, and replay cut it off. | `b306edf42fc` "Linux: log size changes and punches before dropping the range lock" (#19706) |
| `upgrade/upgrade_task_unmount` | The upgrade task cleared `os_upgrade_id` before it released its long hold on the dataset. An unmount in between did not wait for the task, and the eviction of the dataset found the hold still counted. | `dc66d8489e6` "Release the upgrade hold before clearing os_upgrade_id" (#19556) |

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | HEAD^ | HEAD~5 |
|---|---|---|---|---|---|
| ff | zinject-delay-points-v2-wr-fb | 4cd797f136302e1d6e85b259aca65bad2c066f65 | c479c9444161402f95cc1ac579ba1e8df0d548c5 | fe63d5068f247c229aa795a3069458d78bf3d947 | 189a309d464ca51c74d627fdbdd84b5992eade2d |

- `HEAD^` is the head of `zinject-delay-points-v2-wr` that WRS and WRSR3 ran: the points `zfs_freesp:changed` and `dmu_objset_upgrade_task_cb:done`, the two tests and the `cleanup_upgrade` change, 4 commits on wrouesnel's `zinject-points-uplift` (`189a309d464`, on upstream master `a1daebfec3f`).
- `HEAD`, "Undo both fixes (fail-before, VM only) [skip ci]", changes three files and no test. It reverts `b306edf42fc` (clean), and moves `dsl_dataset_long_rele()` in `dmu_objset_upgrade_task_cb()` back after `mutex_exit()` and after the point, since `dc66d8489e6` does not revert cleanly over the point. This is the change of the fail-before tree `ac650b32de61` of the earlier version (TASK-1117-ZDP): main checked that its added and removed lines are those of `ac650b32de61` against its base `513d86b8f88`, line for line. Only the context lines around the points differ.
- `ffr` and `ffd` are two checkouts of this one commit: `ffr` gets the release build, `ffd` the debug build.

| file | mode | sha256 |
|---|---|---|
| tests/zfs-tests/tests/functional/slog/slog_replay_fs_freesp_order.ksh | 100755 | 4a6379e61f9f3d9dde078520e1401ec90aec3d8cbae2a108a4f8fa671fb96735 (WRS's) |
| tests/zfs-tests/tests/functional/upgrade/upgrade_task_unmount.ksh | 100755 | cdb450e655280b2bab6d9735cc7c8e7dcc4a10b20928b35d32ac1cb519f1bca6 (WRS's) |
| module/os/linux/zfs/zfs_vnops_os.c | | 7181b7eea74d37a0d327ee656c3e1ba23e902c00e283b4416b652413170c4687 |
| module/os/linux/zfs/zfs_znode_os.c | | 1518e5fe7592f3a95aed8b7e522d700dd1f5a1a9a951de8f94d7047edd8cd095 |
| module/zfs/dmu_objset.c | | e8a866ab30a4262efa116c51a87147ef4581c7b7355c87f22a9459b12c13cc8f |

## What each test does without the fixes

### `slog_replay_fs_freesp_order`: FAIL on both builds

The test freezes the pool, holds `truncate -s 128K` of a 512K file at `zfs_freesp:changed` (the file is already cut, nothing is logged yet), writes 128K at 128K with fsync while it is held, gives that write 5 s, releases the truncate, fsyncs, compares, exports and imports (replay), and compares again. Without the fix, the truncate holds no range lock at the point, and its size is logged in a `TX_SETATTR` after the release. So:
- the write does not wait: no `NOTE: truncate: the write waited`;
- the file in memory is right: the `cmp` before the export passes;
- the log holds the write first and the size second, so replay cuts the file back to 128K, and the `cmp` after the import fails at the end of the shorter file;
- the punch case is never reached.

No assertion stands on this path, so both builds fail the same way. TASK-1117-ZDP measured it 3/3 on debug builds of the earlier version, with a clean kernel log (`in line 516`, then 477 and 498: the line depends on the random data). Expected on both builds, then `[FAIL]` and no kernel line:

```
SUCCESS: cmp /var/tmp/testdir/expect.truncate /testpool/testfs/truncate
SUCCESS: zfs unmount testpool/testfs
SUCCESS: zpool export testpool
SUCCESS: zpool import -f -d /var/tmp/disk-slog testpool
cmp: EOF on /testpool/testfs/truncate after byte 131072, in line <n>
ERROR: cmp /var/tmp/testdir/expect.truncate /testpool/testfs/truncate exited 1
```

### `upgrade_task_unmount`: PASS on release, assertion on debug

The test holds the upgrade task at `dmu_objset_upgrade_task_cb:done`, unmounts the file system, sleeps 1 s, releases the task (`zinject -c`), then mounts, exports and imports. Without the fix, the task reaches the point with `os_upgrade_id` cleared and its long hold on the dataset still counted. The unmount does not wait for it (`dmu_objset_upgrade_stop()` finds no id), and the `dbu_evict` thread evicts the dataset during the sleep.

- **Release:** nothing checks the hold. Without `ZFS_DEBUG`, `zfs_refcount_destroy()` is a plain store (`include/sys/zfs_refcount.h`), so the eviction frees the dataset and its objset. Once released, the task reads the freed objset and decrements a counter in the freed dataset. The test cannot see that. Expected: `[PASS]` in about 1 s, as with the fix, and no kernel line. This only shows that a release build does not detect the bug, as the pull request text says; it is not a fail-before. A use after free can hit memory that is already reused (an oops in a `z_upgrade` thread, or a KFENCE report). That is not expected, and it is why this run comes last on the release module, once.
- **Debug:** `zfs_refcount_destroy()` checks the count, so the eviction trips an assertion, and `dbu_evict` halts in it. TASK-1117-ZDP saw it with the earlier version, 4 s into the test; `refcount.c` line 91 is the same in this tree. Expected in the kernel log, with the stack `spl_panic`, `zfs_refcount_destroy_many`, `dsl_dataset_evict_async`, `taskq_thread` and Comm `dbu_evict`:

```
VERIFY3U(rc->rc_count, ==, number) failed (1 == 0)
PANIC at refcount.c:91:zfs_refcount_destroy_many()
```

  This is an SPL panic: no oops, and the VM stays up. The test goes on: `zinject -c` and `zfs mount` succeed, then `zpool export` blocks for good behind the halted eviction. The earlier version's log ended:

```
SUCCESS: zfs unmount testpool/testfs
SUCCESS: sleep 1
removed handler <id>
SUCCESS: zinject -c <id>
SUCCESS: zfs mount testpool/testfs
```

  About `hung_task_timeout_secs` later: `INFO: task dbu_evict:<pid> blocked for more than <s> seconds.`, and the same for `zpool`. The runner would kill the test at its 600 s timeout and then wait as long again in the group's cleanup (about 20 minutes in TASK-1117-ZDP): this task does not wait for it. Nothing can unload the module afterwards: this is the reboot of step 5.

## VM rules

These are the rules of TASK-1123-MSD (task-1123-msd.md on this branch), with these additions:

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`, and never import a pool by hand.
- **One module and one ZTS run at a time.** The two builds of step 2 run at once, before any run.
- **Before every ZTS run** (`t` is `ffr` or `ffd`):
  - `lsmod | grep -E '^zfs '` shows a module whose `/sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/$t/module/zfs.ko`;
  - `timeout 30 sudo -n $D/$t/zpool list -H -o name` shows no pool;
  - the first line of `timeout 30 sudo -n $D/$t/zinject` is `No handlers registered.`;
  - `ls -d /var/tmp/testdir /var/tmp/disk-slog /var/tmp/zpool_upgrade_test.dat /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows none of them;
  - `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints `CACHE-OK`.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled. Every run uses `-vKOD`, so that the test's output reaches its log as it runs.
- **Hangs** (any run but step 5's). A build still going after 60 minutes, or a run after 20 minutes, is a hang.
  - Capture into `$L/hang-<t>-<n>.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zinject|ksh'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool`, `zfs`, `zinject` and `ksh` process;
    - `timeout 10 sudo -n $D/$t/zinject`;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
- **Held threads.** A thread held at a zinject point stays held until its handler is removed by its ID. Check for one with `sudo -n grep -l -E 'zio_inject_pause|zio_handle_point_injection|zio_handle_func_injection' /proc/[0-9]*/task/[0-9]*/stack 2>/dev/null`, which must print nothing. A held thread is always a stop, in step 5 too: capture as for a hang, change nothing, do not reboot, and report.
- **Kernel log.** Every run writes its mark first (`task-1123-fbwrc: <t> <n> start`). After the run, check the kernel lines since its mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock`. In steps 3 and 4 none are expected: any line is a stop, after the module's unload if the unload works. The one exception is step 3's rule on a KFENCE report. Pool suspend and resume `WARNING` lines are not a finding: count them. Step 5 says which lines it expects.
- **Reboots:** exactly one, pre-approved for step 5 only, and only after the crash step 5 expects. A hang, an oops, a panic anywhere else, or a module that will not unload is a stop: capture, report and wait for main. So is anything unexpected in step 5 (see its last part).
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end. After the reboot none are loaded.

## Step 0: state

Check and quote:
- `date -u`, `hostname`, `uname -r` (6.12.118 expected), `uptime -s`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 12 GiB free (two trees), else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git truncate fallocate cmp`;
- `ls -d /var/tmp/task-1123-fbwrc /var/log/task-1123-fbwrc /var/tmp/testdir /var/tmp/disk-slog /var/tmp/zpool_upgrade_test.dat 2>&1`: all absent;
- `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1`: none;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty. If it is empty, move it to `$L/zpool.cache.step0` in step 1 once `$L` exists, and say so. Above 0: stop and report;
- `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs`: quote them. If `hung_task_panic` is 1, set it to 0 for this task (`sudo -n sysctl kernel.hung_task_panic=0`) and say so. Change nothing else here;
- `cat /sys/module/kfence/parameters/sample_interval 2>&1`: quote it (0, or no such file: KFENCE checks nothing);
- `losetup -a`.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-fbwrc; L=/var/log/task-1123-fbwrc; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
echo "$(date -u +%FT%TZ) start $(hostname) $(uname -r)" >> $L/progress.md
for t in ffr ffd; do
  mkdir $D/$t && cd $D/$t && git init -q . && git fetch -q --depth 6 $U zinject-delay-points-v2-wr-fb && git checkout -q FETCH_HEAD &&
  echo "$t $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~5) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
```

Expected:

```
ffr 4cd797f136302e1d6e85b259aca65bad2c066f65 c479c9444161402f95cc1ac579ba1e8df0d548c5 fe63d5068f247c229aa795a3069458d78bf3d947 189a309d464ca51c74d627fdbdd84b5992eade2d 0
ffd 4cd797f136302e1d6e85b259aca65bad2c066f65 c479c9444161402f95cc1ac579ba1e8df0d548c5 fe63d5068f247c229aa795a3069458d78bf3d947 189a309d464ca51c74d627fdbdd84b5992eade2d 0
```

Then the files, the files the last commit changes, and the diffstats of the last commit and of the whole tree on the framework:

```sh
D=/var/tmp/task-1123-fbwrc; F=tests/zfs-tests/tests/functional
for t in ffr ffd; do (cd $D/$t &&
  for p in slog/slog_replay_fs_freesp_order upgrade/upgrade_task_unmount; do
    echo "$t $(git ls-tree HEAD $F/$p.ksh | cut -c1-6) $(sha256sum < $F/$p.ksh | cut -c1-64) $p"; done
  for f in module/os/linux/zfs/zfs_vnops_os.c module/os/linux/zfs/zfs_znode_os.c module/zfs/dmu_objset.c; do
    echo "$t $(sha256sum < $f | cut -c1-64) $f"; done
  echo "$t changed: $(git diff --name-only HEAD^ HEAD | paste -sd' ')"
  echo "$t fb:$(git diff --stat HEAD^ HEAD | tail -1)"; echo "$t all:$(git diff --stat HEAD~5 HEAD | tail -1)"); done
```

Expected, the same eight lines for `ffd`:

```
ffr 100755 4a6379e61f9f3d9dde078520e1401ec90aec3d8cbae2a108a4f8fa671fb96735 slog/slog_replay_fs_freesp_order
ffr 100755 cdb450e655280b2bab6d9735cc7c8e7dcc4a10b20928b35d32ac1cb519f1bca6 upgrade/upgrade_task_unmount
ffr 7181b7eea74d37a0d327ee656c3e1ba23e902c00e283b4416b652413170c4687 module/os/linux/zfs/zfs_vnops_os.c
ffr 1518e5fe7592f3a95aed8b7e522d700dd1f5a1a9a951de8f94d7047edd8cd095 module/os/linux/zfs/zfs_znode_os.c
ffr e8a866ab30a4262efa116c51a87147ef4581c7b7355c87f22a9459b12c13cc8f module/zfs/dmu_objset.c
ffr changed: module/os/linux/zfs/zfs_vnops_os.c module/os/linux/zfs/zfs_znode_os.c module/zfs/dmu_objset.c
ffr fb: 3 files changed, 45 insertions(+), 78 deletions(-)
ffr all: 9 files changed, 243 insertions(+), 79 deletions(-)
```

Then the readable checks of the fixes' files:

```sh
D=/var/tmp/task-1123-fbwrc; cd $D/ffd
grep -c -F 'zfs_freesp(zp, vap->va_size, 0, 0, FALSE);' module/os/linux/zfs/zfs_vnops_os.c                    # 1: master's call before b306edf42fc
grep -c zfs_extend_locked module/os/linux/zfs/zfs_znode_os.c; grep -c '"zfs_freesp:changed"' module/os/linux/zfs/zfs_znode_os.c   # 0, 1
grep -n -e 'os->os_upgrade_id = 0;' -e '"dmu_objset_upgrade_task_cb:done"' -e 'dsl_dataset_long_rele(dmu_objset_ds(os), upgrade_tag);' module/zfs/dmu_objset.c | head -3
```

The last command must print the id cleared, then the point, then the release after it:

```
1404:	os->os_upgrade_id = 0;
1411:		    "dmu_objset_upgrade_task_cb:done", &ignored);
1413:	dsl_dataset_long_rele(dmu_objset_ds(os), upgrade_tag);
```

If a fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: both builds, at once (20 to 25 minutes)

```sh
D=/var/tmp/task-1123-fbwrc; L=/var/log/task-1123-fbwrc
for t in "ffr --enable-inject-points" "ffd --enable-debug"; do set -- $t
  setsid nohup sh -c "date -u +%T; cd $D/$1 && ./autogen.sh && ./configure $2 && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$1.log 2>&1 < /dev/null &
done
```

`ffr` is the release build: no `--enable-debug`, so no ASSERT, but with injection points.

Poll `grep -H build-rc= $L/build-*.log` until there are two lines; each must read `build-rc=0`. Then quote:
- the two durations, and every `warning:` line outside objtool notes (none expected);
- `grep -H -E 'assertion support will be enabled|injection points will be built' $L/build-*.log`. Expected: `no` then `yes` for `ffr`, `yes` and `yes` for `ffd`. A `no` for injection points is a stop: the tests would report UNSUPPORTED;
- `grep -H '^DEBUG_ZFS=' $D/ffr/config.log $D/ffd/config.log`: `'_without_debug'` for `ffr`, `'_with_debug'` for `ffd`;
- the point names in each module: `strings $D/<t>/module/zfs.ko | grep -c -x -E 'zfs_freesp:changed|dmu_objset_upgrade_task_cb:done'`, 2 for each;
- the two srcversions, `modinfo -F srcversion $D/<t>/module/zfs.ko`, which must differ.

## Loading a module (steps 3 and 4)

- `lsmod | grep -E '^(zfs|spl) '` must be empty.
- `sudo -n $D/<t>/scripts/zfs.sh`.
- Quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/<t>/module/zfs.ko`.
- Quote the last `ZFS: Loaded module` line. It must not say "(DEBUG mode)" for `ffr`, and must for `ffd`.
- Write the mark: `echo "task-1123-fbwrc: <t> loaded" | sudo -n tee /dev/kmsg > /dev/null`, and a line in `$L/progress.md`.

## A run (steps 3 and 4)

`n` names the run (`freesp-1`, `freesp-2`, `upgrade-1`), `T` the test. After the pre-run checks:

```sh
D=/var/tmp/task-1123-fbwrc; L=/var/log/task-1123-fbwrc; t=<ffr|ffd>; n=<run>; T=<test>
echo "task-1123-fbwrc: $t $n start" | sudo -n tee /dev/kmsg > /dev/null; echo "$(date -u +%FT%TZ) $t $n start" >> $L/progress.md
cd $D/$t && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKOD -t $T > $L/zts-$t-$n.log 2>&1 < /dev/null &
```

Poll until the log has its `Results Summary` and `pgrep -f zfs-tests.sh` prints nothing (about a minute). Quote:
- every `Test:` line with its result and duration (the group's setup, the test, the cleanup);
- the Results Summary;
- every `NOTE` line of the test, and the lines the step names;
- for a FAIL, the failing line with the 20 lines before it. Copy the test's merged log, if there is one: `R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged -path "*$T*" -exec cp {} $L/merged-$t-$n.txt \;`.

After every run:
- no pool: `timeout 30 sudo -n $D/$t/zpool list -H -o name` prints nothing;
- no handler: the first line of `timeout 30 sudo -n $D/$t/zinject` is `No handlers registered.`;
- no held thread (the check of the VM rules);
- the kernel-line check: `sudo -n dmesg | sed -n "/task-1123-fbwrc: $t $n start/,\$p" | grep -E '<the pattern of the VM rules>'`;
- its result in `$L/progress.md`.

A pool, a handler or a held thread left: capture as for a hang, change nothing, and stop. A run that does not end as its step expects is not the expected result: quote it, say so, and go on only if nothing is left and the kernel lines are clean.

## Step 3: the release module (`ffr`)

Load `ffr`. Then:

1. `n=freesp-1`, then `n=freesp-2`, with `T=slog_replay_fs_freesp_order`. Expected each time: `[FAIL]`, with the six lines of "What each test does", no `the write waited` NOTE, and no kernel line. Quote the `cmp: EOF` and `ERROR:` lines, and say whether the `cmp` before the export passed.
2. If `kernel.panic_on_oops` is 1, set it to 0 for the rest of the task (`sudo -n sysctl kernel.panic_on_oops=0`; step 5's reboot restores it), so that an unexpected oops is printed and the VM stays up. Then `n=upgrade-1`, `T=upgrade_task_unmount`, once. Expected: `[PASS]`, the test in about 1 s, and no kernel line. Quote the test's lines from `SUCCESS: zfs unmount testpool/testfs` to `SUCCESS: zpool import -d /var/tmp testpool`.
   - A KFENCE report of a use after free (`BUG: KFENCE: use-after-free`) in `dsl_dataset_long_rele` or `dmu_objset_upgrade_task_cb` is this bug seen on a release build: quote it with its stack, then unload, and go on with step 4 if the unload works.
   - Any other kernel line, an oops or a FAIL is a stop (VM rules).
3. Unload with `sudo -n $D/ffr/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty. If the unload fails, capture as for a hang, stop and report, and do not reboot.

## Step 4: the debug module (`ffd`), `slog_replay_fs_freesp_order`

Load `ffd`. Then `n=freesp-1` and `n=freesp-2`, with `T=slog_replay_fs_freesp_order`. Expected and quoted as in step 3.1: `[FAIL]` at the `cmp` after the import, and no kernel line, as in TASK-1117-ZDP.

Do not unload: step 5 runs on this load.

## Step 5: the debug module, `upgrade_task_unmount` (crash expected; the one reboot)

Before the run:
- the `ffd` module of step 4 is still loaded, and the pre-run checks pass;
- `sysctl kernel.panic_on_oops kernel.hung_task_panic`: both 0 (steps 0 and 3);
- `L=/var/log/task-1123-fbwrc; LC_ALL=C ls -d /var/tmp/* > $L/var-tmp-ffd.txt`;
- start a capture that is flushed line by line and survives your commands: `L=/var/log/task-1123-fbwrc; setsid nohup sudo -n sh -c "dmesg --follow > $L/ffd-follow.txt" > /dev/null 2>&1 < /dev/null &`;
- `touch $L/ffd-start`, and write in `$L/progress.md`: "ffd upgrade-1 run started, reboot follows".

Then one run, under `timeout 900`:

```sh
D=/var/tmp/task-1123-fbwrc; L=/var/log/task-1123-fbwrc
echo "task-1123-fbwrc: ffd upgrade-1 start" | sudo -n tee /dev/kmsg > /dev/null
cd $D/ffd && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKOD -t upgrade_task_unmount > $L/zts-ffd-upgrade-1.log 2>&1 < /dev/null &
```

Poll dmesg every 10 s for at most 3 minutes. Expected within about 10 s of the test's start, in `dbu_evict`:
- `VERIFY3U(rc->rc_count, ==, number) failed (1 == 0)`;
- `PANIC at refcount.c:91:zfs_refcount_destroy_many()`;
- the stack that SPL dumps, with among its frames `spl_panic`, `zfs_refcount_destroy_many`, `dsl_dataset_evict_async` and `taskq_thread`.

Quote the two lines, the `Comm:` line and the first 15 frames, and the time from the run's mark to the `VERIFY3U` line.

In `$L/zts-ffd-upgrade-1.log`: the test's lines up to `SUCCESS: zfs mount testpool/testfs`, then nothing more (the `zpool export` blocks). Quote its last 20 lines.

Wait until 3 minutes after the `PANIC` line. In between, `INFO: task dbu_evict:<pid> blocked for more than <s> seconds.` and the same for `zpool` are expected: they are not a stop; quote them. Then capture into `$L/ffd-state.txt`:
- `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|zfs-tests|ksh'`;
- `timeout 5 sudo -n cat /proc/<pid>/stack` of every D task. Expected: `dbu_evict` in `spl_panic` < `zfs_refcount_destroy_many` < `dsl_dataset_evict_async`, and `zpool export` waiting behind it: quote where;
- the held-thread check (nothing expected: the test removed its handler by its ID before the export);
- `sudo -n tail -n 40 /proc/spl/kstat/zfs/dbgmsg`;
- `sudo -n dmesg | tail -n 200`.

Run no `zpool`, `zfs` or `zinject` command any more: they can block behind the stuck export (`zpool status` did in TASK-1117-ZDP). Do not wait for the runner. If it has returned when you capture, quote its result line.

Save `sudo -n dmesg > $L/dmesg-ffd.txt`. Then the reboot, below.

Not the expected crash:
- **The test PASSes** (`[PASS]`, no kernel line): quote it, and run it once more (`upgrade-2`, the same command with `n=upgrade-2`). If it passes again, do the after-run checks of steps 3 and 4, stop the `dmesg --follow` capture (`sudo -n pkill -f 'dmesg --follow'`), unload, report it as the finding, and do not reboot.
- **No kernel line within 3 minutes, and the runner has not returned:** capture as for a hang, stop and report. Do not reboot.
- **It fails before the hold or at it** (`the upgrade task did not reach its point in 60 seconds`, or a failure before `zfs unmount`): the scenario did not happen. Capture as for a hang, stop and report. Do not reboot.
- **A crash at another place, an oops, or a held thread:** capture, stop and report, and wait for main. Do not reboot.

### The reboot (step 5 only)

Nothing can unload the module now.

1. `$L/report.md` holds everything up to this point, step 5's quotes included.
2. Write `$L/RESUME.md`:

   ```
   TASK-1123-FBWRC RESUME
   reboot after step 5 (ffd upgrade_task_unmount) at <UTC time>
   next: "Step 6: after the reboot", then "The end"
   D=/var/tmp/task-1123-fbwrc L=/var/log/task-1123-fbwrc
   resume message: <its id>, due <UTC time>
   report so far: /var/log/task-1123-fbwrc/report.md
   reboot command: sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'
   ```

3. Arrange your own resumption as in TASK-1121-B24: a message scheduled to your own session about 5 minutes later, saying `TASK-1123-FBWRC: resumed after the reboot of step 5: read /var/log/task-1123-fbwrc/RESUME.md first`. Put its id in RESUME.md. If you cannot schedule it, say so in RESUME.md and reboot anyway: the session normally comes back with the VM.
4. Write the command to the log, then flush the files one by one (a plain `sync` could hang on the stuck pool):

   ```sh
   L=/var/log/task-1123-fbwrc
   echo "$(date -u +%FT%TZ) step 5: rebooting with: sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'" >> $L/progress.md
   sync -f $L/progress.md $L/report.md $L/RESUME.md $L/ffd-follow.txt $L/dmesg-ffd.txt $L/zts-ffd-upgrade-1.log $L/ffd-state.txt
   ```

5. Arm the reboot detached, so that your call returns: `sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'`, and end your turn.

If the VM went down on its own before this (a kernel panic, no reply), nothing on it captured the end. Main then reads the VM's console on the Proxmox host, and after the restart you continue with step 6.

## Step 6: after the reboot

1. Read `$L/RESUME.md`. If the scheduled message has not fired yet (you came back with the VM), delete it and say so. If it woke you, say so.
2. `uptime -s` (after the reboot time), `lsmod | grep -E '^(zfs|spl) '` (empty), `ls -l /dev/zfs 2>&1` (absent), `losetup -a` (empty).
3. `sysctl kernel.panic_on_oops kernel.hung_task_panic`: quote them; they are back to their step 0 values. If `hung_task_panic` is 1, set it to 0 again, as in step 0.
4. The kernel log up to the reboot: `L=/var/log/task-1123-fbwrc; sudo -n journalctl -k -b -1 --no-pager > $L/journal-ffd.txt 2>&1; grep -a -c -E 'VERIFY|ASSERT|PANIC|BUG:|blocked for more than' $L/journal-ffd.txt`. A journal that keeps no earlier boot says so: record it. `$L/ffd-follow.txt` holds the same lines.
5. What the interrupted run left in /var/tmp:

   ```sh
   L=/var/log/task-1123-fbwrc
   LC_ALL=C ls -d /var/tmp/* > $L/var-tmp-after-ffd.txt; LC_ALL=C comm -13 $L/var-tmp-ffd.txt $L/var-tmp-after-ffd.txt
   ```

   Every name listed is either the interrupted run's or this boot's (`systemd-private-*`, left in place). The run's names are `zpool_upgrade_test.dat` (the test's file vdev), `file-vdev0` to `2`, a `zfs-tests.*.run` file, a `zts-results.*` directory, a `constrained_path.*` directory, and possibly `testdir`. `test_results` was there before the run (steps 3 and 4) and stays. Quote `ls -ld` of each of the run's names and remove them with `sudo -n rm -rf`. Anything else is reported and left.
6. `ls -l /etc/zfs/zpool.cache 2>&1`. The test's `zpool create` uses the default cache file, so it may be above 0. Quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`. If it names only `testpool` with paths under `/var/tmp`, move it to `$L/zpool.cache.ffd` and say so. Otherwise report it and leave it. An empty one is moved the same way.
7. `sudo -n fstrim -av`, then "The end".

## The end

- No module is loaded.
- `ls -d /var/tmp/testdir /var/tmp/disk-slog /var/tmp/zpool_upgrade_test.dat /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows none of them.
- `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`.
- `losetup -a` shows no loop device of this task.
- If step 3 changed `kernel.panic_on_oops` and no reboot followed, set it back to its step 0 value.
- Delete the trees: `sudo -n rm -rf /var/tmp/task-1123-fbwrc`. If a stop needs them, keep them and say so.
- `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-fbwrc/report.md` and give the same text as your final message:

1. **Environment:** VM, kernel, nproc, RAM, free space, the sysctls of step 0 (and any you changed), the KFENCE sample interval, `trees.txt`, the step 1 lines (modes, sha256, changed files, diffstats, the readable checks), each build's rc and duration with its two configure lines, its `DEBUG_ZFS` and its warnings, the point-name counts, and each srcversion with its load line ("(DEBUG mode)" or not).
2. **Table:** build | test | run | result | duration | failing line (or what happened instead). Then one verdict line, for example: "release: slog_replay_fs_freesp_order FAIL 2/2 at the cmp after the replay (EOF after byte 131072), upgrade_task_unmount PASS 1/1 (not detected, as expected on release); debug: slog_replay_fs_freesp_order FAIL 2/2 (the same line), upgrade_task_unmount VERIFY3U failed (1 == 0), PANIC at refcount.c:91 in dbu_evict <N> s in, test stopped after `zfs mount`; 1 reboot".
3. **Quotes:** for each FAIL, the failing line with the 20 lines before it; the NOTE lines; the release `upgrade_task_unmount` lines; the kernel-line checks of steps 3 and 4; for step 5, the kernel lines and frames, the test's last 20 lines, the hung-task lines, the D tasks and their stacks.
4. **Reboot:** the time, how you resumed (scheduled message or the VM), the journal count, what was removed from /var/tmp, and where the cache file went.
5. **Deviations, cleanup,** then `End`.
