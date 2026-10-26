# VM: kernel ZTS for the fork branch zdb-il-setattr-mask

These instructions are for the Claude session on a disposable Debian 13 test
VM (kernel 6.12, 16 CPUs). They run, with the kernel module, the new ZTS test
`functional/cli_root/zdb/zdb_il_setattr` of the fork branch
`tigerblue77/zfs:zdb-il-setattr-mask`. The branch is one commit on upstream
master `a128b8a075`: zdb's decoding of the `TX_SETATTR` attribute mask with
the Linux kernel's `ATTR_*` bits on Linux, together with its test. To show
that the test catches the bug, state M is master with the test files of the
branch copied in (the test and its `common.run` and `Makefile.am` lines, no
code): the test must **fail on M** and **pass on the branch head** (state B),
three runs each. The session builds the two states, runs one at a time,
cleans up after itself and writes every result to
`/var/log/zts-zdbil1026/REPORT.txt`, whose expected contents end this
document.

| State | Tree | Code | Expected |
| --- | --- | --- | --- |
| M | `/var/tmp/zts-zdbil1026/M` | master `a128b8a075` + the branch's test file and its two registration lines (no code change) | `zdb_il_setattr` FAIL 3/3, on the test's own `grep` for `AT_SIZE`, the kernel untouched |
| B | `/var/tmp/zts-zdbil1026/B` | head `be86f063d0` "zdb: decode TX_SETATTR masks with the logging kernel's bits" (`cmd/zdb/zdb_il.c` and the three test files) | PASS 3/3 |

The states run in the order B, M. B's clean runs come first; M's are the
expected failure.

What this test does: it creates a pool with `sync=always` on a file vdev,
writes a synchronous file so that a ZIL header is on disk, creates a file and
freezes the pool. It then runs `truncate -s 123456`, `chmod 0640` and
`chown 1234:5678` on the file, each of which logs a `TX_SETATTR` record into
the frozen pool's intent log, unmounts, runs `zdb -iii` on the pool into a
file, and greps that output for `TX_SETATTR`, then for `AT_SIZE  123456`,
`AT_MODE  100640` (or `640`), `AT_UID   1234` and `AT_GID   5678`, each with
`log_must`.

