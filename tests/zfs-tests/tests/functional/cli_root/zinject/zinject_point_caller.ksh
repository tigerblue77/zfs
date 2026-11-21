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
# DESCRIPTION:
#	Every file system operation reaches injection point zfs_enter with
#	its function as the caller, and a handler for 'zfs_enter@caller'
#	acts only on that operation.
#
# STRATEGY:
#	1. Create a pool and file system, and write a file.
#	2. Pause zfs_enter@zfs_write, and start a write to a second file in
#	   the background.  Wait until it is paused, and check that the
#	   first file can still be read and looked up and that the write
#	   stays paused.  Clear the handler, and check that the write
#	   completes.
#	3. Fail zfs_enter@zfs_read once with EIO: check that the first read
#	   of the file fails and the next one returns its contents.
#	4. Delay zfs_enter, for every caller, and check that operations
#	   are counted and still succeed.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDEV=$TEST_BASE_DIR/zinject_point_caller.dat
typeset POOL=zinject_point_caller
typeset FS=$POOL/fs
typeset -i pid=0

function cleanup
{
	zinject -c all
	((pid != 0)) && wait $pid
	destroy_pool $POOL
	rm -f $VDEV
}

log_onexit cleanup

log_assert "zinject -n zfs_enter@caller acts on one kind of operation"

log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV
log_must zfs create $FS
typeset mnt=$(get_prop mountpoint $FS)
log_must dd if=/dev/urandom of=$mnt/file1 bs=128k count=8
typeset sum=$(xxh128digest $mnt/file1)

# Pause writes only.
typeset id=$(zinject -q -n zfs_enter@zfs_write -A pause $POOL)
log_must test -n "$id"
dd if=/dev/urandom of=$mnt/file2 bs=128k count=8 &
pid=$!
wait_inject_point $id 1 30 || log_fail "the write did not reach zfs_enter"
#
# Leave the second file alone while its write is paused: on FreeBSD the
# write holds its vnode lock, so even a stat of it would wait.
#
log_must test "$(xxh128digest $mnt/file1)" = "$sum"
log_must test "$(stat_size $mnt/file1)" = "$((128 * 1024 * 8))"
sleep 2
kill -0 $pid || log_fail "the write did not stay paused"
log_must test "$(inject_point_field $id INJECT)" = "1"
log_must zinject -c $id
wait $pid || log_fail "the write failed after the pause"
pid=0
log_must test "$(stat_size $mnt/file2)" = "$((128 * 1024 * 8))"

# Fail one read.
id=$(zinject -q -n zfs_enter@zfs_read -e io $POOL)
log_must test -n "$id"
#
# dd, so that the file is read with read(2): with some newer cat
# implementations, cat to /dev/null did not reach zfs_read().
#
log_mustnot dd if=$mnt/file1 of=/dev/null bs=128k
log_must test -z "$(inject_point_field $id POINT)"
log_must test "$(xxh128digest $mnt/file1)" = "$sum"

# Delay every operation a little.
id=$(zinject -q -n zfs_enter -D 1 $POOL)
log_must test -n "$id"
log_must ls -l $mnt
log_must test "$(xxh128digest $mnt/file1)" = "$sum"
typeset -i n=$(inject_point_field $id INJECT)
log_note "zfs_enter delayed $n operations"
((n > 0)) || log_fail "zfs_enter was not reached"
log_must zinject -c $id

log_pass "zinject -n zfs_enter@caller acts on one kind of operation"
