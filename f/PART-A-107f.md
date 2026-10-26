# VM 107, part A: sources and builds for dedup-default-blake3 (no module loaded)

These instructions are for the Claude session on VM 107 (Debian 13, kernel
6.12). They prepare a failing-first kernel run of the new ZTS test
`functional/dedup/dedup_default_checksum` of the fork branch
`tigerblue77/zfs:dedup-default-blake3`, head `1db884e834` "Default to BLAKE3
for dedup=on on new pools": `dedup=on` (and `dedup=verify`) use BLAKE3 on a
pool created with `feature@blake3` enabled, recorded at creation as
`org.openzfs:dedup_checksum` in the pool directory of the MOS; existing pools,
also once the feature is enabled later, keep SHA-256. The head is one commit
on upstream master `a128b8a075`. No kernel has run it yet: the fork's ZTS jobs
on this head were still queued on 26 Oct, and the branch's own checks were
userspace only (libzpool, ztest).

This is **part A**: fetch, two worktrees, the commit's test changes copied into
the master tree, two builds, and a record of what was built. It loads no
module and runs no `zpool`, `zfs` or test command. **Part B** (module loads
with srcversion guards, the runs, the kernel checks, cleanup and `REPORT.txt`)
verifies the trees part A leaves instead of rebuilding them; it may come in
this document or later, in its own message.

| Tree | Code | What part B expects of it |
| --- | --- | --- |
| `B` = `/var/tmp/zts-dedupb3-1026f/B` | head `1db884e834`: `module/zfs/ddt.c`, `module/zfs/spa.c`, three headers, two man pages, the new test and the nine dedup tests it moves from `dedup=on` to `dedup=sha256` | `dedup_default_checksum` PASS 3/3; `-T dedup` all PASS |
| `M` = `/var/tmp/zts-dedupb3-1026f/M` | its parent `a128b8a075`, upstream master and the merge base, + every change the head makes under `tests/` (the new test, its `common.run` and `Makefile.am` lines, the nine tests); no code change | `dedup_default_checksum` FAIL 3/3 at its first check of a new pool; `-T dedup` all PASS but that test |

Both trees are configured with `--datadir=/var/tmp/zts-dedupb3-1026f/share`,
which is where their libzfs looks for the pool compatibility files (after
`/usr/local/etc/zfs/compatibility.d`, which takes precedence when it exists).
The test's last pool is created with `-o compatibility=openzfs-2.1-linux`, and
an uninstalled tree otherwise finds no compatibility file (unless one was
installed under `/usr/local`): `zpool create` would refuse the pool with
`could not read/parse feature file(s): openzfs-2.1-linux`, and B would fail
there for want of a file, not for its code. A1 fills that directory with the
trees' own `cmd/zpool/compatibility.d`, which the commit does not change.
Nothing else in the build reads the data directory, and nothing is installed
there (`make install` is never run).

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  There is no `zts` user and none is created.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never reboot.
- Work only under `/var/tmp/zts-dedupb3-1026f`, log to
  `/var/log/zts-dedupb3-1026f`. Never read, write or delete another task's
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
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time.
  Fill no disk beyond the two trees.
- All git work happens in A1. From A1 on, every block starts by sourcing
  `/var/log/zts-dedupb3-1026f/env.sh`, so that it also works in a shell that
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
ls -A /var/tmp; ls -d /var/tmp/zts-dedupb3-1026f /var/log/zts-dedupb3-1026f 2>&1
ls -l /etc/zfs/zpool.cache 2>&1
ls -ld /usr/local/etc/zfs/compatibility.d /usr/local/share/zfs/compatibility.d 2>&1
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
- a test or build process or any `zed` runs, other than this command and the
  shell running it;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `/var/tmp/zts-dedupb3-1026f` or `/var/log/zts-dedupb3-1026f` already
  exists;
