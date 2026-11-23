TASK-1123-ROD3 (from the coordinator session "main"): functional, about 45 min with its build, at most 1 h 30, no reboot. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It checks the third commit of the fork's branch `zpool-destroy-readonly-v3` (upstream PR #20008, "Refuse to destroy a pool imported read-only"): a man page paragraph and a one-line change to the ZTS helper `destroy_pool`, which hundreds of test cleanups call. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rod3/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D=/var/tmp/task-1123-rod3`, `L=/var/log/task-1123-rod3`, `Z=$D/zt`).

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind. If a result surprises you, stop and report it with the outputs.

## Why

- **The PR:** `zpool destroy` of a pool imported with `-o readonly=on` fails ("pool is imported read-only"), where master exits 0 and writes nothing. The ZTS helper `destroy_pool` (tests/zfs-tests/include/libtest.shlib) therefore exports a read-only pool instead of destroying it. Its first two commits passed TASK-1118-ROD and TASK-1120-ROD2 (VM 106, 21/11): pyzfs_unittest 3/3 with no pool left, checkpoint_ro_rewind 3/3, `-T zpool_destroy,zpool_export` 18/18 three times, the four tests whose cleanup meets a read-only pool, and the pyzfs leftover fail-before.
- **Since ROD2:** the branch was rebased onto master `a1daebfec3f` (six commits after ROD2's `c913da86156`: CI, FreeBSD append-only, two ZTS fixes). The PR's own patch is unchanged, and step 1 checks that every file ROD2 checked has ROD2's sum, libtest.shlib apart.
- **The new commit `1f0738f4e1a`** "Document labelclear for read-only pools, fix destroy_pool's check":
  - `man/man8/zpool-destroy.8`: a pool that cannot be imported read-write can only be exported, and its devices then cleared with `zpool labelclear -f`. No code: not run here.
  - `destroy_pool` read the readonly property with `get_pool_prop readonly "$pool"` inside `$(...)`. If `zpool get` failed, `log_fail` ran the test's `log_onexit` teardown inside that subshell (`$$` is still the test's pid there, so `_endlog` does not see a subshell), its FAIL line went into the captured value, and the caller went on to the destroy. It now reads `zpool get -Hpo value readonly "$pool" 2>/dev/null`: if that fails, `destroy_pool` tries the destroy, which `log_must_busy` reports if it fails.
- **What this task runs**, on one debug build of the head: the PR's test 3 times; the groups `zpool_destroy`, `zpool_export`, `pool_checkpoint` and `pyzfs` once, with ROD2's leftover check after pyzfs; ROD2's four read-only cleanups once each; three groups whose tests call `destroy_pool` on many pools: `zpool_create` (36 of its 48 tests, several in loops over lists of pools), `block_cloning` (each of its 37 tests, in its cleanup) and `zpool_split` (each of its 9 tests). Each run line counts the `destroy_pool` destroys and exports it saw. Then step 5 makes `zpool get readonly` fail for one `destroy_pool` call, through a stub `zpool` in that call's PATH. No fail-before: the change is a test-library robustness fix and a man page.

## What is tested (fork https://github.com/tigerblue77/zfs)

