#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# Kernel A/B of the LZ4 compressor: master (A) against the LZ4 1.10
# branch (B), release builds, one pool on a RAM disk, compression=lz4.
#
#   vm-lz4-ab.sh <treeA> <treeB> <resultdir> [rounds]
#
# Rounds alternate the order (A B, then B A, ...). In each round and for
# each build: for each data set (mixed, random) and recordsize (128K,
# 16K), a fresh pool is created, the data set is written and synced, the
# pool is exported and imported (empties the ARC of it), and the file is
# read back. One CSV line per run goes to <resultdir>/runs.csv, medians
# to <resultdir>/summary.txt.
#
# Environment: RAM_MB (RAM disk, 4096), ARC_MB (zfs_arc_max, 1024),
# MIXED_FILE and RANDOM_FILE (data sets, made by the task doc, step 3).
# Disposable VM only: replaces the ZFS module stack and destroys pool lzab.

set -u
A=$1
B=$2
R=$3
ROUNDS=${4:-6}
RAM_MB=${RAM_MB:-4096}
ARC_MB=${ARC_MB:-1024}
MIXED=${MIXED_FILE:-/dev/shm/lzab-mixed}
RANDOM_DATA=${RANDOM_FILE:-/dev/shm/lzab-random}
POOL=lzab
DEV=/dev/ram0
mkdir -p "$R"
LOG=$R/run.log
CSV=$R/runs.csv

log() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$LOG"; }
now() { date +%s.%N; }
busy() { awk '/^cpu / {print $2 + $3 + $4 + $7 + $8}' /proc/stat; }
hz() { getconf CLK_TCK; }

for f in "$MIXED" "$RANDOM_DATA"; do
	[ -s "$f" ] || { log "SETUP missing data set $f"; exit 1; }
done
for t in "$A" "$B"; do
	if ! [ -x "$t/scripts/zfs.sh" ] || ! [ -x "$t/zpool" ]; then
		log "SETUP not a built tree: $t"
		exit 1
	fi
done

if ! [ -b $DEV ]; then
	modprobe brd rd_nr=1 rd_size=$((RAM_MB * 1024)) ||
	    { log "SETUP modprobe brd failed"; exit 1; }
fi

load() {
	local t=$1 ko
	"$t/scripts/zfs.sh" -u > /dev/null 2>&1
	if ! timeout 300 "$t/scripts/zfs.sh" > /dev/null 2>&1; then
		log "SETUP load of $t failed"
		exit 1
	fi
	ko=$(find "$t/module" -name zfs.ko | head -1)
	if [ "$(cat /sys/module/zfs/srcversion)" != \
	    "$(modinfo -F srcversion "$ko")" ]; then
		log "SETUP loaded zfs.ko is not the one of $t"
		exit 1
	fi
	echo $((ARC_MB * 1024 * 1024)) > /sys/module/zfs/parameters/zfs_arc_max
}

# run_one <side> <tree> <round> <dataname> <datafile> <recordsize>
run_one() {
	local side=$1 t=$2 r=$3 dn=$4 df=$5 rs=$6
	local zp="$t/zpool" zf="$t/zfs" t0 t1 c0 c1 ws wc rds rdc lu us cr
	"$zp" destroy -f $POOL > /dev/null 2>&1
	if ! "$zp" create -f -o ashift=12 -O compression=lz4 \
	    -O recordsize="$rs" -O atime=off -O mountpoint=/$POOL \
	    $POOL $DEV > /dev/null 2>> "$LOG"; then
		log "SETUP zpool create failed ($side $dn $rs)"
		exit 1
	fi
	c0=$(busy); t0=$(now)
	dd if="$df" of=/$POOL/f bs=1M status=none
	"$zp" sync $POOL
	t1=$(now); c1=$(busy)
	ws=$(echo "$t1 - $t0" | bc -l)
	wc=$(echo "($c1 - $c0) / $(hz)" | bc -l)
	lu=$("$zf" get -Hpo value logicalused $POOL)
	us=$("$zf" get -Hpo value used $POOL)
	cr=$("$zf" get -Hpo value compressratio $POOL)
	"$zp" export $POOL
	"$zp" import -d $DEV $POOL || { log "SETUP import failed"; exit 1; }
	c0=$(busy); t0=$(now)
	dd if=/$POOL/f of=/dev/null bs=1M status=none
	t1=$(now); c1=$(busy)
	rds=$(echo "$t1 - $t0" | bc -l)
	rdc=$(echo "($c1 - $c0) / $(hz)" | bc -l)
	if ! cmp -s "$df" /$POOL/f; then
		log "RESULT data mismatch ($side round $r $dn $rs)"
	fi
	"$zp" destroy -f $POOL
	printf '%s,%s,%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%s,%s,%s\n' "$r" "$side" \
	    "$dn" "$rs" "$(stat -c %s "$df")" "$ws" "$wc" "$rds" "$rdc" \
	    "$lu" "$us" "$cr" >> "$CSV"
}

