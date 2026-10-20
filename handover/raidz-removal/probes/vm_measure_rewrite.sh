#!/usr/bin/env bash
#
# vm_measure_rewrite.sh - measure how far "allocating=off" followed by
# "zfs rewrite" drains a raidz top-level vdev, and what is left on it, on a
# DISPOSABLE VM with file vdevs only.
#
# Uses the in-tree build in /var/tmp/zts-pr1/patched (openzfs/zfs#19373
# rework, which can remove an empty raidz vdev as a hole).  Run as root.
# Refuses to run if the zfs module is loaded or /var/tmp/zts-measure exists.
# Writes only /var/tmp/zts-measure (files and mountpoints, deleted at the end)
# and /var/log/zts-measure (logs, REPORT.txt).  The pool is "measpool"; it is
# destroyed and the modules are unloaded at the end, also on failure.
#
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C

TREE=/var/tmp/zts-pr1/patched
ZPOOL=$TREE/zpool
ZFS=$TREE/zfs
ZDB=$TREE/zdb
POOL=measpool
DIR=/var/tmp/zts-measure
MNT=$DIR/mnt
LOG=/var/log/zts-measure
REPORT=$LOG/REPORT.txt
LOADED=0

die() { echo "ERROR: $*" >&2; exit 1; }
say() { echo "[$(date +%T)] $*" | tee -a "$LOG/run.log"; }

cleanup() {
	set +e
	if [ "$LOADED" = 1 ]; then
		"$ZPOOL" list "$POOL" >/dev/null 2>&1 &&
		    "$ZPOOL" destroy -f "$POOL" >>"$LOG/run.log" 2>&1
		"$TREE/scripts/zfs.sh" -u >>"$LOG/run.log" 2>&1
	fi
	rm -rf "$DIR"
}

[ "$(id -u)" = 0 ] || die "run as root"
for b in "$ZPOOL" "$ZFS" "$ZDB" "$TREE/scripts/zfs.sh"; do
	[ -x "$b" ] || die "missing $b"
done
[ ! -d /sys/module/zfs ] || die "zfs module already loaded"
[ ! -e "$DIR" ] || die "$DIR exists; remove it first"
install -d "$LOG"
: >"$LOG/run.log"
: >"$REPORT"
install -d "$DIR" "$MNT"
trap cleanup EXIT

"$TREE/scripts/zfs.sh" >>"$LOG/run.log" 2>&1 || die "module load failed"
LOADED=1

