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
. $STF_SUITE/tests/functional/anyraid/default.cfg

#
# DESCRIPTION:
# An AnyRAID pool whose ashift is larger than the ashift of its disks
# can be exported and imported again.
#
# STRATEGY:
# 1. Create an anymirror1 pool with ashift=12 on file vdevs, which have
#    512-byte sectors.
# 2. Write a file and export the pool.
# 3. Import the pool and verify that it is online and the file unchanged.
#

verify_runnable "global"

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	delete_sparse_files
}

log_onexit cleanup

log_assert "An AnyRAID pool with an ashift above its disks' can be imported."

create_sparse_files "disk" 3 $DEVSIZE
log_must zpool create -o ashift=12 $TESTPOOL anymirror1 $disks
log_must file_write -o create -f /$TESTPOOL/f1 -b 1048576 -c 64 -d R
typeset sum=$(xxh128digest /$TESTPOOL/f1)

log_must zpool export $TESTPOOL
log_must zpool import -d $TESTDIR/sparse_files $TESTPOOL
log_must check_state $TESTPOOL "" "ONLINE"
[[ "$(xxh128digest /$TESTPOOL/f1)" == "$sum" ]] || \
    log_fail "/$TESTPOOL/f1 changed across export and import"

log_pass "An AnyRAID pool with an ashift above its disks' can be imported."
