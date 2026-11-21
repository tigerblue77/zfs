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
# DESCRIPTION:
#	The reflow of a raidz expansion passes injection point
#	raidz_expand:reflow before each copy, so 'zinject -n
#	raidz_expand:reflow -A pause' holds the expansion until the handler
#	is cleared.
#
# STRATEGY:
#	1. Create a raidz1 pool of four disks and write some data.
#	2. Add a pause handler at raidz_expand:reflow and attach a fifth
#	   disk.
#	3. Wait until the reflow reaches the point, and check over a few
#	   seconds that it copies nothing more and stays in progress.
#	4. Clear the handler, wait for the expansion and check the pool.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
verify_inject_points

typeset DIR=$TEST_BASE_DIR/zinject_point_reflow
typeset POOL=zinject_point_reflow
typeset POINT=raidz_expand:reflow

function cleanup
{
	zinject -c all
	destroy_pool $POOL
	rm -rf $DIR
}

function reflowed
{
	zpool status -jp $POOL | jq -r '.pools[].raidz_expand_stats.reflowed'
}

log_onexit cleanup

log_assert "zinject -n raidz_expand:reflow -A pause holds a raidz expansion"

log_must mkdir -p $DIR
log_must truncate -s 512M $DIR/dev-{0..4}
log_must zpool create -o cachefile=none $POOL raidz1 $DIR/dev-{0..3}
log_must zfs create -o compress=off -o primarycache=metadata $POOL/fs
log_must fill_fs /$POOL/fs 1 64 1048576 1 R
log_must zpool sync $POOL

typeset id=$(zinject -q -n $POINT -A pause $POOL)
log_must test -n "$id"
log_must zpool attach $POOL raidz1-0 $DIR/dev-4
wait_inject_point $id 1 60 || log_fail "the reflow did not reach $POINT"

typeset before=$(reflowed)
sleep 5
typeset after=$(reflowed)
log_note "reflowed $before bytes when held, $after bytes 5 s later"
log_must test "$before" = "$after"
log_must test "$(inject_point_field $id INJECT)" = "1"
log_must eval "zpool status $POOL | grep -q 'expansion of raidz1-0 in progress'"

log_must zinject -c $id
log_must zpool wait -t raidz_expand $POOL
log_must eval "zpool status $POOL | grep -q 'expanded raidz1-0'"
is_pool_scrubbing $POOL && wait_scrubbed $POOL
log_must zpool scrub -w $POOL
log_must check_pool_status $POOL "errors" "No known data errors"

log_pass "zinject -n raidz_expand:reflow -A pause holds a raidz expansion"
