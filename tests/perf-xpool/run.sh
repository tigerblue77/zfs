#!/usr/bin/env bash
# SPDX-License-Identifier: CDDL-1.0
#
# perf-xpool experiments, run as root by .github/workflows/perf-xpool.yml
# once the freshly built modules are loaded:
#
#   q1  per-I/O cost of ZFS on zero-latency (memory-backed null_blk)
#       devices against the raw device, plus perf profiles of ZFS runs.
#   q2  a fast mirror pool on null_blk beside a slow raidz2 pool on
#       dm-delay over bandwidth-limited null_blk: the fast pool's 4K
#       latency alone, while the slow pool streams writes, then reads,
#       and alone again, with off-CPU stacks, function latencies and
#       kstats.  Q2_DIRTY_MAX sets zfs_dirty_data_max.
#
# It creates null_blk and device-mapper devices, pools and datasets, and
# rewrites module parameters: run it only on a disposable machine.

set -u -o pipefail

EXP=${1:?usage: run.sh q1|q2}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/var/tmp/perf-xpool/$EXP}
WORK=${WORK:-/var/tmp/perf-xpool-work/$EXP}
REPS=${REPS:-3}
Q1_RUNTIME=${Q1_RUNTIME:-15}
Q2_RUNTIME=${Q2_RUNTIME:-30}
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
FIO_PREFIX=()
SCHED_DELAY=""
SCHED_SPAN=""
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
	cat /proc/uptime > "$1.uptime"
	cat /proc/spl/kstat/zfs/arcstats > "$1.arcstats" 2>/dev/null
}

# run_fio LABEL ARGS...: run the fast-pool fio with /proc/stat and
# arcstats snapshots around it.
run_fio() {
	local label=$1 dir rc spid
	shift
	dir=$OUT/runs/$label
	mkdir -p "$dir"
	if [ -n "$SLOW_PID" ] && ! kill -0 "$SLOW_PID" 2>/dev/null; then
		log "WARNING: slow-pool load is not running during $label"
		echo dead > "$dir/slow-load"
	fi
	snap "$dir/pre"
	spid=""
	if [ -n "$SCHED_SPAN" ]; then
		"${REPORT[@]}" schedsample "$SCHED_DELAY" "$SCHED_SPAN" \
		    "$dir/sched.json" &
		spid=$!
	fi
	"${FIO_PREFIX[@]}" "$FIO" --output-format=json \
	    --output="$dir/fio.json" "$@" > "$dir/fio.stderr" 2>&1
	rc=$?
	[ -n "$spid" ] && wait "$spid"
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
	pool_op "$1" export "$1"
	pool_op "$1" import -d /dev "$1"
}

# pool_op POOL ARGS...: zpool ARGS under a 10-minute watchdog; on a hang,
# dump the blocked tasks (sysrq w) and the zpool processes' stacks, and stop.
pool_op() {
	local p=$1 f pid
	shift
	timeout 600 zpool "$@" && return 0
	f=$OUT/hung-$p-$1.txt
	{
		echo "zpool $* failed or hung (rc $?) at $(date +%T)"
		for pid in $(pgrep -x zpool); do
			echo "--- zpool $pid: $(tr '\0' ' ' < /proc/$pid/cmdline)"
			cat "/proc/$pid/stack"
		done
		echo w > /proc/sysrq-trigger
		sleep 2
		dmesg | tail -400
	} > "$f" 2>&1
	die "zpool $* (see $(basename "$f"))"
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
	    zfs_txg_history zio_taskq_read zio_taskq_write \
	    zio_taskq_bulk_lowpri zfs_arc_evict_lane zio_taskq_bulk_yield \
	    zio_taskq_write_pct zio_taskq_write_light spa_sync_light_pri; do
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

# ------------------------------------------------------------------ q2 --

q2_args() {
	local rw=randread nj=${1#??}
	case $1 in
	sw*) rw=randwrite ;;
	esac
	case $1 in
	rc*)
		# Cached reads: a 256 MiB file with primarycache=all.
		fio_args /fastq/c/f file "$rw" 1 "$nj" "$2"
		FIO_ARGS+=(--size=256M)
		;;
	tz|tl)
		# Peak async write throughput with zstd or lz4, 128K records,
		# data that compresses about 2:1; "IOPS" are MiB/s.
		FIO_ARGS=(--name=t --filename="/fastq/$1/f" --rw=write --bs=1M
		    --size=1G --ioengine=psync --time_based --runtime="$2"
		    --buffer_compress_percentage=50 --refill_buffers
		    --lat_percentiles=1 --percentile_list=50:99:99.9)
		;;
	*)
		fio_args /fastq/fs/f file "$rw" 1 "$nj" "$2"
		;;
	esac
}

