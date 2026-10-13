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
#	The copy of a parity-epoch table in the vdev ZAP is the authoritative
#	one; the copy in the labels and the MOS config is what the MOS is
#	read with until the ZAP is reached.  A table that the config carries
#	and the ZAP does not was not stored by the setter: the import drops
#	it, says so, and the config is written back without it.
#
# STRATEGY:
#	1. Create a raidz1 pool and give its vdev a table in the config only.
#	2. Require zdb to open the pool and say that it drops the table.
#	3. Write a block in a read-write import, and require the labels and
#	   the MOS config written back to carry no table.
#

verify_runnable "global"

typeset -r BASEDIR=$TEST_BASE_DIR/raidz_pe_label.$$
typeset -r POOL=${TESTPOOL}_label
typeset -i WIDTH=5
typeset out

function cleanup
{
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "a parity-epoch table that only the config carries is dropped"

log_must mkdir -p $BASEDIR
log_must raidz_epoch_test create $BASEDIR $POOL $WIDTH 1
log_must raidz_epoch_test label $BASEDIR $POOL
log_must eval "zdb -l $BASEDIR/v0 > $BASEDIR/labels.out"
log_must grep -q 'raidz_parity_epochs\[' $BASEDIR/labels.out

log_must eval "zdb -e -p $BASEDIR -G -m $POOL > $BASEDIR/zdb.out 2>&1"
log_must grep -q 'no parity epoch table in the vdev ZAP, dropping' \
    $BASEDIR/zdb.out

out=$(raidz_epoch_test write $BASEDIR $POOL 8192) ||
    log_fail "write failed: $out"
log_note "$out"
log_must eval "zdb -l $BASEDIR/v0 > $BASEDIR/labels.out"
log_mustnot grep -q 'raidz_parity_epochs\[' $BASEDIR/labels.out
log_must eval "zdb -e -p $BASEDIR -C $POOL > $BASEDIR/zdb.out"
log_mustnot grep -q 'raidz_parity_epochs\[' $BASEDIR/zdb.out

log_pass "a parity-epoch table that only the config carries is dropped"
