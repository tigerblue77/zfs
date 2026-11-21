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
. $STF_SUITE/tests/functional/cli_root/zpool_split/zpool_split.cfg

#
# DESCRIPTION:
#	The pool that 'zpool split' creates keeps no MOS object of the log
#	vdevs that it leaves out: neither their metaslab arrays, nor the
#	space maps those name, nor the object of their unflushed txgs.
#	Its MOS is a copy of the original pool's, which keeps them all.
#
# STRATEGY:
#	For a log that is the last top-level vdev, and for one in the
#	middle, which becomes a hole in the new pool:
#	1. Create a mirrored pool with a mirrored log, and write with
#	   sync=always, so that the log has a metaslab space map and an
#	   object for its unflushed txgs
#	2. Split the pool; the new pool is exported, then export the
#	   original pool as well
#	3. Verify that zdb -d reports no leaked MOS object, and zdb -MC
#	   no space map refcount mismatch, on either pool
#	4. Import the new pool and verify the data written
#

verify_runnable "global"

function cleanup
{
	destroy_pool $TESTPOOL2
	destroy_pool $TESTPOOL
	rm -rf $VDEV_DIR $ZDB_OUT
	rm -fd $altroot
}

#
# Run zdb with the arguments given, and fail on a non-zero exit status or
# on a line that reports a leaked object or a refcount mismatch.
#
function check_zdb
{
	typeset -i rc=0

	zdb "$@" > $ZDB_OUT 2>&1 || rc=$?
	if (( rc != 0 )) || grep -qE 'leaked|refcount mismatch' $ZDB_OUT; then
		grep -E 'leaked|refcount mismatch|unreferenced|smp_' $ZDB_OUT
		tail -n 20 $ZDB_OUT
		log_fail "zdb $* exited $rc or reported a leak"
	fi
	log_note "zdb $*: no leak"
}

log_assert "'zpool split' frees the MOS objects of the logs it leaves out"
log_onexit cleanup

typeset VDEV_DIR="$TEST_BASE_DIR/zpool_split_log_objects"
typeset ZDB_OUT="$TEST_BASE_DIR/zpool_split_log_objects.zdb"
typeset altroot="$TESTDIR/altroot-$TESTPOOL2"
typeset layout vdevs pool mntpnt digest zdbm
typeset -i smcount

for layout in trailing middle; do
	log_note "Log layout: $layout"
	vdevs="data1 data2 log1 log2"
	[[ $layout == middle ]] && vdevs="$vdevs data3 data4"
	log_must mkdir -p $VDEV_DIR
	for vdev in $vdevs; do
		log_must truncate -s $SPA_MINDEVSIZE $VDEV_DIR/$vdev
	done

	# 1. The log is vdev 1, the last one or followed by another mirror
	log_must zpool create -f -O sync=always $TESTPOOL \
		mirror $VDEV_DIR/data1 $VDEV_DIR/data2 \
		log mirror $VDEV_DIR/log1 $VDEV_DIR/log2
	if [[ $layout == middle ]]; then
		log_must zpool add -f $TESTPOOL \
			mirror $VDEV_DIR/data3 $VDEV_DIR/data4
	fi

	mntpnt=$(get_prop mountpoint $TESTPOOL)
	log_must dd if=/dev/urandom of=$mntpnt/file bs=128k count=32
	sync_pool $TESTPOOL true
	digest=$(xxh128digest $mntpnt/file)

	# zdb -m lists the unflushed txgs object on the vdev's line
	zdbm=$(zdb -m $TESTPOOL | awk '
	    /^\tvdev / { v = $2; if (v == 1 && /ms_unflushed_phys/) u = 1 }
	    v == 1 && /^\tmetaslab / && $6 != 0 { n++ }
	    END { print (u ? "yes" : "no"), n + 0 }')
	log_note "Log unflushed txgs object, metaslab space maps: $zdbm"
	log_must test "${zdbm% *}" = "yes"
	smcount=${zdbm#* }
	log_must test $smcount -gt 0

	# 2. Without -R, the new pool is exported after the split
	log_must zpool split $TESTPOOL $TESTPOOL2
	log_mustnot poolexists $TESTPOOL2
	log_must zpool export $TESTPOOL

	# 3. Neither pool keeps an object that nothing refers to
	for pool in $TESTPOOL2 $TESTPOOL; do
		check_zdb -e -p $VDEV_DIR -d $pool
		check_zdb -e -p $VDEV_DIR -MC $pool
	done

	# 4. The new pool imports with the data written
	log_must zpool import -d $VDEV_DIR -R $altroot $TESTPOOL2
	mntpnt=$(get_prop mountpoint $TESTPOOL2)
	log_must test "$(xxh128digest $mntpnt/file)" = "$digest"

	cleanup
done

log_pass "'zpool split' frees the MOS objects of the logs it leaves out"
