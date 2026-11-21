TASK-1121-CCM2 (from the coordinator session "main"): functional, about 40 minutes build included, one debug build, NO reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB); not next to a performance run. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-ccm2`, `L=/var/log/task-1121-ccm2`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

TASK-1121-JBIS found the CCM commit (`a66ced4d245`, "Keep AES-CCM datasets to blocks the cipher can encrypt") failing its own new test `zfs_create_crypt_ccm_blksz` in all 3 runs and in `-T zfs_create,zfs_set`: `zfs create -o encryption=aes-256-ccm ... -o volblocksize=16M -V 64M testpool/testvol1` exited 0 where the test expects a refusal. The fork CI failed the same check on five distributions. The cause: `zfs_ioc_create()` checks the properties (`zfs_set_prop_nvlist()`, then `zfs_check_settable()`, where the commit refuses a block size above 8M on `aes-*-ccm`) only once the dataset exists, and by then `zvol_create_cb()` has taken `volblocksize` out of the properties to create the volume with it, so the check never sees a volume's block size. A `recordsize` is still among the properties at that point, which is why the `recordsize=16M` create was refused.

The amended commit (`5c400a6d9f6`, same subject, one commit, rebased from master `1b1aa0e91b6` onto master `a1daebfec3f`, whose only change in between is a ZTS chattr test) differs from `a66ced4d245` in `module/zfs/zfs_ioctl.c` only (17 insertions, 3 deletions): after `dmu_objset_create()`, `zfs_ioc_create()` checks a volume's `volblocksize` above 128K against the encryption suite the new volume got (`dsl_crypto_check_blksz()`, as `zfs_check_settable()` does for `recordsize`), and destroys the volume when it is too large, through the destroy path it already uses when a property cannot be set. The test is unchanged. It stopped at that check in JBIS, so its later steps run here for the first time: `zfs set recordsize=16M` refused on the CCM file system; an inherited `recordsize=16M` giving a 16M file 8M blocks (read with `zdb -K`); a `zfs send -L` stream with a 16M block refused by a non-raw receive below the CCM file system, and the same stream without `-L` received.

This task, on a debug module of the new head: the new test 3 times (n45), `-T zfs_create,zfs_set` once (g45), and the encrypted and large-block send and receive tests of JBIS (`jbis_crypt`, its runfile part reused unchanged) once (c45). No fail-before: JBIS ran master's module (the test fails at its first check; an inherited 16M recordsize panics in `zio_encrypt()`).

## What is tested

Fork https://github.com/tigerblue77/zfs, branch `aes-ccm-block-limit` (main force-pushed it with this head before this task), fetched by full SHA:

| tree | content | tree object |
|---|---|---|
| ccm | `5c400a6d9f63dfccc764e227919a23fbe4fb8861` (aes-ccm-block-limit) | `138a5b9034b72fc913b1313464977b9515bd52b2` |

Its parent is upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp. Touch no block device of the VM. Never run `zpool import` yourself. Never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the tests' own pools.
- Pre-run checks, before every ZTS run: `cat /sys/module/zfs/srcversion` equals `cat $L/sv-ccm`, `timeout 30 sudo -n $D/ccm/zpool list -H -o name` shows no pool, `sudo -n $D/ccm/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. A stale `/var/tmp/tunable-*` file is a stop too: report its `ls -l`. `runs.sh` repeats all these checks before each of its runs and writes a `STOP` line if one fails.
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 60 minutes; a series whose $L/runs.txt has not changed for 30 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- No reboot is approved in this task. A hang, a panic, an oops or a module that will not unload: capture as above, stop, report, and wait for main.
- A ZTS test that does not pass does not stop the task: quote its `Test:` line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:'` of its own log (`find $R -path '*/<test>*' -type f`, R the run's `dir=`: JBIS found a `stdout` and no `merged` file on VM 107), say whether `tests/test-runner/bin/zts-report.py.in` lists it as known, and go on without rerunning it. A failure on a `/dev/zvol/...` path when step 0 found H=no is the environment (TASK-1117-ER34): say so.
- `sudo -n fstrim -av` after deleting the tree. At the end no zfs or spl module is loaded, no `dmesg --follow` of this task runs, and no link of `zfs-helpers.sh` made by this task is left.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 10 GiB free on /var/tmp: one tree), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `pgrep -af 'dmesg --follow'` (empty expected; one left by an earlier task: quote it and leave it), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote them; if hung_task_panic is 1, set it to 0 for this task with `sudo -n sysctl -w kernel.hung_task_panic=0`, say so, and set it back to 1 in step 5).

