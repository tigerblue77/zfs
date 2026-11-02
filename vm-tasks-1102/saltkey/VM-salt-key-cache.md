# VM task: kernel checks of the zio_crypt salt key cache

Self-contained. Run it on a **disposable Linux VM with no pool of its own**:
it loads and unloads the ZFS modules several times, creates and destroys
pools on files under `/var/tmp/skc`, and runs fio. Reboots are allowed (see
"If something goes wrong"). Nothing is posted anywhere: the only output is
`/var/tmp/skc/REPORT.txt` and the files beside it, to be handed back as is.

## What is being tested

Branch `zio-crypt-salt-key-cache-v2` of `https://github.com/tigerblue77/zfs`
at `4bf3e32d76`, four commits on master `0cdf531257`:

| Commit | What it does |
| --- | --- |
| `9fb5a69a90` | zio_crypt: zero the derived key after a QAT operation |
| `570797ac00` | FreeBSD: leave a failed crypto session as it was |
| `9156485339` | zio_crypt: cache keys derived for older salts |
| `4bf3e32d76` | unit: test the cache of keys derived for older salts (userspace only) |

`zio_do_crypt_data()` has a ready key only for the key's current salt; a
block under any other salt gets an HKDF and a new ICP key schedule on every
read. The salt changes at every key load and every `zfs_key_max_salt_uses`
encrypted block writes, so after a key load almost every read of existing
data takes that path. The branch keeps, per loaded key, up to 16 derived keys
for other salts (a salt is admitted on its second decryption miss among the
last 32 missed salts, so that dedup blocks, which have a salt each, are not
cached). The new kstat `/proc/spl/kstat/zfs/zio_crypt_stats` counts
`salt_cache_hits`, `salt_cache_misses`, `salt_cache_admissions` and
`salt_cache_evictions`, in that order. Nothing changes on disk.

What this task must establish, on a real kernel:

