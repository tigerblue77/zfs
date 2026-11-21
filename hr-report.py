#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Host-rig report and calibration verdict.

  hr-report.py calib RAW GRID_PASS [CONFIRM_PASS ...]
      One row per run of RAW/<pass>/<conf>/: the rig's numbers next to the
      host's, each criterion's verdict, and the score.  With GRID_PASS
      alone, the last line is "BEST <conf> ..." (the point to confirm).
      With confirmation passes, the verdict on the chosen point over its
      runs (median of each metric), the last line "CALIBRATED <conf>" or
      "NOT CALIBRATED <conf>: <criteria that failed>".

The criteria and the selection rule were fixed before any run; they are
the constants below.  Histogram percentiles are reported as the upper end
of their log2 bucket, in ms, as zpool iostat -w labels them and as the
host collector reported them (1074 = 2^30 ns, the bucket 537-1074 ms).
"""

import math
import os
import re
import statistics
import sys

DEVS = ["ublkb70", "ublkb71", "ublkb72", "ublkb73", "ublkb74"]
NAN = float("nan")

# The host, DS_HDD_R6_1, WBT on (collector run 18/11 17:27-17:37Z,
# host-iowait/run-20261118T1737Z-summary.txt; sync-queue p50/p99 from the
# owner's commit b55aa658caa; z_wr_int share from the blocked-task sampler)
HOST = {
    "util": 22.0, "disk_r_p99": 1073.7, "syncq_w_p99": 4295.0,
    "syncq_w_p50": 0.0, "disk_w_p99": 268.4, "psi_some": 8.12,
    "txg_mean": 0.77, "txg_p95": 2.76, "r_await": 36.5, "w_await": 9.0,
    "flush_rate": 0.9, "flush_ms": 55.0, "sync_share": 36.9,
    "wbt_zwrint": 7.2, "guest_w_mbs": 51.3, "commits": 0.6,
    "itx_per_w": 148 / 104.0, "amp": 120.8 / 51.3, "pool_r_mbs": 9.8,
}
OFFERED_W_MBS = 50.0


def bidx(ms):
    """log2 bucket index j of a label in ms, (2^(j+1) - 1) ns."""
    if ms is None or isnan(ms) or ms <= 0:
        return None
    return int(round(math.log2(ms * 1e6 + 1))) - 1


# Primary: all must hold.  Workload shape: all must hold.  Secondary: at
# least 3 of 5.  Q1: the rig builds a sync-write queue.
PRIMARY = [
    ("P1", "disk util % (diskstats)", "util", lambda v: 15 <= v <= 30),
    ("P2", "disk_wait r p99 ms", "h_disk_r_p99",
     lambda v: bidx(v) is not None and abs(bidx(v) - 29) <= 1),
    ("P3", "syncq_wait w p99 ms", "h_syncq_w_p99",
     lambda v: bidx(v) is not None and abs(bidx(v) - 31) <= 1),
    ("P3b", "syncq_wait w p50 ms", "h_syncq_w_p50", lambda v: v <= 1.05),
    ("P4", "disk_wait w p99 ms", "h_disk_w_p99",
     lambda v: bidx(v) is not None and abs(bidx(v) - 27) <= 1),
    ("P5", "PSI io some %", "psi_some", lambda v: 4 <= v <= 12),
]
WORKLOAD = [
    ("W1", "guest media MB/s / offered", "w_ratio", lambda v: v >= 0.9),
    ("W2", "ZIL commits/s", "commits", lambda v: 0.45 <= v <= 0.8),
    ("W3", "indirect itx per guest write", "itx_per_w",
     lambda v: 1.1 <= v <= 1.8),
    ("W4", "pool write MB/s / guest write MB/s", "amp",
     lambda v: 1.8 <= v <= 3.0),
    ("W5", "pool read MB/s", "pool_r_mbs", lambda v: 5 <= v <= 20),
]
SECONDARY = [
    ("S1", "txg sync mean / p95 s", ("txg_mean", "txg_p95"),
     lambda a, b: 0.4 <= a <= 1.5 and 1.4 <= b <= 5.5),
    ("S2", "await r / w ms", ("r_await", "w_await"),
     lambda a, b: 18 <= a <= 60 and 4 <= b <= 20),
    ("S3", "flushes/s/disk, mean ms", ("flush_rate", "flush_ms"),
     lambda a, b: 0.5 <= a <= 1.5 and 25 <= b <= 110),
    ("S4", "sync share of leaf writes %", ("sync_share",),
     lambda a: 25 <= a <= 50),
    ("S5", "z_wr_int in blk-wbt, % of D samples", ("wbt_zwrint",),
     lambda a: 2 <= a <= 20),
]
QUEUE = ("Q1", "leaf-seconds with sync writes queued %, max queued",
         ("lq_pend_pct", "lq_sw_pend_max"), lambda a, b: a >= 5 and b >= 10)
VALID = ("V1", "disk completions late > 2 ms %, store errors",
         ("late_gt2", "store_err"), lambda a, b: a <= 1.0 and b == 0)

LHIST = ["tot_r", "tot_w", "disk_r", "disk_w", "syncq_r", "syncq_w",
         "asyncq_r", "asyncq_w"]
RHIST = ["sr_i", "sr_a", "sw_i", "sw_a", "ar_i", "ar_a", "aw_i", "aw_a",
         "sc_i", "sc_a", "tr_i", "tr_a", "rb_i", "rb_a"]
LQ_LAT = {"tot_r": 7, "tot_w": 8, "disk_r": 9, "disk_w": 10, "syncq_r": 11,
          "syncq_w": 12, "asyncq_r": 13, "asyncq_w": 14}
LQ_Q = {"sr_pend": 18, "sr_act": 19, "sw_pend": 20, "sw_act": 21,
        "ar_pend": 22, "ar_act": 23, "aw_pend": 24, "aw_act": 25}
LQ_FIELDS = 32
CLASSES = ["z_wr_int", "z_wr_iss", "z_rd_int", "z_rd_iss", "z_null",
           "z_fr", "z_cl", "zvol", "txg_sync", "python3", "other"]


def isnan(x):
    return isinstance(x, float) and math.isnan(x)


def pct(vals, p):
    if not vals:
        return NAN
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int(math.ceil(p / 100.0 * len(s))) - 1))
    return s[k]


def mean(xs):
    xs = [x for x in xs if x is not None and not isnan(x)]
    return sum(xs) / len(xs) if xs else NAN


def fmt(v, nd=1):
    if v is None or isnan(v):
        return "-"
    return ("%%.%df" % nd) % v


def num(s):
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return None


def histo(path, names):
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
    s = {"disk": {}, "zil": {}, "dmu_tx": {}, "arc": {}, "assign": {}}
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
    except (OSError, ValueError):
        pass
    return s


def txgs(path):
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


def samples(path, lo, hi):
    r = {"ticks": 0, "d": {c: 0 for c in CLASSES},
         "w": {c: 0 for c in CLASSES}, "dsum": 0, "bg": [], "wall": [],
         "stat": {}}
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
                    c = next((x for x in CLASSES[:-1]
                              if p[2].startswith(x)), "other")
                    r["d"][c] += 1
                    r["dsum"] += 1
                    if p[3] == "1":
                        r["w"][c] += 1
                elif p[0] == "I" and len(p) >= 9 and p[7] != "-":
                    r["bg"].append(int(p[7]))
                elif p[0] == "S" and len(p) >= 8 and p[3] != "-":
                    r["stat"].setdefault(p[2], []).append(
                        (t, [int(x) for x in p[3:8]]))
    except (OSError, ValueError):
        pass
    return r


def dev_util(stat, wall, w0, w1):
    if not stat or not wall:
        return NAN
    off = mean([w - t for t, w in wall])
    pts = [(t + off, v) for t, v in stat]
    a = [x for x in pts if x[0] <= w0]
    b = [x for x in pts if x[0] >= w1]
    if not a or not b:
        return NAN
    (ta, va), (tb, vb) = a[-1], b[0]
    if tb - ta <= 0:
        return NAN
    return min(100.0, 100.0 * (vb[4] - va[4]) / ((tb - ta) * 1000.0))


def guest(path, lo, hi):
    out = {}
    end = {}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p or p[0].startswith("#"):
                    continue
                if p[0] == "END":
                    for x in p[1:]:
                        k, n, nb, sk = x.split(":")
                        end[k] = (int(n), int(nb), int(sk))
                    continue
                if len(p) != 4 or p[0] == "ERROR":
                    continue
                t = float(p[0])
                if lo <= t < hi:
                    out.setdefault(p[1], []).append((int(p[2]), int(p[3])))
    except (OSError, ValueError):
        pass
    return out, end


def simstats(path, w0, w1):
    """hddsim statistics lines inside [w0, w1] (wall time)."""
    r = {"n": 0, "busy_us": 0, "f_n": 0, "flx": 0, "dirty_max": 0,
         "late": [0] * 26, "err": 0, "rej": 0, "full": 0, "models": set(),
         "late_max": 0}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) < 6 or p[0] != "S":
                    continue
                wall = float(p[2])
                if not w0 <= wall <= w1:
                    continue
                r["n"] += 1
                r["models"].add(p[4])
                i = 5
                ops = {}
                for _ in range(4):
                    ops[p[i]] = [int(x) for x in p[i + 1:i + 5]]
                    i += 5
                r["f_n"] += ops["f"][0]
                key = None
                for tok in p[i:]:
                    # a word is a field's name, numbers and b:n its values;
                    # a field this parser does not know is skipped
                    if not tok.isdigit() and ":" not in tok:
                        key = tok
                        vals = []
                        continue
                    if key is None:
                        continue
                    if key == "hl" and ":" in tok:
                        b, c = tok.split(":")
                        r["late"][int(b)] += int(c)
                        continue
                    if ":" in tok:
                        continue
                    vals.append(int(tok))
                    if key == "busy_us" and len(vals) == 1:
                        r["busy_us"] += vals[0]
                    elif key == "flx" and len(vals) == 1:
                        r["flx"] += vals[0]
                    elif key == "dirty" and len(vals) == 2:
                        r["dirty_max"] = max(r["dirty_max"], vals[1])
                    elif key == "late_max_us" and len(vals) == 1:
                        r["late_max"] = max(r["late_max"], vals[0])
                    elif key == "full" and len(vals) == 1:
                        r["full"] = vals[0]
                    elif key == "err" and len(vals) == 1:
                        r["err"] = max(r["err"], vals[0])
                    elif key == "rej" and len(vals) == 1:
                        r["rej"] = max(r["rej"], vals[0])
    except (OSError, ValueError, KeyError, IndexError):
        pass
    return r


def run_metrics(d, devdir):
    lay = {"ramp": 60.0, "meas": 300.0}
    try:
        with open(os.path.join(d, "layout.txt")) as f:
            for line in f:
                p = line.split()
                if p and p[0] == "window" and len(p) >= 7:
                    lay["ramp"], lay["meas"] = float(p[2]), float(p[4])
                elif p and "=" in p[0] and len(p) == 1:
                    k, v = p[0].split("=", 1)
                    lay[k] = v
                elif p and p[0] == "srcversion":
                    lay["srcversion"] = p[1]
                elif p and p[0] == "pool":
                    lay["pool"] = " ".join(p[1:])
    except OSError:
        pass
    lo, hi = lay["ramp"], lay["ramp"] + lay["meas"]
    meas = lay["meas"]
    m = {"lay": lay}
    g, end = guest(os.path.join(d, "guest.txt"), lo, hi)
    m["w_mbs"] = sum(b for b, _ in g.get("W", [])) / meas / 1e6
    m["w_ratio"] = m["w_mbs"] / OFFERED_W_MBS
    m["skipped"] = sum(v[2] for v in end.values())
    m["guest_ops"] = sum(len(g.get(k, [])) for k in "WADJ") / meas
    m["guest_wmbs_all"] = sum(b for k in "WADJ" for b, _ in g.get(k, [])) \
        / meas / 1e6
    fs = [ns / 1e6 for k in "SK" for _, ns in g.get(k, [])]
    m["fsync_n"] = len(fs)
    for q in (50, 90, 99):
        m["fsync_p%d" % q] = pct(fs, q)
    m["fsync_mean"] = mean(fs)
    m["fsync_max"] = max(fs) if fs else NAN
    rl = [ns / 1e6 for _, ns in g.get("R", [])]
    m["read_p50"], m["read_p99"] = pct(rl, 50), pct(rl, 99)
    wl = [ns / 1e6 for _, ns in g.get("W", [])]
    m["wlat_p99"] = pct(wl, 99)
    tl = [ns / 1e6 for _, ns in g.get("T", [])]
    m["txn_p99"] = pct(tl, 99)

    s0 = snap(os.path.join(d, "snap0.txt"))
    s1 = snap(os.path.join(d, "snap1.txt"))
    el = s1.get("time", 0) - s0.get("time", 0)
    m["elapsed"] = el
    rate = (lambda x: x / el) if el > 0 else (lambda x: NAN)
    h = hdelta(histo(os.path.join(d, "iostat-w0.txt"), LHIST),
               histo(os.path.join(d, "iostat-w1.txt"), LHIST))
    for k in LHIST:
        m["h_%s_p50" % k] = hpct(h[k], 50)
        m["h_%s_p99" % k] = hpct(h[k], 99)
    q = hdelta(histo(os.path.join(d, "iostat-r0.txt"), RHIST),
               histo(os.path.join(d, "iostat-r1.txt"), RHIST))
    ops = {cl: sum(c for k in (cl + "_i", cl + "_a") for _, c in q[k])
           for cl in ("sr", "sw", "ar", "aw")}
    wtot = ops["sw"] + ops["aw"]
    m["sync_share"] = 100.0 * ops["sw"] / wtot if wtot else NAN
    for k in ("some", "full"):
        a, b = s0.get("psi_" + k), s1.get("psi_" + k)
        m["psi_" + k] = (100.0 * (b - a) / (el * 1e6)
                         if a is not None and b is not None and el > 0
                         else NAN)
    if "cpu" in s0 and "cpu" in s1:
        dc = [y - x for x, y in zip(s0["cpu"], s1["cpu"])]
        tot = sum(dc[:8]) or 1
        m["steal"] = 100.0 * dc[7] / tot if len(dc) > 7 else NAN
        m["iowait"] = 100.0 * dc[4] / tot
    else:
        m["steal"] = m["iowait"] = NAN
    m["load1"] = s1.get("load1", NAN)
    ds = []
    for k in DEVS:
        if k not in s0["disk"] or k not in s1["disk"]:
            continue
        dx = [y - x for x, y in zip(s0["disk"][k], s1["disk"][k])]
        ds.append({
            "r_await": dx[3] / dx[0] if dx[0] else NAN,
            "w_await": dx[7] / dx[4] if dx[4] else NAN,
            "util": 100.0 * dx[9] / (el * 1000) if el > 0 else NAN,
            "w_mbs": dx[6] * 512 / el / 1e6 if el > 0 else NAN,
            "r_mbs": dx[2] * 512 / el / 1e6 if el > 0 else NAN,
            "fl_n": dx[15] if len(dx) > 16 else NAN,
            "fl_ms": dx[16] if len(dx) > 16 else NAN,
        })
    m["util"] = mean([x["util"] for x in ds])
    m["r_await"] = mean([x["r_await"] for x in ds])
    m["w_await"] = mean([x["w_await"] for x in ds])
    m["pool_w_mbs"] = sum(x["w_mbs"] for x in ds) if ds else NAN
    m["pool_r_mbs"] = sum(x["r_mbs"] for x in ds) if ds else NAN
    fl_n = sum(x["fl_n"] for x in ds) if ds else NAN
    m["flush_rate"] = fl_n / len(ds) / el if ds and el > 0 else NAN
    m["flush_ms"] = (sum(x["fl_ms"] for x in ds) / fl_n
                     if ds and fl_n else NAN)
    m["amp"] = (m["pool_w_mbs"] / (m["guest_wmbs_all"] or NAN)
                if m["guest_wmbs_all"] else NAN)
    zil = {k: v - s0["zil"].get(k, 0) for k, v in s1["zil"].items()}
    m["commits"] = rate(zil.get("zil_commit_count", 0))
    nw = len(g.get("W", [])) + sum(len(g.get(k, [])) for k in "ADJ")
    m["itx_per_w"] = (zil.get("zil_itx_indirect_count", 0) / nw
                      if nw else NAN)
    m["itx_needcopy"] = rate(zil.get("zil_itx_needcopy_count", 0))
    dtx = {k: v - s0["dmu_tx"].get(k, 0) for k, v in s1["dmu_tx"].items()}
    m["tx_delay"] = rate(dtx.get("dmu_tx_delay", 0))
    t0 = txgs(os.path.join(d, "txgs0.txt"))
    t1 = txgs(os.path.join(d, "txgs1.txt"))
    done = [v for k, v in t1.items() if v[0] == "C" and
            (k not in t0 or t0[k][0] != "C")]
    st = sorted(v[3] / 1e9 for v in done)
    m["txg_n"] = len(st)
    m["txg_mean"] = mean(st)
    m["txg_p95"] = pct(st, 95)

    sm = samples(os.path.join(d, "samples.txt"), lo, hi)
    m["wbt_zwrint"] = (100.0 * sm["w"]["z_wr_int"] / sm["dsum"]
                       if sm["dsum"] else NAN)
    m["wbt_all_per_tick"] = (sum(sm["w"].values()) / sm["ticks"]
                             if sm["ticks"] else NAN)
    m["wb_bg_lt4"] = (100.0 * sum(1 for x in sm["bg"] if x < 4) /
                      len(sm["bg"]) if sm["bg"] else NAN)
    lq = iostat_lq(os.path.join(d, "iostat-lq.txt"))
    rows = [(leaf, row, dev_util(sm["stat"].get(leaf), sm["wall"],
                                 wall - 1.0, wall))
            for wall, leaves in lq for leaf, row in leaves.items()]
    pend = [x for x in rows if x[1]["sw_pend"] > 0]
    m["lq_pend_pct"] = 100.0 * len(pend) / len(rows) if rows else NAN
    m["lq_sw_pend_max"] = max([x[1]["sw_pend"] for x in rows] or [NAN])
    m["lq_util_pend"] = mean([x[2] for x in pend])
    up = [x[2] for x in pend if not isnan(x[2])]
    m["lq_pend_lt80"] = (100.0 * sum(1 for u in up if u < 80) / len(up)
                         if up else NAN)

    w0, w1 = s0.get("time", 0), s1.get("time", 0)
    late = [0] * 26
    m["store_err"] = 0
    m["refused"] = 0
    m["models"] = set()
    busy = []
    fexec = []
    for dv in DEVS:
        ss = simstats(os.path.join(devdir, dv[5:] + ".stats"), w0, w1)
        late = [a + b for a, b in zip(late, ss["late"])]
        m["store_err"] = max(m["store_err"], ss["err"])
        m["refused"] += ss["rej"]
        m["models"] |= ss["models"]
        busy.append(100.0 * ss["busy_us"] / 1e6 / el if el > 0 else NAN)
        fexec.append(ss["flx"] / ss["f_n"] / 1e3 if ss["f_n"] else NAN)
    tot = sum(late)
    # bucket b holds lateness of 2^b us up to 2^(b+1) us: > 2 ms is b >= 11
    m["late_gt2"] = 100.0 * sum(late[11:]) / tot if tot else NAN
    m["media_busy"] = mean(busy)
    m["flush_exec_ms"] = mean(fexec)
    return m


def verdicts(m):
    out = []
    for cid, name, key, ok in PRIMARY + WORKLOAD:
        v = m.get(key, NAN)
        out.append((cid, name, fmt(v, 2), not isnan(v) and ok(v)))
    sec = 0
    for cid, name, keys, ok in SECONDARY:
        vs = [m.get(k, NAN) for k in keys]
        good = all(not isnan(v) for v in vs) and ok(*vs)
        sec += good
        out.append((cid, name, " / ".join(fmt(v, 2) for v in vs), good))
    for cid, name, keys, ok in (QUEUE, VALID):
        vs = [m.get(k, NAN) for k in keys]
        good = all(not isnan(v) for v in vs) and ok(*vs)
        out.append((cid, name, " / ".join(fmt(v, 2) for v in vs), good))
    return out, sec


def accepted(m):
    v, sec = verdicts(m)
    must = [x for x in v if x[0][0] in "PWQV"]
    failed = [x[0] for x in must if not x[3]]
    if sec < 3:
        failed.append("S(%d of 5)" % sec)
    return failed


def score(m):
    s = 0.0
    for key, ref in (("util", HOST["util"]), ("psi_some", HOST["psi_some"])):
        v = m.get(key, NAN)
        s += abs(math.log2(v / ref)) if v and not isnan(v) and v > 0 else 9
    for key, j in (("h_disk_r_p99", 29), ("h_syncq_w_p99", 31),
                   ("h_disk_w_p99", 27)):
        b = bidx(m.get(key, NAN))
        s += abs(b - j) if b is not None else 9
    return s


ROWS = [
    ("util", "disk util %", "util"), ("media_busy", "media busy %", None),
    ("h_disk_r_p99", "disk_wait r p99 ms", "disk_r_p99"),
    ("h_syncq_w_p99", "syncq_wait w p99 ms", "syncq_w_p99"),
    ("h_syncq_w_p50", "syncq_wait w p50 ms", "syncq_w_p50"),
    ("h_disk_w_p99", "disk_wait w p99 ms", "disk_w_p99"),
    ("h_tot_w_p99", "total_wait w p99 ms", None),
    ("psi_some", "PSI io some %", "psi_some"),
    ("txg_mean", "txg sync mean s", "txg_mean"),
    ("txg_p95", "txg sync p95 s", "txg_p95"),
    ("r_await", "await r ms", "r_await"), ("w_await", "await w ms", "w_await"),
    ("flush_rate", "flushes/s/disk", "flush_rate"),
    ("flush_ms", "flush ms (diskstats)", "flush_ms"),
    ("flush_exec_ms", "flush ms (in the disk)", None),
    ("sync_share", "sync % of leaf writes", "sync_share"),
    ("wbt_zwrint", "z_wr_int in blk-wbt, % of D", "wbt_zwrint"),
    ("wb_bg_lt4", "blk-wbt background limit < 4, % of samples", None),
    ("lq_pend_pct", "leaf-s with sync writes queued %", None),
    ("lq_sw_pend_max", "max sync writes queued on a leaf", None),
    ("lq_util_pend", "util in leaf-s with sync queued %", None),
    ("w_mbs", "guest media MB/s", None),
    ("commits", "ZIL commits/s", "commits"),
    ("itx_per_w", "indirect itx per guest write", "itx_per_w"),
    ("itx_needcopy", "needcopy itx/s", None),
    ("pool_w_mbs", "pool write MB/s", None), ("amp", "write amplification",
                                              "amp"),
    ("pool_r_mbs", "pool read MB/s", "pool_r_mbs"),
    ("fsync_p50", "guest fsync p50 ms", None),
    ("fsync_p99", "guest fsync p99 ms", None),
    ("fsync_max", "guest fsync max ms", None),
    ("read_p99", "guest read p99 ms", None),
    ("txn_p99", "guest db txn p99 ms", None),
    ("tx_delay", "dmu_tx delays/s", None),
    ("late_gt2", "disk completions > 2 ms late %", None),
    ("refused", "disk requests refused, since start", None),
    ("steal", "steal %", None), ("load1", "load1", None),
]


def table(runs):
    names = ["%s/%s" % (p, c) for p, c, _ in runs]
    print("| metric | host | " + " | ".join(names) + " |")
    print("|---|---|" + "---|" * len(runs))
    for key, label, hk in ROWS:
        hv = fmt(HOST[hk], 2) if hk else ""
        print("| %s | %s | %s |" % (label, hv, " | ".join(
            fmt(m.get(key, NAN), 2) for _, _, m in runs)))


def verdict_table(runs):
    names = ["%s/%s" % (p, c) for p, c, _ in runs]
    print("| criterion | " + " | ".join(names) + " |")
    print("|---|" + "---|" * len(runs))
    vs = [verdicts(m)[0] for _, _, m in runs]
    for i, (cid, name, _, _) in enumerate(vs[0]):
        print("| %s %s | %s |" % (cid, name, " | ".join(
            "%s %s" % ("ok" if v[i][3] else "FAIL", v[i][2]) for v in vs)))
    print("| secondary held (of 5) | %s |" % " | ".join(
        str(verdicts(m)[1]) for _, _, m in runs))
    print("| score (lower is closer) | %s |" % " | ".join(
        fmt(score(m), 2) for _, _, m in runs))


def main(argv):
    if len(argv) < 4 or argv[1] != "calib":
        sys.stderr.write(__doc__)
        sys.exit(2)
    raw = argv[2]
    devdir = os.path.join(os.path.dirname(os.path.abspath(raw)), "dev")
    grid = argv[3]
    confirm = argv[4:]
    runs = []
    for p in [grid] + confirm:
        pd = os.path.join(raw, p)
        for c in sorted(os.listdir(pd)) if os.path.isdir(pd) else []:
            if os.path.isdir(os.path.join(pd, c)):
                runs.append((p, c, run_metrics(os.path.join(pd, c), devdir)))
    if not runs:
        sys.exit("no runs under %s" % raw)
    print("## Runs\n")
    table(runs)
    print("\n## Criteria\n")
    verdict_table(runs)
    print()
    for p, c, m in runs:
        print("- %s/%s: srcversion %s, pool %s, models %s" % (
            p, c, m["lay"].get("srcversion", "-"), m["lay"].get("pool", "-"),
            ",".join(sorted(m["models"])) or "-"))
    gr = [(c, m) for p, c, m in runs if p == grid]
    if not confirm:
        ok = [(score(m), c) for c, m in gr if not accepted(m)]
        if ok:
            s, c = min(ok)
            print("\nBEST %s score %.2f accepted" % (c, s))
        else:
            s, c = min((score(m), c) for c, m in gr)
            print("\nBEST %s score %.2f closest, NOT accepted: %s" % (
                c, s, " ".join(accepted(dict(gr)[c]))))
        return
    chosen = {c for p, c, _ in runs if p in confirm}
    if len(chosen) != 1:
        sys.exit("confirmation passes must hold one grid point, not %s" %
                 sorted(chosen))
    c = chosen.pop()
    ms = [m for p, cc, m in runs if cc == c]
    med = {}
    for key in set().union(*[m.keys() for m in ms]):
        vals = [m[key] for m in ms if isinstance(m.get(key), (int, float))
                and not isnan(m[key])]
        if vals:
            med[key] = statistics.median(vals)
    med["store_err"] = max(m.get("store_err", 0) for m in ms)
    med["refused"] = max(m.get("refused", 0) for m in ms)
    failed = accepted(med)
    stab = []
    for key in ("h_disk_r_p99", "h_syncq_w_p99", "h_disk_w_p99"):
        b = [bidx(m.get(key, NAN)) for m in ms]
        if None in b or max(b) - min(b) > 1:
            stab.append(key)
    for key in ("util", "psi_some"):
        v = [m.get(key, NAN) for m in ms]
        if any(isnan(x) for x in v) or statistics.pstdev(v) > 0.25 * mean(v):
            stab.append(key)
    print("\n## Chosen point %s over %d runs (median)\n" % (c, len(ms)))
    table([("median", c, med)])
    print()
    verdict_table([("median", c, med)])
    print("\nstability: %s" % ("ok" if not stab
                               else "FAIL " + " ".join(stab)))
    if failed or stab:
        print("\nNOT CALIBRATED %s: %s" % (c, " ".join(failed + stab)))
    else:
        print("\nCALIBRATED %s" % c)


if __name__ == "__main__":
    main(sys.argv)
