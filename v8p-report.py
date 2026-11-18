#!/usr/bin/env python3
# TASK-1118-V8P: summarize the raw results of v8p-bench.sh as Markdown.
# Usage: v8p-report.py /var/log/task-1118-v8p/raw [PREFIX] > out.md
# Only the passes whose name starts with PREFIX (default P) are read.
# It is TASK-1118-V7P's report with the arms master, v6 and v8, the
# pairs v6/master, v8/master and v8/v6, V7P's v6/master ratios beside
# this run's, and the decision rules of TASK-1118-V8P; the M1 table is
# gone.
#
# Per row and pair a/b, over the passes where both arms ran it: ratio =
# a IOPS / b IOPS in the same pass; geo = geometric mean of the ratios;
# won = passes where a was higher; p perm = exact two-sided sign-flip
# permutation test of the log ratios; p sign = exact two-sided sign test.
# Verdict: "gain" if geo >= 1.03 and p perm <= 0.05, "loss" if geo <=
# 0.97 and p perm <= 0.05, otherwise "no difference shown".  Tail rule on
# the fsync rows (r1, x1), with t = a fsync p99.9 / b fsync p99.9 per
# pass: "tail cost" if geo(t) >= 1.20 and p perm <= 0.05, "tail gain" if
# geo(t) <= 0.83 and p perm <= 0.05, otherwise "no tail difference shown".
#
# ZIL diagnostics, per write over the snapshot window (nops = IOPS times
# the seconds between the two snapshots, as in V6P): "zil_commit/op" from
# zil_commit_count; "writer passes/op" from zil_commit_writer_count (V6P
# called it "commits/op"); "fast path %" = share of zil_commit() calls
# that found their commit already done by another thread's writer pass,
# 100 * (1 - zil_commit_writer_count / zil_commit_count); "itxs/op" from
# zil_itx_count (about 1 for these rows, so a check of nops); "lwbs/op"
# and "KiB/lwb" from the normal and slog metaslab counters together.
#
# "Significantly below" for a pair a/b: geo < 1 and p perm <= 0.05,
# whatever the size of the drop.
#
# Decision rules for v8, fixed before the run; means are over the passes:
#  (1) n32: v8/master not significantly below, and v8/v6 "gain".
#  (2) n8, n8e, r1, w4 and w8, each: v8/v6 not significantly below.
#  (3) r1 and x1: v8/master and v8/v6 not "tail cost"; x1: v8/master
#      not "loss".
#  (4) every row: v8's busy CPU us/op <= 1.03 * max(master's, v6's).
# A rule that lacks its data is "not evaluated".  v8 is recommended only
# if it meets every rule.  The n32 mechanism line (v8's lwbs/op and
# KiB/lwb against master's and v6's) is printed beside the rules and
# decides nothing.
import glob
import itertools
import json
import math
import os
import statistics
import sys

RAW = sys.argv[1]
PREFIX = sys.argv[2] if len(sys.argv) > 2 else "P"
ROWS = ["n8", "n8e", "r1", "x1", "n32", "w4", "w8"]
ARMS = ["master", "v6", "v8"]
VARIANTS = ["v8"]
PAIRS = [("v6", "master"), ("v8", "master"), ("v8", "v6")]
ROW_POOL = {"x1": "c1q", "r1": "c1r", "n8": "c1n", "n32": "c1n",
            "n8e": "c1n", "w4": "c1n", "w8": "c1n"}
FSYNC_ROWS = ("x1", "r1")
# TASK-1118-V7P, geo ratio v6 / master over all its passes (7, 9 on
# null_blk).
V7P_GEO = {"x1": 1.020, "r1": 1.133, "n8": 1.143, "n8e": 1.121,
           "n32": 0.966, "w4": 1.221, "w8": 1.151}
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
        cm = dz.get("zil_commit_count", 0)
        wr = dz.get("zil_commit_writer_count", 0)
        r["zil_commit/op"] = cm / nops
        r["writer passes/op"] = wr / nops
        if cm:
            r["fast path %"] = 100.0 * (1 - wr / cm)
        r["itxs/op"] = dz.get("zil_itx_count", 0) / nops
        lw = dz.get("zil_itx_metaslab_normal_count", 0) + \
            dz.get("zil_itx_metaslab_slog_count", 0)
        r["lwbs/op"] = lw / nops
        if lw:
            r["KiB/lwb"] = (dz.get("zil_itx_metaslab_normal_write", 0) +
                            dz.get("zil_itx_metaslab_slog_write", 0)) / \
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


