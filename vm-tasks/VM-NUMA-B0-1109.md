# VM 107 task: per-node zio completion taskqs (B0) against master (9/11)

These instructions are for VM 107: 32 vCPUs in two NUMA nodes of 16 (vCPU
0-15 pinned to host node 0, vCPU 16-31 to host node 1, distinct cores), 32
GiB of memory (16 GiB bound to each host node), Debian 13, kernel 6.12.
STEP 0 stops if the VM has another shape. Reply with
`/var/log/numa-b0-1109/REPORT.txt` (STEP 6).

**Background.** VM-NUMA-P99-1108 (8/11, this VM, this shape) compared stock
master with fork branch `numa-local-completion` in several arms. Binding each
per-node completion thread to one CPU (arm B1) raised the p99 latency of
16-job reads by 45-47 % and of sync writes by 11-20 %, through run-queue
waits on the bound CPUs. Arm B0, the per-node split with threads left
unbound, had the lowest p99 of the four arms in the two read cells and sat
above A in the two write cells, but it ran only in the diagnosis phase: 2
rounds, 4 cells, with a scheduler trace in every run.

| cell | A p99 us | B0 p99 us | B0 IOPS against A |
|---|---|---|---|
| S1 reads, node 0, 16 jobs | 180 | 155 | 206860 against about 190000 |
| S2 reads, node 1, 16 jobs | 194 | 186 | |
| E3 sync writes, 16 jobs | 475 | 494 | |
| S3 sync writes, 32 jobs | 498 | 541 | |

B0 has since been rebased onto current master as fork branch
`numa-node-completion` (one commit). On this VM it builds the same taskqs as
the B0 of 8/11; the change since then only matters where node ids are not
consecutive or a node has no CPU. This task measures it against its own
master base in all six cells of 8/11, 8 rounds each, without tracing, to
tell whether B0 lowers p99 or raises IOPS, and whether the write rise of the
two traced rounds above is real.

**Builds.**

| tree | commit | what it is |
|---|---|---|
| `A` | `47d0a322063639ed85aebce0ca8dc3af2605a75d` | upstream master, the base of the branch |
| `B0` | `ca28c26469c401ce819cdea1d44243fc533b7bbd` | fork `numa-node-completion` head: the read issue, read interrupt and write interrupt taskqs split into one group per NUMA node with CPUs, a zio dispatched to the group of the dispatching CPU's node; threads unbound; tunable `zio_taskq_numa` (default 1) |

**Cells**, as on 8/11 (E1-E3 are the 6 November cells, S1-S3 the same
scaled to this VM):

| cell | dataset | fio | jobs | placement |
|---|---|---|---|---|
| E1 | nocachelz4 | randread 16k | 8 | node 0 |
| E2 | nocachelz4 | randread 16k | 8 | node 1 |
| E3 | sync | randwrite 16k | 16 | unbound |
| S1 | nocachelz4 | randread 16k | 16 | node 0 |
| S2 | nocachelz4 | randread 16k | 16 | node 1 |
| S3 | sync | randwrite 16k | 32 | unbound |

Each run is 8/11's `run.sh` in its `stat` mode, unchanged in what fio does
(per-job 16 MiB files, psync, 30 s after a 5 s ramp, `perf stat` of the same
events during seconds 10-25), with the same once-a-second sampler of the ZFS
threads and fio, `/proc/stat` and `/proc/schedstat` snapshots around the 15 s
window, the `dmu_tx_quota`, ZIL stall and txg counts of the run, the pool
usage watchdog on writes, and the guest's steal time and load average.

**Rounds.** 8 rounds. Each round runs both arms back to back, A first in odd
rounds and B0 first in even ones (A B0, B0 A, A B0, ...), so that a drift of
the host during the run falls on both arms alike. Each arm's turn is one
segment: load its modules, import the pool, run E1, E2, S1, S2, E3, S3 in
that order, export the pool.

