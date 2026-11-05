# VM 107 task: NUMA baseline of OpenZFS master (5-6/11)

Goal: measure what a 2-socket placement costs OpenZFS today, in this VM, whose 2 virtual
NUMA nodes are pinned to the two real sockets of the T630 host (vCPU 0-7 -> host node 0
cores 0,2,..,14; vCPU 8-15 -> host node 1 cores 1,3,..,15; 8 GiB of memory bound to each
host node). On the real host every storage device (2 Optane NVMe, the mpt3sas HBA, AHCI)
is on node 0, so the ramdisk "device" below is pre-faulted on node 0 (NA) as well.

Rules: this VM is disposable; nothing is posted or pushed; keep the results under
/root/zfs-numa-bench; delete build trees you no longer need; `sudo fstrim -av` at the end.

## Step 0: build and load OpenZFS master
- Source: upstream openzfs/zfs master at 1974111f45 (or the fork's mirror if you cannot
  fetch upstream: tell me and I will push one). RELEASE build (no --enable-debug, no
  --enable-debuginfo: they change performance). Kernel: the one you use for these tasks.
- Load the modules the way you usually do; record the srcversion of the loaded zfs.ko.
- Install fio, numactl (numastat), linux-perf, python3 if missing.

## Step 1: guest inventory (read-only)
```sh
(
B=/root/zfs-numa-bench; mkdir -p "$B" || exit 1
R=$B/$(date +%Y%m%d-%H%M%S); mkdir "$R" && ln -sfn "$R" "$B/current" && cd "$R" || exit 1
{ uname -r; cat /sys/module/zfs/version; lscpu; lscpu -p=CPU,CORE,SOCKET,NODE; numactl -H;
  cat /proc/sys/kernel/numa_balancing; grep -E 'MemTotal|MemAvailable' /proc/meminfo;
  numastat -m; perf version; perf list 2>/dev/null | grep -i -E 'node-load|remote_dram|remote_hitm'; } > inventory.txt 2>&1
echo "results: $R"; numactl -H | head -4; grep -E 'MemAvailable' inventory.txt
)
```

## Steps 2-4: the protocol, adapted to this VM (brd 4 GiB, MemAvailable >= 8 GiB, 16 vCPUs)
Run the blocks below in order, each as pasted (each runs in a subshell).
### Block 1: ramdisk, test pool, layout, measurement helper (about 3-5 min)

