TASK-1123-IS1 (from the coordinator session "main"): functional, about 1 h with its build, at most 2 h. **One reboot is pre-approved, for step 5 only, and only if a run leaves the pool SUSPENDED**; nowhere else. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It tries to reproduce upstream issue openzfs/zfs#20004 ("Fresh 128 MiB mirror suspends during synchronous writes with ENOSPC") on upstream master `a1daebfec3f`, with the reporter's own workload, and measures what the ZIL allocates while it runs. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is1/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The report (johnpwhite, 23/11, master `a1daebfec`, debug build, NixOS kernel 6.18):** a mirror of two sparse 128 MiB files on a 256 MiB tmpfs, `sync=always`, four writers that each overwrite the first 64 KiB of their own file in a loop (`dd bs=8k count=8 conv=notrunc`). After about two minutes, writes fail with `ENOSPC` and the pool is `SUSPENDED`, with 214 KiB allocated, 112 MiB free and no I/O error on any leaf. `zpool events`: `ereport.fs.zfs.data` with `zio_err 0x1c` (ENOSPC), then `ereport.fs.zfs.io_failure`. dbgmsg: `zil block allocation failure: min_size 20480, max_size 20480, error 28`, then 64 KiB data allocations of the syncing txg failing and `trying ganging ... error 28`.
- **The mechanism this task checks (read on master by main):** `zio_alloc_zil()` (`module/zfs/zio.c:5006`) allocates log blocks in open context straight from the metaslabs (log, special, embedded-log, then normal class) with no check against the pool's slop space: slop is enforced only at `dmu_tx_assign()` time, which log blocks never go through. A log block stays allocated until `zil_sync()` frees it after its txg has synced (then two txgs of defer). So on a small, fast pool, synchronous writes can allocate the free space the syncing txg needs for its data; the syncing txg's writes then fail with ENOSPC, the pool suspends, and since the log blocks holding the space belong to that same txg, `zpool clear` is not expected to help. The reporter's per-metaslab trace (16 MiB free per metaslab but 0.5 to 18.5 KiB allocatable, the difference in `ms_allocating[]` of the open txg) fits this.
- **What this task shows:** whether master suspends that pool on our VM, how fast the ZIL allocates in the normal class compared with the pool's size (zil kstats every 5 s), and whether the same writers without the ZIL (`sync=standard`) leave the pool alone. NOT REPRODUCED in every run is a valid and useful result.

## What is tested

The tree `zm` is upstream master, fetched from https://github.com/openzfs/zfs:

| tree | commit | tree hash |
|---|---|---|
| zm | a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 |

