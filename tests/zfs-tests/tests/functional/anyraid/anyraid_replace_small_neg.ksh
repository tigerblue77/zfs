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
# A device that replaces a child of an AnyRAID vdev must have room for every
# tile the vdev counts on that child.
#
# A device attached to the AnyRAID vdev itself only needs room for the map
# and one tile, and a replacement was held to that size too.  The vdev kept
# the capacity of the child it replaced, and on the next import it found
# less room than it had recorded and refused to open, so the pool could not
# be imported.
#
# STRATEGY:
# 1. Create an anymirror1 vdev of three 1 GiB disks with 64 MiB tiles.
# 2. Replacing one of them with a 512 MiB disk, which has room for the map
#    and a few tiles but not for the dozen tiles of the disk, fails.
# 3. Replacing it with a 1 GiB disk succeeds.
# 4. The pool can be exported and imported again.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_replace_small

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A replacement for an AnyRAID child must hold all of its tiles"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/any.{0,1,2} $VDEV_DIR/same.0
log_must truncate -s 512M $VDEV_DIR/small.0

log_must zpool create -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1,2}

log_mustnot zpool replace -f $TESTPOOL $VDEV_DIR/any.1 $VDEV_DIR/small.0
log_must zpool replace -f $TESTPOOL $VDEV_DIR/any.1 $VDEV_DIR/same.0
log_must zpool wait -t replace $TESTPOOL

log_must zpool export $TESTPOOL
log_must zpool import -d $VDEV_DIR $TESTPOOL
log_must check_state $TESTPOOL "" "ONLINE"

log_pass "A replacement for an AnyRAID child must hold all of its tiles"
