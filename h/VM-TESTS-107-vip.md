# VM 107: the `vdevs_in_pool` temporary files, master against the fix

These instructions are for the session on VM 107. They follow
`h/VM-TESTS-107-addwarn.md`, which ran here. Master's
`zpool_add_warn_removal` leaked its loop device in 3 of 3 runs, the
`log_pass` fix cleaned up in 3 of 3, and `-T zpool_add` with that fix passed
19 of 19. It then stopped on nine `tmp.*` files (root's, mode 600, 239 to
1635 bytes) left in its files directory by `zpool_add_001`..`009`.

This task measures that leak on master and attributes it to tests. Then it
checks the fork branch `tigerblue77/zfs:zpool-add-warn-removal-log-pass`,
whose head `1ae75ec36558291eb3bd469cda999ec33f76d97e` now also fixes
`vdevs_in_pool()`. It uses the same tree, `/var/tmp/zts-algo-1026/M` (master
`a128b8a075`, srcversion `8852710DB871A7587B9E722`). Nothing is built: both
fixes are test scripts, which the in-tree runner reads from the source tree,
so both states use the same modules.

| State | Files | Runs | Expected |
| --- | --- | --- | --- |
| M | master's | `mg` (`-T zpool_add`), then `m002`, `m003`, `m006` (`-t`) | `mg` PASS x19, leaving the loop device on `$FD/vdev4`, `vdev0`-`vdev9` and 9 `tmp.*` (002: 2, 003: 6, 006: 1); alone, 002 leaves 2, 003 6, 006 1; each run released |
| B | `libtest.shlib` and `zpool_add_warn_removal.ksh` of `1ae75ec365` | `bg` (`-T zpool_add`), `br` (`-T removal`) | `bg` PASS x19, nothing left; `br` PASS x36 SKIP x1, no `tmp.*` with a config, one empty `tmp.*` |

## What the code does (read at `a128b8a075` and `1ae75ec365`)

- `vdevs_in_pool()` (`tests/zfs-tests/include/libtest.shlib`, lines
  3036-3059) writes the config part of `zpool status` to a file from `mktemp`.
  It returns 1 at the first vdev it does not find (line 3054), before its
  `rm -f` (line 3057). `zfs-tests.sh` exports `TMPDIR=$FILEDIR`, so the file
  is a `tmp.XXXXXXXXXX` in the `-d` directory. It is owned by root (the tests
  run as root), has mode 600, and its first line is `config:`.
- Only a call that returns 1 leaks, so in a passing run only a
  `log_mustnot vdevs_in_pool` leaks. In the zpool_add group these are:
  `zpool_add_002_pos` lines 54 and 65 (2); `zpool_add_003_pos` line 76, once
  for each of the six entries of `VDEV_TYPES` (6); `zpool_add_006_pos`
  line 60 (1). **N = 9.** The `log_must vdevs_in_pool` calls of 001, 002, 004
  and 006 return 0 and remove their file. No other function the group reaches
  calls `mktemp`.
- `1ae75ec365` (its parent `4e8a517729` is the `log_pass` fix, and that
  one's parent is `a128b8a075`) removes the file before returning 1, and
  changes nothing else. Against master the branch changes two files:
  `libtest.shlib` (+4 -1) and `zpool_add_warn_removal.ksh` (+2).
- The removal group calls `log_mustnot vdevs_in_pool` in 11 of its tests and
  twice in `removal.kshlib`, so on master it leaks too. This task runs it only with the
  fix. `-T removal` is `common.run`'s `[tests/functional/removal]`: 36 tests
  and its `cleanup` (no `setup`), 37 `Test:` lines, 4 min 46 s in the fork's
  CI. `removal_with_zdb` is always SKIP on Linux, which zts-report expects.
  That test calls `zdbout=$(mktemp)` at line 21, before its Linux
  `log_unsupported` (line 24) and before its `log_onexit` (line 34). So even
  with the fix the group leaves **one empty `tmp.*` file**. That is a separate
  leak, not this branch's. `remove_attach_mirror` needs `fio` and is SKIP
  without it. The group's tests change seven tunables and restore them.

## Rules

- Log in as `tigerblue77` and run everything as that user, in bash, in this
  order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: module loads and every run go
  through `nice -n 10 ionice -c3` (the blocks do it), one test at a time,
  nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing.
- This task's own directories are `/var/tmp/zts-algo-1026/files-vip`
  (`$FD`) and `/var/log/zts-algo-1026/vip` (`$A`). Outside them, change only
  these. In `/var/tmp/zts-algo-1026/M`: the two files (swapped in STEP 3,
  restored in STEP 5) and the runner's `BASEDIR` (redirected in STEP 1,
  restored in STEP 5). In the repository: what STEP 3's fetch adds. Leave the
  tree, `repo`, `/var/log/zts-algo-1026/addwarn`, the build logs and every
  other directory as they are. Never create, edit or remove
  `/etc/zfs/zpool.cache`, `/etc/zfs/vdev_id.conf` or the `zed` pid file
  yourself.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only what `release` removes (the loop
  device bound to `$FD/vdev4`, `vdev0`-`vdev9`, and the `tmp.*` files it
  checked in `$FD`), the `rmdir` of `$FD`, and the `rm -rf` of
  `$A/results` in STEP 5.
