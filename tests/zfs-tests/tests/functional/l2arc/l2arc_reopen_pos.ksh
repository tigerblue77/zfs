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
. $STF_SUITE/tests/functional/l2arc/l2arc.cfg

#
# DESCRIPTION:
#	'zpool reopen' reopens the cache devices of a pool, as it does the
#	devices of its vdev tree.
#
# STRATEGY:
#	1. Create a pool with a cache device, create files in it, and
#	   export it.
#	2. Move the cache device out of the directory and import the pool
#	   from that directory: the cache device is UNAVAIL.
#	3. Move the cache device back and run 'zpool reopen <pool>'.
#	4. Verify that the cache device is ONLINE and that
#	   'zpool status -x' reports the pool healthy.
#	5. Random read the files and verify that L2ARC writes happen within
#	   60 seconds.
#

verify_runnable "global"

command -v fio > /dev/null || log_unsupported "fio missing"

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -f $VDEV_CACHE $MOVED

	log_must set_tunable32 L2ARC_NOPREFETCH $noprefetch
}

function check_cache_state # state
{
	typeset state=$(get_device_state $TESTPOOL $VDEV_CACHE cache)

	[[ "$state" == "$1" ]] || \
	    log_fail "The cache device is '$state', expected '$1'."
}

function pool_is_healthy
{
	[[ "$(zpool status -x $TESTPOOL)" == "pool '$TESTPOOL' is healthy" ]]
}

#
# Wait at most 60 seconds for the L2ARC to write more than $1 bytes in all.
# This pool has the only cache device, so the global kstat is its own.
#
function wait_l2arc_writes # bytes
{
	typeset -i tries=0
	while (( $(kstat arcstats.l2_write_bytes) <= $1 && tries < 60 )); do
		sleep 1
		(( tries += 1 ))
	done
	(( $(kstat arcstats.l2_write_bytes) > $1 ))
}

log_assert "'zpool reopen' reopens a cache device."

# Outside $VDIR, so that 'zpool import -d $VDIR' does not find it.
MOVED=$TESTDIR/l2arc_reopen_pos.cache

typeset noprefetch=$(get_tunable L2ARC_NOPREFETCH)
log_onexit cleanup

# L2ARC_NOPREFETCH is set to 0 to let L2ARC handle prefetches
log_must set_tunable32 L2ARC_NOPREFETCH 0

typeset fill_mb=200
typeset cache_sz=$(( 1.4 * $fill_mb ))
export FILE_SIZE=$(( floor($fill_mb / $NUMJOBS) ))M

log_must truncate -s ${cache_sz}M $VDEV_CACHE
log_must zpool create -f $TESTPOOL $VDEV cache $VDEV_CACHE
log_must fio $FIO_SCRIPTS/mkfiles.fio
log_must zpool export $TESTPOOL

log_must mv $VDEV_CACHE $MOVED
log_must zpool import -d $VDIR $TESTPOOL
check_cache_state UNAVAIL
log_mustnot pool_is_healthy

log_must mv $MOVED $VDEV_CACHE
log_must zpool reopen $TESTPOOL
check_cache_state ONLINE
log_must pool_is_healthy

typeset written=$(kstat arcstats.l2_write_bytes)
log_must fio $FIO_SCRIPTS/random_reads.fio
wait_l2arc_writes $written || \
    log_fail "Nothing was written to the cache device within 60 seconds."

log_must zpool destroy -f $TESTPOOL

log_pass "'zpool reopen' reopens a cache device."
