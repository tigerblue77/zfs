# VM task: rerun the crash-1 ztest replay and keep the cores, to place the ztest_dataset_open ENOSPC (8/11)

These instructions are for one test VM with 16 vCPUs, 8 GiB of memory
available, Debian 13 and 40 GiB free on `/var/tmp` (STEP 0 checks). VM 106
is the first choice, since it ran the earlier replays of this command; VM 105
or VM 107 will do if they have that shape. Reply with
`/var/log/zrep3-1108/REPORT.txt` (STEP 5). Nothing is posted, pushed or
installed, and no kernel module is built or loaded: ztest runs in user space.

**A functional test.** It counts failures and reads cores, it does not time
anything. It may run while one other VM runs a functional job (ztest, zloop,
ZTS, a replay), but never while a VM runs a performance test, since the
three VMs share one hypervisor and 23 ztest threads would skew a measurement
there. Nothing else runs on this VM meanwhile.

**Background.** On 8/11, VM 106 replayed the "crash 1" ztest command (from a
zloop failure of 6/11 on an earlier head of the branch, `2b7da131bd`) 24
times on upstream master `ef0614e724` and 24 times on the fork branch
`free-on-full-pool` at `6ca2305e65`, both debug user-space builds. Branch
round 11 failed after 263 s with

```
ASSERT at cmd/ztest.c:8817:ztest_generic_run()
VERIFY0(ztest_dataset_open(i)) failed (28)
```

The last progress line was `Pass 7, Complete, 541 ENOSPC, 74.2% of 496M
used`; the pool was not suspended, and the normal class showed 0 deferred
frees (`mc_deferred`). Its core was deleted before anyone read it.

From the code, this is a master ztest bug, not the branch's: after a pass
that hit ENOSPC, `ztest_run()` destroys one dataset (`Destroying
ztest/ds_N to free up space`) and waits for a txg sync, then
`ztest_generic_run()` reopens every dataset with `VERIFY0`, so a create that
still finds no space aborts the run. Four causes can leave the space
missing:

- (a) a pool checkpoint holds the blocks the destroy freed;
- (b) the destroyed dataset was too small to give back enough;
- (c) `dsl_destroy_head()` returned `EBUSY`, which ztest ignores, so
  nothing was destroyed;
- (d) the async destroy had not finished when the reopen ran.

A local run on master (another command, with `-v 1`, `-P 15` and
`-s 128m`) showed (a). This task reruns the VM 106 command with cores kept,
until the failure comes back, and reads the pool's space figures out of each
core to say which cause it was on this command.

**What runs.** STEP 1 fetches both commits by SHA and builds each one as
VM 106 did (`./configure --enable-debug --with-config=user`, `make -j8`).
STEP 3 then runs `zrep3.sh`, one ztest at a time, round after round, the
branch first and master second in each round. Every run uses exactly VM 106's
command and settings:

```
cd <fresh scratch dir> && ulimit -c unlimited &&
nice -n 10 ionice -c3 timeout -k 30 900 <tree>/ztest \
    -G -VVVVV -K raidz -m 0 -r 0 -D 0 -S 0 -R 2 -v 4 -a 12 \
    -C special=random -s 512m -f <scratch dir> \
    -J 4abf7fd74d6647686cf92eb603a57b79782917664e25686c6ea806070cf154bd
```

`-J` fixes the seeds but not the thread timing, so a run does not repeat
another: on VM 106 this command failed in 2 of 24 runs on master and 7 of 24
on the branch, in several different ways, and the `ztest_dataset_open`
failure came once.

