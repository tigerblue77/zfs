# VM 106 (CHSONVTST03): LZ4 1.10.0 update, ZTS and cross-version kernel reads

These instructions produce kernel evidence for the fork branch `lz4-1.10` of
https://github.com/tigerblue77/zfs. They start from the build half sent
earlier, which built two trees on this VM under `/var/tmp/zts-lz4-1026` and
wrote its logs to `/var/log/zts-lz4-1026`. Nothing is rebuilt here.

The branch is three commits on master `a128b8a075b5b13c737e56914cd7043bd3e9ebf8`:
the unmodified import of the LZ4 v1.10.0 library (`61d511b304`), its SPDX tags
(`ad8fa09d6b`), and the integration that replaces the r85 compressor
(`module/zfs/lz4_zfs.c`) and the 1.9.3 decoder (`module/zfs/lz4.c`) with glue
over the new library (`61d3f825aaf668d348c7905c17f8f2ff4b09f8b4`, the head). The
LZ4 block format on disk is unchanged; the compressed *bytes* a block gets are
not, for some block sizes.

| State | Tree | Code |
| --- | --- | --- |
| B | `/var/tmp/zts-lz4-1026/B` | branch head `61d3f825aaf668d348c7905c17f8f2ff4b09f8b4` (master + the three LZ4 commits) |
| M | `/var/tmp/zts-lz4-1026/M` | master `a128b8a075b5b13c737e56914cd7043bd3e9ebf8` (the r85 compressor, the 1.9.3 decoder) |

The run has two parts:

1. **ZTS on both trees, B first**, over the groups that exercise the LZ4
   compressor and the code paths the change touches: `compression`, `rsend`
   (the compressed send/receive and embedded-block tests live here), and
   `cli_root/zfs_receive` (corrective receive, receive from zstd). Each group
   runs on B, then on M; a test that fails on B is rerun once, alone, on M to
   tell a branch regression from a VM or master effect.
2. **Cross-version pools in the kernel**, with each module loaded in turn: a
   pool written by one tree's compressor, exported, and read back by the other
   tree, with compressed ARC on and off, plus `zdb -bcc` from each tree's
   `zdb`. This exercises the one path a changed compressor output can break:
   `arc_hdr_authenticate()` re-compresses the authenticated-only metadata of an
   encrypted dataset to check its MAC when compressed ARC is off, so a block
   whose bytes differ between the two LZ4 versions fails the check.

## Why these groups, and what to expect from them

The change is a drop-in compressor/decoder swap: same block format, possibly
different compressed bytes. The groups below are the ones whose work actually
goes through the LZ4 compressor or the paths that re-handle its output.

- **`compression`** (8 tests): `compress_001_pos`..`compress_004_pos` set
  `compression=lz4`/`on`/`off` and check the ratio and the data; the four
  `l2arc_compressed_arc`, `l2arc_compressed_arc_disabled`, `l2arc_encrypted`
  and `l2arc_encrypted_no_compressed_arc` tests exercise the compressed-ARC and
  encrypted-L2ARC transforms, which re-compress and re-authenticate stored
  blocks. These are the most direct exercise of the compressor and of the
  MAC/`l2arc` re-compression paths, within a single version (so the bytes
  always match and nothing should fail).
- **`rsend`** (81 common + 3 Linux tests): the `send-c_*` tests (compressed
  send and receive, `send-c_verify_ratio`, `send-c_verify_contents`,
  `send-c_lz4_disabled`, `send-c_recv_lz4_disabled`, `send-c_mixed_compression`,
  `send-c_compressed_arc_disabled`, `send-c_embedded_blocks`, ...), the
  `send_embedded_*` tests (embedded block pointers carry an LZ4 payload), and
  the raw/large-block send tests. A compressed send stream keeps the source's
  stored bytes, so a within-version run reproduces them exactly.
- **`cli_root/zfs_receive`** (31 tests): `zfs_receive_corrective`,
  `zfs_receive_compressed_corrective` and `zfs_receive_large_block_corrective`
  decompress and re-compress a block to heal it (`dmu_recv.c`), and
  `zfs_receive_from_zstd` crosses LZ4 (zstd's early-abort probe compresses with
  LZ4). All run within one version here, so they should pass on both trees.

The `l2arc` group itself (persistence, throughput, scaling) is **not** run: its
compressor-relevant cases are the four `l2arc_*` tests already in the
`compression` group, and its throughput tests are slow and noisy on a shared
hypervisor, which would risk the per-tree time budget without adding LZ4
coverage. The embedded-data cases that matter for LZ4 are the `send*embedded*`
tests inside `rsend`; `bclone_*_embedded` and `redacted_embedded` test the block
clone and redaction machinery, not the compressor, and are left out. `history`
does not exercise the compressor and is left out; the LZ4-named tests
(`send-c_lz4_disabled`, `send-c_recv_lz4_disabled`) are in `rsend` and are run.

Expected per group, on **each** tree (the change is within-version here, so B
and M should match except where the VM differs):

- `compression`: 8 tests, all PASS (plus the section `setup` and `cleanup`).
- `cli_root/zfs_receive`: 31 tests, all PASS (plus `setup`/`cleanup`); a
  handful may report SKIP/UNSUPPORTED for missing utilities, which `zts-report`
  lists as expected.
- `rsend`: 84 tests + two sections' `setup`/`cleanup` = 88 results.
  `rsend_008_pos` is SKIP (`log_unsupported "Occasionally hangs"`, issue 6066).
  **This VM has no `60-zvol.rules`** (checked in STEP 0): `/dev/zvol/...` links
  never appear, so the four tests that need a zvol device node fail at a `dd`,
  `new_fs` or `diff` on `/dev/zvol/...`, not in the compressor:
  `recv_dedup_encrypted_zvol`, `send-wR_encrypted_zvol`, `send-c_volume` and
  `send-c_stream_size_estimate` (the last is also an expected `zts-report`
  FAIL, "Kernel splice(2) does not lock the file position"). They fail the same
  way on M, so they are environmental, not the branch. A failure in the group
  can also cascade to a later test whose `$TESTDIR` the failed test left in a
  bad state; the single-test rerun on M (STEP 5) separates a cascade from a
  real result. The `send-c_*` and `send_embedded_*` tests, which are the ones
  the change touches, are expected PASS on both trees.

A test that fails on B and **passes** when rerun alone on M is the only shape
that could point at the branch; STEP 5 says how to read it. A test that fails
on B and also fails when rerun on M is not the branch's.

## Why the cross-version reads, and what to expect

On disk nothing changes: any LZ4 decoder reads what any LZ4 compressor writes,
so a pool written by either tree imports and its data reads back bit-for-bit on
the other tree. The compressed *bytes* a block is stored as do change for some
sizes, and exactly one read path re-derives those bytes and compares them:

- With **compressed ARC on** (the default, `zfs_compressed_arc_enabled=1`), a
  block is kept in the ARC as its stored compressed bytes, and the MAC of an
  encrypted dataset's authenticated-only metadata is checked against those
  stored bytes. Cross-version reads are fine.
- With **compressed ARC off** (`=0`), the block is kept decompressed, and to
  check the MAC `arc_hdr_authenticate()` compresses it again, with the
  *reader's* compressor, and compares the result's MAC to the stored one
  (`module/zfs/arc.c`, `arc_hdr_authenticate()` →
  `spa_do_crypt_mac_abd()` → `ECKSUM` on mismatch → `EIO` at the ARC edge).
  If the reader's bytes differ from the writer's, the read fails with `EIO` and
  a permanent error in `zpool status -v`. This is forgiven only when the key is
  missing (`EACCES`), not on a byte mismatch.

