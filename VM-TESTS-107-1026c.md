# VM 107: kernel ZTS for zpool-remove-dry-run-checks and zfs-allow-destroy-snapshots-test

These instructions are for the Claude session on VM 107. They give two pull
requests that will be opened from the fork `tigerblue77/zfs` kernel results on
their exact heads, which their descriptions still lack ("Not run" for the
first, "I could not run the test suite" for the second). The session builds
three trees (at most two at a time), runs one test at a time, cleans up after
itself and writes every result to `/var/log/zts-rmallow1026c/REPORT.txt`, whose
expected contents end this document.

| Fork branch | Head | Merge base with upstream master | Commit | New test |
| --- | --- | --- | --- | --- |
| `zpool-remove-dry-run-checks` | `59fbe98d73` (1 commit) | `e117a84dbb` | `lib/libzfs/libzfs_pool.c` (the fix), `zpool-remove.8`, the test and its wiring | `removal/removal_dry_run` |
| `zfs-allow-destroy-snapshots-test` | `45729d7354` (1 commit) | `e117a84dbb` | `zfs-allow.8`, the test and its wiring: no code | `delegate/zfs_allow_destroy_snapshot` |

| Tree | Code | Tests run | Labels |
| --- | --- | --- | --- |
| `MRD` | master `e117a84dbb` + the dry-run test, its `common.run` and `Makefile.am` lines | `removal_dry_run` | `m1`-`m3` |
| `BRD` | head of zpool-remove-dry-run-checks | `removal_dry_run` | `b1`-`b3` |
| `BDS` | head of zfs-allow-destroy-snapshots-test | `zfs_allow_destroy_snapshot`, then the `delegate` group once | `d1`-`d3`, `dg` |

Why these states:

- **zpool-remove-dry-run-checks** fixes userspace only: `zpool remove -n` now
  refuses, with the real removal's error, what the kernel would refuse. Its
  test runs each refused case with `-n` and for real and compares the two
  messages, so the kernel's own refusals are half of every check. On master
  (`MRD`) the test must fail at its first case (`zpool remove -n` of a vdev of
  a pool with a raidz prints the estimate and exits 0); on the head (`BRD`) it
  must pass. The same patch already did this on this VM on 25 Oct (on
  `f2842b2468`, master `a0c443a85`), but no record of that run's lines exists
  outside the VM, and the branch has been rebased since.
- **zfs-allow-destroy-snapshots-test** changes no code: its test pins down
  master's own behaviour, so a "master plus test files" state would be this
  head with another man page, and is not built. The test must pass, three
  times; the `delegate` group runs once beside it, because on 20 Oct (same
  patch, older master) this VM reported the group at 18 PASS / 6 FAIL without
  saying which failed or why.
- None of the three trees changes `module/` or `include/`: the kernel module of
  each is master `e117a84dbb`'s. Their srcversions may still differ (the
  generated version header names each tree's commit), so they are recorded but
  not required to differ; what tells the trees apart is their userspace, which
  `usr_check` verifies after each tree's runs: the tests' `zpool` is the tree's
  own, and `libzfs_pool.c` has the fix's `zpool_vdev_remove_check` (2
  occurrences) on `BRD` only.

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created (the `delegate` tests create and
  delete their own `staff1`, `staff2`, `other1`, `other2`).
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot.
- Work only under `/var/tmp/zts-rmallow1026c`, log to
  `/var/log/zts-rmallow1026c`. Never read, write or delete
  `/var/tmp/test_results` or anything else this task did not create: the
  results go to `/var/log/zts-rmallow1026c/results`. The only `rm -rf`
  allowed are the ones written below, with their literal paths.
- Leave every file, process, tmux pane, loop device, pool, user and module you
  did not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never `-j16`, and so do the tests,
  one at a time.
- All git work happens in STEP 1.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-rmallow1026c/env.sh`, so that it also works in a shell that
  keeps no state between commands.
- After any line that says `stop`, run nothing more: go to STEP 6 with what
  you have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

What earlier VM runs taught, and these steps already handle:

- The runner prefixes every `Test:` line with a timestamp: results are matched
  anywhere in the line, not at its start.
- A test that writes nothing on stderr has no `merged` file: the failing line
  and the notes are read from the runner's own `log` file, which every run has.
- On a failed test, the ZTS callbacks print and then clear dmesg (`dmesg -c`):
  kernel messages are read per run from `journalctl -k`, and a dmesg count is
  only a per-state cross-check.
- Each run waits 2 s first: two runs started in the same second would share a
  results directory (`[Errno 17] File exists`).
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`, never
  `\(` (awk turns `\(` into `(`); the patterns below contain none.