**The pool, and why it cannot fill.** The pool of 8/11, unchanged: a 4 GiB
RAM disk pre-faulted on node 0, ashift 12, recordsize 16K, two datasets:
`nocachelz4` (`primarycache=metadata`, `compression=lz4`, 16 files of 16 MiB
laid out with `--buffer_compress_percentage=50`, a compressratio of about
1.33) and `sync` (32 files, 0.5 GiB, `sync=always`). About 0.65 GiB live in
3.875 GiB (17 %). `spa_asize_inflation` stays at its default 24 (checked at
every module load). Reads write nothing. The sync writes add, at most, what
is in flight between txgs; with 24 the pool's quota check holds the unique
dirty data of all unsynced txgs under (free + 1/32 of the pool) / 24 = 134
MiB here, which bounds the peak at about 1.8 GiB (48 %), about 1.2 GiB (30
%) expected. On 8/11 the peak was 32 %. Every write run also has a watchdog
that stops fio above 60 %, and every run checks 60 % before it starts.

**Expected taskq placement**, once the pool is imported (32 CPUs, 2 nodes):

- A: `z_rd_int_0..4` and `z_wr_int_0..4` (5 taskqs, at most 5 threads each),
  `z_rd_iss` (one taskq, at most 8); every thread may run on CPUs 0-31.
- B0: `z_rd_int_0..5` and `z_wr_int_0..5` (6 taskqs, at most 4 threads each;
  `_0` to `_2` take the work dispatched from node 0, `_3` to `_5` from node
  1), `z_rd_iss_0` and `z_rd_iss_1` (at most 8 each); every thread may run on
  CPUs 0-31.

The taskqs are dynamic: a thread that has nothing to do exits, so fewer
threads than the maximum may be listed.

**Duration** (VM 107, 32 vCPUs): STEP 0 2 min; STEP 1 about 15 min (two
release builds with `make -j8`); STEP 2 1 min; STEP 3 2 min; STEP 4 about
65 min (16 segments of six 35 s runs, a module load and a pool import each;
the same 16 segments took about an hour on 8/11); STEP 5 5 min; STEP 6 2
min. In all about 1 h 35 min, of which STEPS 3-4 (about 1 h 10 min) need
the other two VMs idle.

## Rules

- Disposable VM, release builds, no other build, test or benchmark running.
  Builds run `nice -n 10 ionice -c3` with `make -j8`, one tree at a time.
- STEPS 3 and 4 are a performance A/B: run them only while the other two
  test VMs are idle (you are told when). Every run records the guest's steal
  time and load average (`steal_pct`, `load1` in `results.tsv`; STEP 6
  prints them per segment).
- Post nothing, push nothing, install nothing (no package, no `make
  install`), no `zfs-tests.sh`, never reboot.
- Work only under `/var/tmp/numa-b0-1109` (W) and `/var/log/numa-b0-1109`
  (L), on `/dev/ram0` and the pool `numab0`. Never run `zpool`, `zfs` or
  `zdb` while none of this task's modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:`,
  `WARNING:` or hung-task message, or a stop by the 60 % guard: stop and
  report. After a stop past STEP 1, go to STEP 5 with what you have.
