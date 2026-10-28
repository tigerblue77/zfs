# VM 107: scrub over pruned dedup blocks, on master, the PR head and its legacy follow-up

These instructions are for the session on VM 107 (Debian 13). They give
kernel ZTS evidence for the upstream pull request
https://github.com/openzfs/zfs/pull/19403, "Do not let scrub skip a dedup
block its DDT entry does not describe", and for the follow-up commit on top
of it. The session builds three fresh debug trees in its own directories,
runs the dedup tag group and four scrub and resilver tests on each, in the
order M, P, B, then times a scrub over a legacy DDT on P and B, cleans up,
and replies with `REPORT.txt`. This document is read from a checkout of the
branch `vm-tasks-1026` of https://github.com/tigerblue77/zfs in
`/var/tmp/vm-task-107t` (the doc checkout, `DOC` below); the last command of
STEP 8 removes it with the task's own directories.

| Tree | Path | Commit | Tests |
| --- | --- | --- | --- |
| M | `/var/tmp/zts-ddtl-1028/M` | `465d86a781` "ZTS: find zinject in zfs_failsafe when it is not installed": upstream master, P's parent, and the merge base of P with upstream master | master's, plus P's three test-side files (the new test, and its entries in `tests/runfiles/common.run` and `tests/zfs-tests/tests/Makefile.am`), checked out from P before the build |
| P | `/var/tmp/zts-ddtl-1028/P` | `43fda04502` "Do not let scrub skip a dedup block its DDT entry does not describe": the head of the pull request (fork branch `ddt-scrub-pruned-v2`) | its own |
| B | `/var/tmp/zts-ddtl-1028/B` | `5c7c4b63ff` "Use the DDT key alone for legacy tables in ddt_class_contains()": a child of P (fork branch `ddt-scrub-pruned-v2-legacy`) | its own, which are P's |

The three trees therefore differ in `module/zfs/ddt.c` only, and there only
in `ddt_class_contains()`; STEP 2 checks it file by file. Each is built here
with `./autogen.sh && ./configure --enable-debug && make -j8`, and each has
its own module: the runs switch trees with `scripts/zfs.sh -r`.

**The bug.** For every block pointer the traversal of a scrub or resilver
visits, `dsl_scan_visitbp()` calls `ddt_class_contains()` and skips the block
when it returns true: the DDT walk the scan does first (the classes up to
`scn_ddt_class_max`, the duplicate class for a scrub or resilver) has already
read that block through its entry. On M the function matches the key only.
Once `zpool ddtprune` has removed a block's unique entry, writing the same
data twice more creates a new entry with the same key, other DVAs and two
references, in the duplicate class. The walk then reads the new copies, and
the traversal skips the pruned block, still referenced and still carrying the
dedup bit, because its key is in that class. Nothing reads it: a scrub repairs
none of its damage, and a resilver does not copy it to the new device.

**P** checks that the entry found describes the block pointer
(`ddt_object_lookup()` of the whole entry, then `ddt_phys_select()`, as
`zio_ddt_free()` does for pruned blocks), and drops the shortcut that returned
true without a lookup when `max_class == DDT_CLASS_UNIQUE` (no scrub or
resilver uses that limit). It adds the ZTS test `dedup/dedup_prune_scrub`.

**B** goes back to the key-only `ddt_object_contains()`, and to that
shortcut, for tables of version `DDT_VERSION_LEGACY` only: pruning walks only
flat tables (`ddt_prune_walk()` passes `DDT_FLAG_FLAT`), so no entry of a
legacy table was ever pruned and its key is enough. Flat and unconfigured
tables keep P's check.

| `ddt_class_contains()` on | M | P | B |
| --- | --- | --- | --- |
| a flat or unconfigured table | the key only | the whole entry, then `ddt_phys_select()` | as P |
| a legacy table | the key only | as on a flat table | the key only, as M |
| `max_class == DDT_CLASS_UNIQUE` | true at once | lookups | true at once on a legacy table, lookups otherwise |

What the run establishes:

1. **The new test** `dedup_prune_scrub`, run in the dedup group: FAIL on M,
   at the check right after its first scrub (the corrupted side of the mirror
   is not repaired), PASS on P and on B.
2. **Every other result the same on M, P and B**: the dedup group's other 18
   results, and four scrub and resilver tests that hold dedup blocks (12
   results per tree with their setups and cleanups).
3. **B's legacy branch in the kernel** (STEP 7): no existing test scrubs a
   legacy DDT, so a scrub over one, three rounds on P and three on B, is the
   only run of that branch. It must decide as P does (the same count of
   skipped block pointers in every round); its time is recorded.
4. **No kernel finding** anywhere, and a clean unload at the end.

**Disk space.** A built debug tree takes about 1 GB here (the earlier
three-tree tasks on these VMs), the clone a few hundred MB, the tests at most
about 2 GB at a time (the dedup group's `dedup_quota` writes up to 1 GiB) and
STEP 7's pool under 1 GB. With about 11 GB free on `/var/tmp`, the three
trees fit together: all three are built in STEP 2, before the first run, and
none is replaced. STEP 0 stops below 9 GB free, STEP 2 below 4 GB after the
builds.

## The new test, and why M fails

`dedup_prune_scrub.ksh` (150 lines, blob `1d48547dcf`) does, in this order:

| Step | What | Its lines in the log |
| --- | --- | --- |
| 1 | a mirror of two sparse files `a` and `b` in `$TEST_BASE_DIR/dedup_prune_scrub` (the `-d` files directory), a third file `c`; `testpool/testfs` with `dedup=sha256`, recordsize 128k, compression off; the DDT log flushed every txg (two tunables, restored by its cleanup) | |
| 2 | `file1`: 8 blocks of random data, so 8 unique entries; `zpool ddtprune -p 100` prunes all of them | `SUCCESS: zpool ddtprune -p 100 testpool` |
| 3 | `file2` and `file3`, copies of `file1`: 8 new entries with the same keys, two references each, at other DVAs | `NOTE: file1 block 0 at <offset> <asize>, file2 block 0 at <offset> <asize>` (two different pairs) |
| 4 | export; random bytes over `file1`'s blocks on `b` only; `log_mustnot same_blocks a b`; import; `zpool scrub -w`; export; `log_must same_blocks a b` | `SUCCESS: same_blocks <dir>/a <dir>/b exited 1`, `SUCCESS: zpool scrub -w testpool`, `SUCCESS: same_blocks <dir>/a <dir>/b` |
| 5 | import; `zpool replace -w testpool b c`; export; `log_must same_blocks a c`; import; `zpool scrub -w`; `check_pool_status testpool errors "No known data errors"` | `SUCCESS: same_blocks <dir>/a <dir>/c`, a second `SUCCESS: zpool scrub -w testpool`, `SUCCESS: check_pool_status testpool errors No known data errors` |
| end | `log_pass` | `Scrub and resilver cover dedup blocks whose DDT entry was pruned` |

**On M**, steps 1 to 3 pass (pruning exists on master), and step 4's scrub
skips `file1`'s blocks: their keys are in the duplicate class, which the walk
covered through `file2`'s and `file3`'s copies. `b` keeps the random bytes, and
the test ends at `ERROR: same_blocks <dir>/a <dir>/b exited 1`, the first check
after the first scrub, before any resilver. That is the check at which M fails,
and the one the fix is about. **On P and B** the lookup finds the entry,
`ddt_phys_select()` finds none of its DVAs in `file1`'s block pointers, the
traversal scrubs them, and the mirror repairs `b`; the resilver copies them to
`c` the same way.

`file1`'s DDT is a flat one (default features: `fast_dedup` is enabled), so
B runs P's code here: the test cannot tell P from B, and is expected to pass
on both.

`dps` (a helper of STEP 1) prints what the test did, from the runner's log:
the number of `zpool scrub -w testpool` lines, of `same_blocks a b exited 1`
(the corruption shown), of `same_blocks a b` (repaired by the scrub), of
`same_blocks a c` (resilvered) and of the final error check, the `NOTE: file1
block 0 ...` line, the first `ERROR:` line and the end line.

**The end line** of a test is the last line before its `NOTE: Performing
test-fail callback` or `NOTE: Performing local cleanup` line, or its last line
when it has neither. For a test that passes it is its `log_pass` message, not
the `SUCCESS:` line before it: on P and B, `Scrub and resilver cover dedup
blocks whose DDT entry was pruned`, not `SUCCESS: check_pool_status testpool
errors No known data errors`. For a test that fails at a `log_must`, it is the
`ERROR:` line of that check.

## The dedup group, the scrub and resilver tests, and why these

`-T dedup` at B (and at P, and at M once it has P's `common.run`) runs two
sections, 19 results: `tests/functional/dedup` of `common.run`, 16 tests with
no setup or cleanup (`dedup_bclone`, `dedup_bclone_pruned`,
`dedup_fdt_create`, `dedup_fdt_import`, `dedup_fdt_pacing`,
`dedup_legacy_create`, `dedup_legacy_import`, `dedup_legacy_fdt_upgrade`,
`dedup_legacy_fdt_mixed`, `dedup_quota`, `dedup_log_zdb_leak`, `dedup_prune`,
`dedup_prune_leak`, `dedup_prune_percentage`, `dedup_prune_scrub`,
`dedup_zap_shrink`), and `tests/functional/dedup:Linux` of `linux.run`
(`setup`, `dedup_legacy_gang`, `cleanup`). The legacy-table tests are the five
`dedup_legacy_*`; the pruning tests are `dedup_prune`, `dedup_prune_leak`,
`dedup_prune_percentage`, `dedup_bclone_pruned` and the new
`dedup_prune_scrub`. STEP 2 reads the two sections out of each tree's
runfiles as the runner does and checks this list.

`ddt_class_contains()` has one caller, `dsl_scan_visitbp()`, and returns false
at its first line for a block pointer without the dedup bit, a line neither
commit changes. A test reaches the changed code only if it scrubs or resilvers
a pool that holds dedup blocks. Reading the tests at B for `dedup` together
with `scrub`, `resilver`, `replace` or `attach` in one file finds six, one of
which is a property list (`cli_root/zpool_get/zpool_get.cfg`). The other five
are run here:

| Run | Test | Why |
| --- | --- | --- |
| `d` (in `-T dedup`) | `dedup/dedup_prune_scrub` | the case P fixes: pruned entries of a flat table, then a scrub and a resilver |
| `1` | `raidz/raidz_dedup_overrun` | imports a stored RAID-Z pool (created with `compatibility=openzfs-2.3`, which includes `fast_dedup`, and `dedup=sha256`: a flat table) whose dedup entry has a second copy at another width, and scrubs it twice: entries found in the duplicate class that do describe their block pointers, which P must keep skipping |
| `2` | `raidz/raidz_expand_008_pos` | fast dedup on a RAID-Z1 before and after an expansion, then a scrub: `dedup double` and `unique double` block pointers in one flat table |
| `3` | `block_cloning/block_cloning_resilver_free` | a resilver (`zpool attach`) while two files share a block through the DDT: the walk queues it, the traversal must skip both references, and one is freed meanwhile; the resilver side of the same decision |
| `4` | `cli_root/zpool_prefetch/zpool_prefetch_001_pos` | a scrub over 8192 small dedup files, half of them with one to four copies: thousands of unique and duplicate entries, the scrub also forcing the DDT log out |

Each of `1` to `4` runs with `-t`, with its directory's `setup` and `cleanup`:
3 results each.

Left out on purpose:

- `cli_root/zpool_scrub` (18 tests): none of them writes a dedup block (none
  sets `dedup`), so every block pointer they scrub returns at the first line of
  `ddt_class_contains()`. The scrub code they test is the same in M, P and B.
- The rest of `raidz` and of `block_cloning`: no dedup block in a scrub or
  resilver.
- The rest of the dedup group runs anyway, in `-T dedup`. None of those tests
  scrubs or resilvers, so none reaches the changed function; the five
  `dedup_legacy_*` tests show that legacy tables still work on each tree, not
  B's legacy branch. STEP 7 is what reaches that branch.

## STEP 7's measurement, and what it can show

For a dedup block pointer on a legacy table, M and B ask each probed class's
ZAP for the key's length only (`ddt_object_contains()`, then
`ddt_zap_contains()`, one `zap_length_uint64_by_dnode()`). P allocates an entry
(`ddt_alloc()`), has `ddt_object_lookup()` and `ddt_zap_lookup()` allocate a
buffer, copy the value out (one `zap_lookup_length_uint64_by_dnode()`) and
decompress it, compares the entry's DVAs and birth with the block pointer
(`ddt_phys_select()`), and frees both. Both make one ZAP call per class whose
object exists, up to the class where the key is found, and on a legacy table
they decide the same way. So no existing counter tells P from B: both take the same ZAP lock and
leaf buffers (the `dbufstats` kstat cannot tell them apart), and the pool's
`ddt_stats_sha256` kstat counts `ddt_lookup()` only, which a scrub does not
call. What differs is CPU work per dedup block pointer, in syncing context.
`dsl_scan_sync()` logs that time per txg in `zfs_dbgmsg`, `scan visited <n>
blocks of <pool> in <t>ms (..., <k> in ddt, ...)`, next to the count of block
pointers skipped because `ddt_class_contains()` returned true (`in ddt`). That
line, the `scanned <n> ddt entries` line of the walk and the wall time of
`zpool scrub -w` are all this measurement reads. No code is added.

The pool, `ddtl`, is one sparse 2 GiB file, `$W/meas/vdev`, created on B with
`-o cachefile=none -o feature@fast_dedup=disabled` (so its DDT is a legacy
one: `zdb -D` prints `version=0 [LEGACY]`), mounted under `$W/meas/mnt`, with
`dedup=sha256`, `recordsize=4k`, `compression=off` and `atime=off`. It holds
three 256 MiB files: `u` and `d1` of random data, and `d2`, a `dd` copy of
`d1`. That is 65536 entries in the unique class (`u`) and 65536 in the
duplicate class (`d1` and `d2`, two references each). A scrub walks the
duplicate class (65536 entries); its traversal visits 196608 data block
pointers with the dedup bit: the 131072 of `d1` and `d2` are found in the
duplicate class and skipped (`in ddt`), the 65536 of `u` are looked up in the
ditto class (no object, so no ZAP call) and the duplicate class (a ZAP miss)
and scrubbed.

Six rounds, alternating P, B, P, B, P, B. Each loads the tree's module (fresh:
an empty ARC and an empty `dbgmsg`), imports the pool, runs `zpool scrub -w`
timed, reads the scan line of `zpool status`, saves the `dbgmsg`, and exports
the pool. Expected in every round: `in ddt` 131072, the walk 65536 entries
with `class_max = 1` (the duplicate class), and `scrub repaired 0B ... with 0
errors`. The visited count also takes in the pool's own metadata, which grows
a little with each import, scrub and export, so it is recorded with its range,
not required to be equal. If P's extra work shows, it shows as a longer
traversal time on P; the times are recorded with their spread, not judged
here.