## STEP 0: the VM must be free

Run all of these and keep their output:

```sh
export PATH=/usr/sbin:/sbin:$PATH
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh'
ls -A /var/tmp; ls -d /var/tmp/zts-rmallow1026c /var/log/zts-rmallow1026c 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n losetup -a; sudo -n dmsetup ls 2>&1
sudo -n journalctl -k -n 1 --no-pager
getent passwd staff1 staff2 other1 other2; getent group zfsgrp othergrp
command -v ksh python3 git
```

Stop at once, changing nothing, if:

- `id -un` is not `tigerblue77`, or `sudo -n true` fails;
- the system is not Debian 13, or the kernel is not a 6.12 one, or its headers
  (`/lib/modules/$(uname -r)/build`) are missing: do not install them;
- a zfs or spl module is loaded, or `/dev/zfs` exists;
- a test or build process runs, other than this command and the shell running
  it;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `/var/tmp/zts-rmallow1026c` or `/var/log/zts-rmallow1026c` already exists;
- `/var/tmp` has less than 10 GB free (two built trees at a time);
- `journalctl -k` prints no kernel line: without the journal, the kernel
  messages of a failed test are lost;
- `getent` prints anything: the `delegate` setup deletes and recreates those
  users and groups, so existing ones are not ours to lose;
- `ksh`, `python3` or `git` is missing.

Report what was found; that is the whole task then. The loop and dm devices
listed here are not ours: STEP 1 keeps them as the baseline of the leftover
checks. Record `uname -r`: these instructions were written for
`6.12.113+deb13-cloud-amd64`, and another 6.12 kernel is not a reason to stop.

## STEP 1: environment, sources, test files