| commit | sha | tree | what |
|---|---|---|---|
| HEAD | `1f0738f4e1a7f4c08700f09c3ecd366a0cf337af` | `456b28227a3999e035bf54526ef3728aaa9978a4` | branch `zpool-destroy-readonly-v3`: the new commit |
| HEAD~1 | `b04323d0db53cdbea1dfcb27a432e8f25aad54ef` | `092cafa743e161d67e45807b4af94bde818866ef` | upstream PR #20008's head: "Export the read-only pools that test cleanups destroyed" |
| HEAD~2 | `0c361bcbf89ef35f17858265f615d0571c549330` | | "Refuse to destroy a pool imported read-only" |
| HEAD~3 | `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` | | upstream master |

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; the pyzfs tests create their pool files as /var/tmp/tmp-*.zpool. Touch no block device of the VM. Never run `zpool import` yourself (`-a` least of all); never `zfs-tests.sh -x`.
- **Pools:** this task creates only the tests' own pools and, in step 5, `rod3stub` on a file under $D. A pool this task did not create (`tank` or any other) is a stop: report it and leave it as it is (no export, no destroy, no import).
- **Pre-run checks,** before every ZTS run and before step 5:
  - `lsmod | grep -E '^zfs '` shows this task's module;
  - `timeout 30 sudo -n $Z/zpool list -H -o name` shows no pool;
  - `sudo -n $Z/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared);
  - `sudo -n losetup -a` shows nothing;
  - `ls -d /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1` shows nothing;
  - `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one after a run is normal; a non-empty one names a pool left behind: stop and report its `ls -l`.
  - `runs.sh` repeats them (all but lsmod) before each of its runs, writes a `STOP` line if one fails, and records after each run the pools left (`after=[...]`).
- **Every build and every `runs.sh` series runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs:** a build still going after 60 minutes; a series whose $L/runs.txt has not changed for 30 minutes (the longest test here takes about 2 minutes); step 5's script still running when its `timeout` ends.
  - Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh|python'`; `timeout 5 sudo -n cat /proc/<pid>/stack` of every D task and of each `zpool` and `ksh` process; `sudo -n dmesg | tail -n 150`.
  - Then stop and report. This task pre-approves no reboot: a hang, a panic or a module that will not unload is captured as above, reported, and left for main.
