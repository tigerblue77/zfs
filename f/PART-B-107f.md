# VM 107, part B: kernel runs for dedup-default-blake3

These instructions are for the Claude session on VM 107. They continue
`PART-A-107.md` (sha256
`626fcbbadc85e760729468bbc63be515f3aa2492702b7d1f7e4e4122843035c6`), which
left two built trees of the fork branch `tigerblue77/zfs:dedup-default-blake3`
under `/var/tmp/zts-dedupb3-1026f`, the compatibility files their libzfs reads
in `/var/tmp/zts-dedupb3-1026f/share`, and its records under
`/var/log/zts-dedupb3-1026f`. Part B rebuilds nothing: it checks that the
trees are exactly the ones part A recorded, then loads each tree's modules
behind a srcversion guard, runs the new ZTS test
`functional/dedup/dedup_default_checksum` three times and the `dedup` group
once per tree, reads the kernel log of every run from the journal, checks for
leftovers, cleans up and writes `/var/log/zts-dedupb3-1026f/REPORT.txt`, whose
expected contents end this document.

| Tree | Code | Runs | Expected |
| --- | --- | --- | --- |
| `B` | head `1db884e834` "Default to BLAKE3 for dedup=on on new pools" | `b1`-`b3` (`dedup_default_checksum`), then `bg` (`-T dedup` once) | PASS 3/3, 9 checks each; `bg` all PASS (20) |
| `M` | master `a128b8a075` (the head's parent and merge base) + every change of the head under `tests/` | `m1`-`m3`, then `mg` (`-T dedup` once) | FAIL 3/3 at the first check; `mg` all PASS but `dedup_default_checksum` (19 PASS, 1 FAIL) |

The states run in the order B, M. This is the head's first kernel run: the
fork's ZTS jobs on it were still queued on 26 Oct.

## What the test does

`dedup_default_checksum` makes `$TESTPOOL` on the three loop devices that
`zfs-tests.sh` provides, three times, and checks nine times, each with a new
512 KiB file of random data (`file1` to `file9`, `dd ... bs=128k count=4`),
that all four of the file's level-0 blocks are dedup blocks of the expected
checksum in `zdb -ddddddbbbbbb`:

| Check | Pool, dataset | Expected |
| --- | --- | --- |
| 1 | default features, `-O dedup=on`: the root dataset | blake3 |
| 2 | the same pool, `$TESTPOOL/verify` (`dedup=verify`) | blake3 |
| 3 | the same pool, `$TESTPOOL/sha256` (`dedup=sha256`) | sha256 |
| 4 | the root dataset again, after `zpool export` and `zpool import` | blake3 |
| 5 | a new pool created with `-o feature@blake3=disabled -O dedup=on` | sha256 |
| 6 | the same, after `zpool upgrade` (which enables `feature@blake3`) | sha256 |
| 7 | the same, after export and import | sha256 |
| 8 | the same pool, `$TESTPOOL/blake3` (`dedup=blake3`) | blake3 |
| 9 | a new pool created with `-o compatibility=openzfs-2.1-linux -O dedup=on` | sha256 |

Between them it requires `feature@blake3` to be `enabled` on the first pool
before any write and `active` after check 3; `enabled` on the second pool
after the upgrade and after check 7, then `active` after check 8; `disabled`
on the third. A failing check ends the test with
`<dataset>: <n> of 4 blocks are <checksum> dedup blocks` (`testpool/` for a
root dataset: the test adds the slash so that `zdb` reads the dataset, not the
MOS); a failing command ends it with `ERROR: <command> exited <n>`. On B each
check writes its file with a `SUCCESS: dd if=/dev/urandom of=...` line, 9 per
run. A run that fails at check `k` has `k` of them (the check writes its file,
then counts its blocks); one that fails at a command or a feature state
between checks has one per check passed.

## Why M fails, and why that cannot hurt the VM

On master, `ddt_create()` sets `spa_dedup_checksum` to SHA-256 on every pool
at creation and at every load, and nothing reads a recorded choice. So on M
the first pool's `feature@blake3` is `enabled` (master enables it at creation
too) and check 1 writes four SHA-256 dedup blocks: the count of blake3 dedup
blocks is 0, and the test fails with
`testpool/: 0 of 4 blocks are blake3 dedup blocks`, after one `dd`. It is a
`log_fail` of the check itself, so no command failed and there is no `ERROR:`
line. It is the test's first check that expects BLAKE3 on a new pool, and
`log_fail` ends the test there: M gets no further. On M the kernel creates
one pool with SHA-256 dedup, writes 512 KiB, lets `zdb` read it, and the
test's cleanup destroys the pool.

