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
# Copyright (c) 2026 Tigerblue77
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	A TX_SETSAXATTR record needs zilsaxattr, which zil_create()
#	activates on the dataset only when the record is about to be
#	written.  Until the record is written or its transaction group has
#	synced, zilsaxattr must not be disabled.
#
# STRATEGY:
#	1. Let transaction groups sync only when something waits for them,
#	   and create a file system with xattr=sa on a pool where zilsaxattr
#	   is enabled but not active.
#	2. Set an extended attribute, which logs a TX_SETSAXATTR record:
#	   disabling zilsaxattr must fail.
#	3. Once the pool has synced, the record is gone, and the disable
#	   must succeed.
#

verify_runnable "global"

VDEV=$TEST_BASE_DIR/feature_xattr.vdev

function cleanup
{
	destroy_pool $TESTPOOL1
	rm -f $VDEV
	log_must restore_tunable TXG_TIMEOUT
	log_must restore_tunable ZIL_SAXATTR
}

function check_prop # feature value
{
	typeset val=$(get_pool_prop feature@$1 $TESTPOOL1)

	[[ "$val" == "$2" ]] ||
	    log_fail "feature@$1 is '$val', expected '$2'"
}

log_assert "zilsaxattr cannot be disabled while a TX_SETSAXATTR record" \
    "waits to be written"
log_onexit cleanup

log_must save_tunable TXG_TIMEOUT
log_must set_tunable32 TXG_TIMEOUT 3600
log_must save_tunable ZIL_SAXATTR
log_must set_tunable32 ZIL_SAXATTR 1

# 1.
log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create -f $TESTPOOL1 $VDEV
log_must zfs create -o xattr=sa $TESTPOOL1/fs
mntpnt=$(get_prop mountpoint $TESTPOOL1/fs)
log_must touch $mntpnt/file
sync_pool $TESTPOOL1
check_prop zilsaxattr enabled

# 2.
log_must set_xattr test value $mntpnt/file
log_mustnot zpool set feature@zilsaxattr=disabled $TESTPOOL1
check_prop zilsaxattr enabled

# 3. The record is freed after its transaction group syncs.
sync_pool $TESTPOOL1
for i in $(seq 50); do
	zpool set feature@zilsaxattr=disabled $TESTPOOL1 && break
	sleep 0.2
done
check_prop zilsaxattr disabled

log_pass "zilsaxattr cannot be disabled while a TX_SETSAXATTR record" \
    "waits to be written"
