#!/usr/bin/env bash
# SPDX-License-Identifier: CDDL-1.0
#
# perf-xpool experiments, run as root by .github/workflows/perf-xpool.yml
# once the freshly built modules are loaded:
#
#   q1  per-I/O cost of ZFS on zero-latency (memory-backed null_blk)
#       devices against the raw device, plus perf profiles of ZFS runs.
#   qz  zvol rows of q1 under several module-parameter variants
#       (QZ_VARIANTS), interleaved on one runner, with context switches
#       per I/O and perf profiles of selected variants.
#   q2  a fast mirror pool on null_blk beside a slow raidz2 pool on
#       dm-delay over bandwidth-limited null_blk: the fast pool's 4K
#       latency alone, while the slow pool streams writes, then reads,
#       and alone again, with off-CPU stacks, function latencies and
#       kstats.  Q2_DIRTY_MAX sets zfs_dirty_data_max.
#
# It creates null_blk and device-mapper devices, pools and datasets, and
# rewrites module parameters: run it only on a disposable machine.

set -u -o pipefail

EXP=${1:?usage: run.sh q1|q2|qz}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/var/tmp/perf-xpool/$EXP}
WORK=${WORK:-/var/tmp/perf-xpool-work/$EXP}
REPS=${REPS:-3}
Q1_RUNTIME=${Q1_RUNTIME:-15}
Q2_RUNTIME=${Q2_RUNTIME:-30}
QZ_RUNTIME=${QZ_RUNTIME:-8}
QZ_REPS=${QZ_REPS:-2}
# NAME[:PARAM=VALUE[,PARAM=VALUE...]]; every parameter named in any
# variant is reset to its load-time value before each variant applies its
# own.
QZ_VARIANTS=${QZ_VARIANTS:-"def rs:zvol_request_sync=1 mq:zvol_use_blk_mq=1 \
cti:zfs_vdev_disk_calling_thread_io=1 \
rscti:zvol_request_sync=1,zfs_vdev_disk_calling_thread_io=1 \
mqrscti:zvol_use_blk_mq=1,zvol_request_sync=1,zfs_vdev_disk_calling_thread_io=1"}
QZ_PROFILE=${QZ_PROFILE:-"def rscti"}
DIAG_RUNTIME=${DIAG_RUNTIME:-20}
SIZE_MB=${SIZE_MB:-512}
ARC_MAX=${ARC_MAX:-$((2 << 30))}
Q2_DIRTY_MAX=${Q2_DIRTY_MAX:-$((1536 << 20))}
FAST_DEV_MB=${FAST_DEV_MB:-2048}
SLOW_NDEV=${SLOW_NDEV:-6}
SLOW_DEV_MB=${SLOW_DEV_MB:-1024}
SLOW_DELAY_MS=${SLOW_DELAY_MS:-8}
SLOW_MBPS=${SLOW_MBPS:-100}
SLOW_WRITE_MB=${SLOW_WRITE_MB:-256}
SLOW_READ_MB=${SLOW_READ_MB:-2560}

# The fast pool's fio runs under its own name so that the tracing below
# can tell its threads from the slow pool's load generator.
FIO=fiofast
REPORT=("python3" "$HERE/report.py")
PERF=perf
ASYNC_ENGINE=io_uring
OFFCPU_BT=""
SLOW_PID=""
FIO_ARGS=()

mkdir -p "$OUT/runs" "$WORK"
exec > >(tee -a "$OUT/run.log") 2>&1

log() {
	printf '[%s] %s\n' "$(date +%T)" "$*"
}

die() {
	log "FATAL: $*"
	exit 1
}

snap() {
	head -1 /proc/stat > "$1.stat"
	grep '^ctxt ' /proc/stat > "$1.ctxt"
	cat /proc/uptime > "$1.uptime"
	cat /proc/spl/kstat/zfs/arcstats > "$1.arcstats" 2>/dev/null
}

