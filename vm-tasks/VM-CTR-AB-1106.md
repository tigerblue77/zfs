# VM 107 task: A/B of per-CPU hot counters (6/11)

Goal: measure whether turning the per-I/O global counters of the ARC and
the dbuf cache into per-CPU sums (and skipping one useless per-I/O lock)
reduces cross-socket cache-line bouncing and improves scaling, in this
VM whose 2 virtual NUMA nodes are pinned to the T630's two sockets
(node 0 = vCPU 0-7, node 1 = vCPU 8-15, 8 GiB each; kernel 6.12).

- A = master 1974111f45, the RELEASE build you made for VM-NUMA-1105,
  at /var/tmp/zts-numa-1105/M.
- B = fork branch `percpu-hot-counters`, head e0ba7d8c779be98fcf3acaefd91f63960155d1fe
  (https://github.com/tigerblue77/zfs), 4 commits on upstream master
  ef35f144e1:
  1. `arcs_esize[]` of every ARC state: atomic -> wmsum.
  2. `arcs_size[]`: atomic -> wmsum, except arc_anon -> aggsum read
     through its lower bound in `arc_tempreserve_space()`.
  3. `dbuf_caches[].size`: atomic -> aggsum (lower bound on the release
     path, `aggsum_compare()` in the eviction thread).
  4. `vdev_stat_update()` no longer takes `vdev_stat_lock` of a mirror or
     RAID-Z vdev on success (only leaf vdevs record stats). Only the
     mirror block below can show it; the main pool is one device.
- B's base is 4 upstream commits after A (f7c15333ac dmu_recv,
  9699602a00 ZIL suspended commits, 924b8c89c0 Direct I/O reads of
  pending frees, ef35f144e1 encrypted holds). From reading, none of them
  is on a path these runs use (no Direct I/O, no suspension, no recv, no
  encryption; the dbuf.c part only moves the hole check into a function).

Rules: this VM is disposable; nothing is posted or pushed; results under
/root/zfs-numa-bench/ab-1106; RELEASE builds only (no --enable-debug, no
--enable-debuginfo); A and B are run interleaved A1, B, A2 on the same
layout, so A1 vs A2 gives the noise; `sudo fstrim -av` at the end.

## Step 0: build B, check A and B, probe perf (about 15 min)

If this block stops after creating `$AB` or `$B`, remove what it created
before running it again.

