TASK-1120-SCL (from the coordinator session "main"): functional, about 2 h (a debug build of upstream master, then one test in a loop for up to 100 minutes), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB); it can run while the other VMs run other functional tasks, not next to a performance run.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-scl`, `L=/var/log/task-1120-scl`, `Z=$D/zt/zpool`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

`replacement/scrub_cancel` hangs in about 3 % of the fork's CI runs: it stalls at `log_must zpool wait $TESTPOOL1` in its sequential-rebuild pass, right after `set_tunable32 RESILVER_MIN_TIME_MS 3000`, until test-runner kills it at its 600 s timeout. The CI logs hold no state of the pool while it waits. This task loops the test on upstream master's debug build and, when a `zpool wait` of the test has waited more than 60 s, captures the state of the pool while it is stuck, killing nothing; ZTS then kills the test as in CI, and the loop stops.

The test (tests/zfs-tests/tests/functional/replacement/scrub_cancel.ksh): `testpool1` is a stripe of four 128 MiB files, /var/tmp/file-1 to file-4, with 64 MiB of random data. For `healing`, then `sequential`: RESILVER_MIN_TIME_MS 50 and SCAN_SUSPEND_PROGRESS 1; `zpool scrub`; `zpool replace [-s] testpool1 /var/tmp/file-2 /var/tmp/spare-1`, which cancels the scrub; a wait until no scrub runs; a check that a resilver runs and that a scrub is refused; SCAN_SUSPEND_PROGRESS 0 and RESILVER_MIN_TIME_MS 3000; `zpool wait testpool1`; `zpool replace [-s] -w` back to file-2. `zpool wait` with no `-t` waits for each activity in turn (`discard`, `free`, `initialize`, `replace`, `remove`, `resilver`, `scrub`, `trim`, `raidz_expand`, `condense`) until none is in progress. In the sequential pass, `zpool replace -s` starts a rebuild of top-level vdev 1, now `replacing-1`, in a kernel thread named `vdev_rebuild` (`vdev_rebuild_thread()`). When it completes, `vdev_rebuild_complete_sync()` marks the rebuild COMPLETE, asks the async thread (`spa_async`) to detach the replaced file (`spa_vdev_resilver_done()`), and starts a verification scrub (`zfs_rebuild_scrub_enabled`), which the wait covers too.

Two hypotheses to confirm or refute from the captures (line numbers of master `1b1aa0e91b6`):

- **(a) A rebuild left ACTIVE with no thread** (vdev_rebuild.c 980-988). A rebuild thread that stops with an error while no cancel and no reset is wanted takes the "suspend" branch: it clears `vdev_rebuilding` and exits, and the rebuild stays ACTIVE. Nothing restarts it before the next import, so `vdev_rebuild_active()` stays true and the resilver and replace waits never end. Signature: `zpool status` shows the sequential resilver in progress, with the same figures in both captures; the `resilver` and `replace` probes time out; no `vdev_rebuild` thread exists; the history and the dbgmsg show the rebuild of `vdev_id=1` started and never `complete`, `canceled` or `reset`.
- **(b) A replacing vdev never detached** (spa.c 9999-10040). `spa_vdev_resilver_done()` detaches the old half of a replacing vdev once `spa_vdev_resilver_done_hunt()` finds the new half's DTLs empty; when `spa_vdev_detach()` fails it returns at line 10026, and when the hunt finds nothing it detaches nothing. `replacing-1` stays, `vdev_replace_in_progress()` stays true, and the replace wait never ends. Signature: `zpool status` still shows `replacing-1` with file-2 and spare-1 and the rebuild finished; the `replace` probe times out while `resilver` and `scrub` return at once; the history shows the rebuild `complete` and no `detach` of /var/tmp/file-2 after it. The DTLs that `zdb -ddd` prints ("Dirty time logs") tell the two cases apart: a `missing` range left on spare-1 means the hunt found nothing to detach; no range there means the detach itself failed.

The captures also cover two other readings: (c) a waiter that missed its wake-up: every probe returns at once while the test's `zpool wait` still sleeps (unlikely: `spa_sync()` wakes the waiters after every txg, spa.c 11613-11618, and an idle pool still syncs a txg every `zfs_txg_timeout` seconds); (d) the verification scrub never ending: the `scrub` probe times out and `zpool status` shows a scrub in progress.

What is tested: upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` (20/11), tree `2694c12c1e2999e18b844ab46ba029aa42693c09`, fetched by its SHA from the fork https://github.com/tigerblue77/zfs (no kit branch: the scripts are in this file). The parts:

- `loop.sh 200 100` (step 4): runs `zfs-tests.sh -vKO -t tests/functional/replacement/scrub_cancel` up to 200 times; it starts no run once 100 minutes have passed, nor once the watchdog has caught one. Each run gets a line in $L/loop.txt with its rc, its duration, the two `zpool wait` durations in seconds (healing, sequential, from the test's log) and its results.
- `wd.sh` (root, detached): the watchdog. Every second it looks for the test's `zpool wait testpool1` (or `zpool replace ... -w`); once one has run more than 60 s, it writes $L/caught, logs a `HANG` line, and captures into $L/wd/cap-<run>-1/; again at 300 s into cap-<run>-2/ (has anything moved?). It kills nothing. Each command that enters the kernel runs in the background and is waited for at most a fixed time, so a blocked one cannot block the watchdog: it is then reported as still running. A test still running after 420 s with no such `zpool` process (a hang elsewhere) gets one capture, cap-<run>-other/.
- A capture: `wait.txt` (the `ps` line, kernel stack, wait channel and signal state of the waiting `zpool`); `dbgmsg-1.txt` (`/proc/spl/kstat/zfs/dbgmsg` before any command: `zfs-tests.sh` sets zfs_dbgmsg_enable=1 and clears the log at the start of each run, and step 4 sets it before the loop); `kstat.txt` (the pool's `state`, `misc` and last `txgs` kstats, and the scan and rebuild tunables: master has no kstat of its own for a rebuild or a scan, whose state `zpool status` shows); `threads.txt` (the `vdev_rebuild`, `spa_async`, `txg_sync` and `txg_quiesce` threads, with their kernel stacks); `status-1.txt` (`zpool status -v`); ten probes `wait-<activity>.txt`, each `timeout 5 zpool wait -t <activity> testpool1`, `resilver`, `replace` and `scrub` first (rc 124: still in progress after 5 s); `history.txt` (`zpool history -il`); `events.txt` (`zpool events`); `zdb.txt` (`zdb -ddd testpool1`, read from the files in user space, for its DTLs); `sysrq-w.txt` (the blocked tasks that `echo w > /proc/sysrq-trigger` puts in dmesg); `dbgmsg-2.txt`; `status-2.txt`. `probes.txt` has one line per bounded command: its rc and duration in ms, or "still running".

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs: /var/tmp/file-vdev0 to 2 (`zfs-tests.sh`), /var/tmp/file-1 to 4 and /var/tmp/spare-1 (the test). Touch no block device of the VM. Never run `zpool import` yourself (`-a` least of all); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank`, or any pool other than ZTS's `testpool` and `testpool1`) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Nothing in this task kills a process, ZTS's own timeout excepted: never `kill` a `zpool`, `ksh` or test process yourself, not even a stuck one.
- Before the loop: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $Z list -H -o name` shows no pool, `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* 2>&1` shows nothing, `pgrep -a -x zpool; pgrep -a -x lt-zpool` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. `loop.sh` repeats these checks (all but lsmod) before each run, also checks that the watchdog runs, and writes a `STOP` line if one fails.
- The build, the watchdog and the loop run detached (`setsid nohup ... &`) and are polled. A hang is: the build still going after 90 minutes; $L/loop.txt unchanged for 30 minutes, or the loop still going after 130 minutes (100 minutes of runs, plus a caught run, which lasts up to about 25 minutes: 600 s until ZTS kills the test, then the directory's cleanup); a `HANG` line in $L/wd.txt with no `CAPTURED <run> 1` line 10 minutes later (a capture command blocked: note which files of its directory are missing or empty). Then capture `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|spa_|vdev_|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop, report, and leave everything as it is.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. The module is unloaded at the end, unless step 5 finds a pool left.

## STEP 0: state

`hostname`, `uname -r`, `cat /etc/debian_version`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `pgrep -a -x zpool; pgrep -a -x lt-zpool` (empty), and `cat /proc/sys/kernel/sysrq` (quote it; any value works, since root's writes to /proc/sysrq-trigger do not depend on it).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-scl
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-scl; L=/var/log/task-1120-scl
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D $L
mkdir $D/zt $L/wd && cd $D/zt && git init -q . && git fetch -q --depth 1 https://github.com/tigerblue77/zfs 1b1aa0e91b694798085a2771c4eef40e93b9b2db && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD HEAD^{tree} | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree.txt
echo "1b1aa0e91b694798085a2771c4eef40e93b9b2db 2694c12c1e2999e18b844ab46ba029aa42693c09 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional/replacement
printf '%s\n' "8628d2d80695770c0a330c3aa7f91d9ac4db594eaefebc3c914c040b6d59d918  $T/scrub_cancel.ksh" "30dfbe01267e64eee41362323c040455dea16c76c844f2f4c3f50699cc884c07  $T/replacement.cfg" "1e819561df906d5d74bc625aa6eb8616c36fcabb016a7a5848315b599ef733d4  module/zfs/vdev_rebuild.c" "ebc6bb692afded30d7d61a0da7befc1e7c4cab9b6ca4c01f0b34893e7134bf44  module/zfs/spa.c" "690bfa9dca4526c679403b5122e1c43919db595b06bc253d8b367a8a44831110  module/zfs/dsl_scan.c" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
sed -n '987p' module/zfs/vdev_rebuild.c; sed -n '10026p;10039p;11618p' module/zfs/spa.c
```

Expected TREE-OK, SHA-OK, then the four lines `vd->vdev_rebuilding = B_FALSE;`, `if (spa_vdev_detach(spa, guid, pguid, B_TRUE) != 0)`, `spa_notify_waiters(spa);`, `spa_notify_waiters(spa);` (with their tabs). If the fetch fails, wait 1 minute and try once more; if it fails again, try once `https://github.com/openzfs/zfs` with the same SHA and say so. Any other difference: stop and report as is. Do not edit any file of the tree.

## STEP 2: the scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to the path given in its heading. Then:

```sh
L=/var/log/task-1120-scl
printf '%s\n' "b7f26ec9cd742cd548e749480dfaeb43df67446afd501065df093f780c44e0a6  $L/loop.sh" "78d9f90c33e6d5169e796a2ed4804d42f104cdb7fe34a26dc9f8964a036b45e3  $L/wd.sh" "824a61d149e0cb88f4071252752ef107da614cf36fe716241c8fe5101a0c4a00  $L/sum.sh" | sha256sum -c && sh -n $L/loop.sh && sh -n $L/wd.sh && sh -n $L/sum.sh && echo SCRIPTS-OK
```

Expected three `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

### $L/loop.sh

`loop.sh <runs> <minutes>` (login user): see Why. $L/loop.txt gets one line per run, `run-<n> rc=<rc> secs=<s> waits=<healing>,<sequential> setup:<result>:<duration> scrub_cancel:<result>:<duration> cleanup:<result>:<duration> dir=<results dir>`, and `CAUGHT`, `TIME`, `STOP` and `DONE` lines; the console of run n goes to $L/zts-<n>.log, and its number to $D/cur-run, where the watchdog reads it.

```sh
#!/bin/sh
# TASK-1120-SCL loop of replacement/scrub_cancel, login user: loop.sh <runs> <minutes>
D=/var/tmp/task-1120-scl
L=/var/log/task-1120-scl
T=tests/functional/replacement/scrub_cancel
max=$1
limit=$(($2 * 60))
out=$L/loop.txt
cd $D/zt || exit 1
t0=$(date +%s)
echo "START $(date -u +%FT%TZ) runs=$max minutes=$2" >> $out
n=1
while [ $n -le $max ]; do
    if [ -e $L/caught ]; then
        echo "CAUGHT: run $n not started, $(cat $L/caught)" >> $out
        break
    fi
    if [ $(($(date +%s) - t0)) -ge $limit ]; then
        echo "TIME: run $n not started, $2 minutes passed" >> $out
        break
    fi
    pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
    inj=$(sudo -n $D/zt/zinject 2>&1 | head -n 1)
    loops=$(sudo -n losetup -a 2>&1)
    left=$(ls -d /var/tmp/file-* /var/tmp/spare-* 2>/dev/null)
    zp=$(pgrep -a -x zpool; pgrep -a -x lt-zpool)
    wd=$(pgrep -f "sh $L/wd.sh")
    if [ -n "$pools" ] || [ "$inj" != "No handlers registered." ] ||
        [ -n "$loops" ] || [ -n "$left" ] || [ -n "$zp" ] ||
        [ -z "$wd" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before run $n: pools=[$pools] zinject=[$inj] loops=[$loops] left=[$left] zpool=[$zp] wd=[$wd] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    echo $n > $D/cur-run
    s=$(date +%s)
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t $T \
        > $L/zts-$n.log 2>&1 < /dev/null
    rc=$?
    e=$(date +%s)
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$n.log)
    res=$(grep -E 'Test: .*/functional/replacement/[^ ]+ \(run as ' \
        $L/zts-$n.log |
        sed -E 's|.*/functional/replacement/([^ ]+) \(run as [^)]*\) +\[([0-9:]+)\] \[([A-Z]+)\].*|\1:\3:\2|' |
        paste -s -d ' ' -)
    w=$(sudo -n awk '
        function t(s, a) {
            split(s, a, ":")
            return a[1] * 3600 + a[2] * 60 + a[3]
        }
        / SUCCESS: set_tunable32 RESILVER_MIN_TIME_MS 3000$/ { s = t($1) }
        / SUCCESS: zpool wait testpool1$/ && s != "" {
            d = t($1) - s
            if (d < 0)
                d += 86400
            printf "%s%.1f", c, d
            c = ","
            s = ""
        }' "$R/log" 2>/dev/null)
    echo "run-$n rc=$rc secs=$((e - s)) waits=$w $res dir=$R" >> $out
    n=$((n + 1))
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

### $L/wd.sh

`wd.sh` (root, detached): see Why. It stops when $L/wd/stop exists. $L/wd.txt gets `START`, `HANG run=<n> pid=<pid> secs=<s> args=[<command>]` when it catches a wait, `OTHER run=<n>` for a test running more than 420 s with no such wait, `CAPTURED <run> <label> pid= dir=` after each capture, and `DONE`.

```sh
#!/bin/sh
# TASK-1120-SCL watchdog of a stuck zpool wait, run as root: wd.sh
D=/var/tmp/task-1120-scl
L=/var/log/task-1120-scl
Z=$D/zt/zpool
P=testpool1
W=$L/wd
echo "START $(date -u +%FT%TZ)" >> $L/wd.txt

alive() {
    [ -e /proc/$1 ] && [ "$(cut -d' ' -f3 /proc/$1/stat 2>/dev/null)" != Z ]
}

# bounded <seconds> <file> <command...>: run the command in the background
# and wait for it at most <seconds>, so that a command blocked in the kernel
# cannot block the watchdog; print its rc and duration, or that it still runs.
bounded() {
    s=$1
    f=$2
    shift 2
    t1=$(date +%s%3N)
    "$@" > $f 2>&1 < /dev/null &
    b=$!
    i=0
    while alive $b && [ $i -lt $((s * 10)) ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if alive $b; then
        echo "$* -> still running after $s s (pid $b)"
    else
        wait $b
        echo "$* -> rc=$? ms=$(($(date +%s%3N) - t1))"
    fi
}

# cap <pid|none> <label>: the state of the pool while the wait is stuck
cap() {
    w=$1
    C=$W/cap-$(cat $D/cur-run)-$2
    mkdir -p $C
    if [ "$w" != none ]; then
        {
            echo "== $(date -u +%FT%TZ) pid=$w"
            ps -o pid,ppid,stat,etimes,wchan:32,args -p $w
            echo "== stack"
            cat /proc/$w/stack
            echo "== wchan $(cat /proc/$w/wchan)"
            grep -E '^(State|SigPnd|ShdPnd|SigBlk):' /proc/$w/status
        } > $C/wait.txt 2>&1
    fi
    cat /proc/spl/kstat/zfs/dbgmsg > $C/dbgmsg-1.txt 2>&1
    {
        for k in state misc; do
            echo "== $k"
            cat /proc/spl/kstat/zfs/$P/$k
        done
        echo "== txgs"
        tail -n 12 /proc/spl/kstat/zfs/$P/txgs
        echo "== tunables"
        for t in zfs_resilver_min_time_ms zfs_scan_suspend_progress \
            zfs_rebuild_scrub_enabled zfs_scrub_min_time_ms \
            zfs_txg_timeout zfs_dbgmsg_enable; do
            echo "$t=$(cat /sys/module/zfs/parameters/$t)"
        done
    } > $C/kstat.txt 2>&1
    {
        ps -eLo pid,tid,stat,etimes,wchan:32,comm |
            grep -E 'vdev_rebuild|spa_async|txg_sync|txg_quiesce|zpool|ksh'
        for t in $(ps -eLo tid=,comm= |
            awk '$2 ~ /^(vdev_rebuild|spa_async|txg_sync|txg_quiesce)$/ { print $1 }'); do
            echo "== tid $t $(cat /proc/$t/comm)"
            cat /proc/$t/stack
        done
    } > $C/threads.txt 2>&1
    {
        bounded 30 $C/status-1.txt $Z status -v $P
        for a in resilver replace scrub condense free discard initialize \
            trim remove raidz_expand; do
            bounded 15 $C/wait-$a.txt timeout 5 $Z wait -t $a $P
        done
        bounded 30 $C/history.txt $Z history -il $P
        bounded 30 $C/events.txt $Z events
        bounded 120 $C/zdb.txt $D/zt/zdb -ddd $P
    } > $C/probes.txt 2>&1
    k=$(dmesg | wc -l)
    echo w > /proc/sysrq-trigger
    sleep 2
    dmesg | tail -n +$((k + 1)) > $C/sysrq-w.txt
    cat /proc/spl/kstat/zfs/dbgmsg > $C/dbgmsg-2.txt 2>&1
    bounded 30 $C/status-2.txt $Z status -v $P >> $C/probes.txt 2>&1
    echo "CAPTURED $(cat $D/cur-run) $2 pid=$w dir=$C $(date -u +%T)" >> $L/wd.txt
}

while [ ! -e $W/stop ]; do
    r=$(cat $D/cur-run 2>/dev/null)
    # the newest zpool process of the test that waits on testpool1
    ps -eo pid=,etimes=,comm=,args= | awk -v p=$P '
        $3 ~ /^(lt-)?zpool$/ &&
            (($(NF - 1) == "wait" && $NF == p) || / replace( -s)? -w /) {
            w = $1 " " $2
        }
        END { if (w != "") print w }' > $W/now
    read -r pid secs < $W/now
    if [ -n "$pid" ] && [ "$secs" -gt 60 ] && [ ! -e $W/run-$r.c1 ]; then
        a=$(tr '\0' ' ' < /proc/$pid/cmdline)
        touch $W/run-$r.c1
        echo "run=$r pid=$pid [$a] $(date -u +%T)" > $L/caught
        echo "HANG run=$r pid=$pid secs=$secs args=[$a] $(date -u +%T)" >> $L/wd.txt
        cap $pid 1 < /dev/null
    elif [ -n "$pid" ] && [ "$secs" -gt 300 ] && [ ! -e $W/run-$r.c2 ]; then
        touch $W/run-$r.c2
        cap $pid 2 < /dev/null
    fi
    if [ -n "$r" ] && [ ! -e $W/run-$r.c1 ] &&
        ps -eo etimes=,args= |
        awk '/[s]crub_cancel\.ksh/ && $1 > 420 { f = 1 } END { exit !f }'; then
        touch $W/run-$r.c1
        echo "run=$r test older than 420 s, no zpool wait $(date -u +%T)" > $L/caught
        echo "OTHER run=$r test older than 420 s $(date -u +%T)" >> $L/wd.txt
        cap none other < /dev/null
    fi
    sleep 1
done
echo "DONE $(date -u +%FT%TZ)" >> $L/wd.txt
```

### $L/sum.sh

`sum.sh` (login user) prints, from $L/loop.txt, the number of runs, the results of `scrub_cancel` (and any setup or cleanup that did not pass), and for each pass the `zpool wait` durations: count, minimum, median, maximum (with its run), and how many took 10 s or more and 30 s or more.

```sh
#!/bin/sh
# TASK-1120-SCL loop summary, login user: sum.sh
L=/var/log/task-1120-scl
r=$(grep -E '^run-[0-9]+ ' $L/loop.txt)
echo "runs: $(echo "$r" | grep -c '^run-')"
echo "$r" | grep -oE 'scrub_cancel:[A-Z]+' | sort | uniq -c
echo "$r" | grep -oE '(setup|cleanup):(FAIL|KILLED|SKIP)' | sort | uniq -c
echo "$r" | awk '{
    for (i = 2; i <= NF; i++)
        if ($i ~ /^waits=/) {
            k = split(substr($i, 7), w, ",")
            for (j = 1; j <= k; j++)
                print (j == 1 ? "healing" : "sequential"), $1, w[j]
        }
}' | sort -k1,1 -k3,3n | awk '
{
    p = $1
    n[p]++
    v[p, n[p]] = $3
    r[p, n[p]] = $2
    if ($3 >= 10)
        g10[p]++
    if ($3 >= 30)
        g30[p]++
}
END {
    for (p in n) {
        m = n[p]
        printf "%s wait: n=%d min=%s median=%s max=%s (%s) ge10s=%d ge30s=%d\n", p, m, v[p, 1], v[p, int((m + 1) / 2)], v[p, m], r[p, m], g10[p], g30[p]
    }
}'
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-scl; L=/var/log/task-1120-scl
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 4: the loop

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Then:

```sh
echo 1 | sudo -n tee /sys/module/zfs/parameters/zfs_dbgmsg_enable
for t in zfs_resilver_min_time_ms zfs_scan_suspend_progress zfs_rebuild_scrub_enabled zfs_txg_timeout zfs_dbgmsg_enable zfs_dbgmsg_maxsize; do echo "$t=$(cat /sys/module/zfs/parameters/$t)"; done
```

Expected `1`, then 1500, 0, 1, 5, 1 and 4194304: quote them. Start the watchdog:

```sh
L=/var/log/task-1120-scl
sudo -n setsid nohup sh $L/wd.sh > $L/wd.nohup 2>&1 < /dev/null &
sleep 2; pgrep -a -f "sh $L/wd.sh"; cat $L/wd.txt
```

Expected a `wd.sh` process and a `START` line. Run the pre-run checks of the VM rules, then start the loop:

```sh
L=/var/log/task-1120-scl
setsid nohup sh $L/loop.sh 200 100 > $L/loop.nohup 2>&1 < /dev/null &
```

A run takes about a minute. Poll `tail -n 2 $L/loop.txt; tail -n 2 $L/wd.txt` every few minutes until a `DONE` or a `STOP` line in loop.txt. A `HANG` line in wd.txt needs nothing from you: the watchdog captures within a few minutes (`CAPTURED <run> 1`), again at 300 s (`CAPTURED <run> 2`), and kills nothing; ZTS kills the test at 600 s and runs the directory's cleanup, loop.sh then writes the run's line, a `CAUGHT` line and `DONE`. Note the time of the `HANG` line, and meanwhile read nothing but these two files and the capture directory. A `STOP` line (a pre-run check failed, e.g. after a run that failed without hanging): quote it and go on with step 5, whose state checks cover what it found.

## STEP 5: after the loop

```sh
L=/var/log/task-1120-scl
touch $L/wd/stop; sleep 3; tail -n 1 $L/wd.txt
grep -E '^(START|DONE|STOP|CAUGHT|TIME)' $L/loop.txt; cat $L/loop.nohup $L/wd.nohup
sh $L/sum.sh
grep -E '^run-' $L/loop.txt | grep -v ' scrub_cancel:PASS:'; cat $L/wd.txt; ls -d $L/wd/cap-* 2>&1
grep -E '^run-' $L/loop.txt | awk '{ for (i = 2; i <= NF; i++) if ($i ~ /^waits=[^,]*,/) { split(substr($i, 7), w, ","); print w[2], $0 } }' | sort -n | tail -n 3
```

Expected the watchdog's `DONE` line, empty nohup files, the summary, the runs whose test did not pass (none, or the caught one), the watchdog's lines, and the run lines of the three slowest sequential waits (each preceded by that wait in seconds).

If the watchdog caught a run, quote for each capture directory C, in this order:

```sh
C=<capture directory>
cat $C/probes.txt; cat $C/wait.txt; cat $C/threads.txt; cat $C/kstat.txt
cat $C/status-1.txt; cat $C/status-2.txt
grep -nE 'rebuild|scan|detach|attach|replace|vdev' $C/history.txt | tail -n 40; tail -n 30 $C/events.txt
sed -n '/Dirty time logs/,$p' $C/zdb.txt | head -n 60; tail -n 3 $C/zdb.txt
grep -c . $C/sysrq-w.txt; head -n 80 $C/sysrq-w.txt
wc -l $C/dbgmsg-1.txt $C/dbgmsg-2.txt; grep -nE 'rebuild|scan|detach|attach|replac|resilver|scrub|async' $C/dbgmsg-1.txt | tail -n 80; tail -n 20 $C/dbgmsg-2.txt
```

(The files of a probe, `wait-<activity>.txt`, are empty unless the command printed something: quote any that is not.) Then, for the caught run, with R its `dir=` in loop.txt: `grep -nE 'replace|zpool wait|set_tunable32|scrub|KILLED|ERROR|FAIL' $R/log | tail -n 40`, and the timeout report of test-runner (`-O`) in dmesg: `sudo -n dmesg | tail -n +$((N0+1)) | grep -n -A 14 'ZTS timeout debug' | head -n 60`. A caught run that ended in PASS was slow, not stuck: say so, and quote its waits.

Then the verdict on each hypothesis, from the captures: (a), (b), (c), (d) confirmed, refuted or undecided, each with the lines it rests on; anything that matches none of them, described as it is.

Then the state left behind:

```sh
D=/var/tmp/task-1120-scl; Z=$D/zt/zpool
timeout 30 sudo -n $Z list -H -o name; pgrep -a -x zpool; pgrep -a -x lt-zpool; ls -l /var/tmp/file-* /var/tmp/spare-* 2>&1; sudo -n losetup -a; ls -l /etc/zfs/zpool.cache 2>&1
for t in zfs_resilver_min_time_ms zfs_scan_suspend_progress; do echo "$t=$(cat /sys/module/zfs/parameters/$t)"; done
```

With no catch: no pool, no `zpool` process, no file, `1500` and `0`. After a catch the test's own cleanup may not have run (ZTS stops the test with SIGTERM): the tunables may still read 3000 and 0, and the files of the test may remain.

- A pool left: quote `timeout 30 sudo -n $Z status -v <pool>` and, for testpool1, the three probes again (`for a in resilver replace scrub; do sudo -n timeout 5 $Z wait -t $a testpool1; echo "$a rc=$?"; done`), then stop: leave the pool, the module and any `zpool` process as they are, report, and wait for main.
- No pool, but a `zpool` process left: quote its `ps -o pid,stat,etimes,wchan:32,args -p <pid>` and `sudo -n cat /proc/<pid>/stack`, then stop as above (do not kill it).
- Otherwise: the files left are the test's: record their `ls -l`, remove them (`sudo -n rm -f /var/tmp/file-[1-4] /var/tmp/spare-[12]`), `sudo -n fstrim -av`.

Then every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault` with 10 lines before it (none expected; the sysrq dump of a capture and test-runner's timeout report are expected after a catch: count their lines, do not quote them in full), and unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## STEP 6: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `pgrep -a -f wd.sh` (empty), `sudo -n losetup -a` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs, scripts and captures stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line and the sysrq value of step 0; TREE-OK, SHA-OK and the four source lines; SCRIPTS-OK; build rc and warnings; S1 and DEBUG mode; the tunables before the loop; what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. The loop: its START, CAUGHT, TIME, STOP and DONE lines, the number of runs and the minutes they took, the output of `sum.sh`, and every run line whose test did not pass.
3. If a run was caught: its number, the command that waited and how long, then the captures as quoted in step 5, the caught run's log lines and test-runner's timeout report, and one verdict line per hypothesis, e.g. "(b) confirmed: status-1 and status-2 show replacing-1 (file-2, spare-1) with the rebuild finished; replace probe rc=124, resilver and scrub rc=0 in under 100 ms; history: rebuild complete at txg N, no detach after it; zdb: no DTL range on spare-1, so the detach itself failed". If no run was caught: say so plainly, with the number of runs, the sequential waits from `sum.sh` and the run lines of the three slowest sequential waits.
4. The state left behind, the dmesg check, deviations, cleanup, then End.
