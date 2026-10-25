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
. $STF_SUITE/tests/functional/rsend/rsend.kshlib

#
# Description:
# A raw incremental send of an encrypted dataset carries the blocks that
# zfs rewrite -P re-encrypted, so that the replica can be read once its key
# is loaded.  Dedup hits and block clones made after the incremental source,
# which are new logically but not physically, are still sent too.
#
# Strategy:
# 1) Create three encrypted datasets, one of them with dedup=on, write a
#	file in each, snapshot them and raw send the snapshots to a second
#	pool.
# 2) Rewrite the first file with -P.  In the dedup dataset, copy the file,
#	which makes dedup hits, and rewrite it with -P.  In the third, clone
#	the file and rewrite the original with -P.
# 3) Snapshot the datasets again.  A non-raw incremental of the first one
#	must still leave the rewritten blocks out, and a raw one must not.
# 4) Raw send the incrementals, load the key of each replica, mount it,
#	and compare every file with its source.
#

verify_runnable "global"

typeset passphrase="password"
typeset keyfile="$TEST_BASE_DIR/send_raw_rewrite_physical.key"
typeset datasets="rewrite dedup clone"
typeset -i nblocks=32
typeset -i filesize=$((nblocks * 128 * 1024))

function cleanup
{
	typeset ds
	for ds in $datasets; do
		datasetexists $POOL2/$ds && destroy_dataset $POOL2/$ds -r
		datasetexists $POOL/$ds && destroy_dataset $POOL/$ds -r
	done
	rm -f $keyfile $BACKDIR/rewrite-raw $BACKDIR/rewrite-plain
}

# Number of L0 blocks two files share.
function same_blocks # dataset1 file1 dataset2 file2
{
	get_same_blocks $1 $2 $3 $4 $passphrase | wc -w
}

log_assert "Raw incremental sends carry the blocks of zfs rewrite -P"
log_onexit cleanup

log_must eval "echo $passphrase > $keyfile"

for ds in $datasets; do
	typeset dedup=off
	[[ $ds == dedup ]] && dedup=on
	log_must zfs create -o encryption=on -o keyformat=passphrase \
	    -o keylocation=file://$keyfile -o recordsize=128k \
	    -o dedup=$dedup $POOL/$ds
	log_must dd if=/dev/urandom of=$(get_prop mountpoint $POOL/$ds)/file \
	    bs=128k count=$nblocks
done
log_must sync_pool $POOL
for ds in $datasets; do
	log_must zfs snapshot $POOL/$ds@a
	log_must eval "zfs send -w $POOL/$ds@a | zfs recv $POOL2/$ds"
done

typeset mnt=$(get_prop mountpoint $POOL/rewrite)
log_must zfs rewrite -P $mnt/file

# dd rather than cp, which may clone the file instead.
mnt=$(get_prop mountpoint $POOL/dedup)
log_must dd if=$mnt/file of=$mnt/copy bs=128k
log_must zfs rewrite -P $mnt/file

mnt=$(get_prop mountpoint $POOL/clone)
log_must clonefile -f $mnt/file $mnt/clone
log_must zfs rewrite -P $mnt/file
log_must sync_pool $POOL

log_must test $(same_blocks $POOL/rewrite@a file $POOL/rewrite file) -eq 0
log_must test $(same_blocks $POOL/dedup file $POOL/dedup copy) -eq $nblocks
log_must test $(same_blocks $POOL/clone@a file $POOL/clone clone) -eq $nblocks
log_must test $(same_blocks $POOL/clone@a file $POOL/clone file) -eq 0

for ds in $datasets; do
	log_must zfs snapshot $POOL/$ds@b
done

log_must eval "zfs send -i @a $POOL/rewrite@b > $BACKDIR/rewrite-plain"
log_must eval "zfs send -w -i @a $POOL/rewrite@b > $BACKDIR/rewrite-raw"
typeset -i plain_size=$(stat_size $BACKDIR/rewrite-plain)
typeset -i raw_size=$(stat_size $BACKDIR/rewrite-raw)
log_note "Incremental after rewrite -P: $plain_size bytes, raw $raw_size bytes"
log_must test $plain_size -lt $((filesize / 4))
log_must test $raw_size -ge $filesize

for ds in $datasets; do
	if [[ $ds == rewrite ]]; then
		log_must eval "zfs recv $POOL2/$ds < $BACKDIR/rewrite-raw"
	else
		log_must eval "zfs send -w -i @a $POOL/$ds@b |" \
		    "zfs recv $POOL2/$ds"
	fi
	log_must zfs load-key -L file://$keyfile $POOL2/$ds
	log_must zfs mount $POOL2/$ds
	typeset src=$(get_prop mountpoint $POOL/$ds)
	typeset dst=$(get_prop mountpoint $POOL2/$ds)
	for f in file copy clone; do
		[[ -f $src/$f ]] || continue
		log_must test "$(xxh128digest $src/$f)" = \
		    "$(xxh128digest $dst/$f)"
	done
done
log_must check_pool_status $POOL2 "errors" "No known data errors"

log_pass "Raw incremental sends carry the blocks of zfs rewrite -P"
