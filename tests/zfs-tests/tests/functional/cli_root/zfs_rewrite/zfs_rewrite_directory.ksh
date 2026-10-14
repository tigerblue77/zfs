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
#	Verify that zfs rewrite -r rewrites the directories it reaches, the
#	given one included: the blocks of their ZAP objects, fat or micro, and
#	the blocks of their dnodes, as the flags allow.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror, which takes
#	   the metadata, and in a file system, a directory with enough entries
#	   to be a fat ZAP with several leaf blocks and an indirect block, a
#	   directory with a few entries, which stays a micro ZAP, and an empty
#	   directory.  Verify that the blocks of the directories, and of their
#	   dnodes, are on the special mirror.
#	2. Take a snapshot, set allocating=off on the special mirror, and
#	   verify that zfs rewrite -A -S -r, and zfs rewrite -A -r with a
#	   range, which only applies to files, leave these blocks alone.
#	3. Destroy the snapshot, and verify that zfs rewrite -A -r of the file
#	   system leaves none of these blocks on the special mirror.
#	4. Verify the entries of the directories after an export and import,
#	   and that zdb finds no leak.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zfs_rewrite_directory
typeset FS=$TESTPOOL1/fs
typeset -i FAT_ENTRIES=1000
typeset -A dir
typeset SPECIAL SPECIAL_ID MNT

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
# Print the block pointers of object $2 of dataset $1, as zdb prints them,
# and that of the block of the meta-dnode that holds its dnode, as "dnode ...".
#
function obj_meta # dataset object
{
	typeset -i off

	# The meta-dnode is made of 16K blocks of 512-byte dnode slots.
	((off = $2 * 512 / 16384 * 16384))
	zdb -dddddd $1 $2 | awk '
	    /^$/ { found = 0 }
	    found && $2 ~ /^L[0-9]+$/ { print }
	    /^Indirect blocks:/ { found = 1 }'
	zdb -dddddd $1 0 | awk -v off="$(printf "%x" $off)" '
	    /^$/ { found = 0 }
	    found && ($1 "") == off && $2 == "L0" {
	        $1 = ""; $2 = ""; print "dnode" $0 }
	    /^Indirect blocks:/ { found = 1 }'
}

# Save the lines of obj_meta for every directory in directory $1.
function save_meta # directory
{
	typeset d

	mkdir -p $1 || return 1
	sync_pool $TESTPOOL1
	for d in "${!dir[@]}"; do
		obj_meta $FS "$(get_objnum ${dir[$d]})" > $1/$d || return 1
	done
}

#
# Print the number of DVAs on the special mirror of the blocks of a kind in a
# saved list: "dnode", or "zap" for those of the ZAP object.
#
function special_dvas # dnode|zap list
{
	awk -v id=$SPECIAL_ID -v kind=$1 '
	    kind == "dnode" && $1 != "dnode" { next }
	    kind == "zap" && $1 == "dnode" { next }
	    { for (i = 2; i <= NF; i++)
	          if (split($i, d, ":") == 3 && d[1] == id) n++ }
	    END { print n + 0 }' $2
}

# Print the number of blocks of a level in a saved list.
function nblocks # level list
{
	awk -v level=$1 '$2 == level { n++ } END { print n + 0 }' $2
}

# Save the sorted names in every directory in directory $1.
function save_names # directory
{
	typeset d

	mkdir -p $1 || return 1
	for d in "${!dir[@]}"; do
		ls -a ${dir[$d]} | sort > $1/$d || return 1
	done
}

log_assert "zfs rewrite -r rewrites the blocks of directories"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
log_must truncate -s $MINVDEVSIZE $VDIR/r0 $VDIR/r1 $VDIR/r2 $VDIR/s0 $VDIR/s1
# A block that compresses well enough is embedded in its block pointer, and
# then has no DVA to check: keep every block a real one.
log_must zpool create -f -o feature@embedded_data=disabled $TESTPOOL1 \
    raidz $VDIR/r0 $VDIR/r1 $VDIR/r2 special mirror $VDIR/s0 $VDIR/s1
