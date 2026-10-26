# VM: does a racing ftruncate replay differently from what was live? (third run)

These instructions are for the Claude session on a disposable Debian 13 test
VM (kernel 6.12, 16 CPUs). They build upstream master `e117a84dbb` with the
debug kernel module and run an ad-hoc race reproducer, `race19344.sh`, to check
a replay-only gap read from the code of openzfs/zfs#19344 but outside its
scope: an extending write that races an `ftruncate` logs its `TX_WRITE` into
the ZIL before `zfs_setattr()` logs the `TX_SETATTR` for the new size, so a
replay applies write-then-truncate (size = the truncate's target) where the
live order was truncate-then-write (size = the write's end). Either size alone
is legal; only a replay that differs from what was live confirms the gap. The
live path is not expected to be wrong, since `zfs_setattr()` ends with
`zfs_znode_update_vfs()`, which restores `i_size` from `z_size`; the gap is in
the ZIL ordering, seen only across an export/import that replays the log.

The second run (`VM-TESTS-ftrunc2.md`) got the setup right and measured
nothing, because of one line of its script. The ZIL header was on disk before
the freeze, and the frozen log held both records of the racefile:

```
TX_SETATTR          len    112, txg 16, seq 3
        foid 128, mask 0x2068
        AT_GID   0
TX_WRITE            len    192, txg 16, seq 4
        foid 128, offset 20000, length 20000
```

The diagnostic round then stopped on `SETUP ERROR: round 1: log order 'W' for
object 128`: the script found the size change by zdb's `AT_SIZE` line, which
zdb never prints for a log written by a Linux kernel. `lr_mask` holds the
logging kernel's own `va_mask` (zfs_log.c:728), which on Linux is the
`ia_valid` bits of the VFS (zpl_inode.c:513; include/os/linux/spl/sys/vnode.h:70-76
maps `AT_SIZE` to `ATTR_SIZE`, 0x8). zdb decodes the mask with the Solaris
`AT_*` values of lib/libspl/include/sys/vnode.h (`AT_GID` 0x8, `AT_SIZE` 0x80)
at zdb_il.c:263-296, so the truncate's mask, `0x2068` (`ATTR_SIZE | ATTR_MTIME
| ATTR_CTIME | ATTR_FILE`), prints as `AT_GID 0` and never as `AT_SIZE`. The
replay itself reads the mask with the module's own bits (zfs_replay.c:961), so
it applies the size change; only zdb's decoding is wrong (reported upstream
as openzfs/zfs#5046, closed by the stale bot in 2020 without a fix).

The script is changed in that one place: `log_order()` now counts `S` on the
`TX_SETATTR` record of the racefile's `foid` whose raw `mask 0x...` has the
size bit in the Linux kernel's encoding, 0x8, and no longer reads the
`AT_SIZE` line. Everything else is the second run's script, and this document
is the second run's with the new script, its sha256 and its own directories.

The second run had already fixed the first run (`VM-TESTS-ftrunc.md`, 26 Oct
02:10-02:16Z), which measured nothing:

- `replay-truncate`: the racefile was missing after every import because
  nothing was replayed. The pool was new and had never written a ZIL block,
  so when it was frozen its on-disk ZIL header pointed to no log. The first
  fsync after the freeze created the log chain (`zil_create()`, zil.c:1028-1079)
  in a txg after the freeze, and a frozen pool never writes that txg's
  uberblock (vdev_label.c:2187). On import, `zil_claim()` found the old,
  empty header (zil.c:1197) and the replay had nothing to apply. Every ZTS
  replay test writes a synchronous file before `zpool freeze` for this
  reason (slog_replay_fs_001.ksh:64-67: "ZIL records aren't created after
  freezing the pool unless a ZIL header already exists").
- `truncate-direct`: `zdb -dddd rv19344 <obj>` names a pool, not a dataset,
  so zdb dumps object `<obj>` of the MOS (zdb.c:10630-10672, 10940-10943),
  not the file's znode, and prints no `size` line. The cache file was not
  the cause: the path is the literal `/etc/zfs/zpool.cache` in both the
  module and zdb (include/sys/fs/zfs.h:1125), whatever the build prefix.
- An empty value was counted as a mismatch.

The script follows the ZTS replay tests. It uses a child file system,
writes a ZIL header before the freeze and checks that it is on disk. After
the race it makes one fsync, then unmounts, reads the log with
`zdb -iii` while the pool is frozen, exports, imports with `-N`, checks that
the import claimed a log that needs replaying, mounts (that is when the
replay runs) and checks that the replay cleared the log. Every round prints
the order in which the log holds the write (`W`) and the size change (`S`).
A round where anything is missing exits 2 as a setup error and is never
counted. The pool is imported with an altroot and its own cache file under
the script's directory, and zdb reads only that file (`-U`).

This is one build state (master), a diagnostic round and two race cases, with
no fix to compare: the question is whether the gap reproduces on this kernel.
The session builds the tree, runs the cases, cleans up after itself and
writes every result to `/var/log/zts-ftrunc3-1026/REPORT.txt`, whose expected
contents end this document.

| Case | Command | Reads |
| --- | --- | --- |
| diagnostic | `RACE_DIAG=1 race19344.sh replay-truncate 1` | one replay round with everything printed: mount state and `ls -la` of the mountpoint after create, after `import -N` and after mount; the full `zdb -iii` output before the freeze, frozen, claimed and replayed. Not counted |
| `replay-truncate` | `race19344.sh replay-truncate 20` | 20 rounds: `O_DIRECT` write of block 1 vs `truncate -s (BS+4096)` on a frozen pool, then the fsync, export, import and replay; `round i live X replayed Y log WS\|SW claim_txg N flags F` per round, counters at the end |
| `truncate-direct` | `race19344.sh truncate-direct 50` | 50 rounds: the same race without replay; a sanity check of the live path (`zdb` size vs `stat` size) |

`race19344.sh` is inlined in STEP 1 in full, because the VM cannot read this
session's files; its sha256 is checked after it is written. It creates its own
pool `rv19344` (child file system `rv19344/fs`) on a 1G file vdev, refuses to
run if that pool already exists or something is mounted under its directory,
and destroys the pool on exit. STEP 1 moves its working directory under this
task's tree with one `sed`, changing nothing else.

Rules for the whole task:

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
  The race cases touch real pools, so they run under `sudo -n`.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x` (this task
  does not use ZTS at all), never reboot.
- Work only under `/var/tmp/zts-ftrunc3-1026`, log to `/var/log/zts-ftrunc3-1026`.
  Never read, write or delete `/var/tmp/test_results`, the first run's
  `/var/log/zts-ftrunc1026`, the second run's `/var/log/zts-ftrunc2-1026`, or
  anything else this task did not create. The
  only `rm -rf` allowed are the ones written below, with their literal paths
  (the script's own `rm -rf` of its `$DIR` is inside the inlined script and
  runs as part of it).
- `/etc/zfs/zpool.cache` exists on this VM (since 20 Oct, before these tasks)
  and is a 0-byte file since the first run: on Linux the module truncates
  the default cache file when no pool uses it any more (spa_config.c:69-79,
  zfs_file_os.c:363-366). That is expected and is not a reason to stop.
  Never delete, edit or rewrite it. This run's pool uses an altroot and its
  own cache file, so the module should not write it (spa_misc.c:904);
  STEP 4 compares its size and mtime with STEP 1's, for information.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool`, `zfs` or `zdb` command while this
  task's module is not loaded: libzfs would `modprobe` whatever zfs module the
  system has. The in-tree `zpool`/`zfs`/`zdb` are put first in `PATH` for the
  race run so the just-built debug tools are the ones used. Leave no background
  loop or watcher behind.
- Never print the environment or a token (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- A production hypervisor sits underneath this VM: the build runs under
  `nice -n 10 ionice -c3` with `make -j8`. Never `-j16`. The race cases are
  timing-sensitive on purpose and run without `nice`, one at a time.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-ftrunc3-1026/env.sh`.
- After any line that says `stop`, run nothing more: go to STEP 5 with what you
  have. A kernel panic, oops, VERIFY or hung-task message means stop and
  report, never reboot: the maintainer decides about the reboot.

## STEP 0: the VM must be free

Run all of these and keep their output:

```sh
export PATH=/usr/sbin:/sbin:$PATH
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|race19344|zdb'
ls -A /var/tmp; ls -d /var/tmp/zts-ftrunc3-1026 /var/log/zts-ftrunc3-1026 2>&1
ls -l --time-style=full-iso /etc/zfs/zpool.cache 2>&1
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
df -h /var/tmp; free -h
sudo -n true && echo "passwordless sudo ok"
sudo -n losetup -a; sudo -n dmsetup ls 2>&1
sudo -n zpool list 2>&1 | grep -w rv19344 && echo "rv19344 PRESENT" || echo "rv19344 absent"
sudo -n journalctl -k -n 1 --no-pager
command -v bash git fallocate truncate dd stat
fallocate --version 2>&1 | head -1; dd --version 2>&1 | head -1
```

Stop at once, changing nothing, if:

- `id -un` is not `tigerblue77`, or `sudo -n true` fails;
- the system is not Debian 13, or the kernel is not a 6.12 one, or its headers
  (`/lib/modules/$(uname -r)/build`) are missing: do not install them;
- a zfs or spl module is loaded, or `/dev/zfs` exists;
- `sudo -n zpool list` names a pool `rv19344`: do not remove it by hand, report
  it;
- a test or build process runs, other than this command and the shell running
  it;
- a tmux pane other than the one hosting this session runs anything but an idle
  shell;
- `/var/tmp/zts-ftrunc3-1026` or `/var/log/zts-ftrunc3-1026` already exists;
- `/var/tmp` has less than 8 GB free (one built tree plus a 1G vdev);
- `bash`, `git`, `fallocate`, `truncate`, `dd` or `stat` is missing;
- `journalctl -k` prints no kernel line.

Do not stop on `/etc/zfs/zpool.cache`, whatever its size: record its `ls`
line (a 0-byte file is what the earlier runs left).

Report what was found; that is the whole task then. Record `uname -r`, the
distribution, `nproc`, the `fallocate --version` and the `dd --version`:
another 6.12 kernel is not a reason to stop. The loop and dm devices listed
here are not ours: STEP 1 keeps them as the baseline of the leftover checks.

## STEP 1: environment, sources and the race script

```sh
sudo -n mkdir -p /var/log/zts-ftrunc3-1026 &&
    sudo -n chown -R "$(id -un):" /var/log/zts-ftrunc3-1026
cat > /var/log/zts-ftrunc3-1026/env.sh <<'ENV'
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-ftrunc3-1026
L=/var/log/zts-ftrunc3-1026
UPSTREAM=https://github.com/openzfs/zfs
HM=e117a84dbbfe0befe15110b9d1d799f47b1a5546
# The sha256 of the inlined race19344.sh, before the DIR edit: exact
# (tabs kept), and with every run of blanks squeezed to one space, which
# is the gate, since a message in transit may turn tabs into spaces and
# nothing in the script depends on which blank it uses.
RACE_SHA=b0e705a331358bd24e40d8765e87c72dc117b0b9313c2b950fa2285a6d641907
RACE_NSHA=3b6320ac038f8c122111dafc23223be7d7add0f24a8b52e578f35746708289cf
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
SETUP_RE='^(op rc=|SETUP ERROR|pool .* refusing|something is mounted|unknown case)'

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

others() {   # others <pattern>: matching processes, but not the session's bash
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Size, mtime and inode of /etc/zfs/zpool.cache, read only.
cache_stat() {
	stat -c '%s bytes, mtime %Y, inode %i' /etc/zfs/zpool.cache 2>&1
}

# Kernel messages of one run, from the journal.
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

# Leftovers after a race case: the script destroys its own pool and $DIR on
# exit, so anything left here is a defect and stops.
leftovers() {   # leftovers <label>
	local bad=0 p
	{
	p=$(sudo -n $Z/src/zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; bad=1; touch $L/HELD; }
	sudo -n losetup -a | diff -q $L/losetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER loop devices:"; sudo -n losetup -a; bad=1; touch $L/HELD; }
	sudo -n dmsetup ls 2>&1 | diff -q $L/dmsetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER dm devices:"; sudo -n dmsetup ls; bad=1; touch $L/HELD; }
	grep -w zfs /proc/mounts && { bad=1; touch $L/HELD; }
	p=$(others 'race19344')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	[ -e $Z/rv ] && { echo "  LEFTOVER $Z/rv"; bad=1; }
	echo "leftovers after $1: $([ $bad = 0 ] && echo none || echo 'FOUND: stop')"
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	return $bad
}

# One race run: srcversion guard, the script under sudo and an outer timeout,
# then its RESULT line, a setup error if any, the mismatching rounds, the
# rounds whose log shows the gap, the kernel messages and the leftovers.
# Returns 1 when nothing more may run.
race_run() {   # race_run <label> <case> <rounds> <outer timeout s> [diag]
	local label=$1 since t0 rc diag=0
	[ "${5:-}" = diag ] && diag=1
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv 2>&1)" ]; then
		echo "$label: loaded srcversion is not $(cat $L/loaded-srcv 2>&1): stop" | tee -a $L/runs.txt
		return 1
	fi
	since=$(date '+%F %T')
	t0=$(date +%s)
	sudo -n env RACE_DIAG=$diag PATH="$Z/src:/usr/sbin:/sbin:/usr/bin:/bin" \
	    timeout -k 60 $4 bash $Z/race19344.sh $2 $3 > $L/$label.log 2>&1
	rc=$?
	echo "$label rc=$rc wall=$(( $(date +%s) - t0 ))s: $(grep '^RESULT' $L/$label.log || echo 'NO RESULT')" |
	    tee -a $L/runs.txt
	if [ $rc -eq 2 ] || grep -qE "$SETUP_RE" $L/$label.log; then
		echo "  $label: setup problem ($(grep -m1 -E "$SETUP_RE" $L/$label.log)): stop" | tee -a $L/runs.txt
		grep -vE '^(round |zdb |mount )' $L/$label.log | tail -10 | sed 's/^/  /' | tee -a $L/runs.txt
		kern_check $label "$since"
		leftovers $label
		return 1
	fi
	awk '/^round / && $4 != $6' $L/$label.log | sed 's/^/  mismatch: /' | tee -a $L/runs.txt
	awk '/^round / && $5 == "replayed" && $4 == 262144 && $8 == "WS"' $L/$label.log |
	    sed 's/^/  gap in log: /' | tee -a $L/runs.txt
	kern_check $label "$since" || return 1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		echo "$label: outer timeout: stop" | tee -a $L/runs.txt
		return 1
	fi
	if [ $rc -ne 0 ]; then
		echo "$label: script exited $rc: stop" | tee -a $L/runs.txt
		leftovers $label
		return 1
	fi
	leftovers $label
}
ENV
. /var/log/zts-ftrunc3-1026/env.sh
mkdir -p $Z
date '+%F %T' > $L/start-time.txt
sudo -n losetup -a > $L/losetup-before.txt
sudo -n dmsetup ls > $L/dmsetup-before.txt 2>&1
sudo -n dmesg | grep -ciE "$KRE" > $L/dmesg-baseline.txt
echo "baseline: dmesg $(cat $L/dmesg-baseline.txt) suspicious lines, from before this task"
cache_stat > $L/zpool-cache-before.txt
echo "/etc/zfs/zpool.cache before: $(cat $L/zpool-cache-before.txt)"
```

Write the race script, check its sha256, then point its working directory
under this task's tree with the one `sed` and nothing else:

```sh
. /var/log/zts-ftrunc3-1026/env.sh
cat > $Z/race19344.sh <<'RACE'
#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# Ad-hoc race reproducer for openzfs/zfs#19344 and the ftruncate replay gap.
# Run as root on a disposable VM with the module under test loaded. Uses its
# own pool on a file vdev under $DIR, imported with an altroot and its own
# cache file, so the module never writes /etc/zfs/zpool.cache, and zdb reads
# only that cache file (-U).
#
# usage: race19344.sh <case> [rounds]
#   extend-direct    O_DIRECT write of block 1 vs fallocate -l (BS+4096)
#   extend-buffered  same, buffered write
#   punch-eof        O_DIRECT write of block 1 vs fallocate -p -o BS -l 4096
#   zero-eof         O_DIRECT write of block 1 vs fallocate -z -n -o BS -l 4096
#   truncate-direct  O_DIRECT write of block 1 vs truncate -s (BS+4096)
#   replay-extend    extend-direct on a frozen pool, then export/import so
#                    the ZIL is replayed; compares size before and after
#   replay-truncate  truncate-direct the same way
# RACE_DIAG=1 in the environment prints the full zdb output, the mount state
# and the mountpoint listing of every replay round.
#
# Per round it prints: round, live stat size, on-disk size (zdb after
# zpool sync, or stat after the replay), and for replay cases the order in
# which the log holds the write (W) and the size change (S, or T for a
# TX_TRUNCATE). Final line: counters. Exit 2 is a setup error: something
# needed for a measurement was missing, and nothing was counted for it.

set -u
CASE=${1:?case}
ROUNDS=${2:-100}
POOL=rv19344
DIR=/var/tmp/rv19344
FS=$POOL/fs
DEVDIR=$DIR/dev
ALT=$DIR/alt
CACHE=$DIR/zpool.cache
DIAG=${RACE_DIAG:-0}
BS=131072
EXPECT=$((2 * BS))
WOFF=$(printf '%x' $BS)

die()
{
	echo "SETUP ERROR: $*" >&2
	exit 2
}

case $CASE in
extend-direct|extend-buffered|punch-eof|zero-eof|truncate-direct) ;;
replay-extend|replay-truncate) ;;
*)	echo "unknown case $CASE" >&2; exit 2 ;;
esac

if zpool list "$POOL" >/dev/null 2>&1; then
	echo "pool $POOL already exists, refusing" >&2
	exit 2
fi

under_dir()
{
	awk -v d="$DIR" '$2 == d || index($2, d "/") == 1' /proc/mounts
}

if [ -n "$(under_dir)" ]; then
	echo "something is mounted under $DIR, refusing" >&2
	exit 2
fi

rmpool()
{
	if zpool list "$POOL" >/dev/null 2>&1; then
		zpool destroy -f "$POOL" || echo "rmpool: destroy failed" >&2
	fi
	if zpool list "$POOL" >/dev/null 2>&1 || [ -n "$(under_dir)" ]; then
		echo "rmpool: $POOL or a mount is left, $DIR kept" >&2
		return 1
	fi
	rm -rf "$DIR"
}
trap rmpool EXIT

mkpool()
{
	rm -rf "$DIR" && mkdir -p "$DEVDIR" || die "cannot make $DEVDIR"
	truncate -s 1G "$DEVDIR/vdev" || die "cannot make the vdev"
	zpool create -f -R "$ALT" -o cachefile="$CACHE" -O compression=off \
	    -O mountpoint=none -O canmount=off "$POOL" "$DEVDIR/vdev" ||
	    die "zpool create failed"
	zfs create -o mountpoint="$DIR/mnt" -o recordsize=$BS "$FS" ||
	    die "zfs create failed"
	MNT=$(zfs get -H -o value mountpoint "$FS")
	is_mounted || die "$FS is not mounted after zfs create"
	f=$MNT/racefile
}

is_mounted()
{
	[ "$(zfs get -H -o value mounted "$FS" 2>/dev/null)" = yes ] &&
	    awk -v m="$MNT" '$2 == m && $3 == "zfs"' /proc/mounts | grep -q .
}

mount_state()
{
	# $1 = tag
	echo "mount $1: zfs mounted=$(zfs get -H -o value mounted "$FS" 2>&1)" \
	    "mountpoint=$(zfs get -H -o value mountpoint "$FS" 2>&1)"
	under_dir | sed "s/^/mount $1: \/proc\/mounts: /"
	if [ "$DIAG" = 1 ]; then
		ls -la "$MNT" 2>&1 | sed "s/^/mount $1: ls| /"
	fi
}

# zdb_run <tag> <zdb args>: the zdb in PATH, on this pool's own cache file.
# Sets ZOUT and ZRC. Its stderr is printed whole; its stdout whole with
# RACE_DIAG=1, else the ZIL header and record lines.
zdb_run()
{
	typeset tag=$1
	shift
	[ -s "$CACHE" ] || die "$tag: cache file $CACHE is missing or empty"
	ZOUT=$(zdb -U "$CACHE" "$@" 2>"$DIR/zdb.err")
	ZRC=$?
	echo "zdb $tag: rc=$ZRC: zdb -U $CACHE $*"
	sed "s/^/zdb $tag: stderr| /" "$DIR/zdb.err"
	if [ "$DIAG" = 1 ]; then
		printf '%s\n' "$ZOUT" | sed "s/^/zdb $tag: | /"
	else
		printf '%s\n' "$ZOUT" |
		    grep -E 'ZIL header|TX_[A-Z0-9_]+ +len|foid|AT_SIZE|^\ssize\s' |
		    sed "s/^/zdb $tag: | /"
	fi
	[ $ZRC -eq 0 ] || die "$tag: zdb exited $ZRC"
}

# ZIL header fields from ZOUT: zil_hdr (yes/no), claim_txg, flags.
zil_hdr_parse()
{
	typeset h
	h=$(printf '%s\n' "$ZOUT" | grep -m1 'ZIL header:')
	if [ -z "$h" ]; then
		zil_hdr=no claim_txg= flags=
		return
	fi
	zil_hdr=yes
	claim_txg=$(printf '%s\n' "$h" |
	    sed -n 's/.*claim_txg \([0-9][0-9]*\),.*/\1/p')
	flags=$(printf '%s\n' "$h" | sed -n 's/.*flags \(0x[0-9a-f]*\).*/\1/p')
}

# The order in which zdb -iii (in ZOUT) lists the logged ops on object $1:
# W for the TX_WRITE at offset BS, S for a TX_SETATTR whose raw mask has
# the size bit, T for a TX_TRUNCATE; each once, at its first record.
# The size bit is read from the "foid N, mask 0x..." line, never from
# zdb's AT_SIZE line: lr_mask holds the logging kernel's own bits (Linux
# ATTR_SIZE is 0x8), and zdb decodes it with the Solaris AT_* bits (AT_SIZE
# 0x80), so on Linux it prints "AT_GID" for a size change and no AT_SIZE.
SIZE_BIT=8
log_order()
{
	printf '%s\n' "$ZOUT" | awk -v obj="$1" -v off="$WOFF," -v bit=$SIZE_BIT '
	# the bit (1, 2, 4 or 8) of the last hex digit of a "0x..." mask
	function lowbit(m, b,  d) {
		d = index("0123456789abcdef", tolower(substr(m, length(m)))) - 1
		return (d >= 0 && int(d / b) % 2 == 1)
	}
	$1 ~ /^(CI-)?TX_/ && $2 == "len" {
		t = $1; sub(/^CI-/, "", t); foid = ""; next
	}
	$1 == "foid" {
		foid = $2; sub(/,$/, "", foid)
		if (foid != obj)
			next
		if ((t == "TX_WRITE" || t == "TX_WRITE2") && $3 == "offset" &&
		    $4 == off && !w) {
			s = s "W"; w = 1
		}
		if (t == "TX_TRUNCATE" && !tr) {
			s = s "T"; tr = 1
		}
		if (t == "TX_SETATTR" && $3 == "mask" && $4 ~ /^0x[0-9a-f]+$/ &&
		    lowbit($4, bit) && !a) {
			s = s "S"; a = 1
		}
		next
	}
	END { print s }'
}

seed_file()
{
	rm -f "$f"
	dd if=/dev/zero of="$f" bs=$BS count=1 conv=fsync status=none ||
	    die "seed write of $f failed"
}

race_once()
{
	# leaves dd and the competing op finished
	typeset oflag=oflag=direct
	[ "$CASE" = extend-buffered ] && oflag=
	dd if=/dev/urandom of="$f" bs=$BS count=1 seek=1 $oflag \
	    conv=notrunc status=none &
	typeset ap=$!
	case $CASE in
	extend-*|replay-extend)	fallocate -l $((BS + 4096)) "$f" ;;
	punch-eof)	fallocate -p -o $BS -l 4096 "$f" ;;
	zero-eof)	fallocate -z -n -o $BS -l 4096 "$f" ;;
	truncate-direct|replay-truncate)
			truncate -s $((BS + 4096)) "$f" ;;
	esac
	typeset frc=$?
	wait $ap
	typeset drc=$?
	if [ $frc -ne 0 ] || [ $drc -ne 0 ]; then
		echo "op rc=$frc dd rc=$drc" >&2
		exit 2
	fi
}

