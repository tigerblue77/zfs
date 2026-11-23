TASK-1123-ZTK (from the coordinator session "main"): functional, about 1 h with its builds, at most 2 h. No reboot is expected and none is approved. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It gives a pull request to openzfs/zfs, not opened yet ("ZTS: make zfs-tests.sh -t NAME select the test of that name"), the real ZTS run it lacks: its new test `test_runner_by_name` has only been run directly with ksh, in a container that cannot load modules. The fix with its test, against master plus the test alone. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-ztk/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## Why

- **The bug:** `scripts/zfs-tests.sh -t NAME`, given a name rather than a path, runs the first path under the test suite whose name starts with NAME: `find "$STF_SUITE" -name "$SINGLETEST*" -print -quit`. `find` meets paths in the directory order of the file system, and many tests share the start of their name with something that is not that test: a helper (`cmd/mmap_ftruncate.c`, or `bin/mmap_ftruncate` once installed), a directory (`cli_root/zfs_rewrite`, `functional/mmap`), a data file (`tst.args_to_lua.out`), an object file (`many_fds.o`) or another test. A directory or a data file makes test-runner reject the runfile (`... is not an executable regular file.`), so nothing runs; an installed helper is executable, so test-runner runs it instead of the test.
- **The fix** (`scripts/zfs-tests.sh` only): for a name with no `/`, search `$STF_SUITE/tests` only; first a regular file named `NAME` or `NAME.ksh`, skipping `.libs`; only if there is none, the first regular file named `NAME*.ksh`. A path given to `-t` is used as it is, as before.
- **The test** `test_runner_by_name` (group `test_runner`, common runfile): it builds a small fake suite under `$TEST_BASE_DIR` (`/var/tmp/test-runner-by-name.XXXXXX/suite`) in which a script test `sample.ksh` and a program test `sample_prog` share their names with a helper `bin/sample`, a source `cmd/sample.c`, the test's own directory `tests/functional/sample`, `.zcp`, `.c` and `.o` files, a copy in `.libs` and another test `sample_more.ksh`. It runs the tree's own `$SCRIPT_DIR/zfs-tests.sh -k -d <dir> -t <x>` six times (`sample`, `sample.ksh`, `sample_m`, `sample_prog`, the path `tests/functional/sample/sample_more.ksh`, and `missing`) and checks the `[group]` and `tests =` lines of the runfile each one writes. The nested launcher writes that runfile first and then stops at its root check ("This script must not be run as root."), since the test runs as root, before it could load anything or run a test; the test also checks that none of the fake tests ran. It creates no pool.
- **Container run (ksh only, ext4 and tmpfs):** passes with the fix; with master's `zfs-tests.sh` it fails at its first check, `-t sample` having selected the directory `tests/functional/sample` (runfile `[<suite>/tests/functional]`, `tests = ['sample']`). On another file system the directory order can differ, so master may select `bin/sample` or `cmd/sample.c` first instead: each of the three fails at the same first check (step 1 predicts which one this VM gives).
- **Expected:** zt: `-t test_runner_by_name` selects and runs the test, PASS 3/3; group `test_runner` all 4 PASS; every name of the dry selection (step 3a) selects its test. zf: the test, selected by its path, FAILs 2/2 at its first check; in the group, it FAILs the same way and the 3 others PASS.

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parent |
|---|---|---|---|---|
| zt | zts-single-test-ksh-only | fb8fe408e08cd7ca8046204fb75d89ee3d124fb8 | 531393a1fd599aba9405b8f8974953ad9d335a45 | master `acfefd53560409e542e00c84a9c0be05a493278f` (the fix and its test, one commit) |
| zf | zts-single-test-ksh-only-fb | dba311b244379f58b9d90ca9dd87ecc79e022f57 | 89d69795c506e76a9171eabefdf3d160375b81f5 | master `acfefd53560409e542e00c84a9c0be05a493278f` (the test alone) |