- Never run a `zpool` or `zfs` command while this tree's module is not loaded.
  Never print the environment (`env`, `printenv`, `set`).
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-algo-1026/vip/env.sh`.
- Stop at any value the comments or Expected lines do not allow. A `tmp.*`
  count that differs from its prediction is recorded, not a stop. After a
  line ending in `stop`, run only STEP 5; the helpers refuse to run once a
  step has stopped. Clean, repair and retry nothing by hand. If your
  permission system refuses a command, report it and stop. A kernel panic,
  oops, VERIFY or hung task means stop and report, never reboot.
- A single-test block takes 1 to 3 minutes, `mg` and `bg` 5 to 12, `br` 8 to
  20. If your shell tool cannot wait that long in the foreground, start that
  one block with the tool's own background mode and wait for it to exit.
  Never use `&` or `nohup`, and run nothing meanwhile.

What earlier VM runs taught, and the blocks already handle: results are
matched anywhere in a `Test:` line, which starts with a timestamp. A `-t` run
names the test with `.ksh`, a `-T` run without it, and `linux.run` tests are
`Test (Linux): ...`. A literal parenthesis in a regex is `[(]`. The test's
lines are read from the runner's own `log` file. Kernel messages are read per
run from `journalctl -k --since`, without the `ZTS run` markers and
`spa_import_progress_set_notes_impl` lines. Each run waits 2 s first. The
`-d` directory has mode 1777.

## STEP 0: the VM free, the tree as the last task left it

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
LT=tests/zfs-tests/include/libtest.shlib
F=tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
TR=tests/test-runner/bin/test-runner.py
stat -c '%U %n' $W $W/M $W/repo $L     # tigerblue77, four times
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"   # W holds: M repo
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"   # addwarn and the build's logs; no vip
cd $W/M
git rev-parse HEAD                     # a128b8a075b5b13c737e56914cd7043bd3e9ebf8
echo "tracked changes: $(git status --short --untracked-files=no | wc -l)"   # tracked changes: 0
echo "libtest.shlib $(git hash-object $LT), test $(git hash-object $F) $(stat -c %a $F)"
# libtest.shlib 629eb52aaeba514198a8d4573561b5eed10ab31a, test 1591658ba7898aad7a121595da768c0a2215ac73 755
grep -n '^BASEDIR' $TR.in $TR          # line 43 of both: BASEDIR = '/var/tmp/test_results'
echo "srcversion $(modinfo -F srcversion module/zfs.ko), vermagic $(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)"
# srcversion 8852710DB871A7587B9E722, vermagic <uname -r>
```

Stop at once, changing nothing, and report what you found if any value
differs from its comment. That means: another user, `sudo -n` failing, a zfs
or spl module loaded, `/dev/zfs`, a `zed`, a test or build process, a busy
tmux pane, a loop device, a device-mapper device, a zfs or test mount, a
`/testpool*` directory, a non-empty `zpool.cache`, a `vdev_id.conf`, less than
8 GB free, no kernel line, `$W` holding anything but `M repo`, `$L` holding
`vip`, a tracked change, another blob or mode, `BASEDIR` already redirected,
or another srcversion or vermagic. A kernel other than
`6.12.113+deb13-cloud-amd64` is only recorded, and so is the `fio` line.

## STEP 1: this task's environment, the results directory, the module

The block writes nothing unless it can create both directories.

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/log/zts-algo-1026/vip /var/log/zts-algo-1026/vip/orig &&
    mkdir -m 1777 /var/tmp/zts-algo-1026/files-vip; then
echo "mkdir rc=0"
cat > /var/log/zts-algo-1026/vip/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-algo-1026
L=/var/log/zts-algo-1026
# This task's own directories: its logs and the runner's results (A), and
# the zfs-tests.sh -d directory (FD, mode 1777).
A=$L/vip
FD=$W/files-vip
FORK=https://github.com/tigerblue77/zfs
FIXBRANCH=zpool-add-warn-removal-log-pass
# The tree's commit (master), the branch's two commits on it, the srcversion.
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
HF1=4e8a517729942d1ac159eb49cfe9c93574fc0b99
HF=1ae75ec36558291eb3bd469cda999ec33f76d97e
SV=8852710DB871A7587B9E722
# The two files the branch changes, their blobs on master (BM*) and on the
# branch (BF*), and the patch-id of the branch against master.
LT=tests/zfs-tests/include/libtest.shlib
F=tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
T=zpool_add_warn_removal
BML=629eb52aaeba514198a8d4573561b5eed10ab31a
BFL=a7db244ef25c951ed6489806f79b85e744feec1b
BM=1591658ba7898aad7a121595da768c0a2215ac73
BF=7984f12bdfa6e3c5baa79f993311fbd763e8970d
PID=5c275752b9e9a97bcac6d0c1a0f4efcfc3c768ec
# The zpool_add directory, for -t runs of its tests.
ZA=tests/functional/cli_root/zpool_add
TR=tests/test-runner/bin/test-runner.py
V10="vdev0 vdev1 vdev2 vdev3 vdev4 vdev5 vdev6 vdev7 vdev8 vdev9"
# The pid file of the tree's zed, which add_nested_replacing_spare starts.
ZP=$(grep -aoE '/[[:alnum:]/._-]+/zed[.]pid' $W/M/.libs/zed 2>/dev/null | head -n 1)
# The tunables the zpool_add and removal groups change and restore.
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

# What $FD and the loop devices hold. NLO: the number of loop devices. LO: the
# one bound to $FD/vdev4. V: yes when vdev0 to vdev9 are all in $FD. TMPS:
# vdevs_in_pool's files (tmp.<10 characters>, regular, root's, mode 600,
# first line "config:"). TMPE: the same, but empty (removal_with_zdb's).
# OTHER: anything else.
classify() {
	local l e
	l=$(loops)
	NLO=$(printf '%s\n' "$l" | grep -c .)
	LO=$(printf '%s\n' "$l" | awk -v v="$FD/vdev4" \
	    'NF == 2 && $1 ~ /^[/]dev[/]loop[0-9]+$/ && $2 == v { print $1 }')
	V=no
	TMPS=""
	TMPE=""
	OTHER=""
	[ "$(LC_ALL=C ls -A $FD | grep -xE 'vdev[0-9]' | xargs)" = "$V10" ] && V=yes
	for e in $(LC_ALL=C ls -A $FD); do
		case $e in
		vdev[0-9])
			[ $V = yes ] || OTHER="$OTHER $e" ;;
		tmp.??????????)
			if [ -f "$FD/$e" ] && [ ! -L "$FD/$e" ] &&
			    [ "$(stat -c '%U %a' "$FD/$e")" = "root 600" ]; then
				if [ "$(stat -c %s "$FD/$e")" = 0 ]; then
					TMPE="$TMPE $e"
				elif [ "$(sudo -n head -n 1 "$FD/$e")" = "config:" ]; then
					TMPS="$TMPS $e"
				else
					OTHER="$OTHER $e"
				fi
			else
				OTHER="$OTHER $e"
			fi ;;
		*)
			OTHER="$OTHER $e" ;;
		esac
	done
	TMPS=${TMPS# }
	TMPE=${TMPE# }
	OTHER=${OTHER# }
}

# After a run: what it left, against <expected>. none: nothing. tmp: only
# tmp.* files of the two kinds. leak+tmp: also exactly master's
# zpool_add_warn_removal leak (the loop device on $FD/vdev4 and vdev0 to
# vdev9). The tmp.* counts are compared with the predictions, and recorded.
# A pool, device, mount, process or loop device left may hold the module
# (HELD: STEP 5 does not unload).
leftovers() {   # leftovers <label> <none|tmp|leak+tmp> <predicted config tmp.*> <predicted empty tmp.*>
	local bad=0 held=0 ok=no p ks ke
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
	classify
	ks=$(echo $TMPS | wc -w)
	ke=$(echo $TMPE | wc -w)
	echo "$ks $ke" > $A/ntmp-$1.txt
	echo "  loop devices: [$(loops | tr '\n' ';')]"
	echo "  in $FD: vdev0-vdev9 $V, tmp.* with a config x$ks (predicted $3)," \
	    "empty tmp.* x$ke (predicted $4), other [$OTHER]"
	if [ "$ks $ke" != "$3 $4" ]; then
		echo "  tmp.* counts differ from the prediction: recorded, not a stop"
	fi
	case $2 in
	none)
		[ $NLO = 0 ] && [ $V = no ] && [ -z "$TMPS$TMPE$OTHER" ] && ok=yes ;;
	tmp)
		[ $NLO = 0 ] && [ $V = no ] && [ -z "$OTHER" ] && ok=yes ;;
	leak+tmp)
		[ $NLO = 1 ] && [ -n "$LO" ] && [ $V = yes ] && [ -z "$OTHER" ] && ok=yes ;;
	esac
	if [ $bad$held = 00 ] && [ $ok = yes ]; then
		echo "leftovers after $1: as expected ($2)"
	else
		echo "leftovers after $1: FOUND (expected $2)"
	fi
	} > $A/left-$1.txt 2>&1
	tee -a $A/runs.txt < $A/left-$1.txt
	if [ $bad$held = 00 ] && [ $ok = yes ]; then
		return 0
	fi
	if [ $held = 1 ] || [ $NLO != 0 ]; then
		touch $A/HELD
	fi
	stop "$1: leftovers"
}

