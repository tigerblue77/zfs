#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# TASK-1121-WBP2V2 driver: PR2 v2 (zfs_vdev_disk_rt_mask: synchronous
# reads and writes submitted in the real-time I/O priority class, except
# from a thread that set its own priority) against upstream master, on
# Linux 6.12 with mq-deadline, under ionice and cgroup controls.  Two trees:
# M = a1daebfec3f (upstream master), S = M plus the PR2 v2 commit.  Three
# arms: M; S1 = S with zfs_vdev_disk_rt_mask=1 (synchronous reads); S3 = S
# with zfs_vdev_disk_rt_mask=3 (synchronous reads and writes).
#
# One memory-backed null_blk device made to look like a slow disk (8 ms per
# request by timer, 4 tags, mq-deadline with 64 requests, marked
# rotational, blk-wbt off), a pool on it (cachefile=none, mounted under
# $T/mnt), and two cgroups under $CG/wbp2: "vic" and "agg".  Each scenario
# runs four streams for RAMP + MEAS seconds: in vic, a paced reader of 128K
# records and a paced writer of 128K records with an fdatasync() after
# every two; in agg, a closed-loop reader of 128K records with 8 threads
# and a paced writer of 16K into 128K records (each needing a read of its
# record first).  Every file is in a dataset with primarycache=metadata.
#
# Scenarios: a  agg's two processes under "ionice -c3"
#            b  agg's io.prio.class "idle"
#            c  agg's io.max "riops=40 wiops=40" on the device
#            d  iocost on the device (a 150 IOPS, 20 MiB/s linear model,
#               vrate fixed at 100 %), agg io.weight 1, vic 10000
#               (only when the kernel has iocost)
#            e  no control at all
# During the MEAS seconds the requests inserted into the device's queue are
# traced (block_rq_insert: rwbs, ioprio, inserting task) in a tracefs
# instance and summed by task, kind and ioprio; io.stat of vic and agg, the
# txgs kstat, diskstats and /proc/stat are taken at the window's ends.
#
#   wbp2-bench.sh setup               device, cgroups, trace instance, pool
#   wbp2-bench.sh flags               every scenario, 8 s, on every arm
#   wbp2-bench.sh quiet SECONDS       idle, iowait, steal and load1
#   wbp2-bench.sh pass NAME ARMS      every scenario on each arm of ARMS
#   wbp2-bench.sh teardown            pool, modules, cgroups, device removed
#
# WBP2_TEST_ROOT prefixes every system path, for the kit's own tests; sudo
# drops it from the environment.
set -u
export LC_ALL=C

R=${WBP2_TEST_ROOT:-}
T=/var/tmp/task-1121-wbp2v2
L=/var/log/task-1121-wbp2v2
if [ -n "$R" ]; then
    T=$R$T
    L=$R$L
fi
H=$(dirname "$(readlink -f "$0")")
P=$R/sys/module/zfs/parameters
CFS=$R/sys/kernel/config/nullb
SB=$R/sys/block
CG=$R/sys/fs/cgroup
CGW=$CG/wbp2
PS=$R/proc/spl/kstat/zfs
POOL=wbp2p
NB=wbp2nb0
MNT=$T/mnt
NB_SIZE=1536        # MiB
NB_LAT=8000000      # ns per request
NB_QD=4
NR_REQ=64
RAMP=${RAMP:-5}
MEAS=${MEAS:-30}
BS=131072
VR_RATE=40
VW_RATE=20
VW_EVERY=2
AR_THREADS=8
AW_RATE=100
AW_BS=16384
IOMAX="riops=40 wiops=40"
COST_MODEL="ctrl=user model=linear rbps=20971520 rseqiops=150 rrandiops=150 wbps=20971520 wseqiops=150 wrandiops=150"
COST_QOS="enable=1 ctrl=user rpct=95.00 rlat=50000 wpct=95.00 wlat=50000 min=100.00 max=100.00"
ARC_MAX=1073741824
TXG_HIST=100
TRACE_KB=4096
MIN_AVAIL=1024      # MiB of MemAvailable required before each run
PATH_BASE=$PATH
Z=""
TI=""

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
die() { echo "wbp2-bench: $*" >&2; log "STOP: $*"; exit 1; }

ARMS="M S1 S3"
RT=zfs_vdev_disk_rt_mask

arm_tree() {
    case $1 in
    M) echo "$T/M" ;;
    S1|S3) echo "$T/S" ;;
    *) die "unknown arm $1" ;;
    esac
}

