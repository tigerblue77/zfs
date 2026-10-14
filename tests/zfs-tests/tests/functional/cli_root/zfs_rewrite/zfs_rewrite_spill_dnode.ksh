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
#	Verify that zfs rewrite of a whole file also rewrites its spill block
#	and the block that holds its dnode, and that of a whole volume the
#	block that holds its dnode, as the flags allow.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror, which takes
#	   the metadata, and a dataset with xattr=sa and dnodesize=legacy.
#	   Create files with a large extended attribute, so that each has a
#	   spill block: empty files, and files with one data block, which goes
#	   to the raidz vdev and has no indirect block above it.  Create a
#	   volume of two blocks.  Verify that the spill blocks and the blocks
#	   of the dnodes are on the special mirror.
#	2. Rewrite an empty file, and verify that its spill block and the
#	   block of its dnode were written again, and that a rewrite of a part
#	   of a file does not rewrite them.
#	3. Take a snapshot, set allocating=off on the special mirror, and
#	   verify that zfs rewrite -A -S leaves these blocks, which the
#	   snapshot shares, on the special mirror.
#	4. Destroy the snapshot, rewrite with -A, and verify that no spill
#	   block or block of a dnode of the files or of the volume is left on
#	   the special mirror, while their data blocks did not change.
#	5. Verify the data and the extended attributes after an export and
#	   import, and that zdb finds no leak.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zfs_rewrite_spill_dnode
typeset FS=$TESTPOOL1/fs
typeset VOL=$TESTPOOL1/vol
typeset VOLDEV=$ZVOL_DEVDIR/$VOL
typeset XNAME=big
typeset XVAL
typeset -A hash
typeset -A file
typeset SPECIAL SPECIAL_ID

function cleanup
{
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -rf $VDIR
}

#
# Print the id that the DVAs of the blocks give to top-level vdev $2 of pool
# $1, from the pool configuration.
#
function vdev_id # pool vdev
{
	typeset guid

	guid=$(get_vdev_prop guid $1 $2)
	zdb -C $1 | awk -v guid=$guid '
	    $1 == "id:" { id = $2 }
	    $1 == "guid:" && $2 == guid { print id; exit }'
}

#
# Print, for object $2 of dataset $1, its spill block, the block of the
# meta-dnode that holds its dnode, and its data blocks, one line each, as zdb
# prints their block pointers: "spill ...", "dnode ..." and "L0 ...".
#
function obj_meta # dataset object
{
	typeset -i off

	# The meta-dnode is made of 16K blocks of 512-byte dnode slots.
	((off = $2 * 512 / 16384 * 16384))
	zdb -dddddd $1 $2 | awk '
	    /^Spill block:/ { $1 = ""; $2 = ""; print "spill" $0 }
	    /^$/ { found = 0 }
	    found && $2 == "L0" { $1 = ""; $2 = ""; print "L0" $0 }
	    /^Indirect blocks:/ { found = 1 }'
	zdb -dddddd $1 0 | awk -v off="$(printf "%x" $off)" '
	    /^$/ { found = 0 }
	    found && ($1 "") == off && $2 == "L0" {
	        $1 = ""; $2 = ""; print "dnode" $0 }
	    /^Indirect blocks:/ { found = 1 }'
}

#
# Save the lines of obj_meta for every file, and for the volume, in directory
# $1.
#
function save_meta # directory
{
	typeset f

	mkdir -p $1 || return 1
	sync_pool $TESTPOOL1
	for f in "${!file[@]}"; do
		obj_meta $FS "$(get_objnum ${file[$f]})" > $1/$f || return 1
	done
	obj_meta $VOL 1 > $1/vol
}

#
# Print the number of DVAs on the special mirror of the blocks of a kind
# (spill, dnode or L0) in a saved list.
#
function special_dvas # kind list
{
	awk -v id=$SPECIAL_ID -v kind=$1 '
	    $1 == kind { for (i = 2; i <= NF; i++)
	        if (split($i, d, ":") == 3 && d[1] == id) n++ }
	    END { print n + 0 }' $2
}

# Print the lines of a kind of block in a saved list.
function kind # kind list
{
	awk -v kind=$1 '$1 == kind' $2
}

log_assert "zfs rewrite rewrites the spill block and the dnode block of files"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
log_must truncate -s $MINVDEVSIZE $VDIR/r0 $VDIR/r1 $VDIR/r2 $VDIR/s0 $VDIR/s1
log_must zpool create -f $TESTPOOL1 raidz $VDIR/r0 $VDIR/r1 $VDIR/r2 \
    special mirror $VDIR/s0 $VDIR/s1
