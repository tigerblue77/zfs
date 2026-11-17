#!/usr/bin/env python3
# TASK-1118-V6P: summarize the raw results of v6p-bench.sh as Markdown.
# Usage: v6p-report.py /var/log/task-1118-v6p/raw [PREFIX] > out.md
# Only the passes whose name starts with PREFIX (default P) are read, so
# that the A/B passes (P*), the traced passes (H*) and the haltpoll passes
# (Q*) are summarized apart.  It is TASK-1117-V5P's report with v6 in
# place of v5, V5P's v5 ratio beside each row, a tail rule on the fsync
# rows, and the scheduler tables of the traced passes.
#
# Per row, over the passes where both arms ran it: ratio = v6 IOPS /
# master IOPS in the same pass; geo = geometric mean of the ratios; won =
# passes where v6 was higher; p perm = exact two-sided sign-flip
# permutation test of the log ratios; p sign = exact two-sided sign test;
# p MW = exact two-sided rank test of the two arms' IOPS, unpaired.
# Verdict, fixed before the run: "gain" if geo >= 1.03 and p perm <= 0.05,
# "loss" if geo <= 0.97 and p perm <= 0.05, otherwise "no difference
# shown".  Tail rule on the fsync rows (r1, x1), fixed before the run:
# with t = v6 fsync p99.9 / master fsync p99.9 per pass, "tail cost" if
# geo(t) >= 1.20 and p perm <= 0.05, "tail gain" if geo(t) <= 0.83 and
# p perm <= 0.05, otherwise "no tail difference shown".
import glob
import itertools
import json
import math
import os
import re
import statistics
import sys

RAW = sys.argv[1]
PREFIX = sys.argv[2] if len(sys.argv) > 2 else "P"
ROWS = ["n8", "n8e", "r1", "x1", "n32"]
ARMS = ["master", "v6"]
ROW_POOL = {"x1": "c1q", "r1": "c1r", "n8": "c1n", "n32": "c1n",
            "n8e": "c1n"}
FSYNC_ROWS = ("x1", "r1")
# TASK-1117-V5P, geo ratio v5 / master over its passes (6, 8 on null_blk).
V5P_GEO = {"x1": 1.053, "r1": 1.132, "n8": 1.137, "n8e": 1.142,
           "n32": 0.982}