# arm_mask ARM: the zfs_vdev_disk_rt_mask the arm writes, "-" for M, whose
# tree has no such parameter.
arm_mask() {
    case $1 in
    M) echo - ;;
    S1) echo 1 ;;
    S3) echo 3 ;;
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

# pools_other: the imported pools other than ours, one per line.
pools_other() {
    zpool list -H -o name 2>&1 | grep -vx "$POOL"
}

no_other_pool() {
    local o
    o=$(pools_other)
    [ -z "$o" ] || die "a pool this task did not create is imported: [$o]"
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
    echo $ARC_MAX > "$P/zfs_arc_max" || die "zfs_arc_max"
    echo $TXG_HIST > "$P/zfs_txg_history" || die "zfs_txg_history"
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

majmin() { cat "$SB/$NB/dev"; }

devinfo() {
    local q=$SB/$NB/queue
    echo "$NB dev $(majmin) sched $(cat "$q/scheduler")" \
        "rot $(cat "$q/rotational")" \
        "wbt_lat_usec $(cat "$q/wbt_lat_usec")" \
        "nr_requests $(cat "$q/nr_requests")" \
        "write_cache $(tr ' ' _ < "$q/write_cache")" \
        "max_sectors_kb $(cat "$q/max_sectors_kb")" \
        "completion_nsec $(cat "$CFS/$NB/completion_nsec")" \
        "hw_queue_depth $(cat "$CFS/$NB/hw_queue_depth")" \
        "irqmode $(cat "$CFS/$NB/irqmode")" \
        "memory_backed $(cat "$CFS/$NB/memory_backed")"
}

trace_root() {
    local d
    for d in "$R/sys/kernel/tracing" "$R/sys/kernel/debug/tracing"; do
        if [ -d "$d/events/block/block_rq_insert" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

trace_inst() {
    local tr
    tr=$(trace_root) || die "no block tracepoints in tracefs"
    TI=$tr/instances/wbp2
}

cg_files() {
    local f
    for f in io.max io.stat io.weight io.prio.class; do
        [ -e "$CGW/agg/$f" ] && printf '%s ' "$f"
    done
    for f in io.cost.qos io.cost.model; do
        [ -e "$CG/$f" ] && printf 'root:%s ' "$f"
    done
    echo
}

scenarios() {
    local s="a"
    [ -e "$CGW/agg/io.prio.class" ] && s="$s b"
    [ -e "$CGW/agg/io.max" ] && s="$s c"
    if [ -e "$CG/io.cost.qos" ] && [ -e "$CG/io.cost.model" ] &&
        [ -e "$CGW/agg/io.weight" ]; then
        s="$s d"
    fi
    echo "$s e"
}

setup() {
    local q v devt
    mkdir -p "$T" "$L" "$MNT" || die "mkdir"
    echo "task-1121-wbp2v2: start" > "$R/dev/kmsg"
    [ -e "$R/dev/$NB" ] && die "$R/dev/$NB exists"
    [ -d "$CGW" ] && die "$CGW exists"
    command -v ionice > /dev/null || die "no ionice"
    modprobe null_blk nr_devices=0 || die "null_blk is not available"
    v=$CFS/$NB
    mkdir "$v" || die "null_blk $NB"
    if ! { echo $NB_SIZE > "$v/size" &&
        echo 4096 > "$v/blocksize" &&
        echo 1 > "$v/memory_backed" &&
        echo 2 > "$v/irqmode" &&
        echo $NB_LAT > "$v/completion_nsec" &&
        echo 1 > "$v/submit_queues" &&
        echo $NB_QD > "$v/hw_queue_depth" &&
        echo 1 > "$v/power"; }; then
        die "null_blk $NB settings"
    fi
    udevadm settle
    [ -b "$R/dev/$NB" ] || die "no $R/dev/$NB"
    q=$SB/$NB/queue
    if ! grep -q '\[mq-deadline\]' "$q/scheduler"; then
        echo mq-deadline > "$q/scheduler" || die "scheduler"
    fi
    grep -q '\[mq-deadline\]' "$q/scheduler" || die "not mq-deadline"
    echo 1 > "$q/rotational" || die "rotational"
    [ "$(cat "$q/rotational")" = 1 ] || die "not rotational"
    echo $NR_REQ > "$q/nr_requests" || die "nr_requests"
    [ "$(cat "$q/nr_requests")" = $NR_REQ ] || die "nr_requests reads $(cat "$q/nr_requests")"
    echo 0 > "$q/wbt_lat_usec" || die "wbt_lat_usec"
    [ "$(cat "$q/wbt_lat_usec")" = 0 ] || die "wbt_lat_usec reads $(cat "$q/wbt_lat_usec")"
    devinfo > "$L/setup-devices.txt"

    grep -qw io "$CG/cgroup.controllers" || die "no io controller in $CG"
    {
        echo "root subtree_control before: $(cat "$CG/cgroup.subtree_control")"
        if ! grep -qw io "$CG/cgroup.subtree_control"; then
            echo +io > "$CG/cgroup.subtree_control" || die "+io at the root"
            echo "root +io written by the kit"
        fi
    } > "$L/setup-cgroup.txt"
    mkdir "$CGW" || die "mkdir $CGW"
    echo +io > "$CGW/cgroup.subtree_control" || die "+io in $CGW"
    mkdir "$CGW/vic" "$CGW/agg" || die "mkdir vic agg"
    {
        echo "files: $(cg_files)"
        echo "scenarios: $(scenarios)"
    } >> "$L/setup-cgroup.txt"
    scenarios > "$T/scenarios"

    trace_inst
    mkdir -p "$TI" || die "trace instance $TI"
    echo mono > "$TI/trace_clock" || die "trace_clock"
    echo $TRACE_KB > "$TI/buffer_size_kb" || die "buffer_size_kb"
    devt=$(majmin | awk -F: '{print $1 * 1048576 + $2}')
    echo "dev == $devt" > "$TI/events/block/block_rq_insert/filter" ||
        die "trace filter"
    echo "trace $TI filter [$(cat "$TI/events/block/block_rq_insert/filter")]" \
        "clock [$(cat "$TI/trace_clock")] kb $(cat "$TI/buffer_size_kb")" \
        >> "$L/setup-cgroup.txt"

    load_tree "$T/M"
    [ -z "$(zpool list -H -o name 2>&1)" ] || die "a pool exists before setup"
    zpool create -f -o ashift=12 -o cachefile=none -O atime=off \
        -O compression=off -O recordsize=128K -O primarycache=metadata \
        -O mountpoint="$MNT" $POOL "$R/dev/$NB" || die "zpool create"
    zfs create $POOL/v || die "zfs create $POOL/v"
    zfs create $POOL/a || die "zfs create $POOL/a"
    for v in v/r:256 v/w:128 a/r:256 a/w:128; do
        dd if=/dev/urandom of="$MNT/${v%:*}" bs=1M count="${v#*:}" \
            status=none || die "prefill ${v%:*}"
    done
    zpool sync $POOL || die "sync"
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zfs get -r recordsize,primarycache,compression,mountpoint $POOL \
        > "$L/setup-props.txt"
    log "setup done: $(zpool list -Ho size,alloc,cap $POOL)," \
        "MemAvailable $(memavail) MiB, scenarios $(cat "$T/scenarios")"
}

# arm_set ARM: export, load ARM's tree if another is loaded, set and read
# back the arm's zfs_vdev_disk_rt_mask, import our pool from our device
# only.
arm_set() {
    local tree cur mask
    tree=$(arm_tree "$1")
    mask=$(arm_mask "$1")
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    use_tree "${cur:-$tree}"
    if zpool list -H $POOL > /dev/null 2>&1; then
        zpool export $POOL || die "export"
    fi
    no_other_pool
    if [ "$cur" != "$tree" ]; then
        load_tree "$tree"
    fi
    if [ "$mask" = - ]; then
        [ -e "$P/$RT" ] && die "$RT exists on arm $1's tree"
    else
        echo "$mask" > "$P/$RT" || die "$RT=$mask"
        [ "$(cat "$P/$RT")" = "$mask" ] ||
            die "$RT reads $(cat "$P/$RT"), arm $1 wants $mask"
    fi
    zpool import -o cachefile=none -d "$R/dev/$NB" $POOL || die "import"
    no_other_pool
    for v in v a; do
        mountpoint -q "$MNT/$v" || die "$MNT/$v is not mounted"
    done
}

# cg_set SC: the scenario's cgroup settings, read back.
cg_set() {
    local mm v
    mm=$(majmin)
    case $1 in
    a|e) ;;
    b) echo idle > "$CGW/agg/io.prio.class" || die "io.prio.class"
        v=$(cat "$CGW/agg/io.prio.class")
        [ "$v" = idle ] || die "io.prio.class reads $v" ;;
    c) echo "$mm $IOMAX" > "$CGW/agg/io.max" || die "io.max"
        grep -q "^$mm .*riops=40 wiops=40" "$CGW/agg/io.max" ||
            die "io.max reads $(cat "$CGW/agg/io.max")" ;;
    d) echo "$mm $COST_MODEL" > "$CG/io.cost.model" || die "io.cost.model"
        echo "$mm $COST_QOS" > "$CG/io.cost.qos" || die "io.cost.qos"
        grep -q "^$mm enable=1" "$CG/io.cost.qos" ||
            die "io.cost.qos reads $(cat "$CG/io.cost.qos")"
        echo "default 1" > "$CGW/agg/io.weight" || die "agg io.weight"
        echo "default 10000" > "$CGW/vic/io.weight" || die "vic io.weight" ;;
    *) die "unknown scenario $1" ;;
    esac
}

