# VM 107: the `testdir` the l2arc and cache groups leave, before and after

These instructions are for the session on VM 107 (Debian 13). They give the
last two commits of the fork branch `zpool-add-warn-removal-log-pass` of
https://github.com/tigerblue77/zfs their kernel proof: head
`5e95479b7e0ae7789fc64452f75884b52356ba25`, seven commits on upstream master
`a128b8a075b5b13c737e56914cd7043bd3e9ebf8` "Linux 7.3 compat: META". The
session builds one fresh debug tree in its own directory, runs four ZTS
groups in the kernel, checks after each what it left, cleans up, and replies
with `REPORT.txt`. This document is read from a clone of the branch
`vm-tasks-1026` of the same fork in `/var/tmp/vm-task-107s` (the doc
checkout, `DOC` below); the last command of STEP 6 removes it with the task's
own directories.

**The leak.** `l2arc.cfg` puts the l2arc group's vdev files in
`$VDIR=$TESTDIR/disk.l2arc`, and `cache.cfg` puts the cache group's in
`$VDIR=$TESTDIR/disk.cache` and `$VDIR2=$TESTDIR/disk2.cache`. Each group's
`setup.ksh` makes them with `mkdir -p`, which also makes `$TESTDIR`, the
`testdir` directory in the ZTS files directory (`zfs-tests.sh -d`, which is
`TEST_BASE_DIR`). Each group's `cleanup.ksh` removes only its vdev
directories, and none of their tests removes `$TESTDIR` or calls
`default_cleanup`. So on master, after `zfs-tests.sh -T l2arc` and after
`-T cache`, an empty, root-owned `testdir` is left in the files directory,
unless a later group's cleanup happens to remove it.

**The two commits.** `b0324fb5ca` "ZTS: remove TESTDIR after the l2arc tests"
changes `l2arc/cleanup.ksh` from `log_must rm -rf $VDIR` to
`log_must rm -rf $TESTDIR`, and `5e95479b7e` "ZTS: remove TESTDIR after the
cache tests" changes `cache/cleanup.ksh` from `log_must rm -rf $VDIR $VDIR2`
to `log_must rm -rf $TESTDIR`, as `default_cleanup` does. One line each.

**B is the whole branch.** B differs from M by all seven commits of the
branch, not only these two. The other five touch only the removal and
`zpool_add` tests (`zpool_add_warn_removal.ksh`, `removal_with_zdb.ksh`,
`removal_multiple_indirection.ksh`, `removal_condense_export.ksh`) and
`libtest.shlib`'s `vdevs_in_pool()`, which now removes its temporary file
before it returns 1. The l2arc and cache groups run none of those tests. Do
they call `vdevs_in_pool()`? Read at `5e95479b7e` with `git grep`: **no**.
Neither directory names it, nor do the two files of another group that one
l2arc test sources (`cli_root/zfs_load-key/zfs_load-key_common.kshlib` and
the `zfs_load-key.cfg` it sources), and under `tests/zfs-tests/include` the
one occurrence is the function's definition in `libtest.shlib`. STEP 2 and
STEP 4 run the same grep on the VM and record it. So the only difference the
two groups can see between M and B is their own `cleanup.ksh`, which runs
after all their tests: every test result must be the same on both, and only
what is left afterwards may differ.

| Run | Tree state | Group | Files directory | Must leave |
| --- | --- | --- | --- | --- |
| `ml` | M: master `a128b8a075` | `-T l2arc` (setup, 12 tests, cleanup: 14 results) | `$W/files-ml` | exactly an empty `testdir`, root's, and nothing else |
| `mc` | M | `-T cache` (setup, 12 tests, cleanup: 14 results) | `$W/files-mc` | the same |
| `bl` | B: the branch's seven files checked out from `5e95479b7e` | `-T l2arc` | `$W/files-bl` | nothing |
| `bc` | B | `-T cache` | `$W/files-bc` | nothing |

There is one tree, `$W/T`, a worktree at `a128b8a075` built here once with
`./autogen.sh && ./configure --enable-debug && make -j8`. All seven files are
test scripts under `tests/zfs-tests`, which the in-tree runner reads from the
source tree, so M and B share one build and one module load: STEP 4 checks
the seven files out from the head with `git checkout`, and shows that the
tree is then the head's, the runner's results directory apart. Each run has
its own files directory, so that what it leaves is its own. Its srcversion
should be `8852710DB871A7587B9E722`, as for every earlier build of this
master on this VM.

## Rules

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: the build runs under
  `nice -n 10 ionice -c3` with `make -j8`, never more; the module load and
  every run go through `nice -n 10 ionice -c3` too (the blocks do it), one
  thing at a time, nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing once STEP 2 has built the
  tree. `scripts/zfs.sh -r` (load) and `-u` (unload) run only here, on this
  VM, and only in the blocks that say so.
- This task's own directories are `/var/tmp/zts-l2c-1028` (`$W`) and
  `/var/log/zts-l2c-1028` (`$L`). Outside them, the only change is STEP 0's
  release of `/var/tmp/vm-task-107n`, the doc checkout of the previous task,
  under the conditions written there, and STEP 6's removal of the doc
  checkout of this one. Never touch `/var/tmp/zts-algo-1026`,
  `/var/tmp/zts-enc-1026`, another `/var/tmp/zts-*` or `/var/log/zts-*`
  directory, another `/var/tmp/vm-task-*` checkout or `/var/tmp/test_results`.
  Never create, edit or remove `/etc/zfs/zpool.cache` or
  `/etc/zfs/vdev_id.conf` yourself.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only these: STEP 0's `rm -rf` of
  `/var/tmp/vm-task-107n`, STEP 6's `sudo -n rmdir` of the two `testdir`
  directories the M runs leave and its `rmdir` of the four files
  directories, and the final removals of STEP 6.
