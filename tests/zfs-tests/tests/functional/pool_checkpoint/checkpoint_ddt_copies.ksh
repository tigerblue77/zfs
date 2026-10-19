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

. $STF_SUITE/tests/functional/pool_checkpoint/pool_checkpoint.kshlib

#
# DESCRIPTION:
#	A dedup block written again with more copies after the pool
#	checkpoint was taken leaves the checkpoint consistent: no copy
#	written after the checkpoint is freed into it.
#
# STRATEGY:
#	1. In a dedup dataset with copies=1, write a file, then take a
#	   checkpoint.
#	2. Copy the file into a dedup dataset with copies=2, which needs one
#	   more copy of each block than its entry, born before the
#	   checkpoint, holds.
#	3. Remove both files and flush the dedup log, so that every block is
#	   freed.
#	4. Export the pool: zdb verifies the checkpoint without error.
#

verify_runnable "global"

function test_cleanup
{
	restore_tunable DEDUP_LOG_TXG_MAX
	cleanup_test_pool
}

log_assert "More copies of a dedup block after a checkpoint stay out of it"
log_onexit test_cleanup
save_tunable DEDUP_LOG_TXG_MAX

log_must zpool create -O sync=disabled -O compression=off -O dedup=on \
    -O recordsize=128K -o feature@block_cloning=disabled $TESTPOOL $TESTDISK
log_must zfs create -o copies=1 $FS0
log_must zfs create -o copies=2 $FS1
log_must set_tunable32 DEDUP_LOG_TXG_MAX 1

log_must dd if=/dev/urandom of=/$FS0/f bs=128K count=256
sync_pool $TESTPOOL
log_must zpool checkpoint $TESTPOOL

log_must dd if=/$FS0/f of=/$FS1/f bs=128K
sync_pool $TESTPOOL
log_must rm /$FS0/f /$FS1/f
for i in 1 2 3; do
	sync_pool $TESTPOOL
done

log_must zpool export $TESTPOOL
log_must zdb -e $TESTPOOL
log_must zpool import $TESTPOOL
log_must zpool checkpoint -d $TESTPOOL

log_pass "More copies of a dedup block after a checkpoint stay out of it"
