# VM task: ztest without redundancy, master against the scrub fix (9/11)

These instructions are for one test VM with 16 vCPUs, 8 GiB of memory
available, Debian 13 and 40 GiB free on `/var/tmp` (STEP 0 checks). VM 106
is the first choice, since it ran the replays where the failure showed;
VM 105 or VM 107 will do if they have that shape. Reply with
`/var/log/ztest-mf0-1109/REPORT.txt` (STEP 5). Nothing is posted, pushed or
installed, and no kernel module is built or loaded: ztest runs in user space.

**A functional test.** It counts failures and reads cores, it does not time
anything. It may run while one other VM runs a functional job (ztest, zloop,
ZTS, a replay), but never while a VM runs a performance test, since the
three VMs share one hypervisor and three concurrent ztests would skew a
measurement there. Nothing else runs on this VM meanwhile.

**Background.** VM-ZREP3-1108 ran this ztest command on master `ef0614e724`
and on a branch that does not touch ztest. Each tree failed about once in 13
runs with

```
ASSERT at cmd/ztest.c:6963:ztest_scrub()
VERIFY0(error) failed (52)
```

52 is `ECKSUM`. The read-out of two such cores (error log of about 51,000
and 54 bookmarks, an `injecting fault to vdev 2; maxfaults=0` line just
before, and `zdb -bcc` on copies of the pools finding 0 checksum or read
errors and no leak) points at ztest, not at the data:

- `-K raidz -m 0 -r 0 -R 2` gives one RAID-Z child, which clamps the parity
  to 0, and no mirror, so the pool has no redundancy and `MAXFAULTS()` is 0;
- `ztest_fault_inject()` still makes leaf 0 of a top-level vdev unreadable
  or unwritable, or closes its fd, counting on the pool to suspend and the
  I/O to be re-executed;
- `zio_done()` logs the block of every failed logical read or write in the
  pool's error log before it decides to re-execute, and scrub reads never
  suspend the pool, so a scrub that runs while the fault lasts logs every
  block of that leaf;
- `ztest_scrub_impl()` returns `ECKSUM` whenever the error log is not empty,
  and `ztest_scrub()` asserts on it.

The fix is fork branch `ztest-scrub-maxfaults0`, one commit on upstream
master `47d0a32206`: `ztest_scrub_impl()` treats the error log as damage
only when `MAXFAULTS()` is above 0. With no redundancy ztest injects no
damage, and the `zdb -bcc` run after each pass still checks every block.
On a 4-vCPU machine the exact command has not failed this way so far; with
an aid that makes every `ztest_scrub()` wait for its scrub and reach the
check, master did, after a closed fd, and `zdb -bcc` found that pool clean.
This task is the A/B without any aid: same command, both trees,
interleaved, with counts per failure class.

A third run of 8/11 failed with `ztest_dmu_read_write()
VERIFY0(error) failed (5)` (`EIO`) on a single-vdev pool whose vdev showed
54,487 checksum errors, clean on disk; its log had no injection line left.
The fix does not address it. This task counts it on both trees and keeps
its core, to see whether it comes with an injected fault.

**Builds.**

| tree | commit | what it is |
|---|---|---|
| `master` | `47d0a322063639ed85aebce0ca8dc3af2605a75d` | upstream master, the base of the branch |
| `branch` | `c78c3508f124044a2c255a455749e3bdbd82ab5e` | fork `ztest-scrub-maxfaults0`: "ztest: don't fail a scrub on error log entries without redundancy", `cmd/ztest.c` only |

Both are built as on 8/11: `./configure --enable-debug --with-config=user`,
`make -j8`.

**What runs.** STEP 3 runs `mf0.sh`: rounds of three ztests at once, every
one with exactly VM-ZREP3-1108's command:

