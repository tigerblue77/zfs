#!/bin/bash
# TASK-1118-TQL fio driver: upstream master against master plus the SPL
# taskq wake-up change ("tql").  Two release trees, $T/master and $T/tql;
# an arm is a tree, switched by exporting the pool, reloading the spl and
# zfs modules and importing the pool again.  The pool is one brd RAM
# disk, so no row writes to the VM disk.
#
#   tql-bench.sh setup              load master's modules, create the
#                                   brd disk, the pool and its files
#   tql-bench.sh quiet SECONDS      CPU idle, iowait, steal and load1 over
#                                   SECONDS, pool imported, nothing running
#   tql-bench.sh pass NAME ROWS ARMS
#                                   one pass: for each arm of ARMS in that
#                                   order, the rows of ROWS
#   tql-bench.sh lock NAME ROWS ARMS
#                                   as pass, but each row only feeds one
#                                   perf lock contention capture; its fio
#                                   results are not used
#   tql-bench.sh teardown           destroy the pool, unload the modules
#                                   and brd
#
# Rows (RAMP s of fio ramp, then RT s measured):
#   c8  cached 4K random reads, 8 jobs, one 256M file (primarycache=all)
#   r8  uncached 4K random reads, 8 jobs, one 512M file
#       (primarycache=metadata)
#   s8  4K random O_SYNC writes, 8 jobs, one 32M file each
#   w4  128K sequential buffered writes, lz4, half-compressible data,
#       4 jobs, one 128M file each
#   w8  the same with 8 jobs
set -u

T=/var/tmp/task-1118-tql
L=/var/log/task-1118-tql
P=/sys/module/zfs/parameters
POOL=tqlp
DEV=/dev/ram0
RAMP=${RAMP:-5}
RT=${RT:-30}
BRD_KB=${BRD_KB:-4194304}       # 4 GiB
ARC_MAX=$((1 << 30))
DIRTY_MAX=$((256 << 20))
LOCK_S=${LOCK_S:-20}
ARMS_ALL="master tql"
ROWS_ALL="c8 r8 s8 w4 w8"
PATH_BASE=$PATH

die() { echo "tql-bench: $*" >&2; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }

# use_tree ARM: the tree whose zpool and zfs are used.
use_tree() {
    Z=$T/$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    export PATH="$Z:$PATH_BASE"
    command -v zpool | grep -qx "$Z/zpool" || die "zpool is not $Z/zpool"
}

# load_arm ARM: unload whatever ZFS modules are loaded, load ARM's, check
# that spl and zfs are the ones that tree built, set the ARC and dirty
# data limits.
load_arm() {
    local m want got
    use_tree "$1"
    if lsmod | grep -qE '^(zfs|spl) '; then
        "$Z/scripts/zfs.sh" -u || die "unload"
    fi
    "$Z/scripts/zfs.sh" || die "load $1"
    for m in spl zfs; do
        want=$(modinfo -F srcversion "$Z/module/$m.ko")
        got=$(cat /sys/module/$m/srcversion)
        if [ -z "$want" ] || [ "$want" != "$got" ]; then
            die "$m srcversion $got loaded, $1 built $want"
        fi
    done
    echo $ARC_MAX > $P/zfs_arc_max || die "zfs_arc_max"
    echo $DIRTY_MAX > $P/zfs_dirty_data_max || die "zfs_dirty_data_max"
    echo "$1" > "$T/cur-arm"
}

# layfile DIR NAME SIZE NJOBS [fio options]: write NJOBS files
# DIR/NAME.<job>.0 of SIZE, the names fio gives the rows' jobs.
layfile() {
    local d=$1 n=$2 s=$3 j=$4
    shift 4
    fio --name="$n" --directory="$d" --rw=write --bs=1M --size="$s" \
        --numjobs="$j" --ioengine=psync --end_fsync=1 "$@" \
        >> "$L/prefill.log" 2>&1 || die "prefill $d/$n"
}

