TASK-1121-TSN (from the coordinator session "main"): functional, about 30 min, one debug build, no reboot. It checks the fix "ZTS: make zfs-tests.sh -t NAME run the test of that name" by running `zfs-tests.sh -t NAME` for names that collide with other files, first with master's script, then with the fix's, on the same build. Any of the three test VMs (Debian 13, kernel 6.12). Other VMs may run functional tasks at the same time: this task measures nothing.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-tsn`, `L=/var/log/task-1121-tsn`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

`scripts/zfs-tests.sh -t NAME` resolves a name with no slash by `find "$STF_SUITE" -name "NAME*" -print -quit`: the first path whose name starts with NAME, in directory order. Many tests share that prefix with something that is not the test (a directory of the same name, a channel program's `.zcp`/`.out`, a blockfile), and then test-runner rejects the generated runfile and nothing runs. The fix looks for `NAME.ksh` first (regular file), then for `NAME*.ksh`. Which path `find` returns first depends on the file system, so this task records what master does on this VM's /var/tmp and what the fix does.

Tested: fork https://github.com/tigerblue77/zfs, branch `zts-single-test-exact-name`, head `30caf8f1383269cacb32cd12e0582c8e26eac9f1` (one commit, `scripts/zfs-tests.sh` only), parent `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (upstream master).

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; touch no block device of the VM; never `zpool import` anything yourself; never `zfs-tests.sh -x`.
- After every run `zpool list` must show no pool; a pool this task did not create is a stop: report it and leave it alone.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A single test still going after 15 minutes, or a build after 60 minutes, is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'` and `sudo -n dmesg | tail -n 100` into $L/hang-<tag>.txt, stop and report. No reboot is pre-approved.
- `sudo -n fstrim -av` after deleting the tree. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `nproc`, `free -m`, `df -h /var/tmp` (at least 10 GiB free), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/testdir* 2>&1` (none), `id zts`, `command -v ksh`, `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to $L/zpool.cache.step0 and say so; above 0: stop and report).

## STEP 1: tree

```sh
D=/var/tmp/task-1121-tsn; L=/var/log/task-1121-tsn
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L
git clone -q --no-checkout https://github.com/tigerblue77/zfs $D/z && cd $D/z && git fetch -q origin zts-single-test-exact-name && git checkout -q FETCH_HEAD
git rev-parse HEAD 'HEAD^{tree}' HEAD^ && git diff --stat HEAD^ HEAD
git show HEAD^:scripts/zfs-tests.sh > $L/zfs-tests.master.sh && cp scripts/zfs-tests.sh $L/zfs-tests.fix.sh && sha256sum $L/zfs-tests.*.sh
```

Expected: HEAD `30caf8f1383269cacb32cd12e0582c8e26eac9f1`, parent `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, diffstat `scripts/zfs-tests.sh | 8 +++++++-`, `1 file changed, 7 insertions(+), 1 deletion(-)`. Anything else: stop and report.

## STEP 2: build and load

`cd $D/z && sh autogen.sh && ./configure --enable-debug && make -j$(nproc)` detached, log $L/build.txt, then `build-rc=<rc>` and `grep -c 'warning:' $L/build.txt`. Load with `sudo -n ./scripts/zfs.sh` and record `cat /sys/module/zfs/srcversion` and the `ZFS: Loaded module` line.

## STEP 3: the lookups, without running tests

First, what each script would select, with no ZTS run (the lookup is plain shell). For each NAME in `mmap sync zfs_rewrite zfs_load-key zpool_influxdb zvol_stress tst.args_to_lua tst.timeout tst.clone raidz_dedup_overrun raidz_expand_001_pos zpool_import_001_pos.ksh no_such_test`:

```sh
D=/var/tmp/task-1121-tsn; L=/var/log/task-1121-tsn; S=$D/z/tests/zfs-tests
for n in mmap sync zfs_rewrite zfs_load-key zpool_influxdb zvol_stress tst.args_to_lua tst.timeout tst.clone raidz_dedup_overrun raidz_expand_001_pos zpool_import_001_pos.ksh no_such_test; do
  old=$(find "$S" -name "$n*" -print -quit)
  t="${n%.ksh}"; new=$(find "$S" -type f -name "$t.ksh" -print -quit); [ -z "$new" ] && new=$(find "$S" -type f -name "$t*.ksh" -print -quit)
  printf '%s\told=%s\tnew=%s\n' "$n" "${old#$S/}" "${new#$S/}"
done | tee $L/lookups.txt
```

Quote the table. Expected: `new=` is the `.ksh` of that exact name for every NAME that has one (`io/mmap.ksh`, `io/sync.ksh`, `cli_root/zfs_rewrite/zfs_rewrite.ksh`, ...), `raidz_expand_009_neg.ksh`-like for the partial `raidz_expand_00`-style names (none in this list), empty for `no_such_test`. `old=` will show some directories or non-`.ksh` files: which ones depends on this file system; quote them as they come.

## STEP 4: real runs, master's script then the fix's

Pick from STEP 3 every NAME whose `old=` is NOT a `.ksh` file (call them the colliding names), plus `zfs_rewrite` and `mmap` if they are not already in it. For each colliding NAME, run once with master's script and once with the fix's, in the built tree (the scripts differ only in the lookup):

```sh
D=/var/tmp/task-1121-tsn; L=/var/log/task-1121-tsn; n=<NAME>; v=<master|fix>
cd $D/z && cp $L/zfs-tests.$v.sh scripts/zfs-tests.sh && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t "$n" > $L/run-$v-$n.log 2>&1 < /dev/null &
```

Poll until the log has `Percent passed` or `test aborted` (15 min bound per run). Per run, quote: the `test-runner.py ... -c` line's runfile content if printed, any `is not an executable regular file` / `Verification failed` / `test aborted` line, every `Test:` line with its result, and the Results Summary. After each run: `zpool list` (no pool), no new `/var/tmp/file-vdev*`. At the end restore the fix's script: `cp $L/zfs-tests.fix.sh $D/z/scripts/zfs-tests.sh && git -C $D/z status --porcelain` (must print nothing).

Expected: with master's script, the colliding names abort ("is not an executable regular file", "Verification failed", "test aborted") or run another test than the one named; with the fix's script, the `Test:` line names the `.ksh` of that exact name (its own PASS/FAIL/SKIP is whatever that test gives on this VM: quote it, it is not this task's verdict).

## STEP 5: restore

Unload (`sudo -n ./scripts/zfs.sh -u`, rc), `lsmod` empty, remove $D, `sudo -n fstrim -av`, `/etc/zfs/zpool.cache` absent or handled as at step 0. Keep $L.

## Report

1. Environment, STEP 1 values, build rc and warnings, srcversion.
2. The STEP 3 table verbatim.
3. A table: NAME | master script outcome | fix script outcome (the `Test:` path selected and its result) | key lines.
4. One verdict line, e.g. "TSN: master's -t aborted or picked the wrong path for <k> of <n> colliding names; the fix ran the named test for all <n>".
