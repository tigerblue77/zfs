# VM task: why keyed BLAKE3 dedup writes ran slower with dedup=blake3

These instructions are for the disposable test VM they are given to
(the Broadwell VM: Xeon E5-2696 v4, AVX2, no AVX-512, no SHA-NI).
They follow the encrypted dedup A/B of 4 November (`/var/log/dediv-1102/`,
`/var/log/dediv-pre/`), and reply with `REPORT.txt`. Nothing is posted,
pushed or installed.

**Background.** On 4 November, with `dedup=blake3`, the keyed BLAKE3 salt/IV
derivation (B1) used 23% to 30% less CPU per GiB than HMAC-SHA512 (B0), but
wrote 2.5% (1M) and 6.1% (128K) slower in median; with `dedup=sha256` it wrote
14% faster. Holding preemption on during the hash changed nothing. Measured
since on the build host in userspace (libzpool, the implementations this VM
selects forced: SHA-512, SHA-256 and BLAKE3 avx2, AES-NI, GCM avx): per block,
B1 costs less than B0 in every phase, with 1 or 4 threads hashing at once; the
PRF hashes the plaintext once and the checksum hashes the ciphertext once, with
the same number of SIMD calls each; and a whole libzpool write, CPU-bound on 4
busy vCPUs, is faster with B1 in all 28 pairs of runs (median +11% to +25%).
Nothing B1 adds per block costs time there.

What differs here is that this VM is mostly idle during a run: one `dd` writer
kept 3 to 7 of 16 vCPUs busy on 4 November. The hypothesis is that the
throughput of such a run is set by hand-offs between threads (the writer, the
txg sync thread, the write issue and interrupt threads, the loop device worker)
rather than by CPU, and that each hand-off to an idle, halted vCPU waits for it
to wake. B1 halves the issue work per block, so the issue threads go idle more
often and more hand-offs wait (at 1M with `dedup=blake3`, the context
switches rose 8% with B1 on 4 November).

**The test.** Two extra arms, from a test-only patch (T, below), keep B1's
derivation and give back the time B0 spent per block, in two ways:

- `T1H` also computes the HMAC-SHA512 of each block and discards it: B0's
  work, including its single long FPU section with interrupts off, plus B1's.
- `T1S` busy-waits as long as that HMAC would take, with interrupts on and no
  FPU use, then derives with BLAKE3.

If the hypothesis holds, T1H and T1S write at least as fast as B0, although
they spend more CPU per block than B1. If B1's derivation itself cost
throughput, both would write slower than B1.

| Tree | Commit | What it is |
| --- | --- | --- |
| `A` | `2796b5da69ab267f96479f6e09c00613d628d450` | upstream master |
| `B` | `c7ecb59248b313f61865790be850021c7fcba94e` | fork branch `dedup-iv-blake3-v2`: A plus the keyed BLAKE3 derivation and its test |
| `T` | `B` plus `T-ballast.patch` | adds the test-only parameters `zfs_crypt_dedup_ballast` (0 off, 1 HMAC, 2 busy wait) and `zfs_crypt_dedup_ballast_ns_per_kib` |

| Arm | Tree | Parameters at `zfs create` and during the write | Derivation |
| --- | --- | --- | --- |
| `A` | A | (none) | HMAC-SHA512 |
| `B0` | B | `zfs_crypt_dedup_blake3=0` | HMAC-SHA512 |
| `B1` | B | `zfs_crypt_dedup_blake3=1` | keyed BLAKE3 |
| `T1H` | T | blake3 1, ballast 1 | keyed BLAKE3, plus an HMAC-SHA512 discarded |
| `T1S` | T | blake3 1, ballast 2, `ns_per_kib` from this VM's SHA-512 speed | keyed BLAKE3, plus a busy wait as long as the HMAC |

