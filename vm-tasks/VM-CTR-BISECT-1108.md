# VM task: per-commit bisect of the per-CPU hot counters (8/11)

These instructions are for VM 105 (or VM 106): 16 vCPUs in one NUMA node,
Debian 13, kernel 6.12, at least 13 GiB of available memory. STEP 0 stops on
any other shape. Reply with `/var/log/ctr-bisect-1108/REPORT.txt` (STEP 7).

This is a performance run on the hypervisor the three test VMs share: run
STEPS 3 and 4 only while VM 106 and VM 107 are idle (you are told when).
STEPS 0 to 2 (checks and builds) may run before that.

**Background.** VM-CTR-AB-1108 (this VM, 8/11, until about 18:35Z) compared
master with the fork branch `percpu-hot-counters` in three passes, A1, B,
A2. B kept a clear gain on cached reads (8 jobs +21 %, 16 jobs +13 %, every
B sample above every A sample), but some rows lost: 8-job uncached reads
(-11 %, lz4 -15 %), 16 jobs under `perf lock` (-16 %), mirror writes at 16
jobs (-18.5 %) and reads (-6 to -8 %), sync writes at 1, 2 and 4 jobs (-7 to
-10 %). B ran once, next to ztest replays on VM 106 and benchmarks on VM
107. Rows that are the same workload in the same pass differed by up to 25
%, and B's losses cluster in the last 15 minutes of its pass, across three
pools and paths that different commits touch. This task tells noise from an
effect and, for a loss that holds, names the commit that carries it.

**What is compared.** Five builds, from one source tree:

| arm | commit | what it is |
|---|---|---|
| `A` | `ef35f144e1d0df22e835970dc9bc5615f4d1df48` | upstream master, the commit the branch sits on |
| `B1` | `7f19b9fa11cbc744eaf5fe6abd32a07161d4ef79` | + (1) `arcs_esize[]`: atomic -> wmsum |
| `B12` | `ee45ef6ecaa6faf00e7378893458f069538d7bd7` | + (2) `arcs_size[]`: atomic -> wmsum, arc_anon -> aggsum (lower bound in `arc_tempreserve_space()`) |
| `B123` | `0db849c2161f8d2b8a45d94efd3aee3a8f65a347` | + (3) `dbuf_caches[].size`: atomic -> aggsum |
| `B1234` | `e0ba7d8c779be98fcf3acaefd91f63960155d1fe` | + (4) `vdev_stat_update()` skips `vdev_stat_lock` of interior vdevs on success; the B of VM-CTR-AB-1108 |

All five are on the fork branch `percpu-hot-counters-bisect` (a fixed copy
of `percpu-hot-counters`). Nothing is rebased: A is the master commit the
four commits sit on, so the arms differ only by these commits.
(VM-CTR-AB-1108's A was master `1974111f45`, four commits older; none of
those four is on a path measured here.) Each arm adds one commit as it would
land; commit 2 is written on top of commit 1, so an arm with commit 2 alone
would be untested code. Commit 4 changes only mirror, RAID-Z and dRAID
vdevs: on a single-device pool `B123` and `B1234` run the same code.

**Rows.** Only the rows that moved in VM-CTR-AB-1108, plus controls, each
the same workload as there (16K records, per-job files, psync, compression
off unless said, 50 % compressible data):

| row | role | pool | fio | jobs | VM-CTR-AB-1108 |
|---|---|---|---|---|---|
| `cread-8` | G | main, `plain` | randread 4k | 8 | +18.6 to +25.7 % |
| `cread-16` | G | main, `plain` | randread 4k | 16 | +9.8 to +18.2 % |
| `uread-1` | C | main, `nocache` | randread 16k | 1 | -0.4 % |
| `uread-8` | L | main, `nocache` | randread 16k | 8 | -11.1 % (bound), -5.0 %, +13.1 % |
| `ureadlz4-8` | L | main, `nocachelz4` | randread 16k | 8 | -15.5 % (bound), -3.3 % |
| `uread-16` | L | main, `nocache` | randread 16k | 16 | +9.4 %, -7.8 % (perf record), -16.0 % (perf lock) |
| `awrite-8` | L | main, `wplain` | randwrite 16k | 8 | -4.4 % (bound), -1.1 %, +3.5 % |
| `mir-uread-1` | C | mirror, `nocache` | randread 16k | 1 | -1.9 % |
| `mir-uread-16` | L | mirror, `nocache` | randread 16k | 16 | -5.6 %, -7.8 % (perf record) |
| `mir-awrite-1` | L | mirror, `wplain` | randwrite 16k | 1 | -13.0 % (noise 15.3) |
| `mir-awrite-16` | L | mirror, `wplain` | randwrite 16k | 16 | -18.5 %, -7.1 % (perf record) |
| `swrite-1`, `-2`, `-4` | L | sync, `sync` (sync=always) | randwrite 16k | 1, 2, 4 | -6.6, -9.9, -8.6 % |

L: a row that lost. G: the gain that held, a positive control (if B1234
does not gain there, this run cannot see what VM-CTR-AB-1108 saw). C: 1-job
reads that did not move, null controls. `nocache` datasets have
`primarycache=metadata`. Each run is 20 s after a 5 s ramp (30 s there), no
`perf` (the `hitm` and `remote` columns mean nothing on one node).

**How the arms are interleaved.** Row by row: every run loads its arm's
modules unless they are the loaded ones (export the pools, unload, load,
import, about 10 s), so every run starts on a freshly loaded module with an
empty ARC, in every arm alike. Within a row the arms run back to back; their
order alternates from one row to the next (A B, then B A, ...), pass 2 runs
the rows in reverse order and each row's arms in the reverse of pass 1.

**Two stages per pool phase, and when the bisect stops early.**

- **Stage 1** (always): A and B1234, two passes, every row of the phase.
- **Decision** (`decide.py`), per row, from its two pairs: d1 and d2 are
  B1234 against A in pass 1 and pass 2 (%), n the larger two-pass spread of A
  or of B1234 (% of A). **LOSS** when d1 and d2 are both below -max(3, n);
  **no loss** when both are above -3 % and no run of the row had
  `steal_pct` >= 1; **unclear** otherwise.
