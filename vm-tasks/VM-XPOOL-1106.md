# VM-XPOOL-1106: cross-pool interference on 16 vCPUs, decisive variants, A/B/A

Goal: repeat on a 16-vCPU VM (Debian 13, kernel 6.12, 11-16 GiB)
the runner measurements of how much a slow RAID-Z2 pool writing flat out
slows a fast mirror pool, and how much of it the pool-isolation prototype
(each commit behind a tunable) removes.  The 4-vCPU runners exaggerate
CPU starvation; this run says what remains with 16 vCPUs.

Rules: disposable VM only (it creates null_blk/dm devices, pools named fastq,
slowq, slow1..3, and loads modules).  Release build.  No real pool may be
imported.  Use only memory-backed devices (null_blk), nothing on the VM disk.

## 1. Build and load (release)

    sudo apt-get install -y -q build-essential autoconf automake libtool gawk \
      libblkid-dev uuid-dev libudev-dev libssl-dev zlib1g-dev libaio-dev \
      libattr1-dev libelf-dev libtirpc-dev libffi-dev libcurl4-openssl-dev \
      libpam0g-dev python3 python3-dev linux-headers-$(uname -r) fio \
      bpftrace dmsetup linux-perf > /var/tmp/xpool-apt.log 2>&1
    git clone -q --branch perf-xpool-cause https://github.com/tigerblue77/zfs \
      /var/tmp/xpool-src && cd /var/tmp/xpool-src && git checkout -q 6549ab63bd
    sh autogen.sh > /var/tmp/xpool-build.log 2>&1 && ./configure \
      --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-pyzfs \
      >> /var/tmp/xpool-build.log 2>&1 && make -j16 >> /var/tmp/xpool-build.log 2>&1
    sudo make install >> /var/tmp/xpool-build.log 2>&1 && sudo depmod -a
    sudo ./scripts/zfs.sh -u; sudo ./scripts/zfs.sh -v | tail -1
    modinfo -F filename zfs; cat /sys/module/zfs/version   # .../extra/zfs.ko, 2.4.99-1

## 2. Runs (each ~45-60 min; the tunables make A/B/A inside one run)

Run 1, one slow pool (vm1, ~75 min); in the -sep phases the slow pool's threads and
load run on the first half of the CPUs, the fast pool's on the second half.
Device sizes keep the worst case under 9 GiB of RAM (VM 106 has 11 GiB):

    cd /var/tmp/xpool-src && sudo env OUT=/var/tmp/perf-xpool/vm1 \
      QV_POOLS=1 QV_SLOW_WRITE_MB=1024 SLOW_DEV_MB=768 FAST_DEV_MB=1024 \
      ARC_MAX=2147483648 \
      Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
      QV_SLOW_CPUS=0-$(($(nproc) / 2 - 1)) \
      QV_FAST_CPUS=$(($(nproc) / 2))-$(($(nproc) - 1)) \
      QV_PHASES="alone:none sw:write:diag sw-nocomp:write:comp=off \
        sw-pct25:write:pct=25 sw-w2:write:kn=w2 sw-p6:write:kn=p6 \
        sw-w2p6:write:kn=w2p6 sw-all3:write:kn=all3,diag sw-up2:write \
        alone-sep:none:sep sw-sep:write:sep alone-all3:none:kn=all3 \
        alone2:none" \
      tests/perf-xpool/run.sh qv > /var/tmp/vm1.log 2>&1; tail -1 /var/tmp/vm1.log

Run 2, three slow pools on null_blk (vm3, ~50 min), the CPU load of three HDD pools:

    cd /var/tmp/xpool-src && sudo env OUT=/var/tmp/perf-xpool/vm3 \
      QV_POOLS=3n QV3N_DEV_MB=256 QV_SLOW_WRITE_MB=256 FAST_DEV_MB=1024 \
      ARC_MAX=2147483648 \
      Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
      QV_PHASES="alone:none m3w:write:diag m3w-pct25:write:pct=25 \
        m3w-w2:write:kn=w2 m3w-w2l:write:kn=w2l m3w-w2lc:write:kn=w2lc \
        m3w-p6:write:kn=p6 m3w-all3:write:kn=all3,diag m3w-up2:write \
        alone2:none" \
      tests/perf-xpool/run.sh qv > /var/tmp/vm3.log 2>&1; tail -1 /var/tmp/vm3.log

