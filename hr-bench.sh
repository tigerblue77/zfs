#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# Host rig driver: the owner's DS_HDD_R6_1 (5 SATA HDDs in raidz2, no log
# device, scheduler none, blk-wbt at 75 ms, one 1M-volblocksize zvol under
# a Plex VM) rebuilt in a test VM.
#
# Five emulated disks served by hddsim (ublk): one actuator, NCQ with
# shortest-positioning-first reordering, a volatile write cache destaged
# in idle time, SATA flushes that wait for the queue and drain the cache.
# Memory-backed, pattern-compressed: 16 GiB each in a few hundred MiB.
# A raidz2 on them, filled to 66 % with holes, a sparse 1M zvol, and
# hr-load.py's guest: 50 MB/s of 512 KiB writes straddling the zvol's
# blocks, small writes, fsyncs about 0.6 per second, 7 reads per second.
#
#   hr-bench.sh disks TREE          hddsim built, the disks, TREE's modules
#   hr-bench.sh selftest            the model of each grid point, offline
#   hr-bench.sh probe CONF          raw service times of one disk, no pool
#   hr-bench.sh pool                pool, zvol and filler, fast disks
#   hr-bench.sh quiet SECONDS       idle, iowait, steal and load1
#   hr-bench.sh point PASS CONF     one measured run with grid point CONF
#   hr-bench.sh teardown            pool, modules, disks removed
#
# TREE is a built OpenZFS tree (its scripts/zfs.sh loads it).  CONF names
# grid/CONF.conf.  Each point writes RAW/PASS/CONF/: layout.txt, the
# guest's and the sampler's logs, zpool iostat streams, the counters at
# both ends of the measured window; the disks' own per-second statistics
# stay in $L/dev/ID.stats, read by wall time.
set -u
export LC_ALL=C

T=${HR_T:-/var/tmp/task-1121-hrig}
L=${HR_L:-/var/log/task-1121-hrig}
H=$(dirname "$(readlink -f "$0")")
P=/sys/module/zfs/parameters
POOL=hrp
IDS="70 71 72 73 74"
DEV_MIB=16384           # per disk
RAMP=${RAMP:-60}
MEAS=${MEAS:-300}
IVL=0.25                # sampler interval, seconds
WBT_LAT=75000           # usec, the host's
FILL_HI=88              # cap % reached by the filler before the holes
CAP_TARGET=66           # the host's cap %
FILE_MB=1               # filler file size: its holes set fragmentation
LIB_MB=8192
RING_MB=4096
DB_MB=256
JOURNAL_MB=64
MIN_AVAIL=1024          # MiB of MemAvailable required before each run
# Host-equivalent ZFS settings: the host's byte thresholds (4 GiB of dirty
# data: async writes at their minimum below 1.2 GiB, a txg forced at
# 820 MiB, no write delay seen) on a VM with 7.8 GB of RAM.
TUNE="zfs_arc_max=3221225472 zfs_dirty_data_max=1610612736
zfs_dirty_data_sync_percent=53
zfs_vdev_async_write_active_min_dirty_percent=80
zfs_vdev_async_write_active_max_dirty_percent=100
zfs_delay_min_dirty_percent=100 zfs_txg_history=200"
PATH_BASE=$PATH
Z=""