The test file `tests/zfs-tests/tests/functional/test_runner/test_runner_by_name.ksh` has mode `100755` and sha256 `bc6beb24affa37ba376bacae352e512f8f07d1ab8d5bce6e4834cc56f0676ffc` in both trees. The two trees differ only in `scripts/zfs-tests.sh`, so their modules are built from the same sources: the two srcversions are expected to be equal. The module is not under test here; it only has to be a debug build of these sources.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- ZTS uses its own file vdevs. Touch no block device of the VM. Never run `zpool import -a` or `zfs-tests.sh -x`.
- **`zfs-tests.sh` as root, step 3a and 4b only:** `sudo -n $D/<tree>/scripts/zfs-tests.sh -k -d <empty scratch dir> -t <name>` writes its runfile into the scratch dir and then stops at its root check (`zfs-tests.sh: This script must not be run as root.`, exit 1), before it creates a file vdev, loads a module or runs a test; `-k` turns its exit cleanup into a no-op. Run it exactly so, never without `-k` and `-d`, and never as root with any other option.
- **Before every ZTS run:**
  - `lsmod | grep -E '^zfs '` shows the module, and `cat /sys/module/zfs/srcversion` equals `modinfo -F srcversion $D/<tree>/module/zfs.ko` for that step's tree;
  - `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool;
  - `ls -d /var/tmp/testdir /var/tmp/test-runner* /var/tmp/file-vdev* 2>&1` says none exists.
- **Every build and every ZTS run runs detached** (`setsid nohup ... &`) and is polled. The root runs of steps 3a and 4b take a second each and run in the foreground.
- **Hangs.** A build still going after 60 minutes, a single-test run after 15 minutes, or a group after 30 minutes is a hang. Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etime,pcpu,comm,wchan:32 | grep -E ' D|zfs-tests|test-runner|ksh|find|zpool|zfs'`; `/proc/<tid>/stack` of each D-state thread, read in the background (`(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`); `tail -n 200 /proc/spl/kstat/zfs/dbgmsg`; `sudo -n dmesg | tail -n 150`. Then stop and report. **Do not reboot.**
- **Kernel log.** Write a mark before each step's runs and, after them, check the new kernel lines since the mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING|Call Trace|blocked for more than|possible circular|deadlock`. None is expected anywhere in this task (it creates no pool). Attribute any line to the test running then, from the `ZTS run ...` kmsg lines that `-K` writes. A `VERIFY` or `ASSERT` failure is a finding: quote it in full, capture as for a hang, and stop.
- Run `sudo -n fstrim -av` after deleting the trees.

## Step 0: state

