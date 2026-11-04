# VM task: does kpreempt_disable() in the keyed BLAKE3 PRF cost write throughput?

Follow-up of the dedup-iv kernel A/B of 4 November (`/var/log/dediv-1102/`).
There, at 1M with `dedup=blake3`, B1 (keyed BLAKE3 PRF) used 23% less CPU per
GiB than B0 (HMAC-SHA512) but wrote 4.7% slower, in every round. One
hypothesis: in the kernel the PRF hashes the whole block under
`kpreempt_disable()` (the per-CPU BLAKE3 context), about 0.4 ms per 1M block,
on top of the BLAKE3 checksum's own non-preemptible stretch with
`dedup=blake3`. The variant P below takes a context from `kmem_alloc()` instead
and never disables preemption around the hash (the SIMD code still disables it
per FPU section, as always). If P1 writes as fast as B0 at 1M `dedup=blake3`
while keeping B1's CPU saving, the hypothesis holds.

| Arm | Tree | `zfs_crypt_dedup_blake3` at `zfs create` | PRF |
| --- | --- | --- | --- |
| `B0` | B = `8d201c42799b0fd8ef5a460d21f3374750201e41` | 0 | HMAC-SHA512 |
| `B1` | B | 1 | keyed BLAKE3, per-CPU context, preemption off |
| `P1` | P = B + the patch below | 1 | keyed BLAKE3, `kmem_alloc()` context, preemption on |

Same workload and figures as on 4 November (2 GiB of random bytes from a
tmpfs, `dd bs=1M`, `aes-256-gcm`, `compression=off`, pool on a loop device over
a tmpfs file, fresh pool and key per run), plus the context switches of the
whole VM over each run. Three configurations: 1M `blake3` (the anomaly), 1M
`sha256` (where B1 wrote faster) and 128K `blake3`. Seven rounds.

## Rules

- Disposable VM, release builds, nothing else running during the rounds.
  Builds under `nice -n 10 ionice -c3` with `make -j8`, one tree at a time.
- Post nothing, push nothing, install nothing, no `zfs-tests.sh`, no
  `zfs-helpers.sh`. Work only under `/var/tmp/dediv-pre` (W) and
  `/var/log/dediv-pre` (L), the tmpfs at `$W/ram`, its loop device and the pool
  `dediv`.
- Never run `zpool`, `zfs` or `zdb` while none of this task's modules is
  loaded.
- If a check does not print what its comment says, stop and report. A panic,
  oops, `VERIFY`, `ASSERT`, `BUG:` or hung-task message: stop and report.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/dediv-pre L=/var/log/dediv-pre
date -u '+%F %T'; id -un; uname -r; cat /proc/cmdline; nproc
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '                  # nothing
command -v zpool zfs zdb zed                    # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|configure|make|gcc|cc1|fio|bench' | grep -v pgrep
ls -d $W $L 2>&1                                # No such file or directory, twice
losetup -a; grep -E ' zfs |dediv' /proc/mounts  # nothing, nothing
free -g | sed -n 2p                             # "available" 12 or more
df -h /var/tmp                                  # 6G free or more
uptime
```

`/proc/cmdline` must not contain `slub_debug`.

## STEP 1: sources, the patch, release builds

```sh
W=/var/tmp/dediv-pre L=/var/log/dediv-pre
mkdir -p $W $L && cat > $L/env.sh <<EOF2
export PATH=/usr/sbin:/sbin:\$PATH
W=$W L=$L POOL=dediv SIZE_MB=2048 ROUNDS=7
EOF2
. $L/env.sh
for n in B P; do
    git init -q $W/$n && git -C $W/$n fetch -q https://github.com/tigerblue77/zfs 8d201c42799b0fd8ef5a460d21f3374750201e41 &&
    git -C $W/$n checkout -q FETCH_HEAD && echo "$n at $(git -C $W/$n rev-parse HEAD)"
