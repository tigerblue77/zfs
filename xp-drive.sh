#!/bin/sh
# xp-drive.sh: driver of TASK-1120-X3 and TASK-1120-X4, the XPOOL bench
# (run.sh qv from this kit, one invocation per pass) on one release tree,
# $T/zfs, whose modules stay loaded from "load" to "end".  Run as root.
#
#   xp-drive.sh TASK load             load $T/zfs's modules and check them
#   xp-drive.sh TASK state            the checks before a measured part
#   xp-drive.sh TASK pass smoke 1     the functional check before GO: three
#                                     arms, rows rr1 and sw1 of 8 s, not
#                                     measured
#   xp-drive.sh TASK go               record the time of main's GO
#   xp-drive.sh TASK quiet            the quiet check (exit 4: too much steal)
#   xp-drive.sh TASK pass GROUP K [rerun]
#                                     pass K of GROUP: probe, run.sh qv with
#                                     the group's settings and the rotated
#                                     arms, checks, teardown
#   xp-drive.sh TASK timerule         after run2's pass 1: the time rule
#   xp-drive.sh TASK rest             every other planned pass, failed
#                                     passes run once more per group
#   xp-drive.sh TASK plan             the arms and passes still planned
#   xp-drive.sh TASK teardown         remove this task's pools and devices
#   xp-drive.sh TASK end              teardown, unload, restore the sysctls
#
# TASK is x3 or x4.  Exit status: 0 done; 1 STOP (driver.log says why);
# 2 a pass with a failed row (run it again with "rerun" at the end of its
# group); 4 the quiet check failed twice.  The XP_* variables below exist
# only for the kit's mock tests; the task sets none of them.
set -u

TASK=${1:-}
case $TASK in
x3|x4) ;;
*) echo "usage: xp-drive.sh x3|x4 COMMAND ..." >&2; exit 1 ;;
esac
shift
T=${XP_T:-/var/tmp/task-1120-$TASK}
L=${XP_L:-/var/log/task-1120-$TASK}
H=$(cd "$(dirname "$0")" && pwd)
Z=$T/zfs
P=${XP_PARAMS:-/sys/module/zfs/parameters}
KS=${XP_KSTAT:-/proc/spl/kstat/zfs}
NB=${XP_NULLB:-/sys/kernel/config/nullb}
MEMINFO=${XP_MEMINFO:-/proc/meminfo}
FIOFAST=${XP_FIOFAST:-/usr/local/bin/fiofast}
SRCV=${XP_SRCV:-/sys/module/zfs/srcversion}
KMSG=${XP_KMSG:-/dev/kmsg}
SYSRQ=${XP_SYSRQ:-/proc/sysrq-trigger}
ZCACHE=${XP_ZCACHE:-/etc/zfs/zpool.cache}
POOLS_OK="fastq fastr slow1 slow2 slow3"
PATH="$Z:$PATH"
export PATH

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$L/driver.log"; }
stop() { log "STOP: $*"; exit 1; }

# ---------------------------------------------------------------- arms --

# spec ARM: the QV_PHASES entry of ARM.
spec() {
	case $1 in
	m3w|m3w-up2) echo "$1:write" ;;
	m3w-gate-on) echo "$1:write:wb=1" ;;
	m3w-pct25w) echo "$1:write:pct=25,wpct=25" ;;
	m3w-c1) echo "$1:write:kn=c1" ;;
	m3w-wiss2) echo "$1:write:wiss=2" ;;
	m3w-gate-on-c1) echo "$1:write:kn=c1,wb=1" ;;
	m3w-c1-pct25w) echo "$1:write:kn=c1,pct=25,wpct=25" ;;
	m3w-wint2) echo "$1:write:wint=2" ;;
	m3w-c1-pct25) echo "$1:write:kn=c1,pct=25" ;;
	m3w-pct25) echo "$1:write:pct=25" ;;
	alone) echo "$1:none" ;;
	alone-gate-on) echo "$1:none:wb=1" ;;
	tput) echo "$1:none:dirty=268435456" ;;
	tput-gate-on) echo "$1:none:wb=1,dirty=268435456" ;;
	m3w-n10) echo "$1:write:kn=w,bnice=-10" ;;
	m3w-w2|m3w-w2b) echo "$1:write:kn=w2" ;;
	m3w-lowslow) echo "$1:write:kn=c1,slowlow=1" ;;
	m3w-n5) echo "$1:write:kn=w,bnice=-5" ;;
	m3w-w2x) echo "$1:write:kn=w2,ww=2" ;;
	m3w-lowpri) echo "$1:write:kn=w" ;;
	m3w-lowfast) echo "$1:write:kn=w,slowlow=0" ;;
	m3w-w2x1) echo "$1:write:kn=w2,ww=1" ;;
	*) return 1 ;;
	esac
}