echo "round,side,data,recordsize,bytes,write_s,write_cpu_s,read_s,read_cpu_s,logicalused,used,compressratio" > "$CSV"
log "A=$A ($(git -c safe.directory='*' -C "$A" log -1 --format=%h)) B=$B ($(git -c safe.directory='*' -C "$B" log -1 --format=%h)) rounds=$ROUNDS nproc=$(nproc) kernel=$(uname -r)"
for r in $(seq 1 "$ROUNDS"); do
	if [ $((r % 2)) = 1 ]; then order="A B"; else order="B A"; fi
	for side in $order; do
		if [ "$side" = A ]; then t=$A; else t=$B; fi
		load "$t"
		log "round $r side $side loaded, loadavg $(cut -d' ' -f1-3 /proc/loadavg)"
		for dn in mixed random; do
			if [ $dn = mixed ]; then df=$MIXED; else df=$RANDOM_DATA; fi
			for rs in 128K 16K; do
				run_one "$side" "$t" "$r" $dn "$df" $rs
			done
		done
	done
done
"$B/scripts/zfs.sh" -u > /dev/null 2>&1

# Medians per side, data set and recordsize; B/A ratios.
awk -F, 'NR > 1 {
	k = $3 "," $4; s = $2
	n[s, k]++
	w[s, k, n[s, k]] = $5 / 1048576 / $6
	rd[s, k, n[s, k]] = $5 / 1048576 / $8
	wcpu[s, k, n[s, k]] = $7 / ($5 / 1073741824)
	rcpu[s, k, n[s, k]] = $9 / ($5 / 1073741824)
	cr[s, k] = $12; used[s, k] = $11; keys[k] = 1
}
function med(arr, s, k, cnt,   i, j, t, v) {
	for (i = 1; i <= cnt; i++) v[i] = arr[s, k, i]
	for (i = 1; i <= cnt; i++) for (j = i + 1; j <= cnt; j++)
		if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
	return (cnt % 2) ? v[(cnt + 1) / 2] : (v[cnt / 2] + v[cnt / 2 + 1]) / 2
}
function mn(arr, s, k, cnt,   i, m) {
	m = arr[s, k, 1]; for (i = 2; i <= cnt; i++) if (arr[s, k, i] < m) m = arr[s, k, i]
	return m
}
function mx(arr, s, k, cnt,   i, m) {
	m = arr[s, k, 1]; for (i = 2; i <= cnt; i++) if (arr[s, k, i] > m) m = arr[s, k, i]
	return m
}
END {
	printf "%-14s %-4s %22s %22s %10s %10s %8s %12s\n", "data,rs", "side", \
	    "write MiB/s med [min-max]", "read MiB/s med [min-max]", \
	    "wCPU s/GiB", "rCPU s/GiB", "ratio", "used"
	for (k in keys) for (si = 1; si <= 2; si++) {
		s = (si == 1) ? "A" : "B"; c = n[s, k]
		printf "%-14s %-4s %8.0f [%5.0f-%5.0f] %8.0f [%5.0f-%5.0f] %10.2f %10.2f %8s %12s\n", \
		    k, s, med(w, s, k, c), mn(w, s, k, c), mx(w, s, k, c), \
		    med(rd, s, k, c), mn(rd, s, k, c), mx(rd, s, k, c), \
		    med(wcpu, s, k, c), med(rcpu, s, k, c), cr[s, k], used[s, k]
		if (s == "B")
			printf "%-14s B/A  write %.3f read %.3f wCPU %.3f rCPU %.3f\n", k, \
			    med(w, "B", k, c) / med(w, "A", k, n["A", k]), \
			    med(rd, "B", k, c) / med(rd, "A", k, n["A", k]), \
			    med(wcpu, "B", k, c) / med(wcpu, "A", k, n["A", k]), \
			    med(rcpu, "B", k, c) / med(rcpu, "A", k, n["A", k])
	}
}' "$CSV" | tee "$R/summary.txt"
log "done"
