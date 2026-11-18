#!/bin/sh
# k35-size.sh start <prefix> | stop <prefix>
#
# In a tracing instance of its own ("k35"; no other instance is touched),
# records every wake-up of a sleeping kswapd of order > 0 with its kernel
# stack, and every vmap area larger than two pages, guard page included
# (vmalloc:alloc_vmap_area), whose size gives the size of a vmalloc()
# request.  k35-reduce.py reads the instance's trace_pipe during the run
# and writes <prefix>.k35: one line per wake-up, with the size of the last
# vmap area the same task reserved before it, and the vmap areas counted
# by size.  Tracepoints only: no kprobe, nothing tied to the loaded
# modules, so a module reload between runs does not matter.
# stop: stops the reader, saves the per-CPU buffer stats in
# <prefix>.k35.stats and removes the instance.
set -eu
T=${K35_TRACEFS:-/sys/kernel/tracing}
[ -d "$T/events" ] || T=/sys/kernel/debug/tracing
I=$T/instances/k35
D=$(cd "$(dirname "$0")" && pwd)
W=$I/events/vmscan/mm_vmscan_wakeup_kswapd
V=$I/events/vmalloc/alloc_vmap_area
TRIG='stacktrace if order > 0'

case "${1:-}" in
start)
	P=$2
	[ -d "$I" ] || mkdir "$I"
	echo 0 > "$I/tracing_on"
	echo 8192 > "$I/buffer_size_kb"
	echo > "$I/trace"
	echo 'order > 0' > "$W/filter"
	echo "$TRIG" > "$W/trigger"
	echo 'size > 8192' > "$V/filter"
	echo 1 > "$W/enable"
	echo 1 > "$V/enable"
	nohup python3 -I "$D/k35-reduce.py" "$I/trace_pipe" "$P.k35" \
	    > /dev/null 2> "$P.k35.err" &
	echo $! > "$P.k35.pid"
	sleep 1
	echo 1 > "$I/tracing_on"
	;;
stop)
	P=$2
	echo 0 > "$I/tracing_on"
	sleep 2
	R=$(cat "$P.k35.pid")
	kill -TERM "$R" 2> /dev/null || true
	n=0
	while kill -0 "$R" 2> /dev/null && [ $n -lt 30 ]; do
		sleep 1
		n=$((n + 1))
	done
	if kill -0 "$R" 2> /dev/null; then
		echo "k35-size.sh: reader $R did not stop" >&2
		kill -KILL "$R" 2> /dev/null || true
	fi
	for c in "$I"/per_cpu/cpu*; do
		echo "$(basename "$c") $(tr '\n' ' ' < "$c/stats")"
	done > "$P.k35.stats"
	echo 0 > "$W/enable"
	echo 0 > "$V/enable"
	echo "!$TRIG" > "$W/trigger"
	echo 0 > "$W/filter"
	echo 0 > "$V/filter"
	rmdir "$I"
	rm -f "$P.k35.pid"
	;;
*)
	echo "usage: $0 start|stop <prefix>" >&2
	exit 1
	;;
esac
