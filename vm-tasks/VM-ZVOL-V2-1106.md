# VM task 1106: zvol io_uring A/B/C (master, zvol-nowait, zvol-nowait-v2)

Goal: decide whether fork branch `zvol-nowait-v2` keeps the gains of
`zvol-nowait` without its losses, on this 16-vCPU Broadwell VM, where the
losses were measured. `zvol-nowait` (B) makes bio-based zvols take
non-blocking (REQ_NOWAIT) submissions, so io_uring submits each request
from the submitting thread instead of handing it to an io-wq worker. On
this VM that gained 12-19 % with several submitters and at QD1, but lost
14-33 % with one submitter at QD32 (4k-read 32x1 -30 to -33 %, 128k-write
32x1 -21 %, 16k-read and 16k-write 32x1 about -20 %): the single fio
thread then does all the submission work (6.1 us per 4K read, capped at
about 164k IOPS), which A's io-wq workers did in parallel. The 16k-write
loss has the same cause: an earlier pass here found B's fio never blocking
in submission, waiting only in `io_cqring_wait` while the writes waited for
a txg, and issuing about 125-145k IOPS while active against about 255k for
A, with a quarter of its time in `taskq_dispatch_ent`, mostly waking a
`zvol_tq` thread.

`zvol-nowait-v2` (C) keeps B and adds `zvol_nowait_max_inflight` (default
8): a non-blocking bio is queued by its submitter only while its zvol has
fewer than that many requests queued or in progress; deeper, it is failed
with EAGAIN and io_uring retries it from a worker, as in A. A lone request
(QD1) and a few submitters at QD1 stay inline, as in B. C also queues
REQ_NOWAIT requests to blk-mq zvols instead of failing them with EAGAIN,
which this task does not measure.

- A = `ef0614e724d42b58f7b551a5e718648264b2bd0c` (upstream master).
- B = `ad7529b1bbb16ee89e421813e04da847d8ac8fd4` (head of `zvol-nowait`).
- C = `8745eb378af7c41b81431b47c89a70abdbae56f3` (head of `zvol-nowait-v2`).
- Repository: https://github.com/tigerblue77/zfs (all three commits are on it).

On five 4-CPU GitHub runners, where the QD32 rows are CPU-bound and B
loses at most 5 % there, C kept B's QD1 and 1x8 gains but lost 4-14 %
against A on the deep rows where it hands requests back to io-wq (the
EAGAIN round trip), and on 4k-read 32x1, where B's fio thread ran 95-96 %
of the time, C sent under 3 % of the requests to io-wq: the bound counts
requests inside the zvol, and when the submitter is the bottleneck most of
its 32 requests wait on the submitter's side. So the question this task
answers is whether that also holds here, where B loses up to a third:
whether C reaches A on the one-job QD32 rows with the default of 8 or with
another value (section 4c sweeps 4, 8, 16 and 32; section 4b counts the
punts to io-wq). If no value does, the conclusion is that `zvol_nowait`
must default to 0. B's 32x8 gains are expected to go with C, as deep
queues go to io-wq there too.

## Rules

- Disposable VM only (VM 105: Debian 13, kernel 6.12, 16 vCPU, 16 GiB). This
  reloads the ZFS module stack and creates a pool on two brd RAM disks.
  Stop at once if `zpool list` shows any pool, or if a zfs module is loaded
  that you did not load, and report.
- Release builds only (no `--enable-debug`, no `--enable-debuginfo`).
- Nothing is pushed or posted anywhere. Do not claim what you did not
  measure; report what failed.
- At the end, clean up (section 6) and run `sudo fstrim -av`.

## 1. Preflight and packages

```sh
uname -r; nproc; free -g; lscpu | grep -E 'Model name|NUMA'
lsmod | grep -wE 'zfs|spl|brd' || true; zpool list 2>&1 | head -3
sudo apt-get install -y fio jq python3 linux-perf
perf --version; fio --enghelp | grep -w io_uring
sysctl kernel.io_uring_disabled 2>/dev/null        # must be 0 or absent
sudo sysctl -w kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0
```

If `brd` is loaded from an earlier task with no pool on it, keep it (the
scripts reuse `/dev/ram0` and `/dev/ram1`).

## 2. Trees A, B and C (release)

