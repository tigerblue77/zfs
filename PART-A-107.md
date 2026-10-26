# VM 107, part A: sources and builds for ddt-prune-extended (no module loaded)

These instructions are for the Claude session on VM 107 (Debian 13, kernel
6.12). They prepare a failing-first kernel run of the ZTS test
`functional/dedup/dedup_prune_leak` of the fork branch
`tigerblue77/zfs:ddt-prune-extended`, the head of the open upstream pull
request openzfs/zfs#19549 ("ddt: do not prune an entry that a copies=2 write
may have extended", Closes #19443). Its description says the new test case
"was not run in ZTS without the fix": this task runs it with and without.

This is **part A**: fetch, two worktrees, the test file copied into the master
tree, two builds, and a record of what was built. It loads no module and runs
no `zpool`, `zfs` or test command. **Part B** (module loads with srcversion
guards, the runs, the kernel checks, cleanup and `REPORT.txt`) verifies the
trees part A leaves instead of rebuilding them; it may come in this document
or later, in its own message.

| Tree | Code | What part B expects of it |
| --- | --- | --- |
| `B` = `/var/tmp/zts-ddtprune-1026d/B` | head `983709b6a2` "ddt: do not prune an entry that a copies=2 write may have extended": `module/zfs/ddt.c` (the fix), `man/man8/zpool-ddtprune.8`, and the test | `dedup_prune_leak` PASS 3/3 |
| `M` = `/var/tmp/zts-ddtprune-1026d/M` | the head's parent `bc3fe48662`, upstream master and the branch's merge base, + the head's `dedup_prune_leak.ksh` (no code change; the test is already registered) | `dedup_prune_leak` FAIL 3/3, at the new case's leak check |

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never reboot.
- Work only under `/var/tmp/zts-ddtprune-1026d`, log to
  `/var/log/zts-ddtprune-1026d`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`) or anything else this task did not create.
- `/etc/zfs/zpool.cache` may exist as a 0-byte file left by earlier tasks.
  Leave it alone: never delete, edit or rewrite it. Record its size.
- Part A loads no kernel module and runs no `zpool`, `zfs`, `zdb` or
  `zfs-tests.sh` command: libzfs would `modprobe` whatever zfs module the
  system has.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more. Fill no disk beyond
  the two trees.
- All git work happens in A1. From A1 on, every block starts by sourcing
  `/var/log/zts-ddtprune-1026d/env.sh`, so that it also works in a shell that
  keeps no state between commands.
- After any line that says `stop`, run nothing more: write `PART-A.txt` with
  what you have (A3) and reply with it.

## A0: the VM must be free

Run all of these and keep their output:

```sh
export PATH=/usr/sbin:/sbin:$PATH
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
ls -A /var/tmp; ls -d /var/tmp/zts-ddtprune-1026d /var/log/zts-ddtprune-1026d 2>&1
ls -l /etc/zfs/zpool.cache 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n journalctl -k -n 1 --no-pager
command -v ksh python3 git modinfo sha256sum
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
- `/var/tmp/zts-ddtprune-1026d` or `/var/log/zts-ddtprune-1026d` already
  exists;