**The workload** is that of 4 November: `dd bs=1M` of 2 GiB of random bytes
from a tmpfs into an `aes-256-gcm` dataset with `compression=off`, then `zpool
sync`, on a pool whose only vdev is a loop device over a tmpfs file; a fresh
pool and key per run. Four configurations: `recordsize` 1M and 128K, times
`dedup=blake3` and `dedup=sha256`. Seven rounds. Per run: MiB/s from the start
of `dd` to the end of `zpool sync`; busy and idle CPU of the whole VM
(`/proc/stat`); context switches; rescheduling, function-call and TLB IPIs
(`RES`, `CAL`, `TLB` of `/proc/interrupts`).

## Rules

- Disposable VM, release builds, nothing else running during the measured
  rounds. Builds under `nice -n 10 ionice -c3` with `make -j8`, one tree at a
  time.
- Post nothing, push nothing, install nothing (no package, no `make install`),
  no `zfs-tests.sh`, no `zfs-helpers.sh`, never reboot. The only module loaded
  apart from this task's ZFS builds is the optional `cpuidle-haltpoll` of
  STEP 5, which ships with the running kernel.
- Work only under `/var/tmp/dediv-1108` (W) and `/var/log/dediv-1108` (L), the
  tmpfs at `$W/ram`, its loop device and the pool `dediv`.
- Never run `zpool`, `zfs` or `zdb` while none of this task's modules is
  loaded.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:` or
  hung-task message: stop and report. After a stop past STEP 0, go to STEP 6
  with what you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/dediv-1108 L=/var/log/dediv-1108
date -u '+%F %T'; id -un; uname -r; cat /proc/cmdline; nproc
grep -m1 '^model name' /proc/cpuinfo
for f in sha_ni avx2 avx512f; do grep -qw $f /proc/cpuinfo && printf '%s=yes ' $f || printf '%s=no ' $f; done; echo
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl|cpuidle_haltpoll) '  # nothing
command -v zpool zfs zdb zed                    # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|configure|make|gcc|cc1|fio|bench' | grep -v pgrep
ls -d $W $L 2>&1                                # No such file or directory, twice
losetup -a; grep -E ' zfs |dediv' /proc/mounts  # nothing, nothing
ls -l /etc/zfs/zpool.cache 2>&1                 # absent, or 0 bytes
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor 2>&1
free -g | sed -n 2p                             # "available" 12 or more
df -h /var/tmp                                  # 10G free or more
uptime
```

Stop, changing nothing, if: you are not root; `/proc/cmdline` contains
`slub_debug`; the kernel headers are missing; a zfs or spl module is loaded or
a system ZFS is installed; a build, test or benchmark runs; `$W` or `$L`
exists; a loop device or zfs mount exists; `zpool.cache` is not empty; less
than 12 GiB of memory is available or less than 10G is free on `/var/tmp`.
Record the cpuidle driver and governor: they decide how an idle vCPU waits.

## STEP 1: sources, the patch, release builds

