# VM 107, part B: kernel runs for ddt-prune-extended

These instructions are for the Claude session on VM 107. They continue
`PART-A-107.md` (sha256 `61f5ff02a101205b2ad734ebbc412a0347c86843ecb75fe26e419095e1240b88`),
which left two built trees of the fork branch
`tigerblue77/zfs:ddt-prune-extended` (open upstream pull request
openzfs/zfs#19549, Closes #19443) under `/var/tmp/zts-ddtprune-1026d`, and its
records under `/var/log/zts-ddtprune-1026d`. Part B rebuilds nothing: it checks
that the trees are exactly the ones part A recorded, then loads each tree's
modules behind a srcversion guard, runs the ZTS test
`functional/dedup/dedup_prune_leak` three times per tree, reads the kernel log
of every run from the journal, checks for leftovers, cleans up and writes
`/var/log/zts-ddtprune-1026d/REPORT.txt`, whose expected contents end this
document.

| Tree | Code | Runs | Expected |
| --- | --- | --- | --- |
| `B` | head `983709b6a2` (the fix in `module/zfs/ddt.c`, the man page, the test) | `b1`-`b3`, then `bg` (`-T dedup` once) | PASS 3/3, 4 `No leaks` per run; `bg` all PASS |
| `M` | master `bc3fe48662` (the head's parent and merge base) + the head's `dedup_prune_leak.ksh` | `m1`-`m3` (and `mg` only if `bg` had a failure) | FAIL 3/3 at the new case's first leak check |

The states run in the order B, M.

What the test does. Its two existing cases (prune 128 unique entries and free
their blocks, then the same with ganging forced) are master's own and pass on
both trees. The new case creates `$TESTPOOL/$TESTFS1` with `dedup=on`,
`copies=1` and `compression=off`, writes a 128 KiB random block as file `f3`,
sets `copies=2`, writes the same block as `f4` (the dedup write finds the entry
with one copy and adds a second, which `f3`'s block pointer does not have),
removes `f4`, syncs, waits 1 s, runs `zpool ddtprune -p 100`, syncs and runs
`zdb -bcc`, failing on a non-zero exit or a `leaked space` line. It then
removes `f3` and checks `zdb -bcc` again.

**Why M fails, and why that cannot hurt the VM.** On master the prune walk
clears the entry, which still holds both copies, without freeing anything; the
second copy is then referenced by nothing and stays allocated. `zdb -bcc`
prints `leaked space: vdev <v>, offset 0x<o>, size 131072` and
`block traversal size <n> != alloc <n + 131072> (leaked 131072)`, and exits 0
(master's zdb does not count space map leaks in its exit status, a separate
issue), so the test's `grep` is what fails it. The leak is 128 KiB of a test
pool that the test's cleanup destroys; nothing in the kernel stops or panics.
With the fix the entry, holding two copies, is not pruned, and both `zdb` runs
print `No leaks (block sum matches space maps exactly)`. Both behaviours were
reproduced in userspace (libzpool, the same sequence, `zdb -bcc -e`) on these
exact two commits before this was written: master leaked 131072 bytes with
`zdb` exiting 0, the head did not.

Rules for the whole task (part A's still hold):

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot. Rebuild nothing: a tree that does not verify is a stop.
- Work only under `/var/tmp/zts-ddtprune-1026d`, log to
  `/var/log/zts-ddtprune-1026d`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`): the results go to `/var/log/zts-ddtprune-1026d/results`.
  The only `rm -rf` allowed is the one written in B5, with its literal path.
- `/etc/zfs/zpool.cache` may exist as a 0-byte file left by earlier tasks.
  Leave it alone: never delete, edit or rewrite it.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the tests run under
  `nice -n 10 ionice -c3`, one at a time.
- Every block from B1 on starts by sourcing part A's
  `/var/log/zts-ddtprune-1026d/env.sh` and then part B's `envb.sh`.
- After any line that says `stop`, run nothing more: go to B4 and B5 with what
  you have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

What earlier VM runs taught, and these steps already handle:

- The runner prefixes every `Test:` line with a timestamp: results are matched
  anywhere in the line.
- A test that writes nothing on stderr has no `merged` file: the failing line
  and the notes are read from the runner's own `log` file.
- On a failed test, the ZTS callbacks print and then clear dmesg: kernel
  messages are read per run from `journalctl -k`, and a dmesg count is only a
  per-state cross-check.
- Each run waits 2 s first: two runs started in the same second would share a
  results directory.
- `log_must` ends a test at its first failure, so the first failing line is
  the whole story of a failed run.
- `-t` adds the directory's `setup` and `cleanup` around the test: the result
  of a single run is read for the named test only.
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`,
  never `\(`; the patterns below contain none.
- `zfs-tests.sh -d` must point at a mode 1777 directory: part A made
  `$Z/files` so, and B0 checks it.

## B0: the VM must still be free, and part A complete

```sh
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-ddtprune-1026d; L=/var/log/zts-ddtprune-1026d
id -un; uname -r
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
ls -l /etc/zfs/zpool.cache 2>&1
sudo -n journalctl -k -n 1 --no-pager
echo "Z holds: $(LC_ALL=C ls -A $Z | xargs)"
echo "files: mode $(stat -c %a $Z/files), $(ls -A $Z/files | wc -l) entries"
for f in env.sh partA.txt sha-checks.txt copy.txt srcv-B srcv-M tree-B.txt tree-M.txt \
    bins-B.sha256 bins-M.sha256 PART-A.done; do
	[ -s $L/$f ] || echo "missing or empty: $L/$f"
done
echo "results dir: $(ls -A $L/results | wc -l) entries"
echo "env.sh: sha256 $(sha256sum < $L/env.sh | cut -d' ' -f1)"
echo "env.sh without blanks: $(tr -d ' \t' < $L/env.sh | sha256sum | cut -d' ' -f1)"
echo "part A: $(grep -cx 'trees built and recorded' $L/partA.txt) done line, $(grep -c MISMATCH $L/sha-checks.txt) mismatch, finished $(cat $L/PART-A.done)"
ls $L/envb.sh $L/partB-start.txt 2>&1
```

Expected: no module, no `/dev/zfs`, no other test or build process, no busy
tmux pane, `Z holds: B M files repo`, `files: mode 1777, 0 entries`, no
`missing or empty` line, `results dir: 0 entries`,
`env.sh: sha256 9615c7f2d75bfe99f94933c47633bc5427ffbe23544d774195b3d599c40fb9b6`
(or, if only blanks differ, `env.sh without blanks:
e4b0f74a687aec54dc2214632627d880a961edea2a085d33bad38edcb85caaa7`),
`part A: 1 done line, 0 mismatch, finished <date>`, and `No such file or
directory` for `envb.sh` and `partB-start.txt`.

Stop at once, changing nothing, if: `id -un` is not `tigerblue77` or
`sudo -n true` fails; a zfs or spl module is loaded or `/dev/zfs` exists; a
test or build process runs, other than this command and the shell running it;
a tmux pane other than this session's runs anything; `/var/tmp` has less than
8 GB free; `journalctl -k` prints no kernel line; or any other expected line
above differs (part A did not finish, or something changed its output since:
report it, the maintainer decides). `envb.sh` existing means part B already
started once: report, do not start again.

## B1: part B's helpers, baselines, and the check of part A's trees

```sh
. /var/log/zts-ddtprune-1026d/env.sh
cat > $L/envb.sh <<'ENVB'
# Part B's helpers. env.sh (part A's) is sourced first.
# The tunables the dedup tests change and must restore.
TUN="zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct zfs_bclone_enabled"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# The lines of a failed test worth quoting: zdb's leak report, the test's
# failure messages, and a failed log_must.
NRE='leaked space|block traversal size|DDT leak|DOUBLE|leaks the copy|leaks space|causes space leak|not pruned|exited [0-9]'
# One line per zdb -bcc that found no leak (4 when the test passes).
CRE='No leaks'

# Part A's record of <tree> against the tree as it is now: HEAD, changes,
# srcversion, the built modules, libraries and programs, and the results
# directory. Any MISMATCH means this is not the tree part A built.
verify_tree() {   # verify_tree <tree> <expected head> <expected changed files>
	local s
	cd $Z/$1 || return 1
	{
	same "$1 head" "$(git rev-parse HEAD)" "$2"
	same "$1 head as recorded" "$(sed -n 's/^head //p' $L/tree-$1.txt)" "$2"
	same "$1 changes as recorded" "$(git diff HEAD | sha256sum | cut -d' ' -f1)" \
	    "$(sed -n 's/^diff //p' $L/tree-$1.txt)"
	same "$1 changed files" "$(git diff HEAD --name-only | xargs)" "$3"
	same "$1 srcversion of zfs.ko" "$(modinfo -F srcversion module/zfs.ko)" "$(cat $L/srcv-$1)"
	same "$1 srcversion in tree-$1.txt" "$(sed -n 's/^srcversion //p' $L/tree-$1.txt)" \
	    "$(cat $L/srcv-$1)"
	s=$(sha256sum -c --quiet $L/bins-$1.sha256 2>&1 | head -5)
	same "$1 built files ($(wc -l < $L/bins-$1.sha256) hashed)" "${s:-unchanged}" unchanged
	same "$1 results directory" \
	    "$(grep -c "^BASEDIR = '$L/results'\$" tests/test-runner/bin/test-runner.py)" 1
	} | tee -a $L/verify.txt
	! grep -q "^MISMATCH: $1 " $L/verify.txt
}

# Load <tree>'s modules. Whatever is loaded must be <previous>'s, and is
# unloaded first; <previous> is "none" for the first load. No pool may
# appear at load (one would come from a cache file, and is not ours).
switch_to() {   # switch_to <tree> <previous>
	local cur t p
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
	p=$(cd $Z/$1 && sudo -n ./zpool list -H -o name 2>&1)
	if [ -n "$p" ]; then
		echo "$1: pools present right after the load: $p: stop" | tee -a $L/runs.txt
		touch $L/HELD
		return 1
	fi
	echo "$1: no pool after the load" | tee -a $L/runs.txt
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
	p=$(others 'test-runner\.py|zfs-tests\.sh|zfs-tests/tests/functional|zdb ')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|dedup_prune_leak\.blk)'; then
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

# Result counts of a zfs-tests.sh log, e.g. "PASS x18" or "PASS x17, FAIL x1".
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
# the named test's result (-t adds the directory's setup and cleanup, which
# are listed only when they do not pass), the digest of the runner's log,
# the kernel messages and the leftovers. Returns 1 when nothing more may run.
zts_run() {   # zts_run <label> <test name> <test path>
	local label=$1 name=$2 since t0 rc d res
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -t $3 > $L/$label.log 2>&1
	rc=$?
	res=$(grep -E "Test: .*/$name(\.ksh)? \(run as [^)]*\) +\[[0-9:]+\] \[[A-Z]+\]" $L/$label.log |
	    grep -oE '(PASS|FAIL|SKIP|KILLED)\]$' | tr -d ']' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }')
	echo "$label $name ${res:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
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
ENVB
. $L/envb.sh
date '+%F %T' > $L/partB-start.txt
sudo -n losetup -a > $L/losetup-before.txt
sudo -n dmsetup ls > $L/dmsetup-before.txt 2>&1
sudo -n dmesg | grep -ciE "$KRE" > $L/dmesg-baseline.txt
sudo -n journalctl -k -b --no-pager | grep -ciE "$KRE" > $L/journal-baseline.txt
echo "baseline: dmesg $(cat $L/dmesg-baseline.txt), journal (this boot)" \
    "$(cat $L/journal-baseline.txt) suspicious lines, from before part B"
```

Then check both trees against part A's record, without rebuilding:

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh
{
same "srcversions of B and M" "$(cmp -s $L/srcv-B $L/srcv-M && echo equal || echo differ)" differ
same "M's test file is the head's" "$(cmp -s $Z/M/$F $Z/B/$F && echo same || echo different)" same
} | tee -a $L/verify.txt
verify_tree B $HB "tests/test-runner/bin/test-runner.py.in" &&
    verify_tree M $HM "tests/test-runner/bin/test-runner.py.in $F" &&
    ! grep -q '^MISMATCH' $L/verify.txt && echo "trees verified" | tee -a $L/verify.txt
```

Expected: 18 `ok:` lines (2, then 8 for B, then 8 for M, the two `built files`
lines with the number of files part A hashed), then `trees verified`. Any
`MISMATCH` means stop: the tree is not the one part A built and recorded.

## B2: state B (head, the fix)

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh
switch_to B none
```

Expected: `load B rc=0`, `B guard ok: <srcversion>; zfs_dedup_log_txg_max=8
zfs_dedup_log_flush_entries_min=200
zfs_dedup_log_flush_entries_max=4294967295 metaslab_force_ganging=16777217
metaslab_force_ganging_pct=3 zfs_bclone_enabled=1 spl_panic_halt=0` (these
are the module's defaults; other values set by this VM's configuration are
fine, since each run is compared with what the load found), then
`B: no pool after the load`.

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh; cd $Z/B
three b dedup_prune_leak $R && zts_group bg dedup
dmesg_state B
```

| Label | Expected on B |
| --- | --- |
| `b1`-`b3` | **PASS x1**, rc=0, `4 lines matching No leaks` (the four `zdb -bcc` of the test), no `not passed` line (the dedup directory's `setup` and `cleanup` pass), kernel 0, leftovers none, a minute or two each. |
| `bg` | `-T dedup`, once: **PASS x18** (the 15 tests of `common.run`'s dedup group, which has no setup or cleanup, then `linux.run`'s `setup`, `dedup_legacy_gang` and `cleanup`), `every test passed`, kernel 0, leftovers none. The fork's CI ran the whole suite on this head with no unexpected result; a failure here is reported test by test with its first failing line, and B3 then runs the same group on M. |

## B3: state M (master + the test)

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh
switch_to M B
```

Expected: `unload B rc=0`, `load M rc=0`, `M guard ok: <srcversion>; <the same
tunables> spl_panic_halt=0`, `M: no pool after the load`.

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh; cd $Z/M
if three m dedup_prune_leak $R; then
	if [ -s $L/bg.notpassed ]; then
		zts_group mg dedup
	else
		echo "mg not run: every test of bg passed" | tee -a $L/runs.txt
	fi
fi
dmesg_state M
```

`m1`-`m3`: **FAIL x1**, rc=1, each with

```
  [<time>] Test: /var/tmp/zts-ddtprune-1026d/M/tests/zfs-tests/tests/functional/dedup/dedup_prune_leak.ksh (run as root) [<mm:ss>] [FAIL]
    note: leaked space: vdev <v>, offset 0x<o>, size 131072
    note: block traversal size <n> != alloc <n + 131072> (leaked 131072)
    note: DDT pruning leaks the copy an extended entry gained
    first failing line: DDT pruning leaks the copy an extended entry gained
  2 lines matching No leaks
```

then kernel 0 and leftovers none (the test's cleanup destroys the pool,
removes `dedup_prune_leak.blk` and restores the four tunables it changed),
and `mg not run: every test of bg passed`. The two `No leaks` are the
existing cases, which pass on master; the new case fails at its first `zdb`,
so the second (after removing `f3`) is not reached. The `vdev` and `offset`
vary; the size is the 128 KiB block's second copy.

A failure before the new case (the first two cases' messages `DDT pruning
causes space leak`, `DDT entries not pruned`, or a `log_must ... exited` on a
`zpool`, `zfs`, `dd` or `cp`) is a setup or environment failure: report it as
such, it proves nothing about the fix.

## What the results mean

- **As expected** (B passes 3/3 with 4 `No leaks` each and the dedup group
  passes; M fails 3/3 at `DDT pruning leaks the copy an extended entry
  gained` with a `leaked space ... size 131072` note; no kernel line, no
  leftover): the kernel confirms that on master `zpool ddtprune` leaks the
  copy an extended entry gained, and that with the fix the entry is kept and
  nothing leaks, also once the remaining block is freed. The pull request's
  "The new case was not run in ZTS without the fix" can become this result,
  on its exact head.
- **M passes, 1, 2 or 3 times**: the prune did not take the entry on this VM
  (the entry was not yet in the unique class, or not older than the cutoff);
  report the counts and the `No leaks` count of each run. The test is then not
  a reliable regression test here, and the pull request must not claim more.
- **B fails**: report the failing lines run by run. `Freeing the block of a
  kept extended entry leaks space` would mean the kept entry's free path
  leaks; `DDT pruning leaks the copy an extended entry gained` on B would mean
  the fix did not hold.
- **A `bg` failure**: compare with `mg`, test by test. A test failing on both
  is master's or this VM's; failing on B only is the branch's.
- **Any kernel VERIFY, panic or hung task on either state**: a different
  problem; the line and the state it happened in are the result. Stop.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## B4: whole-task kernel check

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh
sudo -n journalctl -k --since "$(cat $L/partB-start.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole of part B: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## B5: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a panicked thread or a leftover pool, device or process keeps the
module busy, and its files may be in use: report them, and the maintainer
decides, reboot included):

```sh
. /var/log/zts-ddtprune-1026d/env.sh; . /var/log/zts-ddtprune-1026d/envb.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
rm -rf /var/tmp/zts-ddtprune-1026d
ls -d /var/tmp/zts-ddtprune-1026d 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-ddtprune-1026d` and write
`/var/log/zts-ddtprune-1026d/REPORT.txt` with:

- part A, short: from `PART-A.txt`, the builds' rc, wall time, warnings and
  srcversions, and the `PART-A.done` time;
- B0, short: anything found, `uname -r`, the free space, the
  `/etc/zfs/zpool.cache` line, the `env.sh` hash line that matched, and the
  B1 baselines;
- `verify.txt`;
- every `load`, `unload`, `guard` and `no pool` line;
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:`, first failing line and notes when it did not pass, its kernel line
  and its leftovers line, the group's counts and per-test digests, each
  `dmesg <tree>` line and the whole-task kernel count;
- a summary table: each label's results against the expected ones, and which
  case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM 107, kernel ZTS of tigerblue77/zfs:ddt-prune-extended (PART-A-107.md, PART-B-107.md)

part A: done <date>; builds B rc=0 <s> warnings <n> srcversion <srcv>, M rc=0 <s> warnings <n> srcversion <srcv>
B0: free; tigerblue77; <uname -r>; /var/tmp <N>G free; zpool.cache <line>; part A complete;
  env.sh sha256 9615c7f2d75b... (or without blanks e4b0f74a687a...)
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines
verify: 18 ok lines (srcversions differ, M's test is the head's; B and M: head, head as recorded,
  changes as recorded, changed files, srcversion x2, <n> built files unchanged, results directory), trees verified

loads: "load B rc=0", "B guard ok: <srcv>; zfs_dedup_log_txg_max=8 zfs_dedup_log_flush_entries_min=200
  zfs_dedup_log_flush_entries_max=4294967295 metaslab_force_ganging=16777217 metaslab_force_ganging_pct=3
  zfs_bclone_enabled=1 spl_panic_halt=0", "B: no pool after the load"; "unload B rc=0", the same for M

runs:
b1 dedup_prune_leak PASS x1 rc=0 wall=<s>
  4 lines matching No leaks
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
bg -T dedup PASS x18 rc=0 wall=<s>
  every test passed
kernel bg: 0 suspicious lines
leftovers after bg: none
dmesg B: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
m1 dedup_prune_leak FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-ddtprune-1026d/M/tests/zfs-tests/tests/functional/dedup/dedup_prune_leak.ksh (run as root) [<mm:ss>] [FAIL]
    note: leaked space: vdev <v>, offset 0x<o>, size 131072
    note: block traversal size <n> != alloc <n + 131072> (leaked 131072)
    note: DDT pruning leaks the copy an extended entry gained
    first failing line: DDT pruning leaks the copy an extended entry gained
  2 lines matching No leaks
kernel m1: 0 suspicious lines
leftovers after m1: none
m2, m3: the same
mg not run: every test of bg passed
dmesg M: <n> suspicious lines in the buffer
kernel, whole of part B: 0 suspicious lines

summary (3 runs each)           M (bc3fe48662 + test)              B (983709b6a2)
  dedup_prune_leak              FAIL 3/3 (leaked 131072, at the    PASS 3/3 (4 No leaks each)
                                new case's first zdb)
  -T dedup (B, once)            -                                  PASS x18
  verdict: as expected; the kernel shows master's ddtprune leaking the copy an extended entry
           gained, and the fix keeping the entry with nothing leaked

cleanup: unload M rc=0, no zfs or spl module left, /var/tmp/zts-ddtprune-1026d removed, /var/tmp <N>G free
deviations: none
```

Estimated duration: about 40 minutes (checks 3, six runs of the test about 10,
the dedup group 15 to 25, cleanup 2).
