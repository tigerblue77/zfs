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
# DESCRIPTION:
#	'zinject -n POINT -e errno' fails the next pass through an injection
#	point once, and 'zinject -n POINT -D msecs' delays every pass until
#	it is cleared.  zinject lists both with their match and inject
#	counts.
#
# STRATEGY:
#	1. Create a pool on a file and export it.
#	2. Add an error handler at spa_load:vdev-metadata.  Check that the
#	   import fails with an I/O error and removes the handler, and that
#	   the next import succeeds.
#	3. Export, and add a delay handler of DELAY ms at the same point.
#	   Check that each import takes at least DELAY ms and that the
#	   handler's MATCH and INJECT counts grow by one each time.
#	4. Clear the handler, and check that the import no longer counts.
#	5. Check that bad delays and -D with -e are rejected.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset VDEV=$TEST_BASE_DIR/zinject_point_delay.dat
typeset POOL=zinject_point_delay
typeset POINT=spa_load:vdev-metadata
typeset -i DELAY=3000

function cleanup
{
	zinject -c all
	poolexists $POOL || zpool import -d $TEST_BASE_DIR $POOL
	destroy_pool $POOL
	rm -f $VDEV
}

# Import the pool, and print how long it took in ms.
function timed_import
{
	typeset -F start=$SECONDS
	zpool import -d $TEST_BASE_DIR $POOL >&2 ||
	    log_fail "zpool import $POOL failed"
	echo $(( (SECONDS - start) * 1000 ))
}

log_onexit cleanup

log_assert "zinject -n can fail an injection point once, or delay it"

log_must truncate -s $MINVDEVSIZE $VDEV
log_must zpool create $POOL $VDEV
log_must zpool export $POOL

# Error action: one failure, then the handler is gone.
typeset id=$(zinject -q -n $POINT -e io $POOL)
log_must test -n "$id"
log_must test "$(inject_point_field $id ACTION)" = "error=io"
log_mustnot zpool import -d $TEST_BASE_DIR $POOL
log_mustnot poolexists $POOL
log_must test -z "$(inject_point_field $id ACTION)"
log_must zpool import -d $TEST_BASE_DIR $POOL
log_must zpool export $POOL

# Delay action: every import is delayed and counted.
id=$(zinject -q -n $POINT -D $DELAY $POOL)
log_must test -n "$id"
log_must test "$(inject_point_field $id ACTION)" = "delay=${DELAY}ms"
log_must test "$(inject_point_field $id MATCH)" = "0"
for n in 1 2; do
	typeset -i took=$(timed_import)
	log_note "import $n took $took ms with a $DELAY ms delay"
	(( took >= DELAY )) ||
	    log_fail "import $n took $took ms, under the $DELAY ms delay"
	log_must test "$(inject_point_field $id MATCH)" = "$n"
	log_must test "$(inject_point_field $id INJECT)" = "$n"
	log_must zpool export $POOL
done

log_must zinject -c $id
log_must zpool import -d $TEST_BASE_DIR $POOL
[[ "$(zinject)" == "No handlers registered."* ]] ||
    log_fail "handler still listed after zinject -c"

# Bad arguments.
log_mustnot zinject -n $POINT -D 0 $POOL
log_mustnot zinject -n $POINT -D 10:1 $POOL
log_mustnot zinject -n $POINT -D abc $POOL
log_mustnot zinject -n $POINT -D 10 -e io $POOL
log_mustnot zinject -n $POINT -D 10 $POOL $POOL
[[ "$(zinject)" == "No handlers registered."* ]] ||
    log_fail "a rejected handler was registered"

log_pass "zinject -n can fail an injection point once, or delay it"
