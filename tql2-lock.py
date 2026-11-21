#!/usr/bin/env python3
"""TASK-1120-TQL2 lock contention tables, from lock:contention_begin and
lock:contention_end samples with kernel call chains.

    perf script -i DATA -F comm,tid,cpu,time,event,trace,ip,sym,symoff |
        tql2-lock.py agg NCPU OUT.md OUT.tsv
    tql2-lock.py summary RAW > summary.md

agg pairs each contention_begin with the contention_end of the same
thread and lock address (a second begin before the end, as a mutex gives
when its optimistic spin turns into a sleep, continues the first) and
charges the wait to:

- the taker: the first frame of the call chain that is not a lock or
  scheduler function, i.e. the function that called the lock primitive;
- the "TQL caller": what TASK-1118-TQL's perf lock contention printed,
  i.e. perf's default without BPF: the first 8 frames, the first 4
  skipped, then the first frame that is not a lock or scheduler
  function;
- a class, from the taker and the two frames above it (CLASSES below).

summary reads RAW/<pass>/<arm>/<row>.agg.tsv and the counter snapshots
and prints the per row and arm means over the passes.
"""

import collections
import glob
import os
import re
import sys

HDR = re.compile(
    r"^\s*(?P<comm>.*?)\s+(?P<tid>\d+)\s+\[(?P<cpu>\d+)\]\s+"
    r"(?P<time>\d+\.\d+):\s+lock:contention_(?P<ev>begin|end):\s+"
    r"(?P<addr>0x[0-9a-f]+)\s+\((?P<rest>[^)]*)\)")
FRAME = re.compile(r"^\s+([0-9a-f]+)\s+(\S+)")

# Lock primitives and scheduler functions (perf's "lock functions" are
# the kernel's lock and sched text sections; these are their members
# that appear in contention call chains).
LOCKFN = re.compile(
    r"^(native_queued_spin_lock_slowpath|__pv_queued_spin_lock_slowpath"
    r"|queued_spin_lock_slowpath|pv_queued_spin_lock_slowpath"
    r"|_raw_(spin|read|write)_(try)?lock\w*|queued_(read|write)_lock_slowpath"
    r"|__mutex_lock\w*|mutex_lock\w*|__mutex_unlock_slowpath|mutex_unlock"
    r"|__ww_mutex_lock\w*|ww_mutex_lock\w*|mutex_spin_on_owner|osq_lock"
    r"|rwsem_down_(read|write)_slowpath|rwsem_optimistic_spin"
    r"|down_(read|write)\w*|__down_(read|write)\w*|down|__down\w*"
    r"|__percpu_down_\w+|percpu_down_\w+|percpu_rwsem_\w+"
    r"|rt_mutex\w*|__rt_mutex\w*|rtlock_\w+|rt_spin_lock\w*"
    r"|schedule|__schedule|schedule_preempt_disabled|io_schedule\w*"
    r"|schedule_timeout\w*|preempt_schedule\w*|__cond_resched"
    r"|wait_for_completion\w*)$")
WQFN = re.compile(
    r"^(__wake_up\w*|add_wait_queue\w*|remove_wait_queue|prepare_to_wait"
    r"\w*|finish_wait|init_wait_entry|__wake_up_common\w*)$")
WAKEFN = re.compile(r"^(try_to_wake_up|ttwu_\w+|wake_up_\w+|"
                    r"autoremove_wake_function|default_wake_function)$")
TQFN = re.compile(r"^(taskq_\w+|task_\w+)$")

CLASS_ORDER = (
    "tq_lock: worker (taskq_thread)",
    "tq_lock: dispatch (taskq_dispatch*)",
    "tq_lock: other taskq_*/task_*",
    "taskq wait-queue lock (__wake_up etc. from taskq_*)",
    "taskq wake-up, scheduler locks (from taskq_*)",
    "tx_sync_lock: re-acquired after a txg wait (cv_wait_common)",
    "tx_sync_lock: entered (txg_wait_*, txg_kick, txg threads)",
    "tx_cpu locks: txg_hold_open",
    "tx_cpu locks: txg_rele_to_*",
    "tx_cpu locks: txg_quiesce",
    "dn_mtx: re-acquired after dn_notxholds wait (dmu_tx_wait)",
    "dmu_tx_* taker (dn_mtx, dp_lock, m_lock of a kmutex)",
    "zil itxg_lock (zil_itx_assign)",
    "other",
)


def base(sym):
    """Function name without +offset and without .part.N-like suffixes."""
    s = sym.split("+", 1)[0]
    return s.split(".", 1)[0]


