# VM 106 (CHSONVTST03): kernel proof of `zfs-rewrite-physical-raw-send` at 86670aff43

These instructions are for the session on VM 106 (Debian 13). They produce the
first kernel evidence for the fork branch `zfs-rewrite-physical-raw-send` of
https://github.com/tigerblue77/zfs: one commit,
`86670aff43e9227126e45fd224cb1a16ee9e1a2d` "Include zfs rewrite -P blocks in
raw encrypted sends", on upstream master
`5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53` "ZTS: sync checksum test data
before fault injection". The session builds two fresh debug trees in its own
directories, runs everything below in the kernel, cleans up, and replies with
`REPORT.txt`. This document is read from a clone of the branch
`vm-tasks-1026` of the same fork in `/var/tmp/vm-task-106r` (the doc
checkout, `DOC` below); the last command of STEP 6 removes it with the task's
own directories.

**The bug.** `zfs rewrite -P` rewrites a file's blocks but keeps their logical
birth txg, and every incremental send prunes by logical birth
(`send_traverse_thread()` always adds `TRAVERSE_LOGICAL`), so it leaves those
blocks out. For a raw stream (`zfs send -w`) of an encrypted dataset that
breaks the replica: rewriting an encrypted block gives it a new IV and MAC,
the stream carries the sender's dnode blocks, whose MACs cover the new block
MACs through the indirect blocks, and the receiver still holds the old
blocks. The receive succeeds. Once the replica's key is loaded, its dnode
blocks fail authentication: the upstream report shows `cannot mount
'<replica>': Input/output error`, permanent errors in `zpool status -v` and
`ereport.fs.zfs.authentication` events.

**The commit.** `setup_to_thread()` in `module/zfs/dmu_send.c` adds
`TRAVERSE_LOGICAL` unless `rawok && to_os->os_encrypted` (exactly when the
stream gets `DMU_BACKUP_FEATURE_RAW`), and `send_traverse_thread()` no longer
adds it. A raw send of an encrypted dataset then prunes by `BP_GET_BIRTH()`,
which `include/sys/spa.h` defines as the physical birth of a block with the
rewrite bit and the logical birth of every other block; `dbuf_write()` sets
the bit at level 0 only, and a clone clears it (`dmu_brt_clone()`).
Non-raw sends, `zfs send -w` of an unencrypted dataset and `zfs diff` keep
pruning by logical birth. `zfs-rewrite.8` documents the cost, and the new ZTS
test `cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send` checks the
behaviour. 5 files, 132 insertions, 3 deletions; the code is one changed
line and 8 new ones in `dmu_send.c`.

**Why this run.** When this document was written, the commit had been built
and checked in userspace only (`--with-config=user`, `make unit`, the
checkstyle targets). This run is its kernel proof on a controlled VM: the new
test against master as the failing-first reference, the send and receive
tests compared tree by tree, and the bug's own reproduction by hand on both
module codes, which no CI run gives.

| Tree | Path | Code | Tests |
| --- | --- | --- | --- |
| `B` | `/var/tmp/zts-rps-1028/B` | the head `86670aff43` | its own |
| `M` | `/var/tmp/zts-rps-1028/M` | master `5ebd13aa9`, the head's parent | master's, plus the head's three files under `tests/`: the new test, and its entries in `tests/runfiles/common.run` and `tests/zfs-tests/tests/Makefile.am` |

Both trees are built here with `./autogen.sh && ./configure --enable-debug &&
make -j8` (8 jobs, not `nproc`: the cap of every build on this VM, see the
rules). Master `5ebd13aa9` changes only files under `tests/` since
`a128b8a075`, so M's module code is the master module every earlier task on
this VM built (srcversion `8852710DB871A7587B9E722`); STEP 1 checks it.

What the run establishes, B first, then M:

1. **The new test alone** (`bn` on B, `mn` on M): PASS on B; on M, FAIL at its
   first check, the raw incremental holding 0 WRITE records for the file
   instead of 32.
2. **The `zfs_rewrite` group** (`bg`, `mg`) **and 16 send and receive tests**
   (`b01`-`b16`, `m01`-`m16`, one `-t` run each): the same result on both
   trees, test by test, except the new test.
3. **The bug by hand** (`rp.sh`, the last step on each tree): on B the replica
   reads back bit-exact with no authentication ereport; on M whatever the
   replica does is recorded, a mount or read error and authentication
   ereports being what the analysis predicts. M's reproduction is the very
   last kernel step, so that whatever the kernel does there cannot cut the
   rest short.
4. **Throughout**: the kernel log of every run and step, which must stay
   clean, and in STEP 6 the module's unload.

## What the new test does, and why M fails

`zfs_rewrite_physical_raw_send.ksh` runs on the group's pool (`setup.ksh`:
`default_setup` on the first of the three loop devices `zfs-tests.sh`
provides) and does, in this order:

| Step | What | Log lines |
| --- | --- | --- |
| 1 | `testpool/testfs1`, encrypted (passphrase in `$TEST_BASE_DIR/rewrite_raw_send.key`), recordsize 128k; `file` of 32 blocks of random data; `@a`; `zfs send -w testpool/testfs1@a`, received as `testpool/testfs2` | `SUCCESS: eval zfs recv testpool/testfs2 < ...` (1st) |
| 2 | `testpool/testfs3`, unencrypted, the same file; `@a` | |
| 3 | `zfs rewrite -P` of both files; `@b` on both | `SUCCESS: zfs rewrite -P ...` |
| 4 | `zfs send -w -i @a testpool/testfs1@b`: 32 WRITE records for the file's object, counted with `zstream dump -v` | `NOTE: zfs send -w -i @a testpool/testfs1@b: <n> WRITE records for object <o>`, then `log_must test <n> -eq 32` |
| 5 | `zfs send -i @a testpool/testfs1@b` and `zfs send -w -i @a testpool/testfs3@b`: 0 each | two more `NOTE: ... WRITE records ...` lines |
| 6 | `zpool events -c`; the raw incremental received into `testpool/testfs2`, its key loaded, mounted, `cmp` with the source file; any `ereport.fs.zfs.authentication` in `zpool events` fails the test | `SUCCESS: eval zfs recv ...` (2nd), `SUCCESS: zfs load-key ...`, `SUCCESS: cmp /testpool/testfs1/file /testpool/testfs2/file`; on an ereport, `Reading the replica posted an authentication ereport` |

**On M**, the raw incremental of step 4 prunes the rewritten blocks by their
unchanged logical birth: the note says `0 WRITE records`, and the test ends
with `ERROR: test 0 -eq 32 exited 1` before any receive of the incremental.
That is the failing-first check on master: the test fails at the cause, not
at its symptom, so a ZTS run on M never builds a broken replica. The symptom
is shown separately by `rp.sh`.

**The `physical_rewrite` feature.** `zfs rewrite -P` returns `ENOTSUP` unless
`feature@physical_rewrite` (`com.truenas:physical_rewrite`, per dataset,
read-only compatible) is enabled on the pool (`zfs_rewrite()` in
`module/zfs/zfs_vnops.c`). The group's pool comes from `default_setup`, whose
`zpool create -f testpool <disk>` names no `-d`, no `-o feature@...` and no
`compatibility`, so every feature is enabled; the sibling
`zfs_rewrite_physical.ksh` relies on the same. Three checks show it here:
`run.sh` prints the setup's `SUCCESS: zpool create` line of every
`zfs_rewrite` run; the new test's digest counts its `SUCCESS: zfs rewrite -P`
line (1 on both trees: the rewrite itself works on master); and `rp.sh`
creates its own pool the same way and prints `feature@physical_rewrite`
before the rewrite (`enabled`) and after it (`active`).

## The bug by hand: `rp.sh`

The upstream report's sequence, with each tree's own `zpool`, `zfs` and
`zstream`. Pool `rp` on one sparse 1 GiB file vdev in `$W/rp`, created with
`-o cachefile=none` and default features (no `-d`), mounted under
`$W/rpmnt`. `rp/src` is encrypted (`aes-256-gcm`, a 32-byte raw key in
`$W/rp/key`, never printed), recordsize 128k, and holds a 16 MiB file `f`
written with `dd ... conv=fsync` (128 level-0 blocks). Then: `@a`, a raw full
send received as `rp/dst`; `zfs rewrite -P` of `f`; `@b`; the raw incremental
`@a..@b` and, for comparison only, the non-raw one, both counted with
`zstream dump -v`; `zpool events -c`; the raw incremental received into
`rp/dst`; `zfs load-key -L file://...` and `zfs mount` of `rp/dst`; `f` read
back and compared by sha256 with the source; then the events, the `errors:`
line of `zpool status -v`, and one verdict line.

| | B | M (what the analysis predicts) |
| --- | --- | --- |
| WRITE records for `f` in the raw incremental | 128 | 0 |
| in the non-raw incremental | 0 | 0 |
| incremental receive, load-key | rc 0, rc 0 | rc 0, rc 0 |
| mount of `rp/dst` | rc 0 | rc 1, `cannot mount 'rp/dst': Input/output error` |
| `f` read back | identical | not read (not mounted), or a read error |
| `ereport.fs.zfs.authentication` | 0 | 1 or more |
| `errors:` | `No known data errors` | `Permanent errors have been detected...` |
| verdict | `replica READABLE` | `replica BROKEN or refused` |