```sh
sudo -n mkdir -p /var/log/zts-rmallow1026c/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-rmallow1026c
cat > /var/log/zts-rmallow1026c/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-rmallow1026c
L=/var/log/zts-rmallow1026c
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The two heads and their common merge base with upstream master (the
# parent of each).
HRD=59fbe98d73ac40f4ee336a1ca140b275ddd79edc
HDS=45729d735429f2791d417e5e55ea29225b1b1366
BASE=e117a84dbbfe0befe15110b9d1d799f47b1a5546
# The tests as files of the tree (F*) and as zfs-tests.sh -t arguments (R*).
FRD=tests/zfs-tests/tests/functional/removal/removal_dry_run.ksh
RRD=tests/functional/removal/removal_dry_run.ksh
FDS=tests/zfs-tests/tests/functional/delegate/zfs_allow_destroy_snapshot.ksh
RDS=tests/functional/delegate/zfs_allow_destroy_snapshot.ksh
WIRING="tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am"
# The tunables the tests change and must restore.
TUN="zfs_removal_suspend_progress zfs_admin_snapshot"
# The users and groups the delegate tests create and must delete.
USERS="staff1 staff2 other1 other2"
GROUPS_="zfsgrp othergrp"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# The lines of a failed test worth quoting.
NRE='succeeded: |unexpected error|unexpected output|zpool remove -n: |cannot destroy|permission denied|cannot execute'
# One line per refused case of removal_dry_run (12 when it passes).
CRE='cannot remove'

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion, and send its ZTS
# results to $L/results. The three trees share master's module code, so
# their srcversions are recorded, not required to differ.
build_tree() {   # build_tree <tree>
	local t0 rc f
	cd $Z/$1 || return 1
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c \
	    './autogen.sh && ./configure --enable-debug && make -j8' \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	if [ $rc -ne 0 ]; then
		grep -nE 'error:|Error [0-9]' $L/build-$1.log | head -20
		echo "build $1 failed: stop" | tee -a $L/runs.txt
		return 1
	fi
	modinfo -F srcversion module/zfs.ko > $L/srcv-$1
	for f in $L/srcv-*; do
		[ "$f" = "$L/srcv-$1" ] && continue
		cmp -s "$f" $L/srcv-$1 &&
		    echo "$1 has the srcversion of ${f##*/srcv-} (same module code)" | tee -a $L/runs.txt
	done
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    tests/test-runner/bin/test-runner.py.in \
	    tests/test-runner/bin/test-runner.py
	if ! grep -qx "BASEDIR = '$L/results'" tests/test-runner/bin/test-runner.py; then
		echo "$1: BASEDIR not redirected: stop" | tee -a $L/runs.txt
		return 1
	fi
	echo "$1 built: srcversion $(cat $L/srcv-$1)" | tee -a $L/runs.txt
}

# Load <tree>'s modules. Whatever is loaded must be <previous>'s, and is
# unloaded first; <previous> is "none" for the first load.
switch_to() {   # switch_to <tree> <previous>
	local cur t
	cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
	if [ -n "$cur" ]; then
		if [ "$2" = none ] || [ "$cur" != "$(cat $L/srcv-$2)" ]; then
			echo "loaded zfs module $cur is not $2's: stop" | tee -a $L/runs.txt
			return 1
		fi
		(cd $Z/$2 && sudo -n ./scripts/zfs.sh -u)
		echo "unload $2 rc=$?" | tee -a $L/runs.txt
	fi
	if lsmod | grep -qE '^(zfs|spl) '; then
		echo "zfs or spl still loaded: stop" | tee -a $L/runs.txt
		return 1
	fi
	(cd $Z/$1 && sudo -n ./scripts/zfs.sh)
	echo "load $1 rc=$?" | tee -a $L/runs.txt
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/srcv-$1)" ] ||
	    [ "$(modinfo -F srcversion $Z/$1/module/zfs.ko)" != "$(cat $L/srcv-$1)" ]; then
		echo "$1 GUARD FAILED: stop" | tee -a $L/runs.txt
		return 1
	fi
	cp $L/srcv-$1 $L/loaded-srcv
	echo $1 > $L/loaded-tree
	for t in $TUN; do echo "$t=$(cat /sys/module/zfs/parameters/$t)"; done > $L/tunables-loaded.txt
	echo "$1 guard ok: $(cat $L/loaded-srcv); $(paste -sd' ' $L/tunables-loaded.txt)" \
	    "spl_panic_halt=$(cat /sys/module/spl/parameters/spl_panic_halt)" | tee -a $L/runs.txt
}

# The userspace the tests of <tree> used: their zpool must be the tree's
# own, and libzfs_pool.c must have <n> occurrences of the fix's check
# (2 on BRD, 0 elsewhere).
usr_check() {   # usr_check <tree> <n>
	local z n
	z=$(readlink -f $Z/$1/tests/zfs-tests/bin/zpool 2>&1)
	n=$(grep -c zpool_vdev_remove_check $Z/$1/lib/libzfs/libzfs_pool.c)
	echo "$1 userspace: tests' zpool -> $z, zpool_vdev_remove_check x$n" | tee -a $L/runs.txt
	if [ "$z" != "$Z/$1/zpool" ] || [ "$n" != "$2" ]; then
		echo "$1: not this tree's userspace (expected $Z/$1/zpool, x$2): stop" | tee -a $L/runs.txt
		return 1
	fi
}

# Kernel messages of one run, from the journal: ZTS clears dmesg after a
# failed test, so dmesg alone misses them.
kern_check() {   # kern_check <label> <since>
	sudo -n journalctl -k --since "$2" --no-pager -o short-iso > $L/kern-$1.txt 2>&1
	echo "kernel $1: $(grep -ciE "$KRE" $L/kern-$1.txt) suspicious lines" | tee -a $L/runs.txt
	grep -iE "$KRE" $L/kern-$1.txt | head -20 | sed 's/^/  /' | tee -a $L/runs.txt
	if grep -qE 'PANIC|VERIFY|ASSERT|BUG:|Oops' $L/kern-$1.txt; then
		echo "$1: panic in the kernel log: stop" | tee -a $L/runs.txt
		touch $L/PANIC
		return 1
	fi
}

# What dmesg still holds: a baseline-style count, not a per-run record.
dmesg_state() {   # dmesg_state <label>
	sudo -n dmesg --time-format=iso > $L/dmesg-$1.txt 2>&1
	echo "dmesg $1: $(grep -ciE "$KRE" $L/dmesg-$1.txt) suspicious lines in the buffer" \
	    "(baseline $(cat $L/dmesg-baseline.txt); a failed test clears it)" | tee -a $L/runs.txt
}

# The test's own lines in the runner's log (Test: lines carry a timestamp,
# and a test that writes no stderr has no "merged" file): each result that
# is not PASS, the line before the failure callbacks (the log_fail or
# log_must message), the lines matching $NRE, and the count of $CRE.
log_digest() {   # log_digest <results dir> <test name>
	awk -v n="$2" -v nre="$NRE" -v cre="$CRE" '
	function flush() {
		if (sec && cre != "") print "  " cnt " lines matching " cre
		if (bad && !done) print "    last line: " prev
	}
	/Test: .* \(run as [^)]*\) +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n ".ksh (") || index($0, "/" n " (")
		bad = sec && $0 !~ /\[PASS\]/
		cnt = 0; notes = 0; done = 0; prev = ""
		if (bad) print "  " $0
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	cre != "" && line ~ cre { cnt++ }
	bad && !done && line ~ /NOTE: Performing (test-fail callback|local cleanup)/ {
		print "    first failing line: " prev; done = 1
	}
	bad && nre != "" && line ~ nre && notes < 12 { print "    note: " line; notes++ }
	{ prev = line }
	END { flush() }' "$1/log"
}

# Leftovers of a run, from the root of the tree whose module is loaded.
# Pools, devices, mounts and processes may hold the module (HELD); files,
# users and tunables the tests own are theirs to restore. All of them stop.
leftovers() {   # leftovers <label>
	local bad=0 p f t
	{
	p=$(sudo -n ./zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; bad=1; touch $L/HELD; }
	sudo -n losetup -a | diff -q $L/losetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER loop devices:"; sudo -n losetup -a; bad=1; touch $L/HELD; }
	sudo -n dmsetup ls 2>&1 | diff -q $L/dmsetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER dm devices:"; sudo -n dmsetup ls; bad=1; touch $L/HELD; }
	grep -w zfs /proc/mounts && { bad=1; touch $L/HELD; }
	p=$(others 'test-runner\.py|zfs-tests\.sh|zfs-tests/tests/functional')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|tmp\.)'; then
		echo "  LEFTOVER files in $Z/files:" $f; bad=1
	elif [ -n "$f" ]; then
		echo "  other files in $Z/files (not fatal):" $f
	fi
	p=$(getent passwd $USERS; getent group $GROUPS_)
	[ -n "$p" ] && { echo "  LEFTOVER users or groups:"; echo "$p"; bad=1; }
	for t in $TUN; do echo "$t=$(cat /sys/module/zfs/parameters/$t)"; done |
	    diff -q $L/tunables-loaded.txt - > /dev/null ||
	    { echo "  LEFTOVER tunables:"; for t in $TUN; do
		echo "    $t=$(cat /sys/module/zfs/parameters/$t)"; done; bad=1; }
	echo "leftovers after $1: $([ $bad = 0 ] && echo none || echo 'FOUND: stop')"
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	return $bad
}

# Result counts of a zfs-tests.sh log, e.g. "PASS x1" or "PASS x22, FAIL x1".
counts() {   # counts <log>
	grep -E 'Test: .* \(run as [^)]*\) +\[[0-9:]+\] \[[A-Z]+\]' $1 |
	    grep -oE '(PASS|FAIL|SKIP|KILLED)\]$' | tr -d ']' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# Before any run: the loaded module is the one recorded.
pre_run() {   # pre_run <label>
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv 2>&1)" ]; then
		echo "$1: loaded srcversion is not $(cat $L/loaded-srcv 2>&1): stop" | tee -a $L/runs.txt
		return 1
	fi
}

# The end of every run: kernel messages, outer timeout, leftovers.
post_run() {   # post_run <label> <since> <rc>
	kern_check $1 "$2" || return 1
	if [ $3 -eq 124 ] || [ $3 -eq 137 ]; then
		echo "$1: outer timeout: stop" | tee -a $L/runs.txt
		return 1
	fi
	leftovers $1
}

# One run of one test, from the root of the loaded tree: the guard,
# zfs-tests.sh as this user under nice, ionice and an outer timeout, then
# the result, the digest of the runner's log, the kernel messages and the
# leftovers. Returns 1 when nothing more may run.
zts_run() {   # zts_run <label> <test name> <test path>
	local label=$1 name=$2 since t0 rc d
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -t $3 > $L/$label.log 2>&1
	rc=$?
	echo "$label $name $(counts $L/$label.log | grep . || echo 'NO RESULT')" \
	    "rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -E 'Test: ' $L/$label.log | grep -vE "/$name(\.ksh)? \(" | grep -v '\[PASS\]' |
	    sed 's/^/  not passed: /' | tee -a $L/runs.txt
	d=$(awk '/^Log directory/ {print $3}' $L/$label.log)
	if [ -f "$d/log" ]; then
		log_digest "$d" $name | tee -a $L/runs.txt
	else
		echo "  no runner log (Log directory: '${d}')" | tee -a $L/runs.txt
	fi
	post_run $label "$since" $rc
}

three() {   # three <label prefix> <test name> <test path>: runs <prefix>1, 2 and 3
	zts_run ${1}1 $2 $3 && zts_run ${1}2 $2 $3 && zts_run ${1}3 $2 $3
}

# One run of a tag group: the same guard, the counts, and the digest of
# every test that did not pass.
zts_group() {   # zts_group <label> <tag>
	local label=$1 since t0 rc d n
	pre_run $label || return 1
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -T $2 > $L/$label.log 2>&1
	rc=$?
	echo "$label -T $2 $(counts $L/$label.log | grep . || echo 'NO RESULT')" \
	    "rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -E 'Test: .* \(run as [^)]*\) +\[[0-9:]+\] \[[A-Z]+\]' $L/$label.log |
	    grep -v '\[PASS\]' |
	    sed -E 's|^.*Test: ([^ ]*/)?([^/ ]+) \(run as .*$|\2|; s|\.ksh$||' > $L/$label.notpassed
	d=$(awk '/^Log directory/ {print $3}' $L/$label.log)
	[ -s $L/$label.notpassed ] || echo "  every test passed" | tee -a $L/runs.txt
	for n in $(cat $L/$label.notpassed); do
		if [ -f "$d/log" ]; then
			log_digest "$d" $n | tee -a $L/runs.txt
		else
			echo "  $n: no runner log (Log directory: '${d}')" | tee -a $L/runs.txt
		fi
	done
	post_run $label "$since" $rc
}
ENV
. /var/log/zts-rmallow1026c/env.sh
mkdir -p $Z/files
date '+%F %T' > $L/start-time.txt
sudo -n losetup -a > $L/losetup-before.txt
sudo -n dmsetup ls > $L/dmsetup-before.txt 2>&1
sudo -n dmesg | grep -ciE "$KRE" > $L/dmesg-baseline.txt
sudo -n journalctl -k -b --no-pager | grep -ciE "$KRE" > $L/journal-baseline.txt
echo "baseline: dmesg $(cat $L/dmesg-baseline.txt), journal (this boot)" \
    "$(cat $L/journal-baseline.txt) suspicious lines, from before this task"
```