**Why M fails in userspace only.** A Linux kernel logs the record's `lr_mask`
with its own `ATTR_*` values (`ATTR_SIZE` 0x8, `ATTR_GID` 0x4, ...), and
master's `zdb` decodes it with the Solaris `AT_*` values of libspl
(`AT_GID` 0x8, `AT_SIZE` 0x80). The truncate's mask, `0x2068`, therefore
prints as `AT_GID   0` and no `AT_SIZE` line, so on M the first of the four
greps fails and `log_must` ends the test there. Nothing in the kernel differs
between M and B: the module logs and replays the same records in both states,
and the pool is never replayed (it is destroyed by the test's cleanup). The
failure is the `grep` of a userspace tool's output; a kernel line on either
state is a different problem, so report it.

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot.
- Work only under `/var/tmp/zts-zdbil1026`, log to `/var/log/zts-zdbil1026`.
  Never read, write or delete `/var/tmp/test_results` or anything else this
  task did not create: the results go to `/var/log/zts-zdbil1026/results`.
  The only `rm -rf` allowed is the one written in STEP 6, with its literal
  path.
- `/etc/zfs/zpool.cache` may exist on this VM as a 0-byte file left by
  earlier tasks. Leave it alone: never delete, edit or rewrite it, and do not
  stop on it.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`. Never `-j16`. The test is not
  timing-sensitive; it runs under the same `nice -n 10 ionice -c3`, one at a
  time.
- All git work happens in STEP 1.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-zdbil1026/env.sh`, so that it also works in a shell that
  keeps no state between commands.
- After any line that says `stop`, run nothing more: go to STEP 6 with what you
  have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

What earlier VM runs taught, and these steps already handle: the runner
prefixes every `Test:` line with a timestamp; a test that writes nothing on
stderr has no `merged` output file (the failing line is read from the runner's
own `log`); a failed test makes ZTS print and clear dmesg, so the kernel
messages of each run are read from `journalctl -k`; two runs started in the
same second would share a results directory, so each run waits 2 s first.

## STEP 0: the VM must be free

Run all of these and keep their output:

```sh
export PATH=/usr/sbin:/sbin:$PATH
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
ls -A /var/tmp; ls -d /var/tmp/zts-zdbil1026 /var/log/zts-zdbil1026 2>&1
ls -l /etc/zfs/zpool.cache 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n losetup -a; sudo -n dmsetup ls 2>&1
sudo -n journalctl -k -n 1 --no-pager
command -v ksh python3 git truncate chmod chown
```

Stop at once, changing nothing, if:

- `id -un` is not `tigerblue77`, or `sudo -n true` fails;
- the system is not Debian 13, or the kernel is not a 6.12 one, or its headers
  (`/lib/modules/$(uname -r)/build`) are missing: do not install them;
- a zfs or spl module is loaded, or `/dev/zfs` exists;
- a test or build process runs, other than this command and the shell running
  it;
- a tmux pane other than the one hosting this session runs anything but an idle
  shell;
- `/var/tmp/zts-zdbil1026` or `/var/log/zts-zdbil1026` already exists;
- `/var/tmp` has less than 10 GB free (two built trees and the test's
  256 MiB file vdev);
- `ksh`, `python3`, `git`, `truncate`, `chmod` or `chown` is missing;
- `journalctl -k` prints no kernel line: without the journal, the kernel
  messages of a failed test are lost.

Report what was found; that is the whole task then. The loop and dm devices
listed here are not ours: STEP 1 keeps them as the baseline of the leftover
checks. Record `uname -r` and the `/etc/zfs/zpool.cache` line: another 6.12
kernel is not a reason to stop, and neither is that file, whatever its size.

## STEP 1: environment and sources

```sh
sudo -n mkdir -p /var/log/zts-zdbil1026/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-zdbil1026
cat > /var/log/zts-zdbil1026/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-zdbil1026
L=/var/log/zts-zdbil1026
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# Upstream master (the branch's base) and the branch's head, its one
# commit: the fix and its test. State M is master with the test files
# of the head copied in.
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
HB=be86f063d0719003ec23e4d79a3cde4704fa3a62
# The test as a file of the tree (F) and as a zfs-tests.sh -t argument (R),
# and the two files that register it.
F=tests/zfs-tests/tests/functional/cli_root/zdb/zdb_il_setattr.ksh
R=tests/functional/cli_root/zdb/zdb_il_setattr.ksh
WIRING="tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# The test's own failure messages worth quoting (log_must prints ERROR:
# and the command; the greps name the attribute).
NRE='ERROR: |AT_SIZE|AT_MODE|AT_UID|AT_GID'
# One line per grep of the zdb output that passed: 5 on a pass.
CRE='SUCCESS: grep -q'

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion and its zdb's sha256,
# and send its ZTS results to $L/results. The zdb must differ from every tree
# built before. The srcversion may match another tree's only if module/ and
# include/ are those of B's commit: the states then differ in zdb alone.
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
	{ cat .libs/zdb 2>/dev/null || cat zdb; } | sha256sum | cut -d' ' -f1 > $L/zdbsum-$1
	for f in $L/srcv-*; do
		[ "$f" = "$L/srcv-$1" ] && continue
		cmp -s "$f" $L/srcv-$1 || continue
		if ! git diff --quiet be86f063d0719003ec23e4d79a3cde4704fa3a62 -- module include; then
			echo "$1 has the srcversion of ${f##*/srcv-} but other module sources: stop" | tee -a $L/runs.txt
			return 1
		fi
		echo "$1 has the srcversion of ${f##*/srcv-}; module/ and include/ identical" | tee -a $L/runs.txt
	done
	for f in $L/zdbsum-*; do
		[ "$f" = "$L/zdbsum-$1" ] && continue
		cmp -s "$f" $L/zdbsum-$1 || continue
		echo "$1 has the zdb of ${f##*/zdbsum-}: stop" | tee -a $L/runs.txt
		return 1
	done
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    tests/test-runner/bin/test-runner.py.in \
	    tests/test-runner/bin/test-runner.py
	if ! grep -qx "BASEDIR = '$L/results'" tests/test-runner/bin/test-runner.py; then
		echo "$1: BASEDIR not redirected: stop" | tee -a $L/runs.txt
		return 1
	fi
	echo "$1 built: srcversion $(cat $L/srcv-$1), zdb $(cut -c1-12 $L/zdbsum-$1)" | tee -a $L/runs.txt
}

# Load <tree>'s modules. Whatever is loaded must be <previous>'s, and is
# unloaded first; <previous> is "none" for the first load. spl_panic_halt
# is recorded for the report; neither state is expected to reach a VERIFY.
switch_to() {   # switch_to <tree> <previous>
	local cur
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
	echo "$1 guard ok: $(cat $L/loaded-srcv);" \
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
# A literal parenthesis in a regex here is written [(], never \(.
log_digest() {   # log_digest <results dir> <test name>
	awk -v n="$2" -v nre="$NRE" -v cre="$CRE" '
	function flush() {
		if (sec && cre != "") print "  " cnt " lines matching " cre
		if (bad && !done) print "    last line: " prev
	}
	/Test: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
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
# Pools, devices, mounts and processes may hold the module (HELD); the
# test's own files are its to remove. All of them stop.
leftovers() {   # leftovers <label>
	local bad=0 p f
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
	if echo "$f" | grep -qE '^zdb_il_setattr'; then
		echo "  LEFTOVER files in $Z/files:" $f; bad=1
	elif [ -n "$f" ]; then
		echo "  other files in $Z/files (not fatal):" $f
	fi
	echo "leftovers after $1: $([ $bad = 0 ] && echo none || echo 'FOUND: stop')"
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	return $bad
}

# One run: srcversion guard, zfs-tests.sh as this user under an outer
# timeout, then the result, the digest of the runner's log, the kernel
# messages and the leftovers. Returns 1 when nothing more may run.
zts_run() {   # zts_run <label>
	local label=$1 name=zdb_il_setattr since t0 rc res d
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv 2>&1)" ]; then
		echo "$label: loaded srcversion is not $(cat $L/loaded-srcv 2>&1): stop" | tee -a $L/runs.txt
		return 1
	fi
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -t $R > $L/$label.log 2>&1
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
	kern_check $label "$since" || return 1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		echo "$label: outer timeout: stop" | tee -a $L/runs.txt
		return 1
	fi
	leftovers $label
}

three() {   # three <label prefix>: runs <prefix>1, 2 and 3
	zts_run ${1}1 && zts_run ${1}2 && zts_run ${1}3
}
ENV
. /var/log/zts-zdbil1026/env.sh
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
worktree per state. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-zdbil1026/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "zdb-il-setattr-mask" "$(git rev-parse origin/zdb-il-setattr-mask)" $HB
same "its base, one commit below" "$(git rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HB)" $HM
same "test files of the commit" "$(git diff --name-only $HM $HB -- tests | xargs)" \
    "tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am $F"
same "code files of the commit" "$(git diff --name-only $HM $HB -- . ':!tests' | xargs)" \
    "cmd/zdb/zdb_il.c"
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done
git diff --stat $HM $HB | tail -1
```

Copy the test into M: the test file from the head, and the head's own diff of
the two registration files, applied onto master:

```sh
. /var/log/zts-zdbil1026/env.sh
cd $Z/M && git checkout $HB -- $F && git diff $HM $HB -- $WIRING | git apply &&
    echo "M: test copied"
git diff HEAD --stat | tail -1
echo "M: $(git diff $HB -- tests | wc -l) $(git diff HEAD -- module include lib cmd | wc -l)"
for t in M B; do
	echo "$t: test $(ls -l $Z/$t/$F | cut -c1-10)," \
	    "AT_SIZE test line $(grep -n 'lr->lr_mask & AT_SIZE)' $Z/$t/cmd/zdb/zdb_il.c | cut -d: -f1)," \
	    "LR_AT_SIZE defines $(grep -c '^#define	LR_AT_SIZE' $Z/$t/cmd/zdb/zdb_il.c)"
done
```

Expected, in order:

```
clone rc=0
fetch rc=0
ok: zdb-il-setattr-mask
ok: its base, one commit below
ok: merge base with upstream master
ok: test files of the commit
ok: code files of the commit
worktree M rc=0 at a128b8a075
worktree B rc=0 at be86f063d0
 4 files changed, 96 insertions(+), 7 deletions(-)
M: test copied
 3 files changed, 67 insertions(+), 1 deletion(-)
M: 0 0
M: test -rwxr-xr-x, AT_SIZE test line 278, LR_AT_SIZE defines 0
B: test -rwxr-xr-x, AT_SIZE test line , LR_AT_SIZE defines 2
```

(`M: 0 0`: no line under `tests/` differs from the head, and no code changed.
M's `zdb` tests the mask against libspl's `AT_SIZE` at `zdb_il.c:278`, the
decoding the test fails on, and defines no `LR_AT_SIZE`; B tests `LR_AT_SIZE`
instead and defines it twice, once for Linux and once for the other
platforms.) Any `MISMATCH` (the branch was force-pushed since this was
written) or other difference means stop.

## STEP 2: builds

```sh
. /var/log/zts-zdbil1026/env.sh
build_tree B && build_tree M && echo "trees built"
```

Expected: `build B rc=0`, `B built: srcversion ..., zdb ...`, `build M rc=0`,
probably `M has the srcversion of B; module/ and include/ identical`, then
`M built: srcversion ..., zdb ...` (two different zdb sums), then
`trees built`. On a failed
build, the `error:` lines go into the report. Both trees built in a debug
userspace-only build where this was written; the kernel module was not built
there (no kernel headers), so a module warning is news worth reporting.

The commit changes no module source (only `cmd/zdb/zdb_il.c`), so the two
modules are built from the same sources and modpost most likely gives them the
same srcversion (it hashes only the headers in an object's own directory, and
`include/zfs_gitrev.h`, the one generated file that differs, is not). That is
expected here: the states differ in `zdb`, which the zdb sums check. The
gitrev lines still tell the trees apart. B's says `...-gbe86f063d0` and M's
`...-ga128b8a075-dirty`, since M carries the copied test. Record both:

```sh
. /var/log/zts-zdbil1026/env.sh
for t in B M; do echo "$t gitrev: $(cut -d'"' -f2 $Z/$t/include/zfs_gitrev.h)"; done
```

If `build_tree M` stops on `other module sources` or on `M has the zdb of
B`, report it with both srcversions, both zdb sums and both gitrev lines, and
do not edit a tree to tell them apart.

## STEP 3: state B (head, the fix)

```sh
. /var/log/zts-zdbil1026/env.sh
switch_to B none
```

Expected: `load B rc=0` and `B guard ok: <srcversion>; spl_panic_halt=<0|1>`.

```sh
. /var/log/zts-zdbil1026/env.sh; cd $Z/B
three b
dmesg_state B
```

b1, b2, b3: **PASS x1**, rc=0, `5 lines matching SUCCESS: grep -q` (the grep
for `TX_SETATTR` and the four attribute greps), in a minute or two at most,
kernel 0, leftovers none. A PASS prints no notes; the per-run log `$L/b1.log`
and the runner log hold them if wanted.

## STEP 4: state M (master + the test)

```sh
. /var/log/zts-zdbil1026/env.sh
switch_to M B
```

```sh
. /var/log/zts-zdbil1026/env.sh; cd $Z/M
three m
dmesg_state M
```

m1, m2, m3: **FAIL x1**, rc=1, with the first failing line

`ERROR: grep -qE ^[[:space:]]*AT_SIZE[[:space:]]+123456$ /var/tmp/zts-zdbil1026/files/zdb_il_setattr.out exited 1`

(the same line also appears as a `note:`), and `1 lines matching SUCCESS:
grep -q` (only the grep for `TX_SETATTR` passed: the records are in the log,
and zdb prints them). Kernel 0, leftovers none. `log_must` ends the test at
the first grep that fails, so the `AT_MODE`, `AT_UID` and `AT_GID` greps are
not reached on M. If they were, master's `zdb` would print `AT_MODE` (with
whatever `lr_mode` holds) and `AT_UID   1234` for the chown, and nothing for
the chmod; this was checked in userspace with zdb's own printer on
hand-built records. The test's `log_onexit` cleanup destroys the
pool and removes its vdev and output file even on this failure.

