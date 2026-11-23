TASK-1123-IS5 (from the coordinator session "main"): functional, about 45 to 50 minutes with its build, at most 1 h 15. **A reboot is pre-approved after each run whose verdict is PANIC (step 5), and only there**; two are expected, at most four in this task. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12.118+deb13-cloud-amd64), **alone on that VM**: it loads `scsi_debug` and `null_blk`, and the expected result is a ZFS kernel thread parked by a failed assertion, then a SysRq reboot. It tries to reproduce upstream issue openzfs/zfs#20007 ("Resilver restarted after failed repair writes during zpool replace trips VERIFY in vdev_dtl_should_excise()") on upstream master `a1daebfec3f` with the reporter's own reproducer, then runs the same scenario against the fix proposed in open PR #19906 and, with EIO write errors, against the reporter's neighbouring PR #20003. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is5/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `T`, `A`, `C`).

## Why

- **The report (rooseveltlai, 23/11; Debian 13, kernel 6.12.118+deb13-cloud-amd64, which is our VMs' kernel; master `a1daebfec`, `--enable-debug`):** when some repair writes to the new disk of a `zpool replace` fail with an error that ZFS records in the DTL, the resilver restarts for just the birth txgs of those blocks; when that restarted resilver finishes, a debug build fails `VERIFY3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg) failed (65 <= 64)`, `PANIC at vdev.c:3404:vdev_dtl_should_excise()`, and `txg_sync` hangs. Release builds compile the assertion out and behave correctly. The reporter's script fires within about 15 s, "every time so far".
- **The path (read on master by main):**
  - `spa_vdev_attach()` sets the new disk's `vdev_resilver_txg` to the attach txg R (`spa.c:8920`) and its DTL to `[4, R+3)`, so the first resilver covers R.
  - A repair write of the scan thread that fails with any error but an EIO not marked for retry (that filter is `vdev.c:5502`) is recorded at the block's birth txg in `DTL_SCRUB` and `DTL_MISSING` (`vdev_stat_update()`, `vdev.c:5547`).
  - At the end of the first resilver, `vdev_dtl_reassess()` (called by `dsl_scan_done()`, `dsl_scan.c:1286`) leaves only those birth txgs, all older than R, in `DTL_MISSING`, and `dsl_scan_done()` starts a new resilver ("starting deferred resilver", `dsl_scan.c:1340`) over those txgs only (from the oldest minus 1 to the newest plus 1), which ends before R unless one of the blocks was born in R - 1.
  - When it completes, `vdev_dtl_should_excise()` asserts R <= `scn_max_txg` (`vdev.c:3404`). In a debug build that is `spl_panic()`: with `spl_panic_halt=0` (the default, `spl-err.c:36`) it prints the message and the stack, then parks `txg_sync` in an uninterruptible sleep for good, holding the pool's config lock and the disk's DTL lock: the pool is frozen until a reboot.
  - If an ordinary write of the current txg also fails on that disk, the DTL gets a txg >= R, the restarted resilver covers R and the assertion holds. The trigger needs failed repair writes only.
- **Why `scsi_debug` and `null_blk`, and that our kernel has them (checked by main):** the reporter fails the next 4 WRITE commands of the new disk with sense MEDIUM ERROR / UNRECOVERED READ ERROR (3/11/00), which the SCSI layer returns at once as `BLK_STS_MEDIUM`, i.e. ENODATA (61), an error ZFS records; the disk serves one command at a time, so those 4 commands are, in practice, resilver writes. ZFS's own injector (`zinject -d <disk> -e io -T write`, whose EIO is marked as retried and so recorded) cannot single out repair writes on this layout: it has no count or priority filter, and every txg's metadata writes go to the same disk (#19906's test gets around that with a special vdev that takes the metadata). dm-flakey, dm-error and null_blk bad blocks return EIO, which master drops at `vdev.c:5502`. Debian's `linux-image-6.12.118+deb13-cloud-amd64` package (debian-security, 21/11) ships `kernel/drivers/scsi/scsi_debug.ko.xz`, `kernel/drivers/block/null_blk/null_blk.ko.xz` and `kernel/fs/configfs/configfs.ko.xz`, built for this kernel, with `CONFIG_DEBUG_FS=y` and `CONFIG_DEBUG_FS_ALLOW_ALL=y` (the scsi_debug `error` file lives in debugfs). Step 0 checks the VM's own copy.
- **Two open PRs touch this:**
  - **#19906** (ahrens, 16/11, "Don't assert that a resilver covered the attach txg", head `3c465f0a721`, not linked from the issue): removes that assertion and its sibling. Its only module change is `module/zfs/vdev.c`, whose parent blob is master's: arm F is master with that file.
  - **#20003** (the reporter, 22/11, "Resilver doesn't repair a write that failed on one disk of a mirror", head `b4578f4312d`): lets a leaf's EIO reach the DTL (`vdev.c:5502` gains `&& !vd->vdev_ops->vdev_op_leaf`). On master a repair write that fails with EIO is forgotten: the replace completes with the new disk missing those blocks (the author's own comment on #20003). With #20003 it is recorded, so an ordinary SCSI write error (sense 3/0C, which reaches ZFS as EIO because ZFS sends these writes failfast) reaches the assertion too. Arm P is master with #20003's `vdev.c`.
- **What this task shows:** whether the reporter's reproducer panics master on our VM (N of M runs) and with which txgs; whether #19906's change lets the same scenario finish (the replace completes after the restarted resilver and a scrub finds nothing missing); and, with EIO write errors, master completing the replace with blocks missing against #20003 panicking. NOT REPRODUCED is a valid result.

## What is tested

Fetched from https://github.com/openzfs/zfs (heads checked by main with `git ls-remote` at 12:22Z: master `a1daebfec3f`, `refs/pull/19906/head` `3c465f0a721`, `refs/pull/20003/head` `b4578f4312d`). Both PRs change only `module/zfs/vdev.c` in the module; each arm takes only that file, so that it differs from master by the change alone (the PRs' tests are not run). `HEAD` stays at master in every arm.

| arm | tree | tree hash | `module/zfs/vdev.c` blob |
|---|---|---|---|
| zm | master a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 | 25ed7a4a7419aceee5f28cfb4fbd49b4ebc86216 |
| F | zm + `vdev.c` of #19906 3c465f0a721582511d34d1148718d9eb62047478 | daed71d781db6d55bfd6e1a8e9b92d65c00cb6b2 | a76b19ccd18e4b38372258c78d3c477bf449e179 |
| P | zm + `vdev.c` of #20003 b4578f4312d3bba362d1afbd31753a78225ab8d3 | 8f6fbff2e60d1b5311416f30d8999f240ad6fddb | ecade036a6901b2df2c48e1c3f68205d56e75f58 |

Each run (the script `is5-run.sh` below, the reporter's script with the tree's binaries, `cachefile=none`, the pool mounted under `$D`, and bounded waits): two memory-backed null_blk disks `is5a`, `is5b` (256 MiB, 4 KiB blocks), a scsi_debug disk (320 MiB, 4 KiB sectors, `max_queue=1 delay=1`); pool `is5pool` = mirror of `is5a` and `is5b` (`ashift=12`, `compression=off`), a 96 MiB file written with `conv=fsync`; `zpool replace is5pool /dev/is5b /dev/<scsi_debug disk>`; one second later the next 4 WRITE(10) and 4 WRITE(16) commands of that disk fail with CHECK CONDITION, sense key MEDIUM ERROR and asc 0x11 (mode `enodata`, as reported) or 0x0c (mode `eio`); then up to 120 s for a `PANIC` in the kernel log or for the replace to finish (`detach vdev=/dev/is5b` in the dbgmsg).
- **PANIC:** the script saves the kernel log since its mark, the threads in D state and their stacks, the dbgmsg and the pool's last txgs, writes `verdict.txt` and exits, leaving the pool, the devices and the modules as they are.
- **Otherwise:** it saves `zpool status -vpP`, runs `zpool scrub -w` (which finds any block the new disk is missing) and `zpool status` again, writes `verdict.txt`, destroys the pool and removes the devices and their modules.

`verdict.txt` also gives the failed I/Os (from the `zio pool=... flags=` kernel lines: repair writes carry `ZIO_FLAG_IO_REPAIR`, ordinary writes do not) and the resilvers from the dbgmsg: the attach txg R (the resilver restart txg minus 3) and each resilver's range; "a restarted resilver ends before the attach txg: yes" is the trigger.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package. Change no sysctl and no module parameter.
- **Alone on the VM:** no other task, build or test runs on it during this task.
- **Devices.** The only block devices this task uses are the three its script creates: the null_blk disks `/dev/is5a` and `/dev/is5b` (made through configfs) and one scsi_debug disk, which the script finds in sysfs and checks (model `scsi_debug`, 655360 sectors, no partition, not mounted) before passing it to `zpool replace`. Never pass any other `/dev` path to a command. Never unload `sd_mod`, `scsi_mod`, `scsi_common` or `configfs`: the VM's own disk may need them. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko` and the srcversion recorded for the current arm;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool;
  - `lsmod | grep -E '^(scsi_debug|null_blk) '` is empty;
  - `cat /sys/module/spl/parameters/spl_panic_halt` is `0`.
- **The builds and every run run detached** (`setsid nohup ... &`) and are polled.
- **After a PANIC** (a run's `verdict.txt` says PANIC, or the kernel lines since the run's mark show `PANIC at` or `VERIFY`, whatever the script says): `txg_sync` is parked for good. Until the reboot of step 5, run no `zpool`, `zfs` or `zdb` command, nothing that reads mounts (`df`, `mount`, `findmnt`, `lsblk`, `systemctl status`, `ls` of anything under `$D/mnt-*`), unload no module, and read kernel files only in the background: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`. Go to step 5.
- **Hangs of another kind** (no PANIC): the build of step 2 still going after 60 minutes or an arm's rebuild after 20, a run whose `steps.txt` has not changed for 5 minutes, or a run with no `run <tag> end` line after 8 minutes. Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zpool|udev|modprobe'`, the `/proc/<tid>/stack` of each D-state thread (in the background as above), `sudo -n dmesg | tail -n 150`. Then stop: **no reboot** (not pre-approved), write the report.
- **Kernel log.** After each run, check the kernel lines since that run's mark (`sudo -n dmesg | sed -n '/task-1123-is5: run <tag> start/,$p'`; the script writes the mark) for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. In a PANIC run the `VERIFY3U` line, the `PANIC at` line and their stack are the expected finding; anything else is a finding: quote it in full. The SCSI error lines (`Sense Key`, `critical medium error`, `I/O error, dev`) and the `zio pool=is5pool` lines are expected in every run: count them.
- **Time.** Count from step 0. An optional run or arm starts only within its limit (step 4's table). At 1 h 10, start nothing new: go to step 7.

## Step 0: state

Check and quote:
- `date -u`, `uname -r` (expected `6.12.118+deb13-cloud-amd64`; with another kernel, carry on only if the module checks below pass, and say so), `nproc`, `free -m` (at least 2048 MB available), `df -h /var/tmp` (at least 8 GiB free, else stop and report);
- `lsmod | grep -E '^(zfs|spl|scsi_debug|null_blk) '`: must be empty. If a pool is imported, stop and report;
- `lsmod | grep -E '^(configfs|sd_mod|scsi_mod|scsi_common) '` and `mountpoint /sys/kernel/config 2>&1`: quote them (compared in step 7);
- `ls -l /dev/zfs /dev/is5a /dev/is5b 2>&1`: none may exist;
- `command -v git python3 udevadm && sudo -n true && echo sudo-ok`;
- `ls -d /lib/modules/$(uname -r)/build`: must exist (kernel headers for the build); if not, stop and report;
- `modinfo -n scsi_debug null_blk configfs`: three paths of this kernel ending in `kernel/drivers/scsi/scsi_debug.ko.xz`, `kernel/drivers/block/null_blk/null_blk.ko.xz` and `kernel/fs/configfs/configfs.ko.xz`; `modinfo -F vermagic scsi_debug null_blk`: both start with `6.12.118+deb13-cloud-amd64 SMP preempt mod_unload modversions`; `modinfo -F parm scsi_debug | grep -cE '^(dev_size_mb|sector_size|max_queue|delay):'` prints `4`. A missing module is a stop: report it (main has the fallback);
- `cat /sys/kernel/security/lockdown`: must show `[none]` (under lockdown the scsi_debug `error` file cannot be opened for writing, and an unsigned zfs module would not load); else stop and report;
- `mountpoint /sys/kernel/debug`: must be a mountpoint;
- `lsblk -dn -o NAME,SIZE,TYPE,TRAN,MODEL`: quote (the VM's own disks);
- `pgrep -a zed; systemctl is-active zfs-zed.service 2>&1`: **no `zed` process may run** (it could fault the disk over its I/O errors or start a scrub after the resilver); else stop and report;
- `ls -d /var/tmp/task-1123-is5 /var/log/task-1123-is5 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic /proc/sys/kernel/panic_on_oops /proc/sys/kernel/panic_on_warn /proc/sys/kernel/sysrq`: quote them; do not change them. **If `hung_task_panic` or `panic_on_warn` is not 0, stop and report**: the parked `txg_sync` is reported as hung after `hung_task_timeout_secs`, and a kernel panic with `panic` at 0 would leave the VM down until a reset from outside;
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service zfs-load-module.service zfs-zed.service 2>&1`: quote it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is5; L=/var/log/task-1123-is5; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
mkdir $D/zm && cd $D/zm && git init -q . &&
  git fetch -q --depth 1 $U a1daebfec3f5ca46ac261ef339efdf0dbeae997c && git checkout -q FETCH_HEAD &&
  echo "zm $(git rev-parse HEAD) $(git write-tree) $(git status --porcelain | wc -l)" | tee $L/trees.txt
git -C $D/zm log -1 --format='%an | %s'
cd $D/zm && git hash-object module/zfs/vdev.c &&
  grep -n 'ASSERT3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg);' module/zfs/vdev.c &&
  grep -n 'if (zio->io_error == EIO &&$' module/zfs/vdev.c &&
  grep -n '"starting deferred resilver"' module/zfs/dsl_scan.c &&
  grep -n 'newvd->vdev_resilver_txg = txg;' module/zfs/spa.c &&
  grep -n 'static unsigned int spl_panic_halt;' module/os/linux/spl/spl-err.c
```

Expected:

```
zm a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0
Rick Galliher | ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
25ed7a4a7419aceee5f28cfb4fbd49b4ebc86216
3404:			ASSERT3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg);
5502:	if (zio->io_error == EIO &&
1340:			    "starting deferred resilver", tx, "errors=%llu",
8920:			newvd->vdev_resilver_txg = txg;
36:static unsigned int spl_panic_halt;
```

Then fetch the two PR commits now (nothing is applied yet):

```sh
D=/var/tmp/task-1123-is5; U=https://github.com/openzfs/zfs
cd $D/zm && for c in 3c465f0a721582511d34d1148718d9eb62047478 b4578f4312d3bba362d1afbd31753a78225ab8d3; do
  git fetch -q --depth 1 $U $c && echo "fetched $c $(git rev-parse $c^{tree}) $(git rev-parse $c:module/zfs/vdev.c)"; done
git -C $D/zm rev-parse HEAD; git -C $D/zm status --porcelain | wc -l
```

Expected:

```
fetched 3c465f0a721582511d34d1148718d9eb62047478 8299524723a189e046f05267e67851132efeb712 a76b19ccd18e4b38372258c78d3c477bf449e179
fetched b4578f4312d3bba362d1afbd31753a78225ab8d3 ed36965a3b709c347374d90657244298c909078d ecade036a6901b2df2c48e1c3f68205d56e75f58
a1daebfec3f5ca46ac261ef339efdf0dbeae997c
0
```

If a fetch fails, wait 1 minute and try once more; if fetching by commit is refused, use `git fetch -q --depth 200 $U master` and `git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c` for master, and `git fetch -q --depth 1 $U refs/pull/19906/head` (resp. `refs/pull/20003/head`) for a PR, then quote `git rev-parse FETCH_HEAD`: if it is not the commit above, the PR has moved on: skip that arm (steps 4 and 6) and report the new head. Any other difference is a stop: report the outputs as they are. Edit no file of the tree; step 6 changes `module/zfs/vdev.c` only, with `git checkout`.

## Step 2: the debug build (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-is5; L=/var/log/task-1123-is5
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes.

## Step 3: load the module and write the script

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`; save it: `cat /sys/module/zfs/srcversion > $L/srcversion-zm.txt`;
- quote the last `ZFS: Loaded module` line of `sudo -n dmesg`, which must say "(DEBUG mode)";
- quote `cat /sys/module/spl/parameters/spl_panic_halt`: **must be `0`** (the failed assertion then parks one thread; with 1 it would panic the kernel and, `panic` being 0, leave the VM down): else stop and report;
- quote `cat /sys/module/zfs/parameters/zfs_vdev_failfast_mask` (expected `1`: mode `eio` relies on it).

**Write the script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is5
cat > $D/is5-run.sh <<'IS5RUN_END'
#!/bin/bash
# is5-run.sh <tag> <enodata|eio>
# One attempt at openzfs/zfs#20007 with the reporter's reproducer, against
# the zfs module loaded from $D/zm: a mirror of two memory-backed null_blk
# disks holding a 96 MiB file, zpool replace of the second by a scsi_debug
# disk that serves one command at a time, and one second later the next
# 4 WRITE(10) and 4 WRITE(16) commands of that disk failed with CHECK
# CONDITION, sense key MEDIUM ERROR and
#   enodata: asc 0x11, as reported (the block layer returns ENODATA)
#   eio:     asc 0x0c, write error (EIO: ZFS sends these writes failfast)
# then up to 120 s for a PANIC or for the replace to finish.  After a PANIC
# nothing touches the pool again: the kernel log, stacks and dbgmsg are
# saved and the script exits, leaving everything as it is.  Otherwise the
# pool is scrubbed and destroyed, and the devices removed.  Runs as root.
# shellcheck disable=SC2317  # teardown runs from the EXIT trap
set -u
D=/var/tmp/task-1123-is5; L=/var/log/task-1123-is5; Z=$D/zm
T=$1; MODE=$2
P=is5pool; MP=$D/mnt-$T; O=$L/run-$T; CFG=/sys/kernel/config/nullb
ZPOOL=$Z/zpool; DBG=/proc/spl/kstat/zfs/dbgmsg
case $MODE in
enodata) ASC=11 ;;
eio) ASC=c ;;
*) echo "usage: $0 <tag> <enodata|eio>" >&2; exit 2 ;;
esac
mkdir -p "$O" || exit 1
log() { echo "$(date -u +%T) $*" | tee -a "$O/steps.txt"; }
klog() { dmesg | sed -n "/task-1123-is5: run $T start/,\$p"; }
# bg <out> <secs> <cmd...>: a command that may block is never waited for
bg() {
	local out=$1 secs=$2 i=0; shift 2
	( timeout "$secs" "$@" > "$out" 2>&1; echo "rc=$?" >> "$out" ) &
	while [ $i -lt "$secs" ] && ! grep -q '^rc=' "$out" 2>/dev/null; do
		sleep 1; i=$((i + 1))
	done
	grep -q '^rc=' "$out" || echo "STILL BLOCKED after ${secs}s" >> "$out"
	grep -q '^rc=0$' "$out"
}
# failed I/Os as vdev_disk_error() prints them: IO_REPAIR is flag bit 1,
# PROBE bit 17
zio_summary() {
	klog | grep -o 'zio pool=.*' > "$O/zio-errors.txt"
	awk '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
	    f = v["flags"] + 0
	    k = (int(f / 131072) % 2) ? "probe" : (int(f / 2) % 2) ? "repair" : "ordinary"
	    n++; c[k]++; e[v["error"] "/type" v["type"]]++ }
	    END { printf "%d failed I/Os: repair %d, ordinary %d, probe %d (errno/type:",
	        n, c["repair"], c["ordinary"], c["probe"]
	        for (x in e) printf " %s x%d", x, e[x]; print ")" }' "$O/zio-errors.txt"
}
# the resilvers from dbgmsg: the attach restarts the resilver at
# txg + TXG_CONCURRENT_STATES (3); each scan covers (mintxg, maxtxg]
scan_summary() {
	grep -E 'restarting resilver|restarting scan|scan setup|scan done|deferred resilver|guid:|DTL_MISSING|vdev attach|detach vdev=' \
	    "$O/dbgmsg.txt" > "$O/dbgmsg-scan.txt"
	awk '/restarting resilver for/ && !r { split($0, a, "txg="); r = a[2] - 3 }
	    /scan setup/ { s++; split($0, a, "mintxg="); split(a[2], b, " maxtxg=")
	        mn[s] = b[1]; mx[s] = b[2] + 0 }
	    END { printf "attach txg %s; %d resilver(s):", (r ? r : "?"), s
	        t = "no"
	        for (i = 1; i <= s; i++) {
	            printf " (%s, %d]", mn[i], mx[i]
	            if (i > 1 && r && mx[i] < r) t = "yes"
	        }
	        print "; a restarted resilver ends before the attach txg: " t }' \
	    "$O/dbgmsg-scan.txt"
}
KEEP=0; STAGE=0
panic_capture() {
	KEEP=1
	sleep 5
	klog > "$O/kernel.txt"
	ps -eLo pid,tid,stat,etime,comm,wchan:32 > "$O/ps-all.txt"
	awk 'NR == 1 || $3 ~ /^D/' "$O/ps-all.txt" > "$O/ps-D.txt"
	for tid in $(awk 'NR > 1 {print $2}' "$O/ps-D.txt" | head -32); do
		{ echo "-- tid $tid $(cat "/proc/$tid/comm" 2>/dev/null)"
		  timeout 5 cat "/proc/$tid/stack"; } >> "$O/stacks.txt" 2>&1
	done
	( cat $DBG > "$O/dbgmsg.txt" 2>&1
	  tail -n 4 "/proc/spl/kstat/zfs/$P/txgs" > "$O/txgs.txt" 2>&1 ) &
	cat "$E" > "$O/inject-after.txt" 2>&1
	sleep 5
	{ echo "RUN $T ($MODE): PANIC after ${W} s: $(grep -m1 -o 'VERIFY.*' "$O/kernel.txt") | $(grep -m1 -o 'PANIC at .*' "$O/kernel.txt")"
	  echo "RUN $T ($MODE): $(zio_summary)"
	  echo "RUN $T ($MODE): $(scan_summary)"; } | tee "$O/verdict.txt" >> "$O/steps.txt"
	exit 0
}
teardown() {
	if [ "$KEEP" = 1 ]; then
		log "run $T end (pool and devices left as they are)"; return
	fi
	if [ "$STAGE" -ge 2 ]; then
		bg "$O/destroy.txt" 120 "$ZPOOL" destroy "$P"
		log "destroy: $(tail -n 1 "$O/destroy.txt")"
		if ! grep -q '^rc=0$' "$O/destroy.txt"; then
			log "run $T end (pool NOT destroyed, devices left)"; return
		fi
	fi
	if [ "$STAGE" -ge 1 ]; then
		udevadm settle -t 30
		lsmod | grep -q '^scsi_debug ' && bg "$O/rm-scsi_debug.txt" 60 \
		    sh -c 'for i in 1 2 3 4 5; do modprobe -r scsi_debug && exit 0; sleep 2; done; exit 1'
		for d in is5a is5b; do
			[ -d "$CFG/$d" ] && { echo 0 > "$CFG/$d/power"; rmdir "$CFG/$d"; }
		done
		lsmod | grep -q '^null_blk ' && bg "$O/rm-null_blk.txt" 60 \
		    sh -c 'for i in 1 2 3 4 5; do modprobe -r null_blk && exit 0; sleep 2; done; exit 1'
	fi
	log "left (empty is clean): $(lsmod | awk '$1 == "scsi_debug" ||
	    $1 == "null_blk" {printf "%s ", $1}')$(ls /dev/is5a /dev/is5b 2>/dev/null)"
	log "run $T end"
}
trap teardown EXIT

echo "task-1123-is5: run $T start" > /dev/kmsg
log "run $T mode=$MODE (sense 3/$ASC/00) zfs srcversion $(cat /sys/module/zfs/srcversion) failfast_mask $(cat /sys/module/zfs/parameters/zfs_vdev_failfast_mask)"
[ "$(cat /sys/module/spl/parameters/spl_panic_halt)" = 0 ] ||
    { log "STOP: spl_panic_halt is not 0"; exit 1; }
if lsmod | grep -qE '^(scsi_debug|null_blk) ' || [ -e /dev/is5a ] || [ -e /dev/is5b ]; then
	log "STOP: scsi_debug, null_blk or is5a/is5b already present"; exit 1
fi
echo 0 > $DBG

# Two memory-backed null_blk disks, as in the report.
modprobe null_blk nr_devices=0 || { log "STOP: modprobe null_blk"; exit 1; }
STAGE=1
mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config
for d in is5a is5b; do
	{ mkdir "$CFG/$d" && echo 256 > "$CFG/$d/size" &&
	    echo 4096 > "$CFG/$d/blocksize" && echo 1 > "$CFG/$d/memory_backed" &&
	    echo 1 > "$CFG/$d/power"; } || { log "STOP: null_blk $d"; exit 1; }
done
# The new disk serves one command at a time, so the resilver takes seconds.
modprobe scsi_debug dev_size_mb=320 sector_size=4096 max_queue=1 delay=1 ||
    { log "STOP: modprobe scsi_debug"; exit 1; }
udevadm settle -t 30
H=; N=
for h in /sys/bus/pseudo/drivers/scsi_debug/adapter0/host*/target*/*:*:*:*; do
	[ -e "$h" ] && { H=${h##*/}; break; }
done
for b in "/sys/bus/scsi/devices/$H/block/"*; do
	[ -n "$H" ] && [ -e "$b" ] && { N=${b##*/}; break; }
done
E=/sys/kernel/debug/scsi_debug/$H/error
log "disks: is5a $(cat /sys/block/is5a/size 2>&1), is5b $(cat /sys/block/is5b/size 2>&1) sectors;" \
    "scsi_debug ${H:-none} = ${N:-none}: $(tr -s ' ' < "/sys/bus/scsi/devices/$H/model" 2>&1)," \
    "$(cat "/sys/block/$N/size" 2>&1) sectors, lbs $(cat "/sys/block/$N/queue/logical_block_size" 2>&1)"
# Use nothing but the two null_blk disks and the fresh scsi_debug disk.
if [ -z "$H" ] || [ -z "$N" ] || [ ! -b /dev/is5a ] || [ ! -b /dev/is5b ] ||
    [ "$(cat /sys/block/is5a/size)" != 524288 ] ||
    ! grep -q '^scsi_debug' "/sys/bus/scsi/devices/$H/model" ||
    [ "$(cat "/sys/block/$N/size")" != 655360 ] ||
    [ -n "$(find "/sys/block/$N/" -maxdepth 1 -name "${N}[0-9]*")" ] ||
    grep -q "^/dev/$N" /proc/mounts || [ ! -e "$E" ]; then
	log "STOP: device check"; exit 1
fi

timeout 60 "$ZPOOL" create -o ashift=12 -o cachefile=none -O compression=off \
    -O mountpoint="$MP" "$P" mirror /dev/is5a /dev/is5b > "$O/create.txt" 2>&1 ||
    { log "STOP: zpool create: $(head -c 300 "$O/create.txt")"; exit 1; }
STAGE=2
dd if=/dev/urandom of="$MP/f" bs=1M count=96 conv=fsync status=none ||
    { log "STOP: dd"; exit 1; }
log "pool created, 96 MiB written, resilver_defer $(timeout 30 "$ZPOOL" get -H -o value feature@resilver_defer "$P")"
timeout 60 "$ZPOOL" replace "$P" /dev/is5b "/dev/$N" > "$O/replace.txt" 2>&1 ||
    { log "STOP: zpool replace: $(head -c 300 "$O/replace.txt")"; exit 1; }
sleep 1
# Mid-resilver: fail the next 4 WRITE(10) and 4 WRITE(16) commands.
for op in 2a 8a; do
	echo "2 -4 $op 0 0 2 3 $ASC 0" > "$E" || { log "STOP: injection refused"; exit 1; }
done
log "is5b replaced by $N; injected: $(tail -n +2 "$E" | tr '\t\n' ' ;')"

t0=$SECONDS; RES=TIMEOUT
while [ $((SECONDS - t0)) -lt 120 ]; do
	sleep 1
	klog | grep -qE 'PANIC at|VERIFY|ASSERT' && { RES=PANIC; break; }
	grep -q 'detach vdev=/dev/is5b' $DBG && { RES=DONE; break; }
done
W=$((SECONDS - t0))
log "after $W s: $RES"
[ $RES = PANIC ] && panic_capture

cat $DBG > "$O/dbgmsg.txt" 2>&1
cat "$E" > "$O/inject-after.txt" 2>&1
bg "$O/status-1.txt" 30 "$ZPOOL" status -vpP "$P"
NEW="/dev/${N}1"
S1=$(awk -v d="$NEW" '$1 == d {print "READ " $3 " WRITE " $4 " CKSUM " $5}' "$O/status-1.txt")
S2="no scrub"
if [ $RES = DONE ]; then
	bg "$O/scrub.txt" 120 "$ZPOOL" scrub -w "$P"
	klog | grep -qE 'PANIC at|VERIFY|ASSERT' && { W=$((SECONDS - t0)); panic_capture; }
	bg "$O/status-2.txt" 30 "$ZPOOL" status -vpP "$P"
	S2="$(awk -v d="$NEW" '$1 == d {print "READ " $3 " WRITE " $4 " CKSUM " $5}' "$O/status-2.txt"), $(grep -m1 -o 'scrub repaired.*' "$O/status-2.txt")"
fi
{ if [ $RES = DONE ]; then echo "RUN $T ($MODE): NO PANIC, replace done after $W s"
  else echo "RUN $T ($MODE): NO PANIC, replace NOT done after $W s"; fi
  echo "RUN $T ($MODE): $NEW after the resilver: ${S1:-not listed}; after a scrub: $S2"
  echo "RUN $T ($MODE): $(zio_summary)"
  echo "RUN $T ($MODE): $(scan_summary)"; } | tee "$O/verdict.txt" >> "$O/steps.txt"
exit 0
IS5RUN_END
chmod 755 $D/is5-run.sh
sha256sum $D/is5-run.sh | cut -c1-64; wc -l < $D/is5-run.sh; bash -n $D/is5-run.sh && echo syntax-ok
```

Expected: `ded5421d5c84ee4da74dc8b4a74a5589bcf98df764bfa40f5edb86ea512b4650`, `201`, `syntax-ok`. A different sum is a stop (the transport changed the text): report the first lines that differ if you can see them.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is5; L=/var/log/task-1123-is5; T=<tag>
setsid nohup sudo -n bash $D/is5-run.sh $T <mode> > $L/run-$T.log 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/run-$T/steps.txt` every 10 s until it ends with a line `run $T end ...` (about 1 minute for a PANIC, 1 to 2 minutes otherwise).

| order | tag | arm | mode | when | expected |
|---|---|---|---|---|---|
| 1 | m-eio | zm | `eio` | always | NO PANIC: one resilver, replace done; the scrub finds the blocks whose writes failed missing on the new disk (CKSUM > 0) |
| 2 | m-enodata1 | zm | `enodata` | always (the report) | PANIC at `vdev.c:3404`; two resilvers, the second ending before the attach txg |
| 3 | m-enodata2 | zm | `enodata` | if run 2 was not a PANIC, or at most 35 minutes have passed | the same |
| 4 | f-enodata1 | F (step 6 first) | `enodata` | always | NO PANIC: two resilvers, the second ending before the attach txg; replace done; scrub CKSUM 0 on the new disk |
| 5 | f-enodata2 | F | `enodata` | if at most 50 minutes have passed | the same |
| 6 | p-eio | P (step 6 first) | `eio` | if at most 58 minutes have passed when step 6 for P would start | PANIC at `vdev.c:3404` |

**A run whose verdict is PANIC continues at step 5**, then comes back here for the next run.

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `verdict.txt` (three lines for a PANIC, four otherwise);
- `dbgmsg-scan.txt` and `zio-errors.txt` in full, and `inject-after.txt` (each count back to 0 means its four failures were used);
- for a NO PANIC run: from `status-1.txt` and `status-2.txt`, the config section and the `scan:` line; `scrub.txt`, `destroy.txt`, `rm-scsi_debug.txt`, `rm-null_blk.txt`, and the `left (empty is clean):` line, which must be empty after the colon;
- the kernel-line check since the run's mark, and the counts of `zio pool=is5pool` lines and of SCSI error lines;
- `lsmod | grep -E '^(scsi_debug|null_blk) '` (empty after a NO PANIC run).

How to read a verdict: in a PANIC run, the attach txg of the last line must equal the left number of the `VERIFY3U` line and the end of the restarted resilver the right one; in an `enodata` run on zm with NO PANIC, `ordinary` above 0 in the failed I/Os (a write of the current txg failed too), or a restarted resilver ending exactly at the attach txg (a failed block born in R - 1), explains it; in an F run, "ends before the attach txg: yes" with NO PANIC is the fix at work.

## Step 5: after a PANIC (the reboot is pre-approved here, once per PANIC run)

1. **Quote** from `$L/run-$T`: `verdict.txt`; from `kernel.txt`, the `VERIFY3U` line, the `PANIC at` line and the stack that follows (through the end of the `Call Trace`), in full; `ps-D.txt` in full; from `stacks.txt`, the stack of `txg_sync` in full and the first three frames of every other; `txgs.txt`; `dbgmsg-scan.txt`, `zio-errors.txt` and `inject-after.txt` if not quoted yet. Do not wait for the hung-task watchdog.
2. Make sure the logs and the tree are on disk: `timeout 30 sync -f $L/run-$T/verdict.txt; timeout 30 sync -f $D/zm/module/zfs.ko` (these sync only the file systems of `/var/log` and `/var/tmp`, never ZFS).
3. **Reboot:** a graceful reboot would hang on the frozen pool, so reboot at once with the magic SysRq key: `echo b | sudo -n tee /proc/sysrq-trigger`. If the VM is not down within 2 minutes, `sudo -n systemctl reboot -ff`. If it is not back within 10 minutes, stop and report.
4. **After the reboot,** check and quote: `uptime`; `uname -r` (as in step 0); `lsmod | grep -E '^(zfs|spl|scsi_debug|null_blk) '` (empty); `ls -l /dev/zfs /dev/is5a /dev/is5b 2>&1` (none); `ls -l /etc/zfs/zpool.cache 2>&1` (as in step 0); `ls -l $L/run-$T/verdict.txt $L/run-$T/kernel.txt` (present, not empty); `cd $D/zm && git rev-parse HEAD && git write-tree && git status --porcelain --untracked-files=no` (the current arm's values: step 1 or step 6); `modinfo -F srcversion $D/zm/module/zfs.ko` (the current arm's saved srcversion).
5. If the next run is on the same arm: `sudo -n $D/zm/scripts/zfs.sh`, then step 3's checks (srcversion equal to the file's and to the saved one, "(DEBUG mode)", `spl_panic_halt` 0), and back to step 4. If the next run is on another arm, go to step 6 (nothing is loaded). If there is no next run, go to step 7.

## Step 6: switch arm (before run 4 with A=F, before run 6 with A=P)

```sh
D=/var/tmp/task-1123-is5; L=/var/log/task-1123-is5; A=<F|P>; C=<commit>
cd $D/zm && git checkout -q $C -- module/zfs/vdev.c &&
  echo "$A $(git rev-parse HEAD) $(git write-tree) $(git hash-object module/zfs/vdev.c)" | tee -a $L/trees.txt &&
  git status --porcelain --untracked-files=no && git diff --cached --numstat
cd $D/zm; grep -n 'ASSERT3U(vd->vdev_resilver_txg' module/zfs/vdev.c; grep -n -A2 'if (zio->io_error == EIO &&$' module/zfs/vdev.c
setsid nohup sh -c "date -u +%T; cd $D/zm && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build-$A.log 2>&1 < /dev/null &
```

**F** (`C=3c465f0a721582511d34d1148718d9eb62047478`), expected before the build (no `ASSERT3U(vd->vdev_resilver_txg` line is left; master's EIO filter, four lines lower):

```
F a1daebfec3f5ca46ac261ef339efdf0dbeae997c daed71d781db6d55bfd6e1a8e9b92d65c00cb6b2 a76b19ccd18e4b38372258c78d3c477bf449e179
M  module/zfs/vdev.c
6	2	module/zfs/vdev.c
5506:	if (zio->io_error == EIO &&
5507-	    !(zio->io_flags & ZIO_FLAG_IO_RETRY))
5508-		return;
```

**P** (`C=b4578f4312d3bba362d1afbd31753a78225ab8d3`), expected before the build:

```
P a1daebfec3f5ca46ac261ef339efdf0dbeae997c 8f6fbff2e60d1b5311416f30d8999f240ad6fddb ecade036a6901b2df2c48e1c3f68205d56e75f58
M  module/zfs/vdev.c
10	4	module/zfs/vdev.c
3404:			ASSERT3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg);
5507:	if (zio->io_error == EIO &&
5508-	    !(zio->io_flags & ZIO_FLAG_IO_RETRY) &&
5509-	    !vd->vdev_ops->vdev_op_leaf)
```

Any other difference is a stop for that arm: skip its runs and report. The rebuild recompiles `vdev.c` (and, the tree being dirty now, `spl-proc.c` and `spa_history.c`, which include `zfs_gitrev.h`) and relinks, about 2 to 4 minutes; poll `grep -H build-rc= $L/build-$A.log`, which must read `build-rc=0`; quote its duration and any `warning:` line. Then:
- if zfs is loaded (no reboot since the last run): `timeout 30 sudo -n $D/zm/zpool list -H -o name` must show no pool, then `sudo -n $D/zm/scripts/zfs.sh -u` and `lsmod | grep -E '^(zfs|spl) '` must be empty;
- `sudo -n $D/zm/scripts/zfs.sh`; `cat /sys/module/zfs/srcversion` must equal `modinfo -F srcversion $D/zm/module/zfs.ko` **and differ from every srcversion saved before** (else the rebuild did not take: stop for that arm); save it in `$L/srcversion-$A.txt`; "(DEBUG mode)"; `spl_panic_halt` 0. Back to step 4.

## Step 7: cleanup

- If zfs is loaded: no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty), then `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.
- `lsmod | grep -E '^(scsi_debug|null_blk) '` and `ls -l /dev/is5a /dev/is5b 2>&1`: must show nothing. If something is left (a run's teardown failed), quote it and leave it.
- `lsmod | grep -E '^(configfs|sd_mod|scsi_mod|scsi_common) '` and `mountpoint /sys/kernel/config 2>&1`: quote them next to step 0's; leave them as they are.
- `grep -c task-1123-is5 /proc/mounts`: must be 0.
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is5` (`sudo -n rm -rf /var/tmp/task-1123-is5`). Anything else new: quote it and leave it.
- `/etc/zfs/zpool.cache`: quote `ls -l` of it; it must be as in step 0 (every pool used `cachefile=none`).
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is5`.

## Report

Write `/var/log/task-1123-is5/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, the module checks, lockdown, the kernel sysctls and the zfs service states of step 0, `trees.txt`, the log line, the blob and the code lines of step 1, the build rc, duration and warnings of each build, each arm's srcversion with "(DEBUG mode)", `spl_panic_halt`, `zfs_vdev_failfast_mask`, the script's sha256.
2. **Table:** tag | arm | mode | verdict (PANIC or NO PANIC, after how many seconds) | failed I/Os (repair / ordinary / probe, errno) | attach txg; resilver ranges | new disk after the resilver; after the scrub | reboot.
3. **Verdict lines:**
   - `#20007 on master a1daebfec3f: reproduced <n>/<m> at vdev.c:3404 (VERIFY3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg) failed (<R> <= <M>))` or `#20007 on master a1daebfec3f: not reproduced in <m> runs`, followed by one sentence: the parked thread and the top of its stack, the attach txg against the restarted resilver's range, and whether every failed write was a repair write.
   - `#19906 (vdev.c of 3c465f0a721) on master: <k>/<m> runs without a panic; replace done after a restarted resilver (<a>, <b>] ending before attach txg <R>; scrub CKSUM <c> on the new disk` (or what happened).
   - `EIO write errors (sense 3/0C): master: <NO PANIC, replace done, the scrub found <c> blocks missing on the new disk | what happened>; #20003 (vdev.c of b4578f4312d): <PANIC at vdev.c:3404 | what happened>`.
4. **Quotes:** per run, what step 4 lists, step 5's quotes and checks for each PANIC, step 6's outputs, and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
