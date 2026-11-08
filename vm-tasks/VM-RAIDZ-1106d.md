# VM: RAID-Z parity math on Broadwell, master against raidz-math-v2 452875f996 (user space)

These instructions are for a test VM of the T630 (Xeon
E5-2696 v4, Broadwell: AVX2, one shuffle port, no AVX-512, no GFNI). They
measure, on this CPU, the two RAID-Z parity commits of the fork branch
`raidz-math-v2`, at a head not measured here before:

- `raidz: multiply by a constant with one table lookup per nibble`
- `RAIDZ: keep parity in registers across data columns`

Everything is user space: no kernel module is loaded, no pool is created, and
no `zpool`, `zfs`, `zdb`, `ztest` or `zfs-tests.sh` command is run. Two release
builds of libzpool are compared: upstream master `ef0614e724` and the branch head
`452875f996`, with two tools: `rzb`, a harness that times
`vdev_raidz_generate_parity()` and `vdev_raidz_reconstruct()` on real raidz
maps of linear and scatter ABDs, and `raidz_test -B`, patched for this run only
(an implementation filter, and thread CPU time instead of wall time).

Rules:

- Run as `tigerblue77`, in bash. Use `sudo -n` only where written.
- Post, push and install nothing. Never reboot.
- Work only under `/var/tmp/raidz-1106d`, log to `/var/log/raidz-1106d`. Leave
  every other file, directory, process and module alone,
  `/etc/zfs/zpool.cache` included.
- A production hypervisor sits underneath: build with `nice -n 10 ionice -c3`
  and `make -j8`; run each benchmark single-threaded, pinned to one CPU, one at
  a time, never several in parallel.
- After any line that says `stop`, run nothing more and report.

## STEP 0: the VM must be free

```sh
id -un; uname -r; nproc; grep -m1 'model name' /proc/cpuinfo
grep -o -w -E 'avx2|avx512f|avx512bw|gfni' /proc/cpuinfo | sort -u | xargs echo "flags present:"
lsmod | grep -E '^(zfs|spl) ' || echo "no zfs/spl loaded"
pgrep -af 'make|configure|cc1|zfs-tests|ztest|zloop|raidz_test' | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)' || echo "no build or test running"
ls -d /var/tmp/raidz-1106d /var/log/raidz-1106d 2>&1
df -BG --output=avail /var/tmp | tail -1
command -v gcc python3 git patch taskset | wc -l
uptime
```

Expected: `tigerblue77`, 16 CPUs, `model name : Intel(R) Xeon(R) CPU
E5-2696 v4 ...`, `flags present: avx2`, `no build or test running`, both
directories `No such file or directory`, at least 6G free, `5`.

Stop if the user is not `tigerblue77`, a build or test runs, either directory
exists, less than 6G is free, or a tool is missing. A loaded zfs module does not
matter (nothing here uses it), record it. If the CPU is not an E5-2696 v4, or
`avx512f` or `gfni` is listed, do not stop: record it, it changes what the
numbers mean.

## STEP 1: sources

```sh
W=/var/tmp/raidz-1106d; L=/var/log/raidz-1106d
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L
mkdir -p $W && cd $W && git init -q src && cd src &&
git fetch -q https://github.com/openzfs/zfs ef0614e724d42b58f7b551a5e718648264b2bd0c > $L/fetch-master.log 2>&1 &&
git fetch -q https://github.com/tigerblue77/zfs 452875f996b5650567ee41660327256c1e4d9386 > $L/fetch-head.log 2>&1 &&
git worktree add -q --detach $W/master ef0614e724d42b58f7b551a5e718648264b2bd0c &&
git worktree add -q --detach $W/head 452875f996b5650567ee41660327256c1e4d9386 &&
git -C $W/head log --format='%h %s' ef0614e724..HEAD
```

Expected: the two commits, `raidz: multiply by a constant with one table
lookup per nibble` and `RAIDZ: keep parity in registers across data columns`,
the second one first. Stop on a fetch error and quote the log.

## STEP 2: the tools

