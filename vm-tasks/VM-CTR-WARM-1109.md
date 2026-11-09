# VM task: the per-CPU hot counters with a warm ARC (9/11)

These instructions are for VM 105 (or VM 106): 16 vCPUs in one NUMA node,
Debian 13, kernel 6.12, at least 13 GiB of available memory. STEP 0 stops on
any other shape. Reply with `/var/log/ctr-warm-1109/REPORT.txt` (STEP 7).

This is a performance run on the hypervisor the three test VMs share: run
STEPS 3 and 4 only while the other two test VMs are idle (you are told
when). STEPS 0 to 2 (checks and builds) may run before that.

**Background.** Two runs of the fork branch `percpu-hot-counters` (four
commits on master) disagree on cached reads:

- VM-CTR-AB-1108 (8/11, VM 105; one module per pass, loaded once, the ARC
  warm from the layout; next to load on the other VMs, B run once) found a
  gain: 8 jobs +18.6 to +25.7 %, 16 jobs +9.8 to +18.2 %, 4 jobs +19.4 %
  (noise 3.4 %); on the 8- and 16-job rows every B sample was above every A
  sample.
- VM-CTR-BISECT-1108 (9/11, VM 105, quiet host; the module reloaded and the
  ARC empty before every run) found none: B1234 against A, 8 jobs -3.4 % on
  average (-15.7 to +5.0), 16 jobs -3.8 %, carried by commit 3 (its step
  -4.3 and -6.0 %, the only one beyond the noise). 16-job uncached reads
  (-5.4 %) and 1-job mirror writes (-6.1 %) lost too, with no single commit
  beyond the noise; the sync-write losses of VM-CTR-AB-1108 did not hold.

The two runs differ in the state of the ARC, and the bisect's own notes say
that an effect needing a long-lived ARC could not show there. This run keeps
the bisect's method (quiet host, interleaved arms, steal and load per run)
with a warm ARC, to answer two questions: is the cached-read gain real, and
what does commit 3 do to cached reads once the ARC is warm.

**What is compared.** Four builds, from one source tree:

| arm | commit | what it is |
|---|---|---|
| `A` | `ef35f144e1d0df22e835970dc9bc5615f4d1df48` | upstream master, the commit the branch sits on |
| `B12` | `ee45ef6ecaa6faf00e7378893458f069538d7bd7` | + (1) `arcs_esize[]`: atomic -> wmsum, (2) `arcs_size[]`: atomic -> wmsum, arc_anon -> aggsum (lower bound in `arc_tempreserve_space()`) |
| `B123` | `0db849c2161f8d2b8a45d94efd3aee3a8f65a347` | + (3) `dbuf_caches[].size`: atomic -> aggsum; mirror rows only |
| `B1234` | `e0ba7d8c779be98fcf3acaefd91f63960155d1fe` | + (4) `vdev_stat_update()` skips `vdev_stat_lock` of interior vdevs on success; the B of VM-CTR-AB-1108 |

All four are on the fork branch `percpu-hot-counters-bisect`, as in
VM-CTR-BISECT-1108; nothing is rebased. B12 against A is commits 1 and 2
(commit 2 is written on top of commit 1, which is not run alone here). B1234
against B12 is commits 3 and 4; commit 4 changes only mirror, RAID-Z and
dRAID vdevs, so on a single-device pool it is commit 3 alone. B123 runs only
the mirror rows, where it splits commit 3 from commit 4.

**Rows.** The three cached-read rows of the question, the rows that lost in
one of the two runs, and two null controls, each the same workload as in
VM-CTR-AB-1108 (16K records, per-job files, psync, compression off, 50 %
compressible data), in the order a slot runs them:

| row | role | pool | fio | jobs | VM-CTR-AB-1108 | VM-CTR-BISECT-1108, B1234/A |
|---|---|---|---|---|---|---|
| `mir-uread-1` | C | mirror, `nocache` | randread 16k | 1 | -1.9 % | no loss |
| `mir-uread-16` | L | mirror, `nocache` | randread 16k | 16 | -5.6 %, -7.8 % (perf record) | no loss |
| `mir-awrite-1` | L | mirror, `wplain` | randwrite 16k | 1 | -13.0 % (noise 15.3) | -6.1 %, no single commit |
| `cread-8` | G | main, `plain` | randread 4k | 8 | +18.6 to +25.7 % | -3.4 %, does not hold |
| `cread-16` | G | main, `plain` | randread 4k | 16 | +9.8 to +18.2 % | -3.8 %, commit 3 |
| `scale-cread-4` | G | main, `plain` | randread 4k | 4 | +19.4 % (noise 3.4) | not run |
| `uread-1` | C | main, `nocache` | randread 16k | 1 | -0.4 % | no loss |
| `uread-16` | L | main, `nocache` | randread 16k | 16 | +9.4 %, -7.8 % (perf record), -16.0 % (perf lock) | -5.4 %, no single commit |
| `swrite-4` | L | sync, `sync` (sync=always) | randwrite 16k | 4 | -8.6 % | no loss |

G: the cached-read gain of VM-CTR-AB-1108, the question. L: a row that lost
in one of the two runs. C: 1-job reads that moved in neither, null controls;
they also set a floor under every other verdict. `nocache` datasets have
`primarycache=metadata`. Each run is 20 s after a 5 s ramp, no `perf`.

**The warm regime.** Each arm runs in slots. A slot exports the phase's
pools, loads the arm's modules (always, also when they are the loaded ones,
so that every slot starts the same way), imports the pools, reads every
file of the phase once in a fixed order (the warm-up), then runs the arm's
rows back to back. In phase P the warm-up reads the per-job files of
`nocache` on both pools (only their metadata stays in the ARC), the two 512
MiB shared files, the mirror's `wplain` files, then the per-job files of
`wplain` and `plain`: 82 files and 1664 MiB of cached data, the rows' own
data last. That is the cached data the layout of VM-CTR-AB-1108 wrote
through the ARC before its first row, and it grows the ARC target `c` the
same way: from reading `arc_adapt()`, `c` starts at `c_min` (1/32 of the
memory, about 0.5 GiB here) and grows by what is added to the ARC while the
ARC is within 32 MiB of it; without memory pressure nothing lowers it.
VM-CTR-AB-1108 had `c` at 1.13 GiB (pass A1) and 1.90 GiB (B, A2) at the end
of its layout; the bisect started every run at `c_min`.

`c` matters to commit 3 beyond the hit rate (from reading `dbuf.c`): the
dbuf cache's target is `c`/32. A cached read takes its dbuf out of that cache
and puts it back on release, updating the cache's size counter (the counter
commit 3 turns into an aggsum); every release compares the counter with the
target, and while the cache is over it the eviction thread compares the
counter with the low-water mark for every dbuf it evicts. With `c` at 0.5
GiB the target is about 16 MiB; at 1.75 GiB it is 56 MiB, against 64, 128
and 256 MiB of 16K records read by `scale-cread-4`, `cread-8` and
`cread-16`. The two regimes run commit 3's code at different eviction rates;
the `dbc_evicts` column counts the evictions.

Phase S holds only the sync pool. A 1 GiB file that no row reads is laid out
next to the sync files and read first by the warm-up (17 files, 1280 MiB),
so that `c` grows there too.

Every run records the ARC size and target and the hit rate since the module
was loaded, at its start and at its end, the run's own demand-data hit rate
and misses, the dbuf cache target and the dbuf cache evictions of the run.
A cached-read run (G) that misses more than 1 % of its records, or a run
that starts with an ARC more than 10 % off A's on the same row and round, is
flagged, and the verdicts that use it say `warm state differs`. A warm-up
that leaves the ARC holding less than 80 % of the cached data it read stops
the run (exit 7): that would be the bisect's regime again.