is_num()
{
	case $1 in
	''|*[!0-9]*) return 1 ;;
	esac
}

live_short=0
disk_short=0
mismatch=0
replays=0
log_ws=0
log_sw=0
gap_in_log=0

if [ "$CASE" = replay-extend ] || [ "$CASE" = replay-truncate ]; then
	rd=$(cat /sys/module/zfs/parameters/zil_replay_disable 2>&1)
	[ "$rd" = 0 ] || die "zil_replay_disable is '$rd', not 0"
	for ((i = 1; i <= ROUNDS; i++)); do
		mkpool
		mount_state "$i created"
		# A ZIL header must be on disk before the freeze, or the log
		# written after it starts in a block no synced header points
		# to, and the import replays nothing.
		dd if=/dev/zero of="$MNT/sync" conv=fdatasync,fsync bs=1 \
		    count=1 status=none || die "header write failed"
		seed_file
		obj=$(stat -c %i "$f")
		is_num "$obj" || die "round $i: no object number for $f"
		zpool sync "$POOL" || die "zpool sync failed"
		zdb_run "$i pre-freeze" -iii "$FS"
		zil_hdr_parse
		[ "$zil_hdr" = yes ] ||
		    die "round $i: no ZIL header on disk before the freeze"
		zpool freeze "$POOL" || die "zpool freeze failed"
		race_once
		live=$(stat -c %s "$f")
		is_num "$live" || die "round $i: no live size for $f"
		# One commit, after both ops have returned: the log keeps
		# the order in which they were logged, which is the order
		# the gap is about.
		dd if=/dev/null of="$f" conv=notrunc,fsync status=none ||
		    die "round $i: fsync of $f failed"
		zfs unmount "$FS" || die "round $i: zfs unmount failed"
		zdb_run "$i frozen" -iii "$FS"
		order=$(log_order "$obj")
		case $order in
		*W*) ;;
		*)	die "round $i: the log holds no TX_WRITE at" \
			    "0x$WOFF for object $obj" ;;
		esac
		if [ "$CASE" = replay-truncate ]; then
			case $order in
			WS|SW) ;;
			*)	die "round $i: log order '$order' for object" \
				    "$obj, expected WS or SW" ;;
			esac
		fi
		zpool export "$POOL" || die "round $i: zpool export failed"
		zpool import -N -f -R "$ALT" -o cachefile="$CACHE" \
		    -d "$DEVDIR" "$POOL" || die "round $i: zpool import failed"
		mount_state "$i imported -N"
		zpool sync "$POOL" || die "zpool sync failed"
		zdb_run "$i claimed" -iii "$FS"
		zil_hdr_parse
		is_num "$claim_txg" && [ "$claim_txg" -ne 0 ] &&
		    [ -n "$flags" ] && [ $((flags & 1)) -eq 1 ] ||
		    die "round $i: import did not claim a log to replay" \
		    "(header $zil_hdr, claim_txg '$claim_txg', flags '$flags')"
		ctxg=$claim_txg cflags=$flags
		zfs mount "$FS" || die "round $i: zfs mount failed"
		is_mounted || die "round $i: $FS is not mounted at $MNT"
		mount_state "$i mounted"
		zpool sync "$POOL" || die "zpool sync failed"
		zdb_run "$i replayed" -iii "$FS"
		zil_hdr_parse
		# zil_replay() ends with zil_destroy(B_FALSE), which clears
		# the header; without a replay the header keeps a block.
		[ "$zil_hdr" = no ] ||
		    die "round $i: ZIL header still present after the mount"
		replays=$((replays + 1))
		[ -e "$f" ] || die "round $i: $f is missing after the replay"
		after=$(stat -c %s "$f")
		is_num "$after" || die "round $i: no replayed size for $f"
		echo "round $i live $live replayed $after log $order" \
		    "claim_txg $ctxg flags $cflags"
		case $order in
		WS)	log_ws=$((log_ws + 1))
			[ "$live" -eq $EXPECT ] &&
			    gap_in_log=$((gap_in_log + 1)) ;;
		SW)	log_sw=$((log_sw + 1)) ;;
		esac
		if [ "$CASE" = replay-extend ]; then
			[ "$live" -lt $EXPECT ] && live_short=$((live_short + 1))
			[ "$after" -lt $EXPECT ] &&
			    disk_short=$((disk_short + 1))
		fi
		[ "$live" != "$after" ] && mismatch=$((mismatch + 1))
		rmpool || die "round $i: cleanup failed"
	done
