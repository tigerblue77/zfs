# VM: kernel A/B of encrypted dedup writes, HMAC-SHA512 against keyed BLAKE3

These instructions are for the session on the disposable test VM they are
given to. They measure, in the kernel, what the fork branch
`dedup-iv-blake3-rb1102` of https://github.com/tigerblue77/zfs changes in the
cost of writing to an encrypted dataset with dedup on, and reply with
`REPORT.txt`. Nothing is posted, pushed or installed.

| Tree | Commit | What it is |
| --- | --- | --- |
| `M` | `2232cabb2a11839f5ac4ac46221cc84e3660bff7` | upstream master `0cdf531257` plus the HMAC key pointer fix (fork branch `zio-crypt-hmac-key-data`), the branch's parent |
| `B` | `8d201c42799b0fd8ef5a460d21f3374750201e41` | the head of `dedup-iv-blake3-rb1102`: M plus "Derive the salt and IV of encrypted dedup blocks with keyed BLAKE3" and its ZTS test |

**What B changes.** An encrypted dedup block needs the same salt and IV for the
same plaintext, so ZFS derives them from it with a keyed PRF: HMAC-SHA512 on
every release. On B, a DSL Crypto Key created while the new module parameter
`zfs_crypt_dedup_blake3` is 1 (its default) on a pool with `feature@blake3`
enabled records `DSL_CRYPTO_DEDUP_PRF = 1` in its ZAP object, and its dedup
blocks use keyed BLAKE3 instead. A key created with the parameter at 0, and
every key on M, records nothing and uses HMAC-SHA512. The parameter is read
only when a key is created (`zfs create -o encryption=...`).

**Three arms**, each on a fresh pool and a fresh key per run:

| Arm | Module | `zfs_crypt_dedup_blake3` at `zfs create` | PRF |
| --- | --- | --- | --- |
| `M` | M | (no such parameter) | HMAC-SHA512 |
| `B0` | B | 0 | HMAC-SHA512 |
| `B1` | B | 1 | keyed BLAKE3 |

`B1` against `B0` is the effect of the PRF alone, in one binary. `B0` against
`M` must show no difference: it is the check that B costs nothing to keys that
keep HMAC-SHA512, which includes every existing key.

**The workload.** `dd` of a 2 GiB file of random bytes, held in a tmpfs, into
an encrypted (`aes-256-gcm`) dataset with `compression=off`, then `zpool
sync`, on a pool whose only vdev is a loop device over a file in a tmpfs, so
that the run is CPU-bound and no disk is involved. Random data makes every
block unique, so every block goes through the PRF, AES-GCM, the dedup
checksum and a new DDT entry. Six configurations: `recordsize` 16K, 128K and
1M, times `dedup=sha256` and `dedup=blake3` (the dedup checksum is a separate
cost from the PRF, and decides what share of the write the PRF is). Five
rounds, alternating the trees between rounds and the arms inside B.

Two figures per run: the wall time from the start of `dd` to the end of
`zpool sync`, as MiB/s, and the busy CPU time of the whole VM over the same
interval (`user+nice+system+irq+softirq` of the `cpu` line of `/proc/stat`), as
CPU seconds per GiB. The second is the one that matters on a machine whose CPUs
have other work; the first is what one writer sees.

There is no `conv=fsync` and no `fsync` anywhere: blocks written by
`dmu_sync()` for the ZIL are never deduplicated (`dmu_write_policy()` turns
dedup off for `WP_DMU_SYNC`), so an fsync would move blocks out of the code
being measured.

## Rules for the whole run

- Run everything as root, in bash, in this order. Every block starts by
  sourcing `/var/log/dediv-1102/env.sh` or by setting what it needs.
- A production hypervisor may sit underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, one tree at a time. The measured
  runs run with nothing else on the VM: no build, no test, no other benchmark.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  never reboot, never run `scripts/zfs-helpers.sh` or `zfs-tests.sh`.
- Work only under `/var/tmp/dediv-1102` (W) and `/var/log/dediv-1102` (L),
  plus the tmpfs this task mounts at `$W/ram`, the loop device it attaches, and
  the pool `dediv` it creates. Never touch `/etc/zfs/zpool.cache` yourself.
- Never run a `zpool`, `zfs` or `zdb` command while none of this task's modules
  is loaded: libzfs would `modprobe` whatever zfs module the system has.
