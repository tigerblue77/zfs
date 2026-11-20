TASK-1120-RBP (from the coordinator session "main"): functional, about 1 h 15, debug build only, no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The two scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-rbp`, `L=/var/log/task-1120-rbp`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. The one import of this task is the one `rb.sh` makes of its own pool `rbtest`, from a copy of its own cache file under $D (step 4.3). If a result surprises you, stop and report it with the outputs.

## Why

`zfs rewrite` keeps the logical block size of a file: after a change of `recordsize`, existing files keep their old blocks. The prototype adds `zfs rewrite -b <size>`. It copies each whole regular file into a shadow object of the new block size, created in the unlinked set and owned as the file is. It then exchanges the two objects' block trees in syncing context (`dmu_object_swap_data()`, carried out by `dmu_objset_sync()` after the meta-dnode sync) and frees the shadow, which then holds the old blocks. Its man page promises the following:

- The file keeps its inode, links and open descriptors.
- After a crash, the file has either its old or its new blocks, and the space of the other is freed once the file system is mounted again.
- The size can only grow.
- Old blocks still referenced by snapshots stay allocated.

The prototype was built and tested in userspace only (ztest 4 times, and a libzpool program). Its kernel path has never run, so this task is its first kernel run.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-rbp`, head `5d04bd6ac8b2292f0e590c0039bad563b82ba813`. That head is a transport commit (`VM transport of reblock-prototype [skip ci]`) with the same tree as its parent. The parent `200e40d0637914dd0acac1310552d7eeeef641e2` (tree `9e968555ce4f15e1d5ac45abbb38d6e857919832`) is the prototype's head, and it is what you build. The prototype is two commits on `cde5d598eb44b5d6efe8167666d988a496346657` (upstream master as of 18/11):

- `054f9b82288014353a9b3212e38215033f550b81` "dmu: add dmu_object_swap_data() to exchange object data", which adds the ztest function `ztest_dmu_object_swap_data`;
- `200e40d0637` "zfs rewrite: add -b to change the block size of files", which adds the ZTS test `cli_root/zfs_rewrite/zfs_rewrite_blocksize`, registered in no runfile and no Makefile.am.

The checks, each with its expected result:

- **Z1** `-t zfs_rewrite_blocksize`, 3 runs: PASS each. `-t` finds the test in the tree although no runfile lists it.
- **Z2** `-T zfs_rewrite`, once: the existing rewrite tests, all PASS. There is no tag `rewrite`; the tag is `zfs_rewrite`. The group is setup, `zfs_rewrite`, `zfs_rewrite_dedup_birth`, `zfs_rewrite_physical`, `zfs_rewrite_physical_raw_send`, `zfs_rewrite_skip_clone`, `zfs_rewrite_skip_current`, `zfs_rewrite_skip_snapshot`, `zfs_rewrite_skip_width` and cleanup.
- **F1 to F14** (`rb.sh`, functional) run on a RAID-Z2 of eight 1.5 GiB file vdevs, `ashift=12`, `compression=off`. A 1 GiB file `f1` is written at `recordsize=128K`, snapshot `@before` is taken, `recordsize` is set to 1M, and then `zfs rewrite -b 1M f1` runs. The checks cover:
  - inode, links and size unchanged;
  - `st_blksize` and zdb `dblk` going from 128K to 1M;
  - sha256 unchanged;
  - `@before` still at 128K with the old content;
  - space accounting;
  - after `@before` is destroyed, a scrub that is clean.

  The space figures follow from the RAID-Z2 allocation with eight disks and 4K sectors. A 128K block is 32 data sectors and 12 parity, 44 rounded up to 45, so 71.1 %. A 1M block is 256 and 86, so 342 and 74.85 %. `used`, `referenced` and user space are deflated by the 128K ratio, so data in 1M blocks shows as 0.950 of what it showed in 128K blocks. Hence:
  - `referenced` after / before ≈ 0.950;
  - `usedbysnapshots` after ≈ `referenced` before;
  - `used` and the pool's `allocated` ≈ 1.95 times;
  - root's user space ≈ 0.950;
  - the pool's `allocated` once `@before` is destroyed ≈ 0.950 of what it was before the rewrite.

  Each ratio is checked against a range a few hundredths wide around its expected value; the CHECK lines give the exact ranges.