The earlier task left `$HOME/zvolab` with `src`, `A`, `B` and `ab.sh`.
Check them; rebuild what is missing.

```sh
W=$HOME/zvolab; cd $W || exit 1
git -C $W/A rev-parse HEAD   # must be ef0614e724d42b58f7b551a5e718648264b2bd0c
git -C $W/B rev-parse HEAD   # must be ad7529b1bbb16ee89e421813e04da847d8ac8fd4
ls $W/A/module/zfs.ko $W/B/module/zfs.ko
sha256sum $W/ab.sh           # must be 39100af08e1b09408e6c0bfbab722bfac9b7cf549de7228cdde607d733a18c54
```

If `$W/src` is missing, `git clone -q https://github.com/tigerblue77/zfs
$W/src`. If A or B is missing or at another commit, recreate it as section 2
of `vm-tasks/VM-ZVOL-AB-1106.md` (fork branch `vm-tasks-1106`) does. If
`ab.sh` is missing or its sha256 differs, recreate it from section 3 of that
document (its own sha256 line is
165b1dcadc12c868179b3711d071a3c8b42d5a9c2498bbd03fddd522ba6c11c2) and check
the sha256 again. Then build C:

```sh
git -C $W/src fetch -q origin zvol-nowait-v2
git -C $W/src worktree add -q --detach $W/C 8745eb378af7c41b81431b47c89a70abdbae56f3
(cd $W/C && ./autogen.sh && ./configure && make -j16) > $W/build-C.log 2>&1 \
  || { echo "build C failed"; tail -30 $W/build-C.log; }
git -C $W/C rev-parse HEAD
git -C $W/src diff --stat ad7529b1bbb16ee89e421813e04da847d8ac8fd4 8745eb378af7c41b81431b47c89a70abdbae56f3
# expect: zvol_impl.h, linux zvol_os.c, zfs.4, tunables.cfg, zvol_stress.ksh
ls $W/C/module/spl.ko $W/C/module/zfs.ko
modinfo -p $W/C/module/zfs.ko | grep zvol_nowait   # zvol_nowait, zvol_nowait_max_inflight
```

## 3. Scripts

`ab.sh` (from the earlier task) runs every row of one build once and writes
`$W/res/results.txt`. Keep the earlier results apart first:

```sh
[ -d $W/res ] && mv $W/res $W/res-ab-before-v2
```

Save the two helpers below as `$W/tsnap.py` and `$W/v2diag.sh`, then
`chmod +x $W/v2diag.sh`. `tsnap.py` snapshots every thread's
`/proc/<pid>/task/<tid>/schedstat` (time on CPU, time waiting on a run
queue) and context switches, and prints one line per row: CPU % of the
`fio`, `iou-wrk*`, `zvol_tq*`, `z_wr_iss*` and `z_rd_int*` threads over the
row, and for the busiest fio thread (the job of a one-job row) its time
running, waiting on a run queue and sleeping, with its voluntary and
involuntary switches per second. `v2diag.sh` adds `zvol_tq` thread wakeups
per task (from `/proc/spl/kstat/taskq/zvol_tq-*`), and in `diag` mode
counts io_uring punts to io-wq (`io_uring:io_uring_queue_async_work`) and
submissions per I/O over 4 s, records 3 s of the fio job thread's on-CPU
call graph, and 3 s of its switch-outs with their stacks and `prev_state`
(R: preempted while runnable; S or D: went to sleep).

