#!/usr/bin/env python3
# xp-report.py: tables and verdicts of TASK-1120-X3, TASK-1120-X4 and
# TASK-1122-X5 (the XPOOL bench, tests/perf-xpool/run.sh qv, one OUT
# directory per pass as xp-drive.sh leaves them in RAW/<group>/p<k>; X5's
# Part B, x5-n8i.sh, in RAW/partb/p<k>/<arm>).  Standard library only.
#
#   xp-report.py x3 RAW              X3's report (groups run2, single, tput)
#   xp-report.py x4 RAW              X4's report (group run2)
#   xp-report.py x5 RAW              X5's report (run2, single, partb)
#   xp-report.py rule1 RAW           X5's rule 1 alone; its last line is
#                                    "RULE 1: yes", "no" or "not decided"
#   xp-report.py layout TASK OUT     the layout check of one pass (exit 1 on
#                                    a difference)
#   xp-report.py pblayout DIR        the same for one pass of X5's Part B
#   xp-report.py retro LABEL=ROOT... X4's step 8: the rows of m3w, m3w-up2,
#                                    m3w-w2, m3w-w2b and m3w-lowpri of every
#                                    pass found under each ROOT
#
# "Beyond the spread": the two [min, max] ranges over the passes do not
# overlap.  Percentages compare means.  The baseline is m3w and m3w-up2
# together.  Latencies in microseconds.

import itertools
import json
import math
import os
import re
import statistics
import sys
import time

WLS = ["rr1", "rr4", "sw1", "sw4"]
LAT = [("iops", "IOPS"), ("p50", "p50"), ("p99", "p99"), ("p999", "p99.9")]
BASE = ("m3w", "m3w-up2")


