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
# Copyright (c) 2026, Tigerblue77.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zfs_load-key/zfs_load-key_common.kshlib

#
# DESCRIPTION:
# AES-CCM cannot encrypt a 16M block, so a dataset encrypted with it must
# never get one: a 16M recordsize or volblocksize is refused, an inherited
# one only grows files to 8M blocks, and a non-raw receive of 16M blocks
# into it fails.
#
# STRATEGY:
# 1. Verify that 'zfs create' refuses aes-256-ccm with recordsize=16M
#    or volblocksize=16M, and that aes-256-gcm accepts recordsize=16M.
# 2. Verify that 'zfs set' refuses recordsize=16M on aes-256-ccm.
# 3. Let the aes-256-ccm file system inherit recordsize=16M, write 16M
#    of incompressible data and sync it.
# 4. Verify that the data reads back and that the file has 8M blocks.
# 5. Verify that a 'zfs send -L' stream with 16M blocks is refused by a
#    non-raw receive below it, and that the stream without -L is not.
#

verify_runnable "both"

typeset fs=$TESTPOOL/$TESTFS1
typeset vol=$TESTPOOL/$TESTVOL1
typeset keyfile=$TEST_BASE_DIR/keyfile.$$
typeset srcfile=$TEST_BASE_DIR/srcfile.$$

function cleanup
{
	datasetexists $fs && destroy_dataset $fs -r
	datasetexists $vol && destroy_dataset $vol -f
	rm -f $keyfile $srcfile
}

log_onexit cleanup

log_assert "aes-*-ccm datasets never get blocks larger than 8M"

log_must eval "echo -n '$RAWKEY' > $keyfile"
typeset key="-o keyformat=raw -o keylocation=file://$keyfile"

log_must zfs create -o compression=off $fs
zfs set recordsize=16M $fs || log_unsupported "recordsize=16M not available"

log_mustnot zfs create -o encryption=aes-256-ccm $key -o recordsize=16M \
    $fs/ccm
log_mustnot datasetexists $fs/ccm
log_mustnot zfs create -o encryption=aes-256-ccm $key -o volblocksize=16M \
    -V 64M $vol
log_mustnot datasetexists $vol
log_must zfs create -o encryption=aes-256-gcm $key -o recordsize=16M $fs/gcm

log_must zfs create -o encryption=aes-256-ccm $key -o recordsize=8M $fs/ccm
log_mustnot zfs set recordsize=16M $fs/ccm
log_must test "$(get_prop recordsize $fs/ccm)" = "8388608"

log_must zfs inherit recordsize $fs/ccm
log_must test "$(get_prop recordsize $fs/ccm)" = "16777216"
typeset mntpnt=$(get_prop mountpoint $fs)
log_must dd if=/dev/urandom of=$srcfile bs=1M count=16
log_must cp $srcfile $mntpnt/ccm/file
log_must zpool sync $TESTPOOL
log_must cmp $srcfile $mntpnt/ccm/file
typeset obj=$(get_objnum $mntpnt/ccm/file)
typeset dblk=$(zdb -K $keyfile -dddd $fs/ccm $obj | \
    awk '/ZFS plain file/ {print $4}')
log_must test "$dblk" = "8M"

log_must cp $srcfile $mntpnt/file
log_must zfs snapshot $fs@snap
log_mustnot eval "zfs send -L $fs@snap | zfs receive $fs/ccm/recv"
log_mustnot datasetexists $fs/ccm/recv
log_must eval "zfs send $fs@snap | zfs receive $fs/ccm/recv"
log_must cmp $srcfile $mntpnt/ccm/recv/file

log_pass "aes-*-ccm datasets never get blocks larger than 8M"