```sh
(
R=$(readlink -f /root/zfs-numa-bench/current) && cd "$R" || exit 1
P=zfsnuma
GB=4
for t in fio numactl numastat perf python3; do
	command -v "$t" > /dev/null || { echo "missing $t: stopping"; exit 1; }
done
zpool list "$P" > /dev/null 2>&1 && { echo "pool $P exists: stopping"; exit 1; }
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
avail=$(awk '/^MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$avail" -ge 8 ] || { echo "MemAvailable ${avail} GiB < 8: stopping"; exit 1; }
disk=$(df -Pk "$R" | awk 'NR == 2 { print int($4 / 1048576) }')
[ "$disk" -ge 5 ] || { echo "only ${disk} GiB free under $R: stopping"; exit 1; }
NA=$(lscpu -p=SOCKET,NODE | awk -F, '!/^#/ && $1 == 0 { print $2; exit }')
NB=$(lscpu -p=SOCKET,NODE | awk -F, '!/^#/ && $1 == 1 { print $2; exit }')
[ -n "$NA" ] && [ -n "$NB" ] || { echo "two sockets not found: stopping"; exit 1; }
EV=""
for e in node-loads node-load-misses node-stores node-store-misses \
    mem_load_uops_l3_miss_retired.local_dram \
    mem_load_uops_l3_miss_retired.remote_dram \
    mem_load_uops_l3_miss_retired.remote_hitm \
    mem_load_uops_l3_miss_retired.remote_fwd \
    context-switches cpu-migrations; do
	perf stat -a -x, -e "$e" -- sleep 0.2 2>&1 > /dev/null |
	    grep -q -E '^[0-9]+,' && EV=${EV:+$EV,}$e
done
printf 'P=%s\nNA=%s\nNB=%s\nMNT=%s\nEV=%s\n' "$P" "$NA" "$NB" "$R/mnt" "$EV" > "$R/env"
mkdir -p raw mnt
memused() { grep -h MemUsed /sys/devices/system/node/node*/meminfo; }
memused > raw/memused.0
modprobe brd rd_nr=1 rd_size=$((GB * 1048576)) max_part=16 || exit 1
# Pre-fault every ramdisk page from a task bound to node NA, so the
# "device" memory lives on socket 0 whatever CPU later does the I/O.
numactl --cpunodebind="$NA" --membind="$NA" dd if=/dev/zero of=/dev/ram0 \
    bs=1M count=$((GB * 1024)) oflag=direct status=none || exit 1
memused > raw/memused.1
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$R/mnt" "$P" /dev/ram0 || exit 1
zfs create "$P/plain" && zfs create "$P/wplain" && zfs create "$P/sync" &&
    zfs create -o primarycache=metadata "$P/nocache" &&
    zfs create -o primarycache=metadata -o compression=lz4 "$P/nocachelz4" ||
    exit 1
A=(numactl --cpunodebind="$NA" --membind="$NA")
for ds in plain wplain sync nocache nocachelz4; do
	"${A[@]}" fio --name=layout --directory="mnt/$ds" \
	    --filename_format='f.$jobnum' --size=16m --bs=1m --rw=write \
	    --numjobs=16 --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "raw/layout-$ds.log" 2>&1 || exit 1
done
for ds in plain wplain; do
	"${A[@]}" fio --name=layout --directory="mnt/$ds" --filename=shared.0 \
	    --size=512m --bs=1m --rw=write --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "raw/layout-$ds-shared.log" 2>&1 || exit 1
done
zfs set sync=always "$P/sync" && zpool sync "$P" || exit 1
memused > raw/memused.2
cat > run.sh << 'EOF'
#!/bin/bash
# run.sh LABEL DATASET RW BS NUMJOBS PLACE(A|B|all) PERF(none|stat|lock|rec|c2c|offcpu) [shared]
set -u
R=$(readlink -f /root/zfs-numa-bench/current) || exit 1
. "$R/env"
label=$1 ds=$2 rw=$3 bs=$4 nj=$5 place=$6 pm=$7 fmode=${8:-perjob}
o=$R/raw/$label
case $place in
A) pre=(numactl --cpunodebind="$NA" --membind="$NA") ;;
B) pre=(numactl --cpunodebind="$NB" --membind="$NB") ;;
*) pre=() ;;
esac
if [ "$fmode" = shared ]; then
	files=(--filename=shared.0 --size=512m)
else
	files=(--filename_format='f.$jobnum' --size=16m)
fi
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6 }' /proc/stat; }
tx() { awk '/dmu_tx_delay|dmu_tx_dirty_throttle|dmu_tx_dirty_over_max/ { s += $3 }
    END { print s + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
dm() { awk '$1 == "demand_data_misses" { print $3 }' /proc/spl/kstat/zfs/arcstats; }
numastat -m > "$o.numastat.0" 2>&1
read -r b0 i0 < <(cpu)
t0=$(tx) m0=$(dm)
"${pre[@]}" fio --name=t --directory="$MNT/$ds" "${files[@]}" --rw="$rw" \
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
offcpu)
	offcputime-bpfcc -K -f 15 > "$o.offcpu.folded" 2> /dev/null ;;
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
	    --percent-limit 0.5 --stdio > "$o.report.txt" 2> /dev/null
	perf report -i "$o.perf.data" --no-children --sort symbol \
	    -g caller,0.5,callee --percent-limit 2 --stdio \
	    > "$o.callers.txt" 2> /dev/null
fi
if [ -s "$o.c2c.data" ]; then
	perf c2c report -i "$o.c2c.data" --stdio --full-symbols \
	    > "$o.c2c.txt" 2> /dev/null
	perf c2c report -i "$o.c2c.data" --stats > "$o.c2c.stats" 2>&1
fi
numastat -m > "$o.numastat.1" 2>&1
perf_cols="- -"
if [ -s "$o.perfstat" ]; then
	perf_cols=$(awk -F, '{ v[$3] = $1 } END {
	    l = v["node-loads"]; m = v["node-load-misses"]
	    h = v["mem_load_uops_l3_miss_retired.remote_hitm"]
	    printf "%s %s", (l > 0 ? sprintf("%.1f", 100 * m / l) : "-"),
	        (h != "" ? sprintf("%.0f", h / 15) : "-") }' "$o.perfstat")
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
line="$label $ds $rw $bs $nj $place $pm $fio_cols $busy $perf_cols $((t1 - t0)) $((m1 - m0))"
echo "$line" >> "$R/summary.txt"
echo "$line"
EOF
chmod 0755 run.sh
echo "label ds rw bs jobs place perf iops MiB/s p99_us cpu% remote% hitm/s throttles arc_dmiss" > summary.txt
awk 'FNR == 1 { f++ } { split($0, a, " "); k = a[1] a[2]; v[f, k] = $(NF - 1) }
    END { for (x in v) { split(x, s, SUBSEP); if (s[1] == 1) printf "%s ramdisk %+d MiB, after layout %+d MiB\n", s[2], (v[2, s[2]] - v[1, s[2]]) / 1024, (v[3, s[2]] - v[2, s[2]]) / 1024 } }' \
    raw/memused.0 raw/memused.1 raw/memused.2 | sort
echo "NA=$NA NB=$NB events: ${EV:-none}"
awk '$1 == "c_max" { printf "ARC c_max %.1f GiB (cached tests need ~6)\n", $3 / 2^30 }' \
    /proc/spl/kstat/zfs/arcstats
zpool list -H -o name,size,alloc,health "$P"
)
```

