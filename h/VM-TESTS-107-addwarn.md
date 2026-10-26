# VM 107: `zpool_add_warn_removal` leaves its loop device behind

These instructions are for the session on VM 107. They run the upstream ZTS
test `functional/cli_root/zpool_add/zpool_add_warn_removal` in the master
tree already built under `/var/tmp/zts-algo-1026` (built at 11:46Z, srcversion
`8852710DB871A7587B9E722`), first as master has it, then with the fix of the
fork branch `tigerblue77/zfs:zpool-add-warn-removal-log-pass` (commit
`4e8a517729942d1ac159eb49cfe9c93574fc0b99`), and check after every run what it
left on the VM. Nothing is built: the fix is a test script, which the in-tree
runner reads from the source tree, so both states use the same modules.

| State | Test file in `/var/tmp/zts-algo-1026/M` | Runs | Expected |
| --- | --- | --- | --- |
| M | master's, `a128b8a075` | `m1`-`m3` | PASS 3/3, rc 0; after each, one loop device bound to `$FD/vdev4` and `vdev0`-`vdev9` in `$FD`, and no cleanup line in the test's log; released after each run |
| B | the fix's (master + 2 lines), nothing else changed | `b1`-`b3`, then `bg` (`-T zpool_add` once) | PASS 3/3, rc 0, the cleanup line and `SUCCESS: losetup -d`, nothing left; `bg` PASS x19, nothing left |

M runs first. At the end the tree gets its original test file and runner back,
and only this task's own directories go.

## What the code does (read at `a128b8a075` and `4e8a517729`)

- The test registers its cleanup with `log_onexit zpool_create_add_cleanup`
  (line 36) and calls `zpool_create_add_setup` (line 38). The setup, in
  `zpool_add.kshlib` (lines 84-102), creates `vdev0` to `vdev9` (256 MiB,
  sparse) in `$TEST_BASE_DIR`, which is the `zfs-tests.sh -d` directory, and
  attaches `vdev4` to a loop device with `losetup -f --show`. The cleanup
  (lines 104-123) destroys `testpool1` if it exists, detaches that loop device
  with `log_must losetup -d`, and removes the ten files.
- `log_onexit` only records the function. `_endlog` in
  `tests/test-runner/include/logapi.shlib` (lines 431-463) runs it, after
  printing `NOTE: Performing local cleanup via log_onexit (<function>)`.
  Only `log_pass`, `log_fail`, `log_unresolved`, `log_unsupported` and
  `log_untested` call `_endlog`. No exit trap is set anywhere.
- Master's test ends at line 116 with its ninth `zpool_create_rm_add`, without
  `log_pass`. The script falls off its end with the status of its last
  command, `((i += 1))`, which is 0, so the runner records PASS and the cleanup
  never runs. Each of its 108 cycles (36 for each of log, cache and spare) ends
  with `zpool destroy -f testpool1`, so no pool is left; the loop device and
  the files are.
- `zfs-tests.sh` detaches only the three loop devices it made for
  `file-vdev0` to `file-vdev2` and removes only those files. Its failsafe
  callback runs only for a KILLED test, and only clears `zinject`. After each
  passing run on master, one loop device (`/dev/loop3` when ZTS's own are
  `loop0`-`loop2`) stays bound to `$FD/vdev4`, and `vdev0`-`vdev9` stay in
  `$FD`, owned by root (the test runs as root) in a sticky directory.
- The fix adds two lines at the end: a blank line and
  `log_pass "Verify 'zpool add' warns for differing redundancy."`. That is
  also the last line of `zpool_add_warn_create` (line 146) and
  `zpool_add_warn_degraded` (line 194), which share the setup and cleanup. The
  test's log then ends with that message, the cleanup note and
  `SUCCESS: losetup -d /dev/loop<N>`.

Both versions of the script were run with the real `ksh`, with `zpool` and the
log functions stubbed. Master exits 0 after 108 `zpool destroy`, with no
cleanup line. The fixed one exits 0 after 108, prints the cleanup line, and
ends with `SUCCESS: losetup -d`. VM 105 saw this on 26 Oct: after
`-T zpool_add` passed 19 of 19, `/dev/loop3` was still bound to `.../vdev4`.

**The group.** `-T zpool_add` runs `common.run`'s
`[tests/functional/cli_root/zpool_add]` (`setup`, 14 tests, `cleanup`: 16
`Test:` lines), then `linux.run`'s `[...zpool_add:Linux]` (`setup`,
`add_nested_replacing_spare`, `cleanup`: 3 `Test (Linux):` lines). That makes
19 results, in about 4 minutes in the fork's CI. Its tests reach outside this
task's directories in three ways, and undo each themselves:

- `add_nested_replacing_spare` links `/etc/zfs/vdev_id.conf` for its length
  and starts a `zed`. This tree's `zed` (plain `./configure`) keeps its pid
  file in `/usr/local/var/run/`, which it creates if missing. That directory,
  empty, is the one thing the group may leave.
- `add-o_ashift` and `add_prop_ashift` change `vdev_file_physical_ashift` and
  restore it.
- `zpool_add_004_pos` adds a zvol through `/dev/zvol/testpool1/testvol`, a
  link only a `60-zvol.rules` udev rule makes. Without one on this VM, that
  test fails whatever the fix, and the checks below accept exactly that
  failure.

## Rules

- Log in as `tigerblue77` and run everything as that user, in bash, in this
  order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: module loads and every run go
  through `nice -n 10 ionice -c3` (the blocks do it), one test at a time,
  nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing.
