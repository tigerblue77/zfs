# VM 105, part B: kernel runs for pool-feature-disable-v2 and vdev-add-event-details

These instructions are for the Claude session on VM 105. They continue
`PART-A-105.md` (sha256
`f66d14237154f5ce04932789947fafad6a67aafff487830ed5d9364a447aa4e4`), which
left three built trees under `/var/tmp/zts-featev-1026e` and its records under
`/var/log/zts-featev-1026e`. Part B rebuilds nothing: it checks that the trees
are exactly the ones part A recorded, then loads each tree's modules behind a
srcversion guard, runs each branch's new ZTS test three times on its head and
three times on the master tree, reads the kernel log of every run from the
journal, checks for leftovers, cleans up and writes
`/var/log/zts-featev-1026e/REPORT.txt`, whose expected contents end this
document.

| Tree | Code | Runs | Expected |
| --- | --- | --- | --- |
| `F` | head `c62b3702d6` of `pool-feature-disable-v2` "Allow an inactive pool feature to be disabled again" | `f1`-`f3` (`zpool_set_feature_disable`); `fg` (`-T zpool_set` once); `fu` (`-T zpool_upgrade` once) | PASS 3/3; `fg` and `fu` all PASS |
| `V` | head `b87a733ac0` of `vdev-add-event-details` (openzfs/zfs#19566) "Describe the added vdevs in the vdev_add event" | `v1`-`v3` (`vdev_add_event`); `vg` (`-T zpool_add` once); `ve` (`events_001_pos` once) | PASS 3/3; `vg` and `ve` all PASS |
| `M` | `e117a84dbb`, both heads' parent, + both heads' changes under `tests/` | `mf1`-`mf3` (`zpool_set_feature_disable`), `mv1`-`mv3` (`vdev_add_event`); `mfg`, `mfu`, `mvg`, `mve` only if `fg`, `fu`, `vg`, `ve` did not all pass | `mf`: FAIL 3/3 at the first `zpool set feature@...=disabled`; `mv`: FAIL 3/3 at the first check |

The states run in the order F, V, M.

## What the two tests do

`zpool_set_feature_disable` (F's) makes `testpool1` on file vdevs
`feature_disable.0` to `.4` (256 MiB each, in `$Z/files`) and checks, in six
steps, that `zpool set feature@<name>=disabled` moves an unused enabled
feature back to disabled and refuses an active one or one that another
enabled feature depends on: (1) `multi_vdev_crash_dump`, across an export and
import and in the history; (2) `zstd_compress`, after which `zfs set
compression=zstd` must fail until it is enabled again; (3) `sha512`, disabled
while a dataset has `checksum=sha512` but no block: the first write must
enable and activate it again (`spa_feature_incr()`'s new re-enable), and
`zpool history -i` must record that enable; (4) `draid`, refused while
`draid_failure_domains` is enabled, then both disabled, a draid vdev refused,
`draid_failure_domains` enabled again (which enables `draid`), a draid vdev
added and the now active `draid` refused; (5) `raidz_expansion`, with an
attach refused, then allowed; (6) `lz4_compress`, active as soon as it is
enabled, refused. On F, 16 `zpool set feature@` commands do what the test
wants, each printing a `SUCCESS: zpool set feature@...` line. Steps 4 to 6 have
never run anywhere: the earlier CI runs stopped at step 3's broken check,
which the squashed head fixes.

`vdev_add_event` (V's) makes `mpool.<pid>` on `vdev1` in `$Z/files`, starts a
ZED, clears the events and adds `vdev2` and `vdev3` in one `zpool add`. It
then requires `zpool events -v` to hold one `vdev_add` event per added vdev,
with that vdev's `vdev_guid` and `vdev_path` (compared by `diff` with what
`zpool get guid` says), the ZED's debug log to have received the same, a
mirror added later to give one event with its guid and no path, and a spare
added alone still to give one event. On V, its three `zpool add` commands
each print a `SUCCESS: zpool add` line.

## Why M fails, and why that cannot hurt the VM

**`zpool_set_feature_disable` on M.** libzfs (`zpool_valid_proplist()`)
refuses `feature@<name>=disabled` on an existing pool before any ioctl: the
first disable, `zpool set feature@multi_vdev_crash_dump=disabled testpool1`,
prints `cannot set property for 'testpool1': property
'feature@multi_vdev_crash_dump' can only be set to 'disabled' at creation
time` and exits 1, so `log_must` ends the test at step 1. That refusal is what
openzfs/zfs#19439 reports. It is also as far as failing-first goes for this
branch: the branch adds a new value of the feature properties and a new value
in the `ZFS_IOC_POOL_SET_PROPS` request (`ZPOOL_FEATURE_DISABLE`), the test
needs both from its first disable on, and no M state without the branch's
code reaches steps 2 to 6. So M shows that the test detects the missing
capability; it says nothing about the kernel code, which the request never
reaches. The kernel evidence is F alone, and F is also the first kernel run of
this head anywhere. (A hybrid of F's libzfs over master's module would fail at
the same line with the kernel's EINVAL: that checks the commit message's
"older kernels refuse it" claim, not the bug, and is not run here.) On M the
kernel only creates, reads and destroys a pool.

**`vdev_add_event` on M.** `spa_vdev_add()` posts one `vdev_add` event with
no vdev (`spa_event_notify(spa, NULL, NULL, ESC_ZFS_VDEV_ADD)`), and
`zfs_event_create()` adds `vdev_guid` and `vdev_path` only for a vdev. So the
two vdevs added in one `zpool add` give a single event, which the test reads as
`- -`; the test's `diff` against the two expected `<guid> <path>` lines fails,
and `log_must` ends the test there: its first check, as the pull request's
description says. The test needs nothing new, so this is the bug itself. On M
the kernel creates a pool, adds two file vdevs, posts master's usual events
and destroys the pool; the ZED reads events, as in every events test.

Before this was written, both heads' parent and the heads were checked out and
M was assembled exactly as part A does, with the same output; V's tree was
built in userspace (`--with-config=user --enable-debug`, no warning), and its
ZED carried the `--runstatedir` pid file path. The fork's CI passed
`vdev_add_event` and the `zpool_add` group on V's exact head (debian13 zfs-qemu
job 121314011737); F's zfs-qemu run had not started.

Rules for the whole task (part A's still hold):

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`, never
  reboot. Rebuild nothing: a tree that does not verify is a stop.
- Work only under `/var/tmp/zts-featev-1026e`, log to
  `/var/log/zts-featev-1026e`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`): the results go to `/var/log/zts-featev-1026e/results`.
  The only `rm -rf` allowed is the one written in B6, with its literal path.
- `/etc/zfs/zpool.cache` may exist as a 0-byte file left by earlier tasks.
  Leave it alone: never delete, edit or rewrite it.
- The events tests' setup (and `add_nested_replacing_spare` in the
  `zpool_add` group) links `/etc/zfs/vdev_id.conf` to a file in `$Z/files`
  for the length of a run, and their cleanup removes it: that link is the one
  thing outside this task's directories that these runs create. It must be
  gone after every run; never create or remove it by hand.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background loop or watcher behind, and no `zed`.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the tests run under
  `nice -n 10 ionice -c3`, one at a time.
- Every block from B1 on starts by sourcing part A's
  `/var/log/zts-featev-1026e/env.sh` and then part B's `envb.sh`.
- After any line that says `stop`, run nothing more: go to B5 and B6 with what
  you have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

What earlier VM runs taught, and these steps already handle:

- The runner prefixes every `Test:` line with a timestamp, and writes
  `Test (Linux):` for a test that only `linux.run` lists (`vdev_add_event`'s
  events group, `add_nested_replacing_spare` in `-T zpool_add`): results are
  matched anywhere in the line, with or without that identifier.
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
- `-t` adds the directory's `setup` and `cleanup` around the test (here
  `zpool_set`'s `default_setup` of `$TESTPOOL`, and the events group's ZED
  setup and `default_setup`): the result of a single run is read for the
  named test only.
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`,
  never `\(`; the patterns below contain none.
- `zfs-tests.sh -d` must point at a mode 1777 directory: part A made
  `$Z/files` so, and B0 checks it. The ZED the tests start keeps its pid file
  in `$Z/run` (part A built it so), which must be empty between runs.

## B0: the VM must still be free, and part A complete

```sh
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-featev-1026e; L=/var/log/zts-featev-1026e
id -un; uname -r
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
pgrep -ax 'zed|lt-zed'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1
sudo -n journalctl -k -n 1 --no-pager
command -v fio || echo "no fio"
echo "Z holds: $(LC_ALL=C ls -A $Z | xargs)"
echo "files: mode $(stat -c %a $Z/files), $(ls -A $Z/files | wc -l) entries;" \
    "run: mode $(stat -c %a $Z/run), $(ls -A $Z/run | wc -l) entries"
for f in env.sh partA.txt sha-checks.txt copy.txt srcv-F srcv-V srcv-M tree-F.txt \
    tree-V.txt tree-M.txt bins-F.sha256 bins-V.sha256 bins-M.sha256 PART-A.done; do
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
no busy tmux pane, the `zpool.cache` line as part A found it and `No such file
or directory` for `/etc/zfs/vdev_id.conf`, the path of `fio` or `no fio`
(either is fine: without it `vdev_set_scheduler` skips itself),
`Z holds: F M V files repo run`,
`files: mode 1777, 0 entries; run: mode 755, 0 entries`, no `missing or
empty` line, `results dir: 0 entries`,
`env.sh: sha256 e44769ea3ab9cf06c8cd73d73e1d1b3c593dd40690061f5fbe6545e91dd48557`
(or, if only blanks differ, `env.sh without blanks:
d9328d386a319cdf32aad505bb6c28eecc0da4907480397546e473ae11a5ffbb`),
`part A: 1 done line, 0 mismatch, finished <date>`, and `No such file or
directory` for `envb.sh` and `partB-start.txt`.

Stop at once, changing nothing, if: `id -un` is not `tigerblue77` or
`sudo -n true` fails; a zfs or spl module is loaded or `/dev/zfs` exists; a
test or build process or any `zed` runs, other than this command and the shell
running it; `/etc/zfs/vdev_id.conf` exists; a tmux pane other than this
session's runs anything; `/var/tmp` has less than 6 GB free; `journalctl -k`
prints no kernel line; or any other expected line above differs (part A did
not finish, or something changed its output since: report it, the maintainer
decides). `envb.sh` existing means part B already started once: report, do not
start again.

## B1: part B's helpers, baselines, and the check of part A's trees

```sh
. /var/log/zts-featev-1026e/env.sh
cat > $L/envb.sh <<'ENVB'
# Part B's helpers. env.sh (part A's) is sourced first.
# The test run once beside vdev_add_event, as a zfs-tests.sh -t argument.
RE1=tests/functional/events/events_001_pos.ksh
# The tunable the zpool_set and zpool_add groups change and must restore.
TUN="vdev_file_physical_ashift"
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# A result line of the runner: "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a test that linux.run lists (the events group,
# add_nested_replacing_spare).
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'

# Per test: NRE, the lines of a failed run worth quoting, and CRE, the lines
# counted in every run of it.
pat() {   # pat <test name>
	case $1 in
	zpool_set_feature_disable)
		# A refused or failed command (log_must copies its stderr, then
		# prints "ERROR: ... exited <n>"; log_mustnot prints "SUCCESS:
		# ... exited <n>"), and check_prop's message. One CRE line per
		# "zpool set feature@" that did what the test wanted: 16 on F.
		NRE="exited [0-9]|cannot set property|is '[^']*', expected"
		CRE='SUCCESS: zpool set feature@' ;;
	vdev_add_event)
		# The events the test read, diff's output and the test's own
		# messages. One CRE line per "zpool add": 3 on V.
		NRE='vdev_add events:|exited [0-9]|^[<>] |ZED did not see|unexpected mirror|expected one vdev_add'
		CRE='SUCCESS: zpool add' ;;
	*)
		NRE='^ERROR: '
		CRE='' ;;
	esac
}

# Part A's record of <tree> against the tree as it is now: HEAD, changes,
# srcversion, the built modules, libraries and programs (zed included), and
# the results directory. Any MISMATCH means this is not the tree part A built.
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
	same "$1 zed's pid file in $Z/run" "$(grep -aq "$Z/run/zed.pid" .libs/zed && echo yes || echo no)" yes
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
# Pools, devices, mounts and processes (zed included) may hold the module
# (HELD); files, the vdev_id.conf link the events setup makes, and tunables
# the tests own are theirs to restore. All of them stop.
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
	if [ -e /etc/zfs/vdev_id.conf ] || [ -L /etc/zfs/vdev_id.conf ]; then
		echo "  LEFTOVER $(ls -l /etc/zfs/vdev_id.conf)"; bad=1
	fi
	f=$(ls -A $Z/run)
	[ -n "$f" ] && { echo "  LEFTOVER in $Z/run:" $f; bad=1; }
	f=$(ls -A $Z/files)
	if echo "$f" | grep -qE '^(file-vdev|device-[0-9]|feature_disable\.|vdev[1-6]$|tmp_events|zed$|zpool_set_features\.)'; then
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
# "PASS x16" or "PASS x15, SKIP x1".
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

# One run of a tag group: the same guard, the counts, and the digest of
# every test that did not pass.
zts_group() {   # zts_group <label> <tag>
	local label=$1 since t0 rc d n c
	pre_run $label || return 1
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -T $2 > $L/$label.log 2>&1
	rc=$?
	c=$(grep -E "$TRE" $L/$label.log | tally)
	echo "$label -T $2 ${c:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
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

# On M, a group runs again only when its run on F or V had a result other
# than PASS (vdev_set_scheduler's SKIP without fio excepted): a test failing
# on both states is master's or this VM's, one failing on F or V only is
# that branch's.
again() {   # again <label on F or V> <label on M> <tag>
	if [ -s $L/$1.notpassed ] && grep -qv '^vdev_set_scheduler$' $L/$1.notpassed; then
		zts_group $2 $3
	else
		echo "$2 not run: $1 had no failure" | tee -a $L/runs.txt
	fi
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

Then check the three trees against part A's record, without rebuilding:

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
{
same "srcversions of F, V and M" "$(sort -u $L/srcv-F $L/srcv-V $L/srcv-M | wc -l) different" \
    "3 different"
same "M's test files are F's and V's" "$({
	for f in tests/runfiles/common.run $FF $FS; do
		cmp -s $Z/M/$f $Z/F/$f && echo same || echo "$f differs from F"
	done
	for f in tests/runfiles/linux.run $FV; do
		cmp -s $Z/M/$f $Z/V/$f && echo same || echo "$f differs from V"
	done; } | sort -u | xargs)" same
same "M's Makefile.am lists both tests" \
    "$(grep -cE 'functional/(cli_root/zpool_set/zpool_set_feature_disable|events/vdev_add_event)\.ksh' $Z/M/$MK)" 2
} | tee -a $L/verify.txt
verify_tree F $HF "tests/test-runner/bin/test-runner.py.in" &&
    verify_tree V $HV "tests/test-runner/bin/test-runner.py.in" &&
    verify_tree M $BASE "tests/runfiles/common.run tests/runfiles/linux.run tests/test-runner/bin/test-runner.py.in $MK $FF $FS $FV" &&
    ! grep -q '^MISMATCH' $L/verify.txt && echo "trees verified" | tee -a $L/verify.txt
```

Expected: 30 `ok:` lines (3, then 9 each for F, V and M, the three `built
files` lines with the number of files part A hashed), then `trees verified`.
Any `MISMATCH` means stop: the tree is not the one part A built and recorded.

## B2: state F (pool-feature-disable-v2)

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
switch_to F none
```

Expected: `load F rc=0`, `F guard ok: <srcversion>;
vdev_file_physical_ashift=9 spl_panic_halt=0` (9 is the module's default;
another value set by this VM's configuration is fine, since each run is
compared with what the load found), then `F: no pool after the load`.

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh; cd $Z/F
three f zpool_set_feature_disable $RF && zts_group fg zpool_set &&
    zts_group fu zpool_upgrade
dmesg_state F
```

| Label | Expected on F |
| --- | --- |
| `f1`-`f3` | **PASS x1**, rc=0, `16 lines matching SUCCESS: zpool set feature@` (every `zpool set feature@` of the six steps), no `not passed` line (`zpool_set`'s `setup` and `cleanup` pass), kernel 0, leftovers none, about a minute each. |
| `fg` | `-T zpool_set`, once: **PASS x16** (setup, master's 13 tests of the group, `zpool_set_feature_disable`, cleanup; `zpool_set_features`, which the head changes to expect only active features to be refused, among them), `every test passed`; or, without `fio`, **PASS x15, SKIP x1** with `vdev_set_scheduler` the SKIP. Kernel 0, leftovers none. |
| `fu` | `-T zpool_upgrade`, once: **PASS x12** (setup, 10 tests, cleanup), `every test passed`, kernel 0, leftovers none. `zpool upgrade` enables features through the `spa_prop_set()` and `spa_sync_props()` paths the head changes. |

F's code has never run on any kernel, so a failure here is a result in
itself: it is reported test by test with its first failing line, and B4 runs
the same group on M to tell the head's failures from master's or this VM's.

## B3: state V (vdev-add-event-details)

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
switch_to V F
```

Expected: `unload F rc=0`, `load V rc=0`, `V guard ok: <srcversion>;
vdev_file_physical_ashift=9 spl_panic_halt=0`, `V: no pool after the load`.

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh; cd $Z/V
three v vdev_add_event $RV && zts_group vg zpool_add &&
    zts_run ve events_001_pos $RE1
dmesg_state V
```

| Label | Expected on V |
| --- | --- |
| `v1`-`v3` | **PASS x1**, rc=0, `3 lines matching SUCCESS: zpool add` (the two vdevs, the mirror, the spare), no `not passed` line (the events group's `setup` and `cleanup` pass), kernel 0, leftovers none, well under a minute each. |
| `vg` | `-T zpool_add`, once: **PASS x19** (setup, the 14 tests of `common.run`, cleanup, then `linux.run`'s setup, `add_nested_replacing_spare` and cleanup, as `Test (Linux):` lines), `every test passed`, kernel 0, leftovers none. Every `zpool add` goes through the changed `spa_vdev_add()`; the fork's CI passed all 19 on this head. |
| `ve` | `events_001_pos` once: **PASS x1**, rc=0, no `not passed` line, kernel 0, leftovers none. It is the existing test that waits for `vdev_add` events (a spare added alone, then a log vdev, which now carries its vdev) in `zpool events` and in the ZED's log. |

## B4: state M (both heads' parent + their tests)

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
switch_to M V
```

Expected: `unload V rc=0`, `load M rc=0`, `M guard ok: <srcversion>;
vdev_file_physical_ashift=9 spl_panic_halt=0`, `M: no pool after the load`.

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh; cd $Z/M
if three mf zpool_set_feature_disable $RF && three mv vdev_add_event $RV; then
	again fg mfg zpool_set && again fu mfu zpool_upgrade && again vg mvg zpool_add &&
	if grep -q '^ve events_001_pos PASS x1 ' $L/runs.txt; then
		echo "mve not run: ve passed" | tee -a $L/runs.txt
	else
		zts_run mve events_001_pos $RE1
	fi
fi
dmesg_state M
```

`mf1`-`mf3`: **FAIL x1**, rc=1, each with

```
  [<time>] Test: /var/tmp/zts-featev-1026e/M/tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_feature_disable (run as root) [<mm:ss>] [FAIL]
    note: cannot set property for 'testpool1': property 'feature@multi_vdev_crash_dump' can only be set to 'disabled' at creation time
    note: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
    first failing line: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
    first ERROR line: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
  0 lines matching SUCCESS: zpool set feature@
```

The two `note:` lines can come in the other order, and `first failing line`
is then the `cannot set property` line: the same result (see the lessons
above). `mv1`-`mv3`: **FAIL x1**, rc=1, each with

```
  [<time>] Test: /var/tmp/zts-featev-1026e/M/tests/zfs-tests/tests/functional/events/vdev_add_event (run as root) [<mm:ss>] [FAIL]
    note: NOTE: vdev_add events: - -
    note: < - -
    note: > <guid> /var/tmp/zts-featev-1026e/files/vdev2
    note: > <guid> /var/tmp/zts-featev-1026e/files/vdev3
    note: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
    first failing line: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
    first ERROR line: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
  1 lines matching SUCCESS: zpool add
```

(the two `>` lines in either order: the test sorts them by guid, which is
random). Each of the six runs then shows kernel 0 and leftovers none (each
test's cleanup destroys its pool and removes its files; the events cleanup
stops the ZED, removes `$Z/files/zed` and the `/etc/zfs/vdev_id.conf` link),
followed by `mfg not run: fg had no failure`, `mfu not run: fu had no
failure`, `mvg not run: vg had no failure` and `mve not run: ve passed`.

On `mf`, the one `ERROR` is the first disable, refused by libzfs; `0 lines
matching` says that no `zpool set feature@` did what the test wanted. On `mv`,
`NOTE: vdev_add events: - -` is the single event without a vdev and `1 lines
matching SUCCESS: zpool add` says that the add itself succeeded: the test
stops at the event check after it.

A failure before those lines (a `log_must` on `truncate`, `zpool create`,
`zed_start` or `zpool events -c`, or a setup that does not pass) is a setup
failure: report it as such, it proves nothing about either branch.

## What the results mean

- **As expected** (F passes 3/3 with 16 `zpool set feature@` successes each,
  `fg` and `fu` pass; V passes 3/3 with 3 `zpool add` successes, `vg` and `ve`
  pass; `mf` fails 3/3 at libzfs's refusal of the first disable; `mv` fails
  3/3 at the event check, after `vdev_add events: - -`; no kernel line, no
  leftover):
  - pool-feature-disable-v2: the first kernel run of this head, and of the
    test's steps 4 to 6 anywhere, passes; on master the test fails at the
    refusal openzfs/zfs#19439 reports. That is the only failing-first this
    branch allows (see "Why M fails").
  - vdev-add-event-details: the claim of openzfs/zfs#19566's description
    holds in the kernel: on master the test fails at its first check, because
    the event describes no vdev; with the head it passes.
- **`mf` fails anywhere else** (a setup failure aside), or **`mf` passes**:
  the run did not end at libzfs's refusal of the first disable; report the
  lines, the test is then not a failing-first test here.
- **`mv` passes**, or shows events other than `- -`: master's event carried
  vdev details on this VM; report it.
- **F or V fails a run of its own test**: report the failing lines run by run;
  a `feature@... is '...', expected '...'` line names the step and the state
  the feature was left in.
- **A group or `ve` failure**: compare with `mfg`, `mfu`, `mvg` or `mve`. A
  test failing on both states is master's or this VM's; failing on F or V
  only is that head's. On M, `-T zpool_set` also runs
  `zpool_set_feature_disable`, whose failure there is the expected one.
- **Any kernel VERIFY, panic or hung task on any state**: a different
  problem; the line and the state it happened in are the result. Stop.
- A result that differs between the three runs of one label is reported as it
  is, run by run.

## B5: whole-task kernel check

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
sudo -n journalctl -k --since "$(cat $L/partB-start.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole of part B: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

## B6: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a panicked thread or a leftover pool, device, process or ZED keeps
the module busy, and its files may be in use: report them, and the maintainer
decides, reboot included):

```sh
. /var/log/zts-featev-1026e/env.sh; . /var/log/zts-featev-1026e/envb.sh
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
pgrep -ax 'zed|lt-zed'                 # must print nothing
ls -l /etc/zfs/vdev_id.conf 2>&1       # No such file or directory
rm -rf /var/tmp/zts-featev-1026e
ls -d /var/tmp/zts-featev-1026e 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-featev-1026e` and write
`/var/log/zts-featev-1026e/REPORT.txt` with:

- part A, short: from `PART-A.txt`, the builds' rc, wall time, warnings and
  srcversions, the zed pid file lines, and the `PART-A.done` time;
- B0, short: anything found, `uname -r`, the free space, the
  `/etc/zfs/zpool.cache` line, whether `fio` is present, the `env.sh` hash
  line that matched, and the B1 baselines;
- `verify.txt`;
- every `load`, `unload`, `guard` and `no pool` line;
- `runs.txt` as it stands: every run's result line, its match count, its
  `Test:` line, notes, first failing line and first ERROR line when it did
  not pass, its kernel line and its leftovers line, the groups' counts and
  per-test digests, the `not run` lines, each `dmesg <tree>` line and the
  whole-task kernel count;
- a summary table: each label's results against the expected ones, and which
  case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM 105, kernel ZTS of tigerblue77/zfs:pool-feature-disable-v2 and vdev-add-event-details (PART-A-105.md, PART-B-105.md)

part A: done <date>; builds F rc=0 <s> warnings <n> srcversion <srcv>, V rc=0 <s> warnings <n> srcversion <srcv>,
  M rc=0 <s> warnings <n> srcversion <srcv>; each tree's zed keeps its pid file in /var/tmp/zts-featev-1026e/run
B0: free; tigerblue77; <uname -r>; /var/tmp <N>G free; zpool.cache <line>; no vdev_id.conf; fio <path, or none>;
  part A complete; env.sh sha256 e44769ea3ab9... (or without blanks d9328d386a31...)
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n>, journal (this boot) <n> suspicious lines
verify: 30 ok lines (srcversions 3 different, M's test files are F's and V's, M's Makefile.am lists both tests;
  F, V and M: head, head as recorded, changes as recorded, changed files, srcversion x2, <n> built files unchanged,
  zed's pid file in $Z/run, results directory), trees verified

loads: "load F rc=0", "F guard ok: <srcv>; vdev_file_physical_ashift=9 spl_panic_halt=0", "F: no pool after the load";
  "unload F rc=0", the same for V; "unload V rc=0", the same for M

runs:
f1 zpool_set_feature_disable PASS x1 rc=0 wall=<s>
  16 lines matching SUCCESS: zpool set feature@
kernel f1: 0 suspicious lines
leftovers after f1: none
f2, f3: the same
fg -T zpool_set PASS x16 rc=0 wall=<s>          (without fio: PASS x15, SKIP x1, rc=<n>)
  every test passed                             (without fio: vdev_set_scheduler's Test: line)
kernel fg: 0 suspicious lines
leftovers after fg: none
fu -T zpool_upgrade PASS x12 rc=0 wall=<s>
  every test passed
kernel fu: 0 suspicious lines
leftovers after fu: none
dmesg F: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
v1 vdev_add_event PASS x1 rc=0 wall=<s>
  3 lines matching SUCCESS: zpool add
kernel v1: 0 suspicious lines
leftovers after v1: none
v2, v3: the same
vg -T zpool_add PASS x19 rc=0 wall=<s>
  every test passed
kernel vg: 0 suspicious lines
leftovers after vg: none
ve events_001_pos PASS x1 rc=0 wall=<s>
kernel ve: 0 suspicious lines
leftovers after ve: none
dmesg V: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
mf1 zpool_set_feature_disable FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-featev-1026e/M/tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_feature_disable (run as root) [<mm:ss>] [FAIL]
    note: cannot set property for 'testpool1': property 'feature@multi_vdev_crash_dump' can only be set to 'disabled' at creation time
    note: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
    first failing line: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
    first ERROR line: ERROR: zpool set feature@multi_vdev_crash_dump=disabled testpool1 exited 1
  0 lines matching SUCCESS: zpool set feature@
kernel mf1: 0 suspicious lines
leftovers after mf1: none
mf2, mf3: the same (the two notes may swap, first failing line then the cannot set property line)
mv1 vdev_add_event FAIL x1 rc=1 wall=<s>
  [<time>] Test: /var/tmp/zts-featev-1026e/M/tests/zfs-tests/tests/functional/events/vdev_add_event (run as root) [<mm:ss>] [FAIL]
    note: NOTE: vdev_add events: - -
    note: < - -
    note: > <guid> /var/tmp/zts-featev-1026e/files/vdev2        (these two in either order)
    note: > <guid> /var/tmp/zts-featev-1026e/files/vdev3
    note: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
    first failing line: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
    first ERROR line: ERROR: diff /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.out /var/tmp/zts-featev-1026e/files/tmp_events.<pid>.exp exited 1
  1 lines matching SUCCESS: zpool add
kernel mv1: 0 suspicious lines
leftovers after mv1: none
mv2, mv3: the same
mfg not run: fg had no failure
mfu not run: fu had no failure
mvg not run: vg had no failure
mve not run: ve passed
dmesg M: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
kernel, whole of part B: 0 suspicious lines

summary (3 runs each)          M (e117a84dbb + tests)                   F (c62b3702d6)     V (b87a733ac0)
  zpool_set_feature_disable    FAIL 3/3 (first disable refused          PASS 3/3           -
                               by libzfs, as in #19439)
  vdev_add_event               FAIL 3/3 (one event, no vdev:            -                  PASS 3/3
                               "- -")
  -T zpool_set (F, once)       -                                        PASS x16 (or x15 + vdev_set_scheduler SKIP)
  -T zpool_upgrade (F, once)   -                                        PASS x12
  -T zpool_add (V, once)       -                                                           PASS x19
  events_001_pos (V, once)     -                                                           PASS
  verdict: as expected; pool-feature-disable-v2's first kernel run passes, and on master its test fails at the
           refusal the issue reports (no kernel failing-first is possible); on master vdev_add_event fails at its
           first check, as openzfs/zfs#19566 says, and passes with the head

cleanup: unload M rc=0, no zfs or spl module left, no zed, no /etc/zfs/vdev_id.conf,
  /var/tmp/zts-featev-1026e removed, /var/tmp <N>G free
deviations: none
```

Estimated duration: about 30 minutes (checks 3, the twelve runs of the two
tests about 8, the three groups about 12, `events_001_pos` 1, the three loads
and cleanup 5).
