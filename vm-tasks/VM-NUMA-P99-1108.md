# VM 107 task: the NUMA p99 rises of 6 November, repeated and explained (8/11)

These instructions are for VM 107 in its new shape: 32 vCPUs in two NUMA
nodes of 16 (vCPU 0-15 pinned to host node 0, vCPU 16-31 to host node 1,
distinct cores), 32 GiB of memory (16 GiB bound to each host node), Debian
13, kernel 6.12. STEP 0 stops if the VM has another shape. Reply with
`/var/log/numa-p99-1108/REPORT.txt` (STEP 7).

**Background.** On 6 November, on this VM in its former shape (16 vCPUs,
2 x 8), the A/B of node-local I/O completion (fork branch
`numa-local-completion`) gave B1 (phase 0+1: per-node completion taskqs whose
threads stay on their node) real gains on uncached reads, but also three p99
rises against A (stock master), each measured in a single pass:

| cell | 6/11 | workload |
|---|---|---|
| `ureadlz4` bound to node 0 | +14.8 % | 16K random reads, lz4 dataset, primarycache=metadata, 8 jobs under `numactl` on node 0 |
| `ureadlz4` bound to node 1 | +6.2 % | the same on node 1 |
| `swrite` at 16 jobs | +40.2 % | 16K random writes, sync=always, 16 jobs, unbound |

This task (a) repeats exactly those cells in 8 rounds per build, A and B1
alternated, to tell noise from a real effect, and (b) for every cell where
the rise holds, measures what explains it with what works in this guest (no
PEBS, so no `perf c2c` or `perf mem`): scheduler traces (`perf sched latency`,
off-CPU stacks from `perf record -e sched:sched_switch -g`, the run-queue wait
of each wakeup, the CPUs the completion threads run on), `/proc/schedstat`
and `/proc/<pid>/schedstat`.

**Builds.** Nothing is rebased: A is the same master commit as on 6
November and B1 the same branch head.