## Rules

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  the module loads, every run and STEP 7's pool steps go through
  `nice -n 10 ionice -c3` too where the blocks say so, one thing at a time,
  nothing beside them.
- Never reboot. Never run `zfs-tests.sh -x` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing once STEP 2 has built the
  trees. `scripts/zfs.sh -r` (load) and `-u` (unload) run only here, on this
  VM, and only in the blocks and helpers that say so.
- This task's own directories are `/var/tmp/zts-ddtl-1028` (`$W`) and
  `/var/log/zts-ddtl-1028` (`$L`). Outside them, the only changes are STEP 0's
  release of `/var/tmp/vm-task-107n`, the doc checkout of an earlier task,
  under the conditions written there, and STEP 8's removal of the doc
  checkout of this one. Never touch another `/var/tmp/zts-*` or
  `/var/log/zts-*` directory, another `/var/tmp/vm-task-*` checkout or
  `/var/tmp/test_results`. Never create, edit or remove
  `/etc/zfs/zpool.cache` or `/etc/zfs/vdev_id.conf` yourself (the kernel
  records the tests' pools in `zpool.cache` while they exist; STEP 7's pool
  uses `cachefile=none`).
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only these: STEP 0's `rm -rf` of
  `/var/tmp/vm-task-107n`, STEP 8's `rmdir` of the empty files directories,
  and the final removals of STEP 8.
- Never run a `zpool`, `zfs` or `zdb` command of a tree other than the one
  whose module is loaded, or while none is. Never print the environment
  (`env`, `printenv`, `set`, `/proc/*/environ`) or a key file.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-ddtl-1028/env.sh`.
- Stop at any value the comments or Expected lines do not allow, unless the
  text says it is recorded only. After a line ending in `stop`, run only
  STEP 8; the helpers refuse to run once a step has stopped. A test that fails
  or is skipped is a result, recorded and compared, not a stop. Clean, repair
  and retry nothing by hand. If your permission system refuses a command,
  report it and stop. A kernel panic, oops, VERIFY or hung task means stop and
  report, never reboot.
- The builds take about 10 to 15 minutes together, each dedup group run 15 to
  25, each tree's four tests 10 to 20, STEP 7's rounds 5 to 10. If your shell
  tool cannot wait that long in the foreground, start that one block with the
  tool's own background mode and wait for it to exit. Never use `&` or
  `nohup`, and run nothing meanwhile.
- Your session cannot run anything after its final reply. Everything, cleanup
  included, happens before it; the reply is the whole of `REPORT.txt` and the
  result of the last command.

What earlier VM runs taught, and the blocks already handle: `modinfo` is in
`/usr/sbin`, not in `tigerblue77`'s PATH, so every block puts `/usr/sbin:/sbin`
first; this VM's umask has been 0002 and 0022, and `env.sh` sets 022; `/var/log`
directories are made with `sudo -n mkdir` and then given to `tigerblue77`.
`configure --enable-debug` writes 3 `#define ZFS_DEBUG 1` lines; compiler
warnings are counted as `:[0-9]+:[0-9]+: warning:`, and objtool's about 83
harmless `warning:` lines per tree are not counted. Results are matched
anywhere in a `Test:` line, which starts with a timestamp; a `-T` run names
tests without `.ksh`, a `-t` run with it, and `linux.run` tests are
`Test (Linux): ...` (the dedup group's `setup`, `dedup_legacy_gang` and
`cleanup`). A literal parenthesis in a regex is `[(]`. The tests' lines are
read from the runner's own `log` file; a test's end line is its `log_pass`
message when it passes (the last line before `NOTE: Performing local
cleanup`), not the `SUCCESS:` line before it. Kernel messages are read per run
from `journalctl -k --since`, without the `ZTS run` markers and the
`spa_import_progress_set_notes_impl` lines (ZTS clears `dmesg` after a failed
test). Each run waits 2 s first. A `-d` directory has mode 1777.
`zfs.sh -r` and `-u` signal the pid `/var/run/zed.pid` names, so they never run
while it exists. `/var/tmp/vm-task-107n` was made by `git init`, a fetch by
commit and a checkout: it has a detached HEAD and no `origin`, so STEP 0 checks
it by its commit, not by branch or remote, and the final removal of this
task's doc checkout accepts either shape.

