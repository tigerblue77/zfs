#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# Host rig driver, v2 (arms): the owner's DS_HDD_R6_1 (5 SATA HDDs in
# raidz2, no log device, scheduler none, blk-wbt at 75 ms, one
# 1M-volblocksize zvol under a Plex VM) rebuilt in a test VM, with the
# device configuration calibration chose, and switched between the arms of
# arms.txt: a code tree, module parameters and device settings each.
#
# Five emulated disks served by hddsim (ublk): one actuator, NCQ with
# shortest-positioning-first reordering and optional NCQ priority, a
# volatile write cache destaged in idle time, SATA flushes that wait for
# the queue and drain the cache.  Memory-backed, pattern-compressed.  A
# raidz2 on them, filled to 66 % with holes, a sparse 1M zvol, and
# hr-load.py's guest.
#
#   hr-bench.sh disks               hddsim built, the disks, tree R loaded
#   hr-bench.sh selftest            the model of the calibrated point,
#                                   NCQ priority off and on, offline
#   hr-bench.sh probe               raw service times of one disk, no pool
#   hr-bench.sh pool                pool, zvol and filler, fast disks
#   hr-bench.sh arm ARM [FILE]      the rig switched to ARM, what is loaded
#                                   written to FILE ($L/arm-ARM.txt)
#   hr-bench.sh armcheck            every arm once, unmeasured: identity,
#                                   and which arms' reads and writes reach
#                                   the disks marked high priority
#   hr-bench.sh quiet SECONDS       idle, iowait, steal and load1
#   hr-bench.sh point PASS ARM      one measured run of ARM
#   hr-bench.sh pass PASS ROW DEADLINE
#                                   a warm-up run (pass W<PASS>), then the
#                                   arms of plan.txt row ROW; a point that
#                                   could end after DEADLINE (Unix time) is
#                                   not started, nor those after it
#   hr-bench.sh teardown            pool, modules, disks removed
#
# The kit lives in $L/bin; $L is its parent directory, $T is
# /var/tmp/<the name of $L>, the trees are under $TREES (hr-trees.sh).
# $L/best.conf is the calibrated grid point, copied there by the task;
# $L/best.guest the guest's settings calibration chose, written there by
# the task: one line, "-" for hr-load.py's defaults, else media_mbps=N
# and/or media_qd=N (v2.1; v2 always ran the defaults).
# Each point exports the pool, loads its arm's tree, writes the arm's
# parameters, imports the pool, reads the guest's database and journal
# areas once (so that their partial-block writes find their blocks in the
# ARC, as on a rig that ran for a while), sets the disks' model and blk-wbt,
# then measures as v1 did: RAW/PASS/ARM/ holds layout.txt, arm.txt, the
# guest's and the sampler's logs, zpool iostat streams and the counters at
# both ends of the measured window; the disks' own per-second statistics
# stay in $L/dev/ID.stats, read by wall time.
set -u
export LC_ALL=C

H=$(dirname "$(readlink -f "$0")")
L=${HR_L:-$(dirname "$H")}
T=${HR_T:-/var/tmp/$(basename "$L")}
TREES=${HR_TREES:-/var/tmp/hra-trees}
BEST=$L/best.conf
BESTG=$L/best.guest
GV=()
P=/sys/module/zfs/parameters
POOL=hrp
IDS="70 71 72 73 74"
DEV_MIB=16384           # per disk
RAMP=${RAMP:-45}
MEAS=${MEAS:-180}
W_RAMP=30               # the warm-up run of each pass
W_MEAS=60
IVL=0.25                # sampler interval, seconds
FILL_HI=88              # cap % reached by the filler before the holes
CAP_TARGET=66           # the host's cap %
FILE_MB=1               # filler file size: its holes set fragmentation
LIB_MB=8192
RING_MB=4096
DB_MB=256
JOURNAL_MB=64
MIN_AVAIL=1024          # MiB of MemAvailable required before each run
SWITCH_PAD=90           # s a point takes beyond RAMP + MEAS, at most
# Host-equivalent ZFS settings, as v1 (calibration), written after every
# module load.
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

# load_tree TREE: unload the ZFS modules, load TREE's, check srcversion,
# write the common settings.
load_tree() {
    local want got kv
    use_tree "$1"
    if lsmod | grep -q '^zfs '; then
        udevadm settle
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
        setp "${kv%%=*}" "${kv#*=}"
    done
    echo "$1" > "$T/cur-tree"
}