| tree | commit | what it is |
|---|---|---|
| `A` | `1974111f45abdd9e9b76d111b3b046fa172e405b` | upstream master, the A of 6 November |
| `B1` | `1a070dadd2c65c8976ff9829e2bf9bb3295c882c` | fork `numa-local-completion` head: phase 0 (read issue, read and write interrupt taskqs split per node, a zio dispatched to the group of the dispatching CPU's node) + phase 1 (each of their threads kept on its node; on a kernel before 6.14, as here, bound to one CPU of the node) |
| `B0` | `168d37e431589aec35d2e05eea404ad275952642` | that branch minus phase 1 (per-node groups, threads unbound); used only in the diagnosis |

B0 and B1 sit on master `ef35f144e1`, 4 commits after A (`dmu_recv`, ZIL
commit while suspended, direct reads of blocks pending free, decrypt on hold;
from reading, none is on a path these cells use).

The diagnosis compares four arms: `A`; `B1`; `B1n0` = B1's build with
`zio_taskq_numa=0` (read when the pool is imported: master's taskq layout and
no binding, on B1's code); `B0`. B1n0 against A isolates everything that is
not the per-node split; B0 against B1 isolates the binding to one CPU.

**Cells.** E1-E3 are the 6 November cells unchanged (job counts included),
S1-S3 the same scaled to this VM (16 jobs per node where 6 November had 8,
one job per vCPU for the unbound write):

| cell | dataset | fio | jobs | placement |
|---|---|---|---|---|
| E1 | nocachelz4 | randread 16k | 8 | node 0 |
| E2 | nocachelz4 | randread 16k | 8 | node 1 |
| E3 | sync | randwrite 16k | 16 | unbound |
| S1 | nocachelz4 | randread 16k | 16 | node 0 |
| S2 | nocachelz4 | randread 16k | 16 | node 1 |
| S3 | sync | randwrite 16k | 32 | unbound |

Each run is VM-NUMA-1105's `run.sh` unchanged in what fio does (per-job 16
MiB files, psync, 30 s after a 5 s ramp, `perf stat` of the same events
during seconds 10-25), plus: a once-a-second sampler of the ZFS threads and
fio (CPU time, last CPU, `/proc/<pid>/schedstat`), `/proc/stat` and
`/proc/schedstat` snapshots around the 15 s window, and the `dmu_tx_quota`,
ZIL stall and txg counts of the run.

**The pool, and why it cannot fill.** Block 1 of VM-NUMA-1105 (4 GiB RAM
disk pre-faulted on node 0, ashift 12, recordsize 16K, the same dataset
properties and fio layout), but with only the two datasets these cells use:
`nocachelz4` (16 files, about 0.14 GiB after lz4) and `sync` (32 files, 0.5
GiB), so 0.65 GiB live in 3.875 GiB (17 %) instead of 2.3 GiB (58 %) on 6
November. `spa_asize_inflation` stays at its default 24 (checked at every
module load). Reads write nothing. The sync writes add, at most, what is in
flight between txgs: the overwritten data blocks freed in the last three txgs
(a txg's freed space is reusable two txgs later), and the log blocks of the
txgs not yet synced plus those freed in the last two. With 24, the pool's
quota check holds the unique dirty data of all unsynced txgs under (free +
1/32 of the pool) / 24 = 134 MiB here, which forces a txg well before the
log's own trigger (15 % of `zfs_wrlog_data_max`, about 0.96 GiB on 32 GiB):
data in flight at most 3 x 134 MiB, log at most 4 txgs of the writes that
reach 134 MiB of unique blocks in a 256 or 512 MiB working set, about 155-190
MiB each. That bounds the peak at about 1.8 GiB (48 %); expected about 1.2
GiB (30 %), since each txg holds about half of that cap. Every write run
also has a watchdog that stops fio above 60 %, and every run checks 60 %
before it starts. (On 5 and 6 November the same 4 GiB disk held 2.3 GiB, so
the cap was half as high, 67 MiB, and the off-CPU profile of `swrite` at 16
jobs on 5 November found 54 s in `txg_wait_synced` next to 120 s in
`zil_commit`: the `swrite` cells ran more throttled than here. If the E3 rise
disappears, that difference may be why; the `quota`, `zilstall` and `txgs`
columns say which regime each run was in.)

**Expected taskq placement**, once the pool is imported (32 CPUs, 2 nodes):

- A and B1n0: `z_rd_int_0..4` and `z_wr_int_0..4` (5 taskqs, at most 5
  threads each), `z_rd_iss` (one taskq, at most 8); every thread may run on
  CPUs 0-31.
- B1: `z_rd_int_0..5` and `z_wr_int_0..5` (6 taskqs, at most 4 threads each;
  `_0` to `_2` serve node 0, `_3` to `_5` node 1), `z_rd_iss_0` and
  `z_rd_iss_1` (at most 8 each); each thread allowed one CPU of its node
  (0-15 for node 0, 16-31 for node 1).
- B0: as B1, but every thread may run on CPUs 0-31.

**Duration** (VM 107, 32 vCPUs): STEP 1 about 20 min; STEP 3 2 min; STEP 4
(phase 1, 16 segments of six 35 s runs) about 80 min; STEP 5 (phase 2) 0 to
2 h: 8 module loads plus, per held cell, 8 traced runs of about 2 min each
(about 20 min for one held cell, 50 min for three, 1 h 45 min for all six);
cleanup 5 min. In all: about 1 h 50 min if nothing holds, about 2 h 40 min
with three held cells, at most 3 h 35 min.

## Rules

- Disposable VM, release builds, no other build, test or benchmark running.
  Builds run `nice -n 10 ionice -c3`, one tree at a time.
- STEPS 3 to 5 are a performance A/B: run them only while the other two test
  VMs are idle (you are told when). Every run records the guest's steal time
  and load average (`steal_pct`, `load1` in `results.tsv`; STEP 7 prints them
  per segment).
- Post nothing, push nothing, install nothing (no package, no `make
  install`), no `zfs-tests.sh`, never reboot.
- Work only under `/var/tmp/numa-p99-1108` (W) and `/var/log/numa-p99-1108`
  (L), on `/dev/ram0` and the pool `numap99`. Never run `zpool`, `zfs` or
  `zdb` while none of this task's modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:`,
  `WARNING:` or hung-task message, or a stop by the 60 % guard: stop and
  report. After a stop past STEP 1, go to STEP 6 with what you have.
- Leave `kernel.numa_balancing` as you find it (STEP 0 records it; it was 1
  on 5 and 6 November).
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must have the new shape and be free

```sh
date -u '+%F %T'; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
numactl -H | sed -n '1,3p;/^node [0-9]* size/p'
cat /proc/sys/kernel/numa_balancing
grep -E 'MemTotal|MemAvailable' /proc/meminfo
ls -d /lib/modules/$(uname -r)/build
for t in fio numactl perf python3 git make gcc; do command -v $t > /dev/null || echo "missing $t"; done
grep -E '^(zfs|spl|brd) ' /proc/modules                         # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/numa-p99-1108 /var/log/numa-p99-1108 2>&1     # No such file or directory, twice
df -h /var/tmp | tail -1                                       # 15G free or more
awk '/^cpu[0-9]/ { n++; if (NF != 10) bad++ } END { print n " cpu lines, " bad + 0 " not of 9 fields" }' /proc/schedstat
cat /proc/self/schedstat                                       # three numbers
sudo -n perf record -a -g -e sched:sched_switch -o - -- sleep 0.2 > /dev/null 2>&1; echo "sched trace rc=$?"
ls /var/tmp/zts-numa-1106/ab-table.txt 2>/dev/null && grep -E 'ureadlz4|swrite' /var/tmp/zts-numa-1106/ab-table.txt
```

Stop, changing nothing, if: `sudo -n true` fails; `nproc` is not 32;
`numactl -H` does not show `available: 2 nodes (0-1)` with `node 0 cpus: 0
1 ... 15` and `node 1 cpus: 16 17 ... 31`; MemTotal is under 30 GiB or
MemAvailable under 24 GiB; the kernel headers or a tool are missing; a zfs,
spl or brd module is loaded, or a pool is imported; a build, test or
benchmark runs; W or L exists; less than 15G is free on `/var/tmp`;
`/proc/schedstat` has no cpu lines or lines that are not of 9 fields; the
sched trace probe does not print `rc=0`. The last line prints the 6
November table rows, if that file is still there, for reference.

## STEP 1: sources and three release builds (about 20 min)

```sh
W=/var/tmp/numa-p99-1108 L=/var/log/numa-p99-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W $L/raw || exit 1
git init -q $W/repo && git -C $W/repo fetch -q --depth 20 https://github.com/tigerblue77/zfs numa-local-completion || exit 1
git -C $W/repo rev-parse FETCH_HEAD                   # 1a070dadd2c65c8976ff9829e2bf9bb3295c882c
for t in A:1974111f45abdd9e9b76d111b3b046fa172e405b B1:1a070dadd2c65c8976ff9829e2bf9bb3295c882c B0:168d37e431589aec35d2e05eea404ad275952642; do
    n=${t%%:*} c=${t#*:}
    git -C $W/repo worktree add -q --detach $W/$n $c && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
for n in A B1 B0; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j$(nproc)' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    echo "$n release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' $W/$n/Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' $W/$n/Makefile)" \
        "affine=$(grep -c '^#define HAVE_KTHREAD_AFFINE_PREFERRED 1' $W/$n/zfs_config.h)" \
        "vermagic=$(modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1) srcversion=$(modinfo -F srcversion $W/$n/module/zfs.ko)"
done 2>&1 | tee $L/builds.txt
grep 'kthread_affine_preferred' $L/build-B1.log | tee -a $L/builds.txt
```

Expected: the three trees at the three commits; `build rc=0` three times;
`release=1 noinline=0 affine=0` three times; `vermagic` = `uname -r` three
times; three different srcversions; the configure line for
`kthread_affine_preferred` ending in `no` (kernel 6.12). On a failed build
quote `grep -nE 'error:|Error [0-9]' $L/build-<n>.log | head -20` and stop.

## STEP 2: the scripts, and the perf events this VM counts

`env.sh` holds the settings; `p99.py` samples, parses and builds the tables;
`load.sh` loads an arm's modules and checks them; `placement.sh` records the
taskqs and their threads' CPUs; `setup.sh` makes the RAM disk, the pool and
the files; `run.sh` runs one cell; `seg.sh` one segment (one module load, one
import, several cells); `phase1.sh` and `phase2.sh` the two phases.

```sh
L=/var/log/numa-p99-1108
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the NUMA p99 repeat, sourced by every script.
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/numa-p99-1108
L=/var/log/numa-p99-1108
P=numap99
MNT=$W/mnt
A=$W/A
B1=$W/B1
B0=$W/B0
NA=0
NB=1
ROUNDS=8
tree_of() {
	case $1 in
	A) echo "$A" ;;
	B1|B1n0) echo "$B1" ;;
	B0) echo "$B0" ;;
	*) return 1 ;;
	esac
}
EOF_ENV
cat > $L/p99.py << 'EOF_P99'
import itertools
import json
import math
import os
import re
import sys
import time

# p99.py: helpers of the NUMA p99 repeat.
#   sample OUT SECONDS      every second, cumulative CPU and run-queue figures
#                           of the ZFS threads and of fio, appended to OUT
#   run PREFIX IOPS         per-run figures from PREFIX.samples, PREFIX.win,
#                           PREFIX.stat0/1 and PREFIX.sched0/1 (one line)
#   offcpu OUT.json         off-CPU, run-queue and on-CPU time per thread
#                           group, from "perf script" output on stdin
#   summary RESULTS EXTRAS  phase 1 tables and verdicts
#   diag RESULTS EXTRAS DIR phase 2 tables

HZ = os.sysconf("SC_CLK_TCK")
GROUPS = (("rdint", "z_rd_int"), ("wrint", "z_wr_int"), ("rdiss", "z_rd_iss"),
    ("wriss", "z_wr_iss"), ("txg", "txg_sync"), ("fio", "fio"))
SHOWN = ("rdint", "wrint", "rdiss", "wriss", "txg", "fio")


def group_of(comm):
    for g, p in GROUPS:
        if comm.startswith(p):
            return g
    if comm.startswith(("z_", "txg_", "dp_")):
        return "zother"
    return None


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


def sample(out, seconds):
    f = open(out, "a", buffering=1)
    end = time.time() + seconds
    kind = {}
    while time.time() < end:
        now = time.time()
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
        for pid in pids:
            if pid not in kind:
                try:
                    c = open("/proc/%s/comm" % pid).read().strip()
                except OSError:
                    continue
                kind[pid] = c if group_of(c) else None
            if kind[pid] is None:
                continue
            try:
                st = open("/proc/%s/stat" % pid).read().rsplit(")", 1)[1]
                ss = open("/proc/%s/schedstat" % pid).read().split()
            except (OSError, IndexError):
                continue
            st = st.split()
            # proc(5): utime 14, stime 15, processor 39; st[0] is field 3.
            f.write("%.3f %s %s %s %d %s %s %s\n" % (now, pid, kind[pid],
                st[36], int(st[11]) + int(st[12]), ss[0], ss[1], ss[2]))
        alive = set(pids)
        for pid in list(kind):
            if pid not in alive:
                del kind[pid]
        time.sleep(max(0.0, 1.0 - (time.time() - now)))


def cpu_lines(path, idx):
    # {cpu: [values...]} from /proc/stat or /proc/schedstat cpuN lines.
    d = {}
    try:
        for ln in open(path):
            f = ln.split()
            if f and f[0].startswith("cpu") and f[0][3:].isdigit():
                d[int(f[0][3:])] = [int(f[i]) for i in idx]
    except (OSError, ValueError, IndexError):
        pass
    return d


def run_figures(prefix, iops):
    # Figures over the measured window (the 15 s of perf stat or trace).
    node = nodes()
    nn = max(node.values()) + 1
    try:
        t0, t1 = [float(x) for x in open(prefix + ".win").read().split()[:2]]
    except (OSError, ValueError):
        return {}
    per = {}
    try:
        for ln in open(prefix + ".samples"):
            f = ln.split()
            if len(f) == 8:
                per.setdefault(f[1], []).append((float(f[0]), f[2],
                    int(f[3]), int(f[4]), int(f[5]), int(f[6]), int(f[7])))
    except OSError:
        pass
    g = {}
    for tid, s in per.items():
        s = [x for x in s if t0 - 0.6 <= x[0] <= t1 + 0.6]
        if len(s) < 2:
            continue
        r = g.setdefault(group_of(s[0][1]), {"cpu": 0.0, "wait": 0,
            "slices": 0, "node": [0.0] * nn, "cpus": set(), "threads": 0})
        r["threads"] += 1
        r["cpu"] += (s[-1][3] - s[0][3]) / HZ
        r["wait"] += s[-1][5] - s[0][5]
        r["slices"] += s[-1][6] - s[0][6]
        for a, b in zip(s, s[1:]):
            if b[3] > a[3]:
                r["node"][node.get(b[2], 0)] += (b[3] - a[3]) / HZ
                r["cpus"].add(b[2])
    out = {}
    secs = max(t1 - t0, 1e-9)
    for k, r in g.items():
        tot = sum(r["node"])
        out[k] = {"threads": r["threads"],
            "cpu_us_io": 1e6 * r["cpu"] / (iops * secs) if iops > 0 else math.nan,
            "rq_us": r["wait"] / 1e3 / r["slices"] if r["slices"] else math.nan,
            "n1": 100.0 * r["node"][1] / tot if tot > 0 and nn > 1 else math.nan,
            "ncpus": len(r["cpus"])}
    s0 = cpu_lines(prefix + ".stat0", range(1, 9))
    s1 = cpu_lines(prefix + ".stat1", range(1, 9))
    q0 = cpu_lines(prefix + ".sched0", (8, 9))
    q1 = cpu_lines(prefix + ".sched1", (8, 9))
    for n in range(nn):
        busy = tot = wait = sl = 0
        for c in [c for c in node if node[c] == n]:
            if c in s0 and c in s1:
                d = [b - a for a, b in zip(s0[c], s1[c])]
                # user nice system idle iowait irq softirq steal
                busy += d[0] + d[1] + d[2] + d[5] + d[6] + d[7]
                tot += sum(d)
            if c in q0 and c in q1:
                wait += q1[c][0] - q0[c][0]
                sl += q1[c][1] - q0[c][1]
        out["node%d" % n] = {"busy": 100.0 * busy / tot if tot else math.nan,
            "rq_us": wait / 1e3 / sl if sl else math.nan}
    return out


def fio_cols(path, rw):
    # IOPS, MiB/s and completion latency p50/p99/p99.9 in us of a fio run.
    try:
        j = json.load(open(path))["jobs"][0]
        x = j["read" if "read" in rw else "write"]
        p = x.get("clat_ns", {}).get("percentile", {})
        return "%.0f %.1f %.0f %.0f %.0f" % (x["iops"], x["bw"] / 1024.0,
            p.get("50.000000", 0) / 1e3, p.get("99.000000", 0) / 1e3,
            p.get("99.900000", 0) / 1e3)
    except (OSError, ValueError, KeyError, IndexError):
        return "nan nan nan nan nan"


def fmt(x, f="%.1f"):
    return "-" if x is None or (isinstance(x, float) and math.isnan(x)) else f % x


def run_line(prefix, iops):
    r = run_figures(prefix, iops)
    cols = []
    for k in SHOWN:
        v = r.get(k, {})
        cols += [fmt(v.get("cpu_us_io"), "%.2f"), fmt(v.get("rq_us")),
            fmt(v.get("n1"), "%.0f"), str(v.get("ncpus", "-"))]
    for n in (0, 1):
        v = r.get("node%d" % n, {})
        cols += [fmt(v.get("busy"), "%.0f"), fmt(v.get("rq_us"))]
    return " ".join(cols)


def extras_header():
    h = []
    for k in SHOWN:
        h += [k + "_cpu_us_io", k + "_rq_us", k + "_n1pct", k + "_ncpus"]
    return " ".join(h + ["node0_busy", "node0_rq_us", "node1_busy",
        "node1_rq_us"])


SCHED = ("__schedule", "schedule", "schedule_timeout", "io_schedule",
    "schedule_hrtimeout_range_clock", "schedule_hrtimeout_range",
    "schedule_preempt_disabled", "io_schedule_timeout", "preempt_schedule",
    "preempt_schedule_common", "__cond_resched", "_cond_resched",
    "preempt_schedule_irq", "schedule_idle", "do_nanosleep")
LOCKS = ("__mutex_lock", "mutex_lock", "rwsem_down", "down_read", "down_write",
    "__rwsem", "rt_mutex", "rrw_enter", "__down", "osq_lock")


def plain(frames):
    # The frames under the scheduler and the tracing glue, innermost first.
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
        ("taskq_thread", "taskq-idle"), ("txg_thread_wait", "txg-idle"),
        ("cv_wait", "cv-wait"), ("cv_timedwait", "cv-wait")):
        if marker in joined:
            return cat
    return "other"


HDR = re.compile(r"^\s*(.+?)\s+(\d+)\s+\[(\d+)\]\s+(\d+\.\d+):\s+(\S+):\s*(.*)$")
SW = re.compile(r"prev_comm=(.*?) prev_pid=(\d+) prev_prio=(-?\d+) "
    r"prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+) next_prio=(-?\d+)")
WK = re.compile(r"comm=(.*?) pid=(\d+) prio=(-?\d+)")


def offcpu(out, stream, tids=None, nstacks=12):
    # Off-CPU intervals (switch-out to switch-in) per thread group, split by
    # category of the stack at switch-out and by node of that CPU, with the
    # run-queue part (wakeup to switch-in); on-CPU time per group and node.
    node = nodes()
    nn = max(node.values()) + 1
    want = set(tids) if tids else None
    pend, woke, oncpu, comm_of = {}, {}, {}, {}
    G = {}
    first = last = None

    def grp(tid, comm):
        if want is not None and comm.startswith("txg_sync"):
            return "txg" if tid in want else None
        return group_of(comm)

    def gr(g):
        return G.setdefault(g, {"off": {}, "rq": [], "on": [0.0] * nn,
            "cpus": set(), "stacks": {}, "tids": set()})

    def finish(ev):
        if ev is None:
            return
        t, cpu, kind, a = ev
        if kind == "switch":
            pcomm, ppid, pstate, ncomm, npid, frames = a
            gp = grp(ppid, pcomm)
            if gp is not None:
                pend[ppid] = (t, pstate, frames, cpu, gp)
                woke.pop(ppid, None)
                if ppid in oncpu:
                    t_in, c_in = oncpu.pop(ppid)
                    r = gr(gp)
                    r["on"][node.get(c_in, 0)] += t - t_in
                    r["cpus"].add(c_in)
            gn = grp(npid, ncomm)
            if gn is not None:
                oncpu[npid] = (t, cpu)
                if npid in pend:
                    t0, st, fr, c0, g0 = pend.pop(npid)
                    w = woke.pop(npid, None)
                    rq = t - (w if w is not None and w >= t0 else t0) \
                        if st.startswith("R") or w is not None else 0.0
                    r = gr(g0)
                    r["tids"].add(npid)
                    cat = category(st, fr)
                    k = (cat, node.get(c0, 0))
                    o = r["off"].setdefault(k, [0.0, 0, 0.0])
                    o[0] += t - t0
                    o[1] += 1
                    o[2] += rq
                    r["rq"].append(rq)
                    key = ";".join(reversed(plain(fr)[:10])) or "-"
                    key = "%s %s" % (cat, key)
                    s = r["stacks"].setdefault(key, [0.0, 0])
                    s[0] += t - t0
                    s[1] += 1
        elif kind == "wakeup":
            pid = a
            if pid in pend and pid not in woke:
                woke[pid] = t

    cur = None
    for ln in stream:
        if ": sched:" not in ln:
            # A frame of the callchain under the last event: "ADDR SYM".
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
    span = (last - first) if first is not None and last is not None else 0.0
    res = {"span_s": span, "groups": {}}
    for g, r in G.items():
        rq = sorted(r["rq"])
        pct = lambda p: rq[min(len(rq) - 1, int(p * len(rq)))] * 1e6 if rq else math.nan
        cats = {}
        for (cat, n), (s, c, q) in r["off"].items():
            e = cats.setdefault(cat, {"ms": 0.0, "n": 0, "rq_ms": 0.0,
                "ms_node": [0.0] * nn})
            e["ms"] += s * 1e3
            e["n"] += c
            e["rq_ms"] += q * 1e3
            e["ms_node"][n] += s * 1e3
        top = sorted(r["stacks"].items(), key=lambda kv: -kv[1][0])[:nstacks]
        res["groups"][g] = {"threads": len(r["tids"]), "cats": cats,
            "rq_p50_us": pct(0.5), "rq_p99_us": pct(0.99),
            "rq_max_us": rq[-1] * 1e6 if rq else math.nan,
            "wakeups": len(rq), "on_ms_node": [x * 1e3 for x in r["on"]],
            "on_ncpus": len(r["cpus"]),
            "stacks": [[k, v[0] * 1e3, v[1]] for k, v in top]}
    json.dump(res, open(out, "w"), indent=1)
    return res


def offcpu_text(res, groups=SHOWN):
    lines = ["trace span %.2f s" % res["span_s"]]
    for g in groups:
        r = res["groups"].get(g)
        if not r:
            continue
        on = r["on_ms_node"]
        lines.append("%-6s threads %d, on-CPU ms by node %s on %d CPUs, "
            "run-queue wait per wakeup p50 %s p99 %s max %s us (%d)" % (g,
            r["threads"], "/".join("%.0f" % x for x in on), r["on_ncpus"],
            fmt(r["rq_p50_us"], "%.0f"), fmt(r["rq_p99_us"], "%.0f"),
            fmt(r["rq_max_us"], "%.0f"), r["wakeups"]))
        for c, e in sorted(r["cats"].items(), key=lambda kv: -kv[1]["ms"]):
            lines.append("    off %-14s %9.1f ms %7d x  run-queue part %8.1f ms"
                "  by node %s" % (c, e["ms"], e["n"], e["rq_ms"],
                "/".join("%.0f" % x for x in e["ms_node"])))
        for k, ms, n in r["stacks"][:6]:
            lines.append("    %9.1f ms %7d x  %s" % (ms, n, k[:150]))
    return "\n".join(lines)


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return math.nan


def median(xs):
    xs = sorted(x for x in xs if not math.isnan(x))
    if not xs:
        return math.nan
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def quart(xs):
    xs = sorted(x for x in xs if not math.isnan(x))
    if not xs:
        return math.nan, math.nan

    def q(p):
        k = (len(xs) - 1) * p
        lo, hi = math.floor(k), math.ceil(k)
        return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)
    return q(0.25), q(0.75)


def mw_greater(a, b):
    # Exact one-sided Mann-Whitney test that b tends to exceed a: the share
    # of all splits of the pooled values whose U for b is at least the
    # observed one.
    a = [x for x in a if not math.isnan(x)]
    b = [x for x in b if not math.isnan(x)]
    if not a or not b:
        return math.nan

    def u(bb, aa):
        return sum(1.0 if y > x else 0.5 if y == x else 0.0
            for y in bb for x in aa)
    obs = u(b, a)
    pool = a + b
    idx = range(len(pool))
    tot = hit = 0
    for comb in itertools.combinations(idx, len(b)):
        s = set(comb)
        bb = [pool[i] for i in comb]
        aa = [pool[i] for i in idx if i not in s]
        tot += 1
        if u(bb, aa) >= obs - 1e-9:
            hit += 1
    return hit / tot


def load_runs(results, extras):
    # results: label build round cell ... columns (header line first).
    rows = {}
    head = None
    for ln in open(results):
        f = ln.split()
        if not f:
            continue
        if head is None:
            head = f
            continue
        rows[f[0]] = dict(zip(head, f))
    ehead = None
    try:
        for ln in open(extras):
            f = ln.split()
            if not f:
                continue
            if ehead is None:
                ehead = f
                continue
            if f[0] in rows:
                rows[f[0]].update(dict(zip(ehead[1:], f[1:])))
    except OSError:
        pass
    return list(rows.values())


CELLS = ("E1", "E2", "E3", "S1", "S2", "S3")
DESC = {"E1": "ureadlz4 node0  8 jobs (6/11: +14.8 %)",
    "E2": "ureadlz4 node1  8 jobs (6/11: +6.2 %)",
    "E3": "swrite   all   16 jobs (6/11: +40.2 %)",
    "S1": "ureadlz4 node0 16 jobs (scaled)",
    "S2": "ureadlz4 node1 16 jobs (scaled)",
    "S3": "swrite   all   32 jobs (scaled)"}


def summary(results, extras, held_out):
    runs = load_runs(results, extras)
    out = []
    held = []
    out.append("cell build  n | iops median [min..max] | p99 us median "
        "[q1..q3] [min..max] | p50 | p99.9")
    for cell in CELLS:
        cr = [r for r in runs if r.get("cell") == cell and r.get("mode") == "stat"]
        if not cr:
            continue
        out.append("%s %s" % (cell, DESC[cell]))
        by = {}
        for b in ("A", "B1"):
            v = [r for r in cr if r["build"] == b]
            by[b] = v
            io = [num(r["iops"]) for r in v]
            p = [num(r["p99_us"]) for r in v]
            q1, q3 = quart(p)
            out.append("   %-3s %2d | %7s [%s..%s] | %6s [%s..%s] [%s..%s] | %s | %s"
                % (b, len(v), fmt(median(io), "%.0f"), fmt(min(io, default=math.nan), "%.0f"),
                fmt(max(io, default=math.nan), "%.0f"), fmt(median(p), "%.0f"),
                fmt(q1, "%.0f"), fmt(q3, "%.0f"), fmt(min(p, default=math.nan), "%.0f"),
                fmt(max(p, default=math.nan), "%.0f"),
                fmt(median([num(r["p50_us"]) for r in v]), "%.0f"),
                fmt(median([num(r["p999_us"]) for r in v]), "%.0f")))
        pa = [num(r["p99_us"]) for r in by["A"]]
        pb = [num(r["p99_us"]) for r in by["B1"]]
        ia = [num(r["iops"]) for r in by["A"]]
        ib = [num(r["iops"]) for r in by["B1"]]
        ma, mb = median(pa), median(pb)
        rel = 100.0 * (mb - ma) / ma if ma and not math.isnan(ma) else math.nan
        ri = 100.0 * (median(ib) - median(ia)) / median(ia) if median(ia) else math.nan
        pair = []
        ra = dict((r["round"], num(r["p99_us"])) for r in by["A"])
        for r in by["B1"]:
            a = ra.get(r["round"])
            if a and not math.isnan(a):
                pair.append(100.0 * (num(r["p99_us"]) - a) / a)
        up = mw_greater(pa, pb)
        down = mw_greater(pb, pa)
        if not math.isnan(rel) and rel >= 5.0 and up <= 0.05:
            verdict = "HOLDS"
            held.append(cell)
        elif not math.isnan(rel) and rel <= -5.0 and down <= 0.05:
            verdict = "LOWER"
        else:
            verdict = "NOISE"
        out.append("   B1/A p99 %s %% (per round: median %s, %s..%s), iops %s %%,"
            " P(B1 > A) one-sided p = %s -> %s" % (fmt(rel, "%+.1f"),
            fmt(median(pair), "%+.1f"), fmt(min(pair, default=math.nan), "%+.1f"),
            fmt(max(pair, default=math.nan), "%+.1f"), fmt(ri, "%+.1f"),
            fmt(up, "%.4f"), verdict))
        ex = []
        for b in ("A", "B1"):
            v = by[b]
            ex.append("   %-3s rdint us/IO %s rq %s us n1 %s %% | wrint us/IO %s rq %s us"
                " n1 %s %% | fio rq %s us | node busy %s/%s %% rq %s/%s us | "
                "quota %s zilstall %s txgs %s peak %s %%" % (b,
                fmt(median([num(r.get("rdint_cpu_us_io")) for r in v]), "%.1f"),
                fmt(median([num(r.get("rdint_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("rdint_n1pct")) for r in v]), "%.0f"),
                fmt(median([num(r.get("wrint_cpu_us_io")) for r in v]), "%.1f"),
                fmt(median([num(r.get("wrint_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("wrint_n1pct")) for r in v]), "%.0f"),
                fmt(median([num(r.get("fio_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("node0_busy")) for r in v]), "%.0f"),
                fmt(median([num(r.get("node1_busy")) for r in v]), "%.0f"),
                fmt(median([num(r.get("node0_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("node1_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("quota")) for r in v]), "%.0f"),
                fmt(median([num(r.get("zilstall")) for r in v]), "%.0f"),
                fmt(median([num(r.get("txgs")) for r in v]), "%.0f"),
                fmt(max([num(r.get("peak_pct")) for r in v], default=math.nan), "%.0f")))
        out += ex
    open(held_out, "w").write(" ".join(held) + "\n")
    out.append("held: %s" % (" ".join(held) or "none"))
    return "\n".join(out)


ARMS = ("A", "B1", "B1n0", "B0")


def diag(results, extras, d):
    runs = [r for r in load_runs(results, extras) if r.get("mode") == "trace"]
    out = []
    for cell in CELLS:
        cr = [r for r in runs if r.get("cell") == cell]
        if not cr:
            continue
        out.append("%s %s" % (cell, DESC[cell]))
        for arm in ARMS:
            v = [r for r in cr if r["build"] == arm]
            if not v:
                continue
            out.append("  %-4s n %d iops %s p99 %s us (runs: %s) | node busy %s/%s %% "
                "rq %s/%s us | rdint rq %s us n1 %s %% cpus %s | wrint rq %s us "
                "n1 %s %% cpus %s" % (arm, len(v),
                fmt(median([num(r["iops"]) for r in v]), "%.0f"),
                fmt(median([num(r["p99_us"]) for r in v]), "%.0f"),
                "/".join(r["p99_us"] for r in v),
                fmt(median([num(r.get("node0_busy")) for r in v]), "%.0f"),
                fmt(median([num(r.get("node1_busy")) for r in v]), "%.0f"),
                fmt(median([num(r.get("node0_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("node1_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("rdint_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("rdint_n1pct")) for r in v]), "%.0f"),
                "/".join(r.get("rdint_ncpus", "-") for r in v),
                fmt(median([num(r.get("wrint_rq_us")) for r in v]), "%.1f"),
                fmt(median([num(r.get("wrint_n1pct")) for r in v]), "%.0f"),
                "/".join(r.get("wrint_ncpus", "-") for r in v)))
            for r in v[:1]:
                try:
                    res = json.load(open(os.path.join(d, r["label"] + ".offcpu.json")))
                except (OSError, ValueError):
                    out.append("       (no off-CPU data for %s)" % r["label"])
                    continue
                for g in ("fio", "rdint", "wrint"):
                    x = res["groups"].get(g)
                    if not x:
                        continue
                    cats = sorted(x["cats"].items(), key=lambda kv: -kv[1]["ms"])[:5]
                    out.append("       %-5s %s: rq/wakeup p50 %s p99 %s max %s us; on-CPU"
                        " ms by node %s on %d CPUs; off: %s" % (g, r["label"],
                        fmt(x["rq_p50_us"], "%.0f"), fmt(x["rq_p99_us"], "%.0f"),
                        fmt(x["rq_max_us"], "%.0f"),
                        "/".join("%.0f" % y for y in x["on_ms_node"]), x["on_ncpus"],
                        ", ".join("%s %.0f ms (rq %.0f)" % (c, e["ms"], e["rq_ms"])
                            for c, e in cats)))
    return "\n".join(out)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "sample":
        sample(sys.argv[2], float(sys.argv[3]))
    elif cmd == "fio":
        print(fio_cols(sys.argv[2], sys.argv[3]))
    elif cmd == "run":
        print(run_line(sys.argv[2], num(sys.argv[3])))
    elif cmd == "extras-header":
        print(extras_header())
    elif cmd == "offcpu":
        tids = None
        if len(sys.argv) > 3:
            tids = open(sys.argv[3]).read().split()
        print(offcpu_text(offcpu(sys.argv[2], sys.stdin, tids)))
    elif cmd == "summary":
        print(summary(sys.argv[2], sys.argv[3], sys.argv[4]))
    elif cmd == "diag":
        print(diag(sys.argv[2], sys.argv[3], sys.argv[4]))
    else:
        sys.exit("unknown command " + cmd)
EOF_P99
cat > $L/load.sh << 'EOF_LOAD'
#!/bin/bash
# load.sh ARM: replace the loaded spl/zfs with ARM's build, check what got
# loaded, and set zio_taskq_numa for the B arms (read at pool import).
set -u
. /var/log/numa-p99-1108/env.sh
arm=$1
T=$(tree_of "$arm") || { echo "unknown arm $arm"; exit 1; }
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ] ||
    { echo "a pool is imported: stopping"; exit 1; }
if grep -q -E '^(zfs|spl) ' /proc/modules; then
	(cd "$T" && ./scripts/zfs.sh -u) || { echo "unload failed"; exit 1; }
fi
(cd "$T" && ./scripts/zfs.sh) || { echo "load of $T failed"; exit 1; }
for m in spl zfs; do
	want=$(modinfo -F srcversion "$T/module/$m.ko")
	have=$(cat "/sys/module/$m/srcversion")
	[ "$want" = "$have" ] || { echo "$m loaded $have, built $want: stopping"; exit 1; }
done
M=/sys/module/zfs/parameters
case $arm in
B1|B0) echo 1 > "$M/zio_taskq_numa" || exit 1 ;;
B1n0) echo 0 > "$M/zio_taskq_numa" || exit 1 ;;
esac
[ "$(cat "$M/spa_asize_inflation")" = 24 ] ||
    { echo "spa_asize_inflation is not 24: stopping"; exit 1; }
echo "loaded $arm zfs $(cat /sys/module/zfs/srcversion)" \
    "zio_taskq_numa=$(cat "$M/zio_taskq_numa" 2> /dev/null || echo absent)"
EOF_LOAD
cat > $L/placement.sh << 'EOF_PLACE'
#!/bin/bash
# placement.sh: the zio taskqs split per node, and the CPUs each of their
# threads may run on.
echo "zio_taskq_numa=$(cat /sys/module/zfs/parameters/zio_taskq_numa 2>/dev/null || echo absent)"
grep -E '^z_(rd_iss|rd_int|wr_int)' /proc/spl/kstat/taskq/summary
for d in /proc/[0-9]*; do
	c=$(cat "$d/comm" 2> /dev/null) || continue
	case $c in
	z_rd_iss*|z_rd_int*|z_wr_int*)
		echo "$c cpus=$(awk '/^Cpus_allowed_list/ { print $2 }' "$d/status")" ;;
	esac
done | sort | uniq -c
EOF_PLACE
cat > $L/setup.sh << 'EOF_SETUP'
#!/bin/bash
# setup.sh: the 4 GiB RAM disk pre-faulted on node NA, the pool, and the
# files of the cells (as VM-NUMA-1105's Block 1 wrote them), with A loaded;
# the pool is left exported.
set -u
. /var/log/numa-p99-1108/env.sh
"$L/load.sh" A || exit 1
export PATH="$A:$PATH"
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
modprobe brd rd_nr=1 rd_size=$((4 * 1048576)) max_part=16 || exit 1
numactl --cpunodebind="$NA" --membind="$NA" dd if=/dev/zero of=/dev/ram0 \
    bs=1M count=4096 oflag=direct status=none || exit 1
grep -h MemUsed /sys/devices/system/node/node*/meminfo
mkdir -p "$MNT" "$L/raw"
zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
    -O recordsize=16k -O mountpoint="$MNT" "$P" /dev/ram0 || exit 1
zfs create -o primarycache=metadata -o compression=lz4 "$P/nocachelz4" &&
    zfs create "$P/sync" || exit 1
lay() {
	numactl --cpunodebind="$NA" --membind="$NA" fio --name=layout \
	    --directory="$MNT/$1" --filename_format='f.$jobnum' --size=16m \
	    --bs=1m --rw=write --numjobs="$2" --ioengine=psync --refill_buffers \
	    --buffer_compress_percentage=50 --fallocate=none --end_fsync=1 \
	    > "$L/raw/layout-$1.log" 2>&1
}
lay nocachelz4 16 && lay sync 32 || { echo "layout failed"; exit 1; }
zfs set sync=always "$P/sync" && zpool sync "$P" || exit 1
zpool list -Hp -o name,size,alloc,free "$P"
zfs list -Hp -o name,used,logicalused,compressratio -r "$P"
zpool export "$P" || exit 1
echo "setup done"
EOF_SETUP
cat > $L/run.sh << 'EOF_RUN'
#!/bin/bash
# run.sh LABEL ARM ROUND CELL MODE(stat|trace): one fio run of CELL exactly
# as VM-NUMA-1105's run.sh did it, plus a thread sampler, CPU snapshots
# around the measured window, a pool usage watchdog on writes and, in trace
# mode, a 2 s scheduler trace in place of perf stat.
set -u
. /var/log/numa-p99-1108/env.sh
label=$1 arm=$2 round=$3 cell=$4 mode=$5
case $cell in
E1) ds=nocachelz4 rw=randread nj=8 place=A ;;
E2) ds=nocachelz4 rw=randread nj=8 place=B ;;
S1) ds=nocachelz4 rw=randread nj=16 place=A ;;
S2) ds=nocachelz4 rw=randread nj=16 place=B ;;
E3) ds=sync rw=randwrite nj=16 place=all ;;
S3) ds=sync rw=randwrite nj=32 place=all ;;
*) echo "unknown cell $cell"; exit 1 ;;
esac
o=$L/raw/$label
case $place in
A) pre=(numactl --cpunodebind="$NA" --membind="$NA") ;;
B) pre=(numactl --cpunodebind="$NB" --membind="$NB") ;;
*) pre=() ;;
esac
pct() { zpool list -Hp -o size,alloc "$P" | awk '{ printf "%.1f", 100 * $2 / $1 }'; }
u=$(pct)
awk -v u="$u" 'BEGIN { exit !(u + 0 <= 60) }' ||
    { echo "pool $u % full before $label: stopping"; exit 3; }
# busy (steal included, as on 6 November), idle + iowait, steal
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6, $9 }' /proc/stat; }
tx() { awk '$1 ~ /^dmu_tx_(delay|dirty_throttle|dirty_over_max)$/ { s += $3 }
    $1 == "dmu_tx_quota" { q = $3 } END { print s + 0, q + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
zs() { awk '$1 == "zil_commit_stall_count" { print $3 + 0 }' /proc/spl/kstat/zfs/zil; }
tg() { awk 'END { print $1 + 0 }' "/proc/spl/kstat/zfs/$P/txgs"; }
snapcpu() {
	grep '^cpu[0-9]' /proc/stat > "$o.stat$1"
	grep '^cpu[0-9]' /proc/schedstat > "$o.sched$1"
}
python3 -I "$L/p99.py" sample "$o.samples" 45 &
spid=$!
la=$(cut -d' ' -f1 /proc/loadavg)
read -r b0 i0 s0 < <(cpu)
read -r t0 q0 < <(tx)
z0=$(zs) g0=$(tg)
"${pre[@]}" fio --name=t --directory="$MNT/$ds" --filename_format='f.$jobnum' \
    --size=16m --rw="$rw" --bs=16k --ioengine=psync --numjobs="$nj" \
    --time_based --runtime=30 --ramp_time=5 --group_reporting --fallocate=none \
    --refill_buffers --buffer_compress_percentage=50 --output-format=json \
    --output="$o.fio.json" > "$o.fio.log" 2>&1 &
fpid=$!
wpid=""
if [ "$rw" = randwrite ]; then
	(
		peak=0
		while kill -0 "$fpid" 2> /dev/null; do
			sleep 2
			u=$(pct)
			peak=$(awk -v a="$peak" -v b="$u" 'BEGIN { print (b + 0 > a + 0) ? b : a }')
			echo "$peak" > "$o.peak"
			if awk -v u="$u" 'BEGIN { exit !(u + 0 > 60) }'; then
				echo "pool $u % full during $label: fio stopped" > "$o.stopped"
				kill -INT "$fpid"
				break
			fi
		done
	) &
	wpid=$!
fi
sleep 10
snapcpu 0
w0=$(date +%s.%N)
case $mode in
stat)
	if [ -n "$EV" ]; then
		perf stat -a -x, -o "$o.perfstat" -e "$EV" -- sleep 15 > /dev/null 2>&1
	else
		sleep 15
	fi
	;;
trace)
	perf record -a -g -B -m 8M -e sched:sched_switch -e sched:sched_wakeup \
	    -e sched:sched_migrate_task -o "$W/trace.data" -- sleep 2 \
	    > "$o.perfrecord.log" 2>&1
	sleep 13
	;;
esac
w1=$(date +%s.%N)
snapcpu 1
echo "$w0 $w1" > "$o.win"
wait "$fpid"
[ -n "$wpid" ] && wait "$wpid"
read -r b1 i1 s1 < <(cpu)
read -r t1 q1 < <(tx)
z1=$(zs) g1=$(tg)
kill "$spid" 2> /dev/null
wait "$spid" 2> /dev/null
# Reports are built once fio has finished, so they do not steal its CPU.
if [ -s "$W/trace.data" ]; then
	perf sched latency -i "$W/trace.data" --sort max > "$o.latency.txt" 2>&1
	perf script -i "$W/trace.data" -F comm,tid,cpu,time,event,trace,ip,sym \
	    2> "$o.script.err" |
	    python3 -I "$L/p99.py" offcpu "$o.offcpu.json" > "$o.offcpu.txt"
	rm -f "$W/trace.data"
fi
perf_cols="- -"
if [ -s "$o.perfstat" ]; then
	perf_cols=$(awk -F, '{ v[$3] = $1 } END {
	    l = v["node-loads"]; m = v["node-load-misses"]
	    h = v["mem_load_uops_l3_miss_retired.remote_hitm"]
	    printf "%s %s", (l > 0 ? sprintf("%.1f", 100 * m / l) : "-"),
	        (h != "" ? sprintf("%.0f", h / 15) : "-") }' "$o.perfstat")
fi
read -r iops mib p50 p99 p999 < <(python3 -I "$L/p99.py" fio "$o.fio.json" "$rw")
busy=$(awk -v b="$((b1 - b0))" -v i="$((i1 - i0))" \
    'BEGIN { printf "%.0f", 100 * b / (b + i) }')
peak=$(cat "$o.peak" 2> /dev/null || echo -)
steal=$(awk -v s="$((s1 - s0))" -v t="$((b1 - b0 + i1 - i0))" \
    'BEGIN { printf "%.1f", (t > 0 ? 100 * s / t : 0) }')
line="$label $arm $round $cell $mode $ds $rw $nj $place $iops $mib $p50 $p99 $p999"
line="$line $busy $perf_cols $((t1 - t0)) $((q1 - q0)) $((z1 - z0)) $((g1 - g0)) $peak $steal $la"
echo "$line" >> "$L/results.tsv"
echo "$label $(python3 -I "$L/p99.py" run "$o" "$iops")" >> "$L/extras.tsv"
echo "$line"
if [ -e "$o.stopped" ]; then
	cat "$o.stopped"
	exit 4
fi
EOF_RUN
cat > $L/seg.sh << 'EOF_SEG'
#!/bin/bash
# seg.sh ARM ROUND MODE CELL...: load ARM's modules, import the pool,
# record the taskq placement, run the cells in order, export the pool.
set -u
. /var/log/numa-p99-1108/env.sh
arm=$1 round=$2 mode=$3
shift 3
k0=$(dmesg | wc -l)
"$L/load.sh" "$arm" || exit 1
PATH="$(tree_of "$arm"):$PATH"
export PATH
zpool import -d /dev -o cachefile=none "$P" || { echo "import failed: stopping"; exit 1; }
"$L/placement.sh" > "$L/raw/place.$mode.$arm.r$round.txt" 2>&1
rc=0
for cell in "$@"; do
	"$L/run.sh" "$mode.$cell.$arm.r$round" "$arm" "$round" "$cell" "$mode" ||
	    { rc=$?; break; }
done
zpool export "$P" || { echo "export failed: stopping"; exit 1; }
n=$(dmesg | tail -n +$((k0 + 1)) |
    grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|WARNING:')
echo "segment $mode $arm r$round: rc $rc, kernel log lines $n"
[ "$n" = 0 ] || exit 2
exit "$rc"
EOF_SEG
cat > $L/phase1.sh << 'EOF_P1'
#!/bin/bash
# phase1.sh: ROUNDS rounds of A and B1 (A then B1 in odd rounds, B1 then A
# in even ones), each segment running the six cells; then the summary.
set -u
. /var/log/numa-p99-1108/env.sh
for r in $(seq 1 "$ROUNDS"); do
	if [ $((r % 2)) = 1 ]; then order="A B1"; else order="B1 A"; fi
	for arm in $order; do
		"$L/seg.sh" "$arm" "$r" stat E1 E2 S1 S2 E3 S3 ||
		    { echo "round $r $arm failed: stopping"; exit 1; }
		echo "round $r $arm done $(date -u +%T)"
	done
done
python3 -I "$L/p99.py" summary "$L/results.tsv" "$L/extras.tsv" "$L/held.txt" |
    tee "$L/summary.txt"
EOF_P1
cat > $L/phase2.sh << 'EOF_P2'
#!/bin/bash
# phase2.sh: for the cells whose rise held, two rounds of four arms with a
# scheduler trace in each run; then the diagnosis tables.
set -u
. /var/log/numa-p99-1108/env.sh
held=$(cat "$L/held.txt")
[ -n "$held" ] || { echo "no cell holds: phase 2 skipped"; exit 0; }
for r in 1 2; do
	if [ "$r" = 1 ]; then order="A B1 B1n0 B0"; else order="B0 B1n0 B1 A"; fi
	for arm in $order; do
		# shellcheck disable=SC2086
		"$L/seg.sh" "$arm" "$r" trace $held ||
		    { echo "round $r $arm failed: stopping"; exit 1; }
		echo "trace round $r $arm done $(date -u +%T)"
	done
done
python3 -I "$L/p99.py" diag "$L/results.tsv" "$L/extras.tsv" "$L/raw" |
    tee "$L/diag.txt"
EOF_P2
chmod 0755 $L/*.sh
cd $L && sha256sum env.sh p99.py load.sh placement.sh setup.sh run.sh seg.sh phase1.sh phase2.sh
```

Must print:
```
9c6e25f194098d4b8c1ba335f11dec400d3673c68a6492db787a7757b3b896ec  env.sh
baec99d9571cfef9d2f4e7f81d9731d8d69c7cbc14a3fb8211f4653d1f61e0a5  p99.py
8ebe47d568429758937497386f6a058c1b90e5d4fa7f290757b36b236fa88462  load.sh
69bc34b7e53a003398553451e9a9115c6f46d20f6236509864dd2f8fad548b9d  placement.sh
2a28e08fa0938dd23ae0d4991ff5ec88e70f5ff43698c9a58ceefa3658af78ab  setup.sh
d2f21567122a073d713fd0729dd93dff0754c3992af6d7425f7b6d074215cf6c  run.sh
d7039e2aa88d7de214578da4ae174ab04d04ca0a719942ac8a519945957d7e05  seg.sh
74df87c9de841b19ab42ca9dd947136721b97bff60c9ff02ec4d548b96fbdab7  phase1.sh
5d4de079ea1a450363ac3db9b323eacea9c87d3138599080106962ef3d71c512  phase2.sh
```

Then the perf events (as Block 1 of VM-NUMA-1105 probed them), recorded in
`env.sh`:

```sh
L=/var/log/numa-p99-1108
EV=""
for e in node-loads node-load-misses node-stores node-store-misses \
    mem_load_uops_l3_miss_retired.local_dram \
    mem_load_uops_l3_miss_retired.remote_dram \
    mem_load_uops_l3_miss_retired.remote_hitm \
    mem_load_uops_l3_miss_retired.remote_fwd \
    context-switches cpu-migrations; do
	sudo -n perf stat -a -x, -e "$e" -- sleep 0.2 2>&1 > /dev/null |
	    grep -q -E '^[0-9]+,' && EV=${EV:+$EV,}$e
done
echo "EV=$EV" >> $L/env.sh && echo "EV=$EV"
echo "label build round cell mode ds rw jobs place iops MiBps p50_us p99_us p999_us cpu_pct remote_pct hitm_s throttles quota zilstall txgs peak_pct steal_pct load1" > $L/results.tsv
echo "label $(python3 -I $L/p99.py extras-header)" > $L/extras.tsv
```

`EV` should hold the events of 5 and 6 November (`node-loads`,
`node-load-misses`, ...); if it is empty, continue and say so: the `remote%`
and `hitm/s` columns will read `-`.

## STEP 3: the RAM disk, the pool and the files (about 2 min)

```sh
sudo -n bash /var/log/numa-p99-1108/setup.sh 2>&1 | tee /var/log/numa-p99-1108/setup.txt
```

Expected: a `loaded A` line; node 0's `MemUsed` about 4 GiB above node 1's;
`numap99` of about 4160749568 bytes with about 0.65 GiB allocated; the two
datasets with `nocachelz4` at a compressratio near 2; `setup done`.

## STEP 4: phase 1, the repeat (about 80 min)

Eight rounds; each round loads A then B1 (odd rounds) or B1 then A (even
rounds), reloading the modules and re-importing the pool for every segment,
and runs E1, E2, S1, S2, E3, S3 in that order. One command, the VM otherwise
idle:

```sh
sudo -n bash /var/log/numa-p99-1108/phase1.sh 2>&1 | tee /var/log/numa-p99-1108/phase1.log
```

Each segment prints a `loaded` line (check the `zio_taskq_numa` value: absent
for A, 1 for B1), one result line per cell and `segment ...: rc 0, kernel log
lines 0`. At the end it prints `summary.txt` and writes the held cells to
`held.txt`. After round 1, compare `raw/place.stat.A.r1.txt` and
`raw/place.stat.B1.r1.txt` with "Expected taskq placement" above.

A cell's rise **holds** when B1's median p99 is at least 5 % above A's and
the exact one-sided Mann-Whitney test of B1's eight p99 values against A's
gives p <= 0.05; **LOWER** is the same the other way; anything else is
**NOISE**.

## STEP 5: phase 2, the diagnosis of the held cells (0 to 2 h)

Only the cells in `held.txt` are run, in four arms (A, B1, B1n0, B0), two
rounds (A B1 B1n0 B0, then B0 B1n0 B1 A). Each run replaces `perf stat` with
a 2 s trace of the scheduler events (`perf record -a -g -e
sched:sched_switch -e sched:sched_wakeup -e sched:sched_migrate_task`),
reduced once fio has finished to `perf sched latency` and to the off-CPU
time of each thread group (`fio`, `z_rd_int`, `z_wr_int`, `z_rd_iss`,
`z_wr_iss`, `txg_sync`) by category of the stack it slept in, the run-queue
wait of each wakeup (wakeup to running), and the on-CPU time per node; the
trace file is deleted afterwards. If `held.txt` is empty this prints `no cell
holds: phase 2 skipped`.

```sh
sudo -n bash /var/log/numa-p99-1108/phase2.sh 2>&1 | tee /var/log/numa-p99-1108/phase2.log
```

## STEP 6: cleanup

```sh
W=/var/tmp/numa-p99-1108 L=/var/log/numa-p99-1108
sudo -n bash -c '
. /var/log/numa-p99-1108/env.sh
cur=$(cat /sys/module/zfs/srcversion 2> /dev/null)
T=$A
for t in "$A" "$B1" "$B0"; do
	[ -n "$cur" ] && [ "$(modinfo -F srcversion "$t/module/zfs.ko")" = "$cur" ] && T=$t
done
[ -z "$cur" ] && { (cd "$A" && ./scripts/zfs.sh) || echo "load of A failed"; }
if ! "$T/zpool" list -H -o name "$P" > /dev/null 2>&1 && grep -q "^brd " /proc/modules; then
	"$T/zpool" import -d /dev -o cachefile=none "$P" || echo "import for destroy failed"
fi
"$T/zpool" list -H -o name "$P" > /dev/null 2>&1 && { "$T/zpool" destroy "$P" || echo "destroy failed"; }
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null
grep -q "^brd " /proc/modules && { rmmod brd || echo "rmmod brd failed"; }
(cd "$T" && ./scripts/zfs.sh -u) || echo "unload failed"
grep -E "^(zfs|spl|brd) " /proc/modules
'
sudo -n rm -rf $W && echo "work dir removed; logs kept in $L"
du -sh $L
sudo -n fstrim -av
```

Expected: nothing between the two `find`/`grep` lines (no pool, no module
left), `work dir removed`, the size of L (under 200 MB), the fstrim summary.

## STEP 7: report

```sh
L=/var/log/numa-p99-1108
{
echo "== builds"; cat $L/builds.txt; grep -h '^EV=' $L/env.sh
echo "== setup"; cat $L/setup.txt
echo "== placement round 1"; for a in A B1; do echo "-- $a"; cat $L/raw/place.stat.$a.r1.txt; done
for a in B1n0 B0; do [ -f $L/raw/place.trace.$a.r1.txt ] && { echo "-- $a"; cat $L/raw/place.trace.$a.r1.txt; }; done
echo "== segments"; grep -h -E '^(loaded|segment|round|trace round|pool|no cell)' $L/phase1.log $L/phase2.log 2>/dev/null
echo "== phase 1 summary"; cat $L/summary.txt
echo "== pool usage peak (write runs)"; awk 'NR > 1 && $22 != "-" { if ($22 + 0 > m) m = $22 + 0 } END { print m + 0 " %" }' $L/results.tsv
echo "== noise per segment: steal % of CPU time and load1 before each run, min..max"
awk 'NR > 1 { k = $5 " " $2 " r" $3; s = $23 + 0; l = $24 + 0
    if (!(k in n)) { o[++m] = k; s0[k] = s1[k] = s; l0[k] = l1[k] = l }
    n[k]++; if (s < s0[k]) s0[k] = s; if (s > s1[k]) s1[k] = s
    if (l < l0[k]) l0[k] = l; if (l > l1[k]) l1[k] = l }
    END { for (i = 1; i <= m; i++) { k = o[i]
        printf "%s: steal %.1f..%.1f %%, load1 %.2f..%.2f\n", k, s0[k], s1[k], l0[k], l1[k] } }' $L/results.tsv
echo "== phase 2"; cat $L/diag.txt 2>/dev/null || echo "(not run)"
for c in $(cat $L/held.txt); do
	for a in A B1 B1n0 B0; do
		f=$L/raw/trace.$c.$a.r1
		[ -f $f.offcpu.txt ] || continue
		echo "-- $c $a r1 off-CPU"; cat $f.offcpu.txt
		echo "-- $c $a r1 perf sched latency"; grep -E '^ *(fio|z_rd_int|z_wr_int|z_rd_iss|z_wr_iss|txg_sync)' $f.latency.txt | head -12
	done
done
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed, and at
its end the output of STEP 6 (cleanup and fstrim) and every deviation from
this document (what and why). Reply with the whole of `REPORT.txt`.

## Reading the result

- **Phase 1.** For each cell, `B1/A p99` is the change of the median p99,
  `per round` the spread of B1 against the A of the same round, `p` the
  one-sided Mann-Whitney test. A 6 November rise that comes out NOISE here
  was not reproduced: a fluctuation of that single pass, or a product of that
  day's setup (16 vCPUs, the fuller pool). E1-E3 run on a VM twice as wide
  as on 6 November, so their absolute numbers are not comparable to that
  day's, only the B1/A change is.
- **Where the time goes** (the second lines of each cell, medians over the
  runs): `rdint`/`wrint us/IO` is the CPU time of the read/write completion
  threads per I/O, `rq` their mean run-queue wait per timeslice
  (`/proc/<pid>/schedstat`), `n1` the share of their CPU time on node 1;
  `fio rq` the same wait for the fio jobs; `node busy` and `rq` per node from
  `/proc/stat` and `/proc/schedstat`; `quota` (`dmu_tx_quota`), `zilstall`
  (`zil_commit_stall_count`) and `txgs` per run; `peak` the highest pool
  usage the watchdog saw.
- **Noise.** `steal` is the share of the guest's CPU time the hypervisor
  gave to others during a run (a busy host shows here; `cpu_pct` still
  counts it as busy, as on 6 November), `load1` the guest's load average
  just before the run. A segment whose steal stands out from the others is
  suspect; say which.
- **Phase 2**, per held cell and arm: `rq/wakeup` is the delay from a
  thread's wakeup to its running (p50, p99, max over the 2 s trace), `on-CPU
  ms by node` where the group ran and on how many CPUs, `off:` where it slept
  (`io-wait` = zio_wait, `zil-commit`, `txg-wait`, `lock`, `preempted` = run
  queue after preemption, `taskq-idle` = a taskq thread waiting for work),
  with the run-queue part of each.
- **What explains a held rise**, by arm:
  - B1n0 like A, B0 like A, B1 high: the binding of each completion thread to
    one CPU. Look for a higher `rq/wakeup` p99 of `rdint`/`wrint` in B1, on
    few CPUs: a woken completion thread waits for its one CPU while another
    is free. Kernels from 6.14 (the host runs 7.0) keep these threads on
    their node instead of one CPU, so the host would sit between B0 and this
    B1.
  - B0 also high: the per-node split itself. For node-bound reads, the
    completion work of 8 or 16 jobs on one node is confined to that node's
    CPUs, which the jobs already keep busy, where A also used the idle node:
    expect `node busy` near 100 % on the jobs' node and a higher `fio rq`.
    That cost would be specific to a load bound to one node.
  - B1n0 also high: not the per-node split; the 4 commits between A and B1's
    base, or another difference of the branch.
  - For `swrite`, compare the `zil-commit` and `io-wait` time of `fio` and the
    `wrint` run-queue wait: the log block completions run on `z_wr_int`.

sha256 of this file without this last line and the blank line before it: 74f4dd5337d40dfe877529af25afb2ab04d1a5f070fd983c2508c5c107ee13e9