- **Part A** (debug build of the branch): reads of data written under older
  salts are served from the cache (the kstat's hits), the data reads back
  unchanged after a key unload and load, after `zfs change-key` while the key
  is loaded and after an unload and load with the new key, after
  `zfs change-key -i`, after an export and import, and with concurrent
  readers of old-salt and dedup data; the module unloads with no leak and no
  kernel message.
- **Part B** (release builds, master against branch): an A/B fio random-read
  benchmark with identical settings, several alternating rounds, reporting
  the spread and not just one number.
- **Part C** (optional, if time allows): the encryption ZTS tests that were
  not run on the branch's kernel module before.

## Rules

- Stop and report if `lsmod` shows `zfs` or `spl` before STEP 1, if
  `zpool list` lists any pool, or if `/var/tmp` has less than 25 GiB free.
- Never use a device: every vdev is a file under `/var/tmp/skc`.
- Run each block as written, in bash, as a user with passwordless sudo
  (ZTS refuses root). Keep each block's output: the report needs it.
- A kernel message containing `VERIFY`, `ASSERT`, `BUG` (the word, not
  `DEBUG`), `Oops`, `panic`, `hung task`, `leaked` or `WARNING:` ends the
  task: save `dmesg` to `$R/dmesg-stop.txt`, report, do not retry.
- Do not change anything in the trees beyond what the blocks do.

## STEP 0: environment

```sh
set -u
T=/var/tmp/skc; R=$T/results
mkdir -p $T $R
{
	date -u; uname -a; cat /etc/os-release | grep PRETTY
	lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket|NUMA node\(s\)|Hypervisor)'
	grep -E 'MemTotal|MemAvailable' /proc/meminfo
	cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "governor: n/a"
	cat /sys/kernel/mm/transparent_hugepage/enabled
	df -h /var/tmp | tail -1
	fio --version; ksh --version 2>&1 | head -1; gcc --version | head -1
	lsmod | grep -E '^(zfs|spl) ' || echo "no zfs module loaded"
	command -v zpool >/dev/null && sudo zpool list 2>&1 || echo "no system zpool"
} 2>&1 | tee $R/env.txt
```

Expected: `no zfs module loaded`, no pool, fio 3.x present. Install missing
build dependencies (kernel headers for the running kernel, autoconf,
automake, libtool, gcc, make, libssl-dev, libblkid-dev, uuid-dev,
libudev-dev, zlib1g-dev, libaio-dev, libtirpc-dev, python3, ksh, fio) with the
distribution's package manager if needed, and record what was installed in
`$R/env.txt`.

## STEP 1: the three trees

`B-dbg` is the branch built with `--enable-debug --enable-debug-kmem` (the
SPL then reports leaked kernel memory when it unloads), `B-rel` the branch in
a release build, `M-rel` master `0cdf531257` (the branch's base) in the same
release build.

```sh
T=/var/tmp/skc; R=$T/results; cd $T
git clone -q https://github.com/tigerblue77/zfs repo
cd repo && git fetch -q origin zio-crypt-salt-key-cache-v2 &&
    test "$(git rev-parse FETCH_HEAD)" = 4bf3e32d768bccaf3572a96b61264dd98b396d48 &&
    test "$(git rev-parse FETCH_HEAD~4)" = 0cdf531257e977b2f4b31a5823e7a5907c49d5a2 &&
    echo "branch head and base as expected" | tee -a $R/env.txt
git worktree add -q --detach $T/B-dbg FETCH_HEAD
git worktree add -q --detach $T/B-rel FETCH_HEAD
git worktree add -q --detach $T/M-rel 0cdf531257e977b2f4b31a5823e7a5907c49d5a2
for t in B-dbg B-rel M-rel; do
	cd $T/$t
	case $t in B-dbg) c="--enable-debug --enable-debug-kmem";; *) c="";; esac
	( ./autogen.sh && ./configure $c && make -j$(nproc) ) > $R/build-$t.log 2>&1
	echo "$t: rc=$? $(git log -1 --format=%h) configure '$c'" \
	    "warnings=$(grep -c 'warning:' $R/build-$t.log)" | tee -a $R/env.txt
done
```

Expected: three lines `rc=0`, each tree at its commit, `warnings=0`. About
15 minutes per tree on 4 CPUs.

Helpers, used by every later step (paste them into each new shell):

```sh
T=/var/tmp/skc; R=$T/results
KS=/proc/spl/kstat/zfs/zio_crypt_stats
# hits misses admissions evictions, or "absent" (master)
kst() { [ -r $KS ] && awk '$1 ~ /^salt_cache_/ { printf "%s ", $3 } END { print "" }' $KS || echo absent; }
# kstat difference between two kst outputs
kdiff() { echo "$1 $2" | awk '{ printf "hits +%d misses +%d admissions +%d evictions +%d\n", $5-$1, $6-$2, $7-$3, $8-$4 }'; }
# load the modules of tree $1, after unloading any; check they are its own
load() {
	sudo $T/$CUR/scripts/zfs.sh -u >/dev/null 2>&1
	lsmod | grep -q '^zfs ' && { echo "zfs still loaded"; return 1; }
	sudo dmesg -C
	sudo $T/$1/scripts/zfs.sh -v | tee -a $R/loads.txt
	CUR=$1; Z=$T/$1/zfs; P=$T/$1/zpool
	local want; want=$(modinfo -F srcversion $T/$1/module/zfs.ko)
	[ "$(cat /sys/module/zfs/srcversion)" = "$want" ] || { echo "wrong module"; return 1; }
	echo "loaded $1: srcversion $want zfs_flags $(cat /sys/module/zfs/parameters/zfs_flags) kstat $(kst)" | tee -a $R/loads.txt
}
kcheck() {   # kcheck <label>: kernel messages since the last load
	sudo dmesg > $R/dmesg-$1.txt
	if grep -E 'VERIFY|ASSERT|\bBUG\b|Oops|panic|hung task|leaked|WARNING:' $R/dmesg-$1.txt; then
		echo "KERNEL FINDING in $1: stop"; return 1
	fi
	echo "kernel $1: clean"
}
CUR=B-dbg
```

## STEP 2 (part A): data under many salts, then a key reload

```sh
load B-dbg || exit 1
cd $T && mkdir -p a mnt && truncate -s 3G a/v0
dd if=/dev/urandom of=$T/key1 bs=32 count=1 status=none
dd if=/dev/urandom of=$T/key2 bs=32 count=1 status=none
dd if=/dev/urandom of=$T/key3 bs=32 count=1 status=none
chmod 600 $T/key?
sudo $P create -o cachefile=none -O mountpoint=none skca $T/a/v0
sudo $Z create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$T/key1 -o recordsize=128k -o primarycache=metadata \
    -o compression=off -o atime=off -o mountpoint=$T/mnt skca/enc
sudo chown "$(id -un):" $T/mnt
# a new salt every 64 encrypted blocks: 20 files of 64 blocks, at least 20 salts
echo 64 | sudo tee /sys/module/zfs/parameters/zfs_key_max_salt_uses
for i in $(seq -w 1 20); do head -c 8M /dev/urandom > $T/mnt/f$i; done
sync; sudo $P sync skca
echo 400000000 | sudo tee /sys/module/zfs/parameters/zfs_key_max_salt_uses
(cd $T/mnt && sha256sum f*) > $R/A-sums.txt
reload() {   # unmount, unload and load the key: a new current salt
	sudo $Z unmount skca/enc && sudo $Z unload-key -r skca/enc &&
	    sudo $Z load-key -r skca/enc && sudo $Z mount -a && echo "key reloaded"
}
pass() {     # pass <label>: read every file of enc/ once, with the kstat around it
	local k0 k1; k0=$(kst)
	(cd $T/mnt && sha256sum -c --quiet $R/A-sums.txt) && echo "$1: 20 of 20 files read back"
	k1=$(kst); echo "$1: $(kdiff "$k0" "$k1")"
}
reload
pass A1; pass A2
kcheck A-reload
```

Expected: `key reloaded`; for `A1` and `A2`, `20 of 20 files read back`,
hits at least 1000, admissions at least 16, evictions at least 1 (more salts
than the 16 slots), misses well below hits (26 October, the same patches on
the previous master, first pass: hits +1309 misses +42 admissions +21
evictions +7).
`kernel A-reload: clean`.

## STEP 3 (part A): zfs change-key, loaded, then reloaded with the new key

```sh
sudo $Z change-key -o keylocation=file://$T/key2 skca/enc && echo "change-key done"
pass A3
reload; pass A4
sudo $Z load-key -n -L file://$T/key1 skca/enc 2>&1 | tail -1
sudo $Z load-key -n -L file://$T/key2 skca/enc 2>&1 | tail -1
kcheck A-change-key
```

Expected: `A3` reads back 20 of 20 with counts like `A2`'s (the loaded key
keeps its master key, and with it its cache, across `change-key`); `A4`
reads back 20 of 20 with counts like `A1`'s (a new cache with the reloaded
key); the `-n` check with `key1` fails (`Key load error: Incorrect key
provided`), with `key2` it succeeds; clean.