# groups: the groups of the task, in the order they run.
groups() {
	case $TASK in
	x3) echo "run2 single tput" ;;
	x4) echo "run2" ;;
	esac
}

# arms0 GROUP: the arms in pass-1 order; passes0 GROUP: the planned passes.
arms0() {
	case $TASK.$1 in
	x3.run2) echo "m3w m3w-gate-on m3w-pct25w m3w-c1 m3w-wiss2" \
	    "m3w-gate-on-c1 m3w-up2 m3w-c1-pct25w m3w-wint2 m3w-c1-pct25" \
	    "m3w-pct25" ;;
	x3.single) echo "alone alone-gate-on" ;;
	x3.tput) echo "tput tput-gate-on" ;;
	x4.run2) echo "m3w m3w-n10 m3w-w2 m3w-lowslow m3w-c1 m3w-up2 m3w-n5" \
	    "m3w-w2x m3w-lowpri m3w-lowfast m3w-w2b m3w-w2x1" ;;
	x3.smoke) echo "m3w-gate-on-c1 m3w-pct25w m3w-wiss2" ;;
	x4.smoke) echo "m3w-w2x m3w-n10 m3w-lowslow" ;;
	*) return 1 ;;
	esac
}

passes0() {
	case $TASK.$1 in
	x3.run2|x4.run2) echo 3 ;;
	x3.single|x3.tput) echo 4 ;;
	x3.smoke|x4.smoke) echo 1 ;;
	*) return 1 ;;
	esac
}

# genv GROUP: the run.sh settings of GROUP, as NAME=VALUE words.
genv() {
	case $1 in
	run2) echo "QV_POOLS=3n QV3N_DEV_MB=128 FAST_DEV_MB=1024" \
	    "Q2_RUNTIME=20 QV_SLOW_WRITE_MB=256 REPS=1 QV_DIAG=0" ;;
	single) echo "QV_POOLS=0 FAST_DEV_MB=1024 Q2_RUNTIME=20 REPS=1" \
	    "QV_DIAG=0" ;;
	tput) echo "QV_POOLS=0 FAST_DEV_MB=1024 SIZE_MB=64 QV_TZ_MB=512" \
	    "Q2_RUNTIME=20 REPS=1 QV_DIAG=0" ;;
	smoke) echo "QV_POOLS=3n QV3N_DEV_MB=128 FAST_DEV_MB=1024" \
	    "Q2_RUNTIME=8 QV_WARM_WRITE=5 QV_SLOW_WRITE_MB=256 REPS=1" \
	    "QV_DIAG=0" ;;
	esac
	[ "$TASK" = x4 ] && echo "QV_FIOLOG=1"
	return 0
}

# gwl GROUP: the rows of GROUP; glimit GROUP: run.sh's time limit (s).
gwl() {
	case $1 in
	run2) echo "rr1 rr4 sw1 sw4" ;;
	single) echo "sw1 sw4" ;;
	tput) echo "tz tz2" ;;
	smoke) echo "rr1 sw1" ;;
	esac
}