Clone once, fetch upstream master to check the merge bases, and make one
worktree per tree. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-rmallow1026c/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "zpool-remove-dry-run-checks" "$(git rev-parse origin/zpool-remove-dry-run-checks)" $HRD
same "zfs-allow-destroy-snapshots-test" "$(git rev-parse origin/zfs-allow-destroy-snapshots-test)" $HDS
same "parents" "$(git rev-parse $HRD^) $(git rev-parse $HDS^)" "$BASE $BASE"
same "merge bases with upstream master" \
    "$(git merge-base FETCH_HEAD $HRD) $(git merge-base FETCH_HEAD $HDS)" "$BASE $BASE"
same "files of the dry-run commit" "$(git diff --name-only $BASE $HRD | xargs)" \
    "lib/libzfs/libzfs_pool.c man/man8/zpool-remove.8 $WIRING $FRD"
same "files of the delegate commit" "$(git diff --name-only $BASE $HDS | xargs)" \
    "man/man8/zfs-allow.8 $WIRING $FDS"
same "module and header lines changed by either" \
    "$(git diff $BASE $HRD -- module include | wc -l) $(git diff $BASE $HDS -- module include | wc -l)" "0 0"
} | tee $L/sha-checks.txt
for t in MRD:$BASE BRD:$HRD BDS:$HDS; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done
```

Expected: `clone rc=0`, `fetch rc=0`, seven `ok:` lines, and three worktrees
with `rc=0` at `e117a84dbb` (MRD), `59fbe98d73` (BRD) and `45729d7354` (BDS).
Any `MISMATCH` (a branch was force-pushed since 26 Oct) means stop.

Copy the dry-run test into `MRD`, with its runfile and `Makefile.am` lines
(applied as the branch's own diff of those two files):

```sh
. /var/log/zts-rmallow1026c/env.sh
cd $Z/MRD && git checkout $HRD -- $FRD &&
    git diff $BASE $HRD -- $WIRING | git apply && echo "MRD: tests copied"
