TASK-1120-HRC (from the coordinator session "main"): functional, about 2 h, debug build only (one tree, rebuilt in place for master), ONE planned reboot at the end (step 6, pre-approved below). Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB); not next to a performance run.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-hrc`, `L=/var/log/task-1120-hrc`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

Two public issues, each fixed by one commit on upstream master `1b1aa0e91b6` (fork branches pushed, pull requests not opened yet: they wait for this task):

1. **#19571** "Panic on `zfs hold` with tag longer than 255 bytes" (branch `userhold-tag-zap-name`, commit `2d93e14facc`, `module/zfs/dsl_userhold.c`). A hold tag is a name in the snapshot's user hold ZAP, which takes at most 255 bytes plus the NUL. `dsl_dataset_user_hold_check_one()` only rejects tags longer than 256 bytes, so a 256-byte tag passes the check while the hold ZAP is absent or a microzap, and `zap_add()` then fails with ENAMETOOLONG in syncing context: `VERIFY0(zap_add(mos, zapobj, htag, 8, 1, &now, tx)) failed (36)`, `PANIC at dsl_userhold.c:168:dsl_dataset_user_hold_sync_one_impl()` (line 178 in 2.4.4, as in the issue), and `txg_sync` stops. The fix rejects tags of 256 bytes or more with E2BIG ("tag too long"). Its test `cli_root/zfs_hold/zfs_hold_tag_length` (new directory with setup and cleanup) tries 256-byte tags with no hold ZAP, with a microzap and with a fat ZAP, a 255-byte tag that must work, and a 1024-byte tag.
2. **#19694** "zfs receive -c: VERIFY0 in dmu_recv_begin_sync() on owned snapshot; DS_FLAG_INCONSISTENT left set after heal" (branch `recv-heal-held-snapshot`, commit `b7d4c2a4073`, `module/zfs/dmu_recv.c`). (a) A corrective receive into a snapshot that has an owner (mounted through `.zfs/snapshot`, an open snapshot zvol, another heal) passes the check, then `dmu_recv_begin_sync()` panics `txg_sync` with `VERIFY0(dsl_dataset_own_obj_force(dp, dsobj, dsflags, dmu_recv_tag, &newds)) failed (16)` (`dmu_recv.c:963`). (b) Every corrective receive, failed or successful, leaves the snapshot marked inconsistent (property `inconsistent` = 1): it can no longer be mounted. The fix returns EBUSY ("dataset is busy") from the check, never marks a healed snapshot, and clears an old mark on success. Its test `cli_root/zfs_receive/zfs_receive_corrective_owned`: a truncated heal must fail and leave `inconsistent` 0; a full heal must succeed, leave it 0, and the snapshot must be readable through `.zfs/snapshot`; a heal while a file of the mounted snapshot is held open must fail with "dataset is busy", and the pool must still sync.

Both were already run in user space (libzpool) by main: master aborts on both VERIFY0s and the fixes return E2BIG and EBUSY. This task runs them in the kernel: the tests 3 times each and their groups once with the fixes; then, with master's two module files, the receive test once (it fails cleanly at its first heal, on the inconsistent mark, before its panic step), and the two panics themselves, outside ZTS, with `fb.sh` (step 2). Each panic stops the `txg_sync` thread of its own pool for good (the SPL halts the thread; the host keeps running), and the module can then no longer be unloaded: hence the reboot of step 6.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-hrc`, head `d06ac793fb55626ac43fe76446d24dc451905db2`, tree `b52fa30064af402fe8c3b541b5254eefaaaa8535`: two transport commits (`[skip ci]`) on upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db`. `94118b7298e4a02bbd8a41c5bee9a198b84c987d` has exactly the tree of `2d93e14facc` (`fbde0136d38aaba8509495376315e3f8d57f7118`); the head adds the change of `b7d4c2a4073` unchanged (its `dmu_recv.c` and test are byte for byte those of `b7d4c2a4073`). Step 5 checks out master's `module/zfs/dsl_userhold.c` and `module/zfs/dmu_recv.c` (from `HEAD~2`), which gives the tree `75eaadf5627a8dfcc7e9b7bc5baf2bfbff4b7b40`: master's module with the fixes' tests, and rebuilds in place.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; `fb.sh` uses files under `$D/fb-heal` and `$D/fb-hold`. Touch no block device of the VM. Never run `zpool import` yourself (`-a` least of all); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the tests' own pools and, in step 5, `hrcr` and `hrch` (`fb.sh`, both with `cachefile=none`, both left wedged until the reboot by design).
- Before every ZTS run and every `fb.sh`: `lsmod | grep -E '^zfs '` shows the module of this step only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool (before `fb.sh hold` it shows `hrcr`, wedged by `fb.sh heal`: skip that one check there, and do not run any other `zpool` or `zfs` command on `hrcr`), `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/corrective_owned* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. `runs.sh` repeats these checks (all but lsmod) before each of its runs and writes a `STOP` line if one fails.
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a series whose $L/runs.txt has not changed for 30 minutes, or the `-T zfs_hold,zfs_receive` series still going after 60 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves exactly one, the planned reboot of step 6, after the two planned panics of step 5; for everything else it replaces the standing approval of 18/11. A hang, a panic or a module that will not unload anywhere else (steps 3 and 4, or step 5 before `fb.sh`): capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end of step 4; after step 6 none is loaded.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/corrective_owned* 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `systemctl is-enabled claude-rc.service 2>&1` (say what it prints: step 6 relies on your session coming back after the reboot), and `cat /proc/sys/kernel/sysrq`.

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-hrc
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-hrc; L=/var/log/task-1120-hrc
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs vm-kit-hrc && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git rev-parse 'HEAD~1^{tree}') $(git status --porcelain | wc -l)" > $L/tree.txt
echo "d06ac793fb55626ac43fe76446d24dc451905db2 94118b7298e4a02bbd8a41c5bee9a198b84c987d 1b1aa0e91b694798085a2771c4eef40e93b9b2db b52fa30064af402fe8c3b541b5254eefaaaa8535 fbde0136d38aaba8509495376315e3f8d57f7118 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional/cli_root
printf '%s\n' "e639908f6129b2cda802a28c4d02df5c03f9f0ffb8e10944b67d3448444f4e9e  module/zfs/dsl_userhold.c" "b5df75165df05b6a1484a761e1518aa52fff637235da823fad52a060720062c4  module/zfs/dmu_recv.c" "3e20349e6cc9d84dc16960236bb7cc4c4bd337720c09fa7caf629027db537000  $T/zfs_hold/zfs_hold_tag_length.ksh" "fc2be4bf04e378d4689cfc78f75cb794d6dc52afb43c3777e782e029f3edc875  $T/zfs_hold/setup.ksh" "44265ffd37c988f313eaa0c1ecb141b3e68056112b8c9f2145f679f653ad700a  $T/zfs_hold/cleanup.ksh" "0ae6493e61e913ee52956997d70b8f1721a3526db4abf6ac870c45147dd969f5  $T/zfs_receive/zfs_receive_corrective_owned.ksh" "2a151b145d94fe9d667e529a953c1e146d5099b2b36bf8f8f4a72cd3d28fedff  tests/runfiles/common.run" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~2 HEAD; ls -l $T/zfs_hold/ | cut -c1-10
```

Expected TREE-OK, SHA-OK, the diffstat

```
 module/zfs/dmu_recv.c                              | 28 ++++++-
 module/zfs/dsl_userhold.c                          | 10 ++-
 tests/runfiles/common.run                          |  6 +-
 tests/zfs-tests/tests/Makefile.am                  |  4 +
 .../tests/functional/cli_root/zfs_hold/cleanup.ksh | 16 ++++
 .../tests/functional/cli_root/zfs_hold/setup.ksh   | 18 +++++
 .../cli_root/zfs_hold/zfs_hold_tag_length.ksh      | 80 +++++++++++++++++++
 .../zfs_receive/zfs_receive_corrective_owned.ksh   | 90 ++++++++++++++++++++++
 8 files changed, 245 insertions(+), 7 deletions(-)