- This task's own directories are `/var/tmp/zts-algo-1026/files-addwarn`
  (`$FD`) and `/var/log/zts-algo-1026/addwarn` (`$A`). Outside them, change
  only these. In `/var/tmp/zts-algo-1026/M`: the test file (swapped in
  STEP 3, restored in STEP 5) and the runner's `BASEDIR` (redirected in
  STEP 1, restored in STEP 5). In the repository: what STEP 3's fetch adds
  (one commit's objects and `FETCH_HEAD`). Leave the tree, `repo`, the build
  logs in `/var/log/zts-algo-1026` and every other directory as they are:
  other tasks use this tree after you. Never create, edit or remove
  `/etc/zfs/zpool.cache`, `/etc/zfs/vdev_id.conf` or the `zed` pid file
  yourself. The kernel and the tests manage them.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. The only things removed by hand are `release`'s (one loop
  device and ten named files in `$FD`), the `rmdir` of `$FD`, and the
  `rm -rf` of `$A/results` in STEP 5.
- Never run a `zpool` or `zfs` command while this tree's module is not loaded.
  Never print the environment (`env`, `printenv`, `set`).
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-algo-1026/addwarn/env.sh`.
- Stop at any value the comments or Expected lines do not allow. After a line
  ending in `stop`, run only STEP 5. The helpers refuse to run once a step has
  stopped. Clean, repair and retry nothing by hand. If your permission system
  refuses a command, report it and stop; do not work around it. A kernel
  panic, oops, VERIFY or hung task means stop and report, never reboot.
- A run block takes 2 to 4 minutes, `bg` 5 to 15. If your shell tool cannot
  wait that long in the foreground, start that one block with the tool's own
  background mode and wait for it to exit. Never use `&` or `nohup`, and run
  nothing meanwhile.

What earlier VM runs taught, and the blocks already handle:

- Results are matched anywhere in a `Test:` line, which starts with a
  timestamp. A `-t` run names the test with `.ksh`
  (`Test: .../zpool_add_warn_removal.ksh (run as root) [..] [PASS]`), a `-T`
  run without it. `linux.run` tests are `Test (Linux): ...`.
- A literal parenthesis in a regex is written `[(]`.
- The test's lines are read from the runner's own `log` file. Kernel messages
  are read per run from `journalctl -k --since`, because ZTS clears dmesg after
  a failure. The `ZTS run` markers of `-K` and `spa_import_progress_set_notes_impl`
  lines are not findings.
- Each run waits 2 s first: two runs started in one second would share a
  results directory.
- `zfs-tests.sh -d` points at a mode 1777 directory.

## STEP 0: the VM free, the tree as built

```sh
export PATH=/usr/sbin:/sbin:$PATH
date -u '+%F %T'
id -un                                # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                              # 6.12.113+deb13-cloud-amd64
nproc                                 # 16
umask                                 # 0002
lsmod | grep -E '^(zfs|spl) '          # nothing
ls /sys/module | grep -xE 'zfs|spl'    # nothing
ls -l /dev/zfs 2>&1                    # No such file or directory
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db'   # nothing
pgrep -ax 'zed|lt-zed'                 # nothing
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                # No devices found
grep -E ' zfs |testpool' /proc/mounts  # nothing
ls -d /testpool* 2>&1                  # No such file or directory
ls -ld /etc/zfs 2>&1                   # a root directory
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
df -h /var/tmp                         # 5G free or more
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
```

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-algo-1026 L=/var/log/zts-algo-1026
F=tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
TR=tests/test-runner/bin/test-runner.py
stat -c '%U %n' $W $W/M $W/repo $L     # tigerblue77, four times
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"   # W holds: M repo
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"   # the build's logs; no addwarn
cd $W/M
git rev-parse HEAD                     # a128b8a075b5b13c737e56914cd7043bd3e9ebf8
git rev-parse --path-format=absolute --git-common-dir   # /var/tmp/zts-algo-1026/repo/.git (or .../repo if bare)
echo "tracked changes: $(git status --short --untracked-files=no | wc -l)"   # tracked changes: 0
echo "test: blob $(git hash-object $F), HEAD's $(git rev-parse HEAD:$F), mode $(stat -c %a $F), log_pass x$(grep -c log_pass $F), $(wc -l < $F) lines"
# test: blob 1591658ba7898aad7a121595da768c0a2215ac73, HEAD's 1591658ba7898aad7a121595da768c0a2215ac73, mode 755, log_pass x0, 116 lines
grep -n '^BASEDIR' $TR.in $TR          # line 43 of both: BASEDIR = '/var/tmp/test_results'
ls -l module/spl.ko module/zfs.ko zpool .libs/zed scripts/zfs.sh scripts/zfs-tests.sh $TR | awk '{ print $1, $NF }'   # 7 lines, x in each mode but the two .ko
echo "srcversion $(modinfo -F srcversion module/zfs.ko), vermagic $(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1), debug x$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h)"
# srcversion 8852710DB871A7587B9E722, vermagic <uname -r>, debug x1
echo "zed pid file: $(grep -aoE '/[[:alnum:]/._-]+/zed[.]pid' .libs/zed | head -n 1)"   # zed pid file: /usr/local/var/run/zed.pid
for f in $L/*.log; do [ -f "$f" ] && echo "${f##*/}: $(grep -cE ': error:|\*\*\* \[' "$f") error lines"; done   # 0 for each
```

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not `tigerblue77`, or `sudo -n` fails;
- a zfs or spl module is loaded, `/dev/zfs` exists, or a `zed` runs;
- a test or build process is listed by the `pgrep -af`;
- a tmux pane other than this session's runs anything but an idle shell;
- a loop device or device-mapper device exists, or a zfs or test mount, or a
  `/testpool*` directory;
- `/etc/zfs/zpool.cache` is not empty (a pool would appear at load), or
  `/etc/zfs/vdev_id.conf` exists (the group's `zed` setup refuses to run);
- less than 5 GB is free on `/var/tmp`, or `journalctl -k` prints no line;
- `$W`, `$W/M`, `$W/repo` or `$L` is missing or not yours, `$W` holds
  `files-addwarn`, or `$L` holds `addwarn` (this task started once already);
- any value of the second block differs from its comment: the HEAD, a tracked
  change, the test's blob, mode, `log_pass` count or length, `BASEDIR`
  already redirected, a missing file, the srcversion, a vermagic other than
  `uname -r`, a non-debug build, or an error line in a build log.

Three values are only recorded, not stops: another entry in `$W`, a kernel
other than `6.12.113+deb13-cloud-amd64` (the vermagic check decides), and a
`zed` pid file path other than `/usr/local/var/run/zed.pid` (the checks use
whatever path the tree's `zed` has).

## STEP 1: this task's environment, the results directory, the module

The block writes nothing unless it can create both directories, so a second
start changes nothing.

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/log/zts-algo-1026/addwarn /var/log/zts-algo-1026/addwarn/orig &&
    mkdir -m 1777 /var/tmp/zts-algo-1026/files-addwarn; then
echo "mkdir rc=0"
cat > /var/log/zts-algo-1026/addwarn/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-algo-1026
L=/var/log/zts-algo-1026
# This task's own directories: its logs and the runner's results (A), and
# the zfs-tests.sh -d directory (FD, mode 1777).
A=$L/addwarn
FD=$W/files-addwarn
FORK=https://github.com/tigerblue77/zfs
FIXBRANCH=zpool-add-warn-removal-log-pass
# The tree's commit (upstream master), the fix on top of it, and the
# srcversion of the tree's zfs.ko.
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
HF=4e8a517729942d1ac159eb49cfe9c93574fc0b99
SV=8852710DB871A7587B9E722
# The test as a file of the tree (F), as a zfs-tests.sh -t argument (R)
# and by name (T); the git blobs of master's file and of the fixed one.
F=tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
R=tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
T=zpool_add_warn_removal
BM=1591658ba7898aad7a121595da768c0a2215ac73
BF=7984f12bdfa6e3c5baa79f993311fbd763e8970d
# The runner, whose results this task sends to $A/results.
TR=tests/test-runner/bin/test-runner.py
# What master's test leaves in $FD, in ls order.
V10="vdev0 vdev1 vdev2 vdev3 vdev4 vdev5 vdev6 vdev7 vdev8 vdev9"
# The pid file of the tree's zed, which add_nested_replacing_spare starts.
ZP=$(grep -aoE '/[[:alnum:]/._-]+/zed[.]pid' $W/M/.libs/zed 2>/dev/null | head -n 1)
# The tunable that add-o_ashift and add_prop_ashift change and restore.
TUN=vdev_file_physical_ashift
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

# A step that stops records why in $A/STOP; no run starts after it.
stop() {   # stop <reason>
	echo "$*: stop" | tee -a $A/runs.txt
	[ -e $A/STOP ] || echo "$*" > $A/STOP
	return 1
}
stopped() {
	[ -e $A/STOP ] || return 1
	echo "stopped earlier ($(cat $A/STOP)): nothing run" | tee -a $A/runs.txt
}

tunables() {
	local t
	for t in $TUN; do
		printf '%s=%s ' $t "$(cat /sys/module/zfs/parameters/$t 2>&1)"
	done
}

loops() {
	sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1
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
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[z]inject ' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Result counts of runner result lines on stdin: "PASS x19", "FAIL x1, PASS x18".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# The test's lines in a runner log: its pool destroys (108 when it runs to
# its end), the cleanup note, its cleanup's losetup -d, log_pass's message,
# ERROR lines, and its last line.
digest() {   # digest <runner log>
	awk -v n="$T" '
	function flush() {
		if (sec)
			printf "  %s: %d pool destroys, cleanup line x%d, losetup -d x%d, log_pass message x%d, ERROR lines x%d\n    last line: %s\n", n, des, cln, det, pas, err, prev
		sec = 0
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		des = 0; cln = 0; det = 0; pas = 0; err = 0; prev = ""
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	line ~ /SUCCESS: zpool destroy -f testpool1$/ { des++ }
	line ~ /NOTE: Performing local cleanup via log_onexit [(]zpool_create_add_cleanup[)]$/ { cln++ }
	line ~ /SUCCESS: losetup -d [/]dev[/]loop[0-9]+$/ { det++ }
	line ~ /^Verify .zpool add. warns for differing redundancy[.]$/ { pas++ }
	line ~ /ERROR: / { err++ }
	{ prev = line }
	END { flush() }' "$1"
}

expected_digest() {   # expected_digest <leak|none>: digest's first line
	local x=1
	[ "$1" = leak ] && x=0
	echo "  $T: 108 pool destroys, cleanup line x$x, losetup -d x$x, log_pass message x$x, ERROR lines x0"
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

# Kernel messages since <since>, from the journal. A finding stops; a panic,
# VERIFY or hung task also keeps the module loaded (PANIC).
kern_check() {   # kern_check <label> <since> <expected ZTS marks>
	local n m
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $A/kern-$1.txt 2>&1
	n=$(grep -iE "$KRE" $A/kern-$1.txt | grep -vcE "$KIGN")
	m=$(grep -c 'ZTS run ' $A/kern-$1.txt)
	echo "kernel $1: $n findings, $m ZTS marks (expected $3)" | tee -a $A/runs.txt
	grep -iE "$KRE" $A/kern-$1.txt | grep -vE "$KIGN" | head -n 20 | sed 's/^/  /' | tee -a $A/runs.txt
	if grep -iE "$KRE" $A/kern-$1.txt | grep -vE "$KIGN" |
	    grep -qiE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than'; then
		touch $A/PANIC
	fi
	if [ "$n" -ne 0 ]; then
		stop "$1: kernel finding"
		return 1
	fi
	if [ "$3" -gt 0 ] && [ "$m" -eq 0 ]; then
		stop "$1: no ZTS mark in the journal, so its kernel check shows nothing"
		return 1
	fi
}

# Before a run: nothing stopped, a new label, this tree's module, and none of
# what a run can leave: pool, loop device, file in $FD, test mount or mount
# point, zed, vdev_id.conf, a changed tunable.
pre_run() {   # pre_run <label>
	local cur p l f m r z v t
	stopped && return 1
	if [ -e $A/$1.log ]; then
		echo "label $1 already used: nothing run" | tee -a $A/runs.txt
		return 1
	fi
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$cur" != "$SV" ]; then
		stop "$1: the loaded srcversion is [$cur], not $SV"
		return 1
	fi
	p=$(sudo -n $W/M/zpool list -H -o name 2>&1)
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	r=$(ls -d /testpool* 2>/dev/null | xargs)
	z=$(pgrep -ax 'zed|lt-zed')
	v=$(ls -l /etc/zfs/vdev_id.conf 2>/dev/null)
	t=$(tunables)
	echo "before $1: pools=[$p] loops=[$l] files=[$f] mounts=[$m] rootdirs=[$r] zed=[$z] vdev_id.conf=[$v] $t" |
	    tee -a $A/runs.txt
	if [ -n "$p$l$f$m$r$z$v" ] || [ "$t" != "$(cat $A/tunables-loaded.txt)" ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: what it left against <expected>. "leak" is exactly what
# master's test leaves (one loop device, bound to $FD/vdev4, and vdev0 to
# vdev9 in $FD) with nothing else. A pool, device, mount or process may hold
# the module (HELD: STEP 5 does not unload).
leftovers() {   # leftovers <label> <leak|none>
	local bad=0 held=0 p l f got
	{
	p=$(sudo -n $W/M/zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; held=1; }
	p=$(sudo -n dmsetup ls 2>&1)
	[ "$p" = "$(cat $A/dm-before.txt)" ] || { echo "  LEFTOVER dm devices: $p"; held=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; held=1; }
	p=$(ls -d /testpool* 2>/dev/null | xargs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; held=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; held=1; }
	[ -n "$ZP" ] && [ -e "$ZP" ] && { echo "  LEFTOVER $ZP"; bad=1; }
	if [ -e /etc/zfs/vdev_id.conf ] || [ -L /etc/zfs/vdev_id.conf ]; then
		echo "  LEFTOVER $(ls -l /etc/zfs/vdev_id.conf)"; bad=1
	fi
	p=$(tunables)
	[ "$p" = "$(cat $A/tunables-loaded.txt)" ] || { echo "  LEFTOVER tunables: $p"; bad=1; }
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	echo "  loop devices: [$(printf '%s' "$l" | tr '\n' ';')]"
	echo "  files in $FD: [$f]"
	if [ -z "$l$f" ]; then
		got=none
	elif [ "$f" = "$V10" ] && [ "$(printf '%s\n' "$l" | grep -c .)" -eq 1 ] &&
	    [ "$(printf '%s\n' "$l" | awk -v v="$FD/vdev4" \
	    'NF == 2 && $1 ~ /^[/]dev[/]loop[0-9]+$/ && $2 == v { print "yes" }')" = yes ]; then
		got=leak
	else
		got=other
	fi
	if [ $bad$held = 00 ] && [ "$got" = "$2" ]; then
		if [ $got = leak ]; then
			echo "leftovers after $1: the leak of master's test, as expected"
		else
			echo "leftovers after $1: none"
		fi
	else
		echo "leftovers after $1: FOUND ($got, expected $2)"
	fi
	} > $A/left-$1.txt 2>&1
	tee -a $A/runs.txt < $A/left-$1.txt
	if [ $bad$held = 00 ] && [ "$got" = "$2" ]; then
		return 0
	fi
	if [ $held = 1 ] || [ "$got" != none ]; then
		touch $A/HELD
	fi
	stop "$1: leftovers"
}

post_run() {   # post_run <label> <since> <rc> <expected ZTS marks> <leak|none>
	local bad=0
	kern_check $1 "$2" $4 || bad=1
	if [ $3 -eq 124 ] || [ $3 -eq 137 ]; then
		stop "$1: outer timeout"
		bad=1
	fi
	leftovers $1 $5 || bad=1
	return $bad
}

# One run of the test (-t: the directory's setup, the test, its cleanup),
# checked against <expected leftovers>: leak on M, none on B.
one() {   # one <label> <leak|none>
	local label=$1 since t0 rc res np d dg bad=0
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/M && timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $FD -t $R) > $A/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $A/$label.log > $A/$label-tests.txt
	res=$(grep -E "/$T([.]ksh)? [(]run as " $A/$label-tests.txt | tally)
	echo "$label $T ${res:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $A/runs.txt
	np=$(grep -vE "/$T([.]ksh)? [(]run as " $A/$label-tests.txt | grep -v '\[PASS\]')
	[ -z "$np" ] || printf '%s\n' "$np" | sed 's/^/  not passed: /' | tee -a $A/runs.txt
	d=$(awk '/^Log directory/ { print $3 }' $A/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		dg=$(digest "$d/log")
	else
		dg="  no runner log (Log directory: '$d')"
	fi
	printf '%s\n' "$dg" | tee -a $A/runs.txt
	if [ "$res" != "PASS x1" ] && [ -f "$d/log" ]; then
		fails "$d/log" $T | tee -a $A/runs.txt
	fi
	post_run $label "$since" $rc 3 $2 || bad=1
	if [ "$res" != "PASS x1" ] || [ $rc -ne 0 ] || [ -n "$np" ] ||
	    [ "$(printf '%s\n' "$dg" | head -n 1)" != "$(expected_digest $2)" ]; then
		stop "$label: result or log not as expected"
		bad=1
	fi
	return $bad
}

# After an M run: detach the one loop device bound to $FD/vdev4 and remove
# vdev0 to vdev9 from $FD, only when that is all there is.
release() {   # release <label>
	local l lo f i rc1 rc2
	stopped && return 1
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	lo=$(printf '%s\n' "$l" | awk -v v="$FD/vdev4" \
	    'NF == 2 && $1 ~ /^[/]dev[/]loop[0-9]+$/ && $2 == v { print $1 }')
	if [ "$(printf '%s\n' "$l" | grep -c .)" -ne 1 ] || [ -z "$lo" ] || [ "$f" != "$V10" ] ||
	    [ -n "$(sudo -n $W/M/zpool list -H -o name 2>&1)" ]; then
		touch $A/HELD
		stop "release after $1: not exactly master's leak"
		return 1
	fi
	sudo -n losetup -d "$lo"
	rc1=$?
	sudo -n rm -f -- $(for i in 0 1 2 3 4 5 6 7 8 9; do printf '%s ' "$FD/vdev$i"; done)
	rc2=$?
	for i in 1 2 3 4 5; do
		[ -z "$(loops)" ] && break
		sleep 1
	done
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	echo "released after $1: losetup -d $lo rc=$rc1, rm of the 10 files rc=$rc2;" \
	    "loop devices now [$l], files now [$f]" | tee -a $A/runs.txt
	if [ $rc1 -ne 0 ] || [ $rc2 -ne 0 ] || [ -n "$l$f" ]; then
		touch $A/HELD
		stop "release after $1: something is left"
		return 1
	fi
}

# The zpool_add group once (-T zpool_add): 16 "Test:" lines (common.run)
# and 3 "Test (Linux):" lines (linux.run). zpool_add_004_pos failing alone
# on a missing /dev/zvol link is this VM's udev, and accepted.
group() {   # group <label>
	local label=$1 since t0 rc c nt nl np d dg n ok bad=0
	pre_run $label || return 1
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/M && timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $FD -T zpool_add) > $A/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $A/$label.log > $A/$label-tests.txt
	c=$(tally < $A/$label-tests.txt)
	nt=$(grep -c 'Test: ' $A/$label-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $A/$label-tests.txt)
	echo "$label -T zpool_add ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl) rc=$rc" \
	    "wall=$(( $(date +%s) - t0 ))s" | tee -a $A/runs.txt
	grep -m 1 '^Missing util' $A/$label.log | sed 's/^/  /' | tee -a $A/runs.txt
	grep -v '\[PASS\]' $A/$label-tests.txt | sed 's/^/  not passed: /' | tee -a $A/runs.txt
	np=$(grep -v '\[PASS\]' $A/$label-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	d=$(awk '/^Log directory/ { print $3 }' $A/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		dg=$(digest "$d/log")
		for n in $np; do fails "$d/log" $n; done > $A/$label-fails.txt
	else
		dg="  no runner log (Log directory: '$d')"
		: > $A/$label-fails.txt
	fi
	printf '%s\n' "$dg" | tee -a $A/runs.txt
	tee -a $A/runs.txt < $A/$label-fails.txt
	post_run $label "$since" $rc 19 none || bad=1
	ok=no
	if [ "$c" = "PASS x19" ] && [ $rc -eq 0 ]; then
		ok=yes
	elif [ "$c" = "FAIL x1, PASS x18" ] && [ "$np" = zpool_add_004_pos ] &&
	    grep -q 'first ERROR line: .*/dev/zvol/testpool1/testvol' $A/$label-fails.txt; then
		ok=zvol
		echo "  zpool_add_004_pos failed on the missing /dev/zvol/testpool1/testvol link:" \
		    "this VM's udev, not the fix" | tee -a $A/runs.txt
	fi
	if [ $ok = no ] || [ "$nt" != 16 ] || [ "$nl" != 3 ] ||
	    [ "$(printf '%s\n' "$dg" | head -n 1)" != "$(expected_digest none)" ]; then
		stop "$label: results not as expected"
		bad=1
	fi
	return $bad
}
ENV
bash -n /var/log/zts-algo-1026/addwarn/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-algo-1026/addwarn/env.sh
date '+%F %T' > $A/start.txt
sudo -n dmsetup ls > $A/dm-before.txt 2>&1
{
echo "start $(cat $A/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
echo "files dir: $(stat -c '%U %a' $FD), $(ls -A $FD | wc -l) entries"
echo "zed pid file: ${ZP:-none}; its directory: $(ls -ld "${ZP%/*}" 2>&1)"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
z=$(ls /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules \
    /etc/udev/rules.d/60-zvol.rules 2>/dev/null | xargs)
echo "60-zvol.rules: ${z:-none}"
echo "dmsetup ls: $(cat $A/dm-before.txt)"
echo "free: $(df -h /var/tmp | tail -n 1)"
echo "journal, this boot, before this task: $(sudo -n journalctl -k -b --no-pager -q |
    grep -iE "$KRE" | grep -vcE "$KIGN") findings"
} 2>&1 | tee $A/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then

```
start <date> (<time> UTC); tigerblue77; 6.12.113+deb13-cloud-amd64; umask 0022
files dir: tigerblue77 1777, 0 entries
zed pid file: /usr/local/var/run/zed.pid; its directory: <its ls -ld, or No such file or directory>
zpool.cache: <as in STEP 0>
60-zvol.rules: <path(s), or none>
dmsetup ls: No devices found
free: <the df line>
journal, this boot, before this task: <n> findings
```

The baseline count is only recorded: each run reads its own window. Then
point the runner's results at `$A/results`, keeping the two files as they are
for STEP 5:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
{
cd $W/M
cp -p $TR.in $A/orig/test-runner.py.in && cp -p $TR $A/orig/test-runner.py; echo "runner saved rc=$?"
sha256sum < $TR.in | cut -d' ' -f1 > $A/orig/sha-in
sha256sum < $TR | cut -d' ' -f1 > $A/orig/sha-py
same "saved $TR.in is HEAD's" "$(git hash-object $A/orig/test-runner.py.in)" "$(git rev-parse HEAD:$TR.in)"
sed -i "s|^BASEDIR = .*|BASEDIR = '$A/results'|" $TR.in $TR
grep -n '^BASEDIR' $TR.in $TR
same "results directory" "$(grep -cx "BASEDIR = '$A/results'" $TR)" 1
same "$TR mode" "$(stat -c %a $TR)" "$(stat -c %a $A/orig/test-runner.py)"
} 2>&1 | tee $A/setup.txt
grep -q '^MISMATCH' $A/setup.txt && stop "the results redirection"
```

Expected:

```
runner saved rc=0
ok: saved tests/test-runner/bin/test-runner.py.in is HEAD's
tests/test-runner/bin/test-runner.py.in:43:BASEDIR = '/var/log/zts-algo-1026/addwarn/results'
tests/test-runner/bin/test-runner.py:43:BASEDIR = '/var/log/zts-algo-1026/addwarn/results'
ok: results directory
ok: tests/test-runner/bin/test-runner.py mode
```

Load the tree's modules:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
if ! stopped; then
	{
	since=$(date '+%F %T')
	(cd $W/M && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh); echo "load rc=$?"
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" $SV
	same "the tree's zfs.ko" "$(modinfo -F srcversion $W/M/module/zfs.ko)" $SV
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV" ]; then
		same "pools after the load" "$(sudo -n $W/M/zpool list -H -o name 2>&1)" ""
		tunables > $A/tunables-loaded.txt
		echo "after the load: $(cat $A/tunables-loaded.txt)spl_panic_halt=$(cat /sys/module/spl/parameters/spl_panic_halt)"
	fi
	kern_check load "$since" 0
	} 2>&1 | tee -a $A/setup.txt
	grep -q '^MISMATCH' $A/setup.txt && { touch $A/HELD; stop "the module load"; }
fi
```

Expected:

```
load rc=0
ok: loaded srcversion
ok: the tree's zfs.ko
ok: pools after the load
after the load: vdev_file_physical_ashift=9 spl_panic_halt=0
kernel load: 0 findings, 0 ZTS marks (expected 0)
```

(9 is the module's default; another value set by this VM's configuration is
fine, since every run is compared with what the load found.)

## STEP 2: state M, master's test, three runs

Run each block only after the previous one ended with its `released` line:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one m1 leak && release m1
```

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one m2 leak && release m2
```

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one m3 leak && release m3
```

Expected, each (`<N>` is the loop device the test took: 3 when ZTS's own are
0 to 2):

```
before m1: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] vdev_file_physical_ashift=9
m1 zpool_add_warn_removal PASS x1 rc=0 wall=<s>s
  zpool_add_warn_removal: 108 pool destroys, cleanup line x0, losetup -d x0, log_pass message x0, ERROR lines x0
    last line: SUCCESS: zpool destroy -f testpool1
kernel m1: 0 findings, 3 ZTS marks (expected 3)
  loop devices: [/dev/loop<N> /var/tmp/zts-algo-1026/files-addwarn/vdev4]
  files in /var/tmp/zts-algo-1026/files-addwarn: [vdev0 vdev1 vdev2 vdev3 vdev4 vdev5 vdev6 vdev7 vdev8 vdev9]
leftovers after m1: the leak of master's test, as expected
released after m1: losetup -d /dev/loop<N> rc=0, rm of the 10 files rc=0; loop devices now [], files now []
```

This is the finding: the run passes, the test reached its end (108
destroys), its cleanup never ran (no cleanup line, no `losetup -d`), and it
left exactly the loop device and the ten files.

## STEP 3: state B, the fix's test file, three runs and the group

Fetch the fix commit into the tree's repository and check it. This is
read-only towards GitHub:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
if ! stopped; then
	{
	cd $W/M
	git -c gc.auto=0 -c maintenance.auto=false fetch -q --no-tags $FORK $FIXBRANCH \
	    > $A/fetch.log 2>&1; echo "fetch rc=$?"
	tail -n 3 $A/fetch.log
	same "$FIXBRANCH" "$(git rev-parse -q --verify FETCH_HEAD)" $HF
	same "its parent" "$(git rev-parse -q --verify "$HF^")" $HM
	same "what it changes" "$(git diff --numstat $HM $HF | xargs)" "2 0 $F"
	same "its test file" "$(git rev-parse -q --verify $HF:$F)" $BF
	echo "lines it adds or removes:"
	git diff -U0 $HM $HF -- $F | grep -E '^[+-]' | grep -vE '^(\+\+\+|---) '
	} 2>&1 | tee $A/fix.txt
fi
```

Expected:

```
fetch rc=0
ok: zpool-add-warn-removal-log-pass
ok: its parent
ok: what it changes
ok: its test file
lines it adds or removes:
+
+log_pass "Verify 'zpool add' warns for differing redundancy."
```

A `MISMATCH` (the branch moved) or a failed fetch means stop. Otherwise put
the fix's test file in M, and only that:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
if ! stopped; then
	if grep -q '^MISMATCH' $A/fix.txt || ! grep -qx 'fetch rc=0' $A/fix.txt; then
		stop "the fix commit is not as expected"
	else
		{
		cd $W/M
		git checkout $HF -- $F; echo "checkout rc=$?"
		same "M: the test" "$(git hash-object $F)" $BF
		same "M: its mode" "$(stat -c %a $F)" 755
		same "M: files changed against HEAD" "$(git diff HEAD --name-only | xargs)" "$TR.in $F"
		same "M: the test against HEAD" "$(git diff HEAD --numstat -- $F | xargs)" "2 0 $F"
		same "M: its last line" "$(tail -n 1 $F)" \
		    "log_pass \"Verify 'zpool add' warns for differing redundancy.\""
		} 2>&1 | tee -a $A/fix.txt
		grep -q '^MISMATCH' $A/fix.txt && stop "the test file in M is not the fix's"
	fi
fi
```

Expected: `checkout rc=0` and five `ok:` lines. The only other change against
HEAD is STEP 1's `BASEDIR`. Then the three runs, one block at a time:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one b1 none
```

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one b2 none
```

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
one b3 none
```

Expected, each:

```
before b1: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] vdev_file_physical_ashift=9
b1 zpool_add_warn_removal PASS x1 rc=0 wall=<s>s
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
    last line: SUCCESS: losetup -d /dev/loop<N>
kernel b1: 0 findings, 3 ZTS marks (expected 3)
  loop devices: []
  files in /var/tmp/zts-algo-1026/files-addwarn: []
leftovers after b1: none
```

Then the group, once (5 to 15 minutes; see the last rule):

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
group bg
```

Expected:

```
before bg: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] vdev_file_physical_ashift=9
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
    last line: SUCCESS: losetup -d /dev/loop<N>
kernel bg: 0 findings, 19 ZTS marks (expected 19)
  loop devices: []
  files in /var/tmp/zts-algo-1026/files-addwarn: []
leftovers after bg: none
```

If STEP 1 found no `60-zvol.rules`, expect instead
`bg -T zpool_add FAIL x1, PASS x18 (Test: x16, Test (Linux): x3) rc=1`, one
`not passed:` line for `zpool_add_004_pos`, its
`first ERROR line: ERROR: zpool add ... /dev/zvol/testpool1/testvol exited <n>`,
and the line `zpool_add_004_pos failed on the missing
/dev/zvol/testpool1/testvol link: this VM's udev, not the fix`. Any other
failure, or a count other than 16 and 3, means stop.

## STEP 4: what the results mean

- **As expected** (M: PASS 3/3, 108 destroys, no cleanup line, the leak after
  each run; B: PASS 3/3, 108 destroys, the cleanup line, `losetup -d`, the
  `log_pass` message, nothing left; `bg` as above, nothing left; no kernel
  finding): on a kernel, master's `zpool_add_warn_removal` passes while
  leaving a loop device and ten files after every run, and the fix's
  `log_pass` makes its cleanup run without changing its result. The
  `-T zpool_add` group with the fix leaves nothing.
- **M leaves nothing, or prints the cleanup line**: the leak does not happen
  with this tree. The commit's description must not claim it.
- **M fails**: `log_fail` ran the cleanup, and the run says nothing about the
  leak. Report its `first ERROR line`, which `one` prints for a run that did
  not pass.
- **B leaves something, or lacks the cleanup line or `losetup -d`**: the fix
  does not do what it says. Report the lines.
- **A `bg` test other than the accepted `zpool_add_004_pos` fails**: report it
  with its `first ERROR line`. VM 105 passed all 19 on 26 Oct (with the leak),
  so it is this VM's or this tree's, not the fix's, unless it is
  `zpool_add_warn_removal` itself.
- **Any kernel finding**: the line and the run are the result. Stop.

## STEP 5: restore, unload, cleanup, report

Run these three blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`, which changed nothing. First, the whole task's
kernel check, and the test file and runner restored:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
{
sudo -n journalctl -k --since "$(cat $A/start.txt)" --no-pager -q -o short-iso > $A/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $A/kern-all.txt | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $A/kern-all.txt) ZTS marks"
grep -iE "$KRE" $A/kern-all.txt | grep -vE "$KIGN" | head -n 40
cd $W/M
git checkout HEAD -- $F; echo "test restored rc=$?"
cp -p $A/orig/test-runner.py.in $TR.in && cp -p $A/orig/test-runner.py $TR; echo "runner restored rc=$?"
same "the test is master's" "$(git hash-object $F)" $BM
same "its mode" "$(stat -c %a $F)" 755
same "$TR.in as found" "$(sha256sum < $TR.in | cut -d' ' -f1)" "$(cat $A/orig/sha-in)"
same "$TR as found" "$(sha256sum < $TR | cut -d' ' -f1)" "$(cat $A/orig/sha-py)"
same "tracked changes in M" "$(git status --short --untracked-files=no | wc -l)" 0
same "M HEAD" "$(git rev-parse HEAD)" $HM
} 2>&1 | tee $A/end.txt
```

Expected: `kernel, whole task: 0 findings, 37 ZTS marks` (3 per run, 19 for
`bg`; fewer after a stop), no kernel line, `test restored rc=0`,
`runner restored rc=0`, six `ok:` lines.

Then unload the module and remove this task's files directory and results.
The module stays loaded if a run left something that can hold it (`HELD`) or
the kernel reported a panic, VERIFY or hung task (`PANIC`). Both directories
stay if a step stopped, for the record:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
{
if [ -e $A/HELD ] || [ -e $A/PANIC ]; then
	echo "module left loaded: $(cd $A && ls HELD PANIC 2>/dev/null | xargs)"
elif [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$SV" ]; then
	(cd $W/M && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u); echo "unload rc=$?"
else
	echo "no module of this tree is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
if [ -e $A/STOP ]; then
	echo "kept: $FD and $A/results (stopped: $(cat $A/STOP))"
else
	rmdir $FD; echo "rmdir files dir rc=$?"
	rm -rf /var/log/zts-algo-1026/addwarn/results; echo "rm results rc=$?"
fi
ls -d $FD $A/results 2>&1
echo "loop devices: [$(loops)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
echo "zed pid directory: $(ls -ld "${ZP%/*}" 2>&1), $(ls -A "${ZP%/*}" 2>/dev/null | wc -l) entries"
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $A/end.txt
```

Expected: `unload rc=0`, `lsmod: []`, `rmdir files dir rc=0`,
`rm results rc=0`, `No such file or directory` twice, `loop devices: []`,
`zpool.cache` 0 bytes (or absent if `/etc/zfs` is missing),
`vdev_id.conf: ... No such file or directory`, the `zed` pid directory as
STEP 1 found it, or newly there with 0 entries (the group's `zed` made it:
leave it), `W holds: M repo`. The tree, `repo`, the build logs and `$A` stay.

Last, assemble `REPORT.txt`:

```sh
. /var/log/zts-algo-1026/addwarn/env.sh
{
echo "VM 107, zpool_add_warn_removal cleanup: master a128b8a075 against the log_pass fix 4e8a517729 (VM-TESTS-107-addwarn.md)"
for s in record setup fix runs end; do
	echo
	echo "== $s.txt"
	cat $A/$s.txt 2>&1
done
echo
echo "== summary"
for l in m1 m2 m3 b1 b2 b3 bg; do
	awk -v l="$l" -v t="  $T: " '
	index($0, l " ") == 1 { print; s = 1; next }
	s && index($0, t) == 1 { print; s = 0 }
	index($0, "kernel " l ":") == 1 || index($0, "leftovers after " l ":") == 1 ||
	    index($0, "released after " l ":") == 1 { print }' $A/runs.txt
done
echo "flags: STOP [$(cat $A/STOP 2>/dev/null)] HELD [$(ls $A/HELD 2>/dev/null)] PANIC [$(ls $A/PANIC 2>/dev/null)]"
} > $A/REPORT.txt 2>&1
wc -l $A/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, and the three
recorded-only values), a `verdict:` line naming the STEP 4 case that applies,
and a `deviations:` line with every deviation from these steps and its
reason. Then `cat /var/log/zts-algo-1026/addwarn/REPORT.txt` and reply with
all of it.

## REPORT.txt expected (summary part)

```
== summary
m1 zpool_add_warn_removal PASS x1 rc=0 wall=<s>s
  zpool_add_warn_removal: 108 pool destroys, cleanup line x0, losetup -d x0, log_pass message x0, ERROR lines x0
kernel m1: 0 findings, 3 ZTS marks (expected 3)
leftovers after m1: the leak of master's test, as expected
released after m1: losetup -d /dev/loop<N> rc=0, rm of the 10 files rc=0; loop devices now [], files now []
<m2 and m3: the same>
b1 zpool_add_warn_removal PASS x1 rc=0 wall=<s>s
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
kernel b1: 0 findings, 3 ZTS marks (expected 3)
leftovers after b1: none
<b2 and b3: the same>
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
kernel bg: 0 findings, 19 ZTS marks (expected 19)
leftovers after bg: none
flags: STOP [] HELD [] PANIC []
verdict: as expected (STEP 4, first case)
deviations: none
```

Estimated duration: about 40 minutes (30 to 50). Checks and setup take 5,
the six runs of the test 2 to 4 each, the group 5 to 15, and the end 3.
