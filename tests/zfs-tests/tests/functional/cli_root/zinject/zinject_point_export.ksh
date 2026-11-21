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
#	Injection point handlers ('zinject -n') are matched by pool name:
#	they can be added before the pool exists, and stay in place while
#	it is exported, imported and destroyed, until they are cleared.
#
# STRATEGY:
#	1. Add a delay handler at spa_load:vdev-metadata for a pool that
#	   doesn't exist yet, then create the pool.
#	2. Export and import it twice, and check that the handler acts on
#	   each import.
#	3. Destroy the pool, check that the handler is still listed, and
#	   that it acts on the import of a new pool of the same name.
#	4. Clear it, and check that the next import is not counted.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
verify_inject_points

typeset VDEV=$TEST_BASE_DIR/zinject_point_export.dat
typeset POOL=zinject_point_export
typeset POINT=spa_load:vdev-metadata

function cleanup
{
	zinject -c all
	poolexists $POOL || zpool import -d $TEST_BASE_DIR $POOL
	destroy_pool $POOL
	rm -f $VDEV
}

function check_counts # match inject
{
	log_must test "$(inject_point_field $id MATCH)" = "$1"
	log_must test "$(inject_point_field $id INJECT)" = "$2"
}

log_onexit cleanup

log_assert "zinject -n handlers outlive export, import and destroy"

log_mustnot poolexists $POOL
typeset id=$(zinject -q -n $POINT -D 10 $POOL)
log_must test -n "$id"
check_counts 0 0

log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV

for n in 1 2; do
	log_must zpool export $POOL
	log_must test "$(inject_point_field $id POINT)" = "$POINT"
	log_must zpool import -d $TEST_BASE_DIR $POOL
	check_counts $n $n
done

log_must zpool destroy $POOL
log_must test "$(inject_point_field $id POINT)" = "$POINT"
log_must rm -f $VDEV
log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV
log_must zpool export $POOL
log_must zpool import -d $TEST_BASE_DIR $POOL
check_counts 3 3

log_must zinject -c $id
log_must zpool export $POOL
log_must zpool import -d $TEST_BASE_DIR $POOL
log_must test -z "$(inject_point_field $id POINT)"

log_pass "zinject -n handlers outlive export, import and destroy"
