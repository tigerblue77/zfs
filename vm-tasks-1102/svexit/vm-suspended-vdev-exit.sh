#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# Does a vdev change whose own txg sync suspends the pool keep
# spa_namespace_lock while it waits?  For a DISPOSABLE VM only: it creates
# and suspends pools, and on an unfixed kernel it leaves processes in D
# state that only a reboot clears.
#
# usage: vm-suspended-vdev-exit.sh <built zfs tree> <result dir>
#
# The modules of <built zfs tree> must be loaded (scripts/zfs.sh) and its
# own zpool/zinject are used.  Every wait is bounded by T seconds.
# Exit status: 0 PASS (no hang), 2 HANG (reboot needed), 3 INCONCLUSIVE
# (the pool never suspended, or the detach was refused), 1 setup error.

set -u
Z=$(cd "$1" && pwd -P) || exit 1
R=$2
T=${T:-60}
P=vxs
P2=vxo
ZPOOL=$Z/zpool
ZINJECT=$Z/zinject
PARAM=/sys/module/zfs/parameters
KSTAT=/proc/spl/kstat/zfs

mkdir -p "$R" || exit 1
: > "$R/blocked.pids"
exec 3>&1 > "$R/run.log" 2>&1

# The outcome goes to the terminal as well as to run.log.
verdict() {
	echo "$*"
	echo "$*" >&3
}

die() {
	verdict "SETUP: $* failed, see $R/run.log"
	exit 1
}
echo "date: $(date -u +%FT%TZ)"
echo "tree: $(git -C "$Z" log -1 --format='%h %s' 2>/dev/null)"
echo "module: $(cat /sys/module/zfs/version 2>/dev/null)"
echo "kernel: $(uname -r)"
"$ZPOOL" version

[ -d /sys/module/zfs ] || die "loading the zfs module"
for p in $P $P2; do
	if "$ZPOOL" list -H -o name 2>/dev/null | grep -qx "$p"; then
		die "pool $p already exists:"
	fi
done

D=$(mktemp -d /var/tmp/vxs.XXXXXX) || die mktemp
old_to=$(cat $PARAM/zfs_txg_timeout)
old_sysrq=$(cat /proc/sys/kernel/sysrq)
echo 1 > $PARAM/zfs_dbgmsg_enable
echo 0 > $KSTAT/dbgmsg

# Run "$@" in the background for at most T seconds.  Returns its status,
# or 124 and records the pid if it is still running.
bounded() {
	name=$1
	shift
	"$@" > "$R/$name.out" 2>&1 &
	pid=$!
	i=0
	while kill -0 $pid 2>/dev/null; do
		if [ $i -ge "$T" ]; then
			echo "$name: STILL RUNNING after ${T}s (pid $pid)"
			echo "$pid $name" >> "$R/blocked.pids"
			return 124
		fi
		sleep 1
		i=$((i + 1))
	done
	wait $pid
	rc=$?
	echo "$name: returned $rc after ${i}s"
	return $rc
}

state() {
	cat $KSTAT/$P/state 2>/dev/null || echo "?"
}

collect() {
	cat $KSTAT/dbgmsg > "$R/dbgmsg.txt" 2>&1
	{
		while read -r pid name; do
			echo "== $name pid $pid" \
			    "state $(awk '{print $3}' /proc/"$pid"/stat 2>/dev/null)"
			cat /proc/"$pid"/stack 2>/dev/null
		done < "$R/blocked.pids"
		echo "== detach pid $dpid" \
		    "state $(awk '{print $3}' /proc/$dpid/stat 2>/dev/null)"
		cat /proc/$dpid/stack 2>/dev/null
		for pid in $(pgrep -x txg_sync); do
			echo "== txg_sync pid $pid"
			cat /proc/"$pid"/stack
		done
	} > "$R/stacks.txt" 2>&1
	echo 1 > /proc/sys/kernel/sysrq
	echo w > /proc/sysrq-trigger
	sleep 2
	dmesg > "$R/dmesg.txt"
	echo "$old_sysrq" > /proc/sys/kernel/sysrq
	echo "pool state: $(state)"
	echo "stacks: $R/stacks.txt, dmesg: $R/dmesg.txt," \
	    "dbgmsg: $R/dbgmsg.txt"
	grep -c spa_vdev_config_exit "$R/dmesg.txt" | \
	    sed 's/^/dmesg frames in spa_vdev_config_exit: /'
}

