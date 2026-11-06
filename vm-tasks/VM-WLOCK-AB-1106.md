# VM 107 task: A/B of two write-path lock fixes (6/11)

Goal: measure whether the two most contended locks that `perf lock` found
in 16K random asynchronous writes on this VM go away, and what it costs:
the `tq_lock` of the only write issue taskq (`z_wr_iss`), and the lock of
the pool's `dp_dirty_dirs` list, taken by `dsl_dir_dirty()` on every
`dbuf_dirty()`. Same VM and protocol as VM-CTR-AB-1106 (2 virtual NUMA
nodes pinned to the T630's two sockets, 16 vCPUs, 5 GiB ramdisk, 4 GiB
single-device pool pre-faulted on node 0, `spa_asize_inflation=6`).

- A = master 1974111f45, the RELEASE build at /var/tmp/zts-numa-1105/M.
- B = fork branch `write-lock-contention`, head
  7dcafc581e26ea8c56aaba59a3e60d0ae01cac5b
  (https://github.com/tigerblue77/zfs), 2 commits on upstream master
  ef0614e724:
  1. `zio_taskq_write_tpq` default 16 -> 6. On 16 CPUs this gives 2 write
     issue taskqs of 6 threads instead of 1 of 12 (from 30 CPUs, one per
     allocator; 80 CPUs or more unchanged). The cost: syncing context
     issues all the blocks of one file through one of them, so a single
     busy file or zvol gets 6 issue threads instead of 12 here.
  2. `dsl_dir_dirty()` returns without the `dp_dirty_dirs` lock when the
     dir is already on that txg's list (`txg_list_member()`).
- B's base is 8 upstream commits after A: the 4 of VM-CTR-AB-1106 (none
  on a path these runs use, from reading) and 4 that touch only ZTS
  files. `spa.c`, `spa_misc.c`, `spl-taskq.c`, `dsl_dir.c` and `txg.c`
  are identical in A and in B's base.
- Five passes, interleaved: A1 (A), B (B), B16 (B with
  `zio_taskq_write_tpq=16`: commit 2 alone), B3 (B with
  `zio_taskq_write_tpq=3`: 4 taskqs of 3 threads, one per allocator, the
  other candidate), A2 (A). A1 vs A2 gives the noise, B16 vs A the effect
  of commit 2, B vs B16 that of commit 1, B3 vs B the choice of 6.

Rules: this VM is disposable; nothing is posted or pushed; results under
/root/zfs-numa-bench/wlock-1106; RELEASE builds only (no --enable-debug,
no --enable-debuginfo); this is the only VM running a task; about 1 h of
runs in all; `sudo fstrim -av` at the end.

## Step 0: build B, check A and B, probe perf (about 15 min)

If this block stops after creating `$AB` or `$B`, remove what it created
before running it again.

```sh
(
set -u
A=/var/tmp/zts-numa-1105/M
B=/var/tmp/zts-wlock-1106/B
WANT=7dcafc581e26ea8c56aaba59a3e60d0ae01cac5b
AB=/root/zfs-numa-bench/wlock-1106
for t in fio numactl perf python3 sfdisk udevadm blockdev lscpu getconf git; do
	command -v "$t" > /dev/null || { echo "missing $t: stopping"; exit 1; }
done
[ -e "$AB" ] && { echo "$AB exists: stopping"; exit 1; }
[ -e "$B" ] && { echo "$B exists: stopping"; exit 1; }
mkdir -p "$AB" /var/tmp/zts-wlock-1106 || exit 1
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
git clone -q --depth 50 --branch write-lock-contention \
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
LOCK=no
if perf lock contention -a -b -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=bpf
elif perf lock record -a -o "$AB/probe.lock" -- sleep 0.5 > /dev/null 2>&1; then
	LOCK=rec
fi
rm -f "$AB/probe.lock"
printf 'LOCK=%s\n' "$LOCK" > "$AB/probe.env"
echo "kernel $k; A $sa; B $sb ($WANT); LOCK=$LOCK"
)
```

