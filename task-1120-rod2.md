TASK-1120-ROD2 (from the coordinator session "main"): functional, about 1 h, debug build only (one tree, no rebuild), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The script of step 2 contains all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-rod2`, `L=/var/log/task-1120-rod2`, `Z=$D/zt`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

The fork's branch `zpool-destroy-readonly-v2` ("Refuse to destroy a pool imported read-only") makes `zpool destroy` of a pool imported with `-o readonly=on` fail, where master exits 0 and writes nothing, so that the "destroyed" pool can be imported again by name: the kernel registers ZFS_IOC_POOL_DESTROY with POOL_CHECK_READONLY (EROFS before the handler runs), `zpool destroy` refuses before it unmounts anything, libzfs reports "pool is read-only", and the ZTS helper `destroy_pool` exports a read-only pool instead of destroying it. Its first commit `2d23f6866e8` passed TASK-1118-ROD (VM 106, 18/11): `-T zpool_destroy,zpool_export` 3/3 debug and 1/1 release, the four tests whose cleanup meets a read-only pool, the fail-before on master's code, and the refusal by hand from both userlands.

The fork's CI then found what those runs did not cover. The pyzfs tests import one pool read-only (`ZFSTest.readonly_pool`), and `_TempPool.cleanUp()` destroyed it with `zpool destroy -f`. With the refusal the pool stays imported read-only: `cleanUp()` swallowed the error and removed the pool's file, and every later bare `zpool sync` on the machine fails with "pool is read-only". `refquota_007_neg` failed that way, through `sync_all_pools`, in a FreeBSD job after `pyzfs_unittest` had passed.

The second commit `f592d04abdb` "Export the read-only pools that test cleanups destroyed" changes tests and scripts only. `_TempPool._destroy()` exports a pool it imported read-only (`zpool export -f`, as `destroy_pool` does); `cleanUp()` removes the pool's files and then raises when that fails, so a pool left behind fails `pyzfs_unittest` itself instead of a later test; `ZFSTest._cleanUp()` cleans every pool before it raises; the error paths of `setUpClass` and of the `_TempPool` constructor pass `check=False`. `cleanup_test_pool` (pool_checkpoint.kshlib) cleans up with `destroy_pool` instead of `zpool destroy`, for `checkpoint_ro_rewind` failing while its pool is read-only. `zfs-tests.sh -x` exports a leftover test pool imported read-only instead of destroying it (not run here: `-x` is never used on these VMs).

This task runs, on one debug build of the head: `pyzfs/pyzfs_unittest` 3 times, each followed by the check that no `pool.*` is left and a bare `zpool sync`; `pool_checkpoint/checkpoint_ro_rewind` 3 times; TASK-1118-ROD's groups `-T zpool_destroy,zpool_export` 3 times; the four read-only cleanups once each. Then the fail-before of the pyzfs leftover, which needs no rebuild: with the first commit's `test_libzfs_core.py`, `pyzfs_unittest` once, expected to PASS and to leave one `pool.<uuid>` imported read-only, on which a bare `zpool sync` fails; step 5 then exports that pool.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zpool-destroy-readonly-v2`, head `f592d04abdb3e32f071ff77c712aa57300660d83`, tree `872a135f6c16569f3d061e91241e348fae582eff`, on `2d23f6866e84379b91d8d5dfe71bf89d470ba296` (tree `8cb67a0c7c0290b762d75ec3ae5dec883042d19b`, the one TASK-1118-ROD tested), on upstream master `c913da86156eba3243cc1ef515e98d69887654a7`. The second commit touches no compiled file: the module, libzfs and zpool are byte for byte those of TASK-1118-ROD (step 1 checks it), so its release build and its checks by hand (KPATH-A, KPATH-B, DEMO-M) are not repeated.

