# VM task: authenticated blocks with compressed ARC disabled (arc-auth-no-recompress)

Self-contained task for a **disposable Debian 13 VM** (no pool on it that
matters, reboots allowed). Nothing is posted anywhere; results stay on the
VM and are copied back by hand.

## What is being tested

Blocks of an encrypted dataset whose object type is authenticated but not
encrypted (the ZPL master node, object 1 of every encrypted file system)
carry a MAC computed over the block as stored, compressed. With
`zfs_compressed_arc_enabled=0`, master's `arc_hdr_authenticate()` compresses
the cached logical data again and checks the MAC over that. If the reader's
compressor does not give back the writer's bytes (other byte order, other
compressor version, such as the pending LZ4 1.10 update), the MAC check
returns ECKSUM, the read EIO, and the mount of the file system fails with a
permanent error and an `ereport.fs.zfs.authentication`.

The fix (fork branch `arc-auth-no-recompress`, commit
`62abfac4146ad7c6e5038641447cd01a6d365c98`, one commit on master
`0cdf531257`) keeps such blocks as stored in the ARC whatever the tunable
says, and checks the MAC over them directly.

To get a pool whose master node master's LZ4 does not reproduce, the image
is written by a **test-only writer**: master plus `lz4-writer-tail.patch`,
whose LZ4 compressor ends its last match 64 bytes earlier than r85 does.
That is valid LZ4 (master decompresses it; it simply ends with a longer
literal run), but different bytes.

Already established on the development host (userspace, libzpool, master
`0cdf531257` built `--with-config=user --enable-debug`; libzpool program
`repro/authmac.c` creating an encrypted `DMU_OST_OTHER` dataset with a
ZPL-like master node microzap, two ZAPs and a text object, all
authenticated):

| Pool written by | reader | compressed ARC | result |
| --- | --- | --- | --- |
| tail-patched LZ4 | master | 0 | master node 8/8 lookups EIO, every other object EIO |
| tail-patched LZ4 | master | 1 | all correct (the image is valid) |
| tail-patched LZ4 | fix | 0 | all correct, also when first read without the key |
| LZ4 hashing byte-swapped words (= a big-endian writer) | master | 0 | 2 ZAPs and text object EIO; 5 of 400 master-node-like microzaps EIO |
| same | fix | 0 | all correct |
| LZ4 1.10 (fork branch `lz4-1.10`) | master | 0 | 4 of 8 64K text blocks EIO; ZAPs and 400 master nodes recompress identically |
| LZ4 1.10 | fix | 0 | all correct |
| master | fix | 0 and 1 | all correct |

Persistent L2ARC (cache device offlined/onlined after `arc_flush()`, so the
headers come back L2-only, then read with compressed ARC disabled, then the
cache device removed): the fix passes; the fix without its `l2ad_alloc`
move fails `VERIFY3U(rc->rc_count, ==, number)` in
`zfs_refcount_destroy_many()` at the removal.

Not yet established, and the point of this task: the same in the kernel
through the ZPL (a mount), and ZTS around encryption, compression, ARC and
L2ARC with the fix.

## Safety rules

- Disposable VM only. `scripts/zfs.sh -r` replaces the module stack; the
  script creates and destroys the pool `arcauth` on a file in
  `/var/tmp/arc-auth`; ZTS creates and destroys `testpool*` pools and loop
  devices.
- If a module unload fails or anything hangs past its bound, collect the
  evidence (end of this file) and **reboot** before the next step.
- Run as root. Post nothing.

## Files needed on the VM

Copy to `/root/` on the VM, from
`/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1102/arc-auth-recompress/`:

| file | sha256 |
| --- | --- |
| `arc-auth-no-recompress.patch` (`git format-patch` of 62abfac414) | `a25e75fee85bf81e48c6b1ffe223cb6b4731ce0551c7b38932966972a94c27c7` |
| `lz4-writer-tail.patch` (test only, never to be merged) | `79d6e923e25c1eac2bf468c81e184b6279b7fda3896cce480b1e6a5e6d5fccb8` |
| `vm-arc-auth.sh` | `752401ce16a2a0f311f9130de24a511a2ba2357c1fb54053f1a7e7c116bc83fb` |

```sh
cd /root && sha256sum arc-auth-no-recompress.patch lz4-writer-tail.patch vm-arc-auth.sh
chmod +x /root/vm-arc-auth.sh
```

Instead of the first patch, the fix can be fetched:
`git fetch https://github.com/tigerblue77/zfs arc-auth-no-recompress`
(its head must be `62abfac414`).

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

## 2. Three builds (about 3 x 15 min)

W = writer (master + test LZ4 patch), M = master, F = fix. All
`--enable-debug`, all from master `0cdf531257`.

```sh
cd /root && [ -d zfs ] || timeout 1800 git clone https://github.com/openzfs/zfs.git
cd /root/zfs && timeout 600 git fetch origin
for t in W M F; do
	rm -rf /root/$t; git -C /root/zfs worktree prune
	git -C /root/zfs worktree add --detach /root/$t 0cdf531257 > /dev/null || echo "worktree $t failed"
done
git -C /root/W apply /root/lz4-writer-tail.patch && echo "W patched"
git -C /root/F -c user.name=vm -c user.email=vm@localhost am -q /root/arc-auth-no-recompress.patch && echo "F patched"
for t in W M F; do git -C /root/$t log -1 --format="$t %h %s"; git -C /root/$t status --short; done
for t in W M F; do
	(cd /root/$t && timeout 1800 ./autogen.sh && timeout 1800 ./configure --enable-debug &&
	    timeout 5400 make -j"$(nproc)") > /root/build-$t.log 2>&1
	echo "build $t rc=$?"; tail -1 /root/build-$t.log
done
```