phase_snap() {
	local d=$OUT/phase/$1.$2 p k
	mkdir -p "$d"
	cat /proc/uptime > "$d/uptime"
	head -1 /proc/stat > "$d/stat"
	for k in arcstats dmu_tx dbufstats zil abdstats; do
		cat "/proc/spl/kstat/zfs/$k" > "$d/$k" 2>/dev/null
	done
	for p in fastq "${SLOW_POOLS[@]:-slowq}"; do
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
	local f n=0 clr="" hist
	echo 'BEGIN { @t0 = nsecs; }'
	for f in zfs_write zfs_read zil_commit zil_commit_waiter \
	    dmu_tx_assign dmu_tx_wait txg_wait_open txg_wait_synced_flags \
	    arc_read arc_wait_for_eviction arc_wait_for_eviction_impl \
	    arc_get_data_impl dbuf_evict_one zio_wait spa_open_common; do
		awk -v f="$f" '$3 == f && $4 == "[zfs]" { found = 1; exit }
		    END { exit !found }' /proc/kallsyms || continue
		hist=""
		case $f in
		zio_wait|arc_read|arc_wait_for_eviction*|zfs_read|zil_commit)
			hist="@fast_h_$f = hist(\$d);" ;;
		esac
		cat <<-EOF
		kprobe:$f { @s${n}[tid] = nsecs; }
		kretprobe:$f /@s${n}[tid] != 0/
		{
			\$d = (nsecs - @s${n}[tid]) / 1000;
			if (comm == "$FIO") {
				@fast_us_$f = sum(\$d);
				@fast_n_$f = count();
				@fast_max_us_$f = max(\$d);
				$hist
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

# ------------------------------------------------------------------ qv --
#
# Variants of q2 that each remove one suspected cause of the interference,
# measured as phases of one job so that they share a runner.  QV_PHASES is a
# list of NAME:LOAD[:OPT,OPT...], LOAD being none, write or read (the slow
# pools' fio load during the phase), and OPT one of
#   comp=off|on       compression of the slow pools' datasets (default on)
#   pc=all|metadata   primarycache of the slow pools' datasets (default all)
#   nice=N            run the fast pool's fio at nice N
#   sep               slow pools' threads and load on QV_SLOW_CPUS (0,1),
#                     the fast pool's threads and fio on QV_FAST_CPUS (2,3)
#   pct=N             zio_taskq_batch_pct for the slow pools only
#   dirty=BYTES       zfs_dirty_data_max (default Q2_DIRTY_MAX)
#   kn=SET            prototype tunables, see qv_knobs (default up)
#   idle              run the slow pools' write issue threads SCHED_IDLE, an
#                     upper bound for making bulk work preemptible
#   diag              also run the sw1 diagnostic
# QV_POOLS=1 is the slow raidz2 of q2; QV_POOLS=3 is three slow raidz2 pools
# on loop devices over files, under dm-delay, so that they can hold dirty
# data far beyond their memory footprint; QV_POOLS=3n is three smaller
# copies of q2's slow pool on null_blk.

QV_POOLS=${QV_POOLS:-1}
QV_PHASES=${QV_PHASES:-alone:none sw:write alone2:none}
QV_WL=${QV_WL:-rr1 rr4 sw1 sw4}
QV_SLOW_WRITE_MB=${QV_SLOW_WRITE_MB:-1024}
QV_READ_MB=${QV_READ_MB:-0}
QV3_DEV_MB=${QV3_DEV_MB:-2048}
QV3N_DEV_MB=${QV3N_DEV_MB:-384}
QV_SLOW_CPUS=${QV_SLOW_CPUS:-0,1}
QV_FAST_CPUS=${QV_FAST_CPUS:-2,3}
QV_WARM_WRITE=${QV_WARM_WRITE:-20}
QV_WARM_READ=${QV_WARM_READ:-15}
SLOW_POOLS=()
SLOW_PIDS=()
SLOW_IMPORT_DIR=/dev/mapper
PIN_PID=""
IDLE_PID=""
FAST_THREADS='^(z_(null|rd|wr|fr|cl|flush|trim)_|dp_sync_taskq)'

# Tunable sets of the prototype commits (pool-isolation-proto):
#   up   all off: the taskq layout and priorities of master, single FIFO
#   c1   synchronous write issue taskq only
#   w    c1 + bulk taskqs at default priority (write side)
#   wy   c1 + bulk taskq threads yield between tasks
#   w2   c1 + default priority + yield
#   r    synchronous read interrupt taskq + bulk taskqs at default priority
#   r2   r + yield
#   l    eviction lane only
#   all  everything up to the yield commit (lane included)
#   p6   write issue threads at 25 % of the CPUs per pool, only
#   w2p6 w2 + p6
#   w2l  w2 + async writes of lightly dirty pools at high priority
#   all2 w2 + p6 + light (no lane)
#   w2lc w2l + sync threads of lightly dirty pools at default priority
#   all3 all2 + that
qv_knobs() {
	local wr="sync null scale null" rd="fixed,1,8 null scale null"
	local low=0 lane=0 yield=0 wpct=80 light=0 slp=0
	local c1w="sync scale scale null" c2r="fixed,1,8 null scale scale"
	case $1 in
	up) ;;
	c1) wr=$c1w ;;
	w) wr=$c1w; low=1 ;;
	wy) wr=$c1w; yield=1 ;;
	w2) wr=$c1w; low=1; yield=1 ;;
	r) rd=$c2r; low=1 ;;
	r2) rd=$c2r; low=1; yield=1 ;;
	l) lane=1 ;;
	all) wr=$c1w; rd=$c2r; low=1; lane=1; yield=1 ;;
	p6) wpct=25 ;;
	w2p6) wr=$c1w; low=1; yield=1; wpct=25 ;;
	w2l) wr=$c1w; low=1; yield=1; light=1 ;;
	all2) wr=$c1w; rd=$c2r; low=1; yield=1; wpct=25; light=1 ;;
	w2lc) wr=$c1w; low=1; yield=1; light=1; slp=1 ;;
	all3) wr=$c1w; rd=$c2r; low=1; yield=1; wpct=25; light=1; slp=1 ;;
	*) die "unknown knob set $1" ;;
	esac
	local m=/sys/module/zfs/parameters
	# No newline: the row parser takes it as part of the last mode.
	printf '%s' "$wr" > $m/zio_taskq_write || die "zio_taskq_write"
	printf '%s' "$rd" > $m/zio_taskq_read || die "zio_taskq_read"
	echo "$low" > $m/zio_taskq_bulk_lowpri || die "zio_taskq_bulk_lowpri"
	echo "$lane" > $m/zfs_arc_evict_lane || die "zfs_arc_evict_lane"
	echo "$yield" > $m/zio_taskq_bulk_yield || die "zio_taskq_bulk_yield"
	echo "$wpct" > $m/zio_taskq_write_pct || die "zio_taskq_write_pct"
	echo "$light" > $m/zio_taskq_write_light || die "zio_taskq_write_light"
	echo "$slp" > $m/spa_sync_light_pri || die "spa_sync_light_pri"
}

