# VM task: the hybrid RAID-Z parity mover in the kernel (spike/kernel-hybrid-v2)

Self-contained task for a **disposable Debian 13 VM** (kernel 6.12, no pool
on it that matters, reboots allowed). Nothing is posted anywhere; results
stay on the VM and are copied back by hand. About 1.5 to 2 h, 6 reboots.
The second run of this task: the first, on fork branch `spike/kernel-hybrid`
(91df27c68d), passed most checks and found three problems, which fork
branch **`spike/kernel-hybrid-v2`** addresses. Run everything again.

## What is being tested

The RAID-Z parity prototype "hybrid" (rewrite what a head owns, redirect
the rest) as a kernel module: `module/zfs/vdev_raidz_mover.c`, started by
a test-only module parameter:

```sh
P=/sys/module/zfs/parameters/raidz_spike_mover
echo "epoch jointfixture" > $P            # adds {0,5,1, T,5,2} to raidz vdev 0
echo "move jointfixture hybrid" > $P      # or redirect; [head=<ds>] [force] [nogang]
echo "census jointfixture" > $P           # count only (read-only pools too)
cat $P   # "running ...", or the last result: key=value ... ms=N rc=E
```

`rc=0` done; `rc=28` refused by the pre-flight; `rc=11` some DVA refused or
the last census not 0. `census_left` is the old DVAs no live redirect
covers (must be 0), `zil_old` the old ZIL DVAs (never moved), `entries`,
`damaged`, `live` (entries reached from block pointers) are the table's,
`gang_small` the gang headers read that have the old, fixed size.

What the first run found, and what v2 changes (each reproduced and checked
in userspace, with `kmover_user`, the same module code through the same
entry points, before and after):

- **(c) `leaked space` after `zfs destroy -r <pool>/fill`: not the mover.**
  The fixture pool has no feature but `raidz_parity_epochs`, so a destroy
  there is synchronous, and its traversal frees a dataset's intent log only
  while the log is claimed. Each import creates the volume's minor, whose
  `zil_replay()` resets the clean log but its first block, unclaimed: the
  destroy left that block allocated (51 sectors, a 120K block at parity 2,
  from reasoning). Reproduced in userspace with the volume's writes through
  its log, with and without the mover, and on upstream master; fixed in
  `dsl_destroy.c`: the synchronous destroy frees the log with
  `zil_destroy_sync()`, as the asynchronous ones do.
- **(d) `census_err=52` on the kernel's pool.** With
  `feature@dynamic_gang_header` active, a gang header written before the
  feature became active (a gang write activates it) holds 512
  bytes, its checksum at their end; the census and the move's liveness
  check checksummed every header at the dynamic size. Both now try the old
  size too, as `zio_checksum_error()` does (userspace, feature enabled:
  `census_err=52` before; after, `gang_small=256`, `rc=0`, census 0, data
  and zdb PASS).
- **(a) p100 hybrid gave 116/816 entries instead of 130/830.** The kernel's
  ZPL registers a file information callback for `DMU_OST_ZFS` when the
  module loads; the fixtures' root is such an objset, created by libzpool
  without user accounting, so the mover's read-write ownership of it
  started the user accounting upgrade, which rewrote its dnodes at the new
  parity during the move, as far as the stop at each disown allowed
  (timing-dependent). The mover now owns the head with
  `dmu_objset_own_obj()`, which starts no upgrade; `kh.sh` runs the
  userspace leg with the same callback registered (`zpl-accounting`).

**Not established, the point of this task**: every run of v2 in a kernel.

## Safety rules

- Disposable VM only. `scripts/zfs.sh` loads the tree's modules; `kh.sh`
  imports, exports and creates pools named `jointfixture` and `khpool` on
  files under `/var/tmp/kh` (about 6 GB at most), sets `metaslab_force_ganging` for a
  moment (restored), and `kh.sh b-cut` **reboots the VM** (sysrq `b`).
- Run as root. Post nothing. Never run two `kh.sh` at once.
- Anything past its bound (every step has one, in `kh.sh`): save `dmesg`,
  `/proc/spl/kstat/zfs/dbgmsg` and `cat $P`, then reboot before going on.

