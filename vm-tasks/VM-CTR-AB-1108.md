# VM task: A/B of per-CPU hot counters, rerun on pools that cannot fill (8/11)

These instructions are for one of the test VMs: VM 105 or VM 106 (16 vCPUs,
one NUMA node), or VM 107 in its new shape (32 vCPUs in two nodes of 16, 32
GiB). STEP 0 records which one it is; the passes need 13 GiB of available
memory on the 16-vCPU VMs (VM 106 had 11 GiB on 5 November: STEP 0 stops if
it still has). Reply with `/var/log/ctr-ab-1108/REPORT.txt` (STEP 7).

**Background.** The A/B of 6 November (VM-CTR-AB-1106, VM 107) stopped in
its first pass: its 4 GiB test pool, already 58 % full with the files laid
out (2.3 GiB), was run with `spa_asize_inflation=6`; during the sync-write
runs the log blocks and the data in flight took the remaining 1.5 GiB, an
allocation in the txg sync failed (ENOSPC) and the pool suspended. This rerun
keeps the builds, the runs and the measured figures of VM-CTR-AB-1106 and
changes only the pools: `spa_asize_inflation` stays at its default 24
(checked at every module load), each pool is sized so that no run takes it
past 50 %, every run checks 60 % before it starts, and every write run has a
watchdog that stops fio above 60 % and stops the pass.

**What is compared.**

| tree | commit | what it is |
|---|---|---|
| `A` | `1974111f45abdd9e9b76d111b3b046fa172e405b` | upstream master, the A of VM-CTR-AB-1106 |
| `B` | `e0ba7d8c779be98fcf3acaefd91f63960155d1fe` | fork branch `percpu-hot-counters`, 4 commits on master `ef35f144e1` |

B's commits: (1) `arcs_esize[]` of every ARC state: atomic -> wmsum; (2)
`arcs_size[]`: atomic -> wmsum, except arc_anon -> aggsum read through its
lower bound in `arc_tempreserve_space()`; (3) `dbuf_caches[].size`: atomic ->
aggsum; (4) `vdev_stat_update()` no longer takes `vdev_stat_lock` of a mirror
or RAID-Z vdev on success (only the mirror runs can show it). B's base is 4
upstream commits after A (`dmu_recv`, ZIL commit while suspended, direct
reads of blocks pending free, decrypt on hold); from reading, none is on a
path these runs use. Nothing is rebased. Three passes, A1 (A), B, A2 (A):
A1 against A2 gives the noise.

**The runs**, unchanged from VM-CTR-AB-1106 (same fio jobs, files, `perf`
modes and columns): the cross-socket penalty at 8 jobs (`numa-*`, bound to
node A or unbound: cread, uread, ureadlz4, awrite), the scaling at 1, 2, 4,
8 and 16 jobs (`scale-*`: cread, uread, awrite, swrite), one shared 512 MiB
file at 16 jobs (`shared-*`), `perf record` (and `perf c2c`, `perf lock` when
STEP 3 finds them) at 16 jobs (`prof-*`, `c2c-*`, `lock-*`), and the mirror
pool at 1 and 16 jobs (`mir-*`). Two columns are added at the end of each
row: `quota` (the `dmu_tx_quota` count of the run) and `peak%` (the highest
pool usage the watchdog saw in a write run). On a one-node VM the `numa-*-A`
rows are bound to the only node, so they differ from `numa-*-all` only by
`numactl`.

## How the pools are sized

A run can only fill its pool with what stays allocated beyond the files laid
out before it: overwritten blocks are freed when their txg syncs and can be
allocated again two txgs later, so a run that writes 100 GiB over a 256 MiB
file holds, at most, its live files plus what a few txgs carry. What one
txg carries is bounded by the code, which gives the peak of each workload:

- **Async writes** (`awrite`, `shared-awrite`, `mir-awrite`). The quota check
  at the pool's root refuses a transaction once the dirty data of all
  unsynced txgs, counted 24 times, reaches the free space plus 1/32 of the
  pool, so the unique blocks dirtied by the txgs in flight stay under
  (free + 1/32) / 24. At most three txgs' worth of old copies are waiting to
  be reused (the one syncing and the two deferred).
- **Sync writes** (`swrite`, sync=always, 16K). Every write also goes to the
  intent log, and on a pool without a log device the log blocks come from
  the same space but are not counted by that check. The log of one txg is
  bounded by the txg it triggers at 15 % of `zfs_wrlog_data_max` (default 2 x
  `zfs_dirty_data_max`, that is 20 % of the RAM), at most four txgs of log
  blocks are allocated at once (the open one, the syncing one, two deferred),
  and a block holds at least 0.8 byte of log per byte allocated (one 16K
  record per block at 1 job): log in flight <= 4 x 0.15 x 1.25 x
  `zfs_wrlog_data_max` = 0.75 x `zfs_wrlog_data_max`, plus three txgs of
  data, at most 3 x 256 MiB at 16 jobs.