```

and, after the `total` line, three `-rwxr-xr-x` lines. If a fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree before step 5.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/runs.sh, the second to $L/fb.sh. Then:

```sh
L=/var/log/task-1120-hrc
printf '%s\n' "f0b5f1c4457e1061c8135dceba93ed7b53266027823ceaa708e64e515bdf6695  $L/runs.sh" "3ca899089a240f1eb07c97f3938a8f977989b1d6ba36e75661976271137bedc2  $L/fb.sh" | sha256sum -c && sh -n $L/runs.sh && sh -n $L/fb.sh && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tag> <runs> <selection>` runs as the login user. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks; each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1120-HRC ZTS runner, run as the login user: runs.sh <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1120-hrc
L=/var/log/task-1120-hrc
tag=$1
runs=$2
shift 2
out=$L/runs.txt
cd $D/zt || exit 1
echo "START $tag $(date -u +%FT%TZ) $*" >> $out
n=1
while [ $n -le $runs ]; do
    pools=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1)
    inj=$(sudo -n $D/zt/zinject 2>&1 | head -n 1)
    loops=$(sudo -n losetup -a 2>&1)
    left=$(ls -d /var/tmp/file-* /var/tmp/spare-* \
        /var/tmp/corrective_owned* 2>/dev/null)
    if [ -n "$pools" ] || [ "$inj" != "No handlers registered." ] ||
        [ -n "$loops" ] || [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: pools=[$pools] zinject=[$inj] loops=[$loops] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO "$@" \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc dir=$R" >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

`fb.sh heal|hold` runs as root, once each, in step 5 only, with master's module loaded. `heal` creates pool `hrcr` (one 512 MiB file, `cachefile=none`, mounted under `$D/fb-heal/mnt`), a file system with a 4 MiB file, its snapshot `s1` and a send stream of it; opens the snapshot's file through `.zfs/snapshot` (the automount owns the snapshot, and the open file keeps it mounted), then starts `zfs receive -c` of that stream into `s1` in the background. `hold` creates pool `hrch` (one 512 MiB file, `cachefile=none`, not mounted), a file system and its snapshot `s1`, then starts `zfs hold` with a 256-byte tag in the background. Each waits up to 60 s for a new `PANIC at` line in the kernel log, saves the kernel log since its start to $L/fb-<mode>-dmesg.txt, and prints (and writes to $L/fb-<mode>.txt) the state and kernel stack of the background command, the state of every `txg_sync` thread, and the VERIFY and PANIC lines. It never waits on the command it started, which stays in D state on master.

```sh
#!/bin/sh
# TASK-1120-HRC fail-before on master, run as root: fb.sh heal|hold
# Starts in the background the command that master's module turns into a
# VERIFY0 in txg_sync, then waits up to 60 s for the PANIC line.  The pool
# stays wedged until the reboot of step 6, so nothing here waits on it.
D=/var/tmp/task-1120-hrc
L=/var/log/task-1120-hrc
Z=$D/zt
mode=$1
v=$D/fb-$mode
o=$L/fb-$mode.txt
case $mode in
heal|hold) ;;
*) echo "usage: fb.sh heal|hold"; exit 1 ;;
esac
mkdir $v || exit 1
n0=$(dmesg | wc -l)
truncate -s 512M $v/vdev || exit 1
if [ $mode = heal ]; then
    p=hrcr
    $Z/zpool create -o cachefile=none -O mountpoint=$v/mnt $p $v/vdev ||
        exit 1
    $Z/zfs create $p/fs || exit 1
    dd if=/dev/urandom of=$v/mnt/fs/file bs=128k count=32 2>/dev/null ||
        exit 1
    $Z/zfs snapshot $p/fs@s1 || exit 1
    $Z/zfs send $p/fs@s1 > $v/s1.zsend || exit 1
    # the automount owns the snapshot; the open file keeps it mounted
    exec 3< $v/mnt/fs/.zfs/snapshot/s1/file || exit 1
    echo "$mode: snapshot mounted: $(grep -c "$p/fs@s1" /proc/mounts)," \
        "inconsistent=$($Z/zfs get -H -o value inconsistent $p/fs@s1)" > $o
    $Z/zfs receive -c $p/fs@s1 < $v/s1.zsend > $v/cmd.out 2>&1 &
    pid=$!
