# VM 107 task: cross-pool interference rerun, with a fast pool that stays clear of the quota (8/11)

These instructions are for VM 107 in its new shape: 32 vCPUs in two NUMA
nodes of 16 (vCPU 0-15 on host node 0, 16-31 on host node 1), 32 GiB,
Debian 13, kernel 6.12. STEP 0 stops if the VM has another shape. Reply
with `/var/log/xpool-1108/REPORT.txt` (STEP 7).

**Background.** VM-XPOOL-1106 (same fork branch `vm-tasks-1106`) measured on
this VM, then 16 vCPUs, how much slow RAID-Z2 pools writing flat out slow a
fast mirror pool, and how much of it the prototype tunables of the
pool-isolation commits remove; on request it also captured, in the phases
where the pools run together, where the fast pool's threads wait. Its main
finding was a caveat: `dmu_tx_quota` counted about 12 000 to 16 000 in every
phase, the alone ones included, and the fast pool's sync writers spent about
half of their off-CPU time in `dmu_tx_wait` -> `txg_wait_synced`. The fast
pool (a mirror of two 1 GiB null_blk devices holding a 512 MiB file) was
small enough for the quota check to force a txg every few hundred writes, so
the sync-write rows mixed that with the cross-pool effect. This rerun keeps
the bench, its three runs, their phases, variants and workloads, and the wait
captures, on the new shape, with a fast pool that stays clear of the quota,
and checks it: at the end of every phase without slow load the bench now
compares `dmu_tx_quota` with the phase's start and stops with a message if it
moved.

**The fast pool, and why its file shrinks.** The quota check at a pool's
root refuses a write once the dirty data of all unsynced txgs, counted 24
times (`spa_asize_inflation`), reaches the free space plus 1/32 of the pool.
`sw1` and `sw4` are 4K random `O_SYNC` writes on one file in a 4K-record
dataset: a txg (every 5 s here, since neither the dirty-data trigger nor the
log trigger is reached) dirties up to every block of the file plus its
indirect blocks (128 KiB per 4 MiB of file), and two txgs are unsynced at a
time. With the 512 MiB file, `sw4` (about 10 800 writes/s on 6 November)
dirties about 175 MiB of blocks and 16 MiB of indirect blocks per txg, 0.38
GiB for two, which needs 9 GiB of free space; the largest fast pool allowed
here (a mirror of two 4 GiB null_blk devices, the 8 GiB of memory-backed
devices this rerun may use for it) has 3.75 GiB of quota, so a cap of (3.75
+ 0.12 - 0.05) / 24 = 0.16 GiB. Only a smaller file fits: with 48 MiB
(`SIZE_MB=48`), two txgs dirty at most 2 x (48 + 1.5) = 99 MiB, under the
163 MiB cap with a margin of 1.6. So the fast pool becomes `FAST_DEV_MB=4096`
with `SIZE_MB=48` in runs 1 and 2 (6 November: 1024 and 512). The reads
(`rr1`, `rr4`) stay uncached 4K random reads (primarycache=metadata) of the
same kind, on a smaller file. The slow pools are unchanged. The fast pool
then peaks around 1.2 GiB of 3.875 (31 %): the file, the copies freed in the
last three txgs (0.15 GiB), and the log blocks of four txgs (about 1 GiB at
`sw4`).

Run 3 (no slow pool) measures the peak compressed async write throughput of
one pool (`tz`, `tl`: 1 GiB files, 1M writes): its txgs carry hundreds of
MiB of dirty data, far over any cap the allowed memory can give, so its
quota count stays non-zero whatever the pool; it gets the 4096 MiB devices
too, records its quota per phase, and is not stopped on it.

**Runs**, as VM-XPOOL-1106 (bench `tests/perf-xpool/run.sh qv` of fork branch
`perf-xpool-cause` at `6549ab63bd83e6b58cda0966e3e78281c8aa27c8`, release
build, `REPS=3`, 20 s runs, `ARC_MAX` 2 GiB, `Q2_DIRTY_MAX` 1.5 GiB):

| run | slow load | phases |
|---|---|---|
| 1 (vm1) | one RAID-Z2 of 6 x 768 MiB null_blk under dm-delay, writing a 1 GiB file | alone, sw, sw-nocomp, sw-pct25, sw-w2, sw-p6, sw-w2p6, sw-all3, sw-up2, alone-sep, sw-sep, alone-all3, alone2 |
| 2 (vm3) | three RAID-Z2 of 6 x 256 MiB, writing 256 MiB files | alone, m3w, m3w-pct25, m3w-w2, m3w-w2l, m3w-w2lc, m3w-p6, m3w-all3, m3w-up2, alone2 |
| 3 (vm0) | none: `tz`, `tl` throughput | alone, alone-p6, alone-w2, alone-all3, alone2 |

In the `-sep` phases the slow pools' threads and load run on node 0 (CPUs
0-15) and the fast pool's on node 1 (16-31).