Udev: `ls -l /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1`. If none of them exists (no zvol udev rules, so no `/dev/zvol` links, which several `zfs_create` tests use), note H=yes: step 4 installs the in-tree helpers and step 5 removes them. A symlink whose target is gone (`readlink -e` prints nothing) counts as absent (H=yes: `zfs-helpers.sh -i` replaces it). Anything else that exists: H=no, say what it is, and do not run `zfs-helpers.sh` in this task (VM 107 was H=no in JBIS: regular files in /etc/udev/rules.d and /usr/lib/udev).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1121-ccm2
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched (TASK-1121-JBIS's /var/log/task-1121-jbis among them). Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1121-ccm2; L=/var/log/task-1121-ccm2; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/ccm && cd $D/ccm && git init -q . && git fetch -q --depth 2 $F 5c400a6d9f63dfccc764e227919a23fbe4fb8861 && git checkout -q FETCH_HEAD && echo FETCH-OK
```

If the fetch fails, wait 1 minute and run the `git fetch ... && git checkout -q FETCH_HEAD` part once more, in `$D/ccm`. If it fails again, fetch the branch by name instead, in the same directory (`git fetch -q --depth 2 $F aes-ccm-block-limit && git checkout -q FETCH_HEAD && echo FETCH-OK`), say so, and go on: the commands below stop on a branch that has moved (main may not have pushed yet: then the branch still holds `a66ced4d245`, and TREE-OK fails).

```sh
D=/var/tmp/task-1121-ccm2; L=/var/log/task-1121-ccm2; T=tests/zfs-tests/tests/functional/cli_root/zfs_create/zfs_create_crypt_ccm_blksz.ksh
cd $D/ccm && echo "ccm $(git rev-parse HEAD) $(git rev-parse 'HEAD^{tree}') $(git rev-parse HEAD~1) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "ccm 5c400a6d9f63dfccc764e227919a23fbe4fb8861 138a5b9034b72fc913b1313464977b9515bd52b2 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' \
"38cac0f0b8647163eb73464559d1197d3832ca1eb930abc702d2507c42db5b72  $D/ccm/module/zfs/zfs_ioctl.c" \
"7068ea85168e52c21ad400db6625fdecd6bc8f9ea2bf474bee57c77b648fcac2  $D/ccm/module/zfs/zio.c" \
"ac8c35b965db2c3906c1785555d4b6b9d3ef0dc0aa5ed62931d3d7b5a157c0a0  $D/ccm/module/zfs/zio_crypt.c" \
"fa6eda2098baee69e28527625b90486d6963a3915201f1da5c183c58ee5ad274  $D/ccm/module/zfs/dmu.c" \
"28c1fd454da3ad95273e87e935451f50fd5a5e8916ca16db0a1e2c6170bea3eb  $D/ccm/module/zfs/dmu_recv.c" \
"6442b9e349320600e80040a246a5e8fafd85d52cd5d4651337a0f142162d7382  $D/ccm/module/zfs/dsl_crypt.c" \
"91a3a2837eb274d5abae0067d14179b85d1f52fd670f2e1159bfc1268a8f0035  $D/ccm/$T" \
"f5a4ed76ee574833c7fe6680ebc108e4cc12bad85b16d40df3881d9adcaab426  $D/ccm/tests/runfiles/common.run" \
> $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
echo "ccm: $(git -C $D/ccm log -1 --format=%s) |$(git -C $D/ccm diff --shortstat HEAD~1 HEAD)"
grep -c 'dsl_crypto_check_blksz' $D/ccm/module/zfs/zfs_ioctl.c; ls -l $D/ccm/$T | cut -c1-10
```

Expected: FETCH-OK, TREE-OK, SHA-OK (all files but `zfs_ioctl.c` have the sums JBIS checked), then `ccm: Keep AES-CCM datasets to blocks the cipher can encrypt | 12 files changed, 248 insertions(+), 9 deletions(-)`, `2` and `-rwxr-xr-x`. Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the tree.

## STEP 2: the kit

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to $L/runs.sh and $L/ccm2.run in that order. Then:

```sh
L=/var/log/task-1121-ccm2
printf '%s\n' "81e9a299422e726a23eee12b43837244789b7579e1298eae0cac4140a9d2cbbf  $L/runs.sh" "9a24810b2663b1ca7817583d3322e10508036583fe5e9b0eba4aa744b4cd768e  $L/ccm2.run" | sha256sum -c && sh -n $L/runs.sh && echo KIT-OK
```

Expected two `OK` lines and KIT-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tree> <tag> <runs> <selection>` is JBIS's, with this task's `D` and `L`. It runs as the login user, from `$D/<tree>`. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks (the srcversion one against `$L/sv-<tree>`); each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1121-CCM2 ZTS runner, run as the login user:
# runs.sh <tree> <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1121-ccm2
L=/var/log/task-1121-ccm2
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

`ccm2.run` is the `jbis_crypt` part of JBIS's `jbis.run`, unchanged: common.run's `[DEFAULT]` and 23 tests of common.run (17 of `rsend`, 6 of `cli_root/zfs_receive`) under the tag `jbis_crypt`. They send and receive encrypted and large-block data, raw and not; `send_raw_large_blocks` and `zfs_receive_large_block_corrective` receive `zfs send -L` streams of 1M blocks into `aes-256-ccm` file systems (allowed: 1M is below the 8M limit).

```
# TASK-1121-CCM2: the jbis_crypt part of TASK-1121-JBIS's jbis.run,
# tests of common.run, unchanged: encrypted and large-block send and
# receive.

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
```

## STEP 3: build (about 10 minutes)

```sh
D=/var/tmp/task-1121-ccm2; L=/var/log/task-1121-ccm2
setsid nohup sh -c "cd $D/ccm && ./autogen.sh && ./configure --enable-debug && nice make -j7; echo build-rc=\$?" > $L/build-ccm.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-ccm.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Then:

```sh
D=/var/tmp/task-1121-ccm2; L=/var/log/task-1121-ccm2
/usr/sbin/modinfo -F srcversion $D/ccm/module/zfs.ko > $L/sv-ccm; cat $L/sv-ccm
```

It must differ from JBIS's builds of the old head, `863F766E2012507AD160B03`, and of master, `371A65E8D86145973554A8B` (equal to either: stop and report).

## STEP 4: the runs

If H=yes: `cd /var/tmp/task-1121-ccm2/ccm && sudo -n ./scripts/zfs-helpers.sh -i`, then quote `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id` (symlinks into `$D/ccm`).

Load: `sudo -n /var/tmp/task-1121-ccm2/ccm/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `$L/sv-ccm`) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). The ZTS runner clears the kernel log, so keep a follower from now on:

```sh
L=/var/log/task-1121-ccm2
setsid nohup sudo -n sh -c "exec dmesg --follow > $L/dmesg-follow.txt" > /dev/null 2>&1 < /dev/null &
```

Run the pre-run checks (tree ccm), then the new test 3 times (tag n45), the two groups once (g45) and the send/receive tests once (c45), one series after the other:

```sh
L=/var/log/task-1121-ccm2
setsid nohup sh -c "sh $L/runs.sh ccm n45 3 -t zfs_create_crypt_ccm_blksz && sh $L/runs.sh ccm g45 1 -T zfs_create,zfs_set && sh $L/runs.sh ccm c45 1 -r $L/ccm2.run -T jbis_crypt" > $L/runs.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` until `DONE c45` or a `STOP` line (about 15 minutes: JBIS took 7 minutes for g45 and 3 for c45). Expected:

- n45: in each run the zfs_create setup, `zfs_create_crypt_ccm_blksz` and the cleanup PASS, the test in well under a minute (it writes 16M twice and sends one stream twice). For the first run, with R its `dir=`, quote `grep -nE 'zfs (create|set|inherit|send)|datasetexists|test [0-9]+M? = |cannot (create|receive)|block size' $R/log | cut -c1-220`. Expected, in order: the `aes-256-ccm` create with `recordsize=16M` `exited 1` and `datasetexists testpool/testfs1/ccm exited 1`; the `aes-256-ccm` create with `volblocksize=16M -V 64M testpool/testvol1` `exited 1` (its message, if the log carries it: `cannot create 'testpool/testvol1': invalid property value`) and `datasetexists testpool/testvol1 exited 1`; the `aes-256-gcm` create with `recordsize=16M` passing; the `aes-256-ccm` create with `recordsize=8M` passing; `zfs set recordsize=16M testpool/testfs1/ccm exited 1`; `test 8388608 = 8388608`; `zfs inherit recordsize`; `test 16777216 = 16777216`; `test 8M = 8M` (the file that inherited 16M got 8M blocks); the `zfs send -L ... | zfs receive testpool/testfs1/ccm/recv` line `exited 1`, with its message if the log carries it (expected to contain `DRR_OBJECT block size 16777216 is more than the destination's encryption allows; send without -L`); `datasetexists testpool/testfs1/ccm/recv exited 1`; the `zfs send` without `-L` passing.
- g45: all PASS, 51 tests (22 of `cli_root/zfs_create`, the new one among them, and 29 of `cli_root/zfs_set`) with their setups and cleanups.
- c45: all PASS, the 23 tests of `jbis_crypt` with the setups and cleanups of `rsend` and `cli_root/zfs_receive`. Quote the lines of `send_encrypted_props`, `send_raw_large_blocks` and `zfs_receive_large_block_corrective`.

