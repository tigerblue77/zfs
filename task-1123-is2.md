TASK-1123-IS2 (from the coordinator session "main"): functional, about 1 h with its build, at most 2 h. **One reboot is pre-approved, for step 5 only**, and only if a run leaves the VM stalled after its own cleanup (see step 5); nowhere else. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), **alone on that VM**: the reproducer lowers `vm.dirty_bytes` to 16 MiB for about 2 minutes per run and can stall every process that writes through a mapping. It tries to reproduce upstream issue openzfs/zfs#19737 ("Linux: background writeback of mmap'd dirty pages livelocks when dmu_tx_assign() returns EDQUOT") on upstream master `a1daebfec3f`, with a control run, and measures whether the stuck pages drain when the writers die and when the quota is raised. It also records, on the way, what a raw `fsync(2)` returns during the stall (issue #19915). Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is2/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The report (myers, 6/11; production on 2.3.9, reproduced on 2.4.4 and master 0606e0dcc, kernels 6.8, 6.18, 7.2, 7.3):** a process that stores through a `MAP_SHARED` mapping into a dataset at its `refquota` makes background writeback fail with `EDQUOT` from `dmu_tx_assign()` in `zfs_putpage()`. The page is re-dirtied and retried without sleeping: the `flush-zfs` kworker spins at 90-100% of a CPU, `dmu_tx_quota` in `/proc/spl/kstat/zfs/dmu_tx` climbs by 50k to 300k per second, the pages stay dirty and pin the host's dirty budget, and every other process that writes through a mapping, on any dataset or pool, blocks in `balance_dirty_pages()` (production: journald, then sshd and sudo, for 6 hours). The error is never reported to the writer. Workaround: raise the quota, or delete the file.
- **Mechanism (read on master by main):** `dsl_dataset_check_quota()` returns `EDQUOT` (not `ERESTART`) at the quota with nothing in flight, so `dmu_tx_assign(DMU_TX_WAIT)` returns at once; `zfs_putpage()` (`module/os/linux/zfs/zfs_vnops_os.c`) re-dirties the page and, for background writeback, returns 0 (`for_sync ? err : 0`), so the writeback loop counts it as progress and comes straight back; nothing calls `mapping_set_error()`. For `fsync`, `zpl_putpage()` (`zpl_file.c:607`) returns `zfs_putpage()`'s positive errno unnegated, and `zpl_fsync()` hands it to userspace: that is #19915.
- **The reporter has patches he has not posted** (asks the maintainers whether to drop or keep the pages dirty); no PR is linked. This task only reproduces on our VM, to give a baseline for a fix.
- **What this task shows:** with the reporter's workload on our VM (Debian 13, 6.12, debug master): the control run without quota (no spin, bystander healthy), the quota run (spin and stall expected), whether killing the writers ends it (expected: no, their pages outlive them), whether raising the quota ends it (expected: yes, within seconds), and the raw `fsync` return during the stall (expected `rc=122 errno=0`).

## What is tested

The tree `zm` is upstream master, fetched from https://github.com/openzfs/zfs:

| tree | commit | tree hash |
|---|---|---|
| zm | a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 |