A different first failing line on M is still a failure, but not the one the
test was written for; report it as it is. The first failing lines that would
mean the *setup* broke, not zdb's decoding, are a `log_must` on `truncate`,
`zpool create`, `dd`, `touch`, `zpool freeze`, `chmod`, `chown`,
`zfs unmount` or `zdb -iii`, and `grep -q TX_SETATTR` (no record in the
log). There the test proves nothing about the bug, so report it as a setup
failure.

## What the results mean

- **As expected** (B passes 3/3 with 5 passing greps per run, M fails 3/3 on
  the `AT_SIZE` grep with 1 passing grep, no kernel line, no leftover): with
  a kernel-written intent log, master's `zdb -iii` does not print the size a
  truncate sets, and the branch's does, along with the mode, uid and gid of a
  chmod and a chown.
- **M passes 3/3**: master's `zdb` printed `AT_SIZE  123456` here, so the
  kernel logged a mask with 0x80 set, which contradicts the mapping in
  include/os/linux/spl/sys/vnode.h. The test is then not a regression test
  here, and the pull request must not claim it fails without the fix; keep
  `$L/m1.log` and report it. **M fails 1 or 2 of 3**: report the counts and
  the first failing line of each run.
- **B fails**: report the failing line run by run. A failed `AT_SIZE`,
  `AT_MODE`, `AT_UID` or `AT_GID` grep means the fix does not decode that
  attribute as this kernel logs it; a failed `grep -q TX_SETATTR` or an
  earlier `log_must` is a setup failure (see STEP 4).