The three files are inlined because the VM cannot read the files they come from.
Write them as they are, then check their sha256. The gate is the sha256 with
every run of blanks squeezed to one space, since a message in transit may turn
tabs into spaces and nothing here depends on which blank is used; the exact
sha256 is information only.

```sh
W=/var/tmp/raidz-1106d; mkdir -p $W/tools && cd $W/tools
cat > rzb.c <<'EOF_RZB_C'
/*
 * Scratch benchmark of the RAID-Z parity math of one libzpool build, on
 * real raidz maps (linear and scatter ABDs).  Not part of any repository.
 *
 * Usage: rzb impl warm|cold <window_ms> <d>:<blocksize> [...]
 * Output CSV: mode,impl,d,bs,abd,op,best_GBps,median_GBps
 * (GB = 1e9 bytes of DATA, i.e. the block size, per call).
 * Every op is verified against the columns of the first generation,
 * before and after the timed runs.
 */
#include <sys/zfs_context.h>
#include <sys/zio.h>
#include <sys/abd.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <libzpool.h>
#include <stdio.h>
#include <time.h>
#include <emmintrin.h>

#define	MAXC	32

typedef struct bset {
	int np, d, ncols;	/* ncols: columns of the row, can be < np + d */
	size_t bs;
	zio_t zio;
	raidz_map_t *rm;
	abd_t *gold[MAXC];
} bset_t;

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

static int
flush_cb(void *buf, size_t len, void *priv)
{
	(void) priv;
	for (size_t i = 0; i < len; i += 64)
		_mm_clflush((const char *)buf + i);
	return (0);
}

static void
flush_set(bset_t *b)
{
	raidz_row_t *rr = b->rm->rm_row[0];

	abd_iterate_func(b->zio.io_abd, 0, b->bs, flush_cb, NULL);
	for (int c = 0; c < rr->rr_cols; c++)
		abd_iterate_func(rr->rr_col[c].rc_abd, 0,
		    rr->rr_col[c].rc_size, flush_cb, NULL);
	_mm_mfence();
}

static void
bset_init(bset_t *b, int np, int d, size_t bs, abd_t *src, int scatter,
    const char *impl)
{
	memset(b, 0, sizeof (*b));
	b->np = np;
	b->d = d;
	b->bs = bs;
	b->zio.io_size = bs;
	b->zio.io_abd = scatter ? abd_alloc(bs, B_FALSE) :
	    abd_alloc_linear(bs, B_FALSE);
	VERIFY3U(abd_is_linear(b->zio.io_abd), ==, !scatter);
	abd_copy(b->zio.io_abd, src, bs);
	b->rm = vdev_raidz_map_alloc(&b->zio, 12, d + np, np);
	VERIFY3U(b->rm->rm_nrows, ==, 1);
	VERIFY0(strcmp(b->rm->rm_ops->name, impl));
	raidz_row_t *rr = b->rm->rm_row[0];
	VERIFY3U(rr->rr_cols, <=, d + np);
	VERIFY3U(rr->rr_cols, >, np);
	b->ncols = rr->rr_cols;
	vdev_raidz_generate_parity(b->rm);
	for (int c = 0; c < b->ncols; c++) {
		size_t sz = rr->rr_col[c].rc_size;
		b->gold[c] = abd_alloc_linear(sz, B_FALSE);
		abd_copy(b->gold[c], rr->rr_col[c].rc_abd, sz);
	}
}

static void
check(bset_t *b, int c, const char *what)
{
	raidz_row_t *rr = b->rm->rm_row[0];
	if (abd_cmp(rr->rr_col[c].rc_abd, b->gold[c]) != 0) {
		fprintf(stderr, "MISMATCH %s np=%d d=%d bs=%zu col=%d\n", what,
		    b->np, b->d, b->bs, c);
		exit(2);
	}
}

static int
smash_cb(void *buf, size_t len, void *priv)
{
	(void) priv;
	memset(buf, 0xa5, len);
	return (0);
}

static void
smash(bset_t *b, int c)
{
	raidz_row_t *rr = b->rm->rm_row[0];
	abd_iterate_func(rr->rr_col[c].rc_abd, 0, rr->rr_col[c].rc_size,
	    smash_cb, NULL);
}

typedef struct op {
	const char *name;
	int np;		/* parity of the map */
	int gen;	/* parity generation, else reconstruction */
	int nt;
	int t[3];	/* target columns: parity ones are treated as missing */
} op_t;

/* columns: 0..np-1 parity, then data */
static const op_t ops[] = {
	{ "gen_p", 1, 1, 0, { 0 } },
	{ "gen_pq", 2, 1, 0, { 0 } },
	{ "gen_pqr", 3, 1, 0, { 0 } },
	{ "rec_p", 2, 0, 1, { 2 } },
	{ "rec_q", 2, 0, 2, { 0, 2 } },
	{ "rec_r", 3, 0, 3, { 0, 1, 3 } },
	{ "rec_pq", 2, 0, 2, { 2, 3 } },
	{ "rec_pqr", 3, 0, 3, { 3, 4, 5 } },
};
#define	NOPS	(sizeof (ops) / sizeof (ops[0]))

/* the op needs data columns the row does not have */
static int
na(const op_t *o, const bset_t *b)
{
	for (int i = 0; i < o->nt; i++)
		if (o->t[i] >= b->ncols)
			return (1);
	return (0);
}

static inline void
run(const op_t *o, bset_t *b)
{
	if (o->gen)
		vdev_raidz_generate_parity(b->rm);
	else
		vdev_raidz_reconstruct(b->rm, o->t, o->nt);
}

static void
verify_op(const op_t *o, bset_t *b)
{
	if (o->gen) {
		for (int p = 0; p < b->np; p++)
			smash(b, p);
		run(o, b);
		for (int p = 0; p < b->np; p++)
			check(b, p, o->name);
	} else {
		for (int i = 0; i < o->nt; i++)
			if (o->t[i] >= b->np)
				smash(b, o->t[i]);
		run(o, b);
		for (int i = 0; i < o->nt; i++)
			if (o->t[i] >= b->np)
				check(b, o->t[i], o->name);
	}
}

static double
measure(const op_t *o, bset_t *b, int cold, double secs)
{
	double t = 0, t0, deadline;
	uint64_t it = 0;

	run(o, b);
	if (!cold) {
		t0 = now();
		deadline = t0 + secs;
		do {
			for (int i = 0; i < 16; i++)
				run(o, b);
			it += 16;
			t = now();
		} while (t < deadline);
		t -= t0;
	} else {
		deadline = now() + 4 * secs;
		while (t < secs && now() < deadline) {
			flush_set(b);
			t0 = now();
			run(o, b);
			t += now() - t0;
			it++;
		}
	}
	return ((double)b->bs * it / t / 1e9);
}

static int
dcmp(const void *x, const void *y)
{
	double a = *(const double *)x, b = *(const double *)y;
	return ((a > b) - (a < b));
}

#define	NWIN	10

int
main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: rzb impl warm|cold ms d:bs ...\n");
		return (1);
	}
	const char *impl = argv[1];
	int cold = strcmp(argv[2], "cold") == 0;
	double secs = atof(argv[3]) / 1000.0;

	kernel_init(SPA_MODE_READ);
	VERIFY0(vdev_raidz_impl_set(impl));

	for (int ai = 4; ai < argc; ai++) {
		int d;
		size_t bs;
		static bset_t sets[2][4];
		static double g[2][NOPS][NWIN];

		if (sscanf(argv[ai], "%d:%zu", &d, &bs) != 2 || d < 3 ||
		    d > 24)
			return (1);
		abd_t *src = abd_alloc_linear(bs, B_FALSE);
		uint8_t *s = abd_to_buf(src);
		srand(12345 + d);
		for (size_t i = 0; i < bs; i++)
			s[i] = rand();
		for (int sc = 0; sc < 2; sc++)
			for (int np = 1; np <= 3; np++)
				bset_init(&sets[sc][np], np, d, bs, src, sc,
				    impl);
		for (int sc = 0; sc < 2; sc++)
			for (size_t k = 0; k < NOPS; k++)
				if (!na(&ops[k], &sets[sc][ops[k].np]))
					verify_op(&ops[k],
					    &sets[sc][ops[k].np]);
		for (int w = 0; w < NWIN; w++)
			for (int sc = 0; sc < 2; sc++)
				for (size_t k = 0; k < NOPS; k++)
					g[sc][k][w] = na(&ops[k],
					    &sets[sc][ops[k].np]) ? 0 :
					    measure(&ops[k],
					    &sets[sc][ops[k].np], cold, secs);
		for (int sc = 0; sc < 2; sc++) {
			for (size_t k = 0; k < NOPS; k++) {
				if (na(&ops[k], &sets[sc][ops[k].np]))
					continue;
				qsort(g[sc][k], NWIN, sizeof (double), dcmp);
				printf("%s,%s,%d,%zu,%s,%s,%.3f,%.3f\n",
				    argv[2], impl, d, bs, sc ? "scatter" :
				    "linear", ops[k].name, g[sc][k][NWIN - 1],
				    (g[sc][k][NWIN / 2 - 1] +
				    g[sc][k][NWIN / 2]) / 2);
			}
		}
		fflush(stdout);
		for (int sc = 0; sc < 2; sc++)
			for (int np = 1; np <= 3; np++)
				for (int c = 0; c < sets[sc][np].ncols; c++)
					check(&sets[sc][np], c, "post");
		/* maps are leaked on purpose: scratch tool */
		abd_free(src);
	}
	kernel_fini();
	return (0);
}
EOF_RZB_C
cat > raidz_bench.patch <<'EOF_RAIDZ_BENCH_PATCH'
--- a/cmd/raidz_test/raidz_bench.c
+++ b/cmd/raidz_test/raidz_bench.c
@@ -34,6 +34,15 @@
 static raidz_map_t *rm_bench;
 static size_t max_data_size = SPA_MAXBLOCKSIZE;
 
+/* local benchmark change: thread CPU time, not wall time (shared host) */
+static hrtime_t
+bench_cpu_ns(void)
+{
+	struct timespec ts;
+	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
+	return ((hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
+}
+
 static void
 bench_init_raidz_map(void)
 {
@@ -88,10 +97,10 @@
 			iter_cnt = GEN_BENCH_MEMORY;
 			iter_cnt /= zio_bench.io_size;
 
-			start = gethrtime();
+			start = bench_cpu_ns();
 			for (iter = 0; iter < iter_cnt; iter++)
 				vdev_raidz_generate_parity(rm_bench);
-			elapsed = NSEC2SEC((double)(gethrtime() - start));
+			elapsed = NSEC2SEC((double)(bench_cpu_ns() - start));
 
 			disksize = (1ULL << ds) / rto_opts.rto_dcols;
 			d_bw = (double)iter_cnt * (double)disksize;
@@ -122,6 +131,9 @@
 	for (impl_name = (char **)raidz_impl_names; *impl_name != NULL;
 	    impl_name++) {
 
+		if (getenv("RAIDZ_BENCH_IMPL") != NULL &&
+		    strstr(getenv("RAIDZ_BENCH_IMPL"), *impl_name) == NULL)
+			continue;
 		if (vdev_raidz_impl_set(*impl_name) != 0)
 			continue;
 
@@ -180,10 +192,10 @@
 			nbad = MIN(3, raidz_ncols(rm_bench) -
 			    raidz_parity(rm_bench));
 
-			start = gethrtime();
+			start = bench_cpu_ns();
 			for (iter = 0; iter < iter_cnt; iter++)
 				vdev_raidz_reconstruct(rm_bench, tgt[fn], nbad);
-			elapsed = NSEC2SEC((double)(gethrtime() - start));
+			elapsed = NSEC2SEC((double)(bench_cpu_ns() - start));
 
 			disksize = (1ULL << ds) / rto_opts.rto_dcols;
 			d_bw = (double)iter_cnt * (double)(disksize);
@@ -214,6 +226,9 @@
 	for (impl_name = (char **)raidz_impl_names; *impl_name != NULL;
 	    impl_name++) {
 
+		if (getenv("RAIDZ_BENCH_IMPL") != NULL &&
+		    strstr(getenv("RAIDZ_BENCH_IMPL"), *impl_name) == NULL)
+			continue;
 		if (vdev_raidz_impl_set(*impl_name) != 0)
 			continue;
 
EOF_RAIDZ_BENCH_PATCH
cat > agg.py <<'EOF_AGG_PY'
#!/usr/bin/env python3
# Aggregates res/: per case, median over reps of the best window (rzb) or
# of the raidz_test -B figure, with min-max, and the paired ratios of the
# variants (same rep = run back to back), median [min-max].
# Usage: agg.py <resdir> <out.md>
import collections
import glob
import os
import re
import statistics as st
import sys

R, OUT = sys.argv[1], sys.argv[2]
PAIRS = tuple(tuple(p.split('/')) for p in os.environ.get('PAIRS', 'p1/master,p2/p1,p2/master').split(','))
VAR = tuple(os.environ.get('VARS', 'master,p1,p2').split(','))

data = collections.defaultdict(dict)   # key -> {(v, rep): value}
for f in glob.glob(os.path.join(R, 'rzb.*.csv')):
    m = re.match(r'rzb\.(warm|cold)\.(\w+)\.r(\d+)\.csv', os.path.basename(f))
    mode, v, rep = m.group(1), m.group(2), int(m.group(3))
    for ln in open(f):
        p = ln.strip().split(',')
        if len(p) != 8:
            continue
        key = ('rzb-' + mode, p[1], int(p[2]), int(p[3]), p[4], p[5])
        data[key][(v, rep)] = float(p[6])
for f in glob.glob(os.path.join(R, 'rtb.*.csv')):
    m = re.match(r'rtb\.(\w+)\.r(\d+)\.csv', os.path.basename(f))
    v, rep = m.group(1), int(m.group(2))
    for ln in open(f):
        p = [x.strip() for x in ln.split(',')]
        if len(p) != 7 or not p[3].isdigit():
            continue
        dcols = int(p[2])
        gbps = float(p[4]) * dcols * 2**20 / 1e9
        key = ('raidz_test-B', p[0], dcols, int(p[3]), 'scatter', p[1])
        data[key][(v, rep)] = gbps


def fmt(xs):
    return '%.2f [%.2f-%.2f]' % (st.median(xs), min(xs), max(xs))


def ratios(d, a, b):
    reps = sorted({r for (v, r) in d if v == a} & {r for (v, r) in d if v == b})
    return [d[(a, r)] / d[(b, r)] for r in reps]


ORDER = {'gen_p': 0, 'gen_pq': 1, 'gen_pqr': 2, 'rec_p': 3, 'rec_q': 4,
         'rec_r': 5, 'rec_pq': 6, 'rec_pr': 7, 'rec_qr': 8, 'rec_pqr': 9}
rows = []
for key in sorted(data, key=lambda k: (k[0], k[1], k[4], ORDER.get(k[5], 99),
                                      k[2], k[3])):
    d = data[key]
    vals = []
    for v in VAR:
        xs = [d[(vv, r)] for (vv, r) in d if vv == v]
        vals.append('%.1f [%.1f-%.1f] n=%d' % (st.median(xs), min(xs), max(xs),
                                               len(xs)) if xs else '-')
    rs = []
    for a, b in PAIRS:
        x = ratios(d, a, b)
        rs.append(fmt(x) if x else '-')
    rows.append((key, vals, rs))

with open(OUT, 'w') as o:
    o.write('GB/s of data, median over reps [min-max]; ratios paired by rep, '
            'median [min-max].\n\n')
    last = None
    for key, vals, rs in rows:
        sec = (key[0], key[1], key[4])
        if sec != last:
            o.write('\n### %s, %s, %s ABD\n\n' % sec)
            o.write('| op | d | size | ' + ' | '.join(VAR) + ' | ' +
                    ' | '.join(a + '/' + b for a, b in PAIRS) + ' |\n|---|---|---|' +
                    '---|' * (len(VAR) + len(PAIRS)) + '\n')
            last = sec
        o.write('| %s | %d | %s | %s | %s |\n' % (
            key[5], key[2], '%dK' % (key[3] // 1024), ' | '.join(vals),
            ' | '.join(rs)))
print('wrote', OUT, len(rows), 'rows')


# Compact view: per (tool, impl, op), for each rep the geometric mean of the
# paired ratio over the sizes, column counts and ABD kinds; then the median
# [min-max] of these per-rep values.
import math
pairs = PAIRS
comp = collections.defaultdict(lambda: collections.defaultdict(list))
for key, d in data.items():
    tool, impl, dc, sz, abd, op = key
    if tool == 'raidz_test-B' and not (131072 <= sz <= 1048576):
        continue
    for a, b in pairs:
        for r in sorted({r for (v, r) in d if v == a} &
                        {r for (v, r) in d if v == b}):
            comp[(tool, impl, op, a + '/' + b)][r].append(
                math.log(d[(a, r)] / d[(b, r)]))
with open(OUT, 'a') as o:
    o.write('\n\n## Compact: per rep, geomean of paired ratios over sizes, '
            'd and ABD kinds; median [min-max] over reps\n\n')
    o.write('raidz_test-B restricted to 128K-1M here.\n\n')
    o.write('| tool | impl | op | ' + ' | '.join(a + '/' + b for a, b in pairs) +
            ' |\n|---|---|---|' + '---|' * len(pairs) + '\n')
    keys = sorted({(t, i, op) for (t, i, op, p) in comp},
                  key=lambda k: (k[0], k[1], ORDER.get(k[2], 99)))
    for t, i, op in keys:
        cells = []
        for a, b in pairs:
            reps = comp.get((t, i, op, a + '/' + b))
            if not reps:
                cells.append('-')
                continue
            g = [math.exp(sum(x) / len(x)) for x in reps.values()]
            cells.append('%.2f [%.2f-%.2f] n=%d' % (st.median(g), min(g),
                                                   max(g), len(g)))
        o.write('| %s | %s | %s | %s |\n' % (t, i, op, ' | '.join(cells)))
EOF_AGG_PY
same() { [ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1 ($2)"; }
for f in rzb.c raidz_bench.patch agg.py; do
	case $f in
	rzb.c) s=f82a759b9fe70882eeb13b3e0ae5af8c58fe79e1f69d66bf04f178bafde7db40; e=5af369a6808ec061d4c7e5b958e79f48747ca34fdf4507ef22b7c55a37bbacbf ;;
	raidz_bench.patch) s=7f73eac939a8612548a4487d184edd7b7a565de0fa11805df6b99fedbe50facc; e=ea6917280534d2a99ce3fcccf9fce03a34fcfb161945aa8e899e0a69905cc7bd ;;
	agg.py) s=fa6910b002045385fd28119b20a68ed904dd4942e31812b883e98ea2356bc7f3; e=7347f4b400dbd55eeaffb97aa070f9107c1dfaf390efd1f5f75a32daaf928a01 ;;
	esac
	same "$f sha256, blanks squeezed" "$(tr -s ' \t' ' ' < $f | sha256sum | cut -d' ' -f1)" $s
	same "$f sha256, exact (information only)" "$(sha256sum $f | cut -d' ' -f1)" $e
done
```