PCT = [("p50", "50.000000"), ("p99", "99.000000"), ("p99.9", "99.900000")]
CLASSES = ["z_wr_iss", "z_wr_int", "z_flush", "z_zvol", "txg_sync", "fio",
           "loop_kworker"]


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def lines(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except OSError:
        return []


def pctl(lat):
    out = {}
    p = (lat or {}).get("percentile") or {}
    for name, key in PCT:
        if key in p:
            out[name] = p[key] / 1000.0
    return out


def tqmap(d):
    """{kstat name: pool} for the arm directory d."""
    m = {}
    for line in lines(os.path.join(d, "tqmap.txt")):
        p = line.split()
        if len(p) == 2:
            m[p[1]] = p[0]
    return m


def lane(kname):
    base = kname.rsplit(".", 1)[0]
    if base.startswith("z_wr_iss_h"):
        return "iss_h"
    if base.startswith("z_wr_iss"):
        return "iss"
    if base.startswith("z_wr_int"):
        return "int"
    if base.startswith("z_flush"):
        return "flush"
    return "zvol"


def iops_series(d, row):
    """Total write IOPS per whole second, over all jobs' iops logs."""
    sec = {}
    for f in glob.glob(os.path.join(d, row + "_iops.*.log")):
        for line in lines(f):
            p = [x.strip() for x in line.split(",")]
            if len(p) < 3 or p[2] != "1":
                continue
            s = int(p[0]) // 1000
            sec[s] = sec.get(s, 0) + int(p[1])
    if len(sec) < 3:
        return []
    keys = sorted(sec)[1:-1]
    return [sec[k] for k in keys]


def one(d, row):
    fio = load(os.path.join(d, row + ".json"))
    if fio is None or not fio.get("jobs"):
        return None
    r = {}
    j = fio["jobs"][0]
    w = j["write"]
    ops = w["iops"]
    r["IOPS"] = ops
    lat = w.get("lat_ns") if w.get("lat_ns", {}).get("percentile") \
        else w.get("clat_ns")
    for k, v in pctl(lat).items():
        r["wr " + k] = v
    sync = j.get("sync") or {}
    if sync.get("total_ios"):
        for k, v in pctl(sync.get("lat_ns")).items():
            r["fsync " + k] = v
    s0 = load(os.path.join(d, row + ".snap0.json"))
    s1 = load(os.path.join(d, row + ".snap1.json"))
    if not (s0 and s1):
        return r
    dt = s1["t"] - s0["t"]
    nops = ops * dt if ops else 0
    hz = s1.get("hz", 100)
    dc = [b - a for a, b in zip(s0["cpu"], s1["cpu"])]
    tot = sum(dc[:8]) or 1
    busy = tot - dc[3] - dc[4]
    r["cpu busy %"] = 100.0 * busy / tot
    r["steal %"] = 100.0 * dc[7] / tot
    r["load1 end"] = s1["load1"]
    if nops:
        r["busy CPU us/op"] = busy / hz * 1e6 / nops
    pool = ROW_POOL[row]
    dev0, dev1 = s0.get("dev", {}), s1.get("dev", {})
    for dv, x1 in dev1.items():
        x0 = dev0.get(dv)
        if x0 is None:
            continue
        if x1["pool"] == "DISK":
            r["disk MB"] = (x1["sectors_written"] -
                            x0["sectors_written"]) * 512 / 1e6
        if x1["pool"] == pool:
            r["_fl"] = r.get("_fl", 0) + x1["flushes"] - x0["flushes"]
            r["_flms"] = r.get("_flms", 0) + x1["flush_ms"] - x0["flush_ms"]
            if pool == "c1r":
                r["test disk MB"] = (x1["sectors_written"] -
                                     x0["sectors_written"]) * 512 / 1e6
    if nops and "_fl" in r:
        r["flushes/op"] = r["_fl"] / nops
        if r["_fl"]:
            r["flush ms"] = r["_flms"] / r["_fl"]
    for k in ("_fl", "_flms"):
        r.pop(k, None)
    t0, t1 = s0.get("threads", {}), s1.get("threads", {})
    for c in CLASSES:
        if c in t1 and c in t0:
            r["cpu " + c] = (t1[c]["ticks"] - t0[c]["ticks"]) / hz
            r["rq ms " + c] = (t1[c]["wait_ns"] - t0[c]["wait_ns"]) / 1e6
    z0, z1 = s0.get("zil", {}), s1.get("zil", {})
    if nops and z1:
        dz = {k: z1[k] - z0.get(k, 0) for k in z1}
        r["commits/op"] = dz.get("zil_commit_writer_count", 0) / nops
        lw = dz.get("zil_itx_metaslab_normal_count", 0)
        r["lwbs/op"] = lw / nops
        if lw:
            r["KiB/lwb"] = dz.get("zil_itx_metaslab_normal_write", 0) / \
                lw / 1024
    tm = tqmap(d)
    k0, k1 = s0.get("taskq", {}), s1.get("taskq", {})
    if nops and k1:
        for kn, v1 in k1.items():
            if tm.get(kn) != pool or kn not in k0:
                continue
            ln = lane(kn)
            v0 = k0[kn]
            ex = (v1.get("tasks_executed_normal", 0) -
                  v0.get("tasks_executed_normal", 0) +
                  v1.get("tasks_executed_priority", 0) -
                  v0.get("tasks_executed_priority", 0))
            key = "tasks/op " + ln
            r[key] = r.get(key, 0) + ex / nops
    samp = {}
    for line in lines(os.path.join(d, row + ".tq.txt")):
        p = line.split()
        if len(p) != 10 or tm.get(p[1]) != pool:
            continue
        ln = lane(p[1])
        e = samp.setdefault(ln, {}).setdefault(p[0], [0, 0])
        e[0] += int(p[3])
        e[1] += int(p[7]) + int(p[8])
    v = list(samp.get("iss", {}).values())
    if v:
        r["iss act mean"] = statistics.mean(x[0] for x in v)
        r["iss act max"] = max(x[0] for x in v)
        r["iss queued mean"] = statistics.mean(x[1] for x in v)
    cols = {"total_wait w us": 8, "disk_wait w us": 10,
            "syncq_wait w us": 12, "syncq_write activ": 21,
            "syncq_write pend": 20}
    acc = {k: [] for k in cols}
    for line in lines(os.path.join(d, row + ".iostat.txt")):
        p = line.split("\t")
        if len(p) != 32 or p[0] != pool:
            continue
        for k, i in cols.items():
            try:
                x = float(p[i])
            except ValueError:
                continue
            acc[k].append(x / 1000 if k.endswith(" us") else x)
    for k, x in acc.items():
        if x:
            r["vq " + k] = statistics.mean(x)
    lo, hi = s0["mono_raw_ns"], s1["mono_raw_ns"]
    tx = [x for x in s1.get("txgs", []) if lo <= x[1] <= hi and
          x[2] == "C"]
    if tx:
        r["txgs"] = len(tx)
        r["txg sync ms"] = statistics.mean(x[11] for x in tx) / 1e6
    ser = iops_series(d, row)
    if ser:
        r["1s IOPS min"] = min(ser)
        r["1s IOPS max"] = max(ser)
    bulk = load(os.path.join(d, row + ".bulk.json"))
    if bulk and bulk.get("jobs"):
        r["bulk MiB/s"] = bulk["jobs"][0]["write"]["bw"] / 1024.0
    return r


def mw_p(a, b):
    """Exact two-sided rank permutation test of two small samples."""
    if len(a) < 2 or len(b) < 2 or len(a) + len(b) > 18:
        return None
    pooled = sorted(a + b)
    rank = {}
    i = 0
    while i < len(pooled):
        j = i
        while j < len(pooled) and pooled[j] == pooled[i]:
            j += 1
        rank[pooled[i]] = (i + j + 1) / 2.0
        i = j
    ranks = [rank[x] for x in a + b]
    n = len(a)
    mean = n * (len(ranks) + 1) / 2.0
    obs = abs(sum(rank[x] for x in a) - mean)
    hit = tot = 0
    for c in itertools.combinations(range(len(ranks)), n):
        tot += 1
        if abs(sum(ranks[k] for k in c) - mean) >= obs - 1e-9:
            hit += 1
    return hit / tot


def perm_p(logs):
    """Exact two-sided sign-flip permutation test of paired log ratios."""
    n = len(logs)
    if n < 2:
        return None
    obs = abs(sum(logs))
    hit = 0
    for signs in itertools.product((1, -1), repeat=n):
        if abs(sum(s * x for s, x in zip(signs, logs))) >= obs - 1e-12:
            hit += 1
    return hit / 2 ** n


def sign_p(won, n):
    """Exact two-sided sign test."""
    if n == 0:
        return None
    k = min(won, n - won)
    p = 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, p)