die() { echo "hr-bench: $*" >&2; log "STOP: $*"; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
memavail() { awk '/^MemAvailable/ {print int($2 / 1024)}' /proc/meminfo; }

use_tree() {
    Z=$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    export PATH="$Z:$PATH_BASE"
    command -v zpool | grep -qx "$Z/zpool" || die "zpool is not $Z/zpool"
}

load_tree() {
    local want got kv
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
    # shellcheck disable=SC2086
    for kv in $TUNE; do
        echo "${kv#*=}" > "$P/${kv%%=*}" || die "${kv%%=*}"
        [ "$(cat "$P/${kv%%=*}")" = "${kv#*=}" ] || die "${kv%%=*} readback"
    done
    echo "$1" > "$T/cur-tree"
}

pval() {
    if [ -e "$P/$1" ]; then echo "$1=$(cat "$P/$1")"; else echo "$1=absent"
    fi
}

# conf CONF: make CONF the disks' parameters, wait until each server
# logs exactly them (a "P" line in its statistics).
conf() {
    local f=$H/grid/$1.conf want id pid n got
    [ -r "$f" ] || die "no grid point $1"
    want=$("$T/hddsim" print "$f") || die "hddsim print $f"
    cp "$f" "$T/params.conf" || die "params"
    for id in $IDS; do
        pid=$(cat "$T/hddsim-$id.pid")
        kill -0 "$pid" 2> /dev/null || die "hddsim $id (pid $pid) is gone"
        kill -HUP "$pid" || die "SIGHUP $id"
    done
    for id in $IDS; do
        n=0
        while :; do
            got=$(grep '^P ' "$L/dev/$id.stats" | tail -1 | cut -d' ' -f3-)
            [ "$got" = "$want" ] && break
            n=$((n + 1))
            [ $n -le 50 ] || die "hddsim $id did not take $1: $got"
            sleep 0.1
        done
    done
    echo "$1" > "$T/cur-conf"
}

# set_wbt: the host's blk-wbt target, written 1 first so that blk-wbt
# starts each run again from its initial depth.
set_wbt() {
    local id q
    for id in $IDS; do
        q=/sys/block/ublkb$id/queue
        if ! echo 1 > "$q/wbt_lat_usec" ||
            ! echo $WBT_LAT > "$q/wbt_lat_usec"; then
            die "wbt_lat_usec on ublkb$id"
        fi
        [ "$(cat "$q/wbt_lat_usec")" = $WBT_LAT ] || die "ublkb$id wbt readback"
    done
}

devinfo() {
    local id q
    for id in $IDS; do
        q=/sys/block/ublkb$id/queue
        echo "ublkb$id dev $(cat "/sys/block/ublkb$id/dev")" \
            "size_mib $(( $(cat "/sys/block/ublkb$id/size") / 2048 ))" \
            "sched $(cat "$q/scheduler")" "rot $(cat "$q/rotational")" \
            "wbt_lat_usec $(cat "$q/wbt_lat_usec")" \
            "write_cache $(tr ' ' _ < "$q/write_cache")" \
            "nr_requests $(cat "$q/nr_requests")" \
            "max_sectors_kb $(cat "$q/max_sectors_kb")" \
            "lbs $(cat "$q/logical_block_size")" \
            "iostats $(cat "$q/iostats")" \
            "wbt_debugfs $([ -r "/sys/kernel/debug/block/ublkb$id/rqos/wbt/enabled" ] &&
                cat "/sys/kernel/debug/block/ublkb$id/rqos/wbt/enabled" || echo -)"
    done
}

build_sim() {
    gcc -O2 -Wall -pthread -o "$T/hddsim" "$H/hddsim.c" -lm \
        > "$L/hddsim-build.txt" 2>&1 || die "hddsim build, see $L/hddsim-build.txt"
    log "hddsim built: $(grep -c 'warning:' "$L/hddsim-build.txt") warnings"
}

disks_up() {
    local id q pid n
    mkdir -p "$L/dev" || die "mkdir"
    if lsmod | grep -q '^ublk_drv '; then
        echo no > "$T/ublk-loaded-by-us"
    else
        modprobe ublk_drv || die "ublk_drv is not available"
        echo yes > "$T/ublk-loaded-by-us"
    fi
    [ -c /dev/ublk-control ] || die "no /dev/ublk-control"
    cp "$H/grid/fast.conf" "$T/params.conf" || die "params"
    for id in $IDS; do
        [ -e "/dev/ublkb$id" ] && die "/dev/ublkb$id exists"
        setsid "$T/hddsim" run "$id" $DEV_MIB "$T/params.conf" \
            "$L/dev/$id.stats" > "$L/dev/$id.out" 2>&1 < /dev/null &
    done
    for id in $IDS; do
        n=0
        until grep -q ready "$L/dev/$id.out" 2> /dev/null &&
            [ -b "/dev/ublkb$id" ]; do
            n=$((n + 1))
            [ $n -le 300 ] || die "/dev/ublkb$id did not appear: $(cat "$L/dev/$id.out")"
            sleep 0.1
        done
        pid=$(awk '/ ready pid / {print $NF}' "$L/dev/$id.out")
        echo "$pid" > "$T/hddsim-$id.pid"
        echo -1000 > "/proc/$pid/oom_score_adj" || die "oom_score_adj $id"
        q=/sys/block/ublkb$id/queue
        echo none > "$q/scheduler" || die "ublkb$id scheduler"
        grep -q '\[none\]' "$q/scheduler" || die "ublkb$id not none"
        [ "$(cat "$q/rotational")" = 1 ] || die "ublkb$id not rotational"
        [ "$(cat "$q/write_cache")" = "write back" ] || die "ublkb$id write cache"
        echo 1 > "$q/iostats" || die "ublkb$id iostats"
    done
    set_wbt
    echo fast > "$T/cur-conf"
}

