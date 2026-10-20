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
#	A vdev configuration change (here zpool detach) returns when
#	the pool suspends while it waits for its txg to sync, and does
#	not leave spa_namespace_lock held: commands on another pool
#	keep working.
#
# STRATEGY:
#	1. Create a mirror of two file vdevs and a second, healthy pool,
#	   and stop background txg syncs.
#	2. Fail every write and every probe on both sides of the mirror.
#	3. Run zpool detach in the background; its txg sync suspends
#	   the pool.
#	4. zpool status of the other pool must return within the
#	   timeout, and zpool detach must return an error.
#
# NOTES:
#	On a kernel where spa_vdev_config_exit() still waits with
#	txg_wait_synced() under spa_namespace_lock, step 4 hangs and so
#	does every recovery: "zpool clear" opens the pool through the
#	namespace lock, "zinject -c all" lists handlers under it and
#	"zinject -c <id>" drops its spa reference under it.  The test
#	then fails and the VM must be rebooted.
#

verify_runnable "global"

log_assert "A vdev config change returns when the pool suspends"

typeset -i TIMEOUT=60
typeset -i dpid=0 spid=0
typeset DEVDIR=$TEST_BASE_DIR/detach_suspend
typeset M1=$DEVDIR/m1
typeset M2=$DEVDIR/m2
typeset OTHER=$DEVDIR/other
set -A ids

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
# Run a command in the background and give it TIMEOUT seconds.
# Returns 0 if it finished, 1 if it is still blocked.
#
function bounded # cmd [args]
{
	"$@" &
	typeset -i pid=$!
	if wait_pid $pid; then
		wait $pid
		log_note "$* returned $?"
		return 0
	fi
	log_note "$* still blocked after ${TIMEOUT}s"
	return 1
}

function cleanup
{
	if ((dpid > 0)) && ! wait_pid $dpid; then
		log_note "zpool detach is still blocked; trying recovery"
		for id in ${ids[@]}; do
			bounded zinject -c $id || break
		done
		if ! bounded zpool clear $TESTPOOL; then
			log_note "recovery impossible: reboot the VM"
			return
		fi
	fi
	zinject -c all
	zpool clear $TESTPOOL
	restore_tunable TXG_TIMEOUT
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -rf $DEVDIR
}

function inject # args
{
	typeset out

	out=$(zinject "$@") || log_fail "zinject $* failed: $out"
	ids[${#ids[@]}]=$(echo "$out" | awk '/Added handler/ { print $3 }')
}

log_must save_tunable TXG_TIMEOUT
log_onexit cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s $MINVDEVSIZE $M1 $M2 $OTHER
log_must set_tunable32 TXG_TIMEOUT 600

log_must zpool create -f $TESTPOOL mirror $M1 $M2
log_must zpool create -f $TESTPOOL1 $OTHER
log_must zpool sync $TESTPOOL $TESTPOOL1

inject -d $M1 -e io -T write $TESTPOOL
inject -d $M2 -e io -T write $TESTPOOL
inject -d $M1 -e nxio -T probe $TESTPOOL
inject -d $M2 -e nxio -T probe $TESTPOOL
log_note "injection handlers: ${ids[*]}"
log_must [ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]

zpool detach $TESTPOOL $M2 &
dpid=$!

# Give the detach time to reach its txg wait, and the pool to suspend.
typeset -i waited=0
while [[ "$(kstat_pool $TESTPOOL state)" != "SUSPENDED" ]]; do
	((waited++ >= TIMEOUT)) && break
	sleep 1
done
log_note "$TESTPOOL state: $(kstat_pool $TESTPOOL state)"

if ! bounded zpool status $TESTPOOL1; then
	log_fail "zpool status of another pool blocked: zpool detach" \
	    "holds spa_namespace_lock on a suspended pool"
fi

if ! wait_pid $dpid; then
	log_fail "zpool detach hung for ${TIMEOUT}s on a suspended pool"
fi
wait $dpid
typeset -i rc=$?
dpid=0

log_note "zpool detach returned $rc"
log_must [ "$(kstat_pool $TESTPOOL state)" == "SUSPENDED" ]
log_must [ $rc -ne 0 ]

log_pass "A vdev config change returns when the pool suspends"