**Wait captures**, in the phases where the slow and the fast pools run
together, baseline (`sw`, `m3w`: all tunables off) and everything on
(`sw-all3`, `m3w-all3`), for `sw4` and `rr4`: right after the phase's end
snapshot, with the slow load still on, the fast pool is exported and
imported (to find its `txg_sync` thread) and a 20 s run captures every
second the tasks in D state counted by (wchan, comm), `/proc/pressure/io` and
`cpu` at its start and end, per-CPU `/proc/stat` over 10 s, and 10 s of
`perf record -a -g -e sched:sched_switch -e sched:sched_wakeup`, reduced to
the off-CPU time of the fast pool's fio (`fiofast`) and `txg_sync` by
category of the stack they slept in (`txg-wait` = `dmu_tx_wait` /
`txg_wait_*`, `zil-commit`, `io-wait`, `lock`, `dirty-throttle`, `preempted`
= run queue), by node, with the run-queue part of each wait and the top 15
stacks. The trace file is deleted once reduced. These runs come after the
phase's end snapshot, so the phase deltas do not include them. This is the
capture done on 6 November, written into the bench by the patch below.

**The patch** (`xpool-1108.patch`, applied to the bench, nothing else in the
tree): `qv_phase` writes each phase's `dmu_tx_quota` delta to `quota.txt`
and logs it; with `QV_QUOTA_ZERO=1` it stops the run when that delta is not 0
in a phase without slow load; `QV_WAITS=1` adds the wait captures after each
`diag` phase, reduced by `WAITS_PY`.

**Duration**: STEP 1 about 10 min; run 1 about 85 min, run 2 about 60 min,
run 3 about 20 min (the wait captures add about 2 min each, 8 in all);
reports and cleanup 10 min. In all about 3 h.

## Rules

- Disposable VM, release build, no other build, test or benchmark running.
  Memory-backed devices only (null_blk), nothing on the VM's disks.
- Post nothing, push nothing, install no package and no ZFS (`make install`
  is not needed: the bench runs with the build tree first in `PATH`). The
  bench itself copies `fio` to `/usr/local/bin/fiofast` and sets
  `kernel.kptr_restrict=0` and `kernel.perf_event_paranoid=-1`, as on 6
  November; STEP 6 removes `fiofast`.
- Work only under `/var/tmp/xpool-1108` (W) and `/var/log/xpool-1108` (L),
  on the null_blk and dm-delay devices of the bench and its pools `fastq`,
  `slowq`, `slow1` to `slow3`. Never run `zpool`, `zfs` or `zdb` while the
  modules of this build are not loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A `FATAL` line of the bench (among them the
  quota stop), a panic, oops, `VERIFY`, `ASSERT`, `BUG:` or hung-task
  message: stop and report. After a stop past STEP 1, go to STEP 6 with what
  you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must have the new shape and be free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
numactl -H | sed -n '1,3p'
grep -E 'MemTotal|MemAvailable' /proc/meminfo
ls -d /lib/modules/$(uname -r)/build
for t in fio perf bpftrace dmsetup python3 git make gcc taskset chrt pgrep numactl; do command -v $t > /dev/null || echo "missing $t"; done
modinfo -n null_blk dm_delay                                   # two paths
grep -E '^(zfs|spl|null_blk) ' /proc/modules                    # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
sudo -n dmsetup ls 2>&1 | grep -E '^slow'                      # nothing
ls /lib/modules/$(uname -r)/extra 2>&1 | grep -E 'zfs|spl'     # nothing (no installed ZFS)
command -v zpool zfs zdb zed                                   # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/xpool-1108 /var/log/xpool-1108 2>&1             # No such file or directory, twice
df -h /var/tmp / | tail -2                                     # 10G free or more on both
```

Stop, changing nothing, if: `sudo -n true` fails; `nproc` is not 32;
`numactl -H` does not show two nodes with CPUs 0-15 and 16-31; MemAvailable
is under 24 GiB; the kernel headers, a tool, `null_blk` or `dm_delay` is
missing; a zfs, spl or null_blk module is loaded, a pool is imported or a
`slow*` dm device exists; a ZFS is installed (modules in `extra`, or
`zpool`/`zfs`/`zdb`/`zed` in `PATH`); a build, test or benchmark runs; W or
L exists; less than 10G is free on `/var/tmp` or `/`.

## STEP 1: source, patch, release build (about 10 min)

```sh
W=/var/tmp/xpool-1108 L=/var/log/xpool-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W || exit 1
git init -q $W/repo && git -C $W/repo fetch -q --depth 5 https://github.com/tigerblue77/zfs perf-xpool-cause || exit 1
git -C $W/repo rev-parse FETCH_HEAD                     # 6549ab63bd83e6b58cda0966e3e78281c8aa27c8
git -C $W/repo worktree add -q --detach $W/src 6549ab63bd83e6b58cda0966e3e78281c8aa27c8 && git -C $W/src log --oneline -1
cat > $L/xpool-1108.patch << 'EOF_PATCH'
diff --git a/tests/perf-xpool/run.sh b/tests/perf-xpool/run.sh
index 3d68cbd584..487b946131 100755
--- a/tests/perf-xpool/run.sh
+++ b/tests/perf-xpool/run.sh
@@ -656,6 +656,63 @@ QV_SLOW_CPUS=${QV_SLOW_CPUS:-0,1}
 QV_FAST_CPUS=${QV_FAST_CPUS:-2,3}
 QV_WARM_WRITE=${QV_WARM_WRITE:-20}
 QV_WARM_READ=${QV_WARM_READ:-15}
