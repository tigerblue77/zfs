# VM 107: userspace benchmarks on the Xeon E5-2696 v4 (no module, no pool)

These instructions are for the Claude session on VM 107. They measure, on this
machine's Broadwell CPU, three things that were measured so far only on a cloud
Xeon with forced AVX2 code paths:

- `bench_cksum`: BLAKE3 against SHA-256 and SHA-512/256 per block size, for the
  "dedup=on uses BLAKE3 on new pools" pull request;
- `bench_crypt`: the cost of the per-salt key derivation (HKDF-SHA512) on reads
  of encrypted blocks written before the last key load, and of the HMAC of
  encrypted dedup writes;
- `bench_comp`: LZ4 r85 (libzpool) against LZ4 1.10 and LZ4 HC, and zstd-1/-3.

Everything is userspace: no kernel module is loaded, no pool is created, and no
`zpool`, `zfs`, `zdb` or `zfs-tests.sh` command is run.

Rules:

- Run as `tigerblue77`, in bash. Use `sudo -n` only where written.
- Post, push and install nothing. Never reboot.
- Work only under `/var/tmp/bench-1026`, log to `/var/log/bench-1026`. Leave
  every other file, directory, process and module alone, `/etc/zfs/zpool.cache`
  included.
- A production hypervisor sits underneath: build with `nice -n 10 ionice -c3`
  and `make -j8`; run each benchmark single-threaded, pinned to one CPU, one at
  a time, never several in parallel.
- After any line that says `stop`, run nothing more and report.

## STEP 0: the VM must be free

```sh
id -un; uname -r; nproc; grep -m1 'model name' /proc/cpuinfo
grep -m1 -o -w -E 'sha_ni|avx512f|gfni|vaes' /proc/cpuinfo | sort -u | xargs echo "flags present:"
lsmod | grep -E '^(zfs|spl) ' || echo "no zfs/spl loaded"
pgrep -af 'make|configure|cc1|zfs-tests|ztest|zloop' | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)' || echo "no build or test running"
ls -d /var/tmp/bench-1026 /var/log/bench-1026 2>&1
df -BG --output=avail /var/tmp | tail -1
uptime
```

Expected: `tigerblue77`, a 6.12 kernel, `model name : Intel(R) Xeon(R) CPU
E5-2696 v4 ...`, `flags present:` with nothing after it, `no zfs/spl loaded`,
`no build or test running`, both directories absent, at least 6G free.

Stop if the user is not `tigerblue77`, a build or test runs, either directory
exists, or less than 6G is free. If the CPU is not an E5-2696 v4, or one of the
four flags is present, do not stop: record it, it changes what the numbers mean.

## STEP 1: sources

```sh
W=/var/tmp/bench-1026; L=/var/log/bench-1026
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L
mkdir -p $W && cd $W
git init -q zfs && cd zfs &&
git fetch -q https://github.com/openzfs/zfs a128b8a075b5b13c737e56914cd7043bd3e9ebf8 > $L/fetch-zfs.log 2>&1 &&
git checkout -q FETCH_HEAD && echo "zfs at $(git rev-parse --short=10 HEAD)"
mkdir -p $W/tasks && cp -r /var/tmp/vm-task-107e/bench $W/tasks/ && (cd $W/tasks && sha256sum bench/*.c)
cd /var/tmp/vm-task-107e && git fetch -q https://github.com/tigerblue77/zfs fcd1e398611a8f36737bfb8e55628135b4a7ac00 > $L/fetch-lz4.log 2>&1 &&
mkdir -p $W/lz4src && for f in lz4.c lz4.h lz4hc.c lz4hc.h; do
	git show FETCH_HEAD:module/lz4/lib/$f > $W/lz4src/$f || echo "missing $f: stop"
done; ls $W/lz4src | xargs echo "lz4src:"
```

Expected: `zfs at a128b8a075`, the sha256 lines of the benchmark sources (record them), and `lz4src:
lz4.c lz4.h lz4hc.c lz4hc.h`. `fcd1e39861` is the head of the fork branch
`lz4hc-poc`, which vendors LZ4 1.10.0.