- Never run a `zpool` or `zfs` command while this tree's module is not loaded.
  Never print the environment (`env`, `printenv`, `set`, `/proc/*/environ`) or
  a key file.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-l2c-1028/env.sh`.
- Stop at any value the comments or Expected lines do not allow, unless the
  text says it is recorded only. After a line ending in `stop`, run only
  STEP 6; the helpers refuse to run once a step has stopped. A test that fails
  or is skipped is a result, recorded and compared, not a stop. Clean, repair
  and retry nothing by hand. If your permission system refuses a command,
  report it and stop. A kernel panic, oops, VERIFY or hung task means stop and
  report, never reboot.
- The build takes about 15 minutes, `ml` and `bl` 10 to 25 each, `mc` and `bc`
  5 to 15. If your shell tool cannot wait that long in the foreground, start
  that one block with the tool's own background mode and wait for it to exit.
  Never use `&` or `nohup`, and run nothing meanwhile.
- Your session cannot run anything after its final reply. Everything, cleanup
  included, happens before it; the reply is the whole of `REPORT.txt` and the
  result of the last command.

What earlier VM runs taught, and the blocks already handle: `modinfo` is in
`/usr/sbin`, not in `tigerblue77`'s PATH, so every block puts `/usr/sbin:/sbin`
first; this VM's umask is 0002, and `env.sh` sets 022; `/var/log` directories
are made with `sudo -n mkdir` and then given to `tigerblue77`.
`configure --enable-debug` writes 3 `#define ZFS_DEBUG 1` lines; compiler
warnings are counted as `:[0-9]+:[0-9]+: warning:`, and objtool's about 83
harmless `warning:` lines per tree are not counted. Results are matched
anywhere in a `Test:` line, which starts with a timestamp; a `-T` run names
tests without `.ksh`, and `linux.run` tests are `Test (Linux): ...` (none
here). A literal parenthesis in a regex is `[(]`. The tests' lines are read
from the runner's own `log` file. Kernel messages are read per run from
`journalctl -k --since`, without the `ZTS run` markers and the
`spa_import_progress_set_notes_impl` lines. Each run waits 2 s first. A `-d`
directory has mode 1777. The l2arc tests need `fio`; no test here uses a zvol
or `zed`.

