#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1118-VQ1 report.

  vq1-report.py RAW [PREFIX]

Reads RAW/<pass>/<arm>/ for every pass whose name starts with PREFIX
(default "P") and prints a Markdown summary: per arm the means over the
passes, the verdict of each pair of arms, ZFS's latency histograms and
request sizes over the measured window, the per-second vdev queues and
what the devices were doing meanwhile, blk-wbt's state, where the threads
slept, the transaction groups, the ZIL, and the noise.

Verdict of a pair a/b on a metric, over the passes where both ran:
ratio = a / b in the same pass; "better" when the geometric mean of the
ratios is at least 3 % on the good side (below 0.97 for a latency, above
1.03 for a throughput) and the exact sign-flip permutation p over the log
ratios is at most 0.05, "worse" when the same holds on the bad side,
otherwise "no difference shown".  Six passes are the fewest that can reach
p <= 0.05.  A metric that is 0 in either run of a pass leaves that pass out.
"""

import itertools
import math
import os
import re
import sys

ARMS = ["M", "P1", "P1A", "P1I"]
PAIRS = [("P1", "M"), ("P1A", "P1"), ("P1I", "P1"), ("P1A", "M"),
         ("P1I", "M")]
DEVS = ["vq0", "vq1", "vq2", "vq3", "vq4"]
CLASSES = ["z_wr_int", "z_wr_iss", "z_rd_int", "z_rd_iss", "z_null",
           "z_fr", "z_cl", "zvol", "txg_sync", "python3", "other"]
# metric, label, lower is better
VERDICT_METRICS = [
    ("f_mean", "fsync mean", True),
    ("f_p90", "fsync p90", True),
    ("r_p50", "read p50", True),
    ("r_p99", "read p99", True),
    ("w_mbs", "writer MB/s", False),
    ("b_mbs", "bulk MB/s", False),
    ("psi_some", "PSI io some", True),
    ("h_syncq_w_p99", "syncq_wait w p99", True),
    ("h_tot_w_p99", "total_wait w p99", True),
    ("h_disk_w_p99", "disk_wait w p99", True),
    ("h_disk_r_p99", "disk_wait r p99", True),
    ("dev_w_mbs", "device write MB/s", True),
    ("txg_stime", "txg sync mean", True),
]
LHIST = ["tot_r", "tot_w", "disk_r", "disk_w", "syncq_r", "syncq_w",
         "asyncq_r", "asyncq_w"]
RHIST = ["sr_i", "sr_a", "sw_i", "sw_a", "ar_i", "ar_a", "aw_i", "aw_a",
         "sc_i", "sc_a", "tr_i", "tr_a", "rb_i", "rb_a"]
# zpool iostat -Hpvlq row: name, 6 default, 11 latency, 14 queue columns
LQ_LAT = {"tot_r": 7, "tot_w": 8, "disk_r": 9, "disk_w": 10, "syncq_r": 11,
          "syncq_w": 12, "asyncq_r": 13, "asyncq_w": 14}
LQ_Q = {"sr_pend": 18, "sr_act": 19, "sw_pend": 20, "sw_act": 21,
        "ar_pend": 22, "ar_act": 23, "aw_pend": 24, "aw_act": 25}
LQ_FIELDS = 32
NAN = float("nan")


def isnan(x):
    return isinstance(x, float) and math.isnan(x)


def pct(vals, p):
    if not vals:
        return NAN
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int(math.ceil(p / 100.0 * len(s))) - 1))
    return s[k]


def fmt(v, nd=1):
    if v is None or isnan(v):
        return "-"
    return ("%%.%df" % nd) % v


def mean(xs):
    xs = [x for x in xs if x is not None and not isnan(x)]
    return sum(xs) / len(xs) if xs else NAN


def span(xs, nd=1):
    xs = [x for x in xs if x is not None and not isnan(x)]
    if not xs:
        return "-"
    return "%s (%s-%s)" % (fmt(mean(xs), nd), fmt(min(xs), nd),
                           fmt(max(xs), nd))


def num(s):
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return None


def read_layout(d):
    lay = {"ramp": 10.0, "meas": 60.0, "ivl": 0.25, "wbt_lat": set()}
    try:
        with open(os.path.join(d, "layout.txt")) as f:
            for line in f:
                p = line.split()
                if not p:
                    continue
                if p[0] == "window" and len(p) >= 7:
                    lay["ramp"] = float(p[2])
                    lay["meas"] = float(p[4])
                    lay["ivl"] = float(p[6])
                elif p[0] == "srcversion" and len(p) > 1:
                    lay["srcversion"] = p[1]
                elif p[0] == "prewarm" and len(p) > 1:
                    lay["prewarm"] = num(p[1])
                elif "=" in p[0] and len(p) == 1:
                    k, v = p[0].split("=", 1)
                    lay[k] = v
                elif p[0] in DEVS and "wbt_lat_usec" in p:
                    lay["wbt_lat"].add(p[p.index("wbt_lat_usec") + 1])
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
    except (OSError, ValueError):
        pass
    return out


def cls(comm):
    for c in CLASSES[:-1]:
        if comm.startswith(c):
            return c
    return "other"


def samples(path, lo, hi):
    """The sampler's file, restricted to sampler time [lo, hi)."""
    r = {"ticks": 0, "d": {c: 0 for c in CLASSES},
         "w": {c: 0 for c in CLASSES}, "dev": {}, "frames": {},
         "wall": [], "stat": {}}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) < 2:
                    continue
                t = float(p[1])
                if not lo <= t < hi:
                    continue
                if p[0] == "T":
                    r["ticks"] += 1
                    if len(p) >= 3:
                        r["wall"].append((t, float(p[2])))
                elif p[0] == "D" and len(p) >= 5:
                    c = cls(p[2])
                    r["d"][c] += 1
                    if p[3] == "1":
                        r["w"][c] += 1
                        if c == "z_wr_int":
                            r["frames"][p[4]] = r["frames"].get(p[4], 0) + 1
                elif p[0] == "I" and len(p) >= 9:
                    e = r["dev"].setdefault(p[2], {"rd": [], "wr": [],
                                                   "trk": [], "bg": [],
                                                   "nm": [], "t": []})
                    e["t"].append(t)
                    for k, i in (("rd", 3), ("wr", 4), ("trk", 5),
                                 ("nm", 6), ("bg", 7)):
                        if p[i] != "-":
                            e[k].append(int(p[i]))
                elif p[0] == "S" and len(p) >= 8 and p[3] != "-":
                    r["stat"].setdefault(p[2], []).append(
                        (t, [int(x) for x in p[3:8]]))
    except (OSError, ValueError):
        pass
    return r


