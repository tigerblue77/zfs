# VM 106 (CHSONVTST03): failing-first runs of `recv_longname_disabled`

These instructions produce kernel evidence for the fork branch
`recv-longname-feature-check` of https://github.com/tigerblue77/zfs. They
start from the build half sent earlier, which built two trees on this VM
under `/var/tmp/zts-rln-1026` and wrote its logs to
`/var/log/zts-rln-1026`. Nothing is rebuilt here. The branch's one commit
changes `zfs_prop_set_special()` in `module/zfs/zfs_ioctl.c` and adds the
ZTS test `tests/functional/rsend/recv_longname_disabled.ksh` (registered
in `linux.run` and `Makefile.am`), which runs here in each tree:

| State | Tree | Code | Runs | Expected |
| --- | --- | --- | --- | --- |
| B | `/var/tmp/zts-rln-1026/B` | branch head `8a47085858d15c27f377542c90422a7ac7afcc5d` (master + the fix + the test) | `b1`-`b3`, `bd` (the same with debug output), `bg` (`-T rsend` once) | PASS 3 of 3 (and `bd`); `bg` 88 PASS, 1 SKIP |
| M | `/var/tmp/zts-rln-1026/M` | master `e117a84dbbfe0befe15110b9d1d799f47b1a5546` + the branch's three test files only | `m1`-`m3` (and `mg1`... only for a `bg` failure) | FAIL 3 of 3, at test line 63 |

B runs first, then M. One separate observation, unrelated to this branch,
follows on M while M is loaded: STEP 4b records master's `zpool remove -n`
output on a pool with a raidz vdev, for another bug report. It does not
change the verdict of the runs above.

## What the test does, and why M must fail

The test runs inside the rsend directory's `setup` and `cleanup`, which
create `testpool`, `testpool2` and `testpool3` on the three loop devices
ZTS makes, and destroy them afterwards.

- Lines 51-55: it creates `testpool/longname_disabled` with `longname=on`,
  creates one short file in it, snapshots it, sends the snapshot with
  `zfs send -p` (properties included) to
  `/var/tmp/zts-rln-1026/files/backdir-rsend/stream`, and checks that the
  stream does not carry the longname feature flag: a stream carries it
  only once the source holds a long name, and this one holds none.
- Lines 57-59: it recreates `testpool3` with `feature@longname=disabled` and
  receives the stream as `testpool3/longname_disabled`.
- Lines 61-63: the received filesystem must have `longname=off`.
- Lines 65-69: creating a 517-byte name in it must fail, and after
  `zpool sync` the feature must still be `disabled`.

**On M,** `zfs_prop_set_special()` returns -1 for a received `longname`
without looking at the pool, on the assumption that
`dmu_recv_begin_check()` already refused a stream that needs the feature.
That stream has no flag, so the received "on" is stored, the receive
succeeds silently, and the kernel logs once:

```
NOTICE: Skipping ZFS_PROP_LONGNAME checks for dsname=testpool3/longname_disabled
```

`get_prop longname` then reads `on`, and the first failing line is the
`log_fail` of lines 62-63. It prints, with no `ERROR:` prefix:

```
testpool3/longname_disabled has longname=on with feature@longname disabled
```

The test ends there and its cleanup destroys the pool. Lines 65-71 never
run on M: no long name is created, so the activation of
`feature@longname` that would trip the VERIFY at `zfeature.c:413`
(`feature_do_action()`) in syncing context, and panic the VM, never
happens. M fails cleanly.

**On B,** a received "on" is checked against the pool with `spa_open()`.
The feature is disabled, so the property fails with `ENOTSUP` and goes to
the receive's error list. `zfs recv` still exits 0; on stderr it prints

```
cannot receive longname property on testpool3/longname_disabled: pool and or dataset must be upgraded to set this property or value
```

which `log_must` hides when the command succeeds. The `bd` run sets
`LOGAPI_DEBUG=1`, which makes ZTS print that stderr, to capture the
message as a user sees it. The filesystem keeps `longname=off`, the
517-byte name fails with "File name too long", the feature stays
`disabled`, and the test passes. The `NOTICE` line above no longer exists
in B's code.

What was checked before this was written:

- The path cannot be run in userspace: `zfs_ioctl.c` and the ZPL code that
  creates names are not part of libzpool, so the M failure above is
  predicted from reading the code of these two commits.
- An older revision of the branch, `8e0d35192`, has the same patch
  (`git patch-id --stable`: `a4f2eb649c4733921ffad46eee1b545f362224a5` for
  both). Another VM ran it on 20 Oct: the test passed with the fix and
  failed cleanly without it, and the rsend group gave 88 PASS, 1 SKIP. Only
  that verdict was kept, not the output.
- The fork's CI ran this exact head on AlmaLinux 8 on 26 Oct:
  `recv_longname_disabled` PASS, and the rsend group 88 PASS, 1 SKIP
  (`rsend_008_pos`) in 14 minutes.

**Why the whole rsend group, once, on B.** `-T rsend` selects 81 tests from
`common.run`'s `[tests/functional/rsend]` and 4 from `linux.run`'s
`[tests/functional/rsend:Linux]` (`send_realloc_dnode_size`,
`send_encrypted_files`, `send-c_longname`, `recv_longname_disabled`), each
section with its own `setup` and `cleanup`: 89 results. It is the group the
changed code serves (every received property goes through
`zfs_prop_set_special()`), and it took 14 minutes in CI, so it should end
well within 25 minutes here. `rsend_008_pos` always reports SKIP
(`log_unsupported "Occasionally hangs"`), which `zts-report` lists as
expected.