Check and quote:
- `date -u`, `uname -r`, `uptime`, `nproc`, `free -m`;
- `df -h /var/tmp`: at least 10 GiB free, else stop and report;
- `findmnt -no SOURCE,FSTYPE -T /var/tmp`: quote it (the test's fake suite lives there, and its file system decides what master selects);
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git`, `sudo -n true && echo sudo-ok`;
- `ls -d /var/tmp/task-1123-ztk /var/log/task-1123-ztk /var/tmp/testdir /var/tmp/test-runner* /var/tmp/file-vdev* 2>&1`: all absent;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or empty; quote which;
- then, once `$L` exists (step 1), `ls -A /var/tmp > $L/vartmp-0.txt`.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-ztk; L=/var/log/task-1123-ztk; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && ls -A /var/tmp > $L/vartmp-0.txt
for t in "zt zts-single-test-ksh-only" "zf zts-single-test-ksh-only-fb"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 2 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
for n in zt zf; do (cd $D/$n && p=tests/zfs-tests/tests/functional/test_runner/test_runner_by_name.ksh && echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64)"); done
for n in zt zf; do (cd $D/$n && git log -1 --format='%an | %s' && git diff --stat HEAD^ HEAD | tail -1); done
diff -rq -x .git $D/zt $D/zf
grep -n 'NEWSINGLETEST=\$(find' $D/zt/scripts/zfs-tests.sh $D/zf/scripts/zfs-tests.sh
for n in zt zf; do grep -n -A1 'test_runner_by_name' $D/$n/tests/runfiles/common.run; done
o=$(mktemp -d $D/order.XXXXXX) && mkdir $o/bin $o/cmd $o/tests && ls -f $o | grep -vx '\.\|\.\.' | tr '\n' ' ' | tee $L/order.txt; echo; rm -r $o
```

Expected:

```
zt fb8fe408e08cd7ca8046204fb75d89ee3d124fb8 531393a1fd599aba9405b8f8974953ad9d335a45 acfefd53560409e542e00c84a9c0be05a493278f 0
zf dba311b244379f58b9d90ca9dd87ecc79e022f57 89d69795c506e76a9171eabefdf3d160375b81f5 acfefd53560409e542e00c84a9c0be05a493278f 0
```

Then `zt 100755 bc6beb24affa37ba376bacae352e512f8f07d1ab8d5bce6e4834cc56f0676ffc` and `zf 100755 bc6beb24affa37ba376bacae352e512f8f07d1ab8d5bce6e4834cc56f0676ffc`. Then `Tigerblue77 | ZTS: make zfs-tests.sh -t NAME select the test of that name`, ` 4 files changed, 101 insertions(+), 2 deletions(-)`, `Tigerblue77 | ZTS: add test_runner_by_name (fail-before, VM only) [skip ci]` and ` 3 files changed, 92 insertions(+), 1 deletion(-)`. Then exactly one line from `diff`: `Files /var/tmp/task-1123-ztk/zt/scripts/zfs-tests.sh and /var/tmp/task-1123-ztk/zf/scripts/zfs-tests.sh differ`. Then:

```
/var/tmp/task-1123-ztk/zt/scripts/zfs-tests.sh:648:		NEWSINGLETEST=$(find "$STF_SUITE/tests" -name .libs -prune \
/var/tmp/task-1123-ztk/zt/scripts/zfs-tests.sh:651:		[ -n "$NEWSINGLETEST" ] || NEWSINGLETEST=$(find \
/var/tmp/task-1123-ztk/zf/scripts/zfs-tests.sh:645:		NEWSINGLETEST=$(find "$STF_SUITE" -name "$SINGLETEST*" -print -quit)
```

Then, twice (zt, then zf), `1160:tests = ['test_runner_by_name', 'test_runner_report', 'test_runner_rerun',` and `1161-    'test_runner_selection']`. Then the order line: `bin`, `cmd` and `tests` in the order this file system lists them (`ls -f`, which is the order `find` meets them). The first of the three predicts what master selects for `-t sample` in step 4: `bin` → `bin/sample`, `cmd` → `cmd/sample.c`, `tests` → the directory `tests/functional/sample`. Quote it; it is not checked against anything.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: the two debug builds, at once (about 10 to 20 minutes)

```sh
D=/var/tmp/task-1123-ztk; L=/var/log/task-1123-ztk
for n in zt zf; do setsid nohup sh -c "date -u +%T; cd $D/$n && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$n.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-z?.log` until there are two lines; each must read `build-rc=0`. Quote both durations and every `warning:` line outside objtool notes. Then quote:
- `for n in zt zf; do modinfo -F srcversion $D/$n/module/zfs.ko; done`: two equal values (same module sources); a difference is a finding: quote it and go on;
- `for n in zt zf; do find $D/$n/tests/zfs-tests -name 'test_runner_by_name*'; done`: exactly `$D/zt/tests/zfs-tests/tests/functional/test_runner/test_runner_by_name.ksh` and the same for `zf`;
- `ls -l $D/zt/tests/zfs-tests/tests/functional/{hkdf/hkdf_test,libzfs/many_fds,vdev_disk/page_alignment,tmpfile/tmpfile_001_pos} $D/zt/scripts/common.sh`: all five exist (the four test programs are built; step 3a selects them).

## Step 3: the fix (zt), all expected PASS

**Load zt:**
- `sudo -n $D/zt/scripts/zfs.sh`;
- quote `cat /sys/module/zfs/srcversion`, which must equal `modinfo -F srcversion $D/zt/module/zfs.ko`;
- quote the last `ZFS: Loaded module` line, which must say "(DEBUG mode)";
- write the mark: `echo "task-1123-ztk: zt loaded" | sudo -n tee /dev/kmsg > /dev/null`.

**3a. Dry selection with the fix** (no test runs; see the VM rules). For each name the PR says master mis-resolves, plus the four program tests, a few helpers' namesakes and a missing name:

```sh
D=/var/tmp/task-1123-ztk; L=/var/log/task-1123-ztk; t=zt
for n in mmap zfs_rewrite sync tst.args_to_lua mmap_ftruncate hkdf_test many_fds page_alignment tmpfile_001_pos mmap_read_truncate mmap_eof_extend mount_null_source send_doall file_append zfs_load-key tst.clone no_such_test; do
  r=$D/dry-$t/$n; mkdir -p $r
  sudo -n $D/$t/scripts/zfs-tests.sh -k -d $r -t $n > $r/out 2>&1; rc=$?
  echo "$t $n rc=$rc | $(tr '\n' ' ' < $r/out)| $(grep -h -e '^\[' -e '^tests = ' $r/zfs-tests.*.run | grep -v '^\[DEFAULT\]' | tr '\n' ' ')"
done | tee $L/dry-$t.txt
```

Quote `$L/dry-zt.txt` in full. Every line must read `zt <name> rc=1 | zfs-tests.sh: This script must not be run as root. | [<group>] tests = ['<test>'] `, with `<group>` and `<test>` as below (`Z` = `/var/tmp/task-1123-ztk/zt/tests/zfs-tests/tests/functional`):

| name | `[<group>]` | `tests = ['<test>']` |
|---|---|---|
| mmap | `[Z/io]` | `mmap.ksh` |
| zfs_rewrite | `[Z/cli_root/zfs_rewrite]` | `zfs_rewrite.ksh` |
| sync | `[Z/io]` | `sync.ksh` |
| tst.args_to_lua | `[Z/channel_program/lua_core]` | `tst.args_to_lua.ksh` |
| mmap_ftruncate | `[Z/mmap]` | `mmap_ftruncate.ksh` |
| hkdf_test | `[Z/hkdf]` | `hkdf_test` |
| many_fds | `[Z/libzfs]` | `many_fds` |
| page_alignment | `[Z/vdev_disk]` | `page_alignment` |
| tmpfile_001_pos | `[Z/tmpfile]` | `tmpfile_001_pos` |
| mmap_read_truncate | `[Z/mmap]` | `mmap_read_truncate.ksh` |
| mmap_eof_extend | `[Z/mmap]` | `mmap_eof_extend.ksh` |
| mount_null_source | `[Z/mount]` | `mount_null_source.ksh` |
| send_doall | `[Z/rsend]` | `send_doall.ksh` |
| file_append | `[Z/append]` | `file_append.ksh` |
| zfs_load-key | `[Z/cli_root/zfs_load-key]` | `zfs_load-key.ksh` |
| tst.clone | `[Z/channel_program/synctask_core]` | `tst.clone.ksh` |

and the last: `zt no_such_test rc=1 | zfs-tests.sh: couldn't find test matching 'no_such_test' | ` (no group). Any other group or test is a finding: quote it and go on. A line whose output is not one of these two messages means the launcher went further than its root check: stop, quote it and `ls -la $D/dry-zt/<name>`, and check `lsmod`, `ls -d /var/tmp/file-vdev*` and `losetup -a`.

**Runs,** one at a time, after the pre-run checks:

```sh
cd /var/tmp/task-1123-ztk/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1123-ztk/zts-<tag>-<k>.log 2>&1 < /dev/null &
```

Quote for each run (the `Test:` lines below are given without their time prefix and padding):
- every `Test:` line with its result and duration;
- the Results Summary, and the lines `zts-report` prints after it;
- from the run's results (`R=$(ls -td /var/tmp/test_results/*/ | head -1)`; `M=$(find $R -path '*test_runner_by_name*' -name merged)`, the test's own `merged` file): `grep -E '^(ASSERTION: |nested-launcher: |nested-runfile: \[[^D]|nested-runfile: tests = |ERROR: |NOTE: |SUCCESS: test ! -e |SUCCESS: rm -rf |zfs-tests.sh -t NAME)' $M`, `grep -c '^SUCCESS: grep -Fqx ' $M` and `grep -c "^SUCCESS: grep -Fq couldn't find test matching 'missing'" $M`; and every line of the run with `FAIL`, `failed`, `SKIP` or `ERROR`.

