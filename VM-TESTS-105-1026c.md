# VM 105: kernel ZTS for the fork branch bookmark-redacted-feature-check

These instructions are for the Claude session on VM 105. They give the pull
request that will be opened from the fork branch
`tigerblue77/zfs:bookmark-redacted-feature-check` the kernel results it lacks.
Its new ZTS test, `redacted_send/redacted_bookmark_feature`, was amended on 25
Oct (the receiving pool now has `bookmarks` enabled, so that the new check is
reached) and has not run in the kernel since; and it was never run without the
fix, because there master's `zfs bookmark` of a received redacted snapshot
trips `VERIFY3U(feature_get_refcount(...), !=, ENOTSUP)` in
`feature_do_action()` from syncing context, which leaves the pool hung in
`txg_sync` and the VM needing a reboot. The session builds two trees, runs one
test at a time, cleans up after itself and writes every result to
`/var/log/zts-bookmark1026c/REPORT.txt`, whose expected contents end this
document.

| Fork branch | Head | Merge base with upstream master | Commit |
| --- | --- | --- | --- |
| `bookmark-redacted-feature-check` | `c3f8b9a6b9` (1 commit) | `bc3fe48662` | `module/zfs/dsl_bookmark.c` (the fix), the new test, its `common.run` and `Makefile.am` lines |

| Tree | Code | Tests run | Labels |
| --- | --- | --- | --- |
| `BBK` | head `c3f8b9a6b9` | the new test as committed; the variant below; the `redacted_send` group once | `b1`-`b3`, `bz1`-`bz3`, `bg` |
| `MBK` | merge base `bc3fe48662` (no fix) | **the variant only**: the committed test is not even copied in | `mz1`-`mz3` |

**The variant, and why it cannot panic the VM.** `redacted_bookmark_feature_zcp.ksh`
(written in STEP 1, never committed) is the committed test with its first
`zfs bookmark`, the one that panics master, replaced by
`zfs.check.bookmark()` in a channel program run with `zfs program -n`. A
`zfs.check.*` function runs only the check function of its sync task
(`zcp_sync_task()` returns right after `checkfunc()` when `sync` is false), and
`-n` runs the whole program in open context, on a transaction that is aborted
afterwards (`zcp_eval_open()`). So `dsl_bookmark_create_check()` runs, and
`dsl_bookmark_create_sync()`, which calls `spa_feature_incr()`, never does.
Without the fix the check accepts the bookmark (it returns 0: that is the bug,
since the sync task would then reach the VERIFY); with the fix it refuses it
with `ENOTSUP` (95). The variant fails when the check returns 0, before any
`zfs bookmark`; its only `zfs bookmark` comes after `redaction_bookmarks` has
been enabled and checked as such, the case where master creates the bookmark
normally (one redaction snapshot, so `redaction_list_spill` is not needed). On
`BBK` the variant is the control: it must pass, the check returning 95.

The states run in the order B, M: `BBK`'s results do not depend on the variant,
and they are in hand before the only state that runs master's bookmark code.

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot.
- Work only under `/var/tmp/zts-bookmark1026c`, log to
  `/var/log/zts-bookmark1026c`. Never read, write or delete
  `/var/tmp/test_results` or anything else this task did not create: the
  results go to `/var/log/zts-bookmark1026c/results`. The only `rm -rf`
  allowed is the one written in STEP 6, with its literal path.
- On `MBK`, run nothing but the variant, and only through `three mz` as
  written: never `redacted_bookmark_feature.ksh`, never `-T`, never a
  `zfs bookmark` by hand. `zts_run` refuses anything else on `MBK`.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never `-j16`, and so do the tests,
  one at a time.
