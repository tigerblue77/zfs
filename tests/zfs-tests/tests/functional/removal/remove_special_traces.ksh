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
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	Once a special vdev is removed and no removed vdev is left in the
#	pool configuration, only the pool history tells that the pool had
#	one: feature@allocation_classes is enabled again, zpool status does
#	not report the removal, and the label left on the removed devices
#	can be cleared.
#
# STRATEGY:
#	1. Create a pool on a raidz vdev and add a special mirror, which
#	   makes feature@allocation_classes active.  Remove it while it is
#	   empty: it becomes a hole at once.  Verify that the feature is
#	   enabled again and that zpool status reports no removal.
#	2. Verify that the removed devices have a label of the pool, and
#	   that zpool labelclear erases it.
#	3. Create a pool on a mirror, add a special mirror and write a file,
#	   whose metadata goes to the special mirror.  Remove the special
#	   mirror, which is copied to an indirect vdev.  Verify that its top
#	   ZAP no longer has an allocation bias, that the feature is enabled
#	   again, and that zpool status reports the removal.
#	4. Rewrite the pool, so that nothing refers to the indirect vdev any
#	   more, and wait for it to become a hole.  Verify that zpool status
#	   no longer reports the removal, that the features of the removal
#	   are enabled again and that zdb finds the pool consistent, also
#	   after an export and import.
#

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/remove_special_traces
typeset MNT

function cleanup
{
	poolexists $TESTPOOL && destroy_pool $TESTPOOL
	rm -rf $VDIR
	restore_tunable CONDENSE_INDIRECT_OBSOLETE_PCT
	restore_tunable CONDENSE_MIN_MAPPING_BYTES
	restore_tunable TXG_TIMEOUT
}

#
# Prints the top ZAP object of the top-level vdev with the given id, as
# recorded in the MOS config.
#
function top_vdev_zap # pool id
{
	zdb -C $1 | awk -v id="$2" '
		/^MOS Configuration:/ { mos = 1; next }
		!mos { next }
		/^ *children\[/ && ind == 0 { ind = index($0, "c") }
		/^ *children\[/ && index($0, "c") == ind {
			want = ($1 == "children[" id "]:")
			next
		}
		want && $1 == "com.delphix:vdev_zap_top:" {
			print $2
			exit
		}'
}

function check_feature # feature state
{
	log_must test "$(get_pool_prop feature@$1 $TESTPOOL)" = "$2"
}

function reports_removal
{
	zpool status $TESTPOOL | grep -q "remove:"
}

function check_no_trace
{
	typeset -i i

	# The empty obsolete bpobj, if any, is destroyed in a later txg.
	for ((i = 0; i < 20; i++)); do
		[[ $(get_pool_prop feature@obsolete_counts $TESTPOOL) == \
		    "enabled" ]] && break
		sync_pool $TESTPOOL
	done
	check_feature allocation_classes enabled
	check_feature device_removal enabled
	check_feature obsolete_counts enabled
	log_mustnot reports_removal
	log_must zdb -b $TESTPOOL
}

log_assert "A removed special vdev leaves no trace but the pool history"
log_onexit cleanup

log_must save_tunable CONDENSE_INDIRECT_OBSOLETE_PCT
log_must save_tunable CONDENSE_MIN_MAPPING_BYTES
log_must save_tunable TXG_TIMEOUT

log_must mkdir -p $VDIR
log_must truncate -s $MINVDEVSIZE $VDIR/0 $VDIR/1 $VDIR/2 $VDIR/s0 $VDIR/s1

log_note "An empty special vdev beside a raidz vdev"
# Keep the pool quiet, so that no metadata goes to the special mirror
# between "zpool add" and "zpool set allocating=off".
log_must set_tunable32 TXG_TIMEOUT 5000
log_must zpool create -f $TESTPOOL raidz $VDIR/0 $VDIR/1 $VDIR/2
check_feature allocation_classes enabled
log_must zpool add $TESTPOOL special mirror $VDIR/s0 $VDIR/s1
log_must zpool set allocating=off $TESTPOOL mirror-1
check_feature allocation_classes active
log_must zpool remove $TESTPOOL mirror-1
log_must restore_tunable TXG_TIMEOUT
log_must test "$(top_vdev_type $TESTPOOL 1)" = "hole"
check_no_trace

# The removed devices keep a label of the pool, which zpool labelclear
# erases.
log_must zdb -lq $VDIR/s0
log_must zpool labelclear $VDIR/s0
log_must zpool labelclear $VDIR/s1
log_mustnot zdb -lq $VDIR/s0
log_mustnot zdb -lq $VDIR/s1
log_must destroy_pool $TESTPOOL

log_note "A special vdev that is copied to an indirect vdev"
# Condense the mapping as soon as part of it is obsolete.
log_must set_tunable32 CONDENSE_INDIRECT_OBSOLETE_PCT 0
log_must set_tunable64 CONDENSE_MIN_MAPPING_BYTES 1
log_must zpool create -f -O compression=off $TESTPOOL mirror $VDIR/0 $VDIR/1
log_must zfs create $TESTPOOL/$TESTFS
MNT=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must zpool add $TESTPOOL special mirror $VDIR/s0 $VDIR/s1
log_must dd if=/dev/urandom of=$MNT/file bs=1M count=8
sync_pool $TESTPOOL
log_must test "$(get_vdev_prop allocated $TESTPOOL mirror-1)" -ne 0

log_must zpool remove $TESTPOOL mirror-1
log_must wait_for_removal $TESTPOOL
log_must test "$(top_vdev_type $TESTPOOL 1)" = "indirect"
typeset zap=$(top_vdev_zap $TESTPOOL 1)
log_must test -n "$zap"
log_mustnot eval "zdb -dddd $TESTPOOL $zap | grep -q allocation_bias"
check_feature allocation_classes enabled
check_feature device_removal active
log_must reports_removal

# Rewrite what the removal copied, so that nothing refers to the indirect
# vdev any more.
log_must zpool rewrite $TESTPOOL
log_must wait_for_top_vdev_type $TESTPOOL 1 hole $MNT
check_no_trace

log_must zpool export $TESTPOOL
log_must zpool import -d $VDIR $TESTPOOL
log_must test "$(top_vdev_type $TESTPOOL 1)" = "hole"
check_no_trace

log_must test "$(zpool history -i $TESTPOOL | grep -c "vdev retire")" -eq 1

log_pass "A removed special vdev leaves no trace but the pool history"
