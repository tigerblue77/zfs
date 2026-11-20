#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1120-WBP2 load generator: one process per stream, every operation's
latency logged.  Each thread names itself NAME (its comm), so that the block
layer's trace shows which stream inserted a request.

  wbp2-load.py reader FILE SECONDS OUT NAME RATE BS THREADS
      buffered preads of BS bytes at random BS-aligned offsets of FILE.
      RATE > 0: paced at RATE per second by one thread (THREADS must be 1);
      RATE = 0: closed loop, THREADS threads each with one read in flight.
  wbp2-load.py writer FILE SECONDS OUT NAME RATE BS EVERY
      pwrites of BS bytes at random BS-aligned offsets of FILE, paced at
      RATE per second, and an fdatasync() after every EVERY writes (EVERY 0:
      never).

Pacing: operation i is due at start + i / RATE; a late operation starts at
once, and a stream more than 0.5 s late starts its schedule again from now,
so that a stall does not turn into a burst.

OUT gets one line per operation, "<t> <kind> <bytes> <latency_ns>", t in
seconds since the start, kind R, W or F (fdatasync), then a last line
"END <ops> <bytes> <seconds>".
"""

import os
import random
import sys
import threading
import time


def name_thread(name):
    try:
        with open("/proc/self/task/%d/comm" % threading.get_native_id(),
                  "w") as f:
            f.write(name[:15])
    except (OSError, AttributeError):
        pass


class Pacer:
    def __init__(self, rate):
        self.period = 1.0 / rate
        self.base = time.monotonic()
        self.i = 0

    def wait(self):
        due = self.base + self.i * self.period
        now = time.monotonic()
        if now < due:
            time.sleep(due - now)
        elif now - due > 0.5:
            self.base = now
            self.i = 0
        self.i += 1


def blocks(fd, bs):
    n = os.fstat(fd).st_size // bs
    if n < 1:
        sys.exit("file smaller than %d bytes" % bs)
    return n


def read_loop(fd, nblk, bs, t0, end, rate, seed, name, out):
    name_thread(name)
    rnd = random.Random(seed)
    pace = Pacer(rate) if rate > 0 else None
    while time.monotonic() < end:
        if pace:
            pace.wait()
        off = rnd.randrange(nblk) * bs
        a = time.monotonic_ns()
        got = len(os.pread(fd, bs, off))
        b = time.monotonic_ns()
        if got != bs:
            sys.exit("reader: short read %d at %d" % (got, off))
        out.append((a / 1e9 - t0, "R", bs, b - a))


def run_reader(path, secs, outp, name, rate, bs, threads):
    if rate > 0 and threads != 1:
        sys.exit("reader: a paced reader has one thread")
    name_thread(name)
    fd = os.open(path, os.O_RDONLY)
    nblk = blocks(fd, bs)
    t0 = time.monotonic()
    end = t0 + secs
    logs = [[] for _ in range(threads)]
    ths = [threading.Thread(target=read_loop,
                            args=(fd, nblk, bs, t0, end, rate, 1120 + i,
                                  name, logs[i]))
           for i in range(threads)]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    ops = [op for log in logs for op in log]
    ops.sort()
    with open(outp, "w") as f:
        for op in ops:
            f.write("%.6f %s %d %d\n" % op)
        f.write("END %d %d %.3f\n" % (len(ops), len(ops) * bs,
                                      time.monotonic() - t0))
    os.close(fd)
    if len(ops) == 0:
        sys.exit("reader: no read completed")


def run_writer(path, secs, outp, name, rate, bs, every):
    name_thread(name)
    fd = os.open(path, os.O_WRONLY)
    nblk = blocks(fd, bs)
    buf = bytearray(os.urandom(bs))
    rnd = random.Random(1121)
    pace = Pacer(rate)
    t0 = time.monotonic()
    end = t0 + secs
    n = 0
    with open(outp, "w") as log:
        while time.monotonic() < end:
            pace.wait()
            off = rnd.randrange(nblk) * bs
            # No two writes alike.
            buf[0:8] = n.to_bytes(8, "little")
            a = time.monotonic_ns()
            os.pwrite(fd, buf, off)
            b = time.monotonic_ns()
            log.write("%.6f W %d %d\n" % (a / 1e9 - t0, bs, b - a))
            n += 1
            if every > 0 and n % every == 0:
                a = time.monotonic_ns()
                os.fdatasync(fd)
                b = time.monotonic_ns()
                log.write("%.6f F 0 %d\n" % (a / 1e9 - t0, b - a))
        log.write("END %d %d %.3f\n" % (n, n * bs, time.monotonic() - t0))
    os.close(fd)


def main(argv):
    if len(argv) != 9 or argv[1] not in ("reader", "writer"):
        sys.stderr.write(__doc__)
        sys.exit(2)
    mode, path, secs, outp, name = argv[1], argv[2], float(argv[3]), \
        argv[4], argv[5]
    rate, bs, last = float(argv[6]), int(argv[7]), int(argv[8])
    if mode == "reader":
        run_reader(path, secs, outp, name, rate, bs, last)
    else:
        if rate <= 0:
            sys.exit("writer: RATE must be above 0")
        run_writer(path, secs, outp, name, rate, bs, last)


if __name__ == "__main__":
    main(sys.argv)
