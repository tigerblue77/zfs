TASK-1123-IS3 (from the coordinator session "main"): functional, about 40 minutes with its build, at most 1 h 15, no reboot expected and none approved. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run and not on the VM running TASK-1123-IS2. It tries to reproduce upstream issue openzfs/zfs#19915 ("Linux: fsync(2) returns a positive ENOSPC on a full pool, since zpl_putpage() doesn't negate zfs_putpage()'s errno") on upstream master `a1daebfec3f`: on a full pool, and on a dataset at its `refquota`, against a control with free space. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is3/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The report (prakashsurya, 16/11; master c913da86, kernels 6.14, 6.17, 7.0):** on a full pool, `fsync(2)` on a file with dirty mmap'd pages returns `28` (a positive ENOSPC) with errno 0, instead of `-1` with errno ENOSPC. A caller that checks `fsync(fd) < 0` believes its data is on disk. On 6.17 the kernel also warns in `writeback_iter()` (`WARN_ON_ONCE(*error > 0)`). With free space, the same program gets 0.
- **Mechanism (read on master `a1daebfec3f` by main):** `zpl_putpage()` (`module/os/linux/zfs/zpl_file.c:597-611`) returns `zfs_putpage()`'s positive errno as is; `zfs_putpage()` returns it from `dmu_tx_assign()` when `for_sync` is set (`zfs_vnops_os.c:4007`); `zpl_fsync()`'s early exit after `zpl_write_cache_pages()` hands it to `vfs_fsync_range()` and on to userspace. `msync(MS_SYNC)` goes through the same `zpl_fsync()`. The reporter's fix: negate at the call site and assert `ret <= 0`. #19737 (myers) saw the same with `EDQUOT`: `fsync()` returned 122.
- **No PR is linked.** This task gives a baseline on our VM for a fix and its ZTS test.

## What is tested

The tree `zm` is upstream master, fetched from https://github.com/openzfs/zfs:

| tree | commit | tree hash |
|---|---|---|
| zm | a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 |

The probe `fsync_sign` (C, below; the reporter's program plus a second `fsync` and an `msync(MS_SYNC)`) stores into 256 pages of a `MAP_SHARED` mapping of a file, then calls `fsync` twice and `msync(MS_SYNC)` once through the raw syscall, and prints each return value and errno. Each run (the script `is3-run.sh` below) creates the pool `is3pool` (512 MiB file, ashift 12, `cachefile=none`, `compression=off`, `recordsize=4k`, the reporter's settings), dataset `fs`, writes an 8 MiB file `victim`, then:
- `free`: nothing more (control);
- `full`: fills the pool with two `dd` until ENOSPC (the reporter's steps);
- `quota`: sets `refquota` of `fs` to its current `referenced`;

then runs the probe (given 60 s, never waited for), records `Dirty`/`Writeback` and the flush threads 10 s later, removes the files and destroys the pool (each bounded).

**REPRODUCED** for a run means a probe line `returned <n>` with n above 0. Expected: `full` returns 28 (ENOSPC), `quota` returns 122 (EDQUOT), `free` returns 0.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- Pools use file vdevs under `/var/tmp/task-1123-is3`. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko`;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool.
- **The build and every run run detached** (`setsid nohup ... &`) and are polled.
- **Hangs.** A build still going after 60 minutes, or a run whose `steps.txt` has not changed for 8 minutes, is a hang: capture `ps -eLo pid,tid,stat,etime,pcpu,comm,wchan:32 | grep -E ' D|flush|txg|zpool|zfs|fsync_sign|dd '`, `/proc/<tid>/stack` of each D-state thread and of the busiest flush thread (read in the background: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`), and `sudo -n dmesg | tail -n 100` into `$L/hang.txt`, then stop and report. Do not reboot.
- **Kernel log.** After each run, check the new kernel lines since that run's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. A `WARNING` in `writeback_iter` during a `full` or `quota` probe is possible (the kernel objecting to the positive errno, as on 6.17): quote it in full with its call trace. A `VERIFY` or `ASSERT` failure is a finding: quote it in full, capture as for a hang, and stop.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 8 GiB free, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `command -v ksh git gcc && sudo -n true && echo sudo-ok`;
- `ls -d /var/tmp/task-1123-is3 /var/log/task-1123-is3 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is3; L=/var/log/task-1123-is3; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
mkdir $D/zm && cd $D/zm && git init -q . &&
  git fetch -q --depth 1 $U a1daebfec3f5ca46ac261ef339efdf0dbeae997c && git checkout -q FETCH_HEAD &&
  echo "zm $(git rev-parse HEAD) $(git write-tree) $(git status --porcelain | wc -l)" | tee $L/trees.txt
git -C $D/zm log -1 --format='%an | %s'
sed -n '597,611p' $D/zm/module/os/linux/zfs/zpl_file.c
```

