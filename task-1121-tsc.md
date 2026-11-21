TASK-1121-TSC (from the coordinator session "main"): functional, about 45 min, debug build only (one tree, rebuilt incrementally for master's dnode.c), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-tsc`, `L=/var/log/task-1121-tsc`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

openzfs/zfs#19513: since #17792, when `dnode_next_offset()` descends one level during a backward search it starts at entry 0 of the lower block, so it skips every lower block whose first entry is a hole. `get_next_chunk()` bounds each chunk of `dmu_free_long_range()` with such searches, so a long free of a sparse file can be done in a single chunk of any size. The branch starts a backward descent at the last entry, and adds the ZTS test `truncate_sparse_chunks`: a file with recordsize=512 (an L1 spans 512K, an L2 512M), one block in each of 768 L1s spread over two L2 ranges whose first L1 is a hole, one block in the next L2 range, then `truncate -s 0` with `zfs_per_txg_dirty_frees_percent=1`. Freed in chunks of at most 256 L1s, the file makes `dmu_free_long_range()` wait for the next txg at least once, which the kstat `dmu_tx_dirty_frees_delay` counts; freed in one chunk, it counts nothing. This task checks that the test fails with master's dnode.c and passes with the branch's.

What is tested: fork https://github.com/tigerblue77/zfs, branch `dnode-next-offset-backwards-v2`, head `591062b5fbca156ce0d2492f38d1cead49cad9fd` (the test), tree `7c086a6066b96d5bca34f354876ad6dc691e6908`; its parent `3136a34bd59ad50ec43a7f3c3fb4f915771cd0e3` is the fix (module/zfs/dnode.c only), on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (21/11). Master (step 4) is the same tree with module/zfs/dnode.c checked out from a1daebfec3f: tree `d3f996de4311ef5323f90ac357a3db5917d418b4`. The test is the same file in both.

Expected, from a libzpool replay of the same layout on a 3 GiB and a 4 GiB vdev (agents/fix-19513-v2/harness/chunks.c, fix-run.txt and master-run.txt): branch 4 chunks of at most 256 L1s and `dmu_tx_dirty_frees_delay` +3 per run (the test requires at least 1), master one chunk of 769 L1s and +0. The test needs more than 768 MiB available in the pool (it logs UNSUPPORTED below that): the L1s a chunk dirties count against the pool's space at 24 times their size until their txg syncs; on a 512 MiB vdev the replay's chunks were held for space (`dmu_tx_quota` +3) and counted no delay. ZTS's default 4G vdevs have the room. On a VM with 7.8 GB, `zfs_dirty_data_max` is about 10 % of RAM, so the test logs a threshold near 8 MB, `768 L1s, 3 chunks or more`, and the layout `4 128K 512`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import` anything (`-a` least of all); never `zfs-tests.sh -x`.
- After every run `zpool list` must show no pool. A pool this task did not create (`tank` or any other) is a stop: report it before the next run, and leave it as it is (no export, no destroy, no import).
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows this step's module only, `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test, `cat /sys/module/zfs/parameters/zfs_per_txg_dirty_frees_percent` prints `30`, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A non-empty cache file names a pool left behind: stop and report its `ls -l`. Another value of the tunable means a run did not restore it: stop and report.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A single test still going after 15 minutes, a group after 45 minutes, a build after 90 minutes is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|truncate|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`), `grep -E 'dirty_frees_delay|memory_reserve|assigned' /proc/spl/kstat/zfs/dmu_tx` and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report. (With master's dnode.c the truncate reserves about 96 MB of L1s in one transaction; a wait there would show as `dmu_tx_memory_reserve` growing.)
- Reboots: none pre-approved; for this task that replaces the standing approval. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 15 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `id zts`.

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1121-tsc
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module. Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1121-tsc; L=/var/log/task-1121-tsc
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs dnode-next-offset-backwards-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "591062b5fbca156ce0d2492f38d1cead49cad9fd 3136a34bd59ad50ec43a7f3c3fb4f915771cd0e3 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 7c086a6066b96d5bca34f354876ad6dc691e6908 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "c8ea77c578db9872486d1d8255ef87da4e8b3fbe54fbc07eb6ee3f9a0ffbec8d  module/zfs/dnode.c" "efb4cf1e8802386569c1e393c9b5b06bb5f043ef59d883b54b3656f29be40be5  tests/zfs-tests/tests/functional/truncate/truncate_sparse_chunks.ksh" "d70c408e1dc42ccaf013873fd3a85d031937551e6602213934e32f02e438540e  tests/zfs-tests/include/tunables.cfg" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~2 HEAD; grep -c '(1 << epbs) - 1 : 0;' module/zfs/dnode.c; ls -l tests/zfs-tests/tests/functional/truncate/truncate_sparse_chunks.ksh | awk '{print $1}'
```

Expected TREE-OK, SHA-OK, the diffstat

```
 module/zfs/dnode.c                                 |  10 +-
 tests/runfiles/common.run                          |   3 +-
 tests/zfs-tests/include/tunables.cfg               |   2 +
 tests/zfs-tests/tests/Makefile.am                  |   1 +
 .../functional/truncate/truncate_sparse_chunks.ksh | 130 +++++++++++++++++++++
 5 files changed, 143 insertions(+), 3 deletions(-)
