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
. $STF_SUITE/tests/functional/zvol/zvol_common.shlib
. $STF_SUITE/tests/functional/zvol/zvol_misc/zvol_misc_common.kshlib

#
# DESCRIPTION:
# Removing the minor of a volume must wait for an open of its device that
# has found the volume and not locked it yet, since the open goes on to use
# the zvol_state_t that the removal frees.
#
# STRATEGY:
# 1. Create a volume with volmode=dev and let udev finish with its device.
# 2. Open the device, and hold the open at the zvol_open:found point.
# 3. Set volmode=none, which removes the minor, and wait for the property
#    to change.  "zfs set" returns once the minor is removed and the
#    zvol_state_t freed: it must still be waiting ten seconds later.
# 4. Release the open.  It must fail, and "zfs set" must then return.
# 5. Set volmode=dev again, and read from the new device.
#
# On Linux 5.14 and newer, del_gendisk() waits for the open, which runs
# under the open_mutex that del_gendisk() takes.  On Linux 5.10 and older,
# the open runs under the disk's bd_mutex, which del_gendisk() does not
# take: unless the removal takes it itself, "zfs set" returns while the
# open is held, and the open then locks a freed zvol_state_t.
#

verify_runnable "global"

if ! is_linux; then
	log_unsupported "zvol_open:found is a point of Linux code"
fi
verify_inject_points

typeset vol=$TESTPOOL/removeopen
typeset workdir=""
typeset started=""

#
# Wait up to $2 seconds for the file $1 to appear.
#
function wait_file # file seconds
{
	typeset -i i

	for ((i = 0; i < $2 * 10; i++)); do
		[[ -e $1 ]] && return 0
		sleep 0.1
	done
	[[ -e $1 ]]
}

function show_stacks
{
	typeset p

	for p in $(pgrep -x dd) $(pgrep -x zfs); do
		log_note "$(ps -o pid=,stat=,wchan=,args= -p $p)"
		cat /proc/$p/stack
	done
}

function cleanup
{
	typeset job

	zinject -c all
	for job in $started; do
		wait_file $workdir/$job.rc 60 || show_stacks
	done
	datasetexists $vol && destroy_dataset $vol
	[[ -n $workdir ]] && rm -rf $workdir
	udev_cleanup
}

log_assert "Removing the minor of a volume waits for an open that found it"
log_onexit cleanup

workdir=$(mktemp -d $TEST_BASE_DIR/zvol_misc_remove_open.XXXXXX) || \
    log_fail "Failed to create a work directory"

# 1. A volume with no partitions, which udev has finished probing
log_must zfs create -V 64M -o volmode=dev $vol
block_device_wait $ZVOL_DEVDIR/$vol
znode=$(readlink -f $ZVOL_DEVDIR/$vol)
log_must test -b $znode
zd=${znode##*/}
log_must test -d /sys/block/$zd

# 2. Hold an open of the device after it has found the volume
id=$(zinject -q -n zvol_open:found -A pause $TESTPOOL)
[[ -n $id ]] || log_fail "zinject -n failed"
(
	dd if=$znode of=/dev/null bs=512 count=1 status=none
	echo $? >$workdir/open.rc
) 2>$workdir/open.err &
started="open"
wait_inject_point $id 1 60 || \
    log_fail "the open did not reach zvol_open:found in 60 seconds"
log_note "$(zinject)"

# 3. Remove the minor: the removal must wait for the held open
(
	zfs set volmode=none $vol
	echo $? >$workdir/set.rc
) 2>$workdir/set.err &
started="open set"
typeset -i i
for ((i = 0; i < 300; i++)); do
	[[ $(get_prop volmode $vol) == "none" ]] && break
	sleep 0.1
done
log_must test "$(get_prop volmode $vol)" = "none"
if wait_file $workdir/set.rc 10; then
	log_note "zfs set: rc $(<$workdir/set.rc), $(<$workdir/set.err)"
	log_fail "The minor of $vol was removed while an open held at" \
	    "zvol_open:found had not returned"
fi
log_note "zfs set volmode=none waits for the held open"

# 4. Release the open: it fails, and then the removal ends
log_must zinject -c $id
if ! wait_file $workdir/open.rc 60 || ! wait_file $workdir/set.rc 60; then
	show_stacks
	log_fail "The open or the removal did not return once released"
fi
log_note "open: rc $(<$workdir/open.rc), $(<$workdir/open.err)"
log_must test "$(<$workdir/set.rc)" = "0"
log_mustnot test "$(<$workdir/open.rc)" = "0"
log_mustnot test -e /sys/block/$zd

# 5. A new minor works
log_must zfs set volmode=dev $vol
block_device_wait $ZVOL_DEVDIR/$vol
log_must dd if=$ZVOL_DEVDIR/$vol of=/dev/null bs=512 count=1

log_pass "Removing the minor of a volume waits for an open that found it"