```
cd <fresh scratch dir> && ulimit -c unlimited &&
nice -n 10 ionice -c3 timeout -k 30 900 <tree>/ztest \
    -G -VVVVV -K raidz -m 0 -r 0 -D 0 -S 0 -R 2 -v 4 -a 12 \
    -C special=random -s 512m -f <scratch dir> \
    -J 4abf7fd74d6647686cf92eb603a57b79782917664e25686c6ea806070cf154bd
```

Odd rounds run master, branch, master; even rounds branch, master, branch;
so over two rounds each tree gets three runs, under the same load. A round
ends when its three runs have ended. `-J` fixes the seeds but not the thread
timing, so no run repeats another.

`mf0.sh` writes one line per run to `runs.txt`, with a `class`:

| class | what the log shows |
|---|---|
| `pass` | ztest exited 0 |
| `ECKSUM` | `ASSERT at cmd/ztest.c:...:ztest_scrub()` or `ztest_run()`, then `VERIFY0(error) failed (52)`: the failure this task is about |
| `EIO` | an `ASSERT at cmd/ztest.c` ending in `failed (5)` |
| `DSOPEN` | `VERIFY0(ztest_dataset_open(i)) failed (28)`, as in VM-ZREP3-1108 |
| `zdb` | ztest's check after a pass: `ztest: '...zdb ...' exit code N` or `died with signal N` |
| `assert` | any other `ASSERT`/`VERIFY`, quoted after `\|` |
| `suspended` | ztest's deadman abort of a suspended pool |
| `timeout` | killed at the 15-minute cap |
| `other` | anything else |

An `ECKSUM` or `EIO` run keeps its scratch dir (the pool's files and the
cores) and gets the read-out (`readout.sh`) at once. Any other failing run
keeps its log and the backtrace of each core (`bt.sh`), and its scratch dir,
cores included, is deleted. A passing run keeps its log, compressed, and its
scratch dir is deleted. The rounds stop at the first of:

1. 95 minutes after the first round started: no round starts after that,
   and the round in progress finishes;
2. 30 rounds;
3. less than 10 GiB free on `/var/tmp` before a round;
4. less than 6 GiB of `MemAvailable` before a round (three ztests take
   about 1.7 GiB each, plus zdb after each pass).

**Duration**: STEP 0 2 min; STEP 1 about 12 min (two debug builds, one
after the other); STEP 2 1 min; STEP 3 about 1 h 45 min (95 min, then the
round in progress, at most 15.5 min, and its read-outs, a few minutes each);
STEPS 4 and 5 about 5 min. In all about 2 h 5 min, at most 2 h 20 min. A run
takes about 5 to 6 min (300 s of ztest plus zdb checks), so 95 minutes hold
about 16 rounds, about 24 runs per tree.

**Disk**: about 1 GiB for the two builds and the repository. A kept run
takes about 2 GiB (pool files and a core of about 1 GiB, as on 8/11). At
8/11's rate, about 2 `ECKSUM` runs on master, none on the branch, and
perhaps one `EIO`; 40 GiB free leaves room for well over ten, and rule 3
stops the rounds before the disk fills.

## Rules

- Disposable VM only, with nothing else running on it. User space only: no
  module is built or loaded, nothing runs `zpool` or `zfs`, and the only
  pools are ztest's own, in files under W.
- Two debug builds (`--enable-debug --with-config=user`), under `nice -n 10
  ionice -c3` with `make -j8`, as on 8/11.
- Post nothing, push nothing. Install nothing (no `make install`, no
  package). Never reboot.
- Work only under `/var/tmp/ztest-mf0-1109` (W) and `/var/log/ztest-mf0-1109`
  (L). The one change outside them is the core dump pattern, and only if it
  is not already `core` with `core_uses_pid` 1 (STEP 2); STEP 4 restores it.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed. ztest itself needs no root.
- Do not change the ztest command, rerun a run, or retry a failed step. If
  a check does not print what its comment says, stop and report.
- If `runs.txt` gets no new line for 45 minutes, record
  `ps -eo pid,stat,etime,wchan:32,cmd | grep -E 'ztest|zdb|gdb|xz' |
  grep -v grep` in `$L/stuck.txt`, then go to STEP 4 and say so in the
  REPORT. Kill nothing: every run is already under `timeout`.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.
