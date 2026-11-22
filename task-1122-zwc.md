TASK-1122-ZWC (from the coordinator session "main"): functional, any VM on Debian 13 / kernel 6.12 (cgroup v2), about 1 h 30 builds included. The new ZTS test of our ZIL series v10a (fork branch `zil-issue-v10a-rb1122`), `functional/vdev_disk/zil_write_cgroup`: a ZIL block write must not be charged to the io cgroup of the writer that commits it. Three debug trees, the test three times on each: (A) master plus only the test commit: PASS; (B) the series' first commit plus only the test commit: FAIL, about 300 writes charged; (C) the series' head: PASS, then `-T vdev_disk` and `-T slog` once. The test needs a block device in `DISKS`: it gets one from the harness's own loop devices, and every run must show that it did (section "Not SKIP"). No kernel error and no reboot are expected. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1122-zwc`, `L=/var/log/task-1122-zwc`.

## Why

Master issues every ZIL block write from the write issue taskq `z_wr_iss`, kernel threads in the root io cgroup. Commit 1 (`54144be5df0`, "Issue ZIL block writes from the thread that writes them out") runs the issue stages of a ZIL block write in the thread that commits it (`zio_issue_async()` returns the zio instead of dispatching it). From Linux 5.18 `bio_alloc()` gives a bio the io cgroup of the thread that allocates it, so with commit 1 alone the log blocks of every writer of a dataset are charged to whichever writer commits them. Commit 2 (`98e18b13d0a`, "Issue ZIL block bios as the write issue taskq does") marks ZIL block writes `ZIO_FLAG_ZILWRITE` and gives their bios the root blkg, moved in from a clone of a bio of the same device that has it (`vd_pool_bio` in `vdev_disk.c`, taken at open or by ZFS's own kernel threads), and best-effort priority 0. Commit 3 (`38aead4a43e`, "Base the ZIL commit timeout on block close while commits queue") does not touch what the test checks.

The test (commit 4, `d4de314ccd0`, "ZTS: check that ZIL block writes stay out of the writer's cgroup") calls `log_unsupported` (the runner reports SKIP) without cgroup v2 at /sys/fs/cgroup, without `io` in its `cgroup.controllers`, or when the first name of `$DISKS` is not a block device under /dev. Otherwise: `zpool create -f testpool <that device>`, `zfs create -o sync=always testpool/testfs`, `zpool sync`; `+io` into the root `cgroup.subtree_control` if `io` is not there yet (`-io` again at its cleanup); a cgroup `/sys/fs/cgroup/zfs_tests_zil_write_cgroup`; from that cgroup, `dd if=/dev/urandom of=/testpool/testfs/file bs=4k count=300`. It then sums the `wios=` fields of every device line of that cgroup's `io.stat`, logs `NOTE: writes charged to the writer's cgroup: N`, and fails with `N writes charged to the writer's cgroup` when N is above 3. Each 4K write is committed at once and writes one log block, so log blocks charged to dd add up to about 300.

The same check by hand (TASK-1118-V6CG, Z1: a null_blk device, O_DSYNC dd from a scope of its own) counted 0 writes on master, 300 with commit 1's equivalent alone (v5) and 0 with the equivalent of commits 1 and 2 (v6). Expected here: (A) PASS with N 0; (B) FAIL with N about 300 (on a loop device the loop driver writes its backing file on behalf of the bio's cgroup, so writes of the backing disk may be charged as well and N may be higher: any N above 3 is the expected FAIL); (C) PASS with N 0.

What is tested: fork https://github.com/tigerblue77/zfs, on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, three trees:

- `h` (part C): branch `zil-issue-v10a-rb1122`, head `d4de314ccd0c3478927b7fd8c5ce780e0eaae749`, tree `34dd78ed8b0afadf6e2e1ad2a26638545a6626be`: commits 1 to 4 on master.
- `mt` (part A): branch `zwc-master-test`, head `5fd89179b7cd3e1c24257b76323815001221c919`, tree `fdbc9dcb2570cb241ee078522121e25eef8a1a6b`: master plus the test commit only, cherry-picked unchanged (same message), parent `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`.
- `c1` (part B): branch `zwc-c1-test`, head `149cdf5ece0749d3672d50bec201100afa146715`, tree `0970541a17eb688e735aadc6f9770e986eb97966`: commit 1 plus the test commit only, parent `54144be5df0ad5e495744bb0d926a08cbfe4f7bb`.

