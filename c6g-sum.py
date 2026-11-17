#!/usr/bin/env python3
"""TASK-1118-V6CG summary: python3 -I c6g-sum.py /var/log/task-1118-v6cg

Reads raw/Z (part Z), raw/T (the traced pass) and raw/P* (the timed
passes) and prints the tables and the verdicts, whose rules are fixed
here before the run:

Z   per arm and case: PASS when the dd writer's scope has at most 3 writes
    charged on c6gb0.  The check is usable as a regression test when Z1
    passes on master, v6 and v6dbg and fails on v5.
CG  v6 (like master) "not held" when, in every pass, A's charged writes
    are at most 3 in s1 and s2 and B's s1/s0 IOPS ratio is at least 0.70;
    v5 "reproduces TASK-1117-V5CG2" when its mean B s1/s0 is at most 0.30.
I   mechanism: A's I/O priority is "idle" in every i1 row; IDLE requests
    queued in mq-deadline (max over the row) 0 on master and v6.  With
    ratios X/master of B's i1 IOPS and fsync p99, per pass, geometric
    mean over passes: v5 "inversion reproduced" when IOPS ratio <= 0.50
    or p99 ratio >= 10; v6 "matches master" when IOPS ratio >= 0.85 and
    p99 ratio <= 1.5.
C   per row c1 and c1e, r = B IOPS v6/master and q = B fsync p99
    v6/master per pass: "clearly worse" when geo(r) <= 0.90 with r < 1 in
    every pass, or geo(q) >= 1.25 with q > 1 in every pass; otherwise
    "within noise".  Both rows within noise: keep v6; else: fallback (keep
    the taskq for writers that are not kernel threads).  v5/v6 is printed
    beside it as the noise of two arms that do the same CPU work.
D   d1, v6 against v5 (both issue inline; v6 also clones): "donor cost
    measurable" when geo(IOPS v6/v5) <= 0.97 with v6 lower in every pass;
    otherwise "no donor cost shown".  Traced pass: on v6 and v6dbg fio's
    clones are 0 in d0 and between 0.8 x lwbs and lwbs + 5 in d1; on
    master and v5 fio clones nothing; master's fio submits no bio.
"""
import glob
import json
import math
import os
import re
import statistics
import sys

L = sys.argv[1]
RAW = os.path.join(L, "raw")


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


MM = read(os.path.join(L, "majmin")).strip()
ARMS = ["master", "v5", "v6", "v6dbg"]
ROWS = ["s0", "s1", "s2", "c1", "c0e", "c1e", "i0", "i1", "d0", "d1"]


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
    out = {"iops": job["write"]["iops"], "bw": job["write"]["bw"] / 1024.0}
    for name, key in (("p50", "50.000000"), ("p99", "99.000000"),
                      ("p999", "99.900000")):
        if key in pc:
            out[name] = pc[key] / 1000.0
    return out


def scope(path, mm=None):
    """What scope() or noscope() of c6g.sh wrote, as a dict."""
    mm = mm or MM
    out = {"wios": None, "iomax": "", "cpumax": "", "rc": None,
           "ioprio": "", "cgid": None, "throttled": None, "secs": None,
           "cgroup": "", "found": False}
    text = read(path)
    if not text:
        return out
    out["found"] = True
    sect = None
    t0 = t1 = None
    for ln in text.splitlines():
        if ln.startswith("cgroup "):
            out["cgroup"] = ln[7:].strip()
        elif ln.startswith("cgroup.id "):
            out["cgid"] = ln.split()[1]
        elif ln.startswith("io.max:"):
            out["iomax"] = ln[7:].strip()
        elif ln.startswith("cpu.max:"):
            out["cpumax"] = ln[8:].strip()
        elif ln.startswith("ioprio:"):
            out["ioprio"] = ln[7:].strip()
        elif ln.startswith("start "):
            t0 = float(ln.split()[1])
        elif ln.startswith("end "):
            t1 = float(ln.split()[1])
        elif ln.startswith("rc "):
            out["rc"] = ln[3:].strip()
        elif ln in ("io.stat:", "cpu.stat:"):
            sect = ln
        elif sect == "io.stat:" and ln.startswith(mm + " "):
            kv = dict(x.split("=", 1) for x in ln.split()[1:] if "=" in x)
            out["wios"] = int(kv.get("wios", 0))
        elif sect == "cpu.stat:" and ln.startswith("nr_throttled "):
            out["throttled"] = int(ln.split()[1])
    if sect and out["wios"] is None:
        out["wios"] = 0
    if t0 is not None and t1 is not None:
        out["secs"] = t1 - t0
    return out


