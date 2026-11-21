#!/bin/bash
# TASK-1121-TQL3 driver: upstream master against master plus the SPL
# taskq change that wakes a sleeping worker after dropping tq_lock
# ("twu").  Two release trees, $T/master and $T/twu; an arm is a tree,
# switched by exporting the pool, reloading the spl and zfs modules and
# importing the pool again.  The pool is one brd RAM disk, so no row
# writes to the VM disk.  Throughput passes and lock passes are separate:
# a lock pass records every lock:contention_begin and lock:contention_end
# event with its kernel call chain (16 frames), which slows the rows, so
# its fio results are not used.
#
#   tql3-bench.sh sites             per arm, the spin lock calls of the
#                                   SPL taskq functions in its spl.ko, as
#                                   perf prints their return addresses,
#                                   to $L/sites-<arm>.txt
#   tql3-bench.sh setup             load master's modules, create the
#                                   brd disk, the pool and its files
#   tql3-bench.sh quiet SECONDS     CPU idle, iowait, steal and load1 over
#                                   SECONDS, pool imported, nothing running
#   tql3-bench.sh pass NAME ROWS ARMS
#                                   one throughput pass: for each arm of
#                                   ARMS in that order, the rows of ROWS,
#                                   RT s measured with their CPU window
#   tql3-bench.sh lock NAME ROWS ARMS
#                                   one lock pass: the same rows, each
#                                   with one LOCK_S s capture kept in
#                                   $T/perf
#   tql3-bench.sh process           turn every capture in $T/perf into
#                                   <row>.agg.md/.agg.tsv (tql2-lock.py)
#                                   and <row>.lock0.txt (perf lock
#                                   contention --stack-skip 0), then
#                                   delete the capture
#   tql3-bench.sh teardown          destroy the pool, unload the modules
#                                   and brd
#
# Rows, J = the number of fio jobs (RAMP s of fio ramp first):
#   rJ   uncached 4K random reads, one 512M file (primarycache=metadata)
#   sJ   4K random O_SYNC writes, one 32M file per job
#   wJ   128K sequential buffered writes, lz4, half-compressible data,
#        one WMB file per job (WMB = 1024 / JOBS_MAX MiB, at least 32)
set -u

T=/var/tmp/task-1121-tql3
L=/var/log/task-1121-tql3
P=/sys/module/zfs/parameters
POOL=tqlp
DEV=/dev/ram0
RAMP=${RAMP:-5}
RT=${RT:-30}
LOCK_S=${LOCK_S:-10}
BRD_KB=${BRD_KB:-4194304}       # 4 GiB
JOBS_MAX=${JOBS_MAX:-$(nproc)}
WMB=$((1024 / JOBS_MAX))
[ "$WMB" -ge 32 ] || WMB=32
ARC_MAX=$((1 << 30))
DIRTY_MAX=$((256 << 20))
ARMS_ALL="master twu"
SITE_FNS="taskq_thread taskq_dispatch taskq_dispatch_ent task_expire
    task_expire_impl taskq_unlock_wake"
PATH_BASE=$PATH
KIT=$(cd "$(dirname "$0")" && pwd)

die() { echo "tql3-bench: $*" >&2; exit 1; }
log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }

# use_tree ARM: the tree whose zpool and zfs are used.
use_tree() {
    Z=$T/$1
    [ -x "$Z/zpool" ] || die "no tree $Z"
    export PATH="$Z:$PATH_BASE"
    command -v zpool | grep -qx "$Z/zpool" || die "zpool is not $Z/zpool"
}

