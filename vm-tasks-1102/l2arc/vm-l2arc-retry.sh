#!/bin/bash
#
# Disposable VM only.  Run as root.  Reloads the zfs module stack, creates
# and destroys the pool "l2rt" on files under /var/tmp/l2arc-retry.
#
#   vm-l2arc-retry.sh TREE RESDIR LABEL bounce|corrupt|both
#
# Both cases: pool on a file, ashift=9, lz4, recordsize=128K; a 1 GiB file
# as cache device; zfs_compressed_arc_enabled=0; a 96 MiB file of compressible
# text, so most records are compressed to a PSIZE that is a multiple of 512
# but not of 4K.  Once the L2ARC holds it, the file system is unmounted, the
# ARC flushed (zinject -a) and the file read again, from the L2ARC.
#   bounce:  the cache device has ashift=12, so those reads use a bounce
#	     buffer of the allocation size (l2arc_read_done() copy).
#   corrupt: the cache device has ashift=9 (no bounce buffer) and its data
#	     region is overwritten with random bytes before the read, so the
#	     L2ARC copies fail their checksum and are read again from the pool.
#   both:    ashift=12 and the overwrite: bounce buffer, then the re-read.
#
# Exit 0: done, module unloaded.  Exit 2: SETUP problem, proves nothing.
# Exit 3: the read did not finish within 120 s; evidence collected, the
# module is left loaded: REBOOT before anything else.

set -u
T=${1:?}; R=${2:?}; LABEL=${3:?}; CASE=${4:?}
Z=/var/tmp/l2arc-retry
POOL=l2rt
MNT=$Z/alt/$POOL
P=/sys/module/zfs/parameters
mkdir -p "$Z" "$R"

log() { echo "$(date +%T) $*" | tee -a "$R/run.log"; }
ks() { awk -v n="$1" '$1 == n { print $3 }' /proc/spl/kstat/zfs/arcstats; }
setup_fail() { log "SETUP $LABEL: $*"; exit 2; }

case "$CASE" in
bounce) CASHIFT=12; CORRUPT=0 ;;
corrupt) CASHIFT=9; CORRUPT=1 ;;
both) CASHIFT=12; CORRUPT=1 ;;
*) echo "usage: $0 TREE RESDIR LABEL bounce|corrupt|both"; exit 2 ;;
esac

timeout 300 "$T/scripts/zfs.sh" -r > "$R/$LABEL-load.log" 2>&1 ||
    setup_fail "module load failed"
ko=$(find "$T/module" -name zfs.ko | head -1)
if [ -z "$ko" ] || [ "$(cat /sys/module/zfs/srcversion)" != \
    "$(modinfo -F srcversion "$ko")" ]; then
	setup_fail "loaded module is not $T"
fi
dmesg -C
echo 1 > /proc/sys/kernel/sysrq
echo 0 > $P/zfs_compressed_arc_enabled
echo 0 > $P/l2arc_noprefetch
echo $((64 << 20)) > $P/l2arc_write_max
[ -w $P/l2arc_dwpd_limit ] && echo 0 > $P/l2arc_dwpd_limit
log "$LABEL: $CASE, $(cat /sys/module/zfs/version) from $T," \
    "compressed_arc=$(cat $P/zfs_compressed_arc_enabled)"

rm -f "$Z/pool.img" "$Z/cache.img"
truncate -s 2G "$Z/pool.img"; truncate -s 1G "$Z/cache.img"
timeout 120 "$T/zpool" create -f -o ashift=9 -o cachefile="$Z/zpool.cache" \
    -R "$Z/alt" -O compression=lz4 \
    -O recordsize=128K -O mountpoint=/$POOL $POOL "$Z/pool.img" \
    > "$R/$LABEL-create.out" 2>&1 || setup_fail "zpool create failed"
timeout 120 "$T/zpool" add -o ashift=$CASHIFT $POOL cache "$Z/cache.img" \
    >> "$R/$LABEL-create.out" 2>&1 || setup_fail "cache add failed"
timeout 60 "$T/zdb" -l "$Z/cache.img" > "$R/$LABEL-cache-label.txt" 2>&1
if ! grep -q "ashift: $CASHIFT\$" "$R/$LABEL-cache-label.txt" &&
    [ $CASHIFT = 12 ]; then
	# Fallback: a loop device with 4K sectors gets ashift=12 by itself.
	log "$LABEL: -o ashift=12 not applied to the cache file," \
	    "using a loop device"
	timeout 120 "$T/zpool" remove $POOL "$Z/cache.img"
	loop=$(losetup -b 4096 -f --show "$Z/cache.img") || setup_fail "losetup"
	timeout 120 "$T/zpool" add $POOL cache "$loop" ||
	    setup_fail "cache add $loop failed"
	timeout 60 "$T/zdb" -l "$loop" > "$R/$LABEL-cache-label.txt" 2>&1
fi
grep -q "ashift: $CASHIFT\$" "$R/$LABEL-cache-label.txt" ||
    setup_fail "cache device ashift is not $CASHIFT"

seq 1 20000000 | head -c 96M > "$MNT/data"
want=$(seq 1 20000000 | head -c 96M | sha256sum | cut -c1-64)
timeout 120 "$T/zpool" sync $POOL
cat "$MNT/data" > /dev/null
# How many of the file's records have a PSIZE that is not a multiple of 4K.
obj=$(stat -c %i "$MNT/data")
timeout 300 "$T/zdb" -U "$Z/zpool.cache" -ddddd $POOL "$obj" \
    > "$R/$LABEL-zdb-data.txt" 2>&1
