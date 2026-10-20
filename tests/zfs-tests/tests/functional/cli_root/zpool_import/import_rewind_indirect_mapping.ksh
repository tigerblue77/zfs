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

#
# Copyright (c) 2026 by the OpenZFS project.  All rights reserved.
#

. $STF_SUITE/tests/functional/cli_root/zpool_import/zpool_import.kshlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	A rewind import to a txg taken during a device removal must
#	either succeed or fail cleanly.  It must not trip the VERIFY0()
#	in vdev_indirect_mapping_open() or vdev_indirect_births_open()
#	when the indirect mapping of that txg is no longer readable.
#
# STRATEGY:
#	1. Create a pool of two file vdevs and fragment the second.
#	2. Remove the second vdev in short bursts, pausing the removal
#	   with zfs_removal_suspend_progress, and note the last synced
#	   txg at each pause.  Each burst appends to the mapping object,
#	   so the previous copy of its blocks is freed.
#	3. Let the removal finish, then churn the pool so that the freed
#	   MOS blocks of those txgs are reallocated.
#	4. Export, then "zpool import -T <txg>" read-only for every noted
#	   txg, newest first.
#	5. Every import must return within the timeout.  A hang with
#	   "PANIC at vdev_indirect_mapping.c" (or births) in the kernel
#	   log is the bug.
#
# NOTES:
#	The removed vdev has had its labels wiped once the removal
#	completed, so the old configuration misses one top-level vdev:
#	zfs_max_missing_tvds is raised to 1 for the rewind.
#
#	Nothing guarantees that the freed MOS blocks get reused, so an
#	import that succeeds proves nothing for that txg.  The test only
#	fails on a hang; the summary says how many txgs were really
#	exercised (clean failures).
#
#	zinject is not used: a handler holds spa_inject_ref, which makes
#	"zpool export" return EBUSY, and handlers do not survive the
#	export, so none can act on the reads done by the import itself.
#

verify_runnable "global"

typeset -i IMPORT_TIMEOUT=120
typeset -i BURSTS=8
typeset -i ipid=0
typeset DEVDIR=$TEST_BASE_DIR/rewind_indirect
typeset KEEP=$DEVDIR/keep
typeset GONE=$DEVDIR/gone
typeset POOL=$TESTPOOL1

function wait_import
{
	typeset -i waited=0

	while kill -0 $ipid 2>/dev/null; do
		((waited++ >= IMPORT_TIMEOUT)) && return 1
		sleep 1
	done
	return 0
}

function panic_count
{
	dmesg | grep -cE \
	    'PANIC at vdev_indirect_(mapping|births)\.c:[0-9]+:.*_open\(\)'
}

function custom_cleanup
{
	set_tunable32 REMOVAL_SUSPEND_PROGRESS 0
	set_zfs_max_missing_tvds 0
	if ((ipid > 0)) && ! wait_import; then
		log_note "zpool import is still running: reboot the VM"
		return
	fi
	poolexists $POOL && destroy_pool $POOL
	rm -rf $DEVDIR
}

log_assert "Rewind import across a device removal does not panic"
log_onexit custom_cleanup

log_must mkdir -p $DEVDIR
log_must truncate -s 512m $KEEP $GONE
log_must zpool create -f -O compression=off -O recordsize=8k \
    $POOL $KEEP $GONE

# Many small, scattered segments on $GONE make a mapping of many entries.
log_must mkfile -n 384m /$POOL/file
log_must randwritecomp /$POOL/file 20000
log_must sync_some_data_a_few_times $POOL 5

log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 1
log_must zpool remove $POOL $GONE
sync_pool $POOL
log_must is_pool_removing $POOL

set -A txgs
typeset -i n=0
for i in {1..$BURSTS}; do
	log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 0
	sleep 0.3
	log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 1
	sync_pool $POOL true
	is_pool_removing $POOL || break
	txgs[n]=$(get_last_txg_synced $POOL)
	log_note "burst $i: txg ${txgs[n]}," \
	    "$(zpool status $POOL | grep -E 'copied|evacuated')"
	((n++))
done
((n > 0)) || log_unsupported "removal finished before the first pause"

log_must set_tunable32 REMOVAL_SUSPEND_PROGRESS 0
log_must wait_for_removal $POOL
log_must zpool status -v $POOL

# Reallocate what the removal freed: MOS blocks are only kept alive by
# the TXG_DEFER_SIZE deferral, so many synced txgs of churn reuse them.
for i in {1..30}; do
	log_must randwritecomp /$POOL/file 2000
	dd if=/dev/urandom of=/$POOL/churn.$i bs=128k count=40 2>/dev/null
	((i > 3)) && rm -f /$POOL/churn.$((i - 3))
	sync_pool $POOL true
done

log_must zpool export $POOL
log_must set_zfs_max_missing_tvds 1

typeset -i before=$(panic_count)
typeset -i ok=0 clean=0
typeset -i k
((k = n - 1))
while ((k >= 0)); do
	typeset txg=${txgs[k]}
	zpool import -d $DEVDIR -N -o readonly=on -T $txg $POOL &
	ipid=$!
	if ! wait_import; then
		typeset -i after=$(panic_count)
		dmesg | tail -40
		if ((after > before)); then
			log_fail "import -T $txg panicked in the indirect" \
			    "mapping open (reboot the VM)"
		fi
		log_fail "import -T $txg hung ${IMPORT_TIMEOUT}s (reboot)"
	fi
	wait $ipid
	typeset -i rc=$?
	ipid=0
	if ((rc == 0)); then
		log_note "txg $txg: imported, mapping still readable"
		((ok++))
		log_must zpool export $POOL
	else
		log_note "txg $txg: clean import failure, rc $rc"
		((clean++))
	fi
	((k--))
done

log_note "txgs tried $n: imported $ok, failed cleanly $clean"
((clean == 0)) && log_note "no txg had an unreadable mapping:" \
    "inconclusive, rerun"

# Leave a pool the cleanup can destroy.
set_zfs_max_missing_tvds 0
log_must zpool import -d $DEVDIR -N $POOL

log_pass "Rewind import across a device removal does not panic"