def histo(path, names):
    """zpool iostat -wpH or -rpH: {column: {bucket: count}}"""
    h = {n: {} for n in names}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) < len(names) + 1 or not p[0].isdigit():
                    continue
                for i, n in enumerate(names):
                    v = p[i + 1]
                    h[n][int(p[0])] = int(v) if v.isdigit() else 0
    except OSError:
        pass
    return h


def hdelta(h0, h1):
    out = {}
    for n, b1 in h1.items():
        b0 = h0.get(n, {})
        out[n] = sorted((k, max(0, c - b0.get(k, 0))) for k, c in b1.items())
    return out


def hpct(b, p):
    """Percentile of a latency histogram, as its bucket's upper end, ms."""
    tot = sum(c for _, c in b)
    if tot == 0:
        return NAN
    acc = 0
    for v, c in b:
        acc += c
        if acc >= p / 100.0 * tot:
            return v / 1e6
    return b[-1][0] / 1e6


def snap(path):
    s = {"disk": {}, "zil": {}, "dmu_tx": {}, "arc": {}, "assign": {},
         "objset": {}}
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
                elif p[0] in ("zil", "dmu_tx", "arc", "assign") and \
                        len(p) == 3:
                    v = num(p[2])
                    if v is not None:
                        s[p[0]][p[1]] = v
                elif p[0] == "objset" and len(p) == 4:
                    o = s["objset"].setdefault(p[1], {})
                    o[p[2]] = num(p[3]) if p[2] != "dataset_name" else p[3]
    except (OSError, ValueError):
        pass
    return s


def txgs(path):
    """txgs kstat: {txg: (state, ndirty, nwritten, stime_ns)}"""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) != 12 or not p[0].isdigit() or len(p[2]) != 1:
                    continue
                out[int(p[0])] = (p[2], int(p[3]), int(p[5]), int(p[11]))
    except (OSError, ValueError):
        pass
    return out


def iostat_lq(path):
    """Stamped zpool iostat -T u -Hpvlq: [(wall, {leaf: {key: value}})],
    without its first block (the averages since the import)."""
    blocks = []
    cur = None
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 2 and p[1].isdigit():
                    cur = (float(p[0]), {})
                    blocks.append(cur)
                elif cur is not None and len(p) == LQ_FIELDS + 1 and \
                        p[1] in DEVS:
                    row = {}
                    for k, i in list(LQ_LAT.items()) + list(LQ_Q.items()):
                        v = num(p[i + 1])
                        row[k] = v if v is not None else 0
                    cur[1][p[1]] = row
    except (OSError, ValueError):
        pass
    return [b for b in blocks[1:] if b[1]]