- **Stage 2**, only for the LOSS and unclear rows: A, B1, B12, B1234 (and
  B123 on the mirror rows), two passes. Each step between consecutive arms
  is one commit's share of the change.
- **Stop rules.** (1) A phase with no row for stage 2 skips it; if neither
  phase has one, the bisect ends after stage 1 (about 55 min in all). (2) If
  a third or more of a phase's stage-1 runs had `steal_pct` >= 1 (the host
  was busy), that phase stops after its cleanup, without stage 2, and STEP 4
  is not run: report, it will be rerun on a quiet host. (3) The safety stops
  under Rules.

**Noise records.** Per run: `steal_pct` (the share of the time the vCPUs
had work that the hypervisor gave to others), the steal jiffies and the load
average just before the run. Per pass: load average and steal at its start
and end (`passes.txt`, summarised by STEP 5).

## The pools

Those of VM-CTR-AB-1108, laid out the same way (its "How the pools are sized"
gives the bounds): phase P, an 8 GiB RAM disk holding a 6 GiB single-device
pool (`zfsbis`: `plain`, `wplain`, `nocache`, `nocachelz4`, 16 files of 16
MiB each, plus the two 512 MiB shared files, which no row reads but which
keep the pool as full as there, so the write rows run under the same quota
cap) and a 2 x 1000 MiB mirror (`zfsbism`: `nocache`, `wplain`, 16 files of
8 MiB); phase S, a RAM disk sized from `zfs_wrlog_data_max` (8 GiB on a 15
GiB VM) holding `zfsbiss/sync`. `spa_asize_inflation` stays at its default
24 (checked at every module load). Expected after layout: main pool about
34 %, mirror about 26 %, sync pool under 4 %. Every run checks 60 % before
it starts, every write run has a watchdog that stops fio above 60 %, and a
pool above 50 % after its layout stops the phase. The pools are exported and
imported between arms; they persist on the RAM disk and are destroyed at the
end of their phase.

**Duration** (16 vCPUs):

| step | estimate |
|---|---|
| STEP 0 to 2 | 12 min: A built in full (about 4 min with `make -j8`, as on 8/11), each B arm's modules rebuilt in the same tree (about 1 min each) |
| STEP 3, phase P | setup 3 min; stage 1 24 min (44 runs of about 27 s, 22 module switches of about 10 s); stage 2 0 to 36 min for the 7 L rows of P (4.6 min per single-device row, 5.8 per mirror row), 55 min if all 11 rows go |
| STEP 4, phase S | setup 2 min; stage 1 6 min; stage 2 0 to 13 min |
| STEP 5 to 7 | 5 min |

In all: about 55 min if stage 1 clears every row (stop rule 1), about 1 h
40 min if all ten L rows go to stage 2, at most about 2 h.

## Rules

- Disposable VM, release builds, no other build, test or benchmark running.
  Builds run `nice -n 10 ionice -c3`, one at a time.
- STEPS 3 and 4 are a performance run: only while VM 106 and VM 107 are idle
  (you are told when). Do not start them otherwise.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), no `zfs-tests.sh`, never reboot.
- Work only under `/var/tmp/ctr-bisect-1108` (W) and
  `/var/log/ctr-bisect-1108` (L), on `/dev/ram0` and its partitions and the
  pools `zfsbis`, `zfsbism`, `zfsbiss`. Never run `zpool`, `zfs` or `zdb`
  while none of this task's modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:` or
  hung-task message, a pool above 60 %, a suspended pool or an ENOSPC message:
  stop and report. After a stop past STEP 1, go to STEP 5 with what you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: which VM, and is it free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
numactl -H | sed -n '1,3p'
grep -E 'MemTotal|MemAvailable' /proc/meminfo
cat /proc/loadavg; grep '^cpu ' /proc/stat
ls -d /lib/modules/$(uname -r)/build
for t in fio numactl python3 git make gcc sfdisk udevadm blockdev dd findmnt modinfo insmod rmmod; do PATH=/usr/sbin:/sbin:$PATH command -v $t > /dev/null || echo "missing $t"; done
grep -E '^(zfs|spl|brd) ' /proc/modules                         # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/ctr-bisect-1108 /var/log/ctr-bisect-1108 2>&1    # No such file or directory, twice
df -h /var/tmp | tail -1                                       # 5G free or more
```

Stop, changing nothing, if: `sudo -n true` fails; the VM is not 16 vCPUs in
one node (`available: 1 nodes (0)`, `node 0 cpus: 0 ... 15`); MemAvailable is
under 13 GiB; the kernel headers or a tool are missing; a zfs, spl or brd
module is loaded or a pool is imported; a build, test or benchmark runs; W or
L exists; less than 5G is free on `/var/tmp`.

## STEP 1: sources and the five builds (about 10 min)

A is built in full (tools and modules); each B arm checks out its commit in
the same tree and rebuilds only the kernel modules, which recompiles the
files the commit changed. Each arm's `spl.ko` and `zfs.ko` are kept in
`$W/ko/<arm>`.

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/ctr-bisect-1108 L=/var/log/ctr-bisect-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W/ko || exit 1
sudo -n dmesg | wc -l > $L/dmesg.start
git init -q $W/repo && git -C $W/repo fetch -q --depth 8 https://github.com/tigerblue77/zfs percpu-hot-counters-bisect || exit 1
git -C $W/repo rev-parse FETCH_HEAD                  # e0ba7d8c779be98fcf3acaefd91f63960155d1fe
git -C $W/repo worktree add -q --detach $W/tree ef35f144e1d0df22e835970dc9bc5615f4d1df48 || exit 1
{
cd $W/tree || exit 1
for t in A:ef35f144e1d0df22e835970dc9bc5615f4d1df48 B1:7f19b9fa11cbc744eaf5fe6abd32a07161d4ef79 B12:ee45ef6ecaa6faf00e7378893458f069538d7bd7 B123:0db849c2161f8d2b8a45d94efd3aee3a8f65a347 B1234:e0ba7d8c779be98fcf3acaefd91f63960155d1fe; do
	n=${t%%:*} c=${t#*:}
	git checkout -q --detach $c || break
	t0=$(date +%s)
	if [ $n = A ]; then
		nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j8' > $L/build-$n.log 2>&1
	else
		nice -n 10 ionice -c3 make -C module -j8 > $L/build-$n.log 2>&1
	fi
	rc=$?
	echo "$n at $(git rev-parse HEAD) build rc=$rc wall=$(( $(date +%s) - t0 ))s"
	[ $rc = 0 ] || break
	mkdir -p $W/ko/$n && cp module/spl.ko module/zfs.ko $W/ko/$n/ || break
	echo "$n zfs.ko srcversion=$(modinfo -F srcversion $W/ko/$n/zfs.ko) vermagic=$(modinfo -F vermagic $W/ko/$n/zfs.ko | cut -d' ' -f1)"
done
echo "release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' Makefile)"
for t in B1:arc B1:arc_os B12:arc B123:dbuf B1234:vdev; do
	n=${t%%:*} f=${t#*:}
	echo "$n recompiled $f.o: $(grep -cE "CC \[M\] .*[ /]$f\.o\$" $L/build-$n.log 2>/dev/null)"
done
} 2>&1 | tee $L/builds.txt
```