| pool | phase | size | live after layout | peak bound | peak % |
|---|---|---|---|---|---|
| main (plain, wplain, nocache, nocachelz4), 1 device | P | 6 GiB partition (5.75 GiB of metaslabs) | 1.89 GiB (33 %) | + 3 x (5.57 + 0.17 - 1.89) / 24 GiB = 0.48 GiB | 41 % |
| mirror (nocache, wplain) | P | 2 x 1000 MiB (992 MiB) | 256 MiB (26 %) | + 3 x (864 + 27 - 256) / 24 MiB = 80 MiB | 34 % |
| sync, 1 device | S | from `zfs_wrlog_data_max`: (0.26 + 0.75 x wrlog + 0.75) / 0.45 GiB, at least 8 | 0.26 GiB | + 0.75 x wrlog + 0.75 GiB | 45 % at most |

The sync pool comes out at 8 GiB on a 15 GiB VM (`zfs_wrlog_data_max` about
2.9 GiB: peak 3.2 GiB of 7.75, 41 %) and 14 GiB on VM 107 with 32 GiB (about
6.3 GiB: peak 5.7 GiB of 13.5, 42 %). The two pools do not fit in RAM
together on the 16-vCPU VMs, so each pass runs in two phases on two RAM
disks, one after the other: P (8 GiB: everything but the sync writes) then S
(the sync pool: `scale-swrite-1` to `-16`, which on 6 November ran right
after `scale-awrite-*`). In phase P the main pool's memory is on node A, the
mirror's on node B (as in VM-CTR-AB-1106); in phase S half and half.

With the default 24, the quota check also caps the txgs of the async writes
that dirty more than about 80 MiB per txg (`awrite` at 8 and 16 jobs,
`shared-awrite-16`, `mir-awrite-16`): those rows run with a non-zero `quota`
column, as the baseline of 5 November did on its 4 GiB pool, in A and B
alike. Removing that cap would take a main pool of about 14 GiB, more RAM
than the 16-vCPU VMs have.

**Duration**: STEP 1 about 25 min on 16 vCPUs (15 on 32); STEP 4 three
passes of about 35 min (phase P about 28 min, phase S about 5 min): 1 h 45
min; cleanup 5 min. In all about 2 h 20 min (2 h 10 min on VM 107).

## Rules

- Disposable VM, release builds, no other build, test or benchmark running.
  Builds run `nice -n 10 ionice -c3`, one tree at a time.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), no `zfs-tests.sh`, never reboot.
- Work only under `/var/tmp/ctr-ab-1108` (W) and `/var/log/ctr-ab-1108` (L),
  on `/dev/ram0` and its partitions and the pools `zfsnuma`, `zfsnumam`,
  `zfsnumas`. Never run `zpool`, `zfs` or `zdb` while none of this task's
  modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:` or
  hung-task message, a pool above 60 %, a suspended pool or an ENOSPC message:
  stop and report. After a stop past STEP 1, go to STEP 6 with what you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: which VM, and is it free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
numactl -H | sed -n '1,3p'
grep -E 'MemTotal|MemAvailable' /proc/meminfo
ls -d /lib/modules/$(uname -r)/build
for t in fio numactl perf python3 git make gcc sfdisk udevadm blockdev dd; do command -v $t > /dev/null || echo "missing $t"; done
grep -E '^(zfs|spl|brd) ' /proc/modules                         # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/ctr-ab-1108 /var/log/ctr-ab-1108 2>&1          # No such file or directory, twice
df -h /var/tmp | tail -1                                       # 10G free or more
```

Stop, changing nothing, if: `sudo -n true` fails; the VM is neither 16 vCPUs
in one node nor 32 vCPUs in two nodes of 16 (`node 0 cpus: 0 ... 15`, `node 1
cpus: 16 ... 31`); MemAvailable is under 13 GiB (16 vCPUs) or 19 GiB (32
vCPUs); the kernel headers or a tool are missing; a zfs, spl or brd module is
loaded or a pool is imported; a build, test or benchmark runs; W or L exists;
less than 10G is free on `/var/tmp`.

## STEP 1: sources and two release builds (about 25 min)

```sh
W=/var/tmp/ctr-ab-1108 L=/var/log/ctr-ab-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W || exit 1
git init -q $W/repo && git -C $W/repo fetch -q --depth 20 https://github.com/tigerblue77/zfs percpu-hot-counters || exit 1
git -C $W/repo rev-parse FETCH_HEAD                   # e0ba7d8c779be98fcf3acaefd91f63960155d1fe
for t in A:1974111f45abdd9e9b76d111b3b046fa172e405b B:e0ba7d8c779be98fcf3acaefd91f63960155d1fe; do
    n=${t%%:*} c=${t#*:}
    git -C $W/repo worktree add -q --detach $W/$n $c && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
for n in A B; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j$(nproc)' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    echo "$n release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' $W/$n/Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' $W/$n/Makefile)" \
        "vermagic=$(modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1) srcversion=$(modinfo -F srcversion $W/$n/module/zfs.ko)"
done 2>&1 | tee $L/builds.txt
```

Expected: A and B at their commits; `build rc=0` twice; `release=1
noinline=0` twice; `vermagic` = `uname -r` twice; two different
srcversions. On a failed build quote `grep -nE 'error:|Error [0-9]'
$L/build-<n>.log | head -20` and stop.

## STEP 2: the scripts

