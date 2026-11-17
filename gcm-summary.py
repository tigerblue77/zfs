# usage: python3 -I gcm-summary.py <dir>
# Reads <dir>/P0-run.txt (run.sh output: avx block, then sse block) and
# <dir>/P[1-9]-{avx,sse}.txt (gcm-pass.sh), prints one markdown table per
# variant: per mode and size, the Linux/ICP ratio of each pass, the median
# and range of P1.. pass ratios, how many of them are above 1, and the
# median GB/s and cycles/byte of each side over P1...
import glob, os, re, statistics, sys

d = sys.argv[1]
row = re.compile(r"^(asm-enc|asm-dec|msg-enc|msg-dec)\s+(\d+)\s+"
                 r"([\d.]+) ; ([\d.]+) ; \S+\s+([\d.]+) ; ([\d.]+) ; \S+\s+"
                 r"([\d.]+)x \[([\d.]+)-([\d.]+)\]")
var_of = {"AVX": "avx", "SSE4.1": "sse"}


def parse(text):
    """{variant: {(mode, size): (icp_gbs, icp_cpb, lx_gbs, lx_cpb, ratio)}}"""
    out, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"Linux column: AES-NI \+ (\S+) variant", line)
        if m:
            cur = var_of[m.group(1)]
            out[cur] = {}
            continue
        m = row.match(line)
        if m and cur:
            g = m.groups()
            out[cur][(g[0], int(g[1]))] = tuple(float(x) for x in g[2:7])
    return out


passes = {}
p0 = os.path.join(d, "P0-run.txt")
if os.path.exists(p0):
    passes["P0"] = parse(open(p0).read())
for f in sorted(glob.glob(os.path.join(d, "P[1-9]-*.txt"))):
    p = os.path.basename(f).split("-")[0]
    for v, rows in parse(open(f).read()).items():
        passes.setdefault(p, {})[v] = rows
names = sorted(passes)
meas = [p for p in names if p != "P0"]
for v in ("avx", "sse"):
    keys = []
    for p in names:
        for k in passes[p].get(v, {}):
            if k not in keys:
                keys.append(k)
    if not keys:
        continue
    print(f"\n### Linux {v} variant / ICP (ratio > 1: Linux faster)\n")
    print("| mode | size | " + " | ".join(names) + " | median " + "/".join(meas)
          + " | range | >1 | ICP GB/s | ICP c/B | Linux GB/s | Linux c/B |")
    print("|---" * (len(names) + 9) + "|")
    for k in keys:
        cells = []
        for p in names:
            r = passes[p].get(v, {}).get(k)
            cells.append(f"{r[4]:.3f}" if r else "-")
        rs = [passes[p][v][k] for p in meas if k in passes[p].get(v, {})]
        if not rs:
            continue
        rat = [r[4] for r in rs]
        med = lambda i: statistics.median(r[i] for r in rs)
        print(f"| {k[0]} | {k[1]} | " + " | ".join(cells)
              + f" | {statistics.median(rat):.3f} | {min(rat):.3f}-{max(rat):.3f}"
              f" | {sum(x > 1 for x in rat)}/{len(rat)} | {med(0):.2f} | {med(1):.2f}"
              f" | {med(2):.2f} | {med(3):.2f} |")
