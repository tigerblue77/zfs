TASK-1122-WRS (from the coordinator session "main"): functional, by day, Sunday 22/11 from about 20:05Z, on VMs 106 and 107 (Debian 13, kernel 6.12). Each VM has its own queue and its own end time (section 1). This is the first run on a kernel of our tests ported onto wrouesnel's zinject framework (the openzfs/zfs#19997 direction). Each branch gets one debug build, and each test runs 3 times. Nothing is measured and there is no fail-before (the v2 tasks do that). No reboot is pre-approved. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1122-wrs/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then go on with what section 1 says for your VM.

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `I`, `B`).

## 1. Your queue and your end time

Each item is one branch of the fork https://github.com/tigerblue77/zfs, with its test or tests. After `n` commits, its parent chain reaches `189a309d464ca51c74d627fdbdd84b5992eade2d`: wrouesnel's branch `zinject-points-uplift`, on upstream master `a1daebfec3f`. Items run in the order of the table.

| item | VM | branch | head | tree | n |
|---|---|---|---|---|---|
| s1 | 106 | zvol-open-pin-state-test-wr | 3e42136b8d9448d56eca1155a0a78d6328196595 | 73f4f8b0178dae5f7624126ff0327468267dec79 | 3 |
| s2 | 106 | zpool-split-hold-newspa-v2-wr | 32ae3d87abe7b3f01031deae09c4593370f398dd | a33a3b4a9d8fb33b36f9aaeda37539deb8654267 | 2 |
| s3 | 106 | zfs-rewrite-write-race-v2-wr | 5fcc467596e5930a47a63d65c0b3ab4dca7abb3b | fae706154bef5a433eb85ffc680e868ad46dd604 | 2 |
| s4 | 106 | setattr-replay-one-record-test-wr | 06a896ae7c51eeb4c54e64e80626be37b9f4d885 | 85562f772e98e25d42bd4f747d01a59c64f2d746 | 2 |
| s5 | 106 | zinject-delay-points-v2-wr | fe63d5068f247c229aa795a3069458d78bf3d947 | 6a57334dcba269337dbfb34ee65a9ad57e7c2f70 | 4 |
| r1 | 107 | raidz-scratch-ub-no-panic-v4-wr | 236b081d03074af2a1f36dbdd910eda63f8e8ef4 | e81936165c634e24b0269a0a13510ebd9a851dfe | 2 |
| r2 | 107 | raidz-reflow-state-lock-order-v2-wr | c30d195fb56545e838ce3a2c0413481e81bcb467 | 1c9cc94dcb14f67caf10610b88eeb9671be09865 | 2 |
| r3 | 107 | retro-19705-test-v2-wr | 0d7d8cdfe2e489d5d539543281b25c946e352161 | 2fe398507d5b9299360834c5553fc8a7cdc857db | 1 |

Tests: every file has mode `100755`. Paths are relative to `tests/zfs-tests/tests/functional/`; the name to run is the file name without `.ksh`.

| item | test file | sha256 |
|---|---|---|
| s1 | zvol/zvol_misc/zvol_misc_remove_open.ksh | f0f80d2ff128a4bd794733b3c6c01033dd4cd9963ce9806188804d4e48f7b7e7 |
| s2 | cli_root/zpool_split/zpool_split_new_pool_busy.ksh | 2e0c233b1e5f5e46bd76f6ebc529f7244ad0e3d27c804efc2981972e004fef7e |
| s3 | cli_root/zfs_rewrite/zfs_rewrite_physical_append.ksh | ee26d432cd8d25b8ec4c221976434e6e422fa19ad79eb06841d6d9ef0232d874 |
| s4 | slog/slog_replay_fs_setattr_size.ksh | ecbf08e39cc8e651e5e2e61bb2c082ba1f5ad110fb542ba367017b50b1464a67 |
| s5 | slog/slog_replay_fs_freesp_order.ksh | 4a6379e61f9f3d9dde078520e1401ec90aec3d8cbae2a108a4f8fa671fb96735 |
| s5 | upgrade/upgrade_task_unmount.ksh | cdb450e655280b2bab6d9735cc7c8e7dcc4a10b20928b35d32ac1cb519f1bca6 |
| r1 | raidz/raidz_expand_scratch_uberblock.ksh | 023039c249172640274c6df1a121da4a44476ebb4978443e2a880a8b1fc95dfd |
| r2 | raidz/raidz_expand_state_lock.ksh | fee2ece40d51995e29d534c321838a4fc6012a65d6c3f683556bab52f9783474 |
| r3 | raidz/raidz_expand_inflight_read.ksh | 4a689dfcf044cea41357759e4097448c75e0a2573575dcf0118d903d7964af0f |

**End times.**
- **VM 106:** everything of this task is over (modules unloaded, trees deleted) by **21:45Z**. F27 stays at its READY line for tonight's window N3, and this task leaves it as it found it (section 5).
- **VM 107:** everything of this task is over by **21:15Z**. Then comes section 6: X5 back at its READY line before 21:50Z.

