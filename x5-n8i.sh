#!/usr/bin/env bash
# SPDX-License-Identifier: CDDL-1.0
#
# x5-n8i.sh: Part B of TASK-1122-X5, the row n8i of TASK-1116-C1B on the
# one loaded module of the X5 tree: 8 O_SYNC writers of 64K, 2 on each of
# 4 zvols (volblocksize 16K, so WR_INDIRECT: 4 dmu_sync() block writes per
# write) of c1n, a raidz1 of five 512 MiB memory-backed null_blk devices,
# no slog.  An arm is a set of module parameters, written before c1n is
# exported and imported again so that its taskqs follow them.  Run as root
# by xp-drive.sh, with the tree's zpool first in PATH.
#
#   x5-n8i.sh setup DIR           null_blk nullb0-nullb4, the pool c1n and
#                                 its zvols c1n/v0-v3, written in full
#   x5-n8i.sh pass DIR ARM...     for each ARM in that order: set it, cycle
#                                 c1n, run the row into DIR/ARM/
#
# Arms: master (zil_issue_inline 0), v10a (1), v10a-c1 (1, and
# zio_taskq_write "sync scale scale null"); every other prototype tunable
# as run.sh's knob set "up".  RT: measured seconds (default 10), after a
# 3 s ramp.  Exit 1 on an error; a zpool command that does not return
# within 10 minutes leaves DIR/hung-*.txt.  The XP_* variables exist only
# for the kit's mock tests.
set -u -o pipefail

P=${XP_PARAMS:-/sys/module/zfs/parameters}
NB=${XP_NULLB:-/sys/kernel/config/nullb}
TQK=${XP_TQKSTAT:-/proc/spl/kstat/taskq}
KS=${XP_KSTAT:-/proc/spl/kstat/zfs}
DEV=${XP_DEV:-/dev}
RT=${RT:-10}
RAMP=3
POOL=c1n
NDEV=5
DEV_MB=512
ZVOL_SIZE=128M
UPW="sync null scale null"
C1W="sync scale scale null"
UPR="fixed,1,8 null scale null"
Z=$(dirname "$(command -v zpool)")
OUTDIR=""

log() { printf '[%s] %s\n' "$(date +%T)" "$*"; }
die() { log "FATAL: $*"; exit 1; }

# pool_op ARGS...: zpool ARGS under a 10-minute watchdog, as run.sh's.
pool_op() {
	local f pid
	timeout 600 zpool "$@" && return 0
	f=$OUTDIR/hung-$POOL-$1.txt
	{
		echo "zpool $* failed or hung (rc $?) at $(date +%T)"
		for pid in $(pgrep -x zpool); do
			echo "--- zpool $pid: $(tr '\0' ' ' < "/proc/$pid/cmdline")"
			cat "/proc/$pid/stack"
		done
		dmesg | tail -400
	} > "$f" 2>&1
	die "zpool $* (see $(basename "$f"))"
}

# nullb_add NAME INDEX SIZE_MB: a memory-backed null_blk device, as
# run.sh's and TASK-1116-C1B's.
nullb_add() {
	local d=$NB/$1
	mkdir "$d" || return 1
	echo "$2" > "$d/index" 2>/dev/null
	echo "$3" > "$d/size" || return 1
	echo 4096 > "$d/blocksize" || return 1
	echo 1 > "$d/memory_backed" || return 1
	echo 1 > "$d/irqmode"
	nproc > "$d/submit_queues"
	echo 256 > "$d/hw_queue_depth"
	echo 1 > "$d/power" || return 1
	udevadm settle
	[ -b "$DEV/$1" ] || { log "$DEV/$1 did not appear"; return 1; }
}

