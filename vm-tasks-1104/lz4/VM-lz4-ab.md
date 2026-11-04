# VM task: kernel A/B of the LZ4 1.10 update (lz4-1.10-rb1103)

Self-contained task for a **disposable Debian 13 VM** (no pool on it that
matters, reboots allowed, 4 or more vCPUs, 8 GiB of RAM or more). Nothing is
posted anywhere; results stay on the VM and are copied back by hand.

## What is being tested

Fork branch `lz4-1.10-rb1103` (head `85fb615e35892b3549fbdc4c871f4da06e8e3e91`,
three commits on master `3028351741106e28af57961a97d81c760a73fd44`) replaces
master's LZ4 (the r85 compressor of `module/zfs/lz4_zfs.c`, the 1.9.3-derived
decoder of `module/zfs/lz4.c`) with the unmodified LZ4 1.10.0 library through
`module/lz4/zfs_lz4.c`.

- **A** = master `3028351741` (the branch's base), **B** = branch head
  `85fb615e35`. Both **release** builds: plain `./configure`, no
  `--enable-debug`, no `--enable-debuginfo` (it adds `-fno-inline`).
- Kernel throughput through the ZPL, `compression=lz4`, `ashift=12`, one
  pool on a RAM disk (`brd`), so that the disk does not limit it.
  Recordsize 128K and 16K, a mixed data set (OpenZFS sources, binaries,
  libraries, documentation) and a random one. Rounds alternate the order
  (A B, B A, ...), each run on a freshly created pool; the read follows an
  export and import, so it decompresses from disk.
- Then `zfs-tests.sh -T compression` on B (and on A only if B shows a
  failure).

B does not include the ARC fix it depends on (fork branch
`arc-auth-no-recompress`); that fix only matters for blocks written by
another compressor with compressed ARC disabled, which nothing here does.

Already established on the development host (userspace, not kernel;
`--enable-debug` for correctness, `-O2 -DNDEBUG` harness for speed, one
core pinned, medians of 7 and 15 alternating runs, noisy VM):

| | sources 4K / 16K / 128K | /usr/bin 4K / 16K / 128K |
| --- | --- | --- |
| compression speed B/A | 0.98-1.00 / 1.01-1.11 / 1.00-1.05 | 1.01-1.02 / 1.00-1.07 / 1.00-1.07 |
| decompression speed B/A | 1.01-1.08 / 0.99-1.02 / 1.07-1.10 | 1.02-1.03 / 1.05-1.06 / 1.16 |
| stored ratio A -> B, ashift=12 | 1.004 / 1.271 / 1.354 -> 1.004 / 1.271 / 1.358 | 1.011 / 1.627 / 2.086 -> 1.011 / 1.627 / 2.098 |
| blocks stored compressed whose bytes differ, ashift=12 | 1.4% / 24.4% / 39.6% | 0% / 26.5% / 95.2% |

Cross-decoding (A's decoder on B's output and the reverse): 28.8M decodes,
0 failures. Absolute userspace speeds per core: 0.75-1 GiB/s compression and
2.1-3 GiB/s decompression for sources, 0.4-0.5 and 2.1-2.9 GiB/s for
binaries.

Not yet established, and the point of this task: the same comparison in
the kernel, where the zio pipeline, checksums and the ARC are in the path,
and ZTS of the compression tag on B.

## Safety rules

- Disposable VM only. `scripts/zfs.sh` replaces the module stack; the
  script creates and destroys the pool `lzab` on `/dev/ram0`; ZTS creates
  and destroys `testpool*` pools and loop devices.
- If a module unload fails or anything hangs past its bound, collect the
  evidence (step 7) and **reboot** before the next step.
- Run as root unless a step says otherwise. Post nothing.

## Files needed on the VM

Copy to `/root/` on the VM, from
`/tmp/claude-0/-home-user/c83a720a-8008-5855-9bfd-25f0ac5db370/scratchpad/out-1103/lz4-1.10/`:

| file | sha256 |
| --- | --- |
| `vm-lz4-ab.sh` | `f0a66552ab702c1070dd4706f78d28053312eaabd62c0731737c66aa30044432` |

```sh
cd /root && sha256sum vm-lz4-ab.sh && chmod +x /root/vm-lz4-ab.sh
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
    rsync strace sudo sysstat xxhash "linux-headers-$(uname -r)"
zpool list 2>/dev/null; lsmod | grep -w zfs   # must show no pool that matters
modinfo brd > /dev/null && echo "brd available"
nproc; free -g | head -2; df -h /dev/shm | tail -1   # /dev/shm needs 2.5 GiB free
```

## 2. Test user and sources (about 5 min)

```sh
E=/var/tmp/lzab
id zts > /dev/null 2>&1 || useradd -m -s /bin/bash zts
echo 'zts ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/zts && chmod 440 /etc/sudoers.d/zts
echo "sudo as zts: $(runuser -u zts -- sudo -n id -un)"          # must be root
install -d -o zts -g zts "$E"
runuser -u zts -- timeout 1800 git clone -q https://github.com/openzfs/zfs.git "$E/src"
runuser -u zts -- timeout 600 git -C "$E/src" fetch -q https://github.com/tigerblue77/zfs \
    lz4-1.10-rb1103:refs/remotes/fork/lz4-1.10-rb1103
runuser -u zts -- git -C "$E/src" worktree add -q --detach "$E/A" 3028351741106e28af57961a97d81c760a73fd44
runuser -u zts -- git -C "$E/src" worktree add -q --detach "$E/B" 85fb615e35892b3549fbdc4c871f4da06e8e3e91
echo "fork head: $(runuser -u zts -- git -C "$E/src" rev-parse fork/lz4-1.10-rb1103)"   # must be 85fb615e35...
echo "B base:    $(runuser -u zts -- git -C "$E/B" rev-parse HEAD~3)"                    # must be 3028351741...
runuser -u zts -- git -C "$E/B" diff --stat HEAD~3 | tail -1   # 14 files changed, 3977 insertions(+), 1891 deletions(-)
```

## 3. Two release builds (about 2 x 15 min)

```sh
E=/var/tmp/lzab
for t in A B; do
	runuser -u zts -- sh -c "cd $E/$t && ./autogen.sh && ./configure && make -j$(nproc)" \
	    > /root/build-$t.log 2>&1
	echo "build $t rc=$? $(ls "$E/$t/module/zfs.ko" "$E/$t/module/spl.ko" 2>&1 | tr '\n' ' ')"
	grep -E '^(KERNEL_)?DEBUG_CPPFLAGS' "$E/$t/Makefile"   # both "-UDEBUG -DNDEBUG"
	grep -c -- '-fno-inline' "$E/$t/Makefile"  # must be 0
done
ls "$E/B/module/lz4/zfs_lz4.o" "$E/A/module/zfs/lz4_zfs.o"   # each tree built its own LZ4
```

## 4. Data sets in RAM (about 2 min)

```sh
E=/var/tmp/lzab
{ runuser -u zts -- git -C "$E/src" archive --format=tar 3028351741106e28af57961a97d81c760a73fd44
  tar --sort=name -cf - -C / usr/bin usr/lib/x86_64-linux-gnu usr/share/doc usr/include 2>/dev/null
} | head -c $((1536 * 1024 * 1024)) > /dev/shm/lzab-mixed
head -c $((512 * 1024 * 1024)) /dev/urandom > /dev/shm/lzab-random
ls -l /dev/shm/lzab-mixed /dev/shm/lzab-random; sha256sum /dev/shm/lzab-mixed | tee /root/lzab-mixed.sha256
```

The mixed file should be the full 1610612736 bytes; if `/usr` holds less,
it is shorter, which is fine as long as both builds read the same file.

## 5. Kernel A/B (about 30 min for 6 rounds)

Keep the VM otherwise idle, and if possible the host too (no other VM
benchmarking at the same time).

```sh
E=/var/tmp/lzab
R=/root/lzab-res; rm -rf $R
journalctl -k --no-pager | wc -l > /root/kmsg-start
RAM_MB=4096 ARC_MB=1024 /root/vm-lz4-ab.sh "$E/A" "$E/B" $R 6; echo "ab rc=$?"
grep -E 'SETUP|RESULT' $R/run.log           # must print nothing
cat $R/summary.txt
journalctl -k --no-pager | tail -n +$(( $(cat /root/kmsg-start) + 1 )) |
    grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'   # must be 0
```

`summary.txt` gives, per data set and recordsize, the median write and
read throughput with min-max over the rounds, CPU seconds per GiB (all
CPUs, from `/proc/stat`), the pool's `compressratio` and `used`, and the
B/A ratios. `runs.csv` keeps every run.

How to read it:

- Expected from userspace: B/A write and wCPU near 1 (within the min-max
  spread), read B/A 1 or above, `compressratio` equal or slightly higher on
  B for `mixed`, both 1.00 for `random`.
- A finding: a B/A write or read ratio below 0.95, or a wCPU or rCPU ratio
  above 1.05, that holds in every round (compare the min-max ranges and
  `runs.csv`, not only the medians); `compressratio` lower on B; any
  `SETUP` or `RESULT` line; a kernel message count other than 0.
- If the spread between rounds is larger than the A/B difference, rerun
  with 10 rounds (`... $R 10`) rather than reading a difference into it.

## 6. ZTS, compression tag, on B (about 20 min)

```sh
E=/var/tmp/lzab
cd "$E/B" && "$E/B/scripts/zfs.sh" -u; timeout 300 "$E/B/scripts/zfs.sh"; echo "load $?"
[ "$(cat /sys/module/zfs/srcversion)" = "$(modinfo -F srcversion "$E/B/module/zfs.ko")" ] && echo "B loaded"
timeout 600 "$E/B/scripts/zfs-helpers.sh" -i; echo "helpers $?"
runuser -u zts -- timeout -k 60 3600 "$E/B/scripts/zfs-tests.sh" -v -T compression \
    > /root/zts-B.log 2>&1; echo "zts B rc=$?"
grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED|^Running Time' /root/zts-B.log
grep -E '\[(FAIL|KILLED)\]' /root/zts-B.log
```

The tag holds `compress_001_pos` to `compress_004_pos` and the
`l2arc_compressed_arc*` and `l2arc_encrypted*` tests. If B shows a FAIL or
KILLED, run the same on A before blaming the change:

```sh
E=/var/tmp/lzab
"$E/B/scripts/zfs-helpers.sh" -r; "$E/A/scripts/zfs.sh" -u; timeout 300 "$E/A/scripts/zfs.sh"
timeout 600 "$E/A/scripts/zfs-helpers.sh" -i
runuser -u zts -- timeout -k 60 3600 "$E/A/scripts/zfs-tests.sh" -v -T compression \
    > /root/zts-A.log 2>&1; echo "zts A rc=$?"
diff <(grep -E '\[(FAIL|KILLED)\]' /root/zts-A.log | awk '{print $2, $NF}' | sort) \
     <(grep -E '\[(FAIL|KILLED)\]' /root/zts-B.log | awk '{print $2, $NF}' | sort)
```

## 7. What to bring back

```sh
E=/var/tmp/lzab
{ uname -r; nproc; free -g | head -2; cat /root/lzab-mixed.sha256
  for t in A B; do runuser -u zts -- git -C "$E/$t" log -1 --format="$t %H"; done
  cat /root/lzab-res/summary.txt
  grep -E 'SETUP|RESULT|round 1 side' /root/lzab-res/run.log
  grep -E '^Results Summary|^PASS|^FAIL|^SKIP|^KILLED|^Running Time' /root/zts-B.log
  grep -E '\[(FAIL|KILLED)\]' /root/zts-B.log
  journalctl -k --no-pager | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
} > /root/lzab-report.txt 2>&1
cat /root/lzab-report.txt
```

Bring back `/root/lzab-report.txt` and `/root/lzab-res/runs.csv`. On a
hang or a module unload failure: `dmesg | tail -200`, `cat
/proc/spl/kstat/zfs/dbgmsg | tail -200` (if loaded), then reboot.