else
    p=hrch
    $Z/zpool create -o cachefile=none -O mountpoint=none $p $v/vdev ||
        exit 1
    $Z/zfs create $p/fs || exit 1
    $Z/zfs snapshot $p/fs@s1 || exit 1
    tag=$(printf '%0256d' 0 | tr 0 a)
    echo "$mode: tag of ${#tag} bytes, no hold before" > $o
    $Z/zfs hold "$tag" $p/fs@s1 > $v/cmd.out 2>&1 &
    pid=$!
fi
i=0
while [ $i -lt 60 ]; do
    dmesg | tail -n +$((n0 + 1)) | grep -q 'PANIC at' && break
    sleep 1
    i=$((i + 1))
done
sleep 5
dmesg | tail -n +$((n0 + 1)) > $L/fb-$mode-dmesg.txt
echo "$mode: waited ${i}s; command pid $pid:" \
    "$(ps -o stat=,wchan:32= -p $pid 2>&1 | tr -s ' ')," \
    "output [$(cat $v/cmd.out)]" >> $o
cat /proc/$pid/stack >> $o 2>&1
for t in $(pgrep -x txg_sync); do
    echo "txg_sync $t: $(ps -o stat=,wchan:32= -p $t | tr -s ' ')"
done >> $o
grep -E 'VERIFY|PANIC' $L/fb-$mode-dmesg.txt >> $o
cat $o
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-hrc; L=/var/log/task-1120-hrc
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 4: the fixes (tags h, r, g)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks of the VM rules, then:

1. The two new tests, 3 runs each (tags h then r), one series after the other:

   ```sh
   L=/var/log/task-1120-hrc
   setsid nohup sh -c "sh $L/runs.sh h 3 -t zfs_hold_tag_length && sh $L/runs.sh r 3 -t zfs_receive_corrective_owned" > $L/runs-hr.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/runs.txt` until a `DONE r` or a `STOP` line. Expected PASS in every run, with each directory's setup and cleanup PASS too. For the first h run and the first r run, with R its `dir=`, quote `grep -hE 'NOTE: zfs hold with a|NOTE: corrective receive into a mounted' $R/log` (expected: four `zfs hold with a <n>-byte tag failed` notes, for 256, 256, 256 and 1024 bytes, each ending in `tag too long`; and one note ending in `dataset is busy`).
2. The two tag groups once (tag g), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-hrc
   setsid nohup sh $L/runs.sh g 1 -T zfs_hold,zfs_receive > $L/runs-g.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE g` or a `STOP` line. Expected all PASS: `zfs_hold_tag_length` and the 35 tests of `cli_root/zfs_receive` (`zfs_receive_corrective`, `zfs_receive_compressed_corrective`, `zfs_receive_large_block_corrective`, `zfs_receive_zstd_corrective` and `zfs_receive_corrective_owned` among them), with their setup and cleanup.

Then quote:

```sh
L=/var/log/task-1120-hrc
grep -E '^(START|DONE|STOP)' $L/runs.txt; cat $L/runs-*.nohup
grep -E '^[a-z0-9]+-[0-9]+ rc=' $L/runs.txt
grep -cE '\[PASS\]$' $L/runs.txt; grep -E '\[(FAIL|KILLED|SKIP)\]$' $L/runs.txt
```

Expected no STOP, empty nohup files, every rc=0, and no FAIL, KILLED or SKIP line. Quote every `Test:` line of $L/runs.txt for tags h and r, and the durations of the five corrective tests of tag g. A non-PASS test: quote its line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:' $R/log` around its failure (R its `dir=`), say whether tests/test-runner/bin/zts-report.py.in lists it as known, and go on with the plan without rerunning it; it does not stop the task.