zvol_of() {
    local d
    if [ -b "/dev/zvol/$1" ]; then echo "/dev/zvol/$1"; return; fi
    for d in /dev/zd*; do
        case $d in *p*) continue ;; esac
        [ -b "$d" ] || continue
        if [ "$("$Z/udev/zvol_id" "$d" 2>/dev/null)" = "$1" ]; then
            echo "$d"; return
        fi
    done
}

zvol_wait() {
    local _
    for _ in $(seq 1 60); do
        [ -n "$(zvol_of "$POOL/vm")" ] && return 0
        udevadm settle
        sleep 1
    done
    die "zvol $POOL/vm did not appear"
}

cap() { zpool list -Hp -o cap $POOL; }

disks() {
    echo "task-1121-hrig: start" > /dev/kmsg
    build_sim
    disks_up
    load_tree "$1"
    {
        echo "kernel $(uname -r)"
        echo "ublk_drv $(modinfo -F vermagic ublk_drv | cut -d' ' -f1)"
        echo "hddsim.c $(sha256sum < "$H/hddsim.c" | cut -d' ' -f1)"
        echo "zfs srcversion $(cat /sys/module/zfs/srcversion) tree $1"
        devinfo
    } > "$L/setup-devices.txt"
    log "disks done: $(echo /dev/ublkb7[0-4])," \
        "MemAvailable $(memavail) MiB"
}

pool() {
    local id devs=() v n first i
    [ -f "$T/cur-tree" ] || die "no disks"
    use_tree "$(cat "$T/cur-tree")"
    [ "$(cat "$T/cur-conf")" = fast ] || die "pool: disks not fast"
    for id in $IDS; do devs+=("/dev/ublkb$id"); done
    zpool create -f -o ashift=12 -o cachefile=none -O atime=off \
        -O compression=off $POOL raidz2 "${devs[@]}" || die "zpool create"
    zfs create -s -V $((LIB_MB + RING_MB + DB_MB + JOURNAL_MB))M \
        -o volblocksize=1M $POOL/vm || die "$POOL/vm"
    zfs create -o recordsize=1M $POOL/fill || die "$POOL/fill"
    zvol_wait
    v=$(zvol_of $POOL/vm)
    python3 -I "$H/hr-load.py" zvolfill "$v" 0 \
        $((LIB_MB + RING_MB + DB_MB + JOURNAL_MB)) || die "zvol fill"
    log "zvol written: cap $(cap) %"
    # filler to FILL_HI, then every 4th file removed (holes), then every
    # 4th from 2, ... until the cap is down to CAP_TARGET
    n=0
    while [ "$(cap)" -lt $FILL_HI ]; do
        python3 -I "$H/hr-load.py" fill /$POOL/fill 1024 $FILE_MB $n ||
            die "fill"
        n=$((n + 1024))
        zpool sync $POOL
    done
    log "filled: $n files, cap $(cap) %"
    for first in 0 2 1 3; do
        [ "$(cap)" -le $CAP_TARGET ] && break
        i=$first
        while [ $i -lt $n ]; do
            rm -f "/$POOL/fill/$(printf 'f%06d' $i)"
            i=$((i + 4))
            if [ $((i % 4096)) -lt 4 ]; then
                zpool sync $POOL
                zpool sync $POOL
                [ "$(cap)" -le $CAP_TARGET ] && break
            fi
        done
        zpool sync $POOL
        zpool sync $POOL
    done
    zpool sync $POOL
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zpool list -Hp -o size,alloc,free,frag,cap $POOL >> "$L/setup-zpool-list.txt"
    zfs get -r volsize,volblocksize,recordsize,compression,sync,logbias \
        $POOL > "$L/setup-props.txt"
    for id in $IDS; do
        grep '^S ' "$L/dev/$id.stats" | tail -1 | awk -v i="$id" \
            '{for (k = 1; k <= NF; k++) if ($k == "full") print "ublkb" i, "full_pages", $(k + 1)}'
    done >> "$L/setup-zpool-list.txt"
    log "pool done: $(zpool list -Ho size,alloc,frag,cap $POOL)," \
        "MemAvailable $(memavail) MiB"
}

