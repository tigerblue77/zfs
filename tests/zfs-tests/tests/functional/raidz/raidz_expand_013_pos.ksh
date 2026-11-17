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
#	A raidz expansion whose last copy fails must pause, not complete.
#	The reflow offset advances when a copy is issued, so a copy that
#	fails after the last one was issued leaves it at the end of the
#	vdev while the progress on disk stays at the failed copy.  The
#	expansion must retry from there as long as the copy fails, and
#	complete once it succeeds.
#
# STRATEGY:
#	1. Create a raidz1 pool with two metaslabs on the raidz vdev and a
#	   special vdev, which takes all the metadata: the raidz vdev holds
#	   no blocks and gets none.  The reflow then copies nothing but the
#	   last sector of each metaslab, which it always copies.
#	2. Attach a disk with the reflow paused once it has copied anything.
#	   It stops after the last sector of the first metaslab, before the
#	   last sector of the vdev: that is the only copy left.
#	3. Inject write errors on every child of the raidz vdev and let the
#	   reflow go on.  Its last copy fails.
#	4. Verify that the expansion paused at that copy and is still in
#	   progress, also after a clear with the errors still injected.
#	5. Remove the injection, clear the errors, verify that the expansion
#	   completes, and scrub the pool.
#

verify_runnable "global"

typeset -r devs=4
typeset -r dev_size_mb=300
typeset -r special_size_mb=256
typeset -r ashift=12
typeset -r sector=$((1 << ashift))

typeset -a disks

min_ms_count=$(get_tunable VDEV_MIN_MS_COUNT)
scrub_after_expand=$(get_tunable SCRUB_AFTER_EXPAND)

function cleanup
{
	log_must zinject -c all
	poolexists "$TESTPOOL" && zpool status -v "$TESTPOOL"
	poolexists "$TESTPOOL" && log_must_busy zpool destroy "$TESTPOOL"

	for i in {1..$((devs + 1))}; do
		log_must rm -f "$TEST_BASE_DIR/dev-$i"
	done
	log_must rm -f "$TEST_BASE_DIR/special-1" "$TEST_BASE_DIR/special-2"

	log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 0
	log_must set_tunable32 SCRUB_AFTER_EXPAND $scrub_after_expand
	log_must set_tunable32 VDEV_MIN_MS_COUNT $min_ms_count
}

function expanding # pool
{
	zpool status $1 | grep -q 'expansion of .* in progress'
}

function reflowed # pool
{
	zpool status -jp $1 | jq -r '.pools[].raidz_expand_stats.reflowed'
}

#
# Print the offset and the failed offset of each pause of the reflow
# recorded in the history of the pool.
#
function reflow_pauses # pool
{
	zpool history -i $1 | awk '/ reflow pause / {
		offset = failed = "";
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^offset=/)
				offset = substr($i, 8);
			if ($i ~ /^failed_offset=/)
				failed = substr($i, 15);
		}
		print offset, failed
	}'
}

function wait_reflow_pauses # pool count
{
	typeset -i t0=$SECONDS

	while (( $(reflow_pauses $1 | wc -l) < $2 )); do
		if ((SECONDS - t0 > 60)); then
			log_fail "reflow did not pause $2 times in 60 seconds"
		fi
		sleep 1
	done
}

#
# The latest pause must be at the last sector of the vdev, after the
# reflow has issued every copy.
#
function check_last_pause # pool
{
	typeset offset failed

	reflow_pauses $1 | tail -1 | read offset failed
	log_note "reflow paused at offset $offset, failed offset $failed"
	if (( offset != end || failed != end - sector )); then
		log_fail "reflow paused at offset $offset, failed offset" \
		    "$failed instead of $end and $((end - sector))"
	fi
	expanding $1 || log_fail "expansion completed although its last" \
	    "copy failed"
}

log_onexit cleanup

log_must set_tunable32 SCRUB_AFTER_EXPAND 0
log_must set_tunable32 VDEV_MIN_MS_COUNT 2

# Start from new files: a run killed before its cleanup leaves labels
# behind, and zpool attach refuses a device that names an active pool.
for i in {1..$((devs + 1))}; do
	disks[$i]=$TEST_BASE_DIR/dev-$i
	log_must rm -f ${disks[$i]}
	log_must truncate -s ${dev_size_mb}M ${disks[$i]}
done
log_must rm -f $TEST_BASE_DIR/special-1 $TEST_BASE_DIR/special-2
log_must truncate -s ${special_size_mb}M $TEST_BASE_DIR/special-1 \
    $TEST_BASE_DIR/special-2

log_must zpool create -f -o ashift=$ashift $TESTPOOL \
    raidz1 ${disks[1..$devs]} \
    special mirror $TEST_BASE_DIR/special-1 $TEST_BASE_DIR/special-2
log_must zpool sync $TESTPOOL

alloc=$(zpool list -v -Hp $TESTPOOL | awk '$1 == "raidz1-0" { print $3 }')
[[ $alloc == 0 ]] || log_fail "raidz1-0 holds $alloc bytes instead of none"

label=$(zdb -l ${disks[1]})
ms_shift=$(echo "$label" | awk '$1 == "metaslab_shift:" { print $2; exit }')
asize=$(echo "$label" | awk '$1 == "asize:" { print $2; exit }')
ms_count=$((asize >> ms_shift))
(( ms_count == 2 )) || log_fail "raidz1-0 has $ms_count metaslabs, not 2"
typeset -r end=$((ms_count << ms_shift))

#
# The pause point is compared with what synced txgs have copied, which is
# nothing until the last sector of the first metaslab.  Before it copies
# the last sector of the vdev, the reflow waits for a txg to sync (its
# on-disk progress is too far behind), and then stops at the pause point.
#
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 1
log_must zpool attach $TESTPOOL raidz1-0 ${disks[$((devs + 1))]}
wait_raidz_expand_paused $TESTPOOL
copied=$(reflowed $TESTPOOL)
if (( copied != sector )); then
	log_fail "reflow copied $copied bytes instead of the last sector of" \
	    "the first metaslab"
fi
expanding $TESTPOOL || log_fail "expansion completed before its last copy"

#
# -F leaves alone the probe that a failed write starts, so that the child
# is not faulted.  Writes to the labels are never injected.
#
for disk in "${disks[@]}"; do
	log_must zinject -d $disk -e io -T write -F $TESTPOOL
done
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 0
wait_reflow_pauses $TESTPOOL 1
check_last_pause $TESTPOOL

# A clear resumes the reflow, which fails again while the error persists.
pauses=$(reflow_pauses $TESTPOOL | wc -l)
log_must zpool clear $TESTPOOL
wait_reflow_pauses $TESTPOOL $((pauses + 1))
check_last_pause $TESTPOOL

log_must zinject -c all
log_must zpool clear $TESTPOOL
typeset -i t0=$SECONDS
while expanding $TESTPOOL; do
	if ((SECONDS - t0 > 60)); then
		log_fail "expansion did not complete in 60 seconds once the" \
		    "error was cleared"
	fi
	sleep 1
done
log_must zpool sync $TESTPOOL
log_must eval "zpool history -i $TESTPOOL | \
    grep -q 'raidz vdev expansion completed'"

verify_pool $TESTPOOL
log_must check_pool_status $TESTPOOL "scan" "with 0 errors"
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "a raidz expansion whose last copy fails pauses and completes later"
