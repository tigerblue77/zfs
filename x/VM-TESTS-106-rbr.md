# VM 106 (CHSONVTST03): `rebuild_reopen` in the kernel, on the fix and on master with the test only

These instructions are for the session on VM 106 (Debian 13). They produce
kernel evidence for the fork branch `rebuild-cancel-state-lock` of
https://github.com/tigerblue77/zfs: two commits on upstream master
`e117a84dbbfe0befe15110b9d1d799f47b1a5546` "libzutil: keep the latest label
when two vdevs claim one id",

- `d0ac1636daac4dbbb34e0b2330b9ba2f2832986e` "Don't cancel a rebuild over a
  leaf that is being reopened", which also adds the test
  `functional/replacement/rebuild_reopen`;
- `0e7f96966ea40a42c5a9e8deca3b727e5d5929df` "Hold SCL_STATE in the rebuild
  initiate and reset sync tasks", the head.

The session builds two debug trees in its own directories, runs
`rebuild_reopen` 5 times and `scrub_cancel` once on each, in the kernel, and
replies with `REPORT.txt`. This document is read from a checkout of the
branch `vm-tasks-1026` of the same fork in `/var/tmp/vm-task-106x` (the doc
checkout, `DOC` below); the last command of STEP 8 removes it with the
task's own directories, unless a run has wedged the system (see "Watching
for a wedge").

**The bug.** Once per metaslab, the sequential resilver (rebuild) thread
asks `vdev_resilver_needed()` whether any leaf still needs it, and cancels
itself when none does (`vdev_rebuild_should_cancel()`,
`module/zfs/vdev_rebuild.c:165` at e117a84db). `vdev_resilver_needed()`
counts only writeable leaves, and the thread holds only `SCL_CONFIG` while it
asks. `vdev_reopen()` closes every leaf and only then opens them again, with
`SCL_STATE_ALL` held as writer, so a check that lands in between finds the
new leaf closed and cancels the rebuild. Nothing restarts it: the replacing
vdev stays, and `zpool wait` never returns. Every `zpool scrub` reopens all
vdevs before it checks whether it may start, so a scrub that is refused
because a resilver runs is enough to trigger it.

**The test.** `rebuild_reopen` makes a pool of four 1 GiB file vdevs with 64
metaslabs each (`zfs_vdev_min_ms_count` 64), writes 64 MiB, and six times
replaces a device with `zpool replace -s` (a sequential resilver, held at
first by `zfs_scan_suspend_progress`) while four background loops request
scrubs. It fails if the resilver is still running after 120 s, if
`zpool history -i` holds a `rebuild ... canceled` line (its message then is
`The sequential resilver was canceled`), or if the replacing vdev is still
there 60 s later. Replayed in userspace (libzpool, file vdevs), it failed in
each of 12 runs on master, at the first rebuild, and passed in each of 12
with the fix. It has never run in a kernel: this task is that run.
`scrub_cancel`, an existing test of the same directory, refuses a scrub right
after `zpool replace -s` and then runs `zpool wait`, which a rebuild canceled
there leaves waiting until test-runner kills the test at its 600 s timeout;
in userspace that happened in none of 600 plain replays.

**The fix.** The first commit holds `SCL_STATE` as reader around the check in
`vdev_rebuild_should_cancel()`; the second takes it, before
`vdev_rebuild_lock`, in `vdev_rebuild_initiate_sync()` and
`vdev_rebuild_reset_sync()`, whose `VERIFY` of `vdev_resilver_needed()` a
reopen can fail the same way (a panic of the sync thread). Together they
change `module/zfs/vdev_rebuild.c` only, 17 insertions and 3 deletions; the
first commit also adds the test and registers it in
`tests/runfiles/common.run` and `tests/zfs-tests/tests/Makefile.am`.

| Tree | Path | Code |
| --- | --- | --- |
| `B` | `/var/tmp/zts-rbr-1028/B` | the head `0e7f96966e`: the fix and the test |
| `M` | `/var/tmp/zts-rbr-1028/M` | master `e117a84db` with the test files of `B` applied (`git diff e117a84db 0e7f96966e -- tests/`): the test, without the `vdev_rebuild.c` change |

Both trees are built here with `./autogen.sh && ./configure --enable-debug &&
make -j8`. STEP 2 checks that M's module code is master's (its
`vdev_rebuild.c` is master's blob, and nothing outside `tests/` differs from
master), that M's test files are B's, and that B and M differ in
`module/zfs/vdev_rebuild.c` only.

What the run establishes, B first, then M (a wedge on M would end the task,
so every run on B comes before any on M):

1. **B** (`b01`-`b05`, `bsc`): `rebuild_reopen` 5 times, then `scrub_cancel`
   once. Expected: every run `PASS`, no rebuild canceled, no kernel finding.
2. **M** (`m01`-`m05`, `msc`): the same. Expected: `rebuild_reopen` fails at
   least once, with `The sequential resilver was canceled` at the first
   rebuild; `scrub_cancel` passes, fails, or is killed at its 600 s timeout
   (the same bug, another symptom). Either outcome is a result: every run is
   reported as it came, and none is retried or added.
3. **Every run**: its result, its durations, its failure point, whether a
   rebuild was canceled, its kernel messages, and what it left behind (see
   "How a run is read"). Then STEP 8 unloads the module, unless a run has
   wedged the system.

## How a run is read

`run.sh` (STEP 4) runs one `scripts/zfs-tests.sh -vK -D -d $W/files -t
tests/functional/replacement/<test>.ksh` on the loaded tree, as user `zts`,
under an outer `timeout` of 1200 s. `-t` runs the directory's `setup`, the
test and its `cleanup`, so a run has three results; `-K` writes one `ZTS run`
line per result to the kernel log; `-D` copies the test's output to the
run's console log as it comes (a run killed by the outer timeout has no
result files, and keeps its last lines there alone); `-d` keeps the
harness's files in `$W/files`. After the run it records:

- **The result and the durations**: the test's `[PASS]`, `[FAIL]` or
  `[KILLED]` and its run time, both from its `Test:` line, and the wall time
  of the whole `zfs-tests.sh` (setup, test, cleanup).
- **The failure point**: the test's end line, the last line before
  `NOTE: Performing test-fail callback` or `NOTE: Performing local cleanup`.
  For a pass it is the `log_pass` message, for a `log_fail` its message (here
  `The sequential resilver was canceled`), for a failed `log_must` its
  `ERROR: <command> exited <n>` line. Also: the `ERROR:` lines, the number of
  `zpool replace` the test completed (`SUCCESS: zpool replace` lines; 6 in a
  passing `rebuild_reopen`, 4 in a passing `scrub_cancel`), and for a run
  that did not pass, the test's last 15 lines before its end.
- **Whether a rebuild was canceled.** The test decides it with `zpool history
  -i $TESTPOOL1 | grep -q "rebuild.*canceled"`, but that history cannot be
  read after the run: the test's cleanup destroys the pool. The same internal
  events reach the module's debug log as `txg <n> rebuild vdev_id=<id>
  vdev_guid=<guid> started|complete|canceled|reset`
  (`spa_history_log_sync()`), and `run.sh` reads them in two places, keeping
  the lines stamped at or after the run's start: in the test's output, where
  ZTS's test-fail callback copies the last 200 lines of the debug log and
  then empties it, and in `/proc/spl/kstat/zfs/dbgmsg` after the run. After
  a pass every event is there (6 `started`, 6 `complete` for
  `rebuild_reopen`); after a failure only those in the 200 lines and after,
  so a `canceled` event may be missing although the test saw it. `rebuild
  canceled` is `yes` when the end line is the test's message or a `canceled`
  event was seen.
- **The kernel log**: `journalctl -k` since the run's start, matched with
  `VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than`
  (case-insensitive, the `ZTS run` lines left out): the count and the first
  20 lines. When test-runner kills a test at its timeout, it writes
  `##### ZTS timeout debug #####`, `top`'s output, a stack and an `strace` to
  `/dev/kmsg`, then runs `echo l > /proc/sysrq-trigger` (a backtrace of every
  active CPU), with or without `-O` at this master
  (`tests/test-runner/bin/test-runner.py.in:204`). Its `Call Trace` lines
  are test-runner's, not the kernel's: a run's **findings** are its
  `VERIFY|ASSERT|PANIC|BUG:|Oops|blocked for more than` lines, plus its
  `call trace` lines only when the run has no such dump.
- **What it left**: pools (by their `/proc/spl/kstat/zfs` directories, and
  by `zpool list` when no pool could be wedged), loop and device-mapper
  devices, files in `$W/files`, test mounts and `/testpool*` directories,
  test processes, D-state tasks, and the tunables.

## Watching for a wedge

A `VERIFY` or `ASSERT` in this module does not panic the kernel (STEPs 5 and
6 check that `spl_panic_halt` is 0): the thread that hit it sleeps for good,
in D state, with whatever it held. A pool it holds can no longer be exported
or destroyed and the module can no longer be unloaded: only a reboot frees
them, and only the owner reboots this VM. M is the tree where that can happen
(the `VERIFY`s of `vdev_rebuild_initiate_sync()` and
`vdev_rebuild_reset_sync()` that the second commit protects). So:

- Every run is bounded by `timeout -k 60 1200` (`OUTER`): a TERM at 1200 s,
  a KILL 60 s later. `timeout` returns once the process it started has died,
  even if a test command below it stays in D state.
