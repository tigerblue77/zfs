TASK-1120-ZEDR (from the coordinator session "main"): functional, about 2 h, debug kernel module of master, three builds (the master tree in full; zed alone, user space, from master and from the fix), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB); it can run while the other VMs run other functional tasks.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-zedr`, `L=/var/log/task-1120-zedr`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. The one import this task makes is `gtest.sh`'s import of its own pool `zedr_trig`, from its own directory (`-d`). Never start a zed or `$D/zedw.sh` by hand, except the `-V` checks of steps 3 and 6 and the zeds that `gtest.sh` starts in step 6: a zed started by hand with the module loaded would act on the test pools. If a result surprises you, stop and report it with the outputs.

## Why

Two zed bugs, fixed on three local branches of the fork that are not pushed (their pull requests wait for this task):

1. **The reaper's lost stop request** (branch `zed-reaper-lost-wakeup`, commit bba82d0118e, cmd/zed/zed_exec.c). On master, the "reap ZEDLETs" thread loops in `wait_for_children()`: it tests `_reap_children_stop`, calls `wait4(WNOHANG)`, and when no child is left calls `pause()` with SIGCHLD unblocked. At exit, after logging `Exiting`, zed runs `zed_exec_fini()`, which sets the flag, sends SIGCHLD to that thread and joins it. A SIGCHLD that lands between the test of the flag and `pause()` only runs the empty handler, and `pause()` then sleeps until the next SIGCHLD, which never comes once the last ZEDLET has been reaped: the main thread waits in `pthread_join()` for good, and SIGTERM only sets `_got_exit` again. The fix keeps SIGCHLD blocked in the reaper and takes it only in `sigsuspend()`, which unblocks and sleeps in one step, and clears the flag where the thread is created.
   ZTS stops zed with `zed_stop` (libtest.shlib): `kill` on every `pgrep -x zed` process, once a second, with no limit, until none is left. A zed that never exits keeps the test in that loop until test-runner kills it at its 600 s timeout; the zed survives, so every later `zed_start` fails with `ZED already running - <pid>` and its cleanup loops again until its own timeout. The fork's CI showed exactly that twice (a debian13 and an ubuntu24 job: an events test KILLED after ten minutes of `kill <pid>`, then `ZED already running` in the following events tests). That this race caused those hangs is inferred, not proven: the CI logs hold no stack of the stuck zed.
   On main's machines, a test program built on the real zed_exec.c (`harness.c`, below) hung in `zed_exec_fini()` 55 times in 1000 runs on master (2 in 100 on another machine) and never with the fix; with `wait4()` made to sleep 500 ms when it returns ECHILD, which holds the window open, master hung 3 times in 3 and the fix 0.
2. **The task queue nobody creates** (branch `zed-slm-handle-taskq`: commit 48b4351543c, a lock around the SLM's shared libzfs handle, then 3a95cbc4c68, which creates `g_taskq`; cmd/zed/agents/zfs_mod.c). At startup the "enum-pools" thread keeps in `g_pool_list` every imported pool whose root vdev state is below DEGRADED (6); `zfs_unavail_pool()` logs `zfs_unavail_pool: examining '<pool>' (state <n>)` (master line 122). Later, each SLM event that iterates the pools with `zfs_iter_pool()` (a udev disk add, or a `vdev_check` event) logs `zfs_iter_pool: evaluating vdevs on <pool> (by ...)` (line 711) for each pool it visits, looks the pool up in `g_pool_list`, and if its root is now DEGRADED or better calls `taskq_dispatch(g_taskq, zfs_enable_ds, ...)` (line 742) to mount its datasets. Nothing creates `g_taskq` on master, so `taskq_dispatch()` locks the mutex inside a NULL `taskq_t` and zed dies with SIGSEGV in its "agents" thread. Never observed in CI; found by reading the code.

Three parts:

- **Part 1, the harness** (step 4): `harness.c` linked against each tree's own zed_exec.o, 1000 runs per arm as is (`0 0`: stop right after the ZEDLET is reaped) and 3 per arm with the window held open (`500 100`).
- **Part 2, the late pool** (step 6, `gtest.sh`): a scenario that makes master's zed reach line 742, read from the code and never run anywhere yet. Why it reaches it:
  1. `zedr_late` (one file vdev; its dataset `zedr_late/fs` is created, then unmounted) gets a `zinject -d <file> -e nxio` handler and is reopened. The injection makes `vdev_open()` of its only leaf fail (vdev.c 2282-2283): the leaf goes CANT_OPEN, and with no replica the root vdev goes CANT_OPEN (4) too. The pool stays imported, so `zpool_iter()` still lists it and its config, with that state, still comes back.
  2. A zed starts (no ZEDLET, its own state and pid files): enum-pools logs `examining 'zedr_late' (state 4)` and keeps it in `g_pool_list`.
  3. `zinject -c all` and `zpool clear`: `zedr_late` is ONLINE (7) again, `zedr_late/fs` still unmounted.
  4. `zedr_trig` (a mirror of two files, `autoreplace=on`, exported, one file moved away) is imported. With autoreplace on and a dead leaf, `spa_load()` calls `spa_check_removed()`, which posts `sysevent.fs.zfs.vdev_check` with the pool and vdev guids (spa.c 2814-2823 and 5720). `zfs_agent_post_event()` maps that class to EC_zfs/ESC_ZFS_vdev_check, and the agents thread passes it to `zfs_slm_event()`, then `zfs_deliver_check()`, which runs `zpool_iter(g_zfshdl, zfs_iter_pool, ...)`.
  5. `zpool_iter()` visits the pools in name order, so `zedr_late` comes before `zedr_trig` (whose matching vdev would stop the iteration). In `zfs_iter_pool(zedr_late)` the pool guid is not the event's, so no vdev is matched, but the `g_pool_list` block runs for every pool visited: `zedr_late` is on the list and ONLINE, so line 742 dispatches to the NULL task queue.
  No udev event is involved (the vdevs are files). Expected: master's zed (`m` rounds) logs `zfs_iter_pool: evaluating vdevs on zedr_late (by GUID)` as its last SLM line and dies with a `segfault` line in dmesg for its "agents" thread, at a small address (about 0x30, in `pthread_mutex_lock` called by `taskq_dispatch`), and `zedr_late/fs` stays unmounted; the fix's zed (`f` rounds) lives, mounts `zedr_late/fs` through `zfs_enable_ds()` (nothing else in this setup mounts it), goes on to `zedr_trig`, and exits quickly on SIGTERM. When gdb is installed, round `mg1` runs master's zed under gdb, whose backtrace should show `taskq_dispatch` called from `zfs_iter_pool` at zfs_mod.c:742. `gtest.sh` checks each step on the way (pool unavailable at enumeration, back ONLINE before the import, the `vdev_check` event posted, `zedr_late` visited) and writes a `STOP` line when one fails: a STOP there means the scenario, not the fix, is wrong.
- **Part 3, the real zed under ZTS** (steps 7 and 8): the `events` group (13 zed starts and stops per run, ZEDLETs `all-debug.sh` and `all-syslog.sh` running on every event) alternately with master's zed (`m`) and the fix's zed (`f`), on one master debug kernel module and one master ZTS tree; then one pair of the `fault` group (without its five scsi_debug and udev tests, which cannot run on these VMs) if time allows. ZTS starts `zed` from its constrained path, `$D/zt/tests/zfs-tests/bin/zed`, a link that `zfs-tests.sh` keeps when it exists; step 6 points it at `$D/zedw.sh`, which reads the run's tag from `$D/cur-tag` and execs that arm's zed with ZTS's own arguments and stderr (several events tests read `zed.log`). A watchdog, `zmon.sh` (root), notes each zed it sees with the run's tag, when `Exiting` appears in its log and when the process is gone; a zed still there 30 s after `Exiting` is a HANG: it records the threads' state, stacks and signal masks (and gdb's backtraces when gdb is installed), then kills it with SIGKILL, so that ZTS goes on instead of cascading into `ZED already running`. `ab.sh` also counts, from ZTS's own logs, the SIGTERMs `zed_stop` needed each time (one per second while zed is there). The race is narrow under ZTS, so master's zed will most likely exit in time in every run; what matters is that the fix's zed exits promptly every time and passes what master's zed passes. A HANG of master's zed, with the reaper in `pause()`, would confirm the bug in situ.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-zedr`, head `127ff4f807e9618bf8d5acc5b3838c4f596b5cf1`: a transport commit that adds the harness as `vm-kit-zedr/harness.c` (no build of the tree uses it), on the empty transport commit `c84f4f11367b3d3bf12d7e3f69bef5c0a982ccdd`, on the fix tree `178a54b70b11475c3a5f9cbd267215c3019c6603`: `4a09ce762598233f4f560ecddfd48e22e253329b` (g_taskq) on `0eb5f6748d0b3e8675c8fa04a7c19f6380e1df95` (the handle lock), both cherry-picked unchanged from `zed-slm-handle-taskq`, on `bba82d0118ef790faf20a60ae96e43bbbec65134` (the reaper), on upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` (20/11). The checksums of step 1 tie the fix's zed_exec.c to the reaper branch and its zfs_mod.c to the head of `zed-slm-handle-taskq`. Three trees:

- `$D/zt`: master, a git checkout, the normal debug build (kernel module, tools, ZTS);
- `$D/am`: master, `$D/af`: the fix, both from `git archive`, configured `--with-config=user --enable-debug --disable-shared` (user space only; static libraries, so `$D/a?/zed` is a self-contained binary) and built with `make zed` only. The harness `$D/h-<v>` is linked against that tree's objects.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs (/var/tmp/file-1 and the like, /var/tmp/spare-1 and the like, /var/tmp/vdev* in the events tests) and `zfs-tests.sh` its own loop devices on /var/tmp/file-vdev*; `gtest.sh` uses files under `$D/g-<tag>`. Touch no block device of the VM; never `zpool import` anything yourself (`-a` least of all); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other; this task creates only ZTS's pools and `zedr_late`, `zedr_trig`) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Before a loop: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (zinject prints a second line, `Run 'zinject -h' for usage information.`: only the first is compared), `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/vdev* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` shows nothing, `pgrep -a -x zed; pgrep -a -x lt-zed` shows nothing (the groups' setup starts their own zed and fail if one runs), `losetup -a` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. `ab.sh` repeats these checks (all but lsmod and zinject) before each run, also checks that the zed link still points at `$D/zedw.sh` and that the watchdog runs, and writes a `STOP` line if one fails; `gtest.sh` checks for pools and zeds before each round.
- The watchdog's SIGKILL of a zed that logged `Exiting` 30 s earlier and is still there is part of this task (part 3); kill nothing else.
- The builds, the harness runs, the rounds of step 6 and the loops run detached (`setsid nohup ... &`) and are polled. A hang is: `build-user.sh` still going after 20 minutes, `hrun.sh` after 30 minutes, the debug build after 60 minutes, the rounds of step 6 after 15 minutes; in a loop, the newest `$L/zts-<tag>.log` unchanged for 20 minutes (ZTS kills a test after 10), or the events loop still going after 100 minutes, the fault loop after 45. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zed|ksh|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/vdev* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `gcc --version | head -n 1`, and `command -v gdb || echo no-gdb` (either is fine: say which; install nothing).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-zedr
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-zedr; L=/var/log/task-1120-zedr
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D $L
mkdir $D/zt $D/am $D/af && cd $D/zt && git init -q . && git fetch -q --depth 6 https://github.com/tigerblue77/zfs vm-kit-zedr && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD HEAD~1 HEAD~2 HEAD~3 HEAD~4 HEAD~5 HEAD~1^{tree} HEAD~2^{tree} | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree.txt
echo "127ff4f807e9618bf8d5acc5b3838c4f596b5cf1 c84f4f11367b3d3bf12d7e3f69bef5c0a982ccdd 4a09ce762598233f4f560ecddfd48e22e253329b 0eb5f6748d0b3e8675c8fa04a7c19f6380e1df95 bba82d0118ef790faf20a60ae96e43bbbec65134 1b1aa0e91b694798085a2771c4eef40e93b9b2db 178a54b70b11475c3a5f9cbd267215c3019c6603 178a54b70b11475c3a5f9cbd267215c3019c6603 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
git show HEAD:vm-kit-zedr/harness.c > $D/harness.c && git archive HEAD~5 | tar -x -C $D/am && git archive HEAD~2 | tar -x -C $D/af && git checkout -q HEAD~5
echo "$(git rev-parse HEAD) $(git status --porcelain | wc -l)"
git diff --stat=80 HEAD FETCH_HEAD~2; git diff --stat=80 FETCH_HEAD~1 FETCH_HEAD
printf '%s\n' "bf407b35e523115fcf4392231cbfb207a3387377f08fa7547e4e133c2239028f  $D/harness.c" "6320522357e403a22215e0a4895b62c30dbe04bbaff62732e5bbfa8d28cdf153  $D/am/cmd/zed/zed_exec.c" "fefd5198723f5594e196b9498e69942157ea3c2182e60bf48e134378c59566ca  $D/af/cmd/zed/zed_exec.c" "9ae6cfc9d7b22cbe85f8c49535372e67410d6d6e61e513a54f0267e44911cf6b  $D/am/cmd/zed/agents/zfs_mod.c" "1e94a3886faf27fcfb43c1a88526e1f89e79969833b1eb13101f037c01a3b526  $D/af/cmd/zed/agents/zfs_mod.c" "6320522357e403a22215e0a4895b62c30dbe04bbaff62732e5bbfa8d28cdf153  $D/zt/cmd/zed/zed_exec.c" "9ae6cfc9d7b22cbe85f8c49535372e67410d6d6e61e513a54f0267e44911cf6b  $D/zt/cmd/zed/agents/zfs_mod.c" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
```