- **Kernel log:** steps 4 and 5 each write a mark with `/dev/kmsg` and end with a check of the kernel lines since their mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock|segfault`. None are expected. Count the `WARNING` lines too and quote each with the 5 lines before it; pool suspend/resume ones are not a finding, and none are expected here.
- Run `sudo -n fstrim -av` after deleting trees or vdev files. Modules are unloaded at the end.

## Step 0: state

Check and quote:
- `date -u`, `hostname`, `uname -r`, `cat /etc/debian_version`, `uptime -s`, `nproc`, `free -m`;
- `df -h / /var/tmp`: at least 20 GiB free on /var/tmp, else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. `ls -l /dev/zfs 2>&1`: must not exist. `sudo -n losetup -a`: empty;
- `ls -d /var/tmp/task-1123-rod3 /var/log/task-1123-rod3 2>&1`: both absent;
- `ls -ld /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1`: none;
- `id zts`, `command -v ksh git`;
- `pgrep -a -x zed; pgrep -a -x lt-zed`: must be empty (a running zed is a stop; do not start or stop zed);
- `cat /proc/sys/kernel/hung_task_timeout_secs`: quote it; do not change it;
- `ls /usr/local/share/zfs/compatibility.d 2>/dev/null | wc -l`: 25 on VMs 105 and 106 since TASK-1118-CMP. Note it: with 0, `zpool_create_features_007_pos` and `_008_pos` are expected to FAIL in step 4 (they need those files), which is the environment, not a finding.

Then the Python pieces, read only (install nothing; quote every line):

```sh
python3 --version
python3 -c 'import cffi; print("cffi", cffi.__version__)'
python3 -c 'import setuptools; print("setuptools", setuptools.__version__)'
python3 -c 'import packaging; print("packaging", packaging.__version__)' || python3 -c 'import distlib; print("distlib", distlib.__version__)'
python3 -c 'import os, sysconfig; h = os.path.join(sysconfig.get_paths()["include"], "Python.h"); print(h, os.path.exists(h))'
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))'); echo "P=$P"; ls -ld $P $P/libzfs_core /etc/ld.so.conf.d/task-1123-rod3.conf 2>&1; /sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair' | grep -c .
```

Note PY, which steps 3 to 6 use: `all` when cffi, setuptools, packaging or distlib, and Python.h (`True`) are all there; `cffi` when cffi is there and one of the others is missing; `none` when cffi is missing. Expected: no `$P/libzfs_core`, no `task-1123-rod3.conf`, and the count `0`. A `libzfs_core` already in $P or a count above 0 is a stop: report it. Note whether $P itself exists (step 4 creates it when it does not).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1123-rod3
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 4 https://github.com/tigerblue77/zfs zpool-destroy-readonly-v3 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD HEAD~1 HEAD~2 HEAD~3 HEAD^{tree} HEAD~1^{tree} | tr '\n' ' ')$(git status --porcelain | wc -l)" > $L/tree.txt
echo "1f0738f4e1a7f4c08700f09c3ecd366a0cf337af b04323d0db53cdbea1dfcb27a432e8f25aad54ef 0c361bcbf89ef35f17858265f615d0571c549330 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 456b28227a3999e035bf54526ef3728aaa9978a4 092cafa743e161d67e45807b4af94bde818866ef 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
T=tests/zfs-tests/tests/functional
printf '%s\n' "298d9fc31865ac9b51ce7d1b88467006200183cb900b1ceac220409402ab23a4  module/zfs/zfs_ioctl.c" "e16ee6df6404c57dfbe3ec579c057eacbd835f74af400936d4d60ba835e4d4ad  cmd/zpool/zpool_main.c" "ede0342c611b90c7dc279db525909e99a1d6b61e094950a4f6df1838196d530b  lib/libzfs/libzfs_pool.c" "368a7742e09f9677a7af8171bcbb760d6f2ef237d7742c86d0f5bac30ab697a9  $T/cli_root/zpool_destroy/zpool_destroy_readonly_neg.ksh" "dcbe3a81a1bdeb3a788b3387818f7072a8f8607e2b03506fe2bcea7aa574ce11  $T/cli_root/zpool_export/zpool_export_readonly_pos.ksh" "93ad5d483988cebc5db5fbd8cd9bfc7633252459741c2553966ab1d9678540e6  tests/zfs-tests/include/libtest.shlib" "35c91ae312a191956e84a584db019a7fa498fe239a4f3e933d0855014e3e94e8  contrib/pyzfs/libzfs_core/test/test_libzfs_core.py" "94fc4aa574f5eff32e3e3f5b6e36a3bffbd71f93cf54eb82c1866dd3c90ebf66  scripts/zfs-tests.sh" "2790678d03c148b27c7c1ada464659ac80dda04c003718e4d5e7800ad97d1bc5  $T/pool_checkpoint/pool_checkpoint.kshlib" "5fdf0969c72a42928f97a8506da5f0559f794b346b975a3c1b24517cf389c88b  man/man8/zpool-destroy.8" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git show HEAD~1:tests/zfs-tests/include/libtest.shlib | sha256sum
git log -3 --format='%an | %s'
git diff --stat=80 HEAD~1 HEAD; git diff --quiet HEAD~1 HEAD -- module cmd lib include && echo CODE-SAME
```

Expected:
- TREE-OK.
- SHA-OK. The first five sums and the three after libtest.shlib are those of TASK-1120-ROD2 (the first five also those of TASK-1118-ROD); libtest.shlib and the man page are the new commit's.
- `b62ee0df2029b1328a1ac960385e7fa1573851c3708d3bb30db004d84189ddf8  -`: the parent's libtest.shlib is the one ROD2 tested.
- `Tigerblue77 | Document labelclear for read-only pools, fix destroy_pool's check`, `Tigerblue77 | Export the read-only pools that test cleanups destroyed`, `Tigerblue77 | Refuse to destroy a pool imported read-only`.
- The diffstat (another git version may draw the bars differently: other files or other counts are a stop, the bars are not):

  ```
   man/man8/zpool-destroy.8              | 12 +++++++++++-
   tests/zfs-tests/include/libtest.shlib |  5 ++++-
   2 files changed, 15 insertions(+), 2 deletions(-)
  ```

- CODE-SAME.

If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report the outputs as they are. Edit no file of the tree.

## Step 2: the files

