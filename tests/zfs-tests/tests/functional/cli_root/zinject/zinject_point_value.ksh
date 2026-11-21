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
#	'zinject -n POINT -o VALUE' acts only once the point's value has
#	reached VALUE.  At raidz_expand:reflow the value is the reflow
#	offset, and a pause from an offset holds the reflow exactly at that
#	offset, rounded down to a sector, also across an export.
#
# STRATEGY:
#	For an offset inside the data (not sector-aligned), one in free
#	space past the data, and one inside the scratch area at the start:
#	1. Create a raidz1 pool of four disks (ashift 12) and write 64M.
#	2. Add a pause handler at raidz_expand:reflow from the offset, and
#	   a delay handler from an offset the reflow never reaches.  Attach
#	   a fifth disk and wait until the reflow is held.
#	3. Check that it is held at the offset rounded down to a sector
#	   (or, for the scratch area, at its end), as listed by zinject and
#	   in the uberblock, and that the delay handler never acted.
#	4. Export, and check that the exported pool's offset is the same.
#	5. Import, clear the handlers, wait for the expansion, scrub.
#	Check that -o is rejected without -n and with a bad value.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

typeset DIR=$TEST_BASE_DIR/zinject_point_value
typeset POOL=zinject_point_value
typeset POINT=raidz_expand:reflow

function cleanup
{
	zinject -c all
	poolexists $POOL || zpool import -d $DIR $POOL 2>/dev/null
	destroy_pool $POOL
	rm -rf $DIR
}

# The reflow offset in the uberblock; zdb_args select the pool.
function reflow_offset # zdb_args...
{
	zdb "$@" -u $POOL | awk '/raidz_reflow/ {
	    for (i = 1; i <= NF; i++)
		if (sub("^off=", "", $i)) print $i }'
}

# Hold an expansion from offset $1, and check that it is held at $2
# (or anywhere inside the first $3 bytes, for the scratch area).
function hold_at # offset expected [scratch]
{
	typeset offset=$1 expected=$2 scratch=$3

	log_must mkdir -p $DIR
	log_must truncate -s 512M $DIR/dev-{0..4}
	log_must zpool create -o ashift=12 $POOL raidz1 $DIR/dev-{0..3}
	log_must zfs create -o compress=off -o primarycache=metadata $POOL/fs
	log_must fill_fs /$POOL/fs 1 64 1048576 1 R
	log_must zpool sync $POOL

	typeset id=$(zinject -q -n $POINT -o $offset -A pause $POOL)
	typeset never=$(zinject -q -n $POINT -o 1T -D 1 $POOL)
	log_must test -n "$id" -a -n "$never"
	log_must test "$(inject_point_field $id FROM)" = "$offset"
	log_must zpool attach $POOL raidz1-0 $DIR/dev-4
	wait_inject_point $id 1 120 || log_fail "the reflow was not held"
	log_must zpool sync $POOL

	typeset at=$(inject_point_field $id AT)
	typeset ub=$(reflow_offset)
	log_note "from $offset: held at $at, uberblock offset $ub"
	if [[ -n $scratch ]]; then
		((at > 0 && at <= scratch)) ||
		    log_fail "held at $at, not at the end of the scratch area"
	else
		log_must test "$at" = "$expected"
	fi
	log_must test "$ub" = "$at"
	log_must test "$(inject_point_field $never INJECT)" = "0"

	log_must zpool export $POOL
	log_must test "$(reflow_offset -e -p $DIR)" = "$at"
	log_must zpool import -d $DIR $POOL
	log_must zinject -c all
	log_must zpool wait -t raidz_expand $POOL
	is_pool_scrubbing $POOL && wait_scrubbed $POOL
	log_must zpool scrub -w $POOL
	log_must check_pool_status $POOL "errors" "No known data errors"
	log_must zpool destroy $POOL
	log_must rm -rf $DIR
}

log_onexit cleanup

log_assert "zinject -n -o holds a raidz reflow exactly at an offset"

# Inside the data, not sector-aligned.
hold_at $((40 * 1048576 + 1234)) $((40 * 1048576))
# Free space past the data.
hold_at $((900 * 1048576 + 4095)) $((900 * 1048576))
# Inside the scratch area: held at its end, within the first few MB.
hold_at 4096 "" $((64 * 1048576))

log_mustnot zinject -o 4096 -d $TEST_BASE_DIR/nonexistent $POOL
log_mustnot zinject -n $POINT -o bogus -A pause $POOL
[[ "$(zinject)" == "No handlers registered."* ]] ||
    log_fail "a rejected handler was registered"

log_pass "zinject -n -o holds a raidz reflow exactly at an offset"