### Block 2: NUMA placement, 8 jobs, A vs B vs unbound, two repetitions (about 15 min)

```sh
(
R=$(readlink -f /root/zfs-numa-bench/current) && [ -x "$R/run.sh" ] || exit 1
for rep in 1 2; do
	for w in "cread plain randread 4k" "uread nocache randread 16k" \
	    "ureadlz4 nocachelz4 randread 16k" "awrite wplain randwrite 16k"; do
		set -- $w
		for place in A B all; do
			"$R/run.sh" "numa-$1-$place-r$rep" "$2" "$3" "$4" 8 "$place" stat
		done
	done
done > "$R/numa.txt"
echo "label ds rw bs jobs place perf iops MiB/s p99_us cpu% remote% hitm/s throttles arc_dmiss"
cat "$R/numa.txt"
)
```

### Block 3: contention scaling, 1/2/4/8/16 jobs plus shared-file runs (about 14 min)

```sh
(
R=$(readlink -f /root/zfs-numa-bench/current) && [ -x "$R/run.sh" ] || exit 1
for w in "cread plain randread 4k" "uread nocache randread 16k" \
    "awrite wplain randwrite 16k" "swrite sync randwrite 16k"; do
	set -- $w
	for nj in 1 2 4 8 16; do
		"$R/run.sh" "scale-$1-$nj" "$2" "$3" "$4" "$nj" all stat
	done
done > "$R/scale.txt"
"$R/run.sh" shared-cread-16 plain randread 4k 16 all stat shared >> "$R/scale.txt"
"$R/run.sh" shared-awrite-16 wplain randwrite 16k 16 all stat shared >> "$R/scale.txt"
# Per-job efficiency against the single-job run of the same workload.
awk '{ split($1, a, "-") } a[1] == "scale" && $5 == 1 { base[a[2]] = $8 }
    a[1] == "scale" { e = (base[a[2]] > 0) ? 100 * $8 / ($5 * base[a[2]]) : 0
        printf "%-8s jobs=%-3s iops=%-9s eff=%3.0f%% cpu=%s%% remote=%s%% hitm/s=%s thr=%s dmiss=%s\n",
        a[2], $5, $8, e, $11, $12, $13, $14, $15 }
    a[1] == "shared" { printf "%-8s jobs=%-3s iops=%-9s (one shared file)\n", a[2], $5, $8 }' \
    "$R/scale.txt"
)
```

### Block 5: cleanup (results are kept)

```sh
(
R=$(readlink -f /root/zfs-numa-bench/current) && . "$R/env" || exit 1
[ "$P" = zfsnuma ] || { echo "unexpected pool name in env: stopping"; exit 1; }
if zpool list "$P" > /dev/null 2>&1; then
	zpool destroy "$P" || { echo "zpool destroy failed: ramdisk kept"; exit 1; }
fi
grep -q '^brd ' /proc/modules && { rmmod brd || echo "rmmod brd failed"; }
mountpoint -q "$R/shm" && { umount "$R/shm" || echo "umount $R/shm failed"; }
rmdir "$R/mnt" "$R/shm" 2> /dev/null
zpool list -H -o name 2> /dev/null | grep -qx "$P" && echo "pool still present"
grep -q '^brd ' /proc/modules && echo "brd still loaded"
echo "clean; results kept in $R ($(du -sh "$R" | cut -f1))"
)
```


