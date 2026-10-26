# VM 105, part A: sources and builds for pool-feature-disable-v2 and vdev-add-event-details (no module loaded)

These instructions are for the Claude session on VM 105 (Debian 13, kernel
6.12). They prepare failing-first kernel runs of the new ZTS tests of two
branches of the fork `tigerblue77/zfs`, neither of which has run without its
branch's code:

- `pool-feature-disable-v2`, head `c62b3702d6` "Allow an inactive pool feature
  to be disabled again" (Closes openzfs/zfs#19439). Its test
  `functional/cli_root/zpool_set/zpool_set_feature_disable` was fixed and
  squashed on 26 Oct and has not run anywhere since: the fork's zfs-qemu run
  had not started by 09:50Z, so this is also its first kernel run with the
  fix.
- `vdev-add-event-details`, head `b87a733ac0` "Describe the added vdevs in the
  vdev_add event", the head of the open upstream pull request
  openzfs/zfs#19566. Its test `functional/events/vdev_add_event` passed in the
  fork's CI on this head, but the claim of the pull request's description that
  it fails on master was never run.

Both heads are one commit on the same parent, upstream master `e117a84dbb`
(their merge base with upstream master, which has one more commit since). So
one master tree serves both: `e117a84dbb` with both heads' changes under
`tests/` copied in, and no code change.

This is **part A**: fetch, three worktrees, the test files copied into the
master tree, three builds, and a record of what was built. It loads no module
and runs no `zpool`, `zfs` or test command. **Part B** (module loads with
srcversion guards, the runs, the kernel checks, cleanup and `REPORT.txt`)
verifies the trees part A leaves instead of rebuilding them; it may come in
this document or later, in its own message.

