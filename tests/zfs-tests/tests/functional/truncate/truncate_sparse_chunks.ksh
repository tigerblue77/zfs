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

. $STF_SUITE/tests/functional/truncate/truncate.cfg
. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	Truncating a sparse file frees it in chunks of at most 256 L1
#	indirect blocks of 128K, also when the first L1 of an L2 range
#	below the end of the file is a hole.
#
#	get_next_chunk() bounds each chunk of dmu_free_long_range() with
#	backward searches for allocated L1s.  When such a search descends
#	into an L2 at its first entry instead of its last (#19513), it
#	skips every L2 whose first L1 is a hole, and the rest of the file
#	is freed in one chunk.  Once the chunks freed in a txg hold
#	zfs_per_txg_dirty_frees_percent of zfs_dirty_data_max in L1s, the
#	next chunk waits for the next txg and counts a
#	dmu_tx_dirty_frees_delay: a file freed in a single chunk counts
#	none.
#
# STRATEGY:
#	1. Create a dataset with recordsize=512: an L1 spans 512K of the
#	   file, an L2 512M
#	2. Write one block in each of enough L1s for several chunks, in L2
#	   ranges whose first L1 is a hole, then one block in the next L2
#	   range, and sync the pool
#	3. Verify that the file has 4 levels, 128K indirect blocks and
#	   512-byte data blocks
#	4. Set zfs_per_txg_dirty_frees_percent to 1, truncate the file to
#	   0 bytes and verify that dmu_tx_dirty_frees_delay grew
#

verify_runnable "global"

function cleanup
{
	restore_tunable PER_TXG_DIRTY_FREES_PERCENT
	datasetexists $fs && destroy_dataset $fs
}

#
# Writes one block in L1 $2 of the L2 range $1 of $file.
#
function write_l1 # <l2> <l1>
{
	dd if=/dev/urandom of=$file bs=512 count=1 conv=notrunc \
	    seek=$((($1 << 20) + ($2 << 10))) 2>/dev/null ||
	    log_fail "dd failed in L2 $1, L1 $2"
}

fs=$TESTPOOL/$TESTFS/sparse

log_assert "Truncating a sparse file frees it in bounded chunks"
log_onexit cleanup

log_must save_tunable PER_TXG_DIRTY_FREES_PERCENT
log_must zfs create -o recordsize=512 -o compression=off $fs
file=$(get_prop mountpoint $fs)/$TESTFILE

#
# A chunk holds at most DMU_MAX_ACCESS / 2 = 32M of L1s.  Write enough of
# them that the chunks of a txg reach the threshold, plus two chunks.
#
typeset -li chunk_bytes=$((32 << 20))
typeset -li dirty_max=$(get_tunable DIRTY_DATA_MAX)
typeset -li threshold=$((dirty_max / 100))
typeset -i chunks=$(((threshold + chunk_bytes - 1) / chunk_bytes + 2))
typeset -i nl1=$((chunks * 256))
typeset -i per_l2=512
typeset -i l2=0
typeset -i l1
typeset -i n=0
log_note "threshold $threshold bytes: $nl1 L1s, $chunks chunks or more"

#
# Until their txg syncs, the L1s a chunk dirties count against the pool's
# space at spa_asize_inflation (24) times their size.  Without room for
# the chunks before the first one delayed, that one is held for space and
# assigned to a later txg instead, where it counts no delay.
#
typeset -li avail=$(get_prop available $TESTPOOL)
((avail > (chunks - 2) * 24 * chunk_bytes)) ||
    log_unsupported "$TESTPOOL has too little space available: $avail"

while ((n < nl1)); do
	l1=1
	while ((l1 <= per_l2 && n < nl1)); do
		write_l1 $l2 $l1
		((l1 += 1))
		((n += 1))
	done
	((l2 += 1))
done
write_l1 $l2 1
sync_pool $TESTPOOL

typeset obj=$(get_objnum $file)
typeset geom=$(zdb -dd $fs $obj | awk -v obj=$obj '$1 == obj {
    print $2, $3, $4 }')
log_note "lvl iblk dblk: $geom"
[[ "$geom" == "4 128K 512" ]] || log_fail "unexpected object layout: $geom"

log_must set_tunable32 PER_TXG_DIRTY_FREES_PERCENT 1
typeset -li before=$(kstat dmu_tx.dmu_tx_dirty_frees_delay)
log_must truncate -s 0 $file
typeset -li after=$(kstat dmu_tx.dmu_tx_dirty_frees_delay)
log_note "dmu_tx_dirty_frees_delay: $before before, $after after"
if ((after == before)); then
	log_note "$(kstat -g dmu_tx)"
	log_fail "$nl1 L1s freed with no dirty frees delay"
fi
log_must test ! -s $file

log_pass "Truncating a sparse file frees it in bounded chunks"
