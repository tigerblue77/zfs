TASK-1120-ZEDT (from the coordinator session "main"): functional, about 1 h 30, debug kernel module of master, three builds (the master tree in full; zed alone with AddressSanitizer from master and from the fix), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-zedt`, `L=/var/log/task-1120-zedt`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. Never start a zed, the ASAN zed or `$D/zedw.sh` by hand, except the `-V` checks of steps 3 and 6: a zed started by hand with the module loaded would act on the test pools. If a result surprises you, stop and report it with the outputs.

## Why

ZED died once in the fork's CI (AlmaLinux 8, during `fault/auto_spare_001_pos`, the only such crash among 555 failed CI logs read): `agents[...]: segfault at eb8 ... error 4 in zed`. The cause is a use-after-free in cmd/zed/agents/fmd_api.c. `fmd_timer_install()` arms a `SIGEV_THREAD` timer whose `sigev_value` is the `fmd_timer_t` itself, and `_timer_notify()` (lines 545-547 on master) reads `ftp->ft_hdl` and the module behind it on a thread created for each expiration. `fmd_timer_remove()` (lines 599-606) frees the `fmd_timer_t` right after `timer_delete()`, which neither waits for nor cancels a notification thread already started. When the agents thread removes a timer (zfs_diagnosis closing or re-arming a case) just as it expires, `_timer_notify()` reads freed memory.

The fix, c40989fdd2f "zed: run FMD timer callbacks on the agents thread", passes only a never-reused id through `sigev_value`. `_timer_notify()` posts that id to the agents queue, and `fmd_module_timeout()`, on the agents thread, calls the module's `fmdo_timeout` only if a timer with that id is still installed; the timers become one-shot. It changes cmd/zed/agents/fmd_api.c, fmd_api.h and zfs_agents.c only: the kernel module and every other program are master's.

Two parts:

1. **Harness, the direct proof.** A small program installs a 100 us timer and removes it 50 to 150 us later, 50 000 times, as zfs_diagnosis does when it closes a case while its timer fires. It is linked with AddressSanitizer against the real fmd_api.o and fmd_serd.o of each tree: `h-m` (master) and `h-f` (the fix, where expirations come back through the queue and are dispatched as zfs_agents.c does). On main's machine (4 CPUs, gcc 13), 3 runs of each: every `h-m` run stopped within its first second on `heap-use-after-free` in `_timer_notify` fmd_api.c:545, the memory freed by `fmd_timer_remove`; every `h-f` run was clean, with about 21 000 timeouts delivered and 42 000 expirations posted (the difference: expirations of timers removed meanwhile, dropped on the agents thread).
2. **The real zed under ZTS, in situ.** Four fault tests (`auto_spare_001_pos`, `auto_spare_002_pos`, `auto_spare_multiple`, `auto_spare_shared`; the first, third and fourth inject I/O errors, which zfs_diagnosis diagnoses through a 15-second timer) run with zed built with AddressSanitizer, alternating master's zed (`m`) and the fix's zed (`f`), on one master debug kernel module and one master ZTS tree. ZTS starts `zed` from its constrained path, `$D/zt/tests/zfs-tests/bin/zed`, a link that `zfs-tests.sh` keeps when it exists. Step 6 points it at `$D/zedw.sh`, which reads the run's tag from `$D/cur-tag` and runs the ASAN zed of that arm with `ASAN_OPTIONS=detect_leaks=0:log_path=$L/asan/<tag>` (one report file per zed process that reports, `$L/asan/<tag>.<pid>`) and with zed's stderr appended to `$L/zedlog-<tag>.txt`; ZTS's own `/var/tmp/zed/zed.log` stays empty (no test of these four reads it, and the group's cleanup deletes it). The race is rare, so `m` will most likely stay clean in a few runs. This part checks that the fix's zed survives every run, reports no memory error, still diagnoses through the timeout (`timer fired`, `solving fault 'fault.fs.zfs.vdev.io'` in its log) and passes what master's zed passes. An ASAN error in `m` would confirm the bug in situ.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-zedt`, head `1a4029863f6e3146b4fe4f41735bc947d9696005`: a transport commit that adds the harness as `vm-kit-zedt/harness.c` (no build of the tree uses it), on the transport commit `4e1736118b6fc2753ca4b12cc7d15654562c52be` (the fix's tree `a97fcdc2ee778586e6302d490f4e1e67d232417c`), on the fix `c40989fdd2f9707898218b37acc4b10a3001f1a2`, on upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` (20/11). Three trees:

- `$D/zt`: master, a git checkout, the normal debug build (kernel module, tools, ZTS);
- `$D/am`: master, `$D/af`: the fix, both from `git archive`, configured `--with-config=user --enable-debug --enable-asan --disable-shared` (the tree's own AddressSanitizer switch, user space only; static libraries, so `$D/a?/zed` is a self-contained binary) and built with `make zed` only.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs (/var/tmp/file-1 and the like, /var/tmp/spare-1 and the like) and `zfs-tests.sh` its own loop devices on /var/tmp/file-vdev*; touch no block device of the VM; never `zpool import` anything (`-a` least of all); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Before the loop: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (zinject prints a second line, `Run 'zinject -h' for usage information.`: only the first is compared), `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` shows nothing, `pgrep -a -x zed; pgrep -a -x lt-zed` shows nothing (the group's setup starts its own zed and fails if one runs), `losetup -a` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. `ab.sh` repeats these checks (all but lsmod and zinject) before each run, checks that the zed link still points at `$D/zedw.sh`, and writes a `STOP` line if one fails.
- The builds, the harness runs and the loop run detached (`setsid nohup ... &`) and are polled. A hang is: `build-asan.sh` still going after 20 minutes, `hrun.sh` after 15 minutes, the debug build after 60 minutes; in the loop, the newest `$L/zts-<tag>.log` unchanged for 20 minutes (ZTS kills a test after 10), or the loop still going after 70 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zed|ksh|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `gcc --version | head -n 1`, and `gcc -print-file-name=libasan.so` (a full path is expected: AddressSanitizer's runtime is installed; the bare name `libasan.so` means it is not: stop and report).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-zedt
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-zedt; L=/var/log/task-1120-zedt
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D $L
mkdir $D/zt $D/am $D/af $L/asan && cd $D/zt && git init -q . && git fetch -q --depth 4 https://github.com/tigerblue77/zfs vm-kit-zedt && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD HEAD~1 HEAD~2 HEAD~3 HEAD~1^{tree} HEAD~2^{tree} | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree.txt
echo "1a4029863f6e3146b4fe4f41735bc947d9696005 4e1736118b6fc2753ca4b12cc7d15654562c52be c40989fdd2f9707898218b37acc4b10a3001f1a2 1b1aa0e91b694798085a2771c4eef40e93b9b2db a97fcdc2ee778586e6302d490f4e1e67d232417c a97fcdc2ee778586e6302d490f4e1e67d232417c 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
git show HEAD:vm-kit-zedt/harness.c > $D/harness.c && git archive HEAD~3 | tar -x -C $D/am && git archive HEAD~2 | tar -x -C $D/af && git checkout -q HEAD~3
echo "$(git rev-parse HEAD) $(git status --porcelain | wc -l)"
git diff --stat=80 HEAD FETCH_HEAD~1; git diff --stat=80 FETCH_HEAD~1 FETCH_HEAD
printf '%s\n' "d21e253a0d5604dd537d829e69050586c9853e2ead8624c77680198ba5cba617  $D/harness.c" "fe28d956a725b3d10df524fa365b474dc873f888ca67e15028610d6b8f917c64  $D/am/cmd/zed/agents/fmd_api.c" "0b459f08d2b087ef26fd3260f03a5237fc219d04af6ad8ddd83ee99713d09b0f  $D/af/cmd/zed/agents/fmd_api.c" "febcd2ebe57f256373f34f356293d08b45a4884fcc27e2f891cf340cab20624d  $D/am/cmd/zed/agents/zfs_agents.c" "e2e82c3891c5d93cb06880e3bb82e3708376271b4b9424e6bdac2393a790ff2e  $D/af/cmd/zed/agents/zfs_agents.c" "fe28d956a725b3d10df524fa365b474dc873f888ca67e15028610d6b8f917c64  $D/zt/cmd/zed/agents/fmd_api.c" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
```

Expected: TREE-OK; `1b1aa0e91b694798085a2771c4eef40e93b9b2db 0` ($D/zt is now on master, clean); the fix's diffstat

```
 cmd/zed/agents/fmd_api.c    | 99 +++++++++++++++++++++++++++++++++++++--------
 cmd/zed/agents/fmd_api.h    |  7 ++++
 cmd/zed/agents/zfs_agents.c | 14 +++++++
 3 files changed, 103 insertions(+), 17 deletions(-)
```

then the harness commit's

```
 vm-kit-zedt/harness.c | 137 ++++++++++++++++++++++++++++++++++++++++++++++++++
 1 file changed, 137 insertions(+)
```

and SHA-OK. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Edit no file of the three trees.

## STEP 2: the files

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to the path given in its heading. Then:

```sh
D=/var/tmp/task-1120-zedt; L=/var/log/task-1120-zedt
printf '%s\n' "29c018a3aa3902b5f25c3e75e698f771ac6a3d53b56330e82c16799040fa5f06  $L/build-asan.sh" "c4fd6dfc70365108df71827f0539038c6f94f89f90dd2ca35f2e8adb869cd067  $L/hrun.sh" "92deee92914bfd66a99bf16cd7da762677796ea40cdcb8ec3c480ce0d1af8c6d  $D/zedw.sh" "e71826aa3cc268993a81b770582f5731df074f5baeff5a5aa1dbbd881d57944f  $L/ab.sh" "2b789cac37abbe1b46846c6cf243a5a46d1762ffbd7ad8209fb69068536fdf24  $L/summary.sh" "2ce3670dac80604c7502e504e338f86748de5e78cd72e8874db4736ffba9bb73  $L/zedt.run" | sha256sum -c && sh -n $L/build-asan.sh && sh -n $L/hrun.sh && sh -n $D/zedw.sh && sh -n $L/ab.sh && sh -n $L/summary.sh && chmod 755 $D/zedw.sh && echo FILES-OK
```

Expected six `OK` lines and FILES-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

### $L/build-asan.sh

`build-asan.sh` (login user) builds, for `m` then `f`: autogen, configure and `make zed` in `$D/a<v>` (log `$L/build-a<v>.log`), then links the harness `$D/h-<v>` from `$D/harness.c` with that tree's fmd_api.o, fmd_serd.o and libnvpair.a (`-DFMD_TIMER_CLASS_OR_NONE="none"` for master, whose API has no timer class; `-DNEW_API` and the real class for the fix). `-fno-optimize-sibling-calls` keeps `fmd_timer_remove` in the free stacks that ASAN prints. It writes one `a<v>-rc=` line per arm and `DONE` to `$L/build-asan.txt`.

```sh
#!/bin/sh
# TASK-1120-ZEDT ASAN builds of zed and of the harness, run as the login user
D=/var/tmp/task-1120-zedt
L=/var/log/task-1120-zedt
out=$L/build-asan.txt
echo "START $(date -u +%FT%TZ)" >> $out
for v in m f; do
    if [ $v = m ]; then
        X='-DFMD_TIMER_CLASS_OR_NONE="none"'
    else
        X='-DNEW_API -DFMD_TIMER_CLASS_OR_NONE=FMD_TIMER_CLASS'
    fi
    (
        cd $D/a$v &&
        ./autogen.sh &&
        ./configure --with-config=user --enable-debug --enable-asan \
            --disable-shared CFLAGS='-g -O2 -fno-optimize-sibling-calls' &&
        nice make -j"$(nproc)" zed &&
        gcc -fsanitize=address -fno-omit-frame-pointer \
            -fno-optimize-sibling-calls -g -O2 -pthread \
            -include ./zfs_config.h -I./include -I./lib/libspl/include \
            -I./lib/libspl/include/os/linux -I./cmd/zed/agents \
            -D_GNU_SOURCE -D_REENTRANT -DDEBUG $X -o $D/h-$v $D/harness.c \
            cmd/zed/agents/zed-fmd_api.o cmd/zed/agents/zed-fmd_serd.o \
            .libs/libnvpair.a -ltirpc -luuid -lrt
    ) > $L/build-a$v.log 2>&1
    echo "a$v-rc=$? $(date -u +%T)" >> $out
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $L/hrun.sh

`hrun.sh <rounds> <iterations>` (login user) runs `h-m` then `h-f`, `<rounds>` times, each with its own ASAN log prefix `$L/asan-h/h<v><n>`, and writes one line per run to `$L/harness.txt`: `rc`, seconds, the number of ASAN files, the error kind, the top frame `function@file:line`, the first four frames of the free stack, and the program's last stdout line (`iters= fired= posted=` when it ends normally; ASAN halts a program at its first error).

```sh
#!/bin/sh
# TASK-1120-ZEDT harness runs, run as the login user: hrun.sh <rounds> <iters>
D=/var/tmp/task-1120-zedt
L=/var/log/task-1120-zedt
rounds=$1
iters=$2
out=$L/harness.txt
mkdir -p $L/asan-h
echo "START $(date -u +%FT%TZ) rounds=$rounds iters=$iters" >> $out
n=1
while [ $n -le $rounds ]; do
    for v in m f; do
        tag=h$v$n
        s=$(date +%s)
        ASAN_OPTIONS=detect_leaks=0:log_path=$L/asan-h/$tag \
            timeout 600 $D/h-$v $iters > $L/$tag.out 2>&1 < /dev/null
        rc=$?
        e=$(date +%s)
        files=$(ls $L/asan-h/$tag.* 2>/dev/null | wc -l)
        kind=$(cat $L/asan-h/$tag.* 2>/dev/null |
            grep -m 1 -oE 'ERROR: AddressSanitizer: [A-Za-z-]+' | cut -d' ' -f3)
        at=$(cat $L/asan-h/$tag.* 2>/dev/null |
            awk '$1 == "#0" { print $4 "@" $5; exit }')
        freed=$(cat $L/asan-h/$tag.* 2>/dev/null | awk '
            /freed by thread/ { f = 1; next }
            f && $1 ~ /^#[0-9]+$/ { printf "%s ", $4; if (++c == 4) exit }')
        echo "$tag rc=$rc secs=$((e - s)) asan_files=$files kind=[$kind] at=[$at] freed_by=[$freed] | $(tail -n 1 $L/$tag.out)" >> $out
    done
    n=$((n + 1))
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $D/zedw.sh

`zedw.sh` (run by ZTS, as root, in place of zed): see Why. A missing or unknown tag makes it exit 1 without starting zed, so the run's setup fails visibly.

```sh
#!/bin/sh
# TASK-1120-ZEDT: ZTS starts this as zed; it runs the ASAN zed of the arm
D=/var/tmp/task-1120-zedt
L=/var/log/task-1120-zedt
tag=$(cat $D/cur-tag)
case $tag in
m[0-9]*) v=m ;;
f[0-9]*) v=f ;;
*) echo "$(date -u +%T) pid=$$ bad tag [$tag]" >> $L/zedw.txt; exit 1 ;;
esac
echo "$(date -u +%T) pid=$$ tag=$tag args=$*" >> $L/zedw.txt
ASAN_OPTIONS=detect_leaks=0:log_path=$L/asan/$tag
export ASAN_OPTIONS
exec $D/a$v/zed "$@" 2>> $L/zedlog-$tag.txt
```

### $L/ab.sh

`ab.sh <pairs> <minutes>` (login user) runs, for n from 1 to `<pairs>`, `m<n>` then `f<n>`, and starts no new pair once `<minutes>` have passed (a `TIME` line). Each run: the pre-run checks, the tag in `$D/cur-tag`, /proc/spl/kstat/zfs/fm saved before and after, and `zfs-tests.sh -vKO -r $L/zedt.run -T zedt` (console in `$L/zts-<tag>.log`). `$L/ab.txt` gets one line per run: `<tag> rc= secs= wrapper=<wrapper starts> up=<zed start lines> exiting=<zed clean exits> asan_files= asan_errors=[<kind>*<count>] | timer_fired= removing_timer= vdev_io=<solving fault 'fault.fs.zfs.vdev.io'> | <test>:<result>:<duration> ... | logdir=<ZTS results directory>`.

```sh
#!/bin/sh
# TASK-1120-ZEDT A/B loop, run as the login user: ab.sh <pairs> <minutes>
D=/var/tmp/task-1120-zedt
L=/var/log/task-1120-zedt
Z=$D/zt/tests/zfs-tests/bin/zed
pairs=$1
limit=$(($2 * 60))
out=$L/ab.txt
cd $D/zt || exit 1
t0=$(date +%s)
echo "START $(date -u +%FT%TZ)" >> $out
n=1
while [ $n -le $pairs ]; do
    if [ $(($(date +%s) - t0)) -ge $limit ]; then
        echo "TIME: pair $n not started, $2 minutes passed" >> $out
        break
    fi
    for v in m f; do
        tag=$v$n
        pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
        left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed \
            /etc/zfs/vdev_id.conf 2>/dev/null)
        zeds=$(pgrep -x zed; pgrep -x lt-zed)
        loops=$(losetup -a)
        link=$(readlink $Z)
        if [ -n "$pools" ] || [ -n "$left" ] || [ -n "$zeds" ] ||
            [ -n "$loops" ] || [ -s /etc/zfs/zpool.cache ] ||
            [ "$link" != "$D/zedw.sh" ]; then
            echo "STOP before $tag: pools=[$pools] left=[$left] zed=[$zeds] loops=[$loops] link=[$link] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
            exit 1
        fi
        echo $tag > $D/cur-tag
        cp /proc/spl/kstat/zfs/fm $L/fm-$tag-before.txt
        s=$(date +%s)
        sudo -n -u zts ./scripts/zfs-tests.sh -vKO -r $L/zedt.run -T zedt \
            > $L/zts-$tag.log 2>&1 < /dev/null
        rc=$?
        e=$(date +%s)
        cp /proc/spl/kstat/zfs/fm $L/fm-$tag-after.txt
        zl=$L/zedlog-$tag.txt
        starts=$(cat $L/zedw.txt 2>/dev/null | grep -c " tag=$tag ")
        up=$(cat $zl 2>/dev/null | grep -c '^ZFS Event Daemon ')
        down=$(cat $zl 2>/dev/null | grep -cx 'Exiting')
        fired=$(cat $zl 2>/dev/null | grep -c 'timer fired')
        rem=$(cat $zl 2>/dev/null | grep -c 'removing timer')
        vio=$(cat $zl 2>/dev/null |
            grep -c "solving fault 'fault.fs.zfs.vdev.io'")
        files=$(ls $L/asan/$tag.* 2>/dev/null | wc -l)
        errs=$(cat $L/asan/$tag.* 2>/dev/null |
            grep -oE 'ERROR: AddressSanitizer: [A-Za-z-]+' | cut -d' ' -f3 |
            sort | uniq -c | awk '{ printf "%s*%s ", $2, $1 }')
        res=$(grep -E 'Test.*: .*/fault/[^ ]+ \(run as ' $L/zts-$tag.log |
            sed -E 's|.*/fault/([^ ]+) \(run as [^)]*\) +\[([0-9:]+)\] \[([A-Z]+)\].*|\1:\3:\2|' |
            tr '\n' ' ')
        R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag.log)
        echo "$tag rc=$rc secs=$((e - s)) wrapper=$starts up=$up exiting=$down asan_files=$files asan_errors=[$errs] | timer_fired=$fired removing_timer=$rem vdev_io=$vio | $res| logdir=$R" >> $out
    done
    n=$((n + 1))
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $L/summary.sh

