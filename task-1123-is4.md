TASK-1123-IS4 (from the coordinator session "main"): functional, about 50 minutes with its build, at most 1 h 30. **One reboot is pre-approved, for step 5 only, and only if a run says HANG**; nowhere else. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run and not on the VM of another task of this series. It tries to reproduce upstream issue openzfs/zfs#19765 ("Linux: posix_fadvise() deadlocks with `zfs rollback` / `zfs receive -F`") on upstream master `a1daebfec3f`, with the reporter's own workload. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is4/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The report (oh-forf, 7/11; reproduced on 2.4.4-pve1, kernel 7.0.14; "the code involved is unchanged on master"):** `posix_fadvise(POSIX_FADV_DONTNEED)` on a file with dirty pages, concurrent with `zfs rollback` (or `zfs receive -F`) of the mounted dataset, freezes the dataset for good; only a reboot helps. With the reporter's script the first rollback hangs.
- **The cycle (checked on master `a1daebfec3f` by main):** `zpl_fadvise()` (`module/os/linux/zfs/zpl_file.c`) holds `z_teardown_lock` as reader (`zpl_enter_verify_zp()`) across `generic_fadvise()`, which for DONTNEED starts writeback of the file and waits for each dirty page's lock; the flusher holds one of those page locks and waits in `zfs_putpage()` for the teardown lock as reader, behind the rollback's pending writer (`zfsvfs_teardown()` in `zfs_suspend_fs()`), which waits for the fadvise task to leave. No PR is linked (the reporter's #19764, closed, was about another inversion).
- **A second cycle may show on master:** `zfs_rezget()` now drops the page cache with `truncate_inode_pages()` while `zfs_resume_fs()` holds the teardown lock as writer (`8eb65b0ea56`, for #19013), and a flusher that holds a page lock and waits for the teardown lock would deadlock that too (that was #19764's subject). The stacks tell the two apart: the rollback in `zfsvfs_teardown()` (the fadvise cycle) or in `zfs_rezget()`/`truncate_inode_pages()` (the rezget cycle).
- **What this task shows:** whether master hangs with the reporter's workload on our VM, and which cycle the stacks show. NOT REPRODUCED in every run is a valid result.

## What is tested

The tree `zm` is upstream master, fetched from https://github.com/openzfs/zfs:

| tree | commit | tree hash |
|---|---|---|
| zm | a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 |

Each run (the script `is4-run.sh` below, the reporter's `zfadv.sh` with the tree's binaries, `cachefile=none` and bounded waits): pool `is4pool` on a 2 GiB sparse file under `$D`, dataset `ds` with a 64 MiB file `f` and a snapshot `@a`; a writer dirtying random pages of `f` through a shared mapping, N advisers looping `open`/`posix_fadvise(DONTNEED)`/`close`, a `sync -f` loop every 20 ms, and `zfs rollback is4pool/ds@a` in a loop for the given seconds. A rollback that has not returned after 20 s is a **HANG**: the script then saves `ps` of every thread, the stacks of every D-state thread and of the `flush-zfs` kworkers, the dbgmsg, kills the processes it started (those in D state stay), writes `HANG`, and leaves the pool as it is. Otherwise it writes `NO HANG`, destroys the pool and removes the file.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- Pools use file vdevs under `/var/tmp/task-1123-is4`. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko`;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool.
- **The build and every run run detached** (`setsid nohup ... &`) and are polled.
- **After a HANG** (a run's `verdict.txt` says HANG): the dataset is frozen, and the reporter saw anything that reads the mount table hang too. Until the reboot of step 5, run no `zpool`, `zfs` or `zdb` command, and none that reads mounts (`df`, `mount`, `findmnt`, `lsblk`, `systemctl status`, `ls` of anything under `$D/mnt-*`); read kernel files only in the background: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`. Start no further run.
- **Hangs of another kind.** A build still going after 60 minutes, or a run whose `steps.txt` has not changed for 5 minutes, is a hang: capture as in step 5 into `$L/hang.txt` and go to step 5.
- **Kernel log.** After each run, check the new kernel lines since that run's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. `blocked for more than` reports are expected only after a HANG. A `VERIFY` or `ASSERT` failure is a finding: quote it in full with its stack, then go to step 5.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 8 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `command -v ksh git python3 && python3 --version && sudo -n true && echo sudo-ok`;
- `ls -d /var/tmp/task-1123-is4 /var/log/task-1123-is4 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic`: quote them; do not change them. **If `hung_task_panic` is not 0, stop and report** (a HANG would panic the VM);
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service zfs-load-module.service 2>&1`: quote it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is4; L=/var/log/task-1123-is4; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
mkdir $D/zm && cd $D/zm && git init -q . &&
  git fetch -q --depth 1 $U a1daebfec3f5ca46ac261ef339efdf0dbeae997c && git checkout -q FETCH_HEAD &&
  echo "zm $(git rev-parse HEAD) $(git write-tree) $(git status --porcelain | wc -l)" | tee $L/trees.txt
git -C $D/zm log -1 --format='%an | %s'
sed -n '/^zpl_fadvise(/,/^}/p' $D/zm/module/os/linux/zfs/zpl_file.c | grep -E 'zpl_enter_verify_zp|generic_fadvise|zfs_exit'
grep -n 'truncate_inode_pages(ZTOI(zp)->i_mapping, 0)' $D/zm/module/os/linux/zfs/zfs_znode_os.c
```

Expected:

```
zm a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0
Rick Galliher | ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
```

then, from `zpl_fadvise()`, in this order: the `zpl_enter_verify_zp` line, a `zfs_exit` line (the WILLNEED early return), the `generic_fadvise` line and the final `zfs_exit` line, i.e. `generic_fadvise()` runs with the teardown lock held, and one `truncate_inode_pages` line in `zfs_rezget()` (line 1233). If the fetch fails, wait 1 minute and try once more; if fetching by commit is refused, use `git fetch -q --depth 200 $U master` and `git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c` instead. Any other difference is a stop: report the outputs as they are. Edit no file of the tree.

## Step 2: the debug build (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-is4; L=/var/log/task-1123-is4
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes. Then quote `grep -E 'HAVE_GENERIC_FADVISE|HAVE_WRITE_CACHE_PAGES' $D/zm/zfs_config.h`.

## Step 3: load the module and write the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)".

**Write the script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is4
cat > $D/is4-run.sh <<'IS4RUN_END'
#!/bin/bash
# is4-run.sh <tag> <advisers> <seconds>
# One attempt at openzfs/zfs#19765, after the reporter's zfadv.sh: on a
# dataset with a snapshot @a and a 64 MiB file f, a writer dirtying pages of
# f through a shared mapping, <advisers> loops of open/fadvise(DONTNEED)/
# close, a sync -f loop, and zfs rollback @a in a loop; a rollback that has
# not returned after 20 s is a HANG.  On a HANG nothing more is run against
# the pool (every command on it would block): stacks are saved and the
# script exits, leaving it as it is.  Runs as root.
set -u
D=/var/tmp/task-1123-is4; L=/var/log/task-1123-is4; Z=$D/zm
T=$1; NA=$2; DUR=$3
P=is4pool; IMG=$D/pool-$T.img; MP=$D/mnt-$T; O=$L/run-$T
ZPOOL=$Z/zpool; ZFS=$Z/zfs
mkdir -p "$O"
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }

log "run $T advisers=$NA seconds=$DUR"
truncate -s 2G "$IMG" || exit 1
timeout 120 "$ZPOOL" create -f -o cachefile=none -O mountpoint="$MP" "$P" "$IMG" ||
    { log "STOP: zpool create"; exit 1; }
timeout 60 "$ZFS" create "$P/ds" || exit 1
dd if=/dev/urandom of="$MP/ds/f" bs=1M count=64 status=none
sync -f "$MP/ds/f"
timeout 60 "$ZFS" snapshot "$P/ds@a" || exit 1
PIDS=""
python3 - "$MP/ds/f" <<'PY' > "$O/writer.log" 2>&1 &
import mmap, os, random, sys
fd = os.open(sys.argv[1], os.O_RDWR); m = mmap.mmap(fd, 0); n = len(m) // 4096
while True:
    for _ in range(256): m[random.randrange(n) * 4096] = random.randrange(256)
PY
PIDS="$PIDS $!"
for i in $(seq 1 "$NA"); do
	python3 - "$MP/ds/f" <<'PY' > "$O/adviser.$i.log" 2>&1 &
import os, sys
while True:
    fd = os.open(sys.argv[1], os.O_RDONLY)
    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    os.close(fd)
PY
	PIDS="$PIDS $!"
done
( while true; do sync -f "$MP/ds"; sleep 0.02; done ) &
SYNCLOOP=$!; PIDS="$PIDS $SYNCLOOP"
log "load started:$PIDS"

END=$((SECONDS + DUR)); n=0; RES=OK
while [ $SECONDS -lt $END ]; do
	"$ZFS" rollback "$P/ds@a" > "$O/rollback.last" 2>&1 & RB=$!
	t0=$SECONDS
	while kill -0 $RB 2>/dev/null; do
		sleep 0.1
		[ $((SECONDS - t0)) -ge 20 ] && { RES=HANG; break 2; }
	done
	n=$((n + 1))
done
log "rollbacks completed: $n in $DUR s -> $RES"

if [ $RES = HANG ]; then
	ps -eLo pid,tid,stat,etime,pcpu,comm,wchan:32 > "$O/ps-all.txt"
	awk 'NR == 1 || $3 ~ /^D/ || $6 ~ /flush/' "$O/ps-all.txt" > "$O/ps-hang.txt"
	for tid in $(awk 'NR > 1 && ($3 ~ /^D/ || $6 ~ /flush-zfs/) {print $2}' "$O/ps-hang.txt" | head -24); do
		{ echo "-- tid $tid $(cat /proc/$tid/comm 2>/dev/null) $(awk '{print $3}' /proc/$tid/stat 2>/dev/null)"
		  timeout 5 cat /proc/$tid/stack; } >> "$O/stacks.txt" 2>&1
	done
	cat /proc/spl/kstat/zfs/dbgmsg > "$O/dbgmsg.txt" 2>&1 &
	sleep 3
	# stop what still runs; tasks in D state stay where they are
	kill -9 $PIDS 2>/dev/null
	echo "RUN $T: rollback $((n + 1)) did not return in 20 s after $n completed => HANG" |
	    tee "$O/verdict.txt" | tee -a "$O/steps.txt"
	log "run $T end (pool left as it is)"
	exit 0
fi

kill -9 $PIDS 2>/dev/null
sleep 2
echo "RUN $T: $n rollbacks completed in $DUR s => NO HANG" | tee "$O/verdict.txt" | tee -a "$O/steps.txt"
( timeout 120 "$ZPOOL" destroy -f "$P" > "$O/destroy.txt" 2>&1; echo "rc=$?" >> "$O/destroy.txt" ) &
i=0
while [ $i -lt 120 ] && ! grep -q '^rc=' "$O/destroy.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
grep -q '^rc=0' "$O/destroy.txt" && rm -f "$IMG"
log "destroy: $(tail -n 1 "$O/destroy.txt" 2>/dev/null || echo STILL BLOCKED)"
log "run $T end"
exit 0
IS4RUN_END
chmod 755 $D/is4-run.sh
sha256sum $D/is4-run.sh | cut -c1-64; wc -l < $D/is4-run.sh; bash -n $D/is4-run.sh && echo syntax-ok
```

Expected: `d18e01ed9a666d1af1ae8b16631640ee1056c673e7cc0a13255ee1e0bd5171c7`, `86`, `syntax-ok`. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is4; L=/var/log/task-1123-is4; T=<tag>
echo "task-1123-is4: run $T start" | sudo -n tee /dev/kmsg > /dev/null
setsid nohup sudo -n bash $D/is4-run.sh $T <advisers> <seconds> > $L/run-$T.log 2>&1 < /dev/null &
```

| order | tag | arguments | when | expected duration |
|---|---|---|---|---|
| 1 | A | `A 4 60` | always (the report) | about 1 to 2 minutes |
| 2 | A2 | `A2 4 120` | only if A said NO HANG | about 2 to 3 minutes |
| 3 | A3 | `A3 8 120` | only if A2 said NO HANG | about 2 to 3 minutes |
| 4 | W | `W 0 120` | only if A3 said NO HANG (control: no adviser) | about 2 to 3 minutes |

Poll `tail -n 3 $L/run-$T/steps.txt` every 20 s until it ends with a `run $T end` line. **The first run that says HANG ends step 4: go to step 5.**

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `$L/run-$T/verdict.txt` (the number of rollbacks completed);
- `rollback.last` and the last line of each `adviser.*.log` and of `writer.log` (normally empty);
- `destroy.txt` for a NO HANG run;
- the kernel-line check since the run's mark.

**For a HANG run, also quote:** `ps-hang.txt` in full; from `stacks.txt`, the stacks of the `zfs` rollback, of every `python3` thread in D state (the advisers and the writer), of the `flush-zfs` kworker(s), and of any `sync` in D state, each in full; then name the cycle: **fadvise** (rollback in `zfsvfs_teardown`/`rrw_enter_write`, an adviser in `__folio_lock` under `generic_fadvise`/`zpl_fadvise`, the flusher in `rrw_enter_read` under `zfs_putpage`), **rezget** (rollback in `truncate_inode_pages` under `zfs_rezget`/`zfs_resume_fs`, the flusher in `rrw_enter_read` under `zfs_putpage`), or other (describe it).

## Step 5: only if a run said HANG (or a hang of another kind)

1. **Capture** into `$L/wedge.txt`, about 3 minutes after the HANG so that the hung-task watchdog has spoken, with no pool command and nothing that reads mounts: `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|flush|txg|zfs|python|sync'`; `/proc/<tid>/stack` of the rollback and of each D-state thread (in the background as in the VM rules); `tail -n 100 /proc/spl/kstat/zfs/dbgmsg` (in the background); `sudo -n dmesg | tail -n 200`. Quote the `blocked for more than` reports and their stacks (none if `hung_task_timeout_secs` is 0).
2. Make sure the logs are on disk: `sync -f $L/wedge.txt` (this syncs only the file system of `/var/log`, never ZFS).
3. **Reboot (pre-approved, this one only).** A graceful reboot would hang on the frozen mount, so reboot at once with the magic SysRq key: `echo b | sudo -n tee /proc/sysrq-trigger`. If the VM is not down within 2 minutes, `sudo -n systemctl reboot -ff`. Wait for the VM, then check and quote: `uptime`, `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), and `ls -l $L/wedge.txt $L/run-*/stacks.txt` (must be there). No further run.

## Step 6: cleanup

- If no reboot happened: no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty); unload with `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is4` (`sudo -n rm -rf /var/tmp/task-1123-is4`). Anything else new: quote it and leave it.
- `/etc/zfs/zpool.cache`: quote `ls -l` of it; it must be as in step 0 (the pool used `cachefile=none`).
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is4`.

## Report

Write `/var/log/task-1123-is4/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, python version, the three kernel sysctls and the zfs service states of step 0, `trees.txt`, the log line and the code lines of step 1, the build rc, duration and warnings, the `zfs_config.h` lines, the srcversion with "(DEBUG mode)", the script's sha256.
2. **Table:** tag | advisers | seconds | rollbacks completed | verdict | cycle (for a HANG).
3. **Verdict line:** `#19765 on master a1daebfec3f: HANG in run <tag> after <n> rollbacks, <fadvise|rezget|other> cycle; reboot done` or `#19765 on master a1daebfec3f: NOT REPRODUCED in <n> runs (<total> rollbacks)`, followed by one sentence naming the three tasks of the cycle and where each waits.
4. **Quotes:** per run, what step 4 lists, step 5's quotes if any, and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