| Tree | Code | What part B expects of it |
| --- | --- | --- |
| `F` = `/var/tmp/zts-featev-1026e/F` | head `c62b3702d6` of `pool-feature-disable-v2` | `zpool_set_feature_disable` PASS 3/3 |
| `V` = `/var/tmp/zts-featev-1026e/V` | head `b87a733ac0` of `vdev-add-event-details` (= openzfs/zfs#19566) | `vdev_add_event` PASS 3/3 |
| `M` = `/var/tmp/zts-featev-1026e/M` | `e117a84dbb`, the parent of both heads, + both heads' changes under `tests/` (the two tests, `zpool_set_features.ksh`, the `common.run`, `linux.run` and `Makefile.am` lines; no code change) | `zpool_set_feature_disable` FAIL 3/3 at its first `zpool set feature@...=disabled`, which libzfs refuses; `vdev_add_event` FAIL 3/3 at its first check, the event having no vdev |

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never reboot.
- Work only under `/var/tmp/zts-featev-1026e`, log to
  `/var/log/zts-featev-1026e`. Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`) or anything else this task did not create.
- `/etc/zfs/zpool.cache` may exist as a 0-byte file left by earlier tasks.
  Leave it alone: never delete, edit or rewrite it. Record its size.
- Part A loads no kernel module and runs no `zpool`, `zfs`, `zdb`, `zed` or
  `zfs-tests.sh` command: libzfs would `modprobe` whatever zfs module the
  system has.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Leave no background loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time.
  Fill no disk beyond the three trees.
- All git work happens in A1. From A1 on, every block starts by sourcing
  `/var/log/zts-featev-1026e/env.sh`, so that it also works in a shell that
  keeps no state between commands. `env.sh` sets `umask 022`, so that files
  are created as on the build machines (`-rwxr-xr-x` for an executable, where
  this VM's umask 0002 would give `-rwxrwxr-x`).
- After any line that says `stop`, run nothing more: write `PART-A.txt` with
  what you have (A3) and reply with it.

## A0: the VM must be free

Run all of these and keep their output:

```sh
export PATH=/usr/sbin:/sbin:$PATH
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc; umask
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb'
pgrep -ax 'zed|lt-zed'
ls -A /var/tmp; ls -d /var/tmp/zts-featev-1026e /var/log/zts-featev-1026e 2>&1
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n journalctl -k -n 1 --no-pager
command -v ksh python3 git modinfo sha256sum fio
```

Stop at once, changing nothing, if:

- `id -un` is not `tigerblue77`, or `sudo -n true` fails;
- the system is not Debian 13, or the kernel is not a 6.12 one, or its headers
  (`/lib/modules/$(uname -r)/build`) are missing: do not install them;
- a zfs or spl module is loaded, or `/dev/zfs` exists;
- a test or build process runs, other than this command and the shell running
  it, or any `zed` runs (the events tests start their own and refuse to run
  beside another);
- `/etc/zfs/vdev_id.conf` exists: the events tests' setup refuses to run with
  it, since it makes its own link there for the length of a run and removes
  it;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `/var/tmp/zts-featev-1026e` or `/var/log/zts-featev-1026e` already exists;
- `/var/tmp` has less than 12 GB free (three built trees of about 1 GB each,
  then the tests' file vdevs and pool images in part B);
- `ksh`, `python3`, `git`, `modinfo` or `sha256sum` is missing;
- `journalctl -k` prints no kernel line: part B reads the kernel messages of
  each run from the journal.

Record `uname -r`, the `umask`, the `/etc/zfs/zpool.cache` line and the path of
`fio` (or its absence): another 6.12 kernel is not a reason to stop, and
neither is that file, nor a missing `fio` (one test of the `zpool_set` group
then skips itself).

## A1: environment, sources, the test files in M

`/var/log` is root's, so the log directory is made with `sudo -n` and handed
to this user. The test files directory is world-writable with the sticky bit
(mode 1777), as `zfs-tests.sh -d` wants: tests that run commands as other
users fail with a 0755 directory. `$Z/run` holds the pid file of the `zed`
that the events tests start (the builds point `--runstatedir` there, so that
it does not create `/usr/local/var/run`); it is made here so that it stays
this user's.

```sh
sudo -n mkdir -p /var/log/zts-featev-1026e/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-featev-1026e
cat > /var/log/zts-featev-1026e/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-featev-1026e
L=/var/log/zts-featev-1026e
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The two heads (one commit each) and their common parent, which is their
# merge base with upstream master.
HF=c62b3702d606b83cf6821cededa22fca6366db0e
HV=b87a733ac05e96d950c5bae6bc0b94235888af3b
BASE=e117a84dbbfe0befe15110b9d1d799f47b1a5546
# Each new test as a file of the tree (F*) and as a zfs-tests.sh -t argument
# (R*): FF for pool-feature-disable-v2, FV for vdev-add-event-details.
FF=tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_feature_disable.ksh
RF=tests/functional/cli_root/zpool_set/zpool_set_feature_disable.ksh
FV=tests/zfs-tests/tests/functional/events/vdev_add_event.ksh
RV=tests/functional/events/vdev_add_event.ksh
# The other files each head changes under tests/; both change MK.
FS=tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_features.ksh
MK=tests/zfs-tests/tests/Makefile.am
TF="tests/runfiles/common.run $MK $FF $FS"
TV="tests/runfiles/linux.run $MK $FV"

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, record its module's srcversion (which must differ
# from the other trees': each pair differs in module/zfs), check that its
# zed keeps its pid file in $Z/run, and send its ZTS results to $L/results.
build_tree() {   # build_tree <tree>
	local t0 rc f
	cd $Z/$1 || return 1
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug --runstatedir=$Z/run && make -j8" \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/partA.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/partA.txt
		echo "build $1 failed: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "build $1 warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$1.log)" | tee -a $L/partA.txt
	if ! grep -aq "$Z/run/zed.pid" .libs/zed; then
		echo "$1: zed does not keep its pid file in $Z/run: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "$1 zed: pid file $Z/run/zed.pid" | tee -a $L/partA.txt
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
# directory (the libraries and programs the tests run, zed included).
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
	    "zed x$(grep -c ' \./\.libs/zed$' $L/bins-$1.sha256)" |
	    tee -a $L/partA.txt
}
ENV
. /var/log/zts-featev-1026e/env.sh
mkdir -p $Z && mkdir -m 1777 $Z/files && mkdir -m 755 $Z/run &&
    echo "files dir mode $(stat -c %a $Z/files), run dir mode $(stat -c %a $Z/run)"