glimit() {
	case $1 in
	run2) echo 3000 ;;
	single) echo 600 ;;
	tput) echo 900 ;;
	smoke) echo 600 ;;
	esac
}

plan_arms() { sed -n 's/^arms //p' "$T/plan.$1"; }
plan_passes() { sed -n 's/^passes //p' "$T/plan.$1"; }

plan_init() {
	local g
	for g in $(groups) smoke; do
		[ -f "$T/plan.$g" ] && continue
		printf 'passes %s\narms %s\n' "$(passes0 "$g")" \
		    "$(arms0 "$g")" > "$T/plan.$g"
	done
}

# rotate OFFSET WORD...: the words starting at the OFFSET-th (from 0),
# wrapping round.
rotate() {
	local off n i out
	off=$1
	shift
	n=$#
	[ "$n" -gt 0 ] || return 0
	off=$((off % n))
	out=""
	i=0
	while [ $i -lt $n ]; do
		eval "out=\"\$out \${$(( (off + i) % n + 1 ))}\""
		i=$((i + 1))
	done
	echo $out
}

# pass_arms GROUP K: pass K's arms in order.  run2: pass k starts at arm
# (k - 1) x ceil(n / passes) + 1 of the n arms still planned; the others
# start one arm further down per pass.
pass_arms() {
	local g=$1 k=$2 list n np step
	if [ "$k" -eq 1 ]; then
		list=$(arms0 "$g")
	else
		list=$(plan_arms "$g")
	fi
	set -- $list
	n=$#
	if [ "$g" = run2 ]; then
		np=$(plan_passes "$g")
		step=$(( (n + np - 1) / np ))
		rotate $(( (k - 1) * step )) $list
	else
		rotate $((k - 1)) $list
	fi
}

# ---------------------------------------------------------------- checks --

srcv_want() { /usr/sbin/modinfo -F srcversion "$Z/module/zfs.ko" 2>/dev/null ||
	modinfo -F srcversion "$Z/module/zfs.ko"; }

pools() { timeout 60 zpool list -H -o name 2>/dev/null; }

# foreign_pools: imported pools whose name is not one of this task's.
foreign_pools() {
	local p out=""
	for p in $(pools); do
		case " $POOLS_OK " in
		*" $p "*) ;;
		*) out="$out $p" ;;
		esac
	done
	echo $out
}

kmsg() { echo "$1" > "$KMSG" 2>/dev/null; }

# nnullb: the null_blk devices that exist.
nnullb() {
	local d n=0
	for d in "$NB"/nullb*; do
		[ -e "$d" ] && n=$((n + 1))
	done
	echo $n
}

# hung_files OUT: the watchdog files run.sh's pool_op left in OUT.
hung_files() {
	local f out=""
	for f in "$1"/hung-*.txt; do
		[ -e "$f" ] && out="$out ${f##*/}"
	done
	echo $out
}

# kernel_bad MARK: lines of the kernel log after MARK that are a stop.
kernel_bad() {
	dmesg 2>/dev/null | sed -n "/$1/,\$p" | grep -E \
	    'VERIFY|PANIC|BUG:|Oops|Call Trace|hung_task|blocked for more than'
}

state() {
	local bad=0 got want x
	if ! lsmod | grep -q '^zfs '; then
		log "state: no zfs module loaded"
		bad=1
	else
		got=$(cat "$SRCV")
		want=$(srcv_want)
		[ "$got" = "$want" ] || { log "state: srcversion $got," \
		    "the tree built $want"; bad=1; }
	fi
	x=$(pools)
	[ -z "$x" ] || { log "state: pools imported: $(echo $x)"; bad=1; }
	x=$(zinject 2>&1 | head -n 1)
	[ "$x" = "No handlers registered." ] || { log "state: zinject says" \
	    "'$x'"; bad=1; }
	x=$(nnullb)
	[ "$x" -eq 0 ] || { log "state: $x null_blk devices"; bad=1; }
	x=$(dmsetup ls 2>/dev/null | grep -c '^slow')
	[ "$x" -eq 0 ] || { log "state: $x dm devices slow*"; bad=1; }
	if [ -s "$ZCACHE" ]; then
		log "state: $ZCACHE is not empty"
		bad=1
	fi
	x=$(pgrep -c -f -- '--name=sw-slow' 2>/dev/null)
	[ "${x:-0}" -eq 0 ] || { log "state: $x slow fio jobs run"; bad=1; }
	if [ $bad -eq 0 ]; then
		log "STATE OK: srcversion $(cat "$SRCV")," \
		    "no pool, no handler, no device"
		return 0
	fi
	stop "state check failed"
}

