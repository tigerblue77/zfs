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
#	A zpool export whose first txg sync suspends the pool fails
#	instead of waiting with spa_namespace_lock held: commands on
#	another pool keep working, zpool clear resumes the pool, and
#	the export then succeeds.
#
# STRATEGY:
#	1. Create a pool with no mounted dataset and a second pool, and
#	   stop background txg syncs
#	2. Fail every write and every probe of the first pool's vdev
#	3. Export it in the background: the history record of the
#	   export dirties a txg, and the export's first txg sync
#	   suspends the pool
#	4. The export must fail, and zpool status of both pools must
#	   return within the timeout
#	5. Clear the injections and the pool; the pool must no longer be
#	   suspended, and a second export must succeed
#
# NOTES:
#	Without the probe failure, a failed write is reallocated and
#	retried forever instead of suspending the pool.  Nothing is
#	mounted, so that the export's unmount does not sync first.
#	Without the fix, the namespace lock stays held until a reboot:
#	every zpool command on the system blocks, and the cleanup then
#	leaves the pools as they are.
#

verify_runnable "global"

log_assert "A zpool export that suspends its pool does not hold the" \
    "namespace lock while it waits"

typeset -i TIMEOUT=60
typeset -i epid=0
typeset -i blocked=0
typeset DEVDIR=$TEST_BASE_DIR/failmode_export
typeset VDEV=$DEVDIR/vdev
typeset OTHER=$DEVDIR/other
typeset OUT=$DEVDIR/export.out

#
# Wait up to TIMEOUT seconds for a process to exit.
#
function wait_pid # pid
{
	typeset -i waited=0

	while kill -0 $1 2>/dev/null; do
		((waited++ >= TIMEOUT)) && return 1
		sleep 1
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
		log_fail "$* still running after ${TIMEOUT}s"
	fi
	wait $pid
}

function cleanup
{
	restore_tunable TXG_TIMEOUT
	if ((blocked != 0)); then
		log_note "commands are blocked: leaving the pools"
		return
	fi
	if ((epid > 0)) && kill -0 $epid 2>/dev/null; then
		log_note "zpool export is still running"
	fi
	zinject -c all
	poolexists $TESTPOOL && zpool clear $TESTPOOL
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -rf $DEVDIR
}

log_must save_tunable TXG_TIMEOUT
log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV $OTHER
log_must set_tunable32 TXG_TIMEOUT 600

log_must zpool create -f -o failmode=wait -O mountpoint=none $TESTPOOL $VDEV
log_must zpool create -f $TESTPOOL1 $OTHER
log_must zpool sync $TESTPOOL $TESTPOOL1

log_must zinject -d $VDEV -e io -T write $TESTPOOL
log_must zinject -d $VDEV -e nxio -T probe $TESTPOOL
log_must [ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]

zpool export $TESTPOOL > $OUT 2>&1 &
epid=$!

typeset -i waited=0
while [[ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]]; do
	if ! kill -0 $epid 2>/dev/null; then
		wait $epid
		log_fail "zpool export returned $? before the pool suspended:" \
		    "$(<$OUT)"
	fi
	((waited++ >= TIMEOUT)) && log_fail "the pool did not suspend"
	sleep 1
done

log_must bounded zpool status $TESTPOOL1
if ! wait_pid $epid; then
	blocked=1
	log_fail "zpool export still running ${TIMEOUT}s after the pool" \
	    "suspended"
fi
wait $epid
typeset -i rc=$?
epid=0
log_note "zpool export returned $rc: $(<$OUT)"
log_must [ $rc -ne 0 ]
log_must poolexists $TESTPOOL
log_must [ "$(kstat_pool $TESTPOOL state)" = "SUSPENDED" ]

log_must bounded zpool status $TESTPOOL
log_must bounded zinject -c all
log_must bounded zpool clear $TESTPOOL
log_must [ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]

log_must zpool export $TESTPOOL
log_must zpool import -d $DEVDIR $TESTPOOL

log_pass "A zpool export that suspends its pool does not hold the" \
    "namespace lock while it waits"