def fmt(v):
    if v is None:
        return "-"
    if abs(v) >= 100:
        return "%.0f" % v
    if abs(v) >= 10:
        return "%.1f" % v
    return "%.2f" % v


def mmm(vals):
    if not vals:
        return "-"
    return "%s [%s-%s]" % (fmt(statistics.mean(vals)), fmt(min(vals)),
                           fmt(max(vals)))


HOP_LINE = re.compile(r"^@(\w+)(?:\[(.*)\])?:\s*(.*)$")
HOP_BUCKET = re.compile(r"^[\[(]([0-9]+|\.\.\.), ([0-9]+|\.\.\.)\)\s+([0-9]+)")
HOP_STATS = re.compile(r"count (\d+), average (-?\d+), total (-?\d+)")


def hop(path):
    """The maps v6p-hop.bt printed at exit, or None without the file."""
    text = lines(path)
    if not text:
        return None
    out = {"count": {}, "stats": {}, "hist": {}}
    cur = None
    for line in text:
        m = HOP_LINE.match(line)
        if m:
            name, key, rest = m.groups()
            key = key or ""
            cur = None
            s = HOP_STATS.match(rest)
            if s:
                out["stats"][(name, key)] = [int(x) for x in s.groups()]
            elif rest.strip().isdigit():
                out["count"][(name, key)] = int(rest)
            elif not rest.strip():
                cur = out["hist"].setdefault((name, key), [])
            continue
        b = HOP_BUCKET.match(line.strip())
        if cur is not None and b:
            lo = 0 if b.group(1) == "..." else int(b.group(1))
            hi = None if b.group(2) == "..." else int(b.group(2))
            cur.append((lo, hi, int(b.group(3))))
    return out