`env.sh` holds the settings; `use.sh` loads a build's modules and checks
them; `run.sh` runs one row; `pass.sh` one pass (both phases); `compare.py`
builds the tables.

```sh
L=/var/log/ctr-ab-1108
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the per-CPU counters A/B, sourced by every script.
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/ctr-ab-1108
L=/var/log/ctr-ab-1108
A=$W/A
B=$W/B
P=zfsnuma
PM=zfsnumam
PS=zfsnumas
EOF_ENV
cat > $L/use.sh << 'EOF_USE'
#!/bin/bash
# use.sh BUILD_DIR: load that build's spl/zfs modules (no pool may be
# imported) and check what got loaded and that spa_asize_inflation is 24.
set -u
. /var/log/ctr-ab-1108/env.sh
D=$1
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ] ||
    { echo "a pool is imported: stopping"; exit 1; }
if grep -q -E '^(zfs|spl) ' /proc/modules; then
	(cd "$D" && ./scripts/zfs.sh -u) > /dev/null 2>&1 || { echo "unload failed"; exit 1; }
fi
(cd "$D" && ./scripts/zfs.sh) > /dev/null 2>&1 || { echo "load of $D failed"; exit 1; }
for m in spl zfs; do
	want=$(modinfo -F srcversion "$D/module/$m.ko")
	have=$(cat "/sys/module/$m/srcversion")
	[ "$want" = "$have" ] || { echo "$m loaded $have is not $D's $want: stopping"; exit 1; }
done
[ "$(cat /sys/module/zfs/parameters/spa_asize_inflation)" = 24 ] ||
    { echo "spa_asize_inflation is not 24: stopping"; exit 1; }
echo "loaded $D zfs srcversion $(cat /sys/module/zfs/srcversion)"
EOF_USE
cat > $L/run.sh << 'EOF_RUN'
#!/bin/bash
# run.sh LABEL DIR RW BS NUMJOBS PLACE(A|B|all) PERF(none|stat|lock|rec|c2c)
#        [shared|perjob [FILESIZE]]
# VM-CTR-AB-1106's run.sh, plus: DIR is relative to W (mnt/plain,
# mntm/nocache, mnts/sync...), the pool's usage is checked against 60 %
# before the run and every 2 s during a write run (fio is stopped above it),
# and the dmu_tx_quota count and the highest usage seen are two more columns.
set -u
. /var/log/ctr-ab-1108/env.sh
R=$(readlink -f "$L/current") || exit 1
. "$R/env"
label=$1 dir=$2 rw=$3 bs=$4 nj=$5 place=$6 pm=$7 fmode=${8:-perjob} fsize=${9:-16m}
o=$R/raw/$label
case $dir in
mnt/*) pool=$P ;;
mntm/*) pool=$PM ;;
mnts/*) pool=$PS ;;
*) echo "unknown dir $dir"; exit 1 ;;
esac
case $place in
A) pre=(numactl --cpunodebind="$NA" --membind="$NA") ;;
B) pre=(numactl --cpunodebind="$NB" --membind="$NB") ;;
*) pre=() ;;
esac
if [ "$fmode" = shared ]; then
	files=(--filename=shared.0 --size=512m)
else
	files=(--filename_format='f.$jobnum' --size="$fsize")
fi
pct() { zpool list -Hp -o size,alloc "$pool" | awk '{ printf "%.1f", 100 * $2 / $1 }'; }
u=$(pct)
awk -v u="$u" 'BEGIN { exit !(u + 0 <= 60) }' ||
    { echo "pool $pool $u % full before $label: stopping"; exit 3; }
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6 }' /proc/stat; }
tx() { awk '/dmu_tx_delay|dmu_tx_dirty_throttle|dmu_tx_dirty_over_max/ { s += $3 }
    $1 == "dmu_tx_quota" { q = $3 } END { print s + 0, q + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
dm() { awk '$1 == "demand_data_misses" { print $3 }' /proc/spl/kstat/zfs/arcstats; }
read -r b0 i0 < <(cpu)
read -r t0 q0 < <(tx)
m0=$(dm)
"${pre[@]}" fio --name=t --directory="$W/$dir" "${files[@]}" --rw="$rw" \
    --bs="$bs" --ioengine=psync --numjobs="$nj" --time_based --runtime=30 \
    --ramp_time=5 --group_reporting --fallocate=none --refill_buffers \
    --buffer_compress_percentage=50 --output-format=json \
    --output="$o.fio.json" > "$o.fio.log" 2>&1 &
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
				echo "pool $pool $u % full during $label: fio stopped" > "$o.stopped"
				kill -INT "$fpid"
				break
			fi
		done
	) &
	wpid=$!
	;;
esac
sleep 10
case $pm in
stat)
	if [ -n "$EV" ]; then
		perf stat -a -x, -o "$o.perfstat" -e "$EV" -- sleep 15 > /dev/null 2>&1
	else
		sleep 15
	fi
	;;
lock)
	perf lock contention -a -b -E 30 -- sleep 15 > "$o.lock.txt" 2>&1 ||
	    perf lock record -a -o "$W/lock.data" -- sleep 15 > /dev/null 2>&1 ;;
rec)
	perf record -a -g -F 199 -o "$W/perf.data" -- sleep 15 > /dev/null 2>&1 ;;
c2c)
	perf c2c record -a -o "$W/c2c.data" -- sleep 10 > /dev/null 2>&1 ;;
esac
wait "$fpid"
[ -n "$wpid" ] && wait "$wpid"
read -r b1 i1 < <(cpu)
read -r t1 q1 < <(tx)
m1=$(dm)
# Reports are built once fio has finished, so they do not steal its CPU.
if [ -s "$W/lock.data" ]; then
	perf lock contention -i "$W/lock.data" -E 30 > "$o.lock.txt" 2>&1
fi
if [ -s "$W/perf.data" ]; then
	perf report -i "$W/perf.data" --no-children --sort symbol \
	    --percent-limit 0.3 --stdio > "$o.report.txt" 2> /dev/null
	perf report -i "$W/perf.data" --no-children --sort symbol \
	    -g caller,0.5,callee --percent-limit 2 --stdio \
	    > "$o.callers.txt" 2> /dev/null
fi
if [ -s "$W/c2c.data" ]; then
	perf c2c report -i "$W/c2c.data" --stdio --full-symbols \
	    > "$o.c2c.txt" 2> /dev/null
	perf c2c report -i "$W/c2c.data" --stats > "$o.c2c.stats" 2>&1
fi
rm -f "$W/perf.data" "$W/c2c.data" "$W/lock.data"
perf_cols="- -"
if [ -s "$o.perfstat" ]; then
	# remote% from node-loads if the VM has them, else from the L3-miss
	# loads served by remote vs local DRAM.
	perf_cols=$(awk -F, '{ v[$3] = $1 } END {
	    l = v["node-loads"]; m = v["node-load-misses"]
	    ld = v["mem_load_uops_l3_miss_retired.local_dram"]
	    rd = v["mem_load_uops_l3_miss_retired.remote_dram"]
	    h = v["mem_load_uops_l3_miss_retired.remote_hitm"]
	    if (l > 0) r = sprintf("%.1f", 100 * m / l)
	    else if (ld + rd > 0) r = sprintf("%.1f", 100 * rd / (ld + rd))
	    else r = "-"
	    printf "%s %s", r, (h != "" ? sprintf("%.0f", h / 15) : "-") }' "$o.perfstat")
fi
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
busy=$(awk -v b="$((b1 - b0))" -v i="$((i1 - i0))" \
    'BEGIN { printf "%.0f", 100 * b / (b + i) }')
peak=$(cat "$o.peak" 2> /dev/null || echo -)
line="$label $dir $rw $bs $nj $place $pm $fio_cols $busy $perf_cols $((t1 - t0)) $((m1 - m0)) $((q1 - q0)) $peak"
echo "$line" >> "$R/summary.txt"
echo "$line"
if [ -e "$o.stopped" ]; then
	cat "$o.stopped"
	exit 4
fi
EOF_RUN
cat > $L/pass.sh << 'EOF_PASS'
#!/bin/bash
# pass.sh LABEL BUILD_DIR: one pass of VM-CTR-AB-1106's runs on one build,
# in two pool phases so that no pool passes 50 %:
#   P: an 8 GiB RAM disk, a 6 GiB single-device pool (node NA) and a
#      2 x 1000 MiB mirror (node NB): every run but the sync writes;
#   S: a RAM disk sized from zfs_wrlog_data_max (half on NA, half on NB),
#      a pool holding only the sync dataset: scale-swrite-1..16.
# Each phase destroys its pools and RAM disk when it ends, also on a
# failure; a pool above 60 % stops the pass.
set -u
. /var/log/ctr-ab-1108/env.sh
. "$L/probe.env"
label=$1 D=$2
R=$L/$label
[ -e "$R" ] && { echo "$R exists: stopping"; exit 1; }
"$L/use.sh" "$D" || exit 1
export PATH="$D:$PATH"
for p in "$P" "$PM" "$PS"; do
	zpool list "$p" > /dev/null 2>&1 && { echo "pool $p exists: stopping"; exit 1; }
done
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
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
mkdir -p "$R/raw" "$W/mnt" "$W/mntm" "$W/mnts" && ln -sfn "$R" "$L/current" &&
    cd "$R" || exit 1
printf 'EV=%s\nNA=%s\nNB=%s\n' "$EV" "$NA" "$NB" > "$R/env"
{ echo "$label $D $(git -C "$D" rev-parse HEAD 2> /dev/null)"
  echo "srcversion $(cat /sys/module/zfs/srcversion)"; uname -r
  echo "host $(hostname) nproc $(nproc) nodes $(numactl -H | awk '/^available/ { print $2 }')" \
      "MemTotal_kB $(awk '/^MemTotal/ { print $2 }' /proc/meminfo)"
  for p in spa_asize_inflation zfs_dirty_data_max zfs_wrlog_data_max zfs_arc_max; do
	echo "$p=$(cat "$M/$p")"
  done
  echo "sync pool RAM disk ${SG} GiB"; } > build.txt
cleanup() {
	cd / || return
	for p in "$PS" "$PM" "$P"; do
		zpool list "$p" > /dev/null 2>&1 && { zpool destroy "$p" || echo "zpool destroy $p failed"; }
	done
	if grep -q '^brd ' /proc/modules; then
		if zpool list "$P" > /dev/null 2>&1 || zpool list "$PM" > /dev/null 2>&1 ||
		    zpool list "$PS" > /dev/null 2>&1; then
			echo "a pool is left: RAM disk kept"
		else
			rmmod brd || echo "rmmod brd failed"
		fi
	fi
}
trap cleanup EXIT
# prefault DEV NODE MIB [SKIP_MIB]: write the pages of DEV from node NODE.
prefault() {
	numactl --cpunodebind="$2" --membind="$2" dd if=/dev/zero of="$1" bs=1M \
	    count="$3" seek="${4:-0}" oflag=direct status=none
}
usage() {
	for p in "$@"; do
		zpool list -Hp -o name,size,alloc "$p" |
		    awk '{ printf "%s size %d alloc %d (%.1f %%)\n", $1, $2, $3, 100 * $3 / $2 }'
	done
}
L1=(numactl --cpunodebind="$NA" --membind="$NA")
# lay TAG DIR FIO_ARGS...: write the files of a dataset, from node NA.
lay() {
	local tag=$1 dir=$2
	shift 2
	"${L1[@]}" fio --name=layout --directory="$W/$dir" "$@" --bs=1m \
	    --rw=write --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "raw/layout-$tag.log" 2>&1
}
echo "label dir rw bs jobs place perf iops MiB/s p99_us cpu% remote% hitm/s throttles arc_dmiss quota peak%" > summary.txt

### Phase P
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.P0
modprobe brd rd_nr=1 rd_size=$((8 * 1048576)) max_part=16 || exit 1
printf 'label: gpt\nsize=6GiB\nsize=1000MiB\nsize=1000MiB\n' |
    sfdisk -q /dev/ram0 > /dev/null || exit 1
udevadm settle
for n in 1 2 3; do [ -b /dev/ram0p$n ] || { echo "no /dev/ram0p$n: stopping"; exit 1; }; done
# The single-device pool's memory on node NA, where the host's devices are,
# the mirror's on NB (as in VM-CTR-AB-1106).
for n in 1 2 3; do
	[ $n = 1 ] && node=$NA || node=$NB
	prefault /dev/ram0p$n "$node" $(( $(blockdev --getsize64 /dev/ram0p$n) / 1048576 )) || exit 1
done
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.P1
awk 'FNR == 1 { f++ } { u[f, $2] = $4 } END { for (n = 0; n < 2; n++) if ((1, n) in u)
    printf "phase P node%d RAM disk %+d MiB\n", n, (u[2, n] - u[1, n]) / 1024 }' \
    raw/memused.P0 raw/memused.P1 >> build.txt
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
for ds in plain wplain; do
	lay "mnt-$ds-shared" "mnt/$ds" --filename=shared.0 --size=512m || exit 1
done
zpool sync "$P" "$PM" || exit 1
usage "$P" "$PM" | sed 's/^/after layout: /' | tee -a build.txt
usage "$P" "$PM" | awk '{ gsub(/[(]/, "", $6); if ($6 + 0 > 50) bad = 1 } END { exit bad }' ||
    { echo "a pool is above 50 % after its layout: stopping"; exit 1; }
awk '$1 == "c_max" || $1 == "c" || $1 == "size" { printf "arc %s %.2f GiB\n", $1, $3 / 2^30 }' \
    /proc/spl/kstat/zfs/arcstats >> build.txt
declare -A WL=([cread]="mnt/plain randread 4k" [uread]="mnt/nocache randread 16k"
    [ureadlz4]="mnt/nocachelz4 randread 16k" [awrite]="mnt/wplain randwrite 16k")
r() { "$L/run.sh" "$@" < /dev/null || exit $?; }
# Block 2 (reduced): the cross-socket penalty, 8 jobs on node A vs unbound.
for n in cread uread ureadlz4 awrite; do
	read -r d rw bs <<< "${WL[$n]}"
	for place in A all; do
		r "numa-$n-$place" "$d" "$rw" "$bs" 8 "$place" stat
	done
done
# Block 3: scaling 1..16 jobs (the sync writes are in phase S), then shared
# files at 16 jobs.
for n in cread uread awrite; do
	read -r d rw bs <<< "${WL[$n]}"
	for nj in 1 2 4 8 16; do
		r "scale-$n-$nj" "$d" "$rw" "$bs" "$nj" all stat
	done
done
r shared-cread-16 mnt/plain randread 4k 16 all stat shared
r shared-awrite-16 mnt/wplain randwrite 16k 16 all stat shared
# Block 4: profiles at 16 jobs (perf record always; c2c and lock if probed).
for n in cread uread awrite; do
	read -r d rw bs <<< "${WL[$n]}"
	r "prof-$n-16" "$d" "$rw" "$bs" 16 all rec
	[ "$C2C" = yes ] && r "c2c-$n-16" "$d" "$rw" "$bs" 16 all c2c
	[ "$LOCK" != no ] && r "lock-$n-16" "$d" "$rw" "$bs" 16 all lock
done
# Block 6: the mirror pool, where commit 4 applies.
for nj in 1 16; do
	r "mir-uread-$nj" mntm/nocache randread 16k "$nj" all stat perjob 8m
	r "mir-awrite-$nj" mntm/wplain randwrite 16k "$nj" all stat perjob 8m
done
r prof-mir-uread-16 mntm/nocache randread 16k 16 all rec perjob 8m
r prof-mir-awrite-16 mntm/wplain randwrite 16k 16 all rec perjob 8m
usage "$P" "$PM" | sed 's/^/end of phase P: /' | tee -a build.txt
zpool destroy "$PM" && zpool destroy "$P" || exit 1
rmmod brd || exit 1

### Phase S
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.S0
modprobe brd rd_nr=1 rd_size=$((SG * 1048576)) max_part=16 || exit 1
half=$((SG * 512))
prefault /dev/ram0 "$NA" "$half" && prefault /dev/ram0 "$NB" "$half" "$half" || exit 1
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.S1
awk 'FNR == 1 { f++ } { u[f, $2] = $4 } END { for (n = 0; n < 2; n++) if ((1, n) in u)
    printf "phase S node%d RAM disk %+d MiB\n", n, (u[2, n] - u[1, n]) / 1024 }' \
    raw/memused.S0 raw/memused.S1 >> build.txt
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$W/mnts" "$PS" /dev/ram0 || exit 1
zfs create "$PS/sync" || exit 1
lay mnts-sync mnts/sync --filename_format='f.$jobnum' --size=16m --numjobs=16 || exit 1
zfs set sync=always "$PS/sync" && zpool sync "$PS" || exit 1
usage "$PS" | sed 's/^/after layout: /' | tee -a build.txt
for nj in 1 2 4 8 16; do
	r "scale-swrite-$nj" mnts/sync randwrite 16k "$nj" all stat
done
usage "$PS" | sed 's/^/end of phase S: /' | tee -a build.txt
echo "pass $label done: $(($(wc -l < summary.txt) - 1)) runs, $(du -sh "$R" | cut -f1)"
EOF_PASS
cat > $L/compare.py << 'EOF_CMP'
#!/usr/bin/env python3
# compare.py: A1/B/A2 tables from the three passes' summary.txt and reports
# (VM-CTR-AB-1106's, plus the dmu_tx_quota count and the peak pool usage).
import os, re, sys
AB = "/var/log/ctr-ab-1108"
P = ["A1", "B", "A2"]
rows = {}
order = []
for p in P:
    for ln in open(os.path.join(AB, p, "summary.txt")).read().splitlines()[1:]:
        f = ln.split()
        if f[0] not in rows:
            order.append(f[0])
        rows.setdefault(f[0], {})[p] = f
def num(x):
    try:
        return float(x)
    except ValueError:
        return float("nan")
def col(f, i):
    return f[i] if len(f) > i else "-"
out = ["%-22s %9s %9s %9s %7s %6s %7s %7s %7s %5s %5s %6s %6s %6s %5s" % ("label",
    "iopsA1", "iopsB", "iopsA2", "B/A%", "noise", "hitmA1", "hitmB", "hitmA2",
    "cpuA1", "cpuB", "quoA1", "quoB", "quoA2", "peak%")]
for lab in order:
    r = rows[lab]
    if not all(p in r for p in P):
        continue
    i = [num(r[p][7]) for p in P]
    h = [r[p][12] for p in P]
    a = (i[0] + i[2]) / 2
    d = 100 * (i[1] - a) / a if a else float("nan")
    n = 100 * abs(i[0] - i[2]) / a if a else float("nan")
    pk = [num(col(r[p], 16)) for p in P]
    pk = [x for x in pk if x == x]
    out.append("%-22s %9.0f %9.0f %9.0f %+7.1f %6.1f %7s %7s %7s %5s %5s %6s %6s %6s %5s" % (
        lab, i[0], i[1], i[2], d, n, h[0], h[1], h[2], r["A1"][10], r["B"][10],
        col(r["A1"], 15), col(r["B"], 15), col(r["A2"], 15),
        "%.0f" % max(pk) if pk else "-"))
open(os.path.join(AB, "compare.txt"), "w").write("\n".join(out) + "\n")
syms = ["add_reference", "remove_reference", "arc_change_state",
    "arc_evictable_space_increment", "arc_evictable_space_decrement",
    "arc_get_data_impl", "arc_free_data_impl", "arc_release",
    "arc_tempreserve_space", "dbuf_hold_impl", "dbuf_rele_and_unlock",
    "dbuf_evict_notify", "aggsum_add", "aggsum_compare", "aggsum_value",
    "percpu_counter_add_batch", "vdev_stat_update",
    "native_queued_spin_lock_slowpath", "osq_lock", "mutex_spin_on_owner"]
sout = ["%-18s %-33s %6s %6s %6s" % ("profile", "symbol", "A1", "B", "A2")]
for p0 in sorted(f for f in os.listdir(os.path.join(AB, "B", "raw"))
    if f.startswith("prof-") and f.endswith(".report.txt")):
    pct = {}
    for p in P:
        try:
            txt = open(os.path.join(AB, p, "raw", p0)).read()
        except OSError:
            txt = ""
        for s in syms:
            m = re.search(r"^\s*([0-9.]+)%.*\]\s+" + re.escape(s) + r"(\s|$)", txt, re.M)
            pct.setdefault(s, {})[p] = m.group(1) if m else "-"
    for s in syms:
        if any(pct[s][p] != "-" for p in P):
            sout.append("%-18s %-33s %6s %6s %6s" % (p0[5:-11], s, pct[s]["A1"],
                pct[s]["B"], pct[s]["A2"]))
open(os.path.join(AB, "symbols.txt"), "w").write("\n".join(sout) + "\n")
short = [l for l in out if re.match(r"(label|numa-|scale-\w+-(1|16) |shared-|mir-)", l)]
# Per-job efficiency at 16 jobs: IOPS(16) / (16 * IOPS(1)) of the same pass.
for w in ("cread", "uread", "awrite", "swrite"):
    e = []
    for p in P:
        try:
            e.append("%3.0f%%" % (100 * num(rows["scale-%s-16" % w][p][7]) /
                (16 * num(rows["scale-%s-1" % w][p][7]))))
        except (KeyError, ZeroDivisionError):
            e.append("  -")
    short.append("eff16 %-16s A1 %s  B %s  A2 %s" % (w, e[0], e[1], e[2]))
print("\n".join(short))
EOF_CMP
chmod 0755 $L/*.sh $L/compare.py
cd $L && sha256sum env.sh use.sh run.sh pass.sh compare.py
```