- After an outer timeout (`zfs-tests.sh rc=124` or `137`), or after a run
  with a kernel finding, `run.sh` captures the evidence without any `zpool`
  or `zfs` command (they would wait on a hung pool): the imported pools and
  their last txgs from `/proc/spl/kstat/zfs`, every D-state task with its
  `/proc/<pid>/stack`, `/proc/spl/kstat/taskq/summary`, then
  `echo w > /proc/sysrq-trigger` (the kernel's dump of its blocked tasks)
  read back from the journal, the whole `dmesg` and the last 400 lines of
  the debug log (`<label>-capture*`).
- If a pool is then still imported, or a task is in D state, the system is
  **wedged**: `run.sh` writes `$L/WEDGED` and exits 4. From then on no
  `zpool`, `zfs`, `zdb`, `zinject` or `zfs.sh` command may run, and STEP 8's
  wedged branch records what is left, assembles `REPORT.txt` and removes
  nothing. Without a pool or a D-state task, an outer timeout or a finding
  still stops the task (exit 3), and STEP 8's normal branch follows.
- **If a block runs well past its stated time** (one `run.sh` takes 23
  minutes at the very most: 1200 s, the 60 s before the KILL, the capture),
  interrupt it if your tool allows, record it with
  `echo "a block ran past its time and was interrupted" >> /var/log/zts-rbr-1028/WEDGED`,
  and go to STEP 8's wedged branch. If even that cannot run, reply with what
  you have: everything of the task is on disk under `/var/log/zts-rbr-1028`.

**After a wedge the VM needs a reboot by its owner; never reboot from this
document.** Before the reply, `REPORT.txt` must hold all that the reboot
loses: the captures do, and STEP 8 adds the D-state tasks and the module's
state. If `/var/log/journal` exists (STEP 0 records it), the kernel log of
the wedged boot stays readable after the reboot with `journalctl -k -b -1`.
A clean shutdown may stall while unmounting `/testpool1`; a reset of the VM
from the hypervisor then completes it.

## Rules for the whole run

- Run everything as root, in bash, in this order. The ZTS runs go through
  `runuser -u zts` inside `run.sh`; nothing else runs as `zts`. Shell
  variables may not survive between your commands, so every block below
  starts by sourcing `/var/log/zts-rbr-1028/env.sh` or by setting what it
  needs.
- A production hypervisor sits underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads and every ZTS run go through `nice -n 10 ionice -c3` too (the
  blocks and the scripts do it), one at a time, with nothing else running
  beside them.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), never reboot, and never run `scripts/zfs-helpers.sh` or
  `zfs-tests.sh -x`. Rebuild nothing once STEP 3 has built the trees.
  `scripts/zfs.sh -r` (reload) and `-u` (unload) run only here, on this VM,
  and only in the blocks that say so, behind their guards.
- Work only under `/var/tmp/zts-rbr-1028` (`W`) and `/var/log/zts-rbr-1028`
  (`L`). Never write to, or delete, `/var/tmp/test_results`, another
  `/var/tmp/zts-*` or `/var/log/zts-*` directory, another `/var/tmp/vm-task-*`
  checkout, or anything else this task did not create; the doc checkout is
  only read until STEP 8 removes it. ZTS pools mount under `/testpool*` while
  they exist; the tests' cleanup removes those directories, and `run.sh`
  checks that it did.
