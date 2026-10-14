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
#	Verify that zfs rewrite -A rewrites the blocks that have a copy on a
#	vdev with allocating=off, data and indirect blocks, and only those.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror.  Write a
#	   file in two datasets whose data blocks go to the special mirror.
#	   Write a file of 4K records, which has two levels of indirect
#	   blocks, and a volume, whose data blocks do not go there, so that
#	   only their indirect blocks are there.  Write a sparse file with
#	   blocks 256 MiB apart while the special mirror does not allocate.
#	2. Rewrite everything with -A while the special mirror allocates, and
#	   verify that no block changed.
#	3. Write a block between the two of the sparse file, so that its new
#	   level 1 indirect block and the level 2 block above go to the
#	   special mirror, but not the level 1 block above its first block.
#	   Set allocating=off on the special mirror.  Rewrite the first block
#	   with -A, and verify that the level 2 block moved, but not the data,
#	   nor the new level 1 block, which is out of the range.
#	4. Rewrite the first two datasets with -A -r, the first one with -W
#	   and the second one with -P too, and verify that the special mirror freed their data, that the
#	   logical birth times were kept by -P, and that no block of the files
#	   is left on the special mirror.
#	5. Rewrite the other datasets and the volume with -A, and verify that
#	   their data blocks did not change, while none of their indirect
#	   blocks is left on the special mirror.
#	6. Verify the data after an export and import, and that zdb finds no
#	   leak.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zfs_rewrite_allocating
typeset VOL=$TESTPOOL1/vol
typeset VOLDEV=$ZVOL_DEVDIR/$VOL
typeset -i DATA_SIZE=$((16 * 1024 * 1024))
typeset -A hash
typeset -A file
typeset SPECIAL SPECIAL_ID

function cleanup
{
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	rm -rf $VDIR
}

function special_alloc
{
	get_vdev_prop allocated $TESTPOOL1 $SPECIAL
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
# Print a line for every block of object $2 of dataset $1, as zdb prints it:
# offset, level, DVAs, sizes, fill, birth times and checksum.
#
function obj_blocks # dataset object
{
	zdb -dddddd $1 $2 | awk '
	    /^$/ { found = 0 }
	    found && $2 ~ /^L[0-9]+$/ { print }
	    /^Indirect blocks:/ { found = 1 }'
}

# Same as obj_blocks, for a file.
function file_blocks # path
{
	obj_blocks "$(zfs list -H -o name $1)" "$(get_objnum $1)"
}

#
# Save the blocks of every file and of the volume, by name, in directory $1.
#
function save_blocks # directory
{
	typeset f

	mkdir -p $1 || return 1
	sync_pool $TESTPOOL1
	for f in "${!file[@]}"; do
		file_blocks ${file[$f]} > $1/$f || return 1
	done
	obj_blocks $VOL 1 > $1/vol
}

#
# Print the number of DVAs on the special mirror in a list of blocks, or in the
# standard input, of the data blocks (L0), the indirect blocks (ind) or all.
#
function special_dvas # L0|ind|all [list]
{
	awk -v id=$SPECIAL_ID -v which=$1 '
	    which == "L0" && $2 != "L0" { next }
	    which == "ind" && $2 == "L0" { next }
	    { for (i = 3; i <= NF; i++)
	          if (split($i, d, ":") == 3 && d[1] == id) n++ }
	    END { print n + 0 }' $2
}

#
# Verify that the data blocks of $1 are the same in two directories of saved
# lists, or only their offsets, or only their offsets and logical birth times.
#
function same_l0 # name directory1 directory2
{
	awk '$2 == "L0"' $2/$1 > $2/$1.l0 || return 1
	awk '$2 == "L0"' $3/$1 > $3/$1.l0 || return 1
	cmp $2/$1.l0 $3/$1.l0
}

function same_l0_offsets # name directory1 directory2
{
	awk '$2 == "L0" { print $1 }' $2/$1 > $2/$1.off || return 1
	awk '$2 == "L0" { print $1 }' $3/$1 > $3/$1.off || return 1
	cmp $2/$1.off $3/$1.off
}

function same_l0_births # name directory1 directory2
{
	typeset d

	for d in $2 $3; do
		awk '$2 == "L0" { for (i = 3; i <= NF; i++)
		    if (sub(/^B=/, "", $i)) { sub(/\/.*/, "", $i);
		    print $1, $i } }' $d/$1 > $d/$1.birth || return 1
	done
	[[ -s $2/$1.birth ]] && cmp $2/$1.birth $3/$1.birth
}

