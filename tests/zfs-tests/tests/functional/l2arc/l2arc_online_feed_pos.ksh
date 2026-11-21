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
#	A cache device is written to again after it was offline or faulted
#	and is brought back with zpool online or zpool clear.
#
# STRATEGY:
#	1. Create a pool with a cache device, create random files in it
#		and random read them, and verify that L2ARC writes happen.
#	2. Offline the cache device and wait long enough for its feed
#		thread to see it, then online it.
#	3. Rewrite the files and random read them, and verify that L2ARC
#		writes happen again within 60 seconds.
#	4. Repeat 2 and 3 with zpool offline -f and zpool clear.
#

verify_runnable "global"

command -v fio > /dev/null || log_unsupported "fio missing"

log_assert "A cache device is written to again after zpool online or clear."

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi

	log_must set_tunable32 L2ARC_NOPREFETCH $noprefetch
}
log_onexit cleanup

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

# L2ARC_NOPREFETCH is set to 0 to let L2ARC handle prefetches
typeset noprefetch=$(get_tunable L2ARC_NOPREFETCH)
log_must set_tunable32 L2ARC_NOPREFETCH 0

# Small enough that the files can be rewritten in place in the 1 GiB pool.
typeset fill_mb=200
typeset cache_sz=$(( 1.4 * $fill_mb ))
export FILE_SIZE=$(( floor($fill_mb / $NUMJOBS) ))M

log_must truncate -s ${cache_sz}M $VDEV_CACHE

log_must zpool create -f $TESTPOOL $VDEV cache $VDEV_CACHE

typeset written=$(kstat arcstats.l2_write_bytes)
log_must fio $FIO_SCRIPTS/mkfiles.fio
log_must fio $FIO_SCRIPTS/random_reads.fio
log_must wait_l2arc_writes $written

for cycle in online clear; do
	if [[ $cycle == online ]]; then
		log_must zpool offline $TESTPOOL $VDEV_CACHE
		log_must check_state $TESTPOOL $VDEV_CACHE "OFFLINE"
	else
		log_must zpool offline -f $TESTPOOL $VDEV_CACHE
		log_must check_state $TESTPOOL $VDEV_CACHE "FAULTED"
	fi

	# The feed thread checks its device at least once a second.
	log_must sleep 5

	log_must zpool $cycle $TESTPOOL $VDEV_CACHE
	log_must check_state $TESTPOOL $VDEV_CACHE "ONLINE"

	written=$(kstat arcstats.l2_write_bytes)
	log_note "after zpool $cycle: l2_write_bytes=$written" \
	    "l2_feeds=$(kstat arcstats.l2_feeds)"

	# Rewritten blocks are new buffers, not yet in the L2ARC.
	log_must fio $FIO_SCRIPTS/mkfiles.fio
	log_must fio $FIO_SCRIPTS/random_reads.fio

	wait_l2arc_writes $written || log_fail "Nothing was written to" \
	    "the cache device within 60 seconds after zpool $cycle."
	log_note "l2_write_bytes=$(kstat arcstats.l2_write_bytes)" \
	    "l2_feeds=$(kstat arcstats.l2_feeds)"
done

log_must zpool destroy -f $TESTPOOL

log_pass "A cache device is written to again after zpool online or clear."
