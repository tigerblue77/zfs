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

#
# Copyright (c) 2026 by the OpenZFS project.  All rights reserved.
#
. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	A device removal restarted by the load of its pool can complete
#	before the load ends, and wait for its last txg without holding
#	spa_namespace_lock while the load takes the lock back and records
#	spa_minref.  Once the removal is done, the pool must look unused:
#	zpool export must succeed.
#
# STRATEGY:
#	1. Create a pool of two vdevs, write to it, and start the removal
#	   of the second vdev with zfs_removal_suspend_progress set, so
#	   that it stays in progress
#	2. Unload the pool with zinject -u: it stays known, not loaded,
#	   with its removal in progress
#	3. Register zinject -w handlers on it at spa_load_impl:end (the
#	   end of a load, after the removal thread is restarted) and at
#	   spa_vdev_exit:unlocked (the last wait of a vdev change, without
#	   the namespace lock), and let the removal go on
#	4. Load the pool by opening it (zpool status), and wait until both
#	   hold: the load at its end, the completed removal at its last
#	   wait
#	5. Remove the first handler, so the load ends while the removal
#	   waits; then the second one, and wait for the removal to end
#	6. Export and import the pool
#
# NOTES:
#	Without the fix, the removal holds a reference on the pool in its
#	last wait, spa_load() counts it in spa_minref, and every export of
#	the pool fails with EBUSY until the module is reloaded.
#

verify_runnable "global"

log_assert "A removal that completes while its pool is loaded leaves the" \
    "pool exportable"

typeset -i TIMEOUT=60
typeset -i HOLD=600
typeset -i opid=0
typeset ids=""
typeset DEVDIR=$TEST_BASE_DIR/vdev_change_load_wait
typeset VDEV1=$DEVDIR/vdev1
typeset VDEV2=$DEVDIR/vdev2
typeset MNT=$DEVDIR/mnt
typeset LIST=$DEVDIR/zinject.out

#
# Wait up to the given number of seconds for a process to exit.
#
function wait_pid # pid [seconds]
{
	typeset -i tenths=0
	typeset -i secs=${2:-$TIMEOUT}

	while kill -0 $1 2>/dev/null; do
		((tenths++ >= secs * 10)) && return 1
		sleep 0.1
	done
	return 0
}

#
# Wait until a thread is held at the point of a zinject -w handler.
#
function wait_held # id what
{
	typeset -i t0=$SECONDS
	typeset held

	while :; do
		zinject > $LIST 2>&1
		held=$(awk -v id=$1 '$1 == id { print $NF }' $LIST)
		[[ -n $held ]] && ((held > 0)) && break
		((SECONDS - t0 > TIMEOUT)) && \
		    log_fail "$2 not held in ${TIMEOUT}s: $(<$LIST)"
		sleep 0.1
	done
	log_note "$2 held by zinject handler $1"
}

function cleanup
{
	typeset id

	#
	# Removing a handler releases the thread it holds before it needs
	# spa_namespace_lock; do it in the background, as the load holds
	# that lock once released.
	#
	for id in $ids; do
		zinject -c $id > /dev/null 2>&1 &
	done
	wait
	zinject -c all
	((opid > 0)) && wait_pid $opid
	set_tunable32 REMOVAL_SUSPEND_PROGRESS 0
	if poolexists $TESTPOOL1; then
		zpool export $TESTPOOL1 || \
		    log_note "$TESTPOOL1 cannot be exported: left busy"
	fi
	rm -rf $DEVDIR
}

log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2
log_must zpool create -f -O mountpoint=none $TESTPOOL1 $VDEV1 $VDEV2
log_must zfs create -o mountpoint=$MNT $TESTPOOL1/fs
log_must dd if=/dev/urandom of=$MNT/file bs=1M count=64
log_must zfs unmount $TESTPOOL1/fs
log_must zfs set canmount=noauto $TESTPOOL1/fs
sync_pool $TESTPOOL1

log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 1
log_must zpool remove $TESTPOOL1 $VDEV2
log_must is_pool_removing $TESTPOOL1

# The removal thread stops, and the pool is no longer loaded.
log_must zinject -u $TESTPOOL1

h1=$(zinject -q -w spa_load_impl:end -s $HOLD $TESTPOOL1)
[[ -n $h1 ]] || log_fail "zinject -w spa_load_impl:end failed"
ids="$h1"
h2=$(zinject -q -w spa_vdev_exit:unlocked -s $HOLD $TESTPOOL1)
[[ -n $h2 ]] || log_fail "zinject -w spa_vdev_exit:unlocked failed"
ids="$h1 $h2"
log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 0

# The first open loads the pool, and the load restarts the removal.
zpool status $TESTPOOL1 > /dev/null 2>&1 &
opid=$!
wait_held $h1 "the load"
wait_held $h2 "the completed removal"
kill -0 $opid 2>/dev/null || log_fail "the open returned during the load"

log_must zinject -c $h1
ids="$h2"
wait_pid $opid || log_fail "the open did not return in ${TIMEOUT}s"
opid=0
log_must zinject -c $h2
ids=""

log_must zpool wait -t remove $TESTPOOL1
sync_pool $TESTPOOL1
log_must is_pool_removed $TESTPOOL1

#
# The removal may still be leaving its last wait: retry while the pool
# is busy.  Without the fix it stays busy.
#
log_must_busy zpool export $TESTPOOL1
log_must zpool import -d $DEVDIR $TESTPOOL1
log_mustnot eval "zpool status -P $TESTPOOL1 | grep -q $VDEV2"
log_must zpool destroy $TESTPOOL1

log_pass "A removal that completes while its pool is loaded leaves the" \
    "pool exportable"
