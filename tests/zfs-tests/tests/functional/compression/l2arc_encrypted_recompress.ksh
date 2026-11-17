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

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# With compressed ARC disabled, the L2ARC compresses an encrypted block
# again and encrypts the result with the block's salt and IV.  A block
# whose stored bytes differ from what this host's compressor gives must
# not be written to the L2ARC as a copy that fails its checksum.
#
# testpool_zstd holds blocks written in 2021 by zstd 1.4.5, whose version
# is in each block's header.  The six 128K blocks of testpool_zstd/x86_64/zstd
# compress here to other bytes that still fit their size on disk, once
# zstd's early abort, which would make them not fit, is disabled.  A
# compressed send stream keeps the stored bytes, and receiving it under an
# encrypted dataset encrypts them as they are.
#
# STRATEGY:
# 1. Receive testpool_zstd/x86_64 with "zfs send -c" into an encrypted
#    dataset of a pool with a cache device.
# 2. Keep no data dbuf cached and no prefetch, so that the blocks read go
#    back to the ARC lists that the L2ARC feed walks, and let the feed walk
#    the whole lists.
# 3. With compressed ARC enabled, import the pool, read the file, wait for
#    data in the L2ARC, unmount, flush the ARC, mount and read the file
#    again: it is read from the L2ARC without checksum errors.
# 4. Do the same with compressed ARC disabled, after the L2ARC feed has run
#    twice, and verify that no L2ARC read fails its checksum.
#

verify_runnable "global"

typeset IMAGE=$STF_SUITE/tests/functional/compression/testpool_zstd.tar.gz
typeset VDIR=$TEST_BASE_DIR/l2arc_encrypted_recompress
typeset VDEV=$VDIR/vdev
typeset VDEV_CACHE=$VDIR/cache
typeset KEYFILE=$VDIR/key
typeset POOL=$TESTPOOL-l2arc
typeset FS=$POOL/encrypted/zstd

function cleanup
{
	poolexists testpool_zstd && destroy_pool testpool_zstd
	poolexists $POOL && destroy_pool $POOL
	rm -rf $VDIR
	restore_tunable COMPRESSED_ARC_ENABLED
	restore_tunable L2ARC_NOPREFETCH
	restore_tunable L2ARC_HEADROOM
	restore_tunable PREFETCH_DISABLE
	restore_tunable DBUF_CACHE_MAX_BYTES
	restore_tunable ZSTD_EARLYABORT_PASS
}

function read_file
{
	log_must test "$(cksum < "$(get_prop mountpoint $FS)/zstd")" = \
	    "$EXPECTED"
}

#
# Wait, a minute at most, until $1 is true after two L2ARC feeds have started
# since the data dbufs were evicted, and every L2ARC write sent is done.
#
function wait_for_feed # condition
{
	typeset -i i=0

	while (( $(kstat dbufstats.cache_count) != 0 )); do
		(( i++ < 60 )) || log_fail "data dbufs still cached"
		sleep 1
	done

	typeset -i feeds=$(( $(kstat arcstats.l2_feeds) + 2 ))
	while (( $(kstat arcstats.l2_feeds) < feeds )) || \
	    (( $(kstat arcstats.l2_writes_sent) != \
	    $(kstat arcstats.l2_writes_done) )) || ! eval "$1"; do
		(( i++ < 60 )) || log_fail "L2ARC feed: $1 not reached"
		sleep 1
	done
}

#
# Import the pool with compressed ARC set to $1, read the file, wait for the
# feed, then flush the ARC and read the file again.  Set HITS and BAD to the
# number of L2ARC reads and of L2ARC checksum failures of the second read.
#
function read_twice # compressed_arc_enabled condition
{
	log_must zpool export $POOL
	log_must set_tunable32 COMPRESSED_ARC_ENABLED $1
	log_must zpool import -l -d $VDIR $POOL

	read_file
	wait_for_feed "$2"
	log_note "l2_bufc_data_asize $(kstat arcstats.l2_bufc_data_asize)"

	typeset -i hits bad
	hits=$(kstat arcstats.l2_hits)
	bad=$(kstat arcstats.l2_cksum_bad)
	log_must zfs unmount $FS
	log_must zinject -a
	log_must zfs mount $FS
	read_file
	HITS=$(( $(kstat arcstats.l2_hits) - hits ))
	BAD=$(( $(kstat arcstats.l2_cksum_bad) - bad ))
	log_note "compressed ARC $1: L2ARC hits $HITS, checksum errors $BAD"
}

log_onexit cleanup

log_assert "L2ARC holds no corrupt copy of an encrypted block that" \
	"compresses again to other bytes"

log_must save_tunable COMPRESSED_ARC_ENABLED
log_must save_tunable L2ARC_NOPREFETCH
log_must save_tunable L2ARC_HEADROOM
log_must save_tunable PREFETCH_DISABLE
log_must save_tunable DBUF_CACHE_MAX_BYTES
log_must save_tunable ZSTD_EARLYABORT_PASS

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
log_must tar --directory $VDIR -xzSf $IMAGE
log_must zpool import -d $VDIR testpool_zstd
EXPECTED=$(cksum < "$(get_prop mountpoint testpool_zstd/x86_64)/zstd")
log_must zfs snapshot testpool_zstd/x86_64@snap

log_must truncate -s $MINVDEVSIZE $VDEV $VDEV_CACHE
log_must zpool create -f $POOL $VDEV cache $VDEV_CACHE
log_must eval "echo 'l2arc-recompress' > $KEYFILE"
log_must zfs create -o encryption=on -o keyformat=passphrase \
	-o keylocation=file://$KEYFILE $POOL/encrypted
log_must eval "zfs send -c testpool_zstd/x86_64@snap | zfs recv $FS"
log_must test "$(get_prop encryptionroot $FS)" = "$POOL/encrypted"
log_must destroy_pool testpool_zstd
log_must rm $VDIR/testpool_zstd

log_must set_tunable32 L2ARC_NOPREFETCH 0
log_must set_tunable64 L2ARC_HEADROOM 0
log_must set_tunable32 PREFETCH_DISABLE 1
log_must set_tunable64 DBUF_CACHE_MAX_BYTES 0
log_must set_tunable32 ZSTD_EARLYABORT_PASS 0

read_twice 1 "(( \$(kstat arcstats.l2_bufc_data_asize) > 0 ))"
log_must test $HITS -gt 0
log_must test $BAD -eq 0

read_twice 0 true
log_must test $BAD -eq 0

log_pass "L2ARC holds no corrupt copy of an encrypted block that" \
	"compresses again to other bytes"
