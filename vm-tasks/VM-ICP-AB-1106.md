# VM task 1106: kernel A/B of the ICP AES-GCM direct-output change

Goal: measure, in the kernel, what the change on fork branch
`icp-gcm-direct-output` saves. In user space the change removed a
per-call `vmem_alloc()` of 32 KiB plus a copy of the ciphertext, and cut
the encrypt time by 7-22 % at 4K-512K (`crypto_test -p`, GitHub runners).
In user space `vmem_alloc()` is `malloc()`; in the kernel it is
`spl_vmem_alloc()` (kvmalloc), so the kernel gain is unknown. The real
target host is a Broadwell Xeon (AES-NI, PCLMULQDQ, AVX2, MOVBE, no VAES,
no AVX-512), where "fastest" picks the `avx` GCM code: that pass matters
most.

- A = `131faa02905d32bfbe6d58358e09d7ab3f72b812` (the base: master
  8d46e23a10 plus the two commits of #19722).
- B = `643cff649757d35d2206c58a3917087b33633e91` (A plus the change,
  head of `icp-gcm-direct-output`).
- Repository: https://github.com/tigerblue77/zfs (both commits are on it).

Only encryption (writes) goes through the changed code. Reads decrypt
through unchanged code: they are the control and should not move.

## Rules

- Disposable VM only (Debian 13, kernel 6.12, 16 vCPU, 16 GiB). This
  reloads the ZFS module stack and creates a pool on a brd RAM disk.
  Stop at once if `zpool list` shows any pool, or if a zfs module is
  loaded that you did not load, and report.
- Nothing is pushed or posted anywhere. Builds in RELEASE mode (no
  `--enable-debug`, no `--enable-debuginfo`).
- Do not claim what you did not measure; report what failed.

## 1. Preflight and packages

```sh
uname -r; nproc; free -g; grep -o -m1 -wE 'aes|pclmulqdq|avx2|movbe|vaes|avx512f' /proc/cpuinfo | sort -u
lsmod | grep -wE 'zfs|spl|brd' || true
sudo apt-get update
sudo apt-get install -y build-essential autoconf automake libtool gawk \
  alien fakeroot dkms libblkid-dev uuid-dev libudev-dev libssl-dev \
  zlib1g-dev libaio-dev libattr1-dev libelf-dev linux-headers-$(uname -r) \
  python3 python3-dev python3-setuptools python3-cffi libffi-dev \
  python3-packaging libtirpc-dev libcurl4-openssl-dev fio jq linux-perf
```

The `avx` GCM code needs aes, pclmulqdq and avx; if the VM lacks one,
report it and skip that pass.

## 2. Build A and B (kernel and user space, release)

```sh
W=$HOME/icpab; mkdir -p $W && cd $W
git clone -q https://github.com/tigerblue77/zfs src
git -C src fetch -q origin icp-gcm-direct-output
git -C src worktree add -q --detach $W/A 131faa02905d32bfbe6d58358e09d7ab3f72b812
git -C src worktree add -q --detach $W/B 643cff649757d35d2206c58a3917087b33633e91
for t in A B; do
  (cd $W/$t && ./autogen.sh && ./configure && make -j16) > $W/build-$t.log 2>&1 \
    || { echo "build $t failed"; tail -30 $W/build-$t.log; }
done
git -C $W/A rev-parse HEAD; git -C $W/B rev-parse HEAD
git -C $W/src diff --stat 131faa0290 643cff6497   # expect only module/icp/algs/modes/gcm.c
```

## 3. Bench script

Save as `$W/ab.sh`. It loads one build, checks that the loaded module is
that build's, creates the pool, and runs every job 3 times.

```bash
#!/bin/bash
# usage: ab.sh <A|B> <label>   e.g. ab.sh A A1
set -u
W=$HOME/icpab; T=$W/$1; L=$2; R=$W/res; mkdir -p $R
sudo $T/scripts/zfs.sh -u >/dev/null 2>&1
sudo $T/scripts/zfs.sh || exit 1
for m in spl zfs; do
  k=$(modinfo -F srcversion $T/module/$m.ko); l=$(cat /sys/module/$m/srcversion)
  echo "$L $m srcversion tree=$k loaded=$l" | tee -a $R/srcversion.txt
  [ "$k" = "$l" ] || { echo "MISMATCH, stop"; exit 1; }
done
sudo $T/zfs version | tee -a $R/srcversion.txt
echo $((2<<30)) | sudo tee /sys/module/zfs/parameters/zfs_arc_max >/dev/null
lsmod | grep -qw brd || sudo modprobe brd rd_nr=1 rd_size=$((8<<20))
[ -f $W/key ] || head -c 32 /dev/urandom > $W/key
sudo $T/zpool create -f -O atime=off -O compression=off icpab /dev/ram0 || exit 1
P=/sys/module/zfs/parameters
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat; }
for impl in fastest avx pclmulqdq; do
  aes=fastest; [ $impl = pclmulqdq ] && aes=aesni
  echo $aes | sudo tee $P/icp_aes_impl >/dev/null
  echo $impl | sudo tee $P/icp_gcm_impl >/dev/null || { echo "$L $impl unsupported"; continue; }
  echo "$L $impl gcm=[$(cat $P/icp_gcm_impl)] aes=[$(cat $P/icp_aes_impl)]" | tee -a $R/impl.txt
  for bs in 128k 16k 4k; do
    sudo $T/zfs create -o recordsize=$bs -o primarycache=metadata \
      -o encryption=aes-256-gcm -o keyformat=raw -o keylocation=file://$W/key icpab/d
    for rw in write read randwrite randread; do
      for rep in 1 2 3; do
        o=$R/$L-$impl-$bs-$rw-$rep.json
        b0=$(busy); t0=$(date +%s.%N)
        sudo fio --name=j --directory=/icpab/d --rw=$rw --bs=$bs --size=768m \
          --numjobs=4 --ioengine=psync --direct=0 --end_fsync=1 \
          --io_size=$([ ${rw#rand} != $rw ] && echo 512m || echo 768m) \
          --group_reporting --output-format=json --output=$o
        sudo $T/zpool sync icpab
        t1=$(date +%s.%N); b1=$(busy)
        k=$([ ${rw%write} != $rw ] && echo write || echo read)
        bytes=$(jq ".jobs[0].$k.io_bytes" $o)
        awk -v L=$L -v i=$impl -v s=$bs -v w=$rw -v r=$rep -v by=$bytes \
          -v t0=$t0 -v t1=$t1 -v c=$(( b1 - b0 )) -v hz=$(getconf CLK_TCK) \
          'BEGIN{g=by/2^30; printf "%s %s %s %s %d MiBps=%.0f cpu_s_per_GiB=%.3f\n",
            L,i,s,w,r,by/2^20/(t1-t0),c/hz/g}' | tee -a $R/results.txt
      done
    done
    sudo $T/zfs destroy icpab/d
  done
done
sudo $T/zpool destroy icpab
sudo $T/scripts/zfs.sh -u
```

Wall time, CPU time and bytes are measured around fio plus the final
`zpool sync`, so the encryption done at txg sync is counted. CPU is all
CPUs, busy jiffies (user, nice, system, irq, softirq, steal).

## 4. Run A/B/A

```sh
chmod +x $W/ab.sh
$W/ab.sh A A1 && $W/ab.sh B B1 && $W/ab.sh A A2
```

Optional, one profile each, only if time allows: during a `fastest`,
128k `write` job on A and on B, `sudo perf record -a -g -o $W/res/perf-$L.data -- sleep 10`,
then `sudo perf report -i ... --stdio --no-children --sort symbol | head -25`;
report the share of `memcpy`/`crypto_uio_copy_to_data`/`spl_kvmalloc`
in A and B.

## 5. Report (under 40 lines, in this order)

1. VM CPU model and flags, `fastest` GCM impl the module picked, the
   srcversion lines (A1, B1, A2 must match their trees).
2. One table for the encrypt path, one row per impl x bs:

   | impl | bs | write MiB/s A->B | write CPU s/GiB A->B | randwrite MiB/s A->B | randwrite CPU s/GiB A->B | A1/A2 spread |

   A = mean of the medians of A1 and A2 (median of the 3 reps each), B =
   median of B1; give the B-vs-A change in %, and the A1/A2 difference in
   % as the noise floor. A change smaller than that floor is "no change".
3. Reads (control): one line, the largest B-vs-A change seen on read and
   randread, throughput and CPU.
4. Anything that failed or was skipped, and the perf shares if taken.

Keep the raw files (`$W/res/`) until the report is written, then clean
up: `sudo $W/A/zpool destroy icpab` if it still exists,
`sudo $W/A/scripts/zfs.sh -u`, `sudo rmmod brd`, delete `$W`, and run
`sudo fstrim -av`.

sha256 of the lines above (this file without this line and the blank one before it): b76f1860d53e12c8930e018b37d375ad5d520c2b9135d4b14e3c06147d4f8975