post_run() {   # post_run <label> <since> <rc> <expected ZTS marks> <leftovers arguments...>
	local label=$1 since=$2 rc=$3 marks=$4 bad=0
	shift 4
	kern_check $label "$since" $marks || bad=1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		stop "$label: outer timeout"
		bad=1
	fi
	leftovers $label "$@" || bad=1
	return $bad
}

# Which test wrote each tmp.* file with a config in $FD: its mtime against the
# end time of each test in the run's result lines.
attribute() {   # attribute <label>
	local ts p e
	sed -nE 's/^\[([^]]+)\] Test( [(][^)]*[)])?: ([^ ]+) [(]run as .*$/\1 \3/p' $A/$1-tests.txt |
	    while read -r ts p; do
		echo "$(date -d "$ts" +%s.%N) ${p##*/}"
	done > $A/$1-ends.txt
	classify
	for e in $TMPS; do
		stat -c '%.9Y' "$FD/$e"
	done | sort -n > $A/$1-mtimes.txt
	echo "  tmp.* by test (mtime): $(awk '
	    NR == FNR { e[NR] = $1; n[NR] = $2; k = NR; next }
	    {
		for (i = 1; i <= k && $1 > e[i]; i++)
			;
		if (i <= k) t = n[i]; else t = "after the last test"
		c[t]++
	    }
	    END { for (t in c) print t " x" c[t] }' $A/$1-ends.txt $A/$1-mtimes.txt |
	    sort | paste -sd, - | sed 's/,/, /g')" | tee -a $A/runs.txt
}

