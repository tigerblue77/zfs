TASK-1120-DLA (from the coordinator session "main"): functional, about 1 h 30, debug build only (one tree, rebuilt in place for the previous head), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-dla`, `L=/var/log/task-1120-dla`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

#19565 ("zpool detach: erase labels once the detach is on disk") moves the erase of a detached device's labels from `spa_vdev_detach()` to `spa_vdev_config_exit_impl()`, once the txg of the detach is on disk. Its previous head `8b14c89f5e7` fails `zhack_label_repair_002`, `_003` and `_004` on every OS of the fork CI, and these tests fail on no other head: after `zpool detach` and `zhack label repair -u`, the import of the detached device fails with `internal error: cannot import 'testpool': Value too large for defined data type` (exit 262; FreeBSD: `I/O error`), and the debug log says `vdev tree has 1 missing top-level vdevs`.

The cause: `8b14c89f5e7` generated the erasing label when it wrote it, after the device had left its mirror, so the label described only the remaining disk (`vd->vdev_top`), with the uberblock of the detach txg, whose blocks the detached device never received. `zhack label repair -u` only restores a txg in that label and uberblock, so the device cannot be imported. On master the label describes the mirror before the detach, with the uberblock of that time. The fix `62016569b1a` generates the label in `spa_vdev_detach()` where master wrote it (`vdev_label_detach_generate()`), keeps it with that uberblock in the vdev, and writes it once the txg is on disk (`vdev_label_detach_write()`). In userspace (libzpool, file vdevs) the three scenarios fail with `8b14c89f5e7` exactly as in CI and pass with the fix and with master, and the config the fix writes to the detached device is byte for byte master's.

What is tested: fork https://github.com/tigerblue77/zfs, branch `vm-kit-dla`, head `02f03d68c6f799e4c80857910a2655786e8f67d6`, tree `f9583023e9dcfe0f3e56f635e56ba40282288065`: a transport commit (same tree) on the fix `62016569b1af25f8cece153a61c5f26bc92b7e06` "zpool detach: erase labels once the detach is on disk", on upstream master `c913da86156eba3243cc1ef515e98d69887654a7`. The previous head `8b14c89f5e7d1ccf7200145b635dd2ffed6590f2` (fetched by its SHA in step 1) has the same tests; it differs in six files only: `include/sys/vdev.h`, `include/sys/vdev_impl.h`, `module/zfs/spa.c`, `module/zfs/spa_misc.c`, `module/zfs/vdev.c`, `module/zfs/vdev_label.c`. Step 5 checks those six out from it, which gives its own tree `406807aad647a4b847ee417798474bf7cafd6f33`, and rebuilds in place.

The parts:

- `lbl.sh` (step 2): a mirror of two 256 MiB files under $D (pool `dlalbl`, `cachefile=none`), `zpool detach` of the second, `zdb -lu` of the detached file, then the pool is destroyed. It prints the label's config txg, the type of its `vdev_tree` and its number of children, the uberblock's txg and root block birth, and the remaining file's label txg (the txg of the detach). Expected with the fix: config txg 0, tree `'mirror'` with 2 children, uberblock txg 0, birth below the remaining file's label txg. With `8b14c89f5e7`: tree `'file'` with 0 children, birth at or above it.
- With the fix: `-T zhack` 3 times (`zhack_label_repair_001` to `_004`, `zhack_metaslab_leak`, `zhack_mos_scan_reclaim`); the PR's own tests `zpool_detach_005_pos` and `zpool_detach_006_pos` 3 times each; `-T zpool_detach,replacement` once. Expected all PASS.
- With `8b14c89f5e7` (fail-before): `zhack_label_repair_002`, `_003` and `_004` once each. Expected FAIL, at the import after `zhack label repair -u`, as in CI.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs, and the `zhack_label_repair` tests their own loop devices on files under /var/tmp, which they create and remove; touch no other block device of the VM. Never run `zpool import` yourself (`-a` least of all; the tests import their own pools, which is expected); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only `dlalbl` (step 2's script, destroyed by it) and the tests' own pools.
- Before every ZTS run and every `lbl.sh`: `lsmod | grep -E '^zfs '` shows the module of this step only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/disk /var/tmp/mirrordisk /var/tmp/detach_* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. `runs.sh` repeats these checks (all but lsmod) before each of its runs and writes a `STOP` line if one fails.
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a series whose $L/runs.txt has not changed for 30 minutes, or the `-T zpool_detach,replacement` series still going after 75 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zhack|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/disk /var/tmp/mirrordisk /var/tmp/detach_* 2>&1` (none), `id zts`, and `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-dla
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-dla; L=/var/log/task-1120-dla
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs vm-kit-dla && git checkout -q FETCH_HEAD && git fetch -q --depth 1 https://github.com/tigerblue77/zfs 8b14c89f5e7d1ccf7200145b635dd2ffed6590f2
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git write-tree) $(git rev-parse '8b14c89f5e7d1ccf7200145b635dd2ffed6590f2^{tree}') $(git status --porcelain | wc -l)" > $L/tree.txt
echo "02f03d68c6f799e4c80857910a2655786e8f67d6 62016569b1af25f8cece153a61c5f26bc92b7e06 c913da86156eba3243cc1ef515e98d69887654a7 f9583023e9dcfe0f3e56f635e56ba40282288065 406807aad647a4b847ee417798474bf7cafd6f33 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional/cli_root
printf '%s\n' "3b69d84724a62e3839451cb9e67a4ac8a185bd7e2a41430ae6c8700c317fb279  module/zfs/spa.c" "f3d0cbb4c76b81803657cfb899483b0038d772dc7645b6298b4c5eb2a613f3ec  module/zfs/spa_misc.c" "206846b2e59a8026f6178b58e2a6ea1b8f26cc608b6d974922334e68a8db88b3  module/zfs/vdev_label.c" "46053195b44fd952fa47b19f8c38e720c0dfd74124c2034bbe24bcb91a7a1d9d  module/zfs/vdev.c" "5de6e6afc388eb2e246f1d8a51bac0d2e30f366f85759eefd5a4d300ef5411a7  include/sys/vdev_impl.h" "6a7a41f1f946f397dd823e5cd20c0a3efdb6e272d76628e300e16786fc725541  include/sys/vdev.h" "779428ed76b9609bc2cf682bf711580d4df48be4191edcc2c5eae012fa659829  $T/zhack/library.kshlib" "f76d837519011ad3c3719c58f0dbcfcf50c021ba3e75f07e89b841835d36ad68  $T/zpool_detach/zpool_detach_005_pos.ksh" "c86c0b8cfdc2f5fb6dc38d6d2330765dfdada31dd52ef6a5e19b6815f89c8854  $T/zpool_detach/zpool_detach_006_pos.ksh" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat=80 8b14c89f5e7d1ccf7200145b635dd2ffed6590f2 HEAD; grep -c vdev_label_detach_generate module/zfs/vdev_label.c module/zfs/spa.c
```

