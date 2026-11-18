#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# TASK-1118-WBT driver: what the block layer's writeback throttle (blk-wbt)
# does to ZFS writes on rotational disks, upstream master against fork
# branch vdev-disk-bio-sync-flags, which submits writes and flushes
# REQ_SYNC|REQ_IDLE as zfs_vdev_disk_sync_writes says.
#
# Five memory-backed null_blk devices made to look like SATA HDDs (8 ms per
# request, 150 MiB/s, 32 tags, a volatile write cache, mq-deadline, marked
# rotational so that blk-wbt's target is 75 ms), a raidz2 over them, and a
# mixed load for RAMP + MEAS seconds: a writer of 512 KiB O_DIRECT writes to
# a zvol of volblocksize 1M, 100 per second, with an fdatasync() after every
# 150; a reader of 32K to 256K O_DIRECT reads, 10 per second, from a zvol
# with primarycache=metadata; and 60 MiB/s of buffered writes to a file.
#
# Arms: M  = $T/M (master), blk-wbt as the row sets it
#       M0 = $T/M, blk-wbt off (wbt_lat_usec 0)
#       S  = $T/B (branch), zfs_vdev_disk_sync_writes=1
#       A  = $T/B, zfs_vdev_disk_sync_writes=2
# Rows: R75 = wbt_lat_usec 75000 (the rotational default)
#       R10 = wbt_lat_usec 10000: the reads of these devices, 8 ms or
#             more, exceed it, which keeps blk-wbt in the scaled-down
#             state that slow reads cause on real disks (the host's disk
#             read p99 was 1074 ms against 75 ms)
#
#   wbt-bench.sh setup                devices, pool, zvols, prefill
#   wbt-bench.sh flags                the flags of the bios each arm issues
#   wbt-bench.sh quiet SECONDS        idle, iowait, steal and load1
#   wbt-bench.sh pass NAME ROWS ARMS  for each arm of ARMS in that order,
#                                     each row of ROWS; one run each
#   wbt-bench.sh teardown             pool, modules and devices removed
#
# Every run exports the pool and imports it again (after reloading the
# modules when the tree changes), so each starts with the pool's data out of
# the ARC and its zpool iostat histograms empty.
set -u

T=/var/tmp/task-1118-wbt
L=/var/log/task-1118-wbt
H=$(dirname "$(readlink -f "$0")")
P=/sys/module/zfs/parameters
CFS=/sys/kernel/config/nullb
POOL=wbtp
NB="wbt0 wbt1 wbt2 wbt3 wbt4"
NB_SIZE=640         # MiB per device
NB_LAT=8000000      # ns per request
NB_MBPS=150
NB_QD=32
NB_CACHE=32         # MiB of volatile write cache per device
RAMP=${RAMP:-10}
MEAS=${MEAS:-60}
IVL=0.25            # sampler interval, seconds
W_RATE=100
W_BS=524288
W_EVERY=150
R_RATE=10
B_MBPS=60
B_SIZE=256
ARC_MAX=$((512 << 20))
PATH_BASE=$PATH
Z=""

die() { echo "wbt-bench: $*" >&2; log "STOP: $*"; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }

row_lat() {
    case $1 in
    R75) echo 75000 ;;
    R10) echo 10000 ;;
    *) die "unknown row $1" ;;
    esac
}

arm_tree() {
    case $1 in
    M|M0) echo "$T/M" ;;
    S|A) echo "$T/B" ;;
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
    echo "$1" > "$T/cur-tree"
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

setup() {
    local n q v devs=()
    mkdir -p "$T" "$L" || die "mkdir"
    echo "task-1118-wbt: start" > /dev/kmsg
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
        "MemAvailable $(awk '/^MemAvailable/ {print int($2/1024)}' \
        /proc/meminfo) MiB"
}

# arm_set ARM ROW DIR: export, load ARM's tree if another is loaded,
# import, set the arm's parameters and the row's blk-wbt target.
arm_set() {
    local tree cur
    tree=$(arm_tree "$1")
    cur=$(cat "$T/cur-tree" 2>/dev/null)
    use_tree "${cur:-$tree}"
    if zpool list -H $POOL > /dev/null 2>&1; then
        zpool export $POOL || die "export"
    fi
    if [ "$cur" != "$tree" ]; then
        load_tree "$tree"
    fi
    zpool import -d /dev $POOL || die "import"
    zvols_wait
    case $1 in
    S) echo 1 > $P/zfs_vdev_disk_sync_writes || die "sync_writes 1" ;;
    A) echo 2 > $P/zfs_vdev_disk_sync_writes || die "sync_writes 2" ;;
    *) [ -e $P/zfs_vdev_disk_sync_writes ] &&
        die "master tree has zfs_vdev_disk_sync_writes" ;;
    esac
    case $1 in
    M0) set_wbt 0 ;;
    *) set_wbt "$(row_lat "$2")" ;;
    esac
    {
        echo "arm $1 row $2 tree $Z"
        echo "srcversion $(cat /sys/module/zfs/srcversion)"
        if [ -e $P/zfs_vdev_disk_sync_writes ]; then
            echo "zfs_vdev_disk_sync_writes=$(cat \
                $P/zfs_vdev_disk_sync_writes)"
        else
            echo "zfs_vdev_disk_sync_writes=absent"
        fi
        local k
        for k in zfs_arc_max zfs_dirty_data_max zfs_txg_timeout \
            zfs_vdev_async_write_min_active zfs_vdev_async_write_max_active \
            zfs_vdev_sync_write_max_active zfs_vdev_aggregation_limit; do
            echo "$k=$(cat $P/$k)"
        done
        devinfo
    } > "$3/layout.txt"
}

