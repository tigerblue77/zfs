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
#	A rollback that fails must not discard data that was written to a
#	file through a shared mapping but not written back yet.
#
# STRATEGY:
#	1. Create a file and take two snapshots of the filesystem.
#	2. Write to the file through a shared mapping and, before that is
#	   written back, roll back to the older snapshot, which fails after
#	   suspending the filesystem as the newer snapshot exists.
#	3. Verify that the mapping still holds the written data, and that
#	   after msync() it reads back from the file and from a snapshot,
#	   which does not share cached pages with the file.
#

verify_runnable "global"

typeset fs=$TESTPOOL/$TESTFS
typeset snap=$fs@mmap_failed_rollback
typeset file=$TESTDIR/mmap_failed_rollback

function cleanup
{
	datasetexists ${snap}_3 && destroy_dataset ${snap}_3
	datasetexists ${snap}_2 && destroy_dataset ${snap}_2
	datasetexists ${snap}_1 && destroy_dataset ${snap}_1
	rm -f $file
}

log_assert "A failed rollback must not discard data written through mmap()"
log_onexit cleanup

log_must dd if=/dev/urandom of=$file bs=128k count=1
log_must zfs snapshot ${snap}_1
log_must zfs snapshot ${snap}_2
log_must mmap_failed_rollback $fs ${snap}_1 $file

log_must zfs snapshot ${snap}_3
typeset -i pagesize=$(getconf PAGESIZE)
typeset bytes=$(od -An -v -tx1 -N $pagesize \
    $TESTDIR/.zfs/snapshot/mmap_failed_rollback_3/mmap_failed_rollback |
    awk '{ for (i = 1; i <= NF; i++) { n++; if ($i != "a5") bad++ } }
    END { print n, bad + 0 }')
[[ $bytes == "$pagesize 0" ]] ||
    log_fail "the data written through the mapping is not in a snapshot" \
    "taken afterwards (bytes read, differing: $bytes)"

log_pass "A failed rollback does not discard data written through mmap()"
