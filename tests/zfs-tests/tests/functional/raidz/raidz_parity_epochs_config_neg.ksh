#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
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
# CDDL HEADER END
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	The parity-epoch table of a raidz vdev is stored by the setter, with
#	its copy in the vdev ZAP and the feature that keep it, and read back
#	from disk.  The config of a raidz vdev that is created, with the pool
#	or by vdev add, must not carry one: it would be used without either.
#
# STRATEGY:
#	1. Create a pool whose raidz vdev config carries a table: refused.
#	2. Create the same pool without the table.
#	3. Add a raidz vdev whose config carries a table: refused, and the
#	   pool keeps its one top-level vdev.
#

verify_runnable "global"

typeset -r BASEDIR=$TEST_BASE_DIR/raidz_pe_config.$$
typeset -r POOL=${TESTPOOL}_config
typeset -i WIDTH=5
typeset out

function cleanup
{
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "the config of a new raidz vdev cannot carry a parity-epoch table"

log_must mkdir -p $BASEDIR

out=$(raidz_epoch_test forge $BASEDIR $POOL $WIDTH 1 create) ||
    log_fail "forge create did not complete: $out"
log_note "$out"
[[ $out == *"forge create rc=22 "* ]] ||
    log_fail "a pool was created with a table in its raidz vdev config"

log_must raidz_epoch_test create $BASEDIR $POOL $WIDTH 1

out=$(raidz_epoch_test forge $BASEDIR $POOL $WIDTH 2 add) ||
    log_fail "forge add did not complete: $out"
log_note "$out"
[[ $out == *"forge add rc=22 top=1"* ]] ||
    log_fail "a raidz vdev with a table in its config was added"

log_pass "the config of a new raidz vdev cannot carry a parity-epoch table"