- Keep everything under W at the end, the trees included: a core can only
  be read with the exact binaries and libraries of the tree that dumped it.
  The reply decides what to delete.

## STEP 0: the VM must have the right shape and be free

```sh
{
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"
uname -r; nproc; . /etc/os-release; echo "$PRETTY_NAME"
grep -E 'MemTotal|MemAvailable' /proc/meminfo
for t in git make gcc autoconf automake libtool gdb xz ionice timeout nice sha256sum awk find; do command -v $t > /dev/null || echo "missing $t"; done
echo "core pattern [$(cat /proc/sys/kernel/core_pattern)] uses_pid $(cat /proc/sys/kernel/core_uses_pid) hard limit $(ulimit -H -c)"
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zdb|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/ztest-mf0-1109 /var/log/ztest-mf0-1109 2>&1   # No such file or directory, twice
df -h /var/tmp /var/log | tail -n +2                         # 40G free on /var/tmp, 1G on /var/log
} 2>&1 | tee /var/tmp/ztest-mf0-1109-step0.txt
```

Stop, changing nothing, if: `sudo -n true` fails; `nproc` is not 16; the OS
is not Debian 13; MemAvailable is under 8 GiB; a tool is missing; the hard
core limit is not `unlimited`; a build, test or benchmark runs; W or L
exists; less than 40G is free on `/var/tmp` or less than 1G on `/var/log`.
The kernel version does not matter: nothing here uses the kernel's ZFS. Any
core pattern is fine here; STEP 2 deals with it.

