#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1121-WBP2V2 report.

  wbp2-report.py tracesum < trace > trace-sum.txt
      sums block_rq_insert events by inserting task, kind and ioprio:
      "Q <comm> <kind> <class> <requests> <bytes>", then
      "E <entries in buffer> <entries written> <lines not parsed>"
  wbp2-report.py brief RUNDIR          one line for driver.log
  wbp2-report.py flags RAW/F           the check before GO, Markdown
  wbp2-report.py RAWDIR PREFIX         summary of the passes PREFIX*

Groups of inserting tasks: vic (wbp2-vr, wbp2-vw), agg (wbp2-ar, wbp2-aw),
zfs (z_* and txg_* threads), kworker, other.  Classes: none, beN, idleN,
rtN (ioprio class and level).

Verdicts (passes where both arms ran the scenario): ratio a / b in the same
pass; "lower" if the geometric mean of the ratios is at most 0.97 and the
exact two-sided sign-flip permutation p of the log ratios is at most 0.05,
"higher" if at least 1.03 with the same p, else "no difference shown".
An expectation "not higher" holds for "lower" and "no difference shown".

Arms: M (upstream master), S1 and S3 (PR2 v2 with zfs_vdev_disk_rt_mask 1
and 3).  The class checks are the facts the code fixes: no real-time request
on M; none from an agg thread under "ionice -c3"; none written on S1; on S1
and S3 most of vic's reads real-time; on S3 some real-time writes.
"""

import itertools
import math
import os
import re
import sys

TRACE_RE = re.compile(
    r"^\s*(.*?)-(\d+)\s+\[\d+\]\s+(?:\S+\s+)?(\d+\.\d+):\s+block_rq_insert:"
    r"\s+(\d+),(\d+)\s+(\S+)\s+(\d+)\s+\(.*?\)\s+(\d+)\s+\+\s+(\d+)\s+"
    r"(\w+),(\d+),(\d+)\s+\[(.*)\]\s*$")
ENTRIES_RE = re.compile(r"entries-in-buffer/entries-written:\s*(\d+)/(\d+)")
SCEN = ["a", "b", "c", "d", "e"]
ARMS = ["M", "S1", "S3"]
PAIRS = [("S1", "M"), ("S3", "M"), ("S3", "S1")]
GROUPS = ["vic", "agg", "zfs", "kworker", "other"]
METRICS = [
    ("vr_p50", "vic read p50 ms"),
    ("vr_p99", "vic read p99 ms"),
    ("vw_fs_mean", "vic fsync mean ms"),
    ("vw_fs_p90", "vic fsync p90 ms"),
    ("ar_mbs", "agg read MB/s"),
    ("aw_ops", "agg write ops/s"),
    ("aw_p99", "agg write p99 ms"),
    ("txg_ms", "txg sync mean ms"),
]
# Firm expectations only, the two harms PR2 v1 showed (TASK-1120-WBP2) that
# PR2 v2 must not have: (scenario, pair, metric) -> verdict.  a: ionice -c3
# still holds agg's reads and still protects vic; b, c, d: the cgroup
# control still limits agg's reads.
EXPECT = {}
for _p in ("S1/M", "S3/M"):
    EXPECT[("a", _p, "ar_mbs")] = "no difference shown"
    for _m in ("vr_p99", "vw_fs_mean", "txg_ms"):
        EXPECT[("a", _p, _m)] = "not higher"
    for _s in ("b", "c", "d"):
        EXPECT[(_s, _p, "ar_mbs")] = "not higher"


def kind_of(rwbs):
    if "D" in rwbs:
        return "D"
    if "R" in rwbs:
        return "R"
    if "W" in rwbs:
        return "W"
    return "O"


def cls_of(name, level):
    if name == "none":
        return "none"
    return "%s%s" % (name, level)


def group_of(comm):
    if comm in ("wbp2-vr", "wbp2-vw"):
        return "vic"
    if comm in ("wbp2-ar", "wbp2-aw"):
        return "agg"
    if comm.startswith("z_") or comm.startswith("txg_"):
        return "zfs"
    if comm.startswith("kworker"):
        return "kworker"
    return "other"


def tracesum(inp, out):
    sums = {}
    inbuf = written = bad = 0
    for line in inp:
        if line.startswith("#"):
            m = ENTRIES_RE.search(line)
            if m:
                inbuf, written = int(m.group(1)), int(m.group(2))
            continue
        if not line.strip():
            continue
        m = TRACE_RE.match(line)
        if not m:
            bad += 1
            continue
        comm = m.group(13).replace(" ", "_")
        if comm.startswith("kworker"):
            comm = "kworker"
        key = (comm, kind_of(m.group(6)), cls_of(m.group(10), m.group(12)))
        n, b = sums.get(key, (0, 0))
        sums[key] = (n + 1, b + int(m.group(7)))
    for key in sorted(sums):
        out.write("Q %s %s %s %d %d\n" % (key + sums[key]))
    out.write("E %d %d %d\n" % (inbuf, written, bad))


def readlines(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except OSError:
        return []


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    k = max(0, int(math.ceil(p * len(s))) - 1)
    return s[k]


def mean(vals):
    return sum(vals) / len(vals) if vals else None


def load_ops(path, t0, t1):
    ops = {"R": [], "W": [], "F": []}
    for line in readlines(path):
        p = line.split()
        if len(p) != 4 or p[0] == "END":
            continue
        t = float(p[0])
        if t0 <= t <= t1 and p[1] in ops:
            ops[p[1]].append((int(p[2]), int(p[3]) / 1e6))
    return ops


def kv(fields):
    d = {}
    for f in fields:
        if "=" in f:
            k, v = f.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                pass
    return d


def snap(path):
    s = {"iostat": {}}
    for line in readlines(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "time":
            s["time"] = float(p[1])
        elif p[0] == "cpu":
            s["cpu"] = [int(x) for x in p[1:]]
        elif p[0] == "loadavg":
            s["load1"] = float(p[1])
        elif p[0] == "disk":
            f = p[1:]
            s["disk"] = (int(f[3]), int(f[5]), int(f[7]), int(f[9]))
        elif p[0] == "iostat" and len(p) >= 3:
            s["iostat"][p[1]] = kv(p[3:]) if p[2] != "-" else {}
    return s


def txgs(path):
    rows = {}
    for line in readlines(path):
        p = line.split()
        if len(p) >= 12 and p[0].isdigit():
            rows[int(p[0])] = (p[2], int(p[11]))
    return rows


def txg_ms(d):
    a, b = txgs(os.path.join(d, "txgs0.txt")), txgs(os.path.join(d, "txgs1.txt"))
    done0 = [t for t, (st, _) in a.items() if st == "C"]
    last0 = max(done0) if done0 else -1
    st = [v / 1e6 for t, (s, v) in b.items() if s == "C" and t > last0]
    return mean(st), len(st)


def layout(d):
    lay = {"ionice": {}, "cgroup": {}}
    for line in readlines(os.path.join(d, "layout.txt")):
        p = line.split()
        if not p:
            continue
        if p[0] == "window":
            lay["ramp"], lay["meas"] = float(p[2]), float(p[4])
        elif p[0] == "srcversion":
            lay["srcversion"] = p[1]
        elif p[0].startswith("zfs_vdev_disk_rt_mask="):
            lay["rt_mask"] = p[0].split("=", 1)[1]
        elif p[0] == "pid":
            m = re.search(r"cgroup (.*?) ionice (.*)$", line)
            if m:
                lay["cgroup"][p[1]] = m.group(1).strip()
                lay["ionice"][p[1]] = m.group(2).strip()
        elif p[0] in ("agg", "vic", "root"):
            lay[" ".join(p[0:2])] = " ".join(p[2:])
        elif p[0].startswith("wbp2nb"):
            lay["dev"] = line
    return lay


def tsum(d):
    q = {}
    e = (0, 0, 0)
    for line in readlines(os.path.join(d, "trace-sum.txt")):
        p = line.split()
        if p and p[0] == "Q" and len(p) == 6:
            q[(p[1], p[2], p[3])] = (int(p[4]), int(p[5]))
        elif p and p[0] == "E" and len(p) == 4:
            e = (int(p[1]), int(p[2]), int(p[3]))
    return q, e


def run_metrics(d):
    lay = layout(d)
    ramp, meas = lay.get("ramp", 0.0), lay.get("meas", 0.0)
    t0, t1 = ramp, ramp + meas
    m = {"lay": lay}
    vr = load_ops(os.path.join(d, "vr.txt"), t0, t1)
    lat = [x[1] for x in vr["R"]]
    m["vr_n"] = len(lat)
    m["vr_p50"], m["vr_p99"] = pct(lat, 0.5), pct(lat, 0.99)
    m["vr_max"] = max(lat) if lat else None
    vw = load_ops(os.path.join(d, "vw.txt"), t0, t1)
    fs = [x[1] for x in vw["F"]]
    m["vw_fs_n"] = len(fs)
    m["vw_fs_mean"], m["vw_fs_p90"] = mean(fs), pct(fs, 0.9)
    m["vw_fs_p99"] = pct(fs, 0.99)
    m["vw_fs_max"] = max(fs) if fs else None
    ar = load_ops(os.path.join(d, "ar.txt"), t0, t1)
    m["ar_mbs"] = sum(x[0] for x in ar["R"]) / 1e6 / meas if meas else None
    m["ar_iops"] = len(ar["R"]) / meas if meas else None
    m["ar_p99"] = pct([x[1] for x in ar["R"]], 0.99)
    aw = load_ops(os.path.join(d, "aw.txt"), t0, t1)
    awl = [x[1] for x in aw["W"]]
    m["aw_ops"] = len(awl) / meas if meas else None
    m["aw_p99"] = pct(awl, 0.99)
    m["aw_max"] = max(awl) if awl else None
    m["txg_ms"], m["txg_n"] = txg_ms(d)
    s0, s1 = snap(os.path.join(d, "snap0.txt")), snap(os.path.join(d, "snap1.txt"))
    dt = s1.get("time", 0) - s0.get("time", 0)
    if "disk" in s0 and "disk" in s1 and dt > 0:
        rs = s1["disk"][1] - s0["disk"][1]
        ws = s1["disk"][3] - s0["disk"][3]
        m["dev_rmbs"] = rs * 512 / 1e6 / dt
        m["dev_wmbs"] = ws * 512 / 1e6 / dt
    if "cpu" in s0 and "cpu" in s1:
        dc = [b - a for a, b in zip(s0["cpu"], s1["cpu"])]
        tot = sum(dc[:8]) or 1
        m["steal"] = 100.0 * dc[7] / tot if len(dc) > 7 else 0.0
    m["load1"] = s1.get("load1")
    for g in ("vic", "agg"):
        a = s0["iostat"].get(g, {})
        b = s1["iostat"].get(g, {})
        for k in ("rbytes", "wbytes", "rios", "wios", "cost.usage"):
            # iocost's fields appear only while it is enabled.
            if k in a and k in b:
                m["%s_%s" % (g, k)] = b[k] - a[k]
            elif k in b and not k.startswith("cost."):
                m["%s_%s" % (g, k)] = b[k]
            else:
                m["%s_%s" % (g, k)] = 0.0
    q, e = tsum(d)
    m["trace"] = q
    m["trace_e"] = e
    return m


def fmt(v, nd=1):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.*f" % (nd, v)
    return str(v)


def brief(d):
    m = run_metrics(d)
    cls = {}
    for (comm, kind, c), (n, _) in m["trace"].items():
        g = group_of(comm)
        cls.setdefault(g, {})
        cls[g][c] = cls[g].get(c, 0) + n
    parts = []
    for g in ("agg", "vic", "zfs", "kworker"):
        if g in cls:
            parts.append("%s %s" % (g, ",".join(
                "%s:%d" % kv2 for kv2 in sorted(cls[g].items()))))
    lost = m["trace_e"][1] - m["trace_e"][0]
    return ("vr p99 %s ms, fsync mean %s ms, ar %s MB/s, aw %s/s; %s%s" % (
        fmt(m["vr_p99"]), fmt(m["vw_fs_mean"]), fmt(m["ar_mbs"]),
        fmt(m["aw_ops"]), "; ".join(parts),
        "; TRACE LOST %d" % lost if lost > 0 else ""))


def runs(raw, prefix):
    out = {}
    for pname in sorted(os.listdir(raw)):
        if not pname.startswith(prefix):
            continue
        pdir = os.path.join(raw, pname)
        if not os.path.isdir(pdir):
            continue
        for arm in ARMS:
            for sc in SCEN:
                d = os.path.join(pdir, arm, sc)
                if os.path.isfile(os.path.join(d, "snap1.txt")):
                    out[(pname, arm, sc)] = run_metrics(d)
    return out


def is_rt(c):
    return c.startswith("rt")


def is_idle(c):
    return c.startswith("idle")


def class_counts(sel, group=None, kind=None):
    cls = {}
    for m in sel:
        for (comm, k, c), (n, _) in m["trace"].items():
            if group is not None and group_of(comm) != group:
                continue
            if kind is not None and k != kind:
                continue
            cls[c] = cls.get(c, 0) + n
    return cls


def class_checks(sc, arm, sel):
    """(check, count, result): the facts the code fixes, see the docstring."""
    out = []
    allc = class_counts(sel)
    tot = sum(allc.values())
    rt = sum(n for c, n in allc.items() if is_rt(c))
    if arm == "M":
        out.append(("no real-time request", "%d of %d" % (rt, tot),
                    "as expected" if rt == 0 else "surprise"))
    if sc == "a":
        ac = class_counts(sel, "agg")
        at = sum(ac.values())
        bad = sum(n for c, n in ac.items() if not is_idle(c))
        out.append(("agg (ionice -c3): every request idle",
                    "%d of %d not idle" % (bad, at),
                    "as expected" if bad == 0 and at > 0 else "surprise"))
    if arm == "S1":
        wc = class_counts(sel, kind="W")
        wrt = sum(n for c, n in wc.items() if is_rt(c))
        out.append(("no real-time write", "%d of %d" % (
            wrt, sum(wc.values())), "as expected" if wrt == 0 else
            "surprise"))
    if arm in ("S1", "S3"):
        vc = class_counts(sel, "vic", "R")
        vt = sum(vc.values())
        vrt = sum(n for c, n in vc.items() if is_rt(c))
        out.append(("vic's reads mostly real-time", "%d of %d" % (vrt, vt),
                    "as expected" if vt and 2 * vrt >= vt else "surprise"))
    if arm == "S3":
        wc = class_counts(sel, kind="W")
        wrt = sum(n for c, n in wc.items() if is_rt(c))
        out.append(("some real-time writes", "%d of %d" % (
            wrt, sum(wc.values())), "as expected" if wrt > 0 else
            "surprise"))
    return out


def mech_rows(rs, label):
    """Per scenario and arm: request classes by group, io.stat."""
    lines = []
    lines.append("## Request classes by inserting group (%s)" % label)
    lines.append("")
    lines.append("Requests inserted into the device's queue during the "
                 "measured windows, summed over the runs; `class:count`, "
                 "reads and writes apart. Listed, not judged: the checks "
                 "follow.")
    lines.append("")
    lines.append("| sc | arm | group | requests | R | W | R classes | "
                 "W classes |")
    lines.append("|---|---|---|---|---|---|---|---|")
    for sc in SCEN:
        for arm in ARMS:
            sel = [m for (p, a, s), m in rs.items() if a == arm and s == sc]
            if not sel:
                continue
            for g in GROUPS:
                rc = class_counts(sel, g, "R")
                wc = class_counts(sel, g, "W")
                tot = sum(class_counts(sel, g).values())
                if g in ("other", "kworker") and tot == 0:
                    continue
                lines.append("| %s | %s | %s | %d | %d | %d | %s | %s |" % (
                    sc, arm, g, tot, sum(rc.values()), sum(wc.values()),
                    " ".join("%s:%d" % x for x in sorted(rc.items())) or "-",
                    " ".join("%s:%d" % x for x in sorted(wc.items())) or "-"))
    lines.append("")
    lines.append("## Class checks (%s)" % label)
    lines.append("")
    lines.append("| sc | arm | check | count | result |")
    lines.append("|---|---|---|---|---|")
    for sc in SCEN:
        for arm in ARMS:
            sel = [m for (p, a, s), m in rs.items() if a == arm and s == sc]
            if not sel:
                continue
            zt = sum(class_counts(sel, "zfs").values())
            lines.append("| %s | %s | zfs threads insert requests | %d | %s |"
                         % (sc, arm, zt, "as expected" if zt > 0 else
                            "surprise"))
            for chk, cnt, res in class_checks(sc, arm, sel):
                lines.append("| %s | %s | %s | %s | %s |" % (
                    sc, arm, chk, cnt, res))
    lines.append("")
    lines.append("## blkcg charge and controller state (%s)" % label)
    lines.append("")
    lines.append("io.stat deltas of the two cgroups over the measured "
                 "window, mean per run. Expected: every arm charges both "
                 "cgroups (rios above 0): PR2 v2 does not touch the bio's "
                 "blkcg. agg cost.usage (us) comes from iocost (scenario "
                 "d), listed, not judged.")
    lines.append("")
    lines.append("| sc | arm | runs | vic rios | vic wios | agg rios | "
                 "agg wios | agg rMB charged / agg-r MB read | agg cost.usage us "
                 "| check |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for sc in SCEN:
        for arm in ARMS:
            sel = [m for (p, a, s), m in rs.items() if a == arm and s == sc]
            if not sel:
                continue
            vr = mean([m["vic_rios"] for m in sel])
            vw = mean([m["vic_wios"] for m in sel])
            ar = mean([m["agg_rios"] for m in sel])
            aw = mean([m["agg_wios"] for m in sel])
            cw = mean([m["agg_cost.usage"] for m in sel])
            rb = sum(m["agg_rbytes"] for m in sel)
            rd = sum((m["ar_mbs"] or 0) * m["lay"].get("meas", 0) * 1e6
                     for m in sel)
            share = rb / rd if rd else None
            ok = all(m["vic_rios"] > 0 and m["agg_rios"] > 0 for m in sel)
            chk = "as expected" if ok else "surprise: not charged"
            lines.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s | %s |"
                         % (sc, arm, len(sel), fmt(vr, 0), fmt(vw, 0),
                            fmt(ar, 0), fmt(aw, 0), fmt(share, 2),
                            fmt(cw, 0), chk))
    lines.append("")
    return lines


def signflip_p(logs):
    n = len(logs)
    obs = abs(sum(logs))
    hit = 0
    for signs in itertools.product((1, -1), repeat=n):
        if abs(sum(s * x for s, x in zip(signs, logs))) >= obs - 1e-12:
            hit += 1
    return hit / float(2 ** n)


def verdict(rs, sc, a, b, key):
    logs = []
    for (p, arm, s), m in rs.items():
        if arm != a or s != sc:
            continue
        o = rs.get((p, b, sc))
        if not o:
            continue
        x, y = m.get(key), o.get(key)
        if x is None or y is None or x <= 0 or y <= 0:
            continue
        logs.append(math.log(x / y))
    if not logs:
        return None, None, 0, "-"
    gm = math.exp(sum(logs) / len(logs))
    p = signflip_p(logs)
    if gm <= 0.97 and p <= 0.05:
        v = "lower"
    elif gm >= 1.03 and p <= 0.05:
        v = "higher"
    else:
        v = "no difference shown"
    lo = sum(1 for x in logs if x < 0)
    return gm, p, len(logs), "%s (%d/%d below 1)" % (v, lo, len(logs)) \
        if v else v


def summary(raw, prefix):
    rs = runs(raw, prefix)
    out = ["# TASK-1121-WBP2V2 summary (%s*)" % prefix, ""]
    passes = sorted({p for p, _, _ in rs})
    out.append("Passes: %s. Runs: %d." % (" ".join(passes) or "-", len(rs)))
    out.append("")
    out.append("## Per arm and scenario, mean over passes")
    out.append("")
    out.append("| sc | arm | runs | vic read p50 / p99 / max ms | "
               "vic fsync n / mean / p90 / p99 ms | agg read MB/s / IOPS / "
               "p99 ms | agg write ops/s / p99 ms | txg sync ms (n) | "
               "dev r / w MB/s |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for sc in SCEN:
        for arm in ARMS:
            sel = [m for (p, a, s), m in rs.items() if a == arm and s == sc]
            if not sel:
                continue

            def mm(k, nd=1):
                return fmt(mean([m[k] for m in sel if m.get(k) is not None]),
                           nd)
            out.append("| %s | %s | %d | %s / %s / %s | %s / %s / %s / %s | "
                       "%s / %s / %s | %s / %s | %s (%s) | %s / %s |" % (
                           sc, arm, len(sel), mm("vr_p50"), mm("vr_p99"),
                           mm("vr_max"), mm("vw_fs_n", 0), mm("vw_fs_mean"),
                           mm("vw_fs_p90"), mm("vw_fs_p99"), mm("ar_mbs"),
                           mm("ar_iops", 0), mm("ar_p99"), mm("aw_ops"),
                           mm("aw_p99"), mm("txg_ms"), mm("txg_n", 0),
                           mm("dev_rmbs"), mm("dev_wmbs")))
    out.append("")
    out.append("## Verdicts")
    out.append("")
    out.append("| sc | pair | metric | geo mean | p | n | verdict | "
               "expected | check |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for sc in SCEN:
        for a, b in PAIRS:
            for key, label in METRICS:
                gm, p, n, v = verdict(rs, sc, a, b, key)
                if n == 0:
                    continue
                pair = "%s/%s" % (a, b)
                exp = EXPECT.get((sc, pair, key), "")
                chk = ""
                if exp == "not higher":
                    chk = "surprise" if v.startswith("higher") else \
                        "as expected"
                elif exp:
                    chk = "as expected" if v.startswith(exp) else "surprise"
                out.append("| %s | %s | %s | %.3f | %.3f | %d | %s | %s | %s |"
                           % (sc, pair, label, gm, p, n, v, exp or "-",
                              chk or "-"))
    out.append("")
    out.extend(mech_rows(rs, prefix + "*"))
    out.append("## Identity and noise")
    out.append("")
    out.append("| pass | arm | sc | srcversion | settings | steal % | load1 | "
               "trace lost | unparsed |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for (p, arm, sc) in sorted(rs):
        m = rs[(p, arm, sc)]
        lay = m["lay"]
        st = []
        for k in ("rt_mask", "agg io.prio.class", "agg io.max",
                  "agg io.weight", "vic io.weight", "root io.cost.qos"):
            if k in lay and lay[k]:
                st.append("%s=%s" % (k.split()[-1], lay[k]))
        e = m["trace_e"]
        out.append("| %s | %s | %s | %s | %s | %s | %s | %d | %d |" % (
            p, arm, sc, lay.get("srcversion", "-"), "; ".join(st) or "-",
            fmt(m.get("steal"), 2), fmt(m.get("load1"), 2),
            max(0, e[1] - e[0]), e[2]))
    out.append("")
    return "\n".join(out)


def flags_report(fdir):
    rs = {}
    for arm in ARMS:
        for sc in SCEN:
            d = os.path.join(fdir, arm, sc)
            if os.path.isfile(os.path.join(d, "snap1.txt")):
                rs[("F", arm, sc)] = run_metrics(d)
    out = ["# TASK-1121-WBP2V2 flag check", ""]
    out.extend(mech_rows(rs, "F"))
    out.append("| arm | sc | srcversion | ionice of the streams | "
               "trace lost | unparsed |")
    out.append("|---|---|---|---|---|---|")
    for (p, arm, sc) in sorted(rs):
        lay = rs[(p, arm, sc)]["lay"]
        e = rs[(p, arm, sc)]["trace_e"]
        io = sorted({"%s %s" % (lay["cgroup"][k].split("/")[-1],
                                lay["ionice"][k]) for k in lay["ionice"]})
        out.append("| %s | %s | %s | %s | %d | %d |" % (
            arm, sc, lay.get("srcversion", "-"), "; ".join(io) or "-",
            max(0, e[1] - e[0]), e[2]))
    out.append("")
    return "\n".join(out)


def main(argv):
    if len(argv) == 2 and argv[1] == "tracesum":
        tracesum(sys.stdin, sys.stdout)
    elif len(argv) == 3 and argv[1] == "brief":
        print(brief(argv[2]))
    elif len(argv) == 3 and argv[1] == "flags":
        print(flags_report(argv[2]))
    elif len(argv) == 3:
        print(summary(argv[1], argv[2]))
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
