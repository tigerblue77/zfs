# VM task: L2ARC reads of blocks cached decompressed (l2arc-retry-size)

Self-contained task for a **disposable Debian 13 VM** (no pool on it that
matters, reboots allowed, 4 GiB of RAM or more). Nothing is posted anywhere;
results stay on the VM and are copied back by hand.

## What is being tested

With `zfs_compressed_arc_enabled=0` the ARC caches a compressed block
decompressed (`arc_hdr_size()` = LSIZE), while the cache device holds it as
stored (PSIZE bytes, padded to the device's allocation size).
`arc_read()` therefore reads PSIZE bytes from the cache device. Master
`0cdf531257` gets two things wrong in `l2arc_read_done()`:

1. **Bounce buffer.** When the allocation size is larger than PSIZE (ashift=12
   cache device, PSIZE not a multiple of 4K), the read goes into a bounce
   buffer, and `l2arc_read_done()` copies `arc_hdr_size()` = LSIZE bytes out
   of it. A debug build fails
   `VERIFY3U(arc_hdr_size(hdr), <, zio->io_size)`. Without assertions
   `abd_copy()` stops at the end of the bounce buffer and loops without
   progress: the read never completes and a `z_rd_int` thread spins.
2. **Retry size.** When the copy on the cache device fails its checksum
   (damaged device, or another compressor), the block is read again from the
   pool with PSIZE as its logical size; its decompression fails and the read
   returns EIO instead of the intact data.

Fix: fork branch `l2arc-retry-size` at
`65c6d002fe6c9cdb688d08bf3991cf171c2a2f8f`, two commits on master
`0cdf531257`: `c81e770827` (retry size), then `65c6d002fe` (copy PSIZE bytes
from the bounce buffer; it needs the first one).

Already established on the development host (userspace, libzpool built
`--with-config=user`, the program from `../bounce/repro/`, pool ashift=9,
compressed ARC off, reads after `arc_flush()`):

| build | cache device | result |
| --- | --- | --- |
| master, debug | ashift=12 | `VERIFY3U(arc_hdr_size(hdr), <, zio->io_size) failed (64512 < 12288)` |
| master, libzpool rebuilt with `-DNDEBUG` | ashift=12 | read never returns; `z_rd_int` in `abd_iterate_func2()`, 52224 bytes left |
| master, debug | ashift=9, bit flips injected on cache reads | 23 checksum failures, EIO on all four objects |
| `c81e770827` only | ashift=9 + bit flips / ashift=12 | correct / same VERIFY |
| `65c6d002fe` | ashift=12, ashift=9, with and without bit flips, compressed ARC on and off | all correct |
| bounce commit alone on master | ashift=12 + bit flips | EIO (why it sits on top of the retry commit) |

Not yet established, and the point of this task: the same in the kernel
through the ZPL, a damaged cache device instead of injected bit flips, and
the l2arc ZTS group.

## Safety rules

- Disposable VM only. `scripts/zfs.sh -r` replaces the module stack; the
  script creates and destroys the pool `l2rt` on files in
  `/var/tmp/l2arc-retry`; ZTS creates and destroys `testpool*` pools and
  loop devices.
- On master the `bounce` case leaves a kernel thread stuck (debug: halted
  after the VERIFY; release: spinning). The script then exits 3 without
  unloading: **reboot** before the next step.
- Run as root. Post nothing.

## Files needed on the VM

Copy to `/root/` on the VM, from
`/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1102/arc-auth-recompress/l2arc-vm/`:
`vm-l2arc-retry.sh`, sha256
`073e2208591f1c0bedadd5a435a690835eeb5444fde5ec8e0c18022df93c5275`.

```sh
cd /root && sha256sum vm-l2arc-retry.sh && chmod +x vm-l2arc-retry.sh
```

## 1. Prepare the VM (once)

```sh
export DEBIAN_FRONTEND=noninteractive
timeout 1800 apt-get update -y
timeout 3600 apt-get install -y build-essential git autoconf automake \
    libtool libtool-bin gawk alien fakeroot dkms acl attr bc cpio \
    cryptsetup dbench fio gdb ksh jq libacl1-dev libaio-dev libattr1-dev \
    libblkid-dev libcurl4-openssl-dev libelf-dev libffi-dev libpam0g-dev \
    libssl-dev libtirpc-dev libudev-dev uuid-dev zlib1g-dev lsscsi parted \
    python3 python3-cffi python3-dev python3-packaging python3-setuptools \
    rsync strace sysstat xxhash "linux-headers-$(uname -r)"
zpool list 2>/dev/null; lsmod | grep -w zfs   # must show no pool that matters
```

## 2. Builds (about 15 min each)

M = master `0cdf531257`, F = `65c6d002fe`, both `--enable-debug`.
MR = master without `--enable-debug`, only for the optional hang run.

```sh
cd /root && [ -d zfs ] || timeout 1800 git clone https://github.com/openzfs/zfs.git
cd /root/zfs && timeout 600 git fetch origin &&
    timeout 600 git fetch https://github.com/tigerblue77/zfs l2arc-retry-size
git rev-parse FETCH_HEAD          # must be 65c6d002fe6c9cdb688d08bf3991cf171c2a2f8f
rm -rf /root/M /root/MR /root/F; git worktree prune
git worktree add --detach /root/M 0cdf531257 > /dev/null
git worktree add --detach /root/MR 0cdf531257 > /dev/null
git worktree add --detach /root/F 65c6d002fe6c9cdb688d08bf3991cf171c2a2f8f > /dev/null
for t in M F MR; do git -C /root/$t log -2 --format="$t %h %s"; done
for t in M F MR; do
	dbg=--enable-debug; [ $t = MR ] && dbg=
	(cd /root/$t && timeout 1800 ./autogen.sh && timeout 1800 ./configure $dbg &&
	    timeout 5400 make -j"$(nproc)") > /root/build-$t.log 2>&1
	echo "build $t rc=$?"; tail -1 /root/build-$t.log
done
```

Skip MR (and its run in section 4) if time is short.

## 3. Kernel cases through the ZPL

Each run: pool on a file (ashift=9, lz4, recordsize=128K), 1 GiB cache file,
`zfs_compressed_arc_enabled=0`, a 96 MiB file of `seq` output (compressible;
the script logs how many records have a PSIZE that is not a multiple of 4K).
Once the L2ARC has stopped growing: unmount, `zinject -a` (flushes the ARC),
mount, `sha256sum` the file (bounded to 120 s), compare with the expected
sum. Cases: `bounce` (cache ashift=12), `corrupt` (cache ashift=9, its data
region overwritten with random bytes before the read: from the L2ARC
header's `start` + 1 MiB, at most 64 MiB, stopping 1 MiB before its `end`;
labels and device header untouched, log blocks only matter to a rebuild,
which the run does not do), `both` (ashift=12 and the overwrite). Every
result is one `RESULT` line in `run.log`.

```sh
R=/root/res; rm -rf $R; mkdir -p $R
/root/vm-l2arc-retry.sh /root/F $R F-bounce  bounce;  echo "rc=$?"
/root/vm-l2arc-retry.sh /root/F $R F-corrupt corrupt; echo "rc=$?"
/root/vm-l2arc-retry.sh /root/F $R F-both    both;    echo "rc=$?"
/root/vm-l2arc-retry.sh /root/M $R M-corrupt corrupt; echo "rc=$?"
/root/vm-l2arc-retry.sh /root/M $R M-bounce  bounce;  echo "rc=$?"   # expect 3
grep -E 'SETUP|WARN|psize_not_4k|overwrote|RESULT|REBOOT' $R/run.log
```

Then **reboot** (M-bounce leaves the stuck thread).

Expected from the userspace runs (not run in a kernel yet):

| run | expected `RESULT` |
| --- | --- |
| F-bounce | `rc=0 data_ok=yes l2_hits=+N` (N > 0) `l2_cksum_bad=+0 dmesg_verify=0` |
| F-corrupt | `data_ok=yes l2_cksum_bad=+N` (N > 0) `data_errors=0 dmesg_verify=0` |
| F-both | `data_ok=yes l2_cksum_bad=+N` (N > 0) `dmesg_verify=0` |
| M-corrupt | `read_rc` non-zero, `err=[...Input/output error]`, `data_ok=no`, `l2_cksum_bad=+N` (N > 0); `data_errors` may be 1 |
| M-bounce | `read=HUNG`, exit 3; `M-bounce-dmesg.txt` has `VERIFY3U(arc_hdr_size(hdr), <, zio->io_size) failed` and `PANIC at arc.c:...:l2arc_read_done()` |

`psize_not_4k` must be well above 0, and a `WARN ... no L2ARC hit` line means
the run read from the pool only and proves nothing. A `SETUP` line means the
same. If `zpool add -o ashift=12` is not applied to the cache file, the
script falls back by itself to a loop device with 4K sectors and says so.

## 4. Optional: the hang on a build without assertions

After the reboot:

```sh
R=/root/res; /root/vm-l2arc-retry.sh /root/MR $R MR-bounce bounce; echo "rc=$?"
grep -E 'SETUP|RESULT|REBOOT' $R/run.log | tail -3
grep -E 'z_rd_int' $R/MR-bounce-threads.txt
grep -n -A12 'abd_iterate_func2\|soft lockup' $R/MR-bounce-dmesg.txt | head -40
```

Expected (from reading, not run in a kernel): `read=HUNG`, exit 3; a
`z_rd_int` thread at about 100% CPU in `MR-bounce-threads.txt`; the sysrq-l
backtrace in `MR-bounce-dmesg.txt` in `abd_iterate_func2()` <-
`abd_copy_off()` <- `l2arc_read_done()`; possibly `soft lockup` warnings.
Then **reboot**.

## 5. ZTS on F (about 1 h per run)

```sh
cd /root/F
timeout 300 ./scripts/zfs.sh -r; echo "load $?"
ko=$(find module -name zfs.ko | head -1)
[ "$(cat /sys/module/zfs/srcversion)" = "$(modinfo -F srcversion $ko)" ] && echo "F loaded"
timeout 600 ./scripts/zfs-helpers.sh -i; echo "helpers $?"
timeout 7200 ./scripts/zfs-tests.sh -v -T l2arc > /root/zts-F.log 2>&1; echo "zts $?"
echo 0 > /sys/module/zfs/parameters/zfs_compressed_arc_enabled
timeout 7200 ./scripts/zfs-tests.sh -v -T l2arc > /root/zts-F-carc0.log 2>&1; echo "zts $?"
for f in /root/zts-F.log /root/zts-F-carc0.log; do
	echo "== $f"; grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED' -A0 $f
	grep -E '\[(FAIL|KILLED)\]' $f
done
dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
```

The tag holds the l2arc and persist_l2arc tests (among them
`l2arc_compressed_arc_disabled`, `l2arc_encrypted_no_compressed_arc` and
`persist_l2arc_00[1-5]`). ZTS cache devices are files with the pool's
ashift (from reading), so ZTS does not reach the bounce buffer; it is the
regression check.
Some tests may assume compressed ARC in their statistics. **If F has any
FAIL or KILLED**, run the same two commands from `/root/M` (after
`./scripts/zfs.sh -r` there and `zfs-helpers.sh -i`) into `zts-M.log` and
`zts-M-carc0.log`, and compare:

```sh
for s in "" -carc0; do
	diff <(grep -E '\[(FAIL|KILLED)\]' /root/zts-M$s.log | awk '{print $2, $NF}' | sort) \
	     <(grep -E '\[(FAIL|KILLED)\]' /root/zts-F$s.log | awk '{print $2, $NF}' | sort)
done
```

Expected: nothing that fails on F only, and a dmesg count of 0.

## Evidence to collect

`/root/res/` (`run.log`, every `*-read.err`, `*-status.out`, `*-dmesg.txt`,
`*-psize.txt`, `*-cache-label.txt`, `*-cache-hdr.txt`, and for a hung run
`*-threads.txt` and `*-stacks.txt`), the tails of `/root/build-*.log`,
`/root/zts-*.log` with the result directory each names
(`/var/tmp/test_results/<date>/`), and `dmesg > /root/dmesg-<step>.txt`
before any reboot.

## Report format

```
builds: M <sha> rc=  F <sha> rc=  MR <sha> rc=|skipped
F-bounce:  <RESULT line>
F-corrupt: <RESULT line>
F-both:    <RESULT line>
M-corrupt: <RESULT line>
M-bounce:  <RESULT line> + first VERIFY/PANIC line of M-bounce-dmesg.txt
MR-bounce: <RESULT line> + top 6 frames of the spinning z_rd_int | skipped
psize_not_4k per run: ...
ZTS F l2arc:       PASS n FAIL n SKIP n KILLED n; FAIL/KILLED lines
ZTS F l2arc carc0: PASS n FAIL n SKIP n KILLED n; FAIL/KILLED lines
ZTS M (only if F failed): same, plus the diff output
dmesg VERIFY/ASSERT/PANIC count after the F runs and after ZTS
anything that differs from the expected tables, as it is
```