+# QV_WAITS=1: after each diag phase, wait captures of sw4 and rr4 (needs
+# WAITS_PY). QV_QUOTA_ZERO=1: stop when dmu_tx_quota rises in an alone phase.
+QV_WAITS=${QV_WAITS:-0}
+QV_QUOTA_ZERO=${QV_QUOTA_ZERO:-0}
+WAITS_PY=${WAITS_PY:-}
+
+# Wait capture: a DIAG_RUNTIME fast-pool run of WL with the slow load still
+# on; meanwhile, every second for 18 s, the tasks in D state by (wchan,
+# comm); PSI at the start and the end; per-CPU /proc/stat over 10 s; and
+# 10 s of sched_switch and sched_wakeup with call graphs, reduced by WAITS_PY
+# to the off-CPU time of the fast pool's fio and of its txg_sync thread
+# (found as the one an export and import of fastq replaces).
+qv_diag_wait() {
+	local ph=$1 wl=$2 label d fpid spid before i
+	label=qvwait.$ph.$wl
+	d=$OUT/diag/$label
+	mkdir -p "$d"
+	pool_op fastq export fastq
+	before=$(pgrep -x txg_sync | sort)
+	pool_op fastq import -d /dev fastq
+	comm -13 <(echo "$before") <(pgrep -x txg_sync | sort) > "$d/fast-txg-sync.tids"
+	q2_args "$wl" "$DIAG_RUNTIME"
+	run_fio "$label" "${FIO_ARGS[@]}" &
+	fpid=$!
+	sleep 1
+	cat /proc/pressure/io > "$d/psi.io.start"
+	cat /proc/pressure/cpu > "$d/psi.cpu.start"
+	(
+		for i in $(seq 1 18); do
+			ps -eLo stat=,wchan:32=,comm= | awk '$1 ~ /^D/ { print $2, $3 }'
+			echo "--"
+			sleep 1
+		done
+	) > "$d/dstate.txt" &
+	spid=$!
+	sleep 1
+	grep '^cpu[0-9]' /proc/stat > "$d/stat.0"
+	"$PERF" record -a -g -m 8M -e sched:sched_switch -e sched:sched_wakeup \
+	    -o "$WORK/wait.data" -- sleep 10 > "$WORK/perf-wait.log" 2>&1
+	grep '^cpu[0-9]' /proc/stat > "$d/stat.1"
+	wait "$spid"
+	wait "$fpid"
+	cat /proc/pressure/io > "$d/psi.io.end"
+	cat /proc/pressure/cpu > "$d/psi.cpu.end"
+	"$PERF" script -i "$WORK/wait.data" \
+	    -F comm,tid,cpu,time,event,trace,ip,sym 2> "$d/script.err" |
+	    python3 -I "$WAITS_PY" offcpu "$d/fast-txg-sync.tids" \
+	    "$d/offcpu.json" > "$d/offcpu.txt"
+	rm -f "$WORK/wait.data"
+}
+
+# The dmu_tx_quota delta of a phase, from its start and end snapshots.
+qv_quota_delta() {
+	awk '$1 == "dmu_tx_quota" { v[FILENAME] = $3 }
+	    END { print v[ARGV[2]] - v[ARGV[1]] }' \
+	    "$OUT/phase/$1.start/dmu_tx" "$OUT/phase/$1.end/dmu_tx"
+}
 SLOW_POOLS=()
 SLOW_PIDS=()
 SLOW_IMPORT_DIR=/dev/mapper