def iostat_w1s(path):
    """Stamped zpool iostat -T u -wpH, one block per second:
    [{column: [(bucket, count)]}], without its first block (the counts
    since the import)."""
    blocks = []
    cur = None
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 2 and p[1].isdigit():
                    cur = {n: [] for n in LHIST}
                    blocks.append(cur)
                elif cur is not None and len(p) >= len(LHIST) + 2 and \
                        p[1].isdigit():
                    for i, n in enumerate(LHIST):
                        v = num(p[i + 2])
                        cur[n].append((int(p[1]), v if v else 0))
    except (OSError, ValueError):
        pass
    return [b for b in blocks[1:] if any(c for _, c in b["syncq_w"])]


def dev_util(stat, wall, w0, w1):
    """Busy % of a device over Unix times [w0, w1], from sampler ticks."""
    # sampler time -> wall time, and the ticks bracketing the interval
    if not stat or not wall:
        return NAN, NAN
    off = mean([w - t for t, w in wall])
    pts = [(t + off, v) for t, v in stat]
    a = [x for x in pts if x[0] <= w0]
    b = [x for x in pts if x[0] >= w1]
    if not a or not b:
        return NAN, NAN
    (ta, va), (tb, vb) = a[-1], b[0]
    if tb - ta <= 0:
        return NAN, NAN
    util = 100.0 * (vb[4] - va[4]) / ((tb - ta) * 1000.0)
    wmbs = (vb[3] - va[3]) * 512 / (tb - ta) / 1e6
    return min(100.0, util), wmbs


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
    m["f_mean"] = sum(fl) / len(fl) if fl else NAN
    m["f_p50"], m["f_p90"] = pct(fl, 50), pct(fl, 90)
    m["f_max"] = max(fl) if fl else NAN
    m["f_all"] = fl
    r = ops(os.path.join(d, "reader.txt"), lo, hi)
    rl = [ns / 1e6 for _, ns in r.get("R", [])]
    m["r_mbs"] = sum(b for b, _ in r.get("R", [])) / meas / 1e6
    m["r_n"] = len(rl)
    m["r_p50"], m["r_p99"] = pct(rl, 50), pct(rl, 99)
    m["r_all"] = rl
    b = ops(os.path.join(d, "bulk.txt"), lo, hi)
    bl = [ns / 1e6 for _, ns in b.get("W", [])]
    m["b_mbs"] = sum(x for x, _ in b.get("W", [])) / meas / 1e6
    m["b_p99"] = pct(bl, 99)

    sm = samples(os.path.join(d, "samples.txt"), lo, hi)
    ticks = sm["ticks"]
    m["ticks"] = ticks
    m["d"] = {c: sm["d"][c] / ticks if ticks else NAN for c in CLASSES}
    m["wbt"] = {c: sm["w"][c] / ticks if ticks else NAN for c in CLASSES}
    m["wbt_all"] = sum(m["wbt"][c] for c in CLASSES) if ticks else NAN
    m["frames"] = sm["frames"]
    dv = {}
    for k, e in sm["dev"].items():
        bg = e["bg"]
        dv[k] = {
            "rd": mean(e["rd"]), "wr": mean(e["wr"]),
            "wrmax": max(e["wr"]) if e["wr"] else NAN,
            "trk": mean(e["trk"]),
            "bg_med": pct(bg, 50) if bg else NAN,
            "bg_lt4": 100.0 * sum(1 for x in bg if x < 4) / len(bg)
            if bg else NAN,
            "bg_le1": 100.0 * sum(1 for x in bg if x <= 1) / len(bg)
            if bg else NAN,
        }
    m["dev"] = dv

    s0 = snap(os.path.join(d, "snap0.txt"))
    s1 = snap(os.path.join(d, "snap1.txt"))
    el = s1.get("time", 0) - s0.get("time", 0)
    m["elapsed"] = el
    rate = (lambda x: x / el) if el > 0 else (lambda x: NAN)

    for kind, names, pre in (("w", LHIST, "h_"), ("r", RHIST, "q_")):
        h = hdelta(histo(os.path.join(d, "iostat-%s0.txt" % kind), names),
                   histo(os.path.join(d, "iostat-%s1.txt" % kind), names))
        if kind == "w":
            for k in LHIST:
                m["h_%s_p50" % k] = hpct(h[k], 50)
                m["h_%s_p99" % k] = hpct(h[k], 99)
                m["h_%s_n" % k] = sum(c for _, c in h[k])
        else:
            for cl in ("sr", "sw", "ar", "aw"):
                n = sum(c for k in (cl + "_i", cl + "_a") for _, c in h[k])
                by = sum(v * c for k in (cl + "_i", cl + "_a")
                         for v, c in h[k])
                m["q_%s_ops" % cl] = rate(n)
                m["q_%s_mbs" % cl] = rate(by) / 1e6

    if "cpu" in s0 and "cpu" in s1:
        dc = [y - x for x, y in zip(s0["cpu"], s1["cpu"])]
        tot = sum(dc[:8]) or 1
        m["steal"] = 100.0 * dc[7] / tot if len(dc) > 7 else NAN
        m["busy"] = 100.0 * (tot - dc[3] - dc[4]) / tot
    else:
        m["steal"] = m["busy"] = NAN
    m["load1"] = s1.get("load1", NAN)
    for k in ("some", "full"):
        a, b2 = s0.get("psi_" + k), s1.get("psi_" + k)
        m["psi_" + k] = (100.0 * (b2 - a) / (el * 1e6)
                         if a is not None and b2 is not None and el > 0
                         else NAN)
    ds = {}
    for k in s1["disk"]:
        if k not in s0["disk"]:
            continue
        dx = [y - x for x, y in zip(s0["disk"][k], s1["disk"][k])]
        # reads, rmerged, rsect, rms, writes, wmerged, wsect, wms,
        # inflight, io_ms, weighted_ms, ...
        ds[k] = {
            "r_await": dx[3] / dx[0] if dx[0] else NAN,
            "w_await": dx[7] / dx[4] if dx[4] else NAN,
            "util": 100.0 * dx[9] / (el * 1000) if el > 0 else NAN,
            "w_mbs": dx[6] * 512 / el / 1e6 if el > 0 else NAN,
            "r_mbs": dx[2] * 512 / el / 1e6 if el > 0 else NAN,
        }
    m["disk"] = ds
    m["dev_w_mbs"] = sum(v["w_mbs"] for v in ds.values()) if ds else NAN
    m["dev_r_mbs"] = sum(v["r_mbs"] for v in ds.values()) if ds else NAN

    m["zil"] = {k: rate(v - s0["zil"].get(k, 0))
                for k, v in s1["zil"].items()}
    m["dmu_tx"] = {k: rate(v - s0["dmu_tx"].get(k, 0))
                   for k, v in s1["dmu_tx"].items()}
    a0, a1 = s0["arc"], s1["arc"]
    hit = a1.get("demand_data_hits", 0) - a0.get("demand_data_hits", 0)
    mis = a1.get("demand_data_misses", 0) - a0.get("demand_data_misses", 0)
    m["arc_ddhit"] = 100.0 * hit / (hit + mis) if hit + mis > 0 else NAN
    m["arc_size_mb"] = a1.get("size", 0) / 2**20 if "size" in a1 else NAN
    asg = {k: v - s0["assign"].get(k, 0) for k, v in s1["assign"].items()}
    tot = sum(asg.values())
    slow = sum(v for k, v in asg.items() if num(k) is not None and
               num(k) >= 1 << 20)
    m["assign_ge1ms"] = 100.0 * slow / tot if tot > 0 else NAN
    m["assign_n"] = rate(tot)
    ob = {}
    for o, v1 in s1["objset"].items():
        v0 = s0["objset"].get(o, {})
        name = v1.get("dataset_name", o)
        ob[name] = {k: rate(v - v0.get(k, 0)) for k, v in v1.items()
                    if isinstance(v, int)}
    m["objset"] = ob

    t0 = txgs(os.path.join(d, "txgs0.txt"))
    t1 = txgs(os.path.join(d, "txgs1.txt"))
    done = [v for k, v in t1.items() if v[0] == "C" and
            (k not in t0 or t0[k][0] != "C")]
    m["txg_n"] = len(done)
    m["txg_st"] = [v[3] / 1e6 for v in done]
    m["txg_stime"] = mean(m["txg_st"])
    m["txg_dirty"] = mean([v[1] / 2**20 for v in done])
    m["txg_written"] = mean([v[2] / 2**20 for v in done])

    # Per-second pool latency: each second's p99, then over the seconds.
    w1 = iostat_w1s(os.path.join(d, "iostat-w1s.txt"))
    for k in ("syncq_w", "tot_w", "disk_r"):
        v = [hpct(b[k], 99) for b in w1]
        v = [x for x in v if not isnan(x)]
        m["s1_%s_med" % k] = pct(v, 50)
        m["s1_%s_max" % k] = max(v) if v else NAN
    m["s1_syncq_w_n"] = len(w1)

    # Per-second queues per leaf, and the device's busy % over that second.
    lq = iostat_lq(os.path.join(d, "iostat-lq.txt"))
    swmax = num(lay.get("zfs_vdev_sync_write_max_active", "10") or "10")
    srmax = num(lay.get("zfs_vdev_sync_read_max_active", "10") or "10")
    rows = []
    for wall, leaves in lq:
        for leaf, row in leaves.items():
            util, wm = dev_util(sm["stat"].get(leaf), sm["wall"],
                                wall - 1.0, wall)
            rows.append((leaf, row, util, wm))
    m["lq_n"] = len(rows)
    pend = [x for x in rows if x[1]["sw_pend"] > 0]
    idle = [x for x in rows if x[1]["sw_pend"] == 0]
    m["lq_pend_pct"] = 100.0 * len(pend) / len(rows) if rows else NAN
    m["lq_sw_pend_mean"] = mean([x[1]["sw_pend"] for x in rows])
    m["lq_sw_pend_max"] = max([x[1]["sw_pend"] for x in rows] or [NAN])
    m["lq_sw_act_max"] = max([x[1]["sw_act"] for x in rows] or [NAN])
    m["lq_sw_over"] = sum(1 for x in rows if x[1]["sw_act"] > swmax)
    m["lq_sr_act_max"] = max([x[1]["sr_act"] for x in rows] or [NAN])
    m["lq_sr_over"] = sum(1 for x in rows if x[1]["sr_act"] > srmax)
    m["lq_aw_act_mean"] = mean([x[1]["aw_act"] for x in rows])
    m["lq_util_pend"] = mean([x[2] for x in pend])
    m["lq_util_idle"] = mean([x[2] for x in idle])
    up = [x[2] for x in pend if not isnan(x[2])]
    m["lq_pend_lt80"] = (100.0 * sum(1 for u in up if u < 80) / len(up)
                         if up else NAN)
    m["lq_syncq_w_pend"] = mean([x[1]["syncq_w"] / 1e6 for x in pend
                                 if x[1]["syncq_w"] > 0])
    m["lq_disk_w_pend"] = mean([x[1]["disk_w"] / 1e6 for x in pend
                                if x[1]["disk_w"] > 0])
    return m


