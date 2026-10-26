# VM 106 (CHSONVTST03): failing-first runs of `removal_with_split_damage`

These instructions produce kernel evidence for the fork branch
`indirect-reconstruct-same-child` of https://github.com/tigerblue77/zfs.
The branch adds the ZTS test
`tests/functional/removal/removal_with_split_damage.ksh`, which has never
run in a kernel. The test runs 3 times in each of two trees that the
preparation step has already built on this VM:

| State | Tree | Code | Expected |
| --- | --- | --- | --- |
| B | `/var/tmp/zts-i4-1026/B` | branch head `eef824daed7a03844530c64695fde77a272f0fc4` (base + ba08cfce4f + eef824daed) | PASS, 3 of 3 |
| M | `/var/tmp/zts-i4-1026/M` | base `e117a84dbbfe0befe15110b9d1d799f47b1a5546` + the branch's three test files only | FAIL, 3 of 3, at test line 83 |

B runs first, then M.

## What the test does, and why M must fail

The test builds a pool on mirror(dsk0, dsk1), with ashift 9. It writes
16 random 128K blocks, adds mirror(dsk2, dsk3), sets
`zfs_remove_max_segment` to 8192 and removes `mirror-0`. Each data block
is then split into 16 or 17 segments on mirror-1. The test exports the
pool and overwrites dsk3 from 4 MiB to 255 MiB with random bytes, which
leaves dsk3's labels intact. It then imports the pool and scrubs it.

- **On B,** `vdev_indirect_splits_enumerate_likely()` first tries "every
  segment from child 0" (dsk2), and that combination verifies. The scrub
  repairs dsk3 and charges checksum errors to dsk3 only.
- **On M,** each damaged block has 2^16 or 2^17 combinations, more than
  `zfs_reconstruct_indirect_combinations_max` (4096). The code samples
  4096 of them at random. The zio is retried once, so each block gets
  8192 tries in total. That finds the one good combination with a
  probability of at most 1 - (1 - 2^-16)^8192, about 12% per block. The
  chance that all 16 blocks recover in one scrub is below 1e-14, so all
  three M runs fail.
- **A failed block** is a permanent error, and
  `vdev_indirect_all_checksum_errors()` charges every copy, the intact
  dsk2 copies included.

The test sets only `REMOVE_MAX_SEGMENT` (`zfs_remove_max_segment`), a
tunable that exists on M as well. The branch's `man/man4/zfs.4` change
only rewords `zfs_reconstruct_indirect_combinations_max`, which also
exists on both. So M fails on the bug, not on a missing tunable. The
first failing line on M is line 83:

```
log_must check_pool_status $TESTPOOL "errors" "No known data errors"
```

It prints:

```
ERROR: check_pool_status testpool errors No known data errors exited 1
```

Lines 84 to 89 are never reached on M.

A userspace replica of the test, built from these same two commits
(libzpool, `--enable-debug`) and run 3 times per state, gave:

| State | scrub errors | dsk2 CKSUM | dsk3 CKSUM |
| --- | --- | --- | --- |
| B | 0, 0, 0 | 0, 0, 0 | 301, 302, 301 |
| M | 16, 14, 15 | 512, 464, 527 | 557, 541, 590 |

The kernel pool also carries ZPL metadata, so its dsk3 counts will be
somewhat different. The signs and the orders of magnitude are what
matter.

## Rules for the whole run

- Run everything as root. Shell variables may not survive between your
  commands, so every block below starts by setting or sourcing them.
- Post nothing, push nothing, install nothing, never reboot, and never run
  `scripts/zfs-helpers.sh` or `zfs-tests.sh -x`.
- Work only under `/var/tmp/zts-i4-1026` (W) and `/var/log/zts-i4-1026`
  (L). Never read from, write to or delete `/var/tmp/test_results`,
  `/var/tmp/zts-19314`, `/var/log/zts-19314`, `/var/tmp/zts-19477`,
  `/var/log/zts-19477`, `/var/tmp/ra-work`, `/var/tmp/raidz-dotprod-bench*`,
  or anything else this task did not create.
