#!/usr/bin/env python3
# TASK-1118-V7P: summarize the raw results of v7p-bench.sh as Markdown.
# Usage: v7p-report.py /var/log/task-1118-v7p/raw [PREFIX] > out.md
# Only the passes whose name starts with PREFIX (default P) are read.
# With PREFIX M, the M1 table follows: n32 on v6 per pass with the
# zfs_commit_timeout_pct its layout.txt recorded, beside the means of
# master and v6 over the P passes.  It is TASK-1118-V6P's report with
# four arms, the rows w4 and w8, a verdict per pair of arms, ZIL
# diagnostics, each arm's position in each pass (raw/PASS/order.txt),
# and the decision rules of TASK-1118-V7P.
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
# Decision rules, fixed before the run, for v7a and v7b ("v7"); means are
# over the passes:
#  (1) n32: v7/v6 "gain", v7/master not "loss", and v7's lwbs/op and
#      KiB/lwb each within +-5 % of master's.
#  (2) n8, n8e and r1, each: v7/master "gain", v7/v6 not "loss", and v7's
#      tasks/op iss within +-10 % of v6's.
#  (3) x1: v7/master not "loss", and v7/master not "tail cost".
#  (4) every row: v7's busy CPU us/op <= 1.03 * max(master's, v6's).
# A rule that lacks its data is "not evaluated".  A variant is
# recommended only if it meets every rule.
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
ROWS = ["n8", "n8e", "r1", "x1", "n32", "w4", "w8"]
ARMS = ["master", "v6", "v7a", "v7b"]
VARIANTS = ["v7a", "v7b"]
PAIRS = [("v6", "master"), ("v7a", "master"), ("v7b", "master"),
         ("v7a", "v6"), ("v7b", "v6"), ("v7b", "v7a")]
ROW_POOL = {"x1": "c1q", "r1": "c1r", "n8": "c1n", "n32": "c1n",
            "n8e": "c1n", "w4": "c1n", "w8": "c1n"}
FSYNC_ROWS = ("x1", "r1")
# TASK-1118-V6P, geo ratio v6 / master over its passes (6, 8 on null_blk).
V6P_GEO = {"x1": 1.099, "r1": 1.155, "n8": 1.165, "n8e": 1.122,
           "n32": 0.944}
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


def rule_lines(data, v):
    """[(rule, row, figures, result)] for variant v."""
    out = []

    def pv(row, a, b):
        st = pair_stats(data, row, a, b, "IOPS")
        if st is None:
            return None, "-"
        both, rat, geo, won, pp = st
        vd = verdict(geo, pp)
        return vd, "%s/%s %.3f %d/%d p %s %s" % (a, b, geo, won, len(rat),
                                                 fmt(pp), vd)

    def res(ok_list):
        if any(x is None for x in ok_list):
            return "not evaluated"
        return "met" if all(ok_list) else "FAILED"

    # (1) n32
    row = "n32"
    v6v, t1 = pv(row, v, "v6")
    vm, t2 = pv(row, v, "master")
    lw = within(mean_of(data, row, v, "lwbs/op"),
                mean_of(data, row, "master", "lwbs/op"), 0.05)
    kb = within(mean_of(data, row, v, "KiB/lwb"),
                mean_of(data, row, "master", "KiB/lwb"), 0.05)
    fig = "%s; %s; lwbs/op %s; KiB/lwb %s vs master" % (
        t1, t2, "%+.1f %%" % (100 * lw[0]) if lw else "-",
        "%+.1f %%" % (100 * kb[0]) if kb else "-")
    out.append(("1", row, fig, res([
        None if v6v is None else v6v == "gain",
        None if vm is None else vm != "loss",
        None if lw is None else lw[1], None if kb is None else kb[1]])))
    # (2) n8, n8e, r1
    for row in ("n8", "n8e", "r1"):
        vm, t1 = pv(row, v, "master")
        v6v, t2 = pv(row, v, "v6")
        it = within(mean_of(data, row, v, "tasks/op iss"),
                    mean_of(data, row, "v6", "tasks/op iss"), 0.10)
        fig = "%s; %s; tasks/op iss %s vs v6" % (
            t1, t2, "%+.1f %%" % (100 * it[0]) if it else "-")
        out.append(("2", row, fig, res([
            None if vm is None else vm == "gain",
            None if v6v is None else v6v != "loss",
            None if it is None else it[1]])))
    # (3) x1
    row = "x1"
    vm, t1 = pv(row, v, "master")
    st = pair_stats(data, row, v, "master", "fsync p99.9")
    if st is None:
        tv, t2 = None, "tail -"
    else:
        tv = tail_verdict(st[2], st[4])
        t2 = "tail %.3f %d/%d p %s %s" % (st[2], st[3], len(st[1]),
                                          fmt(st[4]), tv)
    out.append(("3", row, "%s; %s" % (t1, t2), res([
        None if vm is None else vm != "loss",
        None if tv is None else tv != "tail cost"])))
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


