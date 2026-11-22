TASK-1123-RZRES (from the coordinator session "main"): functional, about 1 h 30 with its build, at most 2 h 15, no reboot expected. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It tries to reproduce upstream issue openzfs/zfs#19614 ("Checksum errors on replacement disk after RAIDZ resilver under write load") on upstream master `a1daebfec3f`: a raidz1 child fails, the pool runs degraded under write load, the child is replaced under write load, and a scrub then counts what the resilver left wrong. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rzres/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The report (first reporter, 2.4.4, arm64):** five 5-wide raidz1 vdevs of SATA SSDs. One SSD failed and was replaced after an RMA, so the pool ran degraded for a while. The resilver ran while the pool kept serving reads and writes. Right after it completed, the new disk showed 100+ checksum errors and went DEGRADED with "too many errors". A second replacement, after a `blkdiscard` of the new disk, did the same. A scrub with writes stopped repaired 1.16 GiB, and no error came back after that. The raidz vdevs were never expanded.
- **A second reporter is out of scope here:** his raidz2 was expanded twice (8 to 9 to 10 wide) and the errors were zero-filled 4 KiB sectors on the new disk. That matches upstream #19395, fixed on master by `21df1e4f74d` ("raidz: resilver every child a block born before an expansion spans"), which 2.4.4 does not have.
- **Nothing on master is known to explain the first case.** A copy of the new disk's column that is never written, or written with the wrong contents, is silent until a scrub or a second failure: with raidz1, a second failure in such a row loses data.
- **What this task shows:** whether master, on file vdevs, leaves checksum errors on the replacement of a faulted raidz1 child after a resilver under write load. NOT REPRODUCED on every run is a valid and useful result.

## What is tested

The tree `zm` is upstream master, fetched from https://github.com/openzfs/zfs:

| tree | commit | tree hash |
|---|---|---|
| zm | a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 |

The runs, each on a fresh pool `rzres` of two 5-wide raidz1 vdevs of 1 GiB files, ashift 12, `compression=lz4`:

| tag | autotrim | hold (s) | pulse | what it adds |
|---|---|---|---|---|
| A | off | 0 | 0 | the report as written: the resilver runs at its own speed under the load |
| B | off | 300 | 1 | the resilver is set up and held for 300 s of writes, then allowed to progress 2 s out of every 10 (`zfs_scan_suspend_progress`), so its traversal spans many txgs of writes, frees and snapshot destroys |
| C | on | 300 | 1 | B with autotrim: TRIM on a file vdev punches a hole, which reads back as zeros like a discarded SSD |
| B2, C2 | as B, C | | | repeats, only if time allows (see step 4) |

Each run (the script `rz-run.sh` below does all of it):
1. writes 2 GiB of half-compressible data in 8 files, a 256 MiB file for sync writes and 3000 small files, then scrubs: this baseline must find nothing;
2. starts the load: random overwrites of 4 KiB to 1 MiB (4 x 5 MiB/s), 128 KiB writes each followed by `fsync` (3 MiB/s), small files created, overwritten and deleted, and a snapshot a minute with the previous one destroyed;
3. after 60 s, faults `d2` (`zpool offline -f`) and leaves the pool degraded under load for 180 s;
4. `zpool replace` of `d2` by a new sparse file (it reads as zeros, like a discarded disk), under load, and waits for the resilver (at most 30 minutes);
5. keeps the load 120 s more, stops it, syncs, and scrubs the quiet pool;
6. saves `zpool status -vp` at each stage, the checksum ereports before and during the final scrub, `zpool history -il`, `zdb -d`, and for up to 3 objects named by level-0 checksum ereports, `zdb -ddddd` of the object (its block pointers with their birth txgs);
7. writes a verdict line and destroys the pool.

**REPRODUCED** for a run means, after the final scrub, with no READ or WRITE error on any leaf: a CKSUM count above 0 on any leaf, or a scrub line other than "repaired 0B". The script writes INCONCLUSIVE if a leaf has READ or WRITE errors.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- Every pool uses file vdevs under `/var/tmp/task-1123-rzres`. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko`;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool;
  - `ls -d $D/vdev-* 2>&1` says none exists;
  - `df -h /var/tmp` shows at least 8 GiB free.
- **The build and every run run detached** (`setsid nohup ... &`) and are polled.
- **Hangs.** A build still going after 60 minutes, or a run whose `steps.txt` has not changed for 40 minutes, is a hang (the script itself gives up on a resilver after 30 minutes).
  - Capture into `$L/hang.txt`:
    - `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|zfs|fio|rz-run'`;
    - `timeout 5 sudo -n cat /proc/<pid>/stack` of each `zpool`, `zfs` and `fio` process;
    - `sudo -n dmesg | tail -n 100`.
  - Then stop and report. Do not reboot.
