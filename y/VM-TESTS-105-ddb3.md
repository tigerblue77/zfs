# VM 105: kernel evidence and a small measurement for `dedup-default-blake3` at 59efd7df84

These instructions are for the session on VM 105 (Debian 13, kernel 6.12,
x86_64). They produce kernel evidence for the fork branch
`dedup-default-blake3` of https://github.com/tigerblue77/zfs, one commit,
`59efd7df84a72e7ae25f03f13cbc2d0b6e2c250e` "Use the faster of BLAKE3 and
SHA-256 for dedup=on on new pools", on upstream master
`a128b8a075b5b13c737e56914cd7043bd3e9ebf8` "Linux 7.3 compat: META", and a
small, indicative timing of `dedup=on` against `dedup=sha256`. The session
builds two debug trees in its own directories, runs everything below in the
kernel, cleans up, and replies with `REPORT.txt`. This document is read from
a checkout of the branch `vm-tasks-1026` of https://github.com/tigerblue77/zfs
in `/var/tmp/vm-task-105y` (the doc checkout, `DOC` below); the last command
of STEP 8 removes it with the task's own directories.

**What the commit does.** `dedup=on` and `dedup=verify` resolve to
`spa_dedup_checksum()`, SHA-256 on every pool today. With the commit, a pool
created with `feature@blake3` enabled records a choice in its pool directory
(object 1 of the MOS): the entry `org.openzfs:dedup_checksum`, 14 (BLAKE3) or
8 (SHA-256), which `ddt_load()` applies at every later import. The new module
parameter `zfs_dedup_default_checksum` decides what a new pool records: `1`
BLAKE3, `2` SHA-256, `0` (the default; any other value behaves as 0) the
faster of the two in the checksum benchmark the module already runs: the
highest 256 KiB figure among the `blake3-*` rows of
`/proc/spl/kstat/zfs/chksum_bench` against the highest among the `sha256-*`
rows, SHA-256 only if its figure is higher and BLAKE3's is not 0. The `0`
choice is logged in the debug log as `pool '<p>': dedup checksum <c> (blake3
<n> MiB/s, sha256 <n> MiB/s)`. A pool created with the feature disabled, or
with a compatibility set that lacks it, records nothing and keeps SHA-256.
The commit also adds the ZTS test `functional/dedup/dedup_default_checksum`,
and sets `dedup=sha256` in the nine dedup tests that inspect the SHA-256 DDT
(they had `dedup=on`).

| Tree | Path | Code | Tests |
| --- | --- | --- | --- |
| `B` | `/var/tmp/zts-ddb3-1028/B` | the commit `59efd7df84` | its own |
| `M` | `/var/tmp/zts-ddb3-1028/M` | its parent `a128b8a075` | the commit's files under `tests/` only (13 files, `git diff a128b8a075 59efd7df84 -- tests`): no module, include or man change |

Both trees are debug builds, `./autogen.sh && ./configure --enable-debug
--datadir=/var/tmp/zts-ddb3-1028/share && make -j8`. The `--datadir` gives
both an uninstalled tree's `libzfs` a pool compatibility directory: pool 4 of
the new test is created with `-o compatibility=openzfs-2.1-linux`, and an
uninstalled tree otherwise has no compatibility file at all (this VM has
none installed), so B would fail there for a reason that is not the
commit's. STEP 1 fills that directory from the trees' own
`cmd/zpool/compatibility.d`, which the commit does not change.

What the run establishes:

1. **Facts on B** (STEP 4), right after its load: the `chksum_bench` lines of
   SHA-256 and BLAKE3 (its first read, which runs the full benchmark; the
   figures then stay for the life of the module), the CPU flags that decide
   them, the entry a new pool records, its debug-log line, and the checksum
   of the blocks a `dedup=on` write gets.
2. **B** (STEP 5): `dedup_default_checksum` 3 times, PASS each, then the
   `dedup` group once, PASS 20.
3. **The measurement** (STEP 6), on B: 2 GiB of random data written into a
   fresh pool with `dedup=on`, then into another with `dedup=sha256`, 3 runs
   each, alternating.
4. **Facts on M and M** (STEP 7): a new pool on M records nothing and writes
   SHA-256 dedup blocks; `dedup_default_checksum` 3 times, FAIL each (see
   below); the `dedup` group once, FAIL 1 (that test) and PASS 19.
5. **Throughout**: the kernel log of every step and run, which must stay
   clean, and in STEP 8 the module's unload.

Either outcome of every run is a valid result: a run is never repeated
beyond what is written here.

## Why M fails the new test, and where