cg_clear() {
    local mm
    mm=$(majmin)
    case $1 in
    a|e) ;;
    b) echo no-change > "$CGW/agg/io.prio.class" ;;
    c) echo "$mm rbps=max wbps=max riops=max wiops=max" > "$CGW/agg/io.max" ;;
    d) echo "$mm enable=0" > "$CG/io.cost.qos"
        echo "default 100" > "$CGW/agg/io.weight"
        echo "default 100" > "$CGW/vic/io.weight" ;;
    esac
    [ "$(cat "$SB/$NB/queue/wbt_lat_usec")" = 0 ] ||
        die "wbt_lat_usec reads $(cat "$SB/$NB/queue/wbt_lat_usec") after $1"
}

cg_show() {
    local mm f
    mm=$(majmin)
    for f in io.prio.class io.max io.weight; do
        [ -e "$CGW/agg/$f" ] &&
            echo "agg $f $(grep -v '^$' "$CGW/agg/$f" | tr '\n' ' ')"
    done
    [ -e "$CGW/vic/io.weight" ] &&
        echo "vic io.weight $(tr '\n' ' ' < "$CGW/vic/io.weight")"
    for f in io.cost.qos io.cost.model; do
        [ -e "$CG/$f" ] && echo "root $f $(grep "^$mm " "$CG/$f")"
    done
}