git diff HEAD --stat | tail -1
echo "MRD: $(git diff $HRD -- tests | wc -l) $(git diff HEAD -- module include lib cmd | wc -l)"
echo "MRD differs from the head in: $(git diff --name-only $HRD | xargs)"
for t in MRD BRD BDS; do
	echo "$t: $FRD $([ -e $Z/$t/$FRD ] && ls -l $Z/$t/$FRD | cut -c1-10 || echo absent)," \
	    "$FDS $([ -e $Z/$t/$FDS ] && ls -l $Z/$t/$FDS | cut -c1-10 || echo absent)," \
	    "fix x$(grep -c zpool_vdev_remove_check $Z/$t/lib/libzfs/libzfs_pool.c)"
done
```

Expected:

```
MRD: tests copied
 3 files changed, 172 insertions(+), 1 deletion(-)
MRD: 0 0
MRD differs from the head in: lib/libzfs/libzfs_pool.c man/man8/zpool-remove.8
MRD: tests/zfs-tests/tests/functional/removal/removal_dry_run.ksh -rwxr-xr-x, tests/zfs-tests/tests/functional/delegate/zfs_allow_destroy_snapshot.ksh absent, fix x0
BRD: tests/zfs-tests/tests/functional/removal/removal_dry_run.ksh -rwxr-xr-x, tests/zfs-tests/tests/functional/delegate/zfs_allow_destroy_snapshot.ksh absent, fix x2
BDS: tests/zfs-tests/tests/functional/removal/removal_dry_run.ksh absent, tests/zfs-tests/tests/functional/delegate/zfs_allow_destroy_snapshot.ksh -rwxr-xr-x, fix x0
```

(`MRD: 0 0`: the test files are the branch's, and no code changed. `MRD`
differs from the head only by the fix and its man page.) Anything else means
stop.

## STEP 2: build MRD and BRD, load MRD

```sh
. /var/log/zts-rmallow1026c/env.sh
build_tree MRD && build_tree BRD && switch_to MRD none
```

Expected: `build MRD rc=0`, `MRD built: srcversion ...`, `build BRD rc=0`,
possibly `BRD has the srcversion of MRD (same module code)`, `BRD built:
srcversion ...`, `load MRD rc=0`, `MRD guard ok: <srcversion>;
zfs_removal_suspend_progress=0 zfs_admin_snapshot=0 spl_panic_halt=0`. On a
failed build, the `error:` lines go into the report.

## STEP 3: runs on MRD (master `e117a84dbb` + the test)

```sh
. /var/log/zts-rmallow1026c/env.sh; cd $Z/MRD
three m removal_dry_run $RRD && usr_check MRD 0
dmesg_state MRD
```

`m1`-`m3`: **FAIL x1**, rc=1, with the first failing line `zpool remove -n
testpool <dir>/d1 succeeded: Memory that will be used after removing
<dir>/d1: <size>` (`<dir>` is the test's `mktemp -d`, under
`/var/tmp/zts-rmallow1026c/files`), the same line as a note, `0 lines matching
cannot remove`, kernel 0, leftovers none. Master's dry run reads only the
indirect size from the config, so for a vdev of a pool with a raidz it prints
the estimate and exits 0, where the real removal is refused with `cannot
remove <dir>/d1: invalid config; all top-level vdevs must have the same sector
size and not be raidz.`
The test fails at its first case, before any real removal; its cleanup
destroys the pool and removes its files. Then `MRD userspace: tests' zpool ->
/var/tmp/zts-rmallow1026c/MRD/zpool, zpool_vdev_remove_check x0`.

A failure at a `zpool create`, `zpool add`, `truncate` or `zpool replace`
before the first `zpool remove -n` is a setup failure: report it as such, it
proves nothing about the fix.

## STEP 4: BRD (head of zpool-remove-dry-run-checks)

Switch, then remove MRD:

```sh
. /var/log/zts-rmallow1026c/env.sh
switch_to BRD MRD && rm -rf /var/tmp/zts-rmallow1026c/MRD &&
    git -C $Z/repo worktree prune && echo "MRD removed"
