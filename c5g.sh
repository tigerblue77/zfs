#!/bin/bash
# TASK-1117-V5CG driver.  Two fio fsync writers in two io cgroups (transient
# systemd scopes): A, 4 jobs, limited by io.max wiops on the pool's only
# disk; B, 1 job, unlimited.  Rows: s0 B alone; s1 A and B on the same
# dataset (one ZIL); s2 A and B on two datasets of the same pool.
#   c5g.sh setup             null_blk, master modules, pool, files
#   c5g.sh pass P "ARMS"     rows s0 s1 s2 for each arm, in that order
#   c5g.sh teardown
set -u
D=/var/tmp/task-1117-v5cg
L=/var/log/task-1117-v5cg
T=$D/zfs                # the one tree; module/zfs.ko is swapped per arm
K=$D/ko                 # $K/<arm>/zfs.ko
NB=c5gb0
POOL=c5g
M=$D/mnt
LIMIT=50                # A's write IOPS limit on /dev/$NB
RT=10                   # seconds measured per row (B)
RAMP=2
export PATH="$T:$PATH"

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
die() { log "FATAL: $*"; exit 1; }

pool_in() { zpool list -H -o name "$POOL" > /dev/null 2>&1; }

nullb_add() {   # nullb_add NAME SIZE_MB: a memory-backed null_blk device
	local d=/sys/kernel/config/nullb/$1
	mkdir "$d" || return 1
	echo "$2" > "$d/size" || return 1
	echo 4096 > "$d/blocksize" || return 1
	echo 1 > "$d/memory_backed" || return 1
	echo 1 > "$d/irqmode"
	echo 1 > "$d/power" || return 1
	udevadm settle
	[ -b "/dev/$1" ]
}

mkfile() {      # mkfile FILE SIZE
	fio --name=pf --filename="$1" --rw=write --bs=1M --size="$2" \
	    --ioengine=psync --end_fsync=1 >> "$L/prefill.log" 2>&1 ||
	    die "prefill $1"
}

load_arm() {    # load_arm ARM: swap in ARM's zfs.ko and load it
	local want got
	[ -f "$K/$1/zfs.ko" ] || die "no $K/$1/zfs.ko"
	if lsmod | grep -q '^zfs '; then
		if pool_in; then zpool export "$POOL" || die "export"; fi
		"$T/scripts/zfs.sh" -u || die "unload"
	fi
	cp "$K/$1/zfs.ko" "$T/module/zfs.ko" || die "cp $1"
	"$T/scripts/zfs.sh" || die "load $1"
	want=$(modinfo -F srcversion "$K/$1/zfs.ko")
	got=$(cat /sys/module/zfs/srcversion)
	if [ -z "$want" ] || [ "$want" != "$got" ]; then
		die "srcversion $got loaded, $1 built $want"
	fi
	log "arm $1 loaded, srcversion $got"
}

# scope UNIT OUT LIMIT CMD...: run CMD in a transient scope of c5g.slice
# with io accounting, plus io.max wiops=LIMIT on /dev/$NB unless LIMIT
# is 0.  OUT.txt: the cgroup, its io.max, CMD's rc and the io.stat after.
scope() {
	local unit=$1 out=$2 lim=$3
	shift 3
	local props=(-p IOAccounting=yes)
	[ "$lim" != 0 ] && props+=(-p "IOWriteIOPSMax=/dev/$NB $lim")
	systemd-run --quiet --scope --slice=c5g.slice --unit="$unit" \
	    "${props[@]}" -- /bin/sh -c '
cg=$(sed -n "s/^0:://p" /proc/self/cgroup)
echo "cgroup $cg"
echo "io.max: $(cat "/sys/fs/cgroup$cg/io.max" 2>&1)"
"$@"
rc=$?
echo "rc $rc"
echo "io.stat:"
cat "/sys/fs/cgroup$cg/io.stat" 2>&1
exit $rc' sh "$@" > "$out.txt" 2>&1
}