Write the four files below, byte for byte (4-space indentation, no tab, a newline after the last line), each with `cat > <path> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), after `mkdir /var/tmp/task-1123-rod3/stub`.

`$L/runs.sh`: `runs.sh <tag> <runs> <selection>` runs as the login user. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks; each run's console goes to `$L/zts-<tag>-<n>.log`, and `$L/runs.txt` gets a line `<tag>-<n> rc=<rc> dir=<result dir> after=[<pools left>] destroy=<d> export=<x> romsg=<m>`, followed by that run's `Test:` lines (path from `functional/`, duration, result). In the run's log, `d` and `x` count the lines `SUCCESS: zpool destroy -f <pool>` and `SUCCESS: zpool export -f <pool>` (what `destroy_pool` prints, plus any such command a test runs itself; a refused one, `... exited 1`, is not counted), and `m` the lines `pool is imported read-only` (a destroy refused where it was not expected prints one).

```sh
#!/bin/sh
# TASK-1123-ROD3 ZTS runner, run as the login user: runs.sh <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1123-rod3
L=/var/log/task-1123-rod3
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
    if [ -n "$R" ] && [ -f "$R/log" ]; then
        cnt="destroy=$(grep -cE 'SUCCESS: zpool destroy -f [^ ]+$' $R/log)"
        cnt="$cnt export=$(grep -cE 'SUCCESS: zpool export -f [^ ]+$' $R/log)"
        cnt="$cnt romsg=$(grep -c 'pool is imported read-only' $R/log)"
    else
        cnt="nolog"
    fi
    echo "$tag-$n rc=$rc dir=$R after=[$after] $cnt" >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

`$D/stub/zpool`: refuses `zpool get ... readonly ...`, records the call, and hands every other command to the tree's zpool.

```sh
#!/bin/sh
# TASK-1123-ROD3 stub: "zpool get ... readonly ..." fails; anything else runs the tree's zpool
case " $* " in
*" get "*" readonly "*)
    echo "$*" >> /var/tmp/task-1123-rod3/stub-calls.txt
    echo "stub: zpool get readonly refused" >&2
    exit 1
    ;;
esac
exec /var/tmp/task-1123-rod3/zt/zpool "$@"
```

`$D/stub/zfs`:

```sh
#!/bin/sh
exec /var/tmp/task-1123-rod3/zt/zfs "$@"
```

`$L/stubcheck.ksh`: sources the tree's libtest.shlib with the stub directory first in PATH, sets a `log_onexit` teardown that would create `$D/stub-onexit`, and calls `destroy_pool` once.

```sh
#!/bin/ksh
# TASK-1123-ROD3: one destroy_pool call while "zpool get readonly" fails; run as root
D=/var/tmp/task-1123-rod3
export STF_SUITE=$D/zt/tests/zfs-tests STF_TOOLS=$D/zt/tests/test-runner
export STF_PATH=$D/stub:/usr/sbin:/usr/bin:/sbin:/bin
. $STF_SUITE/include/libtest.shlib
log_onexit touch $D/stub-onexit
cd /
destroy_pool rod3stub
echo "destroy_pool rc=$?"
```