This affects an encrypted dataset's **authenticated-but-not-encrypted** objects
(`ot_encrypt == FALSE` in `dmu.c`'s `dmu_ot[]`): for a ZPL filesystem that is
essentially the **master node** (object 1, a ZAP), and it is stored compressed
only on small-ashift pools. Encrypted *data* is encrypted, not re-compressed to
check a MAC, so it reads back bit-exact in every case; indirect blocks and the
objset MAC are computed differently and are unaffected.

**The size caveat, stated plainly.** The compatibility study measured that
1.10's output equals r85's for about 100 % of blocks of 4 KiB and below, and
diverges only from 16 KiB up. The ZPL master node is a single small block
(512 B at `ashift=9`, 4 KiB at `ashift=12`), so its r85 and 1.10 bytes are
almost always **identical**, and the cross-version read is therefore **expected
to succeed even with compressed ARC off**. That is why the study could force
this `EIO` only through libzpool, by writing large (16 KiB/128 KiB)
authenticated metadata objects that the kernel CLI cannot create inside an
encrypted dataset. So this part has two legitimate outcomes, and it records
which one happened:

- **Clean reads in all cases** (the expected result): confirms the exposure is
  not reachable through a ZPL dataset at these block sizes, i.e. that the
  on-ZPL master node's bytes did not change. This matches the study's finding
  that the failure is reproducible only in libzpool.
- **`EIO` on the authenticated metadata with compressed ARC off, cross-version
  only** (the study's predicted failure, if the master node happens to be one
  of the changed blocks): record exactly which read fails, its error,
  `zpool status -v` and the `zfs_dbgmsg`.

Either way a read error here is a recorded finding, **not** a reason to stop.
The one thing that is a stop is a kernel `VERIFY`/`ASSERT`, panic, oops or hung
task: these are debug builds, and an assertion in the crypto or ARC path means
stop and report, in whatever state it happened.

The pools are created by hand on file vdevs under `$W`, with
`-o cachefile=none` so `/etc/zfs/zpool.cache` is never written, and are
destroyed at the end. `zdb -bcc` is run from each tree's `zdb` on each pool: it
verifies on-disk checksums (over the stored bytes, which every version agrees
on) and that nothing leaked, and is expected to report no errors and no leaks
from both `zdb`s.

## Rules for the whole run

- Run everything as root. Shell variables may not survive between your
  commands, so every block below starts by setting them or by sourcing
  `/var/log/zts-lz4-1026/env.sh`.
- A production hypervisor sits underneath this VM. Module loads and every ZTS
  run go through `nice -n 10 ionice -c3` (the blocks and the scripts do it),
  one run at a time, with nothing else running beside them.
- Post nothing, push nothing, install nothing, never reboot, and never run
  `scripts/zfs-helpers.sh` or `zfs-tests.sh -x`. Rebuild nothing: the trees are
  already built.
- Work only under `/var/tmp/zts-lz4-1026` (W) and `/var/log/zts-lz4-1026` (L).
  Never read from, write to or delete `/var/tmp/test_results`,
  `/var/tmp/zts-19314`, another `/var/tmp/zts-*` or `/var/log/zts-*` directory,
  or anything else this task did not create. ZTS test pools mount under
  `/testpool*` while they exist; the tests' cleanup removes those directories.
  The hand-built cross-version pools live only on file vdevs under `$W` and
  mount under `$W/cvmnt-*`.
- Never edit, delete or rewrite `/etc/zfs/zpool.cache` yourself. The kernel
  records ZTS pools in it while they exist and truncates it to 0 bytes when
  they are gone; the cross-version pools use `-o cachefile=none` and never
  touch it. STEP 0 and STEP 8 record it.
- Never run a `zpool` or `zfs` command while none of this task's modules is
  loaded. Never print the environment (`env`, `printenv`, `set`).
- The only `rm -rf` allowed is the one in STEP 8. The only other removals are
  the per-pool vdev directories the cross-version script made itself, under
  `$W/cv`.
- If a check does not print what its comment says, stop at once and report. Do
  not rebuild, repair or retry. If your permission system refuses a command, do
  not work around it: report it and stop there. A kernel panic, oops, `VERIFY`,
  `ASSERT` or hung-task message means stop and report; never reboot.
- A ZTS group run takes up to about 20 minutes. If your shell tool cannot wait
  that long for a command in the foreground, start that one command with the
  tool's own background mode and wait until it exits; do not use `&` or
  `nohup`, and run nothing else meanwhile.

## STEP 0: the build half must be finished, and the VM free

First check that the build half has finished:

```sh
date -u +%T
for p in $(pgrep -f '[.]/autogen[.]sh|make -j[8]'); do
    echo "$p cwd=$(readlink /proc/$p/cwd) $(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-100)"
done
echo "build check done"
```

Expected: only the time and `build check done`. A listed process whose `cwd` is
under `/var/tmp/zts-lz4-1026`, or whose command line is the build half's
(`./autogen.sh && ./configure --enable-debug && make -j8`), means the build is
still running: wait about 5 minutes and run the block again, at most 12 times.
Stop and report if it still lists something after an hour, or if it lists
anything else.

Then:

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-lz4-1026 L=/var/log/zts-lz4-1026
id -un                        # root
uname -r                      # 6.12.113+deb13-cloud-amd64 (the kernel of the earlier runs on this VM)
nproc                         # 16
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '  # nothing
ls /dev/zfs 2>&1; ls /sys/module | grep -xE 'zfs|spl'   # No such file or directory, then nothing
command -v zpool zfs zed      # nothing: no system ZFS
pgrep -x zed                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|raidz_test'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
ls -A $W                      # B M repo
ls -A $L                      # build-B.log build-M.log, and maybe other logs of the build half
stat -c '%U %n' $W $W/repo $W/B $W/M $L   # root, five times
readlink -f $W                # /var/tmp/zts-lz4-1026
losetup -a; dmsetup ls 2>&1   # nothing, then: No devices found
grep -E ' zfs |testpool|cvpool|zts-lz4' /proc/mounts   # nothing
ls -d /testpool* /cvpool* 2>&1   # No such file or directory
ls -l /etc/zfs/zpool.cache 2>&1   # No such file or directory, or a 0-byte file
ls -ld /var/tmp/test_results /var/tmp/zts-19314 2>&1
ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules 2>&1; ls -d /dev/zvol 2>&1
df -h /var/tmp                # need about 15G free
id zts; sudo -n -l -U zts | grep -i nopasswd
journalctl -k -b --no-pager -q | grep -iE 'VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than' | grep -vcE 'spa_import_progress_set_notes_impl|ZTS run '
```

Keep this output, which goes into REPORT.txt. The last number is the
kernel-message baseline. The `60-zvol.rules` and `/dev/zvol` lines are only
recorded (they explain the four zvol FAILs in the `rsend` group).

Stop at once, changing nothing, and report what you found if any of these
holds:

- you are not root, or the kernel headers are missing;
- a zfs or spl module is loaded, `/dev/zfs` exists, or `zed` runs;
- a test or build process is running. The `pgrep -af` also matches the shell
  that runs this block, because that shell's command line contains the pattern:
  ignore that one line;
- a tmux pane other than the one hosting this session runs anything but an idle
  shell;
- `$W` or `$L` is missing or not root-owned; `$W` holds anything other than
  `B M repo`; `$L` lacks `build-B.log` or `build-M.log`, or already holds any of
  `env.sh`, `run.sh`, `cv.sh`, `guards.txt`, `results`, `step1.txt` or
  `REPORT.txt` (this task would then have started once already);
- `readlink -f $W` prints anything else;
- a loop device, a device-mapper device, a zfs or test mount, or a
  `/testpool*` or `/cvpool*` directory exists;
- `/etc/zfs/zpool.cache` exists and is not empty;
- less than 15 GB is free on `/var/tmp`;
- user `zts` is missing or has no passwordless sudo.

A kernel other than `6.12.113+deb13-cloud-amd64` is not a stop by itself,
because STEP 1 checks that the modules were built for the running one.

## STEP 1: verify the preparation (all git work happens here, before any chown)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-lz4-1026 L=/var/log/zts-lz4-1026
SHA_B=61d3f825aaf668d348c7905c17f8f2ff4b09f8b4
SHA_M=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
GLUE=module/lz4/zfs_lz4.c
OLDC=module/zfs/lz4.c
OLDZ=module/zfs/lz4_zfs.c
{
echo "B HEAD:   $(git -C $W/B rev-parse HEAD)"     # 61d3f825aaf668d348c7905c17f8f2ff4b09f8b4
echo "B HEAD~3: $(git -C $W/B rev-parse HEAD~3)"   # a128b8a075b5b13c737e56914cd7043bd3e9ebf8
echo "M HEAD:   $(git -C $W/M rev-parse HEAD)"     # a128b8a075b5b13c737e56914cd7043bd3e9ebf8
echo "B tracked changes: $(git -C $W/B status --short --untracked-files=no | wc -l)"   # 0
echo "M tracked changes: $(git -C $W/M status --short --untracked-files=no | wc -l)"   # 0
echo "B vs master: $(git -C $W/B diff --stat $SHA_M HEAD | tail -n 1)"
echo "B commits on master:"
git -C $W/B log --oneline $SHA_M..HEAD          # 61d3f825aa, ad8fa09d6b, 61d511b304
echo "B glue present: $(test -f $W/B/$GLUE && echo yes), old compressor: $(test -e $W/B/$OLDZ && echo present || echo gone), old decoder: $(test -e $W/B/$OLDC && echo present || echo gone)"
# B: glue yes, old compressor gone, old decoder gone
echo "M glue present: $(test -f $W/M/$GLUE && echo yes || echo no), old compressor: $(test -e $W/M/$OLDZ && echo present || echo gone), old decoder: $(test -e $W/M/$OLDC && echo present || echo gone)"
# M: glue no, old compressor present, old decoder present
for t in B M; do
    echo "build-$t.log errors: $(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)"   # 0
    echo "build-$t.log warnings: $(grep -cE ': warning:' $L/build-$t.log)"         # 0
    tail -n 2 $L/build-$t.log
    echo "$t debug build: $(grep -c '^#define ZFS_DEBUG 1' $W/$t/zfs_config.h)"     # 3
    ls -l $W/$t/module/spl.ko $W/$t/module/zfs.ko $W/$t/zpool $W/$t/zfs \
        $W/$t/zstream $W/$t/zdb \
        $W/$t/scripts/zfs.sh $W/$t/scripts/zfs-tests.sh \
        $W/$t/tests/test-runner/bin/test-runner.py \
        $W/$t/tests/zfs-tests/tests/functional/compression/setup.ksh \
        $W/$t/tests/zfs-tests/tests/functional/rsend/setup.ksh \
        $W/$t/tests/zfs-tests/tests/functional/cli_root/zfs_receive/setup.ksh
    echo "$t srcversion $(modinfo -F srcversion $W/$t/module/zfs.ko)"
    echo "$t vermagic $(modinfo -F vermagic $W/$t/module/zfs.ko | cut -d' ' -f1) running $(uname -r)"
done
} 2>&1 | tee $L/step1.txt
```

Expected values:

- the full SHAs and the counts in the comments above; `B vs master` names the
  LZ4 files moved/removed/added; the three B commits are `61d3f825aa`,
  `ad8fa09d6b`, `61d511b304`;
- B has the glue and no longer has the two old files; M has the two old files
  and no glue;
- for each build log, 0 errors **and 0 compiler warnings**, and a last line of a
  finished build (typically
  `make[1]: Leaving directory '/var/tmp/zts-lz4-1026/<B or M>'`);
- `<t> debug build: 3` for both — `configure --enable-debug` writes three
  `#define ZFS_DEBUG 1` lines in `zfs_config.h` (one unconditional, two under
  `#ifdef`); any value of 3 is a debug build. **Not 1.**
- M srcversion `8852710DB871A7587B9E722` (master's value on this VM) and a B
  srcversion that differs, because the LZ4 object changes the module. B's value
  is read in STEP 2; the build half reported `B35972A266DE31839D6B727`, which
  STEP 2 checks against the tree;
- each vermagic equal to the running kernel;
- every `ls -l` path present.

If anything else prints, stop and report. Do not rebuild.

## STEP 2: harness setup

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-lz4-1026 L=/var/log/zts-lz4-1026
for t in B M; do
    sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
        $W/$t/tests/test-runner/bin/test-runner.py.in \
        $W/$t/tests/test-runner/bin/test-runner.py &&
    grep -n '^BASEDIR' $W/$t/tests/test-runner/bin/test-runner.py
done                          # each: 43:BASEDIR = '/var/log/zts-lz4-1026/results', else stop
mkdir $W/files $W/cv $L/results && chmod 1777 $W/files; echo "mkdir rc=$?"   # 0
head -c 32 /dev/urandom > $L/cvkey && chmod 600 $L/cvkey && echo "key bytes $(wc -c < $L/cvkey)"   # 32
chown -R zts $W $L/results; echo "chown rc=$?"   # 0
stat -c '%U %a %n' $W/files $W/cv $L/results   # zts 1777 .../files, zts 755 .../cv, zts 755 .../results
```

The ZTS `-d` directory must be mode 1777: the tests write there both as root
and as `zts`. The key file stays root-owned under `$L`.

Then create the environment file, owned by root:

```sh
cat > /var/log/zts-lz4-1026/env.sh <<'EOF'
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-lz4-1026
L=/var/log/zts-lz4-1026
KEY=$L/cvkey
# Kernel findings. KIGN drops the ZTS -K markers (test names such as
# send-c_verify_ratio match VERIFY) and the import-progress notes. The
# cross-version EIO path logs an "authentication" error, which is a finding
# of this task, not a kernel fault, so it is NOT in KPAT.
KPAT='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
KIGN='spa_import_progress_set_notes_impl|ZTS run '
# The tunables the rsend/compression tests change and must restore.
TUN='zfs_compressed_arc_enabled zfs_disable_ivset_guid_check zfs_recv_defer_batch_size send_holes_without_birth_time zap_micro_max_size l2arc_noprefetch l2arc_write_max'
tunables() {
	for p in $TUN; do
		printf '%s=%s ' "$p" "$(cat /sys/module/zfs/parameters/$p 2>&1)"
	done
}
EOF
. /var/log/zts-lz4-1026/env.sh
svb=$(modinfo -F srcversion $W/B/module/zfs.ko)
svm=$(modinfo -F srcversion $W/M/module/zfs.ko)
echo "srcversions read from the trees: B $svb, M $svm"
if [ -n "$svb" ] && [ -n "$svm" ] && [ "$svb" != "$svm" ] &&
    [ "$svm" = 8852710DB871A7587B9E722 ]; then
    printf 'SV_B=%s\nSV_M=%s\n' "$svb" "$svm" >> $L/env.sh && echo "srcversions recorded"
else
    echo "SRCVERSION CHECK FAILED: stop"
fi
tail -n 2 $L/env.sh
```

Expected: `srcversions read from the trees: B B35972A266DE31839D6B727, M
8852710DB871A7587B9E722`, `srcversions recorded`, then the two lines `SV_B=...`
and `SV_M=8852710DB871A7587B9E722`. On `SRCVERSION CHECK FAILED`, stop: the two
modules are the same, or M is not master's, or B's module did not build.

Then create the per-run ZTS script, also in `$L` and owned by root:

```sh
cat > /var/log/zts-lz4-1026/run.sh <<'EOS'
#!/bin/sh
# usage:
#   run.sh <B|M> <label> group <tag>
#   run.sh <B|M> <label> test  <group_subdir> <test name>
# One run of a whole ZTS group (-T tag) or of one test (-t), on the loaded
# tree, then its kernel messages and leftovers.
. /var/log/zts-lz4-1026/env.sh
t=$1; lbl=$2; mode=$3
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "usage: run.sh <B|M> <label> group <tag> | test <subdir> <name>"; exit 2 ;;
esac
case $lbl in
''|*[!a-z0-9]*) echo "bad label: '$lbl'"; exit 2 ;;
esac
if [ -e "$L/$lbl.log" ]; then
	echo "label already used: $lbl"; exit 2
fi
case $mode in
group)
	tag=$4
	case $tag in
	''|*[!A-Za-z0-9_-]*) echo "bad tag: '$tag'"; exit 2 ;;
	esac
	sel="-T $tag"; outer=2400; what=GROUP ;;
