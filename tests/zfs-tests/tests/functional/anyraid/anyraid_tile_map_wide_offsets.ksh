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
# Tile slots above 255 on a disk survive an export and import.
#
# The on-disk tile map has a 16 bit field for the slot of a tile on its
# disk.  A writer that narrows the slot to 8 bits stores slot 256 as slot 0,
# and the next open of the pool then places two tiles in the same slot of
# that disk: a debug build fails an assertion while it rebuilds the free
# list, a release build silently reads the wrong data.
#
# STRATEGY:
# 1. Create an anymirror0 pool from a big and a small file, with the
#    smallest tile size, so that the big disk has more than 256 tiles.
# 2. Write enough data to use slots above 255 on the big disk.
# 3. Export the pool and dump its tile map with zdb.  Check that the map
#    holds slots above 255 and that no slot is used twice on a disk.
# 4. Import the pool, verify the checksum of the file and scrub.
#
# Sizes:
# - A mirror tile has to hold a maximum sized block, 16 MiB, so 16 MiB is
#   the smallest tile.  The tile size is the larger of the tunable and 1/64
#   of the smallest disk, so the small disk must stay under 1 GiB.
# - Each disk gives up 4 copies of the tile map, 4 * (8 KiB + 64 MiB), and
#   4.5 MiB of labels, and the rest is cut into tiles: 300 MiB gives 2 tiles
#   and 4800 MiB gives 283 tiles.  Disks of equal size cannot get past 64
#   tiles, hence the small one.
# - Slot 256 is the 257th tile of the big disk, so more than 256 * 16 MiB
#   must be allocated.  Writing 4160 MiB (260 tiles) is enough even if the
#   allocator fills one tile at a time, and fits in the 4544 MiB of the pool.
#   The test needs about 4.3 GiB of space in $TEST_BASE_DIR.
#

verify_runnable "global"

typeset -i TILE_SIZE=16777216
typeset -i WRITE_MB=4160
ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
TILEMAP=$TEST_BASE_DIR/anyraid_tile_map.out

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -f $TEST_BASE_DIR/vdev_file.{0,1} $TILEMAP
}

log_onexit cleanup

log_assert "Tile slots above 255 survive an export and import"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE $TILE_SIZE
log_must truncate -s 4800M $TEST_BASE_DIR/vdev_file.0
log_must truncate -s 300M $TEST_BASE_DIR/vdev_file.1
log_must create_pool $TESTPOOL anymirror0 $TEST_BASE_DIR/vdev_file.{0,1}

log_must file_write -o create -f /$TESTPOOL/f1 -b 1048576 -c $WRITE_MB -d R
sum=$(xxh128digest /$TESTPOOL/f1)
[[ -n $sum ]] || log_fail "could not checksum /$TESTPOOL/f1"

log_must zpool export $TESTPOOL

#
# A debug build aborts here, before the module has a chance to trip over the
# same map.  The lines that matter look like:
#	tile           256   offset  0100   disk       00
#
log_must eval "zdb -e -p $TEST_BASE_DIR --anyraid-map $TESTPOOL > $TILEMAP"
set -- $(awk '
    $1 == "tile" && $3 == "offset" {
	n++
	if (substr($4, 1, 2) != "00")
		wide++
	key = $6 ":" $4
	if (key in seen)
		dup++
	seen[key] = 1
    }
    END { print n + 0, wide + 0, dup + 0 }' $TILEMAP)
log_note "tile map: $1 tiles, $2 with a slot above 255, $3 slots used twice"

(( $1 > 256 )) || log_fail "only $1 tiles were allocated, need more than 256"
(( $2 > 0 )) || log_fail "no slot above 255 in the tile map"
(( $3 == 0 )) || log_fail "$3 slots are used by two tiles of the same disk"

log_must zpool import -d $TEST_BASE_DIR $TESTPOOL
newsum=$(xxh128digest /$TESTPOOL/f1)
[[ $sum == "$newsum" ]] || log_fail "checksum of /$TESTPOOL/f1 changed"

log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL errors "No known data errors"

log_pass "Tile slots above 255 survive an export and import"
