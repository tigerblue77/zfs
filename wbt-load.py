#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1118-WBT load generator: one process per stream, paced, logging the
latency of every operation.

  wbt-load.py writer DEV SECONDS OUT [RATE BS EVERY]
      O_DIRECT writes of BS bytes (default 524288) at random BS-aligned
      offsets of DEV, RATE per second (default 100), and an fdatasync()
      after every EVERY writes (default 150): a guest writing to a zvol
      and flushing about every 1.5 s.
  wbt-load.py reader DEV SECONDS OUT [RATE]
      O_DIRECT reads of 32K, 64K, 128K or 256K (weights 1:1:2:6) at random
      aligned offsets of DEV, RATE per second (default 10).
  wbt-load.py bulk FILE SECONDS OUT [MBPS SIZE_MB]
      buffered sequential 1 MiB writes to FILE, MBPS MiB/s (default 60),
      wrapping at SIZE_MB (default 256), never synced: transaction group
      writes only.

Pacing: operation i is due at start + i / RATE; an operation that is late
starts at once, and when a stream is more than 0.5 s late its schedule
starts again from now, so a stall does not turn into a burst.

OUT gets one line per operation: "<t> <kind> <bytes> <latency_ns>", t in
seconds since the start (float), kind W, F (fdatasync) or R; then a last
line "END <ops> <bytes> <seconds>".
"""

import mmap
import os
import random
import sys
import time

READ_SIZES = [(32768, 1), (65536, 1), (131072, 2), (262144, 6)]


def usage():
    sys.stderr.write(__doc__)
    sys.exit(2)


def devsize(fd):
    return os.lseek(fd, 0, os.SEEK_END)


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


def run_writer(dev, secs, out, rate=100.0, bs=524288, every=150):
    fd = os.open(dev, os.O_WRONLY | os.O_DIRECT)
    nblk = devsize(fd) // bs
    if nblk < 1:
        sys.exit("writer: %s smaller than %d" % (dev, bs))
    buf = mmap.mmap(-1, bs)
    buf.write(os.urandom(bs))
    rnd = random.Random(1118)
    pace = Pacer(rate)
    t0 = time.monotonic()
    end = t0 + secs
    n = ops = nbytes = 0
    with open(out, "w") as log:
        while time.monotonic() < end:
            pace.wait()
            off = rnd.randrange(nblk) * bs
            # Change the first bytes, so that no two writes are alike.
            buf[0:8] = n.to_bytes(8, "little")
            a = time.monotonic_ns()
            os.pwrite(fd, buf, off)
            b = time.monotonic_ns()
            log.write("%.6f W %d %d\n" % (a / 1e9 - t0, bs, b - a))
            n += 1
            ops += 1
            nbytes += bs
            if n % every == 0:
                a = time.monotonic_ns()
                os.fdatasync(fd)
                b = time.monotonic_ns()
                log.write("%.6f F 0 %d\n" % (a / 1e9 - t0, b - a))
        log.write("END %d %d %.3f\n" % (ops, nbytes, time.monotonic() - t0))
    os.close(fd)


def run_reader(dev, secs, out, rate=10.0):
    fd = os.open(dev, os.O_RDONLY | os.O_DIRECT)
    size = devsize(fd)
    buf = mmap.mmap(-1, max(s for s, _ in READ_SIZES))
    sizes = [s for s, w in READ_SIZES for _ in range(w)]
    rnd = random.Random(1119)
    pace = Pacer(rate)
    t0 = time.monotonic()
    end = t0 + secs
    ops = nbytes = 0
    with open(out, "w") as log:
        while time.monotonic() < end:
            pace.wait()
            bs = rnd.choice(sizes)
            off = rnd.randrange(size // bs) * bs
            view = memoryview(buf)[:bs]
            a = time.monotonic_ns()
            got = os.preadv(fd, [view], off)
            b = time.monotonic_ns()
            view.release()
            if got != bs:
                sys.exit("reader: short read %d at %d" % (got, off))
            log.write("%.6f R %d %d\n" % (a / 1e9 - t0, bs, b - a))
            ops += 1
            nbytes += bs
        log.write("END %d %d %.3f\n" % (ops, nbytes, time.monotonic() - t0))
    os.close(fd)


def run_bulk(path, secs, out, mbps=60.0, size_mb=256):
    bs = 1 << 20
    fd = os.open(path, os.O_WRONLY | os.O_CREAT, 0o600)
    buf = bytearray(os.urandom(bs))
    pace = Pacer(mbps)
    t0 = time.monotonic()
    end = t0 + secs
    ops = nbytes = 0
    with open(out, "w") as log:
        while time.monotonic() < end:
            pace.wait()
            off = (ops % size_mb) * bs
            buf[0:8] = ops.to_bytes(8, "little")
            a = time.monotonic_ns()
            os.pwrite(fd, buf, off)
            b = time.monotonic_ns()
            log.write("%.6f W %d %d\n" % (a / 1e9 - t0, bs, b - a))
            ops += 1
            nbytes += bs
        log.write("END %d %d %.3f\n" % (ops, nbytes, time.monotonic() - t0))
    os.close(fd)


def main(argv):
    if len(argv) < 5:
        usage()
    mode, target, secs, out = argv[1], argv[2], float(argv[3]), argv[4]
    extra = argv[5:]
    if mode == "writer":
        rate = float(extra[0]) if len(extra) > 0 else 100.0
        bs = int(extra[1]) if len(extra) > 1 else 524288
        every = int(extra[2]) if len(extra) > 2 else 150
        run_writer(target, secs, out, rate, bs, every)
    elif mode == "reader":
        rate = float(extra[0]) if len(extra) > 0 else 10.0
        run_reader(target, secs, out, rate)
    elif mode == "bulk":
        mbps = float(extra[0]) if len(extra) > 0 else 60.0
        size_mb = int(extra[1]) if len(extra) > 1 else 256
        run_bulk(target, secs, out, mbps, size_mb)
    else:
        usage()


if __name__ == "__main__":
    main(sys.argv)
