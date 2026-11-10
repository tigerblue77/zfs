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
# A disk of an anyraid vdev can hold more than 2^16 tiles.
#
# STRATEGY:
# 1. Create an anymirror0 vdev of a 1 GiB disk and a 1040 GiB sparse disk,
#    with 16 MiB tiles: the large disk holds more than 66000 tiles.
# 2. Verify that the vdev uses more tiles than 2^16 per disk would allow.
# 3. Verify that the tile map is version 1.
# 4. Write some data, export and import, and verify that the size and the
#    tile map are unchanged and that a scrub finds no error.
#

verify_runnable "global"

cleanup() {
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -f $TEST_BASE_DIR/vdev_file.{0,1}
	set_tunable64 ANYRAID_MIN_TILE_SIZE 1073741824
}

#
# zdb prints the on-disk map header, and so its version, only from -vv on.
#
map_version() {
	zdb -vv --anyraid-map $TESTPOOL | awk '$1 == "version:" { print $2 }'
}

log_onexit cleanup

log_assert "A disk of an anyraid vdev can hold more than 2^16 tiles"

((tile = 16 * 1024 * 1024))
log_must truncate -s 1G $TEST_BASE_DIR/vdev_file.0
log_must truncate -s 1040G $TEST_BASE_DIR/vdev_file.1
log_must set_tunable64 ANYRAID_MIN_TILE_SIZE $tile

log_must zpool create -f $TESTPOOL anymirror0 $TEST_BASE_DIR/vdev_file.{0,1}

#
# The 1 GiB disk holds at most 64 tiles, so with at most 2^16 tiles per
# disk the vdev would have at most 65600.
#
cap=$(zpool get -Hp -o value size $TESTPOOL)
((tiles = cap / tile))
log_note "size $cap, $tiles tiles of 16 MiB"
(( tiles > 65600 )) || log_fail "the vdev uses only $tiles tiles"

ver=$(map_version)
[[ "$ver" == "1" ]] || log_fail "tile map version is '$ver', not 1"

log_must file_write -o create -f /$TESTPOOL/f1 -b 1048576 -c 256 -d R
log_must zpool export $TESTPOOL
log_must zpool import -d $TEST_BASE_DIR $TESTPOOL

cap2=$(zpool get -Hp -o value size $TESTPOOL)
(( cap2 == cap )) || log_fail "size changed on import: $cap2"
ver=$(map_version)
[[ "$ver" == "1" ]] || log_fail "tile map version is '$ver' after import"

log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "A disk of an anyraid vdev can hold more than 2^16 tiles"
