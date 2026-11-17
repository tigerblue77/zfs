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
# A "zpool sync -f" started while a txg syncs, in a pool that this txg then
# suspends (failmode=wait), does not keep "zpool clear" from resuming the
# pool.  The forced sync used to wait for that txg as SCL_CONFIG writer,
# and "zpool clear" queued behind it to get the pool config, so the pool
# could not be resumed.
#
# STRATEGY:
# 1. Create a pool of two file vdevs with failmode=wait, and write to it.
# 2. Delay every write and fail every uberblock write, so that the next txg
#    syncs for several seconds and then suspends the pool.
# 3. Write to the pool and start "zpool sync" in the background.
# 4. While that txg syncs, start "zpool sync -f" in the background.
# 5. Wait for the pool to suspend.
# 6. Clear the injections and the pool, and verify that both syncs end
#    without error and that the pool is online.
# 7. Verify that "zpool sync -f" still rewrites the vdev labels.
#
# Every command that a regression could leave blocked in the kernel runs
# in the background, with its output in a file, and is waited for with a
# timeout: the test runner, after a test ends, may wait for every process
# that holds its output pipes, so one blocked process would stall the run.
# When a command times out, the test prints where the blocked threads wait
# and fails.
#

verify_runnable "global"

typeset VDEV_DIR=$TEST_BASE_DIR/zpool_sync_force_suspended
typeset VDEV0=$VDEV_DIR/vdev0
typeset VDEV1=$VDEV_DIR/vdev1
typeset OUT=$VDEV_DIR/out
typeset hung=""
set -A bg_pids

function cleanup
{
	bounded 60 zinject -c all
	# A pool that cannot be resumed can be neither exported nor destroyed:
	# leave it rather than block the rest of the run.
	if [[ -z $hung ]] && bounded 60 zpool clear $TESTPOOL1; then
		bounded 60 zpool destroy -f $TESTPOOL1
	fi
	rm -rf $VDEV_DIR
}

# Print where the given processes, the background syncs and the sync
# threads wait.  On Linux, also dump the blocked tasks to the kernel log,
# which is printed when the test fails.
function print_stacks
{
	typeset pid
	typeset seen=" "
	typeset -a pids=("$@" "${bg_pids[@]}")

	if is_linux; then
		pids+=($(pgrep -x txg_sync))
		[[ -w /proc/sysrq-trigger ]] && echo w > /proc/sysrq-trigger
	fi
	for pid in "${pids[@]}"; do
		[[ $seen == *" $pid "* ]] && continue
		seen+="$pid "
		kill -0 $pid 2>/dev/null || continue
		ps -o pid=,wchan=,args= -p $pid
		if is_linux; then
			cat /proc/$pid/stack
		elif is_freebsd; then
			procstat -kk $pid
		fi
	done
	if is_freebsd; then
		procstat -kk -a | grep txg_sync_thread
	fi
}

# Wait up to $2 seconds for the background process $1 to end, and return
# its status.  If it does not end, print where it waits and return 1.
function wait_bounded
{
	typeset pid=$1
	typeset -i timeout=$2

	while kill -0 $pid 2>/dev/null; do
		if ((timeout-- == 0)); then
			print_stacks $pid
			hung=$pid
			return 1
		fi
		sleep 1
	done
	wait $pid
}

# Run a command for at most $1 seconds, and return its status
function bounded
{
	typeset -i timeout=$1
	shift

	"$@" > $OUT 2>&1 < /dev/null &
	typeset pid=$!
	wait_bounded $pid $timeout
	typeset -i status=$?
	cat $OUT
	[[ $hung == "$pid" ]] && echo "$* did not return in $timeout s" >&2
	return $status
}

log_onexit cleanup

log_assert "zpool sync -f started before the pool suspends does not keep" \
    "zpool clear from resuming it"

log_must mkdir -p $VDEV_DIR
log_must truncate -s $((2 * MINVDEVSIZE)) $VDEV0 $VDEV1
log_must zpool create -f -o failmode=wait -O compression=off $TESTPOOL1 \
    $VDEV0 $VDEV1
log_must file_write -o create -f /$TESTPOOL1/seed -b 1048576 -c 32 -d R
log_must zpool sync $TESTPOOL1

# The next txg takes a second for each round of writes, then fails to write
# the uberblocks and suspends the pool in spa_sync_rewrite_vdev_config(),
# holding SCL_CONFIG as reader.
typeset vdev
for vdev in $VDEV0 $VDEV1; do
	log_must zinject -d $vdev -D 1000:8 -T write $TESTPOOL1
	log_must zinject -d $vdev -e io -L uber -T write $TESTPOOL1
done
log_must file_write -o create -f /$TESTPOOL1/file -b 1048576 -c 4 -d R

# Both syncs wait for "zpool clear" once the pool is suspended.
SECONDS=0
zpool sync $TESTPOOL1 > $VDEV_DIR/sync 2>&1 < /dev/null &
typeset sync_pid=$!
bg_pids+=($sync_pid)
sleep 3
if [[ $(kstat_pool $TESTPOOL1 state) == "SUSPENDED" ]]; then
	log_fail "the pool suspended before zpool sync -f started"
fi
zpool sync -f $TESTPOOL1 > $VDEV_DIR/sync_force 2>&1 < /dev/null &
typeset force_pid=$!
bg_pids+=($force_pid)

typeset -i tries=120
until [[ $(kstat_pool $TESTPOOL1 state) == "SUSPENDED" ]]; do
	if ((tries-- == 0)); then
		print_stacks
		log_fail "the pool did not suspend"
	fi
	sleep 1
done
log_note "the pool suspended ${SECONDS%.*} s after the first zpool sync"
if ! kill -0 $force_pid 2>/dev/null; then
	log_fail "zpool sync -f ended before the pool suspended:" \
	    "$(<$VDEV_DIR/sync_force)"
fi

log_must bounded 60 zinject -c all
log_must bounded 60 zpool clear $TESTPOOL1
wait_bounded $sync_pid 60 ||
    log_fail "zpool sync failed: $(<$VDEV_DIR/sync)"
wait_bounded $force_pid 60 ||
    log_fail "zpool sync -f failed: $(<$VDEV_DIR/sync_force)"
log_must test "$(kstat_pool $TESTPOOL1 state)" = "ONLINE"

# Unlike a plain sync, a forced sync rewrites the vdev labels.
typeset -i before=$(zdb -l $VDEV0 | awk '$1 == "txg:" { print $2; exit }')
log_must bounded 60 zpool sync -f $TESTPOOL1
typeset -i after=$(zdb -l $VDEV0 | awk '$1 == "txg:" { print $2; exit }')
if ((after <= before)); then
	log_fail "zpool sync -f did not rewrite the labels (txg $before," \
	    "then $after)"
fi

log_pass "zpool sync -f started before the pool suspends does not keep" \
    "zpool clear from resuming it"