Expected: `W` and `M` at `0cdf531257` (W with `module/zfs/lz4_zfs.c`
modified), `F` one commit above it; three builds `rc=0`.

## 3. Kernel proof (about 10 min)

```sh
R=/root/res; rm -rf $R; mkdir -p $R
/root/vm-arc-auth.sh write /root/W $R; echo "write rc=$?"
/root/vm-arc-auth.sh read /root/M $R M-c1-key        1 key
/root/vm-arc-auth.sh read /root/M $R M-c0-key        0 key
/root/vm-arc-auth.sh read /root/M $R M-c0-nokeyfirst 0 nokeyfirst
/root/vm-arc-auth.sh read /root/F $R F-c0-key        0 key
/root/vm-arc-auth.sh read /root/F $R F-c0-nokeyfirst 0 nokeyfirst
/root/vm-arc-auth.sh read /root/F $R F-c1-key        1 key
grep -E 'SETUP|write:|RESULT|failed' $R/run.log
dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
```

Expected from the userspace runs (not run in a kernel yet):

| run | mount_rc | file | auth_ereports | permanent_errors |
| --- | --- | --- | --- | --- |
| M-c1-key (control: the image is valid) | 0 | hello | 0 | 0 |
| M-c0-key | non-zero, "Input/output error" | empty | >= 1 | 1 |
| M-c0-nokeyfirst | non-zero, "Input/output error" | empty | >= 1 | 1 |
| F-c0-key | 0 | hello | 0 | 0 |
| F-c0-nokeyfirst | 0 | hello | 0 | 0 |
| F-c1-key | 0 | hello | 0 | 0 |

`write:` must say `master node is lz4 400L/200P`; otherwise the image does
not test the path (a 4K-sector setup would store the master node
uncompressed) and the reads prove nothing. If `zfs load-key` fails on the
read-only import (`loadkey_rc` non-zero), rerun the read lines with `RW=1`
in front (the image is copied fresh for every read). A dmesg count other
than 0, or a SETUP line, is a finding: stop and collect the evidence.

## 4. ZTS on F, default settings (about 2 h)

```sh
cd /root/F
timeout 300 ./scripts/zfs.sh -r; echo "load $?"
ko=$(find module -name zfs.ko | head -1)
[ "$(cat /sys/module/zfs/srcversion)" = "$(modinfo -F srcversion $ko)" ] && echo "F loaded"
timeout 600 ./scripts/zfs-helpers.sh -i; echo "helpers $?"
timeout 14400 ./scripts/zfs-tests.sh -v -T crypto,compression,l2arc,arc,zfs_load-key \
    > /root/zts-F.log 2>&1; echo "zts $?"
grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED|^Running Time' /root/zts-F.log
grep -E '\[(FAIL|KILLED)\]' /root/zts-F.log
dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
```

The tags hold, among others, `l2arc_encrypted_no_compressed_arc`,
`l2arc_compressed_arc_disabled` and `persist_l2arc_00[1-5]` (005 restores
encrypted L2ARC contents). Expected: no FAIL or KILLED that does not also
occur on M with the same tags; a failure must be compared against a run of
the same tags on M (after `./scripts/zfs.sh -r` from `/root/M`) before it
is blamed on the patch.

## 5. Same tags with compressed ARC disabled, M then F (optional, about 4 h)

This runs the persistent L2ARC and encryption tests through the changed
code (authenticated headers kept compressed, `l2ad_alloc` moved on a
restored header) instead of only where a test disables the tunable itself.
Tests that check compressed ARC statistics may fail the same way on both.

```sh
for t in M F; do
	cd /root/$t && timeout 300 ./scripts/zfs.sh -r && timeout 600 ./scripts/zfs-helpers.sh -i
	echo 0 > /sys/module/zfs/parameters/zfs_compressed_arc_enabled
	timeout 14400 ./scripts/zfs-tests.sh -v -T crypto,compression,l2arc,arc,zfs_load-key \
	    > /root/zts-$t-carc0.log 2>&1; echo "zts $t rc=$?"
	grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED' /root/zts-$t-carc0.log
	grep -E '\[(FAIL|KILLED)\]' /root/zts-$t-carc0.log | sed "s/^/$t /"
	dmesg | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
done
diff <(grep -E '\[(FAIL|KILLED)\]' /root/zts-M-carc0.log | awk '{print $2, $NF}' | sort) \
     <(grep -E '\[(FAIL|KILLED)\]' /root/zts-F-carc0.log | awk '{print $2, $NF}' | sort)
```

Anything failing on F only is a finding. A debug-build ASSERT in dmesg
during the persistent L2ARC tests on F (`zfs_refcount_destroy_many`,
`arc_hdr_keep_compressed`, `arc_hdr_verify`) is a finding.

## Evidence to collect

`/root/res/` (run.log, every `*-mount.out`, `*-status.out`, `*-events.out`,
`*-dbgmsg.txt`, `zdb-master-node.txt`), `/root/build-*.log` tails,
`/root/zts-*.log` plus the ZTS result directory each one names
(`/var/tmp/test_results/<date>/`), and `dmesg > /root/dmesg.txt` before any
reboot. Report the RESULT lines, the ZTS summaries and every FAIL/KILLED
line as they are.

## Script `vm-arc-auth.sh`

Copied above from the output directory (sha256 in the table). Usage:
`vm-arc-auth.sh write TREE RESDIR` and
`vm-arc-auth.sh read TREE RESDIR LABEL CARC key|nokeyfirst`. It loads the
tree's modules with `scripts/zfs.sh -r`, checks the loaded `srcversion`
against the tree's `zfs.ko`, and unloads them at the end of each step.