## Files needed on the VM

Fork branch `spike/kernel-hybrid-v2` of https://github.com/tigerblue77/zfs
carries the tree to build, this document and `kh.sh`, both in
`contrib/raidz-redirect-spike/vm/`. On a VM that ran the first task, keep
its evidence and unload its modules first (or reboot):

```sh
zpool list 2>/dev/null                    # nothing of the task may be imported
[ -d /root/kh/zfs ] && /root/kh/zfs/scripts/zfs.sh -u
[ -d /var/tmp/kh ] && mv /var/tmp/kh /var/tmp/kh-v1
[ -d /root/kh/zfs ] && mv /root/kh/zfs /root/kh/zfs-v1
```

## 1. Prepare the VM (once, about 10 min)

```sh
export DEBIAN_FRONTEND=noninteractive
timeout 1800 apt-get update -y
timeout 3600 apt-get install -y build-essential git autoconf automake \
    libtool libtool-bin gawk alien fakeroot dkms bc cpio libaio-dev \
    libattr1-dev libblkid-dev libcurl4-openssl-dev libelf-dev libffi-dev \
    libpam0g-dev libssl-dev libtirpc-dev libudev-dev uuid-dev zlib1g-dev \
    python3 python3-cffi python3-dev python3-packaging python3-setuptools \
    kmod "linux-headers-$(uname -r)"
lsmod | grep -w zfs; zpool list 2>/dev/null   # must show nothing that matters
```

## 2. Sources and build (about 20-30 min)

```sh
mkdir -p /root/kh && cd /root/kh
timeout 1800 git clone -b spike/kernel-hybrid-v2 https://github.com/tigerblue77/zfs.git zfs
git -C zfs rev-parse HEAD^       # must be ae8dc980e9d1a83b5dd41b5343b0378985706259
cp zfs/contrib/raidz-redirect-spike/vm/kh.sh /root/kh/kh.sh && chmod +x /root/kh/kh.sh
sha256sum /root/kh/kh.sh         # must be 2984adb7ac761d7b08f6824d7a1dbb9d3a61b5e7525ff07951a70eefe5bd9215
git init -q gen-src && timeout 1800 git -C gen-src fetch -q --depth 1 \
    https://github.com/rdmitry0911/zfs.git 6319a29f86a9771c62a4c59b029c9c18a839998b
git -C gen-src checkout -q FETCH_HEAD && git -C gen-src rev-parse HEAD   # 6319a29f86...
ln -sfn /root/kh/gen-src/contrib/raidz-parity-fixtures /root/kh/fixtures
(cd zfs && timeout 1800 ./autogen.sh && timeout 1800 ./configure --enable-debug &&
    timeout 5400 make -j"$(nproc)") > /root/kh/build.log 2>&1; echo "build rc=$?"
ls -l zfs/module/zfs.ko zfs/module/spl.ko; grep -E 'error:|warning:|undefined' /root/kh/build.log | head
timeout 3600 /root/kh/kh.sh setup; echo "setup rc=$?"
```

Expected: build rc=0, both `.ko` present, no `undefined!` from modpost;
`setup` builds the unchanged generator (its `build.py`) and `kmover_user`
against this tree, loads the modules (`srcversion`, `trigger` PASS), and
creates 10 inputs: 8 cases of 100 units (`r128|r16` x `p0|10|30|100`),
`r16-p30-u1000` and `r16-p100-u1000` (125 MiB). Failure signature: a compile error or modpost `undefined!` in
`vdev_raidz_mover.c`/`vdev_raidz.c` (a kernel-only API problem: report
the lines), `Large kmem_alloc` or a trace in `dmesg` at load.

Nothing is installed: `kh.sh` runs the tree's own `zpool`, `zfs`, `zdb`
and `scripts/zfs.sh`. It writes `CHECK <case> <name> PASS|FAIL`,
`MOVER <case> <result>`, `RESULT` and `INFO` lines to `/var/tmp/kh/kh.log`;
`kh.sh summary` prints every FAIL, RESULT and INFO line and the totals. Each case also checks
that no new `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING:|hung_task|blocked for
more|soft lockup|Large kmem_alloc|Call Trace` appeared in `dmesg`
(`dmesg_clean`), and saves `dmesg` and `dbgmsg` to `/var/tmp/kh/log/`.

