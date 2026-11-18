#!/usr/bin/env python3
"""TASK-1118-TQL report: master against tql (master plus the SPL taskq
wake-up change), from the raw data of tql-bench.sh.

    tql-report.py RAW PREFIX

RAW is the driver's raw directory, PREFIX the name prefix of the
measured passes (P: P1, P2, ...).  The perf lock contention pass, if
any, is RAW/K.  The rules are those of TASK-1118-TQL, written before
the run:

- per row and pass, ratio = tql / master of the row's metric (IOPS, or
  MiB/s for w4 and w8) in that pass; per row, the geometric mean of the
  ratios and the exact sign-flip permutation p (two-sided) of their
  logarithms; "gain" if geo >= 1.03 and p <= 0.05, "loss" if
  geo <= 0.97 and p <= 0.05, otherwise "no difference shown";
- spread of a row = (max - min) / (2 x median) of master's metric over
  the passes;
- rule A, on r8, w4 and w8: tql's ratio exceeds 1 + max(0.03, spread)
  in every pass;
- rule B, on r8, w4 and w8: CPU-seconds per GiB ratio geo <= 0.95 with
  p <= 0.05, throughput geo between 0.97 and 1.03, and the tq_lock wait
  of the perf lock pass (callers taskq_*) of tql at most 0.7 x master's
  on that row;
- decision: "gain" if rule A or rule B holds on r8, w4 or w8 and no row
  (c8 r8 s8 w4 w8) is a "loss"; otherwise "no gain at 8 vCPU".
"""

import itertools
import json
import math
import os
import re
import statistics
import sys

ARMS = ("master", "tql")
ROWS = ("c8", "r8", "s8", "w4", "w8")
DECIDING = ("r8", "w4", "w8")
KIND = {"c8": "read", "r8": "read", "s8": "write", "w4": "write",
        "w8": "write"}
UNIT = {"c8": "IOPS", "r8": "IOPS", "s8": "IOPS", "w4": "MiB/s",
        "w8": "MiB/s"}
DMU_TX = ("dmu_tx_quota", "dmu_tx_dirty_throttle", "dmu_tx_dirty_delay",
          "dmu_tx_dirty_over_max", "dmu_tx_memory_reserve")
UNITS_S = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0, "m": 60.0,
           "h": 3600.0}
LOCK_RE = re.compile(
    r"^\s*(\d+)\s+([\d.]+)\s+(ns|us|ms|s|m|h)\s+([\d.]+)\s+(\S+)\s+"
    r"([\d.]+)\s+(\S+)\s+(\S+)\s+(\S+.*?)\s*$")


def load_json(path):
    try:
        with open(path) as f:
            text = f.read()
    except OSError:
        return None
    i = text.find("{")
    if i < 0:
        return None
    try:
        return json.loads(text[i:])
    except ValueError:
        return None


def pct(d, key):
    """Percentile KEY (a float) of a fio percentile dict, in us."""
    if not d:
        return None
    for k, v in d.items():
        try:
            if abs(float(k) - key) < 1e-6:
                return v / 1000.0
        except ValueError:
            pass
    return None


def load_fio(path, kind):
    j = load_json(path)
    if not j or not j.get("jobs"):
        return None
    s = j["jobs"][0][kind]
    lat = s.get("lat_ns", {}).get("percentile") or \
        s.get("clat_ns", {}).get("percentile")
    return {"iops": s["iops"], "bw": s["bw_bytes"],
            "p99": pct(lat, 99.0), "p999": pct(lat, 99.9)}


def load_cpu(path):
    """Busy CPU seconds, steal % and window from two /proc/stat lines."""
    try:
        with open(path) as f:
            lines = f.read().split("\n")
    except OSError:
        return None
    st = [list(map(int, ln.split()[1:9])) for ln in lines
          if ln.startswith("cpu ")]
    win = [int(ln.split()[1]) for ln in lines if ln.startswith("window ")]
    if len(st) != 2 or not win:
        return None
    d = [b - a for a, b in zip(st[0], st[1])]
    tot = sum(d) or 1
    busy = d[0] + d[1] + d[2] + d[5] + d[6]
    hz = os.sysconf("SC_CLK_TCK")
    return {"busy_s": busy / hz, "steal": 100.0 * d[7] / tot,
            "window": win[0]}