def readf(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def kstat_named(path):
    """A named kstat (header, column line, then name type data)."""
    out = {}
    for line in (readf(path, "") or "").splitlines()[2:]:
        p = line.split()
        if len(p) == 3:
            try:
                out[p[0]] = int(p[2])
            except ValueError:
                pass
    return out


def kstat_snaptime(path):
    """The last field of a kstat's header line: its snaptime, in ns."""
    txt = readf(path, "")
    if not txt:
        return None
    try:
        return int(txt.splitlines()[0].split()[-1])
    except (IndexError, ValueError):
        return None


def tx_assign(path):
    """dmu_tx_assign histogram: {upper bound ns: count}."""
    out = {}
    for line in (readf(path, "") or "").splitlines()[2:]:
        p = line.split()
        if len(p) == 4 and p[1] == "ns":
            try:
                out[int(p[0])] = int(p[3])
            except ValueError:
                pass
    return out


def fio_jobs(path):
    txt = readf(path)
    if not txt or "{" not in txt:
        return None
    try:
        data = json.loads(txt[txt.index("{"):])
    except ValueError:
        return None
    out = []
    for job in data.get("jobs") or []:
        for k in ("read", "write"):
            d = job.get(k) or {}
            if not d.get("total_ios"):
                continue
            lat = d.get("lat_ns") or {}
            pct = lat.get("percentile") or \
                (d.get("clat_ns") or {}).get("percentile") or {}

            def p(key):
                v = pct.get(key)
                return v / 1000.0 if v is not None else None
            mx = (d.get("clat_ns") or {}).get("max") or lat.get("max")
            out.append({"iops": d.get("iops"),
                        "ios": d.get("total_ios"),
                        "bw": d.get("bw_bytes", 0) / 1048576.0,
                        "p50": p("50.000000"), "p99": p("99.000000"),
                        "p999": p("99.900000"),
                        "max_ns": mx})
            break
    return out or None


def stat_line(path):
    txt = readf(path, "")
    if not txt:
        return None
    try:
        return [int(x) for x in txt.split()[1:]]
    except ValueError:
        return None


def stat_split(pre, post):
    """Shares of /proc/stat over a window, in %: user nice system irq
    (irq + softirq) iowait idle steal."""
    a, b = stat_line(pre), stat_line(post)
    if not a or not b:
        return None
    d = [y - x for x, y in zip(a, b)][:8]
    while len(d) < 8:
        d.append(0)
    t = sum(d) or 1
    return {"user": 100.0 * d[0] / t, "nice": 100.0 * d[1] / t,
            "system": 100.0 * d[2] / t, "irq": 100.0 * (d[5] + d[6]) / t,
            "iowait": 100.0 * d[4] / t, "idle": 100.0 * d[3] / t,
            "steal": 100.0 * d[7] / t}


def first_num(path):
    txt = readf(path, "")
    try:
        return float(txt.split()[0])
    except (IndexError, ValueError, AttributeError):
        return None


FAMILIES = [
    ("fio", re.compile(r"^fiofast$")),
    ("fast_int", re.compile(r"^z_(rd|wr|null)_int")),
    ("fast_iss", re.compile(r"^z_wr_iss")),
    ("slow_iss", re.compile(r"^s_wr_iss")),
    ("slow_int", re.compile(r"^s_(rd|wr|null)_int")),
    ("slow_all", re.compile(r"^s_")),
    ("slow_fio", re.compile(r"^fio$")),
    ("evict", re.compile(r"^(arc_evict|dbuf_evict)")),
    ("txg_sync", re.compile(r"^txg_sync$")),
    ("dp_sync", re.compile(r"^dp_sync_taskq")),
]


def sched(path):
    txt = readf(path)
    if not txt:
        return None
    try:
        d = json.loads(txt)
    except ValueError:
        return None
    secs = d.get("secs") or 1.0
    fam = {}
    for name, rx in FAMILIES:
        r = w = n = 0
        for c, v in d.get("comm", {}).items():
            if rx.search(c):
                r += v[0]
                w += v[1]
                n += v[2]
        fam[name] = {"cpus": r / 1e9 / secs, "wait_us": w / 1e3 / max(1, n),
                     "wait_cpus": w / 1e9 / secs}
    return fam


def diskstats(path):
    """{device: sectors written}."""
    out = {}
    for line in (readf(path, "") or "").splitlines():
        p = line.split()
        if len(p) >= 10:
            try:
                out[p[2]] = int(p[9])
            except ValueError:
                pass
    return out


# A taskq kstat: family (z_wr_iss, z_wr_iss_h, z_rd_int_h, ...), the
# taskq's index within it, and its instance number.
TQ_RE = re.compile(r"^([sz]_(?:wr|rd)_(?:iss|int))(_h)?(_\d+)?\.(\d+)$")


def tasks(path):
    """{kstat name: tasks_executed} from a grep -H of taskq kstats."""
    out = {}
    for line in (readf(path, "") or "").splitlines():
        if ":" not in line:
            continue
        p, rest = line.split(":", 1)
        v = rest.split()
        if len(v) >= 3 and v[0] == "tasks_executed":
            try:
                out[os.path.basename(p)] = int(v[-1])
            except ValueError:
                pass
    return out


def tasks_by_family(pre, post):
    """{family: tasks executed between the two snapshots}, or None."""
    a, b = tasks(pre), tasks(post)
    if not b:
        return None
    fam = {}
    for k, v in b.items():
        m = TQ_RE.match(k)
        if m:
            fn = m.group(1) + (m.group(2) or "")
            fam[fn] = fam.get(fn, 0) + v - a.get(k, 0)
    return fam


SLOW_DEVS = ["nullb%d" % i for i in range(2, 20)]


def slow_dev_mibs(pre, post, t0, t1):
    a, b = diskstats(pre), diskstats(post)
    if not a or not b or t0 is None or t1 is None or t1 <= t0:
        return None
    s = sum(b.get(d, 0) - a.get(d, 0) for d in SLOW_DEVS if d in a)
    return s * 512 / 1048576.0 / (t1 - t0)


def lat_log(paths, json_max_ns):
    """Seconds whose largest completion latency is 2 ms or more, and the
    largest value (us), from fio's per-second max-value logs."""
    vals = []
    for p in paths:
        for line in (readf(p, "") or "").splitlines():
            f = line.split(",")
            if len(f) >= 2:
                v = num(f[1])
                if v is not None:
                    vals.append(v)
    if not vals:
        return None
    mx = max(vals)
    unit_ns = True
    if json_max_ns and mx > 0 and json_max_ns / mx > 100:
        unit_ns = False          # the log is in us
    us = [v / 1000.0 if unit_ns else v for v in vals]
    return {"sec2ms": sum(1 for v in us if v >= 2000.0), "max_us": max(us),
            "unit": "ns" if unit_ns else "us"}


def txgs_synced(path, w0, w1):
    """Slow txgs whose sync ended in [w0, w1] (ns) and their mean stime."""
    lines = (readf(path, "") or "").splitlines()
    if not lines or w0 is None or w1 is None:
        return None
    n = 0
    st = []
    earliest = None
    for line in lines[2:]:
        p = line.split()
        if len(p) < 12:
            continue
        try:
            birth = int(p[1])
            ot, qt, wt, stt = (int(x) for x in p[8:12])
        except ValueError:
            continue
        earliest = birth if earliest is None else min(earliest, birth)
        end = birth + ot + qt + wt + stt
        if stt and w0 <= end <= w1:
            n += 1
            st.append(stt / 1e6)
    if earliest is None or earliest > w0:
        return None          # the history does not reach back to the row
    return {"n": n, "stime_ms": statistics.mean(st) if st else None}


# ---------------------------------------------------------------- passes --

class Row:
    pass


def load_pass(out):
    """One run.sh qv OUT directory."""
    ps = {"out": out, "phases": [], "rows": {}, "phase": {}}
    for line in (readf(os.path.join(out, "phases.txt"), "") or "").splitlines():
        p = line.split(None, 1)
        if p:
            ps["phases"].append(p[0])
    t_start = first_num(os.path.join(out, "idle.pre.uptime"))
    rdir = os.path.join(out, "runs")
    try:
        labels = sorted(os.listdir(rdir))
    except OSError:
        labels = []
    for label in labels:
        m = re.match(r"^qv\.(.+)\.([a-z0-9]+)\.r(\d+)$", label)
        if not m:
            continue
        arm, wl = m.group(1), m.group(2)
        d = os.path.join(rdir, label)
        r = Row()
        r.arm, r.wl, r.dir = arm, wl, d
        r.rc = first_num(os.path.join(d, "rc"))
        r.dead = os.path.exists(os.path.join(d, "slow-load"))
        r.jobs = fio_jobs(os.path.join(d, "fio.json"))
        j = r.jobs[0] if r.jobs else {}
        r.iops, r.p50, r.p99, r.p999 = (j.get(k) for k in
                                        ("iops", "p50", "p99", "p999"))
        r.bw = j.get("bw")
        r.ios = j.get("ios")
        r.ok = r.rc == 0 and not r.dead and bool(r.jobs)
        # X5: tasks run by the fast pool's write issue and read interrupt
        # taskqs over the row.
        r.tq = {}
        for k in ("wrtasks", "rdtasks"):
            x = tasks_by_family(os.path.join(d, "pre." + k),
                                os.path.join(d, "post." + k))
            if x:
                r.tq.update(x)
        if wl == "tz2" and r.jobs and len(r.jobs) >= 2:
            r.bw0, r.bw1 = r.jobs[0]["bw"], r.jobs[1]["bw"]
            r.bwsum = r.bw0 + r.bw1
            r.p99_0, r.p99_1 = r.jobs[0]["p99"], r.jobs[1]["p99"]
        r.split = stat_split(os.path.join(d, "pre.stat"),
                             os.path.join(d, "post.stat"))
        r.steal = r.split["steal"] if r.split else None
        r.load1 = first_num(os.path.join(d, "post.loadavg"))
        r.t0 = first_num(os.path.join(d, "pre.uptime"))
        r.t1 = first_num(os.path.join(d, "post.uptime"))
        r.mins = (r.t0 - t_start) / 60.0 if (r.t0 is not None and
                                              t_start is not None) else None
        r.sched = sched(os.path.join(d, "sched.json"))
        wb0 = kstat_named(os.path.join(d, "pre.wbudget"))
        wb1 = kstat_named(os.path.join(d, "post.wbudget"))
        r.wb = None
        if wb0 and wb1:
            dd = {k: wb1.get(k, 0) - wb0.get(k, 0) for k in wb1}
            g = dd.get("gated", 0)
            secs = (r.t1 - r.t0) if (r.t0 is not None and
                                     r.t1 is not None) else None
            r.wb = {"gated": g,
                    "gated_s": g / secs if secs else None,
                    "waited": (dd.get("waited", 0) / g) if g else None,
                    "N": (dd.get("npools_sum", 0) / g) if g else None,
                    "limit": (dd.get("limit_sum", 0) / g) if g else None,
                    "npools_end": wb1.get("npools")}
        # X4: counters, tx assigns, slow device rate, tail mode, txgs.
        r.cnt = {}
        for f in os.listdir(d):
            m2 = re.match(r"^pre\.iostats\.(.+)$", f)
            if m2:
                pool = m2.group(1)
                a = kstat_named(os.path.join(d, f))
                b = kstat_named(os.path.join(d, "post.iostats." + pool))
                if a and b:
                    r.cnt[pool] = {k: b.get(k, 0) - a.get(k, 0) for k in
                                   ("txg_wait_explicit_count",
                                    "txg_wait_restart_count",
                                    "tx_dirty_wait_count",
                                    "waited_async_write_count")
                                   if k in b}
        r.txa = {}
        for f in os.listdir(d):
            m2 = re.match(r"^pre\.txassign\.(.+)$", f)
            if m2:
                pool = m2.group(1)
                a = tx_assign(os.path.join(d, f))
                b = tx_assign(os.path.join(d, "post.txassign." + pool))
                if a and b:
                    r.txa[pool] = sum(b[k] - a.get(k, 0) for k in b
                                      if k >= 2097152)
        r.devmibs = slow_dev_mibs(os.path.join(d, "pre.diskstats"),
                                  os.path.join(d, "post.diskstats"),
                                  r.t0, r.t1)
        logs = sorted(os.path.join(d, f) for f in os.listdir(d)
                      if re.match(r"^fl_clat\.\d+\.log$", f))
        r.tail = lat_log(logs, j.get("max_ns")) if logs else None
        r.mode = None
        if wl in ("rr1", "rr4") and r.p99 is not None:
            r.mode = "low" if r.p99 < 1000 else "high"
        r.w0 = kstat_snaptime(os.path.join(d, "pre.arcstats"))
        r.w1 = kstat_snaptime(os.path.join(d, "post.arcstats"))
        arc = kstat_named(os.path.join(d, "pre.arcstats"))
        r.arc_size = arc.get("size")
        r.arc_anon = arc.get("anon_size")
        ps["rows"][(arm, wl)] = r
    if not ps["phases"]:
        # An archive without phases.txt: the order of run.sh's "qv phase"
        # lines, else of the arms' first rows.
        for line in (readf(os.path.join(out, "run.log"), "") or
                     "").splitlines():
            m = re.search(r"qv phase (\S+) \(", line)
            if m and m.group(1) not in ps["phases"]:
                ps["phases"].append(m.group(1))
        arms = set(k[0] for k in ps["rows"])
        if not arms.issubset(ps["phases"]):
            first = {}
            for (arm, _wl), r in ps["rows"].items():
                if r.t0 is not None:
                    first[arm] = min(first.get(arm, r.t0), r.t0)
            ps["phases"] = sorted(arms, key=lambda x: first.get(x, 0))
    for i, arm in enumerate(ps["phases"]):
        ph = {"pos": i + 1, "before": ps["phases"][i - 1] if i else None}
        pd = os.path.join(out, "phase")
        bws = []
        for k in (1, 2, 3):
            js = fio_jobs(os.path.join(out, "slow.%s.slow%d.json" % (arm, k)))
            if js:
                bws.append(js[0]["bw"])
        ph["slow"] = sum(bws) if len(bws) == 3 else None
        u0 = first_num(os.path.join(pd, arm + ".start", "uptime"))
        u1 = first_num(os.path.join(pd, arm + ".end", "uptime"))
        uw = first_num(os.path.join(pd, arm + ".warm0.uptime"))
        ph["len_min"] = (u1 - u0) / 60.0 if (u0 and u1) else None
        ph["warm_dev"] = slow_dev_mibs(
            os.path.join(pd, arm + ".warm0.diskstats"),
            os.path.join(pd, arm + ".start", "diskstats"), uw, u0)
        ph["phase_dev"] = slow_dev_mibs(
            os.path.join(pd, arm + ".warm0.diskstats"),
            os.path.join(pd, arm + ".end", "diskstats"), uw, u1)
        ph["end"] = os.path.join(pd, arm + ".end")
        ps["phase"][arm] = ph
    for (arm, wl), r in ps["rows"].items():
        ph = ps["phase"].get(arm, {})
        r.pos = ph.get("pos")
        r.before = ph.get("before")
        r.slow = ph.get("slow")
        r.txg = None
        if ph.get("end"):
            res = [txgs_synced(os.path.join(ph["end"], "slow%d.txgs" % k),
                               r.w0, r.w1) for k in (1, 2, 3)]
            if all(x is not None for x in res):
                n = sum(x["n"] for x in res)
                st = [x["stime_ms"] for x in res if x["stime_ms"] is not None]
                r.txg = {"n": n, "stime_ms": statistics.mean(st)
                         if st else None}
    ps["ndiag"] = len([x for x in labels if x.startswith(("qvdiag.",
                                                          "qvsched."))])
    ps["t_start"] = t_start
    return ps


def load_group(raw, group):
    d = os.path.join(raw, group)
    out = []
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in sorted(names, key=lambda s: int(s[1:]) if s[1:].isdigit()
                    else 0):
        if re.match(r"^p\d+$", n) and os.path.isdir(os.path.join(d, n)):
            ps = load_pass(os.path.join(d, n))
            ps["name"] = n
            out.append(ps)
    return out


# ----------------------------------------------------------- statistics --

def series(passes, arms, wl, metric):
    """Values of METRIC for row WL over every pass, for one arm or a list
    of arms pooled (the baseline)."""
    if isinstance(arms, str):
        arms = (arms,)
    v = []
    for ps in passes:
        for a in arms:
            r = ps["rows"].get((a, wl))
            if r is not None and r.ok:
                x = getattr(r, metric, None)
                if x is not None:
                    v.append(x)
    return v


def slow_series(passes, arms):
    if isinstance(arms, str):
        arms = (arms,)
    v = []
    for ps in passes:
        for a in arms:
            x = ps["phase"].get(a, {}).get("slow")
            if x is not None:
                v.append(x)
    return v


def cmpv(a, b):
    """(pct change of means a vs b, beyond the spread?, sign)."""
    if len(a) < 2 or len(b) < 2:
        return None          # an arm run once is not compared
    ma, mb = statistics.mean(a), statistics.mean(b)
    if mb == 0:
        return None
    beyond = max(a) < min(b) or max(b) < min(a)
    return {"pct": 100.0 * (ma - mb) / mb, "beyond": beyond,
            "up": ma > mb, "ma": ma, "mb": mb, "na": len(a), "nb": len(b)}


def f(x, nd=0):
    if x is None:
        return "-"
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return "-"
    return ("%%.%df" % nd) % x


def mr(v, nd=0):
    """mean [min-max]."""
    if not v:
        return "-"
    return "%s [%s-%s]" % (f(statistics.mean(v), nd), f(min(v), nd),
                           f(max(v), nd))


def sgn(p):
    return "%+.0f %%" % p


def cell(passes, a, b, wl, metrics=LAT):
    """The metrics of row WL that differ beyond the spread, A against B."""
    out = []
    for m, lab in metrics:
        c = cmpv(series(passes, a, wl, m), series(passes, b, wl, m))
        if c and c["beyond"]:
            out.append("%s %s" % (lab, sgn(c["pct"])))
    return "; ".join(out) if out else "-"


def slow_cell(passes, a, b):
    c = cmpv(slow_series(passes, a), slow_series(passes, b))
    if c is None:
        return "n/a"
    return sgn(c["pct"]) if c["beyond"] else "-"


def name(a):
    return "baseline" if a == BASE else a


def present(passes, arm):
    return any(any(k[0] == arm for k in ps["rows"]) for ps in passes)


def npasses(passes, arm, wl):
    return len(series(passes, arm, wl, "iops"))


# ---------------------------------------------------------------- tables --

def per_pass_tables(passes, group, wls, arms, tput=False):
    print("\n### Per-pass tables, %s\n" % group)
    for wl in wls:
        print("\n#### %s %s\n" % (group, wl))
        if wl == "tz2":
            print("| arm | pass | pos | fastq MiB/s | fastr MiB/s | total | "
                  "fastq p99 us | fastr p99 us | steal % | load1 (end) |")
            print("|---|---|---|---|---|---|---|---|---|---|")
        else:
            print("| arm | pass | pos | %s | p50 us | p99 us | p99.9 us | "
                  "steal %% | load1 (end) |" % ("MiB/s" if tput else "IOPS"))
            print("|---|---|---|---|---|---|---|---|---|")
        for a in arms:
            got = False
            for ps in passes:
                r = ps["rows"].get((a, wl))
                if r is None:
                    continue
                got = True
                flag = "" if r.ok else " (rc %s%s)" % (
                    f(r.rc), ", slow load dead" if r.dead else "")
                if wl == "tz2":
                    print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
                          % (a + flag, ps["name"], r.pos,
                             f(getattr(r, "bw0", None), 1),
                             f(getattr(r, "bw1", None), 1),
                             f(getattr(r, "bwsum", None), 1),
                             f(getattr(r, "p99_0", None)),
                             f(getattr(r, "p99_1", None)),
                             f(r.steal, 2), f(r.load1, 2)))
                else:
                    print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                        a + flag, ps["name"], r.pos,
                        f(r.bw if tput else r.iops, 1 if tput else 0),
                        f(r.p50, 1), f(r.p99), f(r.p999), f(r.steal, 2),
                        f(r.load1, 2)))
            if not got:
                continue
            if wl == "tz2":
                ms = ["bw0", "bw1", "bwsum", "p99_0", "p99_1"]
            else:
                ms = ["bw" if tput else "iops", "p50", "p99", "p999"]
            vs = [series(passes, a, wl, m) for m in ms]
            n = len(vs[0])
            sv = series(passes, a, wl, "steal")
            lv = series(passes, a, wl, "load1")
            print("| **%s** | **mean (n=%d)** | | %s | %s | %s |" % (
                a, n, " | ".join("**%s**" % f(statistics.mean(v), 1)
                                 if v else "-" for v in vs),
                f(statistics.mean(sv), 2) if sv else "-",
                f(statistics.mean(lv), 2) if lv else "-"))
            print("| %s | spread (max-min) | | %s | %s | %s |" % (
                a, " | ".join(f(max(v) - min(v), 1) if v else "-"
                              for v in vs),
                f(max(sv) - min(sv), 2) if sv else "-",
                f(max(lv) - min(lv), 2) if lv else "-"))


def noise(passes, group):
    print("\n### Noise, %s\n" % group)
    print("| pass | rows | rows rc != 0 or slow load dead | highest steal % "
          "| load1 range (row ends) | rows with steal > 1 % |")
    print("|---|---|---|---|---|---|")
    for ps in passes:
        rows = list(ps["rows"].values())
        bad = [r for r in rows if not r.ok]
        st = [r.steal for r in rows if r.steal is not None]
        ld = [r.load1 for r in rows if r.load1 is not None]
        hi = ["%s/%s %.2f" % (r.arm, r.wl, r.steal) for r in rows
              if r.steal is not None and r.steal > 1.0]
        print("| %s | %d | %s | %s | %s | %s |" % (
            ps["name"], len(rows),
            ", ".join("%s/%s" % (r.arm, r.wl) for r in bad) or "0",
            f(max(st), 2) if st else "-",
            "%s-%s" % (f(min(ld), 2), f(max(ld), 2)) if ld else "-",
            "; ".join(hi) or "none"))