- The only system setting written is `/proc/sysrq-trigger`, with `w` only
  (`run.sh`'s captures). test-runner itself writes `l` there when it kills a
  test at its timeout. Write no module parameter: the tests set theirs and
  put them back, and `run.sh` checks that they did.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the tests' pools in it while they exist and truncates it when they
  are gone.
- Never run a `zpool`, `zfs`, `zdb`, `zinject` or `zstream` command while
  none of this task's modules is loaded: libzfs would `modprobe` whatever zfs
  module the system has. Once `$L/WEDGED` exists, run none of them at all,
  nor `zfs.sh`. Never print the environment (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- The only `rm -rf` allowed is the last block of STEP 8.
- If a check does not print what its comment or the Expected lines say, stop
  at once and report. Do not rebuild, repair or retry, and run no test more
  than the series run. If your permission system refuses a command, do not
  work around it: report it and stop there. A kernel panic, oops, `VERIFY`,
  `ASSERT`, `BUG:` or hung-task message means stop and report, in whatever
  state it happened; never reboot. A stop in STEP 0 changes nothing: report
  and reply. After any later stop, go straight to STEP 8 with what you have.
  Never kill a D-state task, and never try to free a hung pool (`zpool
  clear`, export, destroy, `rmmod`): the hang is the evidence. A test result
  other than the expected one is a result, recorded, never a stop by itself.
- The two builds take about 30 minutes together, each series 10 to 30
  minutes. If your shell tool cannot wait that long for a command in the
  foreground, start that one command with the tool's own background mode and
  wait until it exits; do not use `&` or `nohup`, and run nothing else
  meanwhile.
- The session cannot run anything after its final reply: the last block of
  STEP 8 is the last command, and the reply comes after it. Leave no
  background command, loop or watcher behind.

## What earlier runs on this VM taught, and these steps already handle

- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines in
  `zfs_config.h` (one unconditional, two under `#ifdef`): 3 is a debug build.
- objtool prints about 83 `warning:` lines per tree (`spl_panic()` and
  `luaD_throw()` lacking `__noreturn`), harmless. Compiler warnings are
  counted only in the `file:line:col: warning:` form.
- A `-t` run names its test with `.ksh` in its `Test:` line, and each
  `Test:` line of the console log starts with a timestamp in brackets, which
  the scripts drop. A literal parenthesis in a regex is written `[(]`,
  never `\(`.
- A passing test's end line is its `log_pass` message: the last line before
  `NOTE: Performing local cleanup`.
- The kernel-log scan drops the ZTS `-K` marks (`ZTS run <test>`) and the
  `spa_import_progress_set_notes_impl` notes. ZTS empties dmesg after a
  failed test (its `zfs_dmesg.ksh` callback runs `dmesg -c`), so kernel
  messages are read per run from `journalctl -k`.
- A test that writes nothing on stderr has no `merged` file: its `stdout` is
  read instead. Each run waits 2 s after it ends: two runs started in the
  same second would share a results directory.
- `zfs-tests.sh` refuses to run as root, hence user `zts`. git refuses to
  work in a tree owned by another user: all git work happens in STEPs 1 to 3,
  before the `chown` to `zts`. `zfs-tests.sh -d` must point at a mode 1777
  directory.
- `modinfo` is in `/usr/sbin`: every block puts `/usr/sbin:/sbin` first in
  `PATH`. `zfs.sh -r` and `-u` signal the pid `/var/run/zed.pid` names, so
  they never run while it exists.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rbr-1028 L=/var/log/zts-rbr-1028 DOC=/var/tmp/vm-task-106x
date -u '+%F %T'
id -un                        # root
uname -r                      # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release   # Debian GNU/Linux 13 (trixie)
nproc                         # recorded: 16
umask                         # 0022
grep -m1 '^model name' /proc/cpuinfo
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zdb zed zstream zinject   # nothing: no system ZFS
pgrep -x zed                  # nothing
ls -l /var/run/zed.pid 2>&1   # No such file or directory: a module reload or unload signals the pid it names
modprobe -c 2>/dev/null | grep -E '^options (zfs|spl) '   # nothing
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -d $W $L 2>&1              # No such file or directory, twice
ls -A /var/tmp; ls -d /var/log/zts-* 2>&1
ls -d /var/tmp/vm-task-106u /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028 2>&1   # recorded: the last task on this VM
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|zts-rbr' /proc/mounts   # nothing
ls -d /testpool* 2>&1         # No such file or directory
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
df -h /var/tmp /var/log       # need 20G free on /var/tmp
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"
free -h
id zts; sudo -n -l -U zts | grep -i nopasswd
command -v git ksh python3 modinfo sha256sum truncate runuser timeout   # eight paths
command -v strace killall top   # recorded: the harness's dump on a test timeout uses them
for k in hung_task_timeout_secs hung_task_panic hung_task_warnings panic_on_oops panic_on_warn softlockup_panic sysrq; do
	printf '%s=%s ' $k "$(cat /proc/sys/kernel/$k 2>&1)"
done; echo
ls -d /var/log/journal 2>&1   # recorded: if it exists, the journal survives a reboot
journalctl -k -n 1 --no-pager -q   # one kernel line
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/x/VM-TESTS-106-rbr.md 2>&1
```

Keep this output, which goes into `REPORT.txt`. The number printed after the
one kernel line is the kernel-message baseline. Directories of earlier tasks
under `/var/tmp` and `/var/log` (`zts-*`, `vm-task-*`, `test_results`), the
`vm-task-106u` and `zts-rzx-1028` ones of the last task included, are
recorded only: their presence is not a stop, and this task never touches
them. The `nproc`, CPU, `free`, `strace`/`killall`/`top`, `/var/log/journal`
and `kernel.*` lines are recorded too (Debian's defaults are
`hung_task_timeout_secs=120 hung_task_panic=0 hung_task_warnings=10`). The
doc checkout line is expected as `doc checkout: top /var/tmp/vm-task-106x,
branch vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`) for a clone, or
`branch HEAD, head <a commit of vm-tasks-1026>, origin error: No such remote
'origin'` for a checkout fetched by commit; both are fine. If you read this
document from another place, say so in the report: STEP 8 then leaves that
place alone and removes no doc checkout.

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, a system `zpool`, `zfs`,
  `zdb`, `zed`, `zstream` or `zinject` is installed, `zed` runs,
  `/var/run/zed.pid` exists, or a `modprobe` option is set for zfs or spl
  (it would apply to every load);
- a test or build process is running (the `pgrep -af` line of the shell
  running this block, if it shows, is not one);
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` exists: this task would then have started once already;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load);
- less than 20 GB is free on `/var/tmp` (two built trees, the clone, the
  harness's three sparse 4 GiB file vdevs and the tests' 1 GiB ones);
- user `zts` is missing or has no passwordless sudo;
- one of the eight commands is missing, or `journalctl -k` prints no kernel
  line (the runs read their kernel messages from the journal);
- `hung_task_panic` is not 0 (a hung task would panic the VM and lose its
  state).

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself:
STEP 2 checks that it is within the range the trees support, and STEP 3 that
the modules were built for it. `strace`, `killall` or `top` missing is not a
stop either: only test-runner's dump on a test timeout would lack them.

## STEP 1: directories, `env.sh`, sources (git work, part one)

The environment file first, then this task's record of the doc checkout, a
fresh repository, the fetch of B by its commit from the fork and of upstream
master (read-only towards GitHub):

```sh
export PATH=/usr/sbin:/sbin:$PATH
mkdir /var/tmp/zts-rbr-1028 /var/log/zts-rbr-1028; echo "mkdir rc=$?"   # 0
cat > /var/log/zts-rbr-1028/env.sh <<'EOF'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rbr-1028
L=/var/log/zts-rbr-1028
DOC=/var/tmp/vm-task-106x
DOCFILE=x/VM-TESTS-106-rbr.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
BRANCH=rebuild-cancel-state-lock
# B: the branch's head; HF: its first commit, whose parent HM is upstream
# master.  M: HM with the test files of B.
HB=0e7f96966ea40a42c5a9e8deca3b727e5d5929df
HF=d0ac1636daac4dbbb34e0b2330b9ba2f2832986e
HM=e117a84dbbfe0befe15110b9d1d799f47b1a5546
# What B changes against HM (git diff --numstat), and the patch-ids of the
# whole change, of its tests/ part (what M gets) and of its module/ part.
NUMB='17 3 module/zfs/vdev_rebuild.c 1 1 tests/runfiles/common.run 1 0 tests/zfs-tests/tests/Makefile.am 115 0 tests/zfs-tests/tests/functional/replacement/rebuild_reopen.ksh'
PIDB=8dbb49899a4ac622e9405b6f4a3c92ef121e00e4
PIDT=348e97ceaf84feec13515d055971da0eab0d8a48
PIDK=592db2baeac5c735d5effd69f1cab7e06710769b
# The test files M gets from B, the test directory, the one file only B
# changes, and the blobs: vdev_rebuild.c at HM (VRM) and at HB (VRB);
# common.run, Makefile.am and rebuild_reopen.ksh at HB; scrub_cancel.ksh,
# the same at both.
TFILES='tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/replacement/rebuild_reopen.ksh'
RD=tests/zfs-tests/tests/functional/replacement
VR=module/zfs/vdev_rebuild.c
VRM=b159b3aac046c1a7d04cbd83cdc321003dd4a842
VRB=b1375ee8f7da6b8e7a89d196ee3ea52b669563ea
CRB=935fb2c86c4cc670eafdeeefa9d3aa7ef451cf12
MAB=0af8dc90eb4ffd0ee3e0ac92b53af0d14226d5ca
RRB=7c42a324fdca5ffbd6d7b7051214c1f430ba89c9
SCB=ab712364a89e88e40dc40aed0abd23cb0a89bbab
# rebuild_reopen runs per tree, and the outer timeout of one run (s).
NR=5
OUTER=1200
# Kernel messages: KPAT is what every run is counted with; KSTRICT the
# same without "call trace", which test-runner's dump on a test timeout
# also prints (KTO marks that dump); KIGN drops the ZTS -K marks and the
# import progress notes.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KSTRICT='VERIFY|ASSERT|PANIC|BUG:|Oops|blocked for more than'
KTO='ZTS timeout debug'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The tunables the two tests set and put back, and those that shape a
# rebuild, a scan or the debug log: every run must leave them as loaded.
TUN='zfs_scan_suspend_progress zfs_vdev_min_ms_count zfs_resilver_min_time_ms zfs_scrub_min_time_ms zfs_rebuild_max_segment zfs_rebuild_scrub_enabled zfs_rebuild_vdev_limit zfs_scan_legacy zfs_deadman_failmode zfs_dbgmsg_enable zfs_dbgmsg_maxsize'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: $2, expected $3"
	fi
}

tunables() {
	for p in $TUN; do
		if [ -e /sys/module/zfs/parameters/$p ]; then
			printf '%s=%s ' "$p" "$(cat /sys/module/zfs/parameters/$p)"
		else
			printf '%s=absent ' "$p"
		fi
	done
}

# The imported pools, read from the kstat directories: unlike zpool list,
# this never waits on a pool's locks.
pools_k() {
	for d in /proc/spl/kstat/zfs/*/; do
		[ -d "$d" ] && basename "$d"
	done | xargs
}

# Build <tree> in place, niced: autogen, configure (debug), make -j8.
build_tree() {   # build_tree <B|M>
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug && make -j8") \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "free=$(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/build.txt
		echo "build $1 failed: stop" | tee -a $L/build.txt
		return 1
	fi
}
EOF
bash -n /var/log/zts-rbr-1028/env.sh; echo "env.sh syntax rc=$?"   # 0
. /var/log/zts-rbr-1028/env.sh
date +%s > $L/start.txt
{
echo "start $(date -u '+%F %T') UTC; $(id -un); $(uname -r); umask $(umask)"
if [ "$(git -C $DOC rev-parse --show-toplevel 2>/dev/null)" = $DOC ] && [ -f $DOC/$DOCFILE ]; then
	git -C $DOC rev-parse HEAD > $L/doc-head.txt
	sha256sum < $DOC/$DOCFILE | cut -d' ' -f1 > $L/doc-sha.txt
fi
echo "doc checkout: head $(cat $L/doc-head.txt 2>&1), $DOCFILE sha256 $(cat $L/doc-sha.txt 2>&1)"
git init -q $W/repo > $L/init.log 2>&1; echo "init rc=$?"
git -C $W/repo fetch -q --no-tags $FORK "+$HB:refs/heads/b" > $L/fetch-fork.log 2>&1; echo "fetch B rc=$?"
git -C $W/repo fetch -q --no-tags $UPSTREAM "+refs/heads/master:refs/heads/upstream-master" > $L/fetch-upstream.log 2>&1; echo "fetch upstream rc=$?"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, `start ...; root; <uname -r>;
umask 0022`, the doc checkout line with its full head and the same sha256 as
STEP 0 printed, `init rc=0`, `fetch B rc=0`, `fetch upstream rc=0`, the free
space. A doc checkout line with `No such file or directory` means this
document was not read from `$DOC`: record it, it is not a stop (STEP 8 then
keeps `$DOC`). A failed init or fetch means stop (the fetch logs are
`$L/fetch-*.log`).

Then pin the commits: B and its two commits on master, master in upstream
master, what B changes and its patch-ids, `vdev_rebuild.c` in each. Where
upstream master and the fork branch point now is recorded only:

```sh
. /var/log/zts-rbr-1028/env.sh
{
cd $W/repo
same "B" "$(git rev-parse -q --verify refs/heads/b)" $HB
same "B's parent" "$(git rev-parse -q --verify $HB^)" $HF
same "its parent, master" "$(git rev-parse -q --verify $HF^)" $HM
same "commits from master to B" "$(git rev-list --count $HM..$HB)" 2
same "master in upstream master" "$(git merge-base --is-ancestor $HM refs/heads/upstream-master && echo yes)" yes
same "what B changes" "$(git diff --numstat $HM $HB | xargs)" "$NUMB"
same "B's patch-id" "$(git diff $HM $HB | git patch-id --stable | cut -d' ' -f1)" $PIDB
same "its tests/ part" "$(git diff $HM $HB -- tests/ | git patch-id --stable | cut -d' ' -f1)" $PIDT
same "its module/ part" "$(git diff $HM $HB -- module/ | git patch-id --stable | cut -d' ' -f1)" $PIDK
same "what the first commit changes" "$(git diff --name-only $HM $HF | xargs)" "$VR $TFILES"
same "what the head changes" "$(git diff --name-only $HF $HB | xargs)" $VR
same "vdev_rebuild.c at master, at B" "$(git rev-parse -q --verify $HM:$VR) $(git rev-parse -q --verify $HB:$VR)" "$VRM $VRB"
same "the test at master, at B" "$(git rev-parse -q --verify $HM:$RD/rebuild_reopen.ksh) $(git rev-parse -q --verify $HB:$RD/rebuild_reopen.ksh)" " $RRB"
for c in $HM $HF $HB; do echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"; done
echo "upstream master now (recorded only): $(git log -1 --abbrev=10 --format='%h %s' refs/heads/upstream-master)"
echo "on GitHub now (recorded only): $(git ls-remote $FORK refs/heads/$BRANCH 2>&1 | awk '{ printf "%s=%.10s", $2, $1 }')"
} 2>&1 | tee $L/sha-checks.txt
```

Expected:

```
ok: B
ok: B's parent
ok: its parent, master
ok: commits from master to B
ok: master in upstream master
ok: what B changes
ok: B's patch-id
ok: its tests/ part
ok: its module/ part
ok: what the first commit changes
ok: what the head changes
ok: vdev_rebuild.c at master, at B
ok: the test at master, at B
  e117a84dbb libzutil: keep the latest label when two vdevs claim one id
  d0ac1636da Don't cancel a rebuild over a leaf that is being reopened
  0e7f96966e Hold SCL_STATE in the rebuild initiate and reset sync tasks
upstream master now (recorded only): <its head>
on GitHub now (recorded only): refs/heads/rebuild-cancel-state-lock=0e7f96966e
```

(`the test at master, at B` compares a line that starts with a space: master
has no such file.) Any `MISMATCH` means stop. The last two lines are not
checked: a branch moved since 28 Oct changes nothing here, since B is
fetched by its commit.

## STEP 2: the trees: B, M with the test files, the checks (git work, part two)

One worktree per tree, the test files of B as a patch, applied to M with
its index:

```sh
. /var/log/zts-rbr-1028/env.sh
{
for t in B:$HB M:$HM; do
	git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD 2>&1)"
done
git -C $W/repo diff $HM $HB -- tests/ > $L/tests.diff; echo "tests.diff rc=$?, $(grep -c '^diff --git ' $L/tests.diff) files"
same "tests.diff's patch-id" "$(git -C $W/repo patch-id --stable < $L/tests.diff | cut -d' ' -f1)" $PIDT
git -C $W/M apply --index $L/tests.diff; echo "apply to M rc=$?"
} 2>&1 | tee $L/worktrees.txt
```

Expected: `worktree B rc=0 at 0e7f96966e`, `worktree M rc=0 at e117a84dbb`,
`tests.diff rc=0, 3 files`, `ok: tests.diff's patch-id`, `apply to M rc=0`.
Anything else means stop.

