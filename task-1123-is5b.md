TASK-1123-IS5B (from the coordinator session "main"): functional, about 30 to 45 minutes with its build, at most 1 h 15. **A reboot is pre-approved after a run whose verdict is PANIC (step 5), and only there, made through the recording wrapper of step 3**; one is expected, at most three in this task. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12.118+deb13-cloud-amd64), **alone on that VM**: it loads `scsi_debug` and `null_blk`, and one run is expected to park a ZFS kernel thread on a failed assertion, then a SysRq reboot. It follows TASK-1123-IS5 (VM 105, 23/11 12:28-12:41Z) on upstream issue openzfs/zfs#20007 ("Resilver restarted after failed repair writes during zpool replace trips VERIFY in vdev_dtl_should_excise()") with three changes: (1) the arm with PR #19906's fix runs the reporter's reproducer until it has 3 VALID runs; (2) the EIO comparison of master and PR #20003 uses a write error that the SCSI layer does not retry; (3) every reboot is written to a synced log file before it is made. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-is5b/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `T`, `A`, `C`).

## Why

- **What IS5 showed (VM 105, master `a1daebfec3f`, debug build):**
  - The reporter's reproducer (4 WRITE commands of the new disk failed with sense 3/11/00, i.e. ENODATA) panicked master 2 of 2 times: `VERIFY3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg) failed (66 <= 21)`, then `(66 <= 11)`, `PANIC at vdev.c:3404`, `txg_sync` parked in `spl_panic`. In both runs the 4 failed writes were repair writes, and the restarted resilver, (9, 21] and (9, 11], ended before the attach txg 66.
  - With #19906's `vdev.c`: run f-enodata1 was VALID and passed. Its restarted resilver (12, 15] ended before the attach txg 66; the replace completed; the scrub found CKSUM 0. Run f-enodata2 was **not a test**: one failed repair write was a 4 KiB block born in txg 66, the attach txg, so the restarted resilver was (10, 67] and covered it. One valid run is too few: this task repeats the arm until 3 runs are VALID.
  - The EIO runs (sense 3/0C/00) on master and on #20003 tested nothing. scsi_debug failed WRITE(10) four times, 16 to 24 ms apart, and the fifth try succeeded. No `I/O error` and no `zio pool=` line appeared: ZFS saw no error.
  - Both reboots happened at the pre-approved points, but the VM session's own record does not show the command (it restarted with the VM).