else
	mkpool
	for ((i = 1; i <= ROUNDS; i++)); do
		seed_file
		race_once
		live=$(stat -c %s "$f")
		obj=$(stat -c %i "$f")
		is_num "$live" && is_num "$obj" ||
		    die "round $i: no live size or object for $f"
		zpool sync "$POOL" || die "zpool sync failed"
		zdb_run "$i" -dddd "$FS" "$obj"
		disk=$(printf '%s\n' "$ZOUT" |
		    awk '$1 == "size" { print $2; exit }')
		is_num "$disk" ||
		    die "round $i: zdb printed no size for object $obj"
		echo "round $i live $live disk $disk"
		if [ "$CASE" = truncate-direct ]; then
			# either order is legal; only live != disk is a defect
			:
		else
			[ "$live" -lt $EXPECT ] && live_short=$((live_short + 1))
			[ "$disk" -lt $EXPECT ] &&
			    disk_short=$((disk_short + 1))
		fi
		[ "$live" != "$disk" ] && mismatch=$((mismatch + 1))
	done
fi

echo "RESULT case=$CASE rounds=$ROUNDS live_short=$live_short" \
    "disk_short=$disk_short live_ne_disk=$mismatch replays=$replays" \
    "log_ws=$log_ws log_sw=$log_sw gap_in_log=$gap_in_log"