## STEP 1: sources and debug builds (about 12 min)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/ztest-mf0-1109 L=/var/log/ztest-mf0-1109
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W || exit 1
mv /var/tmp/ztest-mf0-1109-step0.txt $L/step0.txt
cat > $L/env.sh <<'EOF2'
export PATH=/usr/sbin:/sbin:/usr/bin:/bin
W=/var/tmp/ztest-mf0-1109 L=/var/log/ztest-mf0-1109
M=47d0a322063639ed85aebce0ca8dc3af2605a75d
B=c78c3508f124044a2c255a455749e3bdbd82ab5e
EOF2
. $L/env.sh
{
export GIT_TERMINAL_PROMPT=0
git init -q $W/repo &&
timeout 300 git -C $W/repo fetch -q --depth 1 https://github.com/openzfs/zfs $M &&
timeout 300 git -C $W/repo fetch -q --depth 1 https://github.com/tigerblue77/zfs $B &&
git -C $W/repo worktree add -q --detach $W/master $M &&
git -C $W/repo worktree add -q --detach $W/branch $B || echo "fetch failed: stop"
for t in master branch; do git -C $W/$t log -1 --format="$t %H %s"; done
git -C $W/repo cat-file -p $B | grep '^parent' | xargs echo "branch"
git -C $W/repo diff --stat $M $B
sed -n 6963p $W/master/cmd/ztest.c | xargs echo "master ztest.c:6963:"
grep -n 'MAXFAULTS(ztest_shared) > 0' $W/master/cmd/ztest.c $W/branch/cmd/ztest.c
for t in master branch; do
    (cd $W/$t && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --enable-debug --with-config=user && make -j8' > $L/build-$t.log 2>&1
     echo "build $t rc=$? wall=$(( $(date +%s) - t0 ))s warnings $(grep -c 'warning:' $L/build-$t.log)")
    grep -c -- '--enable-debug' $W/$t/config.log | xargs echo "$t config.log lines with --enable-debug:"
    ls $W/$t/ztest $W/$t/zdb $W/$t/.libs/ztest $W/$t/.libs/zdb $W/$t/.libs/libzpool.so
done
sync -f $W
} 2>&1 | tee $L/step1.txt
```

Expected: `master 47d0a322063639ed85aebce0ca8dc3af2605a75d CI: Label PRs
with the priority picked when they are opened`; `branch
c78c3508f124044a2c255a455749e3bdbd82ab5e ztest: don't fail a scrub on error
log entries without redundancy`; `branch parent
47d0a322063639ed85aebce0ca8dc3af2605a75d`; a diff stat of `cmd/ztest.c |
8 +++++++-`, one file; `ASSERT0(error);` on line 6963 of master; one `grep`
hit, in `branch/cmd/ztest.c` only; `build ... rc=0` twice (8/11 had 0
warnings); 1 or more lines with `--enable-debug`, twice; the five paths
listed, twice. On a failed fetch or build quote `grep -nE 'error:|Error [0-9]'
$L/build-<tree>.log | head -20` and stop.

## STEP 2: core dumps and the scripts (1 min)

ztest's child process dumps its core in its working directory, the run's
scratch dir, as `core.<pid>`, when the pattern is `core` and `core_uses_pid`
is 1 (as on 8/11); zdb, run by ztest after each pass, does the same. Anything
else (a pipe to `systemd-coredump` or `apport`, a path) is set to that for
the task and restored in STEP 4; the old values are kept in
`core-settings-before.txt`. A test core then checks that a killed process
leaves one.

`mf0.sh` runs the rounds (see "What runs"). `bt.sh` names the program that
dumped a core and prints the first frames of the thread that crashed.
`readout.sh` is the read-out of an `ECKSUM` or `EIO` run: the log lines
around the failure, the configuration and error state out of the core
(`readout.gdb`), then `zdb -bcc` on the pool as the run left it.

```sh
. /var/log/ztest-mf0-1109/env.sh
{
{ cat /proc/sys/kernel/core_pattern; cat /proc/sys/kernel/core_uses_pid; } > $L/core-settings-before.txt
cat $L/core-settings-before.txt
[ "$(cat /proc/sys/kernel/core_pattern)" = core ] || echo core | sudo -n tee /proc/sys/kernel/core_pattern
[ "$(cat /proc/sys/kernel/core_uses_pid)" = 1 ] || echo 1 | sudo -n tee /proc/sys/kernel/core_uses_pid
echo "core pattern now [$(cat /proc/sys/kernel/core_pattern)] uses_pid $(cat /proc/sys/kernel/core_uses_pid)"
T=$(mktemp -d $W/coretest.XXXXXX)
(cd $T && ulimit -c unlimited && exec sleep 60) & p=$!
sleep 1; kill -ABRT $p; wait $p; echo "test process exit $? (134 expected)"
echo "test core:" $T/core*                             # <W>/coretest.<x>/core.<pid>
rm -rf $T

cat > $L/mf0.sh <<'EOF2'
#!/bin/bash
# mf0.sh: VM-ZTEST-MF0-1109's ztest command in rounds of three concurrent
# runs (master branch master, then branch master branch), until a stop
# rule holds.  One line per run in $L/runs.txt.  An ECKSUM or EIO run keeps
# its scratch dir and gets the read-out; another failure keeps its log and
# backtraces only.
exec < /dev/null
. /var/log/ztest-mf0-1109/env.sh
A=(-G -VVVVV -K raidz -m 0 -r 0 -D 0 -S 0 -R 2 -v 4 -a 12 -C special=random -s 512m)
J=4abf7fd74d6647686cf92eb603a57b79782917664e25686c6ea806070cf154bd
mkdir -p "$W/keep" "$W/logs" || exit 1

run1() {
	local t=$1 r=$2 k=$3 S lg s rc el as inj nc c K co
	S=$(mktemp -d "$W/scratch.XXXXXX") || return 1
	lg=$W/logs/$t-r$r-s$k.log s=$(date +%s)
	(cd "$S" && ulimit -c unlimited && nice -n 10 ionice -c3 \
	    timeout -k 30 900 "$W/$t/ztest" "${A[@]}" -f "$S" -J $J \
	    > "$lg" 2>&1)
	rc=$? el=$(( $(date +%s) - s ))
	as=$(grep -a -m1 -A1 '^ASSERT at' "$lg" | tr '\n' ' ' | cut -c1-200)
	inj=$(grep -a -c 'injecting fault to vdev' "$lg")
	nc=$(find "$S" -maxdepth 1 -name 'core*' | wc -l)
	if [ $rc = 0 ]; then c=pass
	elif echo "$as" | grep -q -E '^ASSERT at cmd/ztest\.c:[0-9]+:ztest_(scrub|run)\(\) VERIFY0\(error\) failed \(52\)'; then c=ECKSUM
	elif echo "$as" | grep -q -E '^ASSERT at cmd/ztest\.c:.* failed \(5\) $'; then c=EIO
	elif grep -a -q -F 'VERIFY0(ztest_dataset_open(i)) failed (28)' "$lg"; then c=DSOPEN
	elif grep -a -q -E "^ztest: '.*zdb .*' (exit code|died with signal)" "$lg"; then c=zdb
	elif [ -n "$as" ]; then c=assert
	elif grep -a -q 'aborting test after .* suspended' "$lg"; then c=suspended
	elif [ $rc = 124 ] || [ $rc = 137 ]; then c=timeout
	else c=other
	fi
	echo "r$r s$k $t rc=$rc ${el}s passes=$(grep -a -c '^Pass ' "$lg") inj=$inj cores=$nc class=$c${as:+ | $as}" >> "$L/runs.txt"
	if [ $rc = 0 ]; then
		xz -T2 "$lg"; rm -rf "$S"; return 0
	fi
	if [ $c = ECKSUM ] || [ $c = EIO ]; then
		K=$W/keep/$t-r$r-s$k
		mv "$S" "$K"
		for co in "$K"/core*; do
			[ -e "$co" ] && bash "$L/bt.sh" $t "$co" >> "$L/bt-$t-r$r-s$k.txt" 2>&1
		done
		bash "$L/readout.sh" $t "$K" "$lg" > "$L/readout-$t-r$r-s$k.txt" 2>&1
	else
		for co in "$S"/core*; do
			[ -e "$co" ] && bash "$L/bt.sh" $t "$co" >> "$L/bt-$t-r$r-s$k.txt" 2>&1
		done
		rm -rf "$S"
	fi
}

t0=$(date +%s) stop=
for r in $(seq 1 30); do
	if [ $(( $(date +%s) - t0 )) -ge 5700 ]; then
		stop="95 minutes"; break
	fi
	free=$(df -B1G --output=avail "$W" | tail -1 | tr -dc 0-9)
	if [ "${free:-0}" -lt 10 ]; then
		stop="under 10 GiB free on $W"; break
	fi
	mem=$(awk '/^MemAvailable:/ {print int($2 / 1048576)}' /proc/meminfo)
	if [ "${mem:-0}" -lt 6 ]; then
		stop="under 6 GiB MemAvailable"; break
	fi
	if [ $((r % 2)) = 1 ]; then trees="master branch master"; else trees="branch master branch"; fi
	echo "round $r: $trees, $(date -u '+%T'), ${free}G free, ${mem}G available" >> "$L/rounds.txt"
	k=0
	for t in $trees; do
		k=$((k + 1))
		run1 $t $r $k &
	done
	wait
	sync -f "$W"
done
echo "stop: ${stop:-30 rounds} after $(( ($(date +%s) - t0) / 60 )) min, $(grep -c . "$L/rounds.txt") rounds" | tee -a "$L/runs.txt"
EOF2

cat > $L/bt.sh <<'EOF2'
#!/bin/bash
# bt.sh <tree> <core>: the program that dumped the core, and the first
# frames of the thread that crashed.
. /var/log/ztest-mf0-1109/env.sh
t=$1 co=$2
g=$(timeout 120 gdb -q -batch -iex 'set debuginfod enabled off' -c "$co" 2>&1 | grep -m1 'Core was generated by')
case $g in
*/zdb*) b=$W/$t/.libs/zdb ;;
*) b=$W/$t/.libs/ztest ;;
esac
echo "== $co ($(du -h "$co" | cut -f1)): $g"
timeout 300 gdb -q -batch -iex 'set debuginfod enabled off' -ex 'bt 12' "$b" "$co" 2>&1 |
    grep -E '^#[0-9]+ ' | awk '!seen[$0]++' | cut -c1-200