def diff_table(passes, title, pairs, wls=WLS, slow=True):
    print("\n### %s\n" % title)
    print("| arm | against | %s%s |" % (" | ".join(wls),
                                       " | slow MiB/s" if slow else ""))
    print("|---|---|%s%s" % ("---|" * len(wls), "---|" if slow else ""))
    for a, b in pairs:
        if not present(passes, a if isinstance(a, str) else a[0]) or \
                not present(passes, b if isinstance(b, str) else b[0]):
            print("| %s | %s | not run |" % (name(a), name(b)))
            continue
        na = min(len(series(passes, x, wl, "iops")) for x in (a, b)
                 for wl in wls)
        if na < 2:
            print("| %s | %s | n=%d, not compared |" % (name(a), name(b), na))
            continue
        cells = [cell(passes, a, b, wl) for wl in wls]
        if slow:
            cells.append(slow_cell(passes, a, b))
        print("| %s | %s | %s |" % (name(a), name(b), " | ".join(cells)))


def means_table(passes, arms, title):
    print("\n### %s (mean [min-max] over the passes)\n" % title)
    print("| arm | n | rr1 IOPS | rr1 p99 | rr4 IOPS | rr4 p99 | sw1 IOPS | "
          "sw1 p99.9 | sw4 IOPS | sw4 p99 | sw4 p99.9 | slow MiB/s |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for a in arms:
        if not present(passes, a if isinstance(a, str) else a[0]):
            continue
        print("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
              % (name(a), npasses(passes, a, "rr1"),
                 mr(series(passes, a, "rr1", "iops")),
                 mr(series(passes, a, "rr1", "p99")),
                 mr(series(passes, a, "rr4", "iops")),
                 mr(series(passes, a, "rr4", "p99")),
                 mr(series(passes, a, "sw1", "iops")),
                 mr(series(passes, a, "sw1", "p999")),
                 mr(series(passes, a, "sw4", "iops")),
                 mr(series(passes, a, "sw4", "p99")),
                 mr(series(passes, a, "sw4", "p999")),
                 mr(slow_series(passes, a), 1)))


def controls(passes, arms):
    print("\n### Controls per arm (mean [min-max])\n")
    print("| arm | slow MiB/s | fio wait_us rr1 | fio wait_us sw4 | "
          "slow_iss cpus rr1 | slow_iss wait_cpus rr1 | slow_iss cpus sw4 "
          "| slow_iss wait_cpus sw4 | txg_sync cpus sw4 |")
    print("|---|---|---|---|---|---|---|---|---|")

    def sv(a, wl, fam, k):
        if isinstance(a, str):
            a = (a,)
        v = []
        for ps in passes:
            for x in a:
                r = ps["rows"].get((x, wl))
                if r is not None and r.ok and r.sched:
                    v.append(r.sched[fam][k])
        return v
    for a in arms:
        if not present(passes, a):
            continue
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            a, mr(slow_series(passes, a), 1),
            mr(sv(a, "rr1", "fio", "wait_us"), 1),
            mr(sv(a, "sw4", "fio", "wait_us"), 1),
            mr(sv(a, "rr1", "slow_iss", "cpus"), 2),
            mr(sv(a, "rr1", "slow_iss", "wait_cpus"), 2),
            mr(sv(a, "sw4", "slow_iss", "cpus"), 2),
            mr(sv(a, "sw4", "slow_iss", "wait_cpus"), 2),
            mr(sv(a, "sw4", "txg_sync", "cpus"), 2)))


def position_table(passes, arms):
    print("\n### Arm positions\n")
    print("| arm | %s |" % " | ".join(ps["name"] for ps in passes))
    print("|---|%s" % ("---|" * len(passes)))
    for a in arms:
        if not present(passes, a):
            continue
        print("| %s | %s |" % (a, " | ".join(
            str(ps["phase"][a]["pos"]) if a in ps["phase"] else "-"
            for ps in passes)))


# ---------------------------------------------------------------- layout --

def layout_of(phase_dir):
    """{family: {"count": taskqs per pool, "max": set, "nice": set}} from a
    phase snapshot, families z_wr_iss, z_wr_iss_h, s_wr_iss, s_wr_iss_h,
    z_wr_int, s_wr_int, and their read counterparts (z_rd_int_h, ...)."""
    fam = {}
    for line in (readf(os.path.join(phase_dir, "taskq.max"), "") or
                 "").splitlines():
        if ":" not in line:
            continue
        path, rest = line.split(":", 1)
        m = TQ_RE.match(os.path.basename(path))
        p = rest.split()
        if not m or len(p) < 3:
            continue
        fn = m.group(1) + (m.group(2) or "")
        x = fam.setdefault(fn, {"bases": set(), "inst": set(), "max": set(),
                                "nice": set()})
        x["bases"].add(m.group(3) or "")
        x["inst"].add(int(m.group(4)))
        try:
            x["max"].add(int(p[-1]))
        except ValueError:
            pass
    for line in (readf(os.path.join(phase_dir, "taskq.nice"), "") or
                 "").splitlines():
        p = line.split()
        if len(p) != 3:
            continue
        m = re.match(r"^([sz]_(?:wr|rd)_(?:iss|int))(_h)?(_\d+)?$", p[2])
        if not m:
            continue
        fn = m.group(1) + (m.group(2) or "")
        x = fam.setdefault(fn, {"bases": set(), "inst": set(), "max": set(),
                                "nice": set()})
        try:
            x["nice"].add(int(p[1]))
        except ValueError:
            pass
    for x in fam.values():
        x["count"] = len(x["bases"])
    return fam


def params_of(phase_dir):
    out = {}
    for line in (readf(os.path.join(phase_dir, "params"), "") or
                 "").splitlines():
        p = line.split(None, 1)
        if len(p) == 2:
            out[p[0]] = p[1].strip()
    return out


def fmt_fam(x):
    if not x or not x["count"]:
        return "absent"
    return "%d x %s, nice %s" % (
        x["count"], "/".join(str(v) for v in sorted(x["max"])) or "?",
        "/".join(str(v) for v in sorted(x["nice"])) or "no thread")


C1W = "sync scale scale null"
UPW = "sync null scale null"

# Expected layouts (8 vCPU, batch and write pct 80): family -> (count,
# threads_max, nice) with None for "absent"; nice None when not checked.
X3_LAYOUT = {}
for _a in ("m3w", "m3w-up2", "m3w-gate-on", "alone", "alone-gate-on",
           "tput", "tput-gate-on", "m3w-c1", "m3w-gate-on-c1", "m3w-pct25w",
           "m3w-wiss2", "m3w-wint2", "m3w-pct25", "m3w-c1-pct25w",
           "m3w-c1-pct25"):
    c1 = "c1" in _a
    e = {"z_wr_iss": (1, 6, -19),
         "z_wr_iss_h": (2, 3, -20) if c1 else None}
    if not _a.startswith(("alone", "tput")):
        e["s_wr_iss"] = (1, 2 if _a in ("m3w-pct25w", "m3w-c1-pct25w",
                                        "m3w-wiss2") else 6, -19)
        if c1:
            e["s_wr_iss_h"] = (1, 2, -20) if "pct25" in _a else (2, 3, -20)
        else:
            e["s_wr_iss_h"] = None
        e["s_wr_int"] = (1, 2, -20) if _a in (
            "m3w-pct25w", "m3w-c1-pct25w", "m3w-wint2", "m3w-pct25",
            "m3w-c1-pct25") else (2, 3, -20)
    X3_LAYOUT[_a] = e

X4_LAYOUT = {}
for _a, (fn, sn, pp, ww, low) in {
        "m3w": (-19, -19, 100, 0, 0), "m3w-up2": (-19, -19, 100, 0, 0),
        "m3w-c1": (-19, -19, 100, 0, 0), "m3w-lowpri": (0, 0, 100, 0, 1),
        "m3w-w2": (0, 0, 100, 0, 1), "m3w-w2b": (0, 0, 100, 0, 1),
        "m3w-w2x": (0, 0, 100, 2, 1), "m3w-w2x1": (0, 0, 100, 1, 1),
        "m3w-n10": (-10, -10, 50, 0, 1), "m3w-n5": (-5, -5, 75, 0, 1),
        "m3w-lowslow": (-19, 0, 100, 0, 0),
        "m3w-lowfast": (0, -19, 100, 0, 1)}.items():
    c1 = _a not in ("m3w", "m3w-up2")
    X4_LAYOUT[_a] = {
        "z_wr_iss": (1, 6, fn), "s_wr_iss": (1, 6, sn),
        "z_wr_iss_h": (2, 3, -20) if c1 else None,
        "s_wr_iss_h": (2, 3, -20) if c1 else None,
        "s_wr_int": (2, 3, -20),
        "_params": {"zio_taskq_write": C1W if c1 else UPW,
                    "zio_taskq_bulk_pri_pct": str(pp),
                    "zio_taskq_write_waited": str(ww),
                    "zio_taskq_bulk_lowpri": str(low)}}


# X5: the knob sets of run.sh (up, c1, c2) and zil_issue_inline (zi=).
C2R = "fixed,1,8 null scale scale"
UPR = "fixed,1,8 null scale null"
X5_RUN2 = ["m3w", "m3w-zi", "m3w-c1", "m3w-up2", "m3w-zi-c1", "m3w-c2"]
X5_SINGLE = ["alone", "alone-c1", "alone-zi", "alone-zi-c1"]
X5_LAYOUT = {}
for _a in X5_RUN2 + X5_SINGLE:
    c1 = _a.endswith("c1")
    c2 = _a.endswith("c2")
    e = {"z_wr_iss": (1, 6, -19),
         "z_wr_iss_h": (2, 3, -20) if c1 else None,
         "z_rd_int_h": (2, 3, -20) if c2 else None}
    if _a.startswith("m3w"):
        e["s_wr_iss"] = (1, 6, -19)
        e["s_wr_iss_h"] = (2, 3, -20) if c1 else None
        e["s_wr_int"] = (2, 3, -20)
        e["s_rd_int_h"] = (2, 3, -20) if c2 else None
    e["_params"] = {"zio_taskq_write": C1W if c1 else UPW,
                    "zio_taskq_read": C2R if c2 else UPR,
                    "zil_issue_inline": "1" if "-zi" in _a else "0",
                    "zio_taskq_write_budget": "0",
                    "zio_taskq_bulk_pri_pct": "100",
                    "zio_taskq_write_waited": "0",
                    "zio_taskq_bulk_lowpri": "0"}
    X5_LAYOUT[_a] = e
# X5's Part B (x5-n8i.sh): one pool, c1n.
PB_ARMS = ["master", "v10a", "v10a-c1"]
PB_LAYOUT = {}
for _a in PB_ARMS:
    c1 = _a.endswith("c1")
    PB_LAYOUT[_a] = {
        "z_wr_iss": (1, 6, -19), "z_wr_iss_h": (2, 3, -20) if c1 else None,
        "_params": {"zio_taskq_write": C1W if c1 else UPW,
                    "zio_taskq_read": UPR,
                    "zil_issue_inline": "0" if _a == "master" else "1",
                    "zio_taskq_write_budget": "0",
                    "zio_taskq_bulk_pri_pct": "100",
                    "zio_taskq_write_waited": "0",
                    "zio_taskq_bulk_lowpri": "0"}}


def x3_params(a):
    p = {"zio_taskq_write": C1W if "c1" in a else UPW,
         "zio_taskq_write_budget": "1" if "gate-on" in a else "0"}
    return p


def layout_diffs(e, lay, par):
    """The differences of one arm's layout LAY and parameters PAR from the
    expected E."""
    diffs = []
    for fn, want in e.items():
        if fn.startswith("_"):
            continue
        got = lay.get(fn)
        if want is None:
            if got and got["count"]:
                diffs.append("%s present (%s)" % (fn, fmt_fam(got)))
            continue
        if not got or not got["count"]:
            diffs.append("%s absent" % fn)
            continue
        cnt, mx, ni = want
        if got["count"] != cnt or got["max"] != {mx}:
            diffs.append("%s %s, expected %d x %d" % (fn, fmt_fam(got),
                                                      cnt, mx))
        elif ni is not None and got["nice"] and got["nice"] != {ni}:
            diffs.append("%s nice %s, expected %d" % (
                fn, "/".join(str(v) for v in sorted(got["nice"])), ni))
    for k, v in (e.get("_params") or {}).items():
        if par.get(k) != v:
            diffs.append("%s=%s, expected %s" % (k, par.get(k), v))
    return diffs