```

then `1` and `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree.

## STEP 2: debug build of the branch (about 12 minutes)

```sh
D=/var/tmp/task-1121-tsc; L=/var/log/task-1121-tsc
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-zd.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zd/module/zfs.ko`.

## STEP 3: the branch (tags b, bt)

`sudo -n $D/zd/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1), the last `ZFS: Loaded module` line ("(DEBUG mode)") and `cat /sys/module/zfs/parameters/zfs_dirty_data_max`. Note N0=`sudo -n dmesg | wc -l`.

A run, one at a time, after the pre-run checks of the rules:

```sh
cd /var/tmp/task-1121-tsc/zd
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1121-tsc/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with result and duration (a `-t` run prints three: the truncate setup, the test, the cleanup) and the Results Summary. The run's result directory is `R=$(awk '/^Log directory/ { print $3 }' $L/zts-<tag>-<n>.log)`. Then:

```sh
L=/var/log/task-1121-tsc; R=$(awk '/^Log directory/ { print $3 }' $L/zts-<tag>-<n>.log)
grep -E 'threshold [0-9]+ bytes|lvl iblk dblk|dmu_tx_dirty_frees_delay|no dirty frees delay|unexpected object layout|too little space' $R/log
```

On a FAIL or KILLED, copy `$R/log` to $L/log-<tag>-<n>.txt and quote the failing lines with the 20 before them. After each run: the pre-run checks again.

1. `-t truncate_sparse_chunks`, 3 runs (tag b). Expected PASS each, with the NOTE lines `threshold <T> bytes: 768 L1s, 3 chunks or more` (T about 8 million on 7.8 GB), `lvl iblk dblk: 4 128K 512` and `dmu_tx_dirty_frees_delay: <x> before, <y> after` with y greater than x (expected y - x = 3, as in the replay). Quote them, and y - x per run.
2. `-T truncate` once (tag bt). Expected all PASS: truncate_001_pos, truncate_002_pos, truncate_sparse_chunks and truncate_timestamps, with the setup and the cleanup. A FAIL is quoted and reported as is, without rerun, saying whether tests/test-runner/bin/zts-report.py.in lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 4: master's dnode.c (tag m)

```sh
cd /var/tmp/task-1121-tsc/zd && git checkout a1daebfec3f5ca46ac261ef339efdf0dbeae997c -- module/zfs/dnode.c && test "$(git write-tree)" = d3f996de4311ef5323f90ac357a3db5917d418b4 && echo TREE-OK && sha256sum module/zfs/dnode.c && git diff --cached --name-status HEAD; grep -c '(1 << epbs) - 1 : 0;' module/zfs/dnode.c
```

Expected TREE-OK, `2663803d41e08cfd0baf259a2165af36f78c568b900926c97dfe76cdb4342e93  module/zfs/dnode.c`, `M` and `module/zfs/dnode.c` separated by a tab, and `0`. Any difference: stop and report.

Rebuild in place, detached and polled like a build:

```sh
D=/var/tmp/task-1121-tsc; L=/var/log/task-1121-tsc
setsid nohup sh -c "cd $D/zd && nice make -j4; echo build-rc=\$?" > $L/build-zm.log 2>&1 < /dev/null &
```

It must end `build-rc=0`; quote `grep -E 'dnode|zfs\.ko' $L/build-zm.log | head -n 20` and every `warning:` line outside objtool notes. Note S2=`modinfo -F srcversion $D/zd/module/zfs.ko`: it must differ from S1. Load: `sudo -n $D/zd/scripts/zfs.sh`, srcversion equal to S2, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

1. `-t truncate_sparse_chunks`, 3 runs (tag m), as step 3.1. Expected FAIL each, with the same threshold and layout NOTE lines, `dmu_tx_dirty_frees_delay: <x> before, <x> after` (no change) and `768 L1s freed with no dirty frees delay`, preceded by the test's dump of the dmu_tx kstats (quote its `dmu_tx_dirty_frees_delay` and `dmu_tx_quota` lines); the cleanup still PASS. Quote the grep output and the failing lines with the 20 before them. A PASS here is the result that matters most: quote its grep output in full and go on with the plan; it does not stop the task. A KILLED (a hang) follows the hang rule.

After them: the pre-run checks once more; the dmesg check since N1 as in step 3. Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 5: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, `zfs_dirty_data_max`; TREE-OK, SHA-OK, diffstat, the grep counts and the mode; build rc and warnings of both builds; S1, S2 and DEBUG mode; step 4's TREE-OK, sha and name-status; what happened to /etc/zfs/zpool.cache at steps 0 and 5.
2. Table: tree | tag | run | result | duration | threshold / L1s | layout | dirty_frees_delay before -> after | key note. Rows: branch b 1-3, branch bt (one row per test), master m 1-3. Then one verdict line, e.g. "dnode-next-offset-backwards-v2 591062b5fbc: truncate_sparse_chunks PASS 3/3 (delay +k each), -T truncate PASS 4; master's dnode.c (a1daebfec3f): FAIL 3/3 (delay +0, '768 L1s freed with no dirty frees delay'): fail-before / pass-after confirmed".
3. Quotes: the grep output of every run, every non-PASS run with its 20 lines, the dmesg checks.
4. Deviations, cleanup, then End.