Must print:
```
2ff41881f7057b185cbf3a635bc303eb9d92dd124f5fc1447c1a4805b0e95159  env.sh
3457a22da77d95920df132a68ec8d9bdb06f51205095d9c24ec03373f6e76dc1  use.sh
777a9b4db942f8ca6d4c160cc65d5dfe67282fb119a00c8c2bb8252adbfca8e3  run.sh
e6bb92832a4c2c6c4de4bec44ebb0d97a9934f1951bb2440a0495efe88a00923  pass.sh
d36c16139d6e92bca2fb8cdd12a9a79fe30b86897285d4244af5c0f2990a0d6c  compare.py
```

## STEP 3: the nodes and the perf features of this VM

```sh
L=/var/log/ctr-ab-1108
nodes=$(numactl -H | awk '/^node [0-9]+ cpus: [0-9]/ { print $2 }' | tr '\n' ' ')
NA=$(echo $nodes | cut -d' ' -f1); NB=$(echo $nodes | cut -d' ' -f2); NB=${NB:-$NA}
EV=""
for e in node-loads node-load-misses \
    mem_load_uops_l3_miss_retired.local_dram \
    mem_load_uops_l3_miss_retired.remote_dram \
    mem_load_uops_l3_miss_retired.remote_hitm \
    mem_load_uops_l3_miss_retired.remote_fwd \
    context-switches cpu-migrations; do
	sudo -n perf stat -a -x, -e "$e" -- sleep 0.2 2>&1 > /dev/null |
	    grep -q -E '^[0-9]+,' && EV=${EV:+$EV,}$e
done
C2C=no
if sudo -n perf c2c record -a -o /var/tmp/ctr-ab-1108/probe.c2c -- sleep 0.5 > /dev/null 2>&1 &&
    sudo -n perf c2c report -i /var/tmp/ctr-ab-1108/probe.c2c --stats 2> /dev/null |
    grep -q -E 'Load Operations *: *[1-9]'; then
	C2C=yes
fi
LOCK=no
if sudo -n perf lock contention -a -b -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=bpf
elif sudo -n perf lock record -a -o /var/tmp/ctr-ab-1108/probe.lock -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=rec
fi
sudo -n rm -f /var/tmp/ctr-ab-1108/probe.c2c /var/tmp/ctr-ab-1108/probe.lock
printf 'EV=%s\nC2C=%s\nLOCK=%s\nNA=%s\nNB=%s\n' "$EV" "$C2C" "$LOCK" "$NA" "$NB" | tee $L/probe.env
```