## Step 1: write the helper scripts (instant)

`use.sh` loads a build, checks its srcversion and sets the two
parameters; `pass.sh` runs one whole pass; `compare.py` builds the tables.

```sh
(
AB=/root/zfs-numa-bench/wlock-1106
[ -f "$AB/builds.env" ] && [ -f "$AB/probe.env" ] || { echo "run step 0 first"; exit 1; }
cat > "$AB/use.sh" << 'EOF'
#!/bin/bash
# use.sh BUILD_DIR TPQ: load that build's spl/zfs modules (no pool may be
# imported), then set spa_asize_inflation=6 and, unless TPQ is "default",
# zio_taskq_write_tpq=TPQ (read when a pool is created or imported).
set -u
D=$1 tpq=$2
if grep -q '^zfs ' /proc/modules; then
	[ -z "$("$D/zpool" list -H -o name 2> /dev/null)" ] ||
	    { echo "a pool is imported: stopping"; exit 1; }
	"$D/scripts/zfs.sh" -u > /dev/null 2>&1 || { echo "unload failed"; exit 1; }
fi
"$D/scripts/zfs.sh" > /dev/null 2>&1 || { echo "load of $D failed"; exit 1; }
want=$(modinfo -F srcversion "$D/module/zfs.ko")
have=$(cat /sys/module/zfs/srcversion)
[ "$want" = "$have" ] || { echo "loaded zfs $have is not $D's $want: stopping"; exit 1; }
M=/sys/module/zfs/parameters
echo 6 > "$M/spa_asize_inflation" || exit 1
[ "$tpq" = default ] || echo "$tpq" > "$M/zio_taskq_write_tpq" || exit 1
[ "$(cat "$M/spa_asize_inflation")" = 6 ] || { echo "spa_asize_inflation not set: stopping"; exit 1; }
echo "loaded $D zfs srcversion $have, zio_taskq_write_tpq=$(cat "$M/zio_taskq_write_tpq")"
EOF
cat > "$AB/pass.sh" << 'EOF'
#!/bin/bash
# pass.sh LABEL BUILD_DIR TPQ: pool, layout, write runs, profiles, cleanup
set -u
label=$1 D=$2 tpq=$3
AB=/root/zfs-numa-bench/wlock-1106
. "$AB/probe.env"
R=$AB/$label
P=zfswl GB=5
[ -e "$R" ] && { echo "$R exists: stopping"; exit 1; }
"$AB/use.sh" "$D" "$tpq" || exit 1
export PATH="$D:$PATH"
zpool list "$P" > /dev/null 2>&1 && { echo "pool $P exists: stopping"; exit 1; }
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
avail=$(awk '/^MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$avail" -ge 10 ] || { echo "MemAvailable ${avail} GiB < 10: stopping"; exit 1; }
# On any exit, destroy this pass's pool and ramdisk; reports are kept.
cleanup() {
	cd / || return
	zpool list "$P" > /dev/null 2>&1 && { zpool destroy "$P" || echo "zpool destroy $P failed"; }
	if grep -q '^brd ' /proc/modules; then
		if zpool list "$P" > /dev/null 2>&1; then
			echo "the pool is left: ramdisk kept"
		else
			rmmod brd || echo "rmmod brd failed"
		fi
	fi
	rmdir "$R/mnt" 2> /dev/null
}
trap cleanup EXIT
mkdir -p "$R/raw" "$R/mnt" && ln -sfn "$R" /root/zfs-numa-bench/current-wl &&
    cd "$R" || exit 1
NA=$(lscpu -p=SOCKET,NODE | awk -F, '!/^#/ && $1 == 0 { print $2; exit }')
[ -n "$NA" ] || { echo "socket 0 not found: stopping"; exit 1; }
M=/sys/module/zfs/parameters
{ echo "$label $D $(git -C "$D" rev-parse HEAD 2> /dev/null)"
  echo "srcversion $(cat /sys/module/zfs/srcversion)"; uname -r
  for p in zio_taskq_write_tpq spa_asize_inflation zio_taskq_batch_pct \
      spa_num_allocators spa_cpus_per_allocator; do
	echo "$p=$(cat "$M/$p")"
  done; } > build.txt
modprobe brd rd_nr=1 rd_size=$((GB * 1048576)) max_part=16 || exit 1
printf 'label: gpt\nsize=4GiB\n' | sfdisk -q /dev/ram0 > /dev/null || exit 1
udevadm settle
[ -b /dev/ram0p1 ] || { echo "no /dev/ram0p1: stopping"; exit 1; }
# Pre-fault the pool's 4 GiB on node NA, as in VM-CTR-AB-1106.
mib=$(( $(blockdev --getsize64 /dev/ram0p1) / 1048576 ))
numactl --cpunodebind="$NA" --membind="$NA" dd if=/dev/zero of=/dev/ram0p1 \
    bs=1M count="$mib" oflag=direct status=none || exit 1
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$R/mnt" "$P" /dev/ram0p1 || exit 1
zfs create "$P/wplain" &&
    zfs create -o recordsize=128k -o compression=zstd "$P/wzstd" &&
    zfs create -o recordsize=128k -o compression=lz4 "$P/wlz4" || exit 1
# The write issue taskqs of this pool, against spa_taskqs_init()'s formula.
wiss() { ls /proc/spl/kstat/taskq | grep -E '^z_wr_iss(_[0-9]+)?\.[0-9]+$'; }
want=$(awk -v n="$(getconf _NPROCESSORS_ONLN)" -v bp="$(cat $M/zio_taskq_batch_pct)" \
    -v wt="$(cat $M/zio_taskq_write_tpq)" -v na="$(cat $M/spa_num_allocators)" \
    -v cpa="$(cat $M/spa_cpus_per_allocator)" '
    function max(a, b) { return a > b ? a : b }
    function min(a, b) { return a < b ? a : b }
    BEGIN { t = max(1, int(n * bp / 100)); c = max(1, int(t / max(1, wt)))
	c = max(c, int((bp + 99) / 100)); a = max(min(na, int(n / max(cpa, 1))), 1)
	c = min(c, a); while (a % c != 0 && a < c * 2) c--; print c }')
have=$(wiss | wc -l)
thr=$(for f in $(wiss); do
	awk '$1 == "threads_max" { printf "%s ", $3 }' "/proc/spl/kstat/taskq/$f"
done)
echo "wr_iss taskqs: $have (expected $want), threads_max: $thr" >> build.txt
[ "$have" = "$want" ] || { echo "wr_iss taskqs $have, expected $want: stopping"; exit 1; }
L=(numactl --cpunodebind="$NA" --membind="$NA")
lay() {
	tag=$1 ds=$2
	shift 2
	"${L[@]}" fio --name=layout --directory="mnt/$ds" "$@" --bs=1m \
	    --rw=write --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "raw/layout-$tag.log" 2>&1
}
lay perjob wplain --filename_format='f.$jobnum' --size=16m --numjobs=16 &&
    lay shared wplain --filename=shared.0 --size=512m &&
    lay zstd wzstd --filename=shared.0 --size=1g &&
    lay lz4 wlz4 --filename=shared.0 --size=1g || exit 1
zpool sync "$P" || exit 1
cat > run.sh << 'EOR'
#!/bin/bash
# run.sh LABEL DIR RW BS NUMJOBS PERF(none|lock|rec) [shared|perjob [FILESIZE
#        [refill|norefill]]]
set -u
R=$(readlink -f /root/zfs-numa-bench/current-wl) || exit 1
label=$1 dir=$2 rw=$3 bs=$4 nj=$5 pm=$6 fmode=${7:-perjob} fsize=${8:-16m}
refill=${9:-refill}
o=$R/raw/$label
if [ "$fmode" = shared ]; then
	files=(--filename=shared.0 --size="$fsize")
else
	files=(--filename_format='f.$jobnum' --size="$fsize")
fi
[ "$refill" = refill ] && rf=(--refill_buffers) || rf=()
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6 }' /proc/stat; }
tx() { awk '/dmu_tx_delay|dmu_tx_dirty_throttle|dmu_tx_dirty_over_max/ { s += $3 }
    $1 == "dmu_tx_quota" { q = $3 } END { print s + 0, q + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
wq() {
	for f in $(ls /proc/spl/kstat/taskq | grep -E '^z_wr_iss(_[0-9]+)?\.[0-9]+$'); do
		awk '$1 == "tasks_executed" { e = $3 } $1 == "thread_sleeps" { s = $3 }
		    END { print e + 0, s + 0 }' "/proc/spl/kstat/taskq/$f"
	done | awk '{ e += $1; s += $2 } END { print e + 0, s + 0 }'
}
read -r b0 i0 < <(cpu)
read -r t0 q0 < <(tx)
read -r e0 s0 < <(wq)
w0=$(date +%s.%N)
fio --name=t --directory="$R/$dir" "${files[@]}" --rw="$rw" \
    --bs="$bs" --ioengine=psync --numjobs="$nj" --time_based --runtime=30 \
    --ramp_time=5 --group_reporting --fallocate=none "${rf[@]}" \
    --buffer_compress_percentage=50 --output-format=json \
    --output="$o.fio.json" > "$o.fio.log" 2>&1 &
fpid=$!
sleep 10
case $pm in
lock)
	perf lock contention -a -b -E 30 -- sleep 15 > "$o.lock.txt" 2>&1 ||
	    perf lock record -a -o "$o.lock.data" -- sleep 15 > /dev/null 2>&1 ;;
rec)
	perf record -a -g -F 199 -o "$o.perf.data" -- sleep 15 > /dev/null 2>&1 ;;
esac
wait "$fpid"
w1=$(date +%s.%N)
read -r b1 i1 < <(cpu)
read -r t1 q1 < <(tx)
read -r e1 s1 < <(wq)
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
rm -f "$o.perf.data" "$o.lock.data"
fio_cols=$(python3 -I - "$o.fio.json" << 'PY'
import json, sys
try:
    x = json.load(open(sys.argv[1]))["jobs"][0]["write"]
    p = x.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0)
    print("%.0f %.1f %.0f" % (x["iops"], x["bw"] / 1024.0, p / 1000.0))
except Exception:
    print("nan nan nan")
PY
)
busy=$(awk -v b="$((b1 - b0))" -v i="$((i1 - i0))" \
    'BEGIN { printf "%.0f", 100 * b / (b + i) }')
wcols=$(awk -v e="$((e1 - e0))" -v s="$((s1 - s0))" -v w0="$w0" -v w1="$w1" \
    'BEGIN { printf "%.0f %.2f", e / (w1 - w0), (e ? s / e : 0) }')
line="$label $dir $rw $bs $nj $pm $fio_cols $busy $((t1 - t0)) $((q1 - q0)) $wcols"
echo "$line" >> "$R/summary.txt"
echo "$line"
EOR
chmod 0755 run.sh
echo "label dir rw bs jobs perf iops MiB/s p99_us cpu% throttles quota wriss_tasks/s sleeps/task" > summary.txt
# Many files: 16K random overwrites of per-job files, 1 to 16 jobs.
for nj in 1 4 8 16; do
	./run.sh "scale-awrite-$nj" mnt/wplain randwrite 16k "$nj" none < /dev/null
done
# One file: all its blocks go through one write issue taskq.
./run.sh shared-awrite-16 mnt/wplain randwrite 16k 16 none shared 512m < /dev/null
./run.sh zstd-seq-1 mnt/wzstd write 1m 1 none shared 1g norefill < /dev/null
./run.sh zstd-rand-4 mnt/wzstd randwrite 128k 4 none shared 1g norefill < /dev/null
./run.sh lz4-rand-4 mnt/wlz4 randwrite 128k 4 none shared 1g norefill < /dev/null
# Profiles.
if [ "$LOCK" != no ]; then
	./run.sh lock-awrite-8 mnt/wplain randwrite 16k 8 lock < /dev/null
	./run.sh lock-awrite-16 mnt/wplain randwrite 16k 16 lock < /dev/null
	./run.sh lock-shared-awrite-16 mnt/wplain randwrite 16k 16 lock shared 512m < /dev/null
fi
./run.sh prof-awrite-16 mnt/wplain randwrite 16k 16 rec < /dev/null
echo "pass $label done: $(($(wc -l < summary.txt) - 1)) runs, $(du -sh "$R" | cut -f1)"
EOF
cat > "$AB/compare.py" << 'EOP'
#!/usr/bin/env python3
# compare.py: tables of the five passes A1, B, B16, B3, A2.
import os, re, sys
AB = sys.argv[1] if len(sys.argv) > 1 else "/root/zfs-numa-bench/wlock-1106"
P = ["A1", "B", "B16", "B3", "A2"]
rows, order = {}, []
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
def rel(v, a):
    return "%+6.1f" % (100 * (v - a) / a) if a else "     -"
# Columns of summary.txt: 0 label 1 dir 2 rw 3 bs 4 jobs 5 perf 6 iops 7 MiB/s
# 8 p99_us 9 cpu% 10 throttles 11 quota 12 wriss_tasks/s 13 sleeps/task
out = ["%-22s %4s %8s %8s %8s %8s %8s %6s %6s %6s %5s  %s" % ("label", "unit",
    "A1", "B", "B16", "B3", "A2", "B%", "B16%", "B3%", "noise", "cpu% A1/B/B16/B3/A2")]
for lab in order:
    r = rows[lab]
    if not all(p in r for p in P) or r["A1"][5] != "none":
        continue
    col, unit = (6, "iops") if r["A1"][3] == "16k" else (7, "MiB")
    v = dict((p, num(r[p][col])) for p in P)
    a = (v["A1"] + v["A2"]) / 2
    n = "%5.1f" % (100 * abs(v["A1"] - v["A2"]) / a) if a else "    -"
    out.append("%-22s %4s %8.0f %8.0f %8.0f %8.0f %8.0f %s %s %s %s  %s" % (lab, unit,
        v["A1"], v["B"], v["B16"], v["B3"], v["A2"], rel(v["B"], a),
        rel(v["B16"], a), rel(v["B3"], a), n, "/".join(r[p][9] for p in P)))
out.append("")
out.append("%-22s %s" % ("wr_iss tasks/s, sleeps/task", "  ".join(P)))
for lab in order:
    r = rows[lab]
    if all(p in r for p in P) and r["A1"][5] == "none":
        out.append("%-22s %s" % (lab, "  ".join("%s,%s" % (r[p][12], r[p][13]) for p in P)))
open(os.path.join(AB, "compare.txt"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
# Lock waits: total wait per caller function, in ms, over the 15 s window.
U = {"s": 1e3, "ms": 1.0, "us": 1e-3, "ns": 1e-6}
LR = re.compile(r"^\s*(\d+)\s+([\d.]+)\s+(s|ms|us|ns)\s+([\d.]+)\s+(s|ms|us|ns)"
    r"\s+([\d.]+)\s+(s|ms|us|ns)\s+(\S+)\s+(\S+)\s*$")
def locks(path):
    w = {}
    try:
        txt = open(path).read()
    except OSError:
        return None
    for ln in txt.splitlines():
        m = LR.match(ln)
        if m:
            fn = re.sub(r"\+0x[0-9a-f]+$", "", m.group(9))
            key = "%s (%s)" % (fn, m.group(8))
            c, t = w.get(key, (0, 0.0))
            w[key] = (c + int(m.group(1)), t + float(m.group(2)) * U[m.group(3)])
    return w
print()
for lab in [l for l in order if l.startswith("lock-")]:
    per = dict((p, locks(os.path.join(AB, p, "raw", lab + ".lock.txt"))) for p in P)
    keys = {}
    for p in P:
        for k, (c, t) in (per[p] or {}).items():
            keys[k] = max(keys.get(k, 0), t)
    top = sorted(keys, key=lambda k: -keys[k])[:10]
    print("%s: total wait ms (contended k)  %s" % (lab, "  ".join(P)))
    for k in top:
        print("  %-40s %s" % (k[:40], "  ".join(
            "%9.0f(%5.0f)" % ((per[p] or {}).get(k, (0, 0))[1],
            (per[p] or {}).get(k, (0, 0))[0] / 1e3) for p in P)))
syms = ["native_queued_spin_lock_slowpath", "_raw_spin_lock_irqsave",
    "taskq_dispatch_ent", "taskq_thread", "txg_list_add", "dsl_dir_dirty",
    "dsl_dir_willuse_space", "__mutex_lock.constprop.0", "mutex_spin_on_owner",
    "osq_lock", "rrw_enter_read_impl", "rrw_exit", "zil_itx_assign",
    "metaslab_free_concrete", "try_to_wake_up", "__wake_up_common"]
print()
print("%-34s %s" % ("prof-awrite-16 self %", "  ".join(P)))
for s in syms:
    vals = []
    for p in P:
        try:
            txt = open(os.path.join(AB, p, "raw", "prof-awrite-16.report.txt")).read()
        except OSError:
            txt = ""
        m = re.search(r"^\s*([0-9.]+)%.*\]\s+" + re.escape(s) + r"(\s|$)", txt, re.M)
        vals.append(m.group(1) if m else "-")
    if any(v != "-" for v in vals):
        print("%-34s %s" % (s, "  ".join("%5s" % v for v in vals)))
EOP
chmod 0755 "$AB/use.sh" "$AB/pass.sh" "$AB/compare.py"
bash -n "$AB/use.sh" && bash -n "$AB/pass.sh" && python3 -I -m py_compile "$AB/compare.py" &&
    echo "scripts parse"
ls -l "$AB"
)
```

