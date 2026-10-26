# VM 107: kernel runs of the tests of openzfs/zfs#19433 in three states

These instructions are for the session on VM 107 (Debian 13, kernel 6.12,
user `tigerblue77` with passwordless sudo). They cover the whole task: STEPs 0
to 4 fetch and build three trees and load nothing, STEPs 5 to 8 load each
tree's modules behind a srcversion guard and run the tests, and STEP 9 checks
the kernel log of the whole task, cleans up and writes
`/var/log/zts-19433-1026m/REPORT.txt`, whose expected contents end this
document. Reply with `REPORT.txt` in full, in this session's own transcript,
and nowhere else.

**For VM 106 instead** (if VM 106 frees first), change only this:

- Log in as root, not `tigerblue77`: STEP 0 expects `root` from `id -un`, and
  instead of `sudo -n true` checks `id zts` and
  `sudo -n -l -U zts | grep -i nopasswd` (the test user `zts`, with
  passwordless sudo). `sudo -n` in front of a command works as root and can
  stay.
- STEP 1's `sudo -n mkdir` and `chown` work unchanged as root.
- `zfs-tests.sh` refuses to run as root. In `env.sh`, in both `zts_run` and
  `group`, put `runuser -u zts --` right before `./scripts/zfs-tests.sh`
  (after `nice -n 10 ionice -c3`). At the end of STEP 5, once `trees
  verified` is printed and before the first load, run
  `chown -R zts /var/tmp/zts-19433-1026m /var/log/zts-19433-1026m/results`:
  git refuses a repository owned by another user, and every git command of
  this task runs in STEPs 2 to 5, before that `chown`.
- Everything else is the same: the same directories, the same Debian 13 6.12
  kernel (`6.12.113+deb13-cloud-amd64` on both VMs so far, `nproc` 16), the
  same `umask 022` in `env.sh`, and the same expected values. A "Stack depot
  reached limit capacity" warning is not expected there, and is folded the
  same way if it comes.

## The pull request and the three trees