date '+%F %T' > $L/partA-start.txt
```

Expected: `files dir mode 1777, run dir mode 755`.

Clone once, fetch upstream master to check the merge base, read the head of
the upstream pull request, and make one worktree per tree. This is read-only
towards GitHub: nothing is pushed.

```sh
. /var/log/zts-featev-1026e/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "pool-feature-disable-v2" "$(git rev-parse origin/pool-feature-disable-v2)" $HF
same "vdev-add-event-details" "$(git rev-parse origin/vdev-add-event-details)" $HV
same "openzfs/zfs#19566's head" "$(git ls-remote $UPSTREAM refs/pull/19566/head | cut -f1)" $HV
same "F's parent" "$(git rev-parse $HF^)" $BASE
same "V's parent" "$(git rev-parse $HV^)" $BASE
same "F's merge base with upstream master" "$(git merge-base FETCH_HEAD $HF)" $BASE
same "V's merge base with upstream master" "$(git merge-base FETCH_HEAD $HV)" $BASE
same "F's files under tests/" "$(git diff --name-only $BASE $HF -- tests | xargs)" "$TF"
same "F's code files" "$(git diff --name-only $BASE $HF -- . ':!tests' | xargs)" \
    "include/sys/fs/zfs.h include/sys/spa_impl.h include/sys/zfeature.h lib/libzfs/libzfs_pool.c man/man7/zpool-features.7 man/man7/zpoolprops.7 module/zfs/spa.c module/zfs/spa_misc.c module/zfs/vdev_indirect.c module/zfs/zfeature.c module/zfs/zfs_vnops.c module/zfs/zvol.c"
same "V's files under tests/" "$(git diff --name-only $BASE $HV -- tests | xargs)" "$TV"
same "V's code files" "$(git diff --name-only $BASE $HV -- . ':!tests' | xargs)" \
    "man/man8/zpool-events.8 module/zfs/spa.c"
} | tee $L/sha-checks.txt
for t in M:$BASE F:$HF V:$HV; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
for h in $HF $HV; do
	echo "$(git log -1 --abbrev=10 --format='%h %s' $h):$(git diff --stat $BASE $h | tail -1)"
done | tee -a $L/worktrees.txt
git log -1 --abbrev=10 --format='parent %h %s' $BASE | tee -a $L/worktrees.txt
```

Expected, in order:

```
clone rc=0
fetch rc=0
ok: pool-feature-disable-v2
ok: vdev-add-event-details
ok: openzfs/zfs#19566's head
ok: F's parent
ok: V's parent
ok: F's merge base with upstream master
ok: V's merge base with upstream master
ok: F's files under tests/
ok: F's code files
ok: V's files under tests/
ok: V's code files
worktree M rc=0 at e117a84dbb
worktree F rc=0 at c62b3702d6
worktree V rc=0 at b87a733ac0
c62b3702d6 Allow an inactive pool feature to be disabled again: 16 files changed, 376 insertions(+), 27 deletions(-)
b87a733ac0 Describe the added vdevs in the vdev_add event: 5 files changed, 153 insertions(+), 4 deletions(-)
parent e117a84dbb libzutil: keep the latest label when two vdevs claim one id
```

Any `MISMATCH` (a branch was force-pushed or rebased since 26 Oct, or the
upstream pull request moved) means stop.

Copy both heads' changes under `tests/` into M, as the heads' own diffs
applied onto their parent (`--index` stages them, so that `git diff HEAD`
shows the new files too):

```sh
. /var/log/zts-featev-1026e/env.sh
{
cd $Z/M && git diff $BASE $HF -- tests | git apply --index &&
    git diff $BASE $HV -- tests | git apply --index && echo "M: tests copied"
git diff HEAD --stat | tail -1
same "M: changes outside tests/" "$(git diff HEAD --name-only -- . ':!tests' | wc -l)" 0
for f in tests/runfiles/common.run $FF $FS; do
	same "M: $f as in F" "$(git diff $HF -- $f | wc -l)" 0
done
for f in tests/runfiles/linux.run $FV; do
	same "M: $f as in V" "$(git diff $HV -- $f | wc -l)" 0
done
same "M: Makefile.am against F" \
    "$(git diff $HF -- $MK | grep -E '^[-+]' | grep -vE '^(---|\+\+\+) ' | tr -d '\t\\' | xargs)" \
    "+functional/events/vdev_add_event.ksh"
same "M: Makefile.am against V" \
    "$(git diff $HV -- $MK | grep -E '^[-+]' | grep -vE '^(---|\+\+\+) ' | tr -d '\t\\' | xargs)" \
    "+functional/cli_root/zpool_set/zpool_set_feature_disable.ksh"
for t in M F V; do
	cd $Z/$t
	echo "$t: tests $([ -e $FF ] && ls -l $FF | cut -c1-10 || echo none)" \
	    "$([ -e $FV ] && ls -l $FV | cut -c1-10 || echo none)," \
	    "registered x$(grep -c "'zpool_set_feature_disable'" tests/runfiles/common.run)" \
	    "x$(grep -c "'vdev_add_event'" tests/runfiles/linux.run);" \
	    "code: spa_feature_disable x$(grep -c '^spa_feature_disable(' module/zfs/zfeature.c)," \
	    "ZPOOL_FEATURE_DISABLE in libzfs x$(grep -c ZPOOL_FEATURE_DISABLE lib/libzfs/libzfs_pool.c)," \
	    "event per added vdev x$(grep -c 'spa_event_create(spa, tvd, NULL, ESC_ZFS_VDEV_ADD)' module/zfs/spa.c)"
done
} | tee $L/copy.txt
```

Expected:

```
M: tests copied
 6 files changed, 291 insertions(+), 6 deletions(-)
