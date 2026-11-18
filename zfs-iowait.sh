#!/bin/bash
#
# zfs-iowait.sh: read-only collector looking for the sources of host I/O
# pressure (PSI) on a Proxmox VE host running OpenZFS.
#
# For DURATION seconds it samples host and per-guest PSI and cgroup I/O,
# zpool iostat (pools and vdevs), the txg history, disk statistics and the
# kernel stacks of tasks blocked in uninterruptible sleep.  Before and after,
# it snapshots the ZFS kstats, the per-dataset kstats and the zpool latency
# and request-size histograms.  Everything goes to
# OUTBASE/zfs-iowait-<UTC stamp>/ and a .tar.gz of it; the only terminal
# output is a short summary and the tarball path.
#
# Read-only: it reads /proc, /sys and the name lines of the guest configs in
# /etc/pve, and runs only the list, get, status and iostat forms of zpool and
# zfs.  It writes nothing outside its output directory and tarball, changes
# no tunable, loads no module and installs nothing.
#
# Environment (all optional):
#   DURATION        seconds to sample (default 600)
#   INTERVAL        sampling interval in seconds (default 10)
#   STACK_INTERVAL  blocked-task sampling interval in seconds (default 0.5,
#                   0 disables it)
#   OUTBASE         parent of the output directory (default /root)
#   TEST_VMS        guest ids flagged as test machines (default "105 106 107")
#   ROOT            prefix for /proc, /sys, /dev and /etc/pve, for testing
#                   against a fake tree only (default empty)

# The awk programs are single-quoted on purpose: their $ are awk's.
# shellcheck disable=SC2016

set -u
export LC_ALL=C
umask 077

P=${ROOT:-}
DURATION=${DURATION:-600}
INTERVAL=${INTERVAL:-10}
STACK_INTERVAL=${STACK_INTERVAL:-0.5}
OUTBASE=${OUTBASE:-/root}
TEST_VMS=${TEST_VMS:-105 106 107}
AWK=${AWK:-awk}

