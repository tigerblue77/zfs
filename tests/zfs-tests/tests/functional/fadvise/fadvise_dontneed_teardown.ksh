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
#	posix_fadvise(POSIX_FADV_DONTNEED) on a file whose mapped pages are
#	dirtied and written back must not deadlock with a writer of the file
#	system's teardown lock.  fadvise holds the lock as reader while it
#	writes the file's dirty pages out and waits for their page locks, so
#	writeback must not wait for the lock with a page locked: once a
#	writer waits for the lock, no new reader gets in.
#
#	The writer here is "zfs set defaultuserquota".  A rollback or a
#	forced receive takes the lock the same way, but zfs_resume_fs() then
#	drops the file's pages with the lock held, and so waits for the
#	pages that page faults lock before they wait for the lock.
#
# STRATEGY:
#	1. Create a 64 MiB file, and keep dirtying random pages of it
#	   through a shared mapping.
#	2. In four processes, call posix_fadvise(POSIX_FADV_DONTNEED) on
#	   the whole file in a loop.
#	3. Sync the file system every 20 ms, so that the flusher keeps
#	   writing the mapped pages back.
#	4. For 30 seconds, set defaultuserquota again and again.  Each set
#	   must return within 30 seconds.  If one has not, nothing short of
#	   a reboot releases it: fail, and leave the file system alone.
#	5. Check that the load was still running.
#

verify_runnable "global"

# Outside the file system under test, which a hang freezes.
OUTDIR=$TEST_BASE_DIR/fadvise_dontneed_teardown
FILE=$TESTDIR/$TESTFILE0
typeset load_pids=""
typeset sync_pid=""
typeset set_pid=""

typeset writer='
import mmap, os, random, sys
fd = os.open(sys.argv[1], os.O_RDWR)
m = mmap.mmap(fd, 0)
n = len(m) // mmap.PAGESIZE
while True:
    for _ in range(256):
        m[random.randrange(n) * mmap.PAGESIZE] = random.randrange(256)
'

typeset adviser='
import os, sys
while True:
    fd = os.open(sys.argv[1], os.O_RDONLY)
    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    os.close(fd)
'

function any_alive # <pid> ...
{
	typeset pid

	for pid in "$@"; do
		kill -0 $pid 2>/dev/null && return 0
	done
	return 1
}

function cleanup
{
	typeset -i n=0

	rm -f $OUTDIR/syncing
	[[ -n $load_pids ]] && kill -9 $load_pids 2>/dev/null
	if [[ -n $set_pid ]] && kill -0 $set_pid 2>/dev/null; then
		log_note "zfs set (pid $set_pid) is still blocked;" \
		    "only a reboot releases $TESTPOOL/$TESTFS"
		return
	fi
	while any_alive $load_pids $sync_pid && ((n < 300)); do
		sleep 0.1
		((n += 1))
	done
	if any_alive $load_pids $sync_pid; then
		log_note "the load is still running; leaving $TESTDIR alone"
		return
	fi
	log_must zfs set defaultuserquota=none $TESTPOOL/$TESTFS
	log_must rm -f $FILE
	log_must rm -rf $OUTDIR
}

log_onexit cleanup

log_assert "fadvise(POSIX_FADV_DONTNEED) of dirty mapped pages does not" \
    "deadlock with a writer of the teardown lock"

log_must rm -rf $OUTDIR
log_must mkdir -p $OUTDIR
log_must dd if=/dev/urandom of=$FILE bs=1M count=64 status=none

# Keep the output of the load off ours: a blocked child holding it would
# stall the run.
python3 -c "$writer" $FILE > $OUTDIR/writer.log 2>&1 < /dev/null &
load_pids=$!
for i in 1 2 3 4; do
	python3 -c "$adviser" $FILE > $OUTDIR/adviser.$i.log 2>&1 < /dev/null &
	load_pids="$load_pids $!"
done
log_must touch $OUTDIR/syncing
while [[ -e $OUTDIR/syncing ]]; do
	sync -f $TESTDIR
	sleep 0.02
done > /dev/null 2>&1 < /dev/null &
sync_pid=$!

typeset -i sets=0
typeset -F1 t0
typeset -F1 longest=0
typeset -i end
((end = SECONDS + 30))
while ((SECONDS < end)); do
	t0=$SECONDS
	zfs set defaultuserquota=$((2 + sets % 2))G $TESTPOOL/$TESTFS \
	    > $OUTDIR/set.out 2>&1 < /dev/null &
	set_pid=$!
	while kill -0 $set_pid 2>/dev/null && ((SECONDS - t0 < 30)); do
		sleep 0.1
	done
	if kill -0 $set_pid 2>/dev/null; then
		log_fail "zfs set defaultuserquota still running after" \
		    "30 seconds, $sets sets completed before it"
	fi
	wait $set_pid || log_fail "zfs set failed: $(<$OUTDIR/set.out)"
	set_pid=""
	((SECONDS - t0 > longest)) && ((longest = SECONDS - t0))
	((sets += 1))
done
log_note "$sets sets of defaultuserquota in 30 seconds," \
    "the longest in $longest seconds"

for pid in $load_pids; do
	kill -0 $pid 2>/dev/null || log_fail "load process $pid exited:" \
	    "$(cat $OUTDIR/*.log)"
done

log_pass "fadvise(POSIX_FADV_DONTNEED) of dirty mapped pages does not" \
    "deadlock with a writer of the teardown lock"