RACE
same "race19344.sh sha256, blanks squeezed" \
    "$(tr -s ' \t' ' ' < $Z/race19344.sh | sha256sum | cut -d' ' -f1)" $RACE_NSHA
same "race19344.sh sha256, exact (information only)" \
    "$(sha256sum $Z/race19344.sh | cut -d' ' -f1)" $RACE_SHA
sed -i 's|^DIR=/var/tmp/rv19344$|DIR='"$Z"'/rv|' $Z/race19344.sh
bash -n $Z/race19344.sh && echo "race19344.sh bash -n ok, DIR -> $Z/rv"
grep -n '^DIR=' $Z/race19344.sh
```

Expected: `ok: race19344.sh sha256, blanks squeezed`, then `race19344.sh
bash -n ok, DIR -> /var/tmp/zts-ftrunc3-1026/rv` and
`32:DIR=/var/tmp/zts-ftrunc3-1026/rv`. The exact sha256 line is information
only: `MISMATCH` there just means tabs became spaces in transit, which changes
nothing the script does; report which of the two lines you got. A `MISMATCH`
on the squeezed line means the script itself did not come through: stop and
report, do not edit it to match.

Clone upstream and check out the base. This is read-only towards GitHub:
nothing is pushed.

```sh
. /var/log/zts-ftrunc3-1026/env.sh
git clone $UPSTREAM $Z/src > $L/clone.log 2>&1; echo "clone rc=$?"
cd $Z/src && git checkout --detach $HM > $L/checkout.log 2>&1
echo "checkout rc=$? at $(git rev-parse --short=10 HEAD)"
git log -1 --oneline
```

Expected: `clone rc=0`, `checkout rc=0 at e117a84dbb`, and the one-line log of
`e117a84dbb libzutil: keep the latest label when two vdevs claim one id`.

## STEP 2: build and load

```sh
. /var/log/zts-ftrunc3-1026/env.sh; cd $Z/src
nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --enable-debug && make -j8' \
    > $L/build.log 2>&1
