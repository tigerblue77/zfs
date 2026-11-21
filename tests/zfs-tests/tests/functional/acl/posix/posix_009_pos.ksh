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
. $STF_SUITE/tests/functional/acl/acl_common.kshlib

#
# DESCRIPTION:
#	With acltype=posix, a setfacl that fails leaves the mode of the file
#	as it was.
#
# STRATEGY:
#	1. Create a file system with xattr=dir, and put a user over their
#	   quota in it.
#	2. As that user, add an entry to the ACL of a 0600 file of theirs.
#	   The xattr object for the ACL cannot be created, so setfacl fails.
#	3. Verify that the mode, the owner and the ACL of the file are
#	   what they were.
#	4. Remove the quota and verify that the same setfacl succeeds: the
#	   quota is what failed it.
#

verify_runnable "global"

typeset fs=$TESTPOOL/$TESTFS/quota
typeset dir=$TESTDIR/quota

function cleanup
{
	datasetexists $fs && destroy_dataset $fs
}

# Print the mode, the owner and the ACL.  Fail if any cannot be read.
function acl_state # path
{
	typeset s a

	s=$(stat -c '%a %U:%G' $1) || return 1
	a=$(getfacl -cpn $1 2>/dev/null) || return 1
	echo "$s $a"
}

log_assert "A setfacl that fails leaves the mode as it was"
log_onexit cleanup

log_must zfs create -o xattr=dir $fs
log_must chmod 777 $dir
log_must user_run $ZFS_ACL_STAFF1 dd if=/dev/urandom of=$dir/big bs=1M count=8
log_must user_run $ZFS_ACL_STAFF1 touch $dir/file
log_must user_run $ZFS_ACL_STAFF1 chmod 600 $dir/file
log_must zfs set userquota@$ZFS_ACL_STAFF1=1M $fs
sync_pool $TESTPOOL

typeset before after

before=$(acl_state $dir/file) || log_fail "Cannot read the state of $dir/file"
[[ ${before%% *} == 600 ]] || log_fail "$dir/file is ${before%% *}"
log_mustnot user_run $ZFS_ACL_STAFF1 setfacl -m u:root:rwx $dir/file
after=$(acl_state $dir/file) ||
    log_fail "Cannot read the state of $dir/file after the failed setfacl"
[[ "$after" == "$before" ]] ||
    log_fail "The failed setfacl left the mode at ${after%% *}: $after"

log_must zfs set userquota@$ZFS_ACL_STAFF1=none $fs
log_must user_run $ZFS_ACL_STAFF1 setfacl -m u:root:rwx $dir/file
after=$(acl_state $dir/file) ||
    log_fail "Cannot read the state of $dir/file after setfacl"
[[ ${after%% *} == 670 && "$after" == *user:0:rwx* ]] ||
    log_fail "Without the quota, setfacl left: $after"

log_pass "A setfacl that fails leaves the mode as it was"