# ------------------------------------------------------------------ load --

load() {
	local want got p v
	[ -x "$Z/scripts/zfs.sh" ] || stop "no tree $Z"
	command -v zpool | grep -qx "$Z/zpool" || stop "zpool is not $Z/zpool"
	lsmod | grep -q '^zfs ' && stop "a zfs module is already loaded"
	mkdir -p "$L" "$T" || stop "mkdir"
	[ -f "$L/sysctl.saved" ] || {
		echo "kernel.kptr_restrict=$(sysctl -n kernel.kptr_restrict)"
		echo "kernel.perf_event_paranoid=$(sysctl -n \
		    kernel.perf_event_paranoid)"
	} > "$L/sysctl.saved"
	"$Z/scripts/zfs.sh" || stop "zfs.sh failed"
	want=$(srcv_want)
	got=$(cat "$SRCV")
	[ -n "$want" ] && [ "$want" = "$got" ] ||
	    stop "srcversion $got loaded, the tree built $want"
	for p in zio_taskq_bulk_lowpri zio_taskq_bulk_yield \
	    zio_taskq_write_pct zio_taskq_write_light spa_sync_light_pri \
	    zfs_arc_evict_lane zio_taskq_write_budget; do
		[ -r "$P/$p" ] || stop "parameter $p is missing"
	done
	v=$(cat "$P/zio_taskq_write_budget")
	[ "$v" = 0 ] || stop "zio_taskq_write_budget is $v, not 0"
	case $TASK in
	x3)
		for p in zio_taskq_bulk_pri_pct zio_taskq_write_waited; do
			[ -e "$P/$p" ] && stop "parameter $p exists in X3's tree"
		done
		;;
	x4)
		[ "$(cat "$P/zio_taskq_bulk_pri_pct" 2>/dev/null)" = 100 ] ||
		    stop "zio_taskq_bulk_pri_pct is not 100"
		[ "$(cat "$P/zio_taskq_write_waited" 2>/dev/null)" = 0 ] ||
		    stop "zio_taskq_write_waited is not 0"
		;;
	esac
	v=$(awk 'NR > 2 && NF == 3 { n++; if ($3 != 0) nz++ }
	    END { print n + 0, nz + 0 }' "$KS/zio_write_budget" 2>/dev/null)
	[ "$v" = "5 0" ] || stop "zio_write_budget kstat: '$v' (counters," \
	    "non-zero), expected '5 0'"
	plan_init
	log "load OK: $Z, srcversion $got, zio_taskq_write" \
	    "'$(cat "$P/zio_taskq_write")', budget 0$( [ "$TASK" = x4 ] &&
	    echo ", bulk_pri_pct 100, write_waited 0")"
}

# ----------------------------------------------------------------- quiet --