**How the arms are interleaved: per round.** A round gives each arm one
slot, in the order A, B12, B123, B1234 in odd rounds and B1234, B123, B12, A
in even rounds (phase S has no B123); four rounds. B123 runs only the mirror
rows, which come first in every slot, right after the warm-up, so that they
follow the same history in every arm. This is still a fair comparison,
although the two arms of a pair run minutes apart instead of back to back:

- A warm ARC does not survive a change of arm: loading another build means
  unloading the module, which empties the ARC. Row by row, every run starts
  on an empty ARC (the bisect), or needs its own warm-up and then starts
  from a history that depends on which arm ran before.
- Every row starts from the same history in every arm: a freshly loaded
  module, the same warm-up, then the same rows in the same order. The
  `arc0_MiB`, `c0_MiB` and `dmiss` columns check that the ARC is in the same
  state.
- The arm order reverses from one round to the next, so over two consecutive
  rounds every arm has the same mean position: host load that drifts steadily
  over a round pair weighs on every arm alike. Four rounds are two such
  pairs. (B123's slot is shorter, so its mirror rows run on average about a
  minute later than the other arms'; a drift of 0.1 % per minute would bias
  its pairs by about 0.1 %.)
- Each comparison is made within one round, and a verdict needs the same
  sign in every round: load that hits one slot moves one round's difference
  and makes the verdict unclear, not wrong.
- Steal and load are recorded per round, per slot and per run, and a busy
  host stops the run.

What it costs: a drift within a round is cancelled over the round pair, not
within the round; the threshold carries it through h, half the spread of the
rounds' differences.

**Verdicts** (`compare.py`), per row and per commit pair X/Y:

| pair | X/Y | rows |
|---|---|---|
| `all four` | B1234/A | all |
| `c1+c2` | B12/A | all |
| `c3` | B1234/B12 on a single-device pool (commit 4 changes nothing there), B123/B12 on the mirror | all |
| `c4` | B1234/B123 | mirror |
| `c3+c4` | B1234/B12 | mirror |

For each round r in which both arms ran the row, d_r is X against Y in %;
m is the mean of the d_r, h half their range (max - min) / 2, f the largest
|m| of the same pair on the two null-control rows (0 for the controls
themselves), and t = max(3, h, f).

- **GAIN**: at least three rounds, every d_r > 0 and m > t.
- **LOSS**: at least three rounds, every d_r < 0 and m < -t.
- **none**: at least three rounds, |m| <= 3, h <= 3 and no run of the two
  arms on that row with `steal_pct` >= 1.
- **unclear**: anything else. `(steal)` is added when a run of the pair had
  `steal_pct` >= 1, `warm state differs` when one of its runs was flagged.

The answers close `compare.txt`. The cached-read gain reproduces when `all
four` is GAIN on `cread-8`, `cread-16` and `scale-cread-4` with no
warm-state flag, is not reproduced when it is GAIN on none of them, and
partly reproduces otherwise; there is no answer when one of them has fewer
than three rounds. Commit 3 is read from its `c3` verdicts, on the three
cread rows first; commits 1+2 and commit 4 are listed the same way.

**Stop rules.** (1) After every round, a phase in which a third or more of
the runs so far had `steal_pct` >= 1 stops (exit 5: the host is busy), and
STEP 4 is not run: report, it will be rerun on a quiet host. (2) A warm-up
that leaves the ARC under 80 % of the cached data it read stops (exit 7).
(3) The safety stops under Rules; every slot ends with a check of the pools'
health and of the kernel log. (4) With fewer than three complete rounds,
every verdict is unclear.

**Noise records.** Per run: `steal_pct` (the share of the time the vCPUs
had work that the hypervisor gave to others), the steal jiffies and the load
average just before the run. Per round and per slot: the load average and
the steal and total jiffies at the start and at the end (`rounds.txt`,
summarised by STEP 5).

## The pools

Those of VM-CTR-BISECT-1108, laid out the same way (VM-CTR-AB-1108's "How
the pools are sized" gives the bounds), plus one file in phase S. Phase P:
an 8 GiB RAM disk holding a 6 GiB single-device pool (`zfswarm`: `plain`,
`wplain`, `nocache`, `nocachelz4`, 16 files of 16 MiB each, plus the two 512
MiB shared files, which only the warm-up reads) and a 2 x 1000 MiB mirror
(`zfswarmm`: `nocache`, `wplain`, 16 files of 8 MiB). Phase S: a RAM disk
sized from `zfs_wrlog_data_max` holding `zfswarms/sync` (16 files of 16 MiB,
sync=always) and `zfswarms/warm` (one 1 GiB file). The sync pool is sized for
1.26 GiB live, at most 0.75 x `zfs_wrlog_data_max` of log in flight
(VM-CTR-AB-1108's bound) and three txgs of the 64 MiB that 4 jobs rewrite,
under 50 % of the pool: 8 GiB on a 15 GiB VM (`zfs_wrlog_data_max` about 3.1
GiB, bound 3.8 GiB of 7.75, 49 %; VM-CTR-AB-1108's `swrite-4` reached 20.7 %
of its pool above the live data, which gives about 37 % here).
`spa_asize_inflation` stays at its default 24 (checked at every module
load). Expected after layout: main pool about 34 %, mirror about 26 %, sync
pool about 16 %. Every run checks 60 % before it starts, every write run has
a watchdog that stops fio above 60 %, and a pool above 50 % after its layout
stops the phase. The RAM disk is filled before any slot loads its module.
The pools are exported and imported at every slot; they persist on the RAM
disk and are destroyed at the end of their phase.

**Duration** (16 vCPUs):

| step | estimate |
|---|---|
| STEP 0 to 2 | 12 min: A built in full (about 4 min with `make -j8`, as on 8/11), each B arm's modules rebuilt in the same tree (1 to 2 min each) |
| STEP 3, phase P | setup 3 min; 4 rounds of about 13.5 min (three slots of about 4 min: module switch and import about 10 s, warm-up up to 15 s, 8 runs of about 27 s; one B123 slot of about 1.7 min, 3 runs): about 57 min, 108 runs |
| STEP 4, phase S | setup 2 min; 4 rounds of three slots of about 40 s (1 run each): about 10 min, 12 runs |
| STEP 5 to 7 | 5 min |

In all about 1 h 25 min, of which about 67 min of performance run (STEPS 3
and 4); about 1 h 40 min if module switches and warm-ups take twice the time
estimated. A busy host stops it earlier (stop rule 1).

## Rules

- Disposable VM, release builds, no other build, test or benchmark running.
  Builds run `nice -n 10 ionice -c3`, one at a time.
- STEPS 3 and 4 are a performance run: only while the other two test VMs
  are idle (you are told when). Do not start them otherwise.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), no `zfs-tests.sh`, never reboot.
- Work only under `/var/tmp/ctr-warm-1109` (W) and `/var/log/ctr-warm-1109`
  (L), on `/dev/ram0` and its partitions and the pools `zfswarm`,
  `zfswarmm`, `zfswarms`. Never run `zpool`, `zfs` or `zdb` while none of
  this task's modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A panic, oops, `VERIFY`, `ASSERT`, `BUG:` or
  hung-task message, a pool above 60 %, a suspended pool or an ENOSPC message:
  stop and report. After a stop past STEP 1, go to STEP 5 with what you have.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: which VM, and is it free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
numactl -H | sed -n '1,3p'
grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release
grep -E 'MemTotal|MemAvailable' /proc/meminfo
cat /proc/loadavg; grep '^cpu ' /proc/stat
ls -d "/lib/modules/$(uname -r)/build"
for t in fio numactl python3 git make gcc sfdisk udevadm blockdev dd findmnt modinfo insmod rmmod; do PATH=/usr/sbin:/sbin:$PATH command -v $t > /dev/null || echo "missing $t"; done
grep -E '^(zfs|spl|brd) ' /proc/modules                         # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/ctr-warm-1109 /var/log/ctr-warm-1109 2>&1        # No such file or directory, twice
df -h /var/tmp | tail -1                                       # 5G free or more
```

Stop, changing nothing, if: `sudo -n true` fails; the system is not Debian
13 (`VERSION_ID="13"`) on a 6.12 kernel; the VM is not 16 vCPUs in one node
(`available: 1 nodes (0)`, `node 0 cpus: 0 ... 15`); MemAvailable is under
13 GiB; the kernel headers or a tool are missing; a zfs, spl or brd
module is loaded or a pool is imported; a build, test or benchmark runs; W or
L exists; less than 5G is free on `/var/tmp`.

## STEP 1: sources and the four builds (about 10 min)

A is built in full (tools and modules); each B arm checks out its commit in
the same tree and rebuilds only the kernel modules, which recompiles the
files that changed since the previous arm. Each arm's `spl.ko` and `zfs.ko`
are kept in `$W/ko/<arm>`.

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/ctr-warm-1109 L=/var/log/ctr-warm-1109
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W/ko || exit 1
sudo -n dmesg | wc -l > $L/dmesg.start
git init -q $W/repo && git -C $W/repo fetch -q --depth 8 https://github.com/tigerblue77/zfs percpu-hot-counters-bisect || exit 1
git -C $W/repo rev-parse FETCH_HEAD                  # e0ba7d8c779be98fcf3acaefd91f63960155d1fe
git -C $W/repo worktree add -q --detach $W/tree ef35f144e1d0df22e835970dc9bc5615f4d1df48 || exit 1
{
cd $W/tree || exit 1
for t in A:ef35f144e1d0df22e835970dc9bc5615f4d1df48 B12:ee45ef6ecaa6faf00e7378893458f069538d7bd7 B123:0db849c2161f8d2b8a45d94efd3aee3a8f65a347 B1234:e0ba7d8c779be98fcf3acaefd91f63960155d1fe; do
	n=${t%%:*} c=${t#*:}
	git checkout -q --detach "$c" || break
	t0=$(date +%s)
	if [ "$n" = A ]; then
		nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j8' > "$L/build-$n.log" 2>&1
	else
		nice -n 10 ionice -c3 make -C module -j8 > "$L/build-$n.log" 2>&1
	fi
	rc=$?
	echo "$n at $(git rev-parse HEAD) build rc=$rc wall=$(( $(date +%s) - t0 ))s"
	[ "$rc" = 0 ] || break
	mkdir -p "$W/ko/$n" && cp module/spl.ko module/zfs.ko "$W/ko/$n/" || break
	echo "$n zfs.ko srcversion=$(modinfo -F srcversion "$W/ko/$n/zfs.ko") vermagic=$(modinfo -F vermagic "$W/ko/$n/zfs.ko" | cut -d' ' -f1)"
done
echo "release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' Makefile) noinline=$(grep -c '^KERNEL_DEBUG_CFLAGS = .*-fno-inline' Makefile)"
for t in B12:arc B12:arc_os B123:dbuf B1234:vdev; do
	n=${t%%:*} f=${t#*:}
	echo "$n recompiled $f.o: $(grep -cE "CC \[M\] .*[ /]$f\.o\$" "$L/build-$n.log" 2>/dev/null)"
done
} 2>&1 | tee $L/builds.txt
```

Expected: four `at <its commit> build rc=0` lines; four different
srcversions; `vermagic` = `uname -r` four times; `release=1 noinline=0`;
every `recompiled` count 1 or more. On a failed build quote `grep -nE
'error:|Error [0-9]' $L/build-<arm>.log | head -20` and stop.

## STEP 2: the scripts

`env.sh` holds the settings and `rows.txt` the rows, in the order a slot
runs them; `arm.sh` loads an arm's modules and checks them; `run.sh` runs
one row on the loaded arm; `slot.sh` one arm's slot (module, import,
warm-up, rows); `phase.sh` one pool phase (layout, the rounds, cleanup);
`compare.py` the tables and verdicts.

