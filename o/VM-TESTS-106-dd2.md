# VM 106 (CHSONVTST03): kernel proof of `dedup-default-blake3` at 59efd7df84

These instructions are for the Claude session on VM 106. They produce the
first kernel evidence for the current head of the fork branch
`dedup-default-blake3` of https://github.com/tigerblue77/zfs:
`59efd7df84a72e7ae25f03f13cbc2d0b6e2c250e` "Use the faster of BLAKE3 and
SHA-256 for dedup=on on new pools", one commit on upstream master
`a128b8a075b5b13c737e56914cd7043bd3e9ebf8` "Linux 7.3 compat: META". The
session builds two fresh debug trees in its own directories, runs everything
below in the kernel, cleans up, and replies with `REPORT.txt`.

**What the commit does.** `dedup=on` and `dedup=verify` resolve to
`spa_dedup_checksum()`, which is SHA-256 on every pool today. With the commit,
a pool created with `feature@blake3` enabled records a choice in its pool
directory in the MOS, `org.openzfs:dedup_checksum` = 14 (BLAKE3) or 8
(SHA-256), and `ddt_load()` applies it at every later import, on any host. The
new module parameter `zfs_dedup_default_checksum` decides what a new pool
records: `1` BLAKE3, `2` SHA-256, and `0` (the default; any other value
behaves as 0) the faster of the two in the checksum benchmark the module
already runs: the highest 256 KiB figure among the `blake3-*` rows against the
highest among the `sha256-*` rows, SHA-256 only if its figure is higher and
BLAKE3's is not 0. The tunable is read only in `spa_create()`, and only when
`feature@blake3` is enabled there; a pool created without the feature, or with
a compatibility set that lacks it, and every existing pool record nothing and
keep SHA-256. The `0` decision is logged in the debug log as
`pool '<p>': dedup checksum <c> (blake3 <n> MiB/s, sha256 <n> MiB/s)`; the
forced values log nothing. The figures are read under a new mutex,
`chksum_stat_lock` in `module/zfs/zfs_chksum.c`, which `chksum_benchmark()`
also holds: at module load, and on every read of
`/proc/spl/kstat/zfs/chksum_bench`.

**Why this run.** The kernel proof of 26 Oct on VM 107 (`f/PART-A-107f.md`,
`f/PART-B-107f.md`) was on the branch's previous head `1db884e834` "Default to
BLAKE3 for dedup=on on new pools", which always recorded BLAKE3. It says
nothing about this head: the tunable, the benchmark reader and the lock are
new, and the test was rewritten (five pools, 14 data writes). Nothing
kernel-side of this head has run anywhere: libzpool never runs the benchmark,
and no kernel has taken the new lock.

| Tree | Path | Code | Tests |
| --- | --- | --- | --- |
| `B` | `/var/tmp/zts-dd2-1026/B` | the head `59efd7df84` | its own |
| `M` | `/var/tmp/zts-dd2-1026/M` | master `a128b8a075`, the head's parent and merge base | every file the head changes under `tests/` (13): `common.run`, `tunables.cfg`, `Makefile.am`, the new test, and the nine dedup tests it moves from `dedup=on` to `dedup=sha256` |

Both trees are built here with `--enable-debug --datadir=/var/tmp/zts-dd2-1026/share`.

What the run establishes:

1. **STEP 4, on B right after its load, before anything reads the
   `chksum_bench` kstat**: with the tunable at 0, 1, 2 and 7 in turn, a new
   pool records 14 for 1, 8 for 2, and for 0 and 7 the choice its debug-log
   line names, which must follow the rule from the two startup figures in that
   line. Then the tunable is set to the value that would record the other
   checksum, the pool is exported and imported, and both the recorded value
   and the checksum of a file written after the import are unchanged.
2. **STEP 5**: `dedup_default_checksum` three times on B (PASS, 14 data writes
   each); then the `0` rule applied by hand to this host's real figures in the
   kstat, against the value a new pool records (`zdb -dddd` of the MOS
   object 1); then the `dedup` group once.
3. **STEP 7**: `dedup_default_checksum` three times on M (FAIL, at the first
   `set_tunable32`, before any pool exists: master has no such tunable), then
   the `dedup` group once, compared with B's result by result.
4. **STEP 6, 8 and 9**: a pool B created (recording BLAKE3) imported by M, a
   pool M created (recording nothing) imported by B, then B's pool again on B
   after M wrote to it: data read back bit-exact, the dedup checksum of a file
   written by the importer, a scrub, and each tree's `zdb -bcc`.
5. **Throughout**: the kernel log of every run and step, which must stay
   clean, and in STEP 10 the module's unload.

## What the new test does, and why M fails

`tests/functional/dedup/dedup_default_checksum.ksh` saves the tunable
(`save_tunable DEDUP_DEFAULT_CHECKSUM`, a file in the `-d` directory), creates
five pools in turn on the three loop devices `zfs-tests.sh` provides, and
restores the tunable in its cleanup. Every check of a dataset writes a new
512 KiB file of random data (`file1` to `file14`, one
`SUCCESS: dd if=/dev/urandom` line each) and requires all four of its level-0
blocks to be dedup blocks of the expected checksum in `zdb -ddddddbbbbbb`;
the recorded value is read with `zdb -dddd testpool 1`.

| Pool | Created with | Files | What it checks |
| --- | --- | --- | --- |
| 1 | tunable `1`, default features, `-O dedup=on` | 1-5 | records 14; `feature@blake3` enabled; root and `dedup=verify` write BLAKE3, `dedup=sha256` SHA-256; the feature active; the tunable set to `2`, export, import; root and verify still BLAKE3 |
| 2 | tunable `2` | 6-8 | records 8; root SHA-256; the feature still enabled; the tunable set to `1`, export, import; still SHA-256 and enabled; a `dedup=blake3` dataset writes BLAKE3; active |
| 3 | tunable `1`, `-o feature@blake3=disabled` | 9-12 | records nothing; SHA-256; `zpool upgrade` (enabled); SHA-256; export, import; nothing recorded, SHA-256, enabled; `dedup=blake3` writes BLAKE3; active |
| 4 | tunable `1`, `-o compatibility=openzfs-2.1-linux` | 13 | the feature disabled; nothing recorded; SHA-256 |
| 5 | tunable `0`, after reading the `chksum_bench` kstat | 14 | records, and writes, what the rule gives on the kstat's 256k column; logs `NOTE: chksum_bench at 256k: blake3 <n> MiB/s, sha256 <n> MiB/s, expecting <c>` |

A failed check ends the test with one of `<ds>: <n> of 4 blocks are <c> dedup
blocks`, `testpool records dedup checksum '<got>', expected '<want>'`,
`feature@blake3 is <state> on testpool, expected <state>`, or, for a failed
command, `ERROR: <command> exited <n>`. The count `k` of `SUCCESS: dd` lines
says where: a block check that fails has written its own file, so it is check
`k`; a recorded-value or feature check that fails comes after `k` writes.
Pool 4 needs the compatibility file `openzfs-2.1-linux`; an uninstalled tree
has none unless it looks elsewhere, which is why STEP 1 and STEP 2 configure
both trees with `--datadir=$W/share` and copy the trees' own
`cmd/zpool/compatibility.d` there (the commit does not change it).

**On M**, `tunables.cfg` (copied from the head) names
`zfs_dedup_default_checksum`, but master's module has no such parameter.
`save_tunable` finds none and saves nothing (it returns 0), so
`SUCCESS: save_tunable DEDUP_DEFAULT_CHECKSUM` is printed; then
`log_must set_tunable32 DEDUP_DEFAULT_CHECKSUM 1` runs
`echo 1 > /sys/module/zfs/parameters/zfs_dedup_default_checksum`, which sysfs
refuses, since a parameter file cannot be created (ksh93 should report it as
`cannot create [Permission denied]`), and the test ends with `ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM
1 exited 1` beside the shell's message, before any pool exists: 0
`SUCCESS: dd` lines, 0 `SUCCESS: zpool create` lines. Its cleanup's
`restore_tunable` finds no parameter and does nothing. So M fails for the
missing tunable, not for the checksum it writes: that is the honest
failing-first on master for this head, since the test cannot run its cases
without the tunable. What master writes on a new pool is shown separately in
STEP 8 (`write M xm`: SHA-256 dedup blocks on a pool with default features,
where pool 1 of the test expects BLAKE3). On M the kernel creates no pool in
these runs.

The nine tests the head moves from `dedup=on` to `dedup=sha256` inspect the
SHA-256 DDT. On master `dedup=on` is SHA-256, so they should pass on M as they
do with `dedup=on`; on B they keep the table they inspect. The group's other
`dedup=on` tests (`dedup_prune`, `dedup_prune_leak`, `dedup_prune_percentage`,
`dedup_quota`, `dedup_log_zdb_leak`) do not name a checksum: on B they write
whatever the tunable's default records on this host.

## The default on this host

The module measures every implementation of every checksum once at load (the
"startup" pass: 256 KiB only), and `set_fastest()` picks each algorithm's
fastest implementation from that figure. The **first read** of
`/proc/spl/kstat/zfs/chksum_bench` runs the benchmark again in full (eight
sizes, 256 KiB measured again, about 2.5 s), and from then on the figures never
change for the life of the module. So:

- **STEP 4 must be the first thing done after B's load.** Nothing may read
  `/proc/spl/kstat/zfs/chksum_bench` before it, not even a `cat` to look. Its
  pools for 0 and 7 decide on the startup figures, which their debug-log lines
  show; `cv.sh auto` in STEP 5 checks afterwards that the kstat's figures
  differ from those (re-measured by the full pass), which shows that nothing
  had read the kstat before STEP 4.
- **From b1 on, the figures are frozen**: the test's pool 5 reads the kstat
  first. The notes of b1, b2, b3 and bg, the debug-log lines of the test's
  pool 5, and the line of the pool `cv.sh auto` creates must all show the same
  two figures, which are the kstat's highest 256k values for `blake3-*` and
  `sha256-*`.
- **Which wins depends on the CPU**, and STEP 0 prints the three flags that
  decide it: `sha_ni`, `avx2`, `avx512f`. With `avx2` or `avx512f`, BLAKE3 is
  expected well ahead with or without `sha_ni` (the branch's figures on a
  SHA-NI and AVX-512 VM, at 64k-256k: blake3-avx2 2357-3546 and blake3-avx512
  4767-6410 MiB/s against sha256-shani 1282-1552); with `sha_ni` and neither
  of the other two, the two are within about 1.4x and either can win. No step
  assumes the answer: every check applies the rule to what this host measured.
- The startup choice (STEP 4) and the choice on the full figures (STEP 5) can
  differ only where the two figures are within the noise of one ~1 ms
  measurement. That is recorded, not a defect: each pool keeps what it
  recorded.

## The cross-version pools

Two pools on one 256 MiB file vdev each, under `$W/cv`, made with
`-o cachefile=none` (so `/etc/zfs/zpool.cache` is never written for them),
`-O dedup=on -O compression=off`, mounted under `$W/cvmnt-<pool>`. Each gets
an 8 MiB random file `a`, a copy `a2` written with `dd` (so it is
deduplicated, not cloned), and a 512 KiB file; every importer reads all files
back against their sha256, writes a 512 KiB file of its own, checks that
file's dedup checksum, scrubs, and exports.

