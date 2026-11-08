#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""
Summarise repeated "CRYPTO_TEST_NS=1 crypto_test -p <alg>" runs: for each
implementation and size, the median throughput over the runs in MB/s
(10^6 bytes per second) and the spread (min-max).  Bench only.
"""
import re
import statistics
import sys

SIZES = [1 << i for i in range(10, 20)]
rows = {}
for ln in open(sys.argv[1]):
	m = re.match(r'^(AES-\w+ \[[^\]]+\])\s+([\d\s]+)$', ln.rstrip())
	if not m:
		continue
	ns = [int(x) for x in m.group(2).split()]
	if len(ns) != len(SIZES):
		continue
	rows.setdefault(m.group(1), []).append(ns)

hdr = '| implementation | runs | ' + ' | '.join(
    '%dK' % (s >> 10) for s in SIZES) + ' |'
print(hdr)
print('|' + '---|' * (len(SIZES) + 2))
for name, runs in rows.items():
	cells = []
	for i, s in enumerate(SIZES):
		mbs = sorted(s * 1000.0 / r[i] for r in runs if r[i] > 0)
		cells.append('%.0f (%.0f-%.0f)' % (statistics.median(mbs),
		    mbs[0], mbs[-1]))
	print('| %s | %d | %s |' % (name, len(runs), ' | '.join(cells)))
