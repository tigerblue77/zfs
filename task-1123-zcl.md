TASK-1123-ZCL (from the coordinator session "main"): functional, on **VM 107** only (Debian 13, Linux 6.12.118, 32 CPUs, 32 GiB), about 1 h 45 with its two builds, at most 2 h 30, not next to a performance run. No reboot is expected and none is approved. It gives a pull request to openzfs/zfs, not opened yet ("zio_crypt: check ZIL and dnode layouts before using them", fork branch `zio-crypt-layout-checks`), the kernel run its head lacks: the fix and its unit test, built twice (debug and release), and on each module the unit test, an encrypted ZIL replay and the ZTS tests that write and read encrypted ZIL and dnode blocks. **This VM has two extra disks, sdc and sdd, that this task must not touch** (see "VM rules"). Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-zcl/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. The expected files of step 1 and the kit of step 2 contain all three. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `F`, `t`, `g`).

## Plan and durations

| step | what | about |
|---|---|---|
| 0 | state of this VM | 3 min |
| 1 | two checkouts of one commit, their checks | 3 min |
| 2 | the kit | 3 min |
| 3 | both builds at once, `nice make -j12` each: `dbg` with `--enable-debug`, `rel` without | 15 min |
| 4 | the kernel-log follower, then the unit test on both trees | 3 min |
| 5 | debug module: `zil.sh` (three rounds), then the ZTS selection twice | 35 min |
| 6 | release module: the same | 35 min |
| 7 | end and cleanup | 5 min |

## Why

**The change.** Text 46 of main's pull request pack changes `module/zfs/zio_crypt.c`, common code built into the Linux and FreeBSD modules and into libzpool:
- `zio_crypt_init_uios_zil()` walks each encrypted ZIL block to tell what is encrypted from what is only authenticated, before the cipher checks the MAC. Its first pass now checks that `zc_nused` and every record length stay within the block, and that `TX_WRITE` and `TX_CLONE_RANGE` records carry their fixed part, and fails with `ECKSUM` otherwise. On master these lengths were bounded only by `ASSERT`s, which a release build compiles out.
- `zio_crypt_init_uios_dnode()` does the same for each dnode block of an encrypted dataset: every dnode must fit in the block with its extra slots, `dn_nblkptr` must be at most 3, and a spill pointer must follow the bonus buffer.
- `zio_do_crypt_data()` tears the uios down only once they exist.

These functions run in both directions: on every write of an encrypted log block or dnode block (`zio_encrypt()`) and on every read of one with the key loaded (`zio_decrypt()`): sync writes and their replay, every dnode block an encrypted dataset writes, every mount, raw receive followed by a key load, thorough scrub, and zdb's decryption in libzpool. A valid block refused by mistake would show as EIO and an authentication ereport on a read, a replay that loses records, or a failed `VERIFY0()` in `zio_encrypt()` on a write. That is what this task looks for. The other side, malformed blocks being refused, needs blocks crafted with matching checksums and MACs: the second commit's unit test `tests/unit/test_zio_crypt.c` makes them in user space (13 cases, `make unit T=zio_crypt`).

**Earlier VM run.** TASK-1121-JBIS (21/11, this VM, then with 8 CPUs) ran the fix commit `ee99c44e161` alone (its part C) on a debug module, srcversion `35C063D87AB73EE899D48B6`: its `zil.sh` replayed an encrypted log of 39 records (`REPLAY-OK`, 0 authentication ereports), `-T zfs_load-key` 7/7, and 30/30 of its encrypted send/receive, ZIL and import tests, kernel log clean. The pull request's VM line cites that run. Nothing else has run this change in a kernel: no release module, and not the head `e472e795e4c`, whose second commit (the unit test, `tests/unit` only) came after JBIS. The unit test has run only in main's container (Ubuntu 24.04, gcc 13.3, user space).

**This task,** on the head as it will be opened, built twice:
- the unit test 3 times and `make -k unit` once, on each tree (the first runs on Debian 13's compiler, and the first outside main's container in a release build);
- `zil.sh`: JBIS's script, extended. Each of its three rounds (`aes-256-gcm`, `aes-256-ccm`, `aes-128-gcm`) uses a file system with 1K dnodes (`dnodesize=auto`, so every dnode has an extra slot) and SA xattrs, one of them large enough to go to a spill block. After JBIS's synced records, it makes 400 creates and writes that the unmount commits together, so that log blocks hold many records each beside the one-record blocks of the synced writes. After the replay it exports and imports the pool once more and compares again: that second import reads back, and decrypts, the dnode blocks the replay wrote;
- the ZTS selection below (97 tests of 31 groups), twice on each module.

The module sources are those of JBIS's `zc` tree (the second commit touches `tests/unit` only), and a srcversion hashes the module's sources, not its build flags: both builds are expected to show JBIS's `35C063D87AB73EE899D48B6`.

**Release.** In a release module the `ASSERT`s that bounded master's ZIL walk are gone, and the new checks (plain conditions, not assertions) are all that bounds both walks; the second pass's new `ASSERT`s exist in the debug module only. On the valid blocks these tests write, both modules must give the same results. Which build a tree holds is told by its `config.log` and configure's assertion line; which module is loaded, by its load line ("(DEBUG mode)" or not) and `zfs_flags` (0 on a release module). Do not count `ZFS_DEBUG` in `zfs_config.h`: every `zfs_config.h` defines it in its in-kernel block (TASK-1123-CCM3).

**Not in this task:**
- malformed blocks in the kernel (a fail-before): they need crafted blocks whose checksums and MACs match, and master's module stops at an `ASSERT` (debug) or loops and reads past the block (release); JBIS left them out for that reason, and the unit test covers them in user space;
- the `slog` group: none of its tests encrypts. `zil.sh` is the encrypted counterpart of its replay tests, and `block_cloning_replay_encrypted` the only ZTS test that replays an encrypted log;
- `rsend/send_leak_keymaps` and `cli_root/zfs_mount/zfs_mount_remount`: each imports a pool by name without `-d`, which reads the labels of every block device of the VM, sdc and sdd included;
- `send_xdr_encoding/xdr_bookmark_raw_with_write` and `xdr_resume_bookmark_raw_with_write`: `tests/test-runner/bin/zts-report.py.in` lists both as known failures (issue 18491);
- `pam` (needs pamtester; it tests the PAM module) and `crypto` (user-space known-answer tests of the ICP);
- FreeBSD: no FreeBSD VM here; the pull request's CI builds it.

## What is tested (fork https://github.com/tigerblue77/zfs)

One commit, fetched by its full SHA into two directories, one per build:

| tree | branch | head | tree hash | `HEAD~1` (the fix) | `HEAD~2` | build |
|---|---|---|---|---|---|---|
| dbg | zio-crypt-layout-checks | e472e795e4c35a13749d728d428047cb56c87893 | 1b636ab9032b621e9d4eb5656668cba97c268075 | ee99c44e161dca6a9767c8a232d3b6425267001f | upstream master `1b1aa0e91b694798085a2771c4eef40e93b9b2db` | `./configure --enable-debug` |
| rel | the same | the same | the same | the same | the same | `./configure` |

## The ZTS selection

`zcl.run` (step 2) holds 97 tests of `common.run` and `linux.run`, unchanged, in their 31 groups, each group with its runfile's settings and the tags `zcl` and one of the five below. Every one of them creates encrypted datasets and writes, reads, sends, receives, imports, scrubs or replays them. Main read their bodies at this head; the upstream CI's durations for them add up to 7 to 9 minutes.