- Leave `kernel.numa_balancing` as you find it (STEP 0 records it).
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must have the right shape and be free

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
ls -d /var/tmp/numa-b0-1109 /var/log/numa-b0-1109 2>&1       # No such file or directory, twice
df -h /var/tmp | tail -1                                       # 10G free or more
awk '/^cpu[0-9]/ { n++; if (NF != 10) bad++ } END { print n " cpu lines, " bad + 0 " not of 9 fields" }' /proc/schedstat
cat /proc/self/schedstat                                       # three numbers
```

Stop, changing nothing, if: `sudo -n true` fails; `nproc` is not 32;
`numactl -H` does not show `available: 2 nodes (0-1)` with `node 0 cpus: 0
1 ... 15` and `node 1 cpus: 16 17 ... 31`; MemTotal is under 30 GiB or
MemAvailable under 24 GiB; the kernel headers or a tool are missing; a zfs,
spl or brd module is loaded, or a pool is imported; a build, test or
benchmark runs; W or L exists; less than 10G is free on `/var/tmp`;
`/proc/schedstat` has no cpu lines or lines that are not of 9 fields.

## STEP 1: sources and two release builds (about 15 min)

```sh
W=/var/tmp/numa-b0-1109 L=/var/log/numa-b0-1109
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W $L/raw || exit 1
git init -q $W/repo && git -C $W/repo fetch -q --depth 2 https://github.com/tigerblue77/zfs numa-node-completion || exit 1
git -C $W/repo rev-parse FETCH_HEAD FETCH_HEAD^       # ca28c26469c401ce819cdea1d44243fc533b7bbd, then 47d0a322063639ed85aebce0ca8dc3af2605a75d
for t in A:47d0a322063639ed85aebce0ca8dc3af2605a75d B0:ca28c26469c401ce819cdea1d44243fc533b7bbd; do
    n=${t%%:*} c=${t#*:}
    git -C $W/repo worktree add -q --detach $W/$n $c && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
for n in A B0; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j8' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    echo "$n release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' $W/$n/Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' $W/$n/Makefile)" \
        "numa_param=$(modinfo -p $W/$n/module/zfs.ko | grep -c '^zio_taskq_numa:')" \
        "vermagic=$(modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1) srcversion=$(modinfo -F srcversion $W/$n/module/zfs.ko)"
done 2>&1 | tee $L/builds.txt
```

Expected: the two commits printed by `rev-parse` (the second is B0's parent,
the A tree); the two trees at their commits; `build rc=0` twice;
`release=1 noinline=0` twice; `numa_param=0` for A and `numa_param=1` for B0;
`vermagic` = `uname -r` twice; two different srcversions. On a failed build
quote `grep -nE 'error:|Error [0-9]' $L/build-<n>.log | head -20` and stop.

## STEP 2: the scripts, and the perf events this VM counts

`env.sh` holds the settings; `b0.py` samples, parses and builds the tables;
`load.sh` loads an arm's modules and checks them; `placement.sh` records the
taskqs and their threads' CPUs; `setup.sh` makes the RAM disk, the pool and
the files; `run.sh` runs one cell; `seg.sh` one segment (one module load, one
import, the six cells); `rounds.sh` the eight rounds and the summary.

```sh
L=/var/log/numa-b0-1109
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the B0 against A run, sourced by every script.
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/numa-b0-1109
L=/var/log/numa-b0-1109
P=numab0
MNT=$W/mnt
A=$W/A
B0=$W/B0
NA=0
NB=1
ROUNDS=8
tree_of() {
	case $1 in
	A) echo "$A" ;;
	B0) echo "$B0" ;;
	*) return 1 ;;
	esac
}
EOF_ENV
cat > $L/b0.py << 'EOF_PY'
import itertools
import json
import math
import os
import sys
import time

# b0.py: helpers of the B0 against A run.
#   sample OUT SECONDS      every second, cumulative CPU and run-queue figures
#                           of the ZFS threads and of fio, appended to OUT
#   fio JSON RW             IOPS, MiB/s, p50, p99, p99.9 of a fio run
#   run PREFIX IOPS         per-run figures from PREFIX.samples, PREFIX.win,
#                           PREFIX.stat0/1 and PREFIX.sched0/1 (one line)
#   extras-header           the header of extras.tsv
#   summary RESULTS EXTRAS  per cell tables, tests and verdicts

HZ = os.sysconf("SC_CLK_TCK")
GROUPS = (("rdint", "z_rd_int"), ("wrint", "z_wr_int"), ("rdiss", "z_rd_iss"),
    ("wriss", "z_wr_iss"), ("txg", "txg_sync"), ("fio", "fio"))
