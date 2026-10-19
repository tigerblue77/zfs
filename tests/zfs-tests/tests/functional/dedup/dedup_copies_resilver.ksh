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
#	Copies of a dedup block written while one side of a mirror is
#	offline are resilvered when it comes back.
#
# STRATEGY:
#	1. On a mirror, write a file in a dedup dataset with copies=1.
#	2. Take one side offline and copy the file into a dedup dataset
#	   with copies=2, which needs one more copy of each block.
#	3. Bring the side back and wait for the resilver.
#	4. A scrub has nothing to repair.
#

verify_runnable "global"

VDEV1=$TEST_BASE_DIR/dedup-copies-resilver-1
VDEV2=$TEST_BASE_DIR/dedup-copies-resilver-2

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	rm -f $VDEV1 $VDEV2
	log_must restore_tunable DEDUP_LOG_TXG_MAX
}

log_assert "Dedup copies written while a mirror side is offline are resilvered"

# Flush the dedup log every txg, so that the entries are on disk.
log_must save_tunable DEDUP_LOG_TXG_MAX
log_must set_tunable32 DEDUP_LOG_TXG_MAX 1
log_onexit cleanup

log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2
log_must zpool create -f -o feature@block_cloning=disabled \
    -O dedup=on -O compression=off -O recordsize=128K \
    $TESTPOOL mirror $VDEV1 $VDEV2
log_must zfs create -o copies=1 $TESTPOOL/a
log_must zfs create -o copies=2 $TESTPOOL/b
mnt_a=$(get_prop mountpoint $TESTPOOL/a)
mnt_b=$(get_prop mountpoint $TESTPOOL/b)

log_must dd if=/dev/urandom of=$mnt_a/f bs=128K count=64
sync_pool $TESTPOOL

log_must zpool offline $TESTPOOL $VDEV2
log_must dd if=$mnt_a/f of=$mnt_b/f bs=128K
sync_pool $TESTPOOL
log_must zpool online $TESTPOOL $VDEV2
log_must zpool wait -t resilver $TESTPOOL

log_must zpool scrub -w $TESTPOOL
zpool status -v $TESTPOOL
log_must check_pool_status $TESTPOOL "scan" "repaired 0B"

log_pass "Dedup copies written while a mirror side is offline are resilvered"
