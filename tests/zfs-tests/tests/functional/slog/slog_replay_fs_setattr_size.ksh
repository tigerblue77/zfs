#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/tests/functional/slog/slog.kshlib

#
# DESCRIPTION:
#	Verify that a TX_SETATTR record that changes the size is replayed
#	again in full if the system stops after its size change reached
#	disk and before its other attributes did.
#
# STRATEGY:
#	1. Import a pool image whose log holds such a record.
#	2. Mount the file system to replay the log, and hold the replay at
#	   the zfs_setattr:size_changed point, once the size has changed.
#	3. Meanwhile, sync and freeze the pool, so that the size change
#	   reaches disk and nothing after it does, then release the replay.
#	   The file must have the size, mode and mtime of the record.
#	4. Export and import to replay the log again.  The file must still
#	   have them.
#
# Linux has logged a size change as TX_TRUNCATE since #19706, so the
# image was written by OpenZFS 2.4.4 on Linux, on a 64M file vdev:
#
#	zpool create -R $ALT testpool_setattr $VDEV
#	zfs create testpool_setattr/fs
#	cd $ALT/testpool_setattr/fs
#	(write 128K to file, then chmod 4777 file)
#	dd if=/dev/zero of=sync bs=1 count=1 conv=fsync
#	zpool sync testpool_setattr
#	zpool freeze testpool_setattr
#	sudo -u nobody truncate -s 4096 file
#	dd if=/dev/null of=file bs=1 count=0 conv=notrunc,fsync
#	zpool export testpool_setattr
#
# The first dd writes the log header before the freeze.  A truncation by
# a user without CAP_FSETID clears the setuid bit, so the record carries
# ATTR_MODE besides ATTR_SIZE and ATTR_MTIME: size 4096, mode 777 and the
# time of the truncation.
#

verify_runnable "global"

if ! is_linux; then
	log_unsupported "zfs_setattr:size_changed is a point of Linux code"
fi

POOL=testpool_setattr
FS=$POOL/fs
IMAGE=setattr_size.dat
BLOCKFILES=$STF_SUITE/tests/functional/slog/blockfiles

function cleanup_setattr_size
{
	zinject -c all
	poolexists $POOL && destroy_pool $POOL
	rm -rf $VDIR
}

#
# Fail unless the file has the size and mode of the record, and the mtime
# $2, or if $2 is empty, an mtime set before the test started.  Set mtime.
#
function check_record # when mtime
{
	typeset file size mode

	file=$(get_prop mountpoint $FS)/file
	size=$(stat -c %s $file)
	mode=$(stat -c %a $file)
	mtime=$(stat -c %y $file)
	log_note "$1: size $size, mode $mode, mtime $mtime"
	[[ $size == 4096 && $mode == 777 ]] || \
	    log_fail "$1: size $size and mode $mode, not 4096 and 777"
	if [[ -n $2 ]]; then
		[[ $mtime == "$2" ]] || log_fail "$1: mtime $mtime, not $2"
	else
		(( $(stat -c %Y $file) < start )) || \
		    log_fail "$1: mtime $mtime, set during the test"
	fi
}

log_assert "Replay applies a TX_SETATTR record that changes the size" \
    "again after a stop between its size and its other attributes."
log_onexit cleanup_setattr_size

start=$(date +%s)
log_must rm -rf $VDIR
log_must mkdir -p $VDIR
log_must eval "bzcat $BLOCKFILES/$IMAGE.bz2 > $VDIR/$IMAGE"

#
# zdb prints the record as "TX_SETATTR len 184, txg 7, seq 3", then
# "foid 2, mask 0x2069".  The mask holds the Linux ATTR_* flags: 0x1 is
# ATTR_MODE and 0x8 is ATTR_SIZE.
#
zil=$(zdb -e -p $VDIR -ivv $FS) || log_fail "zdb cannot read the image"
log_note "$(echo "$zil" | grep -A1 'TX_SETATTR.*seq')"
(( $(echo "$zil" | grep -c 'TX_SETATTR.*seq') == 1 )) || \
    log_fail "the log of the image holds no single TX_SETATTR record"
mask=$(echo "$zil" | awk '/TX_SETATTR.*seq/ { getline; print $NF }')
(( (mask & 0x9) == 0x9 )) || \
    log_fail "the record has mask $mask, not ATTR_SIZE and ATTR_MODE"

log_must zpool import -o cachefile=none -f -N -d $VDIR $POOL
id=$(zinject -q -w zfs_setattr:size_changed -s 60 $POOL)
[[ -n $id ]] || log_fail "zinject -w failed"
zfs mount $FS &
mountpid=$!
wait_inject_point $id

# The size has changed: write it to disk, and nothing that follows it.
log_must zpool sync $POOL
log_must zpool freeze $POOL
log_must zinject -c $id
log_must wait $mountpid
check_record "After the first replay"
rec_mtime=$mtime

log_must zpool export $POOL
# Without the fix, replay_seq on disk is already the seq of the record.
log_note "On disk: $(zdb -e -p $VDIR -i $FS 2>&1 | grep 'ZIL header')"
log_must zpool import -o cachefile=none -f -d $VDIR $POOL
check_record "After the second replay" "$rec_mtime"

log_pass "Replay applies a TX_SETATTR record that changes the size" \
    "again after a stop between its size and its other attributes."