`summary.sh` (login user) prints, from `$L/ab.txt`, the totals of each arm and each test's PASS count per arm.

```sh
#!/bin/sh
# TASK-1120-ZEDT per-arm totals of ab.txt: summary.sh
L=/var/log/task-1120-zedt
grep -E '^[mf][0-9]+ ' $L/ab.txt | awk '
{
    v = substr($1, 1, 1)
    runs[v]++
    for (i = 2; i <= NF; i++) {
        if (split($i, a, "=") == 2 &&
            a[1] ~ /^(asan_files|exiting|timer_fired|removing_timer|vdev_io)$/)
            s[v, a[1]] += a[2]
        if (split($i, a, ":") == 4 && a[2] ~ /^[A-Z]+$/) {
            tot[v, a[1]]++
            if (a[2] == "PASS")
                ok[v, a[1]]++
        }
    }
}
END {
    for (v in runs)
        printf "%s totals: runs=%d asan_files=%d exiting=%d timer_fired=%d removing_timer=%d vdev_io=%d\n", v, runs[v], s[v, "asan_files"], s[v, "exiting"], s[v, "timer_fired"], s[v, "removing_timer"], s[v, "vdev_io"]
    for (k in tot) {
        split(k, b, SUBSEP)
        printf "%s %s %d/%d PASS\n", b[1], b[2], ok[k], tot[k]
    }
}' | sort
```