def load_snap(path):
    out = {"dmu_tx": {}, "arc": {}, "tq": {}, "load1": None, "time": None}
    try:
        with open(path) as f:
            for ln in f:
                w = ln.split()
                if not w:
                    continue
                if w[0] == "time":
                    out["time"] = float(w[1])
                elif w[0] == "loadavg":
                    out["load1"] = float(w[1])
                elif w[0] == "dmu_tx" and len(w) == 3:
                    out["dmu_tx"][w[1]] = int(w[2])
                elif w[0] == "arc" and len(w) == 3:
                    out["arc"][w[1]] = int(w[2])
                elif w[0] == "tq" and len(w) == 4:
                    name = re.sub(r"\.\d+$", "", w[1])
                    t = out["tq"].setdefault(name, {})
                    t[w[2]] = t.get(w[2], 0) + int(w[3])
    except OSError:
        return None
    return out


def load_lock(path):
    """Entries (wait s, contended, type, caller) of a perf lock file."""
    try:
        with open(path) as f:
            lines = f.read().split("\n")
    except OSError:
        return None
    ent = []
    for ln in lines:
        m = LOCK_RE.match(ln)
        if m:
            ent.append((float(m.group(2)) * UNITS_S[m.group(3)],
                        int(m.group(1)), m.group(8), m.group(9)))
    return ent