```sh
W=/var/tmp/dediv-1108 L=/var/log/dediv-1108
mkdir -p $W $L && cat > $L/env.sh <<EOF2
export PATH=/usr/sbin:/sbin:\$PATH
W=$W L=$L POOL=dediv SIZE_MB=2048 ROUNDS=7
EOF2
. $L/env.sh
for t in A:2796b5da69ab267f96479f6e09c00613d628d450 B:c7ecb59248b313f61865790be850021c7fcba94e T:c7ecb59248b313f61865790be850021c7fcba94e; do
    n=${t%%:*} c=${t#*:}
    git init -q $W/$n && git -C $W/$n fetch -q https://github.com/tigerblue77/zfs c7ecb59248b313f61865790be850021c7fcba94e &&
    git -C $W/$n checkout -q $c && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
cat > $L/T-ballast.patch <<'EOF2'
diff --git a/module/zfs/zio_crypt.c b/module/zfs/zio_crypt.c
index ca9b94267f..db41d676f9 100644
--- a/module/zfs/zio_crypt.c
+++ b/module/zfs/zio_crypt.c
@@ -606,6 +606,13 @@ zio_crypt_dedup_blake3(const zio_crypt_key_t *key, const uint8_t *data,
 #endif
 }
 
+/*
+ * A/B test only. With a BLAKE3 key: 1 also computes the HMAC-SHA512 of the
+ * block and discards it, 2 busy-waits ballast_ns_per_kib per KiB instead.
+ */
+static uint_t zfs_crypt_dedup_ballast = 0;
+static uint_t zfs_crypt_dedup_ballast_ns_per_kib = 1600;
+
 int
 zio_crypt_generate_iv_salt_dedup(zio_crypt_key_t *key, uint8_t *data,
     uint_t datalen, uint8_t *ivbuf, uint8_t *salt)
@@ -614,6 +621,16 @@ zio_crypt_generate_iv_salt_dedup(zio_crypt_key_t *key, uint8_t *data,
 	uint8_t digestbuf[SHA512_HMAC_LEN];
 
 	if (key->zk_dedup_prf == ZIO_CRYPT_DEDUP_PRF_BLAKE3) {
+		if (zfs_crypt_dedup_ballast == 1) {
+			ret = zio_crypt_hmac_os(key, data, datalen, digestbuf);
+			if (ret != 0)
+				return (ret);
+		} else if (zfs_crypt_dedup_ballast == 2) {
+			hrtime_t end = gethrtime() + (hrtime_t)datalen *
+			    zfs_crypt_dedup_ballast_ns_per_kib / 1024;
+			while (gethrtime() < end)
+				;
+		}
 		zio_crypt_dedup_blake3(key, data, datalen, digestbuf);
 	} else {
 		ret = zio_crypt_hmac_os(key, data, datalen, digestbuf);
@@ -1768,6 +1785,11 @@ error:
 	return (SET_ERROR(ret));
 }
 
+ZFS_MODULE_PARAM(zfs, zfs_crypt_, dedup_ballast, UINT, ZMOD_RW,
+	"A/B test: 1 adds a discarded HMAC, 2 a busy wait, to BLAKE3 dedup");
+ZFS_MODULE_PARAM(zfs, zfs_crypt_, dedup_ballast_ns_per_kib, UINT, ZMOD_RW,
+	"A/B test: busy wait per KiB of ballast 2");
+
 ZFS_MODULE_PARAM(zfs, zfs_key_,  max_salt_uses, U64, ZMOD_RW,
 	"Max number of times a salt value can be used for generating "
 	"encryption keys before it is rotated");
EOF2
sha256sum $L/T-ballast.patch      # 05732dbc682a84606149c74c78a0fa4bf54cf8b7e6ae3a4a367b88ff2866cfc1
git -C $W/T apply $L/T-ballast.patch && git -C $W/T diff --stat | tail -1   # 1 file changed, 22 insertions(+)
for n in A B T; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure && make -j8' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    grep -c '^#define ZFS_DEBUG 1' $W/$n/zfs_config.h | xargs echo "$n ZFS_DEBUG defines:"   # 1
    modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1 | xargs echo "$n built for:"     # $(uname -r)
    modinfo -F srcversion $W/$n/module/zfs.ko | xargs echo "$n srcversion:"
done
```

Expected: A at `2796b5da69...`, B and T at `c7ecb59248...`; the patch's
sha256 and diff stat as shown; `build rc=0` three times; `ZFS_DEBUG defines:
1` three times (a release build); all built for the running kernel; three
different srcversions. On a failed build quote `grep -nE 'error:|Error [0-9]'
$L/build-<n>.log | head -20` and stop.

## STEP 2: the RAM disk, the source file and the run script