done
cat > $L/P-no-preempt.patch <<'EOF2'
diff --git a/module/zfs/zio_crypt.c b/module/zfs/zio_crypt.c
index c0238460c7..ceaee1699b 100644
--- a/module/zfs/zio_crypt.c
+++ b/module/zfs/zio_crypt.c
@@ -572,7 +572,7 @@ static void
 zio_crypt_dedup_blake3(const zio_crypt_key_t *key, const uint8_t *data,
     uint_t datalen, uint8_t digest[BLAKE3_OUT_LEN])
 {
-#if defined(_KERNEL)
+#if 0 /* A/B: no kpreempt_disable() */
 	/* as in abd_checksum_blake3_native() */
 	kpreempt_disable();
 	BLAKE3_CTX *ctx = blake3_per_cpu_ctx[CPU_SEQID];
@@ -587,7 +587,7 @@ zio_crypt_dedup_blake3(const zio_crypt_key_t *key, const uint8_t *data,
 	/* unlike a checksum context, this one holds secret key words */
 	memset(ctx, 0, sizeof (*ctx));
 
-#if defined(_KERNEL)
+#if 0 /* A/B: no kpreempt_disable() */
 	kpreempt_enable();
 #else
 	kmem_free(ctx, sizeof (*ctx));
EOF2
sha256sum $L/P-no-preempt.patch      # 8e2c9ef9fec6379b26b985b3220b425416281c70d8bbab2321a97b9ba6fb2c96
git -C $W/P apply $L/P-no-preempt.patch && git -C $W/P diff --stat   # 1 file changed, 2 insertions(+), 2 deletions(-)
for n in B P; do
    (cd $W/$n && t0=$(date +%s) &&
     nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure && make -j8' > $L/build-$n.log 2>&1
     echo "$n build rc=$? wall=$(( $(date +%s) - t0 ))s")
    grep -c '^#define ZFS_DEBUG 1' $W/$n/zfs_config.h | xargs echo "$n ZFS_DEBUG defines:"   # 1
    modinfo -F vermagic $W/$n/module/zfs.ko | cut -d' ' -f1 | xargs echo "$n built for:"     # $(uname -r)
    modinfo -F srcversion $W/$n/module/zfs.ko | xargs echo "$n srcversion:"
done
```

Expected: both trees at that commit, the patch's sha256 as shown and its diff
stat, `build rc=0` twice, `ZFS_DEBUG defines: 1` twice, both built for the
running kernel, two different srcversions.

## STEP 2: the RAM disk, the source file and the run script

```sh
. /var/log/dediv-pre/env.sh
mkdir -p $W/ram $W/mnt && mount -t tmpfs -o size=$((SIZE_MB * 2 + 2048))M dediv-ram $W/ram
head -c ${SIZE_MB}M /dev/urandom > $W/ram/src && sha256sum $W/ram/src | tee $L/src.sha256
truncate -s $((SIZE_MB + 1536))M $W/ram/vdev
LOOP=$(losetup -f --show $W/ram/vdev) && echo "LOOP=$LOOP" >> $L/env.sh && echo "loop $LOOP"
head -c 32 /dev/urandom > $W/key && chmod 600 $W/key
cat > $W/run1.sh <<'EOF2'
#!/bin/bash
# run1.sh <round> <arm> <recordsize> <dedup>: one measured write, one CSV line
. /var/log/dediv-pre/env.sh
r=$1 arm=$2 rs=$3 dd=$4
tree=${arm%?}
Z=$W/$tree
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8}' /proc/stat; }
ctxt() { awk '/^ctxt /{print $2}' /proc/stat; }
$Z/zpool create -f -o ashift=12 -O compression=off -O atime=off \
    -O recordsize=$rs -O dedup=$dd -O mountpoint=$W/mnt $POOL $LOOP || exit 1
echo ${arm#?} > /sys/module/zfs/parameters/zfs_crypt_dedup_blake3
$Z/zfs create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$W/key $POOL/enc || exit 1
$Z/zpool sync $POOL
prf=$($Z/zdb -dddd $POOL 2>/dev/null | grep -c 'DSL_CRYPTO_DEDUP_PRF = 1')
c0=$(busy); x0=$(ctxt); t0=$(date +%s.%N)
dd if=$W/ram/src of=$W/mnt/enc/f bs=1M status=none || exit 1
$Z/zpool sync $POOL
t1=$(date +%s.%N); c1=$(busy); x1=$(ctxt)
ddt=$($Z/zpool status -D $POOL | awk '/DDT entries/{gsub(",","",$4); print $4}')
$Z/zpool destroy $POOL || exit 1
awk -v r=$r -v a=$arm -v rs=$rs -v d=$dd -v t0=$t0 -v t1=$t1 -v c0=$c0 \
    -v c1=$c1 -v hz=$(getconf CLK_TCK) -v mb=$SIZE_MB -v prf=$prf -v ddt=$ddt \
    -v x=$((x1 - x0)) \
    'BEGIN { w = t1 - t0; c = (c1 - c0) / hz;
    printf "%d,%s,%s,%s,%.3f,%.1f,%.2f,%.3f,%s,%s,%d\n", r, a, rs, d, w,
    mb / w, c, c / (mb / 1024), prf, ddt, x }'
EOF2
chmod +x $W/run1.sh
echo "round,arm,recordsize,dedup,wall_s,MiBps,busy_cpu_s,cpu_s_per_GiB,prf_blake3_keys,ddt_entries,ctxt" > $L/results.csv
```

## STEP 3: one check per arm

```sh
. /var/log/dediv-pre/env.sh
for n in B P; do
    (cd $W/$n && nice -n 10 ./scripts/zfs.sh); echo "load $n rc=$?"
    cat /sys/module/zfs/srcversion | xargs echo "$n loaded srcversion:"   # as built
    if [ $n = B ]; then arms="B0 B1"; else arms="P1"; fi
    for a in $arms; do $W/run1.sh 0 $a 1m blake3 | tee -a $L/check.csv; done
    (cd $W/$n && ./scripts/zfs.sh -u); echo "unload $n rc=$?"
done
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l   # 0
```

Expected: `prf_blake3_keys` 1 for B1 and P1, 0 for B0; `ddt_entries` 2048
in each line. Otherwise stop.

## STEP 4: the rounds

```sh
. /var/log/dediv-pre/env.sh
for r in $(seq 1 $ROUNDS); do
    if [ $((r % 2)) = 1 ]; then order="B P"; else order="P B"; fi
    for n in $order; do
        (cd $W/$n && nice -n 10 ./scripts/zfs.sh) || { echo "load $n failed: stop"; break 2; }
        for cfg in 1m:blake3 1m:sha256 128k:blake3; do
            rs=${cfg%%:*} dd=${cfg#*:}
            if [ $n = P ]; then arms=P1
            elif [ $((r % 2)) = 1 ]; then arms="B0 B1"; else arms="B1 B0"; fi
            for a in $arms; do
                $W/run1.sh $r $a $rs $dd >> $L/results.csv || { echo "run $r $a $rs $dd failed: stop"; break 4; }
            done
        done
        (cd $W/$n && ./scripts/zfs.sh -u) || { echo "unload $n failed: stop"; break 2; }
        echo "round $r $n done $(date +%T) load $(cut -d' ' -f1 /proc/loadavg)"
    done
done 2>&1 | tee $L/rounds.txt
wc -l < $L/results.csv          # 64: the header and 7 x 9 runs
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | wc -l   # 0
```

## STEP 5: summary, cleanup, report

```sh
. /var/log/dediv-pre/env.sh
python3 -I - $L/results.csv <<'EOF2' | tee $L/summary.txt
import csv, statistics, sys
rows = list(csv.DictReader(open(sys.argv[1])))
g = {}
for x in rows:
    g.setdefault((x["recordsize"], x["dedup"], x["arm"]), []).append(x)
for k in sorted(g):
    v = g[k]
    def m(f): return statistics.median(float(y[f]) for y in v)
    def r(f): return min(float(y[f]) for y in v), max(float(y[f]) for y in v)
    print("%-5s %-6s %-3s n=%d MiB/s %.0f [%.0f..%.0f] CPU s/GiB %.3f [%.3f..%.3f] ctxt %.0f [%.0f..%.0f]"
          % (k + (len(v), m("MiBps")) + r("MiBps") + (m("cpu_s_per_GiB"),) + r("cpu_s_per_GiB")
             + (m("ctxt"),) + r("ctxt")))
by = {}
for x in rows:
    by[(x["round"], x["recordsize"], x["dedup"], x["arm"])] = float(x["MiBps"])
for rs, dd in (("1m", "blake3"), ("1m", "sha256"), ("128k", "blake3")):
    for a in ("B1", "P1"):
        s = [by[(rd, rs, dd, a)] / by[(rd, rs, dd, "B0")] - 1
             for rd in sorted({x["round"] for x in rows})
             if (rd, rs, dd, a) in by and (rd, rs, dd, "B0") in by]
        print("%-5s %-6s %s/B0 MiB/s per round: %s" % (rs, dd, a, " ".join("%+.1f%%" % (100 * y) for y in s)))
EOF2
lsmod | grep -E '^(zfs|spl) ' && echo "module still loaded"
grep -q " $W/mnt " /proc/mounts && echo "pool still mounted: stop, do not clean"
losetup -d $LOOP; umount $W/ram && rm -rf $W && echo "work dir removed; logs kept in $L"
```

Write `/var/log/dediv-pre/REPORT.txt` with STEP 0 to STEP 3 output,
`rounds.txt`, `results.csv` whole, `summary.txt`, the kernel-message counts and
every deviation. Reply with the whole of `REPORT.txt`.

## Reading the result

- The hypothesis holds if, at 1M `blake3`, P1's throughput is at or above
  B0's in most rounds while B1's stays below, and P1's CPU s/GiB stays close
  to B1's (well below B0's).
- It fails if P1 is as slow as B1: the cause is elsewhere (the dedup path,
  the DDT, the checksum), and the throughput loss stays unexplained.
- At 1M `sha256` and 128K `blake3`, P1 should be within B1's spread; a
  difference there is worth reporting too.
