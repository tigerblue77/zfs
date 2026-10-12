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

# DESCRIPTION:
#	Verify zfs rewrite rewrites the blocks of volumes.
#
# STRATEGY:
#	1. Create a volume and fill it with data.
#	2. Save the checksum of the data and the block pointers.
#	3. Rewrite part of the volume by name.
#	4. Verify the checksum is the same and only the blocks of that part
#	   have changed.
#	5. Rewrite the whole volume and verify all the blocks have changed.
#	6. Verify -S skips the blocks shared with a snapshot, and that without
#	   it they are rewritten.
#	7. Verify that read-only volumes, file systems and names that do not
#	   exist are refused.
#	8. Rewrite the volume repeatedly while it is written to, and verify
#	   that no write is lost.
#	9. Rewrite a volume that is open but was not written to, then roll it
#	   back, which suspends it.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset vol=$TESTPOOL/${TESTVOL}_rewrite
typeset dev=$ZVOL_DEVDIR/$vol
typeset blksz=16384
typeset size=64	# MiB
typeset tmp=$(mktemp)
typeset bps=$(mktemp)
typeset bps1=$(mktemp)
typeset shadow=$(mktemp)
typeset pat=$(mktemp)
typeset holder=""

function cleanup
{
	[[ -n "$holder" ]] && kill $holder 2>/dev/null
	rm -f $tmp $bps $bps1 $shadow $pat $pat.0 $pat.1 $pat.2 $pat.3
	datasetexists $vol && destroy_dataset $vol -R
}

# Print the checksum of the data of a device, as the volume holds it: what the
# kernel has cached for the device must not be mistaken for it.
function dev_hash # device
{
	is_linux && blockdev --flushbufs $1
	xxh128digest $1
}

# Print the index, the first DVA and the checksum of every data block.
function get_blocks # dataset
{
	zdb -vvvvv $1 1 | awk '/ L0 / { print l++ " " $3 " " $7 }'
}

# Print the indexes of the blocks that differ between two lists of blocks.
function changed_blocks # list1 list2
{
	echo $(sort -n $1 $2 | uniq -u | cut -f1 -d' ' | sort -nu)
}

# Print the number of blocks that are the same in two lists of blocks.
function same_blocks # list1 list2
{
	sort -n $1 $2 | uniq -d | wc -l
}

# Write patterns to the volume, and to a file that tracks what it must hold.
function writer # count
{
	typeset i=0

	while ((i < $1)); do
		typeset p=$pat.$((i % 4))
		typeset -i r
		((r = i % 32))

		dd if=$p of=$dev bs=1M seek=$r count=1 2>/dev/null || return 1
		dd if=$p of=$shadow bs=1M seek=$r count=1 conv=notrunc \
		    2>/dev/null || return 1
		((i += 1))
	done
	return 0
}

log_assert "zfs rewrite rewrites the blocks of volumes"

log_onexit cleanup

typeset -i nblocks
((nblocks = size * 1024 * 1024 / blksz))

log_must zfs create -V ${size}m -o volblocksize=$blksz $vol
block_device_wait $dev
log_must dd if=/dev/urandom of=$dev bs=1M count=$size
log_must sync_pool $TESTPOOL
typeset orig_hash=$(dev_hash $dev)
log_must [ -n "$orig_hash" ]
log_must eval "get_blocks $vol > $bps1"
log_must [ "$(wc -l < $bps1)" -eq $nblocks ]

# Rewrite a part of the volume: 128 blocks starting at the 64th.
log_must zfs rewrite -o 1048576 -l 2097152 $vol
log_must sync_pool $TESTPOOL
log_must [ "$orig_hash" = "$(dev_hash $dev)" ]
log_must eval "get_blocks $vol > $bps"
log_must [ "$(changed_blocks $bps $bps1)" = "$(echo $(seq 64 191))" ]

# Rewrite all of it, and have the names of the volumes printed.
log_must eval "get_blocks $vol > $bps1"
log_must [ "$(zfs rewrite -v $vol)" = "$vol" ]
log_must sync_pool $TESTPOOL
log_must [ "$orig_hash" = "$(dev_hash $dev)" ]
log_must eval "get_blocks $vol > $bps"
log_must [ "$(same_blocks $bps $bps1)" -eq 0 ]

# Physical rewrite.
log_must eval "get_blocks $vol > $bps1"
log_must zfs rewrite -P $vol
log_must sync_pool $TESTPOOL
log_must [ "$orig_hash" = "$(dev_hash $dev)" ]
log_must eval "get_blocks $vol > $bps"
log_must [ "$(same_blocks $bps $bps1)" -eq 0 ]

# Blocks shared with a snapshot: skipped by -S, rewritten otherwise.
log_must zfs snapshot $vol@snap
log_must eval "get_blocks $vol@snap > $bps1"
log_must zfs rewrite -S $vol
log_must sync_pool $TESTPOOL
log_must eval "get_blocks $vol > $bps"
log_must [ "$(same_blocks $bps $bps1)" -eq $nblocks ]
log_must zfs rewrite $vol
log_must sync_pool $TESTPOOL
log_must eval "get_blocks $vol > $bps"
log_must [ "$(same_blocks $bps $bps1)" -eq 0 ]
log_must [ "$orig_hash" = "$(dev_hash $dev)" ]

# What can not be rewritten.
log_mustnot zfs rewrite $TESTPOOL/${TESTVOL}_does_not_exist
log_mustnot zfs rewrite $TESTPOOL/$TESTFS
log_mustnot zfs rewrite $vol@snap
log_must zfs set readonly=on $vol
log_mustnot zfs rewrite $vol
log_must zfs set readonly=off $vol

# Rewrite again and again while the volume is written to.
log_must zfs destroy $vol@snap
for i in 0 1 2 3; do
	log_must dd if=/dev/urandom of=$pat.$i bs=1M count=1
done
log_must dd if=$dev of=$shadow bs=1M
writer 300 &
typeset wpid=$!
while kill -0 $wpid 2>/dev/null; do
	log_must zfs rewrite $vol
done
wait $wpid
log_must [ $? -eq 0 ]
log_must sync_pool $TESTPOOL
typeset shadow_hash=$(xxh128digest $shadow)
log_must [ -n "$shadow_hash" ]
log_must [ "$shadow_hash" = "$(dev_hash $dev)" ]

# Rewrite a volume that is open, then suspend it by rolling it back.
log_must zfs snapshot $vol@open
typeset open_hash=$(dev_hash $dev)
log_must [ -n "$open_hash" ]
sleep 3600 < $dev &
holder=$!
log_must zfs rewrite $vol
log_must sync_pool $TESTPOOL
log_must [ "$open_hash" = "$(dev_hash $dev)" ]
log_must dd if=/dev/urandom of=$dev bs=1M count=1
log_must zfs rewrite $vol
log_must zfs rollback $vol@open
log_must [ "$open_hash" = "$(dev_hash $dev)" ]
kill $holder
holder=""

log_pass "zfs rewrite rewrites the blocks of volumes"