## STEP 2: release build of libzpool and the benchmarks

A release build (no `--enable-debug`), so the figures are the ones users get.

```sh
W=/var/tmp/bench-1026; L=/var/log/bench-1026; B=$W/tasks/bench
cd $W/zfs && t0=$(date +%s) &&
nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --with-config=user && make -j8' > $L/build.log 2>&1
echo "build rc=$? wall=$(( $(date +%s) - t0 ))s"
grep -c 'warning:' $L/build.log | xargs echo "build warnings:"
F="-O2 -std=gnu99 -include zfs_config.h -include include/sys/simd_config.h -Iinclude \
 -Ilib/libspl/include -Ilib/libspl/include/os/linux -Ilib/libzpool/include -D_GNU_SOURCE \
 -D_REENTRANT -D_FILE_OFFSET_BITS=64 -UDEBUG -DNDEBUG"
Lb="-L.libs -lzpool -lnvpair -lspl -lpthread"
mkdir -p $W/bin
gcc $F -o $W/bin/bench_crypt $B/bench_crypt.c $Lb && echo "bench_crypt built"
gcc $F -o $W/bin/bench_cksum $B/bench_cksum.c $Lb && echo "bench_cksum built"
gcc -O2 -c -I$W/lz4src -o $W/bin/lz4_110.o $B/lz4_110.c && echo "lz4_110.o built"
gcc $F -o $W/bin/bench_comp $B/bench_comp.c $W/bin/lz4_110.o $Lb && echo "bench_comp built"
```

Expected: `build rc=0`, then the four `built` lines. On a failed build, quote
`grep -nE 'error:|Error [0-9]' /var/log/bench-1026/build.log | head -20` and
stop. A compile error in a benchmark: quote it and continue with the others.

## STEP 3: the corpus for bench_comp

Two files, built from what is on the VM: the ZFS sources as text, and 64 MiB of
programs.

```sh
W=/var/tmp/bench-1026; mkdir -p $W/corpus
git -C $W/zfs ls-files -z -- '*.c' '*.h' | (cd $W/zfs && xargs -0 cat) | head -c 11560856 > $W/corpus/source
find /usr/bin -maxdepth 1 -type f -size +64k -print0 | sort -z | xargs -0 cat 2>/dev/null | head -c 67108864 > $W/corpus/usrbin
ls -l $W/corpus
```

Expected: `source` of 11560856 bytes and `usrbin` of 67108864 bytes.

## STEP 4: runs

Five rounds, alternating the three benchmarks, each pinned to CPU 2 under
`nice`. Each round takes a few minutes.

```sh
W=/var/tmp/bench-1026; L=/var/log/bench-1026
cd $W/zfs && export LD_LIBRARY_PATH=$W/zfs/.libs
for r in 1 2 3 4 5; do
	echo "== round $r $(date '+%T') load $(cut -d' ' -f1 /proc/loadavg)"
	taskset -c 2 nice -n 10 $W/bin/bench_cksum > $L/cksum-$r.txt 2>&1; echo "cksum rc=$?"
	taskset -c 2 nice -n 10 $W/bin/bench_crypt avx > $L/crypt-$r.txt 2>&1; echo "crypt rc=$?"
	taskset -c 2 nice -n 10 $W/bin/bench_comp $W/corpus/source $W/corpus/usrbin > $L/comp-$r.txt 2>&1; echo "comp rc=$?"
done 2>&1 | tee $L/rounds.txt
```

Expected: every rc 0. A crash in one benchmark: keep its output file and go on.

## STEP 5: report and cleanup

Write `/var/log/bench-1026/REPORT.txt` with: STEP 0's lines (CPU model, the
flags line, load); the build rc, wall time and warning count; the sha256 lines
of STEP 1; `rounds.txt`; then, for each benchmark, all five outputs in full
(`cat` of the files, in round order). Add the deviations from these steps, if
any. Then:

```sh
rm -rf /var/tmp/bench-1026 && echo "work dir removed; logs kept in /var/log/bench-1026"
```

Reply with the whole of `REPORT.txt`.