```sh
L=/var/log/ctr-warm-1109
cat > $L/env.sh << 'EOF_ENV'
# env.sh: settings of the warm-ARC run, sourced by every script.
# shellcheck disable=SC2034
export PATH=/var/tmp/ctr-warm-1109/tree:/usr/sbin:/sbin:$PATH
W=/var/tmp/ctr-warm-1109
L=/var/log/ctr-warm-1109
K=$W/ko
P=zfswarm
PM=zfswarmm
PS=zfswarms
RUNTIME=20
RAMP=5
ROUNDS=4
# The warm-up reads every file of these patterns once, in this order,
# relative to W: metadata-only datasets and files no row reads first, the
# rows' own data last.
WARM_P="mnt/nocache/f.* mntm/nocache/f.* mnt/plain/shared.0 mnt/wplain/shared.0 mntm/wplain/f.* mnt/wplain/f.* mnt/plain/f.*"
WARM_S="mnts/warm/ballast.0 mnts/sync/f.*"
# Files and cacheable MiB each warm-up reads; the ARC must hold 80 % of
# those MiB once it is done.
WARM_P_FILES=82
WARM_P_MIB=1664
WARM_S_FILES=17
WARM_S_MIB=1280
# mark PHASE ROUND ARM|- start|end: the time, the load average and the
# steal and all jiffies of all vCPUs, one line in rounds.txt.
mark() {
	echo "phase $1 round $2 arm $3 $4 $(date -u +%T)" \
	    "loadavg $(cut -d' ' -f1-3 /proc/loadavg)" \
	    "$(awk '/^cpu / { printf "steal_jiffies %.0f all_jiffies %.0f", $9, $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9 }' /proc/stat)" |
	    tee -a "$L/rounds.txt"
}
EOF_ENV
cat > $L/rows.txt << 'EOF_ROWS'
mir-uread-1 P C mntm/nocache randread 16k 1 8m
mir-uread-16 P L mntm/nocache randread 16k 16 8m
mir-awrite-1 P L mntm/wplain randwrite 16k 1 8m
cread-8 P G mnt/plain randread 4k 8 16m
cread-16 P G mnt/plain randread 4k 16 16m
scale-cread-4 P G mnt/plain randread 4k 4 16m
uread-1 P C mnt/nocache randread 16k 1 16m
uread-16 P L mnt/nocache randread 16k 16 16m
swrite-4 S L mnts/sync randwrite 16k 4 16m
EOF_ROWS
cat > $L/arm.sh << 'EOF_ARM'
#!/bin/bash
# arm.sh ARM: unload the loaded spl and zfs modules, whichever build they
# are (no pool may be imported), load ARM's, check what got loaded and that
# spa_asize_inflation is 24, and record ARM as the loaded build.
set -u
. /var/log/ctr-warm-1109/env.sh
a=$1 D=$K/$1
[ -s "$D/zfs.ko" ] && [ -s "$D/spl.ko" ] || { echo "no modules for $a: stopping"; exit 1; }
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2> /dev/null)" ] ||
    { echo "a pool is imported: stopping"; exit 1; }
rm -f "$W/arm"
for m in zfs spl; do
	if [ -d "/sys/module/$m" ]; then
		rmmod "$m" || { echo "rmmod $m failed: stopping"; exit 1; }
	fi
done
modprobe -q zlib_deflate
modprobe -q zlib_inflate
insmod "$D/spl.ko" && insmod "$D/zfs.ko" || { echo "load of $a failed: stopping"; exit 1; }
udevadm settle
[ -c /dev/zfs ] || { echo "no /dev/zfs after loading $a: stopping"; exit 1; }
for m in spl zfs; do
	want=$(modinfo -F srcversion "$D/$m.ko")
	have=$(cat "/sys/module/$m/srcversion")
	[ "$want" = "$have" ] || { echo "$m loaded $have is not $a's $want: stopping"; exit 1; }
done
[ "$(cat /sys/module/zfs/parameters/spa_asize_inflation)" = 24 ] ||
    { echo "spa_asize_inflation is not 24: stopping"; exit 1; }
echo "$a" > "$W/arm"
EOF_ARM
cat > $L/run.sh << 'EOF_RUN'
#!/bin/bash
# run.sh ROUND ROW ARM: one fio run of ROW (rows.txt) on the loaded build,
# which must be ARM's (slot.sh loads it). The pool's usage is checked
# against 60 % before the run and every 2 s of a write run (fio is stopped
# above it). arcstats and dbufstats are read just before and just after the
# run (kept in raw/); one line goes to results.txt.
set -u
. /var/log/ctr-warm-1109/env.sh
rnd=$1 row=$2 arm=$3
read -r ph _ dir rw bs nj fsize < <(awk -v r="$row" '$1 == r { print $2, $3, $4, $5, $6, $7, $8 }' "$L/rows.txt")
[ -n "${fsize:-}" ] || { echo "unknown row $row: stopping"; exit 1; }
[ "$(cat "$W/arm" 2> /dev/null)" = "$arm" ] || { echo "$arm is not the loaded build: stopping"; exit 1; }
case $dir in
mnt/*) pool=$P ;;
mntm/*) pool=$PM ;;
*) pool=$PS ;;
esac
[ "$(findmnt -n -o SOURCE --target "$W/$dir")" = "$pool/${dir#*/}" ] && [ -f "$W/$dir/f.0" ] ||
    { echo "$W/$dir is not $pool/${dir#*/} with its files: stopping"; exit 1; }
pct() { zpool list -Hp -o size,alloc "$pool" | awk '{ printf "%.1f", 100 * $2 / $1 }'; }
u=$(pct)
awk -v u="$u" 'BEGIN { exit !(u + 0 <= 60) }' ||
    { echo "pool $pool $u % full before $row: stopping"; exit 3; }
# jiffies of all vCPUs: busy (steal excluded), idle + iowait, steal
cpu() { awk '/^cpu / { printf "%.0f %.0f %.0f\n", $2 + $3 + $4 + $7 + $8, $5 + $6, $9 }' /proc/stat; }
quota() { awk '$1 == "dmu_tx_quota" { print $3 }' /proc/spl/kstat/zfs/dmu_tx; }
# ARC size, target, all hits, all misses, demand data hits and misses
arc() { awk '$1 == "size" { s = $3 } $1 == "c" { c = $3 } $1 == "hits" { h = $3 }
    $1 == "misses" { m = $3 } $1 == "demand_data_hits" { dh = $3 }
    $1 == "demand_data_misses" { dm = $3 }
    END { printf "%.0f %.0f %.0f %.0f %.0f %.0f\n", s, c, h, m, dh, dm }' /proc/spl/kstat/zfs/arcstats; }
# dbuf cache target and evictions so far
dbs() { awk '$1 == "cache_target_bytes" { t = $3 } $1 == "cache_total_evicts" { e = $3 }
    END { printf "%.0f %.0f\n", t, e }' /proc/spl/kstat/zfs/dbufstats; }
o=$L/raw/$ph.$rnd.$row.$arm
la=$(cut -d' ' -f1 /proc/loadavg)
cat /proc/spl/kstat/zfs/arcstats > "$o.arc0"
cat /proc/spl/kstat/zfs/dbufstats > "$o.dbuf0"
read -r s0 c0 h0 m0 dh0 dm0 < <(arc)
read -r t0 e0 < <(dbs)
q0=$(quota)
read -r b0 i0 st0 < <(cpu)
fio --name=t --directory="$W/$dir" --filename_format='f.$jobnum' \
    --size="$fsize" --rw="$rw" --bs="$bs" --ioengine=psync --numjobs="$nj" \
    --time_based --runtime="$RUNTIME" --ramp_time="$RAMP" --group_reporting \
    --fallocate=none --refill_buffers --buffer_compress_percentage=50 \
    --output-format=json --output="$o.fio.json" > "$o.fio.log" 2>&1 &
fpid=$!
wpid=""
case $rw in
*write*)
	(
		peak=0
		while kill -0 "$fpid" 2> /dev/null; do
			sleep 2
			u=$(pct)
			peak=$(awk -v a="$peak" -v b="$u" 'BEGIN { print (b + 0 > a + 0) ? b : a }')
			echo "$peak" > "$o.peak"
			if awk -v u="$u" 'BEGIN { exit !(u + 0 > 60) }'; then
				echo "pool $pool $u % full during $row: fio stopped" > "$o.stopped"
				kill -INT "$fpid"
				break
			fi
		done
	) &
	wpid=$!
	;;
esac
wait "$fpid"
frc=$?
[ -n "$wpid" ] && wait "$wpid"
read -r b1 i1 st1 < <(cpu)
q1=$(quota)
read -r s1 c1 h1 m1 dh1 dm1 < <(arc)
read -r _ e1 < <(dbs)
cat /proc/spl/kstat/zfs/arcstats > "$o.arc1"
cat /proc/spl/kstat/zfs/dbufstats > "$o.dbuf1"
fio_cols=$(python3 -I - "$o.fio.json" "$rw" << 'PY'
import json, sys
try:
    j = json.load(open(sys.argv[1]))["jobs"][0]
    x = j["read" if "read" in sys.argv[2] else "write"]
    p = x.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0)
    print("%.0f %.1f %.0f" % (x["iops"], x["bw"] / 1024.0, p / 1000.0))
except Exception:
    print("nan nan nan")
PY
)
read -r iops _ _ <<< "$fio_cols"
db=$((b1 - b0)) di=$((i1 - i0)) ds=$((st1 - st0))
busy=$(awk -v b="$db" -v i="$di" -v s="$ds" 'BEGIN { printf "%.0f", 100 * b / (b + i + s) }')
# steal: share of the time the vCPUs had work (busy + steal) the host gave away
steal=$(awk -v b="$db" -v s="$ds" 'BEGIN { printf "%.2f", (b + s > 0 ? 100 * s / (b + s) : 0) }')
# ARC size and target before and after (MiB), the hit rate since the module
# was loaded before and after (%), and the run's own demand data hit rate
# (%) and misses, dbuf cache target before (MiB) and evictions in the run.
arc_cols=$(awk -v s0="$s0" -v s1="$s1" -v c0="$c0" -v c1="$c1" -v h0="$h0" -v h1="$h1" \
    -v m0="$m0" -v m1="$m1" -v dh0="$dh0" -v dh1="$dh1" -v dm0="$dm0" -v dm1="$dm1" \
    -v t0="$t0" -v e0="$e0" -v e1="$e1" 'function pc(a, b) { return a + b > 0 ? 100 * a / (a + b) : 0 }
    BEGIN { M = 1048576; dh = dh1 - dh0; dm = dm1 - dm0
    printf "%.0f %.0f %.0f %.0f %.2f %.2f %.2f %.0f %.0f %.0f", s0 / M, s1 / M, c0 / M, c1 / M,
        pc(h0, m0), pc(h1, m1), pc(dh, dm), dm, t0 / M, e1 - e0 }')
peak=$(cat "$o.peak" 2> /dev/null || echo -)
line="$rnd $ph $row $arm $fio_cols $busy $steal $ds $((q1 - q0)) $peak $la $arc_cols $(date -u +%T)"
echo "$line" >> "$L/results.txt"
echo "$line"
if [ -e "$o.stopped" ]; then
	cat "$o.stopped"
	exit 4
fi
[ "$frc" = 0 ] || { echo "fio exited $frc (see $o.fio.log): stopping"; exit 1; }
[ "$iops" != nan ] || { echo "no IOPS in $o.fio.json: stopping"; exit 1; }
EOF_RUN
cat > $L/slot.sh << 'EOF_SLOT'
#!/bin/bash
# slot.sh PHASE ROUND ARM: one arm's turn in a round. Export the phase's
# pools, load ARM's modules (always, even if they are the loaded ones, so
# that every slot starts on a fresh module and an empty ARC), import the
# pools, read the warm-up files once in a fixed order, record the ARC after
# it (warm.txt), then run the arm's rows of the phase back to back in
# rows.txt order. B123 runs only the mirror rows, which come first in
# rows.txt, so that they follow the same history in every arm. Steal and
# load are recorded at the start and end of the slot (rounds.txt); the
# slot ends with a check of the pools' health and of the kernel log.
set -u
. /var/log/ctr-warm-1109/env.sh
ph=$1 rnd=$2 arm=$3
case $ph in
P)
	pools=("$P" "$PM")
	warm=$WARM_P wfiles=$WARM_P_FILES wmib=$WARM_P_MIB
	;;
S)
	pools=("$PS")
	warm=$WARM_S wfiles=$WARM_S_FILES wmib=$WARM_S_MIB
	;;
*) echo "usage: slot.sh P|S ROUND ARM"; exit 1 ;;
esac
mark "$ph" "$rnd" "$arm" start > /dev/null
zpool export "${pools[@]}" || { echo "export of ${pools[*]} failed: stopping"; exit 1; }
"$L/arm.sh" "$arm" || exit 1
for p in "${pools[@]}"; do
	zpool import -d /dev -o cachefile=none "$p" ||
	    { echo "import of $p failed: stopping"; exit 1; }
done
cd "$W" || exit 1
files=()
for pat in $warm; do
	# shellcheck disable=SC2206
	files+=($pat)
done
[ "${#files[@]}" = "$wfiles" ] ||
    { echo "warm-up: ${#files[@]} files instead of $wfiles: stopping"; exit 1; }
t0=$(date +%s%N)
for f in "${files[@]}"; do
	[ -f "$f" ] && dd if="$f" of=/dev/null bs=1M status=none ||
	    { echo "warm-up read of $W/$f failed: stopping"; exit 1; }
done
t1=$(date +%s%N)
# seconds, then ARC size, target, MRU and MFU sizes and dbuf cache target (MiB)
wcols=$(awk -v a="$t0" -v b="$t1" '$1 == "size" { s = $3 } $1 == "c" { c = $3 }
    $1 == "mru_size" { r = $3 } $1 == "mfu_size" { f = $3 } END {
    printf "%.1f %.0f %.0f %.0f %.0f", (b - a) / 1e9, s / 1048576, c / 1048576,
        r / 1048576, f / 1048576 }' /proc/spl/kstat/zfs/arcstats)
dbt=$(awk '$1 == "cache_target_bytes" { printf "%.0f", $3 / 1048576 }' /proc/spl/kstat/zfs/dbufstats)
line="$rnd $ph $arm ${#files[@]} $wcols $dbt $(date -u +%T)"
echo "$line" >> "$L/warm.txt"
echo "warm-up: $line"
read -r _ asz _ <<< "$wcols"
awk -v s="$asz" -v m="$wmib" 'BEGIN { exit !(s + 0 >= 0.8 * m) }' ||
    { echo "the ARC holds $asz MiB after a warm-up of $wmib MiB of cached data: stopping"; exit 7; }
mapfile -t myrows < <(awk -v ph="$ph" -v a="$arm" \
    '$2 == ph && (a != "B123" || $4 ~ /^mntm\//) { print $1 }' "$L/rows.txt")
[ "${#myrows[@]}" -gt 0 ] || { echo "no rows for $arm in phase $ph: stopping"; exit 1; }
for row in "${myrows[@]}"; do
	"$L/run.sh" "$rnd" "$row" "$arm" < /dev/null || exit $?
done
for p in "${pools[@]}"; do
	h=$(zpool list -H -o health "$p")
	[ "$h" = ONLINE ] || { echo "pool $p is $h: stopping"; exit 1; }
done
n=$(dmesg | tail -n +$(( $(cat "$L/dmesg.start") + 1 )) |
    grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|suspended|ENOSPC')
[ "$n" = 0 ] || { echo "$n kernel messages since STEP 1 (see dmesg): stopping"; exit 1; }
mark "$ph" "$rnd" "$arm" end > /dev/null
EOF_SLOT
cat > $L/phase.sh << 'EOF_PHASE'
#!/bin/bash
# phase.sh P|S: lay out the phase's pools on A's build, then run ROUNDS
# rounds. A round gives every arm one slot (slot.sh: fresh module, import,
# warm-up, the arm's rows back to back); the arm order is A B12 B123 B1234
# in odd rounds and the reverse in even rounds (no B123 in phase S, which
# has no mirror). After every round, a phase where a third or more of the
# runs so far had steal_pct >= 1 stops (exit 5: the host is busy). The
# pools and the RAM disk are destroyed at the end, also on a failure.
#   P: an 8 GiB RAM disk, a 6 GiB single-device pool and a 2 x 1000 MiB
#      mirror, laid out as in VM-CTR-AB-1108 (the shared files included);
#   S: a RAM disk sized from zfs_wrlog_data_max, the sync dataset and a
#      1 GiB file that only the warm-up reads.
set -u
. /var/log/ctr-warm-1109/env.sh
ph=$1
case $ph in
P) arms=(A B12 B123 B1234) ;;
S) arms=(A B12 B1234) ;;
*) echo "usage: phase.sh P|S"; exit 1 ;;
esac
grep -q '^brd ' /proc/modules && { echo "brd already loaded: stopping"; exit 1; }
"$L/arm.sh" A || exit 1
for p in "$P" "$PM" "$PS"; do
	zpool list "$p" > /dev/null 2>&1 && { echo "pool $p exists: stopping"; exit 1; }
done
M=/sys/module/zfs/parameters
wl=$(cat "$M/zfs_wrlog_data_max")
# Sync pool: live 1.26 GiB (sync files and the warm-up file), log in flight
# up to 4 txgs of 15 % of zfs_wrlog_data_max at 1.25 bytes allocated per
# byte logged, data in flight up to 3 txgs of the 64 MiB that 4 jobs
# rewrite, all under 50 % of the pool.
SG=$(awk -v w="$wl" 'BEGIN { need = 1.26 + 0.75 * w / 2^30 + 0.19
    s = int(need / 0.5) + 1; if (s < 8) s = 8; print s }')
need=$(( (SG > 8 ? SG : 8) + 5 ))
avail=$(awk '/^MemAvailable/ { print int($2 / 1048576) }' /proc/meminfo)
[ "$avail" -ge "$need" ] ||
    { echo "MemAvailable ${avail} GiB < ${need} GiB: stopping"; exit 1; }
mkdir -p "$L/raw" "$W/mnt" "$W/mntm" "$W/mnts" && cd "$L" || exit 1
cleanup() {
	cd / || return
	for p in "$PS" "$PM" "$P"; do
		zpool list "$p" > /dev/null 2>&1 && { zpool destroy "$p" || echo "zpool destroy $p failed"; }
	done
	if grep -q '^brd ' /proc/modules; then
		rmmod brd || echo "rmmod brd failed"
	fi
}
trap cleanup EXIT
usage() {
	for p in "$@"; do
		zpool list -Hp -o name,size,alloc "$p" |
		    awk '{ printf "%s size %d alloc %d (%.1f %%)\n", $1, $2, $3, 100 * $3 / $2 }'
	done
}
# lay TAG DIR FIO_ARGS...: write the files of a dataset.
lay() {
	local tag=$1 dir=$2
	shift 2
	fio --name=layout --directory="$W/$dir" "$@" --bs=1m --rw=write \
	    --ioengine=psync --refill_buffers --buffer_compress_percentage=50 \
	    --fallocate=none --end_fsync=1 > "raw/layout-$tag.log" 2>&1
}
{ echo "phase $ph $(date -u '+%F %T') host $(hostname) nproc $(nproc) MemAvailable ${avail} GiB"
  for p in spa_asize_inflation zfs_dirty_data_max zfs_wrlog_data_max zfs_arc_max zfs_arc_min; do
	echo "$p=$(cat "$M/$p")"
  done
  awk '$1 == "c_min" || $1 == "c_max" { printf "arc %s %.0f MiB\n", $1, $3 / 1048576 }' \
      /proc/spl/kstat/zfs/arcstats; } | tee -a phases.txt
if [ "$ph" = P ]; then
	modprobe brd rd_nr=1 rd_size=$((8 * 1048576)) max_part=16 || exit 1
	printf 'label: gpt\nsize=6GiB\nsize=1000MiB\nsize=1000MiB\n' |
	    sfdisk -q /dev/ram0 > /dev/null || exit 1
	udevadm settle
	for n in 1 2 3; do
		[ -b /dev/ram0p$n ] || { echo "no /dev/ram0p$n: stopping"; exit 1; }
		dd if=/dev/zero of=/dev/ram0p$n bs=1M oflag=direct status=none \
		    count=$(( $(blockdev --getsize64 /dev/ram0p$n) / 1048576 )) || exit 1
	done
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mnt" "$P" /dev/ram0p1 || exit 1
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mntm" "$PM" mirror /dev/ram0p2 /dev/ram0p3 ||
	    exit 1
	zfs create "$P/plain" && zfs create "$P/wplain" &&
	    zfs create -o primarycache=metadata "$P/nocache" &&
	    zfs create -o primarycache=metadata -o compression=lz4 "$P/nocachelz4" &&
	    zfs create -o primarycache=metadata "$PM/nocache" && zfs create "$PM/wplain" ||
	    exit 1
	for ds in mnt/plain mnt/wplain mnt/nocache mnt/nocachelz4 mntm/nocache mntm/wplain; do
		case $ds in mntm/*) fs=8m ;; *) fs=16m ;; esac
		lay "${ds//\//-}" "$ds" --filename_format='f.$jobnum' --size=$fs --numjobs=16 || exit 1
	done
	# Read only by the warm-up: they keep the main pool as full as in
	# VM-CTR-AB-1108 and bring the ARC to the size it had there.
	for ds in plain wplain; do
		lay "mnt-$ds-shared" "mnt/$ds" --filename=shared.0 --size=512m || exit 1
	done
	pools=("$P" "$PM")
else
	modprobe brd rd_nr=1 rd_size=$((SG * 1048576)) max_part=16 || exit 1
	dd if=/dev/zero of=/dev/ram0 bs=1M count=$((SG * 1024)) oflag=direct status=none || exit 1
	zpool create -o ashift=12 -o cachefile=none -O atime=off -O compression=off \
	    -O recordsize=16k -O mountpoint="$W/mnts" "$PS" /dev/ram0 || exit 1
	zfs create "$PS/sync" && zfs create "$PS/warm" || exit 1
	lay mnts-sync mnts/sync --filename_format='f.$jobnum' --size=16m --numjobs=16 || exit 1
	# Read only by the warm-up, so that the ARC grows as in phase P.
	lay mnts-warm mnts/warm --filename=ballast.0 --size=1g || exit 1
	zfs set sync=always "$PS/sync" || exit 1
	pools=("$PS")
fi
zpool sync "${pools[@]}" || exit 1
usage "${pools[@]}" | sed "s/^/phase $ph after layout: /" | tee -a phases.txt
usage "${pools[@]}" | awk '{ gsub(/[(]/, "", $6); if ($6 + 0 > 50) bad = 1 } END { exit bad }' ||
    { echo "a pool is above 50 % after its layout: stopping"; exit 1; }
for ((rnd = 1; rnd <= ROUNDS; rnd++)); do
	order=("${arms[@]}")
	if ((rnd % 2 == 0)); then
		order=()
		for ((i = ${#arms[@]} - 1; i >= 0; i--)); do order+=("${arms[$i]}"); done
	fi
	mark "$ph" "$rnd" - start
	for arm in "${order[@]}"; do
		"$L/slot.sh" "$ph" "$rnd" "$arm" || exit $?
	done
	mark "$ph" "$rnd" - end
	read -r nb nt < <(awk -v ph="$ph" 'NR > 1 && $2 == ph { t++; if ($9 + 0 >= 1) n++ }
	    END { print n + 0, t + 0 }' results.txt)
	echo "phase $ph round $rnd done: $nt runs, $nb with steal_pct >= 1"
	if [ "$nt" -gt 0 ] && [ $((3 * nb)) -ge "$nt" ]; then
		echo "HOST BUSY: $nb of the $nt runs of phase $ph had steal_pct >= 1: stopping"
		exit 5
	fi
done
usage "${pools[@]}" | sed "s/^/phase $ph end: /" | tee -a phases.txt
echo "phase $ph done: $(awk -v ph="$ph" 'NR > 1 && $2 == ph' results.txt | wc -l) runs"
EOF_PHASE
cat > $L/compare.py << 'EOF_CMP'
#!/usr/bin/env python3
# compare.py: the tables of the warm-ARC run, from results.txt, warm.txt and
# rounds.txt.
#  - per round and per slot: steal (% of all vCPU time) and load average;
#  - per slot: the warm-up and the ARC it left;
#  - per row: IOPS of every arm in every round, the ARC at the start of the
#    runs, and the runs whose warm state differs;
#  - per row and commit pair X/Y: d_r = X against Y in round r (%), their
#    mean m, h = half their range, and the verdict:
#      GAIN   at least 3 rounds, every d_r > 0 and m > t
#      LOSS   at least 3 rounds, every d_r < 0 and m < -t
#      none   at least 3 rounds, |m| <= 3, h <= 3, no run with steal >= 1
#      unclear  anything else
#    with t = max(3, h, f), f being the largest |m| of the same pair on the
#    null-control rows (f is 0 for the controls themselves).
#  - the two questions: the cread gain, and commit 3.
import os
import sys

L = "/var/log/ctr-warm-1109"
STEAL = 1.0
nan = float("nan")


def lines(name):
    p = os.path.join(L, name)
    return [r.split() for r in open(p) if r.split()] if os.path.exists(p) else []


def mib(s):
    return float(s[:-1]) * (1024 if s[-1] in "gG" else 1)


rows = lines("rows.txt")
info = {r[0]: r for r in rows}
v = {}
for f in lines("results.txt")[1:]:
    if len(f) >= 24:
        v[(f[0], f[2], f[3])] = f
rounds = sorted({k[0] for k in v}, key=int)
out = []

out.append("== rounds and slots: steal % of all vCPU time, load average at start .. end")
start = {}
for m in lines("rounds.txt"):
    if len(m) < 16:
        continue
    k = (m[1], m[3], m[5])
    st, al = int(m[13]), int(m[15])
    if m[6] == "start":
        start[k] = (m[7], " ".join(m[9:12]), st, al)
    elif k in start:
        t0, l0, s0, a0 = start.pop(k)
        what = "round %s" % k[1] if k[2] == "-" else "  slot %-5s" % k[2]
        out.append("phase %s %s  %s..%s  steal %.3f %%  load %s .. %s"
            % (k[0], what, t0, m[7], 100.0 * (st - s0) / max(al - a0, 1),
               l0, " ".join(m[9:12])))
for k in start:
    out.append("phase %s round %s %s: started, no end mark" % (k[0], k[1], k[2]))

out.append("")
out.append("== warm-up per slot: files, seconds, ARC size, target, MRU, MFU and"
    " dbuf cache target (MiB) after it")
wl = [w for w in lines("warm.txt")[1:] if len(w) >= 11]
for w in wl:
    out.append("phase %s round %s %-5s  files %s  %5ss  arc %5s  c %5s  mru %5s"
        "  mfu %5s  dbuf target %4s" % (w[1], w[0], w[2], w[3], w[4], w[5],
        w[6], w[7], w[8], w[9]))
for ph in ("P", "S"):
    s = [float(w[5]) for w in wl if w[1] == ph]
    if s:
        out.append("phase %s: ARC after the warm-up %.0f .. %.0f MiB over %d slots"
            " (spread %.1f %%)" % (ph, min(s), max(s), len(s),
            100 * (max(s) - min(s)) / max(min(s), 1)))


def arms_of(r):
    return ["A", "B12", "B123", "B1234"] if r[3].startswith("mntm/") \
        else ["A", "B12", "B1234"]


def pairs_of(r):
    if r[3].startswith("mntm/"):
        return [("all four", "B1234", "A"), ("c1+c2", "B12", "A"),
                ("c3", "B123", "B12"), ("c4", "B1234", "B123"),
                ("c3+c4", "B1234", "B12")]
    return [("all four", "B1234", "A"), ("c1+c2", "B12", "A"),
            ("c3", "B1234", "B12")]


# Warm state: a cread row must miss no more than 1 % of its records (it
# reads data the warm-up cached), and every run must start with an ARC
# within 10 % of A's in the same round.
flag = {}
for r in rows:
    row = r[0]
    recs = int(r[6]) * mib(r[7]) * 1024 / 16
    for rd in rounds:
        a = v.get((rd, row, "A"))
        for arm in arms_of(r):
            f = v.get((rd, row, arm))
            if not f:
                continue
            why = []
            if r[2] == "G" and float(f[20]) > 0.01 * recs:
                why.append("%s data misses" % f[20])
            if a and float(a[13]) > 0 and \
                    abs(float(f[13]) - float(a[13])) > 0.1 * float(a[13]):
                why.append("ARC %s MiB vs A %s" % (f[13], a[13]))
            if why:
                flag[(rd, row, arm)] = ", ".join(why)

out.append("")
out.append("== IOPS per row, arm and round; mean; spread over the rounds (% of"
    " the mean); ARC size and target at the start of the runs (mean MiB)")
for r in rows:
    row = r[0]
    if not any((rd, row, a) in v for rd in rounds for a in arms_of(r)):
        continue
    out.append("%s (%s, phase %s)" % (row, r[2], r[1]))
    for arm in arms_of(r):
        x = [float(v[(rd, row, arm)][4]) for rd in rounds if (rd, row, arm) in v]
        if not x:
            continue
        a0 = [float(v[(rd, row, arm)][13]) for rd in rounds if (rd, row, arm) in v]
        c0 = [float(v[(rd, row, arm)][15]) for rd in rounds if (rd, row, arm) in v]
        dm = [v[(rd, row, arm)][20] for rd in rounds if (rd, row, arm) in v]
        mean = sum(x) / len(x)
        out.append("  %-5s %s  mean %8.0f  spread %5.1f %%  arc %5.0f  c %5.0f"
            "  data misses %s" % (arm, " ".join("%8.0f" % y for y in x), mean,
            100 * (max(x) - min(x)) / mean, sum(a0) / len(a0),
            sum(c0) / len(c0), ",".join(dm)))
out.append("warm state differs: " + ("; ".join("%s round %s %s (%s)"
    % (k[1], k[0], k[2], w) for k, w in sorted(flag.items())) or "no run"))


def verdict(row, X, Y, floor):
    d = []
    hot = False
    warm = False
    for rd in rounds:
        x, y = v.get((rd, row, X)), v.get((rd, row, Y))
        if x and y:
            d.append(100 * (float(x[4]) - float(y[4])) / float(y[4]))
            hot |= float(x[8]) >= STEAL or float(y[8]) >= STEAL
            warm |= (rd, row, X) in flag or (rd, row, Y) in flag
    if len(d) < 3:
        return d, nan, nan, "unclear (%d rounds)%s" % (len(d),
            " (steal)" if hot else "")
    m = sum(d) / len(d)
    h = (max(d) - min(d)) / 2
    t = max(3.0, h, floor)
    if all(y > 0 for y in d) and m > t:
        vd = "GAIN"
    elif all(y < 0 for y in d) and m < -t:
        vd = "LOSS"
    elif abs(m) <= 3 and h <= 3 and not hot:
        vd = "none"
    else:
        vd = "unclear"
    if hot:
        vd += " (steal)"
    if warm:
        vd += "; warm state differs"
    return d, m, h, vd


floor = {}
for r in rows:
    if r[2] != "C":
        continue
    for name, X, Y in pairs_of(r):
        d, m, h, vd = verdict(r[0], X, Y, 0.0)
        if m == m:
            floor[name] = max(floor.get(name, 0.0), abs(m))
out.append("")
out.append("== verdicts: X/Y in each round (%), mean m, half-range h, verdict")
out.append("   null-control floor f per pair: " + (", ".join("%s %.1f"
    % (k, floor[k]) for k in sorted(floor)) or "none"))
res = {}
for r in rows:
    row = r[0]
    if not any((rd, row, "A") in v for rd in rounds):
        continue
    out.append("%s (%s)" % (row, r[2]))
    for name, X, Y in pairs_of(r):
        f = 0.0 if r[2] == "C" else floor.get(name, 0.0)
        d, m, h, vd = verdict(row, X, Y, f)
        res[(row, name)] = (m, vd)
        out.append("  %-9s %-10s %-31s m %+6.1f  h %5.1f  %s" % (name,
            "%s/%s" % (X, Y), " ".join("%+.1f" % y for y in d), m, h, vd))

out.append("")
out.append("== the questions")
g = [r[0] for r in rows if r[2] == "G"]
gv = [res.get((row, "all four"), (nan, "missing")) for row in g]
out.append("cread gain, B1234/A: " + ", ".join("%s %s (m %+.1f)"
    % (row, x[1], x[0]) for row, x in zip(g, gv)))
ng = sum(1 for x in gv if x[1].startswith("GAIN") and "warm" not in x[1])
if not g or any(x[1] == "missing" or x[1].startswith("unclear (")
                for x in gv):
    out.append("  -> no answer: a cread row has fewer than three rounds")
elif ng == len(g):
    out.append("  -> the VM-CTR-AB-1108 gain reproduces with a warm ARC")
elif not any(x[1].startswith("GAIN") for x in gv):
    out.append("  -> not reproduced: no cread row gains beyond the noise with"
        " a warm ARC")
else:
    out.append("  -> partly: %d of %d cread rows gain (a warm-state flag"
        " counts against)" % (ng, len(g)))
for name, what in (("c1+c2", "commits 1+2, B12/A"),
                   ("c3", "commit 3, B1234/B12 (B123/B12 on mirror rows)"),
                   ("c4", "commit 4, B1234/B123 (mirror rows)")):
    xs = [(r[0], res[(r[0], name)]) for r in rows if (r[0], name) in res]
    out.append("%s: %s" % (what, ", ".join("%s %s (m %+.1f)"
        % (row, x[1], x[0]) for row, x in xs) or "no data"))
print("\n".join(out))
sys.exit(0)
EOF_CMP
chmod 0755 $L/*.sh $L/*.py
echo "round phase row arm iops MiBps p99_us cpu_pct steal_pct steal_jiffies quota peak_pct load1 arc0_MiB arc1_MiB c0_MiB c1_MiB hit0_pct hit1_pct dhit_pct dmiss dbc_target_MiB dbc_evicts time" > $L/results.txt
echo "round phase arm files seconds arc_MiB c_MiB mru_MiB mfu_MiB dbc_target_MiB time" > $L/warm.txt
cd $L && sha256sum env.sh rows.txt arm.sh run.sh slot.sh phase.sh compare.py
```