def kstat(path):
    out = {}
    for ln in read(path).splitlines()[2:]:
        p = ln.split()
        if len(p) == 3 and p[2].isdigit():
            out[p[0]] = int(p[2])
    return out


def lwbs(d, tag):
    z0 = kstat(os.path.join(d, tag + ".zil0"))
    z1 = kstat(os.path.join(d, tag + ".zil1"))
    k = ("zil_itx_metaslab_normal_count", "zil_itx_metaslab_slog_count")
    if not z0 or not z1:
        return None
    return sum(z1.get(x, 0) - z0.get(x, 0) for x in k)


def txgs(path):
    t = {}
    for ln in read(path).splitlines():
        p = ln.split()
        if len(p) >= 12 and p[0].isdigit() and p[2] == "C":
            t[int(p[0])] = int(p[11])
    return t


def maxsync(d, tag):
    t0 = txgs(os.path.join(d, tag + ".txgs0"))
    t1 = txgs(os.path.join(d, tag + ".txgs1"))
    new = [v for k, v in t1.items() if k not in t0]
    return max(new) / 1e6 if new else None


def cpu(d, tag):
    """Steal % over the row and load1 at its end, from tag.cpu0/1."""
    a = read(os.path.join(d, tag + ".cpu0")).splitlines()
    b = read(os.path.join(d, tag + ".cpu1")).splitlines()
    if len(a) < 2 or len(b) < 2:
        return None, None
    x = [int(v) for v in a[0].split()[1:9]]
    y = [int(v) for v in b[0].split()[1:9]]
    dt = [j - i for i, j in zip(x, y)]
    tot = sum(dt) or 1
    return 100.0 * dt[7] / tot, float(b[1].split()[0])


def ddq(path):
    """Max IDLE and mean BE requests queued in mq-deadline over the row."""
    idle, be = [], []
    for ln in read(path).splitlines():
        p = ln.split()
        if len(p) == 4 and p[1].isdigit():
            be.append(int(p[2]))
            idle.append(int(p[3]))
    if not idle:
        return None, None
    return max(idle), statistics.mean(be)


BT_LINE = re.compile(r"^@(\w+)(?:\[(.*)\])?:\s*(.*)$")
BT_STATS = re.compile(r"count (\d+), average (-?\d+), total (-?\d+)")


def bt(path):
    """Maps printed by c6g-clone.bt, or None without the file."""
    text = read(path)
    if not text:
        return None
    out = {"submit": {}, "clone": {}, "clone_ns": None, "ready": False}
    for ln in text.splitlines():
        if ln.startswith("READY"):
            out["ready"] = True
        m = BT_LINE.match(ln)
        if not m:
            continue
        name, key, rest = m.groups()
        if name == "fio_submit" and rest.strip().isdigit():
            out["submit"][key.strip()] = int(rest)
        elif name == "clone" and rest.strip().isdigit():
            c = [x.strip() for x in key.rsplit(",", 1)]
            out["clone"][(c[0], c[1] if len(c) > 1 else "")] = int(rest)
        elif name == "clone_ns":
            s = BT_STATS.match(rest)
            if s:
                out["clone_ns"] = [int(x) for x in s.groups()]
    return out