### $L/zedt.run

`zedt.run`: the runfile of the loop. Its `[DEFAULT]` is common.run's; one section runs the four tests with the fault group's setup and cleanup, under the tag `zedt`.

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

[tests/functional/fault]
tests = ['auto_spare_001_pos', 'auto_spare_002_pos', 'auto_spare_multiple',
    'auto_spare_shared']
tags = ['zedt']
```

## STEP 3: the ASAN builds (about 3 minutes)

```sh
L=/var/log/task-1120-zedt
setsid nohup sh $L/build-asan.sh > $L/build-asan.nohup 2>&1 < /dev/null &
```

Poll `cat $L/build-asan.txt` until `DONE`. Expected `am-rc=0` and `af-rc=0`, an empty `build-asan.nohup`; quote every `warning:` line of `grep -h 'warning:' $L/build-am.log $L/build-af.log` (none expected: the tree builds with `-Werror`). A non-zero rc: quote the last 40 lines of that arm's log, stop and report. Then:

```sh
D=/var/tmp/task-1120-zedt
for v in m f; do echo "a$v: $(ASAN_OPTIONS=detect_leaks=0 $D/a$v/zed -V) libasan=$(ldd $D/a$v/zed | grep -c libasan) zed_fmt=$(nm $D/a$v/zed | grep -c ' T fmd_module_timeout$') h_fmt=$(nm $D/h-$v | grep -c ' T fmd_module_timeout$')"; done
ls -l $D/am/zed $D/af/zed $D/h-m $D/h-f
```

Expected `am: zfs-2.4.99-1 libasan=1 zed_fmt=0 h_fmt=0` and `af: zfs-2.4.99-1 libasan=1 zed_fmt=1 h_fmt=1` (`fmd_module_timeout` exists only in the fix), and the four files.

## STEP 4: part 1, the harness (about 1 minute)

```sh
L=/var/log/task-1120-zedt
setsid nohup sh $L/hrun.sh 3 50000 > $L/hrun.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/harness.txt` until `DONE` (an `h-m` run ends at its first report, an `h-f` run takes about 10 s). Then quote:

```sh
L=/var/log/task-1120-zedt
cat $L/harness.txt $L/hrun.nohup; ls -l $L/asan-h
cat "$(ls $L/asan-h/hm1.* | head -n 1)"
```

Expected: three lines `hm<n> rc=1 ... asan_files=1 kind=[heap-use-after-free] at=[_timer_notify@cmd/zed/agents/fmd_api.c:545] freed_by=[free kmem_free fmd_hdl_free fmd_timer_remove ]` (gcc 14 may attribute the read to line 547, the next load of the same chain: say so), and three lines `hf<n> rc=0 ... asan_files=0 kind=[] at=[] freed_by=[] | iters=50000 fired=<about 20000> posted=<about 40000>`, with `posted` above `fired`. `hrun.nohup` empty.

- Any `hf` run with `rc` other than 0 or `asan_files` other than 0: quote `cat $L/asan-h/hf*.* $L/hf*.out`, skip steps 5 and 6, go to step 7 and report. That is the result that matters most.
- An `hm` run that ends clean (`rc=0`, no report) or with another error: quote its report and `.out` file, and go on.

## STEP 5: debug build of master (about 10 minutes)

```sh
D=/var/tmp/task-1120-zedt; L=/var/log/task-1120-zedt
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j\$(nproc); echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 6: part 2, ZTS with the ASAN zed (35 to 55 minutes)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Then point ZTS's zed at the wrapper and check the chain, which `-V` exercises without starting the daemon:

```sh
D=/var/tmp/task-1120-zedt; L=/var/log/task-1120-zedt
mkdir -p $D/zt/tests/zfs-tests/bin && ln -sfn $D/zedw.sh $D/zt/tests/zfs-tests/bin/zed && ls -l $D/zt/tests/zfs-tests/bin/zed $D/zedw.sh
for t in m0 f0; do echo $t > $D/cur-tag && $D/zt/tests/zfs-tests/bin/zed -V; done
cat $L/zedw.txt; ls -A $L/asan; wc -c $L/zedlog-m0.txt $L/zedlog-f0.txt
```

Expected: the link `-> /var/tmp/task-1120-zedt/zedw.sh`, `zedw.sh` with mode `-rwxr-xr-x`, `zfs-2.4.99-1` twice, two `zedw.txt` lines (`tag=m0 args=-V`, `tag=f0 args=-V`), nothing in `$L/asan`, and both zedlog files of 0 bytes. Run the pre-run checks of the VM rules, then:

```sh
L=/var/log/task-1120-zedt
setsid nohup sh $L/ab.sh 3 35 > $L/ab.nohup 2>&1 < /dev/null &
```

A run takes about 9 minutes (auto_spare_001_pos about 2, auto_spare_002_pos up to 1, auto_spare_multiple about 5, auto_spare_shared under 1); the loop runs 2 or 3 pairs. Poll `tail -n 2 $L/ab.txt` and `ls -l --time-style=+%T $L/zts-*.log | tail -n 1` until a `DONE` or a `STOP` line. Then:

```sh
L=/var/log/task-1120-zedt
grep -E '^(START|DONE|STOP|TIME)' $L/ab.txt; cat $L/ab.nohup
grep -E '^[mf][0-9]+ ' $L/ab.txt
sh $L/summary.sh
cat $L/zedw.txt; ls -l $L/asan
for f in $L/fm-*-after.txt; do t=${f#$L/fm-}; t=${t%-after.txt}; echo "$t $(awk 'NR == FNR { b[$1] = $3; next } $1 ~ /^erpt-(dropped|duplicates)$/ { printf "%s+%d ", $1, $3 - b[$1] }' $L/fm-$t-before.txt $f)"; done
for v in m f; do echo "== ${v}1"; grep -m 8 -E 'timer fired|removing timer|solving fault' $L/zedlog-${v}1.txt; done
```

Expected:

- START, DONE, no STOP, an empty `ab.nohup`; 4 or 6 run lines (a `TIME` line before DONE when the loop stopped at 2 pairs).
- Every line with `wrapper=1 up=1 exiting=1`: the group's setup started zed once through the wrapper, and its cleanup stopped it cleanly. Any other count means zed died, did not start or restarted: quote `tail -n 40 $L/zedlog-<tag>.txt` for that run.
- `f`: `asan_files=0` in every run. ASAN stops a zed at its first error (as the segfault did in CI), so the tests after it in that run fail too.
- `m`: most likely `asan_files=0` too (see Why).
- An ASAN error whose stack involves neither the timer functions of fmd_api.c nor the agents queue of zfs_agents.c, and that shows in both arms, is a separate finding, not a verdict on the fix: say so.
- `timer_fired`, `removing_timer` and `vdev_io` above 0 in both arms, of the same order: the I/O-error tests went through the timer. 0 everywhere means these tests did not exercise the timer path: say so.
- setup, cleanup, auto_spare_001_pos, auto_spare_multiple and auto_spare_shared PASS in both arms. auto_spare_002_pos may FAIL in both arms at `wait_vdev_state testpool /var/tmp/file-1 DEGRADED 60 exited 1` in an anymirror case: that is the TASK-1120-ASP test issue (prefetch), whose fix is not in this tree, and it says nothing about zed.