ok: M: changes outside tests/
ok: M: tests/runfiles/common.run as in F
ok: M: tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_feature_disable.ksh as in F
ok: M: tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_features.ksh as in F
ok: M: tests/runfiles/linux.run as in V
ok: M: tests/zfs-tests/tests/functional/events/vdev_add_event.ksh as in V
ok: M: Makefile.am against F
ok: M: Makefile.am against V
M: tests -rwxr-xr-x -rwxr-xr-x, registered x1 x1; code: spa_feature_disable x0, ZPOOL_FEATURE_DISABLE in libzfs x0, event per added vdev x0
F: tests -rwxr-xr-x none, registered x1 x0; code: spa_feature_disable x1, ZPOOL_FEATURE_DISABLE in libzfs x1, event per added vdev x0
V: tests none -rwxr-xr-x, registered x0 x1; code: spa_feature_disable x0, ZPOOL_FEATURE_DISABLE in libzfs x0, event per added vdev x1
```

(M registers and carries both tests and none of the code; F and V each carry
their own test and code only.) Anything else means stop.

## A2: builds and the record of what was built

```sh
. /var/log/zts-featev-1026e/env.sh
build_tree F && build_tree V && build_tree M &&
    record_tree F && record_tree V && record_tree M &&
    echo "trees built and recorded" | tee -a $L/partA.txt
```

Expected, for F, then V, then M: `build <tree> rc=0 wall=<s>s`,
`build <tree> warnings: <n>`, `<tree> zed: pid file
/var/tmp/zts-featev-1026e/run/zed.pid`, `<tree> built: srcversion ...` (three
different srcversions), then

```
F recorded: head c62b3702d6, changed: tests/test-runner/bin/test-runner.py.in, <n> files hashed, zfs.ko x1, spl.ko x1, zed x1
V recorded: head b87a733ac0, changed: tests/test-runner/bin/test-runner.py.in, <n> files hashed, zfs.ko x1, spl.ko x1, zed x1
M recorded: head e117a84dbb, changed: tests/runfiles/common.run tests/runfiles/linux.run tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_feature_disable.ksh tests/zfs-tests/tests/functional/cli_root/zpool_set/zpool_set_features.ksh tests/zfs-tests/tests/functional/events/vdev_add_event.ksh, <n> files hashed, zfs.ko x1, spl.ko x1, zed x1
trees built and recorded
```

(The `test-runner.py.in` change is `build_tree`'s redirection of the results
directory to `$L/results`.) On a
failed build, the `error:` lines go into `PART-A.txt`; a compiler warning count
other than 0 is worth quoting with its first lines
(`grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-<tree>.log`), but is not a
reason to stop.

## A3: end of part A

```sh
. /var/log/zts-featev-1026e/env.sh
if ! grep -q MISMATCH $L/sha-checks.txt $L/copy.txt && [ -s $L/tree-F.txt ] &&
    [ -s $L/tree-V.txt ] && [ -s $L/tree-M.txt ] &&
    grep -qx 'trees built and recorded' $L/partA.txt; then
	date '+%F %T' > $L/PART-A.done; echo "part A done"
else
	echo "part A incomplete"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing: part A loads no module
pgrep -ax 'zed|lt-zed'                 # must print nothing
df -h /var/tmp | tail -1
```

Write `/var/log/zts-featev-1026e/PART-A.txt` with, in this order: a first line
`VM 105, part A of pool-feature-disable-v2 and vdev-add-event-details
(PART-A-105.md)`; A0, short (anything found, `id -un`, `uname -r`, the
distribution, `nproc`, the `umask`, the free space, the `/etc/zfs` line, the
`fio` path or its absence); then the contents of `$L/sha-checks.txt`,
`$L/worktrees.txt`, `$L/copy.txt` and `$L/partA.txt`; the `part A done` (or
`incomplete`) line, the `lsmod` and `pgrep` results and the `df` line; and
anything that differed from these steps, and why. Then reply with the whole of
`PART-A.txt`.

Leave the three trees, `$L`, `$Z/files` and `$Z/run` in place: part B starts
from them. Do not start part B without its text.
