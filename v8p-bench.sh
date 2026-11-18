#!/bin/bash
# TASK-1118-V8P fio driver: upstream master, "v6" (ZIL blocks issued by
# the thread that writes them out) and "v8" (v6, with the lwb latency
# behind zfs_commit_timeout_pct timed from the lwb's close).  It is
# TASK-1118-V7P's driver with three arms; the rows, the arm order of each
# pass in raw/NAME/order.txt and the optional zfs_commit_timeout_pct per
# pass (CTP) are unchanged.  Three release trees, $T/master, $T/v6 and
# $T/v8; an arm is a tree, switched by exporting the pools, reloading the
# modules and importing the pools again.
#
#   v8p-bench.sh setup              create the pools and fill the files
#   v8p-bench.sh quiet SECONDS      CPU idle, iowait, steal and load1 over
#                                   SECONDS, pools imported, nothing running
#   v8p-bench.sh pass NAME ROWS ARMS
#                                   one pass: for each arm of ARMS in that
#                                   order, the rows of ROWS; with CTP=N in
#                                   the environment, zfs_commit_timeout_pct
#                                   is N during the pass and the value the
#                                   modules loaded with is put back after it
#   v8p-bench.sh teardown           destroy the pools, devices and files
#
# Disk cap: GB written to the VM disk (DISK) and to the test disk (RDISK)
# since setup began.  A pass does not start, and a row that writes to a
# disk does not start, when that total plus the largest amount the same
# rows wrote before would exceed CAP_GB; the driver then exits with 3.
set -u

T=/var/tmp/task-1118-v8p
L=/var/log/task-1118-v8p
V=$T/vdev
P=/sys/module/zfs/parameters
H=$(dirname "$(readlink -f "$0")")
RAMP=${RAMP:-3}     # fio ramp seconds per row
LEAD=${LEAD:-4}     # seconds the bulk writer runs before the row starts
BULK_RATE=${BULK_RATE:-50m}
CAP_GB=${CAP_GB:-95}
DISK=${DISK:?set DISK to the /proc/diskstats name of the VM disk}
RDISK=${RDISK:?set RDISK to the /dev/disk/by-id path of the test disk}
CTP=${CTP:-}
CTP_OLD=""
POOLS="c1q c1b c1n c1r"
KEYF=$T/key
PATH_BASE=$PATH
ARMS_ALL="master v6 v8"
DISK_ROWS="f1 x1 r1"

die() { echo "v8p-bench: $*" >&2; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }

# Measured seconds per row.
rt() {
    case $1 in
    f1|r1) echo 12 ;;
    *) echo 10 ;;
    esac
}

# The GB a row is assumed to write before it has run once.
row_gb_default() {
    case $1 in
    f1|r1) echo 0.8 ;;
    x1) echo 1.6 ;;
    *) echo 0 ;;
    esac
}

rdev() { basename "$(readlink -f "$RDISK")"; }

# total_gb: GB written to DISK and to the test disk since setup began.
total_gb() {
    awk -v d="$DISK" -v r="$(rdev)" -v f="$T/disk0" '
        BEGIN { while ((getline l < f) > 0) { split(l, a, " "); s0[a[1]] = a[2] } }
        ($3 == d || $3 == r) && ($3 in s0) { t += ($10 - s0[$3]) * 512 }
        END { printf "%.1f", t / 1e9 }' /proc/diskstats
}

# row_est ROW: the largest GB this row wrote so far, or its default.
row_est() {
    local v
    v=$(awk -v r="$1" '$1 == r && $2 > m { m = $2 } END { if (m) print m }' \
        "$T/rowgb" 2>/dev/null)
    if [ -n "$v" ]; then echo "$v"; else row_gb_default "$1"; fi
}

# over EXTRA_GB: true when the total so far plus EXTRA_GB exceeds the cap.
over() {
    awk -v a="$(total_gb)" -v b="$1" -v c="$CAP_GB" \
        'BEGIN { exit !(a + b > c) }'
}

# zvols PREFIX: the block devices of the zvols PREFIX0 to PREFIX3, by
# their /dev/zvol links when udev made them, else the zd devices that the
# loaded tree's zvol_id names so (their numbers change with each import).
zvols() {
    local i d
    if [ -e "/dev/zvol/${1}0" ]; then
        for i in 0 1 2 3; do echo "/dev/zvol/$1$i"; done
        return
    fi
    for i in 0 1 2 3; do
        for d in /dev/zd*; do
            case $d in *p*) continue ;; esac
            [ -b "$d" ] || continue
            if [ "$("$Z/udev/zvol_id" "$d" 2>/dev/null)" = "$1$i" ]; then
                echo "$d"
                break
            fi
        done
    done
}

