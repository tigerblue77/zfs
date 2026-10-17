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
. $STF_SUITE/tests/functional/replacement/replacement.cfg

#
# DESCRIPTION:
# A resilver that cannot read some blocks from the only device holding them
# must leave them missing on the new device, so that the old device is kept
# until they have been copied.
#
# STRATEGY:
# 1. Create a single-device pool and fill it with files.
# 2. Inject read errors on that device, then attach (or replace it by) a
#    second device and wait for the resilver, which reports errors.
# 3. Clear the injection.  Verify that the old device cannot be detached, or
#    that the replace has not completed.
# 4. Scrub the pool, which copies the missing blocks from the old device.
# 5. Detach the old device, or let the replace complete, and verify that
#    every file is intact.
#

verify_runnable "global"

function cleanup
{
	log_must zinject -c all
	destroy_pool $TESTPOOL1
	rm -f ${VDEV_FILES[0]} $SPARE_VDEV_FILE $TEST_BASE_DIR/sums*
}

log_assert "A resilver that cannot read a block keeps the device holding it"
log_onexit cleanup

for mode in attach replace; do
	log_must truncate -s $VDEV_FILE_SIZE ${VDEV_FILES[0]} $SPARE_VDEV_FILE
	log_must zpool create -f -O compression=off -O primarycache=metadata \
	    $TESTPOOL1 ${VDEV_FILES[0]}
	for i in {1..48}; do
		log_must dd if=/dev/urandom of=/$TESTPOOL1/file.$i bs=1M \
		    count=1 status=none
	done
	log_must sync_pool $TESTPOOL1
	log_must eval "(cd /$TESTPOOL1 && xxh128sum file.*) > $TEST_BASE_DIR/sums"
	log_must zpool export $TESTPOOL1
	log_must zpool import -d $TEST_BASE_DIR $TESTPOOL1

	log_must zinject -d ${VDEV_FILES[0]} -e io -T read -f 50 $TESTPOOL1
	log_must zpool $mode $TESTPOOL1 ${VDEV_FILES[0]} $SPARE_VDEV_FILE
	log_must zpool wait -t resilver $TESTPOOL1
	log_must zinject -c all
	log_must eval "zpool status $TESTPOOL1 | grep -q 'resilvered.*with [1-9]'"

	if [[ $mode == attach ]]; then
		log_mustnot zpool detach $TESTPOOL1 ${VDEV_FILES[0]}
	else
		log_must is_pool_replacing $TESTPOOL1
	fi

	log_must zpool scrub -w $TESTPOOL1
	if [[ $mode == attach ]]; then
		log_must zpool detach $TESTPOOL1 ${VDEV_FILES[0]}
	else
		wait_replacing $TESTPOOL1
		log_mustnot is_pool_replacing $TESTPOOL1
	fi
	log_mustnot eval "zpool status $TESTPOOL1 | grep -q ${VDEV_FILES[0]}"
	log_must zpool export $TESTPOOL1
	log_must zpool import -d $TEST_BASE_DIR $TESTPOOL1
	log_must eval "(cd /$TESTPOOL1 && xxh128sum file.*) > $TEST_BASE_DIR/sums.2"
	log_must cmp $TEST_BASE_DIR/sums $TEST_BASE_DIR/sums.2
	destroy_pool $TESTPOOL1
done

log_pass "A resilver that cannot read a block keeps the device holding it"