Main pushes both `zwc-*` branches before this task starts; they exist for this task only.

## Not SKIP: where the block device comes from, and how each run proves it

`scripts/zfs-tests.sh` run without `-f` and without `DISKS` creates its own sparse files `/var/tmp/file-vdev0..2` (4G each), puts a loop device on each (`losetup --show -f`) and exports `DISKS` as their bare names, normally `loop0 loop1 loop2`: every earlier ZTS run on these VMs did this (TASK-1115-AK, TASK-1117-V5F), as the fork's CI does. The test takes the first name, `/dev/loop0`, a block device, and creates its pool on it, so its ZIL blocks go through `vdev_disk.c` and the block layer's io cgroup accounting. With `-f` the harness hands out the files themselves and the test is SKIP. So: never pass `-f`, `-d` or `-s`, never set `DISKS`, `FILES`, `FILEDIR` or `LOOPBACKS`, and start `zfs-tests.sh` exactly as below, through `sudo -n -u zts`, which does not pass your environment on.

A run of the test, alone or in a group, counts only when all four hold; quote them for each run:

1. The run's console log `$L/zts-<tag>.log` has a `LOOPBACKS:` line naming `/dev/loopN` devices and a `DISKS:` line naming the same devices as bare `loopN` names, not `/var/tmp/file-vdev*` paths.
2. The `Test:` line of `zil_write_cgroup` ends `[PASS]` or `[FAIL]`, never `[SKIP]` or `[KILLED]`.
3. The test's own output (its `stdout` file, and `stderr` if there is one, in its directory under the `Log directory:` the run prints) has `SUCCESS: zpool create -f testpool loopN` with the first name of that `DISKS:` line, `SUCCESS: mkdir /sys/fs/cgroup/zfs_tests_zil_write_cgroup`, `SUCCESS: test -f /sys/fs/cgroup/zfs_tests_zil_write_cgroup/io.stat`, the `SUCCESS: sh -c echo $$ > ...` line of the dd (its text spans two lines) and `NOTE: writes charged to the writer's cgroup: N`.
4. It has none of the four unsupported messages: `cgroup v2 is not mounted`, `the io controller is not available`, `is not a block device`, `cannot enable the io controller`.

For a run with tag `<tag>`:

```sh
L=/var/log/task-1122-zwc; tag=mt-1   # the run's tag
grep -E '^(LOOPBACKS|DISKS):' $L/zts-$tag.log
grep -E '^Test: .*zil_write_cgroup' $L/zts-$tag.log
R=$(sed -n 's/^Log directory:[[:space:]]*//p' $L/zts-$tag.log); echo "R=$R"
for f in $(find "$R" -path '*vdev_disk/zil_write_cgroup*' -type f \( -name stdout -o -name stderr \)); do echo "== $f"; grep -nE 'zpool create|zfs_tests_zil_write_cgroup|SUCCESS: sh -c|exec dd|writes charged|not mounted|io controller|not a block device|^ERROR|ERROR:' "$f" | cut -c1-220; done
```

A SKIP, or a run missing one of 1 to 4: quote the run's configuration block (from `--- Configuration ---` to `Missing util(s):`), the test's whole output, `cat /sys/fs/cgroup/cgroup.controllers` and `sudo -n losetup -a`, then stop and report. Do not give the test a block device yourself (no `DISKS`, no brd, no null_blk, no disk of the VM): main decides.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own sparse files and loop devices (see above); touch no other block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh -x`.
- The test itself writes `+io` and, at its cleanup, `-io` to `/sys/fs/cgroup/cgroup.subtree_control` when `io` was not there, and creates and removes `/sys/fs/cgroup/zfs_tests_zil_write_cgroup`: expected. Outside the test, change no cgroup setting, except the restores this task names.
- Before every ZTS run (the pre-run checks): `lsmod | grep -E '^zfs '` shows one module and `cat /sys/module/zfs/srcversion` is the current part's; `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool; `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/disk-slog /var/tmp/disk2-slog 2>&1` shows nothing; `sudo -n losetup -a | grep -c file-vdev` prints 0; `ls -d /sys/fs/cgroup/zfs_tests_zil_write_cgroup 2>&1` shows nothing; `cat /sys/fs/cgroup/cgroup.subtree_control` equals C0 of step 0 (but see "After each run"); `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file, a leftover loop device or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l` or output line, touch nothing, wait for main.
- The ZTS failure callback clears the kernel log (`callbacks/zfs_dmesg.ksh` runs `dmesg -c`), and part B fails on purpose: so every kernel log check of this task reads the follower file `$L/dmesg-follow.txt` of step 3, never `dmesg` itself.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; `-T vdev_disk` after 20 minutes; `-T slog` after 60 minutes. Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|z_|zfs|zpool|ksh|loop|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `tail -n 150 $L/dmesg-follow.txt` into $L/hang-<tag>.txt, then stop and report.
- No reboot is pre-approved: a hang, an oops, a panic or a module that will not unload is captured as above, reported, and main decides.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded and udev helpers of this task removed at the end.