Expected: `ok: <file> sha256, blanks squeezed` for the three files. Stop on a
`MISMATCH` there, and quote the first 20 lines of `diff` against what this
document holds if you can. A `MISMATCH` on an exact line only means that tabs
became spaces.

## STEP 3: release builds

A release build (no `--enable-debug`), so the figures are the ones users get;
libzpool keeps its assertions on either way. The patch is applied with `-l`
(blanks ignored), which also covers tabs turned into spaces.

```sh
W=/var/tmp/raidz-1106d; L=/var/log/raidz-1106d
for t in master head; do
	cd $W/$t && patch -p1 -l < $W/tools/raidz_bench.patch > $L/patch-$t.log 2>&1; echo "$t patch rc=$?"
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --with-config=user --disable-pyzfs && make -j8 raidz_test' > $L/build-$t.log 2>&1
	echo "$t build rc=$? wall=$(( $(date +%s) - t0 ))s warnings=$(grep -c 'warning:' $L/build-$t.log)"
	gcc -O2 -std=gnu11 -include zfs_config.h -include include/sys/simd_config.h -Iinclude \
	    -Ilib/libspl/include -Ilib/libspl/include/os/linux -Ilib/libzpool/include \
	    -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
	    -DLIB_ZPOOL_BUILD -DDEBUG -UNDEBUG -DZFS_DEBUG -DTEXT_DOMAIN=\"zfs-linux-user\" \
	    -o $W/$t/rzb $W/tools/rzb.c -L.libs -lzpool -lnvpair -Wl,-rpath,$W/$t/.libs -lpthread \
	    && echo "$t rzb built"
	objdump -d $(find . -name '*vdev_raidz_math_avx2.o' | head -1) | grep -c vpshufb | xargs echo "$t avx2 vpshufb:"
done
```