## How to read the results (from the study)
### 9.2 How to read the results

**Block 0**
- `node N cpus:` shows the CPU numbering: contiguous ranges, or alternating between sockets (which matters for the aggsum buckets).
- `pci ... node=` shows the Optane's and the HBA's node. That is where VMs and benchmarks should be placed.
- `zfs` gives the version: ≤ 2.4.4 means the `dp_lock`/`dd_lock`/`scl_lock` story applies.
- If Cluster-on-Die is enabled there are 4 nodes; block 1 still picks one node per socket.

**Block 1**
- `Node<NA> ramdisk +10240 MiB` confirms the device memory sits on socket 0.
- The "after layout" deltas show where the ARC data landed (it should be NA).
- `ARC c_max` below about 6 GiB makes the cached tests miss; `arc_dmiss` then grows.

**Block 2**, comparing the same workload across placements (r1 vs r2 gives the noise; treat differences under about 3 % as noise):

| Workload | A vs B shows | `all` shows |
|---|---|---|
| `cread` | the cost of the ARC being one shared pool: B reads data homed on A | — |
| `uread` / `ureadlz4` | the device-locality cost | the cost of spreading jobs across sockets. Random completion placement hurts all three placements; prototype #1/#2 should shrink `remote%` and `hitm/s` and close the A/all gap |

Columns:
- `remote%` = `node-load-misses / node-loads`, the share of DRAM loads served by the other node.
- `hitm/s` = remote HITM loads per second: cache lines modified on the other socket, from true or false sharing.

**Block 3**
- `eff` = IOPS ÷ (jobs × IOPS at 1 job). The knee is where `eff` drops sharply.
  - Expect HT effects above 44 jobs.
  - Expect cross-socket effects between 16 and 44 when `hitm/s` jumps.
- High `cpu%` with flat IOPS means spinning (`native_queued_spin_lock_slowpath`, `osq_lock`, `mutex_spin_on_owner` in block 4).
- Low `cpu%` with flat IOPS means sleeping on a lock or on the ZIL (block 4 off-CPU).
- `thr` > 0 means the dirty-data throttle is active, which is not lock contention.
- `shared-*` versus `scale-*-44` isolates per-file locks (rangelock, `dn_struct_rwlock`, zfetch).

**Block 4**
- The `perf lock` caller names map onto §5: for example `taskq_dispatch_ent` = `tq_lock`, `vdev_stat_update` = `vdev_stat_lock`, `rrw_enter_read_impl` = teardown lock, `zil_commit_writer` = `zl_issuer_lock`, `dsl_pool_dirty_space` = `dp_lock` on 2.4.x.
- In `*.c2c.txt`, the "Shared Data Cache Line Table" rows with high "Rmt HITM" name the bouncing lines: for example `add_reference`/`arc_change_state` = `arcs_*size`, `dbuf_rele_and_unlock` = dbuf cache size, `aggsum_add` = buckets.
- The full call graphs are in `*.callers.txt`.

**Block 6** versus the `scale-*-44/88` rows shows whether `tq_lock`/`mca_lock` contention is relieved by more taskqs or allocators.

### 9.3 A/B-testing a patched module

Do not do this on the host: replacing the module (`zfs.sh -r`) means exporting every production pool.

Use a disposable VM on this host with two virtual NUMA nodes pinned to the two sockets:
- Proxmox `numa: 1`;
- `numa0: cpus=...,hostnodes=0,memory=...,policy=bind` and `numa1: ...hostnodes=1...`;
- vCPU threads pinned per node with `taskset`.

Run blocks 1-5 inside the VM on stock and patched builds, alternating, with at least 2 repetitions each, and matching release builds (no `--enable-debuginfo`). The VM only approximates the host: its vCPU threads can still be descheduled.

## 10. Not verified, open points

## Report (under 40 lines)
- Step 0: commit, kernel, zfs.ko srcversion; which perf events Block 1 found (EV line).
- Block 2 table (numa.txt) as printed, and for each workload: IOPS and p99 of B vs A
  (in %), and remote% / hitm/s for A, B and all.
- Block 3 table (scale.txt): per workload the efficiency per job from 1 to 16 jobs.
- Anything unexpected (errors, missing events, a block that stopped), then cleanup
  state and the fstrim summary.