# zvols: the block devices of c1n/v0 to v3, by their /dev/zvol links when
# udev made them, else the zd devices that the tree's zvol_id names so
# (their numbers change with each import).
zvols() {
	local i d
	if [ -e "$DEV/zvol/$POOL/v0" ]; then
		for i in 0 1 2 3; do echo "$DEV/zvol/$POOL/v$i"; done
		return
	fi
	for i in 0 1 2 3; do
		for d in "$DEV"/zd*; do
			case ${d##*/} in *p*) continue ;; esac
			[ -b "$d" ] || continue
			if [ "$("$Z/udev/zvol_id" "$d" 2>/dev/null)" = "$POOL/v$i" ]
			then
				echo "$d"
				break
			fi
		done
	done
}

zvols_wait() {
	local i
	for i in $(seq 1 60); do
		[ "$(zvols | wc -l)" -eq 4 ] && return 0
		udevadm settle
		sleep 1
	done
	die "the zvols of $POOL did not appear"
}

# arm_set ARM: the module parameters of ARM.
arm_set() {
	local w zi k v
	case $1 in
	master) w=$UPW; zi=0 ;;
	v10a) w=$UPW; zi=1 ;;
	v10a-c1) w=$C1W; zi=1 ;;
	*) die "unknown arm $1" ;;
	esac
	# No newline: the row parser takes it as part of the last mode.
	printf '%s' "$w" > "$P/zio_taskq_write" || die "zio_taskq_write"
	printf '%s' "$UPR" > "$P/zio_taskq_read" || die "zio_taskq_read"
	for k in zio_taskq_bulk_lowpri=0 zfs_arc_evict_lane=0 \
	    zio_taskq_bulk_yield=0 zio_taskq_write_pct=80 \
	    zio_taskq_write_light=0 spa_sync_light_pri=0 \
	    zio_taskq_write_budget=0 zio_taskq_bulk_pri_pct=100 \
	    zio_taskq_write_waited=0 "zil_issue_inline=$zi"; do
		echo "${k#*=}" > "$P/${k%%=*}" || die "${k%%=*}"
	done
	for k in zio_taskq_write:"$w" zil_issue_inline:"$zi"; do
		v=$(cat "$P/${k%%:*}")
		[ "$v" = "${k#*:}" ] || die "${k%%:*} reads '$v', not '${k#*:}'"
	done
}

params() {
	local p
	for p in zio_taskq_write zio_taskq_read zil_issue_inline \
	    zio_taskq_bulk_lowpri zio_taskq_bulk_yield zio_taskq_write_pct \
	    zio_taskq_write_light spa_sync_light_pri zfs_arc_evict_lane \
	    zio_taskq_write_budget zio_taskq_bulk_pri_pct \
	    zio_taskq_write_waited zio_taskq_batch_pct zfs_arc_max \
	    zfs_dirty_data_max zfs_commit_timeout_pct \
	    zfs_immediate_write_sz; do
		[ -r "$P/$p" ] && echo "$p $(cat "$P/$p")"
	done
}

# layout DIR: what the arm's import made, in phase_snap's formats.
layout() {
	grep -H '^threads_max' "$TQK"/z_wr_* > "$1/taskq.max" 2>/dev/null
	ps -eo ni=,comm= | grep -E 'z_wr_' | sort | uniq -c > "$1/taskq.nice"
	params > "$1/params"
}

# snap PREFIX: CPU, load and the write taskqs' and the ZIL's counters.
snap() {
	head -1 /proc/stat > "$1.stat"
	cat /proc/uptime > "$1.uptime"
	cat /proc/loadavg > "$1.loadavg"
	grep -H '^tasks_executed ' "$TQK"/z_wr_* > "$1.wrtasks" 2>/dev/null
	cat "$KS/zil" > "$1.zil" 2>/dev/null
}

