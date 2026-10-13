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
# zpool initialize reaches the end of every logical tile of an anyraidz vdev.
#
# The last metaslab of a logical tile ends where the tile ends.  The
# translation of such a range to a child used to stop at the start of the
# tile instead of its end, which made the range of every child end before it
# began, and initialize (like trim) silently skipped that metaslab.
#
# STRATEGY:
# 1. Create an anyraidz1:2 pool from three 1 GiB files with 64 MiB tiles.
# 2. Initialize the pool to completion with a known pattern and export it.
# 3. On every child, the last MiB of at least one tile slot must hold the
#    pattern.  A logical tile has three metaslabs of 64 MiB, and the last
#    one fills the last third of the slot of the tile on each child.  Only
#    the tiles that exist are initialized, and tile 0 always does, so not
#    every slot has to hold the pattern.
#

verify_runnable "global"

ORIG_PATTERN=$(get_tunable INITIALIZE_VALUE)
ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 INITIALIZE_VALUE $ORIG_PATTERN
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -f $TEST_BASE_DIR/vdev_file.{0,1,2}
}

log_onexit cleanup

log_assert "zpool initialize reaches the end of every anyraidz logical tile"

tile=$((64 * 1024 * 1024))
slots=11

#
# The tiles of a child start after the 4 MiB of front labels and the 2 first
# copies of the tile map (VDEV_ANYRAID_START_OFFSET), each made of an 8 KiB
# header and a 64 MiB map for ashift 13 and below.
#
first=$((4 * 1024 * 1024 + 2 * (8 * 1024 + 64 * 1024 * 1024)))

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE $tile
log_must set_tunable64 INITIALIZE_VALUE $(printf %llu 0xdeadbeefdeadbeef)

log_must truncate -s 1G $TEST_BASE_DIR/vdev_file.{0,1,2}
log_must zpool create -f -o ashift=12 $TESTPOOL anyraidz1:2 \
    $TEST_BASE_DIR/vdev_file.{0,1,2}
log_must zpool initialize -w $TESTPOOL
log_must zpool export $TESTPOOL

for file in $TEST_BASE_DIR/vdev_file.{0,1,2}; do
	hits=0
	for slot in $(seq 0 $((slots - 1))); do
		offset=$((first + (slot + 1) * tile - 1048576))
		if dd if=$file bs=4k skip=$((offset / 4096)) count=256 \
		    2>/dev/null | od -t x4 -Ad |
		    grep -qE "deadbeef +deadbeef +deadbeef +deadbeef"; then
			hits=$((hits + 1))
		fi
	done
	log_note "$file: $hits of $slots slot tails initialized"
	(( hits > 0 )) || log_fail "$file: no slot tail is initialized"
done

log_pass "zpool initialize reaches the end of every anyraidz logical tile"