## STEP 0: the VM free, the space, and the previous doc checkout

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-l2c-1028 L=/var/log/zts-l2c-1028 DOC=/var/tmp/vm-task-107s
date -u '+%F %T'
id -un                                # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                              # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release      # Debian GNU/Linux 13 (trixie)
nproc; umask                          # 16, then 0002
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '          # nothing
ls /sys/module | grep -xE 'zfs|spl'    # nothing
ls -l /dev/zfs 2>&1                    # No such file or directory
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db|[f]io '   # nothing
pgrep -ax 'zed|lt-zed'                 # nothing
ls -l /var/run/zed.pid 2>&1            # No such file or directory: zfs.sh -r and -u signal the pid it names
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                # No devices found
grep -E ' zfs |testpool' /proc/mounts  # nothing
ls -d /testpool* 2>&1                  # No such file or directory
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
ls -d $W $L 2>&1                       # No such file or directory, twice
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
echo "/var/log zts dirs: $(ls -d /var/log/zts-* 2>/dev/null | xargs)"
df -h /var/tmp /var/log
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
sudo -n journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
for c in git ksh python3 modinfo sha256sum timeout fio; do echo "$c: $(command -v $c || echo absent)"; done
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/s/VM-TESTS-107-l2c.md 2>&1
```

Keep this output: it goes into `REPORT.txt` by hand. The number printed
after the one kernel line is the kernel baseline. The `/var/tmp` and `/var/log`
lines are recorded only: earlier tasks' directories there are left alone.
The doc checkout line is expected as `doc checkout: top
/var/tmp/vm-task-107s, branch vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`); if you read
this document from another place, say so: STEP 6 then leaves that place alone.

Stop at once, changing nothing, and report what you found if: the user is not
`tigerblue77` or `sudo -n` fails; the kernel headers are missing; a zfs or
spl module is loaded or `/dev/zfs` exists; a test, build, fio, ksh or zdb
process or any `zed` runs (ignore the line of the shell running this block,
if `pgrep` shows it), or `/var/run/zed.pid` exists; a tmux pane other than
this session's runs anything but an idle shell; a loop or device-mapper
device, a zfs or test mount, a
`/testpool*` directory, a non-empty `zpool.cache` or a `vdev_id.conf` exists;
`$W` or `$L` exists (this task started once already); `journalctl -k` prints
nothing; or `git`, `ksh`, `python3`, `modinfo`, `sha256sum`, `timeout` or
`fio` is absent (without `fio` the l2arc tests are skipped, and the l2arc
runs would not be the group's). A kernel other than
`6.12.113+deb13-cloud-amd64` is not a stop by itself: STEP 2 checks that the
module was built for the running one.

Then the previous task's doc checkout, `/var/tmp/vm-task-107n`. It is removed
only if it exists and holds nothing but that checkout: a clone of
`vm-tasks-1026` of the fork, at its top level, with no change, no untracked
or ignored file, `n/VM-TESTS-107-enc.md` tracked in it, and nothing mounted
under it. Its size and contents are recorded first:

```sh
export PATH=/usr/sbin:/sbin:$PATH
P=/var/tmp/vm-task-107n
{
if [ -e $P ] || [ -L $P ]; then
	echo "$P: $(stat -c '%F %U %a' $P), $(du -sh $P 2>&1 | cut -f1)"
	echo "$P holds: $(LC_ALL=C ls -A $P | xargs)"
	echo "  tracked top-level entries: $(git -C $P ls-tree --name-only HEAD 2>&1 | LC_ALL=C sort | xargs)"
	echo "  top $(git -C $P rev-parse --show-toplevel 2>&1), branch $(git -C $P rev-parse --abbrev-ref HEAD 2>&1)," \
	    "head $(git -C $P log -1 --format=%h 2>&1), origin $(git -C $P remote get-url origin 2>&1)"
	echo "  status lines: $(git -C $P status --porcelain --ignored --untracked-files=all 2>&1 | wc -l)"
	if [ -d $P ] && [ ! -L $P ] && [ "$(stat -c %U $P)" = tigerblue77 ] &&
	    [ "$(git -C $P rev-parse --show-toplevel 2>/dev/null)" = $P ] &&
	    [ "$(git -C $P rev-parse --abbrev-ref HEAD 2>/dev/null)" = vm-tasks-1026 ] &&
	    case "$(git -C $P remote get-url origin 2>/dev/null)" in
	    https://github.com/tigerblue77/zfs|https://github.com/tigerblue77/zfs.git) true ;;
	    *) false ;; esac &&
	    git -C $P ls-files --error-unmatch n/VM-TESTS-107-enc.md > /dev/null 2>&1 &&
	    [ -z "$(git -C $P status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    [ "$(LC_ALL=C ls -A $P | grep -vx .git | xargs)" = "$(git -C $P ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
	    ! grep -q " ${P}[ /]" /proc/mounts; then
		rm -rf $P; echo "released $P rc=$?"
	else
		echo "$P is not only the previous doc checkout: kept"
	fi
else
	echo "$P: absent, nothing to release"
fi
ls -d $P 2>&1
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"
} 2>&1
```

Expected: either the size and contents lines, then `released
/var/tmp/vm-task-107n rc=0` and `No such file or directory`, or
`/var/tmp/vm-task-107n: absent, nothing to release` and `No such file or
directory`; then the free space. `... is not only the previous doc checkout:
kept` is not a stop: record why (the lines above it say which condition
failed). A `rm` that does not end with rc 0, or that your permission system
refuses, is reported and not retried. **Stop** if less than `10G` is free on
`/var/tmp` now (the tree, the clone, and the l2arc group's vdev files, which
`mkfile` fills: about 6 GB at the peak).

## STEP 1: this task's directories and helpers

The block writes nothing unless it can create both directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/tmp/zts-l2c-1028 && sudo -n mkdir /var/log/zts-l2c-1028 &&
    sudo -n chown tigerblue77: /var/log/zts-l2c-1028; then
echo "mkdir rc=0"
cat > /var/log/zts-l2c-1028/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-l2c-1028
L=/var/log/zts-l2c-1028
DOC=/var/tmp/vm-task-107s
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
FIXBRANCH=zpool-add-warn-removal-log-pass
# Master (the tree's commit), the branch's head and its seven commits in
# order (the last two are the l2arc and cache fixes).
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
HB=5e95479b7e0ae7789fc64452f75884b52356ba25
C7="4e8a517729942d1ac159eb49cfe9c93574fc0b99 1ae75ec36558291eb3bd469cda999ec33f76d97e e0725c9ae214b22f0f43e985a567306688df5ae9 56b4b06599cd8d2a8f40d2bb8de054ede3ab6c4c 147a85264cb49fa1e2c94cba2df167de54946203 b0324fb5ca1adab5b0d29070e8651f05f1c7316d 5e95479b7e0ae7789fc64452f75884b52356ba25"
# The branch's seven files (in git's order), their blobs on master (BM) and
# on the branch (BB) in the same order, their modes, and the patch-id.
Z=tests/zfs-tests
FILES="$Z/include/libtest.shlib $Z/tests/functional/cache/cleanup.ksh
$Z/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh
$Z/tests/functional/l2arc/cleanup.ksh
$Z/tests/functional/removal/removal_condense_export.ksh
$Z/tests/functional/removal/removal_multiple_indirection.ksh
$Z/tests/functional/removal/removal_with_zdb.ksh"
FILES=$(echo $FILES)
BM="629eb52aaeba514198a8d4573561b5eed10ab31a c4359b00bc32e931f75e08f68f02fd8d90a90158 1591658ba7898aad7a121595da768c0a2215ac73 6f3497aee4c5ea48a266251961c7a73a685f8838 aa0d5e2cfde5b537ef5653d65e10ccf90f8e9b34 cfb08c364ac23d2d81425f43d197a093db7cbb2b f486d8da44d9ad35f93b0bde2b09d7bacbe55dbe"
BB="a7db244ef25c951ed6489806f79b85e744feec1b 4af7102cdd25200d78f830f28046d68be0c29ee9 7984f12bdfa6e3c5baa79f993311fbd763e8970d 5db29a27f55b9ae0b3940c980dd7e9aeebb865ec 6a73c26d363dff1657b8c87ace3815e901392ce2 1a0ef72aa0a20101f985465c678037a7886b77f0 6318906a1c555349d5a7ce3dbfdb8ad8d9a54b0e"
MODES="644 755 755 755 755 755 755"
NUMSTAT="4 1 $Z/include/libtest.shlib 1 1 $Z/tests/functional/cache/cleanup.ksh 2 0 $Z/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh 1 1 $Z/tests/functional/l2arc/cleanup.ksh 1 0 $Z/tests/functional/removal/removal_condense_export.ksh 2 2 $Z/tests/functional/removal/removal_multiple_indirection.ksh 2 2 $Z/tests/functional/removal/removal_with_zdb.ksh"
PID=62f1631b5e27318156cd1448f65b484cba71683a
TR=tests/test-runner/bin/test-runner.py
# Where vdevs_in_pool() could come from for the two groups: their
# directories and the zfs_load-key files one l2arc test sources.
VIP="$Z/tests/functional/l2arc $Z/tests/functional/cache $Z/tests/functional/cli_root/zfs_load-key/zfs_load-key_common.kshlib $Z/tests/functional/cli_root/zfs_load-key/zfs_load-key.cfg"
# The tunables the l2arc and cache groups change and must restore.
TUN="zfs_arc_max zfs_arc_min l2arc_dwpd_limit l2arc_ext_headroom_pct
l2arc_mfuonly l2arc_noprefetch l2arc_rebuild_blocks_min_l2size
l2arc_rebuild_enabled l2arc_write_max zfs_prefetch_disable"
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

# A step that stops records why in $L/STOP; no run starts after it.
stop() {   # stop <reason>
	echo "$*: stop" | tee -a $L/runs.txt
	[ -e $L/STOP ] || echo "$*" > $L/STOP
	return 1
}
stopped() {
	[ -e $L/STOP ] || return 1
	echo "stopped earlier ($(cat $L/STOP)): nothing run" | tee -a $L/runs.txt
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

# The blobs and modes of the branch's seven files in T, in FILES's order.
blobs() {
	local f
	for f in $FILES; do
		printf '%s ' "$(git -C $W/T hash-object $f)"
	done | sed 's/ $//'
}
modes() {
	local f
	for f in $FILES; do
		printf '%s ' "$(stat -c %a $W/T/$f)"
	done | sed 's/ $//'
}

# Files that name vdevs_in_pool among those the two groups use, then its
# occurrences under tests/zfs-tests/include, in T as it is now.
vip_grep() {
	echo "vdevs_in_pool in the two groups and the files they source: [$(cd $W/T && grep -rl vdevs_in_pool $VIP | xargs)]"
	echo "vdevs_in_pool under $Z/include: [$(cd $W/T && grep -rn vdevs_in_pool $Z/include | cut -d: -f1,3 | xargs)]"
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
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[f]io ' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Result counts of runner result lines on stdin: "PASS x14", "PASS x13, FAIL x1".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
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

# The group cleanup's "rm -rf" of what lies in testdir, from a runner log
# (destroy_pool's "rm -rf /testpool", if any, is left out).
cleanup_rm() {   # cleanup_rm <runner log> <tag>
	awk -v d="/$2/cleanup (" '
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		sec = index($0, d); next }
	sec && /SUCCESS: rm -rf .*[/]testdir/ { line = $0; sub(/^[0-9:.]+ /, "", line); print "  cleanup: " line }' "$1"
}

# Kernel messages since <since>, from the journal. A finding stops; a panic,
# VERIFY or hung task also keeps the module loaded (PANIC).
kern_check() {   # kern_check <label> <since> <expected ZTS marks>
	local n m
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $L/kern-$1.txt 2>&1
	n=$(grep -iE "$KRE" $L/kern-$1.txt | grep -vcE "$KIGN")
	m=$(grep -c 'ZTS run ' $L/kern-$1.txt)
	echo "kernel $1: $n findings, $m ZTS marks (expected $3)" | tee -a $L/runs.txt
	grep -iE "$KRE" $L/kern-$1.txt | grep -vE "$KIGN" | head -n 20 | sed 's/^/  /' | tee -a $L/runs.txt
	if grep -iE "$KRE" $L/kern-$1.txt | grep -vE "$KIGN" |
	    grep -qiE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than'; then
		touch $L/PANIC
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

# What <fd> holds: "" (nothing), "testdir" when it holds exactly an empty
# directory testdir owned by root and not a mount point, or "OTHER: ...".
fd_state() {   # fd_state <fd>
	local f
	f=$(LC_ALL=C ls -A $1 | xargs)
	if [ -z "$f" ]; then
		echo ""
	elif [ "$f" = testdir ] && [ ! -L $1/testdir ] &&
	    [ "$(stat -c '%F %U' $1/testdir)" = "directory root" ] &&
	    [ -z "$(sudo -n ls -A $1/testdir)" ] && ! grep -q " $1/testdir " /proc/mounts; then
		echo testdir
	else
		echo "OTHER: $f"
	fi
}

# Before a run: nothing stopped, a new label, this tree's module, an empty
# files directory of mode 1777, and none of what a run can leave.
pre_run() {   # pre_run <label> <fd>
	local cur p l m r z t
	stopped && return 1
	if [ -e $L/$1.log ]; then
		echo "label $1 already used: nothing run" | tee -a $L/runs.txt
		return 1
	fi
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$cur" != "$SV" ]; then
		stop "$1: the loaded srcversion is [$cur], not $SV"
		return 1
	fi
	p=$(sudo -n $W/T/zpool list -H -o name 2>&1)
	l=$(loops)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	r=$(ls -d /testpool* 2>/dev/null | xargs)
	z=$(pgrep -ax 'zed|lt-zed')
	t="tunables as loaded"
	[ "$(tunables)" = "$(cat $L/tunables-loaded.txt)" ] || t="TUNABLES CHANGED: $(tunables)"
	echo "before $1: fd=$2 ($(stat -c '%U %a' $2), holds [$(LC_ALL=C ls -A $2 | xargs)]) pools=[$p] loops=[$l] mounts=[$m] rootdirs=[$r] zed=[$z] $t" |
	    tee -a $L/runs.txt
	if [ -n "$(LC_ALL=C ls -A $2)" ] || [ "$(stat -c %a $2)" != 1777 ] ||
	    [ -n "$p$l$m$r$z" ] || [ "$t" != "tunables as loaded" ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: what it must leave in its files directory (<want>: "" or
# "testdir"), and nothing else anywhere. A pool, device, mount, process or
# loop device left may hold the module (HELD: STEP 6 does not unload).
leftovers() {   # leftovers <label> <fd> <want>
	local bad=0 held=0 p s
	{
	p=$(sudo -n $W/T/zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; held=1; }
	p=$(sudo -n dmsetup ls 2>&1)
	[ "$p" = "$(cat $L/dm-before.txt)" ] || { echo "  LEFTOVER dm devices: $p"; held=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; held=1; }
	p=$(ls -d /testpool* 2>/dev/null | xargs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; held=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; held=1; }
	p=$(tunables)
	if [ "$p" = "$(cat $L/tunables-loaded.txt)" ]; then
		echo "  tunables: as loaded"
	else
		echo "  LEFTOVER tunables: $p"; bad=1
	fi
	p=$(loops)
	echo "  loop devices: [$(printf '%s' "$p" | tr '\n' ';')]"
	[ -n "$p" ] && held=1
	s=$(fd_state $2)
	echo "  $2 holds: [$(LC_ALL=C ls -A $2 | xargs)] (expected [$3])"
	if [ -n "$(LC_ALL=C ls -A $2)" ]; then
		sudo -n ls -la --time-style=+%T $2 $2/* 2>&1 | sed 's/^/    /'
	fi
	if [ "$s" != "$3" ]; then
		echo "  LEFTOVER in $2: [$s], expected [$3]"; bad=1
	fi
	if [ $bad$held = 00 ]; then
		echo "leftovers after $1: as expected"
	else
		echo "leftovers after $1: NOT AS EXPECTED"
	fi
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	[ $bad$held = 00 ] && return 0
	[ $held = 1 ] && touch $L/HELD
	stop "$1: leftovers"
}

# One tag group (-T <tag>) into its own files directory, then its results,
# the cleanup's rm line, the kernel and what it left. A test result is
# recorded, not a stop.
group() {   # group <label> <tag> <fd> <want left>
	local label=$1 tag=$2 fd=$3 want=$4 since t0 rc c nt nl np d n bad=0
	pre_run $label $fd || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/T && timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $fd -T $tag) > $L/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $L/$label.log > $L/$label-tests.txt
	c=$(tally < $L/$label-tests.txt)
	nt=$(grep -c 'Test: ' $L/$label-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $L/$label-tests.txt)
	echo "$label -T $tag ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl) rc=$rc" \
	    "wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -m 1 '^Missing util' $L/$label.log | sed 's/^/  /' | tee -a $L/runs.txt
	grep -v '\[PASS\]' $L/$label-tests.txt | sed 's/^/  not passed: /' | tee -a $L/runs.txt
	np=$(grep -v '\[PASS\]' $L/$label-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	d=$(awk '/^Log directory/ { print $3 }' $L/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		for n in $np; do
			fails "$d/log" $n
		done | tee -a $L/runs.txt
		cleanup_rm "$d/log" $tag | tee -a $L/runs.txt
	else
		echo "  no runner log (Log directory: '$d')" | tee -a $L/runs.txt
	fi
	kern_check $label "$since" 14 || bad=1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		stop "$label: outer timeout"
		bad=1
	fi
	leftovers $label $fd "$want" || bad=1
	return $bad
}
ENV
bash -n /var/log/zts-l2c-1028/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-l2c-1028/env.sh
date '+%F %T' > $L/start.txt
sudo -n dmsetup ls > $L/dm-before.txt 2>&1
for d in ml mc bl bc; do mkdir -m 1777 $W/files-$d || echo "mkdir files-$d failed"; done
{
echo "start $(cat $L/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
stat -c '%U %a %n' $W $L $W/files-ml $W/files-mc $W/files-bl $W/files-bc
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "fio: $(command -v fio || echo missing)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then `start ...; tigerblue77;
<uname -r>; umask 0022`, six `stat` lines (`tigerblue77 755` for `$W` and
`$L`, `tigerblue77 1777` for the four files directories), the `zpool.cache`,
`fio` and free-space lines. No `mkdir files-.. failed` line.

## STEP 2: the sources, the tree, the build

Clone the branch (read-only towards GitHub), fetch upstream master, and pin
the branch: its head, its seven commits and the file each changes, its
blobs, its patch-id, the merge base, and the `vdevs_in_pool` grep at the head:

```sh
. /var/log/zts-l2c-1028/env.sh
if ! stopped; then
	{
	git clone -q --no-checkout --single-branch --branch $FIXBRANCH $FORK $W/repo > $L/clone.log 2>&1; echo "clone rc=$?"
	git -C $W/repo fetch -q --no-tags $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
	cd $W/repo
	same "$FIXBRANCH" "$(git rev-parse -q --verify origin/$FIXBRANCH)" $HB
	same "its commits on master" "$(git rev-list --reverse $HM..$HB 2>&1 | xargs)" "$C7"
	same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HB 2>&1)" $HM
	for c in $C7; do
		echo "  ${c%${c#??????????}}: $(git log -1 --format=%s $c)" \
		    "($(git diff-tree --no-commit-id --numstat -r $c | awk '{ print $3 " +" $1 " -" $2 }' | xargs))"
	done
	same "what it changes" "$(git diff --numstat $HM $HB | xargs)" "$NUMSTAT"
	same "nothing outside $Z" "$(git diff --name-only $HM $HB -- . ":!$Z" | wc -l)" 0
	same "its blobs" "$(for f in $FILES; do printf '%s ' "$(git rev-parse -q --verify $HB:$f)"; done | sed 's/ $//')" "$BB"
	same "master's blobs" "$(for f in $FILES; do printf '%s ' "$(git rev-parse -q --verify $HM:$f)"; done | sed 's/ $//')" "$BM"
	same "its patch-id" "$(git diff $HM $HB | git patch-id --stable | cut -d' ' -f1)" $PID
	echo "at the head, vdevs_in_pool in the two groups and the files they source: [$(git grep -l vdevs_in_pool $HB -- $VIP | xargs)]"
	echo "at the head, vdevs_in_pool under $Z/include: [$(git grep -n vdevs_in_pool $HB -- $Z/include | cut -d: -f2,4 | xargs)]"
	echo "at the head, what the two groups source: $(git grep -h -E '^\. ' $HB -- $Z/tests/functional/l2arc $Z/tests/functional/cache | sed 's|^\. \$STF_SUITE/||' | LC_ALL=C sort | uniq -c | awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }')"
	git worktree add --detach $W/T $HM > $L/worktree.log 2>&1; echo "worktree rc=$? at $(git -C $W/T rev-parse --short=10 HEAD)"
	} 2>&1 | tee $L/fix.txt
	grep -q '^MISMATCH' $L/fix.txt && stop "the branch is not as expected"
