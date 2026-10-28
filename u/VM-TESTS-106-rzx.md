# VM 106 (CHSONVTST03): kernel evidence of the RAID-Z expansion deadlock and of `raidz-expand-intr-io-start` at 5f74c1f61

These instructions are for the session on VM 106 (Debian 13). They produce
kernel evidence of a deadlock between a RAID-Z expansion and the repair
writes of a resilver on upstream master, and of its fix: the fork branch
`raidz-expand-intr-io-start` of https://github.com/tigerblue77/zfs, one
commit, `5f74c1f61beba246c0bc594c4a9e50cfa01e4b16` "Do not start I/O to an
expanding RAID-Z vdev from an interrupt thread", on upstream master
`5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53` "ZTS: sync checksum test data
before fault injection". The session builds two fresh debug trees in its own
directories, runs everything below in the kernel, and replies with
`REPORT.txt`. This document is read from a clone of the branch
`vm-tasks-1026` of the same fork in `/var/tmp/vm-task-106u` (the doc
checkout, `DOC` below). STEP 3 extracts the amplified test from it, and the
last command of STEP 7 removes it with the task's own directories, unless a
hang has wedged a pool (see "Watching for a hang").

**The deadlock.** While a RAID-Z vdev is being expanded,
`vdev_raidz_io_start()` takes the reflow's range lock `vre_rangelock` as
reader for every I/O to the vdev (`vdev_raidz.c:2847` at 5ebd13aa9), and
waits for a row held as writer or for one a writer is waiting for.
`raidz_reflow_impl()` takes a row as writer (`vdev_raidz.c:4609`) before it
issues the row's reads, one per old child; `raidz_reflow_write_done()`
releases it (`vdev_raidz.c:4472`) once those reads have completed, on
`z_rd_int`, and the writes `raidz_reflow_read_done()` then issues
(`vdev_raidz.c:4517`) have completed, on `z_wr_int`. During a resilver,
`vdev_mirror_io_done()` rewrites the other copies of a block that has more
than one (`vdev_mirror.c:932`) inline, from the `z_rd_int` thread that
completed the read. When such a repair write reaches the expanding vdev and
waits for a row whose reads are queued behind it on `z_rd_int`, and
`z_rd_int` has one thread, nothing completes the row: the scan zio never
finishes, `txg_sync` waits in `dsl_scan_sync()` and the expansion thread in
`txg_wait_synced()`, forever. `zio_execute_stage()` already sends a blocking
stage run in an interrupt thread to the issue taskq, but only for a zio with
no vdev (`zio.c:2738`). This hung `raidz/raidz_expand_011_pos` once in CI
(https://github.com/tigerblue77/zfs/actions/runs/40327942174,
`qemu-x86 (almalinux8)`, 2 vCPUs, 1 run in the 8 looked at), `z_rd_int`'s
stack reading, from the top:

```
cv_wait_common <- zfs_rangelock_enter_reader <- zfs_rangelock_enter_impl <- vdev_raidz_io_start
  <- zio_vdev_io_start <- zio_nowait <- vdev_mirror_io_done <- zio_vdev_io_done <- zio_execute
  <- taskq_thread
```

with `txg_sync` in `zio_wait()` under `dsl_scan_sync()` and `raidz_expand`
in `txg_wait_synced()` under `spa_raidz_expand_thread()`. It has since been
reproduced in userspace only (libzpool, with a diagnostic patch), never on
demand in a kernel.

**The commit.** `zio_execute_stage()` (`module/zfs/zio.c`) also sends
`VDEV_IO_START` to the issue taskq when the zio's vdev is being expanded
(`vdev_rz_expanding`): a repair write started from `z_rd_int` then waits for
the row in `z_wr_iss` (a read of the next copy in `z_rd_iss`), and releasing
the row needs no issue thread. 1 file, 6 insertions, 1 deletion; the two
trees differ in nothing else.

**Why this run.** The commit has been checked in userspace only. This run is
its kernel evidence on a controlled VM, in two parts: on M, the deadlock
itself, made likely by an amplified copy of the test and by CI's thread
layout; on B, the same amplified runs, the unmodified test and the whole
`raidz` group, all passing with no hung task.

| Tree | Path | Code |
| --- | --- | --- |
| `B` | `/var/tmp/zts-rzx-1028/B` | the head `5f74c1f61` |
| `M` | `/var/tmp/zts-rzx-1028/M` | master `5ebd13aa9`, the head's parent |

Both trees are built here with `./autogen.sh && ./configure --enable-debug &&
make -j8` (8 jobs, not `nproc`: the cap of every build on this VM, see the
rules). Master `5ebd13aa9` changes only files under `tests/` since
`a128b8a075`, so M's module code is the master module every earlier task on
this VM built (srcversion `8852710DB871A7587B9E722`); STEP 1 checks it. The
tests are the same in both trees.

What the run establishes, B first, then M (a hang on M ends the task, so
every run on B comes before any on M):

1. **The layout.** After each load and before any pool, `zio_taskq_batch_pct`
   is set to 6: one `z_rd_int`, one `z_wr_int` and one `z_wr_iss` thread, as
   on CI's 2 vCPUs. `lay.sh` checks it on a scratch pool, and every run
   records the `threads_max` of its own pool's taskqs.
2. **B** (`ba1`-`ba8`, `bu01`-`bu10`, `bg`): the amplified test 8 times,
   `raidz_expand_011_pos` 10 times and the `raidz` group once. Every run
   passes, with no hung task and no kernel finding. With bpftrace, no
   `*_int` thread takes the expansion's range lock, and the waits of 50 ms
   or more for it are in issue threads (`z_wr_iss`, `z_rd_iss`).
3. **M** (`mu01`-`mu10`, then `ma1`-`ma8`): `raidz_expand_011_pos` 10 times,
   then the amplified test up to 8 times, stopping at the first hang.
   Predicted: a hang, `z_rd_int`'s stack as above, `z_rd_int` with its one
   thread active, tasks queued and none executed between two captures 60 s
   apart. If M never hangs, the kernel A/B is inconclusive for M, and B's
   results still stand.
4. **Throughout**: the kernel log of every run, and in STEP 7 the module's
   unload, unless a pool is wedged.

## The CI thread layout, forced here

`spa_taskqs_init()` (`module/zfs/spa.c` at 5ebd13aa9) sizes each zio taskq
when a pool is created or imported, from `boot_ncpus` (16 here) and
`zio_taskq_batch_pct` (80 by default; `zio_taskq_batch_tpq` 0,
`zio_taskq_write_tpq` 16 and 4 allocators by default):

- `z_rd_int` and `z_wr_int` are `ZTI_SCALE(0)` (lines 1242-1287):
  `threads = MAX(1, 16 * pct / 100)`; with no tpq, `cpus = MIN(threads, 16)`,
  `count = 1 + threads / 6`, lowered while `count * count > cpus`; then
  `value = (threads * 100 + 16 * count / 2) / (16 * count)`, a percentage of
  the online CPUs (`TASKQ_THREADS_CPU_PCT`) when it lies between 5 and 100,
  which SPL turns into `MAX(num_online_cpus() * value / 100, 1)` threads
  (`spl-taskq.c:1356-1362`).
  - pct 80: threads 12, count 3, value (1200 + 24) / 48 = 25, so
    16 × 25 / 100 = 4 threads in each of three taskqs `z_rd_int_0` to
    `z_rd_int_2` (12 in all).
  - pct 6: threads MAX(1, 96 / 100 = 0) = 1, count 1, value (100 + 8) / 16
    = 6, so MAX(16 × 6 / 100 = 0, 1) = 1 thread in one taskq `z_rd_int`.
    Every pct from 1 to 12 gives the same; 13 is the first with 2.
- `z_wr_iss` is `ZTI_SYNC` (lines 1219-1240): `threads = MAX(1, 16 * pct /
  100)`, `count = MAX(1, threads / 16)`, at least `(pct + 99) / 100` and at
  most the 4 allocators, and `value = (pct + count / 2) / count`, a CPU
  percentage.
  - pct 80: count 1, value 80: one taskq of 16 × 80 / 100 = 12 threads.
  - pct 6: count 1, value 6: one taskq of MAX(0, 1) = 1 thread.
- `z_rd_iss` is `ZTI_N(8)`: 8 threads whatever the pct.

CI's VMs have 2 vCPUs: MAX(1, 2 × 80 / 100) = 1 thread in each of these
taskqs but `z_rd_iss`, which is the layout pct 6 gives on 16 CPUs.