# After a run: detach the loop device bound to $FD/vdev4 and remove vdev0 to
# vdev9 (both or neither) and the checked tmp.* files, only when that is all
# there is.
release() {   # release <label>
	local i e l f lo=no rc1=0 rc2=0 rc3=0
	stopped && return 1
	classify
	[ -n "$LO" ] && lo=yes
	if [ -n "$OTHER" ] || [ $NLO -gt 1 ] || { [ $NLO = 1 ] && [ -z "$LO" ]; } ||
	    [ $lo != $V ] || [ -n "$(sudo -n $W/M/zpool list -H -o name 2>&1)" ]; then
		touch $A/HELD
		stop "release after $1: not only what the tests are known to leave"
		return 1
	fi
	if [ $lo = yes ]; then
		sudo -n losetup -d "$LO"
		rc1=$?
		sudo -n rm -f -- $(for i in 0 1 2 3 4 5 6 7 8 9; do printf '%s ' "$FD/vdev$i"; done)
		rc2=$?
	fi
	for e in $TMPS $TMPE; do
		sudo -n rm -f -- "$FD/$e" || rc3=1
	done
	for i in 1 2 3 4 5; do
		[ -z "$(loops)" ] && break
		sleep 1
	done
	l=$(loops)
	f=$(LC_ALL=C ls -A $FD | xargs)
	echo "released after $1: loop [${LO:-none}] rc=$rc1, vdev0-vdev9 [$V] rc=$rc2," \
	    "tmp.* x$(echo $TMPS $TMPE | wc -w) rc=$rc3; loop devices now [$l], files now [$f]" |
	    tee -a $A/runs.txt
	if [ $rc1 -ne 0 ] || [ $rc2 -ne 0 ] || [ $rc3 -ne 0 ] || [ -n "$l$f" ]; then
		touch $A/HELD
		stop "release after $1: something is left"
		return 1
	fi
}