Expected: TREE-OK; `1b1aa0e91b694798085a2771c4eef40e93b9b2db 0` ($D/zt is now on master, clean); the fix's diffstat

```
 cmd/zed/agents/zfs_mod.c | 22 +++++++++++++++++++---
 cmd/zed/zed_exec.c       | 17 ++++++++++++++---
 2 files changed, 33 insertions(+), 6 deletions(-)
```

then the harness commit's

```
 vm-kit-zedr/harness.c | 100 ++++++++++++++++++++++++++++++++++++++++++++++++++
 1 file changed, 100 insertions(+)
```

and SHA-OK (the fix's zed_exec.c is the reaper branch's, its zfs_mod.c the head of `zed-slm-handle-taskq`'s). If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Edit no file of the three trees.

## STEP 2: the files

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to the path given in its heading. Then:

```sh
D=/var/tmp/task-1120-zedr; L=/var/log/task-1120-zedr
mkdir $D/hz && printf '#!/bin/sh\nexit 0\n' > $D/hz/all-true.sh && chmod 755 $D/hz/all-true.sh
printf '%s\n' "7fe00f23aed44798f0edf1447aa00693fcbc04e7adb5ef21fe33912140f16e8d  $L/hbuild.sh" "0ca17e2742643dffbf850b990fe02cd446115fe170496ede1437679a21a5f5db  $L/build-user.sh" "584d252564f2bd4d9b712ccc4034e4ca1d9dddedde52760cf2f70a229302ac1e  $L/hrun.sh" "6d05a39fc591fe28cba04116c6e0071867ce8c20c08fc51054f1ac9365fb33a8  $D/zedw.sh" "ecf281eb5095070874ecd02a428dea4ef57401b4abfe745125f8a1e43ad1457e  $L/zcap.sh" "5b072e817fb585e2aa3bb16c7831918637d9d6375f73d12f8c60476bcc00bf94  $L/zmon.sh" "f4a3fc1d2f20dafbb973c70c853ac96268e3e682157628e28c44b784974a3dae  $L/ab.sh" "e501a52ccf5f240010380868c87d5ddb340b1e0f81cb7a694ccb70d33b309a77  $L/summary.sh" "8f49b667806590bae1a871790800b92d67b2f2a4c8fed3b99f7b16531ad2dfc6  $L/zedr.run" "4353a6aae8818c7e24bfbc2261cc06dcd9ec740e3b95079616985bb37d9dc2da  $L/gtest.sh" "306c6ca7407560340797866e077e053627ad409277d1b9da58106fce4cf717cb  $D/hz/all-true.sh" | sha256sum -c && for f in $L/hbuild.sh $L/build-user.sh $L/hrun.sh $D/zedw.sh $L/zcap.sh $L/zmon.sh $L/ab.sh $L/summary.sh $L/gtest.sh; do sh -n $f || echo "SYNTAX $f"; done && chmod 755 $D/zedw.sh && echo FILES-OK
```

Expected eleven `OK` lines, no `SYNTAX` line, and FILES-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

### $L/hbuild.sh

`hbuild.sh <tree> <harness.c> <output>` links the harness against that tree's zed_exec.o, zed_log.o and zed_strings.o, with `wait4()` wrapped by the harness. It replaces the previous build script, which pointed at a worktree that no longer exists.

```sh
#!/bin/sh
# TASK-1120-ZEDR harness link: hbuild.sh <tree> <harness.c> <output>
T=$1
H=$2
O=$3
cd $T || exit 1
gcc -DHAVE_CONFIG_H -I. -include ./zfs_config.h \
    -include ./include/sys/simd_config.h -I./include -I./lib/libspl/include \
    -I./lib/libspl/include/os/linux -I./cmd/zed -D_GNU_SOURCE -D_REENTRANT \
    -D_FILE_OFFSET_BITS=64 -DDEBUG -std=gnu99 -Wall -pthread -g -O2 \
    -Wl,--wrap=wait4 -o $O $H cmd/zed/zed-zed_exec.o cmd/zed/zed-zed_log.o \
    cmd/zed/zed-zed_strings.o .libs/libavl.a .libs/libspl_assert.a
```

### $L/build-user.sh

`build-user.sh` (login user) builds, for `m` then `f`: autogen, configure and `make zed` in `$D/a<v>`, then the harness `$D/h-<v>` (log `$L/build-a<v>.log`). It writes one `a<v>-rc=` line per arm and `DONE` to `$L/build-user.txt`.

```sh
#!/bin/sh
# TASK-1120-ZEDR user-space builds of zed and of the harness, login user
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
out=$L/build-user.txt
echo "START $(date -u +%FT%TZ)" >> $out
for v in m f; do
    (
        cd $D/a$v &&
        ./autogen.sh &&
        ./configure --with-config=user --enable-debug --disable-shared &&
        nice make -j"$(nproc)" zed &&
        sh $L/hbuild.sh $D/a$v $D/harness.c $D/h-$v
    ) > $L/build-a$v.log 2>&1
    echo "a$v-rc=$? $(date -u +%T)" >> $out
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $L/hrun.sh

`hrun.sh <runs>` (login user) runs `h-m` then `h-f`, `<runs>` times with the arguments `0 0` (no delay in `wait4()`, `zed_exec_fini()` right after the ZEDLET), then 3 times with `500 100` (`wait4()` sleeps 500 ms when it returns ECHILD, `zed_exec_fini()` 100 ms after the ZEDLET started, which lands the stop request in the window). Each run gives up after 3 s and says `HUNG`; one line per run goes to `$L/harness-runs.txt`, and the totals per arm and arguments to `$L/harness.txt`.

```sh
#!/bin/sh
# TASK-1120-ZEDR harness runs, run as the login user: hrun.sh <runs>
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
out=$L/harness.txt
raw=$L/harness-runs.txt
echo "START $(date -u +%FT%TZ) runs=$1" >> $out
for args in "0 0" "500 100"; do
    n=$1
    [ "$args" = "500 100" ] && n=3
    i=1
    while [ $i -le $n ]; do
        for v in m f; do
            timeout 30 $D/h-$v $D/hz $args > $L/h.out 2>&1 < /dev/null
            rc=$?
            echo "$v|$args|$i|$rc|$(grep '^RESULT:' $L/h.out)" >> $raw
        done
        i=$((i + 1))
    done
done
awk -F'|' '{
    k = "h-" $1 " [" $2 "]"
    n[k]++
    if ($4 == 0 && $5 ~ /returned/)
        r[k]++
    else if ($4 == 1 && $5 ~ /HUNG/)
        h[k]++
    else
        o[k]++
}
END {
    for (k in n)
        printf "%s runs=%d returned=%d hung=%d other=%d\n", k, n[k], r[k], h[k], o[k]
}' $raw | sort >> $out
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $D/zedw.sh