Then:

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3
chmod 755 $D/stub/zpool $D/stub/zfs
printf '%s\n' "dfabba2b7cf228068571bf0ddd13903dcc089aa4e87272b39b1b3e7291135c7c  $L/runs.sh" "475c1c8cd4e63a384ae31bcc5f9d6d810ec7fb3565c89f42ad9a5a8c051c353d  $D/stub/zpool" "8aad29c2b34ec88873e10011abddba336e5e6ad01f5bf4f603638c57202c6b61  $D/stub/zfs" "a65afdf57ecce4ac3633599fc69d896861a654549db7251a79cad9fc36dce667  $L/stubcheck.ksh" | sha256sum -c && sh -n $L/runs.sh && sh -n $D/stub/zpool && sh -n $D/stub/zfs && ksh -n $L/stubcheck.ksh && echo FILES-OK
```

Expected four `OK` lines and FILES-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

## Step 3: debug build (about 5 to 10 minutes)

With PY `all`, as written; with PY `cffi` or `none`, without `--enable-pyzfs`:

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3
setsid nohup sh -c "date -u +%T; cd $D/zt && ./autogen.sh && ./configure --enable-debug --enable-pyzfs && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`. Quote the duration, `grep 'whether to enable pyzfs' $L/build.log` (`yes` with `--enable-pyzfs`) and every `warning:` line outside objtool notes.
- If configure stopped on a Python or pyzfs check although PY was `all`: quote `grep -n -i -E 'python|pyzfs' $L/build.log | tail -n 20`, run the same command once more without `--enable-pyzfs` (log $L/build2.log), and say so. The pyzfs run still takes place, since it needs only cffi.
- Any other build failure: stop and report.

Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`, and quote `ls -l $D/zt/.libs/libzfs_core.so.3 $D/zt/.libs/libnvpair.so.3` (two links to the versioned files).

## Step 4: the runs (about 25 minutes)

**Load:**
- `sudo -n $D/zt/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal S1, and the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- write the mark: `echo "task-1123-rod3: loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**The pyzfs pieces** (skip with PY `none`):

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')
test -d $P || { sudo -n mkdir -p $P && echo "$P" > $L/p-created && echo "P-CREATED $P"; }
sudo -n ln -s $D/zt/contrib/pyzfs/libzfs_core $P/libzfs_core && echo "$D/zt/.libs" | sudo -n tee /etc/ld.so.conf.d/task-1123-rod3.conf && sudo -n ldconfig && echo PLUMB-OK
/sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair'
cd / && sudo -n python3 -c 'import ctypes.util, libzfs_core, libzfs_core.test.test_libzfs_core; print(libzfs_core.__file__, ctypes.util.find_library("nvpair"), ctypes.util.find_library("zfs_core"), libzfs_core.lzc_exists(b"rod3-none"))'
```

Expected PLUMB-OK; ldconfig lines for `libzfs_core.so.3` and `libnvpair.so.3` (maybe also their unversioned `.so` links), all under $D/zt/.libs; then `<P>/libzfs_core/__init__.py`, the two library names and `False`. Any error: stop and report.

Run the pre-run checks, then:

1. **pyzfs** (tag `py`, 1 run; skip with PY `none`): the whole `pyzfs` group, which is this one test with no setup or cleanup.

   ```sh
   L=/var/log/task-1123-rod3
   setsid nohup sh $L/runs.sh py 1 -t tests/functional/pyzfs/pyzfs_unittest > $L/runs-py.nohup 2>&1 < /dev/null &
   ```

   (The path, not the name: the name would also match the executable `pyzfs_unittest.ksh.in`.) Poll `tail -n 3 $L/runs.txt` until a `DONE py` or a `STOP` line. Expected `py-1 rc=0 ... after=[] destroy=0 export=0 romsg=0` and `pyzfs/pyzfs_unittest ... [PASS]`. With R its `dir=`, quote `grep -E '^[0-9:.]+ (Ran [0-9]+ tests|OK|FAILED)' $R/log; grep -c 'command failed' $R/log` (expected `Ran <n> tests in <s>s`, `OK` with or without a skipped count, and `0`). Then ROD2's leftover check: `timeout 60 sudo -n $D/zt/zpool sync; echo "sync rc=$?"` (expected `sync rc=0`, no output: no read-only pool was left behind).

