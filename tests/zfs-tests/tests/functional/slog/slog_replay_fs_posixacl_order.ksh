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

. $STF_SUITE/tests/functional/slog/slog.kshlib

#
# DESCRIPTION:
# Verify that neither replay nor a failed setfacl gives a file the mode
# that goes with a POSIX access ACL without that ACL, or the ACL without
# its mode: when a commit for another file writes the removal of an ACL,
# on a pool without feature@zilsaxattr, where ACLs kept as system
# attributes are not logged, and when storing an ACL fails.
#
# STRATEGY:
# 1. Create a pool with feature@zilsaxattr and one without, each with two
#    filesystems with acltype=posix, one with xattr=sa and one with
#    xattr=dir.  On the first pool, also create one with xattr=dir and
#    sync=always where root is over its user quota, so that the xattr
#    directory that would hold an ACL cannot be created.
# 2. Freeze the pools.  On the first, give two 0600 files an ACL that
#    names a user and fsync them, then remove each ACL, one after a
#    chmod, and fsync another file.  On the second, give a 0640 file an
#    ACL and remove the ACL of another file, and fsync both.  Fail to
#    give a 0600 file an ACL where root is over its quota: the file must
#    keep its mode.
# 3. Copy the vdevs, as after a crash, and import the copies, which
#    replays the logs.
# 4. Each file must have a mode and ACLs it had together before the
#    crash: those before or after the removal on the first pool, those
#    after the change with xattr=dir on the second pool, and with
#    xattr=sa, where the ACL is not logged, those before or after it.
#

verify_runnable "global"

typeset -r NAMED_UID=12345
typeset -A want also

function entry_state # path
{
	typeset path=$1

	echo "$(stat -c %a $path) $(getfacl -cnp $path 2>&1 | tr '\n' ' ')"
}

log_assert "Replay keeps the mode and the access ACL of a file together."
log_onexit cleanup
log_must setup

log_must zpool create $TESTPOOL $VDIR/a
log_must zpool create -o feature@zilsaxattr=disabled $TESTPOOL2 $VDIR/b
for xattr in sa dir; do
	mnt=/$TESTPOOL/$xattr
	log_must zfs create -o acltype=posix -o xattr=$xattr $TESTPOOL/$xattr
	log_must touch $mnt/promote $mnt/promote-chmod
	log_must chmod 600 $mnt/promote $mnt/promote-chmod
	# Make sure the filesystem has a ZIL header before freezing.
	log_must dd if=/dev/zero of=$mnt/sync conv=fdatasync,fsync bs=1 count=1

	mnt=/$TESTPOOL2/$xattr
	log_must zfs create -o acltype=posix -o xattr=$xattr $TESTPOOL2/$xattr
	log_must touch $mnt/add $mnt/remove
	log_must chmod 640 $mnt/add
	log_must chmod 600 $mnt/remove
	log_must setfacl -m u:$NAMED_UID:rwx $mnt/remove
	log_must dd if=/dev/zero of=$mnt/sync conv=fdatasync,fsync bs=1 count=1
done
full=/$TESTPOOL/full
log_must zfs create -o acltype=posix -o xattr=dir -o sync=always \
    $TESTPOOL/full
log_must touch $full/acl
log_must chmod 600 $full/acl
log_must dd if=/dev/zero of=$full/sync conv=fdatasync,fsync bs=1 count=1
sync_pool $TESTPOOL
log_must zfs set userquota@root=1 $TESTPOOL/full
log_must test "$(get_pool_prop feature@zilsaxattr $TESTPOOL)" == "active"
log_must test "$(get_pool_prop feature@zilsaxattr $TESTPOOL2)" == "disabled"
log_must zpool freeze $TESTPOOL
log_must zpool freeze $TESTPOOL2

umask 022
typeset -i failed=0
for xattr in sa dir; do
	mnt=/$TESTPOOL/$xattr
	for entry in $mnt/promote $mnt/promote-chmod; do
		log_must setfacl -m u:$NAMED_UID:rwx $entry
		log_must sync $entry
		also[$entry]=$(entry_state $entry)
		[[ $entry == *-chmod ]] && log_must chmod 600 $entry
		log_must setfacl -b $entry
		want[$entry]=$(entry_state $entry)
	done
	log_must dd if=/dev/zero of=$mnt/sync conv=notrunc,fsync bs=1 count=1

	mnt=/$TESTPOOL2/$xattr
	for entry in $mnt/add $mnt/remove; do
		[[ $xattr == sa ]] && also[$entry]=$(entry_state $entry)
	done
	log_must setfacl -m u:$NAMED_UID:rw- $mnt/add
	log_must setfacl -b $mnt/remove
	log_must sync $mnt/add $mnt/remove
	for entry in $mnt/add $mnt/remove; do
		want[$entry]=$(entry_state $entry)
	done
done

want[$full/acl]=$(entry_state $full/acl)
log_mustnot setfacl -m u:$NAMED_UID:rwx $full/acl
state=$(entry_state $full/acl)
if [[ $state != "${want[$full/acl]}" ]]; then
	log_note "$full/acl: ${want[$full/acl]} before the failed setfacl," \
	    "$state live after it"
	((failed++))
fi

for entry in "${!want[@]}"; do
	log_note "$entry: ${want[$entry]}${also[$entry]:+ or ${also[$entry]}}"
done

log_must cp --sparse=always $VDIR/a $VDIR2/a
log_must cp --sparse=always $VDIR/b $VDIR2/b
log_must zpool export $TESTPOOL
log_must zpool export $TESTPOOL2
log_must zpool import -f -d $VDIR2 $TESTPOOL
log_must zpool import -f -d $VDIR2 $TESTPOOL2

for entry in "${!want[@]}"; do
	state=$(entry_state $entry)
	[[ $state == "${want[$entry]}" ]] && continue
	[[ -n ${also[$entry]} && $state == "${also[$entry]}" ]] && continue
	log_note "$entry: $state after replay"
	((failed++))
done
((failed == 0)) || log_fail "$failed entries in a state they never had"

log_pass "Replay keeps the mode and the access ACL of a file together."