`zedw.sh` (run by ZTS, as root, in place of zed): see Why. A missing or unknown tag makes it exit 1 without starting zed, so the run's setup fails visibly. It keeps ZTS's stderr (`zed.log`), which several events tests read.

```sh
#!/bin/sh
# TASK-1120-ZEDR: ZTS starts this as zed; it runs the zed of the arm
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
tag=$(cat $D/cur-tag)
case $tag in
m-*) v=m ;;
f-*) v=f ;;
*) echo "$(date -u +%T) pid=$$ bad tag [$tag]" >> $L/zedw.txt; exit 1 ;;
esac
echo "$(date -u +%T) pid=$$ tag=$tag args=$*" >> $L/zedw.txt
exec $D/a$v/zed "$@"
```

### $L/zcap.sh

`zcap.sh <pid>` (root) prints the state of a zed that does not exit: its threads with their wait channels, each thread's signal masks and kernel stack, the end of its log, and gdb's backtraces when gdb is installed.

```sh
#!/bin/sh
# TASK-1120-ZEDR state of a zed that does not exit, run as root: zcap.sh <pid>
p=$1
echo "== $(date -u +%FT%TZ) pid=$p"
ps -L -o pid,tid,stat,wchan:32,comm -p $p
for t in /proc/$p/task/*; do
    echo "== task ${t##*/} $(cat $t/comm)"
    grep -E '^(State|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt):' $t/status
    cat $t/stack
done
echo "== log tail"
tail -n 25 /proc/$p/fd/2
if command -v gdb > /dev/null; then
    echo "== gdb"
    timeout 60 gdb -q -batch -p $p -ex 'thread apply all bt' 2>&1 | tail -n 60
fi
```

