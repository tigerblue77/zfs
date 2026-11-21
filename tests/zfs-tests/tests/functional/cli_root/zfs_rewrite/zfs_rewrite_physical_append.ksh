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

# DESCRIPTION:
#	A write that dirties a block while zfs rewrite -P is about to dirty
#	it is not taken for the rewrite: the block gets a new logical birth
#	time, and an incremental stream carries the write.
#
# STRATEGY:
#	1. Create a file of one and a half records, snapshot it and receive
#	   the snapshot into a replica.
#	2. Raise zfs_txg_timeout, so that no transaction group starts to
#	   quiesce on its own while the test runs.
#	3. Rewrite the last block of the file with -P, and hold the rewrite
#	   at the dmu_buf_will_rewrite:clean point, after it has found the
#	   block not dirty and before it dirties it.
#	4. Meanwhile, append to the file.  The append locks only the range
#	   past the end of the file, so it dirties the same block in the
#	   same transaction group without waiting for the rewrite.
#	5. Release the rewrite, snapshot the file and receive the
#	   incremental stream into the replica, which must hold the append.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset src=$TESTPOOL/$TESTFS1
typeset dst=$TESTPOOL/$TESTFS2
typeset orig=$TEST_BASE_DIR/rewrite_append.orig
typeset chunk=$TEST_BASE_DIR/rewrite_append.chunk
typeset expect=$TEST_BASE_DIR/rewrite_append.expect

function cleanup
{
	zinject -c all
	restore_tunable TXG_TIMEOUT
	for ds in $dst $src; do
		datasetexists $ds && destroy_dataset $ds -r
	done
	rm -f $orig $chunk $expect
}

log_assert "zfs rewrite -P does not take a concurrent write for a rewrite"
log_onexit cleanup

# 1. Block 1 of the file holds [128K, 192K) and ends the file.
log_must zfs create -o recordsize=128k -o compression=off $src
typeset file=$(get_prop mountpoint $src)/file
log_must dd if=/dev/urandom of=$orig bs=64k count=3
log_must dd if=/dev/urandom of=$chunk bs=4k count=1
log_must eval "cat $orig $chunk > $expect"
log_must cp $orig $file
sync_pool $TESTPOOL
log_must zfs snapshot $src@a
log_must eval "zfs send $src@a | zfs recv -u $dst"

# 2. Only a quiesce would put the append in a later txg than the rewrite.
log_must save_tunable TXG_TIMEOUT
log_must set_tunable32 TXG_TIMEOUT 600
sync_pool $TESTPOOL

# 3. The rewrite holds its transaction, and the range lock of the block.
typeset id=$(zinject -q -w dmu_buf_will_rewrite:clean -s 60 $TESTPOOL)
[[ -n $id ]] || log_fail "zinject -w failed"
zfs rewrite -P -o 131072 $file &
typeset -i rewritepid=$!
wait_inject_point $id

# 4. The append does not wait for the rewrite.
cat $chunk >> $file &
typeset -i writepid=$!
typeset -i i=0
while kill -0 $writepid 2>/dev/null && ((i++ < 100)); do
	sleep 0.1
done
kill -0 $writepid 2>/dev/null && log_fail "the append waited for the rewrite"
log_must wait $writepid
typeset quiescing=$(kstat_pool $TESTPOOL txgs | awk '$3 == "Q"')
if [[ -n $quiescing ]]; then
	kstat_pool $TESTPOOL txgs
	log_fail "a txg started to quiesce while the rewrite was held"
fi

# 5. A write taken for the rewrite would keep the old logical birth time
# of the block, and the incremental stream would leave the block out.
log_must zinject -c $id
log_must wait $rewritepid
log_must restore_tunable TXG_TIMEOUT
sync_pool $TESTPOOL
log_must cmp $expect $file
log_must zfs snapshot $src@b
log_must eval "zfs send -i @a $src@b | zfs recv -u $dst"
log_must zfs mount $dst
log_must cmp $expect $(get_prop mountpoint $dst)/file

log_pass "zfs rewrite -P does not take a concurrent write for a rewrite"