Expected:

```
zm a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0
Rick Galliher | ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
```

then `zpl_putpage()` ending with `ret = zfs_putpage(pp->mapping->host, pp, wbc, *for_sync);` ... `return (ret);` (no minus sign). If the fetch fails, wait 1 minute and try once more; if fetching by commit is refused, use `git fetch -q --depth 200 $U master` and `git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c` instead. Any other difference is a stop: report the outputs as they are. Edit no file of the tree.

## Step 2: the debug build (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-is3; L=/var/log/task-1123-is3
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes. Then quote `grep -E 'HAVE_WRITE_CACHE_PAGES|HAVE_WRITEPAGE_T_FOLIO|HAVE_VFS_WRITEPAGE' $D/zm/zfs_config.h`.

## Step 3: load the module, write the probe and the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)".

**Write the probe and the script** exactly as below, then check them:

```sh
D=/var/tmp/task-1123-is3
cat > $D/fsync_sign.c <<'IS3C_END'
/*
 * fsync_sign <file>: openzfs/zfs#19915.  Stores into 256 pages of a
 * MAP_SHARED mapping of <file>, then calls fsync(2) twice and msync(2)
 * with MS_SYNC once through the raw syscall, so that a positive return is
 * printed as it is.  Expected: 0, or -1 with errno set.
 */
#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	size_t len = 4096UL * 256;
	long raw;
	int fd;
	char *p;

	if (argc != 2)
		return (2);
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return (1);
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return (1);
	}
	for (size_t i = 0; i < len; i += 4096)
		p[i] = (char)(i / 4096 + 1);
	for (int n = 0; n < 2; n++) {
		errno = 0;
		raw = syscall(SYS_fsync, fd);
		printf("fsync %d: returned %ld, errno=%d (%s)\n", n, raw,
		    errno, strerror(errno));
	}
	errno = 0;
	raw = syscall(SYS_msync, p, len, MS_SYNC);
	printf("msync: returned %ld, errno=%d (%s)\n", raw, errno,
	    strerror(errno));
	return (0);
}
IS3C_END
cat > $D/is3-run.sh <<'IS3RUN_END'
#!/bin/bash
# is3-run.sh <tag> <full|quota|free>
# One attempt at openzfs/zfs#19915: store into 256 mapped pages of a file
# whose new blocks cannot be allocated (pool full, or dataset at its
# refquota), then raw fsync(2) twice and msync(MS_SYNC) once.  "free" is the
# control with space left.  Every pool command is bounded.  Runs as root.
set -u
D=/var/tmp/task-1123-is3; L=/var/log/task-1123-is3; Z=$D/zm
T=$1; MODE=$2
P=is3pool; IMG=$D/pool-$T.img; M=$D/mnt-$T; O=$L/run-$T
ZPOOL=$Z/zpool; ZFS=$Z/zfs
mkdir -p "$O"
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }
bg() {	# never waited for: output read after at most $2 seconds
	local out=$1 secs=$2 i=0; shift 2
	( timeout "$secs" "$@" > "$out" 2>&1; echo "rc=$?" >> "$out" ) &
	while [ $i -lt "$secs" ] && ! grep -q '^rc=' "$out" 2>/dev/null; do
		sleep 1; i=$((i + 1))
	done
	grep -q '^rc=' "$out" 2>/dev/null || echo "STILL BLOCKED after ${secs}s" >> "$out"
}

log "run $T mode=$MODE"
truncate -s 512M "$IMG"
timeout 120 "$ZPOOL" create -f -o ashift=12 -o cachefile=none \
    -O compression=off -O recordsize=4k -O mountpoint="$M" "$P" "$IMG" ||
    { log "STOP: zpool create"; exit 1; }
timeout 60 "$ZFS" create "$P/fs" || exit 1
timeout 60 dd if=/dev/urandom of="$M/fs/victim" bs=1M count=8 status=none
sync -f "$M/fs/victim"
case $MODE in
full)
	timeout 300 dd if=/dev/urandom of="$M/fs/fill" bs=1M status=none
	sync -f "$M/fs/victim"
	timeout 300 dd if=/dev/urandom of="$M/fs/fill2" bs=128k status=none
	sync -f "$M/fs/victim"
	;;
quota)
	used=$(timeout 30 "$ZFS" get -Hp -o value referenced "$P/fs")
	timeout 60 "$ZFS" set refquota="$used" "$P/fs" || log "set refquota rc=$?"
	;;
esac
timeout 30 "$ZFS" list -p -o name,used,avail,refer,refquota "$P" "$P/fs" > "$O/list.txt" 2>&1
log "prepared: $(tail -n 1 "$O/list.txt")"
echo "task-1123-is3: run $T probe" > /dev/kmsg
bg "$O/probe.txt" 60 "$D/fsync_sign" "$M/fs/victim"
log "probe: $(tr '\n' ';' < "$O/probe.txt")"
awk '/^Dirty:|^Writeback:/' /proc/meminfo > "$O/meminfo.txt"
sleep 10
ps -eLo pid,tid,stat,pcpu,comm | awk 'NR == 1 || /flush/' > "$O/ps-flush.txt"
# cleanup: dropping the file discards its dirty pages
bg "$O/rm.txt" 60 rm -f "$M/fs/victim" "$M/fs/fill" "$M/fs/fill2"
bg "$O/destroy.txt" 120 "$ZPOOL" destroy "$P"
grep -q '^rc=0' "$O/destroy.txt" && rm -f "$IMG"
log "rm: $(tail -n 1 "$O/rm.txt"); destroy: $(tail -n 1 "$O/destroy.txt")"
log "run $T end"
exit 0
IS3RUN_END
chmod 755 $D/is3-run.sh
for f in fsync_sign.c is3-run.sh; do echo "$f $(sha256sum < $D/$f | cut -c1-64) $(wc -l < $D/$f)"; done
bash -n $D/is3-run.sh && echo syntax-ok
gcc -Wall -Wextra -O2 -o $D/fsync_sign $D/fsync_sign.c && echo cc-ok
```