- If a check does not print what its comment or the Expected lines say, stop
  and report. Do not rebuild, repair or retry. A kernel panic, oops, `VERIFY`,
  `ASSERT`, `BUG:` or hung-task message means stop and report; never reboot.
  After a stop past STEP 0, go to STEP 6 with what you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/dediv-1102 L=/var/log/dediv-1102
date -u '+%F %T'; id -un; uname -r; nproc
grep -m1 '^model name' /proc/cpuinfo
for f in sha_ni avx2 avx512f avx512vl vaes; do
    grep -qw "$f" /proc/cpuinfo && printf '%s=yes ' $f || printf '%s=no ' $f
done; echo
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '                  # nothing
ls /dev/zfs 2>&1                                # No such file or directory
command -v zpool zfs zdb zed                    # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|configure|make|gcc|cc1|fio|bench' | grep -v pgrep
ls -d $W $L 2>&1                                # No such file or directory, twice
losetup -a; grep -E ' zfs |dediv' /proc/mounts  # nothing, nothing
ls -l /etc/zfs/zpool.cache 2>&1                 # absent, or 0 bytes
free -g | sed -n 2p                             # "available" must be 12 or more
df -h /var/tmp                                  # 10G free
command -v git dd fio perf losetup sha256sum awk   # git dd losetup sha256sum awk required
uptime
```

Stop, changing nothing, if: you are not root; the kernel headers are missing;
a zfs or spl module is loaded or a system ZFS is installed; a build, test or
benchmark runs; `$W` or `$L` exists; a loop device or zfs mount exists;
`zpool.cache` is not empty; less than 12 GiB of memory is available or less
than 10G is free on `/var/tmp`. `fio` and `perf` are optional: record whether
they exist. Record the CPU flags line: it decides which SHA-512 and BLAKE3
code the kernel runs, and so what the numbers mean.

## STEP 1: sources and release builds

```sh
W=/var/tmp/dediv-1102 L=/var/log/dediv-1102
mkdir -p $W $L && cat > $L/env.sh <<EOF
export PATH=/usr/sbin:/sbin:\$PATH
W=$W L=$L POOL=dediv SIZE_MB=2048 ROUNDS=5
EOF
. $L/env.sh
for t in M:2232cabb2a11839f5ac4ac46221cc84e3660bff7 B:8d201c42799b0fd8ef5a460d21f3374750201e41; do
    n=${t%%:*} c=${t#*:}
    git init -q $W/$n && git -C $W/$n fetch -q https://github.com/tigerblue77/zfs $c &&
    git -C $W/$n checkout -q FETCH_HEAD && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
git -C $W/B diff --stat $(git -C $W/M rev-parse HEAD) HEAD | tail -1   # 14 files changed, 490 insertions(+), 36 deletions(-)
for n in M B; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure && make -j8' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    grep -c '^#define ZFS_DEBUG 1' $W/$n/zfs_config.h | xargs echo "$n ZFS_DEBUG defines:"   # 1 (3 is a debug build)
    grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$n.log | xargs echo "$n compiler warnings:"
    modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1 | xargs echo "$n built for:"   # $(uname -r)
done
```

Expected: both commits, the diff stat, `build rc=0` twice, `ZFS_DEBUG
defines: 1` twice (a release build keeps only the line under `#ifdef
CONFIG_ZFS_DEBUG`; `--enable-debug` writes 3; neither tree is configured with
`--enable-debuginfo`, which adds `-fno-inline`), and both modules built for
the running kernel. On a failed build quote
`grep -nE 'error:|Error [0-9]' $L/build-<n>.log | head -20` and stop.

## STEP 2: the RAM disk, the source file and the run script

```sh
. /var/log/dediv-1102/env.sh
mkdir -p $W/ram $W/mnt && mount -t tmpfs -o size=$((SIZE_MB * 2 + 2048))M dediv-ram $W/ram
head -c ${SIZE_MB}M /dev/urandom > $W/ram/src && sha256sum $W/ram/src | tee $L/src.sha256
truncate -s $((SIZE_MB + 1536))M $W/ram/vdev
LOOP=$(losetup -f --show $W/ram/vdev) && echo "LOOP=$LOOP" >> $L/env.sh && echo "loop $LOOP"
head -c 32 /dev/urandom > $W/key && chmod 600 $W/key
cat > $W/run1.sh <<'EOF'
#!/bin/bash
# run1.sh <round> <arm> <recordsize> <dedup>: one measured write, one CSV line
. /var/log/dediv-1102/env.sh
r=$1 arm=$2 rs=$3 dd=$4
case $arm in M) tree=M ;; *) tree=B ;; esac
Z=$W/$tree
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8}' /proc/stat; }
$Z/zpool create -f -o ashift=12 -O compression=off -O atime=off \
    -O recordsize=$rs -O dedup=$dd -O mountpoint=$W/mnt $POOL $LOOP || exit 1