Expected TREE-OK, SHA-OK, the diffstat

```
 include/sys/vdev.h      |   2 +
 include/sys/vdev_impl.h |   2 +
 module/zfs/spa.c        |   9 ++
 module/zfs/spa_misc.c   |   9 +-
 module/zfs/vdev.c       |   5 +
 module/zfs/vdev_label.c | 283 ++++++++++++++++++++++++++++++------------------
 6 files changed, 199 insertions(+), 111 deletions(-)
```

(another git version may count the moved lines of vdev_label.c differently: a different count on that line and the total alone is not a stop, other files or names are), and `module/zfs/vdev_label.c:2`, `module/zfs/spa.c:1`. If a fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree before step 5.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/runs.sh, the second to $L/lbl.sh. Then:

```sh
L=/var/log/task-1120-dla
printf '%s\n' "598bb55383a7502cc82b2ae71c8c7e8c2a45292e0076971f3035b388c8e6a089  $L/runs.sh" "3a255a286ac1ac4e5c5f0a13cef27c2658103ea0b51090fd50c8dda6ec1a6e71  $L/lbl.sh" | sha256sum -c && sh -n $L/runs.sh && sh -n $L/lbl.sh && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tag> <runs> <selection>` runs as the login user. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks; each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1120-DLA ZTS runner, run as the login user: runs.sh <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1120-dla
L=/var/log/task-1120-dla
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
    left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/disk \
        /var/tmp/mirrordisk /var/tmp/detach_* 2>/dev/null)
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

`lbl.sh <tag>` runs as root. It creates the pool `dlalbl` (a mirror of two 256 MiB files in $D/lbl-<tag>), detaches the second file, saves `zdb -lu` of it to $L/lbl-<tag>-b.txt and `zdb -l` of the first plus `zpool status -P` to $L/lbl-<tag>-a.txt, destroys the pool, removes the files, and prints (and appends to $L/lbl.txt) one line: `<tag> detach_rc=<rc> destroy_rc=<rc> a_label_txg=<txg> b_config_txg=<txg> b_tree=<type> b_children=<n> b_ub_txg=<txg> b_ub_birth=<txg>`.

