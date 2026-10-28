# VM 107: L2ARC writes with `l2arc_headroom=0`, on master and on the fix

These instructions are for the session on VM 107 (Debian 13). They give
kernel evidence for the fix on the fork branch `l2arc-headroom-zero` of
https://github.com/tigerblue77/zfs, commit `33e2b1f648` "L2ARC: Do not stop
writing when l2arc_headroom is 0", whose parent is upstream master
`5ebd13aa9a` "ZTS: sync checksum test data before fault injection". The
session builds two debug trees in its own directories, runs the new ZTS test
`functional/l2arc/l2arc_headroom_zero_pos` three times on each, then the
whole `functional/l2arc` group once on the fix, cleans up, and replies with
`REPORT.txt`. This document is read from a checkout of the branch
`vm-tasks-1026` of https://github.com/tigerblue77/zfs in
`/var/tmp/vm-task-107w` (the doc checkout, `DOC` below); the last command of
STEP 7 removes it with the task's own directories.

| Tree | Path | Source | Build | Runs |
| --- | --- | --- | --- | --- |
| B | `/var/tmp/zts-hr0-1028/B` | `33e2b1f648`, the fix: `arc.c` and the test-side files | debug (`./configure --enable-debug`) | `b1`, `b2`, `b3` (the test), then `bg` (`-T l2arc`) |
| M | `/var/tmp/zts-hr0-1028/M` | `5ebd13aa9a` (B's parent) with B's four test-side files checked out on it, and nothing else: `module/` is the parent's | debug (`./configure --enable-debug`) | `m1`, `m2`, `m3` (the test) |

M and B differ in `module/zfs/arc.c` only; STEP 2 checks it before the
builds and again after them.

**The bug.** `l2arc_write_buffers()` scans four ARC lists (MFU and MRU
metadata, MFU and MRU data) for buffers to write to a cache device. For each
list it computes `headroom = target_sz * l2arc_headroom` (scaled by
`l2arc_headroom_boost` when the ARC is compressed) and, since 3523b5f3f
"L2ARC: Implement even-depth multi-sublist scanning" (2025-07-03), stops
scanning the list once the headroom it has consumed reaches it, at line 10067
of `arc.c` on master:

```
		while (processed_sublists < num_sublists && !full) {
			if (consumed_headroom >= headroom)
				break;
```

With `l2arc_headroom=0`, `headroom` is 0 and the check holds before the first
sublist: no list is scanned, and nothing is written to any cache device.
`man/man4/zfs.4` on master says of `l2arc_headroom`: "Setting to 0 disables
the per-cycle headroom limit", and `l2arc_write_sublist()` still reads it so
(line 9720: `if (l2arc_headroom != 0 && ...`). The check 3523b5f3f replaced
was `if (l2arc_headroom != 0 && passed_sz > headroom)`.

**The fix** (B) skips the check when `l2arc_headroom` is 0, as the old one
did:

```
			if (l2arc_headroom != 0 &&
			    consumed_headroom >= headroom)
				break;
```

With any other value the condition is master's, so nothing else changes. No
other ZTS test sets `l2arc_headroom`: B adds its `tunables.cfg` entry.

**The test** (new in B): it sets `l2arc_headroom=0` and
`l2arc_noprefetch=0`, creates `testpool` on `$VDEV` with a 1120 MiB cache file,
writes 800 MB with fio (`mkfiles.fio`, 4 jobs), reads it at random for 10 s
(`random_reads.fio`), waits with `arcstat_quiescence_noecho l2_size`, logs
`NOTE: l2_size=<n>` and requires it above 0. Its `log_onexit` cleanup
destroys the pool and restores both tunables.

**What M does with it: the test is killed, it does not fail.**
`arcstat_quiescence` in `libtest.shlib` waits for a *non-zero* quiescence:

```
	while $do_once || [ $stat1 -ne $stat2 ] || [ $stat2 -eq 0 ]; do
```

On M, `l2_size` stays 0, so the wait never ends: the test never logs its
`l2_size` note and never reaches its check. The runner kills it at its 600 s
timeout (`KILLED`, at about `[10:00]`). No trap is set, so its own cleanup
does not run: the directory's `cleanup.ksh` destroys the pool after it, and
`l2arc_headroom=0` and `l2arc_noprefetch=0` stay set until the next module
load. Every run here starts with its own load (`scripts/zfs.sh -r`), so no
run inherits them, and the L2ARC counters read from arcstats after a run are
that run's alone: `l2_write_bytes` and `l2_writes_sent` 0 on M while
`l2_feeds` counts hundreds of feeds (the feed thread ran every second with a
cache device and found nothing to write), hundreds of MB written on B. That
counter is this task's kernel-side measure of what M wrote; B's runs also
log `l2_size` themselves. (`l2_size` read after a run is 0 on both trees: the
pool is gone by then.)

**What the runner writes to the kernel log when it kills a test.** Before it
sends the kill, `kill_cmd()` in `test-runner.py` writes a debug dump to the
kernel log whenever `/dev/kmsg` exists, `-O` or not (`-O` sets an option
nothing reads): a `##### ZTS timeout debug #####` record with `top`'s output
(when it fits in one kernel record), then `echo l > /proc/sysrq-trigger`,
which prints `sysrq: Show backtrace of all active CPUs` and an NMI backtrace,
with `Call Trace:` lines, of every CPU that is not idle. So each M run puts
one such dump in the kernel log, and its `Call Trace:` lines would match the
findings pattern. The kernel checks of this task therefore set every dump
apart: a dump runs from its first line (`ZTS timeout debug` or `Show
backtrace of all active CPUs`) to the next `ZTS run` mark, which is the
directory's `cleanup` starting right after the kill. Findings are counted
outside dumps only; the dumps are counted, with their lines and those of them
the pattern matches, and kept in the run's kernel file. A run must hold
exactly one dump per test it killed: one more is a stop.

**The disks.** No `DISKS` is given, as in the earlier l2arc runs on this VM:
`zfs-tests.sh` makes its three sparse 4 GiB files `file-vdev0` to
`file-vdev2` in the `-d` directory, layers loop devices on them (they are the
run's `DISKS`), and removes both at its exit. The l2arc tests do not use them:
`l2arc.cfg` puts their pool and cache devices in files under
`$TESTDIR/disk.l2arc` (`$TESTDIR` is `testdir` in the `-d` directory), which
the group's `setup.ksh` makes (`a` and `d`, 1 GiB each, written in full by
`mkfile`), and each test truncates its sparse cache file `b` (and `c`) beside
them. `zfs-tests.sh -t` runs the directory's `setup` and `cleanup` around the
test: three results per run. Master's `l2arc/cleanup.ksh` removes only
`$VDIR`, so each run, on either tree, leaves an empty, root-owned `testdir`
in its `-d` directory (the leak an earlier task on this VM was about); STEP 7
releases them.
Each run has its own `-d` directory, so that what it leaves is its own.

**Disk space.** A built tree takes about 1 GB here, the repository a few
hundred MB. A single run writes about 3 GB at its peak (`a`, `d`, the cache
file); the group about 4.5 GB (one test fills `a` up to 1.5 GB more). STEP 0
stops below 8 GB free on `/var/tmp`, STEP 2 below 5 GB after the builds, and
the group does not start below 5 GB.

## Rules

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order: `zfs-tests.sh` refuses to run as root.
  Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  the module loads and every run go through `nice -n 10 ionice -c3` too (the
  helpers do it), one thing at a time, nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing once STEP 2 has built the
  trees. `scripts/zfs.sh -r` (load) and `-u` (unload) run only here, on this
  VM, and only in the helpers and blocks that say so.
- This task's own directories are `/var/tmp/zts-hr0-1028` (`$W`) and
  `/var/log/zts-hr0-1028` (`$L`). Outside them, the only changes are those the
  tests and the runner make and undo themselves (module tunables, loop
  devices, the `/testpool` mount point, `/etc/zfs/zpool.cache`, which the
  kernel writes, temporary files in `/tmp`, and the runner's timeout dump
  through `/proc/sysrq-trigger`, above) and STEP 7's removal of the doc
  checkout of this task.
  `/var/tmp/vm-task-107v`, the doc checkout of the last task, is already gone:
  STEP 0 only records it. Never touch another `/var/tmp/zts-*` or
  `/var/log/zts-*` directory, another `/var/tmp/vm-task-*` checkout or
  `/var/tmp/test_results`. Never create, edit or remove
  `/etc/zfs/zpool.cache` or `/etc/zfs/vdev_id.conf` yourself.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only what STEP 7 removes: the `testdir`
  each run leaves, the seven `-d` directories, and the final removals.
- Never run a `zpool`, `zfs` or `zdb` command of a tree other than the one
  whose module is loaded, or while none is. Never print the environment
  (`env`, `printenv`, `set`, `/proc/*/environ`).
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-hr0-1028/env.sh`.
- Stop at any value the comments or Expected lines do not allow, unless the
  text says it is recorded only. After a line ending in `stop`, run only
  STEP 7; the helpers refuse to run once a step has stopped. A test that
  fails, is killed or is skipped is a result, recorded and compared, not a
  stop; so are the lines that describe it (its digest, its arcstats, its
  runner time, wall time and rc): only a line ending in `stop`, a `LEFTOVER`
  line, a `MISMATCH` line or a kernel finding stops. Clean, repair and retry
  nothing by hand, and run no test more often than written. If your
  permission system refuses a command, report it and stop.
- **A kernel finding** (panic, oops, VERIFY, ASSERT, BUG, call trace, hung
  task, outside the runner's timeout dumps set apart as described above)
  stops at once, and nothing touches the pool or the module after it:
  STEP 7 then runs no `zpool` command and leaves the module loaded. If a
  block hangs well past its stated time, interrupt it if your tool allows,
  run `touch /var/log/zts-hr0-1028/HELD`, then only STEP 7's first block
  (with `HELD` set it runs no pool command), and report. Never reboot.
- The builds take about 10 to 15 minutes together, a B run 1 to 3 minutes,
  an M run 11 to 13 (the test waits until the runner kills it at 600 s), the
  group 10 to 25. Each run has an outer `timeout` of 1800 s. If your shell
  tool cannot wait that long in the foreground, start that one block with the
  tool's own background mode and wait for it to exit. Never use `&` or
  `nohup`, and run nothing meanwhile.
- Your session cannot run anything after its final reply. Everything, cleanup
  included, happens before it; the reply is the whole of `REPORT.txt` and the
  result of the last command.

What earlier VM runs taught, and the blocks already handle: `modinfo` is in
`/usr/sbin`, not in `tigerblue77`'s PATH, so every block puts `/usr/sbin:/sbin`
first; this VM's umask has been 0002 and 0022, and `env.sh` sets 022; `/var/log`
directories are made with `sudo -n mkdir` and then given to `tigerblue77`.
`configure --enable-debug` writes 3 `#define ZFS_DEBUG 1` lines in
`zfs_config.h` and no `/* #undef ZFS_DEBUG */`. Compiler warnings are counted
as `:[0-9]+:[0-9]+: warning:`, and objtool's about 83 harmless `warning:`
lines per tree are not counted. Results are matched anywhere in a `Test:`
line, which starts with a timestamp; a `-t` run names the test with `.ksh`, a
`-T` run without it; `linux.run` tests are `Test (Linux): ...` (none here). A
literal parenthesis in a regex is `[(]`. The tests' lines are read from the
runner's own `log` file. Kernel messages are read per run from
`journalctl -k --since`, without the `ZTS run` markers of `-K` and the
`spa_import_progress_set_notes_impl` lines. Each run waits 2 s first: two
runs started in one second would share a results directory. A `-d` directory
has mode 1777. The l2arc tests need `fio`; none uses a zvol or `zed`.
`zfs.sh -r` and `-u` signal the pid `/var/run/zed.pid` names, so they never run
while it exists. `zpool list -H` prints nothing when there is no pool. A
block's own exit status is not a result (one ending in a `grep -q ... &&
stop` that finds nothing exits 1): its printed lines are. The doc
checkout may be a clone of `vm-tasks-1026` or a `git init` with a fetch by
commit (a detached HEAD and no `origin`): STEP 0 accepts both shapes, and so
does the final removal.

## STEP 0: the VM free, the space, the previous doc checkout gone

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-hr0-1028 L=/var/log/zts-hr0-1028 DOC=/var/tmp/vm-task-107w
date -u '+%F %T'
id -un                                # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                              # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release      # Debian GNU/Linux 13 (trixie)
nproc; umask                          # 16, then 0002 or 0022
free -m | head -n 2                   # recorded
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '          # nothing
ls /sys/module | grep -xE 'zfs|spl'    # nothing
ls -l /dev/zfs 2>&1                    # No such file or directory
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db|[z]inject|[f]io '   # nothing
pgrep -ax 'zed|lt-zed'                 # nothing
ls -l /var/run/zed.pid 2>&1            # No such file or directory: zfs.sh -r and -u signal the pid it names
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                # No devices found
grep -E ' zfs |testpool' /proc/mounts  # nothing
ls -d /testpool* 2>&1                  # No such file or directory
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
ls -d $W $L 2>&1                       # No such file or directory, twice
ls -d /var/tmp/vm-task-107v 2>&1       # No such file or directory: removed by the last task
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
echo "/var/log zts dirs: $(ls -d /var/log/zts-* 2>/dev/null | xargs)"
df -h /var/tmp /var/log
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
sudo -n journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
for c in git ksh python3 modinfo sha256sum timeout fio truncate losetup; do echo "$c: $(command -v $c || echo absent)"; done
python3 --version; fio --version
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/w/VM-TESTS-107-hr0.md 2>&1
```

Keep this output: it goes into `REPORT.txt` by hand. The number printed
after the one kernel line is the kernel baseline. The `/var/tmp`, `/var/log`
and `free` lines are recorded only: earlier tasks' directories there are left
alone, `/var/tmp/vm-task-107v` included if it is there after all. The doc
checkout line is expected as `doc checkout: top /var/tmp/vm-task-107w, branch
vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`) for a clone, or
`branch HEAD, head <a commit of vm-tasks-1026>, origin error: No such remote
'origin'` for a checkout fetched by commit; both are fine. If you read this
document from another place, say so: STEP 7 then leaves that place alone.