setup() {
    mkdir -p "$T" "$L" || die "mkdir"
    [ -e "$DEV" ] && die "$DEV exists already"
    lsmod | grep -q '^brd ' && die "brd is loaded already"
    load_arm master
    modprobe brd rd_nr=1 rd_size="$BRD_KB" max_part=0 || die "brd"
    udevadm settle
    [ -b "$DEV" ] || die "no $DEV"
    zpool create -f -o ashift=12 -O atime=off -O compression=off \
        -O recordsize=4K "$POOL" "$DEV" || die "zpool create"
    zfs create -o primarycache=all $POOL/cr || die "cr"
    zfs create -o primarycache=metadata $POOL/ur || die "ur"
    zfs create -o primarycache=metadata $POOL/sw || die "sw"
    zfs create -o primarycache=metadata -o recordsize=128K \
        -o compression=lz4 $POOL/w || die "w"
    layfile /$POOL/cr c 256M 1 --refill_buffers
    layfile /$POOL/ur u 512M 1 --refill_buffers
    layfile /$POOL/sw s 32M 8 --refill_buffers
    layfile /$POOL/w w 128M 8 --buffer_compress_percentage=50 \
        --refill_buffers
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zfs list -o name,used,lused,compressratio,recordsize,primarycache \
        -r $POOL > "$L/setup-zfs-list.txt"
    log "setup done"
}

# quiet SECONDS: CPU shares and load1 over SECONDS, from /proc/stat.
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

# prime: after an import, read every file once, so that the indirect
# blocks are cached again and c8's file is in the ARC.  Reads only.
prime() {
    cat /$POOL/ur/u.0.0 /$POOL/sw/s.* /$POOL/w/w.* > /dev/null ||
        die "prime"
    cat /$POOL/cr/c.0.0 > /dev/null || die "prime c"
}

# arm NAME: export the pool, load NAME's modules, import the pool again.
arm() {
    local k
    zpool export $POOL || die "export"
    load_arm "$1"
    zpool import -d "$DEV" $POOL || die "import"
    prime
    {
        echo "arm $1 tree $Z"
        echo "spl srcversion $(cat /sys/module/spl/srcversion)"
        echo "zfs srcversion $(cat /sys/module/zfs/srcversion)"
        echo "nproc $(nproc)"
        for k in zfs_arc_max zfs_dirty_data_max zio_taskq_read \
            zio_taskq_write zio_taskq_batch_pct zio_taskq_write_tpq \
            zfs_vdev_disk_calling_thread_io; do
            [ -r "$P/$k" ] && echo "$k=$(cat $P/$k)"
        done
        echo "brd $(cat /sys/block/ram0/size) sectors"
        echo "cpuidle driver" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_driver 2>&1)" \
            "governor" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_governor 2>&1)"
        for k in /proc/spl/kstat/taskq/z_*; do
            [ -e "$k" ] || continue
            printf 'taskq %s threads_max %s\n' "${k##*/}" \
                "$(awk '$1 == "threads_max" {print $3}' "$k")"
        done
    } > "$ADIR/layout.txt"
    sleep 5
}

# snap FILE: the counters a row is compared on, before and after it.
snap() {
    local f
    {
        echo "time $(date +%s.%N)"
        head -1 /proc/stat
        echo "loadavg $(cut -d' ' -f1-3 /proc/loadavg)"
        awk 'NR > 2 {print "dmu_tx", $1, $3}' /proc/spl/kstat/zfs/dmu_tx
        awk 'NR > 2 && $1 ~ /^(demand_data_(hits|misses)|size|c)$/ {
            print "arc", $1, $3}' /proc/spl/kstat/zfs/arcstats
        for f in /proc/spl/kstat/taskq/z_*; do
            [ -e "$f" ] || continue
            awk -v n="${f##*/}" '$1 ~ /^(tasks_executed|thread_wakeups|thread_wakeups_nowork|thread_sleeps)$/ {
                print "tq", n, $1, $3}' "$f"
        done
    } > "$1"
}

# fio_args ROW: the job options of a row (global options come first in
# the fio command line).
fio_args() {
    case $1 in
    c8) echo "--name=c --filename=/$POOL/cr/c.0.0 --rw=randread --bs=4k" \
        "--size=256M --numjobs=8 --norandommap --randrepeat=0" ;;
    r8) echo "--name=u --filename=/$POOL/ur/u.0.0 --rw=randread --bs=4k" \
        "--size=512M --numjobs=8 --norandommap --randrepeat=0" ;;
    s8) echo "--name=s --directory=/$POOL/sw --rw=randwrite --bs=4k" \
        "--size=32M --numjobs=8 --sync=1 --norandommap --randrepeat=0" ;;
    w4|w8) echo "--name=w --directory=/$POOL/w --rw=write --bs=128k" \
        "--size=128M --numjobs=${1#w} --buffer_compress_percentage=50" \
        "--refill_buffers" ;;
    *) die "unknown row $1" ;;
    esac
}

