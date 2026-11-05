# VM 107 task: A/B/A of node-local I/O completion (6-7/11)

Goal: measure whether finishing each I/O on the NUMA node that issued it pays,
with the same protocol as the baseline (vm-tasks/VM-NUMA-1105.md, fork branch
vm-tasks-1105 at 10a78150c4): its Blocks 1, 2, 3 and 5, unchanged, on two
patched builds, with stock master run again between them so that drift shows
(about 3 h 30 min in all).
This VM: 16 vCPUs, guest node 0 = vCPU 0-7 on host node 0, guest node 1 =
vCPU 8-15 on host node 1, 16 GiB, the brd "device" pre-faulted on node 0.

Rules: this VM is disposable; nothing is posted or pushed; results stay under
/root/zfs-numa-bench and /var/tmp/zts-numa-1106; keep the VM otherwise idle
while a pass runs; RELEASE builds only (no --enable-debug, no
--enable-debuginfo); stop at the first unexpected result and report it;
`sudo fstrim -av` at the end. Every block below runs as your usual user and
uses `sudo -n` itself, like the baseline's rb.sh.

## What is compared

| build | commit | what changes |
|---|---|---|
| A | master 1974111f45, already built at /var/tmp/zts-numa-1105/M | stock: a completion goes to a random `z_rd_int_N`/`z_wr_int_N` taskq of the pool |
| B0 | fork `numa-local-completion~1` = 168d37e431 | phase 0: the read issue taskqs and the read and write interrupt taskqs are split into one group per node; a zio goes to the group of the CPU that dispatches it; threads unbound |
| B1 | fork `numa-local-completion` = 1a070dadd2 | phase 0+1: the threads of each group are also kept on their node; on a kernel before 6.14 (yours, 6.12) each such thread is bound to one CPU of its node |

B0 and B1 sit on master ef35f144e1, 4 commits after A. From reading, none of
the 4 is on a measured path (a dbuf hole-check refactor, O_DIRECT reads, a ZIL
commit while the ZIL is suspended, receive, decrypt on hold).

Expected with 16 CPUs and 2 nodes, once the test pool exists:
- A: `z_rd_int_0..2` and `z_wr_int_0..2` (max 4 threads each), `z_rd_iss`
  (max 8); every zio thread allows CPUs 0-15; no `zio_taskq_numa` parameter.
- B0: `z_rd_int_0..3` and `z_wr_int_0..3` (max 3 each; 0-1 serve node 0,
  2-3 node 1), `z_rd_iss_0..1` (max 8 each); threads allow 0-15;
  `zio_taskq_numa=1`.
- B1: as B0, but each thread of those taskqs allows a single CPU: one of 0-7
  for `*_0`, `*_1` and `z_rd_iss_0`, one of 8-15 for `*_2`, `*_3` and
  `z_rd_iss_1`. (On a kernel 6.14 or later they would allow 0-7 or 8-15.)

## Step 0: preconditions (read-only, then records the baseline as pass A1)
```sh
(
W=/var/tmp/zts-numa-1106; mkdir -p $W || exit 1
base=$(sudo -n readlink -f /root/zfs-numa-bench/current) || exit 1
echo "baseline: $base"
sudo -n wc -l "$base/numa.txt" "$base/scale.txt"  # 24 and 22 lines, else not finished: stop
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # no pool: prints nothing
grep -c '^brd ' /proc/modules                                         # 0
echo "loaded $(cat /sys/module/zfs/srcversion) A $(modinfo -F srcversion /var/tmp/zts-numa-1105/M/module/zfs.ko)"  # equal, or nothing loaded
uname -r; numactl -H | head -3; df -h /var/tmp | tail -1   # 2 nodes; 8 GiB free or more
echo "A1 $base" > $W/passes.txt
)
```