### $L/zmon.sh

`zmon.sh` (root, detached) is the watchdog of part 3 (see Why). It stops when `$L/zmon/stop` exists. `$L/zmon.txt` gets one line per zed once it is gone: `<tag> pid= life_s= exit_ms=<from Exiting seen to gone, -1 if Exiting was never seen> verdict=<OK|SLOW (5 s or more)|HANG (killed)|UNSEEN (gone before Exiting was seen: a fast exit, or a death)>`, plus a `HANG` line when it kills one; the capture goes to `$L/zmon/<pid>.hang`.

```sh
#!/bin/sh
# TASK-1120-ZEDR zed exit watchdog, run as root: zmon.sh
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
W=$L/zmon
HANG_MS=30000
mkdir -p $W
echo "START $(date -u +%FT%TZ)" >> $L/zmon.txt
while [ ! -e $W/stop ]; do
    now=$(date +%s%3N)
    for p in $(pgrep -x zed); do
        [ "$(cut -d' ' -f3 /proc/$p/stat 2>/dev/null)" = Z ] && continue
        [ -e $W/$p.seen ] || echo "$now $(cat $D/cur-tag)" > $W/$p.seen
        if [ ! -e $W/$p.exiting ] &&
            tail -c 4096 /proc/$p/fd/2 2>/dev/null | grep -qx Exiting; then
            echo $now > $W/$p.exiting
        fi
        if [ -e $W/$p.exiting ] && [ ! -e $W/$p.killed ] &&
            [ $((now - $(cat $W/$p.exiting))) -ge $HANG_MS ]; then
            sh $L/zcap.sh $p > $W/$p.hang 2>&1
            kill -9 $p
            echo $now > $W/$p.killed
            echo "HANG $(cat $W/$p.seen) pid=$p killed $(date -u +%T)" >> $L/zmon.txt
        fi
    done
    for f in $W/*.seen; do
        [ -e "$f" ] || continue
        p=${f##*/}
        p=${p%.seen}
        [ -e $W/$p.gone ] && continue
        if [ "$(cat /proc/$p/comm 2>/dev/null)" = zed ] &&
            [ "$(cut -d' ' -f3 /proc/$p/stat 2>/dev/null)" != Z ]; then
            continue
        fi
        echo $now > $W/$p.gone
        read t0 tag < $f
        if [ -e $W/$p.killed ]; then
            ms=$(($(cat $W/$p.killed) - $(cat $W/$p.exiting)))
            v=HANG
        elif [ -e $W/$p.exiting ]; then
            ms=$((now - $(cat $W/$p.exiting)))
            v=OK
            [ $ms -ge 5000 ] && v=SLOW
        else
            ms=-1
            v=UNSEEN
        fi
        echo "$tag pid=$p life_s=$(((now - t0) / 1000)) exit_ms=$ms verdict=$v" >> $L/zmon.txt
    done
    sleep 0.2
done
echo "DONE $(date -u +%FT%TZ)" >> $L/zmon.txt
```

### $L/ab.sh

`ab.sh <group> <pairs> <minutes>` (login user) runs, for n from 1 to `<pairs>`, `m-<group><n>` then `f-<group><n>` (group `e`: the events tests, `x`: the fault tests), and starts no new pair once `<minutes>` have passed (a `TIME` line). Each run: the pre-run checks, the tag in `$D/cur-tag`, and `zfs-tests.sh -vKO -r $L/zedr.run -T zedr<group>` (console in `$L/zts-<tag>.log`). `$L/ab.txt` gets one line per run: `<tag> rc= secs= wrapper=<wrapper starts> zeds= ok= slow= hang= unseen= max_exit_ms= <the watchdog's counts for the tag> stops=<NOTE: Stopping ZED> kills=<SIGTERMs zed_stop sent> maxkills=<most SIGTERMs one stop needed> already=<ZED already running> segv=<segfault lines in dmesg> | <test>:<result>:<duration> ... | logdir=<ZTS results directory>`.