Then what each tree holds: `vdev_rebuild.c` and its markers (`SCL_STATE as
reader` the `spa_config_enter()` lines the fix adds, `the old check` the
`if (!vdev_resilver_needed(` of master, `the new check` the fix's `needed =`),
the two tests, the directory's `setup` and `cleanup`, the registration of the
test, and the range of kernels `META` declares; then M's changes against
master and against B:

```sh
. /var/log/zts-rbr-1028/env.sh
{
k=$(uname -r | cut -d. -f1,2)
for t in B M; do
	cd $W/$t
	echo "$t: $(git log -1 --abbrev=10 --format='%h %s')"
	echo "$t: vdev_rebuild.c $(git hash-object $VR | cut -c1-10):" \
	    "SCL_STATE as reader x$(grep -cF 'spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);' $VR)," \
	    "the old check x$(grep -c 'if [(].vdev_resilver_needed[(]vd, &vrp' $VR)," \
	    "the new check x$(grep -cF 'needed = vdev_resilver_needed(vd, &vrp->vrp_min_txg,' $VR)"
	echo "$t: rebuild_reopen $(ls -l $RD/rebuild_reopen.ksh 2>&1 | cut -c1-10)," \
	    "scrub_cancel $(ls -l $RD/scrub_cancel.ksh 2>&1 | cut -c1-10)," \
	    "setup $(ls -l $RD/setup.ksh 2>&1 | cut -c1-10), cleanup $(ls -l $RD/cleanup.ksh 2>&1 | cut -c1-10);" \
	    "in common.run x$(grep -c "'rebuild_reopen'" tests/runfiles/common.run)," \
	    "in Makefile.am x$(grep -c 'functional/replacement/rebuild_reopen[.]ksh' tests/zfs-tests/tests/Makefile.am)"
	same "$t's test files are B's" "$(git hash-object $TFILES 2>&1 | xargs)" "$CRB $MAB $RRB"
	same "$t's scrub_cancel" "$(git hash-object $RD/scrub_cancel.ksh 2>&1)" $SCB
	lo=$(awk '/^Linux-Minimum:/ { print $2 }' META)
	hi=$(awk '/^Linux-Maximum:/ { print $2 }' META)
	same "$t supports Linux $k (META: $lo to $hi)" \
	    "$([ -n "$lo" ] && [ -n "$hi" ] && [ "$(printf '%s\n' $lo $k $hi | sort -V | xargs)" = "$lo $k $hi" ] && echo within)" within
done
same "B as committed" "$(git -C $W/B status --porcelain --untracked-files=all | wc -l)" 0
same "M: the test files, staged" "$(git -C $W/M status --porcelain --untracked-files=all | xargs)" \
    "M tests/runfiles/common.run M tests/zfs-tests/tests/Makefile.am A tests/zfs-tests/tests/functional/replacement/rebuild_reopen.ksh"
same "M's patch-id against master" "$(git -C $W/M diff --cached HEAD | git -C $W/M patch-id --stable | cut -d' ' -f1)" $PIDT
same "M against master, outside tests/" "$(git -C $W/M diff HEAD --name-only -- . ':!tests' | wc -l)" 0
same "M's vdev_rebuild.c is master's" "$(git -C $W/M hash-object $VR)" $VRM
same "B's vdev_rebuild.c is the fix" "$(git -C $W/B hash-object $VR)" $VRB
same "M against B" "$(git -C $W/M diff $HB --name-only | xargs)" $VR
same "M against B, its size" "$(git -C $W/M diff $HB --stat | tail -n 1)" " 1 file changed, 3 insertions(+), 17 deletions(-)"
} 2>&1 | tee $L/trees.txt
```

Expected, in order:

```
B: 0e7f96966e Hold SCL_STATE in the rebuild initiate and reset sync tasks
B: vdev_rebuild.c b1375ee8f7: SCL_STATE as reader x3, the old check x0, the new check x1
B: rebuild_reopen -rwxr-xr-x, scrub_cancel -rwxr-xr-x, setup -rwxr-xr-x, cleanup -rwxr-xr-x; in common.run x1, in Makefile.am x1
ok: B's test files are B's
ok: B's scrub_cancel
ok: B supports Linux 6.12 (META: 4.18 to 7.2)
M: e117a84dbb libzutil: keep the latest label when two vdevs claim one id
M: vdev_rebuild.c b159b3aac0: SCL_STATE as reader x0, the old check x1, the new check x0
M: rebuild_reopen -rwxr-xr-x, scrub_cancel -rwxr-xr-x, setup -rwxr-xr-x, cleanup -rwxr-xr-x; in common.run x1, in Makefile.am x1
ok: M's test files are B's
ok: M's scrub_cancel
ok: M supports Linux 6.12 (META: 4.18 to 7.2)
ok: B as committed
ok: M: the test files, staged
ok: M's patch-id against master
ok: M against master, outside tests/
ok: M's vdev_rebuild.c is master's
ok: B's vdev_rebuild.c is the fix
ok: M against B
ok: M against B, its size
```

(`6.12` is the running kernel's version; another one is fine as long as both
lines say `ok`.) Any `MISMATCH`, or any other difference, means stop: M must
hold the test and not the fix.

## STEP 3: builds, checks, and the hand-over to `zts` (git work, part three)

Build B, then M (about 30 minutes together; see the rule about long
commands):

```sh
. /var/log/zts-rbr-1028/env.sh
build_tree B && build_tree M
```

Expected: `build B rc=0 wall=<s>s free=<n>G`, then `build M rc=0 wall=<s>s
free=<n>G`. A failed build prints its `error:` lines and `build <tree>
failed: stop`: stop.

Then check what was built, and that the builds changed no tracked file:

```sh
. /var/log/zts-rbr-1028/env.sh
{
for t in B M; do
	cd $W/$t
	same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
	same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1)" 3
	same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
	same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb zinject \
	    scripts/zfs.sh scripts/zfs-tests.sh tests/test-runner/bin/test-runner.py \
	    $RD/rebuild_reopen.ksh $RD/scrub_cancel.ksh $RD/setup.ksh $RD/cleanup.ksh; do
		[ -e $f ] || echo $f; done | xargs)" ""
	modinfo -F srcversion module/zfs.ko > $L/srcv-$t 2>&1
	echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
	    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:')" \
	    "(objtool's, about 83 expected); srcversion $(cat $L/srcv-$t)"
	grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
	echo "$t last build line: $(tail -n 1 $L/build-$t.log)"
done
same "B and M srcversions differ" "$(cat $L/srcv-B $L/srcv-M | grep -xE '[0-9A-F]+' | sort -u | wc -l)" 2
same "B as committed after the build" "$(git -C $W/B status --porcelain --untracked-files=no | wc -l)" 0
same "M after the build" "$(git -C $W/M status --porcelain --untracked-files=no | xargs)" \
    "M tests/runfiles/common.run M tests/zfs-tests/tests/Makefile.am A tests/zfs-tests/tests/functional/replacement/rebuild_reopen.ksh"
same "M against B after the build" "$(git -C $W/M diff $HB --name-only | xargs)" $VR
same "M's vdev_rebuild.c after the build" "$(git -C $W/M hash-object $VR)" $VRM
echo "M srcversion $(cat $L/srcv-M) (recorded only: the master module of the earlier tasks on this VM gave 8852710DB871A7587B9E722, and e117a84db differs from that master outside tests/ in META's Linux-Maximum line only)"
echo "free after the builds: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (10G at least)"
} 2>&1 | tee $L/step3.txt
```

Expected: 4 `ok:` lines per tree, for each tree `compiler warnings: 0` and
about 83 other `warning:` lines with its srcversion, a last build line of a
finished build (typically `make[1]: Leaving directory
'/var/tmp/zts-rbr-1028/<B or M>'`), then 5 more `ok:` lines, M's srcversion
and the free space. Any `MISMATCH`, or less than `10G` free, means stop. A
compiler warning count other than 0 is not a stop: its first lines are
printed, quote them in the report.

Then point both trees' results at `$L/results` (test-runner writes them to
`/var/tmp/test_results` otherwise), store the two srcversions for the guards,
and hand the work directory to `zts`. This is the last git command of the
task but STEP 8's check of the doc checkout:

```sh
. /var/log/zts-rbr-1028/env.sh
{
for t in B M; do
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    $W/$t/tests/test-runner/bin/test-runner.py.in \
	    $W/$t/tests/test-runner/bin/test-runner.py
	same "$t results directory" \
	    "$(grep -c "^BASEDIR = '$L/results'\$" $W/$t/tests/test-runner/bin/test-runner.py)" 1
done
same "B changes now" "$(git -C $W/B diff HEAD --name-only | xargs)" tests/test-runner/bin/test-runner.py.in
same "M changes now" "$(git -C $W/M diff HEAD --name-only | xargs)" \
    "tests/runfiles/common.run tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/replacement/rebuild_reopen.ksh"
same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
same "M head" "$(git -C $W/M rev-parse HEAD)" $HM
} 2>&1 | tee $L/step3b.txt
if ! grep -q '^MISMATCH' $L/sha-checks.txt $L/worktrees.txt $L/trees.txt $L/step3.txt $L/step3b.txt &&
    [ -s $L/srcv-B ] && [ -s $L/srcv-M ] && ! cmp -s $L/srcv-B $L/srcv-M &&
    [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -ge 10 ]; then
	printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh &&
	    echo "srcversions recorded"
	tail -n 2 $L/env.sh
	mkdir $W/files $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
	chown -R zts $W $L/results; echo "chown rc=$?"                        # 0
	stat -c '%U %a %n' $W/files $L/results
else
	echo "CHECKS FAILED: stop"
fi
```

Expected: 6 `ok:` lines (the `test-runner.py.in` change is the results
directory; in M, the three test files are the other changes), `srcversions
recorded`, the two lines `SV_B=<B's>` and `SV_M=<M's>`, `mkdir rc=0`, `chown
rc=0`, then `zts 1777 /var/tmp/zts-rbr-1028/files` and `zts 755
/var/log/zts-rbr-1028/results`. On `CHECKS FAILED`, stop.

## STEP 4: the scripts

`run.sh` runs one ZTS run on the loaded tree, as `zts`, behind its guards,
under the outer timeout, then writes its digest and checks what it left (see
"How a run is read" and "Watching for a wedge"):