# run_fio LABEL ARGS...: run the fast-pool fio with /proc/stat and
# arcstats snapshots around it.
run_fio() {
	local label=$1 dir rc
	shift
	dir=$OUT/runs/$label
	mkdir -p "$dir"
	if [ -n "$SLOW_PID" ] && ! kill -0 "$SLOW_PID" 2>/dev/null; then
		log "WARNING: slow-pool load is not running during $label"
		echo dead > "$dir/slow-load"
	fi
	snap "$dir/pre"
	"$FIO" --output-format=json --output="$dir/fio.json" "$@" \
	    > "$dir/fio.stderr" 2>&1
	rc=$?
	snap "$dir/post"
	echo "$rc" > "$dir/rc"
	if [ "$rc" -ne 0 ]; then
		log "fio $label failed with $rc"
		tail -5 "$dir/fio.stderr"
		return 0
	fi
	printf '%s\n' "$label"
	"${REPORT[@]}" fioline "$dir/fio.json"
}

# fio_args PATH KIND(dev|file) RW QD NJ RUNTIME: fill FIO_ARGS.  Writes
# are O_SYNC everywhere (and sync=always on the datasets), so that every
# write is durable before it returns, as on the raw device.
fio_args() {
	local path=$1 kind=$2 rw=$3 qd=$4 nj=$5 rt=$6 eng=psync
	[ "$qd" -gt 1 ] && eng=$ASYNC_ENGINE
	FIO_ARGS=(--name=t --filename="$path" --rw="$rw" --bs=4k
	    --size="${SIZE_MB}M" --ioengine="$eng" --iodepth="$qd"
	    --numjobs="$nj" --group_reporting --time_based --runtime="$rt"
	    --norandommap --randrepeat=0 --lat_percentiles=1
	    --percentile_list=50:99:99.9)
	if [ "$kind" = dev ]; then
		FIO_ARGS+=(--direct=1)
	else
		FIO_ARGS+=(--direct=0)
	fi
	[ "$rw" = randwrite ] && FIO_ARGS+=(--sync=1)
}

prefill() {
	fio --name=prefill --filename="$1" --rw=write --bs=1M \
	    --size="${SIZE_MB}M" --direct="$2" --end_fsync=1 \
	    --ioengine=psync > "$WORK/prefill.log" 2>&1 ||
	    die "prefill of $1 failed"
}

# nullb_add NAME INDEX SIZE_MB [ATTR=VALUE...]: a memory-backed null_blk
# device created through configfs, so that it keeps what is written.
nullb_add() {
	local name=$1 idx=$2 size=$3 d kv
	shift 3
	d=/sys/kernel/config/nullb/$name
	mkdir "$d" || return 1
	echo "$idx" > "$d/index" 2>/dev/null
	echo "$size" > "$d/size" || return 1
	echo 4096 > "$d/blocksize" || return 1
	echo 1 > "$d/memory_backed" || return 1
	echo 1 > "$d/irqmode"
	nproc > "$d/submit_queues"
	echo 256 > "$d/hw_queue_depth"
	for kv in "$@"; do
		echo "${kv#*=}" > "$d/${kv%%=*}" ||
		    { log "cannot set ${kv%%=*} on $name"; return 1; }
	done
	echo 1 > "$d/power" || return 1
	udevadm settle
	[ -b "/dev/$name" ] || { log "/dev/$name did not appear"; return 1; }
}

# zvol_dev DATASET: the block device of a zvol, once udev has made it.
zvol_dev() {
	local i dev
	for i in $(seq 1 60); do
		if [ -b "/dev/zvol/$1" ]; then
			echo "/dev/zvol/$1"
			return 0
		fi
		for dev in /dev/zd*; do
			[ -b "$dev" ] || continue
			if [ "$(/usr/lib/udev/zvol_id "$dev" 2>/dev/null)" = "$1" ]
			then
				echo "$dev"
				return 0
			fi
		done
		udevadm settle
		sleep 0.5
	done
	return 1
}

# pool_cycle POOL: export and import, which drops the pool's data from
# the ARC so that the read runs below reach the device.
pool_cycle() {
	zpool export "$1" || die "cannot export $1"
	zpool import -d /dev "$1" || die "cannot import $1"
}