qv_setup_fast() {
	nullb_add nullb0 0 "$FAST_DEV_MB" || die "nullb0"
	nullb_add nullb1 1 "$FAST_DEV_MB" || die "nullb1"
	zpool create -f -o ashift=12 -O atime=off \
	    -O primarycache=metadata -O sync=always fastq \
	    mirror /dev/nullb0 /dev/nullb1 || die "zpool create fastq"
	zfs create -o recordsize=4K fastq/fs || die "zfs create fastq/fs"
	prefill /fastq/fs/f 0
	case " $QV_WL " in
	*" tz "*|*" tl "*)
		zfs create -o recordsize=128K -o compression=zstd \
		    -o sync=standard fastq/tz || die "zfs create fastq/tz"
		zfs create -o recordsize=128K -o compression=lz4 \
		    -o sync=standard fastq/tl || die "zfs create fastq/tl"
		;;
	esac
	case " $QV_WL " in
	*" rc1 "*)
		zfs create -o recordsize=4K -o primarycache=all fastq/c ||
		    die "zfs create fastq/c"
		fio --name=prefill --filename=/fastq/c/f --rw=write --bs=1M \
		    --size=256M --end_fsync=1 --ioengine=psync \
		    > "$WORK/prefill-c.log" 2>&1 || die "prefill fastq/c"
		;;
	esac
}

