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
. $STF_SUITE/tests/functional/cli_root/zpool_split/zpool_split.cfg

#
# DESCRIPTION:
#	'zpool split' holds the new pool until it is done with it.  After
#	the split is committed, it drops the pool namespace lock and still
#	logs its history record in the new pool: an export or a destroy of
#	the new pool at that point must fail with EBUSY rather than free
#	the pool under it.
#
# STRATEGY:
#	1. Create a mirrored pool.
#	2. Split it, and hold the split at the
#	   spa_vdev_split_mirror:committed point, after the namespace lock
#	   is dropped and before the new pool is used again.
#	3. Meanwhile, exporting and destroying the new pool must fail.
#	4. Release the split, which must succeed and export the new pool.
#	5. Import the new pool: its history holds the record of the split.
#

verify_runnable "global"

function cleanup
{
	zinject -c all
	destroy_pool $TESTPOOL2
	destroy_pool $TESTPOOL
	rm -rf $VDEV_DIR
}

log_assert "'zpool split' holds the new pool until it is done with it"
log_onexit cleanup

typeset VDEV_DIR="$TEST_BASE_DIR/zpool_split_new_pool_busy"

log_must mkdir -p $VDEV_DIR
log_must truncate -s $SPA_MINDEVSIZE $VDEV_DIR/a $VDEV_DIR/b
log_must zpool create -f $TESTPOOL mirror $VDEV_DIR/a $VDEV_DIR/b

# 2. The handler is on the original pool: it holds no reference on the new one.
typeset id=$(zinject -q -w spa_vdev_split_mirror:committed -s 60 $TESTPOOL)
[[ -n $id ]] || log_fail "zinject -w failed"
zpool split $TESTPOOL $TESTPOOL2 &
typeset -i splitpid=$!
wait_inject_point $id

# 3. Without the hold of the split, the export frees the new pool.
log_must poolexists $TESTPOOL2
log_mustnot_expect busy zpool export $TESTPOOL2
log_mustnot_expect busy zpool destroy $TESTPOOL2
log_must poolexists $TESTPOOL2

# 4. Without -R, the split exports the new pool once it is done with it.
log_must zinject -c $id
log_must wait $splitpid
log_mustnot poolexists $TESTPOOL2

# 5. The split logged its record in the new pool before the export.
log_must zpool import -d $VDEV_DIR $TESTPOOL2
log_must eval "zpool history -i $TESTPOOL2 | \
    grep -q '] split from pool $TESTPOOL\$'"

log_pass "'zpool split' holds the new pool until it is done with it"