## STEP 4 (part A): an encryption root that becomes an inheriting child

```sh
sudo $Z create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$T/key3 -o mountpoint=$T/mnt/child skca/enc/child
sudo chown "$(id -un):" $T/mnt/child
echo 64 | sudo tee /sys/module/zfs/parameters/zfs_key_max_salt_uses
for i in 1 2 3 4; do head -c 8M /dev/urandom > $T/mnt/child/c$i; done
sync; sudo $P sync skca
echo 400000000 | sudo tee /sys/module/zfs/parameters/zfs_key_max_salt_uses
(cd $T/mnt/child && sha256sum c*) > $R/A-child-sums.txt
sudo $Z change-key -i skca/enc/child && sudo $Z get -H -o value encryptionroot skca/enc/child
k0=$(kst); (cd $T/mnt/child && sha256sum -c --quiet $R/A-child-sums.txt) && echo "child: read back (loaded)"
reload
(cd $T/mnt/child && sha256sum -c --quiet $R/A-child-sums.txt) && echo "child: read back (reloaded)"
(cd $T/mnt/child && sha256sum -c --quiet $R/A-child-sums.txt) && echo "child: read back (again)"
echo "child: $(kdiff "$k0" "$(kst)")"
pass A5
kcheck A-inherit
```

Expected: the encryption root printed is `skca/enc`; the three child reads
and `A5` read back; hits over the child reads at least 200; clean.

## STEP 5 (part A): concurrent readers, old salts and dedup blocks