## 3. (a) The shared fixtures through the kernel mover (about 15 min)

```sh
timeout 3600 /root/kh/kh.sh a; echo "a rc=$?"; /root/kh/kh.sh summary | grep ' a-'
```

For each of the 16 runs (8 inputs x hybrid/redirect), on a fresh sparse
copy: kernel import `-N`, `feature@physical_rewrite=enabled` (as the 1 Nov
re-run did for both movers), `epoch`, `move`, export; generator `verify`;
read-only kernel import + `census`; generator `drop-retained`; read-only
`census`; `verify`; read-write import + `zpool scrub -w`; `zdb -bcc`. Then
the same input through `kmover_user` (userspace), for comparison, with
the file information callback the kernel's ZPL registers (`zpl-accounting`).

Expected: every CHECK PASS: `move_rc0`, `census0`, `verify1/2`,
`cold_census0`, `cold_entries` (= after the move), `cold_reached`
(live = entries), `cold_damaged0`, `drop_retained`, `scrub0` (repaired 0B,
0 errors, no counter on any vdev), `zdb` (No leaks), `as_user_*` (kernel
= userspace for entries, rewritten, redirected, kept, census_left, entries
after drop). Each `RESULT a-...` line should say `(SAME)` against 1 Nov
tree D, the p100 hybrid runs included (130 and 830 entries, 14 after the
drop, `round1=14` in the kernel as in userspace: the head's metadata is
redirected in round 1, not rewritten by an upgrade); DIFFERENT is not a
failure in itself but must be reported with both lists. Failure signatures: `census_left` > 0 or `rc=11` (refused
DVAs, `errs=<errno>:<n>`), `verify` FAIL (wrong bytes through a redirect),
`damaged` > 0 or a refused read-write import (EROFS: the table did not
load), zdb `leaked space` / claim errors, a kernel/userspace difference.

## 4. (b) Crash cuts: sysrq `b` during a move (about 25 min, 6 reboots)

```sh
timeout 1800 /root/kh/kh.sh b-dry      # times both movers on r16-p30-u1000
                                       # (userspace: hybrid 7.7 s, redirect 26.4 s)
/root/kh/kh.sh b-cut hybrid 20         # the VM reboots
/root/kh/kh.sh b-check                 # after the reboot, once logged in
```

