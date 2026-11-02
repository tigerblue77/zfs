# VM task: spa_vdev_exit() waiting under spa_namespace_lock on a suspended pool

Self-contained task for a **disposable Debian 13 VM** (no pool on it that
matters, reboot allowed). Nothing is posted anywhere; results stay on the
VM and are copied back by hand.

## What is being tested

Finding: a vdev configuration change (here `zpool detach`) ends in
`spa_vdev_exit()`, whose `spa_vdev_config_exit()` waits in
`txg_wait_synced()` while it still holds `spa_namespace_lock`. If the pool
suspends during that sync, the txg only syncs after `zpool clear` resumes
the pool, but `zpool clear` itself needs `spa_namespace_lock`
(`pool_status_check()` -> `spa_open()`, then `zfs_ioc_clear()`), and so do
`zinject -c` (`zio_inject_list_next()`, `spa_inject_delref()`) and every
`spa_open()` of every other pool. Expected on master: permanent hang of
everything that opens a pool; only a reboot recovers.

Already established on the development host (userspace only, libzpool,
master 0cdf531257 built `--with-config=user --enable-debug`):

| Run (libzpool program `vdev_exit_suspend`) | master | with the fix |
| --- | --- | --- |
| 3-way mirror, writes+probes of m1, m2 fail, detach m3 | pool SUSPENDED, namespace lock HELD, `spa_open()` of the other pool blocked 15 s, `zio_clear_fault()` blocked 15 s, process had to be abandoned | lock free, other pool opens at once, clear resumes the pool, detach returns 0 (20/20 runs) |
| same, without `zinject -c` first | `zpool clear` blocked in `spa_open_common()` (from `pool_status_check()`) | clear runs, pool re-suspends (handlers still there), detach keeps waiting without the lock |
| handlers on all three sides (recipe of the 25 Oct VM script) | detach refused at once with EBUSY ("no valid replicas"), pool never suspends | same |
| 2-way mirror, handlers on m1, detach m2 | detach returns 0, no suspend: m1 takes over the mirror's guid, its handlers stop matching | same |

The program is
`/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1102/suspended-vdev-exit/repro/vdev_exit_suspend.c`
(built by `repro/build.sh <built tree> <binary>` next to it; usage
`vdev_exit_suspend <scratch dir> three|two|all <timeout s>`). It needs no
kernel module and can be rerun on the VM against a `--with-config=user`
build if wanted.

Stacks on master, from gdb on the libzpool program:

```
detach: cv_wait <- txg_wait_synced_flags <- txg_wait_synced <- spa_vdev_config_exit <- spa_vdev_exit <- spa_vdev_detach
other:  mutex_enter <- spa_namespace_enter <- spa_open_common <- spa_open
clear:  mutex_enter <- spa_namespace_enter <- spa_inject_delref <- zio_clear_fault
sync:   cv_timedwait <- zio_wait <- dsl_pool_sync_mos <- dsl_pool_sync <- spa_sync
```