# quiet1: one 60 s measure after load1 fell below 1.0 (at most 5 min);
# prints the steal share.
quiet1() {
	local i a b l0 l1 line
	i=0
	while [ $i -lt "${XP_LOAD_WAIT:-30}" ]; do
		awk '{ exit !($1 < 1.0) }' /proc/loadavg && break
		sleep 10
		i=$((i + 1))
	done
	l0=$(cut -d' ' -f1 /proc/loadavg)
	a=$(head -1 /proc/stat)
	sleep "${XP_QUIET_SECS:-60}"
	b=$(head -1 /proc/stat)
	l1=$(cut -d' ' -f1 /proc/loadavg)
	line=$(echo "$a" "$b" | awk '{
	    n = (NF - 2) / 2
	    for (i = 1; i <= 8; i++) { d[i] = $(i + n + 2) - $(i + 1); t += d[i] }
	    if (t == 0) t = 1
	    printf "steal %.2f idle %.2f iowait %.2f", \
	        100 * d[8] / t, 100 * d[4] / t, 100 * d[5] / t }')
	echo "$(date -u +%H:%M:%S) quiet ${XP_QUIET_SECS:-60} s: $line" \
	    "load1 $l0 -> $l1 (waited $((i * 10)) s for load1 < 1.0)" \
	    >> "$L/quiet-check.txt"
	log "quiet: $line load1 $l0 -> $l1"
	echo "$line" | awk '{ print $2 }'
}

quiet() {
	local s
	s=$(quiet1 | tail -1)
	awk -v s="$s" 'BEGIN { exit !(s > 0.5) }' || { log "QUIET OK"; return 0; }
	log "steal $s % > 0.5 %: waiting 5 minutes, then once more"
	sleep "${XP_QUIET_WAIT:-300}"
	s=$(quiet1 | tail -1)
	awk -v s="$s" 'BEGIN { exit !(s > 0.5) }' || { log "QUIET OK"; return 0; }
	log "STOP: steal $s % > 0.5 % twice: not measuring"
	exit 4
}

# ---------------------------------------------------------------- probe --

probe() {
	echo "$1 $(date -u +%Y-%m-%dT%H:%M:%SZ) $(python3 -I "$H/cpuprobe.py")" \
	    >> "$L/probe.txt"
}

# ------------------------------------------------------------- teardown --

teardown() {
	local p i d bad=0
	pkill -INT -f -- '--name=sw-slow[123]' 2>/dev/null
	pkill -INT -x fiofast 2>/dev/null
	sleep 2
	pkill -KILL -f -- '--name=sw-slow[123]' 2>/dev/null
	pkill -KILL -x fiofast 2>/dev/null
	for p in $(pools); do
		case " $POOLS_OK " in
		*" $p "*)
			timeout 600 zpool destroy -f "$p" ||
			    { log "destroy $p failed"; bad=1; }
			;;
		*)
			log "pool $p is not this task's: left as it is"
			bad=1
			;;
		esac
	done
	for p in 1 2 3; do
		for i in 0 1 2 3 4 5; do
			if dmsetup info "slow$p.$i" > /dev/null 2>&1; then
				dmsetup remove "slow$p.$i" ||
				    { log "dmsetup remove slow$p.$i failed"; bad=1; }
			fi
		done
	done
	for i in $(seq 0 21); do
		d=$NB/nullb$i
		[ -d "$d" ] || continue
		echo 0 > "$d/power"
		rmdir "$d" || { log "rmdir $d failed"; bad=1; }
	done
	if lsmod | grep -q '^null_blk '; then
		modprobe -r null_blk || { log "modprobe -r null_blk failed"; bad=1; }
	fi
	if lsmod | grep -q '^dm_delay '; then
		modprobe -r dm_delay || { log "modprobe -r dm_delay failed"; bad=1; }
	fi
	[ $bad -eq 0 ] || stop "teardown incomplete"
	log "teardown done"
}

# ------------------------------------------------------------------ pass --

# hang_capture TAG: what the old tasks asked for when a zpool command hung.
hang_capture() {
	{
		echo "=== $(date -u) $1"
		cat "$KS/zio_write_budget" 2>&1
		for p in $POOLS_OK; do
			[ -r "$KS/$p/iostats" ] && { echo "--- $p"; \
			    cat "$KS/$p/iostats"; }
		done
		ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|s_|zpool|fio'
		echo w > "$SYSRQ"
		sleep 2
		dmesg | tail -300
	} > "$L/hang-$1.txt" 2>&1
}

