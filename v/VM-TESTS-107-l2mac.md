# VM 107: L2ARC copies of encrypted blocks whose MAC differs, on master and on the fix

These instructions are for the session on VM 107 (Debian 13). They give
kernel evidence for the fix on the fork branch `l2arc-encrypted-mac-mismatch`
of https://github.com/tigerblue77/zfs, commit `288026c17a` "L2ARC: Do not
write encrypted buffers whose MAC differs", whose parent is upstream master
`5ebd13aa9a`. The session builds three trees in its own directories, writes
a pool with zfs-2.4.4, reads copies of it through an L2ARC device on a
release build of master and on a debug build of the fix, runs a control on
the fix with a pool the fix wrote itself, cleans up, and replies with
`REPORT.txt`. This document is read from a checkout of the branch
`vm-tasks-1026` of https://github.com/tigerblue77/zfs in
`/var/tmp/vm-task-107v` (the doc checkout, `DOC` below); the last command of
STEP 8 removes it with the task's own directories.

| Tree | Path | Commit | Build | Used for |
| --- | --- | --- | --- | --- |
| O | `/var/tmp/zts-l2mac-1028/O` | `71a9f95786`, the tag `zfs-2.4.4` (tag object `f75f3256a0`); bundled zstd 1.4.5 | release (`./configure`) | writing the pool `wo`, nothing else: no cache device is ever added on O |
| MR | `/var/tmp/zts-l2mac-1028/MR` | `5ebd13aa9a` "ZTS: sync checksum test data before fault injection": upstream master; bundled zstd 1.5.7 | release (`./configure`) | reading `wo`: the bug, without its assertion |
| FD | `/var/tmp/zts-l2mac-1028/FD` | `288026c17a` "L2ARC: Do not write encrypted buffers whose MAC differs": a child of MR | debug (`./configure --enable-debug`) | reading `wo`; then writing the pool `wf` and reading it (the control) |

MR and FD differ in `module/zfs/arc.c` only, in `l2arc_apply_transforms()`;
STEP 2 checks it. The bundled LZ4 is the same in all three trees (STEP 2
checks it too), so only zstd blocks recompress differently here.

**The bug.** With `zfs_compressed_arc_enabled=0`, the ARC keeps a block read
with its key loaded decrypted and uncompressed. To write such a block to an
L2ARC device, `l2arc_apply_transforms()` compresses it again (the block's
compression and level) and encrypts the result with the block's own salt and
IV, so that the copy holds the bytes on disk. It then asserts, at line 9620
of `arc.c` on master:

```
		/* assert that the MAC we got here matches the one we saved */
		ASSERT0(memcmp(mac, hdr->b_crypt_hdr.b_mac, ZIO_DATA_MAC_LEN));
```

That holds only if the running compressor gives the bytes of the one that
wrote the block. zfs-2.4.4 bundles zstd 1.4.5 and master zstd 1.5.7, and the
header ZFS puts in front of a zstd block also carries the compressor's
version. For the 64 KiB zstd blocks of this task, written by zfs-2.4.4,
master's recompression differs in every block, still fits in the block's
psize (20480 bytes), and so gives another MAC. A debug build of master then
panics the L2ARC feed thread (`VERIFY0(memcmp(mac, hdr->b_crypt_hdr.b_mac,
ZIO_DATA_MAC_LEN)) failed (-103)`, seen in userspace with libzpool). A release
build has no such check and writes the copy. Read back, the copy fails its
checksum (`l2_cksum_bad` in arcstats), the read is retried from the pool, and
the retry fails too, so the read returns EIO: the retry is issued with the
size of the L2ARC copy, a separate problem that the open upstream pull request
https://github.com/openzfs/zfs/pull/19300 addresses. The data on disk is
intact.

**The fix** (FD) returns `EIO` instead of asserting when the MACs differ, as
the function already does when the recompressed data does not fit in the
block's psize. The caller clears `ARC_FLAG_L2CACHE` and moves on: the block is
not written to the L2ARC, and no L2ARC accounting has been done for it. A
buffer that was not recompressed keeps an assertion:

```
		if (memcmp(mac, hdr->b_crypt_hdr.b_mac,
		    ZIO_DATA_MAC_LEN) != 0) {
			ASSERT3P(to_write, ==, cabd);
			ret = SET_ERROR(EIO);
			goto error;
		}
```

**Never a debug build of master.** Its assertion would panic the feed thread
of this VM, which then holds the pool's L2ARC configuration lock for good:
nothing could export the pool or unload the module, and only a reboot would
clear it. MR is therefore built without `--enable-debug`, and STEP 2 checks,
before anything is loaded, that MR's `zfs.ko` holds none of the assertion
texts a debug build has.

What the run establishes:

1. **MR, reading `wo`** (run `mo`): the data blocks of `wo` are written to
   the L2ARC (`l2_bufc_data_asize` > 0 after the feed); read again from it,
   they fail their checksum (`l2_cksum_bad` grows by the number of blocks)
   and their reads return EIO; no kernel message; after an export and an
   import, every block reads correctly from the pool.
2. **FD, reading `wo`** (run `fo`): no assertion and no kernel message; no
   data block in the L2ARC (`l2_bufc_data_asize` 0 after the feed,
   `l2_write_bytes` a few tens of KiB of metadata); every read correct;
   `l2_cksum_bad` 0.
3. **FD, reading `wf`**, a pool FD wrote itself (run `ff`, the control): FD
   recompresses its own blocks to the same bytes, so its data blocks are
   written to the L2ARC and read back from it (`l2_hits` grows by at least
   their number), every read correct, `l2_cksum_bad` 0: the fix does not stop
   the normal caching of encrypted blocks.
4. No kernel finding anywhere, and a clean unload at the end.

**Disk space.** A built tree takes about 1 GB here (the earlier three-tree
tasks on this VM), the clone a few hundred MB, and each pool image and cache
device is a sparse file of which a few MB are written. With about 8.5 GB free
on `/var/tmp`, the three trees fit together: all three are built in STEP 2,
before anything is loaded, and none is replaced. STEP 0 stops below 6 GB
free, STEP 2 below 2 GB after the builds.

## The pool, the reads, and how the ARC is emptied between them

The data and the pool are those of the userspace reproduction that found the
bug (a libzpool program, not part of the fix):

- **The data**: 64 blocks of 64 KiB of compressible text, block `b` filled by
  the program's `fill_block()` (a xorshift over a vocabulary of 64 words).
  `$L/gen.py` (STEP 1) writes the same 4 MiB; STEP 3 checks its output by
  its sha256, `6a3f8153...`, and the sha256 of its 64 block sums,
  `a939caea...`, both computed with the program's own `fill_block()`.
- **The pool** `l2mac`: one 512 MiB sparse file vdev, `ashift=12`,
  `cachefile=none`; the dataset `l2mac/enc` with `encryption=aes-256-gcm`,
  `keyformat=raw` (the program's 32-byte test key, in `$W/key`),
  `compression=zstd` (level 3), `recordsize=64K`, mounted on `$W/mnt`; the
  data in one file, `$W/mnt/data`, written with `dd bs=64K`. The program used
  a non-ZPL dataset; the blocks, their compression and their encryption are
  the same. Every block is stored compressed, psize 20480.
- **The blocks are 64 KiB** so that zstd's early abort (for blocks of 128 KiB
  and more) stays out of the way: for 128 KiB blocks of the same data, it
  rejects the recompression within the psize, and the existing "does not
  fit" path skips them on master too.

A reading run (`l2run`, STEP 1) on the loaded tree:

| Point | What | Why |
| --- | --- | --- |
| | a fresh copy of the pool image (`cp --sparse=always` of its vdev) in `$W/<run>`, and a 256 MiB sparse file `cache` | each run starts from the image as it was written, and its own cache device |
| | `zfs_compressed_arc_enabled=0`, `l2arc_noprefetch=0`, `dbuf_cache_max_bytes=262144`, `zfs_prefetch_disable=1`, written to `/sys/module/zfs/parameters` before the import | the program's three tunables: the ARC keeps the blocks uncompressed, prefetched buffers stay eligible, and the dbuf cache gives the blocks back to the ARC lists, where the feed finds them. The fourth stands for the program's `DMU_READ_NO_PREFETCH`: no prefetch reads a block before its own read does |
| | import (`-N`, `cachefile=none`), `zfs load-key`, `zfs mount`, `zpool add l2mac cache <run>/cache` | |
| s0, s1 | read 1: each of the 64 blocks with its own `dd bs=64K skip=<i> count=1`, its sha256 compared with the expected one | the blocks enter the ARC, decrypted and uncompressed |
| s2 | the feed: arcstats polled once a second until at least 6 feeds have run, every write sent is done and `l2_write_bytes` has not moved for 5 polls, 120 polls at most | the feed thread writes what it can |
| s3 | `zfs unmount`, then `zinject -a` | the ARC emptied, the L2ARC kept (below) |
| s4 | `zfs mount`, read 2 | blocks with an L2ARC copy are read from it |
| s5 | export, import, key, mount, read 3, export | from the pool: the data on disk |