```sh
(
set -u
A=/var/tmp/zts-numa-1105/M
B=/var/tmp/zts-numa-1106/B
WANT=e0ba7d8c779be98fcf3acaefd91f63960155d1fe
AB=/root/zfs-numa-bench/ab-1106
for t in fio numactl numastat perf python3 sfdisk udevadm blockdev git; do
	command -v "$t" > /dev/null || { echo "missing $t: stopping"; exit 1; }
done
[ -e "$AB" ] && { echo "$AB exists: stopping"; exit 1; }
[ -e "$B" ] && { echo "$B exists: stopping"; exit 1; }
mkdir -p "$AB" /var/tmp/zts-numa-1106 || exit 1
release() {
	grep -q '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' "$1/Makefile" &&
	    ! grep -q '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' "$1/Makefile"
}
[ -f "$A/module/zfs.ko" ] && release "$A" ||
    { echo "A is not a release build with modules: stopping"; exit 1; }
a_head=$(git -C "$A" rev-parse HEAD 2> /dev/null)
case "$a_head" in
1974111f45*) ;;
"") echo "warning: $A is not a git tree, cannot check its commit" ;;
*) echo "A is at $a_head, not 1974111f45: stopping"; exit 1 ;;
esac
git clone -q --depth 50 --branch percpu-hot-counters \
    https://github.com/tigerblue77/zfs.git "$B" || exit 1
[ "$(git -C "$B" rev-parse HEAD)" = "$WANT" ] ||
    { echo "B is at $(git -C "$B" rev-parse HEAD), not $WANT: stopping"; exit 1; }
# Configure B exactly as A was configured.
opts=$(sed -n 's/^  \$ \.\/configure *//p' "$A/config.log" | head -1)
case "$opts" in *enable-debug*) echo "A was configured with debug: stopping"; exit 1 ;; esac
echo "configure options: ${opts:-none}"
cd "$B" && ./autogen.sh > "$AB/B-autogen.log" 2>&1 &&
    eval ./configure $opts > "$AB/B-configure.log" 2>&1 &&
    make -j"$(nproc)" > "$AB/B-make.log" 2>&1 ||
    { tail -20 "$AB/B-make.log"; echo "B build failed: stopping"; exit 1; }
release "$B" || { echo "B is not a release build: stopping"; exit 1; }
k=$(uname -r)
for d in "$A" "$B"; do
	v=$(modinfo -F vermagic "$d/module/zfs.ko" | cut -d' ' -f1)
	[ "$v" = "$k" ] || { echo "$d/module/zfs.ko is for $v, not $k: stopping"; exit 1; }
done
sa=$(modinfo -F srcversion "$A/module/zfs.ko")
sb=$(modinfo -F srcversion "$B/module/zfs.ko")
[ "$sa" != "$sb" ] || { echo "A and B have the same srcversion: stopping"; exit 1; }
printf 'A=%s\nB=%s\nSRC_A=%s\nSRC_B=%s\n' "$A" "$B" "$sa" "$sb" > "$AB/builds.env"
# Which perf features this VM offers: counters, c2c (needs PEBS), lock.
EV=""
for e in node-loads node-load-misses \
    mem_load_uops_l3_miss_retired.local_dram \
    mem_load_uops_l3_miss_retired.remote_dram \
    mem_load_uops_l3_miss_retired.remote_hitm \
    mem_load_uops_l3_miss_retired.remote_fwd \
    context-switches cpu-migrations; do
	perf stat -a -x, -e "$e" -- sleep 0.2 2>&1 > /dev/null |
	    grep -q -E '^[0-9]+,' && EV=${EV:+$EV,}$e
done
C2C=no
if perf c2c record -a -o "$AB/probe.c2c" -- sleep 0.5 > /dev/null 2>&1 &&
    perf c2c report -i "$AB/probe.c2c" --stats 2> /dev/null |
    grep -q -E 'Load Operations *: *[1-9]'; then
	C2C=yes
fi
LOCK=no
if perf lock contention -a -b -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=bpf
elif perf lock record -a -o "$AB/probe.lock" -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=rec
fi
rm -f "$AB/probe.c2c" "$AB/probe.lock"
printf 'EV=%s\nC2C=%s\nLOCK=%s\n' "$EV" "$C2C" "$LOCK" > "$AB/probe.env"
echo "kernel $k; A $sa; B $sb ($WANT)"
cat "$AB/probe.env"
)
```

## Step 1: write the helper scripts (instant)

`use.sh` swaps the loaded modules and checks their srcversion; `pass.sh`
runs one whole pass on one build; `compare.py` builds the tables.