Whatever M does is recorded as it is: each line of `rp.sh` prints the rc and
the message of its command. **A clean read on M** (`mount rc=0`, `identical`,
0 authentication ereports, `No known data errors`, so `replica READABLE`)
contradicts the analysis of the bug and must be reported as such, with the
whole `rp-M.txt`, not explained away. So must a raw incremental on M that
carries the 128 blocks.

## The send and receive tests, and why these

The head prunes differently from master only when a raw send of an encrypted
dataset meets a level-0 block with the rewrite bit: everywhere else
`BP_GET_BIRTH()` is the logical birth that `TRAVERSE_LOGICAL` compared. No
test of the tree but the new one rewrites with `-P` and then sends raw (the
sibling `zfs_rewrite_physical` sends non-raw from an unencrypted dataset).
Every other test is therefore expected to give the same result on both
trees, and those that send raw are what shows that nothing else moved. The
16 below are every test of the tree that makes a raw (`-w`) incremental of an
encrypted dataset, which is where the comparison of births matters at all (a
full stream has `fromtxg` 0 and prunes nothing), plus raw full sends and
receives as a control (`11`, `12`) and the resumed raw sends (`15`, `16`: a
resume token keeps `rawok` for raw streams only, so a resumed raw send takes
the same path):

| Label | Test (under `tests/functional`) | Why |
| --- | --- | --- |
| `01` | `rsend/send_encrypted_files` | raw full and raw incremental (`-wp -i`) of an empty, a small, a 32M and a large sparse file, xattrs, and 1000 empty files removed between the snapshots; contents checked |
| `02` | `rsend/send_encrypted_truncated_files` | raw incremental after truncations: FREE records and holes in a raw incremental |
| `03` | `rsend/send_encrypted_freeobjects` | raw incremental whose FREEOBJECTS and OBJECT records share one dnode block |
| `04` | `rsend/send_encrypted_hierarchy` | raw replication incremental (`-wR -i`) of several encryption roots |
| `05` | `rsend/send_encrypted_holds` | raw incremental with holds (`-w -h -i`) |
| `06` | `rsend/send_mixed_raw` | raw and non-raw incrementals of one encrypted dataset side by side: both arms of `rawok && os_encrypted` |
| `07` | `rsend/send_raw_ashift` | raw incrementals into a pool of greater ashift |
| `08` | `rsend/send_raw_spill_block` | raw incrementals of spill blocks |
| `09` | `rsend/send_realloc_encrypted_files` | raw incrementals with reallocated objects, several passes |
| `10` | `cli_root/zfs_receive/zfs_receive_raw_incremental` | raw incremental receive, key load, checksum of a file |
| `11` | `cli_root/zfs_receive/zfs_receive_raw` | raw full receive (control: `fromtxg` 0) |
| `12` | `cli_root/zfs_send/zfs_send_raw` | raw full sends with `-p` and `-R`, of a filesystem and with the key unloaded (control) |
| `13` | `cli_root/zfs_change-key/zfs_change-key_mismatch` | raw incremental onto a received child whose key was re-inherited |
| `14` | `send_xdr_encoding/xdr_bookmark_raw` | raw incremental from a redaction bookmark |
| `15` | `send_xdr_encoding/xdr_resume_raw` | resumed raw full send |
| `16` | `send_xdr_encoding/xdr_resume_bookmark_raw` | resumed raw incremental from a bookmark |

Left out on purpose:

- `rsend/send-wR_encrypted_zvol`, `rsend/recv_dedup_encrypted_zvol` and
  `cli_root/zpool_import/zpool_import_errata4` (raw incrementals of an old
  pool image): they read `/dev/zvol/...` links, which no `60-zvol.rules` makes
  on this VM, so they fail on every tree.
- `send_xdr_encoding/xdr_bookmark_raw_with_write` and
  `xdr_resume_bookmark_raw_with_write`: their own header documents a known
  intermittent upstream failure of raw sends from a redaction bookmark after a
  write (about one run in three), which would make a B/M difference
  meaningless; `14` and `16` cover the same streams without the write.
- `userquota/userspace_send_encrypted`: its group's `setup.ksh` adds a group
  and two users to the VM's account files.
- `cli_root/zpool_import/zpool_import_errata3`: its raw sends are expected to
  fail by design.
- The other raw full sends (`rsend/send_raw_large_blocks`, the corrective
  receives, `zfs_receive_-wR-encrypted-mix`, `zfs_receive_raw_-d`,
  `zfs_receive_raw_-b`, `xdr_raw`, `xdr_replication`,
  `xdr_redacted_received_raw`): the case `11`, `12` and `15` already show.

## Rules for the whole run

- Run everything as root, in bash, in this order. The ZTS runs go through
  `runuser -u zts` inside `run.sh`; nothing else runs as `zts`. Shell variables
  may not survive between your commands, so every block below starts by
  sourcing `/var/log/zts-rps-1028/env.sh` or by setting what it needs.
- A production hypervisor sits underneath this VM. The builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  module loads and every ZTS run and pool step go through `nice -n 10 ionice
  -c3` too (the blocks and the scripts do it), one at a time, with nothing
  else running beside them.
- Post nothing, push nothing, install nothing (no package, no
  `make install`), never reboot, and never run `scripts/zfs-helpers.sh` or
  `zfs-tests.sh -x`. Rebuild nothing once STEP 2 has built the trees.
  `scripts/zfs.sh -r` (reload) and `-u` (unload) run only here, on this VM,
  and only in the blocks that say so, behind their guards.
- Work only under `/var/tmp/zts-rps-1028` (W) and `/var/log/zts-rps-1028`
  (L). Never write to, or delete, `/var/tmp/test_results`, another
  `/var/tmp/zts-*` or `/var/log/zts-*` directory, another
  `/var/tmp/vm-task-*` checkout, or anything else this task did not create;
  the doc checkout is only read until STEP 6 removes it. ZTS pools mount
  under `/testpool*` (and `send_raw_ashift`'s under `/pool9` and `/pool12`)
  while they exist; the tests' cleanup removes those directories, and
  `run.sh` checks that it did.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records the ZTS pools in it while they exist and truncates it to 0 bytes
  when they are gone; the pool of `rp.sh` never touches it. STEP 0 and STEP 6
  record it.
- Never run a `zpool`, `zfs`, `zdb` or `zstream` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Never print the environment (`env`, `printenv`, `set`,
  `/proc/*/environ`) or a key file (`$W/rp/key`, the tests' key files).
- The only `rm -rf` allowed is the one at the end of STEP 6. The only other
  removals are `rp.sh`'s own: its pool directory `$W/rp` (`rm -r`) and its
  empty mount points under `$W/rpmnt` (`rmdir`).
- If a check does not print what its comment or the Expected lines say, stop
  at once and report. Do not rebuild, repair or retry. If your permission
  system refuses a command, do not work around it: report it and stop there.
  A kernel panic, oops, `VERIFY`, `ASSERT`, `BUG:` or hung-task message means
  stop and report, in whatever state it happened; never reboot. A stop in
  STEP 0 changes nothing: report and reply. After any later stop, go straight
  to STEP 6 with what you have, and skip its unload if a pool, a device, a
  mount or a process is left, or the kernel logged a fault: a busy or wedged
  module must not be forced out. A test result other than the expected one is
  a result, recorded, never a stop by itself.
- The two builds take about 30 minutes together, a list of 16 runs 30 to 50.
  If your shell tool cannot wait that long for a command in the foreground,
  start that one command with the tool's own background mode and wait until
  it exits; do not use `&` or `nohup`, and run nothing else meanwhile.
- The session cannot run anything after its final reply: the cleanup at the
  end of STEP 6 is the last command, and the reply comes after it. Leave no
  background command, loop or watcher behind.

## What earlier runs on this VM taught, and these steps already handle

- `configure --enable-debug` writes three `#define ZFS_DEBUG 1` lines in
  `zfs_config.h` (one unconditional, two under `#ifdef`): 3 is a debug build.
- objtool prints about 83 `warning:` lines per tree (`spl_panic()` and
  `luaD_throw()` lacking `__noreturn`), harmless. Compiler warnings are
  counted only in the `file:line:col: warning:` form,
  `:[0-9]+:[0-9]+: warning:`.
- A `-t` run names its test with `.ksh` in its `Test:` line, a group run
  without it, and a test of a `linux.run` section is printed `Test (Linux):`:
  every pattern below allows all three. A literal parenthesis in a regex is
  written `[(]`, never `\(`.
- The kernel-log scan drops the ZTS `-K` markers (`ZTS run <test>`) and the
  `spa_import_progress_set_notes_impl` notes. ZTS clears dmesg after a failed
  test, so kernel messages are read per run from `journalctl -k`.
- This VM has no `60-zvol.rules`, so `/dev/zvol/...` links never appear and
  zvol tests fail on every tree. None of the tests run here needs one (see
  "Left out on purpose"); STEP 0 only records it.
- The trees are not installed, so an uninstalled tree finds no
  `compatibility.d`. No test run here sets `compatibility=`.
- Master's known test leaks (`tmp.*` files of `vdevs_in_pool()`'s callers,
  `removal_with_zdb`'s `tmp.*` file, `removal_condense_export`'s `testdir1`)
  come from tests not run here, and no directory run here calls
  `vdevs_in_pool()`. If those shapes appear in `$W/files` anyway, `run.sh`
  records them as `known=[...]` and stops only on anything else left there.
- A test that writes nothing on stderr has no `merged` file: its `stdout` is
  read instead. Each run waits 2 s after it ends: two runs started in the same
  second would share a results directory.
- git refuses to work in a tree owned by another user: all git work happens
  in STEP 1 and STEP 2, before the `chown` to `zts`. `zfs-tests.sh -d` must
  point at a mode 1777 directory.

## STEP 0: the VM must be free

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rps-1028 L=/var/log/zts-rps-1028 DOC=/var/tmp/vm-task-106r
date -u '+%F %T'
id -un                        # root
uname -r                      # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release   # Debian GNU/Linux 13 (trixie)
nproc                         # 16
umask                         # 0022
grep -m1 '^model name' /proc/cpuinfo
for f in aes avx2 avx512f; do
    grep -qw "$f" /proc/cpuinfo && printf '%s=yes ' $f || printf '%s=no ' $f
done; echo                    # recorded: only the speed of encryption depends on them
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zdb zed zstream   # nothing: no system ZFS
pgrep -x zed                  # nothing
ls -l /var/run/zed.pid 2>&1   # No such file or directory: zfs.sh -r and -u signal the pid it names
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|autogen|configure|make|gcc|cc1|raidz_test'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -d $W $L 2>&1              # No such file or directory, twice
ls -A /var/tmp; ls -d /var/log/zts-* 2>&1
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|zts-rps|rpmnt' /proc/mounts   # nothing
ls -d /testpool* /pool9 /pool12 2>&1   # No such file or directory, three times
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules 2>&1; ls -d /dev/zvol 2>&1
df -h /var/tmp /var/log       # need 15G free on /var/tmp
free -h
id zts; sudo -n -l -U zts | grep -i nopasswd
command -v git ksh python3 modinfo sha256sum truncate runuser timeout   # eight paths
command -v xxh128sum          # recorded: three zfs_rewrite tests need it
journalctl -k -n 1 --no-pager -q   # one kernel line
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/r/VM-TESTS-106-rps.md 2>&1
```

Keep this output, which goes into `REPORT.txt`. The number printed after the
one kernel line is the kernel-message baseline. Directories of earlier tasks
under `/var/tmp` and `/var/log` (`zts-*`,
`vm-task-*`, `test_results`) are recorded only: their presence is not a stop,
and this task never touches them. The `60-zvol.rules`, `/dev/zvol`,
`xxh128sum` and CPU lines are recorded too. The doc checkout line is expected
as `doc checkout: top /var/tmp/vm-task-106r, branch vm-tasks-1026, head
<the branch's head>, origin https://github.com/tigerblue77/zfs` (with or
without `.git`); if you read this document from another place, say so in the
report: STEP 6 then leaves that place alone and removes no doc checkout.

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, a system `zpool`, `zfs`,
  `zdb`, `zed` or `zstream` is installed, `zed` runs, or `/var/run/zed.pid`
  exists;