# snap DIR N: the counters at the start (0) or the end (1) of the window.
snap() {
    local mm g
    mm=$(majmin)
    {
        echo "time $(date +%s.%N)"
        head -1 "$R/proc/stat"
        echo "loadavg $(cat "$R/proc/loadavg")"
        awk -v d=$NB '$3 == d {print "disk", $0}' "$R/proc/diskstats"
        for g in vic agg; do
            echo "iostat $g $(grep "^$mm " "$CGW/$g/io.stat" || echo -)"
        done
    } > "$1/snap$2.txt"
    cp "$PS/$POOL/txgs" "$1/txgs$2.txt" 2> /dev/null
}

kcheck() {
    if dmesg | sed -n '/task-1121-wbp2v2: start/,$p' |
        grep -E 'hung_task|blocked for more than|BUG:|Oops|VERIFY|PANIC|Call Trace|WARNING:|percpu ref' \
        > "$1"; then
        die "kernel log, see $1"
    fi
    rm -f "$1"
}

# cgexec GROUP CMD... &: CMD in cgroup GROUP of $CGW, as the background
# job itself, so that $! is CMD's pid.  Only ever run in the background.
cgexec() {
    local g=$1
    shift
    exec sh -c 'echo $$ > "$0/cgroup.procs" && exec "$@"' "$CGW/$g" "$@"
}

# loads DIR SC SECONDS: the four streams, PIDS set.
loads() {
    local pre=""
    [ "$2" = a ] && pre="ionice -c3"
    cgexec vic python3 -I "$H/wbp2-load.py" reader "$MNT/v/r" "$3" \
        "$1/vr.txt" wbp2-vr $VR_RATE $BS 1 &
    PIDS=$!
    cgexec vic python3 -I "$H/wbp2-load.py" writer "$MNT/v/w" "$3" \
        "$1/vw.txt" wbp2-vw $VW_RATE $BS $VW_EVERY &
    PIDS="$PIDS $!"
    # shellcheck disable=SC2086
    cgexec agg $pre python3 -I "$H/wbp2-load.py" reader "$MNT/a/r" "$3" \
        "$1/ar.txt" wbp2-ar 0 $BS $AR_THREADS &
    PIDS="$PIDS $!"
    # shellcheck disable=SC2086
    cgexec agg $pre python3 -I "$H/wbp2-load.py" writer "$MNT/a/w" "$3" \
        "$1/aw.txt" wbp2-aw $AW_RATE $AW_BS 0 &
    PIDS="$PIDS $!"
}