Each run (the script `is1-run.sh` below does all of it) mounts a fresh 256 MiB tmpfs, creates the pool `smallmir` (mirror of two sparse 128 MiB files on it, `cachefile=none`, mountpoint under `$D`), sets `sync`, starts the writers for the given number of seconds, and samples every 5 s, without any blocking command: the pool state (`/proc/spl/kstat/zfs/smallmir/state`), the zil kstats (`zil_commit_*`, `zil_itx_metaslab_normal_*`), and the last txgs. If the pool suspends, it saves the dbgmsg, the D-state threads with their stacks, `zpool status -p`, `zpool list -p` and `zpool events -v` (each started in the background and given 30 s), writes `REPRODUCED`, and leaves the pool as it is. Otherwise it waits for the writers (60 s at most), saves the same, writes `NOT REPRODUCED`, destroys the pool and unmounts the tmpfs.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- The pool lives on a tmpfs under `/var/tmp/task-1123-is1`. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko`;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool;
  - `mount | grep -c task-1123-is1` prints 0.
- **The build and every run run detached** (`setsid nohup ... &`) and are polled.
- **Once a pool is SUSPENDED** (the run's `verdict.txt` says REPRODUCED, or `/proc/spl/kstat/zfs/smallmir/state` says it): run no `zpool`, `zfs` or `zdb` command yourself except the one `zpool clear` of step 5, and read kernel files only in the background: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`. Start no further run.
- **Hangs.** A build still going after 60 minutes, or a run whose `steps.txt` has not changed for 10 minutes while the pool is not SUSPENDED, is a hang: capture `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zpool|zfs|dd '`, `/proc/<tid>/stack` of each D-state thread (in the background as above) and `sudo -n dmesg | tail -n 100` into `$L/hang.txt`, then stop and report. Do not reboot for a hang.
- **Kernel log.** After each run, check the new kernel lines since that run's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock|suspended`. Pool suspend `WARNING` lines and `blocked for more than` reports are expected only after a run that suspended: quote them. A `VERIFY` or `ASSERT` failure is a finding: quote it in full with its stack, capture as for a hang, and stop.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 8 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `command -v ksh git && sudo -n true && echo sudo-ok`;
- `ls -d /var/tmp/task-1123-is1 /var/log/task-1123-is1 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic`: quote them; do not change them. **If `hung_task_panic` is not 0, run only step 4's run C and report** (a suspended pool would panic the VM);
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service zfs-load-module.service 2>&1`: quote it (none should import or mount anything after a reboot; the pool has `cachefile=none` and its tmpfs is gone after a reboot anyway).

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is1; L=/var/log/task-1123-is1; U=https://github.com/openzfs/zfs
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
D=/var/tmp/task-1123-is1; L=/var/log/task-1123-is1
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes.

## Step 3: load the module and write the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- quote `cat /sys/module/zfs/parameters/zfs_flags /sys/module/zfs/parameters/zfs_txg_timeout /sys/module/zfs/parameters/spa_slop_shift /sys/module/zfs/parameters/zfs_embedded_slog_min_ms` (the debug build's `zfs_flags` includes `ZFS_DEBUG_METASLAB_ALLOC`, bit 13, which logs the allocation failures the reporter quotes).

**Write the script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is1
cat > $D/is1-run.sh <<'IS1RUN_END'
#!/bin/bash
# is1-run.sh <tag> <sync always|standard> <writers> <seconds>
# One attempt at openzfs/zfs#20004: a mirror of two 128 MiB files on a
# 256 MiB tmpfs, <writers> loops of 8 x 8 KiB overwrites at the start of
# their own file.  Never waits on a writer or on a pool command without a
# bound: a suspended pool blocks them for good.  Runs as root.
set -u
D=/var/tmp/task-1123-is1; L=/var/log/task-1123-is1; Z=$D/zm
T=$1; SYNC=$2; NW=$3; SECS=$4
P=smallmir; F=$D/tmpfs-$T; M=$D/mnt-$T; O=$L/run-$T
ZPOOL=$Z/zpool; ZFS=$Z/zfs
K=/proc/spl/kstat/zfs
mkdir -p "$F" "$M" "$O"
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }
# A pool command that may block: started in the background, output read
# after at most $2 seconds.  It is never waited for.
bg() {
	local out=$1 secs=$2; shift 2
	( timeout "$secs" "$@" > "$out" 2>&1; echo "rc=$?" >> "$out" ) &
	local i=0
	while [ $i -lt "$secs" ] && ! grep -q '^rc=' "$out" 2>/dev/null; do
		sleep 1; i=$((i + 1))
	done
	grep -q '^rc=' "$out" 2>/dev/null || echo "STILL BLOCKED after ${secs}s" >> "$out"
}
zilk() {	# the zil kstats that count commits and normal-class lwb space
	awk '$1 ~ /^zil_(commit_count|commit_error_count|commit_stall_count|commit_suspend_count|itx_metaslab_normal_(count|bytes|write|alloc))$/ {printf "%s=%s ", $1, $3}' $K/zil
}
state() { cat $K/$P/state 2>/dev/null || echo none; }

log "run $T sync=$SYNC writers=$NW seconds=$SECS"
echo $((64 << 20)) > /sys/module/zfs/parameters/zfs_dbgmsg_maxsize
echo 0 > $K/dbgmsg
mount -t tmpfs -o size=256m tmpfs "$F" || { log "STOP: tmpfs mount"; echo "STOP" > "$O/verdict.txt"; exit 1; }
truncate -s 128m "$F/leader" "$F/lagging"
"$ZPOOL" create -f -o cachefile=none -O mountpoint="$M" "$P" \
    mirror "$F/leader" "$F/lagging" || { log "STOP: zpool create"; umount "$F"; echo "STOP" > "$O/verdict.txt"; exit 1; }
"$ZFS" set sync="$SYNC" "$P"
"$ZPOOL" list -p -o name,size,alloc,free,health "$P" > "$O/list-start.txt"
"$ZPOOL" get -Hp ashift "$P" >> "$O/list-start.txt"
"$ZPOOL" events -c > /dev/null
echo "start $(zilk)" > "$O/zil.txt"
log "pool created, state $(state)"

# The reporter's writers, verbatim apart from the paths.
for n in $(seq 1 "$NW"); do
	timeout "$SECS" sh -c '
		while dd if=/dev/urandom of="$1" bs=8k count=8 conv=notrunc \
		    2>>"$2"; do
			:
		done
	' sh "$M/sync.$n" "$O/writer.$n.log" &
done

# Sample every 5 s, lock-free: pool state, zil kstats, last txgs.
t=0; SUSP=0
while [ $t -lt $((SECS + 15)) ]; do
	sleep 5; t=$((t + 5))
	s=$(state)
	echo "t=$t state=$s $(zilk)" >> "$O/zil.txt"
	{ echo "== t=$t"; tail -n 4 $K/$P/txgs 2>/dev/null; } >> "$O/txgs.txt"
	if [ "$s" = SUSPENDED ]; then SUSP=$t; break; fi
	if [ $t -ge "$SECS" ] && ! pgrep -f "$M/sync\." > /dev/null; then break; fi
done
log "sampling ended at t=$t, state $(state)"
cat $K/$P/txgs > "$O/txgs-all.txt" 2>&1
ENOSPC=$(cat "$O"/writer.*.log 2>/dev/null | grep -c 'No space left')
EIO=$(cat "$O"/writer.*.log 2>/dev/null | grep -c 'Input/output error')
log "writer errors: ENOSPC lines $ENOSPC, EIO lines $EIO"

if [ $SUSP -gt 0 ]; then
	log "SUSPENDED at t=$SUSP"
	cp $K/dbgmsg "$O/dbgmsg.txt"
	ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg_|z_wr|zil|dd ' > "$O/ps.txt"
	for tid in $(awk '$3 ~ /D/ || $5 ~ /^txg_/ {print $2}' "$O/ps.txt" | head -20); do
		{ echo "== tid $tid $(cat /proc/$tid/comm 2>/dev/null)"; cat /proc/$tid/stack 2>&1; } >> "$O/stacks.txt"
	done
	bg "$O/status-susp.txt" 30 "$ZPOOL" status -p "$P"
	bg "$O/list-susp.txt" 30 "$ZPOOL" list -p -o name,size,alloc,free,health "$P"
	bg "$O/events-susp.txt" 30 "$ZPOOL" events -v "$P"
	grep -cE 'zil block allocation failure' "$O/dbgmsg.txt" > "$O/dbg-counts.txt"
	grep -cE 'trying ganging' "$O/dbgmsg.txt" >> "$O/dbg-counts.txt"
	grep -E 'zil block allocation failure' "$O/dbgmsg.txt" | head -3 > "$O/dbg-zil-first.txt"
	grep -E 'allocation failure' "$O/dbgmsg.txt" | grep -v 'zil block' | head -5 > "$O/dbg-ms-first.txt"
	echo "RUN $T: SUSPENDED at t=${SUSP}s (sync=$SYNC, $NW writers); ENOSPC lines $ENOSPC, EIO lines $EIO => REPRODUCED" |
	    tee "$O/verdict.txt" | tee -a "$O/steps.txt"
	log "run $T end (pool left suspended)"
	exit 0
fi

# Not suspended: the writers are gone or about to be.
i=0
while pgrep -f "$M/sync\." > /dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
pgrep -f "$M/sync\." > /dev/null && log "writers still alive after 60 s"
cp $K/dbgmsg "$O/dbgmsg.txt"
grep -cE 'zil block allocation failure' "$O/dbgmsg.txt" > "$O/dbg-counts.txt"
grep -cE 'trying ganging' "$O/dbgmsg.txt" >> "$O/dbg-counts.txt"
bg "$O/status-end.txt" 30 "$ZPOOL" status -p "$P"
bg "$O/list-end.txt" 30 "$ZPOOL" list -p -o name,size,alloc,free,health "$P"
bg "$O/events-end.txt" 30 "$ZPOOL" events "$P"
echo "end $(zilk)" >> "$O/zil.txt"
echo "RUN $T: not suspended in ${SECS}s (sync=$SYNC, $NW writers); ENOSPC lines $ENOSPC, EIO lines $EIO => NOT REPRODUCED" |
    tee "$O/verdict.txt" | tee -a "$O/steps.txt"
bg "$O/destroy.txt" 60 "$ZPOOL" destroy "$P"
umount "$F" && rmdir "$F" "$M"
log "run $T end"
exit 0
IS1RUN_END
chmod 755 $D/is1-run.sh
sha256sum $D/is1-run.sh | cut -c1-64; wc -l < $D/is1-run.sh; bash -n $D/is1-run.sh && echo syntax-ok
```