Must print:
```
ef17e2129bb060bf8dde4a1b7e2378c60a0ee3f2f2a894bc7b67a2469a780bdc  env.sh
5b637d9d52d9ab3fb95fbbdee70fa8f3e7c1d54e9d919b7b096940f001c5ac48  rows.txt
e6f44fd53a16f076e52ceacec10e6881fbde62c41a3277b2d3af7d9597b6cd58  arm.sh
8854dcacb0664b9f7dedfd9f7a321cfd3660e525c7c675a1146ce317c781c661  run.sh
887d658c0b0a33ae4c497dcb92413a394620fe3eab73e31d09b685086ddcc104  slot.sh
d0aeac61447ee7eeb3602462be0e7085b609d5fe12b28f4a78407165089a50b8  phase.sh
d5c2f98adcf40ee4d882e9385769dbe59a6d1aebac110b0e278c3ffa0e3d39ea  compare.py
```

## STEP 3: phase P (about 1 h)

Only while the other two test VMs are idle. One command:

```sh
sudo -n bash -c '. /var/log/ctr-warm-1109/env.sh
"$L/phase.sh" P; echo "exit $?"' 2>&1 | tee /var/log/ctr-warm-1109/phase-P.log
```

It prints the module parameters and the ARC bounds, the `after layout` lines
(`zfswarm` near 34 %, `zfswarmm` near 26 %), then for every round a `round
N start` mark, for every slot a `warm-up:` line (round, phase, arm, files,
seconds, ARC size, target, MRU, MFU, dbuf cache target, time; the ARC size
near 1700 MiB, 1331 at least or the slot stops) followed by one line per run
(the columns of `results.txt`), then the `round N end` mark and `phase P
round N done: ... runs, ... with steal_pct >= 1`; at the end the `end` usage
lines, `phase P done: 108 runs` and `exit 0`.

