TASK-1120-ZVH2 (from the coordinator session "main"): functional, about 30 min, debug build only, no reboot. VM 106 (Debian 13, kernel 6.12), after TASK-1119-ZVH.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-zvh2`, `L=/var/log/task-1120-zvh2`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

ZVH found that the new test of upstream PR openzfs/zfs#19847, `zvol_misc_snapdev_held`, never checks its devices. It sources only libtest.shlib, blkdev.shlib and zvol_common.shlib, so `blockdev_exists` (line 77), `blockdev_missing` (line 80) and `udev_cleanup` (line 49), defined in tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib, print `<test path>[<line>]: <name>: not found` and check nothing, in every passing run too. The fix is one include line after line 16. This task checks that the test still passes with it, and that the checks then run.

What is tested: https://github.com/openzfs/zfs `refs/pull/19847/head` = `fda72f0e1da096355b308bf7d35927fbdc7aa15f` (tree `e3d82acb14fceb60239930af2f114b8cd5cac7fb`, three commits on master `c913da86156eba3243cc1ef515e98d69887654a7`), ZVH's head, plus that one line, added by hand here and never committed (tree `cc359b57f7e41636bc67f91dee49d99a177aa083` once staged). The module source is ZVH's, so `zfs.ko` should have ZVH's srcversion `815A35AB6826541A818C8D2`.

What the checks print: `blockdev_exists` and `blockdev_missing` print nothing when they succeed. They return 0 silently, and fail the test through `log_fail` with `<device> does not exist as a block device`, `<device> exists when not expected` or `Wait timeout reached for udev_wait`. There is no SUCCESS line of theirs to quote. The evidence that they ran is: no `not found` line in the run's log; the `NOTE: Pruning broken ZVOL symlinks ...` line that `udev_cleanup` prints in the test's cleanup (the call that printed `not found` in ZVH); and the time between the `zfs set snapdev=` SUCCESS lines, which now includes `udev_wait` (`udevadm trigger --action=change`, then `udevadm settle`). With the include the test runs `udevadm trigger --action=change` and `udevadm info --cleanup-db`, as `zvol_misc_snapdev` already does: that is expected.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import` anything (`-a` least of all); never `zfs-tests.sh -x`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks: during ROV3 it imported VM 105's exported measurement pool `tank`. No run of this task includes it, but after every run `zpool list` must still show no pool. A pool this task did not create (`tank` or any other) is a stop: report it before the next run, and leave it as it is (no export, no destroy, no import).
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows this task's module only, `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A single test still going after 30 minutes, a group after 75 minutes, a build after 90 minutes is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: one is pre-approved only where a task says so, and this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `uptime -s`, `free -m`, `df -h / /var/tmp` (at least 15 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `ls -d /var/log/task-1119-zvh 2>&1` (ZVH's logs, expected present and only read here; absent is said, not a stop).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-zvh2
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

TQL's kept trees under /var/tmp stay untouched. Any other difference: stop and report.

## STEP 1: fetch, check, add the line

```sh
D=/var/tmp/task-1120-zvh2; L=/var/log/task-1120-zvh2
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 4 https://github.com/openzfs/zfs refs/pull/19847/head && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "fda72f0e1da096355b308bf7d35927fbdc7aa15f faeecb8d73576cb3c7611a52a76dbca2626a11ad 3d8a0c519957f38901e0b56558295d1a01e28467 c913da86156eba3243cc1ef515e98d69887654a7 e3d82acb14fceb60239930af2f114b8cd5cac7fb 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "51e17c3c3d7b755deda047bce46b94b3ffae65559c2489b2b0c7e9f2445bfa4c  tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh" "c777a21b1613273390570fd393c608375030dc7789fd346baf37c24c461bd5ea  tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib" > $L/sha-orig.txt
sha256sum -c --quiet $L/sha-orig.txt && echo ORIG-OK
sed -n 14,17p tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh
```

Expected TREE-OK, ORIG-OK, and lines 14 to 17: `. $STF_SUITE/include/libtest.shlib`, `. $STF_SUITE/include/blkdev.shlib`, `. $STF_SUITE/tests/functional/zvol/zvol_common.shlib`, then an empty line. If the fetch fails, wait 1 minute and try once more. Any other difference (the PR may have moved): stop and report as is.

Then the edit, exactly this command (in single quotes: `$STF_SUITE` must stay literal), and its checks:

```sh
D=/var/tmp/task-1120-zvh2; L=/var/log/task-1120-zvh2; F=tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh
cd $D/zd && sed -i '16a . $STF_SUITE/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib' $F
sed -n 14,18p $F
echo "2479f8fd760d96eb3551b12b786d4c2db5096fc2f5f45c4766778a0016e4e372  $F" | sha256sum -c --quiet && echo EDIT-OK
ls -l $F | awk '{print $1}'
git diff --stat=80
git diff --numstat
git diff | sed -n 5,11p
git add $F && git write-tree && git hash-object $F
```

Expected:

- lines 14 to 18: the three lines above, then `. $STF_SUITE/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib`, then an empty line;
- EDIT-OK and `-rwxr-xr-x`;
- `git diff --stat=80` (the width is fixed so that COLUMNS cannot change it):

```
 .../zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh | 1 +
 1 file changed, 1 insertion(+)
```