Expected: five `at <its commit> build rc=0` lines; five different
srcversions; `vermagic` = `uname -r` five times; `release=1 noinline=0`;
every `recompiled` count 1 or more. On a failed build quote `grep -nE
'error:|Error [0-9]' $L/build-<arm>.log | head -20` and stop.

## STEP 2: the scripts

`env.sh` holds the settings and `rows.txt` the rows; `arm.sh` loads an arm's
modules and checks them; `run.sh` runs one row on one arm (switching the
modules first if needed); `stage.sh` runs the two passes of a stage;
`phase.sh` one pool phase (layout, stage 1, decision, stage 2, cleanup);
`decide.py` the stage-1 decision; `compare.py` the final tables.

```sh
L=/var/log/ctr-bisect-1108
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the per-commit bisect, sourced by every script.
export PATH=/var/tmp/ctr-bisect-1108/tree:/usr/sbin:/sbin:$PATH
W=/var/tmp/ctr-bisect-1108
L=/var/log/ctr-bisect-1108
K=$W/ko
P=zfsbis
PM=zfsbism
PS=zfsbiss
RUNTIME=20
RAMP=5
EOF_ENV
cat > $L/rows.txt << 'EOF_ROWS'
cread-8 P G mnt/plain randread 4k 8 16m
cread-16 P G mnt/plain randread 4k 16 16m
uread-1 P C mnt/nocache randread 16k 1 16m
uread-8 P L mnt/nocache randread 16k 8 16m
ureadlz4-8 P L mnt/nocachelz4 randread 16k 8 16m
uread-16 P L mnt/nocache randread 16k 16 16m
awrite-8 P L mnt/wplain randwrite 16k 8 16m
mir-uread-1 P C mntm/nocache randread 16k 1 8m
mir-uread-16 P L mntm/nocache randread 16k 16 8m
mir-awrite-1 P L mntm/wplain randwrite 16k 1 8m
mir-awrite-16 P L mntm/wplain randwrite 16k 16 8m
swrite-1 S L mnts/sync randwrite 16k 1 16m
swrite-2 S L mnts/sync randwrite 16k 2 16m
swrite-4 S L mnts/sync randwrite 16k 4 16m
EOF_ROWS
cat > $L/arm.sh << 'EOF_ARM'
#!/bin/bash
# arm.sh ARM: load ARM's spl and zfs modules (no pool may be imported),
# check what got loaded and that spa_asize_inflation is 24, and record ARM
# as the loaded build.
set -u
. /var/log/ctr-bisect-1108/env.sh
a=$1 D=$K/$1
[ -s "$D/zfs.ko" ] && [ -s "$D/spl.ko" ] || { echo "no modules for $a: stopping"; exit 1; }
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2> /dev/null)" ] ||
    { echo "a pool is imported: stopping"; exit 1; }
rm -f "$W/arm"
for m in zfs spl; do
	if [ -d "/sys/module/$m" ]; then
		rmmod "$m" || { echo "rmmod $m failed: stopping"; exit 1; }
	fi
done
modprobe -q zlib_deflate
modprobe -q zlib_inflate
insmod "$D/spl.ko" && insmod "$D/zfs.ko" || { echo "load of $a failed: stopping"; exit 1; }
udevadm settle
[ -c /dev/zfs ] || { echo "no /dev/zfs after loading $a: stopping"; exit 1; }
for m in spl zfs; do
	want=$(modinfo -F srcversion "$D/$m.ko")
	have=$(cat "/sys/module/$m/srcversion")
	[ "$want" = "$have" ] || { echo "$m loaded $have is not $a's $want: stopping"; exit 1; }
done
[ "$(cat /sys/module/zfs/parameters/spa_asize_inflation)" = 24 ] ||
    { echo "spa_asize_inflation is not 24: stopping"; exit 1; }
echo "$a" > "$W/arm"
EOF_ARM
cat > $L/run.sh << 'EOF_RUN'
#!/bin/bash
# run.sh STAGE PASS ROW ARM: one fio run of ROW (rows.txt) on ARM's build.
# If ARM is not the loaded build, export the phase's pools, load ARM's
# modules and import the pools again. The pool's usage is checked against
# 60 % before the run and every 2 s of a write run (fio is stopped above
# it); one line goes to results.txt.
set -u
. /var/log/ctr-bisect-1108/env.sh
stage=$1 pass=$2 row=$3 arm=$4
read -r ph _ dir rw bs nj fsize < <(awk -v r="$row" '$1 == r { print $2, $3, $4, $5, $6, $7, $8 }' "$L/rows.txt")
[ -n "${fsize:-}" ] || { echo "unknown row $row: stopping"; exit 1; }
case $ph in
P) pools=("$P" "$PM") ;;
*) pools=("$PS") ;;
esac
case $dir in
mnt/*) pool=$P ;;
mntm/*) pool=$PM ;;
*) pool=$PS ;;
esac
if [ "$(cat "$W/arm" 2> /dev/null)" != "$arm" ]; then
	zpool export "${pools[@]}" || { echo "export of ${pools[*]} failed: stopping"; exit 1; }
	"$L/arm.sh" "$arm" || exit 1
	for p in "${pools[@]}"; do
		zpool import -d /dev -o cachefile=none "$p" ||
		    { echo "import of $p failed: stopping"; exit 1; }
	done
fi
[ "$(findmnt -n -o SOURCE --target "$W/$dir")" = "$pool/${dir#*/}" ] && [ -f "$W/$dir/f.0" ] ||
    { echo "$W/$dir is not $pool/${dir#*/} with its files: stopping"; exit 1; }
pct() { zpool list -Hp -o size,alloc "$pool" | awk '{ printf "%.1f", 100 * $2 / $1 }'; }
u=$(pct)
awk -v u="$u" 'BEGIN { exit !(u + 0 <= 60) }' ||
    { echo "pool $pool $u % full before $row: stopping"; exit 3; }
# jiffies of all vCPUs: busy (steal excluded), idle + iowait, steal
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8, $5 + $6, $9 }' /proc/stat; }
quota() { awk '$1 == "dmu_tx_quota" { print $3 }' /proc/spl/kstat/zfs/dmu_tx; }
o=$L/raw/$stage.$pass.$row.$arm
la=$(cut -d' ' -f1 /proc/loadavg)
read -r b0 i0 s0 < <(cpu)
q0=$(quota)
fio --name=t --directory="$W/$dir" --filename_format='f.$jobnum' \
    --size="$fsize" --rw="$rw" --bs="$bs" --ioengine=psync --numjobs="$nj" \
    --time_based --runtime="$RUNTIME" --ramp_time="$RAMP" --group_reporting \
    --fallocate=none --refill_buffers --buffer_compress_percentage=50 \
    --output-format=json --output="$o.fio.json" > "$o.fio.log" 2>&1 &
fpid=$!
wpid=""
case $rw in
*write*)
	(
		peak=0
		while kill -0 "$fpid" 2> /dev/null; do
			sleep 2
			u=$(pct)
			peak=$(awk -v a="$peak" -v b="$u" 'BEGIN { print (b + 0 > a + 0) ? b : a }')
			echo "$peak" > "$o.peak"
			if awk -v u="$u" 'BEGIN { exit !(u + 0 > 60) }'; then
				echo "pool $pool $u % full during $row: fio stopped" > "$o.stopped"
				kill -INT "$fpid"
				break
			fi
		done
	) &
	wpid=$!
	;;
esac
wait "$fpid"
frc=$?
[ -n "$wpid" ] && wait "$wpid"
read -r b1 i1 s1 < <(cpu)
q1=$(quota)
fio_cols=$(python3 -I - "$o.fio.json" "$rw" << 'PY'
import json, sys
try:
    j = json.load(open(sys.argv[1]))["jobs"][0]
    x = j["read" if "read" in sys.argv[2] else "write"]
    p = x.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0)
    print("%.0f %.1f %.0f" % (x["iops"], x["bw"] / 1024.0, p / 1000.0))
except Exception:
    print("nan nan nan")
PY
)
read -r iops _ _ <<< "$fio_cols"
db=$((b1 - b0)) di=$((i1 - i0)) ds=$((s1 - s0))
busy=$(awk -v b="$db" -v i="$di" -v s="$ds" 'BEGIN { printf "%.0f", 100 * b / (b + i + s) }')
# steal: share of the time the vCPUs had work (busy + steal) the host gave away
steal=$(awk -v b="$db" -v s="$ds" 'BEGIN { printf "%.2f", (b + s > 0 ? 100 * s / (b + s) : 0) }')
arcc=$(awk '$1 == "c" { printf "%.0f", $3 / 1048576 }' /proc/spl/kstat/zfs/arcstats)
peak=$(cat "$o.peak" 2> /dev/null || echo -)
line="$stage $pass $ph $row $arm $fio_cols $busy $steal $ds $((q1 - q0)) $peak $la $arcc $(date -u +%T)"
echo "$line" >> "$L/results.txt"
echo "$line"
if [ -e "$o.stopped" ]; then
	cat "$o.stopped"
	exit 4
fi
[ "$frc" = 0 ] || { echo "fio exited $frc (see $o.fio.log): stopping"; exit 1; }
[ "$iops" != nan ] || { echo "no IOPS in $o.fio.json: stopping"; exit 1; }
EOF_RUN
cat > $L/stage.sh << 'EOF_STAGE'
#!/bin/bash
# stage.sh STAGE PHASE ROWFILE: two passes over the rows listed in ROWFILE,
# pass 2 in reverse row order. Each row runs its arms back to back (stage
# 1: A and B1234; stage 2: A, B1, B12, B1234, plus B123 on the mirror rows)
# in an order that alternates from one row to the next, so that pass 2 runs
# every row's arms in the reverse of their pass-1 order. The guest's steal
# time and load average are recorded at the start and end of every pass.
set -u
. /var/log/ctr-bisect-1108/env.sh
stage=$1 ph=$2 rf=$3
mapfile -t rows < <(awk 'NF { print $1 }' "$rf")
[ "${#rows[@]}" -gt 0 ] || { echo "stage $stage phase $ph: no rows"; exit 0; }
mark() {
	echo "stage $stage phase $ph pass $1 $2 $(date -u +%T)" \
	    "loadavg $(cut -d' ' -f1-3 /proc/loadavg)" \
	    "$(awk '/^cpu / { print "steal_jiffies", $9, "all_jiffies", $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9 }' /proc/stat)" |
	    tee -a "$L/passes.txt"
}
fwd=1
for pass in 1 2; do
	order=("${rows[@]}")
	if [ "$pass" = 2 ]; then
		order=()
		for ((i = ${#rows[@]} - 1; i >= 0; i--)); do order+=("${rows[$i]}"); done
	fi
	mark "$pass" start
	for row in "${order[@]}"; do
		if [ "$stage" = 1 ]; then
			arms=(A B1234)
		elif awk -v r="$row" '$1 == r && $4 ~ /^mntm\// { f = 1 } END { exit !f }' "$L/rows.txt"; then
			arms=(A B1 B12 B123 B1234)
		else
			arms=(A B1 B12 B1234)
		fi
		if [ "$fwd" = 0 ]; then
			rev=()
			for ((i = ${#arms[@]} - 1; i >= 0; i--)); do rev+=("${arms[$i]}"); done
			arms=("${rev[@]}")
		fi
		for arm in "${arms[@]}"; do
			"$L/run.sh" "$stage" "$pass" "$row" "$arm" < /dev/null || exit $?
		done
		fwd=$((1 - fwd))
	done
	mark "$pass" end
done
EOF_STAGE
cat > $L/phase.sh << 'EOF_PHASE'
#!/bin/bash
# phase.sh P|S: lay out the phase's pools on A's build, run stage 1 (A and
# B1234) over the phase's rows, let decide.py pick the rows of stage 2, run
# stage 2 on them, then destroy the pools and the RAM disk (also on a
# failure). The pools are those of VM-CTR-AB-1108:
#   P: an 8 GiB RAM disk, a 6 GiB single-device pool and a 2 x 1000 MiB
#      mirror, laid out as there (the shared files included);
#   S: a RAM disk sized from zfs_wrlog_data_max, the sync dataset only.
set -u
. /var/log/ctr-bisect-1108/env.sh
ph=$1
case $ph in
P | S) ;;
*) echo "usage: phase.sh P|S"; exit 1 ;;
esac
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
"$L/arm.sh" A || exit 1
for p in "$P" "$PM" "$PS"; do
	zpool list "$p" > /dev/null 2>&1 && { echo "pool $p exists: stopping"; exit 1; }
done
M=/sys/module/zfs/parameters
wl=$(cat "$M/zfs_wrlog_data_max")
# Sync pool: live 0.26 GiB, log in flight up to 4 txgs of 15 % of
# zfs_wrlog_data_max at 1.25 bytes allocated per byte logged, data in
# flight up to 3 txgs of 0.25 GiB, all under 45 % of the pool.
SG=$(awk -v w="$wl" 'BEGIN { need = 0.26 + 0.75 * w / 2^30 + 0.75
    s = int(need / 0.45 + 0.5 + 0.999); if (s < 8) s = 8; print s }')
need=$(( (SG > 8 ? SG : 8) + 5 ))
avail=$(awk '/^MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$avail" -ge "$need" ] ||
    { echo "MemAvailable ${avail} GiB < ${need} GiB: stopping"; exit 1; }
mkdir -p "$L/raw" "$W/mnt" "$W/mntm" "$W/mnts" && cd "$L" || exit 1
cleanup() {
	cd / || return
	for p in "$PS" "$PM" "$P"; do
		zpool list "$p" > /dev/null 2>&1 && { zpool destroy "$p" || echo "zpool destroy $p failed"; }
	done
	if grep -q '^brd ' /proc/modules; then
		rmmod brd || echo "rmmod brd failed"
	fi
}
trap cleanup EXIT
usage() {
	for p in "$@"; do
		zpool list -Hp -o name,size,alloc "$p" |
		    awk '{ printf "%s size %d alloc %d (%.1f %%)\n", $1, $2, $3, 100 * $3 / $2 }'
	done
}
# lay TAG DIR FIO_ARGS...: write the files of a dataset.
lay() {
	local tag=$1 dir=$2
	shift 2
	fio --name=layout --directory="$W/$dir" "$@" --bs=1m --rw=write \
	    --ioengine=psync --refill_buffers --buffer_compress_percentage=50 \
	    --fallocate=none --end_fsync=1 > "raw/layout-$tag.log" 2>&1
}
{ echo "phase $ph $(date -u '+%F %T') host $(hostname) nproc $(nproc) MemAvailable ${avail} GiB"
  for p in spa_asize_inflation zfs_dirty_data_max zfs_wrlog_data_max zfs_arc_max zfs_arc_min; do
	echo "$p=$(cat "$M/$p")"
  done; } | tee -a phases.txt
if [ "$ph" = P ]; then
	modprobe brd rd_nr=1 rd_size=$((8 * 1048576)) max_part=16 || exit 1
	printf 'label: gpt\nsize=6GiB\nsize=1000MiB\nsize=1000MiB\n' |
	    sfdisk -q /dev/ram0 > /dev/null || exit 1
	udevadm settle
	for n in 1 2 3; do
		[ -b /dev/ram0p$n ] || { echo "no /dev/ram0p$n: stopping"; exit 1; }
		dd if=/dev/zero of=/dev/ram0p$n bs=1M oflag=direct status=none \
		    count=$(( $(blockdev --getsize64 /dev/ram0p$n) / 1048576 )) || exit 1
	done
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mnt" "$P" /dev/ram0p1 || exit 1
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mntm" "$PM" mirror /dev/ram0p2 /dev/ram0p3 ||
	    exit 1
	zfs create "$P/plain" && zfs create "$P/wplain" &&
	    zfs create -o primarycache=metadata "$P/nocache" &&
	    zfs create -o primarycache=metadata -o compression=lz4 "$P/nocachelz4" &&
	    zfs create -o primarycache=metadata "$PM/nocache" && zfs create "$PM/wplain" ||
	    exit 1
	for ds in mnt/plain mnt/wplain mnt/nocache mnt/nocachelz4 mntm/nocache mntm/wplain; do
		case $ds in mntm/*) fs=8m ;; *) fs=16m ;; esac
		lay "${ds//\//-}" "$ds" --filename_format='f.$jobnum' --size=$fs --numjobs=16 || exit 1
	done
	# Not read here: they keep the main pool as full as in VM-CTR-AB-1108,
	# so that the write rows run under the same quota cap.
	for ds in plain wplain; do
		lay "mnt-$ds-shared" "mnt/$ds" --filename=shared.0 --size=512m || exit 1
	done
	pools=("$P" "$PM")
else
	modprobe brd rd_nr=1 rd_size=$((SG * 1048576)) max_part=16 || exit 1
	dd if=/dev/zero of=/dev/ram0 bs=1M count=$((SG * 1024)) oflag=direct status=none || exit 1
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mnts" "$PS" /dev/ram0 || exit 1
	zfs create "$PS/sync" || exit 1
	lay mnts-sync mnts/sync --filename_format='f.$jobnum' --size=16m --numjobs=16 || exit 1
	zfs set sync=always "$PS/sync" || exit 1
	pools=("$PS")
fi
zpool sync "${pools[@]}" || exit 1
usage "${pools[@]}" | sed "s/^/phase $ph after layout: /" | tee -a phases.txt
usage "${pools[@]}" | awk '{ gsub(/[(]/, "", $6); if ($6 + 0 > 50) bad = 1 } END { exit bad }' ||
    { echo "a pool is above 50 % after its layout: stopping"; exit 1; }
awk -v ph="$ph" '$2 == ph { print $1 }' rows.txt > "stage1-$ph.txt"
"$L/stage.sh" 1 "$ph" "$L/stage1-$ph.txt" || exit $?
python3 -I "$L/decide.py" "$ph" | tee "decide-$ph.txt"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || { echo "decide.py exited $rc: no stage 2 in phase $ph"; exit "$rc"; }
if [ -s "stage2-$ph.txt" ]; then
	"$L/stage.sh" 2 "$ph" "$L/stage2-$ph.txt" || exit $?
else
	echo "phase $ph: no row goes to stage 2"
fi
usage "${pools[@]}" | sed "s/^/phase $ph end: /" | tee -a phases.txt
echo "phase $ph done: $(awk -v ph="$ph" '$3 == ph' results.txt | wc -l) runs"
EOF_PHASE
cat > $L/decide.py << 'EOF_DECIDE'
#!/usr/bin/env python3
# decide.py PHASE: classify every row of the phase from its stage-1 runs (A
# and B1234, two passes, each pass pairing the two arms run back to back)
# and write the rows that go to stage 2 to stage2-PHASE.txt.
#   d1, d2  B1234 against A in pass 1 and in pass 2, in %
#   n       the larger two-pass spread of A or of B1234, in % of A
#   LOSS    d1 and d2 both below -max(3, n)
#   no loss d1 and d2 both above -3 and no run with steal_pct >= 1
#           ("gain" when both are also above max(3, n))
#   unclear anything else
# LOSS and unclear rows go to stage 2. Exit 5, with no stage 2, when a
# third of the phase's runs or more had steal_pct >= 1 (a busy host).
import os
import sys

L = "/var/log/ctr-bisect-1108"
STEAL = 1.0
ph = sys.argv[1]
rows = [r.split() for r in open(os.path.join(L, "rows.txt")) if r.split()]
iops, steal = {}, {}
for f in [r.split() for r in open(os.path.join(L, "results.txt"))][1:]:
    if len(f) >= 10 and f[0] == "1" and f[2] == ph:
        iops[(f[3], f[4], f[1])] = float(f[5])
        steal[(f[3], f[4], f[1])] = float(f[9])
out = ["%-14s %-4s %9s %9s %9s %9s %7s %7s %6s %6s  %s" % ("row", "role",
    "A p1", "B1234 p1", "B1234 p2", "A p2", "d1%", "d2%", "n%", "steal",
    "verdict")]
sel = []
for r in rows:
    if r[1] != ph:
        continue
    row, role = r[0], r[2]
    try:
        a1, b1 = iops[(row, "A", "1")], iops[(row, "B1234", "1")]
        a2, b2 = iops[(row, "A", "2")], iops[(row, "B1234", "2")]
    except KeyError:
        print("row %s: stage-1 runs missing" % row)
        sys.exit(1)
    d1 = 100 * (b1 - a1) / a1
    d2 = 100 * (b2 - a2) / a2
    n = 100 * max(abs(a1 - a2), abs(b1 - b2)) / ((a1 + a2) / 2)
    t = max(3.0, n)
    s = max(steal[(row, x, p)] for x in ("A", "B1234") for p in ("1", "2"))
    if d1 < -t and d2 < -t:
        v = "LOSS"
    elif d1 > -3 and d2 > -3 and s < STEAL:
        v = "gain" if d1 > t and d2 > t else "no loss"
    else:
        v = "unclear" + (" (steal)" if s >= STEAL else "")
    if v == "LOSS" or v.startswith("unclear"):
        sel.append(row)
    if role == "G" and v != "gain":
        v += "; positive control without B's gain"
    out.append("%-14s %-4s %9.0f %9.0f %9.0f %9.0f %+7.1f %+7.1f %6.1f %6.2f  %s"
        % (row, role, a1, b1, b2, a2, d1, d2, n, s, v))
nbusy = sum(1 for x in steal.values() if x >= STEAL)
busy = 3 * nbusy >= len(steal) > 0
out.append("phase %s stage-1 runs with steal_pct >= %.0f: %d of %d"
    % (ph, STEAL, nbusy, len(steal)))
if busy:
    out.append("HOST BUSY: no stage 2; stop after this phase")
    sel = []
out.append("stage 2 rows of phase %s: %s" % (ph, " ".join(sel) or "none"))
with open(os.path.join(L, "stage2-%s.txt" % ph), "w") as fh:
    fh.write("".join(x + "\n" for x in sel))
print("\n".join(out))
sys.exit(5 if busy else 0)
EOF_DECIDE
cat > $L/compare.py << 'EOF_CMP'
#!/usr/bin/env python3
# compare.py: the tables of the bisect, from results.txt and passes.txt.
#  - per pass: steal (% of all vCPU time) and load average, start and end;
#  - per stage-2 row: IOPS of every arm in both passes, the step of every
#    commit in % of that pass's A (they add up to B1234 against A), and the
#    verdict over the four A/B1234 pairs of stages 1 and 2.
# A step is marked * (loss) or + (gain) when it is beyond max(3, n2) in
# both passes, n2 being the larger two-pass spread of the arms of the row.
import os

L = "/var/log/ctr-bisect-1108"
rows = [r.split() for r in open(os.path.join(L, "rows.txt")) if r.split()]
res = [r.split() for r in open(os.path.join(L, "results.txt"))][1:]
v = {}
for f in res:
    if len(f) >= 10:
        v[(f[0], f[1], f[3], f[4])] = f
out = ["== passes: steal % of all vCPU time, load average at start and end"]
marks = [r.split() for r in open(os.path.join(L, "passes.txt"))] \
    if os.path.exists(os.path.join(L, "passes.txt")) else []
start = {}
for m in marks:
    k = (m[1], m[3], m[5])
    st, al = int(m[13]), int(m[15])
    if m[6] == "start":
        start[k] = (m[7], " ".join(m[9:12]), st, al)
    elif k in start:
        t0, l0, s0, a0 = start[k]
        out.append("stage %s phase %s pass %s  %s..%s  steal %.3f %%  load %s .. %s"
            % (k[0], k[1], k[2], t0, m[7],
               100.0 * (st - s0) / max(al - a0, 1), l0, " ".join(m[9:12])))
out.append("")
out.append("== stage 2: IOPS per arm, then each commit's step in % of A")
out.append("   c1 = B1-A, c2 = B12-B1, c3 = B123-B12 (B1234-B12 on a"
    " single-device pool, where commit 4 changes nothing), c4 = B1234-B123")
any2 = False
for r in rows:
    row = r[0]
    mirror = r[3].startswith("mntm/")
    arms = ["A", "B1", "B12", "B123", "B1234"] if mirror else \
        ["A", "B1", "B12", "B1234"]
    if not any(("2", p, row, a) in v for p in ("1", "2") for a in arms):
        continue
    any2 = True
    x = {}
    for p in ("1", "2"):
        for a in arms:
            f = v.get(("2", p, row, a))
            x[(p, a)] = float(f[5]) if f else float("nan")
    out.append("%s (%s)" % (row, r[2]))
    for p in ("1", "2"):
        out.append("  pass %s  " % p + "  ".join("%s %.0f" % (a, x[(p, a)])
            for a in arms) + "  steal max %.2f" % max((float(v[("2", p, row, a)][9])
            for a in arms if ("2", p, row, a) in v), default=float("nan")))
    am = (x[("1", "A")] + x[("2", "A")]) / 2
    n2 = max(100 * abs(x[("1", a)] - x[("2", a)]) / am for a in arms)
    t = max(3.0, n2)
    steps = list(zip(arms[1:], arms[:-1]))
    names = ["c1", "c2", "c3", "c4"] if mirror else ["c1", "c2", "c3"]
    marks2 = []
    line = "  steps (pass 1 / pass 2):"
    for name, (hi, lo) in zip(names, steps):
        s1 = 100 * (x[("1", hi)] - x[("1", lo)]) / x[("1", "A")]
        s2 = 100 * (x[("2", hi)] - x[("2", lo)]) / x[("2", "A")]
        mk = "*" if s1 < -t and s2 < -t else "+" if s1 > t and s2 > t else ""
        if mk == "*":
            marks2.append(name)
        line += "  %s %+.1f / %+.1f%s" % (name, s1, s2, mk)
    out.append(line + "   n2 %.1f %%" % n2)
    d = []
    for st in ("1", "2"):
        for p in ("1", "2"):
            a, b = v.get((st, p, row, "A")), v.get((st, p, row, "B1234"))
            if a and b:
                d.append(100 * (float(b[5]) - float(a[5])) / float(a[5]))
    mean = sum(d) / len(d) if d else float("nan")
    holds = len(d) == 4 and sum(1 for y in d if y < -3) >= 3 and mean < -3
    if not holds:
        verdict = "loss does not hold"
    elif len(marks2) == 1:
        verdict = "loss holds, carried by commit %s" % marks2[0][1]
    elif marks2:
        verdict = "loss holds, steps %s each beyond the noise" % "+".join(marks2)
    else:
        verdict = "loss holds, no single step beyond the noise"
    out.append("  B1234/A over the 4 pairs: %s  mean %+.1f %%: %s"
        % (" ".join("%+.1f" % y for y in d), mean, verdict))
if not any2:
    out.append("(no stage 2)")
print("\n".join(out))
EOF_CMP
chmod 0755 $L/*.sh $L/*.py
echo "stage pass phase row arm iops MiBps p99_us cpu_pct steal_pct steal_jiffies quota peak_pct load1 arc_c_MiB time" > $L/results.txt
cd $L && sha256sum env.sh rows.txt arm.sh run.sh stage.sh phase.sh decide.py compare.py
```