idle_baseline() {
	snap "$OUT/idle.pre"
	sleep 10
	snap "$OUT/idle.post"
}

perf_report() {
	local data=$WORK/$1.data dir=$2
	mkdir -p "$dir"
	"$PERF" report -i "$data" --stdio --no-children --sort sym -g none \
	    --percent-limit 0.01 > "$dir/$1.self.txt" 2>/dev/null
	"$PERF" report -i "$data" --stdio --children --sort sym -g none \
	    --percent-limit 0.3 > "$dir/$1.children.txt" 2>/dev/null
	"$PERF" report -i "$data" --stdio --no-children --sort comm -g none \
	    > "$dir/$1.comm.txt" 2>/dev/null
	"$PERF" report -i "$data" --stdio --no-children --sort sym \
	    -g graph,1,caller --percent-limit 2 \
	    > "$dir/$1.callers.txt" 2>/dev/null
	rm -f "$data"
}

common_setup() {
	local p
	install -m 0755 "$(command -v fio)" "/usr/local/bin/$FIO" ||
	    die "cannot install $FIO"
	sysctl -q -w kernel.kptr_restrict=0 kernel.perf_event_paranoid=-1
	if ! perf --version > /dev/null 2>&1; then
		for p in /usr/lib/linux-tools/*/perf; do
			[ -x "$p" ] && PERF=$p
		done
	fi
	modprobe configfs
	mountpoint -q /sys/kernel/config ||
	    mount -t configfs none /sys/kernel/config
	modprobe null_blk nr_devices=0 || die "null_blk is not available"
	[ -d /sys/kernel/config/nullb ] || die "null_blk has no configfs"
	echo "$ARC_MAX" > /sys/module/zfs/parameters/zfs_arc_max ||
	    die "cannot set zfs_arc_max"
	sync
	echo 3 > /proc/sys/vm/drop_caches
	{
		uname -a
		lscpu
		free -m
		echo "zfs $(cat /sys/module/zfs/version)"
		fio --version
		bpftrace --version 2>&1 | head -1
		"$PERF" --version 2>&1
		echo "null_blk features: $(cat /sys/kernel/config/nullb/features)"
	} > "$OUT/system.txt" 2>&1
	if ! fio --name=chk --filename=/dev/zero --rw=read --bs=4k \
	    --size=64k --ioengine=io_uring --iodepth=2 > /dev/null 2>&1; then
		ASYNC_ENGINE=libaio
	fi
	echo "QD>1 engine: $ASYNC_ENGINE" > "$OUT/engine.txt"
	for p in "$HERE/offcpu.bt" "$HERE/offcpu-tp.bt"; do
		timeout -s INT 10 bpftrace "$p" > "$WORK/bt-check.txt" 2>&1
		if grep -q elapsed_ms "$WORK/bt-check.txt"; then
			OFFCPU_BT=$p
			break
		fi
		log "bpftrace $(basename "$p") unusable:"
		tail -3 "$WORK/bt-check.txt"
	done
	echo "off-CPU program: ${OFFCPU_BT:-none}" >> "$OUT/engine.txt"
}

dump_params() {
	local p
	for p in zfs_arc_max zfs_arc_min zfs_dirty_data_max \
	    zfs_dirty_data_max_max zfs_dirty_data_sync_percent \
	    zfs_delay_min_dirty_percent zfs_arc_dirty_limit_percent \
	    zfs_arc_anon_limit_percent zfs_arc_pool_dirty_percent \
	    zfs_arc_eviction_pct zfs_arc_overflow_shift zfs_txg_timeout \
	    zfs_vdev_aggregation_limit_non_rotational zio_taskq_batch_pct \
	    zfs_arc_evict_threads zvol_request_sync zvol_use_blk_mq \
	    zfs_vdev_disk_calling_thread_io zvol_threads zvol_num_taskqs \
	    zvol_inline_io zfs_txg_history; do
		[ -r "/sys/module/zfs/parameters/$p" ] &&
		    echo "$p $(cat "/sys/module/zfs/parameters/$p")"
	done
}

# ------------------------------------------------------------------ q1 --

q1_runs() {
	local tag=$1 path=$2 kind=$3 rw=$4 rep=$5 qn qd nj
	for qn in 1:1 1:4 32:1 32:4; do
		qd=${qn%:*}
		nj=${qn#*:}
		fio_args "$path" "$kind" "$rw" "$qd" "$nj" "$Q1_RUNTIME"
		run_fio "q1.$tag.$rw.qd$qd.nj$nj.r$rep" "${FIO_ARGS[@]}"
	done
}

q1_profile() {
	local tag=$1 path=$2 kind=$3 rw=$4 qd=$5 nj=$6 label fpid
	label=q1prof.$tag.$rw.qd$qd.nj$nj
	fio_args "$path" "$kind" "$rw" "$qd" "$nj" "$Q1_RUNTIME"
	run_fio "$label" "${FIO_ARGS[@]}" &
	fpid=$!
	sleep 3
	"$PERF" record -a -g -F 1999 -e cpu-clock -o "$WORK/$label.data" \
	    -- sleep 8 > "$WORK/$label.perf.log" 2>&1 ||
	    { log "perf record failed:"; tail -3 "$WORK/$label.perf.log"; }
	wait "$fpid"
	perf_report "$label" "$OUT/perf"
}

q1() {
	local rep p v1 vm
	common_setup
	nullb_add nullb0 0 1024 || die "nullb0"
	nullb_add nullb1 1 "$FAST_DEV_MB" || die "nullb1"
	nullb_add nullb2 2 "$FAST_DEV_MB" || die "nullb2"
	nullb_add nullb3 3 "$FAST_DEV_MB" || die "nullb3"
	zpool create -f -o ashift=12 -O atime=off -O primarycache=metadata \
	    -O sync=always fast1 /dev/nullb1 || die "zpool create fast1"
	zpool create -f -o ashift=12 -O atime=off -O primarycache=metadata \
	    -O sync=always fastm mirror /dev/nullb2 /dev/nullb3 ||
	    die "zpool create fastm"
	for p in fast1 fastm; do
		zfs create -o recordsize=4K "$p/fs" || die "zfs create $p/fs"
		zfs create -V "${SIZE_MB}M" -o volblocksize=4K "$p/vol" ||
		    die "zfs create $p/vol"
	done
	prefill /dev/nullb0 1
	for p in fast1 fastm; do
		prefill "$(zvol_dev "$p/vol")" 1
		prefill "/$p/fs/f" 0
	done
	zpool sync
	dump_params > "$OUT/params.txt"
	zpool status > "$OUT/zpool-status.txt"
	zfs get -r all fast1 fastm > "$OUT/zfs-props.txt"
	idle_baseline
	for rep in $(seq 1 "$REPS"); do
		log "q1 repetition $rep"
		q1_runs raw /dev/nullb0 dev randwrite "$rep"
		q1_runs raw /dev/nullb0 dev randread "$rep"
		for p in fast1 fastm; do
			v1=f1
			[ "$p" = fastm ] && v1=fm
			q1_runs "${v1}vol" "$(zvol_dev "$p/vol")" dev randwrite "$rep"
			q1_runs "${v1}fs" "/$p/fs/f" file randwrite "$rep"
			pool_cycle "$p"
			q1_runs "${v1}vol" "$(zvol_dev "$p/vol")" dev randread "$rep"
			q1_runs "${v1}fs" "/$p/fs/f" file randread "$rep"
		done
	done
	log "q1 perf profiles"
	q1_profile raw /dev/nullb0 dev randwrite 1 1
	q1_profile raw /dev/nullb0 dev randread 1 1
	vm=$(zvol_dev fastm/vol)
	q1_profile fmfs /fastm/fs/f file randwrite 1 1
	q1_profile fmvol "$vm" dev randwrite 1 1
	q1_profile fmfs /fastm/fs/f file randwrite 32 4
	pool_cycle fastm
	vm=$(zvol_dev fastm/vol)
	q1_profile fmfs /fastm/fs/f file randread 1 1
	q1_profile fmvol "$vm" dev randread 1 1
	q1_profile fmvol "$vm" dev randread 32 4
	cat /proc/spl/kstat/zfs/arcstats > "$OUT/arcstats.end"
	zpool iostat -v > "$OUT/zpool-iostat.end"
}

# ------------------------------------------------------------------ qz --

QZ_DEFAULTS=()

qz_params() {
	local v p
	for v in $QZ_VARIANTS; do
		[ "${v#*:}" = "$v" ] && continue
		for p in ${v#*:}; do
			echo "${p%%=*}"
		done
	done | tr ',' '\n' | sed 's/=.*//' | sort -u
}

