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
#	A pause at an injection point ends when the thread is told to stop:
#	a raidz expansion held by 'zinject -n raidz_expand:reflow -A pause'
#	lets the pool be exported, copies nothing more for the export, and
#	is held again at the same place after the import.
#
# STRATEGY:
#	1. Create a raidz1 pool of four disks and write some data.
#	2. Add a pause handler at raidz_expand:reflow, attach a fifth disk
#	   and wait until the reflow is held.
#	3. Sync, note the reflow offset in the uberblock, and export.  Check
#	   that the export completes and the exported pool's reflow offset
#	   is the same.
#	4. Import the pool, and check that the reflow is held again and
#	   copies nothing.
#	5. Clear the handler, wait for the expansion and check the pool.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset DIR=$TEST_BASE_DIR/zinject_point_stop
typeset POOL=zinject_point_stop
typeset POINT=raidz_expand:reflow

function cleanup
{
	zinject -c all
	poolexists $POOL || zpool import -d $DIR $POOL
	destroy_pool $POOL
	rm -rf $DIR
}

# The reflow offset in the uberblock; zdb_args select the pool.
function reflow_offset # zdb_args...
{
	zdb "$@" -u $POOL | awk '/raidz_reflow/ {
	    for (i = 1; i <= NF; i++)
		if (sub("^off=", "", $i)) print $i }'
}

function reflowed
{
	zpool status -jp $POOL | jq -r '.pools[].raidz_expand_stats.reflowed'
}

log_onexit cleanup

log_assert "A paused raidz reflow lets an export through and is held again"

log_must mkdir -p $DIR
log_must truncate -s 512M $DIR/dev-{0..4}
log_must zpool create $POOL raidz1 $DIR/dev-{0..3}
log_must zfs create -o compress=off -o primarycache=metadata $POOL/fs
log_must fill_fs /$POOL/fs 1 64 1048576 1 R
log_must zpool sync $POOL

typeset id=$(zinject -q -n $POINT -A pause $POOL)
log_must test -n "$id"
log_must zpool attach $POOL raidz1-0 $DIR/dev-4
wait_inject_point $id 1 60 || log_fail "the reflow did not reach $POINT"
log_must zpool sync $POOL
typeset held=$(reflow_offset)
log_note "reflow held at offset $held"
[[ $held == +([0-9]) ]] || log_fail "no reflow offset: '$held'"

# The export must not wait for the handler to be cleared.
zpool export $POOL &
typeset -i pid=$!
typeset -i i
for ((i = 0; i < 60; i++)); do
	kill -0 $pid 2>/dev/null || break
	sleep 1
done
kill -0 $pid 2>/dev/null && log_fail "the export is waiting for the pause"
wait $pid || log_fail "the export failed"

typeset exported=$(reflow_offset -e -p $DIR)
log_note "exported pool's reflow offset $exported"
log_must test "$exported" = "$held"
log_must test "$(inject_point_field $id POINT)" = "$POINT"

log_must zpool import -d $DIR $POOL
wait_inject_point $id 2 60 || log_fail "the reflow was not held again"
typeset before=$(reflowed)
sleep 5
log_must test "$(reflowed)" = "$before"
log_must zpool sync $POOL
log_must test "$(reflow_offset)" = "$held"

log_must zinject -c $id
log_must zpool wait -t raidz_expand $POOL
log_must eval "zpool status $POOL | grep -q 'expanded raidz1-0'"
is_pool_scrubbing $POOL && wait_scrubbed $POOL
log_must zpool scrub -w $POOL
log_must check_pool_status $POOL "errors" "No known data errors"

log_pass "A paused raidz reflow lets an export through and is held again"
