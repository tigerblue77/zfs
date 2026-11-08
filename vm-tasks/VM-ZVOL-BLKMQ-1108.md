# VM task: REQ_NOWAIT requests to blk-mq zvols on Linux 6.12, master against the fix (8/11)

These instructions are for one 16-vCPU test VM: VM 105, VM 106 or VM 107,
whichever is free, as long as it runs Debian 13 with a 6.12 kernel, without
`slub_debug` (STEP 0 checks). Reply with
`/var/log/zvol-blkmq-1108/REPORT.txt` (STEP 5). Nothing is posted, pushed or
installed, apart from `fio` if it is missing.

**Background.** A blk-mq zvol advertises non-blocking submissions, as every
blk-mq queue does (`BLK_FEAT_NOWAIT` is set by `blk_mq_alloc_queue()` on Linux
6.11 and later), whatever the zvol parameters. io_uring therefore issues its
reads and writes to it as `REQ_NOWAIT` requests from the submitting task, and
`zvol_mq_queue_rq()` runs in that task (or in kblockd, or in another task that
runs the hardware queue). On master, with `zvol_request_sync=1` or
`volthreading=off`, `zvol_request_impl()` then processes each request in
place, and an io_uring submitter waits for each of its requests in turn. With
the default settings such requests are queued to the `zvol_tq` taskq.

