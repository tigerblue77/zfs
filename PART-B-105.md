# VM 105, part B: kernel runs for vdev-prop-set-validate-first

These instructions are for the Claude session on VM 105. They continue
`PART-A-105.md` (sha256
`c63b776c20b7017945913755ffcd91bc71d30472c0210f87b58ecb2a330c3199`; an earlier
copy, `a8a5dca3b3b4c8a0cacae5494b6c08b364304928facf0ca8e931c49314683262`,
differs only in how its last `git log` line abbreviates the two commit ids,
and either is fine here), which left two built trees of the fork branch
`tigerblue77/zfs:vdev-prop-set-validate-first` under
`/var/tmp/zts-vdevprop-1026d`, and its records under
`/var/log/zts-vdevprop-1026d`. Part B rebuilds nothing: it checks that the
trees are exactly the ones part A recorded, then loads each tree's modules
behind a srcversion guard, runs the new ZTS test
`functional/cli_root/zpool_set/vdev_set_validate` three times per tree, reads
the kernel log of every run from the journal, checks for leftovers, cleans up
and writes `/var/log/zts-vdevprop-1026d/REPORT.txt`, whose expected contents
end this document.

| Tree | Code | Runs | Expected |
| --- | --- | --- | --- |
| `B` | head `e42b092bc2` "Check every vdev property before applying any" (`module/zfs/vdev.c`, the test) | `b1`-`b3`; `bg` (`-T zpool_set` once); `ac` (`alloc_class_019_pos` once) | PASS 3/3; `bg` all PASS (one SKIP without fio); `ac` PASS |
| `M` | the head's parent `2aaff84e21` (#19388's two commits on master `0d4bb39c2c`) + the head's test files | `m1`-`m3` (and `mg`, `mac` only if `bg`, `ac` did not all pass) | FAIL 3/3 at the first `alloc_bias` check |

The states run in the order B, M.

What the test does. It creates `$TESTPOOL1` on two 256 MiB file vdevs,
`filedev0.<pid>` and `filedev1.<pid>` in `$TEST_BASE_DIR`, and reads the guid
of the first. Through the `vdev_set_props` helper, which sends several
properties in one `ZFS_IOC_VDEV_SET_PROPS` call (`zpool set` sends one), it
then:

1. sets `alloc_bias=2` (special) and `path=/invalid` in one call: the call
   must fail (`log_mustnot`; the path must start with `/dev/`), and
   `zpool get alloc_bias`, which reads the vdev's in-memory bias, must still
   say `none`;
2. sets `alloc_bias=special` with `zpool set`, then `alloc_bias=0` and
   `path=/invalid` in one call: that call must fail and the bias must still be
   `special`;
3. sets `checksum_n=7` and `io_n=9` in one call: both must be set.

**Why M fails, and why that cannot hurt the VM.** On M, `vdev_prop_set()`
checks and applies each property in turn: it stores the bias in memory, then
refuses the path with EINVAL, and the sync task that would have written the
bias never runs. So the helper exits 22 as expected, and `zpool get` then says
`special`: the test fails at its first check with `alloc_bias is 'special',
expected 'none'`, and its cleanup destroys the pool. Nothing reads the
in-memory bias on the way: the feature refcount code that #19388 adds runs
only when a vdev removal completes, which this test never does. Before this
was written, the same four calls were made through `vdev_prop_set()` itself
in userspace (libzpool, debug build, two file vdevs) on these exact two
commits: on M the first call returned 22 and left the bias at `special` (and
the third left it at `none`), on B the bias stayed `none` and then
`special`, and on both the fourth call set 7 and 9; both pools then exported
without an assertion.

