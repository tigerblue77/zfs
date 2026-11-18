#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1118-WBT report.

  wbt-report.py RAW [PREFIX]

Reads RAW/<pass>/<row>/<arm>/ for every pass whose name starts with PREFIX
(default "P") and prints a Markdown summary: per row and arm the means over
the passes, the verdict of each pair of arms, where the threads slept, the
device queues, and the noise.

Verdict of a pair a/b on a metric, over the passes where both ran the row:
ratio = a / b in the same pass; "better" when the geometric mean of the
ratios is at least 3 % on the good side (below 0.97 for a latency, above
1.03 for a throughput) and the exact sign-flip permutation p over the log
ratios is at most 0.05, "worse" when the same holds on the bad side,
otherwise "no difference shown".  Six passes are the fewest that can reach
p <= 0.05.
"""

import itertools
import math
import os
import re
import sys

ARMS = ["M", "M0", "S", "A"]
ROWS = ["R75", "R10"]
PAIRS = [("A", "M"), ("S", "M"), ("A", "S"), ("M0", "M"), ("A", "M0")]
CLASSES = ["z_wr_int", "z_wr_iss", "z_rd_int", "z_rd_iss", "z_null",
           "z_fr", "z_cl", "zvol", "txg_sync", "python3", "other"]
# metric, label, lower is better
VERDICT_METRICS = [
    ("f_mean", "fsync mean ms", True),
    ("f_p90", "fsync p90 ms", True),
    ("r_p50", "read p50 ms", True),
    ("r_p99", "read p99 ms", True),
    ("w_mbs", "writer MB/s", False),
    ("b_mbs", "bulk MB/s", False),
    ("psi_some", "PSI io some %", True),
]


def pct(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int(math.ceil(p / 100.0 * len(s))) - 1))
    return s[k]


def fmt(v, nd=1):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    return ("%%.%df" % nd) % v


def read_layout(d):
    lay = {"ramp": 10.0, "meas": 60.0, "ivl": 0.25}
    try:
        with open(os.path.join(d, "layout.txt")) as f:
            for line in f:
                p = line.split()
                if p and p[0] == "window" and len(p) >= 7:
                    lay["ramp"] = float(p[2])
                    lay["meas"] = float(p[4])
                    lay["ivl"] = float(p[6])
                elif p and p[0] == "srcversion":
                    lay["srcversion"] = p[1]
                elif p and p[0].startswith("zfs_vdev_disk_sync_writes="):
                    lay["sync_writes"] = p[0].split("=", 1)[1]
                elif p and p[0].startswith("wbt") and "wbt_lat_usec" in p:
                    lay.setdefault("wbt_lat", set()).add(
                        p[p.index("wbt_lat_usec") + 1])
    except OSError:
        pass
    return lay


def ops(path, lo, hi):
    """Operations of one stream log inside [lo, hi): {kind: [(bytes, ns)]}"""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) != 4 or p[0] == "END":
                    continue
                t = float(p[0])
                if lo <= t < hi:
                    out.setdefault(p[1], []).append((int(p[2]), int(p[3])))
    except OSError:
        pass
    return out


def cls(comm):
    for c in CLASSES[:-1]:
        if comm.startswith(c):
            return c
    return "other"


def samples(path, lo, hi):
    ticks = 0
    d = {c: 0 for c in CLASSES}
    w = {c: 0 for c in CLASSES}
    dev = {}
    frames = {}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p or len(p) < 2:
                    continue
                t = float(p[1])
                if not lo <= t < hi:
                    continue
                if p[0] == "T":
                    ticks += 1
                elif p[0] == "D" and len(p) >= 5:
                    c = cls(p[2])
                    d[c] += 1
                    if p[3] == "1":
                        w[c] += 1
                        if c == "z_wr_int":
                            frames[p[4]] = frames.get(p[4], 0) + 1
                elif p[0] == "I" and len(p) >= 9:
                    e = dev.setdefault(p[2], {"rd": [], "wr": [], "trk": [],
                                              "bg": [], "nm": []})
                    for k, i in (("rd", 3), ("wr", 4), ("trk", 5),
                                 ("nm", 6), ("bg", 7)):
                        if p[i] != "-":
                            e[k].append(int(p[i]))
    except (OSError, ValueError):
        pass
    return ticks, d, w, dev, frames


def histo(path):
    """zpool iostat -wpH: {column: [(upper_ns, count)]}"""
    names = ["tot_r", "tot_w", "disk_r", "disk_w", "syncq_r", "syncq_w",
             "asyncq_r", "asyncq_w"]
    h = {n: [] for n in names}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) < 9 or not p[0].isdigit():
                    continue
                for i, n in enumerate(names):
                    h[n].append((int(p[0]), int(p[i + 1])))
    except OSError:
        pass
    return h


def hpct(b, p):
    tot = sum(c for _, c in b)
    if tot == 0:
        return float("nan")
    acc = 0
    for v, c in b:
        acc += c
        if acc >= p / 100.0 * tot:
            return v / 1e6
    return b[-1][0] / 1e6


def snap(path):
    s = {"disk": {}, "zil": {}}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p:
                    continue
                if p[0] == "time":
                    s["time"] = float(p[1])
                elif p[0] == "cpu":
                    s["cpu"] = [int(x) for x in p[1:]]
                elif p[0] == "loadavg":
                    s["load1"] = float(p[1])
                elif p[0] == "psi_io":
                    m = re.search(r"total=(\d+)", line)
                    if m:
                        s["psi_" + p[1]] = int(m.group(1))
                elif p[0] == "disk" and len(p) >= 15:
                    s["disk"][p[3]] = [int(x) for x in p[4:]]
                elif p[0] == "zil" and len(p) == 3:
                    s["zil"][p[1]] = int(p[2])
    except (OSError, ValueError):
        pass
    return s


def run_metrics(d):
    lay = read_layout(d)
    lo, hi = lay["ramp"], lay["ramp"] + lay["meas"]
    meas = lay["meas"]
    m = {"lay": lay}
    w = ops(os.path.join(d, "writer.txt"), lo, hi)
    wl = [ns / 1e6 for _, ns in w.get("W", [])]
    fl = [ns / 1e6 for _, ns in w.get("F", [])]
    m["w_mbs"] = sum(b for b, _ in w.get("W", [])) / meas / 1e6
    m["w_p50"], m["w_p99"] = pct(wl, 50), pct(wl, 99)
    m["f_n"] = len(fl)
    m["f_mean"] = sum(fl) / len(fl) if fl else float("nan")
    m["f_p50"], m["f_p90"] = pct(fl, 50), pct(fl, 90)
    m["f_max"] = max(fl) if fl else float("nan")
    m["f_all"] = fl
    r = ops(os.path.join(d, "reader.txt"), lo, hi)
    rl = [ns / 1e6 for _, ns in r.get("R", [])]
    m["r_mbs"] = sum(b for b, _ in r.get("R", [])) / meas / 1e6
    m["r_n"] = len(rl)
    m["r_p50"], m["r_p99"] = pct(rl, 50), pct(rl, 99)
    m["r_p999"] = pct(rl, 99.9)
    m["r_all"] = rl
    b = ops(os.path.join(d, "bulk.txt"), lo, hi)
    bl = [ns / 1e6 for _, ns in b.get("W", [])]
    m["b_mbs"] = sum(x for x, _ in b.get("W", [])) / meas / 1e6
    m["b_p99"] = pct(bl, 99)
    ticks, dd, ww, dev, frames = samples(os.path.join(d, "samples.txt"),
                                         lo, hi)
    ivl = lay["ivl"]
    m["ticks"] = ticks
    # Mean number of threads of a class asleep (in D, or in blk-wbt).
    m["d"] = {c: dd[c] / ticks if ticks else float("nan") for c in CLASSES}
    m["wbt"] = {c: ww[c] / ticks if ticks else float("nan") for c in CLASSES}
    m["wbt_s"] = {c: ww[c] * ivl for c in CLASSES}
    m["wbt_zwrint"] = m["wbt"]["z_wr_int"]
    m["wbt_all"] = sum(m["wbt"][c] for c in CLASSES) if ticks else float("nan")
    m["frames"] = frames
    dv = {}
    for k, e in dev.items():
        dv[k] = {
            "rd": sum(e["rd"]) / len(e["rd"]) if e["rd"] else float("nan"),
            "wr": sum(e["wr"]) / len(e["wr"]) if e["wr"] else float("nan"),
            "wrmax": max(e["wr"]) if e["wr"] else float("nan"),
            "trk": sum(e["trk"]) / len(e["trk"]) if e["trk"] else float("nan"),
            "bg_med": pct(e["bg"], 50) if e["bg"] else float("nan"),
            "bg_le1": (100.0 * sum(1 for x in e["bg"] if x <= 1) /
                       len(e["bg"])) if e["bg"] else float("nan"),
        }
    m["dev"] = dv
    h = histo(os.path.join(d, "iostat-wH.txt"))
    for k in ("tot_w", "disk_w", "disk_r", "syncq_w", "asyncq_w"):
        m["h_" + k + "_p50"] = hpct(h[k], 50)
        m["h_" + k + "_p99"] = hpct(h[k], 99)
    s0 = snap(os.path.join(d, "snap0.txt"))
    s1 = snap(os.path.join(d, "snap1.txt"))
    el = s1.get("time", 0) - s0.get("time", 0)
    m["elapsed"] = el
    if "cpu" in s0 and "cpu" in s1:
        dc = [b - a for a, b in zip(s0["cpu"], s1["cpu"])]
        tot = sum(dc[:8]) or 1
        m["steal"] = 100.0 * dc[7] / tot if len(dc) > 7 else float("nan")
        m["busy"] = 100.0 * (tot - dc[3] - dc[4]) / tot
    else:
        m["steal"] = m["busy"] = float("nan")
    m["load1"] = s1.get("load1", float("nan"))
    for k in ("some", "full"):
        a, b2 = s0.get("psi_" + k), s1.get("psi_" + k)
        m["psi_" + k] = (100.0 * (b2 - a) / (el * 1e6)
                         if a is not None and b2 is not None and el > 0
                         else float("nan"))
    ds = {}
    for k in s1["disk"]:
        if k not in s0["disk"]:
            continue
        a, b2 = s0["disk"][k], s1["disk"][k]
        dx = [y - x for x, y in zip(a, b2)]
        # reads, rmerged, rsect, rms, writes, wmerged, wsect, wms,
        # inflight, io_ms, weighted_ms, ...
        ds[k] = {
            "r_await": dx[3] / dx[0] if dx[0] else float("nan"),
            "w_await": dx[7] / dx[4] if dx[4] else float("nan"),
            "util": 100.0 * dx[9] / (el * 1000) if el > 0 else float("nan"),
            "w_mbs": dx[6] * 512 / el / 1e6 if el > 0 else float("nan"),
        }
    m["disk"] = ds
    z0, z1 = s0["zil"], s1["zil"]
    m["zil"] = {k: z1[k] - z0.get(k, 0) for k in z1}
    return m


def signflip_p(logs):
    n = len(logs)
    if n == 0:
        return float("nan")
    obs = abs(sum(logs))
    hit = 0
    for signs in itertools.product((1, -1), repeat=n):
        if abs(sum(s * x for s, x in zip(signs, logs))) >= obs - 1e-12:
            hit += 1
    return hit / 2.0 ** n


def verdict(ra, rb, key, lower):
    rat = []
    for p in sorted(set(ra) & set(rb)):
        a, b = ra[p].get(key), rb[p].get(key)
        if a is None or b is None or not a > 0 or not b > 0:
            continue
        rat.append(a / b)
    if not rat:
        return None
    logs = [math.log(x) for x in rat]
    geo = math.exp(sum(logs) / len(logs))
    p = signflip_p(logs)
    won = sum(1 for x in rat if (x < 1 if lower else x > 1))
    good = geo <= 0.97 if lower else geo >= 1.03
    bad = geo >= 1.03 if lower else geo <= 0.97
    if good and p <= 0.05:
        v = "better"
    elif bad and p <= 0.05:
        v = "worse"
    else:
        v = "no difference shown"
    return geo, won, len(rat), p, v


def mean(xs):
    xs = [x for x in xs if x is not None and not
          (isinstance(x, float) and math.isnan(x))]
    return sum(xs) / len(xs) if xs else float("nan")


def span(xs):
    xs = [x for x in xs if x is not None and not
          (isinstance(x, float) and math.isnan(x))]
    if not xs:
        return "-"
    return "%s (%s-%s)" % (fmt(mean(xs)), fmt(min(xs)), fmt(max(xs)))


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    raw = argv[1]
    prefix = argv[2] if len(argv) > 2 else "P"
    passes = sorted(p for p in os.listdir(raw)
                    if p.startswith(prefix) and os.path.isdir(
                        os.path.join(raw, p)))
    runs = {}   # runs[row][arm][pass] = metrics
    order = {}
    for p in passes:
        try:
            with open(os.path.join(raw, p, "order.txt")) as f:
                order[p] = f.read().strip()
        except OSError:
            order[p] = "?"
        for row in os.listdir(os.path.join(raw, p)):
            rd = os.path.join(raw, p, row)
            if not os.path.isdir(rd):
                continue
            for arm in os.listdir(rd):
                ad = os.path.join(rd, arm)
                if os.path.isfile(os.path.join(ad, "writer.txt")):
                    runs.setdefault(row, {}).setdefault(arm, {})[p] = \
                        run_metrics(ad)
    print("# TASK-1118-WBT summary (passes %s: %s)\n" %
          (prefix, " ".join(passes)))
    flags = os.path.join(os.path.dirname(os.path.abspath(raw)), "flags.txt")
    if os.path.isfile(flags):
        print("## Bio flags per configuration (flags.txt, rwbs=count)\n")
        print("```")
        with open(flags) as f:
            print(f.read().rstrip())
        print("```\n")
    rows = [r for r in ROWS if r in runs] + sorted(
        r for r in runs if r not in ROWS)
    for row in rows:
        ra = runs[row]
        arms = [a for a in ARMS if a in ra] + sorted(
            a for a in ra if a not in ARMS)
        print("## Row %s\n" % row)
        print("Means over the passes, (min-max) of the per-run values.\n")
        cols = [("passes", None), ("fsync n", "f_n"),
                ("fsync mean ms", "f_mean"), ("fsync p50", "f_p50"),
                ("fsync p90", "f_p90"), ("fsync max", "f_max"),
                ("write p99 ms", "w_p99"), ("writer MB/s", "w_mbs"),
                ("read p50 ms", "r_p50"), ("read p99", "r_p99"),
                ("read MB/s", "r_mbs"), ("bulk MB/s", "b_mbs"),
                ("bulk write p99 ms", "b_p99"), ("PSI some %", "psi_some"),
                ("PSI full %", "psi_full"), ("busy CPU %", "busy")]
        print("| arm | " + " | ".join(c for c, _ in cols[1:]) +
              " | passes |")
        print("|---" * len(cols) + "|---|")
        for a in arms:
            rr = ra[a]
            print("| %s | " % a + " | ".join(
                span([rr[p][k] for p in rr]) for _, k in cols[1:]) +
                " | %d |" % len(rr))
        print()
        print("Pooled over all passes: fsync and read latency percentiles "
              "(ms).\n")
        print("| arm | fsync n | p50 | p90 | p99 | max | reads n | p50 |"
              " p99 | p99.9 | max |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for a in arms:
            fa = [x for p in ra[a] for x in ra[a][p]["f_all"]]
            rl = [x for p in ra[a] for x in ra[a][p]["r_all"]]
            print("| %s | %d | %s | %s | %s | %s | %d | %s | %s | %s | %s |"
                  % (a, len(fa), fmt(pct(fa, 50)), fmt(pct(fa, 90)),
                     fmt(pct(fa, 99)), fmt(max(fa) if fa else None),
                     len(rl), fmt(pct(rl, 50)), fmt(pct(rl, 99)),
                     fmt(pct(rl, 99.9)), fmt(max(rl) if rl else None)))
        print()
        print("Verdicts (geo ratio a/b, passes where a is better / passes,"
              " p perm, verdict).\n")
        print("| pair | " + " | ".join(lbl for _, lbl, _ in VERDICT_METRICS)
              + " |")
        print("|---" * (len(VERDICT_METRICS) + 1) + "|")
        for a, b in PAIRS:
            if a not in ra or b not in ra:
                continue
            cells = []
            for k, _, lower in VERDICT_METRICS:
                v = verdict(ra[a], ra[b], k, lower)
                cells.append("-" if v is None else "%.3f %d/%d p %.3f %s" % v)
            print("| %s/%s | " % (a, b) + " | ".join(cells) + " |")
        print()
        print("Threads asleep, mean number per sample tick: in blk-wbt "
              "(wbt_wait or rq_qos_wait on the stack) / in any "
              "uninterruptible sleep.  Times the measured seconds, it is "
              "the thread-seconds spent there.\n")
        print("| arm | " + " | ".join(CLASSES) + " | all in wbt |")
        print("|---" * (len(CLASSES) + 2) + "|")
        for a in arms:
            rr = ra[a]
            cells = []
            for c in CLASSES:
                cells.append("%s / %s" % (
                    fmt(mean([rr[p]["wbt"][c] for p in rr]), 3),
                    fmt(mean([rr[p]["d"][c] for p in rr]), 3)))
            print("| %s | " % a + " | ".join(cells) + " | %s |" %
                  fmt(mean([rr[p]["wbt_all"] for p in rr]), 3))
        print()
        print("z_wr_int stacks in blk-wbt, all passes (samples, first "
              "frames):\n")
        for a in arms:
            fr = {}
            for p in ra[a]:
                for k, n in ra[a][p]["frames"].items():
                    fr[k] = fr.get(k, 0) + n
            top = sorted(fr.items(), key=lambda x: -x[1])[:3]
            print("- %s: %s" % (a, "; ".join("%d %s" % (n, k)
                                              for k, n in top) or "none"))
        print()
        print("zpool iostat -w percentiles (ms), means over the passes.\n")
        hk = [("total_wait w p50", "h_tot_w_p50"),
              ("total_wait w p99", "h_tot_w_p99"),
              ("disk_wait w p99", "h_disk_w_p99"),
              ("disk_wait r p50", "h_disk_r_p50"),
              ("disk_wait r p99", "h_disk_r_p99"),
              ("syncq_wait w p99", "h_syncq_w_p99"),
              ("asyncq_wait w p99", "h_asyncq_w_p99")]
        print("| arm | " + " | ".join(lbl for lbl, _ in hk) + " |")
        print("|---" * (len(hk) + 1) + "|")
        for a in arms:
            rr = ra[a]
            print("| %s | " % a + " | ".join(
                fmt(mean([rr[p][k] for p in rr])) for _, k in hk) + " |")
        print()
        print("Devices, means over devices and passes: requests in flight "
              "(read, write, max write), writes blk-wbt tracks, its "
              "wb_background median and % of samples at 1 or less; "
              "diskstats r_await, w_await (ms), util %, write MB/s.\n")
        print("| arm | inflight r | inflight w | max w | wbt tracked |"
              " wb_background | <=1 % | r_await | w_await | util % |"
              " MB/s w |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for a in arms:
            rr = ra[a]

            def dm(key, src="dev"):
                return mean([v[key] for p in rr
                             for v in rr[p][src].values()])
            print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
                  % (a, fmt(dm("rd"), 2), fmt(dm("wr"), 2),
                     fmt(mean([max([v["wrmax"] for v in rr[p]["dev"]
                                    .values()] or [float("nan")])
                               for p in rr]), 1),
                     fmt(dm("trk"), 2), fmt(dm("bg_med"), 1),
                     fmt(dm("bg_le1"), 1), fmt(dm("r_await", "disk"), 1),
                     fmt(dm("w_await", "disk"), 1),
                     fmt(dm("util", "disk"), 1),
                     fmt(dm("w_mbs", "disk"), 1)))
        print()
        print("ZIL counters per measured second, means over the passes.\n")
        zk = ["zil_commit_count", "zil_commit_writer_count",
              "zil_itx_indirect_count", "zil_itx_copied_count",
              "zil_itx_needcopy_count", "zil_itx_metaslab_normal_count"]
        print("| arm | " + " | ".join(zk) + " |")
        print("|---" * (len(zk) + 1) + "|")
        for a in arms:
            rr = ra[a]
            print("| %s | " % a + " | ".join(
                fmt(mean([rr[p]["zil"].get(k, float("nan")) /
                          rr[p]["lay"]["meas"] for p in rr]), 1)
                for k in zk) + " |")
        print()
    print("## Noise and identity per run\n")
    print("| pass | row | arm | srcversion | sync_writes | wbt_lat_usec |"
          " steal % | load1 end | ticks | fsync n | reads n |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for p in passes:
        for row in rows:
            for a in ARMS + sorted(set(runs[row]) - set(ARMS)):
                m = runs[row].get(a, {}).get(p)
                if m is None:
                    continue
                lay = m["lay"]
                print("| %s | %s | %s | %s | %s | %s | %s | %s | %d | %d |"
                      " %d |" % (p, row, a, lay.get("srcversion", "?"),
                                 lay.get("sync_writes", "?"),
                                 ",".join(sorted(lay.get("wbt_lat", []))),
                                 fmt(m["steal"], 2), fmt(m["load1"], 2),
                                 m["ticks"], m["f_n"], m["r_n"]))
    print()
    print("## Arm order per pass\n")
    for p in passes:
        print("- %s: %s" % (p, order[p]))


if __name__ == "__main__":
    main(sys.argv)
