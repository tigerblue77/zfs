# VM-XPOOL-1106: cross-pool interference on 16 vCPUs, decisive variants, A/B/A

Goal: repeat on a 16-vCPU, 2-node NUMA VM (Debian 13, kernel 6.12, 16 GiB)
the runner measurements of how much a slow RAID-Z2 pool writing flat out
slows a fast mirror pool, and how much of it the pool-isolation prototype
(4 commits, each behind a tunable) removes.  The 4-vCPU runners exaggerate
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
      /var/tmp/xpool-src && cd /var/tmp/xpool-src && git checkout -q 1c62a4067e
    sh autogen.sh > /var/tmp/xpool-build.log 2>&1 && ./configure \
      --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-pyzfs \
      >> /var/tmp/xpool-build.log 2>&1 && make -j16 >> /var/tmp/xpool-build.log 2>&1
    sudo make install >> /var/tmp/xpool-build.log 2>&1 && sudo depmod -a
    sudo ./scripts/zfs.sh -u; sudo ./scripts/zfs.sh -v | tail -1
    cat /sys/module/zfs/version   # must name the build, 2.4.99-1 + 1c62a4067e

## 2. Runs (each ~45-60 min; the tunables make A/B/A inside one run)

Run 1, one slow pool (vm1), slow pool's threads and load on NUMA node 0 in
the -sep phases, the fast pool's on node 1:

    cd /var/tmp/xpool-src && sudo env OUT=/var/tmp/perf-xpool/vm1 \
      QV_POOLS=1 QV_SLOW_WRITE_MB=1024 ARC_MAX=2147483648 \
      Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
      QV_SLOW_CPUS=$(cat /sys/devices/system/node/node0/cpulist) \
      QV_FAST_CPUS=$(cat /sys/devices/system/node/node1/cpulist) \
      QV_PHASES="alone:none sw:write:diag sw-nocomp:write:comp=off \
        sw-w:write:kn=w sw-all:write:kn=all sw-up2:write \
        alone-sep:none:sep sw-sep:write:sep alone-all:none:kn=all alone2:none" \
      tests/perf-xpool/run.sh qv > /var/tmp/vm1.log 2>&1; tail -1 /var/tmp/vm1.log

Run 2, three slow pools on null_blk (vm3), the CPU load of three HDD pools:

    cd /var/tmp/xpool-src && sudo env OUT=/var/tmp/perf-xpool/vm3 \
      QV_POOLS=3n QV_SLOW_WRITE_MB=256 FAST_DEV_MB=1024 ARC_MAX=2147483648 \
      Q2_DIRTY_MAX=1610612736 REPS=3 Q2_RUNTIME=20 DIAG_RUNTIME=20 \
      QV_PHASES="alone:none m3w:write:diag m3w-nocomp:write:comp=off \
        m3w-w:write:kn=w m3w-all:write:kn=all m3w-up2:write alone2:none" \
      tests/perf-xpool/run.sh qv > /var/tmp/vm3.log 2>&1; tail -1 /var/tmp/vm3.log

Before run 2, destroy run 1's devices (section 4, without the last two
lines), since both use the same device names.  In each run, A = sw/m3w
(kn=up: master's taskqs and FIFO), B = *-w and *-all (prototype),
A again = *-up2; the -nocomp and -sep phases say how much of the gap is CPU.

## 3. Report (under 40 lines)

    python3 tests/perf-xpool/report.py qv /var/tmp/perf-xpool/vm1 > /var/tmp/vm1.txt
    python3 tests/perf-xpool/report.py qv /var/tmp/perf-xpool/vm3 > /var/tmp/vm3.txt

Send back: lscpu summary line, then one table per run with, for rr1, rr4,
sw1, sw4: IOPS median [min..max] and p99 per phase, the "gap closed" lines,
the scheduler lines (fio and fast_int run-queue wait), and for the m* phases
the arcstats lines (size, c, anon, evict counters) and dmu_tx deltas.

## 4. Cleanup

    for p in fastq slowq slow1 slow2 slow3; do sudo zpool destroy -f $p 2>/dev/null; done
    sudo dmsetup ls | awk '/^slow/ {print $1}' | xargs -r -n1 sudo dmsetup remove
    sudo rmdir /sys/kernel/config/nullb/nullb* 2>/dev/null; sudo modprobe -r null_blk
    sudo ./scripts/zfs.sh -u; sudo rm -rf /var/tmp/perf-xpool-work /var/tmp/xpool-src
    sudo fstrim -av