After them: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault` quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## STEP 5: master's module, fail-before (tags m, then fb.sh heal and hold)

```sh
cd /var/tmp/task-1120-hrc/zt && git checkout HEAD~2 -- module/zfs/dsl_userhold.c module/zfs/dmu_recv.c && test "$(git write-tree)" = 75eaadf5627a8dfcc7e9b7bc5baf2bfbff4b7b40 && echo TREE-OK && sha256sum module/zfs/dsl_userhold.c module/zfs/dmu_recv.c && git diff --cached --name-status HEAD
```

Expected TREE-OK, `70420d55f58eccd21e40f27269a1aa1d3a1bf81e3c9a975f17bc6240a3231f5d  module/zfs/dsl_userhold.c`, `4a7e193fe4f9e16e11e19b602a579f03ccfe9044f518cd4973417c9f8a7ab4a2  module/zfs/dmu_recv.c`, and two lines `M` and a file name (`module/zfs/dmu_recv.c`, `module/zfs/dsl_userhold.c`) separated by a tab. Any difference: stop and report.

Rebuild in place, detached and polled like a build (only the two objects and the links: a few minutes):

```sh
D=/var/tmp/task-1120-hrc; L=/var/log/task-1120-hrc
setsid nohup sh -c "cd $D/zt && nice make -j4; echo build-rc=\$?" > $L/build-master.log 2>&1 < /dev/null &
```

It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S2=`modinfo -F srcversion $D/zt/module/zfs.ko`: it must differ from S1. Load: `sudo -n $D/zt/scripts/zfs.sh`, srcversion equal to S2, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`. Run the pre-run checks, then:

1. The receive test once on master (tag m), expected to FAIL cleanly:

   ```sh
   L=/var/log/task-1120-hrc
   setsid nohup sh $L/runs.sh m 1 -t zfs_receive_corrective_owned > $L/runs-m.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE m` or a `STOP` line. Expected rc other than 0 and `zfs_receive_corrective_owned` `[FAIL]` (setup and cleanup PASS). With R its `dir=`, quote `grep -nE 'inconsistent|ERROR|dataset is busy' $R/log | head -n 20`. The prediction: it fails at the first `check_consistent`, right after the truncated heal (`log_mustnot ... zfs receive -c ...` passed), with `... is marked inconsistent (1)`, and never reaches the mounted-snapshot step. A PASS, a FAIL elsewhere, or a hang: quote the whole $R/log, say so plainly, and stop there (do not run `fb.sh`; report and wait for main).
2. The pre-run checks once more and the dmesg check since N1 (none expected), then the heal panic:

   ```sh
   sudo -n sh /var/log/task-1120-hrc/fb.sh heal
   ```

   It returns within about 70 s. Expected: the first line `heal: snapshot mounted: 1, inconsistent=0`, then `waited <n>s` with n below 60, the command in state `D` with an empty output, its kernel stack in `txg_wait_synced` (via `dsl_sync_task` or `dmu_recv_begin`), one `txg_sync` thread in `D`, and the two lines `VERIFY0(dsl_dataset_own_obj_force(dp, dsobj, dsflags, dmu_recv_tag, &newds)) failed (16)` and `PANIC at dmu_recv.c:963:dmu_recv_begin_sync()` (964 is fine too). Quote its whole output.