test)
	sub=$4; what=$5
	case $sub in
	''|*[!A-Za-z0-9_/-]*) echo "bad subdir: '$sub'"; exit 2 ;;
	esac
	case $what in
	''|*[!A-Za-z0-9_-]*) echo "bad test name: '$what'"; exit 2 ;;
	esac
	if [ ! -x "$W/$t/tests/zfs-tests/tests/functional/$sub/$what.ksh" ]; then
		echo "no such test in $t: $sub/$what.ksh"; exit 2
	fi
	sel="-t tests/functional/$sub/$what.ksh"; outer=1800 ;;
*) echo "mode is 'group' or 'test'"; exit 2 ;;
esac
if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
	echo "SV_B and SV_M are missing or equal: nothing run"; exit 1
fi

G=$L/guards.txt

# Guards before the run: the loaded module must be this tree's, the tunables
# as found after the load, no pool, an empty work directory, no loop device,
# no test mount and no test mount point.
cur=$(cat /sys/module/zfs/srcversion 2>&1)
if [ "$cur" != "$sv" ]; then
	echo "$(date -u +%T) GUARD FAILED before $lbl: srcversion=$cur want=$sv:" \
	    "nothing run" | tee -a "$G"
	exit 1
fi
tun=$(tunables)
pools=$("$W/$t/zpool" list -H -o name 2>&1)
files=$(ls -A "$W/files" 2>&1)
loops=$(losetup -a 2>&1)
mnts=$(grep -E ' zfs |testpool|cvpool' /proc/mounts)
roots=$(ls -d /testpool /testpool1 /testpool2 /testpool3 /cvpool 2>/dev/null)
echo "$(date -u +%T) before $lbl ($t $mode ${tag:-$sub/$what}): srcversion=$cur" \
    "pools=[$pools] files=[$files] loops=[$loops] mounts=[$mnts]" \
    "rootdirs=[$roots]" | tee -a "$G"
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
    runuser -u zts -- ./scripts/zfs-tests.sh -vKO -s 3GB -d "$W/files" \
    $sel > "$L/$lbl.log" 2>&1
