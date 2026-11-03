#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# Run anyraid_adopt_map over the configurations of the adoption spike and
# print one summary line per run.  Usage: run_map_matrix.sh <bin> <logdir>
#
BIN=${1:?binary}
LOG=${2:?logdir}
mkdir -p "$LOG" || exit 1

run() {
	name=$1
	shift
	"$BIN" "$@" -v > "$LOG/$name.log" 2>&1
	rc=$?
	printf '%-34s rc=%d  %s\n' "$name" "$rc" \
	    "$(grep -E '^IDENTITY|^blocks' "$LOG/$name.log" | tr '\n' ' ')"
}

# Exhaustive: every sector offset, every psize, 3 tiles of 64 sectors per
# child, tile per child equal to the metaslab size.
for a in 9 12; do
	t=$((a + 6))
	for wp in 3:1 4:1 5:1 4:2 5:2 6:2 8:2 6:3 7:3 10:3; do
		w=${wp%:*}
		p=${wp#*:}
		run "exh-a$a-w$w-p$p" -w "$w" -p "$p" -a "$a" -t "$t" -m "$t" \
		    -n 3 -e
	done
	run "exh-a$a-w5-p2-writes" -w 5 -p 2 -a "$a" -t "$t" -m "$t" -n 3 -e -W
	run "exh-a$a-w6-p2-writes" -w 6 -p 2 -a "$a" -t "$t" -m "$t" -n 3 -e -W
	# Tile per child half the metaslab: aligned for an even width only.
	run "exh-a$a-w6-p2-half" -w 6 -p 2 -a "$a" -t "$t" -m $((t + 1)) \
	    -n 3 -e
	run "exh-a$a-w5-p2-half" -w 5 -p 2 -a "$a" -t "$t" -m $((t + 1)) \
	    -n 3 -e
	# Taller tiles: 256 rows per child, multi-row blocks of every size.
	run "exh-a$a-w5-p2-tall" -w 5 -p 2 -a "$a" -t $((a + 8)) \
	    -m $((a + 8)) -n 3 -e
	run "exh-a$a-w6-p2-tall" -w 6 -p 2 -a "$a" -t $((a + 8)) \
	    -m $((a + 8)) -n 3 -e
	# Control: standard anyraid slot base, must fail everywhere.
	run "exh-a$a-w5-p2-control" -w 5 -p 2 -a "$a" -t "$t" -m "$t" \
	    -n 3 -e -S
done

# Real shapes: 16 GiB tiles per child, 16 GiB metaslabs, 22 TB children
# (1280 slots) for 3+2, 6 TB children (349 slots) for 4+2.
for a in 9 12; do
	run "bnd-a$a-w5-p2" -w 5 -p 2 -a "$a" -t 34 -m 34 -n 1280 -b 48
	run "bnd-a$a-w6-p2" -w 6 -p 2 -a "$a" -t 34 -m 34 -n 349 -b 48
	run "rnd-a$a-w5-p2" -w 5 -p 2 -a "$a" -t 34 -m 34 -n 1280 \
	    -r 2000000 -s 5
	run "rnd-a$a-w6-p2" -w 6 -p 2 -a "$a" -t 34 -m 34 -n 349 \
	    -r 2000000 -s 6
	run "rnd-a$a-w5-p1" -w 5 -p 1 -a "$a" -t 34 -m 34 -n 1280 \
	    -r 500000 -s 7
	run "rnd-a$a-w6-p2-writes" -w 6 -p 2 -a "$a" -t 34 -m 34 -n 349 \
	    -r 200000 -x 512 -s 8 -W
done