## Step 2: the five passes, A1, B, B16, B3, A2 (about 1 h in all)

Each pass loads its build (checking the srcversion), sets
`spa_asize_inflation=6` and its `zio_taskq_write_tpq`, builds the 5 GiB
ramdisk and the 4 GiB pool pre-faulted on node 0, checks the number of
write issue taskqs against `spa_taskqs_init()`'s formula (1 for A1, A2
and B16, 2 for B, 4 for B3 on 16 vCPUs), and runs: 16K random overwrites
of per-job files at 1/4/8/16 jobs; one shared file at 16 jobs (16K), and
at 1 and 4 jobs with zstd and lz4 128K records, where a single file is
limited to one taskq's threads (the cost of commit 1); `perf lock` at 8
and 16 jobs and on the shared file; `perf record` at 16 jobs. It destroys
its pool and the ramdisk when it ends, also on a failure, and the chain
stops at the first failed pass. Run it detached and check `all.log` from
time to time.

```sh
(
AB=/root/zfs-numa-bench/wlock-1106
. "$AB/builds.env" || exit 1
nohup bash -c "'$AB/pass.sh' A1 '$A' default && '$AB/pass.sh' B '$B' default &&
    '$AB/pass.sh' B16 '$B' 16 && '$AB/pass.sh' B3 '$B' 3 &&
    '$AB/pass.sh' A2 '$A' default; echo \"exit \$?\"" > "$AB/all.log" 2>&1 &
echo "started; follow with: tail -f $AB/all.log"
)
```

