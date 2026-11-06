#!/usr/bin/env bash
# SPDX-License-Identifier: CDDL-1.0
#
# Fork-only experiment, run as root by .github/workflows/perf-zvol-v2.yml:
# io_uring rows on a zvol, alternating the kernel modules of three trees
# (TREES/A, TREES/B, TREES/C, each with module/spl.ko and module/zfs.ko)
# and module-parameter variants of them, on one pool of two RAM disks.
#
#   VARIANTS  NAME=TREE[:PARAM=VALUE[,PARAM=VALUE...]] ...
#   REPS      repetitions; odd ones in VARIANTS order, even ones reversed
#   RT        seconds per fio row
#
# It loads and unloads the zfs modules and creates a pool on RAM disks:
# run it only on a disposable machine.

set -u -o pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:?}
TREES=${TREES:?}
RT=${RT:-10}
REPS=${REPS:-2}
VARIANTS=${VARIANTS:-"A=A B=B C=C A2=A"}
MQ_VARIANTS=${MQ_VARIANTS:-""}
DIAG_VARIANTS=${DIAG_VARIANTS:-""}
VOLSIZE_MB=${VOLSIZE_MB:-2048}
DEV_MB=${DEV_MB:-3584}
ARC_MAX=${ARC_MAX:-$((2 << 30))}
POOL=zv2
PERF=perf
DEVS=()

mkdir -p "$OUT/runs"
exec > >(tee -a "$OUT/run.log") 2>&1

log() {
	printf '[%s] %s\n' "$(date +%T)" "$*"
}

die() {
	log "FATAL: $*"
	exit 1
}

tsnap() {
	python3 -I "$HERE/report.py" tsnap "$1"
}

# ------------------------------------------------------------ devices --

setup_devs() {
	local d
	if modprobe brd rd_nr=2 rd_size=$((DEV_MB * 1024)) 2>/dev/null &&
	    [ -b /dev/ram0 ] && [ -b /dev/ram1 ]; then
		DEVS=(/dev/ram0 /dev/ram1)
		echo "devices: brd ${DEV_MB} MiB x2" >> "$OUT/system.txt"
		return 0
	fi
	modprobe configfs
	mountpoint -q /sys/kernel/config ||
	    mount -t configfs none /sys/kernel/config
	modprobe null_blk nr_devices=0 || die "neither brd nor null_blk"
	for i in 0 1; do
		d=/sys/kernel/config/nullb/nullb$i
		mkdir "$d" || die "nullb$i"
		echo "$i" > "$d/index" 2>/dev/null
		echo "$DEV_MB" > "$d/size"
		echo 4096 > "$d/blocksize"
		echo 1 > "$d/memory_backed"
		echo 0 > "$d/irqmode"
		echo 1 > "$d/power" || die "nullb$i power"
	done
	udevadm settle
	DEVS=(/dev/nullb0 /dev/nullb1)
	echo "devices: null_blk memory_backed irqmode=0 ${DEV_MB} MiB x2" \
	    >> "$OUT/system.txt"
}

# ------------------------------------------------------------ modules --

unload() {
	local i
	if [ -e /dev/zfs ] && zpool list -H -o name 2>/dev/null |
	    grep -qx "$POOL"; then
		for i in $(seq 1 20); do
			udevadm settle
			zpool export "$POOL" 2> "$OUT/export.err" && break
			sleep 0.5
		done
		zpool list -H -o name 2>/dev/null | grep -qx "$POOL" &&
		    die "cannot export $POOL: $(cat "$OUT/export.err")"
	fi
	for i in $(seq 1 20); do
		lsmod | grep -qw '^zfs' || break
		rmmod zfs 2>/dev/null || sleep 0.5
	done
	lsmod | grep -qw '^zfs' && die "cannot unload zfs"
	lsmod | grep -qw '^spl' && { rmmod spl || die "cannot unload spl"; }
	return 0
}