- All git work happens in STEP 1.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-bookmark1026c/env.sh`, so that it also works in a shell that
  keeps no state between commands.
- After any line that says `stop`, run nothing more: go to STEP 5 with what
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
ls -A /var/tmp; ls -d /var/tmp/zts-bookmark1026c /var/log/zts-bookmark1026c 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n losetup -a; sudo -n dmsetup ls 2>&1
sudo -n journalctl -k -n 1 --no-pager
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
- `/var/tmp/zts-bookmark1026c` or `/var/log/zts-bookmark1026c` already exists;
- `/var/tmp` has less than 10 GB free (two built trees);
- `journalctl -k` prints no kernel line: without the journal, the kernel
  messages of a failed test are lost;
- `ksh`, `python3` or `git` is missing.

Report what was found; that is the whole task then. The loop and dm devices
listed here are not ours: STEP 1 keeps them as the baseline of the leftover
checks. Record `uname -r`: these instructions were written for
`6.12.113+deb13-cloud-amd64`, and another 6.12 kernel is not a reason to stop.

## STEP 1: environment, sources, variant

```sh
sudo -n mkdir -p /var/log/zts-bookmark1026c/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-bookmark1026c
cat > /var/log/zts-bookmark1026c/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-bookmark1026c
L=/var/log/zts-bookmark1026c
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The branch's head and its merge base with upstream master (its parent).
HBK=c3f8b9a6b91e8657c901f86a31fcdaa477705049
BASE_BK=bc3fe48662e7aab8e639b328a6d37cf12b39e2b2
# The committed test and the variant, as files of the tree (F*) and as
# zfs-tests.sh -t arguments (R*).
FBK=tests/zfs-tests/tests/functional/redacted_send/redacted_bookmark_feature.ksh
RBK=tests/functional/redacted_send/redacted_bookmark_feature.ksh
FV=tests/zfs-tests/tests/functional/redacted_send/redacted_bookmark_feature_zcp.ksh
RV=tests/functional/redacted_send/redacted_bookmark_feature_zcp.ksh
# The only test MBK may run.
MONLY=redacted_bookmark_feature_zcp
# The tunable the redacted_send tests change and must restore.
TUN="zfs_allow_redacted_dataset_mount"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# The lines of a failed test worth quoting.
NRE='zfs program -n|bookmark_check_result|accepted the bookmark|refused the bookmark|cannot create bookmark|feature_do_action'
# One line per run of the variant whose check refused the bookmark.
CRE='refused the bookmark: returned 95'

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion (which must differ
# from every tree built before), and send its ZTS results to $L/results.
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
		cmp -s "$f" $L/srcv-$1 || continue
		echo "$1 has the srcversion of ${f##*/srcv-}: stop" | tee -a $L/runs.txt
		return 1
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
# Pools, devices, mounts and processes may hold the module (HELD); files
# and tunables the tests own are theirs to restore. All of them stop.
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
	p=$(others 'test-runner\.py|zfs-tests\.sh|zfs-tests/tests/functional|zfs program')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|tmp\.)'; then
		echo "  LEFTOVER files in $Z/files:" $f; bad=1
	elif [ -n "$f" ]; then
		echo "  other files in $Z/files (not fatal):" $f
	fi
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

# Checks before any run: the loaded module is the one recorded, and on MBK
# only the variant runs.
pre_run() {   # pre_run <label> <test name or -T tag>
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv 2>&1)" ]; then
		echo "$1: loaded srcversion is not $(cat $L/loaded-srcv 2>&1): stop" | tee -a $L/runs.txt
		return 1
	fi
	if [ "$(cat $L/loaded-tree)" = MBK ] && [ "$2" != "$MONLY" ]; then
		echo "$1: $2 refused on MBK, only $MONLY runs there: stop" | tee -a $L/runs.txt
		return 1
	fi
	if [ "$(cat $L/loaded-tree)" = MBK ] && [ -e $Z/MBK/$FBK ]; then
		echo "$1: $FBK exists on MBK: stop" | tee -a $L/runs.txt
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