```sh
. /var/log/dediv-1108/env.sh
mkdir -p $W/ram $W/mnt && mount -t tmpfs -o size=$((SIZE_MB * 2 + 2048))M dediv-ram $W/ram
head -c ${SIZE_MB}M /dev/urandom > $W/ram/src && sha256sum $W/ram/src | tee $L/src.sha256
truncate -s $((SIZE_MB + 1536))M $W/ram/vdev
LOOP=$(losetup -f --show $W/ram/vdev) && echo "LOOP=$LOOP" >> $L/env.sh && echo "loop $LOOP"
head -c 32 /dev/urandom > $W/key && chmod 600 $W/key
cat > $W/run1.sh <<'EOF2'
#!/bin/bash
# run1.sh <round> <arm> <recordsize> <dedup>: one measured write, one CSV line
. /var/log/dediv-1108/env.sh
r=$1 arm=$2 rs=$3 dd=$4
case $arm in A) tree=A ;; B0|B1) tree=B ;; T1H|T1S) tree=T ;; *) exit 1 ;; esac
[ $arm != T1S ] || [ -n "$NSKIB" ] || exit 1
Z=$W/$tree P=/sys/module/zfs/parameters
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8}' /proc/stat; }
idle() { awk '/^cpu /{print $5+$6}' /proc/stat; }
ctxt() { awk '/^ctxt /{print $2}' /proc/stat; }
ipis() { awk '$1 ~ /^(RES|CAL|TLB):$/ { for (i = 2; i <= NF && $i ~ /^[0-9]+$/; i++) s += $i }
    END { print s + 0 }' /proc/interrupts; }
$Z/zpool create -f -o ashift=12 -O compression=off -O atime=off \
    -O recordsize=$rs -O dedup=$dd -O mountpoint=$W/mnt $POOL $LOOP || exit 1
case $arm in B0) echo 0 > $P/zfs_crypt_dedup_blake3 ;; B1|T1H|T1S) echo 1 > $P/zfs_crypt_dedup_blake3 ;; esac || exit 1
case $arm in
T1H) echo 1 > $P/zfs_crypt_dedup_ballast ;;
T1S) echo $NSKIB > $P/zfs_crypt_dedup_ballast_ns_per_kib && echo 2 > $P/zfs_crypt_dedup_ballast ;;
esac || exit 1
$Z/zfs create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$W/key $POOL/enc || exit 1
$Z/zpool sync $POOL
prf=$($Z/zdb -dddd $POOL 2>/dev/null | grep -c 'DSL_CRYPTO_DEDUP_PRF = 1')
c0=$(busy); i0=$(idle); x0=$(ctxt); q0=$(ipis); t0=$(date +%s.%N)
dd if=$W/ram/src of=$W/mnt/enc/f bs=1M status=none || exit 1
$Z/zpool sync $POOL
t1=$(date +%s.%N); c1=$(busy); i1=$(idle); x1=$(ctxt); q1=$(ipis)
case $arm in T1H|T1S) echo 0 > $P/zfs_crypt_dedup_ballast ;; esac
ddt=$($Z/zpool status -D $POOL | awk '/DDT entries/{gsub(",","",$4); print $4}')
$Z/zpool destroy $POOL || exit 1
awk -v r=$r -v a=$arm -v rs=$rs -v d=$dd -v t0=$t0 -v t1=$t1 -v c0=$c0 -v c1=$c1 \
    -v i0=$i0 -v i1=$i1 -v hz=$(getconf CLK_TCK) -v mb=$SIZE_MB -v prf=$prf \
    -v ddt=$ddt -v x=$((x1 - x0)) -v q=$((q1 - q0)) \
    'BEGIN { w = t1 - t0; c = (c1 - c0) / hz;
    printf "%d,%s,%s,%s,%.3f,%.1f,%.2f,%.3f,%.2f,%s,%s,%d,%d\n", r, a, rs, d, w,
    mb / w, c, c / (mb / 1024), (i1 - i0) / hz, prf, ddt, x, q }'
EOF2
chmod +x $W/run1.sh
echo "round,arm,recordsize,dedup,wall_s,MiBps,busy_cpu_s,cpu_s_per_GiB,idle_cpu_s,prf_blake3_keys,ddt_entries,ctxt,ipis" > $L/results.csv
```

Expected: the sha256 line of the source, `loop /dev/loopN`.

## STEP 3: the checks, and the length of the busy wait

