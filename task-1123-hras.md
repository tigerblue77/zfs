TASK-1123-HRAS (from the coordinator session "main"): a functional smoke test, NOT a measurement, of the host rig's kit v2.2 on VM 105 (CHSONVTST02) booted once into 7.0.14-25-pve, about 55 min on the VM, at most 1 h 45. Kit v2.2 changes only the page store of `hddsim`, the emulated hard disks, so that arm P1I fits in it; it has only run in user space so far. This task runs it as the real ublk disks, built by the VM's compiler, brings up the rig and its pool exactly as the HRA passes do, runs arm P1I's load once for its full duration under a guard that stops the load before the store can fill, then reads data back, scrubs the pool, and restores the VM. Its verdict feeds the owner's decision on whether tonight's HRA passes use kit v2.2. Two reboots are pre-approved for this task only, and no other: into 7.0.14-25-pve at step 3 (`grub-reboot`), back to the default entry, Debian 6.12.118, at step 9; for this task that replaces the standing approval of 18/11. It may run next to functional work on the other VMs, never next to a performance measurement (it loads the host's CPU for about 5 minutes). Nothing is published on GitHub, no credential files, no package installed, fstrim at the end. Report: `/var/log/task-1123-hras/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. The final report is sent after the reboot of step 9.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `T=/var/tmp/task-1123-hras`, `L=/var/log/task-1123-hras`, `R=/var/tmp/hra-trees` (the HRA trees of 22/11, read here and never changed); the blocks below set them.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs, except where this task says that a value is a finding (then it is reported, not a stop).

## Why

- **Last night:** TASK-1121-HRA1's pass A1 stopped at arm P1I (PR1 with `zfs_immediate_write_sz=2097152`). P1I started at 23:14:42Z; at 23:16:18Z, about 87 s into the guest's load, each disk's page store (`store_mb=512`, 512 MiB) was full, the disks refused writes (`err`/`rej` about 2700 per disk), the pool suspended, and the VM was rebooted.
- **The cause:** P1I copies every guest write into the ZIL, whose record headers shift the guest's patterned pages, so a log sector holds the end of one guest page and the start of the next: two runs of an 8-byte word, never one. Kit v2.1's store kept a page compact only when it repeats its first 8 bytes, so it kept almost every log sector in full: its `full` count grew by about 1,180 to 1,270 pages a second per disk, about 5,000 KiB/s.
- **Kit v2.2** keeps a page made of at most four runs of one 8-byte word in 40 bytes, within the same `store_mb=512` per disk (the runs and the full pages together), and each statistics line adds `runs` (pages kept as runs) and `store_kib` (the memory the store has mapped, both kinds of slot; it never unmaps). The disk model is unchanged: `selftest` and `priotest` print the same as kit v2.1 on every grid point, and every other file of the kit is kit v2.1's. It was tested only in user space, with gcc 13.3: 300,000 random store operations with read-back, the same under ASan and UBSan, and a simulated P1I log stream, which it holds in 8.4 % of kit v2.1's bytes. It has never served a ublk disk, been built by Debian 13's gcc, or carried a real P1I load.
- **Projection** for P1I on this rig, from last night's growth: each disk's `store_kib` ends about 96,000 KiB above its value after the pool, at most about 292,000 above it if ZFS never reused freed log space; MemAvailable stays about 2.8 GiB, at worst about 1.85 GiB, above the driver's 1024 MiB check.
- **This task answers:** does kit v2.2 build on the VM without warnings and model the same disk; does one full P1I run stay inside the store with margin, with no refused write, no suspension and enough memory; are pages kept as runs read back intact through ublk, raidz2 and its parity; and does the HRA route to 7.0 still boot (VM 105 moved its GRUB default to 6.12.118 on 23/11, after the last 7.0 boot).

## What you fetch

| What | Source | Commit | Tree |
| --- | --- | --- | --- |
| kit v2.2 | https://github.com/tigerblue77/zfs, by commit (branch `vm-kit-hra22`) | `964c0afff7ab58e7867f808c24fab14c0961799b` (no parent) | `86b41cf24b89622b18946cb4a26b016f5e623b44` |

No tree is fetched or built. The HRA trees in `/var/tmp/hra-trees`, built for 7.0.14-25-pve on 22/11, are checked (`hr-trees.sh check`, read-only) and two are used: R (OpenZFS 2.4.4, commit `71a9f9578616a90c3c14bb59629fb4d31bfd68d1`) for the disks and the pool, P (PR1, commit `846b9194b4a3eb54f601c1bda0382b68d785b0ef`) for arm P1I. Tonight's HRA tasks reuse these trees: never fetch, build, edit or remove anything in `/var/tmp/hra-trees`.

## The rig and the run

- **The rig** is TASK-1121-HRA1's, driven by the kit's `hr-bench.sh`: disks `/dev/ublkb70` to `/dev/ublkb74` (16 GiB, 4K, rotational, volatile write cache, scheduler `none`, blk-wbt 75000), the point `grid/G7.conf` (sha256 `538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8`, `store_mb=512`) as `$L/best.conf`, the guest at its defaults (`-`), pool `hrp` (raidz2, ashift 12, compression off, `cachefile=none`, zvol `hrp/vm` 1M filled whole, filler to 66 %), built on fast disks with tree R. HRA's probe, functional run and armcheck are not run here.
- **The run:** arm P1I only (tree P, blk-wbt 75000, `ncq_prio` 0, `zfs_vdev_disk_sync_writes=1 zfs_immediate_write_sz=2097152`), once, as pass `S1`: `hr-bench.sh point S1 P1I`, HRA's run (the switch: export, tree P loaded, parameters, import, prewarm, the model; then 45 s of ramp and 180 s measured, 225 s of guest load). Last night's stores were full about 87 s into that load.
- **The guard** (`$T/guard.sh`, written at step 6, below) runs from just before the run to the end of step 7's write. Once a second it writes to `$L/guard.log` the time, MemAvailable, and for each disk `full`, `runs`, `store_kib`, `err`, `rej` from its last statistics line and the `E` lines among its last five. At the first stop condition (any disk's `store_kib` above 440,000, `err` or `rej` above 0, an `E` line, fewer than five live `hddsim` servers, or MemAvailable below 1024 MiB) it writes `TRIP` lines and stops the guest's load (`pkill -TERM -f 'hr-load.py guest'`), never an `hddsim` server. The driver then ends the run with `STOP: a load stream failed (rc 143)` once its sampler's 225 s are over.
- **Why 440,000 KiB:** the store holds 524,288 KiB per disk; a write that needs a slot beyond it is refused, and the pool suspends. 440,000 KiB is 84 % of it and leaves 84,288 KiB, about 16 s of last night's growth, for the guard (1 s), the guest to stop and the log writes in flight. It lies above the projection's worst case (about 375,000 KiB at the end of P1I if the pool leaves about 83,000), so it trips only on a store that does worse than kit v2.2 was designed for.
- **The margins PASS asks for:** every disk's `store_kib` at most 393,216 KiB (75 % of the store) over the run, and MemAvailable at least 1536 MiB throughout (the driver's 1024 MiB check before each HRA run, plus 512 MiB for the arms after P1I in a pass: the store never gives memory back).

## Expected (written before the run; report what you see, not this)

| Where | What | Expected | If not |
| --- | --- | --- | --- |
| step 4 | `hddsim built: N warnings` (Debian 13's gcc, `-O2 -Wall`) | 0 | stop: FAIL |
| step 4 | `sha256sum $L/selftest.txt $L/priotest.txt` | `2cbbd8c6d57b89db7003514545198a9b72106d911ea8e506cf62ab63b93cbd04` and `a2de6280d86ab288f37dc5d7ac367908c0d4dfa19f41015f63652bd304b06de3` (kit v2.2 and v2.1 here, identical; the selftest also equals calibration's `selftest-G7.txt`) | see step 4 |
| step 5 | `pool done` | size about 79.5G, cap 60 to 68 %, frag 20 to 45 % (else a finding), MemAvailable at least 2500 MiB (22/11: 3284) | MemAvailable: stop |
| step 5 | the store lines | `full` below 50,000 per disk (22/11 with kit v2.1: 20,108 to 20,143); `store_kib` about 80,000 to 160,000 (a finding above); `err 0 rej 0` | `err`/`rej`: stop |
| step 6 | the run | `point S1 P1I done`, guest `END` and no `ERROR`, guest media near 50 MB/s (a finding below 45) | stop path |
| step 6 | each disk's peak `store_kib` | about 180,000 KiB (the projection at last night's rate), at most about 375,000 (its worst case); PASS at most 393,216 | above 440,000: the guard trips |
| step 6 | `full` and `runs` (orientation only) | `full` up by at most about 22,000 pages per disk; `runs` up by up to about 260,000 (less where ZFS reuses freed log space) | |
| step 6 | lowest MemAvailable | about 2,800 MiB, at worst about 1,900; PASS at least 1536 | below 1024: the guard trips |
| steps 4-9 | the disk check | its expected output every time | stop path |
| steps 6-8 | the pool's state (lock-free read) | `ONLINE` | stop path |
| step 6 | `calib-s1.md`: V1 and `models` | V1 PASS (completions late by more than 2 ms at most 1 %, store errors 0), models `1` | store errors: FAIL; lateness alone: a finding |
| step 7 | the read-back | the file's sha256 `0793dcd6531dfd8a13639ca8d8154c0dbb46ba90ab23bedd2f66e8d00539a61c` written and read back the same; `runs` up by about 8,000 and `full` by about 2,700 per disk (orientation only) | FAIL |
| steps 7, 8 | `zpool status -v hrp` | every vdev `0 0 0`, `errors: No known data errors`; the scrub `repaired 0B ... with 0 errors`, in about 2 to 10 min (never timed on this rig) | FAIL |

**Verdict** (the report's first line, exactly one of these):

- `TASK-1123-HRAS VERDICT: PASS`: the kit checks, 0 warnings, selftest and priotest as expected; the 7.0 boot; the disks and the pool as expected; the run done with no `TRIP`, every disk check as expected, the state `ONLINE`, V1's store errors 0 and models `1`; every disk's peak `store_kib` at most 393,216 and the lowest MemAvailable at least 1536 MiB; the read-back identical, the scrub and both statuses with 0 errors; a clean teardown, back on 6.12.118.
- `TASK-1123-HRAS VERDICT: PASS, LOW MARGIN (<which margin, with its value>)`: all of PASS except the margins (a peak `store_kib` from 393,217 to 440,000, or a lowest MemAvailable from 1024 to 1535 MiB).
- `TASK-1123-HRAS VERDICT: FAIL (<reasons>)`: hddsim warnings, selftest or priotest not as expected, a `TRIP`, a refused write, a store error, an `E` line, a suspension, a hang, a kernel error line during the run, a read-back that differs, or a checksum or scrub error.
- `TASK-1123-HRAS VERDICT: NOT RUN (<reason>)`: a stop before the run that says nothing about kit v2.2 (the VM's state, the trees, the fetch, the 7.0 boot).

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed.
- The pool lives on `/dev/ublkb70` to `/dev/ublkb74` only; the driver creates them and the pool and removes them. Touch no other pool, disk or ublk device; never `zpool import -a` (the driver imports `hrp` by name from the five disks only); the pool has `cachefile=none`.
- The ZTS test `zpool_import_all_001_pos` once imported VM 105's exported measurement pool `tank`, which may still be on this VM's disks, exported. Nothing in this task imports it; the driver stops (`STOP: a pool this task did not create is imported`) if `zpool list` shows any pool but `hrp`. Such a pool is a stop: report it and leave it as it is.
- `/etc/zfs/zpool.cache`: steps 0, 3 and 9. A non-empty one names a pool the module would import at load: never load the module while one exists.
- Leave `/var/log/task-1121-hra1` (last night's attempt, which tonight's HRA1 moves aside), `/var/tmp/hra-trees` and every other HRA directory as you find them.
- **The disks are user processes.** If an `hddsim` server dies while the pool is imported, every I/O of the pool fails and the pool suspends; `zpool` commands then hang. Never kill or signal an `hddsim` process yourself, never run `hddsim del`, and never stop `ublk_drv`, except through `hr-bench.sh teardown`. The guard stops only the guest's load.
- **The disk check** is the block below, read-only (TASK-1121-HRA1's, with this task's directory). Expected: `5`; five lines `70 E 0 err 0 rej 0` to `74 E 0 err 0 rej 0`; `0`; `0`. Anything else is the stop path: fewer than five servers, an `E ` line, a non-zero `err` or `rej`, `rej -`, a line after `ready` in a `.out` file, or a kernel I/O error (`error, dev ublkb7N`) since this task's start marker.

```
L=/var/log/task-1123-hras
ps -C hddsim -o pid= | wc -l
for i in 70 71 72 73 74; do awk -v d=$i '/^E / {e++} /^S / {for (k = 6; k < NF; k++) if ($k == "err" || $k == "rej") v[$k] = $(k + 1)} END {printf "%s E %d err %s rej %s\n", d, e, ("err" in v) ? v["err"] : "-", ("rej" in v) ? v["rej"] : "-"}' $L/dev/$i.stats; done
for i in 70 71 72 73 74; do sed -n '/ready/,$p' $L/dev/$i.out | sed 1d; done | wc -l
sudo -n dmesg | tac | sed '/task-1123-hras: start/q' | grep -c 'error, dev ublkb'
```

- **The store lines** (with `G` set to the tag the step names): each disk's last statistics line, reduced, and MemAvailable, also saved to `$L/store-<tag>.txt`.

```
L=/var/log/task-1123-hras; G=pool
{ for i in 70 71 72 73 74; do grep '^S ' $L/dev/$i.stats | tail -n 1 | awk -v d=$i '{for (k = 6; k < NF; k++) if ($k ~ /^(full|runs|store_kib|err|rej)$/) v[$k] = $(k + 1); printf "ublkb%s full %s runs %s store_kib %s err %s rej %s\n", d, v["full"], v["runs"], v["store_kib"], v["err"], v["rej"]}'; done; awk '/^MemAvailable/ {print "MemAvailable", int($2 / 1024), "MiB"}' /proc/meminfo; } | tee $L/store-$G.txt
```

- **The pool's state, lock-free** (with `G` set as the step says): it never blocks, even on a suspended pool. Read it before every `zpool` command of steps 7 and 8. Expected `ONLINE`; `SUSPENDED`, anything else, or an empty file after 3 s is the stop path.

```
L=/var/log/task-1123-hras; G=s1
(sudo -n cat /proc/spl/kstat/zfs/hrp/state > $L/state-$G.txt 2>&1 &); sleep 3; cat $L/state-$G.txt
```

- Every setup step, the run and the scrub run detached (`setsid nohup ... &`) and are polled. A hang is: `disks` after 5 minutes; `pool` after 30 minutes; the run after 10 minutes, or `driver.log` unchanged for 10 minutes while it runs; step 7's `arm` after 5 minutes; the scrub after 30 minutes; `teardown` after 5 minutes. Then the stop path.
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, the kit's file list, a mode or a hash differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- If the driver stops (`STOP:` in driver.log), or the guard writes a `TRIP` line: the stop path. Never retry the run.

## The stop path (steps 4 to 9)

1. `touch /var/log/task-1123-hras/guard.stop` (if the guard runs). Capture, with no `zpool`, `zfs` or `zdb` command, since each blocks for good on a suspended pool:

```
L=/var/log/task-1123-hras
{ date -u; tail -n 20 $L/driver.log; grep '^TRIP' $L/guard.log; tail -n 3 $L/guard.log; ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zpool|zvol|python3|hddsim|kworker'; tail -n 3 $L/dev/7*.stats | cut -c1-700; cat $L/dev/7*.out; grep -E 'MemAvailable|MemFree' /proc/meminfo; } > $L/stop.txt 2>&1
for p in $(ps -eLo tid=,stat= | awk '$2 ~ /^D/ {print $1}'); do echo "== $p $(cat /proc/$p/comm)"; sudo -n cat /proc/$p/stack; done >> $L/stop.txt 2>&1
sudo -n dmesg | tail -n 150 >> $L/stop.txt; sync -f $L/stop.txt; wc -l $L/stop.txt
```

   Then the lock-free state read with `G=stop`, the disk check, and the store lines with `G=stop`.
2. **The pool is `ONLINE` or `DEGRADED`, or there is no pool yet** (the state file absent before step 5's `pool done`), and a second `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D'` 60 s later shows no `zpool`, `txg_sync`, `zvol` or `z_` thread in D state: if the driver runs, wait for it to end (`STOP:` in driver.log; for the run, at most its 225 s plus 2 minutes from its start), then step 9 from its `teardown`, skipping what has not run.
3. **Otherwise** (`SUSPENDED`, an unreadable state, D-state threads that stay, or `teardown` hung): no `zpool` command and no `teardown`. Run step 9's `dmesg` line (into `$L/kmsg.txt`) and `sync -f $L/kmsg.txt`, then the reboot of step 9 at once and forced, since a graceful one can hang on the suspended pool: `sudo -n systemctl reboot -ff`. That is the second pre-approved reboot, not a third. After it, step 9's checks after the boot, then the report.

## Steps

### 0. State, on Debian 6.12 (T0 = when you start this step)

```
echo "T0=$(date +%s)"; date -u; hostname; uname -r; cat /proc/cmdline; nproc; grep -m1 'model name' /proc/cpuinfo; free -m; df -h / /var/tmp; uptime -s; swapon --show
lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; ls -l /dev/zfs /dev/ublk-control 2>&1; ls /dev/ublkb* /dev/ublkc* 2>&1; losetup -a; ls -d /var/tmp/task-1123-hras /var/log/task-1123-hras 2>&1
ls -d /var/tmp/hra-trees /var/log/task-1121-hra? /var/log/task-1121-hra1-1122 /var/log/task-1122-hrig3 2>&1; ls /var/tmp/hra-trees 2>&1; cut -d' ' -f1-3,6-9 /var/tmp/hra-trees/*.stamp 2>&1
grep -E 'CONFIG_BLK_DEV_UBLK|CONFIG_BLK_WBT=|CONFIG_BLK_WBT_MQ|CONFIG_IO_URING=|CONFIG_PSI=' /boot/config-7.0.14-25-pve
ls -d /usr/src/linux-headers-7.0.14-25-pve /lib/modules/7.0.14-25-pve/kernel/drivers/block/ublk_drv.ko* 2>&1; grep -c "7.0.14-25-pve" /boot/grub/grub.cfg; sudo -n grub-editenv list; grep -E '^GRUB_(DEFAULT|SAVEDEFAULT)=' /etc/default/grub
command -v python3 git make gcc modinfo; python3 --version; gcc --version | head -1; git --version; ls /usr/include/linux/io_uring.h; pgrep -a -x zed || echo NO-ZED
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|bpftrace|hddsim'
ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs, spl, ublk_drv or null_blk module is loaded, `/dev/zfs` or a `/dev/ublk*` exists; `losetup -a` is not empty; `$T` or `$L` exists; less than 6000 MB available; /var/tmp has less than 5 GiB free; /proc/cmdline holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`; `/boot/config-7.0.14-25-pve` lacks `CONFIG_BLK_DEV_UBLK=m` or `=y`, or `CONFIG_BLK_WBT` or `CONFIG_PSI`; the 7.0 headers or the 7.0 `ublk_drv` module are missing; no grub entry names 7.0.14-25-pve; `grub-editenv list` shows a `next_entry`; python3 is older than 3.8; a build, test or load runs; `/var/tmp/hra-trees` or its `R.stamp` or `P.stamp` is missing.
Expected: `uname -r` 6.12.118 (the default entry since 23/11; another 6.12 kernel is noted, not a stop); `/var/tmp/hra-trees` with the seven stamps of 22/11 (R, M, P, A, RQ, PQ, P3W; a missing one other than R or P is noted for tonight, not a stop); `/var/log/task-1121-hra1` present (last night's attempt; leave it).
Note, without stopping: swap, zed (do not start or stop it), the GRUB lines. `/etc/zfs/zpool.cache`: absent is expected; size 0: `L=/var/log/task-1123-hras; sudo -n mkdir -p $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0` and say so; size above 0: stop and report its `ls -l`, load nothing.

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras
sudo -n mkdir -p $T $L/bin && sudo -n chown "$(id -un)" $T $L $L/bin
```

### 1. Kit v2.2, fetched by commit from the fork

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras
cd $L/bin && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs 964c0afff7ab58e7867f808c24fab14c0961799b
test "$(git rev-parse FETCH_HEAD)" = 964c0afff7ab58e7867f808c24fab14c0961799b && echo kit commit OK
test "$(git rev-parse 'FETCH_HEAD^{tree}')" = 86b41cf24b89622b18946cb4a26b016f5e623b44 && echo kit tree OK
test "$(git cat-file -p FETCH_HEAD | grep -c '^parent')" = 0 && echo kit parent OK
git checkout -q FETCH_HEAD && git ls-files -s | awk '{print $1, $4}'
printf '%s  %s\n' \
    e60b3d3de307803d9ee842dab806c1878d80a174355b300274728053b0cbc349 arms.txt \
    f79f57a7cec57a602c0c71acad38f1803af1d63ed0a1558c5ca91429142f056b grid/G1.conf \
    eec8a376ff13d234b3c14935f0ec4e25242c4cb4d7ceea5f9509f014daf42256 grid/G2.conf \
    bb601f6e7c4ead34391e2e8d55ffa8a210b250fc298243ce39cd2349bebd537f grid/G3.conf \
    d879b967ac50153f9b52f5ef5cbe6bcc44beb0154c4a173820e35c8084ce0b93 grid/G4.conf \
    0b721c92bb9fc53f4cc257a1a2f2ac591e0c48871c1e06310c31efabd340ad4b grid/G5.conf \
    3ee3cb3731b5323064c03ea239185fa63ad3a157ba2d7a1a185796b39147285b grid/G6.conf \
    538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8 grid/G7.conf \
    fa7ec5a6b920725373c83e78321824294366859058633fa35a892fc213dfb875 grid/G8.conf \
    ba2267bacf0bf60dc531c2d0431633737017553b17a761fbdfd08844a856b251 grid/fast.conf \
    c1af827f2aa6610b9ab828eb2b047de20cb8fea699dbc1c1018d36e8ed7369c7 hddsim.c \
    1de5d21aaadc484978ec4b65fdbd4824071a47b79b2e4833ebe959a7d9f08cbc hr-bench.sh \
    b259d0f15f7df96326179564088e6f80433c501eca9723d69770f20e2ce44669 hr-load.py \
    1bef26029e0a3cb455e96cc697cf0e209371242a121a728f2d0e230108bc7fd3 hr-report.py \
    f6df70024895b41f9ddff492ed127e6e2a60d69d0301f6c53f8737535b11aa46 hr-sample.py \
    86c60b1db5b6f5c09f0b816d48925faccdd6f1e6a4317656078662c2eb56011a hr-trees.sh \
    b9e80d514452154be77af2246c5f2ae9343e94f6d3dac68ed8a66fd7e6db1cdc patches/p3w-244/0001-vdev_disk-keep-blk-wbt-off-sync-writes-and-flushes.patch \
    72e52afa7b42f2ba8568111e7ef1a8c048a38e2d522a192043dc31de8c1fb5af patches/p3w-244/0002-vdev_disk-add-zfs_vdev_disk_sync_writes.patch \
    298eb484a36e3570f495f927f8bf0d4811382cd9e5b97664e17562d9f68781a2 patches/p3w-244/0003-vdev_queue-add-zfs_vdev_queue_window-capping-active-.patch \
    5b54875c268b72d913be43074f192c3f4b6ec43a09af2948f3a8dedd8b9297cc patches/p3w-244/0004-vdev_queue-adjust-zfs_vdev_queue_window-to-read-late.patch \
    c41836711d2aac9d4dd734e4101c85df87ded0f920443a49a842124273785ebe patches/p3w-244/0005-vdev_queue-trace-the-window-of-zfs_vdev_queue_window.patch \
    e5a2957ab1c02c544fea28c33af277cba6ba3442e875c0214113685e1f60d0a5 patches/p3w-244/0006-vdev_disk-2.4-test-backport-zfs_vdev_disk_sync_write.patch \
    69915cc83e61254f1ac9e663a71cdeda947a7e1ebc3f9ad21dd2d660bf03d48b patches/pr1-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch \
    f62b946632a92b2dc58c21ce9809c4df5892d5ef290da1046ffef6c0e5a80504 patches/pr1-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch \
    5b00170ec7d4e69b6e226d752a3ad1038a4c7930ecc44e44d1bfce0e229472df patches/r244-rt/0001-vdev_disk-add-zfs_vdev_disk_rt_mask.patch \
    87a68f49a3ea7e54cbf56cd9583f20d618627cbe3cad5ca0c9c2bd98805df095 patches/r244-rt/0002-RIG-ONLY-carry-the-real-time-class-to-ublk-as-REQ_ME.patch \
    81d573ba59644ce1a9381cb3f345f5950d73a99c4ade4f37de731327dc45d776 plan.txt > $T/kit.sha256
sha256sum -c $T/kit.sha256 && rm $T/kit.sha256
bash -n hr-bench.sh && bash -n hr-trees.sh && echo syntax-ok; git status --porcelain | wc -l; ls -l hr-bench.sh hr-trees.sh
cp grid/G7.conf $L/best.conf && echo "538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8  $L/best.conf" | sha256sum -c
echo '-' > $L/best.guest && cat $L/best.guest
```

Expected: the three `OK` lines; 27 entries, all `100644` but `100755 hr-bench.sh` and `100755 hr-trees.sh` (the list of TASK-1121-HRA1's step 1); 27 `OK` from `sha256sum -c`; `syntax-ok`; 0; `-rwxr-xr-x` twice; `best.conf: OK`; `-`.

### 2. The trees, checked only (on 6.12)

```
L=/var/log/task-1123-hras; R=/var/tmp/hra-trees
cd $L/bin && bash ./hr-trees.sh check $R > $L/trees-check.txt 2>&1; echo rc=$?; grep -v ' OK$' $L/trees-check.txt
for n in R P; do echo "$n: $(modinfo -p $R/$n/module/zfs.ko | grep -oE '^(zfs_vdev_disk_sync_writes|zfs_immediate_write_sz):' | tr -d : | sort | tr '\n' ' ')"; done
sha256sum $R/*.stamp > $L/stamps-0.txt; wc -l < $L/stamps-0.txt
```

Expected: rc=0 and one line `<name> <name> commit ... tree ... srcversion ... vermagic 7.0.14-25-pve kdir /usr/src/linux-headers-7.0.14-25-pve` per tree, seven; `R: zfs_immediate_write_sz`, `P: zfs_immediate_write_sz zfs_vdev_disk_sync_writes`; 7. A `FAIL` line on R or P is a stop (NOT RUN); a tree missing (`stamp FAIL not built`) other than R and P is noted for tonight (HRA1 rebuilds it); any other `FAIL` on another tree: report it as a finding for tonight (tonight's HRA would stop on it) and go on.

### 3. Boot 7.0.14-25-pve once (pre-approved reboot 1 of 2)

As TASK-1121-HRA1's step 3: write `$L/RESUME.md` ("TASK-1123-HRAS: steps 0-2 done, continue at step 3 after the boot"), boot 7.0.14-25-pve once with `grub-reboot` and the menu entry K35 used (Debian stays the default), and resume by your own arrangement. Before the reboot, quote `sudo -n grub-editenv list` (it must now show that `next_entry`). After the boot: `uname -r` is `7.0.14-25-pve`, `sudo -n grub-editenv list` shows no `next_entry`, `cat /proc/cmdline` (same stop list as step 0), `lsmod | grep -E '^(zfs|spl|ublk_drv) '` empty, `ls -l /etc/zfs/zpool.cache` (as step 0), `free -m`, `swapon --show`. Do not change swap, sysctls or GRUB's configuration.
**If the VM comes back on another kernel** (6.12.118 most likely, since the GRUB default moved to it this morning): this is the finding that matters most for tonight, as every HRA task boots 7.0 this way. Stop, no further reboot: quote `uname -r`, `sudo -n grub-editenv list`, the GRUB lines of step 0, `grep -nE "^menuentry|^submenu|gnulinux.*7.0.14-25-pve" /boot/grub/grub.cfg | head -20`, and the exact `grub-reboot` command you ran; the verdict is `NOT RUN (the grub-reboot route to 7.0.14-25-pve did not boot it)`.

### 4. Disks and hddsim, built by the VM's gcc (about 2 min)

```
L=/var/log/task-1123-hras
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh disks > $L/disks.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `disks done` or `STOP`. Then `gcc --version | head -1; grep 'hddsim built' $L/driver.log; cat $L/disks.nohup $L/setup-devices.txt $L/hddsim-build.txt; cat $L/dev/7*.out; ps -o pid,stat,ni,rss,comm -C hddsim`, the disk check, and:

```
L=/var/log/task-1123-hras
cd $L/bin && sudo -n bash ./hr-bench.sh selftest; tail -n 1 $L/driver.log; sha256sum $L/selftest.txt $L/priotest.txt
test -r /var/log/task-1122-hrig3/selftest-G7.txt && { sed 1d $L/selftest.txt | diff - <(sed 1d /var/log/task-1122-hrig3/selftest-G7.txt) && echo selftest as calibrated; }
sed 1d $L/priotest.txt | sed -E 's/: read n [0-9]+ mean ([0-9.]+) p50 [0-9.]+ p99 ([0-9.]+) .*/: \1 \/ \2/'
```

Expected: `hddsim built: 0 warnings` and an empty `hddsim-build.txt`; five lines `hddsim 7N: /dev/ublkb7N ready pid ...`; five `hddsim` processes; `setup-devices.txt` with kernel 7.0.14-25-pve, the ublk_drv vermagic, `hddsim.c c1af827f2aa6610b9ab828eb2b047de20cb8fea699dbc1c1018d36e8ed7369c7`, `best.conf 538b4c3b879d8e69b0c9a7b3e6d8f2fe2bb3d6337c9b5fa7ba3625ddb12af7b8 ... store_mb=512 ncq_prio=0`, `best.guest -`, R's srcversion from step 2, and for each disk `size_mib 16384 sched ... [none] rot 1 wbt_lat_usec 75000 write_cache write_back ... max_sectors_kb 1024 lbs 4096 iostats 1` (`wbt_debugfs -` is not a stop here). `selftest done`, then the two sha256 of the table, then `selftest as calibrated` when calibration's file is on this VM, then the five priotest values `277.4 / 790.5`, `65.1 / 438.8`, `44.6 / 432.2`, `44.6 / 432.2`, `15.4 / 34.9` (TASK-1121-HRA1's row G7).
A warning in the build: stop path (FAIL), quoting `$L/hddsim-build.txt` whole. A sha256 that differs: if `selftest as calibrated` is printed and the five priotest values are the row's, it is a finding (this VM's gcc prints the same model as for kit v2.1 with some other figure), and you quote both files whole; otherwise the stop path (FAIL). A disk not as expected: the stop path.

### 5. Pool, as HRA builds it (about 10 to 20 min)

```
L=/var/log/task-1123-hras
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh pool > $L/pool.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/driver.log` until `pool done` or `STOP`. Then `cat $L/pool.nohup $L/setup-zpool-list.txt $L/setup-props.txt`, the disk check, and the store lines with `G=pool`. Expected: as the table. MemAvailable below 2500 MiB, or `err`/`rej` above 0: the stop path.

### 6. Arm P1I, one full run, under the guard (about 6 min)

Write the guard, check it, start it:

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras
cat > $T/guard.sh <<'EOF'
#!/bin/bash
# TASK-1123-HRAS guard: once a second, MemAvailable, the number of hddsim
# servers and the last statistics line of each disk; on the first stop
# condition the guest is stopped, so that the pool does not suspend.
L=/var/log/task-1123-hras
LIM=440000
MINMA=1024
tripped=0
while [ ! -e "$L/guard.stop" ]; do
    ma=$(awk '/^MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
    np=$(ps -C hddsim -o stat= | awk '$1 !~ /^Z/ {n++} END {print n + 0}')
    why=""
    [ "$ma" -lt $MINMA ] && why="$why MemAvailable $ma MiB;"
    [ "$np" -lt 5 ] && why="$why $np hddsim;"
    out="$(date +%s) ma $ma"
    for i in 70 71 72 73 74; do
        s=$(tail -n 5 "$L/dev/$i.stats" | awk '
            BEGIN { f = r = k = x = j = "-"; e = 0 }
            /^E / { e++ }
            /^S / {
                for (n = 6; n < NF; n++) {
                    if ($n == "full") f = $(n + 1)
                    if ($n == "runs") r = $(n + 1)
                    if ($n == "store_kib") k = $(n + 1)
                    if ($n == "err") x = $(n + 1)
                    if ($n == "rej") j = $(n + 1)
                }
            }
            END { printf "full %s runs %s kib %s err %s rej %s E %d", f, r, k, x, j, e }')
        out="$out | $i $s"
        read -r _ _ _ _ _ k _ x _ j _ e <<< "$s"
        [ "$k" != - ] && [ "$k" -gt $LIM ] && why="$why $i store_kib $k;"
        [ "$x" != - ] && [ "$x" != 0 ] && why="$why $i err $x;"
        [ "$j" != - ] && [ "$j" != 0 ] && why="$why $i rej $j;"
        [ "$e" != 0 ] && why="$why $i E lines $e;"
    done
    echo "$out${why:+ | why$why}"
    if [ -n "$why" ] && [ $tripped = 0 ]; then
        tripped=1
        echo "TRIP $(date -u +%H:%M:%S):$why"
        if pkill -TERM -f 'hr-load.py guest'; then
            echo "TRIP guest stopped"
        else
            echo "TRIP no guest running"
        fi
    fi
    sleep 1
done
echo "guard end $(date -u +%H:%M:%S)"
EOF
echo "cd82d4675fef7923793038b83f84f757a5da594112a8d2f2321aa39743d65e8f  $T/guard.sh" | sha256sum -c && bash -n $T/guard.sh && echo guard syntax-ok
```

Expected: `guard.sh: OK` and `guard syntax-ok`; a different sha256 means the text changed in transit: stop the task (NOT RUN, nothing has run yet but the disks and the pool: step 9 from `teardown`) and quote `diff` between your file and the block. Then:

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras
setsid nohup sudo -n bash $T/guard.sh > $L/guard.log 2>&1 < /dev/null &
sleep 3; tail -n 1 $L/guard.log | cut -c1-400
```

Expected: a line `<epoch> ma <MiB> | 70 full ... runs ... kib ... err 0 rej 0 E 0 | 71 ...` for the five disks, with no `why`. Then the run:

```
L=/var/log/task-1123-hras
date -u +%T; cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh point S1 P1I > $L/point-S1.nohup 2>&1 < /dev/null &
```

Poll every 30 s: `tail -n 2 $L/driver.log; tail -n 1 $L/guard.log | cut -c1-160; grep -h '^TRIP' $L/guard.log`, until `point S1 P1I done` or `STOP` (about 4 min 25 s). A `TRIP` line or a `STOP`: the stop path at once. Leave the guard running. After `done`:

```
L=/var/log/task-1123-hras
grep -E 'point S1|arm P1I' $L/driver.log; head -12 $L/raw/S1/P1I/arm.txt; grep memavail $L/raw/S1/P1I/layout.txt; tail -n 3 $L/raw/S1/P1I/guest.txt; cat $L/raw/S1/P1I/guest.err
python3 -I $L/bin/hr-report.py calib $L/raw S1 > $L/calib-s1.md; echo rc=$?; sed -n '/^## Criteria/,/^- S1/p' $L/calib-s1.md
```

Then the disk check, the state read with `G=s1`, and the store lines with `G=s1`. Expected: `arm.txt` with `arm P1I P 75000 0 zfs_vdev_disk_sync_writes=1 zfs_immediate_write_sz=2097152`, P's stamp, srcversion loaded = built, `set zfs_vdev_disk_sync_writes=1`, `set zfs_immediate_write_sz=2097152`, a pool line ending `cachefile none`; the guest's `END` line, no `ERROR`, an empty `guest.err`; rc 0; in the Criteria table V1's verdict and its values, and in the `- S1/P1I:` line `models 1` (every other criterion compares the rig with the host and is not judged here). V1 failing on lateness alone is a finding: say whether other VMs were at work, since this host is not kept quiet for this task.

### 7. Read-back of pages kept as runs (about 4 min)

ZFS never reads back the log blocks P1I wrote, and the guest's own data are kept as single-run pages, so this step writes a known file made of 4 KiB sectors with 4, 3, 2 and 8 runs (32 MiB each; 8 runs is kept in full), at the start of the zvol's ring area (8192 MiB in, data the guest no longer needs), under P1I's parameters, then reloads the tree and imports the pool again (nothing left in the ARC) and reads it back. The raidz2 parity of such sectors is kept as runs too.

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras; G=rbw
(sudo -n cat /proc/spl/kstat/zfs/hrp/state > $L/state-$G.txt 2>&1 &); sleep 3; cat $L/state-$G.txt
python3 -I - $T/rb.bin <<'EOF'
import struct, sys
MIB = 1 << 20
def seg(unit, head, base, nbytes):
    # units of `unit` bytes, each one 8-byte value repeated (never 0),
    # after a first unit of `head` bytes when head > 0
    out = bytearray()
    v = base
    if head:
        out += struct.pack("<Q", v | 1 << 63) * (head // 8)
        v += 1
    while len(out) < nbytes:
        out += struct.pack("<Q", v | 1 << 63) * (unit // 8)
        v += 1
    return bytes(out[:nbytes])
with open(sys.argv[1], "wb") as f:
    f.write(seg(1024, 0, 1 << 40, 32 * MIB))     # 4 runs per 4 KiB sector
    f.write(seg(2048, 1024, 2 << 40, 32 * MIB))  # 3 runs
    f.write(seg(4096, 2048, 3 << 40, 32 * MIB))  # 2 runs
    f.write(seg(512, 0, 4 << 40, 32 * MIB))      # 8 runs: kept in full
EOF
echo "0793dcd6531dfd8a13639ca8d8154c0dbb46ba90ab23bedd2f66e8d00539a61c  $T/rb.bin" | sha256sum -c
Z=$(cat $T/cur-tree); v=/dev/zvol/hrp/vm; [ -b $v ] || for d in /dev/zd*; do case $d in *p*) continue ;; esac; [ "$(sudo -n $Z/udev/zvol_id $d 2>/dev/null)" = hrp/vm ] && v=$d; done; echo "tree $Z zvol $v"; ls -l $v
sudo -n dd if=$T/rb.bin of=$v bs=1M seek=8192 oflag=direct conv=fsync status=none; echo dd-rc=$?; sudo -n $Z/zpool sync hrp; echo sync-rc=$?
```

Expected: `ONLINE`; `rb.bin: OK`; `tree /var/tmp/hra-trees/P` and a block device; `dd-rc=0`; `sync-rc=0`. Then the store lines with `G=rbw` and the disk check. Then stop the guard and read its log:

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras
touch $L/guard.stop; sleep 3; tail -n 1 $L/guard.log; grep -c '^TRIP' $L/guard.log
cat > $T/tl.awk <<'EOF'
# guard.log: one line every 15 s and every line with a stop condition,
# then each disk's peaks and the lowest MemAvailable
function col(off,   k, s) {
    s = ""
    for (k = 4; k < NF; k++)
        if ($k == "|" && $(k + 1) ~ /^7[0-4]$/)
            s = s " " $(k + off)
    return (s)
}
/^TRIP/ { print; next }
$2 == "ma" {
    if (t0 == "")
        t0 = $1
    t1 = $1
    if (mn == "" || $3 + 0 < mn)
        mn = $3 + 0
    for (k = 4; k < NF; k++)
        if ($k == "|" && $(k + 1) ~ /^7[0-4]$/) {
            d = $(k + 1)
            if ($(k + 7) + 0 > mk[d]) mk[d] = $(k + 7) + 0
            if ($(k + 5) + 0 > mr[d]) mr[d] = $(k + 5) + 0
            if ($(k + 3) + 0 > mf[d]) mf[d] = $(k + 3) + 0
        }
    if ($1 >= nx || /why/) {
        nx = $1 + 15
        print "+" ($1 - t0) "s ma " $3 " kib" col(7) " runs" col(5) \
            " full" col(3) " rej" col(11) (/why/ ? " WHY" : "")
    }
}
END {
    for (d = 70; d <= 74; d++)
        printf "ublkb%d peak store_kib %d full %d runs %d\n", d, mk[d],
            mf[d], mr[d]
    printf "lowest MemAvailable %d MiB over %d s\n", mn, t1 - t0
}
EOF
awk -f $T/tl.awk $L/guard.log > $L/guard-timeline.txt; cat $L/guard-timeline.txt
```

Expected: `guard end ...`; 0; the timeline: a line every 15 s from the guard's start (`+<s>s ma <MiB> kib <five store_kib> runs <five> full <five> rej <five>`), no `TRIP` or `WHY`, then each disk's peak `store_kib`, `full` and `runs`, and the lowest MemAvailable over the guard's span. Then the reload and the read-back:

```
L=/var/log/task-1123-hras
cd $L/bin && setsid nohup sudo -n bash ./hr-bench.sh arm P1I $L/arm-rb.txt > $L/arm-rb.nohup 2>&1 < /dev/null &
```

Poll until `pgrep -af 'hr-bench.sh ar[m] P1I'` prints nothing (about 30 s; the brackets keep the poll's own shell out of the match); a `STOP` in driver.log is the stop path. Then:

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras; G=rbr
tail -n 1 $L/driver.log; head -6 $L/arm-rb.txt
(sudo -n cat /proc/spl/kstat/zfs/hrp/state > $L/state-$G.txt 2>&1 &); sleep 3; cat $L/state-$G.txt
Z=$(cat $T/cur-tree); v=/dev/zvol/hrp/vm; [ -b $v ] || for d in /dev/zd*; do case $d in *p*) continue ;; esac; [ "$(sudo -n $Z/udev/zvol_id $d 2>/dev/null)" = hrp/vm ] && v=$d; done; echo "tree $Z zvol $v"; ls -l $v
sudo -n dd if=$v of=$T/rb.out bs=1M skip=8192 count=128 iflag=direct status=none; echo dd-rc=$?
sha256sum $T/rb.out; cmp $T/rb.bin $T/rb.out && echo read-back identical
sudo -n $Z/zpool status -v hrp
```

Expected: the `arm P1I: tree /var/tmp/hra-trees/P ...` line and `arm-rb.txt`'s head as step 6's `arm.txt`; `ONLINE`; the tree and a block device; `dd-rc=0`; the sha256 `0793dcd6531dfd8a13639ca8d8154c0dbb46ba90ab23bedd2f66e8d00539a61c`; `read-back identical`; every vdev `0 0 0` and `errors: No known data errors`. If `cmp` differs: `cmp -l $T/rb.bin $T/rb.out | head -5; cmp -l $T/rb.bin $T/rb.out | wc -l` (the 1-based offset divided by 33554432, rounded down, names the part: 0 four runs, 1 three, 2 two, 3 eight runs kept in full), then go on to step 8 (the scrub says more), and the verdict is FAIL. Then the store lines with `G=rbr`.

### 8. Scrub (about 2 to 10 min)

```
L=/var/log/task-1123-hras; T=/var/tmp/task-1123-hras; Z=$(cat $T/cur-tree)
date -u +%T; setsid nohup sudo -n $Z/zpool scrub -w hrp > $L/scrub.nohup 2>&1 < /dev/null &
```

Poll every 2 minutes: the state read with `G=scrub` first (`SUSPENDED`: the stop path), then `Z=$(cat /var/tmp/task-1123-hras/cur-tree); sudo -n $Z/zpool status hrp | grep -A2 'scan:'`, until `scrub repaired`. Then `cat $L/scrub.nohup; sudo -n $Z/zpool status -v hrp` (with `Z` set as above), the store lines with `G=scrub`, and the disk check. Expected: `scrub repaired 0B in ... with 0 errors`, every vdev `0 0 0`, `errors: No known data errors`, an empty `scrub.nohup`.

### 9. Teardown, back to Debian 6.12.118 (pre-approved reboot 2 of 2), end

Run the disk check, then:

```
L=/var/log/task-1123-hras; cd $L/bin
sudo -n bash ./hr-bench.sh teardown; tail -2 $L/driver.log
lsmod | grep -E '^(zfs|spl|ublk_drv) '; ls /dev/ublk* 2>&1; ps -C hddsim -o pid= | wc -l; tail -n 1 $L/dev/7*.stats
sudo -n dmesg | sed -n '/task-1123-hras: start/,$p' > $L/kmsg.txt; grep -E 'BUG|WARNING|hung_task|blocked for more than|VERIFY|PANIC|Call Trace|Oops|ublk|suspended' $L/kmsg.txt | tail -40
cd / && sudo -n tar -czf $L/raw.tar.gz -C $L $(cd $L && ls -d raw dev 2>/dev/null) && sudo -n rm -rf /var/tmp/task-1123-hras
sudo -n fstrim -av && df -h /var/tmp
ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: the `teardown done` line naming no module and no device; no module loaded, no `/dev/ublk*`, 0 `hddsim` processes, each stats file ending with `I ... hddsim 7N stopped (stop 0 del 0 io_err 0)`; no kernel line but `ublk` lines (quote every line, with the step it happened in). `teardown` not returned after 5 minutes: capture as in the stop path, then its forced reboot. `/etc/zfs/zpool.cache`: size 0: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it.
Keep `/var/log/task-1123-hras` (with `raw.tar.gz`); leave `/var/tmp/hra-trees` and every `/var/log/task-1121-hra*` directory as they are.
Then boot back into the default entry, Debian 6.12.118, by a plain reboot (`sudo -n systemctl reboot`), resuming by your own arrangement; then:

```
L=/var/log/task-1123-hras; R=/var/tmp/hra-trees
date -u; uname -r; sudo -n grub-editenv list; lsmod | grep -E '^(zfs|spl|ublk_drv) '; ls /dev/ublk* /dev/zfs 2>&1; ls -l /etc/zfs/zpool.cache 2>&1
[ -e $L/raw.tar.gz ] || (cd / && sudo -n tar -czf $L/raw.tar.gz -C $L $(cd $L && ls -d raw dev 2>/dev/null)); sudo -n rm -rf /var/tmp/task-1123-hras; ls -l $L/raw.tar.gz
sha256sum -c $L/stamps-0.txt | grep -vc ': OK$'; cd $L/bin && bash ./hr-trees.sh check $R > $L/trees-check-end.txt 2>&1; echo rc=$?; diff $L/trees-check.txt $L/trees-check-end.txt && echo trees as found
sudo -n fstrim -av; df -h / /var/tmp; free -m
```

Expected: `uname -r` step 0's (6.12.118), no `next_entry`, no module, no device, `ls -l /etc/zfs/zpool.cache` as at step 0 (after the stop path's forced reboot, a non-empty one: report it and leave it), `raw.tar.gz` (made now if the forced reboot came first); 0; rc as at step 2 and `trees as found`. Then write the report and send it.

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; any commit, tree, parent, file list, mode or hash that differs; a `FAIL` from `hr-trees.sh check` on R or P; the 7.0 boot landing on another kernel (no further reboot); hddsim warnings; selftest or priotest other than expected (beyond step 4's finding case); a disk not as expected after `disks`; MemAvailable below 2500 MiB or a refused write after the pool; the guard's sha256; a `TRIP`; a driver `STOP:`; a pool this task did not create; a disk check other than expected; a state other than `ONLINE`; a hang; any `VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, `WARNING` or hung task in the kernel log; an OOM kill; more than 1 h 45 on the VM (finish the current step, then step 9, and say what did not run). Every stop after step 3 goes through the stop path and ends with step 9's reboot back to 6.12.118. A read-back that differs or a checksum error goes on to the scrub and is a FAIL, not a stop. V1's lateness, the pool's frag, guest MB/s below 45 and the store's orientation values are findings.

## Report

`/var/log/task-1123-hras/report.md`, and the same text as your final message:

1. **The verdict line first**, exactly as defined above, then the answers, each with the figures it rests on:
   a. **The store under P1I**: per disk `full`, `runs` and `store_kib` after the pool (`store-pool.txt`), at the end of the run (`store-s1.txt`) and the guard's peaks, against 524,288 KiB, the PASS margin 393,216 and the guard's 440,000, and against the projection (about +96,000 KiB, at most about +292,000 above the post-pool value); then `guard-timeline.txt` whole.
   b. **Memory**: MemAvailable after the pool, the run's `memavail`, the guard's lowest, after the run.
   c. **Refusals, errors, suspension**: every disk check, the state reads, `TRIP` lines (none expected), the kernel lines.
   d. **The run**: the driver's lines for S1, the guest's END line and MB/s, `arm.txt`'s head, calib-s1.md's Criteria table and its `- S1/P1I` line.
   e. **Read-back and scrub**: the sha256s, `read-back identical` or the `cmp` lines, `store-rbw.txt` and `store-rbr.txt` against `store-s1.txt`, both `zpool status -v` outputs, the scrub's line and duration.
   f. **Kit v2.2 on the VM**: the kit checks, `gcc --version`, `hddsim built: N warnings` and `hddsim-build.txt`, the selftest and priotest sha256s, `selftest as calibrated`, the five priotest values.
   g. **The route to 7.0**: the GRUB lines and `grub-editenv list` before and after `grub-reboot`, the kernel after each boot, as tonight's HRA tasks depend on it.
2. Setup: kernels (both boots), /proc/cmdline, CPUs, RAM, swap, the kernel config lines, `trees-check.txt` (both), the modinfo lines, `setup-devices.txt`, `setup-zpool-list.txt`, `setup-props.txt`, driver.log whole, and the time of each step (T0 to the report).
3. Deviations, what could not be run, end state (modules, devices, processes, `fstrim -av`, `df -h /var/tmp`), what happened to `/etc/zfs/zpool.cache` at steps 0, 3 and 9, the reboots with their UTC times and how you resumed, then End.