```sh
cat > /var/log/zts-rbr-1028/run.sh <<'EOS'
#!/bin/sh
# usage: run.sh <B|M> <label> <rebuild_reopen|scrub_cancel>
# One zfs-tests.sh -t run of functional/replacement/<test> on the loaded
# tree, as zts, under the outer timeout; then its digest, its rebuild
# events, its kernel messages and its leftovers.  Exit 0: the run is clean,
# whatever the test result; 1: a guard failed, nothing ran; 2: a bad
# argument; 3: a leftover, a changed tunable, a kernel finding, no ZTS mark
# in the journal, or an outer timeout; 4: wedged (an outer timeout or a
# kernel finding, with a pool or a D-state task left): no zpool, zfs, zdb,
# zinject or zfs.sh command may run any more.
. /var/log/zts-rbr-1028/env.sh
t=$1; lbl=$2; name=$3
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> <rebuild_reopen|scrub_cancel>"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
case $name in
rebuild_reopen|scrub_cancel) ;;
*) echo "the test is rebuild_reopen or scrub_cancel"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
if [ ! -x "$W/$t/$RD/$name.ksh" ]; then
	echo "no such test in $t: $RD/$name.ksh"; exit 2
fi
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi
G=$L/guards.txt
if [ -e "$L/WEDGED" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: wedged: nothing run" | tee -a "$G"
	exit 1
fi

# The result file of test <name> in directory <dir> (the last component of
# its directory): its "merged" file, else its "stdout".
outfile() {   # outfile <results dir> <dir> <name>
	for f in merged stdout; do
		o=$(find "$1" -type f -name "$f" 2>/dev/null |
		    grep -E "/$2(:Linux)?/$3([.]ksh)?/$f\$" | head -n 1)
		if [ -n "$o" ]; then
			echo "$o"; return 0
		fi
	done
	return 1
}

# The lines of a result file before the failure callbacks and the cleanup,
# and the last of them: a passing test's is its log_pass message.
body() {   # body <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { exit } { print }' "$1"
}
endline() {   # endline <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { f = 1; exit }
	    NF { prev = $0 }
	    END {
		if (f) print "  end line: " prev
		else print "  end line: none (no cleanup reached), last line: " prev
	    }' "$1"
}

# "<pid>:<comm> ..." of every task in D state.
dstate() {
	ps -eo pid=,stat=,comm= | awk '$2 ~ /^D/ { printf "%s%s:%s", s, $1, $3; s = " " }'
}

# Evidence of a wedge, read without any zpool or zfs command (a hung pool
# blocks them): each pool's last txgs, every D-state task and its stack,
# the taskqs; then the blocked tasks the kernel dumps on sysrq w, the whole
# ring buffer and the tail of the debug log.
capture() {   # capture <prefix>
	{
	echo "== $(date -u '+%F %T'), uptime $(cut -d' ' -f1 /proc/uptime) s"
	echo "== imported pools (kstat): [$(pools_k)]"
	echo "== last txgs"
	for d in /proc/spl/kstat/zfs/*/; do
		[ -e "${d}txgs" ] && { echo "${d}txgs"; tail -n 6 "${d}txgs"; }
	done
	echo "== D-state tasks and their stacks"
	for p in $(ps -eo pid=,stat= | awk '$2 ~ /^D/ { print $1 }'); do
		echo "-- pid $p $(cat "/proc/$p/comm" 2>/dev/null)"
		cat "/proc/$p/stack" 2>&1
	done
	echo "== /proc/spl/kstat/taskq/summary"
	cat /proc/spl/kstat/taskq/summary 2>&1
	} > "$1.txt" 2>&1
	s=$(date +%s)
	echo w > /proc/sysrq-trigger
	sleep 3
	journalctl -k --since "@$s" --no-pager -q > "$1-sysrq.txt" 2>&1
	dmesg > "$1-dmesg.txt" 2>&1
	tail -n 400 /proc/spl/kstat/zfs/dbgmsg > "$1-dbgmsg.txt" 2>&1
}

# Guards before the run.  The loaded module comes first, so that no zpool
# command runs without this tree's module; then the tunables as recorded
# after the load, no pool, nothing in the files directory, no loop device,
# no test mount and no test mount point.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion=$cur want=$sv:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pk=$(pools_k)
pools=""
[ -z "$pk" ] && pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(ls -A "$W/files" 2>&1 | xargs)
loops=$(losetup -a 2>&1)
mnts=$(grep -E ' zfs |testpool' /proc/mounts)
roots=$(ls -d /testpool* 2>/dev/null | xargs)
echo "$(date -u +%T) before $lbl ($t $name): srcversion=$cur pools=[$pools]" \
    "kstat_pools=[$pk] files=[$files] loops=[$loops] mounts=[$mnts]" \
    "rootdirs=[$roots]" | tee -a "$G"
if [ "$tun" != "$(cat "$L/tun-$t.txt" 2>&1)" ] ||
    [ -n "$pools$pk$files$loops$mnts$roots" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: tunables=[$tun]: nothing run" | tee -a "$G"
	exit 1
fi

start=$(date +%s)
cd "$W/$t" || exit 1
timeout -k 60 "$OUTER" nice -n 10 ionice -c3 \
    runuser -u zts -- ./scripts/zfs-tests.sh -vK -D -d "$W/files" \
    -t "tests/functional/replacement/$name.ksh" > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
to=no
case $rc in
124|137) to=yes ;;
esac
# Two runs started in the same second would share a results directory.
sleep 2

# Kernel messages of this run.  "ZTS run" is the -K mark: it proves the
# journal saw the run.  A run's findings are its KSTRICT lines, and its
# "call trace" lines too unless test-runner dumped a timeout (KTO), whose
# sysrq l backtraces print them.
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
kn=$(grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
ks=$(grep -iE "$KSTRICT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
kc=$(grep -ic 'call trace' "$L/$lbl-kernel.txt")
kt=$(grep -c "$KTO" "$L/$lbl-kernel.txt")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")
kf=$ks
[ "$kt" -eq 0 ] && kf=$((ks + kc))

# A wedge: after an outer timeout or a finding, a pool still imported or a
# task in D state.  The capture comes first, while the kernel holds it all.
lpk=$(pools_k)
lD=$(dstate)
wedged=no
if [ "$to" = yes ] || [ "$kf" -gt 0 ]; then
	capture "$L/$lbl-capture"
	[ -n "$lpk$lD" ] && wedged=yes
fi

# Results.  Each "Test:" line of the console log starts with a timestamp,
# which is dropped.
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
tl=$(grep -E "/replacement/$name([.]ksh)? [(]run as " "$L/$lbl-tests.txt" | tail -n 1)
res=$(printf '%s\n' "$tl" | grep -oE '\[[A-Z]+\]$' | tr -d '[]')
tt=$(printf '%s\n' "$tl" | grep -oE '\[[0-9:]+\] \[[A-Z]+\]$' | cut -d' ' -f1 | tr -d '[]')
o=""
[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" replacement "$name")
el=""
nrep=none
if [ -n "$o" ]; then
	cp "$o" "$L/$lbl-out.txt"
	el=$(endline "$o")
	nrep=$(body "$o" | grep -c 'SUCCESS: zpool replace ')
fi

# The rebuild events of the debug log stamped at or after the start: in
# the test's output (the test-fail callback's copy) and in the debug log.
{
	[ -n "$o" ] && cat "$o"
	cat /proc/spl/kstat/zfs/dbgmsg 2>/dev/null
} | awk -v s="$start" '$1 ~ /^[0-9]+$/ && $1 + 0 >= s + 0' |
    grep -oE 'txg [0-9]+ rebuild vdev_id=[0-9]+ vdev_guid=[0-9]+ (started|complete|canceled|reset)' |
    sort -u | sort -n -k 2 > "$L/$lbl-rebuild.txt"
ev() {   # ev <event>: how many
	grep -c " $1\$" "$L/$lbl-rebuild.txt"
}
cx=no
printf '%s\n' "$el" | grep -q 'The sequential resilver was canceled' && cx=yes
[ "$(ev canceled)" -gt 0 ] && cx=yes

{
echo "$lbl: tree $t, $name, $(wc -l < "$L/$lbl-tests.txt") results: ${cnt:-none};" \
    "test [${res:-none}] in ${tt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s," \
    "outer timeout $to; log dir ${rd:-none}"
echo "${miss:-no Missing util(s) line}"
sed 's/^/  /' "$L/$lbl-tests.txt"
if [ -n "$o" ]; then
	echo "  zpool replace completed: $nrep"
	grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
	echo "$el"
	if [ "$res" != PASS ]; then
		echo "  the test's last 15 lines before its end:"
		body "$o" | tail -n 15 | sed 's/^/  | /'
	fi
else
	echo "  no stdout or merged file found for the test; the console log's last 15 lines:"
	tail -n 15 "$L/$lbl.log" | sed 's/^/  | /'
fi
echo "  rebuild events since the start: started $(ev started), complete $(ev complete)," \
    "canceled $(ev canceled), reset $(ev reset); rebuild canceled: $cx"
head -n 20 "$L/$lbl-rebuild.txt" | sed 's/^/    /'
echo "  kernel: $kn lines match ($kf findings; $kc call trace lines, $kt timeout dumps of test-runner), $km ZTS marks"
grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vE "$KIGN" | head -n 20 | sed 's/^/    /'
if [ -e "$L/$lbl-capture.txt" ]; then
	echo "  captured: $lbl-capture.txt, -sysrq.txt, -dmesg.txt, -dbgmsg.txt;" \
	    "kstat pools [$lpk], D-state tasks [$lD]"
fi
} > "$L/$lbl-notes.txt"

end=${el#  end line: }
echo "$lbl tree=$t test=$name result=${res:-none} time=${tt:-none} wall=${wall}s" \
    "rc=$rc timeout=$to replaces=$nrep started=$(ev started) complete=$(ev complete)" \
    "canceled=$(ev canceled) reset=$(ev reset) rebuild_canceled=$cx kernel=$kn" \
    "findings=$kf calltrace=$kc timeoutdumps=$kt zts_marks=$km wedged=$wedged" \
    "end=${end:-(no result file)}" > "$L/$lbl-sum.txt"

# Leftovers: all must be empty, and the tunables as after the load.  No
# zpool command once the system may be wedged.
if [ "$wedged" = yes ]; then
	lp="(not read: wedged)"
elif [ -n "$lpk" ]; then
	lp="(not read: a pool is left)"
else
	lp=$("$W/$t/zpool" list -H -o name 2>&1)
fi
ll=$(losetup -a 2>&1)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(ls -A "$W/files" 2>&1 | xargs)
lm=$(grep -E ' zfs |testpool' /proc/mounts)
lr=$(ls -d /testpool* 2>/dev/null | xargs)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
# Leftover processes, leaving out this script and the shells that started
# it: their command lines can contain the same words.
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests[.]sh|test-runner[.]py|functional/|zpool |zinject ' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)

echo "$(date -u +%T) after $lbl: result=${res:-none} rc=$rc wall=${wall}s timeout=$to" \
    "zts_marks=$km kernel=$kn findings=$kf calltrace=$kc timeoutdumps=$kt" \
    "wedged=$wedged" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] kstat_pools=[$lpk]" \
    "loops=[$ll] dm=[$ld] files=[$lf] mounts=[$lm] rootdirs=[$lr]" \
    "procs=[$lq] dstate=[$lD] tunables_same=$ts" | tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ "$wedged" = yes ]; then
	echo "$lbl: wedged after rc=$rc timeout=$to findings=$kf, kstat pools [$lpk], D-state [$lD]" >> "$L/WEDGED"
	echo "WEDGED after $lbl: no zpool, zfs, zdb, zinject or zfs.sh command" \
	    "may run any more: go to STEP 8 (wedged)" | tee -a "$G"
	exit 4
fi
if [ -n "$lp$lpk$ll$ld$lf$lm$lr$lq" ] || [ "$ts" != yes ] ||
    [ "$kf" -gt 0 ] || [ "$km" -lt 1 ] || [ "$to" = yes ]; then
	echo "STOP after $lbl: leftover, tunable changed, kernel finding," \
	    "no ZTS mark in the journal, or outer timeout" | tee -a "$G"
	exit 3
fi
exit 0
EOS
```

