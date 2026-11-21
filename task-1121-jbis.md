TASK-1121-JBIS (from the coordinator session "main"): functional, about 2 h 20 builds included, debug builds only (four trees, built at once), ONE planned reboot at the end (step 8, pre-approved below). Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB); not next to a performance run. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-jbis`, `L=/var/log/task-1121-jbis`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

Three pull requests wait for this task. Each is one commit on upstream master `1b1aa0e91b6`, pushed to the fork; the pull requests are not opened yet. As their commit messages put it:

1. **lzjb** (`98c08646883`, `module/zfs/lzjb.c`, new unit test `tests/unit/test_lzjb.c`). `zfs_lzjb_decompress_buf()` ignored its source length and stopped only once it had produced the destination length, so a block whose stream needs more input than its physical size (damaged, or not written by ZFS) made it read past the end of the source buffer. The fix checks every copymap byte, literal and match token against the end of the source and fails with -1, which the callers turn into EIO, and rejects a match at offset 0. `test_lzjb` decompresses compressor output from a buffer that ends at an inaccessible page and checks that truncated and overlong streams and a zero offset are refused.
2. **CCM** (`a66ced4d245`, 12 files). With the 96-bit nonces ZFS uses, AES-CCM encrypts at most 2^24 - 1 bytes in one operation: a 16M block that does not compress reaches `zio_encrypt()` at 2^24 bytes, the cipher refuses it, and the `VERIFY0()` there takes the system down at the next txg sync. The fix refuses `recordsize`/`volblocksize` above 8M on `aes-*-ccm` datasets, stops `dmu_object_set_blocksize()` at 8M for an inherited recordsize, refuses a non-raw receive of `DRR_OBJECT` records above 8M into such a dataset, and makes `zio_encrypt()` fail the write instead of panicking. Its new ZTS test `cli_root/zfs_create/zfs_create_crypt_ccm_blksz` (in common.run) has only been through `ksh -n`: this is its first run.
3. **zio_crypt** (`ee99c44e161`, `module/zfs/zio_crypt.c` only, no test). `zio_crypt_init_uios_zil()` and `zio_crypt_init_uios_dnode()` walk a block to build the uios before the cipher checks its MAC. The fix checks the ZIL record lengths and the dnode slot, block pointer and spill layout in the first pass and fails with ECKSUM, refusing only blocks ZFS cannot have written. The checks run on encryption too, so what this task looks for is that valid encrypted ZIL and dnode blocks all still pass: a false refusal would show as EIO on a read, a failed ZIL replay, or a `VERIFY0` in `zio_encrypt()` on a write.

Each branch runs on its own debug module. Part A (step 4, lzjb): `test_lzjb` 3 times and `make unit` once; a planted lzjb block whose stream needs more input than the block holds, read through the kernel (`lzjb.sh`); `-T compression,zpool_upgrade,zdb` once. Part B (step 5, CCM): the new test 3 times; `-T zfs_create,zfs_set` once; the encrypted and large-block send and receive tests once. Part C (step 6, zio_crypt): an encrypted ZIL replay (`zil.sh`); `-T zfs_load-key` once; the encrypted send/receive, ZIL and import tests once. Part D (step 7, fail-before) uses a tree with master's module and the two new tests: `test_lzjb` against master's `lzjb.c` (user space: SIGSEGV on the inaccessible page, harmless); the CCM test once (it fails cleanly at its first check, before it writes any data); then the CCM panic itself, outside ZTS (`fb.sh`). That panic halts the thread that hits the `VERIFY0` and wedges its pool for good, so the module can no longer be unloaded: hence the reboot of step 8.

Left out on purpose: the planted lzjb read on master's module (its decoder reads past the source buffer in the kernel, and can fault there); any fail-before of the zio_crypt change (it needs crafted blocks whose checksums match, and master's debug build stops on an `ASSERT` while a release build loops or reads past the block); release builds (none of the three texts claims one; CI builds them).

## What is tested

Fork https://github.com/tigerblue77/zfs, fetched by full SHA (branch names in parentheses). Each commit's parent is upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db`. The four trees share one repository: `lz` is the clone, the other three are worktrees of it.