Must print:
```
024df40adb47fcf289f948a8a4ce64c765254ab5afa04044b1b87939bd637f42  env.sh
2e025c66ab936f48e03cc7bb7a9750fbd48f99c555feef0e6db4f6c5e5be9876  rows.txt
79443dd7a89fe6cd2e342715c7a7ace44ee7831c2007ac961b721d29945bbc64  arm.sh
e8a09423d382592c81203c112e2b390311184d9e376652cc6f3695c7140f1c7a  run.sh
871c627601b700aa0df40d7d87671cd5e6fabcae63fb2364ccc49df1b080f64b  stage.sh
0b3982591a2abe36abff1966f0b45b7004038cea847e1138038dc821bedd4311  phase.sh
306b688f859a747a2a7acaa45d5cc46d55e06ff45b3c5801ee5eddd3389154ad  decide.py
6e1535e1e5307da166ba15a1b4934c7fb934488dc7d1d39661ffb32287e98884  compare.py
```

## STEP 3: phase P (about 30 min, up to 1 h 25 min)

Only while VM 106 and VM 107 are idle. One command:

```sh
sudo -n bash -c '. /var/log/ctr-bisect-1108/env.sh
"$L/phase.sh" P; echo "exit $?"' 2>&1 | tee /var/log/ctr-bisect-1108/phase-P.log
```