| tag | groups (tests) | why |
|---|---|---|
| `zcl_zil` | `block_cloning` (2) | `block_cloning_replay_encrypted`: an encrypted log with `TX_WRITE` and `TX_CLONE_RANGE` records (the two record types the ZIL check sizes on their own), claimed at the import and decrypted at the replay; `block_cloning_cross_enc_dataset`: cloning between encrypted datasets |
| `zcl_send` | `rsend` (16), `cli_root/zfs_receive` (11), `cli_root/zfs_send` (3), `send_xdr_encoding` (6), `cli_root/zfs_rewrite` (1) | encrypted and raw send and receive. A raw stream carries the dnode blocks as they were written, and the receiving side decrypts them once its key is loaded: spill blocks and SA xattrs (`send_raw_spill_block`, `send_encrypted_files`, `send_raw_ashift`), dnode sizes and reallocation (`send_realloc_encrypted_files`), freed objects, raw volumes and a raw deduplicated stream made by an older version (`send-wR_encrypted_zvol`, `recv_dedup_encrypted_zvol`), corrective receive, raw streams in XDR encoding |
| `zcl_key` | `cli_root/zfs_load-key` (7), `zfs_change-key` (10), `zfs_unload-key` (3), `zfs_create` (2), `zpool_create` (2), `zfs_mount`, `zfs_unmount`, `zfs_clone`, `zfs_promote`, `zfs_diff`, `zfs_set`, `zpool_split` (1 each), `zfs_rename` (2), `userquota` (4), `channel_program/lua_core` (1) | key load, unload and change cycles, each mount reading and decrypting the dataset's dnode blocks; every cipher and key format; user and object accounting over encrypted dnodes |
| `zcl_import` | `cli_root/zpool_import` (5), `zpool_scrub` (2), `zpool_status` (3), `cli_root/zdb` (2), `fault` (1) | encrypted pools imported, including the `cryptv0` and `missing_ivset` images older versions wrote (`zpool_import_errata3`, `errata4`); `zpool_scrub_thorough`, whose scrub decrypts every block it reads; error reports on encrypted datasets; zdb's decryption in libzpool, which builds the same `zio_crypt.c`; `decrypt_fault`, the `ECKSUM` to EIO and authentication ereport path that the new checks use |
| `zcl_io` | `direct` (1), `compression` (2), `no_space` (1), `reservation` (2), `l2arc` (1) | Direct I/O, L2ARC (persistent too) and gang blocks on encrypted datasets; encrypted volumes |

