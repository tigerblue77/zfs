#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Host-rig load generator: a QEMU guest on a zvol, as the host's Plex VM.

  hr-load.py guest DEV SECONDS OUT [key=value ...]
      The guest's I/O on zvol DEV for SECONDS, every operation logged to
      OUT as "<t> <kind> <bytes> <latency_ns>" (t in seconds from the
      start), then "END <kind>:<ops>:<bytes>:<skipped> ...".  Streams, all
      open loop with bounded concurrency (a stream more than 2 s behind
      its schedule skips ahead and counts what it skipped):
        W  media writes: media_bs bytes, sequential through the write
           ring, starting media_shift bytes into a 1 MiB block, so that
           they straddle the zvol's blocks as a guest file system's
           extents do (about 1.5 ZIL records per write, as on the host);
           media_mbps MB/s, media_qd in flight; the ring ahead of the
           writer is discarded every discard_mb (a guest deleting old
           media with discard on), so new blocks need no read first
        A  small writes, never synced (guest writeback of metadata and
           database pages): small_rate per second, random in the db area
        D, S  database transactions: db_tps per second, each db_writes
           small writes then fdatasync() (S is the fdatasync)
        J, K  journal commits: every journal_s seconds a 16 KiB write to
           the journal area, then fdatasync() (K)
        R  media reads: read_rate per second, read_qd in flight, inside
           one 1 MiB block of the library area
        X  discards of the ring ahead of the writer
      QEMU with cache=none submits the guest's writes with O_DIRECT and
      turns a guest flush into fdatasync() on the zvol; so does this.
  hr-load.py fill DIR NFILES FILE_MB FIRST
      buffered writes of NFILES files fNNNNNN (from number FIRST) of
      FILE_MB MiB each, in 1 MiB writes, never synced.
  hr-load.py zvolfill DEV OFF_MB LEN_MB
      O_DIRECT sequential 1 MiB writes over [OFF_MB, OFF_MB + LEN_MB).
  hr-load.py probe DEV SECONDS
      QD1 random 4 KiB O_DIRECT reads, then sequential 1 MiB O_DIRECT
      writes, then one fdatasync(), on a raw device: prints the latency
      of each kind (the emulated disk's service times, before any pool).

Every page written repeats an 8-byte value: the emulated disks keep such
pages in 8 bytes, so the pool must have compression=off.

Area layout on the zvol, in MiB: library [0, lib_mb), ring
[lib_mb, + ring_mb), db [.., + db_mb), journal [.., + journal_mb).
"""

import fcntl
import math
import mmap
import os
import random
import struct
import sys
import threading
import time

MIB = 1 << 20
BLKDISCARD = 0x1277

DEFAULTS = {
    "media_mbps": 50.0, "media_bs": 524288, "media_qd": 4,
    "media_shift": 262144, "discard_mb": 64,
    "small_rate": 6.0, "small_sizes": "4096:4,16384:3,32768:2,65536:1",
    "db_tps": 0.4, "db_writes": 3,
    "journal_s": 5.0,
    "read_rate": 7.0, "read_qd": 4,
    "read_sizes": "131072:1,262144:2,524288:2,1048576:3",
    "lib_mb": 8192, "ring_mb": 4096, "db_mb": 256, "journal_mb": 64,
    "seed": 1121,
}


def usage():
    sys.stderr.write(__doc__)
    sys.exit(2)


def pattern(nbytes, base):
    """nbytes of pages that each repeat one 8-byte value (never 0)."""
    return b"".join(struct.pack("<Q", (base + i) | (1 << 63)) * 512
                    for i in range(nbytes // 4096))


class Buffers:
    """Page-aligned patterned buffers for O_DIRECT, distinct values."""

    def __init__(self, nbytes, count, base):
        self.n = nbytes
        self.bufs = []
        for k in range(count):
            m = mmap.mmap(-1, nbytes)
            m.write(pattern(nbytes, base + k * (nbytes // 4096)))
            self.bufs.append(m)
        self.i = 0
        self.lock = threading.Lock()

    def get(self, nbytes):
        with self.lock:
            m = self.bufs[self.i % len(self.bufs)]
            self.i += 1
        return memoryview(m)[:nbytes]


def weighted(spec):
    out = []
    for part in spec.split(","):
        size, w = part.split(":")
        out += [int(size)] * int(w)
    return out


class Sched:
    """Open-loop schedule shared by a stream's threads: next() hands out
    the next due time; when the stream is more than 2 s late it skips
    ahead to now and counts the operations it dropped."""

    def __init__(self, rate, rnd, poisson):
        self.rate = rate
        self.rnd = rnd
        self.poisson = poisson
        self.t = time.monotonic()
        self.lock = threading.Lock()
        self.skipped = 0

    def next(self):
        with self.lock:
            if self.poisson:
                gap = -math.log(1.0 - self.rnd.random()) / self.rate
            else:
                gap = 1.0 / self.rate
            self.t += gap
            now = time.monotonic()
            if now - self.t > 2.0:
                self.skipped += int((now - self.t) * self.rate)
                self.t = now
            return self.t


class Guest:
    def __init__(self, dev, secs, prm):
        self.dev = dev
        self.secs = secs
        self.p = prm
        self.log = []
        self.err = []
        self.t0 = time.monotonic()
        self.end = self.t0 + secs
        self.rnd = random.Random(int(prm["seed"]))
        self.rlock = threading.Lock()
        mb = MIB
        self.lib = (0, int(prm["lib_mb"]) * mb)
        self.ring = (self.lib[1], int(prm["ring_mb"]) * mb)
        self.db = (self.ring[0] + self.ring[1], int(prm["db_mb"]) * mb)
        self.jr = (self.db[0] + self.db[1], int(prm["journal_mb"]) * mb)
        self.size = self.jr[0] + self.jr[1]
        self.wbufs = Buffers(int(prm["media_bs"]), 8, 1 << 40)
        self.sbufs = Buffers(65536, 16, 2 << 40)
        self.rbuf = Buffers(MIB, int(prm["read_qd"]), 3 << 40)
        self.mcur = 0           # ring offset of the next media write
        self.mtot = 0           # bytes the media stream has claimed
        self.mlock = threading.Lock()
        self.disc_upto = 0      # bytes of the ring discarded, in mtot terms
        self.disc_cv = threading.Condition()
        self.scheds = {}
        self.small_sizes = weighted(prm["small_sizes"])
        self.read_sizes = weighted(prm["read_sizes"])

    def rec(self, kind, nbytes, a, b):
        self.log.append((a / 1e9 - self.t0, kind, nbytes, b - a))

    def rand(self):
        with self.rlock:
            return self.rnd.random()

    def openfd(self, write):
        flags = (os.O_RDWR if write else os.O_RDONLY) | os.O_DIRECT
        return os.open(self.dev, flags)

    def wait_until(self, t):
        d = t - time.monotonic()
        if d > 0:
            time.sleep(d)
        return time.monotonic() < self.end

    # W: sequential media writes through the ring
    def media(self, sched):
        fd = self.openfd(True)
        bs = int(self.p["media_bs"])
        shift = int(self.p["media_shift"])
        span = self.ring[1] - shift - bs
        try:
            while True:
                t = sched.next()
                if not self.wait_until(t):
                    break
                with self.mlock:
                    rel = self.mcur
                    self.mcur = (self.mcur + bs) % (span - span % bs)
                    self.mtot += bs
                with self.disc_cv:
                    self.disc_cv.notify()
                off = self.ring[0] + shift + rel
                buf = self.wbufs.get(bs)
                a = time.monotonic_ns()
                os.pwrite(fd, buf, off)
                b = time.monotonic_ns()
                buf.release()
                self.rec("W", bs, a, b)
        finally:
            os.close(fd)

    # X: discard the ring ahead of the writer, discard_mb at a time.  The
    # writer wraps at ring_mb less one block, so the discard position is
    # taken modulo the same length.
    def discard_one(self, fd, wrap, record):
        chunk = int(self.p["discard_mb"]) * MIB
        start = self.disc_upto % wrap
        ln = min(chunk, wrap - start)
        a = time.monotonic_ns()
        fcntl.ioctl(fd, BLKDISCARD, struct.pack(
            "QQ", self.ring[0] + int(self.p["media_shift"]) + start, ln))
        b = time.monotonic_ns()
        if record:
            self.rec("X", ln, a, b)
        self.disc_upto += ln

    def ring_wrap(self):
        bs = int(self.p["media_bs"])
        span = self.ring[1] - int(self.p["media_shift"]) - bs
        return span - span % bs

    def discard_ahead(self):
        # before the clock starts: the first chunks ahead of the writer
        if int(self.p["discard_mb"]) <= 0:
            return
        fd = self.openfd(True)
        try:
            while self.disc_upto < 4 * int(self.p["discard_mb"]) * MIB:
                self.discard_one(fd, self.ring_wrap(), False)
        finally:
            os.close(fd)

    def discarder(self):
        chunk = int(self.p["discard_mb"]) * MIB
        if chunk <= 0:
            return
        fd = self.openfd(True)
        wrap = self.ring_wrap()
        try:
            while time.monotonic() < self.end:
                with self.mlock:
                    need = self.mtot + 4 * chunk
                if self.disc_upto >= need:
                    with self.disc_cv:
                        self.disc_cv.wait(0.5)
                    continue
                self.discard_one(fd, wrap, True)
        finally:
            os.close(fd)

    def small_write(self, fd, area, kind):
        sz = self.small_sizes[int(self.rand() * len(self.small_sizes))]
        off = area[0] + int(self.rand() * ((area[1] - sz) // 4096)) * 4096
        buf = self.sbufs.get(sz)
        a = time.monotonic_ns()
        os.pwrite(fd, buf, off)
        b = time.monotonic_ns()
        buf.release()
        self.rec(kind, sz, a, b)

    # A: small writes, never synced
    def small(self, sched):
        fd = self.openfd(True)
        try:
            while self.wait_until(sched.next()):
                self.small_write(fd, self.db, "A")
        finally:
            os.close(fd)

    # D, S: database transactions
    def dbtx(self, sched):
        fd = self.openfd(True)
        try:
            while self.wait_until(sched.next()):
                t = time.monotonic_ns()
                for _ in range(int(self.p["db_writes"])):
                    self.small_write(fd, self.db, "D")
                a = time.monotonic_ns()
                os.fdatasync(fd)
                b = time.monotonic_ns()
                self.rec("S", 0, a, b)
                self.rec("T", 0, t, b)
        finally:
            os.close(fd)

    # J, K: journal commits
    def journal(self, sched):
        fd = self.openfd(True)
        pos = 0
        try:
            while self.wait_until(sched.next()):
                buf = self.sbufs.get(16384)
                a = time.monotonic_ns()
                os.pwrite(fd, buf, self.jr[0] + pos)
                b = time.monotonic_ns()
                buf.release()
                self.rec("J", 16384, a, b)
                pos = (pos + 16384) % self.jr[1]
                a = time.monotonic_ns()
                os.fdatasync(fd)
                b = time.monotonic_ns()
                self.rec("K", 0, a, b)
        finally:
            os.close(fd)

    # R: reads inside one block of the library
    def reader(self, sched):
        fd = self.openfd(False)
        try:
            while self.wait_until(sched.next()):
                sz = self.read_sizes[int(self.rand() *
                                         len(self.read_sizes))]
                blk = int(self.rand() * (self.lib[1] // MIB))
                off = blk * MIB + int(self.rand() * ((MIB - sz) // 4096 +
                                                     1)) * 4096
                buf = self.rbuf.get(sz)
                a = time.monotonic_ns()
                got = os.preadv(fd, [buf], off)
                b = time.monotonic_ns()
                buf.release()
                if got != sz:
                    raise OSError("short read %d at %d" % (got, off))
                self.rec("R", sz, a, b)
        finally:
            os.close(fd)

    def run(self, out):
        p = self.p
        fd = os.open(self.dev, os.O_RDONLY)
        devsize = os.lseek(fd, 0, os.SEEK_END)
        os.close(fd)
        if devsize < self.size:
            sys.exit("guest: %s is %d bytes, the areas need %d" %
                     (self.dev, devsize, self.size))
        threads = []
        self.discard_ahead()
        self.t0 = time.monotonic()
        self.end = self.t0 + self.secs

        def start(name, fn, *args):
            def body():
                try:
                    fn(*args)
                except Exception as e:  # report, do not hang the run
                    self.err.append("%s: %r" % (name, e))
            th = threading.Thread(target=body, name=name, daemon=True)
            th.start()
            threads.append(th)

        streams = [
            ("W", float(p["media_mbps"]) * 1e6 / int(p["media_bs"]),
             int(p["media_qd"]), self.media, False),
            ("A", float(p["small_rate"]), 2, self.small, True),
            ("D", float(p["db_tps"]), 1, self.dbtx, True),
            ("J", 1.0 / float(p["journal_s"]), 1, self.journal, False),
            ("R", float(p["read_rate"]), int(p["read_qd"]), self.reader,
             True),
        ]
        for kind, rate, qd, fn, poisson in streams:
            if rate <= 0:
                continue
            seed = int(p["seed"]) * 31 + ord(kind)
            s = Sched(rate, random.Random(seed), poisson)
            self.scheds[kind] = s
            for i in range(qd):
                start("%s%d" % (kind, i), fn, s)
        start("X", self.discarder)
        for th in threads:
            th.join(max(0.0, self.end - time.monotonic()) + 120)
        alive = [th.name for th in threads if th.is_alive()]
        stats = {}
        for t, kind, nb, ns in self.log:
            e = stats.setdefault(kind, [0, 0])
            e[0] += 1
            e[1] += nb
        with open(out, "w") as f:
            f.write("# guest %s %.0f s %s\n" % (self.dev, self.secs, " ".join(
                "%s=%s" % (k, p[k]) for k in sorted(p))))
            for t, kind, nb, ns in sorted(self.log):
                f.write("%.6f %s %d %d\n" % (t, kind, nb, ns))
            f.write("END %s\n" % " ".join(
                "%s:%d:%d:%d" % (k, v[0], v[1], self.scheds[k].skipped
                                 if k in self.scheds else 0)
                for k, v in sorted(stats.items())))
            for e in self.err:
                f.write("ERROR %s\n" % e)
            if alive:
                f.write("ERROR threads still running: %s\n" %
                        " ".join(alive))
        if self.err or alive:
            sys.exit("guest: %s" % "; ".join(self.err + alive))


def run_fill(d, nfiles, file_mb, first):
    buf = bytearray(pattern(MIB, 5 << 40))
    for i in range(first, first + nfiles):
        fd = os.open(os.path.join(d, "f%06d" % i),
                     os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            for j in range(file_mb):
                # a distinct first page per write, still one 8-byte value
                buf[0:4096] = struct.pack("<Q", (i << 20 | j) |
                                          (1 << 62)) * 512
                os.write(fd, buf)
        finally:
            os.close(fd)


def run_zvolfill(dev, off_mb, len_mb):
    fd = os.open(dev, os.O_WRONLY | os.O_DIRECT)
    m = mmap.mmap(-1, MIB)
    m.write(pattern(MIB, 6 << 40))
    try:
        for i in range(len_mb):
            os.pwrite(fd, m, (off_mb + i) * MIB)
    finally:
        os.close(fd)


def run_probe(dev, secs):
    fd = os.open(dev, os.O_RDWR | os.O_DIRECT)
    size = os.lseek(fd, 0, os.SEEK_END)
    m = mmap.mmap(-1, MIB)
    m.write(pattern(MIB, 7 << 40))
    rnd = random.Random(7)
    res = {}
    end = time.monotonic() + secs / 2.0
    lat = []
    while time.monotonic() < end:
        off = rnd.randrange(size // 4096) * 4096
        a = time.monotonic_ns()
        os.preadv(fd, [memoryview(m)[:4096]], off)
        lat.append(time.monotonic_ns() - a)
    res["QD1 random 4K read"] = lat
    end = time.monotonic() + secs / 2.0
    lat = []
    off = size // 2
    while time.monotonic() < end and off + MIB <= size:
        a = time.monotonic_ns()
        os.pwrite(fd, m, off)
        lat.append(time.monotonic_ns() - a)
        off += MIB
    res["QD1 sequential 1M write"] = lat
    a = time.monotonic_ns()
    os.fdatasync(fd)
    res["flush after them"] = [time.monotonic_ns() - a]
    os.close(fd)
    for k, v in res.items():
        v.sort()
        print("%s: n %d mean %.1f p50 %.1f p99 %.1f max %.1f ms" % (
            k, len(v), sum(v) / len(v) / 1e6, v[len(v) // 2] / 1e6,
            v[min(len(v) - 1, int(len(v) * 0.99))] / 1e6, v[-1] / 1e6))


def main(argv):
    if len(argv) < 3:
        usage()
    mode = argv[1]
    if mode == "guest" and len(argv) >= 5:
        prm = dict(DEFAULTS)
        for kv in argv[5:]:
            k, _, v = kv.partition("=")
            if k not in prm:
                sys.exit("guest: unknown key %s" % k)
            prm[k] = v
        Guest(argv[2], float(argv[3]), prm).run(argv[4])
    elif mode == "fill" and len(argv) == 6:
        run_fill(argv[2], int(argv[3]), int(argv[4]), int(argv[5]))
    elif mode == "zvolfill" and len(argv) == 5:
        run_zvolfill(argv[2], int(argv[3]), int(argv[4]))
    elif mode == "probe" and len(argv) == 4:
        run_probe(argv[2], float(argv[3]))
    else:
        usage()


if __name__ == "__main__":
    main(sys.argv)