#
# Rewrite with -A and the options in $2 the files of dataset $1, and wait
# until the special mirror has freed nearly all of their data.
#
function rewrite_and_check # dataset options
{
	typeset fs=$TESTPOOL1/$1
	typeset -i before
	typeset -i freed=0
	typeset -i i

	before=$(special_alloc)
	log_must zfs rewrite -A $2 -r "$(get_prop mountpoint $fs)"
	for ((i = 0; i < 30; i++)); do
		sync_pool $TESTPOOL1
		((freed = before - $(special_alloc)))
		((freed >= DATA_SIZE * 7 / 8)) && break
		sleep 1
	done
	log_note "zfs rewrite -A $2 of $fs freed $freed bytes on $SPECIAL"
	if ((freed < DATA_SIZE * 7 / 8)); then
		zpool list -v $TESTPOOL1
		log_fail "zfs rewrite -A $2 left the data of $fs on $SPECIAL"
	fi
}

log_assert "zfs rewrite -A rewrites the blocks on a vdev with allocating=off"

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

# Data on the special mirror, and data with only metadata there.
for fs in small phys; do
	log_must zfs create -o recordsize=128k -o special_small_blocks=128k \
	    $TESTPOOL1/$fs
	file[$fs]=$(get_prop mountpoint $TESTPOOL1/$fs)/f
	log_must dd if=/dev/urandom of=${file[$fs]} bs=128k \
	    count=$((DATA_SIZE / (128 * 1024)))
done
log_must zfs create -o recordsize=4k $TESTPOOL1/dense
file[dense]=$(get_prop mountpoint $TESTPOOL1/dense)/f
log_must dd if=/dev/urandom of=${file[dense]} bs=4k count=2048
log_must zfs create -o recordsize=128k $TESTPOOL1/sparse
file[sparse]=$(get_prop mountpoint $TESTPOOL1/sparse)/f
log_must zfs create -V $((DATA_SIZE / 1048576))m -o volblocksize=16k $VOL
block_device_wait $VOLDEV
log_must dd if=/dev/urandom of=$VOLDEV bs=1M count=$((DATA_SIZE / 1048576)) \
    conv=fsync

#
# The sparse file is written while the special mirror does not allocate.  The
# blocks are allocated when their transaction group syncs: sync before each
# change of the property.
#
sync_pool $TESTPOOL1
log_must zpool set allocating=off $TESTPOOL1 $SPECIAL
log_must dd if=/dev/urandom of=${file[sparse]} bs=128k count=1
log_must dd if=/dev/urandom of=${file[sparse]} bs=128k count=1 seek=2048 \
    conv=notrunc
sync_pool $TESTPOOL1
log_must zpool set allocating=on $TESTPOOL1 $SPECIAL

for f in "${!file[@]}"; do
	hash[$f]=$(xxh128digest ${file[$f]})
done
hash[vol]=$(xxh128digest $VOLDEV)

log_must save_blocks $VDIR/b0
for f in small phys; do
	log_must [ "$(special_dvas L0 $VDIR/b0/$f)" -eq \
	    $((DATA_SIZE / (128 * 1024))) ]