SHOWN = ("rdint", "wrint", "rdiss", "wriss", "txg", "fio")
ARMS = ("A", "B0")
CELLS = ("E1", "E2", "E3", "S1", "S2", "S3")
DESC = {"E1": "ureadlz4 node0  8 jobs",
    "E2": "ureadlz4 node1  8 jobs",
    "E3": "swrite   all   16 jobs",
    "S1": "ureadlz4 node0 16 jobs",
    "S2": "ureadlz4 node1 16 jobs",
    "S3": "swrite   all   32 jobs"}
# A verdict needs the median to move by at least this much (%) and the
# one-sided exact Mann-Whitney test in that direction to give p <= 0.05.
P99_PCT = 5.0
IOPS_PCT = 3.0
ALPHA = 0.05


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
    # Figures over the measured window (the 15 s of perf stat).
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


def rel(b, a):
    if math.isnan(a) or math.isnan(b) or a == 0:
        return math.nan
    return 100.0 * (b - a) / a


def per_round(by, key):
    # B0 against the A of the same round, in %.
    ra = dict((r["round"], num(r[key])) for r in by["A"])
    return [rel(num(r[key]), ra[r["round"]]) for r in by["B0"]
        if r["round"] in ra]


def verdict(change, p_down, p_up, pct, down, up, none):
    if not math.isnan(change) and change <= -pct and p_down <= ALPHA:
        return down
    if not math.isnan(change) and change >= pct and p_up <= ALPHA:
        return up
    return none


def spread(xs):
    xs = [x for x in xs if not math.isnan(x)]
    return "median %s, %s..%s" % (fmt(median(xs), "%+.1f"),
        fmt(min(xs, default=math.nan), "%+.1f"),
        fmt(max(xs, default=math.nan), "%+.1f"))


