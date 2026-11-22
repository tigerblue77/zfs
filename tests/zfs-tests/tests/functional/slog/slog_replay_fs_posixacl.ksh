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
# Verify that replay restores the mode and the POSIX ACLs that files,
# directories and FIFOs got when they were created with acltype=posix:
# the umask, or the default ACL of their parent directory.  Verify also
# that it restores the mode that an access ACL gave an existing file.
#
# STRATEGY:
# 1. Create a pool and, for xattr=sa and xattr=dir, three filesystems
#    with acltype=posix: one with sync=always, one where each new
#    entry is fsynced and one where only its parent directory is.
#    In each, create a directory without a default ACL, one whose
#    default ACL only masks the mode, and one whose default ACL also
#    has a named user and a mask, and two files with mode 644.
# 2. Freeze the pool.  With umask 022, create a file, a directory and
#    a FIFO (not where each entry is fsynced) in each directory.
#    Except where only the parent directory is fsynced, give the two
#    files mode 640 with an access ACL, one with a named user and a
#    mask, the other equivalent to the mode.  Record the mode and the
#    ACLs of every entry.
# 3. Copy the vdev, as after a crash, and import the copy, which
#    replays the log.
# 4. Every entry must have the mode and the ACLs it had before the crash.
#

verify_runnable "global"

typeset -r NAMED_UID=12345
typeset -A expect

function entry_state # path
{
	typeset path=$1

	echo "$(stat -c %a $path) $(getfacl -cnp $path 2>&1 | tr '\n' ' ')"
}

log_assert "Replay restores the mode and the ACLs of new and changed entries."
log_onexit cleanup
log_must setup

log_must zpool create $TESTPOOL $VDIR/a
for xattr in sa dir; do
	for how in always entry parent; do
		fs=$TESTPOOL/$xattr-$how
		mnt=/$fs
		log_must zfs create -o acltype=posix -o xattr=$xattr $fs
		[[ $how == always ]] && log_must zfs set sync=always $fs
		log_must mkdir $mnt/none $mnt/equiv $mnt/named
		log_must setfacl -d -m u::rwx,g::r-x,o::--- $mnt/equiv
		log_must setfacl -d -m \
		    u::rwx,u:$NAMED_UID:rwx,g::r-x,m::rwx,o::--- $mnt/named
		log_must touch $mnt/acl-named $mnt/acl-equiv
		log_must chmod 644 $mnt/acl-named $mnt/acl-equiv
		# Make sure the filesystem has a ZIL header before freezing.
		log_must dd if=/dev/zero of=$mnt/sync conv=fdatasync,fsync \
		    bs=1 count=1
	done
done
log_must test "$(get_pool_prop feature@zilsaxattr $TESTPOOL)" == "active"
log_must zpool freeze $TESTPOOL

umask 022
for xattr in sa dir; do
	for how in always entry parent; do
		mnt=/$TESTPOOL/$xattr-$how
		for dir in none equiv named; do
			log_must touch $mnt/$dir/file
			log_must mkdir $mnt/$dir/dir
			[[ $how != entry ]] && log_must mkfifo $mnt/$dir/fifo
			[[ $how == entry ]] && \
			    log_must sync $mnt/$dir/file $mnt/$dir/dir
		done
		if [[ $how != parent ]]; then
			log_must setfacl -m u:$NAMED_UID:r--,m::r--,o::--- \
			    $mnt/acl-named
			log_must setfacl -m o::--- $mnt/acl-equiv
		fi
		[[ $how == entry ]] && \
		    log_must sync $mnt/acl-named $mnt/acl-equiv
		[[ $how == parent ]] && \
		    log_must sync $mnt/none $mnt/equiv $mnt/named
	done
done

#
# Check the modes before the crash: the umask applies without a default
# ACL, which otherwise masks the mode, as an access ACL does.
#
set -A modes none 644 755 equiv 640 750 named 660 770
for xattr in sa dir; do
	for how in always entry parent; do
		mnt=/$TESTPOOL/$xattr-$how
		for ((i = 0; i < ${#modes[@]}; i += 3)); do
			dir=$mnt/${modes[i]}
			log_must test "$(stat -c %a $dir/file)" == ${modes[i+1]}
			log_must test "$(stat -c %a $dir/dir)" == ${modes[i+2]}
			[[ $how == entry ]] || log_must test \
			    "$(stat -c %a $dir/fifo)" == ${modes[i+1]}
			for entry in $dir/file $dir/dir $dir/fifo; do
				[[ -e $entry ]] || continue
				expect[$entry]=$(entry_state $entry)
				log_note "$entry: ${expect[$entry]}"
			done
		done
		[[ $how == parent ]] && continue
		for entry in $mnt/acl-named $mnt/acl-equiv; do
			log_must test "$(stat -c %a $entry)" == 640
			expect[$entry]=$(entry_state $entry)
			log_note "$entry: ${expect[$entry]}"
		done
	done
done

log_must cp --sparse=always $VDIR/a $VDIR2/a
log_must zpool export $TESTPOOL
log_must zpool import -f -d $VDIR2 $TESTPOOL

typeset -i failed=0
for entry in "${!expect[@]}"; do
	if [[ ! -e $entry ]]; then
		log_note "$entry: missing after replay"
		((failed++))
		continue
	fi
	state=$(entry_state $entry)
	if [[ $state != "${expect[$entry]}" ]]; then
		log_note "$entry: ${expect[$entry]} before the crash," \
		    "$state after replay"
		((failed++))
	fi
done
((failed == 0)) || log_fail "$failed entries changed on replay"

log_pass "Replay restores the mode and the ACLs of new and changed entries."