zvols_wait() {  # zvols_wait PREFIX: wait up to 60 s for its 4 zvols
    local i
    for i in $(seq 1 60); do
        [ "$(zvols "$1" | wc -l)" -eq 4 ] && return 0
        udevadm settle
        sleep 1
    done
    die "the zvols $1[0-3] did not appear"
}

loopdev() { # loopdev FILE: a direct-I/O loop device over FILE
    losetup --direct-io=on --sector-size 4096 -f --show "$1" ||
        die "losetup $1"
}

# fill FILE SIZE: write FILE in full with a non-zero pattern, so that the
# guest file system holds every block of it before the pool exists.
fill() {
    fio --name=fill --filename="$1" --rw=write --bs=1M --size="$2" \
        --direct=1 --ioengine=psync --buffer_pattern=0x5a5a5a5a \
        --end_fsync=1 >> "$L/prefill.log" 2>&1 || die "fill $1"
}

nullb_add() {   # nullb_add NAME SIZE_MB: a memory-backed null_blk device
    local d=/sys/kernel/config/nullb/$1
    mkdir "$d" || return 1
    echo "$2" > "$d/size" || return 1
    echo 4096 > "$d/blocksize" || return 1
    echo 1 > "$d/memory_backed" || return 1
    echo 1 > "$d/irqmode"
    nproc > "$d/submit_queues"
    echo 256 > "$d/hw_queue_depth"
    echo 1 > "$d/power" || return 1
    udevadm settle
    [ -b "/dev/$1" ]
}

# mkzvols PREFIX SIZE: zvols PREFIX0 to PREFIX3 of volblocksize 16K,
# written in full.
mkzvols() {
    local i d
    for i in 0 1 2 3; do
        zfs create -V "$2" -o volblocksize=16K "$1$i" || die "$1$i"
    done
    zvols_wait "$1"
    for d in $(zvols "$1"); do
        [ -b "$d" ] || die "$d is not a block device"
        fio --name=pz --filename="$d" --rw=write --bs=1M --size="$2" \
            --direct=1 --ioengine=psync >> "$L/prefill.log" 2>&1 ||
            die "prefill $d"
    done
}

# use_tree ARM: the tree whose zpool, zfs and zvol_id are used.
use_tree() {
    Z=$T/$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    export PATH="$Z:$PATH_BASE"
    command -v zpool | grep -qx "$Z/zpool" || die "zpool is not $Z/zpool"
}

# load_arm ARM: unload whatever ZFS modules are loaded, load ARM's, and
# check that they are the ones that tree built.
load_arm() {
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
    echo $((2 << 30)) > $P/zfs_arc_max || die "zfs_arc_max"
    echo "$1" > "$T/cur-arm"
}

