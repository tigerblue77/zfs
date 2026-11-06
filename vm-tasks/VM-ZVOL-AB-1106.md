# VM task 1106: kernel A/B of the zvol io_uring path (REQ_NOWAIT, inline, plug)

Goal: measure, in the kernel of a 16-vCPU NUMA VM, what the changes on fork
branch `zvol-inline-io` save per zvol I/O. Every io_uring read or write on
a bio-based zvol is issued from an io_uring worker thread (the zvol does not
take non-blocking submissions), and master then hands it to a `zvol_tq`
thread: two thread hops per request. This is the path of Proxmox VMs on
zvols (QEMU `aio=io_uring`, the default for zfspool storage).

- A = `ef35f144e1d0df22e835970dc9bc5615f4d1df48` (upstream master).
- B = `5ed46819b6d89840a72ebd474cedbba3c9ef354c` (head of `zvol-inline-io`:
  A plus five commits).
- Repository: https://github.com/tigerblue77/zfs (both commits are on it).
- B1, B's defaults: `zvol_nowait=1`, the zvol takes non-blocking
  submissions, so io_uring submits from fio's own thread and the zvol
  queues to its taskq (one hop instead of two).
- B1i, `zvol_nowait=0 zvol_inline_io_uring=131072`: io_uring goes through
  its workers again, and a worker processes requests of up to 128 KiB
  itself (one hop).
- B also stops vdev bios from waiting in the caller's block plug
  (`vdev_disk.c`), which only matters where a zvol request is processed
  in its submitter's thread (`volthreading=off`, `zvol_request_sync=1`,
  B1i) or with `zfs_vdev_disk_calling_thread_io=1`: the optional passes
  A1v/B1v and A1c/B1c measure that.

On GitHub runners (4 vCPU, null_blk, two EPYC 9V74 and one 9V45), B1 gave
+18..+23 % IOPS on io_uring QD1 4K reads and +7..+13 % on sync writes,
-9..+5 % at QD32, with psync unchanged; B1i +6..+10 % at QD1 but down to
-14 % at QD32 with 4 jobs. On VM 105 (this CPU, master, psync, null_blk,
1 vdev) a QD1 4K zvol read took 121 us against 70 us for a file, and 86.5
us with zvol_request_sync=1, which however raised the QD32 read p99 from
1.3 to 9.0 ms. Expected here: the same signs; 128K rows move less; psync
and libaio rows are controls and should not move in B1.

## Rules

- Disposable VM only (Debian 13, kernel 6.12, 16 vCPU, 16 GiB). This
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
numactl -H 2>/dev/null | head -4 || true
lsmod | grep -wE 'zfs|spl|brd' || true; zpool list 2>&1 | head -3
sudo apt-get update
sudo apt-get install -y build-essential autoconf automake libtool gawk \
  libblkid-dev uuid-dev libudev-dev libssl-dev zlib1g-dev libaio-dev \
  libattr1-dev libelf-dev linux-headers-$(uname -r) python3 python3-dev \
  libtirpc-dev libffi-dev libcurl4-openssl-dev fio jq numactl
fio --enghelp | grep -wE 'io_uring|libaio|psync'   # all three needed
sysctl kernel.io_uring_disabled 2>/dev/null        # must be 0 or absent
```

## 2. Build A and B (kernel and user space, release)

```sh
W=$HOME/zvolab; mkdir -p $W && cd $W
git clone -q https://github.com/tigerblue77/zfs src
git -C src fetch -q origin zvol-inline-io
git -C src worktree add -q --detach $W/A ef35f144e1d0df22e835970dc9bc5615f4d1df48
git -C src worktree add -q --detach $W/B 5ed46819b6d89840a72ebd474cedbba3c9ef354c
for t in A B; do
  (cd $W/$t && ./autogen.sh && ./configure && make -j16) > $W/build-$t.log 2>&1 \
    || { echo "build $t failed"; tail -30 $W/build-$t.log; }
