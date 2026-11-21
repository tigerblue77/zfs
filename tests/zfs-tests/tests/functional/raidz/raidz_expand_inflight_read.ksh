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
# Copyright (c) 2026 Tigerblue77
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	During a raidz expansion, an i/o maps the rows that the reflow has
#	copied, but has not yet synced as copied, to their old location.
#	Once spa_sync() makes the progress of a txg the synced one, the
#	reflow copies other data over that old location, so the update
#	must wait for such an i/o that is still in flight.
#
# STRATEGY:
#	1. Create a raidz1 pool with large metaslabs, so that a file of
#	   128K blocks is written in one piece, attach a new disk with the
#	   reflow paused early on, and pick the first block of the file past
#	   the reflow offset.
#	2. Raise zfs_txg_timeout, so that a txg only syncs when the reflow
#	   asks for it, and hold the sync of the next txg at the
#	   raidz_reflow_sync:committed point, once its progress is recorded.
#	   The reflow resumes and records progress past the block.
#	3. Meanwhile, read the block, and hold the read at the
#	   vdev_raidz_io_start:copied point, once it has taken the expansion
#	   range lock and mapped the block to its old location.
#	4. Release the sync and let the reflow run on.  Give it 20 seconds
#	   to copy other data over the old location of the block, which it
#	   can only do once the sync has made the progress the synced one.
#	5. Release the read.  It must find the data of the block: no
#	   checksum error on any vdev.
#	6. Let the expansion finish, scrub, and verify the file.
#

verify_runnable "global"

typeset -r devs=6
typeset -r dev_size_mb=256
# A directory of our own, so that zdb -e and zpool import only see this pool.
typeset -r devdir=$TEST_BASE_DIR/raidz_expand_inflight_read
typeset -r expect=$TEST_BASE_DIR/raidz_expand_inflight_read.expect
typeset -r got=$TEST_BASE_DIR/raidz_expand_inflight_read.got

typeset -a disks

function cleanup
{
	zinject -c all
	wait
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $devdir $expect $got
	restore_tunable TXG_TIMEOUT
	restore_tunable RAIDZ_EXPAND_MAX_REFLOW_BYTES
	restore_tunable VDEV_MIN_MS_COUNT
	restore_tunable SCRUB_AFTER_EXPAND
	restore_tunable PREFETCH_DISABLE
}

function reflowed # pool
{
	zpool status -jp $1 | jq -r '.pools[].raidz_expand_stats.reflowed'
}

#
# The reflow offset (in the raidz vdev's address space) of the exported pool.
#
function reflow_offset
{
	zdb -e -p $devdir -u $TESTPOOL | \
	    awk '/raidz_reflow/ { sub("off=", "", $3); print $3 }'
}

# Sum of the CKSUM column of every vdev of the pool.
function cksum_errors
{
	zpool status -p $TESTPOOL | awk '
	    !NF { isvdev = 0 }
	    isvdev && NF >= 5 { errors += $5 }
	    /CKSUM$/ { isvdev = 1 }
	    END { print errors + 0 }'
}

log_assert "spa_sync() waits for an i/o to the old location of copied rows"
log_onexit cleanup

log_must save_tunable PREFETCH_DISABLE
log_must save_tunable SCRUB_AFTER_EXPAND
log_must save_tunable VDEV_MIN_MS_COUNT
log_must save_tunable RAIDZ_EXPAND_MAX_REFLOW_BYTES
log_must save_tunable TXG_TIMEOUT
log_must set_tunable32 PREFETCH_DISABLE 1
log_must set_tunable32 SCRUB_AFTER_EXPAND 0
# 512M metaslabs: the file is in one piece in the first one.
log_must set_tunable32 VDEV_MIN_MS_COUNT 2

