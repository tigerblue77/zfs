#!/bin/bash
# TASK-1118-V6CG driver: the identity of ZIL block writes issued by the
# writer that commits them.  Arms: master, v5 (ZIL blocks issued inline,
# bios with the committer's io cgroup and I/O priority), v6 (inline, bios
# with the write issue taskq's identity: root blkg, best-effort priority),
# all three release builds in one tree ($T, module/zfs.ko swapped per arm),
# and v6dbg (v6, debug build, its own tree $TD).
#
# Rows, each with fsync writer B (1 fio job, 4K random writes, fsync after
# each, unlimited) measured RT s after a RAMP s ramp:
#   CG  s0  B alone on c6g/fs
#       s1  A (4 jobs, io.max wiops=LIMIT on c6gb0) and B on c6g/fs
#       s2  A on c6g/fs, B on c6g/fs2
#   C   c1  A (4 jobs, cpu.max CPUQUOTA) and B on c6g/fs
#       c0e B alone on c6g/enc (aes-256-gcm)
#       c1e A (cpu.max CPUQUOTA) and B on c6g/enc
#   I   i0  C (buffered 1M writes, unlimited) on c6i/bulk, B on c6i/fs
#       i1  C, A (4 jobs, I/O class idle) and B on c6i/fs
#   D   d0  r1-like writer (12 s, 3 s ramp) on c6r/fs, no scope of its own,
#           checked to be in the root io cgroup
#       d1  the same in a scope with IOAccounting=yes
# c6g is on c6gb0 (memory-backed null_blk, as TASK-1117-V5CG2's c5gb0); c6i
# on c6gb1 (memory-backed, completion after I_NSEC ns, I_DEPTH requests at
# a time, one queue, mq-deadline, writeback throttling off); c6r on the
# test disk RDISK (or, with RDISK empty, D runs on c6g/fs2).
#
#   c6g.sh setup                  devices, master modules, pools, files
#   c6g.sh quiet SECONDS          CPU idle, iowait, steal, load1
#   c6g.sh check ARM              part Z (see check())
#   c6g.sh pass P "ARMS" ["ROWS"] the rows (default all) for each arm, in
#                                 that order; TRACE=1: c6g-clone.bt per row
#   c6g.sh teardown
set -u
D=/var/tmp/task-1118-v6cg
L=/var/log/task-1118-v6cg
T=$D/zfs                # release tree; module/zfs.ko is swapped per arm
TD=$D/dbg               # debug tree of v6 (arm v6dbg)
K=$D/ko                 # $K/<arm>/zfs.ko for master, v5, v6
H=$(dirname "$(readlink -f "$0")")
NB=c6gb0
NI=c6gb1
POOL=c6g
PI=c6i
PR=c6r
M=$D/mnt                # c6g
MI=$D/mnti              # c6i
MR=$D/mntr              # c6r
RDISK=${RDISK:-}
TRACE=${TRACE:-}
LIMIT=50                # A's write IOPS limit on /dev/$NB (s1, s2, Z)
CPUQUOTA=5%             # A's cpu.max (c1, c1e): 5000 100000
I_NSEC=500000           # c6gb1: ns per request
I_DEPTH=2               # c6gb1: requests in the device at once
I_NRREQ=64              # c6gb1: requests the scheduler may hold
RT=10                   # seconds measured per row (B), CG, C and I
RAMP=2
ROWS_ALL="s0 s1 s2 c1 c0e c1e i0 i1 d0 d1"
ARMS_ALL="master v5 v6 v6dbg"
export PATH="$T:$PATH"

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
die() { log "FATAL: $*"; exit 1; }

pool_in() { zpool list -H -o name "$1" > /dev/null 2>&1; }
rdev() { basename "$(readlink -f "$RDISK")"; }