- **Any kernel VERIFY, panic or hung task on either state**: a different
  problem; the line and the state it happened in are the result. Stop.

## STEP 5: whole-task kernel check

```sh
. /var/log/zts-zdbil1026/env.sh
sudo -n journalctl -k --since "$(cat $L/start-time.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## STEP 6: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a leftover pool, device or process keeps the module busy, and its
files may be in use: report them, and the maintainer decides):

```sh
. /var/log/zts-zdbil1026/env.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
rm -rf /var/tmp/zts-zdbil1026
ls -d /var/tmp/zts-zdbil1026 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-zdbil1026` and write `/var/log/zts-zdbil1026/REPORT.txt`
with:

- STEP 0, short: anything found, `id -un`, `uname -r`, the distribution,
  `nproc`, the free space, the `/etc/zfs/zpool.cache` line, and the STEP 1
  baselines;
- `sha-checks.txt`, the two worktree lines, the M copy lines and the STEP 1
  test-file checks;
- per tree: build rc and wall time, srcversion, zdb sum, gitrev line, and every
  `load`, `unload` and `guard` line (with `spl_panic_halt`);
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:` and first failing line and notes when it did not pass, its kernel
  line and its leftovers line, plus each `dmesg <tree>` line and the whole-task
  kernel count;
- a summary table: the results of each state against the expected ones, and
  which case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM, kernel ZTS of tigerblue77/zfs:zdb-il-setattr-mask (VM-TESTS-zdbil.md)