pyzfs from a source tree: the test runs `python3 -m unittest libzfs_core.test...` as root, from the results directory, so the package `libzfs_core` must be on root's `sys.path`, and its cffi bindings open `libnvpair` and `libzfs_core` by name, through `ctypes.util.find_library()`, which reads the ldconfig cache. At run time it needs the Python module `cffi`; `configure --enable-pyzfs` also needs `setuptools`, `packaging` (or `distlib`) and the Python headers, and fails without them. Upstream's in-tree route is `zfs-helpers.sh -i`, which also installs a mount helper and udev rules system-wide (these VMs keep their own `zvol_id` and `60-zvol.rules` since TASK-1117-UDV). Step 4 installs only the two pieces pyzfs needs: a symlink `libzfs_core` in Python's `purelib` directory, and `/etc/ld.so.conf.d/task-1120-rod2.conf` naming the tree's `.libs`; step 6 removes both. Install no package: if `cffi` is missing, the pyzfs runs (step 4.1 and step 5) are skipped, and the report says so.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; the pyzfs tests create their pool files as /var/tmp/tmp-*.zpool. Touch no block device of the VM. Never run `zpool import` yourself (`-a` least of all; the tests import their own pools, which is expected); never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the tests' own pools; the one it handles itself is the `pool.<uuid>` that step 5's fail-before leaves imported read-only, which step 5 exports.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `sudo -n $D/zt/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. `runs.sh` repeats these checks (all but lsmod) before each of its runs, writes a `STOP` line if one fails, and records after each run the pools left (`after=[...]`).
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a series whose $L/runs.txt has not changed for 30 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|python'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `cat /etc/debian_version`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1` (none), `id zts`, and `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed).

Then the Python pieces, read only (install nothing; quote every line):

```sh
python3 --version
python3 -c 'import cffi; print("cffi", cffi.__version__)'
python3 -c 'import setuptools; print("setuptools", setuptools.__version__)'
python3 -c 'import packaging; print("packaging", packaging.__version__)' || python3 -c 'import distlib; print("distlib", distlib.__version__)'
python3 -c 'import os, sysconfig; h = os.path.join(sysconfig.get_paths()["include"], "Python.h"); print(h, os.path.exists(h))'
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))'); echo "P=$P"; ls -ld $P $P/libzfs_core /etc/ld.so.conf.d/task-1120-rod2.conf 2>&1; /sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair' | grep -c .
```

Note PY, which steps 3 to 6 use: `all` when cffi, setuptools, packaging or distlib, and Python.h (`True`) are all there; `cffi` when cffi is there and one of the others is missing; `none` when cffi is missing. Expected: no `$P/libzfs_core`, no `task-1120-rod2.conf`, and the count `0` (no libzfs_core or libnvpair in the ldconfig cache). A `libzfs_core` already in $P or a count above 0 is a stop: report it. Note whether $P itself exists (step 4 creates it when it does not).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-rod2
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-rod2; L=/var/log/task-1120-rod2
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 3 https://github.com/tigerblue77/zfs zpool-destroy-readonly-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD HEAD~1 HEAD~2 HEAD^{tree} HEAD~1^{tree} | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree.txt
echo "f592d04abdb3e32f071ff77c712aa57300660d83 2d23f6866e84379b91d8d5dfe71bf89d470ba296 c913da86156eba3243cc1ef515e98d69887654a7 872a135f6c16569f3d061e91241e348fae582eff 8cb67a0c7c0290b762d75ec3ae5dec883042d19b 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional
printf '%s\n' "298d9fc31865ac9b51ce7d1b88467006200183cb900b1ceac220409402ab23a4  module/zfs/zfs_ioctl.c" "e16ee6df6404c57dfbe3ec579c057eacbd835f74af400936d4d60ba835e4d4ad  cmd/zpool/zpool_main.c" "ede0342c611b90c7dc279db525909e99a1d6b61e094950a4f6df1838196d530b  lib/libzfs/libzfs_pool.c" "368a7742e09f9677a7af8171bcbb760d6f2ef237d7742c86d0f5bac30ab697a9  $T/cli_root/zpool_destroy/zpool_destroy_readonly_neg.ksh" "dcbe3a81a1bdeb3a788b3387818f7072a8f8607e2b03506fe2bcea7aa574ce11  $T/cli_root/zpool_export/zpool_export_readonly_pos.ksh" "b62ee0df2029b1328a1ac960385e7fa1573851c3708d3bb30db004d84189ddf8  tests/zfs-tests/include/libtest.shlib" "35c91ae312a191956e84a584db019a7fa498fe239a4f3e933d0855014e3e94e8  contrib/pyzfs/libzfs_core/test/test_libzfs_core.py" "94fc4aa574f5eff32e3e3f5b6e36a3bffbd71f93cf54eb82c1866dd3c90ebf66  scripts/zfs-tests.sh" "2790678d03c148b27c7c1ada464659ac80dda04c003718e4d5e7800ad97d1bc5  $T/pool_checkpoint/pool_checkpoint.kshlib" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git show HEAD~1:contrib/pyzfs/libzfs_core/test/test_libzfs_core.py | sha256sum
git diff --stat=80 HEAD~1 HEAD; git diff --quiet HEAD~1 HEAD -- module cmd lib include && echo CODE-SAME
```