**Start rule.**
- A build begins only if now + 25 min ≤ your end time.
- An item's runs begin only if now + 20 min ≤ your end time, or now + 35 min for s5, which has two tests.
- If a rule says no, write `<UTC> <item> SKIPPED (end time)` in `$L/progress.md` and go to section 5. Do the same for every later item.

## 2. VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`.
- **On 106,** touch nothing of F27: not `/var/tmp/task-1122-f27` and not `/var/log/task-1122-f27`. The one exception is what main's message says about `ef.txt`. **On 107,** touch nothing of X5 (`/var/tmp/task-1122-x5`, `/var/log/task-1122-x5`) beyond what section 6 says.
- **One build at a time, one ZTS run at a time.** A build may run while the runs of an earlier item go on: builds are `nice`d.
- **Before every ZTS run,** with `I` the item:
  - `lsmod | grep -E '^zfs '` shows a module whose `/sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/$I/module/zfs.ko`;
  - `timeout 30 sudo -n $D/$I/zpool list -H -o name` shows no pool;
  - the first line of `timeout 30 sudo -n $D/$I/zinject` is `No handlers registered.`;
  - `ls -d /var/tmp/testdir 2>&1` says it does not exist.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs.** A build still going after 40 minutes, or a single ZTS run after 20 minutes, is a hang.
  - Capture these into `$L/hang-<item>.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zinject|ksh'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool`, `zfs`, `zinject` and `ksh` process;
    - `timeout 10 sudo -n $D/$I/zinject`;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
  - A thread held at a zinject point (`zio_inject_pause` in its stack) stays held until its handler is removed by its ID. Say so in the report and leave that to main.
- **Kernel log.** After each item, check the new kernel lines since that item's mark (4.3) for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|WARNING|blocked for more than|possible circular|deadlock`. None are expected. Any line is a stop, after the item's unload (4.5) if the unload works.
- Run `sudo -n fstrim -av` after every deletion of a tree.

## 3. Step 0: state

On VM 107, section 6.1 comes first.

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 12 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git`;
- `ls -d /var/tmp/task-1122-wrs /var/log/task-1122-wrs /var/tmp/testdir 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty;
- `losetup -a`.

Then:

```sh
D=/var/tmp/task-1122-wrs; L=/var/log/task-1122-wrs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
echo "task-1122-wrs: start $(date -u +%FT%TZ)" | sudo -n tee /dev/kmsg > /dev/null
echo "$(date -u +%FT%TZ) start $(hostname) $(uname -r)" >> $L/progress.md
```

Write each item's start, build end, runs and end as one line each in `$L/progress.md`.

## 4. Each item

### 4.1 Fetch and check

`I` is the item, `B` its branch, `<n>` its `n`.

```sh
D=/var/tmp/task-1122-wrs; L=/var/log/task-1122-wrs; U=https://github.com/tigerblue77/zfs; I=<item>; B=<branch>
mkdir $D/$I && cd $D/$I && git init -q . && git fetch -q --depth 6 $U $B && git checkout -q FETCH_HEAD &&
echo "$I $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD~<n>) $(git status --porcelain | wc -l)" | tee -a $L/trees.txt
```

Expected: `<item> <head> <tree> 189a309d464ca51c74d627fdbdd84b5992eade2d 0`. Then, for each of the item's test files (`<p>` is its path from the tests table): `git ls-tree HEAD tests/zfs-tests/tests/functional/<p>` must show mode `100755`, and `sha256sum tests/zfs-tests/tests/functional/<p>` must match the table.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

### 4.2 Build (debug, detached)

```sh
D=/var/tmp/task-1122-wrs; L=/var/log/task-1122-wrs; I=<item>
setsid nohup sh -c "date -u +%T; cd $D/$I && ./autogen.sh && ./configure --enable-debug && nice make -j7; echo build-rc=\$?; date -u +%T" > $L/build-$I.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-$I.log` until it appears. Expected and quoted:
- `build-rc=0` and the build's duration;
- `grep -H 'injection points will be built' $L/build-$I.log` ends with `yes`. A `no` is a stop: the ported tests would report UNSUPPORTED;
- every `warning:` line outside objtool notes.

As soon as one build ends, the next item's fetch and build may begin, under the start rule, while this item's runs go on.

### 4.3 Load

- If a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it.
- Run `sudo -n $D/$I/scripts/zfs.sh`.
- Quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/$I/module/zfs.ko`.
- Quote the last `ZFS: Loaded module` line of dmesg; it must say "(DEBUG mode)".
- Write the item's mark: `echo "task-1122-wrs: $I loaded" | sudo -n tee /dev/kmsg > /dev/null`.

### 4.4 Runs

Run each test of the item 3 times, one run at a time, each detached and after the pre-run checks:

```sh
cd /var/tmp/task-1122-wrs/<item>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t <test> > /var/log/task-1122-wrs/zts-<item>-<test>-<k>.log 2>&1 < /dev/null &
```

Quote for each run:
- every `Test:` line with its result and duration (the group's setup, the test, the cleanup);
- the Results Summary;
- from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`): every `NOTE` line, every line containing `zinject` or `inject`, and every line with `FAIL`, `failed` or `ERROR`. Keep at most 40 lines per run, and say if you cut. For runs 2 and 3, quote only the lines that differ from run 1.