rc=$?
wall=$(( $(date +%s) - start ))
# Two runs started in the same second would share a results directory.
sleep 2

# Results. Each "Test:" / "Test (Linux):" line starts with a timestamp, which
# is dropped. A -t run names the test with ".ksh", a -T run without it.
rd=$(awk '/^Log directory/ { print $3 }' "$L/$lbl.log")
miss=$(grep -E '^Missing util' "$L/$lbl.log")
grep -E 'Test( [(][A-Za-z]+[)])?: .* [(]run as [a-z]+[)] +\[[0-9:]+\] \[[A-Z]+\]$' \
    "$L/$lbl.log" | sed 's/^\[[^]]*\] //' > "$L/$lbl-tests.txt"
cnt=$(grep -oE '\[[A-Z]+\]$' "$L/$lbl-tests.txt" | tr -d '[]' | sort |
    uniq -c | awk '{ printf "%s%s %d", s, $2, $1; s = ", " }')
grep -vE '\[PASS\]$' "$L/$lbl-tests.txt" > "$L/$lbl-notpassed.txt"
grep -E '\[(FAIL|KILLED)\]$' "$L/$lbl-notpassed.txt" |
    sed -E 's|^.*/functional/([^ ]+) [(]run as .*$|\1|; s|[.]ksh$||' \
    > "$L/$lbl-failed.txt"
{
echo "$lbl: tree $t, $sel, $(wc -l < "$L/$lbl-tests.txt") results:" \
    "${cnt:-none}; zfs-tests.sh rc=$rc, wall ${wall}s, log dir ${rd:-none}"
echo "${miss:-no Missing util(s) line}"
sed -n '/^Results Summary/,/^Percent passed/p' "$L/$lbl.log"
echo "not passed ($(wc -l < "$L/$lbl-notpassed.txt")):"
sed 's/^/  /' "$L/$lbl-notpassed.txt"
sed -n '/^Tests with /,$p' "$L/$lbl.log"
for n in $(cat "$L/$lbl-failed.txt"); do
	base=${n##*/}
	o=""
	if [ -n "$rd" ] && [ -d "$rd" ]; then
		o=$(find "$rd" -type f -name merged -path "*/$base/*" 2>/dev/null | head -n 1)
		[ -z "$o" ] && o=$(find "$rd" -type f -name stdout -path "*/$base/*" 2>/dev/null | head -n 1)
	fi
	if [ -n "$o" ]; then
		cp "$o" "$L/$lbl-$base-out.txt"
		echo "$n:"
		grep -E 'ERROR: ' "$o" | head -n 10 | sed 's/^/  /'
		awk '/NOTE: Performing (test-fail callback|local cleanup)/ {
			print "  end line: " prev; exit }
		    NF { prev = $0 }' "$o"
	else
		echo "$n: no stdout or merged file found"
	fi
done
} > "$L/$lbl-notes.txt"

# Kernel messages of this run. "ZTS run" is the -K marker: it proves the
# journal saw the run.
journalctl -k --since "@$start" --no-pager -q > "$L/$lbl-kernel.txt" 2>&1
kn=$(grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vcE "$KIGN")
km=$(grep -c 'ZTS run' "$L/$lbl-kernel.txt")

# Leftovers: all must be empty, and the tunables as after the load.
lp=$("$W/$t/zpool" list -H -o name 2>&1)
ll=$(losetup -a 2>&1)
ld=""
command -v dmsetup > /dev/null && ld=$(dmsetup ls 2>&1 | grep -v 'No devices')
lf=$(ls -A "$W/files" 2>&1)
lm=$(grep -E ' zfs |testpool|cvpool' /proc/mounts)
lr=$(ls -d /testpool /testpool1 /testpool2 /testpool3 /cvpool 2>/dev/null)
tn=$(tunables)
ts=no
[ "$tn" = "$(cat "$L/tun-$t.txt")" ] && ts=yes
# Leftover processes, leaving out this script and the shells that started it.
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
    "zts_marks=$km kernel_matches=$kn" | tee -a "$G"
echo "$(date -u +%T) after $lbl leftovers: pools=[$lp] loops=[$ll] dm=[$ld]" \
    "files=[$lf] mounts=[$lm] rootdirs=[$lr] procs=[$lq] tunables_same=$ts" |
    tee -a "$G"