`zrep3.sh` writes one line per run to `runs.txt`. A run that fails keeps
its scratch dir (the pool's files and the core) and its log, and every core
gets a backtrace (`bt.sh`). A `ztest_dataset_open` failure gets the read-out
(`readout.sh`) at once, on every core of that run. A passing run keeps its
log, compressed, and its scratch dir is deleted. The rounds stop at the
first of:

1. a `ztest_dataset_open` failure on the branch tree (after its read-out);
   the same failure on master gets its read-out and the rounds go on;
2. 3 hours after the first run started: no run starts after that, and the
   run in progress finishes;
3. 24 rounds per tree;
4. less than 10 GiB free on `/var/tmp` before a run (not expected: see the
   disk figures below).

**Duration**: STEP 0 2 min; STEP 1 about 12 min (two debug builds, one
after the other); STEP 2 1 min; STEP 3 from about 5 min (the first branch
run fails that way) to about 3 h 20 min (3 hours, then the run in progress,
at most 15.5 min under its cap, and its backtrace or read-out, a minute or
two); STEPS 4 and 5 about 5 min. In all, from about 25 min to about 3 h
40 min. A run takes about 5 min (300 s of ztest plus zdb checks; failures
came after 150 to 400 s), so the 3 hours hold about 17 rounds per tree and
rule 3 only binds if most runs fail early.

**Disk**: about 1 GiB for the two builds and the repository. A failing run
keeps about 2 GiB: VM 106 kept 18 GiB for 9 failing runs, the core alone
being about 1 GiB (1,061,629,952 bytes for branch round 1 of its second
replay), the rest the pool's files. A passing run keeps under 1 MiB (its
`xz` log). At VM 106's rates, about 6 failing runs in 3 hours, so about
15 GiB in all; 40 GiB free leaves room for about 14 failing runs before
rule 4 stops the rounds.

## Rules

- Disposable VM only, with nothing else running on it. User space only: no
  module is built or loaded, nothing runs `zpool` or `zfs`, and the only
  pools are ztest's own, in files under W.
- Two debug builds (`--enable-debug --with-config=user`), under `nice -n 10
  ionice -c3` with `make -j8`, as on VM 106.
- Post nothing, push nothing. Install nothing (no `make install`, no
  package). Never reboot.
- Work only under `/var/tmp/zrep3-1108` (W) and `/var/log/zrep3-1108` (L).
  The one change outside them is the core dump pattern, and only if it is
  not already `core` with `core_uses_pid` 1 (STEP 2); STEP 4 restores it.
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
ls -d /var/tmp/zrep3-1108 /var/log/zrep3-1108 2>&1      # No such file or directory, twice
df -h /var/tmp /var/log | tail -n +2                    # 40G free on /var/tmp, 1G on /var/log
} 2>&1 | tee /var/tmp/zrep3-1108-step0.txt
```

Stop, changing nothing, if: `sudo -n true` fails; `nproc` is not 16; the OS
is not Debian 13; MemAvailable is under 8 GiB; a tool is missing; the hard
core limit is not `unlimited` (`ulimit -c unlimited` would fail and no run
would start); a build, test or benchmark runs; W or L exists; less than 40G
is free on `/var/tmp` or less than 1G on `/var/log`. The kernel version
does not matter: nothing here uses the kernel's ZFS. Any core pattern is
fine here; STEP 2 deals with it.

## STEP 1: sources and debug builds (about 12 min)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zrep3-1108 L=/var/log/zrep3-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W || exit 1
mv /var/tmp/zrep3-1108-step0.txt $L/step0.txt
cat > $L/env.sh <<'EOF2'
export PATH=/usr/sbin:/sbin:/usr/bin:/bin
W=/var/tmp/zrep3-1108 L=/var/log/zrep3-1108
M=ef0614e724d42b58f7b551a5e718648264b2bd0c
B=6ca2305e656728b9f806391ed8eddd14d4685a62
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
git -C $W/repo diff --quiet $M $B -- cmd lib && echo "cmd/ and lib/ identical in both trees"
for t in master branch; do sed -n 8817p $W/$t/cmd/ztest.c | xargs echo "$t ztest.c:8817:"; done
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

Expected: `master ef0614e724d42b58f7b551a5e718648264b2bd0c ZTS: make
dio_read_verify_decline's first read fail its verify`; `branch
6ca2305e656728b9f806391ed8eddd14d4685a62 Keep the space that frees need out
of reach of admin operations`; `cmd/ and lib/ identical in both trees` (the
branch changes only `module/`, `include/` and tests, so both trees run the
same ztest and zdb code); `VERIFY0(ztest_dataset_open(i));` on line 8817 of
both; `build ... rc=0` twice (VM 106 had 0 warnings); 1 or more lines with
`--enable-debug`, twice; the five paths listed, twice. On a failed fetch or
build quote `grep -nE 'error:|Error [0-9]' $L/build-<tree>.log | head -20`
and stop.

## STEP 2: core dumps and the scripts (1 min)

ztest's child process dumps its core in its working directory, the run's
scratch dir, as `core.<pid>`, when the pattern is `core` and
`core_uses_pid` is 1 (as on VM 106). Anything else (a pipe to
`systemd-coredump` or `apport`, a path) is set to that for the task and
restored in STEP 4; the old values are kept in `core-settings-before.txt`.
A test core then checks that a killed process leaves one.

`zrep3.sh` runs the rounds (see "What runs"). `bt.sh` names the program
that dumped a core and prints the first frames of the thread that crashed.
`readout.sh` is the read-out for a `ztest_dataset_open` failure: the log
lines around the ENOSPC recovery, then seven values from the core, then a
supplement (the dataset whose open failed, and the datasets left on disk).

```sh
. /var/log/zrep3-1108/env.sh
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