qv_setup_slow1() {
	local i idx sz mode=dm-delay attrs devs=()
	modprobe dm_delay || { mode=nullb-timer; SLOW_IMPORT_DIR=/dev; }
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
	echo "slow pool: raidz2 of $SLOW_NDEV x ${SLOW_DEV_MB} MiB null_blk," \
	    "$mode ${SLOW_DELAY_MS} ms, ${attrs[*]}" >> "$OUT/setup.txt"
	zpool create -f -o ashift=12 -O atime=off slowq \
	    raidz2 "${devs[@]}" || die "zpool create slowq"
	zfs create slowq/data || die "zfs create slowq/data"
	SLOW_POOLS=(slowq)
	if [ "$QV_READ_MB" -gt 0 ]; then
		log "qv writing the ${QV_READ_MB} MiB read file"
		fio --name=mkread --filename=/slowq/data/r --rw=write --bs=1M \
		    --size="${QV_READ_MB}M" --ioengine=psync --end_fsync=1 \
		    > "$WORK/mkread.log" 2>&1 ||
		    log "WARNING: read file incomplete"
	fi
}

qv_setup_slow3() {
	local p i f loop sz devs dio
	modprobe dm_delay || die "dm_delay is required for QV_POOLS=3"
	mkdir -p "$WORK/img"
	for p in 1 2 3; do
		devs=()
		for i in $(seq 0 $((SLOW_NDEV - 1))); do
			f=$WORK/img/slow$p.$i.img
			truncate -s "${QV3_DEV_MB}M" "$f" || die "truncate $f"
			loop=$(losetup --direct-io=on --sector-size 4096 -f \
			    --show "$f") || die "losetup $f"
			sz=$(blockdev --getsz "$loop")
			dmsetup create "slow$p.$i" --table \
			    "0 $sz delay $loop 0 $SLOW_DELAY_MS" ||
			    die "dmsetup slow$p.$i"
			devs+=("/dev/mapper/slow$p.$i")
		done
		udevadm settle
		zpool create -f -o ashift=12 -O atime=off \
		    "slow$p" raidz2 "${devs[@]}" || die "zpool create slow$p"
		zfs create "slow$p/data" || die "zfs create slow$p/data"
		SLOW_POOLS+=("slow$p")
	done
	losetup -l > "$OUT/losetup.txt" 2>&1
	dio=$(losetup -l -n -O DIO | sort | uniq -c | tr '\n' ' ')
	echo "slow pools: 3 x raidz2 of $SLOW_NDEV x ${QV3_DEV_MB} MiB loop" \
	    "files under dm-delay ${SLOW_DELAY_MS} ms (loop DIO: $dio)" \
	    >> "$OUT/setup.txt"
}

# Three slow raidz2 pools on null_blk under dm-delay, like q2's but
# smaller (QV3N_DEV_MB per device), for the CPU load of several pools.
qv_setup_slow3n() {
	local p i idx sz devs attrs
	modprobe dm_delay || die "dm_delay is required for QV_POOLS=3n"
	for p in 1 2 3; do
		devs=()
		for i in $(seq 0 $((SLOW_NDEV - 1))); do
			idx=$((2 + (p - 1) * SLOW_NDEV + i))
			attrs=()
			if grep -qw mbps /sys/kernel/config/nullb/features; then
				attrs+=("mbps=$SLOW_MBPS")
			fi
			nullb_add "nullb$idx" "$idx" "$QV3N_DEV_MB" "${attrs[@]}" ||
			    die "nullb$idx"
			sz=$(blockdev --getsz "/dev/nullb$idx")
			dmsetup create "slow$p.$i" --table \
			    "0 $sz delay /dev/nullb$idx 0 $SLOW_DELAY_MS" ||
			    die "dmsetup slow$p.$i"
			devs+=("/dev/mapper/slow$p.$i")
		done
		udevadm settle
		zpool create -f -o ashift=12 -O atime=off \
		    "slow$p" raidz2 "${devs[@]}" || die "zpool create slow$p"
		zfs create "slow$p/data" || die "zfs create slow$p/data"
		SLOW_POOLS+=("slow$p")
	done
	echo "slow pools: 3 x raidz2 of $SLOW_NDEV x ${QV3N_DEV_MB} MiB" \
	    "null_blk under dm-delay ${SLOW_DELAY_MS} ms, ${attrs[*]}" \
	    >> "$OUT/setup.txt"
}