case $DURATION in '' | *[!0-9]*) DURATION=600 ;; esac
case $INTERVAL in '' | *[!0-9]*) INTERVAL=10 ;; esac
case $STACK_INTERVAL in '' | *[!0-9.]* | .* | *. | *.*.*) STACK_INTERVAL=0.5 ;; esac
DURATION=$((10#$DURATION))
INTERVAL=$((10#$INTERVAL))
[ "$INTERVAL" -ge 1 ] || INTERVAL=10
COUNT=$((DURATION / INTERVAL))
[ "$COUNT" -ge 1 ] || COUNT=1
DURATION=$((COUNT * INTERVAL))
STACKS=1
case $STACK_INTERVAL in *[1-9]*) ;; *) STACKS=0 ;; esac

stamp=$(date -u +%Y%m%dT%H%MZ)
OUT=$OUTBASE/zfs-iowait-$stamp
n=1
until mkdir "$OUT" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 20 ]; then
        echo "zfs-iowait: cannot create an output directory in $OUTBASE" >&2
        exit 1
    fi
    OUT=$OUTBASE/zfs-iowait-$stamp-$n
done
mkdir "$OUT/start" "$OUT/end"

# From here on, stdout and stderr go to notes.log; the summary uses fd 3.
exec 3>&1 >>"$OUT/notes.log" 2>&1

PIDS=()
NOW=0
TS=0

note() { printf '%(%H:%M:%S)T %s\n' -1 "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
now() { printf -v NOW '%(%s)T' -1; }
ts_real() {
    if [ -n "${EPOCHREALTIME:-}" ]; then
        TS=$EPOCHREALTIME
    else
        printf -v TS '%(%s)T' -1
    fi
}

# readf VAR FILE: first line of FILE into VAR, "-" when absent or empty.
readf() {
    local __v=
    if [ -r "$2" ]; then
        IFS= read -r __v <"$2" || [ -n "$__v" ] || __v=
    fi
    [ -n "$__v" ] || __v=-
    printf -v "$1" '%s' "$__v"
}

# run OUTFILE CMD ARGS...: run a read-only command with a time limit.
run() {
    local out=$1 rc
    shift
    if ! have "$1"; then
        note "skip: $1 not installed ($*)"
        return 127
    fi
    if have timeout; then
        timeout 120 "$@" >"$out"
    else
        "$@" >"$out"
    fi
    rc=$?
    [ "$rc" -eq 0 ] || note "rc=$rc: $*"
    return "$rc"
}

# shellcheck disable=SC2317  # invoked by the EXIT trap
cleanup() {
    local p
    for p in ${PIDS[@]+"${PIDS[@]}"}; do
        kill "$p" 2>/dev/null
    done
}
trap cleanup EXIT
trap 'note "interrupted"; echo "zfs-iowait: interrupted, partial data in $OUT" >&3; exit 130' INT TERM HUP

# ---------------------------------------------------------------------------
# awk programs.  Files are always opened with getline, which skips a file
# that vanished, where a missing command-line operand would abort awk.
# ---------------------------------------------------------------------------

# stdin: "SRC<TAB>DST[<TAB>MAXLINES]"; copy SRC to DST, keeping the first
# line and the last MAXLINES lines when MAXLINES is set.
COPY_AWK='
BEGIN { FS = "\t" }
{
    src = $1; dst = $2; max = ($3 == "") ? 0 : $3 + 0; n = 0
    while ((getline l < src) > 0) {
        n++
        if (max == 0 || n == 1) { print l > dst; continue }
        buf[n % max] = l
    }
    close(src)
    if (max > 0 && n > 1) {
        s = (n - 1 > max) ? n - max + 1 : 2
        for (i = s; i <= n; i++) print buf[i % max] > dst
    }
    if (n > 0) close(dst)
    else if (quiet == 0) print "skip: " src " absent or empty"
}'

# stdin: objset kstat paths; one line per dataset: pool objset name k=v...
OBJSET_AWK='
{
    p = $0; os = p; sub(/.*\//, "", os)
    pool = p; sub(/\/[^\/]*$/, "", pool); sub(/.*\//, "", pool)
    name = "-"; kv = ""; n = 0
    while ((getline l < p) > 0) {
        if (++n <= 2) continue
        split(l, a, " ")
        if (a[1] == "dataset_name") name = a[3]
        else if (a[3] ~ /^[0-9]+$/) kv = kv " " a[1] "=" a[3]
    }
    close(p)
    if (n > 2) print pool, os, name kv
}'

# stdin: files to sample; routes each line to its sample file.
SAMPLE_AWK='
{
    p = $0; f = substr(p, plen + 1); kind = ""; lab = ""
    if (f ~ /^\/proc\/pressure\//) { kind = "psi"; lab = f; sub(/.*\//, "", lab) }
    else if (f ~ /\/txgs$/) {
        kind = "txg"; lab = f; sub(/\/txgs$/, "", lab); sub(/.*\//, "", lab)
    } else if (f == "/proc/diskstats") kind = "disk"
    else {
        kind = "cg"; c = f; sub(/^\/sys\/fs\/cgroup\//, "", c)
        what = c; sub(/.*\//, "", what); sub(/\/[^\/]*$/, "", c)
        if (c ~ /^qemu\.slice\/[0-9]+\.scope$/) {
            lab = c; sub(/^qemu\.slice\//, "", lab); sub(/\.scope$/, "", lab)
            lab = "VM" lab
        } else if (c ~ /^lxc\/[^\/]+$/) { lab = c; sub(/^lxc\//, "", lab); lab = "CT" lab }
        else if (c ~ /^lxc\.payload\.[^\/]+$/) {
            lab = c; sub(/^lxc\.payload\./, "", lab); lab = "CT" lab
        } else lab = "slice:" c
    }
    n = 0
    while ((getline l < p) > 0) {
        if (kind == "psi") print ts, lab, l >> (out "/psi.txt")
        else if (kind == "disk") print ts, l >> (out "/diskstats-samples.txt")
        else if (kind == "cg") print ts, lab, what, l >> (out "/cgroup-samples.txt")
        else { n++; if (n == 1) continue; buf[n % 200] = l }
    }
    close(p)
    if (kind == "txg")
        for (i = (n > 201 ? n - 199 : 2); i <= n; i++)
            print ts, lab, buf[i % 200] >> (out "/txgs-samples.txt")
}'

# stdin: /proc/<pid>/task/<tid>/stat lines; prints tasks in state D with the
# meaningful frames of their kernel stack, then a "T" tick line.
STACK_AWK='
function tag(c,  t) {
    if (c == "/") return "root"
    if (c ~ /^\/qemu\.slice\/[0-9]+\.scope/) {
        t = c; sub(/^\/qemu\.slice\//, "", t); sub(/\.scope.*/, "", t); return "VM" t
    }
    if (c ~ /^\/lxc\/[^\/]+/) { t = c; sub(/^\/lxc\//, "", t); sub(/\/.*/, "", t); return "CT" t }
    if (c ~ /^\/lxc\.payload\.[^\/]+/) {
        t = c; sub(/^\/lxc\.payload\./, "", t); sub(/\/.*/, "", t); return "CT" t
    }
    if (c ~ /^\/system\.slice\//) {
        t = c; sub(/^\/system\.slice\//, "", t); sub(/\/.*/, "", t); return "sys:" t
    }
    t = c; sub(/^\//, "", t); sub(/\/.*/, "", t)
    return (t == "") ? "?" : t
}
{
    line = $0; lp = index(line, "("); r = line; rp = 0
    while ((k = index(r, ")")) > 0) { rp += k; r = substr(r, k + 1) }
    if (lp < 3 || rp <= lp) next
    if (substr(line, rp + 2, 1) != "D") next
    tid = substr(line, 1, lp - 2)
    comm = substr(line, lp + 1, rp - lp - 1); gsub(/[ \t]/, "_", comm)
    sp = root "/proc/" tid "/stack"
    io = 0; zfs = 0; fr = ""; nf = 0; full = ""; nfull = 0
    while ((getline l < sp) > 0) {
        m = split(l, a, " ")
        if (m < 2) continue
        f = a[2]; sub(/\+.*/, "", f)
        if (m >= 3 && (a[3] == "[zfs]" || a[3] == "[spl]")) zfs = 1
        if (f ~ /^io_schedule/) io = 1
        if (nfull < 30) { full = full (nfull ? ";" : "") f; nfull++ }
        if (f ~ /^(__schedule|schedule|io_schedule|__cv_|cv_|__mutex_lock|mutex_lock|rwsem_down|down_read|down_write|__down|wait_for_completion|bit_wait|__wait_on_bit|out_of_line_wait_on_bit|rt_mutex|__rt_mutex|rwbase_)/) continue
        if (nf < 5) { fr = fr (nf ? "<" : "") f; nf++ }
    }
    close(sp)
    cg = "?"; cp = root "/proc/" tid "/cgroup"
    while ((getline l < cp) > 0) if (substr(l, 1, 3) == "0::") cg = substr(l, 4)
    close(cp)
    if (io == 0 && zfs == 0) full = "-"
    print "D", ts, tid, io, zfs, tag(cg), comm, (fr == "" ? "-" : fr), (full == "" ? "-" : full)
}
END { print "T", ts }'

# ---------------------------------------------------------------------------
# Collection
# ---------------------------------------------------------------------------

collect_identity() {
    local f=$OUT/identity.txt v
    {
        echo "date_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        readf v "$P/proc/sys/kernel/hostname"; echo "hostname $v"
        readf v "$P/proc/sys/kernel/osrelease"
        [ "$v" = - ] && v=$(uname -r)
        echo "kernel $v"
        readf v "$P/sys/module/zfs/version"; echo "zfs_module $v"
        readf v "$P/sys/module/spl/version"; echo "spl_module $v"
        "$AWK" '/^processor[ \t]*:/ { n++ }
            /^model name/ && m == "" { m = $0; sub(/^[^:]*: */, "", m) }
            END { print "cpus " (n ? n : "-"); print "cpu_model " (m == "" ? "-" : m) }' \
            "$P/proc/cpuinfo" 2>/dev/null || echo "cpus -"
        "$AWK" '$1 == "MemTotal:" { print "mem_kb " $2 }' "$P/proc/meminfo" 2>/dev/null
        echo "duration_s $DURATION"
        echo "interval_s $INTERVAL"
        if [ "$STACKS" -eq 1 ]; then echo "stacks @${STACK_INTERVAL}s"; else echo "stacks off"; fi
        echo "test_vms $TEST_VMS"
    } >"$f"
    if run "$OUT/.pveversion" pveversion -v; then
        grep -E '^(pve-manager|proxmox-kernel|pve-qemu-kvm|zfsutils-linux)' \
            "$OUT/.pveversion" | sed 's/^/pve /' >>"$f"
    fi
    rm -f "$OUT/.pveversion"
    if run "$OUT/.zfsversion" zfs version; then
        sed 's/^/zfs_version /' "$OUT/.zfsversion" >>"$f"
    fi
    rm -f "$OUT/.zfsversion"
}

PARAMS="zfs_dirty_data_max zfs_dirty_data_max_percent zfs_dirty_data_max_max
zfs_dirty_data_sync_percent zfs_delay_min_dirty_percent zfs_delay_scale
zfs_vdev_async_write_active_min_dirty_percent
zfs_vdev_async_write_active_max_dirty_percent
zfs_vdev_sync_read_min_active zfs_vdev_sync_read_max_active
zfs_vdev_sync_write_min_active zfs_vdev_sync_write_max_active
zfs_vdev_async_read_min_active zfs_vdev_async_read_max_active
zfs_vdev_async_write_min_active zfs_vdev_async_write_max_active
zfs_vdev_max_active zfs_vdev_queue_depth_pct zfs_txg_timeout zfs_txg_history
zfs_arc_max zfs_arc_min zfs_prefetch_disable zil_slog_bulk
zfs_immediate_write_sz zfs_commit_timeout_pct zil_nocacheflush
zfs_nocacheflush zfs_vdev_aggregation_limit
zfs_vdev_aggregation_limit_non_rotating zfs_vdev_scheduler
zfs_vdev_disk_classic zvol_threads zvol_request_sync zvol_use_blk_mq
zvol_blk_mq_threads zvol_blk_mq_queue_depth zvol_blk_mq_blocks_per_thread
zvol_volmode zio_taskq_batch_pct zfs_sync_taskq_batch_pct"

collect_params() {
    local d=$P/sys/module/zfs/parameters p v
    if [ ! -d "$d" ]; then
        note "skip: $d absent (zfs module not loaded?)"
        return
    fi
    for p in $PARAMS; do
        if [ -e "$d/$p" ]; then
            readf v "$d/$p"
            echo "$p $v"
        else
            echo "$p (absent)"
        fi
    done >"$OUT/params.txt"
    for p in "$d"/* "$P"/sys/module/spl/parameters/*; do
        [ -f "$p" ] && printf '%s\n' "$p"
    done | "$AWK" '{ f = $0; n = f; sub(/.*\//, "", n); v = ""
        if ((getline v < f) > 0) print n, v; close(f) }' >"$OUT/params-all.txt"
}

POOLS=()

collect_pools() {
    local name
    if ! have zpool; then
        note "skip: zpool not installed; no pool data"
        return
    fi
    run "$OUT/pools.txt" zpool list -H -p \
        -o name,size,allocated,free,fragmentation,capacity,dedupratio,health
    while IFS=$'\t' read -r name _; do
        [ -n "$name" ] && POOLS+=("$name")
    done <"$OUT/pools.txt"
    run "$OUT/zpool-list-v.txt" zpool list -v -H -P
    run "$OUT/zpool-status.txt" zpool status -P
    run "$OUT/zpool-get.txt" zpool get -H -p -o name,property,value,source \
        ashift,autotrim
    for name in ${POOLS[@]+"${POOLS[@]}"}; do
        run "$OUT/vdev-ashift-$name.txt" zpool get -H -p \
            -o name,property,value ashift "$name" all-vdevs
    done
}

# leaves.tsv: pool class top path dev parent rotational scheduler
# write_cache nr_requests max_sectors_kb model
collect_leaves() {
    local pool cls top path real b parent sp rot sch wc nr mx model q
    [ -s "$OUT/zpool-status.txt" ] || return
    "$AWK" '
        /^ *pool: / { pool = $2; conf = 0; next }
        /^ *scan: / { s = $0; sub(/^ *scan: */, "", s)
            st = (s ~ /in progress/) ? s : "idle"
            sub(/ since.*/, "", st); print pool "\t" st > scan; next }
        /^config:/ { conf = 1; next }
        /^errors:/ { conf = 0; next }
        conf == 0 || NF == 0 || $1 == "NAME" { next }
        {
            s = $0; sub(/^\t/, "", s); ind = match(s, /[^ ]/) - 1
            if ($1 == pool && ind == 0) { cls = "normal"; next }
            if (ind == 0) { cls = $1; next }
            if (ind == 2) top = $1
            if (substr($1, 1, 1) == "/") print pool "\t" cls "\t" top "\t" $1
        }' scan="$OUT/scan.tsv" "$OUT/zpool-status.txt" >"$OUT/.leaves"
    while IFS=$'\t' read -r pool cls top path; do
        real=$(readlink -f -- "$P$path" 2>/dev/null)
        b=${real##*/}
        [ -n "$b" ] || b=-
        parent=$b
        if [ "$b" != - ] && [ -e "$P/sys/class/block/$b/partition" ]; then
            sp=$(readlink -f -- "$P/sys/class/block/$b" 2>/dev/null)
            sp=${sp%/*}
            parent=${sp##*/}
        fi
        q=$P/sys/block/$parent/queue
        readf rot "$q/rotational"
        readf sch "$q/scheduler"
        case $sch in *\[*) sch=${sch#*[}; sch=${sch%%]*} ;; esac
        readf wc "$q/write_cache"
        readf nr "$q/nr_requests"
        readf mx "$q/max_sectors_kb"
        readf model "$P/sys/block/$parent/device/model"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$pool" "$cls" "$top" "$path" "$b" "$parent" "$rot" "$sch" \
            "${wc// /_}" "$nr" "$mx" "${model// /_}"
    done <"$OUT/.leaves" >"$OUT/leaves.tsv"
    rm -f "$OUT/.leaves"
}

DSPROPS="type,recordsize,volblocksize,compression,sync,logbias,primarycache"
DSPROPS="$DSPROPS,secondarycache,atime,relatime,xattr,dedup"
DSPROPS="$DSPROPS,special_small_blocks,redundant_metadata"

collect_datasets() {
    local o raw=$OUT/datasets-raw.txt
    have zfs || { note "skip: zfs not installed"; return; }
    run "$raw" zfs get -H -p -o name,property,value,source \
        -s local,default,inherited,received,temporary -t filesystem,volume \
        "$DSPROPS"
    for o in direct prefetch; do
        if run "$OUT/.opt" zfs get -H -p -o name,property,value,source \
            -s local,default,inherited,received,temporary \
            -t filesystem,volume "$o"; then
            cat "$OUT/.opt" >>"$raw"
        fi
    done
    rm -f "$OUT/.opt"
    "$AWK" 'BEGIN { FS = "\t"; OFS = "\t"
            np = split("type recordsize volblocksize compression sync logbias primarycache secondarycache atime relatime xattr dedup special_small_blocks redundant_metadata direct prefetch", pn, " ") }
        NF >= 3 {
            if (($1 in seen) == 0) { seen[$1] = 1; ord[++nd] = $1 }
            v = $3; if ($4 == "local" || $4 == "received") v = v "*"
            V[$1, $2] = v
        }
        END {
            h = "name"; for (j = 1; j <= np; j++) h = h OFS pn[j]; print h
            for (i = 1; i <= nd; i++) {
                s = ord[i]
                for (j = 1; j <= np; j++) s = s OFS (((ord[i], pn[j]) in V) ? V[ord[i], pn[j]] : "-")
                print s
            }
        }' "$raw" >"$OUT/datasets.tsv"
    if [ -d "$P/dev/zvol" ]; then
        find "$P/dev/zvol" -type l ! -name '*-part[0-9]*' -printf '%P\t%l\n' |
            "$AWK" 'BEGIN { FS = OFS = "\t" } { t = $2; sub(/.*\//, "", t); print $1, t }' \
            >"$OUT/zvols.tsv"
    fi
}

# guests.tsv: kind id name, from the name:/hostname: lines only.
collect_guests() {
    local f
    for f in "$P"/etc/pve/qemu-server/*.conf "$P"/etc/pve/lxc/*.conf; do
        [ -r "$f" ] && printf '%s\n' "$f"
    done | "$AWK" '{
        f = $0; id = f; sub(/.*\//, "", id); sub(/\.conf$/, "", id)
        kind = (f ~ /\/lxc\/[^\/]*$/) ? "CT" : "VM"; key = (kind == "CT") ? "hostname:" : "name:"
        name = "-"
        while ((getline l < f) > 0) {
            if (substr(l, 1, 1) == "[") break
            split(l, a, " ")
            if (a[1] == key && name == "-") name = a[2]
        }
        close(f)
        print kind "\t" id "\t" name
    }' >"$OUT/guests.tsv"
}

KSTATS="arcstats dmu_tx zil zfetchstats dbufstats dnodestats abdstats dmustats
vdev_mirror_stats metaslab_stats zio_stats brtstats"

snapshot() {
    local d=$OUT/$1 k=$P/proc/spl/kstat/zfs f pd pool quiet=0
    [ "$1" = end ] && quiet=1
    ts_real
    printf '%s\n' "$TS" >"$d/ts"
    {
        for f in diskstats vmstat stat meminfo loadavg swaps pressure/io \
            pressure/cpu pressure/memory; do
            printf '%s\t%s\n' "$P/proc/$f" "$d/${f//\//-}"
        done
        for f in $KSTATS; do
            printf '%s\t%s\n' "$k/$f" "$d/$f"
        done
        for f in "$k"/vdev_raidz*; do
            [ -e "$f" ] && printf '%s\t%s\n' "$f" "$d/${f##*/}"
        done
        for pd in "$k"/*/; do
            [ -d "$pd" ] || continue
            pool=${pd%/}
            pool=${pool##*/}
            printf '%s\t%s\t200\n' "${pd}txgs" "$d/pool-$pool-txgs"
            for f in dmu_tx_assign iostats state; do
                printf '%s\t%s\n' "$pd$f" "$d/pool-$pool-$f"
            done
        done
    } | "$AWK" -v quiet="$quiet" "$COPY_AWK"
    for f in "$k"/*/objset-*; do
        [ -e "$f" ] && printf '%s\n' "$f"
    done | "$AWK" "$OBJSET_AWK" >"$d/objsets.txt"
    if have zpool; then
        run "$d/iostat-w.txt" zpool iostat -H -p -P -v -w
        run "$d/iostat-r.txt" zpool iostat -H -p -P -v -r
    fi
}

sample_once() {
    local d f list=()
    for f in io cpu memory; do
        list+=("$P/proc/pressure/$f")
    done
    for d in "$P"/sys/fs/cgroup/qemu.slice/*.scope "$P"/sys/fs/cgroup/lxc/* \
        "$P"/sys/fs/cgroup/lxc.payload.* "$P"/sys/fs/cgroup/qemu.slice \
        "$P"/sys/fs/cgroup/lxc "$P"/sys/fs/cgroup/system.slice \
        "$P"/sys/fs/cgroup/user.slice; do
        [ -d "$d" ] || continue
        list+=("$d/io.pressure" "$d/io.stat")
    done
    for f in "$P"/proc/spl/kstat/zfs/*/txgs; do
        list+=("$f")
    done
    list+=("$P/proc/diskstats")
    printf '%s\n' "${list[@]}" |
        "$AWK" -v ts="$1" -v plen="${#P}" -v out="$OUT" "$SAMPLE_AWK"
}

stack_sampler() {
    local end=$1
    while :; do
        ts_real
        [ "${TS%%.*}" -lt "$end" ] || break
        cat "$P"/proc/[0-9]*/task/[0-9]*/stat 2>/dev/null |
            "$AWK" -v ts="$TS" -v root="$P" "$STACK_AWK"
        sleep "$STACK_INTERVAL"
    done >>"$OUT/stacks.txt"
}

start_background() {
    if have zpool; then
        zpool iostat -T u -H -p -l -q -y "$INTERVAL" "$COUNT" \
            >"$OUT/iostat-pool.txt" &
        PIDS+=("$!")
        zpool iostat -T u -H -p -P -v -l -q -y "$INTERVAL" "$COUNT" \
            >"$OUT/iostat-vdev.txt" &
        PIDS+=("$!")
    fi
    if have iostat; then
        S_TIME_FORMAT=ISO iostat -d -x -t -y "$INTERVAL" "$COUNT" \
            >"$OUT/iostat-x.txt" &
        PIDS+=("$!")
    else
        note "skip: iostat (sysstat) not installed"
    fi
    if [ "$STACKS" -eq 1 ]; then
        [ -r "$P/proc/1/stack" ] ||
            note "kernel stacks unreadable; blocked tasks recorded without frames"
        stack_sampler "$((T0 + DURATION))" &
        PIDS+=("$!")
    else
        note "blocked-task sampler disabled"
    fi
}

wait_background() {
    local p w
    for p in ${PIDS[@]+"${PIDS[@]}"}; do
        for ((w = 0; w < 30; w++)); do
            kill -0 "$p" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$p" 2>/dev/null; then
            note "background job $p still running after the run; stopping it"
            kill "$p" 2>/dev/null
        fi
        wait "$p" 2>/dev/null
    done
    PIDS=()
}

# ---------------------------------------------------------------------------
# Summary.  Each program prints its lines for the terminal and appends
# fuller tables to summary-detail.txt.
# ---------------------------------------------------------------------------

COMMON_AWK='
function num(x) { return (x == "-" || x == "") ? 0 : x + 0 }
function rd1(f,  l) { l = ""; if ((getline l < f) <= 0) l = ""; close(f); return l }
function dur_s(  a, b) {
    a = rd1(d "/start/ts"); b = rd1(d "/end/ts")
    return (a != "" && b != "" && b - a > 0) ? b - a : 0
}
function pct(a, b) { return (b > 0) ? sprintf("%.1f", 100 * a / b) : "-" }
function f1(x) { return sprintf("%.1f", x) }
function f2(x) { return sprintf("%.2f", x) }
function cut(s, n) { return (length(s) > n) ? substr(s, 1, n - 2) ".." : s }
function hsize(b) {
    if (b == "" || b == "-") return "-"
    b += 0
    if (b >= 1048576 && b % 1048576 == 0) return (b / 1048576) "M"
    if (b >= 1024 && b % 1024 == 0) return (b / 1024) "K"
    return b
}
function kstat(f, A,  l, a, n, m) {
    n = 0
    while ((getline l < f) > 0) {
        if (++n <= 2) continue
        m = split(l, a, " "); A[a[1]] = a[m]
    }
    close(f)
    return n > 2
}
'

HEAD_AWK='
function idv(k,  l, a, v, f) {
    f = d "/identity.txt"; v = "-"
    while ((getline l < f) > 0) {
        split(l, a, " ")
        if (a[1] == k) { v = l; sub(/^[^ ]+ /, "", v) }
    }
    close(f); return v
}
function cpuline(f, A,  l, a, i) {
    while ((getline l < f) > 0) {
        split(l, a, " ")
        if (a[1] == "cpu") { for (i = 2; i <= 9; i++) A[i] = a[i] + 0; break }
    }
    close(f)
}
BEGIN {
    mem = idv("mem_kb"); mem = (mem == "-") ? "-" : sprintf("%.0f GiB", mem / 1048576)
    printf "== zfs-iowait %s | kernel %s | zfs %s | %s CPUs | %s | %ss @%ss, stacks %s\n", \
        idv("hostname"), idv("kernel"), idv("zfs_module"), idv("cpus"), mem, \
        idv("duration_s"), idv("interval_s"), idv("stacks")
    f = d "/psi.txt"
    while ((getline l < f) > 0) {
        if (split(l, a, " ") < 7) continue
        k = a[2] " " a[3]; v = a[4]; sub(/^avg10=/, "", v); v += 0
        t = a[7]; sub(/^total=/, "", t)
        N[k]++; S[k] += v; if (((k in MX) == 0) || v > MX[k]) MX[k] = v
        if ((k in T0) == 0) { T0[k] = t; TS0[k] = a[1] }
        T1[k] = t; TS1[k] = a[1]
    }
    close(f)
    for (k in N) {
        R[k] = (TS1[k] > TS0[k]) ? sprintf("%.2f%%", (T1[k] - T0[k]) / ((TS1[k] - TS0[k]) * 1e4)) : "-"
        M[k] = sprintf("avg10 mean %.2f max %.2f", S[k] / N[k], MX[k])
    }
    if (("io some" in N) == 0) print "PSI io: /proc/pressure/io not available"
    else printf "PSI io  some %s (%s) | full %s (%s)\n", R["io some"], M["io some"], \
        R["io full"], M["io full"]
    cpuline(d "/start/stat", C0); cpuline(d "/end/stat", C1)
    tot = 0; for (i = 2; i <= 9; i++) tot += C1[i] - C0[i]
    iow = (tot > 0) ? sprintf("%.2f%%", 100 * (C1[6] - C0[6]) / tot) : "-"
    kstat_v(d "/start/vmstat", V0); kstat_v(d "/end/vmstat", V1)
    sw = ""; f = d "/end/swaps"; n = 0
    while ((getline l < f) > 0) {
        if (++n == 1) continue
        split(l, a, " "); s = a[1]; sub(/.*\//, "", s)
        if (s ~ /^zd[0-9]/) s = s "(zvol)"
        sw = sw (sw == "" ? "" : ",") s
    }
    close(f)
    printf "PSI cpu some %s | mem some %s full %s | cpu iowait %s | swap in/out %s/%s pages, on %s\n", \
        ("cpu some" in R) ? R["cpu some"] : "-", ("memory some" in R) ? R["memory some"] : "-", \
        ("memory full" in R) ? R["memory full"] : "-", iow, \
        num(V1["pswpin"]) - num(V0["pswpin"]), num(V1["pswpout"]) - num(V0["pswpout"]), \
        (sw == "") ? "none" : sw
}
function kstat_v(f, A,  l, a) {
    while ((getline l < f) > 0) { split(l, a, " "); A[a[1]] = a[2] }
    close(f)
}'

CG_AWK='
BEGIN {
    nt = split(tv, tl, " "); for (i = 1; i <= nt; i++) TV[tl[i]] = 1
    f = d "/guests.tsv"
    while ((getline l < f) > 0) { split(l, a, "\t"); NAME[a[1] a[2]] = a[3] }
    close(f)
    f = d "/cgroup-samples.txt"
    while ((getline l < f) > 0) {
        n = split(l, a, " "); if (n < 5) continue
        ts = a[1]; lab = a[2]; LAB[lab] = 1
        if (a[3] == "io.pressure" && n >= 8) {
            t = a[8]; sub(/^total=/, "", t); k = lab SUBSEP a[4]
            if ((k in P0) == 0) { P0[k] = t; PT0[k] = ts }
            P1[k] = t; PT1[k] = ts
        } else if (a[3] == "io.stat") {
            if ((lab in S0) == 0) S0[lab] = ts
            S1[lab] = ts
            for (i = 5; i <= n; i++) { split(a[i], b, "="); ST[lab, ts, b[1]] += b[2] }
        }
    }
    close(f)
    for (lab in LAB) {
        for (j = 1; j <= 2; j++) {
            w = (j == 1) ? "some" : "full"; k = lab SUBSEP w
            PS[lab, w] = ((k in P0) && PT1[k] > PT0[k]) ? (P1[k] - P0[k]) / ((PT1[k] - PT0[k]) * 1e4) : 0
        }
        dt = ((lab in S0) && S1[lab] > S0[lab]) ? S1[lab] - S0[lab] : 0
        for (j = 1; j <= 4; j++) {
            w = (j == 1) ? "rbytes" : (j == 2) ? "wbytes" : (j == 3) ? "rios" : "wios"
            IO[lab, w] = (dt > 0) ? (ST[lab, S1[lab], w] - ST[lab, S0[lab], w]) / dt : 0
        }
    }
    s = ""
    split("qemu.slice lxc system.slice user.slice", sl, " ")
    for (i = 1; i <= 4; i++) {
        lab = "slice:" sl[i]
        if (lab in LAB) s = s sprintf(" %s %.2f/%.2f", sl[i], PS[lab, "some"], PS[lab, "full"])
    }
    print "Cgroup io some/full % of run:" (s == "" ? " no cgroup PSI found" : s)
    ng = 0
    for (lab in LAB) if (lab ~ /^(VM|CT)/) G[++ng] = lab
    for (i = 1; i <= ng; i++)
        for (j = i + 1; j <= ng; j++)
            if (PS[G[j], "some"] > PS[G[i], "some"]) { t = G[i]; G[i] = G[j]; G[j] = t }
    print "Top guests by io some (some/full %, MB/s r/w; * = test VM):"
    line = ""
    for (i = 1; i <= ng && i <= 8; i++) {
        lab = G[i]; id = substr(lab, 3)
        e = sprintf("%-7s %-10s %5.2f/%5.2f r%5.1f w%5.1f", lab ((id in TV) ? "*" : ""), \
            cut((lab in NAME) ? NAME[lab] : "-", 10), PS[lab, "some"], PS[lab, "full"], \
            IO[lab, "rbytes"] / 1e6, IO[lab, "wbytes"] / 1e6)
        if (line == "") line = "  " e; else { print line " | " e; line = "" }
    }
    if (line != "") print line
    if (ng == 0) print "  (no qemu.slice or lxc guest cgroups found)"
    print "== cgroups (some% full% rMB/s wMB/s r_iops w_iops name)" >> det
    for (i = 1; i <= ng; i++) Q[i] = G[i]
    n = ng; for (lab in LAB) if (lab ~ /^slice:/) Q[++n] = lab
    for (i = 1; i <= n; i++) {
        lab = Q[i]
        printf "%-22s %6.2f %6.2f %8.2f %8.2f %8.1f %8.1f %s\n", lab, PS[lab, "some"], \
            PS[lab, "full"], IO[lab, "rbytes"] / 1e6, IO[lab, "wbytes"] / 1e6, \
            IO[lab, "rios"], IO[lab, "wios"], (lab in NAME) ? NAME[lab] : "-" >> det
    }
}'

STACKSUM_AWK='
BEGIN {
    f = d "/stacks.txt"
    while ((getline l < f) > 0) {
        n = split(l, a, " ")
        if (a[1] == "T") { ticks++; continue }
        if (a[1] != "D" || n < 8) continue
        ts = a[2]; io = a[4]; tg = a[6]; comm = a[7]; gsub(/[0-9]+/, "N", comm)
        m = split(a[8], fr, "<"); sg = ""
        for (i = 1; i <= m && i <= 4; i++) sg = sg (i > 1 ? "<" : "") fr[i]
        sg = tg " " comm ": " sg
        if (io == 1) {
            if ((ts in IOT) == 0) { IOT[ts] = 1; nio++ }
            if (((ts, sg) in SEEN) == 0) { SEEN[ts, sg] = 1; CNT[sg]++ }
            if ((sg in EX) == 0) EX[sg] = a[9]
            if (((ts, tg) in TSEEN) == 0) { TSEEN[ts, tg] = 1; TC[tg]++ }
        } else {
            if ((ts in DT) == 0) { DT[ts] = 1; nd++ }
            if (((ts, sg) in SEEN2) == 0) { SEEN2[ts, sg] = 1; CNT2[sg]++ }
            if ((sg in EX2) == 0) EX2[sg] = a[9]
        }
    }
    close(f)
    if (ticks == 0) { print "Blocked tasks: no samples (see notes.log)"; exit }
    nt = 0; for (t in TC) TL[++nt] = t
    for (i = 1; i <= nt; i++) for (j = i + 1; j <= nt; j++)
        if (TC[TL[j]] > TC[TL[i]]) { t = TL[i]; TL[i] = TL[j]; TL[j] = t }
    s = ""; for (i = 1; i <= nt && i <= 5; i++) s = s sprintf(" %s %.1f", TL[i], 100 * TC[TL[i]] / ticks)
    printf "Blocked tasks, %d samples: iowait in %.1f%% of samples; by cgroup:%s\n", \
        ticks, 100 * nio / ticks, (s == "" ? " -" : s)
    ns = 0; for (g in CNT) SL[++ns] = g
    for (i = 1; i <= ns; i++) for (j = i + 1; j <= ns; j++)
        if (CNT[SL[j]] > CNT[SL[i]]) { t = SL[i]; SL[i] = SL[j]; SL[j] = t }
    for (i = 1; i <= ns && i <= 5; i++)
        printf "  %5.1f%% %s\n", 100 * CNT[SL[i]] / ticks, cut(SL[i], 100)
    n2 = 0; for (g in CNT2) SL2[++n2] = g
    for (i = 1; i <= n2; i++) for (j = i + 1; j <= n2; j++)
        if (CNT2[SL2[j]] > CNT2[SL2[i]]) { t = SL2[i]; SL2[i] = SL2[j]; SL2[j] = t }
    printf "  not iowait (D, no io_schedule): %.1f%% of samples%s\n", 100 * nd / ticks, \
        (n2 > 0) ? "; top: " cut(SL2[1], 64) : ""
    print "== blocked-task signatures, % of samples, example stack" >> det
    for (i = 1; i <= ns && i <= 25; i++)
        printf "io %6.2f%% %s\n    %s\n", 100 * CNT[SL[i]] / ticks, SL[i], EX[SL[i]] >> det
    for (i = 1; i <= n2 && i <= 10; i++)
        printf "D  %6.2f%% %s\n    %s\n", 100 * CNT2[SL2[i]] / ticks, SL2[i], EX2[SL2[i]] >> det
}'

POOL_AWK='
function ssort(A, n,  g, i, j, t) {
    for (g = int(n / 2); g > 0; g = int(g / 2))
        for (i = g + 1; i <= n; i++) {
            t = A[i]
            for (j = i; j > g && A[j - g] > t; j -= g) A[j] = A[j - g]
            A[j] = t
        }
}
function msv(x) { return (x == "") ? "-" : sprintf((x < 1e6) ? "%.2f" : "%.1f", x / 1e6) }
function media(p, c) {
    if (LN[p, c] == 0) return "-"
    if (RN[p, c] == LN[p, c]) return "HDD"
    if (R0[p, c] == LN[p, c]) return "SSD"
    return (RN[p, c] + R0[p, c] == LN[p, c]) ? "mixed" : "?"
}
function wsum(p, k, v, w) { if (v != "-" && v != "") { WS[p, k] += v * w; WW[p, k] += w } }
function wavg(p, k) { return (WW[p, k] > 0) ? msv(WS[p, k] / WW[p, k]) : "-" }
function msum(p, k, v) { if (v != "-" && v != "") { MS[p, k] += v; MN[p, k]++ } }
function mavg(p, k) { return (MN[p, k] > 0) ? msv(MS[p, k] / MN[p, k]) : "-" }
function txgrow(p, a, o,  t) {
    t = a[o] + 0
    if ((p, t) in TST) { } else TXL[p, ++TXN[p]] = t
    TST[p, t] = a[o + 2]; TND[p, t] = a[o + 3]; TNW[p, t] = a[o + 5]
    TO[p, t] = a[o + 8]; TW[p, t] = a[o + 10]; TS[p, t] = a[o + 11]
}
function histo(f, tag,  l, a, n, cur, i) {
    cur = ""
    while ((getline l < f) > 0) {
        if (l == "") continue
        n = split(l, a, "\t")
        if (n == 1) { cur = a[1]; continue }
        if (cur == "" || (cur in ISP) == 0) continue
        if (((tag, a[1]) in HBK) == 0) { HBK[tag, a[1]] = 1; HBL[tag, ++HBN[tag]] = a[1] }
        for (i = 2; i <= n; i++) H[tag, cur, a[1], i] = num(a[i])
    }
    close(f)
}
function hd(p, b, i) { return H["e", p, b, i] - H["s", p, b, i] }
function hpct(p, i, q,  j, b, tot, c) {
    tot = 0
    for (j = 1; j <= HBN["we"]; j++) { b = HBL["we", j]; tot += Hw(p, b, i) }
    if (tot <= 0) return "-"
    c = 0
    for (j = 1; j <= HBN["we"]; j++) {
        b = HBL["we", j]; c += Hw(p, b, i)
        if (c >= q * tot) return sprintf("%.0f", (b + 1) / 1e6)
    }
    return "-"
}
function Hw(p, b, i) { return H["we", p, b, i] - H["ws", p, b, i] }
function Hr(p, b, i) { return H["re", p, b, i] - H["rs", p, b, i] }
function ds(f, tag,  l, a, n, i) {
    while ((getline l < f) > 0) {
        n = split(l, a, " "); if (n < 14) continue
        for (i = 4; i <= n; i++) DS[tag, a[3], i] = a[i]
        DSN[tag, a[3]] = n
    }
    close(f)
}
function dd(dev, i) { return DS["e", dev, i] - DS["s", dev, i] }
function objs(f, tag,  l, a, n, i, kv) {
    while ((getline l < f) > 0) {
        n = split(l, a, " ")
        for (i = 4; i <= n; i++) {
            split(a[i], kv, "=")
            if (kv[1] == "zil_commit_count") { ZC[tag, a[1], a[2]] = kv[2]; ZK[a[1], a[2]] = 1 }
        }
    }
    close(f)
}
BEGIN {
    dur = dur_s()
    f = d "/pools.txt"
    while ((getline l < f) > 0) {
        split(l, a, "\t"); p = a[1]; if (p == "") continue
        PL[++np] = p; ISP[p] = 1; FRAG[p] = a[5]; CAP[p] = a[6]; HEALTH[p] = a[8]
    }
    close(f)
    if (np == 0) { print "Pools: none listed (zpool missing or no pools)"; exit }
    f = d "/zpool-get.txt"
    while ((getline l < f) > 0) { split(l, a, "\t"); PROP[a[1], a[2]] = a[3] }
    close(f)
    f = d "/scan.tsv"
    while ((getline l < f) > 0) { split(l, a, "\t"); SCAN[a[1]] = a[2] }
    close(f)
    for (i = 1; i <= np; i++) {
        p = PL[i]; f = d "/vdev-ashift-" p ".txt"; s = ""
        while ((getline l < f) > 0) {
            split(l, a, "\t"); v = a[3]
            if (v == "" || v == "-" || v == "0" || ((p, v) in AS)) continue
            AS[p, v] = 1; s = s (s == "" ? "" : "/") v
        }
        close(f)
        ASH[p] = (s != "") ? s : (((p, "ashift") in PROP) ? PROP[p, "ashift"] : "-")
    }
    f = d "/leaves.tsv"
    while ((getline l < f) > 0) {
        split(l, a, "\t"); p = a[1]; c = a[2]; t = a[3]; dev = a[6]; rot = a[7]
        LN[p, c]++; if (rot == "1") RN[p, c]++; else if (rot == "0") R0[p, c]++
        if (c == "normal") {
            if (((p, t) in TOPW) == 0) TOPL[p, ++TOPN[p]] = t
            TOPW[p, t]++
        }
        if (rot == "1" && dev != "-" && ((p, dev) in HDDSEEN) == 0) {
            HDDSEEN[p, dev] = 1; HDD[p, ++HDDN[p]] = dev
        }
        LEAFDEV[a[4]] = dev
    }
    close(f)
    f = d "/iostat-pool.txt"
    while ((getline l < f) > 0) {
        n = split(l, a, "\t"); if (n < 18) continue
        p = a[1]; IN[p]++
        IR[p] += num(a[4]); IW[p] += num(a[5]); IRB[p] += num(a[6]); IWB[p] += num(a[7])
        wsum(p, "twr", a[8], num(a[4])); wsum(p, "tww", a[9], num(a[5]))
        wsum(p, "dwr", a[10], num(a[4])); wsum(p, "dww", a[11], num(a[5]))
        msum(p, "sqw", a[13]); msum(p, "aqw", a[15])
        q = (n >= 32) ? 19 : 18
        QS[p] += num(a[q]) + num(a[q + 1]) + num(a[q + 2]) + num(a[q + 3])
        QA[p] += num(a[q + 4]) + num(a[q + 5]) + num(a[q + 6]) + num(a[q + 7])
    }
    close(f)
    f = d "/txgs-samples.txt"
    while ((getline l < f) > 0) {
        n = split(l, a, " "); if (n < 14 || a[3] == "txg") continue
        p = a[2]
        if ((p in FTS) == 0) FTS[p] = a[1]
        if (a[1] == FTS[p] && a[3] + 0 > TB[p]) TB[p] = a[3] + 0
        txgrow(p, a, 3)
    }
    close(f)
    for (i = 1; i <= np; i++) {
        p = PL[i]; f = d "/end/pool-" p "-txgs"
        while ((getline l < f) > 0) {
            n = split(l, a, " "); if (n < 12 || a[1] == "txg") continue
            if (p in FTS) txgrow(p, a, 1)
        }
        close(f)
    }
    histo(d "/start/iostat-w.txt", "ws"); histo(d "/end/iostat-w.txt", "we")
    histo(d "/start/iostat-r.txt", "rs"); histo(d "/end/iostat-r.txt", "re")
    ds(d "/start/diskstats", "s"); ds(d "/end/diskstats", "e")
    objs(d "/start/objsets.txt", "s"); objs(d "/end/objsets.txt", "e")
    for (k in ZK) {
        split(k, a, SUBSEP)
        if ((("s", a[1], a[2]) in ZC) && (("e", a[1], a[2]) in ZC)) {
            ZP[a[1]] += ZC["e", a[1], a[2]] - ZC["s", a[1], a[2]]; ZPH[a[1]] = 1
        }
    }
    for (i = 1; i <= np; i++) {
        p = PL[i]
        lay = ""; nl = 0
        for (j = 1; j <= TOPN[p]; j++) {
            t = TOPL[p, j]; ty = t; sub(/-[0-9]+$/, "", ty)
            if (substr(ty, 1, 1) == "/") ty = "disk"
            k = ty "(" TOPW[p, t] ")"
            if (((p, k) in LAYC) == 0) LAYO[p, ++nl] = k
            LAYC[p, k]++
        }
        for (j = 1; j <= nl; j++) {
            k = LAYO[p, j]
            lay = lay (lay == "" ? "" : "+") (LAYC[p, k] > 1 ? LAYC[p, k] "x" : "") k
        }
        med = media(p, "normal")
        cl = ""
        split("logs special dedup cache spares", CL, " ")
        for (j = 1; j <= 5; j++) {
            c = CL[j]
            if (LN[p, c] > 0)
                cl = cl (cl == "" ? "" : " ") c " " LN[p, c] "x" media(p, c)
        }
        nn = IN[p]
        ro = (nn > 0) ? IR[p] / nn : 0; wo = (nn > 0) ? IW[p] / nn : 0
        nt = 0; ss = 0; so = 0; snd = 0; snw = 0; sw = 0
        for (j = 1; j <= TXN[p]; j++) {
            t = TXL[p, j]
            if (t <= TB[p] || TST[p, t] != "C") continue
            nt++; ss += TS[p, t]; so += TO[p, t]; snd += TND[p, t]; snw += TNW[p, t]; sw += TW[p, t]
            SV[nt] = TS[p, t] + 0
        }
        busy = (dur > 0 && nt > 0) ? 100 * ss / 1e9 / dur : 0
        idle = (ro + wo < 1 && busy < 0.5)
        printf "Pool %s (%s): %s %s | ashift %s autotrim %s | frag %s%% cap %s%% | %s | scan %s%s\n", \
            p, HEALTH[p], (lay == "" ? "-" : lay), med, ASH[p], \
            ((p, "autotrim") in PROP) ? PROP[p, "autotrim"] : "-", FRAG[p], CAP[p], \
            (cl == "" ? "no log/special/cache" : cl), (p in SCAN) ? SCAN[p] : "-", \
            idle ? " | idle in the run" : ""
        if (idle) continue
        printf "  r %.0f/s %.1fMB/s w %.0f/s %.1fMB/s | ms r/w total %s/%s disk %s/%s syncq-w %s asyncq-w %s | q sync %.1f async %.1f\n", \
            ro, (nn ? IRB[p] / nn / 1e6 : 0), wo, (nn ? IWB[p] / nn / 1e6 : 0), \
            wavg(p, "twr"), wavg(p, "tww"), wavg(p, "dwr"), wavg(p, "dww"), \
            mavg(p, "sqw"), mavg(p, "aqw"), (nn ? QS[p] / nn : 0), (nn ? QA[p] / nn : 0)
        if (nt > 0) {
            ssort(SV, nt); k = int(0.95 * nt + 0.999999); if (k < 1) k = 1; if (k > nt) k = nt
            tx = sprintf("txg %d: sync %.2fs p95 %.2fs busy %.1f%% open %.1fs dirty %.0fMB", \
                nt, ss / nt / 1e9, SV[k] / 1e9, busy, so / nt / 1e9, snd / nt / 1e6)
            printf "%-8s txgs %d sync_mean_s %.3f p95_s %.3f busy_pct %.2f open_mean_s %.2f wait_mean_s %.3f dirty_mean_MB %.1f written_mean_MB %.1f\n", \
                p, nt, ss / nt / 1e9, SV[k] / 1e9, busy, so / nt / 1e9, sw / nt / 1e9, snd / nt / 1e6, snw / nt / 1e6 >> det
        } else tx = "txg: no completed txg seen (zfs_txg_history 0?)"
        sy = 0; as = 0; rtot = 0; scr = 0; delete RB
        for (j = 1; j <= HBN["re"]; j++) {
            b = HBL["re", j]
            sy += Hr(p, b, 4) + Hr(p, b, 5); as += Hr(p, b, 8) + Hr(p, b, 9)
            c = Hr(p, b, 2) + Hr(p, b, 3) + Hr(p, b, 6) + Hr(p, b, 7)
            scr += Hr(p, b, 10) + Hr(p, b, 11)
            rtot += c
            g = (b + 0 <= 4096) ? "<=4K" : (b + 0 >= 1048576) ? ">=1M" : hsize(b + 0)
            RB[g] += c
        }
        f = d "/start/pool-" p "-dmu_tx_assign"; delete A0; delete A1
        n0 = kstat(f, A0); n1 = kstat(d "/end/pool-" p "-dmu_tx_assign", A1)
        at = 0; al = 0
        for (k in A1) { x = A1[k] - num(A0[k]); at += x; if (k + 0 > 1048576) al += x }
        printf "  %s | sync writes %s%% ZIL %s/s | assign>1ms %s%%\n", \
            tx, pct(sy, sy + as), ((p in ZPH) && dur > 0) ? f1(ZP[p] / dur) : "-", \
            (at > 0) ? sprintf("%.2f", 100 * al / at) : "-"
        s = ""
        split("<=4K 8K 16K 32K 64K 128K 256K 512K >=1M", G, " ")
        for (j = 1; j <= 9; j++) s = s sprintf(" %s %.0f", G[j], (rtot > 0) ? 100 * RB[G[j]] / rtot : 0)
        printf "  reads/s %s by size %%:%s | scrub %s%% | p99 ms disk r %s w %s\n", \
            (dur > 0) ? sprintf("%.0f", rtot / dur) : "-", s, \
            (rtot + scr > 0) ? sprintf("%.0f", 100 * scr / (rtot + scr)) : "-", \
            hpct(p, 4, 0.99), hpct(p, 5, 0.99)
        printf "%-8s p50/p99_ms total_r %s/%s total_w %s/%s disk_r %s/%s disk_w %s/%s syncq_w %s/%s asyncq_w %s/%s leaf_sync_w %d async_w %d\n", \
            p, hpct(p, 2, 0.5), hpct(p, 2, 0.99), hpct(p, 3, 0.5), hpct(p, 3, 0.99), \
            hpct(p, 4, 0.5), hpct(p, 4, 0.99), hpct(p, 5, 0.5), hpct(p, 5, 0.99), \
            hpct(p, 7, 0.5), hpct(p, 7, 0.99), hpct(p, 9, 0.5), hpct(p, 9, 0.99), sy, as >> det
        if (HDDN[p] > 0 && dur > 0) {
            fl = 0; flms = 0; m = 0
            for (j = 1; j <= HDDN[p]; j++) {
                dev = HDD[p, j]
                if ((("e", dev) in DSN) == 0 || (("s", dev) in DSN) == 0) continue
                r = dd(dev, 4); w = dd(dev, 8)
                ra = (r > 0) ? dd(dev, 7) / r : 0; wa = (w > 0) ? dd(dev, 11) / w : 0
                ut = 100 * dd(dev, 13) / (dur * 1000)
                if (DSN["e", dev] >= 20) { fl += dd(dev, 19); flms += dd(dev, 20) }
                HE[++m] = sprintf("%s %.0f/%.0f %.0f%%", dev, ra, wa, ut)
                printf "%-8s %-6s r/s %7.1f w/s %7.1f r_await %7.1f w_await %7.1f util %5.1f flush/s %6.2f\n", \
                    p, dev, r / dur, w / dur, ra, wa, ut, (DSN["e", dev] >= 20) ? dd(dev, 19) / dur : 0 >> det
            }
            s = sprintf("  HDD r/w await ms, util (flush %.1f/s/disk @%sms):", fl / dur / HDDN[p], \
                (fl > 0) ? sprintf("%.0f", flms / fl) : "-")
            for (j = 1; j <= m; j++) {
                s = s "  " HE[j]
                if (j == m || (j - 4) % 6 == 0) { print s; s = "   " }
            }
        }
    }
    f = d "/iostat-vdev.txt"
    while ((getline l < f) > 0) {
        n = split(l, a, "\t"); if (n < 18 || substr(a[1], 1, 1) != "/") continue
        v = a[1]; VN[v]++; VR[v] += num(a[4]); VW[v] += num(a[5])
        wsum(v, "dwr", a[10], num(a[4])); wsum(v, "dww", a[11], num(a[5]))
    }
    close(f)
    print "== leaf vdevs from zpool iostat -v (ops/s r w, disk_wait ms r w)" >> det
    for (v in VN)
        printf "%-60s %-8s %8.1f %8.1f %8s %8s\n", v, (v in LEAFDEV) ? LEAFDEV[v] : "-", \
            VR[v] / VN[v], VW[v] / VN[v], wavg(v, "dwr"), wavg(v, "dww") >> det
}'

DS_AWK='
function load(f, tag,  l, a, n, i, kv, k) {
    while ((getline l < f) > 0) {
        n = split(l, a, " "); if (n < 3) continue
        k = a[1] SUBSEP a[2]; NM[k] = a[3]; K[tag, k] = 1
        for (i = 4; i <= n; i++) { split(a[i], kv, "="); V[tag, k, kv[1]] = kv[2] }
    }
    close(f)
}
function dv(k, f) { return num(V["e", k, f]) - num(V["s", k, f]) }
BEGIN {
    dur = dur_s()
    nt = split(tv, tl, " "); for (i = 1; i <= nt; i++) TV[tl[i]] = 1
    load(d "/start/objsets.txt", "s"); load(d "/end/objsets.txt", "e")
    f = d "/datasets.tsv"; n = 0
    while ((getline l < f) > 0) {
        m = split(l, a, "\t")
        if (++n == 1) { for (i = 1; i <= m; i++) COL[a[i]] = i; continue }
        PR[a[1]] = l
    }
    close(f)
    nk = 0
    for (k in NM) if ((("s", k) in K) && (("e", k) in K)) { L[++nk] = k; OPS[k] = dv(k, "reads") + dv(k, "writes") }
    if (nk == 0 || dur <= 0) { print "Datasets: no per-dataset kstats (objset-*) found"; exit }
    for (i = 1; i <= nk; i++) for (j = i + 1; j <= nk; j++)
        if (OPS[L[j]] > OPS[L[i]]) { t = L[i]; L[i] = L[j]; L[j] = t }
    printf "Datasets by logical ops (top 10 of %d): w ops/s MB/s | r ops/s MB/s | ZIL commits/s\n", nk
    print "== datasets (w/s wMB/s r/s rMB/s zil_commit/s zil_itx/s name props)" >> det
    for (i = 1; i <= nk; i++) {
        k = L[i]; nm = NM[k]; g = ""
        if (match(nm, /(vm|subvol|base|basevol)-[0-9]+-/)) {
            g = substr(nm, RSTART, RLENGTH - 1); sub(/^[a-z]+-/, "", g)
            g = (nm ~ /subvol-/ ? "CT" : "VM") g ((g in TV) ? "*" : "")
        }
        pr = ""
        if (nm in PR) {
            split(PR[nm], a, "\t")
            if (a[COL["type"]] == "volume") pr = "vbs " hsize(a[COL["volblocksize"]] + 0)
            else pr = "rs " hsize(a[COL["recordsize"]] + 0)
            s = a[COL["sync"]]; sub(/\*$/, "", s); if (s != "standard" && s != "-") pr = pr " sync=" s
            s = a[COL["logbias"]]; sub(/\*$/, "", s); if (s != "latency" && s != "-") pr = pr " " s
        }
        zc = ((("e", k, "zil_commit_count") in V)) ? sprintf("%.1f", dv(k, "zil_commit_count") / dur) : "-"
        if (i <= 10)
            printf "  %-30s %-7s w %7.1f %6.2f | r %7.1f %6.2f | zc %5s  %s\n", cut(nm, 30), g, \
                dv(k, "writes") / dur, dv(k, "nwritten") / dur / 1e6, dv(k, "reads") / dur, \
                dv(k, "nread") / dur / 1e6, zc, pr
        if (i <= 60)
            printf "%9.1f %8.2f %9.1f %8.2f %7s %7.1f %s %s\n", dv(k, "writes") / dur, \
                dv(k, "nwritten") / dur / 1e6, dv(k, "reads") / dur, dv(k, "nread") / dur / 1e6, \
                zc, dv(k, "zil_itx_count") / dur, nm, pr >> det
    }
}'

KS_AWK='
function dk(k) { return num(E[k]) - num(S[k]) }
function rate(k) { return (dur > 0) ? sprintf("%.1f", dk(k) / dur) : "-" }
BEGIN {
    dur = dur_s()
    if (kstat(d "/start/dmu_tx", S) && kstat(d "/end/dmu_tx", E)) {
        s = ""
        split("delay dirty_throttle dirty_delay dirty_over_max dirty_frees_delay wrlog_delay memory_reserve memory_reclaim suspended error quota", F, " ")
        for (i = 1; i <= 11; i++)
            if ((("dmu_tx_" F[i]) in E) && (i <= 4 || dk("dmu_tx_" F[i]) != 0))
                s = s " " F[i] " " dk("dmu_tx_" F[i])
        printf "dmu_tx: assigned %s/s | deltas (others 0):%s\n", rate("dmu_tx_assigned"), s
    } else print "dmu_tx: kstat not available"
    delete S; delete E
    if (kstat(d "/start/zil", S) && kstat(d "/end/zil", E)) {
        printf "ZIL: commit %s/s writer %s/s | itx %s/s indirect %s copied %s needcopy %s | lwb MB/s normal %s slog %s\n", \
            rate("zil_commit_count"), rate("zil_commit_writer_count"), rate("zil_itx_count"), \
            rate("zil_itx_indirect_count"), rate("zil_itx_copied_count"), rate("zil_itx_needcopy_count"), \
            (dur > 0) ? sprintf("%.2f", dk("zil_itx_metaslab_normal_bytes") / dur / 1e6) : "-", \
            (dur > 0) ? sprintf("%.2f", dk("zil_itx_metaslab_slog_bytes") / dur / 1e6) : "-"
    } else print "ZIL: kstat not available"
    delete S; delete E
    if (kstat(d "/start/arcstats", S) && kstat(d "/end/arcstats", E)) {
        h = dk("hits"); io = dk("iohits"); m = dk("misses")
        l2 = (("l2_hits" in E) && dk("l2_hits") + dk("l2_misses") > 0) ? pct(dk("l2_hits"), dk("l2_hits") + dk("l2_misses")) "%" : "-"
        printf "ARC %.1f/%.1f/%.1f GiB size/c/max | hit %s%% | miss/s data %s meta %s pf-data %s pf-meta %s | L2 hit %s | throttle +%d\n", \
            num(E["size"]) / 1073741824, num(E["c"]) / 1073741824, num(E["c_max"]) / 1073741824, \
            pct(h, h + io + m), rate("demand_data_misses"), rate("demand_metadata_misses"), \
            rate("prefetch_data_misses"), rate("prefetch_metadata_misses"), l2, dk("memory_throttle_count")
    } else print "ARC: arcstats not available"
}'

summarize() {
    local det=$OUT/summary-detail.txt sum=$OUT/summary.txt
    : >"$det"
    {
        "$AWK" -v d="$OUT" "$COMMON_AWK$HEAD_AWK"
        "$AWK" -v d="$OUT" -v det="$det" -v tv="$TEST_VMS" "$COMMON_AWK$CG_AWK"
        "$AWK" -v d="$OUT" -v det="$det" "$COMMON_AWK$STACKSUM_AWK"
        "$AWK" -v d="$OUT" -v det="$det" "$COMMON_AWK$POOL_AWK"
        "$AWK" -v d="$OUT" -v det="$det" -v tv="$TEST_VMS" "$COMMON_AWK$DS_AWK"
        "$AWK" -v d="$OUT" "$COMMON_AWK$KS_AWK"
    } >"$sum"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

note "start: duration ${DURATION}s interval ${INTERVAL}s stacks ${STACK_INTERVAL}s root '${P}'"
snapshot start
now
T0=$NOW
sample_once "$T0"
start_background
collect_identity
collect_params
collect_pools
collect_leaves
collect_datasets
collect_guests

for ((i = 1; i <= COUNT; i++)); do
    now
    t=$((T0 + i * INTERVAL))
    [ "$t" -gt "$NOW" ] && sleep "$((t - NOW))"
    now
    sample_once "$NOW"
done

wait_background
snapshot end
summarize
note "collector cpu time (user/system, shell then children):"
times
nnotes=$(grep -vc '^$' "$OUT/notes.log")
tarball=$OUT.tar.gz
if ! tar -czf "$tarball" -C "$OUTBASE" "${OUT##*/}"; then
    note "tar failed"
    tarball="(tar failed; files left in $OUT)"
fi
{
    cat "$OUT/summary.txt"
    echo "Notes: $nnotes lines in notes.log; full tables in summary-detail.txt"
    echo "Tarball: $tarball"
} >&3
exit 0