# sites: for each arm and each function of SITE_FNS present in its
# spl.ko, every call to a spin lock, spin unlock or wait queue function
# (an unlock inlined by the kernel's configuration is not listed), as
# "function+0xRET callee landmark": RET is the return address, which is
# what perf prints for the frame that took the lock, and landmark the
# last call before it to schedule, remove_wait_queue, tsd_set or an
# indirect call (the task function), which tells the tq_lock sites of
# taskq_thread() apart.
sites() {
    local a k f
    for a in $ARMS_ALL; do
        k=$T/$a/module/spl.ko
        [ -f "$k" ] || die "no $k"
        for f in $SITE_FNS; do
            objdump -dr --no-show-raw-insn --disassemble="$f" "$k" |
                awk -v f="$f" '
                function hex(s,  i, n) {
                    n = 0
                    for (i = 1; i <= length(s); i++)
                        n = n * 16 + index("0123456789abcdef",
                            substr(s, i, 1)) - 1
                    return n
                }
                function flush() {
                    if (pend != "") {
                        printf "%s+0x%x %s %s\n", f, hex($1) - base, \
                            pend, lm
                        pend = ""
                    }
                }
                $0 ~ "^[0-9a-f]+ <" f ">:$" {
                    base = hex($1); lm = "none"; inf = 1; next
                }
                !inf { next }
                /^[0-9a-f]+ </ { inf = 0; next }
                /R_X86_64_(PLT|PC)32/ {
                    t = $NF; sub(/-0x[0-9a-f]+$/, "", t)
                    if (t ~ /^(_raw_spin_(try)?(un)?lock|__wake_up|add_wait_queue|remove_wait_queue)/)
                        pend = t
                    if (t ~ /^(schedule|remove_wait_queue|tsd_set)$/)
                        lm = t
                    else if (t ~ /^__x86_indirect/)
                        lm = "indirect"
                    next
                }
                /^ +[0-9a-f]+:/ {
                    sub(/:$/, "", $1); flush()
                    if ($0 ~ /call +\*/) lm = "indirect"
                }'
        done > "$L/sites-$a.txt"
        log "sites $a: $(wc -l < "$L/sites-$a.txt") calls"
    done
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
    mkdir -p "$T/perf" "$L" || die "mkdir"
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
    # cr is not read by any row; it keeps the pool's allocated space
    # that of TASK-1118-TQL on an 8-vCPU VM.
    layfile /$POOL/cr c 256M 1 --refill_buffers
    layfile /$POOL/ur u 512M 1 --refill_buffers
    layfile /$POOL/sw s 32M "$JOBS_MAX" --refill_buffers
    layfile /$POOL/w w "${WMB}M" "$JOBS_MAX" \
        --buffer_compress_percentage=50 --refill_buffers
    zpool list -v $POOL > "$L/setup-zpool-list.txt"
    zfs list -o name,used,lused,compressratio,recordsize,primarycache \
        -r $POOL > "$L/setup-zfs-list.txt"
    echo "JOBS_MAX=$JOBS_MAX WMB=$WMB BRD_KB=$BRD_KB" > "$T/setup.env"
    log "setup done (JOBS_MAX $JOBS_MAX, w files ${WMB}M, brd $BRD_KB KiB)"
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
# blocks are cached again.  Reads only.
prime() {
    cat /$POOL/ur/u.0.0 /$POOL/sw/s.* /$POOL/w/w.* > /dev/null ||
        die "prime"
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
        echo "numa nodes $(lscpu | awk -F: '/^NUMA node\(s\)/ {
            gsub(/ /, "", $2); print $2}')"
        for k in zfs_arc_max zfs_dirty_data_max spa_asize_inflation \
            zio_taskq_read zio_taskq_write zio_taskq_batch_pct \
            zio_taskq_write_tpq zfs_vdev_disk_calling_thread_io; do
            [ -r "$P/$k" ] && echo "$k=$(cat $P/$k)"
        done
        echo "brd $(cat /sys/block/ram0/size) sectors"
        echo "cpuidle driver" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_driver 2>&1)" \
            "governor" \
            "$(cat /sys/devices/system/cpu/cpuidle/current_governor 2>&1)"
        cat "$T/setup.env"
        for k in /proc/spl/kstat/taskq/z_*; do
            [ -e "$k" ] || continue
            printf 'taskq %s threads_max %s\n' "${k##*/}" \
                "$(awk '$1 == "threads_max" {print $3}' "$k")"
        done
    } > "$ADIR/layout.txt"
    sleep 5
}

# snap FILE: counters before and after a row: dmu_tx, ARC demand data,
# taskq counters.
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

# fio_args ROW: the job options of a row.
fio_args() {
    local j=${1##*[a-z]}
    if ! [ "$j" -ge 1 ] 2> /dev/null || [ "$j" -gt "$JOBS_MAX" ]; then
        die "row $1: jobs must be 1 to $JOBS_MAX"
    fi
    case $1 in
    r*) echo "--name=u --filename=/$POOL/ur/u.0.0 --rw=randread --bs=4k" \
        "--size=512M --numjobs=$j --norandommap --randrepeat=0" ;;
    s*) echo "--name=s --directory=/$POOL/sw --rw=randwrite --bs=4k" \
        "--size=32M --numjobs=$j --sync=1 --norandommap --randrepeat=0" ;;
    w*) echo "--name=w --directory=/$POOL/w --rw=write --bs=128k" \
        "--size=${WMB}M --numjobs=$j --buffer_compress_percentage=50" \
        "--refill_buffers" ;;
    *) die "unknown row $1" ;;
    esac
}