```

```sh
. /var/log/zts-rmallow1026c/env.sh; cd $Z/BRD
three b removal_dry_run $RRD && usr_check BRD 2
dmesg_state BRD
```

`b1`-`b3`: **PASS x1**, rc=0, `12 lines matching cannot remove` (one note per
refused case: raidz, mixed ashift, the mirror with an active spare, the mirror
leaf, the spare in use, the log mirror leaf, the checkpoint twice, the
`device_removal` feature disabled, the legacy pool, the removal in progress
twice), kernel 0, leftovers none (the test resets
`zfs_removal_suspend_progress` to 0 itself). Then `BRD userspace: tests' zpool
-> /var/tmp/zts-rmallow1026c/BRD/zpool, zpool_vdev_remove_check x2`.

A `zpool remove -n: '<dry>', zpool remove: '<real>'` line on BRD is a real
finding, not a setup problem: the kernel refused with another error than the
one the static check predicted. Report both messages.

## STEP 5: BDS (head of zfs-allow-destroy-snapshots-test)

Build BDS while BRD is loaded, switch, then remove BRD:

```sh
. /var/log/zts-rmallow1026c/env.sh
build_tree BDS && switch_to BDS BRD && rm -rf /var/tmp/zts-rmallow1026c/BRD &&
    git -C $Z/repo worktree prune && echo "BRD removed"