def hist_q(buckets, q):
    """Upper bound (us) of the bucket holding quantile q, as text."""
    tot = sum(c for _, _, c in buckets)
    if not tot:
        return "-"
    acc = 0
    for lo, hi, c in buckets:
        acc += c
        if acc >= q * tot:
            return "<%d" % hi if hi is not None else ">=%d" % lo
    return "-"


def hop_cells(h, ops):
    """One row of the traced-pass table from the parsed maps."""
    w = h["count"].get(("writes", ""), 0)
    cells = [fmt(ops), str(w)]
    for c in ("z_wr_iss", "fio"):
        n = h["count"].get(("wakeups", c), 0)
        st = h["stats"].get(("wake_ns", c))
        idle = h["count"].get(("from_idle", c), 0)
        hb = h["hist"].get(("wake_us", c), [])
        cells.append(fmt(n / w) if w else "-")
        cells.append(fmt(st[1] / 1000.0) if st else "-")
        cells += [hist_q(hb, 0.5), hist_q(hb, 0.9), hist_q(hb, 0.99)]
        cells.append(fmt(100.0 * idle / st[0]) if st and st[0] else "-")
        cells.append(fmt(st[2] / 1000.0 / w) if st and w else "-")
    return cells


def verdict_rows(data, key):
    """Per row: (row, ratios, geo, won, p perm) of v6/master for key."""
    out = []
    for row in ROWS:
        m = data.get(row, {}).get("master", {})
        v = data.get(row, {}).get("v6", {})
        both = sorted(p for p in set(m) & set(v)
                      if m[p].get(key) and v[p].get(key))
        if not both:
            continue
        rat = [v[p][key] / m[p][key] for p in both]
        logs = [math.log(x) for x in rat]
        geo = math.exp(statistics.mean(logs))
        won = sum(1 for x in rat if x > 1)
        out.append((row, both, rat, geo, won, perm_p(logs)))
    return out