- The only `rm -rf` allowed is the one in STEP 6.
- Rebuild nothing: the trees are already built. A production hypervisor
  sits underneath this VM. The tests are short, so run them as written,
  without `nice`.
- If a check does not print what its comment says, stop at once and
  report. Do not rebuild, repair or retry. If your permission system
  refuses a command, do not work around it: report it and stop there.

## STEP 0: the VM must be free, and the preparation must be as described

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-i4-1026 L=/var/log/zts-i4-1026
uname -r                      # 6.12.113+deb13-cloud-amd64 (the kernel of the 25 Oct runs)
nproc                         # 16
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^zfs|^spl'   # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -E '^(zfs|spl)$'   # no /dev/zfs, nothing
command -v zpool zfs zed      # nothing: no system ZFS
pgrep -x zed                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs.sh|make|gcc|cc1|raidz_test'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}'
ls -A $W                      # B M repo
ls -A $L                      # build-B.log build-M.log clone.log fetch.log
stat -c '%U %n' $W $W/repo $W/B $W/M $L   # root, five times
readlink -f $W                # /var/tmp/zts-i4-1026
losetup -a; mount | grep -E 'zts-i4|testpool'   # nothing
ls /var/tmp; ls -la /var/tmp/test_results
df -h /var/tmp                # about 52G free; need 10G
id zts; sudo -n -l -U zts | grep -i nopasswd
journalctl -k -b --no-pager -q | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
```

Keep this output, which goes into REPORT.txt. The last number is the
kernel-message baseline.

`readlink -f $W` must print the path unchanged. The test finds its
vdevs in `zpool status -P` by the paths it built under `$W/files`. A
symlink anywhere in `/var/tmp` would make lines 84 and 85 read empty
counts and fail B for a reason unrelated to the fix.

Stop at once, changing nothing, and report what you found if any of
these holds:

- the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, or `zed` runs;
- a test or build process is running. The `pgrep` also matches the shell
  that runs this block, because that shell's command line contains the
  pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` is missing, holds anything other than the names above,
  or is not root-owned;
- `readlink -f $W` prints anything else;
- a loop device or mount matches;
- less than 10 GB is free on `/var/tmp`;
- user `zts` is missing or has no passwordless sudo.

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself,
because STEP 1 checks that the modules were built for the running one.