memwatch() {
	local ma
	while kill -0 "$1" 2>/dev/null; do
		ma=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' "$MEMINFO")
		echo "$(date -u +%H:%M:%S) $ma" >> "$L/memwatch.log"
		if [ "${ma:-0}" -lt 500 ]; then
			log "MemAvailable $ma MB < 500 MB: stopping the pass"
			touch "$T/memstop"
			kill -TERM "$1"
			return
		fi
		sleep 5
	done
}

pass() {
	local g=$1 k=$2 rerun=${3:-} np arms a specs out work tag t0 t1 rc pid
	local mw nrow bad x wls
	arms0 "$g" > /dev/null || stop "unknown group $g"
	case $k in
	''|*[!0-9]*) stop "pass number $k" ;;
	esac
	[ -f "$T/plan.$g" ] || stop "no plan: run load first"
	np=$(plan_passes "$g")
	if [ "$k" -gt "$np" ]; then
		log "pass $g $k not run: the time rule left $np passes"
		return 0
	fi
	arms=$(pass_arms "$g" "$k")
	specs=""
	for a in $arms; do
		specs="$specs $(spec "$a")" || stop "unknown arm $a"
	done
	tag=$g-p$k
	out=$L/raw/$g/p$k
	work=$T/work/$tag
	if [ -e "$out" ]; then
		[ "$rerun" = rerun ] || stop "$out exists (use rerun)"
		[ -e "$out.failed" ] && stop "$g $k was already run again once"
		mv "$out" "$out.failed" || stop "mv $out"
		log "pass $g $k run again; the first run kept as $out.failed"
	fi
	x=$(pools)
	[ -z "$x" ] || stop "pools imported before pass $g $k: $(echo $x)"
	[ "$(nnullb)" -eq 0 ] ||
	    stop "null_blk devices exist before pass $g $k"
	mkdir -p "$out" "$work" || stop "mkdir"
	echo "$arms" > "$out/order.txt"
	probe "$tag"
	kmsg "xp-$TASK: pass $tag start"
	t0=$(date +%s)
	log "pass $g $k start epoch=$t0 arms: $arms"
	rm -f "$T/memstop"
	# shellcheck disable=SC2046
	env $(genv "$g") QV_WL="$(gwl "$g")" QV_PHASES="${specs# }" \
	    OUT="$out" WORK="$work" \
	    timeout --kill-after=60 "$(glimit "$g")" bash "$H/run.sh" qv \
	    > "$out.console" 2>&1 &
	pid=$!
	memwatch "$pid" &
	mw=$!
	wait "$pid"
	rc=$?
	kill "$mw" 2>/dev/null
	wait "$mw" 2>/dev/null
	t1=$(date +%s)
	x=$(hung_files "$out")
	if [ -n "$x" ]; then
		hang_capture "$tag"
		stop "pass $g $k: a zpool command hung ($x); see" \
		    "hang-$tag.txt; pools left as they are"
	fi
	x=$(kernel_bad "xp-$TASK: pass $tag start")
	if [ -n "$x" ]; then
		dmesg | sed -n "/xp-$TASK: pass $tag start/,\$p" > \
		    "$L/dmesg-$tag.txt"
		stop "pass $g $k: kernel log: $(echo "$x" | head -3 | tr '\n' ' ')"
	fi
	x=$(foreign_pools)
	[ -z "$x" ] || stop "pass $g $k: a pool this task did not create is" \
	    "imported: $x (left as it is)"
	[ -f "$T/memstop" ] && { teardown; stop "pass $g $k: low memory"; }
	wls=$(gwl "$g")
	nrow=0
	bad=""
	for a in $arms; do
		for x in $wls; do
			nrow=$((nrow + 1))
			d=$out/runs/qv.$a.$x.r1
			if [ ! -f "$d/fio.json" ] ||
			    [ "$(cat "$d/rc" 2>/dev/null)" != 0 ] ||
			    [ -e "$d/slow-load" ]; then
				bad="$bad $a/$x"
			fi
		done
	done
	teardown
	log "pass $g $k done epoch=$t1 secs=$((t1 - t0)) run.sh rc=$rc" \
	    "rows=$nrow failed=${bad:- none}"
	[ $rc -eq 0 ] || [ -n "$bad" ] || bad=" (run.sh rc $rc)"
	if [ -n "$bad" ]; then
		log "FAILED ROWS in pass $g $k:$bad"
		return 2
	fi
	return 0
}