def signflip_p(logs):
    n = len(logs)
    if n == 0:
        return NAN
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
        if a is None or b is None or isnan(a) or isnan(b) or \
                not a > 0 or not b > 0:
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


def table(head, rows):
    print("| " + " | ".join(head) + " |")
    print("|---" * len(head) + "|")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    print()


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    raw = argv[1]
    prefix = argv[2] if len(argv) > 2 else "P"
    passes = sorted(p for p in os.listdir(raw)
                    if p.startswith(prefix) and
                    os.path.isdir(os.path.join(raw, p)))
    ra = {}     # ra[arm][pass] = metrics
    order = {}
    for p in passes:
        try:
            with open(os.path.join(raw, p, "order.txt")) as f:
                order[p] = f.read().strip()
        except OSError:
            order[p] = "?"
        for arm in os.listdir(os.path.join(raw, p)):
            ad = os.path.join(raw, p, arm)
            if os.path.isfile(os.path.join(ad, "writer.txt")):
                ra.setdefault(arm, {})[p] = run_metrics(ad)
    arms = [a for a in ARMS if a in ra] + sorted(a for a in ra
                                                 if a not in ARMS)
    print("# TASK-1118-VQ1 summary (passes %s: %s)\n" %
          (prefix, " ".join(passes)))

    def col(a, key, nd=1):
        return span([ra[a][p].get(key) for p in ra[a]], nd)

    print("## Load and pressure\n")
    print("Means over the passes, (min-max) of the per-run values.\n")
    cols = [("fsync n", "f_n", 0), ("fsync mean ms", "f_mean", 1),
            ("fsync p50", "f_p50", 1), ("fsync p90", "f_p90", 1),
            ("fsync max", "f_max", 1), ("write p99 ms", "w_p99", 1),
            ("writer MB/s", "w_mbs", 1), ("read p50 ms", "r_p50", 1),
            ("read p99", "r_p99", 1), ("bulk MB/s", "b_mbs", 1),
            ("bulk write p99 ms", "b_p99", 1), ("PSI some %", "psi_some", 2),
            ("PSI full %", "psi_full", 2), ("busy CPU %", "busy", 1),
            ("ARC demand data hit %", "arc_ddhit", 1)]
    table(["arm"] + [c for c, _, _ in cols] + ["passes"],
          [[a] + [col(a, k, nd) for _, k, nd in cols] + [str(len(ra[a]))]
           for a in arms])

    print("Pooled over all passes: fsync and read latency percentiles "
          "(ms).\n")
    rows = []
    for a in arms:
        fa = [x for p in ra[a] for x in ra[a][p]["f_all"]]
        rl = [x for p in ra[a] for x in ra[a][p]["r_all"]]
        rows.append([a, str(len(fa)), fmt(pct(fa, 50)), fmt(pct(fa, 90)),
                     fmt(pct(fa, 99)), fmt(max(fa) if fa else None),
                     str(len(rl)), fmt(pct(rl, 50)), fmt(pct(rl, 99)),
                     fmt(pct(rl, 99.9)), fmt(max(rl) if rl else None)])
    table(["arm", "fsync n", "p50", "p90", "p99", "max", "reads n", "p50",
           "p99", "p99.9", "max"], rows)

    print("## Verdicts\n")
    print("Geo ratio a/b, passes where a is better / passes, p perm, "
          "verdict.  The histogram percentiles are log2 buckets.\n")
    rows = []
    for k, lbl, lower in VERDICT_METRICS:
        r = [lbl]
        for a, b in PAIRS:
            if a not in ra or b not in ra:
                r.append("-")
                continue
            v = verdict(ra[a], ra[b], k, lower)
            r.append("-" if v is None else "%.3f %d/%d p %.3f %s" % v)
        rows.append(r)
    table(["metric"] + ["%s/%s" % pr for pr in PAIRS], rows)

    print("## ZFS's latency over the measured window\n")
    print("zpool iostat -w histograms, end minus start of the window, pool "
          "level; ms, upper end of the log2 bucket; means over the "
          "passes.\n")
    hk = [("total_wait w p50", "h_tot_w_p50"),
          ("total_wait w p99", "h_tot_w_p99"),
          ("disk_wait w p50", "h_disk_w_p50"),
          ("disk_wait w p99", "h_disk_w_p99"),
          ("syncq_wait w p50", "h_syncq_w_p50"),
          ("syncq_wait w p99", "h_syncq_w_p99"),
          ("asyncq_wait w p99", "h_asyncq_w_p99"),
          ("total_wait r p99", "h_tot_r_p99"),
          ("disk_wait r p50", "h_disk_r_p50"),
          ("disk_wait r p99", "h_disk_r_p99"),
          ("syncq_wait r p99", "h_syncq_r_p99")]
    table(["arm"] + [lbl for lbl, _ in hk],
          [[a] + [fmt(mean([ra[a][p][k] for p in ra[a]])) for _, k in hk]
           for a in arms])

    print("Leaf requests per second and MB/s by class (zpool iostat -r "
          "deltas, individual plus aggregated; MB/s from the lower end of "
          "each size bucket, so a floor), and the devices' own write and "
          "read MB/s (diskstats, sum of the five).\n")
    qk = [("sync_read ops/s", "q_sr_ops"), ("sync_read MB/s", "q_sr_mbs"),
          ("sync_write ops/s", "q_sw_ops"), ("sync_write MB/s", "q_sw_mbs"),
          ("async_read ops/s", "q_ar_ops"),
          ("async_write ops/s", "q_aw_ops"),
          ("async_write MB/s", "q_aw_mbs"), ("device write MB/s",
                                             "dev_w_mbs"),
          ("device read MB/s", "dev_r_mbs")]
    table(["arm"] + [lbl for lbl, _ in qk],
          [[a] + [col(a, k) for _, k in qk] for a in arms])

    print("## Per-second vdev queues and the devices\n")
    print("zpool iostat -l -q every second, per leaf (a leaf-second); queue "
          "sizes are the instant of the report, latencies the second's "
          "averages.  Busy % is the device's io_ticks over the preceding "
          "second, from the sampler's ticks (0.25 s), so a leaf-second is "
          "matched to within a tick.  An active count above "
          "zfs_vdev_sync_*_max_active can only come from the age guard.\n")
    lk = [("leaf-s", "lq_n", 0), ("with sync w pend %", "lq_pend_pct", 1),
          ("sync w pend mean", "lq_sw_pend_mean", 2),
          ("pend max", "lq_sw_pend_max", 0),
          ("sync w activ max", "lq_sw_act_max", 0),
          ("leaf-s sync w activ > max", "lq_sw_over", 0),
          ("sync r activ max", "lq_sr_act_max", 0),
          ("leaf-s sync r activ > max", "lq_sr_over", 0),
          ("async w activ mean", "lq_aw_act_mean", 2)]
    table(["arm"] + [lbl for lbl, _, _ in lk],
          [[a] + [col(a, k, nd) for _, k, nd in lk] for a in arms])
    print("The study's decisive reading: in the leaf-seconds with sync "
          "writes pending, how busy was that device?  Below about 80 % "
          "while sync writes wait means the code, not the device, held "
          "them.\n")
    dk = [("busy % with sync w pend", "lq_util_pend"),
          ("busy % without", "lq_util_idle"),
          ("leaf-s pend with busy < 80 %", "lq_pend_lt80"),
          ("syncq_wait w ms when pend", "lq_syncq_w_pend"),
          ("disk_wait w ms when pend", "lq_disk_w_pend")]
    table(["arm"] + [lbl for lbl, _ in dk],
          [[a] + [col(a, k) for _, k in dk] for a in arms])

    print("Per-second pool latency (zpool iostat -w every second): the "
          "median and the maximum over the seconds of each second's p99 "
          "(ms, log2 buckets); seconds counted are those with sync "
          "writes.\n")
    sk = [("seconds", "s1_syncq_w_n", 0),
          ("syncq_wait w p99 median", "s1_syncq_w_med", 1),
          ("syncq_wait w p99 max", "s1_syncq_w_max", 1),
          ("total_wait w p99 median", "s1_tot_w_med", 1),
          ("total_wait w p99 max", "s1_tot_w_max", 1),
          ("disk_wait r p99 median", "s1_disk_r_med", 1),
          ("disk_wait r p99 max", "s1_disk_r_max", 1)]
    table(["arm"] + [lbl for lbl, _, _ in sk],
          [[a] + [col(a, k, nd) for _, k, nd in sk] for a in arms])

    print("## blk-wbt and the device queues\n")
    print("Sampler, means over devices and passes: requests in flight "
          "(read, write, max write), writes blk-wbt tracks, wb_background "
          "median, % of samples with wb_background below 4 (its unscaled "
          "value at depth 16: scaled down) and at 1 or less; diskstats "
          "r_await, w_await (ms), util %, write MB/s per device.\n")
    rows = []
    for a in arms:
        rr = ra[a]

        def dm(key, src="dev"):
            return mean([v[key] for p in rr for v in rr[p][src].values()])
        rows.append([a, fmt(dm("rd"), 2), fmt(dm("wr"), 2),
                     fmt(mean([max([v["wrmax"] for v in rr[p]["dev"]
                                    .values()] or [NAN]) for p in rr]), 1),
                     fmt(dm("trk"), 2), fmt(dm("bg_med"), 1),
                     fmt(dm("bg_lt4"), 1), fmt(dm("bg_le1"), 1),
                     fmt(dm("r_await", "disk"), 1),
                     fmt(dm("w_await", "disk"), 1),
                     fmt(dm("util", "disk"), 1),
                     fmt(dm("w_mbs", "disk"), 1)])
    table(["arm", "inflight r", "inflight w", "max w", "wbt tracked",
           "wb_background", "< 4 %", "<= 1 %", "r_await", "w_await",
           "util %", "MB/s w"], rows)

    print("Threads asleep, mean number per sample tick: in blk-wbt "
          "(wbt_wait or rq_qos_wait on the stack) / in any uninterruptible "
          "sleep.\n")
    table(["arm"] + CLASSES + ["all in wbt"],
          [[a] + ["%s / %s" % (fmt(mean([ra[a][p]["wbt"][c]
                                         for p in ra[a]]), 3),
                               fmt(mean([ra[a][p]["d"][c]
                                         for p in ra[a]]), 3))
                  for c in CLASSES] +
           [fmt(mean([ra[a][p]["wbt_all"] for p in ra[a]]), 3)]
           for a in arms])
    print("z_wr_int stacks in blk-wbt, all passes (samples, first "
          "frames):\n")
    for a in arms:
        fr = {}
        for p in ra[a]:
            for k, n in ra[a][p]["frames"].items():
                fr[k] = fr.get(k, 0) + n
        top = sorted(fr.items(), key=lambda x: -x[1])[:3]
        txt = "; ".join("%d %s" % (n, k) for k, n in top)
        print("- %s: %s" % (a, txt or "none"))
    print()

    print("## Transaction groups, dmu_tx, ZIL\n")
    print("Txgs committed during the window (txgs kstat): count per run, "
          "sync time mean and pooled p95 (ms), dirty and written MiB per "
          "txg; tx assigns per second and % that took 1 ms or more "
          "(dmu_tx_assign, log2 buckets).\n")
    rows = []
    for a in arms:
        st = [x for p in ra[a] for x in ra[a][p]["txg_st"]]
        rows.append([a, col(a, "txg_n"), col(a, "txg_stime", 0),
                     fmt(pct(st, 95), 0), col(a, "txg_dirty"),
                     col(a, "txg_written"), col(a, "assign_n", 0),
                     col(a, "assign_ge1ms")])
    table(["arm", "txgs", "stime mean ms", "stime p95", "dirty MiB",
           "written MiB", "assigns/s", "assign >= 1 ms %"], rows)
    txk = sorted({k for a in arms for p in ra[a]
                  for k, v in ra[a][p]["dmu_tx"].items() if v})
    if txk:
        print("dmu_tx counters per second (non-zero ones).\n")
        table(["arm"] + txk,
              [[a] + [fmt(mean([ra[a][p]["dmu_tx"].get(k, NAN)
                                for p in ra[a]]), 1) for k in txk]
               for a in arms])
    zk = ["zil_commit_count", "zil_commit_writer_count", "zil_itx_count",
          "zil_itx_indirect_count", "zil_itx_indirect_bytes",
          "zil_itx_needcopy_count", "zil_itx_needcopy_bytes",
          "zil_itx_copied_count", "zil_itx_metaslab_normal_count",
          "zil_itx_metaslab_normal_bytes"]
    print("ZIL counters per second (global kstat; bytes as MB).\n")
    table(["arm"] + zk,
          [[a] + [fmt(mean([ra[a][p]["zil"].get(k, NAN) /
                            (1e6 if k.endswith("_bytes") else 1)
                            for p in ra[a]]), 1) for k in zk]
           for a in arms])
    print("Per dataset (objset kstats), per second: commits, indirect "
          "and need-copy records.\n")
    names = sorted({n for a in arms for p in ra[a]
                    for n in ra[a][p]["objset"]})
    rows = []
    for a in arms:
        for n in names:
            v = [ra[a][p]["objset"].get(n, {}) for p in ra[a]]
            rows.append([a, n] + [fmt(mean([x.get(k, NAN) for x in v]), 1)
                                  for k in ("zil_commit_count",
                                            "zil_itx_indirect_count",
                                            "zil_itx_needcopy_count",
                                            "writes")])
    table(["arm", "dataset", "commits", "indirect", "needcopy", "writes"],
          rows)

    print("## Rig checks\n")
    print("Per arm, means over the passes: blk-wbt scaled down (% of "
          "samples with wb_background < 4 and <= 1, means over devices), "
          "the age guard (leaf-seconds with a sync class active above its "
          "maximum, and the largest sync write active count), the ZIL's "
          "record kinds per second, and the ARC hit rate of demand data "
          "reads.\n")
    rows = []
    for a in arms:
        rr = ra[a]
        rows.append([a,
                     fmt(mean([v["bg_lt4"] for p in rr
                               for v in rr[p]["dev"].values()])),
                     fmt(mean([v["bg_le1"] for p in rr
                               for v in rr[p]["dev"].values()])),
                     fmt(sum(rr[p]["lq_sw_over"] + rr[p]["lq_sr_over"]
                             for p in rr), 0),
                     fmt(max([rr[p]["lq_sw_act_max"] for p in rr]), 0),
                     fmt(mean([rr[p]["zil"].get("zil_itx_indirect_count",
                                                NAN) for p in rr])),
                     fmt(mean([rr[p]["zil"].get("zil_itx_needcopy_count",
                                                NAN) for p in rr])),
                     fmt(mean([rr[p]["arc_ddhit"] for p in rr]))])
    table(["arm", "wb_bg < 4 %", "wb_bg <= 1 %", "leaf-s above max",
           "sync w activ max", "indirect/s", "needcopy/s",
           "ARC dd hit %"], rows)

    print("## Noise and identity per run\n")
    rows = []
    for p in passes:
        for a in arms:
            m = ra[a].get(p)
            if m is None:
                continue
            lay = m["lay"]
            rows.append([p, a, lay.get("srcversion", "?"),
                         lay.get("zfs_vdev_disk_sync_writes", "?"),
                         lay.get("zfs_vdev_queue_age_ms", "?"),
                         lay.get("zfs_immediate_write_sz", "?"),
                         ",".join(sorted(lay["wbt_lat"])),
                         fmt(lay.get("prewarm"), 2), fmt(m["steal"], 2),
                         fmt(m["load1"], 2), str(m["ticks"]),
                         str(m["lq_n"]), str(m["f_n"]), str(m["r_n"])])
    table(["pass", "arm", "srcversion", "sync_writes", "age_ms",
           "immediate_write_sz", "wbt_lat_usec", "prewarm s", "steal %",
           "load1 end", "ticks", "leaf-s", "fsync n", "reads n"], rows)
    print("## Arm order per pass\n")
    for p in passes:
        print("- %s: %s" % (p, order[p]))


if __name__ == "__main__":
    main(sys.argv)
