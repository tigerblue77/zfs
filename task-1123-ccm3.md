TASK-1123-CCM3 (from the coordinator session "main"): functional, about 1 hour builds included, at most 1 h 45; one commit built twice, debug then release, NO reboot. Any of the three test VMs (Debian 13, kernel 6.12; 8 vCPU and 7.8 GB on 105 and 106, 32 CPUs and 32 GiB on 107); not next to a performance run. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The files of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1123-ccm3`, `L=/var/log/task-1123-ccm3`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Plan and durations

| step | what | about |
| --- | --- | --- |
| 0 | state of this VM | 3 min |
| 1 | two checkouts of one commit, their checks | 4 min |
| 2 | the kit | 2 min |
| 3 | both builds at once: `dbg` with `--enable-debug`, `rel` without | 20 min |
| 4 | debug module: the new test 3 times, two groups, the send/receive tests | 12 min |
| 5 | release module: the same | 11 min |
| 6 | end and cleanup | 5 min |

## Why

TASK-1121-CCM2 ran this pull request's commit at its previous head, `5c400a6d9f6`, on a debug module (VM 107): the new test `zfs_create_crypt_ccm_blksz` 3/3, `-T zfs_create,zfs_set` 51/51, the encrypted and large-block send and receive tests of TASK-1121-JBIS (`jbis_crypt`) 23/23, kernel log clean. The branch `aes-ccm-block-limit` has moved since (force-pushed on 22/11): its head is now `f1551ee9483`, which no VM has run, in any build.

What changed between the two heads (`git range-diff` and `git diff 5c400a6d9f6 f1551ee9483`): `module/zfs/zio.c` and the commit message, nothing else. `5c400a6d9f6` also turned the `VERIFY0()` around `spa_do_crypt_abd()` in `zio_encrypt()` into a failed write (EIO, or a suspended pool for a write that must succeed), for an object that already has 16M blocks, which no block size check can reach. Upstream PR #19996 makes that same change on its own; as announced in the review of #19996 posted on 22/11, the hunk was dropped from this commit, whose message now leaves that case to #19996 and cites it. So `f1551ee9483` is `5c400a6d9f6` minus its `zio.c` hunk (24 lines out, 3 back): its `zio.c` is master's again (the file of `a1daebfec3f`, with the `VERIFY0()`), its parent is still upstream master `a1daebfec3f`, its other ten files are byte for byte those of `5c400a6d9f6` (the sha256 CCM2 checked), and its diffstat on master is `11 files changed, 224 insertions(+), 6 deletions(-)` (it was 12 files, +248 -9).

The test file is unchanged, so the expected lines below are CCM2's, with one correction: CCM2 found `zfs set recordsize=16M` on the CCM file system exiting 255 where its text expected 1 (`log_mustnot` accepts any non-zero status: both are refusals). None of these tests reaches the failure in `zio_encrypt()`: the file that inherits `recordsize=16M` gets 8M blocks, and the send and receive tests write 1M blocks at most. The dropped hunk is therefore not expected to change a result; this task checks it on the module as it will be reviewed.

Release: the pull request's VM evidence comes from debug modules only. This change's checks are not assertions (`zfs_check_settable()` and `zfs_ioc_create()` refuse with `ZFS_ERR_BADPROP`, `dmu_object_set_blocksize()` clamps the size, `receive_read_record()` refuses the record), so a release module (no `ASSERT`; `VERIFY` stays) must give the same results. A srcversion hashes the module's sources, not its build flags: the two builds are expected to show the same srcversion (TASK-1121-DBM4 and TASK-1121-B24 saw that). Which build a tree holds is told by its `config.log` (`DEBUG_ZFS='_with_debug'` or `'_without_debug'`), and which module is loaded by its load line (`(DEBUG mode)` or not).

This task, on the new head: the new test 3 times, `-T zfs_create,zfs_set` once and `jbis_crypt` once (its runfile reused unchanged), first on the debug module (tags n45d, g45d, c45d), then the same on the release module (n45r, g45r, c45r). No fail-before: JBIS ran master's module (the test fails at its first check; an inherited 16M recordsize panics in `zio_encrypt()`).

## What is tested

Fork https://github.com/tigerblue77/zfs, branch `aes-ccm-block-limit`, fetched by full SHA, twice (one checkout per build):

| tree | content | build | tree object |
|---|---|---|---|
| dbg | `f1551ee948359a309163f4128103eed072157add` (aes-ccm-block-limit) | `./configure --enable-debug` | `8c981198b3dc786901a3e1b6d010fade12ca20f3` |
| rel | the same commit | `./configure` | `8c981198b3dc786901a3e1b6d010fade12ca20f3` |

Its parent is upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs under /var/tmp. Touch no block device of the VM. Never run `zpool import` yourself. Never `zfs-tests.sh -x`.
- A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the tests' own pools.
- Pre-run checks, before every ZTS run, `<tree>` being the tree whose module is loaded (`dbg` in step 4, `rel` in step 5): `cat /sys/module/zfs/srcversion` equals `cat $L/sv-<tree>`, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `sudo -n $D/<tree>/zinject | head -n 1` prints `No handlers registered.` (only that first line is compared), `sudo -n losetup -a` shows nothing, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. On Linux the module truncates the cache file to 0 bytes when the last pool goes, so an empty one is normal; a non-empty one names a pool left behind: stop and report its `ls -l`. A stale `/var/tmp/tunable-*` file is a stop too: report its `ls -l`. `runs.sh` repeats all these checks before each of its runs and writes a `STOP` line if one fails.
- Every build and every `runs.sh` series runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 75 minutes; a series whose $L/runs.txt has not changed for 30 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|z_|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- No reboot is approved in this task. A hang, a panic, an oops or a module that will not unload: capture as above, stop, report, and wait for main.
- A ZTS test that does not pass does not stop the task: quote its `Test:` line, `grep -n -B 20 -m 1 -E '^ERROR|ERROR:'` of its own log (`find $R -path '*/<test>*' -type f`, R the run's `dir=`: JBIS found a `stdout` and no `merged` file on VM 107), say whether `tests/test-runner/bin/zts-report.py.in` lists it as known, and go on without rerunning it. A failure on a `/dev/zvol/...` path when step 0 found H=no is the environment (TASK-1117-ER34): say so.
- `sudo -n fstrim -av` after deleting the trees. At the end no zfs or spl module is loaded, no `dmesg --follow` of this task runs, and no link of `zfs-helpers.sh` made by this task is left.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp: two trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `id zts`, `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed), `pgrep -af 'dmesg --follow'` (empty expected; one left by an earlier task: quote it and leave it), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote them; if hung_task_panic is 1, set it to 0 for this task with `sudo -n sysctl -w kernel.hung_task_panic=0`, say so, and set it back to 1 in step 6).