def layout_value(r, name):
    for line in r.get("_layout", []):
        m = re.match(r"^%s=(\S+)$" % re.escape(name), line)
        if m:
            return m.group(1)
    return None


def m1_table(data):
    ref, _ = read_data(RAW, "P")
    keys = ["IOPS", "wr p99", "lwbs/op", "KiB/lwb", "zil_commit/op",
            "writer passes/op", "fast path %", "busy CPU us/op"]
    print("## M1: n32 on v6 by zfs_commit_timeout_pct\n")
    print("| pass | arm | pct | " + " | ".join(keys) +
          " | IOPS / P master | IOPS / P v6 |")
    print("|---|---|---|" + "---|" * (len(keys) + 2))
    pm = mean_of(ref, "n32", "master", "IOPS")
    p6 = mean_of(ref, "n32", "v6", "IOPS")
    for a in ("master", "v6"):
        cells = [fmt(mean_of(ref, "n32", a, k)) for k in keys]
        print("| P mean | %s | 10 | %s | - | - |" % (a, " | ".join(cells)))
    for a in ARMS:
        for p, r in sorted(data.get("n32", {}).get(a, {}).items()):
            cells = [fmt(r.get(k)) for k in keys]
            print("| %s | %s | %s | %s | %s | %s |" % (
                p, a, layout_value(r, "zfs_commit_timeout_pct") or "-",
                " | ".join(cells),
                "%.3f" % (r["IOPS"] / pm) if pm else "-",
                "%.3f" % (r["IOPS"] / p6) if p6 else "-"))
    print()


def main():
    data, orders = read_data(RAW, PREFIX)
    print("Passes read: %s*\n" % PREFIX)
    if PREFIX.startswith("M"):
        m1_table(data)

    print("## Verdict per row and pair\n")
    print("| row | pair a/b | n pairs | a IOPS | b IOPS | geo ratio | "
          "median ratio | ratios [min-max] | won | p perm | p sign | p MW | "
          "verdict | V6P v6/master geo |")
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
            if (a, b) == ("v6", "master") and row in V6P_GEO:
                ref = "%.3f" % V6P_GEO[row]
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

    if not PREFIX.startswith("M"):
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

    print("## Arm positions per row (passes in position 1, 2, 3, 4)\n")
    print("| row | " + " | ".join(ARMS) + " |")
    print("|---|" + "---|" * len(ARMS))
    for row in ROWS:
        if row not in data:
            continue
        cells = []
        for a in ARMS:
            n = [0, 0, 0, 0]
            for p in data[row].get(a, {}):
                o = orders.get(p, [])
                if a in o and o.index(a) < 4:
                    n[o.index(a)] += 1
            cells.append(" ".join(str(x) for x in n))
        print("| %s | %s |" % (row, " | ".join(cells)))
    print()

    print("## IOPS per pass: #position (steal %, load1 at the end)\n")
    for row in ROWS:
        if row not in data:
            continue
        passes = sorted({p for a in ARMS for p in data[row].get(a, {})})
        print("### %s\n" % row)
        print("| pass | " + " | ".join(ARMS) +
              " | v6/m | v7a/m | v7b/m | v7a/v6 | v7b/v6 |")
        print("|---|" + "---|" * (len(ARMS) + 5))
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
            for a, b in PAIRS[:5]:
                x = data[row].get(a, {}).get(p)
                y = data[row].get(b, {}).get(p)
                rats.append("%.3f" % (x["IOPS"] / y["IOPS"])
                            if x and y and y["IOPS"] else "-")
            print("| %s | %s | %s |" % (p, " | ".join(cells),
                                        " | ".join(rats)))
        print()


main()