selftest() {
    local g
    for g in G1 G2 G3 G4 G5 G6; do
        "$T/hddsim" selftest "$H/grid/$g.conf" 1800 > "$L/selftest-$g.txt" ||
            die "selftest $g"
    done
    log "selftest done"
}

probe() {
    [ "$(cat "$T/cur-conf" 2> /dev/null)" = fast ] || die "probe before the pool only"
    zpool list -H $POOL > /dev/null 2>&1 && die "probe before the pool only"
    conf "$1"
    python3 -I "$H/hr-load.py" probe /dev/ublkb70 30 > "$L/probe-$1.txt" ||
        die "probe"
    conf fast
    log "probe $1 done: $(tr '\n' ';' < "$L/probe-$1.txt")"
}

# snap DIR N: the counters at the start (0) or the end (1) of the window.
snap() {
    local f
    {
        echo "time $(date +%s.%N)"
        head -1 /proc/stat
        echo "loadavg $(cat /proc/loadavg)"
        sed 's/^/psi_io /' /proc/pressure/io
        awk '$3 ~ /^ublkb7[0-4]$/ {print "disk", $0}' /proc/diskstats
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

stamp() {
    local l
    while IFS= read -r l; do printf '%s %s\n' "$EPOCHREALTIME" "$l"; done
}

kcheck() {
    if dmesg | sed -n '/task-1121-hrig: start/,$p' |
        grep -E 'hung_task|blocked for more than|BUG:|Oops|VERIFY|PANIC|Call Trace' \
        > "$1"; then
        die "kernel log, see $1"
    fi
    rm -f "$1"
}

point() {   # point PASS CONF
    local dir=$L/raw/$1/$2 ma v pids=() p rc=0 ipid wpid id devs=()
    [ -f "$T/cur-tree" ] || die "no setup"
    use_tree "$(cat "$T/cur-tree")"
    zpool list -H $POOL > /dev/null 2>&1 || die "pool $POOL not imported"
    [ "$(zpool list -Ho name | tr '\n' ' ')" = "$POOL " ] ||
        die "a pool this task did not create is imported"
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2"
    mkdir -p "$dir" || die "mkdir $dir"
    log "point $1 $2 start"
    conf "$2"
    set_wbt
    v=$(zvol_of $POOL/vm)
    [ -b "$v" ] || die "zvol device"
    {
        echo "point $1 $2 tree $Z"
        echo "srcversion $(cat /sys/module/zfs/srcversion)"
        echo "window ramp $RAMP meas $MEAS ivl $IVL"
        echo "hddsim $("$T/hddsim" print "$T/params.conf")"
        for p in zfs_arc_max zfs_dirty_data_max zfs_dirty_data_sync_percent \
            zfs_vdev_async_write_active_min_dirty_percent \
            zfs_vdev_async_write_active_max_dirty_percent \
            zfs_delay_min_dirty_percent zfs_txg_timeout zfs_txg_history \
            zfs_vdev_sync_write_max_active zfs_vdev_sync_read_max_active \
            zfs_vdev_async_write_min_active zfs_vdev_async_write_max_active \
            zfs_immediate_write_sz zfs_vdev_disk_sync_writes \
            zfs_vdev_queue_age_ms zfs_vdev_aggregation_limit; do
            pval $p
        done
        echo "pool $(zpool list -Hp -o size,alloc,frag,cap $POOL)"
        echo "memavail $ma"
        devinfo
    } > "$dir/layout.txt"
    for id in $IDS; do devs+=("ublkb$id"); done
    python3 -I "$H/hr-sample.py" $((RAMP + MEAS)) $IVL "$dir/samples.txt" \
        "${devs[@]}" &
    pids+=($!)
    python3 -I "$H/hr-load.py" guest "$v" $((RAMP + MEAS)) \
        "$dir/guest.txt" lib_mb=$LIB_MB ring_mb=$RING_MB db_mb=$DB_MB \
        journal_mb=$JOURNAL_MB > "$dir/guest.err" 2>&1 &
    pids+=($!)
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
    for p in "${pids[@]}"; do wait "$p" || rc=$?; done
    wait $ipid $wpid
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir: $(tail -3 "$dir/guest.err")"
    log "point $1 $2 done: $(awk '$2 == "S" || $2 == "K" {n++; s += $4}
        $2 == "W" {w += $3} END {printf "fsync n %d mean %.0f ms, media %.1f MB/s",
        n, n ? s / n / 1e6 : 0, w / 1e6 / '"$((RAMP + MEAS))"'}' "$dir/guest.txt")"
}

quiet() {
    local a b l0 l1
    a=$(head -1 /proc/stat); l0=$(cut -d' ' -f1 /proc/loadavg)
    sleep "$1"
    b=$(head -1 /proc/stat); l1=$(cut -d' ' -f1 /proc/loadavg)
    log "quiet $1 s: $(echo "$a" "$b" | awk '{
        n = (NF - 2) / 2
        for (i = 1; i <= 8; i++) { d[i] = $(i + n + 2) - $(i + 1); t += d[i] }
        if (t == 0) t = 1
        printf "idle %.2f %% iowait %.2f %% steal %.2f %%", \
            100 * d[4] / t, 100 * d[5] / t, 100 * d[8] / t }')" \
        "load1 $l0 -> $l1"
}

teardown() {
    local id pid n cur
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    if [ -x "$T/hddsim" ] && [ -f "$T/hddsim-70.pid" ]; then
        ( conf fast ) || log "teardown: could not switch the disks to fast"
    fi
    if [ -n "$cur" ]; then
        use_tree "$cur"
        zpool list -H $POOL > /dev/null 2>&1 && zpool destroy $POOL
        "$Z/scripts/zfs.sh" -u
    fi
    for id in $IDS; do
        pid=$(cat "$T/hddsim-$id.pid" 2> /dev/null) || continue
        kill -TERM "$pid" 2> /dev/null
        n=0
        while kill -0 "$pid" 2> /dev/null && [ $n -lt 300 ]; do
            sleep 0.1; n=$((n + 1))
        done
        if kill -0 "$pid" 2> /dev/null; then
            log "teardown: hddsim $id still running, killed"
            kill -KILL "$pid"
            sleep 1
            "$T/hddsim" del "$id"
        fi
        rm -f "$T/hddsim-$id.pid"
    done
    if [ "$(cat "$T/ublk-loaded-by-us" 2> /dev/null)" = yes ]; then
        modprobe -r ublk_drv || log "teardown: ublk_drv stays loaded"
    fi
    rm -f "$T/cur-tree" "$T/cur-conf" "$T/ublk-loaded-by-us"
    log "teardown done: modules $(lsmod | awk '$1 ~ /^(zfs|spl|ublk_drv)$/ {printf "%s ", $1}')," \
        "devices $(shopt -s nullglob; echo /dev/ublkb*)"
}

mkdir -p "$L" "$T" || exit 1
case ${1:-} in
disks) [ $# -eq 2 ] || die "usage: disks TREE"; disks "$2" ;;
pool) pool ;;
selftest) selftest ;;
probe) [ $# -eq 2 ] || die "usage: probe CONF"; probe "$2" ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"; quiet "$2" ;;
point) [ $# -eq 3 ] || die "usage: point PASS CONF"; point "$2" "$3" ;;
teardown) teardown ;;
*) die "usage: disks TREE | selftest | probe CONF | pool | quiet SECONDS |" \
    "point PASS CONF | teardown" ;;
esac
