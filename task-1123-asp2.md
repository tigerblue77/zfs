TASK-1123-ASP2 (from the coordinator session "main"): functional, about 1 h 15 with its build, at most 2 h, no reboot expected and none pre-approved. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12.118 with 6.12.117 kept installed, 8 vCPU, about 7.9 GiB RAM), not next to a performance run. It checks the second commit of our PR 19989 (`fault/auto_spare_002_pos` with prefetch off), which takes a reviewer's two suggestions: the test now checks file-1's checksum error count before it waits for the ZED, and it leaves the Linux "maybe" list. One tree, one debug build. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-asp2/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1123-asp2`, `L=/var/log/task-1123-asp2`, `T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh` (relative to `$D/zt`).

Never run anything outside the steps of this task. If a result surprises you, stop and report it with the outputs.

## Why

- **TASK-1120-ASP (VM 105, 20/11):** master's `auto_spare_002_pos` failed 5 of 8 runs. Each time file-1 stayed ONLINE with its CKSUM below 10 (0 in four `anymirror2` cases, 4 in one `anymirror3` case), so the ZED never degraded it and the test failed 60 s later at `ERROR: wait_vdev_state testpool /var/tmp/file-1 DEGRADED 60 exited 1`. Prefetch reads are speculative and count no checksum error. The fix creates the test file system with `prefetch=none`, so that every read of file-1 is a demand read that counts; it passed 8 of 8. That fix is the head of PR 19989, `53f874e2631`, with the same tree as TASK-1120-ASP's `1037e1f4ec3`.
- **The new commit `be61dbbc0a7`**, on top of it:
  - right after the `zinject` listing, the test reads file-1's `checksum_errors` with `get_vdev_prop`, that is `zpool get -Hpo value checksum_errors testpool /var/tmp/file-1`. It logs `NOTE: <layout>: <n> checksum errors on /var/tmp/file-1` in every layout, and fails at once with `<layout>: <n> checksum errors on /var/tmp/file-1, ZED needs 10` when n is below 10, the ZED's `checksum_n`;
  - `fault/auto_spare_002_pos` leaves the Linux maybe list of `tests/test-runner/bin/zts-report.py.in`, so that a failure shows as unexpected.
- **What this task answers:**
  1. the test passes 5 of 5, and with what margin: the count of each layout in each run (step 4);
  2. `zpool get` reads file-1 in every layout, including once the ZED has put it under `spare-0` beside spare-1. With prefetch off, the ZED may attach the spare during the `dd`, before the test reads the count. A sampler beside each run records the pool's shape and the same `zpool get` every 0.2 s, and places each NOTE among the samples (step 4);
  3. without `prefetch=none`, the check turns master's 60 s timeout into an immediate failure with the new message (step 5). Master failed about 5 of 8 on VM 105, so some runs may pass; that is expected;
  4. the `fault` group with this commit gives the same results as on master on these VMs, except that `auto_spare_002_pos` passes (step 6).
- **The result that matters most** is a count below 10 in a run of the unmodified test (step 4). Either the check would then fail a test that otherwise passes, or `prefetch=none` does not give enough demand reads. TASK-1120-ASP's sampler (every 0.5 s, `zpool status -p` only) saw raidz1's file-1 CKSUM stay 0 in 3 of its 8 runs with the fix, although the case degraded. That is most likely a sampling gap, since each case lasts a few seconds. This task settles it, because the test itself now reads the count.

## What is tested (fork https://github.com/tigerblue77/zfs, branch `auto-spare-002-prefetch-v2`)

| commit | tree | what |
|---|---|---|
| HEAD `be61dbbc0a7e03c086306f5c93ba73876e1ab2ce` | `d1dbabf23daac17b5ffa52a02d77423d59cc8b60` | "ZTS: auto_spare_002_pos: check the errors, expect it to pass" (the count check; the maybe entry removed) |
| HEAD~1 `53f874e2631eea777fb31e5ad576ba379bf3741c` | `ceccb86126eb638d08960630f8d358f6a18c5202` | "ZTS: auto_spare_002_pos: disable prefetch on the test file system" (the head of PR 19989) |
| HEAD~2 `1b1aa0e91b694798085a2771c4eef40e93b9b2db` | | upstream master of 20/11 |

The test file `$T` has mode 100755:

| version | used in | sha256 |
|---|---|---|
| HEAD | steps 4 and 6 | `7642d2cd36c20d0ede1d18360d40a3f73612c6e2b2d4964e1820c2b0fc908ad7` |
| HEAD without ` -o prefetch=none` (made by `sed` in step 5) | step 5 | `8bde11c6720e5a216af69520b901e21be96676cc0e740fc662a30050f476542f` |
| HEAD~1 (TASK-1120-ASP's `asp-f.ksh`) | none: a check of step 1 | `08ae9946040f4ad4e6a863100e3d36a53d163bc8b348f65b4161ab7977cd3a66` |

One debug build serves every step. `zfs-tests.sh` run in the tree reads its tests from `$D/zt/tests/zfs-tests`, and `-t auto_spare_002_pos` resolves to the first file that `find` names `auto_spare_002_pos*` there. So step 5 edits `$T` in place with `sed` and restores it with `git checkout`. A copy under another name in the same directory could be the one `find` picks.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs (`/var/tmp/file-1` to `file-5`, `/var/tmp/spare-1`), its own loop devices on `/var/tmp/file-vdev0` to `2`, and, in the `fault` group, its own scsi_debug disk. Touch no other block device of the VM. Never `zpool import` anything, never `zfs-tests.sh -x`, never pass `-d` or set `DISKS`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- **Before every ZTS run.** `runs.sh` repeats the first five of these checks before each of its runs and writes a `STOP` line if one fails:
  - `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool;
  - `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /var/tmp/testdir /etc/zfs/vdev_id.conf 2>&1` shows none;
  - `pgrep -a -x zed; pgrep -a -x lt-zed` shows nothing (the group's setup starts its own zed and fails if one runs);
  - `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK;
  - `cat /sys/module/zfs/parameters/zfs_prefetch_disable` prints 0 (the fix sets the property on its file system and must not touch the global tunable);
  - `lsmod | grep -E '^zfs '` shows this task's module, and `sudo -n $D/zt/zinject` prints `No handlers registered.`
- **Builds and runs are detached** (`setsid nohup ... &`) and polled. A hang is any of these:
  - the build still going after 60 minutes;
  - `$L/runs.txt` unchanged for 10 minutes while `runs.sh` runs;
  - the `fault` group still going after 90 minutes.

  Then capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etimes,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zed|ksh|dd'`, `timeout 5 sudo -n cat /proc/<pid>/stack` of every D task, and `sudo -n dmesg | tail -n 150`. Then stop and report.
- **Reboots:** this task pre-approves none. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- **Kernel log.** Steps 4, 5 and 6 each start with a mark, `echo "task-1123-asp2 mark <step>" | sudo -n tee /dev/kmsg > /dev/null`, and end with a check of the kernel lines since that mark. ZTS clears the ring buffer, so the check reads the journal:

  ```sh
  L=/var/log/task-1123-asp2; k=<step>
  sudo -n journalctl -k -o short-precise --no-pager | sed -n "/task-1123-asp2 mark $k\$/,\$p" > $L/kern-$k.txt
  wc -l < $L/kern-$k.txt; grep -nE 'VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock|segfault' $L/kern-$k.txt; grep -c 'has been suspended' $L/kern-$k.txt
  ```

  The first number must be above 0, which shows that the mark was found.
  - Steps 4 and 5: no match is expected, and no suspend line.
  - Step 6: the group's `suspend_*` tests write suspend lines; count them. When `auto_offline_001_pos` is KILLED, the runner's `-O` writes a `##### ZTS timeout debug #####` block and a sysrq backtrace, whose `Call Trace` lines are expected: name the block, and quote any match outside it with the 10 lines before it.
- `sudo -n fstrim -av` after deleting the tree. The module is unloaded at the end.

## STEP 0: state

Check and quote:
- `hostname`, `date -u`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `cat /etc/debian_version`. The kernel is 6.12.118 or 6.12.117 (still installed); say which. Any other kernel is a stop;
- `df -h / /var/tmp`: at least 12 GiB free on /var/tmp, else stop and report;
- `ls -dL /lib/modules/$(uname -r)/build`: it must exist, since the build needs the running kernel's headers (missing headers cost a voided run on 23/11). Else stop and report;
- `lsmod | grep -E '^(zfs|spl|scsi_debug) '`: must be empty;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git lsscsi`, `modinfo -F filename scsi_debug`. The `fault` group's scsi_debug tests need the last two; a missing one is not a stop: say so;
- `ls /lib/udev/rules.d /etc/udev/rules.d 2>/dev/null | grep -E '^(60-zvol|69-vdev|90-zfs)\.rules$'`: quote it. 60-zvol has been installed since 18/11. The night restores of 21 and 22/11 installed 69-vdev and 90-zfs on the VMs they ran on; without them `auto_replace_*` cannot pass. Step 6's expectations depend on this line;
- `pgrep -a -x zed; pgrep -a -x lt-zed`: must be empty. A running zed is a stop; do not start or stop zed;
- `ls -d /var/tmp/task-1123-asp2 /var/log/task-1123-asp2 2>&1`: both absent;
- `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /var/tmp/testdir /var/tmp/dev-* /var/tmp/tunable-* /etc/zfs/vdev_id.conf 2>&1`: none;
- `losetup -a`: quote it. No loop device may be backed by a file under /var/tmp;
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected.
  - Size 0: move it aside once `$L` exists (step 1) with `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0`, and say so.
  - Size above 0: stop and report its `ls -l`. Do not read it with zdb, and do not load the module.

Trees and logs that other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1123-asp2; L=/var/log/task-1123-asp2; T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs auto-spare-002-prefetch-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git rev-parse HEAD~1^{tree}) $(git status --porcelain | wc -l)" | tee $L/tree.txt
git log -2 --format='%an | %s'; git diff --stat=100 HEAD~1 HEAD
echo "$(git ls-tree HEAD $T | cut -c1-6) $(ls -l $T | cut -d' ' -f1) $(sha256sum < $T | cut -c1-64) $(git show HEAD~1:$T | sha256sum | cut -c1-64)"
grep -c -e ' -o prefetch=none' $T; grep -n -e 'get_vdev_prop checksum_errors' -e 'ZED needs 10' $T; grep -c "'fault/auto_spare_002_pos'" tests/test-runner/bin/zts-report.py.in
```

Expected (the two lines `90:` and `93:` hold a tab after the colon):

```
be61dbbc0a7e03c086306f5c93ba73876e1ab2ce 53f874e2631eea777fb31e5ad576ba379bf3741c 1b1aa0e91b694798085a2771c4eef40e93b9b2db d1dbabf23daac17b5ffa52a02d77423d59cc8b60 ceccb86126eb638d08960630f8d358f6a18c5202 0
Tigerblue77 | ZTS: auto_spare_002_pos: check the errors, expect it to pass
Tigerblue77 | ZTS: auto_spare_002_pos: disable prefetch on the test file system
 tests/test-runner/bin/zts-report.py.in                        | 1 -
 tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh | 8 ++++++++
 2 files changed, 8 insertions(+), 1 deletion(-)
100755 -rwxr-xr-x 7642d2cd36c20d0ede1d18360d40a3f73612c6e2b2d4964e1820c2b0fc908ad7 08ae9946040f4ad4e6a863100e3d36a53d163bc8b348f65b4161ab7977cd3a66
1
90:	cksum=$(get_vdev_prop checksum_errors "$TESTPOOL" "$FAULT_FILE")
93:	    log_fail "$type: $cksum checksum errors on $FAULT_FILE, ZED needs 10"
0
```

If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report the outputs as they are. Edit no file of the tree, except `$T` in step 5.

## STEP 2: the three files

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to `$L/sampler.sh`, the second to `$L/layouts.awk`, the third to `$L/runs.sh`. Then:

```sh
L=/var/log/task-1123-asp2
printf '%s\n' "aee7093902d86a69a4154e4f816254c7a0bedd5d58b9c56fe9806013d3d7832d  $L/sampler.sh" "0d07c3e83ca136d59f96c259205921e30bd45a9df0ece1e91bf6c822516f2be4  $L/layouts.awk" "77c157d313e17f6ddb75f8c25272288b875f4ed1c3b97e559f4ab8d7247946a7  $L/runs.sh" | sha256sum -c && wc -l $L/sampler.sh $L/layouts.awk $L/runs.sh && sh -n $L/sampler.sh && sh -n $L/runs.sh && awk -f $L/layouts.awk /dev/null && echo FILES-OK
```

Expected: three `OK` lines, the line counts 25, 48 and 54, then FILES-OK. If a sum differs, write that file again from this text; an HTML entity left in it is the usual cause. A second mismatch: stop and report.

**`sampler.sh <tag>`** runs as root until `$L/stop-<tag>` exists. Every 0.2 s while `testpool` exists, it appends to `$L/status-<tag>.txt` one line with these fields:
- the local time;
- `top=`: the top-level vdev;
- `par=`: file-1's parent, which is the top-level, or `spare-N` once the ZED has attached spare-1;
- `f1=` and `ck=`: file-1's state and its CKSUM in `zpool status -p`;
- `sp=`: spare-1's state in the spares list;
- `get=` and `grc=`: what `zpool get -Hpo value checksum_errors testpool /var/tmp/file-1` printed, and its exit code.

It also copies `/var/tmp/zed/zed.log`, which the group's cleanup deletes, to `$L/zed-<tag>.log`. It uses local time because the runner's timestamped log (`$R/log`) does.

**`layouts.awk`** reads a run's status file and its `$R/log`. For each `NOTE: <layout>: <n> checksum errors on /var/tmp/file-1` of the test, it prints one line with these fields:
- `note=` and `at=`: the count and the time of the note;
- `last-sample=` with its `par=`, `f1=`, `sp=` and `get=`: that layout's last sample taken before the note. `par=spare-0` there means the ZED had attached the spare before the test read the count;
- after the `|`, for that layout:
  - `samples=`: all its samples;
  - `spare=`: those with file-1 under `spare-N`;
  - `get-ok=`: those where `zpool get` printed a number and returned 0;
  - `get-ok-spare=`: the same, with file-1 under `spare-N`;
  - `get-eq-ck=`: those where that number equals the CKSUM column;
  - `get-bad=`: those where `zpool get` failed;
  - `ck-max=` and `get-max=`: the highest of each.

**`runs.sh <prefix> <first> <last> <sha256>`** runs as the login user, from `$D/zt`. For each n from first to last, it runs `<prefix><n>`:
- the pre-run checks, plus the test file's sha256, which must equal `<sha256>`; a failure writes `STOP`;
- the sampler, beside `zfs-tests.sh -vKO -t auto_spare_002_pos` (console in `$L/zts-<tag>.log`);
- copies of the test's stdout to `$L/stdout-<tag>.txt` and of `$R/log` to `$L/log-<tag>.txt`.

`$L/runs.txt` gets one line per run: `<tag> rc=<rc> sha=<first 12 hex of the test file> pf=<zfs_prefetch_disable after the run> <ZTS result line> | <layout>:<MATCH>:<count>:<deg|NODEG|CHECKFAIL> for each layout reached | <results dir>`. `deg` means the 60 s wait for file-1 DEGRADED succeeded, `NODEG` that it failed, and `CHECKFAIL` that the new check failed before that wait. `$L/layouts.txt` gets the run's `layouts.awk` lines.

```sh
#!/bin/sh
# TASK-1123-ASP2 sampler, run as root: sampler.sh <tag>
D=/var/tmp/task-1123-asp2
L=/var/log/task-1123-asp2
tag=$1
out=$L/status-$tag.txt
while [ ! -e $L/stop-$tag ]; do
    t=$(date +%T.%3N)
    s=$($D/zt/zpool status -p testpool 2>/dev/null | awk '
        $1 ~ /^(mirror|raidz|anymirror)[0-9]*-[0-9]+$/ { top = $1; par = $1 }
        $1 ~ /^spare-[0-9]+$/ { par = $1 }
        $1 == "/var/tmp/file-1" { st = $2; ck = $5; fp = par }
        $1 == "/var/tmp/spare-1" { sp = $2 }
        END { if (top != "" && st != "")
            printf "top=%s par=%s f1=%s ck=%s sp=%s", top, fp, st, ck, sp }')
    if [ -n "$s" ]; then
        g=$($D/zt/zpool get -Hpo value checksum_errors testpool /var/tmp/file-1 2>&1)
        r=$?
        g=$(printf '%s' "$g" | tr -c '0-9A-Za-z.-' '_' | cut -c1-40)
        echo "$t $s get=$g grc=$r" >> $out
    fi
    [ -f /var/tmp/zed/zed.log ] && cp /var/tmp/zed/zed.log $L/zed-$tag.log
    sleep 0.2
done
rm -f $L/stop-$tag
```

```awk
# TASK-1123-ASP2: awk -v tag=<tag> -f layouts.awk status-<tag>.txt log-<tag>.txt
FILENAME ~ /\/status-/ {
    top = ""; par = ""; f1 = ""; ck = ""; sp = ""; get = ""; grc = ""
    for (k = 2; k <= NF; k++) {
        e = index($k, "=")
        key = substr($k, 1, e - 1)
        val = substr($k, e + 1)
        if (key == "top") top = val
        else if (key == "par") par = val
        else if (key == "f1") f1 = val
        else if (key == "ck") ck = val
        else if (key == "sp") sp = val
        else if (key == "get") get = val
        else if (key == "grc") grc = val
    }
    ns++
    tm[ns] = substr($1, 1, 11); tp[ns] = top; pa[ns] = par
    st[ns] = f1; ss[ns] = sp; gv[ns] = get
    n[top]++
    if (par ~ /^spare-/) nsp[top]++
    if (grc == "0" && get ~ /^[0-9]+$/) {
        ok[top]++
        if (par ~ /^spare-/) oksp[top]++
        if (get == ck) eq[top]++
        if (get + 0 > gmax[top] + 0) gmax[top] = get + 0
    } else
        bad[top]++
    if (ck + 0 > cmax[top] + 0) cmax[top] = ck + 0
    next
}
$2 == "NOTE:" && $5 == "checksum" && $8 == "/var/tmp/file-1" {
    ty = $3
    sub(/:$/, "", ty)
    t = (ty == "raidz" ? "raidz1" : ty) "-0"
    j = 0
    for (i = 1; i <= ns; i++)
        if (tp[i] == t && tm[i] <= $1)
            j = i
    printf "%s %s note=%s at=%s", tag, ty, $4, $1
    if (j)
        printf " last-sample=%s par=%s f1=%s sp=%s get=%s", tm[j], pa[j],
            st[j], ss[j], gv[j]
    else
        printf " last-sample=none"
    printf " | samples=%d spare=%d get-ok=%d get-ok-spare=%d get-eq-ck=%d",
        n[t], nsp[t], ok[t], oksp[t], eq[t]
    printf " get-bad=%d ck-max=%d get-max=%d\n", bad[t], cmax[t], gmax[t]
}
```

```sh
#!/bin/sh
# TASK-1123-ASP2 runs, as the login user: runs.sh <prefix> <first> <last> <sha256>
D=/var/tmp/task-1123-asp2
L=/var/log/task-1123-asp2
T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
P=/sys/module/zfs/parameters/zfs_prefetch_disable
pre=$1
n=$2
last=$3
want=$4
out=$L/runs.txt
cd $D/zt || exit 1
echo "START $pre$n-$pre$last $(date -u +%FT%TZ)" >> $out
while [ $n -le $last ]; do
    tag=$pre$n
    pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
    left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed \
        /var/tmp/testdir /etc/zfs/vdev_id.conf 2>/dev/null)
    zeds=$(pgrep -x zed; pgrep -x lt-zed)
    sha=$(sha256sum < $T | cut -c1-64)
    if [ -n "$pools" ] || [ -n "$left" ] || [ -n "$zeds" ] ||
        [ -s /etc/zfs/zpool.cache ] || [ "$(cat $P)" != 0 ] ||
        [ "$sha" != "$want" ]; then
        echo "STOP before $tag: pools=[$pools] left=[$left] zed=[$zeds] prefetch_disable=[$(cat $P)] sha=[$sha] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n sh $L/sampler.sh $tag > $L/sampler-$tag.nohup 2>&1 &
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t auto_spare_002_pos \
        > $L/zts-$tag.log 2>&1 < /dev/null
    rc=$?
    touch $L/stop-$tag
    wait
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag.log)
    f=$(sudo -n find "$R/output" -type f -path '*auto_spare_002_pos*' \
        -name stdout | head -n 1)
    sudo -n cat "$f" > $L/stdout-$tag.txt
    sudo -n cat "$R/log" > $L/log-$tag.txt
    cases=$(awk '
        /zpool create -f testpool/ { t = $6; m = "-"; c = "-" }
        $2 == "testpool" && / corrupt / { m = $(NF-1) }
        /^NOTE: [a-z0-9]+: [0-9]+ checksum errors on \/var\/tmp\/file-1$/ {
            c = $3 }
        / checksum errors on \/var\/tmp\/file-1, ZED needs 10$/ {
            printf "%s:%s:%s:CHECKFAIL ", t, m, c }
        /wait_vdev_state testpool \/var\/tmp\/file-1 DEGRADED/ {
            printf "%s:%s:%s:%s ", t, m, c,
                ($1 == "SUCCESS:") ? "deg" : "NODEG" }' $L/stdout-$tag.txt)
    res=$(grep -oE 'auto_spare_002_pos.* \[[A-Z]+\]' $L/zts-$tag.log | tail -n 1)
    echo "$tag rc=$rc sha=$(echo $sha | cut -c1-12) pf=$(cat $P) $res | $cases| $R" >> $out
    awk -v tag=$tag -f $L/layouts.awk $L/status-$tag.txt $L/log-$tag.txt \
        >> $L/layouts.txt
    n=$((n + 1))
done
echo "DONE $pre $(date -u +%FT%TZ)" >> $out
```

## STEP 3: debug build (about 10 to 15 minutes)

```sh
D=/var/tmp/task-1123-asp2; L=/var/log/task-1123-asp2
setsid nohup sh -c "date -u +%T; cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j8; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log` until it prints a line; it must read `build-rc=0`. Then quote:
- the duration;
- `grep -E '^checking kernel (source|build) (version|directory)' $L/build.log`, which must name the running kernel;
- every `warning:` line outside objtool notes;
- `grep -c "'fault/auto_spare_002_pos'" $D/zt/tests/test-runner/bin/zts-report.py`: must print 0 (this is the generated report script that the runs use).

Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 4: the branch's test, 5 runs

Load the module with `sudo -n /var/tmp/task-1123-asp2/zt/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion`, which must equal S1, and the last `ZFS: Loaded module` line, which must say "(DEBUG mode)". Write mark 4 and run the pre-run checks of the VM rules. Then:

```sh
D=/var/tmp/task-1123-asp2; L=/var/log/task-1123-asp2
setsid nohup sh $L/runs.sh p 1 5 7642d2cd36c20d0ede1d18360d40a3f73612c6e2b2d4964e1820c2b0fc908ad7 > $L/runs-p.nohup 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/runs.txt` until a `DONE p` or a `STOP` line; a run takes about 30 s. Then:

```sh
L=/var/log/task-1123-asp2
grep -E '^(START|DONE|STOP)' $L/runs.txt; cat $L/runs-p.nohup $L/sampler-p*.nohup
grep -E '^p[0-9]+ ' $L/runs.txt | cut -d'|' -f1-2
grep -E '^NOTE: [a-z0-9]+: [0-9]+ checksum errors on /var/tmp/file-1$' $L/stdout-p?.txt
grep -E '^p[0-9]+ ' $L/layouts.txt
grep -hvE ' get=[0-9]+ grc=0$' $L/status-p?.txt | head -n 20
```

Then the summary per layout over the runs (with `p=v` in step 5):

```sh
L=/var/log/task-1123-asp2; p=p
grep -E "^$p[0-9]+ " $L/layouts.txt | awk '{
    k = $2; split($3, a, "="); c = a[2] + 0
    if (!(k in mn) || c < mn[k]) mn[k] = c
    if (c > mx[k]) mx[k] = c
    r[k]++
    if ($0 ~ / par=spare-/) at[k]++
    for (i = 1; i <= NF; i++) {
        split($i, b, "=")
        if (b[1] == "get-ok-spare") { gs[k] += b[2]; if (b[2] + 0 > 0) gr[k]++ }
        if (b[1] == "get-bad") gb[k] += b[2]
    }
} END { for (k in mn) printf "%s runs=%d count-min=%d count-max=%d spare-at-note=%d runs-with-get-ok-spare=%d get-ok-spare=%d get-bad=%d\n", k, r[k], mn[k], mx[k], at[k], gr[k], gs[k], gb[k] }' | sort
```

Expected:
- `START p1-p5`, `DONE p`, no STOP, and empty nohup files.
- 5 run lines, each `rc=0 sha=7642d2cd36c2 pf=0`, `[PASS]`, with its six layouts in the order mirror, raidz, raidz2, anymirror1, anymirror2, anymirror3, each ending `:deg`.
- 30 NOTE lines, six per run, each with a count of 10 or more. For scale, TASK-1120-ASP's sampler saw file-1's highest CKSUM per run, with the same `prefetch=none` test, at: mirror 833-1535, raidz1 0-1354, raidz2 211-790, anymirror1 7050-10981, anymirror2 2597-5611, anymirror3 4654-8318.
- In `layouts.txt`, for every layout and run:
  - `get-bad=0`, or failures only in samples taken while a pool was being created or destroyed: quote those samples (the last command above lists them);
  - `get-eq-ck` equal or close to `get-ok`: the two commands run a few ms apart, during the `dd`;
  - `ck-max` at least `note`: the count may still rise after the note (reads of file-1 while the spare resilvers and the handler is still active), and only `zpool clear` lowers it. A `ck-max` below `note` means that the sampler missed that part: say so.
- In the summary, for each layout: the lowest and highest count; whether `zpool get` read file-1 under `spare-N` (`runs-with-get-ok-spare` above 0); and in how many runs the last sample before the note already had `par=spare-0`, that is, the test's own `get_vdev_prop` ran after the spare was attached. A layout where file-1 was never seen under `spare-N` is reported as "not observed"; it is not a failure.

Every run that is not PASS: quote from its `stdout-<tag>.txt` the failing line with the 25 lines before it, the failing layout's samples (`grep "top=<top-level, e.g. raidz1-0> " $L/status-<tag>.txt`), and `grep -nE 'Diagnosis Engine|Retire Agent' $L/zed-<tag>.log | tail -n 40`. Then go on with the plan; it does not stop the task. A `CHECKFAIL` here is the result that matters most (see Why). A line `zpool get checksum_errors testpool /var/tmp/file-1` in a stdout is `get_vdev_prop`'s own failure: quote it with the run's `log-<tag>.txt` lines around it.

After the runs: the pre-run checks once more, and the kernel check of mark 4.

## STEP 5: the same test without `-o prefetch=none`, 3 runs

Edit the test in the tree, and check the edit:

```sh
D=/var/tmp/task-1123-asp2; T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
cd $D/zt && sed -n 75p $T && sed -i 's/ -o prefetch=none//' $T && sed -n 75p $T
echo "$(ls -l $T | cut -d' ' -f1) $(sha256sum < $T | cut -c1-64) $(grep -c -e '-o prefetch=none' $T)"
git diff --stat -- $T | tail -n 1; git diff --summary -- $T | wc -l; git diff -U0 -- $T | grep -E '^[-+][^-+]'
```

Expected (a tab begins each `log_must` line, after the `-` or `+` in the last two):

```
	log_must zfs create -o primarycache=none -o prefetch=none \
	log_must zfs create -o primarycache=none \
-rwxr-xr-x 8bde11c6720e5a216af69520b901e21be96676cc0e740fc662a30050f476542f 0
 1 file changed, 1 insertion(+), 1 deletion(-)
0
-	log_must zfs create -o primarycache=none -o prefetch=none \
+	log_must zfs create -o primarycache=none \
```

Any difference: restore the file with `git checkout -- $T`, then stop and report.

Write mark 5 and run the pre-run checks. Then:

```sh
D=/var/tmp/task-1123-asp2; L=/var/log/task-1123-asp2
setsid nohup sh $L/runs.sh v 1 3 8bde11c6720e5a216af69520b901e21be96676cc0e740fc662a30050f476542f > $L/runs-v.nohup 2>&1 < /dev/null &
```

Poll and quote as in step 4, with `v` in place of `p` (`runs-v.nohup`, `sampler-v*.nohup`, `^v[0-9]+ `, `stdout-v?.txt`, `status-v?.txt`, and `p=v` in the summary).

If none of the three runs has a `CHECKFAIL`, run three more, once: the same command with `v 4 6` and `runs-v2.nohup`. If none of the six has one, the demonstration is not made: say so.

Expected, run by run:
- A run in which a layout's count is below 10 ends right after that layout's NOTE. In TASK-1120-ASP this happened in `anymirror2` with 0 in four runs, and in `anymirror3` with 4 in one. The line right after that NOTE is `<layout>: <n> checksum errors on /var/tmp/file-1, ZED needs 10`, and only the test's cleanup follows it. The test is `[FAIL]` in about 15 s, where master's test took 1:12 to 1:17, and the run line in `runs.txt` ends with `<layout>:<MATCH>:<n>:CHECKFAIL`. For each such run, quote:
  - the failing line with the 25 lines before it, from `stdout-<tag>.txt`;
  - `grep -E 'checksum errors on /var/tmp/file-1' $L/log-<tag>.txt`, whose timestamps show that the failure follows the note at once, with no 60 s wait.
- A run in which every count reaches 10 passes, as master did in 3 of 8. Quote its six counts.
- A layout with a count of 10 or more that still ends `NODEG` (the old 60 s timeout) means that the check is necessary but not sufficient: the ZED's duplicate filter and the ereport rate limit can also drop ereports. Quote its samples, ZED lines and stdout lines as in step 4. It does not stop the task.

Then restore the test, and check it:

```sh
D=/var/tmp/task-1123-asp2; T=tests/zfs-tests/tests/functional/fault/auto_spare_002_pos.ksh
cd $D/zt && git checkout -- $T && echo "$(ls -l $T | cut -d' ' -f1) $(sha256sum < $T | cut -c1-64) $(git status --porcelain -- $T | wc -l)"
```

Expected: `-rwxr-xr-x 7642d2cd36c20d0ede1d18360d40a3f73612c6e2b2d4964e1820c2b0fc908ad7 0`. Then the pre-run checks, and the kernel check of mark 5.

## STEP 6: the `fault` group, once, with the unmodified test

Before the run:
- the pre-run checks of the VM rules;
- `sha256sum < $D/zt/$T | cut -c1-64` prints `7642d2cd36c20d0ede1d18360d40a3f73612c6e2b2d4964e1820c2b0fc908ad7`;
- `lsmod | grep -E '^scsi_debug '` is empty;
- `losetup -a` shows no loop device backed by a file under /var/tmp;
- `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` shows none.

Write mark 6. Then:

```sh
D=/var/tmp/task-1123-asp2; L=/var/log/task-1123-asp2
cd $D/zt && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T fault > $L/zts-fault.log 2>&1 < /dev/null &
```

Poll every 2 minutes with `tail -n 2 $L/zts-fault.log; pgrep -f 'zfs-tests.sh -vKO -T fault' > /dev/null || echo ENDED`. It takes about 25 minutes, 10 of them in `auto_offline_001_pos`. Then quote:

```sh
L=/var/log/task-1123-asp2
grep -E 'Test: .* \[[A-Z]+\]' $L/zts-fault.log; sed -n '/^Results Summary/,$p' $L/zts-fault.log
R=$(awk '/^Log directory/ { print $3 }' $L/zts-fault.log); sudo -n find "$R" -path '*auto_spare_002_pos*' -name stdout -exec grep -h 'checksum errors on' {} +
```

Expected, as on master on these VMs (DES on 17/11 and SPM on 21/11 gave PASS 21, FAIL 3, KILLED 1, with every `auto_spare_*` test passing; AH, ZTB and SLD6B-WR (23/11) had the same four non-PASS, plus a failing `auto_spare_002_pos`):
- Results Summary `PASS 21`, `FAIL 3`, `KILLED 1`, with `auto_spare_002_pos` among the PASS and its six counts at 10 or more. Different totals that come only from the four results below (for example an `auto_replace_*` that now passes with the udev rules) are not a finding: say which;
- the four known environment results:
  - `auto_offline_001_pos` KILLED at about 10:00: it stalls after the scsi_debug rescan, waiting for the ZED to bring the disk back;
  - `auto_online_001_pos` FAIL at about 00:00, with `failed to open /dev/disk/by-id/loop0` or `scsi_debug module already installed`, which the killed test left;
  - `auto_replace_001_pos` and `auto_replace_002_pos` FAIL: at about 00:02 with `vdev rule was not registered properly` where step 0 found no 69-vdev rules; where they are installed, with an ENOSPC line, as SLD6B-WR saw on VM 105 on 23/11;
- in zts-report's sections: `auto_replace_001_pos` and `auto_replace_002_pos` among the expected non-PASS; `auto_offline_001_pos` and `auto_online_001_pos` among the unexpected, since no list names them; `auto_spare_002_pos` in no section.

For each of the four known results, quote its result line and its first failing line (`ERROR` or the message), so that it can be compared with the earlier runs. Any other non-PASS, `auto_spare_002_pos` above all:
- quote its failing line with the 20 lines before it, from its `merged` or `stdout` file under `$R`;
- copy its output directory to `$L/fault-nonpass/`;
- say whether zts-report lists it.

Rerun nothing.

After the group:
- `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool. A pool left over (`testpool` or any other) is a stop: report it and do not destroy it.
- `sudo -n $D/zt/zinject` prints `No handlers registered.`
- `pgrep -a -x zed; pgrep -a -x lt-zed` is empty. A zed still running: quote it, then stop and report; do not kill it, since it holds /dev/zfs.
- `lsmod | grep -E '^scsi_debug '`: if the module is loaded, a test of this task loaded it (step 0 found it unloaded). Quote that line and `lsscsi`, then run `sudo -n modprobe -r scsi_debug`. If that fails, quote it, leave the module, and report it.
- `losetup -a`: a loop device backed by `/var/tmp/file-vdev*` is quoted and detached with `sudo -n losetup -d <device>`. Any other loop device is left as it is and reported.
- `ls -l /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* 2>&1` and `ls -ld /var/tmp/testdir 2>&1`. The killed test is known to leave `/var/tmp/file-vdev-1` to `-3` and `/var/tmp/file-vdev-spare`. For `tunable-*` files, quote their names and contents; the module unload below resets every tunable. Remove all of these with `sudo -n rm -rf`, and say what you removed.
- The kernel check of mark 6.

Then unload the module: `sudo -n /var/tmp/task-1123-asp2/zt/scripts/zfs.sh -u`. `lsmod | grep -E '^(zfs|spl) '` must then be empty. If the unload fails: capture as for a hang, then stop and report.

## STEP 7: cleanup

- `lsmod | grep -E '^(zfs|spl|scsi_debug) '`: empty.
- `sudo -n rm -rf /var/tmp/task-1123-asp2`, `sudo -n fstrim -av`, `df -h /var/tmp`.
- `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/zed /var/tmp/testdir 2>&1`: none.
- `ls -l /etc/zfs/zpool.cache 2>&1`. Size 0: `sudo -n mv /etc/zfs/zpool.cache /var/log/task-1123-asp2/zpool.cache.end`. Size above 0: quote it and leave it.
- The logs and the three files stay in `$L`. The ZTS results stay under /var/tmp/test_results.

## Report

Write `$L/report.md` and give the same text as your final message:

1. **Environment:**
   - the VM, kernel, Debian version, nproc, RAM, free space and `hung_task_timeout_secs`;
   - the udev rules line, `lsscsi` and scsi_debug, and the zed line of step 0;
   - `tree.txt`, the log and diffstat lines, the test file's mode and sha256s, and the three grep results;
   - FILES-OK;
   - the build's rc, duration, kernel lines and warnings, and the zts-report.py count;
   - S1 and "(DEBUG mode)";
   - what happened to /etc/zfs/zpool.cache at steps 0 and 7.
2. **Results:**
   - A table of the runs: tag | test file (head, or without prefetch=none) | result | duration | per layout `layout:MATCH:count:deg|NODEG|CHECKFAIL` | layouts whose last sample before the note had `par=spare-0`.
   - The summaries per layout of steps 4 and 5.
   - The `fault` group's Results Summary, with its non-PASS.
   - One verdict line, for example: "head be61dbbc0a7: 5/5 PASS, lowest count per layout mirror A, raidz B, raidz2 C, anymirror1 D, anymirror2 E, anymirror3 F; zpool get read file-1 in all six layouts, under spare-0 in G of them; without prefetch=none: K/3 FAIL at once with 'anymirror2: 0 checksum errors on /var/tmp/file-1, ZED needs 10' (test N s, no 60 s wait), J/3 PASS; -T fault: PASS 21, FAIL 3, KILLED 1, auto_spare_002_pos PASS, the other four as on master on these VMs". Or say what differs.
3. **Quotes:**
   - the START, DONE and STOP lines;
   - the NOTE lines of steps 4 and 5, and `layouts.txt`;
   - every non-PASS run's lines, as asked;
   - the outputs of the step 5 edit and restore;
   - the `fault` group's lines;
   - the kernel checks, with the suspend counts.
4. **Deviations and cleanup,** then `End`.