`run.sh` exits 0 when the run is clean, whatever the test result; 1 when a
guard before the run failed (nothing ran); 2 on a bad argument; 3 when
something is wrong after the run (a leftover, a changed tunable, a kernel
finding, no ZTS mark in the journal, an outer timeout); 4 when the system is
wedged (`$L/WEDGED` written). On 1, 2 or 3, stop and report; clean nothing by
hand. On 4, go to STEP 8, wedged branch.

`series.sh` runs one tree's series on the loaded tree, one `run.sh` each, and
stops at the first that does not exit 0:

```sh
cat > /var/log/zts-rbr-1028/series.sh <<'EOS'
#!/bin/sh
# usage: series.sh <B|M>: on the loaded tree, rebuild_reopen NR times
# (labels b01... or m01...), then scrub_cancel once (bsc or msc); stops at
# the first run.sh that does not exit 0, then prints each run's summary.
. /var/log/zts-rbr-1028/env.sh
case $1 in
B) p=b ;;
M) p=m ;;
*) echo "usage: series.sh <B|M>"; exit 2 ;;
esac
i=1
rc=0
while [ $i -le $((NR + 1)) ]; do
	if [ $i -le "$NR" ]; then
		l=$p$(printf %02d $i); n=rebuild_reopen
	else
		l=${p}sc; n=scrub_cancel
	fi
	sh "$L/run.sh" "$1" "$l" "$n"; rc=$?
	echo "run.sh $l rc=$rc"
	if [ $rc -ne 0 ]; then
		echo "series $1 stopped at $l"
		break
	fi
	i=$((i + 1))
done
[ $rc -eq 0 ] && echo "series $1 done:"
for f in "$L/$p"[0-9][0-9]-sum.txt "$L/${p}sc-sum.txt"; do
	[ -e "$f" ] && echo "  $(cat "$f")"
done
exit $rc
EOS
```

Then the syntax checks:

```sh
. /var/log/zts-rbr-1028/env.sh
{
for f in run.sh series.sh; do
	bash -n $L/$f; echo "$f bash syntax rc=$?"
	sh -n $L/$f; echo "$f sh syntax rc=$?"
done
ls -l $L/run.sh $L/series.sh | cut -d' ' -f1,5,9
} 2>&1 | tee $L/step4.txt
```

Expected: four `syntax rc=0` lines and the two files.

## STEP 5: load B, then B's series

Load B. Nothing is loaded yet (STEP 0), so the reload loads:

```sh
. /var/log/zts-rbr-1028/env.sh
{
echo "loaded before: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # []
if [ -e /var/run/zed.pid ] || pgrep -x zed > /dev/null; then
	echo "zed runs or /var/run/zed.pid exists: nothing loaded: stop"
else
	(cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -r); echo "load B rc=$?"   # 0
fi
echo "loaded $(cat /sys/module/zfs/srcversion 2>&1), SV_B $SV_B"             # the same value twice
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ]; then
	echo "kstat pools: [$(pools_k)]"; $W/B/zpool list -H -o name; echo "zpool list rc=$?"
	echo "spl_panic_halt: $(cat /sys/module/spl/parameters/spl_panic_halt 2>&1)"   # 0
	tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
fi
} 2>&1 | tee $L/load-B.txt
```

Expected: `loaded before: []`, `load B rc=0`, the same srcversion twice,
`kstat pools: []`, `zpool list rc=0` with no pool name, `spl_panic_halt: 0`,
then

```
zfs_scan_suspend_progress=0 zfs_vdev_min_ms_count=16 zfs_resilver_min_time_ms=1500 zfs_scrub_min_time_ms=750 zfs_rebuild_max_segment=1048576 zfs_rebuild_scrub_enabled=1 zfs_rebuild_vdev_limit=67108864 zfs_scan_legacy=0 zfs_deadman_failmode=wait zfs_dbgmsg_enable=1 zfs_dbgmsg_maxsize=4194304
```

(the module's defaults; another value set by this VM's configuration is fine,
since every run is compared with what was recorded here, but
`zfs_deadman_failmode` must not read `panic`, and `zfs_dbgmsg_enable` 0 means
the runs can show no rebuild event: record it). If the load fails, the
srcversion is not B's, a pool appears, `spl_panic_halt` is not 0 or
`zfs_deadman_failmode` reads `panic`, stop.

Then B's series: `rebuild_reopen` 5 times, `scrub_cancel` once (10 to 30
minutes; see the rule about long commands):

```sh
L=/var/log/zts-rbr-1028
sh $L/series.sh B > $L/series-B.txt 2>&1; echo "series.sh rc=$?" >> $L/series-B.txt; sed -n '/^series /,$p' $L/series-B.txt
```

Expected: `series B done:`, then six lines

```
  b0<i> tree=B test=rebuild_reopen result=PASS time=<mm:ss> wall=<s>s rc=0 timeout=no replaces=6 started=6 complete=6 canceled=0 reset=0 rebuild_canceled=no kernel=0 findings=0 calltrace=0 timeoutdumps=0 zts_marks=3 wedged=no end=Refused scrubs do not cancel a sequential resilver
  ...
  bsc tree=B test=scrub_cancel result=PASS time=<mm:ss> wall=<s>s rc=0 timeout=no replaces=4 started=2 complete=2 canceled=0 reset=0 rebuild_canceled=no kernel=0 findings=0 calltrace=0 timeoutdumps=0 zts_marks=3 wedged=no end=Scrub was cancelled by resilver
```

then `series.sh rc=0`. `bsc`'s `started` and `complete` count its two
sequential replaces (the healing ones are not rebuilds); `zfs-tests.sh rc`
and the event counts are recorded, not checked. Each run's notes
(`$L/<label>-notes.txt`, also in `series-B.txt`) show the `setup`, test and
`cleanup` results, the end line and the rebuild events; in `guards.txt`, each
run's `after` line shows `zts_marks=3 kernel=0 findings=0 ... wedged=no`, and
its leftovers line is empty everywhere with `tunables_same=yes`. A result
other than `PASS` is recorded, not a stop; `series B stopped at <label>` is a
stop (the run's own lines in `series-B.txt` say why), and a `run.sh ... rc=4`
sends you to STEP 8, wedged branch.

## STEP 6: swap to M, then M's series

Swap the modules only while B's is loaded with no pool, no mount, no `zed`
and no `$L/WEDGED`; `zfs.sh -r` from M's tree unloads B's stack and loads
M's:

```sh
. /var/log/zts-rbr-1028/env.sh
{
cur=$(cat /sys/module/zfs/srcversion 2>&1)
pk=$(pools_k)
p=""; [ "$cur" = "$SV_B" ] && [ -z "$pk" ] && p=$($W/B/zpool list -H -o name 2>&1)
m=$(grep -E ' zfs |testpool' /proc/mounts)
z=$(pgrep -x zed)$(ls /var/run/zed.pid 2>/dev/null)
echo "before the swap: B's module loaded: $([ "$cur" = "$SV_B" ] && echo yes || echo "no ($cur)"), wedged [$(cat $L/WEDGED 2>/dev/null)], kstat pools [$pk], pools [$p], mounts [$m], zed [$z]"
if [ "$cur" = "$SV_B" ] && [ ! -e $L/WEDGED ] && [ -z "$pk$p$m$z" ]; then
	(cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh -r); echo "reload with M rc=$?"
else
	echo "not swapped: stop"
fi
echo "loaded $(cat /sys/module/zfs/srcversion 2>&1), SV_M $SV_M"
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ]; then
	echo "kstat pools: [$(pools_k)]"; $W/M/zpool list -H -o name; echo "zpool list rc=$?"
	echo "spl_panic_halt: $(cat /sys/module/spl/parameters/spl_panic_halt 2>&1)"
	tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo
	same "M's tunables as B's" "$(cat $L/tun-M.txt)" "$(cat $L/tun-B.txt)"
fi
} 2>&1 | tee $L/swap.txt
```

Expected: `before the swap: B's module loaded: yes, wedged [], kstat pools [],
pools [], mounts [], zed []`, `reload with M rc=0`, the same srcversion twice
(M's), `kstat pools: []`, `zpool list rc=0` with no pool name,
`spl_panic_halt: 0`, the tunables line as B's, `ok: M's tunables as B's`.
Anything else means stop.

Then M's series, the same runs (10 to 30 minutes; up to 23 minutes more for a
run that wedges):

```sh
L=/var/log/zts-rbr-1028
sh $L/series.sh M > $L/series-M.txt 2>&1; echo "series.sh rc=$?" >> $L/series-M.txt; sed -n '/^series /,$p' $L/series-M.txt
```

What the analysis predicts (record whatever is printed instead):
`series M done:`, then for each `rebuild_reopen` run that hits the bug

```
  m0<i> tree=M test=rebuild_reopen result=FAIL time=<mm:ss> wall=<s>s rc=<n> timeout=no replaces=1 started=1 complete=0 canceled=<0 or 1> reset=0 rebuild_canceled=yes kernel=0 findings=0 calltrace=0 timeoutdumps=0 zts_marks=3 wedged=no end=The sequential resilver was canceled
```

(`replaces=1`: the first rebuild; `canceled` is 1 when the event was within
the 200 lines the test-fail callback kept, 0 otherwise; `rebuild_canceled`
says `yes` from the end line either way), a line like B's for any run that
does not hit it, and for `msc` a line with `result=PASS`, `FAIL` or `KILLED`.
A `KILLED` `msc` is test-runner's 600 s timeout in `zpool wait` (the same bug,
another symptom): its line then shows `timeoutdumps=1` and some `calltrace`
lines with `findings=0`, and its leftovers (the test's file vdevs in
`$W/files`, perhaps the changed tunables) stop the series there, which ends
it anyway: `series M stopped at msc`, `series.sh rc=3`. That stop is a
result, not an error. Then `series.sh rc=0` (or `3` as above).

