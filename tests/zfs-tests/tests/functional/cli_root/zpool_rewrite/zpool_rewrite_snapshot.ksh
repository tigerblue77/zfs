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
#	Verify that zpool rewrite -A reports the snapshots, which keep their
#	blocks on a vdev whose allocating property is off, so that it can not
#	be removed until they are destroyed, that zpool rewrite is refused
#	while the pool has a checkpoint, and that an interruption stops it.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror, which takes
#	   the metadata and the small blocks, and a file system with files.
#	   Interrupt zpool rewrite of it, with writes slowed down so that it
#	   lasts longer, and verify that it failed, and that it can be run
#	   again.  Take a snapshot.
#	2. Verify that zpool rewrite is refused while the pool has a
#	   checkpoint.
#	3. Set allocating=off on the special mirror, run zpool rewrite -A -v,
#	   and verify that it reports the snapshot, that space stays allocated
#	   on the special mirror, and that it can not be removed.
#	4. Destroy the snapshot, and verify that nothing stays allocated on
#	   the special mirror, and that it can then be removed, if this build
#	   can remove an empty vdev beside a raidz vdev.

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_rewrite/zpool_rewrite.kshlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zpool_rewrite_snapshot
typeset FS=$POOL/fs
typeset MNT

function cleanup
{
	zinject -c all > /dev/null 2>&1
	poolexists $POOL && destroy_pool $POOL
	rm -rf $VDIR
}

log_assert "zpool rewrite -A reports snapshots, which keep their blocks"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
rewrite_create_pool $VDIR

log_must zfs create -o special_small_blocks=16k $FS
MNT=$(get_prop mountpoint $FS)
for i in 0 1 2 3 4 5 6 7; do
	log_must dd if=/dev/urandom of=$MNT/$i bs=4k count=2
done
log_must dd if=/dev/urandom of=$MNT/large bs=1M count=64
log_must eval "rewrite_contents $MNT > $VDIR/c0"

# An interruption stops it within a step, with an error.  With writes slowed
# down, it lasts longer than the second it is given.
for disk in r0 r1 r2; do
	log_must zinject -d $VDIR/$disk -D20:1 $POOL
done
timeout -s INT 1 zpool rewrite -v $POOL > $VDIR/out.int
typeset -i rc=$?
log_must zinject -c all
cat $VDIR/out.int
log_must [ $rc -eq 124 ]
log_mustnot grep -q "pool metadata: done" $VDIR/out.int
log_must zpool rewrite $POOL
log_must eval "rewrite_contents $MNT > $VDIR/c1"
log_must cmp $VDIR/c0 $VDIR/c1

log_must zfs snapshot $FS@snap

# A checkpoint keeps what is freed: refused.
log_must zpool checkpoint $POOL
log_mustnot eval "zpool rewrite -A $POOL > $VDIR/out.cp 2>&1"
cat $VDIR/out.cp
log_must grep -q "has a checkpoint" $VDIR/out.cp
log_must zpool checkpoint -d -w $POOL

log_must zpool set allocating=off $POOL $SPECIAL
log_must eval "zpool rewrite -A -v $POOL > $VDIR/out"
cat $VDIR/out
log_must grep -q "^$FS: 1 snapshots keep their blocks" $VDIR/out
log_mustnot rewrite_wait_empty $POOL $SPECIAL 5
log_mustnot zpool remove $POOL $SPECIAL
log_must eval "rewrite_contents $MNT > $VDIR/c1"
log_must cmp $VDIR/c0 $VDIR/c1

# Once the snapshot is gone, nothing holds the old blocks.
log_must zfs destroy $FS@snap
log_must zpool wait -t free $POOL
if ! rewrite_wait_empty $POOL $SPECIAL 60; then
	zpool list -v $POOL
	log_fail "Something stays allocated on $SPECIAL"
fi
log_must zdb -b $POOL

rewrite_remove_empty $POOL $SPECIAL
case $? in
0)
	rewrite_check_hole $POOL
	log_must eval "rewrite_contents $MNT > $VDIR/c2"
	log_must cmp $VDIR/c0 $VDIR/c2
	;;
2)
	log_unsupported "Removing an empty vdev beside a raidz vdev"
	;;
*)
	log_fail "Could not remove $SPECIAL"
	;;
esac

log_pass "zpool rewrite -A reports snapshots, which keep their blocks"