def fmt(v, nd=0):
    if v is None:
        return "-"
    if isinstance(v, str):
        return v
    return f"{v:.{nd}f}"


def geo(xs):
    xs = [x for x in xs if x and x > 0]
    if not xs:
        return None
    return math.exp(statistics.mean(math.log(x) for x in xs))


def mean(xs):
    xs = [x for x in xs if x is not None]
    return statistics.mean(xs) if xs else None


def passdirs(prefix):
    out = []
    for p in sorted(os.listdir(RAW)) if os.path.isdir(RAW) else []:
        if p.startswith(prefix) and os.path.isdir(os.path.join(RAW, p)):
            out.append(p)
    return out


def collect(p, arm, tag):
    d = os.path.join(RAW, p, arm)
    if not os.path.isfile(os.path.join(d, tag + ".b.txt")):
        return None
    mm = MM
    if tag in ("i0", "i1"):
        mm = read(os.path.join(L, "majmin-i")).strip()
    elif tag in ("d0", "d1") and os.path.isfile(os.path.join(L, "majmin-r")):
        mm = read(os.path.join(L, "majmin-r")).strip()
    r = {"pass": p, "arm": arm, "tag": tag,
         "b": fio(os.path.join(d, tag + ".b.json")),
         "a": fio(os.path.join(d, tag + ".a.json")),
         "c": fio(os.path.join(d, tag + ".c.json")),
         "sb": scope(os.path.join(d, tag + ".b.txt"), mm),
         "sa": scope(os.path.join(d, tag + ".a.txt"), mm),
         "sync": maxsync(d, tag), "lwbs": lwbs(d, tag),
         "bt": bt(os.path.join(d, tag + ".bt.txt"))}
    r["idle_q"], r["be_q"] = ddq(os.path.join(d, tag + ".dd.txt"))
    r["steal"], r["load1"] = cpu(d, tag)
    return r


def part_z():
    print("## Part Z: writes charged to an io.max writer (proposed test)\n")
    print("| arm | Z1 wios | Z1 dd s | Z1 | Z2 wios | Z2 dd s | Z2 | "
          "Z2 import rc |")
    print("|---|---|---|---|---|---|---|---|")
    res = {}
    for arm in ARMS:
        d = os.path.join(RAW, "Z", arm)
        if not os.path.isdir(d):
            continue
        cells = []
        for z in ("z1", "z2"):
            s = scope(os.path.join(d, z + ".txt"))
            v = None
            if s["found"] and s["wios"] is not None:
                v = "PASS" if s["wios"] <= 3 else "FAIL"
            res[(arm, z)] = v
            cells += [fmt(s["wios"]), fmt(s["secs"], 1), v or "-"]
        si = scope(os.path.join(d, "z2i.txt"))
        print(f"| {arm} | " + " | ".join(cells) + f" | {si['rc'] or '-'} |")
    print()
    want = {"master": "PASS", "v5": "FAIL", "v6": "PASS", "v6dbg": "PASS"}
    got = {a: res.get((a, "z1")) for a in want}
    ok = all(got[a] == want[a] for a in want)
    print("Z1 against the expectation (master PASS, v5 FAIL, v6 PASS, "
          "v6dbg PASS): " + ", ".join(f"{a} {got[a] or '-'}" for a in want)
          + " -> " + ("usable as a regression test" if ok else
                      "NOT as expected"))
    print()