# One zpool_add test alone (-t: the directory's setup, the test, its cleanup).
one() {   # one <label> <test name> <leftovers arguments...>
	local label=$1 name=$2 since t0 rc res np d bad=0
	shift 2
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/M && timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $FD -t $ZA/$name.ksh) > $A/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $A/$label.log > $A/$label-tests.txt
	res=$(grep -E "/$name([.]ksh)? [(]run as " $A/$label-tests.txt | tally)
	echo "$label $name ${res:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $A/runs.txt
	np=$(grep -vE "/$name([.]ksh)? [(]run as " $A/$label-tests.txt | grep -v '\[PASS\]')
	[ -z "$np" ] || printf '%s\n' "$np" | sed 's/^/  not passed: /' | tee -a $A/runs.txt
	d=$(awk '/^Log directory/ { print $3 }' $A/$label.log | head -n 1)
	if [ "$res" != "PASS x1" ] && [ -f "$d/log" ]; then
		fails "$d/log" $name | tee -a $A/runs.txt
	fi
	post_run $label "$since" $rc 3 "$@" || bad=1
	if [ "$res" != "PASS x1" ] || [ $rc -ne 0 ] || [ -n "$np" ]; then
		stop "$label: result not as expected"
		bad=1
	fi
	return $bad
}

# One tag group (-T <tag>), checked against <expected tally> and the counts of
# "Test:" and "Test (Linux):" lines. For zpool_add, also the digest of
# zpool_add_warn_removal and, after a leak+tmp run, the attribution.
group() {   # group <label> <tag> <expected tally> <Test: lines> <Test (Linux): lines> <leftovers arguments...>
	local label=$1 tag=$2 exp=$3 ent=$4 enl=$5 since t0 rc c nt nl np d n dg="" de=none bad=0
	shift 5
	pre_run $label || return 1
	sleep 2
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
	post_run $label "$since" $rc $(( ent + enl )) "$@" || bad=1
	[ "$1" = leak+tmp ] && { de=leak; attribute $label; }
	if [ "$c" != "$exp" ] || [ $rc -ne 0 ] || [ "$nt" != $ent ] || [ "$nl" != $enl ] ||
	    { [ $tag = zpool_add ] && [ "$(printf '%s\n' "$dg" | head -n 1)" != "$(expected_digest $de)" ]; }; then
		stop "$label: results not as expected"
		bad=1
	fi
	return $bad
}
ENV
bash -n /var/log/zts-algo-1026/vip/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-algo-1026/vip/env.sh
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
. /var/log/zts-algo-1026/vip/env.sh
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
lines on line 43 with `'/var/log/zts-algo-1026/vip/results'`,
`ok: results directory`, `ok: ... mode`. Then load the tree's modules:

```sh
. /var/log/zts-algo-1026/vip/env.sh
if ! stopped; then
	{
	since=$(date '+%F %T')
	(cd $W/M && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh); echo "load rc=$?"
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" $SV
	same "the tree's zfs.ko" "$(modinfo -F srcversion $W/M/module/zfs.ko)" $SV
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV" ]; then
		same "pools after the load" "$(sudo -n $W/M/zpool list -H -o name 2>&1)" ""
		tunables > $A/tunables-loaded.txt
		echo "after the load: $(cat $A/tunables-loaded.txt)"
	fi
	kern_check load "$since" 0
	} 2>&1 | tee -a $A/setup.txt
	grep -q '^MISMATCH' $A/setup.txt && { touch $A/HELD; stop "the module load"; }
fi
```

Expected: `load rc=0`, three `ok:` lines, the eight tunables as the module
loads them (`vdev_file_physical_ashift=9 zfs_removal_suspend_progress=0 ...`),
`kernel load: 0 findings, 0 ZTS marks (expected 0)`.

## STEP 2: state M, master's files

The group once, then its three leaking tests alone, one block at a time:

```sh
. /var/log/zts-algo-1026/vip/env.sh
group mg zpool_add "PASS x19" 16 3 leak+tmp 9 0 && release mg
```

```sh
. /var/log/zts-algo-1026/vip/env.sh
one m002 zpool_add_002_pos tmp 2 0 && release m002
```

```sh
. /var/log/zts-algo-1026/vip/env.sh
one m003 zpool_add_003_pos tmp 6 0 && release m003
```