Commit `dbd66096d1` ("zvol: queue REQ_NOWAIT requests to blk-mq zvols", fork
branch `zvol-blkmq-nowait`) queues them in those settings too. It does not
fail them with `EAGAIN` instead, because `queue_rq()` may run outside the
submitting task, and io_uring before Linux 6.14 only retries an `EAGAIN` that
completes in the submitting task. In those settings, requests without
`REQ_NOWAIT` (psync, libaio, io_uring's own workers) are still processed in
place.

It was measured only on 4-CPU GitHub runners with kernel 7.0: with
`zvol_use_blk_mq=1` and `zvol_request_sync=1`, against `zvol_use_blk_mq=1`
alone, one io_uring job at QD32 lost 69-70 % of its 4K random read IOPS and
64-65 % of its O_SYNC 4K random write IOPS on master, and moved by -2.2 to
+3.7 % and -2.7 to +1.5 % with the commit. At QD1 the same setting gained
36-40 % and 12-16 % on master by processing in place, and moved by -2.5 to
+1.9 % and +0.1 to +1.1 % with the commit. This task checks the same on Linux
6.12, the kernels the commit's reasoning is about, and on this VM's 16 vCPUs;
adds `volthreading=off`, eight submitters and two controls; and checks that no
request fails (no `EAGAIN` or `EIO` seen by fio) and that the kernel logs
nothing.

**What is compared.**

| Tree | Commit | What it is |
| --- | --- | --- |
| `A` | `2796b5da69ab267f96479f6e09c00613d628d450` | upstream master |
| `B` | `dbd66096d17bf7801a669664789e9f4e8767df3a` | A plus the commit (`blkdev_compat.h`, `zvol_os.c`, `zfs.4`) |

| Arm | Module parameters and zvol property, all with `zvol_use_blk_mq=1` |
| --- | --- |
| `i` | defaults: `zvol_request_sync=0`, `volthreading=on` |
| `ii` | `zvol_request_sync=1`, `volthreading=on` |
| `iii` | `zvol_request_sync=0`, `volthreading=off` |

**The setup** is the runners': a pool mirroring two 3.5 GiB brd RAM disks
(`ashift=12`, `compression=off`, `atime=off`), one 2 GiB zvol created after
`zvol_use_blk_mq=1` is set (`volblocksize=16K`, `primarycache=metadata`),
filled once and read once, `zfs_arc_max` 2 GiB. Release builds.

**The rows**, fio on the zvol, `O_DIRECT`, 4K random I/O, 15 s each:

| Row | Engine | Jobs x QD | |
| --- | --- | --- | --- |
| `uring-4k-randread-32x1` | io_uring | 1 x 32 | runners' row |
| `uring-4k-randread-1x1` | io_uring | 1 x 1 | runners' row |
| `uring-4k-randread-32x8` | io_uring | 8 x 32 | several submitters |
| `uring-4k-randread-1x8` | io_uring | 8 x 1 | several submitters at QD1 |
| `psync-4k-randread-1x1` | psync | 1 x 1 | control: no `REQ_NOWAIT` |
| `libaio-4k-randread-32x1` | libaio | 1 x 32 | control: no `REQ_NOWAIT` |
| `osync-4k-randwrite-32x1` | io_uring, `O_SYNC` | 1 x 32 | runners' row |
| `osync-4k-randwrite-1x1` | io_uring, `O_SYNC` | 1 x 1 | runners' row |

Per row: IOPS, mean and p99 latency, CPU time of the whole VM per I/O,
context switches per I/O, `zvol_tq` tasks executed per I/O (about 1: the
request was queued to the taskq; about 0: it was processed where `queue_rq()`
ran), fio's exit status, fio's job error and the error lines fio printed.

**The passes**: A1, B1, B2, A2, in that order. Each pass loads one build,
checks that the loaded modules are that tree's, creates a fresh pool and
zvol, and runs the three arms on that zvol, switching the parameter and the
property between them: i, ii, iii in A1 and B1, then iii, ii, i in B2 and A2.
So an arm is compared with arm i of the same module load and pool, and each
build is measured twice, first and last for A.

**Duration**: STEP 0 2 min; STEP 1 about 10 min (two release builds of about
3.5 min each with `make -j8` on VM 105); STEP 2 1 min; STEP 3 about 30 min
(four passes of about 7 min: 24 runs of 15 s and the setup); STEP 4 and 5
about 5 min. In all about 50 minutes, under an hour.

## Rules

- Disposable VM only, no other build, test or benchmark running. This task
  loads and unloads the ZFS module stack and `brd`, and creates one pool,
  `zvolmq`, on the two brd RAM disks only: nothing on the VM's disks, no
  other pool.
- Release builds (no `--enable-debug`, no `--enable-debuginfo`), under
  `nice -n 10 ionice -c3` with `make -j8`, one tree at a time.
- Post nothing, push nothing. Install nothing (no `make install`, no
  `zfs-tests.sh`, no `zfs-helpers.sh`), except `fio` with
  `sudo -n apt-get install -y fio` if STEP 0 finds it missing. Never reboot.
- Work only under `/var/tmp/zvol-blkmq-1108` (W) and
  `/var/log/zvol-blkmq-1108` (L), on `/dev/ram0`, `/dev/ram1` and the pool
  `zvolmq`. Never run `zpool`, `zfs` or `zdb` while none of this task's
  modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A `STOP` line of `run.sh`, a panic, oops,
  `VERIFY`, `ASSERT`, `BUG:`, `WARNING` or hung-task message: stop and
  report. After a stop past STEP 0, go to STEP 4 with what you have.
- A fio error (`EAGAIN`, `EIO` or another) does not stop the pass: it is
  recorded in `fio-errors.txt` and `results.csv`, and it is a finding.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must have the right shape and be free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"
uname -r; nproc; cat /proc/cmdline
grep -m1 '^model name' /proc/cpuinfo; lscpu | grep -E '^NUMA node\(s\)'
grep -E 'MemTotal|MemAvailable' /proc/meminfo
ls -d /lib/modules/$(uname -r)/build
for t in fio python3 git make gcc autoconf automake libtool udevadm; do command -v $t > /dev/null || echo "missing $t"; done
fio --version; fio --enghelp 2>&1 | grep -cwE 'io_uring|libaio|psync'   # 3
sysctl -n kernel.io_uring_disabled 2>&1                       # 0
modinfo -n brd                                                # a path ending in brd.ko*
grep -E '^(zfs|spl|brd) ' /proc/modules                       # nothing (brd: see below)
ls /lib/modules/$(uname -r)/extra 2>&1 | grep -E 'zfs|spl'    # nothing (no installed ZFS)
command -v zpool zfs zdb zed                                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/zvol-blkmq-1108 /var/log/zvol-blkmq-1108 2>&1  # No such file or directory, twice
df -h /var/tmp | tail -1                                      # 5G free or more
```

Stop, changing nothing, if: `sudo -n true` fails; `uname -r` does not start
with `6.12.`; `nproc` is not 16; `/proc/cmdline` contains `slub_debug`;
MemAvailable is under 10 GiB; the kernel headers or a tool other than `fio`
are missing; fio lacks one of the three engines; `io_uring_disabled` is not 0;
`brd` is not a module; a zfs or spl module is loaded; a ZFS is installed; a
build, test or benchmark runs; W or L exists; less than 5G is free on
`/var/tmp`. If only `fio` is missing, install it
(`sudo -n apt-get install -y fio`) and run the two fio lines again. A `brd`
module left loaded by an earlier task is fine: `run.sh` unloads it, and stops
if it is in use.

## STEP 1: sources and release builds (about 10 min)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zvol-blkmq-1108 L=/var/log/zvol-blkmq-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W $L/raw || exit 1
cat > $L/env.sh <<EOF2
export PATH=/usr/sbin:/sbin:\$PATH
W=$W L=$L POOL=zvolmq VOL_MB=2048 DEV_MB=3584 RT=15
EOF2
{
git init -q $W/repo && git -C $W/repo fetch -q --depth 3 https://github.com/tigerblue77/zfs dbd66096d17bf7801a669664789e9f4e8767df3a &&
git -C $W/repo worktree add -q --detach $W/A 2796b5da69ab267f96479f6e09c00613d628d450 &&
git -C $W/repo worktree add -q --detach $W/B dbd66096d17bf7801a669664789e9f4e8767df3a || echo "fetch failed: stop"
for n in A B; do git -C $W/$n log -1 --format="$n %H %s"; done
git -C $W/repo diff --stat 2796b5da69ab267f96479f6e09c00613d628d450 dbd66096d17bf7801a669664789e9f4e8767df3a | tail -1
for n in A B; do printf '%s io_is_nowait calls: ' $n; grep -c 'io_is_nowait(bio, rq)' $W/$n/module/os/linux/zfs/zvol_os.c; done
for n in A B; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure && make -j8' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    grep -c '^#define ZFS_DEBUG 1' $W/$n/zfs_config.h | xargs echo "$n ZFS_DEBUG defines:"
    modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1 | xargs echo "$n built for:"
    for m in spl zfs; do modinfo -F srcversion $W/$n/module/$m.ko | xargs echo "$n $m.ko srcversion:"; done
done
} 2>&1 | tee $L/step1.txt
```

Expected: `A 2796b5da69ab267f96479f6e09c00613d628d450 zfs_znode (Linux):
Optimize hold insertion`, `B dbd66096d17bf7801a669664789e9f4e8767df3a zvol:
queue REQ_NOWAIT requests to blk-mq zvols`; `3 files changed, 30
insertions(+), 2 deletions(-)`; `io_is_nowait calls:` 0 for A and 1 for B;
`build rc=0` twice; `ZFS_DEBUG defines: 1` twice (a release build; a debug
build of these trees prints 3); both built for `uname -r`; two different
`zfs.ko` srcversions. On a failed build quote `grep -nE 'error:|Error
[0-9]' $L/build-<n>.log | head -20` and stop.

## STEP 2: the scripts (1 min)

`run.sh` runs one pass of one build: it unloads and reloads `brd`, loads the
build's `spl.ko` and `zfs.ko` with `insmod` and stops unless their
srcversions are the tree's, sets `zfs_arc_max` and `zvol_use_blk_mq=1`,
creates the pool and the zvol, stops unless the zvol is a blk-mq device and
accepts an `RWF_NOWAIT` read, fills and warms it, then runs the eight rows in
each arm and destroys and unloads everything. It writes kernel log markers
(`zvol-blkmq-1108: pass <P> start|end`) and stops on a kernel warning since
its start marker. `line.py` turns a fio JSON output and the counters into one
line of `results.csv`; `summary.py` makes the tables of STEP 4.

```sh
. /var/log/zvol-blkmq-1108/env.sh
cat > $L/run.sh <<'EOF2'
#!/bin/bash
# run.sh <A|B> <A1|B1|B2|A2>: one pass of one build. Loads its modules,
# creates the pool and the blk-mq zvol, runs the rows of the three arms,
# then destroys the pool and unloads everything.
. /var/log/zvol-blkmq-1108/env.sh
set -u
T=$1 PASS=$2 Z=$W/$1 P=/sys/module/zfs/parameters
case $PASS in A1|B1) ARMS="i ii iii" ;; A2|B2) ARMS="iii ii i" ;; *) exit 1 ;; esac
stop() { echo "STOP $PASS: $*" | tee -a $L/stops.txt; exit 1; }
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat; }
ctxt() { awk '/^ctxt /{print $2}' /proc/stat; }
tqx() { sudo -n cat /proc/spl/kstat/taskq/zvol_tq-* 2>/dev/null |
    awk '$1 == "tasks_executed" { s += $3 } END { print s + 0 }'; }
echo "zvol-blkmq-1108: pass $PASS start" | sudo -n tee /dev/kmsg > /dev/null
grep -qE '^(zfs|spl) ' /proc/modules && stop "zfs or spl already loaded"
grep -q '^brd ' /proc/modules && { sudo -n rmmod brd || stop "brd in use"; }
sudo -n modprobe brd rd_nr=2 rd_size=$((DEV_MB * 1024)) || stop "modprobe brd"
sudo -n modprobe -a zlib_deflate zlib_inflate 2> /dev/null
sudo -n insmod $Z/module/spl.ko && sudo -n insmod $Z/module/zfs.ko || stop "insmod"
for m in spl zfs; do
    k=$(modinfo -F srcversion $Z/module/$m.ko) l=$(cat /sys/module/$m/srcversion)
    echo "$PASS $m srcversion tree=$k loaded=$l" | tee -a $L/srcversion.txt
    [ "$k" = "$l" ] || stop "$m srcversion mismatch"
done
echo $((2 << 30)) | sudo -n tee $P/zfs_arc_max > /dev/null || stop "zfs_arc_max"
echo 1 | sudo -n tee $P/zvol_use_blk_mq > /dev/null || stop "zvol_use_blk_mq"
echo $PASS $(for p in zvol_use_blk_mq zvol_request_sync zvol_threads \
    zvol_num_taskqs zvol_blk_mq_queue_depth zvol_blk_mq_blocks_per_thread \
    zfs_arc_max; do echo $p=$(cat $P/$p); done) | tee -a $L/params.txt
sudo -n $Z/zpool create -f -o ashift=12 -o cachefile=none -O atime=off \
    -O compression=off $POOL mirror /dev/ram0 /dev/ram1 || stop "zpool create"
sudo -n $Z/zfs create -V ${VOL_MB}M -o volblocksize=16k \
    -o primarycache=metadata $POOL/v || stop "zfs create"
sudo -n udevadm settle
Zd=$(basename "$(ls -d /sys/block/zd* 2> /dev/null | head -1)") D=/dev/$Zd
[ -b "$D" ] && [ "$(sudo -n blockdev --getsize64 $D)" -eq $((VOL_MB << 20)) ] ||
    stop "no zvol"
[ -d /sys/block/$Zd/mq ] || stop "$D is not a blk-mq device"
echo "$PASS $D blk-mq hw_queues=$(ls /sys/block/$Zd/mq | wc -l)" \
    "nr_requests=$(cat /sys/block/$Zd/queue/nr_requests)" | tee -a $L/params.txt
sudo -n python3 -I -c '
import mmap, os, sys
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECT)
try:
    print("RWF_NOWAIT read:", os.preadv(fd, [mmap.mmap(-1, 4096)], 0, os.RWF_NOWAIT))
except OSError as e:
    print("RWF_NOWAIT read:", e.strerror)
' $D | sed "s/^/$PASS /" | tee -a $L/params.txt
grep -q 'not supported' $L/params.txt && stop "the zvol refuses RWF_NOWAIT"
sudo -n fio --name=fill --filename=$D --rw=write --bs=1M --direct=1 \
    --ioengine=psync --size=${VOL_MB}M > /dev/null || stop "fill"
sudo -n $Z/zpool sync $POOL
sudo -n fio --name=w --filename=$D --ioengine=io_uring --rw=read --bs=1M \
    --iodepth=8 --direct=1 > /dev/null || stop "warm-up"
sudo -n fio --name=w --filename=$D --ioengine=io_uring --rw=randread --bs=4k \
    --iodepth=32 --direct=1 --time_based --runtime=3 --norandommap \
    > /dev/null || stop "warm-up"
[ "$(tqx)" -gt 0 ] || stop "zvol_tq kstats unreadable"
arm() { # i: defaults; ii: zvol_request_sync=1; iii: volthreading=off
    local rs=0 vt=on
    case $1 in ii) rs=1 ;; iii) vt=off ;; esac
    sudo -n $Z/zpool sync $POOL
    echo $rs | sudo -n tee $P/zvol_request_sync > /dev/null &&
        sudo -n $Z/zfs set volthreading=$vt $POOL/v || stop "arm $1"
    echo "$PASS $(date +%T) arm=$1 zvol_use_blk_mq=$(cat $P/zvol_use_blk_mq)" \
        "zvol_request_sync=$(cat $P/zvol_request_sync)" \
        "volthreading=$(sudo -n $Z/zfs get -H -o value volthreading $POOL/v)" |
        tee -a $L/params.txt
}
row() { # arm tag engine rw qd jobs [fio args]: one fio run, one CSV line
    local a=$1 t=$2 e=$3 rw=$4 qd=$5 nj=$6 o b0 c0 q0 b1 c1 q1 rc el
    shift 6
    o=$L/raw/$PASS-$a-$t
    b0=$(busy) c0=$(ctxt) q0=$(tqx)
    sudo -n fio --name=j --filename=$D --ioengine=$e --rw=$rw --bs=4k \
        --iodepth=$qd --numjobs=$nj --direct=1 --time_based --runtime=$RT \
        --norandommap --randrepeat=0 --group_reporting --output-format=json \
        --output=$o.json "$@" 2> $o.err
    rc=$?
    b1=$(busy) c1=$(ctxt) q1=$(tqx)
    el=$(grep -ciE 'error|unavailable|again' $o.err)
    [ "$rc$el" = 00 ] || { echo "== $PASS $a $t rc=$rc"; cat $o.err; } >> $L/fio-errors.txt
    python3 -I $L/line.py $o.json $PASS $T $a $t $rc $el $((b1 - b0)) \
        $((c1 - c0)) $((q1 - q0)) | tee -a $L/results.csv
}
for a in $ARMS; do
    arm $a
    row $a uring-4k-randread-32x1 io_uring randread 32 1
    row $a uring-4k-randread-1x1 io_uring randread 1 1
    row $a uring-4k-randread-32x8 io_uring randread 32 8
    row $a uring-4k-randread-1x8 io_uring randread 1 8
    row $a psync-4k-randread-1x1 psync randread 1 1
    row $a libaio-4k-randread-32x1 libaio randread 32 1
    row $a osync-4k-randwrite-32x1 io_uring randwrite 32 1 --sync=1
    row $a osync-4k-randwrite-1x1 io_uring randwrite 1 1 --sync=1
done
sudo -n $Z/zpool destroy $POOL || stop "zpool destroy"
sudo -n $Z/scripts/zfs.sh -u || stop "unload"
sudo -n rmmod brd || stop "rmmod brd"
echo "zvol-blkmq-1108: pass $PASS end" | sudo -n tee /dev/kmsg > /dev/null
sudo -n dmesg | sed -n "/zvol-blkmq-1108: pass $PASS start/,\$p" |
    grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more|WARNING|I/O error' &&
    stop "kernel message"
exit 0
EOF2
cat > $L/line.py <<'EOF2'
#!/usr/bin/env python3
# line.py JSON PASS BUILD ARM ROW RC ERRLINES BUSY_TICKS CTXSW TQ_TASKS:
# one CSV line of results.csv from a fio JSON output and the counters.
import json
import os
import sys

f, ps, b, arm, row, rc, el, busy, cs, tq = sys.argv[1:11]
hz = os.sysconf("SC_CLK_TCK")
try:
    t = open(f).read()
    j = json.loads(t[t.index("{"):])["jobs"][0]
    io = j["write"] if j["write"]["total_ios"] else j["read"]
    n = io["total_ios"]
    p99 = io["clat_ns"].get("percentile", {}).get("99.000000", 0) / 1000
    v = [io["iops"], io["lat_ns"]["mean"] / 1000, p99,
         int(busy) / hz * 1e6 / max(n, 1), int(cs) / max(n, 1),
         int(tq) / max(n, 1)]
    print("%s,%s,%s,%s,%.0f,%.1f,%.0f,%.1f,%.2f,%.2f,%d,%s,%d,%s" % (
        ps, b, arm, row, *v, n, rc, j["error"], el))
except (OSError, ValueError, KeyError, IndexError) as e:
    print("%s,%s,%s,%s,0,0,0,0,0,0,0,%s,-1,%s" % (ps, b, arm, row, rc, el))
    print("line.py: %s: %r" % (f, e), file=sys.stderr)
EOF2
cat > $L/summary.py <<'EOF2'
# summary.py results.csv: the tables of STEP 4 (IOPS per pass, arms against
# arm i, B against A, per-I/O counters, lines with an error).
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
R = []
for x in rows:
    if x["row"] not in R:
        R.append(x["row"])
by = {(x["pass"], x["arm"], x["row"]): x for x in rows}
def v(p, a, r, f="iops"):
    x = by.get((p, a, r))
    return float(x[f]) if x else None
def pct(n, d):
    return "%+6.1f" % (100.0 * (n / d - 1)) if n and d else "     -"
def mean(p1, p2, a, r, f="iops"):
    s = [y for y in (v(p1, a, r, f), v(p2, a, r, f)) if y is not None]
    return sum(s) / len(s) if s else None
def spread(p1, p2, a, r):
    x, y = v(p1, a, r), v(p2, a, r)
    return 100.0 * abs(x - y) / ((x + y) / 2) if x and y else None
W = max(len(r) for r in R) if R else 10
P = ("A1", "A2", "B1", "B2")
print("== 1. IOPS per pass")
print("%-*s arm %8s %8s %8s %8s" % (W, "row", "A1", "B1", "B2", "A2"))
for r in R:
    for a in ("i", "ii", "iii"):
        print("%-*s %-3s" % (W, r, a) + "".join(" %8.0f" % (v(p, a, r) or 0)
              for p in ("A1", "B1", "B2", "A2")))
print("\n== 2. Arms ii and iii against arm i of the same pass, IOPS %")
print("%-*s %27s | %27s" % (W, "", "arm ii / arm i", "arm iii / arm i"))
print("%-*s" % (W, "row") + " %6s %6s %6s %6s |" % P + " %6s %6s %6s %6s" % P)
for r in R:
    print("%-*s" % (W, r) + "".join(" %s" % pct(v(p, "ii", r), v(p, "i", r))
          for p in P) + " |" +
          "".join(" %s" % pct(v(p, "iii", r), v(p, "i", r)) for p in P))
print("\n== 3. B against A, IOPS, mean of two passes each (~: change under the larger spread)")
print("%-*s arm %8s %8s %6s %6s %6s" % (W, "row", "A", "B", "B/A%", "A1/A2%", "B1/B2%"))
for r in R:
    for a in ("i", "ii", "iii"):
        ma, mb = mean("A1", "A2", a, r), mean("B1", "B2", a, r)
        sa, sb = spread("A1", "A2", a, r), spread("B1", "B2", a, r)
        flag = ""
        if ma and mb and sa is not None and sb is not None:
            flag = " ~" if abs(100.0 * (mb / ma - 1)) < max(sa, sb) else ""
        print("%-*s %-3s %8.0f %8.0f %s %6.1f %6.1f%s" % (W, r, a, ma or 0,
              mb or 0, pct(mb, ma), sa or 0, sb or 0, flag))
print("\n== 4. Mean of two passes, A / B: zvol_tq tasks per I/O, CPU us per I/O,")
print("context switches per I/O, mean latency us, p99 latency us")
for r in R:
    for a in ("i", "ii", "iii"):
        out = "%-*s %-3s" % (W, r, a)
        for f, fmt in (("tq_io", "%.2f"), ("cpu_us_io", "%.1f"), ("ctx_io", "%.2f"),
                       ("lat_us", "%.1f"), ("p99_us", "%.0f")):
            out += "  " + "/".join(fmt % (mean(p1, p2, a, r, f) or 0)
                                   for p1, p2 in (("A1", "A2"), ("B1", "B2")))
        print(out)
bad = [x for x in rows if x["rc"] != "0" or x["fio_err"] != "0" or x["err_lines"] != "0"]
print("\n== 5. %d lines; %d with a fio exit status, fio error or error message" % (len(rows), len(bad)))
for x in bad:
    print("%s %s %s rc=%s fio_err=%s err_lines=%s ios=%s" % (x["pass"], x["arm"],
          x["row"], x["rc"], x["fio_err"], x["err_lines"], x["ios"]))
EOF2
chmod +x $L/run.sh
sha256sum $L/run.sh $L/line.py $L/summary.py
echo "pass,build,arm,row,iops,lat_us,p99_us,cpu_us_io,ctx_io,tq_io,ios,rc,fio_err,err_lines" > $L/results.csv
```

Expected sha256:

```
9d00d464fbdb3cfea821e497ac6c84a7af26433e5c68586c1883c4683c7a0a4c  run.sh
510d5c50530bc855cb2c947ae7afd756215c41003a7f39fe8585646a91ff0a36  line.py
a550c615854966fb8ca53fd20835bdb0aa574d29bd6a9fcc5198b7bebbca5332  summary.py
```

## STEP 3: the passes (about 30 min)

One command; it stops at the first pass that fails.

```sh
. /var/log/zvol-blkmq-1108/env.sh
for p in A1 B1 B2 A2; do
    $L/run.sh ${p%?} $p || break
    echo "pass $p done $(date +%T)"
done 2>&1 | tee $L/passes.txt
wc -l < $L/results.csv                     # 97: the header and 4 passes x 3 arms x 8 rows
cat $L/stops.txt 2>/dev/null               # nothing
cat $L/fio-errors.txt 2>/dev/null          # nothing
grep -c srcversion $L/srcversion.txt       # 8, each line with tree= equal to loaded=
```

## STEP 4: summary, kernel messages, cleanup (about 3 min)

```sh
. /var/log/zvol-blkmq-1108/env.sh
python3 -I $L/summary.py $L/results.csv | tee $L/summary.txt
sudo -n dmesg | sed -n '/zvol-blkmq-1108: pass A1 start/,$p' > $L/kmsg.txt
grep -c 'zvol-blkmq-1108: pass' $L/kmsg.txt           # 8: a start and an end per pass
grep -c 'DEBUG mode' $L/kmsg.txt                      # 0 (a debug build says so at load)
grep -vE 'zvol-blkmq-1108: pass|ZFS: (Loaded|Unloaded) module|brd: module loaded' $L/kmsg.txt  # nothing expected: report any line
if grep -q '^zfs ' /proc/modules; then
    sudo -n $W/A/zpool list -H -o name 2>/dev/null | grep -qx zvolmq && sudo -n $W/A/zpool destroy zvolmq
    sudo -n $W/A/scripts/zfs.sh -u
fi
grep -q '^brd ' /proc/modules && sudo -n rmmod brd
grep -E '^(zfs|spl|brd) ' /proc/modules               # nothing
rm -rf $W && echo "work dir removed; logs and scripts kept in $L"
sudo -n fstrim -av
```

A pass that stopped may leave B's module loaded: A's `zpool` and `zfs.sh`
work with it (the commit changes no ioctl and no user space).

## STEP 5: REPORT.txt

Write `/var/log/zvol-blkmq-1108/REPORT.txt` with, in this order:

1. A verdict, one line each, for the five points of "Reading the result"
   below, each with the figures it rests on.
2. STEP 0 output; `step1.txt`; the sha256 lines of STEP 2.
3. `srcversion.txt`, `params.txt` and `passes.txt`, whole.
4. `summary.txt`, whole.
5. `results.csv`, whole.
6. `fio-errors.txt` and `stops.txt`, whole, or "none".
7. STEP 4 output, with every line of `kmsg.txt` other than the markers and
   the module load and unload lines (or "none").
8. Every deviation from this document, and anything that looked wrong.

Reply with the whole of `REPORT.txt`. The fio JSON outputs stay in `$L/raw`.

## Reading the result

The runners' figures are from kernel 7.0 on 4 CPUs; this VM may differ in
size, not in sign.

1. **Where requests are processed** (summary, section 4, `zvol_tq` tasks per
   I/O). Arm i: about 1 on every row, A and B. Arms ii and iii: about 0 on
   every row for A; for B, about 1 on the six io_uring rows and about 0 on
   psync and libaio. Anything else means the arms do not measure what they
   claim.
2. **The fix** (section 2). A, arms ii and iii against arm i, one job at
   QD32: far below arm i (runners, arm ii: -69 to -70 % for reads, -64 to
   -65 % for O_SYNC writes). B: within a few percent of arm i (runners: -2.7
   to +3.7 %). The 8-job QD32 row was not measured before: report it as it
   comes.
3. **The trade-off** (section 2). A, arms ii and iii, QD1 rows: above arm i
   (runners, arm ii: +36 to +40 % for reads, +12 to +16 % for O_SYNC
   writes). B: at arm i's level. The 8-job QD1 row was not measured before.
4. **No change where none is expected** (section 3). Arm i: B against A
   within the larger same-code spread on every row. psync and libaio rows:
   the same in every arm.
5. **No failure.** Section 5 lists no line; `fio-errors.txt` and
   `stops.txt` do not exist; no kernel message other than the markers and
   module lines.

A contradiction to any point (for example B's QD32 rows in arm ii or iii
still far below arm i, a B row in arm ii or iii with about 0 tasks per I/O on
io_uring, or any `EAGAIN` or `EIO`) is the result to report first.
