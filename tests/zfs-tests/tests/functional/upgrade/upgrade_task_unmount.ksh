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

. $STF_SUITE/tests/functional/upgrade/upgrade_common.kshlib

#
# DESCRIPTION:
#	A file system can be unmounted, and its dataset freed, while the
#	user accounting upgrade task that its mount started is returning.
#
# STRATEGY:
#	1. Hold the upgrade task at the dmu_objset_upgrade_task_cb:done
#	   point, where it is done with the dataset.
#	2. Mount the file system, which starts the task, and wait for the
#	   task to be held.
#	3. Unmount the file system, which frees the dataset, and give the
#	   asynchronous eviction of the dataset a second to run.  A hold
#	   that the task still counted there trips an assertion in debug
#	   builds.
#	4. Release the task, then mount, export and import the pool.
#

verify_runnable "global"

function cleanup
{
	zinject -c all
	cleanup_upgrade
}

log_assert "A dataset can be freed while its upgrade task returns."
log_onexit cleanup

log_must zpool create $TESTPOOL $TMPDEV
log_must zfs create $TESTPOOL/$TESTFS
log_must zfs unmount $TESTPOOL/$TESTFS

id=$(zinject -q -w dmu_objset_upgrade_task_cb:done -s 60 $TESTPOOL)
[[ -n $id ]] || log_fail "zinject -w failed"
log_must zfs mount $TESTPOOL/$TESTFS
wait_inject_point $id
log_must zfs unmount $TESTPOOL/$TESTFS
log_must sleep 1
log_must zinject -c $id

log_must zfs mount $TESTPOOL/$TESTFS
log_must zpool export $TESTPOOL
log_must zpool import -d $TEST_BASE_DIR $TESTPOOL

log_pass "A dataset can be freed while its upgrade task returns."