Per pass: the 97 tests and the setups and cleanups of 30 groups (`cli_root/zdb` has none), 157 `Test:` lines, all PASS expected. The `fault` group's setup starts a ZED of its own and its cleanup stops it.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- **sdc and sdd.** This VM has two 4 GiB disks, `sdc` and `sdd`, besides its root and `/var/tmp` disks; `sdc` carries a ZFS label that is not this task's. Touch neither: no command of this task names them (the `lsblk -d` of steps 0, 1 and 7 apart, which reads sysfs only), and none reads, probes, labels, wipes, imports or mounts them. ZTS runs with `DISKS` unset (`runs.sh` removes it from the runner's environment), on its own file vdevs and loop devices. Every import of this task names its directory with `-d` (`zil.sh`, and ZTS: step 5 checks its logs for any other). Never run `zpool import` yourself, `zpool import -a`, `zdb -l`, `blkid -p` or `wipefs`, and never `zfs-tests.sh -x`.
- A pool this task did not create is a stop: report it, and leave it as it is (no export, no destroy, no import). This task creates only the ZTS pools and `zclz` (`zil.sh`, `cachefile=none`).
- **One module and one series at a time.** The two builds run together, before any module is loaded.
- **Pre-run checks:** `sh $L/check.sh <tree>` (step 2), with `<tree>` the tree whose module is loaded, prints `CHECK-OK <tree>` when: `/sys/module/zfs/srcversion` equals `$L/sv-<tree>`; `zfs_flags` is not 0 for `dbg` and is 0 for `rel` (the srcversions being equal, this is what tells the two modules apart); `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool; the first line of `zinject` is `No handlers registered.`; `sudo -n losetup -a` shows nothing; no `zed` runs; none of `/var/tmp/file-vdev*`, `/var/tmp/tunable-*`, `/var/tmp/testdir`, `/var/tmp/zed` and `$D/zil-*/dev/vdev` exists; and `/etc/zfs/zpool.cache` is absent or empty (the module truncates it when the last pool goes). Otherwise it prints `CHECK-FAIL` with what it found. It runs before `zil.sh`, before each ZTS run (`runs.sh` calls it and writes a `STOP` line on a failure) and at the end of each module's part. A `CHECK-FAIL` is a stop.
- **Every build, every `zil.sh` and every ZTS series runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs.** A pair of builds still going after 45 minutes; `zil.sh` without `ZIL-DONE` or a `FAILED:` line after 6 minutes; a ZTS run whose console log `$L/zts-<tag>-<n>.log` has not grown for 25 minutes (the longest timeout of the selection is 20 minutes); an unload not back within 2 minutes. Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|txg|z_|zfs|zpool|zdb|zed|ksh|make|cc1'`; `/proc/<tid>/stack` of each thread in state D, read in the background (`(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`); `tail -n 200 /proc/spl/kstat/zfs/dbgmsg`; `sudo -n dmesg | tail -n 150`. Then stop and report. **Do not reboot.**
- **Kernel log.** A follower keeps it from step 4 on (a failing test's callback runs `dmesg -c`, so `dmesg` alone can lose lines). After the unit tests and after each module's part, check the follower's lines since the part's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock|KFENCE|KASAN|segfault|WARNING|zio_crypt`. None is expected. Attribute any line to what ran then, from the `task-1123-zcl: ...` marks and the `ZTS run ...` lines that `-K` writes. Any line but a `WARNING` one is a finding: quote it with the 10 lines before it, capture as for a hang, and stop. A `WARNING` line with no `Call Trace` after it is quoted and counted, and the task goes on.
- **Failures.** Every test of this task's selection, the unit test and `zil.sh` exercise encryption, so a failure among them is a stop for main's attention once the run it belongs to has completed: `runs.sh` itself stops its series with a `HALT after <tag>-<n>` line when a run returns non-zero or shows a `FAIL` or `KILLED` line, setups and cleanups included. Then: quote the run's non-PASS `Test:` lines and, for each, `grep -n -B 20 -m 1 -E 'ERROR|FAIL'` of its own output file (`R` the run's `dir=`: `f=$(find "$R" -path "*/<test>*" -type f \( -name merged -o -name stdout \) | sort | head -n 1)`; JBIS found a `stdout` and no `merged` on this VM), and say whether `zts-report.py.in` lists it as known. Do the end of the part (the checks, the kernel log, the unload if `check.sh` is OK), skip what remains up to step 7, do step 7 without deleting the trees, report and wait for main. A failure of something that is not about encryption (another binary of `make -k unit`) is quoted and reported, and the task goes on. A `SKIP` is quoted and the task goes on.
- A module that will not unload: capture as for a hang, stop and report.
- **Deadline.** No `zil.sh` and no ZTS series starts later than 2 h 05 after the start of step 0. If that time comes, let what runs finish, go to step 7, and report what ran.
- Run `sudo -n fstrim -av` after deleting the trees.

## Step 0: state

Check and quote:
- `date -u` (the start of the task), `hostname` (CHSONVTST04 is VM 107; another VM is a stop: this task needs VM 107's CPUs), `uname -r` (6.12.118 expected; another 6.12 kernel is recorded, not a stop), `uptime -s`, `nproc` (32), `free -m` (about 32 000 MiB total);
- `df -h / /var/tmp`: at least 25 GiB free on `/var/tmp` (two built trees and ZTS's three 4 GiB sparse vdevs), else stop and report;
- `lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINTS`: quote it. Expected the root and `/var/tmp` disks, `sdc` and `sdd` (4G each, not mounted) and no loop device. A loop device, or `sdc` or `sdd` mounted, is a stop;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git python3 fio setfattr getfattr`, `sudo -n true && echo sudo-ok`. All must be there (`zil.sh` uses `setfattr` and `getfattr`; ZTS lists all of them); a missing one is a stop (install nothing);
- `echo "login DISKS=[${DISKS-unset}]"; sudo -n -u zts env -u DISKS sh -c 'echo "zts DISKS=[${DISKS-unset}]"'`: quote both; the second must read `zts DISKS=[unset]` (that is how `runs.sh` starts ZTS);
- `pgrep -a -x zed; pgrep -a -x lt-zed`: must be empty (a running zed is a stop: do not start or stop one);
- `pgrep -af 'dmesg --follow'`: quote it (one left by an earlier task stays as it is);
- `sudo -n losetup -a`: empty;
- `ls -d /var/tmp/task-1123-zcl /var/log/task-1123-zcl /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/testdir /var/tmp/zed 2>&1`: none exists;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0. Above 0 is a stop: quote its `ls -l`, do not read it;
- `sysctl kernel.panic_on_oops kernel.hung_task_panic`: quote them; change nothing;
- `ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|qemu-system-x86'`: nothing (a build, a test or a guest of another task is a stop);
- udev: `ls -l /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id 2>&1`. JBIS found `/etc/udev/rules.d/60-zvol.rules` and `/usr/lib/udev/zvol_id` as regular files: H=no, and this task runs no `zfs-helpers.sh`. If none of the three exists (a symlink whose target is gone counts as absent), note H=yes: step 5 installs `dbg`'s helpers and step 7 removes them (the selection's volume tests need `/dev/zvol` links).

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D && ls -A /var/tmp > $L/vartmp-0.txt && lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINTS > $L/lsblk-0.txt
for t in dbg rel; do mkdir $D/$t && cd $D/$t && git init -q . && git fetch -q --depth 3 $F e472e795e4c35a13749d728d428047cb56c87893 && git checkout -q FETCH_HEAD && echo "FETCH-OK $t"; done
```

Expected `FETCH-OK dbg` and `FETCH-OK rel`. If a fetch fails, wait 1 minute and run that tree's `git fetch -q --depth 3 $F e472e795e4c35a13749d728d428047cb56c87893 && git checkout -q FETCH_HEAD` once more, in its directory. If it fails again, fetch the branch by name instead, in the same directory (`git fetch -q --depth 3 $F zio-crypt-layout-checks && git checkout -q FETCH_HEAD`), say so, and go on: the checks below stop on a branch that has moved.

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl
for t in dbg rel; do cd $D/$t &&
  echo "$t $(git rev-parse HEAD) $(git rev-parse 'HEAD^{tree}') $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git status --porcelain | wc -l)" &&
  git log --format="$t %H %an | %s" HEAD~2..HEAD && echo "$t:$(git diff --shortstat HEAD~2 HEAD)" &&
  echo "$t $(git diff --name-only HEAD~2 HEAD~1 | paste -sd ' ') | $(git diff --name-only HEAD~1 HEAD | paste -sd ' ')"
done > $L/tree.txt
cat > $L/tree.expected <<'EOF'
dbg e472e795e4c35a13749d728d428047cb56c87893 1b636ab9032b621e9d4eb5656668cba97c268075 ee99c44e161dca6a9767c8a232d3b6425267001f 1b1aa0e91b694798085a2771c4eef40e93b9b2db 0
dbg e472e795e4c35a13749d728d428047cb56c87893 Tigerblue77 | unit/zio_crypt: decrypt ZIL and dnode blocks that do not fit
dbg ee99c44e161dca6a9767c8a232d3b6425267001f Tigerblue77 | zio_crypt: check ZIL and dnode layouts before using them
dbg: 5 files changed, 507 insertions(+), 9 deletions(-)
dbg module/zfs/zio_crypt.c | tests/unit/.gitignore tests/unit/Makefile.am tests/unit/mock_dmu.c tests/unit/test_zio_crypt.c
rel e472e795e4c35a13749d728d428047cb56c87893 1b636ab9032b621e9d4eb5656668cba97c268075 ee99c44e161dca6a9767c8a232d3b6425267001f 1b1aa0e91b694798085a2771c4eef40e93b9b2db 0
rel e472e795e4c35a13749d728d428047cb56c87893 Tigerblue77 | unit/zio_crypt: decrypt ZIL and dnode blocks that do not fit
rel ee99c44e161dca6a9767c8a232d3b6425267001f Tigerblue77 | zio_crypt: check ZIL and dnode layouts before using them
rel: 5 files changed, 507 insertions(+), 9 deletions(-)
rel module/zfs/zio_crypt.c | tests/unit/.gitignore tests/unit/Makefile.am tests/unit/mock_dmu.c tests/unit/test_zio_crypt.c
EOF
diff $L/tree.expected $L/tree.txt && echo TREE-OK
cat > $L/sha.txt <<'EOF'
3ea64af37ef200a28e835699431a6b6dd368c26967c59e5c2eef12bb463e6434  dbg/module/zfs/zio_crypt.c
7a45173cb1905eade08b9f08c46fef1eda9ff8aeb984d48d6b2fcc1f3bdf7aa4  dbg/tests/unit/test_zio_crypt.c
4b8547a832f2f8b7932f4698a4acdf79abc15e8c62189e15332d47d39c5b78e5  dbg/tests/unit/Makefile.am
a54abbe21114032f1b82fb6e34b1dc177088c6ff159efd9e4cf7cfa355beb0a0  dbg/tests/unit/mock_dmu.c
c704108b5f1175e81621828dab67f4254df8f46e790c44a9aea50d74cf0135b3  dbg/tests/unit/.gitignore
c935d95e9f50a67e323b255da48ce925b9c62e8f69e87974e00085e735fe082e  dbg/tests/runfiles/common.run
a8bc7ae336803669c56bdecf01d9162a37876257cb229b093d9e2f9fb8b81515  dbg/tests/runfiles/linux.run
3ea64af37ef200a28e835699431a6b6dd368c26967c59e5c2eef12bb463e6434  rel/module/zfs/zio_crypt.c
7a45173cb1905eade08b9f08c46fef1eda9ff8aeb984d48d6b2fcc1f3bdf7aa4  rel/tests/unit/test_zio_crypt.c
4b8547a832f2f8b7932f4698a4acdf79abc15e8c62189e15332d47d39c5b78e5  rel/tests/unit/Makefile.am
a54abbe21114032f1b82fb6e34b1dc177088c6ff159efd9e4cf7cfa355beb0a0  rel/tests/unit/mock_dmu.c
c704108b5f1175e81621828dab67f4254df8f46e790c44a9aea50d74cf0135b3  rel/tests/unit/.gitignore
c935d95e9f50a67e323b255da48ce925b9c62e8f69e87974e00085e735fe082e  rel/tests/runfiles/common.run
a8bc7ae336803669c56bdecf01d9162a37876257cb229b093d9e2f9fb8b81515  rel/tests/runfiles/linux.run
EOF
cd $D && sha256sum -c --quiet $L/sha.txt && echo SHA-OK
for t in dbg rel; do f=$D/$t/module/zfs/zio_crypt.c; echo "$t $(grep -c 'return (SET_ERROR(ECKSUM));' $f) $(grep -c 'the first pass checked these' $f) $(grep -c '%D%/test_zio_crypt$' $D/$t/tests/unit/Makefile.am) $(git -C $D/$t ls-tree HEAD tests/unit/test_zio_crypt.c | cut -c1-6)"; done
```

Expected `TREE-OK`, `SHA-OK` (the fix's `zio_crypt.c` is the file JBIS's `zc` tree built, its sum is that of git's blob at `ee99c44e161` too; both runfiles are master's), then `dbg 5 2 1 100644` and `rel 5 2 1 100644`: five `ECKSUM` returns in `zio_crypt.c` (master has one), the two second-pass comments of the fix (master has none), `test_zio_crypt` in `UNIT_TESTS` (so `make unit` runs it), and the test's source.

Any other difference: stop and report the outputs as they are. Edit no file of the trees.

## Step 2: the kit

Write each file below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > /var/log/task-1123-zcl/<file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), to `check.sh`, `runs.sh`, `zil.sh` and `zcl.run` in that order. Then:

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl
printf '%s\n' "3dd5e6d513e92bf04257063828831302ca5a256f800a71d5b70746494841983c  $L/check.sh" "4c8310deffd739247a42e6c0140db47a6e1b9950fb6487a657321b848f533b61  $L/runs.sh" "f5168b83db0a1170fd20bdcc1abf833d9f6541f457427bacf19807fc30941612  $L/zil.sh" "73040fa64589e0576abb9f78794d71c8a5c2e22236ab3600cf7847a52e6940cd  $L/zcl.run" | sha256sum -c && sh -n $L/check.sh && sh -n $L/runs.sh && sh -n $L/zil.sh && echo KIT-OK
python3 - $L/zcl.run $D/dbg/tests/zfs-tests <<'EOF'
import ast, configparser, os, sys
c = configparser.RawConfigParser()
c.read(sys.argv[1])
groups = tests = 0
for s in c.sections():
    groups += 1
    for t in ast.literal_eval(c.get(s, 'tests')):
        tests += 1
        f = os.path.join(sys.argv[2], s, t + '.ksh')
        if not os.access(f, os.X_OK):
            print('MISSING', f)
