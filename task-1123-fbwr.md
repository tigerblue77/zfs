TASK-1123-FBWR (from the coordinator session "main"): functional, in two parts that may run at the same time: **part A on VM 105**, about 1 h 45 with its builds, at most 2 h 45; **part B on VM 107**, about 2 h, at most 3 h (Debian 13, kernel 6.12), neither next to a performance run. It is the fail-before of six of our regression tests on wrouesnel's zinject framework (the openzfs/zfs#19997 direction). TASK-1122-WRS ran each of them 3 times with its fix, on a debug build: all PASS. Here each runs once on a tree that has the test and its injection points but not the fix, and is expected to FAIL. Three of the six leave a pool that only a reboot clears: one in part A, two in part B. Those reboots are pre-approved, each for its own item only. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-fbwr/report.md` on your VM, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `F`).

## Why

Each tree is one of our `-wr` branches without its fix. The point names, the call sites and the test files are byte for byte those of the `-wr` heads that TASK-1122-WRS ran.

| tag | part | the bug, in one line | without the fix, the test |
|---|---|---|---|
| rw | A | A write that dirties a block between the "not dirty" check of `zfs rewrite -P` and its dirtying is taken for the rewrite and keeps the old birth time. | FAILs at its last `cmp`: the incremental stream left the append out. |
| sa | A | Replaying a `TX_SETATTR` with `ATTR_SIZE` marks the record replayed in two txgs. A stop between them loses the mode and the times. | FAILs: after the second replay the file still has mode 4777. |
| sp | A | `zpool split` uses the new pool after dropping `spa_namespace_lock`, without holding it. | FAILs: the export of the new pool succeeds. Then its cleanup releases the split, which oopses on the freed pool. |
| ri | B | `spa_sync()` advances the reflow offset of a RAID-Z expansion while a read still maps the old location. Upstream fixed this in c9e0d4c8934 (#19705), and this tree reverts that fix. | FAILs: the read finds other data at the old location (checksum errors). |
| rs | B | A scratch uberblock of a RAID-Z expansion that no disk took goes through `VERIFY0()`, which panics the sync thread on release builds. | No log line comes first: `txg_sync` panics. 60 s later the test FAILs waiting for the error message, and the pool stays wedged. |
| rl | B | The expansion thread takes `SCL_STATE` while holding a chunk's range lock. A `zpool clear` and a read of that chunk then deadlock with it. | FAILs after 60 s ("did not return"), and the pool stays deadlocked. |

## What is tested (fork https://github.com/tigerblue77/zfs)

Main pushes the six `-fb` branches before this task starts. `n` is the number of commits from the head down to `189a309d464ca51c74d627fdbdd84b5992eade2d` (wrouesnel's `zinject-points-uplift`, on upstream master `a1daebfec3f`).

| tag | part | branch | head | tree hash | HEAD^ | n | build |
|---|---|---|---|---|---|---|---|
| rw | A | zfs-rewrite-write-race-v2-wr-fb | 2134bc56fe6914e2ed2e75655eabf2a263a0eee8 | a1420fee43c0e6678a86eeb66c268fd618ed5ad2 | 189a309d464ca51c74d627fdbdd84b5992eade2d | 1 | debug |
| sa | A | setattr-replay-one-record-test-wr-fb | 0362274973e3dda739123c7fad7d57fd1bacf7d3 | f6a060429e4737293e05f803eebd755bbe72a100 | 189a309d464ca51c74d627fdbdd84b5992eade2d | 1 | release + `--enable-inject-points` |
| sp | A | zpool-split-hold-newspa-v2-wr-fb | b82a73dd230232ae1338bff6b70281791c94edbd | f6c5d9b00f4faef38904aa2573b9f0a7c2d01a85 | 189a309d464ca51c74d627fdbdd84b5992eade2d | 1 | debug |
| ri | B | retro-19705-test-v2-wr-fb | 7e919c93e3d86f80979f8113d3c7ae8bef06563d | f2c01f9df55b13703bd3b234232e1cb53fca442b | 0d7d8cdfe2e489d5d539543281b25c946e352161 | 2 | debug |
| rs | B | raidz-scratch-ub-no-panic-v4-wr-fb | b475860bad1d56973877f8fcad8632da8db46464 | 2f2906cdce66f420a82767675c27e86bb7c31193 | 38192464f55dd6ff91263f72d14e345889ae2824 | 2 | release + `--enable-inject-points` |
| rl | B | raidz-reflow-state-lock-order-v2-wr-fb | 7e0fda72040a71c4c877bf507f3a40237447976e | 88c1db116db2a0e0b20406a0209c8fbfc82b9269 | c293ac4b392239884db21d22e664e8f117055357 | 2 | debug |

- rw, sa, sp: one commit, "VM only: ...", on `189a309d464`. It carries the injection point and the test of the `-wr` branch's test commit, but not its fix commit.
- ri: `HEAD^` is the head of `retro-19705-test-v2-wr` itself: the test and its two points. `HEAD` reverts upstream's fix in `module/zfs/spa.c`, as `1c24319aa00` did for TASK-1122-R19705.
- rs, rl: `HEAD^` is the first commit of the `-wr` branch, which adds its points (`38192464f55`, `c293ac4b392`). `HEAD` adds the test and its registration, but not the fix's change to `module/zfs/vdev_raidz.c`.

Every test file has mode `100755` and the sha256 that TASK-1122-WRS checked. Paths are under `tests/zfs-tests/tests/functional/`. The name to run is the file name without `.ksh`.

| tag | test file | sha256 |
|---|---|---|
| rw | cli_root/zfs_rewrite/zfs_rewrite_physical_append.ksh | ee26d432cd8d25b8ec4c221976434e6e422fa19ad79eb06841d6d9ef0232d874 |
| sa | slog/slog_replay_fs_setattr_size.ksh | ecbf08e39cc8e651e5e2e61bb2c082ba1f5ad110fb542ba367017b50b1464a67 |
| sp | cli_root/zpool_split/zpool_split_new_pool_busy.ksh | 2e0c233b1e5f5e46bd76f6ebc529f7244ad0e3d27c804efc2981972e004fef7e |
| ri | raidz/raidz_expand_inflight_read.ksh | 4a689dfcf044cea41357759e4097448c75e0a2573575dcf0118d903d7964af0f |
| rs | raidz/raidz_expand_scratch_uberblock.ksh | 023039c249172640274c6df1a121da4a44476ebb4978443e2a880a8b1fc95dfd |
| rl | raidz/raidz_expand_state_lock.ksh | fee2ece40d51995e29d534c321838a4fc6012a65d6c3f683556bab52f9783474 |

sa also carries the pool image `slog/blockfiles/setattr_size.dat.bz2`, sha256 `f68ea0ae6753ad8c9a1354b2091221953ed5351b9c2f0f6dcaf5bb50ccbeb8fb` (SAI's).

The file the fix would change, as it is in each tree:

| tag | file | sha256 | what step 1 checks in it |
|---|---|---|---|
| rw | module/zfs/dbuf.c | 346533974650f00f670d97ac189ddda3a4c52d7b391600fbd8992782b483fd40 | `dbuf_dirty_impl` 0 times (the fix); the point once |
| sa | module/os/linux/zfs/zfs_vnops_os.c | 1f66ee6b85c8e1367574a54fc12f6009e25f33575256933c73912a056780c42c | master's `zfs_freesp(zp, vap->va_size, 0, 0, TRUE);` once; the point once |
| sp | module/zfs/spa.c | 325b7ba2b53267fdf99b6b937f888579bbc1bce22102675e1f340a4a28bce144 | `spa_open_ref(newspa, FTAG)` 0 times; the point once |
| ri | module/zfs/spa.c | 993579efc3dcfe69e319fae619cfa4d2e45cf6aec1a764feb4e9402707f05986 | the range lock of #19705's fix (`vre->vre_rangelock, synced_offset`) 0 times |
| rs | module/zfs/vdev_raidz.c | c4e59a62aa2854ac45b218f80c9a427ab5831843e5e9165ed124b6012cda6cc4 | `raidz_reflow_scratch_ub_sync` 0 times; master's three `VERIFY0(vdev_uberblock_sync_list`; the two points |
| rl | module/zfs/vdev_raidz.c | 96d4ae991aa3d9dbbc475a445a17d20d61689c6cf7ddc9d12cf206351357136e | the point (line 4672) before `spa_config_enter(spa, SCL_STATE, ...)` (line 4679): master's order |

## Which build, and what is expected

| tag | build | why that build | expected | crash risk |
|---|---|---|---|---|
| rw | debug | The failure is a `log_must` in the test. No assertion stands before it: TASK-1121-RWA's debug fail-before failed there, with a clean kernel log. | `ERROR: cmp /var/tmp/rewrite_append.expect /testpool/testfs2/file exited 1`, after cmp's `... differ: char 196609, line <n>` | none |
| sa | release + `--enable-inject-points` | Only an ASSERT stands before the measured failure. On debug, the second txg trips `ASSERT(zh->zh_replay_seq < *replayed_seq)` in `zil_sync()` and stops the sync (TASK-1121-SAT B2: `PANIC at zil.c:4274:zil_sync()`, a reboot). Release has no ASSERT: SAT B1 measured the failure 3/3. A plain release build would have no points (UNSUPPORTED). | `After the second replay: size 4096 and mode 4777, not 4096 and 777`, with `replay_seq` on disk equal to the record's seq | none |
| sp | debug | The failure is measured first (`log_mustnot_expect`). What follows is an oops, not an assertion, so release would oops the same. Debug matches TASK-1121-SPB. | `ERROR: zpool export testpool2 unexpectedly exited 0`; then `BUG: kernel NULL pointer dereference` (or a GPF) in `spa_history_log_internal`, Comm `zpool` | **crash expected** (oops; the pool stays busy) |
| ri | debug | A measured `log_fail`. TASK-1122-R19705's debug fail-before (earlier version of the test) found 42 checksum errors and no kernel line. | `the read found other data at the old location of the block (<N> checksum errors)` | none |
| rs | release + `--enable-inject-points` | No build measures this one. On debug, master's flags trip `ASSERT(zio->io_error == 0 \|\| (zio->io_flags & ZIO_FLAG_CANFAIL))` in `zio_done()` (z_null_int), and txg_sync waits forever. This is from reading; on N-01 we saw it with `cac220eaf9b`, which has the same flags. On release, the `VERIFY0()` panics txg_sync: N-01 measured that, and it is the failure release users meet (#19440's regression). | in the kernel log: `VERIFY0(vdev_uberblock_sync_list(spa, &spa->spa_root_vdev, 1, &spa->spa_ubsync, ZIO_FLAG_CONFIG_WRITER)) failed (5)` and `PANIC at vdev_raidz.c:4946:raidz_reflow_scratch_sync()`, in txg_sync. About 60 s later the test prints `no 'reflow: error [0-9]+ writing uberblock [(]SCRATCH_VALID[)]' in the debug log in 60 seconds` | **crash expected** (the thread halts: no oops, the VM stays up, the pool is wedged) |
| rl | debug | A measured `log_fail` with no assertion before it: TASK-1117-RLI measured this on debug, with no VERIFY, ASSERT or PANIC. | `the read or 'zpool clear' did not return in 60 seconds once the expansion thread was released`, then the note `the pool is deadlocked: leaving it` | **reboot expected** (a 3-way deadlock; hung-task lines; the runner returns on its own) |

A run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

## Not in this task: zvol-open-pin-state-test-wr-fb

Its tree is built (see the comment at the top: `189a309d464` + `684a8aefc5e`'s change + `3e42136b8d9`'s change, without the fix `5439f31735b` and its configure check). On kernel 6.12 it would PASS: `del_gendisk()` takes the `open_mutex` that the held open runs under, so it waits for the open whether the fix is there or not. The fix only acts on Linux 5.10 and older (TASK-1121-ZOT part B was skipped on 6.12 for the same reason). Its fail-before needs the 5.10 guest route of ZV510C, D and E. There, ZV510C's fail-before (N-01 version) printed `The minor of ... was removed while an open held at zvol_open:found had not returned`, then a GPF at 2.9 s. That route takes about 2 h on its own and cannot run next to ZV510D or ZV510E, so it is main's call.

## VM rules

These are the rules of TASK-1123-MSD, with these additions:

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`, and never import a pool by hand.
- **One module and one ZTS run at a time.** The three builds of step 2 run at once, before any item.
- **Before every ZTS run** (`t` is the item's tag):
  - `lsmod | grep -E '^zfs '` shows a module whose `/sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/$t/module/zfs.ko`;
  - `timeout 30 sudo -n $D/$t/zpool list -H -o name` shows no pool;
  - the first line of `timeout 30 sudo -n $D/$t/zinject` is `No handlers registered.`;
  - `ls -d /var/tmp/testdir /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows none of them;
  - `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints `CACHE-OK`.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs** (rw, sa, ri; for sp, rs and rl, their own steps say what to do). A build still going after 60 minutes, or a single-test run after 20 minutes, is a hang.
  - Capture into `$L/hang-<tag>.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zinject|ksh'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool`, `zfs`, `zinject` and `ksh` process;
    - `timeout 10 sudo -n $D/<tag>/zinject`;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
- **Held threads.** A thread held at a zinject point stays held until its handler is removed by its ID. Check for one with `sudo -n grep -l -E 'zio_inject_pause|zio_handle_point_injection|zio_handle_func_injection' /proc/[0-9]*/task/[0-9]*/stack 2>/dev/null`, which must print nothing. A held thread is always a stop, even in a crash item, and even if that item's reboot is pre-approved. Capture as for a hang, change nothing, do not reboot, and report.
- **Kernel log.** After each item, check the new kernel lines since that item's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock`. For rw, sa and ri none are expected: any line is a stop, after the item's unload if the unload works. Pool suspend and resume `WARNING` lines are not a finding: count them. The steps of sp, rs and rl say which lines they expect.
- **Reboots:** exactly one per crash item, pre-approved for that item only: sp in part A, rs and rl in part B. Any other reboot needs main.
  - A hang, a panic, an oops or a module that will not unload anywhere else is a stop: capture, report and wait for main.
  - The same goes for anything unexpected in a crash item: a run that PASSes, or a crash at another place.
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end. After a reboot none are loaded.

## Step 0: state

Check and quote:
- `date -u`, `hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 15 GiB free (three trees), else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git xxh128sum jq` (the raidz tests of part B use the last two);
- `ls -d /var/tmp/task-1123-fbwr /var/log/task-1123-fbwr /var/tmp/testdir 2>&1`: all absent;
- `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1`: none;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty. If it is empty, move it to `$L/zpool.cache.step0` in step 1 once `$L` exists, and say so. Above 0: stop and report;
- `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs`: quote them. If `hung_task_panic` is 1, set it to 0 for this task (`sudo -n sysctl kernel.hung_task_panic=0`) and say so. Change nothing else here;
- `losetup -a`.

## Step 1: fetch and check

Part A:

```sh
D=/var/tmp/task-1123-fbwr; L=/var/log/task-1123-fbwr; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
echo "$(date -u +%FT%TZ) part A start $(hostname) $(uname -r)" >> $L/progress.md
for t in "rw zfs-rewrite-write-race-v2-wr-fb 1" "sa setattr-replay-one-record-test-wr-fb 1" "sp zpool-split-hold-newspa-v2-wr-fb 1"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 4 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~$3) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
```

Expected:

```
rw 2134bc56fe6914e2ed2e75655eabf2a263a0eee8 a1420fee43c0e6678a86eeb66c268fd618ed5ad2 189a309d464ca51c74d627fdbdd84b5992eade2d 189a309d464ca51c74d627fdbdd84b5992eade2d 0
sa 0362274973e3dda739123c7fad7d57fd1bacf7d3 f6a060429e4737293e05f803eebd755bbe72a100 189a309d464ca51c74d627fdbdd84b5992eade2d 189a309d464ca51c74d627fdbdd84b5992eade2d 0
sp b82a73dd230232ae1338bff6b70281791c94edbd f6c5d9b00f4faef38904aa2573b9f0a7c2d01a85 189a309d464ca51c74d627fdbdd84b5992eade2d 189a309d464ca51c74d627fdbdd84b5992eade2d 0
```

Part B: the same with `echo "... part B start ..."` and this loop list:

```sh
for t in "ri retro-19705-test-v2-wr-fb 2" "rs raidz-scratch-ub-no-panic-v4-wr-fb 2" "rl raidz-reflow-state-lock-order-v2-wr-fb 2"; do set -- $t
```

Expected:

```
ri 7e919c93e3d86f80979f8113d3c7ae8bef06563d f2c01f9df55b13703bd3b234232e1cb53fca442b 0d7d8cdfe2e489d5d539543281b25c946e352161 189a309d464ca51c74d627fdbdd84b5992eade2d 0
rs b475860bad1d56973877f8fcad8632da8db46464 2f2906cdce66f420a82767675c27e86bb7c31193 38192464f55dd6ff91263f72d14e345889ae2824 189a309d464ca51c74d627fdbdd84b5992eade2d 0
rl 7e0fda72040a71c4c877bf507f3a40237447976e 88c1db116db2a0e0b20406a0209c8fbfc82b9269 c293ac4b392239884db21d22e664e8f117055357 189a309d464ca51c74d627fdbdd84b5992eade2d 0
```

Then, for your part's three trees: the test's mode and sha256, the fix file's sha256, and the diffstat of the whole tree on the framework. Keep only your part's three lines in the here-document:

```sh
D=/var/tmp/task-1123-fbwr; F=tests/zfs-tests/tests/functional
while read t n test src; do (cd $D/$t && p=$F/$test.ksh &&
  echo "$t $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64) $(sha256sum < $src | cut -c1-64)$(git diff --stat HEAD~$n HEAD | tail -1)"); done <<'EOF'
rw 1 cli_root/zfs_rewrite/zfs_rewrite_physical_append module/zfs/dbuf.c
sa 1 slog/slog_replay_fs_setattr_size module/os/linux/zfs/zfs_vnops_os.c
sp 1 cli_root/zpool_split/zpool_split_new_pool_busy module/zfs/spa.c
ri 2 raidz/raidz_expand_inflight_read module/zfs/spa.c
rs 2 raidz/raidz_expand_scratch_uberblock module/zfs/vdev_raidz.c
rl 2 raidz/raidz_expand_state_lock module/zfs/vdev_raidz.c
EOF
```

Expected (one line each):

```
rw 100755 ee26d432cd8d25b8ec4c221976434e6e422fa19ad79eb06841d6d9ef0232d874 346533974650f00f670d97ac189ddda3a4c52d7b391600fbd8992782b483fd40 4 files changed, 122 insertions(+), 3 deletions(-)
sa 100755 ecbf08e39cc8e651e5e2e61bb2c082ba1f5ad110fb542ba367017b50b1464a67 1f66ee6b85c8e1367574a54fc12f6009e25f33575256933c73912a056780c42c 5 files changed, 150 insertions(+)
sp 100755 2e0c233b1e5f5e46bd76f6ebc529f7244ad0e3d27c804efc2981972e004fef7e 325b7ba2b53267fdf99b6b937f888579bbc1bce22102675e1f340a4a28bce144 4 files changed, 90 insertions(+), 1 deletion(-)
ri 100755 4a689dfcf044cea41357759e4097448c75e0a2573575dcf0118d903d7964af0f 993579efc3dcfe69e319fae619cfa4d2e45cf6aec1a764feb4e9402707f05986 5 files changed, 260 insertions(+), 19 deletions(-)
rs 100755 023039c249172640274c6df1a121da4a44476ebb4978443e2a880a8b1fc95dfd c4e59a62aa2854ac45b218f80c9a427ab5831843e5e9165ed124b6012cda6cc4 4 files changed, 186 insertions(+), 1 deletion(-)
rl 100755 fee2ece40d51995e29d534c321838a4fc6012a65d6c3f683556bab52f9783474 96d4ae991aa3d9dbbc475a445a17d20d61689c6cf7ddc9d12cf206351357136e 4 files changed, 133 insertions(+), 1 deletion(-)
```

Then the readable checks of the fix files, your part's lines only:

```sh
D=/var/tmp/task-1123-fbwr
# part A
cd $D/rw && grep -c dbuf_dirty_impl module/zfs/dbuf.c; grep -c '"dmu_buf_will_rewrite:clean"' module/zfs/dbuf.c                          # 0, 1
cd $D/sa && grep -c -F 'zfs_freesp(zp, vap->va_size, 0, 0, TRUE);' module/os/linux/zfs/zfs_vnops_os.c; grep -c '"zfs_setattr:size_changed"' module/os/linux/zfs/zfs_vnops_os.c   # 1, 1
cd $D/sa && sha256sum < tests/zfs-tests/tests/functional/slog/blockfiles/setattr_size.dat.bz2 | cut -c1-64                                # f68ea0ae6753ad8c9a1354b2091221953ed5351b9c2f0f6dcaf5bb50ccbeb8fb
cd $D/sp && grep -c -F 'spa_open_ref(newspa, FTAG)' module/zfs/spa.c; grep -c '"spa_vdev_split_mirror:committed"' module/zfs/spa.c       # 0, 1
# part B
cd $D/ri && grep -c -F 'vre->vre_rangelock, synced_offset' module/zfs/spa.c; grep -c -e '"raidz_reflow_sync:committed"' -e '"vdev_raidz_io_start:copied"' module/zfs/vdev_raidz.c   # 0, 2
cd $D/rs && grep -c raidz_reflow_scratch_ub_sync module/zfs/vdev_raidz.c; grep -c 'VERIFY0(vdev_uberblock_sync_list' module/zfs/vdev_raidz.c; grep -c '"raidz_reflow_scratch_sync:' module/zfs/vdev_raidz.c   # 0, 3, 2
cd $D/rl && grep -n -e '"raidz_reflow_impl:locked"' -e 'spa_config_enter(spa, SCL_STATE, spa, RW_READER)' module/zfs/vdev_raidz.c        # 4672: ... "raidz_reflow_impl:locked", &ignored);  then  4679: spa_config_enter(spa, SCL_STATE, spa, RW_READER);
```

If a fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: your part's three builds, at once (about 25 to 35 minutes)

Part A:

```sh
D=/var/tmp/task-1123-fbwr; L=/var/log/task-1123-fbwr
for t in "rw --enable-debug" "sa --enable-inject-points" "sp --enable-debug"; do set -- $t
  setsid nohup sh -c "date -u +%T; cd $D/$1 && ./autogen.sh && ./configure $2 && nice make -j3; echo build-rc=\$?; date -u +%T" > $L/build-$1.log 2>&1 < /dev/null &
done
```

Part B: the same with `for t in "ri --enable-debug" "rs --enable-inject-points" "rl --enable-debug"; do set -- $t`.

`sa` and `rs` are release builds: no `--enable-debug`, so no ASSERT, but with injection points.

Poll `grep -H build-rc= $L/build-*.log` until there are three lines; each must read `build-rc=0`. Then quote:
- the three durations, and every `warning:` line outside objtool notes (none expected);
- `grep -H -E 'assertion support will be enabled|injection points will be built' $L/build-*.log`. Expected: `yes` and `yes` for the debug trees (rw, sp, ri, rl); `no` and `yes` for the release trees (sa, rs). A `no` for injection points is a stop: the test would report UNSUPPORTED;
- the point names in each module: `strings $D/<tag>/module/zfs.ko | grep -c -x -E '<the point names>'`, with `dmu_buf_will_rewrite:clean` for rw, `zfs_setattr:size_changed` for sa, `spa_vdev_split_mirror:committed` for sp, `raidz_reflow_sync:committed|vdev_raidz_io_start:copied` for ri (2), `raidz_reflow_scratch_sync:written|raidz_reflow_scratch_sync:reflowed` for rs (2), and `raidz_reflow_impl:locked` for rl. Expected: 1, or 2 where shown;
- the three srcversions, `modinfo -F srcversion $D/<tag>/module/zfs.ko`, which must differ.

## Step 3 and after: the items

Each part runs its items in this order: a measured item, a measured item, then the crash item (sp) in part A; in part B, a measured item, then the crash items rs and rl, with a reboot between them.

| part | 1st | 2nd | 3rd |
|---|---|---|---|
| A | rw (debug, measured) | sa (release, measured) | sp (debug, crash expected) |
| B | ri (debug, measured) | rs (release, crash expected) | rl (debug, reboot expected) |

### Load (every item)

- `lsmod | grep -E '^(zfs|spl) '` must be empty (unload the previous item first).
- `sudo -n $D/<tag>/scripts/zfs.sh`.
- Quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/<tag>/module/zfs.ko`.
- Quote the last `ZFS: Loaded module` line. It must say "(DEBUG mode)" for rw, sp, ri and rl, and must not for sa and rs.
- Write the mark: `echo "task-1123-fbwr: <tag> loaded" | sudo -n tee /dev/kmsg > /dev/null`, and a line in `$L/progress.md`.

### A measured item (rw, sa, ri)

After the pre-run checks, one run:

```sh
cd /var/tmp/task-1123-fbwr/<tag>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t <test> > /var/log/task-1123-fbwr/zts-<tag>.log 2>&1 < /dev/null &
```

Quote:
- every `Test:` line with its result and duration (the group's setup, the test, the cleanup);
- the Results Summary;
- from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`): every `NOTE` line of the test, and every line with `FAIL`, `failed` or `ERROR`;
- the failing line with the 20 lines before it. Copy the merged log to `$L/merged-<tag>.txt`.

Expected per item:
- **rw** (`-t zfs_rewrite_physical_append`, under a minute): the source `cmp $expect $file` succeeds. The last `cmp` fails, the replica's file differing from the first byte of the append on: `/var/tmp/rewrite_append.expect /testpool/testfs2/file differ: char 196609, line <n>`, then `ERROR: cmp /var/tmp/rewrite_append.expect /testpool/testfs2/file exited 1`. Also quote `cat /sys/module/zfs/parameters/zfs_txg_timeout` after the run (5: the cleanup restores it).
- **sa** (`-t slog_replay_fs_setattr_size`, under a minute):
  - the record's lines (`TX_SETATTR ... seq S`, then `foid ..., mask 0x2069`);
  - `After the first replay: size 4096, mode 777, mtime T`;
  - `On disk: ZIL header: ... replay_seq S ...`, where S is the record's own seq (with the fix it is 0);
  - `After the second replay: size 4096, mode 4777, mtime <a time during the run>`;
  - the failure `After the second replay: size 4096 and mode 4777, not 4096 and 777`. The cleanup destroys `testpool_setattr` normally.
- **ri** (`-t raidz_expand_inflight_read`, under a minute):
  - its notes: `reflow offset ..., next ..., block ... at ... size ...`, then `reflow offset at least X (past the block: T)`, where X is expected at or above T, since `spa_sync()` no longer waits for the read;
  - the `zpool status -v` it prints;
  - the failure `the read found other data at the old location of the block (<N> checksum errors)`. TASK-1122-R19705 saw 42 with the earlier version of the test.

After the run:
- no pool: `timeout 30 sudo -n $D/<tag>/zpool list -H -o name` prints nothing;
- no handler: the first line of `timeout 30 sudo -n $D/<tag>/zinject` is `No handlers registered.`;
- no held thread (the check of the VM rules);
- the kernel-line check since the mark: `sudo -n dmesg | sed -n "/task-1123-fbwr: <tag> loaded/,\$p" | grep -E '<the pattern of the VM rules>'`. None expected;
- unload with `sudo -n $D/<tag>/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty. If the unload fails, capture as for a hang, stop and report, and do not reboot.

A pool, a handler or a held thread left: capture as for a hang, change nothing, and stop. A FAIL elsewhere or a PASS is not the expected fail-before: quote it, say so, and go on with the next item only if nothing is left.

### A crash item (sp, rs, rl): before the run

- Every other module is unloaded, and the item's tree is loaded as above.
- If `kernel.panic_on_oops` is 1, set it to 0 for this item (`sudo -n sysctl kernel.panic_on_oops=0`; the reboot restores it), so that an oops is printed and the VM stays up. `kernel.hung_task_panic` must be 0 (step 0, and again after a reboot).
- `LC_ALL=C ls -d /var/tmp/* > $L/var-tmp-<tag>.txt`.
- Start a capture that is flushed line by line and survives your commands: `L=/var/log/task-1123-fbwr; setsid nohup sudo -n sh -c "dmesg --follow > $L/<tag>-follow.txt" > /dev/null 2>&1 < /dev/null &`.
- `touch $L/<tag>-start`, and write in `$L/progress.md`: "<tag> run started, reboot follows".

Then the pre-run checks, and one run, with `-D` so that the test's output reaches `$L/zts-<tag>.log` as it runs, even if the runner never returns:

```sh
cd /var/tmp/task-1123-fbwr/<tag>
setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKOD -t <test> > /var/log/task-1123-fbwr/zts-<tag>.log 2>&1 < /dev/null &
```

### sp (part A, `-t zpool_split_new_pool_busy`): crash expected

Poll dmesg every 10 s for at most 5 minutes. Expected within a minute:
- in `$L/zts-sp.log`: `ERROR: zpool export testpool2 unexpectedly exited 0`;
- after the cleanup's `zinject -c all`, the split resumes and reads the freed `spa_t`: `BUG: kernel NULL pointer dereference, address: 0000000000000018`, `RIP: ... spa_history_log_internal+0x...`, `spa_vdev_split_mirror` and `zfs_ioc_vdev_split` in the trace, Comm `zpool`. A `general protection fault`, or another crash in the same function, confirms it just as well. No oops at all is reported as such: the export result is still the test's verdict.

Quote:
- the `ERROR:` line with the 20 lines before it;
- the `BUG:` (or GPF) line, the `RIP:` line, the line with `Comm:` and the first 15 frames of the Call Trace;
- the cleanup's lines (`zpool destroy -f testpool`: "pool is busy" expected; the dead ioctl keeps its reference on `testpool`);
- whether the runner returned (expected, with FAIL), and its result line.

Wait 2 more minutes. Then capture into `$L/sp-state.txt`:
- `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|zfs-tests|ksh'`;
- the kernel stacks of every D task;
- `timeout 30 sudo -n $D/sp/zpool list -H -o name,health 2>&1` (testpool expected, testpool2 not);
- `timeout 30 sudo -n $D/sp/zinject` (no handler expected);
- the held-thread check;
- `sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg` and `sudo -n dmesg | tail -n 200`.

Save `sudo -n dmesg > $L/dmesg-sp.txt`. Then reboot, as described under "The reboot of a crash item".

### rs (part B, `-t raidz_expand_scratch_uberblock`): crash expected

Poll dmesg every 10 s for at most 5 minutes. Expected within about a minute of the start, in txg_sync:
- `VERIFY0(vdev_uberblock_sync_list(spa, &spa->spa_root_vdev, 1, &spa->spa_ubsync, ZIO_FLAG_CONFIG_WRITER)) failed (5)`;
- `PANIC at vdev_raidz.c:4946:raidz_reflow_scratch_sync()`;
- the stack that SPL dumps (expected among its frames: `spl_panic`, `raidz_reflow_scratch_sync`, `spa_sync`, `txg_sync_thread`).

This is an SPL panic: the thread halts, and there is no oops, so the VM stays up. Quote the two lines and the first 15 frames.

Then, about 60 s later, in `$L/zts-rs.log`: `no 'reflow: error [0-9]+ writing uberblock [(]SCRATCH_VALID[)]' in the debug log in 60 seconds`. Quote it with the 20 lines before it: they must show the hold at `raidz_reflow_scratch_sync:written`, the `zinject -d ... -L uber` lines and the `zinject -c <id>`.

The test's cleanup then blocks in `zpool clear`, which waits for the halted txg_sync. Do not wait for the runner: its 600 s timeout, then `timeout 900`, end it. If it has returned when you capture, quote its result line.

Wait 2 more minutes. Then capture into `$L/rs-state.txt`:
- the `ps` line and the kernel stacks as for sp (txg_sync and the cleanup's `zpool clear` are expected in D);
- `timeout 30 sudo -n $D/rs/zinject`;
- the held-thread check;
- `sudo -n grep -E 'reflow' /proc/spl/kstat/zfs/dbgmsg | tail -n 30` (expected: no `uberblock updated (txg ..., SCRATCH_VALID` line and no `reflow: error` line);
- `sudo -n dmesg | tail -n 200`.

Save `sudo -n dmesg > $L/dmesg-rs.txt`. Then reboot, as described under "The reboot of a crash item". Item rl follows the reboot.

A test line `the sync thread did not reach raidz_reflow_scratch_sync:written in 120 seconds`, or any failure before the `zinject -c`, means the scenario did not happen: capture, stop and report. Do not reboot unless the module cannot be unloaded, and then only after the capture.

### rl (part B, after rs's reboot, `-t raidz_expand_state_lock`): reboot expected

The test holds the expansion thread with a chunk locked, starts a read (`dd`) and a `zpool clear`, then releases the thread. Expected:
- at about 75 s, in `$L/zts-rl.log`: `the read or 'zpool clear' did not return in 60 seconds once the expansion thread was released`;
- then the cleanup's note `the pool is deadlocked: leaving it`;
- the runner returns with FAIL. Quote these with the 20 lines before the failure, and the runner's result line.

Expected in the kernel log: `blocked for more than <hung_task_timeout_secs> seconds` lines for `zpool`, `dd` and `raidz_expand`, about 2 minutes after the release. They are not a stop: quote them. No VERIFY, ASSERT, PANIC or Oops is expected.

Wait until 3 minutes after the failure line. Then capture into `$L/rl-state.txt`:
- `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|dd|raidz|ksh'`;
- the kernel stacks of every D task. Expected, as in TASK-1117-RLI:
  - `raidz_expand` in `spa_config_enter_impl` < `spa_raidz_expand_thread`;
  - `zpool clear` in `spa_config_enter_impl` < `spa_vdev_state_enter` < `zfs_ioc_clear`;
  - `dd` in `zfs_rangelock_enter_impl` < `vdev_raidz_io_start`;
- `timeout 30 sudo -n $D/rl/zinject` (no handler expected: the test removed it by its ID before the deadlock);
- the held-thread check;
- `sudo -n dmesg | tail -n 200`.

Save `sudo -n dmesg > $L/dmesg-rl.txt`. Then reboot, as described under "The reboot of a crash item".

### The reboot of a crash item

Nothing can unload the module now. Do not run any `zpool` or `zfs` command any more: it could block.

1. `$L/report.md` holds everything up to this point, this item's quotes included.
2. Write `$L/RESUME.md`:

   ```
   TASK-1123-FBWR RESUME
   part <A|B>, reboot after item <tag> at <UTC time>
   next: "After a reboot", then <the next item, or "The end">
   D=/var/tmp/task-1123-fbwr L=/var/log/task-1123-fbwr
   resume message: <its id>, due <UTC time>
   report so far: /var/log/task-1123-fbwr/report.md
   ```

3. Arrange your own resumption as in TASK-1121-B24: a message scheduled to your own session about 5 minutes later, saying `TASK-1123-FBWR: resumed after the reboot of item <tag>: read /var/log/task-1123-fbwr/RESUME.md first`. Put its id in RESUME.md. If you cannot schedule it, say so in RESUME.md and reboot anyway: the session normally comes back with the VM.
4. `sync -f $L/report.md $L/RESUME.md $L/<tag>-follow.txt $L/dmesg-<tag>.txt` (a plain `sync` could hang on the stuck pool).
5. Arm the reboot detached, so that your call returns: `sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'`, and end your turn.

If the VM went down on its own before this (a kernel panic, no reply), nothing on it captured the end. Main then reads the VM's console on the Proxmox host, and after the restart you continue with "After a reboot".

### After a reboot

1. Read `$L/RESUME.md`. If the scheduled message has not fired yet (you came back with the VM), delete it and say so. If it woke you, say so.
2. `uptime -s` (after the reboot time), `lsmod | grep -E '^(zfs|spl) '` (empty), `ls -l /dev/zfs 2>&1` (absent), `losetup -a` (empty).
3. `sysctl kernel.panic_on_oops kernel.hung_task_panic`: quote them; they are back to their step 0 values. If `hung_task_panic` is 1, set it to 0 again, as in step 0.
4. The kernel log up to the reboot: `sudo -n journalctl -k -b -1 --no-pager > $L/journal-<tag>.txt 2>&1; grep -a -c -E 'VERIFY|ASSERT|PANIC|BUG:|blocked for more than' $L/journal-<tag>.txt`. A journal that keeps no earlier boot says so: record it. `$L/<tag>-follow.txt` holds the same lines.
5. What the interrupted run left in /var/tmp:

   ```sh
   L=/var/log/task-1123-fbwr; t=<tag>
   LC_ALL=C ls -d /var/tmp/* > $L/var-tmp-after-$t.txt; LC_ALL=C comm -13 $L/var-tmp-$t.txt $L/var-tmp-after-$t.txt
   ```

   Every name listed is either the interrupted run's or this boot's (`systemd-private-*`, left in place). The run's names are:
   - sp: `zpool_split_new_pool_busy` (the test's two file vdevs);
   - rs: `dev-1` to `dev-5`;
   - rl: `dev-1` to `dev-4`;
   - and, for each of them: `file-vdev0` to `2`, a `zfs-tests.*.run` file, a `zts-results.*` directory, a `constrained_path.*` directory, possibly `testdir`, and a new `test_results` if there was none before (it stays).

   Quote `ls -ld` of each of the run's names and remove them with `sudo -n rm -rf`. Anything else is reported and left.
6. `ls -l /etc/zfs/zpool.cache 2>&1`. After sp it may be above 0, because `zpool create` there uses the default cache file. Quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`. If it names only `testpool` with paths under `/var/tmp`, move it to `$L/zpool.cache.<tag>` and say so. Otherwise report it and leave it. After rs and rl it is expected absent or empty (their pools use `cachefile=none`); an empty one is moved the same way.
7. `sudo -n fstrim -av`, then the next item (part B: rl), or "The end".

## The end

- No module is loaded.
- `ls -d /var/tmp/testdir /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows none of them.
- `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`.
- `losetup -a` shows no loop device of this task.
- Delete the trees: `sudo -n rm -rf /var/tmp/task-1123-fbwr`. If a stop needs them, keep them and say so.
- `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-fbwr/report.md` and give the same text as your final message:

1. **Environment:** the part and VM, kernel, nproc, RAM, free space, the sysctls of step 0 (and any you changed), `trees.txt`, the step 1 lines (modes, sha256, diffstats, counts), each build's rc and duration with its two configure lines and its warnings, the point-name counts, and each srcversion with its load line ("(DEBUG mode)" or not).
2. **Table:** tag | tree | build | selection | result | duration | failing line (or what happened instead). Then one verdict line, for example: "part A: rw FAIL at the replica cmp (char 196609); sa (release) FAIL, mode 4777 after the second replay; sp FAIL at 'zpool export testpool2 unexpectedly exited 0', then the oops in spa_history_log_internal; 1 reboot" or "part B: ri FAIL, 42 checksum errors; rs (release) VERIFY0 PANIC at vdev_raidz.c:4946 in txg_sync, test FAIL at the dbgmsg wait; rl FAIL after 60 s, 3-way deadlock; 2 reboots".
3. **Quotes:** for each item, the failing line with the 20 lines before it; the NOTE lines named above; for the crash items, the kernel lines and frames named above, the D tasks and the state captures; and the kernel-line checks of the measured items.
4. **Reboots:** for each one, the time, how you resumed (scheduled message or the VM), the journal count, what was removed from /var/tmp, and where the cache file went.
5. **Deviations, cleanup,** then `End`.