def summary(results, extras):
    runs = [r for r in load_runs(results, extras) if r.get("mode") == "stat"]
    out = ["cell arm  n | iops median [min..max] | p99 us median [q1..q3] "
        "[min..max] | p50 | p99.9 | steal % max | load1 max"]
    tally = {"LOWER": [], "HIGHER": [], "GAIN": [], "LOSS": []}
    for cell in CELLS:
        cr = [r for r in runs if r.get("cell") == cell]
        if not cr:
            continue
        out.append("%s %s" % (cell, DESC[cell]))
        by = {}
        for b in ARMS:
            v = [r for r in cr if r["build"] == b]
            by[b] = v
            io = [num(r["iops"]) for r in v]
            p = [num(r["p99_us"]) for r in v]
            q1, q3 = quart(p)
            out.append("   %-3s %2d | %7s [%s..%s] | %6s [%s..%s] [%s..%s] | %s | %s"
                " | %s | %s" % (b, len(v), fmt(median(io), "%.0f"),
                fmt(min(io, default=math.nan), "%.0f"),
                fmt(max(io, default=math.nan), "%.0f"), fmt(median(p), "%.0f"),
                fmt(q1, "%.0f"), fmt(q3, "%.0f"),
                fmt(min(p, default=math.nan), "%.0f"),
                fmt(max(p, default=math.nan), "%.0f"),
                fmt(median([num(r["p50_us"]) for r in v]), "%.0f"),
                fmt(median([num(r["p999_us"]) for r in v]), "%.0f"),
                fmt(max([num(r.get("steal_pct")) for r in v], default=math.nan), "%.1f"),
                fmt(max([num(r.get("load1")) for r in v], default=math.nan), "%.2f")))
        pa = [num(r["p99_us"]) for r in by["A"]]
        pb = [num(r["p99_us"]) for r in by["B0"]]
        ia = [num(r["iops"]) for r in by["A"]]
        ib = [num(r["iops"]) for r in by["B0"]]
        dp = rel(median(pb), median(pa))
        di = rel(median(ib), median(ia))
        p_lo = mw_greater(pb, pa)
        p_hi = mw_greater(pa, pb)
        i_up = mw_greater(ia, ib)
        i_dn = mw_greater(ib, ia)
        vp = verdict(dp, p_lo, p_hi, P99_PCT, "LOWER", "HIGHER", "NOISE")
        vi = verdict(di, i_dn, i_up, IOPS_PCT, "LOSS", "GAIN", "NO GAIN")
        for v in (vp, vi):
            if v in tally:
                tally[v].append(cell)
        out.append("   p99  B0/A %s %% (per round: %s), p(B0 lower) %s,"
            " p(B0 higher) %s -> %s" % (fmt(dp, "%+.1f"),
            spread(per_round(by, "p99_us")), fmt(p_lo, "%.4f"),
            fmt(p_hi, "%.4f"), vp))
        out.append("   iops B0/A %s %% (per round: %s), p(B0 higher) %s,"
            " p(B0 lower) %s -> %s" % (fmt(di, "%+.1f"),
            spread(per_round(by, "iops")), fmt(i_up, "%.4f"),
            fmt(i_dn, "%.4f"), vi))
        for b in ARMS:
            v = by[b]
            out.append("   %-3s rdint us/IO %s rq %s us n1 %s %% | wrint us/IO %s rq %s us"
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
    out.append("p99:  LOWER in %s; HIGHER in %s" % (
        " ".join(tally["LOWER"]) or "none", " ".join(tally["HIGHER"]) or "none"))
    out.append("iops: GAIN in %s; LOSS in %s" % (
        " ".join(tally["GAIN"]) or "none", " ".join(tally["LOSS"]) or "none"))
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
    elif cmd == "summary":
        print(summary(sys.argv[2], sys.argv[3]))
    else:
        sys.exit("unknown command " + cmd)
EOF_PY
cat > $L/load.sh << 'EOF_LOAD'
#!/bin/bash
# load.sh ARM: replace the loaded spl/zfs with ARM's build and check what got
# loaded: srcversions, zio_taskq_numa (absent for A, 1 by default for B0),
# spa_asize_inflation.
set -u
. /var/log/numa-b0-1109/env.sh
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
numa=$(cat "$M/zio_taskq_numa" 2> /dev/null || echo absent)
case $arm in
A) want=absent ;;
B0) want=1 ;;
esac
[ "$numa" = "$want" ] || { echo "zio_taskq_numa is $numa, not $want: stopping"; exit 1; }
[ "$(cat "$M/spa_asize_inflation")" = 24 ] ||
    { echo "spa_asize_inflation is not 24: stopping"; exit 1; }