- a test or build process is running. The `pgrep -af` also matches the shell
  that runs this block, because that shell's command line contains the
  pattern: ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an
  idle shell;
- `$W` or `$L` exists: this task would then have started once already;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*`, `/pool9` or `/pool12` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty (a pool would appear at
  module load);
- less than 15 GB is free on `/var/tmp` (two built trees, the clone, the
  tests' three sparse 4 GiB file vdevs and what the send tests write);
- user `zts` is missing or has no passwordless sudo;
- one of the eight commands is missing, or `journalctl -k` prints no kernel
  line (the runs read their kernel messages from the journal).

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself:
STEP 2 checks that the modules were built for the running one, and the report
records `uname -r`. `xxh128sum` missing is not a stop either: then
`zfs_rewrite`, `zfs_rewrite_physical` and `zfs_rewrite_skip_width` fail on
both trees (`Missing util(s): xxh128sum`), which the comparison shows.

## STEP 1: directories, sources, the head's test in M (all git work, part one)

The environment file first, then the clone of the branch (read-only towards
GitHub) and the fetch of upstream master, to check the merge base:

```sh
export PATH=/usr/sbin:/sbin:$PATH
mkdir /var/tmp/zts-rps-1028 /var/log/zts-rps-1028; echo "mkdir rc=$?"   # 0
cat > /var/log/zts-rps-1028/env.sh <<'EOF'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-rps-1028
L=/var/log/zts-rps-1028
DOC=/var/tmp/vm-task-106r
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
BRANCH=zfs-rewrite-physical-raw-send
# The branch's head (one commit) and its parent, upstream master; HP is the
# master of the earlier builds on this VM, whose module code M's is.
HB=86670aff43e9227126e45fd224cb1a16ee9e1a2d
HM=5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53
HP=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
# The new test (F) in its directory (D); every file the head changes under
# tests/ (TB) and outside it (CODE), in git's order; the test's blob.
D=tests/zfs-tests/tests/functional/cli_root/zfs_rewrite
F=$D/zfs_rewrite_physical_raw_send.ksh
TB="tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am $F"
CODE="man/man8/zfs-rewrite.8 module/zfs/dmu_send.c"
FBLOB=d6cb3dda2945d8481dda5d9e3d8467ff44b812dd
# The send and receive tests of STEP 4 and 5: <nn>:<path under
# tests/zfs-tests/tests/functional, without .ksh>.
LIST="01:rsend/send_encrypted_files 02:rsend/send_encrypted_truncated_files
03:rsend/send_encrypted_freeobjects 04:rsend/send_encrypted_hierarchy
05:rsend/send_encrypted_holds 06:rsend/send_mixed_raw
07:rsend/send_raw_ashift 08:rsend/send_raw_spill_block
09:rsend/send_realloc_encrypted_files
10:cli_root/zfs_receive/zfs_receive_raw_incremental
11:cli_root/zfs_receive/zfs_receive_raw 12:cli_root/zfs_send/zfs_send_raw
13:cli_root/zfs_change-key/zfs_change-key_mismatch
14:send_xdr_encoding/xdr_bookmark_raw 15:send_xdr_encoding/xdr_resume_raw
16:send_xdr_encoding/xdr_resume_bookmark_raw"
LIST=$(echo $LIST)
# Kernel findings: KPAT stops the task (BUG: covers "sleeping function
# called from invalid context"); KWARN lines are quoted, not a stop by
# themselves (a kernel WARN() also prints a call trace, which KPAT has).
# KIGN drops the ZTS -K markers and the import progress notes.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than|circular locking'
KWARN='WARNING:'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The send and receive tunables, which the tests must leave as loaded
# (send_mixed_raw sets zfs_disable_ivset_guid_check and restores it).
TUN='zfs_disable_ivset_guid_check zfs_send_corrupt_data zfs_send_unmodified_spill_blocks send_holes_without_birth_time zfs_recv_best_effort_corrective'

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