```sh
(
AB=/root/zfs-numa-bench/ab-1106
[ -f "$AB/builds.env" ] && [ -f "$AB/probe.env" ] || { echo "run step 0 first"; exit 1; }
cat > "$AB/use.sh" << 'EOF'
#!/bin/bash
# use.sh BUILD_DIR: load that build's spl/zfs modules (no pool may be imported)
set -u
D=$1
if grep -q '^zfs ' /proc/modules; then
	[ -z "$("$D/zpool" list -H -o name 2> /dev/null)" ] ||
	    { echo "a pool is imported: stopping"; exit 1; }
	"$D/scripts/zfs.sh" -u > /dev/null 2>&1 || { echo "unload failed"; exit 1; }
fi
"$D/scripts/zfs.sh" > /dev/null 2>&1 || { echo "load of $D failed"; exit 1; }
want=$(modinfo -F srcversion "$D/module/zfs.ko")
have=$(cat /sys/module/zfs/srcversion)
[ "$want" = "$have" ] || { echo "loaded zfs $have is not $D's $want: stopping"; exit 1; }
echo "loaded $D zfs srcversion $have"
EOF
cat > "$AB/pass.sh" << 'EOF'
#!/bin/bash
# pass.sh LABEL BUILD_DIR: pool, placement, scaling, profiles, mirror, cleanup
set -u
label=$1 D=$2
AB=/root/zfs-numa-bench/ab-1106
. "$AB/probe.env"
R=$AB/$label
P=zfsnuma PM=zfsnumam GB=5
[ -e "$R" ] && { echo "$R exists: stopping"; exit 1; }
"$AB/use.sh" "$D" || exit 1
export PATH="$D:$PATH"
for p in "$P" "$PM"; do
	zpool list "$p" > /dev/null 2>&1 && { echo "pool $p exists: stopping"; exit 1; }
done
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
avail=$(awk '/^MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$avail" -ge 10 ] || { echo "MemAvailable ${avail} GiB < 10: stopping"; exit 1; }
# On any exit, destroy this pass's pools and ramdisk; reports are kept.
cleanup() {
	cd / || return
	for p in "$PM" "$P"; do
		zpool list "$p" > /dev/null 2>&1 && { zpool destroy "$p" || echo "zpool destroy $p failed"; }
	done
	if grep -q '^brd ' /proc/modules; then
		if zpool list "$P" > /dev/null 2>&1 || zpool list "$PM" > /dev/null 2>&1; then
			echo "a pool is left: ramdisk kept"
		else
			rmmod brd || echo "rmmod brd failed"
		fi
	fi
	rmdir "$R/mnt" "$R/mntm" 2> /dev/null
}
trap cleanup EXIT
mkdir -p "$R/raw" "$R/mnt" "$R/mntm" && ln -sfn "$R" /root/zfs-numa-bench/current &&
    cd "$R" || exit 1
NA=$(lscpu -p=SOCKET,NODE | awk -F, '!/^#/ && $1 == 0 { print $2; exit }')
NB=$(lscpu -p=SOCKET,NODE | awk -F, '!/^#/ && $1 == 1 { print $2; exit }')
[ -n "$NA" ] && [ -n "$NB" ] || { echo "two sockets not found: stopping"; exit 1; }
printf 'P=%s\nNA=%s\nNB=%s\nEV=%s\n' "$P" "$NA" "$NB" "$EV" > "$R/env"
{ echo "$label $D $(git -C "$D" rev-parse HEAD 2> /dev/null)"
  echo "srcversion $(cat /sys/module/zfs/srcversion)"; uname -r; } > build.txt
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.0
modprobe brd rd_nr=1 rd_size=$((GB * 1048576)) max_part=16 || exit 1
printf 'label: gpt\nsize=4GiB\nsize=500MiB\nsize=500MiB\n' |
    sfdisk -q /dev/ram0 > /dev/null || exit 1
udevadm settle
for n in 1 2 3; do [ -b /dev/ram0p$n ] || { echo "no /dev/ram0p$n: stopping"; exit 1; }; done
# Pre-fault the ramdisk pages: the single-device pool's 4 GiB on node NA,
# where the host's devices are (as in VM-NUMA-1105), the mirror's on NB.
for n in 1 2 3; do
	[ $n = 1 ] && node=$NA || node=$NB
	mib=$(( $(blockdev --getsize64 /dev/ram0p$n) / 1048576 ))
	numactl --cpunodebind="$node" --membind="$node" dd if=/dev/zero \
	    of=/dev/ram0p$n bs=1M count="$mib" oflag=direct status=none || exit 1
done
grep -h MemUsed /sys/devices/system/node/node*/meminfo > raw/memused.1
awk 'FNR == 1 { f++ } { u[f, $2] = $4 } END { for (n = 0; n < 2; n++)
    printf "node%d ramdisk %+d MiB\n", n, (u[2, n] - u[1, n]) / 1024 }' \
    raw/memused.0 raw/memused.1 >> build.txt
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$R/mnt" "$P" /dev/ram0p1 || exit 1
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$R/mntm" "$PM" mirror /dev/ram0p2 /dev/ram0p3 ||
    exit 1
zfs create "$P/plain" && zfs create "$P/wplain" && zfs create "$P/sync" &&
    zfs create -o primarycache=metadata "$P/nocache" &&
    zfs create -o primarycache=metadata -o compression=lz4 "$P/nocachelz4" &&
    zfs create -o primarycache=metadata "$PM/nocache" && zfs create "$PM/wplain" ||
    exit 1
L=(numactl --cpunodebind="$NA" --membind="$NA")
for ds in mnt/plain mnt/wplain mnt/sync mnt/nocache mnt/nocachelz4 \
    mntm/nocache mntm/wplain; do
	case $ds in mntm/*) fs=8m ;; *) fs=16m ;; esac
	"${L[@]}" fio --name=layout --directory="$ds" --filename_format='f.$jobnum' \
	    --size=$fs --bs=1m --rw=write --numjobs=16 --ioengine=psync \
	    --refill_buffers --buffer_compress_percentage=50 --fallocate=none \
	    --end_fsync=1 > "raw/layout-${ds//\//-}.log" 2>&1 || exit 1
done
for ds in plain wplain; do
	"${L[@]}" fio --name=layout --directory="mnt/$ds" --filename=shared.0 \
	    --size=512m --bs=1m --rw=write --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "raw/layout-$ds-shared.log" 2>&1 || exit 1
done
zfs set sync=always "$P/sync" && zpool sync "$P" "$PM" || exit 1
awk '$1 == "c_max" || $1 == "c" || $1 == "size" { printf "arc %s %.2f GiB\n", $1, $3 / 2^30 }' \
    /proc/spl/kstat/zfs/arcstats >> build.txt
cat > run.sh << 'EOR'
#!/bin/bash
# run.sh LABEL DIR RW BS NUMJOBS PLACE(A|B|all) PERF(none|stat|lock|rec|c2c)
#        [shared|perjob [FILESIZE]]
# DIR is relative to the pass directory (mnt/plain, mntm/nocache, ...).
set -u
R=$(readlink -f /root/zfs-numa-bench/current) || exit 1
. "$R/env"
label=$1 dir=$2 rw=$3 bs=$4 nj=$5 place=$6 pm=$7 fmode=${8:-perjob} fsize=${9:-16m}
o=$R/raw/$label
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
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6 }' /proc/stat; }
tx() { awk '/dmu_tx_delay|dmu_tx_dirty_throttle|dmu_tx_dirty_over_max/ { s += $3 }
    END { print s + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
dm() { awk '$1 == "demand_data_misses" { print $3 }' /proc/spl/kstat/zfs/arcstats; }
read -r b0 i0 < <(cpu)
t0=$(tx) m0=$(dm)
"${pre[@]}" fio --name=t --directory="$R/$dir" "${files[@]}" --rw="$rw" \
    --bs="$bs" --ioengine=psync --numjobs="$nj" --time_based --runtime=30 \
    --ramp_time=5 --group_reporting --fallocate=none --refill_buffers \
    --buffer_compress_percentage=50 --output-format=json \
    --output="$o.fio.json" > "$o.fio.log" 2>&1 &
fpid=$!
sleep 10
case $pm in
stat)
	perf stat -a -x, -o "$o.perfstat" -e "$EV" -- sleep 15 > /dev/null 2>&1 ;;
lock)
	perf lock contention -a -b -E 30 -- sleep 15 > "$o.lock.txt" 2>&1 ||
	    perf lock record -a -o "$o.lock.data" -- sleep 15 > /dev/null 2>&1 ;;
rec)
	perf record -a -g -F 199 -o "$o.perf.data" -- sleep 15 > /dev/null 2>&1 ;;
c2c)
	perf c2c record -a -o "$o.c2c.data" -- sleep 10 > /dev/null 2>&1 ;;
esac
wait "$fpid"
read -r b1 i1 < <(cpu)
t1=$(tx) m1=$(dm)
# Reports are built once fio has finished, so they do not steal its CPU.
if [ -s "$o.lock.data" ]; then
	perf lock contention -i "$o.lock.data" -E 30 > "$o.lock.txt" 2>&1
fi
if [ -s "$o.perf.data" ]; then
	perf report -i "$o.perf.data" --no-children --sort symbol \
	    --percent-limit 0.3 --stdio > "$o.report.txt" 2> /dev/null
	perf report -i "$o.perf.data" --no-children --sort symbol \
	    -g caller,0.5,callee --percent-limit 2 --stdio \
	    > "$o.callers.txt" 2> /dev/null
fi
if [ -s "$o.c2c.data" ]; then
	perf c2c report -i "$o.c2c.data" --stdio --full-symbols \
	    > "$o.c2c.txt" 2> /dev/null
	perf c2c report -i "$o.c2c.data" --stats > "$o.c2c.stats" 2>&1
fi
rm -f "$o.perf.data" "$o.c2c.data" "$o.lock.data"
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
line="$label $dir $rw $bs $nj $place $pm $fio_cols $busy $perf_cols $((t1 - t0)) $((m1 - m0))"
echo "$line" >> "$R/summary.txt"
echo "$line"
EOR
chmod 0755 run.sh
echo "label dir rw bs jobs place perf iops MiB/s p99_us cpu% remote% hitm/s throttles arc_dmiss" > summary.txt
declare -A WL=([cread]="mnt/plain randread 4k" [uread]="mnt/nocache randread 16k"
    [ureadlz4]="mnt/nocachelz4 randread 16k" [awrite]="mnt/wplain randwrite 16k"
    [swrite]="mnt/sync randwrite 16k")
# Block 2 (reduced): the cross-socket penalty, 8 jobs on node A vs unbound.
for n in cread uread ureadlz4 awrite; do
	read -r d rw bs <<< "${WL[$n]}"
	for place in A all; do
		./run.sh "numa-$n-$place" "$d" "$rw" "$bs" 8 "$place" stat < /dev/null
	done
done
# Block 3: scaling 1..16 jobs, then shared files at 16 jobs.
for n in cread uread awrite swrite; do
	read -r d rw bs <<< "${WL[$n]}"
	for nj in 1 2 4 8 16; do
		./run.sh "scale-$n-$nj" "$d" "$rw" "$bs" "$nj" all stat < /dev/null
	done
done
./run.sh shared-cread-16 mnt/plain randread 4k 16 all stat shared < /dev/null
./run.sh shared-awrite-16 mnt/wplain randwrite 16k 16 all stat shared < /dev/null
# Block 4: profiles at 16 jobs (perf record always; c2c and lock if probed).
for n in cread uread awrite; do
	read -r d rw bs <<< "${WL[$n]}"
	./run.sh "prof-$n-16" "$d" "$rw" "$bs" 16 all rec < /dev/null
	[ "$C2C" = yes ] && ./run.sh "c2c-$n-16" "$d" "$rw" "$bs" 16 all c2c < /dev/null
	[ "$LOCK" != no ] && ./run.sh "lock-$n-16" "$d" "$rw" "$bs" 16 all lock < /dev/null
done
# Block 6: the mirror pool, where commit 4 applies.
for nj in 1 16; do
	./run.sh "mir-uread-$nj" mntm/nocache randread 16k "$nj" all stat perjob 8m < /dev/null
	./run.sh "mir-awrite-$nj" mntm/wplain randwrite 16k "$nj" all stat perjob 8m < /dev/null
done
./run.sh prof-mir-uread-16 mntm/nocache randread 16k 16 all rec perjob 8m < /dev/null
./run.sh prof-mir-awrite-16 mntm/wplain randwrite 16k 16 all rec perjob 8m < /dev/null
echo "pass $label done: $(($(wc -l < summary.txt) - 1)) runs, $(du -sh "$R" | cut -f1)"
EOF
cat > "$AB/compare.py" << 'EOF'
#!/usr/bin/env python3
# compare.py: A1/B/A2 tables from the three passes' summary.txt and reports.
import os, re, sys
AB = "/root/zfs-numa-bench/ab-1106"
P = ["A1", "B", "A2"]
rows = {}
for p in P:
    for ln in open(os.path.join(AB, p, "summary.txt")).read().splitlines()[1:]:
        f = ln.split()
        rows.setdefault(f[0], {})[p] = f
def num(x):
    try:
        return float(x)
    except ValueError:
        return float("nan")
out = ["%-22s %9s %9s %9s %7s %6s %7s %7s %7s %5s %5s" % ("label", "iopsA1",
    "iopsB", "iopsA2", "B/A%", "noise", "hitmA1", "hitmB", "hitmA2", "cpuA1", "cpuB")]
for lab in rows:
    r = rows[lab]
    if not all(p in r for p in P):
        continue
    i = [num(r[p][7]) for p in P]
    h = [r[p][12] for p in P]
    a = (i[0] + i[2]) / 2
    d = 100 * (i[1] - a) / a if a else float("nan")
    n = 100 * abs(i[0] - i[2]) / a if a else float("nan")
    out.append("%-22s %9.0f %9.0f %9.0f %+7.1f %6.1f %7s %7s %7s %5s %5s" % (lab,
        i[0], i[1], i[2], d, n, h[0], h[1], h[2], r["A1"][10], r["B"][10]))
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
EOF
chmod 0755 "$AB/use.sh" "$AB/pass.sh" "$AB/compare.py"
ls -l "$AB"
)
```

