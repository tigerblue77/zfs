#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1120-WBP4 load generator and sampler.

  wbp4-load.py frag DIR N BS
      N files of BS bytes in 64 subdirectories of DIR.
  wbp4-load.py unfrag DIR
      deletes every file of DIR's subdirectories with an odd number.
  wbp4-load.py syncw FILE SECONDS OUT RATE BS
      pwrites of BS bytes at random BS-aligned offsets of FILE, each
      followed by an fdatasync(), paced at RATE per second, until SECONDS
      have passed or SIGTERM.  OUT: "<unix time> <kind> <bytes>
      <latency_ns>", kind W or F, then "END <writes> <seconds>".
  wbp4-load.py churn DIR SECONDS OUT RATE BS KEEP
      creates files of BS bytes in DIR/d, RATE per second, and once more
      than KEEP exist deletes a random one after each creation, so that the
      freed blocks are scattered.  OUT: one line per second, "<unix time>
      C <created> D <deleted>", then "END <created> <deleted> <seconds>".
  wbp4-load.py sample OUT STOPFILE INTERVAL
      every INTERVAL seconds, until STOPFILE exists (at most 900 s), the
      state of every z_trim_* thread: "S <unix time> <comm> <tid> <state>
      <wbt>", wbt 1 when its kernel stack holds wbt_wait or rq_qos_wait.
  wbp4-load.py sched TAG
      prints, for every z_trim_* thread, "K <TAG> <unix time> <comm> <tid>
      <sum_block_runtime ms> <sum_sleep_runtime ms>" ("-" when schedstats
      are off).