| Pool | Created by | Recorded | Expected |
| --- | --- | --- | --- |
| `xb` | B, with the tunable set to 1 for the creation (BLAKE3 whatever this host's figures) | 14 | B writes BLAKE3 dedup blocks; **M** imports it, reads it, and writes SHA-256 (master ignores the entry: a second DDT, nothing wrong with the data); B imports it again and still writes BLAKE3 |
| `xm` | M | nothing | M writes SHA-256; **B** imports it with the tunable at 1 and still writes SHA-256: an existing pool never follows the tunable |

## Rules for the whole run

- Run everything as root, in bash, in this order. The ZTS runs go through
  `runuser -u zts` inside `run.sh`; nothing else runs as `zts`. Shell variables
  may not survive between your commands, so every block below starts by
  sourcing `/var/log/zts-dd2-1026/env.sh` or by setting what it needs.
- A production hypervisor sits underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads and every ZTS run and pool step go through `nice -n 10 ionice
  -c3` too (the blocks and the scripts do it), one at a time, with nothing
  else running beside them.
- Post nothing, push nothing, install nothing (no package, no
  `make install`), never reboot, and never run `scripts/zfs-helpers.sh` or
  `zfs-tests.sh -x`. Rebuild nothing once STEP 2 has built the trees.
- Work only under `/var/tmp/zts-dd2-1026` (W) and `/var/log/zts-dd2-1026`
  (L). Never read from, write to or delete `/var/tmp/test_results`,
  `/var/tmp/zts-19314`, `/var/tmp/zts-lz4-1026`, `/var/log/zts-lz4-1026`,
  another `/var/tmp/zts-*` or `/var/log/zts-*` directory, or anything else
  this task did not create. ZTS pools mount under `/testpool*` while they
  exist; the tests' cleanup removes those directories, and `run.sh` checks
  that it did.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the ZTS pools in it while they exist and truncates it to 0 bytes
  when they are gone; the pools of `cv.sh` never touch it. STEP 0 and STEP 10
  record it.
- Never run a `zpool`, `zfs` or `zdb` command while none of this task's
  modules is loaded: libzfs would `modprobe` whatever zfs module the system
  has. Never print the environment (`env`, `printenv`, `set`,
  `/proc/*/environ`).
- Between B's first load in STEP 4 and `cv.sh early`, run nothing that reads
  `/proc/spl/kstat/zfs/chksum_bench` (see "The default on this host").
- The only `rm -rf` allowed is the one at the end of STEP 10. The only other
  removals are `cv.sh`'s own: its pool directories under `$W/cv` and the empty
  mount points `$W/cvmnt-*`.
- If a check does not print what its comment or the Expected lines say, stop
  at once and report. Do not rebuild, repair or retry. If your permission
  system refuses a command, do not work around it: report it and stop there.
  A kernel panic, oops, `VERIFY`, `ASSERT`, `BUG:` or hung-task message means
  stop and report, in whatever state it happened; never reboot. A stop in
  STEP 0 changes nothing: report and reply. After any later stop, go straight
  to STEP 10 with what you have, and skip its unload if a pool, a device, a
  mount or a process is left, or the kernel logged a fault: a busy or wedged
  module must not be forced out.
- The two builds take about 30 minutes together, a ZTS group run up to about
  25. If your shell tool cannot wait that long for a command in the
  foreground, start that one command with the tool's own background mode and
  wait until it exits; do not use `&` or `nohup`, and run nothing else
  meanwhile.
- The session cannot run anything after its final reply: the cleanup at the
  end of STEP 10 is the last command, and the reply comes after it. Leave no
  background command, loop or watcher behind.

## What earlier runs on this VM taught, and these steps already handle

- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines in
  `zfs_config.h` (one unconditional, two under `#ifdef`): 3 is a debug build.
- objtool prints about 83 `warning:` lines per tree (`spl_panic()` and
  `luaD_throw()` lacking `__noreturn`), harmless. Compiler warnings are
  counted only in the `file:line:col: warning:` form,
  `:[0-9]+:[0-9]+: warning:`.
- A `-t` run names its test with `.ksh` in its `Test:` line, a group run
  without it, and a test of a `linux.run` section is printed `Test (Linux):`
  (here the dedup group's `setup`, `dedup_legacy_gang` and `cleanup`): every
  pattern below allows all three. A literal parenthesis in a regex is written
  `[(]`, never `\(`.
- The kernel-log scan drops the ZTS `-K` markers (`ZTS run <test>`) and the
  `spa_import_progress_set_notes_impl` notes. ZTS clears dmesg after a failed
  test, so kernel messages are read per run from `journalctl -k`.
- This VM has no `60-zvol.rules`, so `/dev/zvol/...` links never appear and
  zvol tests fail on every tree. No test of the dedup group uses a zvol, so it
  does not apply here; STEP 0 only records it.
- The trees are not installed, so an uninstalled tree finds no
  `compatibility.d` (which is why `zpool_upgrade_features_001_pos` failed on
  every tree of earlier tasks). This task gives both trees a data directory
  with the compatibility files, for pool 4 of the new test;
  `zpool_upgrade_features_001_pos` is not run.
- Master's known test leaks (`tmp.*` files of `vdevs_in_pool()`'s callers,
  `removal_with_zdb`'s `tmp.*` file, `removal_condense_export`'s `testdir1`)
  are expected on M, and here on B too, whose `libtest.shlib` is master's. No
  test run here calls that code; if those shapes appear in `$W/files`
  anyway, `run.sh` records them as `known=[...]` and stops only on anything
  else left there.
- A test that writes nothing on stderr has no `merged` file: its `stdout` is
  read instead. Each run waits 2 s after it ends: two runs started in the same
  second would share a results directory.
- git refuses to work in a tree owned by another user: all git work happens
  in STEP 1 and STEP 2, before the `chown` to `zts`. `zfs-tests.sh -d` must
  point at a mode 1777 directory.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-dd2-1026 L=/var/log/zts-dd2-1026
date -u '+%F %T'
id -un                        # root
uname -r                      # 6.12.113+deb13-cloud-amd64 (the kernel of the earlier runs on this VM)
nproc                         # 16
umask                         # 0022
grep -m1 '^model name' /proc/cpuinfo
for f in sha_ni avx2 avx512f; do
    grep -qw "$f" /proc/cpuinfo && printf '%s=yes ' $f || printf '%s=no ' $f
done; echo                    # the CPU flags line: it decides the default
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zdb zed  # nothing: no system ZFS
pgrep -x zed                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|autogen|configure|make|gcc|cc1|raidz_test'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -d $W $L 2>&1              # No such file or directory, twice
ls -A /var/tmp; ls -d /var/log/zts-* 2>&1
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|zts-dd2' /proc/mounts   # nothing
ls -d /testpool* 2>&1         # No such file or directory
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
ls -ld /usr/local/etc/zfs/compatibility.d /usr/local/share/zfs/compatibility.d 2>&1   # No such file or directory, twice
ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules 2>&1; ls -d /dev/zvol 2>&1
df -h /var/tmp                # need 15G free
free -h
id zts; sudo -n -l -U zts | grep -i nopasswd
command -v git ksh python3 modinfo sha256sum truncate runuser timeout   # eight paths
journalctl -k -n 1 --no-pager -q   # one kernel line
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
```

Keep this output, which goes into `REPORT.txt`. The last number is the
kernel-message baseline. `/var/tmp/zts-lz4-1026`, `/var/log/zts-lz4-1026`
(the LZ4 task, which removes them when it ends cleanly), `/var/tmp/zts-19314`
(to be deleted beforehand) and `/var/tmp/test_results` are recorded only:
their presence is not a stop, and this task never touches them. The
`60-zvol.rules`, `/dev/zvol` and `compatibility.d` lines are recorded too (an
existing `/usr/local/etc/zfs/compatibility.d` is not a stop, but say so: its
files take precedence over this task's).

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, a system `zpool`, `zfs`,
  `zdb` or `zed` is installed, or `zed` runs;
- a test or build process is running (the LZ4 task, or anything else). The
  `pgrep -af` also matches the shell that runs this block, because that
  shell's command line contains the pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` exists: this task would then have started once already;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load);
- less than 15 GB is free on `/var/tmp` (two built trees, the tests' three
  sparse 4 GiB file vdevs and the dedup group's own files);
- user `zts` is missing or has no passwordless sudo;
- one of the eight commands is missing, or `journalctl -k` prints no kernel
  line (the runs read their kernel messages from the journal).

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself:
STEP 2 checks that the modules were built for the running one.

## STEP 1: directories, sources, the head's tests in M (all git work, part one)

The environment file first, then the clone (read-only towards GitHub) and the
fetch of upstream master, to check the merge base:

```sh
export PATH=/usr/sbin:/sbin:$PATH
mkdir /var/tmp/zts-dd2-1026 /var/log/zts-dd2-1026; echo "mkdir rc=$?"   # 0
cat > /var/log/zts-dd2-1026/env.sh <<'EOF'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-dd2-1026
L=/var/log/zts-dd2-1026
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# The branch's head (one commit) and its parent, upstream master and the
# merge base.
HB=59efd7df84a72e7ae25f03f13cbc2d0b6e2c250e
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
# The new test as a file of the tree (F); the nine dedup tests the head
# moves from dedup=on to dedup=sha256 (D9); every file the head changes
# under tests/ (TB) and outside it (CODE), in git's order.
D=tests/zfs-tests/tests/functional/dedup
F=$D/dedup_default_checksum.ksh
D9=$(for t in bclone fdt_create fdt_import fdt_pacing legacy_create \
    legacy_fdt_upgrade legacy_gang legacy_import legacy_repair; do
	echo $D/dedup_$t.ksh; done | xargs)
TB=$(echo tests/runfiles/common.run tests/zfs-tests/include/tunables.cfg \
    tests/zfs-tests/tests/Makefile.am $F $D9 | xargs -n1 | LC_ALL=C sort | xargs)
CODE="include/sys/ddt.h include/sys/dmu.h include/sys/zfs_chksum.h include/sys/zio.h man/man4/zfs.4 man/man7/zfsprops.7 man/man7/zpool-features.7 module/zfs/ddt.c module/zfs/spa.c module/zfs/zfs_chksum.c"
# The data directory both trees are configured with: their libzfs reads
# the pool compatibility files from $CD (the test's pool 4 needs one).
SHARE=$W/share
CD=$SHARE/zfs/compatibility.d
# The new tunable (B only), the benchmark kstat, the debug log.
P=/sys/module/zfs/parameters/zfs_dedup_default_checksum
KB=/proc/spl/kstat/zfs/chksum_bench
DBG=/proc/spl/kstat/zfs/dbgmsg
# Kernel findings: KPAT stops the task (BUG: covers "sleeping function
# called from invalid context"); KWARN lines are quoted, not a stop by
# themselves (a kernel WARN() also prints a call trace, which KPAT has).
# KIGN drops the ZTS -K markers and the import progress notes.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking'
KWARN='WARNING:'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The tunables the dedup tests change and must restore.
TUN='zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct zfs_bclone_enabled zfs_dedup_default_checksum'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: $2, expected $3"
	fi
}

tunables() {
	for p in $TUN; do
		if [ -e /sys/module/zfs/parameters/$p ]; then
			printf '%s=%s ' "$p" "$(cat /sys/module/zfs/parameters/$p)"
		else
			printf '%s=absent ' "$p"
		fi
	done
}

# What a run may leave in $W/files without stopping the task: the shapes
# of master's known test leaks (tmp.* files of vdevs_in_pool's callers and
# removal_with_zdb, removal_condense_export's testdir1). No test run here
# calls that code, so they are recorded if they appear; anything else in
# $W/files is a leftover.
files_known() {
	for f in "$W"/files/tmp.?????????? "$W"/files/testdir1; do
		[ -e "$f" ] && echo "${f##*/}"
	done | xargs
}
files_left() {
	ls -A "$W/files" 2>&1 | grep -vxE 'tmp\.[A-Za-z0-9]{10}|testdir1' | xargs
}

# Build <tree> in place, niced: autogen, configure (debug, data directory
# $SHARE), make -j8. Stops on a failed build.
build_tree() {   # build_tree <B|M>
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug --datadir=$SHARE && make -j8") \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/build.txt
		echo "build $1 failed: stop" | tee -a $L/build.txt
		return 1
	fi
}
EOF
. /var/log/zts-dd2-1026/env.sh
date +%s > $L/start.txt
git clone --no-checkout $FORK $W/repo > $L/clone.log 2>&1; echo "clone rc=$?"   # 0
git -C $W/repo fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"   # 0
```

Then the head, its parent, the files it changes, and one worktree per tree:

```sh
. /var/log/zts-dd2-1026/env.sh
{
same "dedup-default-blake3" "$(git -C $W/repo rev-parse origin/dedup-default-blake3)" $HB
same "its parent" "$(git -C $W/repo rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git -C $W/repo merge-base FETCH_HEAD $HB)" $HM
same "files under tests/" "$(git -C $W/repo diff --name-only $HM $HB -- tests | xargs)" "$TB"
same "code files" "$(git -C $W/repo diff --name-only $HM $HB -- . ':!tests' | xargs)" "$CODE"
same "diff stat" "$(git -C $W/repo diff --stat $HM $HB | tail -1)" " 23 files changed, 461 insertions(+), 19 deletions(-)"
same "compatibility files changed" "$(git -C $W/repo diff --name-only $HM $HB -- cmd/zpool/compatibility.d | wc -l)" 0
} | tee $L/sha-checks.txt
for t in M:$HM B:$HB; do
	git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktrees.log 2>&1
	echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD)"
done | tee $L/worktrees.txt
git -C $W/repo log -1 --abbrev=10 --format='%h %s' $HB | tee -a $L/worktrees.txt
git -C $W/repo log -1 --abbrev=10 --format='parent %h %s' $HM | tee -a $L/worktrees.txt
```

Expected, in order:

```
ok: dedup-default-blake3
ok: its parent
ok: merge base with upstream master
ok: files under tests/
ok: code files
ok: diff stat
ok: compatibility files changed
worktree M rc=0 at a128b8a075
worktree B rc=0 at 59efd7df84
59efd7df84 Use the faster of BLAKE3 and SHA-256 for dedup=on on new pools
parent a128b8a075 Linux 7.3 compat: META
```

Any `MISMATCH` (the branch was force-pushed or rebased since 26 Oct) means
stop.

Copy the head's files under `tests/` into M (`git checkout` stages them, so
that `git diff HEAD` shows the new file too), and fill `$CD` with the trees'
compatibility files:

```sh
. /var/log/zts-dd2-1026/env.sh
{
git -C $W/M checkout $HB -- $TB && echo "M: tests copied"
git -C $W/M diff HEAD --stat | tail -1
same "M: changes outside tests/" "$(git -C $W/M diff HEAD --name-only -- . ':!tests' | wc -l)" 0
same "M: tests/ as in the head" "$(git -C $W/M diff $HB -- tests | wc -l)" 0
same "M: differs from the head in" "$(git -C $W/M diff --name-only $HB | xargs)" "$CODE"
same "B: tracked changes" "$(git -C $W/B status --short --untracked-files=no | wc -l)" 0
mkdir -p $SHARE/zfs && cp -R $W/B/cmd/zpool/compatibility.d $SHARE/zfs/ &&
    echo "$CD: $(ls $CD | wc -l) files, openzfs-2.1-linux lists blake3 x$(grep -cx blake3 $CD/openzfs-2.1-linux)"
same "$CD as M's" "$(diff -r $CD $W/M/cmd/zpool/compatibility.d | wc -l)" 0
same "$CD as B's" "$(diff -r $CD $W/B/cmd/zpool/compatibility.d | wc -l)" 0
for t in M B; do
	cd $W/$t
	echo "$t: test $(ls -l $F | cut -c1-10)," \
	    "registered x$(grep -c "'dedup_default_checksum'" tests/runfiles/common.run)" \
	    "x$(grep -c 'functional/dedup/dedup_default_checksum\.ksh' tests/zfs-tests/tests/Makefile.am)," \
	    "tunables.cfg x$(grep -c '^DEDUP_DEFAULT_CHECKSUM[[:space:]]' tests/zfs-tests/include/tunables.cfg);" \
	    "the nine: dedup=sha256 x$(cat $D9 | grep -c 'dedup=sha256') dedup=on x$(cat $D9 | grep -c 'dedup=on');" \
	    "code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x$(grep -c DMU_POOL_DEDUP_CHECKSUM module/zfs/ddt.c)," \
	    "ddt_create_dedup_checksum in spa.c x$(grep -c 'ddt_create_dedup_checksum(spa, tx)' module/zfs/spa.c)," \
	    "chksum_stat_lock in zfs_chksum.c x$(grep -c chksum_stat_lock module/zfs/zfs_chksum.c)," \
	    "the tunable in ddt.c x$(grep -c 'default_checksum, UINT' module/zfs/ddt.c)"
done
} | tee $L/copy.txt
```

Expected:

```
M: tests copied
 13 files changed, 218 insertions(+), 12 deletions(-)
ok: M: changes outside tests/
ok: M: tests/ as in the head
ok: M: differs from the head in
ok: B: tracked changes
/var/tmp/zts-dd2-1026/share/zfs/compatibility.d: 25 files, openzfs-2.1-linux lists blake3 x0
ok: /var/tmp/zts-dd2-1026/share/zfs/compatibility.d as M's
ok: /var/tmp/zts-dd2-1026/share/zfs/compatibility.d as B's
M: test -rwxr-xr-x, registered x1 x1, tunables.cfg x1; the nine: dedup=sha256 x10 dedup=on x0; code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x0, ddt_create_dedup_checksum in spa.c x0, chksum_stat_lock in zfs_chksum.c x0, the tunable in ddt.c x0
B: test -rwxr-xr-x, registered x1 x1, tunables.cfg x1; the nine: dedup=sha256 x10 dedup=on x0; code: DMU_POOL_DEDUP_CHECKSUM in ddt.c x2, ddt_create_dedup_checksum in spa.c x1, chksum_stat_lock in zfs_chksum.c x8, the tunable in ddt.c x1
```