# ------------------------------------------------------------- time rule --

# pass_secs GROUP K: the duration of a pass from driver.log.
pass_secs() {
	awk -v g="$1" -v k="$2" '$2 == "pass" && $3 == g && $4 == k &&
	    $5 == "done" { for (i = 6; i <= NF; i++) if ($i ~ /^secs=/) {
	    sub(/^secs=/, "", $i); s = $i } } END { print s }' \
	    "$L/driver.log"
}

timerule() {
	local go now d1 n1 per arms np sp tp step left dl dropped out
	[ -f "$L/go" ] || stop "no GO time ($L/go)"
	go=$(cat "$L/go")
	now=$(date +%s)
	d1=$(pass_secs run2 1)
	[ -n "$d1" ] || stop "run2 pass 1 has not ended"
	n1=$(wc -w < "$L/raw/run2/p1/order.txt")
	# Fixed cost of a pass (probe, setup, teardown) 90 s; Single and Tput
	# passes 150 s and 180 s (TASK-1116-X2's timings).
	per=$(( (d1 - 90) / n1 ))
	[ "$per" -gt 0 ] || per=0
	arms=$(plan_arms run2)
	np=$(plan_passes run2)
	if [ "$TASK" = x3 ]; then
		sp=$(plan_passes single)
		tp=$(plan_passes tput)
		set -- m3w-up2 m3w-c1-pct25w side4 run2p3 side3
	else
		sp=0
		tp=0
		set -- m3w-w2x1 m3w-lowfast m3w-w2b run2p3
	fi
	dl=$((go + ${XP_LIMIT_MIN:-88} * 60))
	dropped=""
	while :; do
		left=$(( (np - 1) * (90 + $(echo $arms | wc -w) * per) +
		    sp * 150 + tp * 180 ))
		[ $((now + left)) -gt "$dl" ] || break
		if [ $# -gt 0 ]; then
			step=$1
			shift
			case $step in
			side4) sp=3; tp=3 ;;
			side3) sp=2; tp=2 ;;
			run2p3) np=2 ;;
			*) arms=$(echo " $arms " | sed "s/ $step / /; s/^ //; s/ \$//") ;;
			esac
			dropped="$dropped $step"
			continue
		fi
		log "time rule: still past GO + ${XP_LIMIT_MIN:-88} min after" \
		    "every drop; running the rest, the overrun to be reported"
		break
	done
	printf 'passes %s\narms %s\n' "$np" "$arms" > "$T/plan.run2"
	if [ "$TASK" = x3 ]; then
		printf 'passes %s\narms %s\n' "$sp" "$(arms0 single)" \
		    > "$T/plan.single"
		printf 'passes %s\narms %s\n' "$tp" "$(arms0 tput)" \
		    > "$T/plan.tput"
	fi
	out="GO $(date -u -d "@$go" +%H:%M:%S) now $(date -u -d "@$now" +%H:%M:%S)"
	out="$out run2 p1 ${d1} s for $n1 arms (${per} s per arm)"
	out="$out projected end $(date -u -d "@$((now + left))" +%H:%M:%S)"
	out="$out limit $(date -u -d "@$dl" +%H:%M:%S) (GO + ${XP_LIMIT_MIN:-88} min)"
	out="$out dropped:${dropped:- nothing}"
	echo "$out" > "$L/time-rule.txt"
	log "time rule: $out"
	plan_show
}

