#!/bin/bash
# usage: run_zdb.sh <build-tree> <scratch-root> <log>
# Runs "zdb -e -p <dir> -bcc <pool>" (leak and checksum check) and "zdb -e -C"
# on every pool the probe left exported.  The binary in <tree>/.libs is run
# with the libraries of the same tree (the libtool wrapper of build-base points
# at build-src, which was cloned from it).
TREE=$1
ROOT=$2
LOG=$3
ulimit -c 0
export LD_LIBRARY_PATH="$TREE/.libs"
ZDB="$TREE/.libs/zdb"
: > "$LOG"
while read -r dir pool; do
	echo "### $pool ($dir)" >> "$LOG"
	out=$(timeout 600 "$ZDB" -e -p "$dir" -bcc "$pool" 2>&1)
	rc=$?
	echo "$out" | grep -i "leak\|error\|assert\|fail\|panic\|No leaks" | head -8 >> "$LOG"
	echo "zdb -bcc exit status: $rc" >> "$LOG"
	out=$(timeout 600 "$ZDB" -e -p "$dir" -C "$pool" 2>&1)
	echo "zdb -C exit status: $?, holes: $(echo "$out" | grep -c "type: 'hole'")" >> "$LOG"
done < "$ROOT/manifest.txt"
