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

#
# DESCRIPTION:
#	The scratch phase of a raidz expansion records its progress in two
#	uberblocks of its own: one before the start of the vdev is
#	overwritten with its reflowed contents, one after.  If no disk takes
#	either of them, the expansion must pause and retry once the error is
#	cleared, without a panic.
#
# STRATEGY:
#	For each of the two uberblocks:
#	1. Create a raidz1 pool and write some data.
#	2. Hold the sync thread at the zinject point just before that
#	   uberblock is written, and attach a disk.
#	3. Once the thread is held, inject errors on the uberblock area of
#	   the labels of every child, and release it.  No txg can write an
#	   uberblock while the sync thread is held, so the scratch uberblock
#	   is the first one written with the errors in place.
#	4. Verify that the uberblock write failed and that the expansion is
#	   still in progress.  With the errors still in place, the uberblock
#	   of the txg fails as well and the pool suspends.
#	5. Remove the injection, clear the pool, which resumes it, wait for
#	   the expansion to complete, and verify the data after an export and
#	   import.
#

verify_runnable "global"

typeset -r devs=4
typeset -r dev_size_mb=256
typeset -r data_mb=64

typeset -a disks

dbgmsg_enable=$(get_tunable DBGMSG_ENABLE)
scrub_after_expand=$(get_tunable SCRUB_AFTER_EXPAND)

function cleanup
{
	zinject -c all >/dev/null 2>&1
	if poolexists $TESTPOOL; then
		zpool status -v $TESTPOOL
		zpool clear $TESTPOOL >/dev/null 2>&1
		log_must_busy zpool destroy $TESTPOOL
	fi

	for i in {1..$((devs + 1))}; do
		log_must rm -f $TEST_BASE_DIR/dev-$i
	done

	log_must set_tunable32 SCRUB_AFTER_EXPAND $scrub_after_expand
	log_must set_tunable32 DBGMSG_ENABLE $dbgmsg_enable
}

function expanding # pool
{
	zpool status $1 | grep -q 'expansion of .* in progress'
}

#
# Count the lines of the debug log written at or after a time, in seconds
# since the epoch, that match a pattern.
#
function count_dbgmsg # since pattern
{
	kstat dbgmsg | awk -v since=$1 -v pattern="$2" \
	    '$1 >= since && $0 ~ pattern { n++ } END { print n + 0 }'
}

function wait_dbgmsg # since pattern timeout
{
	typeset -i t0=$SECONDS

	until (( $(count_dbgmsg $1 "$2") > 0 )); do
		if ((SECONDS - t0 > $3)); then
			log_fail "no '$2' in the debug log in $3 seconds"
		fi
		sleep 0.5
	done
}

function fail_uberblocks # pool
{
	for disk in "${disks[@]}"; do
		log_must zinject -d $disk -e io -L uber $1
	done
}

#
# $1: the reflow state that the uberblock records, $2: the zinject point
# just before it is written, $3: the state that the debug log shows once it
# is written.
#
function test_uberblock # state point logged
{
	typeset state=$1
	typeset point=raidz_reflow_scratch_sync:$2
	typeset written="reflow: uberblock updated [(]txg [0-9]+, $3,"
	typeset sum hold
	typeset -i since

	log_note "uberblock write failing for $state"
	log_must zpool create -f -o cachefile=none $TESTPOOL raidz1 \
	    ${disks[1..$devs]}
	log_must dd if=/dev/urandom of=/$TESTPOOL/data bs=1M count=$data_mb
	log_must zpool sync $TESTPOOL
	sum=$(xxh128digest /$TESTPOOL/data)

	since=$(date +%s)
	hold=$(zinject -q -w $point -s 600 $TESTPOOL)
	[[ $hold == +([0-9]) ]] || log_fail "zinject -w: '$hold'"
	log_must zpool attach $TESTPOOL raidz1-0 ${disks[$((devs + 1))]}
	wait_inject_point $hold 120
	fail_uberblocks $TESTPOOL
	(( $(count_dbgmsg $since "$written") == 0 )) || \
	    log_fail "uberblock written before the injection"
	log_must zinject -c $hold

	wait_dbgmsg $since \
	    "reflow: error [0-9]+ writing uberblock [(]${state}[)]" 60
	expanding $TESTPOOL || log_fail "expansion ended although the" \
	    "uberblock write failed"

	log_must zinject -c all
	log_must zpool clear $TESTPOOL
	typeset -i t0=$SECONDS
	while expanding $TESTPOOL; do
		if ((SECONDS - t0 > 120)); then
			log_fail "expansion did not complete in 120 seconds" \
			    "once the error was cleared"
		fi
		sleep 1
	done

	log_must zpool export $TESTPOOL
	log_must zpool import -d $TEST_BASE_DIR $TESTPOOL
	[[ $(xxh128digest /$TESTPOOL/data) == "$sum" ]] || \
	    log_fail "data changed by the expansion"
	verify_pool $TESTPOOL
	log_must check_pool_status $TESTPOOL "scan" "with 0 errors"
	log_must zpool destroy $TESTPOOL
}

log_onexit cleanup

log_must set_tunable32 DBGMSG_ENABLE 1
log_must set_tunable32 SCRUB_AFTER_EXPAND 0

for i in {1..$((devs + 1))}; do
	disks[$i]=$TEST_BASE_DIR/dev-$i
	log_must truncate -s ${dev_size_mb}M ${disks[$i]}
done

test_uberblock SCRATCH_VALID written SCRATCH_VALID
test_uberblock SCRATCH_INVALID_SYNCED reflowed SCRATCH_NOT_IN_USE

log_pass "a raidz expansion pauses when its scratch uberblock is not written"
