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
#	A rollback or receive must not deadlock with page faults and
#	writeback on files of the filesystem whose contents it replaces, and
#	must not write back pages that were dirty before it.
#
# STRATEGY:
#	1. Create four files and snapshot the filesystem. Save an incremental
#	   stream that changes two of the files.
#	2. Dirty a page of the fourth file through a mapping, roll back, and
#	   verify that the file holds the snapshot's data after a sync.
#	3. In the background, keep faulting in the first file after dropping
#	   its cached pages, keep touching random pages of the second one,
#	   keep dirtying the third one and writing it back, and keep syncing
#	   the filesystem.
#	4. Once all of them are running, alternately roll back to the
#	   snapshot and receive the stream over and over, and require each
#	   of them to finish within two minutes.
#

verify_runnable "global"

typeset fs=$TESTPOOL/$TESTFS
typeset snap=$fs@mmap_rollback_deadlock
typeset snap2=${snap}_2
typeset base=mmap_rollback_deadlock
typeset log=$TEST_BASE_DIR/$base
typeset -i runtime=30
typeset pids=""
typeset rpid=""
typeset spid=""
typeset hung=""

function running # pid
{
	[[ -d /proc/$1 ]] && [[ $(awk '{print $3}' /proc/$1/stat) != Z ]]
}

function wait_exit # pid seconds
{
	typeset -i n=0

	while running $1 && ((n++ < $2 * 10)); do
		sleep 0.1
	done
	! running $1
}

function cleanup
{
	typeset pid

	[[ -n $pids$spid ]] && kill $pids $spid 2>/dev/null
	# Waiting for tasks stuck in a deadlock, or cleaning up after them,
	# would hang as well.
	[[ -n $hung ]] && return
	for pid in $rpid $pids $spid; do
		wait_exit $pid 60 || return
	done
	wait
	datasetexists $snap2 && destroy_dataset $snap2
	datasetexists $snap && destroy_dataset $snap
	rm -f $TESTDIR/$base.* $log.*
}

#
# Run a command in the background with its input from the given file, and fail
# if it does not finish within two minutes or fails.
#
function run_bounded # description input command...
{
	typeset what=$1
	typeset input=$2
	typeset pid

	shift 2
	"$@" <$input >$log.op 2>&1 &
	rpid=$!
	if ! wait_exit $rpid 120; then
		hung=$rpid
		for pid in $rpid $pids $spid; do
			log_note "$pid: $(cat /proc/$pid/stack 2>/dev/null)"
		done
		log_fail "$what did not finish within two minutes"
	fi
	wait $rpid || log_fail "$what failed: $(<$log.op)"
}

function wait_ready # mode log
{
	typeset -i n=0

	until grep -q ready $2; do
		((n++ < 600)) || log_fail "mmap_fault_loop $1 did not start"
		sleep 0.1
	done
}

log_assert "A rollback or receive must not deadlock with page faults and" \
    "writeback, or write back pages dirtied before it"
log_onexit cleanup

log_must dd if=/dev/urandom of=$TESTDIR/$base.cold bs=1M count=16
log_must dd if=/dev/urandom of=$TESTDIR/$base.hot bs=1M count=16
log_must dd if=/dev/urandom of=$TESTDIR/$base.dirty bs=1M count=8
log_must dd if=/dev/urandom of=$TESTDIR/$base.poke bs=128k count=1
log_must zfs snapshot $snap
typeset sum=$(xxh128digest $TESTDIR/$base.poke)

for mode in cold hot; do
	log_must dd if=/dev/urandom of=$TESTDIR/$base.$mode bs=1M count=1 \
	    conv=notrunc
done
log_must zfs snapshot $snap2
log_must eval "zfs send -i $snap $snap2 >$log.stream"
log_must zfs rollback -r $snap

mmap_fault_loop poke $TESTDIR/$base.poke >$log.poke 2>&1 &
pids=$!
wait_ready poke $log.poke
[[ $(xxh128digest $TESTDIR/$base.poke) != $sum ]] ||
    log_fail "writing through the mapping did not change the file"
run_bounded "rollback" /dev/null zfs rollback $snap
kill $pids
if ! wait_exit $pids 60; then
	hung=$pids
	log_fail "mmap_fault_loop poke did not stop"
fi
wait $pids || log_fail "mmap_fault_loop poke failed: $(<$log.poke)"
pids=""
log_must sync -f $TESTDIR
[[ $(xxh128digest $TESTDIR/$base.poke) == $sum ]] ||
    log_fail "a page dirtied before the rollback was written back"

for mode in cold hot dirty; do
	mmap_fault_loop $mode $TESTDIR/$base.$mode >$log.$mode 2>&1 &
	pids="$pids $!"
done
for mode in cold hot dirty; do
	wait_ready $mode $log.$mode
done
(while true; do sync -f $TESTDIR; sleep 0.05; done) &
spid=$!

typeset -i n=0
SECONDS=0
while ((SECONDS < runtime)); do
	if ((n % 2 == 0)); then
		run_bounded "rollback $n" /dev/null zfs rollback -r $snap
	else
		run_bounded "receive $n" $log.stream zfs receive -F $fs
	fi
	((n++))
done

kill $pids $spid
wait_exit $spid 60 || log_fail "the sync loop did not stop"
spid=""
for pid in $pids; do
	if ! wait_exit $pid 60; then
		hung=$pid
		log_fail "mmap_fault_loop $pid did not stop"
	fi
	wait $pid ||
	    log_fail "mmap_fault_loop $pid failed: $(cat $log.{cold,hot,dirty})"
done
pids=""

log_pass "$n rollbacks and receives did not deadlock with page faults and" \
    "writeback"