echo "build rc=$?"
tail -3 $L/build.log
grep -nE 'error:|Error [0-9]' $L/build.log | head
```

Expected: `build rc=0`. On a failed build, the `error:` lines go into the
report.

```sh
. /var/log/zts-ftrunc3-1026/env.sh; cd $Z/src
modinfo -F srcversion module/zfs.ko > $L/loaded-srcv
sudo -n ./scripts/zfs.sh
echo "load rc=$?"
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv)" ] ||
    [ "$(modinfo -F srcversion module/zfs.ko)" != "$(cat $L/loaded-srcv)" ]; then
	echo "GUARD FAILED: stop"
else
	echo "guard ok: $(cat $L/loaded-srcv)," \
	    "zpool is $(readlink -f $Z/src/zpool >/dev/null 2>&1 && echo in-tree || echo MISSING)," \
	    "zil_replay_disable=$(cat /sys/module/zfs/parameters/zil_replay_disable)"
fi
```

Expected: `load rc=0` and `guard ok: <srcversion>, zpool is in-tree,
zil_replay_disable=0`. The guard makes sure the module that got loaded is the
one just built, so the race uses the tree under test and not a system module.
`zil_replay_disable` must be 0 for any replay to happen; the script refuses
otherwise. Do not change it.

## STEP 3: the diagnostic round, then the race cases

The diagnostic round runs first, alone. Its whole log goes into the report.

```sh
. /var/log/zts-ftrunc3-1026/env.sh
race_run diag-replay-truncate replay-truncate 1 900 diag
wc -l < $L/diag-replay-truncate.log
cut -c1-200 $L/diag-replay-truncate.log | head -150
```

Expected in `diag-replay-truncate.log`, in this order:

- `mount 1 created: zfs mounted=yes mountpoint=/var/tmp/zts-ftrunc3-1026/rv/alt/var/tmp/zts-ftrunc3-1026/rv/mnt`,
  its `/proc/mounts` line (type `zfs`) and an `ls| ` listing of the new, empty
  file system;
- `zdb 1 pre-freeze: rc=0`, then `ZIL header: claim_txg 0, ...`: the header the
  first run lacked;
- `zdb 1 frozen: rc=0`, the same header, then for the racefile's `foid` a
  `TX_WRITE` with `offset 20000, length 20000` and a `TX_SETATTR` with
  `mask 0x2068`, in either order. Master's zdb prints `AT_GID   0` under that
  mask and no `AT_SIZE` line: that is the zdb decoding defect described at
  the top, expected, and not a reason to stop;
- `mount 1 imported -N: zfs mounted=no`, no `/proc/mounts` line, and the
  listing of the empty mountpoint directory;
- `zdb 1 claimed: rc=0`, `ZIL header: claim_txg <N>, ... flags 0x3` (N not 0;
  bit 0x1 is `ZIL_REPLAY_NEEDED`), with the same records;
- `mount 1 mounted: zfs mounted=yes`, its `/proc/mounts` line, and a listing
  that shows `racefile` and `sync`;
- `zdb 1 replayed: rc=0` with no `ZIL header` line (the replay destroyed the
  log);
- `round 1 live <X> replayed <Y> log <WS|SW> claim_txg <N> flags 0x3` and the
  `RESULT` line with `replays=1`.

`zdb ... stderr| ` lines, if any, are zdb's own stderr: keep them. If the
round stops on a `SETUP ERROR`, that line names what was missing; report it
with the whole log and run nothing more. The diagnostic round is not counted
in the verdict, but a mismatch in it is reported as an observation.

Then the counted cases:

```sh
. /var/log/zts-ftrunc3-1026/env.sh
race_run replay-truncate replay-truncate 20 3600 && race_run truncate-direct truncate-direct 50 3600
```

| Case | Expected, and why |
| --- | --- |
| `replay-truncate` | A `RESULT` line with `replays=20`, `log_ws + log_sw = 20` and `live_ne_disk=N`. Each round line ends with the log order. **Any round with `live` != `replayed` confirms the gap**; the expected bad round is `live 262144 replayed 135168 log WS`: the write landed after the truncate (live = write's end) but its `TX_WRITE` was logged before the `TX_SETATTR`, so the replay applies the truncate last. `race_run` prints such rounds as `mismatch:` and `gap in log:`. `0` mismatches in 20 rounds is **inconclusive** (the window is narrow), not a disproof. Kernel 0, leftovers none. |
| `truncate-direct` | A sanity check of the live path only: either `135168` or `262144` is legal, and `live_ne_disk` should be `0`. A non-zero `live_ne_disk` here would be a live-path defect, not the replay gap; report it. Kernel 0, leftovers none. |

If a run exits 2 or prints `SETUP ERROR` or `op rc=` (a missing ZIL header, a
log without the expected records, an import that claimed nothing, a
`zpool freeze`, an `O_DIRECT` write on the frozen pool, a mount or an import
failing, zdb printing no size), that is a setup problem: `race_run` stops and
the report carries the last lines and the exact error; do not work around it.
A setup error is never a result, in either direction.

## What the results mean

- **Gap confirmed**: `replay-truncate` has `live_ne_disk > 0`, with at least
  one `live 262144 replayed 135168 log WS` round (`gap_in_log > 0`), and
  `truncate-direct` has `live_ne_disk 0`. The replay-only ordering gap is real
  on this kernel.
- **Mismatch of another shape**: `live_ne_disk > 0` but no such round (for
  example `live 135168 replayed 262144 log SW`). Not the predicted gap; report
  every mismatching round with its log order.
- **Inconclusive**: `replay-truncate` has `live_ne_disk 0` and `replays=20`,
  with no setup problem. Every round replayed, and none hit the window. The
  window is narrow, so this neither confirms nor disproves the gap, and it
  stays a code-reading note. Report the counts, including `log_ws`/`log_sw`.
- **No measurement**: a setup problem in any run. Nothing was measured by
  that run, and nothing about the gap follows from it.
- **Live-path defect**: `truncate-direct` has `live_ne_disk > 0`. That is a
  different, more serious finding than the replay gap; report every mismatching
  round.
- **Any kernel VERIFY, panic or hung task**: the build is `--enable-debug`, so
  a reached assertion panics or logs; the line and the case are the result.
  Stop.

## STEP 4: whole-task kernel check

```sh
. /var/log/zts-ftrunc3-1026/env.sh
sudo -n journalctl -k --since "$(cat $L/start-time.txt)" --no-pager -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kern-all.txt) suspicious lines" | tee -a $L/runs.txt
grep -iE "$KRE" $L/kern-all.txt | head -40
sudo -n dmesg --time-format=iso | grep -ciE "$KRE" |
    xargs -I{} echo "dmesg now: {} suspicious lines (baseline $(cat $L/dmesg-baseline.txt))" | tee -a $L/runs.txt