qv_slow_stop() {
	local pid
	for pid in "${SLOW_PIDS[@]}"; do
		kill -INT "$pid" 2>/dev/null
	done
	for pid in "${SLOW_PIDS[@]}"; do
		wait "$pid"
	done
	SLOW_PIDS=()
	SLOW_PID=""
}

# qv_slow_start LOAD PHASE PREFIX...: one fio per slow pool.
qv_slow_start() {
	local load=$1 ph=$2 p
	shift 2
	for p in "${SLOW_POOLS[@]}"; do
		case $load in
		write)
			"$@" fio --name="sw-$p" --output-format=json \
			    --output="$OUT/slow.$ph.$p.json" \
			    --filename="/$p/data/w" --rw=write --bs=1M \
			    --size="${QV_SLOW_WRITE_MB}M" --ioengine=psync \
			    --time_based --runtime=36000 \
			    > "$WORK/slow.$ph.$p.log" 2>&1 &
			;;
		read)
			"$@" fio --name="sr-$p" --output-format=json \
			    --output="$OUT/slow.$ph.$p.json" \
			    --filename="/$p/data/r" --rw=read --bs=1M \
			    --size=$((QV_READ_MB / 2))M \
			    --offset_increment=$((QV_READ_MB / 2))M \
			    --numjobs=2 --group_reporting --ioengine=psync \
			    --time_based --runtime=36000 \
			    > "$WORK/slow.$ph.$p.log" 2>&1 &
			;;
		esac
		SLOW_PIDS+=($!)
	done
	SLOW_PID=${SLOW_PIDS[0]:-}
}

# Pin the slow pools' threads (named s_* by this branch) to QV_SLOW_CPUS and
# the fast pool's zio threads to QV_FAST_CPUS, again as dynamic taskqs spawn
# threads.  Failures are counted, since some kernel threads refuse it.
qv_pin_loop() {
	local pid fail=0 ok=0
	declare -A seen
	while :; do
		for pid in $(pgrep '^s_'); do
			[ -n "${seen[$pid]:-}" ] && continue
			if taskset -pc "$QV_SLOW_CPUS" "$pid" > /dev/null 2>&1; then
				ok=$((ok + 1))
			else
				fail=$((fail + 1))
			fi
			seen[$pid]=1
		done
		for pid in $(pgrep "$FAST_THREADS"); do
			[ -n "${seen[$pid]:-}" ] && continue
			if taskset -pc "$QV_FAST_CPUS" "$pid" > /dev/null 2>&1; then
				ok=$((ok + 1))
			else
				fail=$((fail + 1))
			fi
			seen[$pid]=1
		done
		echo "pinned $ok failed $fail" > "$1"
		sleep 0.5
	done
}

# Switch the slow pools' write issue threads to SCHED_IDLE as they appear.
qv_idle_loop() {
	local pid ok=0 fail=0
	declare -A seen
	while :; do
		for pid in $(pgrep '^s_wr_iss'); do
			[ -n "${seen[$pid]:-}" ] && continue
			if chrt -i -p 0 "$pid" > /dev/null 2>&1; then
				ok=$((ok + 1))
			else
				fail=$((fail + 1))
			fi
			seen[$pid]=1
		done
		echo "SCHED_IDLE set $ok failed $fail" > "$1"
		sleep 0.5
	done
}