## Rules for the whole run

- Run everything as root. Shell variables may not survive between your
  commands, so every block below starts by setting them or by sourcing
  `/var/log/zts-rln-1026/env.sh`.
- A production hypervisor sits underneath this VM. Module loads and every
  ZTS run go through `nice -n 10 ionice -c3` (the blocks and `run.sh` do
  it), one run at a time, with nothing else running beside them.
- Post nothing, push nothing, install nothing, never reboot, and never run
  `scripts/zfs-helpers.sh` or `zfs-tests.sh -x`. Rebuild nothing: the trees
  are already built.
- Work only under `/var/tmp/zts-rln-1026` (W) and `/var/log/zts-rln-1026`
  (L). Never read from, write to or delete `/var/tmp/test_results`, another
  `/var/tmp/zts-*` or `/var/log/zts-*` directory, `/var/tmp/ra-work`, or
  anything else this task did not create. The test pools mount under
  `/testpool*` while they exist; the tests' cleanup removes those
  directories, and `run.sh` checks that it did.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the test pools in it while they exist and truncates it to 0 bytes
  when they are gone, as in every ZTS run: STEP 0 and STEP 6 record it.
- Never run a `zpool` or `zfs` command while none of this task's modules is
  loaded. Never print the environment (`env`, `printenv`, `set`).
- The only `rm -rf` allowed is the one in STEP 6. The only other removal
  is STEP 4b's `rm -r` of the directory it made itself.
- If a check does not print what its comment says, stop at once and
  report. Do not rebuild, repair or retry. If your permission system
  refuses a command, do not work around it: report it and stop there. A
  kernel panic, oops, VERIFY or hung-task message means stop and report;
  never reboot.
- The `bg` run takes 15 to 25 minutes. If your shell tool cannot wait that
  long for a command in the foreground, start that one command with the
  tool's own background mode and wait until it exits; do not use `&` or
  `nohup`, and run nothing else meanwhile.

## STEP 0: the build half must be finished, and the VM free

First check that the build half has finished:

```sh
date -u +%T
for p in $(pgrep -f '[.]/autogen[.]sh|make -j[8]'); do
    echo "$p cwd=$(readlink /proc/$p/cwd) $(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-100)"
done
echo "build check done"
```

Expected: only the time and `build check done`. A listed process whose
`cwd` is under `/var/tmp/zts-rln-1026`, or whose command line is the build
half's (`./autogen.sh && ./configure --enable-debug && make -j8`), means
the build is still running: wait about 5 minutes and run the block again,
at most 12 times. Stop and report if it still lists something after an
hour, or if it lists anything else.

Then:

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rln-1026 L=/var/log/zts-rln-1026
id -un                        # root
uname -r                      # 6.12.113+deb13-cloud-amd64 (the kernel of the earlier runs on this VM)
nproc                         # 16
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zed      # nothing: no system ZFS
pgrep -x zed                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|raidz_test'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -A $W                      # B M repo
ls -A $L                      # build-B.log build-M.log, and maybe other logs of the build half
stat -c '%U %n' $W $W/repo $W/B $W/M $L   # root, five times
readlink -f $W                # /var/tmp/zts-rln-1026
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|zts-rln' /proc/mounts   # nothing
ls -d /testpool* 2>&1         # No such file or directory
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
ls -ld /var/tmp/test_results 2>&1
ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules 2>&1; ls -d /dev/zvol 2>&1
df -h /var/tmp                # about 52G free; need 12G
id zts; sudo -n -l -U zts | grep -i nopasswd
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
```

Keep this output, which goes into REPORT.txt. The last number is the
kernel-message baseline. The `60-zvol.rules` and `/dev/zvol` lines are only
recorded (see the `bg` row of the Expected block).

Stop at once, changing nothing, and report what you found if any of
these holds:

- you are not root, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, or `zed` runs;
- a test or build process is running. The `pgrep -af` also matches the
  shell that runs this block, because that shell's command line contains
  the pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` is missing or not root-owned; `$W` holds anything other than
  `B M repo`; `$L` lacks `build-B.log` or `build-M.log`, or already holds
  any of `env.sh`, `run.sh`, `guards.txt`, `results`, `step1.txt` or
  `REPORT.txt` (this task would then have started once already);
- `readlink -f $W` prints anything else;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load);
- less than 12 GB is free on `/var/tmp`;
- user `zts` is missing or has no passwordless sudo.

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself,
because STEP 1 checks that the modules were built for the running one.