```sh
#!/bin/sh
# TASK-1120-DLA label of a detached device, run as root: lbl.sh <tag>
D=/var/tmp/task-1120-dla
L=/var/log/task-1120-dla
Z=$D/zt
tag=$1
p=dlalbl
v=$D/lbl-$tag
mkdir $v || exit 1
truncate -s 256M $v/a $v/b || exit 1
$Z/zpool create -o cachefile=none -O mountpoint=none $p mirror $v/a $v/b ||
    exit 1
$Z/zpool sync $p
$Z/zpool detach $p $v/b
rc=$?
$Z/zdb -lu $v/b > $L/lbl-$tag-b.txt 2>&1
$Z/zdb -l $v/a > $L/lbl-$tag-a.txt 2>&1
$Z/zpool status -P $p >> $L/lbl-$tag-a.txt 2>&1
$Z/zpool destroy $p
drc=$?
rm -rf $v
d=$(awk '/^    txg: / { print $2; exit }' $L/lbl-$tag-a.txt)
s=$(awk '
    /^    txg: / && !c { ct = $2; c = 1 }
    /^        type: / && !t { tt = $2; t = 1 }
    /^        children\[[0-9]+\]:/ { nc++ }
    /^\ttxg = / && !u { ut = $3; u = 1 }
    /birth=[0-9]+L/ && !b { match($0, /birth=[0-9]+L/)
        bt = substr($0, RSTART + 6, RLENGTH - 7); b = 1 }
    END { printf "b_config_txg=%s b_tree=%s b_children=%d b_ub_txg=%s b_ub_birth=%s",
        ct, tt, nc, ut, bt }' $L/lbl-$tag-b.txt)
echo "$tag detach_rc=$rc destroy_rc=$drc a_label_txg=$d $s" | tee -a $L/lbl.txt
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-dla; L=/var/log/task-1120-dla
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 4: the fix (tags n, z, p5, p6, g)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks of the VM rules, then:

1. Label of a detached device (tag n): `sudo -n sh /var/log/task-1120-dla/lbl.sh n`. Expected `n detach_rc=0 destroy_rc=0 a_label_txg=<A> b_config_txg=0 b_tree='mirror' b_children=2 b_ub_txg=0 b_ub_birth=<B>` with B below A. Quote the line, `sed -n '/vdev_tree:/,/features_for_read:/p' $L/lbl-n-b.txt` (the mirror with both files; the detached one has no `create_txg`) and the `zpool status -P` part of $L/lbl-n-a.txt. Then the pre-run checks once more (no pool: `dlalbl` is gone).
2. `-T zhack`, 3 runs (tag z), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-dla
   setsid nohup sh $L/runs.sh z 3 -T zhack > $L/runs-z.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/runs.txt` until a `DONE z` or a `STOP` line. Expected 3 runs with rc=0, each with 6 PASS: `zhack_label_repair_001` to `_004`, `zhack_metaslab_leak`, `zhack_mos_scan_reclaim`.
3. The PR's tests, 3 runs each (tags p5 then p6), after the pre-run checks, one series after the other:

   ```sh
   L=/var/log/task-1120-dla
   setsid nohup sh -c "sh $L/runs.sh p5 3 -t zpool_detach_005_pos && sh $L/runs.sh p6 3 -t zpool_detach_006_pos" > $L/runs-p.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE p6` or a `STOP` line. Expected PASS in every run (the `zpool_detach` setup and cleanup PASS too). For each 005 run, quote `grep -h 'txg of the labels of the copy of m3' $R/log` with R its `dir=` (expected a non-zero txg).
4. `-T zpool_detach,replacement` once (tag g), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-dla
   setsid nohup sh $L/runs.sh g 1 -T zpool_detach,replacement > $L/runs-g.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE g` or a `STOP` line. Expected all PASS: `zpool_detach_001_neg`, `_005_pos`, `_006_pos` and the 18 tests of `replacement`, with their setup and cleanup.

Then quote:

```sh
L=/var/log/task-1120-dla
grep -E '^(START|DONE|STOP)' $L/runs.txt; cat $L/runs-*.nohup
grep -E '^[a-z0-9]+-[0-9]+ rc=' $L/runs.txt
grep -cE '\[PASS\]$' $L/runs.txt; grep -E '\[(FAIL|KILLED|SKIP)\]$' $L/runs.txt
```

Expected no STOP, empty nohup files, every rc=0, and no FAIL, KILLED or SKIP line. Quote every `Test:` line of $L/runs.txt for tags z, p5, p6 and g (they carry the durations). A non-PASS test: quote its line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:' $R/log` around its failure (R its `dir=`), say whether tests/test-runner/bin/zts-report.py.in lists it as known, and go on with the plan without rerunning it; it does not stop the task.

