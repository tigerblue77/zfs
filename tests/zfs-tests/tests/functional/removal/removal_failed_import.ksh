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
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	A zpool import that fails after the load does not wait, with
#	spa_namespace_lock held, for a device removal that the load
#	restarted and that is about to complete.
#
# STRATEGY:
#	1. Create a pool on two file vdevs, write to it, start the
#	   removal of the second one with the removal paused, and export
#	   the pool.
#	2. Delay the load of the pool by 15 seconds, and set the point
#	   vdev_remove_complete:start for it.
#	3. Let the removal run, and import the pool in the background
#	   with -o version=5000, which the kernel refuses once the pool
#	   is loaded.  The load restarts the removal, which copies the
#	   rest and is held at the point.
#	4. Once the load delay is over, the pool has left the
#	   import_progress kstat and the import is failing: release the
#	   removal, which now needs spa_namespace_lock.
#	5. The import must fail within 60 seconds and leave no pool; the
#	   pool must then import, with the removal done.
#
# NOTES:
#	Without the fix, the import never returns and spa_namespace_lock
#	stays held: this test and every later zpool command hang, and the
#	cleanup leaves everything as it is.
#

verify_runnable "global"

typeset DEVDIR=$TEST_BASE_DIR/removal_failed_import
typeset VDEV0=$DEVDIR/vdev0
typeset VDEV1=$DEVDIR/vdev1
typeset POINT=vdev_remove_complete:start
typeset -i TIMEOUT=60
typeset -i ipid=0
typeset id=""

function cleanup
{
	if ((ipid > 0)) && kill -0 $ipid 2>/dev/null; then
		log_note "zpool import is still running, with" \
		    "spa_namespace_lock held: leaving everything as it is"
		return
	fi
	zinject -c all
	log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 0
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $DEVDIR
}

#
# The inject count of handler $1, as zinject lists it.
#
function inject_count # id
{
	zinject | awk -v id=$1 '$1 == id { print $NF }'
}

log_onexit cleanup

log_assert "A failed zpool import does not deadlock with the device" \
    "removal that its load restarted"

log_must rm -rf $DEVDIR
log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV0 $VDEV1
log_must zpool create -f $TESTPOOL $VDEV0 $VDEV1
log_must dd if=/dev/urandom of=/$TESTPOOL/file bs=1M count=64
log_must zpool sync $TESTPOOL

log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 1
log_must zpool remove $TESTPOOL $VDEV1
log_must is_pool_removing $TESTPOOL
log_must zpool export $TESTPOOL

#
# The import delay goes first: zio_handle_pool_delay() may stop its search
# at the first handler of another kind, and miss a delay added after it.
#
log_must zinject -P import -s 15 $TESTPOOL
id=$(zinject -w $POINT -s 600 $TESTPOOL | awk '/^Added handler/ { print $3 }')
[[ -n $id ]] || log_fail "cannot set the point $POINT"
log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 0

zpool import -d $DEVDIR -o version=5000 $TESTPOOL &
ipid=$!

typeset -i waited=0
until [[ $(inject_count $id) == 1 ]]; do
	((waited++ >= TIMEOUT)) && log_fail "the removal did not reach $POINT"
	sleep 1
done
log_note "the removal is held at $POINT"

waited=0
while kstat import_progress | grep -qw $TESTPOOL; do
	((waited++ >= TIMEOUT)) && log_fail "the load did not end"
	sleep 1
done
log_must sleep 2
log_must zinject -c $id

waited=0
while kill -0 $ipid 2>/dev/null; do
	((waited++ >= TIMEOUT)) && \
	    log_fail "zpool import did not return in ${TIMEOUT}s"
	sleep 1
done
wait $ipid && log_fail "zpool import -o version=5000 succeeded"
ipid=0
log_mustnot poolexists $TESTPOOL

log_must zinject -c all
log_must zpool import -d $DEVDIR $TESTPOOL
log_must wait_for_removal $TESTPOOL
log_mustnot vdevs_in_pool $TESTPOOL $VDEV1
log_must zpool export $TESTPOOL

log_pass "A failed zpool import does not deadlock with the device" \
    "removal that its load restarted"
