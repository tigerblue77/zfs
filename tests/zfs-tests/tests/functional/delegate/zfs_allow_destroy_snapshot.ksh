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

. $STF_SUITE/tests/functional/delegate/delegate_common.kshlib

#
# DESCRIPTION:
#	Verify which delegations let a user destroy snapshots.
#
# STRATEGY:
#	1. Create a file system with no children, and a snapshot of it.
#	2. With 'destroy' and 'mount' allowed to the file system and its
#	   descendents (the default), the user can destroy both the
#	   snapshot and the file system.
#	3. With only 'destroy' or only 'mount', the user can destroy neither.
#	4. With 'destroy' and 'mount' allowed to the descendents only (-d),
#	   the user can destroy the snapshot and any child file system, but
#	   not the file system itself, since snapshots are checked as
#	   descendents of their file system.
#	5. With 'destroy' and 'mount' allowed locally only (-l), the user can
#	   destroy the file system, but not its snapshots.
#
# The file system is not mounted, to test only the delegation checks.
#

verify_runnable "both"

log_assert "Verify which delegations let a user destroy snapshots."
log_onexit restore_root_datasets

typeset fs=$ROOT_TESTFS/leaf
typeset snap=$fs@snap

# Start from a file system with no children, and one snapshot.
function reset_fs
{
	datasetexists $fs && destroy_dataset $fs "-Rf"
	log_must zfs create -o canmount=noauto $fs
	log_must zfs snapshot $snap
}

# 'destroy' and 'mount' for the file system and its descendents (default).
reset_fs
log_must zfs allow $STAFF1 destroy,mount $fs
log_must user_run $STAFF1 zfs destroy $snap
log_mustnot datasetexists $snap
log_must user_run $STAFF1 zfs destroy $fs
log_mustnot datasetexists $fs

# Both 'destroy' and 'mount' are required.
for perm in destroy mount; do
	reset_fs
	log_must zfs allow $STAFF1 $perm $fs
	log_mustnot user_run $STAFF1 zfs destroy $snap
	log_must datasetexists $snap
	log_must zfs destroy $snap
	log_mustnot user_run $STAFF1 zfs destroy $fs
	log_must datasetexists $fs
done

# Descendents only: the snapshots and children, but not the file system.
reset_fs
log_must zfs allow -d $STAFF1 destroy,mount $fs
log_must user_run $STAFF1 zfs destroy $snap
log_mustnot datasetexists $snap
log_mustnot user_run $STAFF1 zfs destroy $fs
log_must datasetexists $fs
log_must zfs create -o canmount=noauto $fs/child
log_must user_run $STAFF1 zfs destroy $fs/child
log_mustnot datasetexists $fs/child
log_must datasetexists $fs

# Locally only: the file system, but not its snapshots.
reset_fs
log_must zfs allow -l $STAFF1 destroy,mount $fs
log_mustnot user_run $STAFF1 zfs destroy $snap
log_must datasetexists $snap
log_must zfs destroy $snap
log_must user_run $STAFF1 zfs destroy $fs
log_mustnot datasetexists $fs

log_must restore_root_datasets

log_pass "Verified which delegations let a user destroy snapshots."