def lock_type(flags):
    f = set(flags.replace("flags=", "").split("|")) - {""}
    if "MUTEX" in f:
        return "mutex"
    if "PERCPU" in f:
        return "percpu-rwsem"
    if "RT" in f:
        return "rtmutex"
    if "SPIN" in f:
        if "READ" in f or "WRITE" in f:
            return "rwlock"
        return "spinlock"
    if "READ" in f or "WRITE" in f:
        return "rwsem"
    return "+".join(sorted(f)) or "unknown"


def taker_index(frames):
    for i, (_, sym) in enumerate(frames):
        if not LOCKFN.match(base(sym)):
            return i
    return None


def tql_caller(frames):
    """perf lock contention's default caller without BPF (TQL)."""
    for i, (_, sym) in enumerate(frames[:8]):
        if i < 4:
            continue
        if sym != "[unknown]" and not LOCKFN.match(base(sym)):
            return sym
    return "?"


def classify(ltype, frames, ti):
    if ti is None:
        return "other"
    names = [base(s) for _, s in frames[ti:ti + 4]] + ["", "", "", ""]
    t, p, g = names[0], names[1], names[2]
    if ltype == "spinlock":
        if t == "taskq_thread":
            return CLASS_ORDER[0]
        if t.startswith("taskq_dispatch"):
            return CLASS_ORDER[1]
        if TQFN.match(t):
            return CLASS_ORDER[2]
        if WQFN.match(t) and (TQFN.match(p) or TQFN.match(g)):
            return CLASS_ORDER[3]
        if WAKEFN.match(t) or WQFN.match(t):
            if any(TQFN.match(x) for x in names[1:4]):
                return CLASS_ORDER[4]
    if ltype == "mutex":
        if t == "cv_wait_common" and p.startswith("txg_wait"):
            return CLASS_ORDER[5]
        if t.startswith("txg_wait") or t in ("txg_kick", "txg_sync_thread",
                                             "txg_quiesce_thread",
                                             "txg_delay"):
            return CLASS_ORDER[6]
        if t == "txg_hold_open":
            return CLASS_ORDER[7]
        if t.startswith("txg_rele_to"):
            return CLASS_ORDER[8]
        if t == "txg_quiesce" or (t == "cv_wait_common" and
                                  p == "txg_quiesce"):
            return CLASS_ORDER[9]
        if t == "cv_wait_common" and p == "dmu_tx_wait":
            return CLASS_ORDER[10]
        if t == "zil_itx_assign":
            return CLASS_ORDER[12]
    if t.startswith("dmu_tx_"):
        return CLASS_ORDER[11]
    return "other"


class Stat:
    __slots__ = ("n", "tot", "mx")

    def __init__(self):
        self.n = 0
        self.tot = 0.0
        self.mx = 0.0

    def add(self, w):
        self.n += 1
        self.tot += w
        if w > self.mx:
            self.mx = w


def parse(stream):
    """Yield (ev, tid, addr, time, rest, frames) per sample."""
    cur = None
    frames = []
    for ln in stream:
        m = HDR.match(ln)
        if m:
            if cur:
                yield cur + (frames,)
            cur = (m.group("ev"), int(m.group("tid")), m.group("addr"),
                   float(m.group("time")), m.group("rest"))
            frames = []
            continue
        f = FRAME.match(ln)
        if f and cur:
            frames.append((f.group(1), f.group(2)))
    if cur:
        yield cur + (frames,)