```sh
. /var/log/dediv-1108/env.sh
for n in A B T; do
    (cd $W/$n && nice -n 10 ./scripts/zfs.sh); echo "load $n rc=$?"
    cat /sys/module/zfs/srcversion | xargs echo "$n loaded srcversion:"   # as built
    for p in zfs_sha512_impl zfs_blake3_impl; do printf '%s: ' $p; cat /sys/module/zfs/parameters/$p; done
    for p in zfs_crypt_dedup_blake3 zfs_crypt_dedup_ballast; do
        printf '%s: ' $p; cat /sys/module/zfs/parameters/$p 2>&1   # A: no such file, twice; B: 1, then no such file; T: 1, 0
    done
    if [ $n = T ]; then
        cat /proc/spl/kstat/zfs/chksum_bench > $L/chksum_bench-T.txt
        NSKIB=$(awk '$1 ~ /^sha512-/ && $7 + 0 > m { m = $7 + 0 } END { printf "%d", 1e9 / (m * 1024) }' $L/chksum_bench-T.txt)
        echo "NSKIB=$NSKIB" >> $L/env.sh && echo "busy wait: $NSKIB ns per KiB"   # about 1400 to 1800
        . $L/env.sh
    fi
    case $n in A) arms=A ;; B) arms="B0 B1" ;; T) arms="T1H T1S" ;; esac
    for a in $arms; do $W/run1.sh 0 $a 1m blake3 | tee -a $L/check.csv; done
    (cd $W/$n && ./scripts/zfs.sh -u); echo "unload $n rc=$?"
done
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l   # 0
```

Expected: `rc=0` for every load and unload; `prf_blake3_keys` 0 for A and B0,
1 for B1, T1H and T1S; `ddt_entries` 2048 in each line; T1H and T1S with a
`cpu_s_per_GiB` at or a little above B0's (B0's work plus B1's), so well
above B1's. Otherwise stop: the arms would not measure what they claim.

## STEP 4: the rounds

About 3 minutes per round.

```sh
. /var/log/dediv-1108/env.sh
for r in $(seq 1 $ROUNDS); do
    if [ $((r % 2)) = 1 ]; then order="A B T"; else order="T B A"; fi
    for n in $order; do
        (cd $W/$n && nice -n 10 ./scripts/zfs.sh) || { echo "load $n failed: stop"; break 2; }
        for cfg in 1m:blake3 128k:blake3 1m:sha256 128k:sha256; do
            rs=${cfg%%:*} dd=${cfg#*:}
            case $n$((r % 2)) in
            A?) arms=A ;; B1) arms="B0 B1" ;; B0) arms="B1 B0" ;;
            T1) arms="T1H T1S" ;; T0) arms="T1S T1H" ;;
            esac
            for a in $arms; do
                $W/run1.sh $r $a $rs $dd >> $L/results.csv || { echo "run $r $a $rs $dd failed: stop"; break 4; }
            done
        done
        (cd $W/$n && ./scripts/zfs.sh -u) || { echo "unload $n failed: stop"; break 2; }
        echo "round $r $n done $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)"
    done
done 2>&1 | tee $L/rounds.txt
wc -l < $L/results.csv          # 141: the header and 7 x 20 runs
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l   # 0
```

## STEP 5 (optional): guest halt polling

Only if `modinfo -n cpuidle-haltpoll` prints a path ending in `.ko` and STEP 0
showed a cpuidle driver other than `haltpoll`; otherwise write "STEP 5
skipped" and why. With halt polling an idle vCPU polls for up to 200 us before
it halts, so a hand-off soon after it went idle does not wait for a wake-up.
If B1's deficit against B0 shrinks or turns into a gain here, the wake-ups of
halted vCPUs are what it costs.

```sh
. /var/log/dediv-1108/env.sh
modinfo -n cpuidle-haltpoll
modprobe cpuidle-haltpoll force=1; echo "modprobe rc=$?"
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor   # haltpoll, haltpoll
(cd $W/B && nice -n 10 ./scripts/zfs.sh) || echo "load B failed: stop"
head -1 $L/results.csv > $L/results-haltpoll.csv
for r in 1 2 3 4 5; do
    for cfg in 1m:blake3 128k:blake3; do
        if [ $((r % 2)) = 1 ]; then arms="B0 B1"; else arms="B1 B0"; fi
        for a in $arms; do $W/run1.sh $r $a ${cfg%%:*} ${cfg#*:} >> $L/results-haltpoll.csv || break 3; done
    done
done
(cd $W/B && ./scripts/zfs.sh -u); echo "unload B rc=$?"
rmmod cpuidle_haltpoll; echo "rmmod rc=$?"
cat /sys/devices/system/cpu/cpuidle/current_driver   # as in STEP 0
wc -l < $L/results-haltpoll.csv   # 21: the header and 5 x 4 runs
```