cat "$L/$lbl-notes.txt"
if [ "$kn" -gt 0 ]; then
	grep -iE "$KPAT" "$L/$lbl-kernel.txt" | grep -vE "$KIGN"
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
bash -n /var/log/zts-lz4-1026/run.sh; echo "bash syntax rc=$?"   # 0
sh -n /var/log/zts-lz4-1026/run.sh; echo "sh syntax rc=$?"       # 0
```

`run.sh` exits 0 when the run is clean, whatever the test results. It exits 1
when a guard before the run failed (nothing ran), 2 on a bad argument, and 3
when something is wrong after the run (leftover, changed tunable, kernel match,
missing ZTS mark, outer timeout). On 1, 2 or 3, stop and report; clean nothing
by hand.

Then create the cross-version script, also in `$L` and owned by root:

```sh
cat > /var/log/zts-lz4-1026/cv.sh <<'EOS'
#!/bin/sh
# usage:
#   cv.sh guard <B|M>                     : the loaded module must be this tree's
#   cv.sh write <B|M> <pool> <ashift>     : create+populate, then export
#   cv.sh read  <B|M> <pool> <writer> <0|1> : import, load key, read, export
#   cv.sh zdb   <B|M> <pool>              : zdb -bcc from this tree's zdb
#   cv.sh clean <B|M> <pool>              : destroy the pool and its vdev dir
# All pools are file-vdev pools under $W/cv, created with -o cachefile=none.
. /var/log/zts-lz4-1026/env.sh
cmd=$1; t=$2
case $t in
B) sv=$SV_B ;;
M) sv=$SV_M ;;
*) echo "cv.sh: tree is B or M"; exit 2 ;;
esac
ZP="$W/$t/zpool"; ZFS="$W/$t/zfs"; ZDB="$W/$t/zdb"
zp() { nice -n 10 ionice -c3 "$ZP" "$@"; }
zf() { nice -n 10 ionice -c3 "$ZFS" "$@"; }
guard() {
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	if [ "$cur" != "$sv" ]; then
		echo "GUARD FAILED: loaded srcversion '$cur' is not $t's ($sv)"; return 1
	fi
	if [ -z "$SV_B" ] || [ -z "$SV_M" ] || [ "$SV_B" = "$SV_M" ]; then
		echo "GUARD FAILED: SV_B/SV_M missing or equal"; return 1
	fi
	return 0
}
poolok() {   # a valid, task-made pool name
	case $1 in
	cv[bm]9|cv[bm]12) return 0 ;;
	*) echo "bad pool name: '$1'"; return 1 ;;
	esac
}

case $cmd in
guard)
	guard && echo "guard $t ok: $(cat /sys/module/zfs/srcversion)"
	;;