- **C1 to C8** (`rb.sh`, crash): a second 1 GiB file `f2` at 128K, `recordsize` set to 1M, and a 25 ms delay injected on one vdev so that the copy takes a while. `zfs rewrite -b 1M f2` is started in the background. As soon as its shadow appears in the unlinked set (the dataset kstat `nunlinks - nunlinked` = 1), `zpool freeze` cuts the pool there. That is how ZTS's replay tests simulate a crash: nothing written after the freeze txg reaches the pool's uberblock. The delay is then removed and the rewrite left to end; its own result is lost with the crash. The pool's cache file is copied (`rbtest` is the only pool in it, checked), the pool exported and imported from that copy. Expected:
  - the shadow is in the unlinked set at mount (`nunlinks` 1) and freed within 60 s;
  - `f2`'s content unchanged, with its old blocks (128K, expected) or its new ones (1M, if the exchange had synced before the freeze; then C1 is UNINFORMATIVE, not a failure);
  - `f1` read back after the import, still at 1M with its content;
  - a second `zfs rewrite -b 1M f2` brings it to 1M;
  - a clean scrub;
  - after the last export, `zdb -bcc` of the exported pool reports "No leaks".
- **T1** ztest for 300 seconds on the debug userland, because the prototype adds work in syncing context: exit 0, and at least one `swapped data of objects` line, the prototype's own ztest function printing at `-VVVVVV`.
- **K1** dmesg: no VERIFY, ASSERT, PANIC, BUG, Oops, blocked-task or list_del line.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs, and `rb.sh` and ztest only files under $D. Touch no block device of the VM. Never `zpool import` anything yourself (`-a` least of all); `rb.sh` imports its own pool `rbtest` once, from its own cache file copy, and only after checking that the copy names no other pool. Never `zfs-tests.sh -x`.
- The ZTS test `zpool_import_all_001_pos` imports every exported pool it finds on the VM's disks: during ROV3 it imported VM 105's exported measurement pool `tank`, which may still be there, exported. No run of this task includes it, but after every run `zpool list` must still show no pool. A pool this task did not create (`tank` or any other) is a stop: report it before the next run, and leave it as it is (no export, no destroy, no import).
- Before every ZTS run and before `rb.sh`, run these pre-run checks:
  - `lsmod | grep -E '^zfs '` shows the module of this task only;
  - `timeout 30 sudo -n $D/zr/zpool list -H -o name` shows no pool;
  - `sudo -n $D/zr/zinject` prints `No handlers registered.`;
  - `ls -d /var/tmp/file-vdev* /var/tmp/dev-* $D/vdev $D/mnt 2>&1` shows nothing;
  - `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool that uses it goes, so an empty one is normal. A non-empty one names a pool left behind: stop and report its `ls -l`. `rb.sh`'s pool uses `$D/rb.cache`, never `/etc/zfs/zpool.cache`, and ztest writes its cache file under its own `-f` directory.

  `zts.sh` repeats these checks (all but lsmod, and without the $D paths) before each of its runs, and writes a `STOP` line if one fails.
- Every build, ZTS run, script and ztest runs detached (`setsid nohup ... &`) and is polled. A hang is:
  - a build still going after 90 minutes;
  - a `zts-<tag>.txt` that has not changed for 30 minutes, or a loop still going after 75 minutes;
  - an `rb.txt` that has not changed for 20 minutes, or `rb.sh` still going after 60 minutes;
  - ztest still going after 15 minutes, or a `ztest.log` that has not changed for 10 minutes.

  Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|ztest|zdb'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report. `rb.sh` bounds every command that could block and captures by itself: on a hang it writes a `HANG` line and $L/hang-rb.txt and exits.
- Reboots: one is pre-approved only where a task says so, and this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none), `id zts`, `command -v ksh xxh128sum` (both present: the new test uses `xxh128digest`; if one is missing, stop and report), `cat /proc/sys/kernel/hung_task_timeout_secs`, and `pgrep -a -x zed || echo NO-ZED` (recorded either way, not a stop; do not start or stop zed).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-rbp
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-rbp; L=/var/log/task-1120-rbp
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zr && cd $D/zr && git init -q . && git fetch -q --depth 4 https://github.com/tigerblue77/zfs vm-kit-rbp && git checkout -q FETCH_HEAD~1
echo "$(git rev-parse FETCH_HEAD) $(git rev-parse FETCH_HEAD^{tree}) $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "5d04bd6ac8b2292f0e590c0039bad563b82ba813 9e968555ce4f15e1d5ac45abbb38d6e857919832 200e40d0637914dd0acac1310552d7eeeef641e2 9e968555ce4f15e1d5ac45abbb38d6e857919832 054f9b82288014353a9b3212e38215033f550b81 cde5d598eb44b5d6efe8167666d988a496346657 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "a543fc1177f25779211bd13ecc477727d677fa563a23b646d6812f5466a16a4c  module/zfs/zfs_vnops.c" "6f7de986173b3e38a10544c8704c84ad3d67e429bfa1e21a076946c29d846af4  module/zfs/dnode.c" "64869ed69affa8659056abaf517125e36051c0b44bf3a507b4c08eb0cdad726e  module/zfs/dmu_objset.c" "7785ca495613362cb17cb29d40472fc3c82416a7aa7996f57e280e8a8127a75c  module/os/linux/zfs/zfs_vnops_os.c" "f4f80c2d7576a283dec60b7bdbfc1f5a0c4b3763414b3f6cef3772b4f1136895  cmd/ztest.c" "7d9a7c5bde96dd77eb66a15b294292632d7c2037f78fca7d07ba632051adafc8  tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/zfs_rewrite_blocksize.ksh" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 HEAD~2 HEAD | tail -n 1; git log --format='%H %s' HEAD~2..HEAD
```