cat > $L/zrep3.sh <<'EOF2'
#!/bin/bash
# zrep3.sh: the crash-1 ztest command of VM-ZREP3-1108 in rounds, branch
# then master, one run at a time, until a stop rule holds.  One line per run
# in $L/runs.txt.  A failing run keeps its scratch dir, cores and log; a
# ztest_dataset_open ENOSPC gets the read-out at once.
exec < /dev/null
. /var/log/zrep3-1108/env.sh
A=(-G -VVVVV -K raidz -m 0 -r 0 -D 0 -S 0 -R 2 -v 4 -a 12 -C special=random -s 512m)
J=4abf7fd74d6647686cf92eb603a57b79782917664e25686c6ea806070cf154bd
U='[0-9.]+% of +[0-9.]+[KMGT]? used'
mkdir -p "$W/keep" "$W/logs" || exit 1
t0=$(date +%s) stop=
for r in $(seq 1 24); do
	for t in branch master; do
		if [ $(( $(date +%s) - t0 )) -ge 10800 ]; then
			stop="3 hours"; break 2
		fi
		free=$(df -B1G --output=avail "$W" | tail -1 | tr -dc 0-9)
		if [ "${free:-0}" -lt 10 ]; then
			stop="under 10 GiB free on $W"; break 2
		fi
		S=$(mktemp -d "$W/scratch.XXXXXX") || { stop="mktemp failed"; break 2; }
		lg=$W/logs/$t-r$r.log s=$(date +%s)
		(cd "$S" && ulimit -c unlimited && nice -n 10 ionice -c3 \
		    timeout -k 30 900 "$W/$t/ztest" "${A[@]}" -f "$S" -J $J \
		    > "$lg" 2>&1)
		rc=$? el=$(( $(date +%s) - s ))
		su=$(grep -a -c 'has been suspended' "$lg")
		e28=$(grep -a -c 'error 28' "$lg")
		pk=$(grep -a -o -E "$U" "$lg" | sort -n | tail -1)
		la=$(grep -a -o -E "$U" "$lg" | tail -1)
		nc=$(find "$S" -maxdepth 1 -name 'core*' | wc -l)
		as=$(grep -a -m1 -A1 '^ASSERT at' "$lg" | tr '\n' ' ' | cut -c1-200)
		if [ $rc = 0 ]; then c=pass
		elif grep -a -q -F 'VERIFY0(ztest_dataset_open(i)) failed (28)' "$lg"; then c=DSOPEN
		elif [ -n "$as" ]; then c=assert
		elif grep -a -q 'aborting test after .* suspended' "$lg"; then c=suspended
		elif [ $rc = 124 ] || [ $rc = 137 ]; then c=timeout
		else c=other
		fi
		echo "r$r $t rc=$rc ${el}s susp=$su e28=$e28 peak=[$pk] last=[$la] cores=$nc class=$c${as:+ | $as}" |
		    tee -a "$L/runs.txt"
		if [ $rc = 0 ]; then
			xz -T2 "$lg"; rm -rf "$S"
		else
			K=$W/keep/$t-r$r
			mv "$S" "$K"
			for co in "$K"/core*; do
				[ -e "$co" ] && bash "$L/bt.sh" $t "$co" >> "$L/bt-$t-r$r.txt" 2>&1
			done
			if [ $c = DSOPEN ]; then
				n=0
				for co in "$K"/core*; do
					[ -e "$co" ] || continue
					n=$((n + 1))
					bash "$L/readout.sh" $t "$co" "$lg" > "$L/readout-$t-r$r-${co##*/}.txt" 2>&1
				done
				[ $n = 0 ] && bash "$L/readout.sh" $t none "$lg" > "$L/readout-$t-r$r-nocore.txt" 2>&1
				if [ $t = branch ]; then
					stop="ztest_dataset_open failure on the branch, round $r"; break 2
				fi
			fi
		fi
		sync -f "$W"
	done
