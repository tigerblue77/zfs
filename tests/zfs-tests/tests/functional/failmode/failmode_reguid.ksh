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
#	zpool reguid waits for its txg sync without holding
#	spa_namespace_lock: when that sync suspends the pool, zinject and
#	zpool status of another pool keep working, no other pool can take
#	the new guid, zpool clear resumes the pool, and the reguid then
#	completes.
#
# STRATEGY:
#	1. Create a pool with no mounted dataset, a second pool and a
#	   degraded mirror, and stop background txg syncs
#	2. A reguid of the degraded mirror to the new guid must fail and
#	   leave that guid free
#	3. Fail every write and every probe of the first pool's vdev, and
#	   change its guid to the new guid in the background: the txg
#	   sync of the change suspends the pool
#	4. The listing of zinject handlers and zpool status of the second
#	   pool and of the suspended pool must return within the timeout
#	   while the reguid waits, and a reguid of the second pool to the
#	   same guid must fail
#	5. zinject -c and zpool clear must return within the timeout; the
#	   reguid must then succeed, and the first pool alone must have
#	   the new guid, after an export and import too
#
# NOTES:
#	Without the probe failure, a failed write is reallocated and
#	retried forever instead of suspending the pool.  The listing of
#	zinject handlers takes spa_namespace_lock.  Without the fix, the
#	reguid holds that lock until a reboot: every zpool command on the
#	system blocks, and the cleanup then leaves the pools as they are.
#

verify_runnable "global"

log_assert "zpool reguid does not hold the namespace lock while it" \
    "waits for a suspended pool"

typeset -i TIMEOUT=60
typeset -i rpid=0
typeset -i blocked=0
typeset NEWGUID=7208386112437612741
typeset otherguid
typeset DEVDIR=$TEST_BASE_DIR/failmode_reguid
typeset VDEV=$DEVDIR/vdev
typeset OTHER=$DEVDIR/other
typeset M1=$DEVDIR/m1
typeset M2=$DEVDIR/m2
typeset OUT=$DEVDIR/reguid.out

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
	if ((rpid > 0)) && kill -0 $rpid 2>/dev/null; then
		log_note "zpool reguid is still running"
	fi
	zinject -c all
	for pool in $TESTPOOL $TESTPOOL1 $TESTPOOL2; do
		if poolexists $pool; then
			zpool clear $pool
			destroy_pool $pool
		fi
	done
	rm -rf $DEVDIR
}

log_must save_tunable TXG_TIMEOUT
log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $VDEV $OTHER $M1 $M2
log_must set_tunable32 TXG_TIMEOUT 600

log_must zpool create -f -o failmode=wait -O mountpoint=none $TESTPOOL $VDEV
log_must zpool create -f -O mountpoint=none $TESTPOOL1 $OTHER
log_must zpool create -f -O mountpoint=none $TESTPOOL2 mirror $M1 $M2
otherguid=$(get_pool_prop guid $TESTPOOL1)

log_must zpool offline $TESTPOOL2 $M2
log_mustnot zpool reguid -g $NEWGUID $TESTPOOL2

log_must zpool sync $TESTPOOL $TESTPOOL1
log_must zinject -d $VDEV -e io -T write $TESTPOOL
log_must zinject -d $VDEV -e nxio -T probe $TESTPOOL
log_must [ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]

zpool reguid -g $NEWGUID $TESTPOOL > $OUT 2>&1 &
rpid=$!

typeset -i waited=0
while [[ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]]; do
	if ! kill -0 $rpid 2>/dev/null; then
		wait $rpid
		log_fail "zpool reguid returned $? before the pool suspended:" \
		    "$(<$OUT)"
	fi
	((waited++ >= TIMEOUT)) && log_fail "the pool did not suspend"
	sleep 1
done

log_must bounded zinject
log_must bounded zpool status $TESTPOOL1
log_must bounded zpool status $TESTPOOL
kill -0 $rpid 2>/dev/null || \
    log_fail "zpool reguid returned while the pool was suspended"
log_mustnot bounded zpool reguid -g $NEWGUID $TESTPOOL1
log_must [ "$(get_pool_prop guid $TESTPOOL1)" = "$otherguid" ]

log_must bounded zinject -c all
log_must bounded zpool clear $TESTPOOL
if ! wait_pid $rpid; then
	blocked=1
	log_fail "zpool reguid still running ${TIMEOUT}s after zpool clear"
fi
wait $rpid
typeset -i rc=$?
rpid=0
log_note "zpool reguid returned $rc: $(<$OUT)"
log_must [ $rc -eq 0 ]
log_must [ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]
log_must [ "$(get_pool_prop guid $TESTPOOL)" = "$NEWGUID" ]
log_must [ "$(get_pool_prop guid $TESTPOOL1)" = "$otherguid" ]

log_must zpool export $TESTPOOL
log_must zpool import -d $DEVDIR $TESTPOOL
log_must [ "$(get_pool_prop guid $TESTPOOL)" = "$NEWGUID" ]

log_pass "zpool reguid does not hold the namespace lock while it" \
    "waits for a suspended pool"
