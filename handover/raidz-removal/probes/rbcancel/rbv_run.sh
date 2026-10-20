#!/bin/bash
# usage: rbv_run.sh <bin> <tag> <nproc> <timeout_s> <probe args...>
# Runs <nproc> probes in parallel, each in its own /dev/shm dir, and
# removes only the files the probe creates there.
S=$SCRATCH
BIN=$1 TAG=$2 N=$3 TMO=$4
shift 4
OUT=$S/rbtools/runs
FILES="a-1 a-2 new-1 new-2 file-1 file-2 file-3 file-4 spare-1 zpool.cache zpool.cache.tmp"
dirs=()
clean() {
	for d in "${dirs[@]}"; do
		for f in $FILES; do rm -f "${d:?}/$f"; done
		rmdir "${d:?}" 2>/dev/null || ls -la "$d"
	done
}
pids=()
for i in $(seq 1 "$N"); do
	d=$(mktemp -d /dev/shm/rbv.XXXXXX) || exit 1
	dirs+=("$d")
	( cd "$S/rbtools/runs" && timeout -s KILL "$TMO" "$BIN" "$d" "$@" > "$OUT/$TAG.$i.log" 2>&1; echo "exit=$?" >> "$OUT/$TAG.$i.log" ) &
	pids+=($!)
done
wait "${pids[@]}"
clean
grep -h "RESULT\|VERIFY\|HANG\|CANCEL\|exit=\|iter " "$OUT/$TAG".*.log | sort | uniq -c | sort -rn | head -20