`zio_taskq_batch_pct` is `ZMOD_RW` (`spa.c:12383`) and only
`spa_taskqs_init()` reads it, at each `spa_activate()`: a write to
`/sys/module/zfs/parameters/zio_taskq_batch_pct` after the load and before a
pool's creation applies to that pool and every later one, until the next load
puts back 80. `zfs.sh` has no options of its own (it runs `modprobe` on the
tree's modules), and an option in `/etc/modprobe.d` would be a system file
this task must not write: the sysfs write it is, right after each load
(STEPs 4 and 6), before `lay.sh` creates its pool. The tests create their
pools later, so they all get it; every run checks the value in its guard.

## The amplified test

`raidz_expand_011_pos` makes a raidz1 pool of four 128 MiB files, writes
128 MiB, throttles the expansion with `raidz_expand_max_reflow_bytes` and
attaches a fifth file; then, four times, it offlines a child, writes 1 MiB,
raises the throttle, waits until the expansion pauses for resilver, onlines
the child, waits for the resilver (`zpool wait -t resilver`) and checks that
the expansion resumes. That resilver repairs the copies of the blocks born
during the outage, the metadata's through `vdev_mirror_io_done()`: the path
of the cycle. In natural timing it hangs seldom; the copy makes the cycle
likely, on both trees alike:

- after the attach, `zinject -d <dev-1> -D 1000:64 -T read $TESTPOOL`: every
  read of one healthy child takes at least 1 s (`-D latency:lanes` in
  milliseconds, `parse_delay()` in `cmd/zinject/zinject.c`; 64 lanes, so
  concurrent reads are not serialised; reads only, as
  `block_cloning_resilver_free.ksh` does it). A reflow row of four blocks or
  more reads every old child, so it stays locked as writer for at least 1 s;
  the delayed completion is dispatched to `z_rd_int` when the delay ends
  (`zio_delay_interrupt()`), it does not occupy the thread meanwhile;
- during each `zpool wait -t resilver`, a `zpool clear $TESTPOOL` every 0.3 s
  from a background loop: `spa_vdev_state_exit()` reassesses the DTLs and
  `raidz_dtl_reassessed()` resumes the expansion, so the reflow holds rows
  while the resilver issues its repair writes (each resumed pass stops again
  at its first row that reads the child being resilvered);
- `zinject -c all` once the outages are over (the rest of the reflow and the
  scrub then run at full speed), and in the cleanup before the pool is
  destroyed (an injection handler keeps the pool busy);
- the two waits of each outage relaxed, 60 s to 300 s for the pause for
  resilver and 30 s to 120 s for the resume after it;
- its own `log_pass` message, `an amplified expansion paused on an offline
  child resumes after its resilver`.

`zfs_vdev_removal_max_active` exists (`vdev_queue.c:146`, default 2,
`ZMOD_RW`; reflow I/O uses `ZIO_PRIORITY_REMOVAL`) and is left at its
default: the delay already holds every such row 1 s, and a limit of 1 would
serialise every reflow read of each child and lengthen every run.

The copy is kept out of both trees, in `$W/amp`, and runs from a runfile of
its own, `zfs-tests.sh -r $W/amp/amp.run`, whose one section is that
directory: test-runner takes a section naming an existing directory as it is
(`TestRun.read()`), and the copy sources the loaded tree's
`$STF_SUITE/include/libtest.shlib`. `zfs-tests.sh -t <absolute path>` would
run it too, but the single-test runfile it writes has a fixed 600 s timeout,
which the delayed runs might exceed; this runfile gives 1800 s. With no
`setup` or `cleanup` in `$W/amp`, an amplified run has one result.

STEP 3 extracts the copy byte for byte from this file, between the two
comment lines around the block below, and checks its sha256,
`a7c70098a56e81a0425783aabad3403b7e8df24ecfc0e7d4da4296fba563aa49`; `$L/amp.diff` then
shows what differs from the tree's test (25 lines removed, 43 added).

<!-- raidz_expand_011_amp.ksh: begin -->
```ksh
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
#	raidz_expand_011_pos of 5ebd13aa9, amplified.  Not part of any tree:
#	VM-TESTS-106-rzx.md runs it from its own runfile.
#
#	Once the expansion starts, every read of one healthy child takes at
#	least 1 s (zinject -D).  A reflow row that reads that child stays
#	locked as writer until the read completes, on z_rd_int.  During
#	each resilver, a zpool clear every 0.3 s reassesses the DTLs, which
#	resumes the expansion while the resilver issues its repair writes.
#	The two waits of each outage are five and four times longer.
#

typeset -r devs=5
typeset -r dev_size_mb=128
typeset -r cycles=4

typeset -a disks
typeset -r kickflag=$TEST_BASE_DIR/rzx-kick
typeset -i kicker=0

original_scrub_after_expand=$(get_tunable SCRUB_AFTER_EXPAND)

function stop_kicker
{
	rm -f $kickflag
	if ((kicker != 0)); then
		wait $kicker
		kicker=0
	fi
}

function cleanup
{
	stop_kicker
	log_must zinject -c all
	poolexists "$TESTPOOL" && zpool status -v "$TESTPOOL"
	poolexists "$TESTPOOL" && log_must_busy zpool destroy "$TESTPOOL"

	for i in {1..$devs}; do
		log_must rm -f "$TEST_BASE_DIR/dev-$i"
	done

	log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 0
	log_must set_tunable32 SCRUB_AFTER_EXPAND $original_scrub_after_expand
}

function expanding # pool
{
	zpool status "$1" | grep -q 'expansion of .* in progress'
}

function paused_for_resilver # pool
{
	zpool status "$1" | grep -q 'paused for resilver or clear'
}

function reflowed # pool
{
	zpool status -jp --json-int "$1" | \
	    jq --arg pool "$1" '.pools[$pool].raidz_expand_stats.reflowed'
}

log_onexit cleanup

for i in {1..$devs}; do
	disks[$i]=$TEST_BASE_DIR/dev-$i
	log_must truncate -s ${dev_size_mb}M ${disks[$i]}
done

log_must set_tunable32 SCRUB_AFTER_EXPAND 0

log_must zpool create -f -o cachefile=none $TESTPOOL raidz1 \
    ${disks[1..$((devs - 1))]}
log_must dd if=/dev/urandom of=/$TESTPOOL/data bs=1M count=128
log_must zpool sync $TESTPOOL

# Stop once past the scratch phase, then once per outage.  What the
# scratch phase copies is not counted, so stay well below the total.
reflow_size=$(get_pool_prop allocated $TESTPOOL)
step=$((reflow_size / (cycles + 3)))
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES $step
log_must zpool attach $TESTPOOL raidz1-0 ${disks[$devs]}
# A reflow row of four blocks or more reads every old child.
log_must zinject -d ${disks[1]} -D 1000:64 -T read $TESTPOOL
wait_raidz_expand_paused $TESTPOOL

child=${disks[2]}
typeset -i paused=0
for i in {1..$cycles}; do
	expanding $TESTPOOL || break
	log_must zpool offline $TESTPOOL $child
	log_must dd if=/dev/urandom of=/$TESTPOOL/outage-$i bs=1M count=1
	log_must zpool sync $TESTPOOL

	# The throttle is compared with the bytes copied in synced txgs, so
	# the reflow stops past it: raise it from what has been copied.
	copied=$(reflowed $TESTPOOL)
	[[ $copied == +([0-9]) ]] || log_fail "outage $i: reflowed '$copied'"
	log_note "outage $i: $copied bytes reflowed"
	log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES $((copied + step))

	typeset -i t0=$SECONDS
	while expanding $TESTPOOL && ! paused_for_resilver $TESTPOOL; do
		if ((SECONDS - t0 > 300)); then
			log_fail "outage $i: expansion did not pause for" \
			    "resilver in 300 seconds"
		fi
		sleep 1
	done
	paused_for_resilver $TESTPOOL && ((paused += 1))

	log_must zpool online $TESTPOOL $child
	# Resume the expansion again and again while the resilver runs.
	log_must touch $kickflag
	(
		typeset -i n=0
		while [[ -e $kickflag ]]; do
			zpool clear $TESTPOOL && ((n += 1))
			sleep 0.3
		done
		log_note "outage $i: $n zpool clear during the resilver"
	) &
	kicker=$!
	log_must zpool wait -t resilver $TESTPOOL
	stop_kicker

	# The clock starts once no resilver is left to wait for.
	t0=$SECONDS
	while paused_for_resilver $TESTPOOL; do
		is_pool_resilvering $TESTPOOL && t0=$SECONDS
		if ((SECONDS - t0 > 120)); then
			log_fail "outage $i: expansion still paused for" \
			    "resilver 120 seconds after the resilver ended"
		fi
		sleep 1
	done
	wait_raidz_expand_paused $TESTPOOL
done

((paused > 0)) || log_fail "no outage paused the expansion"
log_note "$paused outages paused the expansion"

# No resilver is left: the rest of the reflow runs at full speed.
log_must zinject -c all
log_must set_tunable64 RAIDZ_EXPAND_MAX_REFLOW_BYTES 0
log_must zpool wait -t raidz_expand $TESTPOOL
verify_pool $TESTPOOL
log_must check_pool_status $TESTPOOL "scan" "with 0 errors"

log_pass "an amplified expansion paused on an offline child resumes after" \
    "its resilver"
```
<!-- raidz_expand_011_amp.ksh: end -->

## Watching for a hang, and what a hang means here

A hung pool cannot be exported and its module cannot be unloaded: nothing
short of a reboot frees them, and only the operator reboots this VM. So every
run is watched while it runs, the evidence is captured while the kernel still
holds it, and the first hang ends the task.

- `run.sh` sets `kernel.hung_task_timeout_secs` to 30 before each 011 run,
  and to 120 (the kernel's default, and CI's) before the `raidz` group, whose
  long tests can keep a txg or a `zpool` command waiting 30 s on a busy pool
  with no deadlock. STEP 7 puts back the value STEP 4 found.
- It starts `zfs-tests.sh` in the background under an outer `timeout`, and
  every 5 s reads the kernel log since the run started and the `z_rd_int`
  kstats. A hung-task line (`INFO: task <comm>:<pid> blocked for more than
  ...`, `z_rd_int`'s if several come at once), or `z_rd_int` holding queued
  tasks without executing one for 120 s, raises a suspicion and a first
  capture (`<label>-hang1*`).
- 60 s later, the reported task still in D state with the same number of
  context switches, or `z_rd_int` still executing nothing, is a hang: a
  second capture (`<label>-hang2*`), then the run is ended by a TERM to its
  `timeout` (which sends KILL 60 s later). Otherwise the suspicion is
  recorded as "not a hang" and the watch goes on; the hung-task line still
  stops the task after the run, as any kernel finding does.
- A capture reads no pool through `zpool` or `zfs`, which would wait on the
  hung pool's locks: the `z_rd_int`, `z_wr_int`, `z_wr_iss` and `z_rd_iss`
  kstats (`threads_active`, `tasks_pending`, `tasks_total`,
  `tasks_executed`, ...) and the rows of `/proc/spl/kstat/taskq/summary`
  with work; each pool's last txgs; every D-state task with its
  `/proc/<pid>/stack`; `echo w > /proc/sysrq-trigger` (the kernel's dump of
  its blocked tasks), read back from the journal; the whole `dmesg`; the
  last 400 lines of `/proc/spl/kstat/zfs/dbgmsg`.
- The 011 runs pass `-D` to `zfs-tests.sh`, which copies each test's output
  to the run's console log as it comes: test-runner writes a test's result
  files only when the test ends, so a hung run keeps its last lines there
  alone.
- `run.sh` then writes `$L/WEDGED` and exits 4. From then on no `zpool`,
  `zfs`, `zdb`, `zinject`, `zstream` or `zfs.sh` command may run: each would
  wait on the hung pool, some of them in D state for good. STEP 7's wedged
  branch records what is left, assembles `REPORT.txt` and removes nothing.

Predicted for a hang of the amplified test on M: hung-task lines for
`z_rd_int`, `txg_sync` and `raidz_expand` within 30 to 60 s of the deadlock,
as in CI, and for `zpool` too: the background `zpool clear`, which waits in
`spa_vdev_state_enter()` as writer of `SCL_STATE` while the stuck row holds
it as reader. For a hang of the unmodified test, the first three.

**After a hang the VM needs a reboot by the operator; never reboot from this
document.** Before the reply, `REPORT.txt` must hold all that the reboot
loses: the captures do (above), and STEP 7 adds the D-state tasks and the
module's state. Everything of the task stays on disk. If `/var/log/journal`
exists (STEP 0 records it), the kernel log of the hung boot stays readable
after the reboot with `journalctl -k -b -1`. A clean shutdown may stall
while unmounting `/testpool`; a reset of the VM from the hypervisor then
completes it. STEP 7 gives the cleanup to run after the reboot.

## Rules for the whole run

- Run everything as root, in bash, in this order. The ZTS runs go through
  `runuser -u zts` inside `run.sh`; nothing else runs as `zts`. Shell
  variables may not survive between your commands, so every block below
  starts by sourcing `/var/log/zts-rzx-1028/env.sh` or by setting what it
  needs.
- A production hypervisor sits underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads and every ZTS run and pool step go through `nice -n 10 ionice
  -c3` too (the blocks and the scripts do it), one at a time, with nothing
  else running beside them.
- Post nothing, push nothing, install nothing (no package, bpftrace
  included, and no `make install`), never reboot, and never run
  `scripts/zfs-helpers.sh` or `zfs-tests.sh -x`. Rebuild nothing once STEP 2
  has built the trees. `scripts/zfs.sh -r` (reload) and `-u` (unload) run
  only here, on this VM, and only in the blocks that say so, behind their
  guards.
- Work only under `/var/tmp/zts-rzx-1028` (W) and `/var/log/zts-rzx-1028`
  (L). Never write to, or delete, `/var/tmp/test_results`, another
  `/var/tmp/zts-*` or `/var/log/zts-*` directory, another
  `/var/tmp/vm-task-*` checkout, or anything else this task did not create;
  the doc checkout is only read until STEP 7 removes it. ZTS pools mount
  under `/testpool*` while they exist; the tests' cleanup removes those
  directories, and `run.sh` checks that it did.
- The only system settings written are
  `/sys/module/zfs/parameters/zio_taskq_batch_pct` (STEPs 4 and 6),
  `/proc/sys/kernel/hung_task_timeout_secs` (`run.sh`; STEP 7 puts it back)
  and `/proc/sysrq-trigger`, with `w` only (`run.sh`'s captures, on a
  suspected hang). Write no other parameter: not `spl_taskq_kick` above all,
  which would add a thread to a stuck taskq and could undo the evidence.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the group's pools in it while they exist and truncates it when
  they are gone; the 011 tests and `lay.sh` use `cachefile=none`.
- Never run a `zpool`, `zfs`, `zdb`, `zinject` or `zstream` command while
  none of this task's modules is loaded: libzfs would `modprobe` whatever zfs
  module the system has. Once `$L/WEDGED` exists, run none of them at all,
  nor `zfs.sh`. Never print the environment (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- The only `rm -rf` allowed is the last block of STEP 7. The only other
  removal is `lay.sh`'s own, of its directory `$W/chk` (`rm -r`).
- If a check does not print what its comment or the Expected lines say, stop
  at once and report. Do not rebuild, repair or retry. If your permission
  system refuses a command, do not work around it: report it and stop there.
  A kernel panic, oops, `VERIFY`, `ASSERT`, `BUG:` or hung-task message means
  stop and report, in whatever state it happened; never reboot. On M, a hang
  is the predicted result, and it ends the task the same way. A stop in STEP 0
  changes nothing: report and reply. After any later stop, go straight to
  STEP 7 with what you have. Never kill a D-state task, and never try to free
  a hung pool (`zpool clear`, export, `zinject -c`, `rmmod`): the hang is the
  evidence. A test result other than the expected one is a result, recorded,
  never a stop by itself.
- The two builds take about 30 minutes together, B's amplified series 25 to
  50, the group 15 to 30. If your shell tool cannot wait that long for a
  command in the foreground, start that one command with the tool's own
  background mode and wait until it exits; do not use `&` or `nohup`, and run
  nothing else meanwhile. (`run.sh` starts `zfs-tests.sh`, and bpftrace, in
  the background itself, and waits for both before it exits.)
- The session cannot run anything after its final reply: the last block of
  STEP 7 is the last command, and the reply comes after it. Leave no
  background command, loop or watcher behind.

## What earlier runs on this VM taught, and these steps already handle

- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines in
  `zfs_config.h` (one unconditional, two under `#ifdef`): 3 is a debug build.
- objtool prints about 83 `warning:` lines per tree (`spl_panic()` and
  `luaD_throw()` lacking `__noreturn`), harmless. Compiler warnings are
  counted only in the `file:line:col: warning:` form.
- A `-t` run names its test with `.ksh` in its `Test:` line, a group or
  runfile run without it, and a test of a `linux.run` section is printed
  `Test (Linux):`: every pattern below allows all three. A literal
  parenthesis in a regex is written `[(]`, never `\(`.
- A passing test's end line is its `log_pass` message: the last line before
  `NOTE: Performing local cleanup`. The end lines expected below are those.
- The kernel-log scan drops the ZTS `-K` markers (`ZTS run <test>`) and the
  `spa_import_progress_set_notes_impl` notes. ZTS clears dmesg after a failed
  test, so kernel messages are read per run from `journalctl -k`.
- A test that writes nothing on stderr has no `merged` file: its `stdout` is
  read instead. Each run waits 2 s after it ends: two runs started in the
  same second would share a results directory.
- git refuses to work in a tree owned by another user: all git work happens
  in STEP 1 and STEP 2, before the `chown` to `zts`. `zfs-tests.sh -d` must
  point at a mode 1777 directory.
- This VM has no `60-zvol.rules`; no test run here uses a zvol. The trees are
  not installed, and no test run here sets `compatibility=`.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rzx-1028 L=/var/log/zts-rzx-1028 DOC=/var/tmp/vm-task-106u
date -u '+%F %T'
id -un                        # root
uname -r                      # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release   # Debian GNU/Linux 13 (trixie)
nproc                         # 16: the taskq arithmetic above assumes it
umask                         # 0022
grep -m1 '^model name' /proc/cpuinfo
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zdb zed zstream zinject   # nothing: no system ZFS
pgrep -x zed                  # nothing
ls -l /var/run/zed.pid 2>&1   # No such file or directory: zfs.sh -r and -u signal the pid it names
modprobe -c 2>/dev/null | grep -E '^options (zfs|spl) '   # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|autogen|configure|make|gcc|cc1|raidz_test|bpftrace'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -d $W $L 2>&1              # No such file or directory, twice
ls -A /var/tmp; ls -d /var/log/zts-* 2>&1
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|zts-rzx' /proc/mounts   # nothing
ls -d /testpool* 2>&1         # No such file or directory
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
df -h /var/tmp /var/log       # need 20G free on /var/tmp
free -h
id zts; sudo -n -l -U zts | grep -i nopasswd
command -v git ksh python3 modinfo sha256sum truncate runuser timeout jq   # nine paths
command -v xxh128sum bpftrace # recorded; bpftrace adds the probes of B's amplified runs
for k in hung_task_timeout_secs hung_task_panic hung_task_warnings hung_task_check_interval_secs \
    panic_on_oops softlockup_panic panic_on_warn sysrq; do
	printf '%s=%s ' $k "$(cat /proc/sys/kernel/$k 2>&1)"
done; echo
cat /sys/kernel/security/lockdown 2>&1   # recorded
ls -d /var/log/journal 2>&1   # recorded: if it exists, the journal survives a reboot
journalctl -k -n 1 --no-pager -q   # one kernel line
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/u/VM-TESTS-106-rzx.md 2>&1
```

Keep this output, which goes into `REPORT.txt`. The number printed after the
one kernel line is the kernel-message baseline. Directories of earlier tasks
under `/var/tmp` and `/var/log` (`zts-*`, `vm-task-*`, `test_results`) are
recorded only: their presence is not a stop, and this task never touches
them. The CPU, `xxh128sum`, `bpftrace`, `lockdown`, `/var/log/journal` and
`kernel.*` lines are recorded too (Debian's defaults are
`hung_task_timeout_secs=120 hung_task_panic=0 hung_task_warnings=10`). The
doc checkout line is expected as `doc checkout: top /var/tmp/vm-task-106u,
branch vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`). If you read
this document from another place, say so in the report, and set `DOCFILE`
in `env.sh` (STEP 1) to that file: STEP 3 extracts the amplified test from
it, and STEP 7 then leaves that place alone and removes no doc checkout.

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, `nproc` is not 16, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, a system `zpool`, `zfs`,
  `zdb`, `zed`, `zstream` or `zinject` is installed, `zed` runs,
  `/var/run/zed.pid` exists, or a `modprobe` option is set for zfs or spl
  (it would apply to every load);
- a test or build process, or a bpftrace, is running. The `pgrep -af` also
  matches the shell that runs this block, because that shell's command line
  contains the pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` exists: this task would then have started once already;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load);
- less than 20 GB is free on `/var/tmp` (two built trees, the clone, the
  harness's three sparse 4 GiB file vdevs, and what the raidz group writes);
- user `zts` is missing or has no passwordless sudo;
- one of the nine commands is missing (`jq`: the 011 tests read the reflow
  progress with it), or `journalctl -k` prints no kernel line (the runs read
  their kernel messages from the journal);
- `hung_task_timeout_secs` is missing (no hung-task detector),
  `hung_task_panic` is not 0 (a hang would panic the VM and lose its state),
  or `hung_task_warnings` is 0 (the kernel reports no more hung tasks this
  boot).

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself:
STEP 2 checks that the modules were built for the running one. `bpftrace`
missing is not a stop: B's amplified runs then go without their probes, and
the report says so.

## STEP 1: directories, `env.sh`, sources (all git work, part one)

The environment file first, then the clone of the branch (read-only towards
GitHub) and the fetch of upstream master, to check the merge base:

```sh
export PATH=/usr/sbin:/sbin:$PATH
mkdir /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028; echo "mkdir rc=$?"   # 0
cat > /var/log/zts-rzx-1028/env.sh <<'EOF'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rzx-1028
L=/var/log/zts-rzx-1028
DOC=/var/tmp/vm-task-106u
DOCFILE=$DOC/u/VM-TESTS-106-rzx.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
BRANCH=raidz-expand-intr-io-start
# The branch's head (one commit) and its parent, upstream master; HP is the
# master of the earlier builds on this VM, whose module code M's is.
HB=5f74c1f61beba246c0bc594c4a9e50cfa01e4b16
HM=5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53
HP=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
# The test (path under tests/zfs-tests/tests/functional, and file), its
# blob in both trees, and the amplified copy (name, sha256).
T011=raidz/raidz_expand_011_pos
F011=tests/zfs-tests/tests/functional/$T011.ksh
B011=5c2e7cdb9317887725854a7dee95b0fa83c55dca
AMP=raidz_expand_011_amp
AMPSHA=a7c70098a56e81a0425783aabad3403b7e8df24ecfc0e7d4da4296fba563aa49
# Runs per tree: the amplified test, and raidz_expand_011_pos.
NA=8
NU=10
# zio_taskq_batch_pct for one z_rd_int, z_wr_int and z_wr_iss thread on
# 16 CPUs, as on the 2-vCPU CI VMs; hung-task timeouts (s) of the 011 runs
# and of the raidz group.
PCT=6
HT_RUN=30
HT_GROUP=120
# Kernel findings: KPAT stops the task (BUG: covers "sleeping function
# called from invalid context"); KWARN lines are quoted, not a stop by
# themselves.  KIGN drops the ZTS -K markers and the import progress notes.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking'
KWARN='WARNING:'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The tunables every run must leave as the load and STEP 4 or 6 set them:
# the taskq layout, the expansion's, the raidz tests' and the deadman's.
TUN='zio_taskq_batch_pct zio_taskq_batch_tpq zio_taskq_write_tpq zio_requeue_io_start_cut_in_line raidz_expand_max_reflow_bytes raidz_expand_max_copy_bytes zfs_scrub_after_expand zfs_embedded_slog_min_ms zfs_prefetch_disable zfs_vdev_removal_max_active zfs_deadman_failmode zfs_dbgmsg_enable'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: $2, expected $3"
	fi
}

tunables() {
	for p in $TUN; do
		if [ -e /sys/module/zfs/parameters/$p ]; then
			printf '%s=%s ' "$p" "$(cat /sys/module/zfs/parameters/$p)"
		else
			printf '%s=absent ' "$p"
		fi
	done
}

# The imported pools, read from the kstat directories: unlike zpool list,
# this never waits on a pool's locks.
pools_k() {
	for d in /proc/spl/kstat/zfs/*/; do
		[ -d "$d" ] && basename "$d"
	done | xargs
}

# What a run may leave in $W/files without stopping the task: the shapes
# of master's known test leaks (tmp.* files of vdevs_in_pool's callers and
# removal_with_zdb, removal_condense_export's testdir1).  Anything else in
# $W/files is a leftover.
files_known() {
	for f in "$W"/files/tmp.?????????? "$W"/files/testdir1; do
		[ -e "$f" ] && echo "${f##*/}"
	done | xargs
}
files_left() {
	ls -A "$W/files" 2>&1 | grep -vxE 'tmp\.[A-Za-z0-9]{10}|testdir1' | xargs
}

# Build <tree> in place, niced: autogen, configure (debug), make -j8.
build_tree() {   # build_tree <B|M>
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug && make -j8") \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/build.txt
		echo "build $1 failed: stop" | tee -a $L/build.txt
		return 1
	fi
}
EOF
. /var/log/zts-rzx-1028/env.sh
date +%s > $L/start.txt
git clone --no-checkout --single-branch --branch $BRANCH $FORK $W/repo > $L/clone.log 2>&1; echo "clone rc=$?"   # 0
git -C $W/repo fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"   # 0
```

Then the head, its parent, what it changes, one worktree per tree, and what
each tree holds:

```sh
. /var/log/zts-rzx-1028/env.sh
{
same "$BRANCH" "$(git -C $W/repo rev-parse origin/$BRANCH)" $HB
same "its parent" "$(git -C $W/repo rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git -C $W/repo merge-base FETCH_HEAD $HB)" $HM
same "files the head changes" "$(git -C $W/repo diff --name-only $HM $HB | xargs)" module/zfs/zio.c
same "diff stat" "$(git -C $W/repo diff --stat $HM $HB | tail -1)" " 1 file changed, 6 insertions(+), 1 deletion(-)"
same "patch-id" "$(git -C $W/repo diff $HM $HB | git -C $W/repo patch-id --stable | cut -d' ' -f1)" b95fc21987f0b59c679e2d5eb7a8cfc4608d45b9
same "the test's blob at both" "$(git -C $W/repo rev-parse $HM:$F011) $(git -C $W/repo rev-parse $HB:$F011)" "$B011 $B011"
same "master's changes outside tests/ since $HP" "$(git -C $W/repo diff --name-only $HP $HM -- . ':!tests' | wc -l)" 0
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
{
for t in M B; do
	cd $W/$t
	echo "$t: $(git log -1 --abbrev=10 --format='%h %s')"
	echo "$t: test $(ls -l $F011 | cut -c1-10) blob $(git hash-object $F011 | cut -c1-10);" \
	    "raidz setup $(ls -l tests/zfs-tests/tests/functional/raidz/setup.ksh | cut -c1-10)," \
	    "cleanup $(ls -l tests/zfs-tests/tests/functional/raidz/cleanup.ksh | cut -c1-10);" \
	    "raidz tests in common.run: $(awk '/^\[tests\/functional\/raidz\]$/ { s = 1; next } s && /^\[/ { exit } s' tests/runfiles/common.run |
	    grep -oE "'[a-z0-9_]+'" | grep -vcE "'(functional|raidz)'")"
	echo "$t: zio.c: requeue with no vdev only x$(grep -c '(stage & ZIO_BLOCKING_STAGES) && zio->io_vd == NULL &&' module/zfs/zio.c)," \
	    "also for an expanding vdev x$(grep -c 'zio->io_vd->vdev_rz_expanding)) &&' module/zfs/zio.c)"
done
same "B differs from M in" "$(git -C $W/B diff --name-only $HM | xargs)" module/zfs/zio.c
same "tracked changes in B, in M" "$(git -C $W/B status --short --untracked-files=no | wc -l) $(git -C $W/M status --short --untracked-files=no | wc -l)" "0 0"
} | tee $L/trees.txt
```

Expected, in order:

```
ok: raidz-expand-intr-io-start
ok: its parent
ok: merge base with upstream master
ok: files the head changes
ok: diff stat
ok: patch-id
ok: the test's blob at both
ok: master's changes outside tests/ since a128b8a075b5b13c737e56914cd7043bd3e9ebf8
worktree M rc=0 at 5ebd13aa9a
worktree B rc=0 at 5f74c1f61b
M: 5ebd13aa9a ZTS: sync checksum test data before fault injection
M: test -rwxr-xr-x blob 5c2e7cdb93; raidz setup -rwxr-xr-x, cleanup -rwxr-xr-x; raidz tests in common.run: 17
M: zio.c: requeue with no vdev only x1, also for an expanding vdev x0
B: 5f74c1f61b Do not start I/O to an expanding RAID-Z vdev from an interrupt thread
B: test -rwxr-xr-x blob 5c2e7cdb93; raidz setup -rwxr-xr-x, cleanup -rwxr-xr-x; raidz tests in common.run: 17
B: zio.c: requeue with no vdev only x0, also for an expanding vdev x1
ok: B differs from M in
ok: tracked changes in B, in M
```

Any `MISMATCH` (the branch was force-pushed or rebased since 28 Oct), or any
other difference, means stop.

## STEP 2: builds, checks, and the hand-over to `zts` (all git work, part two)

Build B, then M (about 30 minutes together; see the rule about long
commands):

```sh
. /var/log/zts-rzx-1028/env.sh
build_tree B && build_tree M
```

Expected: `build B rc=0 wall=<s>s`, then `build M rc=0 wall=<s>s`. A failed
build prints its `error:` lines and `build <tree> failed: stop`: stop.

Then check what was built:

```sh
. /var/log/zts-rzx-1028/env.sh
{
for t in B M; do
	cd $W/$t
	same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
	same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h)" 3
	same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)" "$(uname -r)"
	same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb zinject raidz_test \
	    scripts/zfs.sh scripts/zfs-tests.sh tests/test-runner/bin/test-runner.py $F011 \
	    tests/zfs-tests/tests/functional/raidz/setup.ksh tests/zfs-tests/tests/functional/raidz/cleanup.ksh; do
		[ -e $f ] || echo $f; done | xargs)" ""
	modinfo -F srcversion module/zfs.ko > $L/srcv-$t
	echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
	    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:')" \
	    "(objtool's, about 83 expected); srcversion $(cat $L/srcv-$t)"
	grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
	echo "$t last build line: $(tail -n 1 $L/build-$t.log)"
done
same "B and M srcversions differ" "$(sort -u $L/srcv-B $L/srcv-M | wc -l)" 2
echo "M srcversion $(cat $L/srcv-M); every earlier build of this master module code on this VM: 8852710DB871A7587B9E722"
} 2>&1 | tee $L/step2.txt
```

Expected: 9 `ok:` lines (4 per tree, then the srcversions), for each tree
`compiler warnings: 0` and about 83 other `warning:` lines, a last build line
of a finished build (typically `make[1]: Leaving directory
'/var/tmp/zts-rzx-1028/<B or M>'`), and an M srcversion of
`8852710DB871A7587B9E722`. Any `MISMATCH` means stop. A compiler warning
count other than 0 is not a stop: its first lines are printed, quote them in
the report. Another M srcversion is not a stop either, provided every check
here and in STEP 1 passed: report it.

Then point both trees' results at `$L/results`, store the two srcversions for
the guards, and hand the work directory to `zts`. This is the last git
command of the task but STEP 7's check of the doc checkout:

```sh
. /var/log/zts-rzx-1028/env.sh
{
for t in B M; do
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    $W/$t/tests/test-runner/bin/test-runner.py.in \
	    $W/$t/tests/test-runner/bin/test-runner.py
	same "$t results directory" \
	    "$(grep -c "^BASEDIR = '$L/results'\$" $W/$t/tests/test-runner/bin/test-runner.py)" 1
	same "$t changes now" "$(git -C $W/$t diff HEAD --name-only | xargs)" tests/test-runner/bin/test-runner.py.in
done
same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
same "M head" "$(git -C $W/M rev-parse HEAD)" $HM
} | tee $L/step2b.txt
if ! grep -q '^MISMATCH' $L/sha-checks.txt $L/trees.txt $L/step2.txt $L/step2b.txt &&
    [ -s $L/srcv-B ] && [ -s $L/srcv-M ] && ! cmp -s $L/srcv-B $L/srcv-M; then
	printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh &&
	    echo "srcversions recorded"
else
	echo "CHECKS FAILED: stop"
fi
tail -n 2 $L/env.sh
mkdir $W/files $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
chown -R zts $W $L/results; echo "chown rc=$?"                        # 0
stat -c '%U %a %n' $W/files $L/results
```

Expected: 6 `ok:` lines (the `test-runner.py.in` change is the results
directory, and nothing else may differ), `srcversions recorded`, the two lines
`SV_B=<B's>` and `SV_M=<M's>`, `mkdir rc=0`, `chown rc=0`, then
`zts 1777 /var/tmp/zts-rzx-1028/files` and
`zts 755 /var/log/zts-rzx-1028/results`. On `CHECKS FAILED`, stop.

## STEP 3: the scripts, the amplified test and its runfile

`run.sh` runs one ZTS run on the loaded tree, as `zts`, behind the srcversion
guard, watches it for a hang (see "Watching for a hang"), then writes its
digest, reads its kernel messages from the journal and checks its leftovers:

```sh
cat > /var/log/zts-rzx-1028/run.sh <<'EOS'
#!/bin/sh
# usage:
#   run.sh <B|M> <label> amp           : the amplified test, from its runfile
#   run.sh <B|M> <label> test <path>   : one test of the tree (-t); <path> is
#                                        under tests/zfs-tests/tests/functional,
#                                        without .ksh
#   run.sh <B|M> <label> group raidz   : the raidz group (-T raidz)
# One zfs-tests.sh run on the loaded tree, watched for a hang, then its
# digest, its kernel messages and its leftovers.  Exit 0: the run is clean,
# whatever the test results; 1: a guard failed, nothing ran; 2: a bad
# argument; 3: a leftover, a changed tunable, a kernel message, no ZTS mark
# or an outer timeout; 4: a hang, or a pool left after an outer timeout:
# no zpool, zfs, zdb or zinject command may run any more.
. /var/log/zts-rzx-1028/env.sh
t=$1; lbl=$2; mode=$3; what=$4
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> amp | test <path> | group raidz"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
case $mode in
amp)
	if [ ! -x "$W/amp/$AMP.ksh" ] || [ ! -r "$W/amp/amp.run" ]; then
		echo "no amplified test or runfile in $W/amp"; exit 2
	fi
	sel="-D -r $W/amp/amp.run"; outer=2400; ht=$HT_RUN ;;
test)
	case $what in
	''|/*|*..*|*[!A-Za-z0-9_/-]*) echo "bad test path: '$what'"; exit 2 ;;
	esac
	if [ ! -x "$W/$t/tests/zfs-tests/tests/functional/$what.ksh" ]; then
		echo "no such test in $t: functional/$what.ksh"; exit 2
	fi
	sel="-D -t tests/functional/$what.ksh"; outer=1200; ht=$HT_RUN ;;
group)
	if [ "$what" != raidz ]; then
		echo "the only group is raidz"; exit 2
	fi
	sel="-T raidz"; outer=5400; ht=$HT_GROUP ;;
*) echo "mode is amp, test or group"; exit 2 ;;
esac
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi
G=$L/guards.txt
if [ -e "$L/WEDGED" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: $(cat "$L/WEDGED"):" \
	    "nothing run" | tee -a "$G"
	exit 1
fi

# The result file of test <name> in directory <dir> (the last component of
# its directory): its "merged" file, else its "stdout".
outfile() {   # outfile <results dir> <dir> <name>
	for f in merged stdout; do
		o=$(find "$1" -type f -name "$f" 2>/dev/null |
		    grep -E "/$2(:Linux)?/$3([.]ksh)?/$f\$" | head -n 1)
		if [ -n "$o" ]; then
			echo "$o"; return 0
		fi
	done
	return 1
}

# The lines of a result file before the failure callbacks and the cleanup,
# and the last of them: a passing test's is its log_pass message.
body() {   # body <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { exit } { print }' "$1"
}
endline() {   # endline <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
		print "  end line: " prev; exit }
	    NF { prev = $0 }' "$1"
}

# How far a run of raidz_expand_011_pos or of the amplified copy got: the
# injection, the notes of each outage, the lines of a failure, the end line.
d011() {   # d011 <file>
	body "$1" | grep -E 'SUCCESS: zinject |NOTE: outage [0-9]+: |NOTE: [0-9]+ outages paused' |
	    sed -E 's/^(SUCCESS|NOTE): /  /'
	body "$1" | grep -E 'ERROR: |^outage [0-9]+: |no outage paused|neither paused nor|Unexpected CKSUM' |
	    head -n 8 | sed 's/^/  line: /'
	endline "$1"
}

# "<name>:max=<threads_max>" of each z_rd_int, z_wr_int and z_wr_iss
# taskq: empty while no pool exists.
tq_layout() {
	for f in /proc/spl/kstat/taskq/z_rd_int* /proc/spl/kstat/taskq/z_wr_int* \
	    /proc/spl/kstat/taskq/z_wr_iss*; do
		[ -e "$f" ] && awk -v n="${f##*/}" '$1 == "threads_max" { printf "%s:max=%s ", n, $3 }' "$f"
	done
}
# "<tasks_executed> <tasks queued>" summed over the z_rd_int taskqs.
tqv() {
	cat /proc/spl/kstat/taskq/z_rd_int* 2>/dev/null | awk '
	    $1 == "tasks_executed" { e += $3 } $1 == "tasks_total" { q += $3 }
	    END { print e + 0, q + 0 }'
}
alive() {   # alive <pid>: running, not a zombie
	s=$(awk '/^State:/ { print $2 }' "/proc/$1/status" 2>/dev/null)
	[ -n "$s" ] && [ "$s" != Z ]
}
csw() {   # csw <pid>: "<state> <context switches>"
	awk '/^State:/ { s = $2 } /^(voluntary|nonvoluntary)_ctxt_switches:/ { n += $2 }
	    END { print (s == "" ? "gone" : s), n + 0 }' "/proc/$1/status" 2>/dev/null
}

# Evidence of a hang, read without any zpool or zfs command (a hung pool
# blocks them): the taskq kstats, each pool's last txgs, every D-state
# task and its stack; then the blocked tasks the kernel dumps on sysrq w,
# the whole ring buffer and the tail of the debug log.
capture() {   # capture <prefix>
	{
	echo "== $(date -u '+%F %T'), uptime $(cut -d' ' -f1 /proc/uptime) s"
	echo "== taskq kstats"
	grep -H . /proc/spl/kstat/taskq/z_rd_int* /proc/spl/kstat/taskq/z_wr_int* \
	    /proc/spl/kstat/taskq/z_wr_iss* /proc/spl/kstat/taskq/z_rd_iss* 2>&1 |
	    grep -E ':(threads_max|threads_active|threads_idle|threads_total|tasks_pending|tasks_priority|tasks_total|tasks_delayed|tasks_executed) '
	echo "== taskqs with tasks queued or threads active"
	awk -F'|' 'NR <= 3 { print; next }
	    { a = $2; gsub(/[][]/, " ", a); split(a, x, " ")
	      b = $3; gsub(/[][]/, " ", b); split(b, y, " ")
	      if (x[2] > 0 || y[1] > 0) print }' /proc/spl/kstat/taskq/summary 2>&1
	echo "== last txgs"
	for d in /proc/spl/kstat/zfs/*/; do
		[ -e "${d}txgs" ] && { echo "${d}txgs"; tail -n 6 "${d}txgs"; }
	done
	echo "== D-state tasks"
	for p in $(ps -eo pid=,stat= | awk '$2 ~ /^D/ { print $1 }'); do
		echo "-- pid $p $(cat "/proc/$p/comm" 2>/dev/null), $(csw "$p")"
		cat "/proc/$p/stack" 2>&1
	done
	} > "$1.txt" 2>&1
	s=$(date +%s)
	echo w > /proc/sysrq-trigger
	sleep 3
	journalctl -k --since "@$s" --no-pager -q > "$1-sysrq.txt" 2>&1
	dmesg > "$1-dmesg.txt" 2>&1
	tail -n 400 /proc/spl/kstat/zfs/dbgmsg > "$1-dbgmsg.txt" 2>&1
}

# The first hung-task report of z_rd_int in a kernel log, as its frames
# from the top, without the unreliable "?" ones.
zstack() {   # zstack <kernel file>
	awk '/INFO: task z_rd_int:[0-9]+ blocked for more than/ { s = 1; next }
	    s && /INFO: task / { exit }
	    s && /Call Trace:/ { c = 1; next }
	    s && c && /<\/TASK>/ { exit }
	    s && c {
		sub(/^.* kernel: +/, ""); sub(/^ +/, "")
		if ($0 ~ /^\?/ || $0 ~ /^<TASK>/ || $0 == "") next
		sub(/\+0x.*$/, ""); printf "%s%s", sep, $0; sep = " <- " }
	    END { if (sep != "") print "" }' "$1"
}

# Guards before the run.  The loaded module comes first, so that no zpool
# command runs without this tree's module; then the tunables as recorded
# after the load, no pool, nothing left in the work directory, no loop
# device, no test mount and no test mount point.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion=$cur want=$sv:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pk=$(pools_k)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(files_left)
loops=$(losetup -a 2>&1)
mnts=$(grep -E ' zfs |testpool' /proc/mounts)
roots=$(ls -d /testpool* 2>/dev/null | xargs)
echo "$(date -u +%T) before $lbl ($t $mode $what): srcversion=$cur" \
    "pools=[$pools] kstat_pools=[$pk] files=[$files] known=[$(files_known)]" \
    "loops=[$loops] mounts=[$mnts] rootdirs=[$roots]" | tee -a "$G"
if [ "$tun" != "$(cat "$L/tun-$t.txt" 2>&1)" ] ||
    [ -n "$pools$pk$files$loops$mnts$roots" ]; then
	echo "GUARD FAILED before $lbl: tunables=[$tun]: nothing run" | tee -a "$G"
	exit 1
fi
echo "$ht" > /proc/sys/kernel/hung_task_timeout_secs
if [ "$(cat /proc/sys/kernel/hung_task_timeout_secs)" != "$ht" ]; then
	echo "GUARD FAILED before $lbl: hung_task_timeout_secs is not $ht:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi

start=$(date +%s)
cd "$W/$t" || exit 1
# B's amplified runs only: who takes the expansion's range lock (rl.bt).
bt=""
if [ "$mode" = amp ] && [ "$t" = B ] && [ -n "$BPFTRACE" ]; then
	"$BPFTRACE" "$L/rl.bt" > "$L/$lbl-bt.txt" 2>&1 &
	bt=$!
	sleep 15
	if ! alive "$bt"; then
		wait "$bt"
		echo "bpftrace exited before the run, rc=$?" >> "$L/$lbl-bt.txt"
		bt=""
	fi
fi
t0=$(date +%s)
# $sel is several words on purpose.  -D (the 011 runs) copies the
# test's output to the console log as it comes: a hung run keeps its last
# lines there, since the runner writes its result files only at the end.
# shellcheck disable=SC2086
timeout -k 60 "$outer" nice -n 10 ionice -c3 \
    runuser -u zts -- ./scripts/zfs-tests.sh -vKO -d "$W/files" \
    $sel > "$L/$lbl.log" 2>&1 &
tp=$!

# The watch, every 5 s.  A hung-task line, or z_rd_int queuing tasks
# without executing one for 120 s, starts a suspicion and a first capture.
# 60 s later, the hung task still in D state with no context switch, or
# z_rd_int still stalled, is a hang: a second capture, then the run is
# ended; nothing else ends it early.
layout=""; nh=0; seen=""; why=""; hpid=""; hcsw=""
hang=no; prev=""; st=""; ste=""
while alive "$tp"; do
	sleep 5
	[ -n "$layout" ] || layout=$(tq_layout)
	now=$(date +%s)
	# shellcheck disable=SC2046
	set -- $(tqv)
	if [ "$2" -gt 0 ] && [ "$1" = "$prev" ]; then
		[ -n "$st" ] || { st=$now; ste=$1; }
	else
		st=""
	fi
	prev=$1
	if [ -z "$seen" ]; then
		hl=$(journalctl -k --since "@$start" --no-pager -q 2>/dev/null |
		    grep 'blocked for more than')
		n=$(printf '%s' "$hl" | grep -c .)
		if [ "$n" -gt "$nh" ]; then
			# z_rd_int's line if it is among the new ones.
			h=$(printf '%s\n' "$hl" | sed -n "$((nh + 1)),\$p" |
			    awk '/task z_rd_int:/ { print; exit } NR == 1 { f = $0 }
			    END { if (f != "") print f }' | head -n 1)
			nh=$n
			hpid=$(echo "$h" | sed -n 's/.*INFO: task .*:\([0-9][0-9]*\) blocked for more than.*/\1/p')
			[ -n "$hpid" ] && hcsw=$(csw "$hpid")
			seen=$now; why="hung task: ${h##*INFO: }"
		elif [ -n "$st" ] && [ $((now - st)) -ge 120 ]; then
			hpid=""; seen=$now
			why="z_rd_int executed nothing for $((now - st)) s with $2 tasks queued"
		fi
		if [ -n "$seen" ]; then
			echo "$(date -u +%T) $lbl: suspected hang: $why" | tee -a "$G"
			capture "$L/$lbl-hang1"
		fi
	elif [ $((now - seen)) -ge 60 ]; then
		if [ -n "$hpid" ]; then
			c=$(csw "$hpid")
			case $c in
			D\ *) [ "$c" = "$hcsw" ] && hang=yes ;;
			esac
			v="task $hpid: $hcsw then $c"
		else
			[ -n "$st" ] && [ "$1" = "$ste" ] && hang=yes
			v="z_rd_int: executed $ste then $1, $2 queued"
		fi
		if [ "$hang" = yes ]; then
			echo "$(date -u +%T) $lbl: HANG ($v): second capture, then the" \
			    "run is ended" | tee -a "$G"
			capture "$L/$lbl-hang2"
			kill -TERM "$tp" 2>/dev/null
			break
		fi
		echo "$(date -u +%T) $lbl: not a hang ($v): watching on" | tee -a "$G"
		seen=""; hpid=""
	fi
done
wait "$tp"
rc=$?
wall=$(( $(date +%s) - t0 ))
if [ -n "$bt" ]; then
	kill -INT "$bt" 2>/dev/null
	i=0
	while alive "$bt" && [ $i -lt 30 ]; do
		sleep 1; i=$((i + 1))
	done
	alive "$bt" && kill -KILL "$bt" 2>/dev/null
	wait "$bt"
fi
# Two runs started in the same second would share a results directory.
sleep 2
wedged=no
if [ "$hang" = yes ]; then
	wedged=yes
elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	[ -n "$(pools_k)" ] && wedged=yes
fi

# Results.  Each "Test:" / "Test (Linux):" line starts with a timestamp,
# which is dropped.  A -t run names the test with ".ksh", the others not.
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
grep -vE '\[PASS\]$' "$L/$lbl-tests.txt" > "$L/$lbl-notpassed.txt"
# "<dir> <name>" of every FAIL or KILLED result.
grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-notpassed.txt" |
    sed -E 's|^.*/([^/ ]+)/([^/ ]+) [(]run as .*$|\1 \2|; s|:Linux | |; s|[.]ksh$||' \
    > "$L/$lbl-failed.txt"
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
{
echo "$lbl: tree $t, $sel, $(wc -l < "$L/$lbl-tests.txt") results:" \
    "${cnt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
echo "${miss:-no Missing util(s) line}"
echo "  taskqs: ${layout:-not sampled}"
cat "$L/$lbl-tests.txt"
if [ "$mode" = group ]; then
	sed -n '/^Results Summary/,/^Percent passed/p' "$L/$lbl.log"
	sed -n '/^Tests with /,$p' "$L/$lbl.log"
fi
while read -r dn n; do
	o=""
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" "$dn" "$n")
	if [ -n "$o" ]; then
		cp "$o" "$L/$lbl-$n-out.txt"
		echo "$n (failed):"
		grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
		endline "$o"
	else
		echo "$n (failed): no stdout or merged file found"
	fi
done < "$L/$lbl-failed.txt"
o=""
if [ "$mode" = amp ]; then
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" amp "$AMP")
elif [ "$mode" = test ] && [ "$what" = "$T011" ]; then
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" raidz raidz_expand_011_pos)
fi
if [ -n "$o" ]; then
	cp "$o" "$L/$lbl-011-out.txt"
	d011 "$o"
elif [ "$mode" != group ]; then
	echo "  no stdout or merged file found for the test"
fi
if [ -s "$L/$lbl-bt.txt" ]; then
	f=$L/$lbl-bt.txt
	grep -q '^attached$' "$f" || echo "  bpftrace: not attached: $(head -c 300 "$f" | tr '\n' ' ')"
	for m in starts locks waits50 maxms; do
		echo "  bpftrace @$m: $(sed -n "s/^@$m\[\(.*\)\]: \([0-9]*\)\$/\1=\2/p" "$f" | sort | xargs)"
	done
	echo "  bpftrace on *_int threads: $(for m in starts locks waits50; do
		printf '%s %s, ' "$m" "$(sed -n "s/^@$m\[z_[a-z_]*_int\]: \([0-9]*\)\$/\1/p" "$f" |
		    awk '{ s += $1 } END { print s + 0 }')"; done)"
fi
if [ -e "$L/$lbl-hang1.txt" ]; then
	echo "  suspected hang: $why"
	echo "  hang: $hang; z_rd_int stack: $(zstack "$L/$lbl-kernel.txt")"
	z=$(zstack "$L/$lbl-kernel.txt")
	echo "  stack: rangelock=$(echo "$z" | grep -q 'zfs_rangelock_enter' && echo yes || echo no)" \
	    "raidz_io_start=$(echo "$z" | grep -q 'vdev_raidz_io_start' && echo yes || echo no)" \
	    "mirror_io_done=$(echo "$z" | grep -q 'vdev_mirror_io_done' && echo yes || echo no)"
	for c in hang1 hang2; do
		[ -e "$L/$lbl-$c.txt" ] && echo "  $c z_rd_int: $(grep -E '/z_rd_int[^:]*:(threads_active|tasks_pending|tasks_total|tasks_executed) ' "$L/$lbl-$c.txt" |
		    sed -E 's|^.*/||; s/ +[0-9]+ +/=/' | xargs)"
	done
	echo "  D-state tasks at hang2: $(grep '^-- pid ' "$L/$lbl-hang2.txt" 2>/dev/null | sed 's/^-- pid //; s/,.*$//' | xargs)"
fi
} > "$L/$lbl-notes.txt"

# Kernel messages of this run.  "ZTS run" is the -K marker: it proves the
# journal saw the run.
kn=$(grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
kw=$(grep -E "$KWARN" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")
kh=$(grep -c 'blocked for more than' "$L/$lbl-kernel.txt")

# Leftovers: all must be empty, and the tunables as after the load.  No
# zpool command once the pool may be wedged.
if [ "$wedged" = yes ]; then
	lp="(not read: wedged)"
else
	lp=$("$W/$t/zpool" list -H -o name 2>&1)
fi
lpk=$(pools_k)
ll=$(losetup -a 2>&1)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(files_left)
lk=$(files_known)
lm=$(grep -E ' zfs |testpool' /proc/mounts)
lr=$(ls -d /testpool* 2>/dev/null | xargs)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
lD=$(ps -eo pid=,stat=,comm= | awk '$2 ~ /^D/ { printf "%s%s:%s", s, $1, $3; s = " " }')
# Leftover processes, leaving out this script and the shells that started
# it: their command lines can contain the same words.
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests[.]sh|test-runner[.]py|functional/|/amp/|zpool |zinject |bpftrace' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)

echo "$(date -u +%T) after $lbl: results=${cnt:-none} rc=$rc wall=${wall}s" \
    "zts_marks=$km kernel_matches=$kn kernel_warnings=$kw hung_task_lines=$kh" \
    "hang=$hang" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] kstat_pools=[$lpk]" \
    "loops=[$ll] dm=[$ld] files=[$lf] known=[$lk] mounts=[$lm] rootdirs=[$lr]" \
    "procs=[$lq] dstate=[$lD] tunables_same=$ts" | tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ "$kn" -gt 0 ] || [ "$kw" -gt 0 ]; then
	grep -iE "$KPAT|$KWARN" "$L/$lbl-kernel.txt" | grep -vE "$KIGN" | head -n 20
fi
if [ "$wedged" = yes ]; then
	echo "$lbl: hang=$hang rc=$rc, pools [$lpk]" > "$L/WEDGED"
	echo "HANG after $lbl: the pool is wedged; no zpool, zfs, zdb, zinject" \
	    "or zfs.sh command may run any more: go to STEP 7 (wedged)" | tee -a "$G"
	exit 4
fi
if [ -n "$lp$lpk$ll$ld$lf$lm$lr$lq" ] || [ "$ts" != yes ] ||
    [ "$kn" -gt 0 ] || [ "$km" -lt 1 ] || [ "$rc" -eq 124 ] ||
    [ "$rc" -eq 137 ]; then
	echo "STOP after $lbl: leftover, tunable changed, kernel message," \
	    "no ZTS mark in the journal, or outer timeout" | tee -a "$G"
	exit 3
fi
exit 0
EOS
```

`run.sh` exits 0 when the run is clean, whatever the test results; 1 when a
guard before the run failed (nothing ran); 2 on a bad argument; 3 when
something is wrong after the run (a leftover, a changed tunable, a kernel
message, no ZTS mark in the journal, an outer timeout); 4 after a hang, or a
pool left after an outer timeout (`$L/WEDGED` written). On 1, 2 or 3, stop and
report; clean nothing by hand. On 4, go to STEP 7, wedged branch.

`series.sh` runs one series on the loaded tree, one `run.sh` each, and stops
at the first that does not exit 0:

```sh
cat > /var/log/zts-rzx-1028/series.sh <<'EOS'
#!/bin/sh
# usage: series.sh <B|M> <a|u>: on the loaded tree, the amplified test NA
# times (labels ba1... or ma1...) or raidz_expand_011_pos NU times (bu01...
# or mu01...); stops at the first run.sh that does not exit 0.
. /var/log/zts-rzx-1028/env.sh
case $1 in
B) p=b ;;
M) p=m ;;
*) echo "usage: series.sh <B|M> <a|u>"; exit 2 ;;
esac
case $2 in
a) n=$NA ;;
u) n=$NU ;;
*) echo "usage: series.sh <B|M> <a|u>"; exit 2 ;;
esac
# One line per run: its results, wall time, paused outages and end line.
summary() {
	j=1
	while [ $j -le "$1" ]; do
		if [ "$2" = a ]; then l=$p$2$j; else l=$p$2$(printf %02d $j); fi
		f=$L/$l-notes.txt
		if [ -e "$f" ]; then
			echo "  $(head -n 1 "$f" | sed -E 's/, log dir .*$//; s/, -[DrtT] [^,]*,/,/');" \
			    "$(grep -m 1 -oE '[0-9]+ outages paused' "$f" || echo 'no paused count');" \
			    "$(grep -m 1 '  end line: ' "$f" | sed 's/^ *//')"
		fi
		j=$((j + 1))
	done
}
i=1
while [ $i -le "$n" ]; do
	if [ "$2" = a ]; then
		l=$p$2$i
		sh "$L/run.sh" "$1" "$l" amp; rc=$?
	else
		l=$p$2$(printf %02d $i)
		sh "$L/run.sh" "$1" "$l" test "$T011"; rc=$?
	fi
	echo "run.sh $l rc=$rc"
	if [ $rc -ne 0 ]; then
		echo "series $1 $2 stopped at $l"
		summary $i "$2"
		exit $rc
	fi
	i=$((i + 1))
done
echo "series $1 $2 done:"
summary "$n" "$2"
exit 0
EOS
```

`lay.sh` checks the taskq layout on a scratch pool, after each load:

```sh
cat > /var/log/zts-rzx-1028/lay.sh <<'EOS'
#!/bin/sh
# usage: lay.sh <B|M>: the zio taskq layout of a pool created now on the
# loaded tree: pool rzxchk on one sparse 256 MiB file in $W/chk,
# cachefile=none, not mounted; its taskq kstats; then destroyed.
. /var/log/zts-rzx-1028/env.sh
t=$1
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: lay.sh <B|M>"; exit 2 ;;
esac
cur=$(cat /sys/module/zfs/srcversion 2>&1)
pct=$(cat /sys/module/zfs/parameters/zio_taskq_batch_pct 2>&1)
if [ "$cur" != "$sv" ] || [ "$pct" != "$PCT" ] || [ -e "$W/chk" ] ||
    [ -n "$(pools_k)" ]; then
	echo "GUARD FAILED: srcversion '$cur' (want $sv), zio_taskq_batch_pct" \
	    "'$pct' (want $PCT), $W/chk or a pool exists: nothing run"
	exit 1
fi
ZP=$W/$t/zpool
{ mkdir "$W/chk" && truncate -s 256m "$W/chk/vdev0"; } ||
    { echo "cannot prepare $W/chk"; exit 1; }
if ! nice -n 10 ionice -c3 "$ZP" create -f -o cachefile=none -m none \
    rzxchk "$W/chk/vdev0"; then
	echo "CREATE FAILED"; rm -r "$W/chk"; exit 1
fi
echo "pools: [$(pools_k)]"
for q in z_rd_int z_wr_int z_wr_iss z_rd_iss; do
	for f in /proc/spl/kstat/taskq/$q*; do
		if [ -e "$f" ]; then
			echo "${f##*/}: $(awk '$1 == "threads_max" { print "threads_max", $3 }' "$f")"
		else
			echo "$q: no kstat"
		fi
	done
done
grep -H . /proc/spl/kstat/taskq/z_rd_int.* 2>&1 |
    grep -E ':(threads_max|threads_active|tasks_pending) '
nice -n 10 ionice -c3 "$ZP" destroy rzxchk; echo "destroyed rzxchk rc=$?"
rm -r "$W/chk" && echo "removed $W/chk"
echo "pools after: [$(pools_k)]"
EOS
```

`rl.bt` is the bpftrace program of B's amplified runs. `zfs_rangelock_enter`
is probed rather than `zfs_rangelock_enter_impl`: `vdev_raidz_io_start()`
calls the former, an exported function of another file, while the latter is
static and may be inlined (CI's stack shows it because
`zfs_rangelock_enter()` tail-calls it):

```sh
cat > /var/log/zts-rzx-1028/rl.bt <<'EOF'
/*
 * During one amplified run on B: which threads start I/O on a raidz vdev
 * (vdev_raidz_io_start), which take the expansion's range lock there
 * (zfs_rangelock_enter called while inside it, the only caller that
 * takes it there), and which wait 50 ms or more for it.
 */
BEGIN
{
	printf("attached\n");
}

kprobe:vdev_raidz_io_start
{
	@starts[comm] = count();
	@in[tid] = 1;
}

kretprobe:vdev_raidz_io_start
{
	delete(@in[tid]);
}

kprobe:zfs_rangelock_enter
/@in[tid]/
{
	@locks[comm] = count();
	@t0[tid] = nsecs;
}

kretprobe:zfs_rangelock_enter
/@t0[tid]/
{
	$ms = (nsecs - @t0[tid]) / 1000000;
	@maxms[comm] = max($ms);
	if ($ms >= 50) {
		@waits50[comm] = count();
	}
	delete(@t0[tid]);
}

END
{
	clear(@in);
	clear(@t0);
}
EOF
```

Then the amplified test, extracted from this document, its runfile, and the
checks:

```sh
. /var/log/zts-rzx-1028/env.sh
{
mkdir $W/amp; echo "mkdir rc=$?"
awk '/^<!-- raidz_expand_011_amp.ksh: begin -->$/ { s = 1; next }
    /^<!-- raidz_expand_011_amp.ksh: end -->$/ { s = 0 } s' "$DOCFILE" |
    sed '1d;$d' > $W/amp/$AMP.ksh && chmod 755 $W/amp/$AMP.ksh
same "the amplified test's sha256" "$(sha256sum < $W/amp/$AMP.ksh | cut -d' ' -f1)" $AMPSHA
ksh -n $W/amp/$AMP.ksh 2>&1 | grep -v 'is slower than'; echo "ksh -n done"
cat > $W/amp/amp.run <<'EOF'
[DEFAULT]
pre =
quiet = False
pre_user = root
user = root
timeout = 1800
post_user = root
post =
tags = ['functional']

[/var/tmp/zts-rzx-1028/amp]
tests = ['raidz_expand_011_amp']
tags = ['functional']
EOF
diff -u $W/M/$F011 $W/amp/$AMP.ksh > $L/amp.diff
echo "amp.diff: $(diff $W/M/$F011 $W/amp/$AMP.ksh | grep -c '^<') lines removed," \
    "$(diff $W/M/$F011 $W/amp/$AMP.ksh | grep -c '^>') added"
ls -l $W/amp | sed 1d
echo "BPFTRACE=$(command -v bpftrace)" >> $L/env.sh; tail -n 1 $L/env.sh
for f in run.sh series.sh lay.sh; do
	bash -n $L/$f; echo "$f bash syntax rc=$?"
	sh -n $L/$f; echo "$f sh syntax rc=$?"
done
} 2>&1 | tee $L/step3.txt
```

Expected: `mkdir rc=0`, `ok: the amplified test's sha256`, `ksh -n done`
with nothing before it, `amp.diff: 25 lines removed, 43 added`, the test
(`-rwxr-xr-x`, 5018 bytes) and the runfile, `BPFTRACE=` followed by
bpftrace's path or by nothing, and six `syntax rc=0` lines. A `MISMATCH` on
the sha256 means the extraction went wrong: stop (do not retype the test).

## STEP 4: load B, force the layout, check it

Load B. Nothing is loaded yet (STEP 0), so the reload loads:

```sh
. /var/log/zts-rzx-1028/env.sh
{
echo "loaded before: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # []
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -r; echo "load B rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"             # the same value twice
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ]; then
	echo "kstat pools: [$(pools_k)]"; $W/B/zpool list -H -o name; echo "zpool list rc=$?"
	echo "zio_taskq_batch_pct as loaded: $(cat /sys/module/zfs/parameters/zio_taskq_batch_pct)"
	echo $PCT > /sys/module/zfs/parameters/zio_taskq_batch_pct; echo "set rc=$?"
	tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
	awk '$3 == "vdev_raidz_io_start" || $3 == "zfs_rangelock_enter" { print $3, $4 }' /proc/kallsyms
fi
[ -e $L/sysctl-orig.txt ] || for k in hung_task_timeout_secs hung_task_panic hung_task_warnings; do
	echo "$k=$(cat /proc/sys/kernel/$k)"
done > $L/sysctl-orig.txt
cat $L/sysctl-orig.txt
} 2>&1 | tee $L/load-B.txt
sh /var/log/zts-rzx-1028/lay.sh B 2>&1 | tee /var/log/zts-rzx-1028/lay-B.txt
```

Expected: `loaded before: []`, `load B rc=0`, the same srcversion twice,
`kstat pools: []`, `zpool list rc=0` with no pool name,
`zio_taskq_batch_pct as loaded: 80`, `set rc=0`, then

```
zio_taskq_batch_pct=6 zio_taskq_batch_tpq=0 zio_taskq_write_tpq=16 zio_requeue_io_start_cut_in_line=1 raidz_expand_max_reflow_bytes=0 raidz_expand_max_copy_bytes=167772160 zfs_scrub_after_expand=1 zfs_embedded_slog_min_ms=64 zfs_prefetch_disable=0 zfs_vdev_removal_max_active=2 zfs_deadman_failmode=wait zfs_dbgmsg_enable=1
```

(the module's defaults, but the 6; other values set by this VM's
configuration are fine, since every run is compared with what was recorded
here, but `zio_taskq_batch_pct` must read 6 and `zfs_deadman_failmode` must
not read `panic`), the two symbols `vdev_raidz_io_start [zfs]` and
`zfs_rangelock_enter [zfs]` (only bpftrace needs them), the three
`kernel.hung_task_*` values as STEP 0 printed them, and from `lay.sh`:

```
pools: [rzxchk]
z_rd_int.<n>: threads_max 1
z_wr_int.<n>: threads_max 1
z_wr_iss.<n>: threads_max 1
z_rd_iss.<n>: threads_max 8
/proc/spl/kstat/taskq/z_rd_int.<n>:threads_max                     4    1
/proc/spl/kstat/taskq/z_rd_int.<n>:threads_active                  4    0
/proc/spl/kstat/taskq/z_rd_int.<n>:tasks_pending                   4    0
destroyed rzxchk rc=0
removed /var/tmp/zts-rzx-1028/chk
pools after: []
```

(`<n>` the taskq's instance, normally 0). With the default layout there
would be `z_rd_int_0.0` to `z_rd_int_2.0` with `threads_max 4` and
`z_wr_iss.0` with 12. If the load fails, the srcversion is not B's, a pool
appears, `zio_taskq_batch_pct` does not read 6 or any taskq but `z_rd_iss`
has another `threads_max` than 1, stop.

## STEP 5: B: the amplified test, the test, the raidz group

The amplified test, 8 times (25 to 50 minutes; see the rule about long
commands):

```sh
L=/var/log/zts-rzx-1028
sh $L/series.sh B a > $L/series-B-a.txt 2>&1; echo "series.sh rc=$?" >> $L/series-B-a.txt; sed -n '/^series /,$p' $L/series-B-a.txt
```

Expected: `series B a done:`, 8 lines

```
  ba<i>: tree B, 1 results: PASS 1; zfs-tests.sh rc=0, wall <s>s; <p> outages paused; end line: an amplified expansion paused on an offline child resumes after its resilver
```

with `<p>` from 1 to 4, then `series.sh rc=0`. Each run's notes
(`$L/ba<i>-notes.txt`, also printed in `series-B-a.txt`) show
`taskqs: z_rd_int.<n>:max=1 z_wr_int.<n>:max=1 z_wr_iss.<n>:max=1`, the
`Test:` line of `$W/amp/raidz_expand_011_amp` with `[PASS]`, then

```
  zinject -d /var/tmp/zts-rzx-1028/files/dev-1 -D 1000:64 -T read testpool
  outage 1: <bytes> bytes reflowed
  outage 1: <n> zpool clear during the resilver
  ... (the same two lines for each outage that ran, up to 4)
  <p> outages paused the expansion
  zinject -c all
  end line: an amplified expansion paused on an offline child resumes after its resilver
```

and no `line:` entry; with bpftrace, the four `bpftrace @...` lines and
`bpftrace on *_int threads: starts <n>, locks 0, waits50 0,`. In
`guards.txt`, each run's `after` line shows `zts_marks=1 kernel_matches=0
kernel_warnings=0 hung_task_lines=0 hang=no`, and its leftovers line is
empty everywhere with `tunables_same=yes`. A result other than PASS is
recorded, not a stop; `series B a stopped at ba<i>` is a stop (the run's own
lines in `series-B-a.txt` say why).

Then `raidz_expand_011_pos` as the tree has it, 10 times (10 to 15 minutes):

```sh
L=/var/log/zts-rzx-1028
sh $L/series.sh B u > $L/series-B-u.txt 2>&1; echo "series.sh rc=$?" >> $L/series-B-u.txt; sed -n '/^series /,$p' $L/series-B-u.txt
```

Expected: `series B u done:`, 10 lines

```
  bu<nn>: tree B, 3 results: PASS 3; zfs-tests.sh rc=0, wall <s>s; <p> outages paused; end line: an expansion paused on an offline child resumes after its resilver
```

then `series.sh rc=0`; in `guards.txt`, `zts_marks=3` and the rest as above.

Then the `raidz` group (15 to 30 minutes):

```sh
L=/var/log/zts-rzx-1028
sh $L/run.sh B bg group raidz > $L/bg-console.txt 2>&1; echo "run.sh rc=$?" >> $L/bg-console.txt; tail -n 40 $L/bg-console.txt
```

Expected: `bg: tree B, -T raidz, 19 results: PASS 19; zfs-tests.sh rc=0`
(`setup`, the 17 tests, `cleanup`), the Results Summary with `PASS 19`,
`zts_marks=19 kernel_matches=0 kernel_warnings=0 hung_task_lines=0 hang=no`,
every leftover empty, `run.sh rc=0`. A test that fails is listed with its
`ERROR:` lines and end line (`<name> (failed):`): it is recorded, and only a
hang or a kernel finding is a stop.

## STEP 6: M: swap, the test, then the amplified test last

Swap the modules only while B's is loaded with no pool, no mount, no `zed`
and no `$L/WEDGED`; `zfs.sh -r` from M's tree unloads B's stack and loads
M's. Then the layout again, since a load puts back the default:

```sh
. /var/log/zts-rzx-1028/env.sh
{
cur=$(cat /sys/module/zfs/srcversion 2>&1)
pk=$(pools_k)
p=""; [ "$cur" = "$SV_B" ] && [ -z "$pk" ] && p=$($W/B/zpool list -H -o name 2>&1)
m=$(grep -E ' zfs |testpool' /proc/mounts)
z=$(pgrep -x zed)
echo "before the swap: B's module loaded: $([ "$cur" = "$SV_B" ] && echo yes || echo "no ($cur)"), wedged [$(cat $L/WEDGED 2>/dev/null)], kstat pools [$pk], pools [$p], mounts [$m], zed [$z]"
if [ "$cur" = "$SV_B" ] && [ ! -e $L/WEDGED ] && [ -z "$pk$p$m$z" ]; then
	(cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh -r); echo "reload with M rc=$?"
else
	echo "not swapped: stop"
fi
echo "loaded $(cat /sys/module/zfs/srcversion 2>&1), SV_M $SV_M"
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ]; then
	echo "kstat pools: [$(pools_k)]"; $W/M/zpool list -H -o name; echo "zpool list rc=$?"
	echo "zio_taskq_batch_pct as loaded: $(cat /sys/module/zfs/parameters/zio_taskq_batch_pct)"
	echo $PCT > /sys/module/zfs/parameters/zio_taskq_batch_pct; echo "set rc=$?"
	tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo
	same "M's tunables as B's" "$(cat $L/tun-M.txt)" "$(cat $L/tun-B.txt)"
fi
} 2>&1 | tee $L/swap.txt
sh /var/log/zts-rzx-1028/lay.sh M 2>&1 | tee /var/log/zts-rzx-1028/lay-M.txt
```

Expected: `before the swap: B's module loaded: yes, wedged [], kstat pools [],
pools [], mounts [], zed []`, `reload with M rc=0`, the same srcversion twice
(M's), `kstat pools: []`, `zpool list rc=0` with no pool name,
`zio_taskq_batch_pct as loaded: 80`, `set rc=0`, the tunables line as B's,
`ok: M's tunables as B's`, and `lay.sh`'s lines as in STEP 4. Anything else
means stop.

Then `raidz_expand_011_pos`, 10 times (10 to 15 minutes; a hang here is
possible, as in CI):

```sh
L=/var/log/zts-rzx-1028
sh $L/series.sh M u > $L/series-M-u.txt 2>&1; echo "series.sh rc=$?" >> $L/series-M-u.txt; sed -n '/^series /,$p' $L/series-M-u.txt
```

Expected: as for B, with `mu<nn>` labels and `series.sh rc=0`; or
`series M u stopped at mu<nn>` with `series.sh rc=4` after a hang (then go to
STEP 7, wedged branch, and run nothing of the next block). Its
`guards.txt` lines and its notes show the suspicion, the HANG line and the
stack, as described for the next block.

Last, the amplified test, up to 8 times: the only step of the task meant to
wedge a pool, so it runs last:

```sh
L=/var/log/zts-rzx-1028
sh $L/series.sh M a > $L/series-M-a.txt 2>&1; echo "series.sh rc=$?" >> $L/series-M-a.txt; sed -n '/^series /,$p' $L/series-M-a.txt
```

What the analysis predicts (record whatever is printed instead):
`series M a stopped at ma<i>` and `series.sh rc=4`, after zero or more lines
like B's for the runs before; in `guards.txt`, for `ma<i>`:

```
<time> ma<i>: suspected hang: hung task: task z_rd_int:<pid> blocked for more than 30 seconds.
<time> ma<i>: HANG (task <pid>: D <n> then D <n>): second capture, then the run is ended
<time> after ma<i>: results=none rc=<n> wall=<s>s zts_marks=1 kernel_matches=<many> kernel_warnings=<n> hung_task_lines=<3 or more> hang=yes
<time> after ma<i> leftovers: pools=[(not read: wedged)] kstat_pools=[testpool] ... dstate=[<pid>:z_rd_int <pid>:txg_sync <pid>:raidz_expand <pid>:zpool ...] ...
HANG after ma<i>: the pool is wedged; ...
```

and in `ma<i>-notes.txt`:

```
  suspected hang: hung task: task z_rd_int:<pid> blocked for more than 30 seconds.
  hang: yes; z_rd_int stack: __schedule <- schedule <- cv_wait_common <- zfs_rangelock_enter_reader <- zfs_rangelock_enter_impl <- vdev_raidz_io_start <- zio_vdev_io_start <- zio_nowait <- vdev_mirror_io_done <- zio_vdev_io_done <- zio_execute <- taskq_thread <- kthread <- ret_from_fork ...
  stack: rangelock=yes raidz_io_start=yes mirror_io_done=yes
  hang1 z_rd_int: z_rd_int.<n>:threads_active=1 z_rd_int.<n>:tasks_pending=<q> z_rd_int.<n>:tasks_total=<q> z_rd_int.<n>:tasks_executed=<e>
  hang2 z_rd_int: z_rd_int.<n>:threads_active=1 z_rd_int.<n>:tasks_pending=<q'> z_rd_int.<n>:tasks_total=<q'> z_rd_int.<n>:tasks_executed=<e>
  D-state tasks at hang2: <pid> z_rd_int <pid> txg_sync <pid> raidz_expand <pid> zpool ...
```

(inlined functions may be missing from the stack; `rangelock`,
`raidz_io_start` and `mirror_io_done` are what matter; the same `<e>` in both
captures with `<q>` > 0 is the stall). The kernel matches are the hung-task
reports and the `sysrq w` dump: they are the evidence here, not a fault of
the run. Then go to STEP 7, wedged branch.

If all 8 runs pass instead (`series M a done:` and `series.sh rc=0`), M did
not hang: record it, and go to STEP 7, normal branch.

## STEP 7: the end: unload or not, final checks, report, cleanup

First the module and the final checks. With `$L/WEDGED` present this runs no
`zpool` command, unloads nothing, and says so:

```sh
. /var/log/zts-rzx-1028/env.sh
{
w=""; [ -e $L/WEDGED ] && w=$(cat $L/WEDGED)
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ "$cur" = "$SV_B" ]; then t=B; elif [ "$cur" = "$SV_M" ]; then t=M; else t=""; fi
pk=$(pools_k)
m=$(grep -E ' zfs |testpool' /proc/mounts)
echo "wedged: [$w]; loaded: [${t:-none} $cur]; kstat pools: [$pk]; mounts: [$m]"
if [ -n "$w" ]; then
	echo "unload skipped: wedged; the VM needs a reboot by the operator"
elif [ -n "$t" ] && [ -z "$pk$m" ] && [ -z "$($W/$t/zpool list -H -o name 2>&1)" ]; then
	(cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # unload M rc=0
elif [ -n "$t" ]; then
	echo "unload skipped: a pool or a mount is left"
else
	echo "no module of this task is loaded (srcversion '$cur')"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # lsmod: [] unless wedged
v=$(sed -n 's/^hung_task_timeout_secs=//p' $L/sysctl-orig.txt 2>/dev/null)
[ -n "$v" ] && echo "$v" > /proc/sys/kernel/hung_task_timeout_secs
echo "hung_task_timeout_secs: $(cat /proc/sys/kernel/hung_task_timeout_secs), as found: ${v:-not recorded}"
journalctl -k --since "@$(cat $L/start.txt)" --no-pager -q > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KPAT" $L/kern-all.txt | grep -vcE "$KIGN") matches," \
    "$(grep -E "$KWARN" $L/kern-all.txt | grep -vcE "$KIGN") warnings," \
    "$(grep -c 'blocked for more than' $L/kern-all.txt) hung-task lines," \
    "$(grep -c 'ZTS run' $L/kern-all.txt) ZTS marks"
grep -iE "$KPAT|$KWARN" $L/kern-all.txt | grep -vE "$KIGN" | grep -vE 'kernel: +(\? )?[a-z_0-9.]+\+0x' | head -n 40
echo "kernel, this boot: $(journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN")"
echo "D-state tasks: [$(ps -eo pid=,stat=,comm= | awk '$2 ~ /^D/ { printf "%s%s:%s", s, $1, $3; s = " " }')]"
ls -l /etc/zfs/zpool.cache 2>&1
ls -d /testpool* $W/chk 2>&1; losetup -a
echo "files: [$(ls -A $W/files)]"
df -h /var/tmp
} 2>&1 | tee $L/end.txt
```

Expected, normal branch: `wedged: []; loaded: [M <SV_M>]; kstat pools: [];
mounts: []`, `unload M rc=0`, `lsmod: []`, `hung_task_timeout_secs: <the
value found>, as found: <the same>`, `kernel, whole task: 0 matches, 0
warnings, 0 hung-task lines, 95 ZTS marks` (57 on B: 8 + 30 + 19; 38 on M:
30 + 8), nothing listed, the boot's count equal to the STEP 0 baseline,
`D-state tasks: []`, `zpool.cache` as in STEP 0 or a 0-byte file, two `No such
file or directory` lines and no loop device, `files: []`.

Expected, wedged branch: `wedged: [<label>: hang=yes rc=<n>, pools
[testpool]]; loaded: [M <SV_M>]; kstat pools: [testpool]; mounts: [<the
/testpool mount>]`, `unload skipped: wedged; ...`, `lsmod: [zfs spl]`, the
hung-task timeout put back, the kernel lines of the hang (the frame lines of
the stacks are left out here; they are in the report's hang sections), the
D-state tasks, `/testpool` still there, perhaps the harness's loop devices,
and in `files:` the test's `dev-1` to `dev-5` and `rzx-kick`. All of it is
recorded, none of it touched.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-rzx-1028/env.sh
{
echo "VM 106: kernel evidence of the RAID-Z expansion deadlock (M = master 5ebd13aa9) and of its fix (B = raidz-expand-intr-io-start 5f74c1f61) (u/VM-TESTS-106-rzx.md)"
for s in sha-checks worktrees trees build step2 step2b step3 load-B lay-B; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
echo; echo "== env.sh from SV_B"; sed -n '/^SV_B=/,$p' $L/env.sh
echo; echo "== amp.diff"; cat $L/amp.diff 2>&1
for s in series-B-a series-B-u; do echo; echo "== $s.txt (summary)"; sed -n '/^series /,$p' $L/$s.txt 2>&1; done
echo; echo "== bg-notes.txt"; cat $L/bg-notes.txt 2>&1
echo; echo "== swap.txt"; cat $L/swap.txt 2>&1
echo; echo "== lay-M.txt"; cat $L/lay-M.txt 2>&1
for s in series-M-u series-M-a; do echo; echo "== $s.txt (summary)"; sed -n '/^series /,$p' $L/$s.txt 2>&1; done
echo; echo "== notes of the amplified runs, B then M"
for f in $L/ba[0-9]-notes.txt $L/ma[0-9]-notes.txt; do
	[ -e "$f" ] && { echo "-- ${f##*/}"; cat "$f"; }
done
echo; echo "== notes of the runs of the test not passed, with no result, or with a suspected hang"
for f in $L/bu[0-9][0-9]-notes.txt $L/mu[0-9][0-9]-notes.txt; do
	[ -e "$f" ] || continue
	l=${f##*/}; l=${l%-notes.txt}
	if [ -s $L/$l-notpassed.txt ] || [ ! -s $L/$l-tests.txt ] || [ -e $L/$l-hang1.txt ]; then
		echo "-- $l"; cat "$f"
	fi
done
for c in $L/*-hang2.txt; do
	[ -e "$c" ] || continue
	l=${c##*/}; l=${l%-hang2.txt}
	echo; echo "== hang of $l: the hung-task reports"
	grep -A 40 'blocked for more than' $L/$l-kernel.txt | head -n 200
	echo; echo "== hang of $l: the two captures (kstats, txgs, D-state tasks and their stacks)"
	cat $L/$l-hang1.txt $L/$l-hang2.txt
	echo; echo "== hang of $l: sysrq w, z_rd_int's part"
	grep -A 30 'task:z_rd_int' $L/$l-hang2-sysrq.txt | head -n 40
	echo; echo "== hang of $l: dbgmsg, last 40 lines"
	tail -n 40 $L/$l-hang2-dbgmsg.txt
	echo; echo "== hang of $l: the last 30 lines of the run's console log"
	tail -n 30 $L/$l.log
done
echo; echo "== guards.txt"; cat $L/guards.txt 2>&1
echo; echo "== end.txt"; cat $L/end.txt 2>&1
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt` after its first line, a
`== STEP 0` section (short: anything found, `uname -r`, `nproc`, the CPU
model, the free space, the `zpool.cache`, `xxh128sum`, `bpftrace`,
`lockdown`, `/var/log/journal`, `kernel.*` and doc checkout lines, the
directories of earlier tasks, and the kernel baseline), and at its end a
`== verdict` section (the Expected block below, then for each of its items
whether it matched, and which case of "What each result means" applies) and
a `== deviations` section (every deviation from these instructions, with its
reason; `none` if there was none). Then print it with
`cat /var/log/zts-rzx-1028/REPORT.txt`.

**Normal branch.** As the very last command, and only if the first block of
this step printed `unload <B or M> rc=0` and `lsmod: []`, remove this task's
two directories and the doc checkout (the doc checkout only if it is a clean
clone of `vm-tasks-1026` holding this document, nothing else):

```sh
cd /
D=/var/tmp/vm-task-106u
if [ -e /var/log/zts-rzx-1028/WEDGED ]; then
	echo "wedged: nothing removed"
elif [ -d $D ] && [ ! -L $D ] && [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
    [ "$(git -C $D rev-parse --abbrev-ref HEAD 2>/dev/null)" = vm-tasks-1026 ] &&
    case "$(git -C $D remote get-url origin 2>/dev/null)" in
    https://github.com/tigerblue77/zfs|https://github.com/tigerblue77/zfs.git) true ;;
    *) false ;; esac &&
    git -C $D ls-files --error-unmatch u/VM-TESTS-106-rzx.md > /dev/null 2>&1 &&
    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
    ! grep -q " ${D}[ /]" /proc/mounts; then
	rm -rf /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028 $D; echo "rm rc=$? (doc checkout included)"
else
	rm -rf /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028; echo "rm rc=$? ($D kept: not a clean checkout of vm-tasks-1026)"
fi
ls -d /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, run nothing of this block: keep everything and
say so. If the permission system refuses this `rm -rf`, leave the directories
as they are and say so in the reply. Do not delete them any other way.

**Wedged branch.** Run nothing more: no cleanup, no unload. The reply then
says, besides `REPORT.txt`: that VM 106 needs a reboot by the operator, the
label of the run that hung and its D-state tasks; that everything of the task
is kept (`/var/tmp/zts-rzx-1028`, `/var/log/zts-rzx-1028`,
`/var/tmp/vm-task-106u`); whether `/var/log/journal` exists (then
`journalctl -k -b -1` shows the hung boot's kernel log after the reboot);
that a clean shutdown may stall unmounting `/testpool`, a reset of the VM
from the hypervisor then completing it; and, for after the reboot, once
`REPORT.txt` has been read, this cleanup (it removes nothing while a zfs
module is loaded):

```sh
if lsmod | grep -qE '^(zfs|spl) ' || [ -e /dev/zfs ]; then
	echo "a zfs module is loaded: nothing removed"
else
	rm -rf /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028 /var/tmp/vm-task-106u
	rmdir /testpool 2>/dev/null
	ls -d /var/tmp/zts-rzx-1028 /var/log/zts-rzx-1028 /var/tmp/vm-task-106u /testpool 2>&1
fi
```

Either way, reply with the whole of `REPORT.txt` as printed, and the result
of the `rm` or the wedged note: nothing runs after the reply.

## Expected, as a block for REPORT.txt

```
STEP 0: free; nproc 16; uname -r recorded; at least 20G free on /var/tmp; jq present; bpftrace
  present or not (recorded); hung_task_panic=0; the doc checkout a clone of vm-tasks-1026 at
  /var/tmp/vm-task-106u.
STEP 1-3: every check ok, no MISMATCH; B = 5f74c1f61, M = 5ebd13aa9, differing in module/zfs/zio.c
  only; builds B and M rc=0; compiler warnings 0, about 83 objtool warning: lines per tree;
  ZFS_DEBUG lines 3; vermagic = uname -r; srcversions differ, M's 8852710DB871A7587B9E722; the
  amplified test's sha256 ok, amp.diff 25 lines removed and 43 added.
STEP 4 and 6 (each load): zio_taskq_batch_pct 80 as loaded, then 6; lay.sh: z_rd_int, z_wr_int and
  z_wr_iss threads_max 1, z_rd_iss 8; M's tunables as B's.

Every run: in guards.txt, zts_marks = its result count, kernel_matches=0, kernel_warnings=0,
  hung_task_lines=0, hang=no, every leftover empty, tunables_same=yes; taskqs: max=1 for z_rd_int,
  z_wr_int and z_wr_iss. "Missing util(s):" may list exportfs; it must not list zdb, zinject,
  zpool, zfs or jq. The one exception is M's hang, below.

ba1-ba8 (B, amplified): PASS 1 each; the zinject -d line; 1 to 4 outages paused; the end line
  "an amplified expansion paused on an offline child resumes after its resilver". With bpftrace:
  on *_int threads, locks 0 and waits50 0; any @waits50 entry in a *_iss thread.
bu01-bu10 (B, raidz_expand_011_pos): PASS 3 each; end line "an expansion paused on an offline
  child resumes after its resilver".
bg (B, -T raidz): PASS 19, zts_marks=19.

mu01-mu10 (M, raidz_expand_011_pos): PASS 3 each, or a hang (as for ma, without the zpool task).
ma1-ma8 (M, amplified): a hang in one of them, the series stopping there with rc=4: hung-task
  lines for z_rd_int (and txg_sync, raidz_expand, zpool); HANG confirmed 60 s later (same context
  switch count, D state); z_rd_int's stack with rangelock=yes raidz_io_start=yes
  mirror_io_done=yes; z_rd_int threads_active=1, tasks queued, tasks_executed the same in hang1
  and hang2. Or 8 passes: M did not hang (inconclusive for M).

STEP 7, normal: unload M rc=0; nothing in lsmod; hung_task_timeout_secs put back; whole task
  0 matches, 0 warnings, 0 hung-task lines, 95 ZTS marks; the boot's count equal to the STEP 0
  baseline; nothing left; rm rc=0.
STEP 7, wedged: unload skipped; zfs and spl loaded; hung_task_timeout_secs put back; the hang's
  kernel lines; D-state tasks listed; everything kept; the VM needs a reboot by the operator.
STOP only on: a failed check or guard, a leftover, a tunable left changed, an outer timeout,
  a kernel VERIFY/ASSERT/panic/BUG/oops/hung task, or a hang.
```

## What each result means

- **The evidence holds** when every item above matches, with a hang on M.
  Then, in a kernel and with CI's thread layout: master deadlocks when a
  resilver's repair write, started from the single `z_rd_int` thread, waits
  for a reflow row in `vdev_raidz_io_start()` while that row's reads wait
  for the same thread (the stack, and `z_rd_int` stalled with tasks queued);
  and the commit, the only difference between the trees, removes the cycle
  without a regression the tests see: B passes the amplified test 8 times,
  the test 10 times and the `raidz` group, with no hung task, and with
  bpftrace no `*_int` thread ever takes the expansion's range lock on B.
- **M never hangs** (`ma1`-`ma8` and `mu01`-`mu10` all pass): the kernel A/B
  is inconclusive for M, said as such; B's results, and the bpftrace tables,
  still hold.
- **A finding against the commit**:
  - a hang, or any hung-task line, on B: stop; the captures and the stack are
    the result;
  - an amplified run, or a run of the test, failing on B; its digest locates
    it (`line:` entries, the end line). A failure of B in the group is
    reported with its lines; master's group is not run here, so whether
    master fails it too is left open, unless the same test failed on M's 011
    runs;
  - with bpftrace, a `*_int` thread among B's `@locks`: the requeue did not
    happen for that I/O.
- **Against the analysis, not the commit**: a hang on M whose `z_rd_int`
  stack lacks the rangelock wait under `vdev_raidz_io_start()`, or a hang with
  another task blocked first and `z_rd_int` executing: master deadlocks some
  other way. Report it with the whole capture; it is still master's.
- **Not the commit's**: a test result that is the same on both trees but not
  PASS (the 011 runs), or a failure in a `setup` or `cleanup`: the
  environment.
- **`WARNING:` lines in the kernel log**: quoted with their run. A finding
  only if B shows one that M does not.

Estimated duration: about 3 hours (2.5 to 3.5): checks and sources 10
minutes, the two builds about 30, on B the amplified test 8 times 25 to 50
(3 to 6 minutes each, an estimate: the delay makes every reflow row last at
least 1 s), the test 10 times 10 to 15 and the `raidz` group 15 to 30, the
swap 2, on M the test 10 to 15 and the amplified test up to 25 to 50 (less
after a hang, plus about 3 minutes for the captures), the report and the
cleanup 10.
