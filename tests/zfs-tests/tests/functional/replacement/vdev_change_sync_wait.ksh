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
#	A vdev change on a pool whose txg sync is stalled, as on a device
#	that does not answer, waits for the config lock and for its own
#	txg without holding spa_namespace_lock: zinject and zpool status
#	of another pool keep working meanwhile, and the change completes
#	once the sync goes on.
#
# STRATEGY:
#	1. Create a pool and another pool
#	2. Register two zinject -n handlers at the spa_sync:config point
#	   of the pool, and make the pool sync: the first one holds the
#	   sync, which holds the config lock as reader
#	3. Attach a second vdev in the background, and wait until zpool
#	   status of the pool blocks: the attach then waits for the config
#	   lock as writer
#	4. The listing of zinject handlers and zpool status of the other
#	   pool must return within the timeout while the attach waits
#	5. Remove the first handler: the second one holds the next sync,
#	   while the attach waits for its txg to sync
#	6. The listing and zpool status of both pools must return within
#	   the timeout while the attach still waits; remove the second
#	   handler, and the attach must succeed
#	7. As in 2 to 6, detach the new vdev once it is resilvered
#
# NOTES:
#	The listing of zinject handlers takes spa_namespace_lock.  Without
#	the fix, the vdev change holds it while it waits, and the listing
#	returns only once the handlers are removed.
#

verify_runnable "global"
verify_inject_points

log_assert "A vdev change waits for a stalled sync without holding" \
    "spa_namespace_lock"

typeset -i TIMEOUT=30
typeset -i cpid=0
typeset -i spid=0
typeset -i blocked=0
typeset stuck=""
typeset ids=""
typeset DEVDIR=$TEST_BASE_DIR/vdev_change_sync_wait
typeset VDEV1=$DEVDIR/vdev1
typeset VDEV2=$DEVDIR/vdev2
typeset OTHER=$DEVDIR/other
typeset OUT=$DEVDIR/cmd.out
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
# Wait for the sync to be held at the point of a zinject -n handler,
# giving each listing of the handlers TIMEOUT seconds.
#
function wait_held # id
{
	typeset -i t0=$SECONDS
	typeset held

	while :; do
		inject_point_field $1 INJECT > $LIST 2>&1 &
		typeset -i pid=$!
		if ! wait_pid $pid; then
			blocked=1
			stuck="$stuck $pid"
			log_fail "zinject still running after ${TIMEOUT}s"
		fi
		wait $pid
		held=$(<$LIST)
		[[ $held == +([0-9]) ]] && ((held > 0)) && return
		if ((SECONDS - t0 > TIMEOUT)); then
			log_fail "no sync reached the point of zinject" \
			    "handler $1 in ${TIMEOUT}s"
		fi
		sleep 0.1
	done
}

#
# Wait until zpool status of the pool blocks for 5 seconds: the vdev
# change then waits for the config lock as writer, which new readers
# wait behind.  That status returns once the change has the lock.
#
function wait_writer # pool
{
	typeset -i t0=$SECONDS

	while :; do
		zpool status $1 > /dev/null 2>&1 &
		typeset -i pid=$!
		if ! wait_pid $pid 5; then
			stuck="$stuck $pid"
			return
		fi
		wait $pid
		kill -0 $cpid 2>/dev/null || log_fail "the vdev change" \
		    "returned while the sync was held: $(<$OUT)"
		((SECONDS - t0 > TIMEOUT)) && \
		    log_fail "the vdev change did not wait in ${TIMEOUT}s"
		sleep 0.5
	done
}

#
# Hold the syncs of the pool, start the vdev change while one is held,
# and check that zinject and zpool status answer while the change
# waits for the config lock, and then for its txg.
#
function check_change # pool cmd [args]
{
	typeset pool=$1
	shift

	h1=$(zinject -q -n spa_sync:config -A pause $pool)
	[[ -n $h1 ]] || log_fail "zinject -n failed"
	h2=$(zinject -q -n spa_sync:config -A pause $pool)
	[[ -n $h2 ]] || log_fail "zinject -n failed"
	ids="$h1 $h2"
	zpool sync $pool &
	spid=$!
	wait_held $h1

	"$@" > $OUT 2>&1 &
	cpid=$!
	wait_writer $pool
	log_must bounded zpool status $TESTPOOL3
	wait_held $h1
	kill -0 $cpid 2>/dev/null || \
	    log_fail "the vdev change returned while the sync was held"
	log_must bounded zinject -c $h1

	# The next sync waits for the change, and the change for it.
	wait_held $h2
	log_must bounded zpool status $TESTPOOL3
	log_must bounded zpool status $pool
	kill -0 $cpid 2>/dev/null || \
	    log_fail "the vdev change returned while the sync was held"
	log_must bounded zinject -c $h2
	ids=""

	if ! wait_pid $cpid; then
		blocked=1
		log_fail "the vdev change still running ${TIMEOUT}s after" \
		    "the sync was released"
	fi
	wait $cpid
	typeset -i rc=$?
	cpid=0
	log_note "$* returned $rc: $(<$OUT)"
	log_must [ $rc -eq 0 ]
	log_must wait_pid $spid
	spid=0
}

function cleanup
{
	typeset id pid

	#
	# Without the fix, the commands return once the handlers are
	# removed.  Removing one by its id does not take
	# spa_namespace_lock, unlike zinject -c all, which lists them.
	#
	for id in $ids; do
		zinject -c $id > /dev/null 2>&1 &
	done
	if ((blocked != 0)); then
		((cpid > 0)) && stuck="$stuck $cpid"
		for pid in $stuck; do
			if ! wait_pid $pid $((2 * TIMEOUT)); then
				log_note "commands are blocked: pools left"
				return
			fi
		done
	fi
	zinject -c all
	((cpid > 0)) && wait_pid $cpid
	((spid > 0)) && wait_pid $spid
	for pool in $TESTPOOL1 $TESTPOOL3; do
		poolexists $pool && destroy_pool $pool
	done
	rm -rf $DEVDIR
}

log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2 $OTHER
log_must zpool create -f -O mountpoint=none $TESTPOOL3 $OTHER
log_must zpool create -f -O mountpoint=none $TESTPOOL1 $VDEV1

# zpool attach: spa_vdev_enter(), then spa_vdev_exit().
check_change $TESTPOOL1 zpool attach $TESTPOOL1 $VDEV1 $VDEV2
log_must eval "zpool status -P $TESTPOOL1 | grep -q $VDEV2"
log_must zpool wait -t resilver $TESTPOOL1

# zpool detach: spa_vdev_detach_enter(), then the teardown of the vdev.
check_change $TESTPOOL1 zpool detach $TESTPOOL1 $VDEV2
log_mustnot eval "zpool status -P $TESTPOOL1 | grep -q $VDEV2"

log_pass "A vdev change waits for a stalled sync without holding" \
    "spa_namespace_lock"