fi
```

Expected:

```
clone rc=0
fetch rc=0
ok: zpool-add-warn-removal-log-pass
ok: its commits on master
ok: merge base with upstream master
  4e8a517729: ZTS: end zpool_add_warn_removal with log_pass (tests/zfs-tests/tests/functional/cli_root/zpool_add/zpool_add_warn_removal.ksh +2 -0)
  1ae75ec365: ZTS: remove vdevs_in_pool's temporary file when a vdev is missing (tests/zfs-tests/include/libtest.shlib +4 -1)
  e0725c9ae2: ZTS: create removal_with_zdb's temporary file after the Linux check (tests/zfs-tests/tests/functional/removal/removal_with_zdb.ksh +2 -2)
  56b4b06599: ZTS: restore zfs_remove_max_segment in removal_multiple_indirection (tests/zfs-tests/tests/functional/removal/removal_multiple_indirection.ksh +2 -2)
  147a85264c: ZTS: remove TESTDIR1 after removal_condense_export (tests/zfs-tests/tests/functional/removal/removal_condense_export.ksh +1 -0)
  b0324fb5ca: ZTS: remove TESTDIR after the l2arc tests (tests/zfs-tests/tests/functional/l2arc/cleanup.ksh +1 -1)
  5e95479b7e: ZTS: remove TESTDIR after the cache tests (tests/zfs-tests/tests/functional/cache/cleanup.ksh +1 -1)