## Step 1: sources and two release builds (about 25 min)
`G` is the clone the baseline's M worktree came from; any clone of openzfs/zfs
on the VM will do.
```sh
(
W=/var/tmp/zts-numa-1106 G=/var/tmp/zts-rzsusp-1101/repo
git -C $G fetch -q https://github.com/tigerblue77/zfs \
    numa-local-completion:refs/remotes/fork/numa-local-completion \
    vm-tasks-1105:refs/remotes/fork/vm-tasks-1105 || exit 1
git -C $G rev-parse fork/numa-local-completion   # 1a070dadd2c65c8976ff9829e2bf9bb3295c882c
git -C $G worktree add -q --detach $W/B0 168d37e431589aec35d2e05eea404ad275952642 || exit 1
git -C $G worktree add -q --detach $W/B1 1a070dadd2c65c8976ff9829e2bf9bb3295c882c || exit 1
git -C $W/B1 log --format='%h %s' -3   # 1a070dadd2, 168d37e431, ef35f144e1
for t in B0 B1; do
	(cd $W/$t && nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j8') > $W/build-$t.log 2>&1
	echo "$t rc=$? ZFS_DEBUG=$(grep -c '#define ZFS_DEBUG 1' $W/$t/zfs_config.h) affine=$(grep -c '#define HAVE_KTHREAD_AFFINE_PREFERRED 1' $W/$t/zfs_config.h) srcversion=$(modinfo -F srcversion $W/$t/module/zfs.ko)"
done
grep 'kthread_affine_preferred' $W/build-B1.log   # "... exists... no" on 6.12
)
```
Both rc=0, ZFS_DEBUG=0, affine=0 (on 6.12), and two srcversions that differ
from each other and from A's.