Stop at once, changing nothing, and report what you found if: the user is not
`tigerblue77` or `sudo -n` fails; the kernel headers are missing; a zfs or
spl module is loaded or `/dev/zfs` exists; a test, build, fio, ksh, zdb or
zinject process or any `zed` runs (ignore the line of the shell running this
block, if `pgrep` shows it), or `/var/run/zed.pid` exists; a tmux pane other
than this session's runs anything but an idle shell; a loop or device-mapper
device, a zfs or test mount, a `/testpool*` directory, a non-empty
`zpool.cache` or a `vdev_id.conf` exists; `$W` or `$L` exists (this task
started once already); `journalctl -k` prints nothing; `git`, `ksh`,
`python3`, `modinfo`, `sha256sum`, `timeout`, `fio`, `truncate` or `losetup` is
absent (without `fio` the l2arc tests are skipped); or less than `8G` is free
on `/var/tmp` (two trees, the repository and the group's files: about 7 GB at
the peak). A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by
itself: STEP 2 checks that it is within the range each tree supports, and
that the modules were built for it.

## STEP 1: this task's directories and helpers

The block writes nothing unless it can create both directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/tmp/zts-hr0-1028 && sudo -n mkdir /var/log/zts-hr0-1028 &&
    sudo -n chown tigerblue77: /var/log/zts-hr0-1028; then
echo "mkdir rc=0"
cat > /var/log/zts-hr0-1028/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-hr0-1028
L=/var/log/zts-hr0-1028
DOC=/var/tmp/vm-task-107w
DOCFILE=w/VM-TESTS-107-hr0.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
FIXBRANCH=l2arc-headroom-zero
# B: the fix, the head of the fork branch l2arc-headroom-zero. HP: its
# parent, upstream master; M is HP with B's test-side files.
HB=33e2b1f648c8ad086ac8b33059ae931706039921
HP=5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53
ARC=module/zfs/arc.c
Z=tests/zfs-tests
# B's test-side files, in git's order, and their blobs in B in that order.
TFILES="tests/runfiles/common.run $Z/include/tunables.cfg $Z/tests/Makefile.am
$Z/tests/functional/l2arc/l2arc_headroom_zero_pos.ksh"
TFILES=$(echo $TFILES)
TBLOBS="9c6a79239af28e800e6e6ec1f9da18f38f9ddc9d 5140f9a273b4aea2b60d1aaeb38e7d716fed8861 ec5fc93e59bb5a53f63eea0c89d75f535dd4fb6f 7c6d7c5542432729bceeb9fd024797c346c96baa"
# What B changes against HP: all of it (NUMB) and its test-side files
# (NUMT), and the patch-ids of both.
NUMB="2 1 $ARC 2 1 tests/runfiles/common.run 1 0 $Z/include/tunables.cfg 1 0 $Z/tests/Makefile.am 72 0 $Z/tests/functional/l2arc/l2arc_headroom_zero_pos.ksh"
NUMT="2 1 tests/runfiles/common.run 1 0 $Z/include/tunables.cfg 1 0 $Z/tests/Makefile.am 72 0 $Z/tests/functional/l2arc/l2arc_headroom_zero_pos.ksh"
PIDB=4614f88bb49b976e7ff35e66fca18039b30d1202
PIDT=8c9ea2af0b9fc4208dc2ca6c731e4c4e9eb8e044
# arc.c's blob in HP (and so in M) and in B; the guard B adds back, and the
# check it guards as master has it.
ARCP=15565d119261a6bcd741b4b16b4516d8a02a27c5
ARCB=1fe44ed3bc7ec2a8a3b46bcf59430c8caf09c542
SGUARD='if (l2arc_headroom != 0 &&'
SBARE='if (consumed_headroom >= headroom)'
# An assertion text every debug build of these trees has in zfs.ko.
SDBG='VERIFY3P(to_write, !=, hdr->b_l1hdr.b_pabd)'
# The test as a file of the tree (F), as a zfs-tests.sh -t argument (R) and
# by name (T); the runner.
F=$Z/tests/functional/l2arc/l2arc_headroom_zero_pos.ksh
R=tests/functional/l2arc/l2arc_headroom_zero_pos.ksh
T=l2arc_headroom_zero_pos
TR=tests/test-runner/bin/test-runner.py
# Tunables recorded at each load and compared after each run: the two the
# test sets, and those the other tests of the group change and restore.
TUN="l2arc_headroom l2arc_noprefetch l2arc_headroom_boost l2arc_write_max
l2arc_feed_secs l2arc_mfuonly l2arc_dwpd_limit l2arc_ext_headroom_pct
l2arc_rebuild_enabled l2arc_rebuild_blocks_min_l2size zfs_arc_max
zfs_arc_min zfs_prefetch_disable zfs_compressed_arc_enabled"
TUN=$(echo $TUN)
# The arcstats kept after each run. Each run has its own module load, so
# the counters are that run's.
AS="l2_feeds l2_writes_sent l2_writes_done l2_writes_error l2_write_bytes
l2_size l2_asize l2_hits l2_misses l2_abort_lowmem l2_log_blk_writes"
AS=$(echo $AS)
# Kernel findings; KIGN drops the -K markers and the import progress notes.
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# A result line of the runner, "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a test of linux.run.
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: [$2], expected [$3]"
	fi
}