Expected: `d434ee840b9664b0b7164a03e14905c28c215fc124daf879ed256bee88c6ffc7`, `107`, `syntax-ok`. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is1; L=/var/log/task-1123-is1; T=<tag>
echo "task-1123-is1: run $T start" | sudo -n tee /dev/kmsg > /dev/null
setsid nohup sudo -n bash $D/is1-run.sh $T <sync> <writers> <seconds> > $L/run-$T.log 2>&1 < /dev/null &
```

| order | tag | arguments | when | expected duration |
|---|---|---|---|---|
| 1 | C | `C standard 4 60` | always (control: the same writers without the ZIL) | about 1 to 2 minutes |
| 2 | A | `A always 4 180` | always (the report) | at most about 5 minutes |
| 3 | A2 | `A2 always 4 180` | only if A was NOT REPRODUCED | at most about 5 minutes |
| 4 | A3 | `A3 always 4 180` | only if A2 was NOT REPRODUCED | at most about 5 minutes |
| 5 | A8 | `A8 always 8 180` | only if A3 was NOT REPRODUCED | at most about 5 minutes |

Poll `tail -n 3 $L/run-$T/steps.txt` every 30 s until `steps.txt` ends with a `run $T end` line (or a `STOP:` line). **The first run that says REPRODUCED ends step 4: go to step 5.** If run C itself says REPRODUCED, that is a finding (the pool suspends without the ZIL): go to step 5 all the same and say so.

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `$L/run-$T/verdict.txt`;
- `list-start.txt` (size, ashift), and `list-susp.txt` or `list-end.txt`;
- `zil.txt` in full (one line per 5 s), and from it: the `zil_commit_count` rate per second and the `zil_itx_metaslab_normal_alloc` bytes allocated per 5 s, at the start, at the peak and in the last sample;
- the `txgs.txt` sample nearest the end and, from `txgs-all.txt`, the last 6 txgs (txg, state, ndirty, nwritten, and the open/quiesce/wait/sync times): how long the last txgs stayed open, compared with `zfs_txg_timeout`;
- `dbg-counts.txt` (count of `zil block allocation failure` lines, then of `trying ganging` lines);
- `wc -l` of every `writer.*.log` and their first 3 lines;
- the kernel-line check since the run's mark.

**For a run that says REPRODUCED, also quote:** `dbg-zil-first.txt`, `dbg-ms-first.txt`, `status-susp.txt` (in full: the pool state, the READ/WRITE/CKSUM counts of every vdev, the `errors:` line), from `events-susp.txt` the class line of every event and the `zio_err`, `zio_size`, `zio_objset`, `zio_object`, `zio_level` lines of the first 3 `ereport.fs.zfs.data` events, `ps.txt`, and `stacks.txt` for `txg_sync` and for one `dd`.

## Step 5: only if a run said REPRODUCED

1. Note the time. Read `/proc/spl/kstat/zfs/smallmir/state` in the background (it must say `SUSPENDED`).
2. **One `zpool clear`, observed, never waited for:**
   ```sh
   D=/var/tmp/task-1123-is1; L=/var/log/task-1123-is1
   echo "task-1123-is1: clear" | sudo -n tee /dev/kmsg > /dev/null
   (sudo -n timeout 60 $D/zm/zpool clear smallmir > $L/clear.txt 2>&1; echo "rc=$?" >> $L/clear.txt) &
   ```
   Then, every 10 s for 60 s, read the pool state file in the background as above and `tail -n 1 $L/clear.txt`. Quote: whether `clear` returned and its rc, the state after 10 s and after 60 s (expected: `SUSPENDED` again, or the clear still blocked), the new kernel lines since the `clear` mark (`was suspended and is being resumed`, `has been suspended`), and `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|zpool'`.