c=$(cache_stat)
[ "$c" = "$(cat $L/zpool-cache-before.txt)" ] &&
    echo "/etc/zfs/zpool.cache untouched: $c" | tee -a $L/runs.txt ||
    echo "/etc/zfs/zpool.cache CHANGED: before $(cat $L/zpool-cache-before.txt), now $c" | tee -a $L/runs.txt
ls $L/PANIC $L/HELD 2>&1
df -h /var/tmp
```

A `CHANGED` line on `/etc/zfs/zpool.cache` is information, not a stop: report
it, and leave the file as it is.

## STEP 5: cleanup and report

Only if neither `$L/PANIC` nor `$L/HELD` exists, unload and remove the work
directory (a leftover pool, device or process keeps the module busy: report
them, and the maintainer decides):

```sh
. /var/log/zts-ftrunc3-1026/env.sh
if [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/loaded-srcv)" ]; then
	(cd $Z/src && sudo -n ./scripts/zfs.sh -u); echo "unload rc=$?"
else
	echo "no module of this task is loaded"
fi
lsmod | grep -E '^(zfs|spl) '          # must print nothing
rm -rf /var/tmp/zts-ftrunc3-1026
ls -d /var/tmp/zts-ftrunc3-1026 2>&1    # No such file or directory
df -h /var/tmp
```

Keep `/var/log/zts-ftrunc3-1026` and write `/var/log/zts-ftrunc3-1026/REPORT.txt`
with:

- STEP 0, short: anything found, `id -un`, `uname -r`, the distribution,
  `nproc`, the `fallocate --version` and `dd --version`, the free space, the
  `rv19344 absent` check, the `/etc/zfs/zpool.cache` line and the dmesg
  baseline;
- the sha256 check, the `DIR` edit, the clone and checkout lines;
- build rc and wall time, the srcversion, and the load and guard lines;
- the whole of `diag-replay-truncate.log`;
- every `round` line of `replay-truncate.log`;
- `runs.txt` as it stands: each run's rc, wall time and `RESULT` line, its
  setup problem if any, its mismatching and gap-in-log rounds, its kernel line
  and its leftovers line, plus the whole-task kernel and dmesg counts and the
  `/etc/zfs/zpool.cache` line;
- which case of "What the results mean" applies;
- the cleanup output, or why it was skipped;
- anything that differed from these steps, and why.

Then reply with the whole of `REPORT.txt`.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect.

```
VM, ftruncate replay gap on master e117a84dbb, third run (VM-TESTS-ftrunc3.md)