# A step that stops records why in $L/STOP; nothing runs after it.
stop() {   # stop <reason>
	echo "$*: stop" | tee -a $L/runs.txt
	[ -e $L/STOP ] || echo "$*" > $L/STOP
	return 1
}
stopped() {
	[ -e $L/STOP ] || return 1
	echo "stopped earlier ($(cat $L/STOP)): nothing run" | tee -a $L/runs.txt
}

say() {   # say <line>: on the screen and in $L/runs.txt
	echo "$*" | tee -a $L/runs.txt
}

sv_of() {   # sv_of <B|M>: the srcversion STEP 2 recorded for that tree
	case $1 in
	B) echo "$SV_B" ;;
	M) echo "$SV_M" ;;
	esac
}

tunables() {
	local t
	for t in $TUN; do
		if [ -e /sys/module/zfs/parameters/$t ]; then
			printf '%s=%s ' $t "$(cat /sys/module/zfs/parameters/$t)"
		else
			printf '%s=absent ' $t
		fi
	done
}

# A tunables line with the two tunables the test sets as it sets them: what
# a killed test leaves, its own cleanup not having run.
killed_tun() {   # killed_tun <tunables line>
	echo "$1" | sed -E 's/(^| )l2arc_headroom=[^ ]* /\1l2arc_headroom=0 /; s/(^| )l2arc_noprefetch=[^ ]* /\1l2arc_noprefetch=0 /'
}

loops() {
	sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1
}

zc() {   # zc <tree> <command> <args>: that tree's command, as root, niced
	local t=$1 c=$2
	shift 2
	nice -n 10 ionice -c3 sudo -n $W/$t/$c "$@"
}