Then the same `b-cut`/`b-check` pair for `hybrid 50`, `hybrid 80`,
`redirect 20`, `redirect 50`, `redirect 80`. `b-cut` prepares a copy
(import, feature, epoch), `sync`s, starts the move, and `echo b >
/proc/sysrq-trigger` after pct % of the dry run's time, with no `sync` in
between: what the guest had not written out of its page cache (the vdev
files' pages ZFS did not flush) is lost, as in a power cut of the guest;
the VM's disk must honour flushes (not qemu `cache=unsafe`). `b-check` loads the modules, imports `-f`, censuses, exports,
verifies, runs zdb, then moves again to the end, scrubs, verifies,
drop-retained, verifies, zdb.

Expected: dry runs PASS. After every cut: import succeeds, the census runs
with `damaged0`, `RESULT ... the cut came during the move` (census_left >
0; if it says AFTER, redo that cut with a smaller pct and report both),
`verify1` and `zdb1` PASS on the cut state (each sync task's moves are all
there or all absent: the state before or after each txg), the second move
reaches `census0`, then `scrub0`, `verify2/3`, `drop_retained`, `zdb2`.
Failure signatures: import fails or the table is refused (EROFS,
`damaged` > 0, dbgmsg `rejecting redirect log`), zdb `leaked space` (N
allocated without its entry, or the reverse), claim errors (a kept tail
freed without its entry), `verify` FAIL or ECKSUM, the second move
refusing blocks.

## 5. (c) A pool too full for the move (about 10 min)

```sh
timeout 5400 /root/kh/kh.sh c; echo "c rc=$?"
```

On `r16-p100-u1000` (8046 old DVAs, the data held by the snapshot alone):
epoch, then 450 MiB written at parity 2 into a sparse volume `fill`
(volblocksize 128K, through `/dev/zvol/...` or `/dev/zd0`; a volume
because no file system can be created under the fixture's root, which has
no ZPL). `c-refused`: `move redirect`.
`c-forced`: `move redirect force`. `c-freed`: destroy `fill`, move again.

Expected: c-refused `rc=28`, `why=refused_by_preflight`, nothing moved
(`entries=0`), verify and zdb PASS; the MOVER line shows `pf_free`,
`pf_need_end`, `pf_packed` < `pf_new` or `pf_left` < `pf_need_left`
(userspace: free 277 MB, new extents 297 MB, 275 MB packed). c-forced:
`rc=11`, every refusal ENOSPC (`errs=28:<n>`; userspace, the volume
written through its log: 7249 to 7280 moved, the rest refused in each of
4 passes, 766 to 797 left), pool not suspended (`not_suspended`), verify
and zdb PASS; then an `INFO c-forced fill log first block: DVA[0]=<...>`
line, the volume's block that the destroy left allocated in the first run
(v1 zdb3: vdev 0, offset 0x20048000, 208896 bytes, not checked against
that run's log). c-freed: census 0, scrub, verify, **zdb3 PASS** (v1:
`leaked space`). Failure signatures: the pre-flight
accepts (`preflight=0`), a refusal other than 28, the pool SUSPENDED or a
panic in the sync thread (an allocation of the move or of the MOS failing
in syncing context), zdb leaks: give the `leaked space` lines and the
INFO line.

## 6. (d) Gang blocks and copies=2 (about 10 min)

```sh
timeout 3600 /root/kh/kh.sh d; echo "d rc=$?"
```

On a pool `khpool` the kernel creates (raidz1 of five 256 MiB files,
ashift 12, every feature enabled, dynamic_gang_header among them, which
the fixture and the userspace gang run had disabled), since the fixture's
root takes no file system: dataset `g` with `copies=2`, 8 files of 1 MiB
written with `metaslab_force_ganging=32769` and
`metaslab_force_ganging_pct=100`, snapshot `g@s`, 4 files overwritten,
ganging restored, `g` unmounted (so the hybrid can own it), `zdb -bb` must
show `ganged count` > 0. Then epoch and `move hybrid head=khpool/g`
(`d-gang`), or the same with `nogang` (`d-nogang`).
Files of `g` and of a clone of `g@s` are compared with their sources.

Expected d-gang: `gang_found` (gang_dvas > 0), `old_size_headers`
(`gang_small` > 0: the v1 run failed here with `census_failed
census_err=52`), `rc=0`, `census0`, data of head and snapshot identical,
scrub0, zdb PASS (userspace, `kh_gang.c`'s pool, the same as this one:
2008 old DVAs, 1928 gang, 256 old-size headers, 1597 redirected, 32
rewritten). d-nogang:
`rc=11`, census_left = gang_left > 0 (userspace 192): the probe's mover
cannot move gang blocks, and the census says so. Failure signatures:
census_left > 0 with gang_left 0 (a gang child not reached), ESTALE
refusals (`errs=116:<n>`: the liveness check rejected a gang child),
ECKSUM or a mismatch reading `g` or the clone, zdb leaks.

## 7. (e) Keyless encrypted dataset with fsync'ed writes in its ZIL (about 5 min)

```sh
timeout 3600 /root/kh/kh.sh e; echo "e rc=$?"
```

On `khpool`, as in (d): dataset `enc` (aes-256-gcm, raw key in a file),
4 base files synced, snapshot `enc@s1`, one base file overwritten, a 1-byte fsync file (so a ZIL header exists, as
`slog_replay_fs_001` does), `zpool freeze`, then 8 files of 16 KiB in 4K
writes and 4 of 512 KiB in 128K writes, each `dd conv=fsync`
(acknowledged, never synced to the main pool; 4K writes are logged as
WR_COPIED, 128K ones as WR_INDIRECT by default, from reading), unmount,
export. Import `-N` **without the key** (`keyless`: keystatus
unavailable), epoch, census, `move hybrid`, export, import, census, then
`zfs load-key` and `zfs mount` (replays the ZIL).

Expected: `zil_old_before` > 0, `move_rc0`, `census0` with the ZIL still
there (`zil_kept`), cold census 0 and damaged 0, `mount_replays`, every
fsync'ed file and every base file identical to its source
(`acked_writes`, `base_files`), the snapshot's base file too, after the
replay `zil_old` 0, then scrub0 and zdb PASS. Failure signatures:
the move refusing a batch with EINVAL (`errs=22:`: a ZIL block reached the
move), ESTALE/EACCES refusals (`errs=116:` or `13:`: keyless liveness
check), `zfs mount` failing (ZIL replay EIO/ECKSUM, dbgmsg `zil_replay`),
an acknowledged file missing, short or different, zdb leaked space.

## 8. (f) A child offlined during a move, onlined, resilver, scrub (about 10 min)

```sh
timeout 5400 /root/kh/kh.sh f; echo "f rc=$?"
```

On `r16-p30-u1000`: epoch, `move redirect` (8048 DVAs, 26 s in
userspace), 3 s into it `zpool offline` of `v2` (`offline_during_move`:
the mover was still running), wait for the move, `zpool online`, `zpool wait -t resilver`,
`zpool scrub -w`. `f-replace`: `zpool replace` of `v3` by a new file,
resilver, scrub.

Expected: `move_rc0` and `census0` while DEGRADED, the resilver ends
(`healthy`: ONLINE), `scrub0`: the scrub repairs **0B** with no CKSUM on
`v2`, which proves the resilver rebuilt v2's columns of every new extent
written while it was offline (the table walk of step 6), census 0, verify
and zdb PASS; f-replace the same. Failure signatures: the scrub after the
resilver finds checksum errors on `v2` and repairs bytes (crash_probe's
`rpass R1 none`, without the walk, found 5 to 9), the resilver never
ends, the move refused with EIO (`errs=5:`), a panic.

## Evidence to collect

`/var/tmp/kh/kh.log`, `/var/tmp/kh/log/` (dmesg and dbgmsg per case), the
per-run logs next to each run directory (`*.verify*.log`, `*.zdb*.log`,
`*.status*`, `*.zil.log`), `/root/kh/build.log`'s tail and every
warning/error line of it, `dmesg > /root/kh/dmesg-final.txt`, and the
output of `kh.sh summary`. Run directories can be deleted afterwards.

## Report format

```
VM: <name>, Debian 13, kernel <uname -r>, <nproc> CPUs, <RAM>
tree <git -C zfs rev-parse HEAD> built --enable-debug: rc=<0|n>, modpost undefined: <none|list>
srcversion match: yes|no; checks: <n> PASS, <n> FAIL (kh.sh summary)
(a) 16 runs: <n>/16 all PASS; kernel = userspace in <n>/16; SAME as 1 Nov
    in <n>/16; p100 hybrid round1 kernel/user: <r128> <r16>;
    mover_ms kernel vs user: <r128 hybrid> ... (one line)
(b) cut  mode     pct  delay_s  left_at_import  entries  checks
         hybrid   20   ...      ...             ...      PASS|FAIL <names>
         ... (6 rows)
(c) refused: rc, pf_free, pf_new, pf_packed, pf_left, pf_need_left;
    forced: redirected, refused, errs, census_left, health; the INFO
    "fill log first block" line; freed: census, zdb3 PASS|FAIL
(d) gang: census_dvas, gang_dvas, gang_small, redirected, rewritten, rc; nogang:
    census_left, gang_left, rc; data PASS|FAIL
(e) zil_old before/after move/after replay, files PASS|FAIL, mount rc
(f) offline at <s> s, move rc, resilver time, scrub line, replace scrub line
dmesg: every new VERIFY/ASSERT/WARNING/trace line, verbatim
Every FAIL line of kh.sh summary, verbatim; anything skipped and why.
```
