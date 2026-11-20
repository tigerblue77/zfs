TASK-1120-ASP (from the coordinator session "main"): functional, about 1 h, debug build only (one tree, one build: the two variants differ only in a test script, swapped between runs), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-asp`, `L=/var/log/task-1120-asp`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

`fault/auto_spare_002_pos` fails on these VMs in its `anymirror2` case, at `ERROR: wait_vdev_state testpool /var/tmp/file-1 DEGRADED 60 exited 1`, although `zinject` shows over a thousand corrupted reads of file-1: VM 106 on 15/11 (TASK-1115-AI, master c913da86156: 3 FAIL of 3), VM 105 on 18/11 (TASK-1117-ZTB: 2 of 2, one of them master c913da86156). The fork's CI passes the same case in about 600 Linux runs.

The ZED degrades file-1 after `checksum_n` (10) checksum ereports on it. `zio_checksum_verify()` (module/zfs/zio.c) counts a checksum error and posts its ereport only when the read is not `ZIO_FLAG_SPECULATIVE`, and prefetch reads are speculative. The test reads its file with one sequential `dd`, so after the first megabytes the prefetcher reads ahead and most corrupted reads of file-1 count nothing. Whether the few demand reads reach file-1 depends on the layout: in `anymirror2`, each 64 MiB tile is on three of the five files. The column CKSUM of `zpool status -p` for file-1 is that count.

The candidate fix creates the test's file system with `prefetch=none`, as `events/zed_slow_io.ksh` does, so that every read of file-1 is a demand read. It does not touch the global `zfs_prefetch_disable`, which `ab.sh` checks is still 0 after every run.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-asp`, head `8922962d4909ac3fd0c5f29a67c563f4a3b58664`, tree `ceccb86126eb638d08960630f8d358f6a18c5202`: a transport commit (same tree) on the fix `1037e1f4ec353e0d63ddc0ce40e21202717c1fc3` "ZTS: auto_spare_002_pos: disable prefetch on the test file system", on upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` (20/11). The fix changes tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh only, so one build serves both variants:

- `m`: the test file as on master (`git show HEAD~2:<file>`, saved as $D/asp-m.ksh);
- `f`: the test file of the fix (`git show HEAD:<file>`, saved as $D/asp-f.ksh).

`ab.sh` (step 2) runs `-t auto_spare_002_pos` 8 times with each, alternating m and f, and copies the variant into the tree before each run. Beside each run, `sampler.sh` (root) writes every 0.5 s the top-level vdev name, file-1's state and CKSUM, and spare-1's state, and keeps a copy of the ZED log (/var/tmp/zed/zed.log, which the group cleanup deletes).

Expected: `m` FAILs in some runs, always in `anymirror2`, with file-1's CKSUM below 10 in `anymirror2-0` while MATCH is above 1000; `f` PASSes 8 of 8, with file-1's CKSUM of 10 or more in `anymirror2-0` (a case that ends within a second may fall between two samples and leave none). `m` passing 8 of 8 makes the comparison uninformative, not a failure of the fix: report it so. A FAIL of `m` with file-1's CKSUM of 10 or more in the failing case, or any FAIL of `f`, is the result that matters most: it means the explanation is wrong or incomplete.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs (/var/tmp/file-1 to file-5, /var/tmp/spare-1); touch no block device of the VM; never `zpool import` anything (`-a` least of all); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Before the loop: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `sudo -n $D/zt/zinject` prints `No handlers registered.`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` shows nothing, `pgrep -a -x zed; pgrep -a -x lt-zed` shows nothing (the test's setup starts its own zed and fails if one runs), `cat /sys/module/zfs/parameters/zfs_prefetch_disable` prints 0, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. `ab.sh` repeats these checks (all but lsmod and zinject) before each run and writes a `STOP` line if one fails.
- The loop runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; the loop with $L/ab.txt unchanged for 10 minutes or still going after 45 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zed|ksh|dd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /etc/zfs/vdev_id.conf 2>&1` (none), `id zts`, and `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-asp
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-asp; L=/var/log/task-1120-asp
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs vm-kit-asp && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "8922962d4909ac3fd0c5f29a67c563f4a3b58664 1037e1f4ec353e0d63ddc0ce40e21202717c1fc3 1b1aa0e91b694798085a2771c4eef40e93b9b2db ceccb86126eb638d08960630f8d358f6a18c5202 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
git show HEAD~2:$T > $D/asp-m.ksh && git show HEAD:$T > $D/asp-f.ksh
printf '%s\n' "37f26699f76d10dbb33adcb4e7a41d9be0d63044e987f6fd652d6fc14c6d0d55  $D/asp-m.ksh" "08ae9946040f4ad4e6a863100e3d36a53d163bc8b348f65b4161ab7977cd3a66  $D/asp-f.ksh" "08ae9946040f4ad4e6a863100e3d36a53d163bc8b348f65b4161ab7977cd3a66  $T" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~2 HEAD; ls -l $T; grep -c prefetch=none $D/asp-m.ksh $D/asp-f.ksh
```

Expected TREE-OK, SHA-OK, the diffstat

```
 tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh | 10 +++++++---
 1 file changed, 7 insertions(+), 3 deletions(-)
```

the test file with mode `-rwxr-xr-x`, and `$D/asp-m.ksh:0`, `$D/asp-f.ksh:1`. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree: `ab.sh` alone swaps the test file, with `cp`, which keeps its mode.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/sampler.sh, the second to $L/ab.sh. Then:

```sh
L=/var/log/task-1120-asp
printf '%s\n' "3695a6db8441ead13827f03b3182e6db67e51a2e5ba6bc4b26a9fc04a067b126  $L/sampler.sh" "7c0572866bdb267294667ef9237ddd5d1edb593d678bbe02ebb08f983c7d98b4  $L/ab.sh" | sha256sum -c && sh -n $L/sampler.sh && sh -n $L/ab.sh && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`sampler.sh <tag>` runs as root until $L/stop-<tag> exists. Every 0.5 s it appends to $L/status-<tag>.txt a line `<time> top=<top-level vdev> file1=<state> cksum=<CKSUM> spare1=<state>` while `testpool` exists, and copies /var/tmp/zed/zed.log to $L/zed-<tag>.log.

```sh
#!/bin/sh
# TASK-1120-ASP sampler, run as root: sampler.sh <tag>
D=/var/tmp/task-1120-asp
L=/var/log/task-1120-asp
tag=$1
out=$L/status-$tag.txt
while [ ! -e $L/stop-$tag ]; do
    s=$($D/zt/zpool status -p testpool 2>/dev/null | awk '
        $1 ~ /^(mirror|raidz|anymirror)[0-9]*-[0-9]+$/ { top = $1 }
        $1 == "/var/tmp/file-1" { st = $2; ck = $5 }
        $1 == "/var/tmp/spare-1" { sp = $2 }
        END { if (top != "" && st != "")
            printf "top=%s file1=%s cksum=%s spare1=%s", top, st, ck, sp }')
    [ -n "$s" ] && echo "$(date -u +%T.%N | cut -c1-12) $s" >> $out
    [ -f /var/tmp/zed/zed.log ] && cp /var/tmp/zed/zed.log $L/zed-$tag.log
    sleep 0.5
done
rm -f $L/stop-$tag
```

`ab.sh <pairs>` runs as the login user. For n from 1 to `<pairs>` it runs m<n> then f<n>: the pre-run checks, the variant copied into the tree, /proc/spl/kstat/zfs/fm saved before and after, the sampler started beside `zfs-tests.sh -vKO -t auto_spare_002_pos` (console in $L/zts-<tag>.log) and stopped after it. $L/ab.txt gets one line per run: `<tag> rc=<rc> sha=<first 12 hex of the test file> pf=<zfs_prefetch_disable after the run> <ZTS result line> | <type>:<MATCH>:<deg|NODEG> for each case | cksum_max: <top-level>=<highest CKSUM of file-1> ... | stdout=<the test's stdout file>`.

```sh
#!/bin/sh
# TASK-1120-ASP A/B loop, run as the login user: ab.sh <pairs>
D=/var/tmp/task-1120-asp
L=/var/log/task-1120-asp
T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
P=/sys/module/zfs/parameters/zfs_prefetch_disable
pairs=$1
out=$L/ab.txt
cd $D/zt || exit 1
echo "START $(date -u +%FT%TZ)" >> $out
n=1
while [ $n -le $pairs ]; do
    for v in m f; do
        tag=$v$n
        pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
        left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed \
            /etc/zfs/vdev_id.conf 2>/dev/null)
        zeds=$(pgrep -x zed; pgrep -x lt-zed)
        if [ -n "$pools" ] || [ -n "$left" ] || [ -n "$zeds" ] ||
            [ -s /etc/zfs/zpool.cache ] || [ "$(cat $P)" != 0 ]; then
            echo "STOP before $tag: pools=[$pools] left=[$left] zed=[$zeds] prefetch_disable=[$(cat $P)] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
            exit 1
        fi
        cp $D/asp-$v.ksh $T || exit 1
        cp /proc/spl/kstat/zfs/fm $L/fm-$tag-before.txt
        sudo -n sh $L/sampler.sh $tag > $L/sampler-$tag.nohup 2>&1 &
        sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t auto_spare_002_pos \
            > $L/zts-$tag.log 2>&1 < /dev/null
        rc=$?
        touch $L/stop-$tag
        wait
        cp /proc/spl/kstat/zfs/fm $L/fm-$tag-after.txt
        R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag.log)
        f=$(sudo -n find "$R/output" -type f -path '*auto_spare_002_pos*' -name stdout | head -n 1)
        cases=$(sudo -n cat "$f" | awk '
            /zpool create -f testpool/ { t = $6 }
            $2 == "testpool" && / corrupt / { m = $(NF-1) }
            /wait_vdev_state testpool \/var\/tmp\/file-1 DEGRADED/ {
                printf "%s:%s:%s ", t, m, ($1 == "SUCCESS:") ? "deg" : "NODEG" }')
        ck=$(awk '{ for (k = 2; k <= NF; k++) { split($k, a, "="); v[a[1]] = a[2] }
            c = v["cksum"] + 0; if (!(v["top"] in mx) || c > mx[v["top"]]) mx[v["top"]] = c }
            END { for (t in mx) printf "%s=%d\n", t, mx[t] }' $L/status-$tag.txt 2>/dev/null |
            sort | tr '\n' ' ')
        echo "$tag rc=$rc sha=$(sha256sum $T | cut -c1-12) pf=$(cat $P) $(grep -oE 'auto_spare_002_pos.* \[[A-Z]+\]' $L/zts-$tag.log | tail -n 1) | $cases| cksum_max: $ck| stdout=$f" >> $out
    done
    n=$((n + 1))
done
echo "DONE $(date -u +%FT%TZ)" >> $out
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-asp; L=/var/log/task-1120-asp
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko` (upstream master moved since c913da86156: S1 is recorded, not compared).

## STEP 4: the A/B loop

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks of the VM rules, then:

```sh
L=/var/log/task-1120-asp
setsid nohup sh $L/ab.sh 8 > $L/ab.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/ab.txt` until a `DONE` or a `STOP` line (a run takes about 30 s when it passes and 90 s when it fails). Then:

```sh
L=/var/log/task-1120-asp
grep -E '^(START|DONE|STOP)' $L/ab.txt; cat $L/ab.nohup
grep -E '^[mf][0-9]+ ' $L/ab.txt | cut -d'|' -f1-3
for v in m f; do echo "$v: $(grep -E "^$v[0-9]+ " $L/ab.txt | grep -c '\[PASS\]') PASS, $(grep -E "^$v[0-9]+ " $L/ab.txt | grep -cE '\[(FAIL|KILLED)\]') FAIL"; done
grep -E '^[mf][0-9]+ ' $L/ab.txt | awk '{ print $1, $3, $4 }' | sort -k2 | uniq -c -f1
for f in $L/fm-*-after.txt; do t=${f#$L/fm-}; t=${t%-after.txt}; echo "$t $(awk 'NR == FNR { b[$1] = $3; next } $1 ~ /^erpt-(dropped|duplicates)$/ { printf "%s+%d ", $1, $3 - b[$1] }' $L/fm-$t-before.txt $f)"; done
```

Expected: START, DONE, no STOP, an empty ab.nohup; 16 lines, each with its six cases (mirror, raidz, raidz2, anymirror1, anymirror2, anymirror3) up to the one that fails; `f: 8 PASS, 0 FAIL`; the `m` lines with sha `37f26699f76d` and the `f` lines with `08ae9946040f`, all with `pf=0`. Quote all of it.

For every run with `NODEG`, quote:

```sh
L=/var/log/task-1120-asp; t=<tag>; top=<the top-level of the failing case, e.g. anymirror2-0>
grep "top=$top " $L/status-$t.txt | sed -n '1p;$p'; grep -c "top=$top " $L/status-$t.txt
grep -nE 'Diagnosis Engine|Retire Agent' $L/zed-$t.log | tail -n 40
```

and, from the run's stdout file (the `stdout=` field), the failing line with the 25 lines before it (`sudo -n` to read it). The prediction to check, run by run: in each `m` FAIL, file-1's highest CKSUM in the failing top-level is below 10, file-1 stays ONLINE, and the ZED log has no `solving fault` for it. A FAIL that contradicts it (CKSUM of 10 or more; a `skip faulting` line; file-1 FAULTED or another state) and any `f` FAIL: quote the run's whole `status-<tag>.txt` and the full `grep -nE 'Diagnosis Engine|Retire Agent|checksum' $L/zed-<tag>.log`, and go on with the plan; it does not stop the task.

For the `f` runs, quote `grep -h "top=anymirror2-0 " $L/status-f*.txt | awk '{ print $4 }' | sort -t= -k2 -n | sed -n '1p;$p'` (the lowest and highest CKSUM seen in anymirror2 with the fix).

After the loop: `cd /var/tmp/task-1120-asp/zt && git status --porcelain -- tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh | wc -l` (0: the last run left the fix's file, which is HEAD's), the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault` quoted with 10 lines before it (none expected; a zed segfault would explain a FAIL on its own and must be reported). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`.

## STEP 5: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line of step 0; TREE-OK, SHA-OK, diffstat, the prefetch=none counts; SCRIPTS-OK; build rc and warnings; S1 and DEBUG mode; what happened to /etc/zfs/zpool.cache at steps 0 and 5.
2. Table: tag | variant | result | duration | per case `type:MATCH:deg|NODEG` | file-1's highest CKSUM per top-level. Then one verdict line, e.g. "master test: K/8 FAIL, all in anymirror2 with file-1 CKSUM at most C (MATCH M1-M2), no ZED fault; fix 1037e1f4ec3: 8/8 PASS, anymirror2 file-1 CKSUM C1-C2: comparison informative, explanation holds", or "master 0/8 FAIL: comparison uninformative", or what contradicts the explanation.
3. Quotes: the START/DONE lines, every NODEG run's status, ZED and stdout lines, the erpt deltas, the dmesg check.
4. Deviations, cleanup, then End.