# Build <tree> in place: autogen, configure with <options>, make -j8, niced.
build_tree() {   # build_tree <B|M> [configure options]
	local t=$1 t0 rc
	shift
	stopped && return 1
	t0=$(date +%s)
	(cd $W/$t && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure $* && make -j8") > $L/build-$t.log 2>&1
	rc=$?
	echo "build $t (configure $*) rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "free=$(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -m 10 -nE ': error:|\*\*\* \[' $L/build-$t.log | tee -a $L/build.txt
		stop "the build of $t"
	fi
}

# The runner's timeout dumps in a journal file: a dump runs from a line
# "ZTS timeout debug" or "Show backtrace of all active CPUs" to the next
# "ZTS run" mark. kout prints the lines outside dumps; kdumps prints
# "<dumps> <their lines> <those of them the findings pattern matches>".
kout() {   # kout <journal file>
	awk '
	/ZTS run / { d = 0 }
	!d && /ZTS timeout debug|Show backtrace of all active CPUs/ { d = 1 }
	!d' "$1"
}
kdumps() {   # kdumps <journal file>
	awk '
	/ZTS run / { d = 0 }
	!d && /ZTS timeout debug|Show backtrace of all active CPUs/ { d = 1; n++ }
	d { l++; if (tolower($0) ~ /verify|assert|panic|bug:|oops|call trace|blocked for more than/) c++ }
	END { print n + 0, l + 0, c + 0 }' "$1"
}

# Kernel messages since <since>, from the journal. A finding outside the
# runner's timeout dumps stops and sets PANIC: nothing touches the pool or
# the module after it. More dumps than killed tests stops too.
kern_check() {   # kern_check <label> <since> <expected ZTS marks> <killed tests>
	local n m k kl kc
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $L/kern-$1.txt 2>&1
	n=$(kout $L/kern-$1.txt | grep -iE "$KRE" | grep -vcE "$KIGN")
	m=$(grep -c 'ZTS run ' $L/kern-$1.txt)
	read -r k kl kc <<< "$(kdumps $L/kern-$1.txt)"
	say "kernel $1: $n findings, $m ZTS marks (expected $3), $k timeout dumps (expected $4; $kl lines, $kc of them matching, set apart) ($(wc -l < $L/kern-$1.txt) lines)"
	kout $L/kern-$1.txt | grep -iE "$KRE" | grep -vE "$KIGN" | head -n 20 | sed 's/^/  /' | tee -a $L/runs.txt
	if [ "$n" -ne 0 ]; then
		touch $L/PANIC
		stop "$1: kernel finding"
		return 1
	fi
	if [ "$3" -gt 0 ] && [ "$m" -eq 0 ]; then
		stop "$1: no ZTS mark in the journal, so its kernel check shows nothing"
		return 1
	fi
	if [ "$k" -gt "$4" ]; then
		stop "$1: a timeout dump with no killed test (its lines are in $L/kern-$1.txt)"
		return 1
	fi
}
kfind() {   # kfind <label>: the findings kern_check counted, outside dumps
	if [ -f $L/kern-$1.txt ]; then
		kout $L/kern-$1.txt | grep -iE "$KRE" | grep -vcE "$KIGN"
	else
		echo '?'
	fi
}

# Load <tree>'s module stack with its own scripts/zfs.sh -r, which first
# unloads the one loaded. Only with nothing stopped, no zed and no pool or
# zfs mount left; then the srcversion must be <tree>'s and no pool may
# appear. The tunables found are kept in $L/tun-<label>.txt.
switch_to() {   # switch_to <B|M> <label>
	local t=$1 lb=$2 sv cur prev p since rc
	stopped && return 1
	sv=$(sv_of $t)
	if [ -z "$sv" ]; then
		stop "$lb: no srcversion recorded for $t"
		return 1
	fi
	if [ -e /var/run/zed.pid ] || [ -n "$(pgrep -x 'zed|lt-zed')" ]; then
		stop "$lb: zed runs or /var/run/zed.pid exists, nothing loaded"
		return 1
	fi
	prev=$(cat $L/loaded 2>/dev/null)
	if [ -n "$prev" ]; then
		p=$(zc $prev zpool list -H -o name 2>&1)
		if [ -n "$p$(grep -E ' zfs |testpool' /proc/mounts)" ]; then
			stop "$lb: a pool or a zfs mount is left ($p), nothing reloaded"
			return 1
		fi
	fi
	since=$(date '+%F %T')
	(cd $W/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -r) > $L/load-$lb.log 2>&1
	rc=$?
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	rm -f $L/loaded
	if [ $rc -ne 0 ] || [ "$cur" != "$sv" ]; then
		say "$lb: load $t rc=$rc, srcversion [$cur], expected $sv"
		head -n 20 $L/load-$lb.log | sed 's/^/  /' | tee -a $L/runs.txt
		touch $L/HELD
		stop "$lb: the load of $t"
		return 1
	fi
	echo $t > $L/loaded
	p=$(zc $t zpool list -H -o name 2>&1)
	tunables > $L/tun-$lb.txt
	say "$lb: load $t rc=0, srcversion $cur, pools [$p]"
	say "  tunables: $(cat $L/tun-$lb.txt)"
	if [ -n "$p" ]; then
		stop "$lb: a pool appeared at the load of $t"
		return 1
	fi
	kern_check ld-$lb "$since" 0 0
}

# Test processes still running, leaving out this shell, its forks and its
# ancestors: their command lines can carry the words of the block.
others() {
	local skip me p rest q=$$
	skip=" $$ "
	while [ -n "$q" ] && [ "$q" -gt 1 ]; do
		q=$(awk '/^PPid:/ { print $2 }' /proc/$q/status 2>/dev/null)
		skip="$skip$q "
	done
	me=$(tr '\0' ' ' < /proc/$$/cmdline)
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[f]io |[z]inject ' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Result counts of runner result lines on stdin: "PASS x3", "KILLED x1, PASS x2".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# The first ERROR line and the last line of test <name> in a runner log.
fails() {   # fails <runner log> <name>
	awk -v n="$2" '
	function flush() {
		if (sec)
			printf "  %s: first ERROR line: %s\n    last line: %s\n", n, (err == "" ? "none" : err), prev
		sec = 0
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		err = ""; prev = ""
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	err == "" && line ~ /ERROR: / { err = line }
	{ prev = line }
	END { flush() }' "$1"
}

# The new test's lines in a runner log: l2arc_headroom set to 0, the wait
# for l2_size, the l2_size note(s), log_pass's message, its cleanup and the
# restore of l2arc_headroom, ERROR lines, and its last line.
digest() {   # digest <runner log>
	awk -v n="$T" '
	function flush() {
		if (sec)
			printf "  %s: headroom set to 0 x%d, waited for l2_size x%d, l2_size note [%s], log_pass message x%d, local cleanup x%d, headroom restored x%d, ERROR lines x%d\n    last line: %s\n", n, h0, wt, sz, pas, cln, hr, err, prev
		sec = 0
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		h0 = 0; wt = 0; sz = ""; pas = 0; cln = 0; hr = 0; err = 0; prev = ""
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	line ~ /^SUCCESS: set_tunable64 L2ARC_HEADROOM 0$/ { h0++ }
	line ~ /^Waiting for arcstat l2_size quiescence[.]$/ { wt++ }
	line ~ /^NOTE: l2_size=[0-9]+$/ { v = line; sub(/^NOTE: l2_size=/, "", v); sz = sz (sz == "" ? "" : ",") v }
	line ~ /^l2arc_headroom=0 does not stop L2ARC writes[.]$/ { pas++ }
	line ~ /^NOTE: Performing local cleanup via log_onexit [(]cleanup[)]$/ { cln++ }
	line ~ /^SUCCESS: set_tunable64 L2ARC_HEADROOM [1-9][0-9]*$/ { hr++ }
	line ~ /ERROR: / { err++ }
	{ prev = line }
	END { flush() }' "$1"
}

# The arcstats after a run: all of them in <label>.arcstats, the fields of
# AS as "<name> <value>" lines in <label>.as.
snap() {   # snap <label>
	sudo -n cat /proc/spl/kstat/zfs/arcstats > $L/$1.arcstats 2>&1
	awk -v l="$AS" '
	BEGIN { n = split(l, a, " "); for (i = 1; i <= n; i++) w[a[i]] = 1 }
	($1 in w) { print $1, $3 }' $L/$1.arcstats > $L/$1.as
}
val() {   # val <label> <name>
	awk -v k=$2 '$1 == k { print $2 }' $L/$1.as 2>/dev/null
}
asline() {   # asline <label>: "<name> <value>, ..." in AS's order
	local k
	for k in $AS; do
		printf '%s %s, ' $k "$(val $1 $k)"
	done | sed 's/, $//'
}
# The last five fields of a run's row: l2_write_bytes, l2_writes_sent,
# l2_feeds, kernel findings, timeout dumps; "?" for a value not read.
row_tail() {   # row_tail <label>
	local wb ws fe k
	wb=$(val $1 l2_write_bytes)
	ws=$(val $1 l2_writes_sent)
	fe=$(val $1 l2_feeds)
	k=$(kdumps $L/kern-$1.txt 2>/dev/null | cut -d' ' -f1)
	echo "${wb:-?} ${ws:-?} ${fe:-?} $(kfind $1) ${k:-?}"
}

# What <fd> holds: "" (nothing), "testdir" when it holds exactly an empty
# directory testdir owned by root and not a mount point, or "OTHER: ...".
fd_state() {   # fd_state <fd>
	local f
	f=$(LC_ALL=C ls -A $1 | xargs)
	if [ -z "$f" ]; then
		echo ""
	elif [ "$f" = testdir ] && [ ! -L $1/testdir ] &&
	    [ "$(stat -c '%F %U' $1/testdir)" = "directory root" ] &&
	    [ -z "$(sudo -n ls -A $1/testdir)" ] && ! grep -q " $1/testdir " /proc/mounts; then
		echo testdir
	else
		echo "OTHER: $f"
	fi
}

# Before a run: nothing stopped, a new label, <tree>'s module, an empty
# files directory of mode 1777, and none of what a run can leave.
pre_run() {   # pre_run <label> <fd> <tree>
	local cur p l m r z o d t
	stopped && return 1
	if [ -e $L/$1.log ]; then
		echo "label $1 already used: nothing run" | tee -a $L/runs.txt
		return 1
	fi
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$(cat $L/loaded 2>/dev/null)" != $3 ] || [ "$cur" != "$(sv_of $3)" ]; then
		stop "$1: the loaded module ($cur) is not $3's"
		return 1
	fi
	p=$(zc $3 zpool list -H -o name 2>&1)
	l=$(loops | xargs)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	r=$(ls -d /testpool* 2>/dev/null | xargs)
	z=$(pgrep -ax 'zed|lt-zed')
	o=$(others)
	d=$(sudo -n dmsetup ls 2>&1)
	t="tunables as loaded"
	[ "$(tunables)" = "$(cat $L/tun-$1.txt)" ] || t="TUNABLES CHANGED: $(tunables)"
	say "before $1: fd=$2 ($(stat -c '%U %a' $2), holds [$(LC_ALL=C ls -A $2 | xargs)]) pools=[$p] loops=[$l] mounts=[$m] rootdirs=[$r] zed=[$z] others=[$o] $t"
	if [ -n "$(LC_ALL=C ls -A $2)" ] || [ "$(stat -c %a $2)" != 1777 ] ||
	    [ -n "$p$l$m$r$z$o" ] || [ "$d" != "$(cat $L/dm-before.txt)" ] ||
	    [ "$t" != "tunables as loaded" ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: in its files directory exactly the empty testdir master's
# l2arc cleanup leaves, the tunables <expected>, and nothing else anywhere.
# A pool, device, mount or process left may hold the module (HELD: STEP 7
# does not unload).
leftovers() {   # leftovers <label> <fd> <expected tunables>
	local bad=0 held=0 p s
	{
	p=$(zc $(cat $L/loaded) zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; held=1; }
	p=$(sudo -n dmsetup ls 2>&1)
	[ "$p" = "$(cat $L/dm-before.txt)" ] || { echo "  LEFTOVER dm devices: $p"; held=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; held=1; }
	p=$(ls -d /testpool* 2>/dev/null | xargs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; held=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; held=1; }
	p=$(tunables)
	if [ "$p" = "$(cat $L/tun-$1.txt)" ] && [ "$p" = "$3" ]; then
		echo "  tunables: as loaded"
	elif [ "$p" = "$3" ]; then
		echo "  tunables: l2arc_headroom=0 l2arc_noprefetch=0, as the killed test left them, the others as loaded (the next load resets them)"
	else
		echo "  LEFTOVER tunables: $p"; bad=1
	fi
	p=$(loops)
	echo "  loop devices: [$(printf '%s' "$p" | tr '\n' ';')]"
	[ -n "$p" ] && held=1
	s=$(fd_state $2)
	echo "  $2 holds: [$(LC_ALL=C ls -A $2 | xargs)] (expected [testdir])"
	if [ -n "$(LC_ALL=C ls -A $2)" ]; then
		sudo -n ls -la --time-style=+%T $2 $2/* 2>&1 | sed 's/^/    /'
	fi
	if [ "$s" != testdir ]; then
		echo "  LEFTOVER in $2: [$s], expected [testdir]"; bad=1
	fi
	if [ $bad$held = 00 ]; then
		echo "leftovers after $1: as expected"
	else
		echo "leftovers after $1: NOT AS EXPECTED"
	fi
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	[ $bad$held = 00 ] && return 0
	[ $held = 1 ] && touch $L/HELD
	stop "$1: leftovers"
}

# One run of the test on <tree>: a fresh load of its modules, zfs-tests.sh
# -t (the directory's setup, the test, its cleanup) into its own files
# directory, then its results, the test's lines, the arcstats, the kernel
# and what it left. One summary row in $L/row-<label>.txt: label, tree,
# result, runner time, rc, wall seconds, l2_size note, l2_write_bytes,
# l2_writes_sent, l2_feeds, kernel findings, timeout dumps. A test result is
# recorded, not a stop.
one() {   # one <label> <B|M>
	local lb=$1 t=$2 fd=$W/files-$1 n0 since t0 rc wall res rw rt np d dg note xt nk bad=0
	n0=$(wc -l < $L/runs.txt)
	switch_to $t $lb || return 1
	pre_run $lb $fd $t || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/$t && timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $fd -t $R) > $L/$lb.log 2>&1
	rc=$?
	wall=$(( $(date +%s) - t0 ))
	snap $lb
	grep -E "$TRE" $L/$lb.log > $L/$lb-tests.txt
	res=$(grep -E "/$T([.]ksh)? [(]run as " $L/$lb-tests.txt | tally)
	rw=$(grep -E "/$T([.]ksh)? [(]run as " $L/$lb-tests.txt | sed -E 's/^.*\] \[([A-Z]+)\].*$/\1/' | xargs | tr ' ' ,)
	rt=$(grep -E "/$T([.]ksh)? [(]run as " $L/$lb-tests.txt | sed -E 's/^.* \[([0-9:]+)\] \[[A-Z]+\].*$/\1/' | xargs | tr ' ' ,)
	say "$lb ($t) $T ${res:-NO RESULT} [${rt:-none}] rc=$rc wall=${wall}s; all results: $(tally < $L/$lb-tests.txt)"
	np=$(grep -vE "/$T([.]ksh)? [(]run as " $L/$lb-tests.txt | grep -v '\[PASS\]')
	[ -z "$np" ] || printf '%s\n' "$np" | sed 's/^/  not passed: /' | tee -a $L/runs.txt
	d=$(awk '/^Log directory/ { print $3 }' $L/$lb.log | head -n 1)
	if [ -f "$d/log" ]; then
		dg=$(digest "$d/log")
	else
		dg="  no runner log (Log directory: '$d')"
	fi
	printf '%s\n' "$dg" | tee -a $L/runs.txt
	if [ "$res" != "PASS x1" ] && [ -f "$d/log" ]; then
		fails "$d/log" $T | tee -a $L/runs.txt
	fi
	say "$lb arcstats since its load: $(asline $lb)"
	nk=$(grep -c '\[KILLED\]' $L/$lb-tests.txt)
	kern_check $lb "$since" 3 $nk || bad=1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		stop "$lb: outer timeout"
		bad=1
	fi
	xt=$(cat $L/tun-$lb.txt)
	[ "$res" = "KILLED x1" ] && xt=$(killed_tun "$xt")
	leftovers $lb $fd "$xt" || bad=1
	note=$(printf '%s\n' "$dg" | sed -nE 's/^.* l2_size note \[([^]]*)\].*$/\1/p' | head -n 1)
	echo "$lb $t ${rw:-NONE} ${rt:-none} $rc $wall ${note:--} $(row_tail $lb)" > $L/row-$lb.txt
	tail -n +$((n0 + 1)) $L/runs.txt > $L/sect-$lb.txt
	return $bad
}

# The l2arc group once on B (-T l2arc: setup, 13 tests, cleanup), the same
# way, with its own load and files directory. Its row has the group's
# result counts in place of the test's result, and the new test's l2_size
# note.
group() {   # group <label>
	local lb=$1 fd=$W/files-$1 n0 since t0 rc wall c nt nl np d dg n note bad=0
	n0=$(wc -l < $L/runs.txt)
	stopped && return 1
	if [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 5 ]; then
		stop "$lb: less than 5G free on /var/tmp, the group not run"
		return 1
	fi
	switch_to B $lb || return 1
	pre_run $lb $fd B || return 1
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/B && timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $fd -T l2arc) > $L/$lb.log 2>&1
	rc=$?
	wall=$(( $(date +%s) - t0 ))
	snap $lb
	grep -E "$TRE" $L/$lb.log > $L/$lb-tests.txt
	c=$(tally < $L/$lb-tests.txt)
	nt=$(grep -c 'Test: ' $L/$lb-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $L/$lb-tests.txt)
	say "$lb (B) -T l2arc ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl) rc=$rc wall=${wall}s"
	grep -m 1 '^Missing util' $L/$lb.log | sed 's/^/  /' | tee -a $L/runs.txt
	grep -v '\[PASS\]' $L/$lb-tests.txt | sed 's/^/  not passed: /' | tee -a $L/runs.txt
	np=$(grep -v '\[PASS\]' $L/$lb-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	d=$(awk '/^Log directory/ { print $3 }' $L/$lb.log | head -n 1)
	if [ -f "$d/log" ]; then
		dg=$(digest "$d/log")
		printf '%s\n' "$dg" | tee -a $L/runs.txt
		for n in $np; do
			fails "$d/log" $n
		done | tee -a $L/runs.txt
	else
		dg=""
		echo "  no runner log (Log directory: '$d')" | tee -a $L/runs.txt
	fi
	say "$lb arcstats since its load: $(asline $lb)"
	kern_check $lb "$since" 15 $(grep -c '\[KILLED\]' $L/$lb-tests.txt) || bad=1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		stop "$lb: outer timeout"
		bad=1
	fi
	leftovers $lb $fd "$(cat $L/tun-$lb.txt)" || bad=1
	note=$(printf '%s\n' "$dg" | sed -nE 's/^.* l2_size note \[([^]]*)\].*$/\1/p' | head -n 1)
	echo "$lb B $(echo "${c:-NONE}" | tr -d ' ') - $rc $wall ${note:--} $(row_tail $lb)" > $L/row-$lb.txt
	tail -n +$((n0 + 1)) $L/runs.txt > $L/sect-$lb.txt
	return $bad
}
ENV
bash -n /var/log/zts-hr0-1028/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-hr0-1028/env.sh
date '+%F %T' > $L/start.txt
: > $L/runs.txt
sudo -n dmsetup ls > $L/dm-before.txt 2>&1
for d in b1 b2 b3 m1 m2 m3 bg; do mkdir -m 1777 $W/files-$d || echo "mkdir files-$d failed"; done
if [ "$(git -C $DOC rev-parse --show-toplevel 2>/dev/null)" = $DOC ] && [ -f $DOC/$DOCFILE ]; then
	git -C $DOC rev-parse HEAD > $L/doc-head.txt
	sha256sum < $DOC/$DOCFILE | cut -d' ' -f1 > $L/doc-sha.txt
fi
{
echo "start $(cat $L/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
stat -c '%U %a %n' $W $L $W/files-b1 $W/files-b2 $W/files-b3 $W/files-m1 $W/files-m2 $W/files-m3 $W/files-bg
echo "doc checkout: head $(cat $L/doc-head.txt 2>&1), $DOCFILE sha256 $(cat $L/doc-sha.txt 2>&1)"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "dmsetup ls: $(cat $L/dm-before.txt)"
echo "fio: $(command -v fio || echo missing)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then `start ...; tigerblue77;
<uname -r>; umask 0022`, nine `stat` lines (`tigerblue77 755` for `$W` and
`$L`, `tigerblue77 1777` for the seven `files-*` directories), the doc
checkout line with its full head and the same sha256 as STEP 0 printed, and
the `zpool.cache`, `dmsetup ls: No devices found`, `fio` and free-space
lines. No `mkdir files-.. failed` line. A doc checkout line with `No such
file or directory` means this document was not read from `$DOC`: record it,
it is not a stop (STEP 7 then keeps `$DOC`).

## STEP 2: the sources, the two trees, the builds

Fetch B by its commit from the fork and upstream master (read-only towards
GitHub), and pin them: B's parent is in upstream master, what B changes, its
patch-id and that of its test-side files, their blobs, `arc.c` in both, and
what `zfs.4` says of `l2arc_headroom=0` in the parent. Where upstream master
and the fork branch point now is recorded only:

```sh
. /var/log/zts-hr0-1028/env.sh
if ! stopped; then
	{
	git init -q $W/repo > $L/init.log 2>&1; echo "init rc=$?"
	git -C $W/repo fetch -q --no-tags $FORK "+$HB:refs/heads/b" > $L/fetch-fork.log 2>&1; echo "fetch B rc=$?"
	git -C $W/repo fetch -q --no-tags $UPSTREAM "+refs/heads/master:refs/heads/upstream-master" > $L/fetch-upstream.log 2>&1; echo "fetch upstream rc=$?"
	cd $W/repo
	same "B" "$(git rev-parse -q --verify refs/heads/b)" $HB
	same "B's parent" "$(git rev-parse -q --verify $HB^)" $HP
	same "B's parent in upstream master" "$(git merge-base --is-ancestor $HP refs/heads/upstream-master && echo yes)" yes
	same "what B changes" "$(git diff --numstat $HP $HB | xargs)" "$NUMB"
	same "B's patch-id" "$(git diff $HP $HB | git patch-id --stable | cut -d' ' -f1)" $PIDB
	same "B outside tests/" "$(git diff --name-only $HP $HB -- . ':!tests' | xargs)" $ARC
	same "B's test-side files" "$(git diff --name-only $HP $HB -- tests | xargs)" "$TFILES"
	same "their patch-id" "$(git diff $HP $HB -- tests | git patch-id --stable | cut -d' ' -f1)" $PIDT
	same "their blobs in B" "$(for f in $TFILES; do printf '%s ' "$(git rev-parse -q --verify $HB:$f)"; done | sed 's/ $//')" "$TBLOBS"
	same "arc.c in the parent and in B" "$(git rev-parse -q --verify $HP:$ARC) $(git rev-parse -q --verify $HB:$ARC)" "$ARCP $ARCB"
	same "zfs.4 in the parent: l2arc_headroom=0 disables the limit" \
	    "$(git show $HP:man/man4/zfs.4 | grep -A5 '^[.]It Sy l2arc_headroom Ns ' | grep -cx 'disables the per-cycle headroom limit[.]')" 1
	for c in $HP $HB; do echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"; done
	echo "upstream master now (recorded only): $(git log -1 --abbrev=10 --format='%h %s' refs/heads/upstream-master)"
	echo "on GitHub now (recorded only): $(git ls-remote $FORK refs/heads/$FIXBRANCH 2>&1 |
	    awk '{ printf "%s=%.10s", $2, $1 }')"
	} 2>&1 | tee $L/fix.txt
	grep -q '^MISMATCH' $L/fix.txt && stop "the commits are not as expected"
	grep -qE '^(init|fetch [A-Za-z]+) rc=[1-9]' $L/fix.txt && stop "the init or a fetch failed"
fi
```

Expected:

```
init rc=0
fetch B rc=0
fetch upstream rc=0
ok: B
ok: B's parent
ok: B's parent in upstream master
ok: what B changes
ok: B's patch-id
ok: B outside tests/
ok: B's test-side files
ok: their patch-id
ok: their blobs in B
ok: arc.c in the parent and in B
ok: zfs.4 in the parent: l2arc_headroom=0 disables the limit
  5ebd13aa9a ZTS: sync checksum test data before fault injection
  33e2b1f648 L2ARC: Do not stop writing when l2arc_headroom is 0
upstream master now (recorded only): <its head>
on GitHub now (recorded only): refs/heads/l2arc-headroom-zero=33e2b1f648
```

A `MISMATCH`, or a failed init or fetch, means stop. The last two lines are
not checked.

Then one worktree per tree. M is the parent with B's four test-side files
checked out from B, and nothing else: against the parent it differs by those
files only (so `module/` is the parent's), against B by `arc.c` only. Each
tree's `arc.c` and its two markers (`guard`: `if (l2arc_headroom != 0 &&`, at
line 9720 in `l2arc_write_sublist()` in both trees and also at line 10067 in
B; `bare check`: master's `if (consumed_headroom >= headroom)` at line 10067),
the test file, its runfile entry and its tunable, and the range of kernels the
tree's `META` declares, which must hold the running one:

```sh
. /var/log/zts-hr0-1028/env.sh
if ! stopped; then
	{
	git -C $W/repo worktree add --detach $W/B $HB >> $L/worktree.log 2>&1
	echo "worktree B rc=$? at $(git -C $W/B rev-parse --short=10 HEAD 2>&1)"
	git -C $W/repo worktree add --detach $W/M $HP >> $L/worktree.log 2>&1
	echo "worktree M rc=$? at $(git -C $W/M rev-parse --short=10 HEAD 2>&1)"
	cd $W/M && git checkout $HB -- $TFILES
	echo "M: B's test-side files checked out rc=$?"
	same "B as committed" "$(git -C $W/B status --porcelain --untracked-files=no | wc -l)" 0
	cd $W/M
	same "M against the parent: B's test-side files only" "$(git diff $HP --name-only | xargs)" "$TFILES"
	same "M against the parent: their numstat" "$(git diff $HP --numstat | xargs)" "$NUMT"
	same "M against the parent: their patch-id" "$(git diff $HP | git patch-id --stable | cut -d' ' -f1)" $PIDT
	same "M: module/ as in the parent" "$(git diff $HP --name-only -- module | wc -l)" 0
	same "M against B: arc.c only" "$(git diff $HB --name-only | xargs)" $ARC
	same "M's arc.c is the parent's" "$(git hash-object $ARC)" $ARCP
	same "B's arc.c" "$(git -C $W/B hash-object $ARC)" $ARCB
	k=$(uname -r | cut -d. -f1,2)
	for t in B M; do
		cd $W/$t
		echo "$t: arc.c $(git hash-object $ARC | cut -c1-10)," \
		    "guard x$(grep -c -F "$SGUARD" $ARC) at lines [$(grep -n -F "$SGUARD" $ARC | cut -d: -f1 | xargs)]," \
		    "bare check x$(grep -c -F "$SBARE" $ARC) at line [$(grep -n -F "$SBARE" $ARC | cut -d: -f1 | xargs)];" \
		    "test $(git hash-object $F | cut -c1-10) mode $(stat -c %a $F)," \
		    "in common.run x$(grep -c "'$T'" tests/runfiles/common.run)," \
		    "L2ARC_HEADROOM in tunables.cfg x$(grep -c '^L2ARC_HEADROOM[[:space:]]' $Z/include/tunables.cfg)"
		lo=$(awk '/^Linux-Minimum:/ { print $2 }' META)
		hi=$(awk '/^Linux-Maximum:/ { print $2 }' META)
		same "$t supports Linux $k (META: $lo to $hi)" \
		    "$([ -n "$lo" ] && [ -n "$hi" ] && [ "$(printf '%s\n' $lo $k $hi | sort -V | xargs)" = "$lo $k $hi" ] && echo within)" within
	done
	} 2>&1 | tee $L/trees.txt
	grep -q '^MISMATCH' $L/trees.txt && stop "the trees are not as expected"
	grep -qE '^(worktree [A-Z]+|M: .* checked out) rc=[1-9]' $L/trees.txt && stop "a worktree or the checkout failed"
fi
```

Expected:

```
worktree B rc=0 at 33e2b1f648
worktree M rc=0 at 5ebd13aa9a
M: B's test-side files checked out rc=0
ok: B as committed
ok: M against the parent: B's test-side files only
ok: M against the parent: their numstat
ok: M against the parent: their patch-id
ok: M: module/ as in the parent
ok: M against B: arc.c only
ok: M's arc.c is the parent's
ok: B's arc.c
B: arc.c 1fe44ed3bc, guard x2 at lines [9720 10067], bare check x0 at line []; test 7c6d7c5542 mode 755, in common.run x1, L2ARC_HEADROOM in tunables.cfg x1
ok: B supports Linux 6.12 (META: 4.18 to 7.3)
M: arc.c 15565d1192, guard x1 at lines [9720], bare check x1 at line [10067]; test 7c6d7c5542 mode 755, in common.run x1, L2ARC_HEADROOM in tunables.cfg x1
ok: M supports Linux 6.12 (META: 4.18 to 7.3)
```

(`6.12` is the running kernel's version; another one is fine as long as the
two lines say `ok`.) Anything else means stop.

Then build B and M, one after the other (10 to 15 minutes together; see the
rule about long commands), both debug builds:

```sh
. /var/log/zts-hr0-1028/env.sh
build_tree B --enable-debug && build_tree M --enable-debug
```

Expected: `build B (configure --enable-debug) rc=0 wall=<s>s free=<n>G`, then
the same for M, the free space going down by about 1G per tree. A failed
build prints its first `error:` lines and `the build of <tree>: stop`.

Then check the two builds, point each tree's runner at `$L/results` (instead
of `/var/tmp/test_results`), check again what each tree differs by, and
record the two srcversions for the helpers:

```sh
. /var/log/zts-hr0-1028/env.sh
if ! stopped; then
	{
	for t in B M; do
		cd $W/$t
		same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
		same "$t ZFS_DEBUG lines (defined, undefined)" \
		    "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1) $(grep -c '^/\* #undef ZFS_DEBUG \*/' zfs_config.h 2>&1)" "3 0"
		same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
		same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb \
		    scripts/zfs.sh scripts/zfs-tests.sh $TR; do [ -e $f ] || echo $f; done | xargs)" ""
		modinfo -F srcversion module/zfs.ko > $L/srcv-$t 2>&1
		echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
		    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:') (objtool's, about 83);" \
		    "srcversion $(cat $L/srcv-$t)"
		grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
		same "$t zfs.ko holds the debug assertion text" "$([ "$(grep -a -o -F "$SDBG" module/zfs.ko | wc -l)" -ge 1 ] && echo yes)" yes
		sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" $TR.in $TR
		same "$t results directory" "$(grep -cx "BASEDIR = '$L/results'" $TR) $(grep -cx "BASEDIR = '$L/results'" $TR.in)" "1 1"
		same "$t runner executable" "$([ -x $TR ] && echo yes)" yes
	done
	same "two different srcversions" "$(cat $L/srcv-B $L/srcv-M | grep -xE '[0-9A-F]+' | sort -u | wc -l)" 2
	same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
	same "M head" "$(git -C $W/M rev-parse HEAD)" $HP
	same "B against B's commit" "$(git -C $W/B diff $HB --name-only | xargs)" $TR.in
	same "M against B's commit" "$(git -C $W/M diff $HB --name-only | xargs)" "$ARC $TR.in"
	same "M against the parent" "$(git -C $W/M diff $HP --name-only | xargs)" \
	    "$(echo $TFILES $TR.in | xargs -n1 | LC_ALL=C sort | xargs)"
	same "M: module/ as in the parent" "$(git -C $W/M diff $HP --name-only -- module | wc -l)" 0
	echo "free after the builds: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (5G at least)"
	} 2>&1 | tee $L/build-checks.txt
	if grep -q '^MISMATCH' $L/build-checks.txt; then
		stop "the builds"
	elif [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 5 ]; then
		stop "less than 5G free after the builds"
	else
		printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh
		tail -n 2 $L/env.sh
	fi
fi
```

Expected, for each tree: four `ok:` lines (`ZFS_DEBUG lines` are `3 0`),
`compiler warnings: 0` and about 83 other lines with its srcversion, then
`ok:` for the debug assertion text, the results directory and the runner.
Then `ok: two different srcversions` (`arc.c` differs), six more `ok:` lines,
the free space, and the two lines `SV_B=<B's>` and `SV_M=<M's>`. The
srcversions are recorded only. A compiler warning count other than 0 is not a
stop: quote its lines. Nothing is built after this point.

## STEP 3: B, the test three times (runs `b1`, `b2`, `b3`)

Each run loads B's modules afresh (nothing is loaded yet, so the first
reload loads), then runs the test (1 to 3 minutes each):

```sh
. /var/log/zts-hr0-1028/env.sh
one b1 B && one b2 B && one b3 B
```

Expected, for each of `b1`, `b2`, `b3`:

```
b1: load B rc=0, srcversion <SV_B>, pools []
  tunables: l2arc_headroom=8 l2arc_noprefetch=1 ... (recorded)
kernel ld-b1: 0 findings, 0 ZTS marks (expected 0), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
before b1: fd=/var/tmp/zts-hr0-1028/files-b1 (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] others=[] tunables as loaded
b1 (B) l2arc_headroom_zero_pos PASS x1 [<mm:ss>] rc=0 wall=<s>s; all results: PASS x3
  l2arc_headroom_zero_pos: headroom set to 0 x1, waited for l2_size x1, l2_size note [<n>, above 0], log_pass message x1, local cleanup x1, headroom restored x1, ERROR lines x0
    last line: SUCCESS: set_tunable32 L2ARC_NOPREFETCH 1
b1 arcstats since its load: l2_feeds <n>, l2_writes_sent <n>, l2_writes_done <n>, l2_writes_error 0, l2_write_bytes <n, above 0: hundreds of millions>, l2_size <n> (recorded), ...
kernel b1: 0 findings, 3 ZTS marks (expected 3), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-hr0-1028/files-b1 holds: [testdir] (expected [testdir])
    <the ls -la lines: testdir, a directory owned by root, and nothing in it>
leftovers after b1: as expected
```

The `l2_size` note is the test's own reading of `l2_size` once it settled,
in bytes. The tunables at load are the module's defaults (zfs.4:
`l2arc_headroom=8`, `l2arc_noprefetch=1`); a value set by this VM's
configuration is fine, since each run is compared with what its own load
found. Recorded only: the tunables, the runner time, the wall time, the
`last line:` lines and every arcstats value. A test that does not pass is a
result: its first ERROR line and last line follow it; record it and go on. A
`LEFTOVER` line, or `leftovers after ...: NOT AS EXPECTED`, means the run left
something else: the lines say what, and the step stops.

## STEP 4: M, the test three times (runs `m1`, `m2`, `m3`)

Each run loads M's modules afresh (the reload first unloads B's), then runs
the test, which waits until the runner kills it at 600 s: 11 to 13 minutes
each, 35 to 40 together (see the rule about long commands):

```sh
. /var/log/zts-hr0-1028/env.sh
one m1 M && one m2 M && one m3 M
```

Expected, for each of `m1`, `m2`, `m3`:

```
m1: load M rc=0, srcversion <SV_M>, pools []
  tunables: l2arc_headroom=8 l2arc_noprefetch=1 ... (recorded)
kernel ld-m1: 0 findings, 0 ZTS marks (expected 0), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
before m1: fd=/var/tmp/zts-hr0-1028/files-m1 (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] others=[] tunables as loaded
m1 (M) l2arc_headroom_zero_pos KILLED x1 [10:00] rc=<1 or 2> wall=<about 650 to 750>s; all results: KILLED x1, PASS x2
  l2arc_headroom_zero_pos: headroom set to 0 x1, waited for l2_size x1, l2_size note [], log_pass message x0, local cleanup x0, headroom restored x0, ERROR lines x0
    last line: Waiting for arcstat l2_size quiescence.
  l2arc_headroom_zero_pos: first ERROR line: none
    last line: Waiting for arcstat l2_size quiescence.
m1 arcstats since its load: l2_feeds <n, in the hundreds>, l2_writes_sent 0, l2_writes_done 0, l2_writes_error 0, l2_write_bytes 0, l2_size 0, ...
kernel m1: 0 findings, 3 ZTS marks (expected 3), 1 timeout dumps (expected 1; <n> lines, <n> of them matching, set apart) (<n> lines)
  tunables: l2arc_headroom=0 l2arc_noprefetch=0, as the killed test left them, the others as loaded (the next load resets them)
  loop devices: []
  /var/tmp/zts-hr0-1028/files-m1 holds: [testdir] (expected [testdir])
    <the ls -la lines>
leftovers after m1: as expected
```

**This is the bug in the kernel**: with `l2arc_headroom=0`, master's feed
thread runs (`l2_feeds` grows every second while the cache device is there)
and writes nothing (`l2_write_bytes` and `l2_writes_sent` 0); the test's wait
for a non-zero `l2_size` never ends, and the runner kills it. Before the
kill, the runner writes its timeout dump to the kernel log (see "What the
runner writes ..." above): the `1 timeout dumps` of the kernel line, its
`Call Trace:` lines among the `of them matching`, set apart and not counted
as findings. The runner time is recorded as it comes (`[10:00]` or
`[10:01]`), and so are the rc, the wall time, the `last line:` lines and the
dump's line counts. A test that ends otherwise (`FAIL` with a note
`l2_size=0`, or `PASS`) is a result, not a stop: record it, "What the results
mean" says how to read it. The setup and the cleanup must pass (`all
results: KILLED x1, PASS x2`); one that does not is listed as `not passed:`,
recorded.

## STEP 5: B, the l2arc group (run `bg`)

B's modules are loaded afresh, then the whole group runs once: `setup`, the
13 tests of `common.run`'s `[tests/functional/l2arc]` (B's new one last) and
`cleanup`, 15 results (10 to 25 minutes):

```sh
. /var/log/zts-hr0-1028/env.sh
group bg
```

Expected:

```
bg: load B rc=0, srcversion <SV_B>, pools []
  tunables: ... (recorded)
kernel ld-bg: 0 findings, 0 ZTS marks (expected 0), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
before bg: fd=/var/tmp/zts-hr0-1028/files-bg (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] others=[] tunables as loaded
bg (B) -T l2arc PASS x15 (Test: x15, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <whatever it lists, or no such line>
  l2arc_headroom_zero_pos: headroom set to 0 x1, waited for l2_size x1, l2_size note [<n>, above 0], log_pass message x1, local cleanup x1, headroom restored x1, ERROR lines x0
    last line: SUCCESS: set_tunable32 L2ARC_NOPREFETCH 1
bg arcstats since its load: ... (recorded)
kernel bg: 0 findings, 15 ZTS marks (expected 15), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-hr0-1028/files-bg holds: [testdir] (expected [testdir])
    <the ls -la lines>
leftovers after bg: as expected
```

A test that does not pass is listed as `not passed:` with its first ERROR
line and last line: a result, recorded. With `l2arc_headroom` at 8, which
every other test of the group keeps, B's `arc.c` change does nothing: a test
other than `l2arc_headroom_zero_pos` that does not pass here comes from master
or this VM, not from the fix. Recorded only: the tunables, the wall time, the
`Missing util(s)` and `last line:` lines and the arcstats. The group does not
start with less than 5G free on `/var/tmp`: that is a stop.

## STEP 6: the runs side by side

```sh
. /var/log/zts-hr0-1028/env.sh
{
printf '%-4s %-4s %-12s %-6s %-3s %-6s %-12s %-15s %-6s %-7s %-7s %s\n' run tree result time rc wall l2_size_note l2_write_bytes sent feeds kernel dumps
for r in b1 b2 b3 m1 m2 m3 bg; do
	if [ -s $L/row-$r.txt ]; then
		printf '%-4s %-4s %-12s %-6s %-3s %-6s %-12s %-15s %-6s %-7s %-7s %s\n' $(cat $L/row-$r.txt)
	else
		echo "$r: not run"
	fi
done
nb=0
nm=0
for r in b1 b2 b3; do
	[ -s $L/row-$r.txt ] || continue
	set -- $(cat $L/row-$r.txt)
	[ "$3" = PASS ] && [ "$7" != - ] && [ "${7%%,*}" -gt 0 ] && [ "$8" -gt 0 ] && [ "${11}" -eq 0 ] && nb=$((nb + 1))
done
for r in m1 m2 m3; do
	[ -s $L/row-$r.txt ] || continue
	set -- $(cat $L/row-$r.txt)
	[ "$3" != PASS ] && { [ "$7" = - ] || [ "$7" = 0 ]; } && [ "$8" = 0 ] && [ "$9" = 0 ] && [ "${11}" -eq 0 ] && nm=$((nm + 1))
done
echo "B: $nb of 3 runs PASS, with an l2_size note above 0, l2_write_bytes above 0 and no kernel finding (expected 3)"
echo "M: $nm of 3 runs not PASS, with no l2_size note above 0, l2_write_bytes 0, l2_writes_sent 0 and no kernel finding (expected 3)"
echo "M results: $(for r in m1 m2 m3; do [ -s $L/row-$r.txt ] && awk '{ printf "%s=%s ", $1, $3 }' $L/row-$r.txt; done)(expected KILLED x3)"
if [ -s $L/row-bg.txt ]; then
	set -- $(cat $L/row-bg.txt)
	echo "bg: $3 (expected PASSx15), the new test's l2_size note [$7], kernel findings ${11}, timeout dumps ${12}"
else
	echo "bg: not run"
fi
for t in B M; do
	echo "$t srcversion $(cat $L/srcv-$t)"
done
} 2>&1 | tee $L/compare.txt
```

Expected:

```
run  tree result       time   rc  wall   l2_size_note l2_write_bytes  sent   feeds   kernel  dumps
b1   B    PASS         <m:s>  0   <s>    <n>          <n>             <n>    <n>     0       0
b2   B    PASS         <m:s>  0   <s>    <n>          <n>             <n>    <n>     0       0
b3   B    PASS         <m:s>  0   <s>    <n>          <n>             <n>    <n>     0       0
m1   M    KILLED       10:00  <rc> <s>   -            0               0      <n>     0       1
m2   M    KILLED       10:00  <rc> <s>   -            0               0      <n>     0       1
m3   M    KILLED       10:00  <rc> <s>   -            0               0      <n>     0       1
bg   B    PASSx15      -      0   <s>    <n>          <n>             <n>    <n>     0       0
B: 3 of 3 runs PASS, with an l2_size note above 0, l2_write_bytes above 0 and no kernel finding (expected 3)
M: 3 of 3 runs not PASS, with no l2_size note above 0, l2_write_bytes 0, l2_writes_sent 0 and no kernel finding (expected 3)
M results: m1=KILLED m2=KILLED m3=KILLED (expected KILLED x3)
bg: PASSx15 (expected PASSx15), the new test's l2_size note [<n>], kernel findings 0, timeout dumps 0
B srcversion <SV_B>
M srcversion <SV_M>
```

(The columns are as `printf` aligns them; a wide value pushes the rest of its
row to the right.) Anything else is a result, not a stop: "What the results
mean" says how to read it.

## STEP 7: unload, release, report, cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the whole task's kernel check and the
unload; the module stays loaded on `HELD` or `PANIC`, and then no `zpool`
command runs:

```sh
. /var/log/zts-hr0-1028/env.sh
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso > $L/kern-all.txt 2>&1
read -r k kl kc <<< "$(kdumps $L/kern-all.txt)"
echo "kernel, whole task: $(kout $L/kern-all.txt | grep -iE "$KRE" | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $L/kern-all.txt) ZTS marks, $k timeout dumps ($kl lines, $kc of them matching, set apart)"
kout $L/kern-all.txt | grep -iE "$KRE" | grep -vE "$KIGN" | head -n 40
sudo -n journalctl -k -b --no-pager -q -o short-iso > $L/kern-boot.txt 2>&1
echo "kernel, this boot: $(grep -iE "$KRE" $L/kern-boot.txt | grep -vcE "$KIGN") counted as in STEP 0," \
    "$(kout $L/kern-boot.txt | grep -iE "$KRE" | grep -vcE "$KIGN") outside timeout dumps"
t=$(cat $L/loaded 2>/dev/null)
if [ -e $L/HELD ] || [ -e $L/PANIC ]; then
	echo "module left loaded: $(cd $L && ls HELD PANIC 2>/dev/null | xargs); no pool command run"
elif [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(sv_of $t)" ]; then
	p=$(zc $t zpool list -H -o name 2>&1)
	if [ -n "$p$(grep -E ' zfs |testpool' /proc/mounts)" ]; then
		echo "a pool or a mount is left ($p): module left loaded"
	elif [ -e /var/run/zed.pid ]; then
		echo "/var/run/zed.pid exists: module left loaded"
	else
		(cd $W/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
	fi
else
	echo "no module of this task is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
echo "loop devices: [$(loops | xargs)]"
echo "mounts under W: [$(grep " $W[ /]" /proc/mounts)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings, 33 ZTS marks, 3 timeout dumps
(<n> lines, <c> of them matching, set apart)` (3 marks per test run, 15 for
the group, one dump per M run; fewer after a stop), no kernel line, then
`kernel, this boot: <the STEP 0 baseline plus c> counted as in STEP 0, <the
baseline or less> outside timeout dumps` (less if an earlier task's dump is in
this boot), `unload B rc=0`, `lsmod: []`, `loop devices: []`, `mounts under W:
[]`, a 0-byte or absent `zpool.cache`, no `vdev_id.conf`.

Then release the `testdir` each run left, and only those (each still exactly
an empty directory of root's), then the seven files directories:

```sh
. /var/log/zts-hr0-1028/env.sh
{
for d in b1 b2 b3 m1 m2 m3 bg; do
	f=$W/files-$d
	if [ ! -d $f ]; then
		echo "$f: absent"
		continue
	fi
	if [ "$(fd_state $f)" = testdir ]; then
		sudo -n rmdir $f/testdir; echo "released $f/testdir rc=$?"
	else
		echo "$f holds [$(LC_ALL=C ls -A $f | xargs)]: nothing released"
	fi
	rmdir $f; echo "rmdir files-$d rc=$?"
done
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $L/end.txt
```

Expected: `released /var/tmp/zts-hr0-1028/files-<run>/testdir rc=0` and
`rmdir files-<run> rc=0` for each of the seven runs, `W holds: B M repo`, the
free space. After a stop, a files directory of a run that did not happen is
empty (`... holds []: nothing released`, then `rmdir ... rc=0`), and one that
holds anything else stays (`rmdir` fails on it): report it, remove nothing
else by hand.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-hr0-1028/env.sh
{
echo "VM 107: l2arc_headroom=0, B 33e2b1f648 (the fix, debug) against M (5ebd13aa9a with B's test-side files, debug) (w/VM-TESTS-107-hr0.md)"
for s in record fix trees build build-checks runs compare end; do
	echo
	echo "== $s.txt"
	cat $L/$s.txt 2>&1
done
echo
echo "== summary"
for t in B M; do
	echo "$t srcversion $(cat $L/srcv-$t 2>/dev/null)"
done
for r in b1 b2 b3 m1 m2 m3 bg; do
	grep -E "^$r [(][BM][)] |^  $T: headroom |^$r arcstats |^kernel $r:|^leftovers after $r:|: stop$" $L/sect-$r.txt 2>/dev/null
done
grep -vE '^[A-Z]+ srcversion ' $L/compare.txt 2>/dev/null
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, `uname -r`, the
memory line, the free space, the `vm-task-107v` line, the doc checkout line
and its sha256, the kernel baseline, the tools lines and the `fio` version),
a `verdict:` line naming the case of "What the results mean" that applies,
and a `deviations:` line with every deviation from these steps and its reason
(`none` if there was none). Then `cat /var/log/zts-hr0-1028/REPORT.txt`.
Expected summary:

```
B srcversion <SV_B>
M srcversion <SV_M>
b1 (B) l2arc_headroom_zero_pos PASS x1 [<mm:ss>] rc=0 wall=<s>s; all results: PASS x3
  l2arc_headroom_zero_pos: headroom set to 0 x1, waited for l2_size x1, l2_size note [<n>], log_pass message x1, local cleanup x1, headroom restored x1, ERROR lines x0
b1 arcstats since its load: ...
kernel b1: 0 findings, 3 ZTS marks (expected 3), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
leftovers after b1: as expected
<the same five lines for b2 and b3>
m1 (M) l2arc_headroom_zero_pos KILLED x1 [10:00] rc=<rc> wall=<s>s; all results: KILLED x1, PASS x2
  l2arc_headroom_zero_pos: headroom set to 0 x1, waited for l2_size x1, l2_size note [], log_pass message x0, local cleanup x0, headroom restored x0, ERROR lines x0
m1 arcstats since its load: l2_feeds <n>, l2_writes_sent 0, l2_writes_done 0, l2_writes_error 0, l2_write_bytes 0, ...
kernel m1: 0 findings, 3 ZTS marks (expected 3), 1 timeout dumps (expected 1; <n> lines, <n> of them matching, set apart) (<n> lines)
leftovers after m1: as expected
<the same five lines for m2 and m3>
bg (B) -T l2arc PASS x15 (Test: x15, Test (Linux): x0) rc=0 wall=<s>s
  l2arc_headroom_zero_pos: headroom set to 0 x1, ..., l2_size note [<n>], ...
bg arcstats since its load: ...
kernel bg: 0 findings, 15 ZTS marks (expected 15), 0 timeout dumps (expected 0; 0 lines, 0 of them matching, set apart) (<n> lines)
leftovers after bg: as expected
<the table and the four lines of STEP 6, without the srcversions>
flags: STOP [] HELD [] PANIC []
verdict: as expected (first case)
deviations: none
```

Then, as the very last command, and only if the unload block printed
`unload B rc=0` with `lsmod: []` and `mounts under W: []`, and no step
stopped, remove this task's directories and the doc checkout (the doc
checkout only if it is still the clean checkout STEP 1 recorded, holding this
document, and nothing else). `$L` sits in root's `/var/log`: its contents go
with `rm -rf`, the then empty directory with `sudo -n rmdir`:

```sh
cd /
D=/var/tmp/vm-task-107w
if grep -q " /var/tmp/zts-hr0-1028[ /]" /proc/mounts; then
	echo "something is mounted under /var/tmp/zts-hr0-1028: nothing removed"
else
	if [ -d $D ] && [ ! -L $D ] && [ "$(stat -c %U $D)" = tigerblue77 ] && [ -d $D/.git ] && [ ! -L $D/.git ] &&
	    [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
	    [ -s /var/log/zts-hr0-1028/doc-head.txt ] &&
	    [ "$(git -C $D rev-parse HEAD 2>/dev/null)" = "$(cat /var/log/zts-hr0-1028/doc-head.txt)" ] &&
	    git -C $D ls-files --error-unmatch w/VM-TESTS-107-hr0.md > /dev/null 2>&1 &&
	    [ "$(sha256sum < $D/w/VM-TESTS-107-hr0.md | cut -d' ' -f1)" = "$(cat /var/log/zts-hr0-1028/doc-sha.txt 2>/dev/null)" ] &&
	    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    [ "$(LC_ALL=C ls -A $D | grep -vx .git | xargs)" = "$(git -C $D ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
	    ! grep -q " ${D}[ /]" /proc/mounts; then
		X=$D
	else
		X=""; echo "$D kept: not only the clean doc checkout STEP 1 recorded"
	fi
	rm -rf /var/tmp/zts-hr0-1028 $X /var/log/zts-hr0-1028/*; echo "rm rc=$?"
	sudo -n rmdir /var/log/zts-hr0-1028; echo "rmdir rc=$?"
fi
ls -d /var/tmp/zts-hr0-1028 /var/log/zts-hr0-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, or a step stopped, run nothing of this block:
keep everything and say so. If your permission system refuses this `rm -rf`
or this `rmdir`, leave what is left and say so; do not delete it any other
way. Then reply with all of `REPORT.txt` as printed, and the result of the
last block: nothing runs after the reply.

## What the results mean

- **As expected** (STEP 6: B 3 of 3, M 3 of 3, `bg` `PASSx15`; no kernel
  finding; a clean unload): on a kernel, with `l2arc_headroom=0`, master's
  feed thread runs and writes nothing to the cache device
  (`l2_write_bytes` 0), so the test's wait for a non-zero `l2_size` never
  ends and the runner kills it, three times out of three; with the fix the
  same test passes three times out of three, its cache device holding data
  (the `l2_size` it logs, the bytes written), and the rest of the l2arc group
  still passes.
- **M writes to the cache device** (`l2_write_bytes` above 0), or its test
  passes: the bug did not show. Check STEP 2's `bare check x1 at line
  [10067]` for M, each M run's srcversion line, and that its digest says
  `headroom set to 0 x1`; report `$L/m<n>.arcstats`.
- **M's test fails instead of being killed**, with a note `l2_size=0`: the
  wait returned with `l2_size` 0, which this `libtest.shlib` does not do;
  report its lines. Still master writing nothing, if `l2_write_bytes` is 0.
- **B's test does not pass, or B writes nothing** (`l2_write_bytes` 0, the
  test killed like M's): the fix does not hold in the kernel, or the test
  cannot show it; check STEP 2's `guard x2` for B, report the runs' lines and
  `$L/b<n>.arcstats`. **B passes one or two times out of three**: flaky,
  report every run's lines.
- **`bg` has a test other than `l2arc_headroom_zero_pos` that does not
  pass**: with `l2arc_headroom` at 8 the fix changes nothing for it (see
  STEP 5); report its `not passed:` lines as master's or this VM's.
  `l2arc_headroom_zero_pos` failing in the group while `b1` to `b3` passed:
  report both.
- **`LEFTOVER` anything**: a test or the runner left it; the lines say what.
- **A timeout dump with no killed test** (a stop): a sysrq backtrace or a
  `ZTS timeout debug` record the runner did not cause; report its lines from
  the run's `$L/kern-<run>.txt`. **A killed test with no dump** (`0 timeout
  dumps (expected 1)`): the runner's dump did not reach the kernel log;
  recorded, not a stop.
- **Any kernel finding**: the line and the run are the result. Stop.

Estimated duration: about 1 hour 30 minutes (checks 5, fetch and worktrees
5, the builds 10 to 15, B's runs 5, M's runs 35 to 40, the group 10 to 25,
the end 5).