At each point the arcstats are saved (`$L/<run>-s<n>.as`); the digest of a
run gives the L2ARC sizes after the feed and what changed between points.
`l2_hits` counts reads sent to the L2ARC, good or bad; `l2_cksum_bad` those
whose copy failed its checksum. `l2_bufc_data_asize` is the space data
buffers (the file's blocks) hold in the L2ARC now, `l2_bufc_metadata_asize`
that of metadata. At the flush, `evict_l2_cached` counts the bytes of buffers
evicted with an L2ARC copy, `evict_l2_eligible` those that could have had one,
`evict_l2_ineligible` those that could not (`ARC_FLAG_L2CACHE` cleared, as the
fix does for a block it skips).

**How the ARC is emptied between read 1 and read 2, and why that way.** Read
2 must find the blocks gone from the ARC but still in the L2ARC:

- **Not by `zpool export` and `zpool import`.** The export drops the L2ARC's
  contents with the ARC, and persistent L2ARC (`l2arc_rebuild_enabled=1`)
  restores at import only buffers recorded in committed log blocks: no log
  block is written to a cache device smaller than
  `l2arc_rebuild_blocks_min_l2size` (1 GiB), and on a larger one a log block
  is committed only once full. A second read after an export would go to
  the pool. Read 3 is exactly that, and it shows it.
- **Not by `drop_caches`.** The ARC's shrinker does not take the ARC below
  `zfs_arc_min` (by default 1/32 of the memory, some 250 MB here), far above
  the few MB this pool puts in it, and ZFS reads do not go through the page
  cache: the blocks would stay in the ARC.
- **By `zinject -a`**, from the loaded tree: it calls `arc_flush(NULL,
  B_FALSE)`, which evicts every buffer no one holds, and a buffer with an
  L2ARC copy keeps only that copy (the `l2c_only` state). The userspace
  program made the same call for its pool (`arc_flush(spa, B_FALSE)`). The
  dataset is unmounted first, as the program released its objset: that
  releases the dbufs still holding the last blocks read (the dbuf cache keeps
  up to 256 KiB, the last three or four blocks), and keeps the key loaded.
  The feed is over before the unmount: a block being written to the L2ARC
  could not be evicted, and once the dataset is unmounted, a new copy could
  no longer be encrypted.

Expected, then, per run:

| Run | data in the L2ARC after the feed | read 2 | read 3 |
| --- | --- | --- | --- |
| `mo` (MR reads `wo`) | `N` x 20480 bytes, `N` about 60 (up to 64: the blocks the dbuf cache still holds when the feed runs are not written) | EIO for those `N` blocks, ok for the others; `l2_cksum_bad` +`N`, `l2_hits` at least `N` | ok x64 |
| `fo` (FD reads `wo`) | 0 (`evict_l2_ineligible` at the flush: the `N` blocks the fix skipped) | ok x64 from the pool; `l2_cksum_bad` +0 | ok x64 |
| `ff` (FD reads `wf`) | `N` x 20480 bytes | ok x64, `l2_hits` at least `N`; `l2_cksum_bad` +0 | ok x64 |

## Rules

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order. Use `sudo -n` only where it is written.
- A production hypervisor sits underneath: the builds run under
  `nice -n 10 ionice -c3` with `make -j8`, never more, one tree at a time;
  the module loads and the pool commands go through `nice -n 10 ionice -c3`
  too where the helpers say so, one thing at a time, nothing beside them.
- **Never load a debug build of master** (see above). MR is the only master
  build, a release one, and STEP 2 stops before any load if its `zfs.ko`
  holds an assertion text. Never add a cache device on O.
- Never reboot. Never run `zfs-tests.sh` or `scripts/zfs-helpers.sh`.
  Install, push and post nothing. Rebuild nothing once STEP 2 has built the
  trees. `scripts/zfs.sh -r` (load) and `-u` (unload) run only here, on this
  VM, and only in the blocks and helpers that say so.
- This task's own directories are `/var/tmp/zts-l2mac-1028` (`$W`) and
  `/var/log/zts-l2mac-1028` (`$L`). Outside them, the only change is STEP 8's
  removal of the doc checkout of this task. `/var/tmp/vm-task-107t`, the doc
  checkout of the last task, is already gone: STEP 0 only records it. Never
  touch another `/var/tmp/zts-*` or `/var/log/zts-*` directory, another
  `/var/tmp/vm-task-*` checkout or `/var/tmp/test_results`. Never create,
  edit or remove `/etc/zfs/zpool.cache` or `/etc/zfs/vdev_id.conf` (every
  pool of this task uses `cachefile=none`).
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Remove by hand only what the final block of STEP 8
  removes.
- Never run a `zpool`, `zfs`, `zdb` or `zinject` command of a tree other than
  the one whose module is loaded, or while none is. Never print the
  environment (`env`, `printenv`, `set`, `/proc/*/environ`) or the key file
  `$W/key`.
- From STEP 1 on, every block starts by sourcing
  `/var/log/zts-l2mac-1028/env.sh`.
- Stop at any value the comments or Expected lines do not allow, unless the
  text says it is recorded only. After a line ending in `stop`, run only
  STEP 8; the helpers refuse to run once a step has stopped. A read that
  fails is a result, recorded, not a stop. Clean, repair and retry nothing
  by hand. If your permission system refuses a command, report it and stop.
- **A kernel finding** (panic, oops, VERIFY, ASSERT, BUG, hung task) stops
  at once, and nothing touches the pool or the module after it: the helpers
  check the kernel log after the feed and after read 2, before the next pool
  command, and STEP 8 then runs no `zpool` command and leaves the module
  loaded. If a block hangs well past its stated time, interrupt it if your
  tool allows, run `touch /var/log/zts-l2mac-1028/HELD`, then only STEP 8's
  first block (with `HELD` set it runs no pool command), and report. Never
  reboot.
- The builds take about 15 minutes together, each run 1 to 3 minutes. If
  your shell tool cannot wait that long in the foreground, start that one
  block with the tool's own background mode and wait for it to exit. Never
  use `&` or `nohup`, and run nothing meanwhile.
- Your session cannot run anything after its final reply. Everything, cleanup
  included, happens before it; the reply is the whole of `REPORT.txt` and the
  result of the last command.

What earlier VM runs taught, and the blocks already handle: `modinfo` is in
`/usr/sbin`, not in `tigerblue77`'s PATH, so every block puts `/usr/sbin:/sbin`
first; this VM's umask has been 0002 and 0022, and `env.sh` sets 022; `/var/log`
directories are made with `sudo -n mkdir` and then given to `tigerblue77`.
`configure --enable-debug` writes 3 `#define ZFS_DEBUG 1` lines in
`zfs_config.h`; a release `configure` writes 1 (inside `#ifdef
CONFIG_ZFS_DEBUG`, unused by these builds) and 2 `/* #undef ZFS_DEBUG */`.
Compiler warnings are counted as `:[0-9]+:[0-9]+: warning:`, and objtool's
about 83 harmless `warning:` lines per tree are not counted. Kernel messages
are read from `journalctl -k --since`. `zfs.sh -r` and `-u` signal the pid
`/var/run/zed.pid` names, so they never run while it exists. `zpool list -H`
prints nothing when there is no pool. The doc checkout may be a clone of
`vm-tasks-1026` or a `git init` with a fetch by commit (a detached HEAD and
no `origin`): STEP 0 accepts both shapes, and so does the final removal.

## STEP 0: the VM free, the space, the previous doc checkout gone

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-l2mac-1028 L=/var/log/zts-l2mac-1028 DOC=/var/tmp/vm-task-107v
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
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db|[z]inject|[f]io '   # nothing
pgrep -ax 'zed|lt-zed'                 # nothing
ls -l /var/run/zed.pid 2>&1            # No such file or directory: zfs.sh -r and -u signal the pid it names
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n losetup -l -n --raw -O NAME,BACK-FILE   # nothing
sudo -n dmsetup ls 2>&1                # No devices found
grep -E ' zfs |testpool' /proc/mounts  # nothing
ls -d /l2mac /testpool* 2>&1           # No such file or directory, twice
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf 2>&1   # no zpool.cache or a 0-byte one; no vdev_id.conf
ls -d $W $L 2>&1                       # No such file or directory, twice
ls -d /var/tmp/vm-task-107t 2>&1       # No such file or directory: removed by the last task
echo "/var/tmp holds: $(LC_ALL=C ls -A /var/tmp | xargs)"
echo "/var/log zts dirs: $(ls -d /var/log/zts-* 2>/dev/null | xargs)"
df -h /var/tmp /var/log
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')"
sudo -n journalctl -k -n 1 --no-pager -q   # one kernel line
sudo -n journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
for c in git python3 modinfo sha256sum truncate cmp dd seq; do echo "$c: $(command -v $c || echo absent)"; done
python3 --version
echo "doc checkout: top $(git -C $DOC rev-parse --show-toplevel 2>&1), branch $(git -C $DOC rev-parse --abbrev-ref HEAD 2>&1), head $(git -C $DOC log -1 --format=%h 2>&1), origin $(git -C $DOC remote get-url origin 2>&1)"
sha256sum $DOC/v/VM-TESTS-107-l2mac.md 2>&1
```

Keep this output: it goes into `REPORT.txt` by hand. The number printed
after the one kernel line is the kernel baseline. The `/var/tmp`, `/var/log`
and `free` lines are recorded only: earlier tasks' directories there are left
alone, `/var/tmp/vm-task-107t` included if it is there after all. The doc
checkout line is expected as `doc checkout: top /var/tmp/vm-task-107v, branch
vm-tasks-1026, head <the branch's head>, origin
https://github.com/tigerblue77/zfs` (with or without `.git`) for a clone, or
`branch HEAD, head <a commit of vm-tasks-1026>, origin error: No such remote
'origin'` for a checkout fetched by commit; both are fine. If you read this
document from another place, say so: STEP 8 then leaves that place alone.

Stop at once, changing nothing, and report what you found if: the user is not
`tigerblue77` or `sudo -n` fails; the kernel headers are missing; a zfs or
spl module is loaded or `/dev/zfs` exists; a test, build, fio, ksh, zdb or
zinject process or any `zed` runs (ignore the line of the shell running this
block, if `pgrep` shows it), or `/var/run/zed.pid` exists; a tmux pane other
than this session's runs anything but an idle shell; a loop or device-mapper
device, a zfs or test mount, a `/l2mac` or `/testpool*` directory, a
non-empty `zpool.cache` or a `vdev_id.conf` exists; `$W` or `$L` exists (this
task started once already); `journalctl -k` prints nothing; `git`,
`python3`, `modinfo`, `sha256sum`, `truncate`, `cmp`, `dd` or `seq` is
absent; or less than `6G` is free on `/var/tmp` (three trees, the
clone and the pool files: about 3.5 GB). A kernel other than
`6.12.113+deb13-cloud-amd64` is not a stop by itself: STEP 2 checks that it
is within the range each tree supports, and that the modules were built for
it.

## STEP 1: this task's directories and helpers

The block writes nothing unless it can create both directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
if mkdir /var/tmp/zts-l2mac-1028 && sudo -n mkdir /var/log/zts-l2mac-1028 &&
    sudo -n chown tigerblue77: /var/log/zts-l2mac-1028; then
echo "mkdir rc=0"
cat > /var/log/zts-l2mac-1028/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-l2mac-1028
L=/var/log/zts-l2mac-1028
DOC=/var/tmp/vm-task-107v
DOCFILE=v/VM-TESTS-107-l2mac.md
FORK=https://github.com/tigerblue77/zfs
UPSTREAM=https://github.com/openzfs/zfs
# O: the tag zfs-2.4.4 (its tag object, its commit). MR: upstream master,
# the parent of FD. FD: the fix (fork branch l2arc-encrypted-mac-mismatch).
TAGO=f75f3256a0916c5ba090b088e6ea5a9528f6f956
HO=71a9f9578616a90c3c14bb59629fb4d31bfd68d1
HMR=5ebd13aa9a2329cac7bc8b23bfe946a4cde8bb53
HFD=288026c17aaefb25e2a5b1a8b04fd33bbe938867
ARC=module/zfs/arc.c
ZSTDH=module/zstd/lib/zstd.h
# arc.c's blob in O, MR and FD; what FD changes and its patch-id.
ARCO=34dad8b0e27e49c42281c6878fd12c9b2ad45179
ARCMR=15565d119261a6bcd741b4b16b4516d8a02a27c5
ARCFD=026538964106c083f71848741f38d8890afcc49c
NUMFD="11 2 $ARC"
PIDFD=627b9997c57b2f474a7dc1266295eb914c0e8598
# What zfs.ko holds of the MAC assertion of master, of the assertion FD
# keeps for a buffer that was not recompressed, and of an assertion every
# debug build of these trees has. A release build holds none of them.
SMAC='memcmp(mac, hdr->b_crypt_hdr.b_mac, ZIO_DATA_MAC_LEN)'
SSKIP='VERIFY3P(to_write, ==, cabd)'
SDBG='VERIFY3P(to_write, !=, hdr->b_l1hdr.b_pabd)'
# The pool, its dataset, the file; the data (gen.py's output), its 64 block
# sums (one per line), gen.py itself.
POOL=l2mac
DS=l2mac/enc
MNT=$W/mnt
F=$W/mnt/data
SRCSHA=6a3f815300bbe1dcfd4f560987a1467e895ce5037acef3ad02610e8d40baf9fe
EXPSHA=a939caea9a34790b898c6c00fc8d9f5a065f8af1216a899c4d7285f822609193
GENSHA=67c9fdc358875bd3c24ed8b6060542347d352c270e756023e086cb7c2f6d7b9d
# The reader's tunables, set before each import, and their defaults.
SET="zfs_compressed_arc_enabled=0 l2arc_noprefetch=0 dbuf_cache_max_bytes=262144 zfs_prefetch_disable=1"
DEF="zfs_compressed_arc_enabled=1 l2arc_noprefetch=1 dbuf_cache_max_bytes=18446744073709551615 zfs_prefetch_disable=0"
# Two more a run requires: l2arc_headroom=0 stops every L2ARC write on master.
REQ="l2arc_headroom=8 l2arc_mfuonly=0"
# Recorded at each load and run.
TUN="zfs_compressed_arc_enabled l2arc_noprefetch dbuf_cache_max_bytes
zfs_prefetch_disable l2arc_headroom l2arc_write_max l2arc_feed_secs
l2arc_rebuild_enabled l2arc_rebuild_blocks_min_l2size l2arc_mfuonly
l2arc_exclude_special l2arc_dwpd_limit zfs_arc_min_prefetch_ms"
TUN=$(echo $TUN)
# The arcstats kept at each point of a run.
AS="l2_feeds l2_writes_sent l2_writes_done l2_writes_error l2_write_bytes
l2_size l2_asize l2_bufc_data_asize l2_bufc_metadata_asize l2_hits
l2_misses l2_read_bytes l2_cksum_bad l2_io_error l2_abort_lowmem
l2_rebuild_success l2_log_blk_writes evict_l2_cached evict_l2_eligible
evict_l2_ineligible evict_l2_skip evict_skip"
AS=$(echo $AS)
# Kernel findings.
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KIGN='spa_import_progress_set_notes_impl'

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

say() {   # say <line>: on the screen and in $L/runs.txt
	echo "$*" | tee -a $L/runs.txt
}

sv_of() {   # sv_of <O|MR|FD>: the srcversion STEP 2 recorded for that tree
	case $1 in
	O) echo "$SV_O" ;;
	MR) echo "$SV_MR" ;;
	FD) echo "$SV_FD" ;;
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