- **Why the injection cannot skip the blocks born in the attach txg (checked by main):** scsi_debug's error injection on 6.12 (`error` file, `sdebug_fail_cmd()`) matches a command by opcode and count only, and its LBA-ranged medium errors (`medium_error_start`) apply to reads only (`resp_read_dt0()`, `scsi_debug.c:3949`). The blocks born in the attach txg R to R+3 are the attach's own metadata (the new disk's DTL space map, its vdev ZAPs, the config, the history), 4 KiB each. The first resilver covers (3, R+3] and copies them among the 128 KiB file blocks. Keeping them off the disk would need a special vdev for the metadata, as #19906's own test does, which changes the reporter's pool layout. This task keeps the layout and checks each run instead: a run is **VALID** when its restarted resilver ends before the attach txg, i.e. the last line of its `verdict.txt` ends with `ends before the attach txg: yes`. IS5 saw 3 VALID of 4 such runs.
- **Why 3/0C was retried, and the setting that stops it (6.12 source, read by main):**
  - `scsi_check_sense()` returns NEEDS_RETRY for MEDIUM ERROR unless the asc is 0x11, 0x13 or 0x14 (`scsi_error.c:686-693`). `scsi_decide_disposition()` then retries while `scsi_cmd_retry_allowed()` allows it (`++cmd->retries <= cmd->allowed`, `scsi_error.c:117`) and `scsi_noretry_cmd()` does not forbid it.
  - sd sets `cmd->allowed = sdkp->max_retries` for every read and write (`sd.c:1410`); the default is `SD_MAX_RETRIES` = 5 (`sd.h:27`, `sd.c:3966`).
  - scsi_debug spends one count per attempt, because each retry is a new queuecommand. So `-4` was used up by the retries of one command.
  - `vdev_disk_io_rw()` asks for `REQ_FAILFAST_DEV` on these writes (`vdev_disk.c:1038-1042`), and `scsi_noretry_cmd()` honours it for a CHECK CONDITION. The retries mean the flag did not reach the request. Main has not found why; it is not this task's subject. The script records ZFS's `failfast` vdev property after each run that does not panic.
  - **The setting:** sd's per-disk attribute `max_retries` (`/sys/class/scsi_disk/<H:C:T:L>/max_retries`, `sd.c:622-652`, range -1 to 5), set to `0` on the scsi_debug disk only, in mode `eio`. `scsi_cmd_retry_allowed()` is then false at the first failure, and the command completes with `BLK_STS_IOERR`, i.e. EIO. Each injected count fails exactly one command, as 3/11 does. A count of 24 (4 commands of 6 attempts each) would work only if every failing command were retried exactly 5 times back to back; a command that is not retried would shift it. Mode `enodata` leaves `max_retries` at 5: 3/11 is never retried.
- **Expected:** on master, the EIO-failed repair writes reach ZFS (`error=5`) and are dropped at `vdev.c:5502`: one resilver, the replace completes, and the scrub finds those blocks missing on the new disk (#20003's bug). With #20003's `vdev.c`, they reach the DTL, the resilver restarts, and a VALID run panics at `vdev.c:3404`. With #19906's `vdev.c`, a VALID enodata run completes the replace with nothing missing.

## What is tested

The same arms as IS5, fetched from https://github.com/openzfs/zfs (heads unchanged at 13:15Z by `git ls-remote`: master `a1daebfec3f`, `refs/pull/19906/head` `3c465f0a721`, `refs/pull/20003/head` `b4578f4312d`). Each PR arm takes only that PR's `module/zfs/vdev.c`; `HEAD` stays at master.

| arm | tree | tree hash | `module/zfs/vdev.c` blob | srcversion on VM 105 (IS5) |
|---|---|---|---|---|
| zm | master a1daebfec3f5ca46ac261ef339efdf0dbeae997c | 0b23c141b417a8542546e1f7a32662bc82bc0f92 | 25ed7a4a7419aceee5f28cfb4fbd49b4ebc86216 | 371A65E8D86145973554A8B |
| F | zm + `vdev.c` of #19906 3c465f0a721582511d34d1148718d9eb62047478 | daed71d781db6d55bfd6e1a8e9b92d65c00cb6b2 | a76b19ccd18e4b38372258c78d3c477bf449e179 | 2D702FBD21B6F38609436D7 |
| P | zm + `vdev.c` of #20003 b4578f4312d3bba362d1afbd31753a78225ab8d3 | 8f6fbff2e60d1b5311416f30d8999f240ad6fddb | ecade036a6901b2df2c48e1c3f68205d56e75f58 | 827E0E5CDA9263208787CE9 |

The srcversions should repeat on the same kernel, but a different one is not a stop: it also hashes generated headers.

Each run uses the script `is5b-run.sh` below, which is IS5's `is5-run.sh` with only these changes:
- its paths and kernel-log mark say `is5b`;
- in mode `eio` it writes `0` to the scsi_debug disk's `max_retries` before the pool is made, and logs that value in every mode;
- it logs the injection as written (IS5 read it back after one count was already spent);
- after a run that does not panic it saves `zpool get failfast ... all-vdevs`.

The scenario is unchanged: two memory-backed null_blk disks `is5a`, `is5b`, a scsi_debug disk (320 MiB, 4 KiB sectors, `max_queue=1 delay=1`), a pool `is5pool` mirroring `is5a` and `is5b` (`ashift=12`, `compression=off`, `cachefile=none`, mounted under `$D`), and a 96 MiB file. Then `zpool replace is5pool /dev/is5b /dev/<scsi_debug disk>`; one second later the next 4 WRITE(10) and 4 WRITE(16) commands of that disk fail with sense MEDIUM ERROR, asc 0x11 (`enodata`) or 0x0c (`eio`); then up to 120 s for a PANIC or for the replace to finish. The outcomes and files are IS5's: a PANIC leaves everything as it is; otherwise the script scrubs, destroys the pool and removes the devices. `verdict.txt` gives the failed I/Os (repair / ordinary / probe, errno/type) and the resilvers with the attach txg.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package. Change no sysctl and no module parameter. The one device setting this task changes is the `max_retries` of its own scsi_debug disk (the script does it, mode `eio` only); it goes away with the disk.
- **Alone on the VM:** no other task, build or test runs on it during this task.
- **Devices.** The only block devices this task uses are the three its script creates: the null_blk disks `/dev/is5a` and `/dev/is5b` and one scsi_debug disk, which the script finds in sysfs and checks (model `scsi_debug`, 655360 sectors, no partition, not mounted, a `max_retries` attribute) before passing it to `zpool replace`. Never pass any other `/dev` path to a command. Never unload `sd_mod`, `scsi_mod`, `scsi_common` or `configfs`. Never run `zpool import -a` or `zfs-tests.sh -x`. This task runs no ZTS.
- **Before every run:**
  - `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/zm/module/zfs.ko` and the srcversion saved for the current arm;
  - `timeout 30 sudo -n $D/zm/zpool list -H -o name` shows no pool;
  - `lsmod | grep -E '^(scsi_debug|null_blk) '` is empty;
  - `cat /sys/module/spl/parameters/spl_panic_halt` is `0`.
- **The builds and every run run detached** (`setsid nohup ... &`) and are polled.
- **After a PANIC** (a run's `verdict.txt` says PANIC, or the kernel lines since the run's mark show `PANIC at` or `VERIFY`, whatever the script says): `txg_sync` is parked for good. Until the reboot of step 5, run no `zpool`, `zfs` or `zdb` command, nothing that reads mounts (`df`, `mount`, `findmnt`, `lsblk`, `systemctl status`, `ls` of anything under `$D/mnt-*`), unload no module, and read kernel files only in the background: `(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`. Go to step 5.
- **Reboots** are made only with step 5's command line, which writes the command to `$L/reboots.log` and syncs it before running the wrapper `is5b-reboot.sh`. The wrapper refuses (exit 2) unless the run started in this boot and ended in a PANIC; a refusal is not worked around: report it.
- **Hangs of another kind** (no PANIC): the build of step 2 still going after 60 minutes or an arm's rebuild after 20, a run whose `steps.txt` has not changed for 5 minutes, or a run with no `run <tag> end` line after 8 minutes. Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zpool|udev|modprobe'`, the `/proc/<tid>/stack` of each D-state thread (in the background as above), `sudo -n dmesg | tail -n 150`. Then stop: **no reboot** (not pre-approved), write the report.
- **Kernel log.** After each run, check the kernel lines since that run's mark (`sudo -n dmesg | sed -n '/task-1123-is5b: run <tag> start/,$p'`; the script writes the mark) for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. In a PANIC run the `VERIFY3U` line, the `PANIC at` line and their stack are the expected finding; anything else is a finding: quote it in full. Count the `zio pool=is5pool` lines, the SCSI error lines (`Sense Key`, `critical medium error`, `I/O error, dev`) and scsi_debug's `fail command` lines.
- **Time.** Count from step 0. A run or arm marked conditional starts only within its limit (step 4's table). At 1 h 10, start nothing new: go to step 7.

## Step 0: state

Check and quote:
- `date -u`, `uname -r` (expected `6.12.118+deb13-cloud-amd64`; with another kernel, carry on only if the module checks below pass, and say so), `nproc`, `free -m` (at least 2048 MB available), `df -h /var/tmp` (at least 8 GiB free, else stop and report);
- `lsmod | grep -E '^(zfs|spl|scsi_debug|null_blk) '`: must be empty. If a pool is imported, stop and report;
- `lsmod | grep -E '^(configfs|sd_mod|scsi_mod|scsi_common) '` and `mountpoint /sys/kernel/config 2>&1`: quote them (compared in step 7);
- `ls -l /dev/zfs /dev/is5a /dev/is5b 2>&1`: none may exist;
- `command -v git python3 udevadm logger && sudo -n true && echo sudo-ok`;
- `ls -d /lib/modules/$(uname -r)/build`: must exist; if not, stop and report;
- `modinfo -n scsi_debug null_blk configfs`: three paths of this kernel ending in `kernel/drivers/scsi/scsi_debug.ko.xz`, `kernel/drivers/block/null_blk/null_blk.ko.xz` and `kernel/fs/configfs/configfs.ko.xz`; `modinfo -F vermagic scsi_debug null_blk`: both start with `6.12.118+deb13-cloud-amd64 SMP preempt mod_unload modversions`; `modinfo -F parm scsi_debug | grep -cE '^(dev_size_mb|sector_size|max_queue|delay):'` prints `4`. A missing module is a stop: report it;
- `cat /sys/kernel/security/lockdown`: must show `[none]`; else stop and report;
- `mountpoint /sys/kernel/debug`: must be a mountpoint;
- `lsblk -dn -o NAME,SIZE,TYPE,TRAN,MODEL | wc -l` and `lsblk -dn -o NAME,MODEL | grep -c scsi_debug` (must be 0);
- `pgrep -a zed; systemctl is-active zfs-zed.service 2>&1`: **no `zed` process may run**; else stop and report;
- `ls -d /var/tmp/task-1123-is5b /var/log/task-1123-is5b 2>&1`: both absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- `cat /proc/sys/kernel/hung_task_timeout_secs /proc/sys/kernel/hung_task_panic /proc/sys/kernel/panic /proc/sys/kernel/panic_on_oops /proc/sys/kernel/panic_on_warn /proc/sys/kernel/sysrq`: quote them; do not change them. **If `hung_task_panic` or `panic_on_warn` is not 0, stop and report**;
- `journalctl --list-boots --no-pager | tail -n 2`: quote it (the journal is compared after a reboot);
- `systemctl is-enabled zfs-import-cache.service zfs-import-scan.service zfs-mount.service zfs-load-module.service zfs-zed.service 2>&1`: quote it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b; U=https://github.com/openzfs/zfs
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && ls -A /var/tmp > $L/vartmp-0.txt && touch $L/reboots.log &&
  sudo -n mkdir -p $D && sudo -n chown "$(id -un):" $D
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
cd $D/zm && for c in 3c465f0a721582511d34d1148718d9eb62047478 b4578f4312d3bba362d1afbd31753a78225ab8d3; do
  git fetch -q --depth 1 $U $c && echo "fetched $c $(git rev-parse $c^{tree}) $(git rev-parse $c:module/zfs/vdev.c)"; done
git -C $D/zm rev-parse HEAD; git -C $D/zm status --porcelain | wc -l
```

Expected (as in IS5; `vartmp-0.txt` is now taken before `$D` exists):

```
zm a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0
Rick Galliher | ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
25ed7a4a7419aceee5f28cfb4fbd49b4ebc86216
3404:			ASSERT3U(vd->vdev_resilver_txg, <=, scnp->scn_max_txg);
5502:	if (zio->io_error == EIO &&
1340:			    "starting deferred resilver", tx, "errors=%llu",
8920:			newvd->vdev_resilver_txg = txg;
36:static unsigned int spl_panic_halt;
fetched 3c465f0a721582511d34d1148718d9eb62047478 8299524723a189e046f05267e67851132efeb712 a76b19ccd18e4b38372258c78d3c477bf449e179
fetched b4578f4312d3bba362d1afbd31753a78225ab8d3 ed36965a3b709c347374d90657244298c909078d ecade036a6901b2df2c48e1c3f68205d56e75f58
a1daebfec3f5ca46ac261ef339efdf0dbeae997c
0
```

If a fetch fails, wait 1 minute and try once more; if fetching by commit is refused, use `git fetch -q --depth 200 $U master` and `git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c` for master, and `git fetch -q --depth 1 $U refs/pull/19906/head` (resp. `refs/pull/20003/head`) for a PR, then quote `git rev-parse FETCH_HEAD`: if it is not the commit above, the PR has moved on: skip that arm and report the new head. Any other difference is a stop: report the outputs as they are. Edit no file of the tree; step 6 changes `module/zfs/vdev.c` only, with `git checkout`.

## Step 2: the debug build (about 4 to 20 minutes)

```sh
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b
setsid nohup sh -c "date -u +%T; cd $D/zm && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Quote the duration and every `warning:` line outside objtool notes.

## Step 3: load the module and write the two scripts

**Load zm:**
- `sudo -n $D/zm/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zm/module/zfs.ko`; save it: `cat /sys/module/zfs/srcversion > $L/srcversion-zm.txt`;
- quote the last `ZFS: Loaded module` line of `sudo -n dmesg`, which must say "(DEBUG mode)";
- quote `cat /sys/module/spl/parameters/spl_panic_halt`: **must be `0`**, else stop and report;
- quote `cat /sys/module/zfs/parameters/zfs_vdev_failfast_mask` (expected `1`).

**Write the run script** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is5b
cat > $D/is5b-run.sh <<'IS5BRUN_END'
#!/bin/bash
# is5b-run.sh <tag> <enodata|eio>
# One attempt at openzfs/zfs#20007 with the reporter's reproducer, against
# the zfs module loaded from $D/zm: a mirror of two memory-backed null_blk
# disks holding a 96 MiB file, zpool replace of the second by a scsi_debug
# disk that serves one command at a time, and one second later the next
# 4 WRITE(10) and 4 WRITE(16) commands of that disk failed with CHECK
# CONDITION, sense key MEDIUM ERROR and
#   enodata: asc 0x11, as reported (the block layer returns ENODATA)
#   eio:     asc 0x0c, write error, with the disk's sd max_retries set to
#            0 beforehand so that each failure ends its command (EIO);
#            with the default 5 the SCSI layer retries 3/0C
# then up to 120 s for a PANIC or for the replace to finish.  After a PANIC
# nothing touches the pool again: the kernel log, stacks and dbgmsg are
# saved and the script exits, leaving everything as it is.  Otherwise the
# pool is scrubbed and destroyed, and the devices removed.  Runs as root.
# shellcheck disable=SC2317  # teardown runs from the EXIT trap
set -u
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b; Z=$D/zm
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
klog() { dmesg | sed -n "/task-1123-is5b: run $T start/,\$p"; }
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

echo "task-1123-is5b: run $T start" > /dev/kmsg
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
E=/sys/kernel/debug/scsi_debug/$H/error; MR=/sys/class/scsi_disk/$H/max_retries
log "disks: is5a $(cat /sys/block/is5a/size 2>&1), is5b $(cat /sys/block/is5b/size 2>&1) sectors;" \
    "scsi_debug ${H:-none} = ${N:-none}: $(tr -s ' ' < "/sys/bus/scsi/devices/$H/model" 2>&1)," \
    "$(cat "/sys/block/$N/size" 2>&1) sectors, lbs $(cat "/sys/block/$N/queue/logical_block_size" 2>&1)"
# Use nothing but the two null_blk disks and the fresh scsi_debug disk.
if [ -z "$H" ] || [ -z "$N" ] || [ ! -b /dev/is5a ] || [ ! -b /dev/is5b ] ||
    [ "$(cat /sys/block/is5a/size)" != 524288 ] ||
    ! grep -q '^scsi_debug' "/sys/bus/scsi/devices/$H/model" ||
    [ "$(cat "/sys/block/$N/size")" != 655360 ] ||
    [ -n "$(find "/sys/block/$N/" -maxdepth 1 -name "${N}[0-9]*")" ] ||
    grep -q "^/dev/$N" /proc/mounts || [ ! -e "$E" ] || [ ! -e "$MR" ]; then
	log "STOP: device check"; exit 1
fi
if [ "$MODE" = eio ]; then
	echo 0 > "$MR" || { log "STOP: max_retries"; exit 1; }
fi
log "sd max_retries of $N: $(cat "$MR" 2>&1)"

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
log "is5b replaced by $N; injected: 2 -4 2a 0 0 2 3 $ASC 0; 2 -4 8a 0 0 2 3 $ASC 0"

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
bg "$O/failfast.txt" 30 "$ZPOOL" get -H -o name,value,source failfast "$P" all-vdevs
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
IS5BRUN_END
chmod 755 $D/is5b-run.sh
sha256sum $D/is5b-run.sh | cut -c1-64; wc -l < $D/is5b-run.sh; bash -n $D/is5b-run.sh && echo syntax-ok
```

Expected: `fb2323c372fff2bbe080055f6696e2d8f0420c4f1828a4204ed753da8169d95a`, `208`, `syntax-ok`.

**Write the reboot wrapper** exactly as below, then check it:

```sh
D=/var/tmp/task-1123-is5b
cat > $D/is5b-reboot.sh <<'IS5BREBOOT_END'
#!/bin/bash
# is5b-reboot.sh <tag> [ff]
# The pre-approved reboot of step 5, recorded before it is made.  It runs
# only if run <tag> started in this boot and ended in a PANIC.  It appends
# to reboots.log (and to the journal, at priority crit) the time, the boot
# id, the uptime and the command it is about to run, syncs the run's
# verdict, the module file and that log (ext4 only, never ZFS), and then
# runs the command: SysRq b, or with "ff" "systemctl reboot -ff".
# Runs as root.
set -u
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b
T=$1; R=$L/reboots.log
if [ "${2:-}" = ff ]; then C="systemctl reboot -ff"; else C="echo b > /proc/sysrq-trigger"; fi
now() { date -u +%FT%T.%3NZ; }
K=$(dmesg | sed -n "/task-1123-is5b: run $T start/,\$p")
if [ -z "$K" ] || { ! grep -q ': PANIC after' "$L/run-$T/verdict.txt" 2>/dev/null &&
    ! printf '%s\n' "$K" | grep -q 'PANIC at'; }; then
	echo "$(now) run $T: refused, no PANIC of this run in this boot" | tee -a "$R"
	exit 2
fi
M="run $T: boot_id $(cat /proc/sys/kernel/random/boot_id) uptime $(cut -d' ' -f1 /proc/uptime) about to run: $C"
echo "$(now) $M" >> "$R"
logger -p user.crit -t task-1123-is5b "$M"
for f in "$L/run-$T/verdict.txt" "$D/zm/module/zfs.ko"; do
	timeout 30 sync -f "$f"; rc=$?
	echo "$(now) run $T: sync -f $f rc=$rc" >> "$R"
done
sleep 1
timeout 30 sync -f "$R" || { echo "$(now) run $T: log not synced, stop" >&2; exit 1; }
if [ "${2:-}" = ff ]; then
	systemctl reboot -ff
else
	echo b > /proc/sysrq-trigger
fi
IS5BREBOOT_END
chmod 755 $D/is5b-reboot.sh
sha256sum $D/is5b-reboot.sh | cut -c1-64; wc -l < $D/is5b-reboot.sh; bash -n $D/is5b-reboot.sh && echo syntax-ok
sudo -n bash $D/is5b-reboot.sh none; echo rc=$?
```

Expected: `cd49f6bcbdd5be3a1a7eeed6f89f9460ecd8dc7ee8cf8516f47d3ceae0bd47e6`, `34`, `syntax-ok`, then a line ending in `run none: refused, no PANIC of this run in this boot` and `rc=2` (the wrapper's refusal, tested on a run that does not exist; it also lands in `reboots.log`). A different sum for either script is a stop (the transport changed the text): report the first lines that differ if you can see them. Taking a script out of this file with `sed` rather than pasting the heredoc is fine: the sha256 decides.

## Step 4: the runs, one at a time, in this order

For each run, after the pre-run checks:

```sh
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b; T=<tag>
setsid nohup sudo -n bash $D/is5b-run.sh $T <mode> > $L/run-$T.log 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/run-$T/steps.txt` every 10 s until it ends with a line `run $T end ...` (about 1 minute for a PANIC, 1 to 2 minutes otherwise).

A run is **VALID** when the last line of its `verdict.txt` ends with `a restarted resilver ends before the attach txg: yes`, and **NOT A TEST** otherwise. In a PANIC run, the attach txg of that line must also equal the left number of the `VERIFY3U` line. The control m-eio has no restart to look for: it is a test when its failed I/Os include `5/type2` with `repair` above 0.

| order | tag | arm | mode | when | expected |
|---|---|---|---|---|---|
| 1 | m-eio | zm | `eio` | always | NO PANIC; `sd max_retries of <disk>: 0`; failed I/Os with `5/type2` (EIO) and `repair` above 0; one resilver; replace done; the scrub finds CKSUM > 0 on the new disk |
| 2 to 7 | f-enodata1 to f-enodata6 | F (step 6 first) | `enodata` | until 3 of them are VALID; f-enodata4 to 6 only if at most 50 minutes have passed | NO PANIC; a VALID run: two resilvers, the second ending before the attach txg; replace done; scrub CKSUM 0 on the new disk |
| 8 to 10 | p-eio1 to p-eio3 | P (step 6 first) | `eio` | only if run 1 showed failed I/Os with `5/type2`, and at most 58 minutes have passed when step 6 for P would start; then until one run is VALID or PANICs; p-eio2 and 3 only if at most 62 minutes have passed | PANIC at `vdev.c:3404` in a VALID run |

**A run whose verdict is PANIC continues at step 5**, then comes back here for the next run if there is one. A PANIC in an F run is a finding (the fix failed, or another assertion): after step 5, skip the rest of the F arm and go on with P.

After each run, quote:
- `$L/run-$T/steps.txt` in full, and `$L/run-$T.log` if it is not empty;
- `verdict.txt` (three lines for a PANIC, four otherwise), and VALID or NOT A TEST with its reason (from `dbgmsg-scan.txt`: the restarted resilver's range; from `zio-errors.txt`: the size of the failed write whose birth txg set its end);
- `dbgmsg-scan.txt` and `zio-errors.txt` in full, and `inject-after.txt`;
- for a NO PANIC run: from `status-1.txt` and `status-2.txt`, the config section and the `scan:` line; `failfast.txt`; `scrub.txt`, `destroy.txt`, `rm-scsi_debug.txt`, `rm-null_blk.txt`, and the `left (empty is clean):` line, which must be empty after the colon;
- the kernel-line check since the run's mark, and the counts of `zio pool=is5pool` lines, SCSI error lines and `fail command` lines;
- `lsmod | grep -E '^(scsi_debug|null_blk) '` (empty after a NO PANIC run).

In an `eio` run, each `fail command` line should now come with an `I/O error, dev` line and at least one `zio pool=is5pool ... error=5` line (a merged command can fail several zios); IS5 had 4 `fail command` lines and no failed I/O. A failed I/O counted as `probe`, or a failed `ordinary` write, also makes a run NOT A TEST when its restarted resilver reaches the attach txg; say which.

## Step 5: after a PANIC (the reboot is pre-approved here, once per PANIC run)

1. **Quote** from `$L/run-$T`: `verdict.txt`; from `kernel.txt`, the `VERIFY3U` line, the `PANIC at` line and the stack that follows (through the end of the `Call Trace`), in full; `ps-D.txt` in full; from `stacks.txt`, the stack of `txg_sync` in full and the first three frames of every other; `txgs.txt`; `dbgmsg-scan.txt`, `zio-errors.txt` and `inject-after.txt` if not quoted yet. Do not wait for the hung-task watchdog.
2. **Reboot, recorded first.** Run exactly this one command line. It appends the command to `reboots.log` and syncs that file (ext4) before running the wrapper. The wrapper checks the PANIC, appends its own line (time, boot id, uptime, `about to run: echo b > /proc/sysrq-trigger`) to `reboots.log` and to the journal at priority crit, syncs the verdict, `zfs.ko` and the log, and only then triggers SysRq `b`:

   ```sh
   D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b; T=<tag>
   echo "$(date -u +%FT%T.%3NZ) operator runs: sudo -n bash $D/is5b-reboot.sh $T" >> $L/reboots.log && sync -f $L/reboots.log && sudo -n bash $D/is5b-reboot.sh $T
   ```

   If the wrapper refuses (exit 2), do not reboot by any other means: report it. If the VM is not down within 2 minutes, run the same line with `$T ff` in place of `$T` (the wrapper then logs and runs `systemctl reboot -ff`). If the VM is not back within 10 minutes, stop and report.
3. **After the reboot,** first append and sync one line: `L=/var/log/task-1123-is5b; echo "$(date -u +%FT%T.%3NZ) after reboot: boot_id $(cat /proc/sys/kernel/random/boot_id) uptime $(cut -d' ' -f1 /proc/uptime)" >> $L/reboots.log && sync -f $L/reboots.log`. Then quote:
   - `$L/reboots.log` in full;
   - `journalctl --list-boots --no-pager | tail -n 3`;
   - `journalctl -b -1 -t task-1123-is5b --no-pager -o short-iso-precise` (the wrapper's crit line, if journald kept it), and `journalctl -b -1 -n 3 --no-pager -o short-iso-precise` (the previous boot's last lines);
   - `uptime`; `uname -r` (as in step 0); `lsmod | grep -E '^(zfs|spl|scsi_debug|null_blk) '` (empty); `ls -l /dev/zfs /dev/is5a /dev/is5b 2>&1` (none); `ls -l /etc/zfs/zpool.cache 2>&1` (as in step 0); `ls -l $L/run-$T/verdict.txt $L/run-$T/kernel.txt` (present, not empty);
   - `cd $D/zm && git rev-parse HEAD && git write-tree && git status --porcelain --untracked-files=no` (the current arm's values) and `modinfo -F srcversion $D/zm/module/zfs.ko` (the current arm's saved srcversion).
4. If the next run is on the same arm: `sudo -n $D/zm/scripts/zfs.sh`, then step 3's checks (srcversion equal to the file's and to the saved one, "(DEBUG mode)", `spl_panic_halt` 0), and back to step 4. If it is on another arm, go to step 6 (nothing is loaded). If there is no next run, go to step 7.

## Step 6: switch arm (before f-enodata1 with A=F, before p-eio1 with A=P)

```sh
D=/var/tmp/task-1123-is5b; L=/var/log/task-1123-is5b; A=<F|P>; C=<commit>
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

Any other difference is a stop for that arm: skip its runs and report. The rebuild recompiles `vdev.c`, `spl-proc.c` and `spa_history.c` (the tree is dirty now, which changes `zfs_gitrev.h`) and relinks, about 20 s to 4 minutes; poll `grep -H build-rc= $L/build-$A.log`, which must read `build-rc=0`; quote its duration and any `warning:` line. Then:
- if zfs is loaded: `timeout 30 sudo -n $D/zm/zpool list -H -o name` must show no pool, then `sudo -n $D/zm/scripts/zfs.sh -u` and `lsmod | grep -E '^(zfs|spl) '` must be empty;
- `sudo -n $D/zm/scripts/zfs.sh`; `cat /sys/module/zfs/srcversion` must equal `modinfo -F srcversion $D/zm/module/zfs.ko` **and differ from every srcversion saved before** (else the rebuild did not take: stop for that arm); save it in `$L/srcversion-$A.txt`; "(DEBUG mode)"; `spl_panic_halt` 0. Back to step 4.

## Step 7: cleanup

- If zfs is loaded: no pool left (`timeout 30 sudo -n $D/zm/zpool list -H -o name` empty), then `sudo -n $D/zm/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.
- `lsmod | grep -E '^(scsi_debug|null_blk) '` and `ls -l /dev/is5a /dev/is5b 2>&1`: must show nothing. If something is left, quote it and leave it.
- `lsmod | grep -E '^(configfs|sd_mod|scsi_mod|scsi_common) '` and `mountpoint /sys/kernel/config 2>&1`: quote them next to step 0's; leave them as they are.
- `grep -c task-1123-is5b /proc/mounts`: must be 0.
- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only `task-1123-is5b` (`sudo -n rm -rf /var/tmp/task-1123-is5b`). Anything else new: quote it and leave it.
- `/etc/zfs/zpool.cache`: quote `ls -l` of it; it must be as in step 0.
- `$L/reboots.log` in full, once more.
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-is5b`.

## Report

Write `/var/log/task-1123-is5b/report.md` and give the same text as your final message:

1. **Environment:** kernel, nproc, RAM, free space, the module checks, lockdown, the kernel sysctls, the journal's boots and the zfs service states of step 0, `trees.txt`, the log line, the blob and the code lines of step 1, the build rc, duration and warnings of each build, each arm's srcversion with "(DEBUG mode)", `spl_panic_halt`, `zfs_vdev_failfast_mask`, the two scripts' sha256.
2. **Table:** tag | arm | mode | sd max_retries | verdict (PANIC or NO PANIC, after how many seconds) | VALID or NOT A TEST (and why) | failed I/Os (repair / ordinary / probe, errno) and `fail command` lines | attach txg; resilver ranges | new disk after the resilver; after the scrub | reboot.
3. **Verdict lines:**
   - `#19906 (vdev.c of 3c465f0a721) on master: <v> VALID runs of <n>, <k> of them without a panic; each VALID run: restarted resilver (<a>, <b>] ending before attach txg <R>, replace done, scrub CKSUM <c> on the new disk` (one clause per VALID run), then the NOT A TEST runs with their reason. With IS5's f-enodata1, that makes <v+1> VALID runs.
   - `EIO write errors (sense 3/0C, sd max_retries 0): master: <NO PANIC, replace done, <x> failed repair writes with EIO, the scrub found <c> blocks missing on the new disk | what happened>; #20003 (vdev.c of b4578f4312d): <PANIC at vdev.c:3404 (<R> <= <M>) in run <tag> | what happened>`.
   - `Reboots: <n>, each recorded before it was made: <time of the "about to run" line> -> <boot_id before> / <boot_id after>` (one per reboot), or `Reboots: none`.
   - The `failfast` property ZFS reports for the new disk (`failfast.txt`).
4. **Quotes:** per run, what step 4 lists, step 5's quotes and checks for each PANIC, step 6's outputs, and the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
