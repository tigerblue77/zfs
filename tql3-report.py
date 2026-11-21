#!/usr/bin/env python3
"""TASK-1121-TQL3 report: master against twu (master plus the SPL taskq
change that wakes a sleeping worker after dropping tq_lock), from the raw
data of tql3-bench.sh.

    tql3-report.py LOGDIR

LOGDIR holds raw/ (the passes) and sites-<arm>.txt.  Throughput passes
are raw/P<n>, lock passes raw/L<n> (their .agg.tsv files, written by
tql2-lock.py agg).  The rules are those of TASK-1121-TQL3, written before
the run:

- per row and pass, ratio = twu / master of the row's metric (IOPS for
  r8 and s8, MiB/s for w8) in that pass; per row, the geometric mean of
  the ratios and the exact sign-flip permutation p (two-sided) of their
  logarithms; "gain" if geo >= 1.03 and p <= 0.05, "loss" if
  geo <= 0.97 and p <= 0.05, otherwise "no difference shown";
- spread of a row = (max - min) / (2 x median) of master's metric;
- rule A, per row: twu's ratio exceeds 1 + max(0.03, spread) in every
  pass;
- rule B, per row: CPU-seconds per GiB ratio geo <= 0.95 with p <= 0.05
  and throughput geo between 0.97 and 1.03;
- p99 latency, per row: "worse" if the p99 ratio geo >= 1.05 with
  p <= 0.05, "better" if <= 0.95 with p <= 0.05;
- no row losing: no row has the throughput verdict "loss" or a p99
  "worse";
- lock passes, means over the passes, in % of CPU time: TQ = the
  tq_lock classes, WQ = the taskq wait-queue and wake-up classes;
  M0 (the premise): on r8, master's "tq_lock: worker" class >= 1.5 %;
  M1 (the lever acts): on r8, twu's TQ <= 0.5 x master's;
  M2 (no shift): on r8, twu's TQ + WQ <= 0.7 x master's;
- decision: "gain" if rule A or B holds on a row and no row is losing;
  "loss" if a row is losing; otherwise "mechanism only" if M1 and M2
  hold, "no effect" if not.
"""

import collections
import itertools
import json
import math
import os
import re
import statistics
import sys

ARMS = ("master", "twu")
ROWS = ("r8", "s8", "w8")
KIND = {"r8": "read", "s8": "write", "w8": "write"}
UNIT = {"r8": "IOPS", "s8": "IOPS", "w8": "MiB/s"}
DMU_TX = ("dmu_tx_quota", "dmu_tx_dirty_throttle", "dmu_tx_dirty_delay",
          "dmu_tx_dirty_over_max", "dmu_tx_memory_reserve")
WQ_CLASSES = ("taskq wait-queue lock (__wake_up etc. from taskq_*)",
              "taskq wake-up, scheduler locks (from taskq_*)")
WORKER = "tq_lock: worker (taskq_thread)"
SITE_LABEL = {"tsd_set": "start", "schedule": "after sleep",
              "remove_wait_queue": "after sleep", "indirect": "after task"}


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


def load_agg(path):
    """Span, CPUs, classes and takers of one tql2-lock.py .agg.tsv."""
    d = {"span": None, "ncpu": None, "class": {}, "taker": []}
    with open(path) as f:
        for ln in f:
            w = ln.rstrip("\n").split("\t")
            if w[0] == "#span":
                d["span"], d["ncpu"] = float(w[1]), int(w[2])
            elif w[0] == "class" and len(w) >= 6:
                d["class"][(w[1], w[2])] = float(w[4])
            elif w[0] == "taker" and len(w) >= 7:
                d["taker"].append((w[1], w[2], int(w[3]), float(w[4]),
                                   w[6]))
    return d


def load_sites(path):
    """{"taskq_thread+0xRET": label} for the tq_lock calls of
    taskq_thread(), and whether the map is unambiguous."""
    m = {}
    try:
        with open(path) as f:
            for ln in f:
                w = ln.split()
                if len(w) == 3 and w[0].startswith("taskq_thread+") and \
                        w[1].startswith("_raw_spin_lock"):
                    m[w[0]] = SITE_LABEL.get(w[2], "other site")
    except OSError:
        return {}, False
    labels = sorted(m.values())
    return m, labels == ["after sleep", "after task", "start"]


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