snap() {
    {
        echo "time $(date +%s.%N)"
        head -1 /proc/stat
        echo "loadavg $(cat /proc/loadavg)"
        sed 's/^/psi_io /' /proc/pressure/io
        awk '$3 ~ /^wbt[0-4]$/ {print "disk", $0}' /proc/diskstats
        awk 'NR > 2 {print "zil", $1, $3}' /proc/spl/kstat/zfs/zil
    } > "$1"
}

kcheck() {
    if dmesg | sed -n '/task-1118-wbt: start/,$p' |
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
    python3 -I "$H/wbt-sample.py" "$2" $IVL "$1/samples.txt" $NB &
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

run_one() { # run_one PASS ROW ARM
    local dir=$L/raw/$1/$2/$3 rc
    mkdir -p "$dir" || die "mkdir $dir"
    arm_set "$3" "$2" "$dir"
    echo "window ramp $RAMP meas $MEAS ivl $IVL" >> "$dir/layout.txt"
    sleep 2
    loads "$dir" $((RAMP + MEAS))
    sleep "$RAMP"
    snap "$dir/snap0.txt"
    sleep "$MEAS"
    snap "$dir/snap1.txt"
    waitall
    rc=$?
    zpool iostat -wp $POOL > "$dir/iostat-w.txt" 2>&1
    zpool iostat -wpH $POOL > "$dir/iostat-wH.txt" 2>&1
    zpool iostat -rp $POOL > "$dir/iostat-r.txt" 2>&1
    zpool iostat -lpv $POOL > "$dir/iostat-l.txt" 2>&1
    cp /proc/spl/kstat/zfs/$POOL/txgs "$dir/txgs.txt" 2>/dev/null
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir"
    log "run $1 $2 $3 done:" \
        "$(awk '$2 == "F" {n++; s += $4} END {
            printf "fsync n %d mean %.0f ms", n, n ? s / n / 1e6 : 0}' \
            "$dir/writer.txt")"
}

pass() {    # pass NAME "ROWS" "ARMS"
    local a r
    for a in $3; do arm_tree "$a" > /dev/null; done
    for r in $2; do row_lat "$r" > /dev/null; done
    mkdir -p "$L/raw/$1" || die "mkdir"
    echo "rows $2 arms $3" > "$L/raw/$1/order.txt"
    log "pass $1 start (rows $2, arms $3)"
    for a in $3; do
        for r in $2; do
            run_one "$1" "$r" "$a"
        done
    done
    log "pass $1 done"
}

# flags: for master and for the branch at sync_writes 0, 1 and 2, the bios
# queued to the five devices during 8 s of the load, counted by rwbs.
flags() {
    local tp cfg tree sw n want out
    tp=/sys/kernel/tracing
    [ -e $tp/events/block/block_bio_queue/enable ] ||
        tp=/sys/kernel/debug/tracing
    [ -e $tp/events/block/block_bio_queue/enable ] || die "no tracefs"
    want=""
    for n in $NB; do
        want="$want $(tr ':' ',' < "/sys/block/$n/dev")"
    done
    out=$L/flags.txt
    : > "$out"
    for cfg in M:- B:0 B:1 B:2; do
        tree=$T/${cfg%%:*}
        sw=${cfg#*:}
        mkdir -p "$L/flags/$cfg" || die "mkdir"
        if [ "$tree" = "$T/M" ]; then
            arm_set M R75 "$L/flags/$cfg"
        else
            arm_set A R75 "$L/flags/$cfg"
            echo "$sw" > $P/zfs_vdev_disk_sync_writes || die "sync_writes"
            echo "override zfs_vdev_disk_sync_writes=$(cat \
                $P/zfs_vdev_disk_sync_writes)" >> "$L/flags/$cfg/layout.txt"
        fi
        echo 0 > $tp/tracing_on
        echo 4096 > $tp/buffer_size_kb
        echo > $tp/trace
        echo 1 > $tp/events/block/block_bio_queue/enable
        echo 1 > $tp/tracing_on
        loads "$L/flags/$cfg" 8
        waitall || die "flags $cfg: load failed"
        echo 0 > $tp/tracing_on
        echo 0 > $tp/events/block/block_bio_queue/enable
        awk -v want="$want" -v cfg="$cfg" '
            BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) ok[w[i]] = 1 }
            {
                for (i = 1; i < NF; i++) if ($i == "block_bio_queue:") break
                if (i >= NF || !(($(i + 1)) in ok)) next
                c[$(i + 2)]++
            }
            END {
                printf "%s", cfg
                for (k in c) printf " %s=%d", k, c[k]
                printf "\n"
            }' $tp/trace >> "$out"
        grep -c . $tp/trace > "$L/flags/$cfg/trace-lines.txt"
        echo > $tp/trace
    done
    echo 1 > $tp/tracing_on
    kcheck "$L/flags/kernel.txt"
    cat "$out"
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
flags) flags ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    quiet "$2" ;;
pass) [ $# -eq 4 ] || die "usage: pass NAME ROWS ARMS"
    [ -f "$T/cur-tree" ] || die "no setup"
    pass "$2" "$3" "$4" ;;
teardown) teardown ;;
*) die "usage: setup | flags | quiet SECONDS | pass NAME ROWS ARMS | teardown" ;;
esac