done
echo "stop: ${stop:-24 rounds per tree} after $(( ($(date +%s) - t0) / 60 )) min" | tee -a "$L/runs.txt"
EOF2

cat > $L/bt.sh <<'EOF2'
#!/bin/bash
# bt.sh <tree> <core>: the program that dumped the core, and the first
# frames of the thread that crashed.
. /var/log/zrep3-1108/env.sh
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

cat > $L/readout.sh <<'EOF2'
#!/bin/bash
# readout.sh <tree> <core|none> <ztest log>: the read-out of a
# ztest_dataset_open ENOSPC; see "Reading the result" in VM-ZREP3-1108.md.
. /var/log/zrep3-1108/env.sh
t=$1 C=$2 LG=$3 Z=$W/$1/.libs/ztest G=${2%/*}/gdb-readout-${2##*/}.txt
echo "== read-out: tree $t, core $C, log $LG"
echo "-- log"
grep -a -n -E '^Pass |to free up space|checkpoint_txg|destroy ztest/ds_|dsl_process_async_destroys' "$LG" | tail -25
if [ ! -e "$C" ]; then
	echo "-- no core: the core values cannot be read"
	exit 0
fi
echo "-- core (\$1 to \$7)"
timeout 300 gdb -q -batch -iex 'set debuginfod enabled off' \
    -ex 'print ztest_spa->spa_checkpoint_txg' \
    -ex 'print ztest_spa->spa_checkpoint_info.sci_dspace' \
    -ex 'print ztest_spa->spa_dsl_pool->dp_bptree_obj' \
    -ex 'print ((dsl_dir_phys_t *)ztest_spa->spa_dsl_pool->dp_free_dir->dd_dbuf->db_data)->dd_used_bytes' \
    -ex 'print ((dsl_dir_phys_t *)ztest_spa->spa_dsl_pool->dp_root_dir->dd_dbuf->db_data)->dd_used_bytes' \
    -ex 'print ztest_spa->spa_deferred_bpobj.bpo_phys->bpo_bytes' \
    -ex 'print ztest_spa->spa_rdspace' \
    "$Z" "$C" > "$G" 2>&1
grep -E '^\$[0-9]+ =' "$G"
n=$(grep -c -E '^\$[0-9]+ =' "$G")
[ "$n" = 7 ] || echo "INCOMPLETE: $n of 7 values; the whole gdb output is in $G"
echo "-- supplement: frames, the index of the dataset whose open failed, the datasets on disk"
timeout 300 gdb -q -batch -iex 'set debuginfod enabled off' -ex 'bt 6' -ex 'frame function ztest_generic_run' \
    -ex 'print i' -ex 'print ndatasets' "$Z" "$C" 2>&1 |
    grep -E '^#[0-9]+ |^\$[0-9]+ =' | awk '!seen[$0]++' | cut -c1-160
