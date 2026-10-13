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
# A pool with an AnyRAID vdev and more than three top-level vdevs imports
# after a crash.
#
# An import only accepts an AnyRAID tile map written in the txg of the
# uberblock it loads, and the map is written along with the labels.  A txg
# that changes no config writes the labels of three top-level vdevs picked at
# random, so a crash after a txg that did not pick the AnyRAID vdev left a
# pool whose AnyRAID vdev could not be opened.
#
# STRATEGY:
# 1. Create a pool of five file vdevs and an anymirror0 vdev.
# 2. Ten times: write a file, sync the pool, freeze it so that nothing more
#    reaches the disks, export it and import it again.  Exporting a frozen
#    pool leaves on disk what a crash would.
# 3. Verify the checksum of every file and scrub.
#
# With six top-level vdevs, a txg skips the AnyRAID vdev half of the time, so
# ten rounds miss the defect about once in a thousand runs.
#

verify_runnable "global"

typeset -i TILE_SIZE=67108864
typeset -i ROUNDS=10
ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_tile_map_crash_import

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "A pool with an AnyRAID vdev and six top-level vdevs imports" \
    "after a crash"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE $TILE_SIZE
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 256M $VDEV_DIR/plain.{0,1,2,3,4}
log_must truncate -s 1G $VDEV_DIR/any.{0,1}
log_must create_pool $TESTPOOL $VDEV_DIR/plain.{0,1,2,3,4} \
    anymirror0 $VDEV_DIR/any.{0,1}

typeset -a sums
for i in $(seq 1 $ROUNDS); do
	log_must file_write -o create -f /$TESTPOOL/f$i -b 1048576 -c 16 -d R
	sums[$i]=$(xxh128digest /$TESTPOOL/f$i)
	[[ -n ${sums[$i]} ]] || log_fail "could not checksum /$TESTPOOL/f$i"
	log_must zpool sync $TESTPOOL
	log_must zpool freeze $TESTPOOL
	log_must zpool export $TESTPOOL
	log_must zpool import -d $VDEV_DIR $TESTPOOL
done

for i in $(seq 1 $ROUNDS); do
	[[ $(xxh128digest /$TESTPOOL/f$i) == "${sums[$i]}" ]] || \
	    log_fail "checksum of /$TESTPOOL/f$i changed"
done

log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL errors "No known data errors"

log_pass "A pool with an AnyRAID vdev and six top-level vdevs imports" \
    "after a crash"