Pacing: operation i is due at start + i / RATE; a late operation starts at
once, and a stream more than 0.5 s late starts its schedule again from now.
"""

import os
import random
import re
import signal
import sys
import time

STOP = False


def on_term(signum, frame):
    global STOP
    STOP = True


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


def frag(d, n, bs):
    buf = bytearray(os.urandom(bs))
    for i in range(n):
        sub = os.path.join(d, "%02d" % (i % 64))
        if i < 64:
            os.makedirs(sub, exist_ok=True)
        buf[0:8] = i.to_bytes(8, "little")
        with open(os.path.join(sub, "f%05d" % i), "wb") as f:
            f.write(buf)


def unfrag(d):
    n = 0
    for sub in sorted(os.listdir(d)):
        p = os.path.join(d, sub)
        if not os.path.isdir(p):
            continue
        for name in os.listdir(p):
            if name.startswith("f") and int(name[1:]) % 2 == 1:
                os.unlink(os.path.join(p, name))
                n += 1
    print("deleted %d" % n)


def syncw(path, secs, out, rate, bs):
    signal.signal(signal.SIGTERM, on_term)
    fd = os.open(path, os.O_WRONLY)
    nblk = os.fstat(fd).st_size // bs
    if nblk < 1:
        sys.exit("syncw: file smaller than %d" % bs)
    buf = bytearray(os.urandom(bs))
    rnd = random.Random(1120)
    pace = Pacer(rate)
    t0 = time.monotonic()
    n = 0
    with open(out, "w") as log:
        while not STOP and time.monotonic() - t0 < secs:
            pace.wait()
            buf[0:8] = n.to_bytes(8, "little")
            off = rnd.randrange(nblk) * bs
            w = time.time()
            a = time.monotonic_ns()
            os.pwrite(fd, buf, off)
            b = time.monotonic_ns()
            os.fdatasync(fd)
            c = time.monotonic_ns()
            log.write("%.6f W %d %d\n%.6f F 0 %d\n" % (w, bs, b - a,
                                                      w + (b - a) / 1e9,
                                                      c - b))
            n += 1
        log.write("END %d %.3f\n" % (n, time.monotonic() - t0))
    os.close(fd)


def churn(d, secs, out, rate, bs, keep):
    signal.signal(signal.SIGTERM, on_term)
    sub = os.path.join(d, "d")
    os.makedirs(sub, exist_ok=True)
    buf = bytearray(os.urandom(bs))
    rnd = random.Random(1122)
    live = []
    pace = Pacer(rate)
    t0 = time.monotonic()
    n = deleted = 0
    c1 = d1 = 0
    nxt = time.time() + 1
    with open(out, "w") as log:
        while not STOP and time.monotonic() - t0 < secs:
            pace.wait()
            name = os.path.join(sub, "c%07d" % n)
            buf[0:8] = n.to_bytes(8, "little")
            with open(name, "wb") as f:
                f.write(buf)
            live.append(name)
            n += 1
            c1 += 1
            if len(live) > keep:
                k = rnd.randrange(len(live))
                live[k], live[-1] = live[-1], live[k]
                os.unlink(live.pop())
                deleted += 1
                d1 += 1
            now = time.time()
            if now >= nxt:
                log.write("%.3f C %d D %d\n" % (now, c1, d1))
                c1 = d1 = 0
                nxt = now + 1
        log.write("END %d %d %.3f\n" % (n, deleted, time.monotonic() - t0))


def readline(path):
    try:
        with open(path) as f:
            return f.readline().strip()
    except OSError:
        return None


def trim_threads():
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            comm = readline("/proc/%s/task/%s/comm" % (pid, tid))
            if comm and comm.startswith("z_trim"):
                out.append((pid, tid, comm))
    return out


def sample(out, stopfile, ivl):
    t0 = time.monotonic()
    ths = trim_threads()
    rescan = t0 + 1
    nxt = t0
    with open(out, "w") as log:
        while not os.path.exists(stopfile) and time.monotonic() - t0 < 900:
            now = time.monotonic()
            if now >= rescan:
                ths = trim_threads()
                rescan = now + 1
            w = time.time()
            for pid, tid, comm in ths:
                base = "/proc/%s/task/%s/" % (pid, tid)
                st = readline(base + "stat")
                if st is None:
                    continue
                rp = st.rfind(")")
                state = st[rp + 2:rp + 3] if rp > 0 else "?"
                wbt = 0
                if state == "D":
                    try:
                        with open(base + "stack") as f:
                            for line in f:
                                if "wbt_wait" in line or "rq_qos_wait" in line:
                                    wbt = 1
                                    break
                    except OSError:
                        pass
                log.write("S %.3f %s %s %s %d\n" % (w, comm, tid, state, wbt))
            nxt += ivl
            delay = nxt - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.monotonic()


SCHED_RE = re.compile(r"(sum_block_runtime|sum_sleep_runtime)\s*:\s*([0-9.]+)")


def sched(tag):
    w = time.time()
    for pid, tid, comm in trim_threads():
        v = {}
        try:
            with open("/proc/%s/task/%s/sched" % (pid, tid)) as f:
                for line in f:
                    m = SCHED_RE.search(line)
                    if m:
                        v[m.group(1)] = m.group(2)
        except OSError:
            pass
        print("K %s %.3f %s %s %s %s" % (tag, w, comm, tid,
                                         v.get("sum_block_runtime", "-"),
                                         v.get("sum_sleep_runtime", "-")))


def main(argv):
    try:
        mode = argv[1]
        if mode == "frag" and len(argv) == 5:
            frag(argv[2], int(argv[3]), int(argv[4]))
        elif mode == "unfrag" and len(argv) == 3:
            unfrag(argv[2])
        elif mode == "syncw" and len(argv) == 7:
            syncw(argv[2], float(argv[3]), argv[4], float(argv[5]),
                  int(argv[6]))
        elif mode == "churn" and len(argv) == 8:
            churn(argv[2], float(argv[3]), argv[4], float(argv[5]),
                  int(argv[6]), int(argv[7]))
        elif mode == "sample" and len(argv) == 5:
            sample(argv[2], argv[3], float(argv[4]))
        elif mode == "sched" and len(argv) == 3:
            sched(argv[2])
        else:
            raise IndexError
    except IndexError:
        sys.stderr.write(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