qv_cycle_slow() {
	local pct=$1 p m=/sys/module/zfs/parameters
	local saved
	saved=$(cat $m/zio_taskq_batch_pct)
	[ -n "$pct" ] && echo "$pct" > $m/zio_taskq_batch_pct
	# Import from the dm-delay devices only: the devices under them carry
	# the same labels, and importing from those would skip the delay.
	for p in "${SLOW_POOLS[@]}"; do
		pool_op "$p" export "$p"
		pool_op "$p" import -d "$SLOW_IMPORT_DIR" "$p"
	done
	echo "$saved" > $m/zio_taskq_batch_pct
}

qv_diag() {
	local ph=$1 wl=$2 label d fpid b1 b2 ppid ipid
	label=qvdiag.$ph.$wl
	d=$OUT/diag/$label
	mkdir -p "$d"
	q2_args "$wl" "$DIAG_RUNTIME"
	run_fio "$label" "${FIO_ARGS[@]}" &
	fpid=$!
	sleep 2
	timeout -s INT 14 bpftrace "$WORK/funclat.bt" > "$d/funclat.txt" \
	    2> "$d/funclat.err" &
	b1=$!
	timeout -s INT 14 bpftrace "$HERE/schedlock.bt" > "$d/schedlock.txt" \
	    2> "$d/schedlock.err" &
	b2=$!
	"$PERF" record -a -g -F 499 -e cpu-clock -o "$WORK/perf.data" \
	    -- sleep 8 > "$WORK/perf-qv.log" 2>&1 &
	ppid=$!
	zpool iostat -l -y 8 1 > "$d/zpool-iostat.txt" 2>&1 &
	ipid=$!
	sleep 4
	cat /proc/spl/taskq > "$d/taskq.txt" 2>/dev/null
	wait "$ppid"
	wait "$ipid"
	wait "$b1"
	wait "$b2"
	wait "$fpid"
	perf_report perf "$d"
}

# Diagnostic run S: perf sched record for 6 s, summarized on the spot into
# the scheduling delay (wakeup to running) of the fast pool's threads and the
# threads that held their CPU meanwhile (report.py timehist).
qv_diag_sched() {
	local ph=$1 wl=$2 label d fpid
	label=qvsched.$ph.$wl
	d=$OUT/diag/$label
	mkdir -p "$d"
	q2_args "$wl" "$DIAG_RUNTIME"
	run_fio "$label" "${FIO_ARGS[@]}" &
	fpid=$!
	sleep 4
	"$PERF" sched record -a -o "$WORK/sched.data" -- sleep 6 \
	    > "$WORK/perf-sched.log" 2>&1 ||
	    tail -3 "$WORK/perf-sched.log" > "$d/sched.err"
	wait "$fpid"
	"$PERF" sched timehist -i "$WORK/sched.data" 2> "$d/timehist.err" |
	    "${REPORT[@]}" timehist "$d/timehist.json"
	rm -f "$WORK/sched.data"
}