Rules for the whole task (part A's still hold):

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`,
  not even `fio`), never run `scripts/zfs-helpers.sh`, never use
  `zfs-tests.sh -x`, never reboot. Rebuild nothing: a tree that does not
  verify is a stop.
- Work only under `/var/tmp/zts-vdevprop-1026d`, log to
  `/var/log/zts-vdevprop-1026d`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`): the results go to `/var/log/zts-vdevprop-1026d/results`.
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
  `/var/log/zts-vdevprop-1026d/env.sh` and then part B's `envb.sh`.
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
- `-t` adds the directory's `setup` and `cleanup` around the test (here
  `zpool_set`'s `default_setup` of `$TESTPOOL` on the first disk, and its
  `default_cleanup`): the result of a single run is read for the named test
  only.
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`,
  never `\(`; the patterns below contain none.
- `zfs-tests.sh -d` must point at a mode 1777 directory: part A made
  `$Z/files` so, and B0 checks it.

## B0: the VM must still be free, and part A complete

```sh
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-vdevprop-1026d; L=/var/log/zts-vdevprop-1026d
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
command -v fio || echo "no fio"
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
tmux pane, the path of `fio` or `no fio` (either is fine: without it
`vdev_set_scheduler` skips itself), `Z holds: B M files repo`,
`files: mode 1777, 0 entries`, no `missing or empty` line,
`results dir: 0 entries`,
`env.sh: sha256 9b8e486fa13bafd41e655b62e797761a1b0fd8961e422e427bc1b6c7cc171201`
(or, if only blanks differ, `env.sh without blanks:
23febb083596f9005078bdb857c6e9aa2b7fa7abb858c050d7c49d48f1f8a869`),
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
. /var/log/zts-vdevprop-1026d/env.sh
cat > $L/envb.sh <<'ENVB'
# Part B's helpers. env.sh (part A's) is sourced first.
# The tests run beside the new one, as zfs-tests.sh -t arguments.
RAC=tests/functional/alloc_class/alloc_class_019_pos.ksh
# The tunable the zpool_set group changes (zpool_set_ashift) and must restore.
TUN="vdev_file_physical_ashift"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# The lines of a failed test worth quoting: the refused calls (log_mustnot
# prints "exited <n>" when the command failed as expected), a failed
# log_must, and the test's own alloc_bias message.
NRE='exited [0-9]|alloc_bias is'
# One line per vdev_set_props call that did what the test wanted (3 when
# vdev_set_validate passes).
CRE='SUCCESS: vdev_set_props'

# Part A's record of <tree> against the tree as it is now: HEAD, changes,
# srcversion, the built modules, libraries and programs (the helper's
# included), the helper link and the results directory. Any MISMATCH means
# this is not the tree part A built.
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
	same "$1 helper link" "$(readlink -f tests/zfs-tests/bin/vdev_set_props)" \
	    "$Z/$1/tests/zfs-tests/cmd/vdev_set_props"
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
	p=$(others 'test-runner\.py|zfs-tests\.sh|zfs-tests/tests/functional|zdb |vdev_set_props')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|filedev[01]\.|device-[0-9])'; then
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

# Result counts of a zfs-tests.sh log, e.g. "PASS x16" or "PASS x15, SKIP x1".
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
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh
{
same "srcversions of B and M" "$(cmp -s $L/srcv-B $L/srcv-M && echo equal || echo differ)" differ
same "M's test files are the head's" \
    "$(for f in $F $WIRING; do cmp -s $Z/M/$f $Z/B/$f && echo same || echo "$f differs"; done | sort -u | xargs)" same
} | tee -a $L/verify.txt
verify_tree B $HB "tests/test-runner/bin/test-runner.py.in" &&
    verify_tree M $HM "tests/runfiles/common.run tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/Makefile.am $F" &&
    ! grep -q '^MISMATCH' $L/verify.txt && echo "trees verified" | tee -a $L/verify.txt
```

Expected: 20 `ok:` lines (2, then 9 for B, then 9 for M, the two `built files`
lines with the number of files part A hashed), then `trees verified`. Any
`MISMATCH` means stop: the tree is not the one part A built and recorded.

## B2: state B (head, the fix)

```sh
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh
switch_to B none
```

Expected: `load B rc=0`, `B guard ok: <srcversion>;
vdev_file_physical_ashift=9 spl_panic_halt=0` (9 is the module's default;
another value set by this VM's configuration is fine, since each run is
compared with what the load found), then `B: no pool after the load`.

```sh
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh; cd $Z/B
three b vdev_set_validate $R && zts_group bg zpool_set &&
    zts_run ac alloc_class_019_pos $RAC