After them: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault` quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## STEP 5: the previous head, fail-before (tags o, f2, f3, f4)

```sh
cd /var/tmp/task-1120-dla/zt && git checkout 8b14c89f5e7d1ccf7200145b635dd2ffed6590f2 -- include/sys/vdev.h include/sys/vdev_impl.h module/zfs/spa.c module/zfs/spa_misc.c module/zfs/vdev.c module/zfs/vdev_label.c && test "$(git write-tree)" = 406807aad647a4b847ee417798474bf7cafd6f33 && echo TREE-OK && sha256sum module/zfs/spa.c module/zfs/spa_misc.c module/zfs/vdev_label.c && git diff --cached --name-status HEAD; grep -c vdev_label_detach_generate module/zfs/vdev_label.c
```

Expected TREE-OK, `3e3ab7d1ca6b0fbfd2d3cad2b343bee5a50159de431c70c9eac73c28249d2316  module/zfs/spa.c`, `c6fa02db05200cd932c5a1a4af1901ee57ad15b1c6885d7dbab77fc94ad292d5  module/zfs/spa_misc.c`, `f564a9f734a9765c10c5660bf3422538491abc9239a7f3cbb79a68504c5976f9  module/zfs/vdev_label.c`, six lines `M` and a file name (the six files above) separated by a tab, and `0`. Any difference: stop and report.

Rebuild in place, detached and polled like a build (most of the module and of libzpool recompile: a header changed):

```sh
D=/var/tmp/task-1120-dla; L=/var/log/task-1120-dla
setsid nohup sh -c "cd $D/zt && nice make -j4; echo build-rc=\$?" > $L/build-old.log 2>&1 < /dev/null &
```

It must end `build-rc=0`; quote every `warning:` line outside objtool notes. Note S2=`modinfo -F srcversion $D/zt/module/zfs.ko`: it must differ from S1. Load: `sudo -n $D/zt/scripts/zfs.sh`, srcversion equal to S2, "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`. Run the pre-run checks, then:

1. Label of a detached device (tag o): `sudo -n sh /var/log/task-1120-dla/lbl.sh o`. Expected `b_tree='file' b_children=0`, `b_config_txg=0`, `b_ub_txg=0`, and B at or above A. Quote the line and `sed -n '/vdev_tree:/,/features_for_read:/p' $L/lbl-o-b.txt`.
2. The three tests once each, expected to FAIL (tags f2, f3, f4), after the pre-run checks:

   ```sh
   L=/var/log/task-1120-dla
   setsid nohup sh -c "sh $L/runs.sh f2 1 -t zhack_label_repair_002; sh $L/runs.sh f3 1 -t zhack_label_repair_003; sh $L/runs.sh f4 1 -t zhack_label_repair_004" > $L/runs-f.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE f4` or a `STOP` line. Expected for each: rc other than 0, the test line `[FAIL]`. For each run, with R its `dir=`, quote `grep -nE 'zhack label repair -u|cannot import|exited 26[0-9]|^ERROR|ERROR:' $R/log | head -n 20` and `grep -c 'missing top-level' $R/log`. The prediction to check: the failure is the `zpool import testpool -d <loop device>` right after a successful `zhack label repair -u` (in 002 the second one, after the `-c` attempt; in 003 the last one; in 004 the one after `-cu`), with `internal error: cannot import 'testpool': Value too large for defined data type` and exit 262. A FAIL elsewhere, another error, or a PASS: quote the whole $R/log of that run and say so plainly; it does not stop the task.

After them: the pre-run checks once more (the tests' cleanup detaches their loop devices and removes /var/tmp/disk and /var/tmp/mirrordisk even when they fail), and the dmesg check since N1 as in step 4 (none expected). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`.

## STEP 6: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n losetup -a` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/disk /var/tmp/mirrordisk /var/tmp/detach_* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line of step 0; TREE-OK, SHA-OK, diffstat and the two grep counts; SCRIPTS-OK; rc and warnings of both builds; S1, S2 and DEBUG mode; step 5's TREE-OK, sums and name-status; what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. Table: tag | tree (fix 62016569b1a or previous 8b14c89f5e7) | selection | runs | result per test (PASS count, or FAIL) | durations. Rows: n and o (the two `lbl.sh` lines in full), z, p5, p6, g, f2, f3, f4. Then one verdict line, e.g. "fix 62016569b1a: detached label = mirror before the detach (birth B < A); -T zhack 3/3 runs all PASS, 005 and 006 3/3, zpool_detach+replacement PASS; 8b14c89f5e7: label = remaining disk only, zhack_label_repair_002/003/004 FAIL at the import after -u (EOVERFLOW, exit 262): regression reproduced and fixed", or what differs.
3. Quotes: the START/DONE lines, the 005 txg NOTE lines, every non-PASS of the fix with its lines, each fail-before run's grep lines, the dmesg checks.
4. Deviations, cleanup, then End.
