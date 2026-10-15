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
#	Verify that zpool rewrite -A empties a special vdev whose allocating
#	property is off, so that it can be removed as if it had never been
#	there.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror, which takes
#	   the metadata and the small blocks, and fill it: file systems,
#	   mounted, unmounted and encrypted, with files, a fat directory,
#	   extended attributes in spill blocks, files of other users with
#	   quotas and cloned blocks, and a volume.
#	2. Set allocating=off on the special mirror, run zpool rewrite -A -v,
#	   and verify that nothing stays allocated on the special mirror.
#	3. Verify the data after an export and import, and that zdb finds no
#	   leak.
#	4. Remove the special mirror, and verify that it became a hole, with
#	   no indirect vdev and no feature activated.  A build that can not
#	   remove an empty vdev beside a raidz vdev is reported as such.

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_rewrite/zpool_rewrite.kshlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zpool_rewrite_drain
typeset FS=$POOL/fs
typeset UNMOUNTED=$POOL/unmounted
typeset ENC=$POOL/encrypted
typeset VOL=$POOL/vol
typeset VOLDEV=$ZVOL_DEVDIR/$VOL
typeset MNT XVAL VOLHASH

function cleanup
{
	poolexists $POOL && destroy_pool $POOL
	rm -rf $VDIR
}

# Fill the file system mounted at $1.
function fill # mountpoint
{
	typeset names
	typeset -i i

	log_must mkdir $1/small $1/large $1/fat $1/owned
	for ((i = 0; i < 20; i++)); do
		log_must dd if=/dev/urandom of=$1/small/$i bs=4k \
		    count=$((i % 4 + 1))
	done
	for ((i = 0; i < 3; i++)); do
		log_must dd if=/dev/urandom of=$1/large/$i bs=128k \
		    count=$((i + 2))
	done
	names=$(od -An -tx1 -N$((400 * 30)) /dev/urandom | tr -d ' \n')
	for ((i = 0; i < 400; i++)); do
		: > $1/fat/${names:$((i * 60)):60} || \
		    log_fail "Could not create entry $i in $1/fat"
	done
	for ((i = 0; i < 4; i++)); do
		log_must set_xattr user.rewrite "$XVAL" $1/small/$i
		log_must dd if=/dev/urandom of=$1/owned/$i bs=4k count=2
		log_must chown 1234:5678 $1/owned/$i
	done
}

log_assert "zpool rewrite -A empties a vdev whose allocating property is off"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
rewrite_create_pool $VDIR

# Random, so that it is not compressed into a block too small to spill.
XVAL=$(od -An -tx1 -N750 /dev/urandom | tr -d ' \n')
for fs in $FS $UNMOUNTED; do
	log_must zfs create -o xattr=sa -o dnodesize=legacy \
	    -o special_small_blocks=16k $fs
	log_must zfs set userquota@1234=100m groupquota@5678=100m $fs
	fill $(get_prop mountpoint $fs)
done
log_must eval "echo password | zfs create -o encryption=on \
    -o keyformat=passphrase -o keylocation=prompt \
    -o special_small_blocks=16k $ENC"
log_must dd if=/dev/urandom of=$(get_prop mountpoint $ENC)/file bs=4k count=2
MNT=$(get_prop mountpoint $FS)
for i in 0 1 2; do
	clonefile -f $MNT/large/$i $MNT/large/clone$i || \
	    log_note "Could not clone $MNT/large/$i"
done
log_must zfs create -V 2m -o volblocksize=16k -o special_small_blocks=16k \
    $VOL
block_device_wait $VOLDEV
log_must dd if=/dev/urandom of=$VOLDEV bs=16k count=128 conv=fsync
VOLHASH=$(xxh128digest $VOLDEV)
log_must eval "rewrite_contents $(get_prop mountpoint $POOL) > $VDIR/c0"
log_must zfs unmount $UNMOUNTED
sync_pool $POOL true
log_note "$(get_vdev_prop allocated $POOL $SPECIAL) allocated on $SPECIAL"
log_must [ "$(get_vdev_prop allocated $POOL $SPECIAL)" -gt 0 ]

log_must zpool set allocating=off $POOL $SPECIAL
log_must eval "zpool rewrite -A -v $POOL > $VDIR/out"
cat $VDIR/out
for ds in $POOL $FS $UNMOUNTED $ENC $VOL; do
	log_must grep -q "^$ds: done" $VDIR/out
done
log_must grep -q "^$POOL: pool metadata: done" $VDIR/out
log_must grep -q "^$POOL: $SPECIAL (allocating=off):" $VDIR/out
log_mustnot ismounted $UNMOUNTED
if ! rewrite_wait_empty $POOL $SPECIAL 60; then
	zpool list -v $POOL
	zdb -bb $POOL | tail -40
	log_fail "Something stays allocated on $SPECIAL"
fi

log_must zpool export $POOL
log_must zpool import -d $VDIR $POOL
log_must eval "echo password | zfs load-key $ENC"
log_must zfs mount -a
block_device_wait $VOLDEV
log_must eval "rewrite_contents $(get_prop mountpoint $POOL) > $VDIR/c1"
log_must cmp $VDIR/c0 $VDIR/c1
log_must [ "$(xxh128digest $VOLDEV)" = "$VOLHASH" ]
log_must [ "$(get_vdev_prop allocated $POOL $SPECIAL)" -eq 0 ]
log_must zdb -b $POOL

rewrite_remove_empty $POOL $SPECIAL
case $? in
0)
	rewrite_check_hole $POOL
	log_must eval "rewrite_contents $(get_prop mountpoint $POOL) > $VDIR/c2"
	log_must cmp $VDIR/c0 $VDIR/c2
	log_must zpool export $POOL
	log_must zpool import -d $VDIR $POOL
	rewrite_check_hole $POOL
	log_must zdb -b $POOL
	;;
2)
	log_unsupported "Removing an empty vdev beside a raidz vdev"
	;;
*)
	log_fail "Could not remove $SPECIAL"
	;;
esac

log_pass "zpool rewrite -A empties a vdev whose allocating property is off"