# The current values of the tunables named in <name=value> words.
tunvals() {   # tunvals <name=value>...
	local kv
	for kv in "$@"; do
		printf '%s=%s\n' ${kv%%=*} "$(cat /sys/module/zfs/parameters/${kv%%=*} 2>&1)"
	done | xargs
}

zc() {   # zc <tree> <command> <args>: that tree's command, as root, niced
	local t=$1 c=$2
	shift 2
	nice -n 10 ionice -c3 sudo -n $W/$t/$c "$@"
}

# Run a command with its output in $L/<label>.log; print "<what> rc=<rc>".
do_rc() {   # do_rc <label> <what> <command> <args>
	local lb=$1 w=$2 rc
	shift 2
	echo "+ $*" >> $L/$lb.log
	"$@" >> $L/$lb.log 2>&1
	rc=$?
	say "$w rc=$rc"
	return $rc
}

# Kernel messages since <since>. Any finding stops and sets PANIC: nothing
# touches the pool or the module after it.
kern_check() {   # kern_check <label> <since>
	local n
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $L/kern-$1.txt 2>&1
	n=$(grep -iE "$KRE" $L/kern-$1.txt | grep -vcE "$KIGN")
	say "kernel $1: $n findings ($(wc -l < $L/kern-$1.txt) lines)"
	grep -iE "$KRE" $L/kern-$1.txt | grep -vE "$KIGN" | head -n 20 | sed 's/^/  /' | tee -a $L/runs.txt
	[ "$n" -eq 0 ] && return 0
	touch $L/PANIC
	stop "$1: kernel finding"
}

# Build <tree> in place: autogen, configure with <options>, make -j8, niced.
build_tree() {   # build_tree <O|MR|FD> [configure options]
	local t=$1 t0 rc
	shift
	stopped && return 1
	t0=$(date +%s)
	(cd $W/$t && nice -n 10 ionice -c3 sh -c \
	    "./autogen.sh && ./configure $* && make -j8") > $L/build-$t.log 2>&1
	rc=$?
	echo "build $t (configure $*) rc=$rc wall=$(( $(date +%s) - t0 ))s" \
	    "free=$(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ')" | tee -a $L/build.txt
	if [ $rc -ne 0 ]; then
		grep -m 10 -nE ': error:|\*\*\* \[' $L/build-$t.log | tee -a $L/build.txt
		stop "the build of $t"
	fi
}

# Load <tree>'s module stack with its own scripts/zfs.sh -r, which first
# unloads the one loaded. Only with nothing stopped, no zed and no pool or
# zfs mount left; then the srcversion must be <tree>'s, no pool may appear,
# and the reader's tunables must be at their defaults.
switch_to() {   # switch_to <O|MR|FD> <label>
	local t=$1 lb=$2 sv cur prev p since rc
	stopped && return 1
	sv=$(sv_of $t)
	if [ -z "$sv" ]; then
		stop "$lb: no srcversion recorded for $t"
		return 1
	fi
	if [ -e /var/run/zed.pid ] || [ -n "$(pgrep -x 'zed|lt-zed')" ]; then
		stop "$lb: zed runs or /var/run/zed.pid exists, nothing loaded"
		return 1
	fi
	prev=$(cat $L/loaded 2>/dev/null)
	if [ -n "$prev" ]; then
		p=$(zc $prev zpool list -H -o name 2>&1)
		if [ -n "$p$(grep ' zfs ' /proc/mounts)" ]; then
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
		say "$lb: load $t rc=$rc, srcversion [$cur], expected $sv"
		head -n 20 $L/load-$lb.log | sed 's/^/  /' | tee -a $L/runs.txt
		touch $L/HELD
		stop "$lb: the load of $t"
		return 1
	fi
	echo $t > $L/loaded
	p=$(zc $t zpool list -H -o name 2>&1)
	tunables > $L/tun-$lb.txt
	say "$lb: load $t rc=0, srcversion $cur, pools [$p]"
	say "  tunables: $(cat $L/tun-$lb.txt)"
	if [ -n "$p" ]; then
		stop "$lb: a pool appeared at the load of $t"
		return 1
	fi
	p=$(tunvals $DEF)
	if [ "$p" != "$DEF" ]; then
		say "MISMATCH: $lb: tunables at load [$p], expected [$DEF]"
		stop "$lb: the tunables at load"
		return 1
	fi
	kern_check $lb "$since"
}

# Unload <tree> with its own scripts/zfs.sh -u.
unload_tree() {   # unload_tree <O|MR|FD> <label>
	local t=$1 lb=$2 p rc
	stopped && return 1
	if [ "$(cat $L/loaded 2>/dev/null)" != $t ]; then
		stop "$lb: $t is not the loaded tree"
		return 1
	fi
	p=$(zc $t zpool list -H -o name 2>&1)
	if [ -n "$p$(grep ' zfs ' /proc/mounts)" ] || [ -e /var/run/zed.pid ]; then
		touch $L/HELD
		stop "$lb: a pool, a zfs mount or /var/run/zed.pid is left ($p), nothing unloaded"
		return 1
	fi
	(cd $W/$t && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u) > $L/unload-$lb.log 2>&1
	rc=$?
	p=$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)
	say "$lb: unload $t rc=$rc, lsmod [$p]"
	if [ $rc -ne 0 ] || [ -n "$p" ]; then
		touch $L/HELD
		stop "$lb: the unload of $t"
		return 1
	fi
	rm -f $L/loaded
}

# The arcstats at a point of a run: all of them in <run>-<point>.arcstats,
# the fields of AS as "<name> <value>" lines in <run>-<point>.as.
snap() {   # snap <run> <point>
	sudo -n cat /proc/spl/kstat/zfs/arcstats > $L/$1-$2.arcstats 2>&1
	awk -v l="$AS" '
	BEGIN { n = split(l, a, " "); for (i = 1; i <= n; i++) w[a[i]] = 1 }
	($1 in w) { print $1, $3 }' $L/$1-$2.arcstats > $L/$1-$2.as
}
val() {   # val <run> <point> <name>
	awk -v k=$3 '$1 == k { print $2 }' $L/$1-$2.as
}
dl() {   # dl <run> <from> <to> <names>: "<name> +<change>, ..."
	local r=$1 a=$2 b=$3
	shift 3
	awk -v names="$*" '
	FNR == NR { x[$1] = $2; next }
	{ y[$1] = $2 }
	END {
		n = split(names, k, " ")
		for (i = 1; i <= n; i++)
			printf "%s%s %+.0f", (i > 1 ? ", " : ""), k[i], y[k[i]] - x[k[i]]
		printf "\n"
	}' $L/$r-$a.as $L/$r-$b.as
}