def verdict(geo, pp, up=1.03, down=0.97, words=("gain", "loss",
            "no difference shown")):
    if pp is not None and pp <= 0.05:
        if geo >= up:
            return words[0]
        if geo <= down:
            return words[1]
    return words[2]


def tail_verdict(geo, pp):
    return verdict(geo, pp, 1.20, 0.83, ("tail cost", "tail gain",
                                         "no tail difference shown"))


def pair_stats(data, row, a, b, key):
    """(passes, ratios, geo, won, p perm) of a/b on key, or None."""
    x = data.get(row, {}).get(a, {})
    y = data.get(row, {}).get(b, {})
    both = sorted(p for p in set(x) & set(y)
                  if x[p].get(key) and y[p].get(key))
    if not both:
        return None
    rat = [x[p][key] / y[p][key] for p in both]
    logs = [math.log(v) for v in rat]
    geo = math.exp(statistics.mean(logs))
    won = sum(1 for v in rat if v > 1)
    return both, rat, geo, won, perm_p(logs)


def mean_of(data, row, arm, key):
    x = [r[key] for r in data.get(row, {}).get(arm, {}).values()
         if key in r]
    return statistics.mean(x) if x else None


def within(a, b, tol):
    """Text and truth of |a / b - 1| <= tol, or None without data."""
    if a is None or not b:
        return None
    d = a / b - 1
    return d, abs(d) <= tol


def read_data(raw, prefix):
    data = {}   # data[row][arm][pass] = r
    orders = {}     # orders[pass] = [arm, ...]
    for d in sorted(glob.glob(os.path.join(raw, "*", "*"))):
        arm = os.path.basename(d)
        p = os.path.basename(os.path.dirname(d))
        if not p.startswith(prefix) or arm not in ARMS:
            continue
        if p not in orders:
            o = lines(os.path.join(raw, p, "order.txt"))
            orders[p] = o[0].split() if o else []
        for row in ROWS:
            r = one(d, row)
            if r is not None:
                r["_layout"] = lines(os.path.join(d, "layout.txt"))
                data.setdefault(row, {}).setdefault(arm, {})[p] = r
    return data, orders


def below(st):
    """True if a/b is significantly below 1, None without data."""
    if st is None or st[4] is None:
        return None
    return st[2] < 1 and st[4] <= 0.05


