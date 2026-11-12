# SPDX-License-Identifier: CDDL-1.0
# Bench only, never for upstream.
# Summarize fl4bench CSV output (any number of files, any builds):
#   summarize.py file.csv...
import statistics
import sys
from collections import Counter, defaultdict

R = defaultdict(list)   # (layout, order, impl, build, size) -> [GB/s]
S = defaultdict(list)   # (order, impl, build) -> [B/s]
W = defaultdict(Counter)  # (build, order) -> Counter(winner)
builds, impls, sizes = [], [], []


def seen(lst, v):
    if v not in lst:
        lst.append(v)


for path in sys.argv[1:]:
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if p[0] == 'R' and len(p) == 8:
                _, b, _, impl, order, layout, size, gbs = p
                seen(builds, b)
                seen(impls, impl)
                seen(sizes, int(size))
                R[(layout, order, impl, b, int(size))].append(float(gbs))
            elif p[0] == 'S' and len(p) == 6:
                _, b, _, impl, order, bw = p
                seen(builds, b)
                seen(impls, impl)
                S[(order, impl, b)].append(int(bw) / 1e9)
            elif p[0] == 'W' and len(p) == 5:
                _, b, _, nat, bs = p
                W[(b, 'native')][nat] += 1
                W[(b, 'byteswap')][bs] += 1

sizes.sort()


def fmt(v):
    if not v:
        return '-'
    return '%.2f (%.2f-%.2f)' % (statistics.median(v), min(v), max(v))


def hsize(s):
    return '%dM' % (s >> 20) if s >= 1 << 20 else '%dK' % (s >> 10)


for layout in ('linear', 'scatter'):
    for order in ('native', 'byteswap'):
        n = max((len(v) for k, v in R.items()
                 if k[0] == layout and k[1] == order), default=0)
        if n == 0:
            continue
        print('\n%s, %s ABD, GB/s, median (min-max) over %d rounds\n' %
              (order, layout, n))
        print('| implementation | build | ' +
              ' | '.join(hsize(s) for s in sizes) + ' |')
        print('|---|---|' + '---|' * len(sizes))
        for impl in impls:
            for b in builds:
                cells = [fmt(R[(layout, order, impl, b, s)]) for s in sizes]
                if all(c == '-' for c in cells):
                    continue
                print('| %s | %s | %s |' % (impl, b, ' | '.join(cells)))

if len(builds) == 2:
    a, b = builds
    print('\nratio %s / %s of the medians, linear ABD\n' % (b, a))
    print('| implementation | order | ' +
          ' | '.join(hsize(s) for s in sizes) + ' |')
    print('|---|---|' + '---|' * len(sizes))
    for impl in impls:
        for order in ('native', 'byteswap'):
            cells = []
            for s in sizes:
                x = R[('linear', order, impl, a, s)]
                y = R[('linear', order, impl, b, s)]
                cells.append('%.2f' % (statistics.median(y) /
                                       statistics.median(x))
                             if x and y else '-')
            print('| %s | %s | %s |' % (impl, order, ' | '.join(cells)))

if S:
    print('\nkernel-style selection (128 KiB, 1 ms per implementation), '
          'GB/s, median (min-max)\n')
    print('| implementation | order | ' + ' | '.join(builds) + ' |')
    print('|---|---|' + '---|' * len(builds))
    for impl in impls:
        for order in ('native', 'byteswap'):
            print('| %s | %s | %s |' % (impl, order, ' | '.join(
                fmt(S[(order, impl, b)]) for b in builds)))
    print('\nwinner counts\n')
    for b in builds:
        for order in ('native', 'byteswap'):
            c = W[(b, order)]
            print('- %s %s: %s' % (b, order, ', '.join(
                '%s %d/%d' % (k, v, sum(c.values()))
                for k, v in c.most_common())))