`exit 5` means the host was busy (stop rule 1): skip STEP 4 and go to STEP
5. `exit 7` means the warm-up did not warm the ARC (stop rule 2). Any other
non-zero exit is a stop. After a stop, go to STEP 5 with what you have.

## STEP 4: phase S (about 10 min)

Only after STEP 3 printed `exit 0`, and only while the other two test VMs
are idle:

```sh
sudo -n bash -c '. /var/log/ctr-warm-1109/env.sh
"$L/phase.sh" S; echo "exit $?"' 2>&1 | tee /var/log/ctr-warm-1109/phase-S.log
```

The same output for the sync pool (`zfswarms` near 16 % after layout, the
ARC size near 1300 MiB after each warm-up, 1024 at least, 12 runs), ending
with `phase S done: 12 runs` and `exit 0`.

## STEP 5: the tables

```sh
L=/var/log/ctr-warm-1109
tail -n 1 $L/phase-P.log $L/phase-S.log 2>&1
python3 -I $L/compare.py | tee $L/compare.txt
awk 'NR > 1 && $12 != "-" { if ($12 + 0 > m) m = $12 + 0 } END { print "highest pool usage seen: " m + 0 " %" }' $L/results.txt
awk 'NR > 1 { t++; if ($9 + 0 >= 1) n++ } END { print "runs with steal_pct >= 1: " n + 0 " of " t + 0 }' $L/results.txt
sudo -n dmesg | tail -n +$(( $(cat $L/dmesg.start) + 1 )) | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more|suspended|ENOSPC'   # 0
```