# One run of one test, from the root of the loaded tree: the guards,
# zfs-tests.sh as this user under nice, ionice and an outer timeout, then
# the result, the digest of the runner's log, the kernel messages and the
# leftovers. Returns 1 when nothing more may run.
zts_run() {   # zts_run <label> <test name> <test path>
	local label=$1 name=$2 since t0 rc d
	pre_run $label $name || return 1
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

# One run of a tag group: the same guards, the counts, and the digest of
# every test that did not pass.
zts_group() {   # zts_group <label> <tag>
	local label=$1 since t0 rc d n
	pre_run $label "-T $2" || return 1
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
. /var/log/zts-bookmark1026c/env.sh
mkdir -p $Z/files
date '+%F %T' > $L/start-time.txt
sudo -n losetup -a > $L/losetup-before.txt
sudo -n dmsetup ls > $L/dmsetup-before.txt 2>&1
sudo -n dmesg | grep -ciE "$KRE" > $L/dmesg-baseline.txt
sudo -n journalctl -k -b --no-pager | grep -ciE "$KRE" > $L/journal-baseline.txt
echo "baseline: dmesg $(cat $L/dmesg-baseline.txt), journal (this boot)" \
    "$(cat $L/journal-baseline.txt) suspicious lines, from before this task"
```

Clone once, fetch upstream master to check the merge base, and make one
worktree per tree. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-bookmark1026c/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "bookmark-redacted-feature-check" "$(git rev-parse origin/bookmark-redacted-feature-check)" $HBK
same "its parent" "$(git rev-parse $HBK^)" $BASE_BK
same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HBK)" $BASE_BK
same "files of the commit" "$(git diff --name-only $BASE_BK $HBK | xargs)" \
    "module/zfs/dsl_bookmark.c tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am $FBK"
} | tee $L/sha-checks.txt
for t in MBK:$BASE_BK BBK:$HBK; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done
git diff --stat $BASE_BK $HBK | tail -1
for t in MBK BBK; do
	echo "$t: fix $(grep -c dsl_bookmark_redaction_list_check $Z/$t/module/zfs/dsl_bookmark.c)," \
	    "committed test $([ -e $Z/$t/$FBK ] && ls -l $Z/$t/$FBK | cut -c1-10 || echo absent)"
done
```

Expected:

```
clone rc=0
fetch rc=0
ok: bookmark-redacted-feature-check
ok: its parent
ok: merge base with upstream master
ok: files of the commit
worktree MBK rc=0 at bc3fe48662
worktree BBK rc=0 at c3f8b9a6b9
 4 files changed, 114 insertions(+), 16 deletions(-)
MBK: fix 0, committed test absent
BBK: fix 3, committed test -rwxr-xr-x
```

Any `MISMATCH` (the branch was force-pushed since 26 Oct) or other difference
means stop.

Write the variant, check it, and copy it into both trees. The exact sha256 of
the file is given for information (a tab turned into spaces on the way here
changes it); the gate is the sha256 with every run of blanks squeezed to one
space, which only a change of text or line breaks alters.

```sh
. /var/log/zts-bookmark1026c/env.sh
cat > $L/redacted_bookmark_feature_zcp.ksh <<'ZCP'
#!/bin/ksh
# SPDX-License-Identifier: CDDL-1.0

#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/tests/functional/redacted_send/redacted.kshlib

#
# Test VM only, never committed: redacted_bookmark_feature.ksh with its
# first "zfs bookmark" replaced by zfs.check.bookmark() in a read-only
# channel program.  That runs dsl_bookmark_create_check() in open context
# and never the sync task, so a module without the fix accepts the
# bookmark (returns 0) instead of reaching the VERIFY in
# feature_do_action().  No "zfs bookmark" runs unless the check refused
# the bookmark and redaction_bookmarks is enabled.
#

typeset ds_name="bookmark_feature"
typeset sendfs="$POOL/$ds_name"
typeset recvfs="$POOL2/$ds_name"
typeset clone="$POOL/${ds_name}_clone"
typeset tmpdir="$(get_prop mountpoint $POOL)/tmp"
typeset stream=$(mktemp $tmpdir/stream.XXXX)
typeset lua=$tmpdir/check_bookmark.lua
typeset out rc rv
setup_dataset $ds_name ''

function cleanup
{
	destroy_pool $POOL2
	create_pool $POOL2 $DISK2
	log_must zfs snapshot $POOL2@init
	rm -f $lua
	redacted_cleanup $sendfs
}

log_onexit cleanup

log_must zfs redact $sendfs@snap book1 $clone@snap
log_must eval "zfs send --redact book1 $sendfs@snap >$stream"

destroy_pool $POOL2
log_must zpool create -d $POOL2 $DISK2
log_must zpool set feature@bookmarks=enabled $POOL2
log_must zpool set feature@redacted_datasets=enabled $POOL2
log_must eval "zfs recv $recvfs <$stream"
log_must test "$(get_pool_prop feature@redaction_bookmarks $POOL2)" = \
    "disabled"

cat >$lua <<'EOF'
args = ...
argv = args["argv"]
local err = zfs.check.bookmark(argv[1], argv[2])
return {bookmark_check_result = err}
EOF
out=$(zfs program -n $POOL2 $lua $recvfs@snap $recvfs#book 2>&1)
rc=$?
log_note "zfs program -n, zfs.check.bookmark: rc $rc, output:" $out
(( rc == 0 )) || log_fail "zfs program -n failed: rc $rc"
rv=$(echo "$out" | grep -oE 'bookmark_check_result[^0-9]*[0-9]+' |
    grep -oE '[0-9]+$')
log_must zpool sync $POOL2
log_mustnot bkmarkexists $recvfs#book
log_must test "$(get_pool_prop feature@redaction_bookmarks $POOL2)" = \
    "disabled"
[[ -n "$rv" ]] || log_fail "zfs.check.bookmark returned no result"
[[ "$rv" != 0 ]] || log_fail "zfs.check.bookmark accepted the bookmark" \
    "(returned 0): zfs bookmark would reach feature_do_action()"
log_note "zfs.check.bookmark refused the bookmark: returned $rv"

log_must zpool set feature@redaction_bookmarks=enabled $POOL2
log_must test "$(get_pool_prop feature@redaction_bookmarks $POOL2)" = \
    "enabled"
log_must zfs bookmark $recvfs@snap $recvfs#book
log_must bkmarkexists $recvfs#book
log_must test "$(get_pool_prop feature@redaction_bookmarks $POOL2)" = \
    "active"

log_pass "The bookmark check refuses a redacted snapshot without" \
    "redaction_bookmarks."
ZCP
echo "exact $(sha256sum < $L/redacted_bookmark_feature_zcp.ksh | cut -c1-16)"
echo "squeezed $(tr -s ' \t' ' ' < $L/redacted_bookmark_feature_zcp.ksh | sha256sum | cut -c1-16)"
ksh -n $L/redacted_bookmark_feature_zcp.ksh; echo "ksh -n rc=$?"
grep -nE '^[^#]*zfs bookmark \$|accepted the bookmark' $L/redacted_bookmark_feature_zcp.ksh
for t in MBK BBK; do
	cp $L/redacted_bookmark_feature_zcp.ksh $Z/$t/$FV && chmod 755 $Z/$t/$FV &&
	    echo "$t: variant copied, $(ls -l $Z/$t/$FV | cut -c1-10)"
	git -C $Z/$t status --porcelain
done
```

Expected:

```
exact 6653d10065efcd9a
squeezed 7baf3d396feef92b
ksh -n rc=0
76:[[ "$rv" != 0 ]] || log_fail "zfs.check.bookmark accepted the bookmark" \
83:log_must zfs bookmark $recvfs@snap $recvfs#book
MBK: variant copied, -rwxr-xr-x
?? tests/zfs-tests/tests/functional/redacted_send/redacted_bookmark_feature_zcp.ksh
BBK: variant copied, -rwxr-xr-x
?? tests/zfs-tests/tests/functional/redacted_send/redacted_bookmark_feature_zcp.ksh
```

(`ksh -n` may print style warnings; only its rc matters. The two `grep` lines
show that the variant's only `zfs bookmark`, line 83, comes after its
`log_fail` on a check that returned 0, line 76.) A different `squeezed` value,
another `rc`, or other `grep` lines mean stop: the variant was not received as
written.

## STEP 2: builds

```sh
. /var/log/zts-bookmark1026c/env.sh
build_tree BBK && build_tree MBK && echo "trees built"
```

Expected: `build BBK rc=0`, `BBK built: srcversion ...`, `build MBK rc=0`,
`MBK built: srcversion ...` (two different srcversions: `dsl_bookmark.c`
differs), then `trees built`. On a failed build, the `error:` lines go into the
report.

## STEP 3: BBK (head, the fix)

```sh
. /var/log/zts-bookmark1026c/env.sh
switch_to BBK none
```

Expected: `load BBK rc=0` and `BBK guard ok: <srcversion>;
zfs_allow_redacted_dataset_mount=0 spl_panic_halt=0`.

```sh
. /var/log/zts-bookmark1026c/env.sh; cd $Z/BBK
three b redacted_bookmark_feature $RBK &&
    three bz redacted_bookmark_feature_zcp $RV &&
    zts_group bg redacted_send
grep -h 'zfs.check.bookmark: rc' $(awk '/^Log directory/ {print $3}' $L/bz1.log)/log |
    head -1 | sed 's/^/bz1 program note: /' | tee -a $L/runs.txt
dmesg_state BBK
```

| Label | Expected on BBK, every run, and why |
| --- | --- |
| `b1`-`b3` | **PASS x1**, rc=0, `0 lines matching refused the bookmark: returned 95`, kernel 0, leftovers none. `zfs bookmark` is refused (ENOTSUP) while `redaction_bookmarks` is disabled, leaves it disabled, and succeeds once it is enabled. |
| `bz1`-`bz3` | **PASS x1**, rc=0, `1 lines matching refused the bookmark: returned 95`, kernel 0, leftovers none: the check refuses with ENOTSUP, creates nothing, and the bookmark is created once the feature is enabled. |
| `bg` | **PASS x23** (setup, the 21 tests including `redacted_bookmark_feature`, cleanup), rc=0, `every test passed`, kernel 0, leftovers none. On 20 Oct this group passed with an earlier version of the branch (test before the amendment). |

## STEP 4: MBK (merge base, no fix: the variant only)

```sh
. /var/log/zts-bookmark1026c/env.sh
switch_to MBK BBK
```

Expected: `unload BBK rc=0`, `load MBK rc=0`, `MBK guard ok: <srcversion>;
zfs_allow_redacted_dataset_mount=0 spl_panic_halt=0`.

```sh
. /var/log/zts-bookmark1026c/env.sh; cd $Z/MBK
three mz redacted_bookmark_feature_zcp $RV
grep -h 'zfs.check.bookmark: rc' $(awk '/^Log directory/ {print $3}' $L/mz1.log)/log |
    head -1 | sed 's/^/mz1 program note: /' | tee -a $L/runs.txt
dmesg_state MBK
```

`mz1`-`mz3`: **FAIL x1**, rc=1, with:

- the note `zfs program -n, zfs.check.bookmark: rc 0, output: Channel program
  fully executed and produced output: <...> bookmark_check_result: 0`;
- first failing line `zfs.check.bookmark accepted the bookmark (returned 0):
  zfs bookmark would reach feature_do_action()`;
- `0 lines matching refused the bookmark: returned 95`, kernel 0, leftovers
  none.

Master's check accepts the bookmark, so its sync task would run and trip the
VERIFY; the variant stops there and never runs `zfs bookmark`. The test's
`log_onexit` cleanup recreates `testpool2` as the redacted_send setup left it.

A failure *before* the `zfs program` line (at `zfs redact`, `zfs send`,
`zpool create -d`, `zpool set`, `zfs recv`, or the `disabled` check) is a
setup failure on master: it proves nothing about the check; report it as such.

## What the results mean

- **As expected** (BBK passes 3/3, 3/3 and the group; MBK's variant fails 3/3
  on `accepted the bookmark (returned 0)`; no kernel line, no leftover): the
  kernel confirms that the fix makes the bookmark check refuse a redacted
  snapshot when `redaction_bookmarks` is disabled (ENOTSUP), and that master's
  check accepts it, which is what lets `zfs bookmark` go on to the VERIFY. The
  pull request can say so, and still that the committed test was not run on
  master, and why.
- **MBK's variant passes** (the check returns non-zero on master): master's
  kernel already refuses this bookmark at the check, which contradicts the bug
  as described; report the returned value. **It fails on `returned no result`
  or `zfs program -n failed`**: the channel program did not run as designed;
  report its output, nothing is proved either way.
- **A BBK run fails**: report the failing lines run by run; on `bz`, a check
  returning anything but 95 is reported with its value.
- **Any kernel VERIFY, panic or hung task**: the line and the state it
  happened in are the result. Stop.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## STEP 5: whole-task kernel check

```sh
. /var/log/zts-bookmark1026c/env.sh
sudo -n journalctl -k --since "$(cat $L/start-time.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## STEP 6: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a panicked thread or a leftover pool, device or process keeps the
module busy, and its files may be in use: report them, and the maintainer
decides, reboot included):

```sh
. /var/log/zts-bookmark1026c/env.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
rm -rf /var/tmp/zts-bookmark1026c
ls -d /var/tmp/zts-bookmark1026c 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-bookmark1026c` and write
`/var/log/zts-bookmark1026c/REPORT.txt` with:

- STEP 0, short: anything found, `id -un`, `uname -r`, the distribution,
  `nproc`, the free space, and the STEP 1 baselines;
- `sha-checks.txt`, the two worktree lines, the STEP 1 tree checks and the
  variant checks (both hashes, `ksh -n rc`, the two `grep` lines, the copies);
- per tree: build rc and wall time, srcversion, and every `load`, `unload` and
  `guard` line (with the tunable and `spl_panic_halt`);
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:` and first failing line and notes when it did not pass, its kernel
  line and its leftovers line, plus each `dmesg <tree>` line and the
  whole-task kernel count;
- for `bz1` and `mz1`, the `zfs program` note line as the log has it;
- a summary table: each label's three results against the expected ones, and
  which case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM 105, kernel ZTS of tigerblue77/zfs:bookmark-redacted-feature-check (VM-TESTS-105-1026c.md)

STEP 0: free; tigerblue77; <uname -r, a 6.12 kernel>; Debian GNU/Linux 13 (trixie); nproc 16;
  /var/tmp <N>G free; headers, journal, ksh, python3, git present; passwordless sudo
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines

sources: clone rc=0, fetch rc=0, 4 ok lines (head c3f8b9a6b9, parent and merge base bc3fe48662,
  files dsl_bookmark.c + the test and its wiring)
worktrees: MBK bc3fe48662, BBK c3f8b9a6b9; commit 4 files +114 -16
trees: MBK fix 0, committed test absent; BBK fix 3, committed test -rwxr-xr-x
variant: exact <6653d10065efcd9a or other>, squeezed 7baf3d396feef92b, ksh -n rc=0,
  grep 76 (log_fail on 0) and 83 (the only zfs bookmark); copied into MBK and BBK, -rwxr-xr-x, one ?? line each

builds (rc, wall, srcversion; both distinct): BBK 0 <s> <srcv>  MBK 0 <s> <srcv>
loads: "load BBK rc=0", "BBK guard ok: <srcv>; zfs_allow_redacted_dataset_mount=0 spl_panic_halt=0",
  "unload BBK rc=0", the same for MBK

runs:
b1 redacted_bookmark_feature PASS x1 rc=0 wall=<s>
  0 lines matching refused the bookmark: returned 95
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
bz1 redacted_bookmark_feature_zcp PASS x1 rc=0 wall=<s>
  1 lines matching refused the bookmark: returned 95
kernel bz1: 0 suspicious lines
leftovers after bz1: none
bz2, bz3: the same
bg -T redacted_send PASS x23 rc=0 wall=<s>
  every test passed
kernel bg: 0 suspicious lines
leftovers after bg: none
bz1 program note: <time> NOTE: zfs program -n, zfs.check.bookmark: rc 0, output: <...> bookmark_check_result: 95
dmesg BBK: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
mz1 redacted_bookmark_feature_zcp FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-bookmark1026c/MBK/tests/zfs-tests/tests/functional/redacted_send/redacted_bookmark_feature_zcp.ksh (run as root) [<mm:ss>] [FAIL]
    note: NOTE: zfs program -n, zfs.check.bookmark: rc 0, output: <...> bookmark_check_result: 0
    note: zfs.check.bookmark accepted the bookmark (returned 0): zfs bookmark would reach feature_do_action()
    first failing line: zfs.check.bookmark accepted the bookmark (returned 0): zfs bookmark would reach feature_do_action()
  0 lines matching refused the bookmark: returned 95
kernel mz1: 0 suspicious lines
leftovers after mz1: none
mz2, mz3: the same
mz1 program note: <time> NOTE: zfs program -n, zfs.check.bookmark: rc 0, output: <...> bookmark_check_result: 0
dmesg MBK: <n> suspicious lines in the buffer
kernel, whole task: 0 suspicious lines

summary (3 runs each)                     BBK (c3f8b9a6b9)   MBK (bc3fe48662)
  redacted_bookmark_feature               PASS 3/3           not run (would panic)
  redacted_bookmark_feature_zcp           PASS 3/3 (95)      FAIL 3/3 (check returned 0)
  -T redacted_send                        PASS 23/23         not run
  verdict: as expected; the fix refuses the bookmark at the check in the kernel,
  and master's check accepts it

cleanup: unload MBK rc=0, no zfs or spl module left, /var/tmp/zts-bookmark1026c removed, /var/tmp <N>G free
deviations: none
```

Estimated duration: about 45 minutes (clone and fetch 3, two builds about 10,
nine single runs about 10, the group about 15, checks and cleanup 5).
