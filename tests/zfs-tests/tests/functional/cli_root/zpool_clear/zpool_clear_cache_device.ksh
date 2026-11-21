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
. $STF_SUITE/tests/functional/cli_root/zpool_clear/zpool_clear.cfg

#
# DESCRIPTION:
#	'zpool clear <pool>', with no device named, clears a faulted cache
#	device, as 'zpool clear <pool> <device>' does.
#
# STRATEGY:
#	1. Create a pool with a cache device.
#	2. Fault the cache device with 'zpool offline -f', and verify that
#	   'zpool status -x' reports the pool.
#	3. Run 'zpool clear <pool>', and verify that the cache device is
#	   ONLINE and that 'zpool status -x' reports the pool healthy.
#	4. Repeat 2 and 3 with a temporary fault, 'zpool offline -tf'.
#

verify_runnable "global"

function cleanup
{
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -f $VDEV $CACHE
}

function check_cache_state # state
{
	typeset state=$(get_device_state $TESTPOOL1 $CACHE cache)

	[[ "$state" == "$1" ]] || \
	    log_fail "The cache device is '$state', expected '$1'."
}

function pool_is_healthy
{
	[[ "$(zpool status -x $TESTPOOL1)" == \
	    "pool '$TESTPOOL1' is healthy" ]]
}

log_assert "'zpool clear <pool>' clears a faulted cache device."
log_onexit cleanup

VDEV=$TEST_BASE_DIR/zpool_clear_cache_device.vdev
CACHE=$TEST_BASE_DIR/zpool_clear_cache_device.cache
log_must truncate -s $FILESIZE $VDEV $CACHE
log_must zpool create -f $TESTPOOL1 $VDEV cache $CACHE
check_cache_state ONLINE
log_must pool_is_healthy

for flags in -f -tf; do
	log_must zpool offline $flags $TESTPOOL1 $CACHE
	check_cache_state FAULTED
	log_mustnot pool_is_healthy

	log_must zpool clear $TESTPOOL1
	check_cache_state ONLINE
	log_must pool_is_healthy
done

log_pass "'zpool clear <pool>' clears a faulted cache device."
