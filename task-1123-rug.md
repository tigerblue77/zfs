TASK-1123-RUG (from the coordinator session "main"): functional, about 1 h 15 with its builds, at most 2 h, no reboot expected. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It checks the takeover of upstream PR #19478 by sairex53 ("vdev removal: unallocate unused copy gaps immediately"): his fix, squashed onto master, together with our regression test, against master plus the test alone. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rug/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

A removal copies the free gaps between nearby allocated segments (up to `vdev_removal_max_span`, 32K by default) along with them, in one allocation on another vdev.

- **The bug (issue #19442):** on master the gaps are then freed with `zio_free()`, so they cannot be allocated again for `TXG_DEFER_SIZE` txgs, and a fast removal can run the destination out of space. Reaching that state trips an `ASSERT` in `spa_vdev_copy_impl()` on a debug build, so the test does not go that far.
- **The fix (sairex53's commit):** the gaps are given back to the destination metaslab in the txg of the copy (`metaslab_unalloc_range()`), and are only freed as before while that metaslab is condensing.
- **Our test, `removal_gaps_unallocated`:** a pool without log space maps and with a special vdev; a file written one 16K block per txg with one allocator, every other block overwritten; a second normal vdev (vdev 2) added and the first removed. The copies are then the only allocations on vdev 2, and `zdb -mmmm` shows its space maps. With the fix, they record the copied data only and no free. On master, they record each copy whole, then one free per gap, and the test fails. The destination is never short of space, so neither tree can reach the `ASSERT` above.

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parents down to master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` |
|---|---|---|---|---|
| zd | zpool-remove-unalloc-gaps | d5a9e2d29c6c2e712eba56bcc6035a986e2ab8e1 | dd46f705ee5587e60faf9cf2d1119d32454c99a4 | HEAD^ `d68613427623e38bfb57f1478203a0e4764cd44d` (the fix, author Eduard Zateev), HEAD~2 = master |
| zf | zpool-remove-unalloc-gaps-fb | 8a30c53647284dec6cf357ef5e0f705b22f0c060 | 23aef998734c82929114702250ee00696714e01c | HEAD^ = master (the test alone) |

The test file has mode `100755` and sha256 `394d548f76fe1cbba0e79e6d76a215aa07f2ff252b3f6a93cbe8b7eb442c8548` in both trees. Its path is `tests/zfs-tests/tests/functional/removal/removal_gaps_unallocated.ksh`.

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
- `ls -d /var/tmp/task-1123-rug /var/log/task-1123-rug /var/tmp/testdir 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty;
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-rug; L=/var/log/task-1123-rug; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
for t in "zd zpool-remove-unalloc-gaps 2" "zf zpool-remove-unalloc-gaps-fb 1"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 4 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~$3) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
for n in zd zf; do (cd $D/$n && p=tests/zfs-tests/tests/functional/removal/removal_gaps_unallocated.ksh; echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64)"); done
cd $D/zd && git log -2 --format='%an | %s' && git diff --stat HEAD~2 HEAD | tail -1
```

Expected:

```
zd d5a9e2d29c6c2e712eba56bcc6035a986e2ab8e1 dd46f705ee5587e60faf9cf2d1119d32454c99a4 d68613427623e38bfb57f1478203a0e4764cd44d a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
zf 8a30c53647284dec6cf357ef5e0f705b22f0c060 23aef998734c82929114702250ee00696714e01c a1daebfec3f5ca46ac261ef339efdf0dbeae997c a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
```

Then two lines with mode `100755` and the sha256 above. Then `Tigerblue77 | ZTS: test that removal gives back the gaps it copies`, `Eduard Zateev | vdev removal: unallocate unused copy gaps immediately`, and `6 files changed, 183 insertions(+), 1 deletion(-)`.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: the two debug builds, at once (about 10 to 20 minutes)

Both trees are built with `--enable-debug`, zf included: the test keeps the destination far from full, so master cannot reach the `ASSERT` in `spa_vdev_copy_impl()`, and its failure is the test's own measured line. The same build type on both trees leaves the fix as the only difference.

```sh
D=/var/tmp/task-1123-rug; L=/var/log/task-1123-rug
for n in zd zf; do setsid nohup sh -c "date -u +%T; cd $D/$n && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$n.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-z?.log` until there are two lines; each must read `build-rc=0`. Quote both durations and every `warning:` line outside objtool notes.

## Step 3: the fix (zd), all expected PASS

**Load zd:**
- `sudo -n $D/zd/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zd/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- quote `cat /sys/module/zfs/parameters/spa_num_allocators` (the default is 4; the test sets it to 1 and restores it);
- write the mark: `echo "task-1123-rug: zd loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**Runs,** one at a time, after the pre-run checks:

```sh
cd /var/tmp/task-1123-rug/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-rug/zts-<tag>-<k>.log 2>&1 < /dev/null &
```

Quote for each run:
- every `Test:` line with its result and duration;
- the Results Summary;
- from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`): every `NOTE` line of `removal_gaps_unallocated`, and every line with `FAIL`, `failed` or `ERROR`.

On a FAIL, copy the merged log to `$L/merged-<tag>-<k>.txt` and quote the failing command, its output and the 20 lines before it.

1. `removal_gaps_unallocated`, 3 runs (tag `zd-gaps`, `<sel>` = `-t removal_gaps_unallocated`). Expected: PASS each time, in a few minutes at most, with this note (the numbers are those of a userspace replay of the test; quote them as they come):
   `NOTE: mapped 786432 bytes with 262144 bytes of gaps; vdev 2 allocated 524288 bytes, freed 0 bytes in 0 frees`
2. The `removal` group, one run (tag `removal`, `<sel>` = `-T removal`). Expected: all PASS, `removal_gaps_unallocated` included. If a test skips or fails, say which and with what line; whether it does the same on master is left to main.

After these runs:
- no pool left;
- `cat /sys/module/zfs/parameters/spa_num_allocators` gives the value quoted at load;
- the kernel-line check since the zd mark;
- unload with zd's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 4: the test alone (zf), expected FAIL

Load zf as in step 3, with the mark `task-1123-rug: zf loaded`. Then one run of the test: tag `zf-gaps` (`<sel>` = `-t removal_gaps_unallocated`).

Expected: FAIL, at the test's check of the frees, in a few minutes at most. The merged log shows the note, then the failing line:

```
NOTE: mapped 786432 bytes with 262144 bytes of gaps; vdev 2 allocated 786432 bytes, freed 262144 bytes in 16 frees
vdev 2 freed 262144 bytes in 16 frees instead of giving back 262144 bytes of gaps
```

Quote both lines as they come; the numbers may differ, the failing check may not. The test's cleanup must leave no pool: quote `zpool list` and the cleanup result. No kernel `ASSERT` or `VERIFY` line is expected on this tree either.

A run that passes, or fails elsewhere (for example at `the removal copied no gap`), is not the expected fail-before: quote it and say so.

Then:
- `cat /sys/module/zfs/parameters/spa_num_allocators` gives the value quoted at load;
- the kernel-line check since the zf mark;
- unload with zf's `scripts/zfs.sh -u`; `lsmod` must be empty;
- `sudo -n rm -rf /var/tmp/task-1123-rug`, `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-rug/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, hung_task_timeout_secs, `trees.txt`, the test modes and sha256, the log and diffstat lines, the build rc and durations with their warnings, both srcversions with "(DEBUG mode)", and the `spa_num_allocators` values.
2. **Table:** tag | tree | run | selection | result per test (or "all N PASS") | duration | key note. Then one verdict line, for example: "fix: removal_gaps_unallocated PASS 3/3, removal group PASS; test alone on master: FAIL at the frees check (16 frees, 262144 bytes)".
3. **Quotes:** the NOTE line of each run of the new test (run 1 in full, then what differs), the non-PASS lines, and the kernel-line checks.
4. **Deviations, cleanup,** then `End`.
