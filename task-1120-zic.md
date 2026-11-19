TASK-1120-ZIC (from the coordinator session "main"): functional, about 1 h 30, debug build only (one tree, rebuilt incrementally for master), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The two scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-zic`, `L=/var/log/task-1120-zic`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

Since #16938 (2aa3fbe761a) every zinject handler counts its matches and its injections (`zi_match_count`, `zi_inject_count`: the MATCH and INJECT columns of `zinject`). The hooks run concurrently for zios on different vdevs and taskq threads and hold `inject_lock` only as a reader, so master's plain `++` can lose an update, and the listing can copy a record between a hook's two increments. `zinject_counts` compares the two at 100% frequency and has failed that way here: `match=98 inject=97` (SLD3, VM 106) and 93 against 94 (PDL, VM 107). The branch counts with `atomic_inc_64()` wherever the reader lock is held (the delay hook keeps `++` under `inject_delay_mtx`) and copies the record for the listing with `inject_lock` held as a writer.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zinject-atomic-counts`, head `e29fc6e82a59f0c3802af6db7d5dfed5978d2db8`, tree `d1710cd560cd13782b84dc1c7d9f56b3b9b20b45`, one commit on `cde5d598eb44b5d6efe8167666d988a496346657` (upstream master `c913da86156` plus one CI script commit of the fork: its module source is master's). The commit changes module/zfs/zio_inject.c only. Master (step 5) is the same tree with that file checked out from cde5d598eb4: tree `d70a147b0775bf4bd216072a8b05738b603d6849`, cde5d598eb4's own tree. ZVH and ROV3 recorded srcversion `65D8C0C24A9B688639F0721` for master's module source.

Each tree gets two kinds of trials:

- `zinject_counts`, 30 runs. Each check prints `check_count_freq: freq=<f> match=<m> inject=<i>`: device write errors, then data checksum errors, then delays, each at freq=100 then 50. At freq=100, m must equal i; the first unequal pair fails the test and ends it. A freq=50 check fails only when the injections are 20 or more away from half the matches: that is the test's statistics, not this race, and is recorded apart.
- A stress loop, 50 iterations (`stress.sh`, step 2): a 3-way mirror of three 1 GiB files under $D, a failfast handler failing every write to f1 (`zinject -d <f1> -e io -T write -f 100 -F`), eight parallel `dd` of 64 MiB, `zpool sync`, then the handler's MATCH and INJECT, which must be equal. This is zinject_counts' own layout (a 3-way mirror, the handler on the first vdev, buffered writes) with eight writers instead of one, plus `-F`; the mirror absorbs every failed write, so the pool never suspends and `dd` never fails. The pool is created with `cachefile=none` and mounted under $D, so nothing lands outside $D.

Why `-F`: without it, the first failed write makes ZFS probe f1, the handler fails the probe's writes too, and f1 is FAULTED, so each iteration would count only the writes in flight before that, as zinject_counts does. A failfast handler skips I/O flagged `ZIO_FLAG_TRYHARD` or `ZIO_FLAG_IO_RETRY` before counting anything, and the probe's I/O is TRYHARD, so the probe succeeds, f1 stays writable, and every write to f1 is counted (match and inject stay paired at freq=100). Only zed's diagnosis, if zed runs (step 0 records it), may still fault f1. How many writes each iteration counts is not known before the run. The loop records f1's state and write errors per iteration: ONLINE is expected without zed; FAULTED or DEGRADED is recorded, never a stop.

Expected: on the branch, no mismatch at all. On master, at least one mismatch across the 30 + 50 trials. The race is rare: master showing none makes the comparison uninformative, not a failure of the fix. Report it so, with the number of matches counted.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs and the stress loop only the files under $D; touch no block device of the VM; never `zpool import` anything (`-a` least of all); never `zfs-tests.sh -x`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks: during ROV3 it imported VM 105's exported measurement pool `tank`, which may still be there, exported. No run of this task includes it, but after every run and every loop `zpool list` must still show no pool. A pool this task did not create (`tank` or any other) is a stop: report it before the next run, and leave it as it is (no export, no destroy, no import).
- Before every ZTS run and every loop: `lsmod | grep -E '^zfs '` shows the module of this step only, `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool, `sudo -n $D/zd/zinject` prints `No handlers registered.`, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. `counts.sh` repeats these checks (all but lsmod and zinject) before each of its runs and writes a `STOP` line if one fails.
- Every build, ZTS run and loop runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 30 minutes; a group after 75 minutes; a counts loop whose `counts-<tag>.txt` has not changed for 30 minutes or that is still going after 75 minutes; a stress loop whose `stress-<tag>.txt` has not changed for 10 minutes or that is still going after 45 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- Reboots: one is pre-approved only where a task says so, and this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end. Each stress loop writes about 75 GiB to the VM's disk (1.5 GiB per iteration: f1 stays in the mirror): nothing else to run beside it.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `id zts`, and `pgrep -a -x zed || echo NO-ZED` (recorded either way, not a stop; do not start or stop zed).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-zic
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-zic; L=/var/log/task-1120-zic
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs zinject-atomic-counts && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "e29fc6e82a59f0c3802af6db7d5dfed5978d2db8 cde5d598eb44b5d6efe8167666d988a496346657 d1710cd560cd13782b84dc1c7d9f56b3b9b20b45 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "117130cd6fd90a719cfb9a392e53a1459fd9074f41874781f4524e418a2e4bda  module/zfs/zio_inject.c" "43d8dd234564778fc4083e55af9454a4c80db70c295cc2175b1390beda9a85cf  cmd/zinject/zinject.c" "5325d29751359df12b8cec3c7b5973bb5d9db817a9857128f51f1db042ffceb1  tests/zfs-tests/tests/functional/cli_root/zinject/zinject_counts.ksh" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~1 HEAD; grep -c atomic_inc_64 module/zfs/zio_inject.c
```

Expected TREE-OK, SHA-OK, the diffstat

```
 module/zfs/zio_inject.c | 47 ++++++++++++++++++++++++++++++-----------------
 1 file changed, 30 insertions(+), 17 deletions(-)
```

and `16`. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/counts.sh, the second to $L/stress.sh. Then:

```sh
L=/var/log/task-1120-zic
printf '%s\n' "c597397420d0f838b0ef13cb3c004378df8f95af4431493864e6554cb527e3e5  $L/counts.sh" "5369f6947272555685700c69f3a7af15d106060635f2198eedec4b36a1183d41  $L/stress.sh" | sha256sum -c && sh -n $L/counts.sh && sh -n $L/stress.sh && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`counts.sh <tag> <runs>` runs as the login user. It runs `-t zinject_counts` `<runs>` times, one at a time, each after the pre-run checks; each run's console goes to $L/zts-<tag>-<n>.log, and $L/counts-<tag>.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir> zinject_counts.ksh (run as root) [<duration>] [<result>]` followed by that run's `check_count_freq: freq=` lines (from `<result dir>/log`).

```sh
#!/bin/sh
# TASK-1120-ZIC zinject_counts loop, run as the login user: counts.sh <tag> <runs>
D=/var/tmp/task-1120-zic
L=/var/log/task-1120-zic
tag=$1
runs=$2
out=$L/counts-$tag.txt
cd $D/zd || exit 1
echo "START $tag $(date -u +%FT%TZ)" >> $out
n=1
while [ $n -le $runs ]; do
    pools=$(timeout 30 sudo -n $D/zd/zpool list -H -o name 2>&1)
    left=$(ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>/dev/null)
    if [ -n "$pools" ] || [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: pools=[$pools] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_counts \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc dir=$R $(grep -oE 'zinject_counts.* \[[A-Z]+\]' $L/zts-$tag-$n.log)" >> $out
    grep -h 'check_count_freq: freq=' "$R/log" | sed "s|^|$tag-$n |" >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

`stress.sh <tag> <iterations>` runs as root. Each iteration writes to $L/stress-<tag>.txt a line `<tag>-<i> match=<m> inject=<i> differ=<no|YES> f1=<state>/<write errors> dd_failed=<n> secs=<s>`, and the loop ends with `DONE <tag> iterations=<n> mismatches=<k> matches=<total> <time>` after removing its files. On anything unexpected it writes `STOP <tag>-<i>: <reason>`, the `zinject` listing and `zpool status -v`, and exits leaving the pool and the handler as they are.

```sh
#!/bin/sh
# TASK-1120-ZIC stress loop, run as root: stress.sh <tag> <iterations>
D=/var/tmp/task-1120-zic
L=/var/log/task-1120-zic
Z=$D/zd
P=zicstress
tag=$1
iters=$2
V=$D/vdev-$tag
M=$D/mnt-$tag
out=$L/stress-$tag.txt
i=0

# Records the state and leaves it as it is, for main to look at.
stop() {
    echo "STOP $tag-$i: $*" >> $out
    $Z/zinject >> $out 2>&1
    $Z/zpool status -v >> $out 2>&1
    exit 1
}

echo "START $tag $(date -u +%FT%TZ)" >> $out
[ -z "$($Z/zpool list -H -o name)" ] || stop "a pool exists before the loop"
mkdir -p $V $M || stop "mkdir"
truncate -s 1G $V/f1 $V/f2 $V/f3 || stop "truncate"
mism=0
total=0
i=1
while [ $i -le $iters ]; do
    t0=$(date +%s)
    $Z/zpool create -f -o cachefile=none -O mountpoint=$M $P \
        mirror $V/f1 $V/f2 $V/f3 || stop "zpool create"
    mountpoint -q $M || stop "$M is not mounted"
    $Z/zinject -d $V/f1 -e io -T write -f 100 -F $P > /dev/null ||
        stop "zinject -d"
    pids=
    for j in 1 2 3 4 5 6 7 8; do
        dd if=/dev/urandom of=$M/f$j bs=1M count=64 \
            2>> $L/stress-$tag-dd.txt &
        pids="$pids $!"
    done
    ddfail=0
    for p in $pids; do
        wait $p || ddfail=$((ddfail + 1))
    done
    $Z/zpool sync $P || stop "zpool sync"
    rows=$($Z/zinject | awk '$1 ~ /^[0-9]+$/ && NF >= 8 { print $(NF-1), $NF }')
    [ "$(echo "$rows" | grep -c .)" -eq 1 ] || stop "not one handler row: [$rows]"
    set -- $rows
    match=$1
    inject=$2
    f1=$($Z/zpool status $P | awk -v f=$V/f1 '$1 == f { print $2 "/" $4 }')
    $Z/zinject -c all > /dev/null || stop "zinject -c all"
    $Z/zpool destroy $P || stop "zpool destroy"
    differ=no
    if [ "$match" != "$inject" ]; then
        differ=YES
        mism=$((mism + 1))
    fi
    total=$((total + match))
    echo "$tag-$i match=$match inject=$inject differ=$differ f1=$f1 dd_failed=$ddfail secs=$(($(date +%s) - t0))" >> $out
    i=$((i + 1))
done
rm -rf $V $M
echo "DONE $tag iterations=$iters mismatches=$mism matches=$total $(date -u +%FT%TZ)" >> $out
```

## STEP 3: debug build of the branch (about 12 minutes)

```sh
D=/var/tmp/task-1120-zic; L=/var/log/task-1120-zic
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-zd.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zd/module/zfs.ko`: it must differ from master's `65D8C0C24A9B688639F0721`.

## STEP 4: the branch (tags b...)

`sudo -n $D/zd/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

1. zinject_counts, 30 runs (tag bc), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-zic
   setsid nohup sh $L/counts.sh bc 30 > $L/counts-bc.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/counts-bc.txt` until a `DONE bc` or a `STOP` line. Then:

   ```sh
   L=/var/log/task-1120-zic
   grep -c '\[PASS\]' $L/counts-bc.txt; grep -E '\[(FAIL|KILLED|SKIP)\]|STOP' $L/counts-bc.txt; cat $L/counts-bc.nohup
   grep -c 'freq=100 ' $L/counts-bc.txt
   grep 'freq=100 ' $L/counts-bc.txt | awk '{ for (k = 1; k <= NF; k++) { if ($k ~ /^match=/) m = substr($k, 7); if ($k ~ /^inject=/) j = substr($k, 8) } if (m != j) print }'
   ```

   Expected `30`, nothing, nothing, `90` (three freq=100 checks per passing run) and nothing (no freq=100 mismatch). Quote these outputs, the START and DONE lines, and for each of the three freq=100 checks (device, object, delay: the 1st, 2nd and 3rd freq=100 line of a run) the lowest and highest match. Every non-PASS run: quote its line and its `check_count_freq` lines, copy its `<result dir>/log` to $L/log-bc-<n>.txt, and quote the failing lines with the 20 before them. A freq=100 mismatch on the branch is the result that matters most: quote it in full and go on with the plan; it does not stop the task.
2. Stress loop, 50 iterations (tag bs), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-zic
   setsid nohup sudo -n sh $L/stress.sh bs 50 > $L/stress-bs.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 2 $L/stress-bs.txt` until a `DONE bs` or a `STOP` line. Then:

   ```sh
   D=/var/tmp/task-1120-zic; L=/var/log/task-1120-zic
   grep -E '^(START|DONE|STOP)' $L/stress-bs.txt; cat $L/stress-bs.nohup
   grep -E 'differ=YES|dd_failed=[1-9]' $L/stress-bs.txt
   grep -o 'f1=[A-Z]*' $L/stress-bs.txt | sort | uniq -c
   grep -o ' match=[0-9]*' $L/stress-bs.txt | cut -d= -f2 | sort -n | sed -n '1p;$p'
   grep -o 'secs=[0-9]*' $L/stress-bs.txt | cut -d= -f2 | sort -n | sed -n '1p;$p'
   timeout 30 sudo -n $D/zd/zpool list -H -o name; sudo -n $D/zd/zinject; ls -d $D/vdev-bs $D/mnt-bs 2>&1
   ```

   Expected: START, then `DONE bs iterations=50 mismatches=0 matches=<total>`; no `differ=YES` and no `dd_failed` above 0; f1 states (ONLINE expected without zed; FAULTED or DEGRADED recorded, never a stop); the lowest and highest match and seconds; no pool, `No handlers registered.`, both directories gone. Quote them all, then `sudo -n fstrim -av`. A `dd_failed` above 0: quote `tail -n 20 $L/stress-bs-dd.txt`. A STOP line: quote it and what follows it, leave the pool as it is, and stop and report.
3. `-T zinject` once (tag bz), after the pre-run checks:

   ```sh
   cd /var/tmp/task-1120-zic/zd
   setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zinject > /var/log/task-1120-zic/zts-bz-1.log 2>&1 < /dev/null &
   ```

   Expected all PASS: `zinject_args`, `zinject_counts`, `zinject_probe` (the directory has no setup or cleanup). Quote every `Test:` line with result and duration, the Results Summary, and `grep 'check_count_freq: freq=' $R/log` with `R=$(awk '/^Log directory/ { print $3 }' $L/zts-bz-1.log)`. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether tests/test-runner/bin/zts-report.py.in lists it as known.

After them: the pre-run checks once more (no pool, no handler, CACHE-OK, no file-vdev); every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 5: master's zio_inject.c (tags m...)

```sh
cd /var/tmp/task-1120-zic/zd && git checkout cde5d598eb44b5d6efe8167666d988a496346657 -- module/zfs/zio_inject.c && test "$(git write-tree)" = d70a147b0775bf4bd216072a8b05738b603d6849 && echo TREE-OK && sha256sum module/zfs/zio_inject.c && git diff --cached --name-status HEAD; grep -c atomic_inc_64 module/zfs/zio_inject.c
```

Expected TREE-OK, `5f0ac404a0a29a2fdf8cdafef7cfaa1f4ffcd6a40180defe9fe3d75821e1f850  module/zfs/zio_inject.c`, `M` and `module/zfs/zio_inject.c` separated by a tab, and `0`. Any difference: stop and report.

Rebuild in place, detached and polled like a build:

```sh
D=/var/tmp/task-1120-zic; L=/var/log/task-1120-zic
setsid nohup sh -c "cd $D/zd && nice make -j4; echo build-rc=\$?" > $L/build-zm.log 2>&1 < /dev/null &
```

It must end `build-rc=0`; quote `grep -E 'zio_inject|zfs\.ko|libzpool' $L/build-zm.log | head -n 20` and every `warning:` line outside objtool notes. Note S2=`modinfo -F srcversion $D/zd/module/zfs.ko`: it must differ from S1, and is expected to be `65D8C0C24A9B688639F0721` (another value is recorded and reported, not a stop). Load: `sudo -n $D/zd/scripts/zfs.sh`, srcversion equal to S2, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

1. zinject_counts, 30 runs (tag mc): as step 4.1 with `mc` for `bc`. Here a FAIL with a freq=100 mismatch is what the comparison looks for: quote each such run's line and its `check_count_freq` lines, copy its `<result dir>/log` to $L/log-mc-<n>.txt, and quote its failing lines with the 20 before them. Any other non-PASS run is quoted as in 4.1.
2. Stress loop, 50 iterations (tag ms): as step 4.2 with `ms` for `bs`. Quote every `differ=YES` line.

After them: the pre-run checks once more; the dmesg check since N1 as in step 4 (none expected). Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 6: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line of step 0; TREE-OK, SHA-OK, diffstat and the atomic_inc_64 count; SCRIPTS-OK; build rc and warnings of both builds; S1, S2 (against `65D8C0C24A9B688639F0721`) and DEBUG mode; step 5's TREE-OK, sha and name-status; what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. Table: tree | part | trials | result | mismatches (freq=100 for zinject_counts, MATCH not equal to INJECT for the loop) | matches counted (lowest-highest per check, or the loop's total) | key note. Rows: branch zinject_counts (30), branch stress (50), branch -T zinject (1), master zinject_counts (30), master stress (50). Then one verdict line, e.g. "zinject-atomic-counts e29fc6e82a5: zinject_counts PASS 30/30, 0 of 90 freq=100 checks unequal; stress 0/50 unequal (N matches); -T zinject PASS 3. Master cde5d598eb4: zinject_counts K/30 runs with a freq=100 mismatch (e.g. match=98 inject=97), stress M/50 unequal (N matches): comparison informative", or, if master showed none, "... master 0 mismatches in 30 + 50 trials (N matches counted): comparison uninformative".
3. Quotes: every mismatch line of either tree, every non-PASS run, the START/DONE lines, f1 states, the dmesg checks.
4. Deviations, cleanup, then End.