The same message would come from a `zdb` that printed nothing. B passing
shows that the test's `zdb` pipeline reads these block pointers with this
build, and the dedup group on M runs `zdb` in most of its other tests.

The nine dedup tests that the head moves from `dedup=on` to `dedup=sha256`
inspect the SHA-256 DDT (`DDT-sha256-*` in `zdb -D` or the MOS), and none
reads the `dedup` property back. On master `dedup=on` is SHA-256, so on M they
should pass exactly as with `dedup=on`; on B they keep the SHA-256 DDT they
inspect. Of the group's other tests, `dedup_bclone_pruned` and
`dedup_zap_shrink` already set `dedup=sha256`, `dedup_legacy_fdt_mixed` sets
`skein` and `blake3`, and `dedup_prune`, `dedup_prune_leak`,
`dedup_prune_percentage`, `dedup_quota` and `dedup_log_zdb_leak` use
`dedup=on` without naming a checksum: on B they write BLAKE3 dedup blocks.

Before this was written, part A's git steps, copy and record were run on a
scratch copy (fork clone and upstream fetch of 26 Oct, output identical to
part A's expected lines), with B built in userspace: its libzfs and `zpool`
carry `/var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d` (paths adjusted).
Part B's blocks were run there against stubs that write runner logs in
`test-runner.py`'s format. The branch's own libzpool harness reproducing the
test's checks passes on the head and fails on master (the branch's design
notes); no kernel has run either.

