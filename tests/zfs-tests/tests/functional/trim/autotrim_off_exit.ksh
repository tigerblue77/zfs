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
. $STF_SUITE/tests/functional/trim/trim.cfg

#
# DESCRIPTION:
#	Setting autotrim=off stops the autotrim threads within a few txgs.
#	A thread sees the property change only once spa_sync() kicks it,
#	and spa_sync() used to kick only while autotrim was on, so the
#	thread kept waiting, with the metaslab it had just trimmed still
#	disabled, until autotrim was turned back on or the pool was
#	exported (#19757).  A thread re-enables its metaslab before it
#	exits, so checking that the threads exit covers both.
#
# STRATEGY:
#	1. Create a pool of two top-level file vdevs.
#	2. Set autotrim=on and wait for one more vdev_autotrim thread
#	   per top-level vdev.
#	3. Set autotrim=off, sync the pool, and verify that the threads
#	   exit within a bounded number of syncs.
#	4. Set autotrim=on again and verify that the threads start again.
#

verify_runnable "global"

# The check relies on the names of the kernel threads.
if ! is_linux; then
	log_unsupported "Linux only: counts the vdev_autotrim threads"
fi

log_assert "autotrim=off stops the autotrim threads"

function cleanup
{
	if poolexists $TESTPOOL; then
		destroy_pool $TESTPOOL
	fi

	log_must rm -f $TRIM_VDEV1 $TRIM_VDEV2
}
log_onexit cleanup

# Threads named after vdev_autotrim_thread(), "_thread" stripped by the SPL.
function autotrim_threads
{
	pgrep -x vdev_autotrim | wc -l
}

# Sync the pool until autotrim_threads returns $1, at most $2 times.
function wait_autotrim_threads # count max_syncs
{
	typeset -i want=$1
	typeset -i max=$2
	typeset -i i=0

	while (( $(autotrim_threads) != want )); do
		(( i++ < max )) || return 1
		sync_pool $TESTPOOL
		sleep 1
	done
	log_note "$want vdev_autotrim threads after $i syncs"
	return 0
}

# Threads of other pools, if any, are not counted against this one.
typeset -i base=$(autotrim_threads)

log_must truncate -s $MINVDEVSIZE $TRIM_VDEV1 $TRIM_VDEV2
log_must zpool create -f $TESTPOOL $TRIM_VDEV1 $TRIM_VDEV2

log_must zpool set autotrim=on $TESTPOOL
wait_autotrim_threads $((base + 2)) 10 || \
    log_fail "autotrim=on did not start 2 vdev_autotrim threads"

# Every txg kicks the threads, and each zpool sync syncs at least one.  A
# thread exits after at most 6 kicks: 5 after trimming a metaslab, then 1
# at the end of its pass.
log_must zpool set autotrim=off $TESTPOOL
wait_autotrim_threads $base 10 || \
    log_fail "$(( $(autotrim_threads) - base )) vdev_autotrim threads" \
    "still running after autotrim=off"

log_must zpool set autotrim=on $TESTPOOL
wait_autotrim_threads $((base + 2)) 10 || \
    log_fail "autotrim=on did not restart 2 vdev_autotrim threads"

log_pass "autotrim=off stops the autotrim threads"