```sh
sudo $Z create -o dedup=on skca/enc/dd
sudo chown "$(id -un):" $T/mnt/dd
for i in $(seq 1 8); do head -c 8M /dev/urandom > $T/mnt/dd/d$i; done
sync; (cd $T/mnt/dd && sha256sum d*) > $R/A-dd-sums.txt
reload
k0=$(kst); rc=0
for j in 1 2 3 4; do
	(for r in 1 2 3; do (cd $T/mnt && sha256sum -c --quiet $R/A-sums.txt) || exit 1; done) &
	(for r in 1 2 3; do (cd $T/mnt/dd && sha256sum -c --quiet $R/A-dd-sums.txt) || exit 1; done) &
done
for p in $(jobs -p); do wait $p || rc=1; done
echo "concurrent: rc=$rc $(kdiff "$k0" "$(kst)")"
sudo $P status -x skca
kcheck A-concurrent
```

Expected: `rc=0` (every copy of every file read back), hits in the tens of
thousands, misses at least 6144 (512 dedup blocks with a salt each, each read
12 times); admissions are only reported (readers of one dedup file close
together can miss its salts twice within 32 misses, which admits them);
`pool 'skca' is healthy`; clean.

## STEP 6 (part A): export, import, destroy, unload

```sh
sudo $Z unmount -a; sudo $P export skca
sudo $P import -d $T/a -o cachefile=none skca && sudo $Z load-key -r skca/enc &&
    sudo $Z mount -a && echo "imported"
pass A6
sudo $P scrub -w skca && sudo $P status -x skca
sudo $P destroy skca && echo "destroyed"
sudo $T/B-dbg/scripts/zfs.sh -u && echo "unloaded"
sleep 2; kcheck A-unload
```

Expected: `imported`, `A6` reads back, scrub finds nothing (`healthy`),
`destroyed`, `unloaded`, and no `leaked` line from the SPL in `dmesg`
(`--enable-debug-kmem`); clean.

Optional, recommended: reboot here, so that part B starts without the debug
module's history. After the reboot, paste the helpers again with
`CUR=B-dbg`.

## STEP 7 (part B): the benchmark pool, written once on M-rel

One file vdev, one encryption root with `recordsize=4k`,
`primarycache=metadata` (every read decrypts) and `compression=off`.
`old/` holds 8 files of 128 MiB (dedup off: a few salts, all older after the
key load), `dd/` 8 files of 64 MiB of random data with `dedup=on` (a salt per
block, never cached). Each round loads one tree, imports the pool, loads the
key (so `old/` is under older salts), warms up, and runs four phases of 30 s
of `fio --rw=randread --bs=4k --ioengine=psync`:

| Phase | Jobs | What it shows |
| --- | --- | --- |
| `old1` | 1 on `old/` | per-read cost on one thread |
| `oldN` | `nproc` on `old/` | throughput on cached older salts |
| `ddN` | `nproc` on `dd/` | what the misses cost (salt per block) |
| `mix` | `nproc/2` on each | old-salt reads beside dedup reads |

Recorded per phase: IOPS, mean latency, CPU time per read (all CPUs' busy
time from `/proc/stat` divided by reads), `%steal`, page-ins, and on the
branch the kstat's deltas.

```sh
load M-rel || exit 1
mkdir -p $T/b $T/bmnt && truncate -s 8G $T/b/v0
dd if=/dev/urandom of=$T/bkey bs=32 count=1 status=none && chmod 600 $T/bkey
sudo $P create -o cachefile=none -O mountpoint=none skcb $T/b/v0
sudo $Z create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$T/bkey -o recordsize=4k -o primarycache=metadata \
    -o compression=off -o atime=off -o mountpoint=$T/bmnt skcb/enc
sudo chown "$(id -un):" $T/bmnt && mkdir $T/bmnt/old $T/bmnt/dd
fio --name=w --directory=$T/bmnt/old --rw=write --bs=1m --size=128m --numjobs=8 \
    --end_fsync=1 --group_reporting > $R/B-write-old.txt
sudo $Z set dedup=on skcb/enc
fio --name=w --directory=$T/bmnt/dd --rw=write --bs=1m --size=64m --numjobs=8 \
    --refill_buffers --end_fsync=1 --group_reporting > $R/B-write-dd.txt
(cd $T/bmnt && sha256sum old/* dd/*) > $R/B-sums.txt
sudo $Z list -o name,used,dedup skcb/enc; sudo $P get -H -o value dedupratio skcb
sudo $Z unmount -a; sudo $P export skcb
kcheck B-prep
```

