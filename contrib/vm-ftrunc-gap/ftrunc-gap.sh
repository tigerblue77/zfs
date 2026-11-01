#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# Does a write that lands between a size change and its log record replay
# in the wrong order?  Needs a kernel built with ftrunc-delay.patch (two
# experiment-only module parameters that sleep inside the window) and its
# module loaded.  Run as root on a disposable VM.  Own pool on a file vdev
# under $DIR, altroot and own cache file; zdb reads only that cache file.
#
# usage: ftrunc-gap.sh <case> <delay_ms> [rounds]
#   setattr-direct    truncate -s (BS+4096) vs O_DIRECT write of block 1
#   setattr-buffered  same, buffered write
#   punch-buffered    fallocate -p -o BS -l BS (keep size) vs buffered
#                     write of block 1 on a 2-block file
# delay_ms 0 is the control (no window widening).
#
# Per round: live and replayed size and sha256 of block 1, whether the
# write returned before the competing op, and the order the frozen log
# holds the write (W) and the size change (S, TX_SETATTR) or punch
# (T, TX_TRUNCATE).  Exit 2: setup error, nothing counted.

set -u
CASE=${1:?case}
DELAY=${2:?delay_ms}
ROUNDS=${3:-10}
POOL=rvgap
DIR=/var/tmp/rvgap
FS=$POOL/fs
DEVDIR=$DIR/dev
ALT=$DIR/alt
CACHE=$DIR/zpool.cache
BS=131072
WOFF=$(printf '%x' $BS)
P=/sys/module/zfs/parameters

die() { echo "SETUP ERROR: $*" >&2; exit 2; }

case $CASE in
setattr-direct|setattr-buffered) KNOB=$P/zfs_setattr_size_delay_ms; OP=S ;;
punch-buffered) KNOB=$P/zfs_freesp_log_delay_ms; OP=T ;;
*) die "unknown case $CASE" ;;
esac
[ -w "$KNOB" ] || die "$KNOB missing: module not built with ftrunc-delay.patch"
[ "$(cat $P/zil_replay_disable)" = 0 ] || die "zil_replay_disable is not 0"
zpool list "$POOL" >/dev/null 2>&1 && die "pool $POOL already exists"
under_dir() { awk -v d="$DIR" '$2 == d || index($2, d "/") == 1' /proc/mounts; }
[ -z "$(under_dir)" ] || die "something is mounted under $DIR"

cleanup()
{
	echo 0 > "$KNOB"
	zpool list "$POOL" >/dev/null 2>&1 && zpool destroy -f "$POOL"
	if zpool list "$POOL" >/dev/null 2>&1 || [ -n "$(under_dir)" ]; then
		echo "cleanup: $POOL or a mount is left, $DIR kept" >&2
		return 1
	fi
	rm -rf "$DIR"
}
trap cleanup EXIT

mkpool()
{
	rm -rf "$DIR"
	mkdir -p "$DEVDIR" || die "mkdir $DEVDIR"
	truncate -s 1G "$DEVDIR/vdev" || die "vdev"
	zpool create -f -R "$ALT" -o cachefile="$CACHE" -O compression=off \
	    -O mountpoint=none -O canmount=off "$POOL" "$DEVDIR/vdev" ||
	    die "zpool create"
	zfs create -o mountpoint="$DIR/mnt" -o recordsize=$BS "$FS" ||
	    die "zfs create"
	MNT=$(zfs get -H -o value mountpoint "$FS")
	[ "$(zfs get -H -o value mounted "$FS")" = yes ] || die "not mounted"
	f=$MNT/racefile
}

zdb_iii()
{
	ZOUT=$(zdb -U "$CACHE" -iii "$FS" 2>"$DIR/zdb.err") ||
	    { cat "$DIR/zdb.err" >&2; die "zdb -iii $1 failed"; }
}