Expected on VM 107: `NA=0`, `NB=1`, `C2C=no` (no PEBS in this guest); on
VM 105 or 106: `NA=0`, `NB=0`. `EV` may be shorter on a VM whose vCPU model
hides the uncore events; continue and say so.

## STEP 4: the three passes, A1, B, A2 (about 1 h 45 min)

Each pass loads its build (checking the srcversion and the inflation),
records in `build.txt` the VM (host, vCPUs, nodes, memory) and the sizes, and
runs phase P then phase S; the chain stops at the first failed pass. One
command, the VM otherwise idle:

```sh
sudo -n bash -c '. /var/log/ctr-ab-1108/env.sh
"$L/pass.sh" A1 "$A" && "$L/pass.sh" B "$B" && "$L/pass.sh" A2 "$A"
echo "exit $?"' 2>&1 | tee /var/log/ctr-ab-1108/all.log
```

It ends with `exit 0`. Each pass prints its `after layout` lines (main pool
near 33 %, mirror near 26 %, sync pool under 4 %), one line per row (`quota`
and `peak%` last), its `end of phase` usage and `pass ... done: 42 runs` (39
with neither c2c nor lock, 45 with both).

## STEP 5: the tables

```sh
L=/var/log/ctr-ab-1108
tail -1 $L/all.log | grep -qx 'exit 0' || { echo "passes not finished or failed:"; tail -5 $L/all.log; }
for p in A1 B A2; do echo "== $p"; cat $L/$p/build.txt; done | tee $L/builds-passes.txt
python3 -I $L/compare.py | tee $L/compare-short.txt
awk 'NR == 1 || $3 + 0 >= 0.3 || $4 + 0 >= 0.3 || $5 + 0 >= 0.3' $L/symbols.txt | head -40 | tee $L/symbols-top.txt
awk 'NR > 1 && $17 != "-" { if ($17 + 0 > m) m = $17 + 0 } END { print "highest pool usage seen: " m + 0 " %" }' $L/A1/summary.txt $L/B/summary.txt $L/A2/summary.txt
sudo -n dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|suspended|ENOSPC'   # 0
```