## STEP 1: verify the preparation (all git work happens here, before any chown)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-i4-1026 L=/var/log/zts-i4-1026
SHA_B=eef824daed7a03844530c64695fde77a272f0fc4
SHA_M=e117a84dbbfe0befe15110b9d1d799f47b1a5546
F1=tests/zfs-tests/tests/functional/removal/removal_with_split_damage.ksh
F2=tests/runfiles/common.run
F3=tests/zfs-tests/tests/Makefile.am
{
echo "B HEAD:   $(git -C $W/B rev-parse HEAD)"     # eef824daed7a03844530c64695fde77a272f0fc4
echo "B HEAD~2: $(git -C $W/B rev-parse HEAD~2)"   # e117a84dbbfe0befe15110b9d1d799f47b1a5546
echo "M HEAD:   $(git -C $W/M rev-parse HEAD)"     # e117a84dbbfe0befe15110b9d1d799f47b1a5546
echo "B tracked changes: $(git -C $W/B status --short --untracked-files=no | wc -l)"   # 0
echo "M tracked changes:"
git -C $W/M status --short --untracked-files=no
#   M  tests/runfiles/common.run
#   M  tests/zfs-tests/tests/Makefile.am
#   A  tests/zfs-tests/tests/functional/removal/removal_with_split_damage.ksh
git -C $W/M diff --cached --stat | tail -n 1    #  3 files changed, 93 insertions(+)
echo "M index vs B, test files: $(git -C $W/M diff --cached $SHA_B -- $F1 $F2 $F3 | wc -l)"   # 0
echo "M unstaged: $(git -C $W/M diff --stat | wc -l)"   # 0
echo "M module/man/include/lib/cmd vs base: $(git -C $W/M diff --cached --stat HEAD -- module man include lib cmd | wc -l)"   # 0
echo "test mode: $(stat -c %a $W/B/$F1) $(stat -c %a $W/M/$F1)"   # 755 755
echo "enumerate_likely in B: $(grep -c vdev_indirect_splits_enumerate_likely $W/B/module/zfs/vdev_indirect.c)"   # 4
echo "enumerate_likely in M: $(grep -c vdev_indirect_splits_enumerate_likely $W/M/module/zfs/vdev_indirect.c)"   # 0
for t in B M; do
    echo "build-$t.log errors: $(grep -cE ': error:|\*\*\*' $L/build-$t.log)"   # 0
    tail -n 2 $L/build-$t.log
    ls -l $W/$t/module/spl.ko $W/$t/module/zfs.ko $W/$t/zpool $W/$t/scripts/zfs.sh $W/$t/scripts/zfs-tests.sh
    echo "$t srcversion $(modinfo -F srcversion $W/$t/module/zfs.ko)"
    echo "$t vermagic $(modinfo -F vermagic $W/$t/module/zfs.ko | cut -d' ' -f1) running $(uname -r)"
done
} 2>&1 | tee $L/step1.txt
```

Expected values:

- the full SHAs, counts and status lines in the comments above;
- B srcversion `3239F9FFA58A6AA021B0975`, and M srcversion
  `8852710DB871A7587B9E722`; they must differ, because `vdev_indirect.c`
  differs between the trees;
- each vermagic equal to the running kernel;
- every `ls -l` path present.

If anything else prints, stop and report. Do not rebuild.

## STEP 2: harness setup

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-i4-1026 L=/var/log/zts-i4-1026
for t in B M; do
    sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
        $W/$t/tests/test-runner/bin/test-runner.py.in \
        $W/$t/tests/test-runner/bin/test-runner.py &&
    grep -n '^BASEDIR' $W/$t/tests/test-runner/bin/test-runner.py
done                          # each: 43:BASEDIR = '/var/log/zts-i4-1026/results', else stop
mkdir $W/files $L/results
chown -R zts $W $L/results; echo "chown rc=$?"   # 0
```

Then create the environment file and the per-run script, both in `$L`
and owned by root:

```sh
cat > /var/log/zts-i4-1026/env.sh <<'EOF'
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-i4-1026
L=/var/log/zts-i4-1026
SV_B=3239F9FFA58A6AA021B0975
SV_M=8852710DB871A7587B9E722
TEST=tests/functional/removal/removal_with_split_damage.ksh
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
EOF
cat > /var/log/zts-i4-1026/run.sh <<'EOS'
#!/bin/sh
# usage: sh /var/log/zts-i4-1026/run.sh <B|M> <label>
. /var/log/zts-i4-1026/env.sh
t=$1; lbl=$2
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label>"; exit 2 ;;
esac
G=$L/guards.txt
if [ -z "$lbl" ] || [ -e "$L/$lbl.log" ]; then
	echo "label missing or already used: $lbl"; exit 2
fi

# Guards before the run: this tree's module, default tunables, no pool,
# empty work directory.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
seg=$(cat /sys/module/zfs/parameters/zfs_remove_max_segment 2>&1)
cmb=$(cat /sys/module/zfs/parameters/zfs_reconstruct_indirect_combinations_max 2>&1)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(ls -A "$W/files" 2>&1)
echo "$(date -u +%T) before $lbl: srcversion=$cur want=$sv" \
    "remove_max_segment=$seg combinations_max=$cmb" \
    "pools=[$pools] files=[$files]" | tee -a "$G"
if [ "$cur" != "$sv" ] || [ "$seg" != 16777216 ] || [ "$cmb" != 4096 ] ||
    [ -n "$pools" ] || [ -n "$files" ]; then
	echo "GUARD FAILED before $lbl: nothing run" | tee -a "$G"
	exit 1
fi

# The run.
start=$(date +%s)
cd "$W/$t" || exit 1
timeout 1800 runuser -u zts -- ./scripts/zfs-tests.sh -vKO -s 3GB \
    -d "$W/files" -t "$TEST" > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
# Two runs started in the same second would share a results directory.
sleep 2

# Results. A test with no stderr has no "merged" file: use "stdout".
res=$(grep -E 'Test: .*removal_with_split_damage ' "$L/$lbl.log" |
    grep -oE '\[[A-Z]+\]$')
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
out=""
if [ -n "$rd" ] && [ -d "$rd" ]; then
	out=$(find "$rd" -path '*removal_with_split_damage*' -name merged |
	    head -n 1)
	[ -n "$out" ] || out=$(find "$rd" -path '*removal_with_split_damage*' \
	    -name stdout | head -n 1)
fi
[ -n "$out" ] && cp "$out" "$L/$lbl-out.txt"
{
	echo "$lbl: tree $t, result ${res:-none}, zfs-tests.sh rc=$rc," \
	    "wall ${wall}s, log dir ${rd:-none}"
	grep -E '^Missing util' "$L/$lbl.log"
	grep -E 'Test: .*removal_with_split_damage ' "$L/$lbl.log"
	if [ -n "$out" ]; then
		grep -E 'SUCCESS: (zpool (remove|import|scrub)|wait_for_removal)|ERROR: |SUCCESS: (check_pool_status|test )|scan:|/dsk[23] +[A-Z]+ +[0-9]+ +[0-9]+ +[0-9]+|undamaged child' "$out"
		grep -E -A2 'errors:' "$out"
	else
		echo "no stdout or merged file found for the test"
	fi
} > "$L/$lbl-notes.txt"

# Kernel messages of this run (ZTS clears dmesg after a failure, the
# journal keeps them). "ZTS run" is the -K marker: it proves the journal
# saw the run.
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
kn=$(grep -ciE "$KPAT" "$L/$lbl-kernel.txt")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")

# Leftovers: all must be empty, and the tunable back at its default.
lp=$("$W/$t/zpool" list -H -o name 2>&1)
ll=$(losetup -a | grep zts-i4)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(ls -A "$W/files" 2>&1)
lm=$(mount | grep -E 'zts-i4|testpool')
lr=$(ls -d /dsk0 /dsk1 /dsk2 /dsk3 2>/dev/null)
# Leftover processes, leaving out this script and the shells that started
# it: their command lines can contain the same words.
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests.sh|test-runner|removal_with_split_damage' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)
seg=$(cat /sys/module/zfs/parameters/zfs_remove_max_segment 2>&1)

echo "$(date -u +%T) after $lbl: result=${res:-none} rc=$rc wall=${wall}s" \
    "zts_marks=$km kernel_matches=$kn remove_max_segment=$seg" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] loops=[$ll] dm=[$ld]" \
    "files=[$lf] mounts=[$lm] rootfiles=[$lr] procs=[$lq]" | tee -a "$G"
cat "$L/$lbl-notes.txt"
[ "$kn" -gt 0 ] && grep -iE "$KPAT" "$L/$lbl-kernel.txt"
if [ -n "$lp$ll$ld$lf$lm$lr$lq" ] || [ "$seg" != 16777216 ] ||
    [ "$kn" -gt 0 ] || [ "$km" -lt 1 ]; then
	echo "STOP after $lbl: leftover, tunable not restored, kernel" \
	    "message, or no ZTS mark in the journal" | tee -a "$G"
	exit 3
fi
exit 0
EOS
sh -n /var/log/zts-i4-1026/run.sh; echo "syntax rc=$?"   # 0
```