nullb_add() {   # nullb_add NAME SIZE_MB: as TASK-1117-V5CG2's c5gb0
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

# nullb_slow NAME SIZE_MB: memory-backed, one queue, I_DEPTH requests at a
# time, each completed by a timer I_NSEC ns after it started.
nullb_slow() {
	local d=/sys/kernel/config/nullb/$1
	mkdir "$d" || return 1
	echo "$2" > "$d/size" || return 1
	echo 4096 > "$d/blocksize" || return 1
	echo 1 > "$d/memory_backed" || return 1
	echo 1 > "$d/submit_queues" || return 1
	echo "$I_DEPTH" > "$d/hw_queue_depth" || return 1
	echo 2 > "$d/irqmode" || return 1
	echo "$I_NSEC" > "$d/completion_nsec" || return 1
	echo 1 > "$d/power" || return 1
	udevadm settle
	[ -b "/dev/$1" ]
}

mkfile() {      # mkfile FILE SIZE
	fio --name=pf --filename="$1" --rw=write --bs=1M --size="$2" \
	    --ioengine=psync --end_fsync=1 >> "$L/prefill.log" 2>&1 ||
	    die "prefill $1"
}

# export_all: export the pools that are imported.
export_all() {
	local p
	for p in $POOL $PI $PR; do
		if pool_in "$p"; then zpool export "$p" || die "export $p"; fi
	done
}

load_arm() {    # load_arm ARM: unload, then load ARM's modules
	local want got tree=$T
	case " $ARMS_ALL " in *" $1 "*) ;; *) die "unknown arm $1" ;; esac
	if lsmod | grep -q '^zfs '; then
		export_all
		"$T/scripts/zfs.sh" -u || die "unload"
	fi
	if [ "$1" = v6dbg ]; then
		tree=$TD
	else
		[ -f "$K/$1/zfs.ko" ] || die "no $K/$1/zfs.ko"
		cp "$K/$1/zfs.ko" "$T/module/zfs.ko" || die "cp $1"
	fi
	"$tree/scripts/zfs.sh" || die "load $1"
	want=$(modinfo -F srcversion "$tree/module/zfs.ko")
	got=$(cat /sys/module/zfs/srcversion)
	if [ -z "$want" ] || [ "$want" != "$got" ]; then
		die "srcversion $got loaded, $1 built $want"
	fi
	echo "$1" > "$D/cur-arm"
	log "arm $1 loaded, srcversion $got"
}

# import_all: import the pools by name, load the key, read every file once
# so that no row starts on a cold ARC.
import_all() {
	zpool import -d "/dev/$NB" "$POOL" || die "import $POOL"
	zpool import -d "/dev/$NI" "$PI" || die "import $PI"
	if [ -n "$RDISK" ]; then
		zpool import -d /dev "$PR" || die "import $PR"
	fi
	zfs load-key "$POOL/enc" || die "load-key"
	zfs mount -a || die "mount"
	cat "$M"/fs/a.? "$M/fs/b" "$M/fs2/b" "$M"/enc/a.? "$M/enc/b" \
	    "$MI"/fs/a.? "$MI/fs/b" "$MI/bulk/c" "$(ddir)/d" > /dev/null ||
	    die "warm"
}

ddir() {        # ddir: the directory of D's file
	if [ -n "$RDISK" ]; then echo "$MR/fs"; else echo "$M/fs2"; fi
}

# root_io_off: stop the scopes' slice and wait until the io controller is
# off in the root cgroup, so that a process outside a scope of its own is
# in the root io cgroup.
root_io_off() {
	local i
	systemctl stop c6g.slice > /dev/null 2>&1
	for i in $(seq 1 30); do
		grep -qw io /sys/fs/cgroup/cgroup.subtree_control || return 0
		sleep 0.5
	done
	return 1
}