Quote every file of `$L/asan` in full (`cat`). For each test that is not PASS, quote its first `ERROR:` line with the 25 lines before it, from the run's `logdir` (for the known auto_spare_002_pos failure, the `ERROR:` line alone):

```sh
R=<logdir of the run>; t=<test name>
f=$(sudo -n find "$R/output" -type f -path "*fault/$t*" -name stdout | head -n 1); echo "$f"; sudo -n grep -n -m 1 -B 25 'ERROR: ' "$f"
```

The prediction to check: the fix's zed has no ASAN error, lives through every run (`exiting=1`), diagnoses through the timer (`timer_fired` and `vdev_io` above 0) and passes the same tests as master's zed. What contradicts it, and matters most: an ASAN error in an `f` run, an `f` zed that dies, `timer_fired` or `vdev_io` at 0 in `f` while above 0 in `m`, or a test other than the known auto_spare_002_pos failure that PASSes with `m` and FAILs with `f`. For such a run, also quote `grep -n -E 'timer|solving fault|case (opened|closed)' $L/zedlog-<tag>.txt | tail -n 60`, and go on with the plan. An ASAN error in an `m` run confirms the bug in situ: quote it in full.

After the loop: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault` quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`.

## STEP 7: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed 2>&1` (none), `losetup -a` (empty), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L (the wrapper went with $D; its text is in this task), the ZTS results under /var/tmp/test_results.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, gcc version and the libasan path, the zed line of step 0; TREE-OK, SHA-OK, the two diffstats; FILES-OK; the ASAN builds' rc and warnings and the `a<v>:` lines; the debug build's rc and warnings; S1 and DEBUG mode; the link and `-V` checks; what happened to /etc/zfs/zpool.cache at steps 0 and 7.
2. Part 1, a table: tag | rc | seconds | ASAN kind | where | freed by | stdout. Then one verdict line, e.g. "master fmd_api.c: 3/3 heap-use-after-free in _timer_notify, freed by fmd_timer_remove; fix c40989fdd2f: 3/3 clean, F timeouts delivered, P expirations posted".
3. Part 2, a table: tag | arm | rc | duration | per test result | wrapper/up/exiting | ASAN | timer_fired/removing_timer/vdev_io; then the `summary.sh` output and one verdict line, e.g. "fix zed: N runs, 0 ASAN error, zed alive through every run, timer path exercised (T timer fired, V vdev.io faults), same PASS set as master's zed; master zed: K ASAN errors".
4. Quotes: START/DONE/TIME lines, the zed log samples, the ASAN files, the failing tests' lines, the erpt deltas, the dmesg check.
5. Deviations, cleanup, then End.