Expected TREE-OK, SHA-OK (the first six sums are those of TASK-1118-ROD), `822be346d7df1d3b2f5a82667f3ba4849a0bd1c7e32ae763a8dc97378380d674  -` (the first commit's pyzfs test, which step 5 checks out), the diffstat

```
 contrib/pyzfs/libzfs_core/test/test_libzfs_core.py | 39 +++++++++++++++-------
 scripts/zfs-tests.sh                               | 10 +++++-
 .../pool_checkpoint/pool_checkpoint.kshlib         |  2 +-
 3 files changed, 37 insertions(+), 14 deletions(-)
```

(another git version may draw the bars differently: other files or other counts are a stop, the bars are not), and CODE-SAME. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree before step 5.

## STEP 2: the script

Write the script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > $L/runs.sh <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal). Then:

```sh
L=/var/log/task-1120-rod2
printf '%s\n' "c0b032868e3d7cc7aafa9aaddf518aa39f25ae9a2c91f37227eac69515ca1f98  $L/runs.sh" | sha256sum -c && sh -n $L/runs.sh && echo SCRIPT-OK
```

Expected one `OK` line and SCRIPT-OK. If the sum differs, write the file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tag> <runs> <selection>` runs as the login user. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks; each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir> after=[<pools left after the run>]` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1120-ROD2 ZTS runner, run as the login user: runs.sh <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1120-rod2
L=/var/log/task-1120-rod2
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
    left=$(ls -d /var/tmp/file-* /var/tmp/dev_destroy-readonly-test \
        /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt \
        /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>/dev/null)
    if [ -n "$pools" ] || [ "$inj" != "No handlers registered." ] ||
        [ -n "$loops" ] || [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: pools=[$pools] zinject=[$inj] loops=[$loops] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO "$@" \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    after=$(timeout 30 sudo -n $D/zt/zpool list -H -o name 2>&1 |
        paste -s -d ' ' -)
    echo "$tag-$n rc=$rc dir=$R after=[$after]" >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

## STEP 3: debug build (about 12 minutes)

With PY `all`, as written; with PY `cffi` or `none`, without `--enable-pyzfs`:

```sh
D=/var/tmp/task-1120-rod2; L=/var/log/task-1120-rod2
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug --enable-pyzfs && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote `grep 'whether to enable pyzfs' $L/build.log` (`yes` with `--enable-pyzfs`) and every `warning:` line outside objtool notes. If configure stopped on a Python or pyzfs check although PY was `all`, quote `grep -n -i -E 'python|pyzfs' $L/build.log | tail -n 20`, run the same command once more without `--enable-pyzfs` (log $L/build2.log), and say so: the pyzfs runs still take place, since they need only cffi. Any other build failure: stop and report. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`, and quote `ls -l $D/zt/.libs/libzfs_core.so.3 $D/zt/.libs/libnvpair.so.3` (two links to the versioned files).

## STEP 4: the head (tags py, ck, g, zc1 to zc4)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

The pyzfs pieces (skip with PY `none`):

```sh
D=/var/tmp/task-1120-rod2; L=/var/log/task-1120-rod2
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')
test -d $P || { sudo -n mkdir -p $P && echo "$P" > $L/p-created && echo "P-CREATED $P"; }
sudo -n ln -s $D/zt/contrib/pyzfs/libzfs_core $P/libzfs_core && echo "$D/zt/.libs" | sudo -n tee /etc/ld.so.conf.d/task-1120-rod2.conf && sudo -n ldconfig && echo PLUMB-OK
/sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair'
cd / && sudo -n python3 -c 'import ctypes.util, libzfs_core, libzfs_core.test.test_libzfs_core; print(libzfs_core.__file__, ctypes.util.find_library("nvpair"), ctypes.util.find_library("zfs_core"), libzfs_core.lzc_exists(b"rod2-none"))'
```

Expected PLUMB-OK; ldconfig lines for `libzfs_core.so.3` and `libnvpair.so.3` (maybe also their unversioned `.so` links), all under $D/zt/.libs; then `<P>/libzfs_core/__init__.py`, the two library names and `False` (the module answers: no such dataset). Any error: stop and report.

Run the pre-run checks of the VM rules, then:

1. pyzfs, 3 runs (tag py; skip with PY `none`):

   ```sh
   L=/var/log/task-1120-rod2
   setsid nohup sh $L/runs.sh py 3 -t tests/functional/pyzfs/pyzfs_unittest > $L/runs-py.nohup 2>&1 < /dev/null &
   ```

   (The path, not the name: the name would also match the executable `pyzfs_unittest.ksh.in`.) Poll `tail -n 3 $L/runs.txt` until a `DONE py` or a `STOP` line. Expected 3 runs with rc=0, each `pyzfs/pyzfs_unittest ... [PASS]` and `after=[]`. For each run, with R its `dir=`, quote `grep -E '^[0-9:.]+ (Ran [0-9]+ tests|OK|FAILED)' $R/log; grep -c 'command failed' $R/log` (expected `Ran <n> tests in <s>s`, `OK` with or without a skipped count, and `0`). After the series: `timeout 60 sudo -n $D/zt/zpool sync; echo "sync rc=$?"` (expected `sync rc=0`, no output).
2. `checkpoint_ro_rewind` 3 times (tag ck), the groups 3 times (tag g), then the four read-only cleanups once each (tags zc1 to zc4), after the pre-run checks, in one series:

   ```sh
   L=/var/log/task-1120-rod2; F=tests/functional
   setsid nohup sh -c "sh $L/runs.sh ck 3 -t $F/pool_checkpoint/checkpoint_ro_rewind && sh $L/runs.sh g 3 -T zpool_destroy,zpool_export && sh $L/runs.sh zc1 1 -t $F/channel_program/synctask_core/tst.readonly_pool && sh $L/runs.sh zc2 1 -t $F/cli_root/zpool_clear/zpool_clear_readonly && sh $L/runs.sh zc3 1 -t $F/cli_root/zpool_import/zpool_import_features_003_pos && sh $L/runs.sh zc4 1 -t $F/upgrade/upgrade_readonly_pool" > $L/runs-b.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE zc4` or a `STOP` line. Expected, every run with rc=0 and `after=[]`:
   - ck: `pool_checkpoint/setup`, `checkpoint_ro_rewind`, `pool_checkpoint/cleanup`, all PASS. For the first ck run, quote `grep -nE 'readonly=on|zpool (destroy|export) -f testpool$' $R/log`: the `zpool import -o readonly=on --rewind-to-checkpoint testpool` SUCCESS line, then in the test's cleanup `zpool destroy -f testpool` (the pool is read-write again by then; the read-only branch of `cleanup_test_pool` is reached only when the test fails in between, which this task does not provoke).
   - g: per run, in this order, all PASS: `zpool_destroy_001_pos`, `zpool_destroy_002_pos`, `zpool_destroy_003_neg`, `zpool_destroy_readonly_neg`, `zpool_export/setup`, the twelve `zpool_export_*` tests (`001_pos`, `002_pos`, `003_neg`, `004_pos`, `anyraid_001_pos`, `005_neg`, `parallel_pos`, `parallel_admin`, `readonly_pos`, `stats_pos`, `zvol_remove_pos`, `zvol_open_pos`), `zpool_export/cleanup`: 18 lines, as in TASK-1118-ROD. Say if the list or the order differs. For the first g run, quote `grep -nE 'zpool destroy (-f )?testpool1|read-only|Export it' $R/log | head -n 12`.
   - zc1 to zc4: each test with its directory's setup and cleanup, all PASS. From each, quote `grep -E 'zpool (export|destroy) -f' $R/log`: the `zpool export -f <pool>` SUCCESS line of the test's cleanup (`destroy_pool` exporting its read-only pool: `cp_readonly`, `testpool1`, `testpool1`, `testpool`, as in TASK-1118-ROD); a `zpool destroy -f` of that read-only pool instead is a deviation to report.

Then quote:

```sh
L=/var/log/task-1120-rod2
grep -E '^(START|DONE|STOP)' $L/runs.txt; cat $L/runs-*.nohup
grep -E '^[a-z0-9]+-[0-9]+ rc=' $L/runs.txt
grep -cE '\[PASS\]$' $L/runs.txt; grep -E '\[(FAIL|KILLED|SKIP)\]$' $L/runs.txt
```

Expected no STOP, empty nohup files, every rc=0 and `after=[]`, 78 PASS lines (75 with PY `none`), and no FAIL, KILLED or SKIP line. Quote every `Test:` line of $L/runs.txt (they carry the durations). A non-PASS test: quote its line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:' $R/log` around its failure (R its `dir=`), say whether tests/test-runner/bin/zts-report.py.in lists it as known, and go on with the plan without rerunning it; it does not stop the task. A pool left after a run (`after=` not empty) is a stop: report its `zpool status -v` and leave it.

After them: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than|segfault` quoted with 10 lines before it (none expected). With PY `none`, unload now with `sudo -n $D/zt/scripts/zfs.sh -u` and go to step 6; otherwise keep the module loaded.

## STEP 5: fail-before of the pyzfs leftover (tag fb; skip with PY `none`)

The first commit's pyzfs test, read through the symlink of step 4 (Python: no rebuild):

```sh
cd /var/tmp/task-1120-rod2/zt && git checkout HEAD~1 -- contrib/pyzfs/libzfs_core/test/test_libzfs_core.py && sha256sum contrib/pyzfs/libzfs_core/test/test_libzfs_core.py && git diff --cached --name-status HEAD
```

Expected `822be346d7df1d3b2f5a82667f3ba4849a0bd1c7e32ae763a8dc97378380d674  contrib/pyzfs/libzfs_core/test/test_libzfs_core.py` and one line, `M`, a tab and that file. Any difference: stop and report.

The pre-run checks, then:

```sh
L=/var/log/task-1120-rod2
setsid nohup sh $L/runs.sh fb 1 -t tests/functional/pyzfs/pyzfs_unittest > $L/runs-fb.nohup 2>&1 < /dev/null &
```

Poll until `DONE fb` or a `STOP` line. Expected `fb-1 rc=0 ... after=[pool.<uuid>]` and `pyzfs/pyzfs_unittest ... [PASS]`: the old `cleanUp()` swallows the refused destroy. Then, with R the `dir=` of fb-1:

```sh
D=/var/tmp/task-1120-rod2; L=/var/log/task-1120-rod2; Z=$D/zt; R=<dir of fb-1>
grep -E '^[0-9:.]+ (Ran [0-9]+ tests|OK|FAILED)' $R/log; grep -n 'command failed' $R/log
p=$(timeout 30 sudo -n $Z/zpool list -H -o name | grep -E '^pool\.'); echo "left=[$p]"
timeout 30 sudo -n $Z/zpool get -H -o name,property,value readonly,health "$p"
timeout 30 sudo -n $Z/zpool status -P "$p" | grep -E 'state:|tmp-'; ls -l /var/tmp/tmp-*.zpool* 2>&1
timeout 60 sudo -n $Z/zpool sync; echo "sync rc=$?"
timeout 60 sudo -n $Z/zpool export -f "$p"; echo "export rc=$?"
timeout 30 sudo -n $Z/zpool list -H -o name; ls -l /etc/zfs/zpool.cache 2>&1
```

Expected, in order: `Ran <n> tests ...` and `OK`; at least one `command failed:  b"cannot destroy 'pool.<uuid>': pool is imported read-only\nExport it, then import it read-write to destroy it.\n"`; `left=[pool.<uuid>]` (one name); `readonly` `on` and `health` `ONLINE`; `state: ONLINE` and the vdev `/var/tmp/tmp-<x>.zpool`, whose file no longer exists (`ls` finds nothing: `cleanUp()` removed it while the pool held it open); `sync 'pool.<uuid>' failed: pool is read-only` and `sync rc=1` (what `refquota_007_neg` met in CI); `export rc=0`; no pool; zpool.cache absent or of size 0. A PASS with no pool left, a FAIL, or another error: quote the whole `$R/log` of the run and say so plainly; it does not stop the task, but export a `pool.*` left behind only as above.

After it: the pre-run checks once more (all clean), the dmesg check since N0 as in step 4 (none expected), and unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## STEP 6: cleanup

The pyzfs pieces (skip with PY `none`):

```sh
D=/var/tmp/task-1120-rod2; L=/var/log/task-1120-rod2
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')
sudo -n rm -f $P/libzfs_core /etc/ld.so.conf.d/task-1120-rod2.conf && sudo -n ldconfig && echo UNPLUMB-OK
test -e $L/p-created && sudo -n rmdir $P && echo "P-REMOVED $P"
ls -ld $P/libzfs_core /etc/ld.so.conf.d/task-1120-rod2.conf 2>&1; /sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair' | grep -c .
```

Expected UNPLUMB-OK, P-REMOVED only if step 4 created $P, both paths absent, and the count `0`.

Then `lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n losetup -a` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1` (none), `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and the script stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed line of step 0; Python version, each Python piece of step 0 (version or missing), PY, whether $P existed; TREE-OK, SHA-OK, the old test's sum, the diffstat, CODE-SAME; SCRIPT-OK; configure options, the pyzfs configure line, build rc and warnings; S1 and DEBUG mode; the plumbing lines of step 4; step 5's sum and name-status; what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. Table: tag | test file (head f592d04abdb, or fb: the head with 2d23f6866e8's test_libzfs_core.py) | selection | runs | result per test (PASS count, or FAIL) | durations | pools after. Rows: py, ck, g, zc1 to zc4, fb. Then one verdict line, e.g. "f592d04abdb: pyzfs_unittest 3/3 PASS, no pool left, zpool sync rc 0; checkpoint_ro_rewind 3/3; zpool_destroy+zpool_export 18/18 x3; read-only cleanups 4/4; fail-before with 2d23f6866e8's pyzfs test: PASS but pool.<uuid> left imported read-only, bare zpool sync fails 'pool is read-only': leftover reproduced and fixed", or what differs.
3. Quotes: the START/DONE lines; py's `Ran`/`OK` lines, `command failed` counts and the sync line; ck's lines; g's zpool_destroy_readonly_neg lines; the four zc `zpool export -f` lines; every non-PASS with its lines; fb's lines; the dmesg checks.
4. Deviations, cleanup, then End.
