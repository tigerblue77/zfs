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
#	A vdev property call that sets several properties applies none of
#	them when one is refused, in memory as on disk.
#
# STRATEGY:
#	1. Create a pool of two file vdevs.
#	2. In one call, set the alloc_bias of the first to special and its
#	   path to an invalid value: the call fails, and alloc_bias, which
#	   zpool get reads from memory, is still none.
#	3. Set alloc_bias to special, then in one call set it back to none
#	   along with the invalid path: it is still special.
#	4. In one call, set checksum_n and io_n: both are set.
#

verify_runnable "global"

function cleanup
{
	destroy_pool $TESTPOOL1
	rm -f $FILEDEV0 $FILEDEV1
}

function check_bias # expected
{
	typeset bias=$(zpool get -H -o value alloc_bias $TESTPOOL1 $FILEDEV0)

	[[ "$bias" == "$1" ]] || log_fail "alloc_bias is '$bias', expected '$1'"
}

log_assert "A vdev property call with a refused property applies none"
log_onexit cleanup

FILEDEV0="$TEST_BASE_DIR/filedev0.$$"
FILEDEV1="$TEST_BASE_DIR/filedev1.$$"
log_must truncate -s $MINVDEVSIZE $FILEDEV0 $FILEDEV1
log_must zpool create -f $TESTPOOL1 $FILEDEV0 $FILEDEV1
guid=$(zpool get -H -o value guid $TESTPOOL1 $FILEDEV0)

# alloc_bias 0 is none and 2 is special; a path must be under /dev.
log_mustnot vdev_set_props $TESTPOOL1 $guid alloc_bias=2 path=/invalid
check_bias none

log_must zpool set alloc_bias=special $TESTPOOL1 $FILEDEV0
log_mustnot vdev_set_props $TESTPOOL1 $guid alloc_bias=0 path=/invalid
check_bias special

log_must vdev_set_props $TESTPOOL1 $guid checksum_n=7 io_n=9
log_must test "$(zpool get -H -o value checksum_n $TESTPOOL1 $FILEDEV0)" = 7
log_must test "$(zpool get -H -o value io_n $TESTPOOL1 $FILEDEV0)" = 9

log_pass "A vdev property call with a refused property applies none"