setup() {
    local q=() b=() n=() i r
    mkdir -p "$V" "$L" || die "mkdir"
    [ -b "$RDISK" ] || die "$RDISK is not a block device"
    r=$(rdev)
    awk -v d="$DISK" -v r="$r" '$3 == d || $3 == r {print $3, $10}' \
        /proc/diskstats > "$T/disk0"
    [ "$(wc -l < "$T/disk0")" -eq 2 ] || die "$DISK or $r not in diskstats"
    : > "$T/loops"
    : > "$T/rowgb"
    load_arm master
    # c1q: files written in full first; c1b: sparse, as in C1B and V5P.
    for i in 0 1; do fill "$V/q$i" 3G; q+=("$(loopdev "$V/q$i")"); done
    truncate -s 3G "$V/b0" || die "truncate b0"
    b+=("$(loopdev "$V/b0")")
    printf 'c1q %s\n' "${q[@]}" >> "$T/loops"
    printf 'c1b %s\n' "${b[@]}" >> "$T/loops"
    printf 'c1r /dev/%s\n' "$r" >> "$T/loops"
    modprobe null_blk nr_devices=0 || die "null_blk is not available"
    for i in 0 1 2 3 4; do
        nullb_add "c1nb$i" 512 || die "null_blk c1nb$i"
        n+=("/dev/c1nb$i")
    done
    printf 'c1n %s\n' "${n[@]}" >> "$T/loops"
    zpool create -f -o ashift=12 -O atime=off c1q mirror "${q[@]}" ||
        die "zpool create c1q"
    zpool create -f -o ashift=12 -O atime=off c1b "${b[@]}" ||
        die "zpool create c1b"
    zpool create -f -o ashift=12 -O atime=off c1n raidz1 "${n[@]}" ||
        die "zpool create c1n"
    zpool create -f -o ashift=12 -O atime=off c1r "$RDISK" ||
        die "zpool create c1r"
    zfs create -o recordsize=4K c1q/fs || die "c1q/fs"
    zfs create -o recordsize=4K c1r/fs || die "c1r/fs"
    for i in c1q/bulk c1b/bulk; do
        zfs create -o recordsize=1M -o compression=gzip-9 "$i" || die "$i"
    done
    for i in c1q c1r; do
        fio --name=pf --filename="/$i/fs/f" --rw=write --bs=1M --size=1G \
            --ioengine=psync --end_fsync=1 >> "$L/prefill.log" 2>&1 ||
            die "prefill $i/fs"
    done
    for i in c1q c1b; do
        fio --name=pb --filename="/$i/bulk/w" --rw=write --bs=1M \
            --size=1G --buffer_compress_percentage=50 \
            --refill_buffers --ioengine=psync --end_fsync=1 \
            >> "$L/prefill.log" 2>&1 || die "prefill $i/bulk"
    done
    od -An -tx1 -N32 /dev/urandom | tr -d ' \n' > "$KEYF" || die "key"
    chmod 600 "$KEYF"
    zfs create -o encryption=aes-256-gcm -o keyformat=hex \
        -o keylocation="file://$KEYF" c1n/enc || die "c1n/enc"
    mkzvols c1n/v 128M
    mkzvols c1n/enc/e 128M
    for i in c1n/v c1n/enc/e; do
        echo "$i: $(zvols $i | tr '\n' ' ')"
    done > "$L/setup-zvols.txt"
    zpool list -v > "$L/setup-zpool-list.txt"
    zfs get -r recordsize,compression,volblocksize,sync,logbias,encryption \
        $POOLS > "$L/setup-props.txt"
    log "setup done, $(total_gb) GB written since setup began"
}

# quiet SECONDS: CPU shares and load1 over SECONDS, from /proc/stat.
quiet() {
    local a b l0 l1
    a=$(head -1 /proc/stat)
    l0=$(cut -d' ' -f1 /proc/loadavg)
    sleep "$1"
    b=$(head -1 /proc/stat)
    l1=$(cut -d' ' -f1 /proc/loadavg)
    # user nice system idle iowait irq softirq steal: guest time is
    # already in user and nice.
    log "quiet $1 s: $(echo "$a" "$b" | awk '{
        n = (NF - 2) / 2
        for (i = 1; i <= 8; i++) { d[i] = $(i + n + 2) - $(i + 1); t += d[i] }
        if (t == 0) t = 1
        printf "idle %.2f %% iowait %.2f %% steal %.2f %%", \
            100 * d[4] / t, 100 * d[5] / t, 100 * d[8] / t }')" \
        "load1 $l0 -> $l1"
}

# tqlist: the write and flush taskq kstats there are now.
tqlist() {
    ls /proc/spl/kstat/taskq 2>/dev/null |
        grep -E '^(z_wr_|z_flush_|z_zvol)' | sort
}

# prime: read every file and zvol the rows write, so that their indirect
# blocks are cached again after the module reload.  Reads only.
prime() {
    local d
    cat /c1q/fs/f /c1r/fs/f > /dev/null || die "prime files"
    for d in $(zvols c1n/v) $(zvols c1n/enc/e); do
        dd if="$d" of=/dev/null bs=1M status=none || die "prime $d"
    done
}

