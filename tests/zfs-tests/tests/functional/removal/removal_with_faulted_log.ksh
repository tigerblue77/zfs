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

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	A faulted log device can be removed while the ZIL of an unmounted
#	file system still has a block on it.  The removal frees that log
#	and must not read the faulted device for it: with failmode=wait
#	the read suspends the pool, and "zpool clear" cannot resume it
#	while "zpool remove" holds spa_namespace_lock.
#
# STRATEGY:
#	1. Create a pool with a log device, with failmode=wait.
#	2. Write synchronously to a file system and unmount it, so that
#	   its ZIL keeps a block on the log device.
#	3. Fault the log device with "zpool offline -f".
#	4. Remove it.  The command must return within 60 seconds.  If it
#	   has not, nothing can release it short of a reboot: fail, and
#	   leave the pool alone.
#	5. Verify that the device is gone, that the pool is healthy and
#	   leaks no space, and that the file system can log again.
#

verify_runnable "global"

DISKDIR=$TEST_BASE_DIR/removal_with_faulted_log
DISK=$DISKDIR/pooldisk
LOGDISK=$DISKDIR/logdisk
typeset remove_pid=""

function cleanup
{
	if [[ -n $remove_pid ]] && kill -0 $remove_pid 2>/dev/null; then
		log_note "zpool remove (pid $remove_pid) is still blocked;" \
		    "only a reboot releases $TESTPOOL"
		return
	fi
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $DISKDIR
}

log_onexit cleanup

log_assert "A faulted log device holding a ZIL block can be removed"

log_must rm -rf $DISKDIR
log_must mkdir -p $DISKDIR
log_must truncate -s $MINVDEVSIZE $DISK $LOGDISK
log_must zpool create -o failmode=wait $TESTPOOL $DISK log $LOGDISK
log_must zfs create $TESTPOOL/$TESTFS
typeset mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)

log_must dd if=/dev/urandom of=$mntpnt/file bs=128k count=8 oflag=sync
log_must zfs unmount $TESTPOOL/$TESTFS
sync_pool $TESTPOOL
typeset -i alloc=$(get_vdev_prop allocated $TESTPOOL $LOGDISK)
log_note "$alloc bytes allocated on the log device"
(( alloc > 0 )) || log_fail "no ZIL block left on the log device"

log_must zpool offline -f $TESTPOOL $LOGDISK
log_must test "$(get_device_state $TESTPOOL $LOGDISK logs)" = "FAULTED"

# Keep its output off ours: a blocked child holding it would stall the run.
zpool remove $TESTPOOL $LOGDISK >$DISKDIR/remove.out 2>&1 </dev/null &
remove_pid=$!
typeset -i n=0
while kill -0 $remove_pid 2>/dev/null && (( n < 60 )); do
	sleep 1
	(( n += 1 ))
done
if kill -0 $remove_pid 2>/dev/null; then
	log_fail "zpool remove of the faulted log device still running" \
	    "after $n seconds"
fi
wait $remove_pid
typeset -i rc=$?
log_note "zpool remove returned $rc within $n seconds:" \
    "$(<$DISKDIR/remove.out)"
(( rc == 0 )) || log_fail "zpool remove of the faulted log device failed"

log_mustnot vdevs_in_pool $TESTPOOL $LOGDISK
log_must test "$(get_pool_prop health $TESTPOOL)" = "ONLINE"

sync_pool $TESTPOOL
typeset zdbout
zdbout=$(zdb -b $TESTPOOL 2>&1) || log_fail "zdb -b failed: $zdbout"
# zdb -b doesn't fail on a leak, so check for one.
[[ $zdbout == *"No leaks"* ]] || log_fail "zdb -b: $zdbout"

log_must zfs mount $TESTPOOL/$TESTFS
log_must dd if=/dev/urandom of=$mntpnt/file2 bs=128k count=8 oflag=sync
sync_pool $TESTPOOL

log_pass "A faulted log device holding a ZIL block can be removed"
