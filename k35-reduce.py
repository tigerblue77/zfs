#!/usr/bin/env python3
"""k35-reduce.py <trace_pipe> <out>

Reads the k35 tracing instance (k35-size.sh) while a run goes on and keeps
only what k35-sum.py needs, tab-separated:

  W ts pid comm order gfp vmap_bytes vmap_age_us frames
      one line per wake-up of a sleeping kswapd of order > 0; vmap_bytes
      is the size of the last vmap area (alloc_vmap_area, guard page
      included) the same task reserved before it, vmap_age_us how long
      before ('-' for both if none); frames is the kernel stack, innermost
      first, joined by '<', without the tracing frames and offsets.
  V bucket count
      the vmap areas seen, by requested size (guard page excluded).
  E name count
      lost events, lines not understood, stacks without their event.

Stops on SIGTERM or SIGINT, or at the end of a regular file.
"""
import re
import signal
import sys

HDR = re.compile(r'^\s*(.+)-(\d+)\s+\[(\d+)\]\s+(?:\S+\s+)?(\d+\.\d+): (.*)$')
WAKE = re.compile(r'mm_vmscan_wakeup_kswapd: nid=(\d+) order=(\d+) '
                  r'gfp_flags=(\S+)')
VMAP = re.compile(r'alloc_vmap_area: .*\bsize=(\d+) .*\bfailed=(-?\d+)')
LOST = re.compile(r'LOST (\d+) EVENTS')
OFFS = re.compile(r'\+0x[0-9a-f]+/0x[0-9a-f]+')
MOD = re.compile(r'\s+\[(\S+)\]$')


def bucket(nbytes):
    if nbytes <= 32768:
        return '<=32K'
    if nbytes <= 131072:
        return '32-128K'
    return '>128K'


def clean(frame):
    frame = OFFS.sub('', frame.strip())
    return MOD.sub(r'[\1]', frame)


class Stop(Exception):
    pass


def on_signal(signum, frame):
    raise Stop()


def main():
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    last = {}
    pend = {}
    cur = None
    vcount = {'<=32K': 0, '32-128K': 0, '>128K': 0}
    err = {'lost_events': 0, 'unparsed': 0, 'stack_without_event': 0,
           'event_without_stack': 0, 'vmap_failed': 0, 'other_event': 0}
    out = open(sys.argv[2], 'w')

    def emit(r):
        frames = r['frames']
        while frames and (frames[0].startswith('trace_event_') or
                          frames[0].startswith('__traceiter_') or
                          frames[0].startswith('event_triggers_')):
            frames.pop(0)
        out.write('W\t%s\t%d\t%s\t%d\t%s\t%s\t%s\t%s\n' % (
            r['ts'], r['pid'], r['comm'], r['order'], r['gfp'],
            '-' if r['vm'] is None else r['vm'],
            '-' if r['age'] is None else '%.0f' % r['age'],
            '<'.join(frames[:40])))

    try:
        with open(sys.argv[1], 'r', errors='replace') as f:
            for line in f:
                if line.startswith(' => '):
                    if cur is not None:
                        cur['frames'].append(clean(line[4:]))
                    continue
                if cur is not None:
                    emit(cur)
                    cur = None
                m = HDR.match(line)
                if not m:
                    lm = LOST.search(line)
                    if lm:
                        err['lost_events'] += int(lm.group(1))
                    elif line.strip():
                        err['unparsed'] += 1
                    continue
                comm, pid, _cpu, ts, rest = m.groups()
                pid = int(pid)
                if rest.startswith('<stack trace>'):
                    cur = pend.pop(pid, None)
                    if cur is None:
                        err['stack_without_event'] += 1
                    continue
                w = WAKE.search(rest)
                if w:
                    if pid in pend:
                        err['event_without_stack'] += 1
                        emit(pend.pop(pid))
                    v = last.get(pid)
                    pend[pid] = {
                        'ts': ts, 'pid': pid, 'comm': comm.strip(),
                        'order': int(w.group(2)), 'gfp': w.group(3),
                        'vm': v[1] if v else None,
                        'age': (float(ts) - v[0]) * 1e6 if v else None,
                        'frames': []}
                    continue
                v = VMAP.search(rest)
                if v:
                    size = int(v.group(1))
                    if int(v.group(2)) != 0:
                        err['vmap_failed'] += 1
                    last[pid] = (float(ts), size)
                    vcount[bucket(size - 4096)] += 1
                    continue
                err['other_event'] += 1
    except Stop:
        pass
    finally:
        if cur is not None:
            emit(cur)
        for r in pend.values():
            err['event_without_stack'] += 1
            emit(r)
        for b in ('<=32K', '32-128K', '>128K'):
            out.write('V\t%s\t%d\n' % (b, vcount[b]))
        for k in sorted(err):
            out.write('E\t%s\t%d\n' % (k, err[k]))
        out.close()


if __name__ == '__main__':
    main()