print('RUNFILE', groups, 'groups', tests, 'tests')
EOF
```

Expected four `OK` lines, `KIT-OK`, then `RUNFILE 31 groups 97 tests` with no `MISSING` line (every test of `zcl.run` is an executable `.ksh` of the tree). If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

**`check.sh <tree>`**, the pre-run checks of the VM rules, run as the login user:

```sh
#!/bin/sh
# TASK-1123-ZCL pre-run checks, run as the login user: check.sh <tree>
# Prints CHECK-OK <tree>, or CHECK-FAIL <tree> and what it found.
D=/var/tmp/task-1123-zcl
L=/var/log/task-1123-zcl
t=$1
T=$D/$t
sv=$(cat /sys/module/zfs/srcversion 2>&1)
fl=$(cat /sys/module/zfs/parameters/zfs_flags 2>&1)
case "$t:$fl" in
dbg:0) mode=wrong ;;
dbg:[0-9]* | dbg:-[0-9]* | rel:0) mode=ok ;;
*) mode=wrong ;;
esac
pools=$(timeout 30 sudo -n $T/zpool list -H -o name 2>&1)
inj=$(timeout 30 sudo -n $T/zinject 2>&1 | head -n 1)
loops=$(sudo -n losetup -a 2>&1)
zed=$(pgrep -a -x zed; pgrep -a -x lt-zed)
left=$(ls -d /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/testdir \
    /var/tmp/zed $D/zil-*/dev/vdev 2>/dev/null)
if [ "$sv" = "$(cat $L/sv-$t)" ] && [ "$mode" = ok ] && [ -z "$pools" ] &&
    [ "$inj" = "No handlers registered." ] && [ -z "$loops" ] &&
    [ -z "$zed" ] && [ -z "$left" ] && [ ! -s /etc/zfs/zpool.cache ]; then
    echo "CHECK-OK $t"
    exit 0
fi
echo "CHECK-FAIL $t: srcversion=[$sv] zfs_flags=[$fl] pools=[$pools] zinject=[$inj] loops=[$loops] zed=[$zed] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]"
exit 1
```

**`runs.sh <tree> <tag> <runs> <selection>`** is TASK-1123-REL4's runner with this task's `D` and `L`, the checks of `check.sh`, `DISKS` removed from the runner's environment, and a halt after a run that does not pass. It runs as the login user, from `$D/<tree>`. It runs `zfs-tests.sh -vKO <selection>` `<runs>` times, one at a time, each after `check.sh` and after a kernel mark `task-1123-zcl: <tag>-<n> start`. Each run's console goes to `$L/zts-<tag>-<n>.log`, and `$L/runs.txt` gets a line `<tag>-<n> rc=<rc> <seconds> s dir=<result dir>`, the run's `DISKS:` line, then its `Test:` lines (path from `functional/`, duration, result); then `HALT after <tag>-<n>` if the run returned non-zero or has a `FAIL` or `KILLED` line, or `DONE <tag>` after the last run.

```sh
#!/bin/sh
# TASK-1123-ZCL ZTS runner, run as the login user:
# runs.sh <tree> <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1123-zcl
L=/var/log/task-1123-zcl
tree=$1
tag=$2
runs=$3
shift 3
out=$L/runs.txt
cd $D/$tree || exit 1
echo "START $tag $tree $(date -u +%FT%TZ) $*" >> $out
n=1
while [ $n -le $runs ]; do
    c=$(sh $L/check.sh $tree)
    if [ $? -ne 0 ]; then
        echo "STOP before $tag-$n: $c" >> $out
        exit 1
    fi
    echo "task-1123-zcl: $tag-$n start" | sudo -n tee /dev/kmsg > /dev/null
    s=$(date +%s)
    sudo -n -u zts env -u DISKS ./scripts/zfs-tests.sh -vKO "$@" \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc $(( $(date +%s) - s )) s dir=$R" >> $out
    grep -m 1 -E '^DISKS:' $L/zts-$tag-$n.log | sed 's/^/    /' >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    if [ $rc -ne 0 ] ||
        grep -qE 'Test( \([^)]*\))?: .* \[(FAIL|KILLED)\]$' \
        $L/zts-$tag-$n.log; then
        echo "HALT after $tag-$n: rc=$rc or a test that did not pass" >> $out
        exit 2
    fi
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

**`zil.sh <tree>`** runs as root with the tree's module loaded (step 5c). Each of its three rounds (`aes-256-gcm`, `aes-256-ccm`, `aes-128-gcm`) creates pool `zclz` on a 512 MiB file under `$D/zil-<tree>-<n>/dev` (`cachefile=none`) and an encrypted file system with a raw key in a file, `dnodesize=auto` and `xattr=sa`. Before the freeze: a file with two SA xattrs, the second (3000 bytes) too large for a 1K dnode's bonus buffer, so it goes to a spill block; one synced byte so that a ZIL header exists. Then `zpool freeze` (from then on only the ZIL keeps changes) and JBIS's records: create, rename, symlink, link, mkdir, rmdir, writes (small ones copied into the log, 128K ones logged as indirect), truncate, setattr and remove, each synced; then 400 creates with a small write each, not synced, which the unmount commits together. It records names, types, modes, link counts, checksums, one mtime and the two xattrs, unmounts and exports, counts the ZIL records with `zdb -e -iv`, imports with `-f -l` (the import claims the log; the mount, with the key loaded from its file, decrypts and replays it) and compares (`REPLAY-OK` or `REPLAY-DIFF`); then exports and imports once more, which reads the files' dnode blocks, written by the replay, back from disk and decrypts them, and compares again (`REREAD-OK` or `REREAD-DIFF`). It prints the file system's properties, the authentication ereports of the round, the pool's errors and the kernel lines of the round, destroys the pool and removes its vdev file. A step that fails prints `FAILED: <step>` and ends the script; `ZIL-DONE` ends a complete run.