dmesg_state B
```

| Label | Expected on B |
| --- | --- |
| `b1`-`b3` | **PASS x1**, rc=0, `3 lines matching SUCCESS: vdev_set_props` (the two refused calls, then the `checksum_n`/`io_n` call), no `not passed` line (`zpool_set`'s `setup` and `cleanup` pass), kernel 0, leftovers none, well under a minute each. |
| `bg` | `-T zpool_set`, once: **PASS x16** (setup, the 14 tests of the group, `vdev_set_path`, `vdev_set_allocating` and `vdev_set_validate`, which go through the changed code, among them, and cleanup), `every test passed`; or, without `fio`, **PASS x15, SKIP x1** with `vdev_set_scheduler` the SKIP (`fio missing`). Kernel 0, leftovers none. The fork's CI ran these on an earlier head with the same patch with no unexpected result; a failure here is reported test by test with its first failing line, and B3 then runs the same group on M. |
| `ac` | `alloc_class_019_pos` once: **PASS x1**, `1 lines matching SUCCESS: vdev_set_props` (its one refused `alloc_bias=0 path=/invalid` call), kernel 0, leftovers none. It is #19388's test of a failed `alloc_bias` and `path` call followed by a removal: with the fix the bias stays `special` in memory as well as on disk, and the test must still pass. |

## B3: state M (#19388's commits + the test)

```sh
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh
switch_to M B
```

Expected: `unload B rc=0`, `load M rc=0`, `M guard ok: <srcversion>;
vdev_file_physical_ashift=9 spl_panic_halt=0`, `M: no pool after the load`.

```sh
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh; cd $Z/M
if three m vdev_set_validate $R; then
	if [ -s $L/bg.notpassed ] && grep -qv '^vdev_set_scheduler$' $L/bg.notpassed; then
		zts_group mg zpool_set
	else
		echo "mg not run: bg had no failure" | tee -a $L/runs.txt
	fi &&
	if grep -q '^ac alloc_class_019_pos PASS x1 ' $L/runs.txt; then
		echo "mac not run: ac passed" | tee -a $L/runs.txt
	else
		zts_run mac alloc_class_019_pos $RAC
	fi
fi
dmesg_state M
```

`m1`-`m3`: **FAIL x1**, rc=1, each with

```
  [<time>] Test: /var/tmp/zts-vdevprop-1026d/M/tests/zfs-tests/tests/functional/cli_root/zpool_set/vdev_set_validate (run as root) [<mm:ss>] [FAIL]
    note: SUCCESS: vdev_set_props testpool1 <guid> alloc_bias=2 path=/invalid exited 22
    note: alloc_bias is 'special', expected 'none'
    first failing line: alloc_bias is 'special', expected 'none'
  1 lines matching SUCCESS: vdev_set_props