Expected for each tree: `patch rc=0`, `build rc=0`, `rzb built`; `vpshufb:`
128 for master and 72 for head. On a failed build, quote
`grep -nE 'error:|Error [0-9]' $L/build-<tree>.log | head -20` and stop.

## STEP 4: correctness of the tools

`rzb` checks every result against the columns of the first generation, and
exits with `MISMATCH` and status 2 on a difference.

```sh
W=/var/tmp/raidz-1106d
for t in master head; do for i in scalar sse2 ssse3 avx2; do
	$W/$t/rzb $i warm 1 4:4096 4:8192 5:28672 6:131072 13:1048576 > /dev/null 2>&1; echo "$t $i rc=$?"
done; done
```

Expected: eight lines ending in `rc=0`. Stop otherwise and quote the failing
command's stderr.

## STEP 5: runs

Five rounds; in each, master and head in turn, the order swapped every round,
each tool pinned to CPU 2 under `nice`. A round takes about 10 minutes.

```sh
W=/var/tmp/raidz-1106d; L=/var/log/raidz-1106d; R=$L/res; mkdir -p $R
P="taskset -c 2 nice -n 10"
for r in 1 2 3 4 5; do
	if [ $((r % 2)) = 1 ]; then V="master head"; else V="head master"; fi
	for v in $V; do
		echo "round $r $v start $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
		for i in avx2 ssse3 sse2 scalar; do
			$P $W/$v/rzb $i warm 20 4:4096 4:8192 6:131072 8:131072 8:1048576 >> $R/rzb.warm.$v.r$r.csv 2>> $R/err.log
		done
		for i in avx2 ssse3; do
			$P $W/$v/rzb $i cold 20 4:4096 8:131072 8:1048576 >> $R/rzb.cold.$v.r$r.csv 2>> $R/err.log
		done
		(cd $W/$v && RAIDZ_BENCH_IMPL=avx2,ssse3 $P ./raidz_test -B > $R/rtb.$v.r$r.csv 2>> $R/err.log)
		echo "round $r $v end $(date -u +%T) rc=$?"
	done
done 2>&1 | tee $L/rounds.txt
wc -c $R/err.log
```

Expected: ten `end` lines ending in `rc=0`, and `0 .../err.log`. A crash: keep
the files and go on.

## STEP 6: tables

```sh
L=/var/log/raidz-1106d
VARS=master,head PAIRS=head/master python3 -I /var/tmp/raidz-1106d/tools/agg.py $L/res $L/tables.md
sed -n '/## Compact/,$p' $L/tables.md
```

Expected: `wrote ... rows`, then the compact table: for each tool,
implementation and operation, the ratio head/master, median [min-max] over the
five rounds of its geometric mean over block sizes, column counts and ABD
kinds. Above 1.00 means faster.

## STEP 7: report and cleanup

Write `/var/log/raidz-1106d/REPORT.txt` with: STEP 0's lines; the two commits of
STEP 1; the sha256 lines of STEP 2; STEP 3's lines; STEP 4's lines;
`rounds.txt`; then the whole of `tables.md`. Add the deviations from these
steps, if any. Then:

```sh
tar -C /var/log/raidz-1106d -czf /var/log/raidz-1106d/res.tgz res && rm -rf /var/tmp/raidz-1106d && echo "work dir removed; logs and res.tgz kept in /var/log/raidz-1106d"
```

Reply with the whole of `REPORT.txt`.