awk '/ L0 / { if (match($0, / [0-9a-f]+L\/[0-9a-f]+P /)) {
	split(substr($0, RSTART + 1, RLENGTH - 3), a, "L/"); n++
	h = a[2]; if (length(h) < 4 || substr(h, length(h) - 2) != "000") u++ } }
    END { printf "records=%d psize_not_4k=%d\n", n, u }' \
    "$R/$LABEL-zdb-data.txt" > "$R/$LABEL-psize.txt"
log "$LABEL: $(cat "$R/$LABEL-psize.txt")"

# Wait until the L2ARC stops growing (at most 180 s).
last=-1; same=0
for _ in $(seq 180); do
	now=$(ks l2_asize)
	if [ "$now" = "$last" ] && [ "$now" -gt $((16 << 20)) ]; then
		same=$((same + 1)); [ $same -ge 5 ] && break
	else
		same=0
	fi
	last=$now; sleep 1
done
log "$LABEL: l2_size=$(ks l2_size) l2_asize=$(ks l2_asize)"
[ "$(ks l2_asize)" -gt $((16 << 20)) ] || setup_fail "L2ARC not filled"

if [ $CORRUPT = 1 ]; then
	# The data region runs from the L2ARC header's start to its end
	# (4 MiB of labels and boot area, then one block of device header).
	# Log blocks sit among the data and are only read at a rebuild, which
	# this run does not do.  Leave 1 MiB at each end.
	timeout 60 "$T/zdb" -l "$Z/cache.img" > "$R/$LABEL-cache-hdr.txt" 2>&1
	start=$(awk '$1 == "start:" { print $2 }' "$R/$LABEL-cache-hdr.txt")
	end=$(awk '$1 == "end:" { print $2 }' "$R/$LABEL-cache-hdr.txt")
	if [ -z "$start" ] || [ -z "$end" ]; then
		setup_fail "no L2ARC header"
	fi
	first=$(( (start >> 20) + 1 )); count=$(( (end >> 20) - first - 1 ))
	[ $count -gt 64 ] && count=64
	dd if=/dev/urandom of="$Z/cache.img" bs=1M seek=$first count=$count \
	    conv=notrunc,fsync status=none
	log "$LABEL: overwrote MiB $first..$((first + count)) of the cache" \
	    "device (header start=$start end=$end)"
fi

timeout 120 "$T/zfs" umount $POOL || setup_fail "umount failed"
timeout 120 "$T/zinject" -a > /dev/null 2>&1 || setup_fail "zinject -a failed"
timeout 120 "$T/zfs" mount $POOL || setup_fail "mount failed"
h0=$(ks l2_hits); c0=$(ks l2_cksum_bad); e0=$(ks l2_io_error)

sha256sum "$MNT/data" > "$R/$LABEL-read.out" 2> "$R/$LABEL-read.err" &
rpid=$!
for _ in $(seq 120); do kill -0 $rpid 2>/dev/null || break; sleep 1; done
if kill -0 $rpid 2>/dev/null; then
	log "RESULT $LABEL case=$CASE read=HUNG" \
	    "l2_hits=+$(($(ks l2_hits) - h0))" \
	    "l2_cksum_bad=+$(($(ks l2_cksum_bad) - c0))"
	ps -eLo pid,tid,stat,pcpu,comm | awk '/z_rd_int|sha256sum/' \
	    > "$R/$LABEL-threads.txt"
	tids=$(ps -eLo tid,comm | awk '/z_rd_int|sha256sum/ { print $1 }')
	for t in $tids; do
		echo "== $t"; cat "/proc/$t/stack" 2>/dev/null
	done > "$R/$LABEL-stacks.txt"
	echo l > /proc/sysrq-trigger; echo w > /proc/sysrq-trigger; sleep 2
	dmesg > "$R/$LABEL-dmesg.txt"
	log "$LABEL: $(grep -cE 'VERIFY|PANIC|soft lockup' "$R/$LABEL-dmesg.txt")" \
	    "VERIFY/PANIC/soft lockup lines in dmesg. REBOOT NOW."
	exit 3
fi
wait $rpid; rrc=$?
[ $(($(ks l2_hits) - h0)) -gt 0 ] ||
    log "WARN $LABEL: no L2ARC hit, the run proves nothing"
got=$(cut -c1-64 "$R/$LABEL-read.out")
ok=no; [ "$got" = "$want" ] && ok=yes
timeout 60 "$T/zpool" status -v $POOL > "$R/$LABEL-status.out" 2>&1
dmesg > "$R/$LABEL-dmesg.txt"
log "RESULT $LABEL case=$CASE read_rc=$rrc data_ok=$ok" \
    "err=[$(head -c 80 "$R/$LABEL-read.err")]" \
    "l2_hits=+$(($(ks l2_hits) - h0))" \
    "l2_cksum_bad=+$(($(ks l2_cksum_bad) - c0))" \
    "l2_io_error=+$(($(ks l2_io_error) - e0))" \
    "data_errors=$(grep -c 'Permanent errors' "$R/$LABEL-status.out")" \
    "dmesg_verify=$(grep -cE 'VERIFY|PANIC' "$R/$LABEL-dmesg.txt")"
timeout 120 "$T/zpool" destroy -f $POOL || log "$LABEL: destroy failed"
timeout 300 "$T/scripts/zfs.sh" -u > "$R/$LABEL-unload.log" 2>&1 ||
    log "$LABEL: unload failed, reboot before the next run"
exit 0