EOF2

cat > $L/readout.gdb <<'EOF2'
set pagination off
print ztest_shared->zs_mirrors
print ztest_opts.zo_raid_children
print ztest_opts.zo_raid_parity
print ztest_pool_scrubbed
print ztest_spa->spa_errlist_last.avl_numnodes
print ztest_spa->spa_errlist_scrub.avl_numnodes
print ztest_spa->spa_errlog_last
print ztest_spa->spa_errlog_scrub
print ztest_spa->spa_scrub_finished
set $n = ztest_spa->spa_root_vdev->vdev_children
set $i = 0
while $i < $n
  set $v = ztest_spa->spa_root_vdev->vdev_child[$i]
  printf "top %d: %s state %d cant_read %d cant_write %d errors read %llu write %llu checksum %llu, %llu children\n", $i, $v->vdev_ops->vdev_op_type, $v->vdev_state, $v->vdev_cant_read, $v->vdev_cant_write, $v->vdev_stat.vs_read_errors, $v->vdev_stat.vs_write_errors, $v->vdev_stat.vs_checksum_errors, $v->vdev_children
  if $v->vdev_path != 0
    printf "  path %s\n", $v->vdev_path
  end
  set $j = 0
  while $j < $v->vdev_children
    set $c = $v->vdev_child[$j]
    printf "  child %d: %s state %d cant_read %d cant_write %d errors read %llu write %llu checksum %llu\n", $j, $c->vdev_ops->vdev_op_type, $c->vdev_state, $c->vdev_cant_read, $c->vdev_cant_write, $c->vdev_stat.vs_read_errors, $c->vdev_stat.vs_write_errors, $c->vdev_stat.vs_checksum_errors
    set $j = $j + 1
  end
  set $i = $i + 1