done
for f in dense vol; do
	log_must [ "$(special_dvas L0 $VDIR/b0/$f)" -eq 0 ]
	log_must [ "$(special_dvas ind $VDIR/b0/$f)" -gt 0 ]
done
log_must grep -q " L2 " $VDIR/b0/dense
log_must [ "$(special_dvas all $VDIR/b0/sparse)" -eq 0 ]

# While the special mirror allocates, -A leaves every block alone.
for fs in small phys dense sparse; do
	log_must zfs rewrite -A -r "$(get_prop mountpoint $TESTPOOL1/$fs)"
done
log_must zfs rewrite -A $VOL
log_must save_blocks $VDIR/b1
for f in "${!file[@]}" vol; do
	log_must cmp $VDIR/b0/$f $VDIR/b1/$f
done

#
# A block written in the sparse file while the special mirror allocates puts
# its new level 1 block there, and the level 2 block above.  The level 1 block
# above the first block stays on the raidz vdev.
#
log_must dd if=/dev/urandom of=${file[sparse]} bs=128k count=1 seek=1024 \
    conv=notrunc
hash[sparse]=$(xxh128digest ${file[sparse]})
sync_pool $TESTPOOL1
log_must zpool set allocating=off $TESTPOOL1 $SPECIAL
log_must save_blocks $VDIR/b2
log_must [ "$(awk '$2 == "L2"' $VDIR/b2/sparse | special_dvas all)" -gt 0 ]
log_must [ "$(awk '$2 == "L1" && $1 == "0"' $VDIR/b2/sparse | \
    special_dvas all)" -eq 0 ]
log_must [ "$(awk '$2 == "L1" && $1 == "8000000"' $VDIR/b2/sparse | \
    special_dvas all)" -gt 0 ]

# Rewriting the first block moves the level 2 block, and only that block.
log_must zfs rewrite -A -l 131072 ${file[sparse]}
log_must save_blocks $VDIR/b3
log_must [ "$(awk '$2 == "L2"' $VDIR/b3/sparse | special_dvas all)" -eq 0 ]
log_must [ "$(awk '$2 == "L1" && $1 == "8000000"' $VDIR/b3/sparse)" = \
    "$(awk '$2 == "L1" && $1 == "8000000"' $VDIR/b2/sparse)" ]
log_must same_l0 sparse $VDIR/b2 $VDIR/b3

# The data on the special mirror moves, -W does not keep it there, and -P
# keeps its logical birth times.
rewrite_and_check small "-W"
rewrite_and_check phys "-P"
log_must save_blocks $VDIR/b4
for f in small phys; do
	log_must [ "$(special_dvas all $VDIR/b4/$f)" -eq 0 ]
	log_must same_l0_offsets $f $VDIR/b0 $VDIR/b4
done
log_must same_l0_births phys $VDIR/b0 $VDIR/b4

# Only the indirect blocks of the other files and of the volume move.
for fs in dense sparse; do
	log_must zfs rewrite -A -r "$(get_prop mountpoint $TESTPOOL1/$fs)"
done
log_must zfs rewrite -A $VOL
log_must save_blocks $VDIR/b5
for f in dense vol; do
	log_must [ "$(special_dvas all $VDIR/b5/$f)" -eq 0 ]
	log_must same_l0 $f $VDIR/b0 $VDIR/b5
done
log_must [ "$(special_dvas all $VDIR/b5/sparse)" -eq 0 ]
log_must same_l0 sparse $VDIR/b2 $VDIR/b5

log_must zpool export $TESTPOOL1
log_must zpool import -d $VDIR $TESTPOOL1
block_device_wait $VOLDEV
for f in "${!file[@]}"; do
	log_must [ "$(xxh128digest ${file[$f]})" = "${hash[$f]}" ]
done
log_must [ "$(xxh128digest $VOLDEV)" = "${hash[vol]}" ]
log_must zdb -b $TESTPOOL1

log_pass "zfs rewrite -A rewrites the blocks on a vdev with allocating=off"