```python
#!/usr/bin/env python3
# usage: tsnap.py snap OUT.json | tsnap.py line LABEL TAG PREFIX
import json, os, sys, time


def rd(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return None


def snap(out):
    th = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            b = "/proc/%s/task/%s/" % (pid, tid)
            c, s, st = rd(b + "comm"), rd(b + "schedstat"), rd(b + "status")
            if c is None or s is None or st is None:
                continue
            v = n = 0
            for x in st.splitlines():
                if x.startswith("voluntary_ctxt"):
                    v = int(x.split()[1])
                elif x.startswith("nonvoluntary_ctxt"):
                    n = int(x.split()[1])
            s = s.split()
            th[tid] = [c.strip(), int(s[0]), int(s[1]), v, n]
    json.dump({"t": time.monotonic(), "th": th}, open(out, "w"))


def grp(c):
    for p in ("iou-wrk", "zvol_tq", "z_wr_iss", "z_rd_int", "z_wr_int"):
        if c.startswith(p):
            return p
    return "fio" if c == "fio" else None


def line(label, tag, pre):
    a = json.load(open(pre + ".pre.json"))
    b = json.load(open(pre + ".post.json"))
    dt = b["t"] - a["t"]
    g, jobs = {}, []
    for tid, x in b["th"].items():
        y = a["th"].get(tid)
        if y is None or y[0] != x[0]:
            y = [x[0], 0, 0, 0, 0]
        k = grp(x[0])
        if k is None:
            continue
        d = [x[i] - y[i] for i in range(1, 5)]
        g[k] = g.get(k, 0) + d[0]
        if k == "fio" and d[0] > 0:
            jobs.append(d)
    t = open(pre + ".json").read()
    j = json.loads(t[t.index("{"):])["jobs"][0]
    io = j["write"] if j["write"]["total_ios"] else j["read"]
    pct = io["clat_ns"]["percentile"]
    out = "%s %s iops=%d lat_us=%.1f p50_us=%d p99_us=%d" % (
        label, tag, io["iops"], io["lat_ns"]["mean"] / 1000,
        pct["50.000000"] / 1000, pct["99.000000"] / 1000)
    for k in ("fio", "iou-wrk", "zvol_tq", "z_wr_iss", "z_rd_int"):
        out += " %s_cpu=%.0f%%" % (k, 100.0 * g.get(k, 0) / 1e9 / dt)
    if jobs:
        r, w, v, n = max(jobs)
        out += " job_run=%.0f%% job_rqwait=%.0f%% job_sleep=%.0f%%" \
            " job_vcs/s=%.0f job_nvcs/s=%.0f" % (
                100.0 * r / 1e9 / dt, 100.0 * w / 1e9 / dt,
                100.0 - 100.0 * (r + w) / 1e9 / dt, v / dt, n / dt)
    st = rd(pre + ".stat")
    if st:
        for x in st.splitlines():
            f = x.split(",")
            if len(f) > 2 and f[0].strip().isdigit():
                out += " %s/IO=%.2f" % (f[2].split(":")[-1].replace(
                    "io_uring_", ""), int(f[0]) / 4.0 / io["iops"])
    print(out)


if sys.argv[1] == "snap":
    snap(sys.argv[2])
else:
    line(*sys.argv[2:5])
```