qv_phase() {
	local spec=$1 name load opts o rep wl comp=on pc=all nice="" sep=""
	local pct="" dirty=$Q2_DIRTY_MAX kn=up diag="" idle="" p
	name=${spec%%:*}
	load=${spec#*:}
	opts=""
	case $load in
	*:*) opts=${load#*:}; load=${load%%:*} ;;
	esac
	for o in ${opts//,/ }; do
		case $o in
		comp=*) comp=${o#comp=} ;;
		pc=*) pc=${o#pc=} ;;
		nice=*) nice=${o#nice=} ;;
		sep) sep=1 ;;
		pct=*) pct=${o#pct=} ;;
		dirty=*) dirty=${o#dirty=} ;;
		kn=*) kn=${o#kn=} ;;
		diag) diag=1 ;;
		idle) idle=1 ;;
		*) die "unknown option $o in $spec" ;;
		esac
	done
	log "qv phase $name ($spec)"
	echo "$name $spec" >> "$OUT/phases.txt"
	qv_slow_stop
	echo "$dirty" > /sys/module/zfs/parameters/zfs_dirty_data_max
	qv_knobs "$kn"
	for p in "${SLOW_POOLS[@]}"; do
		zfs set compression="$comp" primarycache="$pc" "$p/data"
	done
	qv_cycle_slow "$pct"
	FIO_PREFIX=()
	[ -n "$nice" ] && FIO_PREFIX+=(nice -n "$nice")
	mkdir -p "$OUT/phase"
	if [ -n "$sep" ]; then
		FIO_PREFIX=(taskset -c "$QV_FAST_CPUS" "${FIO_PREFIX[@]}")
		qv_pin_loop "$OUT/phase/$name.pin" &
		PIN_PID=$!
		sleep 1
	fi
	if [ -n "$idle" ]; then
		qv_idle_loop "$OUT/phase/$name.idle" &
		IDLE_PID=$!
	fi
	if [ "$load" != none ]; then
		if [ -n "$sep" ]; then
			qv_slow_start "$load" "$name" taskset -c "$QV_SLOW_CPUS"
		else
			qv_slow_start "$load" "$name" env
		fi
		if [ "$load" = write ]; then
			sleep "$QV_WARM_WRITE"
		else
			sleep "$QV_WARM_READ"
		fi
	fi
	phase_snap "$name" start
	for rep in $(seq 1 "$REPS"); do
		pool_cycle fastq
		for wl in $QV_WL; do
			if [ "$wl" = rc1 ]; then
				fio --name=warm --filename=/fastq/c/f --rw=read \
				    --bs=1M --size=256M --ioengine=psync \
				    > /dev/null 2>&1
			fi
			q2_args "$wl" "$Q2_RUNTIME"
			run_fio "qv.$name.$wl.r$rep" "${FIO_ARGS[@]}"
		done
	done
	qv_diag "$name" rr1
	qv_diag_sched "$name" rr1
	if [ -n "$diag" ]; then
		qv_diag "$name" sw1
		qv_diag_sched "$name" sw1
	fi
	phase_snap "$name" end
	if [ -n "$IDLE_PID" ]; then
		kill "$IDLE_PID" 2>/dev/null
		wait "$IDLE_PID" 2>/dev/null
		IDLE_PID=""
	fi
	if [ -n "$PIN_PID" ]; then
		kill "$PIN_PID" 2>/dev/null
		wait "$PIN_PID" 2>/dev/null
		PIN_PID=""
		# Give every thread all CPUs back for the next phase.
		for p in $(pgrep '^(s_|z_|dp_sync_taskq)'); do
			taskset -pc "0-$(($(nproc) - 1))" "$p" > /dev/null 2>&1
		done
	fi
	FIO_PREFIX=()
}

qv() {
	local spec
	common_setup
	echo "$Q2_DIRTY_MAX" > /sys/module/zfs/parameters/zfs_dirty_data_max ||
	    die "cannot set zfs_dirty_data_max"
	SCHED_DELAY=3
	SCHED_SPAN=$((Q2_RUNTIME - 6))
	: > "$OUT/setup.txt"
	: > "$OUT/phases.txt"
	echo "fast pool: mirror of 2 x ${FAST_DEV_MB} MiB null_blk" \
	    >> "$OUT/setup.txt"
	qv_setup_fast
	case $QV_POOLS in
	0) ;;
	1) qv_setup_slow1 ;;
	3) qv_setup_slow3 ;;
	3n) qv_setup_slow3n ;;
	*) die "QV_POOLS must be 0, 1, 3 or 3n" ;;
	esac
	echo "zfs_arc_max $ARC_MAX zfs_dirty_data_max $Q2_DIRTY_MAX," \
	    "workloads $QV_WL, slow write file ${QV_SLOW_WRITE_MB} MiB," \
	    "read file ${QV_READ_MB} MiB, ${REPS} x ${Q2_RUNTIME} s" \
	    >> "$OUT/setup.txt"
	zpool sync
	dump_params > "$OUT/params.txt"
	zpool status > "$OUT/zpool-status.txt"
	gen_funclat > "$WORK/funclat.bt"
	cp "$WORK/funclat.bt" "$OUT/funclat.bt"
	idle_baseline
	for spec in $QV_PHASES; do
		qv_phase "$spec"
	done
	qv_slow_stop
	zpool status > "$OUT/zpool-status.end"
	zpool list -v > "$OUT/zpool-list.end"
	free -m > "$OUT/free.end"
}

case $EXP in
q1) q1 ;;
q2) q2 ;;
qv) qv ;;
*) die "unknown experiment $EXP" ;;
esac
log "done"
chmod -R a+rX "$OUT"