ok: what it changes
ok: nothing outside tests/zfs-tests
ok: its blobs
ok: master's blobs
ok: its patch-id
at the head, vdevs_in_pool in the two groups and the files they source: []
at the head, vdevs_in_pool under tests/zfs-tests/include: [tests/zfs-tests/include/libtest.shlib:function vdevs_in_pool]
at the head, what the two groups source: include/libtest.shlib x15, tests/functional/cache/cache.cfg x14, tests/functional/cache/cache.kshlib x14, tests/functional/cli_root/zfs_load-key/zfs_load-key_common.kshlib x1, tests/functional/l2arc/l2arc.cfg x14
worktree rc=0 at a128b8a075
```

A `MISMATCH` (the branch moved) or a failed clone or fetch means stop. The
two `vdevs_in_pool` lines are the answer recorded at the top of this
document: no file of the two groups, or that they source, calls it.

Then build the tree (about 15 minutes; see the rule about long commands),
check it, and point its runner's results at `$L/results`:

```sh
. /var/log/zts-l2c-1028/env.sh
if ! stopped; then
	t0=$(date +%s)
	(cd $W/T && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug && make -j8") > $L/build.log 2>&1
	rc=$?
	{
	echo "build rc=$rc wall=$(( $(date +%s) - t0 ))s"
	cd $W/T
	same "build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build.log)" 0
	same "debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1)" 3
	same "vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
	same "files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb \
	    scripts/zfs.sh scripts/zfs-tests.sh $TR; do [ -e $f ] || echo $f; done | xargs)" ""
	echo "compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build.log) (expected 0);" \
	    "other warning: lines: $(grep 'warning:' $L/build.log | grep -cvE ':[0-9]+:[0-9]+: warning:') (objtool's, about 83)"
	grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build.log
	echo "srcversion $(modinfo -F srcversion module/zfs.ko 2>&1); every earlier build of this master on this VM: 8852710DB871A7587B9E722"
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" $TR.in $TR
	same "results directory" "$(grep -cx "BASEDIR = '$L/results'" $TR)" 1
	same "T head" "$(git rev-parse HEAD)" $HM
	same "T changes" "$(git diff HEAD --name-only | xargs)" $TR.in
	} 2>&1 | tee $L/build.txt
	if [ $rc -ne 0 ] || grep -q '^MISMATCH' $L/build.txt; then
		stop "the build"
	else
		printf 'SV=%s\n' "$(modinfo -F srcversion $W/T/module/zfs.ko)" >> $L/env.sh
		tail -n 1 $L/env.sh
	fi
fi
```

Expected: `build rc=0 wall=<s>s`, four `ok:` lines, `compiler warnings: 0`
and about 83 other lines, `srcversion 8852710DB871A7587B9E722; every earlier
build ...`, three more `ok:` lines (the runner's `.in` file is the only
tracked change), and `SV=<the srcversion>`. A compiler warning count other
than 0 is not a stop: quote its lines. Another srcversion is not a stop
either (every check above passed): report it.

## STEP 3: M: the l2arc group, then the cache group

Load the tree's module stack (nothing is loaded, STEP 0, so the reload
loads), then run the two groups on master's files:

```sh
. /var/log/zts-l2c-1028/env.sh
if ! stopped; then
	{
	since=$(date '+%F %T')
	(cd $W/T && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -r); echo "load rc=$?"
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" $SV
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV" ]; then
		same "pools after the load" "$(sudo -n $W/T/zpool list -H -o name 2>&1)" ""
		tunables > $L/tunables-loaded.txt
		echo "after the load: $(cat $L/tunables-loaded.txt)"
	fi
	same "the l2arc and cache cleanups are master's" "$(git -C $W/T hash-object $Z/tests/functional/l2arc/cleanup.ksh $Z/tests/functional/cache/cleanup.ksh | xargs)" \
	    "6f3497aee4c5ea48a266251961c7a73a685f8838 c4359b00bc32e931f75e08f68f02fd8d90a90158"
	kern_check load "$since" 0
	} 2>&1 | tee $L/load.txt
	grep -q '^MISMATCH' $L/load.txt && { touch $L/HELD; stop "the module load"; }
fi
```

Expected:

```
load rc=0
ok: loaded srcversion
ok: pools after the load
after the load: zfs_arc_max=0 zfs_arc_min=0 l2arc_dwpd_limit=100 l2arc_ext_headroom_pct=25 l2arc_mfuonly=0 l2arc_noprefetch=1 l2arc_rebuild_blocks_min_l2size=1073741824 l2arc_rebuild_enabled=1 l2arc_write_max=67108864 zfs_prefetch_disable=0
ok: the l2arc and cache cleanups are master's
kernel load: 0 findings, 0 ZTS marks (expected 0)
```

(These are the module's defaults. A value set by this VM's configuration is
fine, since each run is compared with what the load found.)

```sh
. /var/log/zts-l2c-1028/env.sh
group ml l2arc $W/files-ml testdir
```

```sh
. /var/log/zts-l2c-1028/env.sh
group mc cache $W/files-mc testdir
```

Run the second only after the first has printed its `leftovers after ml:`
line. Expected:

```
before ml: fd=/var/tmp/zts-l2c-1028/files-ml (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
ml -T l2arc PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <whatever it lists, or no such line>
  cleanup: SUCCESS: rm -rf /var/tmp/zts-l2c-1028/files-ml/testdir/disk.l2arc
kernel ml: 0 findings, 14 ZTS marks (expected 14)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-l2c-1028/files-ml holds: [testdir] (expected [testdir])
    <the ls -la lines: testdir, a directory owned by root, and nothing in it>
leftovers after ml: as expected
before mc: fd=/var/tmp/zts-l2c-1028/files-mc (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
mc -T cache PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <whatever it lists, or no such line>
  cleanup: SUCCESS: rm -rf /var/tmp/zts-l2c-1028/files-mc/testdir/disk.cache /var/tmp/zts-l2c-1028/files-mc/testdir/disk2.cache
kernel mc: 0 findings, 14 ZTS marks (expected 14)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-l2c-1028/files-mc holds: [testdir] (expected [testdir])
    <the same shape>
leftovers after mc: as expected
```

That is the leak on master: each group passes, its cleanup removes only its
vdev directories (the `cleanup:` line), and `testdir`, empty and root's, is
all it leaves. A test that does not pass is listed as `not passed:` with its
first ERROR line and last line; it is a result, compared in STEP 5. A
`LEFTOVER` line, or `leftovers after ...: NOT AS EXPECTED`, means the run left
something else: the `ls -la` lines say what, and the step stops.

## STEP 4: B: the branch's seven files, the same two groups

Put the seven files of the head in the tree, and only them, check that the
tree is then the head's (the runner's results directory apart), and run the
`vdevs_in_pool` grep on it:

```sh
. /var/log/zts-l2c-1028/env.sh
if ! stopped; then
	{
	cd $W/T
	git checkout $HB -- $FILES; echo "checkout rc=$?"
	same "T: the seven files" "$(blobs)" "$BB"
	same "T: their modes" "$(modes)" "$MODES"
	same "T: changes against master" "$(git diff HEAD --name-only | xargs)" \
	    "$(echo $FILES $TR.in | xargs -n1 | LC_ALL=C sort | xargs)"
	same "T: the seven files against master" "$(git diff HEAD --numstat -- $FILES | xargs)" "$NUMSTAT"
	same "T: their patch-id against master" "$(git diff HEAD -- $FILES | git patch-id --stable | cut -d' ' -f1)" $PID
	same "T against the head" "$(git diff $HB --name-only | xargs)" $TR.in
	vip_grep
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" $SV
	} 2>&1 | tee $L/swap.txt
	grep -q '^MISMATCH' $L/swap.txt && stop "the files in T are not the branch's"
fi
```

Expected: `checkout rc=0`, six `ok:` lines, then

```
vdevs_in_pool in the two groups and the files they source: []
vdevs_in_pool under tests/zfs-tests/include: [tests/zfs-tests/include/libtest.shlib:function vdevs_in_pool]
ok: loaded srcversion
```

Nothing is rebuilt or reloaded: the seven files are scripts, and the module
is the same. Then the two groups again:

```sh
. /var/log/zts-l2c-1028/env.sh
group bl l2arc $W/files-bl ""
```

```sh
. /var/log/zts-l2c-1028/env.sh
group bc cache $W/files-bc ""
```

Expected, as for M but with the head's cleanup and nothing left:

```
before bl: fd=/var/tmp/zts-l2c-1028/files-bl (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
bl -T l2arc PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <as in ml>
  cleanup: SUCCESS: rm -rf /var/tmp/zts-l2c-1028/files-bl/testdir
kernel bl: 0 findings, 14 ZTS marks (expected 14)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-l2c-1028/files-bl holds: [] (expected [])
leftovers after bl: as expected
before bc: fd=/var/tmp/zts-l2c-1028/files-bc (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
bc -T cache PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
  Missing util(s): <as in mc>
  cleanup: SUCCESS: rm -rf /var/tmp/zts-l2c-1028/files-bc/testdir
kernel bc: 0 findings, 14 ZTS marks (expected 14)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-l2c-1028/files-bc holds: [] (expected [])
leftovers after bc: as expected
```

## STEP 5: M against B, result by result

```sh
. /var/log/zts-l2c-1028/env.sh
{
for l in ml mc bl bc; do
	sed -E 's|^.*/tests/functional/||; s| [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\]$| \1|' \
	    $L/$l-tests.txt | LC_ALL=C sort > $L/$l-results.txt
done
for p in l:l2arc c:cache; do
	k=${p%%:*}
	LC_ALL=C join -a 1 -a 2 -e none -o 0,1.2,2.2 $L/m$k-results.txt $L/b$k-results.txt |
	    awk '{ printf "%-36s M %-7s B %-7s %s\n", $1, $2, $3, ($2 == $3 ? "same" : "DIFFERS") }'
done > $L/compare-rows.txt
cat $L/compare-rows.txt
echo "results: $(wc -l < $L/compare-rows.txt), the same on M and B: $(grep -c ' same$' $L/compare-rows.txt)," \
    "different: $(grep -c ' DIFFERS$' $L/compare-rows.txt)"
for l in ml mc bl bc; do
	echo "$l left: [$(LC_ALL=C ls -A $W/files-$l | xargs)]"
done
} 2>&1 | tee $L/compare.txt
```

Expected: 28 rows, `l2arc/setup`, the 12 l2arc tests and `l2arc/cleanup`,
then `cache/setup`, the 12 cache tests and `cache/cleanup`, every one
`M PASS    B PASS    same`; then `results: 28, the same on M and B: 28,
different: 0`, and

```
ml left: [testdir]
mc left: [testdir]
bl left: []
bc left: []
```

A row that is not PASS but the same on both trees is recorded: it cannot come
from the branch (see the top of this document) and is master's or this
VM's. A `DIFFERS` row is reported with both runs' `not passed:` lines; since
the two states differ only in `cleanup.ksh`, which runs after every test, a
test that differs is a flaky test, and a `cleanup` that differs is a finding.

## STEP 6: unload, release, report, cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the whole task's kernel check and the
unload; the module stays loaded on `HELD` or `PANIC`:

```sh
. /var/log/zts-l2c-1028/env.sh
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $L/kern-all.txt | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $L/kern-all.txt) ZTS marks"
grep -iE "$KRE" $L/kern-all.txt | grep -vE "$KIGN" | head -n 40
echo "kernel, this boot: $(sudo -n journalctl -k -b --no-pager -q | grep -iE "$KRE" | grep -vcE "$KIGN")"
if [ -e $L/HELD ] || [ -e $L/PANIC ]; then
	echo "module left loaded: $(cd $L && ls HELD PANIC 2>/dev/null | xargs)"
elif [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$SV" ]; then
	p=$(sudo -n $W/T/zpool list -H -o name 2>&1)
	if [ -n "$p$(grep -E ' zfs |testpool' /proc/mounts)" ]; then
		echo "a pool or a mount is left ($p): module left loaded"
	else
		(cd $W/T && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u); echo "unload rc=$?"
	fi
else
	echo "no module of this tree is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
echo "loop devices: [$(loops)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings, 56 ZTS marks` (14 per run; fewer
after a stop), no kernel line, the boot's count equal to the STEP 0
baseline, `unload rc=0`, `lsmod: []`, `loop devices: []`, a 0-byte or absent
`zpool.cache`, no `vdev_id.conf`.

Then release the two `testdir` directories the M runs left, and only them
(each still exactly an empty directory of root's), then the four files
directories:

```sh
. /var/log/zts-l2c-1028/env.sh
{
for d in ml mc; do
	f=$W/files-$d
	if [ "$(fd_state $f)" = testdir ]; then
		sudo -n rmdir $f/testdir; echo "released $f/testdir rc=$?"
	else
		echo "$f holds [$(LC_ALL=C ls -A $f | xargs)]: nothing released"
	fi
done
for d in ml mc bl bc; do
	rmdir $W/files-$d; echo "rmdir files-$d rc=$?"
done
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $L/end.txt
```

Expected: `released /var/tmp/zts-l2c-1028/files-ml/testdir rc=0`, the same
for `files-mc`, four `rmdir files-.. rc=0`, `W holds: T repo`, the free space.
After a stop, a files directory that is not empty stays (`rmdir` fails on
it): report it, remove nothing else by hand.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-l2c-1028/env.sh
{
echo "VM 107: the testdir the l2arc and cache groups leave, master a128b8a075 against 5e95479b7e (s/VM-TESTS-107-l2c.md)"
for s in record fix build load runs swap compare end; do
	echo
	echo "== $s.txt"
	cat $L/$s.txt 2>&1
done
echo
echo "== summary"
for l in ml mc bl bc; do
	grep -E "^$l -T |^kernel $l:|^leftovers after $l:|^  /var/tmp/zts-l2c-1028/files-$l holds:" $L/runs.txt
done
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, `uname -r`, the
free space before and after the release, the release lines of
`/var/tmp/vm-task-107n`, the doc checkout line, the kernel baseline and the
`fio` line), a `verdict:` line naming the case of "What the results mean"
that applies, and a `deviations:` line with every deviation from these steps
and its reason (`none` if there was none). Then
`cat /var/log/zts-l2c-1028/REPORT.txt`. Expected summary:

```
ml -T l2arc PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
kernel ml: 0 findings, 14 ZTS marks (expected 14)
leftovers after ml: as expected
  /var/tmp/zts-l2c-1028/files-ml holds: [testdir] (expected [testdir])
mc -T cache PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
kernel mc: 0 findings, 14 ZTS marks (expected 14)
leftovers after mc: as expected
  /var/tmp/zts-l2c-1028/files-mc holds: [testdir] (expected [testdir])
bl -T l2arc PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
kernel bl: 0 findings, 14 ZTS marks (expected 14)
leftovers after bl: as expected
  /var/tmp/zts-l2c-1028/files-bl holds: [] (expected [])
bc -T cache PASS x14 (Test: x14, Test (Linux): x0) rc=0 wall=<s>s
kernel bc: 0 findings, 14 ZTS marks (expected 14)
leftovers after bc: as expected
  /var/tmp/zts-l2c-1028/files-bc holds: [] (expected [])
flags: STOP [] HELD [] PANIC []
verdict: as expected (first case)
deviations: none
```

Then, as the very last command, and only if the unload block printed
`unload rc=0` with `lsmod: []` and no step stopped, remove this task's
directories and the doc checkout (the doc checkout only if it is a clean
clone of `vm-tasks-1026` holding this document, nothing else). `$L` sits in
root's `/var/log`: its contents go with `rm -rf`, the then empty directory
with `sudo -n rmdir`:

```sh
cd /
D=/var/tmp/vm-task-107s
if [ -d $D ] && [ ! -L $D ] && [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
    [ "$(git -C $D rev-parse --abbrev-ref HEAD 2>/dev/null)" = vm-tasks-1026 ] &&
    case "$(git -C $D remote get-url origin 2>/dev/null)" in
    https://github.com/tigerblue77/zfs|https://github.com/tigerblue77/zfs.git) true ;;
    *) false ;; esac &&
    git -C $D ls-files --error-unmatch s/VM-TESTS-107-l2c.md > /dev/null 2>&1 &&
    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
    ! grep -q " ${D}[ /]" /proc/mounts; then
	X=$D
else
	X=""; echo "$D kept: not a clean checkout of vm-tasks-1026"
fi
rm -rf /var/tmp/zts-l2c-1028 $X /var/log/zts-l2c-1028/*; echo "rm rc=$?"
sudo -n rmdir /var/log/zts-l2c-1028; echo "rmdir rc=$?"
ls -d /var/tmp/zts-l2c-1028 /var/log/zts-l2c-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, or a step stopped, run nothing of this block:
keep everything and say so. If your permission system refuses this `rm -rf`
or this `rmdir`, leave what is left and say so; do not delete it any other
way. Then reply with all of `REPORT.txt` as printed, and the result of the
last block: nothing runs after the reply.

## What the results mean

- **As expected** (every run passes, `ml` and `mc` each leave exactly the
  empty, root-owned `testdir` and nothing else, `bl` and `bc` leave nothing,
  `compare.txt` 28 of 28 the same, no kernel finding): on a kernel, master's
  l2arc and cache groups leak `$TESTDIR`, and with the branch (all seven
  commits, of which only `b0324fb5ca` and `5e95479b7e` reach these groups) they
  leave nothing, with every test result unchanged.
- **`bl` or `bc` leaves `testdir`, or something else**: the branch's
  `cleanup.ksh` does not remove it; check that STEP 4's `ok: T: the seven
  files` held, and report the `ls -la` lines and the `cleanup:` line.
- **`ml` or `mc` leaves nothing**: the leak is not master's as described, or
  the tree was not master's; report the `cleanup:` line and STEP 3's check of
  the two cleanup blobs.
- **`ml` or `mc` leaves something besides `testdir`**: another leak of those
  groups, which reading the code did not find; the `ls -la` lines say what
  and when (against the `Test:` lines of `$L/<label>-tests.txt`).
- **A test fails or is skipped** on both states: master's or this VM's, the
  same with the branch; on one state only: a flaky test (the branch's change
  runs after every test), reported with both runs' lines.
- **`LEFTOVER tunables`**: a test of the group did not restore a tunable;
  report the line.
- **Any kernel finding**: the line and the run are the result. Stop.

Estimated duration: about 1 hour 30 minutes (checks, release and clone 10,
the build 15, `ml` and `bl` 10 to 25 each, `mc` and `bc` 5 to 15 each, the end
5).
