#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# TASK-1120-WBP4 driver: queued TRIMs released at a TRIM's completion,
# issued inline by z_trim_int (M = cde5d598eb4, upstream master) or handed
# to the TRIM issue taskq (T = 1fd3fd8c642), while blk-wbt lets one discard
# per disk through.
#
# Four memory-backed null_blk devices with discard (2 ms per request by
# timer, 2 tags, mq-deadline, marked rotational, blk-wbt at its rotational
# 75 ms target).  blk-wbt takes its depth from nr_requests when the disk
# registers, twice the tag count under mq-deadline, so 2 tags give a
# discard limit (wb_background) of 1, below zfs_vdev_trim_max_active (2):
# the second TRIM issued to a disk waits in wbt_wait() for the first.
# A stripe of the four (cachefile=none, mounted under $T/mnt) holds frag
# (16384 files of 64K written, every other one deleted: about 8192 free
# 64K extents), churn and sync (a 64 MiB file, recordsize 8K).
#
# Rows: mt  a sync writer (8K pwrite + fdatasync, 50 per second) while
#           "zpool trim -w" trims the whole pool, timed (at most TRIM_MAX s)
#       at  autotrim on (zfs_trim_txg_batch 1), the sync writer, and churn:
#           64K files created 400 per second, a random one deleted after
#           each creation once 4000 exist; RAMP_AT + MEAS_AT seconds
# During each window the discards to the four devices are traced in a
# tracefs instance (block_bio_queue, block_getrq, block_rq_issue,
# block_rq_complete), the z_trim_* threads are sampled every 20 ms (state,
# wbt_wait on the stack) and their schedstat times read at both ends.
#
#   wbp4-bench.sh setup               devices, trace instance, pool, layout
#   wbp4-bench.sh flags               mt and a short at, on both trees
#   wbp4-bench.sh quiet SECONDS       idle, iowait, steal and load1
#   wbp4-bench.sh pass NAME ARMS      mt then at on each arm of ARMS
#   wbp4-bench.sh teardown            pool, modules, devices removed
#
# WBP4_TEST_ROOT prefixes every system path, for the kit's own tests; sudo
# drops it from the environment.
set -u
export LC_ALL=C

R=${WBP4_TEST_ROOT:-}
T=/var/tmp/task-1120-wbp4
L=/var/log/task-1120-wbp4
if [ -n "$R" ]; then
    T=$R$T
    L=$R$L
fi
H=$(dirname "$(readlink -f "$0")")
P=$R/sys/module/zfs/parameters
CFS=$R/sys/kernel/config/nullb
SB=$R/sys/block
DBG=$R/sys/kernel/debug/block
SCHED=$R/proc/sys/kernel/sched_schedstats
POOL=wbp4p
NBS="wbp4nb0 wbp4nb1 wbp4nb2 wbp4nb3"
MNT=$T/mnt
NB_SIZE=1024        # MiB per device
NB_LAT=2000000      # ns per request
NB_QD=2
FRAG_N=16384
FRAG_BS=65536
SYNC_RATE=50
SYNC_BS=8192
CHURN_RATE=400
CHURN_BS=65536
CHURN_KEEP=4000
RAMP_MT=${RAMP_MT:-5}
TRIM_MAX=${TRIM_MAX:-120}
RAMP_AT=${RAMP_AT:-10}
MEAS_AT=${MEAS_AT:-40}
TXG_BATCH=1
ARC_MAX=1073741824
TXG_HIST=100
TRACE_KB=8192
EVENTS="block_bio_queue block_getrq block_rq_issue block_rq_complete"
MIN_AVAIL=1024
PATH_BASE=$PATH
Z=""
TI=""

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
die() { echo "wbp4-bench: $*" >&2; log "STOP: $*"; exit 1; }

arm_tree() {
    case $1 in
    M) echo "$T/M" ;;
    T) echo "$T/T" ;;
    *) die "unknown arm $1" ;;
    esac
}

use_tree() {
    Z=$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    PATH="$Z:$PATH_BASE"
    export PATH
    [ "$(command -v zpool)" = "$Z/zpool" ] || die "zpool is not $Z/zpool"
}

no_other_pool() {
    local o
    o=$(zpool list -H -o name 2>&1 | grep -vx "$POOL")
    [ -z "$o" ] || die "a pool this task did not create is imported: [$o]"
}