STEP 0: free; tigerblue77; <uname -r, a 6.12 kernel>; Debian GNU/Linux 13 (trixie); nproc 16;
  fallocate <version>; dd <version>; /var/tmp <N>G free; bash, git, fallocate, truncate, dd, stat present;
  passwordless sudo; rv19344 absent; /etc/zfs/zpool.cache <ls line: 0 bytes, as the earlier runs left it>;
  baseline: loop <losetup -a, or none>; dm <dmsetup ls>; dmesg <n> suspicious lines

sources: race19344.sh sha256 ok (blanks squeezed; exact: <ok or MISMATCH>), DIR -> /var/tmp/zts-ftrunc3-1026/rv, bash -n ok;
  clone rc=0, checkout rc=0 at e117a84dbb
build: rc=0 wall=<s>, srcversion <srcv>, load rc=0, guard ok, zpool in-tree, zil_replay_disable=0

diagnostic round (diag-replay-truncate.log, whole):
<the log: mount 1 created ..., zdb 1 pre-freeze ... ZIL header ..., zdb 1 frozen ... TX_WRITE / TX_SETATTR ...,
 mount 1 imported -N ..., zdb 1 claimed ... flags 0x3 ..., mount 1 mounted ..., zdb 1 replayed ...,
 round 1 live <X> replayed <Y> log <WS|SW> claim_txg <N> flags 0x3, RESULT ... replays=1 ...>