When `all.log` ends with `exit 0`:

```sh
(
AB=/root/zfs-numa-bench/wlock-1106
tail -1 "$AB/all.log" | grep -qx 'exit 0' || { echo "passes not finished or failed:"; tail -5 "$AB/all.log"; exit 1; }
for p in A1 B B16 B3 A2; do echo "$p: $(sed -n '2p;4p;9p' "$AB/$p/build.txt" | tr '\n' ' ')"; done
python3 -I "$AB/compare.py" | tee "$AB/compare-all.txt"
)
```

## Step 3: cleanup and trim

The passes already destroyed their pool and ramdisk and deleted the perf
data files once reported. A's modules are loaded again, as before this
task; the A and B build trees are kept for a re-run.

```sh
(
AB=/root/zfs-numa-bench/wlock-1106
. "$AB/builds.env" || exit 1
"$A/zpool" list -H -o name 2> /dev/null | grep -x zfswl && echo "pool still present"
grep -q '^brd ' /proc/modules && echo "brd still loaded"
"$AB/use.sh" "$A" 16 > /dev/null || echo "reload of A failed"
echo 24 > /sys/module/zfs/parameters/spa_asize_inflation
echo "loaded zfs srcversion $(cat /sys/module/zfs/srcversion) (A is $SRC_A)," \
    "tpq $(cat /sys/module/zfs/parameters/zio_taskq_write_tpq)," \
    "inflation $(cat /sys/module/zfs/parameters/spa_asize_inflation)"
du -sh "$AB" "$B"
sudo fstrim -av
)
```