procinfo() {
    local p
    for p in $PIDS; do
        echo "pid $p cgroup $(tr '\n' ' ' 2>&1 < "$R/proc/$p/cgroup")" \
            "ionice $(ionice -p "$p" 2>&1)"
    done
}

waitall() {
    local p rc=0
    for p in $PIDS; do
        wait "$p" || rc=$?
    done
    return $rc
}

# run_one PASS ARM SC: one scenario; KEEP_TRACE=1 keeps the raw trace.
run_one() {
    local dir=$L/raw/$1/$2/$3 rc ma ev
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2 $3"
    mkdir -p "$dir" || die "mkdir $dir"
    cg_set "$3"
    {
        echo "arm $2 tree $Z scenario $3"
        echo "srcversion $(cat "$R/sys/module/zfs/srcversion")"
        for v in zfs_arc_max zfs_txg_timeout zfs_dirty_data_max \
            zfs_vdev_sync_read_max_active zfs_vdev_async_write_max_active \
            zfs_vdev_max_active $RT; do
            pval "$v"
        done
        echo "memavail $ma"
        devinfo
        cg_show
        echo "window ramp $RAMP meas $MEAS"
    } > "$dir/layout.txt"
    ev=$TI/events/block/block_rq_insert/enable
    echo 0 > "$ev"
    : > "$TI/trace"
    loads "$dir" "$3" $((RAMP + MEAS + 1))
    sleep 1
    procinfo >> "$dir/layout.txt"
    sleep $((RAMP - 1))
    snap "$dir" 0
    echo 1 > "$ev"
    sleep "$MEAS"
    echo 0 > "$ev"
    snap "$dir" 1
    waitall
    rc=$?
    cg_clear "$3"
    cat "$TI/trace" > "$dir/trace.txt"
    python3 -I "$H/wbp2-report.py" tracesum < "$dir/trace.txt" \
        > "$dir/trace-sum.txt" || die "tracesum $dir"
    [ "${KEEP_TRACE:-0}" = 1 ] || rm -f "$dir/trace.txt"
    zpool sync $POOL || die "sync after $1 $2 $3"
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir"
    log "run $1 $2 $3 done: $(python3 -I "$H/wbp2-report.py" brief "$dir")"
}

pass() {    # pass NAME "ARMS"
    local a s
    for a in $2; do arm_mask "$a" > /dev/null; done
    trace_inst
    mkdir -p "$L/raw/$1" || die "mkdir"
    echo "arms $2 scenarios $(cat "$T/scenarios")" > "$L/raw/$1/order.txt"
    log "pass $1 start (arms $2)"
    for a in $2; do
        arm_set "$a"
        # shellcheck disable=SC2013
        for s in $(cat "$T/scenarios"); do
            run_one "$1" "$a" "$s"
        done
        no_other_pool
    done
    log "pass $1 done"
}

flags() {
    local a s
    RAMP=2
    MEAS=8
    KEEP_TRACE=1
    trace_inst
    log "flags start"
    for a in $ARMS; do
        arm_set "$a"
        # shellcheck disable=SC2013
        for s in $(cat "$T/scenarios"); do
            run_one F "$a" "$s"
        done
    done
    python3 -I "$H/wbp2-report.py" flags "$L/raw/F" > "$L/flags.txt" ||
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
    local cur g mm
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    if [ -n "$cur" ]; then
        use_tree "$cur"
        zpool list -H $POOL > /dev/null 2>&1 && zpool destroy $POOL
        "$Z/scripts/zfs.sh" -u
    fi
    if [ -e "$SB/$NB/dev" ] && [ -e "$CG/io.cost.qos" ]; then
        mm=$(majmin)
        grep -q "^$mm enable=1" "$CG/io.cost.qos" &&
            echo "$mm enable=0" > "$CG/io.cost.qos"
    fi
    for g in vic agg; do
        [ -d "$CGW/$g" ] && rmdir "$CGW/$g"
    done
    [ -d "$CGW" ] && rmdir "$CGW"
    if tr=$(trace_root); then
        if [ -d "$tr/instances/wbp2" ]; then
            echo 0 > "$tr/instances/wbp2/events/block/block_rq_insert/enable"
            rmdir "$tr/instances/wbp2"
        fi
    fi
    if [ -d "$CFS/$NB" ]; then
        echo 0 > "$CFS/$NB/power"
        rmdir "$CFS/$NB"
    fi
    modprobe -r null_blk
    rm -f "$T/cur-tree"
    log "teardown done: cgroup $([ -d "$CGW" ] && echo left || echo removed)," \
        "device $([ -e "$R/dev/$NB" ] && echo left || echo removed)"
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