3. The pre-run checks except the pool list (see the VM rules: `hrcr` is wedged), then the hold panic:

   ```sh
   sudo -n sh /var/log/task-1120-hrc/fb.sh hold
   ```

   Expected: `hold: tag of 256 bytes, no hold before`, `waited <n>s` with n below 60, the command in `D` with an empty output, two `txg_sync` threads in `D` (one per wedged pool), and the lines `VERIFY0(zap_add(mos, zapobj, htag, 8, 1, &now, tx)) failed (36)` and `PANIC at dsl_userhold.c:168:dsl_dataset_user_hold_sync_one_impl()`. Quote its whole output.

   If `fb.sh` prints `waited 60s` with no PANIC line, or anything else than predicted, quote everything, then go on to step 6 all the same (the pools are in an unknown state and the reboot is the way out); say it plainly in the report. If `fb.sh` itself does not return within 3 minutes, capture as for a hang (into $L/hang.txt) and go on to step 6.

## STEP 6: the planned reboot

Nothing can unload the module now. Write $L/RESUME.md (what is done; next: step 7 of TASK-1120-HRC, then the report), flush only that filesystem, and arm the reboot detached so that your call returns (a plain `sync` would hang on the wedged pools; `b` reboots at once, without unmounting them):

```sh
L=/var/log/task-1120-hrc
sudo -n dmesg > $L/dmesg-before-reboot.txt; sync -f $L/RESUME.md $L/dmesg-before-reboot.txt $L/fb-heal.txt $L/fb-hold.txt $L/runs.txt
sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'
```

After the reboot, read $L/RESUME.md and carry on with step 7 without waiting for main. If the VM is not back within 15 minutes, main will check.

## STEP 7: after the reboot, cleanup

`uptime -s` (later than step 0's), `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty: nothing loads the in-tree module at boot), `ls -l /dev/zfs 2>&1` (absent), `sudo -n losetup -a` (empty), `ls -l /etc/zfs/zpool.cache 2>&1` (absent or size 0: both `fb.sh` pools had `cachefile=none` and the ZTS pools were destroyed; size above 0: report it, leave it, do not read it). If `journalctl --list-boots` shows the previous boot, quote `sudo -n journalctl -k -b -1 | grep -E 'VERIFY|PANIC at'` (the same four lines as step 5, if the journal is persistent; say so if it is not).

Then: `ls -l $D/fb-heal $D/fb-hold` (record), `sudo -n rm -rf $D` (the tree and the two wedged pools' files: never import them), `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/corrective_owned* 2>&1` (none), `df -h /var/tmp`. If /etc/zfs/zpool.cache exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`. Logs and scripts stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line and the claude-rc line of step 0; TREE-OK, SHA-OK, diffstat; SCRIPTS-OK; rc and warnings of both builds; S1, S2 and DEBUG mode; step 5's TREE-OK, sums and name-status; what happened to /etc/zfs/zpool.cache at steps 0 and 7; the reboot (time armed, `uptime -s` after).
2. Table: tag | tree (fixes d06ac793fb5, or master's module 75eaadf5627) | selection | runs | result per test (PASS count, or FAIL) | durations. Rows: h, r, g, m, fb-heal, fb-hold (for the last two: the PANIC line and the wait in seconds). Then one verdict line, e.g. "fixes: zfs_hold_tag_length 3/3, zfs_receive_corrective_owned 3/3, zfs_hold+zfs_receive 36/36 PASS; master's module: zfs_receive_corrective_owned FAIL at the first heal (inconsistent 1), corrective receive into a mounted snapshot panics txg_sync (VERIFY0 own_obj_force, 16, dmu_recv.c:963), a 256-byte hold panics txg_sync (VERIFY0 zap_add, 36, dsl_userhold.c:168): #19571 and #19694 reproduced and fixed", or what differs.
3. Quotes: the START/DONE lines, the NOTE lines of step 4.1, every non-PASS of the fixes with its lines, the m run's grep lines, both `fb.sh` outputs, the dmesg checks, the journal lines of step 7.
4. Deviations, cleanup, then End.
