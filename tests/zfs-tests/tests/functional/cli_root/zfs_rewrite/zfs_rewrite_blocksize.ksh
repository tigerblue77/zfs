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
#	Verify zfs rewrite -b changes the block size of whole files in place.
#
# STRATEGY:
#	1. Write a file at a 16k record size, with a hole, and a hard link.
#	2. Rewrite it with -b 1M.
#	3. Verify its content, inode, links and holes are kept, and that its
#	   block size, and that of the link, is now 1M.
#	4. Verify a smaller size is refused and a file of one block is left.
#	5. Hold back the freeing of unlinked files, rewrite another file, and
#	   verify the shadow left in the unlinked set is freed on remount.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset default_unlink_sp=$(get_tunable UNLINK_SUSPEND_PROGRESS)

function cleanup
{
	log_must set_tunable32 UNLINK_SUSPEND_PROGRESS $default_unlink_sp
	mounted $TESTDIR || log_must zfs mount $TESTPOOL/$TESTFS
	rm -rf $TESTDIR/*
	log_must zfs inherit recordsize $TESTPOOL/$TESTFS
	log_must zfs inherit compression $TESTPOOL/$TESTFS
}

function unlinked
{
	typeset nunlinks=$(kstat_dataset $TESTPOOL/$TESTFS nunlinks)
	typeset nunlinked=$(kstat_dataset $TESTPOOL/$TESTFS nunlinked)
	echo $((nunlinks - nunlinked))
}

log_assert "zfs rewrite -b changes the block size of whole files in place"

log_onexit cleanup

log_must zfs set recordsize=16k $TESTPOOL/$TESTFS
log_must zfs set compression=off $TESTPOOL/$TESTFS

# 8 MiB: 2 MiB of data, a 4 MiB hole, 2 MiB of data.
log_must dd if=/dev/urandom of=$TESTDIR/file bs=1M count=2
log_must dd if=/dev/urandom of=$TESTDIR/file bs=1M count=2 seek=6
log_must ln $TESTDIR/file $TESTDIR/link
log_must sync_pool $TESTPOOL
log_must [ "$(stat -c %o $TESTDIR/file)" = "16384" ]

typeset hash=$(xxh128digest $TESTDIR/file)
typeset inode=$(stat -c %i $TESTDIR/file)
typeset blocks=$(stat -c %b $TESTDIR/file)

log_must zfs rewrite -b 1M $TESTDIR/file
log_must sync_pool $TESTPOOL

log_must [ "$(xxh128digest $TESTDIR/file)" = "$hash" ]
log_must [ "$(stat -c %i $TESTDIR/file)" = "$inode" ]
log_must [ "$(stat -c %h $TESTDIR/file)" = "2" ]
log_must [ "$(stat -c %o $TESTDIR/file)" = "1048576" ]
log_must [ "$(stat -c %o $TESTDIR/link)" = "1048576" ]
log_must eval "zdb -dddd $TESTPOOL/$TESTFS $inode | grep -q ' 1M '"

# The hole is still a hole: the file uses about as much space as before.
typeset newblocks=$(stat -c %b $TESTDIR/file)
log_note "512-byte blocks before $blocks, after $newblocks"
log_must [ $newblocks -lt $((blocks * 11 / 10)) ]

# The shadow is gone.
log_must [ $(unlinked) -eq 0 ]

# Smaller blocks are refused; a file of one block is left as it is.
log_mustnot zfs rewrite -b 128k $TESTDIR/file
log_must [ "$(stat -c %o $TESTDIR/file)" = "1048576" ]
log_must dd if=/dev/urandom of=$TESTDIR/small bs=10k count=1
log_must sync_pool $TESTPOOL
typeset smallbs=$(stat -c %o $TESTDIR/small)
log_must zfs rewrite -b 1M $TESTDIR/small
log_must [ "$(stat -c %o $TESTDIR/small)" = "$smallbs" ]

# -b rewrites whole files only.
log_mustnot zfs rewrite -b 1M -o 0 -l 1048576 $TESTDIR/file
log_mustnot zfs rewrite -b 1M -S $TESTDIR/file
log_mustnot zfs rewrite -b 3000 $TESTDIR/file

# A crash after the exchange leaves the shadow in the unlinked set, and the
# next mount frees it.
log_must dd if=/dev/urandom of=$TESTDIR/file2 bs=1M count=4
log_must sync_pool $TESTPOOL
typeset hash2=$(xxh128digest $TESTDIR/file2)
log_must set_tunable32 UNLINK_SUSPEND_PROGRESS 1
log_must zfs rewrite -b 1M $TESTDIR/file2
log_must [ $(unlinked) -eq 1 ]
log_must zfs umount $TESTPOOL/$TESTFS
log_must set_tunable32 UNLINK_SUSPEND_PROGRESS 0
log_must zfs mount $TESTPOOL/$TESTFS
log_must eval "for i in \$(seq 1 30); do [ \$(unlinked) -eq 0 ] && break; \
    sleep 1; done; [ \$(unlinked) -eq 0 ]"
log_must [ "$(xxh128digest $TESTDIR/file2)" = "$hash2" ]
log_must [ "$(stat -c %o $TESTDIR/file2)" = "1048576" ]

log_pass "zfs rewrite -b changes the block size of whole files in place"