# Build <tree> in place, niced: autogen, configure (debug), make -j8.
# Stops on a failed build.
build_tree() {   # build_tree <B|M>
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug && make -j8") \
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
. /var/log/zts-rps-1028/env.sh
date +%s > $L/start.txt
git clone --no-checkout --single-branch --branch $BRANCH $FORK $W/repo > $L/clone.log 2>&1; echo "clone rc=$?"   # 0
git -C $W/repo fetch $UPSTREAM master > $L/fetch.log 2>&1; echo "fetch rc=$?"   # 0
```

Then the head, its parent, the files it changes, and one worktree per tree:

```sh
. /var/log/zts-rps-1028/env.sh
{
same "$BRANCH" "$(git -C $W/repo rev-parse origin/$BRANCH)" $HB
same "its parent" "$(git -C $W/repo rev-parse $HB^)" $HM
same "merge base with upstream master" "$(git -C $W/repo merge-base FETCH_HEAD $HB)" $HM
same "files under tests/" "$(git -C $W/repo diff --name-only $HM $HB -- tests | xargs)" "$TB"
same "code files" "$(git -C $W/repo diff --name-only $HM $HB -- . ':!tests' | xargs)" "$CODE"
same "diff stat" "$(git -C $W/repo diff --stat $HM $HB | tail -1)" " 5 files changed, 132 insertions(+), 3 deletions(-)"
same "patch-id" "$(git -C $W/repo diff $HM $HB | git -C $W/repo patch-id --stable | cut -d' ' -f1)" b04d23cf66874b82d044e35dcd24cb9dd56286c3
same "the test's blob" "$(git -C $W/repo rev-parse $HB:$F)" $FBLOB
same "master's changes outside tests/ since $HP" "$(git -C $W/repo diff --name-only $HP $HM -- . ':!tests' | wc -l)" 0
same "the 16 tests unchanged by the head" "$(for e in $LIST; do git -C $W/repo diff --quiet $HM $HB -- tests/zfs-tests/tests/functional/${e#*:}.ksh || echo ${e#*:}; done | xargs)" ""
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
ok: zfs-rewrite-physical-raw-send
ok: its parent
ok: merge base with upstream master
ok: files under tests/
ok: code files
ok: diff stat
ok: patch-id
ok: the test's blob
ok: master's changes outside tests/ since a128b8a075b5b13c737e56914cd7043bd3e9ebf8
ok: the 16 tests unchanged by the head
worktree M rc=0 at 5ebd13aa9a
worktree B rc=0 at 86670aff43
86670aff43 Include zfs rewrite -P blocks in raw encrypted sends
parent 5ebd13aa9a ZTS: sync checksum test data before fault injection
```

Any `MISMATCH` (the branch was force-pushed or rebased since 28 Oct) means
stop.

Copy the head's three files under `tests/` into M (`git checkout` stages
them, so that `git diff HEAD` shows the new file too), and check both trees:

```sh
. /var/log/zts-rps-1028/env.sh
{
git -C $W/M checkout $HB -- $TB && echo "M: tests copied"
git -C $W/M diff HEAD --stat | tail -1
same "M: changes outside tests/" "$(git -C $W/M diff HEAD --name-only -- . ':!tests' | wc -l)" 0
same "M: tests/ as in the head" "$(git -C $W/M diff $HB -- tests | wc -l)" 0
same "M: differs from the head in" "$(git -C $W/M diff --name-only $HB | xargs)" "$CODE"
same "B: tracked changes" "$(git -C $W/B status --short --untracked-files=no | wc -l)" 0
same "the 16 tests in both trees" "$(for t in M B; do for e in $LIST; do [ -x $W/$t/tests/zfs-tests/tests/functional/${e#*:}.ksh ] || echo $t:${e#*:}; done; done | xargs)" ""
for t in M B; do
	cd $W/$t
	echo "$t: test $(ls -l $F | cut -c1-10) blob $(git hash-object $F | cut -c1-10)," \
	    "registered x$(grep -c "'zfs_rewrite_physical_raw_send'" tests/runfiles/common.run)" \
	    "x$(grep -c 'functional/cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send\.ksh' tests/zfs-tests/tests/Makefile.am);" \
	    "dmu_send.c: raw encrypted condition x$(grep -c 'rawok && to_os->os_encrypted' module/zfs/dmu_send.c)," \
	    "unconditional TRAVERSE_LOGICAL x$(grep -c 'st_arg->flags | TRAVERSE_LOGICAL' module/zfs/dmu_send.c);" \
	    "default_setup's create: $(grep -c 'log_must zpool create -f \$TESTPOOL \$disklist$' tests/zfs-tests/include/libtest.shlib)"
done
} | tee $L/copy.txt
```

Expected:

```
M: tests copied
 3 files changed, 118 insertions(+), 1 deletion(-)
ok: M: changes outside tests/
ok: M: tests/ as in the head
ok: M: differs from the head in
ok: B: tracked changes
ok: the 16 tests in both trees
M: test -rwxr-xr-x blob d6cb3dda29, registered x1 x1; dmu_send.c: raw encrypted condition x0, unconditional TRAVERSE_LOGICAL x1; default_setup's create: 1
B: test -rwxr-xr-x blob d6cb3dda29, registered x1 x1; dmu_send.c: raw encrypted condition x1, unconditional TRAVERSE_LOGICAL x0; default_setup's create: 1
```

(M carries the head's tests byte for byte and none of its code; the last
figure is the `zpool create -f $TESTPOOL $disklist` line of
`default_setup_noexit`, with no `-d` or `-o`.) Anything else means stop.

## STEP 2: builds, checks, and the hand-over to `zts` (all git work, part two)

Build B, then M (about 30 minutes together; see the rule about long
commands):

```sh
. /var/log/zts-rps-1028/env.sh
build_tree B && build_tree M
```

Expected: `build B rc=0 wall=<s>s`, then `build M rc=0 wall=<s>s`. A failed
build prints its `error:` lines and `build <tree> failed: stop`: stop.

Then check what was built:

```sh
. /var/log/zts-rps-1028/env.sh
{
for t in B M; do
	cd $W/$t
	same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
	same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h)" 3
	same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko | cut -d' ' -f1)" "$(uname -r)"
	same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb zstream \
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
echo "M srcversion $(cat $L/srcv-M); every earlier build of this master module code on this VM: 8852710DB871A7587B9E722"
} 2>&1 | tee $L/step2.txt
```

Expected: 9 `ok:` lines (4 per tree, then the srcversions), for each tree
`compiler warnings: 0` and about 83 other `warning:` lines, a last build line
of a finished build (typically `make[1]: Leaving directory
'/var/tmp/zts-rps-1028/<B or M>'`), and an M srcversion of
`8852710DB871A7587B9E722`. Any `MISMATCH` means stop. A compiler warning
count other than 0 is not a stop: its first lines are printed, quote them in
the report. Another M srcversion is not a stop either, provided every check
here and in STEP 1 passed (STEP 1 showed that M's module code is that
master's): report it.

Then point both trees' results at `$L/results`, record the trees, store the
two srcversions for the guards, and hand the work directory to `zts`. This is
the last git command of the task:

```sh
. /var/log/zts-rps-1028/env.sh
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
} | tee $L/step2b.txt
if ! grep -q '^MISMATCH' $L/sha-checks.txt $L/copy.txt $L/step2.txt $L/step2b.txt &&
    [ -s $L/srcv-B ] && [ -s $L/srcv-M ] && ! cmp -s $L/srcv-B $L/srcv-M; then
	printf 'SV_B=%s\nSV_M=%s\n' "$(cat $L/srcv-B)" "$(cat $L/srcv-M)" >> $L/env.sh &&
	    echo "srcversions recorded"
else
	echo "CHECKS FAILED: stop"
fi
tail -n 2 $L/env.sh
mkdir $W/files $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
chown -R zts $W $L/results; echo "chown rc=$?"                        # 0
stat -c '%U %a %n' $W/files $L/results
```

Expected: 7 `ok:` lines (the `test-runner.py.in` change is the results
directory, and nothing else may differ), `srcversions recorded`, the two lines
`SV_B=<B's>` and `SV_M=<M's>`, `mkdir rc=0`, `chown rc=0`, then
`zts 1777 /var/tmp/zts-rps-1028/files` and
`zts 755 /var/log/zts-rps-1028/results`. On `CHECKS FAILED`, stop. The ZTS
`-d` directory must be mode 1777: the tests write there both as root and as
`zts`. `env.sh` and the scripts stay root's, in `$L`.

## STEP 3: the scripts

`run.sh` runs one ZTS run on the loaded tree, as `zts`, behind the srcversion
guard, then writes its digest, reads its kernel messages from the journal and
checks its leftovers:

```sh
cat > /var/log/zts-rps-1028/run.sh <<'EOS'
#!/bin/sh
# usage:
#   run.sh <B|M> <label> test <path>        : one test (-t); <path> is under
#                                             tests/zfs-tests/tests/functional,
#                                             without .ksh
#   run.sh <B|M> <label> group zfs_rewrite  : the zfs_rewrite group (-T)
# One zfs-tests.sh run on the loaded tree, then its digest, its kernel
# messages and its leftovers.
. /var/log/zts-rps-1028/env.sh
t=$1; lbl=$2; mode=$3; what=$4
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> test <path> | group zfs_rewrite"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
case $mode in
group)
	if [ "$what" != zfs_rewrite ]; then
		echo "the only group is zfs_rewrite"; exit 2
	fi
	sel="-T zfs_rewrite"; outer=3600 ;;