If `modprobe` fails, skip the rest of this step. If `rmmod` fails, report it:
the vCPUs then keep polling until the next reboot, which changes nothing else.

## STEP 6: summary, cleanup, report

```sh
. /var/log/dediv-1108/env.sh
python3 -I - $L/results.csv $L/results-haltpoll.csv <<'EOF2' | tee $L/summary.txt
import csv, os, statistics as st, sys
for path in sys.argv[1:]:
    if not os.path.exists(path):
        continue
    rows = list(csv.DictReader(open(path)))
    print("==", path)
    g = {}
    for x in rows:
        g.setdefault((x["recordsize"], x["dedup"], x["arm"]), []).append(x)
    for k in sorted(g):
        v = g[k]
        def m(f, s=1.0): return st.median(float(y[f]) / s for y in v)
        def r(f): return min(float(y[f]) for y in v), max(float(y[f]) for y in v)
        print("%-5s %-6s %-3s n=%d MiB/s %.0f [%.0f..%.0f] CPU s/GiB %.3f [%.3f..%.3f] idle s %.1f ctxt %.0f ipis %.0f"
              % (k + (len(v), m("MiBps")) + r("MiBps") + (m("cpu_s_per_GiB"),) + r("cpu_s_per_GiB")
                 + (m("idle_cpu_s"), m("ctxt"), m("ipis"))))
    by = {(x["round"], x["recordsize"], x["dedup"], x["arm"]): x for x in rows}
    for rs in ("1m", "128k"):
        for dd in ("blake3", "sha256"):
            for a in ("A", "B1", "T1H", "T1S"):
                for f in ("MiBps", "cpu_s_per_GiB"):
                    s = sorted(float(by[(rd, rs, dd, a)][f]) / float(by[(rd, rs, dd, "B0")][f]) - 1
                               for rd in sorted({x["round"] for x in rows})
                               if (rd, rs, dd, a) in by and (rd, rs, dd, "B0") in by)
                    if s:
                        print("%-5s %-6s %-3s/B0 %-13s median %+.1f%% per round: %s" % (rs, dd, a, f,
                              100 * st.median(s), " ".join("%+.1f%%" % (100 * y) for y in s)))
EOF2
lsmod | grep -E '^(zfs|spl) ' && echo "module still loaded"
grep -q " $W/mnt " /proc/mounts && echo "pool still mounted: stop, do not clean"
losetup -d $LOOP; umount $W/ram && rm -rf $W && echo "work dir removed; logs kept in $L"
```

Write `/var/log/dediv-1108/REPORT.txt` with STEP 0 to STEP 3 output,
`chksum_bench-T.txt`, `check.csv`, `rounds.txt`, `results.csv` and
`results-haltpoll.csv` whole, `summary.txt`, STEP 5's output or why it was
skipped, the kernel-message counts and every deviation. Reply with the whole
of `REPORT.txt`.

## Reading the result

- **Hypothesis confirmed** if, with `dedup=blake3`, B1 again writes slower
  than B0 while T1H and T1S write at least as fast as B0 in most rounds,
  although their CPU per GiB is above B1's. B1's loss then comes from doing
  less work per block on a mostly idle VM, not from anything the keyed BLAKE3
  derivation adds. With `dedup=sha256`, all three should write faster than B0.
- **T1H near B0 but T1S near B1**: what helps is B0's long section with
  interrupts off (the HMAC's single FPU section per block), not the busy time.
- **T1H and T1S both below B1**: the BLAKE3 derivation costs throughput by
  itself in the kernel, against the userspace measurements; report it.
- **A and B0** should overlap in every configuration, as on 4 November.
- STEP 5: B1 at or above B0 with halt polling confirms that the wait is the
  wake-up of halted vCPUs.