## STEP 1: verify the preparation (all git work happens here, before any chown)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rln-1026 L=/var/log/zts-rln-1026
SHA_B=8a47085858d15c27f377542c90422a7ac7afcc5d
F1=tests/zfs-tests/tests/functional/rsend/recv_longname_disabled.ksh
F2=tests/runfiles/linux.run
F3=tests/zfs-tests/tests/Makefile.am
FIX='source == ZPROP_SRC_RECEIVED && intval == 0'
NOTE='Skipping ZFS_PROP_LONGNAME'
{
echo "B HEAD:   $(git -C $W/B rev-parse HEAD)"     # 8a47085858d15c27f377542c90422a7ac7afcc5d
echo "B HEAD~1: $(git -C $W/B rev-parse HEAD~1)"   # e117a84dbbfe0befe15110b9d1d799f47b1a5546
echo "M HEAD:   $(git -C $W/M rev-parse HEAD)"     # e117a84dbbfe0befe15110b9d1d799f47b1a5546
echo "B tracked changes: $(git -C $W/B status --short --untracked-files=no | wc -l)"   # 0
echo "B vs master:$(git -C $W/B diff --stat HEAD~1 HEAD | tail -n 1)"   #  4 files changed, 83 insertions(+), 16 deletions(-)
echo "B vs master, module:$(git -C $W/B diff --stat HEAD~1 HEAD -- module | tail -n 1)"   #  1 file changed, 10 insertions(+), 15 deletions(-)
echo "M tracked changes:"
git -C $W/M status --short --untracked-files=no
#   M  tests/runfiles/linux.run
#   M  tests/zfs-tests/tests/Makefile.am
#   A  tests/zfs-tests/tests/functional/rsend/recv_longname_disabled.ksh
git -C $W/M diff --cached --stat | tail -n 1    #  3 files changed, 73 insertions(+), 1 deletion(-)
echo "M index vs B, test files: $(git -C $W/M diff --cached $SHA_B -- $F1 $F2 $F3 | wc -l)"   # 0
echo "M unstaged: $(git -C $W/M diff --stat | wc -l)"   # 0
echo "M module/man/include/lib/cmd vs master: $(git -C $W/M diff --cached --stat HEAD -- module man include lib cmd | wc -l)"   # 0
echo "test mode: $(stat -c %a $W/B/$F1) $(stat -c %a $W/M/$F1)"   # 755 755
echo "test in linux.run: B $(grep -c recv_longname_disabled $W/B/$F2), M $(grep -c recv_longname_disabled $W/M/$F2)"   # B 1, M 1
echo "fix in zfs_ioctl.c: B $(grep -cF "$FIX" $W/B/module/zfs/zfs_ioctl.c), M $(grep -cF "$FIX" $W/M/module/zfs/zfs_ioctl.c)"   # B 1, M 0
echo "skip notice in zfs_ioctl.c: B $(grep -cF "$NOTE" $W/B/module/zfs/zfs_ioctl.c), M $(grep -cF "$NOTE" $W/M/module/zfs/zfs_ioctl.c)"   # B 0, M 1
for t in B M; do
    echo "build-$t.log errors: $(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)"   # 0
    tail -n 2 $L/build-$t.log
    echo "$t debug build: $(grep -c '^#define ZFS_DEBUG 1' $W/$t/zfs_config.h)"   # 1
    ls -l $W/$t/module/spl.ko $W/$t/module/zfs.ko $W/$t/zpool $W/$t/zfs $W/$t/zstream \
        $W/$t/scripts/zfs.sh $W/$t/scripts/zfs-tests.sh \
        $W/$t/tests/test-runner/bin/test-runner.py \
        $W/$t/tests/zfs-tests/tests/functional/rsend/setup.ksh
    echo "$t srcversion $(modinfo -F srcversion $W/$t/module/zfs.ko)"
    echo "$t vermagic $(modinfo -F vermagic $W/$t/module/zfs.ko | cut -d' ' -f1) running $(uname -r)"
done
} 2>&1 | tee $L/step1.txt
```

Expected values:

- the full SHAs, counts and status lines in the comments above;
- for each build log, 0 errors, and a last line of a finished build
  (typically `make[1]: Leaving directory '/var/tmp/zts-rln-1026/<B or M>'`,
  plus any closing line the build half added); no `Error` in the tail;
- M srcversion `8852710DB871A7587B9E722` (the value of every earlier build
  of `e117a84dbb` on this VM, since the test files do not reach the
  module), and a B srcversion that differs from it, because
  `zfs_ioctl.c` differs. B's value is not known in advance: STEP 2 reads it
  from the tree;
- each vermagic equal to the running kernel;
- every `ls -l` path present.

If anything else prints, stop and report. Do not rebuild.

## STEP 2: harness setup

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rln-1026 L=/var/log/zts-rln-1026
for t in B M; do
    sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
        $W/$t/tests/test-runner/bin/test-runner.py.in \
        $W/$t/tests/test-runner/bin/test-runner.py &&
    grep -n '^BASEDIR' $W/$t/tests/test-runner/bin/test-runner.py
done                          # each: 43:BASEDIR = '/var/log/zts-rln-1026/results', else stop
mkdir $W/files $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
chown -R zts $W $L/results; echo "chown rc=$?"   # 0
stat -c '%U %a %n' $W/files $L/results   # zts 1777 /var/tmp/zts-rln-1026/files, then zts 755 /var/log/zts-rln-1026/results
```

The ZTS `-d` directory must be mode 1777: the tests write there both as
root and as `zts`.

Then create the environment file, owned by root:

```sh
cat > /var/log/zts-rln-1026/env.sh <<'EOF'
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rln-1026
L=/var/log/zts-rln-1026
T=recv_longname_disabled
# Kernel findings. KIGN drops the ZTS -K markers (test names such as
# send-c_verify_ratio match VERIFY) and the import progress notes.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The lines of recv_longname_disabled worth quoting.
NPAT='ASSERTION:|SUCCESS: (zfs (create|snapshot)|touch|eval zfs (send|recv)|stream_has_features|zpool (create|sync))|ERROR: |has longname=|feature@longname is |cannot (receive|touch)|Skipping ZFS_PROP_LONGNAME'
# The tunables the rsend tests change and must restore.
TUN='zfs_compressed_arc_enabled zfs_disable_ivset_guid_check zfs_recv_defer_batch_size send_holes_without_birth_time zap_micro_max_size'
tunables() {
	for p in $TUN; do
		printf '%s=%s ' "$p" "$(cat /sys/module/zfs/parameters/$p 2>&1)"
	done
}
EOF
. /var/log/zts-rln-1026/env.sh
svb=$(modinfo -F srcversion $W/B/module/zfs.ko)
svm=$(modinfo -F srcversion $W/M/module/zfs.ko)
echo "srcversions read from the trees: B $svb, M $svm"
if [ -n "$svb" ] && [ -n "$svm" ] && [ "$svb" != "$svm" ] &&
    [ "$svm" = 8852710DB871A7587B9E722 ]; then
    printf 'SV_B=%s\nSV_M=%s\n' "$svb" "$svm" >> $L/env.sh && echo "srcversions recorded"
else
    echo "SRCVERSION CHECK FAILED: stop"
fi
tail -n 2 $L/env.sh
```