Then quote:

```sh
L=/var/log/task-1121-ccm2
grep -E '^(START|DONE|STOP) ' $L/runs.txt; cat $L/runs.nohup
grep -E '^(n45|g45|c45)-[0-9]+ rc=' $L/runs.txt
for t in n45 g45 c45; do sed -n "/^START $t /,/^DONE $t /p" $L/runs.txt | grep -cE '\[PASS\]$'; done
grep -E '\[(FAIL|KILLED|SKIP)\]$' $L/runs.txt
```

Expected no STOP, an empty nohup file, every rc=0, the PASS counts 9, 55 and 27 (setups and cleanups included), and no FAIL, KILLED or SKIP line. Quote every `Test:` line of tag n45, and the Results Summary of $L/zts-g45-1.log and $L/zts-c45-1.log. In JBIS, g45 was `PASS 54`, `FAIL 1` (the new test) and c45 `PASS 27`.

## STEP 5: end and cleanup

The pre-run checks once more. Then every line of `$L/dmesg-follow.txt` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than|segfault|KASAN|WARNING`, apart from the module's own "(DEBUG mode)" lines, quoted with 10 lines before it (none expected). Unload with `sudo -n /var/tmp/task-1121-ccm2/ccm/scripts/zfs.sh -u` (quote rc; `lsmod | grep -E '^(zfs|spl) '` empty), stop the follower (`sudo -n pkill -f '^dmesg --follow$'`; `pgrep -af 'dmesg --follow'` shows only what step 0 found) and save `sudo -n dmesg > /var/log/task-1121-ccm2/dmesg-end.txt`. If H=yes: `cd /var/tmp/task-1121-ccm2/ccm && sudo -n ./scripts/zfs-helpers.sh -r`, then `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id /usr/sbin/mount.zfs 2>&1` (none left). If step 0 set hung_task_panic to 0: `sudo -n sysctl -w kernel.hung_task_panic=1`.

`ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0 expected (size 0: `sudo -n mv /etc/zfs/zpool.cache /var/log/task-1121-ccm2/zpool.cache.end`; size above 0: report it, leave it, do not read it). Then `sudo -n rm -rf /var/tmp/task-1121-ccm2`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `df -h /var/tmp`. Logs and the kit stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed, sysctl, follower and udev (H) lines of step 0; FETCH-OK (by SHA or by name), TREE-OK, SHA-OK, the subject/shortstat line; KIT-OK; rc and warnings of the build; the srcversion and the load line "(DEBUG mode)"; what happened to /etc/zfs/zpool.cache at steps 0 and 5; the zfs-helpers links (installed, removed, or H=no).
2. Table: tag | what | runs | result (PASS count of the tests, setups and cleanups not counted, or FAIL with the test) | durations. Rows: n45, g45, c45. Then one verdict line, e.g. "CCM 5c400a6d9f6: new test 3/3, g45 51/51, c45 23/23, kernel log clean", or what differs.
3. Values for the PR text, one line each, exactly these keys (n/a where a step did not run):

   ```
   ccm.sv       = <srcversion>
   ccm.new      = <PASS>/3 (<durations>)
   ccm.newlog   = vol16M <exited N>, "<its cannot create line or none>"; set16M <exited N>; inherited dblk <value>; send -L recv <exited N>, "<its cannot receive line or none>"; send recv <PASS/FAIL>
   ccm.zts      = <PASS>/51 (<non-PASS: test, result, cause>), Running Time <t>
   ccm.crypt    = <PASS>/23 (<non-PASS>), Running Time <t>
   ccm.kernel   = <matching kernel lines over step 4: none, or the lines>
   ```
4. Quotes: the START/DONE lines, the grep lines of n45, every non-PASS with its lines, the dmesg check.
5. Deviations, cleanup, then End.
