#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""
Bench only, not for upstream (issue #16503).

Summarise the CSV lines printed by bench: for each ABD type, one row per
algorithm and implementation, one column per size, each cell the median
GB/s over the rounds with the spread (min-max) after it.  With --best,
only the fastest implementation of each algorithm at each size (by
median) is printed, with its name.
"""
import statistics
import sys


def size_name(s):
	return '%dM' % (s >> 20) if s >= 1 << 20 else '%dK' % (s >> 10)


def main(argv):
	best = '--best' in argv
	paths = [a for a in argv[1:] if not a.startswith('--')]
	data = {}
	order = []
	for path in paths:
		for ln in open(path):
			f = ln.strip().split(',')
			if len(f) != 7 or f[0] != 'R':
				continue
			key = (f[2], f[3])
			if key not in order:
				order.append(key)
			data.setdefault((f[4], key, int(f[5])), []).append(
			    float(f[6]))
	sizes = sorted({k[2] for k in data})
	for abd in ('linear', 'scatter'):
		print('\n%s ABD, GB/s, median (min-max)\n' % abd)
		print('| %s | %s |' % ('algorithm' if best else 'implementation',
		    ' | '.join(size_name(s) for s in sizes)))
		print('|---|' + '---|' * len(sizes))
		if best:
			algs = []
			for a, _ in order:
				if a not in algs:
					algs.append(a)
			for a in algs:
				cells = []
				for s in sizes:
					cand = [(statistics.median(data[(abd, k, s)]),
					    k) for k in order if k[0] == a and
					    (abd, k, s) in data]
					m, k = max(cand)
					v = data[(abd, k, s)]
					cells.append('%.2f (%.2f-%.2f) %s' % (m,
					    min(v), max(v), k[1]))
				print('| %s | %s |' % (a, ' | '.join(cells)))
			continue
		for k in order:
			cells = []
			for s in sizes:
				v = data.get((abd, k, s))
				if not v:
					cells.append('-')
					continue
				cells.append('%.2f (%.2f-%.2f)' % (
				    statistics.median(v), min(v), max(v)))
			print('| %s-%s | %s |' % (k[0], k[1], ' | '.join(cells)))
	n = {len(v) for v in data.values()}
	print('\nrounds per cell: %s' % ','.join(str(x) for x in sorted(n)))


if __name__ == '__main__':
	main(sys.argv)
