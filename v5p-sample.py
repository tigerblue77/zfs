#!/usr/bin/env python3
# TASK-1117-V5P: sample the write and flush taskqs 10 times a second.
# Usage: v5p-sample.py SECONDS > out.txt
# Each line: t name.instance threads [active idle] max queued [normal high]
import re
import sys
import time

SUMMARY = "/proc/spl/kstat/taskq/summary"
KEEP = ("z_wr_", "z_flush", "z_zvol")
LINE = re.compile(r"^(\S+)\s*\|\s*(\d+)\s*\[\s*(\d+)\s+(\d+)\]\s*(\d+)\s*\|"
                  r"\s*(\d+)\s*\[\s*(\d+)\s+(\d+)\]\s*(\d+)")

end = time.monotonic() + float(sys.argv[1])
out = sys.stdout
while True:
    now = time.monotonic()
    if now >= end:
        break
    try:
        with open(SUMMARY) as f:
            text = f.read()
    except OSError:
        text = ""
    t = "%.2f" % time.time()
    for line in text.splitlines():
        if not line.startswith(KEEP):
            continue
        m = LINE.match(line)
        if m:
            out.write(t + " " + " ".join(m.groups()) + "\n")
    out.flush()
    time.sleep(max(0.0, 0.1 - (time.monotonic() - now)))
