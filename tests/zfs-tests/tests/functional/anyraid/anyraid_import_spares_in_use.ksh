#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#


#
# Copyright (c) 2026 by Tigerblue77.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# A pool keeps working, and imports, while every child of its AnyRAID vdev is
# a spare vdev.
#
# The tile map was written only to the leaves directly under the AnyRAID
# vdev.  A child being replaced, or covered by a hot spare, is a replacing or
# spare vdev, and none of its leaves got the map any more.  When every child
# was in that state, no leaf got the map, the sync of the txg failed for want
# of a single good write, and the pool was suspended at the second replace.
#
# STRATEGY:
# 1. Create an anymirror1 vdev of two disks with two hot spares, and write.
# 2. Replace both disks with the spares, wait for the resilver, and write.
# 3. Export and import the pool, and check the data and the pool.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_import_spares

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A pool works and imports while every AnyRAID child is a spare vdev"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/any.{0,1} $VDEV_DIR/spare.{0,1}

log_must zpool create -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1} \
    spare $VDEV_DIR/spare.{0,1}
log_must file_write -o create -b 1048576 -c 64 -d 'R' -f /$TESTPOOL/f1
log_must zpool sync $TESTPOOL

log_must zpool replace $TESTPOOL $VDEV_DIR/any.0 $VDEV_DIR/spare.0
log_must zpool replace $TESTPOOL $VDEV_DIR/any.1 $VDEV_DIR/spare.1
log_must zpool wait -t resilver $TESTPOOL
log_must file_write -o create -b 1048576 -c 64 -d 'R' -f /$TESTPOOL/f2
log_must zpool sync $TESTPOOL
typeset sum1=$(xxh128digest /$TESTPOOL/f1)
typeset sum2=$(xxh128digest /$TESTPOOL/f2)

log_must zpool export $TESTPOOL
log_must zpool import -d $VDEV_DIR $TESTPOOL

log_must test "$(xxh128digest /$TESTPOOL/f1)" = "$sum1"
log_must test "$(xxh128digest /$TESTPOOL/f2)" = "$sum2"
log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL state ONLINE true
log_must is_pool_scrubbed $TESTPOOL true

log_pass "A pool works and imports while every AnyRAID child is a spare vdev"
