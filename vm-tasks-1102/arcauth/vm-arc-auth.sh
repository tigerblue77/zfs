#!/bin/bash
#
# Disposable VM only.  Run as root.  Creates and destroys the pool "arcauth"
# on a file under /var/tmp/arc-auth, and reloads the zfs module stack.
#
#   vm-arc-auth.sh write TREE RESDIR
#	Load TREE's modules (the test writer: master + lz4-writer-tail.patch),
#	create the image /var/tmp/arc-auth/img/pool.orig: ashift=9, lz4, an
#	encrypted file system arcauth/enc holding one small file.  Unload.
#
#   vm-arc-auth.sh read TREE RESDIR LABEL CARC MODE
#	Load TREE's modules, set zfs_compressed_arc_enabled=CARC, import a
#	fresh copy of the image read-only, load the key and mount arcauth/enc.
#	MODE "key": nothing before load-key.  MODE "nokeyfirst": first read
#	the ZPL master node without the key (zfs get version), so the mount
#	authenticates a header already cached.  Prints one RESULT line.  Unload.
#
# Every step has a time bound; a SETUP line means the run proves nothing.

set -u
cmd=${1:?}; T=${2:?}; R=${3:?}
Z=/var/tmp/arc-auth
POOL=arcauth
mkdir -p "$Z/img" "$Z/alt" "$R"

log() { echo "$(date +%T) $*" | tee -a "$R/run.log"; }

load() {
	timeout 300 "$T/scripts/zfs.sh" -r > "$R/load-$1.log" 2>&1 ||
	    { log "SETUP $1: module load failed"; exit 2; }
	ko=$(find "$T/module" -name zfs.ko | head -1)
	if [ -z "$ko" ] || [ "$(cat /sys/module/zfs/srcversion)" != \
	    "$(modinfo -F srcversion "$ko")" ]; then
		log "SETUP $1: loaded zfs module is not $T"; exit 2
	fi
	log "$1: loaded $(cat /sys/module/zfs/version) from $T"
}

unload() {
	timeout 300 "$T/scripts/zfs.sh" -u > "$R/unload-$1.log" 2>&1 ||
	    log "unload $1 failed (reboot before the next step)"
}

case "$cmd" in
write)
	load write
	[ -s "$Z/key" ] || head -c 32 /dev/urandom > "$Z/key"
	rm -f "$Z/img/pool.img" "$Z/img/pool.orig"
	truncate -s 512M "$Z/img/pool.img"
	timeout 120 "$T/zpool" create -f -o ashift=9 -R "$Z/alt" \
	    -O compression=lz4 -O mountpoint=none "$POOL" "$Z/img/pool.img" \
	    > "$R/create.out" 2>&1 || { log "SETUP create failed"; exit 2; }
	timeout 120 "$T/zfs" create -o encryption=aes-256-gcm \
	    -o keyformat=raw -o "keylocation=file://$Z/key" \
	    -o mountpoint=/arcauth-enc "$POOL/enc" >> "$R/create.out" 2>&1 ||
	    { log "SETUP zfs create failed"; exit 2; }
	echo hello > "$Z/alt/arcauth-enc/hello"
	timeout 120 "$T/zpool" export "$POOL" ||
	    { log "SETUP export failed"; exit 2; }
	# The ZPL master node (object 1) must be a 1K block stored as 512 B.
	timeout 300 "$T/zdb" -e -p "$Z/img" -K "$Z/key" -ddddd "$POOL/enc" 1 \
	    > "$R/zdb-master-node.txt" 2>&1
	if grep -q 'L0 .*lz4.*400L/200P' "$R/zdb-master-node.txt"; then
		log "write: master node is lz4 400L/200P"
	else
		log "SETUP write: master node not lz4 400L/200P, see zdb-master-node.txt"
	fi
	cp --sparse=always "$Z/img/pool.img" "$Z/img/pool.orig"
	log "write: image $(sha256sum < "$Z/img/pool.orig" | cut -c1-16)"
	unload write
	;;
read)
	LABEL=${4:?}; CARC=${5:?}; MODE=${6:?}
	load "$LABEL"
	echo "$CARC" > /sys/module/zfs/parameters/zfs_compressed_arc_enabled
	log "$LABEL: zfs_compressed_arc_enabled=$(cat \
	    /sys/module/zfs/parameters/zfs_compressed_arc_enabled)"
	cp --sparse=always "$Z/img/pool.orig" "$Z/img/pool.img"
	ro="-o readonly=on"
	[ "${RW:-0}" = 1 ] && ro=""
	# shellcheck disable=SC2086
	timeout 300 "$T/zpool" import -d "$Z/img" $ro -R "$Z/alt" -N "$POOL" \
	    > "$R/$LABEL-import.out" 2>&1 ||
	    { log "SETUP $LABEL: import failed"; unload "$LABEL"; exit 2; }
	if [ "$MODE" = nokeyfirst ]; then
		timeout 120 "$T/zfs" get -H -o value version "$POOL/enc" \
		    > "$R/$LABEL-version-nokey.out" 2>&1
		log "$LABEL: version without key: $(head -1 \
		    "$R/$LABEL-version-nokey.out")"
	fi
	timeout 120 "$T/zfs" load-key "$POOL/enc" > "$R/$LABEL-loadkey.out" 2>&1
	krc=$?
	timeout 120 "$T/zfs" mount "$POOL/enc" > "$R/$LABEL-mount.out" 2>&1
	mrc=$?
	file=$(cat "$Z/alt/arcauth-enc/hello" 2>/dev/null)
	"$T/zpool" status -v "$POOL" > "$R/$LABEL-status.out" 2>&1
	"$T/zpool" events -v > "$R/$LABEL-events.out" 2>&1
	nauth=$(grep -c 'class = "ereport.fs.zfs.authentication"' \
	    "$R/$LABEL-events.out")
	perm=$(grep -c 'Permanent errors' "$R/$LABEL-status.out")
	log "RESULT $LABEL carc=$CARC mode=$MODE loadkey_rc=$krc" \
	    "mount_rc=$mrc file=[$file] auth_ereports=$nauth" \
	    "permanent_errors=$perm mount_msg=[$(head -1 "$R/$LABEL-mount.out")]"
	cp /proc/spl/kstat/zfs/dbgmsg "$R/$LABEL-dbgmsg.txt" 2>/dev/null
	"$T/zfs" umount -a > /dev/null 2>&1
	timeout 120 "$T/zpool" export "$POOL" > /dev/null 2>&1 ||
	    log "$LABEL: export failed"
	unload "$LABEL"
	;;
*)
	echo "usage: $0 write TREE RESDIR | read TREE RESDIR LABEL CARC MODE"
	exit 2
	;;
esac