Expected TREE-OK, SHA-OK, ` 18 files changed, 993 insertions(+), 9 deletions(-)`, and

```
200e40d0637914dd0acac1310552d7eeeef641e2 zfs rewrite: add -b to change the block size of files
054f9b82288014353a9b3212e38215033f550b81 dmu: add dmu_object_swap_data() to exchange object data
```

If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/zts.sh, the second to $L/rb.sh. Then:

```sh
L=/var/log/task-1120-rbp
printf '%s\n' "24a50a3f2fb681b9c91284fa47cef24e5c21c74581413be10628dbe171f53c9a  $L/zts.sh" "4093024b4e6a33db300462476c3d91a9c0c6657acf2a30d802cfa6383eab19c8  $L/rb.sh" | sha256sum -c && sh -n $L/zts.sh && sh -n $L/rb.sh && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`zts.sh <tag> <runs> -t|-T <name>` runs as the login user. It runs `zfs-tests.sh -vKO -t|-T <name>` `<runs>` times, one at a time, each after the pre-run checks. Each run's console goes to $L/zts-<tag>-<n>.log, and $L/zts-<tag>.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's result lines, shortened to the path under `functional/`.

```sh
#!/bin/sh
# TASK-1120-RBP ZTS loop, run as the login user: zts.sh <tag> <runs> -t|-T <name>
D=/var/tmp/task-1120-rbp
L=/var/log/task-1120-rbp
tag=$1
runs=$2
opt=$3
name=$4
out=$L/zts-$tag.txt
cd $D/zr || exit 1
echo "START $tag $opt $name $(date -u +%FT%TZ)" >> $out
n=1
while [ $n -le $runs ]; do
    pools=$(timeout 30 sudo -n $D/zr/zpool list -H -o name 2>&1)
    hdl=$(timeout 30 sudo -n $D/zr/zinject 2>&1)
    left=$(ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>/dev/null)
    if [ -n "$pools" ] || [ "$hdl" != "No handlers registered." ] ||
        [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: pools=[$pools] handlers=[$hdl] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO $opt $name \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc dir=$R" >> $out
    grep -E 'Test: .* \[[A-Z]+\]' $L/zts-$tag-$n.log |
        sed -e 's|^.*Test: .*/functional/|    |' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

`rb.sh` runs as root, once. It creates the pool `rbtest`:

- eight 1.5 GiB files under $D/vdev, RAID-Z2, `ashift=12`;
- `cachefile=$D/rb.cache`, mounted under $D/mnt, `compression=off`;
- two file systems, `rbtest/fs` for F and `rbtest/c` for C.

It runs F1 to F14 and C1 to C8 (see Why) and writes $L/rb.txt. Every command that could block is run in the background and given a bound: 60 s to 30 minutes depending on the command. If a command does not return, the script writes a `HANG` line and $L/hang-rb.txt and exits. An unexpected error writes a `STOP` line instead, and the script exits leaving the pool and its files as they are.

Its only kernel import is `zpool import -c $D/rb.cache.bkp rbtest`, after `zdb -C -U` has shown that this copy names `rbtest` only. It ends by exporting its pool and running `zdb -e -p $D/vdev -bcc rbtest` on the exported pool (userspace, no import in the kernel; the output goes to $L/rb-zdb.txt). Then it removes its files.

```sh
#!/bin/sh
# TASK-1120-RBP zfs rewrite -b, functional and crash checks, run as root: rb.sh
D=/var/tmp/task-1120-rbp
L=/var/log/task-1120-rbp
Z=$D/zr
K=/proc/spl/kstat/zfs
P=rbtest
V=$D/vdev
M=$D/mnt
C=$D/rb.cache
out=$L/rb.txt
cmd=$L/rb-cmd.txt
cur=setup

# Records the kernel state when a command does not return, and stops.
hang() {
    echo "HANG $cur: $*" >> $out
    {
        echo "== $* $(date -u +%FT%TZ)"
        ps -eLo pid,tid,stat,comm,wchan:32 |
            grep -E ' D|txg|zfs|zpool|zinject|zdb'
        for t in $(ps -eLo tid=,stat= | awk '$2 ~ /^D/ { print $1 }'); do
            echo "== stack $t $(cat /proc/$t/comm)"
            cat /proc/$t/stack
        done
        cat $K/$P/state
        dmesg | tail -n 150
    } > $L/hang-rb.txt 2>&1
    exit 2
}

# Stops on an unexpected error, leaving the pool and the files as they are.
stop() {
    echo "STOP $cur: $*" >> $out
    cat $cmd >> $out 2>&1
    exit 1
}

# bounded <seconds> <command ...>: output in $cmd; a hang past <seconds>.
bounded() {
    secs=$1
    shift
    "$@" > $cmd 2>&1 &
    bp=$!
    n=0
    while kill -0 $bp 2>/dev/null; do
        [ $n -lt $secs ] || hang "$* still running after ${secs}s"
        sleep 1
        n=$((n + 1))
    done
    wait $bp
}

# check <id> <expected ...>: states what the next check expects.
check() {
    cur=$1
    shift
    echo "CHECK $cur expect: $*" >> $out
}

# result <status> <evidence ...>: 0 PASS, 2 UNINFORMATIVE, else FAIL.
result() {
    case $1 in
    0) r=PASS ;;
    2) r=UNINFORMATIVE ;;
    *) r=FAIL ;;
    esac
    shift
    echo "RESULT $cur $r: $*" >> $out
}

# ratio <a> <b> <low> <high>: prints a/b; status 0 within [low, high].
ratio() {
    awk -v a="$1" -v b="$2" -v lo=$3 -v hi=$4 'BEGIN {
        if (b + 0 == 0) { print "none"; exit 1 }
        r = a / b; printf "%.4f", r; exit !(r >= lo && r <= hi) }'
}

# zobj <dataset> <object>: lvl iblk dblk dsize lsize of the object (zdb).
zobj() {
    $Z/zdb -U $C -dddd $1 $2 |
        awk -v o=$2 '$1 == o && NF >= 9 { print $2, $3, $4, $5, $7; exit }'
}

# dblk <zobj output>: its data block size.
dblk() {
    echo $1 | cut -d' ' -f3
}

# sha <file>, st <file>: sha256; inode, links, size and st_blksize.
sha() {
    sha256sum < $1 | cut -d' ' -f1
}
st() {
    stat -c '%i %h %s %o' $1
}

# space <dataset>: used, referenced, usedbysnapshots, the pool's allocated
# and root's user space, in bytes.
space() {
    echo $($Z/zfs get -Hp -o value used,referenced,usedbysnapshots $1) \
        $($Z/zpool list -Hp -o allocated $P) \
        $($Z/zfs userspace -Hp -o name,used $1 |
        awk '$1 == "root" { print $2 }')
}

# unl <dataset>: nunlinks and nunlinked of its objset kstat.
unl() {
    for k in $K/$P/objset-*; do
        awk -v ds=$1 '$1 == "dataset_name" && $3 == ds { f = 1 }
            $1 == "nunlinks" { a = $3 }
            $1 == "nunlinked" { b = $3 }
            END { if (f) print a, b }' $k
    done
}

# drained <dataset>: status 0 once its unlinked set is empty, within 60 s.
drained() {
    w=0
    while [ $w -lt 60 ]; do
        [ "$(unl $1 | awk '{ print $1 - $2 }')" = 0 ] && return 0
        sleep 1
        w=$((w + 1))
    done
    return 1
}

# scrubbed <file>: scrub, status -v in <file>; status 0 when it is clean.
scrubbed() {
    bounded 900 $Z/zpool scrub -w $P || stop "zpool scrub"
    bounded 60 $Z/zpool status -v $P || stop "zpool status"
    cp $cmd $1
    e=$(awk '$2 == "ONLINE" && NF == 5 { s += $3 + $4 + $5 }
        END { print s + 0 }' $1)
    ev="$(grep -E 'scan:|errors:' $1 | tr -s ' ' | tr '\n' ' ')counters $e"
    grep -q 'scrub repaired 0B' $1 && grep -q 'with 0 errors' $1 &&
        grep -q 'No known data errors' $1 && [ "$e" = 0 ]
}

echo "START $(date -u +%FT%TZ)" >> $out
[ -z "$($Z/zpool list -H -o name)" ] || stop "a pool exists before the run"
mkdir -p $V $M || stop "mkdir"
vdevs=
for k in 1 2 3 4 5 6 7 8; do
    truncate -s 1536M $V/v$k || stop "truncate"
    vdevs="$vdevs $V/v$k"
done
bounded 120 $Z/zpool create -f -o ashift=12 -o cachefile=$C \
    -O mountpoint=$M -O compression=off $P raidz2 $vdevs ||
    stop "zpool create"
bounded 60 $Z/zfs create -o recordsize=128K $P/fs || stop "zfs create fs"
bounded 60 $Z/zfs create -o recordsize=128K $P/c || stop "zfs create c"
mountpoint -q $M/fs && mountpoint -q $M/c || stop "not mounted"

# F: a file written at 128K, then recordsize=1M and zfs rewrite -b 1M.
F=$M/fs/f1
bounded 300 dd if=/dev/urandom of=$F bs=1M count=1024 || stop "dd f1"
bounded 300 $Z/zpool sync $P || stop "zpool sync"
h1=$(sha $F)
s1=$(st $F)
o1=${s1%% *}
z1=$(zobj $P/fs $o1)
echo "F f1 sha256 $h1, stat (inode links size blksize) [$s1]," \
    "zdb (lvl iblk dblk dsize lsize) [$z1]" >> $out
check F1 "f1 written at 128K: st_blksize 131072, zdb dblk 128K"
[ "${s1##* }" = 131072 ] && [ "$(dblk "$z1")" = 128K ]
result $? "stat [$s1] zdb [$z1]"
[ "${s1##* }" = 131072 ] || stop "f1 is not at 128K"
bounded 60 $Z/zfs snapshot $P/fs@before || stop "zfs snapshot"
bounded 300 $Z/zpool sync $P || stop "zpool sync"
b=$(space $P/fs)
echo "F before (used referenced usedbysnapshots allocated root): $b" >> $out
[ "$(echo $b | wc -w)" = 5 ] || stop "not 5 space values before: [$b]"
bounded 60 $Z/zfs set recordsize=1M $P/fs || stop "zfs set recordsize"

check F2 "zfs rewrite -b 1M f1 returns 0"
t0=$(date +%s)
bounded 900 $Z/zfs rewrite -b 1M $F
rc=$?
result $rc "rc $rc in $(($(date +%s) - t0))s, output [$(cat $cmd)]"
[ $rc = 0 ] || stop "zfs rewrite -b failed"
bounded 300 $Z/zpool sync $P || stop "zpool sync"

check F3 "no shadow left: unlinked set of $P/fs empty within 60 s"
drained $P/fs
result $? "nunlinks nunlinked $(unl $P/fs)"
bounded 300 $Z/zpool sync $P || stop "zpool sync"
s2=$(st $F)
z2=$(zobj $P/fs $o1)

check F4 "same inode, links and size, st_blksize 1048576"
[ "${s2% *}" = "${s1% *}" ] && [ "${s2##* }" = 1048576 ]
result $? "before [$s1] after [$s2]"

check F5 "zdb dblk 1M"
[ "$(dblk "$z2")" = 1M ]
result $? "before [$z1] after [$z2]"

check F6 "content unchanged: sha256 $h1"
h=$(sha $F)
[ "$h" = "$h1" ]
result $? "sha256 $h"

check F7 "@before keeps the old file: sha256 $h1, st_blksize 131072," \
    "zdb dblk 128K"
S=$M/fs/.zfs/snapshot/before/f1
hs=$(sha $S)
ss=$(st $S)
zs=$(zobj $P/fs@before $o1)
[ "$hs" = "$h1" ] && [ "${ss##* }" = 131072 ] && [ "$(dblk "$zs")" = 128K ]
result $? "sha256 $hs stat [$ss] zdb [$zs]"

a=$(space $P/fs)
echo "F after (used referenced usedbysnapshots allocated root): $a" >> $out
[ "$(echo $a | wc -w)" = 5 ] || stop "not 5 space values after: [$a]"
set -- $b $a

check F8 "referenced after / before in [0.93, 0.97]" \
    "(0.950: RAID-Z2 6+2, 4K sectors, 1M against 128K blocks)"
r=$(ratio "$7" "$2" 0.93 0.97)
result $? "referenced $2 -> $7, ratio $r"

check F9 "usedbysnapshots after / referenced before in [0.98, 1.02]" \
    "(the old blocks, held by @before)"
r=$(ratio "$8" "$2" 0.98 1.02)
result $? "usedbysnapshots $3 -> $8, ratio $r"

check F10 "used after / referenced before in [1.92, 1.98]"
r=$(ratio "$6" "$2" 1.92 1.98)
result $? "used $1 -> $6, ratio $r"

check F11 "pool allocated after / before in [1.90, 1.98]"
r=$(ratio "$9" "$4" 1.90 1.98)
result $? "allocated $4 -> $9, ratio $r"

check F12 "root's user space after / before in [0.93, 0.97]"
r=$(ratio "${10}" "$5" 0.93 0.97)
result $? "root $5 -> ${10}, ratio $r"

bounded 120 $Z/zfs destroy $P/fs@before || stop "zfs destroy"
bounded 600 $Z/zpool wait -t free $P || stop "zpool wait -t free"
bounded 300 $Z/zpool sync $P || stop "zpool sync"
d=$(space $P/fs)
echo "F @before destroyed (used referenced usedbysnapshots allocated root):" \
    "$d" >> $out
[ "$(echo $d | wc -w)" = 5 ] || stop "not 5 space values: [$d]"
set -- $b $d

check F13 "@before destroyed: pool allocated / allocated before" \
    "in [0.93, 0.97]"
r=$(ratio "$9" "$4" 0.93 0.97)
result $? "allocated $4 -> $9, ratio $r; referenced $7"

check F14 "scrub repairs nothing, no error"
scrubbed $L/rb-status-f.txt
result $? "$ev"

# C: zfs rewrite -b cut by a crash: zpool freeze while it copies (a delay on
# one vdev slows it down), then export and import from the cache file.
cur=Csetup
G=$M/c/f2
bounded 300 dd if=/dev/urandom of=$G bs=1M count=1024 || stop "dd f2"
bounded 300 $Z/zpool sync $P || stop "zpool sync"
h2=$(sha $G)
s3=$(st $G)
o2=${s3%% *}
echo "C f2 sha256 $h2, stat [$s3], zdb [$(zobj $P/c $o2)]" >> $out
[ "${s3##* }" = 131072 ] || stop "f2 is not at 128K"
bounded 60 $Z/zfs set recordsize=1M $P/c || stop "zfs set recordsize"
bounded 60 $Z/zinject -d $V/v1 -D 25:1 $P || stop "zinject -D"
$Z/zfs rewrite -b 1M $G > $L/rb-c-rewrite.txt 2>&1 &
rp=$!
t0=$(date +%s)
n=0
while [ "$(unl $P/c | awk '{ print $1 - $2 }')" != 1 ]; do
    kill -0 $rp 2>/dev/null ||
        stop "the rewrite ended before its shadow was seen:" \
        "$(cat $L/rb-c-rewrite.txt)"
    [ $n -lt 600 ] || hang "no shadow 60 s after zfs rewrite -b of f2"
    sleep 0.1
    n=$((n + 1))
done
echo "C shadow seen after $(($(date +%s) - t0))s:" \
    "nunlinks nunlinked $(unl $P/c)" >> $out
bounded 600 $Z/zpool freeze $P || stop "zpool freeze"
run=no
kill -0 $rp 2>/dev/null && run=yes
echo "C frozen $(($(date +%s) - t0))s after the start," \
    "rewrite still running: $run" >> $out
bounded 60 $Z/zinject -c all || stop "zinject -c all"
n=0
while kill -0 $rp 2>/dev/null; do
    [ $n -lt 900 ] || hang "zfs rewrite -b of f2 running 900 s after freeze"
    sleep 1
    n=$((n + 1))
done
wait $rp
echo "C rewrite rc $? after $(($(date +%s) - t0))s," \
    "output [$(cat $L/rb-c-rewrite.txt)]" >> $out
cp $C $D/rb.cache.bkp || stop "cp $C"
names=$($Z/zdb -C -U $D/rb.cache.bkp | awk '$1 == "name:" { print $2 }')
echo "C cache file backup names: $names" >> $out
[ "$names" = "'$P'" ] || stop "the cache file backup does not name $P only"
bounded 600 $Z/zpool export $P || stop "zpool export"
[ -z "$($Z/zpool list -H -o name)" ] || stop "a pool remains after export"
bounded 600 $Z/zpool import -c $D/rb.cache.bkp $P || stop "zpool import -c"
[ "$($Z/zpool list -H -o name)" = $P ] || stop "pools other than $P"
u=$(unl $P/c)
s4=$(st $G)
z4=$(zobj $P/c $o2)
echo "C imported: nunlinks nunlinked $u, f2 stat [$s4] zdb [$z4]" >> $out

check C1 "the crash left the shadow in the unlinked set: nunlinks 1 at" \
    "mount (0 with f2 at 1M: the rewrite had synced, uninformative)"
case "${u%% *} ${s4##* }" in
"1 "*) rc=0 ;;
"0 1048576") rc=2 ;;
*) rc=1 ;;
esac
result $rc "nunlinks nunlinked $u, f2 st_blksize ${s4##* }"

check C2 "unlinked set of $P/c empty within 60 s of the mount"
drained $P/c
result $? "nunlinks nunlinked $(unl $P/c)"

check C3 "f2 content unchanged: sha256 $h2"
h=$(sha $G)
[ "$h" = "$h2" ]
result $? "sha256 $h"

check C4 "f2 has its old blocks (131072/128K, expected) or the new ones" \
    "(1048576/1M), stat and zdb agreeing"
case "${s4##* } $(dblk "$z4")" in
"131072 128K"|"1048576 1M") rc=0 ;;
*) rc=1 ;;
esac
result $rc "stat [$s4] zdb [$z4]"

check C5 "f1 read back after the import: sha256 $h1, st_blksize 1048576"
h=$(sha $F)
s=$(st $F)
[ "$h" = "$h1" ] && [ "${s##* }" = 1048576 ]
result $? "sha256 $h stat [$s]"

check C6 "zfs rewrite -b 1M of f2 again: rc 0, st_blksize 1048576," \
    "sha256 $h2, no shadow left"
bounded 900 $Z/zfs rewrite -b 1M $G
rc=$?
bounded 300 $Z/zpool sync $P || stop "zpool sync"
drained $P/c
dr=$?
h=$(sha $G)
s=$(st $G)
[ $rc = 0 ] && [ $dr = 0 ] && [ "$h" = "$h2" ] && [ "${s##* }" = 1048576 ]
result $? "rc $rc, nunlinks nunlinked $(unl $P/c), sha256 $h, stat [$s]," \
    "zdb [$(zobj $P/c $o2)]"

check C7 "scrub repairs nothing, no error"
scrubbed $L/rb-status-c.txt
result $? "$ev"

check C8 "zdb -bcc of the exported pool: rc 0, No leaks, no leaked line"
bounded 600 $Z/zpool export $P || stop "zpool export"
[ -z "$($Z/zpool list -H -o name)" ] || stop "a pool remains after export"
bounded 1800 $Z/zdb -e -p $V -bcc $P
rc=$?
cp $cmd $L/rb-zdb.txt
l=$(grep -c -E 'leaked space|\(leaked|potentially leaked' $L/rb-zdb.txt)
g=$(grep -c -i error $L/rb-zdb.txt)
grep -q 'No leaks' $L/rb-zdb.txt && [ $rc = 0 ] && [ $l = 0 ]
result $? "rc $rc, $(grep 'No leaks' $L/rb-zdb.txt | tr -s ' \t' ' ')," \
    "leak lines $l, lines with error $g"

rm -rf $V $M $C $D/rb.cache.bkp
echo "DONE $(date -u +%FT%TZ) results $(grep -c '^RESULT' $out)," \
    "FAIL $(grep -c '^RESULT .* FAIL:' $out)" >> $out
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-rbp; L=/var/log/task-1120-rbp
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-zr.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zr/module/zfs.ko`: it must differ from master's `65D8C0C24A9B688639F0721`.

## STEP 4: kernel checks

`sudo -n $D/zr/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

1. **Z1**, `-t zfs_rewrite_blocksize` 3 times (tag bs), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-rbp
   setsid nohup sh $L/zts.sh bs 3 -t zfs_rewrite_blocksize > $L/zts-bs.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/zts-bs.txt` until a `DONE bs` or a `STOP` line. Expected for each run: `bs-<n> rc=0`, then `cli_root/zfs_rewrite/setup`, `zfs_rewrite_blocksize` and `cleanup`, each `[PASS]`. Quote $L/zts-bs.txt in full, `cat $L/zts-bs.nohup`, and the Results Summary of each `$L/zts-bs-<n>.log`. Also quote, from the first run's `$R/log` (R from its `dir=`), the test's line `512-byte blocks before <b>, after <a>`: the space the 8 MiB file with its 4 MiB hole takes before and after `-b 1M`. For every non-PASS run, take R from its `dir=`:
   - copy `$R/log` to $L/log-bs-<n>.txt;
   - quote its failing line with the 20 lines before it;
   - quote its `log_note` lines (`NOTE:`).

   This test has never run before; a FAIL is a result to report, not a reason to stop. Then `ls -d /var/tmp/file-vdev* 2>&1` (none).
2. **Z2**, `-T zfs_rewrite` once (tag zr), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-rbp
   setsid nohup sh $L/zts.sh zr 1 -T zfs_rewrite > $L/zts-zr.nohup 2>&1 < /dev/null &
   ```

   Poll as above. Expected `zr-1 rc=0` and 10 `[PASS]` lines (setup, the 8 tests, cleanup). Quote the lines of $L/zts-zr.txt with their durations and the Results Summary. Quote a FAIL with the 20 lines before it (from `$R/log`, copied to $L/log-zr.txt), and say whether tests/test-runner/bin/zts-report.py.in lists it as known. Report it as is, without a rerun.
3. **F1 to F14, C1 to C8**, `rb.sh` once, after the pre-run checks:

   ```sh
   L=/var/log/task-1120-rbp
   setsid nohup sudo -n sh $L/rb.sh > $L/rb.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/rb.txt` until a `DONE`, `STOP` or `HANG` line (about 10 to 20 minutes). Each check writes a line `CHECK <id> expect: ...` before it runs and a line `RESULT <id> PASS|FAIL|UNINFORMATIVE: <evidence>` after. Lines starting with `F ` or `C ` record values (sizes, timings, the shadow and the freeze). Expected: 22 RESULT lines, all PASS (C1 may be UNINFORMATIVE, see Why), and `DONE ... results 22, FAIL 0`.

   Then:

   ```sh
   D=/var/tmp/task-1120-rbp; L=/var/log/task-1120-rbp
   cat $L/rb.txt; cat $L/rb.nohup
   grep -E 'scan:|errors:' $L/rb-status-f.txt $L/rb-status-c.txt
   grep -E 'No leaks|leaked|error' $L/rb-zdb.txt; tail -n 25 $L/rb-zdb.txt
   timeout 30 sudo -n $D/zr/zpool list -H -o name; sudo -n $D/zr/zinject; ls -d $D/vdev $D/mnt $D/rb.cache $D/rb.cache.bkp 2>&1
   ```

   Quote all of it: $L/rb.txt in full. A FAIL is reported with its evidence, and the run goes on. Expected after DONE: no pool, `No handlers registered.`, and none of the four paths left. Then `sudo -n fstrim -av`.

   A `STOP` line means an unexpected error: quote it and what follows it, `timeout 30 sudo -n $D/zr/zpool status -v`, `sudo -n $D/zr/zinject`, and leave the pool as it is. A `HANG` line means `rb.sh` captured a command that did not return: quote $L/hang-rb.txt. After either, stop and report.

After them, run the pre-run checks once more (no pool, no handler, CACHE-OK, no leftovers). **K1**: quote every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del`, with 10 lines before it (none expected). Unload with `sudo -n $D/zr/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must then be empty.

## STEP 5: ztest, 300 seconds (T1, userspace, module unloaded)

```sh
D=/var/tmp/task-1120-rbp; L=/var/log/task-1120-rbp
mkdir $D/zt && setsid nohup sh -c "cd $D/zt && ulimit -c 0 && $D/zr/ztest -T 300 -f $D/zt -VVVVVV; echo ztest-rc=\$?" > $L/ztest.log 2>&1 < /dev/null &
```

Poll `grep ztest-rc= $L/ztest.log` (about 6 minutes; ztest kills and restarts its own children on purpose, so a kill rate is not a failure). Then:

```sh
L=/var/log/task-1120-rbp
head -n 3 $L/ztest.log; grep ztest-rc= $L/ztest.log; grep -c 'swapped data of objects' $L/ztest.log; grep -m 3 'swapped data of objects' $L/ztest.log; grep -B 2 ztest-rc= $L/ztest.log; ls -l $L/ztest.log
```

Expected: the RAID type and vdev lines, `ztest-rc=0`, a count of 1 or more and 3 such lines, and the `killed, completed` line. With a non-zero rc, quote `tail -n 60 $L/ztest.log` (the failed assertion and the call trace), keep $D/zt for main, and say so.

## STEP 6: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty). Unless step 5 asked to keep $D/zt: `sudo -n rm -rf $D`; otherwise `sudo -n rm -rf $D/zr`. Then `sudo -n fstrim -av`, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` (none) and `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. **Environment**:
   - the VM, kernel, Debian version, nproc, RAM, hung_task_timeout_secs and the zed line of step 0;
   - TREE-OK, SHA-OK, the diffstat and the two commit lines;
   - SCRIPTS-OK;
   - the build rc and its warnings;
   - S1 (against `65D8C0C24A9B688639F0721`) and DEBUG mode;
   - what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. **Table**: check | expected | PASS / FAIL / UNINFORMATIVE | evidence. The evidence is the RESULT line's text, or the test's result line with its duration. Rows:
   - Z1, one per run;
   - Z2, one per test;
   - F1 to F14 and C1 to C8;
   - T1 (rc, number of swaps, the `killed, completed` line);
   - K1.

   Then one verdict line, for example "reblock-prototype 200e40d0637, first kernel run: Z1 3/3 PASS, Z2 10/10 PASS, rb.sh 22/22 PASS (C1: the crash left 1 shadow, freed at mount; f2 kept its 128K blocks), ztest 300 s rc 0 with N swaps, dmesg clean", or what differs.
3. **Quotes**: $L/rb.txt in full, every non-PASS ZTS run with its failing lines, the ztest lines of step 5, and the dmesg check.
4. Deviations, cleanup, then End.