```sh
#!/bin/sh
# TASK-1120-ZEDR A/B loop, run as the login user: ab.sh <group> <pairs> <minutes>
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
Z=$D/zt/tests/zfs-tests/bin/zed
g=$1
pairs=$2
limit=$(($3 * 60))
out=$L/ab.txt
cd $D/zt || exit 1
t0=$(date +%s)
echo "START $g $(date -u +%FT%TZ)" >> $out
n=1
while [ $n -le $pairs ]; do
    if [ $(($(date +%s) - t0)) -ge $limit ]; then
        echo "TIME: $g pair $n not started, $3 minutes passed" >> $out
        break
    fi
    for v in m f; do
        tag=$v-$g$n
        pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
        left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/vdev* \
            /var/tmp/zed /etc/zfs/vdev_id.conf 2>/dev/null)
        zeds=$(pgrep -x zed; pgrep -x lt-zed)
        loops=$(losetup -a)
        link=$(readlink $Z)
        mon=$(pgrep -f "sh $L/zmon.sh")
        if [ -n "$pools" ] || [ -n "$left" ] || [ -n "$zeds" ] ||
            [ -n "$loops" ] || [ -s /etc/zfs/zpool.cache ] ||
            [ "$link" != "$D/zedw.sh" ] || [ -z "$mon" ]; then
            echo "STOP before $tag: pools=[$pools] left=[$left] zed=[$zeds] loops=[$loops] link=[$link] mon=[$mon] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
            exit 1
        fi
        echo $tag > $D/cur-tag
        k0=$(sudo -n dmesg | wc -l)
        s=$(date +%s)
        sudo -n -u zts ./scripts/zfs-tests.sh -vKO -r $L/zedr.run -T zedr$g \
            > $L/zts-$tag.log 2>&1 < /dev/null
        rc=$?
        e=$(date +%s)
        sleep 2
        segv=$(sudo -n dmesg | tail -n +$((k0 + 1)) |
            grep -c -E 'segfault|traps:')
        starts=$(grep -c " tag=$tag " $L/zedw.txt)
        zm=$(grep "^$tag " $L/zmon.txt | awk '{
            n++
            split($4, a, "=")
            split($5, b, "=")
            if (a[2] + 0 > mx)
                mx = a[2] + 0
            c[b[2]]++
        }
        END {
            printf "zeds=%d ok=%d slow=%d hang=%d unseen=%d max_exit_ms=%d", n, c["OK"], c["SLOW"], c["HANG"], c["UNSEEN"], mx
        }')
        R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag.log)
        st=$(sudo -n find "$R" -type f -name merged -exec awk '
            FNR == 1 { on = 0 }
            /NOTE: Stopping ZED/ { s++; on = 1; c = 0; next }
            on && /SUCCESS: kill / { k++; if (++c > m) m = c; next }
            { on = 0 }
            /ZED already running/ { a++ }
            END { printf "stops=%d kills=%d maxkills=%d already=%d", s, k, m, a }' {} +)
        res=$(grep -E 'Test.*: .*/functional/(events|fault)/[^ ]+ \(run as ' \
            $L/zts-$tag.log |
            sed -E 's|.*/functional/[a-z]+/([^ ]+) \(run as [^)]*\) +\[([0-9:]+)\] \[([A-Z]+)\].*|\1:\3:\2|' |
            tr '\n' ' ')
        echo "$tag rc=$rc secs=$((e - s)) wrapper=$starts $zm $st segv=$segv | $res| logdir=$R" >> $out
    done
    n=$((n + 1))
done
echo "DONE $g $(date -u +%FT%TZ)" >> $out
```

### $L/summary.sh

`summary.sh` (login user) prints, from `$L/ab.txt`, the totals of each arm and group (`m-e`, `f-e`, `m-x`, `f-x`) and each test's PASS count.

```sh
#!/bin/sh
# TASK-1120-ZEDR per-arm totals of ab.txt: summary.sh
L=/var/log/task-1120-zedr
grep -E '^[mf]-[ex][0-9]+ ' $L/ab.txt | awk '
{
    v = substr($1, 1, 3)
    runs[v]++
    for (i = 2; i <= NF; i++) {
        if (split($i, a, "=") == 2) {
            if (a[1] ~ /^(zeds|ok|slow|hang|unseen|stops|kills|already|segv)$/)
                s[v, a[1]] += a[2]
            if (a[1] ~ /^(max_exit_ms|maxkills)$/ && a[2] + 0 > x[v, a[1]])
                x[v, a[1]] = a[2] + 0
        }
        if (split($i, a, ":") == 4 && a[2] ~ /^[A-Z]+$/) {
            tot[v, a[1]]++
            if (a[2] == "PASS")
                ok[v, a[1]]++
        }
    }
}
END {
    for (v in runs)
        printf "%s totals: runs=%d zeds=%d ok=%d slow=%d hang=%d unseen=%d max_exit_ms=%d stops=%d kills=%d maxkills=%d already=%d segv=%d\n", v, runs[v], s[v, "zeds"], s[v, "ok"], s[v, "slow"], s[v, "hang"], s[v, "unseen"], x[v, "max_exit_ms"], s[v, "stops"], s[v, "kills"], x[v, "maxkills"], s[v, "already"], s[v, "segv"]
    for (k in tot) {
        split(k, b, SUBSEP)
        printf "%s %s %d/%d PASS\n", b[1], b[2], ok[k] + 0, tot[k]
    }
}' | sort
```

### $L/zedr.run

`zedr.run`: the runfile of the loops. Its `[DEFAULT]` is linux.run's; the events section is linux.run's under the tag `zedre`; the fault section is linux.run's without `auto_offline_001_pos`, `auto_online_001_pos`, `auto_online_002_pos`, `auto_replace_001_pos` and `auto_replace_002_pos` (scsi_debug and udev rules, which these VMs lack), under the tag `zedrx`.

```ini
[DEFAULT]
pre = setup
quiet = False
pre_user = root
user = root
timeout = 600
post_user = root
post = cleanup
failsafe_user = root
failsafe = callbacks/zfs_failsafe
tags = ['functional']

[tests/functional/events]
tests = ['events_001_pos', 'events_002_pos', 'zed_rc_filter', 'zed_fd_spill',
    'zed_cksum_reported', 'zed_cksum_config', 'zed_io_config',
    'zed_slow_io', 'zed_slow_io_many_vdevs', 'zed_diagnose_multiple',
    'zed_synchronous_zedlet', 'slow_vdev_sit_out', 'slow_vdev_sit_out_neg',
    'slow_vdev_degraded_sit_out', 'vdev_add_event']
tags = ['zedre']

[tests/functional/fault]
tests = ['auto_spare_001_pos', 'auto_spare_002_pos', 'auto_spare_double',
    'auto_spare_multiple', 'auto_spare_ashift', 'auto_spare_rotational',
    'auto_spare_shared', 'decrypt_fault', 'decompress_fault', 'fault_limits',
    'scrub_after_resilver', 'suspend_on_probe_errors', 'suspend_resume_single',
    'suspend_draid_fgroups', 'suspend_txg_sync_idle', 'suspend_zil_keep_first',
    'zpool_status_-s']
tags = ['zedrx']
```

### $L/gtest.sh

`gtest.sh <tag>` (login user) is part 2 (see Why): `m<n>` runs master's zed, `f<n>` the fix's, `mg<n>` master's under gdb. It writes `$L/g-<tag>.txt` (with one `RESULT` line), the zed's log to `$L/g-<tag>.zedlog`, and gdb's output to `$L/g-<tag>.gdb`. On any failed check it writes `STOP: <why>`, stops its zed, clears the injection and destroys its two pools. Its zed has no ZEDLET (`-d` on an empty directory) and its own pid and state files.

