# VM 107, phase C: the removal and zpool_add groups with the whole branch

These instructions are for the session on VM 107. They follow
`h/VM-TESTS-107-vip.md`, which ran here. With `1ae75ec365`'s files,
`-T zpool_add` passed 19 of 19 and left nothing. `-T removal` passed 36 with
1 SKIP and left no config `tmp.*` file. It stopped on two more leaks of
master's tests, in its own run:

- `zfs_remove_max_segment` was left at 1048576: `removal_multiple_indirection`'s
  cleanup hard-codes 1M, and the module's default is `SPA_MAXBLOCKSIZE`,
  16777216.
- An empty, root-owned `testdir1` was left in `files-vip`:
  `removal_condense_export`'s `reset` calls only `default_cleanup_noexit`,
  which removes `$TESTDIR`, not `$TESTDIR1`.

The module was unloaded afterwards. `files-vip` (holding `testdir1`) and
`vip/results` were kept.

The fork branch `tigerblue77/zfs:zpool-add-warn-removal-log-pass` is now
`147a85264cb49fa1e2c94cba2df167de54946203`, five commits on master
`a128b8a075`, one file each:

| Commit | Change | File |
| --- | --- | --- |
| `4e8a517729942d1ac159eb49cfe9c93574fc0b99` | `log_pass` at the end | `cli_root/zpool_add/zpool_add_warn_removal.ksh` (+2) |
| `1ae75ec36558291eb3bd469cda999ec33f76d97e` | `vdevs_in_pool()` removes its file before returning 1 | `include/libtest.shlib` (+4 -1) |
| `e0725c9ae214b22f0f43e985a567306688df5ae9` | `mktemp` after the Linux `log_unsupported` | `removal/removal_with_zdb.ksh` (+2 -2) |
| `56b4b06599cd8d2a8f40d2bb8de054ede3ab6c4c` | saves `zfs_remove_max_segment` and restores that, not 1M | `removal/removal_multiple_indirection.ksh` (+2 -2) |
| `147a85264cb49fa1e2c94cba2df167de54946203` | `rm -rf $TESTDIR1` in `reset` | `removal/removal_condense_export.ksh` (+1) |

This task swaps the branch's five files into the master tree
`/var/tmp/zts-algo-1026/M` (srcversion `8852710DB871A7587B9E722`). It runs
`-T removal` once (`br`), then `-T zpool_add` once (`bg`), expecting both to
pass and to leave **nothing**. That means no `tmp.*` of any kind
(`removal_with_zdb` no longer creates one on Linux), no `testdir1`, no loop
device, and every tunable at its post-load value after each group, with
`zfs_remove_max_segment` back at 16777216. Nothing is built: all five files
are test scripts, which the in-tree runner reads from the source tree.

The other hard-coded resets in the removal tests are the module's defaults:
`REMOVAL_SUSPEND_PROGRESS` and `SCAN_SUSPEND_PROGRESS` 0,
`CONDENSE_INDIRECT_COMMIT_ENTRY_DELAY_MS` 0, `CONDENSE_INDIRECT_OBSOLETE_PCT`
25, `CONDENSE_MIN_MAPPING_BYTES` 131072. `metaslab_force_ganging` is saved and
restored.

## Rules

- Log in as `tigerblue77` and run everything as that user, in bash, in this
  order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: module loads and every run go
  through `nice -n 10 ionice -c3` (the blocks do it), one test at a time,
  nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing.
- This task's own directories are `/var/tmp/zts-algo-1026/files-vipc` (`$FD`)
  and `/var/log/zts-algo-1026/vip-c` (`$A`). Outside them, change only these.
  In `/var/tmp/zts-algo-1026`: STEP 0 releases the earlier task's
  `files-vip` (its empty `testdir1`, then the directory). In
  `/var/tmp/zts-algo-1026/M`: the five files (swapped in STEP 2, restored in
  STEP 5) and the runner's `BASEDIR` (redirected in STEP 1, restored in
  STEP 5). In the repository: what STEP 2's fetch adds. Leave the tree,
  `repo`, `/var/log/zts-algo-1026/addwarn`, `/var/log/zts-algo-1026/vip` (its
  `results` included), the build logs and every other directory as they are.
  Never create, edit or remove `/etc/zfs/zpool.cache`,
  `/etc/zfs/vdev_id.conf` or the `zed` pid file yourself.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only these: STEP 0's `rmdir` of
  `files-vip/testdir1` and `files-vip`, and STEP 5's `rmdir` of `$FD`, its
  `rm -rf` of `$A/results`, and the final `rm -rf` of `$A`.