- **Kernel log.** After each run, check the new kernel lines since that run's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock`. None are expected. A debug `VERIFY` or `ASSERT` failure is a finding: quote it in full with its stack, capture as for a hang, and stop.
- Run `sudo -n fstrim -av` after deleting trees. Modules are unloaded at the end.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 15 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `command -v ksh git fio && fio --version`: if `fio` is missing, stop and report (do not install it);
- `pgrep -a zed`: quote it (a running zed is not a stop; do not start or stop it);
- `ls -d /var/tmp/task-1123-rzres /var/log/task-1123-rzres 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty;
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it.

Note the time of step 0: runs B2 and C2 start only if step 0 was less than 1 h 35 before.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-rzres; L=/var/log/task-1123-rzres; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
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
D=/var/tmp/task-1123-rzres; L=/var/log/task-1123-rzres
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes.

## Step 3: load the module and write the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)".

**Write the script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-rzres
cat > $D/rz-run.sh <<'RZRUN_END'
#!/bin/bash
# rz-run.sh <tag> <autotrim on|off> <hold-seconds> <pulse 0|1>
# One attempt at openzfs/zfs#19614: replace a faulted raidz1 child under
# write load, then scrub and count checksum errors.  Runs as root.
set -u
D=/var/tmp/task-1123-rzres; L=/var/log/task-1123-rzres; Z=$D/zm
T=$1; AT=$2; HOLD=$3; PULSE=$4
P=rzres; V=$D/vdev-$T; M=$D/mnt-$T; O=$L/run-$T
ZPOOL=$Z/zpool; ZFS=$Z/zfs; ZDB=$Z/zdb
PP=/sys/module/zfs/parameters
mkdir -p "$V" "$M" "$O"
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }
param() { echo "$2" > "$PP/$1"; log "param $1=$(cat "$PP/$1")"; }
vtable() {	# NAME STATE READ WRITE CKSUM of every vdev line
	"$ZPOOL" status -p "$P" | awk '/^config:/ {c = 1} /^errors:/ {c = 0}
	    c && NF >= 5 && $3 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ {
	    print $1, $2, $3, $4, $5}'
}
snap() {
	"$ZPOOL" status -vp "$P" > "$O/status-$1.txt" 2>&1
	vtable > "$O/vt-$1.txt"
}
fail() {
	echo 0 > "$PP/zfs_scan_suspend_progress"
	log "STOP: $*"; echo "STOP $*" > "$O/verdict.txt"; exit 1
}

log "run $T autotrim=$AT hold=$HOLD pulse=$PULSE"
param zfs_scan_suspend_progress 0
param zfs_zevent_len_max 50000

# 1. Pool: two 5-wide raidz1 of 1 GiB files, ashift 12, lz4.
for i in 0 1 2 3 4 5 6 7 8 9; do truncate -s 1G "$V/d$i"; done
truncate -s 1G "$V/new"		# the replacement: sparse, reads as zeros
"$ZPOOL" create -f -o ashift=12 -o cachefile=none -o autotrim="$AT" \
    -O compression=lz4 -O mountpoint="$M" "$P" \
    raidz1 "$V/d0" "$V/d1" "$V/d2" "$V/d3" "$V/d4" \
    raidz1 "$V/d5" "$V/d6" "$V/d7" "$V/d8" "$V/d9" || fail "zpool create"
"$ZFS" create "$P/fs" || fail "zfs create"
mkdir -p "$M/fs/small" "$M/fs/churn"

# 2. Base data: 8 x 256 MiB files, a 256 MiB file for sync writes,
#    3000 small files of 1-64 KiB.
export FIO_DIR=$M/fs
cat > "$O/fill.fio" <<'EOF'
[global]
directory=${FIO_DIR}
ioengine=psync
buffer_compress_percentage=50
refill_buffers=1
bs=128k
rw=write
end_fsync=1
[fill]
filename_format=base.$jobnum
numjobs=8
size=256m
[syncfill]
filename=syncfile
size=256m
EOF
fio --output="$O/fio-fill.txt" "$O/fill.fio" || fail "fio fill"
for i in $(seq 1 3000); do
	head -c $(( (RANDOM % 64 + 1) * 1024 )) /dev/urandom > "$M/fs/small/s$i"
done
"$ZPOOL" sync "$P"
log "fill done"

# 3. Baseline: a scrub of the healthy pool must find nothing.
timeout 1200 "$ZPOOL" scrub -w "$P" || fail "baseline scrub"
snap baseline
awk '$3 + $4 + $5 > 0 {bad = 1} END {exit bad}' "$O/vt-baseline.txt" ||
    fail "baseline scrub found errors"
log "baseline clean"
"$ZPOOL" events -c > /dev/null

# 4. Load, until $O/stop exists: random overwrites of the base files
#    (4 KiB-1 MiB, 4 x 5 MiB/s), 128 KiB writes with fsync (3 MiB/s,
#    indirect ZIL writes), small-file create/overwrite/delete, and a
#    snapshot a minute with the previous one destroyed.
cat > "$O/load.fio" <<'EOF'
[global]
directory=${FIO_DIR}
ioengine=psync
buffer_compress_percentage=50
refill_buffers=1
time_based=1
runtime=7200
[ow]
filename_format=base.$jobnum
numjobs=4
size=256m
rw=randwrite
bsrange=4k-1m
rate=5m
[sy]
filename=syncfile
size=256m
rw=randwrite
bs=128k
fsync=1
rate=3m
EOF
rm -f "$O/stop"
fio --output="$O/fio-load.txt" "$O/load.fio" &
FIOPID=$!
(
	while [ ! -e "$O/stop" ]; do
		for i in $(seq 1 200); do
			head -c $(( (RANDOM % 64 + 1) * 1024 )) /dev/urandom \
			    > "$M/fs/churn/c$i"
		done
		for i in $(seq 1 20); do
			dd if=/dev/urandom bs=4k count=1 conv=notrunc \
			    of="$M/fs/small/s$(( RANDOM % 3000 + 1 ))" \
			    seek=$(( RANDOM % 4 )) status=none
		done
		rm -f "$M/fs/churn/"c*
	done
) &
CHURNPID=$!
(
	n=0
	while [ ! -e "$O/stop" ]; do
		sleep 60
		n=$((n + 1))
		"$ZFS" snapshot "$P/fs@s$n"
		[ $n -gt 1 ] && "$ZFS" destroy "$P/fs@s$((n - 1))"
	done
) &
SNAPPID=$!
log "load started (fio $FIOPID, churn $CHURNPID, snapshots $SNAPPID)"
stop_load() {
	touch "$O/stop"
	kill "$FIOPID" "$SNAPPID" 2> /dev/null
	wait "$FIOPID" "$CHURNPID" "$SNAPPID" 2> /dev/null
	log "load stopped"
}
sleep 60

# 5. The disk fails: d2 is faulted and the pool runs degraded under load.
"$ZPOOL" offline -f "$P" "$V/d2" || { stop_load; fail "offline -f"; }
log "d2 faulted"
sleep 180
snap degraded

# 6. Replace under load.  With HOLD > 0 the resilver is set up but held
#    for HOLD seconds while the load goes on, then released.  With
#    PULSE=1 it is then allowed to progress 2 s out of every 10, so that
#    its traversal spans many more txgs of writes.
[ "$HOLD" -gt 0 ] && param zfs_scan_suspend_progress 1
"$ZPOOL" replace "$P" "$V/d2" "$V/new" || { stop_load; fail "replace"; }
RS=$(date +%s)
log "replace issued"
if [ "$HOLD" -gt 0 ]; then
	sleep "$HOLD"
	snap held
	param zfs_scan_suspend_progress 0
fi
k=0
while "$ZPOOL" status "$P" | grep -q -E 'resilver in progress|replacing-'; do
	k=$((k + 1))
	if [ $k -ge 180 ]; then		# 30 minutes of 10 s polls
		snap resilver-timeout
		stop_load
		fail "resilver not finished after 30 minutes"
	fi
	[ $((k % 6)) -eq 0 ] && "$ZPOOL" status "$P" | grep -A2 'scan:' \
	    >> "$O/resilver-progress.txt"
	if [ "$PULSE" -eq 1 ]; then
		echo 0 > "$PP/zfs_scan_suspend_progress"
		sleep 2
		echo 1 > "$PP/zfs_scan_suspend_progress"
		sleep 8
	else
		sleep 10
	fi
done
echo 0 > "$PP/zfs_scan_suspend_progress"
log "resilver done after $(( $(date +%s) - RS )) s"
snap post-resilver

# 7. Two more minutes of load, then stop it and sync.
sleep 120
stop_load
"$ZPOOL" sync "$P"
snap pre-scrub
"$ZPOOL" events -v "$P" > "$O/events-pre-scrub.txt" 2>&1
"$ZPOOL" events -c > /dev/null

# 8. The check: a scrub of the quiet pool.
timeout 1800 "$ZPOOL" scrub -w "$P" || log "scrub -w rc=$?"
snap final
"$ZPOOL" events -v "$P" > "$O/events-scrub.txt" 2>&1
"$ZPOOL" history -il "$P" > "$O/history.txt" 2>&1
grep -E 'scan setup|scan done|scan aborted|vdev attach|vdev online|offline' \
    "$O/history.txt" > "$O/history-scan.txt"
for f in pre-scrub scrub; do
	awk 'function flush() {
		if (cls ~ /checksum/) print vp, os, ob, lv, bl, off, sz
		cls = vp = os = ob = lv = bl = off = sz = ""
	    }
	    /ereport\.fs\.zfs\.|sysevent\./ && !/class = / {flush()}
	    /^[ \t]*class = / {cls = $3}
	    /^[ \t]*vdev_path = / {vp = $3}
	    /^[ \t]*zio_objset = / {os = $3}
	    /^[ \t]*zio_object = / {ob = $3}
	    /^[ \t]*zio_level = / {lv = $3}
	    /^[ \t]*zio_blkid = / {bl = $3}
	    /^[ \t]*zio_offset = / {off = $3}
	    /^[ \t]*zio_size = / {sz = $3}
	    END {flush()}' "$O/events-$f.txt" > "$O/cksum-$f.txt"
done
"$ZDB" -d "$P" > "$O/zdb-d.txt" 2>&1

# For up to 3 objects named by level-0 checksum ereports of the scrub,
# the block pointers with their birth txgs (B=logical/physical).
awk '$4 == "0x0" {print $2, $3}' "$O/cksum-scrub.txt" | sort -u | head -3 |
while read -r os ob; do
	ds=$(awk -v id=", ID $((os))," 'index($0, "Dataset ") == 1 &&
	    index($0, id) > 0 {print $2}' "$O/zdb-d.txt")
	[ -n "$ds" ] || continue
	"$ZDB" -ddddd "$ds" "$((ob))" > "$O/zdb-obj-$((os))-$((ob)).txt" 2>&1
done

# 9. Verdict for this run.
NEWCK=$(awk -v n="$V/new" '$1 == n {print $5}' "$O/vt-final.txt")
OTHER=$(awk -v n="$V/new" '$1 != n && $1 ~ /^\// {s += $5} END {print s + 0}' \
    "$O/vt-final.txt")
IOERR=$(awk '$1 ~ /^\// {s += $3 + $4} END {print s + 0}' "$O/vt-final.txt")
SCAN=$(grep 'scan:' "$O/status-final.txt" | sed 's/^ *//')
ERRS=$(grep '^errors:' "$O/status-final.txt")
if [ "$IOERR" -gt 0 ]; then
	V1="INCONCLUSIVE (READ/WRITE errors $IOERR)"
elif [ "${NEWCK:-0}" -gt 0 ] || [ "$OTHER" -gt 0 ] ||
    ! echo "$SCAN" | grep -q 'repaired 0B'; then
	V1="REPRODUCED"
else
	V1="NOT REPRODUCED"
fi
echo "RUN $T: new CKSUM ${NEWCK:-?}, other leaves CKSUM $OTHER; $SCAN;" \
    "$ERRS => $V1" | tee "$O/verdict.txt" | tee -a "$O/steps.txt"

# 10. Cleanup of this run.
"$ZPOOL" destroy "$P" || log "zpool destroy failed"
rm -rf "$V" "$M"
param zfs_scan_suspend_progress 0
log "run $T end"
exit 0
RZRUN_END
chmod 755 $D/rz-run.sh
sha256sum $D/rz-run.sh | cut -c1-64; wc -l < $D/rz-run.sh; bash -n $D/rz-run.sh && echo syntax-ok
```

