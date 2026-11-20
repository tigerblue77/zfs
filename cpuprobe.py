#!/usr/bin/env python3
# cpuprobe.py: a fixed amount of CPU work (SHA-256 of 512 MiB of zeros per
# process), timed on one CPU and then on every CPU at once, three times each.
# Prints one line: the median seconds of each, and the steal and load1 seen.
import hashlib
import multiprocessing as mp
import os
import statistics
import time

MIB = 512


def work(_):
    buf = b"\0" * (1 << 20)
    h = hashlib.sha256()
    t = time.perf_counter()
    for _ in range(MIB):
        h.update(buf)
    return time.perf_counter() - t


def cpu_line():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    return v


def main():
    n = os.cpu_count()
    a = cpu_line()
    one = [work(0) for _ in range(3)]
    alls = []
    with mp.Pool(n) as p:
        for _ in range(3):
            t = time.perf_counter()
            p.map(work, range(n))
            alls.append(time.perf_counter() - t)
    b = cpu_line()
    d = [y - x for x, y in zip(a, b)]
    steal = 100.0 * d[7] / max(1, sum(d)) if len(d) > 7 else float("nan")
    with open("/proc/loadavg") as f:
        load1 = f.read().split()[0]
    print("cpuprobe single_s %.3f all_s %.3f ncpu %d steal_pct %.2f "
          "load1 %s single_runs %s all_runs %s" %
          (statistics.median(one), statistics.median(alls), n, steal, load1,
           ",".join("%.3f" % x for x in one),
           ",".join("%.3f" % x for x in alls)))


if __name__ == "__main__":
    main()