# Order of the first TX_WRITE at offset BS, of the first TX_SETATTR whose
# raw mask has Linux ATTR_SIZE (0x8; zdb decodes lr_mask with the Solaris
# AT_* bits, so its AT_SIZE line cannot be used) and of the first
# TX_TRUNCATE, for object $1.
log_order()
{
	printf '%s\n' "$ZOUT" | awk -v obj="$1" -v off="$WOFF," '
	function bit8(m,  d) {
		d = index("0123456789abcdef", tolower(substr(m, length(m)))) - 1
		return (d >= 8)
	}
	$1 ~ /^(CI-)?TX_/ && $2 == "len" { t = $1; sub(/^CI-/, "", t); next }
	$1 == "foid" {
		o = $2; sub(/,$/, "", o)
		if (o != obj) next
		if ((t == "TX_WRITE" || t == "TX_WRITE2") && $3 == "offset" &&
		    $4 == off && !w) { s = s "W"; w = 1 }
		if (t == "TX_TRUNCATE" && !tr) { s = s "T"; tr = 1 }
		if (t == "TX_SETATTR" && $3 == "mask" && bit8($4) && !a) {
			s = s "S"; a = 1
		}
	}
	END { print s }'
}

blk1() { dd if="$f" bs=$BS skip=1 count=1 status=none | sha256sum | cut -c1-12; }

gap=0; same=0; other=0
for ((i = 1; i <= ROUNDS; i++)); do
	mkpool
	# A ZIL header must be on disk before the freeze (see
	# slog_replay_fs_001.ksh), or the import has nothing to replay.
	dd if=/dev/zero of="$MNT/sync" bs=1 count=1 conv=fsync status=none ||
	    die "header write"
	nblk=1
	[ "$CASE" = punch-buffered ] && nblk=2
	dd if=/dev/urandom of="$f" bs=$BS count=$nblk conv=fsync \
	    status=none || die "seed"
	obj=$(stat -c %i "$f")
	zpool sync "$POOL" || die "zpool sync"
	zdb_iii pre-freeze
	printf '%s\n' "$ZOUT" | grep -q 'ZIL header:' ||
	    die "round $i: no ZIL header before the freeze"
	zpool freeze "$POOL" || die "zpool freeze"

	oflag=
	[ "$CASE" = setattr-direct ] && oflag=oflag=direct
	echo "$DELAY" > "$KNOB"
	if [ "$OP" = S ]; then
		truncate -s $((BS + 4096)) "$f" &
	else
		fallocate -p -o $BS -l $BS "$f" &
	fi
	op=$!
	# Let the op change the size or free the range and reach the delay.
	[ "$DELAY" -gt 0 ] && sleep 0.2
	dd if=/dev/urandom of="$f" bs=$BS count=1 seek=1 $oflag \
	    conv=notrunc status=none || die "round $i: write failed"
	kill -0 $op 2>/dev/null && wfirst=1 || wfirst=0
	wait $op || die "round $i: competing op failed"
	echo 0 > "$KNOB"

	live=$(stat -c %s "$f")
	livesum=$(blk1)
	dd if=/dev/null of="$f" conv=notrunc,fsync status=none ||
	    die "round $i: fsync"
	zfs unmount "$FS" || die "round $i: unmount"
	zdb_iii frozen
	order=$(log_order "$obj")
	case $order in
	"W$OP"|"${OP}W") ;;
	*) die "round $i: log order '$order', expected W$OP or ${OP}W" ;;
	esac
	zpool export "$POOL" || die "round $i: export"
	zpool import -N -f -R "$ALT" -o cachefile="$CACHE" -d "$DEVDIR" \
	    "$POOL" || die "round $i: import"
	zfs mount "$FS" || die "round $i: mount (replay)"
	zpool sync "$POOL" || die "zpool sync"
	zdb_iii replayed
	printf '%s\n' "$ZOUT" | grep -q 'ZIL header:' &&
	    die "round $i: log not replayed (header still present)"
	[ -e "$f" ] || die "round $i: $f missing after the replay"
	after=$(stat -c %s "$f")
	aftersum=$(blk1)

	echo "round $i live $live/$livesum replayed $after/$aftersum" \
	    "log $order write_returned_first $wfirst"
	if [ "$live" = "$after" ] && [ "$livesum" = "$aftersum" ]; then
		same=$((same + 1))
	elif [ "$order" = "W$OP" ] && [ "$wfirst" = 1 ]; then
		gap=$((gap + 1))
	else
		other=$((other + 1))
	fi
	cleanup || die "round $i: cleanup"
done
echo "RESULT case=$CASE delay=$DELAY rounds=$ROUNDS same=$same" \
    "gap=$gap other=$other"