setup() {
	local i d devs=()
	modprobe configfs
	mountpoint -q /sys/kernel/config ||
	    mount -t configfs none /sys/kernel/config
	modprobe null_blk nr_devices=0 || die "null_blk is not available"
	[ -d "$NB" ] || die "null_blk has no configfs"
	# zfs_arc_max as TASK-1116-C1B; zfs_dirty_data_max as Part A's run.sh.
	echo $((2 << 30)) > "$P/zfs_arc_max" || die "zfs_arc_max"
	echo $((1536 << 20)) > "$P/zfs_dirty_data_max" ||
	    die "zfs_dirty_data_max"
	for i in $(seq 0 $((NDEV - 1))); do
		nullb_add "nullb$i" "$i" "$DEV_MB" || die "nullb$i"
		devs+=("$DEV/nullb$i")
	done
	pool_op create -f -o ashift=12 -O atime=off "$POOL" raidz1 \
	    "${devs[@]}"
	for i in 0 1 2 3; do
		zfs create -V "$ZVOL_SIZE" -o volblocksize=16K "$POOL/v$i" ||
		    die "zfs create $POOL/v$i"
	done
	zvols_wait
	for d in $(zvols); do
		[ -b "$d" ] || die "$d is not a block device"
		fio --name=pz --filename="$d" --rw=write --bs=1M \
		    --size="$ZVOL_SIZE" --direct=1 --ioengine=psync \
		    >> "$OUTDIR/prefill.log" 2>&1 || die "prefill $d"
	done
	{
		echo "c1n: raidz1 of $NDEV x $DEV_MB MiB null_blk, ashift 12;" \
		    "zvols v0-v3 of $ZVOL_SIZE, volblocksize 16K, written" \
		    "in full"
		echo "zvols: $(zvols | tr '\n' ' ')"
		zpool list -v "$POOL"
		zfs get -r -H -o name,property,value \
		    volblocksize,sync,logbias,compression "$POOL"
		params
	} > "$OUTDIR/setup.txt" 2>&1
	log "setup done"
}

# row DIR: the row n8i into DIR.
row() {
	local dir=$1 args i d rc
	pool_op sync "$POOL"
	sleep 2
	# 8 O_SYNC writers, 2 on each of 4 zvols; options before the first
	# --name are global, the --filename after each --name is its job's.
	args=(--rw=randwrite --bs=64k --ioengine=psync --direct=1 --sync=1
	    --size="$ZVOL_SIZE" --numjobs=2 --group_reporting)
	i=0
	for d in $(zvols); do
		args+=(--name="n8i-v$i" --filename="$d")
		i=$((i + 1))
	done
	[ "$i" -eq 4 ] || die "n8i: found $i zvols"
	snap "$dir/pre"
	fio --time_based --runtime="$RT" --ramp_time="$RAMP" \
	    --lat_percentiles=1 --percentile_list=50:99:99.9 \
	    "${args[@]}" --output-format=json --output="$dir/n8i.json" \
	    > "$dir/fio.log" 2>&1
	rc=$?
	snap "$dir/post"
	echo "$rc" > "$dir/rc"
	log "row n8i rc=$rc"
}

pass() {
	local a d
	for a in "$@"; do
		case $a in
		master|v10a|v10a-c1) ;;
		*) die "unknown arm $a" ;;
		esac
	done
	for a in "$@"; do
		d=$OUTDIR/$a
		mkdir -p "$d" || die "mkdir $d"
		log "arm $a"
		arm_set "$a"
		udevadm settle
		pool_op export "$POOL"
		pool_op import -d "$DEV" "$POOL"
		zvols_wait
		layout "$d"
		sleep 5
		row "$d"
	done
}

cmd=${1:-}
OUTDIR=${2:-}
[ -n "$OUTDIR" ] || die "usage: x5-n8i.sh setup DIR | pass DIR ARM..."
mkdir -p "$OUTDIR" || die "mkdir $OUTDIR"
shift 2
case $cmd in
setup) [ $# -eq 0 ] || die "usage: x5-n8i.sh setup DIR"; setup ;;
pass) [ $# -gt 0 ] || die "usage: x5-n8i.sh pass DIR ARM..."; pass "$@" ;;
*) die "usage: x5-n8i.sh setup DIR | pass DIR ARM..." ;;
esac