setp() {
    local v
    [ -e "$P/$1" ] || die "$Z has no $1"
    echo "$2" > "$P/$1" || die "$1=$2"
    v=$(cat "$P/$1")
    [ "$v" = "$2" ] || die "$1 reads $v, not $2"
}

load_tree() {
    local want got
    use_tree "$1"
    if lsmod | grep -q '^zfs '; then
        "$Z/scripts/zfs.sh" -u || die "unload"
    fi
    "$Z/scripts/zfs.sh" || die "load $1"
    want=$(modinfo -F srcversion "$Z/module/zfs.ko")
    got=$(cat "$R/sys/module/zfs/srcversion")
    if [ -z "$want" ] || [ "$want" != "$got" ]; then
        die "srcversion $got loaded, $1 built $want"
    fi
    setp zfs_arc_max $ARC_MAX
    setp zfs_txg_history $TXG_HIST
    setp zfs_trim_txg_batch $TXG_BATCH
    no_other_pool
    echo "$1" > "$T/cur-tree"
}

pval() {
    if [ -e "$P/$1" ]; then
        echo "$1=$(cat "$P/$1")"
    else
        echo "$1=absent"
    fi
}

memavail() { awk '/^MemAvailable/ {print int($2 / 1024)}' "$R/proc/meminfo"; }

wbtval() {  # wbtval DEV FILE: a value of blk-wbt's debugfs state, or "-"
    cat "$DBG/$1/rqos/wbt/$2" 2>/dev/null || echo -
}

devinfo() {
    local n q
    for n in $NBS; do
        q=$SB/$n/queue
        echo "$n dev $(cat "$SB/$n/dev") sched $(cat "$q/scheduler")" \
            "rot $(cat "$q/rotational")" \
            "wbt_lat_usec $(cat "$q/wbt_lat_usec")" \
            "nr_requests $(cat "$q/nr_requests")" \
            "discard_max_bytes $(cat "$q/discard_max_bytes")" \
            "wb_background $(wbtval "$n" wb_background)" \
            "wb_normal $(wbtval "$n" wb_normal)" \
            "wbt_enabled $(wbtval "$n" enabled)" \
            "completion_nsec $(cat "$CFS/$n/completion_nsec")" \
            "hw_queue_depth $(cat "$CFS/$n/hw_queue_depth")" \
            "discard $(cat "$CFS/$n/discard")"
    done
}