# qz_apply VARIANT: reset every parameter the variants touch, apply this
# variant's values, then export and import both pools so that a change
# of zvol_use_blk_mq reaches the zvols' block devices.
qz_apply() {
	local v=$1 kv p d
	for kv in "${QZ_DEFAULTS[@]}"; do
		echo "${kv#*=}" > "/sys/module/zfs/parameters/${kv%%=*}"
	done
	if [ "${v#*:}" != "$v" ]; then
		for kv in $(echo "${v#*:}" | tr ',' ' '); do
			echo "${kv#*=}" > "/sys/module/zfs/parameters/${kv%%=*}" ||
			    die "cannot set ${kv%%=*}"
		done
	fi
	for p in fast1 fastm; do
		pool_cycle "$p"
	done
	{
		echo "variant ${v%%:*}:"
		for p in $(qz_params); do
			echo "  $p $(cat "/sys/module/zfs/parameters/$p")"
		done
		for p in fast1 fastm; do
			d=$(basename "$(readlink -f "$(zvol_dev "$p/vol")")")
			if [ -d "/sys/block/$d/mq" ]; then
				echo "  $p/vol $d blk-mq"
			else
				echo "  $p/vol $d bio"
			fi
		done
	} >> "$OUT/variants.txt"
}

qz_variant_runs() {
	local v=$1 rep=$2 name=${1%%:*} p t rw qn qd nj
	log "qz variant $name repetition $rep"
	qz_apply "$v"
	for p in fast1 fastm; do
		t=f1vol
		[ "$p" = fastm ] && t=fmvol
		for rw in randwrite randread; do
			[ "$rw" = randread ] && pool_cycle "$p"
			for qn in 1:1 1:4 32:1 32:4; do
				qd=${qn%:*}
				nj=${qn#*:}
				fio_args "$(zvol_dev "$p/vol")" dev "$rw" "$qd" \
				    "$nj" "$QZ_RUNTIME"
				run_fio "qz.$name.$t.$rw.qd$qd.nj$nj.r$rep" \
				    "${FIO_ARGS[@]}"
			done
		done
	done
}

qz() {
	local rep v p list rev name qn
	common_setup
	nullb_add nullb0 0 1024 || die "nullb0"
	nullb_add nullb1 1 "$FAST_DEV_MB" || die "nullb1"
	nullb_add nullb2 2 "$FAST_DEV_MB" || die "nullb2"
	nullb_add nullb3 3 "$FAST_DEV_MB" || die "nullb3"
	zpool create -f -o ashift=12 -O atime=off -O primarycache=metadata \
	    -O sync=always fast1 /dev/nullb1 || die "zpool create fast1"
	zpool create -f -o ashift=12 -O atime=off -O primarycache=metadata \
	    -O sync=always fastm mirror /dev/nullb2 /dev/nullb3 ||
	    die "zpool create fastm"
	for p in fast1 fastm; do
		zfs create -V "${SIZE_MB}M" -o volblocksize=4K "$p/vol" ||
		    die "zfs create $p/vol"
	done
	prefill /dev/nullb0 1
	for p in fast1 fastm; do
		prefill "$(zvol_dev "$p/vol")" 1
	done
	zpool sync
	for p in $(qz_params); do
		[ -w "/sys/module/zfs/parameters/$p" ] ||
		    die "parameter $p is missing"
		QZ_DEFAULTS+=("$p=$(cat "/sys/module/zfs/parameters/$p")")
	done
	dump_params > "$OUT/params.txt"
	echo "$QZ_VARIANTS" > "$OUT/qz-variants.txt"
	zpool status > "$OUT/zpool-status.txt"
	idle_baseline
	list=($QZ_VARIANTS)
	for rep in $(seq 1 "$QZ_REPS"); do
		for qn in randwrite randread; do
			fio_args /dev/nullb0 dev "$qn" 1 1 "$QZ_RUNTIME"
			run_fio "qz.raw.raw.$qn.qd1.nj1.r$rep" "${FIO_ARGS[@]}"
		done
		if [ $((rep % 2)) -eq 1 ]; then
			for v in "${list[@]}"; do
				qz_variant_runs "$v" "$rep"
			done
		else
			for ((rev = ${#list[@]} - 1; rev >= 0; rev--)); do
				qz_variant_runs "${list[$rev]}" "$rep"
			done
		fi
	done
	log "qz perf profiles"
	for v in "${list[@]}"; do
		name=${v%%:*}
		case " $QZ_PROFILE " in
		*" $name "*) ;;
		*) continue ;;
		esac
		qz_apply "$v"
		q1_profile "qz$name.fmvol" "$(zvol_dev fastm/vol)" dev \
		    randwrite 1 1
		pool_cycle fastm
		q1_profile "qz$name.fmvol" "$(zvol_dev fastm/vol)" dev \
		    randread 1 1
		q1_profile "qz$name.fmvol" "$(zvol_dev fastm/vol)" dev \
		    randread 32 1
	done
	qz_apply def
	cat /proc/spl/taskq > "$OUT/taskq.end" 2>/dev/null
	zpool iostat -v > "$OUT/zpool-iostat.end"
}

# ------------------------------------------------------------------ q2 --

q2_args() {
	local rw=randread nj=${1#??}
	case $1 in
	sw*) rw=randwrite ;;
	esac
	fio_args /fastq/fs/f file "$rw" 1 "$nj" "$2"
}

phase_snap() {
	local d=$OUT/phase/$1.$2 p k
	mkdir -p "$d"
	cat /proc/uptime > "$d/uptime"
	head -1 /proc/stat > "$d/stat"
	for k in arcstats dmu_tx dbufstats zil abdstats; do
		cat "/proc/spl/kstat/zfs/$k" > "$d/$k" 2>/dev/null
	done
	for p in fastq slowq; do
		for k in txgs dmu_tx_assign iostats; do
			cat "/proc/spl/kstat/zfs/$p/$k" > "$d/$p.$k" 2>/dev/null
		done
	done
	for k in cpu memory io; do
		cat "/proc/pressure/$k" > "$d/psi.$k" 2>/dev/null
	done
	cat /proc/spl/taskq > "$d/taskq" 2>/dev/null
	cat /proc/meminfo > "$d/meminfo"
}

# Diagnostic run A: off-CPU stacks of the fiofast threads (bpftrace) and
# a /proc/<tid>/stack sampler, side by side.
q2_diag_a() {
	local label=q2diagA.$1.$2 d fpid bpid spid
	d=$OUT/diag/$label
	mkdir -p "$d"
	q2_args "$2" "$DIAG_RUNTIME"
	run_fio "$label" "${FIO_ARGS[@]}" &
	fpid=$!
	sleep 2
	bpid=""
	if [ -n "$OFFCPU_BT" ]; then
		timeout -s INT 14 bpftrace "$OFFCPU_BT" > "$d/offcpu.txt" \
		    2> "$d/offcpu.err" &
		bpid=$!
	fi
	"${REPORT[@]}" sample 12 0.005 "$FIO" "$d/procstack.json" &
	spid=$!
	[ -n "$bpid" ] && wait "$bpid"
	wait "$spid"
	wait "$fpid"
}

gen_funclat() {
	local f n=0 clr=""
	echo 'BEGIN { @t0 = nsecs; }'
	for f in zfs_write zfs_read zil_commit zil_commit_waiter \
	    dmu_tx_assign dmu_tx_wait txg_wait_open txg_wait_synced_flags \
	    arc_read arc_wait_for_eviction arc_get_data_impl dbuf_evict_one \
	    zio_wait spa_open_common; do
		awk -v f="$f" '$3 == f && $4 == "[zfs]" { found = 1; exit }
		    END { exit !found }' /proc/kallsyms || continue
		cat <<-EOF
		kprobe:$f { @s${n}[tid] = nsecs; }
		kretprobe:$f /@s${n}[tid] != 0/
		{
			\$d = (nsecs - @s${n}[tid]) / 1000;
			if (comm == "$FIO") {
				@fast_us_$f = sum(\$d);
				@fast_n_$f = count();
				@fast_max_us_$f = max(\$d);
			}
			if (\$d >= 50) {
				@slow_us_${f}[comm] = sum(\$d);
				@slow_n_${f}[comm] = count();
			}
			delete(@s${n}[tid]);
		}
		EOF
		clr="$clr clear(@s$n);"
		n=$((n + 1))
	done
	printf 'END { printf("elapsed_ms %%d\\n", (nsecs - @t0) / 1000000);\n'
	printf '    clear(@t0);%s }\n' "$clr"
}

# Diagnostic run B: latency of selected ZFS functions (bpftrace kprobes),
# a system-wide perf profile and the pools' per-vdev latencies.
q2_diag_b() {
	local label=q2diagB.$1.$2 d fpid bpid ppid ipid
	d=$OUT/diag/$label
	mkdir -p "$d"
	q2_args "$2" "$DIAG_RUNTIME"
	run_fio "$label" "${FIO_ARGS[@]}" &
	fpid=$!
	sleep 2
	timeout -s INT 14 bpftrace "$WORK/funclat.bt" > "$d/funclat.txt" \
	    2> "$d/funclat.err" &
	bpid=$!
	"$PERF" record -a -g -F 999 -e cpu-clock -o "$WORK/perf.data" \
	    -- sleep 8 > "$WORK/perf-b.log" 2>&1 &
	ppid=$!
	zpool iostat -l -y fastq slowq 8 1 > "$d/zpool-iostat.txt" 2>&1 &
	ipid=$!
	sleep 4
	cat /proc/spl/taskq > "$d/taskq.txt" 2>/dev/null
	wait "$ppid"
	wait "$ipid"
	wait "$bpid"
	wait "$fpid"
	perf_report perf "$d"
}

q2_phase() {
	local ph=$1 rep wl
	log "q2 phase $ph"
	phase_snap "$ph" start
	for rep in $(seq 1 "$REPS"); do
		pool_cycle fastq
		for wl in rr1 rr4 sw1 sw4; do
			q2_args "$wl" "$Q2_RUNTIME"
			run_fio "q2.$ph.$wl.r$rep" "${FIO_ARGS[@]}"
		done
	done
	pool_cycle fastq
	for wl in rr1 sw1; do
		q2_diag_a "$ph" "$wl"
		q2_diag_b "$ph" "$wl"
	done
	phase_snap "$ph" end
}

slow_start() {
	local name=$1
	shift
	fio --name="$name" --output-format=json \
	    --output="$OUT/slow-$name.json" "$@" > "$WORK/slow-$name.log" 2>&1 &
	SLOW_PID=$!
}

slow_stop() {
	if kill -0 "$SLOW_PID" 2>/dev/null; then
		kill -INT "$SLOW_PID"
	else
		log "WARNING: slow-pool load had exited early:"
		tail -5 "$WORK/slow-$1.log"
	fi
	wait "$SLOW_PID"
	SLOW_PID=""
}

q2() {
	local i idx sz mode=dm-delay attrs devs=()
	common_setup
	echo "$Q2_DIRTY_MAX" > /sys/module/zfs/parameters/zfs_dirty_data_max ||
	    die "cannot set zfs_dirty_data_max"
	nullb_add nullb0 0 "$FAST_DEV_MB" || die "nullb0"
	nullb_add nullb1 1 "$FAST_DEV_MB" || die "nullb1"
	modprobe dm_delay || mode=nullb-timer
	for i in $(seq 0 $((SLOW_NDEV - 1))); do
		idx=$((i + 2))
		attrs=()
		if grep -qw mbps /sys/kernel/config/nullb/features; then
			attrs+=("mbps=$SLOW_MBPS")
		fi
		if [ "$mode" = nullb-timer ]; then
			attrs+=(irqmode=2 \
			    "completion_nsec=$((SLOW_DELAY_MS * 1000000))")
		fi
		nullb_add "nullb$idx" "$idx" "$SLOW_DEV_MB" "${attrs[@]}" ||
		    die "nullb$idx"
		if [ "$mode" = dm-delay ]; then
			sz=$(blockdev --getsz "/dev/nullb$idx")
			dmsetup create "slow$i" --table \
			    "0 $sz delay /dev/nullb$idx 0 $SLOW_DELAY_MS" ||
			    die "dmsetup slow$i"
			devs+=("/dev/mapper/slow$i")
		else
			devs+=("/dev/nullb$idx")
		fi
	done
	udevadm settle
	{
		echo "slow devices: $SLOW_NDEV x ${SLOW_DEV_MB} MiB, $mode" \
		    "${SLOW_DELAY_MS} ms, null_blk limit ${attrs[*]:-none}"
		echo "fast devices: 2 x ${FAST_DEV_MB} MiB null_blk mirror"
		echo "zfs_arc_max $ARC_MAX zfs_dirty_data_max $Q2_DIRTY_MAX"
	} > "$OUT/setup.txt"
	zpool create -f -o ashift=12 -O atime=off -O primarycache=metadata \
	    -O sync=always fastq mirror /dev/nullb0 /dev/nullb1 ||
	    die "zpool create fastq"
	zfs create -o recordsize=4K fastq/fs || die "zfs create fastq/fs"
	zpool create -f -o ashift=12 -O atime=off slowq raidz2 "${devs[@]}" ||
	    die "zpool create slowq"
	zfs create slowq/data || die "zfs create slowq/data"
	prefill /fastq/fs/f 0
	zpool sync
	dump_params > "$OUT/params.txt"
	zpool status > "$OUT/zpool-status.txt"
	gen_funclat > "$WORK/funclat.bt"
	cp "$WORK/funclat.bt" "$OUT/funclat.bt"
	idle_baseline

	q2_phase alone

	slow_start slowwr --filename=/slowq/data/w --rw=write --bs=1M \
	    --size="${SLOW_WRITE_MB}M" --ioengine=psync --time_based \
	    --runtime=36000
	sleep 20
	q2_phase slowwrite
	slow_stop slowwr
	rm -f /slowq/data/w
	for i in 1 2 3 4; do
		zpool sync slowq
	done

	log "q2 writing the ${SLOW_READ_MB} MiB read file"
	fio --name=mkread --filename=/slowq/data/r --rw=write --bs=1M \
	    --size="${SLOW_READ_MB}M" --ioengine=psync --end_fsync=1 \
	    > "$WORK/mkread.log" 2>&1 || log "WARNING: read file incomplete"
	zpool sync slowq
	slow_start slowrd --filename=/slowq/data/r --rw=read --bs=1M \
	    --size=$((SLOW_READ_MB / 2))M \
	    --offset_increment=$((SLOW_READ_MB / 2))M --numjobs=2 \
	    --group_reporting --ioengine=psync --time_based --runtime=36000
	sleep 15
	q2_phase slowread
	slow_stop slowrd

	sleep 10
	q2_phase alone2
	zpool status > "$OUT/zpool-status.end"
	zpool list -v > "$OUT/zpool-list.end"
}

case $EXP in
q1) q1 ;;
q2) q2 ;;
qz) qz ;;
*) die "unknown experiment $EXP" ;;
esac
log "done"
chmod -R a+rX "$OUT"