def rule_lines(data, v):
    """[(rule, row, figures, result)] for variant v."""
    out = []

    def pv(row, a, b):
        st = pair_stats(data, row, a, b, "IOPS")
        if st is None:
            return None, "%s/%s -" % (a, b)
        both, rat, geo, won, pp = st
        sb = below(st)
        return st, "%s/%s %.3f %d/%d p %s %s%s" % (
            a, b, geo, won, len(rat), fmt(pp), verdict(geo, pp),
            ", significantly below" if sb else "")

    def res(ok_list):
        if any(x is None for x in ok_list):
            return "not evaluated"
        return "met" if all(ok_list) else "FAILED"

    def notbelow(st):
        b = below(st)
        return None if b is None else not b

    # (1) n32
    row = "n32"
    sm, t1 = pv(row, v, "master")
    s6, t2 = pv(row, v, "v6")
    out.append(("1", row, "%s; %s" % (t1, t2), res([
        notbelow(sm),
        None if s6 is None else verdict(s6[2], s6[4]) == "gain"])))
    # (2) the rows where v6 gained
    for row in ("n8", "n8e", "r1", "w4", "w8"):
        s6, t1 = pv(row, v, "v6")
        sm, t2 = pv(row, v, "master")
        out.append(("2", row, "%s; %s" % (t1, t2), res([notbelow(s6)])))
    # (3) tails on r1 and x1, and x1's rate
    for row in FSYNC_ROWS:
        figs, oks = [], []
        if row == "x1":
            sm, t1 = pv(row, v, "master")
            figs.append(t1)
            oks.append(None if sm is None else
                       verdict(sm[2], sm[4]) != "loss")
        for b in ("master", "v6"):
            st = pair_stats(data, row, v, b, "fsync p99.9")
            if st is None:
                figs.append("tail %s/%s -" % (v, b))
                oks.append(None)
                continue
            tv = tail_verdict(st[2], st[4])
            figs.append("tail %s/%s %.3f %d/%d p %s %s" % (
                v, b, st[2], st[3], len(st[1]), fmt(st[4]), tv))
            oks.append(tv != "tail cost")
        out.append(("3", row, "; ".join(figs), res(oks)))
    # (4) every row
    for row in ROWS:
        if row not in data:
            continue
        c = mean_of(data, row, v, "busy CPU us/op")
        ref = [mean_of(data, row, a, "busy CPU us/op")
               for a in ("master", "v6")]
        ref = [x for x in ref if x is not None]
        if c is None or not ref:
            out.append(("4", row, "busy CPU us/op -", "not evaluated"))
            continue
        lim = 1.03 * max(ref)
        out.append(("4", row, "busy CPU us/op %s, limit %s (%+.1f %% of "
                    "max(master, v6))" % (fmt(c), fmt(lim),
                                          100 * (c / max(ref) - 1)),
                    "met" if c <= lim else "FAILED"))
    return out


def mechanism(data, v):
    """The n32 line: v's lwbs/op and KiB/lwb against master's and v6's."""
    parts = []
    for k in ("lwbs/op", "KiB/lwb", "writer passes/op", "fast path %"):
        x, m, s6 = (mean_of(data, "n32", a, k) for a in (v, "master", "v6"))
        if x is None or m is None or s6 is None:
            parts.append("%s -" % k)
            continue
        near = "master" if abs(x - m) < abs(x - s6) else "v6"
        parts.append("%s %s (master %s, v6 %s: nearer %s)" % (
            k, fmt(x), fmt(m), fmt(s6), near))
    return "; ".join(parts)


