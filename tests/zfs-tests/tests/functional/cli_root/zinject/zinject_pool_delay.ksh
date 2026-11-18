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
#	An import or export delay added with 'zinject -P' fires on the
#	import or export of its pool, whatever handlers were added before
#	it, and the handler is removed once it has fired.
#
# STRATEGY:
#	1. Create two pools on file vdevs and export the second one
#	2. Add an I/O delay on the first pool, then an import delay on the
#	   second: its import must take at least the delay and remove the
#	   import delay handler
#	3. With the I/O delay still registered, add an export delay on the
#	   second pool: its export must take at least the delay and remove
#	   the export delay handler
#	4. Clear all handlers, add an export delay on the first pool, then
#	   an import delay on the second: the import must take at least the
#	   delay and remove the import delay handler, and the export delay
#	   handler of the first pool must remain
#

verify_runnable "global"

DEVICE_DIR=$TEST_BASE_DIR/dev_zinject-pool-delay
DELAY=5

function cleanup
{
	zinject -c all
	poolexists $TESTPOOL1 && destroy_pool $TESTPOOL1
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	log_must rm -rf $DEVICE_DIR
}

# Whether a pool delay handler of the given command is registered for pool.
function pool_delay_registered # pool import|export
{
	zinject | grep -qE "^ *[0-9]+ +$1 +[0-9]+ +$2\$"
}

# 'zpool <cmd> <args> <pool>', where pool has a delay handler for cmd: it
# must take at least DELAY seconds and remove the handler.
function delayed # import|export pool [args]
{
	typeset cmd=$1
	typeset pool=$2
	shift 2

	log_must pool_delay_registered $pool $cmd
	SECONDS=0
	log_must zpool $cmd "$@" $pool
	typeset elapsed=$SECONDS
	log_note "zpool $cmd $pool took $elapsed seconds"
	(( elapsed >= DELAY )) || \
	    log_fail "zpool $cmd $pool took $elapsed s, delay is $DELAY s"
	log_mustnot pool_delay_registered $pool $cmd
}

log_assert "zinject import and export delays fire behind other handlers"

log_onexit cleanup

log_must mkdir -p $DEVICE_DIR
log_must truncate -s $MINVDEVSIZE $DEVICE_DIR/disk1 $DEVICE_DIR/disk2
log_must zpool create -f -O mountpoint=none $TESTPOOL1 $DEVICE_DIR/disk1
log_must zpool create -f -O mountpoint=none $TESTPOOL2 $DEVICE_DIR/disk2
log_must zpool export $TESTPOOL2

log_must zinject -d $DEVICE_DIR/disk1 -D 1:1 $TESTPOOL1
log_must zinject -P import -s $DELAY $TESTPOOL2
delayed import $TESTPOOL2 -d $DEVICE_DIR

log_must zinject -P export -s $DELAY $TESTPOOL2
delayed export $TESTPOOL2

log_must zinject -c all
log_must zinject -P export -s $DELAY $TESTPOOL1
log_must zinject -P import -s $DELAY $TESTPOOL2
delayed import $TESTPOOL2 -d $DEVICE_DIR
log_must pool_delay_registered $TESTPOOL1 export

log_pass "zinject import and export delays fire behind other handlers"
