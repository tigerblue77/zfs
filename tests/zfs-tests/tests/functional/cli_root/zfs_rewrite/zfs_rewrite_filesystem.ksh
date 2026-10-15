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
#	Verify that zfs rewrite of a file system given by name rewrites all
#	of it, mounted or not: the files and directories, those that no path
#	reaches included, the objects of the file system itself, the blocks
#	of dnodes, the accounting objects and the block of the objset, as the
#	flags allow, and leaves the files in the delete queue alone; and that
#	zfs rewrite of a whole volume rewrites all of it.
#
# STRATEGY:
#	1. Create a pool with a raidz vdev and a special mirror, which takes
#	   the metadata and the small file blocks, and two file systems with
#	   files of a few sizes, a fat directory, extended attributes in spill
#	   blocks, and files of other users, groups and projects, with quotas.
#	   In the first one, which stays mounted, hide a file under the mount
#	   of another file system, and keep a file open once it is removed.
#	   Unmount the second one.  Create a volume whose blocks all go to
#	   the special mirror.  Verify that each has blocks on the special
#	   mirror, the objset block and the accounting objects included.
#	2. Take a snapshot of the first one, set allocating=off on the
#	   special mirror, and verify that zfs rewrite -A -S of it moves
#	   nothing and reports the blocks left alone.
#	3. Destroy the snapshot, and verify that zfs rewrite -A of each file
#	   system leaves nothing on the special mirror but the blocks of the
#	   removed file, which it reports as left alone, and that a second
#	   zfs rewrite -A rewrites nothing, and that zfs rewrite -A of the
#	   volume leaves nothing of it there.
#	4. Verify the contents of the file systems after an export and
#	   import, and that zdb finds no leak.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zfs_rewrite_filesystem
typeset FS1=$TESTPOOL1/fs1
typeset FS2=$TESTPOOL1/fs2
typeset COVER=$TESTPOOL1/cover
typeset VOL=$TESTPOOL1/vol
typeset VOLDEV=$ZVOL_DEVDIR/$VOL
typeset XNAME=rewrite
typeset XVAL
typeset SPECIAL SPECIAL_ID MNT1 MNT2 VOLHASH
typeset -i UNLINKED_OBJ HIDDEN_OBJ