```sh
#!/bin/sh
# TASK-1123-ZCL, run as root with the tree's module loaded: zil.sh <tree>
# TASK-1121-JBIS's zil.sh, three times, on file systems with 1K dnodes and
# SA xattrs.  Each round freezes a new pool, then makes an encrypted file
# system log create, rename, symlink, link, mkdir, rmdir, write, truncate,
# setattr and remove records: some synced one at a time, then 400 creates
# and writes that the unmount commits together.  It imports the pool
# again, which claims, decrypts and replays the log, and compares the files
# with what was written; then it exports and imports the pool once more,
# which reads back and decrypts the dnode blocks the replay wrote, and
# compares again.
D=/var/tmp/task-1123-zcl
L=/var/log/task-1123-zcl
t=$1
Z=$D/$t
p=zclz
pat='VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|KASAN|segfault|zio_crypt'
[ -x $Z/zpool ] || exit 1
fail() {
    echo "FAILED: $*" >> $o
    cat $o
    exit 1
}
state() {
    (cd $m && find . -printf '%p %y %m %n %l\n' | sort &&
        find . -type f -exec sha256sum {} + | sort -k 2 &&
        stat -c '%n %Y' small &&
        echo "x user.small $(getfattr --only-values -n user.small x)" &&
        echo "x user.large $(getfattr --only-values -n user.large x |
            sha256sum | cut -c1-64)")
}
round() {
    n=$1
    c=$2
    v=$D/zil-$t-$n
    o=$L/zil-$t-$n.txt
    m=$v/mnt/fs
    echo "== round $t-$n $c $(date -u +%T)"
    mkdir $v $v/dev || exit 1
    : > $o
    n0=$(dmesg | wc -l)
    a0=$($Z/zpool events | grep -c authentication)
    truncate -s 512M $v/dev/vdev || fail truncate
    head -c 32 /dev/urandom > $v/key || fail key
    $Z/zpool create -o cachefile=none -O mountpoint=$v/mnt $p $v/dev/vdev ||
        fail zpool create
    $Z/zfs create -o encryption=$c -o keyformat=raw \
        -o keylocation=file://$v/key -o dnodesize=auto -o xattr=sa $p/fs ||
        fail zfs create
    # before the freeze: two SA xattrs, the second too large for the bonus
    # buffer of a 1K dnode, so that it goes to a spill block
    x=$(head -c 1500 /dev/urandom | od -An -tx1 | tr -d ' \n')
    touch $m/x && setfattr -n user.small -v zcl $m/x &&
        setfattr -n user.large -v "$x" $m/x || fail setfattr
    # a ZIL header must exist before the freeze for the records to be kept
    dd if=/dev/zero of=$m/sync conv=fdatasync,fsync bs=1 count=1 \
        2>/dev/null && $Z/zpool freeze $p || fail freeze
    touch $m/a && mv $m/a $m/b && ln -s b $m/c && ln $m/b $m/d &&
        mkdir $m/e $m/f && rmdir $m/f &&
        dd if=/dev/urandom of=$m/small bs=1k count=3 oflag=sync \
        2>/dev/null &&
        dd if=/dev/urandom of=$m/large bs=128k count=16 oflag=sync \
        2>/dev/null &&
        dd if=/dev/urandom of=$m/e/trunc bs=8k count=4 oflag=sync \
        2>/dev/null &&
        truncate -s 5000 $m/e/trunc && chmod 640 $m/small &&
        touch -m -t 202001020304 $m/small && rm $m/d || fail synced records
    # 400 creates and small writes, logged without a sync: the unmount
    # commits them together, many records to a log block
    mkdir $m/many || fail mkdir many
    i=1
    while [ $i -le 400 ]; do
        echo "zcl $t $n $i" > $m/many/f$i || fail write many/f$i
        i=$((i + 1))
    done
    state > $v/before
    $Z/zfs unmount $p/fs && $Z/zpool export $p || fail unmount or export
    $Z/zdb -e -p $v/dev -iv $p/fs > $v/zdb-il.txt 2>&1
    echo "ZIL records before the import:" \
        "$(grep ' len ' $v/zdb-il.txt | grep -oE 'TX_[A-Z_]+' | sort |
        uniq -c | tr -s ' ' | tr '\n' ';')" >> $o
    $Z/zpool import -f -l -o cachefile=none -d $v/dev $p || fail import 1
    state > $v/after
    if cmp -s $v/before $v/after; then
        echo "REPLAY-OK: $(wc -l < $v/after) lines of names, types, modes," \
            "links, sums, xattrs and mtime identical" >> $o
    else
        echo "REPLAY-DIFF:" >> $o
        diff $v/before $v/after >> $o
    fi
    $Z/zpool export $p &&
        $Z/zpool import -f -l -o cachefile=none -d $v/dev $p ||
        fail import 2
    state > $v/reread
    if cmp -s $v/before $v/reread; then
        echo "REREAD-OK: the same $(wc -l < $v/reread) lines after a" \
            "second export and import" >> $o
    else
        echo "REREAD-DIFF:" >> $o
        diff $v/before $v/reread >> $o
    fi
    echo "$($Z/zfs get -H -o value encryption,dnodesize,xattr,mounted \
        $p/fs | tr '\n' ' ')authentication ereports:" \
        "$(( $($Z/zpool events | grep -c authentication) - a0 ))" >> $o
    $Z/zpool status -v $p | sed -n '/^errors:/,$p' >> $o
    $Z/zpool destroy $p && rm $v/dev/vdev || echo "destroy failed" >> $o
    dmesg | tail -n +$((n0 + 1)) > $L/zil-$t-$n-dmesg.txt
    echo "kernel log: $(wc -l < $L/zil-$t-$n-dmesg.txt) new line(s)," \
        "$(grep -cE "$pat" $L/zil-$t-$n-dmesg.txt) matching $pat" >> $o
    cat $o
}
round 1 aes-256-gcm
round 2 aes-256-ccm
round 3 aes-128-gcm
echo ZIL-DONE
```

**`zcl.run`**, the runfile of the selection (see "The ZTS selection"):