def per_op(r, fam):
    """Tasks of taskq family FAM per fio I/O of row R, or None."""
    if not r.ok or not r.ios or r.tq is None:
        return None
    return r.tq.get(fam, 0) / float(r.ios)


def routing_check(ps):
    """X5: where the fast pool's ZIL blocks and synchronous reads went.
    With c1 and the switch off a ZIL block is one task of z_wr_iss_h, with
    it on none (issued inline); with c2 a read completes in z_rd_int_h."""
    rows, bad = [], []
    for (a, wl), r in sorted(ps["rows"].items()):
        if a not in X5_LAYOUT:
            continue
        if not r.ok:
            rows.append("%s/%s failed, not checked" % (a, wl))
            continue
        h = per_op(r, "z_wr_iss_h")
        rh = per_op(r, "z_rd_int_h")
        rows.append("%s/%s iss_h %s rd_int_h %s" % (a, wl, f(h, 2),
                                                    f(rh, 2)))
        if h is None:
            bad.append("%s/%s (no task counts)" % (a, wl))
            continue
        c1 = a.endswith("c1")
        ok = True
        if not c1:
            ok = h == 0
        elif "-zi" in a:
            ok = not wl.startswith("sw") or h < 0.05
        elif wl == "sw1":
            ok = h >= 0.5
        elif wl.startswith("sw"):
            ok = h >= 0.1
        if a.endswith("c2"):
            ok = ok and (not wl.startswith("rr") or (rh or 0) >= 0.5)
        else:
            ok = ok and not rh
        if not ok:
            bad.append("%s/%s" % (a, wl))
    return rows, bad


def layout_check(task, out, quiet=False):
    """Compare each arm's phase-end layout of one pass with the expected
    table; returns the number of differences."""
    ps = load_pass(out)
    exp = {"x3": X3_LAYOUT, "x4": X4_LAYOUT, "x5": X5_LAYOUT}[task]
    bad = 0
    rows = []
    for a in ps["phases"]:
        d = os.path.join(out, "phase", a + ".end")
        lay = layout_of(d)
        par = params_of(d)
        e = exp.get(a)
        if e is None:
            rows.append((a, "no expected layout", "?"))
            bad += 1
            continue
        if task == "x3":
            e = dict(e, _params=x3_params(a))
        diffs = layout_diffs(e, lay, par)
        desc = "; ".join("%s: %s" % (fn, fmt_fam(lay.get(fn))) for fn in
                         ("z_wr_iss", "z_wr_iss_h", "s_wr_iss", "s_wr_iss_h",
                          "s_wr_int", "z_rd_int_h", "s_rd_int_h") if fn in e)
        keys = ["zio_taskq_write", "zio_taskq_write_budget",
                "zio_taskq_bulk_lowpri", "zio_taskq_bulk_yield",
                "zio_taskq_bulk_pri_pct", "zio_taskq_write_waited",
                "zio_taskq_batch_pct", "zio_taskq_write_pct"]
        if task == "x5":
            keys += ["zio_taskq_read", "zil_issue_inline"]
        desc += "; params " + ", ".join("%s=%s" % (k.replace("zio_taskq_",
                                                              ""), par[k])
                                        for k in keys if k in par)
        rows.append((a, desc, "; ".join(diffs) if diffs else "as expected"))
        bad += 1 if diffs else 0
    # The budget's and the exemption's counters, row by row.
    cbad = []
    crow = []
    for a in ps["phases"]:
        for wl in WLS + ["tz", "tz2"]:
            r = ps["rows"].get((a, wl))
            if r is None:
                continue
            if task == "x3":
                g = r.wb["gated"] if r.wb else None
                crow.append("%s/%s %s" % (a, wl, g))
                on = "gate-on" in a
                if g is None or (on and g <= 0) or (not on and g != 0):
                    cbad.append("%s/%s" % (a, wl))
            else:
                w = sum(c.get("waited_async_write_count", 0)
                        for c in r.cnt.values()) if r.cnt else None
                crow.append("%s/%s %s" % (a, wl, w))
                if w is None or (WW.get(a, 0) == 0 and w != 0):
                    cbad.append("%s/%s" % (a, wl))
    bad += len(cbad)
    rrows, rbad = routing_check(ps) if task == "x5" else ([], [])
    bad += len(rbad)
    if not quiet:
        print("\n### Layout check, %s\n" % "/".join(
            os.path.normpath(out).split(os.sep)[-2:]))
        print("| arm | seen at phase end | check |")
        print("|---|---|---|")
        for a, desc, chk in rows:
            print("| %s | %s | %s |" % (a, desc, chk))
        print("\n%s per row: %s" % (
            "d(gated)" if task == "x3" else "d(waited_async_write_count)",
            ", ".join(crow) or "none"))
        print("\nCOUNTERS %s: %s" % (
            "OK" if not cbad else "DIFFER",
            ("gated above 0 in every gate-on row and 0 elsewhere" if
             task == "x3" else "no waited write in an arm with "
             "zio_taskq_write_waited=0") if not cbad else
            "rows " + ", ".join(cbad)))
        if task == "x5":
            print("\nTasks per fio I/O, z_wr_iss_h and z_rd_int_h of fastq: "
                  "%s" % (", ".join(rrows) or "none"))
            print("\nROUTING %s: %s" % (
                "OK" if not rbad else "DIFFERS",
                "z_wr_iss_h: none without c1; with c1, at least 0.5 per "
                "I/O in sw1 and 0.1 in sw4 with the switch off, below 0.05 "
                "in the sw rows with it on; z_rd_int_h: at least 0.5 per "
                "I/O in the rr rows of m3w-c2, none elsewhere" if not rbad
                else "rows " + ", ".join(rbad)))
        nl = bad - len(cbad) - len(rbad)
        print("\nLAYOUT %s: %d arm(s) differ" % ("OK" if not nl else
                                                 "DIFFERS", nl))
    return bad


# --------------------------------------------------------------- probes --

def probes(path):
    s, a = [], []
    for line in (readf(path, "") or "").splitlines():
        m = re.search(r"single_s ([\d.]+) all_s ([\d.]+)", line)
        if m:
            s.append(float(m.group(1)))
            a.append(float(m.group(2)))
    return s, a


def probe_line(raw):
    s, a = probes(os.path.join(os.path.dirname(raw.rstrip("/")),
                               "probe.txt"))
    if not s:
        print("\nCPU probe: no probe.txt lines.")
        return
    print("\nCPU probe over %d runs: single_s %s-%s (first %s, last %s), "
          "all_s %s-%s (first %s, last %s)." % (
              len(s), f(min(s), 3), f(max(s), 3), f(s[0], 3), f(s[-1], 3),
              f(min(a), 3), f(max(a), 3), f(a[0], 3), f(a[-1], 3)))


# -------------------------------------------------------------------- X3 --

X3_RUN2 = ["m3w", "m3w-gate-on", "m3w-pct25w", "m3w-c1", "m3w-wiss2",
           "m3w-gate-on-c1", "m3w-up2", "m3w-c1-pct25w", "m3w-wint2",
           "m3w-c1-pct25", "m3w-pct25"]


def above(passes, a, b, wl):
    c = cmpv(series(passes, a, wl, "iops"), series(passes, b, wl, "iops"))
    return c, bool(c and c["beyond"] and c["up"])


def below(passes, a, b, wl):
    c = cmpv(series(passes, a, wl, "iops"), series(passes, b, wl, "iops"))
    return c, bool(c and c["beyond"] and not c["up"])


def tail_rises(passes, a, b):
    out = []
    for wl in WLS:
        for m, lab in (("p99", "p99"), ("p999", "p99.9")):
            c = cmpv(series(passes, a, wl, m), series(passes, b, wl, m))
            if c and c["beyond"] and c["up"]:
                out.append("%s %s %s" % (wl, lab, sgn(c["pct"])))
    return out


def pc(c):
    return "%s%s" % (sgn(c["pct"]), " beyond" if c["beyond"] else
                     " within") if c else "n/a"


def budget_verdict(passes, arm, base, ref, label):
    """X3's rule: rr1 and rr4 IOPS beyond the spread above BASE and not
    beyond the spread below REF; no p99 or p99.9 beyond the spread above
    BASE on any Run 2 row; slow MiB/s not beyond the spread below BASE."""
    if not present(passes, arm):
        print("- **%s**: not run." % label)
        return
    conds = []
    up = []
    for wl in ("rr1", "rr4"):
        c, ok = above(passes, arm, base, wl)
        up.append(ok)
        conds.append("%s IOPS vs %s %s" % (wl, name(base), pc(c)))
    refok = True
    if present(passes, ref):
        for wl in ("rr1", "rr4"):
            c, bl = below(passes, arm, ref, wl)
            refok = refok and not bl
            conds.append("%s IOPS vs %s %s" % (wl, ref, pc(c)))
    else:
        conds.append("%s not run" % ref)
        refok = None
    tails = tail_rises(passes, arm, base)
    sc = cmpv(slow_series(passes, arm), slow_series(passes, base))
    slowok = not (sc and sc["beyond"] and not sc["up"])
    conds.append("tail rises vs %s: %s" % (name(base),
                                            ", ".join(tails) or "none"))
    conds.append("slow MiB/s vs %s %s" % (name(base), pc(sc)))
    parts = [all(up), refok is not False, not tails, slowok]
    verdict = "yes" if all(parts) else ("no" if not all(up) else "partly")
    print("- **%s: %s** (read gain above %s: %s; not below %s: %s; no tail "
          "rise: %s; slow pools kept: %s). %s." % (
              label, verdict, name(base), "yes" if all(up) else "no", ref,
              {True: "yes", False: "no", None: "n/a"}[refok],
              "yes" if not tails else "no", "yes" if slowok else "no",
              "; ".join(conds)))