`run.sh` exits 0 when the run is clean, whatever the test result. It
exits 1 when a guard before the run failed (nothing ran), 2 on a bad
label, and 3 when something is wrong after the run. On 1, 2 or 3, stop
and report; clean nothing by hand.

## STEP 3: state B

```sh
. /var/log/zts-i4-1026/env.sh
cd $W/B && ./scripts/zfs.sh; echo "load rc=$?"                    # 0
cat /sys/module/zfs/srcversion                                   # 3239F9FFA58A6AA021B0975
cat /sys/module/zfs/parameters/zfs_remove_max_segment             # 16777216
cat /sys/module/zfs/parameters/zfs_reconstruct_indirect_combinations_max   # 4096
```

Then run b1, b2 and b3, one at a time. Go on to the next only if the
previous one exited 0:

```sh
sh /var/log/zts-i4-1026/run.sh B b1; echo "run.sh rc=$?"
sh /var/log/zts-i4-1026/run.sh B b2; echo "run.sh rc=$?"
sh /var/log/zts-i4-1026/run.sh B b3; echo "run.sh rc=$?"
```

Each run should take well under 2 minutes; the runner kills a test at
600 s.

## STEP 4: state M

Unload B only while `/sys/module/zfs/srcversion` is still B's, then load
M:

```sh
. /var/log/zts-i4-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_B" ] && cd $W/B && ./scripts/zfs.sh -u; echo "unload rc=$?"   # 0
lsmod | grep -E '^zfs|^spl'                                      # nothing
cd $W/M && ./scripts/zfs.sh; echo "load rc=$?"                    # 0
cat /sys/module/zfs/srcversion                                   # 8852710DB871A7587B9E722
cat /sys/module/zfs/parameters/zfs_remove_max_segment             # 16777216
cat /sys/module/zfs/parameters/zfs_reconstruct_indirect_combinations_max   # 4096
```

```sh
sh /var/log/zts-i4-1026/run.sh M m1; echo "run.sh rc=$?"
sh /var/log/zts-i4-1026/run.sh M m2; echo "run.sh rc=$?"
sh /var/log/zts-i4-1026/run.sh M m3; echo "run.sh rc=$?"
```

## Expected

This block goes into REPORT.txt as written.

```
Every run: run.sh rc=0; zts_marks >= 1; kernel_matches=0; all leftovers
empty; remove_max_segment=16777216 after the run.
"Missing util(s):" must not list xxh128sum, mktemp, truncate or dd.

B (b1, b2, b3), each:
  Test: /var/tmp/zts-i4-1026/B/tests/zfs-tests/tests/functional/removal/removal_with_split_damage (run as root) [mm:ss] [PASS]
  SUCCESS: zpool remove testpool mirror-0 / wait_for_removal / two zpool import / zpool scrub -w
  scan: scrub repaired <size> in <time> with 0 errors on <date>
  .../files/tmp.<X>/dsk2  ONLINE  0  0  0
  .../files/tmp.<X>/dsk3  ONLINE  0  0  <n>      n > 0 (userspace replica: 301-302)
  errors: No known data errors
  SUCCESS: check_pool_status testpool errors No known data errors
  SUCCESS: test 0 -eq 0
  SUCCESS: test <n> -gt 0
  SUCCESS: test <32 hex digits> = <the same 32 hex digits>
  Split blocks are reconstructed from the undamaged child.
  No "ERROR:" line.

M (m1, m2, m3), each, deterministic in practice (P(pass) < 1e-14 per run):
  Test: /var/tmp/zts-i4-1026/M/tests/zfs-tests/tests/functional/removal/removal_with_split_damage (run as root) [mm:ss] [FAIL]
  SUCCESS: zpool remove testpool mirror-0 / wait_for_removal / one zpool import / zpool scrub -w
  scan: scrub repaired <size> in <time> with <N> errors on <date>   N about 13-16 (replica: 14-16)
  .../files/tmp.<X>/dsk2  ONLINE  0  0  <c2>     c2 > 0, about 32 to 34 x N (replica: 464-527)
  .../files/tmp.<X>/dsk3  ONLINE  0  0  <c3>     c3 > c2 (replica: 541-590)
  errors: Permanent errors have been detected in the following files:
          /var/tmp/zts-i4-1026/files/testdir/testfile0
  ERROR: check_pool_status testpool errors No known data errors exited 1   (test line 83)
  No "SUCCESS: check_pool_status" and no "SUCCESS: test" line.
  <label>-out.txt also holds the dbgmsg and dmesg dumps that ZTS adds
  after a failure: they are findings only if they show an assertion,
  a panic or a hung task.
```