## Step 2: the baseline's blocks, byte for byte, and the helpers
```sh
(
W=/var/tmp/zts-numa-1106 G=/var/tmp/zts-rzsusp-1101/repo
git -C $G show 10a78150c486b982969c8723980c9a7754c0cc25:vm-tasks/VM-NUMA-1105.md > $W/VM-NUMA-1105.md || exit 1
awk -v d="$W" '/^```sh$/ { n++; f = d "/blk" n ".sh"; printf "" > f; on = 1; next } /^```$/ { on = 0; next } on { print > f }' $W/VM-NUMA-1105.md
cd $W && sha256sum blk1.sh blk2.sh blk3.sh blk4.sh blk5.sh
)
```
Must print, in order (inventory, Block 1, Block 2, Block 3, Block 5):
```
f02a8c1afd302141b4f3f998d96bff62f136a12987702c939ba1e96737cbeea9  blk1.sh
dbe4b51f4b9ebbc7ebcd2dcabc404391fd3fc94d8e5b205a2ae70e619ca9e2db  blk2.sh
f5fa2cfa82a05acbc3b79f5c5437a2417b0fa9e20e8cc3ff642bdc68a9c2e061  blk3.sh
45bfb79c931d1b7b56dfab1e7cf6d09d652fcfc75e55834942fa46a5fc67013e  blk4.sh
9c489f271b9413dde823bcfc11e5b63abc8b7ffbae956a320b962036f3eb18c7  blk5.sh
```
Then the helpers (load.sh: swap the module stack and check srcversion;
placement.sh: taskqs and thread CPUs; pass.sh: one pass; zio-cpu.py: CPU of
the completion threads per node, sampled once a second during Blocks 2 and 3;
ab-table.py: the report table):
```sh
W=/var/tmp/zts-numa-1106
cat > $W/load.sh << 'E'
#!/bin/bash
# load.sh TREE: replace the loaded spl/zfs with TREE's and check srcversion.
set -u
T=$1
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ] ||
    { echo "a pool is imported: stopping"; exit 1; }
grep -q '^brd ' /proc/modules && { echo "brd loaded: stopping"; exit 1; }
if grep -q '^zfs ' /proc/modules; then
	(cd "$T" && sudo -n ./scripts/zfs.sh -u) || { echo "unload failed"; exit 1; }
fi
(cd "$T" && sudo -n ./scripts/zfs.sh) || { echo "load failed"; exit 1; }
for m in spl zfs; do
	want=$(modinfo -F srcversion "$T/module/$m.ko") have=$(cat /sys/module/$m/srcversion)
	[ "$want" = "$have" ] || { echo "$m loaded $have, built $want: stopping"; exit 1; }
done
echo "loaded $T zfs=$(cat /sys/module/zfs/srcversion) spl=$(cat /sys/module/spl/srcversion)"
E
cat > $W/placement.sh << 'E'
#!/bin/bash
# placement.sh: per-node split of the zio taskqs and CPUs of their threads.
echo "zio_taskq_numa=$(cat /sys/module/zfs/parameters/zio_taskq_numa 2>/dev/null || echo absent)"
sudo -n grep -E '^z_(rd_iss|rd_int|wr_int)' /proc/spl/kstat/taskq/summary
for d in /proc/[0-9]*; do
	c=$(cat "$d/comm" 2> /dev/null) || continue
	case $c in
	z_rd_iss*|z_rd_int_*|z_wr_int_*)
		echo "$c cpus=$(awk '/^Cpus_allowed_list/ { print $2 }' "$d/status")" ;;
	esac
done | sort | uniq -c
E
cat > $W/pass.sh << 'E'
#!/bin/bash
# pass.sh LABEL TREE: load TREE, run the baseline's inventory and Blocks 1, 2,
# 3 and 5 with TREE's tools, sample the completion threads' CPU during Blocks
# 2 and 3, and record where the results went.
set -u
W=/var/tmp/zts-numa-1106 L=$1 T=$2
rb() { sudo -n env PATH="$T:/usr/sbin:/usr/bin:/sbin:/bin" bash "$W/blk$1.sh"; }
k0=$(sudo -n dmesg | wc -l)
"$W/load.sh" "$T" || exit 1
rb 1 > "$W/$L-inventory.out" 2>&1 || { echo "inventory failed"; exit 1; }
R=$(sudo -n readlink -f /root/zfs-numa-bench/current) || exit 1
echo "$L $R" >> "$W/passes.txt"
rb 2 > "$W/$L-b1.out" 2>&1 || { echo "Block 1 failed"; rb 5; exit 1; }
"$W/placement.sh" > "$W/$L-placement.txt" 2>&1
sudo -n python3 -I "$W/zio-cpu.py" "$W/$L-ziocpu.txt" 7200 &
sp=$!
rb 3 > "$W/$L-b2.out" 2>&1 || echo "Block 2 rc=$?"
rb 4 > "$W/$L-b3.out" 2>&1 || echo "Block 3 rc=$?"
sudo -n kill "$sp" 2> /dev/null; wait "$sp" 2> /dev/null
rb 5 > "$W/$L-b5.out" 2>&1 || { echo "Block 5 failed"; exit 1; }
echo "$L results $R: numa $(sudo -n cat "$R/numa.txt" | wc -l) scale $(sudo -n cat "$R/scale.txt" | wc -l)"
echo "$L kernel log: $(sudo -n dmesg | tail -n +$((k0 + 1)) | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|WARNING:')"
tail -1 "$W/$L-b5.out"
E
cat > $W/zio-cpu.py << 'E'
import os
import sys
import time

# zio-cpu.py OUT [SECONDS]: once a second, for each z_rd_int_* and z_wr_int_*
# thread, append "time node ticks" to OUT: the CPU ticks (user + system) it
# used since the previous sample, and the node of the CPU it last ran on.
out = open(sys.argv[1], "a", buffering=1)
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 14400)
node = {}
for n in os.listdir("/sys/devices/system/node"):
    if n.startswith("node") and n[4:].isdigit():
        p = "/sys/devices/system/node/%s/cpulist" % n
        for r in open(p).read().strip().split(","):
            if r:
                a, _, b = r.partition("-")
                for c in range(int(a), int(b or a) + 1):
                    node[c] = int(n[4:])
kind = {}
last = {}
first = True
while time.time() < end:
    now = time.time()
    seen = set()
    pids = [p for p in os.listdir("/proc") if p.isdigit()]
    for pid in pids:
        if pid not in kind:
            try:
                c = open("/proc/%s/comm" % pid).read().strip()
            except OSError:
                continue
            kind[pid] = c.startswith(("z_rd_int_", "z_wr_int_"))
        if not kind[pid]:
            continue
        try:
            f = open("/proc/%s/stat" % pid).read().rsplit(")", 1)[1].split()
        except (OSError, IndexError):
            continue
        # Fields after the command: state is field 3, utime 14, stime 15,
        # processor 39 in proc(5) numbering.
        ticks = int(f[11]) + int(f[12])
        cpu = int(f[36])
        seen.add(pid)
        # A thread first seen after the first scan started since then.
        prev = last.get(pid, None if first else 0)
        if prev is not None and ticks > prev:
            out.write("%.2f %d %d\n" % (now, node.get(cpu, -1),
                ticks - prev))
        last[pid] = ticks
    first = False
    for pid in list(last):
        if pid not in seen:
            del last[pid]
    alive = set(pids)
    for pid in list(kind):
        if pid not in alive:
            del kind[pid]
    time.sleep(max(0.0, 1.0 - (time.time() - now)))
E
cat > $W/ab-table.py << 'E'
import bisect
import math
import os
import sys

# ab-table.py PASSES: each line of PASSES is "LABEL DIR", LABEL one of
# A1 B0 A2 B1 A3.  Reads DIR/numa.txt (Block 2) and DIR/scale.txt (Block 3)
# as run.sh writes them (label ds rw bs jobs place perf iops MiB/s p99_us
# cpu% remote% hitm/s throttles arc_dmiss), and LABEL-ziocpu.txt next to
# PASSES, written by zio-cpu.py, if there is one.  A run's fio measures the
# 30 s after a 5 s ramp, from the time run.sh writes raw/RUN.numastat.0.
nan = float("nan")
hz = os.sysconf("SC_CLK_TCK")
here = os.path.dirname(os.path.abspath(sys.argv[1]))
passes = [ln.split() for ln in open(sys.argv[1]) if ln.strip()]


def num(s):
    try:
        return float(s)
    except ValueError:
        return nan


def mean(xs):
    xs = [x for x in xs if not math.isnan(x)]
    return sum(xs) / len(xs) if xs else nan


def completion(samples, d, run, iops):
    # Completion thread CPU in us per I/O, and its share on node 1 in %.
    try:
        t0 = os.path.getmtime("%s/raw/%s.numastat.0" % (d, run)) + 5
    except OSError:
        return nan, nan
    if not samples[0] or math.isnan(iops) or iops <= 0:
        return nan, nan
    lo = bisect.bisect_right(samples[0], t0)
    hi = bisect.bisect_right(samples[0], t0 + 30)
    tot = sum(samples[2][lo:hi])
    n1 = sum(t for n, t in zip(samples[1][lo:hi], samples[2][lo:hi])
        if n == 1)
    if tot == 0:
        return 0.0, nan
    return 1e6 * tot / hz / (iops * 30), 100.0 * n1 / tot


data = {}
for lab, d in passes:
    samples = ([], [], [])
    try:
        for ln in open("%s/%s-ziocpu.txt" % (here, lab)):
            f = ln.split()
            if len(f) == 3:
                for i in range(3):
                    samples[i].append(float(f[i]))
    except OSError:
        pass
    for name in ("numa.txt", "scale.txt"):
        try:
            lines = open(d + "/" + name).read().splitlines()
        except OSError:
            continue
        for ln in lines:
            f = ln.split()
            t = f[0].split("-") if f else []
            if len(f) < 13 or t[0] not in ("numa", "scale"):
                continue
            place = t[2] if t[0] == "numa" else "all"
            key = (t[0], t[1], place, int(f[4]))
            iops = num(f[7])
            cus, n1 = completion(samples, d, f[0], iops)
            v = (iops, num(f[9]), num(f[11]), num(f[12]) / 1e6, cus, n1)
            row = data.setdefault(key, {}).setdefault(lab, [[] for _ in v])
            for i, x in enumerate(v):
                row[i].append(x)


def val(runs, lab, i):
    return mean(runs[lab][i]) if lab in runs else nan


def rel(b, a):
    if math.isnan(a) or math.isnan(b) or a == 0:
        return "    -"
    return "%+5.1f" % (100.0 * (b - a) / a)


def fmt(x, f):
    return "-" if math.isnan(x) else f % x


def own(runs, labs):
    # remote%, hitm M/s, completion us/IO and node-1 share, over labs.
    return " %4s %5s %5s %3s" % tuple(
        fmt(mean([val(runs, x, i) for x in labs]), f)
        for i, f in ((2, "%.1f"), (3, "%.2f"), (4, "%.1f"), (5, "%.0f")))


wl = ["cread", "uread", "ureadlz4", "awrite", "swrite"]
pl = {"A": "node0", "B": "node1", "all": "all"}
print("blk workload place jobs |   A: iops  p99 drift% rem%  hitm   cus n1%"
    " | B0: iops%  p99% rem%  hitm   cus n1% | B1: iops%  p99% rem%  hitm"
    "   cus n1%")
for k in sorted(data, key=lambda k: (k[0], wl.index(k[1]),
    list(pl).index(k[2]), k[3])):
    runs = data[k]
    a = [x for x in (val(runs, lab, 0) for lab in ("A1", "A2", "A3"))
        if not math.isnan(x)]
    ai = mean(a)
    drift = 100.0 * (max(a) - min(a)) / ai if len(a) > 1 else nan
    out = "%-3s %-8s %-5s %4d | %9s %4s %6s%s |" % (
        "2" if k[0] == "numa" else "3", k[1], pl[k[2]], k[3],
        fmt(ai, "%.0f"),
        fmt(mean([val(runs, x, 1) for x in ("A1", "A2", "A3")]), "%.0f"),
        fmt(drift, "%.1f"), own(runs, ("A1", "A2", "A3")))
    for b, refs in (("B0", ("A1", "A2")), ("B1", ("A2", "A3"))):
        ri = mean([val(runs, x, 0) for x in refs])
        rp = mean([val(runs, x, 1) for x in refs])
        out += " %s %s %s%s |" % (b, rel(val(runs, b, 0), ri),
            rel(val(runs, b, 1), rp), own(runs, (b,)))
    print(out[:-2])
E
chmod 0755 $W/load.sh $W/placement.sh $W/pass.sh
sha256sum $W/load.sh $W/placement.sh $W/pass.sh $W/zio-cpu.py $W/ab-table.py
```
Must print:
```
01e959606a27febdb339fd336b37fe6e1a23c460c72f7b60adb78024a8ecb28b  /var/tmp/zts-numa-1106/load.sh
437e336f465cad520402df175ec3f6921fb97fe5966243c08b49dfd0cafe5a25  /var/tmp/zts-numa-1106/placement.sh
5fa0c5e6d674d896759cf7c9108c0fb00599cff6be41545fcb0263017e6bb7d5  /var/tmp/zts-numa-1106/pass.sh
67fc2b03186d02b60910aa094094b53648d84d25c895509f3584b2d1d68c36b2  /var/tmp/zts-numa-1106/zio-cpu.py
8a47280bb9189a454867840a8236608ae45dd477a5642363f18f2aa057fa26e5  /var/tmp/zts-numa-1106/ab-table.py
```