## STEP 0: the VM free, the space, and the previous doc checkout

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-ddtl-1028 L=/var/log/zts-ddtl-1028 DOC=/var/tmp/vm-task-107t
date -u '+%F %T'
id -un                                # tigerblue77
sudo -n true && echo "passwordless sudo ok"
uname -r                              # recorded: 6.12.113+deb13-cloud-amd64 on the earlier runs
grep PRETTY_NAME /etc/os-release      # Debian GNU/Linux 13 (trixie)
nproc; umask                          # 16, then 0002 or 0022
free -m | head -n 2                   # recorded
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '          # nothing
ls /sys/module | grep -xE 'zfs|spl'    # nothing
ls -l /dev/zfs 2>&1                    # No such file or directory
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db|[f]io '   # nothing
pgrep -ax 'zed|lt-zed'                 # nothing
ls -l /var/run/zed.pid 2>&1            # No such file or directory: zfs.sh -r and -u signal the pid it names
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                # No devices found
grep -E ' zfs |testpool' /proc/mounts  # nothing
ls -d /testpool* /dedup_overrun 2>&1   # No such file or directory, twice
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
ls -d $W $L 2>&1                       # No such file or directory, twice
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
echo "/var/log zts dirs: $(ls -d /var/log/zts-* 2>/dev/null | xargs)"
df -h /var/tmp /var/log
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
sudo -n journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
for c in git ksh python3 modinfo sha256sum timeout truncate bzcat; do echo "$c: $(command -v $c || echo absent)"; done
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/t/VM-TESTS-107-ddtl.md 2>&1
```

Keep this output: it goes into `REPORT.txt` by hand. The number printed
after the one kernel line is the kernel baseline. The `/var/tmp`, `/var/log`
and `free` lines are recorded only: earlier tasks' directories there are left
alone. The doc checkout line is expected as `doc checkout: top
/var/tmp/vm-task-107t, branch vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`) for a clone, or
`branch HEAD, head <a commit of vm-tasks-1026>, origin error: No such remote
'origin'` for a checkout fetched by commit; both are fine. If you read this
document from another place, say so: STEP 8 then leaves that place alone.

Stop at once, changing nothing, and report what you found if: the user is not
`tigerblue77` or `sudo -n` fails; the kernel headers are missing; a zfs or
spl module is loaded or `/dev/zfs` exists; a test, build, fio, ksh or zdb
process or any `zed` runs (ignore the line of the shell running this block,
if `pgrep` shows it), or `/var/run/zed.pid` exists; a tmux pane other than
this session's runs anything but an idle shell; a loop or device-mapper
device, a zfs or test mount, a `/testpool*` or `/dedup_overrun` directory, a
non-empty `zpool.cache` or a `vdev_id.conf` exists; `$W` or `$L` exists (this
task started once already); `journalctl -k` prints nothing; or `git`, `ksh`,
`python3`, `modinfo`, `sha256sum`, `timeout` or `truncate` is absent. A
kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself: STEP 2
checks that the modules were built for the running one. `bzcat` absent is not
a stop either: `raidz_dedup_overrun` then fails on all three trees at its
first `bzcat`, which STEP 6 shows as the same on each.

Then the doc checkout of an earlier task, `/var/tmp/vm-task-107n`. It was made
by `git init`, a fetch by commit and a checkout, so it has a detached HEAD
and no `origin`, and the release before this one kept it for that reason. It
is removed only if it exists and holds nothing but that checkout: a
directory of `tigerblue77`'s with its own `.git` directory, at its top level,
whose HEAD is `5336fb106438f45c94d4dc8f3cc04847af5733b6` (the commit that
added `n/VM-TESTS-107-enc.md`, tracked in it), whose top-level entries are
exactly its tracked ones, with no change, no untracked or ignored file, and
nothing mounted under it. Its size and contents are recorded first:

```sh
export PATH=/usr/sbin:/sbin:$PATH
P=/var/tmp/vm-task-107n
{
if [ -e $P ] || [ -L $P ]; then
	echo "$P: $(stat -c '%F %U %a' $P), $(du -sh $P 2>&1 | cut -f1)"
	echo "$P holds: $(LC_ALL=C ls -A $P | xargs)"
	echo "  tracked top-level entries: $(git -C $P ls-tree --name-only HEAD 2>&1 | LC_ALL=C sort | xargs)"
	echo "  top $(git -C $P rev-parse --show-toplevel 2>&1), HEAD $(git -C $P rev-parse HEAD 2>&1)," \
	    "branch $(git -C $P rev-parse --abbrev-ref HEAD 2>&1), remotes [$(git -C $P remote 2>&1 | xargs)]"
	echo "  .git: $(stat -c %F $P/.git 2>&1); status lines: $(git -C $P status --porcelain --ignored --untracked-files=all 2>&1 | wc -l)"
	if [ -d $P ] && [ ! -L $P ] && [ "$(stat -c %U $P)" = tigerblue77 ] &&
	    [ -d $P/.git ] && [ ! -L $P/.git ] &&
	    [ "$(git -C $P rev-parse --show-toplevel 2>/dev/null)" = $P ] &&
	    [ "$(git -C $P rev-parse HEAD 2>/dev/null)" = 5336fb106438f45c94d4dc8f3cc04847af5733b6 ] &&
	    git -C $P ls-files --error-unmatch n/VM-TESTS-107-enc.md > /dev/null 2>&1 &&
	    [ -z "$(git -C $P status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    [ "$(LC_ALL=C ls -A $P | grep -vx .git | xargs)" = "$(git -C $P ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
	    ! grep -q " ${P}[ /]" /proc/mounts; then
		rm -rf $P; echo "released $P rc=$?"
	else
		echo "$P is not only that doc checkout: kept"
	fi
else
	echo "$P: absent, nothing to release"
fi
ls -d $P 2>&1
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"
} 2>&1
```

Expected, from what the last task recorded of it:

```
/var/tmp/vm-task-107n: directory tigerblue77 755, 1.7M
/var/tmp/vm-task-107n holds: .git PART-A-105.md PART-A-107.md PART-B-105.md PART-B-107.md VM-BENCH-107.md VM-TESTS-105-1026c.md VM-TESTS-106-i4.md VM-TESTS-107-1026c.md VM-TESTS-ftrunc2.md VM-TESTS-ftrunc3.md VM-TESTS-zdbil.md bench e f g h k l m n
  tracked top-level entries: PART-A-105.md PART-A-107.md PART-B-105.md PART-B-107.md VM-BENCH-107.md VM-TESTS-105-1026c.md VM-TESTS-106-i4.md VM-TESTS-107-1026c.md VM-TESTS-ftrunc2.md VM-TESTS-ftrunc3.md VM-TESTS-zdbil.md bench e f g h k l m n
  top /var/tmp/vm-task-107n, HEAD 5336fb106438f45c94d4dc8f3cc04847af5733b6, branch HEAD, remotes []
  .git: directory; status lines: 0
released /var/tmp/vm-task-107n rc=0
ls: cannot access '/var/tmp/vm-task-107n': No such file or directory
/var/tmp free: <n>G
```

or `/var/tmp/vm-task-107n: absent, nothing to release`, the same `ls` line and
the free space. `... is not only that doc checkout: kept` is not a stop:
record why (the lines above it say which condition failed). A `rm` that does
not end with rc 0, or that your permission system refuses, is reported and not
retried. **Stop** if less than `9G` is free on `/var/tmp` now (three trees,
the clone, the tests and STEP 7's pool: about 5 GB at the peak).

## STEP 1: this task's directories and helpers

The block writes nothing unless it can create both directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/tmp/zts-ddtl-1028 && sudo -n mkdir /var/log/zts-ddtl-1028 &&
    sudo -n chown tigerblue77: /var/log/zts-ddtl-1028; then
echo "mkdir rc=0"
cat > /var/log/zts-ddtl-1028/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-ddtl-1028
L=/var/log/zts-ddtl-1028
DOC=/var/tmp/vm-task-107t
DOCFILE=t/VM-TESTS-107-ddtl.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# M: upstream master, P's parent and the merge base of P with master. P: the
# head of https://github.com/openzfs/zfs/pull/19403. B: a child of P.
HM=465d86a7814f63ea6441557db0c0c62c348218f1
HP=43fda04502f31e8f1c19fe776054a9c4fd47df8c
HB=5c7c4b63ff866b466a9d23cf1f0469e23be8b57e
Z=tests/zfs-tests
DDT=module/zfs/ddt.c
# ddt.c's blob in M, P and B; the new test and its blob; P's three
# test-side files (git's order), which M takes from P before its build;
# what P and B change, and their patch-ids.
DDTM=ce520e168d8652dd077d3a0b034ba9e46e3bce65
DDTP=9d20e652a3ada03944e8c1faa271f9bbc7506d3b
DDTB=7d255b8eeff1246594dfceeafda4295b0c61e5e9
NT=$Z/tests/functional/dedup/dedup_prune_scrub.ksh
NTBLOB=1d48547dcf191309f598dcda5e5394166bffad8e
TF="tests/runfiles/common.run $Z/tests/Makefile.am $NT"
NUMP="17 7 $DDT 1 0 tests/runfiles/common.run 1 0 $Z/tests/Makefile.am 150 0 $NT"
NUMB="20 0 $DDT"
PIDP=cad19e1cb2a84324aac8bfd2a152ef0577937097
PIDB=e6d1ce29a2599d022f3e54b4bca79080c54788cc
TR=tests/test-runner/bin/test-runner.py
# The dedup tag group: tests/functional/dedup (no setup, no cleanup) and
# tests/functional/dedup:Linux (setup, one test, cleanup): 19 results.
DG="dedup_bclone dedup_bclone_pruned dedup_fdt_create dedup_fdt_import
dedup_fdt_pacing dedup_legacy_create dedup_legacy_import
dedup_legacy_fdt_upgrade dedup_legacy_fdt_mixed dedup_quota
dedup_log_zdb_leak dedup_prune dedup_prune_leak dedup_prune_percentage
dedup_prune_scrub dedup_zap_shrink"
DG=$(echo $DG)
DGL=dedup_legacy_gang
# The four scrub and resilver tests: <run>:<path under tests/functional>.
ONE="1:raidz/raidz_dedup_overrun 2:raidz/raidz_expand_008_pos
3:block_cloning/block_cloning_resilver_free
4:cli_root/zpool_prefetch/zpool_prefetch_001_pos"
ONE=$(echo $ONE)
# The tunables these runs change and must restore.
TUN="zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min
zfs_dedup_log_flush_entries_max metaslab_force_ganging
metaslab_force_ganging_pct zfs_bclone_enabled zfs_dbgmsg_enable
zfs_scan_legacy zfs_scan_issue_strategy zfs_scan_vdev_limit
zfs_scrub_after_expand"
# STEP 7's pool.
MP=ddtl
# Kernel findings; KIGN drops the -K markers and the import progress notes.
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# A result line of the runner, "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a test of linux.run.
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: [$2], expected [$3]"
	fi
}

# A step that stops records why in $L/STOP; nothing runs after it.
stop() {   # stop <reason>
	echo "$*: stop" | tee -a $L/runs.txt
	[ -e $L/STOP ] || echo "$*" > $L/STOP
	return 1
}
stopped() {
	[ -e $L/STOP ] || return 1
	echo "stopped earlier ($(cat $L/STOP)): nothing run" | tee -a $L/runs.txt
}

sv_of() {   # sv_of <M|P|B>: the srcversion STEP 2 recorded for that tree
	case $1 in
	M) echo "$SV_M" ;;
	P) echo "$SV_P" ;;
	B) echo "$SV_B" ;;
	esac
}

tunables() {
	local t
	for t in $TUN; do
		if [ -e /sys/module/zfs/parameters/$t ]; then
			printf '%s=%s ' $t "$(cat /sys/module/zfs/parameters/$t)"
		else
			printf '%s=absent ' $t
		fi
	done
}

loops() {
	sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1
}

# The mount points the runs' pools make under /.
rootdirs() {
	ls -d /testpool* /dedup_overrun 2>/dev/null | xargs
}

# Build <tree> in place: autogen, configure (debug), make -j8, niced.
build_tree() {   # build_tree <M|P|B>
	local t0 rc
	stopped && return 1
	t0=$(date +%s)
	(cd $W/$1 && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure --enable-debug && make -j8") > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "free=$(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -m 10 -nE ': error:|\*\*\* \[' $L/build-$1.log | tee -a $L/build.txt
		stop "the build of $1"
	fi
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
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[f]io ' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Result counts of runner result lines on stdin: "PASS x19", "FAIL x1, PASS x18".
tally() {
	grep -oE '[A-Z]+\]( [(]RERAN[)])?$' | sed 's/\].*$//' | sort | uniq -c |
	    awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# The lines of test <name> in a runner log, timestamps stripped. A group
# run names a test without .ksh, a -t run with it.
section() {   # section <runner log> <name>
	awk -v n="$2" '
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		next
	}
	sec { line = $0; sub(/^[0-9:.]+ /, "", line); print line }' "$1"
}

# The end line of test <name>: the last line before its test-fail callbacks
# and its log_onexit cleanup, else its last line. Its log_pass message when
# it passes; the line that failed when a log_must fails.
endline() {   # endline <runner log> <name>
	section "$1" "$2" | awk '
	/^NOTE: Performing (test-fail callback|local cleanup)/ { exit }
	NF { prev = $0 }
	END { print prev }'
}

# The first ERROR line and the end line of test <name>.
fails() {   # fails <runner log> <name>
	local e
	e=$(section "$1" "$2" | grep -m 1 'ERROR: ')
	echo "  $2: first ERROR line: ${e:-none}"
	echo "    end line: $(endline "$1" "$2")"
}

# How far dedup_prune_scrub got, from its lines in a runner log.
dps() {   # dps <runner log>
	local s e
	s=$(section "$1" dedup_prune_scrub)
	e=$(printf '%s\n' "$s" | grep -m 1 'ERROR: ')
	echo "  dedup_prune_scrub: scrubs x$(printf '%s\n' "$s" | grep -c '^SUCCESS: zpool scrub -w testpool$')," \
	    "corruption shown x$(printf '%s\n' "$s" | grep -cE '^SUCCESS: same_blocks [^ ]+/a [^ ]+/b exited 1$')," \
	    "repaired by the scrub x$(printf '%s\n' "$s" | grep -cE '^SUCCESS: same_blocks [^ ]+/a [^ ]+/b$')," \
	    "resilvered x$(printf '%s\n' "$s" | grep -cE '^SUCCESS: same_blocks [^ ]+/a [^ ]+/c$')," \
	    "no data errors x$(printf '%s\n' "$s" | grep -c '^SUCCESS: check_pool_status testpool errors No known data errors$')"
	echo "  note: $(printf '%s\n' "$s" | grep -m 1 '^NOTE: file1 block 0 at ' | sed 's/^NOTE: //')"
	echo "  first ERROR line: ${e:-none}"
	echo "  end line: $(endline "$1" dedup_prune_scrub)"
}

# Kernel messages since <since>, from the journal. A finding stops; a panic,
# VERIFY or hung task also keeps the module loaded (PANIC).
kern_check() {   # kern_check <label> <since> <expected ZTS marks>
	local n m
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $L/kern-$1.txt 2>&1
	n=$(grep -iE "$KRE" $L/kern-$1.txt | grep -vcE "$KIGN")
	m=$(grep -c 'ZTS run ' $L/kern-$1.txt)
	echo "kernel $1: $n findings, $m ZTS marks (expected $3)" | tee -a $L/runs.txt
	grep -iE "$KRE" $L/kern-$1.txt | grep -vE "$KIGN" | head -n 20 | sed 's/^/  /' | tee -a $L/runs.txt
	if grep -iE "$KRE" $L/kern-$1.txt | grep -vE "$KIGN" |
	    grep -qiE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than'; then
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

# Load <tree>'s module stack with its own scripts/zfs.sh -r, which first
# unloads the one loaded. Only with nothing stopped, no /var/run/zed.pid
# and no pool or zfs mount left; then the srcversion must be <tree>'s, no
# pool may appear, and the tunables must be those of its first load.
switch_to() {   # switch_to <M|P|B> <label>
	local t=$1 lb=$2 sv cur prev p since rc
	stopped && return 1
	sv=$(sv_of $t)
	if [ -z "$sv" ]; then
		stop "$lb: no srcversion recorded for $t"
		return 1
	fi
	if [ -e /var/run/zed.pid ]; then
		stop "$lb: /var/run/zed.pid exists, nothing loaded"
		return 1
	fi
	prev=$(cat $L/loaded 2>/dev/null)
	if [ -n "$prev" ]; then
		p=$(sudo -n $W/$prev/zpool list -H -o name 2>&1)
		if [ -n "$p$(grep -E ' zfs |testpool' /proc/mounts)" ]; then
			stop "$lb: a pool or a zfs mount is left ($p), nothing reloaded"
			return 1
		fi
	fi
	since=$(date '+%F %T')
	(cd $W/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -r) > $L/load-$lb.log 2>&1
	rc=$?
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	rm -f $L/loaded
	if [ $rc -ne 0 ] || [ "$cur" != "$sv" ]; then
		echo "$lb: load $t rc=$rc, srcversion [$cur], expected $sv" | tee -a $L/runs.txt
		head -n 20 $L/load-$lb.log | sed 's/^/  /' | tee -a $L/runs.txt
		touch $L/HELD
		stop "$lb: the load of $t"
		return 1
	fi
	echo $t > $L/loaded
	p=$(sudo -n $W/$t/zpool list -H -o name 2>&1)
	[ -e $L/tunables-$t.txt ] || tunables > $L/tunables-$t.txt
	echo "$lb: load $t rc=0, srcversion $cur, pools [$p]" | tee -a $L/runs.txt
	if [ -n "$p" ]; then
		stop "$lb: a pool appeared at the load of $t"
		return 1
	fi
	if [ "$(tunables)" != "$(cat $L/tunables-$t.txt)" ]; then
		stop "$lb: tunables [$(tunables)] differ from the first load of $t"
		return 1
	fi
	kern_check $lb "$since" 0
}

# Before a run: nothing stopped, a new label, <tree>'s module loaded, a new
# files directory of mode 1777, and none of what a run can leave.
pre_run() {   # pre_run <label> <tree> <fd>
	local cur p l m r z tu
	stopped && return 1
	if [ -e $L/$1.log ] || [ -e $3 ]; then
		echo "label $1 already used: nothing run" | tee -a $L/runs.txt
		return 1
	fi
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$(cat $L/loaded 2>/dev/null)" != "$2" ] || [ "$cur" != "$(sv_of $2)" ]; then
		stop "$1: the loaded module is [$cur], not $2's"
		return 1
	fi
	if ! mkdir -m 1777 $3; then
		stop "$1: cannot make $3"
		return 1
	fi
	p=$(sudo -n $W/$2/zpool list -H -o name 2>&1)
	l=$(loops)
	m=$(grep -E ' zfs |testpool' /proc/mounts)
	r=$(rootdirs)
	z=$(pgrep -ax 'zed|lt-zed')
	tu="tunables as loaded"
	[ "$(tunables)" = "$(cat $L/tunables-$2.txt)" ] || tu="TUNABLES CHANGED: $(tunables)"
	echo "before $1: tree $2, fd=$3 ($(stat -c '%U %a' $3), holds [$(LC_ALL=C ls -A $3 | xargs)])" \
	    "pools=[$p] loops=[$l] mounts=[$m] rootdirs=[$r] zed=[$z] $tu" | tee -a $L/runs.txt
	if [ -n "$(LC_ALL=C ls -A $3)" ] || [ "$(stat -c %a $3)" != 1777 ] ||
	    [ -n "$p$l$m$r$z" ] || [ "$tu" != "tunables as loaded" ] || [ -e /var/run/zed.pid ]; then
		stop "$1: the VM is not clean before the run, nothing run"
		return 1
	fi
}

# After a run: nothing left anywhere, its files directory empty. A pool,
# device, mount, process or loop device left may hold the module (HELD:
# STEP 8 does not unload).
leftovers() {   # leftovers <label> <tree> <fd>
	local bad=0 held=0 p
	{
	p=$(sudo -n $W/$2/zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; held=1; }
	p=$(sudo -n dmsetup ls 2>&1)
	[ "$p" = "$(cat $L/dm-before.txt)" ] || { echo "  LEFTOVER dm devices: $p"; held=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; held=1; }
	p=$(rootdirs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; held=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; held=1; }
	[ -e /var/run/zed.pid ] && { echo "  LEFTOVER /var/run/zed.pid"; bad=1; }
	p=$(tunables)
	if [ "$p" = "$(cat $L/tunables-$2.txt)" ]; then
		echo "  tunables: as loaded"
	else
		echo "  LEFTOVER tunables: $p"; bad=1
	fi
	p=$(loops)
	echo "  loop devices: [$(printf '%s' "$p" | tr '\n' ';')]"
	[ -n "$p" ] && held=1
	p=$(LC_ALL=C ls -A $3 | xargs)
	echo "  $3 holds: [$p] (expected [])"
	if [ -n "$p" ]; then
		sudo -n ls -laR --time-style=+%T $3 2>&1 | head -n 40 | sed 's/^/    /'
		bad=1
	fi
	if [ $bad$held = 00 ]; then
		echo "leftovers after $1: as expected"
	else
		echo "leftovers after $1: NOT AS EXPECTED"
	fi
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	[ $bad$held = 00 ] && return 0
	[ $held = 1 ] && touch $L/HELD
	stop "$1: leftovers"
}

# One zfs-tests.sh run on the loaded <tree>, -T <tag> or -t <path under
# tests/functional, without .ksh>, into its own files directory, then its
# results, its kernel messages and what it left. A test result is recorded,
# not a stop; a run without its <n> result lines is.
zts() {   # zts <label> <tree> <-T|-t> <tag or path> <n>
	local lb=$1 t=$2 how=$3 what=$4 want=$5 fd=$W/files-$1 sel outer since t0 rc c nt nl np d n bad=0
	case $how in
	-T) sel="-T $what"; outer=3600 ;;
	-t) sel="-t tests/functional/$what.ksh"; outer=1800 ;;
	*) echo "zts: -T or -t"; return 1 ;;
	esac
	pre_run $lb $t $fd || return 1
	sleep 2   # two runs started in one second would share a results directory
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $W/$t && timeout -k 60 $outer nice -n 10 ionice -c3 ./scripts/zfs-tests.sh -vK \
	    -d $fd $sel) > $L/$lb.log 2>&1
	rc=$?
	grep -E "$TRE" $L/$lb.log > $L/$lb-tests.txt
	c=$(tally < $L/$lb-tests.txt)
	nt=$(grep -c 'Test: ' $L/$lb-tests.txt)
	nl=$(grep -c 'Test [(]Linux[)]: ' $L/$lb-tests.txt)
	echo "$lb $t $how $what ${c:-NO RESULT} (Test: x$nt, Test (Linux): x$nl, of $want) rc=$rc" \
	    "wall=$(( $(date +%s) - t0 ))s" | tee -a $L/runs.txt
	grep -m 1 '^Missing util' $L/$lb.log | sed 's/^/  /' | tee -a $L/runs.txt
	grep -v '\[PASS\]' $L/$lb-tests.txt | sed 's/^/  not passed: /' | tee -a $L/runs.txt
	np=$(grep -v '\[PASS\]' $L/$lb-tests.txt |
	    sed -E 's|^.*/([^/ ]+) [(]run as .*$|\1|; s|[.]ksh$||' | xargs)
	d=$(awk '/^Log directory/ { print $3 }' $L/$lb.log | head -n 1)
	if [ -f "$d/log" ]; then
		for n in $np; do
			[ "$what" = dedup ] && [ $n = dedup_prune_scrub ] && continue
			fails "$d/log" $n
		done | tee -a $L/runs.txt
		if [ "$what" = dedup ]; then
			dps "$d/log" > $L/dps-$lb.txt
			tee -a $L/runs.txt < $L/dps-$lb.txt
		fi
	else
		echo "  no runner log (Log directory: '$d')" | tee -a $L/runs.txt
	fi
	kern_check $lb "$since" $want || bad=1
	if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
		stop "$lb: outer timeout"
		bad=1
	fi
	if [ $((nt + nl)) -ne $want ]; then
		stop "$lb: $((nt + nl)) result lines, not $want"
		bad=1
	fi
	leftovers $lb $t $fd || bad=1
	return $bad
}

# From a dbgmsg dump, for pool $MP: dsl_scan_sync's "scan visited" lines
# (count, ms, blocks visited, "in ddt"), dsl_scan_ddt's "scanned ... ddt
# entries" lines (entries, class_max), and the "scan issued" lines (count,
# ms, blocks).
scan_digest() {   # scan_digest <dbgmsg file>
	LC_ALL=C awk -v p="$MP" '
	index($0, "scan visited ") && index($0, " blocks of " p " in ") {
		for (i = 1; i < NF; i++) {
			if ($i == "visited")
				v += $(i + 1)
			if ($i == "in" && $(i + 1) ~ /^[0-9]+ms$/) {
				x = $(i + 1); sub(/ms$/, "", x); vms += x
			}
			if ($i == "ddt," && $(i - 1) == "in")
				d += $(i - 2)
		}
		nv++
	}
	index($0, " ddt entries on " p " with class_max = ") {
		for (i = 1; i < NF; i++) {
			if ($i == "scanned")
				e += $(i + 1)
			if ($i == "=") {
				cm = $(i + 1); sub(/;$/, "", cm)
			}
		}
	}
	index($0, "scan issued ") && index($0, " blocks for " p " (") {
		for (i = 1; i < NF; i++) {
			if ($i == "issued")
				is += $(i + 1)
			if ($i == "in" && $(i + 1) ~ /^[0-9]+ms$/) {
				x = $(i + 1); sub(/ms$/, "", x); ims += x
			}
		}
		ni++
	}
	END {
		printf "%d %d %d %d %d %s %d %d %d\n", nv, vms, v, d, e,
		    (cm == "" ? "none" : cm), ni, ims, is
	}' "$1"
}

# STEP 7, one round on <tree>: load it, import the pool, scrub it timed,
# keep its dbgmsg, export it. One row in $L/meas-rows.txt: round, tree,
# import s, scrub s, then scan_digest's nine fields.
meas_round() {   # meas_round <n> <P|B>
	local n=$1 t=$2 since a b c rc1 rc2=- rc3=- st row
	switch_to $t l$n || return 1
	since=$(date '+%F %T')
	a=$(date +%s.%N)
	nice -n 10 ionice -c3 sudo -n $W/$t/zpool import -o cachefile=none -d $W/meas $MP > $L/r$n.log 2>&1
	rc1=$?
	b=$(date +%s.%N)
	c=$b
	if [ $rc1 -eq 0 ]; then
		nice -n 10 ionice -c3 sudo -n $W/$t/zpool scrub -w $MP >> $L/r$n.log 2>&1
		rc2=$?
		c=$(date +%s.%N)
		st=$(sudo -n $W/$t/zpool status $MP 2>&1 | grep -o 'scrub repaired .* with [0-9]* errors')
		sudo -n cat /proc/spl/kstat/zfs/dbgmsg > $L/r$n-dbgmsg.txt 2>&1
		sudo -n $W/$t/zpool export $MP >> $L/r$n.log 2>&1
		rc3=$?
		row="$n $t $(LC_ALL=C awk -v a=$a -v b=$b -v c=$c 'BEGIN { printf "%.2f %.2f", b - a, c - b }') $(scan_digest $L/r$n-dbgmsg.txt)"
		echo "$row" >> $L/meas-rows.txt
		echo "$row" | LC_ALL=C awk -v st="$st" '{
			printf "r%s %s: import %ss, scrub %ss (%s); traversal %s txgs %s ms, visited %s, in ddt %s; walk %s entries, class_max %s; issued %s blocks in %s ms over %s txgs\n",
			    $1, $2, $3, $4, st, $5, $6, $7, $8, $9, $10, $13, $12, $11 }' | tee -a $L/meas.txt
	fi
	echo "r$n $t: import rc=$rc1, scrub rc=$rc2, export rc=$rc3" | tee -a $L/meas.txt
	kern_check r$n "$since" 0 || return 1
	if [ "$rc1$rc2$rc3" != 000 ]; then
		stop "r$n: the import, the scrub or the export failed"
		return 1
	fi
}

# STEP 7's rounds summed up per tree, P first.
meas_summary() {
	LC_ALL=C awk '
	{
		t = $2; n[t]++; s[t] += $4; v[t] += $6
		if (n[t] == 1 || $4 < s0[t]) s0[t] = $4
		if (n[t] == 1 || $4 > s1[t]) s1[t] = $4
		if (n[t] == 1 || $6 < v0[t]) v0[t] = $6
		if (n[t] == 1 || $6 > v1[t]) v1[t] = $6
		if (n[t] == 1 || $7 < b0[t]) b0[t] = $7
		if (n[t] == 1 || $7 > b1[t]) b1[t] = $7
		k = $8 " " $9 " " $10
		if (!(k in seen)) {
			seen[k] = 1; nk++; keys = keys (nk > 1 ? " | " : "") k
		}
		rows++
	}
	END {
		split("P B", order, " ")
		for (i = 1; i <= 2; i++) {
			t = order[i]
			if (!(t in n)) {
				printf "%s: no round\n", t
				continue
			}
			printf "%s: %d rounds; scrub min %.2f mean %.2f max %.2f s; traversal min %d mean %.1f max %d ms; visited %d to %d\n",
			    t, n[t], s0[t], s[t] / n[t], s1[t], v0[t], v[t] / n[t], v1[t], b0[t], b1[t]
		}
		if (("P" in n) && ("B" in n) && s["P"] > 0 && v["P"] > 0)
			printf "B against P, means: scrub %+.1f%%, traversal %+.1f%%\n",
			    (s["B"] / n["B"] / (s["P"] / n["P"]) - 1) * 100,
			    (v["B"] / n["B"] / (v["P"] / n["P"]) - 1) * 100
		printf "counts (in ddt, walk entries, class_max) over %d rounds: %s (%d distinct)\n",
		    rows, keys, nk
	}' $L/meas-rows.txt
}
ENV
bash -n /var/log/zts-ddtl-1028/env.sh; echo "env.sh syntax rc=$?"
cat > /var/log/zts-ddtl-1028/group.py <<'PY'
# The sections of a tree's common.run and linux.run that "-T dedup" runs,
# read as the test runner reads them, and the result lines they give.
import ast
import configparser
import os
import sys

d = sys.argv[1]
c = configparser.RawConfigParser()
c.read([os.path.join(d, 'common.run'), os.path.join(d, 'linux.run')])
out = []
n = 0
for s in c.sections():
    if 'tests' not in c.options(s):
        continue
    if 'dedup' not in ast.literal_eval(c.get(s, 'tags')):
        continue
    tests = ast.literal_eval(c.get(s, 'tests'))
    pre = c.get(s, 'pre').strip()
    post = c.get(s, 'post').strip()
    n += len(tests) + (1 if pre else 0) + (1 if post else 0)
    out.append('%s: pre [%s] post [%s] tests [%s]' %
               (s, pre, post, ' '.join(tests)))
print('%s; %d results' % ('; '.join(out), n))
PY
. /var/log/zts-ddtl-1028/env.sh
date '+%F %T' > $L/start.txt
sudo -n dmsetup ls > $L/dm-before.txt 2>&1
if [ "$(git -C $DOC rev-parse --show-toplevel 2>/dev/null)" = $DOC ] && [ -f $DOC/$DOCFILE ]; then
	git -C $DOC rev-parse HEAD > $L/doc-head.txt
	sha256sum < $DOC/$DOCFILE | cut -d' ' -f1 > $L/doc-sha.txt
fi
{
echo "start $(cat $L/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
stat -c '%U %a %n' $W $L
echo "doc checkout: head $(cat $L/doc-head.txt 2>&1), $DOCFILE sha256 $(cat $L/doc-sha.txt 2>&1)"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/record.txt
else
echo "mkdir failed: this task's directories exist or cannot be made: stop"
fi
```

Expected: `mkdir rc=0`, `env.sh syntax rc=0`, then `start ...; tigerblue77;
<uname -r>; umask 0022`, two `stat` lines (`tigerblue77 755` for `$W` and
`$L`), the doc checkout line with its full head and the same sha256 as STEP 0
printed, and the `zpool.cache` and free-space lines. A doc checkout line with
`No such file or directory` means this document was not read from `$DOC`:
record it, it is not a stop (STEP 8 then keeps `$DOC`).

## STEP 2: the sources, the three trees, the builds

Fetch B by its commit from the fork and upstream master (read-only towards
GitHub), and pin the three commits: B's parent is P, P's parent is M, M is the
merge base of P with upstream master, what each commit changes, its patch-id,
`ddt.c` in each, the new test. Where the two fork branches and the pull
request point now is recorded only (`ddt-scrub-pruned-v2`, and so the pull
request, may since have moved on to B):

```sh
. /var/log/zts-ddtl-1028/env.sh
if ! stopped; then
	{
	git init -q $W/repo > $L/init.log 2>&1; echo "init rc=$?"
	git -C $W/repo fetch -q --no-tags $FORK "+$HB:refs/heads/b" > $L/fetch-fork.log 2>&1; echo "fetch B rc=$?"
	git -C $W/repo fetch -q --no-tags $UPSTREAM "+refs/heads/master:refs/heads/upstream-master" \
	    > $L/fetch-upstream.log 2>&1; echo "fetch master rc=$?"
	cd $W/repo
	same "B" "$(git rev-parse -q --verify refs/heads/b)" $HB
	same "B's parent" "$(git rev-parse -q --verify $HB^)" $HP
	same "P's parent" "$(git rev-parse -q --verify $HP^)" $HM
	same "merge base of P with upstream master" "$(git merge-base refs/heads/upstream-master $HP 2>&1)" $HM
	same "what P changes" "$(git diff --numstat $HM $HP | xargs)" "$NUMP"
	same "what B changes" "$(git diff --numstat $HP $HB | xargs)" "$NUMB"
	same "P's patch-id" "$(git diff $HM $HP | git patch-id --stable | cut -d' ' -f1)" $PIDP
	same "B's patch-id" "$(git diff $HP $HB | git patch-id --stable | cut -d' ' -f1)" $PIDB
	same "ddt.c in M, P, B" "$(for c in $HM $HP $HB; do git rev-parse -q --verify $c:$DDT; done | xargs)" "$DDTM $DDTP $DDTB"
	same "the new test in P and B" "$(for c in $HP $HB; do git rev-parse -q --verify $c:$NT; done | xargs)" "$NTBLOB $NTBLOB"
	for c in $HM $HP $HB; do echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"; done
	echo "upstream master now (recorded only): $(git log -1 --abbrev=10 --format='%h %s' refs/heads/upstream-master)"
	echo "on GitHub now (recorded only): $(git ls-remote $FORK refs/heads/ddt-scrub-pruned-v2 refs/heads/ddt-scrub-pruned-v2-legacy 2>&1 |
	    awk '{ printf "%s%s=%.10s", s, $2, $1; s = ", " }'), $(git ls-remote $UPSTREAM refs/pull/19403/head 2>&1 |
	    awk '{ printf "%s=%.10s", $2, $1 }')"
	} 2>&1 | tee $L/fix.txt
	grep -q '^MISMATCH' $L/fix.txt && stop "the commits are not as expected"
	grep -qE '^(init|fetch [A-Za-z]+) rc=[1-9]' $L/fix.txt && stop "the init or a fetch failed"
fi
```

Expected:

```
init rc=0
fetch B rc=0
fetch master rc=0
ok: B
ok: B's parent
ok: P's parent
ok: merge base of P with upstream master
ok: what P changes
ok: what B changes
ok: P's patch-id
ok: B's patch-id
ok: ddt.c in M, P, B
ok: the new test in P and B
  465d86a781 ZTS: find zinject in zfs_failsafe when it is not installed
  43fda04502 Do not let scrub skip a dedup block its DDT entry does not describe
  5c7c4b63ff Use the DDT key alone for legacy tables in ddt_class_contains()
upstream master now (recorded only): <its head>
on GitHub now (recorded only): refs/heads/ddt-scrub-pruned-v2=<43fda04502 or 5c7c4b63ff>, refs/heads/ddt-scrub-pruned-v2-legacy=5c7c4b63ff, refs/pull/19403/head=<the same as ddt-scrub-pruned-v2>
```

A `MISMATCH`, or a failed init or fetch, means stop (the commits were
rewritten). The last two lines are not checked.

Then one worktree per tree, P's three test-side files in M, and what each
tree holds: `ddt.c` and its three markers (`phys select` is P's
`ddt_phys_select()` call in `ddt_class_contains()`, `legacy branch` B's test
of `DDT_VERSION_LEGACY`, `unique shortcut` the `DDT_CLASS_UNIQUE` test that P
removed and B brought back for legacy tables), the new test and its two
registrations, the dedup group as the runner will read it, and the test files
the runs need:

```sh
. /var/log/zts-ddtl-1028/env.sh
if ! stopped; then
	{
	for t in M:$HM P:$HP B:$HB; do
		git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktree.log 2>&1
		echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD 2>&1)"
	done
	git -C $W/M checkout $HP -- $TF; echo "M: P's test files rc=$?"
	same "M against master" "$(git -C $W/M diff HEAD --name-only | xargs)" "$TF"
	same "M against P" "$(git -C $W/M diff $HP --name-only | xargs)" $DDT
	same "P as committed" "$(git -C $W/P status --porcelain --untracked-files=no | wc -l)" 0
	same "B as committed" "$(git -C $W/B status --porcelain --untracked-files=no | wc -l)" 0
	same "B against P" "$(git -C $W/B diff $HP --name-only | xargs)" $DDT
	for t in M P B; do
		cd $W/$t
		echo "$t: ddt.c $(git hash-object $DDT | cut -c1-10)," \
		    "phys select x$(grep -c 'found = (ddt_phys_select(ddt, dde, bp) !=' $DDT)," \
		    "legacy branch x$(grep -c 'if (ddt->ddt_version == DDT_VERSION_LEGACY) {' $DDT)," \
		    "unique shortcut x$(grep -c 'if (max_class == DDT_CLASS_UNIQUE)' $DDT);" \
		    "new test $(stat -c %A $NT 2>&1) $(git hash-object $NT 2>&1 | cut -c1-10)," \
		    "in common.run x$(grep -c "'dedup_prune_scrub'" tests/runfiles/common.run)," \
		    "in Makefile.am x$(grep -c 'functional/dedup/dedup_prune_scrub[.]ksh' $Z/tests/Makefile.am)"
		same "$t: the dedup group" "$(python3 $L/group.py $W/$t/tests/runfiles 2>&1)" \
		    "tests/functional/dedup: pre [] post [] tests [$DG]; tests/functional/dedup:Linux: pre [setup] post [cleanup] tests [$DGL]; 19 results"
		same "$t: the tests' files" "$(cd $W/$t/$Z/tests/functional &&
		    for f in $(for n in $DG $DGL setup cleanup; do echo dedup/$n; done) \
		    $(for e in $ONE; do p=${e#*:}; echo $p ${p%/*}/setup ${p%/*}/cleanup; done); do
			[ -x $f.ksh ] || echo $f
		    done | xargs)" ""
	done
	} 2>&1 | tee $L/trees.txt
	grep -q '^MISMATCH' $L/trees.txt && stop "the trees are not as expected"
	grep -qE '^(worktree [MPB]|M: P.s test files) rc=[1-9]' $L/trees.txt && stop "a worktree or the checkout failed"
fi
```

Expected:

```
worktree M rc=0 at 465d86a781
worktree P rc=0 at 43fda04502
worktree B rc=0 at 5c7c4b63ff
M: P's test files rc=0
ok: M against master
ok: M against P
ok: P as committed
ok: B as committed
ok: B against P
M: ddt.c ce520e168d, phys select x0, legacy branch x0, unique shortcut x1; new test -rwxr-xr-x 1d48547dcf, in common.run x1, in Makefile.am x1
ok: M: the dedup group
ok: M: the tests' files
P: ddt.c 9d20e652a3, phys select x1, legacy branch x0, unique shortcut x0; new test -rwxr-xr-x 1d48547dcf, in common.run x1, in Makefile.am x1
ok: P: the dedup group
ok: P: the tests' files
B: ddt.c 7d255b8eef, phys select x1, legacy branch x1, unique shortcut x1; new test -rwxr-xr-x 1d48547dcf, in common.run x1, in Makefile.am x1
ok: B: the dedup group
ok: B: the tests' files
```

(M is master's code with P's tests byte for byte: `M against P` is `ddt.c`
alone, and `B against P` too.) Anything else means stop.

Then build M, P and B, one after the other (10 to 15 minutes together; see
the rule about long commands):

```sh
. /var/log/zts-ddtl-1028/env.sh
build_tree M && build_tree P && build_tree B
```

Expected: `build M rc=0 wall=<s>s free=<n>G`, then the same for P and B, the
free space going down by about 1G per tree. A failed build prints its first
`error:` lines and `the build of <tree>: stop`.

Then check the three builds, point their runners' results at `$L/results`,
and record the three srcversions for the helpers:

```sh
. /var/log/zts-ddtl-1028/env.sh
if ! stopped; then
	{
	for t in M P B; do
		cd $W/$t
		same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
		same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1)" 3
		same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
		same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zdb \
		    scripts/zfs.sh scripts/zfs-tests.sh $TR; do [ -e $f ] || echo $f; done | xargs)" ""
		modinfo -F srcversion module/zfs.ko > $L/srcv-$t 2>&1
		echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
		    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:') (objtool's, about 83);" \
		    "srcversion $(cat $L/srcv-$t)"
		grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
		sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" $TR.in $TR
		same "$t results directory" "$(grep -cx "BASEDIR = '$L/results'" $TR)" 1
	done
	same "three different srcversions" "$(cat $L/srcv-M $L/srcv-P $L/srcv-B | grep -xE '[0-9A-F]+' | sort -u | wc -l)" 3
	same "M head" "$(git -C $W/M rev-parse HEAD)" $HM
	same "P head" "$(git -C $W/P rev-parse HEAD)" $HP
	same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
	same "M changes" "$(git -C $W/M diff HEAD --name-only | xargs)" "$(echo $TF $TR.in | xargs -n1 | LC_ALL=C sort | xargs)"
	same "P changes" "$(git -C $W/P diff HEAD --name-only | xargs)" $TR.in
	same "B changes" "$(git -C $W/B diff HEAD --name-only | xargs)" $TR.in
	same "M against P" "$(git -C $W/M diff $HP --name-only | xargs)" "$DDT $TR.in"
	same "B against P" "$(git -C $W/B diff $HP --name-only | xargs)" "$DDT $TR.in"
	echo "free after the builds: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (4G at least)"
	} 2>&1 | tee $L/build-checks.txt
	if grep -q '^MISMATCH' $L/build-checks.txt; then
		stop "the builds"
	elif [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 4 ]; then
		stop "less than 4G free after the builds"
	else
		printf 'SV_M=%s\nSV_P=%s\nSV_B=%s\n' "$(cat $L/srcv-M)" "$(cat $L/srcv-P)" "$(cat $L/srcv-B)" >> $L/env.sh
		tail -n 3 $L/env.sh
	fi
fi
```

Expected, for each tree: four `ok:` lines, `compiler warnings: 0` and about
83 other lines with its srcversion, and `ok: <tree> results directory`; then
`ok: three different srcversions`, eight more `ok:` lines (the runner's `.in`
file is the only change besides M's test files and `ddt.c`), the free space,
and the three lines `SV_M=<M's>`, `SV_P=<P's>`, `SV_B=<B's>`. No srcversion
of these commits is known from an earlier build on this VM: they are recorded
only, and the report gives all three. A compiler warning count other than 0
is not a stop: quote its lines.

## STEP 3: M: the dedup group, then the four tests

Load M's module stack (nothing is loaded, STEP 0, so the reload loads), then
run the dedup group on it (15 to 25 minutes):

```sh
. /var/log/zts-ddtl-1028/env.sh
switch_to M lm && zts md M -T dedup 19
```

Expected:

```
lm: load M rc=0, srcversion <SV_M>, pools []
kernel lm: 0 findings, 0 ZTS marks (expected 0)
before md: tree M, fd=/var/tmp/zts-ddtl-1028/files-md (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
md M -T dedup FAIL x1, PASS x18 (Test: x16, Test (Linux): x3, of 19) rc=1 wall=<s>s
  Missing util(s): <whatever it lists, or no such line>
  not passed: [<time>] Test: /var/tmp/zts-ddtl-1028/M/tests/zfs-tests/tests/functional/dedup/dedup_prune_scrub (run as root) [<mm:ss>] [FAIL]
  dedup_prune_scrub: scrubs x1, corruption shown x1, repaired by the scrub x0, resilvered x0, no data errors x0
  note: file1 block 0 at <offset> <asize>, file2 block 0 at <offset> <asize>
  first ERROR line: ERROR: same_blocks /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/a /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/b exited 1
  end line: ERROR: same_blocks /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/a /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/b exited 1
kernel md: 0 findings, 19 ZTS marks (expected 19)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-ddtl-1028/files-md holds: [] (expected [])
leftovers after md: as expected
```

That is the bug on master, at its check: the scrub ran (`scrubs x1`), the
corruption was there before it (`corruption shown x1`), and the side `b` was
not repaired. `rc=1` is zts-report's verdict on an unexpected failure,
recorded only. The two offsets of the note differ (`file1`'s block is not
`file2`'s). Another test that does not pass is listed as `not passed:` with
its first ERROR line and end line; it is a result, compared in STEP 6. A
`LEFTOVER` line, or `leftovers after md: NOT AS EXPECTED`, means the run left
something: the lines say what, and the step stops.

Then the four tests, one `-t` run each (10 to 20 minutes together):

```sh
. /var/log/zts-ddtl-1028/env.sh
for e in $ONE; do zts m${e%%:*} M -t ${e#*:} 3 || break; done
```

Expected, four times, with `m1` to `m4` and their paths:

```
before m1: tree M, fd=/var/tmp/zts-ddtl-1028/files-m1 (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
m1 M -t raidz/raidz_dedup_overrun PASS x3 (Test: x3, Test (Linux): x0, of 3) rc=0 wall=<s>s
  Missing util(s): <as in md>
kernel m1: 0 findings, 3 ZTS marks (expected 3)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-ddtl-1028/files-m1 holds: [] (expected [])
leftovers after m1: as expected
```

`m2` is `raidz/raidz_expand_008_pos`, `m3`
`block_cloning/block_cloning_resilver_free`, `m4`
`cli_root/zpool_prefetch/zpool_prefetch_001_pos`. A test that does not pass
is recorded with its lines; the loop stops only at a stop.

## STEP 4: P: the same runs

```sh
. /var/log/zts-ddtl-1028/env.sh
switch_to P lp && zts pd P -T dedup 19
```

Expected:

```
lp: load P rc=0, srcversion <SV_P>, pools []
kernel lp: 0 findings, 0 ZTS marks (expected 0)
before pd: tree P, fd=/var/tmp/zts-ddtl-1028/files-pd (tigerblue77 1777, holds []) pools=[] loops=[] mounts=[] rootdirs=[] zed=[] tunables as loaded
pd P -T dedup PASS x19 (Test: x16, Test (Linux): x3, of 19) rc=0 wall=<s>s
  Missing util(s): <as in md>
  dedup_prune_scrub: scrubs x2, corruption shown x1, repaired by the scrub x1, resilvered x1, no data errors x1
  note: file1 block 0 at <offset> <asize>, file2 block 0 at <offset> <asize>
  first ERROR line: none
  end line: Scrub and resilver cover dedup blocks whose DDT entry was pruned
kernel pd: 0 findings, 19 ZTS marks (expected 19)
  tunables: as loaded
  loop devices: []
  /var/tmp/zts-ddtl-1028/files-pd holds: [] (expected [])
leftovers after pd: as expected
```

The end line is the test's `log_pass` message; the `SUCCESS: check_pool_status
...` line comes before it. Then:

```sh
. /var/log/zts-ddtl-1028/env.sh
for e in $ONE; do zts p${e%%:*} P -t ${e#*:} 3 || break; done
```

Expected: as for `m1` to `m4`, with `p1` to `p4`, tree `P` and `files-p<n>`.

## STEP 5: B: the same runs

```sh
. /var/log/zts-ddtl-1028/env.sh
switch_to B lb && zts bd B -T dedup 19
```

Expected: as for `pd`, with `lb`, `bd`, tree `B`, `<SV_B>` and `files-bd`:
`PASS x19`, and the same `dedup_prune_scrub` lines (`scrubs x2`, ..., end line
`Scrub and resilver cover dedup blocks whose DDT entry was pruned`). Then:

```sh
. /var/log/zts-ddtl-1028/env.sh
for e in $ONE; do zts b${e%%:*} B -t ${e#*:} 3 || break; done
```

Expected: as for `m1` to `m4`, with `b1` to `b4`, tree `B` and `files-b<n>`.

## STEP 6: M, P and B, result by result

```sh
. /var/log/zts-ddtl-1028/env.sh
{
for t in M P B; do
	k=$(echo $t | tr MPB mpb)
	for r in d 1 2 3 4; do
		[ -f $L/$k$r-tests.txt ] || continue
		sed -E 's|^.*/tests/functional/||; s| [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\].*$| \1|; s|[.]ksh | |' \
		    $L/$k$r-tests.txt | sed "s|^|$r:|"
	done | LC_ALL=C sort > $L/res-$t.txt
done
LC_ALL=C join -a 1 -a 2 -e none -o 0,1.2,2.2 $L/res-M.txt $L/res-P.txt |
    LC_ALL=C join -a 1 -a 2 -e none -o 0,1.2,1.3,2.2 - $L/res-B.txt |
    awk '{
	s = ($2 == $3 && $3 == $4) ? "same" : "DIFFERS"
	if ($1 == "d:dedup/dedup_prune_scrub" && $2 == "FAIL" && $3 == "PASS" && $4 == "PASS")
		s = "expected"
	printf "%-50s M %-5s P %-5s B %-5s %s\n", $1, $2, $3, $4, s
    }' > $L/compare-rows.txt
cat $L/compare-rows.txt
echo "results: $(wc -l < $L/compare-rows.txt), the same on M, P and B: $(grep -c ' same$' $L/compare-rows.txt)," \
    "the expected difference: $(grep -c ' expected$' $L/compare-rows.txt), other differences: $(grep -c ' DIFFERS$' $L/compare-rows.txt)"
for k in md pd bd; do
	sed "s/^ /$k:/" $L/dps-$k.txt 2>&1
done
for t in M P B; do
	echo "$t srcversion $(cat $L/srcv-$t)"
done
} 2>&1 | tee $L/compare.txt
```

Expected: 31 rows (`1:` to `4:`, each test with its directory's `setup` and
`cleanup`, then the 19 `d:` rows), every one `M PASS  P PASS  B PASS  same`
except

```
d:dedup/dedup_prune_scrub                          M FAIL  P PASS  B PASS  expected
```

then `results: 31, the same on M, P and B: 30, the expected difference: 1,
other differences: 0`, the three `dedup_prune_scrub` digests (`md:` as in
STEP 3, `pd:` and `bd:` as in STEP 4), and the three srcversions.

A row that is not PASS but the same on all three trees is recorded: the three
trees differ in `ddt_class_contains()` only, so it is master's or this VM's. A
`DIFFERS` row is reported with each run's `not passed:` lines. Between P and B
it can only be a flaky test: none of the tests here scrubs a legacy table, so
B runs P's code in all of them. Between M and P or B, outside
`dedup_prune_scrub`, it is a finding about P's change, or a flaky test if
neither P nor B differs from the other.

## STEP 7: a scrub over a legacy DDT, P against B

B is loaded (STEP 5). Make the pool (see "STEP 7's measurement" at the top),
check that its table is a legacy one with the expected classes, and export it:

```sh
. /var/log/zts-ddtl-1028/env.sh
if ! stopped; then
	{
	same "loaded tree" "$(cat $L/loaded 2>&1)" B
	same "loaded srcversion" "$(cat /sys/module/zfs/srcversion 2>&1)" "$SV_B"
	echo "free before: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (2G at least);" \
	    "$(free -m | awk '/^Mem:/ { print "memory: total " $2 " MiB, available " $7 " MiB" }')"
	since=$(date '+%F %T')
	mkdir $W/meas && truncate -s 2G $W/meas/vdev; echo "vdev rc=$?"
	nice -n 10 ionice -c3 sudo -n $W/B/zpool create -o cachefile=none -o feature@fast_dedup=disabled \
	    -O mountpoint=$W/meas/mnt -O dedup=sha256 -O compression=off -O recordsize=4k -O atime=off \
	    $MP $W/meas/vdev; echo "create rc=$?"
	for f in u d1; do
		nice -n 10 ionice -c3 sudo -n dd if=/dev/urandom of=$W/meas/mnt/$f bs=1M count=256 \
		    iflag=fullblock status=none
		echo "$f rc=$?"
	done
	sudo -n $W/B/zpool sync $MP; echo "sync rc=$?"
	nice -n 10 ionice -c3 sudo -n dd if=$W/meas/mnt/d1 of=$W/meas/mnt/d2 bs=1M status=none; echo "d2 rc=$?"
	sudo -n $W/B/zpool sync $MP; echo "sync rc=$?"
	same "d2 is d1" "$(sudo -n cmp $W/meas/mnt/d1 $W/meas/mnt/d2 2>&1; echo $?)" 0
	same "feature@fast_dedup" "$(sudo -n $W/B/zpool get -H -o value feature@fast_dedup $MP 2>&1)" disabled
	same "DDT entries" "$(sudo -n $W/B/zpool status -D $MP 2>&1 | grep -o 'DDT entries [0-9]*')" "DDT entries 131072"
	echo "dedupratio $(sudo -n $W/B/zpool list -H -o dedupratio $MP 2>&1) (1.50x)"
	sudo -n $W/B/zpool export $MP; echo "export rc=$?"
	sudo -n $W/B/zdb -e -p $W/meas -D $MP > $L/meas-zdb.txt 2>&1; echo "zdb rc=$?"
	grep -E '^DDT-|^dedup = ' $L/meas-zdb.txt
	same "a legacy table" "$(grep -c '^DDT-sha256: version=0 \[LEGACY\]' $L/meas-zdb.txt)" 1
	same "its classes" "$(grep -E '^DDT-sha256-zap-[a-z]+: ' $L/meas-zdb.txt | sed -E 's/: .*entries=/ /' | xargs)" \
	    "DDT-sha256-zap-duplicate 65536 DDT-sha256-zap-unique 65536"
	echo "W/meas holds: $(LC_ALL=C ls -A $W/meas | xargs)"
	kern_check meas "$since" 0
	} 2>&1 | tee $L/meas-setup.txt
	grep -qE '^MISMATCH|^(vdev|create|u|d1|sync|d2|export|zdb) rc=[1-9]' $L/meas-setup.txt && stop "STEP 7's pool"
fi
```

Expected:

```
ok: loaded tree
ok: loaded srcversion
free before: <n>G (2G at least); memory: total <n> MiB, available <n> MiB
vdev rc=0
create rc=0
u rc=0
d1 rc=0
sync rc=0
d2 rc=0
sync rc=0
ok: d2 is d1
ok: feature@fast_dedup
ok: DDT entries
dedupratio 1.50x (1.50x)
export rc=0
zdb rc=0
DDT-sha256: version=0 [LEGACY]; flags=0x00; rootobj=<n>
DDT-sha256-zap-duplicate: dspace=<n>; mspace=<n>; entries=65536
DDT-sha256-zap-unique: dspace=<n>; mspace=<n>; entries=65536
dedup = 1.50, compress = 1.00, copies = 1.00, dedup * compress / copies = 1.50
ok: a legacy table
ok: its classes
W/meas holds: mnt vdev
kernel meas: 0 findings, 0 ZTS marks (expected 0)
```

(`mnt` is the pool's mount point, empty and root's: `zpool export` leaves a
mount point that was set explicitly, and the next import mounts on it again.)
Less than 2G free, or any `MISMATCH`, means stop. Then the six rounds, P
first, alternating (5 to 10 minutes; see the rule about long commands):

```sh
. /var/log/zts-ddtl-1028/env.sh
for r in 1:P 2:B 3:P 4:B 5:P 6:B; do
	meas_round ${r%%:*} ${r#*:} || break
done
{
[ -s $L/meas-rows.txt ] && meas_summary
t=$(cat $L/loaded 2>/dev/null)
echo "after the rounds: loaded ${t:-none}, pools [$([ -n "$t" ] && sudo -n $W/$t/zpool list -H -o name 2>&1)]," \
    "mounts [$(grep -E ' zfs |testpool' /proc/mounts)], W/meas holds [$(LC_ALL=C ls -A $W/meas | xargs)]"
} 2>&1 | tee $L/meas-summary.txt
```

Expected, for each round (`l<n>` is its load):

```
l1: load P rc=0, srcversion <SV_P>, pools []
kernel l1: 0 findings, 0 ZTS marks (expected 0)
r1 P: import <s>s, scrub <s>s (scrub repaired 0B in <hh:mm:ss> with 0 errors); traversal <n> txgs <t> ms, visited <v>, in ddt 131072; walk 65536 entries, class_max 1; issued <n> blocks in <t> ms over <n> txgs
r1 P: import rc=0, scrub rc=0, export rc=0
kernel r1: 0 findings, 0 ZTS marks (expected 0)
```

then `l2`/`r2` with B, and so on to `r6`; then

```
P: 3 rounds; scrub min <s> mean <s> max <s> s; traversal min <t> mean <t> max <t> ms; visited <v> to <v>
B: 3 rounds; scrub min <s> mean <s> max <s> s; traversal min <t> mean <t> max <t> ms; visited <v> to <v>
B against P, means: scrub <+/-x>%, traversal <+/-y>%
counts (in ddt, walk entries, class_max) over 6 rounds: 131072 65536 1 (1 distinct)
after the rounds: loaded B, pools [], mounts [], W/meas holds [mnt vdev]
```

The counts are the result that is checked: one distinct set over the six
rounds, with 131072 skipped block pointers and a walk of 65536 entries in
class 1, and `0 errors` in every round. The visited counts are a little over
196608 and may differ by a few blocks between rounds (the pool's metadata):
recorded only. The times are recorded as they come, with their minimum and
maximum; their reading is left to whoever reads the report. A round whose
import, scrub or export fails stops, after its export was tried.

## STEP 8: unload, release, report, cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the whole task's kernel check and the
unload; the module stays loaded on `HELD` or `PANIC`:

```sh
. /var/log/zts-ddtl-1028/env.sh
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $L/kern-all.txt | grep -vcE "$KIGN") findings," \
    "$(grep -c 'ZTS run ' $L/kern-all.txt) ZTS marks"
grep -iE "$KRE" $L/kern-all.txt | grep -vE "$KIGN" | head -n 40
echo "kernel, this boot: $(sudo -n journalctl -k -b --no-pager -q | grep -iE "$KRE" | grep -vcE "$KIGN")"
t=$(cat $L/loaded 2>/dev/null)
if [ -e $L/HELD ] || [ -e $L/PANIC ]; then
	echo "module left loaded: $(cd $L && ls HELD PANIC 2>/dev/null | xargs)"
elif [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(sv_of $t)" ]; then
	p=$(sudo -n $W/$t/zpool list -H -o name 2>&1)
	if [ -n "$p$(grep -E ' zfs |testpool' /proc/mounts)" ]; then
		echo "a pool or a mount is left ($p): module left loaded"
	elif [ -e /var/run/zed.pid ]; then
		echo "/var/run/zed.pid exists: module left loaded"
	else
		(cd $W/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u); echo "unload $t rc=$?"
	fi
else
	echo "no module of this task is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
echo "loop devices: [$(loops)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings, 93 ZTS marks` (31 per tree; fewer
after a stop), no kernel line, the boot's count equal to the STEP 0 baseline,
`unload B rc=0`, `lsmod: []`, `loop devices: []`, a 0-byte or absent
`zpool.cache` (its time may have moved: the tests' pools were recorded in it
while they existed), no `vdev_id.conf`.

Then the files directories, each empty after its run:

```sh
. /var/log/zts-ddtl-1028/env.sh
{
for d in $W/files-*; do
	[ -d "$d" ] || continue
	rmdir $d; echo "rmdir ${d##*/} rc=$?"
done
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee -a $L/end.txt
```

Expected: fifteen `rmdir files-<label> rc=0` (`files-b1` to `files-pd`),
`W holds: B M P meas repo`, the free space. After a stop, a files directory
that is not empty stays (`rmdir` fails on it): report it, remove nothing else
by hand.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-ddtl-1028/env.sh
{
echo "VM 107: scrub over pruned dedup blocks, M 465d86a781, P 43fda04502, B 5c7c4b63ff (t/VM-TESTS-107-ddtl.md)"
for s in record fix trees build build-checks runs compare meas-setup meas meas-summary end; do
	echo
	echo "== $s.txt"
	cat $L/$s.txt 2>&1
done
echo
echo "== summary"
for t in M P B; do
	echo "$t srcversion $(cat $L/srcv-$t 2>/dev/null)"
done
for k in m p b; do
	for r in d 1 2 3 4; do
		grep -E "^$k$r [MPB] -[Tt] |^kernel $k$r:|^leftovers after $k$r:" $L/runs.txt
		[ $r = d ] && cat $L/dps-${k}d.txt 2>/dev/null
	done
done
grep '^results: ' $L/compare.txt
cat $L/meas-summary.txt 2>/dev/null
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, `uname -r`, the
memory line, the free space before and after the release, the release lines
of `/var/tmp/vm-task-107n`, the doc checkout line and its sha256, the kernel
baseline, the `bzcat` line), a `verdict:` line naming the case of "What the
results mean" that applies, and a `deviations:` line with every deviation
from these steps and its reason (`none` if there was none). Then
`cat /var/log/zts-ddtl-1028/REPORT.txt`. Expected summary:

```
M srcversion <SV_M>
P srcversion <SV_P>
B srcversion <SV_B>
md M -T dedup FAIL x1, PASS x18 (Test: x16, Test (Linux): x3, of 19) rc=1 wall=<s>s
kernel md: 0 findings, 19 ZTS marks (expected 19)
leftovers after md: as expected
  dedup_prune_scrub: scrubs x1, corruption shown x1, repaired by the scrub x0, resilvered x0, no data errors x0
  note: file1 block 0 at <offset> <asize>, file2 block 0 at <offset> <asize>
  first ERROR line: ERROR: same_blocks /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/a /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/b exited 1
  end line: ERROR: same_blocks /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/a /var/tmp/zts-ddtl-1028/files-md/dedup_prune_scrub/b exited 1
m1 M -t raidz/raidz_dedup_overrun PASS x3 (Test: x3, Test (Linux): x0, of 3) rc=0 wall=<s>s
kernel m1: 0 findings, 3 ZTS marks (expected 3)
leftovers after m1: as expected
<the same three lines for m2, m3, m4>
pd P -T dedup PASS x19 (Test: x16, Test (Linux): x3, of 19) rc=0 wall=<s>s
kernel pd: 0 findings, 19 ZTS marks (expected 19)
leftovers after pd: as expected
  dedup_prune_scrub: scrubs x2, corruption shown x1, repaired by the scrub x1, resilvered x1, no data errors x1
  note: file1 block 0 at <offset> <asize>, file2 block 0 at <offset> <asize>
  first ERROR line: none
  end line: Scrub and resilver cover dedup blocks whose DDT entry was pruned
<p1 to p4 as m1 to m4>
<bd as pd, b1 to b4 as m1 to m4>
results: 31, the same on M, P and B: 30, the expected difference: 1, other differences: 0
P: 3 rounds; scrub min <s> mean <s> max <s> s; traversal min <t> mean <t> max <t> ms; visited <v> to <v>
B: 3 rounds; scrub min <s> mean <s> max <s> s; traversal min <t> mean <t> max <t> ms; visited <v> to <v>
B against P, means: scrub <+/-x>%, traversal <+/-y>%
counts (in ddt, walk entries, class_max) over 6 rounds: 131072 65536 1 (1 distinct)
after the rounds: loaded B, pools [], mounts [], W/meas holds [mnt vdev]
flags: STOP [] HELD [] PANIC []
verdict: as expected (first case)
deviations: none
```

Then, as the very last command, and only if the unload block printed
`unload B rc=0` with `lsmod: []` and no step stopped, remove this task's
directories and the doc checkout (the doc checkout only if it is still the
clean checkout STEP 1 recorded, holding this document, and nothing else).
`$L` sits in root's `/var/log`: its contents go with `rm -rf`, the then empty
directory with `sudo -n rmdir`:

```sh
cd /
D=/var/tmp/vm-task-107t
if [ -d $D ] && [ ! -L $D ] && [ "$(stat -c %U $D)" = tigerblue77 ] && [ -d $D/.git ] && [ ! -L $D/.git ] &&
    [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
    [ -s /var/log/zts-ddtl-1028/doc-head.txt ] &&
    [ "$(git -C $D rev-parse HEAD 2>/dev/null)" = "$(cat /var/log/zts-ddtl-1028/doc-head.txt)" ] &&
    git -C $D ls-files --error-unmatch t/VM-TESTS-107-ddtl.md > /dev/null 2>&1 &&
    [ "$(sha256sum < $D/t/VM-TESTS-107-ddtl.md | cut -d' ' -f1)" = "$(cat /var/log/zts-ddtl-1028/doc-sha.txt 2>/dev/null)" ] &&
    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
    [ "$(LC_ALL=C ls -A $D | grep -vx .git | xargs)" = "$(git -C $D ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
    ! grep -q " ${D}[ /]" /proc/mounts; then
	X=$D
else
	X=""; echo "$D kept: not only the clean doc checkout STEP 1 recorded"
fi
rm -rf /var/tmp/zts-ddtl-1028 $X /var/log/zts-ddtl-1028/*; echo "rm rc=$?"
sudo -n rmdir /var/log/zts-ddtl-1028; echo "rmdir rc=$?"
ls -d /var/tmp/zts-ddtl-1028 /var/log/zts-ddtl-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, or a step stopped, run nothing of this block:
keep everything and say so. If your permission system refuses this `rm -rf`
or this `rmdir`, leave what is left and say so; do not delete it any other
way. Then reply with all of `REPORT.txt` as printed, and the result of the
last block: nothing runs after the reply.

## What the results mean

- **As expected** (`dedup_prune_scrub` FAIL on M at `same_blocks ... a ... b`
  after `scrubs x1`, PASS on P and B with `scrubs x2, corruption shown x1,
  repaired by the scrub x1, resilvered x1, no data errors x1` and the
  `log_pass` end line; `compare.txt` 30 of 31 the same and the one expected
  difference; STEP 7 one distinct set of counts with `in ddt` 131072 and the
  walk 65536 entries of class 1, 0 errors in each round; no kernel finding; a
  clean unload): on a kernel, master's scrub skips a pruned dedup block whose
  key has a new entry, and leaves its damage; P scrubs and resilvers it; B
  does the same on flat tables, and on a legacy table decides as P does while
  using the key alone. The times are what they are: B's legacy branch
  avoids per-block work, not ZAP lookups, so a small or no difference is a
  possible outcome and not a failure.
- **`dedup_prune_scrub` passes on M**: the bug did not show here. Check that
  STEP 2's `M: ddt.c ce520e168d, phys select x0` held, and report M's digest:
  `repaired by the scrub x1` on M would mean the scrub read `file1`'s blocks
  anyway.
- **It fails on M at another check**: report its first ERROR line and end
  line. Before the first scrub (`scrubs x0`), the test's own setup failed on
  master (the prune, the copies), which says nothing about the fix.
- **It fails on P or on B**: the fix does not hold in the kernel, or the test
  is flaky; report the digest and the run's lines. On B only, since B runs
  P's code on the test's flat table, a flaky test is the likely reading.
- **Another row `DIFFERS`**: see STEP 6; report both runs' lines.
- **A row not PASS on all three trees**: master's or this VM's, recorded.
- **STEP 7's counts differ between P and B**: B's legacy branch decides
  otherwise than P's lookup on a legacy table: a finding. Report the rows and
  each round's `scan visited` and `scanned ... ddt entries` lines from
  `$L/r<n>-dbgmsg.txt`.
- **STEP 7's counts differ between rounds of one tree**: the scan did not
  visit the same blocks every time (a restarted scan, say); report it, and
  read the times with that in mind.
- **`LEFTOVER tunables`**: a test of the run did not restore a tunable;
  report the line.
- **Any kernel finding**: the line and the run are the result. Stop.

Estimated duration: about 2 hours 30 minutes (checks, release, fetch and
worktrees 10, the builds 15, each tree's dedup group 15 to 25 and its four
tests 10 to 20, the comparison 1, STEP 7 10, the end 5).
