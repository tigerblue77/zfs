#!/usr/bin/env python3
"""TASK-1117-V5CG summary: python3 -I c5g-sum.py /var/log/task-1117-v5cg

Per row: B's IOPS and fsync percentiles (us), A's IOPS and fsync p50, the
writes charged to A's and B's io cgroups on the pool's disk (io.stat wios),
the longest txg sync that ended during the row (ms), then means per arm.
"""
import json
import os
import sys

L = sys.argv[1]
with open(os.path.join(L, "majmin")) as f:
    MM = f.read().strip()
TAGS = ["s0", "s1", "s2"]


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def fio(path):
    j = load(path)
    if not j or not j.get("jobs"):
        return {}
    job = j["jobs"][0]
    pc = ((job.get("sync") or {}).get("lat_ns") or {}).get("percentile") or {}
    out = {"iops": job["write"]["iops"]}
    for name, key in (("p50", "50.000000"), ("p99", "99.000000"),
                      ("p999", "99.900000")):
        if key in pc:
            out[name] = pc[key] / 1000.0
    return out


def scope(path):
    out = {"wios": None, "iomax": "", "rc": None}
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        return out
    stat = False
    for ln in lines:
        if ln.startswith("io.max:"):
            out["iomax"] = ln[7:].strip()
        elif ln.startswith("rc "):
            out["rc"] = ln[3:].strip()
        elif ln == "io.stat:":
            stat = True
        elif stat and ln.startswith(MM + " "):
            kv = dict(x.split("=", 1) for x in ln.split()[1:] if "=" in x)
            out["wios"] = int(kv.get("wios", 0))
    if stat and out["wios"] is None:
        out["wios"] = 0
    return out


def txgs(path):
    t = {}
    try:
        with open(path) as f:
            for ln in f:
                p = ln.split()
                if len(p) >= 12 and p[0].isdigit() and p[2] == "C":
                    t[int(p[0])] = int(p[11])
    except OSError:
        pass
    return t


def maxsync(d, tag):
    t0 = txgs(os.path.join(d, tag + ".txgs0"))
    t1 = txgs(os.path.join(d, tag + ".txgs1"))
    new = [v for k, v in t1.items() if k not in t0]
    return max(new) / 1e6 if new else None


def fmt(v, nd=0):
    if v is None:
        return "-"
    return f"{v:.{nd}f}"


raw = os.path.join(L, "raw")
rows = []
for p in sorted(os.listdir(raw)):
    pd = os.path.join(raw, p)
    if not os.path.isdir(pd):
        continue
    for arm in sorted(os.listdir(pd)):
        d = os.path.join(pd, arm)
        if not os.path.isdir(d):
            continue
        for tag in TAGS:
            b = fio(os.path.join(d, tag + ".b.json"))
            a = fio(os.path.join(d, tag + ".a.json")) if tag != "s0" else {}
            sb = scope(os.path.join(d, tag + ".b.txt"))
            sa = scope(os.path.join(d, tag + ".a.txt")) if tag != "s0" \
                else {}
            rows.append({"pass": p, "arm": arm, "tag": tag, "b": b, "a": a,
                         "sa": sa, "sb": sb, "sync": maxsync(d, tag)})

print("| pass | arm | row | B IOPS | B fsync p50 | p99 | p99.9 | "
      "A IOPS | A fsync p50 | A wios | B wios | max txg sync ms | rc A/B |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in rows:
    b, a, sa, sb = r["b"], r["a"], r["sa"], r["sb"]
    print(f"| {r['pass']} | {r['arm']} | {r['tag']} | {fmt(b.get('iops'))} | "
          f"{fmt(b.get('p50'))} | {fmt(b.get('p99'))} | "
          f"{fmt(b.get('p999'))} | {fmt(a.get('iops'))} | "
          f"{fmt(a.get('p50'))} | {fmt(sa.get('wios'))} | "
          f"{fmt(sb.get('wios'))} | {fmt(r['sync'], 1)} | "
          f"{sa.get('rc', '-')}/{sb.get('rc')} |")

print()
print("Means over passes")
print()
print("| arm | row | n | B IOPS | B fsync p50 | B fsync p99 | A IOPS | "
      "A wios | B wios |")
print("|---|---|---|---|---|---|---|---|---|")
arms = []
for r in rows:
    if r["arm"] not in arms:
        arms.append(r["arm"])
for arm in arms:
    for tag in TAGS:
        sel = [r for r in rows if r["arm"] == arm and r["tag"] == tag]

        def mean(get):
            v = [get(r) for r in sel]
            v = [x for x in v if x is not None]
            return sum(v) / len(v) if v else None
        print(f"| {arm} | {tag} | {len(sel)} | "
              f"{fmt(mean(lambda r: r['b'].get('iops')))} | "
              f"{fmt(mean(lambda r: r['b'].get('p50')))} | "
              f"{fmt(mean(lambda r: r['b'].get('p99')))} | "
              f"{fmt(mean(lambda r: r['a'].get('iops')))} | "
              f"{fmt(mean(lambda r: r['sa'].get('wios')))} | "
              f"{fmt(mean(lambda r: r['sb'].get('wios')))} |")

print()
print("io.max seen by A (first row of each arm):")
for arm in arms:
    sel = [r for r in rows if r["arm"] == arm and r["tag"] != "s0"]
    if sel:
        print(f"- {arm}: {sel[0]['sa'].get('iomax') or '(empty)'}")