# arm NAME: export the pools, load NAME's modules, import the pools again;
# record which taskq kstat instance belongs to which pool.
arm() {
    local pool k before after
    zpool export $POOLS || die "export"
    load_arm "$1"
    : > "$ADIR/tqmap.txt"
    for pool in $POOLS; do
        before=$(tqlist)
        zpool import -d /dev "$pool" || die "import $pool"
        after=$(tqlist)
        comm -13 <(echo "$before") <(echo "$after") |
            sed "s/^/$pool /" >> "$ADIR/tqmap.txt"
    done
    zfs load-key c1n/enc || die "load-key"
    zfs mount -a || die "mount"
    zvols_wait c1n/v
    zvols_wait c1n/enc/e
    prime
    if [ -n "$CTP" ]; then
        CTP_OLD=$(cat $P/zfs_commit_timeout_pct) || die "commit_timeout_pct"
        echo "$CTP" > $P/zfs_commit_timeout_pct || die "set CTP=$CTP"
    fi
    {
        echo "arm $1 tree $Z"
        echo "srcversion $(cat /sys/module/zfs/srcversion)"
        echo "zfs_arc_max=$(cat $P/zfs_arc_max)"
        echo "zio_taskq_write=$(cat $P/zio_taskq_write)"
        echo "zfs_commit_timeout_pct=$(cat $P/zfs_commit_timeout_pct)"
        echo "cpuidle driver" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_driver 2>&1)" \
            "governor" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_governor 2>&1)"
        echo "root subtree_control $(cat /sys/fs/cgroup/cgroup.subtree_control)"
        while read -r pool k; do
            printf '%s %s threads_max %s\n' "$pool" "$k" \
                "$(awk '$1 == "threads_max" {print $3}' \
                /proc/spl/kstat/taskq/"$k")"
        done < "$ADIR/tqmap.txt"
        ps -eo comm=,nice= | awk '$1 ~ /^z_wr_iss/ {print $1, $2}' |
            sort | uniq -c | sed 's/^/threads nice: /'
    } > "$ADIR/layout.txt"
    sleep 5
}

snap() { python3 -I "$H/v5p-snap.py" "$DISK" "$T" "$2" > "$1"; }

bulk_start() {  # bulk_start POOL OUT RT: a rate-limited compressible stream
    fio --name=bulk --filename="/$1/bulk/w" --rw=write --bs=1M \
        --size=1G --buffer_compress_percentage=50 --refill_buffers \
        --ioengine=psync --rate="$BULK_RATE" --time_based \
        --runtime=$((LEAD + RAMP + $3 + 2)) --output-format=json \
        --output="$2" > /dev/null 2>&1 &
    BULK_PID=$!
    sleep "$LEAD"
}

row() { # row NAME DIR
    local name=$1 dir=$2 args=() rc d i pool r bs sz prefix gb g0
    local pids=()
    r=$(rt "$name")
    BULK_PID=""
    case $name in
    f1|x1) pool=c1q ;;
    r1) pool=c1r ;;
    n8|n8i|n32|n8e|w4|w8) pool=c1n ;;
    *) die "unknown row $name" ;;
    esac
    case " $DISK_ROWS " in
    *" $name "*)
        if over "$(row_est "$name")"; then
            log "CAP: $(total_gb) GB written, row $name not run"
            exit 3
        fi
        ;;
    esac
    g0=$(total_gb)
    # Let the previous row's txgs end first.
    zpool sync $POOLS
    sleep 2
    case $name in
    x1) bulk_start c1b "$dir/$name.bulk.json" "$r" ;;
    esac
    case $name in
    n8|n8i|n8e)
        bs=16k; [ "$name" = n8i ] && bs=64k
        sz=128M
        prefix=$pool/v; [ "$name" = n8e ] && prefix=c1n/enc/e
        # 8 O_SYNC writers, 2 on each of 4 zvols of a raidz1, no slog.
        args=(--rw=randwrite --bs="$bs" --ioengine=psync --direct=1
            --sync=1 --size="$sz" --numjobs=2 --group_reporting)
        i=0
        for d in $(zvols "$prefix"); do
            args+=(--name="v$i" --filename="$d")
            i=$((i + 1))
        done
        [ "$i" -eq 4 ] || die "$name: found $i zvols"
        ;;
    n32|w4|w8)
        # 32, 4 or 8 O_SYNC writers on one zvol, where an open lwb
        # waits for more records (zfs_commit_timeout_pct).
        d=$(zvols c1n/v | head -1)
        [ -b "$d" ] || die "$name: no zvol"
        args=(--name=v0 --filename="$d" --rw=randwrite --bs=16k
            --ioengine=psync --direct=1 --sync=1 --size=128M
            --numjobs="${name#[nw]}" --group_reporting)
        ;;
    *)
        # 4K random writes, each followed by an fsync.
        args=(--name=fs --filename="/$pool/fs/f" --rw=randwrite --bs=4k
            --size=1G --ioengine=psync --fsync=1 --numjobs=1
            --group_reporting --norandommap --randrepeat=0)
        ;;
    esac
    snap "$dir/$name.snap0.json" "$pool"
    python3 -I "$H/v5p-sample.py" $((RAMP + r + 1)) \
        > "$dir/$name.tq.txt" 2>/dev/null &
    pids+=($!)
    zpool iostat -Hpy -lq "$pool" 1 $((RAMP + r + 1)) \
        > "$dir/$name.iostat.txt" 2>&1 &
    pids+=($!)
    # Global options first: fio applies those after a --name to that job.
    fio --time_based --runtime="$r" --ramp_time="$RAMP" \
        --lat_percentiles=1 --percentile_list=50:99:99.9 \
        --write_iops_log="$dir/$name" --log_avg_msec=1000 \
        "${args[@]}" \
        --output-format=json --output="$dir/$name.json" \
        > "$dir/$name.fio.log" 2>&1
    rc=$?
    snap "$dir/$name.snap1.json" "$pool"
    wait "${pids[@]}"
    if [ -n "$BULK_PID" ]; then
        wait "$BULK_PID"
    fi
    gb=$(awk -v a="$(total_gb)" -v b="$g0" 'BEGIN {printf "%.2f", a - b}')
    echo "$name $gb" >> "$T/rowgb"
    log "row $name rc=$rc disk $gb GB"
}

