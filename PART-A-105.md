# VM 105, part A: sources and builds for vdev-prop-set-validate-first (no module loaded)

These instructions are for the Claude session on VM 105 (Debian 13, kernel
6.12). They prepare a failing-first kernel run of the new ZTS test
`functional/cli_root/zpool_set/vdev_set_validate` of the fork branch
`tigerblue77/zfs:vdev-prop-set-validate-first`, whose top commit "Check every
vdev property before applying any" fixes openzfs/zfs#19474: a vdev property
call refused on one property leaves the properties applied before it in
memory but not on disk. That test has never run without the fix.

The branch has three commits on upstream master `0d4bb39c2c`: the two of the
open upstream pull request openzfs/zfs#19388 ("Return allocation_classes to
enabled when no class vdev is left", "Keep allocation_classes active while a
bias is special on disk"), which add the `vdev_set_props` test helper the new
test uses, and the fix on top. So the master tree here is the fix's parent,
`2aaff84e21` (#19388's two commits on master), with the head's test files
copied in.

This is **part A**: fetch, two worktrees, the test files copied into the
master tree, two builds, and a record of what was built. It loads no module
and runs no `zpool`, `zfs` or test command. **Part B** (module loads with
srcversion guards, the runs, the kernel checks, cleanup and `REPORT.txt`)
verifies the trees part A leaves instead of rebuilding them; it may come in
this document or later, in its own message.

| Tree | Code | What part B expects of it |
| --- | --- | --- |
| `B` = `/var/tmp/zts-vdevprop-1026d/B` | head `e42b092bc2` "Check every vdev property before applying any": `module/zfs/vdev.c` (the fix), the test and its two registration lines | `vdev_set_validate` PASS 3/3 |
| `M` = `/var/tmp/zts-vdevprop-1026d/M` | the head's parent `2aaff84e21` (#19388's two commits on master `0d4bb39c2c`) + the head's test file and its `common.run` and `Makefile.am` lines (no code change) | `vdev_set_validate` FAIL 3/3, at its first `alloc_bias` check |

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never reboot.
- Work only under `/var/tmp/zts-vdevprop-1026d`, log to
  `/var/log/zts-vdevprop-1026d`. Never read, write or delete another task's
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
  `/var/log/zts-vdevprop-1026d/env.sh`, so that it also works in a shell that
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
ls -A /var/tmp; ls -d /var/tmp/zts-vdevprop-1026d /var/log/zts-vdevprop-1026d 2>&1
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
- `/var/tmp/zts-vdevprop-1026d` or `/var/log/zts-vdevprop-1026d` already
  exists;
- `/var/tmp` has less than 12 GB free (two built trees, then the tests' file
  vdevs in part B);
- `ksh`, `python3`, `git`, `modinfo` or `sha256sum` is missing;
- `journalctl -k` prints no kernel line: part B reads the kernel messages of
  each run from the journal.

Record `uname -r` and the `/etc/zfs/zpool.cache` line: another 6.12 kernel is
not a reason to stop, and neither is that file.

## A1: environment, sources, the test files in M

`/var/log` is root's, so the log directory is made with `sudo -n` and handed
to this user. The test files directory is world-writable with the sticky bit
(mode 1777), as `zfs-tests.sh -d` wants: tests that run commands as other
users fail with a 0755 directory.

```sh
sudo -n mkdir -p /var/log/zts-vdevprop-1026d/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-vdevprop-1026d
cat > /var/log/zts-vdevprop-1026d/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-vdevprop-1026d
L=/var/log/zts-vdevprop-1026d
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The branch's head (the fix and its test), its parent (the two commits of
# #19388 on master, which add the vdev_set_props helper) and the merge base
# with upstream master.
HB=e42b092bc22abc6bcbf34255c99d868121d94e03
HM=2aaff84e217b66174ebbf37abfdd688515223568
BASE=0d4bb39c2c389f5a9f20b517ec77ed31578a0edd
# The test as a file of the tree (F) and as a zfs-tests.sh -t argument (R),
# and the two files that register it.
F=tests/zfs-tests/tests/functional/cli_root/zpool_set/vdev_set_validate.ksh
R=tests/functional/cli_root/zpool_set/vdev_set_validate.ksh
WIRING="tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am"

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion (which must differ
# from the other tree's: the two trees differ in module/zfs/vdev.c), check
# that the test helper is the tree's own, and send its ZTS results to
# $L/results.
build_tree() {   # build_tree <tree>
	local t0 rc f h
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
	h=$(readlink -f tests/zfs-tests/bin/vdev_set_props)
	echo "$1 helper: tests/zfs-tests/bin/vdev_set_props -> $h" | tee -a $L/partA.txt
	if [ "$h" != "$Z/$1/tests/zfs-tests/cmd/vdev_set_props" ] || [ ! -x "$h" ]; then
		echo "$1: the test helper is not this tree's: stop" | tee -a $L/partA.txt
		return 1
	fi
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
# directory (the libraries and programs the tests run, the helper included).
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
	    "spl.ko x$(grep -c ' module/spl\.ko$' $L/bins-$1.sha256)," \
	    "helper x$(grep -c ' \./tests/zfs-tests/cmd/\.libs/vdev_set_props$' $L/bins-$1.sha256)" |
	    tee -a $L/partA.txt
}
ENV
. /var/log/zts-vdevprop-1026d/env.sh
mkdir -p $Z && mkdir -m 1777 $Z/files && echo "files dir mode $(stat -c %a $Z/files)"
date '+%F %T' > $L/partA-start.txt
```

Expected: `files dir mode 1777`.