# scope UNIT OUT CLASS PROP... -- CMD...: run CMD in a transient scope of
# c6g.slice with the systemd properties PROP, at I/O class CLASS (- for
# unchanged).  OUT.txt: the cgroup and its id, io.max, cpu.max, the I/O
# priority, start and end times, CMD's rc, then io.stat and cpu.stat.
scope() {
	local unit=$1 out=$2 cls=$3 props=()
	shift 3
	while [ $# -gt 0 ] && [ "$1" != -- ]; do props+=(-p "$1"); shift; done
	[ $# -gt 1 ] || die "scope $unit: no command"
	shift
	systemd-run --quiet --scope --slice=c6g.slice --unit="$unit" \
	    "${props[@]}" -- /bin/sh -c '
cls=$1
shift
cg=$(sed -n "s/^0:://p" /proc/self/cgroup)
echo "cgroup $cg"
echo "cgroup.id $(stat -c %i "/sys/fs/cgroup$cg")"
echo "io.max: $(cat "/sys/fs/cgroup$cg/io.max" 2>&1)"
echo "cpu.max: $(cat "/sys/fs/cgroup$cg/cpu.max" 2>&1)"
[ "$cls" = - ] || ionice -c "$cls" -p $$
echo "ioprio: $(ionice -p $$)"
echo "start $(date +%s.%N)"
"$@"
rc=$?
echo "end $(date +%s.%N)"
echo "rc $rc"
echo "io.stat:"
cat "/sys/fs/cgroup$cg/io.stat" 2>&1
echo "cpu.stat:"
cat "/sys/fs/cgroup$cg/cpu.stat" 2>&1
exit $rc' sh "$cls" "$@" > "$out.txt" 2>&1
}

# noscope OUT CMD...: run CMD from here, after checking that the io
# controller is off in the root cgroup; OUT.txt as scope() writes it.
noscope() {
	local out=$1 cg rc
	shift
	root_io_off || { echo "io on in the root cgroup" > "$out.txt"; return 99; }
	cg=$(sed -n 's/^0:://p' /proc/self/cgroup)
	{
		echo "cgroup $cg (no scope)"
		echo "cgroup.id $(stat -c %i "/sys/fs/cgroup$cg")"
		echo "root subtree_control" \
		    "$(cat /sys/fs/cgroup/cgroup.subtree_control)"
		echo "ioprio: $(ionice -p $$)"
		echo "start $(date +%s.%N)"
	} > "$out.txt"
	"$@" >> "$out.txt" 2>&1
	rc=$?
	{
		echo "end $(date +%s.%N)"
		echo "root subtree_control" \
		    "$(cat /sys/fs/cgroup/cgroup.subtree_control)"
		echo "rc $rc"
	} >> "$out.txt"
	return $rc
}

# ddsample OUT SECONDS: mq-deadline's queued requests on $NI per priority
# class (RT BE IDLE), 10 times a second.
ddsample() {
	local f=/sys/kernel/debug/block/$NI/sched/queued end
	if [ ! -r "$f" ]; then
		echo "no $f" > "$1"
		return
	fi
	end=$(($(date +%s) + $2))
	while [ "$(date +%s)" -lt "$end" ]; do
		printf '%s %s\n' "$(date +%s.%N)" "$(cat "$f")"
		sleep 0.1
	done > "$1"
}

# bt_start OUT / bt_stop: c6g-clone.bt around a row (TRACE=1).
bt_start() {
	local i
	bpftrace -B line "$H/c6g-clone.bt" > "$1" 2>&1 &
	BT_PID=$!
	for i in $(seq 1 60); do
		grep -q '^READY' "$1" && return 0
		kill -0 "$BT_PID" 2> /dev/null ||
		    die "bpftrace exited: $(tail -3 "$1")"
		sleep 0.5
	done
	kill -INT "$BT_PID"
	die "bpftrace did not attach in 30 s"
}

bt_stop() {
	kill -INT "$BT_PID" || die "bpftrace is gone"
	wait "$BT_PID"
}

row() {         # row TAG ARM PASS
	local tag=$1 arm=$2 p=$3 dir=$L/raw/$3/$2 rc pa="" pc="" pd=""
	local adir bdir pool aprops=() acls=- alead=1 b=() a=()
	local common=(--ioengine=psync --rw=randwrite --bs=4k --fsync=1
	    --norandommap --randrepeat=0 --time_based --lat_percentiles=1
	    --percentile_list=50:99:99.9 --output-format=json)
	mkdir -p "$dir" || die "mkdir $dir"
	case $tag in
	s0|s1|c1) adir=$M/fs; bdir=$M/fs; pool=$POOL ;;
	s2) adir=$M/fs; bdir=$M/fs2; pool=$POOL ;;
	c0e|c1e) adir=$M/enc; bdir=$M/enc; pool=$POOL ;;
	i0|i1) adir=$MI/fs; bdir=$MI/fs; pool=$PI ;;
	d0|d1) adir=""; bdir=$(ddir); pool=$POOL
		[ -n "$RDISK" ] && pool=$PR ;;
	*) die "unknown row $tag" ;;
	esac
	case $tag in
	s1|s2) aprops=(IOAccounting=yes "IOWriteIOPSMax=/dev/$NB $LIMIT") ;;
	c1|c1e) aprops=("CPUQuota=$CPUQUOTA"); alead=3 ;;
	i1) aprops=(IOAccounting=yes); acls=3 ;;
	esac
	if [ "${tag#d}" != "$tag" ]; then
		# r1-like: TASK-1117-V5P's r1 job, 12 s after a 3 s ramp.
		b=(fio --name=d --filename="$bdir/d" --size="$(dsize)"
		    --runtime=12 --ramp_time=3 "${common[@]}"
		    --output="$dir/$tag.b.json")
	else
		b=(fio --name=b --filename="$bdir/b" --size=64M --runtime="$RT"
		    --ramp_time="$RAMP" "${common[@]}"
		    --output="$dir/$tag.b.json")
	fi
	a=(fio --name=a --numjobs=4 --group_reporting
	    --filename_format="$adir/a.\$jobnum" --size=16M
	    --runtime=$((alead + RT + RAMP + 3)) --ramp_time=1 "${common[@]}"
	    --output="$dir/$tag.a.json")
	zpool sync "$POOL" "$PI" ${RDISK:+"$PR"}
	sleep 1
	cat "/proc/spl/kstat/zfs/$pool/txgs" > "$dir/$tag.txgs0"
	cat /proc/spl/kstat/zfs/zil > "$dir/$tag.zil0"
	[ -n "$TRACE" ] && bt_start "$dir/$tag.bt.txt"
	if [ "$tag" = i0 ] || [ "$tag" = i1 ]; then
		fio --name=c --filename="$MI/bulk/c" --rw=write --bs=1M \
		    --size=256M --ioengine=psync --time_based \
		    --runtime=$((4 + alead + RAMP + RT + 4)) \
		    --output-format=json --output="$dir/$tag.c.json" \
		    > /dev/null 2>&1 &
		pc=$!
		ddsample "$dir/$tag.dd.txt" $((4 + alead + RAMP + RT + 2)) &
		pd=$!
		sleep 4
	fi
	if [ ${#aprops[@]} -gt 0 ]; then
		scope "c6g-a-$tag-$arm-$p" "$dir/$tag.a" "$acls" \
		    "${aprops[@]}" -- "${a[@]}" &
		pa=$!
		sleep "$alead"
	fi
	if [ "$tag" = d0 ]; then
		noscope "$dir/$tag.b" "${b[@]}"
	else
		scope "c6g-b-$tag-$arm-$p" "$dir/$tag.b" - IOAccounting=yes \
		    -- "${b[@]}"
	fi
	rc=$?
	if [ -n "$pa" ]; then
		wait "$pa" || rc=$((rc + 100))
	fi
	if [ -n "$pc" ]; then
		wait "$pc" || rc=$((rc + 1000))
	fi
	[ -n "$pd" ] && wait "$pd"
	[ -n "$TRACE" ] && bt_stop
	cat /proc/spl/kstat/zfs/zil > "$dir/$tag.zil1"
	cat "/proc/spl/kstat/zfs/$pool/txgs" > "$dir/$tag.txgs1"
	systemctl stop c6g.slice > /dev/null 2>&1
	log "row $tag arm $arm pass $p rc=$rc"
}

dsize() { if [ -n "$RDISK" ]; then echo 1G; else echo 256M; fi; }

layout() {      # layout FILE
	{
		echo "arm $(cat "$D/cur-arm")"
		echo "srcversion $(cat /sys/module/zfs/srcversion)"
		dmesg | grep 'ZFS: Loaded module' | tail -1
		echo "calling_thread_io" \
		    "$(cat /sys/module/zfs/parameters/zfs_vdev_disk_calling_thread_io)"
		echo "root subtree_control" \
		    "$(cat /sys/fs/cgroup/cgroup.subtree_control)"
		for d in "$NB" "$NI" ${RDISK:+"$(rdev)"}; do
			echo "$d dev $(cat "/sys/block/$d/dev")" \
			    "scheduler $(cat "/sys/block/$d/queue/scheduler")" \
			    "nr_requests $(cat "/sys/block/$d/queue/nr_requests")" \
			    "wbt_lat_usec $(cat "/sys/block/$d/queue/wbt_lat_usec" 2>&1)" \
			    "rotational $(cat "/sys/block/$d/queue/rotational")"
		done
		echo "$NI prio_aging_expire" \
		    "$(cat "/sys/block/$NI/queue/iosched/prio_aging_expire" 2>&1)" \
		    "completion_nsec" \
		    "$(cat "/sys/kernel/config/nullb/$NI/completion_nsec")" \
		    "hw_queue_depth" \
		    "$(cat "/sys/kernel/config/nullb/$NI/hw_queue_depth")"
		zpool list -v "$POOL" "$PI" ${RDISK:+"$PR"}
		zfs get -H -r recordsize,compression,encryption,sync,logbias \
		    "$POOL" "$PI" ${RDISK:+"$PR"}
	} > "$1" 2>&1
}

setup() {
	local i
	mkdir -p "$M" "$MI" "$MR" "$L/raw" || die "mkdir"
	lsmod | grep -q '^null_blk ' && die "null_blk is already loaded"
	modprobe null_blk nr_devices=0 || die "null_blk is not available"
	nullb_add "$NB" 1024 || die "null_blk $NB"
	nullb_slow "$NI" 1024 || die "null_blk $NI"
	cat "/sys/block/$NB/dev" > "$L/majmin" || die "majmin"
	cat "/sys/block/$NI/dev" > "$L/majmin-i" || die "majmin-i"
	modprobe mq-deadline > /dev/null 2>&1
	echo mq-deadline > "/sys/block/$NI/queue/scheduler" ||
	    die "mq-deadline is not available"
	grep -q '\[mq-deadline\]' "/sys/block/$NI/queue/scheduler" ||
	    die "scheduler of $NI: $(cat "/sys/block/$NI/queue/scheduler")"
	echo "$I_NRREQ" > "/sys/block/$NI/queue/nr_requests" ||
	    die "nr_requests $NI"
	echo 0 > "/sys/block/$NI/queue/wbt_lat_usec" ||
	    log "wbt_lat_usec of $NI could not be set to 0"
	if [ ! -r "/sys/kernel/debug/block/$NI/sched/queued" ]; then
		mount -t debugfs none /sys/kernel/debug > /dev/null 2>&1
	fi
	[ -r "/sys/kernel/debug/block/$NI/sched/queued" ] ||
	    log "no /sys/kernel/debug/block/$NI/sched/queued: I's queue samples will be empty"
	if [ -n "$RDISK" ]; then
		[ -b "$RDISK" ] || die "$RDISK is not a block device"
		cat "/sys/block/$(rdev)/dev" > "$L/majmin-r" || die "majmin-r"
	fi
	load_arm master
	od -An -tx1 -N32 /dev/urandom | tr -d ' \n' > "$D/key" || die "key"
	chmod 600 "$D/key"
	zpool create -f -o ashift=12 -O atime=off -O recordsize=4K -m "$M" \
	    "$POOL" "/dev/$NB" || die "zpool create $POOL"
	zfs create "$POOL/fs" || die "fs"
	zfs create "$POOL/fs2" || die "fs2"
	zfs create -o encryption=aes-256-gcm -o keyformat=hex \
	    -o keylocation="file://$D/key" "$POOL/enc" || die "enc"
	zpool create -f -o ashift=12 -O atime=off -O recordsize=4K \
	    -O compression=off -m "$MI" "$PI" "/dev/$NI" ||
	    die "zpool create $PI"
	zfs create "$PI/fs" || die "$PI/fs"
	zfs create -o recordsize=128K "$PI/bulk" || die "$PI/bulk"
	if [ -n "$RDISK" ]; then
		zpool create -f -o ashift=12 -O atime=off -O recordsize=4K \
		    -m "$MR" "$PR" "$RDISK" || die "zpool create $PR"
		zfs create "$PR/fs" || die "$PR/fs"
	fi
	for i in 0 1 2 3; do
		mkfile "$M/fs/a.$i" 16M
		mkfile "$M/enc/a.$i" 16M
		mkfile "$MI/fs/a.$i" 16M
	done
	mkfile "$M/fs/b" 64M
	mkfile "$M/fs2/b" 64M
	mkfile "$M/enc/b" 64M
	mkfile "$M/fs/zc" 2M
	mkfile "$MI/fs/b" 64M
	mkfile "$MI/bulk/c" 256M
	mkfile "$(ddir)/d" "$(dsize)"
	zpool sync
	export_all
	log "setup done: $NB is $(cat "$L/majmin"), $NI is $(cat "$L/majmin-i")" \
	    "${RDISK:+, $PR on $(rdev) $(cat "$L/majmin-r")}"
}

# zwios FILE: the writes io.stat charged in FILE to the device of $NB.
zwios() {
	awk -v mm="$(cat "$L/majmin")" '
		$1 == "io.stat:" { s = 1; next }
		$1 == "cpu.stat:" { s = 0 }
		s && $1 == mm { for (i = 2; i <= NF; i++)
			if ($i ~ /^wios=/) { sub(/^wios=/, "", $i); print $i; f = 1 } }
		END { if (!f) print 0 }' "$1"
}

# check ARM: part Z, the proposed deterministic test.  Z1: the pool is
# imported from here (no io cgroup of its own) and a txg synced, then dd
# writes 300 blocks of 4K with O_DSYNC in a scope held to wiops=LIMIT on
# $NB; the writes io.stat charges to that scope on $NB are counted.  Z2:
# the same, the pool imported by a process in a scope with an io cgroup of
# its own and dd started at once (the window before v6 has its root blkg
# donor).  PASS when at most 3 writes are charged.
check() {
	local arm=$1 dir=$L/raw/Z/$1 z w
	local dd=(timeout 300 dd if=/dev/urandom of="$M/fs/zc" bs=4k count=300
	    oflag=dsync conv=notrunc status=none)
	mkdir -p "$dir" || die "mkdir $dir"
	load_arm "$arm"
	root_io_off || die "the io controller stays on in the root cgroup"
	zpool import -d "/dev/$NB" "$POOL" || die "import $POOL"
	zfs load-key "$POOL/enc" > /dev/null 2>&1
	zpool sync "$POOL"
	sleep 1
	scope "c6g-z1-$arm" "$dir/z1" - IOAccounting=yes \
	    "IOWriteIOPSMax=/dev/$NB $LIMIT" -- "${dd[@]}"
	zpool export "$POOL" || die "export $POOL"
	scope "c6g-z2i-$arm" "$dir/z2i" - IOAccounting=yes -- \
	    zpool import -d "/dev/$NB" "$POOL" || die "import $POOL (z2)"
	scope "c6g-z2-$arm" "$dir/z2" - IOAccounting=yes \
	    "IOWriteIOPSMax=/dev/$NB $LIMIT" -- "${dd[@]}"
	zpool export "$POOL" || die "export $POOL"
	systemctl stop c6g.slice > /dev/null 2>&1
	for z in z1 z2; do
		w=$(zwios "$dir/$z.txt")
		if [ "$w" -le 3 ]; then
			log "check $z arm $arm wios=$w PASS"
		else
			log "check $z arm $arm wios=$w FAIL"
		fi
	done
}

pass() {        # pass P "ARMS" ["ROWS"]
	local p=$1 rows=${3:-$ROWS_ALL} arm tag
	for arm in $2; do
		case " $ARMS_ALL " in *" $arm "*) ;; *) die "unknown arm $arm" ;; esac
	done
	for tag in $rows; do
		case " $ROWS_ALL " in *" $tag "*) ;; *) die "unknown row $tag" ;; esac
	done
	if [ -n "$TRACE" ]; then
		command -v bpftrace > /dev/null || die "TRACE=1 needs bpftrace"
	fi
	log "pass $p start: arms $2, rows $rows${TRACE:+, traced}"
	for arm in $2; do
		load_arm "$arm"
		import_all
		mkdir -p "$L/raw/$p/$arm" || die "mkdir"
		layout "$L/raw/$p/$arm/layout.txt"
		for tag in $rows; do row "$tag" "$arm" "$p"; done
		export_all
	done
	log "pass $p done"
}