```
# TASK-1123-ZCL: tests of common.run and linux.run that write and read
# encrypted dnode and ZIL blocks, under the tag zcl.  The sections keep
# their runfile's settings; only their tags and test lists are ours.

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

[tests/functional/block_cloning]
tests = ['block_cloning_replay_encrypted', 'block_cloning_cross_enc_dataset']
tags = ['functional', 'zcl', 'zcl_zil']

[tests/functional/rsend]
tests = ['send_encrypted_files', 'send_encrypted_freeobjects',
    'send_encrypted_hierarchy', 'send_encrypted_holds',
    'send_encrypted_incremental', 'send_encrypted_props',
    'send_encrypted_truncated_files', 'send_realloc_encrypted_files',
    'send_raw_spill_block', 'send_raw_ashift', 'send_raw_large_blocks',
    'send_raw_inherited_key', 'send_mixed_raw', 'send-wR_encrypted_zvol',
    'recv_dedup_encrypted_zvol', 'rsend-exclude_002_pos']
tags = ['functional', 'zcl', 'zcl_send']

[tests/functional/cli_root/zfs_receive]
tests = ['zfs_receive_from_encrypted', 'zfs_receive_to_encrypted',
    'zfs_receive_raw', 'zfs_receive_raw_incremental', 'zfs_receive_raw_-d',
    'zfs_receive_raw_-b', 'zfs_receive_-wR-encrypted-mix',
    'zfs_receive_corrective', 'zfs_receive_compressed_corrective',
    'zfs_receive_large_block_corrective', 'zfs_receive_zstd_corrective']
tags = ['functional', 'zcl', 'zcl_send']

[tests/functional/cli_root/zfs_send]
tests = ['zfs_send_encrypted', 'zfs_send_encrypted_unloaded', 'zfs_send_raw']
tags = ['functional', 'zcl', 'zcl_send']

[tests/functional/send_xdr_encoding]
tests = ['xdr_raw', 'xdr_bookmark_raw', 'xdr_redacted_received_raw',
    'xdr_replication', 'xdr_resume_raw', 'xdr_resume_bookmark_raw']
tags = ['functional', 'zcl', 'zcl_send']

[tests/functional/cli_root/zfs_rewrite]
tests = ['zfs_rewrite_physical_raw_send']
tags = ['functional', 'zcl', 'zcl_send']

[tests/functional/cli_root/zfs_load-key]
tests = ['zfs_load-key', 'zfs_load-key_all', 'zfs_load-key_file',
    'zfs_load-key_https', 'zfs_load-key_location', 'zfs_load-key_noop',
    'zfs_load-key_recursive']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_change-key]
tests = ['zfs_change-key', 'zfs_change-key_child', 'zfs_change-key_format',
    'zfs_change-key_inherit', 'zfs_change-key_load', 'zfs_change-key_location',
    'zfs_change-key_pbkdf2iters', 'zfs_change-key_clones',
    'zfs_change-key_userprop', 'zfs_change-key_mismatch']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_unload-key]
tests = ['zfs_unload-key', 'zfs_unload-key_all', 'zfs_unload-key_recursive']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_create]
tests = ['zfs_create_encrypted', 'zfs_create_crypt_combos']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zpool_create]
tests = ['zpool_create_encrypted', 'zpool_create_crypt_combos']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_mount]
tests = ['zfs_mount_encrypted']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_unmount]
tests = ['zfs_unmount_unload_keys']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_clone]
tests = ['zfs_clone_encrypted']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_promote]
tests = ['zfs_promote_encryptionroot']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_rename]
tests = ['zfs_rename_encrypted_child', 'zfs_rename_to_encrypted']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_diff]
tests = ['zfs_diff_encrypted']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zfs_set]
tests = ['zfs_set_keylocation']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zpool_split]
tests = ['zpool_split_encryption']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/userquota]
tests = ['userspace_encrypted', 'userspace_encrypted_unmounted',
    'userspace_send_encrypted', 'userspace_send_encrypted_changes']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/channel_program/lua_core]
tests = ['tst.encryption']
tags = ['functional', 'zcl', 'zcl_key']

[tests/functional/cli_root/zpool_import]
tests = ['zpool_import_encrypted', 'zpool_import_encrypted_load',
    'zpool_import_encrypted_zplprops', 'zpool_import_errata3',
    'zpool_import_errata4']
timeout = 1200
tags = ['functional', 'zcl', 'zcl_import']

[tests/functional/cli_root/zpool_scrub]
tests = ['zpool_scrub_encrypted_unloaded', 'zpool_scrub_thorough']
tags = ['functional', 'zcl', 'zcl_import']

[tests/functional/cli_root/zpool_status]
tests = ['zpool_status_005_pos', 'zpool_status_010_pos',
    'zpool_status_011_pos']
tags = ['functional', 'zcl', 'zcl_import']

[tests/functional/cli_root/zdb]
tests = ['zdb_encrypted', 'zdb_encrypted_raw']
pre =
post =
timeout = 1200
tags = ['functional', 'zcl', 'zcl_import']

[tests/functional/fault]
tests = ['decrypt_fault']
tags = ['functional', 'zcl', 'zcl_import']

[tests/functional/direct]
tests = ['dio_encryption']
tags = ['functional', 'zcl', 'zcl_io']

[tests/functional/compression]
tests = ['l2arc_encrypted', 'l2arc_encrypted_no_compressed_arc']
tags = ['functional', 'zcl', 'zcl_io']

[tests/functional/no_space]
tests = ['enospc_ganging']
tags = ['functional', 'zcl', 'zcl_io']

[tests/functional/reservation]
tests = ['reservation_023_pos', 'reservation_024_neg']
tags = ['functional', 'zcl', 'zcl_io']

[tests/functional/l2arc]
tests = ['persist_l2arc_002_pos']
tags = ['functional', 'zcl', 'zcl_io']
```

## Step 3: both builds at once (about 15 minutes)

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl
setsid nohup sh -c "date -u +%T; cd $D/dbg && ./autogen.sh && ./configure --enable-debug && nice make -j12; echo build-rc=\$?; date -u +%T" > $L/build-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "date -u +%T; cd $D/rel && ./autogen.sh && ./configure && nice make -j12; echo build-rc=\$?; date -u +%T" > $L/build-rel.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-dbg.log $L/build-rel.log` until there are two lines; each must read `build-rc=0`. A build that fails: quote the last 40 lines of its log, let the other one end, and stop. Then:

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl
for t in dbg rel; do echo "$t $(head -n 1 $L/build-$t.log) $(tail -n 1 $L/build-$t.log) $(grep -c 'warning:' $L/build-$t.log) $(grep 'warning:' $L/build-$t.log | grep -vc objtool)"; done
for t in dbg rel; do
  echo "$t [$(grep -m1 '^  \$ ./configure' $D/$t/config.log | sed 's/ *$//')] $(grep '^DEBUG_ZFS=' $D/$t/config.log) $(grep -m1 -o 'whether assertion support will be enabled\.\.\. [a-z]*' $L/build-$t.log)"
done > $L/builds.txt
cat > $L/builds.expected <<'EOF'
dbg [  $ ./configure --enable-debug] DEBUG_ZFS='_with_debug' whether assertion support will be enabled... yes
rel [  $ ./configure] DEBUG_ZFS='_without_debug' whether assertion support will be enabled... no
EOF
diff $L/builds.expected $L/builds.txt && echo BUILDS-OK
for t in dbg rel; do /usr/sbin/modinfo -F srcversion $D/$t/module/zfs.ko > $L/sv-$t; echo "$t $(cat $L/sv-$t)"; done
```

