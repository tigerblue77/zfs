#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
#
# Reporting side of tests/perf-xpool/run.sh: turns the fio JSON, /proc and
# kstat snapshots, perf reports and bpftrace output it leaves in OUT into a
# compact text summary.  Also holds the /proc/<tid>/stack sampler.
#
#   report.py fioline FIO_JSON
#   report.py sample SECONDS INTERVAL COMM OUT_JSON
#   report.py q1 OUT
#   report.py q2 OUT
#   report.py qz OUT

import collections
import json
import os
import re
import statistics
import sys
import time

USER_HZ_US = 10000      # one /proc/stat jiffy (USER_HZ = 100) in microseconds


def readf(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def median(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def fmt(v, nd=1):
    if v is None:
        return "-"
    if isinstance(v, float) and abs(v) >= 10000:
        return "%.0f" % v
    return ("%." + str(nd) + "f") % v


def pct_change(new, base):
    if new is None or base in (None, 0):
        return None
    return 100.0 * (new - base) / base


# ---------------------------------------------------------------- fio ----

def load_fio(path):
    txt = readf(path)
    if not txt or "{" not in txt:
        return None
    try:
        data = json.loads(txt[txt.index("{"):])
    except ValueError:
        return None
    jobs = data.get("jobs") or []
    if not jobs:
        return None
    job = jobs[0]
    res = {"usr_cpu": job.get("usr_cpu"), "sys_cpu": job.get("sys_cpu")}
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
        res.update({
            "dir": k,
            "iops": d.get("iops"),
            "ios": d.get("total_ios"),
            "bw_mib": d.get("bw_bytes", 0) / 1048576.0,
            "mean": lat.get("mean", 0) / 1000.0,
            "p50": p("50.000000"),
            "p99": p("99.000000"),
            "p999": p("99.900000"),
            "max": lat.get("max", 0) / 1000.0,
        })
        break
    return res if "dir" in res else None


def stat_busy(path):
    txt = readf(path)
    if not txt:
        return None
    f = [int(x) for x in txt.split()[1:]]
    # user nice system idle iowait irq softirq steal ...
    busy = f[0] + f[1] + f[2] + f[5] + f[6]
    return busy, sum(f[:8])


def uptime(path):
    txt = readf(path)
    return float(txt.split()[0]) if txt else None


def kstat(path):
    """Parse a named kstat (name type data) into {name: int}."""
    out = {}
    txt = readf(path)
    if not txt:
        return out
    for line in txt.splitlines()[2:]:
        p = line.split()
        if len(p) == 3:
            try:
                out[p[0]] = int(p[2])
            except ValueError:
                pass
    return out


def idle_rate(out):
    b0 = stat_busy(os.path.join(out, "idle.pre.stat"))
    b1 = stat_busy(os.path.join(out, "idle.post.stat"))
    t0 = uptime(os.path.join(out, "idle.pre.uptime"))
    t1 = uptime(os.path.join(out, "idle.post.uptime"))
    if None in (b0, b1, t0, t1) or t1 <= t0:
        return 0.0
    return (b1[0] - b0[0]) / (t1 - t0)


def load_run(out, label, irate):
    d = os.path.join(out, "runs", label)
    r = load_fio(os.path.join(d, "fio.json"))
    if r is None:
        return None
    b0 = stat_busy(os.path.join(d, "pre.stat"))
    b1 = stat_busy(os.path.join(d, "post.stat"))
    t0 = uptime(os.path.join(d, "pre.uptime"))
    t1 = uptime(os.path.join(d, "post.uptime"))
    if None not in (b0, b1, t0, t1) and t1 > t0:
        busy = (b1[0] - b0[0]) - irate * (t1 - t0)
        r["cpu_us_io"] = busy * USER_HZ_US / r["ios"] if r["ios"] else None
        r["busy_pct"] = 100.0 * (b1[0] - b0[0]) / max(1, b1[1] - b0[1])
    c0 = readf(os.path.join(d, "pre.ctxt"))
    c1 = readf(os.path.join(d, "post.ctxt"))
    if c0 and c1 and r["ios"]:
        r["ctx_io"] = (int(c1.split()[1]) - int(c0.split()[1])) / r["ios"]
    a0 = kstat(os.path.join(d, "pre.arcstats"))
    a1 = kstat(os.path.join(d, "post.arcstats"))
    if a0 and a1 and r["ios"]:
        r["miss_per_io"] = (a1.get("demand_data_misses", 0) -
                            a0.get("demand_data_misses", 0)) / r["ios"]
    return r


def cmd_fioline(path):
    r = load_fio(path)
    if r is None:
        print("    fio: no result in %s" % path)
        return
    print("    %-5s iops=%8.0f mean=%7.1fus p50=%7.1f p99=%7.1f "
          "p99.9=%8.1f max=%9.1f" % (r["dir"], r["iops"], r["mean"],
                                     r["p50"] or 0, r["p99"] or 0,
                                     r["p999"] or 0, r["max"]))


# ------------------------------------------------------ /proc sampler ----

def fast_tids(comm):
    tids = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        c = readf("/proc/%s/comm" % pid, "").strip()
        if c != comm:
            continue
        try:
            tids += [(pid, t) for t in os.listdir("/proc/%s/task" % pid)]
        except OSError:
            pass
    return tids


def cmd_sample(seconds, interval, comm, outpath):
    end = time.time() + float(seconds)
    counts = collections.Counter()
    n = 0
    tids = []
    it = 0
    while time.time() < end:
        if it % 50 == 0:
            tids = fast_tids(comm)
        it += 1
        for pid, tid in tids:
            st = readf("/proc/%s/task/%s/stat" % (pid, tid))
            if not st:
                continue
            state = st[st.rindex(")") + 2]
            if state == "R":
                key = "R"
            else:
                stk = readf("/proc/%s/task/%s/stack" % (pid, tid), "")
                frames = []
                for line in stk.splitlines():
                    m = re.search(r"\]\s+([^+\s]+)", line)
                    if m:
                        frames.append(m.group(1))
                key = state + ";" + ";".join(frames[:14])
            counts[key] += 1
            n += 1
        time.sleep(float(interval))
    with open(outpath, "w") as f:
        json.dump({"samples": n, "stacks": dict(counts)}, f)


# ------------------------------------------------------------- perf ----

IDLE_RE = re.compile(
    r"(safe_halt|default_idle|cpuidle|intel_idle|acpi_idle|poll_idle|"
    r"do_idle|cpu_startup_entry|mwait_idle|arch_cpu_idle|cpu_idle_poll|"
    r"amd_e400_idle|haltpoll|native_halt|acpi_safe_halt|__cpuidle)")

CATEGORIES = [
    ("checksum", r"fletcher|zio_checksum|sha256|sha512|blake3|skein|edonr"),
    ("compress", r"lz4|zle_|zio_compress|zio_decompress|zstd|gzip"),
    ("abd/memcpy", r"^abd_|memcpy|memmove|copy_user|copy_to_user|"
     r"copy_from_user|rep_movs|_copy_to_iter|_copy_from_iter|copyin|"
     r"copyout|clear_page|memset|copy_page"),
    ("taskq/cv (SPL)", r"^taskq_|^cv_|^__cv_|^task_|spl_taskq"),
    ("sched/wakeup/IPI", r"schedule|wake_up|try_to_wake|ttwu|"
     r"finish_task_switch|enqueue_|dequeue_|update_curr|pick_next|"
     r"select_task_rq|send_call_func|send_IPI|x2apic|sysvec_|"
     r"smp_call|psi_|cpuacct|reweight|set_next_entity|put_prev|"
     r"update_load|newidle_balance|load_balance|switch_to|switch_mm|"
     r"hrtimer|tick_|rcu_|raw_spin_rq|activate_task|resched|"
     r"enqueue_task|check_preempt|kvm_|pv_|hv_"),
    ("locks", r"mutex|spin_lock|spin_unlock|spin_trylock|queued_spin|"
     r"osq_lock|rwsem|^rw_enter|^rw_exit|^rw_tryenter|down_read|up_read|"
     r"down_write|up_write|_raw_read|_raw_write|rrw_|rrm_"),
    ("ZIL", r"^zil_|lwb"),
    ("zio/vdev/metaslab", r"^zio_|^vdev_|^metaslab_|^spa_|^range_tree|"
     r"^zfs_btree|^space_map"),
    ("DMU/dbuf/ARC/txg", r"^dmu_|^dbuf_|^arc_|^dnode_|^dsl_|^txg_|"
     r"buf_hash|multilist|aggsum|wmsum|^l2arc|^dbu_|^zrl_"),
    ("ZPL/zvol", r"^zfs_|^zpl_|^zvol_|^zfs_rangelock|^zcommon"),
    ("kmem/page alloc", r"spl_kmem|kmem_cache|kmalloc|kfree|slab|vmem|"
     r"alloc_pages|get_page_from_freelist|free_unref|__free_pages|"
     r"rmqueue|__alloc|mempool|memcg|page_counter"),
    ("block layer/null_blk/dm", r"^blk_|^bio_|^null_|submit_bio|^dm_|"
     r"bvec|end_bio|^__blk|^__bio|delay_|^nullb|^bdev_|iomap"),
    ("syscall/vfs/io_uring", r"entry_SYSCALL|do_syscall|syscall|io_uring|"
     r"^io_|__x64_sys|ksys_|^vfs_|rw_verify|fsnotify|security_|apparmor|"
     r"fdget|__fget|fput|aa_|lsm_|file_|iov_iter"),
]
CAT_RE = [(n, re.compile(r)) for n, r in CATEGORIES]


def categorize(sym, dso):
    if dso == ".":
        return "userspace (fio)"
    for n, r in CAT_RE:
        if r.search(sym):
            return n
    return "other kernel"


def parse_perf_self(path):
    rows = []
    for line in (readf(path, "") or "").splitlines():
        m = re.match(r"^\s*([\d.]+)%\s+\[(.)\]\s+(.+?)\s*$", line)
        if m:
            rows.append((float(m.group(1)), m.group(2), m.group(3)))
    return rows


def parse_perf_children(path):
    rows = []
    for line in (readf(path, "") or "").splitlines():
        m = re.match(r"^\s*([\d.]+)%\s+([\d.]+)%\s+\[(.)\]\s+(.+?)\s*$",
                     line)
        if m:
            rows.append((float(m.group(1)), float(m.group(2)),
                         m.group(3), m.group(4)))
    return rows


def parse_perf_comm(path):
    rows = []
    for line in (readf(path, "") or "").splitlines():
        m = re.match(r"^\s*([\d.]+)%\s+(\S.*?)\s*$", line)
        if m and not line.lstrip().startswith("#"):
            rows.append((float(m.group(1)), m.group(2)))
    return rows


def perf_summary(prefix, label, top=30, children=True, comm=False):
    rows = parse_perf_self(prefix + ".self.txt")
    if not rows:
        print("  %s: no perf data" % label)
        return
    idle = sum(p for p, d, s in rows if IDLE_RE.search(s))
    busy = max(1e-9, sum(p for p, d, s in rows) - idle)
    cats = collections.Counter()
    for p, d, s in rows:
        if not IDLE_RE.search(s):
            cats[categorize(s, d)] += p
    print("  %s: %.1f%% of samples idle; shares below are of the "
          "non-idle %.1f%%" % (label, idle, busy))
    print("    categories: " + ", ".join(
        "%s %.1f%%" % (n, 100 * v / busy) for n, v in cats.most_common()))
    if top:
        print("    top %d self symbols (%% of non-idle):" % top)
        n = 0
        for p, d, s in rows:
            if IDLE_RE.search(s):
                continue
            n += 1
            print("      %5.1f%%  [%s] %s" % (100 * p / busy, d, s))
            if n >= top:
                break
    if children:
        ch = parse_perf_children(prefix + ".children.txt")
        want = re.compile(r"^(zfs_|zpl_|zvol_|zil_|zio_|dmu_|dbuf_|arc_|"
                          r"taskq_|vdev_|txg_|spa_sync|metaslab_|abd_|"
                          r"fletcher|try_to_wake_up|__schedule|schedule$|"
                          r"__cv_|cv_|null_|submit_bio)")
        sel = [r for r in ch if want.search(r[3])][:25]
        if sel:
            print("    inclusive (children, % of non-idle): " +
                  "; ".join("%s %.1f" % (s, 100 * c / busy)
                            for c, _s, _d, s in sel))
    if comm:
        cr = parse_perf_comm(prefix + ".comm.txt")
        if cr:
            print("    by thread name (%% of all samples): " +
                  ", ".join("%s %.1f" % (c, p) for p, c in cr[:14]))


# --------------------------------------------------------- bpftrace ----

PURE_SCHED = re.compile(
    r"^(finish_task_switch|__schedule|schedule|io_schedule|preempt_schedule|"
    r"__cond_resched|_cond_resched|cond_resched|__traceiter|bpf_|kprobe|"
    r"__kprobe|irqentry_exit|raw_irqentry|asm_|exit_to_user|"
    r"cv_wait|__cv_wait|__cv_timedwait|cv_timedwait|cv_wait_common|"
    r"__cv_broadcast|do_nanosleep|hrtimer_nanosleep|usleep_range|"
    r"msleep|__wait_|wait_for_completion|prepare_to_wait)")
LOCK_FRAMES = re.compile(
    r"(mutex|rwsem|^rw_enter|down_read|down_write|rt_mutex|osq_lock|"
    r"^__down|rrw_enter)")
ENTRY_FRAMES = re.compile(
    r"^(__x64_sys|do_syscall_64|entry_SYSCALL|x64_sys_call|ret_from_fork|"
    r"kthread$|__do_sys|ksys_|syscall_exit)")
def parse_bt_maps(path):
    """Return {mapname: [(keyparts, value)]} from bpftrace text output."""
    maps = collections.defaultdict(list)
    lines = (readf(path, "") or "").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^@(\w+): (\d+)$", line)
        if m:
            maps[m.group(1)].append(([], int(m.group(2))))
            i += 1
            continue
        m = re.match(r"^@(\w+)\[(.*)\]: (\d+)$", line)
        if m:
            maps[m.group(1)].append(([x.strip() for x in
                                      m.group(2).split(",")],
                                     int(m.group(3))))
            i += 1
            continue
        m = re.match(r"^@(\w+)\[$", line)
        if m:
            name = m.group(1)
            frames = []
            i += 1
            while i < len(lines):
                ln = lines[i]
                e = re.match(r"^(,.*)?\]: (\d+)$", ln)
                if e:
                    extra = [x.strip() for x in (e.group(1) or "").split(",")
                             if x.strip()]
                    maps[name].append(([frames] + extra, int(e.group(2))))
                    break
                fr = ln.split()
                if fr:
                    frames.append(re.sub(r"\+.*$", "", fr[0]))
                i += 1
        i += 1
    return maps


def elapsed_ms(path):
    m = re.search(r"elapsed_ms (\d+)", readf(path, "") or "")
    return int(m.group(1)) if m else None


STATE = {"0": "R", "1": "S", "2": "D", "256": "R+(preempted)",
         "1026": "D|NOLOAD"}


def reason(frames, state):
    if state == "256":
        return "preempted (CPU contention)"
    inner = [f for f in frames if not PURE_SCHED.search(f)]
    if inner and LOCK_FRAMES.search(inner[0]):
        caller = next((f for f in inner if not LOCK_FRAMES.search(f)), "?")
        return "lock %s in %s" % (inner[0], caller)
    s = ";".join(frames)
    for key, name in (
            ("arc_wait_for_eviction", "ARC eviction wait (global)"),
            ("dmu_tx_delay", "dmu_tx delay (write throttle)"),
            ("dmu_tx_wait", "dmu_tx_wait (tx assign)"),
            ("txg_wait_open", "txg_wait_open"),
            ("txg_wait_synced", "txg_wait_synced"),
            ("zil_commit_waiter", "ZIL commit wait (own lwb I/O)"),
            ("zil_commit", "ZIL commit (other)"),
            ("zio_wait", "zio_wait (own I/O)"),
            ("dbuf_evict", "dbuf eviction"),
            ("zfs_rangelock", "range lock")):
        if key in s:
            return name
    if state == "0":
        return "yield/cond_resched: " + (inner[0] if inner else "?")
    return "other: " + (inner[0] if inner else "?")


def trim(frames, n=9):
    out = []
    for f in frames:
        if not out and PURE_SCHED.search(f):
            continue
        if ENTRY_FRAMES.search(f):
            break
        out.append(f)
        if len(out) >= n:
            break
    return out or frames[:n]


def offcpu_summary(path, ops, top=8):
    maps = parse_bt_maps(path)
    el = elapsed_ms(path)
    off = maps.get("off_us", [])
    cnt = {}
    for k, v in maps.get("off_n", []):
        cnt[(tuple(k[0]), k[1] if len(k) > 1 else "")] = v
    if not off:
        sw = maps.get("sw_n", [])
        if sw:
            print("    (fallback tracepoint stacks, counts only)")
            tot = sum(v for _k, v in sw)
            for k, v in sorted(sw, key=lambda x: -x[1])[:top]:
                print("      %5.1f%% n=%d %s %s" % (
                    100.0 * v / tot, v, STATE.get(k[1], k[1]),
                    " <- ".join(trim(k[0]))))
        else:
            print("    no off-CPU data (%s)" % os.path.basename(path))
        return {}
    total = sum(v for _k, v in off)
    bystate = collections.Counter()
    byreason = collections.Counter()
    for k, v in off:
        st = k[1] if len(k) > 1 else ""
        bystate[STATE.get(st, st)] += v
        byreason[reason(k[0], st)] += v
    secs = (el or 0) / 1000.0
    print("    traced %.1fs, off-CPU total %.0f ms over %s threads-time; "
          "ops in window ~%s" % (secs, total / 1000.0,
                                 "fiofast", fmt(ops, 0)))
    print("    by state: " + ", ".join("%s %.1f%%" % (s, 100.0 * v / total)
                                       for s, v in bystate.most_common()))
    print("    by reason (off-CPU us per I/O): " + "; ".join(
        "%s %s (%.1f%%)" % (r, fmt(v / ops if ops else None, 2),
                            100.0 * v / total)
        for r, v in byreason.most_common(8)))
    print("    top stacks by off-CPU time:")
    for k, v in sorted(off, key=lambda x: -x[1])[:top]:
        st = k[1] if len(k) > 1 else ""
        n = cnt.get((tuple(k[0]), st), 0)
        print("      %5.1f%% n=%-8d avg=%7.1fus %-3s %s" % (
            100.0 * v / total, n, v / n if n else 0,
            STATE.get(st, st), " <- ".join(trim(k[0]))))
    rq = dict((k[0], v) for k, v in maps.get("rq_us", []))
    rqn = dict((k[0], v) for k, v in maps.get("rq_n", []))
    rqm = dict((k[0], v) for k, v in maps.get("rq_max_us", []))
    if rq:
        names = sorted(rq, key=lambda c: -rq[c])[:12]
        if "fiofast" not in names and "fiofast" in rq:
            names.append("fiofast")
        print("    run-queue wait by thread name (total ms, mean us, "
              "max us): " + "; ".join(
                  "%s %.0f/%.1f/%d" % (c, rq[c] / 1000.0,
                                       rq[c] / max(1, rqn.get(c, 1)),
                                       rqm.get(c, 0)) for c in names))
    return byreason


def funclat_summary(path):
    maps = parse_bt_maps(path)
    fast = collections.defaultdict(dict)
    slow = []
    for name, entries in maps.items():
        for pre, field in (("fast_max_us_", "max"), ("fast_us_", "us"),
                           ("fast_n_", "n")):
            if name.startswith(pre):
                fast[name[len(pre):]][field] = entries[0][1]
                break
        else:
            if name.startswith("slow_us_"):
                fn = name[len("slow_us_"):]
                cnt = dict((k[0], v) for k, v in
                           maps.get("slow_n_" + fn, []))
                for k, v in entries:
                    slow.append((v, fn, k[0], cnt.get(k[0], 0)))
    if fast:
        print("    fiofast function latency (calls, mean us, max us): " +
              "; ".join("%s %d/%.1f/%d" % (
                  f, d.get("n", 0), d.get("us", 0) / max(1, d.get("n", 1)),
                  d.get("max", 0))
                  for f, d in sorted(fast.items(),
                                     key=lambda x: -x[1].get("us", 0))))
    if slow:
        print("    calls >= 50us, all threads (fn/comm: n, total ms): " +
              "; ".join("%s/%s %d %.0f" % (fn, c, n, v / 1000.0)
                        for v, fn, c, n in sorted(slow, reverse=True)[:14]))
    if not fast and not slow:
        print("    no function latency data (%s)" % os.path.basename(path))


def procstack_summary(path, top=6):
    txt = readf(path)
    if not txt:
        return
    d = json.loads(txt)
    n = d.get("samples", 0)
    if not n:
        return
    st = sorted(d["stacks"].items(), key=lambda x: -x[1])
    print("    /proc stack samples (%d): " % n + "; ".join(
        "%.1f%% %s" % (100.0 * v / n, (lambda p: p[0] + " " + " <- ".join(
            trim(p[1:], 6)))(k.split(";"))) for k, v in st[:top]))


# ---------------------------------------------------------------- q1 ----

Q1_TARGETS = [("raw", "raw nullb0"), ("f1vol", "zvol, 1 vdev"),
              ("f1fs", "file, 1 vdev"), ("fmvol", "zvol, mirror"),
              ("fmfs", "file, mirror")]
QDNJ = [(1, 1), (1, 4), (32, 1), (32, 4)]


def cmd_q1(out):
    irate = idle_rate(out)
    print("== Q1: 4K I/O on null_blk (memory_backed), medians of runs ==")
    print("idle CPU baseline: %.2f jiffies/s (subtracted)" % irate)
    print(readf(os.path.join(out, "engine.txt"), "").strip())
    res = {}
    for rw in ("randwrite", "randread"):
        print("\n-- %s 4K (%s) --" % (rw, "sync=always / O_SYNC"
                                     if rw == "randwrite" else
                                     "primarycache=metadata, O_DIRECT on "
                                     "devices"))
        print("%-14s %-9s %9s %8s %8s %8s %9s %9s %7s %7s %6s" % (
            "target", "qd/nj", "IOPS", "mean_us", "p50_us", "p99_us",
            "p99.9_us", "CPUus/IO", "dp50", "dCPU", "miss"))
        for t, tname in Q1_TARGETS:
            for qd, nj in QDNJ:
                runs = []
                for rep in range(1, 10):
                    r = load_run(out, "q1.%s.%s.qd%d.nj%d.r%d" % (
                        t, rw, qd, nj, rep), irate)
                    if r:
                        runs.append(r)
                if not runs:
                    continue
                m = dict((k, median([r.get(k) for r in runs]))
                         for k in ("iops", "mean", "p50", "p99", "p999",
                                   "cpu_us_io", "miss_per_io"))
                m["n"] = len(runs)
                m["iops_min"] = min(r["iops"] for r in runs)
                m["iops_max"] = max(r["iops"] for r in runs)
                res[(t, rw, qd, nj)] = m
                base = res.get(("raw", rw, qd, nj))
                dp50 = (m["p50"] - base["p50"]) if base and m["p50"] and \
                    base["p50"] and t != "raw" else None
                dcpu = (m["cpu_us_io"] - base["cpu_us_io"]) if base and \
                    m["cpu_us_io"] is not None and \
                    base.get("cpu_us_io") is not None and t != "raw" \
                    else None
                print("%-14s %-9s %9.0f %8.1f %8s %8s %9s %9s %7s %7s "
                      "%6s" % (tname, "%d/%d" % (qd, nj), m["iops"],
                               m["mean"], fmt(m["p50"]), fmt(m["p99"]),
                               fmt(m["p999"]), fmt(m["cpu_us_io"], 2),
                               fmt(dp50), fmt(dcpu, 2),
                               fmt(m["miss_per_io"], 2) if rw == "randread"
                               and t != "raw" else "-"))
    print("\nIOPS spread across repetitions (min..max): " + "; ".join(
        "%s/%s/%d/%d %.0f..%.0f" % (k[0], k[1][4:], k[2], k[3],
                                    v["iops_min"], v["iops_max"])
        for k, v in sorted(res.items()) if k[2] == 1 and k[3] == 1))
    pdir = os.path.join(out, "perf")
    if os.path.isdir(pdir):
        print("\n== Q1 perf profiles (perf record -a -g, cpu-clock) ==")
        labels = sorted(set(f.rsplit(".", 2)[0] for f in os.listdir(pdir)
                            if f.endswith(".self.txt")))
        for lb in labels:
            r = load_run(out, lb, irate)
            extra = ""
            if r:
                extra = " [iops %.0f, mean %.1fus, CPU %.2fus/IO]" % (
                    r["iops"], r["mean"], r.get("cpu_us_io") or 0)
            full = ("fmfs.randwrite.qd1.nj1" in lb or
                    "fmfs.randread.qd1.nj1" in lb)
            perf_summary(os.path.join(pdir, lb), lb + extra,
                         top=30 if full else 12)


# ---------------------------------------------------------------- qz ----

QZ_TARGETS = [("f1vol", "1 vdev"), ("fmvol", "mirror")]
# 1u: QD1 through io_uring; 1: QD1 through psync
QZ_QDNJ = [("1", 1), ("1u", 1), ("1", 4), ("32", 1), ("32", 4)]


def cmd_qz(out):
    irate = idle_rate(out)
    variants = [v.split(":")[0] for v in
                (readf(os.path.join(out, "qz-variants.txt"), "")).split()]
    print("== QZ: zvol 4K I/O on null_blk, variants interleaved, medians "
          "of repetitions ==")
    print("idle CPU baseline: %.2f jiffies/s (subtracted)" % irate)
    print(readf(os.path.join(out, "engine.txt"), "").strip())
    print("variants: " + " ".join(
        readf(os.path.join(out, "qz-variants.txt"), "").split()))
    res = {}

    def get(v, t, rw, qd, nj):
        runs = []
        for rep in range(1, 10):
            r = load_run(out, "qz.%s.%s.%s.qd%s.nj%d.r%d" % (
                v, t, rw, qd, nj, rep), irate)
            if r:
                runs.append(r)
        if not runs:
            return None
        m = dict((k, median([r.get(k) for r in runs]))
                 for k in ("iops", "mean", "p50", "p99", "p999",
                           "cpu_us_io", "ctx_io"))
        m["iops_min"] = min(r["iops"] for r in runs)
        m["iops_max"] = max(r["iops"] for r in runs)
        m["n"] = len(runs)
        return m

    for rw in ("randwrite", "randread"):
        raw = get("raw", "raw", rw, "1", 1)
        print("\n-- %s 4K (%s) --" % (rw, "sync=always, O_SYNC"
                                     if rw == "randwrite" else
                                     "primarycache=metadata, O_DIRECT"))
        if raw:
            print("raw nullb0 QD1/1: %.0f IOPS (%.0f..%.0f), p50 %s us, "
                  "CPU %s us/IO, ctx %s/IO" % (
                      raw["iops"], raw["iops_min"], raw["iops_max"],
                      fmt(raw["p50"]), fmt(raw["cpu_us_io"], 2),
                      fmt(raw["ctx_io"], 2)))
        print("%-8s %-7s %-6s %8s %15s %8s %8s %8s %8s %6s %7s %7s" % (
            "variant", "pool", "qd/nj", "IOPS", "min..max", "mean_us",
            "p50_us", "p99_us", "CPUus/IO", "ctx/IO", "dIOPS%", "dp50"))
        for t, tname in QZ_TARGETS:
            for qd, nj in QZ_QDNJ:
                for v in variants:
                    m = get(v, t, rw, qd, nj)
                    if m is None:
                        continue
                    res[(v, t, rw, qd, nj)] = m
                    b = res.get((variants[0], t, rw, qd, nj))
                    di = pct_change(m["iops"], b["iops"]) if b and \
                        v != variants[0] else None
                    dp = (m["p50"] - b["p50"]) if b and m["p50"] and \
                        b["p50"] and v != variants[0] else None
                    print("%-8s %-7s %-6s %8.0f %15s %8.1f %8s %8s %8s "
                          "%6s %7s %7s" % (
                              v, tname, "%s/%d" % (qd, nj), m["iops"],
                              "%.0f..%.0f" % (m["iops_min"],
                                              m["iops_max"]),
                              m["mean"], fmt(m["p50"]), fmt(m["p99"]),
                              fmt(m["cpu_us_io"], 2), fmt(m["ctx_io"], 2),
                              fmt(di), fmt(dp)))
    vt = readf(os.path.join(out, "variants.txt"))
    if vt:
        seen = []
        for blk in vt.split("variant ")[1:]:
            if blk not in seen:
                seen.append(blk)
        print("\nparameters as applied (distinct):")
        for blk in seen:
            print("  variant " + blk.rstrip().replace("\n", ";"))
    pdir = os.path.join(out, "perf")
    if os.path.isdir(pdir):
        print("\n== QZ perf profiles (perf record -a -g, cpu-clock) ==")
        labels = sorted(set(f.rsplit(".", 2)[0] for f in os.listdir(pdir)
                            if f.endswith(".self.txt")))
        for lb in labels:
            r = load_run(out, lb, irate)
            extra = ""
            if r:
                extra = " [iops %.0f, mean %.1fus, CPU %.2fus/IO, " \
                    "ctx %.2f/IO]" % (r["iops"], r["mean"],
                                      r.get("cpu_us_io") or 0,
                                      r.get("ctx_io") or 0)
            perf_summary(os.path.join(pdir, lb), lb + extra, top=14,
                         comm=True)


# ---------------------------------------------------------------- q2 ----

Q2_WL = [("rr1", "4K randread QD1 nj1"), ("rr4", "4K randread QD1 nj4"),
         ("sw1", "4K sync write QD1 nj1"), ("sw4", "4K sync write QD1 nj4")]
PHASES = ["alone", "slowwrite", "slowread", "alone2"]


def psi_total(path):
    txt = readf(path, "") or ""
    m = re.search(r"^some .*total=(\d+)", txt, re.M)
    f = re.search(r"^full .*total=(\d+)", txt, re.M)
    return (int(m.group(1)) if m else None, int(f.group(1)) if f else None)


def txgs(path):
    rows = {}
    for line in (readf(path, "") or "").splitlines():
        p = line.split()
        if len(p) >= 12 and p[0].isdigit():
            rows[int(p[0])] = p
    return rows


def hist_kstat(path):
    h = {}
    for line in (readf(path, "") or "").splitlines():
        m = re.match(r"^(\d+) ns\s+\d+\s+(\d+)", line)
        if m:
            h[int(m.group(1))] = int(m.group(2))
    return h


def hist_pct(h, q):
    tot = sum(h.values())
    if not tot:
        return None
    acc = 0
    for b in sorted(h):
        acc += h[b]
        if acc >= q * tot:
            return b
    return max(h)


def phase_report(out, ph):
    s = os.path.join(out, "phase", ph + ".start")
    e = os.path.join(out, "phase", ph + ".end")
    if not os.path.isdir(s) or not os.path.isdir(e):
        print("  (no phase snapshots)")
        return
    t = (uptime(os.path.join(e, "uptime")) or 0) - \
        (uptime(os.path.join(s, "uptime")) or 0)
    b0 = stat_busy(os.path.join(s, "stat"))
    b1 = stat_busy(os.path.join(e, "stat"))
    line = "  phase %.0fs" % t
    if b0 and b1:
        line += ", CPU busy %.1f%%" % (100.0 * (b1[0] - b0[0]) /
                                       max(1, b1[1] - b0[1]))
    for k in ("cpu", "memory", "io"):
        p0 = psi_total(os.path.join(s, "psi." + k))
        p1 = psi_total(os.path.join(e, "psi." + k))
        if p0[0] is not None and p1[0] is not None and t > 0:
            line += ", PSI %s some %.1f%%" % (
                k, (p1[0] - p0[0]) / 1e4 / t)
            if p0[1] is not None and p1[1] is not None:
                line += " full %.1f%%" % ((p1[1] - p0[1]) / 1e4 / t)
    print(line)
    a0 = kstat(os.path.join(s, "arcstats"))
    a1 = kstat(os.path.join(e, "arcstats"))
    if a1:
        g = 1 << 20
        print("  arcstats end: size %d MiB, c %d MiB, c_max %d MiB, "
              "anon %d MiB, mru %d MiB, mfu %d MiB, need_free %d, "
              "avail %d MiB" % (
                  a1.get("size", 0) // g, a1.get("c", 0) // g,
                  a1.get("c_max", 0) // g, a1.get("anon_size", 0) // g,
                  a1.get("mru_size", 0) // g, a1.get("mfu_size", 0) // g,
                  a1.get("arc_need_free", 0),
                  a1.get("memory_available_bytes", 0) // g))
        keys = ("hits", "misses", "demand_data_misses", "evict_skip",
                "evict_not_enough", "memory_throttle_count",
                "memory_direct_count", "memory_indirect_count",
                "mutex_miss", "access_skip", "deleted",
                "evict_l2_ineligible")
        print("  arcstats delta: " + ", ".join(
            "%s %d" % (k, a1.get(k, 0) - a0.get(k, 0)) for k in keys
            if k in a1))
    d0 = kstat(os.path.join(s, "dmu_tx"))
    d1 = kstat(os.path.join(e, "dmu_tx"))
    if d1:
        print("  dmu_tx delta: " + ", ".join(
            "%s %d" % (k[7:], d1[k] - d0.get(k, 0)) for k in sorted(d1)
            if d1[k] - d0.get(k, 0)))
    u0 = kstat(os.path.join(s, "dbufstats"))
    u1 = kstat(os.path.join(e, "dbufstats"))
    if u1:
        print("  dbufstats: cache_size %d MiB (target %d), "
              "total_evicts delta %d, hash_misses delta %d" % (
                  u1.get("cache_size_bytes", 0) >> 20,
                  u1.get("cache_target_bytes", 0) >> 20,
                  u1.get("cache_total_evicts", 0) -
                  u0.get("cache_total_evicts", 0),
                  u1.get("hash_misses", 0) - u0.get("hash_misses", 0)))
    for pool in ("fastq", "slowq"):
        h0 = hist_kstat(os.path.join(s, pool + ".dmu_tx_assign"))
        h1 = hist_kstat(os.path.join(e, pool + ".dmu_tx_assign"))
        hd = dict((b, h1.get(b, 0) - h0.get(b, 0)) for b in h1)
        x0 = txgs(os.path.join(s, pool + ".txgs"))
        x1 = txgs(os.path.join(e, pool + ".txgs"))
        new = [x1[k] for k in sorted(x1) if k not in x0 and x1[k][2] == "C"]
        line = "  %s: dmu_tx_assign n=%d p50<=%sns p99<=%sns max<=%sns" % (
            pool, sum(hd.values()), hist_pct(hd, 0.5), hist_pct(hd, 0.99),
            max([b for b in hd if hd[b]] or [0]))
        if new:
            nd = [int(r[3]) for r in new]
            st = [int(r[11]) for r in new]
            line += "; txgs %d, ndirty mean %d MiB max %d MiB, sync " \
                "mean %.0f ms max %.0f ms" % (
                    len(new), statistics.mean(nd) / 1048576,
                    max(nd) / 1048576, statistics.mean(st) / 1e6,
                    max(st) / 1e6)
        print(line)


def cmd_q2(out):
    irate = idle_rate(out)
    print("== Q2: fast mirror (null_blk) next to slow raidz2 ==")
    print(readf(os.path.join(out, "setup.txt"), "").strip())
    for f in ("slow-slowwr.json", "slow-slowrd.json"):
        r = load_fio(os.path.join(out, f))
        if r:
            print("slow load %s: %.0f MiB/s (%s), mean %.1f ms per 1M "
                  "I/O" % (f[5:11], r["bw_mib"], r["dir"],
                           r["mean"] / 1000.0))
    table = {}
    print("\n%-22s %-9s %9s %8s %8s %8s %9s %7s %7s %7s %7s" % (
        "workload", "phase", "IOPS", "mean_us", "p50_us", "p99_us",
        "p99.9_us", "dIOPS%", "dp50%", "dp99%", "dp999%"))
    for wl, wname in Q2_WL:
        for ph in PHASES:
            runs = [load_run(out, "q2.%s.%s.r%d" % (ph, wl, rep), irate)
                    for rep in range(1, 10)]
            runs = [r for r in runs if r]
            if not runs:
                continue
            m = dict((k, median([r.get(k) for r in runs]))
                     for k in ("iops", "mean", "p50", "p99", "p999",
                               "cpu_us_io", "busy_pct", "miss_per_io"))
            m["spread"] = (min(r["iops"] for r in runs),
                           max(r["iops"] for r in runs))
            table[(wl, ph)] = m
            b = table.get((wl, "alone"))
            print("%-22s %-9s %9.0f %8.1f %8s %8s %9s %7s %7s %7s %7s" % (
                wname, ph, m["iops"], m["mean"], fmt(m["p50"]),
                fmt(m["p99"]), fmt(m["p999"]),
                fmt(pct_change(m["iops"], b and b["iops"])),
                fmt(pct_change(m["p50"], b and b["p50"])),
                fmt(pct_change(m["p99"], b and b["p99"])),
                fmt(pct_change(m["p999"], b and b["p999"]))))
    print("\nper-run context (median): CPU busy %, CPU us/IO, ARC data "
          "misses/IO, IOPS min..max")
    for (wl, ph), m in sorted(table.items()):
        print("  %-4s %-9s busy %5s%%  cpu/IO %6s  miss/IO %5s  "
              "iops %.0f..%.0f" % (wl, ph, fmt(m["busy_pct"]),
                                   fmt(m["cpu_us_io"], 2),
                                   fmt(m["miss_per_io"], 2),
                                   m["spread"][0], m["spread"][1]))
    for ph in PHASES:
        print("\n== phase %s ==" % ph)
        phase_report(out, ph)
        for wl in ("rr1", "sw1"):
            da = os.path.join(out, "diag", "q2diagA.%s.%s" % (ph, wl))
            db = os.path.join(out, "diag", "q2diagB.%s.%s" % (ph, wl))
            ra = load_run(out, "q2diagA.%s.%s" % (ph, wl), irate)
            el = elapsed_ms(os.path.join(da, "offcpu.txt"))
            ops = ra["iops"] * el / 1000.0 if ra and el else None
            print("  [%s %s] diag run A: %s" % (
                ph, wl, "iops %.0f mean %.1fus p99 %sus" % (
                    ra["iops"], ra["mean"], fmt(ra["p99"])) if ra
                else "no fio result"))
            offcpu_summary(os.path.join(da, "offcpu.txt"), ops)
            procstack_summary(os.path.join(da, "procstack.json"))
            rb = load_run(out, "q2diagB.%s.%s" % (ph, wl), irate)
            print("  [%s %s] diag run B: %s" % (
                ph, wl, "iops %.0f mean %.1fus p99 %sus" % (
                    rb["iops"], rb["mean"], fmt(rb["p99"])) if rb
                else "no fio result"))
            funclat_summary(os.path.join(db, "funclat.txt"))
            perf_summary(os.path.join(db, "perf"), "perf -a", top=12,
                         children=False, comm=True)
            io = readf(os.path.join(db, "zpool-iostat.txt"))
            if io:
                print("    zpool iostat -l:")
                for line in io.strip().splitlines():
                    if line.strip():
                        print("      " + line)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: report.py fioline|sample|q1|q2|qz ARGS")
    cmd = sys.argv[1]
    if cmd == "fioline":
        cmd_fioline(sys.argv[2])
    elif cmd == "sample":
        cmd_sample(*sys.argv[2:6])
    elif cmd == "q1":
        cmd_q1(sys.argv[2])
    elif cmd == "q2":
        cmd_q2(sys.argv[2])
    elif cmd == "qz":
        cmd_qz(sys.argv[2])
    else:
        sys.exit("unknown command " + cmd)


if __name__ == "__main__":
    main()
