#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1118-VQ1 sampler, read-only, run as root.

  vq1-sample.py SECONDS INTERVAL OUT DEV...

Every INTERVAL seconds for SECONDS seconds it writes to OUT:
  "T <t> <wall>"                             one tick, wall = Unix time
  "I <t> <dev> <rd> <wr> <wbt> <normal> <background> <enabled>"
      requests in flight in DEV's queue (/sys/block/DEV/inflight), the
      writes blk-wbt tracks and its current limits (debugfs rqos/wbt; "-"
      when absent)
  "S <t> <dev> <reads> <rsect> <writes> <wsect> <io_ticks_ms>"
      DEV's cumulative counters (/sys/block/DEV/stat)
  "P <t> <some_us> <full_us>"                /proc/pressure/io totals
  "C <t> <busy> <iowait> <total>"            /proc/stat cpu jiffies
  "D <t> <comm> <wbt> <frames>"
      a thread in uninterruptible sleep, whether its kernel stack holds
      wbt_wait or rq_qos_wait (1) or not (0), and its first five
      meaningful frames joined by "<"
t is in seconds since the start.  TASK-1118-WBT's sampler, plus wall time,
device counters, PSI and CPU per tick.
"""

import os
import sys
import time

SKIP = ("__schedule", "schedule", "io_schedule", "__cv_", "cv_",
        "__mutex_lock", "mutex_lock", "rwsem_down", "down_read",
        "down_write", "__down", "wait_for_completion", "bit_wait",
        "__wait_on_bit", "out_of_line_wait_on_bit", "rt_mutex",
        "__rt_mutex", "rwbase_")


def readline(path):
    try:
        with open(path) as f:
            return f.readline().strip()
    except OSError:
        return None


def wbt_state(dev):
    d = "/sys/kernel/debug/block/%s/rqos/wbt" % dev
    tracked = 0
    try:
        with open(d + "/inflight") as f:
            for line in f:
                tracked += int(line.split()[-1])
    except (OSError, ValueError, IndexError):
        return "- - - -"
    return "%d %s %s %s" % (tracked, readline(d + "/wb_normal") or "-",
                            readline(d + "/wb_background") or "-",
                            readline(d + "/enabled") or "-")


def devstat(dev):
    st = readline("/sys/block/%s/stat" % dev)
    if st is None:
        return "- - - - -"
    p = st.split()
    if len(p) < 10:
        return "- - - - -"
    return "%s %s %s %s %s" % (p[0], p[2], p[4], p[6], p[9])


def psi():
    some = full = "-"
    try:
        with open("/proc/pressure/io") as f:
            for line in f:
                p = line.split()
                tot = [x.split("=", 1)[1] for x in p if x.startswith("total=")]
                if p and tot and p[0] == "some":
                    some = tot[0]
                elif p and tot and p[0] == "full":
                    full = tot[0]
    except OSError:
        pass
    return "%s %s" % (some, full)


def cpu():
    st = readline("/proc/stat")
    if st is None or not st.startswith("cpu "):
        return "- - -"
    v = [int(x) for x in st.split()[1:9]]
    tot = sum(v)
    return "%d %d %d" % (tot - v[3] - v[4], v[4], tot)


def blocked(t, out):
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            base = "/proc/%s/task/%s/" % (pid, tid)
            st = readline(base + "stat")
            if st is None:
                continue
            rp = st.rfind(")")
            lp = st.find("(")
            if lp < 0 or rp < lp or st[rp + 2:rp + 3] != "D":
                continue
            comm = st[lp + 1:rp].replace(" ", "_")
            wbt = 0
            frames = []
            try:
                with open(base + "stack") as f:
                    for line in f:
                        p = line.split()
                        if len(p) < 2:
                            continue
                        fn = p[1].split("+")[0]
                        if fn in ("wbt_wait", "rq_qos_wait"):
                            wbt = 1
                        if fn.startswith(SKIP):
                            continue
                        if len(frames) < 5:
                            frames.append(fn)
            except OSError:
                pass
            out.write("D %.3f %s %d %s\n" % (t, comm, wbt,
                                             "<".join(frames) or "-"))


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        sys.exit(2)
    secs, ivl, path, devs = float(argv[1]), float(argv[2]), argv[3], argv[4:]
    t0 = time.monotonic()
    nxt = t0
    with open(path, "w") as out:
        while True:
            now = time.monotonic()
            if now - t0 >= secs:
                break
            t = now - t0
            out.write("T %.3f %.3f\n" % (t, time.time()))
            for dev in devs:
                inf = readline("/sys/block/%s/inflight" % dev) or "- -"
                out.write("I %.3f %s %s %s\n" % (t, dev, " ".join(
                    inf.split()), wbt_state(dev)))
                out.write("S %.3f %s %s\n" % (t, dev, devstat(dev)))
            out.write("P %.3f %s\n" % (t, psi()))
            out.write("C %.3f %s\n" % (t, cpu()))
            blocked(t, out)
            nxt += ivl
            delay = nxt - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.monotonic()


if __name__ == "__main__":
    main(sys.argv)