## Step 3: four passes, B0 A2 B1 A3 (about 40 min each)
Run each line as its own command, in this order, the VM otherwise idle. A pass
prints a `loaded` line (srcversion as built), a `results` line (numa 24, scale
22), a kernel log count (0) and Block 5's `clean;` line.
```sh
/var/tmp/zts-numa-1106/pass.sh B0 /var/tmp/zts-numa-1106/B0 2>&1 | tee /var/tmp/zts-numa-1106/pass-B0.log
/var/tmp/zts-numa-1106/pass.sh A2 /var/tmp/zts-numa-1105/M 2>&1 | tee /var/tmp/zts-numa-1106/pass-A2.log
/var/tmp/zts-numa-1106/pass.sh B1 /var/tmp/zts-numa-1106/B1 2>&1 | tee /var/tmp/zts-numa-1106/pass-B1.log
/var/tmp/zts-numa-1106/pass.sh A3 /var/tmp/zts-numa-1105/M 2>&1 | tee /var/tmp/zts-numa-1106/pass-A3.log
```
After each pass, compare /var/tmp/zts-numa-1106/<label>-placement.txt with
"Expected" above.

## Step 4: table, cleanup, fstrim
```sh
(
W=/var/tmp/zts-numa-1106
cat $W/passes.txt                                        # A1 B0 A2 B1 A3
sudo -n python3 -I $W/ab-table.py $W/passes.txt | tee $W/ab-table.txt
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null; grep -c '^brd ' /proc/modules   # nothing, 0
echo "loaded $(cat /sys/module/zfs/srcversion) A $(modinfo -F srcversion /var/tmp/zts-numa-1105/M/module/zfs.ko)"  # A is loaded again
du -sh $W/B0 $W/B1; df -h /var/tmp | tail -1
sudo -n fstrim -av
)
```
Keep the B0 and B1 trees (needed to rerun a pass) and every result directory.

