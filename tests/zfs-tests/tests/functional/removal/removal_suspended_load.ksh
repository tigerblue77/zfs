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
# Copyright (c) 2026 by the OpenZFS project.  All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	A device removal that completes while its pool is being loaded,
#	and whose last step waits for the pool to resume, does not leave
#	its hold in spa_minref: the pool can be exported afterwards.
#
# STRATEGY:
#	1. Run removal_suspended_load, a libzpool program, on two file
#	   vdevs in a directory of its own.  It imports a pool whose
#	   removal is in progress with a load delay, has the restarted
#	   removal complete while the load waits for the namespace lock,
#	   with the pool suspended at the removal's last step, then
#	   resumes the pool.
#	2. It must find the reference count of the idle pool equal to
#	   spa_minref, and the export must succeed.
#
# NOTES:
#	The kernel cannot be driven into this window: the removal holds
#	spa_namespace_lock between its two steps, and zinject cannot add
#	a handler while the pool is being loaded.  The code is the same
#	in libzpool, where the program can hold the config lock and mark
#	the pool suspended at the right time.
#

verify_runnable "global"

typeset DIR=$TEST_BASE_DIR/removal_suspended_load

function cleanup
{
	rm -rf $DIR
}

log_onexit cleanup

log_assert "A removal completed during a load leaves spa_minref right"

log_must rm -rf $DIR
log_must mkdir -p $DIR
log_must removal_suspended_load $DIR

log_pass "A removal completed during a load leaves spa_minref right"
