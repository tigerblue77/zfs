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
#	A leaf can be detached while its initialize thread loads a
#	metaslab.  The thread used to drop the config lock while it loaded
#	the metaslab and translated its free ranges up the vdev tree, so a
#	detach in between cleared the leaf's parent and the translation
#	dereferenced it (#19400).
#
# STRATEGY:
#	1. Create a pool of a mirror and a special vdev: the mirror holds
#	   only file data, and its space maps are read from the special vdev.
#	2. Free every other block of a file, so that the space map of the
#	   mirror's first metaslab spans more than one block.
#	3. Export and import the pool with preloading off, so that this
#	   metaslab is neither loaded nor cached.
#	4. Delay the reads of the special vdev, start initializing a leaf of
#	   the mirror, wait until a read is delayed, and detach the leaf.
#	5. Verify that the detach returns, that the load of the metaslab
#	   waited for the delayed read, and that the leaf is gone.
#

verify_runnable "global"

function cleanup
{
	restore_tunable METASLAB_SM_BLKSZ_NO_LOG
	restore_tunable METASLAB_PRELOAD_ENABLED
	restore_tunable DBGMSG_ENABLE

	# A detach that did not return still holds the pool.
	if [[ -n $detach_pid ]] && kill -0 $detach_pid 2>/dev/null; then
		log_note "zpool detach did not return, leaving $TESTPOOL"
		return
	fi
	zinject -c all >/dev/null 2>&1
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $workdir
}

function delayed_reads
{
	zinject | awk -v ms=$delay_ms '$4 == ms && $5 == 1 { print $7 }'
}

log_assert "A leaf can be detached while its initialize thread loads" \
    "a metaslab"

typeset -i delay_ms=5000
typeset -i i n sm_length load_ms
detach_pid=
workdir=$(mktemp -d $TEST_BASE_DIR/zpool_initialize_detach_load.XXXXXX) ||
    log_fail "cannot create test directory"
log_onexit cleanup

save_tunable METASLAB_SM_BLKSZ_NO_LOG
save_tunable METASLAB_PRELOAD_ENABLED
save_tunable DBGMSG_ENABLE
log_must set_tunable32 DBGMSG_ENABLE 1
#
# A read holds the SCL_ZIO lock until it completes, so the detach waits
# for the reads in flight.  The space map of a metaslab of more than one
# block has an indirect block, read before the data blocks: the detach
# takes the lock between the two.  Without log space maps, the frees are
# written to that space map in their own txg.
#
log_must set_tunable32 METASLAB_SM_BLKSZ_NO_LOG 4096

log_must truncate -s 512M $workdir/a $workdir/b
log_must truncate -s 256M $workdir/special
log_must zpool create -f -o feature@log_spacemap=disabled $TESTPOOL \
    mirror $workdir/a $workdir/b special $workdir/special
log_must zfs create -o compression=lz4 -o recordsize=4k $TESTPOOL/$TESTFS
mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must dd if=/dev/urandom of=$mntpnt/file bs=4k count=2048
sync_pool $TESTPOOL
# The zeroed blocks become holes, which frees them.
log_must stride_dd -i /dev/zero -o $mntpnt/file -b 4096 -c 1024 -s 2
sync_pool $TESTPOOL
# The import reads back the blocks of the last txgs: age the space map.
sync_pool $TESTPOOL
sync_pool $TESTPOOL
sync_pool $TESTPOOL
log_must zpool export $TESTPOOL

sm_length=$(zdb -e -p $workdir -m $TESTPOOL 0 0 |
    awk '/smp_length/ { print $3; exit }')
log_note "space map of metaslab 0 of the mirror: $sm_length bytes"
(( sm_length > 4096 )) ||
    log_fail "space map of metaslab 0 fits in one block: $sm_length bytes"

log_must set_tunable32 METASLAB_PRELOAD_ENABLED 0
log_must zpool import -d $workdir $TESTPOOL
#
# The import queues a configuration update for the async thread, which
# starts after the next txg and holds the namespace lock until it is done.
# The second sync waits for it, so that it can not delay the detach.
#
sync_pool $TESTPOOL
sync_pool $TESTPOOL

log_must zinject -d $workdir/special -D $delay_ms:1 -T read $TESTPOOL
log_must zpool initialize $TESTPOOL $workdir/a
for (( i = 0; i < 300; i++ )); do
	n=$(delayed_reads)
	(( n > 0 )) && break
	sleep 0.1
done
(( i < 300 )) || log_fail "no read of the special vdev was delayed"

zpool detach $TESTPOOL $workdir/a &
detach_pid=$!
for (( i = 0; i < 120; i++ )); do
	kill -0 $detach_pid 2>/dev/null || break
	sleep 1
done
kill -0 $detach_pid 2>/dev/null &&
    log_fail "zpool detach did not return within 120 seconds"
wait $detach_pid || log_fail "zpool detach failed"
detach_pid=
log_must zinject -c all

load=$(kstat dbgmsg | grep "metaslab_load: .*, spa $TESTPOOL, class normal," |
    grep ", vdev_id 0, ms_id 0," | tail -n 1)
log_note "$load"
load_ms=$(echo "$load" |
    sed -n 's/.*, loading_time \([0-9]*\) ms,.*/\1/p')
(( load_ms >= delay_ms )) ||
    log_fail "metaslab 0 was not loaded behind the delayed read"

zpool status -P $TESTPOOL | grep -q "$workdir/a" &&
    log_fail "$workdir/a is still in $TESTPOOL"
log_must check_pool_status $TESTPOOL "state" "ONLINE"

log_pass "A leaf can be detached while its initialize thread loads" \
    "a metaslab"