```

```sh
. /var/log/zts-rmallow1026c/env.sh; cd $Z/BDS
three d zfs_allow_destroy_snapshot $RDS && zts_group dg delegate && usr_check BDS 0
dmesg_state BDS
```

| Label | Expected on BDS, and why |
| --- | --- |
| `d1`-`d3` | **PASS x1**, rc=0, kernel 0, leftovers none (the `delegate` cleanup deletes its users and groups and puts `zfs_admin_snapshot` back to 0). With `destroy,mount` for the file system and its descendents, `staff1` destroys the snapshot and the file system; with only one of the two, neither; with `-d`, the snapshot and a child but not the file system; with `-l`, the file system but not its snapshot. These expectations come from reading the code: a failure here means the test (and the man page text it backs) is wrong, and its first failing line says which case. |
| `dg` | `-T delegate`: 24 results (setup, 22 tests, cleanup), `zfs_allow_destroy_snapshot` among the PASS. Every result that is not PASS is reported with its first failing line. This branch changes no code, so any failure here is master's or this VM's, not the branch's; on 20 Oct this VM reported 18 PASS / 6 FAIL for the same group. A group where every test is SKIP after a setup note `Test user staff1 cannot execute zfs utilities` means the tree is not readable by other users: report it, change no permission. |

Then `BDS userspace: tests' zpool -> /var/tmp/zts-rmallow1026c/BDS/zpool,
zpool_vdev_remove_check x0`.

## What the results mean

- **zpool-remove-dry-run-checks, as expected** (M fails 3/3 at the raidz case,
  B passes 3/3 with 12 refusals, no kernel line, no leftover): the kernel
  confirms that the dry run now refuses what the kernel refuses, with the same
  message, and that master's dry run does not; the PR's "Not run" line can
  become this result, on its exact head.
- **Contradicted** by: an M run that passes (the test does not catch the bug);
  an M run that fails at setup; a B run that fails (report the messages); any
  panic, VERIFY or hung task.
- **zfs-allow-destroy-snapshots-test**: PASS 3/3 means the delegation
  behaviour the test and the man page describe is the kernel's own on Linux
  6.12. A FAIL means one of the expectations read from the code is wrong: the
  test and the man page are to be corrected, as its description says.
- **The delegate group**: a list, test by test, of what does not pass on
  master's code on this VM, with the reason each first failing line gives.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## STEP 6: whole-task kernel check

```sh
. /var/log/zts-rmallow1026c/env.sh
sudo -n journalctl -k --since "$(cat $L/start-time.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## STEP 7: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a panicked thread or a leftover pool, device or process keeps the
module busy, and its files may be in use: report them, and the maintainer
decides, reboot included):

```sh
. /var/log/zts-rmallow1026c/env.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
getent passwd $USERS; getent group $GROUPS_    # must print nothing
rm -rf /var/tmp/zts-rmallow1026c
ls -d /var/tmp/zts-rmallow1026c 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-rmallow1026c` and write
`/var/log/zts-rmallow1026c/REPORT.txt` with:

- STEP 0, short: anything found, `id -un`, `uname -r`, the distribution,
  `nproc`, the free space, and the STEP 1 baselines;