## STEP 0: state

`hostname`, `uname -r` (must start with `6.12.`), `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 30 GiB free on /var/tmp: three trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task with `sudo -n sysctl -w kernel.hung_task_panic=0` and say so), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (empty: a running zed is a stop; do not start or stop zed), `pgrep -af 'dmesg --follow'` (empty expected; one left by an earlier task: quote it and leave it), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/disk-slog /var/tmp/disk2-slog 2>&1` (none), `sudo -n losetup -a` (quote; none backed by `/var/tmp/file-vdev*`), `ls -l /etc/zfs/zpool.cache 2>&1`.

The test's prerequisites, each quoted:

```sh
stat -fc %T /sys/fs/cgroup                              # cgroup2fs
cat /sys/fs/cgroup/cgroup.controllers                   # must contain io
cat /sys/fs/cgroup/cgroup.subtree_control               # C0: note it; with or without io, both are fine
ls -d /sys/fs/cgroup/zfs_tests_zil_write_cgroup 2>&1    # absent
grep -E '^CONFIG_(BLK_CGROUP|BLK_DEV_LOOP)=' /boot/config-$(uname -r)   # BLK_CGROUP=y, BLK_DEV_LOOP=y or m
test -x /sbin/losetup && echo LOSETUP-OK
sudo -n -u zts env | grep -E '^(DISKS|FILES|FILEDIR|LOOPBACKS)='; echo "zts-env-rc=$?"   # nothing, zts-env-rc=1
```

Stop and report, changing nothing, if: the kernel is not 6.12; the type is not `cgroup2fs`; `io` is not in `cgroup.controllers`; `CONFIG_BLK_CGROUP` is not `y`; LOSETUP-OK is missing; zts's environment names one of those variables; the test's cgroup directory exists.

Udev: `ls -l /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1`. If none of them exists (no zvol udev rules, so no `/dev/zvol` links, which three `slog_replay_volume*` tests use), note H=yes: step 5 installs the in-tree helpers just before `-T slog` and step 6 removes them. A symlink whose target is gone (`readlink -e` prints nothing) counts as absent (H=yes). Anything else that exists: H=no, say what it is, and do not run `zfs-helpers.sh` in this task.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched.

## STEP 1: the trees

```sh
D=/var/tmp/task-1122-zwc; L=/var/log/task-1122-zwc
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/h && cd $D/h
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zil-issue-v10a-rb1122 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~4) $(git status --porcelain | wc -l)"
#   d4de314ccd0c3478927b7fd8c5ce780e0eaae749 34dd78ed8b0afadf6e2e1ad2a26638545a6626be a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -5
#   d4de314ccd0 ZTS: check that ZIL block writes stay out of the writer's cgroup
#   38aead4a43e Base the ZIL commit timeout on block close while commits queue
#   98e18b13d0a Issue ZIL block bios as the write issue taskq does
#   54144be5df0 Issue ZIL block writes from the thread that writes them out
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
```

The two other trees, cloned from `h`, which holds both parents:

```sh
D=/var/tmp/task-1122-zwc
git clone -q $D/h $D/mt && cd $D/mt
git fetch -q https://github.com/tigerblue77/zfs zwc-master-test && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~1) $(git status --porcelain | wc -l)"
#   5fd89179b7cd3e1c24257b76323815001221c919 fdbc9dcb2570cb241ee078522121e25eef8a1a6b a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git clone -q $D/h $D/c1 && cd $D/c1
git fetch -q https://github.com/tigerblue77/zfs zwc-c1-test && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~1) $(git status --porcelain | wc -l)"
#   149cdf5ece0749d3672d50bec201100afa146715 0970541a17eb688e735aadc6f9770e986eb97966 54144be5df0ad5e495744bb0d926a08cbfe4f7bb 0
```

Then, for the three trees:

```sh
D=/var/tmp/task-1122-zwc; T=tests/zfs-tests/tests/functional/vdev_disk/zil_write_cgroup.ksh
for t in mt c1 h; do cd $D/$t && echo "== $t: $(git log -1 --format=%s) |$(git diff --shortstat HEAD~1 HEAD)"; sha256sum module/zfs/zio.c module/os/linux/zfs/vdev_disk.c module/zfs/zil.c $T; echo "$(grep -c 'zb_object == ZB_ZIL_OBJECT' module/zfs/zio.c) $(grep -c vd_pool_bio module/os/linux/zfs/vdev_disk.c) $(grep -c lwb_close_timestamp include/sys/zil_impl.h) $(grep -c zil_write_cgroup tests/runfiles/common.run) $(grep -c zil_write_cgroup tests/zfs-tests/tests/Makefile.am) $(test -x $T && echo X)"; done
```

Expected, each tree: `== <t>: ZTS: check that ZIL block writes stay out of the writer's cgroup | 3 files changed, 86 insertions(+), 1 deletion(-)`; the test file `5ab3660e378afcbcd2cae80028b62bdcb766f59778229f3d9a767f09d2af87dd` in all three; then:

| tree | zio.c | vdev_disk.c | zil.c | counts line |
|---|---|---|---|---|
| mt | `be31510828342e582cdc11c93ed51d175c177e3d10b7ffe7d285ad1d5876e32d` | `05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86` | `a4b2910a3331bfba09cae55d8710a61eb9ded2165dac0792992febe0c9618c85` | `0 0 0 1 1 X` |
| c1 | `82f2ad8dbc003c3c6f33f7a8776cd76991ddc766ef3ab97df39ede7a60a5f84a` | `05a9989eb31dcc92e53fd779290d5181a12aac7925bc273457af49786747cf86` | `a4b2910a3331bfba09cae55d8710a61eb9ded2165dac0792992febe0c9618c85` | `1 0 0 1 1 X` |
| h | `15e2e279dd809102b98b1dfc3e7dab4cf5b93a2aa99b35b84c9975c4de87d034` | `b75cce01d810530b93e11545cd37d5857635ffe71bf98c47e0d2bc21fd244ad0` | `50d718ee437a6c53f20412ffe76377c7344a395f71944cd3c1198dcb0328b9cf` | `1 7 1 1 1 X` |

(The counts: commit 1's inline issue in `zio_issue_async()`, commit 2's `vd_pool_bio`, commit 3's `lwb_close_timestamp`, the test in `common.run` and in `Makefile.am`, `X` when the test is executable.)

Stop and report if a hash, a count or a subject differs (a branch may have moved, or main has not pushed yet). If a fetch fails, wait 1 minute and try once more. Do not edit any file of the trees.

## STEP 2: builds (the three at once, about 30 minutes)

```sh
D=/var/tmp/task-1122-zwc; L=/var/log/task-1122-zwc
setsid nohup sh -c "cd $D/mt && ./autogen.sh && ./configure --enable-debug && nice make -j3; echo build-rc=\$?" > $L/build-mt.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/c1 && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-c1.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/h && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-h.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S_MT, S_C1, S_H = `modinfo -F srcversion $D/<tree>/module/zfs.ko`; all three must differ.

## STEP 3: part A, master plus the test (debug)

Start the kernel log follower, once for the whole task, and keep its PID:

```sh
L=/var/log/task-1122-zwc
setsid nohup sudo -n sh -c "exec dmesg --follow > $L/dmesg-follow.txt" > /dev/null 2>&1 < /dev/null &
sleep 2; pgrep -n -f '^dmesg --follow$' > $L/follower.pid; cat $L/follower.pid
```

`sudo -n $D/mt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S_MT) and the "(DEBUG mode)" load line of the follower file. Note K0=`wc -l < $L/dmesg-follow.txt`.

