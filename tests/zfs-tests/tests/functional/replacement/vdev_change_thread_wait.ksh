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
#	A vdev change waits for the autotrim and rebuild threads that it
#	stops without holding spa_namespace_lock: while a thread is slow
#	to stop, zinject and zpool status of the pool and of another pool
#	keep working, and the change completes once the thread stops.
#
# STRATEGY:
#	1. Create a pool with autotrim on, export and import it so that
#	   its autotrim thread runs, and create another pool
#	2. Hold the autotrim thread at the vdev_autotrim_thread:exit point
#	   and add a vdev to the pool in the background
#	3. The listing of zinject handlers must show the thread held
#	   within the timeout, zpool status of both pools must return
#	   within the timeout while the add still waits, and the add must
#	   succeed once the thread is released
#	4. Create a pool with autotrim on and data, stop rebuild progress,
#	   and attach a second vdev with a sequential rebuild
#	5. Hold the autotrim thread and the rebuild thread at their exit
#	   points and detach the new vdev in the background
#	6. As in 3, first for the autotrim thread and then for the
#	   rebuild thread, which stops only when the detach asks it to
#
# NOTES:
#	The listing of zinject handlers takes spa_namespace_lock.  Without
#	the fix, the vdev change holds it while it waits for the held
#	thread, and the listing returns only when the hold time runs out.
#

verify_runnable "global"

log_assert "A vdev change waits for the autotrim and rebuild threads" \
    "without holding spa_namespace_lock"

typeset -i TIMEOUT=30
typeset -i HOLD=100
typeset -i cpid=0
typeset -i blocked=0
typeset stuck=""
typeset DEVDIR=$TEST_BASE_DIR/vdev_change_thread_wait
typeset VDEV1=$DEVDIR/vdev1
typeset VDEV2=$DEVDIR/vdev2
typeset VDEV3=$DEVDIR/vdev3
typeset VDEV4=$DEVDIR/vdev4
typeset OTHER=$DEVDIR/other
typeset OUT=$DEVDIR/cmd.out
typeset LIST=$DEVDIR/zinject.out

#
# Wait up to TIMEOUT seconds for a process to exit.
#
function wait_pid # pid
{
	typeset -i tenths=0

	while kill -0 $1 2>/dev/null; do
		((tenths++ >= TIMEOUT * 10)) && return 1
		sleep 0.1
	done
	return 0
}

#
# Run a command and give it TIMEOUT seconds; fail if it is still
# running then.
#
function bounded # cmd [args]
{
	"$@" &
	typeset -i pid=$!

	if ! wait_pid $pid; then
		blocked=1
		stuck="$stuck $pid"
		log_fail "$* still running after ${TIMEOUT}s"
	fi
	wait $pid
}

#
# Wait for a thread to be held at the point of a zinject -w handler,
# giving each listing of the handlers TIMEOUT seconds.
#
function wait_held # id
{
	typeset -i t0=$SECONDS
	typeset held

	while :; do
		zinject > $LIST 2>&1 &
		typeset -i pid=$!
		if ! wait_pid $pid; then
			blocked=1
			stuck="$stuck $pid"
			log_fail "zinject still running after ${TIMEOUT}s"
		fi
		wait $pid
		held=$(awk -v id=$1 '$1 == id { print $NF }' $LIST)
		[[ -n $held ]] && ((held > 0)) && return
		if ((SECONDS - t0 > TIMEOUT)); then
			log_fail "no thread reached the point of zinject" \
			    "handler $1 in ${TIMEOUT}s"
		fi
		sleep 0.1
	done
}

#
# With a thread held at the point of handler id, check that the pool,
# another pool and zinject answer while the vdev change still waits,
# then release the thread.
#
function check_held # id pool
{
	wait_held $1
	log_must bounded zpool status $TESTPOOL3
	log_must bounded zpool status $2
	kill -0 $cpid 2>/dev/null || \
	    log_fail "the vdev change returned while a thread was held"
	log_must bounded zinject -c $1
}

#
# Wait for the vdev change to finish and check that it succeeded.
#
function wait_change
{
	if ! wait_pid $cpid; then
		blocked=1
		log_fail "the vdev change still running ${TIMEOUT}s after" \
		    "the threads were released"
	fi
	wait $cpid
	typeset -i rc=$?
	cpid=0
	log_note "the vdev change returned $rc: $(<$OUT)"
	log_must [ $rc -eq 0 ]
}

function cleanup
{
	#
	# Without the fix, the commands return once the hold time of the
	# handlers runs out.
	#
	if ((blocked != 0)); then
		typeset -i waited=0
		typeset pid

		((cpid > 0)) && stuck="$stuck $cpid"
		for pid in $stuck; do
			while kill -0 $pid 2>/dev/null &&
			    ((waited++ < HOLD + TIMEOUT)); do
				sleep 1
			done
			if kill -0 $pid 2>/dev/null; then
				log_note "commands are blocked: pools left"
				return
			fi
		done
	fi
	zinject -c all
	((cpid > 0)) && wait_pid $cpid
	restore_tunable SCAN_SUSPEND_PROGRESS
	for pool in $TESTPOOL1 $TESTPOOL2 $TESTPOOL3; do
		poolexists $pool && destroy_pool $pool
	done
	rm -rf $DEVDIR
}

log_must save_tunable SCAN_SUSPEND_PROGRESS
log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2 $VDEV3 $VDEV4 $OTHER
log_must zpool create -f -O mountpoint=none $TESTPOOL3 $OTHER

# zpool add: spa_vdev_enter() stops the autotrim thread.
log_must zpool create -f -o autotrim=on -O mountpoint=none $TESTPOOL1 $VDEV1
log_must zpool export $TESTPOOL1
log_must zpool import -d $DEVDIR $TESTPOOL1

id=$(zinject -q -w vdev_autotrim_thread:exit -s $HOLD $TESTPOOL1)
[[ -n $id ]] || log_fail "zinject -w failed"
zpool add -f $TESTPOOL1 $VDEV2 > $OUT 2>&1 &
cpid=$!
check_held $id $TESTPOOL1
wait_change
log_must eval "zpool status -P $TESTPOOL1 | grep -q $VDEV2"

#
# zpool detach: spa_vdev_detach_enter() stops the autotrim thread, then
# the rebuild thread, which no progress can end meanwhile.
#
log_must zpool create -f -o autotrim=on $TESTPOOL2 $VDEV3
mntpnt=$(get_prop mountpoint $TESTPOOL2)
log_must dd if=/dev/urandom of=$mntpnt/file bs=1M count=16
sync_pool $TESTPOOL2
log_must set_tunable32 SCAN_SUSPEND_PROGRESS 1
log_must zpool attach -s $TESTPOOL2 $VDEV3 $VDEV4

aid=$(zinject -q -w vdev_autotrim_thread:exit -s $HOLD $TESTPOOL2)
[[ -n $aid ]] || log_fail "zinject -w failed"
rid=$(zinject -q -w vdev_rebuild_thread:exit -s $HOLD $TESTPOOL2)
[[ -n $rid ]] || log_fail "zinject -w failed"
zpool detach $TESTPOOL2 $VDEV4 > $OUT 2>&1 &
cpid=$!
check_held $aid $TESTPOOL2
check_held $rid $TESTPOOL2
wait_change
log_mustnot eval "zpool status -P $TESTPOOL2 | grep -q $VDEV4"

log_pass "A vdev change waits for the autotrim and rebuild threads" \
    "without holding spa_namespace_lock"