# load VARIANT: unload, insmod its tree's modules, check their srcversion,
# set its parameters, import the pool.
load() {
	local v=$1 name tree params kv m k l p
	name=${v%%=*}
	tree=${v#*=}
	params=""
	if [ "${tree#*:}" != "$tree" ]; then
		params=${tree#*:}
		tree=${tree%%:*}
	fi
	unload
	modprobe -a zlib_deflate zlib_inflate 2>/dev/null
	insmod "$TREES/$tree/module/spl.ko" || die "insmod spl ($tree)"
	insmod "$TREES/$tree/module/zfs.ko" || die "insmod zfs ($tree)"
	for m in spl zfs; do
		k=$(modinfo -F srcversion "$TREES/$tree/module/$m.ko")
		l=$(cat "/sys/module/$m/srcversion")
		echo "$name $tree $m srcversion tree=$k loaded=$l" >> \
		    "$OUT/srcversion.txt"
		[ "$k" = "$l" ] || die "$name: $m srcversion mismatch"
	done
	echo "$ARC_MAX" > /sys/module/zfs/parameters/zfs_arc_max
	for kv in $(echo "$params" | tr ',' ' '); do
		echo "${kv#*=}" > "/sys/module/zfs/parameters/${kv%%=*}" ||
		    die "$name: cannot set ${kv%%=*}"
	done
	{
		printf '%s tree=%s' "$name" "$tree"
		for p in zvol_nowait zvol_nowait_max_inflight zvol_use_blk_mq \
		    zvol_request_sync zvol_threads zvol_num_taskqs; do
			printf ' %s=%s' "$p" \
			    "$(cat /sys/module/zfs/parameters/$p 2>/dev/null ||
			    echo absent)"
		done
		echo
	} | tee -a "$OUT/params.txt"
	udevadm settle
	if [ "$#" -gt 1 ]; then
		return 0
	fi
	zpool import -d "$(dirname "${DEVS[0]}")" "$POOL" ||
	    die "$name: cannot import $POOL"
	zvol_dev > /dev/null || die "$name: no zvol device"
}

zvol_dev() {
	local i
	for i in $(seq 1 60); do
		if [ -b "/dev/zvol/$POOL/v" ]; then
			readlink -f "/dev/zvol/$POOL/v"
			return 0
		fi
		udevadm settle
		sleep 0.5
	done
	return 1
}

# ---------------------------------------------------------------- fio --

busy() {
	awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat
}

ctxt() {
	awk '/^ctxt/{print $2}' /proc/stat
}

# row LABEL RW BS QD NJ [FIO ARGS...]
row() {
	local label=$1 rw=$2 bs=$3 qd=$4 nj=$5 dir dev
	shift 5
	dir=$OUT/runs/$label
	mkdir -p "$dir"
	dev=$(zvol_dev) || die "no zvol device for $label"
	echo "$(busy) $(ctxt) $(date +%s.%N)" > "$dir/pre.stat"
	tsnap "$dir/pre.threads.json"
	fio --name=j --filename="$dev" --ioengine=io_uring --rw="$rw" \
	    --bs="$bs" --iodepth="$qd" --numjobs="$nj" --direct=1 \
	    --time_based --runtime="$RT" --norandommap --randrepeat=0 \
	    --group_reporting --output-format=json --output="$dir/fio.json" \
	    "$@" > "$dir/fio.stderr" 2>&1
	echo $? > "$dir/rc"
	tsnap "$dir/post.threads.json"
	echo "$(busy) $(ctxt) $(date +%s.%N)" > "$dir/post.stat"
	python3 -I "$HERE/report.py" line "$dir"
}

warmup() {
	local dev
	dev=$(zvol_dev) || die "no zvol device for warm-up"
	fio --name=w --filename="$dev" --ioengine=io_uring --rw=read --bs=1M \
	    --iodepth=8 --direct=1 > /dev/null 2>&1
	fio --name=w --filename="$dev" --ioengine=io_uring --rw=randread \
	    --bs=4k --iodepth=32 --direct=1 --time_based --runtime=3 \
	    --norandommap > /dev/null 2>&1
}

# The rows of one pass of a variant.
main_rows() {
	local n=$1 r=$2
	warmup
	row "$n.uring-4k-read-32x1.r$r" read 4k 32 1
	row "$n.uring-16k-randread-32x1.r$r" randread 16k 32 1
	row "$n.uring-4k-randread-1x1.r$r" randread 4k 1 1
	row "$n.uring-4k-randread-8x1.r$r" randread 4k 8 1
	row "$n.uring-4k-randread-16x1.r$r" randread 4k 16 1
	row "$n.uring-4k-randread-1x8.r$r" randread 4k 1 8
	row "$n.uring-4k-randread-32x8.r$r" randread 4k 32 8
	row "$n.osync-4k-randwrite-1x1.r$r" randwrite 4k 1 1 --sync=1
	row "$n.uring-16k-write-32x1.r$r" write 16k 32 1
	zpool sync "$POOL"
}

mq_rows() {
	local n=$1 r=$2
	warmup
	row "$n.uring-4k-randread-32x1.r$r" randread 4k 32 1
	row "$n.uring-4k-randread-1x1.r$r" randread 4k 1 1
	row "$n.osync-4k-randwrite-32x1.r$r" randwrite 4k 32 1 --sync=1
	row "$n.osync-4k-randwrite-1x1.r$r" randwrite 4k 1 1 --sync=1
	zpool sync "$POOL"
}

# diag_row LABEL RW BS QD NJ: a row with io_uring punts counted, and a
# CPU profile of the whole machine.
diag_row() {
	local label=$1 dir fpid
	dir=$OUT/runs/$label
	row "$@" &
	fpid=$!
	sleep 2
	if "$PERF" --version > /dev/null 2>&1; then
		"$PERF" stat -a -x, -o "$dir/perf-stat.txt" \
		    -e io_uring:io_uring_queue_async_work \
		    -e io_uring:io_uring_submit_req \
		    -e sched:sched_switch -- sleep 3 > /dev/null 2>&1 ||
		    "$PERF" stat -a -x, -o "$dir/perf-stat.txt" \
		    -e io_uring:io_uring_queue_async_work -- sleep 3 \
		    > /dev/null 2>&1 ||
		    echo "perf stat failed" > "$dir/perf-stat.txt"
		"$PERF" record -a -g -F 2999 -e cpu-clock -o "$dir/perf.data" \
		    -- sleep 3 > "$dir/perf-record.log" 2>&1
	fi
	wait "$fpid"
	if [ -s "$dir/perf.data" ]; then
		"$PERF" report -i "$dir/perf.data" --stdio --no-children \
		    --sort comm -g none > "$dir/perf.comm.txt" 2>/dev/null
		"$PERF" report -i "$dir/perf.data" --stdio --comms fio \
		    --children --sort sym -g none --percent-limit 0.5 \
		    > "$dir/perf.fio.children.txt" 2>/dev/null
		"$PERF" report -i "$dir/perf.data" --stdio --comms fio \
		    --no-children --sort sym -g none --percent-limit 0.3 \
		    > "$dir/perf.fio.self.txt" 2>/dev/null
		rm -f "$dir/perf.data"
	fi
}

diag_rows() {
	local n=$1
	warmup
	diag_row "$n.diag-4k-read-32x1" read 4k 32 1
	diag_row "$n.diag-4k-randread-1x1" randread 4k 1 1
	diag_row "$n.diag-4k-randread-32x8" randread 4k 32 8
	diag_row "$n.diag-16k-write-32x1" write 16k 32 1
	zpool sync "$POOL"
}

# --------------------------------------------------------------- main --

main() {
	local rep v list rev first i
	{
		uname -a
		lscpu
		free -m
		fio --version
		"$PERF" --version 2>&1
	} > "$OUT/system.txt" 2>&1
	sysctl -q -w kernel.kptr_restrict=0 kernel.perf_event_paranoid=-1
	if ! "$PERF" --version > /dev/null 2>&1; then
		for p in /usr/lib/linux-tools/*/perf; do
			[ -x "$p" ] && PERF=$p
		done
	fi
	setup_devs
	read -ra list <<< "$VARIANTS"
	first=${list[0]}
	load "$first" noimport
	zpool create -f -o ashift=12 -O atime=off -O compression=off \
	    "$POOL" mirror "${DEVS[@]}" || die "zpool create"
	zfs create -V "${VOLSIZE_MB}M" -o volblocksize=16k \
	    -o primarycache=metadata "$POOL/v" || die "zfs create"
	fio --name=fill --filename="$(zvol_dev)" --rw=write --bs=1M \
	    --direct=1 --ioengine=psync > "$OUT/fill.log" 2>&1 ||
	    die "fill failed"
	zpool sync "$POOL"
	zpool status "$POOL" > "$OUT/zpool-status.txt"
	echo "$VARIANTS" > "$OUT/variants.txt"
	echo "$MQ_VARIANTS" > "$OUT/mq-variants.txt"
	for rep in $(seq 1 "$REPS"); do
		if [ $((rep % 2)) -eq 1 ]; then
			for v in "${list[@]}"; do
				log "pass ${v%%=*} rep $rep"
				load "$v"
				main_rows "${v%%=*}" "$rep"
			done
		else
			for ((rev = ${#list[@]} - 1; rev >= 0; rev--)); do
				v=${list[$rev]}
				log "pass ${v%%=*} rep $rep"
				load "$v"
				main_rows "${v%%=*}" "$rep"
			done
		fi
	done
	read -ra list <<< "$MQ_VARIANTS"
	for rep in $(seq 1 "$REPS"); do
		for ((i = 0; i < ${#list[@]}; i++)); do
			v=${list[$i]}
			[ $((rep % 2)) -eq 0 ] && v=${list[${#list[@]} - 1 - i]}
			log "mq pass ${v%%=*} rep $rep"
			load "$v"
			mq_rows "${v%%=*}" "$rep"
		done
	done
	read -ra list <<< "$DIAG_VARIANTS"
	for v in "${list[@]}"; do
		log "diag pass ${v%%=*}"
		load "$v"
		diag_rows "${v%%=*}"
	done
	unload
	log "done"
}

main