Run 3, no slow pool (vm0, ~20 min): the cost of the write issue thread share
(p6, 25 % of the CPUs per pool) on the peak async write throughput of one
pool, zstd and lz4, 128K records, 2:1 compressible data ("IOPS" = MiB/s):

    cd /var/tmp/xpool-src && sudo env OUT=/var/tmp/perf-xpool/vm0 \
      QV_POOLS=0 QV_WL="tz tl" FAST_DEV_MB=3072 SIZE_MB=128 ARC_MAX=2147483648 \
      Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
      QV_PHASES="alone:none alone-p6:none:kn=p6 alone-w2:none:kn=w2 \
        alone-all3:none:kn=all3 alone2:none" \
      tests/perf-xpool/run.sh qv > /var/tmp/vm0.log 2>&1; tail -1 /var/tmp/vm0.log

Before runs 2 and 3, destroy the previous run's devices (section 4, without the last two
lines), since both use the same device names.  In each run, A = sw/m3w
(kn=up: master's taskqs and FIFO), B = *-w (bulk at nice 0), *-wy (bulk
threads yield between tasks), *-w2 (both) and *-all (all tunables), A again
= *-up2; -p6 (write issue threads at 25 % of the CPUs per pool, every pool),
-w2p6, -w2l (lightly dirty pools issue their async writes at high priority),
-w2lc (w2l + their sync threads at default priority) and -all3 (all
commits) are the three newest; -nocomp, -pct25 (the
slow pools only get zio_taskq_batch_pct=25) and -sep say how much of the gap
is CPU.  On the 4-vCPU runners -w2 closed 37-80 % of the p99.9 excess,
-w2p6 63-85 %, but p6 (one thread there) cost the fast pool alone 17-29 % of
its sync-write IOPS and 45 % of its compressed write throughput: at 16 vCPUs
p6 gives 4 threads, and vm0 measures what that costs.  Each
phase also has a perf sched run: its "sched delay" lines give the wakeup-to-run
delay tail of the fast pool's fio and completion threads and which threads
held the CPU meanwhile -- the line to look at on this 6.12 kernel.

## 3. Report (under 40 lines)

    python3 tests/perf-xpool/report.py qv /var/tmp/perf-xpool/vm1 > /var/tmp/vm1.txt
    python3 tests/perf-xpool/report.py qv /var/tmp/perf-xpool/vm3 > /var/tmp/vm3.txt
    python3 tests/perf-xpool/report.py qv /var/tmp/perf-xpool/vm0 > /var/tmp/vm0.txt

Send back: lscpu summary line, then one table per run (vm0: MiB/s per
phase for tz and tl) with, for rr1, rr4,
sw1, sw4: IOPS median [min..max] and p99 per phase, the "gap closed" lines,
the scheduler lines (fio and fast_int run-queue wait), and for the m* phases
the arcstats lines (size, c, anon, evict counters) and dmu_tx deltas, and
the "sched delay" lines of each phase.

## 4. Cleanup

    for p in fastq slowq slow1 slow2 slow3; do sudo zpool destroy -f $p 2>/dev/null; done
    sudo dmsetup ls | awk '/^slow/ {print $1}' | xargs -r -n1 sudo dmsetup remove
    sudo rmdir /sys/kernel/config/nullb/nullb* 2>/dev/null; sudo modprobe -r null_blk
    sudo ./scripts/zfs.sh -u; sudo rm -rf /var/tmp/perf-xpool-work /var/tmp/xpool-src
    sudo fstrim -av