- `/var/tmp` has less than 12 GB free (two built trees, then the tests' three
  sparse 4 GiB file vdevs and the dedup group's own files in part B);
- `ksh`, `python3`, `git`, `modinfo` or `sha256sum` is missing;
- `journalctl -k` prints no kernel line: part B reads the kernel messages of
  each run from the journal.

Record `uname -r`, the `umask`, the `/etc/zfs/zpool.cache` line and the two
`compatibility.d` lines (`No such file or directory` is the expected answer
for both; an existing `/usr/local/etc/zfs/compatibility.d` is not a reason to
stop, but say so: its files take precedence over this task's). Another 6.12
kernel is not a reason to stop, and neither is the cache file.

## A1: environment, sources, the test changes in M

`/var/log` is root's, so the log directory is made with `sudo -n` and handed
to this user. The test files directory is world-writable with the sticky bit
(mode 1777), as `zfs-tests.sh -d` wants: tests that run commands as other
users fail with a 0755 directory.

```sh
sudo -n mkdir -p /var/log/zts-dedupb3-1026f/results &&
    sudo -n chown -R "$(id -un):" /var/log/zts-dedupb3-1026f
cat > /var/log/zts-dedupb3-1026f/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-dedupb3-1026f
L=/var/log/zts-dedupb3-1026f
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The branch's head (one commit) and its parent, which is upstream master
# and the merge base.
HB=1db884e83448844ca226ac8e7990c0b0fc5a4464
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
# The new test as a file of the tree (F) and as a zfs-tests.sh -t argument (R).
F=tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh
R=tests/functional/dedup/dedup_default_checksum.ksh
# The nine dedup tests the head moves from dedup=on to dedup=sha256 (D9), and
# every file it changes under tests/, in git's order (TB).
D=tests/zfs-tests/tests/functional/dedup
D9=$(for t in bclone fdt_create fdt_import fdt_pacing legacy_create \
    legacy_fdt_upgrade legacy_gang legacy_import legacy_repair; do
	echo $D/dedup_$t.ksh; done | xargs)
TB=$(echo tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am $F $D9 |
    xargs -n1 | LC_ALL=C sort | xargs)
# The data directory both trees are configured with, and the compatibility
# files their libzfs reads from it.
SHARE=$Z/share
CD=$SHARE/zfs/compatibility.d

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Build a tree, niced, check that its libzfs reads compatibility files from
# $CD, record its module's srcversion (which must differ from the other
# tree's: the two trees differ in module/zfs), and send its ZTS results to
# $L/results.
build_tree() {   # build_tree <tree>
	local t0 rc f
	cd $Z/$1 || return 1
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug --datadir=$SHARE && make -j8" \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/partA.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/partA.txt
		echo "build $1 failed: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "build $1 warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$1.log)" | tee -a $L/partA.txt
	if ! grep -aqF "$CD" .libs/libzfs.so; then
		echo "$1: libzfs does not read compatibility files from $CD: stop" | tee -a $L/partA.txt
		return 1
	fi
	echo "$1 libzfs: compatibility files from $CD" | tee -a $L/partA.txt
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
. /var/log/zts-dedupb3-1026f/env.sh
mkdir -p $Z && mkdir -m 1777 $Z/files && echo "files dir mode $(stat -c %a $Z/files)"
date '+%F %T' > $L/partA-start.txt
```

Expected: `files dir mode 1777`.

Clone once, fetch upstream master to check the merge base, and make one
worktree per tree. This is read-only towards GitHub: nothing is pushed.

```sh
. /var/log/zts-dedupb3-1026f/env.sh
git clone --no-checkout $FORK $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/repo && git fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"
{
same "dedup-default-blake3" "$(git rev-parse origin/dedup-default-blake3)" $HB
same "its parent" "$(git rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git merge-base FETCH_HEAD $HB)" $HM
same "files under tests/" "$(git diff --name-only $HM $HB -- tests | xargs)" "$TB"
same "code files" "$(git diff --name-only $HM $HB -- . ':!tests' | xargs)" \
    "include/sys/ddt.h include/sys/dmu.h include/sys/zio.h man/man7/zfsprops.7 man/man7/zpool-features.7 module/zfs/ddt.c module/zfs/spa.c"
same "compatibility files changed" "$(git diff --name-only $HM $HB -- cmd/zpool/compatibility.d | wc -l)" 0
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git worktree add --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
echo "$(git log -1 --abbrev=10 --format='%h %s' $HB):$(git diff --stat $HM $HB | tail -1)" |
    tee -a $L/worktrees.txt
git log -1 --abbrev=10 --format='parent %h %s' $HM | tee -a $L/worktrees.txt
```

Expected, in order:

```
clone rc=0
fetch rc=0
ok: dedup-default-blake3
ok: its parent
ok: merge base with upstream master
ok: files under tests/
ok: code files
ok: compatibility files changed
worktree M rc=0 at a128b8a075
worktree B rc=0 at 1db884e834
1db884e834 Default to BLAKE3 for dedup=on on new pools: 19 files changed, 225 insertions(+), 17 deletions(-)
parent a128b8a075 Linux 7.3 compat: META
```

Any `MISMATCH` (the branch was force-pushed or rebased since 26 Oct) means
stop.

Copy the head's files under `tests/` into M (`git checkout` stages them, so
that `git diff HEAD` shows the new file too), and fill `$CD` with the trees'
compatibility files:

```sh
. /var/log/zts-dedupb3-1026f/env.sh
{
cd $Z/M && git checkout $HB -- $TB && echo "M: tests copied"
git diff HEAD --stat | tail -1
same "M: changes outside tests/" "$(git diff HEAD --name-only -- . ':!tests' | wc -l)" 0
same "M: tests/ as in the head" "$(git diff $HB -- tests | wc -l)" 0
echo "M differs from the head in: $(git diff --name-only $HB | xargs)"
mkdir -p $SHARE/zfs && cp -R $Z/B/cmd/zpool/compatibility.d $SHARE/zfs/ &&
    echo "$CD: $(ls $CD | wc -l) files, openzfs-2.1-linux lists blake3 x$(grep -cx blake3 $CD/openzfs-2.1-linux)"
same "$CD as M's" "$(diff -r $CD $Z/M/cmd/zpool/compatibility.d | wc -l)" 0
same "$CD as B's" "$(diff -r $CD $Z/B/cmd/zpool/compatibility.d | wc -l)" 0
for t in M B; do
	cd $Z/$t
	echo "$t: test $(ls -l $F | cut -c1-10)," \
	    "registered x$(grep -c "'dedup_default_checksum'" tests/runfiles/common.run)" \
	    "x$(grep -c 'functional/dedup/dedup_default_checksum\.ksh' tests/zfs-tests/tests/Makefile.am)," \
	    "the nine: dedup=sha256 x$(cat $D9 | grep -c 'dedup=sha256') dedup=on x$(cat $D9 | grep -c 'dedup=on');" \
	    "code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x$(grep -c DMU_POOL_DEDUP_CHECKSUM module/zfs/ddt.c)," \
	    "ddt_create_dedup_checksum in spa.c x$(grep -c 'ddt_create_dedup_checksum(spa, tx)' module/zfs/spa.c)"
done
} | tee $L/copy.txt
```

Expected:

```
M: tests copied
 12 files changed, 115 insertions(+), 12 deletions(-)
ok: M: changes outside tests/
ok: M: tests/ as in the head
M differs from the head in: include/sys/ddt.h include/sys/dmu.h include/sys/zio.h man/man7/zfsprops.7 man/man7/zpool-features.7 module/zfs/ddt.c module/zfs/spa.c
/var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d: 25 files, openzfs-2.1-linux lists blake3 x0
ok: /var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d as M's
ok: /var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d as B's
M: test -rwxr-xr-x, registered x1 x1, the nine: dedup=sha256 x10 dedup=on x0; code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x0, ddt_create_dedup_checksum in spa.c x0
B: test -rwxr-xr-x, registered x1 x1, the nine: dedup=sha256 x10 dedup=on x0; code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x2, ddt_create_dedup_checksum in spa.c x1
```

(M carries the head's tests, byte for byte, and none of its code; on master the
nine tests have `dedup=on` ten times and `dedup=sha256` never.
`openzfs-2.1-linux` lacking blake3 is what the test's last pool relies on.)
Anything else means stop.

## A2: builds and the record of what was built

```sh
. /var/log/zts-dedupb3-1026f/env.sh
build_tree B && build_tree M && record_tree B && record_tree M &&
    echo "trees built and recorded" | tee -a $L/partA.txt
```

Expected, for B, then M: `build <tree> rc=0 wall=<s>s`,
`build <tree> warnings: <n>`, `<tree> libzfs: compatibility files from
/var/tmp/zts-dedupb3-1026f/share/zfs/compatibility.d`, `<tree> built:
srcversion ...` (two different srcversions), then

```
B recorded: head 1db884e834, changed: tests/test-runner/bin/test-runner.py.in, <n> files hashed, zfs.ko x1, spl.ko x1
M recorded: head a128b8a075, changed: tests/runfiles/common.run tests/test-runner/bin/test-runner.py.in tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/dedup/dedup_bclone.ksh tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh tests/zfs-tests/tests/functional/dedup/dedup_fdt_create.ksh tests/zfs-tests/tests/functional/dedup/dedup_fdt_import.ksh tests/zfs-tests/tests/functional/dedup/dedup_fdt_pacing.ksh tests/zfs-tests/tests/functional/dedup/dedup_legacy_create.ksh tests/zfs-tests/tests/functional/dedup/dedup_legacy_fdt_upgrade.ksh tests/zfs-tests/tests/functional/dedup/dedup_legacy_gang.ksh tests/zfs-tests/tests/functional/dedup/dedup_legacy_import.ksh tests/zfs-tests/tests/functional/dedup/dedup_legacy_repair.ksh, <n> files hashed, zfs.ko x1, spl.ko x1
trees built and recorded
```

(The `test-runner.py.in` change is `build_tree`'s redirection of the results
directory to `$L/results`.) On a failed build, the `error:` lines go into
`PART-A.txt`; a compiler warning count other than 0 is worth quoting with its
first lines (`grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-<tree>.log`),
but is not a reason to stop.

## A3: end of part A

```sh
. /var/log/zts-dedupb3-1026f/env.sh
if ! grep -q MISMATCH $L/sha-checks.txt $L/copy.txt && [ -s $L/tree-B.txt ] &&
    [ -s $L/tree-M.txt ] && grep -qx 'trees built and recorded' $L/partA.txt; then
	date '+%F %T' > $L/PART-A.done; echo "part A done"
else
	echo "part A incomplete"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing: part A loads no module
pgrep -ax 'zed|lt-zed'                 # must print nothing
df -h /var/tmp | tail -1
```

Write `/var/log/zts-dedupb3-1026f/PART-A.txt` with, in this order: a first
line `VM 107, part A of dedup-default-blake3 (PART-A-107.md)`; A0, short
(anything found, `id -un`, `uname -r`, the distribution, `nproc`, the `umask`,
the free space, the `/etc/zfs/zpool.cache` line, the two `compatibility.d`
lines); then the contents of `$L/sha-checks.txt`, `$L/worktrees.txt`,
`$L/copy.txt` and `$L/partA.txt`; the `part A done` (or `incomplete`) line,
the `lsmod` and `pgrep` results and the `df` line; and anything that differed
from these steps, and why. Then reply with the whole of `PART-A.txt`.

Leave both trees, `$L`, `$Z/files` and `$Z/share` in place: part B starts
from them. Do not start part B without its text.
