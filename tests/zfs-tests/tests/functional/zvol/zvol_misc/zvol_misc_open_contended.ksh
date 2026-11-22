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

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Opening a ZVOL from several processes while spa_namespace_lock is held
# must not leave a reference on the zfs module once the ZVOL is closed.
#
# On Linux 5.10 and older, __blkdev_get() restarts an open for which
# zvol_open() returns -ERESTARTSYS. If another process opens the device
# before the restarted open goes on, the restarted open keeps a reference
# on the disk and on the zfs module that is never dropped.
#
# STRATEGY:
# 1. Create a ZVOL, and a second pool whose writes are delayed
# 2. Record the zfs module's use count
# 3. Hold spa_namespace_lock with "zpool reguid" on the second pool
# 4. Open the ZVOL from four processes, and check that no open completes
#    while the lock is held
# 5. Check that every open succeeds once the lock is released
# 6. Verify that the module's use count is back to its value in step 2
#

verify_runnable "global"

if ! is_linux; then
	log_unsupported "The leak is in the Linux block layer"
fi

# Linux 5.11 reworked __blkdev_get(): every open holds its own references.
# Kernels that took the rework, such as RHEL 8.5 and later, pass either way.
if [[ $(linux_version) -ge $(linux_version "5.11") ]]; then
	log_unsupported "Only Linux 5.10 and older leak a restarted open"
fi

if [[ ! -r /sys/module/zfs/refcnt ]]; then
	log_unsupported "The zfs module's use count is not available"
fi

function cleanup
{
	wait
	log_must zinject -c all
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	datasetexists $VOL && destroy_dataset $VOL
	[[ -n $WORKDIR ]] && rm -rf $WORKDIR
	log_pos restore_tunable VOL_OPEN_TIMEOUT_MS
}

#
# The zfs module's use count. Commands that other processes run, such as
# those zed starts, hold the module while they run: take the lowest of a
# few reads.
#
function zfs_use_count
{
	typeset -i i n lowest=-1

	for ((i = 0; i < 4; i++)); do
		n=$(</sys/module/zfs/refcnt)
		((lowest < 0 || n < lowest)) && lowest=$n
		sleep 0.25
	done
	echo $lowest
}

function count_files
{
	find $WORKDIR -name "$1.*" | wc -l
}

log_assert "Opening a ZVOL while spa_namespace_lock is held leaks nothing"
log_onexit cleanup

typeset -i openers=4 i n started before after deadline
VOL=$TESTPOOL/openvol
DEV=$ZVOL_DEVDIR/$VOL
WORKDIR=$(mktemp -d $TEST_BASE_DIR/zvol_misc_open_contended.XXXXXX) || \
    log_fail "Failed to create a work directory"

log_must save_tunable VOL_OPEN_TIMEOUT_MS

# 1. Create a ZVOL, and a second pool whose writes are delayed
log_must zfs create -V 16M $VOL
block_device_wait $DEV
log_must truncate -s $MINVDEVSIZE $WORKDIR/vdev
log_must zpool create -f -O mountpoint=none $TESTPOOL1 $WORKDIR/vdev
log_must zpool sync $TESTPOOL1

# A first open that finds the lock held waits for it in zvol_open() for up
# to zvol_open_timeout_ms, then has the kernel restart the open. Wait for
# longer than the lock is held below. A module without the parameter has
# the kernel restart the open at once.
if tunable_exists VOL_OPEN_TIMEOUT_MS; then
	log_must set_tunable32 VOL_OPEN_TIMEOUT_MS 60000
fi

# 2. Record the zfs module's use count
log_must udevadm settle
before=$(zfs_use_count)
log_note "zfs module use count before the opens: $before"

# 3. Hold spa_namespace_lock with "zpool reguid" on the second pool. It
# holds the lock until the txg that changes the GUID is synced. With every
# write delayed by a second, that takes at least four seconds: the MOS,
# then the even labels, the uberblocks and the odd labels, one at a time.
log_must zinject -d $WORKDIR/vdev -D 1000:64 -T write $TESTPOOL1
(zpool reguid $TESTPOOL1; echo $? >$WORKDIR/reguid.rc) &
sleep 1

# 4. Open the ZVOL from four processes, and check that no open completes
# while the lock is held. Each keeps the ZVOL open for a while, so that
# the others find it open when the lock is released.
for ((i = 1; i <= openers; i++)); do
	(
		touch $WORKDIR/started.$i
		{ touch $WORKDIR/opened.$i; sleep 3; } 3<$DEV || \
		    touch $WORKDIR/failed.$i
		touch $WORKDIR/closed.$i
	) &
done
sleep 1
started=$(count_files started)
n=$(count_files opened)
[[ -e $WORKDIR/reguid.rc ]] && \
    log_fail "zpool reguid ended before the opens could be checked"
(( started == openers )) || \
    log_fail "Only $started of $openers openers started"
(( n == 0 )) || \
    log_fail "$n of $openers opens completed while the lock was held"

# 5. Check that every open succeeds once the lock is released
deadline=$(( $(date +%s) + 120 ))
while [[ ! -e $WORKDIR/reguid.rc ]] || \
    (( $(count_files closed) < openers )); do
	if (( $(date +%s) > deadline )); then
		for p in $(pgrep -x zpool); do
			log_note "$(ps -o stat=,wchan=,args= -p $p)"
			cat /proc/$p/stack
		done
		log_fail "zpool reguid or an opener still runs after 120s"
	fi
	sleep 1
done
wait
log_must test "$(<$WORKDIR/reguid.rc)" = "0"
n=$(count_files failed)
(( n == 0 )) || log_fail "$n of $openers opens failed"
n=$(count_files opened)
(( n == openers )) || log_fail "Only $n of $openers opens completed"

# 6. Verify that the module's use count is back to its value in step 2
deadline=$(( $(date +%s) + 30 ))
while :; do
	after=$(zfs_use_count)
	(( after <= before || $(date +%s) > deadline )) && break
	sleep 1
done
log_note "zfs module use count after the opens: $after"
(( after <= before )) || \
    log_fail "The zfs module's use count rose from $before to $after"

log_pass "Opening a ZVOL while spa_namespace_lock is held leaks nothing"