After a wedge: `series M stopped at m0<i>` (or `msc`) with `run.sh ... rc=4`
and `series.sh rc=4`; in `guards.txt` the run's `after` line with
`wedged=yes`, its leftovers line with `pools=[(not read: wedged)]` and the
D-state tasks, then `WEDGED after ...`. Go to STEP 8, wedged branch.

## STEP 7: the two trees side by side

```sh
. /var/log/zts-rbr-1028/env.sh
{
for t in B M; do
	p=$(echo $t | tr BM bm)
	n=0; np=0; nf=0; no=0; nc=0; nk=0
	for i in $(seq 1 $NR); do
		f=$L/$p$(printf %02d $i)-sum.txt
		[ -e $f ] || continue
		n=$((n + 1))
		case " $(cat $f) " in
		*" result=PASS "*) np=$((np + 1)) ;;
		*" result=FAIL "*) nf=$((nf + 1)) ;;
		*) no=$((no + 1)) ;;
		esac
		grep -q ' rebuild_canceled=yes ' $f && nc=$((nc + 1))
		grep -q ' findings=0 ' $f || nk=$((nk + 1))
	done
	echo "$t rebuild_reopen: $n of $NR runs: PASS $np, FAIL $nf, other $no; rebuild canceled in $nc; runs with a kernel finding $nk"
	echo "$n $np $nf $no $nc $nk" > $L/count-$t.txt
	f=$L/${p}sc-sum.txt
	if [ -e $f ]; then
		echo "$t scrub_cancel: $(sed -E 's/^.* (result=[A-Za-z]+) .* (rebuild_canceled=[a-z]+) .* (findings=[0-9]+) .* (timeoutdumps=[0-9]+) .*$/\1 \2 \3 \4/' $f)"
	else
		echo "$t scrub_cancel: not run"
	fi
done
set -- $(cat $L/count-B.txt 2>/dev/null)
b="$*"
set -- $(cat $L/count-M.txt 2>/dev/null)
if [ "$b" = "$NR $NR 0 0 0 0" ] && grep -q ' result=PASS ' $L/bsc-sum.txt 2>/dev/null; then
	echo "B: as expected (every run PASS, no rebuild canceled, no kernel finding)"
else
	echo "B: NOT AS EXPECTED"
fi
if [ -n "$1" ] && [ "$1" -gt 0 ] && { [ "$3" -gt 0 ] || [ "$5" -gt 0 ]; }; then
	echo "M: as expected (rebuild_reopen failed at least once, in $3 of $1 runs; a rebuild canceled in $5)"
else
	echo "M: NOT AS EXPECTED, or not run"
fi
} 2>&1 | tee $L/compare.txt
```

Expected:

```
B rebuild_reopen: 5 of 5 runs: PASS 5, FAIL 0, other 0; rebuild canceled in 0; runs with a kernel finding 0
B scrub_cancel: result=PASS rebuild_canceled=no findings=0 timeoutdumps=0
M rebuild_reopen: 5 of 5 runs: PASS <p>, FAIL <f>, other <o>; rebuild canceled in <c>; runs with a kernel finding 0
M scrub_cancel: result=<PASS, FAIL or KILLED> rebuild_canceled=<yes or no> findings=0 timeoutdumps=<0 or 1>
B: as expected (every run PASS, no rebuild canceled, no kernel finding)
M: as expected (rebuild_reopen failed at least once, in <f> of 5 runs; a rebuild canceled in <c>)
```

`NOT AS EXPECTED` is a result, not a stop: "What each result means" says how
to read it. After a wedge, the counts cover the runs that ran.

## STEP 8: the end: unload or not, final checks, report, cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the module and the final checks. With
`$L/WEDGED` present this runs no `zpool` command, unloads nothing, and says
so:

```sh
. /var/log/zts-rbr-1028/env.sh
{
w=""; [ -e $L/WEDGED ] && w=$(cat $L/WEDGED)
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ -n "$cur" ] && [ "$cur" = "$SV_B" ]; then t=B; elif [ -n "$cur" ] && [ "$cur" = "$SV_M" ]; then t=M; else t=""; fi
pk=$(pools_k)
m=$(grep -E ' zfs |testpool' /proc/mounts)
echo "wedged: [$w]; loaded: [${t:-none} $cur]; kstat pools: [$pk]; mounts: [$m]"
if [ -n "$w" ]; then
	echo "unload skipped: wedged; the VM needs a reboot by its owner"
elif [ -n "$t" ] && [ -z "$pk$m" ] && [ ! -e /var/run/zed.pid ] && [ -z "$($W/$t/zpool list -H -o name 2>&1)" ]; then
	(cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # unload M rc=0
elif [ -n "$t" ]; then
	echo "unload skipped: a pool, a mount or /var/run/zed.pid is left"
else
	echo "no module of this task is loaded (srcversion '$cur')"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # lsmod: [] unless wedged
journalctl -k --since "@$(cat $L/start.txt)" --no-pager -q > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KPAT" $L/kern-all.txt | grep -vcE "$KIGN") matches," \
    "$(grep -iE "$KSTRICT" $L/kern-all.txt | grep -vcE "$KIGN") without call trace lines," \
    "$(grep -c "$KTO" $L/kern-all.txt) timeout dumps of test-runner," \
    "$(grep -c 'blocked for more than' $L/kern-all.txt) hung-task lines," \
    "$(grep -c 'ZTS run' $L/kern-all.txt) ZTS marks"
grep -iE "$KPAT" $L/kern-all.txt | grep -vE "$KIGN" | grep -vE 'kernel: +(\? )?[a-z_0-9.]+\+0x' | head -n 40
echo "kernel, this boot: $(journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN")"
echo "D-state tasks: [$(ps -eo pid=,stat=,comm= | awk '$2 ~ /^D/ { printf "%s%s:%s", s, $1, $3; s = " " }')]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "testpool dirs: [$(ls -d /testpool* 2>/dev/null | xargs)]"
echo "loop devices: [$(losetup -a 2>&1 | xargs)]"
echo "mounts under W: [$(grep " $W[ /]" /proc/mounts)]"
echo "files: [$(ls -A $W/files 2>&1 | xargs)]"
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
df -h /var/tmp
} 2>&1 | tee $L/end.txt
```

Expected, normal branch: `wedged: []; loaded: [M <SV_M>]; kstat pools: [];
mounts: []`, `unload M rc=0`, `lsmod: []`, `kernel, whole task: 0 matches, 0
without call trace lines, 0 timeout dumps of test-runner, 0 hung-task lines,
36 ZTS marks` (3 per run, 12 runs), nothing listed, the boot's count equal to
the STEP 0 baseline, `D-state tasks: []`, `zpool.cache` as in STEP 0 or a
0-byte file, `testpool dirs: []`, `loop devices: []`, `mounts under W: []`,
`files: []`, `W holds: B M files repo`. A killed `msc` adds its timeout dump
(1, with its call trace lines, and the matches they make) and leaves its file
vdevs in `files:`; fewer ZTS marks mean fewer runs, after a stop. A run that
was captured adds the lines of its `sysrq w` dump to this count too.

Expected, wedged branch: `wedged: [<label>: wedged after ...]`, `loaded: [<B
or M> <its srcversion>]`, the pools and mounts that are left, `unload
skipped: wedged; ...`, `lsmod: [zfs spl]` (and whatever modules they need),
the kernel lines of the wedge (the frame lines of the stacks are left out
here; the captures hold them), the D-state tasks, perhaps `/testpool1`, the
harness's loop devices and the tests' files. All of it is recorded, none of it
touched.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-rbr-1028/env.sh
{
echo "VM 106: rebuild_reopen in the kernel, B = rebuild-cancel-state-lock 0e7f96966e (the fix), M = master e117a84db with the test only (x/VM-TESTS-106-rbr.md)"
for s in record sha-checks worktrees trees build step3 step3b step4 load-B; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
echo; echo "== env.sh from SV_B"; sed -n '/^SV_B=/,$p' $L/env.sh
echo; echo "== series-B.txt (summary)"; sed -n '/^series /,$p' $L/series-B.txt 2>&1
echo; echo "== swap.txt"; cat $L/swap.txt 2>&1
echo; echo "== series-M.txt (summary)"; sed -n '/^series /,$p' $L/series-M.txt 2>&1
echo; echo "== compare.txt"; cat $L/compare.txt 2>&1
echo; echo "== notes of every run, B then M"
for f in $L/b[0-9][0-9]-notes.txt $L/bsc-notes.txt $L/m[0-9][0-9]-notes.txt $L/msc-notes.txt; do
	[ -e "$f" ] && { echo "-- ${f##*/}"; cat "$f"; }
done
for c in $L/*-capture.txt; do
	[ -e "$c" ] || continue
	l=${c##*/}; l=${l%-capture.txt}
	echo; echo "== capture of $l: the kernel lines of the run that match"
	grep -iE "$KPAT" $L/$l-kernel.txt | grep -vE "$KIGN" | head -n 60
	echo; echo "== capture of $l: the first kernel report, 60 lines"
	grep -m 1 -iE -A 60 "$KSTRICT" $L/$l-kernel.txt | grep -vE "$KIGN" | head -n 61
	echo; echo "== capture of $l: pools, txgs, D-state tasks and their stacks"
	sed -n '1,/^== \/proc\/spl\/kstat\/taskq\/summary$/p' $c
	echo; echo "== capture of $l: sysrq w, 150 lines"
	head -n 150 $L/$l-capture-sysrq.txt
	echo; echo "== capture of $l: dbgmsg, last 40 lines"
	tail -n 40 $L/$l-capture-dbgmsg.txt
	echo; echo "== capture of $l: the last 30 lines of the run's console log"
	tail -n 30 $L/$l.log