def main():
    data = {}   # data[row][arm][pass] = r
    for d in sorted(glob.glob(os.path.join(RAW, "*", "*"))):
        arm = os.path.basename(d)
        p = os.path.basename(os.path.dirname(d))
        if not p.startswith(PREFIX) or arm not in ARMS:
            continue
        for row in ROWS:
            r = one(d, row)
            if r is not None:
                r["_hop"] = hop(os.path.join(d, row + ".hop.txt"))
                data.setdefault(row, {}).setdefault(arm, {})[p] = r

    print("Passes read: %s*\n" % PREFIX)
    print("## Verdict per row\n")
    print("| row | n pairs | master IOPS | v6 IOPS | geo ratio | median "
          "ratio | ratios [min-max] | won | p perm | p sign | p MW | "
          "verdict | V5P v5 geo |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for row, both, rat, geo, won, pp in verdict_rows(data, "IOPS"):
        m = data[row]["master"]
        v = data[row]["v6"]
        ps = sign_p(won, len(rat))
        pm = mw_p([r["IOPS"] for r in m.values()],
                  [r["IOPS"] for r in v.values()])
        verdict = "no difference shown"
        if pp is not None and pp <= 0.05:
            if geo >= 1.03:
                verdict = "gain"
            elif geo <= 0.97:
                verdict = "loss"
        print("| %s | %d | %s | %s | %.3f | %.3f | %.2f-%.2f | %d/%d | "
              "%s | %s | %s | %s | %.3f |" % (
                  row, len(rat), mmm([r["IOPS"] for r in m.values()]),
                  mmm([r["IOPS"] for r in v.values()]), geo,
                  statistics.median(rat), min(rat), max(rat), won, len(rat),
                  fmt(pp), fmt(ps), fmt(pm), verdict, V5P_GEO[row]))
    print()

    print("## Tail rule, fsync rows (v6 / master fsync p99.9)\n")
    print("| row | n pairs | ratios [min-max] | geo | worse in | p perm | "
          "verdict |")
    print("|---|---|---|---|---|---|---|")
    for row, both, rat, geo, won, pp in verdict_rows(data, "fsync p99.9"):
        if row not in FSYNC_ROWS:
            continue
        verdict = "no tail difference shown"
        if pp is not None and pp <= 0.05:
            if geo >= 1.20:
                verdict = "tail cost"
            elif geo <= 0.83:
                verdict = "tail gain"
        print("| %s | %d | %.2f-%.2f | %.3f | %d/%d | %s | %s |" % (
            row, len(rat), min(rat), max(rat), geo, won, len(rat),
            fmt(pp), verdict))
    print()

    print("## Latency, CPU and noise (mean [min-max] over passes)\n")
    for row in ROWS:
        if row not in data:
            continue
        keys = ["wr p50", "wr p99", "wr p99.9"]
        if row in FSYNC_ROWS:
            keys += ["fsync p50", "fsync p99", "fsync p99.9"]
        keys += ["cpu busy %", "busy CPU us/op", "steal %", "load1 end"]
        if row == "x1":
            keys.append("bulk MiB/s")
        print("### %s\n" % row)
        print("| arm | n | " + " | ".join(keys) + " |")
        print("|---|---|" + "---|" * len(keys))
        for a in ARMS:
            runs = list(data[row].get(a, {}).values())
            if not runs:
                continue
            cells = [mmm([r[k] for r in runs if k in r]) for k in keys]
            print("| %s | %d | %s |" % (a, len(runs), " | ".join(cells)))
        print()

    print("## Diagnostics (means over passes)\n")
    diag = ["tasks/op iss", "tasks/op int", "tasks/op flush", "tasks/op zvol",
            "iss act mean", "iss act max", "iss queued mean",
            "commits/op", "lwbs/op", "KiB/lwb", "flushes/op", "flush ms",
            "vq syncq_write activ", "vq syncq_write pend",
            "vq syncq_wait w us", "vq disk_wait w us", "vq total_wait w us",
            "txgs", "txg sync ms", "1s IOPS min", "1s IOPS max",
            "disk MB", "test disk MB"]
    diag += ["cpu " + c for c in CLASSES] + ["rq ms " + c for c in CLASSES]
    for row in ROWS:
        if row not in data:
            continue
        print("### %s\n" % row)
        print("| metric | master | v6 |")
        print("|---|---|---|")
        for k in diag:
            vals = []
            for a in ARMS:
                x = [r[k] for r in data[row].get(a, {}).values() if k in r]
                vals.append(fmt(statistics.mean(x)) if x else "-")
            if any(x != "-" for x in vals):
                print("| %s | %s |" % (k, " | ".join(vals)))
        print()

    print("## IOPS per pass: master / v6 (steal %, load1 at the end)\n")
    for row in ROWS:
        if row not in data:
            continue
        passes = sorted({p for a in ARMS for p in data[row].get(a, {})})
        print("### %s\n" % row)
        print("| pass | master | v6 | ratio |")
        print("|---|---|---|---|")
        for p in passes:
            cells = []
            for a in ARMS:
                r = data[row].get(a, {}).get(p)
                cells.append("%s (%s, %s)" % (
                    fmt(r["IOPS"]), fmt(r.get("steal %")),
                    fmt(r.get("load1 end"))) if r else "-")
            m = data[row].get("master", {}).get(p)
            v = data[row].get("v6", {}).get(p)
            ratio = "%.3f" % (v["IOPS"] / m["IOPS"]) if m and v and \
                m["IOPS"] else "-"
            print("| %s | %s | %s |" % (p, " | ".join(cells), ratio))
        print()

    traced = [(row, a, p, r) for row in ROWS for a in ARMS
              for p, r in sorted(data.get(row, {}).get(a, {}).items())
              if r.get("_hop")]
    if not traced:
        return
    print("## Scheduler hops (v6p-hop.bt; rows traced, so their IOPS are "
          "not comparable with untraced passes)\n")
    print("Per thread class: wakeups per write, mean time from "
          "sched_waking to running (us), its p50/p90/p99 (upper bound of "
          "the 10 us bucket), the share of those switches made from the "
          "idle task, and that waiting time summed per write (us).\n")
    head = ["row", "arm", "pass", "IOPS", "writes"]
    for c in ("iss", "fio"):
        head += [c + " wake/write", c + " mean us", c + " p50", c + " p90",
                 c + " p99", c + " from idle %", c + " us/write"]
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for row, a, p, r in traced:
        print("| %s | %s | %s | %s |" % (
            row, a, p, " | ".join(hop_cells(r["_hop"], r.get("IOPS")))))
    print()


main()