# setp NAME VALUE: write a module parameter and read it back.
setp() {
    [ -e "$P/$1" ] || die "$Z has no $1"
    echo "$2" > "$P/$1" || die "$1=$2"
    [ "$(cat "$P/$1")" = "$2" ] || die "$1 reads $(cat "$P/$1"), not $2"
}

# read_guest: GV = the words of best.guest ("-": none), each checked.
read_guest() {
    local w
    [ -r "$BESTG" ] || die "no guest settings $BESTG"
    read -r -a GV <<< "$(head -n 1 "$BESTG")"
    [ ${#GV[@]} -gt 0 ] || die "empty $BESTG"
    if [ "${GV[0]}" = - ] && [ ${#GV[@]} -eq 1 ]; then
        GV=()
        return 0
    fi
    for w in "${GV[@]}"; do
        [[ $w =~ ^(media_mbps|media_qd)=[0-9]+(\.[0-9]+)?$ ]] ||
            die "bad guest setting $w in $BESTG"
    done
}

pval() {
    if [ -e "$P/$1" ]; then echo "$1=$(cat "$P/$1")"; else echo "$1=absent"
    fi
}

# conf FILE: make FILE the disks' parameters, wait until each server
# logs exactly them (a "P" line in its statistics).
conf() {
    local f=$1 want id pid n got
    [ -r "$f" ] || die "no parameters $f"
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
            [ $n -le 50 ] || die "hddsim $id did not take $f: $got"
            sleep 0.1
        done
    done
    echo "$f" > "$T/cur-conf"
}

# set_wbt USEC: blk-wbt's target, written 1 first so that blk-wbt starts
# each run again from its initial depth (0 switches it off).
set_wbt() {
    local id q
    for id in $IDS; do
        q=/sys/block/ublkb$id/queue
        if ! echo 1 > "$q/wbt_lat_usec" ||
            ! echo "$1" > "$q/wbt_lat_usec"; then
            die "wbt_lat_usec $1 on ublkb$id"
        fi
        [ "$(cat "$q/wbt_lat_usec")" = "$1" ] || die "ublkb$id wbt readback"
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
    set_wbt 75000
    echo "$H/grid/fast.conf" > "$T/cur-conf"
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

fast() { [ "$(cat "$T/cur-conf" 2> /dev/null)" = "$H/grid/fast.conf" ]; }

disks() {
    echo "$(basename "$L"): start" > /dev/kmsg
    [ -r "$BEST" ] || die "no calibrated point $BEST"
    read_guest
    [ -f "$TREES/R.stamp" ] || die "tree R not built"
    build_sim
    "$T/hddsim" print "$BEST" > /dev/null || die "bad $BEST"
    disks_up
    load_tree "$TREES/R"
    {
        echo "kernel $(uname -r)"
        echo "ublk_drv $(modinfo -F vermagic ublk_drv | cut -d' ' -f1)"
        echo "hddsim.c $(sha256sum < "$H/hddsim.c" | cut -d' ' -f1)"
        echo "best.conf $(sha256sum < "$BEST" | cut -d' ' -f1)" \
            "$("$T/hddsim" print "$BEST")"
        echo "best.guest ${GV[*]:--}"
        echo "zfs srcversion $(cat /sys/module/zfs/srcversion) tree $TREES/R"
        devinfo
    } > "$L/setup-devices.txt"
    log "disks done: $(echo /dev/ublkb7[0-4]), L $L, T $T, trees $TREES," \
        "MemAvailable $(memavail) MiB"
}

pool() {
    local id devs=() v n first i
    [ -f "$T/cur-tree" ] || die "no disks"
    use_tree "$(cat "$T/cur-tree")"
    fast || die "pool: disks not fast"
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
    "$T/hddsim" selftest "$BEST" 1800 > "$L/selftest.txt" || die "selftest"
    "$T/hddsim" priotest "$BEST" 1800 > "$L/priotest.txt" || die "priotest"
    log "selftest done"
}

probe() {
    fast || die "probe before the pool only"
    zpool list -H $POOL > /dev/null 2>&1 && die "probe before the pool only"
    conf "$BEST"
    python3 -I "$H/hr-load.py" probe /dev/ublkb70 30 > "$L/probe.txt" ||
        die "probe"
    conf "$H/grid/fast.conf"
    log "probe done: $(tr '\n' ';' < "$L/probe.txt")"
}

# arm_known ARM: ARM is a line of arms.txt.
arm_known() {
    awk -v a="$1" '$1 == a {f = 1} END {exit !f}' "$H/arms.txt"
}

# arm_def ARM: "TREE WBT NCQ_PRIO PARAMETER..." from arms.txt (empty for
# an unknown ARM).
arm_def() {
    awk -v a="$1" '$1 == a {$1 = ""; print substr($0, 2)}' "$H/arms.txt"
}

arm_ready() {   # arm_ready ARM: its tree was built
    local tree
    read -r tree _ <<< "$(arm_def "$1")"
    [ -f "$TREES/$tree.stamp" ]
}

# arm ARM FILE: export, load ARM's tree, its parameters, import, prewarm,
# the disks' model with ARM's NCQ priority, ARM's blk-wbt target; then
# write to FILE what is loaded.  The setup steps run on fast disks.
arm() {
    local a=$1 out=$2 tree wbt prio kv id v s0 s1 devs=() pv=()
    arm_known "$a" || die "unknown arm $a"
    read -r tree wbt prio _ <<< "$(arm_def "$a")"
    read -r -a pv <<< "$(arm_def "$a" | cut -d' ' -f4-)"
    [ "${pv[0]:-}" = - ] && pv=()
    [ -f "$TREES/$tree.stamp" ] || die "arm $a: tree $tree not built"
    conf "$H/grid/fast.conf"
    use_tree "$(cat "$T/cur-tree")"
    if zpool list -H $POOL > /dev/null 2>&1; then
        zpool export $POOL || die "export $POOL"
    fi
    load_tree "$TREES/$tree"
    for kv in "${pv[@]}"; do
        setp "${kv%%=*}" "${kv#*=}"
    done
    for id in $IDS; do devs+=(-d "/dev/ublkb$id"); done
    zpool import "${devs[@]}" -o cachefile=none $POOL || die "import $POOL"
    [ "$(zpool list -Ho name | tr '\n' ' ')" = "$POOL " ] ||
        die "a pool this task did not create is imported"
    zvol_wait
    v=$(zvol_of $POOL/vm)
    [ -b "$v" ] || die "zvol device"
    s0=$EPOCHREALTIME
    dd if="$v" of=/dev/null bs=1M iflag=direct status=none \
        skip=$((LIB_MB + RING_MB)) count=$((DB_MB + JOURNAL_MB)) ||
        die "prewarm"
    s1=$EPOCHREALTIME
    { cat "$BEST"; echo "ncq_prio=$prio"; } > "$T/arm.conf"
    conf "$T/arm.conf"
    set_wbt "$wbt"
    {
        echo "arm $a $(arm_def "$a")"
        echo "stamp $(cat "$TREES/$tree.stamp")"
        echo "srcversion $(cat /sys/module/zfs/srcversion)" \
            "built $(modinfo -F srcversion "$Z/module/zfs.ko")"
        echo "version $(cat /sys/module/zfs/version)"
        for kv in "${pv[@]}"; do echo "set ${kv%%=*}=$(cat "$P/${kv%%=*}")"; done
        echo "prewarm $(echo "$s0 $s1" | awk '{printf "%.2f", $2 - $1}') s"
        echo "pool $(zpool list -Hp -o name,size,alloc,frag,cap,health $POOL)" \
            "cachefile $(zpool get -Ho value cachefile $POOL)"
        for id in $IDS; do
            echo "hddsim $id $(grep '^P ' "$L/dev/$id.stats" | tail -1 |
                cut -d' ' -f3-)"
        done
        devinfo
        for kv in "$P"/*; do echo "param ${kv##*/}=$(cat "$kv")"; done
        for kv in /sys/module/spl/parameters/*; do
            echo "spl ${kv##*/}=$(cat "$kv")"
        done
    } > "$out"
    log "arm $a: tree $tree srcversion $(cat /sys/module/zfs/srcversion)" \
        "wbt $wbt ncq_prio $prio ${pv[*]:-}"
}

# armcheck: every arm once, unmeasured; after each switch 256 MiB of the
# library are read and 16 synchronous 64 KiB writes go to the journal area,
# and the disks' statistics say which arrived marked high priority: reads
# when the loaded zfs_vdev_disk_rt_mask has bit 1, writes when it has bit 2
# and zfs_vdev_disk_sync_writes submits them REQ_SYNC|REQ_IDLE (the rig
# marker marks no other write); nothing marked elsewhere.
armcheck() {
    local a v t0 t1 n m nw mw rt sw wr ww verdict bad=0 arms=()
    mkdir -p "$L/armcheck" || die "mkdir"
    : > "$L/armcheck.txt"
    read -r -a arms <<< "$(awk '$1 !~ /^#/ {printf "%s ", $1}' "$H/arms.txt")"
    for a in "${arms[@]}"; do
        if ! arm_ready "$a"; then
            echo "$a skipped: no tree" >> "$L/armcheck.txt"
            continue
        fi
        arm "$a" "$L/armcheck/$a.txt"
        rt=$(cat "$P/zfs_vdev_disk_rt_mask" 2> /dev/null || echo 0)
        sw=$(cat "$P/zfs_vdev_disk_sync_writes" 2> /dev/null || echo 0)
        v=$(zvol_of $POOL/vm)
        t0=$(date +%s.%N)
        dd if="$v" of=/dev/null bs=1M iflag=direct status=none \
            skip=$(( (RANDOM % 31) * 256 )) count=256 || die "armcheck read"
        dd if=/dev/zero of="$v" bs=64K count=16 oflag=direct,dsync \
            status=none seek=$(( (LIB_MB + RING_MB + DB_MB) * 16 )) ||
            die "armcheck write"
        sleep 2
        t1=$(date +%s.%N)
        read -r n m nw mw <<< "$(cat "$L"/dev/7?.stats | awk -v a="$t0" -v b="$t1" '
            $1 == "S" && $3 >= a && $3 <= b && $6 == "r" && $11 == "w" {
                n += $7
                nw += $12
                for (i = 7; i < NF; i++)
                    if ($i == "mark") { m += $(i + 1); mw += $(i + 2) }
            } END { printf "%d %d %d %d", n, m, nw, mw }')"
        wr=unmarked
        ww=unmarked
        [ $((rt & 1)) -ne 0 ] && wr=marked
        [ $((rt & 2)) -ne 0 ] && [ "$sw" -ge 1 ] && ww=marked
        verdict=FAIL
        if [ "$n" -gt 0 ] && [ "$nw" -gt 0 ] &&
            { [ $wr = marked ] && [ "$m" -gt 0 ] ||
            { [ $wr = unmarked ] && [ "$m" -eq 0 ]; }; } &&
            { [ $ww = marked ] && [ "$mw" -gt 0 ] ||
            { [ $ww = unmarked ] && [ "$mw" -eq 0 ]; }; }; then
            verdict=ok
        fi
        [ $verdict = ok ] || bad=1
        echo "$a $verdict: $n disk reads, $m marked, want $wr;" \
            "$nw disk writes, $mw marked, want $ww;" \
            "rt_mask $rt sync_writes $sw;" \
            "$(sed -n 's/^srcversion //p' "$L/armcheck/$a.txt");" \
            "$(grep -o 'wbt_lat_usec [0-9]*' "$L/armcheck/$a.txt" | sort |
                uniq -c | tr -s ' ' | tr '\n' ';')" \
            "$(grep -o ' ncq_prio=[0-9]' "$L/armcheck/$a.txt" | sort |
                uniq -c | tr -s ' ' | tr '\n' ';')" \
            "$(sed -n 's/^set //p' "$L/armcheck/$a.txt" | tr '\n' ' ')" \
            >> "$L/armcheck.txt"
    done
    log "armcheck done: $(grep -c ' ok:' "$L/armcheck.txt") ok," \
        "$(grep -c ' FAIL:' "$L/armcheck.txt") FAIL," \
        "$(grep -c ' skipped:' "$L/armcheck.txt") skipped"
    [ $bad -eq 0 ] || die "armcheck, see $L/armcheck.txt"
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

# wtrace on|off DIR: best effort, never a stop.  With PR3's window on
# (P3W, zfs_vdev_queue_window non-zero), the tree has the tracepoint
# zfs:zfs_vdev__queue__window when the module was built with tracepoints,
# else the stub function of the same name; trace it in an instance of its
# own during the measured window and save the lines to DIR/window.txt.  The stub gets (vdev_t *, max) only: its lines
# give the vdev pointer and how often each leaf's window was adjusted, not
# the window itself.
WT=/sys/kernel/tracing
wtrace() {
    local i
    i=$WT/instances/hrw
    if [ "$1" = on ]; then
        [ "$(cat "$P/zfs_vdev_queue_window" 2> /dev/null || echo 0)" != 0 ] ||
            return 0
        [ -d "$WT/instances" ] || { echo "window_trace none: no tracefs" \
            >> "$2/layout.txt"; return 0; }
        rmdir "$i" 2> /dev/null
        echo '-:hrw/win' >> "$WT/kprobe_events" 2> /dev/null
        mkdir "$i" 2> /dev/null || return 0
        if [ -d "$i/events/zfs/zfs_vdev__queue__window" ] &&
            echo 1 > "$i/events/zfs/zfs_vdev__queue__window/enable"; then
            echo "window_trace event" >> "$2/layout.txt"
        elif grep -qw trace_zfs_vdev__queue__window /proc/kallsyms &&
            echo 'p:hrw/win trace_zfs_vdev__queue__window vd=%di:x64 max=%si:u64' \
                >> "$WT/kprobe_events" 2> /dev/null &&
            echo 1 > "$i/events/hrw/win/enable"; then
            echo "window_trace kprobe (vdev pointer and max only)" \
                >> "$2/layout.txt"
        else
            echo "window_trace none" >> "$2/layout.txt"
        fi
        return 0
    fi
    [ -d "$i" ] || return 0
    cp "$i/trace" "$2/window.txt" 2> /dev/null
    echo 0 > "$i/events/zfs/zfs_vdev__queue__window/enable" 2> /dev/null
    echo 0 > "$i/events/hrw/win/enable" 2> /dev/null
    rmdir "$i" 2> /dev/null
    echo '-:hrw/win' >> "$WT/kprobe_events" 2> /dev/null
    return 0
}

stamp() {
    local l
    while IFS= read -r l; do printf '%s %s\n' "$EPOCHREALTIME" "$l"; done
}

kcheck() {
    if dmesg | sed -n "/$(basename "$L"): start/,\$p" |
        grep -E 'hung_task|blocked for more than|BUG:|Oops|VERIFY|PANIC|Call Trace' \
        > "$1"; then
        die "kernel log, see $1"
    fi
    rm -f "$1"
}

point() {   # point PASS ARM [RAMP MEAS]
    local dir=$L/raw/$1/$2 ma v pids=() p rc=0 ipid wpid id devs=()
    local ramp=${3:-$RAMP} meas=${4:-$MEAS}
    read_guest
    [ -f "$T/cur-tree" ] || die "no setup"
    use_tree "$(cat "$T/cur-tree")"
    zpool list -H $POOL > /dev/null 2>&1 || die "pool $POOL not imported"
    [ "$(zpool list -Ho name | tr '\n' ' ')" = "$POOL " ] ||
        die "a pool this task did not create is imported"
    ma=$(memavail)
    [ "$ma" -ge $MIN_AVAIL ] || die "MemAvailable $ma MiB before $1 $2"
    mkdir -p "$dir" || die "mkdir $dir"
    log "point $1 $2 start"
    arm "$2" "$dir/arm.txt"
    v=$(zvol_of $POOL/vm)
    [ -b "$v" ] || die "zvol device"
    {
        echo "point $1 $2 tree $Z"
        echo "srcversion $(cat /sys/module/zfs/srcversion)"
        echo "window ramp $ramp meas $meas ivl $IVL"
        echo "hddsim $("$T/hddsim" print "$T/params.conf")"
        echo "guest ${GV[*]:--}"
        for p in zfs_arc_max zfs_dirty_data_max zfs_dirty_data_sync_percent \
            zfs_vdev_async_write_active_min_dirty_percent \
            zfs_vdev_async_write_active_max_dirty_percent \
            zfs_delay_min_dirty_percent zfs_txg_timeout zfs_txg_history \
            zfs_vdev_sync_write_max_active zfs_vdev_sync_read_max_active \
            zfs_vdev_async_write_min_active zfs_vdev_async_write_max_active \
            zfs_immediate_write_sz zfs_vdev_disk_sync_writes \
            zfs_vdev_queue_age_ms zfs_vdev_aggregation_limit \
            zfs_vdev_disk_rt_mask; do
            pval $p
        done
        echo "pool $(zpool list -Hp -o size,alloc,frag,cap $POOL)"
        echo "memavail $ma"
        devinfo
    } > "$dir/layout.txt"
    for id in $IDS; do devs+=("ublkb$id"); done
    python3 -I "$H/hr-sample.py" $((ramp + meas)) $IVL "$dir/samples.txt" \
        "${devs[@]}" &
    pids+=($!)
    python3 -I "$H/hr-load.py" guest "$v" $((ramp + meas)) \
        "$dir/guest.txt" lib_mb=$LIB_MB ring_mb=$RING_MB db_mb=$DB_MB \
        journal_mb=$JOURNAL_MB "${GV[@]}" > "$dir/guest.err" 2>&1 &
    pids+=($!)
    sleep "$ramp"
    wtrace on "$dir"
    snap "$dir" 0
    zpool iostat -T u -Hpvlq $POOL 1 $((meas + 1)) 2>&1 | stamp \
        > "$dir/iostat-lq.txt" &
    ipid=$!
    zpool iostat -T u -wpH $POOL 1 $((meas + 1)) 2>&1 | stamp \
        > "$dir/iostat-w1s.txt" &
    wpid=$!
    sleep "$meas"
    snap "$dir" 1
    wtrace off "$dir"
    for p in "${pids[@]}"; do wait "$p" || rc=$?; done
    wait $ipid $wpid
    kcheck "$dir/kernel.txt"
    [ "$rc" -eq 0 ] || die "a load stream failed (rc $rc) in $dir: $(tail -3 "$dir/guest.err")"
    log "point $1 $2 done: $(awk '$2 == "S" || $2 == "K" {n++; s += $4}
        $2 == "W" {w += $3} END {printf "fsync n %d mean %.0f ms, media %.1f MB/s",
        n, n ? s / n / 1e6 : 0, w / 1e6 / '"$((ramp + meas))"'}' "$dir/guest.txt")"
}

pass() {    # pass PASS ROW DEADLINE
    local p=$1 dl=$3 arms=() ready=() a
    read -r -a arms <<< "$(awk -v r="$2" '$1 == r {$1 = ""; print}' \
        "$H/plan.txt")"
    [ ${#arms[@]} -gt 0 ] || die "no plan row $2"
    for a in "${arms[@]}"; do
        arm_known "$a" || die "unknown arm $a in plan row $2"
    done
    for a in "${arms[@]}"; do
        if arm_ready "$a"; then ready+=("$a"); else
            log "pass $p: $a skipped, no tree"; fi
    done
    [ ${#ready[@]} -gt 0 ] || die "no arm of row $2 has a tree"
    log "pass $p start (row $2: ${ready[*]}; deadline" \
        "$(date -u -d "@$dl" +%H:%M:%S))"
    # warm-up: the row's last arm, so that its first has a predecessor
    a=${ready[${#ready[@]} - 1]}
    if [ $(( $(date +%s) + W_RAMP + W_MEAS + SWITCH_PAD )) -gt "$dl" ]; then
        log "pass $p: warm-up dropped by the deadline"
    else
        point "W$p" "$a" $W_RAMP $W_MEAS
    fi
    for a in "${ready[@]}"; do
        if [ $(( $(date +%s) + RAMP + MEAS + SWITCH_PAD )) -gt "$dl" ]; then
            log "pass $p: $a and the arms after it dropped by the deadline"
            break
        fi
        point "$p" "$a"
    done
    log "pass $p done"
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
        ( conf "$H/grid/fast.conf" ) ||
            log "teardown: could not switch the disks to fast"
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
case ${1:-}.$# in
disks.1) disks ;;
pool.1) pool ;;
selftest.1) selftest ;;
probe.1) probe ;;
arm.2) arm "$2" "$L/arm-$2.txt" ;;
arm.3) arm "$2" "$3" ;;
armcheck.1) armcheck ;;
quiet.2) quiet "$2" ;;
point.3) point "$2" "$3" ;;
pass.4) pass "$2" "$3" "$4" ;;
teardown.1) teardown ;;
*) die "usage: disks | selftest | probe | pool | arm ARM [FILE] |" \
    "armcheck | quiet SECONDS | point PASS ARM | pass PASS ROW DEADLINE |" \
    "teardown" ;;
esac