## STEP 6: cleanup

The passes already destroyed their pools and RAM disks and deleted the perf
data files once reported.

```sh
W=/var/tmp/ctr-ab-1108 L=/var/log/ctr-ab-1108
sudo -n bash -c '. /var/log/ctr-ab-1108/env.sh
for p in "$P" "$PM" "$PS"; do "$A/zpool" list -H -o name "$p" 2> /dev/null && "$A/zpool" destroy "$p"; done
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null
grep -q "^brd " /proc/modules && { rmmod brd || echo "rmmod brd failed"; }
grep -q -E "^(zfs|spl) " /proc/modules && { (cd "$A" && ./scripts/zfs.sh -u) || echo "unload failed"; }
grep -E "^(zfs|spl|brd) " /proc/modules'
sudo -n rm -rf $W && echo "work dir removed; logs kept in $L"
du -sh $L
sudo -n fstrim -av
```

Expected: no pool, nothing loaded, `work dir removed`, the size of L, the
fstrim summary. (If a pass stopped with a pool imported by B's build, use
`$B/zpool` in the first line instead.)

## STEP 7: report

```sh
L=/var/log/ctr-ab-1108
{
echo "== builds"; cat $L/builds.txt; cat $L/probe.env
echo "== passes"; cat $L/builds-passes.txt
echo "== short table"; cat $L/compare-short.txt
echo "== symbols"; cat $L/symbols-top.txt
for f in lock-cread-16 lock-uread-16 lock-awrite-16 c2c-awrite-16; do
	for p in A1 B A2; do
		[ -f $L/$p/raw/$f.lock.txt ] && { echo "-- $p $f"; head -14 $L/$p/raw/$f.lock.txt; }
		[ -f $L/$p/raw/$f.c2c.txt ] && { echo "-- $p $f"; grep -A 14 'Shared Data Cache Line Table' $L/$p/raw/$f.c2c.txt | head -16; }
	done
done
echo "== full table"; cat $L/compare.txt
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed, and at
its end the last lines of STEP 5 (highest usage, kernel message count), the
output of STEP 6 and every deviation from this document (what and why).
Reply with the whole of `REPORT.txt`.

## How to read the results

- `B/A%` is B's IOPS against the mean of A1 and A2; `noise` is |A1 - A2|
  over that mean. Treat a `B/A%` within the row's `noise` (and under about
  3 %) as no change.
- `hitm/s` counts loads that hit a line modified in the other socket's
  cache (VM 107 only; on a one-node VM it reads near 0 or `-`). The counters
  B changes are written by every reader or writer, so the expected effect is
  lower `hitm/s` at 16 jobs unbound, and a smaller IOPS gap between
  `numa-*-A` and `numa-*-all`. On VM 107, 16 jobs use half of its 32 vCPUs.
- Where to expect it: `cread` (dbuf cache size, commit 3: two updates per
  read), `uread`/`ureadlz4` (arc_uncached and arc_anon sizes, commits 1 and
  2), `awrite`/`swrite` (arc_anon and arc_mru sizes, commits 1 and 2),
  `mir-*` (also commit 4). Nothing for B at 1 job beyond noise: a regression
  there would be the cost of `aggsum_add()` (a bucket mutex) over an
  uncontended atomic.
- `quota` should be 0 on every read row and may be in the thousands on the
  quota-capped write rows (see "How the pools are sized"); compare those rows
  only between passes, where the cap is the same. `peak%` should stay under
  50 %.
- `symbols.txt`: in A, the atomics are inlined, so their cost shows as self
  time of `add_reference`, `arc_change_state`, `dbuf_rele_and_unlock`,
  `dbuf_hold_impl`, `arc_get_data_impl`, `arc_free_data_impl`. In B these
  should drop, and `aggsum_add` / `percpu_counter_add_batch` appear with a
  small share. A large `native_queued_spin_lock_slowpath` or
  `mutex_spin_on_owner` in B would point at aggsum bucket or core locks.
- With `perf c2c`, the "Shared Data Cache Line Table" rows with high "Rmt
  HITM" name the bouncing lines; with `perf lock`, the first callers name the
  contended locks (`vdev_stat_update` = `vdev_stat_lock`, `aggsum_add` =
  aggsum buckets).

sha256 of this file without this last line and the blank line before it: 658b667e8b331f5e34870bf0a80937c2aeed0334699d39f7f4d2be8d6111dcfe