## STEP 6: cleanup

The phases already destroyed their pools and RAM disks.

```sh
W=/var/tmp/ctr-warm-1109 L=/var/log/ctr-warm-1109
sudo -n bash -c '. /var/log/ctr-warm-1109/env.sh
if [ -d /sys/module/zfs ]; then
	for p in "$P" "$PM" "$PS"; do zpool list -H -o name "$p" 2> /dev/null && zpool destroy "$p"; done
fi
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null
grep -q "^brd " /proc/modules && { rmmod brd || echo "rmmod brd failed"; }
for m in zfs spl; do [ -d /sys/module/$m ] && { rmmod $m || echo "rmmod $m failed"; }; done
grep -E "^(zfs|spl|brd) " /proc/modules'
sudo -n rm -rf $W && echo "work dir removed; logs kept in $L"
du -sh $L
sudo -n fstrim -av
```

Expected: no pool, nothing loaded, `work dir removed`, the size of L, the
fstrim summary.

## STEP 7: report

```sh
L=/var/log/ctr-warm-1109
{
echo "== builds"; cat $L/builds.txt
echo "== phases"; cat $L/phases.txt
echo "== tables"; cat $L/compare.txt
echo "== warm-ups"; cat $L/warm.txt
echo "== results"; cat $L/results.txt
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt`, what STEP 0 printed, and at
its end the last lines of STEP 3 and STEP 4, the last four lines of STEP 5,
the output of STEP 6, which other VMs were busy during STEPS 3 and 4 (if you
know) and every deviation from this document (what and why). Reply with the
whole of `REPORT.txt`.