Expected: the test PASSes every time, and its setup and cleanup PASS. Its `wait_inject_point ... || log_fail` lines make a PASS mean that the thread really was held at the point.

Otherwise:
- **UNSUPPORTED:** the module was built without injection points. This is a stop.
- **FAIL:**
  - Copy the merged log to `$L/merged-<item>-<test>-<k>.txt`.
  - Quote the failing command, its output and the 20 lines before it.
  - Go on with the remaining runs only if the failure left no pool, no handler and no held thread. Otherwise stop.

### 4.5 After the item's runs

- `timeout 30 sudo -n $D/$I/zpool list -H -o name` shows no pool.
- The first line of `zinject` is `No handlers registered.`.
- Quote the kernel-line check since the item's mark: `sudo -n dmesg | sed -n "/task-1122-wrs: $I loaded/,\$p" | grep -E '<the pattern of section 2>'`.
- Unload with this tree's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must then be empty.
- `sudo -n rm -rf $D/$I && sudo -n fstrim -av`.

## 5. The end (both VMs)

Do this after your last item, or after a skip, or after a stop (except one that leaves a module that will not unload or a held thread: then change nothing and report).

- No module is loaded (unload as in 4.5).
- `ls -d /var/tmp/task-1122-wrs/* /var/tmp/testdir 2>&1` shows only what a stop kept for main.
- `ls -l /etc/zfs/zpool.cache 2>&1` shows the file absent or empty. If it is not empty, say so, change nothing and stop.
- `losetup -a` shows no loop device of this task.
- Delete the trees: `sudo -n rm -rf /var/tmp/task-1122-wrs`. If a stop needs them, keep them and say so.
- `sudo -n fstrim -av`, `df -h /var/tmp`.
- **VM 106 only:** run `sudo -n sh /var/log/task-1122-f27/bin/f27-drive.sh state; echo rc=$?`. Expected: `STATE OK (loaded: ; ...)` and rc 0, so F27 is back at its READY line. Write `<UTC> setup TASK-1122-WRS over, F27 READY (STATE OK)` in `/var/log/night-1122/progress.md`.

Then write the report (section 7) and end your turn with it.

- **On 106,** nothing more follows: tonight's N3 window and the night file apply as before.
- **On 107,** create a one-shot scheduled task of your own session 2 minutes ahead (text `WRS continue X5`) before you end that turn. When it arrives, do section 6.2.

## 6. VM 107 only: X5

**6.1 Before step 0.** X5's modules are loaded: that is its READY line.
- Unload them the way X5's step 6 does, with the assignments and the PATH of X5's step 3: `sudo -n sh <X5's L>/bin/xp-drive.sh x5 end; echo rc=$?`.
- Expected: rc 0, `npools at the end: 0`, and the `end: modules unloaded, ... sysctls restored` line. This also removes `fiofast` and puts the two sysctls back. Nothing else of X5 is touched: its tree, kit and logs stay.
- Write `<UTC> setup X5 modules unloaded for TASK-1122-WRS (main's message)` in `/var/log/night-1122/progress.md`.

**6.2 After section 5.** Do it by 21:15Z, or at once after a stop that left no module loaded and no held thread. Run X5's step 3 again, in full and as written, with one change.
- Before step 3, write a new mark: `echo "task-1122-x5: re-prep $(date -u +%FT%TZ)" | sudo -n tee /dev/kmsg > /dev/null`. In step 3's kernel-line check, read the log from that mark (`/task-1122-x5: re-prep/`) instead of `/task-1122-x5: start/`, because this task's debug runs came after X5's first mark.
- Expected: what X5's step 3 expects, with the same srcversion `AA2E86BD194B0901550FC42` as at 17:10Z.
- Then X5's step 4: end your turn with the interim results of this re-run of step 3 (load, state, both smoke passes), with its READY line as the very last line.
- Write `<UTC> setup X5 READY again (re-prep after TASK-1122-WRS)` in `/var/log/night-1122/progress.md`.

Tonight's N1 window then applies unchanged (22:00Z, the night file's deferred GO).

A stop in 6.2 follows X5's own stop rules: report it and wait for main. A stop of this task that leaves a module loaded or a thread held means no 6.2: report it, and main decides.

## 7. Report

Write `/var/log/task-1122-wrs/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space. Then, per item: its `trees.txt` line, the test modes and sha256 OK, the build's rc and duration with its injection-points line and its warnings, and the srcversion with "(DEBUG mode)".
2. **Table:** item | test | run | result | duration | key note. Then one verdict line per item, e.g. "s2 zpool-split-hold-newspa-v2-wr: zpool_split_new_pool_busy PASS 3/3 (debug)", or SKIPPED with the reason.
3. **Quotes:** the NOTE and zinject lines (run 1 in full, then what differs), every non-PASS detail, and the kernel-line checks.
4. **What did not run, deviations, cleanup:** on 106, F27's STATE OK line; on 107, X5's `end` line. Then `End`.