- `git diff --numstat`: `1`, `0` and `tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh`, separated by tabs;
- the hunk: `@@ -14,6 +14,7 @@`, the three context lines, `+. $STF_SUITE/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib`, an empty context line and `#`;
- `git write-tree`: `cc359b57f7e41636bc67f91dee49d99a177aa083`; `git hash-object`: `942441eeedc150e6f35bc54e88e30f7a2fa6fc57`.

Any difference: stop and report. Do not edit anything else, and do not commit.

## STEP 2: debug build (about 10 minutes)

```sh
D=/var/tmp/task-1120-zvh2; L=/var/log/task-1120-zvh2
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-zd.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Quote `modinfo -F srcversion $D/zd/module/zfs.ko`: expected `815A35AB6826541A818C8D2`, ZVH's value for the same module source (the edited file is a test). A different value is recorded and reported, not a stop.

## STEP 3: the PR's head with the include (all expected PASS)

`sudo -n $D/zd/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/zd/module/zfs.ko`) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

A run, one at a time, after the pre-run checks of the rules:

```sh
cd /var/tmp/task-1120-zvh2/zd
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1120-zvh2/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with result and duration (a `-t` run of a zvol_misc test prints three: setup, the test, cleanup) and the Results Summary. The run's result directory is `R=$(awk '/^Log directory/ { print $3 }' $L/zts-<tag>-<n>.log)`; `$R/log` holds the output of every test of the run, each line timestamped. On a FAIL or KILLED, copy `$R/log` to $L/log-<tag>-<n>.txt and quote the failing lines with the 20 before them. After each run: the pre-run checks again (no pool, no file-vdev, CACHE-OK).

1. `-t zvol_misc_snapdev_held`, 3 runs (tag sh). Expected PASS each. For each run:

   ```sh
   L=/var/log/task-1120-zvh2; n=<n>
   R=$(awk '/^Log directory/ { print $3 }' $L/zts-sh-$n.log)
   grep -c 'not found' $R/log
   grep -E 'snapdev=(visible|hidden)|does not exist as a block device|exists when not expected|Wait timeout reached for udev_wait|Pruning broken ZVOL symlinks|not found' $R/log
   ```

   Expected: `0`; then `SUCCESS: zfs create -V 64M -o snapdev=hidden testpool/heldvol`, six SUCCESS lines alternating `zfs set snapdev=visible testpool/heldvol` and `zfs set snapdev=hidden testpool/heldvol` (the three loop iterations), and one `NOTE: Pruning broken ZVOL symlinks ...`. Quote them with their timestamps, and give the gaps between consecutive `zfs set` SUCCESS lines. Any `not found` line is quoted with the 20 lines before it.

   Once, after run 1, the same two commands on ZVH's first run, read-only, for comparison: `R=$(awk '/^Log directory/ { print $3 }' /var/log/task-1119-zvh/zts-sh-1.log)`. Expected there: `7` (`blockdev_exists: not found` and `blockdev_missing: not found` three times each, at `[77]` and `[80]`, and `udev_cleanup: not found` once, which ksh prints with the line numbers of the cleanup call chain rather than 49), the same seven SUCCESS lines with shorter gaps, and no `Pruning` line. If ZVH's log or its result directory is gone, say so and go on.

   If a run FAILs at a device check (`does not exist as a block device`, `exists when not expected` or `Wait timeout reached for udev_wait`): copy `$R/log` to $L, quote the failing lines and the 20 before them, and after the three runs run the test once more (tag sh, run 4, same quotes). That is a finding about the PR's test (the snapshot's device did not appear or go away in time), not a VM problem: go on with the plan and report it as such.
2. `-t zvol_misc_snapdev.ksh` once (tag sd), with the `.ksh`: `-t` runs the first file `find` returns for `<name>*`, and `zvol_misc_snapdev*` also matches `zvol_misc_snapdev_held.ksh`. Check that its `Test:` line names `zvol_misc_snapdev.ksh`. Expected PASS: this existing test sources zvol_misc_common.kshlib and runs the same device checks on this VM. Quote `grep -c 'not found' $R/log` (expected 0).
3. `-T zvol_misc` once (tag zm). Expected all PASS: 17 lines, as in ZVH (13 tests, plus a setup and a cleanup for each of the two sections, common.run and linux.run). Quote `grep -c 'not found' $R/log` (expected 0; any line quoted with the test it belongs to). A FAIL is quoted and reported as is, without rerun, saying whether tests/test-runner/bin/zts-report.py.in lists it as known.

After them: no pool left; `sudo -n dmesg | tail -n +$((N0+1)) | grep -cE 'zl_get_data|PANIC|VERIFY'` must print 0, and every line matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it. Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 4: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment; TREE-OK, ORIG-OK, EDIT-OK, mode, diffstat, numstat, write-tree and hash-object; build rc and warnings; srcversions (against ZVH's `815A35AB6826541A818C8D2`) and DEBUG mode; what happened to /etc/zfs/zpool.cache at steps 0 and 4.
2. Table: tag | run | selection | result | duration | `not found` count | key note. Then one verdict line, e.g. "#19847 fda72f0e1 + zvol_misc_common.kshlib include: zvol_misc_snapdev_held PASS 3/3 with its device checks running (0 `not found`, udev_cleanup's Pruning note present, ZVH had 7 `not found`), zvol_misc_snapdev PASS, zvol_misc group PASS 17; no zl_get_data/VERIFY/PANIC".
3. Quotes: per sh run the grep output with timestamps and gaps; ZVH's for comparison; any device-check failure; the dmesg checks.
4. Deviations, cleanup, then End.