2. **The other runs,** in one series, after the pre-run checks:

   ```sh
   L=/var/log/task-1123-rod3; F=tests/functional
   setsid nohup sh -c "sh $L/runs.sh pr 3 -t $F/cli_root/zpool_destroy/zpool_destroy_readonly_neg && sh $L/runs.sh g 1 -T zpool_destroy,zpool_export && sh $L/runs.sh ck 1 -T pool_checkpoint && sh $L/runs.sh zc1 1 -t $F/channel_program/synctask_core/tst.readonly_pool && sh $L/runs.sh zc2 1 -t $F/cli_root/zpool_clear/zpool_clear_readonly && sh $L/runs.sh zc3 1 -t $F/cli_root/zpool_import/zpool_import_features_003_pos && sh $L/runs.sh zc4 1 -t $F/upgrade/upgrade_readonly_pool && sh $L/runs.sh cr 1 -T zpool_create && sh $L/runs.sh bc 1 -T block_cloning && sh $L/runs.sh sp 1 -T zpool_split" > $L/runs-b.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/runs.txt` every few minutes until `DONE sp` or a `STOP` line. Expected, every run with rc=0, `after=[]` and `romsg=0`:
   - **pr** (3 runs): `cli_root/zpool_destroy/zpool_destroy_readonly_neg` PASS, alone (its directory has no setup or cleanup). Expected `destroy=0 export=0`: the test destroys and exports its pool itself, without `-f`, so its cleanup's `destroy_pool` finds no pool.
   - **g:** in this order, all PASS: `zpool_destroy_001_pos`, `zpool_destroy_002_pos`, `zpool_destroy_003_neg`, `zpool_destroy_readonly_neg`, `zpool_export/setup`, the twelve `zpool_export_*` tests (`001_pos`, `002_pos`, `003_neg`, `004_pos`, `anyraid_001_pos`, `005_neg`, `parallel_pos`, `parallel_admin`, `readonly_pos`, `stats_pos`, `zvol_remove_pos`, `zvol_open_pos`), `zpool_export/cleanup`: 18 lines, as in ROD2. Say if the list or the order differs.
   - **ck:** `pool_checkpoint/setup`, the 20 tests of `[tests/functional/pool_checkpoint]` in tests/runfiles/common.run, in that order (`checkpoint_ro_rewind` among them), `pool_checkpoint/cleanup`: 22 lines, all PASS (about 4 minutes on these VMs).
   - **zc1 to zc4:** each test with its directory's setup and cleanup, all PASS. From each, with R its `dir=`, quote `grep -E 'zpool (export|destroy) -f [^ ]+$' $R/log`. Expected among them the `zpool export -f <pool>` SUCCESS line of the test's cleanup, `destroy_pool` exporting its read-only pool after reading `on` through the new line: `cp_readonly`, `testpool1`, `testpool1`, `testpool`, as in ROD2 (so `export=1` each). A `zpool destroy -f` of that read-only pool instead is a deviation to report; the other `zpool destroy -f` lines are the read-write pools.
   - **cr:** `zpool_create/setup`, the 48 tests of `[tests/functional/cli_root/zpool_create]` in common.run's order, `zpool_create/cleanup`: 50 lines, all PASS (about 7 minutes), `zpool_create_features_007_pos` and `_008_pos` apart on a VM whose step 0 count was 0.
   - **bc:** `block_cloning/setup`, the 18 tests of `[tests/functional/block_cloning]` (common.run), cleanup, then setup, the 19 of `[tests/functional/block_cloning:Linux]` (linux.run), cleanup: 41 lines, all PASS (about 2 minutes).
   - **sp:** `zpool_split/setup`, the 8 tests of `[tests/functional/cli_root/zpool_split]`, cleanup, then setup, `zpool_split_wholedisk`, cleanup: 13 lines, all PASS (about 1 minute).
   - `destroy` above 0 on g, ck, zc1 to zc3, cr, bc and sp; `destroy=0` on zc4, whose only pool is the read-only one. Quote each run's counts as they come.

Then quote:

```sh
L=/var/log/task-1123-rod3
grep -E '^(START|DONE|STOP)' $L/runs.txt; cat $L/runs-*.nohup
grep -E '^[a-z0-9]+-[0-9]+ rc=' $L/runs.txt
grep -cE '\[PASS\]$' $L/runs.txt; grep -E '\[(FAIL|KILLED|SKIP)\]$' $L/runs.txt
```

