#!/bin/bash
# usage: rbwait_run.sh <bin> <tag> <nproc> <iters> <modes> [hang_secs]
# Runs <nproc> probe processes in parallel, each in its own /dev/shm dir,
# and removes only the files it created.
S=$SCRATCH
BIN=$1 TAG=$2 N=$3 IT=$4 MODES=$5 HS=${6:-60}
OUT=$S/pr/evidence/rbwait_runs
pids=()
cleanup() { for d in "${dirs[@]}"; do rm -f "${d:?}"/file-1 "${d:?}"/file-2 "${d:?}"/file-3 "${d:?}"/file-4 "${d:?}"/spare-1 "${d:?}"/zpool.cache "${d:?}"/zpool.cache.tmp; rmdir "${d:?}" 2>/dev/null; done; }
trap 'pkill -f "bin_rbwait.* /dev/shm/rbw"; sleep 2; cleanup; exit 1' INT TERM
dirs=()
for i in $(seq 1 "$N"); do
	d=$(mktemp -d /dev/shm/rbw.XXXXXX) || exit 1
	dirs+=("$d")
	( timeout 3000 "$BIN" "$d" "$IT" "$MODES" "$HS" > "$OUT/$TAG.$i.log" 2>&1; echo "exit=$?" >> "$OUT/$TAG.$i.log" ) &
	pids+=($!)
done
wait "${pids[@]}"
for d in "${dirs[@]}"; do
	rm -f "${d:?}"/file-1 "${d:?}"/file-2 "${d:?}"/file-3 "${d:?}"/file-4 "${d:?}"/spare-1 "${d:?}"/zpool.cache "${d:?}"/zpool.cache.tmp
	rmdir "${d:?}" 2>/dev/null || ls -la "$d"
done
grep -h "RESULT\|HANG\|exit=" "$OUT/$TAG".*.log