Rules for the whole task (part A's still hold):

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot. Rebuild nothing: a tree that does not verify is a stop.
- Work only under `/var/tmp/zts-dedupb3-1026f`, log to
  `/var/log/zts-dedupb3-1026f`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`): the results go to `/var/log/zts-dedupb3-1026f/results`.
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
  `/var/log/zts-dedupb3-1026f/env.sh` and then part B's `envb.sh`.
- After any line that says `stop`, run nothing more: go to B4 and B5 with what
  you have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

What earlier VM runs taught, and these steps already handle:

- The runner prefixes every `Test:` line with a timestamp, and writes
  `Test (Linux):` for a test that only `linux.run` lists (here the dedup
  group's `setup`, `dedup_legacy_gang` and `cleanup`): results are matched
  anywhere in the line, with or without that identifier, and a `(RERAN)`
  suffix is counted too.
- A test that writes nothing on stderr has no `merged` file: the failing line
  and the notes are read from the runner's own `log` file.
- That log sorts a test's stdout and stderr lines by the time the runner read
  them, so a failed `log_must`'s copy of the command's stderr and its
  `ERROR:` line can come in either order: the digest also prints the first
  `ERROR:` line, which cannot move.
- On a failed test, the ZTS callbacks print and then clear dmesg: kernel
  messages are read per run from `journalctl -k`, and a dmesg count is only a
  per-state cross-check.
- Each run waits 2 s first: two runs started in the same second would share a
  results directory.
- `log_must` ends a test at its first failure, so the first failing line is
  the whole story of a failed run.
- `-t` adds the directory's `setup` and `cleanup` around the test (the dedup
  directory's `setup` does nothing, its `cleanup` runs `default_cleanup`): the
  result of a single run is read for the named test only. In `common.run` the
  dedup group has neither (`pre =`, `post =`); `linux.run`'s part of it has
  both.
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`,
  never `\(`.
- `zfs-tests.sh -d` must point at a mode 1777 directory: part A made
  `$Z/files` so, and B0 checks it. No test of the dedup group starts a `zed`,
  so the trees keep the default run state directory; a `zed` after a run is
  still a leftover.

## B0: the VM must still be free, and part A complete

```sh
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-dedupb3-1026f; L=/var/log/zts-dedupb3-1026f
id -un; uname -r
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
pgrep -ax 'zed|lt-zed'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
ls -l /etc/zfs/zpool.cache 2>&1
ls -ld /usr/local/etc/zfs/compatibility.d /usr/local/share/zfs/compatibility.d 2>&1
sudo -n journalctl -k -n 1 --no-pager
echo "Z holds: $(LC_ALL=C ls -A $Z | xargs)"
echo "files: mode $(stat -c %a $Z/files), $(ls -A $Z/files | wc -l) entries;" \
    "share: $(ls $Z/share/zfs/compatibility.d | wc -l) compatibility files"
for f in env.sh partA.txt sha-checks.txt copy.txt srcv-B srcv-M tree-B.txt tree-M.txt \
    bins-B.sha256 bins-M.sha256 PART-A.done; do
	[ -s $L/$f ] || echo "missing or empty: $L/$f"
done
echo "results dir: $(ls -A $L/results | wc -l) entries"
echo "env.sh: sha256 $(sha256sum < $L/env.sh | cut -d' ' -f1)"
echo "env.sh without blanks: $(tr -d ' \t' < $L/env.sh | sha256sum | cut -d' ' -f1)"
echo "part A: $(grep -cx 'trees built and recorded' $L/partA.txt) done line," \
    "$(cat $L/sha-checks.txt $L/copy.txt | grep -c MISMATCH) mismatch, finished $(cat $L/PART-A.done)"
ls $L/envb.sh $L/partB-start.txt 2>&1
```

Expected: no module, no `/dev/zfs`, no other test or build process, no `zed`,
no busy tmux pane, the `zpool.cache` and `compatibility.d` lines as part A
found them (`No such file or directory` for both `compatibility.d`, unless
part A reported otherwise), `Z holds: B M files repo share`,
`files: mode 1777, 0 entries; share: 25 compatibility files`, no `missing or
empty` line, `results dir: 0 entries`,
`env.sh: sha256 2ca2b4e480e3c5998bcd0a1a432db8548b72059431cac1ff66e82b640d30e6e7`
(or, if only blanks differ, `env.sh without blanks:
66a27026c34270fff8af6751ba9ecb8844330d6280aca00856c6c463ca640723`),
`part A: 1 done line, 0 mismatch, finished <date>`, and `No such file or
directory` for `envb.sh` and `partB-start.txt`.

Stop at once, changing nothing, if: `id -un` is not `tigerblue77` or
`sudo -n true` fails; a zfs or spl module is loaded or `/dev/zfs` exists; a
test or build process or any `zed` runs, other than this command and the shell
running it; a tmux pane other than this session's runs anything; `/var/tmp`
has less than 8 GB free; `journalctl -k` prints no kernel line; or any other
expected line above differs (part A did not finish, or something changed its
output since: report it, the maintainer decides). `envb.sh` existing means
part B already started once: report, do not start again.

## B1: part B's helpers, baselines, and the check of part A's trees

```sh
. /var/log/zts-dedupb3-1026f/env.sh
cat > $L/envb.sh <<'ENVB'
# Part B's helpers. env.sh (part A's) is sourced first.
# The tunables the dedup tests change and must restore.
TUN="zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct zfs_bclone_enabled"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# A result line of the runner: "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a test that linux.run lists.
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'

# Per test: NRE, the lines of a failed run worth quoting, and CRE, the lines
# counted in every run of it.
pat() {   # pat <test name>
	case $1 in
	dedup_default_checksum)
		# check_dedup_cksum's message, a failed log_must ("ERROR: ...
		# exited <n>", after the command's stderr) and a refused zpool
		# or zfs command. One CRE line per check that wrote its file:
		# 9 on B; k when check k fails; the checks passed when a
		# command or a feature state between them fails.
		NRE='of 4 blocks are|exited [0-9]|cannot '
		CRE='SUCCESS: dd if=/dev/urandom' ;;
	*)
		NRE='^ERROR: |leaked space'
		CRE='' ;;
	esac
}

# Part A's record of <tree> against the tree as it is now: HEAD, changes,
# srcversion, the built modules, libraries and programs, where libzfs reads
# compatibility files, and the results directory. Any MISMATCH means this is
# not the tree part A built.
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
	same "$1 libzfs reads $CD" "$(grep -aqF "$CD" .libs/libzfs.so && echo yes || echo no)" yes
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
	if grep -qE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than' $L/kern-$1.txt; then
		echo "$1: panic or hung task in the kernel log: stop" | tee -a $L/runs.txt
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

# The named test's lines in the runner's log (a Test: line carries a
# timestamp and, for a linux.run test, "(Linux)"; a test that writes no
# stderr has no "merged" file). For each run of it that did not pass: its
# Test: line, the lines matching NRE before the failure callbacks, the line
# just before them, and the first ERROR line; for every run, the count of
# CRE lines.
log_digest() {   # log_digest <results dir> <test name>
	pat $2
	awk -v n="$2" -v nre="$NRE" -v cre="$CRE" '
	function flush() {
		if (bad && !done) {
			print "    last line: " prev
			print "    first ERROR line: " (err != "" ? err : "none")
		}
		if (sec && cre != "") print "  " cnt " lines matching " cre
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		bad = sec && $0 !~ /\[PASS\]/
		cnt = 0; notes = 0; done = 0; prev = ""; err = ""
		if (bad) print "  " $0
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	cre != "" && line ~ cre { cnt++ }
	bad && !done && line ~ /NOTE: Performing (test-fail callback|local cleanup)/ {
		print "    first failing line: " prev
		print "    first ERROR line: " (err != "" ? err : "none")
		done = 1
	}
	bad && !done && err == "" && line ~ /^ERROR: / { err = line }
	bad && !done && nre != "" && line ~ nre && notes < 12 { print "    note: " line; notes++ }
	{ prev = line }
	END { flush() }' "$1/log"
}

# Leftovers of a run, from the root of the tree whose module is loaded.
# Pools, devices, mounts and processes (a zed included) may hold the module
# (HELD); files and tunables the tests own are theirs to restore. All of
# them stop.
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
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed:"; echo "$p"; bad=1; touch $L/HELD; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|dedup_repair\.|dedup_log_zdb_leak\.|dedup_mount|vdevfile\.)'; then
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

# Result counts of the Test: lines of a zfs-tests.sh log (stdin), e.g.
# "PASS x20" or "FAIL x1, PASS x19".
tally() {
	grep -oE '(PASS|FAIL|SKIP|KILLED)\]( [(]RERAN[)])?$' | sed 's/\].*$//' |
	    sort | uniq -c | awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
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
	res=$(grep -E "$TRE" $L/$label.log | grep -E "/$name(\.ksh)? [(]run as " | tally)
	echo "$label $name ${res:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -E "$TRE" $L/$label.log | grep -vE "/$name(\.ksh)? [(]run as " | grep -v '\[PASS\]' |
	    sed 's/^/  not passed: /' | tee -a $L/runs.txt
	d=$(awk '/^Log directory/ {print $3}' $L/$label.log | head -1)
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

# One run of a tag group: the same guard, the counts, the results of the
# nine tests the head moves to dedup=sha256, and the digest of every test
# that did not pass.
zts_group() {   # zts_group <label> <tag>
	local label=$1 since t0 rc d n c c9 f
	pre_run $label || return 1
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -T $2 > $L/$label.log 2>&1
	rc=$?
	c=$(grep -E "$TRE" $L/$label.log | tally)
	echo "$label -T $2 ${c:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	c9=$(for f in $D9; do f=${f##*/}; f=${f%.ksh}
		grep -E "$TRE" $L/$label.log | grep -E "/$f(\.ksh)? [(]run as "; done | tally)
	echo "  the nine tests moved to dedup=sha256: ${c9:-NO RESULT}" | tee -a $L/runs.txt
	grep -E "$TRE" $L/$label.log | grep -v '\[PASS\]' |
	    sed -E 's|^.*Test( [(][^)]*[)])?: ([^ ]*/)?([^/ ]+) [(]run as .*$|\3|; s|\.ksh$||' |
	    sort -u > $L/$label.notpassed
	d=$(awk '/^Log directory/ {print $3}' $L/$label.log | head -1)
	[ -n "$c" ] && [ ! -s $L/$label.notpassed ] && echo "  every test passed" | tee -a $L/runs.txt
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
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh
{
same "srcversions of B and M" "$(cmp -s $L/srcv-B $L/srcv-M && echo equal || echo differ)" differ
same "M's test files are the head's" \
    "$(for f in $TB; do cmp -s $Z/M/$f $Z/B/$f || echo $f; done | xargs)" ""
same "$CD is both trees' compatibility.d" \
    "$( (diff -r $CD $Z/M/cmd/zpool/compatibility.d; diff -r $CD $Z/B/cmd/zpool/compatibility.d) | wc -l)" 0
} | tee -a $L/verify.txt
verify_tree B $HB "tests/test-runner/bin/test-runner.py.in" &&
    verify_tree M $HM "$(echo tests/test-runner/bin/test-runner.py.in $TB | xargs -n1 | LC_ALL=C sort | xargs)" &&
    ! grep -q '^MISMATCH' $L/verify.txt && echo "trees verified" | tee -a $L/verify.txt
```

Expected: 21 `ok:` lines (3, then 9 for B, then 9 for M, the two `built files`
lines with the number of files part A hashed), then `trees verified`. Any
`MISMATCH` means stop: the tree is not the one part A built and recorded.

## B2: state B (head)

```sh
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh
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
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh; cd $Z/B
three b dedup_default_checksum $R && zts_group bg dedup
dmesg_state B
```

| Label | Expected on B |
| --- | --- |
| `b1`-`b3` | **PASS x1**, rc=0, `9 lines matching SUCCESS: dd if=/dev/urandom` (the nine checks), no `not passed` line (the dedup directory's `setup` and `cleanup` pass), kernel 0, leftovers none, a minute or two each. |
| `bg` | `-T dedup`, once: **PASS x20** (`common.run`'s 17 tests, `dedup_default_checksum` among them, then `linux.run`'s `setup`, `dedup_legacy_gang` and `cleanup` as `Test (Linux):` lines), `the nine tests moved to dedup=sha256: PASS x9`, `every test passed`, kernel 0, leftovers none. The group's `dedup=on` tests (`dedup_prune`, `dedup_prune_leak`, `dedup_prune_percentage`, `dedup_quota`, `dedup_log_zdb_leak`) write BLAKE3 dedup blocks here. A failure is reported test by test with its first failing line, and `mg` shows whether master fails it too. |

## B3: state M (master + the head's tests)

```sh
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh
switch_to M B
```

Expected: `unload B rc=0`, `load M rc=0`, `M guard ok: <srcversion>; <the same
tunables> spl_panic_halt=0`, `M: no pool after the load`.

```sh
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh; cd $Z/M
three m dedup_default_checksum $R && zts_group mg dedup
dmesg_state M
```

`m1`-`m3`: **FAIL x1**, rc=1, each with

```
  [<time>] Test: /var/tmp/zts-dedupb3-1026f/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [FAIL]
    note: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first failing line: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first ERROR line: none
  1 lines matching SUCCESS: dd if=/dev/urandom
```

then kernel 0 and leftovers none (the test's cleanup destroys the pool). `mg`,
once: **FAIL x1, PASS x19**, rc=1,
`the nine tests moved to dedup=sha256: PASS x9`, and the same digest for
`dedup_default_checksum` alone, its `Test:` line without `.ksh` (the group
names its tests without it):

```
  [<time>] Test: /var/tmp/zts-dedupb3-1026f/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum (run as root) [<mm:ss>] [FAIL]
    note: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first failing line: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first ERROR line: none
  1 lines matching SUCCESS: dd if=/dev/urandom
```

then kernel 0, leftovers none. `first ERROR line: none` says that no command
failed: the check counted the blocks and found none of BLAKE3. `1 lines
matching` says that the run stopped at check 1.

A failure before check 1 (`log_must` on `zpool create`, `zfs create` or `dd`,
`feature@blake3` not `enabled` on the first pool, or a `setup` that does not
pass) is a setup or environment failure: report it as such, it proves nothing
about the head.

## What the results mean

- **As expected** (B passes 3/3 with 9 checks each and the dedup group passes,
  the nine included; M fails 3/3 at `testpool/: 0 of 4 blocks are blake3
  dedup blocks` with one check, and `mg` fails only that test, the nine
  passing; no kernel line, no leftover): on a kernel, a pool created with
  `feature@blake3` enabled writes BLAKE3 dedup blocks for `dedup=on` and
  `dedup=verify` and keeps doing so across an export and import, while a pool
  created without the feature (also after `zpool upgrade` and a re-import) or
  with `compatibility=openzfs-2.1-linux` keeps SHA-256; master writes SHA-256
  on the new pool, so the test detects the change; and the nine tests' move
  to `dedup=sha256` passes on both trees. This is the head's first kernel
  evidence.
- **M passes, or fails elsewhere** (a setup failure aside): master's
  `dedup=on` did not write SHA-256 dedup blocks on a new pool here, or the
  test stopped before check 1; report the lines, the test is then not a
  failing-first test here.
- **B fails**: with a `<n> of 4 blocks` note, the count `k` of `SUCCESS: dd`
  lines is the failing check (table above); with an `ERROR:` line, `k` checks
  passed before the failing command. Checks 1, 2 or 4 with `0 of 4 ... blake3`:
  the recorded choice was not applied (4 alone: not read back at import).
  Checks 5 to 7 with `0 of 4 ... sha256`: a pool created without the feature
  or upgraded later switched to BLAKE3, which the head promises never
  happens. Check 3 or 8: an explicit checksum changed. An `ERROR: test
  <state> = <expected> exited 1` names a `feature@blake3` state that differs
  (active too early, or not active after BLAKE3 blocks). `cannot create
  'testpool': could not read/parse feature file(s): openzfs-2.1-linux` with 8
  checks passed is this VM's libzfs not finding the compatibility file for
  check 9's pool, an environment failure, not the head's.
- **A `bg` failure**: compare with `mg`, test by test. A test failing on both
  is master's or this VM's; failing on B only is the head's (most likely a
  `dedup=on` test that depends on SHA-256 without saying so).
- **One of the nine fails on `mg`**: the move to `dedup=sha256` is not
  neutral on master (or the test fails on this VM anyway: compare with `bg`).
- **Any kernel VERIFY, panic or hung task on either state**: a different
  problem; the line and the state it happened in are the result. Stop.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## B4: whole-task kernel check

```sh
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh
sudo -n journalctl -k --since "$(cat $L/partB-start.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole of part B: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
echo "$CD after the runs: $(diff -r $CD $Z/B/cmd/zpool/compatibility.d | wc -l) lines differ from B's" |
    tee -a $L/runs.txt
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## B5: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a panicked thread or a leftover pool, device or process keeps the
module busy, and its files may be in use: report them, and the maintainer
decides, reboot included):

```sh
. /var/log/zts-dedupb3-1026f/env.sh; . /var/log/zts-dedupb3-1026f/envb.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
pgrep -ax 'zed|lt-zed'                 # must print nothing
ls -l /etc/zfs/zpool.cache 2>&1        # as B0 found it, or as the tests left it
rm -rf /var/tmp/zts-dedupb3-1026f
ls -d /var/tmp/zts-dedupb3-1026f 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-dedupb3-1026f` and write
`/var/log/zts-dedupb3-1026f/REPORT.txt` with:

- part A, short: from `PART-A.txt`, the builds' rc, wall time, warnings and
  srcversions, the libzfs compatibility line, and the `PART-A.done` time;
- B0, short: anything found, `uname -r`, the free space, the
  `/etc/zfs/zpool.cache` and `compatibility.d` lines, the `env.sh` hash line
  that matched, and the B1 baselines;
- `verify.txt`;
- every `load`, `unload`, `guard` and `no pool` line;
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:` line, notes, first failing line and first ERROR line when it did
  not pass, its kernel line and its leftovers line, the groups' counts, the
  nine tests' line and per-test digests, each `dmesg <tree>` line, the
  whole-task kernel count and the `$CD` line;
- a summary table: each label's results against the expected ones, and which
  case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM 107, kernel ZTS of tigerblue77/zfs:dedup-default-blake3 (PART-A-107.md, PART-B-107.md)

part A: done <date>; builds B rc=0 <s> warnings <n> srcversion <srcv>, M rc=0 <s> warnings <n> srcversion <srcv>;
  both libzfs read compatibility files from /var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d
B0: free; tigerblue77; <uname -r>; /var/tmp <N>G free; zpool.cache <line>; compatibility.d <lines>; part A complete;
  env.sh sha256 2ca2b4e480e3... (or without blanks 66a27026c342...)
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines
verify: 21 ok lines (srcversions differ, M's test files are the head's, $CD is both trees' compatibility.d;
  B and M: head, head as recorded, changes as recorded, changed files, srcversion x2, <n> built files unchanged,
  libzfs reads $CD, results directory), trees verified

loads: "load B rc=0", "B guard ok: <srcv>; zfs_dedup_log_txg_max=8 zfs_dedup_log_flush_entries_min=200
  zfs_dedup_log_flush_entries_max=4294967295 metaslab_force_ganging=16777217 metaslab_force_ganging_pct=3
  zfs_bclone_enabled=1 spl_panic_halt=0", "B: no pool after the load"; "unload B rc=0", the same for M

runs:
b1 dedup_default_checksum PASS x1 rc=0 wall=<s>s
  9 lines matching SUCCESS: dd if=/dev/urandom
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
bg -T dedup PASS x20 rc=0 wall=<s>s
  the nine tests moved to dedup=sha256: PASS x9
  every test passed
kernel bg: 0 suspicious lines
leftovers after bg: none
dmesg B: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
m1 dedup_default_checksum FAIL x1 rc=1 wall=<s>s
  [<time>] Test: /var/tmp/zts-dedupb3-1026f/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [FAIL]
    note: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first failing line: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first ERROR line: none
  1 lines matching SUCCESS: dd if=/dev/urandom
kernel m1: 0 suspicious lines
leftovers after m1: none
m2, m3: the same
mg -T dedup FAIL x1, PASS x19 rc=1 wall=<s>s
  the nine tests moved to dedup=sha256: PASS x9
  [<time>] Test: /var/tmp/zts-dedupb3-1026f/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum (run as root) [<mm:ss>] [FAIL]
    note: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first failing line: testpool/: 0 of 4 blocks are blake3 dedup blocks
    first ERROR line: none
  1 lines matching SUCCESS: dd if=/dev/urandom
kernel mg: 0 suspicious lines
leftovers after mg: none
dmesg M: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
kernel, whole of part B: 0 suspicious lines
/var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d after the runs: 0 lines differ from B's

summary (3 runs each)            M (a128b8a075 + tests)                 B (1db884e834)
  dedup_default_checksum         FAIL 3/3 (check 1: 0 of 4 blocks       PASS 3/3 (9 checks each)
                                 are blake3)
  -T dedup (once)                FAIL x1 (dedup_default_checksum),      PASS x20
                                 PASS x19, the nine PASS x9             (the nine PASS x9)
  verdict: as expected; on a kernel dedup=on writes BLAKE3 on a pool created with feature@blake3 and SHA-256
           otherwise, master writes SHA-256 on the new pool, and the nine tests' move to dedup=sha256 is neutral

cleanup: unload M rc=0, no zfs or spl module left, no zed, zpool.cache <line>, /var/tmp/zts-dedupb3-1026f removed,
  /var/tmp <N>G free
deviations: none
```

Estimated duration: about an hour (checks 3, six runs of the test about 8,
the two dedup group runs about 20 each, loads and cleanup 5).