```sh
. /var/log/zts-algo-1026/vip/env.sh
one m006 zpool_add_006_pos tmp 1 0 && release m006
```

Expected (`<N>` is the loop device `zpool_add_warn_removal` took, 3 when ZTS's
own are 0 to 2):

```
before mg: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
mg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  zpool_add_warn_removal: 108 pool destroys, cleanup line x0, losetup -d x0, log_pass message x0, ERROR lines x0
    last line: SUCCESS: zpool destroy -f testpool1
kernel mg: 0 findings, 19 ZTS marks (expected 19)
  loop devices: [/dev/loop<N> /var/tmp/zts-algo-1026/files-vip/vdev4]
  in /var/tmp/zts-algo-1026/files-vip: vdev0-vdev9 yes, tmp.* with a config x9 (predicted 9), empty tmp.* x0 (predicted 0), other []
leftovers after mg: as expected (leak+tmp)
  tmp.* by test (mtime): zpool_add_002_pos x2, zpool_add_003_pos x6, zpool_add_006_pos x1
released after mg: loop [/dev/loop<N>] rc=0, vdev0-vdev9 [yes] rc=0, tmp.* x9 rc=0; loop devices now [], files now []
```

and, for `m002` (the same shape for `m003` with 6 and `m006` with 1):

```
before m002: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
m002 zpool_add_002_pos PASS x1 rc=0 wall=<s>s
kernel m002: 0 findings, 3 ZTS marks (expected 3)
  loop devices: []
  in /var/tmp/zts-algo-1026/files-vip: vdev0-vdev9 no, tmp.* with a config x2 (predicted 2), empty tmp.* x0 (predicted 0), other []
leftovers after m002: as expected (tmp)
released after m002: loop [none] rc=0, vdev0-vdev9 [no] rc=0, tmp.* x2 rc=0; loop devices now [], files now []
```

## STEP 3: state B, the branch's two files

Fetch the branch into the tree's repository and check it (read-only towards
GitHub):

```sh
. /var/log/zts-algo-1026/vip/env.sh
if ! stopped; then
	{
	cd $W/M
	git -c gc.auto=0 -c maintenance.auto=false fetch -q --no-tags $FORK $FIXBRANCH \
	    > $A/fetch.log 2>&1; echo "fetch rc=$?"
	tail -n 3 $A/fetch.log
	same "$FIXBRANCH" "$(git rev-parse -q --verify FETCH_HEAD)" $HF
	same "its parents" "$(git rev-parse -q --verify "$HF^") $(git rev-parse -q --verify "$HF~2")" "$HF1 $HM"
	same "what it changes" "$(git diff --numstat $HM $HF | xargs)" "4 1 $LT 2 0 $F"
	same "its blobs" "$(git rev-parse -q --verify $HF:$LT) $(git rev-parse -q --verify $HF:$F)" "$BFL $BF"
	same "its patch-id" "$(git diff $HM $HF | git patch-id --stable | cut -d' ' -f1)" $PID
	echo "lines it adds or removes:"
	git diff -U0 $HM $HF | grep -E '^[+-]' | grep -vE '^(\+\+\+|---) '
	} 2>&1 | tee $A/fix.txt
fi
```

Expected:

```
fetch rc=0
ok: zpool-add-warn-removal-log-pass
ok: its parents
ok: what it changes
ok: its blobs
ok: its patch-id
lines it adds or removes:
-		grep -wq ${vdev##*/} $tmpfile || return 1
+		if ! grep -wq ${vdev##*/} $tmpfile; then
+			rm -f $tmpfile
+			return 1
+		fi
+
+log_pass "Verify 'zpool add' warns for differing redundancy."
```

A `MISMATCH` (the branch moved) or a failed fetch means stop. Otherwise put
the branch's two files in M, and only them:

```sh
. /var/log/zts-algo-1026/vip/env.sh
if ! stopped; then
	if grep -q '^MISMATCH' $A/fix.txt || ! grep -qx 'fetch rc=0' $A/fix.txt; then
		stop "the branch is not as expected"
	else
		{
		cd $W/M
		git checkout $HF -- $LT $F; echo "checkout rc=$?"
		same "M: the two files" "$(git hash-object $LT) $(git hash-object $F)" "$BFL $BF"
		same "M: their modes" "$(stat -c %a $LT) $(stat -c %a $F)" "644 755"
		same "M: files changed against HEAD" "$(git diff HEAD --name-only | xargs)" "$TR.in $LT $F"
		same "M: the two files against HEAD" "$(git diff HEAD --numstat -- $LT $F | xargs)" "4 1 $LT 2 0 $F"
		same "M: their patch-id against HEAD" \
		    "$(git diff HEAD -- $LT $F | git patch-id --stable | cut -d' ' -f1)" $PID
		} 2>&1 | tee -a $A/fix.txt
		grep -q '^MISMATCH' $A/fix.txt && stop "the files in M are not the branch's"
	fi
fi
```