Expected no STOP, empty nohup files, every rc=0, `after=[]` and `romsg=0`, 160 PASS lines (159 with PY `none`), and no FAIL, KILLED or SKIP line (the two compatibility tests apart, as above). Quote the `Test:` lines of py, pr, g and zc1 to zc4 in full (they carry the durations); for ck, cr, bc and sp, the three longest of each, with their durations (all of them stay in $L/runs.txt).
- **A non-PASS test:** quote its line and, with R its `dir=`, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:' $R/log` around its failure; say whether tests/test-runner/bin/zts-report.py.in lists it as known. Go on with the plan without rerunning it; it does not stop the task.
- **A `romsg` above 0:** quote `grep -n -B 8 'pool is imported read-only' $R/log` of that run.
- **A pool left after a run** (`after=` not empty) is a stop: report its `zpool status -v` and leave it.

**Kernel lines since the mark:**

```sh
L=/var/log/task-1123-rod3
sudo -n dmesg | sed -n '/task-1123-rod3: loaded$/,$p' > $L/dmesg-4.txt; head -n 1 $L/dmesg-4.txt; wc -l < $L/dmesg-4.txt
grep -n -E 'VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more than|possible circular|deadlock|segfault' $L/dmesg-4.txt; grep -c WARNING $L/dmesg-4.txt
```

Expected the mark as first line, no matching line, and `0`. If the file is empty, the mark has left the ring buffer: say so, and grep `sudo -n dmesg` whole instead.

Keep the module loaded for step 5.

## Step 5: destroy_pool when zpool get fails (about 2 minutes)

The pre-run checks first (all clean). Then make the pool and check the stub by hand:

```sh
D=/var/tmp/task-1123-rod3; Z=$D/zt
echo "task-1123-rod3: stub check" | sudo -n tee /dev/kmsg > /dev/null
truncate -s 128M $D/stub-vdev && sudo -n $Z/zpool create -o cachefile=none -m none rod3stub $D/stub-vdev && echo CREATED
timeout 30 sudo -n $Z/zpool get -H -o name,property,value readonly,cachefile,health rod3stub
timeout 30 sudo -n $D/stub/zpool get -Hpo value readonly rod3stub; echo "stub-get rc=$?"
timeout 30 sudo -n $D/stub/zpool list -H -o name; cat $D/stub-calls.txt; rm -f $D/stub-calls.txt
```

