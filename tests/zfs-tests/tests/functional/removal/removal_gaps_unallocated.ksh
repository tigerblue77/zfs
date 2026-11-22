#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source. A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

# DESCRIPTION:
#	A removal copies the free gaps between nearby allocated segments
#	together with them, in one allocation on another vdev. Once the copy
#	is written, the gaps must be given back in the txg of the copy, so
#	that they can be allocated again at once. Freeing them instead keeps
#	them from being allocated for TXG_DEFER_SIZE more txgs, and a fast
#	removal can run the other vdev out of space. A gap given back never
#	reaches the space maps of that vdev; a gap freed is recorded there as
#	allocated, then freed.
#
# STRATEGY:
#	1. Create a pool without log space maps, so that the metaslab space
#	   maps keep the allocations and frees of every txg, and with a
#	   special vdev, so that no metadata is allocated on normal vdevs.
#	2. Write a file one 16K block per txg, then overwrite every other
#	   block, leaving after each block that stays a 16K gap, which the
#	   default vdev_removal_max_span lets a copy span.
#	3. Add a second normal vdev and remove the first one. The copies are
#	   the only allocations on the new vdev.
#	4. With zdb, check that the removal copied gaps, and that the new
#	   vdev recorded no free and allocated only the data.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

verify_runnable "global"

log_assert "Removal gives back the free gaps it copies instead of freeing them"

set -A disks $DISKS
if (( ${#disks[@]} < 3 )); then
	log_unsupported "needs at least three disks"
fi

mapout=$TEST_BASE_DIR/mapout.$$
destout=$TEST_BASE_DIR/destout.$$

function cleanup
{
	if poolexists $TESTPOOL ; then
		destroy_pool $TESTPOOL
	fi
	restore_tunable SPA_NUM_ALLOCATORS
	rm -f $mapout $destout
}

#
# Print the sum of the hexadecimal numbers read from stdin.
#
function hexsum
{
	typeset -i sum=0 n
	typeset x

	while read -r x; do
		n=16#$x
		(( sum += n ))
	done
	echo $sum
}

log_onexit cleanup

# With a single allocator, each block lands right after the previous one.
save_tunable SPA_NUM_ALLOCATORS
log_must set_tunable32 SPA_NUM_ALLOCATORS 1

log_must zpool create -f -o feature@log_spacemap=disabled \
    -O compression=off -O recordsize=16k \
    $TESTPOOL ${disks[0]} special ${disks[2]}
mntpnt=$(get_prop mountpoint $TESTPOOL)

typeset -i nblocks=32
typeset -i i
for (( i = 0; i < nblocks; i++ )); do
	log_must dd if=/dev/urandom of=$mntpnt/file bs=16k count=1 seek=$i \
	    conv=notrunc
	sync_pool $TESTPOOL
done
log_must stride_dd -i /dev/urandom -o $mntpnt/file -b 16384 \
    -c $((nblocks / 2)) -s 2 -k 1
sync_pool $TESTPOOL

# ${disks[0]} is vdev 0 and the special vdev is vdev 1, so this is vdev 2.
log_must zpool add $TESTPOOL ${disks[1]}
log_must zpool remove $TESTPOOL ${disks[0]}
log_must wait_for_removal $TESTPOOL
log_mustnot vdevs_in_pool $TESTPOOL ${disks[0]}

log_must eval "zdb -mmmm $TESTPOOL 0 > $mapout"
log_must eval "zdb -mmmm $TESTPOOL 2 > $destout"

mapped=$(sed -n 's/.*vim_bytes_mapped = 0x\([0-9a-f]*\)$/\1/p' $mapout | \
    hexsum)
gaps=$(sed -n 's/.*(\([0-9a-f]*\) obsolete)$/\1/p' $mapout | hexsum)
allocated=$(sed -n 's/.* A range: .* size: \([0-9a-f]*\).*/\1/p' $destout | \
    hexsum)
freed=$(sed -n 's/.* F range: .* size: \([0-9a-f]*\).*/\1/p' $destout | \
    hexsum)
frees=$(grep -c ' F range: ' $destout)
log_note "mapped $mapped bytes with $gaps bytes of gaps;" \
    "vdev 2 allocated $allocated bytes, freed $freed bytes in $frees frees"

(( gaps > 0 )) || log_fail "the removal copied no gap"
(( frees == 0 )) || log_fail "vdev 2 freed $freed bytes in $frees frees" \
    "instead of giving back $gaps bytes of gaps"
(( allocated == mapped - gaps )) || log_fail "vdev 2 allocated" \
    "$allocated bytes for $((mapped - gaps)) bytes of data"

log_pass "Removal gives back the free gaps it copies instead of freeing them"