Expected: `srcversions read from the trees: B <B's>, M
8852710DB871A7587B9E722`, `srcversions recorded`, then the two lines
`SV_B=<B's>` and `SV_M=8852710DB871A7587B9E722`. On `SRCVERSION CHECK
FAILED`, stop: the two modules are the same, or M is not master's.

Then create the per-run script, also in `$L` and owned by root:

```sh
cat > /var/log/zts-rln-1026/run.sh <<'EOS'
#!/bin/sh
# usage: sh /var/log/zts-rln-1026/run.sh <B|M> <label> <test name|GROUP> [debug]
# One run of one rsend test (-t) or of the rsend group (-T rsend) on the
# loaded tree, then its kernel messages and leftovers.
. /var/log/zts-rln-1026/env.sh
t=$1; lbl=$2; what=$3; dbg=$4
G=$L/guards.txt
res=""; cnt=""
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> <test name|GROUP> [debug]"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
case $dbg in
''|debug) ;;
*) echo "the 4th argument is either absent or the word debug"; exit 2 ;;
esac
case $what in
GROUP) sel="-T rsend"; outer=3600 ;;
''|*[!A-Za-z0-9_-]*) echo "bad test name: '$what'"; exit 2 ;;
*)
	if [ ! -x "$W/$t/tests/zfs-tests/tests/functional/rsend/$what.ksh" ]; then
		echo "no such test in $t: rsend/$what.ksh"; exit 2
	fi
	sel="-t tests/functional/rsend/$what.ksh"; outer=1800 ;;
esac
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi

# The result file of one test: its "merged" file, else its "stdout" (a
# test that writes nothing on stderr has no "merged" file).
outfile() {   # outfile <results dir> <test name>
	for f in merged stdout; do
		o=$(find "$1" -type f -name "$f" 2>/dev/null |
		    grep -E "/rsend(:Linux)?/$2([.]ksh)?/$f\$" | head -n 1)
		if [ -n "$o" ]; then
			echo "$o"; return 0
		fi
	done
	return 1
}

# Guards before the run. The loaded module comes first, so that no zpool
# command runs without this tree's module; then the tunables as found
# after the load, no pool, an empty work directory, no loop device, no
# test mount and no test mount point.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion=$cur want=$sv:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(ls -A "$W/files" 2>&1)
loops=$(losetup -a 2>&1)
mnts=$(grep -E ' zfs |testpool' /proc/mounts)
roots=$(ls -d /testpool /testpool1 /testpool2 /testpool3 2>/dev/null)
echo "$(date -u +%T) before $lbl ($t $what${dbg:+ $dbg}): srcversion=$cur" \
    "pools=[$pools] files=[$files] loops=[$loops] mounts=[$mnts]" \
    "rootdirs=[$roots]" | tee -a "$G"
if [ "$tun" != "$(cat "$L/tun-$t.txt" 2>&1)" ] ||
    [ -n "$pools$files$loops$mnts$roots" ]; then
	echo "GUARD FAILED before $lbl: tunables=[$tun]: nothing run" | tee -a "$G"
	exit 1
fi

# The run, under nice and ionice; "debug" adds LOGAPI_DEBUG=1, which makes
# log_must print the stderr of the commands that succeed.
if [ "$dbg" = debug ]; then
	set -- env LOGAPI_DEBUG=1
else
	set --
fi
start=$(date +%s)
cd "$W/$t" || exit 1
# $sel is two words on purpose.
# shellcheck disable=SC2086
timeout -k 60 "$outer" nice -n 10 ionice -c3 "$@" \
    runuser -u zts -- ./scripts/zfs-tests.sh -vKO -s 3GB -d "$W/files" \
    $sel > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
# Two runs started in the same second would share a results directory.
sleep 2

# Results. Every "Test:" line starts with a timestamp, which is dropped;
# a -t run names the test with ".ksh", a -T run without it, and the
# linux.run tests are "Test (Linux):".
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
if [ "$what" != GROUP ]; then
	res=$(grep -E "/rsend/${what}[.]ksh [(]run as " "$L/$lbl-tests.txt" |
	    grep -oE '\[[A-Z]+\]$')
	if [ "$what" = "$T" ]; then
		npat=$NPAT
	else
		npat='ERROR: '
	fi
	out=""
	if [ -n "$rd" ] && [ -d "$rd" ]; then
		out=$(outfile "$rd" "$what")
	fi
	{
	echo "$lbl: tree $t, rsend/$what${dbg:+ ($dbg)}, result ${res:-none}," \
	    "zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
	echo "${miss:-no Missing util(s) line}"
	cat "$L/$lbl-tests.txt"
	if [ -n "$out" ]; then
		cp "$out" "$L/$lbl-out.txt"
		grep -E "$npat" "$out" | sed -E 's/a{512}/<512 x a>/' | head -n 40
		awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
			print "end line: " prev; exit }
		    NF { prev = $0 }' "$out"
	else
		echo "no stdout or merged file found for the test"
	fi
	} > "$L/$lbl-notes.txt"
else
	cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
	    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
	grep -vE '\[PASS\]$' "$L/$lbl-tests.txt" > "$L/$lbl-notpassed.txt"
	grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-notpassed.txt" |
	    sed -E 's|^.*/rsend/([^ /]+) [(]run as .*$|\1|; s|[.]ksh$||' \
	    > "$L/$lbl-failed.txt"
	{
	echo "$lbl: tree $t, -T rsend, $(wc -l < "$L/$lbl-tests.txt") results:" \
	    "${cnt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
	echo "${miss:-no Missing util(s) line}"
	sed -n '/^Results Summary/,/^Percent passed/p' "$L/$lbl.log"
	echo "not passed ($(wc -l < "$L/$lbl-notpassed.txt")):"
	sed 's/^/  /' "$L/$lbl-notpassed.txt"
	sed -n '/^Tests with /,$p' "$L/$lbl.log"
	for n in $(cat "$L/$lbl-failed.txt"); do
		o=""
		if [ -n "$rd" ] && [ -d "$rd" ]; then
			o=$(outfile "$rd" "$n")
		fi
		if [ -n "$o" ]; then
			cp "$o" "$L/$lbl-$n-out.txt"
			echo "$n:"
			grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
			awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
				print "  end line: " prev; exit }
			    NF { prev = $0 }' "$o"
		else
			echo "$n: no stdout or merged file found"
		fi
	done
	} > "$L/$lbl-notes.txt"
fi

# Kernel messages of this run (ZTS clears dmesg after a failure, the
# journal keeps them). "ZTS run" is the -K marker: it proves the journal
# saw the run. The longname notice is M's skipped check.
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
kn=$(grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")
kl=$(grep -c 'Skipping ZFS_PROP_LONGNAME checks' "$L/$lbl-kernel.txt")

# Leftovers: all must be empty, and the tunables as after the load.
lp=$("$W/$t/zpool" list -H -o name 2>&1)
ll=$(losetup -a 2>&1)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(ls -A "$W/files" 2>&1)
lm=$(grep -E ' zfs |testpool' /proc/mounts)
lr=$(ls -d /testpool /testpool1 /testpool2 /testpool3 2>/dev/null)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
# Leftover processes, leaving out this script and the shells that started
# it: their command lines can contain the same words.
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests[.]sh|test-runner[.]py|functional/rsend/' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)

echo "$(date -u +%T) after $lbl: result=${res:-${cnt:-none}} rc=$rc wall=${wall}s" \
    "zts_marks=$km kernel_matches=$kn longname_notes=$kl" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] loops=[$ll] dm=[$ld]" \
    "files=[$lf] mounts=[$lm] rootdirs=[$lr] procs=[$lq] tunables_same=$ts" |
    tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ "$kn" -gt 0 ]; then
	grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vE "$KIGN"
fi
if [ -n "$lp$ll$ld$lf$lm$lr$lq" ] || [ "$ts" != yes ] ||
    [ "$kn" -gt 0 ] || [ "$km" -lt 1 ] || [ "$rc" -eq 124 ] ||
    [ "$rc" -eq 137 ]; then
	echo "STOP after $lbl: leftover, tunable changed, kernel message," \
	    "no ZTS mark in the journal, or outer timeout" | tee -a "$G"
	exit 3
fi
exit 0
EOS
bash -n /var/log/zts-rln-1026/run.sh; echo "bash syntax rc=$?"   # 0
sh -n /var/log/zts-rln-1026/run.sh; echo "sh syntax rc=$?"       # 0
```