M's `tunables.cfg` (the commit's) names `zfs_dedup_default_checksum`, which
M's module does not have. The test first runs `log_must save_tunable
DEDUP_DEFAULT_CHECKSUM`, which finds no such parameter and saves nothing
(`SUCCESS: save_tunable DEDUP_DEFAULT_CHECKSUM`), then `log_must
set_tunable32 DEDUP_DEFAULT_CHECKSUM 1`, which writes `1` to
`/sys/module/zfs/parameters/zfs_dedup_default_checksum`. sysfs cannot create
that file (ksh93 reports it as `cannot create [Permission denied]`), so the
test ends with `ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1`
before any pool exists: 0 `SUCCESS: zpool create` and 0 `SUCCESS: dd` lines.
Its cleanup's `restore_tunable` finds no parameter and succeeds. So M fails
for the missing tunable, before it reaches the check that would find no
entry recorded (`check_recorded blake3` on pool 1). That a new pool on M
records no entry, and that `dedup=on` writes SHA-256 there, is shown directly
by STEP 7's facts on M, with the same commands as on B.

The nine tests moved to `dedup=sha256` inspect the SHA-256 DDT. On M,
`dedup=on` is SHA-256 anyway, so they pass there as they did before; on B
they keep the table they inspect whatever this host's default is.

## How a pool's choice is read

- **The entry**: `zdb -dddd <pool> 1` dumps the pool directory, and its line
  `org.openzfs:dedup_checksum = 14` (or `= 8`) is the entry; the test reads
  it the same way (`awk '$1 == "org.openzfs:dedup_checksum" { print $3 }'`).
  No such line: nothing recorded.
- **The blocks**: a 512 KiB file of random data written in 128 KiB records
  has four level-0 blocks; `zdb -ddddddbbbbbb <pool>/ <object>` (the trailing
  slash names the root dataset, the object number is the file's inode
  number) prints one `L0 DVA` line each, with the checksum name and `dedup`
  for a dedup block, as the test checks them.
- **The DDT**: `zdb -D <pool>` prints one `DDT-<checksum>: version=...` line
  per dedup table in use.
- The pools of this document (`ddb3f` for the facts, `ddb3m` for the
  measurement) use a cache file of their own, `-o cachefile=<their
  directory>/cache`, and `zdb` reads it with `-U`: `/etc/zfs/zpool.cache`
  never holds them. The ZTS pools use the default cache file, as in every
  earlier run.
- **The benchmark**: the module measures every implementation once at load
  (256 KiB only); the first read of `/proc/spl/kstat/zfs/chksum_bench` runs
  the full benchmark (all sizes, 256 KiB again, a few seconds), and from then
  on the figures do not change until the module is unloaded. STEP 4 makes
  that first read on B, before B creates any pool, so every later choice on
  B (the facts pool, pool 5 of each test run, the measurement's `dedup=on`
  pools) is made from the figures STEP 4 prints. Pool 5 of the test logs
  them (`NOTE: chksum_bench at 256k: blake3 <n> MiB/s, sha256 <n> MiB/s,
  expecting <c>`), and STEP 8 compares.
- **Which wins depends on the CPU**: STEP 0 prints `sha_ni`, `avx2` and
  `avx512f`. With `avx2` or `avx512f`, BLAKE3 is expected well ahead, with or
  without `sha_ni`; with `sha_ni` and neither of the other two, the two are
  close and either can win. No step assumes the answer: every check applies
  the rule to the figures this host measured.

## The measurement, and what it is worth

This is an **indicative single-VM measurement**, not a benchmark: one VM on
a shared hypervisor, a debug build (assertions on), one file vdev on the
VM's own disk, 3 runs per variant. It shows the order of magnitude of the
difference on this host and nothing more.

- `$W/meas/src`: 2 GiB from `/dev/urandom`, written once (incompressible).
- Each run (`meas.sh`): a fresh pool `ddb3m` on a new sparse 4 GiB file,
  `compression=off`, `recordsize=128K`, `atime=off`, and `dedup=on` or
  `dedup=sha256` on its root dataset; the source read once into the page
  cache; then, timed by `/usr/bin/time -f '%e %U %S'`, `dd if=src
  of=<mount>/data bs=1M` followed by `zpool sync`; then the pool's DDT and
  entry, and the pool is destroyed and its file removed. At most 4 GiB of
  sparse vdev and the 2 GiB source exist at once (under 8 GB).
- The checksums are computed by the kernel's zio threads, not by `dd`, so the
  user and system times of `/usr/bin/time` (those of `dd` and `zpool`) miss
  most of that work. Each run therefore also records, from `/proc/stat`, the
  CPU time of the whole VM over the same interval: kernel (system, irq,
  softirq), user, iowait and steal, in seconds.
- Order: `on1`, `sha1`, `on2`, `sha2`, `on3`, `sha3`. Every pool is fresh,
  and each `dedup=on` pool must record what STEP 4's pool recorded. If that
  is SHA-256 on this host, both variants write SHA-256 and no difference is
  expected; the summary says so.

## Rules for the whole run

- Run everything as root, in bash, in this order. The ZTS runs go through
  `runuser -u $ZU` inside `run.sh`, where `ZU` is the test user STEP 1
  picks (`zts` if it has passwordless sudo, else `tigerblue77`);
  `zfs-tests.sh` refuses to run as root. Nothing else runs as that user.
  Shell variables may not survive between your commands, so every block
  below starts by sourcing `/var/log/zts-ddb3-1028/env.sh` or by setting
  what it needs.
- A production hypervisor sits underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads, ZTS runs, pool steps and the measurement go through `nice -n
  10 ionice -c3` too (the scripts do it), one at a time, with nothing else
  running beside them.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), never reboot, and never run `scripts/zfs-helpers.sh` or
  `zfs-tests.sh -x`. Rebuild nothing once STEP 2 has built the trees.
  `scripts/zfs.sh -r` (load) and `-u` (unload) run only in `load.sh` and in
  STEP 8's first block.
- Work only under `/var/tmp/zts-ddb3-1028` (`W`) and `/var/log/zts-ddb3-1028`
  (`L`). Never read from, write to or delete `/var/tmp/test_results`, another
  `/var/tmp/zts-*` or `/var/log/zts-*` directory, another
  `/var/tmp/vm-task-*` checkout, or anything else this task did not create;
  the doc checkout is only read until STEP 8 removes it. ZTS pools mount
  under `/testpool*` while they exist; the tests' cleanup removes those
  directories, and `run.sh` checks that it did.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the ZTS pools in it while they exist and truncates it to 0 bytes
  when they are gone.
- Never run a `zpool`, `zfs` or `zdb` command while none of this task's
  modules is loaded: libzfs would `modprobe` whatever zfs module the system
  has. The scripts check the loaded `srcversion` first. Never print the
  environment (`env`, `printenv`, `set`, `/proc/*/environ`).
- The only `rm -rf` allowed is the last block of STEP 8. The other removals
  are the scripts' own (`facts.sh` its `$W/facts-<tree>`, `meas.sh` its
  `$W/meas/<label>`) and STEP 6's `rm` of `$W/meas/src`.
- If a check does not print what its comment or the Expected lines say, stop
  at once and report, unless the text says the value is recorded only. Do
  not rebuild, repair or retry. If your permission system refuses a command,
  do not work around it: report it and stop there. A test result other than
  the expected one is a result, recorded, never a stop by itself, and so is
  a `MISMATCH` line of `facts.sh` or a measurement figure.
- **A kernel finding** (a line matching `VERIFY|ASSERT|PANIC|BUG:|Oops|call
  trace|blocked for more than`) stops the task: the scripts check the kernel
  log after every step and run, write `$L/STOP` and `$L/HOLD`, and STEP 8
  then leaves the module loaded. After any stop, go straight to STEP 8. A
  stop in STEP 0 or at STEP 1's `mkdir` changes nothing: report and reply.
  Once `$L/STOP` exists, every script refuses to run.
- The two builds take about 30 minutes together, a group run 15 to 30. If
  your shell tool cannot wait that long in the foreground, start that one
  command with the tool's own background mode and wait until it exits; do
  not use `&` or `nohup`, and run nothing else meanwhile.
- The session cannot run anything after its final reply: the last block of
  STEP 8 is the last command, and the reply comes after it. Leave no
  background command, loop or watcher behind.

## What earlier runs on this VM and its siblings taught, and these steps already handle

- `modinfo` and `losetup` are in `/usr/sbin` and `/sbin`: every block puts
  them first in `PATH`. `env.sh` sets the umask to 022.
- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines in
  `zfs_config.h` (one unconditional, two under `#ifdef`): 3 is a debug build.
- objtool prints about 83 `warning:` lines per tree (`spl_panic()` and
  `luaD_throw()` lacking `__noreturn`), harmless. Compiler warnings are
  counted only in the `file:line:col: warning:` form.
- `srcversion` hashes a module's C sources only: M's module code is master
  `a128b8a075`'s, whose debug builds on this VM had srcversion
  `8852710DB871A7587B9E722` (recorded, not a stop if it differs).
- A `-t` run names its test with `.ksh` in its `Test:` line, a group run
  without it, and a test of a `linux.run` section is printed `Test (Linux):`
  (the dedup group's `setup`, `dedup_legacy_gang` and `cleanup`): every
  pattern below allows all three. A literal parenthesis in a regex is written
  `[(]`, never `\(`.
- A passing test's end line is its `log_pass` message, the last line before
  `NOTE: Performing local cleanup`; a failing test's is the line before
  `NOTE: Performing test-fail callback`. A test that writes nothing on stderr
  has no `merged` file: its `stdout` is read instead. Each run waits 2 s
  after it ends: two runs started in the same second would share a results
  directory.
- ZTS clears dmesg after a failed test, so kernel messages are read per run
  from `journalctl -k`. The scan drops the ZTS `-K` markers (`ZTS run
  <test>`) and the `spa_import_progress_set_notes_impl` notes.
- **The stack depot.** When this VM boots with `slub_debug`, the kernel keeps
  an allocation stack for every slab object, and once its stack depot is full
  it prints, once per boot, a `------------[ cut here ]------------` block
  whose message is `Stack depot reached limit capacity`, at
  `lib/stackdepot.c`, with a call trace through the slab allocator. It is not
  a ZFS finding. `kfilter` (in `env.sh`) folds each such block into one
  `DEPOT SPLAT (...)` line, counted apart and never a reason to stop. STEP 0
  records `slub_debug` and this boot's count.
- git refuses to work in a tree owned by another user: all git work happens
  in STEP 1 and STEP 2, before the `chown` to `$ZU`. `zfs-tests.sh -d` must
  point at a mode 1777 directory.
- `zfs.sh -r` and `-u` signal the pid `/var/run/zed.pid` names, so they never
  run while it exists. No test run here starts a `zed` or uses a zvol.
- The dedup tests `dedup_legacy_gang` and `dedup_log_zdb_leak` need
  `xxh128sum`: without it they fail on both trees alike (STEP 0 records it).

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-ddb3-1028 L=/var/log/zts-ddb3-1028 DOC=/var/tmp/vm-task-105y
date -u '+%F %T'
id -un                        # root
uname -r                      # 6.12.<n>: recorded
grep PRETTY_NAME /etc/os-release   # Debian GNU/Linux 13 (trixie)
uname -m; nproc; umask        # x86_64, then recorded, then 0022 (recorded)
grep -m1 '^model name' /proc/cpuinfo
grep -o -w 'sha_ni\|avx2\|avx512f' /proc/cpuinfo | sort -u   # recorded: the flags that decide the default
echo "cmdline: $(grep -o 'slub_debug=[^ ]*' /proc/cmdline)"   # recorded
free -m | sed -n 1,2p         # recorded
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zdb zed zinject   # nothing: no system ZFS
pgrep -ax 'zed|lt-zed'        # nothing
ls -l /var/run/zed.pid 2>&1   # No such file or directory
echo "modprobe options: $(grep -rhsE '^options +(zfs|spl) ' /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d | xargs)"   # nothing after the colon
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|autogen|configure|make|gcc|cc1|ksh|zdb|fio'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -d $W $L 2>&1              # No such file or directory, twice
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
echo "/var/log zts dirs: $(ls -d /var/log/zts-* 2>/dev/null | xargs)"
losetup -a; dmsetup ls 2>&1   # recorded: STEP 3 keeps them as the baseline
grep -E ' zfs |testpool' /proc/mounts   # nothing
ls -d /testpool* /ddb3f /ddb3m 2>&1     # No such file or directory, three times
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
ls -ld /usr/local/etc/zfs/compatibility.d /usr/local/share/zfs/compatibility.d 2>&1   # No such file or directory, twice (recorded)
df -h /var/tmp /var/log
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"   # 12G at least
for u in zts tigerblue77; do echo "$u: $(id $u 2>&1); NOPASSWD lines: $(sudo -n -l -U $u 2>/dev/null | grep -c NOPASSWD)"; done
for c in git ksh python3 modinfo sha256sum truncate runuser timeout sudo losetup journalctl; do echo "$c: $(command -v $c || echo absent)"; done
for c in /usr/bin/time xxh128sum dmsetup getconf; do echo "$c: $(command -v $c || echo absent)"; done
journalctl -k -n 1 --no-pager -q   # one kernel line
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
echo "stack depot warnings this boot: $(journalctl -k -b --no-pager -q | grep -c 'Stack depot reached limit capacity')"
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/y/VM-TESTS-105-ddb3.md 2>&1
```

Keep this output, which goes into `REPORT.txt`. The number printed after the
one kernel line is the kernel baseline (it may count lines of an earlier
stack depot splat, which are recorded only). Directories of earlier tasks
under `/var/tmp` and `/var/log` (`zts-*`, `vm-task-*`, `test_results`) are
recorded only: their presence is not a stop, and this task never touches
them. The CPU, flags, `cmdline`, memory, loop and dm, `compatibility.d`,
`/usr/bin/time`, `xxh128sum`, `dmsetup`, `getconf` and stack depot lines are
recorded too. The doc checkout line is expected as `doc checkout: top
/var/tmp/vm-task-105y, branch vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`) for a clone, or
`branch HEAD, head <a commit of vm-tasks-1026>, origin error: No such remote
'origin'` for a checkout fetched by commit; both are fine. If you read this
document from another place, say so in the report: STEP 8 then leaves that
place alone and removes no doc checkout.

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, `uname -m` is not `x86_64`, or the kernel headers are
  missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, a system `zpool`, `zfs`,
  `zdb`, `zed` or `zinject` is installed, `zed` runs, `/var/run/zed.pid`
  exists, or a `modprobe` option is set for zfs or spl;
- a test or build process is running. The `pgrep -af` also matches the shell
  that runs this block, because that shell's command line contains the
  pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` exists: this task would then have started once already;
- a zfs or test mount, or a `/testpool*`, `/ddb3f` or `/ddb3m` directory
  exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load), or `/etc/zfs/vdev_id.conf` exists;
- less than 12G is free on `/var/tmp` (two built trees and the clone, about
  3 GB; the ZTS file vdevs, sparse, a few GB at most; the measurement, about
  4.5 GB at once);
- neither `zts` nor `tigerblue77` has a `NOPASSWD` line;
- one of the eleven commands of the first `for` line is absent, or
  `journalctl -k` prints no kernel line.

A kernel other than 6.12 is not a stop by itself: STEP 1 checks that it is
within the range each tree supports, and STEP 2 that the modules were built
for it. An existing `/usr/local/etc/zfs/compatibility.d` is not a stop
either, but say so: its files take precedence over this task's.
`/usr/bin/time` absent: `meas.sh` falls back to bash's `time` keyword, with
the same three figures, and says so.

## STEP 1: directories, `env.sh`, the test user, the sources (all git work, part one)

The block writes nothing unless it can create both directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/tmp/zts-ddb3-1028 && mkdir /var/log/zts-ddb3-1028; then
echo "mkdir rc=0"
cat > /var/log/zts-ddb3-1028/env.sh <<'EOF'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-ddb3-1028
L=/var/log/zts-ddb3-1028
DOC=/var/tmp/vm-task-105y
DOCREL=y/VM-TESTS-105-ddb3.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
BRANCH=dedup-default-blake3
# B: the commit; HM: its parent, upstream master when it was written. The
# patch-ids of the whole commit and of its part under tests/.
HB=59efd7df84a72e7ae25f03f13cbc2d0b6e2c250e
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
PIDB=d7a18452c55da7758076c3f507df45996c13389d
PIDT=7a8f33caf94032340ec212ddb665a8f4e2af705d
# The new test and its blob; the nine tests moved to dedup=sha256; the 13
# files the commit changes under tests/ and the 10 outside, in git's order.
D=tests/zfs-tests/tests/functional/dedup
F=$D/dedup_default_checksum.ksh
BF=7cc34dc7ec132740fa4d8ea5201219c02068b467
D9="$D/dedup_bclone.ksh $D/dedup_fdt_create.ksh $D/dedup_fdt_import.ksh $D/dedup_fdt_pacing.ksh $D/dedup_legacy_create.ksh $D/dedup_legacy_fdt_upgrade.ksh $D/dedup_legacy_gang.ksh $D/dedup_legacy_import.ksh $D/dedup_legacy_repair.ksh"
TB="tests/runfiles/common.run tests/zfs-tests/include/tunables.cfg tests/zfs-tests/tests/Makefile.am $D/dedup_bclone.ksh $F $D/dedup_fdt_create.ksh $D/dedup_fdt_import.ksh $D/dedup_fdt_pacing.ksh $D/dedup_legacy_create.ksh $D/dedup_legacy_fdt_upgrade.ksh $D/dedup_legacy_gang.ksh $D/dedup_legacy_import.ksh $D/dedup_legacy_repair.ksh"
CODE="include/sys/ddt.h include/sys/dmu.h include/sys/zfs_chksum.h include/sys/zio.h man/man4/zfs.4 man/man7/zfsprops.7 man/man7/zpool-features.7 module/zfs/ddt.c module/zfs/spa.c module/zfs/zfs_chksum.c"
# The data directory both trees are configured with: their libzfs reads
# the pool compatibility files from $CD (pool 4 of the test needs one).
SHARE=$W/share
CD=$SHARE/zfs/compatibility.d
# The new tunable (B only), the benchmark kstat, the debug log.
P=/sys/module/zfs/parameters/zfs_dedup_default_checksum
KB=/proc/spl/kstat/zfs/chksum_bench
DBG=/proc/spl/kstat/zfs/dbgmsg
# Kernel findings stop the task; WARNING: lines are quoted, not a stop.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KWARN='WARNING:'
# The tunables the dedup tests change and must restore.
TUN='zfs_dedup_default_checksum zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct zfs_bclone_enabled'
# The outer timeout of each ZTS run, in seconds.
TMO=1800

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: [$2], expected [$3]"
	fi
}

# A step that stops records the first reason in $L/STOP; HOLD also keeps
# the module loaded at the end. Every script refuses to run after a stop.
stop() {   # stop <reason>
	echo "$*: stop"
	[ -e "$L/STOP" ] || echo "$*" > "$L/STOP"
}
hold() {   # hold <reason>
	[ -e "$L/HOLD" ] || echo "$*" > "$L/HOLD"
	stop "$*"
}
stopped() {
	[ -e "$L/STOP" ] || return 1
	echo "stopped earlier ($(cat "$L/STOP")): nothing run"
}

tunables() {
	for tp in $TUN; do
		if [ -e /sys/module/zfs/parameters/$tp ]; then
			printf '%s=%s ' "$tp" "$(cat /sys/module/zfs/parameters/$tp)"
		else
			printf '%s=absent ' "$tp"
		fi
	done
}

# Loop and device-mapper devices, compared with STEP 3's baseline.
loops_now() {
	losetup -a 2>&1 | sort
}
dm_now() {
	if command -v dmsetup > /dev/null; then
		dmsetup ls 2>&1 | sort
	else
		echo "no dmsetup"
	fi
}

# What a run may leave in $W/files without stopping the task: the shapes
# of master's known test leaks (tmp.* files, testdir1). No test run here
# is known to leave them; anything else there is a leftover.
files_known() {
	for f in "$W"/files/tmp.?????????? "$W"/files/testdir1; do
		[ -e "$f" ] && echo "${f##*/}"
	done | xargs
}
files_left() {
	ls -A "$W/files" 2>&1 | grep -vxE 'tmp\.[A-Za-z0-9]{10}|testdir1' | xargs
}

# Kernel log lines, the ZTS markers and import notes dropped, each stack
# depot block (slub_debug, not ZFS) folded into one DEPOT SPLAT line.
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
				print "DEPOT SPLAT (stack depot full, slub_debug; not ZFS): " n " lines"
			else
				for (i = 1; i <= n; i++) print buf[i]
			inb = 0
		}
		next
	}
	{ print }
	END { if (inb) for (i = 1; i <= n; i++) print buf[i] }'
}

