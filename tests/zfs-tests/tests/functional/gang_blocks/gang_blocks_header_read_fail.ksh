#!/bin/ksh
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
# Description:
# A failed read of a gang header frees the ABD that wraps the header
# buffer.  zio_gang_tree_assemble_done() used to return on an error
# before freeing it, so every failed gang header read leaked an abd_t
# (#19391).
#
# Strategy:
# 1. Write a gang block, and export and import the pool, so that the
#    block is read from disk.
# 2. Inject checksum errors on the file's data and read it once.
# 3. Read it again many times, each read failing on the gang header.
# 4. Verify that abdstats struct_size did not grow with the reads.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/gang_blocks/gang_blocks.kshlib

function cleanup_header_read_fail
{
	zinject -c all >/dev/null 2>&1
	cleanup
}

log_assert "A failed gang header read does not leak its ABD."

preamble
log_onexit cleanup_header_read_fail

typeset -i nreads=2000
typeset -i i before after

log_must zpool create -f $TESTPOOL $DISKS
log_must zfs create -o recordsize=128k -o compression=off -o atime=off \
    $TESTPOOL/$TESTFS
mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
set_tunable64 METASLAB_FORCE_GANGING 100000
set_tunable32 METASLAB_FORCE_GANGING_PCT 100
log_must dd if=/dev/urandom of=$mntpnt/file bs=128k count=1
sync_pool $TESTPOOL
restore_tunable METASLAB_FORCE_GANGING
restore_tunable METASLAB_FORCE_GANGING_PCT
check_is_gang_dva "$(get_first_block_dva $TESTPOOL/$TESTFS file)"

log_must zpool export $TESTPOOL
log_must zpool import $TESTPOOL
log_must zinject -t data -e checksum -f 100 $mntpnt/file
# The first failed read also starts the error log: sync it before measuring.
log_mustnot dd if=$mntpnt/file of=/dev/null bs=128k count=1
sync_pool $TESTPOOL

before=$(kstat abdstats.struct_size)
for (( i = 0; i < nreads; i++ )); do
	dd if=$mntpnt/file of=/dev/null bs=128k count=1 2>/dev/null &&
	    log_fail "read $i of the file did not fail"
done
sync_pool $TESTPOOL
after=$(kstat abdstats.struct_size)
log_note "abdstats struct_size $before before $nreads failed reads," \
    "$after after"
# An abd_t is far larger than 16 bytes.
(( after - before < nreads * 16 )) ||
    log_fail "struct_size grew by $((after - before)) bytes"

log_must zinject -c all
log_pass "A failed gang header read does not leak its ABD."