`run.sh` exits 0 when the run is clean, whatever the test result. It
exits 1 when a guard before the run failed (nothing ran), 2 on a bad
argument, and 3 when something is wrong after the run. On 1, 2 or 3, stop
and report; clean nothing by hand.

## STEP 3: state B

```sh
. /var/log/zts-rln-1026/env.sh
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"         # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ] && { $W/B/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
# zfs_compressed_arc_enabled=1 zfs_disable_ivset_guid_check=0 zfs_recv_defer_batch_size=33554432 send_holes_without_birth_time=1 zap_micro_max_size=131072
```

Then run b1, b2, b3 and bd, one at a time. Go on to the next only if the
previous one printed `run.sh rc=0`:

```sh
sh /var/log/zts-rln-1026/run.sh B b1 recv_longname_disabled; echo "run.sh rc=$?"
sh /var/log/zts-rln-1026/run.sh B b2 recv_longname_disabled; echo "run.sh rc=$?"
sh /var/log/zts-rln-1026/run.sh B b3 recv_longname_disabled; echo "run.sh rc=$?"
sh /var/log/zts-rln-1026/run.sh B bd recv_longname_disabled debug; echo "run.sh rc=$?"
```

Each takes about a minute; the runner kills a test at 600 s. Then, once,
the rsend group (15 to 25 minutes; see the last rule about long commands):

```sh
sh /var/log/zts-rln-1026/run.sh B bg GROUP > /var/log/zts-rln-1026/bg-console.txt 2>&1; echo "run.sh rc=$?" >> /var/log/zts-rln-1026/bg-console.txt
cat /var/log/zts-rln-1026/bg-console.txt
```

## STEP 4: state M

Unload B only while `/sys/module/zfs/srcversion` is still B's, then load
M:

```sh
. /var/log/zts-rln-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_B" ] && cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
```

If `unload rc` is not 0 or `lsmod` prints anything, stop. Otherwise:

```sh
. /var/log/zts-rln-1026/env.sh
cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_M $SV_M"         # 8852710DB871A7587B9E722 twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ] && { $W/M/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo                    # as in STEP 3
cmp $L/tun-B.txt $L/tun-M.txt && echo "same tunables as B"          # same tunables as B
```

```sh
sh /var/log/zts-rln-1026/run.sh M m1 recv_longname_disabled; echo "run.sh rc=$?"
sh /var/log/zts-rln-1026/run.sh M m2 recv_longname_disabled; echo "run.sh rc=$?"
sh /var/log/zts-rln-1026/run.sh M m3 recv_longname_disabled; echo "run.sh rc=$?"
```

Then, only if `bg` ran, rerun once on M each test that ended FAIL or
KILLED in `bg` (other than `setup`, `cleanup` and `recv_longname_disabled`
itself, which m1-m3 cover), at most 6. This tells a failure that master
has as well from one the branch brings. If the list is empty, the block
only prints `rerun on M: 0`; if it is long, run it like `bg`:

```sh
. /var/log/zts-rln-1026/env.sh
{
grep -vxE "setup|cleanup|$T" $L/bg-failed.txt > $L/mg-list.txt
echo "bg FAIL or KILLED: $(wc -l < $L/bg-failed.txt); rerun on M: $(wc -l < $L/mg-list.txt)"
i=0
for n in $(cat $L/mg-list.txt); do
    i=$((i + 1))
    if [ $i -gt 6 ]; then
        echo "more than 6: the rest is not rerun"
        break
    fi
    echo "mg$i = $n"
    sh $L/run.sh M mg$i "$n"; rc=$?
    echo "mg$i run.sh rc=$rc"
    [ $rc -eq 0 ] || break
done
} > $L/mg-console.txt 2>&1
cat $L/mg-console.txt
```

## STEP 4b: separate observation on M, master's `zpool remove -n` with raidz

Run this only if every M run so far printed `run.sh rc=0`, and while M is
still loaded. M's userspace is master's (only test files differ), so its
in-tree `zpool` shows what master prints. The block makes a pool
`drytest` (a name ZTS never uses) on three 1 GiB sparse files in a fresh
directory under `$W`: one plain vdev and one raidz vdev. It then asks for a
dry run of removing the plain vdev, then for the removal itself, destroys
the pool and removes the directory. The pool is created with
`-m none -o cachefile=none`, so that nothing is mounted under `/` and
`/etc/zfs/zpool.cache` is not touched; neither option takes part in the
removal checks. The guards: M's module must be the loaded one, and no
pool may exist before `zpool create` (else the step is skipped); if
`mktemp`, `truncate` or `zpool create` fails, the remove commands are
skipped, but whatever was created is still destroyed and removed. The
kernel log is checked afterwards as after each run.

```sh
. /var/log/zts-rln-1026/env.sh
zp() { nice -n 10 ionice -c3 "$W/M/zpool" "$@"; }
start=$(date +%s)
{
echo "dry step on M, $(date -u +%T)"
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$SV_M" ]; then
    echo "SKIPPED: the loaded srcversion '$cur' is not M's ($SV_M)"
elif [ -n "$(zp list -H -o name 2>&1)" ]; then
    echo "SKIPPED: a pool exists before zpool create:"
    zp list -H -o name 2>&1
else
    dir=$(mktemp -d "$W/dry.XXXXXX") || dir=""
    echo "dir=[$dir]"
    if [ -n "$dir" ] && truncate -s 1g "$dir/d1" "$dir/d2" "$dir/d3"; then
        if zp create -f -m none -o cachefile=none drytest "$dir/d1" raidz "$dir/d2" "$dir/d3"; then
            echo "zpool create exit status 0"
            zp status -P drytest
            zp remove -n drytest "$dir/d1"; echo "exit status $?"
            zp remove drytest "$dir/d1"; echo "exit status $?"
        else
            echo "zpool create failed: the remove commands are skipped"
        fi
    else
        echo "mktemp or truncate failed: no pool created"
    fi
    if zp list -H -o name drytest > /dev/null 2>&1; then
        zp destroy drytest; echo "zpool destroy exit status $?"
    fi
    case $dir in
    "$W"/dry.*) rm -r "$dir"; echo "rm exit status $?" ;;
    esac
    journalctl -k --since "@$start" --no-pager -q > $L/dry-kernel.txt 2>&1
    echo "dry kernel_matches=$(grep -iE "$KPAT" $L/dry-kernel.txt | grep -vcE "$KIGN")"
    grep -iE "$KPAT" $L/dry-kernel.txt | grep -vE "$KIGN"
    echo "dry leftovers: pools=[$(zp list -H -o name 2>&1)] dirs=[$(ls -d "$W"/dry.* 2>/dev/null)]" \
        "mounts=[$(grep -E ' zfs |drytest' /proc/mounts)] rootdir=[$(ls -d /drytest 2>/dev/null)]" \
        "loops=[$(losetup -a 2>&1)]"
fi
} 2>&1 | tee $L/dry.txt
```

Expected, from reading master's code: the dry run prints an estimate and
exits 0, without checking that a pool with a raidz vdev cannot remove a
vdev; the removal itself is refused.

```
dry step on M, <time>
dir=[/var/tmp/zts-rln-1026/dry.<X>]
zpool create exit status 0
<zpool status -P drytest: .../dry.<X>/d1 on its own, then raidz1-1 with .../d2 and .../d3>
Memory that will be used after removing /var/tmp/zts-rln-1026/dry.<X>/d1: <size>
exit status 0
<the refusal, as printed; it should name raidz>
exit status 1
zpool destroy exit status 0
rm exit status 0
dry kernel_matches=0
dry leftovers: pools=[] dirs=[] mounts=[] rootdir=[] loops=[]
```

