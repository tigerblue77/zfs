TASK-1121-TWT (from the coordinator session "main"): functional, about 45 min, one debug build, no reboot. M-43: the ZTS test that keeps our fix for an over-wide RAID-Z expansion from regressing. Run it on the fix 3 times, then the raidz group once. No fail-before: TASK-1121-TWP already showed master wedging on this kind of pool. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB). Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-twt`, `L=/var/log/task-1121-twt`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

Releases up to 2.4.x accept `zpool attach` of a 57th child to a RAID-Z vdev at ashift 16, which the scratch area of the expansion cannot hold; the next txg fails `VERIFY3U(write_size, >=, raidvd->vdev_children << ashift)` in `raidz_reflow_scratch_sync()`, and so does every read-write import of that pool afterwards. Master (#19365) refuses new attaches of that kind. The fix `cdccdbee036` ("raidz: stop an expansion too wide for the scratch area") parks such an expansion on import; TASK-1121-TWP validated it on this kernel on 21/11.

The branch now adds the test `raidz_expand_too_wide` (second commit, `b34acb8dd30`). Master cannot make the state, so the test ships the pool as an image: `tests/zfs-tests/tests/functional/raidz/blockfiles/raidz_expand_too_wide.tar.bz2`, 135156 bytes, a sparse tar of 57 files of 64 MiB (`d00` to `d56`) holding pool `too_wide`: a raidz1 of 56 at ashift 16, the 57th attached by the libzpool of OpenZFS 2.4.4, which aborted at the VERIFY before reflowing anything (never exported). The test extracts it under /var/tmp, then: imports it read-write with `-f` (in the background, failing after 300 s rather than hanging), waits for one `reflow too wide vdev=0 width=57` record in `zpool history -i`, checks that `zpool status` shows the expansion in progress at `0.00% done, (copy is slow, no estimated time)`, that `timeout 10 zpool wait -t raidz_expand` exits 124, that `zpool attach` of a 58th file and `zpool checkpoint` are refused with `raidz expansion in progress` and that the vdev still has 57 files; writes 128 MiB to `too_wide/fs`, scrubs (0B repaired, 0 errors), exports, imports again (second record), checks the file's xxh128, exports, and runs `zdb -e -bcc` (no leaks). Each import prints one console warning `WARNING: pool 'too_wide': raidz vdev 0 is too wide to be expanded to 57 children, its expansion is stopped`, so a run of the test adds exactly two to dmesg.

Main checked the image in user space only: with master's libzpool the read-write import aborts at the same VERIFY, with the fix's it parks (one warning, `vre_too_wide` set, txgs sync, export and re-import work), and `zdb -e -bcc` finds no leaks. This task is the first run of the test itself.

What is tested: fork https://github.com/tigerblue77/zfs, branch `raidz-expand-too-wide-park`, head `b34acb8dd304eea59b66745ddb972972bac44d1b` (the test), tree `36c7a6f14b85c5ebeaaa6ff292d445e5ef819351`; its parent `cdccdbee0365e108a42ec0f4f71a93573e9d66e8` is the fix (tree `81e1b01ab63ca8643f563874e81517015332245f`), on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`. Main pushes the branch before sending this task.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; touch no block device of the VM; never `zpool import` anything yourself (`-a` least of all); never `zfs-tests.sh -x`.
- After every run `zpool list` must show no pool. A pool this task did not create (`tank` or any other) is a stop: report it before the next run, and leave it as it is (no export, no destroy, no import). A pool `too_wide` left after a run means the test failed or was killed: record `timeout 30 sudo -n $D/zd/zpool status too_wide`, stop and report (do not export or destroy it).
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows this task's module only, `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool, `ls -d /var/tmp/raidz_expand_too_wide.* /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A non-empty cache file names a pool left behind: stop and report its `ls -l`.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A single test still going after 15 minutes, the raidz group after 60 minutes, a build after 90 minutes is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|raidz|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<tid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report. Run no zpool, zfs or zdb command, no plain `sync` and no `df` once a hang is suspected: they would sleep uninterruptibly.
- Reboots: none pre-approved; for this task that replaces the standing approval. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 15 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/raidz_expand_too_wide.* /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `id zts`, `command -v ksh jq xxh128sum bzip2` (all four; a missing one: stop and report, install nothing), `pgrep -a -x zed; pgrep -a -x lt-zed` (record; do not start or stop one).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1121-twt
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module. Trees and logs other tasks keep under /var/tmp and /var/log stay untouched (TASK-1121-TWP's $L, /var/log/task-1121-twp, included). Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1121-twt; L=/var/log/task-1121-twt
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs raidz-expand-too-wide-park && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "b34acb8dd304eea59b66745ddb972972bac44d1b cdccdbee0365e108a42ec0f4f71a93573e9d66e8 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 36c7a6f14b85c5ebeaaa6ff292d445e5ef819351 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "b73896a00a3271e73d78321894ab7862d9896ade19d9d580902301449a3b8538  module/zfs/vdev_raidz.c" "5e8d85f89fd4df26bfcbbc73853c6f4037e8aaa84ac0f6cc03952f35c50742c9  include/sys/vdev_raidz.h" "411602683a731383138b5d02cc37aab768c50a8e037c1db7f1cd9ef65e5b6573  tests/zfs-tests/tests/functional/raidz/raidz_expand_too_wide.ksh" "588e133e53141f1b768e9341b1b71655daf7dc25eb562fe32c5371f8c0a75215  tests/zfs-tests/tests/functional/raidz/blockfiles/raidz_expand_too_wide.tar.bz2" "2bba980072988562924a30f023c0570bef4ec0c3b2daa79296a4ddcaeb3074e5  tests/runfiles/common.run" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~1 HEAD; ls -l tests/zfs-tests/tests/functional/raidz/raidz_expand_too_wide.ksh tests/zfs-tests/tests/functional/raidz/blockfiles/raidz_expand_too_wide.tar.bz2 | awk '{print $1, $5}'
tar -tvjf tests/zfs-tests/tests/functional/raidz/blockfiles/raidz_expand_too_wide.tar.bz2 | awk '{ n++; s += $3 } END { print n, s }'
```

Expected TREE-OK, SHA-OK, the diffstat

```
 tests/runfiles/common.run                          |   2 +-
 tests/zfs-tests/tests/Makefile.am                  |   2 +
 .../raidz/blockfiles/raidz_expand_too_wide.tar.bz2 | Bin 0 -> 135156 bytes
 .../functional/raidz/raidz_expand_too_wide.ksh     | 169 +++++++++++++++++++++
 4 files changed, 172 insertions(+), 1 deletion(-)
```

then `-rwxr-xr-x 5682` and `-rw-r--r-- 135156`, and `57 3825205248` (57 members of 64 MiB). If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree.

## STEP 2: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1121-twt; L=/var/log/task-1121-twt
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-zd.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zd/module/zfs.ko`.

## STEP 3: the runs (tags t, tr)

`sudo -n $D/zd/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

A run, one at a time, after the pre-run checks of the rules:

```sh
cd /var/tmp/task-1121-twt/zd
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1121-twt/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Before each run note Nb=`sudo -n dmesg | wc -l`. Per run: every `Test:` line with result and duration and the Results Summary. The run's result directory is `R=$(awk '/^Log directory/ { print $3 }' $L/zts-<tag>-<n>.log)`. Then:

```sh
L=/var/log/task-1121-twt; R=$(awk '/^Log directory/ { print $3 }' $L/zts-<tag>-<n>.log)
grep -E 'bounded 300 import_stopped|still waiting|raidz expansion in progress|reflow too wide|records, expected|devices, expected|did not return|changed across|zdb -e' $R/log
sudo -n dmesg | tail -n +$((Nb+1)) | grep -c "WARNING: pool 'too_wide': raidz vdev 0 is too wide to be expanded to 57 children, its expansion is stopped"
sudo -n dmesg | tail -n +$((Nb+1)) | grep -E 'VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than'
```

On a FAIL or KILLED, copy `$R/log` to $L/log-<tag>-<n>.txt and quote the failing lines with the 30 before them. After each run: the pre-run checks again.

1. `-t raidz_expand_too_wide`, 3 runs (tag t). Expected PASS each, in about a minute (a `-t` run prints three `Test:` lines: the raidz setup, the test, the cleanup). In the grep of the log: `SUCCESS: bounded 300 import_stopped 1 -f` and `SUCCESS: bounded 300 import_stopped 2`, the NOTE `zpool wait -t raidz_expand is still waiting after 10 seconds`, the two `raidz expansion in progress` refusals (attach, then checkpoint), and `SUCCESS: zdb -e -p ... -bcc too_wide`; nothing matching `records, expected`, `devices, expected`, `did not return` or `changed across`. The warning count `2`, and no VERIFY, ASSERT, PANIC, BUG, Oops or hung task. Quote all of it per run.
2. `-T raidz` once (tag tr). Expected all PASS, `raidz_expand_too_wide` among them (between `raidz_expand_scrub_repair` and `raidz_zinject`), with the setup and the cleanup; the warning count `2` for the whole group. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted and reported as is, without rerun, saying whether tests/test-runner/bin/zts-report.py.in lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected), and the total count of `is too wide to be expanded` lines since N0 (expected 8: two per run of the test). Unload with `sudo -n $D/zd/scripts/zfs.sh -u` (quote rc) and `lsmod | grep -E '^(zfs|spl) '` (empty).

## STEP 4: cleanup

`sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/raidz_expand_too_wide.* /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, zed running or not; TREE-OK, SHA-OK, diffstat, the modes, sizes and the tar listing count; build rc and warnings; S1 and DEBUG mode; what happened to /etc/zfs/zpool.cache at steps 0 and 4.
2. Table: tag | run | test | result | duration | warnings in dmesg | key lines. Rows: t 1-3, then tr (one row per test). Then one verdict line, e.g. "raidz-expand-too-wide-park b34acb8dd30 on 6.12 debug: raidz_expand_too_wide PASS 3/3 (2 warnings each, no VERIFY), -T raidz PASS <n>/<n>".
3. Quotes: the grep output of every run, every non-PASS run with its 30 lines, the dmesg checks.
4. Deviations, cleanup, then End.