def agg(ncpu, out_md, out_tsv):
    pending = {}
    nb = ne = unmatched_end = cont = 0
    t0 = t1 = None
    by_class = collections.defaultdict(Stat)
    by_taker = collections.defaultdict(Stat)
    by_tql = collections.defaultdict(Stat)
    tql_to = collections.defaultdict(lambda: collections.defaultdict(Stat))
    stacks = {}
    for ev, tid, addr, t, rest, frames in parse(sys.stdin):
        t0 = t if t0 is None else t0
        t1 = t
        key = (tid, addr)
        if ev == "begin":
            nb += 1
            if key in pending:
                cont += 1
                continue
            pending[key] = (t, rest, frames)
            continue
        ne += 1
        b = pending.pop(key, None)
        if b is None:
            unmatched_end += 1
            continue
        w = t - b[0]
        ltype = lock_type(b[1])
        fr = b[2]
        ti = taker_index(fr)
        cl = classify(ltype, fr, ti)
        tk = tuple(s for _, s in fr[ti:ti + 3]) if ti is not None else ("?",)
        caller = tql_caller(fr)
        by_class[(cl, ltype)].add(w)
        by_taker[(ltype,) + tk + (cl,)].add(w)
        by_tql[(ltype, caller)].add(w)
        tql_to[(ltype, caller)][(tk[0], cl)].add(w)
        k2 = (ltype,) + tk + (cl,)
        if k2 not in stacks:
            stacks[k2] = [s for _, s in fr[:16]]
    span = (t1 - t0) if t0 is not None and t1 is not None else 0.0
    cpu_s = span * ncpu if span > 0 else 1.0
    with open(out_md, "w") as f:
        f.write(f"capture {span:.2f} s, {ncpu} CPUs; contention_begin "
                f"{nb} (continued {cont}), contention_end {ne}, "
                f"end without begin {unmatched_end}, begin without end "
                f"{len(pending)}\n\n")
        f.write("### By class\n\n| class | type | contended | wait s | "
                "% of CPU time | avg us | max ms |\n|---|---|---|---|---|"
                "---|---|\n")
        for (cl, lt), s in sorted(by_class.items(),
                                  key=lambda kv: -kv[1].tot):
            f.write(f"| {cl} | {lt} | {s.n} | {s.tot:.3f} | "
                    f"{100 * s.tot / cpu_s:.3f} | "
                    f"{1e6 * s.tot / s.n:.1f} | {1e3 * s.mx:.2f} |\n")
        f.write("\n### Top 30 takers (taker, its caller, the caller's "
                "caller)\n\n| type | taker | caller | caller's caller | "
                "class | contended | wait s | avg us | max ms |\n"
                "|---|---|---|---|---|---|---|---|---|\n")
        top = sorted(by_taker.items(), key=lambda kv: -kv[1].tot)[:30]
        for k, s in top:
            tk = list(k[1:-1]) + ["", "", ""]
            f.write(f"| {k[0]} | {tk[0]} | {tk[1]} | {tk[2]} | {k[-1]} | "
                    f"{s.n} | {s.tot:.3f} | {1e6 * s.tot / s.n:.1f} | "
                    f"{1e3 * s.mx:.2f} |\n")
        f.write("\n### TQL callers (perf's default: 8 frames, 4 skipped) "
                "and the takers behind them\n\n| type | TQL caller | wait s "
                "| contended | takers behind it (share of wait) |\n"
                "|---|---|---|---|---|\n")
        for (lt, caller), s in sorted(by_tql.items(),
                                      key=lambda kv: -kv[1].tot)[:20]:
            parts = sorted(tql_to[(lt, caller)].items(),
                           key=lambda kv: -kv[1].tot)[:3]
            desc = "; ".join(f"{tk} [{cl}] {100 * x.tot / s.tot:.0f} %"
                             for (tk, cl), x in parts)
            f.write(f"| {lt} | {caller} | {s.tot:.3f} | {s.n} | {desc} |\n")
        f.write("\n### Call chains of the top 10 takers (first sample, "
                "innermost first)\n\n")
        for k, s in top[:10]:
            f.write(f"- {k[0]} {k[1]} [{k[-1]}], {s.tot:.3f} s: "
                    + " <- ".join(stacks.get(k, [])) + "\n")
    with open(out_tsv, "w") as f:
        f.write(f"#span\t{span:.3f}\t{ncpu}\t{nb}\t{ne}\t{unmatched_end}"
                f"\t{len(pending)}\n")
        for (cl, lt), s in by_class.items():
            f.write(f"class\t{cl}\t{lt}\t{s.n}\t{s.tot:.6f}\t{s.mx:.6f}\n")
        for (lt, caller), s in by_tql.items():
            if s.tot >= 0.005:
                f.write(f"tql\t{caller}\t{lt}\t{s.n}\t{s.tot:.6f}\t"
                        f"{s.mx:.6f}\n")
        for k, s in sorted(by_taker.items(), key=lambda kv: -kv[1].tot)[:40]:
            f.write(f"taker\t{'/'.join(k[1:-1])}\t{k[0]}\t{s.n}\t"
                    f"{s.tot:.6f}\t{s.mx:.6f}\t{k[-1]}\n")


def load_tsv(path):
    d = {"class": {}, "tql": {}, "taker": {}, "span": None}
    with open(path) as f:
        for ln in f:
            w = ln.rstrip("\n").split("\t")
            if w[0] == "#span":
                d["span"] = (float(w[1]), int(w[2]))
            elif w[0] in ("class", "tql", "taker"):
                d[w[0]][(w[1], w[2])] = (int(w[3]), float(w[4]))
    return d


def load_snap(path):
    out = {}
    try:
        with open(path) as f:
            for ln in f:
                w = ln.split()
                if len(w) == 3 and w[0] == "dmu_tx":
                    out[w[1]] = int(w[2])
                elif len(w) == 3 and w[0] == "txa":
                    out["txa " + w[1]] = int(w[2])
    except OSError:
        pass
    return out