- `sha-checks.txt`, the three worktree lines and the STEP 1 test-file checks;
- per tree: build rc and wall time, srcversion (and whether it equals
  another tree's), and every `load`, `unload` and `guard` line (with the
  tunables and `spl_panic_halt`), and its `userspace` line;
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:`, first failing line and notes when it did not pass, its kernel line
  and its leftovers line, plus each `dmesg <tree>` line and the whole-task
  kernel count;
- for `dg`: the counts, and one line per test that did not pass with its
  first failing line;
- a summary table: each label's results against the expected ones, and which
  case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM 107, kernel ZTS of tigerblue77/zfs:zpool-remove-dry-run-checks and :zfs-allow-destroy-snapshots-test (VM-TESTS-107-1026c.md)

STEP 0: free; tigerblue77; <uname -r, a 6.12 kernel>; Debian GNU/Linux 13 (trixie); nproc 16;
  /var/tmp <N>G free; headers, journal, ksh, python3, git present; passwordless sudo; no staff/other users
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines

sources: clone rc=0, fetch rc=0, 7 ok lines (heads 59fbe98d73 45729d7354, parents and merge bases
  e117a84dbb, files as listed, no module or header line changed)
worktrees: MRD e117a84dbb, BRD 59fbe98d73, BDS 45729d7354
test files: MRD 3 files, +172 -1, 0 0; MRD differs from the head in libzfs_pool.c and zpool-remove.8;
  fix x0 / x2 / x0

builds (rc, wall, srcversion; same module code, equal or not):
  MRD 0 <s> <srcv>  BRD 0 <s> <srcv>  BDS 0 <s> <srcv>
loads: every "load <tree> rc=0", "<tree> guard ok: <srcv>; zfs_removal_suspend_progress=0
  zfs_admin_snapshot=0 spl_panic_halt=0", "unload <tree> rc=0"

runs:
m1 removal_dry_run FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-rmallow1026c/MRD/tests/zfs-tests/tests/functional/removal/removal_dry_run.ksh (run as root) [<mm:ss>] [FAIL]
    note: zpool remove -n testpool <dir>/d1 succeeded: Memory that will be used after removing <dir>/d1: <size>
    first failing line: zpool remove -n testpool <dir>/d1 succeeded: Memory that will be used after removing <dir>/d1: <size>
  0 lines matching cannot remove
kernel m1: 0 suspicious lines
leftovers after m1: none
m2, m3: the same
MRD userspace: tests' zpool -> /var/tmp/zts-rmallow1026c/MRD/zpool, zpool_vdev_remove_check x0
dmesg MRD: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
b1 removal_dry_run PASS x1 rc=0 wall=<s>
  12 lines matching cannot remove
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
BRD userspace: tests' zpool -> /var/tmp/zts-rmallow1026c/BRD/zpool, zpool_vdev_remove_check x2
dmesg BRD: <n>
d1 zfs_allow_destroy_snapshot PASS x1 rc=0 wall=<s>
  0 lines matching cannot remove
kernel d1: 0 suspicious lines
leftovers after d1: none
d2, d3: the same
dg -T delegate <PASS x.., FAIL x.., SKIP x..; 24 in all> rc=<0 or 1> wall=<s>
  <every test passed, or per test not passed: its Test: line and first failing line>
kernel dg: 0 suspicious lines
leftovers after dg: none
BDS userspace: tests' zpool -> /var/tmp/zts-rmallow1026c/BDS/zpool, zpool_vdev_remove_check x0
dmesg BDS: <n>
kernel, whole task: 0 suspicious lines

summary (3 runs each)                  M (MRD, master + test)   B (head)
  removal_dry_run                      FAIL 3/3                 PASS 3/3 (12 refusals)
  zfs_allow_destroy_snapshot           -                        PASS 3/3 (BDS)
  -T delegate (BDS, once)              <counts>; not passed: <names, or none>
  verdict: as expected for zpool-remove-dry-run-checks and zfs-allow-destroy-snapshots-test

cleanup: unload BDS rc=0, no zfs or spl module left, no staff/other user or group, /var/tmp/zts-rmallow1026c removed,
  /var/tmp <N>G free
deviations: none
```

Estimated duration: about 45 minutes (clone and fetch 3, three builds about
15, six dry-run runs about 8, three delegate runs 2, the delegate group about
10, checks and cleanup 5).
