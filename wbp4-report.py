#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1120-WBP4 report.

  wbp4-report.py trace2csv < trace > discards.csv
      pairs each discard's block_bio_queue (q, its submitting task),
      block_getrq (g: after blk-wbt and the tag), block_rq_issue (d) and
      block_rq_complete (c) by device and sector: one line per discard,
      "q,comm,dev,sector,nsect,g,d,c" (seconds, trace clock), then
      "# E <in buffer> <written> <unparsed> <not completed> <completions
      without a queue event>"
  wbp4-report.py brief RUNDIR          one line for driver.log
  wbp4-report.py flags RAW/F           the check before GO, Markdown
  wbp4-report.py RAWDIR PREFIX         summary of the passes PREFIX*

g - q is the time the submitter slept between submit_bio() and getting a
request: blk-wbt's wait for discards (and a wait for a tag).  c - q is the
discard's latency as ZFS submitted it.

Verdicts (passes where both arms ran the row): ratio T / M in the same
pass; "lower" if the geometric mean of the ratios is at most 0.97 and the
exact two-sided sign-flip permutation p of the log ratios is at most 0.05,
"higher" if at least 1.03 with the same p, else "no difference shown".
"""

import itertools
import math
import os
import re
import sys

LINE_RE = re.compile(r"^\s*(.*?)-(\d+)\s+\[\d+\]\s+(?:\S+\s+)?(\d+\.\d+):\s+"
                     r"(block_bio_queue|block_getrq|block_rq_issue|"
                     r"block_rq_complete):\s+(.*)$")
BIO_RE = re.compile(r"^(\d+),(\d+)\s+(\S+)\s+(\d+)\s+\+\s+(\d+)\s+\[(.*)\]\s*$")
ISS_RE = re.compile(r"^(\d+),(\d+)\s+(\S+)\s+\d+\s+\(.*?\)\s+(\d+)\s+\+\s+"
                    r"(\d+)\s+\S+\s+\[(.*)\]\s*$")
CMP_RE = re.compile(r"^(\d+),(\d+)\s+(\S+)\s+\(.*?\)\s+(\d+)\s+\+\s+(\d+)"
                    r"\s+\S+\s+\[(-?\d+)\]\s*$")
ENTRIES_RE = re.compile(r"entries-in-buffer/entries-written:\s*(\d+)/(\d+)")
ARMS = ["M", "T"]
ROWS = ["mt", "at"]
COMMS = ["z_trim_int", "z_trim_iss", "other"]
METRICS = {
    "mt": [("trim_s", "trim seconds"), ("disc_per_s", "discards/s"),
           ("busy", "device discard-busy fraction"),
           ("fs_p50", "fsync p50 ms"), ("fs_p99", "fsync p99 ms"),
           ("qc_p99", "discard latency p99 ms")],
    "at": [("disc_per_s", "discards/s"), ("fs_p50", "fsync p50 ms"),
           ("fs_p99", "fsync p99 ms"), ("qc_p99", "discard latency p99 ms")],
}
EXPECT = {("mt", "trim_s"): "lower", ("mt", "disc_per_s"): "higher",
          ("mt", "busy"): "higher"}


def trace2csv(inp, out):
    recs = {}
    inbuf = written = bad = orphan_c = 0
    rows = []
    for line in inp:
        if line.startswith("#"):
            m = ENTRIES_RE.search(line)
            if m:
                inbuf, written = int(m.group(1)), int(m.group(2))
            continue
        if not line.strip():
            continue
        m = LINE_RE.match(line)
        if not m:
            bad += 1
            continue
        ts, ev, rest = float(m.group(3)), m.group(4), m.group(5)
        if ev in ("block_bio_queue", "block_getrq"):
            b = BIO_RE.match(rest)
        elif ev == "block_rq_issue":
            b = ISS_RE.match(rest)
        else:
            b = CMP_RE.match(rest)
        if not b:
            bad += 1
            continue
        if "D" not in b.group(3):
            continue
        key = (b.group(1), b.group(2), b.group(4))
        if ev == "block_bio_queue":
            recs[key] = {"q": ts, "comm": b.group(6).replace(",", "_"),
                         "nsect": int(b.group(5))}
            continue
        r = recs.get(key)
        if r is None:
            if ev == "block_rq_complete":
                orphan_c += 1
            continue
        if ev == "block_getrq":
            r.setdefault("g", ts)
        elif ev == "block_rq_issue":
            r.setdefault("d", ts)
        else:
            r["c"] = ts
            rows.append((key, r))
            del recs[key]
    for key, r in rows:
        out.write("%.6f,%s,%s:%s,%s,%d,%s,%s,%.6f\n" % (
            r["q"], r["comm"], key[0], key[1], key[2], r["nsect"],
            "%.6f" % r["g"] if "g" in r else "",
            "%.6f" % r["d"] if "d" in r else "", r["c"]))
    out.write("# E %d %d %d %d %d\n" % (inbuf, written, bad, len(recs),
                                         orphan_c))


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
    return s[max(0, int(math.ceil(p * len(s))) - 1)]


def mean(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else None


def comm_of(c):
    return c if c in ("z_trim_int", "z_trim_iss") else "other"


def union_len(iv):
    tot = 0.0
    end = None
    for a, b in sorted(iv):
        if end is None or a > end:
            tot += b - a
            end = b
        elif b > end:
            tot += b - end
            end = b
    return tot


def layout(d):
    lay = {"wb_background": []}
    for line in readlines(os.path.join(d, "layout.txt")):
        p = line.split()
        if not p:
            continue
        if p[0] == "window":
            lay["w0"], lay["w1"] = float(p[1]), float(p[2])
            lay["rc"], lay["capped"] = p[4], p[6]
        elif p[0] == "srcversion":
            lay["srcversion"] = p[1]
        elif p[0] == "arm":
            lay["arm"], lay["row"] = p[1], p[-1]
        elif "=" in p[0]:
            k, v = p[0].split("=", 1)
            lay[k] = v
        elif p[0].startswith("wbp4nb") and "wb_background" in p:
            lay["wb_background"].append(p[p.index("wb_background") + 1])
    return lay


def run_metrics(d):
    lay = layout(d)
    m = {"lay": lay}
    w0, w1 = lay.get("w0", 0.0), lay.get("w1", 0.0)
    span_w = w1 - w0
    m["trim_s"] = span_w if lay.get("row") == "mt" else None
    m["capped"] = lay.get("capped", "-")
    disc = []
    e = [0, 0, 0, 0, 0]
    for line in readlines(os.path.join(d, "discards.csv")):
        if line.startswith("# E"):
            e = [int(x) for x in line.split()[2:7]]
            continue
        p = line.split(",")
        if len(p) != 8:
            continue
        disc.append({"q": float(p[0]), "comm": comm_of(p[1]), "dev": p[2],
                     "bytes": int(p[4]) * 512,
                     "g": float(p[5]) if p[5] else None,
                     "d": float(p[6]) if p[6] else None, "c": float(p[7])})
    m["trace_e"] = e
    m["n"] = len(disc)
    m["bytes"] = sum(x["bytes"] for x in disc)
    qc = [(x["c"] - x["q"]) * 1e3 for x in disc]
    m["qc_p50"], m["qc_p99"] = pct(qc, 0.5), pct(qc, 0.99)
    dt = [(x["c"] - x["d"]) * 1e3 for x in disc if x["d"] is not None]
    m["dev_p50"] = pct(dt, 0.5)
    by = {}
    for c in COMMS:
        sel = [x for x in disc if x["comm"] == c]
        waits = [x["g"] - x["q"] for x in sel if x["g"] is not None]
        by[c] = {"n": len(sel), "wait_s": sum(waits),
                 "wait_p99_ms": (pct(waits, 0.99) or 0) * 1e3 if waits
                 else None}
    m["by"] = by
    if disc:
        t0 = min(x["q"] for x in disc)
        t1 = max(x["c"] for x in disc)
        span = t1 - t0
    else:
        span = 0.0
    if lay.get("row") == "mt":
        m["disc_per_s"] = m["n"] / span if span > 0 else None
        devs = sorted({x["dev"] for x in disc})
        fr = []
        for dv in devs:
            iv = [(x["d"], x["c"]) for x in disc
                  if x["dev"] == dv and x["d"] is not None]
            if span > 0:
                fr.append(union_len(iv) / span)
        m["busy"] = mean(fr)
    else:
        m["disc_per_s"] = m["n"] / span_w if span_w > 0 else None
        m["busy"] = None
    fs = []
    for line in readlines(os.path.join(d, "syncw.txt")):
        p = line.split()
        if len(p) == 4 and p[1] == "F" and w0 <= float(p[0]) <= w1:
            fs.append(int(p[3]) / 1e6)
    m["fs_n"] = len(fs)
    m["fs_p50"], m["fs_p99"] = pct(fs, 0.5), pct(fs, 0.99)
    m["fs_max"] = max(fs) if fs else None
    smp = {}
    for line in readlines(os.path.join(d, "samples.txt")):
        p = line.split()
        if len(p) != 6 or p[0] != "S" or not w0 <= float(p[1]) <= w1:
            continue
        s = smp.setdefault(comm_of(p[2]), [0, 0, 0])
        s[0] += 1
        s[1] += p[4] == "D"
        s[2] += p[5] == "1"
    m["smp"] = smp
    k = {}
    for line in readlines(os.path.join(d, "sched.txt")):
        p = line.split()
        if len(p) == 7 and p[0] == "K" and p[5] != "-":
            k.setdefault((p[3], p[4]), {})[p[1]] = float(p[5])
    dsec = {}
    for (comm, tid), v in k.items():
        if "0" in v and "1" in v:
            c = comm_of(comm)
            dsec[c] = dsec.get(c, 0.0) + (v["1"] - v["0"]) / 1e3
    m["dsec"] = dsec
    ch = [line.split() for line in readlines(os.path.join(d, "churn.txt"))]
    ch = [p for p in ch if len(p) == 5 and p[1] == "C" and
          w0 <= float(p[0]) <= w1]
    m["churn_c"] = sum(int(p[2]) for p in ch) / span_w if ch and span_w else None
    m["churn_d"] = sum(int(p[4]) for p in ch) / span_w if ch and span_w else None
    return m


def fmt(v, nd=1):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.*f" % (nd, v)
    return str(v)


def brief(d):
    m = run_metrics(d)
    by = m["by"]
    lost = m["trace_e"][1] - m["trace_e"][0]
    s = ("discards %d (z_trim_int %d, z_trim_iss %d, other %d), wbt wait s "
         "int %.2f iss %.2f, latency p99 %s ms, fsync p99 %s ms" % (
             m["n"], by["z_trim_int"]["n"], by["z_trim_iss"]["n"],
             by["other"]["n"], by["z_trim_int"]["wait_s"],
             by["z_trim_iss"]["wait_s"], fmt(m["qc_p99"]), fmt(m["fs_p99"])))
    if m["trim_s"] is not None:
        s = "trim %.2f s%s, %s" % (m["trim_s"], " CAPPED" if
                                   m["capped"] == "yes" else "", s)
    if lost > 0:
        s += ", TRACE LOST %d" % lost
    return s


def runs(raw, prefix):
    out = {}
    for pname in sorted(os.listdir(raw)):
        if not pname.startswith(prefix):
            continue
        for arm in ARMS:
            for row in ROWS:
                d = os.path.join(raw, pname, arm, row)
                if os.path.isfile(os.path.join(d, "discards.csv")):
                    out[(pname, arm, row)] = run_metrics(d)
    return out


def mech(rs, label):
    out = ["## Who submits the discards, and who waits (%s)" % label, ""]
    out.append("Per arm and row, summed or averaged over the runs. Share: "
               "discards submitted by the thread. wbt wait: g - q summed per "
               "run (s) and its p99 per discard (ms). wbt samples: share of "
               "the 20 ms samples of that thread class asleep in "
               "wbt_wait/rq_qos_wait. D s: schedstat sum_block_runtime per "
               "run (s). Expected: M has discards submitted by z_trim_int "
               "that wait in blk-wbt; T has none submitted by z_trim_int.")
    out.append("")
    out.append("| row | arm | runs | discards/run | thread | share | wbt wait "
               "s/run | wait p99 ms | wbt samples | D s/run | check |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for row in ROWS:
        for arm in ARMS:
            sel = [m for (p, a, r), m in rs.items() if a == arm and r == row]
            if not sel:
                continue
            tot = sum(m["n"] for m in sel)
            for c in COMMS:
                n = sum(m["by"][c]["n"] for m in sel)
                ws = mean([m["by"][c]["wait_s"] for m in sel])
                wp = mean([m["by"][c]["wait_p99_ms"] for m in sel])
                sm = [m["smp"].get(c, [0, 0, 0]) for m in sel]
                ns = sum(x[0] for x in sm)
                wb = sum(x[2] for x in sm) / float(ns) if ns else None
                ds = mean([m["dsec"].get(c) for m in sel])
                chk = "-"
                if c == "z_trim_int":
                    if arm == "M":
                        chk = "as expected" if n > 0 and (ws or 0) > 0 else \
                            "surprise: %s" % ("no discard" if n == 0 else
                                              "no wbt wait")
                    else:
                        chk = "as expected" if n == 0 else \
                            "surprise: %d discards" % n
                out.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s | %s "
                           "| %s |" % (row, arm, len(sel),
                                       fmt(tot / float(len(sel)), 0), c,
                                       fmt(n / float(tot), 3) if tot else "-",
                                       fmt(ws, 2), fmt(wp, 1), fmt(wb, 3),
                                       fmt(ds, 2), chk))
    out.append("")
    return out


def signflip_p(logs):
    obs = abs(sum(logs))
    hit = 0
    for signs in itertools.product((1, -1), repeat=len(logs)):
        if abs(sum(s * x for s, x in zip(signs, logs))) >= obs - 1e-12:
            hit += 1
    return hit / float(2 ** len(logs))


def verdict(rs, row, key):
    logs = []
    for (p, arm, r), m in rs.items():
        if arm != "T" or r != row:
            continue
        o = rs.get((p, "M", row))
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
    return gm, p, len(logs), "%s (%d/%d below 1)" % (v, lo, len(logs))


def per_run_table(rs):
    out = ["| pass | arm | row | srcversion | wb_background | trim s | "
           "discards | MiB | discards/s | busy | latency p50 / p99 ms | "
           "device p50 ms | fsync n / p50 / p99 / max ms | churn c / d per s "
           "| trace lost | unparsed / open / orphan |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for (p, arm, row) in sorted(rs):
        m = rs[(p, arm, row)]
        e = m["trace_e"]
        out.append("| %s | %s | %s | %s | %s | %s%s | %d | %s | %s | %s | "
                   "%s / %s | %s | %d / %s / %s / %s | %s / %s | %d | "
                   "%d / %d / %d |" % (
                       p, arm, row, m["lay"].get("srcversion", "-"),
                       ",".join(m["lay"]["wb_background"]) or "-",
                       fmt(m["trim_s"], 2),
                       " CAPPED" if m["capped"] == "yes" else "", m["n"],
                       fmt(m["bytes"] / 1048576.0, 0), fmt(m["disc_per_s"], 0),
                       fmt(m["busy"], 3), fmt(m["qc_p50"], 2),
                       fmt(m["qc_p99"], 2), fmt(m["dev_p50"], 2), m["fs_n"],
                       fmt(m["fs_p50"], 2), fmt(m["fs_p99"], 2),
                       fmt(m["fs_max"], 2), fmt(m["churn_c"], 0),
                       fmt(m["churn_d"], 0), max(0, e[1] - e[0]), e[2], e[3],
                       e[4]))
    out.append("")
    return out


def summary(raw, prefix):
    rs = runs(raw, prefix)
    out = ["# TASK-1120-WBP4 summary (%s*)" % prefix, ""]
    passes = sorted({p for p, _, _ in rs})
    out.append("Passes: %s. Runs: %d." % (" ".join(passes) or "-", len(rs)))
    out.append("")
    out.append("## Verdicts, T / M")
    out.append("")
    out.append("| row | metric | geo mean | p | n | verdict | expected | "
               "check |")
    out.append("|---|---|---|---|---|---|---|---|")
    for row in ROWS:
        for key, label in METRICS[row]:
            gm, p, n, v = verdict(rs, row, key)
            if n == 0:
                continue
            exp = EXPECT.get((row, key), "")
            chk = ("as expected" if v.startswith(exp) else "surprise") \
                if exp else "-"
            out.append("| %s | %s | %.3f | %.3f | %d | %s | %s | %s |" % (
                row, label, gm, p, n, v, exp or "-", chk))
    out.append("")
    out.append("## Means per arm and row")
    out.append("")
    out.append("| row | arm | runs | trim s | discards/s | busy | latency p50 "
               "/ p99 ms | fsync p50 / p99 / max ms |")
    out.append("|---|---|---|---|---|---|---|---|")
    for row in ROWS:
        for arm in ARMS:
            sel = [m for (p, a, r), m in rs.items() if a == arm and r == row]
            if not sel:
                continue

            def mm(k, nd=2):
                return fmt(mean([m.get(k) for m in sel]), nd)
            out.append("| %s | %s | %d | %s | %s | %s | %s / %s | %s / %s / "
                       "%s |" % (row, arm, len(sel), mm("trim_s"),
                                 mm("disc_per_s", 0), mm("busy", 3),
                                 mm("qc_p50"), mm("qc_p99"), mm("fs_p50"),
                                 mm("fs_p99"), mm("fs_max")))
    out.append("")
    out.extend(mech(rs, prefix + "*"))
    out.append("## Per run")
    out.append("")
    out.extend(per_run_table(rs))
    return "\n".join(out)


def flags_report(fdir):
    rs = {}
    for arm in ARMS:
        for row in ROWS:
            d = os.path.join(fdir, arm, row)
            if os.path.isfile(os.path.join(d, "discards.csv")):
                rs[("F", arm, row)] = run_metrics(d)
    out = ["# TASK-1120-WBP4 rig check", ""]
    out.extend(mech(rs, "F"))
    out.extend(per_run_table(rs))
    return "\n".join(out)


def main(argv):
    if len(argv) == 2 and argv[1] == "trace2csv":
        trace2csv(sys.stdin, sys.stdout)
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