Each run (the script `is2-run.sh` below; it follows the reporter's `repro.sh`, with every pool command bounded and the sysctls restored on any exit): pool `is2pool` on a sparse file under `$D` (`cachefile=none`, `compression=off`), dataset `q` with the given `refquota` filled to 62 MiB, dataset `other` with no quota, `vm.dirty_bytes=16M` and `vm.dirty_background_bytes=4M`; three writers that store one byte per 4 KiB page of a 512 MiB `MAP_SHARED` file in `q`, forever, and a bystander that stores into 16 pages of a mapped file in `other` and appends a tick line every second. Then:
- **load:** six 10 s windows of samples: `dmu_tx_quota` per second, CPU of the flush threads, `Dirty`/`Writeback` from `/proc/meminfo`, the bystander's ticks per 10 s (healthy: 10), writers in D state; the busiest flush thread's kernel stack, three times; a raw `fsync(2)` (syscall 74 through ctypes, twice) on the first writer's file;
- **killed:** the three writers are killed (`kill -9`), two windows;
- **recovery** (quota runs only): `zfs set refquota=2G` on `q`, three windows;
- the cleanup: kill the bystander, restore the four sysctls, remove the mapped files and destroy the pool (each bounded, never waited for), remove the pool file.

**STALL REPRODUCED** for a run means: in the load windows, `dmu_tx_quota` above 1000/s in some window and the bystander at 3 ticks or fewer in some window after the first. `SPIN WITHOUT STALL` means the first without the second.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- Pools use file vdevs under `/var/tmp/task-1123-is2`. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Nothing else runs on this VM during the runs** (no build, no other task): the stall reaches every writer through a mapping.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko`;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool;
  - `cat /proc/sys/vm/dirty_bytes /proc/sys/vm/dirty_background_bytes /proc/sys/vm/dirty_ratio /proc/sys/vm/dirty_background_ratio` equals the four values of step 0;
  - `df -h /var/tmp` shows at least 8 GiB free.
- **The build and every run run detached** (`setsid nohup ... &`) and are polled. While a run is in its load windows, poll with reads only (`tail`, `cat` of files under `$L`), every 30 s.
- **Hangs.** A build still going after 60 minutes, or a run whose `steps.txt` has not changed for 6 minutes, is a hang: capture as in step 5's capture list into `$L/hang.txt`, then go to step 5.
- **Kernel log.** After each run, check the new kernel lines since that run's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. A `WARNING` in `writeback_iter` during the `fsync` probe is possible (the kernel objecting to the positive errno, seen on 6.17): quote it. A `VERIFY` or `ASSERT` failure is a finding: quote it in full with its stack, and go to step 5's capture, then stop.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 12 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `command -v ksh git python3 && python3 --version && sudo -n true && echo sudo-ok`;
- `ls -d /var/tmp/task-1123-is2 /var/log/task-1123-is2 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/vm/dirty_bytes /proc/sys/vm/dirty_background_bytes /proc/sys/vm/dirty_ratio /proc/sys/vm/dirty_background_ratio`: quote the four values; they must be the same before every run and at the end;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic`: quote them; do not change them. **If `hung_task_panic` is not 0, run only run N and report**;
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service zfs-load-module.service 2>&1`: quote it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is2; L=/var/log/task-1123-is2; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
mkdir $D/zm && cd $D/zm && git init -q . &&
  git fetch -q --depth 1 $U a1daebfec3f5ca46ac261ef339efdf0dbeae997c && git checkout -q FETCH_HEAD &&
  echo "zm $(git rev-parse HEAD) $(git write-tree) $(git status --porcelain | wc -l)" | tee $L/trees.txt
git -C $D/zm log -1 --format='%an | %s'
```

Expected:

```
zm a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0
Rick Galliher | ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
```

If the fetch fails, wait 1 minute and try once more; if fetching by commit is refused, use `git fetch -q --depth 200 $U master` and `git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c` instead. Any other difference is a stop: report the outputs as they are. Edit no file of the tree.

## Step 2: the debug build (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-is2; L=/var/log/task-1123-is2
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes. Then quote `grep -E 'HAVE_WRITE_CACHE_PAGES|HAVE_WRITEPAGE_T_FOLIO|HAVE_VFS_WRITEPAGE' $D/zm/zfs_config.h` (which writeback loop this kernel uses: the kernel's `write_cache_pages()` or ZFS's own).

## Step 3: load the module and write the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)".

**Write the script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is2
cat > $D/is2-run.sh <<'IS2RUN_END'
#!/bin/bash
# is2-run.sh <tag> <refquota|none> <pool-size>
# One attempt at openzfs/zfs#19737, after the reporter's repro.sh: a file
# pool, dataset q (refquota, filled to 62M), three MAP_SHARED writers on q,
# a bystander on dataset other, vm.dirty_bytes=16M; 60 s of samples, then,
# for a quota run, refquota raised to 1G and 30 s more.  Every pool command
# is bounded; sysctls are restored on any exit.  Runs as root.
set -u
D=/var/tmp/task-1123-is2; L=/var/log/task-1123-is2; Z=$D/zm
T=$1; QUOTA=$2; PSIZE=$3
P=is2pool; IMG=$D/pool-$T.img; M=$D/mnt-$T; O=$L/run-$T; W=$O/work
ZPOOL=$Z/zpool; ZFS=$Z/zfs
mkdir -p "$O" "$W"
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }
ODB=$(cat /proc/sys/vm/dirty_bytes); ODBB=$(cat /proc/sys/vm/dirty_background_bytes)
ODR=$(cat /proc/sys/vm/dirty_ratio); ODBR=$(cat /proc/sys/vm/dirty_background_ratio)
PIDS=""; DONE=0
# A command that may block: started in the background, its output read
# after at most $2 seconds.  It is never waited for.
bg() {
	local out=$1 secs=$2 i=0; shift 2
	( timeout "$secs" "$@" > "$out" 2>&1; echo "rc=$?" >> "$out" ) &
	while [ $i -lt "$secs" ] && ! grep -q '^rc=' "$out" 2>/dev/null; do
		sleep 1; i=$((i + 1))
	done
	grep -q '^rc=' "$out" 2>/dev/null || echo "STILL BLOCKED after ${secs}s" >> "$out"
}
cleanup() {
	[ $DONE = 1 ] && return; DONE=1
	log "cleanup"
	for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
	echo "$ODBB" > /proc/sys/vm/dirty_background_bytes
	echo "$ODB" > /proc/sys/vm/dirty_bytes
	[ "$ODB" = 0 ] && echo "$ODR" > /proc/sys/vm/dirty_ratio
	[ "$ODBB" = 0 ] && echo "$ODBR" > /proc/sys/vm/dirty_background_ratio
	log "sysctls: $(cat /proc/sys/vm/dirty_bytes) $(cat /proc/sys/vm/dirty_background_bytes) $(cat /proc/sys/vm/dirty_ratio) $(cat /proc/sys/vm/dirty_background_ratio)"
	bg "$O/rm.txt" 60 sh -c "rm -f $M/q/out* $M/other/by"
	log "rm: $(tail -n 1 "$O/rm.txt")"
	bg "$O/destroy.txt" 120 "$ZPOOL" destroy -f "$P"
	log "destroy: $(tail -n 1 "$O/destroy.txt")"
	grep -q '^rc=0' "$O/destroy.txt" && rm -f "$IMG"
	log "run $T end"
}
trap cleanup EXIT
trap 'log interrupted; exit 130' INT TERM HUP