case $arm in
B0) echo 0 > /sys/module/zfs/parameters/zfs_crypt_dedup_blake3 ;;
B1) echo 1 > /sys/module/zfs/parameters/zfs_crypt_dedup_blake3 ;;
esac
$Z/zfs create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$W/key $POOL/enc || exit 1
$Z/zpool sync $POOL
prf=$($Z/zdb -dddd $POOL 2>/dev/null | grep -c 'DSL_CRYPTO_DEDUP_PRF = 1')
c0=$(busy); t0=$(date +%s.%N)
dd if=$W/ram/src of=$W/mnt/enc/f bs=1M status=none || exit 1
$Z/zpool sync $POOL
t1=$(date +%s.%N); c1=$(busy)
ddt=$($Z/zpool status -D $POOL | awk '/DDT entries/{gsub(",","",$4); print $4}')
$Z/zpool destroy $POOL || exit 1
awk -v r=$r -v a=$arm -v rs=$rs -v d=$dd -v t0=$t0 -v t1=$t1 -v c0=$c0 \
    -v c1=$c1 -v hz=$(getconf CLK_TCK) -v mb=$SIZE_MB -v prf=$prf -v ddt=$ddt \
    'BEGIN { w = t1 - t0; c = (c1 - c0) / hz;
    printf "%d,%s,%s,%s,%.3f,%.1f,%.2f,%.3f,%s,%s\n", r, a, rs, d, w,
    mb / w, c, c / (mb / 1024), prf, ddt }'
EOF
chmod +x $W/run1.sh
echo "round,arm,recordsize,dedup,wall_s,MiBps,busy_cpu_s,cpu_s_per_GiB,prf_blake3_keys,ddt_entries" > $L/results.csv
```

Expected: the sha256 line of the source, `loop /dev/loopN`.

## STEP 3: one load of each tree, for the checks and the kernel's own figures

```sh
. /var/log/dediv-1102/env.sh
for n in M B; do
    (cd $W/$n && nice -n 10 ./scripts/zfs.sh); echo "load $n rc=$?"
    cat /sys/module/zfs/srcversion | xargs echo "$n srcversion:"
    for p in zfs_sha256_impl zfs_sha512_impl zfs_blake3_impl; do
        printf '%s: ' $p; cat /sys/module/zfs/parameters/$p
    done
    for p in icp_aes_impl icp_gcm_impl; do printf '%s: ' $p; cat /sys/module/icp/parameters/$p; done
    ls /sys/module/zfs/parameters/zfs_crypt_dedup_blake3 2>&1   # M: No such file; B: the path
    cat /proc/spl/kstat/zfs/chksum_bench > $L/chksum_bench-$n.txt
    $W/run1.sh 0 $( [ $n = M ] && echo M || echo B1 ) 128k sha256 | tee -a $L/check.csv
    [ $n = B ] && $W/run1.sh 0 B0 128k sha256 | tee -a $L/check.csv
    (cd $W/$n && ./scripts/zfs.sh -u); echo "unload $n rc=$?"