## How to read the results

- **compare.txt**, in order: steal and load per round and per slot; the
  warm-up of every slot and the ARC it left (the same size in every slot of
  a phase, within a few %, is the warm state this run needs); per row, the
  IOPS of every arm in every round with their mean and spread, the ARC size
  and target at the start of the runs and the data misses per run, then the
  runs whose warm state differs; per row and commit pair, the four d_r, m, h
  and the verdict; the two answers.
- **The cached-read gain.** If it reproduces here (GAIN on the three cread
  rows), while the bisect saw none with an empty ARC, it is real and needs a
  warm ARC; `c1+c2` and `c3` on those rows say which commits carry it. If it
  is not reproduced, the likely reading is that the gain of VM-CTR-AB-1108
  came from its conditions (B ran once, next to load on the other VMs), not
  from the series; the differences under Regime below are what this run
  cannot rule out.
- **Commit 3.** LOSS on the cread rows here too: it costs on cached reads in
  both regimes. GAIN here: it trades the bisect's loss with an empty ARC
  (`cread-16`, step -4.3 and -6.0 %) for a gain with a warm one. none: it
  buys nothing measurable with a warm ARC and costs with an empty one. The
  `dbc_evicts` column counts the dbufs the cache evicted in each run, each
  eviction comparing commit 3's counter once. Whether it goes is the
  maintainer's decision.
