#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# TASK-1118-VQ1 driver: the vdev queue's sync classes while blk-wbt is
# scaled down.  Upstream master against fork commit 846b9194b4a, which
# submits sync writes and flushes REQ_SYNC|REQ_IDLE (P1), against P1 plus
# the age guard (zfs_vdev_queue_age_ms), and against P1 with the zvol's
# writes logged as copies instead of indirect.
#
# Five memory-backed null_blk devices made to look like SATA HDDs (8 ms per
# request, 150 MiB/s, 32 tags, a volatile write cache, mq-deadline, marked
# rotational), with blk-wbt's read latency target at 4 ms, below the 8 ms
# every read takes, so that blk-wbt scales down after each window in which
# it saw a read; a raidz2 over them; and TASK-1118-WBT's load for RAMP + MEAS
# seconds: a writer of 512 KiB O_DIRECT writes to a zvol of volblocksize 1M,
# 100 per second, with an fdatasync() after every 150; a reader of 32K to
# 256K O_DIRECT reads, 10 per second, from a zvol with primarycache=metadata;
# and 60 MiB/s of buffered writes to a file.  After each import the written
# zvol is read once, so that its blocks are in the ARC as on the host, and
# its writes need no read-modify-write reads.
#
# Arms: M   = $T/M, master c913da86156
#       P1  = $T/P, 846b9194b4a, zfs_vdev_disk_sync_writes=1
#       P1A = $T/A, the age guard, zfs_vdev_disk_sync_writes=1,
#             zfs_vdev_queue_age_ms=50
#       P1I = $T/P, zfs_vdev_disk_sync_writes=1,
#             zfs_immediate_write_sz=2097152: the writer's 512 KiB records
#             are below it, so zil_write_state() logs them WR_NEED_COPY
#             (data copied into log blocks at commit) instead of WR_INDIRECT
#             (the whole 1M block written by dmu_sync() at commit)
#
#   vq1-bench.sh setup               devices, pool, zvols, prefill
#   vq1-bench.sh quiet SECONDS       idle, iowait, steal and load1
#   vq1-bench.sh pass NAME ARMS      one run of each arm of ARMS, in order
#   vq1-bench.sh teardown            pool, modules and devices removed
#
# Every run exports the pool and imports it again (after reloading the
# modules when the tree changes), so each starts with its zpool iostat
# histograms empty; the measured window is the difference between the
# counters taken at its start and at its end.  During the window, zpool
# iostat -l -q (per leaf) and -w (pool) report every second; each line is
# stamped with the time it arrived.
set -u
export LC_ALL=C

T=/var/tmp/task-1118-vq1
L=/var/log/task-1118-vq1
H=$(dirname "$(readlink -f "$0")")
P=/sys/module/zfs/parameters
CFS=/sys/kernel/config/nullb
POOL=vqp
NB="vq0 vq1 vq2 vq3 vq4"
NB_SIZE=640         # MiB per device
NB_LAT=8000000      # ns per request
NB_MBPS=150
NB_QD=32
NB_CACHE=32         # MiB of volatile write cache per device
WBT_LAT=4000        # usec, below NB_LAT
RAMP=${RAMP:-10}
MEAS=${MEAS:-60}
IVL=0.25            # sampler interval, seconds
W_RATE=100
W_BS=524288
W_EVERY=150
R_RATE=10
B_MBPS=60
B_SIZE=256
ARC_MAX=$((1 << 30))
TXG_HIST=200
AGE_MS=50
IMM_DEF=32768
IMM_COPY=2097152
MIN_AVAIL=1024      # MiB of MemAvailable required before each run
PATH_BASE=$PATH
Z=""

die() { echo "vq1-bench: $*" >&2; log "STOP: $*"; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }

arm_tree() {
    case $1 in
    M) echo "$T/M" ;;
    P1|P1I) echo "$T/P" ;;
    P1A) echo "$T/A" ;;
    *) die "unknown arm $1" ;;
    esac
}

use_tree() {
    Z=$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    export PATH="$Z:$PATH_BASE"
    command -v zpool | grep -qx "$Z/zpool" || die "zpool is not $Z/zpool"
}

