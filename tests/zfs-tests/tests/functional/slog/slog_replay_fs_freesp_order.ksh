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
#	Verify that replay applies a truncation or a hole punch before a
#	write that was made after it had changed the file.
#
# STRATEGY:
#	1. Create a pool with a slog and freeze it.
#	2. Truncate the file, or punch a hole in it, and hold that
#	   operation at the zfs_freesp:changed point, after it has changed
#	   the file and before it is logged.
#	3. Meanwhile, write into the range it freed and fsync.  Give the
#	   write five seconds, then release the held operation.
#	4. Export and import to replay the log.  The file must hold the
#	   write.
#

verify_runnable "global"
verify_inject_points

function cleanup_freesp
{
	zinject -c all
	cleanup
}

log_assert "Replay applies a truncation or a punch before a later write."
log_onexit cleanup_freesp
log_must setup

log_must zpool create $TESTPOOL $VDEV log mirror $LDEV
log_must zfs create -o compression=off -o recordsize=128k \
    $TESTPOOL/$TESTFS
log_must mkdir -p $TESTDIR
log_must dd if=/dev/urandom of=$TESTDIR/old bs=128k count=4
log_must dd if=/dev/urandom of=$TESTDIR/new bs=128k count=1

for op in truncate punch; do
	file=/$TESTPOOL/$TESTFS/$op
	expect=$TESTDIR/expect.$op
	log_must cp $TESTDIR/old $file
	log_must cp $TESTDIR/old $expect

	#
	# truncate: cut the file to 128K, then write 128K at 128K.
	# punch: free [128K, 384K), then write 128K at 256K.
	#
	if [[ $op == truncate ]]; then
		set -A change truncate -s 128K $file
		seek=1
		log_must truncate -s 128K $expect
	else
		set -A change fallocate -p -o 128K -l 256K $file
		seek=2
		log_must dd if=/dev/zero of=$expect bs=128k seek=1 count=2 \
		    conv=notrunc
	fi
	log_must dd if=$TESTDIR/new of=$expect bs=128k seek=$seek count=1 \
	    conv=notrunc

	# Make sure the dataset has a ZIL header before freezing.
	log_must dd if=/dev/zero of=/$TESTPOOL/$TESTFS/sync \
	    conv=fdatasync,fsync bs=1 count=1
	log_must zpool freeze $TESTPOOL

	id=$(zinject -q -n zfs_freesp:changed -A pause $TESTPOOL)
	[[ -n $id ]] || log_fail "zinject -n failed"
	"${change[@]}" &
	changepid=$!
	wait_inject_point $id 1 60 || \
	    log_fail "no thread reached zfs_freesp:changed in 60 seconds"

	dd if=$TESTDIR/new of=$file bs=128k seek=$seek count=1 \
	    conv=notrunc,fsync status=none &
	writepid=$!
	# The write is logged at once, unless the held operation blocks it.
	typeset -i i=0
	while kill -0 $writepid 2>/dev/null && ((i++ < 50)); do
		sleep 0.1
	done
	kill -0 $writepid 2>/dev/null && log_note "$op: the write waited"

	log_must zinject -c $id
	log_must wait $changepid
	log_must wait $writepid
	# Log the change too, if it was logged after the write.
	log_must dd if=/dev/null of=$file bs=1 count=0 conv=notrunc,fsync

	log_must cmp $expect $file
	log_must zfs unmount $TESTPOOL/$TESTFS
	log_must zpool export $TESTPOOL
	log_must zpool import -f -d $VDIR $TESTPOOL
	log_must cmp $expect $file
done

log_pass "Replay applies a truncation or a punch before a later write."
