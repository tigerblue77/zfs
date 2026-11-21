TASK-1121-AR3 (from the coordinator session "main"): performance, VM 105 (CHSONVTST02) on 7.0.14-25-pve. The measurement TASK-1116-AR2 was written for and never ran (paused on 17/11), rebased on the current head of upstream PR #19935: do C2 and C3 remove more of the direct reclaim and of the falls of the ARC target that #19935 leaves, and do they bring gains? About 3 h on the VM plus the wait for main's GO (steps 0 to 5 about 55 min, at most 70; step 6 about 100, at most 115; step 7 and END about 25, at most 30), at most 3 h 35 without that wait. Its measured part (step 6, about 100 min, never more than 115) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 5 are functional and may run while other VMs work. Nothing is published on GitHub, no credential files, fstrim after deletions. Report: `/var/log/task-1121-ar3/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. The final report is sent after the reboot of END.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `L=/var/log/task-1121-ar3`, `D=/var/tmp/task-1121-ar3`; the blocks below set them.

**Reboots.** Two are part of this task: into 7.0.14-25-pve at step 4 (`grub-reboot`, Debian stays the default) and back into Debian at END. A third is allowed only for a module that will not unload or a hung task, after its diagnostics are saved under `$L`. Before each, create a one-shot scheduled task of your own session 5 minutes ahead to resume, as in your earlier tasks; report each reboot with its UTC time. A measured run is never resumed after a reboot: go to step 7, then END.

## Why

TASK-1116-AR (16 Nov) measured our own branch for #19543 on this VM, on the encrypted-read workload with fragmented memory:
- **F1 (F + C1)** cut F's direct reclaim by about 95 %, but still had +7/+5 allocstall_normal and 7 falls of `c` > 1 GiB per encrypted run (M: 2), all of the real-shortage kind: kswapd runs for order 0 reaching priority 1–2 with MemFree about 1 GiB. Mean `c` 5.5 GiB.
- **FA could not be judged.** C3 read `high_wmark_pages()`, which includes `zone->watermark_boost`. Both FA loads followed an F run with a Normal-zone boost of 28756 pages, so `arc_sys_free` came out at 1116 and 1235 MiB instead of about 495 MiB. With that reserve FA had no reclaim at all, and its higher mean read throughput (542–619 MB/s against 471–506) may be ARC hits only.
- **Without MGLRU,** F still had 18 stalls in its control (9 decrypt buffers, 9 ABD pages), M none.

TASK-1116-AR2 was to test C2 and C3 on top of F1, with C3 fixed. It never ran. Since then:
- **Our branch is dropped for upstream PR #19935** (Alexander Motin). Its head `c8ac7824ffd` contains everything of F1 that had a measured effect: no kswapd wake-up for large SPL requests, and the order-0 page that may wake kswapd at the low watermark (`spl_wake_kswapd_if_needed()`, mask `__GFP_KSWAPD_RECLAIM|__GFP_HIGHMEM|__GFP_NOWARN|__GFP_NOMEMALLOC`), allocated and freed before every attempt of a KM_SLEEP request above 128 KiB (each pass of `spl_kmem_alloc_impl()`'s loop) and before each slab-growth `vmalloc()` in `kv_alloc()`. Its threshold is 128 KiB where ours was 32 KiB; no request between them appeared in TASK-1118-K35.
- **TASK-1118-K35B measured nearly this code** as P2S (`3338e9f3b7e`, our commit on #19935's previous head; `c8ac7824ffd` differs from it only by the function's name, `__GFP_NOMEMALLOC` and comments): 0 wake-ups of order ≥ 4, but still +10/+17 allocstall_normal, 4/8 falls of `c` > 1 GiB and mean `c` 5662/5453 MiB. The remainder C2 and C3 aim at is still there with #19935.
- **The base.** `c8ac7824ffd`'s parent `cde5d598eb4` is `c913da86156` (AR's, K35's and K35B's M) plus one commit that touches only CI. So every source file outside #19935's four SPL files is the one M, F1 and P2S were built from, and C2 and C3v2 change exactly the files they changed on F1.
- **C3 is fixed (C3v2).** It sums the unboosted high watermarks, `zone->_watermark[WMARK_HIGH]`: the value the kernel itself boosts from (`boost_watermark()`), and which only a change of `min_free_kbytes`, `watermark_scale_factor` or memory hotplug changes. A configure check covers kernels before 5.0, where the field is `watermark[]` and there is no boost. So `arc_sys_free` no longer depends on what ran before the load. Expected on this VM: 5 × (55 + 6114 + 19171) pages = 20480 × 25340 = **518963200** bytes (about 495 MiB).
- **C2 is unchanged.** C3 tests H2 (the reserve sits where the kernel already reclaims) and C2 tests H3 (the ARC lowers its target too late): when `arc_available_memory()` is below 0, `arc_adapt()` lowers `c` at once by the shortfall, less the eviction already pending.
- **Every load starts with no watermark boost** (step 6), so that no arm starts its runs boosted; AR's loads kept whatever the previous run left.

Questions:
1. Do C2 and/or C3 remove more than #19935 does (P3's remaining stalls and falls), and do they bring gains (mean `c`, read throughput)?
2. Is a throughput gain speed or ARC hits? Answer it per pass: first pass against later passes, with the bytes read from the disks and the ARC hit ratio of each pass.
3. Where do P3's remaining falls and direct reclaims come from (waking order, gfp mask, caller), and do C2 or C3 remove them?
4. Without MGLRU (lru_gen=n), what is left of P3's cost and of the best C arm's, and what is it?

## What you fetch, and what this file carries

| What | Source | Commit | Tree |
| --- | --- | --- | --- |
| #19935 head (arm P3) | https://github.com/openzfs/zfs `refs/pull/19935/head` | `c8ac7824ffd6f71e220b8afb09b662cc61b906fc` (parent `cde5d598eb44b5d6efe8167666d988a496346657`, whose parent is `c913da86156eba3243cc1ef515e98d69887654a7`) | `d4811874880011a51063eff181585c91b364f94c` |
| M, for the clone | already in `~/work/z-M0` | `c913da86156eba3243cc1ef515e98d69887654a7` | `7e32938edf1a54c02519e2dc0104e9325b5a6f4d` |

Carried at the end of this file (appendix), checked by sha256 at step 1: `C2.patch` b9f83878d2f4d3e38d78614111b663f682d7c90e6f0a958effc15c1362114632 and `C3v2.patch` 51aa57347cf8b9ef96af2e445f21750c2d18777c14f5803427884fea5df4a6d8 (base64; TASK-1116-AR2's, unchanged), `dr2.sh` 8c9716872ee3e4e2344363777837071aaada8b1b6c7f6832a421c4d359ab2632 and `dr2-sum.py` 43cf05d2032affcf9a2fec190d2bd45af01d32d350c810f42021e5666b08dd6a (TASK-1116-AR2's, unchanged; no tab in either). Also used, already on this VM: AR's archive `/var/log/task-1116-ar/files.tar.gz` (`seq.sh`, `t2.sh`, `control.sh`, `summ.sh`, `analyze.sh`, `fragmem.c`, ...), the pool `tank` (exported, on this VM's test disks) and its key.

## Arms

| Arm | Tree | Contents | `git diff --numstat` against `c8ac7824ffd` |
| --- | --- | --- | --- |
| P3 | `~/work/z-P3` (new) | #19935 head | (none) |
| P3C2 | `~/work/z-P3C2` (new) | P3 + C2.patch | `18 3 module/zfs/arc.c` |
| P3C3 | `~/work/z-P3C3` (new) | P3 + C3v2.patch | `50 0 config/kernel-zone-watermark.m4`, `2 0 config/kernel.m4`, `37 0 module/os/linux/zfs/arc_os.c` |
| P3A | `~/work/z-P3A` (new) | P3 + C2.patch + C3v2.patch | the four lines above |

M, F, F1, FA, P2 and P2S are not run again; AR's and K35B's numbers stand for them, for orientation only (other sessions, and the hypervisor moved to 7.0.14-29-pve on 18/11 between AR and K35B).

## VM rules for this task

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file; `tank`'s key stays where it is. No package is installed.
- The pool is `tank`, imported and exported only by this task's steps and scripts, with the tree's own `zpool` and `zfs`. Never `zpool import -a`. A pool other than `tank` imported at any point is a stop: report it and leave it as it is (no export, no destroy, no import).
- `/etc/zfs/zpool.cache`: at step 0, absent is expected and size 0 is moved aside; above 0 bytes it names a pool the module would import at load: stop and report its `ls -l`, load nothing. Importing `tank` writes it and exporting `tank` empties it; END moves an empty one aside.
- This is a performance measurement: nothing else on this VM from step 6's start to its last run (no build, ztest, ZTS, second load, bpftrace or blktrace).
- If a fetch fails, wait 1 minute and try once more, then stop and report. If a commit, a tree, a parent, a numstat or a sha256 differs, stop and report the outputs as they are: do not edit, retype or repair any file, and compare each value with the program, not by eye.
- Long steps (the builds, the runs) run detached (`setsid nohup ... &`) and are polled. A hang is: the builds still going after 45 minutes; a run still going 15 minutes after its start (an encrypted run takes about 10.5, a control about 1.5), or the run log unchanged for 15 minutes while runs go on. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zpool|fio|dd|fragmem|kworker'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.

## Steps

### 0. State, on Debian 6.12 (T0 = when you start this step)

```sh
L=/var/log/task-1121-ar3; D=/var/tmp/task-1121-ar3
echo "T0=$(date +%s)"; date -u; hostname; uname -r; nproc; free -m; df -h / /var/tmp ~/work; uptime -s; swapon --show
lsmod | grep -E '^(zfs|spl|ublk_drv|null_blk) '; ls -l /dev/zfs 2>&1; ls -d $L $D ~/work/z-P3 ~/work/z-P3C2 ~/work/z-P3C3 ~/work/z-P3A 2>&1
ls -d ~/work/z-M0 /usr/src/linux-headers-7.0.14-25-pve /boot/vmlinuz-7.0.14-25-pve /var/log/task-1116-ar/files.tar.gz
git -C ~/work/z-M0 rev-parse HEAD 'HEAD^{tree}'; git -C ~/work/z-M0 status --short --untracked-files=no | wc -l; grep -m1 '^  \$ ./configure' ~/work/z-M0/config.log
sudo -n grub-editenv list; ls -l /etc/zfs/zpool.cache 2>&1; cat /sys/kernel/mm/lru_gen/enabled; sysctl vm.compact_unevictable_allowed
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|t2.sh|seq.sh|hddsim'
```

Expected: a `6.12.` kernel; no module, no `/dev/zfs`; `$L`, `$D` and the four `~/work/z-P3*` absent; the four paths of the next line present; `c913da86156eba3243cc1ef515e98d69887654a7`, `7e32938edf1a54c02519e2dc0104e9325b5a6f4d`, `0`, and the configure line `$ ./configure --with-linux=/usr/src/linux-headers-7.0.14-25-pve --with-linux-obj=/usr/src/linux-headers-7.0.14-25-pve`; no `next_entry`; no process listed. Note lru_gen and `vm.compact_unevictable_allowed` as found (END restores them). Stop and report, changing nothing, if: the kernel is not 6.12; a module is loaded or `/dev/zfs` exists; `$L`, `$D` or a `z-P3*` tree exists; a path is missing; z-M0 is at another commit or tree, or `git status` is not empty; less than 20 GiB free on the filesystem of `~/work`; `grub-editenv list` shows a `next_entry`; `/etc/zfs/zpool.cache` is above 0 bytes; a build, test or read job runs. If z-M0's configure line differs from the one above, go on (step 2 uses the line above) and say so.

Then the log directory and your copy of this file. In tonight's schedule the copy you fetched and checked is `/var/tmp/tasks-1121-night/task-1121-ar3.md`; if main's message had you fetch it elsewhere, use that path in this block:

```sh
L=/var/log/task-1121-ar3; D=/var/tmp/task-1121-ar3
sudo -n mkdir -p $L && sudo -n chown "$(id -un)" $L && mkdir -p $D && cp /var/tmp/tasks-1121-night/task-1121-ar3.md $L/ && sha256sum $L/task-1121-ar3.md
if [ -f /etc/zfs/zpool.cache ] && [ ! -s /etc/zfs/zpool.cache ]; then sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0 && echo moved-empty-cache; fi
```

The sha256 must be the one main's message gave for this file; if not, stop and report.

### 1. Patches and tools, from this file (about 1 min)

```sh
L=/var/log/task-1121-ar3; D=/var/tmp/task-1121-ar3; TF=$L/task-1121-ar3.md
cd $D && for f in C2.patch C3v2.patch; do awk -v s="base64 -d > $f <<'EOF'" '$0 == s {on = 1; next} on && $0 == "EOF" {on = 0} on' $TF | base64 -d > $f; done
cd $D && for f in dr2.sh dr2-sum.py; do awk -v s="cat > $f <<'EOF'" '$0 == s {on = 1; next} on && $0 == "EOF" {on = 0} on' $TF > $f; done
cd $D && chmod +x dr2.sh dr2-sum.py && printf '%s  %s\n' \
    b9f83878d2f4d3e38d78614111b663f682d7c90e6f0a958effc15c1362114632 C2.patch \
    51aa57347cf8b9ef96af2e445f21750c2d18777c14f5803427884fea5df4a6d8 C3v2.patch \
    8c9716872ee3e4e2344363777837071aaada8b1b6c7f6832a421c4d359ab2632 dr2.sh \
    43cf05d2032affcf9a2fec190d2bd45af01d32d350c810f42021e5666b08dd6a dr2-sum.py | sha256sum -c
cd $D && sh -n dr2.sh && python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' dr2-sum.py && echo tools-syntax-ok
```

Expected: four `OK`, then `tools-syntax-ok`. A file that does not check: stop and report (there is no hand-edit fallback).

What `dr2.sh` and `dr2-sum.py` add to AR's `dr.sh` and `dr-sum.py`:
- `dr2.sh start <prefix> <disk>...` records, besides what `dr.sh` did: every wake-up of a sleeping kswapd of **any** order with its kernel stack (dr.sh kept order 0 without stack); every kswapd run (`mm_vmscan_kswapd_wake`, with the order it balances for) and every time kswapd goes to sleep; and in each 4 Hz sample the wall clock (`W=`), the ARC data hits, iohits and misses, and the sectors read from the given disks (`rd_sect=`). Same buffer size as dr.sh.
- `dr2-sum.py` prints dr-sum.py's lines first, unchanged in meaning, then: one line per direct reclaim (time, order, gfp mask, task, caller, pages, whether a kswapd run was going on, MemFree and the Normal zone just before); kswapd wake-ups by order and kswapd runs by order; each fall of `c` by more than 1 GiB within 5 s, with the ARC shrinker requests in it and, for the kswapd run that made the request of lowest priority number, the order it balanced for and the wake-ups that woke it (order, gfp mask, task, caller). A fall with no shrinker request is the ARC lowering `c` itself (reap thread, or arc_adapt with C2). With `--passes <file>` it adds the per-pass table described in step 6.

### 2. #19935, the four trees, release builds (on Debian, against the 7.0.14-25-pve headers; about 25 min)

```sh
cd ~/work/z-M0 && git fetch -q https://github.com/openzfs/zfs.git refs/pull/19935/head; echo rc=$?; git rev-parse FETCH_HEAD
```

Expected `c8ac7824ffd6f71e220b8afb09b662cc61b906fc`. If FETCH_HEAD is another commit (the PR was pushed again), fetch the commit itself and say so: `cd ~/work/z-M0 && git fetch -q https://github.com/openzfs/zfs.git c8ac7824ffd6f71e220b8afb09b662cc61b906fc`. If that fails twice, stop and report: build nothing, never measure another head. Then:

```sh
P=c8ac7824ffd6f71e220b8afb09b662cc61b906fc; cd ~/work/z-M0
git cat-file -e "$P^{commit}" && echo P3 present
test "$(git rev-parse "$P^{tree}")" = d4811874880011a51063eff181585c91b364f94c && echo P3 tree OK
git log -2 --format='%H %P | %an | %s' $P
git diff --numstat c913da86156eba3243cc1ef515e98d69887654a7 $P
```

Expected, in order: `P3 present`; `P3 tree OK`;
```
c8ac7824ffd6f71e220b8afb09b662cc61b906fc cde5d598eb44b5d6efe8167666d988a496346657 | Alexander Motin | Linux: Don't wake kswapd for larger spl_kvmalloc() attempts
cde5d598eb44b5d6efe8167666d988a496346657 c913da86156eba3243cc1ef515e98d69887654a7 | Tigerblue77 | CI: give the Linux test VMs 6 GiB, as FreeBSD's
```
and the numstat (tab-separated):
```
5	9	.github/workflows/scripts/qemu-5-setup.sh
1	0	include/os/linux/spl/sys/kmem.h
5	0	man/man4/spl.4
3	1	module/os/linux/spl/spl-kmem-cache.c
46	6	module/os/linux/spl/spl-kmem.c
```

The four trees, worktrees of the z-M0 clone as every earlier arm, then the patches:

```sh
D=/var/tmp/task-1121-ar3; P=c8ac7824ffd6f71e220b8afb09b662cc61b906fc
cd ~/work/z-M0 && for a in P3 P3C2 P3C3 P3A; do git worktree add -q --detach ~/work/z-$a $P || break; done; git worktree list | grep -E 'z-P3'
cd ~/work/z-P3C2 && git apply $D/C2.patch && echo P3C2 applied
cd ~/work/z-P3C3 && git apply $D/C3v2.patch && git add -N config/kernel-zone-watermark.m4 && echo P3C3 applied
cd ~/work/z-P3A && git apply $D/C2.patch && git apply $D/C3v2.patch && git add -N config/kernel-zone-watermark.m4 && echo P3A applied
```

Expected: four worktree lines at `c8ac7824ffd`, then the three `applied` lines. Then the four builds at once, detached, two jobs each on the 8 vCPUs, configured as K35B built z-P2 and z-P2S:

```sh
L=/var/log/task-1121-ar3
setsid nohup bash -c 'for a in P3 P3C2 P3C3 P3A; do ( cd ~/work/z-$a && t=$(date +%s) && ./autogen.sh > '$L'/autogen-$a.log 2>&1 && ./configure --with-linux=/usr/src/linux-headers-7.0.14-25-pve --with-linux-obj=/usr/src/linux-headers-7.0.14-25-pve > '$L'/configure-$a.log 2>&1 && nice -n 10 make -j2 > '$L'/make-$a.log 2>&1; echo "$a rc=$? $(( $(date +%s) - t )) s" ) & done; wait; echo build-done' > $L/build.log 2>&1 < /dev/null &
```

