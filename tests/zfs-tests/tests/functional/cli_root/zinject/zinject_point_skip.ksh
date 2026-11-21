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
#	'zinject -n dsl_scan_sync:progress -A skip' keeps a scrub from
#	making progress, also across an export and import, until the
#	handler is cleared.  The point is in syncing context, so a pause
#	handler there does not act.
#
# STRATEGY:
#	1. Create a pool and write 64M.
#	2. Add a pause handler and then a skip handler at
#	   dsl_scan_sync:progress, and start a scrub.
#	3. Check that the scrub stays in progress without examining
#	   anything more, that the skip handler acts and the pause handler
#	   doesn't, and that the pool still syncs.
#	4. Export and import the pool, and check the same again.
#	5. Clear the handlers, and check that the scrub completes without
#	   errors.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
verify_inject_points

typeset VDEV=$TEST_BASE_DIR/zinject_point_skip.dat
typeset POOL=zinject_point_skip
typeset POINT=dsl_scan_sync:progress

function cleanup
{
	zinject -c all
	poolexists $POOL || zpool import -d $TEST_BASE_DIR $POOL
	destroy_pool $POOL
	rm -f $VDEV
}

function scan_stat # field
{
	zpool status -jp $POOL | jq -r ".pools[].scan_stats.$1"
}

# Check that the scrub stays where it is for 5 seconds.
function check_held
{
	typeset before=$(scan_stat examined)
	typeset -i skips=$(inject_point_field $skip INJECT)
	sleep 5
	log_must test "$(scan_stat state)" = "SCANNING"
	log_must test "$(scan_stat examined)" = "$before"
	log_note "examined $before; skipped $skips, then" \
	    "$(inject_point_field $skip INJECT) txgs"
	(($(inject_point_field $skip INJECT) > skips)) ||
	    log_fail "the skip handler did not act"
	log_must test "$(inject_point_field $pause INJECT)" = "0"
	log_must zpool sync $POOL
}

log_onexit cleanup

log_assert "zinject -n dsl_scan_sync:progress -A skip holds a scrub"

log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV
log_must zfs create -o compress=off $POOL/fs
log_must fill_fs /$POOL/fs 1 64 1048576 1 R
log_must zpool sync $POOL

typeset pause=$(zinject -q -n $POINT -A pause $POOL)
typeset skip=$(zinject -q -n $POINT -A skip $POOL)
log_must test -n "$pause" -a -n "$skip"
log_must test "$(inject_point_field $skip ACTION)" = "skip"

log_must zpool scrub $POOL
check_held
(($(inject_point_field $pause MATCH) > 0)) ||
    log_fail "the scan did not reach $POINT"

log_must zpool export $POOL
log_must zpool import -d $TEST_BASE_DIR $POOL
check_held

log_must zinject -c all
log_must zpool wait -t scrub $POOL
log_must test "$(scan_stat state)" = "FINISHED"
log_must check_pool_status $POOL "errors" "No known data errors"
log_must is_pool_scrubbed $POOL

log_mustnot zinject -A skip $POOL
log_mustnot zinject -n $POINT -A skip -A pause $POOL
log_mustnot zinject -n $POINT -A skip -e io $POOL

log_pass "zinject -n dsl_scan_sync:progress -A skip holds a scrub"
