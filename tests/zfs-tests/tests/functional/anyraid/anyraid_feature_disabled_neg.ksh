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
# An AnyRAID vdev cannot be created or added without the anyraid feature.
#
# Creating an AnyRAID vdev counts against the anyraid feature.  Without the
# feature enabled, the count was taken anyway: on a pool without feature flags
# a debug build fails an assertion in feature_do_action(), and a pool with
# the feature disabled accepted the vdev anyway.
#
# STRATEGY:
# 1. zpool create -d with an anymirror1 vdev fails.
# 2. zpool create -o version=28 with an anymirror1 vdev fails.
# 3. zpool create -d -o feature@anyraid=enabled with an anymirror1 vdev
#    succeeds, and the feature is active.
# 4. On a pool created with -d and a plain vdev, zpool add of an anymirror1
#    vdev fails, and succeeds once the feature is enabled.
#

verify_runnable "global"

ORIG_TILE_SIZE=$(get_tunable ANYRAID_MIN_TILE_SIZE)
VDEV_DIR=$TEST_BASE_DIR/anyraid_feature_disabled

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	set_tunable64 ANYRAID_MIN_TILE_SIZE $ORIG_TILE_SIZE
	rm -rf $VDEV_DIR
}

log_onexit cleanup

log_assert "An AnyRAID vdev needs the anyraid feature"

log_must set_tunable64 ANYRAID_MIN_TILE_SIZE 67108864
log_must rm -rf $VDEV_DIR
log_must mkdir $VDEV_DIR
log_must truncate -s 1G $VDEV_DIR/any.{0,1} $VDEV_DIR/plain.0

log_mustnot zpool create -f -d $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1}
log_mustnot poolexists $TESTPOOL
log_mustnot zpool create -f -o version=28 $TESTPOOL \
    anymirror1 $VDEV_DIR/any.{0,1}
log_mustnot poolexists $TESTPOOL

log_must zpool create -f -d -o feature@anyraid=enabled $TESTPOOL \
    anymirror1 $VDEV_DIR/any.{0,1}
log_must test "$(get_pool_prop feature@anyraid $TESTPOOL)" = "active"
log_must zpool destroy $TESTPOOL

log_must zpool create -f -d $TESTPOOL $VDEV_DIR/plain.0
log_mustnot zpool add -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1}
log_must zpool set feature@anyraid=enabled $TESTPOOL
log_must zpool add -f $TESTPOOL anymirror1 $VDEV_DIR/any.{0,1}
log_must test "$(get_pool_prop feature@anyraid $TESTPOOL)" = "active"

log_pass "An AnyRAID vdev needs the anyraid feature"