end
EOF2

cat > $L/readout.sh <<'EOF2'
#!/bin/bash
# readout.sh <tree> <kept dir> <ztest log>: the read-out of an ECKSUM or
# EIO run; see "Reading the result" in VM-ZTEST-MF0-1109.md.
. /var/log/ztest-mf0-1109/env.sh
t=$1 K=$2 LG=$3
echo "== read-out: tree $t, dir $K, log $LG"
echo "-- log: last passes, the failure, injections and scans"
grep -a -n -E '^Pass |^ASSERT at|failed \([0-9]+\)|injecting fault|scan (done|cancelled|aborted)|suspend' "$LG" | tail -40 | cut -c1-200
for co in "$K"/core*; do
	[ -e "$co" ] || continue
	case $(timeout 120 gdb -q -batch -iex 'set debuginfod enabled off' -c "$co" 2>&1 | grep -m1 'Core was generated by') in
	*/zdb*) echo "-- $co: a zdb core, no read-out"; continue ;;
	esac
	echo "-- core $co: \$1 mirrors, \$2 RAID-Z children, \$3 parity, \$4 ztest_pool_scrubbed, \$5 to \$9 the in-core error log, then the vdevs"
	timeout 300 gdb -q -batch -iex 'set debuginfod enabled off' -x "$L/readout.gdb" \
	    "$W/$t/.libs/ztest" "$co" 2>&1 | grep -E '^\$[0-9]+ =|^top |^  path |^  child '
done
echo "-- zdb -bcc on the pool as the run left it"
(ulimit -c 0; cd "$K" && timeout 1200 "$W/$t/zdb" -e -p "$K" -bcc ztest 2>&1) |
    grep -E -i 'leak|error|errno|ASSERT|VERIFY|bp count|block traversal' | cut -c1-120 | head -30