The test, three times, one after the other, each after the pre-run checks:

```sh
D=/var/tmp/task-1122-zwc; L=/var/log/task-1122-zwc; n=1   # then 2, 3
cd $D/mt && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zil_write_cgroup > $L/zts-mt-$n.log 2>&1 < /dev/null &
```

Expected PASS each time, in well under a minute, with N 0. For each run quote: the `Test:` line (result and duration); the four "Not SKIP" items and N; then "After each run" below. A FAIL: quote the last 40 lines of its console log and of the test's output, then go on to the next run; two FAILs in a row: stop and report.

### After each run (all parts)

- The follower lines since the previous mark matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than|list_del|WARNING|segfault`, apart from the module's own "(DEBUG mode)" lines, each with 10 lines before it (none expected); then take the new mark (`wc -l < $L/dmesg-follow.txt`).
- `cat /sys/fs/cgroup/cgroup.subtree_control` equals C0. If it holds `io` while C0 did not: quote the test output's `NOTE: could not disable the io controller again` line if there is one, run `echo -io | sudo -n tee /sys/fs/cgroup/cgroup.subtree_control` once, quote its result and the file again; if `io` stays, go on (the test accepts `io` already on and then leaves it alone), compare the next pre-run checks with the file as it now is, and report it.
- `ls -d /sys/fs/cgroup/zfs_tests_zil_write_cgroup 2>&1` shows nothing. If the directory is there: quote `cat /sys/fs/cgroup/zfs_tests_zil_write_cgroup/cgroup.procs`; empty: `sudo -n rmdir /sys/fs/cgroup/zfs_tests_zil_write_cgroup` once and say so; a process listed: stop and report.
- `sudo -n losetup -a | grep -c file-vdev` prints 0 and `ls -d /var/tmp/file-vdev* 2>&1` shows nothing (the harness detaches and removes its own at its end).

After the three runs: the pre-run checks once more. Unload with `sudo -n $D/mt/scripts/zfs.sh -u` (quote rc, 0 expected; `lsmod | grep -E '^(zfs|spl) '` empty).

## STEP 4: part B, commit 1 plus the test (debug), the expected failure

`sudo -n $D/c1/scripts/zfs.sh`; srcversion equal to S_C1 and the "(DEBUG mode)" load line; take a new follower mark. The test three times, each after the pre-run checks:

```sh
D=/var/tmp/task-1122-zwc; L=/var/log/task-1122-zwc; n=1   # then 2, 3
cd $D/c1 && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zil_write_cgroup > $L/zts-c1-$n.log 2>&1 < /dev/null &
```

Expected within a minute each time: FAIL, with the four "Not SKIP" items present, `NOTE: writes charged to the writer's cgroup: N` with N about 300, then `N writes charged to the writer's cgroup` (the message of `log_fail`; the failure callbacks' dumps and the cleanup's lines come after it). For each run quote: the `Test:` line; the four items and N; that failing line; and say whether any `ERROR:` line comes before it (none expected: the failure must be the count and nothing before it). Then "After each run" (the cleanup runs on failure too: no pool, no cgroup directory and `subtree_control` back to C0 expected). A run that PASSES instead: quote the test's whole output, report it as the scenario not reproduced, and go on with the next run (no rerun beyond the three). A run that fails at an `ERROR:` line instead of the count: quote 40 lines of its output and go on.

After the three runs: the pre-run checks once more. Unload with `sudo -n $D/c1/scripts/zfs.sh -u` (quote rc, 0 expected).

## STEP 5: part C, the head (debug)

`sudo -n $D/h/scripts/zfs.sh`; srcversion equal to S_H and the "(DEBUG mode)" load line; take a new follower mark.

1. The test three times, each after the pre-run checks, tags `h-1` to `h-3`:

   ```sh
   D=/var/tmp/task-1122-zwc; L=/var/log/task-1122-zwc; n=1   # then 2, 3
   cd $D/h && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zil_write_cgroup > $L/zts-h-$n.log 2>&1 < /dev/null &
   ```

   Expected and quoted as in part A: PASS, N 0, the four items; then "After each run". A FAIL: quote the last 40 lines of its console log and of the test's output, then go on; two FAILs in a row: stop and report.
2. `-T vdev_disk` once (tag hv), after the pre-run checks: `cd $D/h && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T vdev_disk > $L/zts-hv.log 2>&1 < /dev/null &`. Expected: `page_alignment` and `zil_write_cgroup` PASS (Results Summary `PASS 2`). Quote both `Test:` lines with duration and the Results Summary; `zil_write_cgroup` here is a fourth head run: the four "Not SKIP" items and N, with tag `hv`. Then "After each run".
3. If H=yes: `cd $D/h && sudo -n ./scripts/zfs-helpers.sh -i`, then quote `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id` (symlinks into `$D/h`).
4. `-T slog` once (tag hs), after the pre-run checks: `cd $D/h && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T slog > $L/zts-hs.log 2>&1 < /dev/null &`. Expected: the setup, the 27 tests of `common.run` and the cleanup PASS (Results Summary `PASS 29`). Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before its first `ERROR:` in its own output and reported as is, without rerun; this tree's `tests/test-runner/bin/zts-report.py.in` lists no `slog` and no `vdev_disk` test as known. If H=no and `slog_replay_volume`, `slog_replay_volume_split_boundary` or `slog_replay_volume_split_write` fails at `dd ... of=/dev/zvol/testpool/testvol` for want of the link: the environment (TASK-1116-C1 saw exactly these three fail that way, on a release and a debug build alike, on a VM without the rules), say so. Then "After each run".

After them: the pre-run checks once more. Unload with `sudo -n $D/h/scripts/zfs.sh -u` (quote rc, 0 expected; `lsmod | grep -E '^(zfs|spl) '` empty).

## STEP 6: end and restore

Stop the follower: `sudo -n kill $(cat $L/follower.pid)`; `pgrep -af 'dmesg --follow'` shows only what step 0 found. Keep $L/dmesg-follow.txt.

If H=yes: `cd $D/h && sudo -n ./scripts/zfs-helpers.sh -r`, then `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id /usr/sbin/mount.zfs 2>&1` (none left).

Restore and check, quoting each:

- `cat /sys/fs/cgroup/cgroup.subtree_control` equals C0. If `io` is in it and was not in C0: `echo -io | sudo -n tee /sys/fs/cgroup/cgroup.subtree_control` once, quote the result and the file again; if it stays, report it and leave it.
- `ls -d /sys/fs/cgroup/zfs_tests_zil_write_cgroup 2>&1`: nothing.
- `sudo -n losetup -a` (none backed by `/var/tmp/file-vdev*`); `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/disk-slog /var/tmp/disk2-slog 2>&1` (none).
- If step 0 set hung_task_panic to 0: `sudo -n sysctl -w kernel.hung_task_panic=1`.

Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`; `ls -l /etc/zfs/zpool.cache 2>&1` (absent or empty expected; empty: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it). Logs stay in $L; the ZTS results stay under /var/tmp/test_results.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, nproc, RAM; C0, `cgroup.controllers`, the CONFIG lines, zts-env-rc, H; the three tree lines and the per-tree check (OK or what differs); build rcs and warnings; S_MT, S_C1, S_H and the three load lines.
2. One table of every `zil_write_cgroup` run, ten rows (mt-1 to mt-3, c1-1 to c1-3, h-1 to h-3, hv): result, duration, the `DISKS:` line, the device of `zpool create`, N, the failing line (part B), `subtree_control` after the run, any unsupported message (none expected).
3. `-T vdev_disk`: the two `Test:` lines and the Results Summary. `-T slog`: the Results Summary and every non-PASS with its lines; the zfs-helpers links (installed and removed, or H=no).
4. Kernel log per part: "no VERIFY/ASSERT/PANIC/BUG/Oops/WARNING/hung task" or what was seen; the three unload rcs.
5. End state: `subtree_control` against C0, the cgroup directory, loop devices, the cache file at steps 0 and 6, hung_task_panic; no reboot.

One-line verdict: "ZWC master+test PASS 3/3 (N 0 0 0), c1+test FAIL 3/3 (N a b c), head PASS 3/3 (N 0 0 0); -T vdev_disk PASS 2/2, -T slog PASS 29/29; every run on a loop device, none SKIP" or what differs.