log_must mkdir -p $devdir
for i in {0..$(($devs - 1))}; do
	device=$devdir/dev-$i
	log_must truncate -s ${dev_size_mb}M $device
	disks[${#disks[*]}+1]=$device
done
log_must truncate -s ${dev_size_mb}M $devdir/dev-$devs

# 1. The data is read from the devices and not from the ARC.
log_must zpool create -f -o cachefile=none -o ashift=12 $TESTPOOL \
    raidz1 ${disks[@]}
log_must zfs create -o recordsize=128k -o compression=off \
    -o primarycache=metadata -o atime=off $TESTPOOL/fs
typeset -r file=/$TESTPOOL/fs/file
log_must dd if=/dev/urandom of=$file bs=1M count=200
typeset -r digest=$(xxh128digest $file)
typeset -r object=$(ls -i $file | awk '{print $1}')

log_must zpool sync $TESTPOOL
allocated=$(get_pool_prop allocated $TESTPOOL)
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES $((allocated / 20))
log_must zpool attach $TESTPOOL raidz1-0 $devdir/dev-$devs
wait_raidz_expand_paused $TESTPOOL
log_must zpool export $TESTPOOL

#
# The reflow resumes at the offset synced on disk, and its next txg records
# progress up to the first row that it could not copy without overwriting
# the old location of a row past that offset: with 6 old disks and 4K
# sectors, offset + (offset / 4K / 6 - 6) * 4K.  The block must lie in
# between.
#
typeset -ri old=$(reflow_offset)
(( old > 0 )) || log_fail "no reflow offset in the uberblock"
typeset -ri new=$(( (old / 4096 + old / 4096 / devs - devs) * 4096 ))
typeset -i xoff=-1 xsize=0 xblk=0
zdb -e -p $devdir -ddddd $TESTPOOL/fs $object | \
    awk '$2 == "L0" { split($3, dva, ":"); print $1, dva[2], dva[3] }' | \
    while read loff off asize; do
	if (( 16#$off >= old && (xoff < 0 || 16#$off < xoff) )); then
		xoff=$((16#$off))
		xsize=$((16#$asize))
		xblk=$((16#$loff / 131072))
	fi
done
log_note "reflow offset $old, next $new, block $xblk at $xoff size $xsize"
(( xoff >= old && xoff + xsize <= new )) || \
    log_fail "no block of the file between offsets $old and $new"

log_must zpool import -o cachefile=none -d $devdir $TESTPOOL
# This also caches the indirect blocks that the read below needs.
log_must dd if=$file of=$expect bs=128k skip=$xblk count=1

# 2. Only the reflow, which waits for each of its txgs, syncs a txg now.
log_must set_tunable32 TXG_TIMEOUT 600
sync_pool $TESTPOOL
typeset syncid=$(zinject -q -w raidz_reflow_sync:committed -s 60 $TESTPOOL)
[[ -n $syncid ]] || log_fail "zinject -w failed"
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES \
    $(($(reflowed $TESTPOOL) + 1))
wait_inject_point $syncid

# 3. The read takes the range lock of the block and maps it with the
# offset that spa_sync() has not yet replaced.
typeset readid=$(zinject -q -w vdev_raidz_io_start:copied -s 60 $TESTPOOL)
[[ -n $readid ]] || log_fail "zinject -w failed"
dd if=$file of=$got bs=128k skip=$xblk count=1 &
typeset -i readpid=$!
wait_inject_point $readid

#
# 4. The data whose new location is the old location of the block is at
# about 7/6 of the block's offset.  The reflow copies at most a sixth past
# the synced offset, so once it has copied 3/2 of the end of the block, that
# data is copied and synced.  It cannot get there while spa_sync() waits
# for the read.
#
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 0
log_must zinject -c $syncid
typeset -ri target=$(( (xoff + xsize) * 3 / 2 ))
typeset -i t0=$SECONDS
while (( $(reflowed $TESTPOOL) < target && SECONDS - t0 < 20 )); do
	sleep 0.5
done
log_note "reflowed $(reflowed $TESTPOOL) (past the block: $target)"
# Another i/o held at the point, e.g. by spa_sync(), would hold the reflow.
typeset -i held=$(zinject | awk -v id=$readid '$1 == id { print $NF }')

# 5. A read of the old location after the copy fails its checksum.
log_must zinject -c $readid
wait $readpid
typeset -i readrc=$?
log_must restore_tunable TXG_TIMEOUT
typeset -i errors=$(cksum_errors)
if (( errors != 0 )); then
	zpool status -v $TESTPOOL
	log_fail "the read found other data at the old location of the" \
	    "block ($errors checksum errors)"
fi
(( held == 1 )) || log_unsupported "$held i/os were held, not the read alone"
(( readrc == 0 )) || log_fail "the read failed ($readrc)"
log_must cmp $expect $got

# 6. Nothing else was overwritten.
log_must zpool wait -t raidz_expand $TESTPOOL
log_must zpool scrub -w $TESTPOOL
log_must eval "zpool status -v $TESTPOOL"
log_must check_pool_status $TESTPOOL "errors" "No known data errors"
log_must check_pool_status $TESTPOOL "scan" "repaired 0B"
(( $(cksum_errors) == 0 )) || log_fail "checksum errors after the scrub"
[[ $(xxh128digest $file) == $digest ]] || log_fail "the file changed"

log_pass "spa_sync() waits for an i/o to the old location of copied rows"