echo "zdb pipeline exit ${PIPESTATUS[0]}"
EOF2
chmod +x $L/mf0.sh $L/bt.sh $L/readout.sh
for f in mf0.sh bt.sh readout.sh; do bash -n $L/$f && echo "$f: syntax ok"; done
df -h /var/tmp | tail -1
} 2>&1 | tee $L/step2.txt
```

Expected: the old pattern and `uses_pid` value; `core pattern now [core]
uses_pid 1`; `test process exit 134 (134 expected)`; `test core:` and the
path of a `core.<pid>` file; `syntax ok` three times. A `test core:` line
that ends in `core*` means no core was written: stop (cores would be lost).

## STEP 3: the rounds (about 1 h 45 min)

```sh
. /var/log/ztest-mf0-1109/env.sh
bash $L/mf0.sh > $L/run.out 2>&1; tail -4 $L/runs.txt
```

The first three lines of `runs.txt` come after about 5 to 6 min, then three
about every 6 min (`rounds.txt` gets one line as each round starts). A
passing run reads like `r1 s2 branch rc=0 312s passes=7 inj=0 cores=0
class=pass` (`inj` counts the `injecting fault` lines, which ztest prints,
from its debug log, only when it crashes). The last line is `stop: <reason>
after <N> min, <R> rounds`.

## STEP 4: cleanup (about 2 min; keeps every kept run, log and tree)

```sh
. /var/log/ztest-mf0-1109/env.sh
{
pgrep -af 'ztest|zdb|gdb' | grep -v pgrep               # nothing
o=$(sed -n 1p $L/core-settings-before.txt); u=$(sed -n 2p $L/core-settings-before.txt)
[ "$(cat /proc/sys/kernel/core_pattern)" = "$o" ] || printf '%s\n' "$o" | sudo -n tee /proc/sys/kernel/core_pattern
[ "$(cat /proc/sys/kernel/core_uses_pid)" = "$u" ] || echo "$u" | sudo -n tee /proc/sys/kernel/core_uses_pid
echo "core pattern restored: [$(cat /proc/sys/kernel/core_pattern)] uses_pid $(cat /proc/sys/kernel/core_uses_pid)"
ls -d $W/scratch.* 2>&1 | head -3                       # none
sudo -n dmesg | grep -c -i -E 'out of memory|oom-kill' | xargs echo "OOM lines in the kernel log:"
du -sh $W/repo $W/master $W/branch $W/logs $W/keep
for d in $W/keep/*; do [ -d "$d" ] && echo "$(du -sh "$d" | cut -f1) ${d##*/}: $(ls "$d" | tr '\n' ' ' | cut -c1-200)"; done
for t in master branch; do
    for c in pass ECKSUM EIO DSOPEN zdb assert suspended timeout other; do
        n=$(grep -c " $t rc=.* class=$c\b" $L/runs.txt)
        [ $n = 0 ] || echo "$t $c $n"
    done
    echo "$t runs $(grep -c " $t rc=" $L/runs.txt)"