# load_tree TREE: unload the ZFS modules, load TREE's, check srcversion.
load_tree() {
    local want got
    use_tree "$1"
    if lsmod | grep -q '^zfs '; then
        "$Z/scripts/zfs.sh" -u || die "unload"
    fi
    "$Z/scripts/zfs.sh" || die "load $1"
    want=$(modinfo -F srcversion "$Z/module/zfs.ko")
    got=$(cat /sys/module/zfs/srcversion)
    if [ -z "$want" ] || [ "$want" != "$got" ]; then
        die "srcversion $got loaded, $1 built $want"
    fi
    echo "$ARC_MAX" > $P/zfs_arc_max || die "zfs_arc_max"
    echo "$TXG_HIST" > $P/zfs_txg_history || die "zfs_txg_history"
    echo "$1" > "$T/cur-tree"
}

# setp NAME VALUE: write a module parameter and read it back.  With VALUE
# "absent", check instead that the loaded tree does not have it.
setp() {
    local v
    if [ "$2" = absent ]; then
        [ -e "$P/$1" ] && die "$Z has $1"
        return 0
    fi
    [ -e "$P/$1" ] || die "$Z has no $1"
    echo "$2" > "$P/$1" || die "$1=$2"
    v=$(cat "$P/$1")
    [ "$v" = "$2" ] || die "$1 reads $v, not $2"
}

params() {
    case $1 in
    M) setp zfs_vdev_disk_sync_writes absent
        setp zfs_vdev_queue_age_ms absent
        setp zfs_immediate_write_sz $IMM_DEF ;;
    P1) setp zfs_vdev_disk_sync_writes 1
        setp zfs_vdev_queue_age_ms absent
        setp zfs_immediate_write_sz $IMM_DEF ;;
    P1A) setp zfs_vdev_disk_sync_writes 1
        setp zfs_vdev_queue_age_ms $AGE_MS
        setp zfs_immediate_write_sz $IMM_DEF ;;
    P1I) setp zfs_vdev_disk_sync_writes 1
        setp zfs_vdev_queue_age_ms absent
        setp zfs_immediate_write_sz $IMM_COPY ;;
    *) die "unknown arm $1" ;;
    esac
}

pval() {
    if [ -e "$P/$1" ]; then
        echo "$1=$(cat "$P/$1")"
    else
        echo "$1=absent"
    fi
}

# zvol_of NAME: the block device of zvol NAME, by its /dev/zvol link when
# udev made one, else the zd device the loaded tree's zvol_id names so.
zvol_of() {
    local d
    if [ -b "/dev/zvol/$1" ]; then
        echo "/dev/zvol/$1"
        return
    fi
    for d in /dev/zd*; do
        case $d in *p*) continue ;; esac
        [ -b "$d" ] || continue
        if [ "$("$Z/udev/zvol_id" "$d" 2>/dev/null)" = "$1" ]; then
            echo "$d"
            return
        fi
    done
}

zvols_wait() {
    local _
    for _ in $(seq 1 60); do
        [ -n "$(zvol_of $POOL/w)" ] && [ -n "$(zvol_of $POOL/r)" ] &&
            return 0
        udevadm settle
        sleep 1
    done
    die "zvols $POOL/w and $POOL/r did not appear"
}

nullb_add() {   # nullb_add NAME
    local d=$CFS/$1
    mkdir "$d" || return 1
    echo "$NB_SIZE" > "$d/size" &&
        echo 4096 > "$d/blocksize" &&
        echo 1 > "$d/memory_backed" &&
        echo 2 > "$d/irqmode" &&
        echo "$NB_LAT" > "$d/completion_nsec" &&
        echo 1 > "$d/submit_queues" &&
        echo "$NB_QD" > "$d/hw_queue_depth" &&
        echo "$NB_MBPS" > "$d/mbps" &&
        echo "$NB_CACHE" > "$d/cache_size" &&
        echo 1 > "$d/power" || return 1
    udevadm settle
    [ -b "/dev/$1" ]
}