SPECIAL=$(zpool status $TESTPOOL1 | \
    awk '/special/ { found = 1 } found && /mirror-/ { print $1; exit }')
[[ -n "$SPECIAL" ]] || log_fail "Could not find the special mirror"
SPECIAL_ID=$(vdev_id $TESTPOOL1 $SPECIAL)
[[ -n "$SPECIAL_ID" ]] || log_fail "Could not find the id of $SPECIAL"
log_note "$SPECIAL is top-level vdev $SPECIAL_ID"

# An attribute too large for the bonus buffer of a 512-byte dnode.
XVAL=$(printf "%01500d" 0)
log_must zfs create -o xattr=sa -o dnodesize=legacy -o atime=off \
    -o recordsize=128k $FS
typeset dir
dir=$(get_prop mountpoint $FS)
for i in 0 1 2 3; do
	file[empty$i]=$dir/empty$i
	log_must touch ${file[empty$i]}
	file[data$i]=$dir/data$i
	log_must dd if=/dev/urandom of=${file[data$i]} bs=8k count=1
done
for f in "${!file[@]}"; do
	log_must set_xattr $XNAME "$XVAL" ${file[$f]}
	hash[$f]=$(xxh128digest ${file[$f]})
done
log_must zfs create -V 32k -o volblocksize=16k $VOL
block_device_wait $VOLDEV
log_must dd if=/dev/urandom of=$VOLDEV bs=16k count=2 conv=fsync
hash[vol]=$(xxh128digest $VOLDEV)

log_must save_meta $VDIR/m0
for f in "${!file[@]}"; do
	log_must [ "$(special_dvas spill $VDIR/m0/$f)" -gt 0 ]
	log_must [ "$(special_dvas dnode $VDIR/m0/$f)" -gt 0 ]
	log_must [ "$(special_dvas L0 $VDIR/m0/$f)" -eq 0 ]
done
log_must [ "$(special_dvas dnode $VDIR/m0/vol)" -gt 0 ]
log_must [ "$(special_dvas L0 $VDIR/m0/vol)" -eq 0 ]
log_must [ "$(kind L0 $VDIR/m0/vol | wc -l)" -eq 2 ]

# A whole file is rewritten with its spill block and dnode block.
log_must zfs rewrite ${file[empty0]}
log_must save_meta $VDIR/m1
log_must [ "$(kind spill $VDIR/m1/empty0)" != "$(kind spill $VDIR/m0/empty0)" ]
log_must [ "$(kind dnode $VDIR/m1/empty0)" != "$(kind dnode $VDIR/m0/empty0)" ]

# A part of a file is rewritten without them.
log_must zfs rewrite -l 4096 ${file[data0]}
log_must save_meta $VDIR/m2
log_must [ "$(kind spill $VDIR/m2/data0)" = "$(kind spill $VDIR/m1/data0)" ]
log_must [ "$(kind L0 $VDIR/m2/data0)" != "$(kind L0 $VDIR/m1/data0)" ]

# A snapshot shares the blocks: -S leaves them where they are.
log_must zfs snapshot $FS@snap
log_must zfs snapshot $VOL@snap
log_must zpool set allocating=off $TESTPOOL1 $SPECIAL
log_must zfs rewrite -A -S -r $dir
log_must zfs rewrite -A -S $VOL
log_must save_meta $VDIR/m3
for f in "${!file[@]}" vol; do
	log_must cmp $VDIR/m2/$f $VDIR/m3/$f
done

# Without the snapshot, -A moves them, and leaves the data blocks alone.
log_must zfs destroy $FS@snap
log_must zfs destroy $VOL@snap
log_must zfs rewrite -A -r $dir
log_must zfs rewrite -A $VOL
log_must save_meta $VDIR/m4
for f in "${!file[@]}" vol; do
	log_must [ "$(special_dvas spill $VDIR/m4/$f)" -eq 0 ]
	log_must [ "$(special_dvas dnode $VDIR/m4/$f)" -eq 0 ]
	log_must [ "$(kind L0 $VDIR/m4/$f)" = "$(kind L0 $VDIR/m2/$f)" ]
done

log_must zpool export $TESTPOOL1
log_must zpool import -d $VDIR $TESTPOOL1
block_device_wait $VOLDEV
for f in "${!file[@]}"; do
	log_must [ "$(xxh128digest ${file[$f]})" = "${hash[$f]}" ]
	log_must [ "$(get_xattr $XNAME ${file[$f]})" = "$XVAL" ]
done
log_must [ "$(xxh128digest $VOLDEV)" = "${hash[vol]}" ]
log_must zdb -b $TESTPOOL1

log_pass "zfs rewrite rewrites the spill block and the dnode block of files"