Poll `cat $L/build.log` until `build-done` (a hang is 45 minutes without it). On an `rc=` other than 0, quote `grep -nE 'error:|Error [0-9]' $L/make-<arm>.log | head -20` (or the configure log's last lines) and stop. Then, per tree:

```sh
L=/var/log/task-1121-ar3
cat $L/build.log
for a in P3 P3C2 P3C3 P3A; do cd ~/work/z-$a && { echo "== $a"
grep -m1 '^  \$ ./configure' config.log
grep -E 'kernel crypto API is available|NODE_DATA\(\) is available|struct zone has watermark_boost' $L/configure-$a.log
grep -n 'warning:' $L/make-$a.log | grep -E 'spl-kmem|arc\.c|arc_os\.c' | grep -v -i objtool
git status --short --untracked-files=no; git diff --numstat
sha256sum module/os/linux/spl/spl-kmem.c module/os/linux/spl/spl-kmem-cache.c include/os/linux/spl/sys/kmem.h module/zfs/arc.c module/os/linux/zfs/arc_os.c config/kernel.m4
test -f config/kernel-zone-watermark.m4 && sha256sum config/kernel-zone-watermark.m4
echo "$a $(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1) spl $(modinfo -F srcversion module/spl.ko) zfs $(modinfo -F srcversion module/zfs.ko)"
} 2>&1 | tee $L/tree-$a.txt; done
cd ~/work/z-P3A && perl scripts/cstyle.pl -cpP module/os/linux/spl/spl-kmem.c module/os/linux/spl/spl-kmem-cache.c module/zfs/arc.c module/os/linux/zfs/arc_os.c; echo cstyle-rc=$?
```

Expected in every tree: four `rc=0` lines and `build-done`; the configure line `$ ./configure --with-linux=/usr/src/linux-headers-7.0.14-25-pve --with-linux-obj=/usr/src/linux-headers-7.0.14-25-pve`; `checking whether kernel crypto API is available... no` (`yes` is a stop: the workload measures the ICP's decrypt buffers); no warning line; the numstat of the Arms table (`git status` shows the same files, the m4 file as added); vermagic `7.0.14-25-pve`; and these sha256:

| File | P3 | P3C2 | P3C3 | P3A |
| --- | --- | --- | --- | --- |
| `module/os/linux/spl/spl-kmem.c` | 1844efe75d2266f239979cb40b2fe883a1160492d9582d10c2c9b894816d4aa8 | same | same | same |
| `module/os/linux/spl/spl-kmem-cache.c` | 4dfe7d85834b03349988697664406d3196d5f0e14375d5f43e9acdd914a0a3bf | same | same | same |
| `include/os/linux/spl/sys/kmem.h` | 6cfc530fec80a96bbe743ce8e611a41affdfe067477a968cb94981f39bff6cb8 | same | same | same |
| `module/zfs/arc.c` | 091f206e6a12f598184e3a070f3a6edebd6f93e1ff5bde9ab601002b43ad041a | 089dcc7f9288b12420c4c31f42135d438aabe901c15ab8f6be6fe985c600e18a | 091f206e6a12f598184e3a070f3a6edebd6f93e1ff5bde9ab601002b43ad041a | 089dcc7f9288b12420c4c31f42135d438aabe901c15ab8f6be6fe985c600e18a |
| `module/os/linux/zfs/arc_os.c` | 4b22976302542e5151fb5bcfbe09a78443d67b6980d36be9abf61941a114a7b4 | 4b22976302542e5151fb5bcfbe09a78443d67b6980d36be9abf61941a114a7b4 | 963809962e48812740732baadf3016032cfe7266effef53a408ac8864a33fda1 | 963809962e48812740732baadf3016032cfe7266effef53a408ac8864a33fda1 |
| `config/kernel.m4` | 8cd4293e0a02f292bcadf23d741db67e7e19db1ee807c3e8cd7e7e0c95db277a | 8cd4293e0a02f292bcadf23d741db67e7e19db1ee807c3e8cd7e7e0c95db277a | 7511d6681b6f0b2343a7016277ec35cfc06217b0d3e8cbbe10ccedb9914cd484 | 7511d6681b6f0b2343a7016277ec35cfc06217b0d3e8cbbe10ccedb9914cd484 |
| `config/kernel-zone-watermark.m4` | – | – | 9272d5d4d250696342ad54681f7d6c6617061b92967939c1ceb3647a8b6960a6 | 9272d5d4d250696342ad54681f7d6c6617061b92967939c1ceb3647a8b6960a6 |
| configure prints | – | – | `checking whether NODE_DATA() is available... yes` and `checking whether struct zone has watermark_boost... yes` | same |

`cstyle-rc=0` with nothing printed. A sha256, a numstat or a configure line that differs: stop and report.

**srcversions.** Record each tree's line (`<arm> 7.0.14-25-pve spl ... zfs ...`): those are the values every load of that arm must show (step 6). Required, else stop and report: the same spl srcversion in the four trees; four different zfs srcversions. Expected, computed here from builds of the same sources (srcversion hashes only the module's own source files, without comments or whitespace, so builds against other headers have given the values seen on this VM: AR's FA, K35B's P2 and P2S):

| | P3 | P3C2 | P3C3 | P3A |
| --- | --- | --- | --- | --- |
| spl | 2707CE99FE3DD5AE73FCC36 | same | same | same |
| zfs | DC89AB9376CF0B2D8E0FB13 | C2DED177A7E5F5660E3594C | 24C9757D0E6F5A94B1FDF00 | 81597E511A7C3EDC7940A10 |

The zfs values are those of M, F1C2, F1C3 and F1A in TASK-1116-AR2, since the zfs module's sources are the same; P3's is the M value AR and K35B loaded here. The spl value differs from every earlier arm's (M BA40B6EA1E022CA48AB235F, F1 5E5E3AB4C54D2B9564A229C, P2 E529BEA80646EF655DE556C, P2S 6134B4E29B07DB15BDD6BFF). A value that differs from this table while the sha256 above match is a finding, not a stop: report it (it would mean a build difference, not a source difference).

### 3. AR's scripts (about 15 min)

```sh
D=/var/tmp/task-1121-ar3
mkdir $D/ar $D/ar.orig && tar -xzf /var/log/task-1116-ar/files.tar.gz -C $D/ar && tar -xzf /var/log/task-1116-ar/files.tar.gz -C $D/ar.orig && find $D/ar \( -name '*.sh' -o -name '*.py' -o -name '*.c' \) | sort
```

Then make in `$D/ar` the changes K35 and K35B made (K35's report, Deviations): the AR path `/var/tmp/task-1116-ar` replaced by `/var/tmp/task-1121-ar3/ar` in `t2.sh`, `summ.sh`, `analyze.sh` and `control.sh` and nothing else in them; AR's old run outputs removed from `$D/ar` (scripts, `fragmem.c` and AR's tools stay); `fragmem` rebuilt from `fragmem.c` with `gcc -O2`. Use AR's `seq.sh`, `t2.sh` and `control.sh`, and what they call, as AR ran them, with only the changes listed in step 6 (items 0 to 7 in `seq.sh`; item 5 in `t2.sh`, if needed). Make them now, so that step 6 starts on GO without editing. When done:

```sh
L=/var/log/task-1121-ar3; D=/var/tmp/task-1121-ar3
for f in seq.sh t2.sh control.sh summ.sh analyze.sh; do diff -u $D/ar.orig/$f $D/ar/$f; done > $L/scripts.diff; wc -l $L/scripts.diff; bash -n $D/ar/seq.sh && bash -n $D/ar/t2.sh && echo scripts-syntax-ok
```

(If the archive keeps the scripts in a subdirectory, use the paths `find` printed.) `scripts.diff` is quoted in the report.

### 4. Boot into 7.0.14-25-pve, state, smoke test (about 12 min, functional)

Boot 7.0.14-25-pve once with `grub-reboot` and the menu entry AR, K35 and K35B used (Debian stays the default), resuming by your own arrangement. After the boot:

```sh
L=/var/log/task-1121-ar3
date -u; uname -r; uptime -s; cat /proc/cmdline; sudo -n grub-editenv list; lsmod | grep -E '^(zfs|spl) '; free -m
sysctl vm.min_free_kbytes vm.watermark_boost_factor vm.watermark_scale_factor; cat /sys/kernel/mm/transparent_hugepage/enabled
awk '/^Node/{z=$4} $1=="pages"&&$2=="free"{f=$3} $1=="boost"{b=$2} $1=="min"{mi=$2} $1=="low"{lo=$2} $1=="high"{hi=$2} $1=="managed"{print z, "free", f, "boost", b, "min", mi, "low", lo, "high", hi, "managed", $2}' /proc/zoneinfo | tee $L/zones-boot.txt
sudo -n swapoff -a; echo y | sudo -n tee /sys/kernel/mm/lru_gen/enabled > /dev/null; sudo -n sysctl -q -w vm.compact_unevictable_allowed=0; swapon --show; cat /sys/kernel/mm/lru_gen/enabled; sysctl vm.compact_unevictable_allowed
```

Expected: `7.0.14-25-pve`; no `next_entry`; no module; `0x0007`; `vm.compact_unevictable_allowed = 0`; on AR's day min_free_kbytes 67584, watermark_boost_factor 15000, watermark_scale_factor 10 and the zones' min/low/high DMA 37/46/55, DMA32 4076/5095/6114, Normal 12781/15976/19171 pages. Stop if the kernel is not 7.0.14-25-pve, a `next_entry` is left, a module is loaded, or `/proc/cmdline` holds `slub_debug`, `kasan`, `kmemleak`, `page_poison`, `init_on_free` or `init_on_alloc=1`. If the unboosted high watermarks (high − boost) are not 55, 6114 and 19171 pages, go on: the expected `arc_sys_free` of the C3 arms is computed at each load from `/proc/zoneinfo`, not from these numbers. Say so in the report.

**Smoke test**, with P3 loaded and `tank` imported:

```sh
L=/var/log/task-1121-ar3; D=/var/tmp/task-1121-ar3; Z=~/work/z-P3
sudo -n sysctl -q -w vm.min_free_kbytes="$(sysctl -n vm.min_free_kbytes)"; grep -E '^ +boost' /proc/zoneinfo
cd $Z && sudo -n ./scripts/zfs.sh -r; echo rc=$?
echo "built spl $(modinfo -F srcversion $Z/module/spl.ko) zfs $(modinfo -F srcversion $Z/module/zfs.ko) loaded spl $(cat /sys/module/spl/srcversion) zfs $(cat /sys/module/zfs/srcversion)"
awk '$1=="arc_sys_free"{print $3}' /proc/spl/kstat/zfs/arcstats | tee $D/asf0; cat /sys/module/spl/parameters/spl_kmem_alloc_max
sudo -n $Z/zpool list -H -o name; sudo -n $Z/zpool import tank && sudo -n $Z/zfs load-key -a && sudo -n $Z/zfs mount -a; echo rc=$?
sudo -n $Z/zpool list -H -o name; sudo -n $Z/zpool list -vHP tank | tee $L/tank-layout.txt
for v in $(awk '$1 ~ /^\/dev\// {print $1}' $L/tank-layout.txt); do basename "$(readlink -f "$v")"; done | tee $D/disks
```

Expected: every boost line 0; `rc=0`; built and loaded srcversions equal (step 2's P3 line); `arc_sys_free` 345133056; `spl_kmem_alloc_max` 1048576; no pool before the import, `rc=0`, then `tank` only; the pool's devices as `/proc/diskstats` names them (whole disks or partitions, as the pool uses them). Stop if the srcversions differ, `spl_kmem_alloc_max` is not 1048576 (#19935's threshold derives from it), the import fails, or another pool shows. If `arc_sys_free` is not 345133056, go on: `$D/asf0` is what the arms without C3 must show, and the floor of the C3 arms (step 6); say so. Then:

```sh
D=/var/tmp/task-1121-ar3
sudo -n $D/dr2.sh start $D/smoke $(cat $D/disks); sleep 5; sudo -n $D/dr2.sh stop $D/smoke
grep -c ' S W=' $D/smoke.dr; grep -m1 -o 'rd_sect=[0-9]*' $D/smoke.dr; python3 -I $D/dr2-sum.py $D/smoke.dr | head -30; ls -d /sys/kernel/tracing/instances/dr 2>&1
```

Expected: about 20 samples, with `W=` and `rd_sect=` in them; no lost events and no `no ARC filter` warning in dr2.sh's lines; `dr2-sum.py` prints its block without an error; no `instances/dr` left. Otherwise stop and report. Say which disks you passed. Leave P3 loaded and `tank` imported and idle.

### 5. Note to main, then wait for GO

Step 6 must run alone on the host, on a quiet VM. End your turn with the interim results (step 0's lines; the four `OK` of step 1; step 2's fetch lines, the trees' tables as found, configure lines, warnings, cstyle and the srcversions against the table; `scripts.diff`'s length and the list of what you changed; step 4's boot, cmdline, sysctl and zone lines; the smoke test) and, as the very last line, exactly:

`TASK-1121-AR3 READY-TO-MEASURE (about 100 min alone on the host, at most 115). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 6 only on a message from main that contains `GO TASK-1121-AR3`; note the time of GO in `$L/go.txt` (`date +%s`). On `HOLD`, keep waiting. On `ABORT TASK-1121-AR3`, go to END without running anything. Nothing runs while you wait (P3 loaded, `tank` imported and idle).

### 6. Runs (alone on the host; about 100 min, never more than 115)

`seq.sh` with these changes, each quoted in `scripts.diff`:

**0. Paths:** AR's `/var/tmp/task-1116-ar` becomes `/var/tmp/task-1121-ar3/ar` and `/var/log/task-1116-ar` becomes `/var/log/task-1121-ar3`; `<outdir>` below is `seq.sh`'s output directory, under `/var/tmp/task-1121-ar3`. `seq.sh` may take the chain to run (item 6) as an argument.

**1. Arms:** the four of the Arms table and their trees. After every load, check that the loaded spl and zfs srcversions (`/sys/module/{spl,zfs}/srcversion`) are the arm's tree's (`modinfo -F srcversion ~/work/z-<arm>/module/{spl,zfs}.ko`, step 2's lines); a mismatch is a stop.

**2. A reload before every run,** each run starting from `zpool export tank` and a fresh load of its arm (as K35B made explicit; the order below changes arm at every run anyway).

**3. Every load** follows this, after `zpool export tank`:

```sh
zones() {   # one line per zone: name, present, free, boost, min, low, high (pages)
    awk '/^Node/{z=$4} $1=="pages"&&$2=="free"{f=$3} $1=="boost"{b=$2}
         $1=="min"{mi=$2} $1=="low"{lo=$2} $1=="high"{hi=$2}
         $1=="present"{print z, $2, f, b, mi, lo, hi}' /proc/zoneinfo
}
expected_asf() {   # <arm>: arc_sys_free the arm must have just after its load
    a0=$(cat /var/tmp/task-1121-ar3/asf0)
    case $1 in
    P3|P3C2) echo "$a0" ;;
    P3C3|P3A) awk -v a0="$a0" '/^Node/{z=$4} $1=="boost"{b=$2} $1=="high"{h=$2}
        $1=="present" && $2 > 0 && (z=="DMA" || z=="DMA32" || z=="Normal") {
        s += h - b } END { v = s * 20480; if (v < a0) v = a0;
        printf "%.0f\n", v }' /proc/zoneinfo ;;
    esac
}
clear_boost() {   # rewrites min_free_kbytes with its own value: the kernel
                  # recomputes the same watermarks and sets every boost to 0
    sysctl -q -w vm.min_free_kbytes="$(sysctl -n vm.min_free_kbytes)"
}
```

- Log `zones` before anything else (`<run>.zones-pre`).
- **Boost test, once:** the first time a C3 arm (P3C3 or P3A) is about to load and `<run>.zones-pre` shows a boost in any zone, load it **before** clearing (`scripts/zfs.sh -r` from its tree, srcversions checked), log `zones`, `arc_sys_free` and `expected_asf`, then `scripts/zfs.sh -u`. Pass if `arc_sys_free` equals `expected_asf` and 518963200 (or, if step 4 found other watermarks, the value of the boost-free C3 loads). This is the load that gave 1116 MiB in AR. If it fails, C3v2 is wrong: abort P3C3 and P3A and say so. If no C3 load ever sees a boost, say so.
- `clear_boost`, then log `zones` (`<run>.zones-load`): every boost must be 0. If one is not, `clear_boost` again, up to 3 times; then abort the arm.
- Load (`scripts/zfs.sh -r` from the arm's tree), check both srcversions, then check that `awk '$1=="arc_sys_free"{print $3}' /proc/spl/kstat/zfs/arcstats` equals `expected_asf <arm>`. If not, **abort the arm**: skip its remaining runs, go on with the others, and say so.
- Then import, `load-key -a`, `mount -a`, as AR did.

**4. dr2.sh instead of dr.sh,** around every run as in AR: `$D/dr2.sh start <outdir>/<run> $(cat $D/disks)` just before `t2.sh` or `control.sh`, `$D/dr2.sh stop <outdir>/<run>` just after.

**5. Passes:** for each encrypted run write `<outdir>/<run>.passes`, one line per complete pass: start epoch, end epoch, MB/s as `t2.sh` reports it, the epochs as `date +%s.%N` prints them. If `t2.sh` does not already give each pass's start and end wall-clock time, add a `date +%s.%N` line before and after each pass in `t2.sh`, nothing else.

**6. Order.** Idle 60 s first (steal, load1); if steal is above 2 % or load1 above 2 at its end, wait 2 minutes, measure once more, and go on either way, saying so. Then:
1. Encrypted runs, `t2.sh <run> enc /tank/enc/f <tree> 600 10 600` as in AR: P3-1, P3C2-1, P3C3-1, P3A-1, P3C3-2, P3A-2, P3-2, P3C2-2.
2. Controls, AR's `control.sh` (fragmem `<MemFree+1024> 2 60`, 3 s after the reads start): ctl-P3-1, ctl-P3C2-1, ctl-P3C3-1, ctl-P3A-1, ctl-P3C3-2, ctl-P3A-2, ctl-P3-2, ctl-P3C2-2.
3. lru_gen=n controls: `echo n > /sys/kernel/mm/lru_gen/enabled`, then ctl-P3-n1 and ctl-<best>-n1, then `echo y` and check that it reads 0x0007 again. `<best>` is the C arm (P3C2, P3C3 or P3A) with the lowest allocstall_normal + allocstall_movable summed over its encrypted runs and controls (dr2-sum.py's "over the samples" line); on a tie, fewer falls of `c` > 1 GiB; then the higher mean `c` in its encrypted runs. Run 1 and 2 as one detached chain, then pick `<best>` (say which it was and why), then run 3 as a second chain.

**7. Time rule.** Before each run marked `-2` (and before nothing else), `seq.sh` adds up now, 630 s for each encrypted run and 90 s for each control still to run, that run and the two lru_gen=n controls included and runs already skipped excluded. If the sum is past GO + 115 min (`$(cat $L/go.txt)` + 6900), it skips that run and logs `SKIP <run> (time rule)`. Never drop a run marked `-1`, the lru_gen=n controls, the boost test or the per-fall and per-call records.

Run each chain detached (`setsid nohup sudo -n bash ...`, its output in a file) and poll its log every few minutes. Stop and report if a run prints an OOM kill, an allocation failure or a hung task in dmesg. A failed run is not rerun. When the last run has ended, export `tank`, unload the modules (`scripts/zfs.sh -u` from z-P3), write in your chat the line `TASK-1121-AR3 MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 7 without waiting.

### 7. Report (about 15 min)

Run AR's analysis per run, then `python3 -I $D/dr2-sum.py --passes <outdir>/<run>.passes <outdir>/<run>.dr` for each encrypted run and `python3 -I $D/dr2-sum.py <outdir>/<run>.dr` for each control, all into `<outdir>/dr2-sum.txt`. dr2-sum.py counts a fall as a drop of `c` by more than 1 GiB within 5 s; if its count for a run differs from AR's analysis, give both.

**Builds:** the step 2 tables, as found: fetch lines, checksums, numstat, configure lines, srcversions expected and found, warnings, cstyle.

**Loads:** one line per load: run, the boosts before clearing (per zone), the boosts at load, `arc_sys_free`, `expected_asf`, pass/fail. Then the boost test: run, boosts at that load, `arc_sys_free`, expected.

**Encrypted runs,** per run and mean per arm:
- AR's columns, as in AR's table;
- **read throughput per pass** (dr2-sum.py's pass table): reader MB/s, disk MB/s, ARC data hit ratio and disk bytes per byte read, for the first pass and as the mean of the later passes;
- **falls of `c` > 1 GiB by cause:** how many came from a kswapd run of order 0, of order > 0, from direct reclaim, or from the ARC lowering `c` itself; for the kswapd runs, the wake-ups that woke them counted by gfp mask and caller (#19935's order-0 page from `spl_wake_kswapd_if_needed`, mask `GFP_NOWAIT|__GFP_HIGHMEM|__GFP_NOMEMALLOC` or as the trace prints it; boost wake-ups with gfp `none`; ABD `GFP_NOIO|…`; others by name); and at each fall the Normal zone's free pages against its boosted high, and its boost. Say whether the boost is what drives these kswapd runs;
- **each direct reclaim** (dr2-sum.py's per-call lines), counted by order, gfp mask, kind of caller and kswapd state (run going on or asleep), with the Normal zone's free pages against its boosted min and low. Give the per-call lines of P3's runs in full in an appendix;
- **order-0 wake-ups by #19935's page** per run (K35B's P2S: 282 and 376).

**Controls,** including the lru_gen=n ones: AR's columns, plus the per-call counts as above.

**lru_gen=n:** ctl-P3-n1 and ctl-<best>-n1 against AR's ctl-F-n1 (18 stalls: 9 decrypt, 9 ABD) and ctl-M-n1 (0), and against the same arms with MGLRU. Say what part of the controls' stalls MGLRU explains, and what the rest is, from the per-call lines: callers, gfp masks, orders, whether a kswapd run was going on, and the Normal zone against its boosted min and low at each stall.

**Differences larger than the spread** between two runs of the same arm: P3 against each C arm, and the C arms among themselves. A difference between two arm means counts only if it is larger than the spread (max − min) of each arm's two runs; otherwise write "no difference shown".

**Against earlier runs, for orientation only** (other modules, other sessions): P3 against AR's F1 (+7/+5 allocstall_normal, 7/7 falls, mean `c` 5473/5549 MiB) and K35B's P2S (+10/+17, 4/8, 5662/5453 MiB).

**Host load:** steal and load1 per run, with the UTC times and the time of GO; say whether steal was above 0.1 % in any run.

**Answers** to the four questions under "Why", one or two lines each, with the numbers they rest on.

## Expected

| | P3 | P3C2 | P3C3 | P3A |
| --- | --- | --- | --- | --- |
| `arc_sys_free` at every load | 345133056 | 345133056 | 518963200 | 518963200 |
| encrypted: Δ allocstall_normal | about +10 to +17 (K35B's P2S) | below P3 if H3 holds | below P3 if H2 holds | the lowest |
| encrypted: falls of `c` > 1 GiB | about 4 to 8, kswapd runs of order 0 | fewer if H3 holds | fewer if H2 holds | the fewest |
| mean `c` (encrypted) | about 5.5 GiB | about P3 | higher than P3 if its falls go, else up to 165 MiB lower (the larger reserve) | as P3C3 |
| kswapd wake-ups of order ≥ 4 | 0 | 0 | 0 | 0 |
| first pass MB/s | 476 | 476 | 476 | 476 |
| later passes: disk MB/s | about 471 | the same | the same | the same: a higher reader MB/s with the same disk MB/s is ARC hits |
| ctl-<arm>-n1 (P3 and the best C arm): Δ allocstall_normal | about 0 | about 0 | about 0 | about 0 |

What main will read from it:
- **C2 or C3 worth proposing on top of #19935:** they cut P3's stalls or falls by more than the spread between two P3 runs, at no cost in throughput.
- **A gain that is real:** a higher mean `c` with more ARC hits per pass; disk MB/s cannot rise above about 476.
- **P3's remainder:** what wakes the kswapd runs behind its falls, and which callers still reclaim directly.
- **Without MGLRU:** F-n's 18 stalls were the decrypt buffers and ABD pages that #19935's page addresses; if ctl-P3-n1 shows none, MGLRU and the missing kswapd wake-ups (H1) account for all of the control cost.

## END

- **Archive:** outputs (`.dr`, `.passes`, `.zones-*`, `.rec`, `.trace`, logs, `dr2-sum.txt`, AR's analysis output, the scripts with `scripts.diff`, the patches, the tools, the chain logs, steal, the smoke files, `asf0`, `disks`) in `$L/files.tar.gz`; then remove `/var/tmp/task-1121-ar3` whole.
- **Keep:** the pool `tank` (exported) and its key; every tree under `~/work`, the four new `z-P3*` included (a follow-up may rerun them); the Proxmox kernel packages; `$L`.
- **Restore:** `vm.compact_unevictable_allowed` and lru_gen as step 0 found them (on AR's day 1 and 0x0007), swap as found. `vm.min_free_kbytes` was only rewritten with its own value; the reboot discards the rest. `/etc/zfs/zpool.cache`: size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0, report it and leave it.
- **Boot:** back into the Debian 6.12 kernel by a plain reboot (the default entry), resuming by your own arrangement; then `uname -r`, `sudo -n grub-editenv list` (no `next_entry`), `lsmod | grep -E '^(zfs|spl) '` (nothing), `sudo -n fstrim -av`, `df -h / /var/tmp`, and report the final state at the top of the report (kernel, next_entry, modules, sysctl, swap, fstrim, free space, the reboots with their UTC times and how you resumed). Then send the full report as your last message.

## Stop conditions (all in one place)

Step 0's list; a copy of this file whose sha256 is not the one main gave; a patch or tool whose sha256 differs; a fetch failing twice, or no way to get `c8ac7824ffd6f71e220b8afb09b662cc61b906fc`; any commit, tree, parent, numstat or sha256 that differs; a failed build, configure's crypto line `yes`, a C3 tree without both configure `yes` lines, spl srcversions that differ between the trees or two trees with the same zfs srcversion; after the boot, a kernel other than 7.0.14-25-pve, a `next_entry` left, a module loaded or a debug option on the cmdline; at the smoke test, a srcversion mismatch, `spl_kmem_alloc_max` not 1048576, a failed import, another pool, or a dr2.sh smoke result other than expected; at any load, a srcversion mismatch; a pool other than `tank`; a hang (capture first); an OOM kill, an allocation failure or a hung task in dmesg (save dmesg and stacks under `$L`, report; reboot only if the VM is wedged). For each stop, report what you see, then go to END for the parts that apply (restore, boot back to Debian). An arm aborted by the load checks of step 6 (`expected_asf`, the boosts, the boost test) is not a stop: the other arms go on. A result against the expectations, a srcversion that differs only from the computed table, and a run skipped by the time rule are findings, not stops.

## Appendix: files carried by this task

Step 1 extracts each file from the lines between its `<<'EOF'` line and the next line `EOF`. Run in `/var/tmp/task-1121-ar3`, this block would write the same four files.

```sh
base64 -d > C2.patch <<'EOF'
ZGlmZiAtLWdpdCBhL21vZHVsZS96ZnMvYXJjLmMgYi9tb2R1bGUvemZzL2FyYy5jCmluZGV4
IDg0NzBiYTAyYTFmLi4wYTQ1MzExMGQzYyAxMDA2NDQKLS0tIGEvbW9kdWxlL3pmcy9hcmMu
YworKysgYi9tb2R1bGUvemZzL2FyYy5jCkBAIC01MDk3LDkgKzUwOTcsMjQgQEAgc3RhdGlj
IHZvaWQKIGFyY19hZGFwdCh1aW50NjRfdCBieXRlcykKIHsKIAkvKgotCSAqIFdha2UgcmVh
cCB0aHJlYWQgaWYgd2UgZG8gbm90IGhhdmUgYW55IGF2YWlsYWJsZSBtZW1vcnkKLQkgKi8K
LQlpZiAoYXJjX3JlY2xhaW1fbmVlZGVkKCkpIHsKKwkgKiBJZiB0aGUgc3lzdGVtIGlzIHNo
b3J0IG9mIG1lbW9yeSwgbG93ZXIgdGhlIHRhcmdldCBieSB0aGUgc2hvcnRmYWxsCisJICog
YXQgb25jZSBhbmQgd2FrZSB0aGUgcmVhcCB0aHJlYWQuICBUaGUgcmVhcCB0aHJlYWQgbG93
ZXJzIHRoZQorCSAqIHRhcmdldCBvbmx5IGFmdGVyIHJlYXBpbmcgdGhlIGttZW0gY2FjaGVz
IGFuZCBhIGRlbGF5IG9mCisJICogYXJjX2ttZW1fY2FjaGVfcmVhcF9yZXRyeV9tcywgd2hp
Y2ggbGV0cyBhIGZhc3QgYWxsb2NhdG9yIHRha2UgdGhlCisJICogZnJlZSBtZW1vcnkgZG93
biB0byB0aGUga2VybmVsJ3Mgd2F0ZXJtYXJrcyBmaXJzdC4gIENvdW50IHRoZQorCSAqIGV2
aWN0aW9uIGFscmVhZHkgcGVuZGluZyBhZ2FpbnN0IHRoZSBzaG9ydGZhbGwsIHNvIHRoYXQg
Y2FsbHMgbWFkZQorCSAqIGJlZm9yZSBpdCBjb21wbGV0ZXMgZG8gbm90IGxvd2VyIHRoZSB0
YXJnZXQgYWdhaW4gZm9yIHRoZSBzYW1lCisJICogc2hvcnRmYWxsLgorCSAqLworCWludDY0
X3QgYXZhaWwgPSBhcmNfYXZhaWxhYmxlX21lbW9yeSgpOworCWlmIChhdmFpbCA8IDApIHsK
KwkJdWludDY0X3QgbmVlZCA9IC1hdmFpbDsKKwkJdWludDY0X3QgYXNpemUgPSBhZ2dzdW1f
bG93ZXJfYm91bmQoJmFyY19zdW1zLmFyY3N0YXRfc2l6ZSk7CisJCXVpbnQ2NF90IGMgPSBh
cmNfYzsKKwkJdWludDY0X3QgcGVuZGluZyA9IGFzaXplID4gYyA/IGFzaXplIC0gYyA6IDA7
CisKKwkJaWYgKGMgPiBhcmNfY19taW4gJiYgbmVlZCA+IHBlbmRpbmcpCisJCQkodm9pZCkg
YXJjX3JlZHVjZV90YXJnZXRfc2l6ZShuZWVkIC0gcGVuZGluZyk7CiAJCXp0aHJfd2FrZXVw
KGFyY19yZWFwX3p0aHIpOwogCQlyZXR1cm47CiAJfQo=
EOF
base64 -d > C3v2.patch <<'EOF'
ZGlmZiAtLWdpdCBhL2NvbmZpZy9rZXJuZWwtem9uZS13YXRlcm1hcmsubTQgYi9jb25maWcv
a2VybmVsLXpvbmUtd2F0ZXJtYXJrLm00Cm5ldyBmaWxlIG1vZGUgMTAwNjQ0CmluZGV4IDAw
MDAwMDAwMDAwLi42M2RlMjg2ZDZiMgotLS0gL2Rldi9udWxsCisrKyBiL2NvbmZpZy9rZXJu
ZWwtem9uZS13YXRlcm1hcmsubTQKQEAgLTAsMCArMSw1MCBAQAorZG5sICMgU1BEWC1MaWNl
bnNlLUlkZW50aWZpZXI6IENEREwtMS4wCitkbmwgIworZG5sICMgUmVhZGluZyB0aGUgem9u
ZXMnIGhpZ2ggd2F0ZXJtYXJrcyBuZWVkcyBOT0RFX0RBVEEoKSwgd2hpY2ggcmVhZHMKK2Ru
bCAjIG5vZGVfZGF0YVtdLCBvciBjb250aWdfcGFnZV9kYXRhIHdpdGhvdXQgTlVNQS4gIEJv
dGggYXJlIGV4cG9ydGVkIG9uCitkbmwgIyB0aGUgYXJjaGl0ZWN0dXJlcyB0aGlzIHdhcyBj
aGVja2VkIG9uICh4ODYgYW5kIGFybTY0IHNpbmNlIGJlZm9yZQorZG5sICMgNC4xOCksIGJ1
dCBhIG1vZHVsZSBjYW5ub3QgYXNzdW1lIGl0LCBzbyBjaGVjayB0aGF0IHRoZSBvbmUgaW4g
dXNlIGlzLgorZG5sICMKK2RubCAjIDUuMCBzcGxpdCBlYWNoIHpvbmUgd2F0ZXJtYXJrIGlu
dG8gX3dhdGVybWFya1tdIGFuZCB3YXRlcm1hcmtfYm9vc3QsCitkbmwgIyBhbmQgdGhlICpf
d21hcmtfcGFnZXMoKSBoZWxwZXJzIHJldHVybiB0aGVpciBzdW0uICBCZWZvcmUgNS4wIHRo
ZXJlCitkbmwgIyBpcyBubyBib29zdCwgYW5kIHdhdGVybWFya1tdIGlzIHdoYXQgdGhvc2Ug
aGVscGVycyByZXR1cm4uCitkbmwgIworQUNfREVGVU4oW1pGU19BQ19LRVJORUxfU1JDX1pP
TkVfV0FURVJNQVJLXSwgWworCVpGU19MSU5VWF9URVNUX1NSQyhbbm9kZV9kYXRhXSwgWwor
CQkjaW5jbHVkZSA8bGludXgvbW16b25lLmg+CisJXSwgWworCQlzdHJ1Y3Qgem9uZSAqem9u
ZSBfX2F0dHJpYnV0ZV9fICgodW51c2VkKSkgPQorCQkgICAgJk5PREVfREFUQSgwKS0+bm9k
ZV96b25lc1swXTsKKwkJdW5zaWduZWQgbG9uZyBoaWdoIF9fYXR0cmlidXRlX18gKCh1bnVz
ZWQpKSA9CisJCSAgICBoaWdoX3dtYXJrX3BhZ2VzKHpvbmUpOworCV0pCisKKwlaRlNfTElO
VVhfVEVTVF9TUkMoW3pvbmVfd2F0ZXJtYXJrX2Jvb3N0XSwgWworCQkjaW5jbHVkZSA8bGlu
dXgvbW16b25lLmg+CisJXSwgWworCQlzdHJ1Y3Qgem9uZSAqem9uZSBfX2F0dHJpYnV0ZV9f
ICgodW51c2VkKSkgPSBOVUxMOworCQl1bnNpZ25lZCBsb25nIGhpZ2ggX19hdHRyaWJ1dGVf
XyAoKHVudXNlZCkpID0KKwkJICAgIHpvbmUtPl93YXRlcm1hcmtbV01BUktfSElHSF0gKyB6
b25lLT53YXRlcm1hcmtfYm9vc3Q7CisJXSkKK10pCisKK0FDX0RFRlVOKFtaRlNfQUNfS0VS
TkVMX1pPTkVfV0FURVJNQVJLXSwgWworCUFDX01TR19DSEVDS0lORyhbd2hldGhlciBOT0RF
X0RBVEEoKSBpcyBhdmFpbGFibGVdKQorCVpGU19MSU5VWF9URVNUX1JFU1VMVF9TWU1CT0wo
W25vZGVfZGF0YV0sCisJICAgIFsobm9kZV9kYXRhfGNvbnRpZ19wYWdlX2RhdGEpXSwKKwkg
ICAgW21tL251bWEuYyBtbS9tZW1ibG9jay5jIG1tL25vYm9vdG1lbS5jIGFyY2gveDg2L21t
L251bWEuYyBhcmNoL2FybTY0L21tL251bWEuYyBkcml2ZXJzL2Jhc2UvYXJjaF9udW1hLmNd
LCBbCisJCUFDX01TR19SRVNVTFQoeWVzKQorCQlBQ19ERUZJTkUoSEFWRV9OT0RFX0RBVEEs
IDEsIFtOT0RFX0RBVEEoKSBpcyBhdmFpbGFibGVdKQorCV0sWworCQlBQ19NU0dfUkVTVUxU
KG5vKQorCV0pCisKKwlBQ19NU0dfQ0hFQ0tJTkcoW3doZXRoZXIgc3RydWN0IHpvbmUgaGFz
IHdhdGVybWFya19ib29zdF0pCisJWkZTX0xJTlVYX1RFU1RfUkVTVUxUKFt6b25lX3dhdGVy
bWFya19ib29zdF0sIFsKKwkJQUNfTVNHX1JFU1VMVCh5ZXMpCisJCUFDX0RFRklORShIQVZF
X1pPTkVfV0FURVJNQVJLX0JPT1NULCAxLAorCQkgICAgW3N0cnVjdCB6b25lIGhhcyB3YXRl
cm1hcmtfYm9vc3RdKQorCV0sWworCQlBQ19NU0dfUkVTVUxUKG5vKQorCV0pCitdKQpkaWZm
IC0tZ2l0IGEvY29uZmlnL2tlcm5lbC5tNCBiL2NvbmZpZy9rZXJuZWwubTQKaW5kZXggYjk1
YzZjOTQ4NDMuLjM1YzgzODk4OWRkIDEwMDY0NAotLS0gYS9jb25maWcva2VybmVsLm00Cisr
KyBiL2NvbmZpZy9rZXJuZWwubTQKQEAgLTQzLDYgKzQzLDcgQEAgQUNfREVGVU4oW1pGU19B
Q19LRVJORUxfVEVTVF9TUkNdLCBbCiAJWkZTX0FDX0tFUk5FTF9TUkNfQUNDRVNTX09LX1RZ
UEUKIAlaRlNfQUNfS0VSTkVMX1NSQ19QREVfREFUQQogCVpGU19BQ19LRVJORUxfU1JDX0dF
TkVSSUNfRkFEVklTRQorCVpGU19BQ19LRVJORUxfU1JDX1pPTkVfV0FURVJNQVJLCiAJWkZT
X0FDX0tFUk5FTF9TUkNfU0NIRUQKIAlaRlNfQUNfS0VSTkVMX1NSQ19VU0xFRVBfUkFOR0UK
IAlaRlNfQUNfS0VSTkVMX1NSQ19WTUFMTE9DX1BBR0VfS0VSTkVMCkBAIC0xNjMsNiArMTY0
LDcgQEAgQUNfREVGVU4oW1pGU19BQ19LRVJORUxfVEVTVF9SRVNVTFRdLCBbCiAJWkZTX0FD
X0tFUk5FTF9PQkpUT09MCiAJWkZTX0FDX0tFUk5FTF9QREVfREFUQQogCVpGU19BQ19LRVJO
RUxfR0VORVJJQ19GQURWSVNFCisJWkZTX0FDX0tFUk5FTF9aT05FX1dBVEVSTUFSSwogCVpG
U19BQ19LRVJORUxfU0NIRUQKIAlaRlNfQUNfS0VSTkVMX1VTTEVFUF9SQU5HRQogCVpGU19B
Q19LRVJORUxfVk1BTExPQ19QQUdFX0tFUk5FTApkaWZmIC0tZ2l0IGEvbW9kdWxlL29zL2xp
bnV4L3pmcy9hcmNfb3MuYyBiL21vZHVsZS9vcy9saW51eC96ZnMvYXJjX29zLmMKaW5kZXgg
NmZkYWZlMmM3ODEuLjI0NzI3NzBhMWE4IDEwMDY0NAotLS0gYS9tb2R1bGUvb3MvbGludXgv
emZzL2FyY19vcy5jCisrKyBiL21vZHVsZS9vcy9saW51eC96ZnMvYXJjX29zLmMKQEAgLTMz
NCw2ICszMzQsNDMgQEAgYXJjX3NldF9zeXNfZnJlZSh1aW50NjRfdCBhbGxtZW0pCiAJICog
dG8gM3ggdG8gZW5zdXJlIHdlJ3JlIGFib3ZlIGl0LgogCSAqLwogCWFyY19zeXNfZnJlZSA9
IHdtYXJrICogMyArIGFsbG1lbSAvIDMyOworCisjaWZkZWYgSEFWRV9OT0RFX0RBVEEKKwkv
KgorCSAqIFRoZSBlc3RpbWF0ZSBhYm92ZSBmb2xsb3dzIHRoZSBkZWZhdWx0IG1pbl9mcmVl
X2tieXRlcy4gIFRoZSBrZXJuZWwKKwkgKiByYWlzZXMgbWluX2ZyZWVfa2J5dGVzIHdoZW4g
dHJhbnNwYXJlbnQgaHVnZSBwYWdlcyBhcmUgZW5hYmxlZCAoc2VlCisJICogc2V0X3JlY29t
bWVuZGVkX21pbl9mcmVlX2tieXRlcygpKSwgYW5kIHNvIG1heSB0aGUgYWRtaW5pc3RyYXRv
ciwKKwkgKiB3aGljaCBjYW4gcHV0IHRoZSBib29zdGVkIGhpZ2ggd2F0ZXJtYXJrcyBhYm92
ZSBhcmNfc3lzX2ZyZWUgLyAyLgorCSAqIFRoZSBBUkMgd291bGQgdGhlbiBzZXR0bGUgd2hl
cmUgdGhlIGtlcm5lbCBhbHJlYWR5IHJlY2xhaW1zLCBhbmQKKwkgKiBhbGxvY2F0aW9ucyB3
b3VsZCBzdGFsbCBpbiBkaXJlY3QgcmVjbGFpbS4gIFNvIGFsc28gc3RheSBhYm92ZQorCSAq
IHR3aWNlIHRoZSBoaWdoIHdhdGVybWFya3MgdGhlIGtlcm5lbCB1c2VzLCBib29zdGVkIGJ5
IHVwIHRvIDE1MCUuCisJICoKKwkgKiBTdW0gdGhlIHVuYm9vc3RlZCB3YXRlcm1hcmtzLiAg
aGlnaF93bWFya19wYWdlcygpIGFkZHMgdGhlIHpvbmUncworCSAqIGN1cnJlbnQgd2F0ZXJt
YXJrX2Jvb3N0LCB3aGljaCBhIGZyYWdtZW50YXRpb24gZXZlbnQgcmFpc2VzIGFuZCBvbmx5
CisJICoga3N3YXBkJ3MgbmV4dCBiYWxhbmNpbmcgb2YgdGhlIG5vZGUgY2xlYXJzLCBzbyBp
dCB3b3VsZCBtYWtlCisJICogYXJjX3N5c19mcmVlIGRlcGVuZCBvbiB3aGF0IHJhbiBiZWZv
cmUgdGhlIG1vZHVsZSB3YXMgbG9hZGVkLgorCSAqLworCXVpbnQ2NF90IGhpZ2ggPSAwOwor
CWludCBuaWQ7CisKKwlmb3JfZWFjaF9vbmxpbmVfbm9kZShuaWQpIHsKKwkJcGdfZGF0YV90
ICpwZ2RhdCA9IE5PREVfREFUQShuaWQpOworCisJCWZvciAoaW50IGkgPSAwOyBpIDw9IFpP
TkVfTk9STUFMOyBpKyspIHsKKwkJCXN0cnVjdCB6b25lICp6b25lID0gJnBnZGF0LT5ub2Rl
X3pvbmVzW2ldOworCisJCQlpZiAoIXBvcHVsYXRlZF96b25lKHpvbmUpKQorCQkJCWNvbnRp
bnVlOworI2lmZGVmIEhBVkVfWk9ORV9XQVRFUk1BUktfQk9PU1QKKwkJCWhpZ2ggKz0gcHRv
Yih6b25lLT5fd2F0ZXJtYXJrW1dNQVJLX0hJR0hdKTsKKyNlbHNlCisJCQloaWdoICs9IHB0
b2IoaGlnaF93bWFya19wYWdlcyh6b25lKSk7CisjZW5kaWYKKwkJfQorCX0KKwloaWdoICs9
IGhpZ2ggKiAxNTAgLyAxMDA7CisJYXJjX3N5c19mcmVlID0gTUFYKGFyY19zeXNfZnJlZSwg
MiAqIGhpZ2gpOworI2VuZGlmCiB9CiAKIHZvaWQK
EOF
cat > dr2.sh <<'EOF'
#!/bin/sh
# dr2.sh start <prefix> [disk...] | stop <prefix>
# AR's dr.sh, extended for AR2.  In a tracing instance of its own ("dr";
# the main instance that trace.sh uses is not touched) it records:
# - every direct reclaim: begin (order, gfp flags, kernel stack) and end
#   (pages reclaimed);
# - every wake-up of a sleeping kswapd, of any order (dr.sh kept order 0
#   only), with gfp flags, task and kernel stack;
# - every kswapd run (mm_vmscan_kswapd_wake, with the order it balances
#   for) and every time kswapd goes to sleep;
# - every request to the ARC shrinker;
# - 4 times per second a sample line in the same buffer: wall clock (W=),
#   free memory, inactive file pages, each zone's free pages, boost and
#   (boosted) min/low/high watermarks, ARC size and data hits/misses,
#   reclaim counters, and the sectors read from the given disks (rd_sect=).
# stop: stop both and save everything in <prefix>.dr.
T=/sys/kernel/tracing
I=$T/instances/dr
A=/proc/spl/kstat/zfs/arcstats
EV="mm_vmscan_direct_reclaim_begin mm_vmscan_direct_reclaim_end
    mm_vmscan_wakeup_kswapd mm_vmscan_kswapd_wake mm_vmscan_kswapd_sleep
    mm_shrink_slab_start mm_shrink_slab_end"

case "$1" in
start)
    p=${2:?prefix}
    shift 2
    disks="$*"
    [ -d $T/events ] || mount -t tracefs nodev $T
    [ -d $I ] && rmdir $I
    mkdir $I || exit 1
    echo 4096 > $I/buffer_size_kb
    echo mono > $I/trace_clock
    V=$I/events/vmscan
    echo stacktrace > $V/mm_vmscan_direct_reclaim_begin/trigger
    echo stacktrace > $V/mm_vmscan_wakeup_kswapd/trigger
    for e in mm_shrink_slab_start mm_shrink_slab_end; do
        echo 'shrink.function == arc_shrinker_scan' > $V/$e/filter \
            2>/dev/null || echo "WARNING: no ARC filter on $e"
    done
    for e in $EV; do
        echo 1 > "$V/$e/enable"
    done
    echo 1 > $I/tracing_on
    (
        while [ -d $I ]; do
            w=$(date +%s.%N)
            s=$(awk '$1=="MemFree:"{f=$2} $1=="Inactive(file):"{i=$2}
                END{printf "MemFree_kB=%d Inactive_file_kB=%d ", f, i}' \
                /proc/meminfo)
            z=$(awk '/^Node/{n=$2; sub(",", "", n); z=$4}
                $1=="pages" && $2=="free"{f=$3}
                $1=="boost"{b=$2} $1=="min"{mi=$2} $1=="low"{lo=$2}
                $1=="high"{printf "z%s_%s=%d/%d/%d/%d/%d ",
                n, z, f, b, mi, lo, $2}' /proc/zoneinfo)
            a=$(awk '$1=="c"||$1=="size"||$1=="arc_sys_free"|| \
                $1=="arc_need_free"||$1=="memory_direct_count"|| \
                $1=="memory_indirect_count"||$1=="demand_data_hits"|| \
                $1=="demand_data_iohits"||$1=="demand_data_misses"|| \
                $1=="prefetch_data_hits"||$1=="prefetch_data_iohits"|| \
                $1=="prefetch_data_misses"{printf "%s=%s ", $1, $3}' \
                $A 2>/dev/null)
            v=$(awk '$1=="allocstall_normal"|| \
                $1=="allocstall_movable"||$1=="pgscan_direct"|| \
                $1=="pgsteal_direct"||$1=="pageoutrun"|| \
                $1=="compact_stall"||$1=="oom_kill" \
                {printf "%s=%s ", $1, $2}' /proc/vmstat)
            d=$(awk -v d="$disks" 'BEGIN{n=split(d, x, " ");
                for (i = 1; i <= n; i++) want[x[i]] = 1}
                ($3 in want){r+=$6} END{printf "rd_sect=%.0f", r}' \
                /proc/diskstats)
            echo "S W=$w $s$z$a$v$d" > $I/trace_marker 2>/dev/null
            sleep 0.25
        done
    ) > /dev/null 2>&1 &
    echo $! > /run/dr-sampler.pid
    echo "dr2.sh: tracing into $I, sampler pid $!, disks: ${disks:-none}"
    ;;
stop)
    p=${2:?prefix}
    [ -d $I ] || { echo "dr2.sh: not started"; exit 1; }
    kill "$(cat /run/dr-sampler.pid)" 2>/dev/null
    rm -f /run/dr-sampler.pid
    sleep 0.5
    echo 0 > $I/tracing_on
    cat $I/trace > "$p.dr"
    V=$I/events/vmscan
    for e in $EV; do
        echo 0 > "$V/$e/enable"
    done
    echo '!stacktrace' > $V/mm_vmscan_direct_reclaim_begin/trigger
    echo '!stacktrace' > $V/mm_vmscan_wakeup_kswapd/trigger
    echo 0 > $V/mm_shrink_slab_start/filter
    echo 0 > $V/mm_shrink_slab_end/filter
    rmdir $I || echo "dr2.sh: could not remove $I"
    echo "dr2.sh: $p.dr: $(grep -c . "$p.dr") lines," \
        "$(grep -c ' S W=' "$p.dr") samples," \
        "lost: $(grep -o 'LOST [0-9]* EVENTS' "$p.dr" | head -1)"
    ;;
*)
    echo "usage: $0 start <prefix> [disk...] | stop <prefix>" >&2
    exit 2
    ;;
esac
EOF
cat > dr2-sum.py <<'EOF'
#!/usr/bin/env python3
"""dr2-sum.py [--passes FILE] <run.dr>...

Summarises the traces saved by "dr2.sh stop", one block per file.  The
lines dr-sum.py printed come first and read the same way:
- direct reclaim: number of calls, their total and longest duration, pages
  reclaimed per call, how many called the ARC shrinker, calls by gfp flags,
  by task, by kind of caller and by the innermost frames above the
  allocator;
- order-0 wake-ups of a sleeping kswapd by gfp flags and by task;
- ARC shrinker requests from kswapd by priority;
- from the 4-per-second samples: lowest MemFree, per zone the largest boost
  and the time below the boosted low and min watermarks, arc_sys_free, the
  ARC target, and counter differences.
Then, new in dr2-sum.py:
- each direct reclaim call: time, order, gfp flags, task, caller, pages
  reclaimed, ARC shrinker batches, whether a kswapd run was going on
  (between a kswapd_wake and the next kswapd_sleep), and MemFree,
  Inactive(file) and the Normal zone at the sample before it;
- wake-ups of a sleeping kswapd of every order: by order, and those of
  order > 0 by gfp flags and innermost frames; kswapd runs by the order
  they balance for;
- falls of c, a fall being a drop of c by more than 1 GiB within 5 s of
  samples.  For each: when, c before and after, MemFree, Inactive(file),
  arc_sys_free and the Normal zone at its start, the ARC shrinker requests
  in it (from 1 s before its start to its end), and for the kswapd run
  that made the request of lowest priority number: the order it balanced
  for and the wake-ups that woke it (order, gfp flags, task, caller).  A
  fall with no shrinker request is the ARC lowering c itself (reap thread,
  or arc_adapt with C2);
- with --passes FILE, which needs a single .dr file: FILE has one line per
  read pass, "<start epoch> <end epoch> [MB/s]", epochs as `date +%s.%N`
  prints them.  Per pass: the reader's MB/s if given, the MB/s read from
  the disks given to dr2.sh, the ARC data hit ratio, the bytes read from
  the disks per byte read by the reader, and the mean c; then the first
  pass against the mean of the later ones.
Standard library only.
"""
import bisect
import math
import re
import sys
from collections import Counter

LINE = re.compile(r'^\s*(?P<comm>.*?)-(?P<pid>\d+)\s+(?:\(\s*[-\d]+\)\s+)?'
                  r'\[\d+\]\s+(?:\S+\s+)?(?P<ts>\d+\.\d+):\s+'
                  r'(?P<ev><stack trace>|\w+):?\s?(?P<rest>.*)$')
FRAME = re.compile(r'^\s*=>\s+(\S+)')
KV = re.compile(r'(\w+)=(\S+)')
ARC_START = re.compile(r'nid: (\d+) .*cache items (\d+) delta (-?\d+) '
                       r'.*priority (-?\d+)')
ARC_END = re.compile(r'last shrinker return val (-?\d+)')
MIB = 1 << 20
GIB = 1 << 30
PAGE = 4096
STACKED = ('mm_vmscan_direct_reclaim_begin', 'mm_vmscan_wakeup_kswapd')
MAX_LIST = 40

# Frames of the tracing, page allocator, kswapd wake-up and reclaim code:
# skipped to find who allocated.
SKIP = re.compile(r'^(trace_|__traceiter|__trace_|event_|ftrace|'
                  r'perf_trace|stack_trace|__bpf|'
                  r'try_to_free_pages|__perform_reclaim|'
                  r'wakeup_kswapd|wake_all_kswapds|rmqueue|__rmqueue|'
                  r'__alloc_pages|alloc_pages|__alloc_frozen|'
                  r'alloc_frozen|get_page_from_freelist|'
                  r'__folio_alloc|folio_alloc|alloc_pages_mpol|'
                  r'alloc_pages_bulk|__alloc_pages_bulk|'
                  r'vm_area_alloc_pages|__vmalloc_area_node|'
                  r'__vmalloc_node_range|__vmalloc_node|__vmalloc_noprof|'
                  r'__kmalloc_large|___kmalloc_large|__kmalloc_node|'
                  r'kmalloc_large|allocate_slab|new_slab|___slab_alloc|'
                  r'__slab_alloc|kmem_cache_alloc|__kmalloc)')


def kind(frames):
    s = ' '.join(frames)
    if 'gcm_mode_decrypt' in s or 'ccm_mode_decrypt' in s:
        return 'decrypt buffer (ICP)'
    if 'vm_area_alloc_pages' in s or 'vmalloc' in s:
        return 'vmalloc, other caller'
    if 'abd_alloc' in s:
        return 'ABD pages'
    if 'kmalloc' in s or 'slab' in s or 'kmem_cache' in s:
        return 'kmalloc/slab'
    if 'handle_mm_fault' in s or 'do_anonymous_page' in s or \
            'do_user_addr_fault' in s:
        return 'user page fault'
    if not frames:
        return '(no stack)'
    return 'other'


def caller(frames, n=3):
    fr = [f for f in frames if not SKIP.match(f)]
    return ' <- '.join(fr[:n]) or '(no stack)'


def parse(path):
    """Return [ts, comm, pid, ev, rest, frames] with stacks attached."""
    evs = []
    frames = None
    with open(path, errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                continue
            fm = FRAME.match(line)
            if fm:
                if frames is not None:
                    frames.append(fm.group(1).split('+')[0])
                continue
            m = LINE.match(line)
            if not m:
                continue
            ev = m['ev'].strip()
            pid = int(m['pid'])
            if ev == '<stack trace>':
                # attach to the last stack-triggered event of that pid
                frames = []
                for e in reversed(evs[-200:]):
                    if e[2] == pid and e[3] in STACKED and not e[5]:
                        e[5] = frames
                        break
                continue
            frames = None
            evs.append([float(m['ts']), m['comm'].strip(), pid, ev,
                        m['rest'], []])
    return evs


def pct(n, d):
    return '%.0f%%' % (100.0 * n / d) if d else '-'


def num(s, k):
    try:
        return int(s[k])
    except (KeyError, ValueError):
        return None


def fnum(s, k):
    try:
        return float(s[k])
    except (KeyError, ValueError):
        return None


class Run:
    def __init__(self, path):
        self.path = path
        self.drs = []       # dict per direct reclaim call
        self.wakes = []     # (ts, order, gfp, comm, frames)
        self.kswapd = []    # (ts, nid, 'wake'|'sleep', order)
        self.arc = []       # (ts, comm, pid, nid, priority, delta, direct)
        self.samples = []   # (ts, dict)
        open_dr = {}
        for ts, comm, pid, ev, rest, frames in parse(path):
            d = dict(KV.findall(rest))
            if ev == 'mm_vmscan_direct_reclaim_begin':
                open_dr[pid] = {'ts': ts, 'order': num(d, 'order'),
                                'gfp': d.get('gfp_flags', '?'),
                                'comm': comm, 'frames': frames,
                                'arc': 0, 'cred': 0}
            elif ev == 'mm_vmscan_direct_reclaim_end':
                o = open_dr.pop(pid, None)
                if o is None:
                    continue
                o['dur'] = ts - o['ts']
                o['nr'] = num(d, 'nr_reclaimed') or 0
                self.drs.append(o)
            elif ev == 'mm_shrink_slab_start':
                m = ARC_START.search(rest)
                if not m:
                    continue
                direct = not comm.startswith('kswapd')
                if direct and pid in open_dr:
                    open_dr[pid]['arc'] += 1
                self.arc.append((ts, comm, pid, int(m.group(1)),
                                 int(m.group(4)), int(m.group(3)), direct))
            elif ev == 'mm_shrink_slab_end':
                m = ARC_END.search(rest)
                if m and pid in open_dr and not comm.startswith('kswapd'):
                    open_dr[pid]['cred'] += max(int(m.group(1)), 0)
            elif ev == 'mm_vmscan_wakeup_kswapd':
                self.wakes.append((ts, num(d, 'order'),
                                   d.get('gfp_flags', '?'), comm, frames))
            elif ev == 'mm_vmscan_kswapd_wake':
                self.kswapd.append((ts, num(d, 'nid'), 'wake',
                                    num(d, 'order')))
            elif ev == 'mm_vmscan_kswapd_sleep':
                self.kswapd.append((ts, num(d, 'nid'), 'sleep', None))
            elif ev == 'tracing_mark_write' and rest.startswith('S '):
                self.samples.append((ts, dict(KV.findall(rest))))
        self.open_dr = len(open_dr)
        self.sts = [s[0] for s in self.samples]
        self.normal = sorted(k for _, s in self.samples[:1] for k in s
                             if re.match(r'z\d+_Normal$', k))

    def sample_at(self, ts):
        if not self.samples:
            return {}
        i = bisect.bisect_right(self.sts, ts) - 1
        return self.samples[max(i, 0)][1]

    def t(self, ts):
        return ts - (self.sts[0] if self.sts else 0)

    def mem(self, ts):
        """MemFree, Inactive(file) and Normal zone at the sample before."""
        s = self.sample_at(ts)
        out = 'MemFree %s, Inactive(file) %s MiB' % (
            num(s, 'MemFree_kB') // 1024 if num(s, 'MemFree_kB') is not None
            else '?',
            num(s, 'Inactive_file_kB') // 1024
            if num(s, 'Inactive_file_kB') is not None else '?')
        for z in self.normal:
            try:
                f, b, mi, lo, hi = [int(x) for x in s[z].split('/')]
                out += ('; %s free %d, boost %d, boosted min/low/high '
                        '%d/%d/%d pages' % (z, f, b, mi, lo, hi))
            except (KeyError, ValueError):
                pass
        return out

    def kswapd_state(self, ts, nid=0):
        last = None
        for k in self.kswapd:
            if k[0] > ts:
                break
            if k[1] == nid:
                last = k
        if last is None:
            return 'kswapd: no event yet'
        if last[2] == 'wake':
            return 'kswapd run (order %s) since %.1f s' % (
                last[3], ts - last[0])
        return 'kswapd asleep since %.1f s' % (ts - last[0])

    def run_wakers(self, ts, nid):
        """The kswapd run going on at ts, and the wake-ups that woke it."""
        wake = None
        bound = None
        for k in self.kswapd:
            if k[0] > ts:
                break
            if k[1] != nid:
                continue
            if k[2] == 'wake':
                bound = wake[0] if wake else bound
                wake = k
            else:
                bound = k[0]
                wake = None
        if wake is None:
            return None, []
        lo = bound if bound is not None else -1.0
        return wake, [w for w in self.wakes if lo < w[0] <= wake[0]]


def print_basic(r):
    drs = r.drs
    n = len(drs)
    durs = sorted(d['dur'] for d in drs)
    print('direct reclaim calls: %d (still open at the end: %d)' %
          (n, r.open_dr))
    if n:
        tot = sum(durs)
        print('  duration: total %.1f ms, mean %.3f ms, p99 %.2f ms, '
              'max %.2f ms' % (tot * 1e3, tot * 1e3 / n,
                               durs[max(math.ceil(0.99 * n) - 1, 0)] * 1e3,
                               durs[-1] * 1e3))
        h = Counter()
        for d in drs:
            x = d['nr']
            h['0' if x == 0 else '1-31' if x < 32 else
              '32-511' if x < 512 else '>=512'] += 1
        print('  pages reclaimed per call: ' + ', '.join(
            '%s: %d' % (k, h[k]) for k in ('0', '1-31', '32-511', '>=512')) +
            '; total %d pages' % sum(d['nr'] for d in drs))
        arc = [d for d in drs if d['arc']]
        print('  calls that reached the ARC shrinker: %d (%s), ARC batches '
              '%d, credited %d pages' % (len(arc), pct(len(arc), n),
                                         sum(d['arc'] for d in arc),
                                         sum(d['cred'] for d in arc)))
        print('  by order: ' + ', '.join(
            '%s: %d' % (o, c) for o, c in
            sorted(Counter(d['order'] for d in drs).items(),
                   key=lambda x: (x[0] is None, x[0]))))
        print('  by gfp flags: ' + '; '.join(
            '%d %s' % (c, g) for g, c in
            Counter(d['gfp'] for d in drs).most_common(5)))
        print('  by task: ' + ', '.join(
            '%d %s' % (c, g) for g, c in
            Counter(d['comm'] for d in drs).most_common(6)))
        print('  by kind of caller: ' + ', '.join(
            '%d %s' % (c, g) for g, c in
            Counter(kind(d['frames']) for d in drs).most_common()))
        print('  by innermost frames above the allocator:')
        for k, c in Counter(caller(d['frames'], 4)
                            for d in drs).most_common(6):
            print('    %6d %s' % (c, k))
    w0 = [w for w in r.wakes if w[1] == 0]
    print('order-0 kswapd wake-ups: %d; by gfp flags: %s; by task: '
          '%s' % (len(w0),
                  '; '.join('%d %s' % (c, g) for g, c in
                            Counter(w[2] for w in w0).most_common(5)) or '-',
                  ', '.join('%d %s' % (c, g) for g, c in
                            Counter(w[3] for w in w0).most_common(5)) or '-'))
    ka = Counter(a[4] for a in r.arc if not a[6])
    print('ARC shrinker requests from kswapd by priority: ' + (', '.join(
        '%d:%d' % (p, ka[p]) for p in sorted(ka)) or '-'))


def print_samples(r):
    samples = r.samples
    if not samples:
        print('no samples')
        return
    t0 = samples[0][0]
    mf = [(num(s, 'MemFree_kB'), ts) for ts, s in samples
          if num(s, 'MemFree_kB') is not None]
    lo = min(mf)
    print('samples: %d over %.0f s; lowest MemFree %d MiB at +%.1f s; '
          'Inactive(file) %d..%d MiB' % (
              len(samples), samples[-1][0] - t0, lo[0] // 1024, lo[1] - t0,
              min(num(s, 'Inactive_file_kB') or 0
                  for _, s in samples) // 1024,
              max(num(s, 'Inactive_file_kB') or 0
                  for _, s in samples) // 1024))
    zones = sorted({k for _, s in samples for k in s
                    if re.match(r'z\d+_', k)})
    dt = (samples[-1][0] - t0) / max(len(samples) - 1, 1)
    for z in zones:
        vals = []
        for _, s in samples:
            try:
                vals.append([int(x) for x in s[z].split('/')])
            except (KeyError, ValueError):
                pass
        if not vals or max(v[2] for v in vals) == 0:
            continue
        mib = PAGE / MIB
        below_low = sum(1 for v in vals if v[0] < v[3])
        below_min = sum(1 for v in vals if v[0] < v[2])
        print('  %s: free %d..%d MiB, boost max %d MiB (%s of samples '
              'boosted), unboosted min/low/high %.0f/%.0f/%.0f MiB, '
              'below boosted low %.1f s, below boosted min %.1f s' % (
                  z, min(v[0] for v in vals) * mib,
                  max(v[0] for v in vals) * mib,
                  max(v[1] for v in vals) * mib,
                  pct(sum(1 for v in vals if v[1]), len(vals)),
                  (vals[0][2] - vals[0][1]) * mib,
                  (vals[0][3] - vals[0][1]) * mib,
                  (vals[0][4] - vals[0][1]) * mib,
                  below_low * dt, below_min * dt))
    asf = [num(s, 'arc_sys_free') for _, s in samples
           if num(s, 'arc_sys_free') is not None]
    cs = [num(s, 'c') for _, s in samples if num(s, 'c') is not None]
    if asf and cs:
        print('  arc_sys_free %d MiB; c start %d, lowest %d, mean %d MiB' % (
            asf[0] // MIB, cs[0] // MIB, min(cs) // MIB,
            sum(cs) // len(cs) // MIB))
    first, last = samples[0][1], samples[-1][1]
    diffs = []
    for k in ('allocstall_normal', 'allocstall_movable', 'pgscan_direct',
              'pgsteal_direct', 'pageoutrun', 'compact_stall', 'oom_kill',
              'memory_direct_count', 'memory_indirect_count'):
        a, b = num(first, k), num(last, k)
        if a is not None and b is not None:
            diffs.append('%s +%d' % (k, b - a))
    print('  over the samples: ' + ', '.join(diffs))


def print_calls(r):
    if not r.drs:
        return
    print('each direct reclaim call (t from the first sample):')
    for d in r.drs[:MAX_LIST]:
        print('  +%.2f s %.2f ms order %s %s, %s, %s [%s]; reclaimed %d, '
              'ARC batches %d; %s; %s' % (
                  r.t(d['ts']), d['dur'] * 1e3, d['order'], d['gfp'],
                  d['comm'], caller(d['frames']), kind(d['frames']),
                  d['nr'], d['arc'], r.kswapd_state(d['ts']),
                  r.mem(d['ts'])))
    if len(r.drs) > MAX_LIST:
        print('  ... %d more' % (len(r.drs) - MAX_LIST))


def print_wakes(r):
    print('kswapd wake-ups of any order: %d; by order: %s' % (
        len(r.wakes), ', '.join('%s:%d' % (o, c) for o, c in sorted(
            Counter(w[1] for w in r.wakes).items(),
            key=lambda x: (x[0] is None, x[0]))) or '-'))
    hi = [w for w in r.wakes if w[1]]
    if hi:
        print('  order > 0 by gfp flags: ' + '; '.join(
            '%d %s' % (c, g) for g, c in
            Counter(w[2] for w in hi).most_common(4)))
        print('  order > 0 by innermost frames:')
        for k, c in Counter('order %s: %s' % (w[1], caller(w[4], 4))
                            for w in hi).most_common(6):
            print('    %6d %s' % (c, k))
    runs = [k for k in r.kswapd if k[2] == 'wake']
    print('kswapd runs: %d; by order: %s; went to sleep %d times' % (
        len(runs), ', '.join('%s:%d' % (o, c) for o, c in sorted(
            Counter(k[3] for k in runs).items(),
            key=lambda x: (x[0] is None, x[0]))) or '-',
        sum(1 for k in r.kswapd if k[2] == 'sleep')))


def find_falls(r):
    cs = [(ts, num(s, 'c')) for ts, s in r.samples if num(s, 'c') is not None]
    falls = []
    n = len(cs)
    i = 0
    while i < n:
        mn = i
        k = i
        while k + 1 < n and cs[k + 1][0] - cs[i][0] <= 5.0:
            k += 1
            if cs[k][1] < cs[mn][1]:
                mn = k
        if cs[i][1] - cs[mn][1] > GIB:
            st = max(range(i, mn + 1), key=lambda x: (cs[x][1], x))
            e = mn
            while e + 1 < n and cs[e + 1][1] < cs[e][1]:
                e += 1
            falls.append((cs[st], cs[e]))
            i = e + 1
        else:
            i += 1
    return falls


def print_falls(r):
    falls = find_falls(r)
    print('falls of c by more than 1 GiB within 5 s: %d' % len(falls))
    for (t0, c0), (t1, c1) in falls[:MAX_LIST]:
        s = r.sample_at(t0)
        avail = None
        if None not in (num(s, 'MemFree_kB'), num(s, 'Inactive_file_kB'),
                        num(s, 'arc_sys_free')):
            avail = (num(s, 'MemFree_kB') + num(s, 'Inactive_file_kB')) * \
                1024 - num(s, 'arc_sys_free')
        print('  fall at +%.1f s, %.1f s long: c %d -> %d MiB; %s; '
              'free + inactive(file) - arc_sys_free %s MiB' % (
                  r.t(t0), t1 - t0, c0 // MIB, c1 // MIB, r.mem(t0),
                  avail // MIB if avail is not None else '?'))
        win = [a for a in r.arc if t0 - 1.0 <= a[0] <= t1]
        ks = [a for a in win if not a[6]]
        dr = [a for a in win if a[6]]
        print('    ARC shrinker requests: from kswapd %s; from direct '
              'reclaim %d%s' % (
                  ', '.join('prio %d: %d' % (p, c) for p, c in
                            sorted(Counter(a[4] for a in ks).items()))
                  or 'none', len(dr),
                  (' (' + ', '.join('%d %s' % (c, g) for g, c in
                                    Counter(a[1] for a in dr).most_common(3))
                   + ')') if dr else ''))
        if ks:
            a = min(ks, key=lambda x: (x[4], x[0]))
            wake, wakers = r.run_wakers(a[0], a[3])
            if wake is None:
                print('    lowest-priority request (prio %d) at +%.2f s: no '
                      'kswapd_wake before it in the trace' % (
                          a[4], r.t(a[0])))
                continue
            print('    lowest-priority request (prio %d) at +%.2f s, in the '
                  'kswapd run started at +%.2f s for order %s; woken by %d '
                  'wake-up(s)%s' % (
                      a[4], r.t(a[0]), r.t(wake[0]), wake[3], len(wakers),
                      '' if wakers else ': none recorded since its previous '
                      'run or sleep (it went on without sleeping, or its '
                      'order was raised while it was awake)'))
            for (o, g, c, fr), cnt in Counter(
                    (w[1], w[2], w[3], caller(w[4])) for w in wakers
                    ).most_common(4):
                print('      %d x order %s %s, %s, %s' % (cnt, o, g, c, fr))
        elif dr:
            for d in r.drs:
                if t0 - 1.0 <= d['ts'] <= t1 and d['arc']:
                    print('    direct reclaim at +%.2f s: order %s %s, %s, '
                          '%s' % (r.t(d['ts']), d['order'], d['gfp'],
                                  d['comm'], caller(d['frames'])))
        else:
            print('    no shrinker request: the ARC lowered c itself (reap '
                  'thread, or arc_adapt with C2)')
    if len(falls) > MAX_LIST:
        print('  ... %d more' % (len(falls) - MAX_LIST))


def print_passes(r, path):
    passes = []
    with open(path) as f:
        for line in f:
            x = line.split()
            if len(x) >= 2:
                passes.append((float(x[0]), float(x[1]),
                               float(x[2]) if len(x) > 2 else None))
    ws = [(fnum(s, 'W'), s) for _, s in r.samples if fnum(s, 'W') is not None]
    hk = ('demand_data_hits', 'prefetch_data_hits')
    ik = ('demand_data_iohits', 'prefetch_data_iohits')
    mk = ('demand_data_misses', 'prefetch_data_misses')

    def tot(s, keys):
        return sum(num(s, k) or 0 for k in keys)

    rows = []
    print('passes: %d' % len(passes))
    for i, (a, b, mbs) in enumerate(passes):
        sel = [x for x in ws if a <= x[0] <= b]
        if len(sel) < 2:
            print('  pass %d: fewer than 2 samples' % (i + 1))
            continue
        (wa, sa), (wb, sb) = sel[0], sel[-1]
        dt = wb - wa
        disk = ((num(sb, 'rd_sect') or 0) - (num(sa, 'rd_sect') or 0)) * 512
        h = tot(sb, hk) - tot(sa, hk)
        io = tot(sb, ik) - tot(sa, ik)
        m = tot(sb, mk) - tot(sa, mk)
        cs = [num(s, 'c') for _, s in sel if num(s, 'c') is not None]
        dmbs = disk / dt / 1e6 if dt > 0 else 0
        ratio = disk / (mbs * 1e6 * (b - a)) if mbs else None
        rows.append((mbs, dmbs, 100.0 * h / (h + io + m) if h + io + m
                     else 0, ratio))
        print('  pass %d: %.1f s, reader %s MB/s, disks %.0f MB/s, ARC data '
              'hits %d / iohits %d / misses %d (%.0f%% hits), disk bytes per '
              'byte read %s, mean c %d MiB' % (
                  i + 1, b - a, '%.0f' % mbs if mbs else '?', dmbs, h, io, m,
                  rows[-1][2], '%.2f' % ratio if ratio is not None else '?',
                  sum(cs) // len(cs) // MIB if cs else -1))
    if len(rows) > 1:
        def mean(v):
            v = [x for x in v if x is not None]
            return sum(v) / len(v) if v else None

        later = rows[1:]
        for name, j in (('reader MB/s', 0), ('disk MB/s', 1),
                        ('ARC data hits %', 2), ('disk bytes per byte', 3)):
            f, m = rows[0][j], mean(x[j] for x in later)
            print('  %s: first pass %s, later passes %s' % (
                name, '%.2f' % f if f is not None else '?',
                '%.2f' % m if m is not None else '?'))


def main():
    args = sys.argv[1:]
    passes = None
    if args[:1] == ['--passes']:
        if len(args) != 3:
            print('--passes FILE needs exactly one .dr file')
            return 2
        passes, args = args[1], args[2:]
    if not args:
        print(__doc__)
        return 2
    for p in args:
        r = Run(p)
        print('== %s' % p)
        print_basic(r)
        print_samples(r)
        print_calls(r)
        print_wakes(r)
        print_falls(r)
        if passes:
            print_passes(r, passes)
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
EOF
```