(M carries the head's tests byte for byte and none of its code; on master the
nine tests have `dedup=on` ten times and `dedup=sha256` never.
`openzfs-2.1-linux` lacking blake3 is what the test's pool 4 relies on.)
Anything else means stop.

## STEP 2: builds, checks, and the hand-over to `zts` (all git work, part two)

Build B, then M (about 30 minutes together; see the rule about long
commands):

```sh
. /var/log/zts-dd2-1026/env.sh
build_tree B && build_tree M
```

Expected: `build B rc=0 wall=<s>s`, then `build M rc=0 wall=<s>s`. A failed
build prints its `error:` lines and `build <tree> failed: stop`: stop.

Then check what was built:

```sh
. /var/log/zts-dd2-1026/env.sh
{
for t in B M; do
	cd $W/$t
	same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
	same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h)" 3
	same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)" "$(uname -r)"
	same "$t libzfs reads $CD" "$(grep -aqF "$CD" .libs/libzfs.so && echo yes || echo no)" yes
	same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb \
	    scripts/zfs.sh scripts/zfs-tests.sh tests/test-runner/bin/test-runner.py \
	    $F $D/setup.ksh $D/cleanup.ksh; do [ -e $f ] || echo $f; done | xargs)" ""
	modinfo -F srcversion module/zfs.ko > $L/srcv-$t
	echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
	    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:')" \
	    "(objtool's, about 83 expected); srcversion $(cat $L/srcv-$t)"
	grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
	echo "$t last build line: $(tail -n 1 $L/build-$t.log)"
done
same "B and M srcversions differ" "$(sort -u $L/srcv-B $L/srcv-M | wc -l)" 2
echo "M srcversion $(cat $L/srcv-M); every earlier build of master's module code on this VM: 8852710DB871A7587B9E722"
} 2>&1 | tee $L/step2.txt
```

Expected: 11 `ok:` lines (5 per tree, then the srcversions), for each tree
`compiler warnings: 0` and about 83 other `warning:` lines, a last build line
of a finished build (typically `make[1]: Leaving directory
'/var/tmp/zts-dd2-1026/<B or M>'`), and an M srcversion of
`8852710DB871A7587B9E722`. Any `MISMATCH` means stop. A compiler warning
count other than 0 is not a stop: its first lines are printed, quote them in
the report. Another M srcversion is not a stop either, provided every check
here and in STEP 1 passed (STEP 1 showed that M's module code is master's):
report it.

Then point both trees' results at `$L/results`, record the trees, store the
two srcversions for the guards, and hand the work directory to `zts`. This is
the last git command of the task:

```sh
. /var/log/zts-dd2-1026/env.sh
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
same "M's test files are the head's" "$(for f in $TB; do cmp -s $W/M/$f $W/B/$f || echo $f; done | xargs)" ""
same "$CD is both trees' compatibility.d" \
    "$( (diff -r $CD $W/M/cmd/zpool/compatibility.d; diff -r $CD $W/B/cmd/zpool/compatibility.d) | wc -l)" 0
} | tee $L/step2b.txt
if ! grep -q '^MISMATCH' $L/sha-checks.txt $L/copy.txt $L/step2.txt $L/step2b.txt &&
    [ -s $L/srcv-B ] && [ -s $L/srcv-M ] && ! cmp -s $L/srcv-B $L/srcv-M; then
	printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh &&
	    echo "srcversions recorded"
else
	echo "CHECKS FAILED: stop"
fi
tail -n 2 $L/env.sh
mkdir $W/files $W/cv $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
chown -R zts $W $L/results; echo "chown rc=$?"                               # 0
stat -c '%U %a %n' $W/files $W/cv $L/results
```

Expected: 8 `ok:` lines (the `test-runner.py.in` change is the results
directory, and nothing else may differ), `srcversions recorded`, the two lines
`SV_B=<B's>` and `SV_M=<M's>`, `mkdir rc=0`, `chown rc=0`, then
`zts 1777 /var/tmp/zts-dd2-1026/files`, `zts 755 /var/tmp/zts-dd2-1026/cv`
and `zts 755 /var/log/zts-dd2-1026/results`. On `CHECKS FAILED`, stop. The
ZTS `-d` directory must be mode 1777: the tests write there both as root and
as `zts`. `env.sh` and the scripts stay root's, in `$L`.

## STEP 3: the two scripts

`run.sh` runs one ZTS run on the loaded tree, as `zts`, behind the srcversion
guard, then writes its digest, reads its kernel messages from the journal and
checks its leftovers:

```sh
cat > /var/log/zts-dd2-1026/run.sh <<'EOS'
#!/bin/sh
# usage:
#   run.sh <B|M> <label> test <test name>   : one dedup test (-t)
#   run.sh <B|M> <label> group              : the dedup group (-T dedup)
# One zfs-tests.sh run on the loaded tree, then its digest, its kernel
# messages and its leftovers.
. /var/log/zts-dd2-1026/env.sh
t=$1; lbl=$2; mode=$3; what=$4
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> test <name> | group"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
case $mode in
group)
	sel="-T dedup"; outer=3600; what="" ;;
test)
	case $what in
	''|*[!A-Za-z0-9_]*) echo "bad test name: '$what'"; exit 2 ;;
	esac
	if [ ! -x "$W/$t/$D/$what.ksh" ]; then
		echo "no such test in $t: $D/$what.ksh"; exit 2
	fi
	sel="-t tests/functional/dedup/$what.ksh"; outer=1800 ;;
*) echo "mode is 'test' or 'group'"; exit 2 ;;
esac
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi
G=$L/guards.txt

# The result file of test <name>: its "merged" file, else its "stdout"
# (a test that writes nothing on stderr has no "merged" file).
outfile() {   # outfile <results dir> <name>
	for f in merged stdout; do
		o=$(find "$1" -type f -name "$f" 2>/dev/null |
		    grep -E "/dedup(:Linux)?/$2([.]ksh)?/$f\$" | head -n 1)
		if [ -n "$o" ]; then
			echo "$o"; return 0
		fi
	done
	return 1
}

# How far dedup_default_checksum got, from its result file: the counts of
# its SUCCESS lines, its benchmark note checked against the rule, the lines
# of a failed check or command (before the failure callbacks, which dump
# the debug log), and the line before its cleanup.
ddc_digest() {   # ddc_digest <file>
	echo "  dedup_default_checksum: SUCCESS lines:" \
	    "dd x$(grep -c 'SUCCESS: dd if=/dev/urandom' "$1")," \
	    "zpool create x$(grep -c 'SUCCESS: zpool create' "$1")," \
	    "set_tunable32 x$(grep -c 'SUCCESS: set_tunable32 DEDUP_DEFAULT_CHECKSUM' "$1")," \
	    "save_tunable x$(grep -c 'SUCCESS: save_tunable DEDUP_DEFAULT_CHECKSUM' "$1")," \
	    "restore_tunable x$(grep -c 'SUCCESS: restore_tunable DEDUP_DEFAULT_CHECKSUM' "$1")"
	n=$(grep -m 1 'NOTE: chksum_bench at 256k:' "$1" | sed 's/^.*NOTE: //')
	echo "  note: ${n:-none}"
	# shellcheck disable=SC2046
	set -- "$1" $(echo "$n" | sed -n 's/^chksum_bench at 256k: blake3 \([0-9]*\) MiB\/s, sha256 \([0-9]*\) MiB\/s, expecting \([a-z0-9]*\) *$/\1 \2 \3/p')
	if [ $# -eq 4 ]; then
		r=blake3
		if [ "$2" -gt 0 ] && [ "$3" -gt "$2" ]; then
			r=sha256
		fi
		if [ "$r" = "$4" ]; then
			echo "  the rule on the note's figures gives $r: agrees"
		else
			echo "  the rule on the note's figures gives $r: DISAGREES"
		fi
		echo "$2 $3 $4" > "$L/$lbl-note.txt"
	fi
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { exit } { print }' "$1" |
	    grep -E 'of 4 blocks are|records dedup checksum|feature@blake3 is|ERROR: |cannot |zfs_dedup_default_checksum' |
	    head -n 12 | sed 's/^/  line: /'
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
		print "  end line: " prev; exit }
	    NF { prev = $0 }' "$1"
}

# Guards before the run. The loaded module comes first, so that no zpool
# command runs without this tree's module; then the tunables as found
# after the load, no pool, nothing left in the work directory, no loop
# device, no test mount and no test mount point.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion=$cur want=$sv:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(files_left)
loops=$(losetup -a 2>&1)
mnts=$(grep -E ' zfs |testpool|cvmnt' /proc/mounts)
roots=$(ls -d /testpool /testpool1 /testpool2 /testpool3 2>/dev/null)
echo "$(date -u +%T) before $lbl ($t $mode $what): srcversion=$cur" \
    "pools=[$pools] files=[$files] known=[$(files_known)] loops=[$loops]" \
    "mounts=[$mnts] rootdirs=[$roots]" | tee -a "$G"
if [ "$tun" != "$(cat "$L/tun-$t.txt" 2>&1)" ] ||
    [ -n "$pools$files$loops$mnts$roots" ]; then
	echo "GUARD FAILED before $lbl: tunables=[$tun]: nothing run" | tee -a "$G"
	exit 1
fi

start=$(date +%s)
cd "$W/$t" || exit 1
# $sel is two words on purpose.
# shellcheck disable=SC2086
timeout -k 60 "$outer" nice -n 10 ionice -c3 \
    runuser -u zts -- ./scripts/zfs-tests.sh -vKO -d "$W/files" \
    $sel > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
# Two runs started in the same second would share a results directory.
sleep 2

# Results. Each "Test:" / "Test (Linux):" line starts with a timestamp,
# which is dropped. A -t run names the test with ".ksh", a -T run without.
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
grep -vE '\[PASS\]$' "$L/$lbl-tests.txt" > "$L/$lbl-notpassed.txt"
grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-notpassed.txt" |
    sed -E 's|^.*/functional/dedup/([^ ]+) [(]run as .*$|\1|; s|[.]ksh$||' \
    > "$L/$lbl-failed.txt"
{
echo "$lbl: tree $t, $sel, $(wc -l < "$L/$lbl-tests.txt") results:" \
    "${cnt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
echo "${miss:-no Missing util(s) line}"
if [ "$mode" = test ]; then
	cat "$L/$lbl-tests.txt"
else
	sed -n '/^Results Summary/,/^Percent passed/p' "$L/$lbl.log"
	echo "not passed ($(wc -l < "$L/$lbl-notpassed.txt")):"
	sed 's/^/  /' "$L/$lbl-notpassed.txt"
	sed -n '/^Tests with /,$p' "$L/$lbl.log"
	c9=$(for f in $D9; do f=${f##*/}; f=${f%.ksh}
		grep -E "/$f [(]run as " "$L/$lbl-tests.txt"; done |
	    grep -oE '\[[A-Z]+\]$' | tr -d '[]' | sort | uniq -c |
	    awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
	echo "the nine tests moved to dedup=sha256: ${c9:-none}"
	for n in $(cat "$L/$lbl-failed.txt"); do
		o=""
		[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" "$n")
		if [ -n "$o" ]; then
			cp "$o" "$L/$lbl-$n-out.txt"
			echo "$n:"
			grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
			awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
				print "  end line: " prev; exit }
			    NF { prev = $0 }' "$o"
		else
			echo "$n: no stdout or merged file found"
		fi
	done
fi
o=""
[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" dedup_default_checksum)
if [ -n "$o" ]; then
	cp "$o" "$L/$lbl-ddc-out.txt"
	ddc_digest "$o"
elif [ "$what" = dedup_default_checksum ] || [ "$mode" = group ]; then
	echo "  dedup_default_checksum: no stdout or merged file found"
fi
} > "$L/$lbl-notes.txt"

# Kernel messages of this run. "ZTS run" is the -K marker: it proves the
# journal saw the run.
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
kn=$(grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
kw=$(grep -E "$KWARN" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")

# Leftovers: all must be empty, and the tunables as after the load.
lp=$("$W/$t/zpool" list -H -o name 2>&1)
ll=$(losetup -a 2>&1)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(files_left)
lk=$(files_known)
lm=$(grep -E ' zfs |testpool|cvmnt' /proc/mounts)
lr=$(ls -d /testpool /testpool1 /testpool2 /testpool3 2>/dev/null)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
# Leftover processes, leaving out this script and the shells that started
# it: their command lines can contain the same words.
anc=" $$ "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ]; do
	p=$(awk '/^PPid:/ { print $2 }' "/proc/$p/status" 2>/dev/null)
	anc="$anc$p "
done
lq=$(pgrep -af 'zfs-tests[.]sh|test-runner[.]py|functional/' |
    while read -r pid rest; do
	case "$anc" in
	*" $pid "*) ;;
	*) echo "$pid $rest" ;;
	esac
    done)

echo "$(date -u +%T) after $lbl: results=${cnt:-none} rc=$rc wall=${wall}s" \
    "zts_marks=$km kernel_matches=$kn kernel_warnings=$kw" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] loops=[$ll] dm=[$ld]" \
    "files=[$lf] known=[$lk] mounts=[$lm] rootdirs=[$lr] procs=[$lq]" \
    "tunables_same=$ts" | tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ "$kn" -gt 0 ] || [ "$kw" -gt 0 ]; then
	grep -iE "$KPAT|$KWARN" "$L/$lbl-kernel.txt" | grep -vE "$KIGN" | head -n 20
fi
if [ -n "$lp$ll$ld$lf$lm$lr$lq" ] || [ "$ts" != yes ] ||
    [ "$kn" -gt 0 ] || [ "$km" -lt 1 ] || [ "$rc" -eq 124 ] ||
    [ "$rc" -eq 137 ]; then
	echo "STOP after $lbl: leftover, tunable changed, kernel message," \
	    "no ZTS mark in the journal, or outer timeout" | tee -a "$G"
	exit 3
fi
exit 0
EOS
```

`run.sh` exits 0 when the run is clean, whatever the test results. It exits 1
when a guard before the run failed (nothing ran), 2 on a bad argument, and 3
when something is wrong after the run (a leftover, a changed tunable, a kernel
fault, no ZTS mark in the journal, an outer timeout). On 1, 2 or 3, stop and
report; clean nothing by hand.

`cv.sh` makes and checks the pools of STEP 4, STEP 5 and the cross-version
steps, with the loaded tree's own `zpool` and `zdb`, behind the same guard:

```sh
cat > /var/log/zts-dd2-1026/cv.sh <<'EOS'
#!/bin/sh
# usage (the tree named must be the loaded one):
#   cv.sh early B            : before anything reads chksum_bench: values 0, 1, 2, 7
#   cv.sh auto B             : after b1-b3: the default against the frozen kstat
#   cv.sh write <B|M> <pool> : create and fill xb (on B) or xm (on M), export it
#   cv.sh read <B|M> <pool>  : import xb or xm, read it back, write, scrub, export
#   cv.sh zdb <B|M> <pool>   : this tree's zdb -bcc and DDT list, pool exported
#   cv.sh clean <B|M> <pool> : destroy the pool if imported, remove its files
# Every pool is on one 256 MiB file vdev in $W/cv/<pool>, made with
# -o cachefile=none and mounted under $W/cvmnt-<pool>; zdb finds it with -e -p.
. /var/log/zts-dd2-1026/env.sh
cmd=$1; t=$2; pool=$3
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "cv.sh: the tree is B or M"; exit 2 ;;
esac
ZP=$W/$t/zpool; ZDB=$W/$t/zdb
start=$(date +%s)

zp() { nice -n 10 ionice -c3 "$ZP" "$@"; }
guard() {
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
		echo "GUARD FAILED: SV_B/SV_M missing or equal"; return 1
	fi
	if [ "$cur" != "$sv" ]; then
		echo "GUARD FAILED: loaded srcversion '$cur' is not $t's ($sv)"; return 1
	fi
}
poolok() {
	case $1 in
	dv0|dv1|dv2|dv7|da|xb|xm) return 0 ;;
	*) echo "bad pool name: '$1'"; return 1 ;;
	esac
}
# What pool <p> records for dedup=on in its pool directory: 14, 8 or none.
recorded() {
	out=$("$ZDB" -e -p "$W/cv/$1" -dddd "$1" 1 2>&1) || { echo zdb-failed; return; }
	v=$(echo "$out" | awk '$1 == "org.openzfs:dedup_checksum" { print $3 }')
	echo "${v:-none}"
}
# The level-0 blocks of file <f> in the root dataset of pool <p>:
# "<blake3 dedup> <sha256 dedup> <all>".
blocks() {
	"$ZDB" -e -p "$W/cv/$1" -ddddddbbbbbb "$1/" "$(stat -c %i "$2")" 2>/dev/null |
	    awk '/L0 DVA/ { n++; if (/ blake3 .* dedup /) b++; if (/ sha256 .* dedup /) s++ }
	    END { print b + 0, s + 0, n + 0 }'
}
feat() { zp get -H -o value feature@blake3 "$1" 2>&1; }
num() { case $1 in blake3) echo 14 ;; sha256) echo 8 ;; *) echo "?" ;; esac; }
rule() {   # rule <blake3> <sha256>: the default's choice for these figures
	if [ "$1" -gt 0 ] && [ "$2" -gt "$1" ]; then echo sha256; else echo blake3; fi
}
# The auto lines of pool <p> in the debug log, "<choice> <blake3> <sha256>".
dbgline() {
	sed -n "s/^.*pool '$1': dedup checksum \([a-z0-9]*\) (blake3 \([0-9]*\) MiB\/s, sha256 \([0-9]*\) MiB\/s).*\$/\1 \2 \3/p" "$DBG"
}
# The highest 256k figures of the blake3-* and sha256-* rows of a copy of
# the kstat, read as the test reads them: "<blake3> <sha256> <row> <row>".
kfig() {
	awk '$1 == "implementation" { for (i = 2; i <= NF; i++) if ($i == "256k") col = i; next }
	    col && $1 ~ /^blake3-/ && $col + 0 > b { b = $col + 0; bn = $1 }
	    col && $1 ~ /^sha256-/ && $col + 0 > s { s = $col + 0; sn = $1 }
	    END { print b + 0, s + 0, (bn == "" ? "-" : bn), (sn == "" ? "-" : sn) }' "$1"
}
wfile() {   # wfile <path> <KiB>: random data in 128 KiB writes
	dd if=/dev/urandom of="$1" bs=128k count=$(( $2 / 128 )) status=none
}
mkpool() {   # mkpool <p>: dedup=on, no compression, one new file vdev
	if [ -e "$W/cv/$1" ]; then
		echo "exists already: $W/cv/$1"; return 1
	fi
	mkdir -p "$W/cv/$1" && truncate -s 256m "$W/cv/$1/vdev0" &&
	    zp create -f -o cachefile=none -O dedup=on -O compression=off \
	    -O mountpoint="$W/cvmnt-$1" "$1" "$W/cv/$1/vdev0"
}
reimport() {
	zp export "$1" && zp import -d "$W/cv/$1" -o cachefile=none "$1"
}
rmpool() {   # rmpool <p>: destroy if imported, then its files
	if [ -n "$(zp list -H -o name "$1" 2>/dev/null)" ]; then
		zp destroy "$1"; echo "  destroyed $1 rc=$?"
	fi
	case $1 in
	?*) rm -r "${W:?}/cv/$1" && echo "  removed $W/cv/$1" ;;
	esac
	rmdir "$W/cvmnt-$1" 2>/dev/null
}
# The end of every command: its kernel messages, and no pool left imported.
finish() {   # finish <rc>
	k=$L/kern-cv-$cmd-$t-${pool:-all}-$start.txt
	journalctl -k --since "@$start" --no-pager -q > "$k" 2>&1
	kn=$(grep -iE "$KPAT" "$k" | grep -vcE "$KIGN")
	kw=$(grep -E "$KWARN" "$k" | grep -vcE "$KIGN")
	lp=$(zp list -H -o name 2>&1)
	tv=absent
	[ -e "$P" ] && tv=$(cat "$P")
	echo "cv $cmd $t ${pool:-}: rc=$1 kernel_matches=$kn kernel_warnings=$kw" \
	    "pools_imported=[$lp] tunable=$tv"
	if [ "$kn" -gt 0 ] || [ "$kw" -gt 0 ]; then
		grep -iE "$KPAT|$KWARN" "$k" | grep -vE "$KIGN" | head -n 20 | sed 's/^/  kernel: /'
	fi
	if [ "$kn" -gt 0 ] || [ -n "$lp" ]; then
		echo "STOP after cv $cmd: kernel message or a pool still imported"; exit 3
	fi
	exit "$1"
}

case $cmd in
early)
	[ "$t" = B ] || { echo "early runs on B"; exit 2; }
	guard || exit 1
	echo "=== early ($t), $(date -u +%T): the tunable after the load: $(cat "$P")"
	: > "$L/early-figures.txt"
	for v in 0 1 2 7; do
		p=dv$v; m=$W/cvmnt-$p
		echo "$v" > "$P" || { echo "cannot set the tunable to $v"; finish 1; }
		mkpool $p || { echo "$p: CREATE FAILED"; finish 1; }
		r1=$(recorded $p); f1=$(feat $p)
		# The other checksum's value, for the export and import.
		if [ "$r1" = 8 ]; then o=1; else o=2; fi
		echo "$o" > "$P"
		reimport $p || { echo "$p: EXPORT/IMPORT FAILED"; finish 1; }
		wfile "$m/f" 512 && zp sync $p || { echo "$p: WRITE FAILED"; finish 1; }
		r2=$(recorded $p); bl=$(blocks $p "$m/f"); f2=$(feat $p)
		n=$(dbgline $p | wc -l); d=$(dbgline $p | tail -n 1)
		bad=""
		case $v in
		1) want=14; [ "$n" -eq 0 ] || bad="$bad, auto lines $n (want 0)" ;;
		2) want=8; [ "$n" -eq 0 ] || bad="$bad, auto lines $n (want 0)" ;;
		*)
			# shellcheck disable=SC2086
			set -- $d
			if [ "$n" -ne 1 ] || [ $# -ne 3 ]; then
				want="?"; bad="$bad, auto lines $n (want 1)"
			else
				want=$(num "$1")
				[ "$1" = "$(rule "$2" "$3")" ] ||
				    bad="$bad, choice $1 against the rule's $(rule "$2" "$3")"
				echo "$p $d" >> "$L/early-figures.txt"
			fi ;;
		esac
		if [ "$want" = 14 ]; then wb="4 0 4"; wf=active; else wb="0 4 4"; wf=enabled; fi
		[ "$r1" = "$want" ] || bad="$bad, recorded $r1 at creation"
		[ "$r2" = "$want" ] || bad="$bad, recorded $r2 after the import"
		[ "$bl" = "$wb" ] || bad="$bad, blocks $bl (want $wb)"
		[ "$f1" = enabled ] || bad="$bad, feature@blake3 $f1 at creation"
		[ "$f2" = "$wf" ] || bad="$bad, feature@blake3 $f2 after the write (want $wf)"
		echo "$p: tunable $v; recorded $r1 at creation, $r2 after the tunable was" \
		    "set to $o and an export/import; a 512K file then: $bl (blake3 dedup," \
		    "sha256 dedup, all L0); feature@blake3 $f1, then $f2; auto line: ${d:-none}"
		if [ -z "$bad" ]; then echo "$p ok"; else echo "$p MISMATCH${bad}"; fi
		rmpool $p
	done
	echo 0 > "$P"
	echo "the tunable restored: $(cat "$P")"
	a0=$(sed -n 's/^dv0 //p' "$L/early-figures.txt")
	a7=$(sed -n 's/^dv7 //p' "$L/early-figures.txt")
	if [ -n "$a0" ] && [ "$a0" = "$a7" ]; then
		echo "dv0 and dv7: the same auto line ($a0)"
	else
		echo "dv0 and dv7: auto lines DIFFER ([$a0] [$a7])"
	fi
	echo "dbgmsg lines ignoring a recorded checksum: $(grep -c 'ignoring.*dedup checksum' "$DBG")"
	finish 0
	;;
auto)
	[ "$t" = B ] || { echo "auto runs on B"; exit 2; }
	guard || exit 1
	[ "$(cat "$P")" = 0 ] || { echo "the tunable is $(cat "$P"), not 0"; finish 1; }
	cat "$KB" > "$L/kstat-1.txt"
	# shellcheck disable=SC2046
	set -- $(kfig "$L/kstat-1.txt")
	kb=$1; ks=$2; want=$(rule "$1" "$2")
	echo "=== auto ($t), $(date -u +%T): kstat at 256k: fastest blake3 $1 MiB/s ($3)," \
	    "fastest sha256 $2 MiB/s ($4): the rule gives $want"
	bad=""
	for l in b1 b2 b3; do
		if [ -s "$L/$l-note.txt" ]; then
			x=$(cat "$L/$l-note.txt")
			if [ "$x" = "$kb $ks $want" ]; then
				echo "$l's note ($x): the same figures and choice as the kstat now"
			else
				echo "$l's note ($x): DIFFERS from the kstat now"; bad="$bad, $l's note"
			fi
		else
			echo "$l's note: none"
		fi
	done
	tl=$(dbgline testpool | grep .)
	tn=$(echo "$tl" | grep -c .)
	td=$(echo "$tl" | grep . | grep -vcx "$want $kb $ks")
	echo "the test's pool 5 auto lines in the debug log: $tn, differing from the kstat: $td"
	[ "$td" = 0 ] || bad="$bad, $td of the test's auto lines"
	a0=$(sed -n 's/^dv0 //p' "$L/early-figures.txt")
	# shellcheck disable=SC2086
	set -- $a0
	if [ $# -ne 3 ]; then
		echo "startup figures: none recorded by early"
	elif [ "$2 $3" = "$kb $ks" ]; then
		echo "startup figures (dv0: $a0): EQUAL to the kstat's: the full pass had run before early"
	else
		echo "startup figures (dv0: $a0): re-measured by the first kstat read, as expected;" \
		    "startup choice $1, choice on the full figures $want"
	fi
	mkpool da || { echo "da: CREATE FAILED"; finish 1; }
	r1=$(recorded da); f1=$(feat da)
	n=$(dbgline da | wc -l); d=$(dbgline da | tail -n 1)
	if [ "$r1" = 8 ]; then o=1; else o=2; fi
	echo "$o" > "$P"
	reimport da || { echo "da: EXPORT/IMPORT FAILED"; echo 0 > "$P"; finish 1; }
	echo 0 > "$P"
	wfile "$W/cvmnt-da/f" 512 && zp sync da || { echo "da: WRITE FAILED"; finish 1; }
	r2=$(recorded da); bl=$(blocks da "$W/cvmnt-da/f"); f2=$(feat da)
	cat "$KB" > "$L/kstat-2.txt"
	if [ "$(sed 1d "$L/kstat-1.txt" | sha256sum)" = "$(sed 1d "$L/kstat-2.txt" | sha256sum)" ]; then
		fz="the same rows (frozen)"
	else
		fz="rows DIFFER"; bad="$bad, kstat not frozen"
	fi
	w=$(num "$want")
	if [ "$w" = 14 ]; then wb="4 0 4"; wf=active; else wb="0 4 4"; wf=enabled; fi
	[ "$n" -eq 1 ] || bad="$bad, auto lines $n (want 1)"
	[ "$d" = "$want $kb $ks" ] || bad="$bad, auto line [$d] (want [$want $kb $ks])"
	[ "$r1" = "$w" ] || bad="$bad, recorded $r1 at creation"
	[ "$r2" = "$w" ] || bad="$bad, recorded $r2 after the import"
	[ "$bl" = "$wb" ] || bad="$bad, blocks $bl (want $wb)"
	[ "$f1" = enabled ] || bad="$bad, feature@blake3 $f1 at creation"
	[ "$f2" = "$wf" ] || bad="$bad, feature@blake3 $f2 after the write (want $wf)"
	echo "da: tunable 0; auto line: ${d:-none}; recorded $r1 at creation, $r2 after the" \
	    "tunable was set to $o and an export/import; a 512K file then: $bl;" \
	    "feature@blake3 $f1, then $f2"
	echo "kstat read again after the creation: $fz"
	if [ -z "$bad" ]; then echo "auto ok"; else echo "auto MISMATCH${bad}"; fi
	rmpool da
	finish 0
	;;
write)
	guard || exit 1
	case $t$pool in
	Bxb|Mxm) ;;
	*) echo "write: xb on B, xm on M"; exit 2 ;;
	esac
	m=$W/cvmnt-$pool
	# B records BLAKE3 whatever this host's figures, so that M meets it.
	[ "$t" = B ] && echo 1 > "$P"
	mkpool "$pool" || { echo "$pool: CREATE FAILED"; finish 1; }
	[ "$t" = B ] && echo 0 > "$P"
	{ wfile "$m/a" 8192 && dd if="$m/a" of="$m/a2" bs=128k status=none &&
	    wfile "$m/$t.f" 512 && zp sync "$pool"; } || { echo "$pool: WRITE FAILED"; finish 1; }
	(cd "$m" && sha256sum a a2 "$t.f") > "$L/$pool.sha"
	r=$(recorded "$pool"); f=$(feat "$pool")
	ba=$(blocks "$pool" "$m/a"); ba2=$(blocks "$pool" "$m/a2"); bf=$(blocks "$pool" "$m/$t.f")
	if [ "$t" = B ]; then w=14; wa="64 0 64"; wf="4 0 4"; wfe=active
	else w=none; wa="0 64 64"; wf="0 4 4"; wfe=enabled; fi
	bad=""
	[ "$r" = "$w" ] || bad="$bad, recorded $r"
	[ "$ba" = "$wa" ] || bad="$bad, a: $ba"
	[ "$ba2" = "$wa" ] || bad="$bad, a2: $ba2"
	[ "$bf" = "$wf" ] || bad="$bad, $t.f: $bf"
	[ "$f" = "$wfe" ] || bad="$bad, feature@blake3 $f"
	echo "$pool written by $t: recorded $r; a (8 MiB) $ba, its copy a2 $ba2, $t.f $bf" \
	    "(blake3 dedup, sha256 dedup, all L0); feature@blake3 $f;" \
	    "dedupratio $(zp list -H -o dedupratio "$pool")"
	if [ -z "$bad" ]; then echo "write $t $pool ok"; else echo "write $t $pool MISMATCH${bad}"; fi
	zp export "$pool" || { echo "$pool: EXPORT FAILED"; finish 1; }
	finish 0
	;;
read)
	guard || exit 1
	case $t$pool in
	Mxb|Bxm|Bxb) ;;
	*) echo "read: xb on M then on B, xm on B"; exit 2 ;;
	esac
	m=$W/cvmnt-$pool
	# On B the tunable is set to 1 for the whole read: an import must not
	# consult it.
	[ "$t" = B ] && echo 1 > "$P"
	if ! zp import -d "$W/cv/$pool" -o cachefile=none "$pool"; then
		echo "$pool: IMPORT FAILED"; [ "$t" = B ] && echo 0 > "$P"; finish 1
	fi
	r=$(recorded "$pool"); f1=$(feat "$pool")
	nf=$(wc -l < "$L/$pool.sha")
	nok=$( (cd "$m" && sha256sum -c "$L/$pool.sha" 2>&1) | grep -c ': OK$')
	{ wfile "$m/$t-read.f" 512 && zp sync "$pool"; } || { echo "$pool: WRITE FAILED"; finish 1; }
	bl=$(blocks "$pool" "$m/$t-read.f")
	(cd "$m" && sha256sum "$t-read.f") >> "$L/$pool.sha"
	zp scrub -w "$pool"; src=$?
	st=$(zp status -v "$pool" 2>&1)
	scan=$(echo "$st" | sed -n 's/^ *scan: //p')
	errs=$(echo "$st" | sed -n 's/^errors: //p')
	f2=$(feat "$pool")
	dr=$(zp list -H -o dedupratio "$pool")
	[ "$t" = B ] && echo 0 > "$P"
	zp export "$pool"; erc=$?
	if [ "$pool" = xb ]; then w=14; wfe=active; else w=none; wfe=enabled; fi
	if [ "$t" = B ] && [ "$w" = 14 ]; then wb="4 0 4"; else wb="0 4 4"; fi
	bad=""
	[ "$r" = "$w" ] || bad="$bad, recorded $r"
	[ "$nok" = "$nf" ] || bad="$bad, $nok of $nf files read back"
	[ "$bl" = "$wb" ] || bad="$bad, $t-read.f: $bl (want $wb)"
	[ "$src" = 0 ] || bad="$bad, scrub rc=$src"
	case $scan in *"with 0 errors"*) ;; *) bad="$bad, scan: $scan" ;; esac
	[ "$errs" = "No known data errors" ] || bad="$bad, errors: $errs"
	[ "$f1" = "$wfe" ] && [ "$f2" = "$wfe" ] || bad="$bad, feature@blake3 $f1, then $f2"
	[ "$erc" = 0 ] || bad="$bad, export rc=$erc"
	echo "$pool read by $t: recorded $r; $nok of $nf files read back bit-exact;" \
	    "$t-read.f written: $bl; scrub: $scan; errors: $errs;" \
	    "feature@blake3 $f1, then $f2; dedupratio $dr"
	if [ -z "$bad" ]; then echo "read $t $pool ok"; else echo "read $t $pool MISMATCH${bad}"; fi
	finish 0
	;;
zdb)
	guard || exit 1
	case $pool in xb|xm) ;; *) echo "zdb: xb or xm"; exit 2 ;; esac
	if [ -n "$(zp list -H -o name "$pool" 2>/dev/null)" ]; then
		echo "$pool is imported: nothing run"; exit 2
	fi
	o=$L/zdb-$t-$pool-$start.txt
	"$ZDB" -e -p "$W/cv/$pool" -bcc "$pool" > "$o" 2>&1; zrc=$?
	nl=$(grep -c 'No leaks (block sum matches space maps exactly)' "$o")
	pr=$(grep -iE 'leaked|error|!=' "$o" | grep -v 'verify nothing leaked')
	echo "zdb -bcc, $t's zdb, $pool: rc=$zrc, 'No leaks' x$nl," \
	    "problem lines $(echo "$pr" | grep -c .)"
	echo "$pr" | grep . | head -n 10 | sed 's/^/  /'
	grep -E 'bp count:|bp deduped:' "$o" | sed 's/^[[:space:]]*/  /'
	"$ZDB" -e -p "$W/cv/$pool" -D "$pool" 2>&1 | grep -E '^DDT-|^dedup = ' | sed 's/^/  /'
	finish 0
	;;
clean)
	guard || exit 1
	poolok "$pool" || exit 2
	rmpool "$pool"
	finish 0
	;;
*)
	echo "cv.sh: unknown command '$cmd'"; exit 2 ;;
esac
EOS
for f in run.sh cv.sh; do
	bash -n /var/log/zts-dd2-1026/$f; echo "$f bash syntax rc=$?"   # 0
	sh -n /var/log/zts-dd2-1026/$f; echo "$f sh syntax rc=$?"       # 0
done
```

`cv.sh` prints, for each pool it checks, one line of what it found and one
verdict line, `<what> ok` or `<what> MISMATCH, <the differences>`. A
`MISMATCH` is a result, recorded and not a stop: go on. Its last line is
`cv <command> <tree> <pool>: rc=<rc> kernel_matches=<n> kernel_warnings=<n>
pools_imported=[...] tunable=<value>`. It exits 0 when the step ran to its end,
1 when one of its commands failed (`CREATE FAILED`, `IMPORT FAILED`, ...: stop),
2 on a bad argument (stop), and 3 after `STOP after cv ...` (a kernel fault, or
a pool left imported: stop, and skip the unload of STEP 10).

## STEP 4: load B, then the tunable before anything reads the benchmark

```sh
. /var/log/zts-dd2-1026/env.sh
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load B rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"             # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ] && { $W/B/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
```

Expected: `load B rc=0`, the same srcversion twice, `zpool list rc=0` with no
pool name, and

```
zfs_dedup_log_txg_max=8 zfs_dedup_log_flush_entries_min=200 zfs_dedup_log_flush_entries_max=4294967295 metaslab_force_ganging=16777217 metaslab_force_ganging_pct=3 zfs_bclone_enabled=1 zfs_dedup_default_checksum=0
```

(the module's defaults; other values set by this VM's configuration are fine,
since every run is compared with what the load found, but
`zfs_dedup_default_checksum` must be 0). If the load fails, the srcversion is
not B's or a pool appears, stop.

Then at once, with nothing in between:

```sh
sh /var/log/zts-dd2-1026/cv.sh early B > /var/log/zts-dd2-1026/early.txt 2>&1; echo "cv.sh rc=$?" >> /var/log/zts-dd2-1026/early.txt; cat /var/log/zts-dd2-1026/early.txt
```

Expected, `<c>` being the rule's choice on the two startup figures `<b>`
(BLAKE3) and `<s>` (SHA-256) of the auto lines, and `<v>` its value (14 for
blake3, 8 for sha256):

```
=== early (B), <time>: the tunable after the load: 0
dv0: tunable 0; recorded <v> at creation, <v> after the tunable was set to <2 for 14, 1 for 8> and an export/import; a 512K file then: <4 0 4 for 14, 0 4 4 for 8> (blake3 dedup, sha256 dedup, all L0); feature@blake3 enabled, then <active for 14, enabled for 8>; auto line: <c> <b> <s>
dv0 ok
  destroyed dv0 rc=0
  removed /var/tmp/zts-dd2-1026/cv/dv0
dv1: tunable 1; recorded 14 at creation, 14 after the tunable was set to 2 and an export/import; a 512K file then: 4 0 4 (blake3 dedup, sha256 dedup, all L0); feature@blake3 enabled, then active; auto line: none
dv1 ok
  destroyed dv1 rc=0
  removed /var/tmp/zts-dd2-1026/cv/dv1
dv2: tunable 2; recorded 8 at creation, 8 after the tunable was set to 1 and an export/import; a 512K file then: 0 4 4 (blake3 dedup, sha256 dedup, all L0); feature@blake3 enabled, then enabled; auto line: none
dv2 ok
  destroyed dv2 rc=0
  removed /var/tmp/zts-dd2-1026/cv/dv2
dv7: <as dv0, with "tunable 7" and the same auto line>
dv7 ok
  destroyed dv7 rc=0
  removed /var/tmp/zts-dd2-1026/cv/dv7
the tunable restored: 0
dv0 and dv7: the same auto line (<c> <b> <s>)
dbgmsg lines ignoring a recorded checksum: 0
cv early B : rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
cv.sh rc=0
```

On an x86 host with `avx2` or `avx512f`, `<c>` is expected to be `blake3`
(`<v>` 14). The verdict lines check everything else: 1 records 14 and 2
records 8 without an auto line (a forced value never reads the benchmark), 0
and 7 record what their single auto line names, which follows the rule, the
recorded value survives the tunable's change and an export/import, the file
written after the import has the recorded checksum, and `feature@blake3` is
enabled at creation and active only after BLAKE3 blocks. This is the first
time `chksum_bench_fastest()` and its lock run in a kernel.

## STEP 5: ZTS on B, then the default against the frozen kstat

Run b1, b2 and b3, one at a time. Go on to the next only if the previous one
printed `run.sh rc=0`:

```sh
sh /var/log/zts-dd2-1026/run.sh B b1 test dedup_default_checksum; echo "run.sh rc=$?"
sh /var/log/zts-dd2-1026/run.sh B b2 test dedup_default_checksum; echo "run.sh rc=$?"
sh /var/log/zts-dd2-1026/run.sh B b3 test dedup_default_checksum; echo "run.sh rc=$?"
```

Each takes a few minutes; the runner kills a test at 600 s. Expected for each,
after its three `guards.txt` lines (`before b1` with every list empty, then
`results=PASS 3`, `zts_marks=3`, `kernel_matches=0`, `kernel_warnings=0`, then
every leftover empty and `tunables_same=yes`):

```
b1: tree B, -t tests/functional/dedup/dedup_default_checksum.ksh, 3 results: PASS 3; zfs-tests.sh rc=0, wall <s>s, log dir /var/log/zts-dd2-1026/results/<time>
<a Missing util(s) line, or "no Missing util(s) line">
Test: /var/tmp/zts-dd2-1026/B/tests/zfs-tests/tests/functional/dedup/setup (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-dd2-1026/B/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-dd2-1026/B/tests/zfs-tests/tests/functional/dedup/cleanup (run as root) [<mm:ss>] [PASS]
  dedup_default_checksum: SUCCESS lines: dd x14, zpool create x5, set_tunable32 x5, save_tunable x1, restore_tunable x1
  note: chksum_bench at 256k: blake3 <b> MiB/s, sha256 <s> MiB/s, expecting <c>
  the rule on the note's figures gives <c>: agrees
  end line: SUCCESS: zpool sync testpool
run.sh rc=0
```

with no `line:` entry. b1's note shows the full pass's figures (its pool 5
made the first read of the kstat); b2's and b3's must show the same two
figures. Then, once, with B still loaded:

```sh
sh /var/log/zts-dd2-1026/cv.sh auto B > /var/log/zts-dd2-1026/auto.txt 2>&1; echo "cv.sh rc=$?" >> /var/log/zts-dd2-1026/auto.txt; cat /var/log/zts-dd2-1026/auto.txt
cat /var/log/zts-dd2-1026/kstat-1.txt
```

Expected (`<b>`, `<s>` the kstat's highest 256k figures, `<c>` the rule's
choice on them, `<v>` its value):

```
=== auto (B), <time>: kstat at 256k: fastest blake3 <b> MiB/s (<row>), fastest sha256 <s> MiB/s (<row>): the rule gives <c>
b1's note (<b> <s> <c>): the same figures and choice as the kstat now
b2's note (<b> <s> <c>): the same figures and choice as the kstat now
b3's note (<b> <s> <c>): the same figures and choice as the kstat now
the test's pool 5 auto lines in the debug log: <0 to 3>, differing from the kstat: 0
startup figures (dv0: <c0> <b0> <s0>): re-measured by the first kstat read, as expected; startup choice <c0>, choice on the full figures <c>
da: tunable 0; auto line: <c> <b> <s>; recorded <v> at creation, <v> after the tunable was set to <2 or 1> and an export/import; a 512K file then: <4 0 4 or 0 4 4>; feature@blake3 enabled, then <active or enabled>
kstat read again after the creation: the same rows (frozen)
auto ok
  destroyed da rc=0
  removed /var/tmp/zts-dd2-1026/cv/da
cv auto B : rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
cv.sh rc=0
```

then the whole kstat (a header line, the `implementation` line and one row per
implementation; the rows the maxima came from are named above). This is the
rule applied by hand to this host's real figures, compared with what a new
pool records in its MOS. Then the dedup group, once (15 to 25 minutes; see the
rule about long commands):

```sh
sh /var/log/zts-dd2-1026/run.sh B bg group > /var/log/zts-dd2-1026/bg-console.txt 2>&1; echo "run.sh rc=$?" >> /var/log/zts-dd2-1026/bg-console.txt
cat /var/log/zts-dd2-1026/bg-console.txt
```

Expected: `results=PASS 20` (`common.run`'s 17 tests, `dedup_default_checksum`
among them, then `linux.run`'s `setup`, `dedup_legacy_gang` and `cleanup` as
`Test (Linux):` lines), the Results Summary with `PASS 20`, `not passed (0):`,
`the nine tests moved to dedup=sha256: PASS 9`, then the
`dedup_default_checksum` digest as in b1 (`dd x14`, the same note, `agrees`),
`kernel_matches=0`, every leftover empty, `run.sh rc=0`. A test that fails is
listed with its `ERROR:` lines and end line; STEP 7's comparison tells whether
master fails it too.

## STEP 6: the pool B creates for M

```sh
L=/var/log/zts-dd2-1026
sh $L/cv.sh write B xb > $L/cv-write-B-xb.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-write-B-xb.txt; cat $L/cv-write-B-xb.txt
sh $L/cv.sh zdb B xb > $L/cv-zdb-B-xb-1.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-zdb-B-xb-1.txt; cat $L/cv-zdb-B-xb-1.txt
```

Expected:

```
xb written by B: recorded 14; a (8 MiB) 64 0 64, its copy a2 64 0 64, B.f 4 0 4 (blake3 dedup, sha256 dedup, all L0); feature@blake3 active; dedupratio <about 1.94x>
write B xb ok
cv write B xb: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
cv.sh rc=0
zdb -bcc, B's zdb, xb: rc=0, 'No leaks' x1, problem lines 0
  bp count: <n>
  bp deduped: <n> ref>1: <n> deduplication: <r>
  <the DDT-blake3-* objects and the "dedup = ..." line of zdb -D>
cv zdb B xb: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
cv.sh rc=0
```

The `DDT-` lines are recorded only: with fast dedup, entries can still sit in
the DDT log when the pool is exported, so the list is not checked.

## STEP 7: ZTS on M, and the group compared result by result

Unload B only while `/sys/module/zfs/srcversion` is still B's, then load M:

```sh
. /var/log/zts-dd2-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_B" ] && cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload B rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
```

If `unload B rc` is not 0 or `lsmod` prints anything, stop. Otherwise:

```sh
. /var/log/zts-dd2-1026/env.sh
cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load M rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_M $SV_M"             # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ] && { $W/M/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo
same "M's tunables, the new one aside, as B's" \
    "$(sed 's/ zfs_dedup_default_checksum=[^ ]*//' $L/tun-M.txt)" \
    "$(sed 's/ zfs_dedup_default_checksum=[^ ]*//' $L/tun-B.txt)"
ls $P 2>&1                    # No such file or directory: master has no such parameter
```

Expected: `load M rc=0`, the same srcversion twice, `zpool list rc=0` with no
pool name (`xb` is exported: it does not appear), the tunables line ending in
`zfs_dedup_default_checksum=absent`, `ok: M's tunables, the new one aside, as
B's`, and `No such file or directory`.

```sh
sh /var/log/zts-dd2-1026/run.sh M m1 test dedup_default_checksum; echo "run.sh rc=$?"
sh /var/log/zts-dd2-1026/run.sh M m2 test dedup_default_checksum; echo "run.sh rc=$?"
sh /var/log/zts-dd2-1026/run.sh M m3 test dedup_default_checksum; echo "run.sh rc=$?"
```

Expected for each (`results=FAIL 1, PASS 2`, `zts_marks=3`, `kernel_matches=0`,
every leftover empty, `tunables_same=yes`):

```
m1: tree M, -t tests/functional/dedup/dedup_default_checksum.ksh, 3 results: FAIL 1, PASS 2; zfs-tests.sh rc=1, wall <s>s, log dir /var/log/zts-dd2-1026/results/<time>
<a Missing util(s) line, or "no Missing util(s) line">
Test: /var/tmp/zts-dd2-1026/M/tests/zfs-tests/tests/functional/dedup/setup (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-dd2-1026/M/tests/zfs-tests/tests/functional/dedup/dedup_default_checksum.ksh (run as root) [<mm:ss>] [FAIL]
Test: /var/tmp/zts-dd2-1026/M/tests/zfs-tests/tests/functional/dedup/cleanup (run as root) [<mm:ss>] [PASS]
  dedup_default_checksum: SUCCESS lines: dd x0, zpool create x0, set_tunable32 x0, save_tunable x1, restore_tunable x1
  note: none
  line: <the shell's message naming /sys/module/zfs/parameters/zfs_dedup_default_checksum, e.g. "...: cannot create [Permission denied]">
  line: ERROR: set_tunable32 DEDUP_DEFAULT_CHECKSUM 1 exited 1
  end line: <one of the two lines above: the runner may read them in either order>
run.sh rc=0
```

Then the dedup group, once:

```sh
sh /var/log/zts-dd2-1026/run.sh M mg group > /var/log/zts-dd2-1026/mg-console.txt 2>&1; echo "run.sh rc=$?" >> /var/log/zts-dd2-1026/mg-console.txt
cat /var/log/zts-dd2-1026/mg-console.txt
```

Expected: `results=FAIL 1, PASS 19`, `zfs-tests.sh rc=1`, `not passed (1):`
with the `dedup_default_checksum` line only (without `.ksh`), the "Tests with
results other than PASS that are unexpected" section naming it,
`the nine tests moved to dedup=sha256: PASS 9`, its digest as in m1 (twice:
once as a failed test, once as `dedup_default_checksum`), `kernel_matches=0`,
every leftover empty, `run.sh rc=0`.

Then the two groups, result by result:

```sh
. /var/log/zts-dd2-1026/env.sh
for l in bg mg; do
	sed -E 's|^.*/functional/dedup/([^ ]+) [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\]$|\1 \2|; s|[.]ksh | |' \
	    $L/$l-tests.txt | LC_ALL=C sort > $L/$l-results.txt
done
LC_ALL=C join -a 1 -a 2 -e none -o 0,1.2,2.2 $L/bg-results.txt $L/mg-results.txt |
    awk '{ printf "%-28s B %-7s M %-7s %s\n", $1, $2, $3, ($2 == $3 ? "same" : "DIFFERS") }' > $L/compare.txt
echo "tests: $(wc -l < $L/compare.txt), the same on both: $(grep -c ' same$' $L/compare.txt)," \
    "different: $(grep -c ' DIFFERS$' $L/compare.txt)" >> $L/compare.txt
echo "benchmark notes of b1, b2, b3 and bg: $(cat $L/b1-note.txt $L/b2-note.txt $L/b3-note.txt $L/bg-note.txt 2>/dev/null | sort -u | wc -l) distinct," \
    "$(cat $L/b1-note.txt $L/b2-note.txt $L/b3-note.txt $L/bg-note.txt 2>/dev/null | wc -l) found" >> $L/compare.txt
cat $L/compare.txt
```

Expected: 20 rows, every one `B PASS M PASS same` but
`dedup_default_checksum B PASS M FAIL DIFFERS`, then
`tests: 20, the same on both: 19, different: 1` and
`benchmark notes of b1, b2, b3 and bg: 1 distinct, 4 found`.

## STEP 8: master writes its pool, then reads B's

```sh
L=/var/log/zts-dd2-1026
sh $L/cv.sh write M xm > $L/cv-write-M-xm.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-write-M-xm.txt; cat $L/cv-write-M-xm.txt
sh $L/cv.sh read M xb > $L/cv-read-M-xb.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-read-M-xb.txt; cat $L/cv-read-M-xb.txt
sh $L/cv.sh zdb M xb > $L/cv-zdb-M-xb.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-zdb-M-xb.txt; cat $L/cv-zdb-M-xb.txt
sh $L/cv.sh zdb M xm > $L/cv-zdb-M-xm.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-zdb-M-xm.txt; cat $L/cv-zdb-M-xm.txt
```

Run them one at a time; go on only on `cv.sh rc=0`. Expected:

```
xm written by M: recorded none; a (8 MiB) 0 64 64, its copy a2 0 64 64, M.f 0 4 4 (blake3 dedup, sha256 dedup, all L0); feature@blake3 enabled; dedupratio <about 1.94x>
write M xm ok
cv write M xm: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=absent
xb read by M: recorded 14; 3 of 3 files read back bit-exact; M-read.f written: 0 4 4; scrub: scrub repaired 0B in <t> with 0 errors on <date>; errors: No known data errors; feature@blake3 active, then active; dedupratio <r>
read M xb ok
cv read M xb: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=absent
zdb -bcc, M's zdb, xb: rc=0, 'No leaks' x1, problem lines 0
...
zdb -bcc, M's zdb, xm: rc=0, 'No leaks' x1, problem lines 0
...
```

each followed by `cv.sh rc=0`. `write M xm` is what master does on a new pool
with default features: SHA-256, where the test's pool 1 (with `1`) and, on an
x86 host, the default expect BLAKE3. `read M xb`: master imports a pool that
recorded BLAKE3, ignores the entry (M's `zdb` still prints it), reads B's
BLAKE3 blocks, and writes SHA-256 into a second DDT.

## STEP 9: B reads master's pool, then its own again

Unload M only while it is the loaded module, then load B again:

```sh
. /var/log/zts-dd2-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_M" ] && cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload M rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
```

If `unload M rc` is not 0 or `lsmod` prints anything, stop. Otherwise:

```sh
. /var/log/zts-dd2-1026/env.sh
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load B rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"             # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ] && { $W/B/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-B2.txt; cmp $L/tun-B.txt $L/tun-B2.txt && echo "the same tunables as at B's first load"
```

```sh
L=/var/log/zts-dd2-1026
sh $L/cv.sh read B xm > $L/cv-read-B-xm.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-read-B-xm.txt; cat $L/cv-read-B-xm.txt
sh $L/cv.sh read B xb > $L/cv-read-B-xb.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-read-B-xb.txt; cat $L/cv-read-B-xb.txt
sh $L/cv.sh zdb B xm > $L/cv-zdb-B-xm.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-zdb-B-xm.txt; cat $L/cv-zdb-B-xm.txt
sh $L/cv.sh zdb B xb > $L/cv-zdb-B-xb-2.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-zdb-B-xb-2.txt; cat $L/cv-zdb-B-xb-2.txt
sh $L/cv.sh clean B xb > $L/cv-clean-xb.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-clean-xb.txt; cat $L/cv-clean-xb.txt
sh $L/cv.sh clean B xm > $L/cv-clean-xm.txt 2>&1; echo "cv.sh rc=$?" >> $L/cv-clean-xm.txt; cat $L/cv-clean-xm.txt
```

One at a time; go on only on `cv.sh rc=0`. Expected:

```
xm read by B: recorded none; 3 of 3 files read back bit-exact; B-read.f written: 0 4 4; scrub: scrub repaired 0B in <t> with 0 errors on <date>; errors: No known data errors; feature@blake3 enabled, then enabled; dedupratio <r>
read B xm ok
cv read B xm: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
xb read by B: recorded 14; 4 of 4 files read back bit-exact; B-read.f written: 4 0 4; scrub: scrub repaired 0B in <t> with 0 errors on <date>; errors: No known data errors; feature@blake3 active, then active; dedupratio <r>
read B xb ok
cv read B xb: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
zdb -bcc, B's zdb, xm: rc=0, 'No leaks' x1, problem lines 0
...
zdb -bcc, B's zdb, xb: rc=0, 'No leaks' x1, problem lines 0
...
  removed /var/tmp/zts-dd2-1026/cv/xb
cv clean B xb: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
  removed /var/tmp/zts-dd2-1026/cv/xm
cv clean B xm: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] tunable=0
```

each followed by `cv.sh rc=0`. `read B xm` runs with the tunable at 1 from
before the import to after the export: a pool that recorded nothing keeps
SHA-256 whatever the tunable. `read B xb` reads B's BLAKE3 blocks and M's
SHA-256 ones, and B still writes BLAKE3 into the pool that recorded it.

## STEP 10: unload, final checks, report, cleanup

Unload only when no run or step stopped with a leftover pool, device, mount or
process, and none had a kernel fault. Otherwise skip the unload, keep
everything, and say why in the report:

```sh
. /var/log/zts-dd2-1026/env.sh
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ "$cur" = "$SV_B" ]; then t=B; elif [ "$cur" = "$SV_M" ]; then t=M; else t=""; fi
if [ -n "$t" ]; then
	echo "pools: [$($W/$t/zpool list -H -o name 2>&1)], cv: [$(ls -A $W/cv)]"   # pools: [], cv: []
	(cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # unload B rc=0
else
	echo "no module of this task is loaded (srcversion '$cur')"
fi
lsmod | grep -E '^(zfs|spl) '                                       # nothing
journalctl -k --since "@$(cat $L/start.txt)" --no-pager -q > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KPAT" $L/kern-all.txt | grep -vcE "$KIGN") matches," \
    "$(grep -E "$KWARN" $L/kern-all.txt | grep -vcE "$KIGN") warnings"   # 0 matches, 0 warnings
journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN"   # the STEP 0 baseline
ls -l /etc/zfs/zpool.cache 2>&1                                    # as in STEP 0, or a 0-byte file
ls -d /testpool* $W/cvmnt-* 2>&1; losetup -a                       # No such file or directory twice, then nothing
echo "files: [$(ls -A $W/files)]"                                  # files: []
ls -ld /var/tmp/test_results /var/tmp/zts-19314 /var/tmp/zts-lz4-1026 2>&1   # as in STEP 0, untouched
df -h /var/tmp
```

Write `/var/log/zts-dd2-1026/REPORT.txt` with:

- STEP 0, short: anything found, `uname -r`, `nproc`, the CPU model and the
  flags line, the free space, the `zpool.cache`, `compatibility.d`,
  `60-zvol.rules`, `test_results`, `zts-19314` and `zts-lz4-1026` lines, and
  the kernel baseline;
- `sha-checks.txt`, `worktrees.txt`, `copy.txt`, `build.txt`, `step2.txt` and
  `step2b.txt` whole, and `env.sh` from the line `SV_B=` on;
- every load and unload line of STEP 4, 7, 9 and 10, with the tunables lines;
- `early.txt` whole;
- `guards.txt` whole;
- for each run (b1, b2, b3, bg, m1, m2, m3, mg): its result, wall time and
  the whole `<label>-notes.txt`;
- `auto.txt` and `kstat-1.txt` whole;
- `compare.txt` whole;
- every `cv-*.txt` of STEP 6, 8 and 9 whole, in the order they ran;
- the kernel findings per run and step (`kernel_matches`, `kernel_warnings`,
  and any line quoted), the whole-task count and the final count against the
  STEP 0 baseline;
- any leftover, and the STEP 10 output;
- the Expected block below, then for each of its items whether it matched,
  and which case of "What each result means" applies;
- every deviation from these instructions, with its reason.

Then print it with `cat /var/log/zts-dd2-1026/REPORT.txt`. Then, as the very
last command, and only if the unload block ran and printed `rc=0` with
nothing left in `lsmod`, remove the two directories of this task:

```sh
rm -rf /var/tmp/zts-dd2-1026 /var/log/zts-dd2-1026; echo "rm rc=$?"
ls -d /var/tmp/zts-dd2-1026 /var/log/zts-dd2-1026 2>&1   # both: No such file or directory
```

If the permission system refuses this `rm -rf`, leave both directories as
they are and say so in the reply. Do not delete them any other way. Then
reply with the whole of `REPORT.txt` as printed, and the result of the `rm`:
nothing runs after the reply.

## Expected, as a block for REPORT.txt

```
STEP 1-2: every check ok, no MISMATCH; builds B and M rc=0; compiler warnings 0, about 83
  objtool warning: lines per tree; ZFS_DEBUG lines 3; vermagic = uname -r; both libzfs read
  /var/tmp/zts-dd2-1026/share/zfs/compatibility.d; srcversions differ, M's 8852710DB871A7587B9E722.

STEP 4 (B, before any read of chksum_bench): dv0 ok, dv1 ok, dv2 ok, dv7 ok:
  1 records 14 and 2 records 8, with no auto line; 0 and 7 record the choice of their one auto
  line, the same line for both, which follows the rule on its two startup figures (blake3 on x86
  with avx2 or avx512f); every recorded value unchanged after the tunable was set to the other
  value and an export/import; the file written then has the recorded checksum; feature@blake3
  enabled at creation, active only after BLAKE3 blocks; the tunable back at 0; no "ignoring" line.

Every ZTS run: run.sh rc=0; zts_marks >= 3 (about 20 for a group); kernel_matches=0;
  kernel_warnings=0; every leftover empty; tunables_same=yes. "Missing util(s):" may list
  exportfs; it must not list zdb, zpool or zfs.

b1, b2, b3 (B): PASS 3 each (setup, the test, cleanup); dd x14, zpool create x5,
  set_tunable32 x5, save_tunable x1, restore_tunable x1; the note's choice agrees with the rule;
  end line "SUCCESS: zpool sync testpool"; no "line:" entry.
auto (B): auto ok; b1-b3's notes equal the frozen kstat's figures and choice; the test's own
  auto lines (0 to 3) all equal them; the startup figures re-measured (different); da records
  the rule's choice on the kstat's figures, keeps it across the tunable's change and an
  export/import, writes it; the kstat unchanged by a second read.
bg (B, -T dedup): PASS 20; the nine moved to dedup=sha256 PASS 9; dedup_default_checksum dd x14,
  the same note.
m1, m2, m3 (M): FAIL 1, PASS 2 each (the test fails, setup and cleanup pass); dd x0, zpool create
  x0, set_tunable32 x0, save_tunable x1, restore_tunable x1; "ERROR: set_tunable32
  DEDUP_DEFAULT_CHECKSUM 1 exited 1" beside the shell's message about
  /sys/module/zfs/parameters/zfs_dedup_default_checksum: master has no such tunable.
mg (M, -T dedup): FAIL 1 (dedup_default_checksum, as in m1), PASS 19; the nine PASS 9.
compare: 20 tests, 19 the same on both, dedup_default_checksum B PASS M FAIL; the benchmark notes
  of b1, b2, b3 and bg: 1 distinct, 4 found.

Cross-version, every line ok, every cv.sh rc=0, kernel_matches=0:
  write B xb: recorded 14, BLAKE3 dedup blocks (a 64, a2 64 deduplicated, B.f 4), active.
  zdb B xb: rc=0, No leaks, no problem line.
  write M xm: recorded none, SHA-256 dedup blocks, feature@blake3 enabled.
  read M xb: entry 14 still there, 3 of 3 files bit-exact, M writes SHA-256, scrub 0 errors,
    No known data errors.
  zdb M xb, zdb M xm: rc=0, No leaks, no problem line.
  read B xm (tunable 1): recorded none, 3 of 3 files bit-exact, B writes SHA-256, scrub clean,
    feature@blake3 still enabled.
  read B xb: recorded 14, 4 of 4 files bit-exact (B's BLAKE3 and M's SHA-256), B writes BLAKE3,
    scrub clean.
  zdb B xm, zdb B xb: rc=0, No leaks, no problem line; both pools removed.

STEP 10: unload B rc=0; nothing in lsmod; whole task 0 matches, 0 warnings; the boot's count
  equal to the STEP 0 baseline; zpool.cache as in STEP 0 or 0 bytes; nothing left.
STOP only on: a failed check or guard, a leftover, a tunable left changed, an outer timeout,
  a failed command in a cv.sh step, or a kernel VERIFY/ASSERT/panic/BUG/oops/hung task.
```

## What each result means

- **The proof holds** when every item of the Expected block matches. Then, in
  a kernel: a pool created with `feature@blake3` records what the tunable
  selects (1 BLAKE3, 2 SHA-256, 0 and any other value the faster of the two on
  this host, by the documented rule on the module's own figures, before and
  after the full benchmark), keeps it across a tunable change and an
  export/import, and writes it for `dedup=on`; forced values never consult
  the benchmark; pools created without the feature or with
  `openzfs-2.1-linux` keep SHA-256, also after `zpool upgrade`; explicit
  checksums are unchanged; the figures freeze at the first kstat read and
  the new lock raised no kernel complaint at load, on kstat reads or at
  creation; master's module, which has no tunable, fails the test before any
  pool; the nine tests' move to `dedup=sha256` passes on both trees; master
  imports and scrubs a pool that recorded BLAKE3 and writes SHA-256 into it,
  and this head imports master's pool and keeps SHA-256 there whatever the
  tunable.
- **A finding against the head**:
  - an `early` or `auto` MISMATCH. A wrong value at creation for 1 or 2: the
    tunable is not applied. For 0 or 7, a recorded value other than the auto
    line's choice, or a choice against the rule on the line's own figures:
    `ddt_dedup_checksum_auto()` does not do what it documents. A value changed
    after the import, or a file written after it with the other checksum:
    `ddt_load()` does not apply the entry, or the tunable leaks into an
    import. An auto line for 1 or 2: a forced value read the benchmark; none
    for 0 or 7: the decision is not logged. A feature state other than the
    expected one: activation. In `auto`, a note, a test's auto line or `da`'s
    line with figures other than the kstat's: the figures changed after the
    first read (the freeze, or the lock);
  - a b run that fails. The `dd x<k>` count and the `line:` entries locate it
    (files 1-5 are pool 1, 6-8 pool 2, 9-12 pool 3, 13 pool 4, 14 pool 5; see
    the table of the test). An exception that is the environment, not the
    head: pool 4's `zpool create` failing with `could not read/parse feature
    file(s): openzfs-2.1-linux` after 12 writes (libzfs did not find `$CD`);
  - a bg test that fails on B only (`compare.txt`: B FAIL, M PASS), other
    than `dedup_default_checksum`: most likely a `dedup=on` test depending on
    SHA-256 without saying so;
  - a cross-version MISMATCH: B writing BLAKE3 into `xm` (an existing pool
    following the tunable), B not writing BLAKE3 into `xb` after M's writes
    (the entry lost or ignored), a file not read back bit-exact, a scrub
    error, a `zdb -bcc` leak or error line;
  - any kernel `VERIFY`, `ASSERT`, `BUG:`, panic or hung task: stop; the line
    and the state it happened in are the result. The new lock is taken at
    module load (STEP 4 and STEP 9's reload), on every read of the kstat (the
    test's pool 5, `cv.sh auto`) and at every creation with the tunable at 0
    or 7.
- **Not the head's**: a test that fails on both trees in `compare.txt`
  (master's or this VM's: say so); a failure in a `setup` or `cleanup`, or
  before `set_tunable32` on M (`save_tunable` refusing because a
  `tunable-DEDUP_DEFAULT_CHECKSUM` file was left in `$W/files`, which `run.sh`'s
  guard should have caught): the environment.
- **M passes `dedup_default_checksum`, or fails anywhere but the first
  `set_tunable32`**: the M tree is not what STEP 1 checked, or the run did not
  start; report the lines.
- **The startup choice differs from the choice on the full figures** (in
  `auto.txt`): the two algorithms are within the noise of one ~1 ms
  measurement on this host. Each pool keeps what it recorded, so this is not
  a defect; report both pairs of figures.
- **The startup figures EQUAL the kstat's** (in `auto.txt`): the kstat had
  been read between B's load and `cv.sh early`, so dv0 and dv7 decided on the
  full figures. The rest of the proof stands; report it.
- **`WARNING:` lines in the kernel log**: quoted with their run or step. A
  finding only if B shows one that M does not.
- A result that differs between b1, b2 and b3, or between m1, m2 and m3, is
  reported as it is, run by run.

Estimated duration: about 2 hours (checks and sources 10 minutes, the two
builds about 30, STEP 4 3, b1-b3 about 6, `auto` 1, `bg` 15 to 25, STEP 6 2,
the swap and m1-m3 about 5, `mg` 15 to 25, STEP 8 and 9 with their swap
about 8, report and cleanup 10).
