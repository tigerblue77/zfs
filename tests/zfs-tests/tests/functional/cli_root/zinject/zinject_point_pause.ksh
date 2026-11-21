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
#	'zinject -n POINT -A pause' blocks each thread that reaches the
#	point until the handler is cleared, and 'zinject -w' lets a test
#	wait until a thread is there.
#
# STRATEGY:
#	1. Create a pool on a file and export it.
#	2. Add a pause handler at spa_load:vdev-metadata and start an
#	   import in the background.
#	3. Wait for the import to reach the point (wait_inject_point), and
#	   check that it stays blocked there, counted once.
#	4. Clear the handler, and check that the import completes and the
#	   pool is usable.
#	5. Check that -A pause is rejected without -n, and with -e or -D.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
verify_inject_points

typeset VDEV=$TEST_BASE_DIR/zinject_point_pause.dat
typeset POOL=zinject_point_pause
typeset POINT=spa_load:vdev-metadata
typeset -i pid=0

function cleanup
{
	zinject -c all
	((pid != 0)) && wait $pid
	poolexists $POOL || zpool import -d $TEST_BASE_DIR $POOL
	destroy_pool $POOL
	rm -f $VDEV
}

log_onexit cleanup

log_assert "zinject -n -A pause holds a thread at an injection point"

log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV
log_must zpool export $POOL

typeset id=$(zinject -q -n $POINT -A pause $POOL)
log_must test -n "$id"
log_must test "$(inject_point_field $id ACTION)" = "pause"

zpool import -d $TEST_BASE_DIR $POOL &
pid=$!
wait_inject_point $id 1 60 || log_fail "the import did not reach $POINT"

#
# The import holds the namespace lock while it is paused, so don't run
# anything that needs it (such as zpool list) until it is released.
#
sleep 3
kill -0 $pid || log_fail "the import did not stay paused at $POINT"
log_must test "$(inject_point_field $id MATCH)" = "1"
log_must test "$(inject_point_field $id INJECT)" = "1"

log_must zinject -c $id
wait $pid
typeset -i status=$?
pid=0
((status == 0)) || log_fail "the import failed after the pause: $status"
log_must poolexists $POOL
log_must zpool sync $POOL

log_mustnot zinject -A pause $POOL
log_mustnot zinject -n $POINT -A pause -e io $POOL
log_mustnot zinject -n $POINT -A pause -D 10 $POOL
log_mustnot zinject -n $POINT -A bogus $POOL
[[ "$(zinject)" == "No handlers registered."* ]] ||
    log_fail "a rejected handler was registered"

log_pass "zinject -n -A pause holds a thread at an injection point"