function cleanup
{
	exec 5<&-
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
# Print the number of DVAs on the special mirror of the blocks of each
# object of dataset $1 that has some, as "object count", the accounting
# objects being -1 to -3 and the meta-dnode 0, and as "root count" that of
# the block of the objset, from what zdb prints of them.
#
function special_objects # dataset
{
	zpool sync $TESTPOOL1 || return 1
	zdb -dddddd $1 | awk -v id=$SPECIAL_ID '
	    function dvas(  n, i, d) {
	        for (i = 1; i <= NF; i++)
	            if (split($i, d, ":") == 3 && d[1] == id) n++
	        return n + 0
	    }
	    /^Dataset / {
	        s = $0
	        while (match(s, /DVA\[[0-9]+\]=<[0-9]+:/)) {
	            v = substr(s, RSTART, RLENGTH)
	            sub(/.*</, "", v); sub(/:/, "", v)
	            if (v == id) c["root"]++
	            s = substr(s, RSTART + RLENGTH)
	        }
	        next
	    }
	    $1 == "Object" && $2 == "lvl" { hdr = 1; next }
	    hdr { obj = $1; hdr = 0; next }
	    /^Spill block:/ { c[obj] += dvas() }
	    /^$/ { found = 0 }
	    found { c[obj] += dvas() }
	    /^Indirect blocks:/ { found = 1 }
	    END { for (o in c) if (c[o] > 0) print o, c[o] }' | sort -k1,1
}

# Print the checksums of the files under directory $1, and their xattrs.
function contents # directory
{
	typeset f

	find $1 -type f | sort | while read -r f; do
		echo "$(xxh128digest $f) $f $(get_xattr $XNAME $f 2>/dev/null | \
		    xxh128digest)"
	done
}

#
# Fill file system $1, mounted at $2: small files, whose data go to the
# special mirror, large ones, a fat directory, extended attributes too large
# for the dnode, which go to spill blocks, and files of other users, groups
# and projects, which have quotas.
#
function fill # filesystem mountpoint
{
	typeset names
	typeset -i i

	log_must zfs set userquota@1234=100m groupquota@5678=100m \
	    projectquota@42=100m $1
	log_must mkdir $2/small $2/large $2/fat $2/owned
	for ((i = 0; i < 20; i++)); do
		log_must dd if=/dev/urandom of=$2/small/$i bs=4k count=$((i % 4 + 1))
	done
	for ((i = 0; i < 4; i++)); do
		log_must dd if=/dev/urandom of=$2/large/$i bs=128k count=$((i + 2))
	done
	names=$(od -An -tx1 -N$((600 * 30)) /dev/urandom | tr -d ' \n')
	for ((i = 0; i < 600; i++)); do
		: > $2/fat/${names:$((i * 60)):60} || \
		    log_fail "Could not create entry $i in $2/fat"
	done
	for ((i = 0; i < 4; i++)); do
		log_must set_xattr $XNAME "$XVAL" $2/small/$i
	done
	for ((i = 0; i < 4; i++)); do
		log_must dd if=/dev/urandom of=$2/owned/$i bs=4k count=2
		log_must chown 1234:5678 $2/owned/$i
		if is_linux; then
			log_must zfs project -p 42 $2/owned/$i
		fi
	done
}

log_assert "zfs rewrite of a file system by name rewrites all of it"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
# A name relative to the current directory must not be an existing path.
log_must cd $VDIR
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

# Random, so that it is not compressed into a block too small to spill.
XVAL=$(od -An -tx1 -N750 /dev/urandom | tr -d ' \n')
for fs in $FS1 $FS2; do
	log_must zfs create -o xattr=sa -o dnodesize=legacy -o atime=off \
	    -o special_small_blocks=16k $fs
done
MNT1=$(get_prop mountpoint $FS1)
MNT2=$(get_prop mountpoint $FS2)
fill $FS1 $MNT1
fill $FS2 $MNT2

# A file that only the mount point of another file system hides.
log_must mkdir $MNT1/covered
log_must dd if=/dev/urandom of=$MNT1/covered/hidden bs=4k count=2
HIDDEN_OBJ=$(get_objnum $MNT1/covered/hidden)
log_must zfs create -o mountpoint=$MNT1/covered $COVER
log_mustnot test -e $MNT1/covered/hidden

# A file still open once removed, which stays in the delete queue.
log_must dd if=/dev/urandom of=$MNT1/unlinked bs=4k count=2
UNLINKED_OBJ=$(get_objnum $MNT1/unlinked)
exec 5< $MNT1/unlinked
log_must rm $MNT1/unlinked

log_must eval "contents $MNT2 > $VDIR/c2.0"
log_must zfs unmount $FS2

log_must zfs create -V 1m -o volblocksize=16k -o special_small_blocks=16k \
    $VOL
block_device_wait $VOLDEV
log_must dd if=/dev/urandom of=$VOLDEV bs=16k count=64 conv=fsync
VOLHASH=$(xxh128digest $VOLDEV)
log_must eval "special_objects $VOL > $VDIR/v.0"
for obj in root 0 1 2; do
	log_must grep -q "^$obj " $VDIR/v.0
done

log_must eval "special_objects $FS1 > $VDIR/s1.0"
log_must eval "special_objects $FS2 > $VDIR/s2.0"
for s in $VDIR/s1.0 $VDIR/s2.0; do
	log_note "$s: $(wc -l < $s) objects with blocks on the special mirror"
	for obj in root -1 -2 -3 0 1; do
		log_must grep -q "^$obj " $s
	done
done
log_must grep -q "^$HIDDEN_OBJ " $VDIR/s1.0
log_must grep -q "^$UNLINKED_OBJ " $VDIR/s1.0

# A snapshot shares the blocks: -S leaves them where they are.
log_must zfs snapshot $FS1@snap
log_must zpool set allocating=off $TESTPOOL1 $SPECIAL
log_must eval "zfs rewrite -A -S -v $FS1 > $VDIR/out.snap"
cat $VDIR/out.snap
log_must eval "special_objects $FS1 > $VDIR/s1.1"
log_must cmp $VDIR/s1.0 $VDIR/s1.1
log_must grep -q "^$FS1: done" $VDIR/out.snap
log_must grep -q "^$FS1: left alone: [1-9][0-9]* blocks shared with snapshots" \
    $VDIR/out.snap

# Without it, -A moves everything but the removed file, still open.
log_must zfs destroy $FS1@snap
log_must eval "zfs rewrite -A -v $FS1 > $VDIR/out.1"
cat $VDIR/out.1
log_must eval "special_objects $FS1 > $VDIR/s1.2"
cat $VDIR/s1.2
log_must [ "$(awk '{ print $1 }' $VDIR/s1.2)" = "$UNLINKED_OBJ" ]
log_must grep -q "^$UNLINKED_OBJ " $VDIR/s1.2
log_must grep -q "^$FS1: left alone: .* [1-9][0-9]* unlinked objects" $VDIR/out.1

# Unmounted, the same, without anything left.
log_must eval "zfs rewrite -A -v $FS2 > $VDIR/out.2"
cat $VDIR/out.2
log_must eval "special_objects $FS2 > $VDIR/s2.2"
cat $VDIR/s2.2
log_must [ ! -s $VDIR/s2.2 ]
log_mustnot ismounted $FS2

# Nothing left to do.
log_must eval "zfs rewrite -A -v $FS1 > $VDIR/out.3"
log_must grep -q "^$FS1: done, [0-9]* objects, 0B marked" $VDIR/out.3
log_must eval "special_objects $FS1 > $VDIR/s1.3"
log_must cmp $VDIR/s1.2 $VDIR/s1.3

# The volume, its properties and its objset included.
log_must zfs rewrite -A $VOL
log_must eval "special_objects $VOL > $VDIR/v.1"
cat $VDIR/v.1
log_must [ ! -s $VDIR/v.1 ]

exec 5<&-
log_must zfs unmount $COVER
log_must eval "contents $MNT1 > $VDIR/c1.0"
log_must zpool export $TESTPOOL1
log_must zpool import -d $VDIR $TESTPOOL1
# The import mounts it again.
ismounted $FS2 || log_must zfs mount $FS2
log_must zfs unmount $COVER
block_device_wait $VOLDEV
log_must [ "$(xxh128digest $VOLDEV)" = "$VOLHASH" ]
log_must eval "contents $MNT1 > $VDIR/c1.1"
log_must eval "contents $MNT2 > $VDIR/c2.1"
log_must cmp $VDIR/c1.0 $VDIR/c1.1
log_must cmp $VDIR/c2.0 $VDIR/c2.1
log_must [ "$(zfs get -Hpo value userused@1234 $FS1)" -gt 0 ]
if is_linux; then
	log_must [ "$(zfs get -Hpo value projectused@42 $FS2)" -gt 0 ]
fi
log_must zdb -b $TESTPOOL1

log_pass "zfs rewrite of a file system by name rewrites all of it"
