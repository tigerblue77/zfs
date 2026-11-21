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

# DESCRIPTION:
# Verify the ZED mounts the datasets of a pool that was unavailable when it
# started, once that pool is available again, and keeps running.
#
# STRATEGY:
# 1. Create a pool with an unmounted dataset, and a mirror with
#    autoreplace=on, exported, then one of its files removed.
# 2. Make the first pool unavailable with an injected open error and a
#    reopen.
# 3. Start the ZED and wait for it to list that pool as unavailable.
# 4. Clear the injection and the pool, which comes back online.
# 5. Import the mirror: its missing leaf posts a vdev_check event, for
#    which the ZED visits the pools, the first pool included.
# 6. Verify the ZED mounts the dataset and is still running.

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/events/events_common.kshlib

verify_runnable "global"

# zpool_iter() visits the pools in name order and stops at the vdev of the
# event, so the late pool must sort before the trigger pool.
LATE_POOL=$TESTPOOL1
LATE_FS=$LATE_POOL/$TESTFS1
TRIGGER_POOL=$TESTPOOL2

# What the ZED logs about the pools it enumerates and visits
SLM_LINES='zfs_unavail_pool|zfs_slm_event|zfs_deliver_check|zfs_iter_pool'

zedpid=""

function cleanup
{
	zed_stop
	log_must zinject -c all
	if poolexists $LATE_POOL; then
		zpool clear $LATE_POOL
		destroy_pool $LATE_POOL
	fi
	poolexists $TRIGGER_POOL && destroy_pool $TRIGGER_POOL
	log_must rm -rf $TESTDIR1
	log_must rm -f $VDEV1 $VDEV2 $VDEV3
}

# The ZED is a child of this shell: a dead one can linger as a zombie.
function zed_alive
{
	typeset st=$(ps -o stat= -p $zedpid 2>/dev/null)

	[[ -n $st && $st != Z* ]]
}

function pool_health
{
	zpool list -Ho health $1 2>/dev/null
}

function late_fs_mounted
{
	zfs get -Hpo value mounted $LATE_FS 2>/dev/null
}

function fail_with_zed_log
{
	log_note "SLM lines of $ZED_LOG:"
	grep -E "$SLM_LINES" $ZED_LOG | tail -n 20
	log_note "Last lines of $ZED_LOG:"
	tail -n 20 $ZED_LOG
	log_note "Segfaults in dmesg:" \
	    "$(dmesg | grep -E 'segfault|traps:' | tail -n 3)"
	log_note "vdev_check events:" \
	    "$(zpool events -H | grep -c 'sysevent\.fs\.zfs\.vdev_check')"
	log_fail "$@"
}

log_assert "The ZED mounts the datasets of a pool unavailable at its start"
log_onexit cleanup

log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2 $VDEV3
log_must zpool create -o cachefile=none -m none $LATE_POOL $VDEV1
log_must zfs create -o mountpoint=$TESTDIR1 $LATE_FS
log_must zfs unmount $LATE_FS
log_must test "$(late_fs_mounted)" = "no"
log_must zpool create -o cachefile=none -o autoreplace=on -m none \
    $TRIGGER_POOL mirror $VDEV2 $VDEV3
log_must zpool export $TRIGGER_POOL
log_must rm -f $VDEV3

# The only leaf of the late pool fails to reopen: the pool stays imported,
# with its root vdev unavailable.
log_must zpool sync $LATE_POOL
log_must zinject -d $VDEV1 -e nxio $LATE_POOL
log_must zpool reopen $LATE_POOL
typeset -i i=0
while [[ $(pool_health $LATE_POOL) == @(ONLINE|DEGRADED) ]]; do
	(( i++ < 30 )) || log_fail "$LATE_POOL is still available"
	sleep 1
done
log_note "$LATE_POOL is $(pool_health $LATE_POOL)"

log_must zpool events -c
log_must zed_start

# At startup the ZED keeps the pools whose root vdev state is below
# DEGRADED (6), to enable their datasets once they are available.
pattern="zfs_unavail_pool: examining '$LATE_POOL'"
i=0
until grep -q "$pattern" $ZED_LOG 2>/dev/null; do
	(( i++ < 60 )) || fail_with_zed_log "the ZED did not list $LATE_POOL"
	sleep 1
done
# The children the ZED forks, if any, have higher pids.
zedpid=$(zed_check)
zedpid=${zedpid%% *}
zed_alive || fail_with_zed_log "the ZED is not running"
state=$(sed -n "s/.*$pattern (state \([0-9]*\)).*/\1/p" $ZED_LOG |
    head -n 1)
[[ $state == [0-5] ]] ||
    fail_with_zed_log "$LATE_POOL was available to the ZED (state $state)"

log_must zinject -c all
log_must zpool clear $LATE_POOL
i=0
until [[ $(pool_health $LATE_POOL) == @(ONLINE|DEGRADED) ]]; do
	(( i++ < 30 )) || fail_with_zed_log "$LATE_POOL did not come back"
	sleep 1
done
zed_alive || fail_with_zed_log "the ZED died before the import"
log_note "$LATE_FS mounted before the import: $(late_fs_mounted)"

# The import posts a vdev_check event for the missing leaf. The ZED visits
# the late pool for it, finds it back, and mounts its dataset: nothing else
# mounts it.
log_must zpool import -d $VDEV2 -o cachefile=none $TRIGGER_POOL
i=0
until [[ $(late_fs_mounted) == "yes" ]]; do
	zed_alive || fail_with_zed_log "the ZED died after the import"
	(( i++ < 120 )) || fail_with_zed_log "the ZED did not mount $LATE_FS"
	sleep 1
done
zed_alive || fail_with_zed_log "the ZED died after mounting $LATE_FS"
log_note "$(grep -E "$SLM_LINES" $ZED_LOG)"

log_pass "The ZED mounts the datasets of a pool unavailable at its start"