Expected: about 1 GiB in `old/` and 512 MiB in `dd/`, dedupratio `1.00x`;
clean.

## STEP 8 (part B): six rounds, alternating

```sh
N=$(nproc); H=$((N / 2)); [ $H -ge 1 ] || H=1; RT=30
# the 8 files STEP 7 wrote in each directory, read by every job
FL=$(printf 'w.%d.0:' 0 1 2 3 4 5 6 7); FL=${FL%:}
fiocmd() {   # fiocmd <dir> <jobs> <runtime> <json>
	fio --name=$1 --directory=$T/bmnt/$1 --filename=$FL --rw=randread --bs=4k \
	    --ioengine=psync --numjobs=$2 --time_based --runtime=$3 --group_reporting \
	    --allow_file_create=0 --output-format=json --output=$4 >/dev/null 2>&1
}
fioval() { python3 -I -c 'import json,sys
j=json.load(open(sys.argv[1]))["jobs"][0]["read"]
print(round(j["iops"]), j["total_ios"], "%.1f" % (j["lat_ns"]["mean"]/1000))' "$1"; }
cpu() { awk '/^cpu / { print $2+$3+$4+$7+$8, $9 }' /proc/stat; }
phase() {    # phase <round> <tree> <name>
	local r=$1 t=$2 ph=$3 c0 c1 k0 k1 g0 g1 ios=0 iops=0 lat='' v
	c0=$(cpu); k0=$(kst); g0=$(awk '$1=="pgpgin" {print $2}' /proc/vmstat)
	case $ph in
	old1) fiocmd old 1 $RT $R/fio-$r-$t-$ph.json ;;
	oldN) fiocmd old $N $RT $R/fio-$r-$t-$ph.json ;;
	ddN)  fiocmd dd $N $RT $R/fio-$r-$t-$ph.json ;;
	mix)  fiocmd old $H $RT $R/fio-$r-$t-mixold.json & fiocmd dd $H $RT $R/fio-$r-$t-mixdd.json; wait ;;
	esac
	c1=$(cpu); k1=$(kst); g1=$(awk '$1=="pgpgin" {print $2}' /proc/vmstat)
	for f in $([ $ph = mix ] && echo mixold mixdd || echo $ph); do
		v=$(fioval $R/fio-$r-$t-$f.json) || v="0 0 0"
		set -- $v
		iops=$((iops + $1)); ios=$((ios + $2)); lat="$lat $f:${3}us"
	done
	set -- $c0 $c1
	local busy=$(( $3 - $1 )) steal=$(( $4 - $2 ))
	local us=$(awk -v b=$busy -v n=$ios 'BEGIN { printf "%.2f", n ? b * 10000 / n : 0 }')
	local k="n/a"; [ "$k0" = absent ] || k=$(kdiff "$k0" "$k1")
	printf '%s\t%s\t%s\t%d\t%s\t%s\t%d\t%d\t%s\n' $r $t $ph $iops "$us" "${lat# }" $steal $((g1 - g0)) "$k" |
	    tee -a $R/B-rows.tsv
}
round() {    # round <round> <tree>
	load $2 || return 1
	sudo $P import -d $T/b -o cachefile=none skcb && sudo $Z load-key skcb/enc >/dev/null &&
	    sudo $Z mount -a || return 1
	fiocmd old $N 10 $R/warm-$1-$2-old.json; fiocmd dd $N 10 $R/warm-$1-$2-dd.json
	for ph in old1 oldN ddN mix; do phase $1 $2 $ph; done
	[ $1 = 6 ] && (cd $T/bmnt && sha256sum -c --quiet $R/B-sums.txt) && echo "round 6 $2: data read back"
	sudo $Z unmount -a && sudo $P export skcb
	kcheck B-$1-$2
}
printf 'round\ttree\tphase\tiops\tcpu_us_per_read\tlat\tsteal_ticks\tpgpgin_kib\tkstat\n' > $R/B-rows.tsv
for r in 1 2 3 4 5 6; do
	if [ $((r % 2)) = 1 ]; then o="M-rel B-rel"; else o="B-rel M-rel"; fi
	for t in $o; do echo "== round $r $t $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"; round $r $t || break 2; done
done
sudo $T/$CUR/scripts/zfs.sh -u
```