- Never run a `zpool` or `zfs` command while this tree's module is not loaded.
  Never print the environment (`env`, `printenv`, `set`).
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-algo-1026/vip-c/env.sh`.
- Stop at any value the comments or Expected lines do not allow. After a line
  ending in `stop`, run only STEP 5; the helpers refuse to run once a step has
  stopped. Clean, repair and retry nothing by hand. If your permission system
  refuses a command, report it and stop. A kernel panic, oops, VERIFY or hung
  task means stop and report, never reboot.
- `br` takes 8 to 20 minutes and `bg` 5 to 12. If your shell tool cannot wait
  that long in the foreground, start that one block with the tool's own
  background mode and wait for it to exit. Never use `&` or `nohup`, and run
  nothing meanwhile.

What earlier VM runs taught, and the blocks already handle: results are
matched anywhere in a `Test:` line, which starts with a timestamp. A `-T` run
names tests without `.ksh`, and `linux.run` tests are `Test (Linux): ...`. A
literal parenthesis in a regex is `[(]`. The test's lines are read from the
runner's own `log` file. Kernel messages are read per run from
`journalctl -k --since`, without the `ZTS run` markers and
`spa_import_progress_set_notes_impl` lines. Each run waits 2 s first. The
`-d` directory has mode 1777.

## STEP 0: the VM free, the tree as the last task left it, and its release

```sh
export PATH=/usr/sbin:/sbin:$PATH
date -u '+%F %T'
id -un                                # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                              # 6.12.113+deb13-cloud-amd64
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
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
df -h /var/tmp                         # 8G free or more
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
command -v fio                         # recorded: without it remove_attach_mirror is SKIP
```

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-algo-1026 L=/var/log/zts-algo-1026
TR=tests/test-runner/bin/test-runner.py
stat -c '%U %n' $W $W/M $W/repo $L     # tigerblue77, four times
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"   # W holds: M files-vip repo
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"   # addwarn, vip and the build's logs; no vip-c
echo "vip flags: $(cd $L/vip && ls STOP HELD PANIC 2>/dev/null | xargs); stopped on: $(cat $L/vip/STOP 2>&1)"
# vip flags: STOP; stopped on: <the br leftovers>
cd $W/M
git rev-parse HEAD                     # a128b8a075b5b13c737e56914cd7043bd3e9ebf8
echo "tracked changes: $(git status --short --untracked-files=no | wc -l)"   # tracked changes: 0
for f in include/libtest.shlib tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh \
    tests/functional/removal/removal_condense_export.ksh \
    tests/functional/removal/removal_multiple_indirection.ksh \
    tests/functional/removal/removal_with_zdb.ksh; do
	echo "$(git hash-object tests/zfs-tests/$f) $(stat -c %a tests/zfs-tests/$f) ${f##*/}"
done
# 629eb52aaeba514198a8d4573561b5eed10ab31a 644 libtest.shlib
# 1591658ba7898aad7a121595da768c0a2215ac73 755 zpool_add_warn_removal.ksh
# aa0d5e2cfde5b537ef5653d65e10ccf90f8e9b34 755 removal_condense_export.ksh
# cfb08c364ac23d2d81425f43d197a093db7cbb2b 755 removal_multiple_indirection.ksh
# f486d8da44d9ad35f93b0bde2b09d7bacbe55dbe 755 removal_with_zdb.ksh
grep -n '^BASEDIR' $TR.in $TR          # line 43 of both: BASEDIR = '/var/tmp/test_results'
echo "srcversion $(modinfo -F srcversion module/zfs.ko), vermagic $(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)"
# srcversion 8852710DB871A7587B9E722, vermagic <uname -r>
```

Stop at once, changing nothing, and report what you found if any value
differs from its comment. That covers another user, a failing `sudo -n`, a
module, `/dev/zfs`, a `zed`, a test or build process, a busy tmux pane, a loop
or device-mapper device, a zfs or test mount, a `/testpool*` directory, a
non-empty `zpool.cache`, a `vdev_id.conf`, and less than 8 GB free. It also
covers `$W` holding anything but `M files-vip repo`, `$L` holding `vip-c`, a
`HELD` or `PANIC` in `vip`, a tracked change, another blob or mode, `BASEDIR`
already redirected, and another srcversion or vermagic. The kernel and `fio`
lines are only recorded.

Then release what the earlier task's stopped run left, and only that: the
empty `testdir1` (root's, not a mount point) in `files-vip`, then
`files-vip` itself:

```sh
export PATH=/usr/sbin:/sbin:$PATH
V=/var/tmp/zts-algo-1026/files-vip
echo "files-vip: $(stat -c '%U %a' $V), holds: [$(LC_ALL=C ls -A $V | xargs)]"   # tigerblue77 1777, holds: [testdir1]
echo "testdir1: $(stat -c '%F %U %a' $V/testdir1), $(sudo -n ls -A $V/testdir1 | wc -l) entries"   # directory root <mode>, 0 entries
echo "mounts under files-vip: $(grep -c " $V" /proc/mounts)"   # mounts under files-vip: 0
if [ "$(LC_ALL=C ls -A $V | xargs)" = testdir1 ] && [ ! -L $V/testdir1 ] &&
    [ "$(stat -c '%F %U' $V/testdir1)" = "directory root" ] &&
    [ -z "$(sudo -n ls -A $V/testdir1)" ] && ! grep -q " $V" /proc/mounts; then
	sudo -n rmdir $V/testdir1 && rmdir $V; echo "released rc=$?"
else
	echo "files-vip is not exactly an empty testdir1: stop"
fi
ls -d $V 2>&1                          # No such file or directory
echo "W holds: $(LC_ALL=C ls -A /var/tmp/zts-algo-1026 | xargs)"   # W holds: M repo
```

Expected: the four commented lines, `released rc=0`, `No such file or
directory`, `W holds: M repo`. Anything else means stop.

## STEP 1: this task's environment, the results directory, the module

The block writes nothing unless it can create both directories.

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/log/zts-algo-1026/vip-c /var/log/zts-algo-1026/vip-c/orig &&
    mkdir -m 1777 /var/tmp/zts-algo-1026/files-vipc; then
echo "mkdir rc=0"
cat > /var/log/zts-algo-1026/vip-c/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-algo-1026
L=/var/log/zts-algo-1026
# This task's own directories: its logs and the runner's results (A), and
# the zfs-tests.sh -d directory (FD, mode 1777).
A=$L/vip-c
FD=$W/files-vipc
FORK=https://github.com/tigerblue77/zfs
FIXBRANCH=zpool-add-warn-removal-log-pass
# The tree's commit (master), the branch's head and its five commits in
# order, and the srcversion.
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
HF=147a85264cb49fa1e2c94cba2df167de54946203
C5="4e8a517729942d1ac159eb49cfe9c93574fc0b99 1ae75ec36558291eb3bd469cda999ec33f76d97e e0725c9ae214b22f0f43e985a567306688df5ae9 56b4b06599cd8d2a8f40d2bb8de054ede3ab6c4c 147a85264cb49fa1e2c94cba2df167de54946203"
SV=8852710DB871A7587B9E722
# The branch's five files (in git's order), their blobs on master (BM) and
# on the branch (BB) in the same order, and its patch-id against master.
Z=tests/zfs-tests
FILES="$Z/include/libtest.shlib $Z/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
$Z/tests/functional/removal/removal_condense_export.ksh
$Z/tests/functional/removal/removal_multiple_indirection.ksh
$Z/tests/functional/removal/removal_with_zdb.ksh"
FILES=$(echo $FILES)
BM="629eb52aaeba514198a8d4573561b5eed10ab31a 1591658ba7898aad7a121595da768c0a2215ac73 aa0d5e2cfde5b537ef5653d65e10ccf90f8e9b34 cfb08c364ac23d2d81425f43d197a093db7cbb2b f486d8da44d9ad35f93b0bde2b09d7bacbe55dbe"
BB="a7db244ef25c951ed6489806f79b85e744feec1b 7984f12bdfa6e3c5baa79f993311fbd763e8970d 6a73c26d363dff1657b8c87ace3815e901392ce2 1a0ef72aa0a20101f985465c678037a7886b77f0 6318906a1c555349d5a7ce3dbfdb8ad8d9a54b0e"
MODES="644 755 755 755 755"
NUMSTAT="4 1 $Z/include/libtest.shlib 2 0 $Z/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh 1 0 $Z/tests/functional/removal/removal_condense_export.ksh 2 2 $Z/tests/functional/removal/removal_multiple_indirection.ksh 2 2 $Z/tests/functional/removal/removal_with_zdb.ksh"
PID=2a60d27438c600412cec3cfa099c5d85ac902768
T=zpool_add_warn_removal
TR=tests/test-runner/bin/test-runner.py
# The pid file of the tree's zed, which add_nested_replacing_spare starts.
ZP=$(grep -aoE '/[[:alnum:]/._-]+/zed[.]pid' $W/M/.libs/zed 2>/dev/null | head -n 1)
# The tunables the zpool_add and removal groups change and must restore.
TUN="vdev_file_physical_ashift zfs_removal_suspend_progress zfs_remove_max_segment
zfs_scan_suspend_progress zfs_condense_indirect_commit_entry_delay_ms
zfs_condense_indirect_obsolete_pct zfs_condense_min_mapping_bytes metaslab_force_ganging"
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

# The blobs and modes of the branch's five files in M, in FILES's order.
blobs() {
	local f
	for f in $FILES; do
		printf '%s ' "$(git -C $W/M hash-object $f)"
	done | sed 's/ $//'
}
modes() {
	local f
	for f in $FILES; do
		printf '%s ' "$(stat -c %a $W/M/$f)"
	done | sed 's/ $//'
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

# Result counts of runner result lines on stdin: "PASS x19", "PASS x36, SKIP x1".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# zpool_add_warn_removal's lines in a runner log: its pool destroys (108 when
# it runs to its end), the cleanup note, its cleanup's losetup -d, log_pass's
# message, ERROR lines, and its last line.
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
# what a run can leave.
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
	t="tunables as loaded"
	[ "$(tunables)" = "$(cat $A/tunables-loaded.txt)" ] || t="TUNABLES CHANGED: $(tunables)"
	echo "before $1: pools=[$p] loops=[$l] files=[$f] mounts=[$m] rootdirs=[$r] zed=[$z] vdev_id.conf=[$v] $t" |
	    tee -a $A/runs.txt
	if [ -n "$p$l$f$m$r$z$v" ] || [ "$t" != "tunables as loaded" ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: nothing may be left. A pool, device, mount, process or loop
# device left may hold the module (HELD: STEP 5 does not unload).
leftovers() {   # leftovers <label>
	local bad=0 held=0 p l f
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
	if [ "$p" = "$(cat $A/tunables-loaded.txt)" ]; then
		echo "  tunables: as loaded ($(echo $p | tr ' ' '\n' | grep '^zfs_remove_max_segment='))"
	else
		echo "  LEFTOVER tunables: $p"; bad=1
	fi
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	echo "  loop devices: [$(printf '%s' "$l" | tr '\n' ';')]"
	echo "  files in $FD: [$f]"
	[ -n "$l" ] && held=1
	if [ -n "$f" ]; then
		sudo -n ls -la --time-style=+%T $FD | sed 's/^/    /'
		bad=1
	fi
	if [ $bad$held = 00 ]; then
		echo "leftovers after $1: none"
	else
		echo "leftovers after $1: FOUND"
	fi
	} > $A/left-$1.txt 2>&1
	tee -a $A/runs.txt < $A/left-$1.txt
	[ $bad$held = 00 ] && return 0
	[ $held = 1 ] && touch $A/HELD
	stop "$1: leftovers"
}

post_run() {   # post_run <label> <since> <rc> <expected ZTS marks>
	local bad=0
	kern_check $1 "$2" $4 || bad=1
	if [ $3 -eq 124 ] || [ $3 -eq 137 ]; then
		stop "$1: outer timeout"
		bad=1
	fi
	leftovers $1 || bad=1
	return $bad
}

# One tag group (-T <tag>), checked against <expected tally> and the counts of
# "Test:" and "Test (Linux):" lines; for zpool_add, also against the digest of
# zpool_add_warn_removal with its fix.
group() {   # group <label> <tag> <expected tally> <Test: lines> <Test (Linux): lines>
	local label=$1 tag=$2 exp=$3 ent=$4 enl=$5 since t0 rc c nt nl np d n dg="" bad=0
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/M && timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $FD -T $tag) > $A/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $A/$label.log > $A/$label-tests.txt
	c=$(tally < $A/$label-tests.txt)
	nt=$(grep -c 'Test: ' $A/$label-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $A/$label-tests.txt)
	echo "$label -T $tag ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl) rc=$rc" \
	    "wall=$(( $(date +%s) - t0 ))s" | tee -a $A/runs.txt
	grep -m 1 '^Missing util' $A/$label.log | sed 's/^/  /' | tee -a $A/runs.txt
	grep -v '\[PASS\]' $A/$label-tests.txt | sed 's/^/  not passed: /' | tee -a $A/runs.txt
	np=$(grep -v '\[PASS\]' $A/$label-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	d=$(awk '/^Log directory/ { print $3 }' $A/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		[ $tag = zpool_add ] && dg=$(digest "$d/log")
		for n in $np; do
			fails "$d/log" $n
		done | tee -a $A/runs.txt
	else
		echo "  no runner log (Log directory: '$d')" | tee -a $A/runs.txt
	fi
	[ -z "$dg" ] || printf '%s\n' "$dg" | tee -a $A/runs.txt
	post_run $label "$since" $rc $(( ent + enl )) || bad=1
	if [ "$c" != "$exp" ] || [ $rc -ne 0 ] || [ "$nt" != $ent ] || [ "$nl" != $enl ] ||
	    { [ $tag = zpool_add ] && [ "$(printf '%s\n' "$dg" | head -n 1)" != \
	    "  $T: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0" ]; }; then
		stop "$label: results not as expected"
		bad=1
	fi
	return $bad
}
ENV
bash -n /var/log/zts-algo-1026/vip-c/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-algo-1026/vip-c/env.sh
date '+%F %T' > $A/start.txt
sudo -n dmsetup ls > $A/dm-before.txt 2>&1
{
echo "start $(cat $A/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
echo "files dir: $(stat -c '%U %a' $FD), $(ls -A $FD | wc -l) entries"
echo "zed pid file: ${ZP:-none}; its directory: $(ls -ld "${ZP%/*}" 2>&1)"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "fio: $(command -v fio || echo missing)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $A/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then `start ...; tigerblue77;
6.12.113+deb13-cloud-amd64; umask 0022`, `files dir: tigerblue77 1777, 0
entries`, the `zed`, `zpool.cache`, `fio` and free-space lines. Then point the
runner's results at `$A/results`, keeping the two files as they are for
STEP 5:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
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

Expected: `runner saved rc=0`, `ok: saved ... is HEAD's`, the two `BASEDIR`
lines on line 43 with `'/var/log/zts-algo-1026/vip-c/results'`,
`ok: results directory`, `ok: ... mode`.

## STEP 2: the branch's five files in M

Fetch the branch into the tree's repository and pin it: its head, each of
its five commits and the file each changes, its blobs and its patch-id
(read-only towards GitHub):

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
if ! stopped; then
	{
	cd $W/M
	git -c gc.auto=0 -c maintenance.auto=false fetch -q --no-tags $FORK $FIXBRANCH \
	    > $A/fetch.log 2>&1; echo "fetch rc=$?"
	tail -n 3 $A/fetch.log
	same "$FIXBRANCH" "$(git rev-parse -q --verify FETCH_HEAD)" $HF
	same "its commits on master" "$(git rev-list --reverse $HM..$HF 2>&1 | xargs)" "$C5"
	same "its merge base" "$(git merge-base $HM $HF 2>&1)" $HM
	for c in $C5; do
		echo "  ${c%${c#??????????}}: $(git log -1 --format=%s $c)" \
		    "($(git diff-tree --no-commit-id --numstat -r $c | awk '{ print $3 " +" $1 " -" $2 }' | xargs))"
	done
	same "what it changes" "$(git diff --numstat $HM $HF | xargs)" "$NUMSTAT"
	same "its blobs" "$(for f in $FILES; do printf '%s ' "$(git rev-parse -q --verify $HF:$f)"; done | sed 's/ $//')" "$BB"
	same "its patch-id" "$(git diff $HM $HF | git patch-id --stable | cut -d' ' -f1)" $PID
	} 2>&1 | tee $A/fix.txt
fi
```

Expected:

```
fetch rc=0
ok: zpool-add-warn-removal-log-pass
ok: its commits on master
ok: its merge base
  4e8a517729: ZTS: end zpool_add_warn_removal with log_pass (tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh +2 -0)
  1ae75ec365: ZTS: remove vdevs_in_pool's temporary file when a vdev is missing (tests/zfs-tests/include/libtest.shlib +4 -1)
  e0725c9ae2: ZTS: create removal_with_zdb's temporary file after the Linux check (tests/zfs-tests/tests/functional/removal/removal_with_zdb.ksh +2 -2)
  56b4b06599: ZTS: restore zfs_remove_max_segment in removal_multiple_indirection (tests/zfs-tests/tests/functional/removal/removal_multiple_indirection.ksh +2 -2)
  147a85264c: ZTS: remove TESTDIR1 after removal_condense_export (tests/zfs-tests/tests/functional/removal/removal_condense_export.ksh +1 -0)
ok: what it changes
ok: its blobs
ok: its patch-id
```

A `MISMATCH` (the branch moved) or a failed fetch means stop. Otherwise put
the five files in M, and only them:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
if ! stopped; then
	if grep -q '^MISMATCH' $A/fix.txt || ! grep -qx 'fetch rc=0' $A/fix.txt; then
		stop "the branch is not as expected"
	else
		{
		cd $W/M
		git checkout $HF -- $FILES; echo "checkout rc=$?"
		same "M: the five files" "$(blobs)" "$BB"
		same "M: their modes" "$(modes)" "$MODES"
		same "M: files changed against HEAD" "$(git diff HEAD --name-only | xargs)" "$TR.in $FILES"
		same "M: the five files against HEAD" "$(git diff HEAD --numstat -- $FILES | xargs)" "$NUMSTAT"
		same "M: their patch-id against HEAD" \
		    "$(git diff HEAD -- $FILES | git patch-id --stable | cut -d' ' -f1)" $PID
		} 2>&1 | tee -a $A/fix.txt
		grep -q '^MISMATCH' $A/fix.txt && stop "the files in M are not the branch's"
	fi
fi
```

Expected: `checkout rc=0` and five `ok:` lines. Against HEAD, M then differs
by exactly the branch's five commits, plus STEP 1's `BASEDIR`. Then load the
tree's modules:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
if ! stopped; then
	{
	since=$(date '+%F %T')
	(cd $W/M && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh); echo "load rc=$?"
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" $SV
	same "the tree's zfs.ko" "$(modinfo -F srcversion $W/M/module/zfs.ko)" $SV
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV" ]; then
		same "pools after the load" "$(sudo -n $W/M/zpool list -H -o name 2>&1)" ""
		same "zfs_remove_max_segment after the load" \
		    "$(cat /sys/module/zfs/parameters/zfs_remove_max_segment)" 16777216
		tunables > $A/tunables-loaded.txt
		echo "after the load: $(cat $A/tunables-loaded.txt)"
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
ok: zfs_remove_max_segment after the load
after the load: vdev_file_physical_ashift=9 zfs_removal_suspend_progress=0 zfs_remove_max_segment=16777216 zfs_scan_suspend_progress=0 zfs_condense_indirect_commit_entry_delay_ms=0 zfs_condense_indirect_obsolete_pct=25 zfs_condense_min_mapping_bytes=131072 metaslab_force_ganging=16777217
kernel load: 0 findings, 0 ZTS marks (expected 0)
```

(These are the module's defaults. A value of another tunable set by this
VM's configuration is fine, since each group is compared with what the load
found; `zfs_remove_max_segment` must be 16777216.)

## STEP 3: the removal group, then the zpool_add group

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
if command -v fio > /dev/null; then
	group br removal "PASS x36, SKIP x1" 37 0
else
	group br removal "PASS x35, SKIP x2" 37 0
fi
```

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
group bg zpool_add "PASS x19" 16 3
```

Expected:

```
before br: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
br -T removal PASS x36, SKIP x1 (Test: x37, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  not passed: [<time>] Test: /var/tmp/zts-algo-1026/M/tests/zfs-tests/tests/functional/removal/removal_with_zdb (run as root) [00:00] [SKIP]
  removal_with_zdb: first ERROR line: none
    last line: ZDB fails during concurrent pool activity.
kernel br: 0 findings, 37 ZTS marks (expected 37)
  tunables: as loaded (zfs_remove_max_segment=16777216)
  loop devices: []
  files in /var/tmp/zts-algo-1026/files-vipc: []
leftovers after br: none
before bg: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
    last line: SUCCESS: losetup -d /dev/loop<N>
kernel bg: 0 findings, 19 ZTS marks (expected 19)
  tunables: as loaded (zfs_remove_max_segment=16777216)
  loop devices: []
  files in /var/tmp/zts-algo-1026/files-vipc: []
leftovers after bg: none
```

Without `fio`, `br` also lists `remove_attach_mirror` as SKIP (`fio missing`),
with `PASS x35, SKIP x2`.

## STEP 4: what the results mean

- **As expected** (both groups pass, nothing left after either, no kernel
  finding): with the branch's five commits, `-T removal` and `-T zpool_add`
  leave no `tmp.*` file, no `testdir1`, no loop device and no changed
  tunable on a kernel. The four test leaks found on this VM are all closed:
  the loop device and vdev files, `vdevs_in_pool`'s files,
  `removal_with_zdb`'s file, `testdir1` and `zfs_remove_max_segment`.
- **`LEFTOVER tunables`** with `zfs_remove_max_segment` not 16777216: commit
  `56b4b06599` does not restore it. Another tunable: another test's leak,
  which reading the code did not find. Report the line.
- **A file or directory left in `$FD`**: the `ls -la` lines under
  `files in` name it, with its owner, size and time. Report them; its time
  against the `Test:` lines of `$A/<label>-tests.txt` tells which test left it.
- **A test fails, or a loop device is left**: report its `first ERROR line`
  and the `LEFTOVER` lines.
- **Any kernel finding**: the line and the run are the result. Stop.

## STEP 5: restore, unload, cleanup, report

Run these blocks in all cases, a stop included, except after a stop in STEP 0
or at STEP 1's `mkdir`. First, the whole task's kernel check, and the five
files and the runner restored:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
{
sudo -n journalctl -k --since "$(cat $A/start.txt)" --no-pager -q -o short-iso > $A/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $A/kern-all.txt | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $A/kern-all.txt) ZTS marks"
grep -iE "$KRE" $A/kern-all.txt | grep -vE "$KIGN" | head -n 40
cd $W/M
git checkout HEAD -- $FILES; echo "files restored rc=$?"
cp -p $A/orig/test-runner.py.in $TR.in && cp -p $A/orig/test-runner.py $TR; echo "runner restored rc=$?"
same "the five files are master's" "$(blobs)" "$BM"
same "their modes" "$(modes)" "$MODES"
same "$TR.in as found" "$(sha256sum < $TR.in | cut -d' ' -f1)" "$(cat $A/orig/sha-in)"
same "$TR as found" "$(sha256sum < $TR | cut -d' ' -f1)" "$(cat $A/orig/sha-py)"
same "tracked changes in M" "$(git status --short --untracked-files=no | wc -l)" 0
same "M HEAD" "$(git rev-parse HEAD)" $HM
} 2>&1 | tee $A/end.txt
```

Expected: `kernel, whole task: 0 findings, 56 ZTS marks` (37 and 19; fewer
after a stop), no kernel line, `files restored rc=0`, `runner restored rc=0`,
six `ok:` lines. Then unload and remove the files directory and the results.
The module stays loaded on `HELD` or `PANIC`, and both directories stay after
a stop:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
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
	rm -rf /var/log/zts-algo-1026/vip-c/results; echo "rm results rc=$?"
fi
ls -d $FD $A/results 2>&1
echo "loop devices: [$(loops)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
echo "zed pid directory: $(ls -ld "${ZP%/*}" 2>&1), $(ls -A "${ZP%/*}" 2>/dev/null | wc -l) entries"
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $A/end.txt
```

Expected: `unload rc=0`, `lsmod: []`, `rmdir files dir rc=0`,
`rm results rc=0`, `No such file or directory` twice, `loop devices: []`, a
0-byte `zpool.cache`, no `vdev_id.conf`, the `zed` pid directory empty,
`W holds: M repo`, `L holds: addwarn`, `vip`, `vip-c` and the build's logs.
Then assemble `REPORT.txt`:

```sh
. /var/log/zts-algo-1026/vip-c/env.sh
{
echo "VM 107, phase C: removal and zpool_add groups with 147a85264c (VM-TESTS-107-vipc.md)"
for s in record setup fix runs end; do
	echo
	echo "== $s.txt"
	cat $A/$s.txt 2>&1
done
echo
echo "== summary"
for l in br bg; do
	grep -E "^$l -T |^kernel $l:|^leftovers after $l:" $A/runs.txt
	awk -v k="kernel $l:" 'index($0, k) == 1 { s = 1 }
	    s && /^  (tunables|LEFTOVER tunables)/ { print; exit }' $A/runs.txt
done
echo "flags: STOP [$(cat $A/STOP 2>/dev/null)] HELD [$(ls $A/HELD 2>/dev/null)] PANIC [$(ls $A/PANIC 2>/dev/null)]"
} > $A/REPORT.txt 2>&1
wc -l $A/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, the release lines,
the kernel and `fio` lines), a `verdict:` line naming the STEP 4 case that
applies, and a `deviations:` line with every deviation from these steps and
its reason. Then `cat /var/log/zts-algo-1026/vip-c/REPORT.txt` and reply with
all of it. Expected summary:

```
br -T removal PASS x36, SKIP x1 (Test: x37, Test (Linux): x0) rc=0 wall=<s>s
kernel br: 0 findings, 37 ZTS marks (expected 37)
leftovers after br: none
  tunables: as loaded (zfs_remove_max_segment=16777216)
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
kernel bg: 0 findings, 19 ZTS marks (expected 19)
leftovers after bg: none
  tunables: as loaded (zfs_remove_max_segment=16777216)
flags: STOP [] HELD [] PANIC []
verdict: as expected (STEP 4, first case)
deviations: none
```

Only after your reply holds the whole report, and only if the unload block
printed `unload rc=0` with `lsmod: []` and no step stopped, remove this
task's log directory as the very last command:

```sh
rm -rf /var/log/zts-algo-1026/vip-c; echo "rm rc=$?"
ls -d /var/log/zts-algo-1026/vip-c 2>&1   # No such file or directory
```

If your permission system refuses this `rm -rf`, leave the directory and say
so. Do not delete it any other way.

Estimated duration: about 30 minutes (20 to 45). Checks, release and setup
take 5, `br` 8 to 20, `bg` 5 to 12, and the end 3.