def summary(raw):
    rows = collections.defaultdict(list)
    for p in sorted(glob.glob(os.path.join(raw, "L*", "*", "*.agg.tsv"))):
        arm = os.path.basename(os.path.dirname(p))
        row = os.path.basename(p)[:-len(".agg.tsv")]
        rows[(row, arm)].append(p)
    if not rows:
        print("No .agg.tsv under", raw)
        return
    print("# TASK-1120-TQL2 lock summary\n")
    print("Means over the lock passes; % of CPU time = wait / (capture s "
          "x CPUs).  A spinlock's wait is CPU spent spinning; a mutex's "
          "includes sleeping.\n")
    print("## Classes\n")
    print("| row | arm | passes | class | type | wait s | % of CPU time | "
          "contended |\n|---|---|---|---|---|---|---|---|")
    keyrows = sorted(rows)
    for key in keyrows:
        ds = [load_tsv(p) for p in rows[key]]
        acc = collections.defaultdict(lambda: [0, 0.0, 0.0])
        for d in ds:
            span, ncpu = d["span"]
            for (cl, lt), (n, tot) in d["class"].items():
                a = acc[(cl, lt)]
                a[0] += n
                a[1] += tot
                a[2] += 100 * tot / (span * ncpu) if span > 0 else 0
        k = len(ds)
        for (cl, lt), a in sorted(
                acc.items(), key=lambda kv: (CLASS_ORDER.index(kv[0][0])
                                             if kv[0][0] in CLASS_ORDER
                                             else 99, -kv[1][1])):
            print(f"| {key[0]} | {key[1]} | {k} | {cl} | {lt} | "
                  f"{a[1] / k:.3f} | {a[2] / k:.3f} | {a[0] // k} |")
    print("\n## tq_lock and taskq wake-up totals (the tq_lock lead)\n")
    print("| row | arm | tq_lock s | tq_lock % CPU | worker s | dispatch s "
          "| taskq wait-queue s | TQL metric (callers taskq_*) s |\n"
          "|---|---|---|---|---|---|---|---|")
    for key in keyrows:
        ds = [load_tsv(p) for p in rows[key]]
        k = len(ds)
        tq = wk = dp = wq = pct = tqlm = 0.0
        for d in ds:
            span, ncpu = d["span"]
            for (cl, lt), (n, tot) in d["class"].items():
                if cl.startswith("tq_lock"):
                    tq += tot
                    pct += 100 * tot / (span * ncpu) if span > 0 else 0
                if cl == CLASS_ORDER[0]:
                    wk += tot
                if cl == CLASS_ORDER[1]:
                    dp += tot
                if cl == CLASS_ORDER[3]:
                    wq += tot
            for (caller, lt), (n, tot) in d["tql"].items():
                if caller.startswith("taskq_"):
                    tqlm += tot
        print(f"| {key[0]} | {key[1]} | {tq / k:.3f} | {pct / k:.3f} | "
              f"{wk / k:.3f} | {dp / k:.3f} | {wq / k:.3f} | "
              f"{tqlm / k:.3f} |")
    print("\n## TQL callers resolved (mean wait s; the takers behind each "
          "are in the row's .agg.md)\n")
    print("| row | arm | TQL caller | type | wait s |\n|---|---|---|---|---|")
    for key in keyrows:
        ds = [load_tsv(p) for p in rows[key]]
        k = len(ds)
        acc = collections.defaultdict(float)
        for d in ds:
            for (caller, lt), (n, tot) in d["tql"].items():
                acc[(caller, lt)] += tot
        for (caller, lt), tot in sorted(acc.items(),
                                        key=lambda kv: -kv[1])[:6]:
            print(f"| {key[0]} | {key[1]} | {caller} | {lt} | "
                  f"{tot / k:.3f} |")
    print("\n## dmu_tx counters per row (after - before, mean)\n")
    names = ("dmu_tx_assigned", "dmu_tx_quota", "dmu_tx_group",
             "dmu_tx_dirty_delay", "dmu_tx_dirty_over_max",
             "dmu_tx_dirty_throttle", "dmu_tx_memory_reserve")
    print("| row | arm | " + " | ".join(n[7:] for n in names) +
          " |\n|---|---|" + "---|" * len(names))
    for key in keyrows:
        vals = collections.defaultdict(list)
        for p in rows[key]:
            pre = p[:-len(".agg.tsv")]
            s0, s1 = load_snap(pre + ".snap0"), load_snap(pre + ".snap1")
            for n in names:
                if n in s0 and n in s1:
                    vals[n].append(s1[n] - s0[n])
        cells = [f"{sum(vals[n]) // len(vals[n])}" if vals[n] else "n/a"
                 for n in names]
        print(f"| {key[0]} | {key[1]} | " + " | ".join(cells) + " |")


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "agg":
        agg(int(sys.argv[2]), sys.argv[3], sys.argv[4])
    elif len(sys.argv) == 3 and sys.argv[1] == "summary":
        summary(sys.argv[2])
    else:
        sys.exit("usage: tql2-lock.py agg NCPU OUT.md OUT.tsv | "
                 "summary RAW")


if __name__ == "__main__":
    main()