done
sudo -n fstrim -av
df -h /var/tmp | tail -1
} 2>&1 | tee $L/step4.txt
```

`core pattern restored` must show the values of `core-settings-before.txt`.
`OOM lines` should be 0; if not, say which runs ended with `rc=137` near
that time. Everything under W stays: the reply decides what to delete. The
VM is then idle.

## STEP 5: REPORT.txt

Write `/var/log/ztest-mf0-1109/REPORT.txt` yourself first, with, in this
order:

1. The stop line of `runs.txt`, and per tree the number of runs and the
   count per class (STEP 4 prints them), for example `master 24 runs: pass
   20, ECKSUM 2, zdb 1, assert 1; branch 24 runs: pass 22, assert 2`.
2. For each `ECKSUM` or `EIO` run (tree, round, slot): the `ASSERT` text;
   the injection lines of the log, with how many lines before the failure
   the last one comes; `$1` to `$9` of the core, each named (mirrors, RAID-Z
   children, parity, `ztest_pool_scrubbed`, the two in-core error lists, the
   two error log objects, `spa_scrub_finished`); each top-level vdev line
   and child line whose state is not 7 or whose error counts are not all 0;
   and zdb's verdict: `No leaks` or the leak lines, the `Error counts` table
   if any, and its exit.
3. One line per other failing run: tree, round, slot, `class`, the `ASSERT`
   text or the log's failure line, which program dumped the core (ztest or
   zdb) and its size, the first three function names of its backtrace, and
   which known failure it is, if one: the `vdev_removal.c:1574` VERIFY, the
   `zil.c:873` ASSERT in `zil_free_lwb()`, the `ztest_dataset_open` ENOSPC,
   the `range_tree.c:663` ASSERT `segment already in tree` under
   `zil_parse()` in a dataset destroy (#19524), the deadman abort of a
   suspended pool, zdb's post-run check (`dump_zpool`); otherwise "new".
   For a `suspended` run, add the last three lines of `grep -a -E
   'injecting fault|failed probe' $W/logs/<tree>-r<N>-s<K>.log`.
4. Every deviation from this document, and anything that looked wrong.

Then append the logs:

```sh
. /var/log/ztest-mf0-1109/env.sh
{
for s in 0 1 2; do echo "== STEP $s"; cat $L/step$s.txt; done
echo "== rounds.txt"; cat $L/rounds.txt
echo "== runs.txt"; cat $L/runs.txt
for f in $L/readout-*.txt $L/bt-*.txt $L/stuck.txt; do [ -f "$f" ] && { echo "== ${f##*/}"; cat "$f"; }; done
echo "== run.out"; cat $L/run.out
echo "== STEP 4"; cat $L/step4.txt
} >> $L/REPORT.txt
wc -l $L/REPORT.txt
```

Reply with the whole of `REPORT.txt`. The run logs stay in `$W/logs`, the
kept runs (pool files and cores) in `$W/keep/<tree>-r<N>-s<K>`.

## Reading the result

**The fix works** if the branch has no `ECKSUM` while master has some. On
the branch `ztest_scrub_impl()` cannot return `ECKSUM` with this command at
all (`MAXFAULTS()` is 0), so a branch `ECKSUM` means the branch binary did
not run: check STEP 1. Its read-out on master should look like 8/11's:
`$1` 0, `$2` 1, `$3` 0 (so `MAXFAULTS()` is 0), an injection line before
the failure, and `zdb -bcc` reporting no leak and no error count.

**No `ECKSUM` on master** is possible: at 8/11's rate of 1 in 13 runs, 24
master runs miss it about one time in seven, and three concurrent runs
change the timing that the failure depends on. It then says only that the
failure is rare under this load; the cause rests on the read-outs of 8/11
and the code.

**What else the branch may show.** On the branch, a run that master aborts
at a scrub goes on past it, into what that abort hid. Local runs (with an
aid that makes every `ztest_scrub()` reach its check) showed two such
outcomes, neither caused by the fix: `suspended`, when the injected fault
was a closed fd (a reopen keeps the closed fd, so with no redundancy the
pool cannot resume until the pass ends; the log shows `failed probe` after
the injection), and the #19524 ASSERT above (a pool that suspends while the
ZIL creates its chain leaves a freed log block in the on-disk ZIL header).
So the branch need not fail less in total. What would count against the fix
is `zdb` on the branch only: damage found after a pass, which would mean
that something real hid behind the error log. A class that shows once on
either tree is noise at these counts.

**`EIO`** is not addressed by the fix. Its read-out says whether it comes
with an injected fault (an injection line in the log, a top-level vdev with
`cant_read` set, or with read or checksum errors while zdb finds the pool
clean). Counts alike on both trees are expected.

Not covered here: configurations with redundancy (the branch changes nothing
there: `MAXFAULTS()` is above 0 and the check is as before), `-M` runs (no
zdb check after a pass), other ztest commands, a release build, the kernel
module, and FreeBSD.