openzfs/zfs#19433, "raidz: fix scrub repairs during a raidz expansion"
(wrouesnel), is open. Its head has been
`04195230fd72ef56cec1f676ee2a4340ea879a97` since a force-push on 26 Oct, a
rebase onto `da0903a63e` ("zio_crypt_os: add Linux kernel crypto API
backend", 23 Oct) of the six commits approved on 19 Oct as `ca1fbc462`:
`git range-diff` shows the two kernel commits and two of the test commits
identical, and only the test lists in `common.run` and `Makefile.am` changed
in the other two. The two kernel fixes are in `module/zfs/vdev_raidz.c`:

- `3343d58f9e` "raidz: hold the expansion rangelock until repair writes
  finish". A scrub's repair write to a row that the reflow is copying could
  land after the copy, so the repair was lost, or it overwrote what the
  reflow had just put there. The rangelock is now held until the repair
  writes complete.
- `7bcd6857c6` "raidz: reconstruct rows not yet reflowed with their own
  width". The combinatorial reconstruction (`raidz_simulate_failure()`)
  treated the rows of a block that are still at their old location as if
  they had the new width. So a block straddling the reflow offset, with one
  damaged sector per row, could not be reconstructed.

The other four commits are ZTS only: the new tests
`raidz_expand_scrub_repair` and `raidz_expand_boundary_repair`, a rewritten
`wait_raidz_expand_paused()` in `libtest.shlib` (it now waits until the
reflow has copied at least `raidz_expand_max_reflow_bytes` and stopped, and
fails after 300 s otherwise), the tunable `RAIDZ_EXPAND_MAX_COPY_BYTES` in
`tunables.cfg`, and `raidz_expand_001_pos` setting `SCRUB_AFTER_EXPAND` to 0.

| Tree | Code | Tests in it |
| --- | --- | --- |
| `B` = `/var/tmp/zts-19433-1026m/B` | the head `04195230fd` | the head's |
| `X` = `/var/tmp/zts-19433-1026m/X` | upstream master `a128b8a075` ("Linux 7.3 compat: META", 26 Oct, 18 commits past the base) with the head merged: `git merge --no-commit` of `04195230fd`, then the scripted resolution of STEP 3 | master's, with the head's merged in |
| `M` = `/var/tmp/zts-19433-1026m/M` | the base `da0903a63e`, without either fix | the head's seven files under `tests/`, copied in |

**X.** The merge onto `a128b8a075` is not clean. Master's `c8534b99c2`
("raidz: don't lose the expansion wakeup when a resilver ends early") added
`raidz_expand_011_pos` at the same place in the two test lists where the head
adds `raidz_expand_boundary_repair`, so `tests/runfiles/common.run` and
`tests/zfs-tests/tests/Makefile.am` conflict, and nothing else does
(`vdev_raidz.c` merges cleanly). STEP 3 checks that the conflict is exactly
that, resolves both by keeping both sides, and checks the result to the
blob: the resolved lists, the hash of every file and of the whole tree. The
head's code change lands on master with its patch-id unchanged
(`8b7b558444`). X also runs master's `raidz_expand_011_pos`, which calls the
rewritten `wait_raidz_expand_paused()`: on master that helper returned as
soon as the amount copied stopped changing.

**M** is the base plus all seven of the head's files under `tests/`. These
four change what runs on M:

- `raidz_expand_boundary_repair.ksh` and `raidz_expand_scrub_repair.ksh` do
  not exist in the base;
- `libtest.shlib` has the rewritten `wait_raidz_expand_paused()`, which all
  three tests run on M call;
- `tunables.cfg` maps `RAIDZ_EXPAND_MAX_COPY_BYTES`, without which
  `raidz_expand_boundary_repair` can neither set nor restore that tunable;
- `raidz_expand_001_pos.ksh` is the head's (with `SCRUB_AFTER_EXPAND` 0).

The other two, `common.run` and `Makefile.am`, only register the new tests
for `-T` and for installation. A `-t` run reads neither, and M runs no group
unless STEP 8's condition holds. M's kernel code is the base's, byte for
byte, and differs from B's only in `include/sys/vdev_raidz_impl.h` and
`module/zfs/vdev_raidz.c`.

The states run in the order B, X, M. The two that should pass come first,
and the one with the bugs comes last, so that whatever M does cannot cost
the other two their evidence.

## What the tests do, and what each tree should show

| Label | Test | B | X | M |
| --- | --- | --- | --- | --- |
| `bb`/`xb`/`mb` 1-3 | `raidz_expand_boundary_repair` | PASS 3/3 | PASS 3/3 | FAIL 3/3 at the paused scrub |
| `bs`/`xs`/`ms` 1-3 | `raidz_expand_scrub_repair` | PASS 3/3 | PASS 3/3 | FAIL in some or all runs; PASS or SKIP possible (a race) |
| `be`/`xe`/`me` 1-3 | `raidz_expand_001_pos` | PASS 3/3 | PASS 3/3 | PASS 3/3 expected |
| `xw` 1-3 | `raidz_expand_011_pos` (master's) | - | PASS 3/3 | - |
| `bg`, `xg` | `-T raidz`, once | PASS x20 | PASS x21 | `mg` only if `bg` or `xg` has a test that did not pass |

Each `-t` run also runs the raidz directory's `setup` and `cleanup`, which
do nothing and pass: three `Test:` lines per run. `-T raidz` selects
`common.run`'s `[tests/functional/raidz]` section (no `linux.run` section
carries the tag): on B its 18 tests plus `setup` and `cleanup` (20 results),
on X the same with `raidz_expand_011_pos` (21).

**`raidz_expand_boundary_repair`** makes a raidz1 pool of six 256 MiB files
in its own directory under the `-d` directory, writes a 400 MiB file of
random data (`recordsize=128k`, no compression) and attaches a seventh file
with the reflow paused at 1/80 of the allocated space. It exports the pool,
zeroes disk 0 from the first row that is still at its old location, imports,
and moves the pause point on in 1/80 steps, with
`raidz_expand_max_copy_bytes` at 1 MiB + 4 KiB, until a block of the file
straddles the reflow offset (`NOTE: block at <off> (asize <n>) straddles
it`), at most 20 times, after which it reports SKIP (`no block of the file
straddles the reflow offset`). It then scrubs with the reflow paused, and
requires `errors: No known data errors`, the expansion still in progress, a
scrub that repaired something, and the file's xxh128 unchanged. It lets the
expansion finish, scrubs again and requires `repaired 0B`.

- **B and X:** PASS. The author's runs with both fixes: 20 of 20 on Linux
  with a debug build, 10 of 10 release, 10 of 10 on FreeBSD.
- **M:** FAIL, after the paused scrub, at
  `ERROR: check_pool_status testpool errors No known data errors exited 1`,
  with `scan: scrub repaired <n> in <t> with <k> errors` (k is 1 in the
  author's runs) and `errors: Permanent errors have been detected in the
  following files:` above it. Without `7bcd6857c6` the straddling block
  cannot be reconstructed. The author: 20 of 20 failed with the rangelock fix
  alone on Linux debug, 10 of 10 release, 10 of 10 on FreeBSD. In userspace
  (libzpool, debug, the test's sequence) on M's code, the paused scrub left
  1 permanent error, the block read back wrong, and the final scrub
  repaired 24 KiB; on X's code, 0 errors and 0 repaired at the end.

**`raidz_expand_scrub_repair`** makes a raidz3 pool of six 128 MiB files,
fills three datasets with random files, and attaches a seventh with the
reflow paused at a tenth. Three times it exports, zeroes the data area of
disks 0 to 2, imports, starts a scrub and only then moves the pause point to
3/10, 5/10 and 7/10. It requires `No known data errors` after each scrub and
the expansion still in progress, and notes
`the reflow did not run during this scrub` when the reflow did not move
during it. With none of the three overlapping it reports SKIP (`the reflow
did not run during any of the scrubs`). Then it lets the expansion finish,
scrubs, and requires `repaired 0B`: every repair of the three scrubs must
have stuck.

- **B and X:** PASS. The author: 20 of 20 on Linux debug with the rangelock
  fix (on an earlier version of the test), 0 of 30 failed with both fixes
  and 10 ms of write delay on the zeroed disks.
- **M:** FAIL in some or all runs, either at a round's
  `check_pool_status testpool errors No known data errors` (permanent
  errors) or at the final `check_pool_status testpool scan repaired 0B`
  (repairs lost, found by the last scrub). It is a race. The author's
  earlier version failed 20 of 20 on Linux debug without the rangelock fix,
  and the current one 6 of 10 on FreeBSD. In userspace on M's code, 2 of 2
  runs passed as they were; with 5 ms added to each write to the zeroed
  disks, 2 of 2 failed, one losing 37 blocks from its second scrub on, the
  other leaving 11.4 MiB for the final scrub to repair (on the older base
  of 19 Oct, 3 of 4 runs failed without any delay). X passed both ways.
  A PASS or a SKIP on M is a result to report, with its per-scrub notes,
  not a stop.

**`raidz_expand_001_pos`** creates a raidz pool of random parity (1 to 3) on
six 128 MiB files, fills it, attaches a seventh with the reflow paused at a
random offset, zeroes the parity count of disks twice with a scrub after
each, lets the expansion finish, and replaces disks twice with resilvers,
requiring `No known data errors` each time. PASS on all three trees: on M
only the test changed. The author never saw it fail on master. CI ran it
69 times on other branches, at 8 s to 3 min each, with 1 FAIL. A FAIL on M at
`test_scrub`'s errors check would mean that a block straddling the random
pause offset met the reconstruction bug; report it with its first `ERROR:`
line.

**`raidz_expand_011_pos`** (master's, on X only) offlines a child of a
raidz1 that is expanding, four times, waits for the expansion to pause for
resilver, onlines it and requires the expansion not to stay paused, calling
`wait_raidz_expand_paused` after the attach and after each outage. PASS 3/3:
it sets `RAIDZ_EXPAND_MAX_REFLOW_BYTES` before each call, so the rewritten
helper returns once the reflow has reached that point. It passed in CI on
master, at 14 to 20 s.

**Why M cannot hurt the VM.** The bugs damage the test pool's own data: a
block reads back wrong or a repair is lost, and the tests report it
(permanent errors, a later scrub repairing more). Nothing in that path
asserts. In the userspace runs on the base, a debug libzpool, which has the
same `ASSERT`s as a debug module, ran both scenarios to the end, data loss
included, and no assertion fired. The test's cleanup destroys the pool and
removes its files.

What was checked before this was written:

- The merge, the resolution and every hash in STEP 3 were run on a copy of
  the same commits. X built with `--enable-debug` in userspace with no
  compiler warning, and 3 `ZFS_DEBUG` lines in `zfs_config.h`.
- The userspace reproducers of the 19 Oct review (libzpool, debug, the two
  tests' sequences), rebuilt on X's and M's code on 26 Oct. Boundary: X
  clean, M 1 permanent error and 24 KiB left for the final scrub. Scrub
  race: X clean in 3 runs (one with 5 ms of write delay), every scrub
  overlapping the reflow; M clean in 2 runs without delay and failed in 2
  of 2 with it.
- The kernel has not run either new test on these trees. The author's fork
  CI passed the head (`zfs-qemu`, 15 of 15 jobs, one known flaky test
  rerun).

## Rules

- Log in as `tigerblue77` and run everything as that user, in bash, in this
  order. Use `sudo -n` only where it is written. There is no `zts` user and
  none is created.
- A production hypervisor sits underneath. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads and every test run go through `nice -n 10 ionice -c3` too
  (the blocks do it), one at a time, with nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing (no package, no `make install`).
- This task's own directories are `/var/tmp/zts-19433-1026m` (`$Z`: the
  repository, the three trees, and `files`, the `-d` directory, mode 1777)
  and `/var/log/zts-19433-1026m` (`$L`: logs, and `results`, where the
  runner's `BASEDIR` points). Never read, write or delete anything else in
  `/var/tmp` or `/var/log`: another task's `/var/tmp/zts-*` or
  `/var/log/zts-*`, `/var/tmp/test_results`. Never create, edit or remove
  `/etc/zfs/zpool.cache`, `/etc/zfs/vdev_id.conf` or anything else in
  `/etc`.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. The only removal by hand is STEP 9's `rm -rf` of `$Z`,
  with its literal path.
- Never run a `zpool`, `zfs` or `zdb` command while none of this task's
  modules is loaded: libzfs would `modprobe` whatever zfs module the system
  has. STEPs 0 to 4 load nothing. Never print the environment (`env`,
  `printenv`, `set`, `/proc/*/environ`).
- From STEP 2 on, every block starts by sourcing
  `/var/log/zts-19433-1026m/env.sh`, so that it also works in a shell that
  keeps no state between commands.
- Stop at any value that the comments or Expected lines do not allow. After
  a line ending in `stop`, run only STEP 9: the helpers refuse to build, load
  or run once a step has stopped. Clean, repair and retry nothing by hand.
  If your permission system refuses a command, report it and stop. A kernel
  panic, oops, VERIFY, ASSERT or hung task means stop and report, never
  reboot: the maintainer decides about the reboot.
- **A test's own result is not a stop**: a FAIL or SKIP where the tables
  expect otherwise is reported with its digest, and the runs go on. A guard,
  a kernel finding, a leftover, an outer timeout or any other check that
  does not print what is expected is a stop.
- Each build takes 5 to 15 minutes, each block of three runs 5 to 15, and
  each group 20 to 45. If your shell tool cannot wait that long in the
  foreground, start that one block with the tool's own background mode and
  wait for it to exit. Never use `&` or `nohup`, and run nothing meanwhile.

What earlier VM runs taught, and the blocks already handle:

- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines into
  `zfs_config.h` (one from the option, two in the kernel copy-builtin
  block), so a debug build is checked as 3. Compiler warnings are counted
  only as `file:line:col: warning:`: this kernel's objtool prints its own 83
  or so `warning: objtool:` lines per tree, which are recorded, not counted.
- Results are matched anywhere in a `Test:` line, which starts with a
  timestamp. A `-t` run names the test with `.ksh`, a `-T` run without it,
  and a `linux.run` test is `Test (Linux): ...`; both count. A literal
  parenthesis in a regex is `[(]`.
- The test's lines are read from the runner's own `log` file (a test that
  writes nothing on stderr has no `merged` file). `log_must` ends a test at
  its first failure, so its first `ERROR:` line, and the line before the
  failure callbacks, are the whole story of a failed run.
- Kernel messages are read per run from `journalctl -k --since` (the
  failure callbacks print and then clear dmesg). The `ZTS run` markers and
  the `spa_import_progress_set_notes_impl` notes are dropped before
  counting. A one-time "Stack depot reached limit capacity" WARNING of this
  kernel (a `[ cut here ]` block at `lib/stackdepot.c`, with a call trace
  through the slab allocator) is folded into one `DEPOT SPLAT` line, counted
  apart, and is never a stop. A ZFS finding names ZFS: `VERIFY3(...) failed`
  or `PANIC at <file>.c:<line>:<function>()`, a `BUG:` or `Oops` with its
  `RIP` in `[zfs]` or `[spl]`, a `WARNING:` at a `module/` path, or a task
  blocked in ZFS functions.
- Each run waits 2 s first: two runs started in one second would share a
  results directory. The `-d` directory has mode 1777.
- Some of master's tests leak a loop device, a temporary file in the `-d`
  directory or a tunable (`zpool_add_warn_removal`, the callers of
  `vdevs_in_pool`, `removal_with_zdb`, `removal_multiple_indirection`'s
  `zfs_remove_max_segment`, `removal_condense_export`'s `testdir1`). None of
  them is in the raidz group, and no raidz test calls `vdevs_in_pool`, so
  this task expects nothing left after any run: a leftover is a stop.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-19433-1026m L=/var/log/zts-19433-1026m
date -u '+%F %T'
id -un                                  # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                                # 6.12.<n>+deb13-cloud-amd64 (6.12.113 so far)
grep PRETTY_NAME /etc/os-release        # PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
nproc; umask                            # 16, then 0002
ls -d /lib/modules/$(uname -r)/build    # the headers' path
lsmod | grep -E '^(zfs|spl) '            # nothing
ls /sys/module | grep -xE 'zfs|spl'      # nothing
ls -l /dev/zfs 2>&1                      # No such file or directory
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db'   # nothing
pgrep -ax 'zed|lt-zed'                   # nothing
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                  # No devices found
grep -E ' zfs |testpool' /proc/mounts    # nothing
ls -d /testpool* 2>&1                    # No such file or directory
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache, or a 0-byte one; no vdev_id.conf
ls -d $Z $L 2>&1                         # No such file or directory, twice
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
df -h /var/tmp                           # 12G free or more
```

```sh
export PATH=/usr/sbin:/sbin:$PATH
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
echo "stack depot warnings this boot: $(sudo -n journalctl -k -b --no-pager -q | grep -c 'Stack depot reached limit capacity')"
echo "modprobe options: $(grep -rhsE '^options +(zfs|spl) ' /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d | xargs)"
for c in ksh python3 git modinfo sha256sum timeout jq xxh128sum; do
	echo "$c: $(command -v $c || echo absent)"
done
```

Keep the output of both blocks for `REPORT.txt`. Expected, besides the
comments: only this session's tmux pane, one kernel line, `stack depot
warnings this boot:` 0 or 1 (recorded), `modprobe options:` with nothing
after it, and a path for each of the eight commands.

Stop at once, changing nothing, and report what you found if: `id -un` is
not `tigerblue77` or `sudo -n true` fails; the system is not Debian 13 on a
6.12 kernel with its headers (do not install them); a zfs or spl module is
loaded or `/dev/zfs` exists; a test or build process or any `zed` runs; a
tmux pane other than this session's runs anything but an idle shell; a loop
or device-mapper device, a zfs or test mount or a `/testpool*` directory
exists; `zpool.cache` is not empty or `vdev_id.conf` exists; `$Z` or `$L`
exists (this task started once already); `/var/tmp` has less than 12G free
(three built trees take about 4G, the tests up to 3G more); `journalctl -k`
prints no kernel line; a `modprobe` option for zfs or spl is set (it would
apply to every load and change the guard's values); or one of the eight
commands is absent. `jq` and `xxh128sum` matter: the tests read the reflow
progress with `jq`, and without `xxh128sum` the boundary test's check that
the file is intact compares two empty strings and passes for nothing. A
6.12 kernel other than `6.12.113` is not a stop, and neither is a 0-byte
`zpool.cache`.

## STEP 1: this task's directories, `env.sh` and the resolver

`/var/log` is root's, so `$L` is made with `sudo -n` and handed to this user.
The block writes no file unless it can create all the directories.

```sh
export PATH=/usr/sbin:/sbin:$PATH
if sudo -n mkdir /var/log/zts-19433-1026m &&
    sudo -n chown "$(id -un):" /var/log/zts-19433-1026m &&
    mkdir /var/log/zts-19433-1026m/results /var/log/zts-19433-1026m/merge &&
    mkdir /var/tmp/zts-19433-1026m && mkdir -m 1777 /var/tmp/zts-19433-1026m/files; then
echo "mkdir rc=0"
cat > /var/log/zts-19433-1026m/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
Z=/var/tmp/zts-19433-1026m
L=/var/log/zts-19433-1026m
UPSTREAM=https://github.com/openzfs/zfs
PR=19433
# The pull request's head, its base (the merge base with master) and its six
# commits in order; upstream master of 26 Oct, onto which X merges the head.
HB=04195230fd72ef56cec1f676ee2a4340ea879a97
MB=da0903a63e0df9392233a5ccafcf68e7fd660eff
MA=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
C6="b51c31799eb4e8656117c1e934c1b8e70bdd6d4b 3343d58f9e4cea2ec50d862804ebb2bf83b8b2d5
7417f2083fdde0087c4ff833d8c2cc6bfbdfbf0b 41ffed6448402fd8ad20f4e3e0e890e184ba0070
7bcd6857c6e3900b2bd5643fb9c7215a6ab3ed64 04195230fd72ef56cec1f676ee2a4340ea879a97"
C6=$(echo $C6)
# The head's two code files (KF) and seven files under tests/ (TF), in
# git's order, the five of TF that the merge does not touch (TO), and the
# patch-ids of the whole change and of its code part.
KF="include/sys/vdev_raidz_impl.h module/zfs/vdev_raidz.c"
TS=tests/zfs-tests
TO="$TS/include/libtest.shlib $TS/include/tunables.cfg
$TS/tests/functional/raidz/raidz_expand_001_pos.ksh
$TS/tests/functional/raidz/raidz_expand_boundary_repair.ksh
$TS/tests/functional/raidz/raidz_expand_scrub_repair.ksh"
TO=$(echo $TO)
TF="tests/runfiles/common.run $TS/include/libtest.shlib $TS/include/tunables.cfg
$TS/tests/Makefile.am $TS/tests/functional/raidz/raidz_expand_001_pos.ksh
$TS/tests/functional/raidz/raidz_expand_boundary_repair.ksh
$TS/tests/functional/raidz/raidz_expand_scrub_repair.ksh"
TF=$(echo $TF)
PID=2cc6fc9e90536843c8501ecb879a4ae45e9b8627
PIDK=8b7b5584449ef8cf91e9e9c58b7313265dc5be94
TR=tests/test-runner/bin/test-runner.py
# The files each tree changes against its HEAD once built (the runner's
# BASEDIR included), and the tree of its index.
CH_B="$TR.in"
CH_M="tests/runfiles/common.run $TR.in $(echo $TF | cut -d' ' -f2-)"
CH_X="$KF tests/runfiles/common.run $TR.in $(echo $TF | cut -d' ' -f2-)"
TREE_B=8e21b925336df71a7a120de77333cc9b758a79b0
TREE_M=05da8b8f2d9872894710413cccdf3ff694164837
TREE_X=6badff68214b71c3b9bc9f7a0d5a6f02fe5514dc
# X: the two files the merge leaves in conflict and their resolved blobs.
XU="tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am"
XB="726c73e1bf0aa12beb114484027cdfe3ea344bcf 50168c46979ca56c73b9cd3799f6882224a0c4fa"
# The raidz tests listed in common.run (B and M; X adds 011_pos).
RL="raidz_001_neg raidz_002_pos raidz_dedup_overrun raidz_expand_001_pos
raidz_expand_002_pos raidz_expand_003_neg raidz_expand_003_pos raidz_expand_004_pos
raidz_expand_005_pos raidz_expand_006_neg raidz_expand_007_neg raidz_expand_008_pos
raidz_expand_009_neg raidz_expand_010_pos raidz_expand_boundary_repair
raidz_expand_rewrite raidz_expand_scrub_repair raidz_zinject"
RL=$(echo $RL)
RLX=$(echo $RL | sed 's/raidz_expand_010_pos/& raidz_expand_011_pos/')
RD=tests/functional/raidz
# The tunables the raidz tests change and must restore.
TUN="zfs_prefetch_disable zfs_scrub_after_expand zfs_embedded_slog_min_ms
raidz_expand_max_reflow_bytes raidz_expand_max_copy_bytes"
TUN=$(echo $TUN)
# Kernel findings, and a result line of the runner: "Test: <path> (run as
# root) [mm:ss] [RESULT]", or "Test (Linux): ..." for a linux.run test.
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: [$2], expected [$3]"
	fi
}

# A step that stops records why in $L/STOP; nothing is built, loaded or run
# after it.
stop() {   # stop <reason>
	echo "$*: stop" | tee -a $L/runs.txt
	[ -e $L/STOP ] || echo "$*" > $L/STOP
	return 1
}
stopped() {
	[ -e $L/STOP ] || return 1
	echo "stopped earlier ($(cat $L/STOP)): nothing done" | tee -a $L/runs.txt
}

tunables() {
	local t
	for t in $TUN; do
		printf '%s=%s ' $t "$(cat /sys/module/zfs/parameters/$t 2>&1)"
	done
}

loops() {
	sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1
}

# The raidz tests of common.run and the raidz files of Makefile.am in the
# current tree.
runlist() {
	sed -n '/^\[tests\/functional\/raidz\]$/,/^tags/{/^tags/d;p}' tests/runfiles/common.run |
	    grep -oE "'[a-z0-9_]+'" | tr -d "'" | xargs
}
mklist() {
	grep -oE 'functional/raidz/[a-z0-9_]+[.]ksh' tests/zfs-tests/tests/Makefile.am |
	    sed 's|.*/||; s|[.]ksh$||' | xargs
}

# Test processes still running, leaving out this shell, its forks and its
# ancestors: their command lines can carry the words of the block.
others() {
	local skip me p rest q=$$
	skip=" $$ "
	while [ -n "$q" ] && [ "$q" -gt 1 ]; do
		q=$(awk '/^PPid:/ { print $2 }' /proc/$q/status 2>/dev/null)
		skip="$skip$q "
	done
	me=$(tr '\0' ' ' < /proc/$$/cmdline)
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[z]inject ' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' 2>/dev/null < /proc/$p/cmdline)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Kernel log lines without what is known not to be a finding: the runner's
# "ZTS run" markers (test names), the import progress notes ("Verifying
# ..."), and a WARN splat of a full stack depot, folded into one line.
kfilter() {
	awk '
	index($0, "ZTS run ") || index($0, "spa_import_progress_set_notes_impl") { next }
	index($0, "[ cut here ]") { inb = 1; n = 0; dep = 0 }
	inb {
		buf[++n] = $0
		if (index($0, "Stack depot reached limit capacity") || index($0, "lib/stackdepot.c"))
			dep = 1
		if (index($0, "[ end trace ")) {
			if (dep)
				print "DEPOT SPLAT (stack depot full, not a ZFS finding): " n " lines"
			else
				for (i = 1; i <= n; i++) print buf[i]
			inb = 0
		}
		next
	}
	{ print }
	END { if (inb) for (i = 1; i <= n; i++) print buf[i] }'
}

# Kernel messages since <since>, from the journal. A finding stops; a panic,
# VERIFY, ASSERT, BUG, oops or hung task also keeps the module loaded (PANIC).
kern_check() {   # kern_check <label> <since> <expected ZTS marks>
	local n m d
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $L/kern-$1.txt 2>&1
	kfilter < $L/kern-$1.txt > $L/kernf-$1.txt
	n=$(grep -ciE "$KRE" $L/kernf-$1.txt)
	m=$(grep -c 'ZTS run ' $L/kern-$1.txt)
	d=$(grep -c '^DEPOT SPLAT' $L/kernf-$1.txt)
	echo "kernel $1: $n findings, $m ZTS marks (expected $3), $d stack depot splats" | tee -a $L/runs.txt
	grep -iE "$KRE" $L/kernf-$1.txt | head -n 20 | sed 's/^/  /' | tee -a $L/runs.txt
	if grep -qiE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than' $L/kernf-$1.txt; then
		touch $L/PANIC
	fi
	if [ "$n" -ne 0 ]; then
		stop "$1: kernel finding"
		return 1
	fi
	if [ "$3" -gt 0 ] && [ "$m" -eq 0 ]; then
		stop "$1: no ZTS mark in the journal, so its kernel check shows nothing"
		return 1
	fi
}

# Build a tree, niced: a debug build with no compiler warning, both modules
# for the running kernel, a srcversion of its own; then point its runner's
# results at $L/results.
build_tree() {   # build_tree <tree>
	local t0 rc f
	stopped && return 1
	cd $Z/$1 || { stop "no tree $1"; return 1; }
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c \
	    './autogen.sh && ./configure --enable-debug && make -j8' \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/partA.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -n 20 | tee -a $L/partA.txt
		stop "build $1 failed"
		return 1
	fi
	modinfo -F srcversion module/zfs.ko > $L/srcv-$1 2>&1
	{
	same "$1 debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1$' zfs_config.h)" 3
	same "$1 compiler warnings" "$(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$1.log)" 0
	echo "$1 objtool notes: $(grep -c 'warning: objtool:' $L/build-$1.log)"
	same "$1 modules" "$(ls module/spl.ko module/zfs.ko 2>&1 | xargs)" "module/spl.ko module/zfs.ko"
	same "$1 vermagic" "$(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)" "$(uname -r)"
	for f in $L/srcv-*; do
		[ "$f" = "$L/srcv-$1" ] && continue
		same "$1 srcversion differs from ${f##*/srcv-}'s" \
		    "$(cmp -s "$f" $L/srcv-$1 && echo same || echo differs)" differs
	done
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" $TR.in $TR
	same "$1 results directory" "$(grep -cx "BASEDIR = '$L/results'" $TR)" 1
	echo "$1 built: srcversion $(cat $L/srcv-$1)"
	} 2>&1 | tee $L/built-$1.txt | tee -a $L/partA.txt
	if grep -q '^MISMATCH' $L/built-$1.txt; then
		stop "tree $1 is not as expected after its build"
		return 1
	fi
}

# What STEP 5 verifies before anything of <tree> is loaded: its HEAD, its
# index tree, the hash and list of its changes against HEAD, its module's
# srcversion, and the sha256 of its two modules and of every built file
# under a .libs directory (the libraries and programs the tests run).
record_tree() {   # record_tree <tree>
	local n
	stopped && return 1
	cd $Z/$1 || return 1
	{
	echo "head $(git rev-parse HEAD)"
	echo "tree $(git write-tree)"
	echo "diff $(git diff HEAD | sha256sum | cut -d' ' -f1)"
	echo "changed $(git diff HEAD --name-only | xargs)"
	echo "srcversion $(modinfo -F srcversion module/zfs.ko)"
	} > $L/tree-$1.txt
	{ ls module/*.ko; find . -name .git -prune -o -path '*/.libs/*' -type f -print; } |
	    sort | xargs -d '\n' sha256sum > $L/bins-$1.sha256
	n=$(wc -l < $L/bins-$1.sha256)
	echo "$1 recorded: head $(git rev-parse --short=10 HEAD), tree $(git write-tree | cut -c1-10)," \
	    "$n files hashed, zfs.ko x$(grep -c ' module/zfs[.]ko$' $L/bins-$1.sha256)," \
	    "spl.ko x$(grep -c ' module/spl[.]ko$' $L/bins-$1.sha256)" | tee -a $L/partA.txt
}

# The record of <tree> against the tree as it is now. Any MISMATCH means
# this is not the tree that was built.
verify_tree() {   # verify_tree <tree> <head> <index tree> <changed files>
	local s
	cd $Z/$1 || return 1
	{
	same "$1 head" "$(git rev-parse HEAD)" "$2"
	same "$1 index tree" "$(git write-tree)" "$3"
	same "$1 as recorded" "$(sed -n 's/^head //p; s/^tree //p' $L/tree-$1.txt | xargs)" "$2 $3"
	same "$1 changes as recorded" "$(git diff HEAD | sha256sum | cut -d' ' -f1)" \
	    "$(sed -n 's/^diff //p' $L/tree-$1.txt)"
	same "$1 changed files" "$(git diff HEAD --name-only | xargs)" "$4"
	same "$1 srcversion" "$(modinfo -F srcversion module/zfs.ko)" "$(cat $L/srcv-$1)"
	s=$(sha256sum -c --quiet $L/bins-$1.sha256 2>&1 | head -n 5)
	same "$1 built files ($(wc -l < $L/bins-$1.sha256) hashed)" "${s:-unchanged}" unchanged
	same "$1 results directory" "$(grep -cx "BASEDIR = '$L/results'" $TR)" 1
	} 2>&1 | tee -a $L/verify.txt
	! grep -q "^MISMATCH: $1 " $L/verify.txt
}

# Load <tree>'s modules. Whatever is loaded must be <previous>'s, and is
# unloaded first; <previous> is "none" for the first load. No pool may
# appear at load (one would come from a cache file, and is not ours).
switch_to() {   # switch_to <tree> <previous>
	local cur p since
	stopped && return 1
	since=$(date '+%F %T')
	cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
	if [ -n "$cur" ]; then
		if [ "$2" = none ] || [ "$cur" != "$(cat $L/srcv-$2 2>&1)" ]; then
			stop "the loaded zfs module $cur is not ${2}'s"
			return 1
		fi
		(cd $Z/$2 && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u)
		echo "unload $2 rc=$?" | tee -a $L/runs.txt
	fi
	if lsmod | grep -qE '^(zfs|spl) '; then
		stop "zfs or spl still loaded before loading $1"
		return 1
	fi
	(cd $Z/$1 && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh)
	echo "load $1 rc=$?" | tee -a $L/runs.txt
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$cur" != "$(cat $L/srcv-$1)" ] ||
	    [ "$(modinfo -F srcversion $Z/$1/module/zfs.ko)" != "$(cat $L/srcv-$1)" ]; then
		stop "$1 guard failed: loaded srcversion [$cur], built [$(cat $L/srcv-$1)]"
		return 1
	fi
	cp $L/srcv-$1 $L/loaded-srcv
	echo $1 > $L/loaded-tree
	tunables > $L/tunables-loaded.txt
	echo "$1 guard ok: srcversion $cur, zfs_flags $(cat /sys/module/zfs/parameters/zfs_flags)," \
	    "spl_panic_halt $(cat /sys/module/spl/parameters/spl_panic_halt)" | tee -a $L/runs.txt
	echo "$1 tunables: $(cat $L/tunables-loaded.txt)" | tee -a $L/runs.txt
	if [ "$(cat /sys/module/zfs/parameters/zfs_flags)" != 4294932990 ]; then
		stop "$1: zfs_flags is not a debug module's default"
		return 1
	fi
	p=$(sudo -n $Z/$1/zpool list -H -o name 2>&1)
	if [ -n "$p" ]; then
		touch $L/HELD
		stop "$1: pools present right after the load: $p"
		return 1
	fi
	echo "$1: no pool after the load" | tee -a $L/runs.txt
	kern_check load-$1 "$since" 0
}

# Before a run: nothing stopped, a new label, the loaded tree's module, and
# none of what a run can leave.
pre_run() {   # pre_run <label>
	local cur p l f m r z v t
	stopped && return 1
	if [ -e $L/$1.log ]; then
		echo "label $1 already used: nothing run" | tee -a $L/runs.txt
		return 1
	fi
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$cur" != "$(cat $L/loaded-srcv 2>&1)" ]; then
		stop "$1: the loaded srcversion is [$cur], not [$(cat $L/loaded-srcv 2>&1)]"
		return 1
	fi
	p=$(sudo -n $Z/$(cat $L/loaded-tree)/zpool list -H -o name 2>&1)
	l=$(loops)
	[ "$l" = "$(cat $L/loops-before.txt)" ] && l=""
	f=$(LC_ALL=C ls -A $Z/files | xargs)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	r=$(ls -d /testpool* 2>/dev/null | xargs)
	z=$(pgrep -ax 'zed|lt-zed')
	v=$(ls -l /etc/zfs/vdev_id.conf 2>/dev/null)
	t="tunables as loaded"
	[ "$(tunables)" = "$(cat $L/tunables-loaded.txt)" ] || t="TUNABLES CHANGED: $(tunables)"
	echo "before $1: pools=[$p] new loops=[$l] files=[$f] mounts=[$m] rootdirs=[$r] zed=[$z] vdev_id.conf=[$v] $t" |
	    tee -a $L/runs.txt
	if [ -n "$p$l$f$m$r$z$v" ] || [ "$t" != "tunables as loaded" ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: nothing may be left. A pool, device, mount, process or loop
# device left may hold the module (HELD: STEP 9 does not unload).
leftovers() {   # leftovers <label>
	local bad=0 held=0 p
	{
	p=$(sudo -n $Z/$(cat $L/loaded-tree)/zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; held=1; }
	p=$(sudo -n dmsetup ls 2>&1)
	[ "$p" = "$(cat $L/dm-before.txt)" ] || { echo "  LEFTOVER dm devices: $p"; held=1; }
	p=$(loops)
	[ "$p" = "$(cat $L/loops-before.txt)" ] || { echo "  LEFTOVER loop devices: $p"; held=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; held=1; }
	p=$(ls -d /testpool* 2>/dev/null | xargs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; held=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; held=1; }
	if [ -e /etc/zfs/vdev_id.conf ] || [ -L /etc/zfs/vdev_id.conf ]; then
		echo "  LEFTOVER $(ls -l /etc/zfs/vdev_id.conf)"; bad=1
	fi
	p=$(tunables)
	[ "$p" = "$(cat $L/tunables-loaded.txt)" ] || { echo "  LEFTOVER tunables: $p"; bad=1; }
	p=$(LC_ALL=C ls -A $Z/files | xargs)
	if [ -n "$p" ]; then
		echo "  LEFTOVER files in $Z/files: $p"
		sudo -n ls -la --time-style=+%T $Z/files | sed 's/^/    /'
		bad=1
	fi
	if [ $bad$held = 00 ]; then
		echo "leftovers after $1: none"
	else
		echo "leftovers after $1: FOUND"
	fi
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	[ $bad$held = 00 ] && return 0
	[ $held = 1 ] && touch $L/HELD
	stop "$1: leftovers"
}

post_run() {   # post_run <label> <since> <rc> <expected ZTS marks>
	local bad=0
	kern_check $1 "$2" $4 || bad=1
	if [ $3 -eq 124 ] || [ $3 -eq 137 ]; then
		stop "$1: outer timeout"
		bad=1
	fi
	leftovers $1 || bad=1
	return $bad
}

# Result counts of runner result lines on stdin: "PASS x20", "PASS x19, FAIL x1".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# What a runner log says of test <name>, one paragraph per run of it: its
# result and counters (boundary: pause moves and straddling blocks; scrub:
# scrubs during which the reflow did not run, and the reflowed byte counts
# of each scrub; 001_pos: its raidz level; 011_pos: its outage note), what
# the scan lines of the zpool status it printed repaired and with how many
# errors, its errors lines, and, when it did not pass, its first ERROR line
# and its last line before the failure callbacks.
digest() {   # digest <runner log> <name>
	awk -v n="$2" '
	function flush() {
		if (!sec)
			return
		printf "  %s %s:", n, res
		if (n == "raidz_expand_boundary_repair")
			printf " pause moves x%d, straddling blocks x%d", pm, st
		else if (n == "raidz_expand_scrub_repair")
			printf " scrubs without reflow x%d, reflowed (before->after):%s", nr, (rf == "" ? " none" : rf)
		else if (n == "raidz_expand_001_pos")
			printf " %s", (lvl == "" ? "no zpool create line" : lvl)
		else if (n == "raidz_expand_011_pos")
			printf " %s", (ou == "" ? "no outage note" : ou)
		printf "\n"
		if (ns)
			print "    scans, repaired/errors:" sc
		for (i = 1; i <= ne; i++)
			print "    errors: " eo[i] " x" ec[eo[i]]
		if (res != "PASS") {
			print "    first ERROR line: " (err == "" ? "none" : err)
			print "    last line before the callbacks: " (done ? fl : prev)
		}
		sec = 0
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		res = $0; sub(/^.*\[/, "", res); sub(/\].*$/, "", res)
		pm = 0; st = 0; nr = 0; ns = 0; ne = 0; sc = ""; rf = ""; lvl = ""; ou = ""
		err = ""; fl = ""; prev = ""; done = 0
		delete ec
		delete eo
		next
	}
	!sec || done { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	line ~ /NOTE: Performing (test-fail callback|local cleanup)/ { fl = prev; done = 1; next }
	line ~ /^NOTE: reflow offset [0-9]+ after the second pause$/ { pm++ }
	line ~ /^NOTE: block at [0-9]+ [(]asize [0-9]+[)] straddles it$/ { st++ }
	line ~ /^NOTE: the reflow did not run during this scrub$/ { nr++ }
	n == "raidz_expand_scrub_repair" && line ~ /^NOTE: reflowed [0-9]+ before the scrub, [0-9]+ after it$/ {
		split(line, w, " "); rf = rf " " w[3] "->" w[7]
	}
	line ~ /^SUCCESS: zpool create -f -o cachefile=none testpool raidz[0-9] / {
		match(line, /raidz[0-9]/); lvl = substr(line, RSTART, RLENGTH)
	}
	line ~ /^NOTE: [0-9]+ outages paused the expansion$/ { ou = substr(line, 7) }
	line ~ /^[ \t]*scan: / && ns < 10 {
		s = line
		if (match(s, /repaired [^ ]+ in [^ ]+ with [0-9]+ errors/)) {
			split(substr(s, RSTART, RLENGTH), w, " ")
			sc = sc " " w[2] "/" w[6]
		} else {
			sub(/^[ \t]*scan: /, "", s)
			sc = sc " [" s "]"
		}
		ns++
	}
	line ~ /^errors: / {
		e = substr(line, 9)
		if (!(e in ec))
			eo[++ne] = e
		ec[e]++
	}
	err == "" && line ~ /^ERROR: / { err = line }
	line != "" { prev = line }
	END { flush() }' "$1"
}

# The lines of test <name> in a runner log, kept in <file> for the report's
# reader: the result line, the output, and the failure callbacks (zfs_dbgmsg
# and dmesg).
keep() {   # keep <runner log> <name> <file>
	awk -v n="$2" '
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
	}
	sec { print }' "$1" > "$3"
}

# One run of one raidz test with -t (which adds the directory's setup and
# cleanup), from the root of the loaded tree: the guard, zfs-tests.sh under
# an outer timeout, nice and ionice, the named test's result, the digest,
# the kernel messages and the leftovers. Returns 1 when nothing more may run;
# a test that fails is a result, not a reason to return 1.
zts_run() {   # zts_run <label> <test name>
	local label=$1 name=$2 t since t0 rc res rt d bad=0
	pre_run $label || return 1
	t=$(cat $L/loaded-tree)
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $Z/$t && timeout -k 60 1800 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -t $RD/$name.ksh) > $L/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $L/$label.log > $L/$label-tests.txt
	res=$(grep -F "/$RD/$name.ksh (run as " $L/$label-tests.txt | tally)
	rt=$(grep -F "/$RD/$name.ksh (run as " $L/$label-tests.txt | grep -oE '\[[0-9:]+\] \[' | tr -d '[] ' | xargs)
	echo "$label $name ${res:-NO RESULT} rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "(runner ${rt:-none}; Test: lines x$(wc -l < $L/$label-tests.txt))" | tee -a $L/runs.txt
	grep -vF "/$RD/$name.ksh (run as " $L/$label-tests.txt | grep -v '\[PASS\]' |
	    sed 's/^/  not passed: /' | tee -a $L/runs.txt
	d=$(awk '/^Log directory/ { print $3 }' $L/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		digest "$d/log" $name | tee -a $L/runs.txt
		[ "$res" = "PASS x1" ] || keep "$d/log" $name $L/$label-$name.txt
	else
		echo "  no runner log (Log directory: '$d')" | tee -a $L/runs.txt
	fi
	post_run $label "$since" $rc 3 || bad=1
	return $bad
}

three() {   # three <label prefix> <test name>: runs <prefix>1, 2 and 3
	zts_run ${1}1 $2 && zts_run ${1}2 $2 && zts_run ${1}3 $2
}

# One run of the raidz group (-T raidz): the same guard, the counts against
# <expected tally>, the digest of every test that did not pass and of the
# two new tests, the kernel messages and the leftovers.
group() {   # group <label> <expected tally>
	local label=$1 exp=$2 t since t0 rc c nt nl d n np bad=0
	pre_run $label || return 1
	t=$(cat $L/loaded-tree)
	sleep 2
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $Z/$t && timeout -k 60 3600 nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $Z/files -T raidz) > $L/$label.log 2>&1
	rc=$?
	grep -E "$TRE" $L/$label.log > $L/$label-tests.txt
	c=$(tally < $L/$label-tests.txt)
	nt=$(grep -c 'Test: ' $L/$label-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $L/$label-tests.txt)
	echo "$label -T raidz ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl; expected $exp)" \
	    "rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -m 1 '^Missing util' $L/$label.log | sed 's/^/  /' | tee -a $L/runs.txt
	grep -v '\[PASS\]' $L/$label-tests.txt | sed 's/^/  not passed: /' | tee -a $L/runs.txt
	np=$(grep -v '\[PASS\]' $L/$label-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	printf '%s' "$np" > $L/$label.notpassed
	d=$(awk '/^Log directory/ { print $3 }' $L/$label.log | head -n 1)
	if [ -f "$d/log" ]; then
		for n in $(echo raidz_expand_boundary_repair raidz_expand_scrub_repair $np | xargs -n 1 | sort -u); do
			digest "$d/log" $n
		done | tee -a $L/runs.txt
		for n in $np; do
			keep "$d/log" $n $L/$label-$n.txt
		done
	else
		echo "  no runner log (Log directory: '$d')" | tee -a $L/runs.txt
	fi
	post_run $label "$since" $rc $(( nt + nl )) || bad=1
	return $bad
}

# The results of every label, per tree and test.
summary() {
	local t p n
	for t in B:b X:x M:m; do
		for p in b:raidz_expand_boundary_repair s:raidz_expand_scrub_repair e:raidz_expand_001_pos w:raidz_expand_011_pos; do
			n=${p#*:}
			grep -E "^${t#*:}${p%%:*}[1-3] $n " $L/runs.txt | awk '{ print $3 }' | sort | uniq -c |
			    awk -v t=${t%%:*} -v n=$n '{ s = s sep $2 " x" $1; sep = ", " }
				END { if (s != "") print "  " t " " n ": " s " (of 3)" }'
		done
	done
	grep -E '^[bxm]g -T raidz ' $L/runs.txt | sed 's/^/  /'
}
ENV
cat > /var/log/zts-19433-1026m/merge/resolve.py <<'PY'
# resolve.py <file> <master side> <head side> <resolved>: the file must hold
# exactly one conflict block, whose two sides are exactly the given texts,
# and no other conflict marker; the block is replaced by <resolved>.
import re
import sys

path, ours, theirs, resolved = sys.argv[1:5]
text = open(path, encoding='utf-8').read()
want = [open(f, encoding='utf-8').read() for f in (ours, theirs, resolved)]
block = re.compile(r'^<<<<<<< [^\n]*\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n',
    re.M | re.S)
found = block.findall(text)
if len(found) != 1:
    sys.exit('%s: %d conflict blocks, expected 1' % (path, len(found)))
if found[0][0] != want[0] or found[0][1] != want[1]:
    sys.exit('%s: the conflict block is not the expected one' % path)
rest = block.sub('', text, count=1)
if re.search(r'^(<<<<<<<|=======|>>>>>>>|\|\|\|\|\|\|\|)( |$)', rest, re.M):
    sys.exit('%s: a conflict marker outside the block' % path)
with open(path, 'w', encoding='utf-8') as out:
    out.write(block.sub(lambda m: want[2], text, count=1))
print('%s: resolved' % path)
PY
bash -n /var/log/zts-19433-1026m/env.sh; echo "env.sh syntax rc=$?"
python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' \
    /var/log/zts-19433-1026m/merge/resolve.py; echo "resolve.py syntax rc=$?"
. /var/log/zts-19433-1026m/env.sh
date '+%F %T' > $L/start.txt
{
echo "start $(cat $L/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
echo "files dir: $(stat -c '%U %a' $Z/files), $(ls -A $Z/files | wc -l) entries"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, `resolve.py syntax rc=0`,
then `start <date> (<time> UTC); tigerblue77; <uname -r>; umask 0022`,
`files dir: tigerblue77 1777, 0 entries`, the `zpool.cache` line as STEP 0
found it, and the free-space line. `mkdir failed` means stop (and STEP 9 is
not run: nothing of this task exists, or another run of it does).

## STEP 2: the sources, pinned

Clone upstream once and fetch the pull request's head (read-only towards
GitHub):

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	{
	git clone -q --no-checkout $UPSTREAM $Z/repo > $L/clone.log 2>&1; echo "clone rc=$?"
	cd $Z/repo &&
	    git -c gc.auto=0 fetch -q --no-tags $UPSTREAM pull/$PR/head:refs/remotes/origin/pr-$PR \
	    > $L/fetch.log 2>&1
	echo "fetch rc=$?"
	same "pull/$PR/head" "$(git rev-parse -q --verify refs/remotes/origin/pr-$PR)" $HB
	same "its six commits on its base" "$(git rev-list --reverse $MB..$HB 2>&1 | xargs)" "$C6"
	for c in $C6; do
		echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"
	done
	same "its base is its merge base with master" "$(git merge-base $MA $HB 2>&1)" $MB
	same "master $MA is in upstream master" \
	    "$(git merge-base --is-ancestor $MA origin/master && echo yes || echo no)" yes
	echo "upstream master now: $(git log -1 --abbrev=10 --format='%h %s' origin/master)"
	same "its files under tests/" "$(git diff --name-only $MB $HB -- tests | xargs)" "$TF"
	same "its code files" "$(git diff --name-only $MB $HB -- . ':!tests' | xargs)" "$KF"
	same "its patch-id" "$(git diff $MB $HB | git patch-id --stable | cut -d' ' -f1)" $PID
	same "the patch-id of its code" \
	    "$(git diff $MB $HB -- $KF | git patch-id --stable | cut -d' ' -f1)" $PIDK
	echo "head: $(git log -1 --abbrev=10 --format='%h %s' $HB):$(git diff --stat $MB $HB | tail -n 1)"
	echo "base: $(git log -1 --abbrev=10 --format='%h %s' $MB)"
	echo "master: $(git log -1 --abbrev=10 --format='%h %s' $MA), $(git rev-list --count $MB..$MA) commits past the base"
	} 2>&1 | tee $L/sha-checks.txt
	grep -q '^MISMATCH' $L/sha-checks.txt && stop "the pull request or master is not as expected"
	grep -qx 'clone rc=0' $L/sha-checks.txt && grep -qx 'fetch rc=0' $L/sha-checks.txt ||
	    stop "the clone or the fetch failed"
fi
```

Expected:

```
clone rc=0
fetch rc=0
ok: pull/19433/head
ok: its six commits on its base
  b51c31799e ZTS: scrub repairs during a running raidz expansion
  3343d58f9e raidz: hold the expansion rangelock until repair writes finish
  7417f2083f ZTS: wait for the raidz reflow to reach its pause point
  41ffed6448 ZTS: repair a block that straddles the raidz reflow offset
  7bcd6857c6 raidz: reconstruct rows not yet reflowed with their own width
  04195230fd ZTS: raidz_expand_001_pos: don't race the post-expansion scrub
ok: its base is its merge base with master
ok: master a128b8a075b5b13c737e56914cd7043bd3e9ebf8 is in upstream master
upstream master now: <sha> <subject>
ok: its files under tests/
ok: its code files
ok: its patch-id
ok: the patch-id of its code
head: 04195230fd ZTS: raidz_expand_001_pos: don't race the post-expansion scrub: 9 files changed, 397 insertions(+), 25 deletions(-)
base: da0903a63e zio_crypt_os: add Linux kernel crypto API backend
master: a128b8a075 Linux 7.3 compat: META, 18 commits past the base
```

`upstream master now` is recorded only: it was `a128b8a075 Linux 7.3
compat: META` when this was written, and X is built on `a128b8a075`
whatever master has become. A `MISMATCH` (the head was pushed again) or a
failed clone or fetch means stop.

## STEP 3: the three trees

One worktree per tree:

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	cd $Z/repo
	for t in B:$HB X:$MA M:$MB; do
		git worktree add -q --detach $Z/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
		echo "worktree ${t%%:*} rc=$? at $(git -C $Z/${t%%:*} rev-parse --short=10 HEAD 2>&1)"
	done 2>&1 | tee $L/worktrees.txt
	[ "$(grep -c ' rc=0 at ' $L/worktrees.txt)" = 3 ] || stop "a worktree could not be made"
fi
```

Expected:

```
worktree B rc=0 at 04195230fd
worktree X rc=0 at a128b8a075
worktree M rc=0 at da0903a63e
```

M: the head's seven files under `tests/`, and nothing else (`git checkout`
stages them, so that `git diff HEAD` shows the new files too):

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	{
	cd $Z/M
	git checkout $HB -- $TF; echo "checkout rc=$?"
	git diff HEAD --stat | tail -n 1
	same "M: files changed against the base" "$(git diff HEAD --name-only | xargs)" "$TF"
	same "M: tests/ as in the head" "$(git diff $HB -- tests | wc -l)" 0
	same "M: everything else as in the base" "$(git diff HEAD -- . ':!tests' | wc -l)" 0
	same "M: differs from the head in" "$(git diff $HB --name-only | xargs)" "$KF"
	same "M: index tree" "$(git write-tree)" $TREE_M
	same "M: modes of the three tests" "$(cd $TS/tests/functional/raidz && stat -c %a \
	    raidz_expand_001_pos.ksh raidz_expand_boundary_repair.ksh raidz_expand_scrub_repair.ksh | xargs)" \
	    "755 755 755"
	same "M: raidz tests in common.run" "$(runlist)" "$RL"
	same "B: raidz tests in common.run" "$(cd $Z/B && runlist)" "$RL"
	same "B: index tree" "$(cd $Z/B && git write-tree)" $TREE_B
	} 2>&1 | tee $L/copy-M.txt
	if grep -q '^MISMATCH' $L/copy-M.txt || ! grep -qx 'checkout rc=0' $L/copy-M.txt; then
		stop "M is not the base with the head's test files"
	fi
fi
```

Expected:

```
checkout rc=0
 7 files changed, 363 insertions(+), 13 deletions(-)
ok: M: files changed against the base
ok: M: tests/ as in the head
ok: M: everything else as in the base
ok: M: differs from the head in
ok: M: index tree
ok: M: modes of the three tests
ok: M: raidz tests in common.run
ok: B: raidz tests in common.run
ok: B: index tree
```

X: merge the head into master without committing. The `-c` options fix
what a user's git configuration could change: the identity a merge needs
(no commit is made with it), the conflict style the resolver reads, and no
replay of a recorded resolution. The merge is expected to fail with the two
conflicts:

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	cd $Z/X
	git -c user.name=merge-check -c user.email=merge-check@localhost \
	    -c merge.conflictStyle=merge -c rerere.enabled=false \
	    merge --no-commit --no-ff $HB > $L/merge.log 2>&1
	echo "merge rc=$?" | tee $L/merge-X.txt
	cat $L/merge.log | tee -a $L/merge-X.txt
	{
	same "X: merge exit status" "$(sed -n 's/^merge rc=//p' $L/merge-X.txt)" 1
	same "X: HEAD" "$(git rev-parse HEAD)" $MA
	same "X: MERGE_HEAD" "$(git rev-parse -q --verify MERGE_HEAD)" $HB
	same "X: files in conflict" "$(git diff --name-only --diff-filter=U | xargs)" "$XU"
	same "X: CONFLICT lines" "$(grep -c '^CONFLICT' $L/merge.log)" 2
	} 2>&1 | tee -a $L/merge-X.txt
	grep -q '^MISMATCH' $L/merge-X.txt && stop "the merge of the head onto master is not as expected"
fi
```

Expected:

```
merge rc=1
Auto-merging module/zfs/vdev_raidz.c
Auto-merging tests/runfiles/common.run
CONFLICT (content): Merge conflict in tests/runfiles/common.run
Auto-merging tests/zfs-tests/tests/Makefile.am
CONFLICT (content): Merge conflict in tests/zfs-tests/tests/Makefile.am
Automatic merge failed; fix conflicts and then commit the result.
ok: X: merge exit status
ok: X: HEAD
ok: X: MERGE_HEAD
ok: X: files in conflict
ok: X: CONFLICT lines
```

Then the resolution. The six files below are each side of each conflict,
as git writes them, and the resolution, which keeps both sides in the
lists' order. `resolve.py` (STEP 1) refuses a file that holds anything but
exactly that one block, then replaces the block. The checks after it cover
the resolved lists, the blob of each resolved file, the tree of the whole
merge, and the head's other changes, which must be on master unchanged:

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	RM=$L/merge
	printf '%s\n' "    'raidz_expand_010_pos', 'raidz_expand_011_pos'," \
	    "    'raidz_expand_rewrite'," > $RM/run-master.txt
	printf '%s\n' "    'raidz_expand_010_pos', 'raidz_expand_boundary_repair'," \
	    "    'raidz_expand_rewrite', 'raidz_expand_scrub_repair'," > $RM/run-head.txt
	printf '%s\n' "    'raidz_expand_010_pos', 'raidz_expand_011_pos'," \
	    "    'raidz_expand_boundary_repair', 'raidz_expand_rewrite'," \
	    "    'raidz_expand_scrub_repair'," > $RM/run-resolved.txt
	printf '\t%s\n' 'functional/raidz/raidz_expand_011_pos.ksh \' > $RM/mk-master.txt
	printf '\t%s\n' 'functional/raidz/raidz_expand_boundary_repair.ksh \' > $RM/mk-head.txt
	printf '\t%s\n' 'functional/raidz/raidz_expand_011_pos.ksh \' \
	    'functional/raidz/raidz_expand_boundary_repair.ksh \' > $RM/mk-resolved.txt
	{
	cd $Z/X
	same "the six texts" "$(cd $RM && cat run-master.txt run-head.txt run-resolved.txt \
	    mk-master.txt mk-head.txt mk-resolved.txt | sha256sum | cut -c1-16)" 5cf30f1ba0c8022c
	python3 -I $RM/resolve.py tests/runfiles/common.run \
	    $RM/run-master.txt $RM/run-head.txt $RM/run-resolved.txt
	echo "resolve common.run rc=$?"
	python3 -I $RM/resolve.py tests/zfs-tests/tests/Makefile.am \
	    $RM/mk-master.txt $RM/mk-head.txt $RM/mk-resolved.txt
	echo "resolve Makefile.am rc=$?"
	git add $XU; echo "add rc=$?"
	same "X: files in conflict after the resolution" "$(git diff --name-only --diff-filter=U | xargs)" ""
	same "X: raidz tests in common.run" "$(runlist)" "$RLX"
	same "X: raidz files in Makefile.am" "$(mklist)" "cleanup $RLX setup"
	same "X: the resolved files" "$(git hash-object $XU | xargs)" "$XB"
	same "X: index tree" "$(git write-tree)" $TREE_X
	same "X: files changed against master" "$(git diff --cached --name-only HEAD | xargs)" "$KF $TF"
	same "X: the head's code change, on master" \
	    "$(git diff --cached HEAD -- $KF | git patch-id --stable | cut -d' ' -f1)" $PIDK
	same "X: the head's other five test files" "$(git diff --cached $HB -- $TO | wc -l)" 0
	same "X: changes not staged" "$(git diff --name-only | wc -l)" 0
	git diff --cached --stat HEAD | tail -n 1
	} 2>&1 | tee -a $L/merge-X.txt
	if grep -q '^MISMATCH' $L/merge-X.txt ||
	    [ "$(grep -cE '^resolve .* rc=0$|^add rc=0$' $L/merge-X.txt)" != 3 ]; then
		stop "the resolution of X is not the expected one"
	fi
fi
```

Expected:

```
ok: the six texts
tests/runfiles/common.run: resolved
resolve common.run rc=0
tests/zfs-tests/tests/Makefile.am: resolved
resolve Makefile.am rc=0
add rc=0
ok: X: files in conflict after the resolution
ok: X: raidz tests in common.run
ok: X: raidz files in Makefile.am
ok: X: the resolved files
ok: X: index tree
ok: X: files changed against master
ok: X: the head's code change, on master
ok: X: the head's other five test files
ok: X: changes not staged
 9 files changed, 397 insertions(+), 24 deletions(-)
```

(One deletion fewer than the head's 25: master's line
`'raidz_expand_010_pos', 'raidz_expand_011_pos',` stays, where the head
rewrote the base's `'raidz_expand_010_pos',`.) Any other line means stop.
The resolved part of `common.run` reads

```
    'raidz_expand_007_neg', 'raidz_expand_008_pos', 'raidz_expand_009_neg',
    'raidz_expand_010_pos', 'raidz_expand_011_pos',
    'raidz_expand_boundary_repair', 'raidz_expand_rewrite',
    'raidz_expand_scrub_repair',
    'raidz_zinject']
```

and that of `Makefile.am` lists `raidz_expand_011_pos.ksh`, then
`raidz_expand_boundary_repair.ksh`, then the head's
`raidz_expand_rewrite.ksh` and `raidz_expand_scrub_repair.ksh`.

## STEP 4: the builds, and the record of what was built

One block per tree. Each builds the tree (`./autogen.sh && ./configure
--enable-debug && make -j8` under `nice -n 10 ionice -c3`), checks it,
points its runner's results at `$L/results`, and records it:

```sh
. /var/log/zts-19433-1026m/env.sh
build_tree B && record_tree B
```

```sh
. /var/log/zts-19433-1026m/env.sh
build_tree X && record_tree X
```

```sh
. /var/log/zts-19433-1026m/env.sh
build_tree M && record_tree M
```

Expected, for each tree:

```
build <tree> rc=0 wall=<s>s
ok: <tree> debug build (ZFS_DEBUG lines)
ok: <tree> compiler warnings
<tree> objtool notes: <n>
ok: <tree> modules
ok: <tree> vermagic
ok: <tree> srcversion differs from <each tree built before>'s
ok: <tree> results directory
<tree> built: srcversion <srcversion>
<tree> recorded: head <sha>, tree <tree>, <n> files hashed, zfs.ko x1, spl.ko x1
```

with heads and trees `04195230fd`/`8e21b92533` for B, `a128b8a075`/`6badff6821`
for X and `da0903a63e`/`05da8b8f2d` for M. The objtool count is recorded
(about 83 on this kernel). On a failed build, its `error:` lines are
printed and the step stops.

## STEP 5: the run half: the trees as built, and the baselines

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	{
	echo "run half start $(date '+%F %T')"
	same "three srcversions" "$(cat $L/srcv-B $L/srcv-X $L/srcv-M | sort -u | wc -l)" 3
	same "X's merge is still in progress" "$(git -C $Z/X rev-parse -q --verify MERGE_HEAD)" $HB
	} 2>&1 | tee -a $L/verify.txt
	verify_tree B $HB $TREE_B "$CH_B" && verify_tree X $MA $TREE_X "$CH_X" &&
	    verify_tree M $MB $TREE_M "$CH_M" && ! grep -q '^MISMATCH' $L/verify.txt &&
	    echo "trees verified" | tee -a $L/verify.txt
	grep -qx 'trees verified' $L/verify.txt || stop "the trees are not the ones built"
	loops > $L/loops-before.txt
	sudo -n dmsetup ls > $L/dm-before.txt 2>&1
	echo "baseline: loops [$(cat $L/loops-before.txt | xargs)], dm [$(cat $L/dm-before.txt)]," \
	    "stack depot warnings this boot $(sudo -n journalctl -k -b --no-pager -q | grep -c 'Stack depot reached limit capacity')" |
	    tee -a $L/verify.txt
fi
```

Expected: `run half start <date>`, `ok: three srcversions`,
`ok: X's merge is still in progress`, then 8 `ok:` lines per tree (head,
index tree, as recorded, changes as recorded, changed files, srcversion,
built files with the number hashed, results directory), `trees verified`,
and `baseline: loops [], dm [No devices found], stack depot warnings this
boot <0 or 1>`. Any `MISMATCH` means stop: a tree is not the one STEP 4
built and recorded.

## STEP 6: state B (the head)

```sh
. /var/log/zts-19433-1026m/env.sh
switch_to B none
```

Expected:

```
load B rc=0
B guard ok: srcversion <B's>, zfs_flags 4294932990, spl_panic_halt 0
B tunables: zfs_prefetch_disable=0 zfs_scrub_after_expand=1 zfs_embedded_slog_min_ms=64 raidz_expand_max_reflow_bytes=0 raidz_expand_max_copy_bytes=167772160
B: no pool after the load
kernel load-B: 0 findings, 0 ZTS marks (expected 0), 0 stack depot splats
```

(4294932990 is a debug module's `zfs_flags`; the tunables are the module's
defaults, and each run is compared with what the load found.) Then the runs,
one block each:

```sh
. /var/log/zts-19433-1026m/env.sh
three bb raidz_expand_boundary_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three bs raidz_expand_scrub_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three be raidz_expand_001_pos
```

```sh
. /var/log/zts-19433-1026m/env.sh
group bg "PASS x20"
```

Expected, for each of `bb1` to `bb3` (and alike for the other labels):

```
before bb1: pools=[] new loops=[] files=[] mounts=[] rootdirs=[] zed=[] vdev_id.conf=[] tunables as loaded
bb1 raidz_expand_boundary_repair PASS x1 rc=0 wall=<s>s (runner <mm:ss>; Test: lines x3)
  raidz_expand_boundary_repair PASS: pause moves x<n>, straddling blocks x<k>
    scans, repaired/errors: <size>/0 0B/0
    errors: No known data errors x2
kernel bb1: 0 findings, 3 ZTS marks (expected 3), 0 stack depot splats
leftovers after bb1: none
```

| Label | Expected on B |
| --- | --- |
| `bb1`-`bb3` | PASS x1 each, as above: `straddling blocks` 1 or more, the paused scrub repairing more than 0B with 0 errors, the final one `0B/0` |
| `bs1`-`bs3` | PASS x1 each: `raidz_expand_scrub_repair PASS: scrubs without reflow x<0 to 2>, reflowed (before->after): <a>-><b> <c>-><d> <e>-><f>`, then `scans, repaired/errors: <r1>/0 <r2>/0 <r3>/0 0B/0` and `errors: No known data errors x4` |
| `be1`-`be3` | PASS x1 each: `raidz_expand_001_pos PASS: raidz<1 to 3>`, no scans line |
| `bg` | `bg -T raidz PASS x20 (Test: x20, Test (Linux): x0; expected PASS x20) rc=0`, the `Missing util(s)` line if any, no `not passed` line, the digests of the two new tests as above, `kernel bg: 0 findings, 20 ZTS marks (expected 20)`, `leftovers after bg: none` |

`1 stack depot splats` instead of 0 on a kernel line is the folded warning
described above: counted, never a stop. A runner time near 5:00 or more for
one wait is worth a note: the new helper fails after 300 s. A `-t` run
reaching 10:00 is `KILLED` by the runner's 600 s limit for single tests.

## STEP 7: state X (master with the head merged)

```sh
. /var/log/zts-19433-1026m/env.sh
switch_to X B
```

Expected: `unload B rc=0`, `load X rc=0`, `X guard ok: srcversion <X's>,
zfs_flags 4294932990, spl_panic_halt 0`, the same tunables, `X: no pool
after the load`, `kernel load-X: 0 findings, ...`.

```sh
. /var/log/zts-19433-1026m/env.sh
three xb raidz_expand_boundary_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three xs raidz_expand_scrub_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three xe raidz_expand_001_pos
```

```sh
. /var/log/zts-19433-1026m/env.sh
three xw raidz_expand_011_pos
```

```sh
. /var/log/zts-19433-1026m/env.sh
group xg "PASS x21"
```

| Label | Expected on X |
| --- | --- |
| `xb1`-`xb3`, `xs1`-`xs3`, `xe1`-`xe3` | as on B |
| `xw1`-`xw3` | PASS x1 each: `raidz_expand_011_pos PASS: <1 to 4> outages paused the expansion`, no scans line |
| `xg` | `xg -T raidz PASS x21 (Test: x21, Test (Linux): x0; expected PASS x21) rc=0`, no `not passed` line, `kernel xg: 0 findings, 21 ZTS marks (expected 21)`, `leftovers after xg: none` |

## STEP 8: state M (the base, with the head's tests)

```sh
. /var/log/zts-19433-1026m/env.sh
switch_to M X
```

Expected: `unload X rc=0`, `load M rc=0`, `M guard ok: ...`, the same
tunables, `M: no pool after the load`, `kernel load-M: 0 findings, ...`.

```sh
. /var/log/zts-19433-1026m/env.sh
three mb raidz_expand_boundary_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three ms raidz_expand_scrub_repair
```

```sh
. /var/log/zts-19433-1026m/env.sh
three me raidz_expand_001_pos
```

```sh
. /var/log/zts-19433-1026m/env.sh
if ! stopped; then
	if [ -s $L/bg.notpassed ] || [ -s $L/xg.notpassed ]; then
		group mg "FAIL x2, PASS x18, or FAIL x1, PASS x19"
	else
		echo "mg not run: bg and xg passed every test" | tee -a $L/runs.txt
	fi
fi
```

Expected for `mb1`-`mb3`, each:

```
mb1 raidz_expand_boundary_repair FAIL x1 rc=<n> wall=<s>s (runner <mm:ss>; Test: lines x3)
  raidz_expand_boundary_repair FAIL: pause moves x<n>, straddling blocks x<k>
    scans, repaired/errors: <size>/<1 or more>
    errors: Permanent errors have been detected in the following files: x1
    first ERROR line: ERROR: check_pool_status testpool errors No known data errors exited 1
    last line before the callbacks: ERROR: check_pool_status testpool errors No known data errors exited 1
kernel mb1: 0 findings, 3 ZTS marks (expected 3), 0 stack depot splats
leftovers after mb1: none
```

(The last line can also be a line of the command's own output, since the
runner sorts a test's stdout and stderr lines by when it read them; the
first `ERROR:` line cannot move.) The whole failing test, with the
`zfs_dbgmsg` and dmesg the failure callbacks print, is kept in
`$L/mb1-raidz_expand_boundary_repair.txt`.

`ms1`-`ms3`, in a run that fails:

```
ms1 raidz_expand_scrub_repair FAIL x1 ...
  raidz_expand_scrub_repair FAIL: scrubs without reflow x<n>, reflowed (before->after): <...>
    scans, repaired/errors: <r1>/0 <r2>/0 <r3>/0 <more than 0B>/0
    errors: No known data errors x4
    first ERROR line: ERROR: check_pool_status testpool scan repaired 0B exited 1
    last line before the callbacks: <that line, or a line of zpool status>
```

that is, the final scrub still repairing, or, in a run where a scrub found
permanent errors, a scan with errors above 0 there, an `errors: Permanent
errors have been detected in the following files:` line, and `first ERROR
line: ERROR: check_pool_status testpool errors No known data errors exited
1` after that scrub. A PASS (all three scrubs' repairs held) or a SKIP (no scrub
overlapped the reflow) is reported as it is, with its counters.

`me1`-`me3`: PASS x1 each, as on B. `mg`: `mg not run: bg and xg passed
every test`. If it does run (a test of `bg` or `xg` did not pass), expect
`raidz_expand_boundary_repair` FAIL and `raidz_expand_scrub_repair` FAIL
or PASS, and compare every other test with `bg` and `xg`: a test that
fails on all three trees is the VM's or the base's, one that fails on B and
X only comes with the head.

## What the results mean

- **As expected** (B and X pass everything, the groups included; M fails
  `raidz_expand_boundary_repair` 3/3 at the paused scrub's errors check and
  `raidz_expand_scrub_repair` in one run or more; no kernel finding, no
  leftover): the kernel confirms both bugs on the base and both fixes on the
  head, alone and merged onto current master, where master's
  `raidz_expand_011_pos` also passes with the rewritten helper.
- **M passes `raidz_expand_boundary_repair`**, or skips it: the straddling
  block was not damaged as the test intends on this VM (or none was
  found). Report the counters and the scan lines of each run.
- **M passes `raidz_expand_scrub_repair` 3/3**: the race did not show on
  this VM in three runs. Report the `scrubs without reflow` and `reflowed`
  counters; the test is then weaker here than the author's 6 of 10.
- **B or X fails a new test**: report its first `ERROR:` line and its scan
  lines run by run, and the file kept for it. Failing on X only would point
  at the merge with master (the code is the head's, the patch-id says so).
- **X fails `raidz_expand_011_pos`** (passing on master in CI): the rewritten
  `wait_raidz_expand_paused` does not suit it. Report its first `ERROR:` line
  and last line.
- **`log_fail` "expansion of testpool neither paused nor completed in 300
  seconds"** in any run: the new helper's timeout. Report which wait (the
  line before it) and the run's wall time.
- **Any kernel finding, a hang, or a leftover**: the line and the run are
  the result. The run stopped there.

## STEP 9: kernel check of the whole task, cleanup, report

Run these blocks in all cases, a stop included, except after a stop in STEP
0 or at STEP 1's `mkdir`. First, the whole task's kernel check:

```sh
. /var/log/zts-19433-1026m/env.sh
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso > $L/kern-all.txt 2>&1
kfilter < $L/kern-all.txt > $L/kernf-all.txt
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kernf-all.txt) findings," \
    "$(grep -c 'ZTS run ' $L/kern-all.txt) ZTS marks, $(grep -c '^DEPOT SPLAT' $L/kernf-all.txt) stack depot splats"
grep -iE "$KRE" $L/kernf-all.txt | head -n 40
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings, 131 ZTS marks, 0 stack depot
splats` (B: 27 + 20, X: 36 + 21, M: 27, and 20 more if `mg` ran; fewer after
a stop; a stack depot splat is counted, not a finding), and no kernel line.
Then unload, and remove the work directory, unless a step stopped. The
module stays loaded on `HELD` or `PANIC`, and `$Z` stays after any stop:

```sh
. /var/log/zts-19433-1026m/env.sh
{
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -e $L/HELD ] || [ -e $L/PANIC ]; then
	echo "module left loaded: $(cd $L && ls HELD PANIC 2>/dev/null | xargs)"
elif [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	(cd $Z/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
else
	echo "no module of this task is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
if [ -e $L/STOP ]; then
	echo "kept: $Z (stopped: $(cat $L/STOP))"
elif [ -n "$(lsmod | grep -E '^(zfs|spl) ')" ]; then
	echo "kept: $Z (a module is still loaded)"
else
	rm -rf /var/tmp/zts-19433-1026m; echo "rm work directory rc=$?"
fi
ls -d $Z 2>&1
echo "loop devices: [$(loops | xargs)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $L/end.txt
```

Expected: `unload M rc=0`, `lsmod: []`, `rm work directory rc=0`, `No such
file or directory`, `loop devices: []`, `zpool.cache` as STEP 0 found it, no
`vdev_id.conf`, and about as much free space as STEP 0 found. Then assemble
`REPORT.txt`:

```sh
. /var/log/zts-19433-1026m/env.sh
{
echo "VM 107, kernel ZTS of openzfs/zfs#19433 in three states (VM-TESTS-19433.md)"
for s in record sha-checks worktrees copy-M merge-X partA verify runs end; do
	echo
	echo "== $s.txt"
	cat $L/$s.txt 2>&1
done
echo
echo "== summary"
summary
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HELD [$(cd $L && ls HELD 2>/dev/null)] PANIC [$(cd $L && ls PANIC 2>/dev/null)]"
echo "kept for failed runs: $(cd $L && ls *-raidz_*.txt 2>/dev/null | xargs)"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, `uname -r`, the
free space, the `zpool.cache` line, the stack depot count, the `jq` and
`xxh128sum` paths), a `verdict:` line naming the case of "What the results
mean" that applies, and a `deviations:` line with every deviation from these
steps and its reason. Then `cat /var/log/zts-19433-1026m/REPORT.txt` and reply
with all of it. Keep `/var/log/zts-19433-1026m`, its `results` included: the
maintainer reads the failing runs there.

## REPORT.txt expected

`<...>` stands for what the run fills in; everything else is what these
instructions expect. The summary ends the generated part, and the lines
appended by hand follow it:

```
== summary
  B raidz_expand_boundary_repair: PASS x3 (of 3)
  B raidz_expand_scrub_repair: PASS x3 (of 3)
  B raidz_expand_001_pos: PASS x3 (of 3)
  X raidz_expand_boundary_repair: PASS x3 (of 3)
  X raidz_expand_scrub_repair: PASS x3 (of 3)
  X raidz_expand_001_pos: PASS x3 (of 3)
  X raidz_expand_011_pos: PASS x3 (of 3)
  M raidz_expand_boundary_repair: FAIL x3 (of 3)
  M raidz_expand_scrub_repair: FAIL x<n>[, PASS x<n>][, SKIP x<n>] (of 3)
  M raidz_expand_001_pos: PASS x3 (of 3)
  bg -T raidz PASS x20 (Test: x20, Test (Linux): x0; expected PASS x20) rc=0 wall=<s>s
  xg -T raidz PASS x21 (Test: x21, Test (Linux): x0; expected PASS x21) rc=0 wall=<s>s
flags: STOP [] HELD [] PANIC []
kept for failed runs: mb1-raidz_expand_boundary_repair.txt mb2-raidz_expand_boundary_repair.txt mb3-raidz_expand_boundary_repair.txt ms<n>-raidz_expand_scrub_repair.txt ...
STEP 0: <anything found>; <uname -r>; /var/tmp <N>G free; zpool.cache <line>; stack depot <0 or 1>; jq <path>; xxh128sum <path>
verdict: as expected (first case of "What the results mean")
deviations: none
```

Above it, in order: `record.txt` (start, user, kernel, umask 0022, files
dir, `zpool.cache`, free space), `sha-checks.txt` (STEP 2's expected lines),
`worktrees.txt`, `copy-M.txt`, `merge-X.txt` (STEP 3's expected lines),
`partA.txt` (STEP 4's lines for B, X and M), `verify.txt` (STEP 5's 26
`ok:` lines, `trees verified` and the baseline), `runs.txt` (every load,
guard, `before`, result, digest, kernel and leftovers line of STEPs 6 to
8), and `end.txt` (STEP 9's kernel line and cleanup).

Estimated duration: about 3 hours (2.5 to 4.5). STEPs 0 to 3 take 10
minutes, the three builds 15 to 45, the 30 single runs 2 to 5 minutes each
(1 to 2.5 hours), the two groups 20 to 45 minutes each, and STEP 9 5.