# set_wbt LAT: wbt_lat_usec of the five devices, read back.  Writing 1
# first makes blk-wbt start again from its initial depth in every run: it
# keeps its scaling step when the value written is the one it has.
set_wbt() {
    local n v
    for n in $NB; do
        if ! echo 1 > "/sys/block/$n/queue/wbt_lat_usec" ||
            ! echo "$1" > "/sys/block/$n/queue/wbt_lat_usec"; then
            die "wbt_lat_usec $1 on $n"
        fi
        v=$(cat "/sys/block/$n/queue/wbt_lat_usec")
        [ "$v" = "$1" ] || die "$n wbt_lat_usec reads $v, not $1"
    done
}

devinfo() {
    local n q
    for n in $NB; do
        q=/sys/block/$n/queue
        echo "$n dev $(cat "/sys/block/$n/dev")" \
            "sched $(cat "$q/scheduler")" "rot $(cat "$q/rotational")" \
            "wbt_lat_usec $(cat "$q/wbt_lat_usec")" \
            "write_cache $(tr ' ' _ < "$q/write_cache")" \
            "nr_requests $(cat "$q/nr_requests")" \
            "max_sectors_kb $(cat "$q/max_sectors_kb")" \
            "mbps $(cat "$CFS/$n/mbps")" \
            "completion_nsec $(cat "$CFS/$n/completion_nsec")" \
            "hw_queue_depth $(cat "$CFS/$n/hw_queue_depth")" \
            "cache_size $(cat "$CFS/$n/cache_size")" \
            "wbt_debugfs $([ -r "/sys/kernel/debug/block/$n/rqos/wbt/enabled" ] &&
                cat "/sys/kernel/debug/block/$n/rqos/wbt/enabled" || echo -)"
    done
}

memavail() { awk '/^MemAvailable/ {print int($2 / 1024)}' /proc/meminfo; }

setup() {
    local n q v devs=()
    mkdir -p "$T" "$L" || die "mkdir"
    [ -n "${EPOCHREALTIME:-}" ] || die "this bash has no EPOCHREALTIME"
    echo "task-1118-vq1: start" > /dev/kmsg
    for n in $NB; do [ -e "/dev/$n" ] && die "/dev/$n exists"; done
    modprobe null_blk nr_devices=0 || die "null_blk is not available"
    for n in $NB; do
        nullb_add "$n" || die "null_blk $n"
        q=/sys/block/$n/queue
        echo mq-deadline > "$q/scheduler" || die "$n scheduler"
        grep -q '\[mq-deadline\]' "$q/scheduler" || die "$n not mq-deadline"
        echo 1 > "$q/rotational" || die "$n rotational"
        [ "$(cat "$q/rotational")" = 1 ] || die "$n not rotational"
        # -1: the default target for the device as it now is.
        echo -1 > "$q/wbt_lat_usec" || die "$n wbt_lat_usec -1"
        v=$(cat "$q/wbt_lat_usec")
        [ "$v" = 75000 ] || die "$n wbt_lat_usec $v, not the rotational 75000"
        [ "$(cat "$q/write_cache")" = "write back" ] ||
            die "$n write_cache is $(cat "$q/write_cache")"
        devs+=("/dev/$n")
    done
    devinfo > "$L/setup-devices.txt"
    load_tree "$T/M"
    zpool create -f -o ashift=12 -O atime=off -O compression=off \
        $POOL raidz2 "${devs[@]}" || die "zpool create"
    zfs create -V 320M -o volblocksize=1M $POOL/w || die "$POOL/w"
    zfs create -V 320M -o volblocksize=1M -o primarycache=metadata \
        $POOL/r || die "$POOL/r"
    zfs create -o recordsize=1M $POOL/bulk || die "$POOL/bulk"
    zvols_wait
    for n in w r; do
        dd if=/dev/urandom of="$(zvol_of $POOL/$n)" bs=1M count=320 \
            oflag=direct status=none || die "prefill $n"
    done
    dd if=/dev/urandom of=/$POOL/bulk/f bs=1M count=$B_SIZE status=none ||
        die "prefill bulk"
    zpool sync $POOL || die "sync"
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zfs get -r volsize,volblocksize,recordsize,primarycache,compression,sync \
        $POOL > "$L/setup-props.txt"
    log "setup done: $(zpool list -Ho size,alloc,cap $POOL)," \
        "MemAvailable $(memavail) MiB"
}