Expected:

```
fsync_sign.c 1ecd39050efc2716a14d894fa6f047fc1beae82f64d40338fd5c41efee9e2eb9 49
is3-run.sh 98453eddd84deca4fe6e015d9eb011ff551f632146ef0fb00146908cd5666017 57
syntax-ok
cc-ok
```

with no compiler warning. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is3; L=/var/log/task-1123-is3; T=<tag>
echo "task-1123-is3: run $T start" | sudo -n tee /dev/kmsg > /dev/null
setsid nohup sudo -n bash $D/is3-run.sh $T <mode> > $L/run-$T.log 2>&1 < /dev/null &
```

| order | tag | mode | expected probe lines | expected duration |
|---|---|---|---|---|
| 1 | F | `free` | `fsync 0: returned 0`, `fsync 1: returned 0`, `msync: returned 0` | about 1 minute |
| 2 | E | `full` | `returned 28, errno=0` for both `fsync` lines (the bug); `msync` as it comes | about 2 to 3 minutes |
| 3 | Q | `quota` | `returned 122, errno=0` for both `fsync` lines (the bug); `msync` as it comes | about 1 to 2 minutes |
| 4 | E2 | `full` | as E | about 2 to 3 minutes |

Poll `tail -n 3 $L/run-$T/steps.txt` every 30 s until it ends with `run $T end`.

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `probe.txt` in full;
- `list.txt` (used, avail, refer, refquota of the pool and `fs`);
- `meminfo.txt` and `ps-flush.txt` (whether a flush thread spins on the pages the failed `fsync` re-dirtied: its %CPU 10 s after the probe);
- `rm.txt`, `destroy.txt`;
- the kernel-line check since the run's mark (and, if a `WARNING` appears, whether it falls after the run's `probe` kmsg line).

## Step 5: cleanup

- no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty); unload with `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty;
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is3` (`sudo -n rm -rf /var/tmp/task-1123-is3`). Anything else new: quote it and leave it;
- `ls -l /etc/zfs/zpool.cache 2>&1`: as in step 0;
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is3`.

## Report

Write `/var/log/task-1123-is3/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, `trees.txt`, the log line and the `zpl_putpage()` lines, the build rc, duration and warnings, the `zfs_config.h` writeback lines, the srcversion with "(DEBUG mode)", the two sha256 sums.
2. **Table:** tag | mode | avail of `fs` before the probe | fsync 0 | fsync 1 | msync | flush thread %CPU 10 s later | kernel WARNING (yes/no, where) | verdict.
3. **Verdict line:** `#19915 on master a1daebfec3f: REPRODUCED (full: fsync returned 28 errno 0; quota: 122 errno 0; free: 0)` or what was seen, followed by one sentence on `msync` and on the kernel warning.
4. **Quotes:** per run, what step 4 lists (runs F and E in full, then what differs), and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