## Step 2: the three passes, A1, B, A2 (about 1 h 50 min in all)

Each pass loads its build (checking the srcversion), builds a 5 GiB
ramdisk with a 4 GiB single-device pool pre-faulted on node 0 (as in
VM-NUMA-1105) and a 2 x 500 MiB mirror pool pre-faulted on node 1, and
runs: the cross-socket penalty at 8 jobs (node A vs unbound), the scaling
at 1/2/4/8/16 jobs and on shared files, `perf record` (and `perf c2c`,
`perf lock` when step 0 found them) at 16 jobs, and the mirror pool at 1
and 16 jobs. It destroys its pools and the ramdisk when it ends, also on
a failure, and the chain stops at the first failed pass. Run it detached
and check `all.log` from time to time.

```sh
(
AB=/root/zfs-numa-bench/ab-1106
. "$AB/builds.env" || exit 1
nohup bash -c "'$AB/pass.sh' A1 '$A' && '$AB/pass.sh' B '$B' && '$AB/pass.sh' A2 '$A'; echo \"exit \$?\"" \
    > "$AB/all.log" 2>&1 &
echo "started; follow with: tail -f $AB/all.log"
)
```

When `all.log` ends with `exit 0`:

```sh
(
AB=/root/zfs-numa-bench/ab-1106
tail -1 "$AB/all.log" | grep -qx 'exit 0' || { echo "passes not finished or failed:"; tail -5 "$AB/all.log"; exit 1; }
for p in A1 B A2; do head -2 "$AB/$p/build.txt" | tr '\n' ' '; echo; done
python3 -I "$AB/compare.py"
echo; awk 'NR == 1 || $3 + 0 >= 0.3 || $4 + 0 >= 0.3 || $5 + 0 >= 0.3' "$AB/symbols.txt" | head -40
)
```