cat > "$W/mmap_writer.py" <<'PY'
import mmap, os, sys
path, mib = sys.argv[1], int(sys.argv[2])
size = mib << 20
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, size)
m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
n = 0
while True:
    for off in range(0, size, 4096):
        m[off] = (n + 1) & 0xff
    n += 1
    print("pass %d complete" % n, flush=True)
PY
cat > "$W/bystander.py" <<'PY'
import mmap, os, sys, time
path, ticks = sys.argv[1], sys.argv[2]
size = 256 << 20
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, size)
m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
tf = open(ticks, "a", buffering=1)
page = 0
while True:
    for _ in range(16):
        m[(page % (size // 4096)) * 4096] = page & 0xff
        page += 1
    tf.write("tick %d\n" % page)
    time.sleep(1)
PY
# fsync through the raw syscall, so that a positive return is seen as is
cat > "$W/fsync_probe.py" <<'PY'
import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
fd = os.open(sys.argv[1], os.O_RDWR)
for i in range(2):
    ctypes.set_errno(0)
    rc = libc.syscall(74, fd)
    print("fsync %d: rc=%d errno=%d" % (i, rc, ctypes.get_errno()), flush=True)
PY

log "run $T refquota=$QUOTA pool=$PSIZE"
truncate -s "$PSIZE" "$IMG" || exit 1
timeout 120 "$ZPOOL" create -f -o cachefile=none -O compression=off \
    -O mountpoint="$M" "$P" "$IMG" || { log "STOP: zpool create"; exit 1; }
timeout 60 "$ZFS" create -o refquota="$QUOTA" "$P/q" || exit 1
timeout 60 "$ZFS" create "$P/other" || exit 1
echo 16777216 > /proc/sys/vm/dirty_bytes
echo 4194304 > /proc/sys/vm/dirty_background_bytes
timeout 120 dd if=/dev/zero of="$M/q/fill" bs=1M count=62 conv=fsync status=none
timeout 60 "$ZFS" list -p -o name,used,refer,refquota,avail "$P/q" "$P/other" > "$O/zfs-list-start.txt"
log "filled; sysctls dirty_bytes=$(cat /proc/sys/vm/dirty_bytes) dirty_background_bytes=$(cat /proc/sys/vm/dirty_background_bytes)"

kq() { awk '$1 == "dmu_tx_quota" {print $3}' /proc/spl/kstat/zfs/dmu_tx; }
fcpu() { cat /proc/[0-9]*/task/*/stat 2>/dev/null |
    awk '$2 ~ /flush/ {t += $14 + $15} END {print t + 0}'; }
: > "$W/ticks"
for i in 1 2 3; do
	python3 "$W/mmap_writer.py" "$M/q/out$i" 512 > "$W/w$i.log" 2>&1 &
	PIDS="$PIDS $!"; WPIDS="${WPIDS:-} $!"
done
python3 "$W/bystander.py" "$M/other/by" "$W/ticks" > "$W/by.log" 2>&1 &
BY=$!; PIDS="$PIDS $BY"
log "writers:$PIDS"

# window <name> <count>: 10 s windows of samples
MAXR=0; MINT=999
window() {
	local name=$1 n=$2 w q0 q1 c0 c1 k0 k1 r cpu bt dn
	q0=$(kq); c0=$(fcpu); k0=$(wc -l < "$W/ticks")
	for w in $(seq 1 "$n"); do
		sleep 10
		q1=$(kq); c1=$(fcpu); k1=$(wc -l < "$W/ticks")
		r=$(( (q1 - q0) / 10 )); cpu=$(( (c1 - c0) / 10 ))
		bt=$((k1 - k0)); dn=0
		for p in $PIDS; do
			[ "$p" != "$BY" ] && grep -q '^State:.*D' /proc/$p/status 2>/dev/null && dn=$((dn + 1))
		done
		echo "$name w$w quota/s=$r flushCPU%=$cpu Dirty_kB=$(awk '/^Dirty:/ {print $2}' /proc/meminfo) Wback_kB=$(awk '/^Writeback:/ {print $2}' /proc/meminfo) bystander=$bt/10s writersD=$dn" | tee -a "$O/samples.txt"
		ps -eLo pid,tid,stat,pcpu,wchan:30,comm | awk 'NR == 1 || /flush/' >> "$O/ps-$name.txt"
		if [ "$name" = load ]; then
			[ $r -gt $MAXR ] && MAXR=$r
			[ $w -gt 1 ] && [ $bt -lt $MINT ] && MINT=$bt
		fi
		q0=$q1; c0=$c1; k0=$k1
	done
}
window load 6
# the busiest flush thread's kernel stack, three times
for i in 1 2 3; do
	tid=$(ps -eLo tid,pcpu,comm | awk '$3 ~ /flush/ {print $2, $1}' | sort -rn | head -1 | cut -d' ' -f2)
	[ -n "$tid" ] && { echo "== $i tid $tid"; timeout 5 cat /proc/$tid/stack; } >> "$O/flush-stacks.txt" 2>&1
	sleep 1
done
bg "$O/fsync-probe.txt" 60 python3 "$W/fsync_probe.py" "$M/q/out1"
bg "$O/zfs-list-load.txt" 30 "$ZFS" list -p -o name,used,refer,refquota,avail "$P/q" "$P/other"
for i in 1 2 3; do echo "w$i: $(tail -n 1 "$W/w$i.log")"; done > "$O/writers.txt"
V="NOT REPRODUCED"
[ $MAXR -gt 1000 ] && [ $MINT -le 3 ] && V="STALL REPRODUCED"
[ $MAXR -gt 1000 ] && [ $MINT -gt 3 ] && V="SPIN WITHOUT STALL"
echo "RUN $T load: max quota/s $MAXR, min bystander ticks $MINT/10s => $V" | tee "$O/verdict.txt" | tee -a "$O/steps.txt"

# The writers' dirty pages outlive them: kill the writers, watch 20 s,
# then (quota runs only) raise the quota and watch 30 s.
for p in $WPIDS; do kill -9 "$p" 2>/dev/null; done
log "writers killed"
window killed 2
if [ "$QUOTA" != none ]; then
	log "raising refquota to 2G"
	bg "$O/set-quota.txt" 60 "$ZFS" set refquota=2G "$P/q"
	log "zfs set: $(tail -n 1 "$O/set-quota.txt")"
	window recovery 3
fi
for n in killed recovery; do
	R=$(awk -v n="$n" '$1 == n {l = $2 " " $3 " " $5 " " $7} END {print l}' "$O/samples.txt")
	[ -n "$R" ] && echo "RUN $T $n (last window): $R" | tee -a "$O/verdict.txt" | tee -a "$O/steps.txt"
done
exit 0
IS2RUN_END
chmod 755 $D/is2-run.sh
sha256sum $D/is2-run.sh | cut -c1-64; wc -l < $D/is2-run.sh; bash -n $D/is2-run.sh && echo syntax-ok
```

Expected: `e5f6bc96cb0a3576080b3832488ae2c1ecd83748d61f111ac9308d015b9bf47f`, `164`, `syntax-ok`. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is2; L=/var/log/task-1123-is2; T=<tag>
echo "task-1123-is2: run $T start" | sudo -n tee /dev/kmsg > /dev/null
setsid nohup sudo -n bash $D/is2-run.sh $T <refquota> <pool size> > $L/run-$T.log 2>&1 < /dev/null &
```

| order | tag | arguments | what | expected duration |
|---|---|---|---|---|
| 1 | N | `N none 4G` | control: no quota, a pool large enough for the three files | about 2 to 4 minutes |
| 2 | Q | `Q 64M 1G` | the report | about 3 to 5 minutes |
| 3 | Q2 | `Q2 64M 1G` | repeat | about 3 to 5 minutes |

Poll `tail -n 3 $L/run-$T/steps.txt` every 30 s until it ends with `run $T end`.

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `$L/run-$T/verdict.txt`;
- `samples.txt` in full (one line per 10 s window: load, killed, recovery);
- `flush-stacks.txt` (the first stack in full, then whether the other two differ);
- from `ps-load.txt`, the flush thread lines of the last window (stat, %CPU, wchan);
- `fsync-probe.txt` (expected in a quota run: `fsync 0: rc=122 errno=0` and `fsync 1: rc=122 errno=0`, i.e. a positive errno returned as the value; in run N: `rc=0`);
- `zfs-list-start.txt`, `zfs-list-load.txt`, `writers.txt` (the last pass line of each writer), `set-quota.txt`, `rm.txt`, `destroy.txt`;
- the four sysctls (`cat /proc/sys/vm/dirty_bytes /proc/sys/vm/dirty_background_bytes /proc/sys/vm/dirty_ratio /proc/sys/vm/dirty_background_ratio`): they must equal step 0's; if not, restore them by hand with the step 0 values (`dirty_background_bytes` then `dirty_bytes`, then the ratios only if the matching byte value is 0), and say so;
- `awk '/^Dirty:|^Writeback:/' /proc/meminfo` and `ps -eLo pid,tid,stat,pcpu,comm | awk '/flush/'` one minute after the run's end (expected: Dirty small, no flush thread above a few % CPU);
- the kernel-line check since the run's mark.

## Step 5: only if the VM stays stalled after a run's cleanup

"Stalled" means one of: `destroy.txt` or `rm.txt` says `STILL BLOCKED`; one minute after the run's end a flush thread is still above 50% CPU or `Dirty` is still above 8000 kB; `steps.txt` stopped changing for 6 minutes. Then:
1. **Capture** into `$L/wedge.txt`, without any `zpool`, `zfs` or `zdb` command, and reading kernel files only in the background (`(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`): `ps -eLo pid,tid,stat,etime,pcpu,comm,wchan:32 | grep -E ' D|flush|txg|zpool|zfs|python'`; `/proc/<tid>/stack` of the busiest flush thread and of each D-state process; `awk '/^Dirty:|^Writeback:/' /proc/meminfo`; `cat /proc/spl/kstat/zfs/dmu_tx`; `tail -n 100 /proc/spl/kstat/zfs/dbgmsg`; `sudo -n dmesg | tail -n 150`.
2. Restore the four sysctls by hand if they differ from step 0's, wait 2 minutes, and capture `Dirty` and the flush threads again: if the stall has ended, do not reboot; carry on with step 6 (no further run).
3. Otherwise make sure the logs are on disk (`sync -f $L/wedge.txt`, which syncs only the file system of `/var/log`), and **reboot (pre-approved, this one only):** `echo b | sudo -n tee /proc/sysrq-trigger`; if the VM is not down within 2 minutes, `sudo -n systemctl reboot -ff`. Wait for the VM, then quote `uptime`, `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty), the four sysctls (must be step 0's after a reboot), and `ls -l $L/wedge.txt`. No further run.

## Step 6: cleanup

- If no reboot happened: no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty), then unload with `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.
- The four sysctls equal step 0's.
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is2` (`sudo -n rm -rf /var/tmp/task-1123-is2`). Anything else new: quote it and leave it.
- `/etc/zfs/zpool.cache`: quote `ls -l` of it; it must be as in step 0.
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is2`.

## Report

Write `/var/log/task-1123-is2/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, python version, the four vm sysctls, the three kernel sysctls and the zfs service states of step 0, `trees.txt` and the log line, the build rc, duration and warnings, the `zfs_config.h` writeback lines, the srcversion with "(DEBUG mode)", the script's sha256.
2. **Table:** tag | refquota | pool | load: max quota/s, max flush CPU %, Dirty kB (last window), min bystander ticks/10 s | fsync probe rc/errno | killed: quota/s, Dirty, bystander | recovery: quota/s, Dirty, bystander | verdict.
3. **Verdict lines:** `#19737 on master a1daebfec3f: STALL REPRODUCED in <runs> (control N: <bystander ticks>, quota/s 0)` or `NOT REPRODUCED in <runs>`, followed by one sentence: the flush thread's stack through `zfs_putpage`/`dmu_tx_assign`, whether killing the writers ended it, and how fast raising the quota did. Then `#19915 on master a1daebfec3f: fsync returned <rc> with errno <e> during the stall` (or what it returned).
4. **Quotes:** per run, what step 4 lists (run N and run Q in full, then what Q2 changes), step 5's quotes if any, and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
