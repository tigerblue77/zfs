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
# A checkpoint can be taken, and rewound to, while an AnyRAID vdev has no
# tile mapped yet.
#
# Taking the checkpoint recorded the last tile mapped on each AnyRAID vdev,
# and took it from an empty tile map without checking: a debug build failed
# an assertion, and a production build dereferenced NULL.
#
# STRATEGY:
# 1. Create a pool of a single disk with an anymirror1 log vdev, and turn
#    sync writes off so that the log vdev maps no tile.
# 2. Take a checkpoint.
# 3. Turn sync writes back on and write synchronously, so that the log vdev
#    maps a tile.
# 4. Rewind to the checkpoint, and check that the tile maps are what they
#    were when the checkpoint was taken.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_checkpoint_empty

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A checkpoint works while an AnyRAID vdev has no tile mapped"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/data.0 $VDEV_DIR/log.{0,1}

log_must zpool create -f -O sync=disabled $TESTPOOL $VDEV_DIR/data.0 \
    log anymirror1 $VDEV_DIR/log.{0,1}
log_must zpool sync $TESTPOOL
map=$(zdb --anyraid-map $TESTPOOL)
log_note "$map"

log_must zpool checkpoint $TESTPOOL

log_must zfs set sync=always $TESTPOOL
log_must file_write -o create -f /$TESTPOOL/f1 -b 131072 -c 64 -d R
log_must zpool sync $TESTPOOL
log_note "$(zdb --anyraid-map $TESTPOOL)"

log_must zpool export $TESTPOOL
log_must zpool import -d $VDEV_DIR --rewind-to-checkpoint $TESTPOOL
map2=$(zdb --anyraid-map $TESTPOOL)
log_must test "$map" == "$map2"

log_pass "A checkpoint works while an AnyRAID vdev has no tile mapped"
