#!/usr/bin/env python3
"""k35-sum.py <run>.k35 ...

Summarises the files k35-reduce.py wrote.  The run label is the file name
without .k35 and any directory; the arm is the label up to its last '-'
(M-1 and M-2 are arm M).  Every wake-up of order > 0 gets:

- a class, from the innermost SPL frame of its stack and the kernel
  frames inside it:
    K   kmalloc ← spl_kvmalloc (the physical attempt)
    VF  vmalloc ← spl_kvmalloc (the vmalloc fallback)
    VD  vmalloc ← spl_kmem_alloc_impl, no spl_kvmalloc (KM_VMEM above
        spl_kmem_alloc_max)
    K:f / V:f / spl:f  other SPL paths, f = innermost SPL frame
    other:f  no SPL frame, f = first frame outside the allocator
- a request size bucket, <=32K, 32-128K or >128K:
    K: from the order, which is get_order(size) for kmalloc_large and at
       most 3 for slab-backed kmalloc;
    V*: from the last vmap area the task reserved before the wake-up
       (guard page excluded), if at most 50 ms before and consistent with
       the order (order <= ilog2(pages)); '?' otherwise;
    others: n/a;
- a caller: the first frame outside SPL above the SPL frames (for K/V/
  spl classes), plus the task name with trailing digits dropped.

Prints markdown tables: per run and per arm (sum and mean per run) of
wake-ups by order, by class x bucket, the order >= 4 and order-8 subsets,
the callers, one example stack per arm and class, the vmap areas by
bucket, and the E diagnostics.
"""
import os
import re
import sys
from collections import Counter, OrderedDict

BUCKETS = ('<=32K', '32-128K', '>128K', '?', 'n/a')
ALLOC = re.compile(r'alloc|kswapd|slab|freelist|rmqueue|vmalloc|vm_area|'
                   r'folio|kmalloc|kmem_cache|compact|reclaim')


def bucket(nbytes):
    if nbytes <= 32768:
        return '<=32K'
    if nbytes <= 131072:
        return '32-128K'
    return '>128K'


def split(frame):
    i = frame.find('[')
    if i < 0:
        return frame, ''
    return frame[:i], frame[i + 1:-1]


def classify(frames):
    parts = [split(f) for f in frames]
    names = [p[0] for p in parts]
    isspl = [p[1] == 'spl' or p[0].startswith('spl_') for p in parts]
    s = next((i for i, x in enumerate(isspl) if x), None)
    if s is None:
        c = next((n for n in names if not ALLOC.search(n)), '?')
        return 'other:' + c, c
    j = s
    while j < len(names) and isspl[j]:
        j += 1
    kern = names[:s]
    spl = names[s:j]
    caller = names[j] if j < len(names) else '?'
    if any('vmalloc' in n or 'vm_area_alloc' in n for n in kern):
        if 'spl_kvmalloc' in spl:
            cls = 'VF'
        elif 'spl_kmem_alloc_impl' in spl:
            cls = 'VD'
        else:
            cls = 'V:' + spl[0]
    elif any('kmalloc' in n for n in kern):
        cls = 'K' if 'spl_kvmalloc' in spl else 'K:' + spl[0]
    else:
        cls = 'spl:' + spl[0]
    return cls, caller


def size_bucket(cls, order, vm, age):
    if cls == 'K' or cls.startswith('K:'):
        if order <= 3:
            return '<=32K', ''
        return ('32-128K' if order <= 5 else '>128K'), ''
    if cls in ('VF', 'VD') or cls.startswith('V:'):
        if vm == '-' or age == '-':
            return '?', 'no vmap area before it'
        if float(age) > 50000:
            return '?', 'vmap area older than 50 ms'
        req = int(vm) - 4096
        pages = (req + 4095) // 4096
        if pages < 1 or order > pages.bit_length() - 1:
            return '?', 'order above ilog2(pages) of the vmap area'
        return bucket(req), ''
    return 'n/a', ''


def task(comm):
    return re.sub(r'[_/:]?\d+$', '', comm)