# row NAME DIR [lock]: one row; with "lock", one perf lock contention
# capture of LOCK_S seconds after the ramp instead of the CPU window.
row() {
    local name=$1 dir=$2 mode=${3:-} fpid rc
    # Let the previous row's txgs end first.
    zpool sync $POOL
    sleep 2
    snap "$dir/$name.snap0"
    # shellcheck disable=SC2046
    fio --time_based --runtime="$RT" --ramp_time="$RAMP" --ioengine=psync \
        --group_reporting --lat_percentiles=1 --percentile_list=50:99:99.9 \
        $(fio_args "$name") \
        --output-format=json --output="$dir/$name.json" \
        > "$dir/$name.fio.log" 2>&1 &
    fpid=$!
    if [ "$mode" = lock ]; then
        sleep $((RAMP + 2))
        if ! perf lock contention -a -b -k wait_total -E 30 -- \
            sleep "$LOCK_S" > "$dir/$name.lock.txt" 2>&1 ||
            ! grep -q contended "$dir/$name.lock.txt"; then
            mv "$dir/$name.lock.txt" "$dir/$name.lock-b.txt"
            # Without BPF: record 10 s of contention events instead.
            perf lock record -a -o "$T/lock.data" -- sleep 10 \
                > /dev/null 2>&1
            perf lock contention -i "$T/lock.data" -k wait_total -E 30 \
                > "$dir/$name.lock.txt" 2>&1
            rm -f "$T/lock.data"
        fi
    else
        # The CPU window: from 1 s after the ramp to 1 s before the end.
        sleep $((RAMP + 1))
        head -1 /proc/stat > "$dir/$name.cpu"
        sleep $((RT - 2))
        head -1 /proc/stat >> "$dir/$name.cpu"
        echo "window $((RT - 2))" >> "$dir/$name.cpu"
    fi
    wait "$fpid"
    rc=$?
    snap "$dir/$name.snap1"
    log "row $name rc=$rc"
    [ "$rc" -eq 0 ] || die "fio $name rc=$rc, see $dir/$name.fio.log"
}

check_args() {     # check_args ROWS ARMS
    local x
    for x in $1; do
        case " $ROWS_ALL " in *" $x "*) ;; *) die "unknown row $x" ;; esac
    done
    for x in $2; do
        case " $ARMS_ALL " in *" $x "*) ;; *) die "unknown arm $x" ;; esac
    done
}

pass() {    # pass NAME "ROWS" "ARMS" [lock]
    local pn=$1 a k
    check_args "$2" "$3"
    mkdir -p "$L/raw/$pn" || die "mkdir"
    echo "$3" > "$L/raw/$pn/order.txt"
    log "pass $pn start${4:+ (perf lock)}"
    for a in $3; do
        ADIR=$L/raw/$pn/$a
        mkdir -p "$ADIR"
        log "pass $pn arm $a"
        arm "$a"
        for k in $2; do
            row "$k" "$ADIR" "${4:-}"
        done
    done
    log "pass $pn done"
}

teardown() {
    if [ -f "$T/cur-arm" ]; then use_tree "$(cat "$T/cur-arm")"
    else use_tree master; fi
    zpool destroy $POOL
    "$Z/scripts/zfs.sh" -u
    modprobe -r brd
    rm -f "$T/cur-arm"
    log "teardown done"
}

# fio uses a job name that names an existing path as its file name, so
# run from an empty directory.
mkdir -p "$T/cwd" || die "mkdir $T/cwd"
cd "$T/cwd" || die "cd $T/cwd"

case ${1:-} in
setup) setup ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    mkdir -p "$L" || die "mkdir"
    quiet "$2" ;;
pass|lock) [ $# -eq 4 ] || die "usage: $1 NAME ROWS ARMS"
    [ -f "$T/cur-arm" ] || die "no setup"
    use_tree "$(cat "$T/cur-arm")"
    if [ "$1" = lock ]; then
        command -v perf > /dev/null || die "no perf"
        pass "$2" "$3" "$4" lock
    else
        pass "$2" "$3" "$4"
    fi ;;
teardown) teardown ;;
*) die "usage: setup | quiet SECONDS | pass NAME ROWS ARMS |" \
    "lock NAME ROWS ARMS | teardown" ;;
esac