## STEP 5: what each result means

- **Failing-first is shown** when B passes 3 of 3 and M fails 3 of 3,
  both as in the expected block. That is, M fails at line 83 with
  permanent errors on `testfile0` and with checksum errors on dsk2, the
  intact child.
- **The fix, or the test, is contradicted** by any of these:
  - **any B run fails:** report the first `ERROR:` line and the dsk2/dsk3
    rows;
  - **any M run passes:** the chance of that is below 1e-14, so it would
    mean the test does not exercise the bug on this kernel;
  - **an M run fails at line 84 or 85 instead of 83:** no data error, but
    dsk2 charged, or dsk3 not charged.
- **A run is inconclusive** when `run.sh` exits 0 but the run shows
  either of these:
  - `[KILLED]`, `[SKIP]` or `[UNSUPPORTED]`;
  - an `ERROR:` line before `zpool scrub -w` (pool creation, removal,
    export, `dd` or import): that is not the bug.

  Report it as inconclusive. You may rerun such a run with a new label
  (b4, m4, ...), at most twice per state. A `run.sh` exit of 1, 2 or 3 is
  not inconclusive: it is a stop, and nothing more runs.
- **`xxh128sum` listed as missing** makes B's last check (line 89)
  compare two empty strings. Then B's PASS still shows the
  reconstruction, but not that the file contents survived. Say so.

## STEP 6: unload, report, cleanup

```sh
. /var/log/zts-i4-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_M" ] && cd $W/M && ./scripts/zfs.sh -u; echo "unload rc=$?"   # 0
lsmod | grep -E '^zfs|^spl'                                       # nothing
ls -la /var/tmp/test_results                                      # as in STEP 0
journalctl -k -b --no-pager -q | grep -ciE "$KPAT"                # STEP 0 baseline, or report the new lines
```

Write `/var/log/zts-i4-1026/REPORT.txt` with:

- STEP 0 output (short) and `uname -r`;
- `step1.txt` whole: the two HEADs, every STEP 1 check and the two
  srcversions;
- the load checks of STEP 3 and STEP 4, and `guards.txt` whole;
- for each of the 6 runs (and any rerun): PASS, FAIL, SKIP or KILLED, the
  wall time, and the whole `<label>-notes.txt`;
- kernel findings per run, and the final journal count against the STEP 0
  baseline;
- any leftover;
- the "Expected" block above, then for each state whether it matched, and
  which STEP 5 case applies;
- every deviation from these instructions, with its reason.

Then print it with `cat /var/log/zts-i4-1026/REPORT.txt` and copy it
whole into your reply. Only after that, as the very last command, remove
the two directories of this task:

```sh
rm -rf /var/tmp/zts-i4-1026 /var/log/zts-i4-1026; echo "rm rc=$?"
ls -d /var/tmp/zts-i4-1026 /var/log/zts-i4-1026 2>&1   # both: No such file or directory
```

If the permission system refuses this `rm -rf`, leave both directories
as they are and say so in the reply. Do not delete them any other way.