Copy every run's `$R/log` to `$L/log-<tag>-<k>.txt`. On a FAIL that is not expected, also quote the failing command, its output and the 20 lines before it.

**3b. `test_runner_by_name` selected by its name, 3 runs** (tag `zt-name`, `<sel>` = `-t test_runner_by_name`). **The selection is under test:** the runner must start the test, with the line `Test: /var/tmp/task-1123-ztk/zt/tests/zfs-tests/tests/functional/test_runner/test_runner_by_name.ksh (run as root) [00:0N] [PASS]`; if it aborts before running a test, quote its output in full, say so, and go on to 3c. A `[SKIP]` (`zfs-tests.sh not found`: the test did not get `SCRIPT_DIR`) is a finding: quote it. Expected: PASS each time, in a few seconds; Results Summary `PASS 1`. The `merged` lines, in this order (`<w>` is the test's `/var/tmp/test-runner-by-name.XXXXXX`):

```
ASSERTION: zfs-tests.sh -t NAME selects the test named NAME.
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [<w>/suite/tests/functional/sample]
nested-runfile: tests = ['sample.ksh']
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [<w>/suite/tests/functional/sample]
nested-runfile: tests = ['sample.ksh']
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [<w>/suite/tests/functional/sample]
nested-runfile: tests = ['sample_more.ksh']
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [<w>/suite/tests/functional/sample]
nested-runfile: tests = ['sample_prog']
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [tests/functional/sample]
nested-runfile: tests = ['sample_more.ksh']
nested-launcher: zfs-tests.sh: couldn't find test matching 'missing'
SUCCESS: test ! -e <w>/executed
zfs-tests.sh -t NAME selects the test named NAME.
NOTE: Performing local cleanup via log_onexit (cleanup)
SUCCESS: rm -rf <w>
```

and the counts `10` (`SUCCESS: grep -Fqx`) and `1` (`couldn't find test matching 'missing'`). Quote run 1's lines in full, then for runs 2 and 3 only what differs. After each run, `ls -d /var/tmp/test-runner* 2>&1` must say none exists.

**3c. The `test_runner` group, one run** (tag `zt-group`, `<sel>` = `-T test_runner`): 4 tests, `.../tests/functional/test_runner/test_runner_by_name`, `test_runner_report`, `test_runner_rerun` and `test_runner_selection`, each `(run as root)`. Expected: all PASS, `test_runner_by_name` with the same `merged` lines as in 3b. For any test that fails or is skipped, quote it; step 4a reruns it on zf, to tell a master failure from one of the fix.

After these runs:
- no pool left; `ls -d /var/tmp/test-runner* /var/tmp/file-vdev* 2>&1`: none;
- the kernel-line check since the zt mark: nothing from the list;
- unload with zt's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 4: the test alone (zf), expected FAIL

Load zf as in step 3 (srcversion against `$D/zf/module/zfs.ko`), with the mark `task-1123-ztk: zf loaded`.

**4a. Only if a test other than `test_runner_by_name` failed or was skipped in 3c:** run each such test once on zf first (tag `zf-recheck-<test>`, `<sel>` = `-t tests/functional/test_runner/<test>.ksh`), with the usual quotes, and say for each whether master fails or skips it the same way. Then the pre-run checks again.

**4b. Dry selection with master's lookup** (informative, no expected result, no stop on what it selects): the loop of 3a with `t=zf` (output `$L/dry-zf.txt`). Quote it in full. Each line must still end at the root check or at `couldn't find test matching`, as in 3a; what it selects is master's answer on this VM's file system, to set against the PR's claims (in its container: `mmap` → the directory `functional/mmap`, `zfs_rewrite` → the directory `cli_root/zfs_rewrite`, `sync` → the directory `channel_program/synctask_core`, `tst.args_to_lua` → its `.out` file, `many_fds` → `many_fds.o`). Say for each name whether master selected the test, and if not, what.

**4c. The test selected by its path, 2 runs** (tag `zf-path`, `<sel>` = `-t tests/functional/test_runner/test_runner_by_name.ksh`, a path: master's lookup by name is the bug under test, so it is not used to select the test), each after the pre-run checks. The runner must start the test: `Test: /var/tmp/task-1123-ztk/zf/tests/zfs-tests/tests/functional/test_runner/test_runner_by_name.ksh (run as root) [00:0N] [FAIL]`. Expected: `[FAIL]` each time, in a few seconds, at the test's first check (`-t sample`). The `merged` lines, in this order:

```
ASSERTION: zfs-tests.sh -t NAME selects the test named NAME.
nested-launcher: zfs-tests.sh: This script must not be run as root.
nested-runfile: [<w>/suite/tests/functional]
nested-runfile: tests = ['sample']
ERROR: grep -Fqx [<w>/suite/tests/functional/sample] <w>/run.1/zfs-tests.<pid>.run exited 1
NOTE: Performing local cleanup via log_onexit (cleanup)
SUCCESS: rm -rf <w>
```

and the counts `0` and `0`. The two `nested-runfile` lines are master's selection for `-t sample`, which follows the directory order of step 1: `[<w>/suite/tests/functional]` and `tests = ['sample']` (the directory `tests/functional/sample`, as in the PR's container), or `[<w>/suite/bin]` and `tests = ['sample']` (the helper), or `[<w>/suite/cmd]` and `tests = ['sample.c']` (the source). Each of the three is the expected fail-before; say which one, and whether it matches step 1's order line. In `$R/log` the same lines carry a time prefix. A PASS, a SKIP, or a FAIL at any other line (a `run.2` or later in the `ERROR:` line, say) is not the expected fail-before: quote it and say so. After each run, `ls -d /var/tmp/test-runner* 2>&1` must say none exists.

**4d. The `test_runner` group, one run** (tag `zf-group`, `<sel>` = `-T test_runner`): expected `test_runner_by_name` `[FAIL]` with the same `merged` lines as in 4c, and `test_runner_report`, `test_runner_rerun`, `test_runner_selection` PASS (unless 4a showed master failing one of them).

After the runs:
- no pool left (`timeout 30 sudo -n $D/zf/zpool list -H -o name` empty); `ls -d /var/tmp/test-runner* /var/tmp/file-vdev* 2>&1`: none;
- the kernel-line check since the zf mark: nothing from the list;
- unload with zf's `scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## Step 5: cleanup

- `ls -A /var/tmp | diff $L/vartmp-0.txt -`: quote it. Remove only what this task created there: `task-1123-ztk`, `test-runner*`, `file-vdev*`, `zfs-tests.*.run`, `zts-results.*`, `zts-report.*`, `constrained_path.*`, and the `test_results` runs of this task (their logs are in `$L`). Anything else new: quote it and leave it. The runfiles of steps 3a and 4b belong to root inside `$D`, which is yours: `rm -rf $D` removes them; if it cannot, use `sudo -n rm -rf $D`;
- `ls -l /etc/zfs/zpool.cache 2>&1`: as in step 0; quote it and leave it (this task creates no pool, so it must not have changed);
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-ztk`.

## Report

Write `/var/log/task-1123-ztk/report.md` and give the same text as your final message:

1. **Environment:** kernel, uptime, nproc, RAM, free space, the file system of `/var/tmp`, `trees.txt`, the mode and sha256 of the test in both trees, the log and diffstat lines, the `diff` line, the `find` lines of `zfs-tests.sh`, the runfile lines, the order line, the build rc and durations with their warnings, both srcversions with "(DEBUG mode)", and the `find` and `ls -l` lines of step 2.
2. **Table:** tag | tree | run | selection | result per test (or "all N PASS") | duration | key lines (for `test_runner_by_name`: the `tests =` line of each nested runfile, or the `ERROR:` line). Then the dry selections as a table: name | zt selected | zf selected. Then one verdict line, for example: "fix: `-t test_runner_by_name` selects the test, PASS 3/3 (nested selections sample.ksh, sample.ksh, sample_more.ksh, sample_prog, sample_more.ksh by path, missing not found; no fake test ran); group test_runner all 4 PASS; dry selection: the 16 names all select their test, no_such_test not found; master: <n> of the 16 names select something else (list); test alone: FAIL 2/2 at the first check, `-t sample` having selected <the directory tests/functional/sample | bin/sample | cmd/sample.c>, as step 1's order predicts; group test_runner 3 PASS and test_runner_by_name FAIL; no kernel lines".
3. **Quotes:** the `merged` lines of each `test_runner_by_name` run (zt run 1 in full, then what differs; both zf runs in full; the group runs' if they differ), `dry-zt.txt` and `dry-zf.txt` in full, the non-PASS lines, and the kernel-line checks.
4. **Deviations, cleanup,** then `End`.