def part_t():
    ps = passdirs("T")
    if not ps:
        return
    print("## Traced pass (c6g-clone.bt)\n")
    print("| pass | arm | row | B cgroup | B cg id | A cg id | lwbs | "
          "fio submits by cg id | clones by command, cg id | clone ns avg "
          "| ready | rc |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    checks = []
    for p in ps:
        for arm in ARMS:
            for tag in ("d0", "d1", "s1"):
                r = collect(p, arm, tag)
                if r is None:
                    continue
                b = r["bt"] or {"submit": {}, "clone": {}, "clone_ns": None,
                                "ready": False}
                sub = ", ".join(f"{k}: {v}" for k, v in b["submit"].items())
                cl = ", ".join(f"{k[0]} {k[1]}: {v}"
                               for k, v in b["clone"].items())
                cns = b["clone_ns"][1] if b["clone_ns"] else None
                print(f"| {p} | {arm} | {tag} | {r['sb']['cgroup']} | "
                      f"{r['sb']['cgid'] or '-'} | {r['sa']['cgid'] or '-'} | "
                      f"{fmt(r['lwbs'])} | {sub or '0'} | {cl or '0'} | "
                      f"{fmt(cns)} | {'yes' if b['ready'] else 'no'} | "
                      f"{r['sb']['rc']} |")
                fio_cl = sum(v for k, v in b["clone"].items()
                             if k[0] == "fio")
                fio_sub = sum(b["submit"].values())
                lw = r["lwbs"] or 0
                if arm in ("v6", "v6dbg") and tag == "d0":
                    checks.append((p, arm, tag, "fio clones 0", fio_cl == 0,
                                   fio_cl))
                if arm in ("v6", "v6dbg") and tag == "d1":
                    checks.append((p, arm, tag, "fio clones in "
                                   "[0.8 x lwbs, lwbs + 5]",
                                   0.8 * lw <= fio_cl <= lw + 5,
                                   f"{fio_cl} for {lw} lwbs"))
                if arm in ("master", "v5"):
                    checks.append((p, arm, tag, "fio clones 0", fio_cl == 0,
                                   fio_cl))
                if arm == "master":
                    checks.append((p, arm, tag, "fio submits 0",
                                   fio_sub == 0, fio_sub))
    print()
    print("Mechanism checks of the traced pass:\n")
    for p, arm, tag, what, ok, val in checks:
        print(f"- {p} {arm} {tag}: {what}: {val} -> "
              f"{'OK' if ok else 'MISMATCH'}")
    print()


def timed():
    ps = passdirs("P")
    rows = []
    for p in ps:
        for arm in ARMS:
            for tag in ROWS:
                r = collect(p, arm, tag)
                if r is not None:
                    rows.append(r)
    print("## Timed passes: every row (latencies in us)\n")
    print("| pass | arm | row | B IOPS | B fsync p50 | p99 | p99.9 | A IOPS | "
          "A wios | B wios | A ioprio | A throttled | IDLE q max | BE q "
          "mean | C MiB/s | max txg sync ms | steal % | load1 | rc A/B |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
          "---|---|---|")
    for r in rows:
        b, a, sa, sb = r["b"], r["a"], r["sa"], r["sb"]
        print(f"| {r['pass']} | {r['arm']} | {r['tag']} | {fmt(b.get('iops'))}"
              f" | {fmt(b.get('p50'))} | {fmt(b.get('p99'))} | "
              f"{fmt(b.get('p999'))} | {fmt(a.get('iops'))} | "
              f"{fmt(sa['wios'])} | {fmt(sb['wios'])} | "
              f"{sa['ioprio'] or '-'} | {fmt(sa['throttled'])} | "
              f"{fmt(r['idle_q'])} | {fmt(r['be_q'], 1)} | "
              f"{fmt(r['c'].get('bw'))} | {fmt(r['sync'], 1)} | "
              f"{fmt(r['steal'], 2)} | {fmt(r['load1'], 1)} | "
              f"{sa['rc'] or '-'}/{sb['rc']} |")
    print()
    print("Noise per pass: highest steal % and load1 range over its rows\n")
    for p in ps:
        s = [r for r in rows if r["pass"] == p]
        st = [r["steal"] for r in s if r["steal"] is not None]
        ld = [r["load1"] for r in s if r["load1"] is not None]
        print(f"- {p}: steal max {fmt(max(st) if st else None, 2)} %, "
              f"load1 {fmt(min(ld) if ld else None, 1)}-"
              f"{fmt(max(ld) if ld else None, 1)}")
    print()

    def sel(arm, tag):
        return {r["pass"]: r for r in rows
                if r["arm"] == arm and r["tag"] == tag}

    print("## Means over passes\n")
    print("| arm | row | n | B IOPS | B fsync p50 | B fsync p99 | B p99.9 | "
          "A IOPS | A wios | B wios |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for arm in ARMS:
        for tag in ROWS:
            s = list(sel(arm, tag).values())
            if not s:
                continue
            print(f"| {arm} | {tag} | {len(s)} | "
                  f"{fmt(mean([r['b'].get('iops') for r in s]))} | "
                  f"{fmt(mean([r['b'].get('p50') for r in s]))} | "
                  f"{fmt(mean([r['b'].get('p99') for r in s]))} | "
                  f"{fmt(mean([r['b'].get('p999') for r in s]))} | "
                  f"{fmt(mean([r['a'].get('iops') for r in s]))} | "
                  f"{fmt(mean([r['sa']['wios'] for r in s]))} | "
                  f"{fmt(mean([r['sb']['wios'] for r in s]))} |")
    print()

    def ratios(x, y, tag, key):
        """Per pass: x/y of B's key in row tag, over passes with both."""
        sx, sy = sel(x, tag), sel(y, tag)
        out = []
        for p in sorted(set(sx) & set(sy)):
            vx, vy = sx[p]["b"].get(key), sy[p]["b"].get(key)
            if vx and vy:
                out.append(vx / vy)
        return out

    def rline(name, rs):
        g = geo(rs)
        return (f"{name}: per pass " + ", ".join(f"{x:.3f}" for x in rs) +
                f"; geo {fmt(g, 3)}")

    print("## Verdicts (rules in the header of c6g-sum.py)\n")
    # CG
    print("### CG\n")
    for arm in ("master", "v5", "v6"):
        s0, s1, s2 = sel(arm, "s0"), sel(arm, "s1"), sel(arm, "s2")
        both = sorted(set(s0) & set(s1))
        if not both:
            continue
        drag = [s1[p]["b"]["iops"] / s0[p]["b"]["iops"] for p in both
                if s0[p]["b"].get("iops") and s1[p]["b"].get("iops")]
        aw = [r["sa"]["wios"] for r in list(s1.values()) + list(s2.values())
              if r["sa"]["wios"] is not None]
        held = max(aw) if aw else None
        notheld = (held is not None and held <= 3 and drag and
                   min(drag) >= 0.70)
        line = (f"- {arm}: A wios max over s1/s2 {fmt(held)}, A IOPS s1 mean "
                f"{fmt(mean([r['a'].get('iops') for r in s1.values()]))}; "
                f"B s1/s0 per pass " + ", ".join(f"{x:.2f}" for x in drag) +
                f" (mean {fmt(mean(drag), 2)})")
        if arm == "v5":
            rep = drag and mean(drag) <= 0.30
            line += " -> " + ("reproduces TASK-1117-V5CG2" if rep else
                              "does NOT reproduce TASK-1117-V5CG2")
        else:
            line += " -> " + ("not held" if notheld else "HELD or mismatch")
        print(line)
    print()
    # I
    print("### I\n")
    for arm in ("master", "v5", "v6"):
        s = sel(arm, "i1")
        if not s:
            continue
        prios = sorted({r["sa"]["ioprio"] for r in s.values()})
        iq = [r["idle_q"] for r in s.values() if r["idle_q"] is not None]
        print(f"- {arm}: A ioprio {', '.join(prios) or '-'}; IDLE queued max "
              f"per pass {', '.join(str(x) for x in iq) or '-'}; C MiB/s mean "
              f"{fmt(mean([r['c'].get('bw') for r in s.values()]))}")
    for arm in ("v5", "v6"):
        ri = ratios(arm, "master", "i1", "iops")
        rp = ratios(arm, "master", "i1", "p99")
        if not ri:
            continue
        gi, gp = geo(ri), geo(rp)
        nb = len(set(sel(arm, "i1")) & set(sel("master", "i1")))
        if len(ri) < nb or len(rp) < nb:
            gi = gp = None  # a pass where B completed no fsync
        print("- " + rline(f"{arm}/master i1 B IOPS", ri))
        print("- " + rline(f"{arm}/master i1 B fsync p99", rp))
        if arm == "v5":
            mv = mean([r["b"].get("iops") for r in sel("v5", "i1").values()])
            mm = mean([r["b"].get("iops") for r in
                       sel("master", "i1").values()])
            q = mv / mm if mv is not None and mm else None
            print(f"- v5/master i1 B IOPS, ratio of the means: {fmt(q, 3)}")
            v = ((q is not None and q <= 0.50) or
                 (gp is not None and gp >= 10))
            print(f"- v5: {'inversion reproduced' if v else 'inversion NOT reproduced'}")
        else:
            v = gi is not None and gi >= 0.85 and gp is not None and gp <= 1.5
            print(f"- v6: {'matches master' if v else 'does NOT match master'}")
    print()
    # C
    print("### C\n")
    worse = []
    for tag in ("c1", "c1e"):
        s = sel("master", tag)
        th = [r["sa"]["throttled"] for r in list(s.values()) +
              list(sel("v6", tag).values())]
        r = ratios("v6", "master", tag, "iops")
        q = ratios("v6", "master", tag, "p99")
        if not r:
            continue
        gr, gq = geo(r), geo(q)
        cw = ((gr is not None and gr <= 0.90 and all(x < 1 for x in r)) or
              (gq is not None and gq >= 1.25 and all(x > 1 for x in q)))
        worse.append(cw)
        base = "s0" if tag == "c1" else "c0e"
        dm = {}
        for a in ("master", "v6"):
            sb, sx = sel(a, base), sel(a, tag)
            dm[a] = mean([sx[p]["b"]["iops"] / sb[p]["b"]["iops"]
                          for p in set(sb) & set(sx)
                          if sb[p]["b"].get("iops") and sx[p]["b"].get("iops")])
        print(f"- {tag}: A nr_throttled (master, v6) "
              f"{', '.join(fmt(x) for x in th)}")
        print("  - " + rline("v6/master B IOPS", r))
        print("  - " + rline("v6/master B fsync p99", q))
        print("  - " + rline("v5/v6 B IOPS (noise reference)",
                             ratios("v5", "v6", tag, "iops")))
        print(f"  - B {tag}/{base} (drag): master {fmt(dm['master'], 2)}, "
              f"v6 {fmt(dm['v6'], 2)}")
        print(f"  - {tag}: {'CLEARLY WORSE' if cw else 'within noise'}")
    if worse:
        print(f"- C decision: {'fallback' if any(worse) else 'keep v6'}")
    print()
    # D
    print("### D\n")
    for tag in ("d0", "d1"):
        for x, y in (("v6", "v5"), ("v6", "master"), ("v5", "master")):
            r = ratios(x, y, tag, "iops")
            if r:
                print(f"- {tag} " + rline(f"{x}/{y} IOPS", r) +
                      f", {x} lower in {sum(1 for v in r if v < 1)}/{len(r)}")
    r = ratios("v6", "v5", "d1", "iops")
    if r:
        g = geo(r)
        dc = g is not None and g <= 0.97 and all(x < 1 for x in r)
        print(f"- D decision: {'donor cost measurable' if dc else 'no donor cost shown'}")
    print()


def main():
    print("# TASK-1118-V6CG summary\n")
    part_z()
    part_t()
    timed()


main()