| tree | content | tree object |
|---|---|---|
| lz | `98c08646883f6ad8bf50776919d317074f6e3692` (lzjb-decompress-bound) | `afe3fad81a0b29a6ca602f274d37eaa21a884eab` |
| ccm | `a66ced4d245e3fb7ad20efbffda68258d644d1fd` (aes-ccm-block-limit) | `62dd1b8e86c55c0404df552138327787aee3f7cc` |
| zc | `ee99c44e161dca6a9767c8a232d3b6425267001f` (zio-crypt-layout-checks) | `ce5a88f26dbcd564595c03b64d0c1bfd0cb4d592` |
| fb | master, plus `tests/unit/{test_lzjb.c,Makefile.am,.gitignore}` from lz and the new test, `tests/runfiles/common.run` and `tests/zfs-tests/tests/Makefile.am` from ccm: module/ and include/ are master's | `9b3dfde8e60b654e2d40aa1a690f542b6adf7b66` (`git write-tree`) |

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp; the scripts of step 2 use files under `$D/lzjb`, `$D/zil` and `$D/fb-ccm`. Touch no block device of the VM. Never run `zpool import` yourself (`-a` least of all): the only imports outside ZTS are the ones `lzjb.sh` and `zil.sh` make of their own pool, from their own directory, with `-o cachefile=none`. Never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the tests' own pools and `jbisl` (`lzjb.sh`), `jbisz` (`zil.sh`) and `jbisc` (`fb.sh`, left wedged until the reboot by design), all three with `cachefile=none`.
- Pre-run checks, before every ZTS run and every script of step 2 (`<t>` is the tree of the step): `cat /sys/module/zfs/srcversion` equals `cat $L/sv-<t>`, `timeout 30 sudo -n $D/<t>/zpool list -H -o name` shows no pool, `sudo -n $D/<t>/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. A stale `/var/tmp/tunable-*` file is a stop too: report its `ls -l`. `runs.sh` repeats all these checks before each of its runs and writes a `STOP` line if one fails.
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a series whose $L/runs.txt has not changed for 30 minutes; `lzjb.sh` or `zil.sh` not back after 5 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report. (`fb.sh` has its own rule, step 7.)
- Reboots: this task pre-approves exactly one, the planned reboot of step 8, after the planned panic of step 7; for everything else it replaces the standing approval of 18/11. A hang, a panic or a module that will not unload anywhere else: capture as above, stop, report, and wait for main.
- A ZTS test that does not pass does not stop the task: quote its `Test:` line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:' <its merged file>` (`find $R -path '*/<test>/merged'`, R the run's `dir=`), say whether `tests/test-runner/bin/zts-report.py.in` lists it as known, and go on without rerunning it. A failure on a `/dev/zvol/...` path when step 0 found H=no is the environment (TASK-1117-ER34): say so.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. At the end no zfs or spl module is loaded and no link of `zfs-helpers.sh` made by this task is left.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 30 GiB free on /var/tmp: four trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote them; if hung_task_panic is 1, set it to 0 for this task with `sudo -n sysctl -w kernel.hung_task_panic=0` and say so: the reboot restores it), `cat /proc/sys/kernel/sysrq`, and `systemctl is-enabled claude-rc.service 2>&1` (say what it prints: step 8 relies on your session coming back after the reboot).

Udev: `ls -l /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1`. Expected: none of them exists (no zvol udev rules, so no `/dev/zvol` links, which `zpool_import_errata3` and `errata4` of step 6 need): note H=yes, step 4 installs the in-tree helpers and step 6 removes them. A symlink whose target is gone (`readlink -e` prints nothing) counts as absent (H=yes: `zfs-helpers.sh -i` replaces it). Anything else that exists: H=no, say what it is, and do not run `zfs-helpers.sh` in this task.

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1121-jbis
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/lz && cd $D/lz && git init -q . && git fetch -q --depth 2 $F 98c08646883f6ad8bf50776919d317074f6e3692 a66ced4d245e3fb7ad20efbffda68258d644d1fd ee99c44e161dca6a9767c8a232d3b6425267001f && echo FETCH-OK
```

If the fetch fails, wait 1 minute and run it once more. If it fails again, fetch the branches by name instead, in the same directory (`git fetch -q --depth 2 $F lzjb-decompress-bound aes-ccm-block-limit zio-crypt-layout-checks && echo FETCH-OK`), say so, and go on: the commands below stop on a branch that has moved.

```sh
D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis
cd $D/lz && git checkout -q 98c08646883f6ad8bf50776919d317074f6e3692 &&
git worktree add -q --detach $D/ccm a66ced4d245e3fb7ad20efbffda68258d644d1fd &&
git worktree add -q --detach $D/zc ee99c44e161dca6a9767c8a232d3b6425267001f &&
git worktree add -q --detach $D/fb 1b1aa0e91b694798085a2771c4eef40e93b9b2db &&
cd $D/fb &&
git checkout a66ced4d245e3fb7ad20efbffda68258d644d1fd -- tests/runfiles/common.run tests/zfs-tests/tests/Makefile.am tests/zfs-tests/tests/functional/cli_root/zfs_create/zfs_create_crypt_ccm_blksz.ksh &&
git checkout 98c08646883f6ad8bf50776919d317074f6e3692 -- tests/unit/test_lzjb.c tests/unit/Makefile.am tests/unit/.gitignore &&
echo TREES-OK
for t in lz ccm zc; do echo "$t $(git -C $D/$t rev-parse HEAD) $(git -C $D/$t rev-parse 'HEAD^{tree}') $(git -C $D/$t rev-parse HEAD~1) $(git -C $D/$t status --porcelain | wc -l)"; done > $L/tree.txt
echo "fb $(git -C $D/fb rev-parse HEAD) $(git -C $D/fb write-tree) $(git -C $D/fb status --porcelain | wc -l)" >> $L/tree.txt
printf '%s\n' "lz 98c08646883f6ad8bf50776919d317074f6e3692 afe3fad81a0b29a6ca602f274d37eaa21a884eab 1b1aa0e91b694798085a2771c4eef40e93b9b2db 0" "ccm a66ced4d245e3fb7ad20efbffda68258d644d1fd 62dd1b8e86c55c0404df552138327787aee3f7cc 1b1aa0e91b694798085a2771c4eef40e93b9b2db 0" "zc ee99c44e161dca6a9767c8a232d3b6425267001f ce5a88f26dbcd564595c03b64d0c1bfd0cb4d592 1b1aa0e91b694798085a2771c4eef40e93b9b2db 0" "fb 1b1aa0e91b694798085a2771c4eef40e93b9b2db 9b3dfde8e60b654e2d40aa1a690f542b6adf7b66 6" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
```

```sh
D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis; T=tests/zfs-tests/tests/functional/cli_root/zfs_create/zfs_create_crypt_ccm_blksz.ksh
printf '%s\n' \
"461c782acf5d905a8de965ca623b903d56a3707fddc45b014d87b9b0851447f7  $D/lz/module/zfs/lzjb.c" \
"f4e368950b93fed59b202238c1b23999e88f0f228a916d0e28fdaa58c0c387fe  $D/lz/tests/unit/test_lzjb.c" \
"ec7171c7ce6bb217735bb00305d7ab13cffafd36f8a9cbf0c87760aab2c2b6bb  $D/lz/tests/unit/Makefile.am" \
"7068ea85168e52c21ad400db6625fdecd6bc8f9ea2bf474bee57c77b648fcac2  $D/ccm/module/zfs/zio.c" \
"ac8c35b965db2c3906c1785555d4b6b9d3ef0dc0aa5ed62931d3d7b5a157c0a0  $D/ccm/module/zfs/zio_crypt.c" \
"fa6eda2098baee69e28527625b90486d6963a3915201f1da5c183c58ee5ad274  $D/ccm/module/zfs/dmu.c" \
"28c1fd454da3ad95273e87e935451f50fd5a5e8916ca16db0a1e2c6170bea3eb  $D/ccm/module/zfs/dmu_recv.c" \
"6442b9e349320600e80040a246a5e8fafd85d52cd5d4651337a0f142162d7382  $D/ccm/module/zfs/dsl_crypt.c" \
"a0cd9ece024f304d05185f7a1cb3d0894681a9804a8e4f5b7e0ac4fa3d2ffb83  $D/ccm/module/zfs/zfs_ioctl.c" \
"91a3a2837eb274d5abae0067d14179b85d1f52fd670f2e1159bfc1268a8f0035  $D/ccm/$T" \
"f5a4ed76ee574833c7fe6680ebc108e4cc12bad85b16d40df3881d9adcaab426  $D/ccm/tests/runfiles/common.run" \
"3ea64af37ef200a28e835699431a6b6dd368c26967c59e5c2eef12bb463e6434  $D/zc/module/zfs/zio_crypt.c" \
"58efafd8f2b1bbc29881ecdd564ad93aa319d02ca7d8d30261bd23f0d5a15b50  $D/fb/module/zfs/lzjb.c" \
"be31510828342e582cdc11c93ed51d175c177e3d10b7ffe7d285ad1d5876e32d  $D/fb/module/zfs/zio.c" \
"eb0fde335cc6598fe96209d471d9d7ad02acfe5f0e72795bf875e76cfb2c5505  $D/fb/module/zfs/zio_crypt.c" \
"f4e368950b93fed59b202238c1b23999e88f0f228a916d0e28fdaa58c0c387fe  $D/fb/tests/unit/test_lzjb.c" \
"91a3a2837eb274d5abae0067d14179b85d1f52fd670f2e1159bfc1268a8f0035  $D/fb/$T" \
> $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
for t in lz ccm zc; do echo "$t: $(git -C $D/$t log -1 --format=%s) |$(git -C $D/$t diff --shortstat HEAD~1 HEAD)"; done
git -C $D/fb diff --cached --name-status HEAD; ls -l $D/ccm/$T $D/fb/$T | cut -c1-10
```

Expected: TREES-OK, TREE-OK, SHA-OK, then

```
lz: lzjb: bound decompression by the source length | 4 files changed, 304 insertions(+), 3 deletions(-)
ccm: Keep AES-CCM datasets to blocks the cipher can encrypt | 12 files changed, 231 insertions(+), 6 deletions(-)
zc: zio_crypt: check ZIL and dnode layouts before using them | 1 file changed, 38 insertions(+), 7 deletions(-)
```

six name-status lines (`M` `tests/runfiles/common.run`, `M` `tests/unit/.gitignore`, `M` `tests/unit/Makefile.am`, `A` `tests/unit/test_lzjb.c`, `M` `tests/zfs-tests/tests/Makefile.am`, `A` `.../zfs_create_crypt_ccm_blksz.ksh`, tab-separated), and two `-rwxr-xr-x`. Any other difference (a branch may have moved): stop and report as is. Do not edit any file of the trees.

## STEP 2: the kit

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to $L/runs.sh, $L/lzjb.sh, $L/zil.sh, $L/fb.sh and $L/jbis.run in that order. Then:

```sh
L=/var/log/task-1121-jbis
printf '%s\n' "87f774078174a8cf491ac36bfd815fbd954e43d429958d6e9284661e4230bd71  $L/runs.sh" "2387646e880cc269d60e58121304982e1508514293de6952383e3d1fefd05e02  $L/lzjb.sh" "751fe4db08dc395832893fe51012ab78eb4263219b2a9353643bad5a16bb52ef  $L/zil.sh" "eb6abb9f68f1ab9256e399f01fb5374205da7b0be7904d926ec4d5e9763ee9d4  $L/fb.sh" "ab217c794458ed1990aba47f497fe0dbe06841c69caf55ad2014014318ace11d  $L/jbis.run" | sha256sum -c && sh -n $L/runs.sh && sh -n $L/lzjb.sh && sh -n $L/zil.sh && sh -n $L/fb.sh && echo KIT-OK
```

Expected five `OK` lines and KIT-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tree> <tag> <runs> <selection>` runs as the login user, from `$D/<tree>`. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks (the srcversion one against `$L/sv-<tree>`); each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1121-JBIS ZTS runner, run as the login user:
# runs.sh <tree> <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1121-jbis
L=/var/log/task-1121-jbis
tree=$1
tag=$2
runs=$3
shift 3
out=$L/runs.txt
T=$D/$tree
cd $T || exit 1
echo "START $tag $tree $(date -u +%FT%TZ) $*" >> $out
n=1
while [ $n -le $runs ]; do
    sv=$(cat /sys/module/zfs/srcversion 2>&1)
    pools=$(timeout 30 sudo -n $T/zpool list -H -o name 2>&1)
    inj=$(sudo -n $T/zinject 2>&1 | head -n 1)
    loops=$(sudo -n losetup -a 2>&1)
    left=$(ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* \
        2>/dev/null)
    if [ "$sv" != "$(cat $L/sv-$tree)" ] || [ -n "$pools" ] ||
        [ "$inj" != "No handlers registered." ] || [ -n "$loops" ] ||
        [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: srcversion=[$sv] pools=[$pools] zinject=[$inj] loops=[$loops] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    sudo -n -u zts ./scripts/zfs-tests.sh -vKO "$@" \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc dir=$R" >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

`lzjb.sh` runs as root once, in step 4, with the lz module loaded. It creates pool `jbisl` (one 256 MiB file, `ashift=9`, `cachefile=none`) and `jbisl/fs` with `compression=lzjb`, `checksum=off`, `recordsize=128k`, writes a 128 KiB file of a repeated text line, exports the pool and reads the file's one L0 block pointer with `zdb -e`. It then overwrites exactly the block's physical size on the vdev file (DVA offset + 4 MiB of labels and boot area) with copymap bytes 0 each followed by 8 literals, a stream that produces at most 8 bytes per 9 and so needs more input than the block holds; block pointer and metadata are left as they are, and `checksum=off` lets the read reach the decompressor. It imports the pool again, reads the file twice, and reports the reads, the ARC's `zfs_dbgmsg` line for a failed decompression, the pool's errors and ereports, and the kernel log since its start.

```sh
#!/bin/sh
# TASK-1121-JBIS, run as root with the lz tree's module loaded: lzjb.sh
# Writes a 128K file with compression=lzjb and checksum=off, replaces its
# block on the exported file vdev with a stream of literals that needs more
# input than the block's physical size, imports the pool again and reads
# the file twice.  The block pointer and the metadata are left as they are.
D=/var/tmp/task-1121-jbis
L=/var/log/task-1121-jbis
Z=$D/lz
v=$D/lzjb
o=$L/lzjb.txt
p=jbisl
mkdir $v $v/dev || exit 1
n0=$(dmesg | wc -l)
truncate -s 256M $v/dev/vdev || exit 1
$Z/zpool create -o cachefile=none -o ashift=9 -O mountpoint=$v/mnt $p \
    $v/dev/vdev || exit 1
$Z/zfs create -o compression=lzjb -o checksum=off -o recordsize=128k \
    $p/fs || exit 1
yes 'TASK-1121-JBIS lzjb planted block' | head -c 131072 > $v/data
cp $v/data $v/mnt/fs/f && $Z/zpool sync $p && cmp $v/data $v/mnt/fs/f ||
    exit 1
obj=$(stat -c %i $v/mnt/fs/f)
$Z/zpool export $p || exit 1
$Z/zdb -e -p $v/dev -ddddd $p/fs $obj > $v/zdb.txt 2>&1
nl0=$(awk '$2 == "L0"' $v/zdb.txt | wc -l)
set -- $(awk '$2 == "L0" { print $3, $4 }' $v/zdb.txt)
echo "object $obj: $nl0 L0 block pointer(s): $*" > $o
case "$nl0 $1 $2" in
"1 0:"*":"*[0-9a-f]" 20000L/"*P) ;;
*) echo "STOP: not one plain L0 block on vdev 0" >> $o; cat $o; exit 1 ;;
esac
off=$(echo $1 | cut -d: -f2)
asize=$(echo $1 | cut -d: -f3)
psize=${2#20000L/}
psize=${psize%P}
P=$((0x$psize))
S=$(((0x$off + 0x400000) / 512))
if [ $P -ge 131072 ] || [ $P -ne $((0x$asize)) ] || [ $((P % 512)) -ne 0 ]
then
    echo "STOP: psize $P, asize $((0x$asize))" >> $o
    cat $o
    exit 1
fi
dd if=$v/dev/vdev of=$v/orig bs=512 skip=$S count=$((P / 512)) 2>/dev/null
echo "the block as written (first 32 bytes):" >> $o
od -A d -c -N 32 $v/orig >> $o
i=0
while [ $i -lt $P ]; do
    printf '\000JBISLZJB'
    i=$((i + 9))
done > $v/plant.full
head -c $P $v/plant.full > $v/plant
dd if=$v/plant of=$v/dev/vdev bs=512 seek=$S count=$((P / 512)) \
    conv=notrunc 2>/dev/null || exit 1
dd if=$v/dev/vdev bs=512 skip=$S count=$((P / 512)) 2>/dev/null |
    cmp - $v/plant || exit 1
echo "planted at sector $S: $P bytes, a copymap byte 0 and 8 literals," \
    "repeated: at most $((P / 9 * 8 + 8)) bytes of output, lsize 131072" >> $o
$Z/zpool import -o cachefile=none -d $v/dev $p || exit 1
for r in 1 2; do
    dd if=$v/mnt/fs/f of=/dev/null bs=128k 2> $v/read.err
    rc=$?
    echo "read $r: rc=$rc [$(grep -v records $v/read.err | head -n 1)]" >> $o
done
echo "zfs_dbgmsg: $(grep -c 'compress 3, psize' \
    /proc/spl/kstat/zfs/dbgmsg) line(s); last:" >> $o
grep 'compress 3, psize' /proc/spl/kstat/zfs/dbgmsg | tail -n 1 >> $o
$Z/zpool status -v $p | sed -n '/^errors:/,$p' >> $o
$Z/zpool events | grep ereport | tail -n 3 >> $o
$Z/zpool destroy $p || echo "destroy failed" >> $o
dmesg | tail -n +$((n0 + 1)) > $L/lzjb-dmesg.txt
pat='VERIFY|ASSERT|PANIC|BUG|Oops|general protection|KASAN|segfault'
echo "kernel log: $(wc -l < $L/lzjb-dmesg.txt) new line(s)," \
    "$(grep -cE "$pat" $L/lzjb-dmesg.txt) matching $pat" >> $o
cat $o
```

`zil.sh` runs as root once, in step 6, with the zc module loaded. It creates pool `jbisz` (one 512 MiB file, `cachefile=none`, no log device) and an `aes-256-gcm` file system with a raw key in a file, writes one synced byte so that a ZIL header exists, freezes the pool (`zpool freeze`: from then on only the ZIL keeps changes), makes create, rename, symlink, link, mkdir, rmdir, write (small ones, copied into the log; 128K ones, logged as indirect), truncate, setattr and remove records, records names, types, modes, link counts, checksums and one mtime, unmounts and exports, counts the ZIL records with `zdb -e -iv`, then imports with `-f -l` (the key loads from its file, the mount replays the ZIL, which decrypts each log block) and compares.

```sh
#!/bin/sh
# TASK-1121-JBIS, run as root with the zc tree's module loaded: zil.sh
# Freezes a pool, makes an encrypted file system log create, rename,
# symlink, link, mkdir, rmdir, write, truncate, setattr and remove records,
# then imports the pool again, which decrypts and replays that ZIL, and
# compares the files with what was written.
D=/var/tmp/task-1121-jbis
L=/var/log/task-1121-jbis
Z=$D/zc
v=$D/zil
o=$L/zil.txt
p=jbisz
m=$v/mnt/fs
mkdir $v $v/dev || exit 1
n0=$(dmesg | wc -l)
truncate -s 512M $v/dev/vdev || exit 1
head -c 32 /dev/urandom > $v/key || exit 1
$Z/zpool create -o cachefile=none -O mountpoint=$v/mnt $p $v/dev/vdev ||
    exit 1
$Z/zfs create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation=file://$v/key $p/fs || exit 1
# a ZIL header must exist before the freeze for the records to be kept
dd if=/dev/zero of=$m/sync conv=fdatasync,fsync bs=1 count=1 2>/dev/null &&
    $Z/zpool freeze $p || exit 1
touch $m/a && mv $m/a $m/b && ln -s b $m/c && ln $m/b $m/d &&
    mkdir $m/e $m/f && rmdir $m/f &&
    dd if=/dev/urandom of=$m/small bs=1k count=3 oflag=sync 2>/dev/null &&
    dd if=/dev/urandom of=$m/large bs=128k count=16 oflag=sync 2>/dev/null &&
    dd if=/dev/urandom of=$m/e/trunc bs=8k count=4 oflag=sync 2>/dev/null &&
    truncate -s 5000 $m/e/trunc && chmod 640 $m/small &&
    touch -m -t 202001020304 $m/small && rm $m/d || exit 1
state() {
    (cd $m && find . -printf '%p %y %m %n %l\n' | sort &&
        find . -type f -exec sha256sum {} + | sort -k 2 &&
        stat -c '%n %Y' small)
}
state > $v/before
$Z/zfs unmount $p/fs && $Z/zpool export $p || exit 1
$Z/zdb -e -p $v/dev -iv $p/fs > $v/zdb-il.txt 2>&1
echo "ZIL records before the import:" \
    "$(grep ' len ' $v/zdb-il.txt | grep -oE 'TX_[A-Z_]+' | sort | uniq -c |
    tr -s ' ' | tr '\n' ';')" > $o
$Z/zpool import -f -l -o cachefile=none -d $v/dev $p || exit 1
state > $v/after
if cmp -s $v/before $v/after; then
    echo "REPLAY-OK: $(wc -l < $v/after) lines of names, types, modes," \
        "links, sums and mtime identical" >> $o
else
    echo "REPLAY-DIFF:" >> $o
    diff $v/before $v/after >> $o
fi
echo "mounted: $($Z/zfs get -H -o value mounted $p/fs)," \
    "authentication ereports: $($Z/zpool events | grep -c authentication)" >> $o
$Z/zpool status -v $p | sed -n '/^errors:/,$p' >> $o
$Z/zpool destroy $p || echo "destroy failed" >> $o
dmesg | tail -n +$((n0 + 1)) > $L/zil-dmesg.txt
pat='VERIFY|ASSERT|PANIC|BUG|Oops|general protection|KASAN|segfault'
echo "kernel log: $(wc -l < $L/zil-dmesg.txt) new line(s)," \
    "$(grep -cE "$pat" $L/zil-dmesg.txt) matching $pat" >> $o
cat $o
```

`fb.sh` runs as root once, in step 7 only, with the fb module (master's) loaded. It creates pool `jbisc` (one 512 MiB file, `cachefile=none`), `jbisc/fs` with `compression=off` and `recordsize=16M`, and below it `jbisc/fs/ccm` with `encryption=aes-256-ccm`, which inherits `recordsize=16M`; writes 16 MiB of random data to one file, and starts `zpool sync` in the background. It waits up to 60 s for a new `PANIC at` line in the kernel log, saves the kernel log since its start to $L/fb-ccm-dmesg.txt, and prints (and writes to $L/fb-ccm.txt) the state and kernel stack of `zpool sync`, the state of every `txg_sync` thread, the VERIFY, PANIC and `Comm:` lines and the first call trace. It never waits on the command it started.

```sh
#!/bin/sh
# TASK-1121-JBIS fail-before on master, run as root: fb.sh
# An aes-256-ccm file system inherits recordsize=16M and gets 16M of random
# data, then the pool is synced in the background.  The VERIFY0 expected in
# zio_encrypt() halts the thread that hits it and the pool stays wedged
# until the reboot of step 8, so nothing here waits on it.
D=/var/tmp/task-1121-jbis
L=/var/log/task-1121-jbis
Z=$D/fb
v=$D/fb-ccm
o=$L/fb-ccm.txt
p=jbisc
mkdir $v $v/dev || exit 1
n0=$(dmesg | wc -l)
truncate -s 512M $v/dev/vdev || exit 1
head -c 32 /dev/urandom > $v/key || exit 1
$Z/zpool create -o cachefile=none -O mountpoint=$v/mnt $p $v/dev/vdev ||
    exit 1
$Z/zfs create -o compression=off -o recordsize=16M $p/fs || exit 1
$Z/zfs create -o encryption=aes-256-ccm -o keyformat=raw \
    -o keylocation=file://$v/key $p/fs/ccm || exit 1
echo "recordsize of $p/fs/ccm:" \
    "$($Z/zfs get -H -o value,source recordsize $p/fs/ccm | tr '\t' ' ')," \
    "encryption: $($Z/zfs get -H -o value encryption $p/fs/ccm)" > $o
dd if=/dev/urandom of=$v/mnt/fs/ccm/file bs=1M count=16 2>/dev/null ||
    exit 1
$Z/zpool sync $p > $v/cmd.out 2>&1 &
pid=$!
i=0
while [ $i -lt 60 ]; do
    dmesg | tail -n +$((n0 + 1)) | grep -q 'PANIC at' && break
    sleep 1
    i=$((i + 1))
done
sleep 5
dmesg | tail -n +$((n0 + 1)) > $L/fb-ccm-dmesg.txt
echo "waited ${i}s; zpool sync pid $pid:" \
    "$(ps -o stat=,wchan:32= -p $pid 2>&1 | tr -s ' ')," \
    "output [$(cat $v/cmd.out)]" >> $o
cat /proc/$pid/stack >> $o 2>&1
for t in $(pgrep -x txg_sync); do
    echo "txg_sync $t: $(ps -o stat=,wchan:32= -p $t | tr -s ' ')"
done >> $o
grep -E 'VERIFY|PANIC|Comm:' $L/fb-ccm-dmesg.txt >> $o
grep -m 1 -A 12 'Call Trace:' $L/fb-ccm-dmesg.txt >> $o
cat $o
```

`jbis.run` is a runfile for `zfs-tests.sh -r`: common.run's `[DEFAULT]`, and tests of four directories of common.run and linux.run, unchanged, under three tags of their own. `jbis_crypt` (23 tests: 17 of `rsend`, 6 of `cli_root/zfs_receive`) sends and receives encrypted and large-block data, raw and not; `send_raw_large_blocks` and `zfs_receive_large_block_corrective` receive `zfs send -L` streams of 1M blocks into `aes-256-ccm` file systems (allowed: 1M is below the 8M limit), `send_raw_spill_block`, `send_raw_ashift` and `send_encrypted_files` write encrypted files with SA xattrs and spill blocks, and `send_realloc_encrypted_files` uses `dnodesize` `auto` and `4k`. `jbis_zil` (2 tests) replays encrypted `TX_CLONE_RANGE` records and clones between encrypted datasets. `jbis_import` (5 tests) imports encrypted pools, among them the `cryptv0` and `missing_ivset` images of `zpool_import_errata3` and `errata4`, written by older versions.

```
# TASK-1121-JBIS: tests of common.run and linux.run, selected by tag.
# jbis_crypt: encrypted and large-block send and receive.
# jbis_zil: encrypted ZIL replay and cloning.
# jbis_import: encrypted pool imports, older pool images included.

[DEFAULT]
pre = setup
quiet = False
pre_user = root
user = root
timeout = 600
post_user = root
post = cleanup
failsafe_user = root
failsafe = callbacks/zfs_failsafe
tags = ['functional']

[tests/functional/rsend]
tests = ['send_encrypted_files', 'send_encrypted_freeobjects',
    'send_encrypted_hierarchy', 'send_encrypted_holds',
    'send_encrypted_incremental', 'send_encrypted_props',
    'send_encrypted_truncated_files', 'send_realloc_encrypted_files',
    'send_raw_spill_block', 'send_raw_ashift', 'send_raw_large_blocks',
    'send_raw_inherited_key', 'send_mixed_raw', 'send_leak_keymaps',
    'send_large_blocks_initial', 'send_large_blocks_incremental',
    'send-cpL_varied_recsize']
tags = ['functional', 'jbis_crypt']

[tests/functional/cli_root/zfs_receive]
tests = ['zfs_receive_from_encrypted', 'zfs_receive_to_encrypted',
    'zfs_receive_raw', 'zfs_receive_raw_incremental',
    'zfs_receive_-wR-encrypted-mix', 'zfs_receive_large_block_corrective']
tags = ['functional', 'jbis_crypt']

[tests/functional/block_cloning]
tests = ['block_cloning_replay_encrypted', 'block_cloning_cross_enc_dataset']
tags = ['functional', 'jbis_zil']

[tests/functional/cli_root/zpool_import]
tests = ['zpool_import_encrypted', 'zpool_import_encrypted_load',
    'zpool_import_encrypted_zplprops', 'zpool_import_errata3',
    'zpool_import_errata4']
tags = ['functional', 'jbis_import']
```

## STEP 3: builds (the four at once, about 35 minutes)

```sh
D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis
for t in lz ccm zc fb; do setsid nohup sh -c "cd $D/$t && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-$t.log 2>&1 < /dev/null & done
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Then:

```sh
D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis
for t in lz ccm zc fb; do /usr/sbin/modinfo -F srcversion $D/$t/module/zfs.ko > $L/sv-$t; done; grep . $L/sv-*
ls -l $D/lz/tests/unit/test_lzjb $D/fb/tests/unit/test_lzjb | cut -c1-10
```

Expected four different srcversions, and two `-rwx` lines. fb's module sources are master's: on VM 106, TASK-1120-HRC's build of master's module had srcversion `371A65E8D86145973554A8B`; say whether fb's is the same (not a stop either way).

## STEP 4: part A, lz (lzjb)

If H=yes: `cd /var/tmp/task-1121-jbis/lz && sudo -n ./scripts/zfs-helpers.sh -i`, then quote `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id` (symlinks into `$D/lz`). They stay through steps 5 and 6.

Load: `sudo -n /var/tmp/task-1121-jbis/lz/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `$L/sv-lz`), the last `ZFS: Loaded module` line ("(DEBUG mode)"), `cat /sys/module/zfs/parameters/zfs_compressed_arc_enabled` (1, the default) and `cat /sys/module/spl/parameters/spl_panic_halt` (0). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks (tree lz), then:

1. The planted block: `sudo -n sh /var/log/task-1121-jbis/lzjb.sh`. It returns within a minute. Expected output:
   - `object <n>: 1 L0 block pointer(s): 0:<offset>:<asize> 20000L/<psize>P`, psize equal to asize, a few KiB (a user-space model of the compressor gives `1a00`, 6656 bytes);
   - the first 32 bytes of the block as written: a `\0`, then `T A S K - 1 1 2`, a `\0`, then `1 - J B I S   l`, a `\0`, ... (copymap bytes and the literals of the file's first line: the lzjb stream of the file);
   - `planted at sector <s>: <P> bytes, ...`;
   - `read 1: rc=1 [dd: error reading '/var/tmp/task-1121-jbis/lzjb/mnt/fs/f': Input/output error]`, and the same for read 2;
   - `zfs_dbgmsg: <k> line(s); last:` with k at least 1, and a line ending `compress 3, psize <P>, lsize 131072` (3 is LZJB: the ARC's message for a failed decompression);
   - the `errors:` section of `zpool status -v` and up to three ereport lines: quote them as they are;
   - `kernel log: <n> new line(s), 0 matching ...`.

   Quote the whole output. A read with rc=0, no dbgmsg line, a matching kernel line, or a `STOP:` line: quote everything, stop and report (skip the rest of step 4).
2. The unit tests (user space, as the login user):

   ```sh
   D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis
   cd $D/lz && for n in 1 2 3; do timeout 600 make unit T=lzjb > $L/unit-lz-$n.log 2>&1; echo "unit-lz-$n rc=$?"; done; timeout 1800 make unit > $L/unit-lz-all.log 2>&1; echo "unit-lz-all rc=$?"
   grep -hE '^lzjb\.|tests successful' $L/unit-lz-[123].log; grep -E 'tests successful' $L/unit-lz-all.log
   ```

   Expected four `rc=0`; in each of the three runs the four lines `lzjb.roundtrip`, `lzjb.truncated`, `lzjb.overlong`, `lzjb.offset_zero` with `[ OK    ]` and `4 of 4 (100%) tests successful` (each run draws new random data); `make unit` prints twelve summary lines, each `N of N (100%)`. Quote them. A failure: quote the last 30 lines of its log and go on.
3. The groups once (tag g44), after the pre-run checks:

   ```sh
   L=/var/log/task-1121-jbis
   setsid nohup sh $L/runs.sh lz g44 1 -T compression,zpool_upgrade,zdb > $L/runs-A.nohup 2>&1 < /dev/null &
   ```

   Poll `tail -n 3 $L/runs.txt` until `DONE g44` or a `STOP` line (about 15 minutes). Expected all PASS: 46 tests (8 of `compression`, 10 of `cli_root/zpool_upgrade`, which imports pool images of versions 1 to 15 written by older versions, and 28 of `cli_root/zdb`) with the setup and cleanup of the first two. Quote the Results Summary at the end of $L/zts-g44-1.log and the lines of `compress_004_pos`, `zdb_decompress` and `zdb_display_block` (they use `compression=lzjb`).

Then the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than|segfault|KASAN|WARNING` quoted with 10 lines before it (none expected). Unload with `sudo -n /var/tmp/task-1121-jbis/lz/scripts/zfs.sh -u` (quote rc; `lsmod | grep -E '^(zfs|spl) '` empty) and save `sudo -n dmesg > /var/log/task-1121-jbis/dmesg-A.txt`.

## STEP 5: part B, ccm (CCM)

Load `sudo -n /var/tmp/task-1121-jbis/ccm/scripts/zfs.sh`; srcversion equal to `$L/sv-ccm`, "(DEBUG mode)"; note N1. Run the pre-run checks (tree ccm), then the new test 3 times (tag n45), the two groups once (g45) and the send/receive tests once (c45), one series after the other:

```sh
L=/var/log/task-1121-jbis
setsid nohup sh -c "sh $L/runs.sh ccm n45 3 -t zfs_create_crypt_ccm_blksz && sh $L/runs.sh ccm g45 1 -T zfs_create,zfs_set && sh $L/runs.sh ccm c45 1 -r $L/jbis.run -T jbis_crypt" > $L/runs-B.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` until `DONE c45` or a `STOP` line (about 30 minutes). Expected:

- n45: in each run the zfs_create setup, `zfs_create_crypt_ccm_blksz` and the cleanup PASS, the test in well under a minute. For the first run, with R its `dir=`, quote `grep -nE 'zfs (create|set|inherit|send)|test [0-9]+M? = |cannot receive|block size' $R/log | cut -c1-220`. Expected, in order: the `aes-256-ccm` creates with `recordsize=16M` and with `volblocksize=16M` `exited 1`; the `aes-256-gcm` create with `recordsize=16M` passing; the `aes-256-ccm` create with `recordsize=8M` passing; `zfs set recordsize=16M testpool/testfs1/ccm exited 1`; `test 8388608 = 8388608`; `zfs inherit recordsize`; `test 16777216 = 16777216`; `test 8M = 8M` (the file that inherited 16M got 8M blocks); the `zfs send -L ... | zfs receive testpool/testfs1/ccm/recv` line `exited 1` and its error message; the `zfs send` without `-L` passing.
- g45: all PASS, 51 tests (22 of `cli_root/zfs_create`, the new one among them, and 29 of `cli_root/zfs_set`) with their setups and cleanups.
- c45: all PASS, the 23 tests of `jbis_crypt` with the setups and cleanups of `rsend` and `cli_root/zfs_receive`. Quote the lines of `send_encrypted_props`, `send_raw_large_blocks` and `zfs_receive_large_block_corrective` (the last two receive 1M blocks into `aes-256-ccm`).

Then quote:

```sh
L=/var/log/task-1121-jbis
grep -E '^(START|DONE|STOP) .*(n45|g45|c45)' $L/runs.txt; cat $L/runs-B.nohup
grep -E '^(n45|g45|c45)-[0-9]+ rc=' $L/runs.txt
for t in n45 g45 c45; do sed -n "/^START $t /,/^DONE $t /p" $L/runs.txt | grep -cE '\[PASS\]$'; done
sed -n '/^START n45 /,/^DONE c45 /p' $L/runs.txt | grep -E '\[(FAIL|KILLED|SKIP)\]$'
```

Expected no STOP, an empty nohup file, every rc=0, the PASS counts 9, 55 and 27 (setups and cleanups included), and no FAIL, KILLED or SKIP line. Quote every `Test:` line of tag n45, and the Results Summary of $L/zts-g45-1.log and $L/zts-c45-1.log.

After them: the pre-run checks once more, the dmesg check since N1 as in step 4 (none expected), unload (`sudo -n /var/tmp/task-1121-jbis/ccm/scripts/zfs.sh -u`, quote rc), `sudo -n dmesg > /var/log/task-1121-jbis/dmesg-B.txt`.

## STEP 6: part C, zc (zio_crypt)

Load `sudo -n /var/tmp/task-1121-jbis/zc/scripts/zfs.sh`; srcversion equal to `$L/sv-zc`, "(DEBUG mode)"; note N2. Run the pre-run checks (tree zc), then:

1. The encrypted ZIL replay: `sudo -n sh /var/log/task-1121-jbis/zil.sh`. It returns within a minute. Expected output:
   - `ZIL records before the import:` and counts per type: TX_CREATE, TX_RENAME, TX_SYMLINK, TX_LINK, TX_MKDIR, TX_RMDIR, TX_WRITE (about 23), TX_TRUNCATE, TX_SETATTR, TX_REMOVE (the counts are what they are: quote them);
   - `REPLAY-OK: <n> lines of names, types, modes, links, sums and mtime identical`;
   - `mounted: yes, authentication ereports: 0`;
   - `errors: No known data errors`;
   - `kernel log: <n> new line(s), 0 matching ...`.

   Quote the whole output. A `REPLAY-DIFF`, an ereport count above 0, or a matching kernel line: quote everything, stop and report. An empty record list alone: quote `sudo -n head -n 40 /var/tmp/task-1121-jbis/zil/zdb-il.txt` and go on.
2. The groups (tags g46, c46), after the pre-run checks:

   ```sh
   L=/var/log/task-1121-jbis
   setsid nohup sh -c "sh $L/runs.sh zc g46 1 -T zfs_load-key && sh $L/runs.sh zc c46 1 -r $L/jbis.run -T jbis_crypt,jbis_zil,jbis_import" > $L/runs-C.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE c46` or a `STOP` line (about 25 minutes). Expected all PASS: the 7 tests of `cli_root/zfs_load-key`; the 30 tests of `jbis.run` (23 jbis_crypt, 2 jbis_zil, 5 jbis_import); setups and cleanups. Quote with the same commands as in step 5 (tags g46 and c46; PASS counts 9 and 38), the Results Summaries, and the lines of `block_cloning_replay_encrypted`, `zpool_import_errata3`, `zpool_import_errata4`, `send_raw_spill_block` and `send_encrypted_files`.

After them: the pre-run checks once more, the dmesg check since N2 as in step 4 (none expected), unload (`sudo -n /var/tmp/task-1121-jbis/zc/scripts/zfs.sh -u`, quote rc), `sudo -n dmesg > /var/log/task-1121-jbis/dmesg-C.txt`. If H=yes: `cd /var/tmp/task-1121-jbis/lz && sudo -n ./scripts/zfs-helpers.sh -r`, then `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id /usr/sbin/mount.zfs 2>&1` (none left).

## STEP 7: part D, fb (fail-before: master's module with the new tests)

1. `test_lzjb` against master's `lzjb.c`, in user space, no module loaded:

   ```sh
   D=/var/tmp/task-1121-jbis; L=/var/log/task-1121-jbis
   N=$(sudo -n dmesg | wc -l); cd $D/fb && timeout 600 make unit T=lzjb > $L/unit-fb.log 2>&1; echo "unit-fb rc=$?"
   grep -nE '^lzjb\.|rror|signal|ssert|tests successful' $L/unit-fb.log; sudo -n dmesg | tail -n +$((N+1)) | grep -E 'segfault|test_lzjb'
   ```

   Expected a non-zero rc; `lzjb.roundtrip` `[ OK    ]` (valid streams decode the same with the old decoder); `lzjb.truncated` and `lzjb.overlong` `[ ERROR ]` with `child killed by signal 11 (Segmentation fault)` (the old decoder reads into the inaccessible page after the source); `lzjb.offset_zero` `[ FAIL ]` with an assertion that it got 0 where -1 was expected; `1 of 4 (25%) tests successful`; and in the kernel log up to two `test_lzjb[...]: segfault at ...` lines (user space). Quote it all. Anything else: quote it and go on (user space: it does not stop the task).
2. Load `sudo -n /var/tmp/task-1121-jbis/fb/scripts/zfs.sh`; srcversion equal to `$L/sv-fb`, "(DEBUG mode)"; `cat /sys/module/spl/parameters/spl_panic_halt` must print 0 (1 would turn the planned VERIFY into a kernel panic of the VM: then do 7.3, unload, skip 7.4 and step 8, and do step 9 without its reboot checks); note N3. Run the pre-run checks (tree fb).
3. The CCM test once on master's module (tag m45), expected to FAIL cleanly:

   ```sh
   L=/var/log/task-1121-jbis
   setsid nohup sh $L/runs.sh fb m45 1 -t zfs_create_crypt_ccm_blksz > $L/runs-m45.nohup 2>&1 < /dev/null &
   ```

   Poll until `DONE m45` or a `STOP` line. Expected rc other than 0, `zfs_create_crypt_ccm_blksz` `[FAIL]` in a few seconds, setup and cleanup PASS. With R its `dir=`, quote `grep -nE 'unexpectedly|ERROR' $R/log | cut -c1-220 | head -n 5`: expected one `ERROR: zfs create -o encryption=aes-256-ccm -o keyformat=raw -o keylocation=file:///var/tmp/keyfile.<pid> -o recordsize=16M testpool/testfs1/ccm unexpectedly exited 0`, the test's first check (master accepts the property; the test stops there and writes no data). A PASS, a failure elsewhere, or a hang: quote the whole $R/log, say so plainly, and stop there (do not run `fb.sh`; unload; report and wait for main).
4. The pre-run checks once more and the dmesg check since N3 (none expected). Write $L/RESUME.md (what is done; next: step 8, then step 9 and the report of TASK-1121-JBIS), arrange your resumption after a reboot the way TASK-1120-HRC (claude-rc.service) or DBM3 (a scheduled task of your own session) did, and say which. Then the panic:

   ```sh
   sudo -n sh /var/log/task-1121-jbis/fb.sh
   ```

   It returns within about 70 s. Expected: the first line `recordsize of jbisc/fs/ccm: 16M inherited from jbisc/fs, encryption: aes-256-ccm`; then `waited <n>s` with n below 60 (the txg sync starts within about 5 s); `zpool sync` in state `D` with an empty output, its kernel stack in a `txg_wait_synced` frame; one `txg_sync` thread in `D`; the lines `VERIFY0(spa_do_crypt_abd(B_TRUE, spa, &zio->io_bookmark, BP_GET_TYPE(bp), BP_GET_DEDUP(bp), BP_SHOULD_BYTESWAP(bp), salt, iv, mac, psize, zio->io_abd, eabd, &no_crypt)) failed (5)` (another error number: quote it) and `PANIC at zio.c:5684:zio_encrypt()` (5685 or 5686 is fine too); a `Comm:` line naming the thread that hit it (expected a write issue taskq thread, `z_wr_iss`, since the encrypt stage runs after the write is handed to that taskq: say which it is); and the first frames of its call trace (`spl_panic`, `zio_encrypt`, `zio_execute`, taskq frames). Quote the whole output.

   If `fb.sh` prints `waited 60s` with no PANIC line, or anything else than predicted, quote everything, then go on to step 8 all the same (the pool is in an unknown state and the reboot is the way out); say it plainly in the report. If `fb.sh` itself does not return within 3 minutes, capture as for a hang (into $L/hang.txt) and go on to step 8.

## STEP 8: the planned reboot

Nothing can unload the module now. Flush only the log filesystem, and arm the reboot detached so that your call returns (a plain `sync` may hang on the wedged pool; `b` reboots at once, without unmounting it):

```sh
L=/var/log/task-1121-jbis
sudo -n dmesg > $L/dmesg-before-reboot.txt; sync -f $L/RESUME.md $L/dmesg-before-reboot.txt $L/fb-ccm.txt $L/runs.txt
sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'
```

After the reboot, read $L/RESUME.md and carry on with step 9 without waiting for main. If the VM is not back within 15 minutes, main will check.

## STEP 9: after the reboot, cleanup

`uptime -s` (later than step 0's), `uname -r`, `lsmod | grep -E '^(zfs|spl) '` (must be empty: nothing loads the in-tree module at boot), `ls -l /dev/zfs 2>&1` (absent), `sudo -n losetup -a` (empty), `sysctl kernel.hung_task_panic` (its step 0 value), `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1` (as at step 0), `ls -l /etc/zfs/zpool.cache 2>&1` (absent or size 0: `jbisc` had `cachefile=none` and the ZTS pools were destroyed; size above 0: report it, leave it, do not read it). If `journalctl --list-boots` shows the previous boot, quote `sudo -n journalctl -k -b -1 | grep -E 'VERIFY|PANIC at|Comm:'` (the lines of step 7.4, if the journal is persistent; say so if it is not).

Then: `ls -l /var/tmp/task-1121-jbis/fb-ccm/dev` (record), `sudo -n rm -rf /var/tmp/task-1121-jbis` (the four trees, the scripts' files and the wedged pool's vdev: never import it), `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `df -h /var/tmp`. If /etc/zfs/zpool.cache exists with size 0, `sudo -n mv /etc/zfs/zpool.cache /var/log/task-1121-jbis/zpool.cache.end`. Logs and the kit stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed, claude-rc, sysctl and udev (H) lines of step 0; FETCH-OK (by SHA or by name), TREES-OK, TREE-OK, SHA-OK, the three subject/shortstat lines; KIT-OK; rc and warnings of the four builds; the four srcversions, each load line "(DEBUG mode)", and whether fb's equals `371A65E8D86145973554A8B`; what happened to /etc/zfs/zpool.cache at steps 0 and 9; the zfs-helpers links (installed, removed); the reboot (time armed, `uptime -s` after, how you resumed).
2. Table: part | tree | what | runs | result (PASS count of the tests, setups and cleanups not counted, or FAIL with the test) | durations. Rows: A unit (`make unit T=lzjb` ×3, `make unit`), A lzjb.sh, A g44, B n45, B g45, B c45, C zil.sh, C g46, C c46, D unit-fb, D m45, D fb.sh. Then one verdict line, e.g. "lzjb: test_lzjb 3/3, make unit 12/12, planted block EIO twice with no fault, g44 46/46; CCM: new test 3/3, g45 51/51, c45 23/23; zio_crypt: encrypted ZIL replay OK, g46 7/7, c46 30/30; master: test_lzjb 1 of 4 (SIGSEGV x2, offset 0 accepted), new test FAIL at its first check, CCM 16M inherited: VERIFY0 spa_do_crypt_abd failed (5), PANIC at zio.c:5684 in z_wr_iss", or what differs.
3. Values for the PR texts, one line each, exactly these keys (n/a where a step did not run):

   ```
   lzjb.unit    = <runs with 4 of 4>/3
   lzjb.unitall = <binaries with N of N> of 12
   lzjb.psize   = <P>
   lzjb.read    = <rc of read 1>, <rc of read 2>, "<dd message>"
   lzjb.dbgmsg  = "<the dbgmsg line from 'compress' on>"
   lzjb.kernel  = <matching kernel lines over step 4: none, or the lines>
   lzjb.zts     = <PASS>/46 (<non-PASS: test, result, cause>), Running Time <t>
   lzjb.fb      = "<summary line>" (roundtrip <r>, truncated <r>, overlong <r>, offset_zero <r>), segfault lines <k>
   ccm.new      = <PASS>/3 (<durations>)
   ccm.zts      = <PASS>/51 (<non-PASS>), Running Time <t>
   ccm.crypt    = <PASS>/23 (<non-PASS>), Running Time <t>
   ccm.kernel   = <matching kernel lines over step 5>
   ccm.fbtest   = <result> ("<the ERROR line, keyfile path shortened>")
   ccm.fbpanic  = "<VERIFY0 line>" / "<PANIC line>" / thread <Comm> / txg_sync <state> / zpool sync <state> / waited <n>s
   zc.zil       = "<REPLAY-OK line>" / records "<record counts>" / auth ereports <n>
   zc.loadkey   = <PASS>/7
   zc.crypt     = <PASS>/30 (<non-PASS>), Running Time <t>
   zc.kernel    = <matching kernel lines over step 6>
   ```
4. Quotes: the START/DONE lines, the outputs of `lzjb.sh`, `zil.sh` and `fb.sh`, the unit test lines, the grep lines of n45 and m45, every non-PASS with its lines, the dmesg checks, the journal lines of step 9.
5. Deviations, cleanup, then End.