echo "loaded $arm zfs $(cat /sys/module/zfs/srcversion) zio_taskq_numa=$numa"
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
# files of the cells (as on 8/11), with A loaded; the pool is left exported.
set -u
. /var/log/numa-b0-1109/env.sh
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
# run.sh LABEL ARM ROUND CELL: one fio run of CELL as on 8/11 (stat mode),
# with a thread sampler, CPU snapshots around the measured window, a pool
# usage watchdog on writes, and the guest's steal time and load average.
set -u
. /var/log/numa-b0-1109/env.sh
label=$1 arm=$2 round=$3 cell=$4 mode=stat
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
# busy (steal included, as on 6 and 8 November), idle + iowait, steal
cpu() { awk '/^cpu / { print $2 + $3 + $4 + $7 + $8 + $9, $5 + $6, $9 }' /proc/stat; }
tx() { awk '$1 ~ /^dmu_tx_(delay|dirty_throttle|dirty_over_max)$/ { s += $3 }
    $1 == "dmu_tx_quota" { q = $3 } END { print s + 0, q + 0 }' /proc/spl/kstat/zfs/dmu_tx; }
zs() { awk '$1 == "zil_commit_stall_count" { print $3 + 0 }' /proc/spl/kstat/zfs/zil; }
tg() { awk 'END { print $1 + 0 }' "/proc/spl/kstat/zfs/$P/txgs"; }
snapcpu() {
	grep '^cpu[0-9]' /proc/stat > "$o.stat$1"
	grep '^cpu[0-9]' /proc/schedstat > "$o.sched$1"
}
python3 -I "$L/b0.py" sample "$o.samples" 45 &
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
if [ -n "$EV" ]; then
	perf stat -a -x, -o "$o.perfstat" -e "$EV" -- sleep 15 > /dev/null 2>&1
else
	sleep 15
fi
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
perf_cols="- -"
if [ -s "$o.perfstat" ]; then
	perf_cols=$(awk -F, '{ v[$3] = $1 } END {
	    l = v["node-loads"]; m = v["node-load-misses"]
	    h = v["mem_load_uops_l3_miss_retired.remote_hitm"]
	    printf "%s %s", (l > 0 ? sprintf("%.1f", 100 * m / l) : "-"),
	        (h != "" ? sprintf("%.0f", h / 15) : "-") }' "$o.perfstat")
fi
read -r iops mib p50 p99 p999 < <(python3 -I "$L/b0.py" fio "$o.fio.json" "$rw")
busy=$(awk -v b="$((b1 - b0))" -v i="$((i1 - i0))" \
    'BEGIN { printf "%.0f", 100 * b / (b + i) }')
peak=$(cat "$o.peak" 2> /dev/null || echo -)
steal=$(awk -v s="$((s1 - s0))" -v t="$((b1 - b0 + i1 - i0))" \
    'BEGIN { printf "%.1f", (t > 0 ? 100 * s / t : 0) }')
line="$label $arm $round $cell $mode $ds $rw $nj $place $iops $mib $p50 $p99 $p999"
line="$line $busy $perf_cols $((t1 - t0)) $((q1 - q0)) $((z1 - z0)) $((g1 - g0)) $peak $steal $la"
echo "$line" >> "$L/results.tsv"
echo "$label $(python3 -I "$L/b0.py" run "$o" "$iops")" >> "$L/extras.tsv"
echo "$line"
if [ -e "$o.stopped" ]; then
	cat "$o.stopped"
	exit 4
fi
EOF_RUN
cat > $L/seg.sh << 'EOF_SEG'
#!/bin/bash
# seg.sh ARM ROUND: load ARM's modules, import the pool, record the taskq
# placement, run the six cells in order, export the pool.
set -u
. /var/log/numa-b0-1109/env.sh
arm=$1 round=$2
k0=$(dmesg | wc -l)
"$L/load.sh" "$arm" || exit 1
PATH="$(tree_of "$arm"):$PATH"
export PATH
zpool import -d /dev -o cachefile=none "$P" || { echo "import failed: stopping"; exit 1; }
"$L/placement.sh" > "$L/raw/place.$arm.r$round.txt" 2>&1
rc=0
for cell in E1 E2 S1 S2 E3 S3; do
	"$L/run.sh" "$cell.$arm.r$round" "$arm" "$round" "$cell" || { rc=$?; break; }
done
zpool export "$P" || { echo "export failed: stopping"; exit 1; }
n=$(dmesg | tail -n +$((k0 + 1)) |
    grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|WARNING:')
echo "segment $arm r$round: rc $rc, kernel log lines $n"
[ "$n" = 0 ] || exit 2
exit "$rc"
EOF_SEG
cat > $L/rounds.sh << 'EOF_ROUNDS'
#!/bin/bash
# rounds.sh: ROUNDS rounds of A and B0 (A then B0 in odd rounds, B0 then A
# in even ones), each segment running the six cells; then the summary.
set -u
. /var/log/numa-b0-1109/env.sh
for r in $(seq 1 "$ROUNDS"); do
	if [ $((r % 2)) = 1 ]; then order="A B0"; else order="B0 A"; fi
	for arm in $order; do
		"$L/seg.sh" "$arm" "$r" || { echo "round $r $arm failed: stopping"; exit 1; }
		echo "round $r $arm done $(date -u +%T)"
	done
done
python3 -I "$L/b0.py" summary "$L/results.tsv" "$L/extras.tsv" | tee "$L/summary.txt"
EOF_ROUNDS
chmod 0755 $L/*.sh
cd $L && sha256sum env.sh b0.py load.sh placement.sh setup.sh run.sh seg.sh rounds.sh
```

Must print:
```
60d8382c29df4a1ad0b347385bb901cd8db0f15281aa82dfcbbf54be1750978c  env.sh
d618d75d74ca989ed9df4a35fc8dd9104f2eaef5d7c64acbd919290b351c389b  b0.py
41885de32c925d0ebadded533a8bf6229347093fe33a16222b3b1bd00f187d0c  load.sh
69bc34b7e53a003398553451e9a9115c6f46d20f6236509864dd2f8fad548b9d  placement.sh
24cff8568be03572d743fbcd8364c6f8bf4294285d37d184c3deb47013c12985  setup.sh
89aa6bd27e26c87dbef27f6f5940595f89002d5b68659a59aa9d956c25c702d3  run.sh
015eba0ac9324194120bb232df6cebd9a1c1940bbd02a903bbafdc8885bc6e40  seg.sh
9ec7959a32e6f8992b384eb8ac89d5059e52ea007c4edad48b27861b1b5cd578  rounds.sh
```

Then the perf events (as on 8/11), recorded in `env.sh`, and the headers of
the two tables:

```sh
L=/var/log/numa-b0-1109
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
echo "label $(python3 -I $L/b0.py extras-header)" > $L/extras.tsv
```

`EV` should hold the events of 8/11 (`node-loads`, `node-load-misses`, ...);
if it is empty, continue and say so: the `remote%` and `hitm/s` columns will
read `-`.

## STEP 3: the RAM disk, the pool and the files (about 2 min)

Only while the other two test VMs are idle.

```sh
sudo -n bash /var/log/numa-b0-1109/setup.sh 2>&1 | tee /var/log/numa-b0-1109/setup.txt
```

Expected: a `loaded A ... zio_taskq_numa=absent` line; node 0's `MemUsed`
about 4 GiB above node 1's; `numab0` of about 4160749568 bytes with about
0.65 GiB allocated; the two datasets, `nocachelz4` at a compressratio near
1.33; `setup done`.

## STEP 4: the eight rounds (about 65 min)

Only while the other two test VMs are idle. One command, the VM otherwise
idle:

```sh
sudo -n bash /var/log/numa-b0-1109/rounds.sh 2>&1 | tee /var/log/numa-b0-1109/rounds.log
```

Each segment prints a `loaded` line (`zio_taskq_numa=absent` for A, `=1` for
B0), one result line per cell and `segment ...: rc 0, kernel log lines 0`.
After round 1, compare `raw/place.A.r1.txt` and `raw/place.B0.r1.txt` with
"Expected taskq placement" above; if B0 shows the same taskqs as A, stop. At
the end it prints `summary.txt`.

Per cell, the p99 verdict is **LOWER** when B0's median p99 is at least 5 %
below A's and the exact one-sided Mann-Whitney test of B0's eight p99 values
against A's gives p <= 0.05, **HIGHER** the same the other way, **NOISE**
otherwise. The IOPS verdict is **GAIN** when B0's median IOPS is at least 3 %
above A's with the one-sided test at p <= 0.05, **LOSS** the same the other
way, **NO GAIN** otherwise. The last two lines list the cells of each
verdict.

## STEP 5: cleanup

```sh
W=/var/tmp/numa-b0-1109 L=/var/log/numa-b0-1109
sudo -n bash -c '
. /var/log/numa-b0-1109/env.sh
cur=$(cat /sys/module/zfs/srcversion 2> /dev/null)
T=$A
for t in "$A" "$B0"; do
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
left), `work dir removed`, the size of L (under 100 MB), the fstrim summary.

## STEP 6: report

```sh
L=/var/log/numa-b0-1109
{
echo "== builds"; cat $L/builds.txt; grep -h '^EV=' $L/env.sh
echo "== setup"; cat $L/setup.txt
echo "== placement round 1"; for a in A B0; do echo "-- $a"; cat $L/raw/place.$a.r1.txt; done
echo "== segments"; grep -h -E '^(loaded|segment|round|pool|zio_taskq_numa is)' $L/rounds.log
echo "== summary"; cat $L/summary.txt 2>/dev/null || python3 -I $L/b0.py summary $L/results.tsv $L/extras.tsv
echo "== pool usage peak (write runs)"; awk 'NR > 1 && $22 != "-" { if ($22 + 0 > m) m = $22 + 0 } END { print m + 0 " %" }' $L/results.tsv
echo "== noise per segment: steal % of CPU time and load1 before each run, min..max"
awk 'NR > 1 { k = $2 " r" $3; s = $23 + 0; l = $24 + 0
    if (!(k in n)) { o[++m] = k; s0[k] = s1[k] = s; l0[k] = l1[k] = l }
    n[k]++; if (s < s0[k]) s0[k] = s; if (s > s1[k]) s1[k] = s
    if (l < l0[k]) l0[k] = l; if (l > l1[k]) l1[k] = l }
    END { for (i = 1; i <= m; i++) { k = o[i]
        printf "%s: steal %.1f..%.1f %%, load1 %.2f..%.2f\n", k, s0[k], s1[k], l0[k], l1[k] } }' $L/results.tsv
echo "== results.tsv"; cat $L/results.tsv
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed and which
other VMs were busy at any time during STEPS 3-4 (none, if the rules held),
and at its end the output of STEP 5 (cleanup and fstrim) and every deviation
from this document (what and why). Reply with the whole of `REPORT.txt`.

## Reading the result

- **Verdicts.** For each cell, `B0/A` is the change of the median, `per
  round` the spread of B0 against the A of the same round, and the two `p`
  the one-sided exact Mann-Whitney tests in each direction (with 8 runs per
  arm the smallest possible p is 1/12870). A verdict needs both the size and
  the test. Twelve verdicts are drawn, so one p <= 0.05 by chance is not
  unlikely; the size threshold is what keeps a chance result from counting.
- **What settles B0.** B0 is worth proposing as it is when no cell is
  HIGHER or LOSS and at least one is LOWER or GAIN. A HIGHER or LOSS only
  in the write cells (E3, S3) would point at the split of `z_wr_int`, which
  could then be left out; one in the node-bound read cells would point at
  the split itself.
- **Where the time goes** (the last two lines of each cell, medians over the
  runs): `rdint`/`wrint us/IO` is the CPU time of the read/write completion
  threads per I/O, `rq` their mean run-queue wait per timeslice
  (`/proc/<pid>/schedstat`), `n1` the share of their CPU time on node 1: in
  the reads bound to node 0 (E1, S1) a lower `n1` for B0 than for A means
  B0's completions run on the node that issued the reads, in E2 and S2 a
  higher one. `fio rq` is the same wait for the fio jobs; `node busy` and
  `rq` per node come from `/proc/stat` and `/proc/schedstat`; `quota`
  (`dmu_tx_quota`), `zilstall` (`zil_commit_stall_count`) and `txgs` per
  run; `peak` the highest pool usage the watchdog saw.
- **Noise.** `steal` is the share of the guest's CPU time the hypervisor
  gave to others during a run, `load1` the guest's load average just before
  it. A segment whose steal stands out from the others is suspect; say
  which.

sha256 of this file without this last line and the blank line before it: 6013683efcab1492c06886d8d1457fcc49b85763205e09a157a8867f5d8293ee