## Step 3: cleanup and trim

The passes already destroyed their pools and ramdisks and deleted the
perf data files once reported. A's modules stay loaded, as before this
task. The A and B build trees are kept for a re-run.

```sh
(
AB=/root/zfs-numa-bench/ab-1106
. "$AB/builds.env" || exit 1
"$A/zpool" list -H -o name 2> /dev/null | grep -E -x 'zfsnuma|zfsnumam' && echo "pool still present"
grep -q '^brd ' /proc/modules && echo "brd still loaded"
[ "$(cat /sys/module/zfs/srcversion)" = "$SRC_A" ] || "$AB/use.sh" "$A"
echo "loaded zfs srcversion $(cat /sys/module/zfs/srcversion) (A is $SRC_A)"
du -sh "$AB" "$B"
sudo fstrim -av
)
```

## How to read the results

- `B/A%` is B's IOPS against the mean of A1 and A2; `noise` is
  |A1 - A2| over that mean. Treat a `B/A%` within the row's `noise` (and
  under about 3 %) as no change.
- `hitm/s` counts loads that hit a line modified in the other socket's
  cache. The counters B changes are written by every reader or writer,
  so the expected effect is lower `hitm/s` at 16 jobs unbound, and a
  smaller IOPS gap between `numa-*-A` and `numa-*-all`.