@@ -1076,6 +1133,16 @@ qv_phase() {
 		qv_diag_sched "$name" sw1
 	fi
 	phase_snap "$name" end
+	q=$(qv_quota_delta "$name")
+	echo "$name $q" >> "$OUT/quota.txt"
+	log "phase $name: dmu_tx_quota delta $q"
+	if [ "$QV_QUOTA_ZERO" = 1 ] && [ "$load" = none ] && [ "$q" != 0 ]; then
+		die "dmu_tx_quota rose by $q in the alone phase $name: the fast pool still hits the quota, the sync-write rows are not clean; stopping"
+	fi
+	if [ "$QV_WAITS" = 1 ] && [ -n "$diag" ]; then
+		qv_diag_wait "$name" sw4
+		qv_diag_wait "$name" rr4
+	fi
 	if [ -n "$IDLE_PID" ]; then
 		kill "$IDLE_PID" 2>/dev/null
 		wait "$IDLE_PID" 2>/dev/null
EOF_PATCH
sha256sum $L/xpool-1108.patch                           # c3d00b09573cbdfae6a6c3e0a252ad413525e303274ae55624b62a56989d5d9d
git -C $W/src apply $L/xpool-1108.patch && git -C $W/src diff --stat | tail -1   # 1 file changed, 67 insertions(+)
sha256sum $W/src/tests/perf-xpool/run.sh                # 31b8bcbd0a67e0a65abd5fd96fb91c3ca149a615d077918036e6812aedde38f3
bash -n $W/src/tests/perf-xpool/run.sh && echo "run.sh parses"
(cd $W/src && t0=$(date +%s) &&
 nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j$(nproc)' > $L/build.log 2>&1
 echo "build rc=$? wall=$(( $(date +%s) - t0 ))s")
echo "release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' $W/src/Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' $W/src/Makefile)" \
    "vermagic=$(modinfo -F vermagic $W/src/module/zfs.ko | cut -d' ' -f1) srcversion=$(modinfo -F srcversion $W/src/module/zfs.ko)" | tee $L/build.txt
```

Expected: the commit `6549ab63bd perf-xpool: sync-thread priority variants,
floored write issue share`; both sha256 values as in the comments; `1 file
changed, 72 insertions(+)`; `run.sh parses`; `build rc=0`; `release=1
noinline=0`; `vermagic` = `uname -r`. On a failed build quote `grep -nE
'error:|Error [0-9]' $L/build.log | head -20` and stop.

## STEP 2: the helper scripts, and the modules

```sh
L=/var/log/xpool-1108
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the cross-pool rerun, sourced by the scripts.
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/xpool-1108
L=/var/log/xpool-1108
X=$W/src
EOF_ENV
cat > $L/waits.py << 'EOF_WAITS'
import json
import math
import os
import re
import sys

# waits.py: the wait captures of the cross-pool rerun.
#   offcpu TIDS OUT.json    from "perf script" output on stdin: off-CPU time
#                           of the fast pool's fio (fiofast) and txg_sync
#                           (the thread ids in TIDS), by category of the
#                           stack it slept in and by node, the run-queue part
#                           of each wait, and the top stacks
#   summary DIR...          side-by-side tables of the captures under each
#                           DIR (an OUT directory of run.sh qv)

SCHED = ("__schedule", "schedule", "schedule_timeout", "io_schedule",
    "schedule_hrtimeout_range_clock", "schedule_hrtimeout_range",
    "schedule_preempt_disabled", "io_schedule_timeout", "preempt_schedule",
    "preempt_schedule_common", "__cond_resched", "_cond_resched",
    "preempt_schedule_irq", "schedule_idle", "do_nanosleep")
LOCKS = ("__mutex_lock", "mutex_lock", "rwsem_down", "down_read", "down_write",
    "__rwsem", "rt_mutex", "rrw_enter", "__down", "osq_lock")
CATS = ("txg-wait", "dirty-throttle", "zil-commit", "io-wait", "lock",
    "preempted", "cv-wait", "txg-idle", "taskq-idle", "other")
HDR = re.compile(r"^\s*(.+?)\s+(\d+)\s+\[(\d+)\]\s+(\d+\.\d+):\s+(\S+):\s*(.*)$")
SW = re.compile(r"prev_comm=(.*?) prev_pid=(\d+) prev_prio=(-?\d+) "
    r"prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+) next_prio=(-?\d+)")
WK = re.compile(r"comm=(.*?) pid=(\d+) prio=(-?\d+)")


def nodes():
    node = {}
    base = "/sys/devices/system/node"
    for n in os.listdir(base):
        if not (n.startswith("node") and n[4:].isdigit()):
            continue
        for r in open("%s/%s/cpulist" % (base, n)).read().strip().split(","):
            if r:
                a, _, b = r.partition("-")
                for c in range(int(a), int(b or a) + 1):
                    node[c] = int(n[4:])
    return node


def plain(frames):
    return [f for f in frames if f not in SCHED and
        not f.startswith(("__traceiter", "__probestub", "perf_", "trace_"))]


def category(state, frames):
    if state.startswith("R"):
        return "preempted"
    inner = plain(frames)
    if inner and inner[0].startswith(LOCKS):
        return "lock"
    joined = " ".join(frames)
    for marker, cat in (("txg_wait_synced", "txg-wait"),
        ("txg_wait_open", "txg-wait"), ("dmu_tx_delay", "dirty-throttle"),
        ("dmu_tx_wait", "txg-wait"), ("zil_commit", "zil-commit"),
        ("zio_wait", "io-wait"), ("io_schedule", "io-wait"),
        ("txg_thread_wait", "txg-idle"), ("taskq_thread", "taskq-idle"),
        ("cv_wait", "cv-wait"), ("cv_timedwait", "cv-wait")):
        if marker in joined:
            return cat
    return "other"


def offcpu(tidfile, out, stream, nstacks=15):
    node = nodes()
    nn = max(node.values()) + 1
    try:
        txg = set(open(tidfile).read().split())
    except OSError:
        txg = set()
    pend, woke = {}, {}
    G = {}

    def grp(tid, comm):
        if comm == "fiofast":
            return "fiofast"
        if comm.startswith("txg_sync") and tid in txg:
            return "txgfast"
        return None

    def finish(ev):
        if ev is None:
            return
        t, cpu, kind, a = ev
        if kind == "switch":
            pcomm, ppid, pstate, ncomm, npid, frames = a
            g = grp(ppid, pcomm)
            if g is not None:
                pend[ppid] = (t, pstate, frames, cpu, g)
                woke.pop(ppid, None)
            if npid in pend:
                t0, st, fr, c0, g0 = pend.pop(npid)
                w = woke.pop(npid, None)
                rq = t - (w if w is not None and w >= t0 else t0) \
                    if st.startswith("R") or w is not None else 0.0
                r = G.setdefault(g0, {"off": {}, "stacks": {}, "tids": set()})
                r["tids"].add(npid)
                cat = category(st, fr)
                o = r["off"].setdefault((cat, node.get(c0, 0)), [0.0, 0, 0.0])
                o[0] += t - t0
                o[1] += 1
                o[2] += rq
                key = "%s %s" % (cat, ";".join(reversed(plain(fr)[:10])) or "-")
                s = r["stacks"].setdefault(key, [0.0, 0])
                s[0] += t - t0
                s[1] += 1
        elif kind == "wakeup":
            if a in pend and a not in woke:
                woke[a] = t

    cur = None
    first = last = None
    for ln in stream:
        if ": sched:" not in ln:
            if cur is not None and cur[2] == "switch":
                f = ln.split()
                if len(f) >= 2:
                    cur[3][5].append(f[1].split("+0x", 1)[0])
            continue
        m = HDR.match(ln)
        if not m:
            continue
        finish(cur)
        cur = None
        t, cpu, ev, rest = float(m.group(4)), int(m.group(3)), m.group(5), m.group(6)
        first = t if first is None else first
        last = t
        if ev.endswith("sched_switch"):
            s = SW.search(rest)
            if s:
                cur = (t, cpu, "switch", (s.group(1), s.group(2), s.group(4),
                    s.group(5), s.group(6), []))
        elif ev.endswith(("sched_wakeup", "sched_wakeup_new")):
            s = WK.search(rest)
            if s:
                cur = (t, cpu, "wakeup", s.group(2))
    finish(cur)
    res = {"span_s": (last - first) if first is not None else 0.0,
        "txg_tids": sorted(txg), "groups": {}}
    for g, r in G.items():
        cats = {}
        for (cat, n), (sec, c, q) in r["off"].items():
            e = cats.setdefault(cat, {"ms": 0.0, "n": 0, "rq_ms": 0.0,
                "ms_node": [0.0] * nn})
            e["ms"] += sec * 1e3
            e["n"] += c
            e["rq_ms"] += q * 1e3
            e["ms_node"][n] += sec * 1e3
        top = sorted(r["stacks"].items(), key=lambda kv: -kv[1][0])[:nstacks]
        res["groups"][g] = {"threads": len(r["tids"]), "cats": cats,
            "stacks": [[k, v[0] * 1e3, v[1]] for k, v in top]}
    json.dump(res, open(out, "w"), indent=1)
    lines = ["span %.1f s, fast txg_sync tids %s" % (res["span_s"],
        " ".join(res["txg_tids"]) or "none found")]
    for g in ("fiofast", "txgfast"):
        r = res["groups"].get(g)
        if not r:
            lines.append("%s: no off-CPU interval" % g)
            continue
        lines.append("%s (%d threads)" % (g, r["threads"]))
        for c, e in sorted(r["cats"].items(), key=lambda kv: -kv[1]["ms"]):
            lines.append("  %-14s %9.1f ms %7d x  run-queue part %8.1f ms  by node %s"
                % (c, e["ms"], e["n"], e["rq_ms"],
                "/".join("%.0f" % x for x in e["ms_node"])))
        for k, ms, n in r["stacks"]:
            lines.append("  %9.1f ms %7d x  %s" % (ms, n, k[:160]))
    return "\n".join(lines)


def psi(path):
    # "some avg10=..." and "full avg10=..." of a /proc/pressure file.
    d = {}
    try:
        for ln in open(path):
            f = ln.split()
            d[f[0]] = float(f[1].split("=")[1])
    except (OSError, IndexError, ValueError):
        pass
    return d


def node_cpu(d):
    node = nodes()
    nn = max(node.values()) + 1
    s = [{}, {}]
    for i in (0, 1):
        try:
            for ln in open(os.path.join(d, "stat.%d" % i)):
                f = ln.split()
                s[i][int(f[0][3:])] = [int(x) for x in f[1:9]]
        except (OSError, ValueError):
            return None
    out = []
    for n in range(nn):
        busy = io = tot = 0
        for c, v1 in s[1].items():
            if node.get(c, 0) != n or c not in s[0]:
                continue
            dd = [b - a for a, b in zip(s[0][c], v1)]
            busy += dd[0] + dd[1] + dd[2] + dd[5] + dd[6] + dd[7]
            io += dd[4]
            tot += sum(dd)
        out.append((100.0 * busy / tot if tot else math.nan,
            100.0 * io / tot if tot else math.nan))
    return out


def dstate(d):
    counts, samples = {}, 0
    try:
        for ln in open(os.path.join(d, "dstate.txt")):
            ln = ln.strip()
            if ln == "--":
                samples += 1
                continue
            if ln:
                counts[ln] = counts.get(ln, 0) + 1
    except OSError:
        pass
    return counts, samples


def summary(dirs):
    out = []
    for top in dirs:
        diag = os.path.join(top, "diag")
        caps = sorted(x for x in os.listdir(diag) if x.startswith("qvwait.")) \
            if os.path.isdir(diag) else []
        # Phases in the order they ran (the baseline first).
        try:
            order = [ln.split()[0] for ln in open(os.path.join(top, "phases.txt"))
                if ln.strip()]
        except OSError:
            order = []
        rank = lambda c: order.index(c.split(".", 1)[1].rsplit(".", 1)[0]) \
            if c.split(".", 1)[1].rsplit(".", 1)[0] in order else len(order)
        caps.sort(key=rank)
        if not caps:
            continue
        out.append("=== %s" % top)
        for wl in sorted({c.rsplit(".", 1)[1] for c in caps}):
            cs = [c for c in caps if c.endswith("." + wl)]
            names = [c.split(".", 1)[1].rsplit(".", 1)[0] for c in cs]
            out.append("-- %s: %s" % (wl, " | ".join(names)))
            res = []
            for c in cs:
                try:
                    res.append(json.load(open(os.path.join(diag, c, "offcpu.json"))))
                except (OSError, ValueError):
                    res.append({"groups": {}})
            for g in ("fiofast", "txgfast"):
                out.append("  %s off-CPU ms (run-queue part) by node:" % g)
                for cat in CATS:
                    row = []
                    for r in res:
                        e = r["groups"].get(g, {}).get("cats", {}).get(cat)
                        row.append("%7.0f (%5.0f) %s" % (e["ms"], e["rq_ms"],
                            "/".join("%.0f" % x for x in e["ms_node"]))
                            if e else "%7s %7s %s" % ("-", "", ""))
                    if any(not x.strip().startswith("-") for x in row):
                        out.append("    %-14s %s" % (cat, " | ".join(row)))
            out.append("  CPU per node, busy % / iowait %:")
            for c, n in zip(cs, names):
                v = node_cpu(os.path.join(diag, c))
                out.append("    %-12s %s" % (n, "  ".join("node%d %.0f / %.1f" %
                    (i, b, w) for i, (b, w) in enumerate(v or []))))
            out.append("  PSI avg10 at start -> end (io some, io full, cpu some):")
            for c, n in zip(cs, names):
                p = [psi(os.path.join(diag, c, "psi.%s" % k)) for k in
                    ("io.start", "io.end", "cpu.start", "cpu.end")]
                out.append("    %-12s io %s -> %s, full %s -> %s, cpu %s -> %s" % (n,
                    p[0].get("some", "-"), p[1].get("some", "-"),
                    p[0].get("full", "-"), p[1].get("full", "-"),
                    p[2].get("some", "-"), p[3].get("some", "-")))
            out.append("  D state, tasks per sample by (wchan, comm), top 10:")
            for c, n in zip(cs, names):
                counts, samples = dstate(os.path.join(diag, c))
                topd = sorted(counts.items(), key=lambda kv: -kv[1])[:10]
                out.append("    %-12s %s" % (n, ", ".join("%s %.2f" % (k,
                    v / max(samples, 1)) for k, v in topd) or "none"))
    return "\n".join(out)


if __name__ == "__main__":
    if sys.argv[1] == "offcpu":
        print(offcpu(sys.argv[2], sys.argv[3], sys.stdin))
    elif sys.argv[1] == "summary":
        print(summary(sys.argv[2:]))
    else:
        sys.exit("unknown command " + sys.argv[1])
EOF_WAITS
cat > $L/xclean.sh << 'EOF_CLEAN'
#!/bin/bash
# xclean.sh: destroy the pools and devices of a qv run (the zfs module
# stays loaded), and say what is left.
set -u
. /var/log/xpool-1108/env.sh
if grep -q '^zfs ' /proc/modules; then
	for p in fastq slowq slow1 slow2 slow3; do
		"$X/zpool" list -H -o name "$p" > /dev/null 2>&1 &&
		    { "$X/zpool" destroy -f "$p" || echo "destroy $p failed"; }
	done
fi
dmsetup ls 2> /dev/null | awk '/^slow/ { print $1 }' | xargs -r -n1 dmsetup remove
rmdir /sys/kernel/config/nullb/nullb* 2> /dev/null
grep -q '^null_blk ' /proc/modules && { modprobe -r null_blk || echo "modprobe -r null_blk failed"; }
echo "left: pools [$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d -printf '%f ' 2>/dev/null)]" \
    "dm [$(dmsetup ls 2> /dev/null | awk '/^slow/ { printf "%s ", $1 }')]" \
    "null_blk [$(grep -c '^null_blk ' /proc/modules)]"
EOF_CLEAN
chmod 0755 $L/xclean.sh
cd $L && sha256sum env.sh waits.py xclean.sh
```

Must print:
```
4be595ad8da11d63c1b350e89bd1061f1cf36f5eac502a1afbb388e560a15ca2  env.sh
5cc4db5241a887cc5a72eb1de987827484dcde33e6ef9986e576c5788da30e6a  waits.py
08d80fefbbbca8d3454526ce864bcd68e5bcb154b26d22e764cf03572bf00d04  xclean.sh
```

```sh
X=/var/tmp/xpool-1108/src
(cd $X && sudo -n ./scripts/zfs.sh) && echo "load rc=0"
echo "loaded $(cat /sys/module/zfs/srcversion), built $(modinfo -F srcversion $X/module/zfs.ko)"   # equal
for p in spa_asize_inflation zfs_dirty_data_max zfs_wrlog_data_max zio_taskq_bulk_lowpri zio_taskq_write_pct spa_sync_light_pri; do
    echo "$p=$(cat /sys/module/zfs/parameters/$p)"
done | tee -a /var/log/xpool-1108/build.txt
```

Expected: `load rc=0`; equal srcversions; `spa_asize_inflation=24`; the four
prototype parameters present (their defaults: `zio_taskq_bulk_lowpri=0`,
`zio_taskq_write_pct=80`, `spa_sync_light_pri=0`).

## STEP 3: run 1, one slow pool (about 85 min)

One command, the VM otherwise idle:

```sh
L=/var/log/xpool-1108 W=/var/tmp/xpool-1108
sudo -n env PATH="$W/src:$PATH" OUT=$W/out/vm1 WORK=$W/work/vm1 \
  QV_POOLS=1 QV_SLOW_WRITE_MB=1024 SLOW_DEV_MB=768 FAST_DEV_MB=4096 SIZE_MB=48 \
  ARC_MAX=2147483648 Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
  QV_SLOW_CPUS=0-15 QV_FAST_CPUS=16-31 QV_WAITS=1 QV_QUOTA_ZERO=1 WAITS_PY=$L/waits.py \
  QV_PHASES="alone:none sw:write:diag sw-nocomp:write:comp=off \
    sw-pct25:write:pct=25 sw-w2:write:kn=w2 sw-p6:write:kn=p6 \
    sw-w2p6:write:kn=w2p6 sw-all3:write:kn=all3,diag sw-up2:write \
    alone-sep:none:sep sw-sep:write:sep alone-all3:none:kn=all3 \
    alone2:none" \
  bash $W/src/tests/perf-xpool/run.sh qv > $L/vm1.log 2>&1; tail -1 $L/vm1.log
grep -E 'dmu_tx_quota delta|FATAL|WARNING' $L/vm1.log
sudo -n bash $L/xclean.sh
```

Expected: the last line `[...] done`; a `dmu_tx_quota delta 0` line for
`alone`, `alone-sep`, `alone-all3` and `alone2` (the write phases count the
slow pool's too); no `FATAL`; `xclean.sh` ends with `left: pools [] dm []
null_blk [0]`. If the bench stops with `dmu_tx_quota rose by N in the alone
phase ...`, report that line and stop: the fast pool still hits the quota.

## STEP 4: run 2, three slow pools (about 60 min)

```sh
L=/var/log/xpool-1108 W=/var/tmp/xpool-1108
sudo -n env PATH="$W/src:$PATH" OUT=$W/out/vm3 WORK=$W/work/vm3 \
  QV_POOLS=3n QV3N_DEV_MB=256 QV_SLOW_WRITE_MB=256 FAST_DEV_MB=4096 SIZE_MB=48 \
  ARC_MAX=2147483648 Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
  QV_WAITS=1 QV_QUOTA_ZERO=1 WAITS_PY=$L/waits.py \
  QV_PHASES="alone:none m3w:write:diag m3w-pct25:write:pct=25 \
    m3w-w2:write:kn=w2 m3w-w2l:write:kn=w2l m3w-w2lc:write:kn=w2lc \
    m3w-p6:write:kn=p6 m3w-all3:write:kn=all3,diag m3w-up2:write \
    alone2:none" \
  bash $W/src/tests/perf-xpool/run.sh qv > $L/vm3.log 2>&1; tail -1 $L/vm3.log
grep -E 'dmu_tx_quota delta|FATAL|WARNING' $L/vm3.log
sudo -n bash $L/xclean.sh
```

Expected as for run 1, with `delta 0` for `alone` and `alone2`.

## STEP 5: run 3, no slow pool (about 20 min)

```sh
L=/var/log/xpool-1108 W=/var/tmp/xpool-1108
sudo -n env PATH="$W/src:$PATH" OUT=$W/out/vm0 WORK=$W/work/vm0 \
  QV_POOLS=0 QV_WL="tz tl" FAST_DEV_MB=4096 SIZE_MB=128 ARC_MAX=2147483648 \
  Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 QV_QUOTA_ZERO=0 \
  QV_PHASES="alone:none alone-p6:none:kn=p6 alone-w2:none:kn=w2 \
    alone-all3:none:kn=all3 alone2:none" \
  bash $W/src/tests/perf-xpool/run.sh qv > $L/vm0.log 2>&1; tail -1 $L/vm0.log
grep -E 'dmu_tx_quota delta|FATAL|WARNING' $L/vm0.log
sudo -n bash $L/xclean.sh
```

Expected: `done`, five `dmu_tx_quota delta` lines (not 0: see above), no
`FATAL`, nothing left.

## STEP 6: reports and cleanup

```sh
L=/var/log/xpool-1108 W=/var/tmp/xpool-1108
for r in vm1 vm3 vm0; do
    python3 -I $W/src/tests/perf-xpool/report.py qv $W/out/$r > $L/$r.txt 2>&1
    echo "$r report: $(wc -l < $L/$r.txt) lines"
    cp $W/out/$r/quota.txt $L/$r.quota.txt
done
python3 -I $L/waits.py summary $W/out/vm1 $W/out/vm3 > $L/waits.txt 2>&1; wc -l $L/waits.txt
for d in $W/out/vm1/diag/qvwait.* $W/out/vm3/diag/qvwait.*; do
    echo "== ${d#$W/out/}"; cat $d/offcpu.txt
done > $L/waits-stacks.txt 2>&1
sudo -n tar -C $W/out -czf $L/out.tgz . && ls -lh $L/out.tgz
sudo -n bash $L/xclean.sh
X=$W/src; (cd $X && sudo -n ./scripts/zfs.sh -u) && echo "unload rc=0"
grep -E '^(zfs|spl|null_blk) ' /proc/modules                    # nothing
sudo -n rm -f /usr/local/bin/fiofast
sudo -n dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'   # 0
sudo -n rm -rf $W && echo "work dir removed; logs kept in $L"
du -sh $L
sudo -n fstrim -av
```

`out.tgz` keeps every raw result of the three runs (the perf and trace data
files are already deleted).

## STEP 7: report

```sh
L=/var/log/xpool-1108
{
lscpu | grep -E '^(Model name|CPU\(s\)|NUMA node[0-9]+ CPU)'
echo "== build"; cat $L/build.txt
for r in vm1 vm3 vm0; do echo "== $r dmu_tx_quota per phase"; cat $L/$r.quota.txt; done
for r in vm1 vm3 vm0; do echo "== $r report"; cat $L/$r.txt; done
echo "== waits (side by side)"; cat $L/waits.txt
echo "== waits (top stacks per capture)"; cat $L/waits-stacks.txt
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed, and at
its end the output of STEP 6 and every deviation from this document (what
and why). Reply with the whole of `REPORT.txt`.

## Reading the result

- `quota.txt`: 0 in every alone phase of runs 1 and 2 means the sync-write
  rows of those phases measure the fast pool itself; in the write phases the
  count is the slow pools' (the kstat is global). In the wait captures,
  `fiofast` should show little or no `txg-wait` any more; what remains is the
  wait for the log blocks (`zil-commit`), for I/O, and the run queue.
- The report tables are those of 6 November (IOPS median [min..max], p99,
  "gap closed" by each variant, scheduler and "sched delay" lines, arcstats
  and dmu_tx deltas). Compare variants within a run; compare with 6 November
  only for the direction of an effect: the VM is twice as wide, and the fast
  pool and its file differ.
- Waits, side by side, baseline then all-on: where the fast pool's writers
  and its `txg_sync` sleep (lock, txg, dirty throttle, log, I/O, run queue),
  by node, the D-state tasks per sample, PSI and per-node busy and iowait.
  "CPU under 25 % busy with a high iowait" on the host means mostly
  `zil-commit` and `io-wait` here with little run-queue time; a large
  `preempted` or run-queue part means the slow pools' threads hold the CPU.

sha256 of this file without this last line and the blank line before it: 7b7b324a6bea76bb889ad733df04fe897333ea8b858c7fdea4db2c6779317f07