# fits GROUP: true when a pass of GROUP, as long as its last one (or
# 150 s, Single, and 180 s, Tput, before any), ends by GO + XP_HARD_MIN
# (90) minutes.
fits() {
	local d
	d=$(awk -v g="$1" '$2 == "pass" && $3 == g && $5 == "done" {
	    for (i = 6; i <= NF; i++) if ($i ~ /^secs=/) {
	    sub(/^secs=/, "", $i); s = $i } } END { print s }' "$L/driver.log")
	if [ -z "$d" ]; then
		case $1 in
		tput) d=180 ;;
		*) d=150 ;;
		esac
	fi
	[ $(( $(date +%s) + d )) -le \
	    $(( $(cat "$L/go") + ${XP_HARD_MIN:-90} * 60 )) ]
}

# rest: every pass after run2's first, group by group as planned; a pass
# with a failed row (run2's first included) runs once more at the end of
# its group, in the same order; a second failure is a stop.  No pass
# starts that would end after GO + 90 minutes.
rest() {
	local g k n failed rc skipped=""
	[ -f "$L/go" ] || stop "no GO time ($L/go)"
	for g in $(groups); do
		n=$(plan_passes "$g")
		k=1
		[ "$g" = run2 ] && k=2
		failed=""
		if [ "$g" = run2 ] && [ ! -e "$L/raw/run2/p1.failed" ] &&
		    grep -q 'FAILED ROWS in pass run2 1:' "$L/driver.log"; then
			failed=1
		fi
		while [ "$k" -le "$n" ]; do
			if ! fits "$g"; then
				log "pass $g $k not run: it would end after GO +" \
				    "${XP_HARD_MIN:-90} min"
				skipped="$skipped $g-p$k"
				k=$((k + 1))
				continue
			fi
			pass "$g" "$k"
			rc=$?
			case $rc in
			0) ;;
			2) failed="$failed $k" ;;
			*) exit "$rc" ;;
			esac
			k=$((k + 1))
		done
		for k in $failed; do
			if ! fits "$g"; then
				log "pass $g $k not run again: it would end after" \
				    "GO + ${XP_HARD_MIN:-90} min; its failed rows" \
				    "stay out of the tables"
				skipped="$skipped $g-p$k-rerun"
				continue
			fi
			pass "$g" "$k" rerun
			[ $? -eq 0 ] || stop "pass $g $k failed twice"
		done
	done
	probe final
	if [ -n "$skipped" ]; then
		log "REST DONE: not run for the 90-minute limit:$skipped"
	else
		log "REST DONE: every planned pass ran"
	fi
}

plan_show() {
	local g
	for g in $(groups); do
		echo "plan $g: $(plan_passes "$g") passes; arms after pass 1:" \
		    "$(plan_arms "$g")"
	done
}

end() {
	local n
	teardown
	n=$(awk '$1 == "npools" { print $3 }' "$KS/zio_write_budget" 2>/dev/null)
	log "npools at the end: ${n:-unreadable}"
	if lsmod | grep -q '^zfs '; then
		"$Z/scripts/zfs.sh" -u || stop "zfs.sh -u failed"
	fi
	rm -f "$FIOFAST"
	[ -f "$L/sysctl.saved" ] && sysctl -q -p "$L/sysctl.saved"
	log "end: modules unloaded, $FIOFAST removed, sysctls restored" \
	    "($(tr '\n' ' ' < "$L/sysctl.saved" 2>/dev/null))"
}

mkdir -p "$L" "$T" 2>/dev/null
cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
load) load ;;
state) state ;;
go) date +%s > "$L/go"; log "GO received" ;;
quiet) quiet ;;
pass) [ $# -ge 2 ] || stop "usage: pass GROUP K [rerun]"
	pass "$@" ;;
timerule) timerule ;;
rest) rest ;;
plan) plan_show ;;
teardown) teardown ;;
end) end ;;
*) echo "usage: xp-drive.sh x3|x4 load|state|go|quiet|pass G K [rerun]|" \
    "timerule|rest|plan|teardown|end" >&2; exit 1 ;;
esac