def geo(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def perm_p(xs):
    """Exact two-sided sign-flip permutation p of the log ratios."""
    ls = [math.log(x) for x in xs]
    obs = abs(sum(ls))
    n = 0
    hit = 0
    for signs in itertools.product((1, -1), repeat=len(ls)):
        n += 1
        if abs(sum(s * v for s, v in zip(signs, ls))) >= obs - 1e-12:
            hit += 1
    return hit / n


def verdict(g, p, lo=0.97, hi=1.03):
    if g >= hi and p <= 0.05:
        return "gain"
    if g <= lo and p <= 0.05:
        return "loss"
    return "no difference shown"


def fmt(x, nd=1):
    return "n/a" if x is None else f"{x:.{nd}f}"


def pass_order(raw, name):
    try:
        with open(os.path.join(raw, name, "order.txt")) as f:
            return f.read().split()
    except OSError:
        return []


def collect(raw, prefix):
    passes = sorted((d for d in os.listdir(raw)
                     if re.fullmatch(re.escape(prefix) + r"\d+", d)),
                    key=lambda d: int(d[len(prefix):]))
    data = {}
    for pn in passes:
        for a in ARMS:
            ad = os.path.join(raw, pn, a)
            for r in ROWS:
                fio = load_fio(os.path.join(ad, r + ".json"), KIND[r])
                if fio is None:
                    continue
                cpu = load_cpu(os.path.join(ad, r + ".cpu"))
                s0 = load_snap(os.path.join(ad, r + ".snap0"))
                s1 = load_snap(os.path.join(ad, r + ".snap1"))
                v = dict(fio)
                v["metric"] = fio["iops"] if UNIT[r] == "IOPS" else \
                    fio["bw"] / 2**20
                if cpu:
                    gib = fio["bw"] * cpu["window"] / 2**30
                    v["cpu_gib"] = cpu["busy_s"] / gib if gib > 0 else None
                    v["steal"] = cpu["steal"]
                else:
                    v["cpu_gib"] = v["steal"] = None
                v["load1"] = s1["load1"] if s1 else None
                if s0 and s1:
                    v["dmu_tx"] = {k: s1["dmu_tx"].get(k, 0) -
                                   s0["dmu_tx"].get(k, 0) for k in DMU_TX}
                    h = s1["arc"].get("demand_data_hits", 0) - \
                        s0["arc"].get("demand_data_hits", 0)
                    m = s1["arc"].get("demand_data_misses", 0) - \
                        s0["arc"].get("demand_data_misses", 0)
                    v["miss"] = 100.0 * m / (h + m) if h + m else None
                    if s0["time"] and s1["time"]:
                        v["span"] = s1["time"] - s0["time"]
                    v["tq"] = {}
                    for name, t1 in s1["tq"].items():
                        t0 = s0["tq"].get(name, {})
                        v["tq"][name] = {k: t1.get(k, 0) - t0.get(k, 0)
                                         for k in t1}
                data[(pn, a, r)] = v
    return passes, data


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: tql-report.py RAW PREFIX")
    raw, prefix = sys.argv[1], sys.argv[2]
    passes, data = collect(raw, prefix)
    print("# TASK-1118-TQL summary\n")
    print(f"Passes: {' '.join(passes) or 'none'}; arm order per pass: " +
          "; ".join(f"{p} {' '.join(pass_order(raw, p))}" for p in passes))
    print()

    res = {}
    print("## Throughput per pass\n")
    for r in ROWS:
        rows = [p for p in passes
                if (p, "master", r) in data and (p, "tql", r) in data]
        print(f"### {r} ({UNIT[r]})\n")
        print("| pass | order | master | tql | tql/master |")
        print("|---|---|---|---|---|")
        ratios = []
        for p in rows:
            m = data[(p, "master", r)]["metric"]
            t = data[(p, "tql", r)]["metric"]
            ratios.append(t / m)
            print(f"| {p} | {' '.join(pass_order(raw, p))} | {m:.1f} | "
                  f"{t:.1f} | {t / m:.4f} |")
        print()
        if not ratios:
            res[r] = None
            continue
        mv = [data[(p, "master", r)]["metric"] for p in rows]
        spread = (max(mv) - min(mv)) / (2 * statistics.median(mv))
        g = geo(ratios)
        pp = perm_p(ratios)
        cr = [data[(p, "tql", r)]["cpu_gib"] / data[(p, "master", r)]
              ["cpu_gib"] for p in rows
              if data[(p, "tql", r)]["cpu_gib"] and
              data[(p, "master", r)]["cpu_gib"]]
        p99 = [data[(p, "tql", r)]["p99"] / data[(p, "master", r)]["p99"]
               for p in rows if data[(p, "tql", r)]["p99"] and
               data[(p, "master", r)]["p99"]]
        res[r] = {"n": len(ratios), "geo": g, "p": pp,
                  "won": sum(1 for x in ratios if x > 1),
                  "min": min(ratios), "spread": spread,
                  "verdict": verdict(g, pp),
                  "cpu_geo": geo(cr) if cr else None,
                  "cpu_p": perm_p(cr) if cr else None,
                  "p99_geo": geo(p99) if p99 else None,
                  "p99_p": perm_p(p99) if p99 else None}

    print("## Verdicts (tql/master)\n")
    print("| row | passes | geo ratio | passes won | p perm | verdict | "
          "spread | min ratio | CPU-s/GiB geo | p | p99 geo | p |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in ROWS:
        v = res.get(r)
        if not v:
            print(f"| {r} | 0 | n/a |  |  | not evaluated |  |  |  |  |"
                  "  |  |")
            continue
        print(f"| {r} | {v['n']} | {v['geo']:.4f} | {v['won']}/{v['n']} | "
              f"{v['p']:.3f} | {v['verdict']} | {100 * v['spread']:.2f} % "
              f"| {v['min']:.4f} | {fmt(v['cpu_geo'], 4)} | "
              f"{fmt(v['cpu_p'], 3)} | {fmt(v['p99_geo'], 4)} | "
              f"{fmt(v['p99_p'], 3)} |")
    print()

    print("## Per row and arm, means over the passes\n")
    print("| row | arm | metric | CPU-s/GiB | p99 us | p99.9 us | "
          "data miss % | " + " | ".join(k[7:] for k in DMU_TX) + " |")
    print("|---|---|---|---|---|---|---|" + "---|" * len(DMU_TX))
    for r in ROWS:
        for a in ARMS:
            vs = [data[(p, a, r)] for p in passes if (p, a, r) in data]
            if not vs:
                continue

            def mean(key, vs=vs):
                xs = [v[key] for v in vs if v.get(key) is not None]
                return statistics.mean(xs) if xs else None
            dt = [statistics.mean(v["dmu_tx"][k] for v in vs
                                  if "dmu_tx" in v) if any(
                "dmu_tx" in v for v in vs) else None for k in DMU_TX]
            print(f"| {r} | {a} | {fmt(mean('metric'))} | "
                  f"{fmt(mean('cpu_gib'), 3)} | {fmt(mean('p99'))} | "
                  f"{fmt(mean('p999'))} | {fmt(mean('miss'))} | " +
                  " | ".join(fmt(x, 0) for x in dt) + " |")
    print()

    print("## Taskqs per row and arm (means over the passes)\n")
    print("Tasks executed per second of the run, thread sleeps and wakeups "
          "per task, and the share of wakeups that found no task.\n")
    print("| row | arm | taskq | tasks/s | sleeps/task | wakeups/task | "
          "nowork % |")
    print("|---|---|---|---|---|---|---|")
    for r in ROWS:
        for a in ARMS:
            vs = [data[(p, a, r)] for p in passes
                  if (p, a, r) in data and "tq" in data[(p, a, r)]]
            if not vs:
                continue
            names = sorted({n for v in vs for n in v["tq"]})
            agg = []
            for n in names:
                tot = {}
                for v in vs:
                    for k, x in v["tq"].get(n, {}).items():
                        tot[k] = tot.get(k, 0) + x
                ex = tot.get("tasks_executed", 0)
                if ex <= 0:
                    continue
                agg.append((ex, n, tot))
            span = sum(v["span"] for v in vs if v.get("span")) or None
            for ex, n, tot in sorted(agg, reverse=True)[:4]:
                wk = tot.get("thread_wakeups", 0)
                print(f"| {r} | {a} | {n} | "
                      f"{fmt(ex / span if span else None, 0)} | "
                      f"{tot.get('thread_sleeps', 0) / ex:.3f} | "
                      f"{wk / ex:.3f} | "
                      f"{100.0 * tot.get('thread_wakeups_nowork', 0) / wk if wk else 0:.1f} |")
    print()
    print("(tasks/s is over the whole span from snapshot to snapshot, "
          "ramp included.)\n")

    lock = {}
    kdir = os.path.join(raw, "K")
    print("## perf lock contention pass (not used for throughput)\n")
    if os.path.isdir(kdir):
        print(f"Arm order: {' '.join(pass_order(raw, 'K'))}\n")
        print("| row | arm | source | tq_lock wait s (callers taskq_*) | "
              "contended | wait-queue lock wait s | top caller |")
        print("|---|---|---|---|---|---|---|")
        for r in ROWS:
            for a in ARMS:
                f = os.path.join(kdir, a, r + ".lock.txt")
                ent = load_lock(f)
                if ent is None:
                    continue
                src = "perf lock record" if os.path.exists(
                    os.path.join(kdir, a, r + ".lock-b.txt")) else "-b"
                tq = [e for e in ent if e[3].startswith("taskq_")]
                wq = [e for e in ent if re.match(
                    r"(__wake_up|add_wait_queue|remove_wait_queue|"
                    r"prepare_to_wait|finish_wait)", e[3])]
                w = sum(e[0] for e in tq)
                lock[(a, r)] = w if ent else None
                top = max(ent, key=lambda e: e[0]) if ent else None
                print(f"| {r} | {a} | {src} | {w:.3f} | "
                      f"{sum(e[1] for e in tq)} | "
                      f"{sum(e[0] for e in wq):.3f} | "
                      f"{(top[3] + ' ' + format(top[0], '.3f') + ' s') if top else 'none parsed'} |")
        print()
    else:
        print("No perf lock pass (RAW/K missing).\n")

    print("## Decision rules\n")
    print("| rule | row | figures | result |")
    print("|---|---|---|---|")
    holds = []
    for r in DECIDING:
        v = res.get(r)
        if not v:
            print(f"| A | {r} | no data | not evaluated |")
            print(f"| B | {r} | no data | not evaluated |")
            continue
        thr = 1 + max(0.03, v["spread"])
        a_ok = v["min"] > thr
        print(f"| A | {r} | min ratio {v['min']:.4f} against "
              f"{thr:.4f} (spread {100 * v['spread']:.2f} %) over "
              f"{v['n']} passes | {'holds' if a_ok else 'fails'} |")
        if a_ok:
            holds.append(f"A on {r}")
        lm, lt = lock.get(("master", r)), lock.get(("tql", r))
        if v["cpu_geo"] is None or lm is None or lt is None:
            print(f"| B | {r} | CPU geo {fmt(v['cpu_geo'], 4)}, lock "
                  f"{fmt(lt, 3)} s / {fmt(lm, 3)} s | not evaluated |")
            continue
        lr = lt / lm if lm > 0 else None
        b_ok = (v["cpu_geo"] <= 0.95 and v["cpu_p"] <= 0.05 and
                0.97 <= v["geo"] <= 1.03 and lr is not None and lr <= 0.7)
        print(f"| B | {r} | CPU-s/GiB geo {v['cpu_geo']:.4f} p "
              f"{v['cpu_p']:.3f}; throughput geo {v['geo']:.4f}; tq_lock "
              f"wait {lt:.3f} s / {lm:.3f} s = {fmt(lr, 3)} | "
              f"{'holds' if b_ok else 'fails'} |")
        if b_ok:
            holds.append(f"B on {r}")
    losing = [r for r in ROWS if res.get(r) and
              res[r]["verdict"] == "loss"]
    print(f"| no row losing | all | losing: {', '.join(losing) or 'none'} "
          f"| {'holds' if not losing else 'fails'} |")
    print()
    if holds and not losing:
        print(f"**Decision: gain** ({', '.join(holds)}; no row losing).")
    elif losing:
        print(f"**Decision: no gain at 8 vCPU** (losing: "
              f"{', '.join(losing)}" +
              (f"; {', '.join(holds)} held" if holds else "") + ").")
    else:
        print("**Decision: no gain at 8 vCPU** (neither rule holds on r8, "
              "w4 or w8).")
    print()

    print("## Noise\n")
    print("| pass | highest steal % | load1 range | rows with steal > 1 % |")
    print("|---|---|---|---|")
    for p in passes:
        vs = [(a, r, data[(p, a, r)]) for a in ARMS for r in ROWS
              if (p, a, r) in data]
        st = [v["steal"] for _, _, v in vs if v["steal"] is not None]
        ld = [v["load1"] for _, _, v in vs if v["load1"] is not None]
        hi = [f"{a}/{r} {v['steal']:.2f}" for a, r, v in vs
              if v["steal"] is not None and v["steal"] > 1.0]
        print(f"| {p} | {fmt(max(st) if st else None, 2)} | "
              f"{fmt(min(ld) if ld else None, 2)}-"
              f"{fmt(max(ld) if ld else None, 2)} | "
              f"{', '.join(hi) or 'none'} |")
    print()
    print("Arm position: geometric mean of tql/master by the arm that ran "
          "first.\n")
    print("| row | tql first | master first |")
    print("|---|---|---|")
    for r in ROWS:
        cols = []
        for first in ("tql", "master"):
            xs = [data[(p, "tql", r)]["metric"] / data[(p, "master", r)]
                  ["metric"] for p in passes
                  if pass_order(raw, p)[:1] == [first] and
                  (p, "tql", r) in data and (p, "master", r) in data]
            cols.append(f"{geo(xs):.4f} ({len(xs)})" if xs else "n/a")
        print(f"| {r} | {cols[0]} | {cols[1]} |")


if __name__ == "__main__":
    main()