```bash
#!/bin/bash
# usage: v2diag.sh <A|B|C> <label> <rows|diag> [param=value ...]
set -u
W=$HOME/zvolab; T=$W/$1; L=$2; MODE=$3; shift 3; R=$W/res2; mkdir -p $R
P=/sys/module/zfs/parameters
sudo $T/scripts/zfs.sh -u >/dev/null 2>&1
lsmod | grep -qwE '^(zfs|spl)' && { echo "zfs/spl still loaded, stop"; exit 1; }
sudo modprobe -a zlib_deflate zlib_inflate 2>/dev/null
sudo insmod $T/module/spl.ko && sudo insmod $T/module/zfs.ko || exit 1
for m in spl zfs; do
  k=$(modinfo -F srcversion $T/module/$m.ko); l=$(cat /sys/module/$m/srcversion)
  echo "$L $m srcversion tree=$k loaded=$l" | tee -a $R/srcversion.txt
  [ "$k" = "$l" ] || { echo "MISMATCH, stop"; exit 1; }
done
echo $((2<<30)) | sudo tee $P/zfs_arc_max >/dev/null
for kv in "$@"; do echo ${kv#*=} | sudo tee $P/${kv%%=*} >/dev/null || exit 1; done
echo "$L $MODE nowait=$(cat $P/zvol_nowait 2>/dev/null || echo absent)" \
  "max_inflight=$(cat $P/zvol_nowait_max_inflight 2>/dev/null || echo absent)" | tee -a $R/params.txt
lsmod | grep -qw brd || sudo modprobe brd rd_nr=2 rd_size=$((4<<20))
sudo $T/zpool create -f -o ashift=12 -O atime=off -O compression=off \
  zvolab mirror /dev/ram0 /dev/ram1 || exit 1
sudo $T/zfs create -V 3G -o volblocksize=16k -o primarycache=metadata zvolab/v || exit 1
sudo udevadm settle; D=/dev/$(basename $(ls -d /sys/block/zd* | head -1))
[ -b $D ] && [ $(sudo blockdev --getsize64 $D) -eq $((3<<30)) ] || { echo "no zvol"; exit 1; }
sudo fio --name=fill --filename=$D --rw=write --bs=1M --direct=1 --ioengine=psync \
  --size=3G > /dev/null || exit 1
tqk() { cat /proc/spl/kstat/taskq/zvol_tq-* 2>/dev/null |
  awk '$1 == "tasks_executed" {e += $3} $1 == "thread_wakeups" {w += $3} END {print e + 0, w + 0}'; }
one() { # tag rw bs qd nj [extra fio args]
  local t=$1 rw=$2 bs=$3 qd=$4 nj=$5 o=$R/$L-$1 pid tid k0 k1; shift 5
  sudo fio --name=j --filename=$D --ioengine=io_uring --rw=$rw --bs=$bs --iodepth=$qd \
    --numjobs=$nj --direct=1 --time_based --runtime=15 --norandommap \
    --randrepeat=0 --group_reporting --output-format=json --output=$o.json "$@" &
  pid=$!
  sleep 2   # thread counters are taken while fio runs: its threads end with it
  sudo python3 -I $W/tsnap.py snap $o.pre.json; k0=$(tqk)
  if [ $MODE = diag ]; then
    tid=$(ps -eLo tid=,comm=,pcpu= --sort=-pcpu | awk '$2 == "fio" {print $1; exit}')
    sudo perf stat -a -x, -o $o.stat -e io_uring:io_uring_queue_async_work \
      -e io_uring:io_uring_submit_req -- sleep 4 >/dev/null 2>&1
    sudo python3 -I $W/tsnap.py snap $o.post.json; k1=$(tqk)
    sudo perf record -q -F 4999 -g -t $tid -o $o.on.data -- sleep 3 >/dev/null 2>&1
    sudo perf record -q -e sched:sched_switch -g -t $tid -o $o.off.data \
      -- sleep 3 >/dev/null 2>&1
  else
    sleep 10
    sudo python3 -I $W/tsnap.py snap $o.post.json; k1=$(tqk)
  fi
  wait $pid
  echo "$(sudo python3 -I $W/tsnap.py line $L $t $o)" \
    "zvol_tq_wakeups/task=$(echo $k0 $k1 | awk '{printf "%.2f", ($3 == $1) ? 0 : ($4 - $2) / ($3 - $1)}')" |
    tee -a $R/results.txt
  if [ -s $o.on.data ]; then
    sudo perf report -i $o.on.data --stdio --children --sort sym -g none \
      --percent-limit 1.5 2>/dev/null | grep -v '^#' | grep . > $o.on.txt
    sudo perf script -i $o.off.data -F trace 2>/dev/null |
      grep -o 'prev_state=[A-Z+]*' | sort | uniq -c > $o.off-states.txt
    sudo perf report -i $o.off.data --stdio --no-children --sort sym \
      -g caller,0.5,callee --percent-limit 3 2>/dev/null | grep -v '^#' |
      grep . | head -120 > $o.off.txt
    sudo rm -f $o.on.data $o.off.data
  fi
}
one 4k-read-32x1 read 4k 32 1
one 16k-randread-32x1 randread 16k 32 1
one 4k-randread-1x1 randread 4k 1 1
one 4k-randread-8x1 randread 4k 8 1
one 4k-randread-1x8 randread 4k 1 8
one 4k-randread-32x8 randread 4k 32 8
one 16k-write-32x1 write 16k 32 1
sudo $T/zpool destroy zvolab
sudo $T/scripts/zfs.sh -u
```

## 4. Runs

### 4a. Six full passes, interleaved (about 17 minutes each)

```sh
chmod +x $W/ab.sh $W/v2diag.sh
$W/ab.sh A A1 && $W/ab.sh B B1 && $W/ab.sh C C1 && \
  $W/ab.sh C C2 && $W/ab.sh B B2 && $W/ab.sh A A2
grep -c . $W/res/results.txt      # 6 passes x 61 lines
```

### 4b. Diagnostic pass, one per build (about 3 minutes each)

```sh
$W/v2diag.sh A dA diag && $W/v2diag.sh B dB diag && $W/v2diag.sh C dC diag
```