# The kernel messages since <epoch> in $L/kern-<label>.txt, filtered in
# $L/kernf-<label>.txt. Prints "<findings> <warnings> <ZTS marks>
# <depot splats> <lines>".
kscan() {   # kscan <label> <epoch>
	journalctl -k --since "@$2" --no-pager -q -o short-iso > "$L/kern-$1.txt" 2>&1
	kfilter < "$L/kern-$1.txt" > "$L/kernf-$1.txt"
	echo "$(grep -ciE "$KPAT" "$L/kernf-$1.txt")" \
	    "$(grep -cE "$KWARN" "$L/kernf-$1.txt")" \
	    "$(grep -c 'ZTS run ' "$L/kern-$1.txt")" \
	    "$(grep -c '^DEPOT SPLAT' "$L/kernf-$1.txt")" \
	    "$(wc -l < "$L/kern-$1.txt")"
}
# The lines a scan found, quoted.
kquote() {   # kquote <label>
	grep -iE "$KPAT|$KWARN" "$L/kernf-$1.txt" | head -n 20 | sed 's/^/  kernel: /'
}

# The highest 256k figures of the blake3-* and sha256-* rows of a copy of
# the kstat, read as the test reads them: "<blake3> <sha256> <row> <row>".
kfig() {   # kfig <file>
	awk '$1 == "implementation" { for (i = 2; i <= NF; i++) if ($i == "256k") col = i; next }
	    col && $1 ~ /^blake3-/ && $col + 0 > b { b = $col + 0; bn = $1 }
	    col && $1 ~ /^sha256-/ && $col + 0 > s { s = $col + 0; sn = $1 }
	    END { print b + 0, s + 0, (bn == "" ? "-" : bn), (sn == "" ? "-" : sn) }' "$1"
}
rule() {   # rule <blake3> <sha256>: what the default records for these figures
	if [ "$1" -gt 0 ] && [ "$2" -gt "$1" ]; then echo sha256; else echo blake3; fi
}
cname() {   # cname <recorded value>: the checksum it names
	case $1 in
	14) echo blake3 ;;
	8) echo sha256 ;;
	'') echo none ;;
	*) echo "other($1)" ;;
	esac
}