done
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l
```

Expected: `rc=0` for every load and unload; in each check line,
`prf_blake3_keys` 1 for `B1` and 0 for `M` and `B0` (the record exists only
where it should), and `ddt_entries` 16384 (2 GiB in 128K blocks: every block
went through dedup). A wrong `prf_blake3_keys` or a `ddt_entries` of 0 means
the arms do not measure what they claim: stop. The current impl is the one in
brackets in each `*_impl` line; `chksum_bench-*.txt` is the kernel's own
benchmark of SHA-512 and BLAKE3 per implementation and size (unkeyed, but the
keyed BLAKE3 costs the same, and HMAC-SHA512 adds two compressions).

## STEP 4: the rounds

About 3 minutes per tree and round, plus the loads.

```sh
. /var/log/dediv-1102/env.sh
for r in $(seq 1 $ROUNDS); do
    if [ $((r % 2)) = 1 ]; then order="M B"; else order="B M"; fi
    for n in $order; do
        (cd $W/$n && nice -n 10 ./scripts/zfs.sh) || { echo "load $n failed: stop"; break 2; }
        for rs in 16k 128k 1m; do
            for dd in sha256 blake3; do
                if [ $n = M ]; then arms=M
                elif [ $(( (r + ${#rs}) % 2 )) = 1 ]; then arms="B0 B1"
                else arms="B1 B0"; fi
                for a in $arms; do
                    $W/run1.sh $r $a $rs $dd >> $L/results.csv || { echo "run $r $a $rs $dd failed: stop"; break 5; }
                done
            done
        done
        (cd $W/$n && ./scripts/zfs.sh -u) || { echo "unload $n failed: stop"; break 2; }
        echo "round $r $n done $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)"
    done
done 2>&1 | tee $L/rounds.txt
wc -l < $L/results.csv          # 91: the header and 5 x (6 + 12) runs
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l   # as in STEP 3
```

## STEP 5: optional, only if `fio` exists: four writers

The same measurement with four concurrent writers at 128K, `dedup=sha256`,
three rounds; it shows whether the saving holds when every CPU is busy.

```sh
. /var/log/dediv-1102/env.sh
sed -e 's|^dd if=.*|fio --name=w --directory=$W/mnt/enc --rw=write --bs=1M --size=$((SIZE_MB / 4))M --numjobs=4 --ioengine=psync --refill_buffers --buffer_compress_percentage=0 --end_fsync=0 --group_reporting --minimal > /dev/null \|\| exit 1|' \
    $W/run1.sh > $W/run4.sh && chmod +x $W/run4.sh && grep -c '^fio ' $W/run4.sh   # 1
for r in 1 2 3; do
    if [ $((r % 2)) = 1 ]; then order="M B"; else order="B M"; fi
    for n in $order; do
        (cd $W/$n && nice -n 10 ./scripts/zfs.sh) || break 2
        if [ $n = M ]; then arms=M; elif [ $((r % 2)) = 1 ]; then arms="B0 B1"; else arms="B1 B0"; fi
        for a in $arms; do $W/run4.sh $r $a 128k sha256 >> $L/results-fio.csv || break 3; done
        (cd $W/$n && ./scripts/zfs.sh -u) || break 2
    done
done
```

`fio --refill_buffers` generates its random data while it writes, in
userspace, the same for every arm.

## STEP 6: summary, cleanup, report

```sh
. /var/log/dediv-1102/env.sh
for f in $L/results.csv $L/results-fio.csv; do [ -f $f ] || continue; echo "== $f"
awk -F, 'NR > 1 { k = $3 " " $4 " " $2; n[k]++; w[k, n[k]] = $6; c[k, n[k]] = $8 }
END { for (k in n) { m = n[k]
    for (i = 1; i <= m; i++) { a[i] = w[k, i]; b[i] = c[k, i] }
    asort(a); asort(b)
    printf "%-22s n=%d MiB/s median %.0f [%.0f..%.0f]  CPU s/GiB median %.3f [%.3f..%.3f]\n",
        k, m, a[int((m + 1) / 2)], a[1], a[m], b[int((m + 1) / 2)], b[1], b[m] } }' $f | sort
done | tee $L/summary.txt
lsmod | grep -E '^(zfs|spl) ' && echo "module still loaded"
grep -q " $W/mnt " /proc/mounts && echo "pool still mounted: stop, do not clean"
losetup -d $LOOP; umount $W/ram && rm -rf $W && echo "work dir removed; logs kept in $L"
```

`asort` needs gawk; with mawk, report the raw CSV only. Write
`/var/log/dediv-1102/REPORT.txt` with: STEP 0's output; STEP 1's lines; the
source sha256; STEP 3's output and both `chksum_bench-*.txt` whole;
`rounds.txt`; `results.csv` and `results-fio.csv` whole; `summary.txt`; the
kernel-message counts; every deviation from these steps and why. Reply with
the whole of `REPORT.txt`.

## What the result should look like, and what would be a finding

- `B0` and `M` within each other's spread in every configuration. A
  difference is a regression of B for keys that keep HMAC-SHA512: report it.
- `B1` below `B0` in CPU s/GiB in every configuration, by more at 128K and 1M
  than at 16K: per block, the PRF cost scales with the block, while HKDF of the
  per-block salt and the per-block overheads do not.
- The userspace microbenchmark on the build host (see the proposal) predicts
  the size of the saving from the per-block costs; a kernel saving far from it
  is worth explaining.