Each pass leaves, per row `<r>`, `$W/res2/<label>-<r>.on.txt` (fio job
thread, inclusive % per function), `.off-states.txt` and `.off.txt` (its
switch-outs), and its line in `$W/res2/results.txt`.

### 4c. Threshold sweep on C, `rows` mode (about 3 minutes each)

```sh
$W/v2diag.sh A sA rows && $W/v2diag.sh B sB rows && \
  $W/v2diag.sh C sC8 rows && \
  $W/v2diag.sh C sC4 rows zvol_nowait_max_inflight=4 && \
  $W/v2diag.sh C sC16 rows zvol_nowait_max_inflight=16 && \
  $W/v2diag.sh C sC32 rows zvol_nowait_max_inflight=32 && \
  $W/v2diag.sh A sA2 rows
```

## 5. Report (in this order, under 70 lines)

1. CPU model, kernel, `git rev-parse` of A, B, C, every srcversion line
   (each pass must match its tree), and the `params.txt` lines (B and C:
   nowait=1; in `res2`, C: max_inflight=8, then 4, 16, 32).
2. From 4a, a table with A = mean of A1 and A2, B = mean of B1, B2, C = mean
   of C1, C2; B and C as % change against A; and the same-code spreads
   A1/A2, B1/B2, C1/C2 in %. A change smaller than the larger spread of the
   two builds compared is "no change".

   | row | IOPS A / B / C | B % | C % | lat us A / B / C | CPU us/IO A / B / C | ctx/IO A / B / C | zvol_tq s A / B / C | A1/A2 B1/B2 C1/C2 % |

   rows, io_uring: 4k-read 32x1, 16k-read 32x1, 16k-randread 32x1,
   16k-write 32x1, 16k-randwrite 32x1, 128k-write 32x1, 4k-randread 1x1,
   1x8, 32x1, 32x8, 4k-randwrite 1x1, 32x8, 128k-write 32x8, 128k-read 1x1;
   osync-4k-randwrite 1x1 and 32x8; arc-4k-randread 1x1 and 32x8.
3. Every other io_uring row where C differs from A or from B by more than
   twice the spread (one line each: row, A, B, C, %), and every psync or
   libaio row where B or C moves beyond its spread against A (they should
   not: those submitters do not set REQ_NOWAIT).
4. From 4b, per build and row, the `results.txt` line as printed (IOPS,
   latencies, thread CPU %, job run/rqwait/sleep %, switches/s, punts and
   submissions per I/O, wakeups per task). For C, punts per I/O say how
   often the bound sent a request to io-wq. Then, for B and C at
   4k-read-32x1 and 16k-write-32x1, the 15 largest inclusive entries of
   `.on.txt` (name and %), and the entries for `zvol_submit_bio`,
   `zvol_request_impl`, `taskq_dispatch_ent`,
   `__wake_up`, `try_to_wake_up`, `submit_bio_noacct`,
   `__blkdev_direct_IO_async`, `bio_iov_iter_get_pages`, `io_issue_sqe`,
   `io_wq_enqueue`, `io_cqring_wait` and any `*send*IPI*` symbol (0 when
   absent); for A the same list. For 16k-write-32x1 in A, B and C:
   `.off-states.txt` as is, and the three largest stacks of `.off.txt`
   (the functions from `schedule` down to the syscall or kernel thread
   entry, one line each).
5. From 4c, a table: the rows of 4c by IOPS for sA, sB, sC4, sC8, sC16,
   sC32, sA2, with B and each C as % against the mean of sA and sA2, and the
   sA/sA2 spread.
6. Anything that failed or looked wrong (a mismatch, a fio error, a missing
   perf event or kstat). The full results stay on the VM (section 6).

## 6. Clean up

```sh
zpool list 2>&1 | grep -q zvolab && sudo $W/C/zpool destroy zvolab
sudo $W/A/scripts/zfs.sh -u; sudo rmmod brd
tar czf $HOME/zvolab-v2-res-1106.tgz -C $W res res2
sudo fstrim -av
```

Keep `$W` (the trees and scripts) unless the disk is short: a later task may
reuse it. If it has to go, `rm -rf $W` after the tar.

sha256 of every line above (check: head -n -2 VM-ZVOL-V2-1106.md | sha256sum): 4e027a5ae37c9984432b299492ecad3f82eed2c7ae7fe31d7291db8cbe5ed9a5
