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
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	zpool remove --destroy-snapshots destroys the snapshots that keep
#	blocks on a device, and only them, before removing it.  It destroys
#	none, and removes nothing, while a file system still keeps blocks on
#	the device, or if one of the snapshots cannot be destroyed.  Without
#	it, the removal of a device that is not empty from a raidz pool is
#	refused, and the error points to the option.
#
# STRATEGY:
#	1. Create a pool on a mirror, a file system and a snapshot @before,
#	   then add a special mirror, write a file, whose metadata goes to
#	   the special mirror, and take a snapshot @after.
#	2. Verify that zpool remove -n --destroy-snapshots lists @after but
#	   not @before, and the file system, which keeps blocks on the device.
#	3. Verify that --destroy-snapshots refuses, names the file system,
#	   and destroys and removes nothing.
#	4. Turn off allocations from the special mirror and delete the file,
#	   and wait until the file system no longer keeps blocks on it.
#	5. Verify that nothing is destroyed or removed while @after has a
#	   user hold, or a clone.
#	6. Remove the special mirror with --destroy-snapshots, and verify
#	   that @after is gone, @before is kept and the removal completes.
#	7. Create a pool on a raidz vdev with a special mirror, a file system
#	   and a snapshot.  Verify that the removal of the special mirror is
#	   refused with a pointer to --destroy-snapshots, and that with it the
#	   removal is refused too, naming the file system, and the snapshot is
#	   kept.
#

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/remove_destroy_snapshots
typeset FS=$TESTPOOL/$TESTFS
typeset OUT=$VDIR/out

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $VDIR
}

#
# Succeeds if zpool remove -n --destroy-snapshots lists no file system or
# volume that keeps blocks on vdev $1.
#
function no_head_on # vdev
{
	zpool remove -n --destroy-snapshots $TESTPOOL $1 > $OUT || return 1
	! grep -q "^File systems and volumes" $OUT
}

log_assert "zpool remove --destroy-snapshots destroys the snapshots that" \
    "keep blocks on the device"
log_onexit cleanup

log_must mkdir -p $VDIR
log_must truncate -s $MINVDEVSIZE $VDIR/0 $VDIR/1 $VDIR/2 $VDIR/s0 $VDIR/s1

log_must zpool create -f $TESTPOOL mirror $VDIR/0 $VDIR/1
log_must zfs create $FS
typeset mntpnt=$(get_prop mountpoint $FS)
log_must dd if=/dev/urandom of=$mntpnt/file0 bs=128k count=8
log_must zfs snapshot $FS@before
log_must zpool add $TESTPOOL special mirror $VDIR/s0 $VDIR/s1
log_must dd if=/dev/urandom of=$mntpnt/file1 bs=128k count=8
sync_pool $TESTPOOL
log_must zfs snapshot $FS@after
log_must test "$(get_vdev_prop allocated $TESTPOOL mirror-1)" -ne 0

log_note "-n lists the snapshots and the file system"
log_must eval "zpool remove -n --destroy-snapshots $TESTPOOL mirror-1 > $OUT"
cat $OUT
log_must grep -q "$FS@after" $OUT
log_mustnot grep -q "$FS@before" $OUT
log_must grep -q "^File systems and volumes that keep blocks" $OUT
log_must grep -qx "	$FS" $OUT
log_must snapexists $FS@after
log_must vdevs_in_pool $TESTPOOL mirror-1

log_note "Nothing is destroyed while the file system keeps blocks there"
log_mustnot_expect "$FS" zpool remove --destroy-snapshots $TESTPOOL mirror-1
log_must snapexists $FS@after
log_must vdevs_in_pool $TESTPOOL mirror-1

log_note "The file system stops keeping blocks on the special mirror"
log_must zpool set allocating=off $TESTPOOL mirror-1
log_must rm $mntpnt/file1
typeset -i i
for ((i = 0; i < 30; i++)); do
	sync_pool $TESTPOOL true
	no_head_on mirror-1 && break
	sleep 1
done
cat $OUT
log_must no_head_on mirror-1
log_must grep -q "$FS@after" $OUT

log_note "A snapshot with a user hold, or a clone, is not destroyed"
log_must zfs hold keep $FS@after
log_mustnot zpool remove --destroy-snapshots $TESTPOOL mirror-1
log_must snapexists $FS@after
log_must vdevs_in_pool $TESTPOOL mirror-1
log_must zfs release keep $FS@after
log_must zfs clone $FS@after $TESTPOOL/clone
log_mustnot zpool remove --destroy-snapshots $TESTPOOL mirror-1
log_must snapexists $FS@after
log_must vdevs_in_pool $TESTPOOL mirror-1
log_must zfs destroy $TESTPOOL/clone

log_note "The snapshots that keep blocks on the device are destroyed"
log_must zpool remove --destroy-snapshots $TESTPOOL mirror-1
log_must wait_for_removal $TESTPOOL
log_mustnot snapexists $FS@after
log_must snapexists $FS@before
log_mustnot vdevs_in_pool $TESTPOOL mirror-1
log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL "errors" "No known data errors"
log_must destroy_pool $TESTPOOL

log_note "A raidz pool"
log_must zpool create -f $TESTPOOL raidz $VDIR/0 $VDIR/1 $VDIR/2 \
    special mirror $VDIR/s0 $VDIR/s1
log_must zfs create $FS
mntpnt=$(get_prop mountpoint $FS)
log_must dd if=/dev/urandom of=$mntpnt/file bs=128k count=8
log_must zfs snapshot $FS@snap
log_mustnot_expect "destroy-snapshots" zpool remove $TESTPOOL mirror-1
log_must eval "zpool remove -n --destroy-snapshots $TESTPOOL mirror-1 > $OUT"
log_must grep -q "$FS@snap" $OUT
log_mustnot_expect "$FS" zpool remove --destroy-snapshots $TESTPOOL mirror-1
log_must snapexists $FS@snap
log_must vdevs_in_pool $TESTPOOL mirror-1

log_pass "zpool remove --destroy-snapshots destroys the snapshots that" \
    "keep blocks on the device"