# capture DATA LOG: LOCK_S s of contention events with kernel call chains.
capture() {
    local rc
    perf record -a -q -e lock:contention_begin -e lock:contention_end \
        -c 1 -m 2048 --call-graph fp,16 --kernel-callchains -o "$1" \
        -- sleep "$LOCK_S" > "$2" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "retry without --kernel-callchains (rc $rc)" >> "$2"
        perf record -a -q -e lock:contention_begin \
            -e lock:contention_end -c 1 -m 2048 --call-graph fp,16 \
            -o "$1" -- sleep "$LOCK_S" >> "$2" 2>&1
        rc=$?
    fi
    return "$rc"
}

# row NAME DIR PASS ARM MODE: one row.  MODE pass: RT s measured, with
# the CPU window from 1 s after the ramp to 1 s before the end.  MODE
# lock: one capture 2 s after the ramp, the fio results not used.
row() {
    local name=$1 dir=$2 pn=$3 a=$4 mode=$5 fpid rc rt=$RT
    # Let the previous row's txgs end first.
    zpool sync $POOL
    sleep 2
    snap "$dir/$name.snap0"
    [ "$mode" = lock ] && rt=$((LOCK_S + 8))
    # shellcheck disable=SC2046
    fio --time_based --runtime="$rt" --ramp_time="$RAMP" --ioengine=psync \
        --group_reporting --lat_percentiles=1 --percentile_list=50:99:99.9 \
        $(fio_args "$name") \
        --output-format=json --output="$dir/$name.json" \
        > "$dir/$name.fio.log" 2>&1 &
    fpid=$!
    if [ "$mode" = lock ]; then
        sleep $((RAMP + 2))
        capture "$T/perf/$pn-$a-$name.data" "$dir/$name.rec.log" ||
            log "row $name: perf record failed, see $dir/$name.rec.log"
    else
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
        [[ $x =~ ^(r|s|w)[0-9]+$ ]] || die "unknown row $x"
        fio_args "$x" > /dev/null
    done
    for x in $2; do
        case " $ARMS_ALL " in *" $x "*) ;; *) die "unknown arm $x" ;; esac
    done
}

onepass() {    # onepass NAME "ROWS" "ARMS" MODE
    local pn=$1 a k
    check_args "$2" "$3"
    [ -e "$L/raw/$pn" ] && die "pass $pn exists already"
    mkdir -p "$L/raw/$pn" "$T/perf" || die "mkdir"
    echo "$3" > "$L/raw/$pn/order.txt"
    echo "$4" > "$L/raw/$pn/mode.txt"
    log "pass $pn start ($4)"
    for a in $3; do
        ADIR=$L/raw/$pn/$a
        mkdir -p "$ADIR"
        log "pass $pn arm $a"
        arm "$a"
        for k in $2; do
            row "$k" "$ADIR" "$pn" "$a" "$4"
        done
    done
    log "pass $pn done"
}

# process: one capture at a time, deleted once its tables are written.
process() {
    local f b pn a name dir ncpu
    ncpu=$(nproc)
    for f in "$T"/perf/*.data; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .data)
        pn=${b%%-*}
        a=${b#*-}
        name=${a#*-}
        a=${a%%-*}
        dir=$L/raw/$pn/$a
        [ -d "$dir" ] || die "no $dir for $f"
        perf script -i "$f" -F comm,tid,cpu,time,event,trace,ip,sym,symoff \
            2> "$dir/$name.script.err" |
            python3 -I "$KIT/tql2-lock.py" agg "$ncpu" \
            "$dir/$name.agg.md" "$dir/$name.agg.tsv" ||
            die "agg $f"
        perf lock contention -i "$f" -k wait_total -E 30 --stack-skip 0 \
            --max-stack 16 > "$dir/$name.lock0.txt" 2>&1
        log "processed $b ($(du -h "$f" | cut -f1))"
        rm -f "$f"
    done
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
sites) mkdir -p "$L" || die "mkdir"
    command -v objdump > /dev/null || die "no objdump"
    sites ;;
setup) setup ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"
    mkdir -p "$L" || die "mkdir"
    quiet "$2" ;;
pass|lock) [ $# -eq 4 ] || die "usage: $1 NAME ROWS ARMS"
    [ -f "$T/cur-arm" ] || die "no setup"
    if [ "$1" = lock ]; then
        command -v perf > /dev/null || die "no perf"
    fi
    use_tree "$(cat "$T/cur-arm")"
    # setup's JOBS_MAX, WMB and BRD_KB
    # shellcheck disable=SC1091
    . "$T/setup.env"
    onepass "$2" "$3" "$4" "$1" ;;
process) mkdir -p "$L" || die "mkdir"
    process ;;
teardown) teardown ;;
*) die "usage: sites | setup | quiet SECONDS | pass NAME ROWS ARMS |" \
    "lock NAME ROWS ARMS | process | teardown" ;;
esac