```

then kernel 0 and leftovers none (the test's cleanup destroys `testpool1` and
removes its two files), and `mg not run: bg had no failure`, `mac not run: ac
passed`. The one `SUCCESS: vdev_set_props` is the first call, refused with
EINVAL (22) as the test wants; the test stops at the check after it.

A failure before that check (a `log_must` on `truncate`, `zpool create` or
`zpool get guid`, or `vdev_set_props` not found) is a setup failure: report
it as such, it proves nothing about the fix. So is `vdev_set_props ...
unexpectedly exited 0` (the path was accepted).

## What the results mean

- **As expected** (B passes 3/3 with 3 helper calls each, the `zpool_set`
  group and `alloc_class_019_pos` pass; M fails 3/3 at `alloc_bias is
  'special', expected 'none'` after the first call was refused with 22; no
  kernel line, no leftover): the kernel confirms that without the fix a vdev
  property call refused on one property keeps the properties applied before
  it in memory, and that with the fix it applies none. This is the first
  kernel run of `vdev_set_validate` without the fix. The fix commit's patch-id
  (`99f9b667bf1c`) is unchanged when it is put onto #19388's rebased copy, so
  the result carries over to the branch once rebased.
- **M passes**: the first call did not change the bias on this VM; report it,
  the test is then not a regression test here.
- **B fails**: report the failing lines run by run; `alloc_bias is ...` on B
  means the fix did not hold in the kernel.
- **A `bg` or `ac` failure**: compare with `mg` or `mac`. A test failing on
  both is #19388's, master's or this VM's; failing on B only is the fix's.
- **Any kernel VERIFY, panic or hung task on either state**: a different
  problem; the line and the state it happened in are the result. Stop.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## B4: whole-task kernel check

```sh
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh
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
. /var/log/zts-vdevprop-1026d/env.sh; . /var/log/zts-vdevprop-1026d/envb.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
rm -rf /var/tmp/zts-vdevprop-1026d
ls -d /var/tmp/zts-vdevprop-1026d 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-vdevprop-1026d` and write
`/var/log/zts-vdevprop-1026d/REPORT.txt` with:

- part A, short: from `PART-A.txt`, the builds' rc, wall time, warnings and
  srcversions, the helper lines, and the `PART-A.done` time;
- B0, short: anything found, `uname -r`, the free space, the
  `/etc/zfs/zpool.cache` line, whether `fio` is present, the `env.sh` hash
  line that matched, and the B1 baselines;
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
VM 105, kernel ZTS of tigerblue77/zfs:vdev-prop-set-validate-first (PART-A-105.md, PART-B-105.md)

part A: done <date>; builds B rc=0 <s> warnings <n> srcversion <srcv>, M rc=0 <s> warnings <n> srcversion <srcv>;
  helper links into each tree
B0: free; tigerblue77; <uname -r>; /var/tmp <N>G free; zpool.cache <line>; fio <path, or none>; part A complete;
  env.sh sha256 9b8e486fa13b... (or without blanks 23febb083596...)
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines
verify: 20 ok lines (srcversions differ, M's test files are the head's; B and M: head, head as recorded,
  changes as recorded, changed files, srcversion x2, <n> built files unchanged, helper link, results directory),
  trees verified

loads: "load B rc=0", "B guard ok: <srcv>; vdev_file_physical_ashift=9 spl_panic_halt=0",
  "B: no pool after the load"; "unload B rc=0", the same for M

runs:
b1 vdev_set_validate PASS x1 rc=0 wall=<s>
  3 lines matching SUCCESS: vdev_set_props
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
bg -T zpool_set PASS x16 rc=0 wall=<s>          (without fio: PASS x15, SKIP x1, rc=<n>)
  every test passed                             (without fio: vdev_set_scheduler's Test: line, "fio missing")
kernel bg: 0 suspicious lines
leftovers after bg: none
ac alloc_class_019_pos PASS x1 rc=0 wall=<s>
  1 lines matching SUCCESS: vdev_set_props
kernel ac: 0 suspicious lines
leftovers after ac: none
dmesg B: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
m1 vdev_set_validate FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-vdevprop-1026d/M/tests/zfs-tests/tests/functional/cli_root/zpool_set/vdev_set_validate (run as root) [<mm:ss>] [FAIL]
    note: SUCCESS: vdev_set_props testpool1 <guid> alloc_bias=2 path=/invalid exited 22
    note: alloc_bias is 'special', expected 'none'
    first failing line: alloc_bias is 'special', expected 'none'
  1 lines matching SUCCESS: vdev_set_props
kernel m1: 0 suspicious lines
leftovers after m1: none
m2, m3: the same
mg not run: bg had no failure
mac not run: ac passed
dmesg M: <n> suspicious lines in the buffer
kernel, whole of part B: 0 suspicious lines

summary (3 runs each)           M (2aaff84e21 + test)               B (e42b092bc2)
  vdev_set_validate             FAIL 3/3 (alloc_bias 'special'      PASS 3/3
                                after the refused call)
  -T zpool_set (B, once)        -                                   PASS x16 (or x15 + vdev_set_scheduler SKIP)
  alloc_class_019_pos (B, once) -                                   PASS
  verdict: as expected; without the fix a refused vdev property call keeps the bias it applied first,
           with the fix it applies nothing

cleanup: unload M rc=0, no zfs or spl module left, /var/tmp/zts-vdevprop-1026d removed, /var/tmp <N>G free
deviations: none
```

Estimated duration: about 30 minutes (checks 3, six runs of the test about 5,
the `zpool_set` group about 10 to 15, `alloc_class_019_pos` 2, cleanup 2).