def lat_verdict(g, p):
    if g is None:
        return "n/a"
    if g >= 1.05 and p <= 0.05:
        return "worse"
    if g <= 0.95 and p <= 0.05:
        return "better"
    return "no difference shown"


def fmt(x, nd=1):
    return "n/a" if x is None else f"{x:.{nd}f}"


def pass_order(raw, name):
    try:
        with open(os.path.join(raw, name, "order.txt")) as f:
            return f.read().split()
    except OSError:
        return []


def passes_of(raw, prefix):
    return sorted((d for d in os.listdir(raw)
                   if re.fullmatch(re.escape(prefix) + r"\d+", d)),
                  key=lambda d: int(d[len(prefix):]))


def collect(raw):
    passes = passes_of(raw, "P")
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


def throughput(raw, passes, data):
    res = {}
    print("## Throughput per pass\n")
    for r in ROWS:
        rows = [p for p in passes
                if (p, "master", r) in data and (p, "twu", r) in data]
        print(f"### {r} ({UNIT[r]})\n")
        print("| pass | order | master | twu | twu/master |")
        print("|---|---|---|---|---|")
        ratios = []
        for p in rows:
            m = data[(p, "master", r)]["metric"]
            t = data[(p, "twu", r)]["metric"]
            ratios.append(t / m)
            print(f"| {p} | {' '.join(pass_order(raw, p))} | {m:.1f} | "
                  f"{t:.1f} | {t / m:.4f} |")
        print()
        if not ratios:
            res[r] = None
            continue

        def ratio_of(key, rows=rows):
            return [data[(p, "twu", r)][key] / data[(p, "master", r)][key]
                    for p in rows if data[(p, "twu", r)][key] and
                    data[(p, "master", r)][key]]
        mv = [data[(p, "master", r)]["metric"] for p in rows]
        spread = (max(mv) - min(mv)) / (2 * statistics.median(mv))
        g = geo(ratios)
        pp = perm_p(ratios)
        cr, p99, p999 = ratio_of("cpu_gib"), ratio_of("p99"), \
            ratio_of("p999")
        res[r] = {"n": len(ratios), "geo": g, "p": pp,
                  "won": sum(1 for x in ratios if x > 1),
                  "min": min(ratios), "spread": spread,
                  "verdict": verdict(g, pp),
                  "cpu_geo": geo(cr) if cr else None,
                  "cpu_p": perm_p(cr) if cr else None,
                  "p99_geo": geo(p99) if p99 else None,
                  "p99_p": perm_p(p99) if p99 else None,
                  "p999_geo": geo(p999) if p999 else None,
                  "p999_p": perm_p(p999) if p999 else None}
        res[r]["lat"] = lat_verdict(res[r]["p99_geo"], res[r]["p99_p"])

    print("## Verdicts (twu/master)\n")
    print("| row | passes | geo ratio | passes won | p perm | verdict | "
          "spread | min ratio | CPU-s/GiB geo | p | p99 geo | p | "
          "p99 verdict | p99.9 geo | p |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in ROWS:
        v = res.get(r)
        if not v:
            print(f"| {r} | 0 | n/a |  |  | not evaluated |" + "  |" * 9)
            continue
        print(f"| {r} | {v['n']} | {v['geo']:.4f} | {v['won']}/{v['n']} | "
              f"{v['p']:.3f} | {v['verdict']} | {100 * v['spread']:.2f} % "
              f"| {v['min']:.4f} | {fmt(v['cpu_geo'], 4)} | "
              f"{fmt(v['cpu_p'], 3)} | {fmt(v['p99_geo'], 4)} | "
              f"{fmt(v['p99_p'], 3)} | {v['lat']} | "
              f"{fmt(v['p999_geo'], 4)} | {fmt(v['p999_p'], 3)} |")
    print()
    return res


def means_table(passes, data):
    print("## Per row and arm, means over the throughput passes\n")
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

    print("## Taskqs per row and arm (means over the throughput passes)\n")
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
                nw = tot.get("thread_wakeups_nowork", 0)
                print(f"| {r} | {a} | {n} | "
                      f"{fmt(ex / span if span else None, 0)} | "
                      f"{tot.get('thread_sleeps', 0) / ex:.3f} | "
                      f"{wk / ex:.3f} | "
                      f"{100.0 * nw / wk if wk else 0:.1f} |")
    print()
    print("(tasks/s is over the whole span from snapshot to snapshot, "
          "ramp included.)\n")


def locks(logdir, raw):
    """Lock pass tables; returns {(arm, row): {"tq", "wq", "worker"}}."""
    lp = passes_of(raw, "L")
    out = {}
    print("## Lock passes\n")
    if not lp:
        print("No lock pass (raw/L1 missing).\n")
        return out
    print(f"Passes: " + "; ".join(f"{p} {' '.join(pass_order(raw, p))}"
                                  for p in lp) + "\n")
    sites = {a: load_sites(os.path.join(logdir, f"sites-{a}.txt"))
             for a in ARMS}
    for a in ARMS:
        print(f"Site map of {a}'s taskq_thread(): "
              f"{'unambiguous' if sites[a][1] else 'AMBIGUOUS, not split'}"
              f" ({', '.join(f'{k} {v}' for k, v in sorted(sites[a][0].items())) or 'none'})\n")
    print("| row | arm | passes | TQ % CPU | worker % | dispatch % | other "
          "tq_lock % | WQ % CPU | TQ+WQ % CPU | worker after sleep % | "
          "after task % | start % | unmapped % |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in ROWS:
        for a in ARMS:
            fs = [os.path.join(raw, p, a, r + ".agg.tsv") for p in lp]
            ds = [load_agg(f) for f in fs if os.path.exists(f)]
            ds = [d for d in ds if d["span"]]
            if not ds:
                continue
            acc = collections.defaultdict(float)
            for d in ds:
                cpu_s = d["span"] * d["ncpu"]
                for (cl, lt), tot in d["class"].items():
                    acc[cl] += 100.0 * tot / cpu_s / len(ds)
                smap, ok = sites[a]
                for tk, lt, n, tot, cl in d["taker"]:
                    if cl != WORKER:
                        continue
                    site = smap.get(tk.split("/")[0]) if ok else None
                    acc["site " + (site or "unmapped")] += \
                        100.0 * tot / cpu_s / len(ds)
            tq = sum(v for k, v in acc.items() if k.startswith("tq_lock"))
            wq = sum(acc[k] for k in WQ_CLASSES)
            out[(a, r)] = {"tq": tq, "wq": wq, "worker": acc[WORKER]}
            print(f"| {r} | {a} | {len(ds)} | {tq:.3f} | {acc[WORKER]:.3f} "
                  f"| {acc['tq_lock: dispatch (taskq_dispatch*)']:.3f} | "
                  f"{acc['tq_lock: other taskq_*/task_*']:.3f} | {wq:.3f} | "
                  f"{tq + wq:.3f} | {acc['site after sleep']:.3f} | "
                  f"{acc['site after task']:.3f} | {acc['site start']:.3f} "
                  f"| {acc['site unmapped'] + acc['site other site']:.3f} |")
    print("\n(worker sites are summed from each capture's top 40 takers, so "
          "they may add up to less than the worker class.)\n")

    print("### Taskq wait-queue takers (mean wait s per capture)\n")
    print("| row | arm | taker / caller | type | wait s |\n|---|---|---|---|"
          "---|")
    for r in ROWS:
        for a in ARMS:
            fs = [os.path.join(raw, p, a, r + ".agg.tsv") for p in lp]
            ds = [load_agg(f) for f in fs if os.path.exists(f)]
            if not ds:
                continue
            acc = collections.defaultdict(float)
            for d in ds:
                for tk, lt, n, tot, cl in d["taker"]:
                    if cl in WQ_CLASSES:
                        fr = [re.sub(r"\+0x[0-9a-f]+$", "", x)
                              for x in tk.split("/")]
                        acc[(" / ".join(fr[:2]), lt)] += tot / len(ds)
            for (k, lt), tot in sorted(acc.items(), key=lambda kv: -kv[1]):
                print(f"| {r} | {a} | {k} | {lt} | {tot:.3f} |")
    print()
    return out


def decide(res, lk):
    print("## Decision rules\n")
    print("| rule | row | figures | result |")
    print("|---|---|---|---|")
    holds = []
    for r in ROWS:
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
        if v["cpu_geo"] is None:
            print(f"| B | {r} | no CPU data | not evaluated |")
            continue
        b_ok = (v["cpu_geo"] <= 0.95 and v["cpu_p"] <= 0.05 and
                0.97 <= v["geo"] <= 1.03)
        print(f"| B | {r} | CPU-s/GiB geo {v['cpu_geo']:.4f} p "
              f"{v['cpu_p']:.3f}; throughput geo {v['geo']:.4f} | "
              f"{'holds' if b_ok else 'fails'} |")
        if b_ok:
            holds.append(f"B on {r}")
    losing = [r for r in ROWS if res.get(r) and
              (res[r]["verdict"] == "loss" or res[r]["lat"] == "worse")]
    print(f"| no row losing | all | losing: {', '.join(losing) or 'none'} "
          f"| {'holds' if not losing else 'fails'} |")

    def lock_rule(name, row, fig, ok):
        print(f"| {name} | {row} | {fig} | "
              f"{'not evaluated' if ok is None else ('holds' if ok else 'fails')} |")
        return ok
    m, t = lk.get(("master", "r8")), lk.get(("twu", "r8"))
    m0 = m1 = m2 = None
    if m:
        m0 = lock_rule("M0", "r8", f"master worker {m['worker']:.3f} % "
                       "against 1.5 %", m["worker"] >= 1.5)
    else:
        lock_rule("M0", "r8", "no lock data", None)
    if m and t:
        m1 = lock_rule("M1", "r8", f"TQ twu {t['tq']:.3f} % / master "
                       f"{m['tq']:.3f} % against 0.5",
                       t["tq"] <= 0.5 * m["tq"])
        m2 = lock_rule("M2", "r8", f"TQ+WQ twu {t['tq'] + t['wq']:.3f} % "
                       f"/ master {m['tq'] + m['wq']:.3f} % against 0.7",
                       t["tq"] + t["wq"] <= 0.7 * (m["tq"] + m["wq"]))
    else:
        lock_rule("M1", "r8", "no lock data", None)
        lock_rule("M2", "r8", "no lock data", None)
    for r in ("s8", "w8"):
        mm, tt = lk.get(("master", r)), lk.get(("twu", r))
        if mm and tt:
            print(f"| (M1, M2 listed) | {r} | TQ {tt['tq']:.3f} / "
                  f"{mm['tq']:.3f} %; TQ+WQ {tt['tq'] + tt['wq']:.3f} / "
                  f"{mm['tq'] + mm['wq']:.3f} % | not judged |")
    print()
    if losing:
        print(f"**Decision: loss** (losing: {', '.join(losing)}" +
              (f"; {', '.join(holds)} held" if holds else "") + ").")
    elif holds:
        print(f"**Decision: gain** ({', '.join(holds)}; no row losing).")
    elif m1 and m2:
        print("**Decision: mechanism only** (no throughput or CPU rule "
              "holds; M1 and M2 hold on r8).")
    else:
        print("**Decision: no effect** (no throughput or CPU rule holds; "
              f"M1 {'holds' if m1 else 'fails or not evaluated'}, M2 "
              f"{'holds' if m2 else 'fails or not evaluated'} on r8; M0 "
              f"{'holds' if m0 else 'fails or not evaluated'}).")
    print()


def noise(raw, passes, data):
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
    print("Arm position: geometric mean of twu/master by the arm that ran "
          "first.\n")
    print("| row | twu first | master first |")
    print("|---|---|---|")
    for r in ROWS:
        cols = []
        for first in ("twu", "master"):
            xs = [data[(p, "twu", r)]["metric"] / data[(p, "master", r)]
                  ["metric"] for p in passes
                  if pass_order(raw, p)[:1] == [first] and
                  (p, "twu", r) in data and (p, "master", r) in data]
            cols.append(f"{geo(xs):.4f} ({len(xs)})" if xs else "n/a")
        print(f"| {r} | {cols[0]} | {cols[1]} |")
    print()


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: tql3-report.py LOGDIR")
    logdir = sys.argv[1]
    raw = os.path.join(logdir, "raw")
    passes, data = collect(raw)
    print("# TASK-1121-TQL3 summary\n")
    print(f"Throughput passes: {' '.join(passes) or 'none'}; arm order per "
          "pass: " + "; ".join(f"{p} {' '.join(pass_order(raw, p))}"
                               for p in passes))
    print()
    res = throughput(raw, passes, data)
    means_table(passes, data)
    lk = locks(logdir, raw)
    decide(res, lk)
    noise(raw, passes, data)


if __name__ == "__main__":
    main()