SPECIAL=$(zpool status $TESTPOOL1 | \
    awk '/special/ { found = 1 } found && /mirror-/ { print $1; exit }')
[[ -n "$SPECIAL" ]] || log_fail "Could not find the special mirror"
SPECIAL_ID=$(vdev_id $TESTPOOL1 $SPECIAL)
[[ -n "$SPECIAL_ID" ]] || log_fail "Could not find the id of $SPECIAL"
log_note "$SPECIAL is top-level vdev $SPECIAL_ID"

log_must zfs create -o dnodesize=legacy -o atime=off $FS
MNT=$(get_prop mountpoint $FS)
dir[top]=$MNT
dir[fat]=$MNT/fat
dir[micro]=$MNT/micro
dir[empty]=$MNT/empty
log_must mkdir ${dir[fat]} ${dir[micro]} ${dir[empty]}

# Random names: names of 50 characters or more make a ZAP fat at once.
typeset names
names=$(od -An -tx1 -N$((FAT_ENTRIES * 30)) /dev/urandom | tr -d ' \n')
typeset -i i
for ((i = 0; i < FAT_ENTRIES; i++)); do
	: > ${dir[fat]}/${names:$((i * 60)):60} || \
	    log_fail "Could not create entry $i in ${dir[fat]}"
done
for ((i = 0; i < 8; i++)); do
	log_must touch ${dir[micro]}/${names:$((i * 40)):40}
done
log_must [ "$(ls ${dir[fat]} | wc -l)" -eq $FAT_ENTRIES ]
# zdb reads the last synced state.
sync_pool $TESTPOOL1
typeset obj
obj=$(get_objnum ${dir[fat]})
log_must eval "zdb -dddd $FS $obj | grep -q 'Fat ZAP stats'"
for d in micro empty; do
	obj=$(get_objnum ${dir[$d]})
	log_must eval "zdb -dddd $FS $obj | grep -q 'microzap:'"
done
log_must save_names $VDIR/n0

log_must save_meta $VDIR/m0
for d in "${!dir[@]}"; do
	log_must [ "$(special_dvas zap $VDIR/m0/$d)" -gt 0 ]
	log_must [ "$(special_dvas dnode $VDIR/m0/$d)" -gt 0 ]
done
log_must [ "$(nblocks L0 $VDIR/m0/fat)" -ge 4 ]
log_must [ "$(nblocks L1 $VDIR/m0/fat)" -ge 1 ]

# A snapshot shares the blocks: -S leaves them where they are.
log_must zfs snapshot $FS@snap
log_must zpool set allocating=off $TESTPOOL1 $SPECIAL
log_must zfs rewrite -A -S -r $MNT
# A range only applies to files.
log_must zfs rewrite -A -r -o 4096 $MNT
log_must save_meta $VDIR/m1
for d in "${!dir[@]}"; do
	log_must cmp $VDIR/m0/$d $VDIR/m1/$d
done

# Without the snapshot, -A moves them.
log_must zfs destroy $FS@snap
log_must eval "zfs rewrite -A -r -v $MNT > $VDIR/rewritten"
for d in "${!dir[@]}"; do
	log_must grep -qx "${dir[$d]}" $VDIR/rewritten
done
log_must save_meta $VDIR/m2
for d in "${!dir[@]}"; do
	log_must [ "$(special_dvas zap $VDIR/m2/$d)" -eq 0 ]
	log_must [ "$(special_dvas dnode $VDIR/m2/$d)" -eq 0 ]
done

log_must zpool export $TESTPOOL1
log_must zpool import -d $VDIR $TESTPOOL1
log_must save_names $VDIR/n1
for d in "${!dir[@]}"; do
	log_must cmp $VDIR/n0/$d $VDIR/n1/$d
done
log_must zdb -b $TESTPOOL1

log_pass "zfs rewrite -r rewrites the blocks of directories"