pass() {    # pass NAME "ROWS" "ARMS"
    local pn=$1 a r est=0 k
    for a in $3; do
        case " $ARMS_ALL " in *" $a "*) ;; *) die "unknown arm $a" ;; esac
    done
    for r in $2; do
        case " $DISK_ROWS " in
        *" $r "*)
            for a in $3; do
                est=$(awk -v a="$est" -v b="$(row_est "$r")" \
                    'BEGIN {print a + b}')
            done
            ;;
        esac
    done
    if over "$est"; then
        log "CAP: $(total_gb) GB written + about $est GB for pass $pn" \
            "> $CAP_GB GB: pass $pn not run"
        exit 3
    fi
    case $CTP in
    "") ;;
    *[!0-9]*) die "CTP=$CTP is not a number" ;;
    esac
    mkdir -p "$L/raw/$pn" || die "mkdir"
    echo "$3" > "$L/raw/$pn/order.txt"
    log "pass $pn start, $(total_gb) GB written, about $est GB expected" \
        "${CTP:+(zfs_commit_timeout_pct $CTP)}"
    for a in $3; do
        ADIR=$L/raw/$pn/$a
        mkdir -p "$ADIR"
        log "pass $pn arm $a"
        arm "$a"
        for k in $2; do
            row "$k" "$ADIR"
        done
    done
    if [ -n "$CTP_OLD" ]; then
        echo "$CTP_OLD" > $P/zfs_commit_timeout_pct ||
            die "restore zfs_commit_timeout_pct"
        log "zfs_commit_timeout_pct back to" \
            "$(cat $P/zfs_commit_timeout_pct)"
    fi
    log "pass $pn done, $(total_gb) GB written since setup began" \
        "(cap $CAP_GB)"
}

teardown() {
    local p l i
    [ -f "$T/cur-arm" ] && use_tree "$(cat "$T/cur-arm")"
    for p in $POOLS; do zpool destroy "$p"; done
    if [ -f "$T/loops" ]; then
        while read -r p l; do
            case $l in /dev/loop*) losetup -d "$l" ;; esac
        done < "$T/loops"
    fi
    for i in 0 1 2 3 4; do
        if [ -d "/sys/kernel/config/nullb/c1nb$i" ]; then
            echo 0 > "/sys/kernel/config/nullb/c1nb$i/power"
            rmdir "/sys/kernel/config/nullb/c1nb$i"
        fi
    done
    modprobe -r null_blk
    [ -b "$RDISK-part1" ] && zpool labelclear -f "$RDISK-part1"
    wipefs -a "$RDISK" > /dev/null
    blkdiscard -f "$RDISK" 2> /dev/null
    "$Z/scripts/zfs.sh" -u
    rm -rf "$V" "$T/loops" "$T/disk0" "$KEYF"
    log "teardown done"
}

case ${1:-} in
setup) setup ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    mkdir -p "$L" || die "mkdir"
    quiet "$2" ;;
pass) [ $# -eq 4 ] || die "usage: pass NAME ROWS ARMS"
    [ -f "$T/cur-arm" ] || die "no setup"
    use_tree "$(cat "$T/cur-arm")"
    pass "$2" "$3" "$4" ;;
teardown) teardown ;;
*) die "usage: setup | quiet SECONDS | pass NAME ROWS ARMS | teardown" ;;
esac
