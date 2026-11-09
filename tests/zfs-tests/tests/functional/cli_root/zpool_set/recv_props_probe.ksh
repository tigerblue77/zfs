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
# Copyright (c) 2026 Tigerblue77
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	Measurement only, not for merging: what received dnodesize and
#	compression=zstd values do to large_dnode and zstd_compress, before
#	and after the receive completes, and whether the datasets can be
#	used while the receive is in progress.  Results are log notes
#	starting with "PROBE".
#

verify_runnable "global"

SRC=$TESTPOOL1
DST=$TESTPOOL2
SRC_VDEV=$TEST_BASE_DIR/recv_props_probe_src
DST_VDEV=$TEST_BASE_DIR/recv_props_probe_dst
FULL=$TEST_BASE_DIR/recv_props_probe.full
INCR=$TEST_BASE_DIR/recv_props_probe.incr
FIFO=$TEST_BASE_DIR/recv_props_probe.fifo
ERR=$TEST_BASE_DIR/recv_props_probe.err
HEAD_KB=64
recv_pid=

function cleanup
{
	exec 3>&-
	[[ -n "$recv_pid" ]] && wait $recv_pid
	poolexists $SRC && destroy_pool $SRC
	poolexists $DST && destroy_pool $DST
	rm -f $SRC_VDEV $DST_VDEV $FULL $INCR $FIFO $ERR
}

function probe
{
	log_note "PROBE $*"
}

function state # label
{
	probe "$1: large_dnode=$(get_pool_prop feature@large_dnode $DST)" \
	    "zstd_compress=$(get_pool_prop feature@zstd_compress $DST)"
}

function props # label dataset
{
	probe "$1: $2 $(zfs get -H -o property,value,source,received \
	    dnodesize,compression,mounted $2 2>&1 | tr '\t\n' ' ;')"
}

function try_disable # label feature
{
	if zpool set feature@$2=disabled $DST 2>$ERR; then
		probe "$1: disabling $2 accepted"
		zpool set feature@$2=enabled $DST
	else
		probe "$1: disabling $2 refused: $(cat $ERR)"
	fi
}

function start_recv # stream dataset receive-args...
{
	typeset stream=$1
	typeset ds=$2
	typeset -i i=0
	shift 2

	zfs receive "$@" < $FIFO &
	recv_pid=$!
	exec 3>$FIFO
	dd if=$stream bs=1k count=$HEAD_KB >&3 2>/dev/null
	while ! datasetexists $ds; do
		((i++ < 60)) || log_fail "the receive into $ds did not start"
		sleep 1
	done
	# Received properties are set after the begin sync task.
	sleep 5
	kill -0 $recv_pid || log_fail "the receive into $ds ended early"
}

function end_recv # stream
{
	dd if=$1 bs=1k skip=$HEAD_KB >&3 2>/dev/null
	exec 3>&-
	wait $recv_pid
	probe "receive exit status $?"
	recv_pid=
}

log_assert "Probe of received dnodesize and compression=zstd values"
log_onexit cleanup

log_must truncate -s $MINVDEVSIZE $SRC_VDEV $DST_VDEV
log_must zpool create -f $SRC $SRC_VDEV
log_must zpool create -f $DST $DST_VDEV
log_must mkfifo $FIFO

# Source: dnodesize=1k and compression=zstd set, but no large dnode and no
# zstd block (random data does not compress), so a stream without -c has
# neither the large_dnode nor the zstd flag.
log_must zfs create -o compression=off $SRC/p
mntpnt=$(get_prop mountpoint $SRC/p)
log_must dd if=/dev/urandom of=$mntpnt/data1 bs=1M count=1
log_must zfs snapshot $SRC/p@base
log_must dd if=/dev/urandom of=$mntpnt/data2 bs=1M count=1
log_must zfs set dnodesize=1k $SRC/p
log_must zfs set compression=zstd $SRC/p
log_must zfs snapshot $SRC/p@a
sync_pool $SRC
probe "source: large_dnode=$(get_pool_prop feature@large_dnode $SRC)" \
    "zstd_compress=$(get_pool_prop feature@zstd_compress $SRC)"
log_must eval "zfs send -p $SRC/p@a > $FULL"
log_must eval "zfs send -p -i @base $SRC/p@a > $INCR"
probe "full stream: $(zstream dump $FULL | grep -m2 features | tr '\n' ' ')"
probe "incr stream: $(zstream dump $INCR | grep -m2 features | tr '\n' ' ')"
state "start"

# 1. Completed full receive with the properties
log_must eval "zfs receive $DST/full < $FULL"
state "1 full receive done"
props "1 full receive done" $DST/full
try_disable "1 full receive done" large_dnode
try_disable "1 full receive done" zstd_compress
log_must zfs destroy -r $DST/full
state "1 after destroy"

# 2. Full receive in progress
start_recv $FULL $DST/full2 $DST/full2
state "2 full receive in progress"
props "2 full receive in progress" $DST/full2
if zfs mount $DST/full2 2>$ERR; then
	probe "2 zfs mount accepted"
else
	probe "2 zfs mount refused: $(cat $ERR)"
fi
try_disable "2 full receive in progress" large_dnode
try_disable "2 full receive in progress" zstd_compress
end_recv $FULL
state "2 full receive done"
log_must zfs destroy -r $DST/full2

# 3. Incremental receive in progress into a mounted file system
log_must eval "zfs send $SRC/p@base | zfs receive $DST/inc"
state "3 base received"
props "3 base received" $DST/inc
start_recv $INCR $DST/inc -F $DST/inc
state "3 incremental in progress"
props "3 incremental in progress" $DST/inc
imnt=$(get_prop mountpoint $DST/inc)
if touch $imnt/probe 2>$ERR; then
	probe "3 write to the head during the receive accepted"
else
	probe "3 write to the head during the receive refused: $(cat $ERR)"
fi
probe "3 datasets: $(zfs list -H -o name -t all -r $DST | tr '\n' ' ')"
try_disable "3 incremental in progress" large_dnode
try_disable "3 incremental in progress" zstd_compress
end_recv $INCR
state "3 incremental done"
props "3 incremental done" $DST/inc
try_disable "3 incremental done" large_dnode
try_disable "3 incremental done" zstd_compress

log_pass "Probe of received dnodesize and compression=zstd values"