# arm_set ARM DIR: export, load ARM's tree if another is loaded, set the
# arm's parameters, import, set blk-wbt's target, read the written zvol.
arm_set() {
    local tree cur w a b k
    tree=$(arm_tree "$1")
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    use_tree "${cur:-$tree}"
    if zpool list -H $POOL > /dev/null 2>&1; then
        zpool export $POOL || die "export"
    fi
    if [ "$cur" != "$tree" ]; then
        load_tree "$tree"
    fi
    params "$1"
    zpool import -d /dev $POOL || die "import"
    zvols_wait
    set_wbt $WBT_LAT
    w=$(zvol_of $POOL/w)
    a=$EPOCHREALTIME
    dd if="$w" of=/dev/null bs=1M iflag=direct status=none ||
        die "prewarm $w"
    b=$EPOCHREALTIME
    {
        echo "arm $1 tree $Z"
        echo "srcversion $(cat /sys/module/zfs/srcversion)"
        pval zfs_vdev_disk_sync_writes
        pval zfs_vdev_queue_age_ms
        pval zfs_immediate_write_sz
        for k in zfs_arc_max zfs_dirty_data_max zfs_txg_timeout \
            zfs_txg_history zfs_vdev_async_write_min_active \
            zfs_vdev_async_write_max_active zfs_vdev_sync_read_max_active \
            zfs_vdev_sync_write_max_active zfs_vdev_aggregation_limit; do
            pval $k
        done
        echo "prewarm $(echo "$a $b" | awk '{printf "%.2f", $2 - $1}') s"
        echo "pool $(zpool list -Ho size,alloc,cap $POOL)"
        echo "memavail $(memavail)"
        devinfo
    } > "$2/layout.txt"
}

# snap DIR N: the counters at the start (0) or the end (1) of the window.
snap() {
    local f
    {
        echo "time $(date +%s.%N)"
        head -1 /proc/stat
        echo "loadavg $(cat /proc/loadavg)"
        sed 's/^/psi_io /' /proc/pressure/io
        awk '$3 ~ /^vq[0-4]$/ {print "disk", $0}' /proc/diskstats
        awk 'NR > 2 {print "zil", $1, $3}' /proc/spl/kstat/zfs/zil
        awk 'NR > 2 {print "dmu_tx", $1, $3}' /proc/spl/kstat/zfs/dmu_tx
        awk 'NR > 2 {print "arc", $1, $3}' /proc/spl/kstat/zfs/arcstats
        awk 'NR > 2 {print "assign", $1, $NF}' \
            /proc/spl/kstat/zfs/$POOL/dmu_tx_assign
        for f in "/proc/spl/kstat/zfs/$POOL"/objset-*; do
            [ -e "$f" ] || continue
            awk -v o="${f##*/}" 'NR > 2 {print "objset", o, $1, $3}' "$f"
        done
    } > "$1/snap$2.txt"
    zpool iostat -wpH $POOL > "$1/iostat-w$2.txt" 2>&1
    zpool iostat -rpH $POOL > "$1/iostat-r$2.txt" 2>&1
    cp /proc/spl/kstat/zfs/$POOL/txgs "$1/txgs$2.txt" 2>/dev/null
}

# stamp: prefix each line read with the Unix time it arrived at.
stamp() {
    local l
    while IFS= read -r l; do
        printf '%s %s\n' "$EPOCHREALTIME" "$l"
    done
}

kcheck() {
    if dmesg | sed -n '/task-1118-vq1: start/,$p' |
        grep -E 'hung_task|blocked for more than|BUG:|Oops|VERIFY|PANIC|Call Trace' \
        > "$1"; then
        die "kernel log, see $1"
    fi
    rm -f "$1"
}