Clone once, fetch upstream master to check the merge base, and make one
worktree per tree. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-vdevprop-1026d/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "vdev-prop-set-validate-first" "$(git rev-parse origin/vdev-prop-set-validate-first)" $HB
same "its parent" "$(git rev-parse $HB^)" $HM
same "two commits below the parent" "$(git rev-parse $HM~2)" $BASE
same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HB)" $BASE
same "test files of the fix" "$(git diff --name-only $HM $HB -- tests | xargs)" "$WIRING $F"
same "code files of the fix" "$(git diff --name-only $HM $HB -- . ':!tests' | xargs)" \
    "module/zfs/vdev.c"
same "helper in the parent" "$(git ls-tree --name-only $HM tests/zfs-tests/cmd/vdev_set_props.c)" \
    "tests/zfs-tests/cmd/vdev_set_props.c"
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
git diff --stat $HM $HB | tail -1 | tee -a $L/worktrees.txt
git log --abbrev=10 --format='%h %s' $BASE..$HM | tee -a $L/worktrees.txt
```

Expected, in order:

```
clone rc=0
fetch rc=0
ok: vdev-prop-set-validate-first
ok: its parent
ok: two commits below the parent
ok: merge base with upstream master
ok: test files of the fix
ok: code files of the fix
ok: helper in the parent
worktree M rc=0 at 2aaff84e21
worktree B rc=0 at e42b092bc2
 4 files changed, 108 insertions(+), 15 deletions(-)
2aaff84e21 Keep allocation_classes active while a bias is special on disk
b5d40bc328 Return allocation_classes to enabled when no class vdev is left
```

Any `MISMATCH` (the branch was force-pushed or rebased since 26 Oct) means
stop.

Copy the test into M: the test file from the head, and the head's own diff of
the two registration files, applied onto the parent:

```sh
. /var/log/zts-vdevprop-1026d/env.sh
{
cd $Z/M && git checkout $HB -- $F && git diff $HM $HB -- $WIRING | git apply &&
    echo "M: test copied"
git diff HEAD --stat | tail -1
echo "M: $(git diff $HB -- tests | wc -l) $(git diff HEAD -- module include lib cmd | wc -l)"
echo "M differs from the head in: $(git diff --name-only $HB | xargs)"
for t in M B; do
	echo "$t: test $(ls -l $Z/$t/$F | cut -c1-10)," \
	    "two passes x$(grep -c 'boolean_t apply = B_FALSE' $Z/$t/module/zfs/vdev.c)," \
	    "runfile x$(grep -c "'vdev_set_validate'" $Z/$t/tests/runfiles/common.run)," \
	    "helper in commands.cfg x$(grep -c vdev_set_props $Z/$t/tests/zfs-tests/include/commands.cfg)"
done
} | tee $L/copy.txt
```

Expected:

```
M: test copied
 3 files changed, 69 insertions(+), 1 deletion(-)
M: 0 0
M differs from the head in: module/zfs/vdev.c
M: test -rwxr-xr-x, two passes x0, runfile x1, helper in commands.cfg x1
B: test -rwxr-xr-x, two passes x1, runfile x1, helper in commands.cfg x1
```

(`M: 0 0`: no line under `tests/` differs from the head, and no code changed.
`boolean_t apply = B_FALSE` starts the fix's two passes in `vdev_prop_set()`:
on B only. Both trees register the test and list the helper.) Anything else
means stop.

## A2: builds and the record of what was built

```sh
. /var/log/zts-vdevprop-1026d/env.sh
build_tree B && build_tree M && record_tree B && record_tree M &&
    echo "trees built and recorded" | tee -a $L/partA.txt
```

Expected: `build B rc=0`, `build B warnings: <n>`,
`B helper: tests/zfs-tests/bin/vdev_set_props -> /var/tmp/zts-vdevprop-1026d/B/tests/zfs-tests/cmd/vdev_set_props`,
`B built: srcversion ...`, the same three lines for M (two different
srcversions), then

```
B recorded: head e42b092bc2, changed: tests/test-runner/bin/test-runner.py.in, <n> files hashed, zfs.ko x1, spl.ko x1, helper x1
M recorded: head 2aaff84e21, changed: tests/runfiles/common.run tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/cli_root/zpool_set/vdev_set_validate.ksh, <n> files hashed, zfs.ko x1, spl.ko x1, helper x1
trees built and recorded
```

(The `test-runner.py.in` change is `build_tree`'s redirection of the results
directory to `$L/results`.) On a failed build, the `error:` lines go into
`PART-A.txt`; a compiler warning count other than 0 is worth quoting with its
first lines (`grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-<tree>.log`),
but is not a reason to stop.

## A3: end of part A

```sh
. /var/log/zts-vdevprop-1026d/env.sh
if ! grep -q MISMATCH $L/sha-checks.txt && [ -s $L/tree-B.txt ] && [ -s $L/tree-M.txt ] &&
    grep -qx 'trees built and recorded' $L/partA.txt; then
	date '+%F %T' > $L/PART-A.done; echo "part A done"
else
	echo "part A incomplete"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing: part A loads no module
df -h /var/tmp | tail -1
```

Write `/var/log/zts-vdevprop-1026d/PART-A.txt` with, in this order: a first
line `VM 105, part A of vdev-prop-set-validate-first (PART-A-105.md)`; A0,
short (anything found, `id -un`, `uname -r`, the distribution, `nproc`, the
free space, the `/etc/zfs/zpool.cache` line); then the contents of
`$L/sha-checks.txt`, `$L/worktrees.txt`, `$L/copy.txt` and `$L/partA.txt`;
the `part A done` (or `incomplete`) line, the `lsmod` result and the `df`
line; and anything that differed from these steps, and why. Then reply with
the whole of `PART-A.txt`.

Leave both trees, `$L` and `$Z/files` in place: part B starts from them. Do
not start part B without its text.