done
echo; echo "== guards.txt"; cat $L/guards.txt 2>&1
echo; echo "== WEDGED"; cat $L/WEDGED 2>&1
echo; echo "== end.txt"; cat $L/end.txt 2>&1
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt` after its first line, a
`== STEP 0` section (short: anything found, `uname -r`, `nproc`, the CPU
model, the free space, the `zpool.cache`, `strace`/`killall`/`top`,
`/var/log/journal`, `kernel.*` and doc checkout lines with its sha256, the
directories of earlier tasks, and the kernel baseline), and at its end a
`== verdict` section (the Expected block below, then for each of its items
whether it matched, and which case of "What each result means" applies) and
a `== deviations` section (every deviation from these instructions, with its
reason; `none` if there was none). Then print it with
`cat /var/log/zts-rbr-1028/REPORT.txt`.

**Normal branch.** As the very last command, and only if the first block of
this step printed `unload <B or M> rc=0`, `lsmod: []`, `loop devices: []`
and `mounts under W: []`, remove this task's two directories and the doc
checkout (the doc checkout only if it is still the clean checkout STEP 1
recorded, holding this document, and nothing else):

```sh
cd /
D=/var/tmp/vm-task-106x
if [ -e /var/log/zts-rbr-1028/WEDGED ]; then
	echo "wedged: nothing removed"
elif lsmod | grep -qE '^(zfs|spl) ' || [ -n "$(losetup -a 2>&1)" ] || grep -q " /var/tmp/zts-rbr-1028[ /]" /proc/mounts; then
	echo "a module, a loop device or a mount under /var/tmp/zts-rbr-1028 is left: nothing removed"
else
	if [ -d $D ] && [ ! -L $D ] && [ -d $D/.git ] && [ ! -L $D/.git ] &&
	    [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
	    [ -s /var/log/zts-rbr-1028/doc-head.txt ] &&
	    [ "$(git -C $D rev-parse HEAD 2>/dev/null)" = "$(cat /var/log/zts-rbr-1028/doc-head.txt)" ] &&
	    git -C $D ls-files --error-unmatch x/VM-TESTS-106-rbr.md > /dev/null 2>&1 &&
	    [ "$(sha256sum < $D/x/VM-TESTS-106-rbr.md | cut -d' ' -f1)" = "$(cat /var/log/zts-rbr-1028/doc-sha.txt 2>/dev/null)" ] &&
	    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    ! grep -q " ${D}[ /]" /proc/mounts; then
		X=$D
	else
		X=""; echo "$D kept: not only the clean doc checkout STEP 1 recorded"
	fi
	rm -rf /var/tmp/zts-rbr-1028 /var/log/zts-rbr-1028 $X; echo "rm rc=$?"
fi
ls -d /var/tmp/zts-rbr-1028 /var/log/zts-rbr-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, run nothing of this block: keep everything and
say so. If your permission system refuses this `rm -rf`, leave the
directories as they are and say so in the reply; do not delete them any other
way.

**Wedged branch.** Run nothing more: no cleanup, no unload. The reply then
says, besides `REPORT.txt`: that VM 106 needs a reboot by its owner, the
label of the run that wedged and its D-state tasks; that everything of the
task is kept (`/var/tmp/zts-rbr-1028`, `/var/log/zts-rbr-1028`,
`/var/tmp/vm-task-106x`); whether `/var/log/journal` exists (then
`journalctl -k -b -1` shows the wedged boot's kernel log after the reboot);
that a clean shutdown may stall unmounting `/testpool1`, a reset of the VM
from the hypervisor then completing it; and, for after the reboot, once
`REPORT.txt` has been read, this cleanup (it removes nothing while a zfs
module is loaded):

```sh
if lsmod | grep -qE '^(zfs|spl) ' || [ -e /dev/zfs ]; then
	echo "a zfs module is loaded: nothing removed"
else
	rm -rf /var/tmp/zts-rbr-1028 /var/log/zts-rbr-1028 /var/tmp/vm-task-106x
	rmdir /testpool /testpool1 2>/dev/null
	ls -d /var/tmp/zts-rbr-1028 /var/log/zts-rbr-1028 /var/tmp/vm-task-106x /testpool /testpool1 2>&1
fi
```

Either way, reply with the whole of `REPORT.txt` as printed, and the result
of the `rm` or the wedged note: nothing runs after the reply.

## Expected, as a block for REPORT.txt

```
STEP 0: free; uname -r recorded; at least 20G free on /var/tmp; zts with passwordless sudo; the
  eight commands present; hung_task_panic=0; the doc checkout at /var/tmp/vm-task-106x.
STEP 1-4: every check ok, no MISMATCH; B = 0e7f96966e (two commits on master e117a84db), M =
  e117a84db with B's test files, staged (patch-id 348e97ce...), its vdev_rebuild.c master's
  b159b3aac0; M and B differing in module/zfs/vdev_rebuild.c only (3 insertions, 17 deletions from
  M to B); builds B and M rc=0; ZFS_DEBUG lines 3; compiler warnings 0, about 83 objtool
  warning: lines per tree; vermagic = uname -r; srcversions differ; results in $L/results.
STEP 5 and 6 (each load): the srcversion of the tree, no pool, spl_panic_halt 0, the tunables at
  their defaults; M's tunables as B's.

Every run: in guards.txt, zts_marks=3, kernel=0, findings=0, wedged=no, every leftover empty,
  tunables_same=yes; 3 results (setup, the test, cleanup). "Missing util(s):" may list exportfs;
  it must not list zdb, zinject, zpool or zfs.

b01-b05 (B, rebuild_reopen): PASS each; replaces=6, started=6 complete=6 canceled=0;
  rebuild_canceled=no; end line "Refused scrubs do not cancel a sequential resilver".
bsc (B, scrub_cancel): PASS; end line "Scrub was cancelled by resilver".
m01-m05 (M, rebuild_reopen): FAIL in at least one run, end line "The sequential resilver was
  canceled", rebuild_canceled=yes, at the first rebuild (replaces=1); a run that does not hit the
  bug as on B.
msc (M, scrub_cancel): PASS, FAIL, or KILLED at its 600 s timeout with a timeout dump of
  test-runner (findings=0) and a stop for its leftovers.
No kernel finding anywhere; no wedge.

STEP 8, normal: unload M rc=0; nothing in lsmod; whole task 0 matches (but a killed msc's dump),
  0 hung-task lines, 36 ZTS marks; the boot's count equal to the STEP 0 baseline; nothing left;
  rm rc=0.
STEP 8, wedged: unload skipped; zfs and spl loaded; the wedge's kernel lines; D-state tasks
  listed; everything kept; the VM needs a reboot by its owner.
STOP only on: a failed check or guard, a leftover, a tunable left changed, an outer timeout,
  a kernel VERIFY/ASSERT/panic/BUG/oops/hung task, or a wedge.
```

## What each result means

- **The evidence holds** when B is `as expected` and M is `as expected`
  (STEP 7), with no kernel finding: in a kernel, a sequential resilver on
  master is canceled by the `zpool scrub` commands refused while it runs, and
  `rebuild_reopen` catches it (the end line, and `rebuild_canceled=yes`);
  with the fix, the only difference between the trees, the same test passes
  5 times out of 5 and `scrub_cancel` passes.
- **M never fails** (`m01`-`m05` all `PASS`, `started=6 complete=6
  canceled=0`): the window was not hit in this kernel in 5 runs; the kernel
  A/B is inconclusive for M, said as such, and B's results still stand.
- **M fails without a canceled rebuild** (another end line,
  `rebuild_canceled=no`): not this bug, or not only; the end line, the
  test's last lines and the rebuild events in the run's notes locate it.
  Report it as found.
- **`msc` `KILLED` on M**: test-runner's 600 s timeout, most likely in the
  `zpool wait` that a canceled rebuild never ends (its last lines in the
  notes show where it waited, and its rebuild events whether a `canceled`
  came first); the same bug, a second symptom. `msc` passing is just as
  possible: in userspace it hung in none of 600 plain replays.
- **A finding against the fix**: any run of B that does not pass, a
  `rebuild_canceled=yes` on B, or any kernel finding or wedge on B. The run's
  notes and captures are the result.
- **A kernel finding on M** (a `VERIFY` failure in
  `vdev_rebuild_initiate_sync()` or `vdev_rebuild_reset_sync()`, the second
  commit's case, or any other): stop; the lines and the captures are the
  result, and a wedge is likely (STEP 8, wedged branch).
- **Not the fix's**: a failure in `setup` or `cleanup`, or a `Missing
  util(s)` line naming a tool a test needs: the environment.

Estimated duration: about 1.5 hours (1 to 2.5): checks and sources 10
minutes, the two builds about 30, B's series 10 to 30 (`rebuild_reopen` 1 to
5 minutes a run, `scrub_cancel` 1 to 3), the swap 2, M's series 10 to 30
(a failing `rebuild_reopen` takes less than a passing one; a `KILLED`
`scrub_cancel` about 11 minutes; a wedge up to 23), the report and the
cleanup 10.