# loads DIR SECONDS: start the three load streams and the sampler.
loads() {
    local w r
    w=$(zvol_of $POOL/w)
    r=$(zvol_of $POOL/r)
    if [ ! -b "$w" ] || [ ! -b "$r" ]; then
        die "zvol devices"
    fi
    # shellcheck disable=SC2086
    python3 -I "$H/vq1-sample.py" "$2" $IVL "$1/samples.txt" $NB &
    PIDS=($!)
    python3 -I "$H/wbt-load.py" writer "$w" "$2" "$1/writer.txt" \
        $W_RATE $W_BS $W_EVERY &
    PIDS+=($!)
    python3 -I "$H/wbt-load.py" reader "$r" "$2" "$1/reader.txt" $R_RATE &
    PIDS+=($!)
    python3 -I "$H/wbt-load.py" bulk /$POOL/bulk/f "$2" "$1/bulk.txt" \
        $B_MBPS $B_SIZE &
    PIDS+=($!)
}

waitall() {
    local p rc=0
    for p in "${PIDS[@]}"; do
        wait "$p" || rc=$?
    done
    return $rc
}

run_one() { # run_one PASS ARM
    local dir=$L/raw/$1/$2 rc ipid wpid ma
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2"
    mkdir -p "$dir" || die "mkdir $dir"
    arm_set "$2" "$dir"
    echo "window ramp $RAMP meas $MEAS ivl $IVL" >> "$dir/layout.txt"
    sleep 2
    loads "$dir" $((RAMP + MEAS))
    sleep "$RAMP"
    snap "$dir" 0
    zpool iostat -T u -Hpvlq $POOL 1 $((MEAS + 1)) 2>&1 | stamp \
        > "$dir/iostat-lq.txt" &
    ipid=$!
    zpool iostat -T u -wpH $POOL 1 $((MEAS + 1)) 2>&1 | stamp \
        > "$dir/iostat-w1s.txt" &
    wpid=$!
    sleep "$MEAS"
    snap "$dir" 1
    waitall
    rc=$?
    wait $ipid $wpid
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir"
    log "run $1 $2 done:" \
        "$(awk '$2 == "F" {n++; s += $4} END {
            printf "fsync n %d mean %.0f ms", n, n ? s / n / 1e6 : 0}' \
            "$dir/writer.txt")"
}

pass() {    # pass NAME "ARMS"
    local a
    for a in $2; do arm_tree "$a" > /dev/null; done
    mkdir -p "$L/raw/$1" || die "mkdir"
    echo "arms $2" > "$L/raw/$1/order.txt"
    log "pass $1 start (arms $2)"
    for a in $2; do
        run_one "$1" "$a"
    done
    log "pass $1 done"
}

quiet() {
    local a b l0 l1
    a=$(head -1 /proc/stat)
    l0=$(cut -d' ' -f1 /proc/loadavg)
    sleep "$1"
    b=$(head -1 /proc/stat)
    l1=$(cut -d' ' -f1 /proc/loadavg)
    log "quiet $1 s: $(echo "$a" "$b" | awk '{
        n = (NF - 2) / 2
        for (i = 1; i <= 8; i++) { d[i] = $(i + n + 2) - $(i + 1); t += d[i] }
        if (t == 0) t = 1
        printf "idle %.2f %% iowait %.2f %% steal %.2f %%", \
            100 * d[4] / t, 100 * d[5] / t, 100 * d[8] / t }')" \
        "load1 $l0 -> $l1"
}

teardown() {
    local n cur
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    if [ -n "$cur" ]; then
        use_tree "$cur"
        zpool list -H $POOL > /dev/null 2>&1 && zpool destroy $POOL
        "$Z/scripts/zfs.sh" -u
    fi
    for n in $NB; do
        if [ -d "$CFS/$n" ]; then
            echo 0 > "$CFS/$n/power"
            rmdir "$CFS/$n"
        fi
    done
    modprobe -r null_blk
    rm -f "$T/cur-tree"
    log "teardown done"
}

mkdir -p "$L" || exit 1
case ${1:-} in
setup) setup ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    quiet "$2" ;;
pass) [ $# -eq 3 ] || die "usage: pass NAME ARMS"
    [ -f "$T/cur-tree" ] || die "no setup"
    pass "$2" "$3" ;;
teardown) teardown ;;
*) die "usage: setup | quiet SECONDS | pass NAME ARMS | teardown" ;;
esac
