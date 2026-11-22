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
# The thread that commits a ZIL block issues its write, but the write is
# not charged to that thread's io cgroup: a ZIL block carries the records
# of every writer of the dataset, so a writer held back by its cgroup
# would hold back the others.
#
# STRATEGY:
# 1. Create a pool on a block device and a dataset with sync=always.
# 2. Enable the io controller in the root cgroup and create a cgroup.
# 3. From that cgroup, write 300 blocks of 4K, each one committed to the
#    ZIL.
# 4. Verify that io.stat charges at most 3 writes to that cgroup.
#

verify_runnable "global"

typeset CG=/sys/fs/cgroup
typeset TCG=$CG/zfs_tests_zil_write_cgroup
typeset IO_ENABLED=false
typeset DISK=${DISKS%% *}
DISK=${DISK#$DEV_DSKDIR/}

function cleanup
{
	[[ -d $TCG ]] && rmdir $TCG
	if $IO_ENABLED; then
		echo -io > $CG/cgroup.subtree_control || \
		    log_note "could not disable the io controller again"
	fi
	destroy_pool $TESTPOOL
}

[[ -f $CG/cgroup.controllers ]] || \
    log_unsupported "cgroup v2 is not mounted at $CG"
grep -qw io $CG/cgroup.controllers || \
    log_unsupported "the io controller is not available"
is_disk_device $DEV_DSKDIR/$DISK || \
    log_unsupported "$DISK is not a block device"

log_assert "ZIL block writes are not charged to the committer's io cgroup"
log_onexit cleanup

log_must zpool create -f $TESTPOOL $DISK
log_must zfs create -o sync=always $TESTPOOL/$TESTFS
typeset mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must zpool sync $TESTPOOL

if ! grep -qw io $CG/cgroup.subtree_control; then
	echo +io > $CG/cgroup.subtree_control || \
	    log_unsupported "cannot enable the io controller"
	IO_ENABLED=true
fi
[[ -d $TCG ]] && log_must rmdir $TCG
log_must mkdir $TCG
log_must test -f $TCG/io.stat

log_must sh -c 'echo $$ > "$1/cgroup.procs" &&
    exec dd if=/dev/urandom of="$2" bs=4k count=300' sh $TCG $mntpnt/file

typeset -i wios=$(awk '{
	for (i = 2; i <= NF; i++)
		if ($i ~ /^wios=/) { sub(/^wios=/, "", $i); w += $i }
	} END { print w + 0 }' $TCG/io.stat)
log_note "writes charged to the writer's cgroup: $wios"
# Charged to the writer, the log blocks would add up to 300 writes or more.
(( wios <= 3 )) || log_fail "$wios writes charged to the writer's cgroup"

log_pass "ZIL block writes are not charged to the committer's io cgroup"