Why the 25 Oct VM run was inconclusive: its script put `-e nxio -T probe`
on the side it detached. `vdev_dtl_required()` calls
`zio_handle_device_injection(vd, NULL, ECHILD)`, and an ENXIO handler on
that vdev answers EIO for a NULL zio, so the detach was refused ("no valid
replicas") before anything synced. With a 2-way mirror the remaining side
inherits the mirror's guid, so handlers on it stop matching. Hence the
3-way mirror below, with no handler on the detached side.

Not yet established, and the point of this task: the same in the kernel
(uninterruptible D-state waits, real `zpool`/`zinject` commands), and the
fix in the kernel.

## Safety rules

- Disposable VM only. `scripts/zfs.sh -r` replaces the module stack;
  the test creates pools `vxs`, `vxo` (and ZTS creates `testpool*`).
- On master the run is expected to leave processes in D state that no
  signal clears: **reboot the VM** after it, before anything else.
- Every step below has a time bound. If a step outlives its bound, collect
  what section "Evidence" says, then reboot.
- Run as root. Post nothing.

## Files needed on the VM

From the development host, copy to `/root/` on the VM:

- `/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1102/suspended-vdev-exit/vm-suspended-vdev-exit.sh`
- `/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1102/suspended-vdev-exit/spa-vdev-exit-suspended.patch`
  (the fix plus its ZTS case: `git format-patch` of commit 638c2590b9 on
  the local branch `spa-vdev-exit-suspended`, based on master 0cdf531257;
  not pushed)

The script is also reproduced at the end of this file.

## 1. Prepare the VM (once)

```sh
export DEBIAN_FRONTEND=noninteractive
timeout 1800 apt-get update -y
timeout 3600 apt-get install -y build-essential git autoconf automake \
    libtool libtool-bin gawk alien fakeroot dkms acl attr bc cpio \
    cryptsetup dbench fio gdb ksh jq libacl1-dev libaio-dev libattr1-dev \
    libblkid-dev libcurl4-openssl-dev libelf-dev libffi-dev libpam0g-dev \
    libssl-dev libtirpc-dev libudev-dev uuid-dev zlib1g-dev lsscsi parted \
    python3 python3-cffi python3-dev python3-packaging python3-setuptools \
    rsync strace sysstat xxhash "linux-headers-$(uname -r)"
zpool list 2>/dev/null; lsmod | grep -w zfs   # must show no pool that matters
```

## 2. Master: build, load, run (expected: HANG)

```sh
cd /root && [ -d zfs ] || timeout 1800 git clone https://github.com/openzfs/zfs.git
cd /root/zfs && timeout 600 git fetch origin && git checkout --detach 0cdf531257
git log -1 --oneline                       # record it
timeout 1800 ./autogen.sh > /root/build-master.log 2>&1 &&
timeout 1800 ./configure --enable-debug >> /root/build-master.log 2>&1 &&
timeout 5400 make -j"$(nproc)" >> /root/build-master.log 2>&1; echo "build $?"
tail -3 /root/build-master.log
timeout 300 ./scripts/zfs.sh -r; echo "load $?"
cat /sys/module/zfs/version; ./zpool version   # both must be the tree just built
chmod +x /root/vm-suspended-vdev-exit.sh
/root/vm-suspended-vdev-exit.sh /root/zfs /root/res-master; echo "exit $?"
grep -E 'returned|STILL|pool|HANG|PASS|FAIL|INCONCLUSIVE|SETUP' /root/res-master/run.log
```

The script needs about 4 minutes on master (60 s bound per blocked step).
Then collect (section "Evidence") and **reboot**.

## 3. Fixed tree: build, load, run (expected: PASS)

After the reboot:

```sh
cd /root/zfs && git checkout --detach 0cdf531257 && git clean -fdx -q
git -c user.name=vm -c user.email=vm@localhost am /root/spa-vdev-exit-suspended.patch
git log -2 --oneline                       # record it
timeout 1800 ./autogen.sh > /root/build-fixed.log 2>&1 &&
timeout 1800 ./configure --enable-debug >> /root/build-fixed.log 2>&1 &&
timeout 5400 make -j"$(nproc)" >> /root/build-fixed.log 2>&1; echo "build $?"
tail -3 /root/build-fixed.log
timeout 300 ./scripts/zfs.sh -r; echo "load $?"
cat /sys/module/zfs/version; ./zpool version
/root/vm-suspended-vdev-exit.sh /root/zfs /root/res-fixed; echo "exit $?"
grep -E 'returned|STILL|pool|HANG|PASS|FAIL|INCONCLUSIVE|SETUP' /root/res-fixed/run.log
```

Then the new ZTS case and the existing tests around vdev changes and
suspended pools, still on the fixed tree (ZTS creates and destroys
`testpool*` pools and loop devices):

```sh
cd /root/zfs
timeout 600 ./scripts/zfs-helpers.sh -i; echo "helpers $?"
timeout 1200 ./scripts/zfs-tests.sh -v -t \
    "$PWD/tests/zfs-tests/tests/functional/failmode/failmode_vdev_detach.ksh" \
    > /root/zts-detach.log 2>&1; echo "zts $?"
grep -E '^Test|PASS|FAIL|KILLED' /root/zts-detach.log | tail -5
timeout 14400 ./scripts/zfs-tests.sh -v \
    -T failmode,zpool_detach,zpool_attach,zpool_replace,zpool_split,zpool_clear,removal,replacement \
    > /root/zts-related.log 2>&1; echo "zts $?"
grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED|^Running Time' -A0 /root/zts-related.log
grep -E '\[(FAIL|KILLED)\]' /root/zts-related.log
```

If any of these hangs past its bound, collect the evidence and reboot.
A failure in the related tags must be compared against a run of the same
tags on master (after a reboot) before it is blamed on the patch.

Optional, to see the master behaviour of the ZTS case itself (it wedges
the VM on master; reboot afterwards): copy
`tests/zfs-tests/tests/functional/failmode/failmode_vdev_detach.ksh` from
the fixed checkout into the master checkout and run it the same way. On
master it should fail with "zpool status testpool1 still running after
60s", and its cleanup then blocks.

## Evidence to collect (after every run, and before any reboot)

The script already writes, in its result directory:
`run.log` (every step with its duration and status), `stacks.txt`
(`/proc/<pid>/stack` of every blocked command, of the detach and of every
`txg_sync` thread), `dmesg.txt` (after `echo w > /proc/sysrq-trigger`:
all D-state tasks), `dbgmsg.txt` (`/proc/spl/kstat/zfs/dbgmsg`, enabled
by the script), and the output of each command (`*.out`).
For a ZTS hang, add:

```sh
for p in $(pgrep -x zpool) $(pgrep -x zinject); do
    echo "== $p $(tr '\0' ' ' < /proc/$p/cmdline)"; cat /proc/$p/stack
done > /root/zts-stacks.txt 2>&1
echo 1 > /proc/sys/kernel/sysrq; echo w > /proc/sysrq-trigger; dmesg > /root/zts-dmesg.txt
cat /proc/spl/kstat/zfs/dbgmsg > /root/zts-dbgmsg.txt
tar czf /root/results.tgz /root/res-* /root/zts-* /root/build-*.log 2>/dev/null
```

Bring back `/root/results.tgz` (or the `res-master` and `res-fixed`
directories).

## How to read the result

| Observation | Meaning |
| --- | --- |
| master: `HANG`; `status_other: STILL RUNNING`; `zinject_clear` and `clear` STILL RUNNING; `stacks.txt` shows the detach in `txg_wait_synced <- spa_vdev_config_exit <- spa_vdev_exit <- spa_vdev_detach <- zfs_ioc_vdev_detach`, and the others waiting for `spa_namespace_lock` (`spa_namespace_enter_interruptible <- spa_all_configs` or `spa_namespace_enter <- spa_open_common` for `zpool status`, `spa_namespace_enter <- zio_inject_list_next` for `zinject -c`, `spa_namespace_enter <- spa_open_common` for `zpool clear`) | **Finding confirmed** in the kernel. Scope: every pool on the system, until reboot |
| master: pool SUSPENDED, detach still running, but `zpool status vxo` returns within 60 s | **Finding refuted** for this path: the detach waits without blocking the namespace. Keep `stacks.txt` to see where the detach really waits |
| master or fixed: `INCONCLUSIVE: detach returned ... before any suspend` | The recipe did not reach the state (as on 25 Oct). Look at `detach.out` and `dbgmsg.txt`; nothing is proven either way |
| master or fixed: `INCONCLUSIVE: detach running, pool not suspended` | The sync is stuck for another reason; read `stacks.txt` |
| fixed: `PASS` (status of vxo, zinject -c and zpool clear all return within seconds; detach returns 0 after the clear; m3 gone) | **Fix works** in the kernel for this path |
| fixed: `HANG` | Fix does not hold in the kernel; `stacks.txt` says which lock or wait remains |
| fixed: `failmode_vdev_detach` PASS; related tags no new FAIL against master | No regression seen in the vdev-change tests |

## What the fix does (commit 638c2590b9, branch `spa-vdev-exit-suspended`, not pushed)

At the end of a vdev change only (`spa_vdev_exit()`), the txg wait is
done with `TXG_WAIT_SUSPEND` under the namespace lock. If the pool is
suspended, the thread takes a hold on the spa, drops
`spa_namespace_lock`, waits for the txg without it (as
`spa_sync_allpools()` does), takes it back and drops the hold.
`spa_vdev_top_lock` stays held, so no other vdev change on that pool can
start, and the hold makes a concurrent export fail with EBUSY. When the
pool does not suspend nothing changes. The command still blocks until the
pool is resumed (as other writes do with `failmode=wait`) and then
completes. The intermediate `spa_vdev_config_exit()` waits of multi-step
operations (removal, split, raidz expansion, l2cache removal) keep the
lock, as do `spa_config_update()` (import, `spa_vdev_add()` tail,
async config update) and `spa_export_common()`; those paths can still
hang the same way and are not covered.

## Appendix: vm-suspended-vdev-exit.sh

Identical to the file listed above; to recreate it on the VM, paste the
block below into `cat > /root/vm-suspended-vdev-exit.sh <<'EOF'` ... `EOF`.

```sh
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
```