def x3(raw):
    run2 = load_group(raw, "run2")
    single = load_group(raw, "single")
    tput = load_group(raw, "tput")
    print("# TASK-1120-X3 summary (xp-report.py)\n")
    print("Passes: run2 %d, single %d, tput %d.  Beyond the spread = the "
          "[min, max] ranges over the passes do not overlap; %% compare "
          "means; baseline = m3w + m3w-up2 (m3w alone if m3w-up2 ran in "
          "fewer than 2 passes); an arm run in fewer than 2 passes is not "
          "compared." % (len(run2), len(single), len(tput)))
    base = BASE if npasses(run2, "m3w-up2", "rr1") >= 2 else "m3w"
    print("\n## Verdicts\n")
    # Issue or interrupt.
    for arm in ("m3w-wiss2", "m3w-wint2"):
        if not present(run2, arm):
            print("- **%s**: not run." % arm)
            continue
        ok = True
        txt = []
        for wl in ("rr1", "rr4"):
            c, up = above(run2, arm, base, wl)
            c2, bl = below(run2, arm, "m3w-pct25w", wl)
            ok = ok and up and not bl
            txt.append("%s IOPS vs baseline %s, vs m3w-pct25w %s; p99 vs "
                       "baseline %s; p99.9 vs baseline %s" % (
                           wl, pc(c), pc(c2),
                           pc(cmpv(series(run2, arm, wl, "p99"),
                                   series(run2, base, wl, "p99"))),
                           pc(cmpv(series(run2, arm, wl, "p999"),
                                   series(run2, base, wl, "p999")))))
        print("- **Issue or interrupt, %s: the cut alone gives pct25w's read "
              "gains: %s** (%s)." % (arm, "yes" if ok else "no",
                                     "; ".join(txt)))
    budget_verdict(run2, "m3w-gate-on", base, "m3w-pct25w",
                   "Budget against pct25w (m3w-gate-on)")
    budget_verdict(run2, "m3w-gate-on-c1", "m3w-c1", "m3w-c1-pct25w",
                   "With c1 (m3w-gate-on-c1, against m3w-c1 and "
                   "m3w-c1-pct25w)")
    # Lone pool.
    if present(single, "alone-gate-on"):
        cs = []
        for wl in ("sw1", "sw4"):
            for m, lab in (("iops", "IOPS"), ("p99", "p99"),
                           ("p999", "p99.9")):
                c = cmpv(series(single, "alone-gate-on", wl, m),
                         series(single, "alone", wl, m))
                cs.append((wl, lab, c))
        for m, lab in (("bw", "MiB/s"), ("p99", "p99")):
            c = cmpv(series(tput, "tput-gate-on", "tz", m),
                     series(tput, "tput", "tz", m))
            cs.append(("tz", lab, c))
        bey = ["%s %s %s" % (w, lab, sgn(c["pct"])) for w, lab, c in cs
               if c and c["beyond"]]
        print("- **Lone pool: %s** (%s)." % (
            "no difference beyond the spread" if not bey else
            "differences beyond the spread: " + ", ".join(bey),
            "; ".join("%s %s %s" % (w, lab, pc(c)) for w, lab, c in cs)))
    else:
        print("- **Lone pool**: Single not run.")
    # Two bulk pools.
    if present(tput, "tput-gate-on"):
        cs = []
        for m, lab in (("bw0", "fastq MiB/s"), ("bw1", "fastr MiB/s"),
                       ("bwsum", "total MiB/s")):
            c = cmpv(series(tput, "tput-gate-on", "tz2", m),
                     series(tput, "tput", "tz2", m))
            cs.append("%s %s" % (lab, pc(c)))
        print("- **Two bulk pools (tz2, tput-gate-on against tput)**: %s." %
              "; ".join(cs))
    else:
        print("- **Two bulk pools**: Tput not run.")
    gate_kstat(run2, single, tput)
    print("\n## Differences beyond the spread\n")
    diff_table(run2, "Calibration", [("m3w-up2", "m3w")])
    diff_table(run2, "Run 2 against the baseline",
               [(a, base) for a in X3_RUN2 if a not in BASE])
    diff_table(run2, "Run 2 pairs", [
        ("m3w-wiss2", "m3w-pct25w"), ("m3w-wint2", "m3w-pct25w"),
        ("m3w-gate-on", "m3w-pct25w"), ("m3w-gate-on", "m3w-wiss2"),
        ("m3w-gate-on-c1", "m3w-c1"), ("m3w-gate-on-c1", "m3w-c1-pct25w"),
        ("m3w-c1-pct25w", "m3w-c1"), ("m3w-pct25", "m3w-pct25w"),
        ("m3w-pct25", "m3w-wint2"), ("m3w-pct25", "m3w-wiss2"),
        ("m3w-pct25w", "m3w-pct25"), ("m3w-c1-pct25", "m3w-c1"),
        ("m3w-c1-pct25", "m3w-c1-pct25w"),
        ("m3w-c1-pct25w", "m3w-c1-pct25")])
    diff_table(single, "Single", [("alone-gate-on", "alone")],
               wls=["sw1", "sw4"], slow=False)
    print("\n### Tput (tput-gate-on against tput)\n")
    print("| row | metric | tput | tput-gate-on | change |")
    print("|---|---|---|---|---|")
    for wl, ms in (("tz", (("bw", "MiB/s"), ("p99", "p99 us"))),
                   ("tz2", (("bw0", "fastq MiB/s"), ("bw1", "fastr MiB/s"),
                            ("bwsum", "total MiB/s"),
                            ("p99_0", "fastq p99 us"),
                            ("p99_1", "fastr p99 us")))):
        for m, lab in ms:
            a = series(tput, "tput", wl, m)
            b = series(tput, "tput-gate-on", wl, m)
            print("| %s | %s | %s | %s | %s |" % (wl, lab, mr(a, 1),
                                                  mr(b, 1),
                                                  pc(cmpv(b, a))))
    means_table(run2, [base] + [a for a in X3_RUN2 if a not in BASE],
                "Run 2 means")
    controls(run2, X3_RUN2)
    for grp, ps in (("run2", run2), ("single", single), ("tput", tput)):
        if ps:
            layout_check("x3", ps[0]["out"])
    position_table(run2, X3_RUN2)
    noise(run2, "run2")
    noise(single, "single")
    noise(tput, "tput")
    probe_line(raw)
    per_pass_tables(run2, "run2", WLS, X3_RUN2)
    per_pass_tables(single, "single", ["sw1", "sw4"],
                    ["alone", "alone-gate-on"])
    per_pass_tables(tput, "tput", ["tz", "tz2"], ["tput", "tput-gate-on"],
                    tput=True)


GATE_EXPECT = {
    # (arm kind, row) -> (N range, limit range)
    "on-rr": ((2.5, 3.5), (1.5, 2.5)),
    "on-sw": ((2.5, 4.5), (1.5, 2.5)),
    "alone": ((0.95, 1.05), (5.5, 6.5)),
    "tz": ((0.95, 1.05), (5.5, 6.5)),
    "tz2": ((1.5, 2.5), (2.5, 3.5)),
}


def gate_kstat(run2, single, tput):
    print("\n### Gate kstat per arm and row (mean [min-max] over the "
          "passes)\n")
    print("Expected: d(gated) 0 in every arm with zio_taskq_write_budget=0; "
          "with 1, mean N in [2.5, 3.5] and mean limit in [1.5, 2.5] on rr1 "
          "and rr4, N in [2.5, 4.5] and limit in [1.5, 2.5] on sw1 and sw4, "
          "N in [0.95, 1.05] and limit in [5.5, 6.5] for alone-gate-on and "
          "tz, N in [1.5, 2.5] and limit in [2.5, 3.5] on tz2.\n")
    print("| group | arm | row | d(gated)/s | waited share | mean N | "
          "mean limit | check |")
    print("|---|---|---|---|---|---|---|---|")
    off = []
    offarms = []
    norows = []
    for grp, passes, arms, wls in (
            ("run2", run2, X3_RUN2, WLS),
            ("single", single, ["alone", "alone-gate-on"], ["sw1", "sw4"]),
            ("tput", tput, ["tput", "tput-gate-on"], ["tz", "tz2"])):
        for a in arms:
            if not present(passes, a):
                continue
            for wl in wls:
                rs = [ps["rows"].get((a, wl)) for ps in passes]
                rs = [r for r in rs if r is not None]
                if any(r.wb is None for r in rs):
                    norows.append("%s/%s" % (a, wl))
                rs = [r for r in rs if r.wb is not None]
                if not rs:
                    continue
                g = [r.wb["gated"] for r in rs]
                if "gate-on" not in a:
                    if a not in offarms:
                        offarms.append(a)
                    if any(x != 0 for x in g):
                        off.append("%s/%s (budget off, gated %s)" % (
                            a, wl, mr(g)))
                    continue
                gs = [r.wb["gated_s"] for r in rs
                      if r.wb["gated_s"] is not None]
                ws = [r.wb["waited"] for r in rs if r.wb["waited"] is not None]
                ns = [r.wb["N"] for r in rs if r.wb["N"] is not None]
                ls = [r.wb["limit"] for r in rs if r.wb["limit"] is not None]
                if a.startswith("alone"):
                    k = "alone"
                elif grp == "tput":
                    k = wl
                else:
                    k = "on-rr" if wl.startswith("rr") else "on-sw"
                (n0, n1), (l0, l1) = GATE_EXPECT[k]
                ok = all(x > 0 for x in g) and ns and ls and \
                    n0 <= statistics.mean(ns) <= n1 and \
                    l0 <= statistics.mean(ls) <= l1
                chk = "as expected" if ok else "DIFFERS"
                if not ok:
                    off.append("%s/%s" % (a, wl))
                print("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                    grp, a, wl, mr(gs, 0), mr(ws, 2), mr(ns, 2), mr(ls, 2),
                    chk))
    print("\nBudget-off arms (%s): d(gated) %s." % (
        ", ".join(offarms), "0 in every row" if not any(
            "budget off" in x for x in off) else "above 0 in " + ", ".join(
            x for x in off if "budget off" in x)))
    if norows:
        print("Rows without a wbudget snapshot: %s." % ", ".join(norows))
    print("\n- **Gate kstat: %s.**" % ("every row as expected" if not off
                                       else "differs in " + ", ".join(off)))


# -------------------------------------------------------------------- X4 --

X4_RUN2 = ["m3w", "m3w-n10", "m3w-w2", "m3w-lowslow", "m3w-c1", "m3w-up2",
           "m3w-n5", "m3w-w2x", "m3w-lowpri", "m3w-lowfast", "m3w-w2b",
           "m3w-w2x1"]
COUNTERS = [("txg_wait_explicit_count", "explicit"),
            ("txg_wait_restart_count", "restart"),
            ("tx_dirty_wait_count", "dirty"),
            ("waited_async_write_count", "waited writes")]
WW = {"m3w-w2x": 2, "m3w-w2x1": 1}


def cnt_series(passes, a, wl, key, which):
    v = []
    for ps in passes:
        r = ps["rows"].get((a, wl))
        if r is None or not r.cnt:
            continue
        if which == "fast":
            x = r.cnt.get("fastq", {}).get(key)
        else:
            xs = [r.cnt.get("slow%d" % k, {}).get(key) for k in (1, 2, 3)]
            x = sum(xs) if all(y is not None for y in xs) else None
        if x is not None:
            v.append(x)
    return v


def x4_b(passes):
    print("\n### (b) Intermediate nice\n")
    arms = [("m3w-c1", -19), ("m3w-n10", -10), ("m3w-n5", -5),
            ("m3w-lowpri", 0)]
    print("| metric | %s |" % " | ".join("%s (nice %d)" % a for a in arms))
    print("|---|%s" % ("---|" * len(arms)))
    for wl, m, lab in (("rr1", "iops", "rr1 IOPS"), ("rr1", "p99", "rr1 p99"),
                       ("rr4", "iops", "rr4 IOPS"), ("rr4", "p99", "rr4 p99"),
                       ("sw1", "p99", "sw1 p99"), ("sw1", "p999", "sw1 p99.9"),
                       ("sw4", "p99", "sw4 p99"),
                       ("sw4", "p999", "sw4 p99.9")):
        print("| %s | %s |" % (lab, " | ".join(
            mr(series(passes, a, wl, m)) for a, _n in arms)))
    print("| slow MiB/s | %s |" % " | ".join(mr(slow_series(passes, a), 1)
                                             for a, _n in arms))
    for fam_k, lab in (("cpus", "slow_iss cpus sw4"),
                       ("wait_cpus", "slow_iss wait_cpus sw4")):
        vals = []
        for a, _n in arms:
            v = [ps["rows"][(a, "sw4")].sched["slow_iss"][fam_k]
                 for ps in passes if (a, "sw4") in ps["rows"] and
                 ps["rows"][(a, "sw4")].ok and ps["rows"][(a, "sw4")].sched]
            vals.append(mr(v, 2))
        print("| %s | %s |" % (lab, " | ".join(vals)))
    print()
    if not (present(passes, "m3w-c1") and present(passes, "m3w-lowpri")):
        print("- **(b)**: m3w-c1 or m3w-lowpri not run.")
        return

    def mean(v):
        return statistics.mean(v) if v else None
    for arm in ("m3w-n10", "m3w-n5"):
        if not present(passes, arm):
            print("- **(b) %s**: not run." % arm)
            continue
        kept = {}
        conds = []
        readok = True
        for wl in ("rr1", "rr4"):
            a = mean(series(passes, arm, wl, "iops"))
            c = mean(series(passes, "m3w-c1", wl, "iops"))
            lo = mean(series(passes, "m3w-lowpri", wl, "iops"))
            k = (a - c) / (lo - c) if None not in (a, c, lo) and lo != c \
                else None
            kept[wl] = k
            cc, up = above(passes, arm, "m3w-c1", wl)
            readok = readok and up and k is not None and k >= 2.0 / 3
            conds.append("%s IOPS vs m3w-c1 %s, share of lowpri's gain kept "
                         "%s" % (wl, pc(cc), f(k, 2)))
        sa = mean(slow_series(passes, arm))
        sc = mean(slow_series(passes, "m3w-c1"))
        sl = mean(slow_series(passes, "m3w-lowpri"))
        paid = (sc - sa) / (sc - sl) if None not in (sa, sc, sl) and \
            sc != sl else None
        s_up = cmpv(slow_series(passes, arm), slow_series(passes,
                                                          "m3w-lowpri"))
        slowok = bool(s_up and s_up["beyond"] and s_up["up"]) or \
            (paid is not None and paid <= 0.5)
        conds.append("slow MiB/s vs m3w-lowpri %s, share of lowpri's cost "
                     "paid %s" % (pc(s_up), f(paid, 2)))
        tails = []
        for wl in ("sw1", "sw4"):
            for m, lab in (("p99", "p99"), ("p999", "p99.9")):
                c = cmpv(series(passes, arm, wl, m),
                         series(passes, "m3w-c1", wl, m))
                if c and c["beyond"] and c["up"]:
                    tails.append("%s %s %s" % (wl, lab, sgn(c["pct"])))
        conds.append("sw tail rises vs m3w-c1: %s" % (", ".join(tails) or
                                                     "none"))
        parts = [readok, slowok, not tails]
        v = "yes" if all(parts) else ("no" if not any(parts) else "partly")
        print("- **(b) %s keeps most of the read gain with less slow-pool "
              "cost: %s** (read gain kept: %s; slow-pool cost: %s; no sw "
              "tail rise: %s). %s." % (arm, v, "yes" if readok else "no",
                                       "yes" if slowok else "no",
                                       "yes" if not tails else "no",
                                       "; ".join(conds)))


