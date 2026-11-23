TASK-1123-RSR (from the coordinator session "main"): functional, about 2 h 30 with its builds, at most 3 h 30, no reboot expected. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It checks the new head of upstream PR #19988 by johnpwhite ("dsl_scan: cap the resilver range at the setup txg"): its test against master code, and the two tests of upstream PR #19845 by wrouesnel ("dsl_scan: wait for the DTL end before restarting a resilver") against #19988 alone and with #19845's fix. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rsr/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `G`).

## Why

- **The bug (#19988):** a write to a mirror child that fails in txg T adds T to the child's DTL during the sync of T, before `dsl_scan_sync()` runs. A resilver set up later in that sync took `scn_max_txg` = T + 1 from the DTLs, queued reads of blocks born in T, and a later sync pass of T rewrote some of them in place. The queued reads then failed their checksum on every child: checksum errors on the healthy child.
- **The fix:** `dsl_scan_setup_sync()` caps `scn_max_txg` at the setup txg (the bound is exclusive, so T stays in the DTL). With `resilver_defer` enabled, `dsl_scan_done()` starts another resilver for what is left; without the feature, a resilver restart is requested. A scrub is never turned into a resilver.
- **Its test** `zpool_resilver_failing_writes` (group `zpool_resilver`): mirror of two files, writes injected to fail on the second child, a resilver started after the first write error, then the healthy child must count no checksum error and a scrub must find nothing. The author reports, on Linux 6.18 debug: 3/3 PASS with the fix; 3/3 FAIL without it (5004, 4896 and 5094 checksum errors on the healthy child).
- **#19845** fixes the same in-place rewrite on another path: a resilver restarted early after an attach (`zfs_resilver_disable_defer=1`, or less than 10 % issued) is set up before the end of the new device's DTL. Its `resilver_restart_003` fails on master (permanent errors, checksum errors on every device); `resilver_restart_004` guards against skipping blocks that `dmu_sync()` wrote before the attach.
- **Userspace (libzpool, debug, main's replay):** with #19988 alone, the early restart after the second attach is capped at its setup txg, so the device attached second cannot be excised when that pass completes (`vdev_dtl_should_excise()`), and a second pass over the whole range follows (`scan setup func=2 mintxg=3` again after `starting deferred resilver`). Master and #19988 + #19845 run one pass. No checksum error in any variant there.
- **What this task shows:** the fail-before / pass-after of the new test on our kernel; that #19988 passes the resilver groups; that `resilver_restart_003` and `_004` pass with #19988 alone and with both PRs; and, from the kernel debug log, how many resilver passes each variant runs in those two tests.

## What is tested (fetched from https://github.com/openzfs/zfs; nothing comes from our fork)

| tree | how it is made | tree hash | what it is |
|---|---|---|---|
| zp | `pull/19988/head` = fc692f6c83ac9610d94e08a79d6deed720f0c6fd, parent master c913da86156eba3243cc1ef515e98d69887654a7 | de77491089747070e769b48b222d9fb1ffa160f0 | the fix with its test |
| zx | zp + cherry-pick of #19845's test commit cca45f3eb46fdb27c5c31d35e408b235c1e308fc (made in step 4, inside zp) | 9beef7d40efc712b6945497bd8b9eb9905be548d | the fix, with #19845's two tests; code identical to zp |
| zf | zp with `module/zfs/dsl_scan.c` taken from c913da8 (staged, not committed) | 7c6e95789c9bfd00af14021e1feddea8b3d4fd9e | the test alone, on master code |
| zc | zp + cherry-picks of cca45f3eb46fdb27c5c31d35e408b235c1e308fc then 0e74be1d97657e4687cdc8ef6787a81bc78b9d72 (`pull/19845/head`) | 2c03478443ff314e5e5575f7c699720113628fc0 | both PRs |

Test files (mode `100755`):

| path under `tests/zfs-tests/tests/functional/` | sha256 |
|---|---|
| cli_root/zpool_resilver/zpool_resilver_failing_writes.ksh | 4e19527aca89a88384e320370b6a5ddbd516f27e0de3229d1af4023ee20a9cc7 |
| replacement/resilver_restart_003.ksh | b18c7e1e2ce3a76b292853bb4a74a08aabda0bdd7aa74b7da95971b6631386cd |
| replacement/resilver_restart_004.ksh | 450938243641dca4fa4c742664af08dd9f9eca55d31800726d3e606380f90a02 |

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`.
- **Before every ZTS run:**
  - `lsmod | grep -E '^zfs '` shows the module of that step's tree (`cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/<tree>/module/zfs.ko`);
  - `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool;
  - `ls -d /var/tmp/testdir 2>&1` says it does not exist.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs.** A build still going after 60 minutes, a single-test run after 25 minutes, or a group after 90 minutes is a hang.
  - Capture into `$L/hang.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|zfs|ksh'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool` and `ksh` process;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
- **Kernel log.** After each step, check the new kernel lines since that step's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock`. None are expected. Pool suspend/resume `WARNING` lines are not a finding: count them.
- **Debug log of the resilver passes** (steps 4 and 6 only). Before each run of `resilver_restart_003` or `_004`: `echo 0 | sudo -n tee /proc/spl/kstat/zfs/dbgmsg > /dev/null` (this clears it). After the run: `sudo -n grep -E 'create pool|scan (setup|done|cancelled|aborted)|starting deferred resilver|vdev attach' /proc/spl/kstat/zfs/dbgmsg > $L/dbg-<tag>-<k>.txt`, then quote `wc -l` of it and the lines themselves (they are short; about 10 to 40 per run; each round starts at a `create pool` line). The `vdev attach` line is logged a few txgs after the restart that the attach caused, so it can follow that restart's lines.
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 15 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git`;
- `ls -d /var/tmp/task-1123-rsr /var/log/task-1123-rsr /var/tmp/testdir 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty;
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it.

## Step 1: fetch, make the trees and check

```sh
D=/var/tmp/task-1123-rsr; L=/var/log/task-1123-rsr; U=https://github.com/openzfs/zfs
G="git -c user.name=task -c user.email=task@localhost"
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
for n in zp zf zc; do mkdir $D/$n && cd $D/$n && git init -q . && git fetch -q --depth 2 $U pull/19988/head && git checkout -q FETCH_HEAD || echo "FETCH FAILED $n"; done
cd $D/zf && git checkout -q c913da86156eba3243cc1ef515e98d69887654a7 -- module/zfs/dsl_scan.c
cd $D/zc && git fetch -q --depth 3 $U pull/19845/head && $G cherry-pick cca45f3eb46fdb27c5c31d35e408b235c1e308fc 0e74be1d97657e4687cdc8ef6787a81bc78b9d72 > /dev/null && echo "zx-tree $(git rev-parse HEAD~1^{tree})"
for n in zp zf zc; do cd $D/$n && echo "$n $(git log --format=%H -1 HEAD~$( [ $n = zc ] && echo 2 || echo 0 )) $(git write-tree) $(git status --porcelain | wc -l)"; done | tee $L/trees.txt
cd $D/zp && git log -1 --format='%an | %s' && git diff --stat HEAD^ HEAD | tail -1
cd $D/zc && git log -2 --format='%an | %s' && git diff --stat HEAD~2 HEAD | tail -1
for n in zp zf zc; do (cd $D/$n && p=tests/zfs-tests/tests/functional/cli_root/zpool_resilver/zpool_resilver_failing_writes.ksh && echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64) failing_writes"); done
(cd $D/zc && for f in resilver_restart_003 resilver_restart_004; do p=tests/zfs-tests/tests/functional/replacement/$f.ksh; echo "zc $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64) $f"; done)
```

Expected:

```
zx-tree 9beef7d40efc712b6945497bd8b9eb9905be548d
zp fc692f6c83ac9610d94e08a79d6deed720f0c6fd de77491089747070e769b48b222d9fb1ffa160f0 0
zf fc692f6c83ac9610d94e08a79d6deed720f0c6fd 7c6e95789c9bfd00af14021e1feddea8b3d4fd9e 1
zc fc692f6c83ac9610d94e08a79d6deed720f0c6fd 2c03478443ff314e5e5575f7c699720113628fc0 0
```

(zf's one status line is `M  module/zfs/dsl_scan.c`.) Then `John White | dsl_scan: cap the resilver range at the setup txg` and `4 files changed, 178 insertions(+), 1 deletion(-)`; then `Will Rouesnel | dsl_scan: wait for the DTL end before an early restart`, `Will Rouesnel | ZTS: a restarted resilver must not report errors` and `6 files changed, 287 insertions(+), 2 deletions(-)`; then three `100755` lines with the failing_writes sha256 of the table, and two `zc 100755` lines with the sha256 of 003 and 004.

If a fetch fails, wait 1 minute and try once more. If the cherry-pick stops on a conflict, or any other line differs: stop and report the outputs as they are. Edit no file.

## Step 2: the three debug builds, at once (about 15 to 30 minutes)

```sh
D=/var/tmp/task-1123-rsr; L=/var/log/task-1123-rsr
for n in zp zf zc; do setsid nohup sh -c "date -u +%T; cd $D/$n && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$n.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-z?.log` until there are three lines; each must read `build-rc=0`. Quote the three durations and every `warning:` line outside objtool notes.

## Step 3: the fix (zp), all expected PASS

**Load zp:**
- `sudo -n $D/zp/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zp/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- quote `cat /sys/module/zfs/parameters/zfs_dbgmsg_enable`, which should be `1` (the default); if it is `0`, set it with `echo 1 | sudo -n tee /sys/module/zfs/parameters/zfs_dbgmsg_enable` and say so;
- write the mark: `echo "task-1123-rsr: zp loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**Runs,** one at a time, after the pre-run checks:

```sh
cd /var/tmp/task-1123-rsr/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-rsr/zts-<tag>-<k>.log 2>&1 < /dev/null &
```

Quote for each run:
- every `Test:` line with its result and duration;
- the Results Summary;
- from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`): every `NOTE` line of `zpool_resilver_failing_writes`, `resilver_restart_003` and `resilver_restart_004`, and every line with `FAIL`, `failed` or `ERROR`.

On a FAIL, copy the merged log to `$L/merged-<tag>-<k>.txt` and quote the failing command, its output and the 20 lines before it.

1. `zpool_resilver_failing_writes`, 3 runs (tag `zp-fw`, `<sel>` = `-t zpool_resilver_failing_writes`). Expected: PASS each time, in about 1 to 3 minutes; its note `checksum errors on .../a: 0`.
2. Groups, one run each, in this order (tag = the group, `<sel>` = `-T <tag>`): `zpool_resilver`, `replacement`. Expected: all PASS. If a test fails or skips, say which; step 5 does not rerun it, main compares with master.

## Step 4: #19845's tests on the fix alone (zx, still zp's module)

The cherry-pick adds test files only, so zp's build and loaded module stay valid:

```sh
D=/var/tmp/task-1123-rsr; U=https://github.com/openzfs/zfs; G="git -c user.name=task -c user.email=task@localhost"
cd $D/zp && git fetch -q --depth 3 $U pull/19845/head && $G cherry-pick cca45f3eb46fdb27c5c31d35e408b235c1e308fc > /dev/null && echo "zx $(git rev-parse HEAD^{tree})" && git diff --name-only HEAD^ HEAD | grep -v '^tests/' ; git diff --stat HEAD^ HEAD | tail -1
```

Expected: `zx 9beef7d40efc712b6945497bd8b9eb9905be548d`, no file name outside `tests/`, and `5 files changed, 254 insertions(+), 1 deletion(-)`.

Runs, with the dbgmsg capture of the VM rules around each:
1. `resilver_restart_003`, 2 runs (tag `zx-003`, `<sel>` = `-t resilver_restart_003`). Expected: PASS each time. A `SKIP` with "the first resilver finished too soon" is not a failure: quote it.
2. `resilver_restart_004`, 2 runs (tag `zx-004`, `<sel>` = `-t resilver_restart_004`). Expected: PASS each time; its scrubs `repaired 0B`.

Expected in the dbgmsg lines, for each round whose note says `the running resilver was restarted` (a `scan cancelled` or `scan aborted` line in that round): the `scan setup func=2` that follows the restart has a `maxtxg` equal to its own txg, and after its `scan done` come `starting deferred resilver` and a further `scan setup func=2 mintxg=3 ...` with its own `scan done`: the device attached second is resilvered a second time over the whole range. Quote what you see; if it differs, say how. It does not change PASS or FAIL.

After these runs:
- no pool left;
- the kernel-line check since the zp mark;
- unload with zp's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 5: the test alone (zf), expected FAIL

Load zf as in step 3, with the mark `task-1123-rsr: zf loaded`. Then `zpool_resilver_failing_writes`, 2 runs (tag `zf-fw`).

Expected: each FAILs at `test N -eq 0` after the note `checksum errors on .../a: N`, with N in the thousands (the author saw 4896 to 5094). Quote the note and the failing line. The test's cleanup must leave no pool: quote `zpool list`.

A run that passes, or fails elsewhere (for example "No injected write error reached" or "No resilver started while writes failed"), is not the expected fail-before: quote it and say so.

Then the kernel-line check since the zf mark, and unload with zf's `scripts/zfs.sh -u`; `lsmod` must be empty.

## Step 6: both PRs (zc), all expected PASS

Load zc as in step 3, with the mark `task-1123-rsr: zc loaded`. Runs, with the dbgmsg capture around 003 and 004:
1. `zpool_resilver_failing_writes`, 2 runs (tag `zc-fw`). Expected: PASS, `checksum errors on .../a: 0`.
2. `resilver_restart_003`, 2 runs (tag `zc-003`). Expected: PASS.
3. `resilver_restart_004`, 2 runs (tag `zc-004`). Expected: PASS, `repaired 0B`.

Expected in the dbgmsg lines, for each round with a `scan cancelled` or `scan aborted` line: the restarted pass is the last `scan setup func=2` of the round (the early restart waits for the end of the new device's DTL, so nothing is left for a second pass). In a round where the first resilver ended first, a separate pass for the device attached second is normal. Quote what you see.

Then:
- no pool left; the kernel-line check since the zc mark;
- unload with zc's `scripts/zfs.sh -u`; `lsmod` must be empty;
- `sudo -n rm -rf /var/tmp/task-1123-rsr`, `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-rsr/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, hung_task_timeout_secs, `trees.txt` and the `zx-tree` lines, the test modes and sha256, the log and diffstat lines, the build rc and durations with their warnings, and the three srcversions with "(DEBUG mode)".
2. **Table:** tag | tree | run | selection | result per test (or "all N PASS") | duration | key note. Then one verdict line, for example: "fix: failing_writes PASS 3/3 (0 checksum errors), zpool_resilver and replacement PASS; 19845 tests on the fix alone: 003 PASS 2/2, 004 PASS 2/2, second full pass seen in N of M restarted rounds; test alone: FAIL 2/2 (N, M checksum errors); both PRs: failing_writes, 003, 004 PASS, no second pass".
3. **Quotes:** the NOTE lines of each run (run 1 in full, then what differs), the non-PASS lines, the dbgmsg lines of each 003/004 run, and the kernel-line checks.
4. **Deviations, cleanup,** then `End`.
