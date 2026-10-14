#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#


#
# Copyright (c) 2026 by Tigerblue77.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Adding an AnyRAID vdev to a pool activates the anyraid feature.
#
# zpool add counted the new AnyRAID vdevs after moving them to the root
# vdev, so it found none and never activated the feature.
#
# STRATEGY:
# 1. Create a pool on a plain vdev, with the anyraid feature enabled.
# 2. zpool add an anymirror1 vdev; feature@anyraid is now active.
# 3. Export and import the pool; the feature is still active.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_add_feature_active

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "Adding an AnyRAID vdev activates the anyraid feature"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/plain.0 $VDEV_DIR/any.{0,1}

log_must zpool create -f $TESTPOOL $VDEV_DIR/plain.0
log_must test "$(get_pool_prop feature@anyraid $TESTPOOL)" = "enabled"

log_must zpool add -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1}
log_must test "$(get_pool_prop feature@anyraid $TESTPOOL)" = "active"

log_must zpool export $TESTPOOL
log_must zpool import -d $VDEV_DIR $TESTPOOL
log_must test "$(get_pool_prop feature@anyraid $TESTPOOL)" = "active"

log_pass "Adding an AnyRAID vdev activates the anyraid feature"