echo 600 > $PARAM/zfs_txg_timeout
truncate -s 256M "$D/m1" "$D/m2" "$D/m3" "$D/other" || die truncate
"$ZPOOL" create -f $P mirror "$D/m1" "$D/m2" "$D/m3" || die "create $P"
"$ZPOOL" create -f $P2 "$D/other" || die "create $P2"
"$ZPOOL" sync $P $P2 || die "zpool sync"

# m3 gets no handler: an ENXIO handler on the detached side makes
# vdev_dtl_required() refuse the detach ("no valid replicas").
for dev in "$D/m1" "$D/m2"; do
	"$ZINJECT" -d "$dev" -e io -T write $P || die zinject
	"$ZINJECT" -d "$dev" -e nxio -T probe $P || die zinject
done
echo "before detach: $(state)"

"$ZPOOL" detach $P "$D/m3" > "$R/detach.out" 2>&1 &
dpid=$!
i=0
while [ "$(state)" != SUSPENDED ] && kill -0 $dpid 2>/dev/null &&
    [ $i -lt "$T" ]; do
	sleep 1
	i=$((i + 1))
done
echo "after ${i}s: pool $(state)," \
    "detach $(kill -0 $dpid 2>/dev/null && echo running || echo exited)"

if [ "$(state)" != SUSPENDED ]; then
	if kill -0 $dpid 2>/dev/null; then
		verdict "INCONCLUSIVE: detach running, pool not suspended"
		collect
	else
		wait $dpid
		verdict "INCONCLUSIVE: detach returned $? before any suspend:" \
		    "$(cat "$R/detach.out")"
	fi
	exit 3
fi

if ! bounded status_other "$ZPOOL" status $P2; then
	echo "zpool status $P2 blocked while $P is suspended"
	bounded zinject_clear "$ZINJECT" -c all
	bounded clear "$ZPOOL" clear $P
	collect
	verdict "HANG: the suspended detach blocks other pools and the" \
	    "recovery; reboot the VM"
	exit 2
fi

bounded zinject_clear "$ZINJECT" -c all
bounded clear "$ZPOOL" clear $P
i=0
while kill -0 $dpid 2>/dev/null && [ $i -lt "$T" ]; do
	sleep 1
	i=$((i + 1))
done
if kill -0 $dpid 2>/dev/null; then
	echo "zpool detach still running ${T}s after zpool clear"
	collect
	verdict "HANG: detach did not complete after the clear; reboot the VM"
	exit 2
fi
wait $dpid
drc=$?
echo "detach returned $drc: $(cat "$R/detach.out")"
bounded status_pool "$ZPOOL" status -P $P
cat "$R/status_pool.out"
gone=yes
grep -q "$D/m3" "$R/status_pool.out" && gone=no
echo "pool $(state), m3 detached: $gone"
cat $KSTAT/dbgmsg > "$R/dbgmsg.txt" 2>&1

bounded destroy1 "$ZPOOL" destroy -f $P
bounded destroy2 "$ZPOOL" destroy -f $P2
echo "$old_to" > $PARAM/zfs_txg_timeout
rm -rf "$D"
if [ $drc -eq 0 ] && [ $gone = yes ]; then
	verdict "PASS: no hang, detach completed after zpool clear"
	exit 0
fi
verdict "FAIL: no hang, but detach rc $drc, m3 detached: $gone"
exit 1