replay-truncate rounds:
round 1 live <X> replayed <Y> log <WS|SW> claim_txg <N> flags 0x3
<... one line per round, 20 in all>

runs:
diag-replay-truncate rc=0 wall=<s>: RESULT case=replay-truncate rounds=1 live_short=0 disk_short=0 live_ne_disk=<0|1> replays=1 log_ws=<n> log_sw=<n> gap_in_log=<n>
kernel diag-replay-truncate: 0 suspicious lines
leftovers after diag-replay-truncate: none
replay-truncate rc=0 wall=<s>: RESULT case=replay-truncate rounds=20 live_short=0 disk_short=0 live_ne_disk=<N> replays=20 log_ws=<n> log_sw=<n> gap_in_log=<n>
  mismatch: round <i> live 262144 replayed 135168 log WS ...      (one line per mismatching round, or none)
  gap in log: round <i> live 262144 replayed 135168 log WS ...    (one line per such round, or none)
kernel replay-truncate: 0 suspicious lines
leftovers after replay-truncate: none
truncate-direct rc=0 wall=<s>: RESULT case=truncate-direct rounds=50 live_short=0 disk_short=0 live_ne_disk=0 replays=0 log_ws=0 log_sw=0 gap_in_log=0
kernel truncate-direct: 0 suspicious lines
leftovers after truncate-direct: none
kernel, whole task: 0 suspicious lines
dmesg now: <n> suspicious lines (baseline <n>)
/etc/zfs/zpool.cache untouched: <size, mtime, inode as in STEP 1>

verdict: <gap confirmed (live_ne_disk=<N>, gap_in_log=<n>, with live 262144 / replayed 135168 / log WS) |
  mismatch of another shape: ... | inconclusive (live_ne_disk 0, replays 20, narrow window) |
  no measurement: <setup error> | live-path defect (truncate-direct live_ne_disk > 0) | kernel event: ...>

cleanup: unload rc=0, no zfs or spl module left, /var/tmp/zts-ftrunc3-1026 removed, /var/tmp <N>G free
deviations: none
```