trace_root() {
    local d
    for d in "$R/sys/kernel/tracing" "$R/sys/kernel/debug/tracing"; do
        if [ -d "$d/events/block/block_bio_queue" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

trace_inst() {
    local tr
    tr=$(trace_root) || die "no block tracepoints in tracefs"
    TI=$tr/instances/wbp4
}

trace_enable() {    # trace_enable 0|1
    local e
    for e in $EVENTS; do
        echo "$1" > "$TI/events/block/$e/enable" || die "trace $e $1"
    done
}

devs() {
    local n s=""
    for n in $NBS; do
        s="$s $R/dev/$n"
    done
    echo "$s"
}

setup() {
    local n q v devf e
    mkdir -p "$T" "$L" "$MNT" || die "mkdir"
    echo "task-1120-wbp4: start" > "$R/dev/kmsg"
    for n in $NBS; do
        [ -e "$R/dev/$n" ] && die "$R/dev/$n exists"
    done
    modprobe null_blk nr_devices=0 || die "null_blk is not available"
    for n in $NBS; do
        v=$CFS/$n
        mkdir "$v" || die "null_blk $n"
        if ! { echo $NB_SIZE > "$v/size" &&
            echo 4096 > "$v/blocksize" &&
            echo 1 > "$v/memory_backed" &&
            echo 1 > "$v/discard" &&
            echo 2 > "$v/irqmode" &&
            echo $NB_LAT > "$v/completion_nsec" &&
            echo 1 > "$v/submit_queues" &&
            echo $NB_QD > "$v/hw_queue_depth" &&
            echo 1 > "$v/power"; }; then
            die "null_blk $n settings"
        fi
    done
    udevadm settle
    devf=""
    for n in $NBS; do
        [ -b "$R/dev/$n" ] || die "no $R/dev/$n"
        q=$SB/$n/queue
        if ! grep -q '\[mq-deadline\]' "$q/scheduler"; then
            echo mq-deadline > "$q/scheduler" || die "$n scheduler"
        fi
        grep -q '\[mq-deadline\]' "$q/scheduler" || die "$n not mq-deadline"
        echo 1 > "$q/rotational" || die "$n rotational"
        [ "$(cat "$q/rotational")" = 1 ] || die "$n not rotational"
        # The rotational default, written: the device registered as
        # non-rotational, with blk-wbt's 2 ms target.
        echo 75000 > "$q/wbt_lat_usec" || die "$n wbt_lat_usec"
        v=$(cat "$q/wbt_lat_usec")
        [ "$v" = 75000 ] || die "$n wbt_lat_usec reads $v, not 75000"
        v=$(cat "$q/discard_max_bytes")
        [ "$v" -gt 0 ] || die "$n has no discard"
        v=$(wbtval "$n" wb_background)
        [ "$v" = - ] || [ "$v" = 1 ] ||
            die "$n blk-wbt lets $v discards through, not 1"
        e=$(cat "$SB/$n/dev")
        devf="$devf${devf:+ || }dev == $(echo "$e" | awk -F: '{print $1 * 1048576 + $2}')"
    done
    devinfo > "$L/setup-devices.txt"

    if [ -e "$SCHED" ]; then
        cat "$SCHED" > "$T/schedstats.orig"
        echo 1 > "$SCHED" || die "sched_schedstats"
    fi
    trace_inst
    mkdir -p "$TI" || die "trace instance $TI"
    echo mono > "$TI/trace_clock" || die "trace_clock"
    echo $TRACE_KB > "$TI/buffer_size_kb" || die "buffer_size_kb"
    for e in $EVENTS; do
        if ! echo "($devf) && rwbs ~ \"*D*\"" \
            > "$TI/events/block/$e/filter" 2>/dev/null; then
            echo "($devf)" > "$TI/events/block/$e/filter" ||
                die "trace filter $e"
        fi
    done
    {
        echo "schedstats $(cat "$T/schedstats.orig" 2>/dev/null || echo absent) -> $(cat "$SCHED" 2>/dev/null || echo absent)"
        echo "trace $TI clock [$(cat "$TI/trace_clock")] kb $(cat "$TI/buffer_size_kb")"
        for e in $EVENTS; do
            echo "filter $e [$(cat "$TI/events/block/$e/filter")]"
        done
    } > "$L/setup-trace.txt"

    load_tree "$T/M"
    [ -z "$(zpool list -H -o name 2>&1)" ] || die "a pool exists before setup"
    # shellcheck disable=SC2046
    zpool create -f -o ashift=12 -o cachefile=none -o autotrim=off \
        -O atime=off -O compression=off -O mountpoint="$MNT" \
        $POOL $(devs) || die "zpool create"
    zfs create -o recordsize=64K $POOL/frag || die "zfs create frag"
    zfs create -o recordsize=64K $POOL/churn || die "zfs create churn"
    zfs create -o recordsize=8K $POOL/sync || die "zfs create sync"
    python3 -I "$H/wbp4-load.py" frag "$MNT/frag" $FRAG_N $FRAG_BS ||
        die "frag"
    zpool sync $POOL || die "sync"
    python3 -I "$H/wbp4-load.py" unfrag "$MNT/frag" > "$L/setup-unfrag.txt" ||
        die "unfrag"
    dd if=/dev/urandom of="$MNT/sync/f" bs=1M count=64 status=none ||
        die "prefill sync"
    zpool sync $POOL || die "sync"
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zfs get -r recordsize,compression,mountpoint,used $POOL \
        > "$L/setup-props.txt"
    zpool get -H -o name,property,value scheduler $POOL all-vdevs \
        > "$L/setup-vdev-scheduler.txt" 2>&1
    log "setup done: $(zpool list -Ho size,alloc,cap $POOL)," \
        "$(cat "$L/setup-unfrag.txt"), MemAvailable $(memavail) MiB"
}

arm_set() {
    local tree cur d imp=""
    tree=$(arm_tree "$1")
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    use_tree "${cur:-$tree}"
    if zpool list -H $POOL > /dev/null 2>&1; then
        zpool export $POOL || die "export"
    fi
    no_other_pool
    if [ "$cur" != "$tree" ]; then
        load_tree "$tree"
    fi
    for d in $(devs); do
        imp="$imp -d $d"
    done
    # shellcheck disable=SC2086
    zpool import -o cachefile=none $imp $POOL || die "import"
    no_other_pool
    zpool set autotrim=off $POOL || die "autotrim=off"
    for d in frag churn sync; do
        mountpoint -q "$MNT/$d" || die "$MNT/$d is not mounted"
    done
}

layout() {  # layout DIR ARM ROW
    local v
    {
        echo "arm $2 tree $Z row $3"
        echo "srcversion $(cat "$R/sys/module/zfs/srcversion")"
        for v in zfs_trim_txg_batch zfs_vdev_trim_max_active \
            zfs_vdev_trim_min_active zfs_trim_queue_limit \
            zfs_trim_extent_bytes_min zfs_trim_extent_bytes_max \
            zfs_arc_max zfs_txg_timeout; do
            pval $v
        done
        echo "autotrim $(zpool get -H -o value autotrim $POOL)"
        echo "memavail $(memavail)"
        devinfo
    } > "$1/layout.txt"
}

snap() {    # snap DIR N
    local n
    {
        echo "time $(date +%s.%N)"
        head -1 "$R/proc/stat"
        echo "loadavg $(cat "$R/proc/loadavg")"
        for n in $NBS; do
            awk -v d="$n" '$3 == d {print "disk", $0}' "$R/proc/diskstats"
        done
    } > "$1/snap$2.txt"
    python3 -I "$H/wbp4-load.py" sched "$2" >> "$1/sched.txt"
}

kcheck() {
    if dmesg | sed -n '/task-1120-wbp4: start/,$p' |
        grep -E 'hung_task|blocked for more than|BUG:|Oops|VERIFY|PANIC|Call Trace|WARNING:' \
        > "$1"; then
        die "kernel log, see $1"
    fi
    rm -f "$1"
}

trace_out() {   # trace_out DIR
    cat "$TI/trace" > "$1/trace.txt"
    python3 -I "$H/wbp4-report.py" trace2csv < "$1/trace.txt" \
        > "$1/discards.csv" || die "trace2csv $1"
    [ "${KEEP_TRACE:-0}" = 1 ] || rm -f "$1/trace.txt"
}

run_mt() {  # run_mt PASS ARM
    local dir=$L/raw/$1/$2/mt ma spid wpid rc w0 w1 capped=no
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2 mt"
    mkdir -p "$dir" || die "mkdir $dir"
    layout "$dir" "$2" mt
    trace_enable 0
    : > "$TI/trace"
    rm -f "$dir/stop"
    python3 -I "$H/wbp4-load.py" sample "$dir/samples.txt" "$dir/stop" 0.02 &
    spid=$!
    python3 -I "$H/wbp4-load.py" syncw "$MNT/sync/f" \
        $((RAMP_MT + TRIM_MAX + 15)) "$dir/syncw.txt" $SYNC_RATE $SYNC_BS &
    wpid=$!
    sleep "$RAMP_MT"
    snap "$dir" 0
    trace_enable 1
    w0=$(date +%s.%N)
    timeout "$TRIM_MAX" zpool trim -w $POOL
    rc=$?
    w1=$(date +%s.%N)
    if [ $rc -eq 124 ]; then
        capped=yes
        zpool trim -c $POOL
    elif [ $rc -ne 0 ]; then
        die "zpool trim rc $rc"
    fi
    sleep 1
    trace_enable 0
    snap "$dir" 1
    kill -TERM $wpid
    wait $wpid || die "sync writer failed in $dir"
    touch "$dir/stop"
    wait $spid
    echo "window $w0 $w1 rc $rc capped $capped" >> "$dir/layout.txt"
    zpool status -t $POOL > "$dir/status.txt" 2>&1
    trace_out "$dir"
    kcheck "$dir/kernel.txt"
    log "run $1 $2 mt done: $(python3 -I "$H/wbp4-report.py" brief "$dir")"
}

run_at() {  # run_at PASS ARM
    local dir=$L/raw/$1/$2/at ma spid wpid cpid rc=0 w0 w1
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2 at"
    mkdir -p "$dir" || die "mkdir $dir"
    zpool set autotrim=on $POOL || die "autotrim=on"
    layout "$dir" "$2" at
    trace_enable 0
    : > "$TI/trace"
    rm -f "$dir/stop"
    python3 -I "$H/wbp4-load.py" sample "$dir/samples.txt" "$dir/stop" 0.02 &
    spid=$!
    python3 -I "$H/wbp4-load.py" syncw "$MNT/sync/f" \
        $((RAMP_AT + MEAS_AT + 1)) "$dir/syncw.txt" $SYNC_RATE $SYNC_BS &
    wpid=$!
    python3 -I "$H/wbp4-load.py" churn "$MNT/churn" \
        $((RAMP_AT + MEAS_AT + 1)) "$dir/churn.txt" $CHURN_RATE $CHURN_BS \
        $CHURN_KEEP &
    cpid=$!
    sleep "$RAMP_AT"
    snap "$dir" 0
    trace_enable 1
    w0=$(date +%s.%N)
    sleep "$MEAS_AT"
    w1=$(date +%s.%N)
    trace_enable 0
    snap "$dir" 1
    wait $wpid || rc=$?
    wait $cpid || rc=$?
    touch "$dir/stop"
    wait $spid
    zpool set autotrim=off $POOL || die "autotrim=off"
    echo "window $w0 $w1 rc 0 capped no" >> "$dir/layout.txt"
    trace_out "$dir"
    rm -rf "$MNT/churn/d" || die "rm churn"
    zpool sync $POOL || die "sync after $1 $2 at"
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir"
    log "run $1 $2 at done: $(python3 -I "$H/wbp4-report.py" brief "$dir")"
}

pass() {    # pass NAME "ARMS"
    local a
    for a in $2; do arm_tree "$a" > /dev/null; done
    trace_inst
    mkdir -p "$L/raw/$1" || die "mkdir"
    echo "arms $2" > "$L/raw/$1/order.txt"
    log "pass $1 start (arms $2)"
    for a in $2; do
        arm_set "$a"
        run_mt "$1" "$a"
        run_at "$1" "$a"
        no_other_pool
    done
    log "pass $1 done"
}

flags() {
    local a
    RAMP_AT=5
    MEAS_AT=15
    KEEP_TRACE=1
    trace_inst
    log "flags start"
    for a in M T; do
        arm_set "$a"
        run_mt F "$a"
        run_at F "$a"
    done
    python3 -I "$H/wbp4-report.py" flags "$L/raw/F" > "$L/flags.txt" ||
        die "flags report"
    log "flags done"
}

quiet() {
    local a b l0 l1
    a=$(head -1 "$R/proc/stat")
    l0=$(cut -d' ' -f1 "$R/proc/loadavg")
    sleep "$1"
    b=$(head -1 "$R/proc/stat")
    l1=$(cut -d' ' -f1 "$R/proc/loadavg")
    log "quiet $1 s: $(echo "$a" "$b" | awk '{
        n = (NF - 2) / 2
        for (i = 1; i <= 8; i++) { d[i] = $(i + n + 2) - $(i + 1); t += d[i] }
        if (t == 0) t = 1
        printf "idle %.2f %% iowait %.2f %% steal %.2f %%", \
            100 * d[4] / t, 100 * d[5] / t, 100 * d[8] / t }')" \
        "load1 $l0 -> $l1"
}

teardown() {
    local cur n tr e
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    if [ -n "$cur" ]; then
        use_tree "$cur"
        zpool list -H $POOL > /dev/null 2>&1 && zpool destroy $POOL
        "$Z/scripts/zfs.sh" -u
    fi
    if tr=$(trace_root) && [ -d "$tr/instances/wbp4" ]; then
        for e in $EVENTS; do
            echo 0 > "$tr/instances/wbp4/events/block/$e/enable"
        done
        rmdir "$tr/instances/wbp4"
    fi
    if [ -e "$T/schedstats.orig" ] && [ -e "$SCHED" ]; then
        cat "$T/schedstats.orig" > "$SCHED"
    fi
    for n in $NBS; do
        if [ -d "$CFS/$n" ]; then
            echo 0 > "$CFS/$n/power"
            rmdir "$CFS/$n"
        fi
    done
    modprobe -r null_blk
    rm -f "$T/cur-tree"
    log "teardown done: schedstats $(cat "$SCHED" 2>/dev/null || echo absent)"
}

mkdir -p "$L" || exit 1
case ${1:-} in
setup) setup ;;
flags) [ -f "$T/cur-tree" ] || die "no setup"
    flags ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    quiet "$2" ;;
pass) [ $# -eq 3 ] || die "usage: pass NAME ARMS"
    [ -f "$T/cur-tree" ] || die "no setup"
    pass "$2" "$3" ;;
teardown) teardown ;;
*) die "usage: setup | flags | quiet SECONDS | pass NAME ARMS | teardown" ;;
esac