def x4_counters(passes):
    print("\n### Counters per arm and row (growth over the row, mean "
          "[min-max]; fast = fastq, slow = slow1 + slow2 + slow3)\n")
    print("| arm | row | %s |" % " | ".join(
        "%s fast | %s slow" % (lab, lab) for _k, lab in COUNTERS))
    print("|---|---|%s" % ("---|" * (2 * len(COUNTERS))))
    grown = []
    for a in X4_RUN2:
        if not present(passes, a):
            continue
        for wl in WLS:
            cells = []
            for k, _lab in COUNTERS:
                for which in ("fast", "slow"):
                    v = cnt_series(passes, a, wl, k, which)
                    cells.append(mr(v))
                    if k == "waited_async_write_count" and \
                            WW.get(a, 0) == 0 and any(x > 0 for x in v):
                        grown.append("%s/%s %s" % (a, wl, which))
            print("| %s | %s | %s |" % (a, wl, " | ".join(cells)))
    print("\n- **waited_async_write_count growth in an arm with "
          "zio_taskq_write_waited=0: %s.**" % (", ".join(grown) if grown
                                               else "none"))


def x4_c(passes):
    print("\n### (c) Txg-waiter exemption\n")
    for arm in ("m3w-w2x", "m3w-w2x1"):
        if not present(passes, arm):
            print("- **%s**: not run." % arm)
            continue
        out = []
        for wl, m, lab in (("sw1", "p999", "sw1 p99.9"),
                           ("sw4", "p99", "sw4 p99"),
                           ("sw4", "p999", "sw4 p99.9"),
                           ("rr1", "iops", "rr1 IOPS"),
                           ("rr1", "p99", "rr1 p99"),
                           ("rr4", "iops", "rr4 IOPS"),
                           ("rr4", "p99", "rr4 p99")):
            out.append("%s %s" % (lab, pc(cmpv(series(passes, arm, wl, m),
                                                series(passes, "m3w-w2", wl,
                                                       m)))))
        out.append("slow MiB/s %s" % pc(cmpv(slow_series(passes, arm),
                                             slow_series(passes, "m3w-w2"))))
        print("- **%s against m3w-w2**: %s." % (arm, "; ".join(out)))
    for arm in ("m3w-w2", "m3w-w2x", "m3w-w2x1"):
        if not present(passes, arm):
            continue
        rows = []
        for wl in WLS:
            e = cnt_series(passes, arm, wl, "txg_wait_explicit_count", "fast")
            r = cnt_series(passes, arm, wl, "txg_wait_restart_count", "fast")
            wf = cnt_series(passes, arm, wl, "waited_async_write_count",
                            "fast")
            ws = cnt_series(passes, arm, wl, "waited_async_write_count",
                            "slow")
            rows.append("%s: fast waiters explicit %s restart %s, waited "
                        "writes fast %s slow %s" % (wl, mr(e), mr(r), mr(wf),
                                                    mr(ws)))
        print("- %s: %s." % (arm, "; ".join(rows)))


def x4_d(passes):
    print("\n### (d) lowpri's slow-pool cost\n")
    arms = ["m3w-c1", "m3w-n10", "m3w-n5", "m3w-lowpri", "m3w-lowslow",
            "m3w-lowfast"]
    for arm in ("m3w-lowslow", "m3w-lowfast"):
        if not present(passes, arm):
            print("- %s: not run." % arm)
            continue
        for ref in ("m3w-lowpri", "m3w-c1"):
            print("- %s against %s: slow MiB/s %s; rr1 IOPS %s; rr4 IOPS %s."
                  % (arm, ref, pc(cmpv(slow_series(passes, arm),
                                       slow_series(passes, ref))),
                     pc(cmpv(series(passes, arm, "rr1", "iops"),
                             series(passes, ref, "rr1", "iops"))),
                     pc(cmpv(series(passes, arm, "rr4", "iops"),
                             series(passes, ref, "rr4", "iops")))))
    print("\nSlow device MiB/s (sectors written on nullb2-nullb19, parity "
          "included), per window:\n")
    print("| arm | warm-up (fast pool idle) | rr1 | rr4 | sw1 | sw4 | "
          "whole phase |")
    print("|---|---|---|---|---|---|---|")
    for a in arms:
        if not present(passes, a):
            continue
        warm = [ps["phase"][a]["warm_dev"] for ps in passes
                if a in ps["phase"] and ps["phase"][a]["warm_dev"] is not None]
        whole = [ps["phase"][a]["phase_dev"] for ps in passes
                 if a in ps["phase"] and
                 ps["phase"][a]["phase_dev"] is not None]
        print("| %s | %s | %s | %s |" % (
            a, mr(warm, 1), " | ".join(mr(series(passes, a, wl, "devmibs"),
                                          1) for wl in WLS), mr(whole, 1)))
    print("\nScheduler, sw4 and rr1 (cpus / wait_cpus, means):\n")
    print("| arm | row | slow_iss | slow_fio | txg_sync | fio wait_us |")
    print("|---|---|---|---|---|---|")
    for a in arms:
        if not present(passes, a):
            continue
        for wl in ("rr1", "sw4"):
            rs = [ps["rows"][(a, wl)] for ps in passes
                  if (a, wl) in ps["rows"] and ps["rows"][(a, wl)].ok and
                  ps["rows"][(a, wl)].sched]
            if not rs:
                continue

            def m(fam, k):
                return statistics.mean(r.sched[fam][k] for r in rs)
            print("| %s | %s | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f | "
                  "%.1f |" % (a, wl, m("slow_iss", "cpus"),
                              m("slow_iss", "wait_cpus"),
                              m("slow_fio", "cpus"), m("slow_fio",
                                                       "wait_cpus"),
                              m("txg_sync", "cpus"), m("txg_sync",
                                                       "wait_cpus"),
                              m("fio", "wait_us")))


def x4_extras(passes):
    print("\n### Per-row extras (mean [min-max])\n")
    print("Tx assigns over about 1 ms: the dmu_tx_assign buckets of 2^21 ns "
          "and up (each bucket counts delays up to its name, above half "
          "of it).  Tail mode: rr1 and rr4 rows with p99 below 1000 us are "
          "low, others high.\n")
    print("| arm | row | slow dev MiB/s | tx assign > 1 ms fast | slow | "
          "tail mode (low/high) | seconds >= 2 ms | largest us | slow txgs "
          "synced | mean stime ms |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for a in X4_RUN2:
        if not present(passes, a):
            continue
        for wl in WLS:
            rs = [ps["rows"][(a, wl)] for ps in passes
                  if (a, wl) in ps["rows"] and ps["rows"][(a, wl)].ok]
            if not rs:
                continue
            tf = [r.txa.get("fastq") for r in rs if "fastq" in r.txa]
            ts = [sum(r.txa.get("slow%d" % k, 0) for k in (1, 2, 3))
                  for r in rs if r.txa]
            modes = [r.mode for r in rs if r.mode]
            sec = [r.tail["sec2ms"] for r in rs if r.tail]
            mx = [r.tail["max_us"] for r in rs if r.tail]
            tn = [r.txg["n"] for r in rs if r.txg]
            tst = [r.txg["stime_ms"] for r in rs if r.txg and
                   r.txg["stime_ms"] is not None]
            print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                a, wl, mr([r.devmibs for r in rs if r.devmibs is not None],
                          1), mr(tf), mr(ts),
                "%d/%d" % (modes.count("low"), modes.count("high"))
                if modes else "-", mr(sec), mr(mx), mr(tn) if tn else "n/a",
                mr(tst, 1) if tst else "n/a"))
    units = set(r.tail["unit"] for ps in passes for r in ps["rows"].values()
                if r.tail)
    print("\nfio max-value log unit, from the json max: %s." % (
        ", ".join(sorted(units)) or "no log"))


def x4(raw):
    run2 = load_group(raw, "run2")
    print("# TASK-1120-X4 summary (xp-report.py)\n")
    print("Passes: run2 %d.  Beyond the spread = the [min, max] ranges over "
          "the passes do not overlap; %% compare means; baseline = m3w + "
          "m3w-up2." % len(run2))
    print("\n## Verdicts\n")
    x4_b(run2)
    x4_c(run2)
    x4_d(run2)
    print("\n### Defaults\n")
    diff_table(run2, "Calibration (m3w-up2 against m3w)",
               [("m3w-up2", "m3w")])
    x4_counters(run2)
    print("\n## Differences beyond the spread\n")
    diff_table(run2, "Against the baseline",
               [(a, BASE) for a in X4_RUN2 if a not in BASE])
    diff_table(run2, "Pairs", [
        ("m3w-n10", "m3w-c1"), ("m3w-n5", "m3w-c1"), ("m3w-lowpri", "m3w-c1"),
        ("m3w-n10", "m3w-lowpri"), ("m3w-n5", "m3w-lowpri"),
        ("m3w-lowpri", "m3w-n10"), ("m3w-lowpri", "m3w-n5"),
        ("m3w-lowslow", "m3w-lowpri"), ("m3w-lowslow", "m3w-c1"),
        ("m3w-lowfast", "m3w-c1"), ("m3w-lowfast", "m3w-lowpri"),
        ("m3w-lowpri", "m3w-lowslow"), ("m3w-w2", "m3w-lowpri"),
        ("m3w-w2x", "m3w-w2"), ("m3w-w2x1", "m3w-w2"),
        ("m3w-w2b", "m3w-w2")])
    means_table(run2, [BASE] + [a for a in X4_RUN2 if a not in BASE],
                "Run 2 means")
    x4_extras(run2)
    controls(run2, X4_RUN2)
    if run2:
        layout_check("x4", run2[0]["out"])
    position_table(run2, X4_RUN2)
    noise(run2, "run2")
    probe_line(raw)
    per_pass_tables(run2, "run2", WLS, X4_RUN2)


# -------------------------------------------------------------------- X5 --

SWL = ["sw1", "sw4"]


def yn(x):
    return "yes" if x else "no"


def x5_base(run2):
    """Rules 3 and 4's "m3w": the baseline, m3w and m3w-up2 together as in
    X3 and X4, or m3w alone if m3w-up2 ran in fewer than 2 passes."""
    return BASE if npasses(run2, "m3w-up2", "rr1") >= 2 else "m3w"


def short(passes, arms, wls):
    """The rows of ARMS (an arm or the baseline each) with fewer than 2
    passes."""
    out = []
    for a in arms:
        for wl in wls:
            if len(series(passes, a, wl, "iops")) < 2:
                out.append("%s/%s" % (name(a), wl))
    return out


