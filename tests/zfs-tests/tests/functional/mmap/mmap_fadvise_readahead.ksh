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
#	posix_fadvise(POSIX_FADV_WILLNEED) on a file with cached pages must
#	not wait for a writer's range lock while its readahead batch holds
#	locked pages.  A writer that then needs one of those pages in
#	update_pages() deadlocks with it and stops the pool (#19447).
#
# STRATEGY:
#	1. Keep file data out of the ARC (primarycache=metadata) and write
#	   a file of four records.
#	2. Fault in the first page of record 2 through a shared mapping.
#	3. Delay reads on the pool's vdev by DELAY_MS.
#	4. Write the first page of record 1: zfs_write() holds the range
#	   lock while it waits for the delayed read of that record.
#	5. Time fadvise(WILLNEED) over record 1 and the cached page, and
#	   fail if it takes LIMIT_MS or more: it waited for the writer.
#
#	The write covers only the page readahead tries first, which an
#	unfixed module unlocks before it waits, so this cannot deadlock.
#	Every wait is bounded and the delay is cleared after CLEAR_S seconds
#	whatever happens, and again on exit.
#

verify_runnable "global"

if [[ $(linux_version) -lt $(linux_version "5.18") ]]; then
	log_unsupported "Needs address_space_operations->readahead (5.18+)"
fi

typeset TESTFILE=$TESTDIR/mmap_fadvise_readahead.$$
typeset READY=$TESTDIR/mmap_fadvise_readahead.ready.$$
typeset GO=$TESTDIR/mmap_fadvise_readahead.go.$$
typeset OUT=$TESTDIR/mmap_fadvise_readahead.out.$$
typeset VDEV=${DISKS%% *}
typeset -i RECSZ=131072
typeset -i DELAY_MS=5000
typeset -i LIMIT_MS=$((DELAY_MS / 2))
typeset -i CLEAR_S=$((2 * DELAY_MS / 1000 + 2))
typeset -i TIMEOUT_S=120
typeset helper_pid=""

function cleanup
{
	zinject -c all >/dev/null 2>&1
	if [[ -n "$helper_pid" ]]; then
		typeset -i i=0
		while kill -0 $helper_pid 2>/dev/null && ((i < 60)); do
			sleep 1
			((i++))
		done
		kill -9 $helper_pid 2>/dev/null
	fi
	log_must zfs inherit primarycache $TESTPOOL/$TESTFS
	log_must zfs inherit recordsize $TESTPOOL/$TESTFS
	rm -f $TESTFILE $READY $GO $OUT
}

log_assert "fadvise(WILLNEED) readahead does not wait for a writer's range lock"
log_onexit cleanup

log_must zfs set recordsize=$RECSZ $TESTPOOL/$TESTFS
log_must zfs set primarycache=metadata $TESTPOOL/$TESTFS
log_must dd if=/dev/urandom of=$TESTFILE bs=$RECSZ count=4
sync_pool $TESTPOOL

mmap_fadvise_readahead $TESTFILE $RECSZ $READY $GO $LIMIT_MS >$OUT 2>&1 &
helper_pid=$!

typeset -i i=0
while [[ ! -e $READY ]] && kill -0 $helper_pid 2>/dev/null && ((i < 300)); do
	sleep 0.1
	((i++))
done
[[ -e $READY ]] || log_fail "helper did not get ready: $(<$OUT)"

log_must zinject -d $VDEV -D $DELAY_MS:8 -T read $TESTPOOL
log_must touch $GO

#
# An unfixed module's fadvise waits for the writer and then reads the record
# under the delay, so clear the delay after a bounded time either way.
#
typeset -i waited=0
while kill -0 $helper_pid 2>/dev/null && ((waited < TIMEOUT_S)); do
	sleep 1
	((waited++))
	if ((waited == CLEAR_S)); then
		log_must zinject -c all
	fi
done
if kill -0 $helper_pid 2>/dev/null; then
	log_fail "helper still running after ${TIMEOUT_S}s: $(<$OUT)"
fi

wait $helper_pid
typeset -i rc=$?
helper_pid=""
log_must zinject -c all
log_note "$(<$OUT)"

case $rc in
0)
	log_pass "fadvise(WILLNEED) did not wait for the writer's range lock"
	;;
1)
	log_fail "fadvise(WILLNEED) waited for the writer's range lock"
	;;
3)
	log_fail "the read delay did not hold the writer, nothing was tested"
	;;
*)
	log_fail "mmap_fadvise_readahead failed with status $rc"
	;;
esac