# Census of the blocks that zdb finds with a DVA on top-level vdev 0.
census() { # tag allocated
	awk -v tag="$1" -v alloc="$2" -v names="$LOG/zdbd-$1.txt" '
	function hex(s,    i, v) {
		v = 0
		for (i = 1; i <= length(s); i++)
			v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
		return v
	}
	BEGIN {
		while ((getline l < names) > 0) {
			if (l !~ /^Dataset .*, ID [0-9]+,/)
				continue
			nm = l; sub(/^Dataset /, "", nm); sub(/ \[.*/, "", nm)
			id = l; sub(/.*, ID /, "", id); sub(/,.*/, "", id)
			ds[id] = nm
		}
		ds[0] = "MOS"
	}
	/^objset [0-9]+ object [0-9]+ level/ {
		s = $0; on0 = 0; asz = 0
		while (match(s, /DVA\[[0-9]\]=<[0-9]+:[0-9a-f]+:[0-9a-f]+>/)) {
			d = substr(s, RSTART, RLENGTH)
			s = substr(s, RSTART + RLENGTH)
			split(d, p, /[<:>]/)
			if (p[2] == "0") { on0 = 1; asz += hex(p[4]) }
		}
		T++
		if (!on0)
			next
		t = $0; sub(/.*\[L[0-9]+ /, "", t); sub(/\].*/, "", t)
		o = ($2 in ds) ? ds[$2] : "objset " $2
		k = o " | " t; n[k]++; b[k] += asz; N++; B += asz
	}
	END {
		printf "== %s: raidz1-0 allocated %s bytes; zdb: %d of %d " \
		    "blocks (%d bytes) on vdev 0\n", tag, alloc, N, T, B
		for (k in n)
			printf "%9d blocks %13d bytes  %s\n", n[k], b[k], k
	}' "$LOG/zdbb-$1.txt" | { IFS= read -r h; echo "$h"; sort -k1,1nr | awk 'NR <= 30'; }
}

measure() { # tag
	local tag=$1 alloc
	"$ZPOOL" sync "$POOL"
	sleep 2
	"$ZPOOL" sync "$POOL"
	alloc=$("$ZPOOL" get -Hpo value allocated "$POOL" raidz1-0)
	"$ZPOOL" list -v -p "$POOL" >"$LOG/list-$tag.txt" 2>&1
	"$ZPOOL" export "$POOL"
	"$ZDB" -e -p "$DIR" -d "$POOL" >"$LOG/zdbd-$tag.txt" 2>&1 || true
	"$ZDB" -e -p "$DIR" -bbbbb "$POOL" >"$LOG/zdbb-$tag.txt" 2>&1 || true
	"$ZPOOL" import -d "$DIR" "$POOL"
	census "$tag" "$alloc" | tee -a "$REPORT"
	echo >>"$REPORT"
}

fill() { # directory count
	local i
	mkdir -p "$1"
	for ((i = 0; i < $2; i++)); do
		head -c $(((RANDOM % 61 + 4) * 1024)) /dev/urandom >"$1/s$i"
	done
}

say "creating the pool"
for f in a0 a1 a2 b0 b1 b2; do
	truncate -s 2G "$DIR/$f"
done
"$ZPOOL" create -o ashift=12 -O compression=off -O recordsize=128K \
    -O mountpoint="$MNT" "$POOL" raidz1 "$DIR"/a0 "$DIR"/a1 "$DIR"/a2 \
    raidz1 "$DIR"/b0 "$DIR"/b1 "$DIR"/b2

say "writing data, snapshots and a clone"
"$ZFS" create "$POOL/fs"
FS=$MNT/fs
for k in 0 1 2 3; do
	fill "$FS/dir$k" 500
done
for ((i = 0; i < 40; i++)); do
	head -c 16M /dev/urandom >"$FS/big$i"
done
"$ZPOOL" sync "$POOL"
"$ZFS" snapshot "$POOL/fs@s1"
for ((i = 0; i < 40; i += 2)); do
	dd if=/dev/urandom of="$FS/big$i" bs=128K count=32 \
	    seek=$((RANDOM % 96)) conv=notrunc status=none
done
for ((i = 0; i < 300; i++)); do
	rm -f "$FS/dir0/s$i"
done
fill "$FS/dir4" 500
"$ZFS" snapshot "$POOL/fs@s2"
"$ZFS" clone "$POOL/fs@s1" "$POOL/clone"
fill "$MNT/clone/new" 50
measure 1-initial

say "allocating=off on raidz1-0, then zfs rewrite of every file"
"$ZPOOL" set allocating=off "$POOL" raidz1-0
"$ZFS" rewrite -r "$FS" "$MNT/clone" >>"$LOG/run.log" 2>&1 ||
    say "zfs rewrite exited with $?"
measure 2-after-rewrite

say "destroying the clone and the snapshots"
"$ZFS" destroy "$POOL/clone"
"$ZFS" destroy "$POOL/fs@s2"
"$ZFS" destroy "$POOL/fs@s1"
measure 3-no-snapshots

say "30 txgs of small changes"
for ((i = 0; i < 30; i++)); do
	echo "$i" >"$FS/churn"
	"$ZPOOL" sync "$POOL"
done
measure 4-after-churn

say "trying to remove raidz1-0"
{
	echo "== zpool remove $POOL raidz1-0"
	if "$ZPOOL" remove "$POOL" raidz1-0 2>&1; then
		echo "exit 0"
	else
		echo "exit $?"
	fi
	"$ZPOOL" status "$POOL" 2>&1 | sed -n '/config:/,$p'
} | tee -a "$REPORT"
say "done"