# Read the 64 blocks of $F, each with its own dd of 64 KiB, and compare each
# with its expected sha256. One line per block in <label>-r<n>.txt:
# "<block> <ok|EIO|DIFFERS|ERROR> rc=<dd's rc> <sha256> <dd's first error>".
read_pass() {   # read_pass <label> <n>
	local f=$L/$1-r$2.txt i out sum rc exp st
	: > $f
	for i in $(seq 0 63); do
		exp=$(sed -n "$((i + 1))p" $L/expected.txt)
		out=$(sudo -n dd if=$F bs=64K skip=$i count=1 status=none 2> $L/dd.err | sha256sum; echo "${PIPESTATUS[0]}")
		sum=${out%% *}
		rc=${out##*$'\n'}
		if [ "$rc" = 0 ] && [ "$sum" = "$exp" ]; then
			st=ok
		elif [ "$rc" != 0 ] && grep -q 'Input/output error' $L/dd.err; then
			st=EIO
		elif [ "$rc" = 0 ]; then
			st=DIFFERS
		else
			st=ERROR
		fi
		echo "$i $st rc=$rc $sum $(head -n 1 $L/dd.err)" >> $f
	done
	awk -v r=$1 -v p=$2 '
	{ c[$2]++ }
	$2 != "ok" {
		if (s != "" && $1 == e + 1) {
			e = $1
		} else {
			if (s != "")
				l = l (l == "" ? "" : ",") (s == e ? s : s "-" e)
			s = e = $1
		}
	}
	END {
		if (s != "")
			l = l (l == "" ? "" : ",") (s == e ? s : s "-" e)
		printf "%s read %s: ok x%d, EIO x%d, DIFFERS x%d, ERROR x%d; not ok [%s]\n",
		    r, p, c["ok"], c["EIO"], c["DIFFERS"], c["ERROR"], l
	}' $f | tee -a $L/runs.txt
}

# Wait for the L2ARC feed thread: one poll a second, until at least 6 feeds
# have run, every write sent is done and l2_write_bytes has not moved over
# the last 5 polls; 120 polls at most.
feed_wait() {   # feed_wait <run>
	local n=0 st=0 f0 f=0 w=0 w0=-1 s=0 d=0 a
	a=$(sudo -n cat /proc/spl/kstat/zfs/arcstats 2>&1)
	f0=$(echo "$a" | awk '$1 == "l2_feeds" { print $3 }')
	f0=${f0:-0}
	while [ $n -lt 120 ]; do
		sleep 1
		n=$((n + 1))
		a=$(sudo -n cat /proc/spl/kstat/zfs/arcstats 2>&1)
		f=$(echo "$a" | awk '$1 == "l2_feeds" { print $3 }')
		w=$(echo "$a" | awk '$1 == "l2_write_bytes" { print $3 }')
		s=$(echo "$a" | awk '$1 == "l2_writes_sent" { print $3 }')
		d=$(echo "$a" | awk '$1 == "l2_writes_done" { print $3 }')
		f=${f:-0}
		if [ "$w" = "$w0" ] && [ "$s" = "$d" ]; then
			st=$((st + 1))
		else
			st=0
		fi
		w0=$w
		[ $((f - f0)) -ge 6 ] && [ $st -ge 5 ] && break
	done
	say "$1: feed: ${n}s, l2_feeds +$((f - f0)), l2_writes_sent $s, done $d," \
	    "l2_write_bytes $w, $([ $st -ge 5 ] && echo settled || echo 'NOT SETTLED')"
}

# The vdev rows (name, state, read, write, cksum) and the errors lines of a
# zpool status file.
st_digest() {   # st_digest <file>
	awk -v p=$POOL '
	/^errors:/ { e = 1 }
	e { if (NF) print "  " $0; next }
	$1 == p || $1 ~ /^\// { printf "  %s %s %s %s %s\n", $1, $2, $3, $4, $5 }' "$1"
}

# After a write or a run: no pool, no zfs mount, no zed, no /l2mac.
pool_clean() {   # pool_clean <label> <tree>
	local p
	p="pools [$(zc $2 zpool list -H -o name 2>&1)] mounts [$(grep ' zfs ' /proc/mounts)]"
	p="$p zed [$(pgrep -ax 'zed|lt-zed')$(ls /var/run/zed.pid 2>/dev/null)] rootdirs [$(ls -d /$POOL 2>/dev/null)]"
	if [ "$p" = "pools [] mounts [] zed [] rootdirs []" ]; then
		say "after $1: nothing left"
	else
		say "after $1: LEFT $p"
		touch $L/HELD
		stop "$1: leftovers"
	fi
}

# Write the pool with the loaded <tree>: $W/<label>/vdev, the dataset, the
# data in $F, read 1 (from the ARC), export.
write_pool() {   # write_pool <label> <O|FD>
	local lb=$1 t=$2 D=$W/$1 since
	stopped && return 1
	if [ "$(cat $L/loaded 2>/dev/null)" != $t ] || [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(sv_of $t)" ]; then
		stop "$lb: the loaded module is not $t's"
		return 1
	fi
	if [ -e $D ] || [ -n "$(zc $t zpool list -H -o name 2>&1)$(grep ' zfs ' /proc/mounts)" ]; then
		stop "$lb: $D, a pool or a zfs mount exists, nothing written"
		return 1
	fi
	since=$(date '+%F %T')
	do_rc $lb "$lb: vdev" sh -c "mkdir $D && truncate -s 512M $D/vdev" &&
	    do_rc $lb "$lb: create" zc $t zpool create -o ashift=12 -o cachefile=none \
	    -O mountpoint=none -O atime=off $POOL $D/vdev &&
	    do_rc $lb "$lb: dataset" zc $t zfs create -o encryption=aes-256-gcm \
	    -o keyformat=raw -o keylocation=file://$W/key -o compression=zstd \
	    -o recordsize=64K -o mountpoint=$MNT $DS &&
	    do_rc $lb "$lb: write" nice -n 10 ionice -c3 sudo -n dd if=$W/src of=$F bs=64K status=none &&
	    do_rc $lb "$lb: sync" zc $t zpool sync $POOL || {
		stop "$lb: writing the pool"
		return 1
	}
	say "$(same "$lb: the file is the data" "$(sudo -n cmp $W/src $F 2>&1; echo $?)" 0)"
	say "$lb: file $(sudo -n stat -c '%U %a %s' $F 2>&1);" \
	    "$(zc $t zfs get -Hp -o property,value encryption,keystatus,compression,recordsize,compressratio,logicalused,used $DS 2>&1 |
	    awk '{ printf "%s%s=%s", s, $1, $2; s = " " }')"
	read_pass $lb 1
	zc $t zpool status $POOL > $L/$lb-status.txt 2>&1
	st_digest $L/$lb-status.txt | tee -a $L/runs.txt
	do_rc $lb "$lb: export" zc $t zpool export $POOL || {
		stop "$lb: the export"
		return 1
	}
	say "$lb: $D holds [$(LC_ALL=C ls -A $D | xargs)], vdev $(du -k $D/vdev | cut -f1) KiB allocated;" \
	    "$MNT holds [$(LC_ALL=C ls -A $MNT 2>&1 | xargs)]"
	kern_check $lb "$since" || return 1
	pool_clean $lb $t || return 1
	if grep -q "^MISMATCH: $lb:" $L/runs.txt || ! grep -q "^$lb read 1: ok x64, " $L/runs.txt; then
		stop "$lb: the pool written is not as expected"
		return 1
	fi
}

# One reading run on the loaded <tree> (see "The pool, the reads, ..."):
# a copy of <source dir>/vdev and a new cache device in $W/<run>, the
# reader's tunables, import, read 1, the feed, unmount, zinject -a, mount,
# read 2, export, import, read 3, export. The kernel log is checked after
# the feed and after read 2, before the next pool command.
l2run() {   # l2run <run> <O|MR|FD> <source dir>
	local r=$1 t=$2 R=$W/$1 since kv p
	stopped && return 1
	if [ "$(cat $L/loaded 2>/dev/null)" != $t ] || [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(sv_of $t)" ]; then
		stop "$r: the loaded module is not $t's"
		return 1
	fi
	if [ -e $R ] || [ ! -f $3/vdev ] || [ -n "$(zc $t zpool list -H -o name 2>&1)$(grep ' zfs ' /proc/mounts)" ]; then
		stop "$r: $R exists, $3/vdev is missing, or a pool or a zfs mount exists: nothing run"
		return 1
	fi
	since=$(date '+%F %T')
	do_rc $r "$r: copy" sh -c "mkdir $R && cp --sparse=always $3/vdev $R/vdev && truncate -s 256M $R/cache" || {
		stop "$r: the copy"
		return 1
	}
	for kv in $SET; do
		echo ${kv#*=} | sudo -n tee /sys/module/zfs/parameters/${kv%%=*} > /dev/null
	done
	tunables > $L/tun-$r.txt
	say "$r: tunables $(cat $L/tun-$r.txt)"
	p=$(tunvals $SET $REQ)
	if [ "$p" != "$SET $REQ" ]; then
		say "MISMATCH: $r: tunables [$p], expected [$SET $REQ]"
		stop "$r: the tunables"
		return 1
	fi
	do_rc $r "$r: import" zc $t zpool import -d $R -o cachefile=none -N $POOL &&
	    do_rc $r "$r: load-key" zc $t zfs load-key $DS &&
	    do_rc $r "$r: mount" zc $t zfs mount $DS &&
	    do_rc $r "$r: cache device" zc $t zpool add $POOL cache $R/cache || {
		stop "$r: the import"
		return 1
	}
	snap $r s0
	read_pass $r 1
	snap $r s1
	feed_wait $r
	snap $r s2
	kern_check $r-feed "$since" || return 1
	zc $t zpool status -v $POOL > $L/$r-status1.txt 2>&1
	do_rc $r "$r: unmount" zc $t zfs unmount $DS &&
	    do_rc $r "$r: zinject -a" zc $t zinject -a || {
		stop "$r: the flush"
		return 1
	}
	snap $r s3
	do_rc $r "$r: mount" zc $t zfs mount $DS || {
		stop "$r: the mount"
		return 1
	}
	read_pass $r 2
	snap $r s4
	zc $t zpool status -v $POOL > $L/$r-status2.txt 2>&1
	kern_check $r-read2 "$since" || return 1
	do_rc $r "$r: export" zc $t zpool export $POOL &&
	    do_rc $r "$r: import again" zc $t zpool import -d $R -o cachefile=none -N $POOL &&
	    do_rc $r "$r: load-key" zc $t zfs load-key $DS &&
	    do_rc $r "$r: mount" zc $t zfs mount $DS || {
		stop "$r: the second import"
		return 1
	}
	read_pass $r 3
	snap $r s5
	zc $t zpool status -v $POOL > $L/$r-status3.txt 2>&1
	do_rc $r "$r: export" zc $t zpool export $POOL || {
		stop "$r: the last export"
		return 1
	}
	kern_check $r "$since" || return 1
	run_digest $r > $L/digest-$r.txt
	tee -a $L/runs.txt < $L/digest-$r.txt
	say "$r: $R holds [$(LC_ALL=C ls -A $R | xargs)]"
	pool_clean $r $t
}

# What a run shows, from its arcstats and its status files.
run_digest() {   # run_digest <run>
	local r=$1
	echo "$r after the feed: data asize $(val $r s2 l2_bufc_data_asize)," \
	    "metadata asize $(val $r s2 l2_bufc_metadata_asize), l2_size $(val $r s2 l2_size)," \
	    "l2_asize $(val $r s2 l2_asize)"
	echo "$r read 1 and feed (s0 to s2): $(dl $r s0 s2 l2_write_bytes l2_writes_sent l2_writes_error l2_feeds l2_abort_lowmem)"
	echo "$r flush (s2 to s3): $(dl $r s2 s3 evict_l2_cached evict_l2_eligible evict_l2_ineligible evict_l2_skip evict_skip)"
	echo "$r read 2 (s3 to s4): $(dl $r s3 s4 l2_hits l2_misses l2_read_bytes l2_cksum_bad l2_io_error)"
	echo "$r read 3 (s4 to s5): $(dl $r s4 s5 l2_hits l2_misses l2_cksum_bad l2_io_error l2_rebuild_success)"
	echo "$r at the end (s5): l2_write_bytes $(val $r s5 l2_write_bytes), l2_hits $(val $r s5 l2_hits)," \
	    "l2_cksum_bad $(val $r s5 l2_cksum_bad), l2_io_error $(val $r s5 l2_io_error)"
	echo "$r zpool status after the feed:"
	st_digest $L/$r-status1.txt
	echo "$r zpool status after read 2:"
	st_digest $L/$r-status2.txt
	echo "$r zpool status after read 3:"
	st_digest $L/$r-status3.txt
}

# The counts of read <n> of <run>: "<ok> <EIO> <DIFFERS> <ERROR>".
rcounts() {   # rcounts <run> <n>
	grep "^$1 read $2: " $L/runs.txt | tail -n 1 |
	    sed -E 's/^.* ok x([0-9]+), EIO x([0-9]+), DIFFERS x([0-9]+), ERROR x([0-9]+);.*$/\1 \2 \3 \4/'
}
ENV
bash -n /var/log/zts-l2mac-1028/env.sh; echo "env.sh syntax rc=$?"
cat > /var/log/zts-l2mac-1028/gen.py <<'PY'
# The data of the userspace reproduction: 64 blocks of 64 KiB, block b
# filled by fill_block(buf, 65536, b) with the 64-word vocabulary.
import sys

WORDS = (
    "the pool block cache device write read mirror stripe vdev label uber "
    "txg sync dirty record dnode object bonus spill indirect level gang "
    "hole embedded checksum fletcher sha256 blake3 compress lz4 zstd gzip "
    "zle encrypt aes gcm ccm salt iv mac key wrap load unload snapshot "
    "clone send receive scrub resilver trim special dedup log spare l2arc "
    "arc mru mfu ghost evict prefetch metadata").split()
assert len(WORDS) == 64
M = (1 << 64) - 1
BLK = 65536


def block(b):
    x = (0x9E3779B97F4A7C15 * (b + 1)) & M
    out = bytearray()
    while len(out) < BLK:
        x ^= (x << 13) & M
        x ^= x >> 7
        x ^= (x << 17) & M
        out += WORDS[x % 64].encode()
        out += b' ' if (x >> 8) % 9 else b'\n'
    return bytes(out[:BLK])


out = sys.stdout.buffer
for b in range(64):
    out.write(block(b))
out.flush()
PY
. /var/log/zts-l2mac-1028/env.sh
date '+%F %T' > $L/start.txt
if [ "$(git -C $DOC rev-parse --show-toplevel 2>/dev/null)" = $DOC ] && [ -f $DOC/$DOCFILE ]; then
	git -C $DOC rev-parse HEAD > $L/doc-head.txt
	sha256sum < $DOC/$DOCFILE | cut -d' ' -f1 > $L/doc-sha.txt
fi
{
echo "start $(cat $L/start.txt) ($(date -u '+%T') UTC); $(id -un); $(uname -r); umask $(umask)"
stat -c '%U %a %n' $W $L
echo "doc checkout: head $(cat $L/doc-head.txt 2>&1), $DOCFILE sha256 $(cat $L/doc-sha.txt 2>&1)"
same "gen.py" "$(sha256sum < $L/gen.py | cut -d' ' -f1)" $GENSHA
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
printed, `ok: gen.py`, and the `zpool.cache` and free-space lines. A doc
checkout line with `No such file or directory` means this document was not
read from `$DOC`: record it, it is not a stop (STEP 8 then keeps `$DOC`).
`MISMATCH: gen.py` means the file was not written as printed here: stop.

## STEP 2: the sources, the three trees, the builds

Fetch FD by its commit from the fork, and upstream master and the tag
`zfs-2.4.4` (read-only towards GitHub), and pin the three commits: FD's
parent is MR, MR is in upstream master, the tag is the expected object and
commit, what FD changes, its patch-id, `arc.c` in each. Where upstream master
and the fork branch point now is recorded only:

```sh
. /var/log/zts-l2mac-1028/env.sh
if ! stopped; then
	{
	git init -q $W/repo > $L/init.log 2>&1; echo "init rc=$?"
	git -C $W/repo fetch -q --no-tags $FORK "+$HFD:refs/heads/fd" > $L/fetch-fork.log 2>&1; echo "fetch FD rc=$?"
	git -C $W/repo fetch -q --no-tags $UPSTREAM "+refs/heads/master:refs/heads/upstream-master" \
	    "+refs/tags/zfs-2.4.4:refs/tags/zfs-2.4.4" > $L/fetch-upstream.log 2>&1; echo "fetch upstream rc=$?"
	cd $W/repo
	same "FD" "$(git rev-parse -q --verify refs/heads/fd)" $HFD
	same "FD's parent" "$(git rev-parse -q --verify $HFD^)" $HMR
	same "MR in upstream master" "$(git merge-base --is-ancestor $HMR refs/heads/upstream-master && echo yes)" yes
	same "the tag zfs-2.4.4" "$(git rev-parse -q --verify refs/tags/zfs-2.4.4) $(git cat-file -t refs/tags/zfs-2.4.4 2>&1)" "$TAGO tag"
	same "its commit" "$(git rev-parse -q --verify 'refs/tags/zfs-2.4.4^{commit}')" $HO
	same "what FD changes" "$(git diff --numstat $HMR $HFD | xargs)" "$NUMFD"
	same "FD's patch-id" "$(git diff $HMR $HFD | git patch-id --stable | cut -d' ' -f1)" $PIDFD
	same "arc.c in O, MR, FD" "$(for c in $HO $HMR $HFD; do git rev-parse -q --verify $c:$ARC; done | xargs)" "$ARCO $ARCMR $ARCFD"
	for c in $HO $HMR $HFD; do echo "  $(git log -1 --abbrev=10 --format='%h %s' $c)"; done
	echo "upstream master now (recorded only): $(git log -1 --abbrev=10 --format='%h %s' refs/heads/upstream-master)"
	echo "on GitHub now (recorded only): $(git ls-remote $FORK refs/heads/l2arc-encrypted-mac-mismatch 2>&1 |
	    awk '{ printf "%s=%.10s", $2, $1 }')"
	} 2>&1 | tee $L/fix.txt
	grep -q '^MISMATCH' $L/fix.txt && stop "the commits are not as expected"
	grep -qE '^(init|fetch [A-Za-z]+) rc=[1-9]' $L/fix.txt && stop "the init or a fetch failed"
fi
```

Expected:

```
init rc=0
fetch FD rc=0
fetch upstream rc=0
ok: FD
ok: FD's parent
ok: MR in upstream master
ok: the tag zfs-2.4.4
ok: its commit
ok: what FD changes
ok: FD's patch-id
ok: arc.c in O, MR, FD
  71a9f95786 Tag zfs-2.4.4
  5ebd13aa9a ZTS: sync checksum test data before fault injection
  288026c17a L2ARC: Do not write encrypted buffers whose MAC differs
upstream master now (recorded only): <its head>
on GitHub now (recorded only): refs/heads/l2arc-encrypted-mac-mismatch=288026c17a
```

A `MISMATCH`, or a failed init or fetch, means stop. The last two lines are
not checked.

Then one worktree per tree, and what each holds: `arc.c` and its two markers
(`MAC assert` the `ASSERT0()` of the bug, with its line; `MAC skip` the
`ASSERT3P(to_write, ==, cabd)` of the fix), the bundled zstd version, LZ4's
two files, and the range of kernels the tree's `META` declares, which must
hold the running one (for O there is no fallback: the pool can only be
written with O's module, so a kernel O does not support, or a build of O that
fails, stops the task):

```sh
. /var/log/zts-l2mac-1028/env.sh
if ! stopped; then
	{
	for t in O:$HO MR:$HMR FD:$HFD; do
		git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktree.log 2>&1
		echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD 2>&1)"
	done
	k=$(uname -r | cut -d. -f1,2)
	for t in O MR FD; do
		cd $W/$t
		same "$t as committed" "$(git status --porcelain --untracked-files=no | wc -l)" 0
		echo "$t: arc.c $(git hash-object $ARC | cut -c1-10)," \
		    "MAC assert x$(grep -c 'ASSERT0(memcmp(mac, hdr->b_crypt_hdr.b_mac, ZIO_DATA_MAC_LEN));' $ARC)" \
		    "at line [$(grep -n 'ASSERT0(memcmp(mac, hdr->b_crypt_hdr.b_mac, ZIO_DATA_MAC_LEN));' $ARC | cut -d: -f1 | xargs)]," \
		    "MAC skip x$(grep -c 'ASSERT3P(to_write, ==, cabd);' $ARC);" \
		    "zstd $(awk '/^#define ZSTD_VERSION_(MAJOR|MINOR|RELEASE) / { printf "%s%s", s, $3; s = "." }' $ZSTDH)," \
		    "lz4 $(git hash-object module/zfs/lz4.c | cut -c1-10) $(git hash-object module/zfs/lz4_zfs.c | cut -c1-10)"
		lo=$(awk '/^Linux-Minimum:/ { print $2 }' META)
		hi=$(awk '/^Linux-Maximum:/ { print $2 }' META)
		same "$t supports Linux $k (META: $lo to $hi)" \
		    "$([ -n "$lo" ] && [ -n "$hi" ] && [ "$(printf '%s\n' $lo $k $hi | sort -V | xargs)" = "$lo $k $hi" ] && echo within)" within
	done
	} 2>&1 | tee $L/trees.txt
	grep -q '^MISMATCH' $L/trees.txt && stop "the trees are not as expected"
	grep -qE '^worktree [A-Z]+ rc=[1-9]' $L/trees.txt && stop "a worktree failed"
fi
```

Expected:

```
worktree O rc=0 at 71a9f95786
worktree MR rc=0 at 5ebd13aa9a
worktree FD rc=0 at 288026c17a
ok: O as committed
O: arc.c 34dad8b0e2, MAC assert x1 at line [9437], MAC skip x0; zstd 1.4.5, lz4 26dd09acd4 672b1bd27e
ok: O supports Linux 6.12 (META: 4.18 to 7.2)
ok: MR as committed
MR: arc.c 15565d1192, MAC assert x1 at line [9620], MAC skip x0; zstd 1.5.7, lz4 26dd09acd4 672b1bd27e
ok: MR supports Linux 6.12 (META: 4.18 to 7.3)
ok: FD as committed
FD: arc.c 0265389641, MAC assert x0 at line [], MAC skip x1; zstd 1.5.7, lz4 26dd09acd4 672b1bd27e
ok: FD supports Linux 6.12 (META: 4.18 to 7.3)
```

(`6.12` is the running kernel's version; another one is fine as long as the
three lines say `ok`.) Anything else means stop.

Then build O, MR and FD, one after the other, O first (about 15 minutes
together; see the rule about long commands). O and MR are release builds, FD
a debug build:

```sh
. /var/log/zts-l2mac-1028/env.sh
build_tree O && build_tree MR && build_tree FD --enable-debug
```

Expected: `build O (configure ) rc=0 wall=<s>s free=<n>G`, then `build MR
(configure ) ...` and `build FD (configure --enable-debug) ...`, the free
space going down by about 1G per tree. A failed build prints its first
`error:` lines and `the build of <tree>: stop`. **If O fails to build**, the
pool cannot be written by zfs-2.4.4: O's userspace tools need O's module, and
nothing else here writes with zstd 1.4.5. Do not try another way: run STEP 8
and report the error lines.

Then check the three builds and record the three srcversions for the
helpers. MR must be a release build whose `zfs.ko` holds none of the three
assertion texts: if it holds one, it is never loaded:

```sh
. /var/log/zts-l2mac-1028/env.sh
if ! stopped; then
	{
	for t in O MR FD; do
		cd $W/$t
		same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
		case $t in FD) d="3 0" ;; *) d="1 2" ;; esac
		same "$t ZFS_DEBUG lines (defined, undefined)" \
		    "$(grep -c '^#define ZFS_DEBUG 1' zfs_config.h 2>&1) $(grep -c '^/\* #undef ZFS_DEBUG \*/' zfs_config.h 2>&1)" "$d"
		same "$t vermagic" "$(modinfo -F vermagic module/zfs.ko 2>&1 | cut -d' ' -f1)" "$(uname -r)"
		same "$t files the runs use" "$(for f in module/spl.ko module/zfs.ko zpool zfs zinject scripts/zfs.sh; do
		    [ -e $f ] || echo $f; done | xargs)" ""
		same "$t as committed after the build" "$(git status --porcelain --untracked-files=no | wc -l)" 0
		modinfo -F srcversion module/zfs.ko > $L/srcv-$t 2>&1
		echo "$t compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) (expected 0);" \
		    "other warning: lines: $(grep 'warning:' $L/build-$t.log | grep -cvE ':[0-9]+:[0-9]+: warning:') (objtool's, about 83);" \
		    "srcversion $(cat $L/srcv-$t)"
		grep -m5 -nE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log
		echo "$t zfs.ko: MAC assert text x$(grep -a -o -F "$SMAC" module/zfs.ko | wc -l)," \
		    "skip assert text x$(grep -a -o -F "$SSKIP" module/zfs.ko | wc -l)," \
		    "debug assert text x$(grep -a -o -F "$SDBG" module/zfs.ko | wc -l)" | tee $L/strings-$t.txt
	done
	same "O's zfs.ko (a release build)" "$(sed 's/^O zfs.ko: //' $L/strings-O.txt)" \
	    "MAC assert text x0, skip assert text x0, debug assert text x0"
	same "MR's zfs.ko (a release build: the MAC assertion is not in it)" "$(sed 's/^MR zfs.ko: //' $L/strings-MR.txt)" \
	    "MAC assert text x0, skip assert text x0, debug assert text x0"
	same "FD's zfs.ko (a debug build of the fix)" \
	    "$(sed -E 's/^FD zfs.ko: //; s/skip assert text x[1-9][0-9]*/skip assert text present/; s/debug assert text x[1-9][0-9]*/debug assert text present/' $L/strings-FD.txt)" \
	    "MAC assert text x0, skip assert text present, debug assert text present"
	same "three different srcversions" "$(cat $L/srcv-O $L/srcv-MR $L/srcv-FD | grep -xE '[0-9A-F]+' | sort -u | wc -l)" 3
	same "O head" "$(git -C $W/O rev-parse HEAD)" $HO
	same "MR head" "$(git -C $W/MR rev-parse HEAD)" $HMR
	same "FD head" "$(git -C $W/FD rev-parse HEAD)" $HFD
	same "MR against FD" "$(git -C $W/MR diff $HFD --name-only | xargs)" $ARC
	echo "free after the builds: $(df -BG --output=avail /var/tmp | tail -n 1 | tr -d ' ') (2G at least)"
	} 2>&1 | tee $L/build-checks.txt
	if grep -q '^MISMATCH' $L/build-checks.txt; then
		stop "the builds"
	elif [ "$(df -BG --output=avail /var/tmp | tail -n 1 | tr -dc 0-9)" -lt 2 ]; then
		stop "less than 2G free after the builds"
	else
		printf 'SV_O=%s\nSV_MR=%s\nSV_FD=%s\n' "$(cat $L/srcv-O)" "$(cat $L/srcv-MR)" "$(cat $L/srcv-FD)" >> $L/env.sh
		tail -n 3 $L/env.sh
	fi
fi
```

Expected, for each tree: five `ok:` lines (`ZFS_DEBUG lines` are `1 2` for O
and MR, `3 0` for FD), `compiler warnings: 0` and about 83 other lines with
its srcversion, and its `zfs.ko` line: `MAC assert text x0, skip assert text
x0, debug assert text x0` for O and MR, `MAC assert text x0, skip assert text
x1, debug assert text x1` for FD (a count above 1 is fine for FD). Then `ok:`
for the three `zfs.ko` checks, `ok: three different srcversions`, four more
`ok:` lines, the free space, and the three lines `SV_O=<O's>`,
`SV_MR=<MR's>`, `SV_FD=<FD's>`. The srcversions are recorded only. A compiler
warning count other than 0 is not a stop: quote its lines. A `MISMATCH` on
MR's `zfs.ko` means MR was built with assertions: stop, and never load it.

## STEP 3: the data, and O writes the pool `wo`

First the data and the key, from `gen.py`, nothing loaded:

```sh
. /var/log/zts-l2mac-1028/env.sh
if ! stopped; then
	{
	python3 -I $L/gen.py > $W/src; echo "gen rc=$?"
	for i in $(seq 0 63); do
		dd if=$W/src bs=64K skip=$i count=1 status=none | sha256sum | cut -d' ' -f1
	done > $L/expected.txt
	same "the data" "$(sha256sum < $W/src | cut -d' ' -f1) $(stat -c %s $W/src)" "$SRCSHA 4194304"
	same "its 64 block sums" "$(sha256sum < $L/expected.txt | cut -d' ' -f1) $(sort -u $L/expected.txt | wc -l)" "$EXPSHA 64"
	(umask 077 && printf '%s' abcdefghijklmnopqrstuvwxyz012345 > $W/key); echo "key rc=$?"
	same "the key file" "$(stat -c '%U %a %s' $W/key)" "tigerblue77 600 32"
	} 2>&1 | tee $L/data.txt
	grep -qE '^MISMATCH|^(gen|key) rc=[1-9]' $L/data.txt && stop "the data"
fi
```

Expected: `gen rc=0`, `ok: the data`, `ok: its 64 block sums`, `key rc=0`,
`ok: the key file`. Then load O, write the pool, export it, and unload O:

```sh
. /var/log/zts-l2mac-1028/env.sh
switch_to O lo && write_pool wo O && unload_tree O uo
```

Expected:

```
lo: load O rc=0, srcversion <SV_O>, pools []
  tunables: zfs_compressed_arc_enabled=1 l2arc_noprefetch=1 dbuf_cache_max_bytes=18446744073709551615 zfs_prefetch_disable=0 l2arc_headroom=<n> ... (recorded)
kernel lo: 0 findings (<n> lines)
wo: vdev rc=0
wo: create rc=0
wo: dataset rc=0
wo: write rc=0
wo: sync rc=0
ok: wo: the file is the data
wo: file root 644 4194304; encryption=aes-256-gcm keystatus=available compression=zstd recordsize=65536 compressratio=<about 3.1> logicalused=<about 4.3 million> used=<about 1.4 million>
wo read 1: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
  l2mac ONLINE 0 0 0
  /var/tmp/zts-l2mac-1028/wo/vdev ONLINE 0 0 0
  errors: No known data errors
wo: export rc=0
wo: /var/tmp/zts-l2mac-1028/wo holds [vdev], vdev <n> KiB allocated; /var/tmp/zts-l2mac-1028/mnt holds []
kernel wo: 0 findings (<n> lines)
after wo: nothing left
uo: unload O rc=0, lsmod []
```

The file's mode, `compressratio`, `logicalused` and `used` are recorded only
(`used` is about 64 x 20 KiB of data with its metadata: the blocks are
stored compressed). `$W/mnt` is the dataset's mount point, empty and root's
once exported; every later import mounts on it again.

## STEP 4: MR reads `wo` (run `mo`)

Load MR (nothing is loaded, so the reload loads) and run (1 to 3 minutes):

```sh
. /var/log/zts-l2mac-1028/env.sh
switch_to MR lm && l2run mo MR $W/wo
```

Expected:

```
lm: load MR rc=0, srcversion <SV_MR>, pools []
  tunables: ... (recorded)
kernel lm: 0 findings (<n> lines)
mo: copy rc=0
mo: tunables zfs_compressed_arc_enabled=0 l2arc_noprefetch=0 dbuf_cache_max_bytes=262144 zfs_prefetch_disable=1 l2arc_headroom=8 ... (recorded)
mo: import rc=0
mo: load-key rc=0
mo: mount rc=0
mo: cache device rc=0
mo read 1: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
mo: feed: <s>s, l2_feeds +<n>, l2_writes_sent <n>, done <n>, l2_write_bytes <about 1.2 to 1.4 million>, settled
kernel mo-feed: 0 findings (<n> lines)
mo: unmount rc=0
mo: zinject -a rc=0
mo: mount rc=0
mo read 2: ok x<64 - N>, EIO x<N>, DIFFERS x0, ERROR x0; not ok [0-<N - 1>, or similar]
kernel mo-read2: 0 findings (<n> lines)
mo: export rc=0
mo: import again rc=0
mo: load-key rc=0
mo: mount rc=0
mo read 3: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
mo: export rc=0
kernel mo: 0 findings (<n> lines)
mo after the feed: data asize <N x 20480>, metadata asize <n>, l2_size <n>, l2_asize <n>
mo read 1 and feed (s0 to s2): l2_write_bytes +<about N x 20480 and metadata>, l2_writes_sent +<n>, l2_writes_error +0, l2_feeds +<n>, l2_abort_lowmem +<n>
mo flush (s2 to s3): evict_l2_cached +<at least N x 65536>, evict_l2_eligible +<n>, evict_l2_ineligible +<n>, evict_l2_skip +<0 or a few>, evict_skip +<n>
mo read 2 (s3 to s4): l2_hits +<at least N>, l2_misses +<n>, l2_read_bytes +<n>, l2_cksum_bad +<N>, l2_io_error +0
mo read 3 (s4 to s5): l2_hits +<0 or a few>, l2_misses +<n>, l2_cksum_bad +0, l2_io_error +0, l2_rebuild_success +0
mo at the end (s5): l2_write_bytes <n>, l2_hits <n>, l2_cksum_bad <N>, l2_io_error 0
mo zpool status after the feed:
  l2mac ONLINE 0 0 0
  /var/tmp/zts-l2mac-1028/mo/vdev ONLINE 0 0 0
  /var/tmp/zts-l2mac-1028/mo/cache ONLINE 0 0 0
  errors: No known data errors
mo zpool status after read 2:
  <the same rows, counters recorded>
  errors: <recorded: the file listed with permanent errors, or a count>
mo zpool status after read 3:
  <recorded>
mo: /var/tmp/zts-l2mac-1028/mo holds [cache vdev]
after mo: nothing left
```

`N` is the number of data blocks written to the L2ARC: `data asize` is `N` x
20480. It is expected near 60 and at most 64: the blocks whose dbufs the dbuf
cache still holds when the feed runs (the last three or four read) are not
written, and read correctly from the pool in read 2. **This is the bug on a
release build of master**: every block written to the L2ARC fails its
checksum when it is read back (`l2_cksum_bad` +`N`) and its read returns EIO,
with no kernel message; read 3, from the pool, is correct. Record exactly
what happens: an `l2_cksum_bad` other than the number of EIO reads, reads
that return the data (the retry worked) or `DIFFERS` lines (wrong data
returned) are results, not stops, and the per-block lines are in
`$L/mo-r2.txt`. `NOT SETTLED` is recorded only. The errors that `zpool status`
lists after read 2 are those of this copy of the image: they come from the
failed retries, not from the disk.

## STEP 5: FD reads `wo` (run `fo`)

```sh
. /var/log/zts-l2mac-1028/env.sh
switch_to FD lf && l2run fo FD $W/wo
```

Expected: as for `mo`, with `lf`, `fo`, `<SV_FD>` and `$W/fo`, except:

```
fo: feed: <s>s, l2_feeds +<n>, l2_writes_sent <n>, done <n>, l2_write_bytes <a few tens of thousands>, settled
fo read 2: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
fo after the feed: data asize 0, metadata asize <n>, l2_size <n>, l2_asize <n>
fo flush (s2 to s3): evict_l2_cached +<n, metadata only>, evict_l2_eligible +<n>, evict_l2_ineligible +<at least N x 65536>, evict_l2_skip +<0 or a few>, evict_skip +<n>
fo read 2 (s3 to s4): l2_hits +<a few, metadata>, l2_misses +<n>, l2_read_bytes +<n>, l2_cksum_bad +0, l2_io_error +0
fo at the end (s5): l2_write_bytes <n>, l2_hits <n>, l2_cksum_bad 0, l2_io_error 0
fo zpool status after read 2:
  ...
  errors: No known data errors
```

**This is the fix**: no kernel message (a debug build: the old assertion
would have panicked here), no data block in the L2ARC, the blocks it
skipped counted as `evict_l2_ineligible` at the flush (the fix cleared their
`ARC_FLAG_L2CACHE`), every read correct, `l2_cksum_bad` 0. The userspace run
wrote 24064 bytes of metadata to the L2ARC; a ZPL dataset has a little more.

## STEP 6: the control: FD writes `wf` and reads it (run `ff`)

FD is reloaded to write `wf` with its default tunables, and again to read it
with a fresh ARC, as MR and FD read `wo`:

```sh
. /var/log/zts-l2mac-1028/env.sh
switch_to FD lw && write_pool wf FD && switch_to FD lc && l2run ff FD $W/wf
```

Expected: `lw` as `lo` with FD and `<SV_FD>`, then the `wf` lines as the
`wo` lines of STEP 3 (with `$W/wf`), then `lc` and the run `ff` as `mo`
with `ff` and `$W/ff`, except:

```
ff: feed: <s>s, l2_feeds +<n>, l2_writes_sent <n>, done <n>, l2_write_bytes <about 1.2 to 1.4 million>, settled
ff read 2: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
ff after the feed: data asize <N x 20480>, metadata asize <n>, l2_size <n>, l2_asize <n>
ff flush (s2 to s3): evict_l2_cached +<at least N x 65536>, ...
ff read 2 (s3 to s4): l2_hits +<at least N>, l2_misses +<n>, l2_read_bytes +<n>, l2_cksum_bad +0, l2_io_error +0
ff at the end (s5): l2_write_bytes <n>, l2_hits <n>, l2_cksum_bad 0, l2_io_error 0
ff zpool status after read 2:
  ...
  errors: No known data errors
```

**This is the control**: FD recompresses the blocks it wrote to the same
bytes, so their MACs match and they are written to the L2ARC and read back
from it, correctly. In userspace, the same control wrote 1313280 bytes and
read 70 buffers back from the L2ARC with no checksum error.

## STEP 7: the three runs side by side

```sh
. /var/log/zts-l2mac-1028/env.sh
{
for r in mo fo ff; do
	if [ ! -s $L/$r-s5.as ]; then
		echo "$r: not complete"
		continue
	fi
	d=$(val $r s2 l2_bufc_data_asize)
	h=$(dl $r s3 s4 l2_hits | awk '{ print $2 + 0 }')
	c=$(dl $r s3 s4 l2_cksum_bad | awk '{ print $2 + 0 }')
	ct=$(val $r s5 l2_cksum_bad)
	set -- $(rcounts $r 2)
	ok2=$1 eio2=$2 diff2=$3 err2=$4
	set -- $(rcounts $r 3)
	ok3=$1
	echo "$r: data asize after the feed $d ($((d / 20480)) blocks); read 1 [$(rcounts $r 1)]," \
	    "read 2 [$ok2 $eio2 $diff2 $err2] l2_hits +$h l2_cksum_bad +$c, read 3 [$(rcounts $r 3)] (ok EIO DIFFERS ERROR)"
	case $r in
	mo) [ "$d" -gt 0 ] && [ "$eio2" -gt 0 ] && [ "$diff2" -eq 0 ] && [ "$c" -gt 0 ] && [ "$ok3" -eq 64 ] ;;
	fo) [ "$d" -eq 0 ] && [ "$ok2" -eq 64 ] && [ "$ok3" -eq 64 ] && [ "$ct" -eq 0 ] ;;
	ff) [ "$d" -gt 0 ] && [ "$ok2" -eq 64 ] && [ "$ok3" -eq 64 ] && [ "$h" -ge $((d / 20480)) ] && [ "$ct" -eq 0 ] ;;
	esac && echo "  $r: as expected" || echo "  $r: NOT AS EXPECTED"
done
for t in O MR FD; do
	echo "$t srcversion $(cat $L/srcv-$t)"
done
} 2>&1 | tee $L/compare.txt
```

Expected:

```
mo: data asize after the feed <N x 20480> (<N> blocks); read 1 [64 0 0 0], read 2 [<64 - N> <N> 0 0] l2_hits +<at least N> l2_cksum_bad +<N>, read 3 [64 0 0 0] (ok EIO DIFFERS ERROR)
  mo: as expected
fo: data asize after the feed 0 (0 blocks); read 1 [64 0 0 0], read 2 [64 0 0 0] l2_hits +<a few> l2_cksum_bad +0, read 3 [64 0 0 0] (ok EIO DIFFERS ERROR)
  fo: as expected
ff: data asize after the feed <N x 20480> (<N> blocks); read 1 [64 0 0 0], read 2 [64 0 0 0] l2_hits +<at least N> l2_cksum_bad +0, read 3 [64 0 0 0] (ok EIO DIFFERS ERROR)
  ff: as expected
O srcversion <SV_O>
MR srcversion <SV_MR>
FD srcversion <SV_FD>
```

`NOT AS EXPECTED` is a result, not a stop: "What the results mean" says how
to read it.

## STEP 8: unload, report, cleanup

Run these blocks in all cases, a stop included, except after a stop in
STEP 0 or at STEP 1's `mkdir`. First the whole task's kernel check and the
unload; the module stays loaded on `HELD` or `PANIC`, and then no `zpool`
command runs:

```sh
. /var/log/zts-l2mac-1028/env.sh
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso > $L/kern-all.txt 2>&1
echo "kernel, whole task: $(grep -iE "$KRE" $L/kern-all.txt | grep -vcE "$KIGN") findings"
grep -iE "$KRE" $L/kern-all.txt | grep -vE "$KIGN" | head -n 40
echo "kernel, this boot: $(sudo -n journalctl -k -b --no-pager -q | grep -iE "$KRE" | grep -vcE "$KIGN")"
t=$(cat $L/loaded 2>/dev/null)
if [ -e $L/HELD ] || [ -e $L/PANIC ]; then
	echo "module left loaded: $(cd $L && ls HELD PANIC 2>/dev/null | xargs); no pool command run"
elif [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(sv_of $t)" ]; then
	p=$(zc $t zpool list -H -o name 2>&1)
	if [ -n "$p$(grep ' zfs ' /proc/mounts)" ]; then
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
echo "loop devices: [$(sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1 | xargs)]"
echo "mounts under W: [$(grep " $W[ /]" /proc/mounts)]"
echo "zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"
echo "vdev_id.conf: $(ls -l /etc/zfs/vdev_id.conf 2>&1)"
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "free: $(df -h /var/tmp | tail -n 1)"
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings`, no kernel line, the boot's count
equal to the STEP 0 baseline, `unload FD rc=0`, `lsmod: []`, `loop devices:
[]`, `mounts under W: []`, a 0-byte or absent `zpool.cache`, unchanged, no
`vdev_id.conf`, `W holds: FD MR O ff fo key mnt mo repo src wf wo`, the free
space.

Then assemble `REPORT.txt`:

```sh
. /var/log/zts-l2mac-1028/env.sh
{
echo "VM 107: L2ARC MAC mismatch, O 71a9f95786 (zfs-2.4.4), MR 5ebd13aa9a (release), FD 288026c17a (debug) (v/VM-TESTS-107-l2mac.md)"
for s in record fix trees build build-checks data runs compare end; do
	echo
	echo "== $s.txt"
	cat $L/$s.txt 2>&1
done
echo
echo "== summary"
for t in O MR FD; do
	echo "$t srcversion $(cat $L/srcv-$t 2>/dev/null)"
done
for w in wo wf; do
	grep -E "^$w read 1: |^kernel $w:|^after $w:" $L/runs.txt
done
for r in mo fo ff; do
	grep -E "^$r: feed: |^kernel $r(-feed|-read2)?:|^after $r:" $L/runs.txt
	cat $L/digest-$r.txt 2>/dev/null
done
grep -vE '^[A-Z]+ srcversion ' $L/compare.txt 2>/dev/null
echo "flags: STOP [$(cat $L/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} > $L/REPORT.txt 2>&1
wc -l $L/REPORT.txt
```

Append, by hand, the STEP 0 output (short: anything found, `uname -r`, the
memory line, the free space, the `vm-task-107t` line, the doc checkout line
and its sha256, the kernel baseline, the tools lines), a `verdict:` line
naming the case of "What the results mean" that applies, and a `deviations:`
line with every deviation from these steps and its reason (`none` if there
was none). Then `cat /var/log/zts-l2mac-1028/REPORT.txt`. Expected summary:

```
O srcversion <SV_O>
MR srcversion <SV_MR>
FD srcversion <SV_FD>
wo read 1: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
kernel wo: 0 findings (<n> lines)
after wo: nothing left
wf read 1: ok x64, EIO x0, DIFFERS x0, ERROR x0; not ok []
kernel wf: 0 findings (<n> lines)
after wf: nothing left
mo: feed: <s>s, ..., settled
kernel mo-feed: 0 findings (<n> lines)
kernel mo-read2: 0 findings (<n> lines)
kernel mo: 0 findings (<n> lines)
after mo: nothing left
<mo's digest, as in STEP 4>
<fo's lines and digest, as in STEP 5>
<ff's lines and digest, as in STEP 6>
<the six lines of STEP 7 without the srcversions: three "as expected">
flags: STOP [] HELD [] PANIC []
verdict: as expected (first case)
deviations: none
```

Then, as the very last command, and only if the unload block printed
`unload FD rc=0` with `lsmod: []` and `mounts under W: []`, and no step
stopped, remove this task's directories and the doc checkout (the doc
checkout only if it is still the clean checkout STEP 1 recorded, holding this
document, and nothing else). `$L` sits in root's `/var/log`: its contents go
with `rm -rf`, the then empty directory with `sudo -n rmdir`:

```sh
cd /
D=/var/tmp/vm-task-107v
if grep -q " /var/tmp/zts-l2mac-1028[ /]" /proc/mounts; then
	echo "something is mounted under /var/tmp/zts-l2mac-1028: nothing removed"
else
	if [ -d $D ] && [ ! -L $D ] && [ "$(stat -c %U $D)" = tigerblue77 ] && [ -d $D/.git ] && [ ! -L $D/.git ] &&
	    [ "$(git -C $D rev-parse --show-toplevel 2>/dev/null)" = $D ] &&
	    [ -s /var/log/zts-l2mac-1028/doc-head.txt ] &&
	    [ "$(git -C $D rev-parse HEAD 2>/dev/null)" = "$(cat /var/log/zts-l2mac-1028/doc-head.txt)" ] &&
	    git -C $D ls-files --error-unmatch v/VM-TESTS-107-l2mac.md > /dev/null 2>&1 &&
	    [ "$(sha256sum < $D/v/VM-TESTS-107-l2mac.md | cut -d' ' -f1)" = "$(cat /var/log/zts-l2mac-1028/doc-sha.txt 2>/dev/null)" ] &&
	    [ -z "$(git -C $D status --porcelain --ignored --untracked-files=all 2>&1)" ] &&
	    [ "$(LC_ALL=C ls -A $D | grep -vx .git | xargs)" = "$(git -C $D ls-tree --name-only HEAD | LC_ALL=C sort | xargs)" ] &&
	    ! grep -q " ${D}[ /]" /proc/mounts; then
		X=$D
	else
		X=""; echo "$D kept: not only the clean doc checkout STEP 1 recorded"
	fi
	rm -rf /var/tmp/zts-l2mac-1028 $X /var/log/zts-l2mac-1028/*; echo "rm rc=$?"
	sudo -n rmdir /var/log/zts-l2mac-1028; echo "rmdir rc=$?"
fi
ls -d /var/tmp/zts-l2mac-1028 /var/log/zts-l2mac-1028 $D 2>&1   # No such file or directory, three times
```

If the unload did not happen, or a step stopped, run nothing of this block:
keep everything and say so. If your permission system refuses this `rm -rf`
or this `rmdir`, leave what is left and say so; do not delete it any other
way. Then reply with all of `REPORT.txt` as printed, and the result of the
last block: nothing runs after the reply.

## What the results mean

- **As expected** (STEP 7's three `as expected`; `mo`: data blocks in the
  L2ARC after the feed, read 2 EIO for them with `l2_cksum_bad` the same
  number, read 3 correct; `fo`: data asize 0, every read correct,
  `evict_l2_ineligible` at the flush; `ff`: data blocks in the L2ARC, read 2
  correct with `l2_hits` at least their number; `l2_cksum_bad` 0 on FD; no
  kernel finding; a clean unload): on a kernel, a release build of master
  writes to the L2ARC copies of zstd blocks written by zfs-2.4.4 that can
  never be read back, and their reads return EIO although the data on disk is
  intact; the fix writes none of them, without an assertion in a debug build,
  and still caches the encrypted blocks the running compressor reproduces.
- **`mo` writes no data block** (data asize 0): the bug did not show. Check
  STEP 2's zstd versions (`1.4.5` in O, `1.5.7` in MR), that `wo`'s
  `compressratio` shows compressed blocks, and `mo`'s tunables line; report
  `$L/mo-s2.arcstats`.
- **`mo`'s read 2 returns the data** with `l2_cksum_bad` > 0: the bad copies
  were written and failed their checksum, but the retry from the pool worked
  on this kernel (the size problem of the pull request named above did not
  show). Still the bug; report it so.
- **`mo`'s read 2 has `DIFFERS` lines**: a read returned other data without an
  error: a finding beyond this task's; report `$L/mo-r2.txt`.
- **`fo` writes data blocks** (data asize > 0) or has `l2_cksum_bad` > 0:
  the fix does not hold in the kernel. Report `fo`'s digest and arcstats.
- **`ff` writes no data block, or read 2 has fewer `l2_hits` than blocks**:
  either the fix stops normal caching, or FD does not reproduce its own
  blocks; compare with `fo` and report both digests.
- **`NOT SETTLED`**: the L2ARC was still writing after 120 s; read the run's
  numbers with that in mind.
- **Any kernel finding**: the line and the run are the result. Stop.

Estimated duration: about 45 minutes (checks 5, fetch and worktrees 5, the
builds 15, the data and `wo` 3, each run 2 to 3, the control 5, the end 3).