write)
	pool=$3; ashift=$4
	guard || exit 1
	poolok "$pool" || exit 2
	case $ashift in 9|12) ;; *) echo "ashift is 9 or 12"; exit 2 ;; esac
	dir="$W/cv/$pool"
	if [ -e "$dir" ]; then echo "dir exists: $dir"; exit 2; fi
	if [ -n "$(zp list -H -o name 2>&1 | grep -x "$pool")" ]; then
		echo "pool $pool already exists"; exit 2
	fi
	mkdir "$dir" || exit 1
	truncate -s 3g "$dir/vdev0" || exit 1
	echo "=== cv write $t $pool ashift=$ashift"
	zp create -f -o cachefile=none -o ashift="$ashift" \
	    -O compression=off -O mountpoint=none "$pool" "$dir/vdev0" || exit 1
	# authenticated-only metadata lives in the master node of an encrypted
	# ZPL dataset; normalization/casesensitivity/utf8only enlarge it.
	zf create -o compression=lz4 -o encryption=on -o keyformat=raw \
	    -o keylocation="file://$KEY" -o normalization=formD \
	    -o casesensitivity=mixed -o utf8only=on \
	    -o mountpoint="$W/cvmnt-$pool-enc" "$pool/enc" || exit 1
	zf create -o compression=lz4 -o mountpoint="$W/cvmnt-$pool-plain" \
	    "$pool/plain" || exit 1
	zf mount "$pool/enc" || exit 1
	zf mount "$pool/plain" || exit 1
	# compressible mixed data: the tree's own C sources, repeated to ~48 MiB.
	src="$W/$t/module/zfs"
	for d in enc plain; do
		: > "$W/cvmnt-$pool-$d/data"
		i=0
		while [ "$(wc -c < "$W/cvmnt-$pool-$d/data")" -lt 50331648 ]; do
			cat "$src"/*.c >> "$W/cvmnt-$pool-$d/data" 2>/dev/null
			i=$((i + 1)); [ "$i" -gt 40 ] && break
		done
		sync
	done
	sha_enc=$(sha256sum "$W/cvmnt-$pool-enc/data" | cut -d' ' -f1)
	sha_plain=$(sha256sum "$W/cvmnt-$pool-plain/data" | cut -d' ' -f1)
	printf '%s\n%s\n' "$sha_enc" "$sha_plain" > "$L/$pool.sha"
	zp sync "$pool"
	echo "master node (enc, object 1) compression, written by $t:"
	"$ZDB" -dddddd "$pool/enc" 1 2>/dev/null |
	    grep -m1 -oE '[0-9a-f]+L/[0-9a-f]+P' |
	    sed 's/^/  L\/P sizes: /' || true
	zf unmount "$pool/enc"; zf unmount "$pool/plain"
	zp export "$pool"
	echo "cv write $t $pool: exported, sha recorded"
	;;
read)
	pool=$3; writer=$4; carc=$5
	guard || exit 1
	poolok "$pool" || exit 2
	case $carc in 0|1) ;; *) echo "carc is 0 or 1"; exit 2 ;; esac
	dir="$W/cv/$pool"
	start=$(date +%s)
	def=$(cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled)
	echo "=== cv read: writer=$writer reader=$t pool=$pool compressed_arc=$carc (default was $def)"
	echo "$carc" > /sys/module/zfs/parameters/zfs_compressed_arc_enabled
	rc=0
	if ! zp import -N -o cachefile=none -d "$dir" "$pool"; then
		echo "IMPORT FAILED"; echo 1 > /sys/module/zfs/parameters/zfs_compressed_arc_enabled; exit 1
	fi
	zf load-key "$pool/enc" || { echo "LOAD-KEY FAILED"; rc=1; }
	# Reading the authenticated master node (zfs get of ZPL props) and the
	# encrypted + plain data. With carc=0 and a cross-version pool, the
	# master-node read is the one that can EIO; data reads should not.
	got_enc=err; got_plain=err
	props=err
	if zf mount "$pool/enc" 2>/tmp/cv-mnt-enc.$$; then
		props=$(zf get -H -o value normalization,utf8only,casesensitivity "$pool/enc" 2>&1 | tr '\n' ',')
		got_enc=$(sha256sum "$W/cvmnt-$pool-enc/data" 2>/dev/null | cut -d' ' -f1)
		[ -z "$got_enc" ] && got_enc=readerr
		zf unmount "$pool/enc" 2>/dev/null
	else
		props="mount failed: $(cat /tmp/cv-mnt-enc.$$)"
	fi
	rm -f /tmp/cv-mnt-enc.$$
	if zf mount "$pool/plain" 2>/dev/null; then
		got_plain=$(sha256sum "$W/cvmnt-$pool-plain/data" 2>/dev/null | cut -d' ' -f1)
		[ -z "$got_plain" ] && got_plain=readerr
		zf unmount "$pool/plain" 2>/dev/null
	fi
	want_enc=$(sed -n 1p "$L/$pool.sha"); want_plain=$(sed -n 2p "$L/$pool.sha")
	echo "  enc data:   $([ "$got_enc" = "$want_enc" ] && echo bit-exact || echo "DIFFERS/ERR ($got_enc)")"
	echo "  plain data: $([ "$got_plain" = "$want_plain" ] && echo bit-exact || echo "DIFFERS/ERR ($got_plain)")"
	echo "  enc ZPL props (master node read): $props"
	echo "  zpool status -v:"
	zp status -v "$pool" 2>&1 | sed 's/^/    /'
	echo "  zfs_dbgmsg tail:"
	tail -n 15 /proc/spl/kstat/zfs/dbgmsg 2>/dev/null | sed 's/^/    /'
	zp export "$pool"
	echo 1 > /sys/module/zfs/parameters/zfs_compressed_arc_enabled
	echo "  tunable restored: zfs_compressed_arc_enabled=$(cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled)"
	# Kernel-fault check for this read (an EIO is not a fault; a VERIFY is).
	journalctl -k --since "@$start" --no-pager -q > "$L/cvread-$writer-$t-$pool-$carc-kernel.txt" 2>&1
	kn=$(grep -iE "$KPAT" "$L/cvread-$writer-$t-$pool-$carc-kernel.txt" | grep -vcE "$KIGN")
	echo "  kernel_faults=$kn"
	if [ "$kn" -gt 0 ]; then
		echo "  KERNEL FAULT (stop):"
		grep -iE "$KPAT" "$L/cvread-$writer-$t-$pool-$carc-kernel.txt" | grep -vE "$KIGN" | sed 's/^/    /'
	fi
	;;
zdb)
	pool=$3
	guard || exit 1
	poolok "$pool" || exit 2
	dir="$W/cv/$pool"
	echo "=== cv zdb -bcc: $t's zdb on $pool"
	"$ZDB" -e -p "$dir" -bcc "$pool" > "$L/zdb-$t-$pool.txt" 2>&1
	zrc=$?
	echo "  zdb rc=$zrc"
	echo "  leaks/errors:"
	grep -iE 'leaked|leak|error' "$L/zdb-$t-$pool.txt" | grep -vi 'No known data errors' | sed 's/^/    /'
	grep -E 'No leaks|block sum matches|bp count:' "$L/zdb-$t-$pool.txt" | sed 's/^/    /'
	;;
clean)
	pool=$3
	guard || exit 1
	poolok "$pool" || exit 2
	dir="$W/cv/$pool"
	if zp list -H -o name "$pool" > /dev/null 2>&1; then
		zp export "$pool" 2>/dev/null
	fi
	case $dir in
	"$W"/cv/*) rm -rf "$dir"; echo "removed $dir rc=$?" ;;
	*) echo "refusing to remove '$dir'" ;;
	esac
	;;
*)
	echo "cv.sh: unknown command '$cmd'"; exit 2 ;;
esac
EOS
bash -n /var/log/zts-lz4-1026/cv.sh; echo "bash syntax rc=$?"   # 0
sh -n /var/log/zts-lz4-1026/cv.sh; echo "sh syntax rc=$?"       # 0
```

`cv.sh` guards the loaded srcversion on every call. `write`/`read`/`zdb`/`clean`
refuse any pool name other than `cvb9`, `cvb12`, `cvm9`, `cvm12`, and remove
only directories under `$W/cv`. `read` sets `zfs_compressed_arc_enabled` for the
one read and always restores it to 1. An import or key-load failure is reported
and is not a stop; a kernel `VERIFY`/panic during a read is a stop.

## STEP 3: ZTS on B

```sh
. /var/log/zts-lz4-1026/env.sh
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"           # the same value twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_B" ] && { $W/B/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-B.txt; cat $L/tun-B.txt; echo
# zfs_compressed_arc_enabled=1 ... (the module defaults; recorded as the B baseline)
```

Then run the three groups, one at a time. Go on to the next only if the
previous one printed `run.sh rc=0`. Each may take up to ~20 minutes; see the
last rule about long commands.

```sh
sh /var/log/zts-lz4-1026/run.sh B bcomp  group compression        > $L/bcomp-console.txt  2>&1; echo "run.sh rc=$?" >> $L/bcomp-console.txt;  cat $L/bcomp-console.txt
sh /var/log/zts-lz4-1026/run.sh B brsend group rsend              > $L/brsend-console.txt 2>&1; echo "run.sh rc=$?" >> $L/brsend-console.txt; cat $L/brsend-console.txt
sh /var/log/zts-lz4-1026/run.sh B brecv  group zfs_receive        > $L/brecv-console.txt  2>&1; echo "run.sh rc=$?" >> $L/brecv-console.txt;  cat $L/brecv-console.txt
```

Expected on B, per the "Why these groups" section:

- `bcomp`: Results Summary `PASS 8` (plus `setup`/`cleanup` PASS), no FAIL.
- `brecv`: `PASS 31` (plus `setup`/`cleanup`); any non-PASS is a `zts-report`
  expected SKIP/UNSUPPORTED, listed under "Tests with results other than PASS
  that are expected".
- `brsend`: 88 results. `rsend_008_pos` SKIP. The four zvol tests
  (`recv_dedup_encrypted_zvol`, `send-wR_encrypted_zvol`, `send-c_volume`,
  `send-c_stream_size_estimate`) FAIL on this VM (no `60-zvol.rules`), and may
  cascade to one or two later tests whose `$TESTDIR` the failed test left in a
  bad state. The `send-c_*` and `send_embedded_*` tests PASS. Every FAIL is
  recorded in `brsend-failed.txt` for the rerun in STEP 5.

`run.sh rc=0` for each. Any `run.sh` exit of 1, 2 or 3 is a stop.

## STEP 4: ZTS on M

Unload B only while `/sys/module/zfs/srcversion` is still B's, then load M:

```sh
. /var/log/zts-lz4-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_B" ] && cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
```

If `unload rc` is not 0 or `lsmod` prints anything, stop. Otherwise:

```sh
. /var/log/zts-lz4-1026/env.sh
cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_M $SV_M"           # 8852710DB871A7587B9E722 twice
[ "$(cat /sys/module/zfs/srcversion 2>&1)" = "$SV_M" ] && { $W/M/zpool list -H -o name; echo "zpool list rc=$?"; }   # no pool name, then rc=0
tunables > $L/tun-M.txt; cat $L/tun-M.txt; echo
cmp $L/tun-B.txt $L/tun-M.txt && echo "same tunables as B"          # same tunables as B
```

```sh
sh /var/log/zts-lz4-1026/run.sh M mcomp  group compression        > $L/mcomp-console.txt  2>&1; echo "run.sh rc=$?" >> $L/mcomp-console.txt;  cat $L/mcomp-console.txt
sh /var/log/zts-lz4-1026/run.sh M mrsend group rsend              > $L/mrsend-console.txt 2>&1; echo "run.sh rc=$?" >> $L/mrsend-console.txt; cat $L/mrsend-console.txt
sh /var/log/zts-lz4-1026/run.sh M mrecv  group zfs_receive        > $L/mrecv-console.txt  2>&1; echo "run.sh rc=$?" >> $L/mrecv-console.txt;  cat $L/mrecv-console.txt
```

Expected on M: the same picture as B (same within-version compressor), including
the same four zvol FAILs and the `rsend_008_pos` SKIP. The `compression` and
`zfs_receive` groups: all PASS.

## STEP 5: rerun each B-only failure once, alone, on M

Still with M loaded, rerun each test that FAILed or was KILLED on B but is
**not** one of the four known zvol tests, running it alone (`-t`), which gives it
its own `setup` and so removes the group-ordering effect. This tells a failure
the branch introduced (passes alone on M) from one master has too or the group
left behind (fails alone on M too, or was only a cascade). At most 8.

```sh
. /var/log/zts-lz4-1026/env.sh
{
ZVOL='recv_dedup_encrypted_zvol|send-wR_encrypted_zvol|send-c_volume|send-c_stream_size_estimate'
cat $L/brsend-failed.txt $L/bcomp-failed.txt $L/brecv-failed.txt 2>/dev/null \
    | grep -vE "setup|cleanup|($ZVOL)\$" | sort -u > $L/rerun-list.txt
echo "B failures to rerun on M (zvol tests excluded): $(wc -l < $L/rerun-list.txt)"
cat $L/rerun-list.txt
i=0
while IFS= read -r n; do
    [ -z "$n" ] && continue
    i=$((i + 1))
    if [ $i -gt 8 ]; then echo "more than 8: the rest is not rerun"; break; fi
    sub=${n%/*}; base=${n##*/}
    echo "r$i = $sub/$base"
    sh $L/run.sh M "rr$i" test "$sub" "$base" > $L/rr$i-console.txt 2>&1; rc=$?
    cat $L/rr$i-console.txt
    echo "rr$i run.sh rc=$rc"
    [ $rc -eq 0 ] || break
