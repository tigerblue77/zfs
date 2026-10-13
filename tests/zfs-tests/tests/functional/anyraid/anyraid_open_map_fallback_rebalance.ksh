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
# A pool with a rebalance in progress imports from the tile map of another
# disk when the map of the first one cannot be read.
#
# Opening an AnyRAID vdev tries the tile map of each child in turn.  It took
# the rebalance in progress from the header of the first child before reading
# that child's map, and kept it when the read failed, so the next child found
# a rebalance already registered and the open failed with EINVAL, whatever
# the other children held.
#
# STRATEGY:
# 1. Create an anymirror1 vdev, write to it, attach a larger disk and start
#    a rebalance that pauses after 16 MiB.
# 2. Export the pool, and overwrite the start of the tile map in every copy
#    on the first disk, leaving its headers alone.
# 3. Import the pool, let the rebalance and its scrub finish, and check the
#    pool, then export and import it again.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_open_map_fallback

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	restore_tunable ANYRAID_RELOCATE_MAX_BYTES_PAUSE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A rebalancing pool imports from the map of another disk"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must save_tunable ANYRAID_RELOCATE_MAX_BYTES_PAUSE
log_must set_tunable64 ANYRAID_RELOCATE_MAX_BYTES_PAUSE $((16 * 1024 * 1024))
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/any.{0,1,2}
log_must truncate -s 2G $VDEV_DIR/any.3

log_must zpool create -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1,2}
log_must file_write -o create -b 1048576 -c 512 -d R -f /$TESTPOOL/f1
log_must zpool attach $TESTPOOL anymirror1-0 $VDEV_DIR/any.3
log_must zpool rebalance $TESTPOOL anymirror1-0
log_must sleep 1
log_must eval "zpool status $TESTPOOL | grep -q 'rebalance.*in progress'"
log_must zpool export $TESTPOOL

#
# Each of the 4 copies of the map is an 8 KiB header followed by the map.
# Two copies follow the 4 MiB at the start of the disk, and two end where
# the 512 KiB of labels at its end begin.
#
typeset -i HDR=8192
typeset -i COPY=$((HDR + 64 * 1024 * 1024))
typeset -i PSIZE=$((1024 * 1024 * 1024))
for off in $((4194304)) $((4194304 + COPY)) \
    $((PSIZE - 524288 - 2 * COPY)) $((PSIZE - 524288 - COPY)); do
	log_must dd if=/dev/urandom of=$VDEV_DIR/any.0 bs=$HDR count=1 \
	    seek=$(((off + HDR) / HDR)) conv=notrunc
done

log_must zpool import -d $VDEV_DIR $TESTPOOL
log_must set_tunable64 ANYRAID_RELOCATE_MAX_BYTES_PAUSE 0
log_must zpool wait -t anyraid_relocate,scrub $TESTPOOL
log_must check_pool_status $TESTPOOL state ONLINE true

log_must zpool export $TESTPOOL
log_must zpool import -d $VDEV_DIR $TESTPOOL

log_pass "A rebalancing pool imports from the map of another disk"