Quote `dry.txt` whole in the report, the refusal exactly as printed.
Whatever it shows, it changes nothing in the verdict of the recv-longname
runs. A `dry kernel_matches` above 0, or any leftover, is a stop as after
a run: report it, and do not unload in STEP 6.

## Expected

This block goes into REPORT.txt as written.

```
Every run: run.sh rc=0; zts_marks >= 1 (3 for one test: setup, test and cleanup; about 89 for bg);
kernel_matches=0; every leftover empty; tunables_same=yes.
"Missing util(s):" must not list zstream.
<n> below is the third loop device ZTS made (loop2 when no other loop device exists).

B (b1, b2, b3), each, zfs-tests.sh rc=0, longname_notes=0:
  Test: /var/tmp/zts-rln-1026/B/tests/zfs-tests/tests/functional/rsend/setup (run as root) [mm:ss] [PASS]
  Test: /var/tmp/zts-rln-1026/B/tests/zfs-tests/tests/functional/rsend/recv_longname_disabled.ksh (run as root) [mm:ss] [PASS]
  Test: /var/tmp/zts-rln-1026/B/tests/zfs-tests/tests/functional/rsend/cleanup (run as root) [mm:ss] [PASS]
  ASSERTION: A received longname=on needs feature@longname.
  SUCCESS: zfs create -o longname=on testpool/longname_disabled
  SUCCESS: touch /testpool/longname_disabled/shortname
  SUCCESS: zfs snapshot testpool/longname_disabled@snap
  SUCCESS: eval zfs send -p testpool/longname_disabled@snap >/var/tmp/zts-rln-1026/files/backdir-rsend/stream
  SUCCESS: stream_has_features /var/tmp/zts-rln-1026/files/backdir-rsend/stream longname exited 1
  SUCCESS: zpool create -o feature@longname=disabled testpool3 loop<n>
  SUCCESS: eval zfs recv testpool3/longname_disabled </var/tmp/zts-rln-1026/files/backdir-rsend/stream
  SUCCESS: touch /testpool3/longname_disabled/file-<512 x a> exited 1
  SUCCESS: zpool sync testpool3
  SUCCESS: zpool create testpool3 loop<n>          (the test's cleanup)
  end line: A received longname=on needs feature@longname.
  No "ERROR:" line and no "has longname=" line.

bd: as b1, plus two lines that LOGAPI_DEBUG=1 prints (stderr of commands that succeeded):
  cannot receive longname property on testpool3/longname_disabled: pool and or dataset must be upgraded to set this property or value
                                                   (just before "SUCCESS: eval zfs recv ...")
  touch: cannot touch '/testpool3/longname_disabled/file-<512 x a>': File name too long
  If neither line is there, the variable did not reach the test: report "message not shown"; it is not a stop.

bg (B, -T rsend, once), zfs-tests.sh rc=0, longname_notes=0:
  bg: tree B, -T rsend, 89 results: PASS 88, SKIP 1; zfs-tests.sh rc=0, wall <s>s
  Results Summary
  PASS      88
  SKIP       1
  Running Time:  00:<mm>:<ss>
  Percent passed:  98.9%
  not passed (1):
    Test: /var/tmp/zts-rln-1026/B/tests/zfs-tests/tests/functional/rsend/rsend_008_pos (run as root) [00:00] [SKIP]
  Tests with results other than PASS that are expected:
      SKIP rsend/rsend_008_pos (... 6066)
  no test under "Tests with results other than PASS that are unexpected:"
  The fork's CI gave exactly this on this head, in 14 minutes. If this VM has no 60-zvol.rules
  (STEP 0), /dev/zvol/... links never appear, and the four tests that use them
  (recv_dedup_encrypted_zvol, send-c_stream_size_estimate, send-c_volume, send-wR_encrypted_zvol)
  may FAIL; mg1... then shows whether master fails them too.

M (m1, m2, m3), each, zfs-tests.sh rc=1, longname_notes=1:
  Test: /var/tmp/zts-rln-1026/M/tests/zfs-tests/tests/functional/rsend/setup (run as root) [mm:ss] [PASS]
  Test: /var/tmp/zts-rln-1026/M/tests/zfs-tests/tests/functional/rsend/recv_longname_disabled.ksh (run as root) [mm:ss] [FAIL]
  Test: /var/tmp/zts-rln-1026/M/tests/zfs-tests/tests/functional/rsend/cleanup (run as root) [mm:ss] [PASS]
  ASSERTION and the same seven SUCCESS lines as B, from "zfs create" to "eval zfs recv"
  testpool3/longname_disabled has longname=on with feature@longname disabled      (test lines 62-63)
  <time> NOTICE: Skipping ZFS_PROP_LONGNAME checks for dsname=testpool3/longname_disabled
                                                   (in the dmesg dump that ZTS adds after a failure)
  SUCCESS: zpool create testpool3 loop<n>          (the test's cleanup)
  end line: testpool3/longname_disabled has longname=on with feature@longname disabled
  No "SUCCESS: touch /testpool3/..." line and no "SUCCESS: zpool sync" line: lines 65-71 never run,
  no long name is created, feature@longname is never activated, and nothing panics.
  <label>-out.txt also holds the dbgmsg and dmesg dumps that ZTS adds after a failure: they are
  findings only if they show an assertion, a panic or a hung task.

mg1... : only for a bg FAIL or KILLED. A test that fails on M too fails without the branch
(master's or this VM's); one that passes on M fails because of the branch.
```