Expected: `checkout rc=0` and five `ok:` lines: against HEAD, M then differs
by exactly the branch's two commits (and STEP 1's `BASEDIR`). Then the
zpool_add group, and the removal group (its expectation depends on `fio`):

```sh
. /var/log/zts-algo-1026/vip/env.sh
group bg zpool_add "PASS x19" 16 3 none 0 0 && release bg
```

```sh
. /var/log/zts-algo-1026/vip/env.sh
if command -v fio > /dev/null; then
	group br removal "PASS x36, SKIP x1" 37 0 tmp 0 1 && release br
else
	group br removal "PASS x35, SKIP x2" 37 0 tmp 0 1 && release br
fi
```

Expected:

```
before bg: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  zpool_add_warn_removal: 108 pool destroys, cleanup line x1, losetup -d x1, log_pass message x1, ERROR lines x0
    last line: SUCCESS: losetup -d /dev/loop<N>
kernel bg: 0 findings, 19 ZTS marks (expected 19)
  loop devices: []
  in /var/tmp/zts-algo-1026/files-vip: vdev0-vdev9 no, tmp.* with a config x0 (predicted 0), empty tmp.* x0 (predicted 0), other []
leftovers after bg: as expected (none)
released after bg: loop [none] rc=0, vdev0-vdev9 [no] rc=0, tmp.* x0 rc=0; loop devices now [], files now []
before br: pools=[] loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
br -T removal PASS x36, SKIP x1 (Test: x37, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <whatever it lists>
  not passed: [<time>] Test: /var/tmp/zts-algo-1026/M/tests/zfs-tests/tests/functional/removal/removal_with_zdb (run as root) [00:00] [SKIP]
  removal_with_zdb: first ERROR line: none
    last line: ZDB fails during concurrent pool activity.
kernel br: 0 findings, 37 ZTS marks (expected 37)
  loop devices: []
  in /var/tmp/zts-algo-1026/files-vip: vdev0-vdev9 no, tmp.* with a config x0 (predicted 0), empty tmp.* x1 (predicted 1), other []
leftovers after br: as expected (tmp)
released after br: loop [none] rc=0, vdev0-vdev9 [no] rc=0, tmp.* x1 rc=0; loop devices now [], files now []
```

Without `fio`, `br` also lists `remove_attach_mirror` as SKIP (`fio
missing`), with `PASS x35, SKIP x2`. `br` stops on any `tmp.*` file it cannot
classify or any other leftover, and on a test that fails or is KILLED.

## STEP 4: what the results mean

- **As expected**: on master, `-T zpool_add` passes and leaves 9 config files.
  The mtimes put them in 002 (2), 003 (6) and 006 (1), and the three tests
  alone leave the same counts. With the branch, the group leaves nothing
  (the loop device too). `-T removal` passes and leaves no config file, only
  `removal_with_zdb`'s empty file: a separate leak in that test (its `mktemp`
  comes before its `log_unsupported`), which `1ae75ec365` does not cover.
- **A count differs from its prediction**: report the counts and the
  attribution as measured. The prediction came from the code, so a
  difference names a call this reading missed.
- **A config file left with the branch** (`bg` or `br`): the fix is
  incomplete. Report the count and the run.
- **A test fails, or anything else is left**: report it with its
  `first ERROR line` and the `LEFTOVER` lines.
- **Any kernel finding**: the line and the run are the result. Stop.

## STEP 5: restore, unload, cleanup, report

Run these three blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First, the whole task's kernel check, and the
two files and the runner restored:

```sh
. /var/log/zts-algo-1026/vip/env.sh
{
sudo -n journalctl -k --since "$(cat $A/start.txt)" --no-pager -q -o short-iso > $A/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $A/kern-all.txt | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $A/kern-all.txt) ZTS marks"
grep -iE "$KRE" $A/kern-all.txt | grep -vE "$KIGN" | head -n 40
cd $W/M
git checkout HEAD -- $LT $F; echo "files restored rc=$?"
cp -p $A/orig/test-runner.py.in $TR.in && cp -p $A/orig/test-runner.py $TR; echo "runner restored rc=$?"
same "the two files are master's" "$(git hash-object $LT) $(git hash-object $F)" "$BML $BM"
same "their modes" "$(stat -c %a $LT) $(stat -c %a $F)" "644 755"
same "$TR.in as found" "$(sha256sum < $TR.in | cut -d' ' -f1)" "$(cat $A/orig/sha-in)"
same "$TR as found" "$(sha256sum < $TR | cut -d' ' -f1)" "$(cat $A/orig/sha-py)"
same "tracked changes in M" "$(git status --short --untracked-files=no | wc -l)" 0
same "M HEAD" "$(git rev-parse HEAD)" $HM
} 2>&1 | tee $A/end.txt
```

Expected: `kernel, whole task: 0 findings, 84 ZTS marks` (19, 3 x 3, 19, 37;
fewer after a stop), no kernel line, `files restored rc=0`,
`runner restored rc=0`, six `ok:` lines. Then unload and remove this task's
files directory and results. The module stays loaded on `HELD` or `PANIC`, and
both directories stay after a stop:

```sh
. /var/log/zts-algo-1026/vip/env.sh
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
	rm -rf /var/log/zts-algo-1026/vip/results; echo "rm results rc=$?"
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
`rm results rc=0`, `No such file or directory` twice, `loop devices: []`, a
0-byte `zpool.cache`, no `vdev_id.conf`, the `zed` pid directory empty,
`W holds: M repo`. Last, assemble `REPORT.txt`:

```sh
. /var/log/zts-algo-1026/vip/env.sh
{
echo "VM 107, vdevs_in_pool tmp.* leak: master a128b8a075 against 1ae75ec365 (VM-TESTS-107-vip.md)"
for s in record setup fix runs end; do
	echo
	echo "== $s.txt"
	cat $A/$s.txt 2>&1
done
echo
echo "== summary"
for l in mg m002 m003 m006 bg br; do
	awk -v l="$l" '
	index($0, l " ") == 1 || index($0, "kernel " l ":") == 1 ||
	    index($0, "leftovers after " l ":") == 1 || index($0, "released after " l ":") == 1 { print }
	index($0, l " ") == 1 { s = 1; next }
	s && index($0, "  in ") == 1 { print; s = 0 }' $A/runs.txt
done
grep '^  tmp[.][*] by test' $A/runs.txt
echo "flags: STOP [$(cat $A/STOP 2>/dev/null)] HELD [$(ls $A/HELD 2>/dev/null)] PANIC [$(ls $A/PANIC 2>/dev/null)]"
} > $A/REPORT.txt 2>&1
wc -l $A/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, the kernel and
`fio` lines), a `verdict:` line naming the STEP 4 case that applies, and a
`deviations:` line with every deviation from these steps and its reason.
Then `cat /var/log/zts-algo-1026/vip/REPORT.txt` and reply with all of it.

Expected summary:

```
mg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 wall=<s>s
kernel mg: 0 findings, 19 ZTS marks (expected 19)
  in /var/tmp/zts-algo-1026/files-vip: vdev0-vdev9 yes, tmp.* with a config x9 (predicted 9), empty tmp.* x0 (predicted 0), other []
leftovers after mg: as expected (leak+tmp)
released after mg: loop [/dev/loop<N>] rc=0, vdev0-vdev9 [yes] rc=0, tmp.* x9 rc=0; loop devices now [], files now []
m002 zpool_add_002_pos PASS x1 rc=0 wall=<s>s ... tmp.* with a config x2 (predicted 2) ... as expected (tmp) ... released
m003 zpool_add_003_pos PASS x1 ... x6 (predicted 6) ...; m006 zpool_add_006_pos PASS x1 ... x1 (predicted 1) ...
bg -T zpool_add PASS x19 (Test: x16, Test (Linux): x3) rc=0 ... x0 (predicted 0), empty tmp.* x0 ... as expected (none)
br -T removal PASS x36, SKIP x1 (Test: x37, Test (Linux): x0) rc=0 ... x0 (predicted 0), empty tmp.* x1 (predicted 1) ... as expected (tmp)
  tmp.* by test (mtime): zpool_add_002_pos x2, zpool_add_003_pos x6, zpool_add_006_pos x1
flags: STOP [] HELD [] PANIC []
verdict: as expected (STEP 4, first case)
deviations: none
```

Estimated duration: about 45 minutes (35 to 65). Checks and setup take 5,
`mg` and `bg` 5 to 12 each, the three single tests 1 to 3 each, `br` 8 to 20,
and the end 3.