Expected:
- per tree its start and end times (quote the durations), the count of `warning:` lines and the count outside objtool notes (0 expected; JBIS's debug builds had none). Quote every `warning:` line that is not an objtool note: the release build has no `-Werror`, so a warning there does not stop it;
- `BUILDS-OK`: `dbg` configured with `--enable-debug` (`_with_debug`, assertions `yes`), `rel` with a bare `./configure` (`_without_debug`, assertions `no`). Quote `$L/builds.txt`. A `rel` tree with `--enable-debug`, `_with_debug` or `yes` is not a release build: stop and report;
- `dbg 35C063D87AB73EE899D48B6` and `rel 35C063D87AB73EE899D48B6` (JBIS's `zc` build; see "Why"). A different value, or two different values, is recorded, not a stop. Master's module, `371A65E8D86145973554A8B` (JBIS's `fb`), is a stop: the fix would not be in the module.

## Step 4: the kernel-log follower, then the unit test on both trees

Start the follower once; it runs until step 7:

```sh
L=/var/log/task-1123-zcl
setsid nohup sudo -n sh -c "echo \$\$ > $L/follow.pid; exec dmesg --follow > $L/dmesg-follow.txt" > /dev/null 2>&1 < /dev/null &
sleep 2; cat $L/follow.pid; pgrep -af 'dmesg --follow'
echo "task-1123-zcl: unit start" | sudo -n tee /dev/kmsg > /dev/null
```

Then the unit test, user space only (no module is loaded), 3 times per tree, and every unit test binary once:

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl
for t in dbg rel; do cd $D/$t || break
  for k in 1 2 3; do make unit T=zio_crypt > $L/unit-$t-$k.txt 2>&1; echo "$t-$k rc=$? ok=$(grep -cF '[ OK    ]' $L/unit-$t-$k.txt) $(grep -h 'tests successful' $L/unit-$t-$k.txt)"; done
  make -k unit > $L/unit-$t-all.txt 2>&1; echo "$t-all rc=$? binaries=$(grep -c 'UNITTEST' $L/unit-$t-all.txt) full=$(grep -c '(100%) tests successful' $L/unit-$t-all.txt)"
done | tee $L/unit.txt
grep -E '^zio_crypt\.|\[ [A-Z]+ +\]' $L/unit-dbg-1.txt
for t in dbg rel; do grep -E 'UNITTEST|tests successful' $L/unit-$t-all.txt; done
sed -n '/task-1123-zcl: unit start/,$p' $L/dmesg-follow.txt | grep -E 'segfault|traps:|VERIFY|ASSERT|PANIC'
```

`T=zio_crypt` is the name `make unit` takes (it looks for `test_<T>`; `T=test_zio_crypt` would find nothing). Expected:
- six lines `<tree>-<k> rc=0 ok=13 13 of 13 (100%) tests successful, 0 (0%) test skipped.`;
- `dbg-all rc=0 binaries=12 full=12` and the same for `rel`: the eleven binaries of master and `test_zio_crypt` (13). JBIS's `make unit` on the same master printed, besides `test_lzjb`'s 4, `N of N` with N = 59, 9, 8, 7, 7, 3, 120, 2, 3, 12 and 1;
- the four tests of `test_zio_crypt` with their 13 cases, each `[ OK    ]`: `zio_crypt.zil_valid` (`nused=full`, `nused=partial`), `zio_crypt.zil_malformed` (`layout=` `nused_below_header`, `nused_past_block`, `header_past_nused`, `reclen_zero`, `reclen_past_nused`, `write_short`, `clone_range_short`), `zio_crypt.dnode_valid`, `zio_crypt.dnode_malformed` (`layout=` `extra_slots_past_block`, `nblkptr_too_large`, `spill_over_blkptrs`). Quote them for `dbg` run 1, then only what differs elsewhere;
- no kernel line (each case's buffer ends at an inaccessible page: a fault there would show as a `segfault` line).

A `test_zio_crypt` run that is not 13 of 13, on either tree, is a stop for main once the loop has ended: quote its file in full, then step 7 without deleting the trees. Another binary of `make -k unit` that fails is not about this change: quote its lines and go on.

## Step 5: the debug module (`dbg`)

### 5a. Load

If step 0 found H=yes: `cd /var/tmp/task-1123-zcl/dbg && sudo -n ./scripts/zfs-helpers.sh -i`, then quote `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id` (symlinks into `$D/dbg`; they serve step 6 too).

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl; t=dbg
lsmod | grep -E '^(zfs|spl) '
sudo -n $D/$t/scripts/zfs.sh; echo "load rc=$?"
echo "$t loaded=$(cat /sys/module/zfs/srcversion) built=$(cat $L/sv-$t) zfs_flags=$(cat /sys/module/zfs/parameters/zfs_flags)"
sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1
echo "task-1123-zcl: $t loaded" | sudo -n tee /dev/kmsg > /dev/null; sh $L/check.sh $t
```

Expected: the `lsmod` line prints nothing; `load rc=0`; `loaded` equals `built`; `zfs_flags` other than 0; the load line with "(DEBUG mode)", expected `ZFS: Loaded module v2.4.99-1 (DEBUG mode), ZFS pool version 5000, ZFS filesystem version 5`; `CHECK-OK dbg`. Without "(DEBUG mode)", or with `zfs_flags=0`, the wrong module is loaded: unload it, stop and report.

### 5b. `zil.sh`

```sh
L=/var/log/task-1123-zcl; t=dbg
setsid nohup sudo -n sh $L/zil.sh $t > $L/zil-$t.out 2>&1 < /dev/null &
```

Poll `tail -n 2 $L/zil-dbg.out` until `ZIL-DONE` or a `FAILED:` line (about 2 minutes). Then quote `$L/zil-dbg.out` in full and run `sh $L/check.sh dbg` (`CHECK-OK dbg`). Expected for each of the three rounds, in this order:
- `== round dbg-<n> <cipher> <time>`;
- two `1 / 1 key(s) successfully loaded` lines (zpool's wording may differ slightly), one per import;
- `ZIL records before the import:` and the count per type: about 404 `TX_CREATE`, 3 `TX_MKDIR` and 423 `TX_WRITE`, and the others as JBIS counted them (1 `TX_LINK`, 1 `TX_REMOVE`, 1 `TX_RENAME`, 1 `TX_RMDIR`, 4 `TX_SETATTR`, 1 `TX_SYMLINK`, 1 `TX_TRUNCATE`). The counts are what they are: quote them;
- `REPLAY-OK: 819 lines of names, types, modes, links, sums, xattrs and mtime identical` (410 names, 406 checksums, the mtime and the two xattrs);
- `REREAD-OK: the same 819 lines after a second export and import`;
- `<cipher> auto sa yes authentication ereports: 0`;
- `errors: No known data errors`;
- `kernel log: <n> new line(s), 0 matching ...`.

A `FAILED:` line, a `REPLAY-DIFF` or `REREAD-DIFF`, an ereport count above 0, `destroy failed` or a matching kernel line is a stop for main: quote everything, start no ZTS series, do 5d (the unload only if `check.sh` is OK), then step 7 without deleting the trees.

### 5c. The ZTS selection, twice (about 25 to 30 minutes)

```sh
L=/var/log/task-1123-zcl
setsid nohup sh $L/runs.sh dbg zd 2 -r $L/zcl.run -T zcl > $L/runs-dbg.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` and `ls -l --time-style=+%T $L/zts-zd-*.log` until `DONE zd`, a `STOP` or a `HALT` line (each run takes about 10 to 15 minutes). Then, with `g=zd`:

```sh
L=/var/log/task-1123-zcl; g=zd
grep -E "^(START|DONE) $g |^(STOP|HALT) "; cat $L/runs-dbg.nohup
grep -A1 -E "^$g-[0-9]+ rc=" $L/runs.txt
for k in 1 2; do f=$L/zts-$g-$k.log; [ -f $f ] || continue; echo "$g-$k pass=$(grep -cE 'Test( \([^)]*\))?: .* \[PASS\]$' $f) other=$(grep -cE 'Test( \([^)]*\))?: .* \[(FAIL|KILLED|SKIP)\]$' $f)"; grep -E 'Test( \([^)]*\))?: .* \[(FAIL|KILLED|SKIP)\]$' $f; sed -n '/^Results Summary/,/^Percent passed/p' $f; done
awk -v g="$g" '/^START / { on = ($2 == g) } on' $L/runs.txt | grep -E '/(block_cloning_replay_encrypted|send_encrypted_files|send_raw_spill_block|send_realloc_encrypted_files|recv_dedup_encrypted_zvol|zpool_import_errata3|zpool_import_errata4|zpool_scrub_thorough|zdb_encrypted|decrypt_fault) '
for k in 1 2; do R=$(sed -n "s/^$g-$k rc=.* dir=//p" $L/runs.txt); [ -n "$R" ] || continue; echo "$g-$k imports=$(grep -cE '(SUCCESS|ERROR): (eval )?(echo [^|]*\| *)?zpool import' $R/log) without-d=$(grep -E '(SUCCESS|ERROR): (eval )?(echo [^|]*\| *)?zpool import' $R/log | grep -cv -- ' -d ')"; done
```

The first `grep` reads `$L/runs.txt`: add it as the file argument. Expected:
- `START zd` and `DONE zd`, no `STOP` or `HALT` line, an empty nohup file;
- `zd-1 rc=0 <s> s dir=/var/tmp/test_results/<time>` followed by `    DISKS:           loop<a> loop<b> loop<c>`, and the same for `zd-2`. A `DISKS:` line naming anything but three loop devices is a stop;
- `zd-1 pass=157 other=0` and `zd-2 pass=157 other=0`, Results Summary `PASS 157` each, `Percent passed: 100.0%`. Quote the two Running Times;
- the lines of the ten tests named in the `awk` command, twice each, all `[PASS]` (JBIS: `block_cloning_replay_encrypted [00:07]`, `zpool_import_errata3 [00:03]`, `zpool_import_errata4 [00:04]`, `send_raw_spill_block [00:09]`, `send_encrypted_files [00:11]`);
- `zd-<k> imports=<n> without-d=0` for each run, `n` above 0: every import the tests logged named its directory, so none read sdc or sdd.

A `STOP` or a `HALT`: see the VM rules ("Pre-run checks", "Failures").

### 5d. End of the debug part

```sh
D=/var/tmp/task-1123-zcl; L=/var/log/task-1123-zcl; t=dbg
sh $L/check.sh $t
sed -n "/task-1123-zcl: $t loaded/,\$p" $L/dmesg-follow.txt | grep -nE 'VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock|KFENCE|KASAN|segfault|WARNING|zio_crypt'
sudo -n $D/$t/scripts/zfs.sh -u; echo "unload rc=$?"; lsmod | grep -E '^(zfs|spl) '
```

Expected `CHECK-OK dbg`, no kernel line (see "Kernel log" in the VM rules), `unload rc=0` and nothing from `lsmod`.

## Step 6: the release module (`rel`)

The same as step 5 with `t=rel` and the tag `zr`, without the `zfs-helpers.sh` part:
- **6a.** The load commands of 5a with `t=rel`. Expected `load rc=0`, `loaded` equal to `built`, **`zfs_flags=0`**, the load line **without** "(DEBUG mode)", expected `ZFS: Loaded module v2.4.99-1, ZFS pool version 5000, ZFS filesystem version 5`, and `CHECK-OK rel`. With "(DEBUG mode)" or a `zfs_flags` other than 0, a debug module is loaded: unload it, stop and report.
- **6b.** `setsid nohup sudo -n sh $L/zil.sh rel > $L/zil-rel.out 2>&1 < /dev/null &`, polled and checked as in 5b: the same lines for rounds `rel-1` to `rel-3` (`REPLAY-OK` and `REREAD-OK` with 819 lines, 0 ereports, no kernel line).
- **6c.** `setsid nohup sh $L/runs.sh rel zr 2 -r $L/zcl.run -T zcl > $L/runs-rel.nohup 2>&1 < /dev/null &`, polled and quoted as in 5c with `g=zr` and `runs-rel.nohup`. Expected the same: 157 PASS lines in each of `zr-1` and `zr-2`, three loop devices, `without-d=0`.
- **6d.** The end of 5d with `t=rel`.

## Step 7: end and cleanup

- Stop the follower: `L=/var/log/task-1123-zcl; sudo -n kill "$(cat $L/follow.pid)"; sleep 1; pgrep -af 'dmesg --follow'` shows only what step 0 found. Keep `$L/dmesg-follow.txt`.
- `lsmod | grep -E '^(zfs|spl) '`: empty. `ls -l /dev/zfs 2>&1`: absent. `sudo -n losetup -a`: empty. `pgrep -a -x zed`: empty.
- If H=yes: `cd /var/tmp/task-1123-zcl/dbg && sudo -n ./scripts/zfs-helpers.sh -r`, then `ls -l /usr/lib/udev/rules.d/60-zvol.rules /usr/lib/udev/zvol_id /usr/sbin/mount.zfs 2>&1` (none left).
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0, as in step 0. Above 0: report it and leave it; do not read it.
- `lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINTS | diff /var/log/task-1123-zcl/lsblk-0.txt -`: no output expected (the same disks, sdc and sdd unmounted, no loop device left). Quote any difference.
- `ls -A /var/tmp | diff /var/log/task-1123-zcl/vartmp-0.txt -`: quote it. Unless a stop for main says to keep the trees, remove only what this task created there: `sudo -n rm -rf /var/tmp/task-1123-zcl`, and any `file-vdev*`, `zfs-tests.*.run`, `zts-results.*`, `zts-report.*` or `constrained_path.*` the runs left. `test_results` stays. Anything else new: quote it and leave it.
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-zcl`.

## Report

Write `/var/log/task-1123-zcl/report.md` and give the same text as your final message:

1. **Environment:** VM and hostname, kernel, nproc, RAM, free space, the `lsblk`, `DISKS`, zed, follower, losetup, cache, sysctl and udev (H) lines of step 0; FETCH-OK for both trees (by SHA or by branch name), TREE-OK, SHA-OK, the two count lines; KIT-OK and the RUNFILE line; each build's rc, duration and warning counts (and any warning outside objtool notes); `builds.txt` and BUILDS-OK; both srcversions; for each load the srcversion, `zfs_flags` and the load line.
2. **Table:** module | what | runs | result (PASS count, or each non-PASS with its failing line) | durations. Rows: `test_zio_crypt` and `make -k unit` on each tree; `zil.sh` rounds 1 to 3 on each module; `zd-1`, `zd-2`, `zr-1`, `zr-2`. Then one verdict line, for example: "zio-crypt-layout-checks e472e795e4c: debug: unit 13/13 x3, make unit 12/12, zil.sh 3/3 (REPLAY-OK and REREAD-OK, 0 ereports), ZTS 97/97 x2; release: the same; kernel log clean; sdc and sdd untouched", or what differs.
3. **Values for the pull request text,** one line each, exactly these keys (n/a where a step did not run):

   ```
   zcl.env        = VM 107 (<hostname>), Linux <uname -r>, <nproc> CPUs, <MiB> MiB
   zcl.builds     = dbg rc <rc> <mm:ss>, rel rc <rc> <mm:ss>; warnings outside objtool: <none | the lines>
   zcl.release    = BUILDS-OK <yes | what differs>
   zcl.sv         = dbg <SV>, rel <SV> (<both JBIS's zc 35C063D87AB73EE899D48B6 | what differs>)
   zcl.load       = dbg "<load line>" zfs_flags <v>; rel "<load line>" zfs_flags <v>
   zcl.unit       = dbg: test_zio_crypt <k>/3 runs "13 of 13", make -k unit <n>/12 binaries at 100 %; rel: the same (<non-PASS: binary, line>)
   zcl.dbg.zil    = <k>/3 rounds REPLAY-OK and REREAD-OK (<lines>), round 1 records "<counts>", authentication ereports <n>, kernel lines <n>
   zcl.dbg.zts    = zd-1 <PASS>/97, zd-2 <PASS>/97 (<non-PASS>), Running Time <t1>, <t2>
   zcl.dbg.kernel = <none | the lines>
   zcl.rel.zil    = <k>/3 rounds REPLAY-OK and REREAD-OK (<lines>), round 1 records "<counts>", authentication ereports <n>, kernel lines <n>
   zcl.rel.zts    = zr-1 <PASS>/97, zr-2 <PASS>/97 (<non-PASS>), Running Time <t1>, <t2>
   zcl.rel.kernel = <none | the lines>
   zcl.disks      = lsblk before and after: <identical | the diff>; DISKS of the ZTS runs: <three loop devices each | what else>; imports without -d: <0 | the lines>
   ```

4. **Quotes:** the START/DONE lines, the unit lines of step 4 (`dbg` run 1 in full, then what differs), both `zil-<tree>.out` files, the ten tests' lines of each ZTS run, every non-PASS with its lines, the kernel-log checks.
5. **Deviations, cleanup,** then `End`.
