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
. $STF_SUITE/tests/functional/trim/trim.kshlib
. $STF_SUITE/tests/functional/trim/trim.cfg

#
# DESCRIPTION:
#	A cache device can be removed, and its pool exported and destroyed,
#	while the L2ARC feed thread of the device trims ahead of its write
#	hand.  The feed thread then holds the config lock only in short
#	stretches, and taking the config lock as writer to remove the device
#	must not leave it waiting for that lock.  Nor must the removal wait
#	for the feed thread to go idle: with long TRIMs, its passes run back
#	to back.
#
# STRATEGY:
#	1. Set 'l2arc_trim_ahead = 1' and create a pool whose cache device
#	   is smaller than the data read through it, so that the write hand
#	   of the device wraps and each feed pass trims ahead of it.
#	2. Read the data randomly until the feed thread trims.
#	3. Split TRIMs into 512-byte extents issued one at a time, so that
#	   the TRIM of a feed pass outlasts the feed interval, and wait until
#	   a pass trims.
#	4. Remove the cache device in the background, and fail if it does
#	   not return within 60 seconds.
#	5. Add the cache device back, and do the same with zpool export,
#	   then, after the import, with zpool destroy.
#

verify_runnable "global"

command -v fio > /dev/null || log_unsupported "fio missing"

log_assert "A cache device can be removed, exported and destroyed while" \
    "its feed thread trims."

typeset l2arc_trimahead=$(get_tunable L2ARC_TRIM_AHEAD)
typeset l2arc_writemax=$(get_tunable L2ARC_WRITE_MAX)
typeset l2arc_dwpdlimit=$(get_tunable L2ARC_DWPD_LIMIT)
typeset trim_extentmax=$(get_tunable TRIM_EXTENT_BYTES_MAX)
typeset trim_queuelimit=$(get_tunable TRIM_QUEUE_LIMIT)
VDEVS="$TRIM_VDEV1 $TRIM_VDEV2"

# Set once a command has not returned: the pool must not be touched then.
typeset hung=""

function cleanup
{
	log_must set_tunable32 L2ARC_TRIM_AHEAD $l2arc_trimahead
	log_must set_tunable32 L2ARC_WRITE_MAX $l2arc_writemax
	log_must set_tunable32 L2ARC_DWPD_LIMIT $l2arc_dwpdlimit
	log_must set_tunable32 TRIM_EXTENT_BYTES_MAX $trim_extentmax
	log_must set_tunable32 TRIM_QUEUE_LIMIT $trim_queuelimit

	if [[ -n "$hung" ]]; then
		log_note "Leaving $TESTPOOL alone: $hung did not return"
	else
		poolexists $TESTPOOL && destroy_pool $TESTPOOL
		log_must rm -f $VDEVS
	fi
}
log_onexit cleanup

function slow_trims
{
	log_must set_tunable32 TRIM_EXTENT_BYTES_MAX 512
	log_must set_tunable32 TRIM_QUEUE_LIMIT 1
}

function fast_trims
{
	log_must set_tunable32 TRIM_EXTENT_BYTES_MAX $trim_extentmax
	log_must set_tunable32 TRIM_QUEUE_LIMIT $trim_queuelimit
}

function simple_trims
{
	kstat_pool $TESTPOOL iostats.simple_trim_extents_written
}

#
# Read the data until the feed thread trims ahead of its write hand.
#
function wait_feed_trims
{
	typeset -i before after tries

	for tries in {1..24}; do
		before=$(simple_trims)
		log_must fio $FIO_SCRIPTS/random_reads.fio
		after=$(simple_trims)
		if (( after > before )); then
			log_note "$((after - before)) TRIMs during the read"
			return
		fi
	done
	log_fail "The L2ARC feed thread does not trim"
}

#
# Wait until a feed pass is trimming in 512-byte extents, at least a
# thousand in a second, and return right away.  Reading more data gives
# the next pass more to trim.
#
function wait_trimming_now
{
	typeset -i before after tries

	slow_trims
	for tries in {1..12}; do
		before=$(simple_trims)
		sleep 1
		after=$(simple_trims)
		log_note "$((after - before)) TRIMs in a second"
		(( after - before >= 1000 )) && return
		RUNTIME=2
		log_must fio $FIO_SCRIPTS/random_reads.fio
		RUNTIME=5
	done
	log_fail "The L2ARC feed thread does not trim in small extents"
}

#
# Run a command in the background and wait for it for at most 60 seconds.
# It needs seconds: a few txg syncs, and the feed thread ends its TRIM after
# the TRIM in flight and exits after the writes of its pass.  A command that
# does not return then waits in the kernel, where it cannot be killed: show
# where, along with the L2ARC feed threads, and fail.
#
function must_return
{
	typeset -i pid secs=0 limit=60
	typeset p

	log_note "Running in the background: $*"
	"$@" &
	pid=$!
	while kill -0 $pid 2>/dev/null; do
		if (( secs++ == limit )); then
			hung="$*"
			if is_linux; then
				for p in $pid $(pgrep '^l2arc_'); do
					log_note "$(ps -o comm=,wchan= -p $p)"
					cat /proc/$p/stack
				done
			fi
			log_fail "$* did not return in $limit seconds"
		fi
		sleep 1
	done
	wait $pid || log_fail "$* failed"
	log_note "$* returned after about $secs seconds"
}

log_must set_tunable32 L2ARC_TRIM_AHEAD 1
log_must set_tunable32 L2ARC_WRITE_MAX $((64 * 1024 * 1024))
log_must set_tunable32 L2ARC_DWPD_LIMIT 0

log_must truncate -s $((MINVDEVSIZE)) $TRIM_VDEV2
log_must truncate -s $((4 * MINVDEVSIZE)) $TRIM_VDEV1

log_must zpool create -f $TESTPOOL $TRIM_VDEV1 cache $TRIM_VDEV2

export DIRECTORY=/$TESTPOOL
export NUMJOBS=1
export FILE_SIZE=$((3 * MINVDEVSIZE))
export PERF_RANDSEED=1234
export PERF_COMPPERCENT=66
export PERF_COMPCHUNK=0
export RUNTIME=5
export BLOCKSIZE=128K
export SYNC_TYPE=0
export DIRECT=0

log_must fio $FIO_SCRIPTS/mkfiles.fio

wait_feed_trims
wait_trimming_now
must_return zpool remove $TESTPOOL $TRIM_VDEV2
fast_trims
log_must zpool add -f $TESTPOOL cache $TRIM_VDEV2

wait_feed_trims
wait_trimming_now
must_return zpool export $TESTPOOL
fast_trims
log_must zpool import -d $TRIM_VDEV1 -d $TRIM_VDEV2 $TESTPOOL

wait_feed_trims
wait_trimming_now
must_return zpool destroy $TESTPOOL
log_must rm -f $VDEVS

log_pass "A cache device can be removed, exported and destroyed while" \
    "its feed thread trims."