3. **Capture** into `$L/wedge.txt`, about 3 minutes after the suspension so that the hung-task watchdog has spoken: the `ps` line above, `/proc/<tid>/stack` of `txg_sync`, of the `zpool clear` if it is still there, and of one `dd` (all in the background), the pool state file, `tail -n 300 /proc/spl/kstat/zfs/dbgmsg` (in the background, into `$L/dbgmsg-tail.txt`), and `sudo -n dmesg | tail -n 150`. Quote the `blocked for more than` reports (none if `hung_task_timeout_secs` is 0).
4. Make sure the logs are on disk: `sync -f $L/wedge.txt` (this syncs only the file system of `/var/log`, never ZFS).
5. **Reboot (pre-approved, this one only).** A graceful reboot can hang on the suspended pool's mount, so reboot at once with the magic SysRq key: `echo b | sudo -n tee /proc/sysrq-trigger`. If that does not take the VM down within 2 minutes, `sudo -n systemctl reboot -ff`. Wait for the VM, then check and quote: `uptime`, `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `mount | grep -c task-1123-is1` (must be 0), and `ls -l $L/wedge.txt` (must be there).

## Step 6: cleanup

- If no run said REPRODUCED: no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty), `mount | grep -c task-1123-is1` prints 0, then unload with `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is1` (`sudo -n rm -rf /var/tmp/task-1123-is1`). Anything else new: quote it and leave it.
- `/etc/zfs/zpool.cache`: quote `ls -l` of it; it must be as in step 0 (the pool used `cachefile=none`).
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is1`.

## Report

Write `/var/log/task-1123-is1/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, the three kernel sysctls and the zfs service states of step 0, `trees.txt` and the log line, the build rc, duration and warnings, the srcversion with "(DEBUG mode)", the four module parameters of step 3, the script's sha256.
2. **Table:** tag | sync | writers | seconds | suspended at (s) or no | zil commits/s (peak) | ZIL normal-class bytes per 5 s (peak) | `zil block allocation failure` lines | `trying ganging` lines | writer ENOSPC / EIO lines | ereports (data, io_failure) | verdict.
3. **Verdict line:** `#20004 on master a1daebfec3f: REPRODUCED in <run> after <n> s (control C not suspended)` or `#20004 on master a1daebfec3f: NOT REPRODUCED in <n> runs (C, A, ...)`, followed by one sentence of evidence: for REPRODUCED, how many bytes the ZIL allocated in the normal class during the last open txg against the pool's free space, the first failing allocation (ZIL or data, its size), the leaf error counts (expected all 0), and what `zpool clear` did; for NOT REPRODUCED, the peak ZIL allocation rate and the longest txg.
4. **Quotes:** per run, what step 4 lists (run C and the first A in full, then what differs), step 5's quotes if any, and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