It prints the module parameters, the `after layout` lines (`zfsbis` near 34
%, `zfsbism` near 26 %), one line per run (stage, pass, phase, row, arm,
IOPS, MiB/s, p99, cpu %, steal %, steal jiffies, quota, peak %, load1, ARC
target, time) and `stage ... pass ... start|end` lines, then the decision
table and `stage 2 rows of phase P: ...`, the stage-2 runs if any, the `end`
usage lines, `phase P done: N runs` (44 without stage 2) and `exit 0`.

`exit 5` means the host was busy (stop rule 2): skip STEP 4 and go to STEP
5. Any other non-zero exit is a stop: go to STEP 5 with what you have.

## STEP 4: phase S (about 8 min, up to 21 min)

Only after STEP 3 printed `exit 0`, and only while VM 106 and VM 107 are
idle:

```sh
sudo -n bash -c '. /var/log/ctr-bisect-1108/env.sh
"$L/phase.sh" S; echo "exit $?"' 2>&1 | tee /var/log/ctr-bisect-1108/phase-S.log
```

The same output for the sync pool (`zfsbiss` under 4 % after layout, 12 runs
without stage 2), ending with `exit 0`.

## STEP 5: the tables

```sh
L=/var/log/ctr-bisect-1108
tail -n 1 $L/phase-P.log $L/phase-S.log 2>&1
python3 -I $L/compare.py | tee $L/compare.txt
awk 'NR > 1 && $13 != "-" { if ($13 + 0 > m) m = $13 + 0 } END { print "highest pool usage seen: " m + 0 " %" }' $L/results.txt
awk 'NR > 1 { t++; if ($10 + 0 >= 1) n++ } END { print "runs with steal_pct >= 1: " n + 0 " of " t + 0 }' $L/results.txt
sudo -n dmesg | tail -n +$(( $(cat $L/dmesg.start) + 1 )) | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|suspended|ENOSPC'   # 0
```