def rule1(run2, single):
    """Rule 1: c1 on top of v10a.  Returns (verdict, figures)."""
    a, b, sa, sb = "m3w-zi-c1", "m3w-zi", "alone-zi-c1", "alone-zi"
    lines = []
    miss = short(run2, (a, b), WLS) + short(single, (sa, sb), SWL)
    sc = cmpv(slow_series(run2, a), slow_series(run2, b))
    if sc is None:
        miss.append("slow MiB/s")
    if miss:
        lines.append("fewer than 2 passes: %s" % ", ".join(miss))
        return "not decided", lines
    ups = []
    for wl in SWL:
        c, up = above(run2, a, b, wl)
        ups.append(up)
        lines.append("%s IOPS %s" % (wl, pc(c)))
    tails = tail_rises(run2, a, b)
    slowok = not (sc["beyond"] and not sc["up"])
    lines.append("p99 or p99.9 beyond the spread above on a Run 2 row: %s"
                 % (", ".join(tails) or "none"))
    lines.append("slow MiB/s %s" % pc(sc))
    aok = True
    for wl in SWL:
        c, bl = below(single, sa, sb, wl)
        aok = aok and not bl
        lines.append("Single %s IOPS %s against %s %s" % (wl, sa, sb, pc(c)))
    parts = [any(ups), not tails, slowok, aok]
    lines.append("sw1 or sw4 IOPS above: %s; no tail rise: %s; slow pools "
                 "not below: %s; Single not below: %s" %
                 tuple(yn(x) for x in parts))
    return ("yes" if all(parts) else "no"), lines


def rule3(run2, base):
    """Rule 3: C2 (m3w-c2) against the baseline."""
    a = "m3w-c2"
    lines = []
    miss = short(run2, (a, base), WLS)
    sc = cmpv(slow_series(run2, a), slow_series(run2, base))
    if sc is None:
        miss.append("slow MiB/s")
    if miss:
        lines.append("fewer than 2 passes: %s" % ", ".join(miss))
        return "not decided", lines
    c, up = above(run2, a, base, "rr4")
    lines.append("rr4 IOPS %s" % pc(c))
    c1, _ = above(run2, a, base, "rr1")
    lines.append("rr1 IOPS %s (not in the rule)" % pc(c1))
    tails = tail_rises(run2, a, base)
    slowok = not (sc["beyond"] and not sc["up"])
    lines.append("p99 or p99.9 beyond the spread above on a row: %s" %
                 (", ".join(tails) or "none"))
    lines.append("slow MiB/s %s" % pc(sc))
    ok = up and not tails and slowok
    return ("candidate for a default code change" if ok else "closed"), lines


def rule4(run2, single, base):
    """Rule 4: v10a itself, m3w-zi against the baseline and alone-zi
    against alone, sw1 and sw4.  Returns (losses, gains, cells)."""
    losses, gains, cells = [], [], []
    for passes, a, b, grp in ((run2, "m3w-zi", base, "Run 2"),
                              (single, "alone-zi", "alone", "Single")):
        for wl in SWL:
            cs = []
            for m, lab in LAT:
                c = cmpv(series(passes, a, wl, m), series(passes, b, wl, m))
                cs.append("%s %s" % (lab, pc(c)))
                if not c or not c["beyond"]:
                    continue
                good = c["up"] if m == "iops" else not c["up"]
                (gains if good else losses).append(
                    "%s %s %s %s" % (grp, wl, lab, sgn(c["pct"])))
            cells.append("%s %s, %s against %s: %s" % (grp, wl, a, name(b),
                                                       ", ".join(cs)))
    return losses, gains, cells


def perm_p(logs):
    """Exact two-sided sign-flip permutation test of paired log ratios
    (V7P to V10P's)."""
    n = len(logs)
    if n < 2:
        return None
    obs = abs(sum(logs))
    hit = 0
    for signs in itertools.product((1, -1), repeat=n):
        if abs(sum(s * x for s, x in zip(signs, logs))) >= obs - 1e-12:
            hit += 1
    return hit / 2 ** n


def load_partb(raw, group="partb"):
    """X5's Part B: [{"name", "arms": {arm: Row}}] per pass."""
    d = os.path.join(raw, group)
    out = []
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in sorted(names, key=lambda s: int(s[1:]) if s[1:].isdigit()
                    else 0):
        pd = os.path.join(d, n)
        if not re.match(r"^p\d+$", n) or not os.path.isdir(pd):
            continue
        order = (readf(os.path.join(pd, "order.txt"), "") or "").split()
        ps = {"name": n, "dir": pd, "order": order, "arms": {}}
        for a in PB_ARMS:
            ad = os.path.join(pd, a)
            if not os.path.isdir(ad):
                continue
            r = Row()
            r.arm, r.dir = a, ad
            r.rc = first_num(os.path.join(ad, "rc"))
            r.jobs = fio_jobs(os.path.join(ad, "n8i.json"))
            j = r.jobs[0] if r.jobs else {}
            r.iops, r.p50, r.p99, r.p999, r.ios = (
                j.get(k) for k in ("iops", "p50", "p99", "p999", "ios"))
            r.ok = r.rc == 0 and bool(r.jobs)
            r.split = stat_split(os.path.join(ad, "pre.stat"),
                                 os.path.join(ad, "post.stat"))
            r.steal = r.split["steal"] if r.split else None
            r.busy = (100.0 - r.split["idle"] - r.split["iowait"] -
                      r.split["steal"]) if r.split else None
            r.load1 = first_num(os.path.join(ad, "post.loadavg"))
            r.tq = tasks_by_family(os.path.join(ad, "pre.wrtasks"),
                                   os.path.join(ad, "post.wrtasks")) or {}
            z0 = kstat_named(os.path.join(ad, "pre.zil"))
            z1 = kstat_named(os.path.join(ad, "post.zil"))
            r.zil = {k: z1[k] - z0.get(k, 0) for k in z1} if z1 else {}
            r.pos = order.index(a) + 1 if a in order else None
            ps["arms"][a] = r
        out.append(ps)
    return out


def pb_series(pb, a, metric):
    v = []
    for ps in pb:
        r = ps["arms"].get(a)
        if r is not None and r.ok:
            x = getattr(r, metric, None)
            if x is not None:
                v.append(x)
    return v


def pb_per_op(r, fam=None, zil=None):
    if not r.ok or not r.ios:
        return None
    if fam:
        return r.tq.get(fam, 0) / float(r.ios)
    return r.zil.get(zil, 0) / float(r.ios) if r.zil else None


def pb_pair(pb, a, b):
    """Over the passes where both arms ran: A IOPS / B IOPS in the same
    pass, geometric mean, passes won and the sign-flip p."""
    rat = []
    for ps in pb:
        ra, rb = ps["arms"].get(a), ps["arms"].get(b)
        if ra is not None and rb is not None and ra.ok and rb.ok and \
                ra.iops and rb.iops:
            rat.append(ra.iops / rb.iops)
    if not rat:
        return None
    logs = [math.log(x) for x in rat]
    return {"n": len(rat), "geo": math.exp(sum(logs) / len(logs)),
            "won": sum(1 for x in rat if x > 1), "p": perm_p(logs),
            "min": min(rat), "max": max(rat)}


def rule2(pb, r1):
    """Rule 2: v10a + c1 against v10a on n8i."""
    if not pb:
        return "not run", ["Part B did not run (rule 1: %s)" % r1]
    c = pb_pair(pb, "v10a-c1", "v10a")
    if c is None:
        return "not decided", ["no pass with both v10a-c1 and v10a"]
    lines = ["v10a-c1 / v10a IOPS in the same pass: geometric mean %.3f "
             "[%.3f-%.3f], won %d of %d, p perm %s" % (
                 c["geo"], c["min"], c["max"], c["won"], c["n"],
                 f(c["p"], 3))]
    if c["n"] < 6:
        lines.append("%d passes: with fewer than 6 an exact sign-flip test "
                     "cannot give p < 0.05" % c["n"])
        return "not decided", lines
    below = c["geo"] < 1 and c["p"] is not None and c["p"] < 0.05
    lines.append("significantly below (p perm < 0.05 and below): %s" %
                 yn(below))
    return ("c1 closed on that loss" if below else
            "c1 PR as a default change"), lines


def pb_layout_check(pdir, quiet=False):
    """The layout and parameters each arm of one Part B pass imported c1n
    with, and that its row wrote WR_INDIRECT records (at least one
    indirect itx per write); returns the number of arms that differ."""
    bad = 0
    rows = []
    for a in PB_ARMS:
        ad = os.path.join(pdir, a)
        if not os.path.isdir(ad):
            continue
        lay, par = layout_of(ad), params_of(ad)
        diffs = layout_diffs(PB_LAYOUT[a], lay, par)
        js = fio_jobs(os.path.join(ad, "n8i.json"))
        z0 = kstat_named(os.path.join(ad, "pre.zil"))
        z1 = kstat_named(os.path.join(ad, "post.zil"))
        ios = js[0].get("ios") if js else None
        ind = None
        if ios and "zil_itx_indirect_count" in z1:
            ind = (z1["zil_itx_indirect_count"] -
                   z0.get("zil_itx_indirect_count", 0)) / float(ios)
        if ind is None or ind < 1:
            diffs.append("indirect itxs per write %s, expected 1 or more" %
                         f(ind, 2))
        rows.append((a, "z_wr_iss: %s; z_wr_iss_h: %s; indirect itxs per "
                     "write %s; params %s" % (
            fmt_fam(lay.get("z_wr_iss")), fmt_fam(lay.get("z_wr_iss_h")),
            f(ind, 2),
            ", ".join("%s=%s" % (k.replace("zio_taskq_", ""), par.get(k))
                      for k in ("zio_taskq_write", "zio_taskq_read",
                                "zil_issue_inline", "zio_taskq_write_budget",
                                "zio_taskq_bulk_pri_pct",
                                "zio_taskq_write_waited",
                                "zio_taskq_bulk_lowpri"))),
            "; ".join(diffs) or "as expected"))
        bad += 1 if diffs else 0
    if not rows:
        bad += 1
    if not quiet:
        print("\n### Part B layout check, %s\n" % "/".join(
            os.path.normpath(pdir).split(os.sep)[-2:]))
        print("| arm | seen after the import | check |")
        print("|---|---|---|")
        for a, desc, chk in rows:
            print("| %s | %s | %s |" % (a, desc, chk))
        print("\nPARTB LAYOUT %s: %d arm(s) differ%s" % (
            "OK" if not bad else "DIFFERS", bad,
            "" if rows else " (no arm directory)"))
    return bad