def load(path):
    runs = []
    vm = Counter()
    err = Counter()
    with open(path) as f:
        for line in f:
            t = line.rstrip('\n').split('\t')
            if t[0] == 'W' and len(t) >= 9:
                frames = t[8].split('<') if t[8] else []
                cls, caller = classify(frames)
                b, why = size_bucket(cls, int(t[4]), t[6], t[7])
                runs.append({'order': int(t[4]), 'gfp': t[5], 'cls': cls,
                             'bucket': b, 'why': why, 'caller': caller,
                             'task': task(t[3]), 'frames': frames})
            elif t[0] == 'V' and len(t) == 3:
                vm[t[1]] += int(t[2])
            elif t[0] == 'E' and len(t) == 3:
                err[t[1]] += int(t[2])
    return runs, vm, err


def table(title, rows, cols, data):
    print('\n**%s**\n' % title)
    print('| | ' + ' | '.join(cols) + ' |')
    print('|---' * (len(cols) + 1) + '|')
    for r in rows:
        print('| %s | ' % r + ' | '.join(str(data.get((r, c), 0))
                                          for c in cols) + ' |')


def counts(ws, key):
    return Counter(key(w) for w in ws)


def main():
    files = sys.argv[1:]
    if not files:
        print(__doc__)
        sys.exit(1)
    runs = OrderedDict()
    for p in files:
        label = os.path.basename(p)
        if label.endswith('.k35'):
            label = label[:-4]
        runs[label] = load(p)
    arms = OrderedDict()
    for label in runs:
        arms.setdefault(label.rsplit('-', 1)[0], []).append(label)

    def per(selector, keyf):
        cols = list(runs) + ['%s (sum/mean)' % a for a in arms]
        keys = set()
        data = {}
        for label, (ws, _, _) in runs.items():
            c = counts([w for w in ws if selector(w)], keyf)
            for k, n in c.items():
                keys.add(k)
                data[(k, label)] = n
        for a, labels in arms.items():
            for k in keys:
                s = sum(data.get((k, lb), 0) for lb in labels)
                data[(k, '%s (sum/mean)' % a)] = '%d / %.1f' % (
                    s, s / len(labels))
        return sorted(keys, key=str), cols, data

    allw = (lambda w: True)
    ge4 = (lambda w: w['order'] >= 4)
    o8 = (lambda w: w['order'] == 8)
    r, c, d = per(allw, lambda w: 'order %d' % w['order'])
    table('Wake-ups of order > 0, by order', r, c, d)
    for title, sel in (('all orders > 0', allw), ('order >= 4', ge4),
                       ('order 8', o8)):
        r, c, d = per(sel, lambda w: '%s, %s' % (w['cls'], w['bucket']))
        table('Wake-ups by class and request size, %s' % title, r, c, d)
    r, c, d = per(ge4, lambda w: 'bucket %s' % w['bucket'])
    table('Wake-ups of order >= 4 by request size only', r, c, d)
    r, c, d = per(ge4, lambda w: '%s ← %s (%s)' % (
        w['cls'], w['caller'], w['task']))
    table('Wake-ups of order >= 4 by class, caller and task', r, c, d)
    r, c, d = per(lambda w: w['bucket'] == '?', lambda w: '%s: %s' % (
        w['cls'], w['why']))
    table('Wake-ups whose size could not be found', r, c, d)

    print('\n**vmap areas by requested size, and diagnostics**\n')
    keys = ['vmap ' + b for b in BUCKETS[:3]]
    errk = sorted({k for _, (_, _, e) in runs.items() for k in e})
    print('| | ' + ' | '.join(keys + errk) + ' |')
    print('|---' * (len(keys) + len(errk) + 1) + '|')
    for label, (_, vm, err) in runs.items():
        print('| %s | ' % label + ' | '.join(
            [str(vm.get(b, 0)) for b in BUCKETS[:3]] +
            [str(err.get(k, 0)) for k in errk]) + ' |')

    print('\n**One stack per arm and class (order >= 4 first)**\n')
    for a, labels in arms.items():
        seen = set()
        ws = [w for lb in labels for w in runs[lb][0]]
        ws.sort(key=lambda w: -w['order'])
        for w in ws:
            if w['cls'] in seen:
                continue
            seen.add(w['cls'])
            print('- %s, %s, order %d, %s, gfp %s:\n  `%s`' % (
                a, w['cls'], w['order'], w['bucket'], w['gfp'],
                ' ← '.join(w['frames'])))


if __name__ == '__main__':
    main()