- `/var/tmp` has less than 12 GB free (two built trees, then the test's three
  sparse 4 GiB file vdevs and the dedup group's data in part B);
- `ksh`, `python3`, `git`, `modinfo` or `sha256sum` is missing;
- `journalctl -k` prints no kernel line: part B reads the kernel messages of
  each run from the journal.

Record `uname -r` and the `/etc/zfs/zpool.cache` line: another 6.12 kernel is
not a reason to stop, and neither is that file.

## A1: environment, sources, the test file in M

`/var/log` is root's, so the log directory is made with `sudo -n` and handed
to this user. The test files directory is world-writable with the sticky bit
(mode 1777), as `zfs-tests.sh -d` wants: tests that run commands as other
users fail with a 0755 directory.

```sh
sudo -n mkdir -p /var/log/zts-ddtprune-1026d/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-ddtprune-1026d
cat > /var/log/zts-ddtprune-1026d/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-ddtprune-1026d
L=/var/log/zts-ddtprune-1026d
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The branch's head (one commit: the fix, the man page, the test) and its
# parent, which is upstream master and the merge base.
HB=983709b6a25ad6c2d495111432becad3ffd25f7b
HM=bc3fe48662e7aab8e639b328a6d37cf12b39e2b2
# The test as a file of the tree (F) and as a zfs-tests.sh -t argument (R).
# It is already in common.run and Makefile.am: only the file changes.
F=tests/zfs-tests/tests/functional/dedup/dedup_prune_leak.ksh
R=tests/functional/dedup/dedup_prune_leak.ksh

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion (which must differ
# from the other tree's: the two trees differ in module/zfs/ddt.c), and
# send its ZTS results to $L/results.
build_tree() {   # build_tree <tree>
	local t0 rc f
	cd $Z/$1 || return 1
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c \
	    './autogen.sh && ./configure --enable-debug && make -j8' \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/partA.txt
	if [ $rc -ne 0 ]; then
		grep -nE 'error:|Error [0-9]' $L/build-$1.log | head -20 | tee -a $L/partA.txt
		echo "build $1 failed: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "build $1 warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$1.log)" | tee -a $L/partA.txt
	modinfo -F srcversion module/zfs.ko > $L/srcv-$1
	for f in $L/srcv-*; do
		[ "$f" = "$L/srcv-$1" ] && continue
		cmp -s "$f" $L/srcv-$1 || continue
		echo "$1 has the srcversion of ${f##*/srcv-}: stop" | tee -a $L/partA.txt
		return 1
	done
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    tests/test-runner/bin/test-runner.py.in \
	    tests/test-runner/bin/test-runner.py
	if ! grep -qx "BASEDIR = '$L/results'" tests/test-runner/bin/test-runner.py; then
		echo "$1: BASEDIR not redirected: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "$1 built: srcversion $(cat $L/srcv-$1)" | tee -a $L/partA.txt
}

# What part B verifies before it loads anything from <tree>: its HEAD, the
# hash and list of its changes against HEAD, its module's srcversion, and
# the sha256 of its two modules and of every built file under a .libs
# directory (the libraries and programs the tests run).
record_tree() {   # record_tree <tree>
	local n
	cd $Z/$1 || return 1
	{
	echo "head $(git rev-parse HEAD)"
	echo "diff $(git diff HEAD | sha256sum | cut -d' ' -f1)"
	echo "changed $(git diff HEAD --name-only | xargs)"
	echo "srcversion $(modinfo -F srcversion module/zfs.ko)"
	} > $L/tree-$1.txt
	{ ls module/*.ko; find . -name .git -prune -o -path '*/.libs/*' -type f -print; } |
	    sort | xargs -d '\n' sha256sum > $L/bins-$1.sha256
	n=$(wc -l < $L/bins-$1.sha256)
	echo "$1 recorded: head $(git rev-parse --short=10 HEAD)," \
	    "changed: $(sed -n 's/^changed //p' $L/tree-$1.txt)," \
	    "$n files hashed, zfs.ko x$(grep -c ' module/zfs\.ko$' $L/bins-$1.sha256)," \
	    "spl.ko x$(grep -c ' module/spl\.ko$' $L/bins-$1.sha256)" | tee -a $L/partA.txt
}
ENV
. /var/log/zts-ddtprune-1026d/env.sh
mkdir -p $Z && mkdir -m 1777 $Z/files && echo "files dir mode $(stat -c %a $Z/files)"
date '+%F %T' > $L/partA-start.txt
```

Expected: `files dir mode 1777`.

Clone once, fetch upstream master to check the merge base, and make one
worktree per tree. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-ddtprune-1026d/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "ddt-prune-extended" "$(git rev-parse origin/ddt-prune-extended)" $HB
same "its parent" "$(git rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HB)" $HM
same "test files of the commit" "$(git diff --name-only $HM $HB -- tests | xargs)" "$F"
same "code files of the commit" "$(git diff --name-only $HM $HB -- . ':!tests' | xargs)" \
    "man/man8/zpool-ddtprune.8 module/zfs/ddt.c"
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
git diff --stat $HM $HB | tail -1 | tee -a $L/worktrees.txt
```

Expected, in order:

```
clone rc=0
fetch rc=0
ok: ddt-prune-extended
ok: its parent
ok: merge base with upstream master
ok: test files of the commit
ok: code files of the commit
worktree M rc=0 at bc3fe48662
worktree B rc=0 at 983709b6a2
 3 files changed, 60 insertions(+), 2 deletions(-)
```

Any `MISMATCH` (the branch was force-pushed since 26 Oct) means stop.

Copy the test into M, as the head's file:

```sh
. /var/log/zts-ddtprune-1026d/env.sh
{
cd $Z/M && git checkout $HB -- $F && echo "M: test copied"
git diff HEAD --stat | tail -1
echo "M: $(git diff $HB -- tests | wc -l) $(git diff HEAD -- module include lib cmd | wc -l)"
echo "M differs from the head in: $(git diff --name-only $HB | xargs)"
for t in M B; do
	echo "$t: test $(ls -l $Z/$t/$F | cut -c1-10)," \
	    "prunable x$(grep -c prunable $Z/$t/module/zfs/ddt.c)," \
	    "new case x$(grep -c 'leaks the copy an extended entry gained' $Z/$t/$F)"
done
} | tee $L/copy.txt
```

Expected:

```
M: test copied
 1 file changed, 37 insertions(+)
M: 0 0
M differs from the head in: man/man8/zpool-ddtprune.8 module/zfs/ddt.c
M: test -rwxr-xr-x, prunable x0, new case x1
B: test -rwxr-xr-x, prunable x2, new case x1
```

(`M: 0 0`: no line under `tests/` differs from the head, and no code changed.
`prunable` is the fix's new variable in `ddt_prune_walk()`, twice on B and
absent on M; both trees carry the new test case.) Anything else means stop.

## A2: builds and the record of what was built

```sh
. /var/log/zts-ddtprune-1026d/env.sh
build_tree B && build_tree M && record_tree B && record_tree M &&
    echo "trees built and recorded" | tee -a $L/partA.txt
```

Expected: `build B rc=0`, `build B warnings: <n>`, `B built: srcversion ...`,
`build M rc=0`, `build M warnings: <n>`, `M built: srcversion ...` (two
different srcversions), then

```
B recorded: head 983709b6a2, changed: tests/test-runner/bin/test-runner.py.in, <n> files hashed, zfs.ko x1, spl.ko x1
M recorded: head bc3fe48662, changed: tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/functional/dedup/dedup_prune_leak.ksh, <n> files hashed, zfs.ko x1, spl.ko x1
trees built and recorded
```

(The `test-runner.py.in` change is `build_tree`'s redirection of the results
directory to `$L/results`.) On a failed build, the `error:` lines go into
`PART-A.txt`; a compiler warning count other than 0 is worth quoting with its
first lines (`grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-<tree>.log`),
but is not a reason to stop.

## A3: end of part A

```sh
. /var/log/zts-ddtprune-1026d/env.sh
if ! grep -q MISMATCH $L/sha-checks.txt && [ -s $L/tree-B.txt ] && [ -s $L/tree-M.txt ] &&
    grep -qx 'trees built and recorded' $L/partA.txt; then
	date '+%F %T' > $L/PART-A.done; echo "part A done"
else
	echo "part A incomplete"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing: part A loads no module
df -h /var/tmp | tail -1
```

Write `/var/log/zts-ddtprune-1026d/PART-A.txt` with, in this order: a first
line `VM 107, part A of ddt-prune-extended (PART-A-107.md)`; A0, short
(anything found, `id -un`, `uname -r`, the distribution, `nproc`, the free
space, the `/etc/zfs/zpool.cache` line); then the contents of
`$L/sha-checks.txt`, `$L/worktrees.txt`, `$L/copy.txt` and `$L/partA.txt`;
the `part A done` (or `incomplete`) line, the `lsmod` result and the `df`
line; and anything that differed from these steps, and why. Then reply with
the whole of `PART-A.txt`.

Leave both trees, `$L` and `$Z/files` in place: part B starts from them. Do
not start part B without its text.
