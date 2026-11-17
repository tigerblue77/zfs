#!/usr/bin/env python3
# TASK-1117-V5P: one snapshot of what a row needs, as JSON on stdout.
# Usage: v5p-snap.py DISK TASKDIR POOL
#   DISK     the /proc/diskstats name of the VM disk (e.g. sdb)
#   TASKDIR  /var/tmp/task-1117-v5p (holds "loops" and vdev/)
#   POOL     the pool the row measures (its txg history is kept)
import glob
import json
import os
import sys
import time

disk, taskdir, pool = sys.argv[1], sys.argv[2], sys.argv[3]
snap = {"t": time.time(),
        "mono_raw_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)}


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


for line in read("/proc/stat").splitlines():
    if line.startswith("cpu "):
        snap["cpu"] = [int(x) for x in line.split()[1:]]
        break
snap["load1"] = float(read("/proc/loadavg").split()[0])

# Devices: the VM disk and every device of the pools, by pool.
devs = {disk: "DISK"}
for line in read(os.path.join(taskdir, "loops")).splitlines():
    p = line.split()
    if len(p) == 2:
        devs[os.path.basename(p[1])] = p[0]
snap["dev"] = {}
for line in read("/proc/diskstats").splitlines():
    p = line.split()
    if len(p) >= 20 and p[2] in devs:
        snap["dev"][p[2]] = {"pool": devs[p[2]], "writes": int(p[7]),
                             "sectors_written": int(p[9]),
                             "flushes": int(p[18]), "flush_ms": int(p[19])}

# Backing files: bytes the guest file system holds for them.
snap["alloc"] = {}
for f in glob.glob(os.path.join(taskdir, "vdev", "*")):
    try:
        snap["alloc"][os.path.basename(f)] = os.stat(f).st_blocks * 512
    except OSError:
        pass

# Journal commits of the guest file systems (ext4), if any.
snap["jbd2"] = {}
for f in glob.glob("/proc/fs/jbd2/*/info"):
    first = read(f).split()
    if first and first[0].isdigit():
        snap["jbd2"][f.split("/")[4]] = int(first[0])

# CPU and run-queue wait of thread classes, by name.
GROUPS = [("z_wr_iss_h", "z_wr_iss_h"), ("z_wr_iss", "z_wr_iss"),
          ("z_wr_int", "z_wr_int"), ("z_flush", "z_flush"),
          ("z_zvol", "z_zvol"), ("txg_sync", "txg_sync"),
          ("dp_sync_taskq", "dp_sync_taskq"), ("fio", "fio")]
cls = {}
for pid in os.listdir("/proc"):
    if not pid.isdigit():
        continue
    s = read("/proc/%s/stat" % pid)
    if not s:
        continue
    comm = s[s.index("(") + 1:s.rindex(")")]
    g = None
    for prefix, name in GROUPS:
        if comm.startswith(prefix):
            g = name
            break
    if g is None:
        if comm.startswith("kworker") and "loop" in comm:
            g = "loop_kworker"
        elif comm.startswith("loop"):
            g = "loop"
        else:
            continue
    rest = s[s.rindex(")") + 2:].split()
    c = cls.setdefault(g, {"ticks": 0, "run_ns": 0, "wait_ns": 0,
                           "slices": 0, "count": 0})
    c["ticks"] += int(rest[11]) + int(rest[12])
    c["count"] += 1
    ss = read("/proc/%s/schedstat" % pid).split()
    if len(ss) == 3:
        c["run_ns"] += int(ss[0])
        c["wait_ns"] += int(ss[1])
        c["slices"] += int(ss[2])
snap["threads"] = cls
snap["hz"] = os.sysconf("SC_CLK_TCK")


def named(path):
    """A named kstat as {name: value}."""
    out = {}
    for line in read(path).splitlines()[2:]:
        p = line.split()
        if len(p) == 3 and p[2].lstrip("-").isdigit():
            out[p[0]] = int(p[2])
    return out


# Taskq counters of the write and flush taskqs, by kstat name.
TQKEYS = ("threads_total", "threads_max", "tasks_dispatched",
          "tasks_executed_normal", "tasks_executed_priority",
          "threads_created", "threads_destroyed", "thread_wakeups",
          "thread_sleeps")
snap["taskq"] = {}
for f in glob.glob("/proc/spl/kstat/taskq/*"):
    b = os.path.basename(f)
    if b.startswith(("z_wr_", "z_flush", "z_zvol")):
        k = named(f)
        snap["taskq"][b] = {x: k[x] for x in TQKEYS if x in k}

snap["zil"] = named("/proc/spl/kstat/zfs/zil")

# The txg history of the pool: txg birth state ndirty nread nwritten reads
# writes otime qtime wtime stime (times in ns).
snap["txgs"] = []
for line in read("/proc/spl/kstat/zfs/%s/txgs" % pool).splitlines()[1:]:
    p = line.split()
    if len(p) == 12 and p[0].isdigit():
        snap["txgs"].append([int(p[0]), int(p[1]), p[2]] +
                            [int(x) for x in p[3:]])

json.dump(snap, sys.stdout)
print()