STEP 0: free; tigerblue77; <uname -r, a 6.12 kernel>; Debian GNU/Linux 13 (trixie); nproc 16;
  /var/tmp <N>G free; headers, journal, ksh, python3, git, truncate, chmod, chown present; passwordless sudo
  /etc/zfs/zpool.cache <ls line, or absent>
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines

sources: clone rc=0, fetch rc=0, 5 ok lines (head be86f063d0, base and merge base a128b8a075,
  test files = 3, code files = 1)
worktrees: M a128b8a075, B be86f063d0; commit 4 files +96 -7
M copy: "M: test copied", 3 files +67 -1, "M: 0 0"
AT_SIZE test line M 278 B none; LR_AT_SIZE defines M 0 B 2; test executable in both

builds (rc, wall, srcversion, zdb; zdb distinct): B 0 <s> <srcv> <zdb>  M 0 <s> <srcv> <zdb>
  <"M has the srcversion of B; module/ and include/ identical", or nothing>
gitrev: B zfs-<version>-gbe86f063d0, M zfs-<version>-ga128b8a075-dirty
loads: "load B rc=0", "B guard ok: <srcv>; spl_panic_halt=<n>",
  the same for M, "unload B rc=0"

runs:
b1 zdb_il_setattr PASS x1 rc=0 wall=<s>
  5 lines matching SUCCESS: grep -q
kernel b1: 0 suspicious lines
leftovers after b1: none
b2, b3: the same
dmesg B: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
m1 zdb_il_setattr FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-zdbil1026/M/tests/zfs-tests/tests/functional/cli_root/zdb/zdb_il_setattr.ksh (run as root) [<mm:ss>] [FAIL]
    note: ERROR: grep -qE ^[[:space:]]*AT_SIZE[[:space:]]+123456$ /var/tmp/zts-zdbil1026/files/zdb_il_setattr.out exited 1
    first failing line: ERROR: grep -qE ^[[:space:]]*AT_SIZE[[:space:]]+123456$ /var/tmp/zts-zdbil1026/files/zdb_il_setattr.out exited 1
  1 lines matching SUCCESS: grep -q
kernel m1: 0 suspicious lines
leftovers after m1: none
m2, m3: the same
dmesg M: <n> suspicious lines in the buffer
kernel, whole task: 0 suspicious lines

summary               zdb_il_setattr
  B  (be86f063d0)     PASS 3/3
  M  (a128b8a075 + test) FAIL 3/3 (userspace: zdb -iii prints no AT_SIZE for the truncate)
  verdict: as expected; with a kernel-written log, master's zdb misdecodes the TX_SETATTR
           mask and the branch's prints the size, mode, uid and gid

cleanup: unload M rc=0, no zfs or spl module left, /var/tmp/zts-zdbil1026 removed, /var/tmp <N>G free
deviations: none
```

The exact prefix of the `note:` lines (`ERROR:` alone, or with a timestamp in
brackets) follows the ZTS log format and may differ; the content is what
matters. `log_fail` without a message prints nothing of its own, so the first
failing line is the `ERROR:` line `log_must` printed for the grep.
