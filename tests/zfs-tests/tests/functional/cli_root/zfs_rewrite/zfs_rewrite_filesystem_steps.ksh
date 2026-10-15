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

# DESCRIPTION:
#	Verify that zfs rewrite of a file system given by name goes in steps,
#	reported with -v, which an interruption stops, and that it is refused
#	for what can not be written to.
#
# STRATEGY:
#	1. Create a file system with more data than one step rewrites, and
#	   verify that zfs rewrite -v of it reports more than one step, and
#	   that its data are intact.
#	2. Interrupt zfs rewrite of it, with writes slowed down so that it
#	   lasts longer, and verify that it failed, and that it can be run
#	   again.
#	3. Verify that zfs rewrite refuses a range, a snapshot, a read-only
#	   file system, one partially received, an encrypted one whose key is
#	   not loaded, and -P for an encrypted one, which it rewrites without.

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDIR=$TEST_BASE_DIR/zfs_rewrite_filesystem_steps
typeset BIG=$TESTPOOL/big
typeset PART=$TESTPOOL/partial
typeset ENC=$TESTPOOL/encrypted
typeset MNT EMNT

function cleanup
{
	zinject -c all > /dev/null 2>&1
	for ds in $BIG $PART $ENC; do
		datasetexists $ds && destroy_dataset $ds -r
	done
	rm -rf $VDIR
}

function rewrite_fails # errno arguments...
{
	typeset -i want=$1

	shift
	zfs rewrite "$@"
	(($? == want))
}

# Print the checksums of the files under directory $1.
function contents # directory
{
	typeset f

	find $1 -type f | sort | while read -r f; do
		echo "$(xxh128digest $f) $f"
	done
}

log_assert "zfs rewrite of a file system by name goes in steps"

log_onexit cleanup

log_must rm -rf $VDIR
log_must mkdir -p $VDIR
# A name relative to the current directory must not be an existing path.
log_must cd $VDIR

# More than the 64 MiB of a step.
log_must zfs create -o compression=off $BIG
MNT=$(get_prop mountpoint $BIG)
for i in 0 1 2 3; do
	log_must dd if=/dev/urandom of=$MNT/$i bs=1M count=25
done
log_must eval "contents $MNT > $VDIR/c0"
log_must sync_pool $TESTPOOL

log_must eval "zfs rewrite -v $BIG > $VDIR/out"
cat $VDIR/out
log_must grep -q "^$BIG: [0-9]* objects, .* marked\$" $VDIR/out
log_must grep -q "^$BIG: done, [0-9]* objects, 10[0-9]M marked\$" $VDIR/out
log_must eval "contents $MNT > $VDIR/c1"
log_must cmp $VDIR/c0 $VDIR/c1

# An interruption stops it within a step, with an error.  With writes slowed
# down, it lasts longer than the second it is given.
for disk in $(get_disklist $TESTPOOL); do
	log_must zinject -d $disk -D20:1 $TESTPOOL
done
timeout -s INT 1 zfs rewrite -v $BIG > $VDIR/out.int
typeset -i rc=$?
log_must zinject -c all
cat $VDIR/out.int
log_must [ $rc -eq 124 ]
log_mustnot grep -q "done" $VDIR/out.int
log_must zfs rewrite $BIG
log_must eval "contents $MNT > $VDIR/c2"
log_must cmp $VDIR/c0 $VDIR/c2

# A file system can not be rewritten in part.
log_must rewrite_fails 22 -o 4096 $BIG
log_must rewrite_fails 22 -l 4096 $BIG

# What can not be written to.
log_must zfs snapshot $BIG@snap
log_mustnot zfs rewrite $BIG@snap
log_must zfs set readonly=on $BIG
log_must rewrite_fails 30 $BIG
log_must zfs unmount $BIG
log_must rewrite_fails 30 $BIG
log_must zfs set readonly=off $BIG
log_must zfs mount $BIG

log_must eval "zfs send $BIG@snap > $VDIR/stream"
log_must truncate -s 1m $VDIR/stream
log_mustnot eval "zfs receive -s $PART < $VDIR/stream"
log_must datasetexists $PART
log_must rewrite_fails 16 $PART

log_must eval "echo password | zfs create -o encryption=on \
    -o keyformat=passphrase -o keylocation=prompt $ENC"
EMNT=$(get_prop mountpoint $ENC)
log_must dd if=/dev/urandom of=$EMNT/file bs=128k count=4
log_must eval "contents $EMNT > $VDIR/e0"
log_mustnot zfs rewrite -P $ENC
log_must zfs rewrite $ENC
log_must zfs unmount $ENC
log_must zfs unload-key $ENC
log_must rewrite_fails 13 $ENC
log_must eval "echo password | zfs load-key $ENC"
log_must zfs rewrite $ENC
log_must zfs mount $ENC
log_must eval "contents $EMNT > $VDIR/e1"
log_must cmp $VDIR/e0 $VDIR/e1

log_pass "zfs rewrite of a file system by name goes in steps"
