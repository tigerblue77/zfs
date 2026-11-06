#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
#
# Reporting side of tests/perf-zvol-v2/run.sh.
#
#   report.py tsnap OUT_JSON   snapshot of every thread's schedstat
#   report.py line RUN_DIR     one line for a finished row
#   report.py summary OUT      tables over every row of OUT/runs

import collections
import json
import os
import re
import statistics
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")


def readf(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def tsnap(out):
    threads = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        tdir = "/proc/%s/task" % pid
        try:
            tids = os.listdir(tdir)
        except OSError:
            continue
        for tid in tids:
            base = "%s/%s" % (tdir, tid)
            comm = readf(base + "/comm")
            ss = readf(base + "/schedstat")
            st = readf(base + "/status")
            if comm is None or ss is None or st is None:
                continue
            ss = ss.split()
            vol = nvol = 0
            for line in st.splitlines():
                if line.startswith("voluntary_ctxt_switches"):
                    vol = int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches"):
                    nvol = int(line.split()[1])
            threads[tid] = [comm.strip(), int(ss[0]), int(ss[1]),
                            int(ss[2]), vol, nvol]
    with open(out, "w") as f:
        json.dump({"t": time.monotonic(), "threads": threads}, f)


def group(comm):
    for prefix, g in (("iou-wrk", "iou-wrk"), ("iou-sqp", "iou-wrk"),
                      ("zvol_tq", "zvol_tq"), ("z_wr_iss", "z_wr_iss"),
                      ("z_wr_int", "z_wr_int"), ("z_rd_int", "z_rd_int"),
                      ("z_", "z_other"), ("txg_", "z_other"),
                      ("kworker", "kworker"), ("ksoftirqd", "kworker")):
        if comm.startswith(prefix):
            return g
    if comm == "fio":
        return "fio"
    return "other"


def threads_delta(d):
    pre = json.loads(readf(d + "/pre.threads.json") or "null")
    post = json.loads(readf(d + "/post.threads.json") or "null")
    if not pre or not post:
        return None
    dt = post["t"] - pre["t"]
    g = collections.defaultdict(lambda: [0, 0, 0, 0, 0])
    fio_threads = []
    for tid, v in post["threads"].items():
        p = pre["threads"].get(tid)
        if p is not None and p[0] != v[0]:
            p = None
        base = p if p is not None else [v[0], 0, 0, 0, 0, 0]
        run, wait = v[1] - base[1], v[2] - base[2]
        vol, nvol = v[4] - base[4], v[5] - base[5]
        k = group(v[0])
        a = g[k]
        a[0] += run
        a[1] += wait
        a[2] += vol
        a[3] += nvol
        a[4] += 1 if run > 0 else 0
        if k == "fio" and run > 0:
            fio_threads.append((run, wait, vol, nvol))
    res = {"dt": dt}
    for k, a in g.items():
        res[k] = {"cpu": 100.0 * a[0] / 1e9 / dt,
                  "wait": 100.0 * a[1] / 1e9 / dt,
                  "vcsw": a[2] / dt, "nvcsw": a[3] / dt, "n": a[4]}
    # the busiest fio thread: the job of a one-job row
    if fio_threads:
        run, wait, vol, nvol = max(fio_threads)
        res["fio1"] = {"cpu": 100.0 * run / 1e9 / dt,
                       "wait": 100.0 * wait / 1e9 / dt,
                       "vcsw": vol / dt, "nvcsw": nvol / dt}
    return res


def load_row(d):
    if (readf(d + "/rc") or "1").strip() != "0":
        return None
    txt = readf(d + "/fio.json")
    if not txt or "{" not in txt:
        return None
    try:
        data = json.loads(txt[txt.index("{"):])
    except ValueError:
        return None
    job = data["jobs"][0]
    k = "write" if job["write"].get("total_ios") else "read"
    x = job[k]
    pre = (readf(d + "/pre.stat") or "").split()
    post = (readf(d + "/post.stat") or "").split()
    r = {"iops": x["iops"], "lat": x["lat_ns"]["mean"] / 1000.0,
         "p99": x["clat_ns"]["percentile"].get("99.000000", 0) / 1000.0,
         "ios": x["total_ios"]}
    if len(pre) == 3 and len(post) == 3 and x["iops"]:
        ios = x["iops"] * (float(post[2]) - float(pre[2]))
        r["cpu_io"] = (int(post[0]) - int(pre[0])) / HZ * 1e6 / ios
        r["ctx_io"] = (int(post[1]) - int(pre[1])) / ios
    r["thr"] = threads_delta(d)
    pst = readf(d + "/perf-stat.txt")
    if pst:
        for line in pst.splitlines():
            f = line.split(",")
            if len(f) > 3 and f[0].strip().isdigit():
                r["ev:" + f[2].split(":")[-1]] = int(f[0])
    return r


def fmt_thr(t):
    if not t:
        return ""
    out = []
    for k in ("fio", "iou-wrk", "zvol_tq", "z_wr_iss", "z_rd_int"):
        if k in t:
            out.append("%s=%.0f%%" % (k, t[k]["cpu"]))
    if "fio1" in t:
        f = t["fio1"]
        out.append("fio1 run=%.0f%% rq=%.0f%% vcs/s=%.0f nvcs/s=%.0f" %
                   (f["cpu"], f["wait"], f["vcsw"], f["nvcsw"]))
    return " ".join(out)


def line(d):
    r = load_row(d)
    if r is None:
        print("  %s: failed" % os.path.basename(d))
        return
    print("  %-40s iops=%8.0f lat=%7.1f p99=%7.0f cpu/io=%5.1f ctx/io=%5.2f"
          " %s" % (os.path.basename(d), r["iops"], r["lat"], r["p99"],
                   r.get("cpu_io", 0), r.get("ctx_io", 0),
                   fmt_thr(r["thr"])))


def pct(a, b):
    if a is None or not b:
        return None
    return 100.0 * (a - b) / b


def summary(out):
    runs = os.path.join(out, "runs")
    rows = collections.defaultdict(lambda: collections.defaultdict(list))
    diag = {}
    for name in sorted(os.listdir(runs)):
        m = re.match(r"([^.]+)\.(.+)\.r(\d+)$", name)
        r = load_row(os.path.join(runs, name))
        if m:
            if r is not None:
                rows[m.group(2)][m.group(1)].append(r)
            continue
        m = re.match(r"([^.]+)\.(diag-.+)$", name)
        if m and r is not None:
            diag[(m.group(2), m.group(1))] = (r, os.path.join(runs, name))
    variants = [v.split("=")[0] for v in
                (readf(os.path.join(out, "variants.txt")) or "").split()]
    mqv = [v.split("=")[0] for v in
           (readf(os.path.join(out, "mq-variants.txt")) or "").split()]

    def med(lst, key):
        vals = [x[key] for x in lst if x.get(key) is not None]
        return statistics.median(vals) if vals else None

    def table(vs, base, title):
        print("== %s: IOPS (median of runs) and change vs %s; spread is "
              "(max-min)/median of the runs" % (title, base))
        print("%-26s %s" % ("row", " ".join("%17s" % v for v in vs)))
        for rname in sorted(rows):
            have = [v for v in vs if rows[rname].get(v)]
            if not have:
                continue
            b = med(rows[rname].get(base, []), "iops")
            cells = []
            for v in vs:
                lst = rows[rname].get(v, [])
                i = med(lst, "iops")
                if i is None:
                    cells.append("%17s" % "-")
                    continue
                vals = [x["iops"] for x in lst]
                sp = 100.0 * (max(vals) - min(vals)) / i if i else 0
                c = pct(i, b)
                if v == base or c is None:
                    cells.append("%8.0f   (s%3.0f%%)" % (i, sp))
                else:
                    cells.append("%7.0f %+5.1f%% s%2.0f" % (i, c, sp))
            print("%-26s %s" % (rname, " ".join(cells)))
        print()
        print("%-26s %s" % ("lat us / cpu us/IO / ctx/IO",
                            " ".join("%17s" % v for v in vs)))
        for rname in sorted(rows):
            if not any(rows[rname].get(v) for v in vs):
                continue
            cells = []
            for v in vs:
                lst = rows[rname].get(v, [])
                if not lst:
                    cells.append("%17s" % "-")
                    continue
                cells.append("%5.0f/%5.1f/%5.2f" % (
                    med(lst, "lat"), med(lst, "cpu_io") or 0,
                    med(lst, "ctx_io") or 0))
            print("%-26s %s" % (rname, " ".join(cells)))
        print()
        print("thread CPU % (fio/iou-wrk/zvol_tq/z_wr_iss), first run, and "
              "the fio job thread's run%/runqueue-wait%")
        for rname in sorted(rows):
            if not any(rows[rname].get(v) for v in vs):
                continue
            print(rname)
            for v in vs:
                lst = rows[rname].get(v, [])
                if lst and lst[0]["thr"]:
                    print("   %-6s %s" % (v, fmt_thr(lst[0]["thr"])))
        print()

    if variants:
        table(variants, variants[0], "main rows")
    if mqv:
        table(mqv, mqv[0], "blk-mq rows")
    if diag:
        print("== diagnostic rows: io_uring punts per I/O (perf stat over 3 s"
              " of the run) and where fio's CPU time goes")
        for (rname, v), (r, d) in sorted(diag.items()):
            ios_s = r["iops"]
            punts = r.get("ev:io_uring_queue_async_work")
            subs = r.get("ev:io_uring_submit_req")
            sw = r.get("ev:sched_switch")
            print("%-24s %-5s iops=%7.0f punts/s=%s submits/s=%s "
                  "switches/IO=%s" % (
                      rname, v, ios_s,
                      "%.0f" % (punts / 3.0) if punts is not None else "-",
                      "%.0f" % (subs / 3.0) if subs is not None else "-",
                      "%.2f" % (sw / 3.0 / ios_s) if sw and ios_s else "-"))
            print("    " + fmt_thr(r["thr"]))
            comm = readf(os.path.join(d, "perf.comm.txt")) or ""
            top = [x for x in comm.splitlines()
                   if x.strip() and not x.startswith("#")][:8]
            if top:
                print("    perf by comm: " + "; ".join(
                    " ".join(x.split()) for x in top))
            ch = readf(os.path.join(d, "perf.fio.children.txt")) or ""
            keep = []
            for x in ch.splitlines():
                if not x.strip() or x.startswith("#"):
                    continue
                if re.search(r"zvol_|taskq_|try_to_wake_up|wake_up|"
                             r"submit_bio|blkdev_|__blkdev|io_issue_sqe|"
                             r"io_submit_sqes|io_read|io_write|io_wq_enq|"
                             r"io_queue_iowq|kmalloc|kmem_|spl_|bio_alloc|"
                             r"bio_iov|pin_user|bio_endio|io_cqring_wait|"
                             r"schedule$|__schedule|rw_|down_read|up_read|"
                             r"_raw_spin|native_send_call|"
                             r"default_send_IPI|__x2apic_send|"
                             r"hv_send_ipi|do_syscall|entry_SYSCALL|"
                             r"io_uring_enter|__do_sys_io_uring_enter|"
                             r"file_update_time|blk_finish_plug|"
                             r"io_req_rw_complete|tctx_task_work|"
                             r"__io_run_local_work|io_run_local_work", x):
                    keep.append("      " + " ".join(x.split()))
            if keep:
                print("    fio children%:")
                print("\n".join(keep[:40]))
        print()


def main():
    cmd = sys.argv[1]
    if cmd == "tsnap":
        tsnap(sys.argv[2])
    elif cmd == "line":
        line(sys.argv[2])
    elif cmd == "summary":
        summary(sys.argv[2])
    else:
        sys.exit("unknown command " + cmd)


if __name__ == "__main__":
    main()