## STEP 6: cleanup

The phases already destroyed their pools and RAM disks.

```sh
W=/var/tmp/ctr-bisect-1108 L=/var/log/ctr-bisect-1108
sudo -n bash -c '. /var/log/ctr-bisect-1108/env.sh
if [ -d /sys/module/zfs ]; then
	for p in "$P" "$PM" "$PS"; do zpool list -H -o name "$p" 2> /dev/null && zpool destroy "$p"; done
fi
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null
grep -q "^brd " /proc/modules && { rmmod brd || echo "rmmod brd failed"; }
for m in zfs spl; do [ -d /sys/module/$m ] && { rmmod $m || echo "rmmod $m failed"; }; done
grep -E "^(zfs|spl|brd) " /proc/modules'
sudo -n rm -rf $W && echo "work dir removed; logs kept in $L"
du -sh $L
sudo -n fstrim -av
```

Expected: no pool, nothing loaded, `work dir removed`, the size of L, the
fstrim summary.

## STEP 7: report

```sh
L=/var/log/ctr-bisect-1108
{
echo "== builds"; cat $L/builds.txt
echo "== phases"; cat $L/phases.txt
echo "== stage 1 decisions"; cat $L/decide-P.txt $L/decide-S.txt
echo "== tables"; cat $L/compare.txt
echo "== results"; cat $L/results.txt
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed, and at
its end the last lines of STEP 3 and STEP 4, the last four lines of STEP 5,
the output of STEP 6 and every deviation from this document (what and why).
Reply with the whole of `REPORT.txt`.

## How to read the results

- **Stage 1** (`decide-P.txt`, `decide-S.txt`): one line per row with the
  four IOPS (A and B1234 in pass 1, B1234 and A in pass 2), d1, d2, n, the
  highest `steal_pct` of the row's runs and the verdict. `gain` on both
  `cread` rows is the expected positive control; a `cread` row without it
  is flagged.
- **Stage 2** (`compare.txt`): per row, every arm's IOPS in both passes,
  then each commit's step in % of that pass's A (c1 = B1 - A, c2 = B12 - B1,
  c3 = B123 - B12, or B1234 - B12 on a single-device pool, c4 = B1234 -
  B123), marked `*` (loss) or `+` (gain) when beyond max(3, n2) in both
  passes, n2 being the larger two-pass spread of the row's arms. The verdict
  uses the four A/B1234 pairs of the two stages: a loss holds when three of
  them or more are below -3 % and their mean is too; it is carried by the
  commit whose step is the only one marked `*`.
- **What the code leads to expect.** Commits 1 and 3 replace atomics that a
  cached read updates on one global line, hence the `cread` gain. Commit 2 is
  the only one that puts a lock (an aggsum bucket mutex, shared by two vCPUs
  here) on the uncached-read and write paths, a few hundred ns per I/O at
  most: invisible at 1 job, where a sync write takes about 175 us. Commit 4
  can change only the `mir-*` rows.
- **Regime.** Every run starts on a freshly loaded module: the ARC is empty
  and its target starts at `arc_c_min` (1/32 of the memory) and grows during
  the run (`arc_c_MiB` is its value at the end); in VM-CTR-AB-1108 the ARC
  had grown over a 35 min pass (1.1 to 1.9 GiB at the end of the layout).
  Runs are 20 s instead of 30. Compare arms within this run, not with
  VM-CTR-AB-1108's absolute numbers. An effect that needs a long-lived ARC
  (eviction, the shrinker over minutes) cannot show here.
- **Noise.** `steal_pct` near 0 means the vCPUs ran whenever they had work.
  A pass whose steal or load stands out in the `passes` table makes its rows
  suspect; say which. `quota` is the `dmu_tx_quota` count of the run (0 on
  reads; in the thousands on the quota-capped write rows, in every arm
  alike), `peak_pct` the highest pool usage seen in a write run (under 50 %
  expected).

sha256 of this file without this last line and the blank line before it: b5e2215d41e79aa3495b0be584c996d24c1fa1422260a4e212267affc1399574