- Where to expect it: `cread` (dbuf cache size, commit 3: two updates per
  read), `uread`/`ureadlz4` (arc_uncached and arc_anon sizes, commits 1
  and 2), `awrite`/`swrite` (arc_anon and arc_mru sizes, commits 1 and
  2), `mir-*` (also commit 4). Nothing for B at 1 job beyond noise: a
  regression there would be the cost of `aggsum_add()` (a bucket mutex)
  over an uncontended atomic.
- `symbols.txt`: in A, the atomics are inlined, so their cost shows as
  self time of `add_reference`, `arc_change_state`, `dbuf_rele_and_unlock`,
  `dbuf_hold_impl`, `arc_get_data_impl`, `arc_free_data_impl`. In B these
  should drop, and `aggsum_add` / `percpu_counter_add_batch` appear with a
  small share. A large `native_queued_spin_lock_slowpath` or
  `mutex_spin_on_owner` in B would point at aggsum bucket or core locks.
- With `perf c2c`, the "Shared Data Cache Line Table" rows with high
  "Rmt HITM" name the bouncing lines (e.g. `arc_change_state`,
  `dbuf_rele_and_unlock`); with `perf lock`, the first callers name the
  contended locks (`vdev_stat_update` = `vdev_stat_lock`, `aggsum_add` =
  aggsum buckets).

## Report (under 40 lines)

- Step 0: kernel, A and B commits, both srcversions (and that each pass's
  `build.txt` shows the expected one), the probe line (EV, C2C, LOCK).
- The short table printed by `compare.py` (`label`, IOPS A1/B/A2, `B/A%`,
  `noise`, `hitm/s`, `cpu%`), as printed.
- The symbols whose share moved most between A and B (from `symbols.txt`),
  and the top lines of the `c2c` or `lock` reports if they exist.
- Anything unexpected (a stop, an error, a missing event, B slower at
  1 job), then the cleanup state and the fstrim summary.

sha256 of every line above (check: head -n -2 VM-CTR-AB-1106.md | sha256sum): 68e41d3dc76032bfa46f8f9f12189bad65d43af9314ee5619e2150d59fb045eb