How to read the table: one row per workload and place (Block 2, 8 jobs:
node0 = fio bound to node 0, node1 = bound to node 1, all = unbound) or per
job count (Block 3, unbound). `A` gives the mean of A1, A2 and A3 and
`drift%` = (max - min) / mean of their IOPS; `B0` is compared with the mean of
A1 and A2, `B1` with A2 and A3, in % of IOPS and of p99 (microseconds). A
difference smaller than drift% is noise. Each group also shows its own:
- `rem%`: share of DRAM loads served by the other node (run.sh's remote%);
- `hitm`: remote HITM loads, in millions per second (run.sh's hitm/s);
- `cus`: CPU time of the `z_rd_int`/`z_wr_int` threads per I/O, in
  microseconds, over the 30 measured seconds of the run (from zio-cpu.py;
  A1 has none, so A's is the mean of A2 and A3);
- `n1%`: the share of that CPU time spent on node 1.

The baseline already showed that placing fio on the far node costs about
nothing on this VM, so expect small IOPS changes. What should still move if
the change works: under A, `n1%` shows where the scheduler already runs
completions handed to a random taskq of the pool; under B1 it should follow
the issuer (near 0 at node0, near 100 at node1); under B0 it moves that way
only as far as the scheduler's wakeup placement takes it. `rem%`, `hitm` and `cus` of `uread` and
`ureadlz4` should drop with it; `cread` (ARC hits complete no I/O) should not
change. B also gives each node 6 read completion threads where A lets any
node use all 12: a lower IOPS at node0 or node1 with a lower `rem%` points at
that capacity, not at locality.

## What to expect on the real host (expectation, not measured)
The host differs from this VM in where completions start. Here brd completes
each bio inline on the CPU that submitted it, so the zio is dispatched from
the issuer's node. On the host, an NVMe completion runs on a CPU of the
interrupt vector of the issuer's hardware queue; with managed IRQ affinity
these vectors are spread over both nodes even though the Optanes sit on node
0, so an Optane completion should also start on the issuer's node, and B
should keep checksum, decompression and the ARC buffer allocation of a read
there instead of on a random node; to check with
`grep -H . /proc/irq/*/effective_affinity_list` against the nvme lines of
/proc/interrupts. The HBA and AHCI have few vectors, possibly all on node 0:
their completions would start on node 0, and B would then keep them on node 0,
the device's node, not the issuer's; those pools are disk-bound anyway.
With 44 CPUs per node the host has 8 interrupt taskqs per type (4 per node,
8 threads each). The Proxmox kernel is 7.0, so B1 there would use the
kernel's node affinity (threads free to move within their node) rather than
the one-CPU binding B1 gets on 6.12 here. Expected gain there: lower remote%
and HITM for uncached Optane reads issued on node 1 or on both nodes, more
with lz4 or encryption, and an IOPS gain probably in single digits for small
random reads, as prior art suggests; nothing for ARC hits.

## Report (under 40 lines)
- Kernel; zfs.ko srcversion of A, B0, B1 (built = loaded for each pass);
  the configure line for kthread_affine_preferred.
- One line on placement: taskq counts and thread CPUs for A2, B0, B1 against
  "Expected".
- ab-table.txt verbatim (it has the hitm, cus and n1% columns).
- Per pass: kernel log count, any failed block or unexpected line; then the
  cleanup state and the fstrim summary.

sha256 of this file without this last line and the blank line before it: 445dbbfa0c926cec7f23c47cbd14652899ae3aec463453c59aef692b80cb46ae