def partb_tables(pb):
    print("\n## Part B (n8i on c1n, TASK-1116-C1B's row)\n")
    if not pb:
        print("Not run.")
        return
    print("| arm | n | IOPS | p50 us | p99 us | p99.9 us | busy CPU % | "
          "steal % | load1 (end) | z_wr_iss tasks/write | z_wr_iss_h "
          "tasks/write | indirect itxs/write | commits/write |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for a in PB_ARMS:
        rs = [ps["arms"][a] for ps in pb if a in ps["arms"] and
              ps["arms"][a].ok]
        if not rs:
            continue

        def ops(fn):
            return [x for x in (fn(r) for r in rs) if x is not None]
        print("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s "
              "| %s |" % (
                  a, len(rs), mr(pb_series(pb, a, "iops")),
                  mr(pb_series(pb, a, "p50"), 1),
                  mr(pb_series(pb, a, "p99")), mr(pb_series(pb, a, "p999")),
                  mr(pb_series(pb, a, "busy"), 1),
                  mr(pb_series(pb, a, "steal"), 2),
                  mr(pb_series(pb, a, "load1"), 2),
                  mr(ops(lambda r: pb_per_op(r, fam="z_wr_iss")), 2),
                  mr(ops(lambda r: pb_per_op(r, fam="z_wr_iss_h")), 2),
                  mr(ops(lambda r: pb_per_op(r, zil="zil_itx_indirect_count")),
                     2),
                  mr(ops(lambda r: pb_per_op(r, zil="zil_commit_count")), 2)))
    print("\n| pair (IOPS, same pass) | n | geometric mean [min-max] | "
          "passes won | p perm |")
    print("|---|---|---|---|---|")
    for a, b in (("v10a-c1", "v10a"), ("v10a", "master"),
                 ("v10a-c1", "master")):
        c = pb_pair(pb, a, b)
        if c is None:
            print("| %s / %s | 0 | - | - | - |" % (a, b))
            continue
        print("| %s / %s | %d | %.3f [%.3f-%.3f] | %d | %s |" % (
            a, b, c["n"], c["geo"], c["min"], c["max"], c["won"],
            f(c["p"], 3)))
    print("\n| arm | pass | pos | IOPS | p99 us | p99.9 us | steal % | "
          "load1 (end) |")
    print("|---|---|---|---|---|---|---|---|")
    for a in PB_ARMS:
        for ps in pb:
            r = ps["arms"].get(a)
            if r is None:
                continue
            print("| %s%s | %s | %s | %s | %s | %s | %s | %s |" % (
                a, "" if r.ok else " (rc %s)" % f(r.rc), ps["name"],
                r.pos, f(r.iops), f(r.p99), f(r.p999), f(r.steal, 2),
                f(r.load1, 2)))
    pb_layout_check(pb[0]["dir"])


def x5_routing(passes, arms, wls):
    print("\n| arm | row | z_wr_iss | z_wr_iss_h | z_rd_int | z_rd_int_h |")
    print("|---|---|---|---|---|---|")
    for a in arms:
        for wl in wls:
            rs = [ps["rows"][(a, wl)] for ps in passes
                  if (a, wl) in ps["rows"] and ps["rows"][(a, wl)].ok]
            if not rs:
                continue
            print("| %s | %s | %s |" % (a, wl, " | ".join(
                mr([x for x in (per_op(r, fam) for r in rs)
                    if x is not None], 2)
                for fam in ("z_wr_iss", "z_wr_iss_h", "z_rd_int",
                            "z_rd_int_h"))))


def x5(raw):
    run2 = load_group(raw, "run2")
    single = load_group(raw, "single")
    pb = load_partb(raw)
    base = x5_base(run2)
    print("# TASK-1122-X5 summary (xp-report.py)\n")
    print("Passes: run2 %d, single %d, partb %d.  Beyond the spread = the "
          "[min, max] ranges over the passes do not overlap; %% compare "
          "means; \"m3w\" in rules 3 and 4 = the baseline, %s; an arm run "
          "in fewer than 2 passes is not compared.  Part B: ratios of "
          "IOPS in the same pass, exact sign-flip permutation p." % (
              len(run2), len(single), len(pb),
              "m3w + m3w-up2" if base == BASE else "m3w alone (m3w-up2 ran "
              "in fewer than 2 passes)"))
    print("\n## Verdicts\n")
    v1, l1 = rule1(run2, single)
    print("- **Rule 1, c1 on top of v10a: %s** -> %s (%s)." % (
        v1, {"yes": "c1 stays a default-change candidate, go to Part B",
             "no": "c1 is closed (\"v10a covers it\")",
             "not decided": "no verdict"}[v1], "; ".join(l1)))
    v2, l2 = rule2(pb, v1)
    print("- **Rule 2, Part B: %s** (%s)." % (v2, "; ".join(l2)))
    v3, l3 = rule3(run2, base)
    print("- **Rule 3, C2: %s** (%s)." % (v3, "; ".join(l3)))
    losses, gains, cells = rule4(run2, single, base)
    print("- **Rule 4, v10a itself (for V-01): %s; gains beyond the spread: "
          "%s** (%s)." % (
              "loss beyond the spread: " + ", ".join(losses) +
              " (a finding for the v10a PR)" if losses else
              "no loss beyond the spread", ", ".join(gains) or "none",
              "; ".join(cells)))
    print("\n### Defaults\n")
    diff_table(run2, "Calibration (m3w-up2 against m3w)",
               [("m3w-up2", "m3w")])
    ww = [(ps["name"], a, wl) for grp in (run2, single) for ps in grp
          for (a, wl), r in ps["rows"].items()
          if r.cnt and any(c.get("waited_async_write_count", 0)
                           for c in r.cnt.values())]
    print("\nwaited_async_write_count: %s." % (
        "no growth in any row" if not ww else "grew in " + ", ".join(
            "%s %s/%s" % x for x in ww)))
    print("\n### Routing (tasks of fastq's taskqs per fio I/O, mean "
          "[min-max])")
    x5_routing(run2, X5_RUN2, WLS)
    x5_routing(single, X5_SINGLE, SWL)
    print("\n## Differences beyond the spread\n")
    diff_table(run2, "Run 2 against the baseline",
               [(a, base) for a in X5_RUN2 if a not in BASE])
    diff_table(run2, "Run 2 pairs", [
        ("m3w-zi-c1", "m3w-zi"), ("m3w-zi-c1", "m3w-c1"),
        ("m3w-zi", "m3w-c1"), ("m3w-c1", "m3w"), ("m3w-zi", "m3w"),
        ("m3w-c2", "m3w")])
    diff_table(single, "Single", [
        ("alone-c1", "alone"), ("alone-zi", "alone"),
        ("alone-zi-c1", "alone-zi"), ("alone-zi-c1", "alone-c1"),
        ("alone-zi-c1", "alone")], wls=SWL, slow=False)
    partb_tables(pb)
    means_table(run2, [base] + [a for a in X5_RUN2 if a not in BASE],
                "Run 2 means")
    controls(run2, X5_RUN2)
    if run2:
        layout_check("x5", run2[0]["out"])
    if single:
        layout_check("x5", single[0]["out"])
    position_table(run2, X5_RUN2)
    position_table(single, X5_SINGLE)
    noise(run2, "run2")
    noise(single, "single")
    probe_line(raw)
    per_pass_tables(run2, "run2", WLS, X5_RUN2)
    per_pass_tables(single, "single", SWL, X5_SINGLE)


def rule1_cmd(raw):
    run2 = load_group(raw, "run2")
    single = load_group(raw, "single")
    v, lines = rule1(run2, single)
    print("Rule 1 (c1 on top of v10a), passes: run2 %d, single %d." % (
        len(run2), len(single)))
    for x in lines:
        print("- " + x)
    print("RULE 1: %s" % v)


# ----------------------------------------------------------------- retro --

RETRO_ARMS = ["m3w", "m3w-up2", "m3w-w2", "m3w-w2b", "m3w-lowpri"]


def find_passes(root):
    out = []
    for dp, dns, fns in os.walk(root):
        if dp.endswith(".failed"):
            dns[:] = []
            continue
        if "runs" in dns and ("phases.txt" in fns or any(
                x.startswith("qv.") for x in os.listdir(
                    os.path.join(dp, "runs")))):
            out.append(dp)
            dns[:] = []
    return sorted(out)


def retro(specs):
    runs = []
    for s in specs:
        if "=" not in s:
            sys.exit("retro: LABEL=ROOT expected, got %s" % s)
        lab, root = s.split("=", 1)
        passes = []
        for out in find_passes(root):
            ps = load_pass(out)
            if any(a in ps["phases"] for a in RETRO_ARMS):
                ps["name"] = os.path.relpath(out, root)
                passes.append(ps)
        runs.append((lab, root, passes))
    print("# TASK-1120-X4 step 8: the archived runs side by side\n")
    print("| run | pass | arm | row | IOPS | p99 | p99.9 | tail mode | pos "
          "| arm before | min since pass start | user % | nice % | sys % | "
          "irq+soft % | iowait % | idle % | steal % | load1 | ARC size MiB "
          "| anon MiB | slow MiB/s | fio wait_us | slow_iss cpus | slow txgs "
          "| phase min |")
    print("|%s" % ("---|" * 26))
    for lab, root, passes in runs:
        if not passes:
            print("| %s | not archived (no pass under %s) |%s" % (
                lab, root, " |" * 24))
            continue
        for ps in passes:
            for a in RETRO_ARMS:
                if a not in ps["phase"]:
                    continue
                for wl in WLS:
                    r = ps["rows"].get((a, wl))
                    if r is None:
                        continue
                    sp = r.split or {}
                    print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s "
                          "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s "
                          "| %s | %s | %s | %s | %s | %s |" % (
                              lab, ps["name"], a, wl, f(r.iops), f(r.p99),
                              f(r.p999), r.mode or "-", r.pos,
                              r.before or "-", f(r.mins, 1),
                              f(sp.get("user"), 1), f(sp.get("nice"), 1),
                              f(sp.get("system"), 1), f(sp.get("irq"), 1),
                              f(sp.get("iowait"), 1), f(sp.get("idle"), 1),
                              f(sp.get("steal"), 2), f(r.load1, 2),
                              f(r.arc_size / 1048576.0 if r.arc_size else
                                None),
                              f(r.arc_anon / 1048576.0 if r.arc_anon else
                                None),
                              f(r.slow, 1),
                              f(r.sched["fio"]["wait_us"], 1) if r.sched
                              else "not archived",
                              f(r.sched["slow_iss"]["cpus"], 2) if r.sched
                              else "not archived",
                              r.txg["n"] if r.txg else "n/a",
                              f(ps["phase"][a]["len_min"], 1)))
    print("\n## One column per run\n")
    labs = [lab for lab, _r, _p in runs]
    print("| | %s |" % " | ".join(labs))
    print("|---|%s" % ("---|" * len(labs)))

    def col(fn):
        return " | ".join(fn(p) for _l, _r, p in runs)

    def lowshare(p, arms):
        ms = [r.mode for ps in p for a in arms
              for r in [ps["rows"].get((a, "rr1"))] if r and r.mode]
        return "%d/%d" % (ms.count("low"), len(ms)) if ms else "-"
    for wl in WLS:
        print("| m3w + m3w-up2 %s IOPS | %s |" % (
            wl, col(lambda p: mr(series(p, BASE, wl, "iops")))))
        print("| m3w-w2 (+ w2b) %s IOPS | %s |" % (
            wl, col(lambda p: mr(series(p, ("m3w-w2", "m3w-w2b"), wl,
                                        "iops")))))
    print("| m3w-w2 (+ w2b) rr1 p99 | %s |" % col(
        lambda p: mr(series(p, ("m3w-w2", "m3w-w2b"), "rr1", "p99"))))
    print("| low-mode rr1 rows, baseline | %s |" % col(
        lambda p: lowshare(p, BASE)))
    print("| low-mode rr1 rows, m3w-w2 (+ w2b) | %s |" % col(
        lambda p: lowshare(p, ("m3w-w2", "m3w-w2b"))))

    def dates(p):
        ts = []
        for ps in p:
            for r in ps["rows"].values():
                try:
                    ts.append(os.path.getmtime(os.path.join(r.dir,
                                                            "fio.json")))
                except OSError:
                    pass
        if not ts:
            return "-"
        return "%s to %s UTC" % (
            time.strftime("%d/%m %H:%M", time.gmtime(min(ts))),
            time.strftime("%H:%M", time.gmtime(max(ts))))
    print("| date and hours (fio.json mtimes) | %s |" % col(dates))
    print("| passes with these arms | %s |" % col(lambda p: str(len(p))))
    print("| arms per pass | %s |" % col(
        lambda p: "/".join(str(len(ps["phases"])) for ps in p) or "-"))
    print("| diagnostic runs (qvdiag + qvsched) per pass | %s |" % col(
        lambda p: "/".join(str(ps["ndiag"]) for ps in p) or "-"))
    print("| arm before m3w-w2 | %s |" % col(
        lambda p: ", ".join(ps["phase"]["m3w-w2"]["before"] or "first"
                            for ps in p if "m3w-w2" in ps["phase"]) or "-"))


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: xp-report.py x3|x4|x5|rule1 RAW | layout x3|x4|x5 "
                 "OUT | pblayout DIR | retro LABEL=ROOT...")
    cmd = sys.argv[1]
    if cmd == "x3":
        x3(sys.argv[2])
    elif cmd == "x4":
        x4(sys.argv[2])
    elif cmd == "x5":
        x5(sys.argv[2])
    elif cmd == "rule1":
        rule1_cmd(sys.argv[2])
    elif cmd == "layout":
        if len(sys.argv) != 4 or sys.argv[2] not in ("x3", "x4", "x5"):
            sys.exit("usage: layout x3|x4|x5 OUT")
        sys.exit(1 if layout_check(sys.argv[2], sys.argv[3]) else 0)
    elif cmd == "pblayout":
        sys.exit(1 if pb_layout_check(sys.argv[2]) else 0)
    elif cmd == "retro":
        retro(sys.argv[2:])
    else:
        sys.exit("unknown command %s" % cmd)


if __name__ == "__main__":
    main()