- **The L rows.** `uread-16` and `mir-awrite-1` lost in the bisect with no
  single commit beyond the noise: their `c1+c2`, `c3` (and `c4`) verdicts say
  where a loss sits once the ARC is warm, if it holds.
- **What the code leads to expect.** Commits 1 and 3 replace atomics that a
  cached read updates on one global line, hence the expected `cread` gain.
  Commit 2 is the only one that puts a lock (an aggsum bucket mutex, shared by
  two vCPUs here) on the uncached-read and write paths, a few hundred ns per
  I/O at most. Commit 4 can change only the `mir-*` rows.
- **Regime.** What still differs from VM-CTR-AB-1108: the ARC was filled by
  the warm-up's reads here, by the layout's writes there; a slot keeps its
  module about 4 min, VM-CTR-AB-1108 kept it for a 35 min pass; there the RAM
  disk was filled after the module was loaded, which may have made the
  kernel ask the ARC to shrink, here it is filled before any slot loads its
  module; runs are 20 s instead of 30, without `perf`. Compare arms within
  this run, not with VM-CTR-AB-1108's absolute numbers.
- **Noise.** `steal_pct` near 0 means the vCPUs ran whenever they had work.
  A round or slot whose steal or load stands out in the first table makes
  its runs suspect; say which. `quota` is the `dmu_tx_quota` count of the run
  (0 on reads), `peak_pct` the highest pool usage seen in a write run (under
  50 % expected). The null-control floor f printed above the verdicts is the
  smallest effect this run can see; a floor above 3 % raises every threshold
  to it.

sha256 of this file without this last line and the blank line before it: ede5793a1495d2e10ae18d77944f7777d5aad514d03f61ef3f001cd94b195c3f