Expected: `6f6574dabfee7b54b2b26b13026ae0601fbb4c9294af3c8e5695ba484d511f29`, `249`, `syntax-ok`. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-rzres; L=/var/log/task-1123-rzres; T=<tag>
echo "task-1123-rzres: run $T start" | sudo -n tee /dev/kmsg > /dev/null
setsid nohup sudo -n bash $D/rz-run.sh $T <autotrim> <hold> <pulse> > $L/run-$T.log 2>&1 < /dev/null &
```

| order | tag | arguments | expected duration |
|---|---|---|---|
| 1 | A | `A off 0 0` | about 12 minutes |
| 2 | B | `B off 300 1` | about 20 minutes |
| 3 | C | `C on 300 1` | about 20 minutes |
| 4 | B2 | `B2 off 300 1` | only if step 0 was less than 1 h 35 ago |
| 5 | C2 | `C2 on 300 1` | only if step 0 was still less than 1 h 35 ago |

Poll `tail -n 3 $L/run-$T/steps.txt` every few minutes until `$L/run-$T/verdict.txt` exists and `steps.txt` ends with `run $T end` (or a `STOP:` line).

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `$L/run-$T/verdict.txt`;
- `vt-baseline.txt`, `vt-post-resilver.txt` and `vt-final.txt` (NAME STATE READ WRITE CKSUM of each vdev);
- the `scan:` line and the line after it from `status-post-resilver.txt` (the "resilvered" line) and from `status-final.txt`, and the `errors:` line and what follows it from `status-final.txt`;
- `history-scan.txt` (the txgs of the offline, the attach, `scan setup` with its `mintxg`/`maxtxg`, and `scan done`): from them, how many txgs the resilver spanned;
- `wc -l` of `cksum-pre-scrub.txt` and `cksum-scrub.txt` (one line per checksum ereport: vdev path, objset, object, level, blkid, leaf offset, size), and their first 20 lines;
- the kernel-line check since the run's mark.

**If a run is REPRODUCED:**
- for each `zdb-obj-*.txt`: its header line with `dblk`, and the `L0` lines whose first column (the file offset, in hex) equals blkid x dblk for the blkids of `cksum-scrub.txt`; quote the `B=<logical>/<physical>` birth txgs and say, from `history-scan.txt`, whether each block was born before the replace's attach txg, between the attach and the `scan setup` `maxtxg`, or after it;
- count the ereports by vdev path and by size (`cut -d' ' -f1 | sort | uniq -c`, `cut -d' ' -f7 | sort | uniq -c`);
- the run's files stay in `$L/run-$T` (the pool itself is destroyed by the script); carry on with the next run.

**If a run says STOP:** quote its last 30 lines of `steps.txt` and `status-*.txt` files, then `timeout 60 sudo -n $D/zm/zpool destroy -f rzres` and `sudo -n rm -rf $D/vdev-$T $D/mnt-$T`. If the STOP is the baseline scrub finding errors, stop the task (the environment is at fault). Otherwise carry on with the next run.

## Step 5: cleanup

- no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty);
- unload with `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty;
- `sudo -n rm -rf /var/tmp/task-1123-rzres`, `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-rzres`.

## Report

Write `/var/log/task-1123-rzres/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, fio version, zed, hung_task_timeout_secs, `trees.txt` and the log line, the build rc, duration and warnings, the srcversion with "(DEBUG mode)", the script's sha256.
2. **Table:** tag | autotrim | hold/pulse | resilver duration (s) | resilver txg span (attach txg, scan maxtxg, scan done txg) | CKSUM on new / on other leaves | final scrub line | checksum ereports (pre-scrub / scrub) | verdict.
3. **Verdict line:** `#19614 on master a1daebfec3f: REPRODUCED in <runs>` or `#19614 on master a1daebfec3f: NOT REPRODUCED in <n> runs (A, B, C...)`, followed by the evidence in one sentence: for REPRODUCED, which vdev, how many sectors, of what size, and where their birth txgs fall relative to the attach and `maxtxg`; for NOT REPRODUCED, the txg span the resilvers covered under load.
4. **Quotes:** per run, what step 4 lists (run A in full, then what differs), and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