Expected: 48 rows (6 rounds x 2 trees x 4 phases), each with IOPS above 0;
`round 6 M-rel: data read back` and the same for `B-rel`; every `kcheck`
clean. On `B-rel`: `old1`/`oldN` hits about equal to the reads and few
misses; `ddN` misses about equal to the reads and admissions 0; `mix` both.
`pgpgin` near 0 after the warm-up (the vdev file stays in the page cache).
About 40 minutes.

## STEP 9: the table

```sh
python3 -I - $R/B-rows.tsv <<'EOF' | tee $R/B-table.txt
import csv, statistics, sys
rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
def stats(xs):
    m = statistics.median(xs)
    cv = 100 * statistics.pstdev(xs) / statistics.mean(xs) if statistics.mean(xs) else 0
    return m, min(xs), max(xs), cv
print("phase  tree   n  IOPS median [min-max] CV%       CPU us/read median [min-max] CV%")
for ph in ["old1", "oldN", "ddN", "mix"]:
    med = {}
    for t in ["M-rel", "B-rel"]:
        sel = [r for r in rows if r["phase"] == ph and r["tree"] == t]
        if not sel: continue
        i = stats([float(r["iops"]) for r in sel]); c = stats([float(r["cpu_us_per_read"]) for r in sel])
        med[t] = (i[0], c[0])
        print("%-6s %-6s %d  %8.0f [%.0f-%.0f] %4.1f   %7.2f [%.2f-%.2f] %4.1f" % ((ph, t, len(sel)) + i + c))
    if len(med) == 2:
        print("%-6s B/M    IOPS %+.1f%%, CPU per read %+.1f%%" % (ph,
            100 * (med["B-rel"][0] / med["M-rel"][0] - 1), 100 * (med["B-rel"][1] / med["M-rel"][1] - 1)))
EOF
```

## STEP 10 (part C, optional): ZTS on B-dbg

Only with at least 1 hour left. Tests that read encrypted data under older
salts and were not run on this code in a kernel before:

```sh
CUR=M-rel; load B-dbg || exit 1
cd $T/B-dbg && sudo ./scripts/zfs-helpers.sh -i
./scripts/zfs-tests.sh -v -T zfs_load-key,zfs_unload-key,zfs_change-key > $R/C-keys.txt 2>&1
for t in zpool_scrub_encrypted_unloaded zpool_import_encrypted zpool_import_encrypted_load \
    zpool_import_errata3 zpool_import_errata4 block_cloning_cross_enc_dataset \
    block_cloning_replay_encrypted zdb_encrypted zdb_encrypted_raw \
    send_encrypted_incremental send_raw_large_blocks; do
	./scripts/zfs-tests.sh -v -t $t > $R/C-$t.txt 2>&1
done
grep -h -E '^Test.*\[(PASS|FAIL|SKIP|KILLED)\]' $R/C-*.txt | awk '{ print $NF, $2 }' | sort | uniq -c
grep -h -E '^Test.*\[(FAIL|KILLED)\]' $R/C-*.txt
sudo ./scripts/zfs.sh -u; kcheck C
```

A test that fails here is rerun once, alone; if it fails again, build master
in a fourth tree (`--enable-debug`, as STEP 1) and run it there: a failure on
both is not the branch's.

## If something goes wrong

- A module that will not unload (`zfs still loaded`): `sudo reboot`, paste
  the helpers, and continue from the step that was running (part B: from
  the round that was running; a round's rows are complete or absent).
- A kernel finding: stop (see the rules), keep the module loaded if the
  system still answers, save `dmesg`, and report.
- A build failure: report the last 50 lines of the build log; do not patch.

## Report

`/var/tmp/skc/REPORT.txt`, written by hand, with:

1. `env.txt` whole.
2. Part A: for each of `A1`-`A6`, the `read back` line and the kstat line;
   the `child`, `concurrent` and `change-key` lines; every `kcheck` line;
   whether `dmesg` had any SPL `leaked` line at unload.
3. Part B: `B-table.txt` whole, the `== round` lines (with the load
   averages), the steal ticks of each round, and the two `data read back`
   lines.
4. Part C if run: the counts and every failing line.
5. Anything done differently from this text, and why.

Hand back `REPORT.txt` and the `results/` directory (`B-rows.tsv`, the
fio JSON files, the dmesg files) as they are.