test)
	case $what in
	''|/*|*..*|*[!A-Za-z0-9_/-]*) echo "bad test path: '$what'"; exit 2 ;;
	esac
	if [ ! -x "$W/$t/tests/zfs-tests/tests/functional/$what.ksh" ]; then
		echo "no such test in $t: functional/$what.ksh"; exit 2
	fi
	sel="-t tests/functional/$what.ksh"; outer=1800 ;;
*) echo "mode is 'test' or 'group'"; exit 2 ;;
esac
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi
G=$L/guards.txt
RT=zfs_rewrite_physical_raw_send

# The result file of test <name> in directory <dir> (the last component of
# its directory): its "merged" file, else its "stdout" (a test that writes
# nothing on stderr has no "merged" file).
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

# The lines of a result file before the failure callbacks and the cleanup.
body() {   # body <file>
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ { exit } { print }' "$1"
}
endline() {   # endline <file>: the last line before the callbacks and cleanup
	awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
		print "  end line: " prev; exit }
	    NF { prev = $0 }' "$1"
}

# How far zfs_rewrite_physical_raw_send got, from its result file: the
# counts of its SUCCESS lines, its WRITE record notes, the lines of a failed
# check, and the line before its cleanup.
rps_digest() {   # rps_digest <file>
	echo "  $RT: SUCCESS lines:" \
	    "zfs rewrite -P x$(grep -c 'SUCCESS: zfs rewrite -P ' "$1")," \
	    "zfs recv x$(grep -c 'SUCCESS: eval zfs recv ' "$1")," \
	    "zfs load-key x$(grep -c 'SUCCESS: zfs load-key ' "$1")," \
	    "cmp x$(grep -c 'SUCCESS: cmp ' "$1")"
	body "$1" | grep 'WRITE records for object' | sed 's/^.*NOTE: /  note: /'
	body "$1" | grep -E 'ERROR: |authentication ereport|cannot ' | head -n 12 |
	    sed 's/^/  line: /'
	endline "$1"
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
mnts=$(grep -E ' zfs |testpool|rpmnt' /proc/mounts)
roots=$(ls -d /testpool /testpool1 /testpool2 /testpool3 /pool9 /pool12 2>/dev/null)
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
# "<dir> <name>" of every FAIL or KILLED result.
grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-notpassed.txt" |
    sed -E 's|^.*/([^/ ]+)/([^/ ]+) [(]run as .*$|\1 \2|; s|:Linux | |; s|[.]ksh$||' \
    > "$L/$lbl-failed.txt"
{
echo "$lbl: tree $t, $sel, $(wc -l < "$L/$lbl-tests.txt") results:" \
    "${cnt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
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
if [ "$mode" = group ] || [ "${what%/*}" = cli_root/zfs_rewrite ]; then
	o=""
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" zfs_rewrite setup)
	if [ -n "$o" ]; then
		echo "  setup: $(grep -m 1 'SUCCESS: zpool create' "$o" || echo 'no zpool create line')"
	else
		echo "  setup: no stdout or merged file found"
	fi
fi
if [ "$mode" = group ] || [ "$what" = cli_root/zfs_rewrite/$RT ]; then
	o=""
	[ -n "$rd" ] && [ -d "$rd" ] && o=$(outfile "$rd" zfs_rewrite $RT)
	if [ -n "$o" ]; then
		cp "$o" "$L/$lbl-rps-out.txt"
		rps_digest "$o"
	else
		echo "  $RT: no stdout or merged file found"
	fi
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
lm=$(grep -E ' zfs |testpool|rpmnt' /proc/mounts)
lr=$(ls -d /testpool /testpool1 /testpool2 /testpool3 /pool9 /pool12 2>/dev/null)
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

`list.sh` runs the 16 tests of `LIST` on the loaded tree, one `run.sh` each,
in order, and stops at the first that does not exit 0:

```sh
cat > /var/log/zts-rps-1028/list.sh <<'EOS'
#!/bin/sh
# usage: list.sh <B|M>: the 16 send and receive tests on the loaded tree,
# labels b01-b16 or m01-m16.
. /var/log/zts-rps-1028/env.sh
case $1 in
B) p=b ;;
M) p=m ;;
*) echo "usage: list.sh <B|M>"; exit 2 ;;
esac
for e in $LIST; do
	n=${e%%:*}
	sh "$L/run.sh" "$1" "$p$n" test "${e#*:}"; rc=$?
	echo "run.sh $p$n rc=$rc"
	if [ $rc -ne 0 ]; then
		echo "list $1 stopped at $p$n"; exit $rc
	fi
done
echo "list $1 done:"
for e in $LIST; do
	head -n 1 "$L/$p${e%%:*}-notes.txt" | sed -E 's/; log dir .*$//; s/^/  /'
done
exit 0
EOS
```

`rp.sh` reproduces the bug by hand on the loaded tree (see "The bug by hand"):

```sh
cat > /var/log/zts-rps-1028/rp.sh <<'EOS'
#!/bin/sh
# usage: rp.sh <B|M>   (the tree named must be the loaded one)
# The bug's reproduction, with the loaded tree's own zpool, zfs and zstream:
# pool rp on one sparse 1 GiB file vdev in $W/rp, -o cachefile=none and
# default features, mounted under $W/rpmnt; rp/src encrypted with a raw key
# file, holding a 16 MiB file; a raw full send to rp/dst, zfs rewrite -P, a
# raw incremental received on rp/dst, its key loaded, mounted, read back.
. /var/log/zts-rps-1028/env.sh
t=$1
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: rp.sh <B|M>"; exit 2 ;;
esac
ZP=$W/$t/zpool; ZF=$W/$t/zfs; ZS=$W/$t/zstream
R=$W/rp; MP=$W/rpmnt
start=$(date +%s)
zp() { nice -n 10 ionice -c3 "$ZP" "$@"; }
zf() { nice -n 10 ionice -c3 "$ZF" "$@"; }

# The end of the script: its kernel messages, and nothing left imported or
# mounted.
finish() {   # finish <rc>
	k=$L/kern-rp-$t.txt
	journalctl -k --since "@$start" --no-pager -q > "$k" 2>&1
	kn=$(grep -iE "$KPAT" "$k" | grep -vcE "$KIGN")
	kw=$(grep -E "$KWARN" "$k" | grep -vcE "$KIGN")
	lp=$(zp list -H -o name 2>&1)
	lm=$(grep -E ' zfs |rpmnt' /proc/mounts)
	lf=$(ls -d "$R" "$MP" 2>/dev/null | xargs)
	echo "rp $t: rc=$1 kernel_matches=$kn kernel_warnings=$kw" \
	    "pools_imported=[$lp] mounts=[$lm] left=[$lf]"
	if [ "$kn" -gt 0 ] || [ "$kw" -gt 0 ]; then
		grep -iE "$KPAT|$KWARN" "$k" | grep -vE "$KIGN" | head -n 20 | sed 's/^/  kernel: /'
	fi
	if [ "$kn" -gt 0 ] || [ -n "$lp$lm" ]; then
		echo "STOP after rp $t: kernel message, or a pool or mount left"; exit 3
	fi
	exit "$1"
}
# "<WRITE records for object obj> <WRITE records in all>" of a stream.
writes() {   # writes <stream> <obj>
	"$ZS" dump -v < "$1" 2>/dev/null |
	    awk -v o="$2" '$1 == "WRITE" { n++; if ($4 == o) f++ } END { print f + 0, n + 0 }'
}

cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ] || [ "$cur" != "$sv" ]; then
	echo "GUARD FAILED: loaded srcversion '$cur', $t's '$sv': nothing run"; exit 1
fi
if [ -e "$R" ] || [ -e "$MP" ] || [ -n "$(zp list -H -o name 2>&1)" ]; then
	echo "GUARD FAILED: $R or $MP exists, or a pool is imported: nothing run"; exit 1
fi
echo "=== rp ($t), $(date -u +%T)"
{ mkdir "$R" && truncate -s 1g "$R/vdev0" &&
    (umask 077 && head -c 32 /dev/urandom > "$R/key"); } ||
    { echo "rp: cannot prepare $R"; finish 1; }
zp create -f -o cachefile=none -O mountpoint="$MP" rp "$R/vdev0" ||
    { echo "rp: CREATE FAILED"; finish 1; }
echo "rp: created; feature@encryption $(zp get -H -o value feature@encryption rp)," \
    "feature@physical_rewrite $(zp get -H -o value feature@physical_rewrite rp)"
zf create -o encryption=aes-256-gcm -o keyformat=raw -o keylocation="file://$R/key" rp/src ||
    { echo "rp: ENCRYPTED CREATE FAILED"; finish 1; }
{ dd if=/dev/urandom of="$MP/src/f" bs=1M count=16 conv=fsync status=none &&
    zp sync rp; } || { echo "rp: WRITE FAILED"; finish 1; }
h0=$(sha256sum < "$MP/src/f" | cut -d' ' -f1)
obj=$(stat -c %i "$MP/src/f")
echo "rp/src: f of $(stat -c %s "$MP/src/f") bytes, object $obj," \
    "recordsize $(zf get -H -o value recordsize rp/src), encryption $(zf get -H -o value encryption rp/src)"
{ zf snapshot rp/src@a && zf send -w rp/src@a > "$R/full.zs"; } ||
    { echo "rp: FULL SEND FAILED"; finish 1; }
zf receive rp/dst < "$R/full.zs" || { echo "rp: FULL RECEIVE FAILED"; finish 1; }
echo "raw full send: $(stat -c %s "$R/full.zs") bytes, WRITE records (f, all):" \
    "$(writes "$R/full.zs" "$obj"); rp/dst keystatus $(zf get -H -o value keystatus rp/dst)"
zf rewrite -P "$MP/src/f" || { echo "rp: REWRITE FAILED"; finish 1; }
zp sync rp
same "f unchanged by zfs rewrite -P" "$(sha256sum < "$MP/src/f" | cut -d' ' -f1)" "$h0"
echo "after the rewrite: feature@physical_rewrite $(zp get -H -o value feature@physical_rewrite rp)"
{ zf snapshot rp/src@b && zf send -w -i @a rp/src@b > "$R/incr.zs" &&
    zf send -i @a rp/src@b > "$R/incr-plain.zs"; } ||
    { echo "rp: INCREMENTAL SEND FAILED"; finish 1; }
echo "raw incremental: $(stat -c %s "$R/incr.zs") bytes, WRITE records (f, all):" \
    "$(writes "$R/incr.zs" "$obj"); non-raw incremental: $(stat -c %s "$R/incr-plain.zs")" \
    "bytes, WRITE records (f, all): $(writes "$R/incr-plain.zs" "$obj")"
zp events -c > /dev/null; ec=$?
ro=$(zf receive rp/dst < "$R/incr.zs" 2>&1); rr=$?
echo "events cleared rc=$ec; raw incremental received into rp/dst rc=$rr [$ro]"
lo=$(zf load-key -L "file://$R/key" rp/dst 2>&1); lk=$?
mo=$(zf mount rp/dst 2>&1); mr=$?
mt=$(zf get -H -o value mounted rp/dst 2>&1)
echo "load-key rp/dst rc=$lk [$lo]; mount rp/dst rc=$mr [$mo]; mounted $mt"
if [ "$mt" = yes ]; then
	so=$(sha256sum "$MP/dst/f" 2>&1); sr=$?
	if [ $sr -ne 0 ]; then
		rd="read error: $so"
	elif [ "${so%% *}" = "$h0" ]; then
		rd=identical
	else
		rd=DIFFERS
	fi
else
	rd="not read: rp/dst is not mounted"
fi
echo "read back rp/dst/f: $rd"
ev=$(zp events -H 2>&1)
zp events -v > "$L/rp-$t-events.txt" 2>&1
na=$(echo "$ev" | grep -c 'ereport\.fs\.zfs\.authentication')
cl=$(echo "$ev" | grep -oE '(ereport|resource|sysevent)\.fs\.zfs\.[a-z0-9_.]+' | sort | uniq -c |
    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }')
echo "events since the receive: ereport.fs.zfs.authentication x$na; all: [${cl:-none}]"
st=$(zp status -v rp 2>&1)
echo "$st" > "$L/rp-$t-status.txt"
er=$(echo "$st" | sed -n 's/^errors: //p')
echo "rp status: errors: $er"
echo "$st" | sed -n '/^errors:/,$p' | sed 1d | grep . | head -n 5 | sed 's/^/  /'
if [ "$rr" = 0 ] && [ "$lk" = 0 ] && [ "$mr" = 0 ] && [ "$rd" = identical ] &&
    [ "$na" = 0 ] && [ "$er" = "No known data errors" ]; then
	echo "rp $t: replica READABLE"
else
	echo "rp $t: replica BROKEN or refused"
fi
zp destroy rp; echo "  destroyed rp rc=$?"
rm -r "${R:?}" && echo "  removed $R"
rmdir "$MP/src" "$MP/dst" 2>/dev/null
rmdir "$MP" 2>/dev/null
finish 0
EOS
```

`rp.sh` exits 0 when it ran to its end, whatever the replica did; 1 when one
of its preparing commands failed (`CREATE FAILED`, `REWRITE FAILED`, ...:
stop); 2 on a bad argument (stop); 3 after `STOP after rp ...` (a kernel
fault, or a pool or mount left: stop, and skip the unload of STEP 6).

`cmp.sh` compares B's runs with M's, result by result (`bn` with `mn`, `bg`
with `mg`, `b01` with `m01`, ...):

```sh
cat > /var/log/zts-rps-1028/cmp.sh <<'EOS'
#!/bin/sh
# usage: cmp.sh: every result of B's runs against M's.
. /var/log/zts-rps-1028/env.sh
# "<key>:<test path> <result>" for each result line of run <label>.
rows() {   # rows <label> <key>
	sed -E 's|^.*/tests/functional/||; s|:Linux/|/|;
	    s| [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\]$| \1|; s|[.]ksh | |' \
	    "$L/$1-tests.txt" | sed "s|^|$2:|"
}
: > "$L/cmp-b.txt"
: > "$L/cmp-m.txt"
for k in n g $(for e in $LIST; do echo "${e%%:*}"; done); do
	for p in b m; do
		if [ -s "$L/$p$k-tests.txt" ]; then
			rows "$p$k" "$k" >> "$L/cmp-$p.txt"
		else
			echo "$k:no-results none" >> "$L/cmp-$p.txt"
		fi
	done
done
LC_ALL=C sort -o "$L/cmp-b.txt" "$L/cmp-b.txt"
LC_ALL=C sort -o "$L/cmp-m.txt" "$L/cmp-m.txt"
LC_ALL=C join -a 1 -a 2 -e none -o 0,1.2,2.2 "$L/cmp-b.txt" "$L/cmp-m.txt" |
    awk '{ printf "%-66s B %-7s M %-7s %s\n", $1, $2, $3, ($2 == $3 ? "same" : "DIFFERS") }' \
    > "$L/compare.txt"
echo "tests: $(wc -l < "$L/compare.txt"), the same on both: $(grep -c ' same$' "$L/compare.txt")," \
    "different: $(grep -c ' DIFFERS$' "$L/compare.txt")" >> "$L/compare.txt"
cat "$L/compare.txt"
EOS
for f in run.sh list.sh rp.sh cmp.sh; do
	bash -n /var/log/zts-rps-1028/$f; echo "$f bash syntax rc=$?"   # 0
	sh -n /var/log/zts-rps-1028/$f; echo "$f sh syntax rc=$?"       # 0
done
```

Expected: eight `syntax rc=0` lines.

## STEP 4: B: the new test, the group, the 16 tests, the bug by hand

Load B. Nothing is loaded yet (STEP 0), so the reload loads:

```sh
. /var/log/zts-rps-1028/env.sh
{
echo "loaded before: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # []
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -r; echo "load B rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"             # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ] && { $W/B/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
} 2>&1 | tee $L/load-B.txt
```

Expected: `loaded before: []`, `load B rc=0`, the same srcversion twice,
`zpool list rc=0` with no pool name, and

```
zfs_disable_ivset_guid_check=0 zfs_send_corrupt_data=0 zfs_send_unmodified_spill_blocks=1 send_holes_without_birth_time=1 zfs_recv_best_effort_corrective=0
```

(the module's defaults; other values set by this VM's configuration are fine,
since every run is compared with what the load found). If the load fails, the
srcversion is not B's or a pool appears, stop.

The new test alone, then the group. Go on to the group only if `bn` printed
`run.sh rc=0`:

```sh
L=/var/log/zts-rps-1028
sh $L/run.sh B bn test cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send > $L/bn-console.txt 2>&1; echo "run.sh rc=$?" >> $L/bn-console.txt; cat $L/bn-console.txt
```

Expected (after its three `guards.txt` lines, `before bn` with every list empty,
then `results=PASS 3`, `zts_marks=3`, `kernel_matches=0`, `kernel_warnings=0`,
and every leftover empty with `tunables_same=yes`):

```
bn: tree B, -t tests/functional/cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send.ksh, 3 results: PASS 3; zfs-tests.sh rc=0, wall <s>s, log dir /var/log/zts-rps-1028/results/<time>
<a Missing util(s) line, or "no Missing util(s) line">
Test: /var/tmp/zts-rps-1028/B/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/setup (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-rps-1028/B/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send.ksh (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-rps-1028/B/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/cleanup (run as root) [<mm:ss>] [PASS]
  setup: SUCCESS: zpool create -f testpool loop<N>
  zfs_rewrite_physical_raw_send: SUCCESS lines: zfs rewrite -P x1, zfs recv x2, zfs load-key x1, cmp x1
  note: zfs send -w -i @a testpool/testfs1@b: 32 WRITE records for object <o>
  note: zfs send -i @a testpool/testfs1@b: 0 WRITE records for object <o>
  note: zfs send -w -i @a testpool/testfs3@b: 0 WRITE records for object <p>
  end line: SUCCESS: cmp /testpool/testfs1/file /testpool/testfs2/file
run.sh rc=0
```

with no `line:` entry: no failed check, no authentication ereport. The
`setup:` line must name no `-d` and no `-o`.

```sh
L=/var/log/zts-rps-1028
sh $L/run.sh B bg group zfs_rewrite > $L/bg-console.txt 2>&1; echo "run.sh rc=$?" >> $L/bg-console.txt; cat $L/bg-console.txt
```

Expected: `results=PASS 8` (`setup`, the six tests, `cleanup`), `zts_marks=8`,
the Results Summary with `PASS 8`, the same `setup:` line, the
`zfs_rewrite_physical_raw_send` digest as in `bn` (three notes, `32` then `0`
and `0`, the same end line), `kernel_matches=0`, every leftover empty,
`run.sh rc=0`. A test that fails is listed with its `ERROR:` lines and end
line (`<name> (failed):`); the comparison of STEP 5 tells whether master
fails it too.

Then the 16 send and receive tests (30 to 50 minutes; see the rule about long
commands):

```sh
L=/var/log/zts-rps-1028
sh $L/list.sh B > $L/list-B.txt 2>&1; echo "list.sh rc=$?" >> $L/list-B.txt; tail -n 20 $L/list-B.txt
```

Expected: 16 `run.sh b<nn> rc=0` lines in `list-B.txt`, then `list B done:`,
16 summary lines `b<nn>: tree B, -t tests/functional/<path>.ksh, 3 results:
PASS 3; zfs-tests.sh rc=0, wall <s>s`, and `list.sh rc=0`. Each run's
`guards.txt` lines must show `zts_marks=3`, `kernel_matches=0` and every
leftover empty. A result other than `PASS 3` is recorded, not a stop: the
comparison decides. A `list B stopped at b<nn>` line is a stop (the run's own
lines in `list-B.txt` say why).

Then the bug by hand on B:

```sh
L=/var/log/zts-rps-1028
sh $L/rp.sh B > $L/rp-B.txt 2>&1; echo "rp.sh rc=$?" >> $L/rp-B.txt; cat $L/rp-B.txt
```

Expected:

```
=== rp (B), <time>
rp: created; feature@encryption enabled, feature@physical_rewrite enabled
rp/src: f of 16777216 bytes, object <o>, recordsize 128K, encryption aes-256-gcm
raw full send: <bytes> bytes, WRITE records (f, all): 128 <n>; rp/dst keystatus unavailable
ok: f unchanged by zfs rewrite -P
after the rewrite: feature@physical_rewrite active
raw incremental: <bytes> bytes, WRITE records (f, all): 128 <n>; non-raw incremental: <bytes> bytes, WRITE records (f, all): 0 <n>
events cleared rc=0; raw incremental received into rp/dst rc=0 []
load-key rp/dst rc=0 []; mount rp/dst rc=0 []; mounted yes
read back rp/dst/f: identical
events since the receive: ereport.fs.zfs.authentication x0; all: [<other classes, or none>]
rp status: errors: No known data errors
rp B: replica READABLE
  destroyed rp rc=0
  removed /var/tmp/zts-rps-1028/rp
rp B: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] mounts=[] left=[]
rp.sh rc=0
```

## STEP 5: M: the same, the comparison, then the bug by hand last

Swap the modules only while B's is loaded with no pool, no mount and no
`zed`; `zfs.sh -r` from M's tree unloads B's stack and loads M's:

```sh
. /var/log/zts-rps-1028/env.sh
{
cur=$(cat /sys/module/zfs/srcversion 2>&1)
p=""; [ "$cur" = "$SV_B" ] && p=$($W/B/zpool list -H -o name 2>&1)
m=$(grep -E ' zfs |testpool|rpmnt' /proc/mounts)
z=$(pgrep -x zed)
echo "before the swap: B's module loaded: $([ "$cur" = "$SV_B" ] && echo yes || echo "no ($cur)"), pools [$p], mounts [$m], zed [$z]"
if [ "$cur" = "$SV_B" ] && [ -z "$p$m$z" ]; then
	(cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh -r); echo "reload with M rc=$?"
else
	echo "not swapped: stop"
fi
echo "loaded $(cat /sys/module/zfs/srcversion 2>&1), SV_M $SV_M"
if [ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ]; then
	$W/M/zpool list -H -o name; echo "zpool list rc=$?"
	tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo
	same "M's tunables as B's" "$(cat $L/tun-M.txt)" "$(cat $L/tun-B.txt)"
fi
} 2>&1 | tee $L/swap.txt
```

Expected: `before the swap: B's module loaded: yes, pools [], mounts [], zed
[]`, `reload with M rc=0`, the same srcversion twice (M's), `zpool list rc=0`
with no pool name, the tunables line as B's, and `ok: M's tunables as B's`.
Anything else means stop.

Then the same runs on M, in the same order:

```sh
L=/var/log/zts-rps-1028
sh $L/run.sh M mn test cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send > $L/mn-console.txt 2>&1; echo "run.sh rc=$?" >> $L/mn-console.txt; cat $L/mn-console.txt
```

Expected (`results=FAIL 1, PASS 2`, `zts_marks=3`, `kernel_matches=0`, every
leftover empty, `tunables_same=yes`):

```
mn: tree M, -t tests/functional/cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send.ksh, 3 results: FAIL 1, PASS 2; zfs-tests.sh rc=1, wall <s>s, log dir /var/log/zts-rps-1028/results/<time>
<a Missing util(s) line, or "no Missing util(s) line">
Test: /var/tmp/zts-rps-1028/M/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/setup (run as root) [<mm:ss>] [PASS]
Test: /var/tmp/zts-rps-1028/M/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send.ksh (run as root) [<mm:ss>] [FAIL]
Test: /var/tmp/zts-rps-1028/M/tests/zfs-tests/tests/functional/cli_root/zfs_rewrite/cleanup (run as root) [<mm:ss>] [PASS]
zfs_rewrite_physical_raw_send (failed):
  ERROR: test 0 -eq 32 exited 1
  end line: <the ERROR line above, or the note before it: the runner may read stdout and stderr in either order>
  setup: SUCCESS: zpool create -f testpool loop<N>
  zfs_rewrite_physical_raw_send: SUCCESS lines: zfs rewrite -P x1, zfs recv x1, zfs load-key x0, cmp x0
  note: zfs send -w -i @a testpool/testfs1@b: 0 WRITE records for object <o>
  line: ERROR: test 0 -eq 32 exited 1
  end line: <as above>
run.sh rc=0
```

That is the test failing on master at its first check, for the reason the
commit gives: the raw incremental leaves out the 32 rewritten blocks
(`zfs rewrite -P x1`: the rewrite itself, and so `physical_rewrite`, works on
master; `zfs recv x1`: the incremental was never received).

```sh
L=/var/log/zts-rps-1028
sh $L/run.sh M mg group zfs_rewrite > $L/mg-console.txt 2>&1; echo "run.sh rc=$?" >> $L/mg-console.txt; cat $L/mg-console.txt
sh $L/list.sh M > $L/list-M.txt 2>&1; echo "list.sh rc=$?" >> $L/list-M.txt; tail -n 20 $L/list-M.txt
```

Run the second line only if the first ends with `run.sh rc=0`. Expected for
`mg`: `results=FAIL 1, PASS 7`, `zfs-tests.sh rc=1`, the "Tests with results
other than PASS that are unexpected" section naming
`zfs_rewrite_physical_raw_send`, its `(failed):` lines and digest as in `mn`,
`kernel_matches=0`, every leftover empty, `run.sh rc=0`. For `list.sh M`: as
for B, with `m<nn>` labels, and `list.sh rc=0`.

Then B against M, result by result:

```sh
sh /var/log/zts-rps-1028/cmp.sh
```

Expected: 59 rows (`n`: 3, `g`: 8, `01` to `16`: 3 each), every one
`B PASS    M PASS    same` but these two:

```
g:cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send               B PASS    M FAIL    DIFFERS
n:cli_root/zfs_rewrite/zfs_rewrite_physical_raw_send               B PASS    M FAIL    DIFFERS
```

then `tests: 59, the same on both: 57, different: 2`. A row that is not PASS
on either tree but the same on both is recorded (master's or this VM's, not
the change's); any other `DIFFERS` row is a finding, see "What each result
means".

Last, the bug by hand on M. This is the only step of the task that builds a
replica the analysis says is broken; it runs last on purpose:

```sh
L=/var/log/zts-rps-1028
sh $L/rp.sh M > $L/rp-M.txt 2>&1; echo "rp.sh rc=$?" >> $L/rp-M.txt; cat $L/rp-M.txt
```

What the analysis predicts (record whatever is printed instead):

```
=== rp (M), <time>
rp: created; feature@encryption enabled, feature@physical_rewrite enabled
rp/src: f of 16777216 bytes, object <o>, recordsize 128K, encryption aes-256-gcm
raw full send: <bytes> bytes, WRITE records (f, all): 128 <n>; rp/dst keystatus unavailable
ok: f unchanged by zfs rewrite -P
after the rewrite: feature@physical_rewrite active
raw incremental: <bytes> bytes, WRITE records (f, all): 0 <n>; non-raw incremental: <bytes> bytes, WRITE records (f, all): 0 <n>
events cleared rc=0; raw incremental received into rp/dst rc=0 []
load-key rp/dst rc=0 []; mount rp/dst rc=1 [cannot mount 'rp/dst': Input/output error]; mounted no
read back rp/dst/f: not read: rp/dst is not mounted
events since the receive: ereport.fs.zfs.authentication x<1 or more>; all: [...]
rp status: errors: Permanent errors have been detected, but none of the affected
  <the next lines of zpool status -v>
rp M: replica BROKEN or refused
  destroyed rp rc=0
  removed /var/tmp/zts-rps-1028/rp
rp M: rc=0 kernel_matches=0 kernel_warnings=0 pools_imported=[] mounts=[] left=[]
rp.sh rc=0
```

Equally consistent with the analysis: the mount succeeds and the read of `f`
fails (`read error: ...`), or the file differs, with authentication ereports
or permanent errors. Quote `rp-M.txt` whole either way. If the kernel logs a
fault here, `rp.sh` says `STOP after rp M`: go to STEP 6 and skip its unload.

## STEP 6: unload, final checks, report, cleanup

Unload only when no run or step stopped with a leftover pool, device, mount or
process, and none had a kernel fault. Otherwise skip the unload, keep
everything, and say why in the report:

```sh
. /var/log/zts-rps-1028/env.sh
{
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ "$cur" = "$SV_B" ]; then t=B; elif [ "$cur" = "$SV_M" ]; then t=M; else t=""; fi
if [ -n "$t" ]; then
	p=$($W/$t/zpool list -H -o name 2>&1)
	m=$(grep -E ' zfs |testpool|rpmnt' /proc/mounts)
	echo "pools: [$p], mounts: [$m], rp: [$(ls -d $W/rp $W/rpmnt 2>/dev/null | xargs)]"   # pools: [], mounts: [], rp: []
	if [ -z "$p$m" ]; then
		(cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # unload M rc=0
	else
		echo "unload skipped: a pool or a mount is left"
	fi
else
	echo "no module of this task is loaded (srcversion '$cur')"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"   # lsmod: []
journalctl -k --since "@$(cat $L/start.txt)" --no-pager -q > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KPAT" $L/kern-all.txt | grep -vcE "$KIGN") matches," \
    "$(grep -E "$KWARN" $L/kern-all.txt | grep -vcE "$KIGN") warnings," \
    "$(grep -c 'ZTS run' $L/kern-all.txt) ZTS marks"   # 0 matches, 0 warnings, 118 marks (59 per tree)
grep -iE "$KPAT|$KWARN" $L/kern-all.txt | grep -vE "$KIGN" | head -n 40
echo "kernel, this boot: $(journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN")"   # the STEP 0 baseline
ls -l /etc/zfs/zpool.cache 2>&1                                    # as in STEP 0, or a 0-byte file
ls -d /testpool* /pool9 /pool12 $W/rp $W/rpmnt 2>&1; losetup -a    # No such file or directory five times, then nothing
echo "files: [$(ls -A $W/files)]"                                  # files: []
df -h /var/tmp
} 2>&1 | tee $L/end.txt
```

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-rps-1028/env.sh
{
echo "VM 106: kernel proof of zfs-rewrite-physical-raw-send 86670aff43 against master 5ebd13aa9 (r/VM-TESTS-106-rps.md)"
for s in sha-checks worktrees copy build step2 step2b load-B; do
	echo; echo "== $s.txt"; cat $L/$s.txt 2>&1
done
echo; echo "== env.sh from SV_B"; sed -n '/^SV_B=/,$p' $L/env.sh
for l in bn bg; do echo; echo "== $l-notes.txt"; cat $L/$l-notes.txt 2>&1; done
echo; echo "== list-B.txt (summary)"; sed -n '/^list B /,$p' $L/list-B.txt 2>&1
echo; echo "== rp-B.txt"; cat $L/rp-B.txt 2>&1
echo; echo "== swap.txt"; cat $L/swap.txt 2>&1
for l in mn mg; do echo; echo "== $l-notes.txt"; cat $L/$l-notes.txt 2>&1; done
echo; echo "== list-M.txt (summary)"; sed -n '/^list M /,$p' $L/list-M.txt 2>&1
echo; echo "== not passed in the 16 runs"
for e in $LIST; do
	for p in b m; do
		[ -s $L/$p${e%%:*}-notpassed.txt ] && { echo "$p${e%%:*}:"; cat $L/$p${e%%:*}-notes.txt; }
	done
done
echo; echo "== compare.txt"; cat $L/compare.txt 2>&1
echo; echo "== rp-M.txt"; cat $L/rp-M.txt 2>&1
echo; echo "== guards.txt"; cat $L/guards.txt 2>&1
echo; echo "== end.txt"; cat $L/end.txt 2>&1
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Then add by hand, at the top of `REPORT.txt` after its first line, a
`== STEP 0` section (short: anything found, `uname -r`, `nproc`, the CPU
model and flags line, the free space, the `zpool.cache`, `60-zvol.rules`,
`xxh128sum` and doc checkout lines, the directories of earlier tasks, and the
kernel baseline), and at its end a `== verdict` section (the Expected block
below, then for each of its items whether it matched, and which case of
"What each result means" applies) and a `== deviations` section (every
deviation from these instructions, with its reason; `none` if there was
none). Then print it with `cat /var/log/zts-rps-1028/REPORT.txt`.

Then, as the very last command, and only if the unload block printed
`unload <B or M> rc=0` and `lsmod: []`, remove this task's two directories
and the doc checkout (the doc checkout only if it is a clean clone of
`vm-tasks-1026` holding this document, nothing else):

```sh
cd /
D=/var/tmp/vm-task-106r
if [ -d $D ] && [ ! -L $D ] && [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
    [ "$(git -C $D rev-parse --abbrev-ref HEAD 2>/dev/null)" = vm-tasks-1026 ] &&
    case "$(git -C $D remote get-url origin 2>/dev/null)" in
    https://github.com/tigerblue77/zfs|https://github.com/tigerblue77/zfs.git) true ;;
    *) false ;; esac &&
    git -C $D ls-files --error-unmatch r/VM-TESTS-106-rps.md > /dev/null 2>&1 &&
    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
    ! grep -q " ${D}[ /]" /proc/mounts; then
	rm -rf /var/tmp/zts-rps-1028 /var/log/zts-rps-1028 $D; echo "rm rc=$? (doc checkout included)"
else
	rm -rf /var/tmp/zts-rps-1028 /var/log/zts-rps-1028; echo "rm rc=$? ($D kept: not a clean checkout of vm-tasks-1026)"
fi
ls -d /var/tmp/zts-rps-1028 /var/log/zts-rps-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, run nothing of this block: keep everything and
say so. If the permission system refuses this `rm -rf`, leave the directories
as they are and say so in the reply. Do not delete them any other way. Then
reply with the whole of `REPORT.txt` as printed, and the result of the `rm`:
nothing runs after the reply.

## Expected, as a block for REPORT.txt

```
STEP 0: free; uname -r recorded; at least 15G free on /var/tmp; the doc checkout a clone of
  vm-tasks-1026 at /var/tmp/vm-task-106r.
STEP 1-2: every check ok, no MISMATCH; builds B and M rc=0; compiler warnings 0, about 83
  objtool warning: lines per tree; ZFS_DEBUG lines 3; vermagic = uname -r; srcversions differ,
  M's 8852710DB871A7587B9E722 (master's module code, unchanged since a128b8a075).

Every ZTS run: run.sh rc=0; zts_marks = its result count; kernel_matches=0; kernel_warnings=0;
  every leftover empty; tunables_same=yes. "Missing util(s):" may list exportfs; it must not list
  zdb, zpool, zfs or zstream.

bn (B, the new test alone): PASS 3; setup "SUCCESS: zpool create -f testpool loop<N>" (no -d, no
  -o: physical_rewrite enabled); zfs rewrite -P x1, zfs recv x2, zfs load-key x1, cmp x1; notes
  32, 0, 0 WRITE records; end line the cmp; no "line:" entry.
bg (B, -T zfs_rewrite): PASS 8; the new test's digest as in bn.
b01-b16 (B): PASS 3 each.
rp B: physical_rewrite enabled, then active; raw incremental 128 WRITE records for f, non-raw 0;
  receive, load-key, mount rc=0; f identical; 0 authentication ereports; No known data errors;
  replica READABLE; rp.sh rc=0, kernel_matches=0.

mn (M, the new test alone): FAIL 1, PASS 2 (setup and cleanup pass); zfs rewrite -P x1,
  zfs recv x1, zfs load-key x0, cmp x0; one note, "0 WRITE records"; "ERROR: test 0 -eq 32
  exited 1": master's raw incremental leaves out the 32 rewritten blocks.
mg (M, -T zfs_rewrite): FAIL 1 (the new test, as in mn), PASS 7.
m01-m16 (M): PASS 3 each.
compare: 59 rows, 57 the same on both, the new test B PASS M FAIL in n and g.
rp M: raw incremental 0 WRITE records for f; then whatever happens, quoted whole. Predicted:
  receive and load-key rc=0, mount rc=1 "Input/output error" (or a read error), authentication
  ereports >= 1, permanent errors, replica BROKEN or refused; kernel_matches=0.

STEP 6: unload M rc=0; nothing in lsmod; whole task 0 matches, 0 warnings; the boot's count
  equal to the STEP 0 baseline; zpool.cache as in STEP 0 or 0 bytes; nothing left.
STOP only on: a failed check or guard, a leftover, a tunable left changed, an outer timeout,
  a failed preparing command in rp.sh, or a kernel VERIFY/ASSERT/panic/BUG/oops/hung task.
```

## What each result means

- **The proof holds** when every item of the Expected block matches. Then, in
  a kernel: master's raw incremental of an encrypted dataset leaves out the
  blocks `zfs rewrite -P` rewrote (the new test fails at that count, and the
  replica built that way does what `rp-M.txt` shows), while the head's carries
  every one of them and the replica reads back bit-exact with no
  authentication ereport; non-raw incrementals of the same dataset, and raw
  ones of an unencrypted dataset, still leave the rewritten blocks out on both
  trees, so `-P` keeps its benefit there; and the head changes nothing else
  the send and receive tests see: the 16 tests and the five other
  `zfs_rewrite` tests give the same results on both trees.
- **A finding against the head**:
  - `bn` or `bg` fails. The digest locates it: a note other than 32 for
    `testfs1 -w` (the head does not include the blocks), a note other than 0
    for `testfs1` without `-w` or `testfs3 -w` (the head includes them where
    it must not: `-P` lost its benefit for non-raw or unencrypted streams), no
    second `zfs recv` (the incremental was refused), no `cmp` or an
    `authentication ereport` line (the replica is still broken although the
    blocks were sent);
  - a row of `compare.txt` with B worse than M (B FAIL, M PASS) other than the
    new test: the changed traversal affects another send; its `ERROR:` lines
    are in the run's notes;
  - `rp B` not READABLE, or its raw incremental not 128: as for `bn`, with the
    reporter's own sequence;
  - any kernel `VERIFY`, `ASSERT`, `BUG:`, panic or hung task on B: stop; the
    line and the state it happened in are the result.
- **Against the analysis of the bug, not the head**: `mn` passing, or failing
  anywhere but at `test 0 -eq 32` (the M tree is not what STEP 1 checked, or
  master already sends the blocks: report the lines); `rp M` READABLE, or its
  raw incremental carrying the 128 blocks. Either is reported as a
  contradiction of the analysis, with the whole output.
- **Not the head's**: a row that is the same on both trees but not PASS
  (master's or this VM's, such as a missing `xxh128sum`: say so); a failure in
  a `setup` or `cleanup` on both trees: the environment. A kernel fault during
  `rp M`, on master's module, is master's: it is the most important result of
  that step, reported with its lines.
- **`WARNING:` lines in the kernel log**: quoted with their run or step. A
  finding only if B shows one that M does not.
- A result that differs between `bn` and the new test in `bg`, or between
  `mn` and the new test in `mg`, is reported as it is, run by run.

Estimated duration: about 3 hours (checks and sources 10 minutes, the two
builds about 30, on each tree the new test 2, the group 5, the 16 tests 30 to
50 and `rp.sh` 2, the swap 1, the comparison 1, report and cleanup 10).