row() {         # row TAG ARM PASS
	local tag=$1 arm=$2 p=$3 dir=$L/raw/$3/$2 bdir=$M/fs pa="" rc
	local common=(--ioengine=psync --rw=randwrite --bs=4k --fsync=1
	    --norandommap --randrepeat=0 --time_based --lat_percentiles=1
	    --percentile_list=50:99:99.9 --output-format=json)
	mkdir -p "$dir" || die "mkdir $dir"
	[ "$tag" = s2 ] && bdir=$M/fs2
	zpool sync "$POOL"
	sleep 1
	cat "/proc/spl/kstat/zfs/$POOL/txgs" > "$dir/$tag.txgs0"
	if [ "$tag" != s0 ]; then
		scope "c5g-a-$tag-$arm-$p" "$dir/$tag.a" "$LIMIT" \
		    fio --name=a --numjobs=4 --group_reporting \
		    --filename_format="$M/fs/a.\$jobnum" --size=16M \
		    --runtime=$((RT + RAMP + 3)) --ramp_time=1 "${common[@]}" \
		    --output="$dir/$tag.a.json" &
		pa=$!
		sleep 1
	fi
	scope "c5g-b-$tag-$arm-$p" "$dir/$tag.b" 0 \
	    fio --name=b --filename="$bdir/b" --size=64M \
	    --runtime="$RT" --ramp_time="$RAMP" "${common[@]}" \
	    --output="$dir/$tag.b.json"
	rc=$?
	if [ -n "$pa" ]; then
		wait "$pa" || rc=$((rc + 100))
	fi
	cat "/proc/spl/kstat/zfs/$POOL/txgs" > "$dir/$tag.txgs1"
	log "row $tag arm $arm pass $p rc=$rc"
}

layout() {      # layout FILE
	{
		echo "arm $(cat "$D/cur-arm")"
		echo "srcversion $(cat /sys/module/zfs/srcversion)"
		echo "calling_thread_io" \
		    "$(cat /sys/module/zfs/parameters/zfs_vdev_disk_calling_thread_io)"
		echo "scheduler $(cat "/sys/block/$NB/queue/scheduler")"
		echo "wbt_lat_usec $(cat "/sys/block/$NB/queue/wbt_lat_usec" 2>&1)"
		echo "root subtree_control" \
		    "$(cat /sys/fs/cgroup/cgroup.subtree_control)"
		zpool list -v "$POOL"
		zfs get -H -r recordsize,sync,logbias,mountpoint "$POOL"
	} > "$1" 2>&1
}

setup() {
	mkdir -p "$M" "$L/raw" || die "mkdir"
	lsmod | grep -q '^null_blk ' && die "null_blk is already loaded"
	modprobe null_blk nr_devices=0 || die "null_blk is not available"
	nullb_add "$NB" 1024 || die "null_blk $NB"
	cat "/sys/block/$NB/dev" > "$L/majmin" || die "majmin"
	load_arm master
	echo master > "$D/cur-arm"
	zpool create -f -o ashift=12 -O atime=off -O recordsize=4K -m "$M" \
	    "$POOL" "/dev/$NB" || die "zpool create"
	zfs create "$POOL/fs" || die "fs"
	zfs create "$POOL/fs2" || die "fs2"
	for i in 0 1 2 3; do mkfile "$M/fs/a.$i" 16M; done
	mkfile "$M/fs/b" 64M
	mkfile "$M/fs2/b" 64M
	zpool sync "$POOL"
	zpool export "$POOL" || die "export"
	log "setup done, /dev/$NB is $(cat "$L/majmin")"
}

pass() {        # pass P "ARMS"
	local p=$1 arm tag
	for arm in $2; do
		load_arm "$arm"
		echo "$arm" > "$D/cur-arm"
		zpool import -d "/dev/$NB" "$POOL" || die "import"
		# Read every file once, so that no row starts on a cold ARC.
		cat "$M"/fs/a.? "$M/fs/b" "$M/fs2/b" > /dev/null || die "warm"
		mkdir -p "$L/raw/$p/$arm" || die "mkdir"
		layout "$L/raw/$p/$arm/layout.txt"
		for tag in s0 s1 s2; do row "$tag" "$arm" "$p"; done
		zpool export "$POOL" || die "export"
	done
	log "pass $p done"
}

teardown() {
	if lsmod | grep -q '^zfs '; then
		pool_in || zpool import -d "/dev/$NB" "$POOL" > /dev/null 2>&1
		if pool_in; then zpool destroy "$POOL" || log "destroy failed"; fi
		"$T/scripts/zfs.sh" -u || log "unload failed"
	fi
	if [ -d "/sys/kernel/config/nullb/$NB" ]; then
		echo 0 > "/sys/kernel/config/nullb/$NB/power"
		rmdir "/sys/kernel/config/nullb/$NB" || log "rmdir $NB failed"
	fi
	if lsmod | grep -q '^null_blk '; then
		modprobe -r null_blk || log "null_blk unload failed"
	fi
	systemctl stop c5g.slice > /dev/null 2>&1
	systemctl reset-failed 'c5g-*' > /dev/null 2>&1
	log "teardown done"
}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
mkdir -p "$L" "$D" || exit 1
command -v zpool | grep -qx "$T/zpool" || die "zpool is not $T/zpool"
case "${1:-}" in
setup) setup ;;
pass) [ $# -eq 3 ] || die "usage: pass P ARMS"; pass "$2" "$3" ;;
teardown) teardown ;;
*) die "usage: $0 setup | pass P ARMS | teardown" ;;
esac