done < $L/rerun-list.txt
} > $L/rerun-console.txt 2>&1
cat $L/rerun-console.txt
```

If `rerun-list.txt` is empty (only the four zvol tests failed on B, the expected
case), the block prints `B failures to rerun on M ...: 0` and runs nothing.

How to read a rerun:

- **FAIL alone on M too:** not the branch. Either an environmental or a master
  issue; for the four zvol tests this is the missing `60-zvol.rules`.
- **PASS alone on M:** the test failed in B's group but passes alone on M. Check
  B's `brsend-<name>-out.txt`: if its first `ERROR:` is a `truncate`/`zpool
  create` on a missing `$TESTDIR`, it is a group-ordering cascade behind an
  earlier (zvol) failure in the group, not the branch. Report it as such with
  the line. If instead the failure is inside the compressor, the send/receive,
  or the data comparison, that is a candidate finding against the branch:
  report the `end line` and every `ERROR:` line, and do not rerun it on B (that
  would need another module swap). A `-T rsend` on M would settle a genuine
  ambiguity; this doc does not include one, so flag it for the maintainer.

## STEP 6: cross-version pools, case (a) — written by M, read by B

M is loaded. Write the two M-written pools (ashift 9 and 12), run M's `zdb -bcc`
on each, then swap to B and read them with compressed ARC on and off and run B's
`zdb -bcc`.

```sh
. /var/log/zts-lz4-1026/env.sh
sh $L/cv.sh guard M
sh $L/cv.sh write M cvm9  9  2>&1 | tee $L/cv-write-cvm9.txt
sh $L/cv.sh write M cvm12 12 2>&1 | tee $L/cv-write-cvm12.txt
sh $L/cv.sh zdb   M cvm9      2>&1 | tee $L/cv-zdb-M-cvm9.txt
sh $L/cv.sh zdb   M cvm12     2>&1 | tee $L/cv-zdb-M-cvm12.txt
$W/M/zpool list -H -o name; echo "pools imported after write/zdb: rc=$? (want: none)"
```

Expected from `write`: each pool created and exported; the master-node (object
1 of `pool/enc`) line printing `L/P sizes:` with `L` not equal to `P` at
ashift 9 (compressed), and possibly equal at ashift 12 (stored off). M's `zdb
-bcc`: `No leaks` / `block sum matches`, no `leaked` and no `error` line.

Swap to B:

```sh
. /var/log/zts-lz4-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_M" ] && cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload M rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load B rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_B $SV_B"           # the same value twice
```

If the unload rc is not 0, or `lsmod` prints anything, or the loaded srcversion
is not B's, stop. Otherwise read each M-written pool under B, compressed ARC on
then off, then B's `zdb -bcc`:

```sh
. /var/log/zts-lz4-1026/env.sh
for pool in cvm9 cvm12; do
    sh $L/cv.sh read B $pool M 1 2>&1 | tee $L/cv-read-B-$pool-carc1.txt
    sh $L/cv.sh read B $pool M 0 2>&1 | tee $L/cv-read-B-$pool-carc0.txt
    sh $L/cv.sh zdb  B $pool     2>&1 | tee $L/cv-zdb-B-$pool.txt
done
$W/B/zpool list -H -o name; echo "pools imported after reads: rc=$? (want: none)"
cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled   # 1
```

Expected for each M-written pool read under B:

- `enc data` and `plain data`: **bit-exact** with both `compressed_arc=1` and
  `=0` (the LZ4 block format is unchanged; encrypted data is decrypted, not
  re-compressed).
- `enc ZPL props (master node read)`: the values printed and no error, with
  `compressed_arc=1` (MAC checked against the stored bytes).
- With `compressed_arc=0`: the master-node read is the cross-version
  re-compression case. **Expected clean** (the master node is a single small
  block whose r85 and 1.10 bytes are the same, so the recomputed MAC matches),
  in which case `zpool status -v` shows `No known data errors` and the props
  print. **If instead** the read fails, `zpool status -v` shows a permanent
  error against `<pool>/enc:<object 1>` and the props line reads `mount
  failed: ... Input/output error`; `zfs_dbgmsg` shows an `error 52` (ECKSUM) /
  authentication line. That is the study's predicted `EIO`, a recorded finding,
  **not** a stop; record it exactly as printed.
- `kernel_faults=0` for every read. A non-zero count (a `VERIFY`/`ASSERT`/panic)
  is a stop.
- B's `zdb -bcc` on each pool: `No leaks`, no error.

## STEP 7: cross-version pools, case (b) — written by B, read by M

B is loaded. Write the two B-written pools, run B's `zdb -bcc`, then swap to M
and read them, then M's `zdb -bcc`.

```sh
. /var/log/zts-lz4-1026/env.sh
sh $L/cv.sh guard B
sh $L/cv.sh write B cvb9  9  2>&1 | tee $L/cv-write-cvb9.txt
sh $L/cv.sh write B cvb12 12 2>&1 | tee $L/cv-write-cvb12.txt
sh $L/cv.sh zdb   B cvb9      2>&1 | tee $L/cv-zdb-B-cvb9.txt
sh $L/cv.sh zdb   B cvb12     2>&1 | tee $L/cv-zdb-B-cvb12.txt
$W/B/zpool list -H -o name; echo "pools imported after write/zdb: rc=$? (want: none)"
```

Swap to M:

```sh
. /var/log/zts-lz4-1026/env.sh
[ "$(cat /sys/module/zfs/srcversion)" = "$SV_B" ] && cd $W/B && nice -n 10 ionice -c3 ./scripts/zfs.sh -u; echo "unload B rc=$?"   # 0
lsmod | grep -E '^(zfs|spl) '                                       # nothing
cd $W/M && nice -n 10 ionice -c3 ./scripts/zfs.sh; echo "load M rc=$?"   # 0
echo "loaded $(cat /sys/module/zfs/srcversion), SV_M $SV_M"           # 8852710DB871A7587B9E722 twice
```

If the unload rc is not 0, or `lsmod` prints anything, or the loaded srcversion
is not M's, stop. Otherwise:

```sh
. /var/log/zts-lz4-1026/env.sh
for pool in cvb9 cvb12; do
    sh $L/cv.sh read M $pool B 1 2>&1 | tee $L/cv-read-M-$pool-carc1.txt
    sh $L/cv.sh read M $pool B 0 2>&1 | tee $L/cv-read-M-$pool-carc0.txt
    sh $L/cv.sh zdb  M $pool     2>&1 | tee $L/cv-zdb-M-$pool.txt