Expected CREATED; `rod3stub readonly off`, `rod3stub cachefile none`, `rod3stub health ONLINE`; `stub: zpool get readonly refused` and `stub-get rc=1`; `rod3stub` (the stub hands `list` to the tree's zpool); `get -Hpo value readonly rod3stub`. Any difference: destroy the pool with `timeout 60 sudo -n $Z/zpool destroy rod3stub`, stop and report.

Then the call:

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3; Z=$D/zt
timeout 300 sudo -n ksh $L/stubcheck.ksh > $L/stubcheck.out 2>&1; echo "rc=$?"; cat $L/stubcheck.out
cat $D/stub-calls.txt; ls -l $D/stub-onexit 2>&1
timeout 30 sudo -n $Z/zpool list -H -o name; ls -l /etc/zfs/zpool.cache 2>&1
```

Expected, in order:
- `rc=0`, then exactly two lines, `SUCCESS: zpool destroy -f rod3stub` and `destroy_pool rc=0`: the readonly property could not be read, so `destroy_pool` destroyed the pool, silently and without failing the test;
- `get -Hpo value readonly rod3stub`, one line: `destroy_pool` asked once, and the stub refused;
- `ls: cannot access '/var/tmp/task-1123-rod3/stub-onexit': No such file or directory`: the `log_onexit` teardown did not run. With the parent's `get_pool_prop` line it ran inside `$(...)` and created that file; main checked that with fake `zpool` and `zfs` commands, so it is not repeated here;
- no pool; zpool.cache as before step 5 (absent or 0 bytes: the pool had `cachefile=none`).

An `ERROR:` or `Performing local cleanup` line, the file `stub-onexit`, or an rc other than 0 is the finding: quote everything above and say so plainly. If `rod3stub` is still there, destroy it with `timeout 60 sudo -n $Z/zpool destroy rod3stub; echo "destroy rc=$?"` and report.

Then:
- `rm -f /var/tmp/task-1123-rod3/stub-vdev`;
- the pre-run checks once more (all clean);
- the kernel lines since the second mark: the commands of step 4 with `task-1123-rod3: stub check$` as the mark and `$L/dmesg-5.txt` as the file; same expectations;
- unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 6: cleanup

The pyzfs pieces (skip with PY `none`):

```sh
D=/var/tmp/task-1123-rod3; L=/var/log/task-1123-rod3
P=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))')
sudo -n rm -f $P/libzfs_core /etc/ld.so.conf.d/task-1123-rod3.conf && sudo -n ldconfig && echo UNPLUMB-OK
test -e $L/p-created && sudo -n rmdir $P && echo "P-REMOVED $P"
ls -ld $P/libzfs_core /etc/ld.so.conf.d/task-1123-rod3.conf 2>&1; /sbin/ldconfig -p | grep -E 'libzfs_core|libnvpair' | grep -c .
```

Expected UNPLUMB-OK, P-REMOVED only if step 4 created $P, both paths absent, and the count `0`.

Then:
- `lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n losetup -a` (empty);
- `sudo -n rm -rf /var/tmp/task-1123-rod3`, `sudo -n fstrim -av`;
- `ls -d /var/tmp/file-* /var/tmp/dev_destroy-readonly-test /var/tmp/dev_export-readonly-test /var/tmp/destroy-readonly-mnt /var/tmp/export-readonly-mnt /var/tmp/tmp-*.zpool* 2>&1` (none), `df -h /var/tmp`;
- `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it.

Logs and the files of step 2 stay in $L (runs.sh and stubcheck.ksh; the stubs go with $D); the ZTS results stay under /var/tmp/test_results.

## Report

Write `/var/log/task-1123-rod3/report.md` and give the same text as your final message:

1. **Environment:** VM, kernel, Debian version, nproc, RAM, free space, hung_task_timeout_secs, the zed line, the compatibility.d count; the Python version and pieces, PY, whether $P existed; TREE-OK, SHA-OK, the parent's libtest.shlib sum, the three log lines, the diffstat, CODE-SAME; FILES-OK; the configure options, the pyzfs configure line, build rc, duration and warnings; S1 and "(DEBUG mode)"; the plumbing lines; what happened to /etc/zfs/zpool.cache at steps 0 and 6.
2. **Table:** tag | selection | runs | result per test (PASS count, or each non-PASS) | duration (from START/DONE) | destroy / export / romsg | pools after. Rows py, pr, g, ck, zc1 to zc4, cr, bc, sp, then step 5 (`destroy_pool rod3stub` with `zpool get readonly` refused | rc | lines | stub calls | stub-onexit). Then one verdict line, for example: "1f0738f4e1a: zpool_destroy_readonly_neg 3/3; zpool_destroy+zpool_export 18/18; pool_checkpoint 22/22; pyzfs 1/1, no pool left, zpool sync rc 0; read-only cleanups 4/4, each exported by destroy_pool; zpool_create 50/50, block_cloning 41/41, zpool_split 13/13; <total> destroy_pool destroys and <total> exports counted, romsg 0 everywhere; with zpool get refused, destroy_pool destroyed the pool, rc 0, no teardown run; kernel lines clean", or what differs.
3. **Quotes:** the START/DONE lines; every run line with its counts; py's `Ran`/`OK` lines, `command failed` count and the sync line; the four zc `zpool export -f` lines; every non-PASS with its lines; step 5's outputs in full; the kernel-line checks.
4. **Deviations, cleanup,** then `End`.
