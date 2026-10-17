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
# Copyright (c) 2026 Tigerblue77
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	"zpool set feature@<name>=disabled" moves an enabled feature that
#	allows it back to the disabled state, and refuses it while the feature
#	is active, while an enabled feature depends on it, or for a feature
#	that cannot be disabled.
#
# STRATEGY:
#	1. Disable multi_vdev_crash_dump, check it reads "disabled" across an
#	   export and import and in the history, then enable it again.
#	2. Disabling zstd_compress fails.
#	3. Disabling draid fails while draid_failure_domains is enabled.
#	   Disable both: adding a draid vdev must then fail, and succeed once
#	   they are enabled again.
#	4. Disable raidz_expansion on a raidz pool: "zpool attach" to the raidz
#	   must then fail.  Enable it again, expand the raidz, and check that
#	   disabling the now active feature fails.
#

verify_runnable "global"

function cleanup
{
	destroy_pool $TESTPOOL1
	rm -f $TEST_BASE_DIR/feature_disable.*
}

function check_prop # feature value
{
	typeset val=$(get_pool_prop feature@$1 $TESTPOOL1)

	[[ "$val" == "$2" ]] ||
	    log_fail "feature@$1 is '$val', expected '$2'"
}

log_assert "'zpool set feature@<name>=disabled' disables an unused feature"
log_onexit cleanup

for i in 0 1 2 3 4; do
	log_must truncate -s $MINVDEVSIZE $TEST_BASE_DIR/feature_disable.$i
done

# 1. A feature that nothing activates.
log_must zpool create -f $TESTPOOL1 $TEST_BASE_DIR/feature_disable.0
check_prop multi_vdev_crash_dump enabled
log_must zpool set feature@multi_vdev_crash_dump=disabled $TESTPOOL1
check_prop multi_vdev_crash_dump disabled
log_must eval "zpool history $TESTPOOL1 | \
    grep -q 'feature@multi_vdev_crash_dump=disabled'"
log_must zpool export $TESTPOOL1
log_must zpool import -d $TEST_BASE_DIR $TESTPOOL1
check_prop multi_vdev_crash_dump disabled
log_must zpool set feature@multi_vdev_crash_dump=enabled $TESTPOOL1
check_prop multi_vdev_crash_dump enabled

# 2. A feature that cannot be disabled.
log_mustnot zpool set feature@zstd_compress=disabled $TESTPOOL1
check_prop zstd_compress enabled

# 3. draid and draid_failure_domains, which depends on it.
log_mustnot zpool set feature@draid=disabled $TESTPOOL1
check_prop draid enabled
log_must zpool set feature@draid_failure_domains=disabled $TESTPOOL1
log_must zpool set feature@draid=disabled $TESTPOOL1
check_prop draid disabled
check_prop draid_failure_domains disabled
log_mustnot zpool add -f $TESTPOOL1 draid1:2d:1s \
    $TEST_BASE_DIR/feature_disable.{1,2,3,4}
log_must zpool set feature@draid_failure_domains=enabled $TESTPOOL1
check_prop draid enabled
log_must zpool add -f $TESTPOOL1 draid1:2d:1s \
    $TEST_BASE_DIR/feature_disable.{1,2,3,4}
check_prop draid active
log_mustnot zpool set feature@draid=disabled $TESTPOOL1
log_must destroy_pool $TESTPOOL1

# 4. raidz_expansion.
log_must zpool create -f $TESTPOOL1 raidz1 \
    $TEST_BASE_DIR/feature_disable.{0,1,2}
log_must zpool set feature@raidz_expansion=disabled $TESTPOOL1
check_prop raidz_expansion disabled
log_mustnot zpool attach $TESTPOOL1 raidz1-0 $TEST_BASE_DIR/feature_disable.3
log_must zpool set feature@raidz_expansion=enabled $TESTPOOL1
log_must zpool attach -w $TESTPOOL1 raidz1-0 $TEST_BASE_DIR/feature_disable.3
check_prop raidz_expansion active
log_mustnot zpool set feature@raidz_expansion=disabled $TESTPOOL1
check_prop raidz_expansion active

log_pass "'zpool set feature@<name>=disabled' disables an unused feature"
