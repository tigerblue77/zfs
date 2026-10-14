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
# Copyright (c) 2025 Klara, Inc.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Anyraid rebalance works correctly when paused and resumed
#
# STRATEGY:
# 1. Create an anymirror1 vdev with several small disks
# 2. Fill the small disks
# 3. Attach a larger disk and count the tiles that are mapped
# 4. Rebalance the vdev
# 5. Pause the rebalance
# 6. Export and import the pool
# 7. Resume the rebalance
# 8. Verify that available space has increased after completion, unless the
#    attach already made all of it available
# 9. Verify that scrub found no errors
#
# Sizes, in tiles of 64 MiB:
# - A disk gives up a little over 260 MiB to its labels and the 4 copies of
#   the tile map, which leaves room for 8 tiles on a 775 MiB disk and for 12
#   on the 1088 MiB one. A tile takes a slot on 2 disks, so the 5 small disks
#   hold 20 tiles, and with the big one added no layout holds more than 26.
# - The size of the vdev is the number of tiles that are mapped, plus the tiles
#   that its free slots can still hold. When M tiles are mapped, the small
#   disks have 40 - 2 * M free slots. If there are at least 12, every free
#   slot of the big disk finds a partner and the size is the full 26 tiles.
#   Otherwise some slots of the big disk have none, and the size is only
#   M + 40 - 2 * M = 40 - M tiles until a rebalance moves tiles onto it.
# - M is not a constant of the test. Every allocator of the pool fills
#   metaslabs, and so maps tiles, of its own, and the pool has up to
#   spa_num_allocators of them, fewer on a machine with few CPUs. So the test
#   counts the tiles after the attach and holds the size to what that count
#   allows. After the rebalance the size is the full 26 tiles if the attach
#   already gave them, and larger than it was if it did not.
#

verify_runnable "global"

cleanup() {
	zpool destroy $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE 1073741824
	restore_tunable ANYRAID_RELOCATE_MAX_BYTES_PAUSE
	rm $TEST_BASE_DIR/vdev_file.{0,1,2,3,4,5}
}

log_onexit cleanup

typeset -i TILE=$((64 * 1024 * 1024))
typeset -i MAX_CAP=$((26 * TILE))

log_must truncate -s 775M $TEST_BASE_DIR/vdev_file.{0,1,2,3,4}
log_must truncate -s 1088M $TEST_BASE_DIR/vdev_file.5
set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
save_tunable ANYRAID_RELOCATE_MAX_BYTES_PAUSE
set_tunable64 ANYRAID_RELOCATE_MAX_BYTES_PAUSE $((16 * 1024 * 1024))

log_assert "Anyraid rebalance works correctly when paused and resumed"

log_must create_pool $TESTPOOL anymirror1 $TEST_BASE_DIR/vdev_file.{0,1,2,3,4}

log_must file_write -o create -b 1048576 -c 600 -d 'R' -f /$TESTPOOL/f1

cap=$(zpool get -Hp -o value size $TESTPOOL)
[[ "$cap" -eq $((20 * 64 * 1024 * 1024)) ]] || \
	log_fail "Incorrect space for anyraid vdev: $cap"

log_must zpool attach $TESTPOOL anymirror1-0 $TEST_BASE_DIR/vdev_file.5

#
# Count the tiles that are mapped now. Tiles are only ever added, so this is
# at least the number that was mapped when the size was worked out.
#
log_must zpool sync $TESTPOOL
typeset -i mapped=$(zdb --anyraid-map $TESTPOOL |
    awk '$1 == "tiles" { print $2; exit }')
(( mapped > 0 )) || log_fail "Could not count the mapped tiles"
typeset -i min_cap=$(((40 - mapped) * TILE))
(( min_cap > MAX_CAP )) && min_cap=$MAX_CAP

log_must file_write -o create -b 1048576 -c 240 -d 'R' -f /$TESTPOOL/f2


log_must zpool rebalance $TESTPOOL anymirror1-0
typeset -i cap=$(zpool get -Hp -o value size $TESTPOOL)
(( cap % TILE == 0 && cap >= min_cap && cap <= MAX_CAP )) || \
	log_fail "Incorrect space for anyraid vdev: $cap, not between" \
	    "$min_cap and $MAX_CAP with $mapped tiles mapped"

log_must sleep 1
log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL -d $TEST_BASE_DIR

set_tunable64 ANYRAID_RELOCATE_MAX_BYTES_PAUSE 0

log_must zpool wait -t anyraid_relocate,scrub $TESTPOOL
log_must zpool sync $TESTPOOL

typeset -i min_end=$cap
(( cap < MAX_CAP )) && (( min_end += TILE ))
typeset -i cap_end=$(zpool get -Hp -o value size $TESTPOOL)
(( cap_end >= min_end && cap_end <= MAX_CAP )) || \
	log_fail "Incorrect space for anyraid vdev: $cap_end, was $cap," \
	    "not between $min_end and $MAX_CAP"

log_must check_pool_status $TESTPOOL state ONLINE true
log_must is_pool_scrubbed $TESTPOOL true
cksum_count=$(zpool status -v $TESTPOOL | grep ONLINE | awk 'NF > 2 && $5 != 0' | wc -l)
[[ "$cksum_count" -eq 0 ]] || log_fail "checksum errors detected"

log_pass "Anyraid rebalance works correctly"