(ulimit -c 0; timeout 300 "$W/$t/zdb" -e -p "${C%/*}" -d ztest 2>&1) |
    grep -E '^Dataset ztest/ds_[0-9]+ |rror|ASSERT|VERIFY' | cut -c1-80
EOF2
chmod +x $L/zrep3.sh $L/bt.sh $L/readout.sh
for f in zrep3.sh bt.sh readout.sh; do bash -n $L/$f && echo "$f: syntax ok"; done
df -h /var/tmp | tail -1
} 2>&1 | tee $L/step2.txt
```

Expected: the old pattern and `uses_pid` value; `core pattern now [core]
uses_pid 1`; `test process exit 134 (134 expected)`; `test core:` and the
path of a `core.<pid>` file; `syntax ok` three times. A `test core:` line
that ends in `core*` means no core was written: stop (cores would be lost
again).

## STEP 3: the rounds (5 min to about 3 h 20 min)

```sh
. /var/log/zrep3-1108/env.sh
bash $L/zrep3.sh > $L/run.out 2>&1; tail -3 $L/runs.txt
```

The first line of `runs.txt` comes after about 5 min, then one about every
5 min. A passing run reads like `r1 branch rc=0 306s susp=0 e28=0
peak=[38.7% of 608M used] last=[...] cores=0 class=pass` (`e28` is 0 on
passing runs: ztest writes its debug log, where the `error 28` lines are,
only when it crashes). The last line is `stop: <reason> after <N> min`.

`class` is `DSOPEN` (the failure this task looks for), `assert` (another
`ASSERT`/`VERIFY`, quoted after `|`), `suspended` (ztest's deadman abort of
a suspended pool), `timeout` (killed at the 15-minute cap), or `other`.

## STEP 4: cleanup (about 2 min; keeps every core, log and tree)

```sh
. /var/log/zrep3-1108/env.sh
{
pgrep -af 'ztest|zdb|gdb' | grep -v pgrep               # nothing
o=$(sed -n 1p $L/core-settings-before.txt); u=$(sed -n 2p $L/core-settings-before.txt)
[ "$(cat /proc/sys/kernel/core_pattern)" = "$o" ] || printf '%s\n' "$o" | sudo -n tee /proc/sys/kernel/core_pattern
[ "$(cat /proc/sys/kernel/core_uses_pid)" = "$u" ] || echo "$u" | sudo -n tee /proc/sys/kernel/core_uses_pid
echo "core pattern restored: [$(cat /proc/sys/kernel/core_pattern)] uses_pid $(cat /proc/sys/kernel/core_uses_pid)"
ls -d $W/scratch.* 2>&1 | head -3                       # none
du -sh $W/repo $W/master $W/branch $W/logs $W/keep
for d in $W/keep/*; do [ -d "$d" ] && echo "$(du -sh "$d" | cut -f1) ${d##*/}: $(ls "$d" | tr '\n' ' ' | cut -c1-200)"; done
sudo -n fstrim -av
df -h /var/tmp | tail -1
} 2>&1 | tee $L/step4.txt
```

`core pattern restored` must show the values of `core-settings-before.txt`.
Everything under W stays: the reply decides what to delete. The VM is then
idle.

## STEP 5: REPORT.txt

Write `/var/log/zrep3-1108/REPORT.txt` yourself first, with, in this order:

1. The stop line of `runs.txt`, the number of rounds run per tree, and the
   failing runs per tree (for example `branch 3 of 17, master 1 of 17`).
2. For each `DSOPEN` run (tree, round): the seven values `$1` to `$7`, each
   named (checkpoint txg, checkpoint space, bptree object, `$FREE` used,
   root used, deferred-free bytes, `spa_rdspace`); the log lines that place
   the checkpoint, the `Destroying ztest/ds_N` line and the destroy and
   `freed N blocks` lines that follow it; the supplement (the index `i`,
   whether `ds_N` is still among the datasets on disk); and the cause, (a),
   (b), (c), (d) or "unclassified", by "Reading the result" below, with the
   values it rests on. If several hold, name them all.
3. One line per other failing run: tree, round, `class`, the `ASSERT` text
   or the log's failure line, which program dumped the core (ztest or zdb)
   and its size, the first three function names of its backtrace, and which
   known failure it is, if one: the `vdev_removal.c:1574` VERIFY, the
   `zil.c:873` ASSERT in `zil_free_lwb()`, the deadman abort of a suspended
   pool, zdb's post-run check (`dump_zpool`); otherwise "new".
4. Every deviation from this document, and anything that looked wrong.

Then append the logs:

```sh
. /var/log/zrep3-1108/env.sh
{
for s in 0 1 2; do echo "== STEP $s"; cat $L/step$s.txt; done
echo "== runs.txt"; cat $L/runs.txt
for f in $L/readout-*.txt $L/bt-*.txt $L/stuck.txt; do [ -f "$f" ] && { echo "== ${f##*/}"; cat "$f"; }; done
echo "== run.out"; cat $L/run.out
echo "== STEP 4"; cat $L/step4.txt
} >> $L/REPORT.txt
wc -l $L/REPORT.txt
```

Reply with the whole of `REPORT.txt`. The run logs stay in `$W/logs`, the
failing runs (pool files, cores, the full gdb output of each read-out) in
`$W/keep/<tree>-r<N>`.

## Reading the result

The seven values of a read-out, in order: `$1` `spa_checkpoint_txg` (0: no
checkpoint); `$2` the space the checkpoint holds; `$3` `dp_bptree_obj` (not
0: an async destroy is queued); `$4` the bytes in the `$FREE` dir, freed but
not yet given back; `$5` the bytes used under the pool's root dir; `$6` the
bytes of deferred frees; `$7` `spa_rdspace`, the pool's raw space, for
scale. The log lines come from ztest's progress output and from its debug
log, which it prints when it crashes.

1. **(a) the checkpoint holds the freed blocks**: `$1` is not 0, `$2` is
   tens of MB, and the log shows a `checkpoint_txg` line near the
   `Destroying ztest/ds_N` line. This is what the local master run showed.
2. **(d) the async destroy had not finished**: `$3` is not 0 and `$4` is
   above 0.
3. **(b) or (c)**: `$1`, `$3` and `$4` are all 0. Then (c), the destroy
   returned `EBUSY` and ztest went on, if no `destroy ztest/ds_N (id`
   line (N being the number of the `Destroying` line) follows the
   `Destroying ztest/ds_N` line; otherwise (b), the dataset was too small,
   with a small `freed N blocks` after it. The supplement cross-checks: a
   `ds_N` still listed on disk means the destroy did not take effect, as in
   (c), and `i` is the index of the dataset whose create failed.
4. **None of these** (for example `$1` is 0, `$3` is 0 and `$4` is above 0):
   "unclassified", with the values. A read-out marked `INCOMPLETE` gives
   no cause: quote the end of the gdb output it names.

The branch changes no code of checkpoints, dataset destroy, async destroy
or ztest: its files are `include/sys/dsl_synctask.h`, `dmu_tx.c`,
`dsl_dir.c`, `dsl_pool.c`, `spa_misc.c` (space checks), the ZPL files of
both platforms, and tests. So (a) or (d), on either tree, is master's
behaviour. (b) or (c) found on the branch only would need a closer look at
its space checks, since a dataset create goes through them.

**No `DSOPEN` in 3 hours** is a likely outcome: at VM 106's rate (1 in 24
branch runs, 0 in 24 master runs), about 17 branch runs miss it about one
time in two. It then says only that the failure is rare on this command;
the cause rests on the local master run, and the other failures are still
counted.

**The other failures** add to VM 106's 24 runs per tree. Its counts were:
the removal VERIFY master 1, branch 3; the suspended-pool abort branch 1;
`zil_free_lwb` branch 1; the `ztest_dataset_open` ENOSPC branch 1; zdb's
post-run check master 1, branch 1.

Not covered here: other ztest commands, a release build, the kernel module,
and FreeBSD.