Udev: `ls -l /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1`. If none of them exists (no zvol udev rules, so no `/dev/zvol` links, which several `zfs_create` tests use), note H=yes: step 4 installs the in-tree helpers of `dbg` and step 6 removes them. A symlink whose target is gone (`readlink -e` prints nothing) counts as absent (H=yes: `zfs-helpers.sh -i` replaces it). Anything else that exists: H=no, say what it is, and do not run `zfs-helpers.sh` in this task (VM 107 was H=no in JBIS and CCM2: regular files in /etc/udev/rules.d and /usr/lib/udev).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1123-ccm3
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched (TASK-1121-CCM2's /var/log/task-1121-ccm2 among them, on the VM that ran it). Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1123-ccm3; L=/var/log/task-1123-ccm3; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
for t in dbg rel; do mkdir $D/$t && cd $D/$t && git init -q . && git fetch -q --depth 2 $F f1551ee948359a309163f4128103eed072157add && git checkout -q FETCH_HEAD && echo "FETCH-OK $t"; done
```

Expected `FETCH-OK dbg` and `FETCH-OK rel`. If a fetch fails, wait 1 minute and run the `git fetch ... && git checkout -q FETCH_HEAD` part once more, in that tree's directory. If it fails again, fetch the branch by name instead, in the same directory (`git fetch -q --depth 2 $F aes-ccm-block-limit && git checkout -q FETCH_HEAD && echo FETCH-OK`), say so, and go on: the commands below stop on a branch that has moved.

```sh
D=/var/tmp/task-1123-ccm3; L=/var/log/task-1123-ccm3; T=tests/zfs-tests/tests/functional/cli_root/zfs_create/zfs_create_crypt_ccm_blksz.ksh
for t in dbg rel; do cd $D/$t && echo "$t $(git rev-parse HEAD) $(git rev-parse 'HEAD^{tree}') $(git rev-parse HEAD~1) $(git status --porcelain | wc -l)"; done > $L/tree.txt
printf '%s\n' "dbg f1551ee948359a309163f4128103eed072157add 8c981198b3dc786901a3e1b6d010fade12ca20f3 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0" "rel f1551ee948359a309163f4128103eed072157add 8c981198b3dc786901a3e1b6d010fade12ca20f3 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
for t in dbg rel; do printf '%s\n' \
"38cac0f0b8647163eb73464559d1197d3832ca1eb930abc702d2507c42db5b72  $D/$t/module/zfs/zfs_ioctl.c" \
"be31510828342e582cdc11c93ed51d175c177e3d10b7ffe7d285ad1d5876e32d  $D/$t/module/zfs/zio.c" \
"ac8c35b965db2c3906c1785555d4b6b9d3ef0dc0aa5ed62931d3d7b5a157c0a0  $D/$t/module/zfs/zio_crypt.c" \
"fa6eda2098baee69e28527625b90486d6963a3915201f1da5c183c58ee5ad274  $D/$t/module/zfs/dmu.c" \
"28c1fd454da3ad95273e87e935451f50fd5a5e8916ca16db0a1e2c6170bea3eb  $D/$t/module/zfs/dmu_recv.c" \
"6442b9e349320600e80040a246a5e8fafd85d52cd5d4651337a0f142162d7382  $D/$t/module/zfs/dsl_crypt.c" \
"91a3a2837eb274d5abae0067d14179b85d1f52fd670f2e1159bfc1268a8f0035  $D/$t/$T" \
"f5a4ed76ee574833c7fe6680ebc108e4cc12bad85b16d40df3881d9adcaab426  $D/$t/tests/runfiles/common.run"; done > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
for t in dbg rel; do echo "$t: $(git -C $D/$t log -1 --format=%s) |$(git -C $D/$t diff --shortstat HEAD~1 HEAD)"; done
for t in dbg rel; do echo "$t $(grep -c 'dsl_crypto_check_blksz' $D/$t/module/zfs/zfs_ioctl.c) $(grep -c 'VERIFY0(spa_do_crypt_abd' $D/$t/module/zfs/zio.c) $(grep -c 'cannot encrypt objset' $D/$t/module/zfs/zio.c) $(ls -l $D/$t/$T | cut -c1-10)"; done
```

Expected: TREE-OK, SHA-OK (`zio.c` has master's sum, `be315108...`; the seven other files have the sums CCM2 checked on `5c400a6d9f6`), then twice `<tree>: Keep AES-CCM datasets to blocks the cipher can encrypt | 11 files changed, 224 insertions(+), 6 deletions(-)`, then `dbg 2 1 0 -rwxr-xr-x` and `rel 2 1 0 -rwxr-xr-x` (`dsl_crypto_check_blksz` twice in `zfs_ioctl.c`; master's `VERIFY0(spa_do_crypt_abd(...))` back in `zio.c`, and no trace of the dropped hunk's `cannot encrypt objset` message; the test executable). Any other difference (the branch may have moved): stop and report as is. Do not edit any file of the trees.

## STEP 2: the kit

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to $L/runs.sh and $L/ccm2.run in that order. Then:

```sh
L=/var/log/task-1123-ccm3
printf '%s\n' "aad465b65863a32882b1e5d257612ccff6861813e402e9cc048d755764b40a0e  $L/runs.sh" "9a24810b2663b1ca7817583d3322e10508036583fe5e9b0eba4aa744b4cd768e  $L/ccm2.run" | sha256sum -c && sh -n $L/runs.sh && echo KIT-OK
```

Expected two `OK` lines and KIT-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tree> <tag> <runs> <selection>` is CCM2's, with this task's `D` and `L`. It runs as the login user, from `$D/<tree>`. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after the pre-run checks (the srcversion one against `$L/sv-<tree>`); each run's console goes to $L/zts-<tag>-<n>.log, and $L/runs.txt gets a line `<tag>-<n> rc=<rc> dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1123-CCM3 ZTS runner, run as the login user:
# runs.sh <tree> <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1123-ccm3
L=/var/log/task-1123-ccm3
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

`ccm2.run` is CCM2's file byte for byte (same sum), which is the `jbis_crypt` part of JBIS's `jbis.run`, unchanged: common.run's `[DEFAULT]` and 23 tests of common.run (17 of `rsend`, 6 of `cli_root/zfs_receive`) under the tag `jbis_crypt`. They send and receive encrypted and large-block data, raw and not; `send_raw_large_blocks` and `zfs_receive_large_block_corrective` receive `zfs send -L` streams of 1M blocks into `aes-256-ccm` file systems (allowed: 1M is below the 8M limit).

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

## STEP 3: both builds at once (about 20 minutes)

Half the CPUs each (4 jobs per build on 8 vCPUs, 16 on VM 107):

```sh
D=/var/tmp/task-1123-ccm3; L=/var/log/task-1123-ccm3; J=$(( $(nproc) / 2 ))
setsid nohup sh -c "cd $D/dbg && ./autogen.sh && ./configure --enable-debug && nice make -j$J; echo build-rc=\$?" > $L/build-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/rel && ./autogen.sh && ./configure && nice make -j$J; echo build-rc=\$?" > $L/build-rel.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-dbg.log $L/build-rel.log` until two lines. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected: CCM2's debug build had none; the release build has no `-Werror`, so a warning there would not stop it: quote it). Then:

```sh
D=/var/tmp/task-1123-ccm3; L=/var/log/task-1123-ccm3
grep -H 'whether assertion support will be enabled' $L/build-dbg.log $L/build-rel.log
grep -H '^DEBUG_ZFS=' $D/dbg/config.log $D/rel/config.log
for t in dbg rel; do echo "$t $(grep -c 'define ZFS_DEBUG 1' $D/$t/zfs_config.h) $(grep -c -a 'DEBUG mode' $D/$t/module/zfs.ko)"; done
for t in dbg rel; do /usr/sbin/modinfo -F srcversion $D/$t/module/zfs.ko > $L/sv-$t; echo "$t $(cat $L/sv-$t)"; done
```

Expected:

- `checking whether assertion support will be enabled... yes` for `dbg`, `... no` for `rel`;
- `DEBUG_ZFS='_with_debug'` for `dbg`, `DEBUG_ZFS='_without_debug'` for `rel`;
- `dbg 1 <n>` with n above 0 (the debug module carries its "(DEBUG mode)" string) and `rel 0 0`;
- SD (`dbg`) and SR (`rel`). They are expected equal (same sources); a difference is recorded, not a stop. Each must differ from CCM2's build of the previous head, `916F9C58D9B0C93FBD401BF`, and from JBIS's builds of `a66ced4d245`, `863F766E2012507AD160B03`, and of master, `371A65E8D86145973554A8B` (equal to any of them: stop and report).

A `rel` tree with `_with_debug`, a `yes`, a `1` or a "DEBUG mode" count above 0 is not a release build: stop and report.

## STEP 4: the runs on the debug module

If H=yes: `cd /var/tmp/task-1123-ccm3/dbg && sudo -n ./scripts/zfs-helpers.sh -i`, then quote `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id` (symlinks into `$D/dbg`; they serve step 5 too: `rel`'s `zvol_id` and rules are the same sources).

Load: `sudo -n /var/tmp/task-1123-ccm3/dbg/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `$L/sv-dbg`), the last `ZFS: Loaded module` line of `sudo -n dmesg` (it must contain "(DEBUG mode)") and `cat /sys/module/zfs/parameters/zfs_flags` (not 0 on a debug module; recorded). The ZTS runner clears the kernel log, so keep a follower from now on:

```sh
L=/var/log/task-1123-ccm3
setsid nohup sudo -n sh -c "exec dmesg --follow > $L/dmesg-follow-dbg.txt" > /dev/null 2>&1 < /dev/null &
```

Run the pre-run checks (tree `dbg`), then the new test 3 times (tag n45d), the two groups once (g45d) and the send/receive tests once (c45d), one series after the other:

```sh
L=/var/log/task-1123-ccm3
setsid nohup sh -c "sh $L/runs.sh dbg n45d 3 -t zfs_create_crypt_ccm_blksz && sh $L/runs.sh dbg g45d 1 -T zfs_create,zfs_set && sh $L/runs.sh dbg c45d 1 -r $L/ccm2.run -T jbis_crypt" > $L/runs-dbg.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` until `DONE c45d` or a `STOP` line (about 11 minutes: CCM2 took 12 s for n45, 7 min 10 for g45 and 2 min 47 for c45). Expected:

- n45d: in each run the zfs_create setup, `zfs_create_crypt_ccm_blksz` and the cleanup PASS, the test in a few seconds (CCM2: `[00:02]` each). For the first run, with R its `dir=`, quote `grep -nE 'zfs (create|set|inherit|send)|datasetexists|test [0-9]+M? = |cannot (create|receive)|block size' $R/log | cut -c1-220`. Expected, in order (as CCM2 found them on the previous head; the test has not changed): the `aes-256-ccm` create with `recordsize=16M` `exited 1` and `datasetexists testpool/testfs1/ccm exited 1`; the `aes-256-ccm` create with `volblocksize=16M -V 64M testpool/testvol1` `exited 1` and `datasetexists testpool/testvol1 exited 1`; the `aes-256-gcm` create with `recordsize=16M` passing; the `aes-256-ccm` create with `recordsize=8M` passing; `zfs set recordsize=16M testpool/testfs1/ccm exited 255` (CCM2's value; another non-zero status is a refusal too: record it); `test 8388608 = 8388608`; `zfs inherit recordsize testpool/testfs1/ccm`; `test 16777216 = 16777216`; `test 8M = 8M` (the file that inherited 16M got 8M blocks); `eval zfs send -L testpool/testfs1@snap | zfs receive testpool/testfs1/ccm/recv exited 1`; `datasetexists testpool/testfs1/ccm/recv exited 1`; `eval zfs send testpool/testfs1@snap | zfs receive testpool/testfs1/ccm/recv` passing. CCM2's log carried no `cannot create` or `cannot receive` message line: none is expected; one that appears is quoted (expected wordings: `cannot create 'testpool/testvol1': invalid property value`, and for the receive `DRR_OBJECT block size 16777216 is more than the destination's encryption allows; send without -L`).
- g45d: all PASS, 51 tests (22 of `cli_root/zfs_create`, the new one among them, and 29 of `cli_root/zfs_set`) with their setups and cleanups.
- c45d: all PASS, the 23 tests of `jbis_crypt` with the setups and cleanups of `rsend` and `cli_root/zfs_receive`. Quote the lines of `send_encrypted_props`, `send_raw_large_blocks` and `zfs_receive_large_block_corrective`.

Then quote:

```sh
L=/var/log/task-1123-ccm3
grep -E '^(START|DONE|STOP) ' $L/runs.txt; cat $L/runs-dbg.nohup
grep -E '^(n45d|g45d|c45d)-[0-9]+ rc=' $L/runs.txt
for t in n45d g45d c45d; do sed -n "/^START $t /,/^DONE $t /p" $L/runs.txt | grep -cE '\[PASS\]$'; done
sed -n '/^START n45d /,/^DONE c45d /p' $L/runs.txt | grep -E '\[(FAIL|KILLED|SKIP)\]$'
```

Expected no STOP, an empty nohup file, every rc=0, the PASS counts 9, 55 and 27 (setups and cleanups included), and no FAIL, KILLED or SKIP line. Quote every `Test:` line of tag n45d, and the Results Summary of $L/zts-g45d-1.log and $L/zts-c45d-1.log (CCM2: `PASS 55`, Running Time 00:07:10; `PASS 27`, 00:02:47).

End of the debug part: the pre-run checks once more (tree `dbg`). Then every line of `$L/dmesg-follow-dbg.txt` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than|segfault|KASAN|WARNING`, apart from the module's own "(DEBUG mode)" lines, quoted with 10 lines before it (none expected). Unload with `sudo -n /var/tmp/task-1123-ccm3/dbg/scripts/zfs.sh -u` (quote rc; `lsmod | grep -E '^(zfs|spl) '` empty), stop the follower (`sudo -n pkill -f '^dmesg --follow$'`; `pgrep -af 'dmesg --follow'` shows only what step 0 found) and save `sudo -n dmesg > /var/log/task-1123-ccm3/dmesg-dbg.txt`. A module that will not unload: capture as the VM rules say, stop and report (step 5 needs it gone).

## STEP 5: the same runs on the release module

Load: `sudo -n /var/tmp/task-1123-ccm3/rel/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `$L/sv-rel`), the last `ZFS: Loaded module` line of `sudo -n dmesg`, which must NOT contain "(DEBUG mode)" (expected `ZFS: Loaded module v2.4.99-1, ZFS pool version 5000, ZFS filesystem version 5`; with "(DEBUG mode)", the debug module is loaded: unload it, stop and report), and `cat /sys/module/zfs/parameters/zfs_flags` (`0` expected on a release module). Start a second follower:

```sh
L=/var/log/task-1123-ccm3
setsid nohup sudo -n sh -c "exec dmesg --follow > $L/dmesg-follow-rel.txt" > /dev/null 2>&1 < /dev/null &
```

Run the pre-run checks (tree `rel`), then:

```sh
L=/var/log/task-1123-ccm3
setsid nohup sh -c "sh $L/runs.sh rel n45r 3 -t zfs_create_crypt_ccm_blksz && sh $L/runs.sh rel g45r 1 -T zfs_create,zfs_set && sh $L/runs.sh rel c45r 1 -r $L/ccm2.run -T jbis_crypt" > $L/runs-rel.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` until `DONE c45r` or a `STOP` line (about 10 minutes). Expected exactly as in step 4, with the tags n45r, g45r and c45r: the same `grep` of the first n45r run with the same lines in the same order, g45r 51 PASS, c45r 23 PASS. Then the same quotes, with `runs-rel.nohup`, the tags `n45r|g45r|c45r`, the range `/^START n45r /,/^DONE c45r /`, every `Test:` line of n45r, and the Results Summary of $L/zts-g45r-1.log and $L/zts-c45r-1.log. Expected the same counts (9, 55, 27) and no FAIL, KILLED or SKIP line.

End of the release part: the pre-run checks once more (tree `rel`); every line of `$L/dmesg-follow-rel.txt` matching the pattern of step 4, quoted with 10 lines before it (none expected, and no "(DEBUG mode)" line at all); unload with `sudo -n /var/tmp/task-1123-ccm3/rel/scripts/zfs.sh -u` (quote rc; `lsmod | grep -E '^(zfs|spl) '` empty); stop the follower as in step 4; `sudo -n dmesg > /var/log/task-1123-ccm3/dmesg-rel.txt`.

## STEP 6: end and cleanup

If H=yes: `cd /var/tmp/task-1123-ccm3/dbg && sudo -n ./scripts/zfs-helpers.sh -r`, then `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id /usr/sbin/mount.zfs 2>&1` (none left). If step 0 set hung_task_panic to 0: `sudo -n sysctl -w kernel.hung_task_panic=1`. `pgrep -af 'dmesg --follow'` shows only what step 0 found.

`ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0 expected (size 0: `sudo -n mv /etc/zfs/zpool.cache /var/log/task-1123-ccm3/zpool.cache.end`; size above 0: report it, leave it, do not read it). Then `sudo -n rm -rf /var/tmp/task-1123-ccm3`, `sudo -n fstrim -av`, `ls -d /var/tmp/file-* /var/tmp/spare-* /var/tmp/tunable-* 2>&1` (none), `df -h /var/tmp`. Logs and the kit stay in $L (the ZTS results stay under /var/tmp/test_results).

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, the zed, sysctl, follower and udev (H) lines of step 0; FETCH-OK for both trees (by SHA or by name), TREE-OK, SHA-OK, the two subject/shortstat lines, the two count lines; KIT-OK; rc and warnings of both builds; the assertion, `DEBUG_ZFS`, `ZFS_DEBUG` and "DEBUG mode" lines; SD and SR; for each load the srcversion, the load line and `zfs_flags`; what happened to /etc/zfs/zpool.cache at steps 0 and 6; the zfs-helpers links (installed, removed, or H=no).
2. Table: tag | build | what | runs | result (PASS count of the tests, setups and cleanups not counted, or FAIL with the test) | durations. Rows: n45d, g45d, c45d, n45r, g45r, c45r. Then one verdict line, e.g. "CCM f1551ee9483: debug: new test 3/3, g45 51/51, c45 23/23; release: new test 3/3, g45 51/51, c45 23/23; kernel log clean in both", or what differs.
3. Values for the PR text, one line each, exactly these keys (n/a where a step did not run):

   ```
   ccm.sv         = dbg <SD>, rel <SR>
   ccm.load       = dbg "<load line>"; rel "<load line>"
   ccm.dbg.new    = <PASS>/3 (<durations>)
   ccm.dbg.newlog = vol16M <exited N>, "<its cannot create line or none>"; set16M <exited N>; inherited dblk <value>; send -L recv <exited N>, "<its cannot receive line or none>"; send recv <PASS/FAIL>
   ccm.dbg.zts    = <PASS>/51 (<non-PASS: test, result, cause>), Running Time <t>
   ccm.dbg.crypt  = <PASS>/23 (<non-PASS>), Running Time <t>
   ccm.dbg.kernel = <matching kernel lines over step 4: none, or the lines>
   ccm.rel.new    = <PASS>/3 (<durations>)
   ccm.rel.newlog = vol16M <exited N>, "<line or none>"; set16M <exited N>; inherited dblk <value>; send -L recv <exited N>, "<line or none>"; send recv <PASS/FAIL>
   ccm.rel.zts    = <PASS>/51 (<non-PASS>), Running Time <t>
   ccm.rel.crypt  = <PASS>/23 (<non-PASS>), Running Time <t>
   ccm.rel.kernel = <matching kernel lines over step 5: none, or the lines>
   ```
4. Quotes: the START/DONE lines, the grep lines of n45d-1 and n45r-1, every non-PASS with its lines, the two dmesg checks.
5. Deviations, cleanup, then End.
