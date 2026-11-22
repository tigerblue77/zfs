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
#	A vdev change made of several steps (the removal of a log, cache
#	or data device, a RAID-Z expansion, a split) waits for a stalled
#	sync between two of its steps without holding spa_namespace_lock:
#	zinject and zpool status of every pool keep working meanwhile, and
#	the change completes once the sync goes on.
#
# STRATEGY:
#	1. Create a pool with two data vdevs, a log and a cache device, and
#	   another pool
#	2. Register two zinject -w handlers at the spa_sync:config point
#	   of the pool, and make the pool sync: the first one holds the
#	   sync, which holds the config lock as reader
#	3. Start the change in the background, and wait until zpool status
#	   of the pool blocks: the change then waits for the config lock as
#	   writer
#	4. Remove the first handler: the change makes its first step, and
#	   the second handler holds the next sync, which the change waits
#	   for before its next step
#	5. The listing of zinject handlers and zpool status of both pools
#	   must return within the timeout while the change still waits;
#	   remove the second handler, and the change must succeed
#	6. As in 2 to 5, remove the log device, the cache device and the
#	   second data vdev; then expand a RAID-Z vdev of a new pool, and
#	   split a mirror of another one
#
# NOTES:
#	The listing of zinject handlers takes spa_namespace_lock.  Without
#	the fix, the change holds it while it waits between its steps, and
#	the listing returns only once the handlers are removed.
#

verify_runnable "global"

log_assert "A vdev change waits for a stalled sync between its steps" \
    "without holding spa_namespace_lock"

typeset -i TIMEOUT=30
typeset -i HOLD=100
typeset -i cpid=0
typeset -i spid=0
typeset -i blocked=0
typeset stuck=""
typeset ids=""
typeset DEVDIR=$TEST_BASE_DIR/vdev_change_step_wait
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
# Wait for the sync to be held at the point of a zinject -w handler,
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
			log_fail "no sync reached the point of zinject" \
			    "handler $1 in ${TIMEOUT}s"
		fi
		sleep 0.1
	done
}

#
# Wait until zpool status of the pool blocks for 5 seconds: the change
# then waits for the config lock as writer, which new readers wait
# behind.  That status returns once the change has the lock.
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
# Hold a sync of the pool while the change waits for the config lock,
# so that the next sync is the one the change waits for after its first
# step.  Hold that one too, and check that zinject and zpool status
# answer while the change waits between its steps.
#
function check_step # pool cmd [args]
{
	typeset pool=$1
	shift

	h1=$(zinject -q -w spa_sync:config -s $HOLD $pool)
	[[ -n $h1 ]] || log_fail "zinject -w failed"
	h2=$(zinject -q -w spa_sync:config -s $HOLD $pool)
	[[ -n $h2 ]] || log_fail "zinject -w failed"
	ids="$h1 $h2"
	zpool sync $pool &
	spid=$!
	wait_held $h1

	"$@" > $OUT 2>&1 &
	cpid=$!
	wait_writer $pool
	log_must bounded zinject -c $h1

	# The change makes its first step and waits for the next sync.
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
	typeset id pid pool

	#
	# Without the fix, the commands return once the handlers are
	# removed.  Removing one does not need spa_namespace_lock until
	# the thread it holds has left, so do it in the background.
	#
	for id in $ids; do
		zinject -c $id > /dev/null 2>&1 &
	done
	if ((blocked != 0)); then
		((cpid > 0)) && stuck="$stuck $cpid"
		for pid in $stuck; do
			if ! wait_pid $pid $((HOLD + TIMEOUT)); then
				log_note "commands are blocked: pools left"
				return
			fi
		done
	fi
	zinject -c all
	((cpid > 0)) && wait_pid $cpid
	((spid > 0)) && wait_pid $spid
	for pool in $TESTPOOL1 $TESTPOOL2 $TESTPOOL3; do
		poolexists $pool && destroy_pool $pool
	done
	rm -rf $DEVDIR
}

log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $OTHER $DEVDIR/vdev{1..4} \
    $DEVDIR/log $DEVDIR/cache $DEVDIR/raidz{1..4} $DEVDIR/mirror{1,2}
log_must zpool create -f -O mountpoint=none $TESTPOOL3 $OTHER

# Removals: spa_vdev_remove_log(), the cache device, vdev_passivate().
log_must zpool create -f -O mountpoint=none $TESTPOOL1 \
    $DEVDIR/vdev1 $DEVDIR/vdev2 log $DEVDIR/log cache $DEVDIR/cache
check_step $TESTPOOL1 zpool remove $TESTPOOL1 $DEVDIR/log
check_step $TESTPOOL1 zpool remove $TESTPOOL1 $DEVDIR/cache
check_step $TESTPOOL1 zpool remove $TESTPOOL1 $DEVDIR/vdev2
log_must zpool wait -t remove $TESTPOOL1
log_mustnot eval "zpool status -P $TESTPOOL1 | \
    grep -qE '$DEVDIR/(log|cache|vdev2)[[:space:]]'"
log_must destroy_pool $TESTPOOL1

# RAID-Z expansion: the attach waits for the deferred frees.
log_must zpool create -f -O mountpoint=none $TESTPOOL1 \
    raidz1 $DEVDIR/raidz1 $DEVDIR/raidz2 $DEVDIR/raidz3
check_step $TESTPOOL1 zpool attach $TESTPOOL1 raidz1-0 $DEVDIR/raidz4
log_must zpool wait -t raidz_expand $TESTPOOL1
log_must eval "zpool status -P $TESTPOOL1 | grep -q $DEVDIR/raidz4"
log_must destroy_pool $TESTPOOL1

# zpool split: the pool syncs its logs out before the split.
log_must zpool create -f -O mountpoint=none $TESTPOOL1 \
    mirror $DEVDIR/mirror1 $DEVDIR/mirror2
check_step $TESTPOOL1 zpool split $TESTPOOL1 $TESTPOOL2
log_must zpool import -d $DEVDIR $TESTPOOL2
log_must destroy_pool $TESTPOOL2
log_must destroy_pool $TESTPOOL1

log_pass "A vdev change waits for a stalled sync between its steps" \
    "without holding spa_namespace_lock"