## How to read the results

- `B%`, `B16%`, `B3%` are each pass's IOPS (16K rows) or MiB/s (128K and
  1M rows) against the mean of A1 and A2; `noise` is |A1 - A2| over that
  mean. Treat a change within the row's `noise` (and under about 3 %) as
  no change.
- Commit 2 alone is `B16` against A: expected on `scale-awrite-8/16`, with
  `txg_list_add (mutex)` gone from the `lock-*` tables of B16, B and B3.
- Commit 1 is `B` against `B16`: expected on `scale-awrite-8/16`, with the
  `taskq_dispatch_ent` and `taskq_thread` spinlock waits lower in B and
  lower still in B3. Its cost shows on the one-file rows (`shared-awrite-16`,
  `zstd-*`, `lz4-*`): there B and B3 give one file 6 and 3 issue threads
  instead of 12. A loss there larger than the gain on the many-file rows
  would argue against 6 (or for B3's 3 if B3 loses little).
- `wriss_tasks/s` is the write issue tasks run per second (all z_wr_iss
  taskqs), `sleeps/task` how often a worker slept for one (each sleep is a
  wakeup taken under `tq_lock`); `throttles` and `quota` count dmu_tx
  delays and `dmu_tx_quota` refusals, which should stay near 0 with
  `spa_asize_inflation=6`.
- The `lock-*` tables give, per caller, the total wait over the 15 s
  window and the contended count in thousands: `taskq_dispatch_ent` and
  `taskq_thread` are `tq_lock` (zio taskqs, mostly z_wr_iss here),
  `txg_list_add` the `dp_dirty_dirs` lock, `rrw_enter_read_impl`/`rrw_exit`
  the teardown lock of `zfs_enter()`, `zil_itx_assign` the `itxg_lock`,
  `metaslab_free_concrete` a metaslab's `ms_lock`.

## Report (under 40 lines)

- Step 0: kernel, A and B commits, both srcversions, `LOCK=`, and the
  per-pass line of step 2 (srcversion, tpq, wr_iss taskqs) as printed.
- The first table of `compare.py` (`label` to `cpu%`), as printed, and
  the `lock-awrite-16` and `lock-shared-awrite-16` tables.
- The `prof-awrite-16` symbol lines that moved most.
- Anything unexpected (a stop, an error, a missing event, a throttle or
  quota count far from 0), then the cleanup state and the fstrim summary.

sha256 of every line above (check: head -n -2 VM-WLOCK-AB-1106.md | sha256sum): 72e5352f725dbcae41680cbadb7c272aeb27ea5d8ffedc710a4dc3c77532c979