quiet() {       # quiet SECONDS: as TASK-1118-V6P's quiet check
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

teardown() {
	local p d
	if lsmod | grep -q '^zfs '; then
		pool_in "$POOL" || zpool import -d "/dev/$NB" "$POOL" > /dev/null 2>&1
		pool_in "$PI" || zpool import -d "/dev/$NI" "$PI" > /dev/null 2>&1
		if [ -n "$RDISK" ] && ! pool_in "$PR"; then
			zpool import -d /dev "$PR" > /dev/null 2>&1
		fi
		for p in $POOL $PI $PR; do
			if pool_in "$p"; then
				zpool destroy "$p" || log "destroy $p failed"
			fi
		done
		if [ -n "$RDISK" ] && [ -b "$RDISK-part1" ]; then
			zpool labelclear -f "$RDISK-part1"
		fi
		"$T/scripts/zfs.sh" -u || log "unload failed"
	fi
	for d in "$NB" "$NI"; do
		if [ -d "/sys/kernel/config/nullb/$d" ]; then
			echo 0 > "/sys/kernel/config/nullb/$d/power"
			rmdir "/sys/kernel/config/nullb/$d" || log "rmdir $d failed"
		fi
	done
	if lsmod | grep -q '^null_blk '; then
		modprobe -r null_blk || log "null_blk unload failed"
	fi
	if [ -n "$RDISK" ] && [ -b "$RDISK" ]; then
		wipefs -a "$RDISK" > /dev/null
		blkdiscard -f "$RDISK" 2> /dev/null
	fi
	systemctl stop c6g.slice > /dev/null 2>&1
	systemctl reset-failed 'c6g-*' > /dev/null 2>&1
	rm -f "$D/key"
	log "teardown done"
}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
mkdir -p "$L" "$D" || exit 1
command -v zpool | grep -qx "$T/zpool" || die "zpool is not $T/zpool"
case "${1:-}" in
setup) setup ;;
quiet) [ $# -eq 2 ] || die "usage: quiet SECONDS"; quiet "$2" ;;
check) [ $# -eq 2 ] || die "usage: check ARM"; check "$2" ;;
pass) [ $# -eq 3 ] || [ $# -eq 4 ] || die "usage: pass P ARMS [ROWS]"
	pass "$2" "$3" "${4:-}" ;;
teardown) teardown ;;
*) die "usage: $0 setup | quiet S | check ARM | pass P ARMS [ROWS] | teardown" ;;
esac