```sh
#!/bin/sh
# TASK-1120-ZEDR late pool scenario, run as the login user: gtest.sh <tag>
D=/var/tmp/task-1120-zedr
L=/var/log/task-1120-zedr
Z=$D/zt
tag=$1
case $tag in
mg[0-9]*) v=m; gdb=1 ;;
m[0-9]*) v=m; gdb=0 ;;
f[0-9]*) v=f; gdb=0 ;;
*) echo "usage: gtest.sh m<n>|f<n>|mg<n>"; exit 2 ;;
esac
G=$D/g-$tag
out=$L/g-$tag.txt
zlog=$L/g-$tag.zedlog
zpid=
made=0
say() {
    echo "$(date -u +%T) $*" >> $out
}
zp() {
    timeout 120 sudo -n $Z/zpool "$@"
}
alive() {
    [ -n "$zpid" ] && [ "$(cat /proc/$zpid/comm 2>/dev/null)" = zed ] &&
        [ "$(cut -d' ' -f3 /proc/$zpid/stat 2>/dev/null)" != Z ]
}
mnt() {
    timeout 60 sudo -n $Z/zfs get -H -o value mounted zedr_late/fs 2>&1
}
stopzed() {
    t1=$(date +%s%3N)
    sudo -n kill $zpid
    i=0
    while alive && [ $i -lt 150 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if alive; then
        sudo -n sh $L/zcap.sh $zpid > $L/g-$tag.hang 2>&1
        sudo -n kill -9 $zpid
        say "zed still there 15 s after SIGTERM: captured, killed"
    else
        say "zed exited $(($(date +%s%3N) - t1)) ms after SIGTERM"
    fi
}
cleanup() {
    alive && stopzed
    [ $made = 1 ] || return
    sudo -n $Z/zinject -c all > /dev/null 2>&1
    for p in zedr_trig zedr_late; do
        if zp list -H -o name $p > /dev/null 2>&1; then
            zp clear $p > /dev/null 2>&1
            zp destroy -f $p >> $out 2>&1
            say "destroy $p rc=$?"
        fi
    done
    if grep -q " $G/" /proc/mounts; then
        say "$G left in place: still mounted"
    else
        sudo -n rm -rf $G
    fi
}
stop() {
    say "STOP: $*"
    cleanup
    exit 1
}
say "START $tag arm=$v gdb=$gdb"
[ -z "$(pgrep -x zed; pgrep -x lt-zed)" ] || stop "a zed is running"
[ -z "$(zp list -H -o name 2>&1)" ] || stop "a pool exists"
mkdir $G || stop "mkdir $G"
made=1
mkdir $G/va $G/vb $G/away $G/zedlets $G/mnt
truncate -s 256M $G/va/late.img $G/vb/t1.img $G/vb/t2.img
zp create -o cachefile=none -m none zedr_late $G/va/late.img ||
    stop "create zedr_late"
timeout 60 sudo -n $Z/zfs create -o mountpoint=$G/mnt/late zedr_late/fs ||
    stop "create zedr_late/fs"
timeout 60 sudo -n $Z/zfs unmount zedr_late/fs || stop "unmount zedr_late/fs"
[ "$(mnt)" = no ] || stop "zedr_late/fs is still mounted"
zp create -o cachefile=none -o autoreplace=on -m none zedr_trig \
    mirror $G/vb/t1.img $G/vb/t2.img || stop "create zedr_trig"
zp export zedr_trig || stop "export zedr_trig"
mv $G/vb/t2.img $G/away/ || stop "mv t2.img"
zp sync zedr_late
sudo -n $Z/zinject -d $G/va/late.img -e nxio zedr_late >> $out 2>&1 ||
    stop "zinject"
zp reopen zedr_late >> $out 2>&1
say "reopen rc=$?"
sleep 10
h=$(zp list -H -o health zedr_late)
say "zedr_late before zed: health=$h kstat=$(cat /proc/spl/kstat/zfs/zedr_late/state)"
zp status zedr_late >> $out 2>&1
case $h in
ONLINE|DEGRADED) stop "zedr_late is still available" ;;
esac
zp events -c > /dev/null
k0=$(sudo -n dmesg | wc -l)
a="-vF -d $G/zedlets -p $G/zed.pid -s $G/zed.state -j 1"
if [ $gdb = 1 ]; then
    sudo -n sh -c "cd $G && setsid nohup gdb -q -batch -ex 'run $a 2> $zlog' -ex bt -ex 'info threads' $D/am/zed > $L/g-$tag.gdb 2>&1 < /dev/null &"
else
    sudo -n sh -c "cd $G && setsid nohup $D/a$v/zed $a > /dev/null 2> $zlog < /dev/null &"
fi
i=0
until grep -q "zfs_unavail_pool: examining 'zedr_late'" $zlog 2>/dev/null; do
    i=$((i + 1))
    [ $i -le 60 ] || stop "no enumeration line in 60 s"
    sleep 1
done
sleep 2
zpid=$(cat $G/zed.pid)
say "zed pid=$zpid: $(grep "zfs_unavail_pool: examining 'zedr_late'" $zlog)"
alive || stop "zed is not running"
sudo -n $Z/zinject -c all >> $out 2>&1
zp clear zedr_late >> $out 2>&1
say "clear rc=$?"
sleep 3
h=$(zp list -H -o health zedr_late)
m=$(mnt)
say "zedr_late after clear: health=$h mounted=$m"
[ "$h" = ONLINE ] || [ "$h" = DEGRADED ] || stop "zedr_late is not back"
[ "$m" = no ] || stop "zedr_late/fs is mounted before the import"
alive || stop "zed died before the import"
l0=$(wc -l < $zlog)
zp import -d $G/vb -o cachefile=none zedr_trig >> $out 2>&1
say "import rc=$? zedr_trig=$(zp list -H -o health zedr_trig)"
i=0
while [ $i -lt 30 ] && alive && [ "$(mnt)" != yes ]; do
    sleep 1
    i=$((i + 1))
done
sleep 3
if alive; then
    za=alive
else
    za=gone
fi
m=$(mnt)
seg=$(sudo -n dmesg | tail -n +$((k0 + 1)) | grep -E 'segfault|traps:' |
    tail -n 2 | tr '\n' ' ')
vc=$(zp events -H | grep -c 'sysevent\.fs\.zfs\.vdev_check')
il=$(tail -n +$((l0 + 1)) $zlog | grep -c 'evaluating vdevs on zedr_late ')
last=$(tail -n +$((l0 + 1)) $zlog |
    grep -E 'zfs_slm_event|zfs_deliver_check|zfs_iter_pool|zfs_process_add' |
    tail -n 1)
say "RESULT $tag arm=$v zed=$za mounted=$m vdev_check=$vc iter_late=$il segfault=[$seg] last=[$last]"
tail -n +$((l0 + 1)) $zlog |
    grep -E 'zfs_slm_event|zfs_deliver_check|zfs_iter_pool|zfs_process_add' >> $out
cleanup
say "DONE"
```

## STEP 3: the user-space builds (about 6 minutes)

```sh
L=/var/log/task-1120-zedr
setsid nohup sh $L/build-user.sh > $L/build-user.nohup 2>&1 < /dev/null &
```

Poll `cat $L/build-user.txt` until `DONE`. Expected `am-rc=0` and `af-rc=0`, an empty `build-user.nohup`; quote every `warning:` line of `grep -h 'warning:' $L/build-am.log $L/build-af.log` (none expected: the tree builds with `-Werror`; the harness is built with `-Wall` only, so a warning there is possible: quote it). A non-zero rc: quote the last 40 lines of that arm's log, stop and report. Then:

```sh
D=/var/tmp/task-1120-zedr
for v in m f; do echo "a$v: $($D/a$v/zed -V) zed_sigsuspend=$(nm $D/a$v/zed | grep -c ' U sigsuspend') zed_pause=$(nm $D/a$v/zed | grep -c ' U pause') h_sigsuspend=$(nm $D/h-$v | grep -c ' U sigsuspend') h_pause=$(nm $D/h-$v | grep -c ' U pause')"; done
ls -l $D/am/zed $D/af/zed $D/h-m $D/h-f
```

Expected `am: zfs-2.4.99-1 zed_sigsuspend=0 zed_pause=1 h_sigsuspend=0 h_pause=1` and `af: zfs-2.4.99-1 zed_sigsuspend=1 zed_pause=0 h_sigsuspend=1 h_pause=0` (only the fix's reaper sleeps in `sigsuspend()`), and the four files.

## STEP 4: part 1, the harness (about 5 minutes)

```sh
L=/var/log/task-1120-zedr
setsid nohup sh $L/hrun.sh 1000 > $L/hrun.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/harness.txt` until `DONE` (a run takes about 0.1 s, a hung one 3 s). Then quote:

```sh
L=/var/log/task-1120-zedr
cat $L/harness.txt $L/hrun.nohup; wc -l $L/harness-runs.txt
grep -v -E '\|(0\|RESULT: zed_exec_fini returned|1\|RESULT: zed_exec_fini HUNG \(> 3 s\))$' $L/harness-runs.txt | head -n 20
```

Expected: `harness-runs.txt` with 2006 lines, the last command printing nothing, an empty `hrun.nohup`, and

- `h-f [0 0] runs=1000 returned=1000 hung=0 other=0` and `h-f [500 100] runs=3 returned=3 hung=0 other=0`;
- `h-m [500 100] runs=3 returned=0 hung=3 other=0`;
- `h-m [0 0] runs=1000` with some `hung` (a few percent on main's machines; the rate depends on the machine, and 0 is possible on a fast VM: then the `500 100` line carries the result; say so).

An `h-f` run that hung or ended otherwise contradicts the fix, and matters most: quote `grep '^f|' $L/harness-runs.txt | grep -v -E '\|0\|RESULT: zed_exec_fini returned$'`, and go on with the plan.

## STEP 5: debug build of master (about 10 minutes)

```sh
D=/var/tmp/task-1120-zedr; L=/var/log/task-1120-zedr
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j\$(nproc); echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 6: load, links, and part 2, the late pool (about 8 minutes)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Then point ZTS's zed at the wrapper and check the chain, which `-V` exercises without starting the daemon:

```sh
D=/var/tmp/task-1120-zedr; L=/var/log/task-1120-zedr
mkdir -p $D/zt/tests/zfs-tests/bin && ln -sfn $D/zedw.sh $D/zt/tests/zfs-tests/bin/zed && ls -l $D/zt/tests/zfs-tests/bin/zed $D/zedw.sh
for t in m-0 f-0; do echo $t > $D/cur-tag && $D/zt/tests/zfs-tests/bin/zed -V; done
cat $L/zedw.txt
```

Expected: the link `-> /var/tmp/task-1120-zedr/zedw.sh`, `zedw.sh` with mode `-rwxr-xr-x`, `zfs-2.4.99-1` twice, and two `zedw.txt` lines (`tag=m-0 args=-V`, `tag=f-0 args=-V`).

Run the pre-run checks of the VM rules, then part 2: four rounds, master's zed first, then a fifth under gdb if step 0 found gdb.

```sh
L=/var/log/task-1120-zedr
setsid nohup sh -c "for t in m1 f1 m2 f2; do sh $L/gtest.sh \$t; done; command -v gdb > /dev/null && sh $L/gtest.sh mg1; echo GDONE" > $L/gtest.nohup 2>&1 < /dev/null &
```

Poll `tail -n 1 $L/gtest.nohup` until `GDONE` (about a minute per round). Then:

```sh
L=/var/log/task-1120-zedr
cat $L/gtest.nohup; grep -h -E 'START|STOP|reopen rc|before zed|zed pid=|clear rc|after clear|import rc|RESULT|exited|killed|destroy|DONE' $L/g-*.txt
cat $L/g-m1.txt $L/g-f1.txt
for t in m1 f1; do echo "== $t"; grep -n -E 'zfs_unavail_pool|zfs_slm_event|zfs_deliver_check|zfs_iter_pool|zfs_process_add|Exiting' $L/g-$t.zedlog | head -n 30; done
test -e $L/g-mg1.gdb && grep -n -A 25 'received signal' $L/g-mg1.gdb
```

Expected, every round: `reopen rc=0`; `before zed: health=UNAVAIL` (or `SUSPENDED`; `kstat=` gives the pool's state); `zed pid=<n>: zfs_unavail_pool: examining 'zedr_late' (state 4)` (any state below 6 is fine); `clear rc=0`, `after clear: health=ONLINE mounted=no`; `import rc=0 zedr_trig=DEGRADED`; then the `RESULT` line:

- `m1`, `m2` (master): `zed=gone mounted=no vdev_check=<1 or more> iter_late=1 segfault=[... agents[<pid>]: segfault at <small address> ...] last=[zfs_iter_pool: evaluating vdevs on zedr_late (by GUID)]`;
- `f1`, `f2` (fix): `zed=alive mounted=yes vdev_check=<1 or more> iter_late=1 segfault=[]`, with `last` a line about `zedr_trig` (`zfs_iter_pool` on it, or `zfs_process_add` of its missing vdev), and `zed exited <ms> ms after SIGTERM` (under 1000 expected);
- `mg1`, when gdb is there: `zed=gone mounted=no` (gdb kills its zed once done), and in `g-mg1.gdb` the signal SIGSEGV in thread "agents", a backtrace with `taskq_dispatch` (tq=0x0) called from `zfs_iter_pool` at `zfs_mod.c:742`, itself under `zpool_iter` and `zfs_deliver_check`;
- every round ends with `destroy zedr_trig rc=0`, `destroy zedr_late rc=0` and `DONE`.

The prediction to check: master's zed dies at line 742 once a pool that was unavailable at its start is back and an SLM event visits it, and the fix's zed instead mounts that pool's dataset and lives on. What contradicts it, and matters most: an `f` round with `zed=gone` or `mounted=no` (quote `tail -n 40 $L/g-<tag>.zedlog` and the dmesg lines after N0). A `STOP:` line in a round means the scenario did not unfold as read from the code (say at which check), not that a zed is wrong: quote that round's `g-<tag>.txt` and `.zedlog` in full, finish the other rounds as planned, and go on to step 7. An `m` round with `zed=alive` (no crash) also means the scenario missed: quote the same.

After the rounds: the pre-run checks of the VM rules once more (no pool, no zed, `No handlers registered.`), and `ls -d $D/g-* 2>&1` (none).

## STEP 7: part 3, the events loop (60 to 80 minutes)

Note N1=`sudo -n dmesg | wc -l`. Start the watchdog, check that it runs, then the loop:

```sh
L=/var/log/task-1120-zedr
sudo -n setsid nohup sh $L/zmon.sh > $L/zmon.nohup 2>&1 < /dev/null &
sleep 2; pgrep -a -f "sh $L/zmon.sh"; cat $L/zmon.txt
setsid nohup sh $L/ab.sh e 3 45 > $L/ab-e.nohup 2>&1 < /dev/null &
```

Expected: a `zmon.sh` process and a `START` line. An events run takes about 13 to 16 minutes (the slowest tests: zed_slow_io, slow_vdev_sit_out, slow_vdev_degraded_sit_out); the loop runs 2 or 3 pairs. Poll `tail -n 2 $L/ab.txt`, `ls -l --time-style=+%T $L/zts-*.log | tail -n 1` and `grep -c HANG $L/zmon.txt` until a `DONE e` or a `STOP` line. A HANG needs nothing from you: the watchdog has captured and killed that zed, and ZTS goes on.

## STEP 8: the fault pair, if time allows (about 25 minutes)

Only if the events loop wrote `DONE e` (no STOP) less than 75 minutes after its `START e` line, run, after the pre-run checks:

```sh
L=/var/log/task-1120-zedr
setsid nohup sh $L/ab.sh x 1 1 > $L/ab-x.nohup 2>&1 < /dev/null &
```

and poll as in step 7 until `DONE x` or a `STOP` line. Otherwise skip it and say why. Then stop the watchdog:

```sh
L=/var/log/task-1120-zedr
sudo -n touch $L/zmon/stop; sleep 2; tail -n 1 $L/zmon.txt
```

(a `DONE` line expected), and quote:

```sh
L=/var/log/task-1120-zedr
grep -E '^(START|DONE|STOP|TIME)' $L/ab.txt; cat $L/ab-*.nohup $L/zmon.nohup
grep -E '^[mf]-' $L/ab.txt
sh $L/summary.sh
grep -E '^(START|DONE|HANG)' $L/zmon.txt; grep -E 'verdict=(SLOW|HANG)' $L/zmon.txt; grep -c 'verdict=UNSEEN' $L/zmon.txt
wc -l < $L/zedw.txt; grep -c 'bad tag' $L/zedw.txt
for f in $L/zmon/*.hang; do test -e $f && { echo "== $f"; cat $f; }; done
```

Expected:

- START, DONE, no STOP, empty `.nohup` files; 4 or 6 events run lines (a `TIME` line before `DONE e` when the loop stopped at 2 pairs), and 2 fault run lines if step 8 ran.
- Every line with `wrapper` equal to `zeds` (each zed ZTS started went through the wrapper and was seen by the watchdog), `already=0`, `segv=0`, and `stops` at or above `zeds` (the events cleanup calls `zed_stop` once more when zed is already gone).
- `f`: `hang=0 slow=0`, `max_exit_ms` under 5000 and `maxkills` 1 or 2 in every run: each zed of the fix exited within a second or two of ZTS's first SIGTERM.
- `m`: most likely the same (see Why). A `hang` above 0 (or `maxkills` of 30 or more) is the bug in situ: quote its `.hang` file in full. Expected there: the "reap ZEDLETs" thread sleeping in `pause()` (`do_sys_pause` or `__do_sys_pause` in its wait channel or stack), its `SigBlk` with every standard signal blocked but SIGCHLD (bit 0x10000 clear), the main "zed" thread in a futex wait (`pthread_join`), and with gdb, `pause` under `wait_for_children` in the reaper and `pthread_join` under `zed_exec_fini` in the main thread.
- `unseen` counts zeds that were gone before the watchdog saw their `Exiting` (normal for a fast exit); with `segv=0`, none of them died.
- The same PASS set in both arms. A test that fails in both arms says nothing about zed: quote its first `ERROR:` line only. A test that PASSes with `m` and fails with `f`, or the opposite, matters: quote its first `ERROR:` line with the 25 lines before it, from the run's `logdir`:

```sh
R=<logdir of the run>; t=<test name>
f=$(sudo -n find "$R" -type f -path "*/$t/*" -name merged | head -n 1); echo "$f"; sudo -n grep -n -m 1 -B 25 'ERROR: ' "$f"
```

The prediction to check: the fix's zed never hangs nor exits slowly, never needs more than a SIGTERM or two, and passes what master's zed passes. What contradicts it, and matters most: a HANG or SLOW in an `f` run, `maxkills` above 2 in an `f` run, `segv` above 0 in an `f` run, or a test that passes with `m` and fails with `f`. For an `f` HANG, quote its `.hang` file in full.

After the loops: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N1+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault|traps:` quoted with 10 lines before it (none expected; the segfaults of step 6's `m` rounds come before N1). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`.

## STEP 9: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `pgrep -a -f zmon.sh` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/vdev* /var/tmp/zed 2>&1` (none), `losetup -a` (empty), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L (the wrapper went with $D; its text is in this task), the ZTS results under /var/tmp/test_results.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, gcc version, gdb or not, the zed line of step 0; TREE-OK, SHA-OK, the two diffstats; FILES-OK; the user builds' rc and warnings and the `a<v>:` lines; the debug build's rc and warnings; S1 and DEBUG mode; the link and `-V` checks; what happened to /etc/zfs/zpool.cache at steps 0 and 9.
2. Part 1, the four lines of `harness.txt` as a table: binary | arguments | runs | returned | hung | other. Then one verdict line, e.g. "master zed_exec.c: H/1000 hung as is, 3/3 with the window held open; fix bba82d0118e: 0/1000 and 0/3".
3. Part 2, a table: round | health before zed | enumerated state | health after clear | import rc | vdev_check | iter_late | zed after | zedr_late/fs mounted | segfault | last SLM line | exit after SIGTERM. Then the gdb backtrace (first 12 frames) if `mg1` ran, and one verdict line, e.g. "master: 2/2 zed killed by SIGSEGV at the dispatch after visiting zedr_late; fix 4a09ce76259: 2/2 alive, dataset mounted by zfs_enable_ds".
4. Part 3, a table: tag | rc | duration | wrapper/zeds | ok/slow/hang/unseen | max_exit_ms | stops/kills/maxkills | already | segv | tests not PASS; then the `summary.sh` output and one verdict line, e.g. "fix zed: N zeds in K runs, 0 hang, 0 slow, max exit X ms, at most one SIGTERM each, same PASS set as master's zed; master zed: H hangs".
5. Quotes: START/DONE/TIME lines, the HANG captures, the failing tests' lines, the dmesg check.
6. Deviations, cleanup, then End.