# Build <tree> in place, niced: autogen, configure (debug, data directory
# $SHARE), make -j8.
build_tree() {   # build_tree <B|M>
	stopped && return 1
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug --datadir=$SHARE && make -j8") \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "free=$(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -n 20 | tee -a $L/build.txt
		stop "the build of $1" | tee -a $L/build.txt
		return 1
	fi
}
EOF
bash -n /var/log/zts-ddb3-1028/env.sh; echo "env.sh syntax rc=$?"
. /var/log/zts-ddb3-1028/env.sh
date +%s > $L/start.txt
zu=""
for u in zts tigerblue77; do
	if [ -n "$(id -u $u 2>/dev/null)" ] && [ "$(id -u $u)" != 0 ] &&
	    sudo -n -l -U $u 2>/dev/null | grep -q NOPASSWD; then
		zu=$u
		break
	fi
done
echo "ZU=$zu" >> $L/env.sh
if [ "$(git -C $DOC rev-parse --show-toplevel 2>/dev/null)" = $DOC ] && [ -f $DOC/$DOCREL ]; then
	git -C $DOC rev-parse HEAD > $L/doc-head.txt
	sha256sum < $DOC/$DOCREL | cut -d' ' -f1 > $L/doc-sha.txt
fi
{
echo "start $(date -d @$(cat $L/start.txt) '+%F %T') ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
stat -c '%U %a %n' $W $L
echo "test user: ${zu:-NONE}"
echo "doc checkout: head $(cat $L/doc-head.txt 2>&1), $DOCREL sha256 $(cat $L/doc-sha.txt 2>&1)"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
[ -n "$zu" ] || stop "no test user with passwordless sudo"
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then `start <date> (<time>
UTC); root; <uname -r>; umask 0022`, two `stat` lines (`root 755` for `$W`
and `$L`), `test user: zts` (or `tigerblue77` if `zts` has no passwordless
sudo), the doc checkout line with its full head and the same sha256 as
STEP 0 printed, and the `zpool.cache` and free-space lines. A doc checkout
line with `No such file or directory` means this document was not read from
`$DOC`: record it, it is not a stop (STEP 8 then keeps `$DOC`). `test user:
NONE` means stop.

Then fetch the commit by its hash from the fork, and upstream master
(read-only towards GitHub), and pin them: the commit's parent, the parent in
upstream master, what the commit changes, its patch-ids, the new test's blob.
Where upstream master and the fork branch point now is recorded only:

```sh
. /var/log/zts-ddb3-1028/env.sh
if ! stopped; then
	{
	git init -q $W/repo > $L/init.log 2>&1; echo "init rc=$?"
	git -C $W/repo fetch -q --no-tags $FORK "+$HB:refs/heads/b" > $L/fetch-fork.log 2>&1; echo "fetch B rc=$?"
	git -C $W/repo fetch -q --no-tags $UPSTREAM "+refs/heads/master:refs/heads/upstream-master" > $L/fetch-upstream.log 2>&1; echo "fetch upstream rc=$?"
	cd $W/repo
	same "B" "$(git rev-parse -q --verify refs/heads/b)" $HB
	same "B's parent" "$(git rev-parse -q --verify $HB^)" $HM
	same "B's parent in upstream master" "$(git merge-base --is-ancestor $HM refs/heads/upstream-master && echo yes)" yes
	same "files B changes under tests/" "$(git diff --name-only $HM $HB -- tests | xargs)" "$TB"
	same "files B changes outside tests/" "$(git diff --name-only $HM $HB -- . ':!tests' | xargs)" "$CODE"
	same "diff stat" "$(git diff --stat $HM $HB | tail -n 1)" " 23 files changed, 461 insertions(+), 19 deletions(-)"
	same "patch-id" "$(git diff $HM $HB | git patch-id --stable | cut -d' ' -f1)" $PIDB
	same "patch-id under tests/" "$(git diff $HM $HB -- tests | git patch-id --stable | cut -d' ' -f1)" $PIDT
	same "the new test's blob" "$(git rev-parse -q --verify $HB:$F)" $BF
	same "compatibility.d unchanged by B" "$(git diff --name-only $HM $HB -- cmd/zpool/compatibility.d | wc -l)" 0
	for c in $HB $HM; do echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"; done
	echo "upstream master now (recorded only): $(git log -1 --abbrev=10 --format='%h %s' refs/heads/upstream-master)"
	echo "on GitHub now (recorded only): $(git ls-remote $FORK refs/heads/$BRANCH 2>&1 | awk '{ printf "%s=%.10s", $2, $1 }')"
	} 2>&1 | tee $L/sha-checks.txt
	grep -q '^MISMATCH' $L/sha-checks.txt && stop "the commits are not as expected"
	grep -qE '^(init|fetch [A-Za-z]+) rc=[1-9]' $L/sha-checks.txt && stop "the init or a fetch failed"
fi
```

Expected:

```
init rc=0
fetch B rc=0
fetch upstream rc=0
ok: B
ok: B's parent
ok: B's parent in upstream master
ok: files B changes under tests/
ok: files B changes outside tests/
ok: diff stat
ok: patch-id
ok: patch-id under tests/
ok: the new test's blob
ok: compatibility.d unchanged by B
  59efd7df84 Use the faster of BLAKE3 and SHA-256 for dedup=on on new pools
  a128b8a075 Linux 7.3 compat: META
upstream master now (recorded only): <its head>
on GitHub now (recorded only): refs/heads/dedup-default-blake3=59efd7df84
```

A `MISMATCH`, or a failed init or fetch, means stop. The last two lines are
not checked (the fetch is by hash, so a later push to the branch changes
nothing here).

Then one worktree per tree, B's test-side files into M (the commit's own
diff under `tests/`, applied to M's index and files), the compatibility
directory, and what each tree holds:

```sh
. /var/log/zts-ddb3-1028/env.sh
if ! stopped; then
	{
	for t in B:$HB M:$HM; do
		git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
		echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD 2>&1)"
	done
	git -C $W/repo diff --no-color --no-ext-diff $HM $HB -- tests > $L/tests.patch
	echo "tests.patch: $(grep -c '^diff --git ' $L/tests.patch) files, patch-id $(git -C $W/repo patch-id --stable < $L/tests.patch | cut -d' ' -f1)"
	git -C $W/M apply --index $L/tests.patch; echo "apply rc=$?"
	cd $W/M
	echo "M:$(git diff --cached --stat HEAD | tail -n 1)"
	same "M: what it changes" "$(git diff --cached --name-only HEAD | xargs)" "$TB"
	same "M: unstaged changes" "$(git diff --name-only | wc -l)" 0
	same "M: tests/ as in B" "$(git diff --cached $HB -- tests | wc -l)" 0
	same "M: differs from B in" "$(git diff --cached --name-only $HB | xargs)" "$CODE"
	same "B: as committed" "$(git -C $W/B status --porcelain --untracked-files=no | wc -l)" 0
	mkdir -p $SHARE/zfs && cp -R $W/B/cmd/zpool/compatibility.d $SHARE/zfs/ &&
	    echo "$CD: $(ls $CD | wc -l) files, openzfs-2.1-linux lists blake3 x$(grep -cx blake3 $CD/openzfs-2.1-linux)"
	same "$CD as M's" "$(diff -r $CD $W/M/cmd/zpool/compatibility.d 2>&1 | wc -l)" 0
	same "$CD as B's" "$(diff -r $CD $W/B/cmd/zpool/compatibility.d 2>&1 | wc -l)" 0
	k=$(uname -r | cut -d. -f1,2)
	for t in B M; do
		cd $W/$t
		echo "$t: test $(ls -l $F | cut -c1-10) blob $(git hash-object $F | cut -c1-10)," \
		    "registered x$(grep -c "'dedup_default_checksum'" tests/runfiles/common.run)" \
		    "x$(grep -c 'functional/dedup/dedup_default_checksum\.ksh' tests/zfs-tests/tests/Makefile.am)," \
		    "tunables.cfg x$(grep -c '^DEDUP_DEFAULT_CHECKSUM[[:space:]]' tests/zfs-tests/include/tunables.cfg);" \
		    "the nine: dedup=sha256 x$(cat $D9 | grep -c 'dedup=sha256') dedup=on x$(cat $D9 | grep -c 'dedup=on')"
		echo "$t: code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x$(grep -c DMU_POOL_DEDUP_CHECKSUM module/zfs/ddt.c)," \
		    "ddt_create_dedup_checksum in spa.c x$(grep -c 'ddt_create_dedup_checksum(spa, tx)' module/zfs/spa.c)," \
		    "chksum_stat_lock in zfs_chksum.c x$(grep -c chksum_stat_lock module/zfs/zfs_chksum.c)," \
		    "the tunable in ddt.c x$(grep -c 'default_checksum, UINT' module/zfs/ddt.c)"
		lo=$(awk '/^Linux-Minimum:/ { print $2 }' META)
		hi=$(awk '/^Linux-Maximum:/ { print $2 }' META)
		same "$t supports Linux $k (META: $lo to $hi)" \
		    "$([ -n "$lo" ] && [ -n "$hi" ] && [ "$(printf '%s\n' $lo $k $hi | sort -V | xargs)" = "$lo $k $hi" ] && echo within)" within
	done
	} 2>&1 | tee $L/trees.txt
	grep -q '^MISMATCH' $L/trees.txt && stop "the trees are not as expected"
	grep -qE '^(worktree [A-Z]+|apply) rc=[1-9]' $L/trees.txt && stop "a worktree or the apply failed"
fi
```

Expected:

```
worktree B rc=0 at 59efd7df84
worktree M rc=0 at a128b8a075
tests.patch: 13 files, patch-id 7a8f33caf94032340ec212ddb665a8f4e2af705d
apply rc=0
M: 13 files changed, 218 insertions(+), 12 deletions(-)
ok: M: what it changes
ok: M: unstaged changes
ok: M: tests/ as in B
ok: M: differs from B in
ok: B: as committed
/var/tmp/zts-ddb3-1028/share/zfs/compatibility.d: 25 files, openzfs-2.1-linux lists blake3 x0
ok: /var/tmp/zts-ddb3-1028/share/zfs/compatibility.d as M's
ok: /var/tmp/zts-ddb3-1028/share/zfs/compatibility.d as B's
B: test -rwxr-xr-x blob 7cc34dc7ec, registered x1 x1, tunables.cfg x1; the nine: dedup=sha256 x10 dedup=on x0
B: code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x2, ddt_create_dedup_checksum in spa.c x1, chksum_stat_lock in zfs_chksum.c x8, the tunable in ddt.c x1
ok: B supports Linux 6.12 (META: 4.18 to 7.3)
M: test -rwxr-xr-x blob 7cc34dc7ec, registered x1 x1, tunables.cfg x1; the nine: dedup=sha256 x10 dedup=on x0
M: code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x0, ddt_create_dedup_checksum in spa.c x0, chksum_stat_lock in zfs_chksum.c x0, the tunable in ddt.c x0
ok: M supports Linux 6.12 (META: 4.18 to 7.3)
```

(M carries B's tests byte for byte and none of its code; `openzfs-2.1-linux`
lacking blake3 is what pool 4 of the test relies on; `6.12` is the running
kernel's version, and another is fine as long as the two lines say `ok`.)
Anything else means stop.

## STEP 2: builds, checks, and the hand-over to the test user (all git work, part two)

Build B, then M (about 30 minutes together; see the rule about long
commands):

```sh
. /var/log/zts-ddb3-1028/env.sh
build_tree B && build_tree M
```

Expected: `build B rc=0 wall=<s>s free=<n>G`, then the same for M, the free
space going down by about 1G per tree. A failed build prints its `error:`
lines and `the build of <tree>: stop`: stop.

Then check what was built:

```sh
. /var/log/zts-ddb3-1028/env.sh
if ! stopped; then
	{
	for t in B M; do
		cd $W/$t
		same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
		same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1)" 3
		same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
		same "$t libzfs reads $CD" "$(grep -aqF "$CD" .libs/libzfs.so && echo yes || echo no)" yes
		same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb \
		    scripts/zfs.sh scripts/zfs-tests.sh tests/test-runner/bin/test-runner.py \
		    $F $D/setup.ksh $D/cleanup.ksh; do [ -e $f ] || echo $f; done | xargs)" ""
		modinfo -F srcversion module/zfs.ko > $L/srcv-$t 2>&1
		echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
		    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:')" \
		    "(objtool's, about 83 expected); srcversion $(cat $L/srcv-$t)"
		grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
		echo "$t last build line: $(tail -n 1 $L/build-$t.log)"
	done
	same "B and M srcversions differ" "$(cat $L/srcv-B $L/srcv-M | grep -xE '[0-9A-F]+' | sort -u | wc -l)" 2
	echo "M srcversion $(cat $L/srcv-M); master a128b8a075's debug builds on this VM: 8852710DB871A7587B9E722"
	echo "free after the builds: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (8G at least)"
	} 2>&1 | tee $L/step2.txt
	grep -q '^MISMATCH' $L/step2.txt && stop "the builds are not as expected"
	[ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 8 ] && stop "less than 8G free after the builds"
fi
```

Expected: 11 `ok:` lines (5 per tree, then the srcversions), for each tree
`compiler warnings: 0` and about 83 other `warning:` lines with its
srcversion, a last build line of a finished build (typically `make[1]:
Leaving directory '/var/tmp/zts-ddb3-1028/<B or M>'`), M's srcversion with
the earlier one, and the free space. Any `MISMATCH` means stop. A compiler
warning count other than 0 is not a stop: its first lines are printed,
quote them in the report. Another M srcversion is not a stop either,
provided every check here and in STEP 1 passed: report it.

Then point both trees' results at `$L/results`, store the two srcversions,
and hand the work directory to the test user. This is the last git command of
the task but STEP 8's check of the doc checkout:

```sh
. /var/log/zts-ddb3-1028/env.sh
if ! stopped; then
	{
	for t in B M; do
		sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
		    $W/$t/tests/test-runner/bin/test-runner.py.in \
		    $W/$t/tests/test-runner/bin/test-runner.py
		same "$t results directory" \
		    "$(grep -c "^BASEDIR = '$L/results'\$" $W/$t/tests/test-runner/bin/test-runner.py)" 1
	done
	same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
	same "M head" "$(git -C $W/M rev-parse HEAD)" $HM
	same "B changes now" "$(git -C $W/B diff HEAD --name-only | xargs)" tests/test-runner/bin/test-runner.py.in
	same "M changes now" "$(git -C $W/M diff HEAD --name-only | xargs)" \
	    "$(echo tests/test-runner/bin/test-runner.py.in $TB | xargs -n1 | LC_ALL=C sort | xargs)"
	same "M's test files are B's" "$(for f in $TB; do cmp -s $W/M/$f $W/B/$f || echo $f; done | xargs)" ""
	} 2>&1 | tee $L/step2b.txt
	if ! grep -q '^MISMATCH' $L/sha-checks.txt $L/trees.txt $L/step2.txt $L/step2b.txt &&
	    [ -s $L/srcv-B ] && [ -s $L/srcv-M ] && ! cmp -s $L/srcv-B $L/srcv-M; then
		printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh &&
		    echo "srcversions recorded" | tee -a $L/step2b.txt
		tail -n 3 $L/env.sh | tee -a $L/step2b.txt
		. $L/env.sh
		mkdir $W/files $L/results && chmod 1777 $W/files; echo "mkdir rc=$?" | tee -a $L/step2b.txt
		chown -R $ZU $W $L/results; echo "chown rc=$?" | tee -a $L/step2b.txt
		stat -c '%U %a %n' $W/files $L/results | tee -a $L/step2b.txt
	else
		stop "the checks before the hand-over" | tee -a $L/step2b.txt
	fi
fi
```

Expected: 7 `ok:` lines (the `test-runner.py.in` change is the results
directory, and nothing else may differ but M's 13 test files),
`srcversions recorded`, the lines `ZU=<the test user>`, `SV_B=<B's>` and
`SV_M=<M's>`, `mkdir rc=0`, `chown rc=0`, then `<ZU> 1777
/var/tmp/zts-ddb3-1028/files` and `<ZU> 755 /var/log/zts-ddb3-1028/results`.
`the checks before the hand-over: stop` means stop. `env.sh` and the
scripts stay root's, in `$L`.

## STEP 3: the scripts, and the baselines

`load.sh` loads a tree's modules with its own `scripts/zfs.sh -r`, behind
its guards:

```sh
cat > /var/log/zts-ddb3-1028/load.sh <<'EOS'
#!/bin/sh
# usage: load.sh <B|M>: load that tree's module stack with its own
# scripts/zfs.sh -r, which first unloads the one loaded.  Only with no pool,
# no zfs mount and no zed; then the srcversion must be the tree's and no
# pool may appear.  Records the tunables in $L/tun-<tree>.txt.  Exit 0: ok;
# 1: a guard failed, nothing done; 2: a bad argument; 3: the load failed, a
# pool appeared or the kernel logged a finding ($L/STOP written).
. /var/log/zts-ddb3-1028/env.sh
t=$1
case $t in
B) sv=$SV_B; want=0 ;;
M) sv=$SV_M; want=absent ;;
*) echo "usage: load.sh <B|M>"; exit 2 ;;
esac
stopped && exit 1
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M missing or equal: nothing loaded"; exit 1
fi
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
mods=$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)
p=""
[ "$cur" = "$SV_B" ] && p=$("$W/B/zpool" list -H -o name 2>&1)
[ "$cur" = "$SV_M" ] && p=$("$W/M/zpool" list -H -o name 2>&1)
m=$(grep -E ' zfs |testpool' /proc/mounts)
z=$(pgrep -ax 'zed|lt-zed'; ls /var/run/zed.pid 2>/dev/null)
echo "before the load of $t: modules [$mods] srcversion [$cur], pools [$p], mounts [$m], zed [$z]"
if [ -n "$mods" ] && [ "$cur" != "$SV_B" ] && [ "$cur" != "$SV_M" ]; then
	echo "a module of no tree of this task is loaded: nothing loaded"; exit 1
fi
if [ -n "$p$m$z" ]; then
	echo "a pool, a zfs mount or zed is there: nothing loaded"; exit 1
fi
start=$(date +%s)
(cd "$W/$t" && nice -n 10 ionice -c3 ./scripts/zfs.sh -r) > "$L/load-$t.log" 2>&1
rc=$?
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ $rc -ne 0 ] || [ "$cur" != "$sv" ]; then
	echo "load $t rc=$rc, srcversion [$cur], expected $sv"
	head -n 20 "$L/load-$t.log" | sed 's/^/  /'
	hold "the load of $t"
	exit 3
fi
p=$("$W/$t/zpool" list -H -o name 2>&1)
tunables > "$L/tun-$t.txt"
v=absent
[ -e $P ] && v=$(cat $P)
echo "load $t rc=0, srcversion $cur, pools [$p], zfs_dedup_default_checksum $v"
echo "  tunables: $(cat "$L/tun-$t.txt")"
set -- $(kscan "load-$t" "$start")
echo "kernel load-$t: $1 findings, $2 warnings, $4 depot splats ($5 lines)"
kquote "load-$t"
if [ -n "$p" ]; then
	hold "a pool appeared at the load of $t"; exit 3
fi
if [ "$1" -gt 0 ]; then
	hold "load-$t: kernel finding"; exit 3
fi
if [ "$v" != "$want" ]; then
	stop "zfs_dedup_default_checksum is $v after the load of $t, expected $want"; exit 3
fi
exit 0
EOS
```

`run.sh` runs one ZTS run on the loaded tree, as `$ZU`, behind the
srcversion guard, then writes its digest, reads its kernel messages from the
journal and checks its leftovers:

```sh
cat > /var/log/zts-ddb3-1028/run.sh <<'EOS'
#!/bin/sh
# usage: run.sh <B|M> <label> test    : dedup/dedup_default_checksum (-t)
#        run.sh <B|M> <label> group   : the dedup group (-T dedup)
# One zfs-tests.sh run on the loaded tree, as $ZU, under an outer timeout
# of $TMO s, then its digest, its kernel messages and its leftovers.
# Exit 0: the run is clean, whatever the test results; 1: a guard failed,
# nothing ran; 2: a bad argument; 3: a leftover, a changed tunable, a
# kernel finding, no ZTS mark in the journal, or the outer timeout
# ($L/STOP written).
. /var/log/zts-ddb3-1028/env.sh
t=$1; lbl=$2; mode=$3
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> test|group"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
case $mode in
test) sel="-t tests/functional/dedup/dedup_default_checksum.ksh" ;;
group) sel="-T dedup" ;;
*) echo "the mode is test or group"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
stopped && exit 1
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ] || [ -z "$ZU" ]; then
	echo "SV_B, SV_M or ZU missing, or SV_B equal to SV_M: nothing run"; exit 1
fi
G=$L/guards.txt

# The result file of test <name> in directory <dir> (the last component of
# its directory): its "merged" file, else its "stdout".
outfile() {   # outfile <results dir> <dir> <name>
	for f in merged stdout; do
		o=$(find "$1" -type f -name "$f" 2>/dev/null |
		    grep -E "/$2(:Linux)?/$3([.]ksh)?/$f\$" | head -n 1)
		if [ -n "$o" ]; then
			echo "$o"; return 0
		fi
	done
	return 1
}

# The lines of a result file before the failure callbacks and the cleanup,
# and the last of them: a passing test's is its log_pass message.
body() {   # body <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { exit } { print }' "$1"
}
endline() {   # endline <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
		print "  end line: " prev; exit }
	    NF { prev = $0 }' "$1"
}

# How far dedup_default_checksum got: the counts of its SUCCESS lines, its
# benchmark note checked against the rule, the lines of a failed check or
# command, and its end line.
ddc_digest() {   # ddc_digest <file>
	echo "  dedup_default_checksum: SUCCESS lines:" \
	    "save_tunable x$(grep -c 'SUCCESS: save_tunable DEDUP_DEFAULT_CHECKSUM' "$1")," \
	    "set_tunable32 x$(grep -c 'SUCCESS: set_tunable32 DEDUP_DEFAULT_CHECKSUM' "$1")," \
	    "zpool create x$(grep -c 'SUCCESS: zpool create' "$1")," \
	    "dd x$(grep -c 'SUCCESS: dd if=/dev/urandom' "$1")," \
	    "restore_tunable x$(grep -c 'SUCCESS: restore_tunable DEDUP_DEFAULT_CHECKSUM' "$1")"
	n=$(grep -m 1 'NOTE: chksum_bench at 256k:' "$1" | sed 's/^.*NOTE: //')
	echo "  note: ${n:-none}"
	# shellcheck disable=SC2046
	set -- $(echo "$n" | sed -n 's/^chksum_bench at 256k: blake3 \([0-9]*\) MiB\/s, sha256 \([0-9]*\) MiB\/s, expecting \([a-z0-9]*\) *$/\1 \2 \3/p')
	if [ $# -eq 3 ]; then
		r=$(rule "$1" "$2")
		if [ "$r" = "$3" ]; then
			echo "  the rule on the note's figures gives $r: agrees"
		else
			echo "  the rule on the note's figures gives $r: DISAGREES with the note's $3"
		fi
	fi
	body "$o" | grep -E 'ERROR: |cannot create|records dedup checksum|blocks are|feature@blake3 is' |
	    head -n 8 | sed 's/^/  line: /'
	endline "$o"
}

# Guards before the run: the loaded module first, so that no zpool command
# runs without this tree's module; then the tunables as recorded at the
# load, no pool, nothing in $W/files, loop and dm devices as at the start,
# no test mount, no test mount point, no zed.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion '$cur', want $sv: nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(files_left)
lo=CHANGED
loops_now | cmp -s "$L/loop-before.txt" - && lo=same
dm=CHANGED
dm_now | cmp -s "$L/dm-before.txt" - && dm=same
mnts=$(grep -E ' zfs |testpool' /proc/mounts)
roots=$(ls -d /testpool* 2>/dev/null | xargs)
zed=$(pgrep -ax 'zed|lt-zed'; ls /var/run/zed.pid 2>/dev/null)
echo "$(date -u +%T) before $lbl ($t $mode): srcversion=$cur pools=[$pools]" \
    "files=[$files] known=[$(files_known)] loops=$lo dm=$dm mounts=[$mnts]" \
    "rootdirs=[$roots] zed=[$zed]" | tee -a "$G"
if [ "$tun" != "$(cat "$L/tun-$t.txt" 2>&1)" ] || [ -n "$pools$files$mnts$roots$zed" ] ||
    [ "$lo$dm" != samesame ]; then
	echo "GUARD FAILED before $lbl: tunables [$tun]: nothing run" | tee -a "$G"
	exit 1
fi

start=$(date +%s)
cd "$W/$t" || exit 1
# $sel is two words on purpose.
# shellcheck disable=SC2086
timeout -k 60 "$TMO" nice -n 10 ionice -c3 runuser -u "$ZU" -- \
    ./scripts/zfs-tests.sh -vK -d "$W/files" $sel > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
# Two runs started in the same second would share a results directory.
sleep 2

# Results.  Each "Test:" / "Test (Linux):" line starts with a timestamp,
# which is dropped.  A -t run names the test with ".ksh", a group run not.
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z0-9_-]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
# "<dir> <name>" of every FAIL or KILLED result.
grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-tests.txt" |
    sed -E 's|^.*/([^/ ]+)/([^/ ]+) [(]run as .*$|\1 \2|; s|[.]ksh$||' \
    > "$L/$lbl-failed.txt"
set -- $(kscan "$lbl" "$start")
kn=$1; kw=$2; km=$3; kd=$4
{
echo "$lbl: tree $t, $mode, $(wc -l < "$L/$lbl-tests.txt") results: ${cnt:-none};" \
    "zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
echo "${miss:-no Missing util(s) line}"
cat "$L/$lbl-tests.txt"
if [ "$mode" = group ]; then
	sed -n '/^Results Summary/,/^Percent passed/p' "$L/$lbl.log"
	sed -n '/^Tests with /,$p' "$L/$lbl.log"
fi
while read -r dn n; do
	o=""
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" "$dn" "$n")
	if [ -n "$o" ]; then
		cp "$o" "$L/$lbl-$n-out.txt"
		echo "$n (failed):"
		grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
		endline "$o"
	else
		echo "$n (failed): no stdout or merged file found"
	fi
done < "$L/$lbl-failed.txt"
o=""
[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" dedup dedup_default_checksum)
if [ -n "$o" ]; then
	cp "$o" "$L/$lbl-ddc-out.txt"
	ddc_digest "$o"
else
	echo "  dedup_default_checksum: no stdout or merged file found"
fi
echo "  kernel: $kn findings, $kw warnings, $km ZTS marks, $kd depot splats"
kquote "$lbl"
} > "$L/$lbl-notes.txt" 2>&1

# Leftovers: all must be empty, loop and dm devices as at the start, and
# the tunables as after the load.  Processes are looked for leaving out
# this script and the shells that started it.
lp=$("$W/$t/zpool" list -H -o name 2>&1)
lf=$(files_left)
lk=$(files_known)
ll=CHANGED
loops_now | cmp -s "$L/loop-before.txt" - && ll=same
ld=CHANGED
dm_now | cmp -s "$L/dm-before.txt" - && ld=same
lm=$(grep -E ' zfs |testpool' /proc/mounts)
lr=$(ls -d /testpool* 2>/dev/null | xargs)
lz=$(pgrep -ax 'zed|lt-zed'; ls /var/run/zed.pid 2>/dev/null)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests[.]sh|test-runner[.]py|functional/dedup' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)
echo "$(date -u +%T) after $lbl: results=${cnt:-none} rc=$rc wall=${wall}s" \
    "zts_marks=$km kernel_findings=$kn kernel_warnings=$kw depot_splats=$kd" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] files=[$lf] known=[$lk]" \
    "loops=$ll dm=$ld mounts=[$lm] rootdirs=[$lr] zed=[$lz] procs=[$lq]" \
    "tunables_same=$ts" | tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ -n "$lp$lm$lq" ] || [ "$ll$ld" != samesame ] || [ "$kn" -gt 0 ]; then
	hold "after $lbl: a pool, a mount, a process or a device left, or a kernel finding" | tee -a "$G"
	exit 3
fi
if [ -n "$lf$lr$lz" ] || [ "$ts" != yes ] || [ "$km" -lt 1 ] ||
    [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	stop "after $lbl: a leftover, a tunable changed, no ZTS mark in the journal, or the outer timeout" | tee -a "$G"
	exit 3
fi
exit 0
EOS
```

`facts.sh` creates one pool on the loaded tree and reads what it records:

```sh
cat > /var/log/zts-ddb3-1028/facts.sh <<'EOS'
#!/bin/sh
# usage: facts.sh <B|M>: on the loaded tree, what a new pool records.  On B
# first the chksum_bench kstat (its first read runs the full benchmark,
# whose figures then stay for the life of the module).  Then the pool ddb3f
# on one sparse 1 GiB file in $W/facts-<tree>, its cache file there,
# dedup=on, compression=off, recordsize=128K, atime=off, mounted on
# $W/facts-<tree>/mnt: the entry of its pool directory, its debug-log line,
# feature@blake3, and the four level-0 blocks of a 512 KiB file of random
# data written with dedup=on; then the pool is destroyed and the directory
# removed.  A MISMATCH line is a result.  Exit 0: done; 1: a guard failed,
# nothing done; 2: a bad argument; 3: a command failed, a pool is left or
# the kernel logged a finding ($L/STOP written).
. /var/log/zts-ddb3-1028/env.sh
t=$1
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: facts.sh <B|M>"; exit 2 ;;
esac
stopped && exit 1
ZP=$W/$t/zpool
ZDB=$W/$t/zdb
FD=$W/facts-$t
PL=ddb3f
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ -z "$sv" ] || [ "$cur" != "$sv" ]; then
	echo "GUARD FAILED: srcversion '$cur', want '$sv': nothing done"; exit 1
fi
p=$("$ZP" list -H -o name 2>&1)
if [ -n "$p" ] || [ -e "$FD" ] || [ -e "/$PL" ]; then
	echo "GUARD FAILED: pools [$p], or $FD or /$PL exists: nothing done"; exit 1
fi
start=$(date +%s)
zp() { nice -n 10 ionice -c3 "$ZP" "$@"; }
zd() { nice -n 10 ionice -c3 "$ZDB" -U "$FD/cache" "$@"; }
# The end: destroy the pool if it is there, remove the directory, read the
# kernel log, and exit.
fin() {   # fin <rc>
	r=$1
	if [ -n "$(zp list -H -o name $PL 2>/dev/null)" ]; then
		zp destroy $PL; echo "destroy rc=$?"
	fi
	if [ -z "$(zp list -H -o name 2>&1)" ] && ! grep -q " $FD/" /proc/mounts; then
		rm -r "$FD" && echo "removed $FD"
	fi
	set -- $(kscan "facts-$t" "$start")
	echo "kernel facts-$t: $1 findings, $2 warnings, $4 depot splats ($5 lines)"
	kquote "facts-$t"
	lp=$(zp list -H -o name 2>&1)
	if [ "$1" -gt 0 ] || [ -n "$lp" ]; then
		hold "facts $t: a kernel finding or a pool left [$lp]"; exit 3
	fi
	if [ "$r" -ne 0 ]; then
		stop "facts $t: a command failed"; exit 3
	fi
	echo "facts $t done: records ${rec:-nothing}"
	exit 0
}

echo "facts $t, $(date -u '+%F %T') UTC, srcversion $cur"
echo "tunable zfs_dedup_default_checksum: $(cat $P 2>&1)"
if [ "$t" = B ]; then
	cat $KB > $L/chksum_bench-B.txt 2>&1
	echo "chksum_bench (the first read): its header and its sha256 and blake3 rows:"
	grep -E '^(implementation|sha256-|blake3-)' $L/chksum_bench-B.txt | sed 's/^/  /'
	kfig $L/chksum_bench-B.txt > $L/kfig-B.txt
	set -- $(cat $L/kfig-B.txt)
	kb=$1; ks=$2
	echo "fastest at 256k: blake3 $1 MiB/s ($3), sha256 $2 MiB/s ($4); the rule gives $(rule $1 $2)"
fi
mkdir "$FD" && truncate -s 1G "$FD/vdev" &&
    zp create -o cachefile="$FD/cache" -O dedup=on -O compression=off \
    -O recordsize=128K -O atime=off -O mountpoint="$FD/mnt" $PL "$FD/vdev"
rc=$?
echo "create rc=$rc"
[ $rc -eq 0 ] || fin 1
zd -dddd $PL 1 > $L/facts-$t-mos1.txt 2>&1; echo "zdb -dddd $PL 1 rc=$?"
rec=$(awk '$1 == "org.openzfs:dedup_checksum" { print $3 }' $L/facts-$t-mos1.txt)
e=$(grep -E '^[[:space:]]+org[.]openzfs:dedup_checksum = ' $L/facts-$t-mos1.txt | sed 's/^[[:space:]]*//; s/ *$//')
echo "pool directory: ${e:-no org.openzfs:dedup_checksum entry} ($(cname "$rec"))"
dl=$(grep "pool '$PL': dedup checksum" $DBG | tail -n 1 | sed 's/^[0-9]* *//')
echo "debug log: ${dl:-no line for $PL}"
echo "features: $(zp get -H -o property,value feature@blake3,feature@fast_dedup $PL 2>&1 | xargs)"
nice -n 10 ionice -c3 dd if=/dev/urandom of="$FD/mnt/f" bs=128k count=4 status=none &&
    zp sync $PL
rc=$?
echo "write rc=$rc"
[ $rc -eq 0 ] || fin 1
obj=$(stat -c %i "$FD/mnt/f")
zd -ddddddbbbbbb $PL/ "$obj" > $L/facts-$t-blocks.txt 2>&1; echo "zdb -ddddddbbbbbb $PL/ $obj rc=$?"
set -- $(awk '/L0 DVA/ { n++; if (/ blake3 .* dedup /) b++; if (/ sha256 .* dedup /) s++ }
    END { print b + 0, s + 0, n + 0 }' $L/facts-$t-blocks.txt)
nb=$1; ns=$2; nl=$3
echo "blocks of the 512 KiB file (object $obj): $nb blake3 dedup, $ns sha256 dedup, of $nl L0 blocks"
echo "  first: $(grep -m 1 'L0 DVA' $L/facts-$t-blocks.txt | sed 's/^ *//')"
zd -D $PL > $L/facts-$t-ddt.txt 2>&1; echo "zdb -D $PL rc=$?"
grep -E '^DDT-|^dedup = ' $L/facts-$t-ddt.txt | sed 's/^/  /'
ddt=$(grep -oE '^DDT-[a-z0-9]+:' $L/facts-$t-ddt.txt | sort -u | xargs)
fa=$(zp get -H -o value feature@blake3 $PL 2>&1)
echo "feature@blake3 after the write: $fa"
if [ "$t" = B ]; then
	c=$(cname "$rec")
	same "B: the entry is what the rule gives on the kstat's figures" "$c" "$(rule $kb $ks)"
	same "B: the debug log's figures are the kstat's" \
	    "$(echo "$dl" | sed -n 's/^.*(blake3 \([0-9]*\) MiB\/s, sha256 \([0-9]*\) MiB\/s).*$/\1 \2/p')" "$kb $ks"
	same "B: the debug log names the recorded checksum" \
	    "$(echo "$dl" | sed -n 's/^.*: dedup checksum \([a-z0-9]*\) (.*$/\1/p')" "$c"
	if [ "$c" = blake3 ]; then
		same "B: the four blocks are blake3 dedup blocks" "$nb $ns $nl" "4 0 4"
		same "B: the DDT" "$ddt" "DDT-blake3:"
		same "B: feature@blake3 after the write" "$fa" active
	else
		same "B: the four blocks are sha256 dedup blocks" "$nb $ns $nl" "0 4 4"
		same "B: the DDT" "$ddt" "DDT-sha256:"
		same "B: feature@blake3 after the write" "$fa" enabled
	fi
else
	same "M: no entry" "${rec:-none}" none
	same "M: no debug log line" "${dl:-none}" none
	same "M: the four blocks are sha256 dedup blocks" "$nb $ns $nl" "0 4 4"
	same "M: the DDT" "$ddt" "DDT-sha256:"
	same "M: feature@blake3 after the write" "$fa" enabled
fi
fin 0
EOS
```

`meas.sh` makes one timed write of the measurement (bash, for the fallback
to its `time` keyword):

```sh
cat > /var/log/zts-ddb3-1028/meas.sh <<'EOS'
#!/bin/bash
# usage: meas.sh <label> <on|sha256>: on B, one timed write.  A fresh pool
# ddb3m on one sparse 4 GiB file in $W/meas/<label>, its cache file there,
# compression=off, recordsize=128K, atime=off, dedup=<on|sha256>, mounted
# on $W/meas/mnt.  $W/meas/src is read once into the page cache, then copied
# in with dd bs=1M and flushed with zpool sync, timed by /usr/bin/time
# (wall, and the user and system time of dd and zpool); /proc/stat gives
# the CPU time of the whole VM over the same interval, the kernel's zio
# threads, which compute the checksums, included.  Then the pool's entry
# and DDT; the pool is destroyed and $W/meas/<label> removed.  One line in
# $L/meas.tsv.  Exit 0: done; 1: a guard failed, nothing done; 2: a bad
# argument; 3: a command failed, a pool is left or the kernel logged a
# finding ($L/STOP written).
. /var/log/zts-ddb3-1028/env.sh
lbl=$1; v=$2
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
case $v in
on|sha256) ;;
*) echo "usage: meas.sh <label> <on|sha256>"; exit 2 ;;
esac
stopped && exit 1
ZP=$W/B/zpool
ZDB=$W/B/zdb
MD=$W/meas/$lbl
MP=$W/meas/mnt
SRC=$W/meas/src
PL=ddb3m
cur=$(cat /sys/module/zfs/srcversion 2>&1)
p=$([ "$cur" = "$SV_B" ] && "$ZP" list -H -o name 2>&1)
if [ -z "$SV_B" ] || [ "$cur" != "$SV_B" ] || [ -n "$p" ] || [ -e "$MD" ] ||
    [ "$(stat -c %s "$SRC" 2>&1)" != 2147483648 ] || [ "$(cat $P 2>&1)" != 0 ] ||
    grep -q "^$lbl	" $L/meas.tsv 2>/dev/null; then
	echo "GUARD FAILED: srcversion '$cur' (want B's), pools [$p], $MD exists," \
	    "$SRC is not 2 GiB, the tunable is not 0, or $lbl was measured: nothing done"
	exit 1
fi
start=$(date +%s)
zp() { nice -n 10 ionice -c3 "$ZP" "$@"; }
fin() {   # fin <rc>
	r=$1
	if [ -n "$(zp list -H -o name $PL 2>/dev/null)" ]; then
		zp destroy $PL || echo "destroy failed"
	fi
	if [ -z "$(zp list -H -o name 2>&1)" ] && ! grep -q " $MP " /proc/mounts; then
		rm -r "$MD" || echo "rm of $MD failed"
	fi
	set -- $(kscan "meas-$lbl" "$start")
	echo "  kernel meas-$lbl: $1 findings, $2 warnings, $4 depot splats ($5 lines)"
	kquote "meas-$lbl"
	lp=$(zp list -H -o name 2>&1)
	if [ "$1" -gt 0 ] || [ -n "$lp" ]; then
		hold "meas $lbl: a kernel finding or a pool left [$lp]"; exit 3
	fi
	if [ "$r" -ne 0 ]; then
		stop "meas $lbl: a command failed"; exit 3
	fi
	exit 0
}
mkdir "$MD" && truncate -s 4G "$MD/vdev" &&
    zp create -o cachefile="$MD/cache" -O dedup=$v -O compression=off \
    -O recordsize=128K -O atime=off -O mountpoint="$MP" $PL "$MD/vdev" &&
    zp sync $PL
rc=$?
if [ $rc -ne 0 ]; then
	echo "$lbl: create rc=$rc"; fin 1
fi
cat "$SRC" > /dev/null
c0=$(head -n 1 /proc/stat)
if [ -x /usr/bin/time ]; then
	timer=/usr/bin/time
	nice -n 10 ionice -c3 /usr/bin/time -f '%e %U %S' -o "$L/meas-$lbl.time" \
	    sh -c "dd if=$SRC of=$MP/data bs=1M status=none && $ZP sync $PL"
	rc=$?
else
	timer=bash
	TIMEFORMAT='%R %U %S'
	{ time nice -n 10 ionice -c3 sh -c "dd if=$SRC of=$MP/data bs=1M status=none && $ZP sync $PL" ; } 2> "$L/meas-$lbl.time"
	rc=$?
fi
c1=$(head -n 1 /proc/stat)
read -r tw tu ts < <(tail -n 1 "$L/meas-$lbl.time")
read -r kk ku kio kst < <(printf '%s\n%s\n' "$c0" "$c1" | awk -v hz="$(getconf CLK_TCK)" '
    NR == 1 { for (i = 2; i <= 9; i++) a[i] = $i; next }
    { for (i = 2; i <= 9; i++) d[i] = $i - a[i]
      printf "%.2f %.2f %.2f %.2f\n", (d[4] + d[7] + d[8]) / hz, (d[2] + d[3]) / hz, d[6] / hz, d[9] / hz }')
"$ZDB" -U "$MD/cache" -dddd $PL 1 > "$L/meas-$lbl-mos1.txt" 2>&1
rec=$(awk '$1 == "org.openzfs:dedup_checksum" { print $3 }' "$L/meas-$lbl-mos1.txt")
"$ZDB" -U "$MD/cache" -D $PL > "$L/meas-$lbl-ddt.txt" 2>&1
ddt=$(grep -oE '^DDT-[a-z0-9]+:' "$L/meas-$lbl-ddt.txt" | sort -u | xargs)
al=$(zp list -Hp -o allocated $PL 2>&1)
mbs=$(awk -v w="$tw" 'BEGIN { if (w > 0) printf "%.0f", 2048 / w; else print "?" }')
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$lbl" "$v" "$(cname "$rec")" \
    "$tw" "$tu" "$ts" "$kk" "$ku" "$kio" "$kst" "$mbs" "$ddt" "$al" "$rc" >> $L/meas.tsv
echo "$lbl: dedup=$v, the pool records $(cname "$rec"); wall $tw s, user $tu s, sys $ts s" \
    "(dd and zpool sync, $timer); VM CPU: kernel $kk s, user $ku s, iowait $kio s," \
    "steal $kst s; $mbs MiB/s; DDT [$ddt]; allocated $al; rc=$rc"
grep -E '^DDT-' "$L/meas-$lbl-ddt.txt" | sed 's/^/  /'
[ $rc -eq 0 ] || fin 1
fin 0
EOS
```

Then the checks of the four scripts, and the baselines the runs compare
with:

```sh
. /var/log/zts-ddb3-1028/env.sh
{
for f in load.sh run.sh facts.sh; do
	sh -n $L/$f; echo "$f sh syntax rc=$?"
	bash -n $L/$f; echo "$f bash syntax rc=$?"
done
bash -n $L/meas.sh; echo "meas.sh bash syntax rc=$?"
loops_now > $L/loop-before.txt
dm_now > $L/dm-before.txt
echo "baseline: loop devices [$(xargs < $L/loop-before.txt)], dm [$(xargs < $L/dm-before.txt)]," \
    "zpool.cache $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
echo "kernel this boot: $(journalctl -k -b --no-pager -q | kfilter | grep -ciE "$KPAT") findings," \
    "$(journalctl -k -b --no-pager -q | kfilter | grep -c '^DEPOT SPLAT') depot splats"
ls -l $L | sed 1d | awk '{ print $1, $3, $NF }'
} 2>&1 | tee $L/step3.txt
```

Expected: seven `syntax rc=0` lines, the baseline line (loop and dm devices
as STEP 0 printed them, recorded only; `zpool.cache` absent or 0 bytes), the
kernel line (recorded; the first number as STEP 0's baseline), and the files
of `$L`, `root`'s, with `env.sh`, the four scripts, `results` (the test
user's), and the logs so far. A `syntax rc` other than 0 means the scripts
were not written as printed here: stop.

## STEP 4: load B, and the facts on B

Load B (nothing is loaded yet, so the reload loads). **Nothing may read
`/proc/spl/kstat/zfs/chksum_bench` before `facts.sh B`**: its first read is
the one that runs the full benchmark, and the facts are made of its figures.

```sh
L=/var/log/zts-ddb3-1028
sh $L/load.sh B 2>&1 | tee $L/load-B.txt; sh $L/facts.sh B > $L/facts-B.txt 2>&1; echo "facts.sh rc=$?" >> $L/facts-B.txt; cat $L/facts-B.txt
```

Expected from `load.sh`: `before the load of B: modules [] srcversion [],
pools [], mounts [], zed []`, `load B rc=0, srcversion <SV_B>, pools [],
zfs_dedup_default_checksum 0`, the tunables line (recorded), and `kernel
load-B: 0 findings, 0 warnings, 0 depot splats (<n> lines)`. Then from
`facts.sh`:

```
facts B, <date> UTC, srcversion <SV_B>
tunable zfs_dedup_default_checksum: 0
chksum_bench (the first read): its header and its sha256 and blake3 rows:
  implementation              1k      4k     16k     64k    256k      1m      4m     16m
  sha256-generic  ...
  ... (one row per implementation this CPU has)
  blake3-generic  ...
  ...
fastest at 256k: blake3 <n> MiB/s (blake3-<impl>), sha256 <n> MiB/s (sha256-<impl>); the rule gives <c>
create rc=0
zdb -dddd ddb3f 1 rc=0
pool directory: org.openzfs:dedup_checksum = <14 or 8> (<c>)
debug log: pool 'ddb3f': dedup checksum <c> (blake3 <n> MiB/s, sha256 <n> MiB/s)
features: feature@blake3 enabled feature@fast_dedup enabled
write rc=0
zdb -ddddddbbbbbb ddb3f/ <obj> rc=0
blocks of the 512 KiB file (object <obj>): <4 0, or 0 4> blake3 dedup, ... of 4 L0 blocks
  first: <obj's first L0 DVA line, with <c> and dedup>
zdb -D ddb3f rc=0
  DDT-<c>: version=1 [FDT]; flags=0x03 [ FLAT LOG]; rootobj=<n>
  <the DDT's object and log lines>
  dedup = 1.00, compress = 1.00, copies = 1.00, dedup * compress / copies = 1.00
feature@blake3 after the write: <active for blake3, enabled for sha256>
ok: B: the entry is what the rule gives on the kstat's figures
ok: B: the debug log's figures are the kstat's
ok: B: the debug log names the recorded checksum
ok: B: the four blocks are <c> dedup blocks
ok: B: the DDT
ok: B: feature@blake3 after the write
destroy rc=0
removed /var/tmp/zts-ddb3-1028/facts-B
kernel facts-B: 0 findings, 0 warnings, 0 depot splats (<n> lines)
facts B done: records <14 or 8>
facts.sh rc=0
```

`<c>` is `blake3` or `sha256`, whichever the rule gives on the printed
figures; the figures, the implementations and `<c>` are the result. The
`DDT-` and `dedup =` lines are recorded only. `load.sh` exiting other than 0
(a `stop` line), or `facts.sh rc=1` or `rc=3`, means stop; a `MISMATCH` line
of `facts.sh` is a result against the commit, recorded, not a stop.

## STEP 5: B: the test 3 times, then the dedup group

`dedup_default_checksum` 3 times (a few minutes each), each run stopping
the series if it does not exit 0:

```sh
L=/var/log/zts-ddb3-1028
for l in b1 b2 b3; do sh $L/run.sh B $l test > $L/$l-console.txt 2>&1; r=$?; echo "run.sh $l rc=$r" | tee -a $L/$l-console.txt; [ $r -eq 0 ] || break; done; cat $L/b[123]-console.txt
```

Expected, for each of `b1`, `b2`, `b3`, after its `before` line:

```
<time> after b<i>: results=PASS 3 rc=0 wall=<s>s zts_marks=3 kernel_findings=0 kernel_warnings=0 depot_splats=0
<time> after b<i> leftovers: pools=[] files=[] known=[] loops=same dm=same mounts=[] rootdirs=[] zed=[] procs=[] tunables_same=yes
b<i>: tree B, test, 3 results: PASS 3; zfs-tests.sh rc=0, wall <s>s, log dir /var/log/zts-ddb3-1028/results/<timestamp>
no Missing util(s) line
Test: /var/tmp/zts-ddb3-1028/B/tests/zfs-tests/tests/functional/dedup/setup (run as root) [00:00] [PASS]
Test: /var/tmp/zts-ddb3-1028/B/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-ddb3-1028/B/tests/zfs-tests/tests/functional/dedup/cleanup (run as root) [00:00] [PASS]
  dedup_default_checksum: SUCCESS lines: save_tunable x1, set_tunable32 x5, zpool create x5, dd x14, restore_tunable x1
  note: chksum_bench at 256k: blake3 <n> MiB/s, sha256 <n> MiB/s, expecting <c>
  the rule on the note's figures gives <c>: agrees
  end line: dedup=on uses the checksum recorded when the pool was created
  kernel: 0 findings, 0 warnings, 3 ZTS marks, 0 depot splats
run.sh b<i> rc=0
```

The note's figures are expected to be STEP 4's (frozen since its read);
STEP 8 compares. A `Missing util(s):` line is recorded (it must not list
`zdb`, `zpool` or `zfs`). A FAIL is a result, recorded, not a stop: its
`line:` entries and end line say where (`<ds>: <n> of 4 blocks are <c> dedup
blocks`, `testpool records dedup checksum '<got>', expected '<want>'`,
`feature@blake3 is <state> on testpool, expected <state>`, or `ERROR:
<command> exited <n>`), and the count of `SUCCESS: dd` lines says how far:
a block check that fails has written its own file. A `run.sh` exit other
than 0 is a stop (its `GUARD FAILED`, `stop` or `usage` line says why).

Then the dedup group (15 to 30 minutes; see the rule about long commands):

```sh
L=/var/log/zts-ddb3-1028
sh $L/run.sh B bg group > $L/bg-console.txt 2>&1; echo "run.sh bg rc=$?" | tee -a $L/bg-console.txt; cat $L/bg-console.txt
```

Expected: `after bg: results=PASS 20 rc=0 ... zts_marks=20 kernel_findings=0
kernel_warnings=0 depot_splats=0`, every leftover empty as above, `bg: tree
B, group, 20 results: PASS 20; zfs-tests.sh rc=0`, the 20 `Test:` and
`Test (Linux):` lines (the 17 tests of `common.run`'s section, then
`linux.run`'s `setup`, `dedup_legacy_gang` and `cleanup`), the Results
Summary with `PASS 20`, the `dedup_default_checksum` lines as in `b1`, and
`run.sh bg rc=0`. A test that fails is listed with its `ERROR:` lines and
end line (`<name> (failed):`): it is recorded, not a stop.

## STEP 6: B: the measurement

The source (2 GiB of random data), then the six runs in their order, then
the source removed (about 5 to 15 minutes):

```sh
. /var/log/zts-ddb3-1028/env.sh
if ! stopped; then
	{
	echo "free before the measurement: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (7G at least)"
	if [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 7 ]; then
		stop "less than 7G free for the measurement"
	elif mkdir $W/meas && nice -n 10 ionice -c3 dd if=/dev/urandom of=$W/meas/src bs=1M count=2048 iflag=fullblock status=none; then
		echo "src: $(stat -c %s $W/meas/src) bytes; timer: $([ -x /usr/bin/time ] && echo /usr/bin/time || echo "bash's time")"
		for l in on1:on sha1:sha256 on2:on sha2:sha256 on3:on sha3:sha256; do
			bash $L/meas.sh ${l%%:*} ${l#*:}; r=$?
			echo "meas.sh ${l%%:*} rc=$r"
			[ $r -eq 0 ] || break
		done
		rm $W/meas/src; echo "rm src rc=$?"
	else
		stop "the source of the measurement"
	fi
	} 2>&1 | tee $L/meas-step.txt
fi
```

Expected: the free space, `src: 2147483648 bytes; timer: /usr/bin/time` (or
`bash's time`), then for each of `on1`, `sha1`, `on2`, `sha2`, `on3`, `sha3`:

```
<label>: dedup=<on or sha256>, the pool records <c>; wall <s> s, user <s> s, sys <s> s (dd and zpool sync, /usr/bin/time); VM CPU: kernel <s> s, user <s> s, iowait <s> s, steal <s> s; <n> MiB/s; DDT [DDT-<c or sha256>:]; allocated <n>; rc=0
  <the DDT lines>
  kernel meas-<label>: 0 findings, 0 warnings, 0 depot splats (<n> lines)
meas.sh <label> rc=0
```

with `<c>` the checksum STEP 4's pool recorded, `DDT [DDT-<c>:]` for the
`on` runs and `DDT [DDT-sha256:]` for the `sha` runs, `allocated` about
2.2 GB, then `rm src rc=0`. The figures are the result, recorded. A
`meas.sh` exit other than 0 is a stop. Then the summary:

```sh
. /var/log/zts-ddb3-1028/env.sh
awk -F'\t' '
function med(s,    a, n, i, j, x) {
	n = split(s, a, " ")
	for (i = 2; i <= n; i++) { x = a[i]; for (j = i - 1; j > 0 && a[j] + 0 > x + 0; j--) a[j + 1] = a[j]; a[j + 1] = x }
	return (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
}
{ w[$2] = w[$2] " " $4; k[$2] = k[$2] " " $7; c[$2] = c[$2] " " $3; n[$2]++ }
END {
	for (v in n)
		printf "dedup=%s (records%s): %d runs; wall median %s s [%s ]; VM kernel CPU median %s s [%s ]; %.0f MiB/s at the median\n",
		    v, c[v], n[v], med(w[v]), w[v], med(k[v]), k[v], 2048 / med(w[v])
	if (n["on"] && n["sha256"])
		printf "sha256 / on: wall x%.2f, VM kernel CPU x%.2f\n", med(w["sha256"]) / med(w["on"]), med(k["sha256"]) / med(k["on"])
}' $L/meas.tsv 2>&1 | sort | tee $L/meas-summary.txt
echo "the dedup=on pools record: $(cut -f3 $L/meas.tsv | paste -sd' ' -) (STEP 4: $(cname "$(sed -n 's/^facts B done: records //p' $L/facts-B.txt)"))" | tee -a $L/meas-summary.txt
```

Expected: one line per variant, `dedup=on (records <c> <c> <c>): 3 runs;
...` and `dedup=sha256 (records <c> <c> <c>): 3 runs; ...`, the line `sha256 /
on: wall x<r>, VM kernel CPU x<r>`, and `the dedup=on pools record: <the six
pools' entries> (STEP 4: <c>)`: every pool records STEP 4's `<c>` (the
`sha256` pools too: the entry is the pool's default, which their
`dedup=sha256` overrides). If `<c>` is `sha256`, both variants wrote SHA-256
and the ratios only show the noise. The figures are recorded, never a stop.

## STEP 7: M: swap, the facts, the test 3 times, the dedup group

Swap to M only while B's module is loaded with no pool, no mount and no
`zed`; `load.sh M` checks it, and `zfs.sh -r` from M's tree unloads B's
stack and loads M's:

```sh
L=/var/log/zts-ddb3-1028
sh $L/load.sh M 2>&1 | tee $L/load-M.txt; sh $L/facts.sh M > $L/facts-M.txt 2>&1; echo "facts.sh rc=$?" >> $L/facts-M.txt; cat $L/facts-M.txt
```

Expected from `load.sh`: `before the load of M: modules [zfs spl]
srcversion [<SV_B>], pools [], mounts [], zed []`, `load M rc=0, srcversion
<SV_M>, pools [], zfs_dedup_default_checksum absent`, the tunables line with
`zfs_dedup_default_checksum=absent` and the others as B's, and `kernel
load-M: 0 findings, ...`. Then from `facts.sh`:

```
facts M, <date> UTC, srcversion <SV_M>
tunable zfs_dedup_default_checksum: cat: /sys/module/zfs/parameters/zfs_dedup_default_checksum: No such file or directory
create rc=0
zdb -dddd ddb3f 1 rc=0
pool directory: no org.openzfs:dedup_checksum entry (none)
debug log: no line for ddb3f
features: feature@blake3 enabled feature@fast_dedup enabled
write rc=0
zdb -ddddddbbbbbb ddb3f/ <obj> rc=0
blocks of the 512 KiB file (object <obj>): 0 blake3 dedup, 4 sha256 dedup, of 4 L0 blocks
  first: <the L0 DVA line, with sha256 and dedup>
zdb -D ddb3f rc=0
  DDT-sha256: version=1 [FDT]; flags=0x03 [ FLAT LOG]; rootobj=<n>
  ...
feature@blake3 after the write: enabled
ok: M: no entry
ok: M: no debug log line
ok: M: the four blocks are sha256 dedup blocks
ok: M: the DDT
ok: M: feature@blake3 after the write
destroy rc=0
removed /var/tmp/zts-ddb3-1028/facts-M
kernel facts-M: 0 findings, 0 warnings, 0 depot splats (<n> lines)
facts M done: records nothing
facts.sh rc=0
```

This is master's behaviour with the default features: no entry, and
`dedup=on` writes SHA-256 where B's pool 1 expects BLAKE3. Stops and results
as in STEP 4.

Then `dedup_default_checksum` 3 times:

```sh
L=/var/log/zts-ddb3-1028
for l in m1 m2 m3; do sh $L/run.sh M $l test > $L/$l-console.txt 2>&1; r=$?; echo "run.sh $l rc=$r" | tee -a $L/$l-console.txt; [ $r -eq 0 ] || break; done; cat $L/m[123]-console.txt
```

Expected, for each of `m1`, `m2`, `m3` (see "Why M fails the new test"):

```
<time> after m<i>: results=FAIL 1, PASS 2 rc=<n> wall=<s>s zts_marks=3 kernel_findings=0 kernel_warnings=0 depot_splats=0
<time> after m<i> leftovers: pools=[] files=[] known=[] loops=same dm=same mounts=[] rootdirs=[] zed=[] procs=[] tunables_same=yes
m<i>: tree M, test, 3 results: FAIL 1, PASS 2; zfs-tests.sh rc=<n>, wall <s>s, log dir /var/log/zts-ddb3-1028/results/<timestamp>
no Missing util(s) line
Test: /var/tmp/zts-ddb3-1028/M/tests/zfs-tests/tests/functional/dedup/setup (run as root) [00:00] [PASS]
Test: /var/tmp/zts-ddb3-1028/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [FAIL]
Test: /var/tmp/zts-ddb3-1028/M/tests/zfs-tests/tests/functional/dedup/cleanup (run as root) [00:00] [PASS]
dedup_default_checksum (failed):
  ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1
  end line: ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1
  dedup_default_checksum: SUCCESS lines: save_tunable x1, set_tunable32 x0, zpool create x0, dd x0, restore_tunable x1
  note: none
  line: <the shell's message, ending in cannot create [Permission denied]>
  line: ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1
  end line: ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1
  kernel: 0 findings, 0 warnings, 3 ZTS marks, 0 depot splats
run.sh m<i> rc=0
```

`zfs-tests.sh rc` is recorded (non-zero for an unexpected failure). The
shell's message is recorded as printed. A FAIL elsewhere (another `ERROR:`
line, a `zpool create` that succeeded) is a result, recorded.

Then the dedup group (15 to 30 minutes):

```sh
L=/var/log/zts-ddb3-1028
sh $L/run.sh M mg group > $L/mg-console.txt 2>&1; echo "run.sh mg rc=$?" | tee -a $L/mg-console.txt; cat $L/mg-console.txt
```

Expected: `after mg: results=FAIL 1, PASS 19 rc=<n> ... zts_marks=20
kernel_findings=0 kernel_warnings=0 depot_splats=0`, every leftover empty,
`mg: tree M, group, 20 results: FAIL 1, PASS 19`, the 20 lines with
`dedup_default_checksum` the one `[FAIL]`, the Results Summary with `PASS
19` and `FAIL 1`, its `(failed)` lines and its digest as in `m1`, and
`run.sh mg rc=0`. Any other test failing on M is a result, recorded.

## STEP 8: the comparison, the unload, the report, the cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the comparison:

```sh
. /var/log/zts-ddb3-1028/env.sh
{
echo "per run: results, rc, wall; the test's lines; the kernel"
for l in b1 b2 b3 bg m1 m2 m3 mg; do
	f=$L/$l-notes.txt
	if [ ! -e $f ]; then
		echo "$l: not run"
		continue
	fi
	head -n 1 $f | sed -E 's/, log dir .*$//'
	grep -E '^  (dedup_default_checksum: SUCCESS|note:|the rule|line:|end line:|kernel:)' $f | sed 's/^  /    /'
done
echo "B's figures at 256k (blake3 sha256): STEP 4 [$(cut -d' ' -f1,2 $L/kfig-B.txt 2>&1)]"
for l in b1 b2 b3 bg; do
	n=$(sed -n 's/^  note: chksum_bench at 256k: blake3 \([0-9]*\) MiB\/s, sha256 \([0-9]*\) MiB\/s, .*$/\1 \2/p' $L/$l-notes.txt 2>/dev/null)
	echo "  $l's note [${n:-none}]: $([ "$n" = "$(cut -d' ' -f1,2 $L/kfig-B.txt 2>/dev/null)" ] && echo same || echo differs)"
done
echo "facts: B $(tail -n 2 $L/facts-B.txt 2>/dev/null | head -n 1); M $(tail -n 2 $L/facts-M.txt 2>/dev/null | head -n 1)"
grep -h '^MISMATCH' $L/facts-B.txt $L/facts-M.txt 2>/dev/null
cat $L/meas-summary.txt 2>/dev/null
} 2>&1 | tee $L/compare.txt
```

Expected: for `b1` to `b3` the `PASS 3` lines with the digest of STEP 5, for
`bg` `PASS 20`, for `m1` to `m3` `FAIL 1, PASS 2` with the digest of STEP 7,
for `mg` `FAIL 1, PASS 19`; STEP 4's two figures and `same` for each of the
four notes; `facts: B facts B done: records <14 or 8>; M facts M done:
records nothing`, no `MISMATCH` line, and the measurement summary. A note
that `differs` is recorded (it would mean the figures moved after STEP 4's
read), as is anything not run.

Then the whole task's kernel check and the unload; the module stays loaded
on `HOLD`, and then no `zpool` command runs:

```sh
. /var/log/zts-ddb3-1028/env.sh
{
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
t=""
[ -n "$cur" ] && [ "$cur" = "$SV_B" ] && t=B
[ -n "$cur" ] && [ "$cur" = "$SV_M" ] && t=M
echo "loaded: [${t:-none} $cur]; HOLD [$(cat $L/HOLD 2>/dev/null)]; STOP [$(cat $L/STOP 2>/dev/null)]"
if [ -e $L/HOLD ]; then
	echo "module left loaded: HOLD; no pool command run"
elif [ -n "$t" ]; then
	p=$($W/$t/zpool list -H -o name 2>&1)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	if [ -n "$p$m" ]; then
		echo "a pool or a mount is left ($p $m): module left loaded"
	elif [ -e /var/run/zed.pid ]; then
		echo "/var/run/zed.pid exists: module left loaded"
	else
		(cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"
	fi
else
	echo "no module of this task is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
set -- $(kscan all "$(cat $L/start.txt)")
echo "kernel, whole task: $1 findings, $2 warnings, $3 ZTS marks, $4 depot splats"
grep -iE "$KPAT|$KWARN" $L/kernf-all.txt | grep -vE 'kernel: +(\? )?[a-z_0-9.]+\+0x' | head -n 40
echo "kernel, this boot: $(journalctl -k -b --no-pager -q | kfilter | grep -ciE "$KPAT")"
echo "loop devices as at the start: $(loops_now | cmp -s $L/loop-before.txt - && echo yes || echo NO)"
echo "dm devices as at the start: $(dm_now | cmp -s $L/dm-before.txt - && echo yes || echo NO)"
echo "mounts: [$(grep -E ' zfs |testpool' /proc/mounts)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
ls -d /testpool* /ddb3f /ddb3m 2>&1
echo "files: [$(ls -A $W/files 2>&1)]"
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/end.txt
```

Expected: `loaded: [M <SV_M>]; HOLD []; STOP []`, `unload M rc=0`, `lsmod:
[]`, `kernel, whole task: 0 findings, 0 warnings, 46 ZTS marks, 0 depot
splats` (23 per tree: 3 x 3 and 20), no kernel line, the boot's count equal
to STEP 0's baseline, `yes` twice, `mounts: []`, `zpool.cache` absent or 0
bytes, no `vdev_id.conf`, three `No such file or directory` lines, `files:
[]`, `W holds: B M files meas repo share`, and the free space.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-ddb3-1028/env.sh
{
echo "VM 105: dedup-default-blake3, B = 59efd7df84 (debug), M = a128b8a075 with B's tests (debug); kernel evidence and an indicative measurement (y/VM-TESTS-105-ddb3.md)"
for s in record sha-checks trees build step2 step2b step3 load-B facts-B; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
for l in b1 b2 b3 bg; do echo; echo "== $l-notes.txt"; cat $L/$l-notes.txt 2>&1; done
for s in meas-step meas-summary load-M facts-M; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
for l in m1 m2 m3 mg; do echo; echo "== $l-notes.txt"; cat $L/$l-notes.txt 2>&1; done
for s in guards compare end; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
echo
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HOLD [$(cat $L/HOLD 2>/dev/null)]"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt` after its first line, a `==
STEP 0` section (short: anything found, `uname -r`, `nproc`, the CPU model
and flags, `cmdline`, the memory line, the free space, the `zpool.cache`,
`/usr/bin/time`, `xxh128sum`, the test users line, the doc checkout line and
its sha256, the directories of earlier tasks, the stack depot count and the
kernel baseline), and at its end a `== verdict` section (the Expected block
below, then for each of its items whether it matched, and which case of
"What each result means" applies) and a `== deviations` section (every
deviation from these instructions, with its reason; `none` if there was
none). Then print it with `cat /var/log/zts-ddb3-1028/REPORT.txt`.

Then, as the very last command, and only if the unload block printed
`unload M rc=0` with `lsmod: []` and `mounts: []` and no step stopped,
remove this task's directories and the doc checkout (the doc checkout only
if it is still the clean checkout STEP 1 recorded, holding this document,
and nothing else):

```sh
cd /
D=/var/tmp/vm-task-105y
if [ -e /var/log/zts-ddb3-1028/STOP ] || [ -e /var/log/zts-ddb3-1028/HOLD ] || lsmod | grep -qE '^(zfs|spl) '; then
	echo "a step stopped or a module is loaded: nothing removed"
elif grep -qE " /var/tmp/zts-ddb3-1028[ /]| /testpool" /proc/mounts; then
	echo "something is mounted under /var/tmp/zts-ddb3-1028 or /testpool: nothing removed"
else
	if [ -d $D ] && [ ! -L $D ] && [ -d $D/.git ] && [ ! -L $D/.git ] &&
	    [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
	    [ -s /var/log/zts-ddb3-1028/doc-head.txt ] &&
	    [ "$(git -C $D rev-parse HEAD 2>/dev/null)" = "$(cat /var/log/zts-ddb3-1028/doc-head.txt)" ] &&
	    git -C $D ls-files --error-unmatch y/VM-TESTS-105-ddb3.md > /dev/null 2>&1 &&
	    [ "$(sha256sum < $D/y/VM-TESTS-105-ddb3.md | cut -d' ' -f1)" = "$(cat /var/log/zts-ddb3-1028/doc-sha.txt 2>/dev/null)" ] &&
	    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    [ "$(LC_ALL=C ls -A $D | grep -vx .git | xargs)" = "$(git -C $D ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
	    ! grep -q " ${D}[ /]" /proc/mounts; then
		X=$D
	else
		X=""; echo "$D kept: not only the clean doc checkout STEP 1 recorded"
	fi
	rm -rf /var/tmp/zts-ddb3-1028 /var/log/zts-ddb3-1028 $X; echo "rm rc=$?"
fi
ls -d /var/tmp/zts-ddb3-1028 /var/log/zts-ddb3-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, or a step stopped, this block removes nothing:
everything is kept, and the reply says so. If your permission system refuses
this `rm -rf`, leave what is left and say so; do not delete it any other
way. Then reply with all of `REPORT.txt` as printed, and the result of the
last block: nothing runs after the reply.

## Expected, as a block for REPORT.txt

```
STEP 0: free; root; x86_64; at least 12G free on /var/tmp; a test user with passwordless sudo;
  the CPU flags, cmdline, /usr/bin/time and xxh128sum recorded; the doc checkout at
  /var/tmp/vm-task-105y.
STEP 1-3: every check ok, no MISMATCH; B = 59efd7df84, its parent a128b8a075 in upstream master;
  M = a128b8a075 with B's 13 files under tests/ and none of the 10 others; builds B and M rc=0,
  compiler warnings 0, about 83 objtool warning: lines per tree, ZFS_DEBUG lines 3, vermagic =
  uname -r, libzfs reading /var/tmp/zts-ddb3-1028/share/zfs/compatibility.d; srcversions differ,
  M's recorded (8852710DB871A7587B9E722 earlier); seven syntax rc=0.
STEP 4: load B rc=0, zfs_dedup_default_checksum 0; facts B: the chksum_bench rows, the rule's
  choice <c> on their figures, the entry = 14 (blake3) or 8 (sha256) as the rule gives, the
  debug-log line with the same figures and choice, 4 of 4 blocks <c> dedup blocks, DDT-<c>,
  feature@blake3 active for blake3 (enabled for sha256); six ok lines.
b1-b3: PASS 3 each (setup, the test, cleanup); SUCCESS lines save_tunable x1, set_tunable32 x5,
  zpool create x5, dd x14, restore_tunable x1; the note's figures = STEP 4's, its expectation =
  the rule's; end line "dedup=on uses the checksum recorded when the pool was created".
bg: PASS 20.
STEP 6: six runs rc=0; the dedup=on pools write DDT-<c>, the sha256 pools DDT-sha256; every pool
  records <c>; the figures and their medians recorded (indicative only).
STEP 7: load M rc=0, the tunable absent; facts M: no entry, no debug-log line, 4 of 4 blocks
  sha256 dedup blocks, DDT-sha256, feature@blake3 enabled; five ok lines.
m1-m3: FAIL 1, PASS 2 each: dedup_default_checksum FAIL at "ERROR: set_tunable32
  DEDUP_DEFAULT_CHECKSUM 1 exited 1", before any pool (zpool create x0, dd x0).
mg: FAIL 1 (dedup_default_checksum, as in m1), PASS 19.
Every run and step: kernel_findings=0, kernel_warnings=0, every leftover empty, loops and dm
  same, tunables_same=yes; zts_marks = its result count.
STEP 8: unload M rc=0; lsmod []; whole task 0 findings, 0 warnings, 46 ZTS marks; the boot's
  count equal to the STEP 0 baseline; nothing left; rm rc=0.
STOP only on: a failed check or guard, a leftover, a changed tunable, an outer timeout, a
  failed command of facts.sh or meas.sh, or a kernel finding.
```

## What each result means

- **The evidence holds** when every item above matches. Then, in a kernel:
  a pool B creates with the default features records, in its pool
  directory, the faster of BLAKE3 and SHA-256 by the figures this host's
  benchmark printed, and `dedup=on` writes dedup blocks of that checksum; the
  new test passes three times and the dedup group passes whole on B,
  including the nine tests that now name `dedup=sha256`; with the same
  tests, M fails the new test, because master has no
  `zfs_dedup_default_checksum`, and passes the rest of the group, and a pool
  M creates records nothing and writes SHA-256. The measurement says how
  much the choice saves on this host, as an indication.
- **Against the commit**: a `MISMATCH` line of `facts.sh B` (an entry other
  than the rule's choice, a debug-log line with other figures, blocks or a
  DDT of another checksum); `dedup_default_checksum` failing on B (its
  `line:` entries and the `dd` count locate it); another dedup test failing
  on B but not on M; any kernel finding on B.
- **Not against the commit**: a test failing the same way on B and on M
  (the environment; `dedup_legacy_gang` and `dedup_log_zdb_leak` without
  `xxh128sum`, for one); `dedup_default_checksum` failing on M, as
  predicted, or on M at another point (record where: the prediction was
  wrong, not the commit); a note whose figures differ from STEP 4's (record
  it: the benchmark was run again, which the module should not do).
- **The measurement**: `dedup=on` faster than `dedup=sha256` in wall time
  or kernel CPU when the pools record BLAKE3 is the expected direction;
  similar figures when they record SHA-256 (both variants are the same
  checksum). A small or reversed difference with BLAKE3 recorded is
  recorded as it is: the disk, not the checksum, can bound a 2 GiB write
  here. It is indicative only, whatever it shows.
- **Any kernel finding**: the line and the step are the result. Stop.

Estimated duration: about 2 hours (1.5 to 2.5): checks and sources 10
minutes, the two builds about 30, the facts 2, `b1`-`b3` 5 to 10, `bg` 15
to 30, the measurement 5 to 15, the swap and the facts on M 3, `m1`-`m3` 3
to 5, `mg` 15 to 30, the end 5.