def main():
    data, orders = read_data(RAW, PREFIX)
    print("Passes read: %s*\n" % PREFIX)

    print("## Verdict per row and pair\n")
    print("| row | pair a/b | n pairs | a IOPS | b IOPS | geo ratio | "
          "median ratio | ratios [min-max] | won | p perm | p sign | p MW | "
          "verdict | V7P v6/master geo |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for row in ROWS:
        for a, b in PAIRS:
            st = pair_stats(data, row, a, b, "IOPS")
            if st is None:
                continue
            both, rat, geo, won, pp = st
            x = [data[row][a][p]["IOPS"] for p in both]
            y = [data[row][b][p]["IOPS"] for p in both]
            ref = "-"
            if (a, b) == ("v6", "master") and row in V7P_GEO:
                ref = "%.3f" % V7P_GEO[row]
            print("| %s | %s/%s | %d | %s | %s | %.3f | %.3f | %.2f-%.2f | "
                  "%d/%d | %s | %s | %s | %s | %s |" % (
                      row, a, b, len(rat), mmm(x), mmm(y), geo,
                      statistics.median(rat), min(rat), max(rat), won,
                      len(rat), fmt(pp), fmt(sign_p(won, len(rat))),
                      fmt(mw_p(x, y)), verdict(geo, pp), ref))
    print()

    print("## Tail rule, fsync rows (a / b fsync p99.9)\n")
    print("| row | pair a/b | n pairs | ratios [min-max] | geo | worse in | "
          "p perm | verdict |")
    print("|---|---|---|---|---|---|---|---|")
    for row in FSYNC_ROWS:
        for a, b in PAIRS:
            st = pair_stats(data, row, a, b, "fsync p99.9")
            if st is None:
                continue
            both, rat, geo, won, pp = st
            print("| %s | %s/%s | %d | %.2f-%.2f | %.3f | %d/%d | %s | "
                  "%s |" % (row, a, b, len(rat), min(rat), max(rat), geo,
                            won, len(rat), fmt(pp), tail_verdict(geo, pp)))
    print()

    print("## Decision rules (fixed before the run)\n")
    for v in VARIANTS:
        rl = rule_lines(data, v)
        bad = sorted({x[0] for x in rl if x[3] != "met"})
        print("### %s\n" % v)
        print("| rule | row | figures | result |")
        print("|---|---|---|---|")
        for x in rl:
            print("| (%s) | %s | %s | %s |" % x)
        print()
        print("n32 mechanism (decides nothing): %s\n" % mechanism(data, v))
        if bad:
            print("**%s is not recommended**: rule(s) %s not met.\n" % (
                v, ", ".join("(%s)" % x for x in bad)))
        else:
            print("**%s meets every rule.**\n" % v)

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
            "zil_commit/op", "writer passes/op", "fast path %", "itxs/op",
            "lwbs/op", "KiB/lwb", "flushes/op", "flush ms",
            "vq syncq_write activ", "vq syncq_write pend",
            "vq syncq_wait w us", "vq disk_wait w us", "vq total_wait w us",
            "txgs", "txg sync ms", "1s IOPS min", "1s IOPS max",
            "disk MB", "test disk MB"]
    diag += ["cpu " + c for c in CLASSES] + ["rq ms " + c for c in CLASSES]
    for row in ROWS:
        if row not in data:
            continue
        print("### %s\n" % row)
        print("| metric | " + " | ".join(ARMS) + " |")
        print("|---|" + "---|" * len(ARMS))
        for k in diag:
            vals = [fmt(mean_of(data, row, a, k)) for a in ARMS]
            if any(x != "-" for x in vals):
                print("| %s | %s |" % (k, " | ".join(vals)))
        print()

    print("## Arm positions per row (passes in position 1, 2, 3)\n")
    print("| row | " + " | ".join(ARMS) + " |")
    print("|---|" + "---|" * len(ARMS))
    for row in ROWS:
        if row not in data:
            continue
        cells = []
        for a in ARMS:
            n = [0] * len(ARMS)
            for p in data[row].get(a, {}):
                o = orders.get(p, [])
                if a in o and o.index(a) < len(ARMS):
                    n[o.index(a)] += 1
            cells.append(" ".join(str(x) for x in n))
        print("| %s | %s |" % (row, " | ".join(cells)))
    print()

    print("## IOPS per pass: #position (steal %, load1 at the end)\n")
    for row in ROWS:
        if row not in data:
            continue
        passes = sorted({p for a in ARMS for p in data[row].get(a, {})},
                        key=lambda p: (len(p), p))
        print("### %s\n" % row)
        print("| pass | " + " | ".join(ARMS) +
              " | v6/m | v8/m | v8/v6 |")
        print("|---|" + "---|" * (len(ARMS) + len(PAIRS)))
        for p in passes:
            o = orders.get(p, [])
            cells = []
            for a in ARMS:
                r = data[row].get(a, {}).get(p)
                cells.append("%s #%s (%s, %s)" % (
                    fmt(r["IOPS"]), o.index(a) + 1 if a in o else "?",
                    fmt(r.get("steal %")), fmt(r.get("load1 end")))
                    if r else "-")
            rats = []
            for a, b in PAIRS:
                x = data[row].get(a, {}).get(p)
                y = data[row].get(b, {}).get(p)
                rats.append("%.3f" % (x["IOPS"] / y["IOPS"])
                            if x and y and y["IOPS"] else "-")
            print("| %s | %s | %s |" % (p, " | ".join(cells),
                                        " | ".join(rats)))
        print()


main()