done
$W/M/zpool list -H -o name; echo "pools imported after reads: rc=$? (want: none)"
cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled   # 1
```

Expected: the mirror of STEP 6, now with master's r85 reading the branch's
1.10-written pool. Data bit-exact in all cases; master node read clean with
`compressed_arc=1`; with `compressed_arc=0` expected clean (small master node),
an `EIO` the recorded predicted failure if the bytes differed;
`kernel_faults=0`; `zdb -bcc` clean from M's zdb.

As a control, the same read on the **same** tree must always be clean. If any
same-version reader saw an error, or any cross-version reader saw a kernel
fault, stop.

## STEP 8: unload, clean the pools, report, cleanup

Unload only when no run and no cross-version read stopped with a leftover or a
kernel fault (a busy or wedged module must not be forced out). Otherwise skip
this block, keep everything, and say why in the report.

```sh
. /var/log/zts-lz4-1026/env.sh
# Destroy any cross-version pool and its vdev dir (idempotent; guards the tree).
cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
if [ "$cur" = "$SV_M" ]; then t=M; elif [ "$cur" = "$SV_B" ]; then t=B; else t=""; fi
if [ -n "$t" ]; then
    for pool in cvb9 cvb12 cvm9 cvm12; do
        sh $L/cv.sh clean $t $pool 2>&1
    done
    (cd $W/$t && nice -n 10 ionice -c3 ./scripts/zfs.sh -u); echo "unload $t rc=$?"   # 0
else
    echo "no module of this task is loaded (srcversion '$cur'): not unloading"
fi
lsmod | grep -E '^(zfs|spl) '                                       # nothing
journalctl -k -b --no-pager -q | grep -iE "$KPAT" | grep -vcE "$KIGN"   # the STEP 0 baseline
ls -l /etc/zfs/zpool.cache 2>&1                                    # as in STEP 0, or a 0-byte file
ls -ld /var/tmp/test_results /var/tmp/zts-19314 2>&1               # as in STEP 0, untouched
ls -d /testpool* /cvpool* 2>&1; losetup -a                         # No such file or directory, then nothing
ls -d $W/cvmnt-* 2>&1                                              # No such file or directory (mountpoints removed with the datasets)
df -h /var/tmp
```

Write `/var/log/zts-lz4-1026/REPORT.txt` with:

- STEP 0 output (short), `uname -r`, the `60-zvol.rules`, `/etc/zfs/zpool.cache`,
  `test_results`/`zts-19314` lines, and the kernel baseline;
- `step1.txt` whole: the two HEADs, every STEP 1 check (including the two 0-error
  0-warning build logs, the `debug build: 3` lines, the glue-present/old-files-
  gone checks) and the two srcversions;
- the STEP 2 output, and `env.sh` from the line `SV_B=` on;
- the load checks of STEP 3 and STEP 4 (with both tunables lines), and
  `guards.txt` whole;
- for each ZTS group run (bcomp, brsend, brecv, mcomp, mrsend, mrecv) and each
  STEP 5 rerun: its result counts, wall time and the whole `<label>-notes.txt`;
  and the STEP 5 `rerun-console.txt` whole;
- for the cross-version part: the four `cv-write-*.txt`, the eight
  `cv-read-*.txt` (data result, props line, `zpool status -v`, the
  `zfs_dbgmsg` tail and `kernel_faults` for each), and the eight `cv-zdb-*.txt`,
  each under case (a) and case (b) headings; state for each read whether it was
  clean or `EIO`, and against which object;
- kernel findings per run and the final journal count against the STEP 0
  baseline;
- any leftover, and the STEP 8 output;
- the two "Expected" summaries below, then whether each matched;
- every deviation from these instructions, with its reason.

Then print it with `cat /var/log/zts-lz4-1026/REPORT.txt` and copy it whole into
your reply. Only after that, as the very last command, and only if the unload
block ran and printed `rc=0` with nothing left in `lsmod`, remove the two
directories of this task:

```sh
rm -rf /var/tmp/zts-lz4-1026 /var/log/zts-lz4-1026; echo "rm rc=$?"
ls -d /var/tmp/zts-lz4-1026 /var/log/zts-lz4-1026 2>&1   # both: No such file or directory
```

If the permission system refuses this `rm -rf`, leave both directories as they
are and say so in the reply. Do not delete them any other way.

## Expected, as a block for REPORT.txt

```
Every ZTS run: run.sh rc=0; zts_marks >= 1; kernel_matches=0; every leftover
empty; tunables_same=yes. "Missing util(s):" may list exportfs; it must not
list zstream, zdb, zpool or zfs.

ZTS, B then M (same within-version compressor, so B and M match):
  compression: PASS 8 (+ setup/cleanup), no FAIL, both trees.
  cli_root/zfs_receive: PASS 31 (+ setup/cleanup), both trees; any non-PASS is
    a zts-report expected SKIP/UNSUPPORTED.
  rsend: 88 results. rsend_008_pos SKIP (issue 6066). The four zvol tests
    (recv_dedup_encrypted_zvol, send-wR_encrypted_zvol, send-c_volume,
    send-c_stream_size_estimate) FAIL on both trees (this VM has no
    60-zvol.rules); a group cascade may add one or two more. send-c_* and
    send_embedded_* PASS on both trees.
  STEP 5: every non-zvol B failure, rerun alone on M, either FAILs on M too
    (not the branch) or is a group cascade behind a zvol failure (missing
    $TESTDIR, not the branch). No failure isolates to the LZ4 change.

Cross-version pools, both directions (M->B and B->M), ashift 9 and 12:
  enc data and plain data: bit-exact with compressed_arc=1 and =0, all cases.
  enc master node (authenticated) with compressed_arc=1: read OK, all cases.
  enc master node with compressed_arc=0, cross-version: expected CLEAN, because
    the ZPL master node is a single small block whose r85 and 1.10 bytes are
    identical; this confirms the arc_hdr_authenticate() EIO is not reachable
    through a ZPL dataset at these sizes (it was reproduced only in libzpool,
    with large authenticated metadata objects). If a read does EIO, that is the
    study's predicted failure: record the object, the error, zpool status -v
    and zfs_dbgmsg. Same-version reads always clean.
  kernel_faults=0 on every read. zdb -bcc from both trees' zdb on every pool:
    No leaks, no error.
STOP only on: a guard failure, a changed/unrestored tunable, a leftover, an
  outer timeout, or a kernel VERIFY/ASSERT/panic/oops/hung task in any state.
```

## What each result means

- **The update is clean** when: both trees give the same ZTS picture over the
  three groups, with only the VM's zvol tests (and any cascade behind them)
  failing and nothing isolating to the LZ4 change in STEP 5; every
  cross-version data read is bit-exact; the authenticated master-node read is
  clean with compressed ARC on, and clean (or a recorded `EIO`) with it off,
  with no kernel fault; and `zdb -bcc` is clean from both `zdb`s.
- **A finding against the change** is a STEP 5 rerun that fails inside the
  compressor, the send/receive or the data comparison and passes alone on M
  (not a cascade), or any cross-version **data** read that is not bit-exact, or
  a same-version read that errors. Report it with the lines.
- **The predicted authenticated-metadata `EIO`** (a cross-version read with
  compressed ARC off failing on `<pool>/enc` object 1) is the study's finding,
  recorded with its `zpool status -v` and `zfs_dbgmsg`, not a stop.
- **A stop** is any kernel `VERIFY`/`ASSERT`/panic/oops/hung task (in a ZTS run
  or a cross-version read), a guard or leftover failure, a tunable left changed,
  or an outer timeout. Report the lines and the state it happened in; do not
  unload a wedged module.

Estimated duration: 90 to 120 minutes (checks and loads ~10; the three B groups
~35, the three M groups ~35; STEP 5 reruns 0 to ~10; the cross-version writes,
reads and `zdb -bcc` ~15; report and cleanup ~5).