done
git -C $W/A rev-parse HEAD; git -C $W/B rev-parse HEAD
git -C $W/src diff --stat ef35f144e1d0df22e835970dc9bc5615f4d1df48 5ed46819b6d89840a72ebd474cedbba3c9ef354c
# expect: blkdev_compat.h, zvol_impl.h, freebsd and linux zvol_os.c,
# zvol.c, zfs.4, tunables.cfg, zvol_stress.ksh
ls $W/A/module/spl.ko $W/A/module/zfs.ko $W/B/module/spl.ko $W/B/module/zfs.ko
```

## 3. Bench script

Save as `$W/ab.sh`. It loads one build with insmod from its tree, checks
that the loaded modules are that tree's (srcversion), creates a mirror of
two brd disks with one 3 GiB zvol (volblocksize 16K, the Proxmox default),
fills it, and runs every job once (A/B/A gives the repetitions).

```bash
#!/bin/bash
# usage: ab.sh <A|B> <label> [param=value ...]   e.g. ab.sh B B1i zvol_nowait=0
set -u
W=$HOME/zvolab; T=$W/$1; L=$2; shift 2; R=$W/res; mkdir -p $R
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
echo "$L inline_io_uring=$(cat $P/zvol_inline_io_uring 2>/dev/null || echo absent)" \
  "nowait=$(cat $P/zvol_nowait 2>/dev/null || echo absent)" \
  "request_sync=$(cat $P/zvol_request_sync) blk_mq=$(cat $P/zvol_use_blk_mq)" \
  "calling_thread_io=$(cat $P/zfs_vdev_disk_calling_thread_io)" | tee -a $R/params.txt
lsmod | grep -qw brd || sudo modprobe brd rd_nr=2 rd_size=$((4<<20))
sudo $T/zpool create -f -o ashift=12 -O atime=off -O compression=off \
  zvolab mirror /dev/ram0 /dev/ram1 || exit 1
sudo $T/zfs create -V 3G -o volblocksize=16k -o primarycache=metadata ${VOLPROPS:-} \
  zvolab/v || exit 1
