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
# An AnyRAID vdev is not opened when fewer than nparity + 1 of its
# children can hold a tile, and a child that fails to open while the
# vdev is created does not stop the creation from failing cleanly.
#
# STRATEGY:
# 1. On Linux, put one of three files on a full tmpfs, so that the
#    probe of its device fails when it is opened, and verify that
#    creating an anymirror1 pool on them fails.
# 2. Create an anymirror1 pool of three files with 64 MiB tiles and
#    export it.
# 3. Shrink two of the files below the size of a single tile.
# 4. Verify that "zpool import -d" lists the pool as unavailable with
#    insufficient replicas, and that it cannot be imported.
#

verify_runnable "global"

DIR=$TEST_BASE_DIR/anyraid_open_without_tiles
MNT=$TEST_BASE_DIR/anyraid_open_without_tiles.mnt
LISTING=$TEST_BASE_DIR/anyraid_open_without_tiles.out
TMO=120

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	is_linux && umount $MNT > /dev/null 2>&1
	rm -rf $DIR $MNT $LISTING
	set_tunable64 ANYRAID_MIN_TILE_SIZE 1073741824
}

log_onexit cleanup

log_assert "AnyRAID vdevs without nparity + 1 children for a tile fail to open"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must mkdir -p $DIR $MNT

#
# The probe of a device rewrites a few blocks of its labels.  On a full
# tmpfs, the sparse file cannot store them, so the child fails to open
# after its size is known.
#
if is_linux; then
	log_must mount -t tmpfs -o size=16m tmpfs $MNT
	log_must truncate -s 768M $MNT/vdev.0 $DIR/vdev.{1,2}
	dd if=/dev/zero of=$MNT/fill bs=1M count=32 2>/dev/null
	log_mustnot timeout $TMO zpool create -f $TESTPOOL anymirror1 \
	    $MNT/vdev.0 $DIR/vdev.{1,2}
	log_mustnot poolexists $TESTPOOL
	log_must umount $MNT
	log_must rm -f $DIR/vdev.{1,2}
fi

log_must truncate -s 768M $DIR/vdev.{0,1,2}
log_must zpool create -f $TESTPOOL anymirror1 $DIR/vdev.{0,1,2}
log_must file_write -o create -f /$TESTPOOL/file.bin -b 1048576 -c 64 -d R
log_must zpool export $TESTPOOL

#
# Each child holds the 260 MiB of AnyRAID maps and seven 64 MiB tiles.
# At 300 MiB, a child still opens but has room for no tile, and only one
# child is left to hold the two copies of a tile.
#
log_must truncate -s 300M $DIR/vdev.{1,2}

log_must eval "timeout $TMO zpool import -d $DIR > $LISTING"
cat $LISTING
log_must grep -Eq "anymirror1-0 +UNAVAIL +insufficient replicas" $LISTING
log_mustnot timeout $TMO zpool import -d $DIR $TESTPOOL
log_mustnot poolexists $TESTPOOL

log_pass "AnyRAID vdevs without nparity + 1 children for a tile fail to open"
