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
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	'zpool split' builds the config of the new pool from the config it
#	read before its ioctl.  A removal that completes in between turns a
#	mirror of that config into an indirect vdev: the split must then
#	fail with EINVAL rather than give the new pool a leaf where the
#	original pool now has that indirect vdev, and leave the original
#	pool as it was.
#
# STRATEGY:
#	1. Create a pool of two mirrors
#	2. Start split_on_signal, which opens the pool and waits
#	3. Remove the first mirror and wait for the removal to complete
#	4. Let split_on_signal split the pool from the config it read
#	   before the removal: the split must fail with EINVAL, and the
#	   pool must be unchanged
#	5. Split the pool again from its current config: it must succeed
#

verify_runnable "global"

function cleanup
{
	[[ -n $pid ]] && kill $pid 2>/dev/null
	destroy_pool $TESTPOOL2
	destroy_pool $TESTPOOL
	rm -rf $VDEV_DIR
	rm -fd $altroot
}

log_assert "'zpool split' fails with EINVAL on a config that predates" \
	"the removal of a mirror"
log_onexit cleanup

typeset VDEV_DIR="$TEST_BASE_DIR/zpool_split_indirect_stale"
typeset altroot="$TESTDIR/altroot-$TESTPOOL2"
typeset out="$VDEV_DIR/split_on_signal.out"
typeset pid=""
typeset -i rc tries=0

log_must mkdir -p $VDEV_DIR
for vdev in a1 a2 b1 b2; do
	log_must truncate -s $MINVDEVSIZE $VDEV_DIR/$vdev
done

# 1. mirror-0 is the one to remove
log_must zpool create -f $TESTPOOL mirror $VDEV_DIR/a1 $VDEV_DIR/a2 \
	mirror $VDEV_DIR/b1 $VDEV_DIR/b2

# 2. "ready" means that split_on_signal holds the config with mirror-0
split_on_signal $TESTPOOL $TESTPOOL2 > $out 2>&1 &
pid=$!
while ! grep -qsx ready $out; do
	if (( ++tries > 60 )) || ! kill -0 $pid; then
		log_fail "split_on_signal did not get ready: $(<$out)"
	fi
	sleep 1
done

# 3. mirror-0 becomes indirect-0
log_must zpool remove $TESTPOOL mirror-0
log_must wait_for_removal $TESTPOOL

# 4. The config held still has mirror-0, whose leaf the split takes
log_must kill -USR1 $pid
wait $pid
rc=$?
pid=""
log_note "split_on_signal exited with $rc: $(<$out)"
log_must test $rc -eq 1
log_must grep -q "invalid argument" $out
log_mustnot poolexists $TESTPOOL2
log_must check_state $TESTPOOL "" ONLINE
for vdev in b1 b2; do
	log_must check_state $TESTPOOL $VDEV_DIR/$vdev ONLINE
done

# 5. The current config has indirect-0, which the new pool takes as is
log_must zpool split -R $altroot $TESTPOOL $TESTPOOL2
log_must check_state $TESTPOOL2 "" ONLINE

log_pass "'zpool split' fails with EINVAL on a config that predates" \
	"the removal of a mirror"