## STEP 5: what each result means

- **Failing-first is shown** when B passes 3 of 3, M fails 3 of 3, both as
  in the Expected block: M ends at lines 62-63 with
  `testpool3/longname_disabled has longname=on with feature@longname
  disabled` and logs the `Skipping ZFS_PROP_LONGNAME checks` notice once per
  run, B logs none and keeps `longname=off`, and no run has a kernel
  finding. Then the kernel confirms that master stores a received
  `longname=on` on a pool without the feature, and that the fix refuses it.
- **The fix, or the test, is contradicted** by any of these:
  - **any B run fails** (b1-b3, bd, or `recv_longname_disabled` inside
    `bg`): report its `end line` and every quoted line;
  - **any M run passes:** master refused the received "on" on this VM, so
    the test does not exercise the bug here;
  - **an M run fails anywhere but lines 62-63**, for instance with
    `has longname=` absent and an `ERROR:` line on `stream_has_features`
    (the stream carried the longname flag) or on `zfs recv`: report the
    line; it is not the bug.
- **A run is inconclusive** when `run.sh` exits 0 but the run shows
  `[KILLED]`, `[SKIP]` or `[UNSUPPORTED]` for `recv_longname_disabled`, or
  a failed `setup`, or an `ERROR:` line before `eval zfs recv` (pool
  creation, `zfs create`, `touch`, `zfs send`). Report it as inconclusive.
  You may rerun such a run with a new label (b4, m4, ...), at most twice
  per state. A `run.sh` exit of 1, 2 or 3 is not inconclusive: it is a
  stop, and nothing more runs.
- **`bg`:** PASS 88, SKIP 1 (`rsend_008_pos`) is the expected result. For
  each other test that fails, its `mg` rerun decides: failing on M too,
  it is not the branch's (say so, with the `60-zvol.rules` line of STEP 0
  for the four zvol tests); passing on M, it is a finding against the
  branch, with its `end line` and `ERROR:` lines. A `setup` or `cleanup`
  that fails makes the group inconclusive.
- **`bd`:** the `cannot receive longname property ...` line, when present,
  is the message a user sees on such a receive with the fix; quote it
  exactly. Its absence only means `LOGAPI_DEBUG` did not reach the test.
- **A kernel VERIFY, panic or hung task in any run** is a stop, whatever
  the test result: report the lines and the state it happened in.

## STEP 6: unload, report, cleanup

Unload only when no run (nor STEP 4b) stopped with a leftover pool, loop
device, mount or process, and none had a kernel finding (a busy or wedged
module must not be forced out). Otherwise skip this block, keep everything, and say
why in the report:

```sh
. /var/log/zts-rln-1026/env.sh
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ "$cur" = "$SV_M" ]; then t=M; elif [ "$cur" = "$SV_B" ]; then t=B; else t=""; fi
if [ -n "$t" ]; then
    (cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # unload M rc=0
else
    echo "no module of this task is loaded (srcversion '$cur')"
fi
lsmod | grep -E '^(zfs|spl) '                                       # nothing
journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN"   # the STEP 0 baseline
ls -l /etc/zfs/zpool.cache 2>&1                                     # as in STEP 0, or a 0-byte file
ls -ld /var/tmp/test_results 2>&1                                   # as in STEP 0
ls -d /testpool* 2>&1; losetup -a                                   # No such file or directory, then nothing
df -h /var/tmp
```

Write `/var/log/zts-rln-1026/REPORT.txt` with:

- STEP 0 output (short), `uname -r`, the `60-zvol.rules` and
  `/etc/zfs/zpool.cache` lines, and the kernel baseline;
- `step1.txt` whole: the two HEADs, every STEP 1 check and the two
  srcversions;
- the STEP 2 output, and `env.sh` from the line `SV_B=` on;
- the load checks of STEP 3 and STEP 4 (with both tunables lines), and
  `guards.txt` whole;
- for each run (b1, b2, b3, bd, bg, m1, m2, m3, every mg and any rerun):
  its result, wall time and the whole `<label>-notes.txt`; for `bg`, also
  `bg-tests.txt` whole, and the `mg` mapping from `mg-console.txt`;
- kernel findings per run, `longname_notes` per run, and the final
  journal count against the STEP 0 baseline;
- `dry.txt` whole (STEP 4b), or why the step was skipped, under its own
  heading, apart from the recv-longname results;
- any leftover, and the STEP 6 output;
- the "Expected" block above, then for each state whether it matched, and
  which STEP 5 case applies; for `bd`, the `cannot receive` line exactly as
  printed, or "message not shown";
- every deviation from these instructions, with its reason.

Then print it with `cat /var/log/zts-rln-1026/REPORT.txt` and copy it
whole into your reply. Only after that, as the very last command, and only
if the unload block ran and printed `rc=0` with nothing left in `lsmod`,
remove the two directories of this task:

```sh
rm -rf /var/tmp/zts-rln-1026 /var/log/zts-rln-1026; echo "rm rc=$?"
ls -d /var/tmp/zts-rln-1026 /var/log/zts-rln-1026 2>&1   # both: No such file or directory
```

If the permission system refuses this `rm -rf`, leave both directories
as they are and say so in the reply. Do not delete them any other way.

Estimated duration: 40 to 60 minutes (checks 5, four B runs about 5, the
rsend group 15 to 25, three M runs about 3, any `mg` reruns 1 to 2 each,
STEP 4b 1, report and cleanup 5).