echo "$L volthreading=$(sudo $T/zfs get -H -o value volthreading zvolab/v)" | tee -a $R/params.txt
sudo udevadm settle; D=/dev/$(basename $(ls -d /sys/block/zd* | head -1))
[ -b $D ] && [ $(sudo blockdev --getsize64 $D) -eq $((3<<30)) ] || { echo "no zvol"; exit 1; }
sudo fio --name=fill --filename=$D --rw=write --bs=1M --direct=1 --ioengine=psync \
  --size=3G > /dev/null || exit 1
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat; }
ctxt() { awk '/^ctxt/{print $2}' /proc/stat; }
tqcpu() { ps -eLo comm=,cputimes= | awk '$1 ~ /^zvol_tq/ {s += $2} END {print s+0}'; }
HZ=$(getconf CLK_TCK)
run() { # tag engine rw bs qd nj [extra fio args]
  local t=$1 e=$2 rw=$3 bs=$4 qd=$5 nj=$6; shift 6
  local o=$R/$L-$t.json b0 b1 c0 c1 q0 q1 k
  b0=$(busy); c0=$(ctxt); q0=$(tqcpu)
  (sleep 8; ps -eLo comm= | grep -c '^iou-wrk' > $R/$L-$t.iouwrk) &
  sudo fio --name=j --filename=$D --ioengine=$e --rw=$rw --bs=$bs --iodepth=$qd \
    --numjobs=$nj --direct=1 --time_based --runtime=15 --norandommap \
    --randrepeat=0 --group_reporting --output-format=json --output=$o "$@"
  b1=$(busy); c1=$(ctxt); q1=$(tqcpu)
  k=read; [ "${rw%write}" != "$rw" ] && k=write
  jq -r --arg L $L --arg t $t --argjson cpu $((b1-b0)) --argjson cs $((c1-c0)) \
    --argjson tq $((q1-q0)) --argjson hz $HZ ".jobs[0].$k |
    \"\(\$L) \(\$t) iops=\(.iops|floor) lat_us=\(.lat_ns.mean/1000*10|floor/10)\" +
    \" p99_us=\(.clat_ns.percentile[\"99.000000\"]/1000|floor)\" +
    \" cpu_us_io=\(\$cpu/\$hz*1e6/.total_ios*100|floor/100)\" +
    \" ctx_io=\(\$cs/.total_ios*100|floor/100) zvol_tq_cpu_s=\(\$tq)\"" $o |
    sed "s/\$/ iou_wrk=$(cat $R/$L-$t.iouwrk 2>/dev/null)/" |
    tee -a $R/results.txt
}
b0=$(busy); sleep 10; echo "$L idle_jiffies_per_s=$(( ($(busy) - b0) / 10 ))" | tee -a $R/results.txt
for bs in ${SIZES:-4k 16k 128k}; do
  for rw in randread randwrite read write; do
    for qn in 1:1 1:8 32:1 32:8; do
      run uring-$bs-$rw-${qn/:/x} io_uring $rw $bs ${qn%:*} ${qn#*:}
    done
  done
done
for rw in randread randwrite; do        # controls: no change expected
  for nj in 1 8; do
    run psync-4k-$rw-1x$nj psync $rw 4k 1 $nj
    run libaio-4k-$rw-32x$nj libaio $rw 4k 32 $nj
  done
done
for qn in 1:1 32:8; do                  # ZIL path: O_SYNC 4K writes
  run osync-4k-randwrite-${qn/:/x} io_uring randwrite 4k ${qn%:*} ${qn#*:} --sync=1
done
sudo $T/zfs set primarycache=all zvolab/v   # ARC hits: the hop is most of the cost
sudo fio --name=warm --filename=$D --rw=read --bs=1M --direct=1 --size=1G \
  --loops=2 --ioengine=psync > /dev/null
for qn in 1:1 32:8; do
  run arc-4k-randread-${qn/:/x} io_uring randread 4k ${qn%:*} ${qn#*:} --size=1G
done
sudo $T/zpool destroy zvolab
sudo $T/scripts/zfs.sh -u
```

`cpu_us_io` is busy jiffies of all CPUs (user, nice, system, irq, softirq,
steal) per I/O, idle included (the idle rate is printed once per pass).
`ctx_io` is context switches per I/O. `zvol_tq_cpu_s` is the CPU seconds the
`zvol_tq` threads used during the job: near 0 on the 4K and 16K io_uring rows
in B1i means the inline path was taken (it stays high in A and B1).
`iou_wrk` is the number of io_uring worker threads alive 8 s into the job:
0 on io_uring rows in B1 means io_uring submitted without its workers.

## 4. Run A/B/A (and the optional in-build control)

```sh
chmod +x $W/ab.sh
$W/ab.sh A A1 && $W/ab.sh B B1 && \
  $W/ab.sh B B1i zvol_nowait=0 zvol_inline_io_uring=131072 && $W/ab.sh A A2
# optional, 4K rows only, in this order, if time allows:
export SIZES=4k
VOLPROPS="-o volthreading=off" $W/ab.sh A A1v     # master, inline in submitter
VOLPROPS="-o volthreading=off" $W/ab.sh B B1v zvol_nowait=0  # same, plug fix
$W/ab.sh A A1c zfs_vdev_disk_calling_thread_io=1
$W/ab.sh B B1c zfs_vdev_disk_calling_thread_io=1 zvol_nowait=0
$W/ab.sh B B0 zvol_nowait=0                       # should match A
unset SIZES
```

A full pass takes about 17 minutes, a 4K-only pass about 8. Optional, one profile each, only if time
allows: during `uring-4k-randread-1x1` on A and on B,
`sudo perf record -a -g -o $W/res/perf-$L.data -- sleep 10`, then
`sudo perf report -i ... --stdio --no-children --sort comm | head -15`;
report the share of `zvol_tq-*` and `iou-wrk-*` in A and B.

## 5. Report (under 40 lines, in this order)

1. VM CPU model, NUMA nodes, kernel, the srcversion lines (every pass
   must match its tree), `params.txt`, and the `iou_wrk` value of
   uring-4k-randread-1x1 in A1, B1 and B1i.
2. A table of the rows below, A = mean of A1 and A2, B1 and B1i with their
   change in %, and the A1/A2 difference in % as the noise floor (a change
   smaller than the floor is "no change"):

   | row | IOPS A / B1 / B1i | mean lat us A / B1 / B1i | CPU us/IO A / B1 / B1i | ctx/IO A / B1 / B1i | zvol_tq s A / B1 / B1i | A1/A2 % |

   rows: uring-4k-randread 1x1, 1x8, 32x1, 32x8; uring-4k-randwrite 1x1,
   32x1, 32x8; uring-16k-randread 1x1, 32x8; uring-128k-read 1x1, 32x8;
   uring-128k-write 32x8; osync-4k-randwrite 1x1, 32x8; arc-4k-randread
   1x1, 32x8.
3. Controls: one line each for psync and libaio, the largest B1-vs-A and
   B1i-vs-A change (IOPS and CPU), and whether it exceeds the noise floor.
4. Any other io_uring row whose change exceeds twice the noise floor, in
   B1 or B1i.
5. Optional passes, 4K rows only: B1v vs A1v and B1c vs A1c (the plug
   fix), A1v vs A (volthreading=off on master, the no-code option), and
   B0 vs A: one line each with the largest IOPS and latency changes, and
   for A1v vs A the psync, libaio QD32 and io_uring QD32 reads.
6. The full `results.txt` stays on the VM at `$W/res/` (do not paste it).

## 6. Clean up

```sh
zpool list 2>&1 | grep -q zvolab && sudo $W/B/zpool destroy zvolab
sudo $W/A/scripts/zfs.sh -u; sudo rmmod brd
tar czf $HOME/zvolab-res-1106.tgz -C $W res
rm -rf $W
sudo fstrim -av
```

sha256 of every line above (check: head -n -2 VM-ZVOL-AB-1106.md | sha256sum): 165b1dcadc12c868179b3711d071a3c8b42d5a9c2498bbd03fddd522ba6c11c2
