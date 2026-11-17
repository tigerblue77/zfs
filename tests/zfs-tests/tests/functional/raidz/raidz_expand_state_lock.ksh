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
#	A RAID-Z expansion copies each chunk with its range locked against
#	other I/O and with SCL_STATE held.  A read of the chunk waits for
#	the range while its zio holds SCL_ZIO, and 'zpool clear' takes
#	SCL_STATE and then SCL_ZIO as writer.  The expansion must not wait
#	for SCL_STATE while it holds the range, or the three wait for each
#	other forever.
#
# STRATEGY:
#	1. Create a raidz1 pool whose data is never cached, write a file.
#	2. Hold the expansion thread once it has locked a chunk, and attach
#	   a disk.
#	3. Read the file; the read blocks on the chunk.
#	4. Run 'zpool clear', then release the expansion thread.
#	5. Verify that the read and 'zpool clear' return, that the expansion
#	   completes and that the data is intact.
#
#	Without the fix, the pool deadlocks in step 5: the test fails after
#	60 seconds and leaves the pool, whose commands no longer return.
#

verify_runnable "global"

typeset -r devs=3
typeset -r dev_size_mb=256
typeset -r data_mb=96

typeset -a disks
typeset deadlocked=false
typeset readpid clearpid sum hold

function cleanup
{
	log_must restore_tunable SCRUB_AFTER_EXPAND
	if $deadlocked; then
		log_note "the pool is deadlocked: leaving it"
		return
	fi
	zinject -c all >/dev/null 2>&1
	poolexists $TESTPOOL && log_must_busy zpool destroy $TESTPOOL

	for i in {1..$((devs + 1))}; do
		log_must rm -f $TEST_BASE_DIR/dev-$i
	done
}

function running # pid
{
	kill -0 $1 2>/dev/null
}

log_onexit cleanup

log_must save_tunable SCRUB_AFTER_EXPAND
log_must set_tunable32 SCRUB_AFTER_EXPAND 0

for i in {1..$((devs + 1))}; do
	disks[$i]=$TEST_BASE_DIR/dev-$i
	log_must truncate -s ${dev_size_mb}M ${disks[$i]}
done

# 1. Every read of the file goes to the disks.
log_must zpool create -f -o cachefile=none -O primarycache=metadata \
    -O atime=off $TESTPOOL raidz1 ${disks[1..$devs]}
log_must dd if=/dev/urandom of=/$TESTPOOL/data bs=1M count=$data_mb
log_must zpool sync $TESTPOOL
sum=$(xxh128digest /$TESTPOOL/data)

# 2. Hold the expansion thread with a chunk locked.
hold=$(zinject -q -w raidz_reflow_impl:locked -s 600 $TESTPOOL)
[[ $hold == +([0-9]) ]] || log_fail "zinject -w: '$hold'"
log_must zpool attach $TESTPOOL raidz1-0 ${disks[$((devs + 1))]}
wait_inject_point $hold 120

#
# 3. The whole file reads in well under a second; a read still running
# after 10 seconds waits for the locked chunk.
#
dd if=/$TESTPOOL/data of=/dev/null bs=128k 2>/dev/null &
readpid=$!
sleep 10
running $readpid || log_fail "the read did not wait for the locked chunk"

# 4. 'zpool clear' waits for a config lock, then the thread goes on.
zpool clear $TESTPOOL &
clearpid=$!
sleep 3
log_must zinject -c $hold

# 5.
typeset -i t0=$SECONDS
while running $readpid || running $clearpid; do
	if ((SECONDS - t0 > 60)); then
		deadlocked=true
		log_fail "the read or 'zpool clear' did not return in 60" \
		    "seconds once the expansion thread was released"
	fi
	sleep 1
done
log_must wait $clearpid
log_must wait $readpid

log_must zpool wait -t raidz_expand $TESTPOOL
[[ $(xxh128digest /$TESTPOOL/data) == "$sum" ]] || \
    log_fail "data changed by the expansion"
verify_pool $TESTPOOL

log_pass "the expansion takes SCL_STATE before the range lock of a chunk"
