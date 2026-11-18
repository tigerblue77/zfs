TASK-1118-ROV2 (from the coordinator session "main"): functional, about 3 h builds included, no reboot. VM 105 or 106 (Debian 13, kernel 6.12).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `Z`).

## Why

With `zfs_recover=1` and a read-write import, a space map ALLOC entry only part of which is free is skipped since 6495dafd58b (#18255): zfs_range_tree_remove() logs through zfs_panic_recover() and returns without removing anything, so the free part of the entry stays in ms_allocatable and is allocated again over live data. The same skip applies to the unflushed allocations of the log space maps, and an ALLOC replayed from the log that overlaps one already recorded is dropped whole. The branch removes the part that is free (and records the whole of an overlapping logged allocation) when zfs_recover is set, keeping the warnings; without zfs_recover nothing changes.

The new test `zhack_metaslab_append` (cli_root/zhack group) writes a file of one 128K block, appends with the new `zhack metaslab append` a FREE of the block, an ALLOC of its first 4K and an ALLOC of the whole block to its metaslab's space map, imports with zfs_recover=1, runs `zpool initialize -w` (which writes a pattern over every range the pool considers free) and scrubs. With the fix: no checksum error, file unchanged. With master's code: the block's last 124K are free, initialize overwrites them, and the scrub reports checksum errors.

Version 2 of this task: it also tests a second fix and its test. Since d2f5cb3a50d, module/zfs/range_tree.c defines zfs_panic_recover() as an abort in every userspace build, libzpool included, so `zdb -AA` (which sets zfs_recover) aborts at the first overlapping or missing range instead of warning as 2.4 did. The fix keeps that macro out of the libzpool build. The new test `zdb_space_map_overlap` (cli_root/zdb group) appends two FREEs then two ALLOCs of a file's block to its metaslab's space map, checks that `zdb -mmm` fails and that `zdb -AA -mmm` succeeds and reports both overlaps.

What is tested: fork https://github.com/tigerblue77/zfs, branch `range-tree-recover-overlap-zdb`, head `cdfebe2cfc6a4e8d5b2af3aef6e83aecda12ae4e`, tree `9fdc6f6d5a6cfe448f78486fdb55cceb3c19c19e`, five commits on upstream master `cde5d598eb44b5d6efe8167666d988a496346657` (the fix `7d0294aaac6252a55aea3d08c4f4b67217034220`, the zhack subcommand `bc33b6449a3058ec4527cb357c58ddb499134b77`, the test `b750df1f158d96fcadf3c2bf4ddb9ff566a922bb`, the range_tree fix `639a1bc01cff7008a364ee60540565892587b4f7`, the zdb test). The fail-before (step 5) is master's code with the branch's zhack and tests: it checks module/zfs/space_map.c, module/zfs/metaslab.c, module/zfs/spa_log_spacemap.c, include/sys/space_map.h and module/zfs/range_tree.c out of master in the RELEASE tree (tree `1f7c3884529da704850a2380d0e54c6ec7dbeaff`) and rebuilds incrementally.

**Never run zhack_metaslab_append on a debug build with master's files**: there the load trips the debug check of the metaslab's free space (metaslab_verify_space(), a VERIFY) and panics the VM. The fail-before is on the release build only.

## VM rules (as before)

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- No ZTS run while a pool of your own exists: before every run, `lsmod | grep -E '^zfs '` shows the tree of that step only, `timeout 30 sudo -n $D/<tree>/zpool list -H -o name` shows no pool, `cat /sys/module/zfs/parameters/zfs_recover` gives 0, and `ls -d /var/tmp/zhack_metaslab_append /var/tmp/zdb_space_map_overlap /var/tmp/tunable-RECOVER 2>&1` says none exists.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A run of one test still going after 30 minutes, a group run after 75 minutes, or a build after 90 minutes is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'` and `sudo -n dmesg | tail -n 100` into $L/hang.txt, stop and report; do not reboot without main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `cat /etc/debian_version`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp, else stop and report), `lsmod | grep -E '^(zfs|spl) '` (must be empty; if a pool is imported, stop and ask main), `ls -l /dev/zfs 2>&1` (must not exist), `cat /proc/cmdline`, `id zts`, `command -v ksh git`, `git --version` (2.22 or later), `ls -d /var/tmp/task-1118-rov /var/log/task-1118-rov 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/zhack_metaslab_append /var/tmp/zdb_space_map_overlap /var/tmp/tunable-* 2>&1; losetup -a`. ZTS scratch files left there with no loop device on them and no module loaded are ours: delete them, `sudo -n fstrim -av`, and say so.

## STEP 1: fetch and check the two trees

```sh
D=/var/tmp/task-1118-rov; L=/var/log/task-1118-rov; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 6 $U range-tree-recover-overlap-zdb && git checkout -q FETCH_HEAD && cp -a $D/zd $D/zr
for n in zd zr; do cd $D/$n && echo "$n $(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git rev-parse HEAD~4) $(git rev-parse HEAD~5) $(git write-tree) $(git status --porcelain | wc -l)"; done > $L/trees.txt
printf '%s\n' "zd cdfebe2cfc6a4e8d5b2af3aef6e83aecda12ae4e 639a1bc01cff7008a364ee60540565892587b4f7 b750df1f158d96fcadf3c2bf4ddb9ff566a922bb bc33b6449a3058ec4527cb357c58ddb499134b77 7d0294aaac6252a55aea3d08c4f4b67217034220 cde5d598eb44b5d6efe8167666d988a496346657 9fdc6f6d5a6cfe448f78486fdb55cceb3c19c19e 0" "zr cdfebe2cfc6a4e8d5b2af3aef6e83aecda12ae4e 639a1bc01cff7008a364ee60540565892587b4f7 b750df1f158d96fcadf3c2bf4ddb9ff566a922bb bc33b6449a3058ec4527cb357c58ddb499134b77 7d0294aaac6252a55aea3d08c4f4b67217034220 cde5d598eb44b5d6efe8167666d988a496346657 9fdc6f6d5a6cfe448f78486fdb55cceb3c19c19e 0" > $L/trees.expected
diff $L/trees.expected $L/trees.txt && echo TREES-OK
cat > $L/sha.txt <<'EOF'
5c338fbf13fd27add0bedcdc97b2c896f1e9f7c545e5b37571651069d9a3584d  module/zfs/space_map.c
4c1d29a0b7d83981137daa7c1769896e47e799dbc3ebcb6ef19fa3f905012063  module/zfs/metaslab.c
4b070844ba8e23834868c164d4db8b4aca99f509d8fbb8045c0c88ef4a1c712e  module/zfs/spa_log_spacemap.c
c297ac3fae4f78a3898d8879118429aceed1141b06f5970f3d76099930d093c9  include/sys/space_map.h
84cb33a2eacbc176f431e39c0081bef20c523056668cd08b17fbd42faeb81a85  cmd/zhack.c
3c78e667dbb2984a8823fe3205067d31f75f59bda4a21c79a7a77899e47e2841  man/man1/zhack.1
f73a7bdaa3e023cb540b0f543d9fce708fab43223710e7655700e73062ec2fa7  tests/zfs-tests/tests/functional/cli_root/zhack/zhack_metaslab_append.ksh
105839ad34ca4360685d93f64fe3f5c7a112e25cb77f29b829b8f023bf349155  tests/runfiles/common.run
b2a4876ce6616578290c388904f27133b53c68fa8513a7a0f99802b379b19d18  tests/zfs-tests/include/tunables.cfg
52a2c0748b36defd1efa6dd1009471f25baabfa5639dc270e8315fe099e3d0b2  tests/zfs-tests/tests/Makefile.am
2fbb9363e37c7774e9bfdc9081222ec70d99009a0e0dfc085f2820dde61ba283  module/zfs/range_tree.c
93ee72afb78cb2dcd3c25d5a597b67e4293e666479ea95153452988adb2ccad5  tests/zfs-tests/tests/functional/cli_root/zdb/zdb_space_map_overlap.ksh
EOF
for n in zd zr; do (cd $D/$n && sha256sum -c --quiet $L/sha.txt && echo "$n SHA-OK"); done
cd $D/zd && git diff --name-status HEAD~5 HEAD && ls -l tests/zfs-tests/tests/functional/cli_root/zhack/zhack_metaslab_append.ksh tests/zfs-tests/tests/functional/cli_root/zdb/zdb_space_map_overlap.ksh && grep -n '^RECOVER' tests/zfs-tests/include/tunables.cfg
```

Expected TREES-OK, two SHA-OK lines, twelve name-status lines (`A` for the two .ksh, `M` for the ten others), mode `-rwxr-xr-x` for both, and the line `RECOVER				recover				zfs_recover`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report the outputs as they are. Do not edit any file.

## STEP 2: the two builds, at once (about 45 minutes)

```sh
D=/var/tmp/task-1118-rov; L=/var/log/task-1118-rov
setsid nohup sh -c "cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-zd.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/zr && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?" > $L/build-zr.log 2>&1 < /dev/null &
```

Poll `grep -H build-rc= $L/build-z?.log` until two lines; each must read `build-rc=0`. Quote every `warning:` line outside objtool notes (release has no -Werror).

## STEP 3: debug runs (all expected PASS)

Loading a tree T: if a module is loaded, unload it with the `scripts/zfs.sh -u` of the tree that loaded it; then `sudo -n $D/T/scripts/zfs.sh`. Quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/T/module/zfs.ko`) and the last `ZFS: Loaded module` line of dmesg ("(DEBUG mode)" for zd, not for zr); note N0=`sudo -n dmesg | wc -l`.

Load zd. A run, one at a time, detached, after the pre-run checks of the rules (`<sel>` is `-T <tag>` or `-t <test>`):

```sh
cd /var/tmp/task-1118-rov/<tree>
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > /var/log/task-1118-rov/zts-<tag>-<n>.log 2>&1 < /dev/null &
```

Per run: every `Test:` line with its result and duration, in the order printed, the Results Summary, and from its merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`) every line with FAIL, failed or ERROR. On a FAIL, copy the merged log to $L/merged-<tag>-<n>.txt and quote the failing command, its output and the 20 lines before it. After each run: `cat /sys/module/zfs/parameters/zfs_recover` (must be 0) and `ls -d /var/tmp/zhack_metaslab_append /var/tmp/tunable-RECOVER 2>&1` (both absent).

1. The new test, 3 runs (tag zd, `<sel>` = `-t zhack_metaslab_append`), each expected PASS. From the merged log of each run quote the `NOTE: /testpool/file: 0:<offset>:20000` line, the SUCCESS line of `zhack -d /var/tmp/zhack_metaslab_append metaslab append testpool 0 free ...`, the SUCCESS lines of `zpool import -d ...` and `zpool initialize -w testpool`, and the last 10 lines before `PASS`. Then the new dmesg lines of the three runs matching `zfs: rt=`: expected one or more per run, each reading `WARNING: zfs: rt={spa=testpool ... ms_allocatable}: removing segment (offset=<offset of that run> size=20000) not completely overlapped by existing ones, removing the overlapping part`. Quote them all.
1b. The zdb test, 3 runs (tag zs, `<sel>` = `-t zdb_space_map_overlap`), each expected PASS. From the merged log of each run quote the `NOTE: /testpool/file: 0:<offset>:20000` line, the SUCCESS line of the `zhack ... metaslab append` command, the lines of the two zdb commands and their results, and the last 10 lines before `PASS`. Then quote `grep -E 'overlapping|removing' /var/tmp/zdb_space_map_overlap/zdb-AA.out` if the file is still there (the test's cleanup removes it: if so, say so; do not change the test). This test loads no module code of its own: zdb runs in userspace, so it adds no dmesg line.
2. Related groups, 1 run each, in this order (tags zh, zb, lg, zi, zp, rm): `-T zhack`, `-T zdb`, `-T log_spacemap`, `-T zpool_initialize`, `-T zpool_import`, `-T removal`. Each expected all PASS (with its directory's setup and cleanup, if any); zhack must list `zhack_metaslab_append` between `zhack_label_repair_004` and `zhack_metaslab_leak`; zdb must list `zdb_space_map_overlap` between `zdb_simulated_ddt_errors` and `zdb_tunables`. A FAIL is quoted as above and reported as it is, without a rerun; say for each FAIL whether the same test is listed as a known failure in tests/test-runner/bin/zts-report.py.in of the tree.

After these runs: `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool; quote the new dmesg lines (`sudo -n dmesg | tail -n +$((N0+1))`) matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than`. Expected: only the `WARNING: zfs: rt=... ms_allocatable}: removing segment ... size=20000) not completely overlapped by existing ones, removing the overlapping part` lines of the zd runs and of the zhack group's run of the new test; anything else is quoted in full with the 10 lines before it. Unload with zd's `scripts/zfs.sh -u`, `sudo -n rm -rf $D/zd`, `sudo -n fstrim -av`.

## STEP 4: release run (expected PASS)

Load zr ("(DEBUG mode)" must be absent); note N0. Two runs (tag zr, in zr): `<sel>` = `-t zhack_metaslab_append`, with the same expectations and quotes as the zd runs of step 3.1, then `-t zdb_space_map_overlap`, as in step 3.1b. After it: no pool left, zfs_recover 0, the dmesg check since this N0 (only the expected `removing the overlapping part` warning). Unload with zr's `scripts/zfs.sh -u`.

## STEP 5: master's code in the release tree, fail-before (about 20 minutes)

Check the five files out of master in zr, check the tree, rebuild in place:

```sh
cd /var/tmp/task-1118-rov/zr && git checkout cde5d598eb44b5d6efe8167666d988a496346657 -- module/zfs/space_map.c module/zfs/metaslab.c module/zfs/spa_log_spacemap.c include/sys/space_map.h module/zfs/range_tree.c &&
test "$(git write-tree)" = 1f7c3884529da704850a2380d0e54c6ec7dbeaff && echo TREE-OK &&
printf '%s\n' "8357c49c0b43811b14e0584dc92bab38336d00c9af3cd33843975ff49fca7f52  module/zfs/space_map.c" "0e30204e2b5f86eb93285b690e768ebd85bb6fb0d320227578219814cc2974ba  module/zfs/metaslab.c" "f1e9be445949ebf99dbc334e9cde621f194aace4de711a04f42936bc2613ac80  module/zfs/spa_log_spacemap.c" "3dfc025454e6f18166ad1a258488f52fd4e6c880c5fecd81dc47b2c9fe47328f  include/sys/space_map.h" "5a513a3fa86207d7da02a32e0277ac82f7c4ba34840a7fb7dc6a918c1d769ecf  module/zfs/range_tree.c" | sha256sum -c --quiet && echo SHA-OK &&
git diff --cached --name-status HEAD
```

Expected TREE-OK, SHA-OK and five `M` lines (include/sys/space_map.h, module/zfs/metaslab.c, module/zfs/range_tree.c, module/zfs/spa_log_spacemap.c, module/zfs/space_map.c). Then, detached and polled like a build: `setsid nohup sh -c "cd /var/tmp/task-1118-rov/zr && nice make -j4; echo build-rc=\$?" > /var/log/task-1118-rov/build-fb.log 2>&1 < /dev/null &`. It must end with `build-rc=0`; quote `grep -E 'space_map|metaslab|spa_log_spacemap|range_tree|zfs\.ko|libzpool' $L/build-fb.log | head -n 30` and every `warning:` line outside objtool notes. `modinfo -F srcversion $D/zr/module/zfs.ko` must differ from zr's value of step 4. Load zr: the srcversion must equal the new `modinfo` value, no "(DEBUG mode)"; note N1=`sudo -n dmesg | wc -l`.

Fail-before fb: the pre-run checks, then one run (tag fb, in zr, `<sel>` = `-t zhack_metaslab_append`). Expected FAIL at the end: the SUCCESS line of `zpool initialize -w testpool`, then the scrub, then `zpool status -v` showing CKSUM errors on /var/tmp/zhack_metaslab_append/vdev and a permanent error in /testpool/file, then `Unexpected CKSUM errors found on testpool (<n>)`. Quote those lines, the 20 lines before them, and the cleanup that follows (the pool destroyed, then RECOVER restored). Quote the new dmesg lines since N1 matching `zfs: rt=`: expected master's message, `removing segment (offset=<offset> size=20000) not completely overlapped by existing one (offset=<offset + 0x1000> size=<1f000 or more>)`. A fb run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

Fail-before fz: the pre-run checks, then one run (tag fz, in zr, `<sel>` = `-t zdb_space_map_overlap`). Expected FAIL at the `zdb -AA -e -p ... -mmm testpool` command (zdb aborts with master's range_tree.c: exit status 134 or a message naming SIGABRT), after a passing `zdb -mmm` check. Quote that command's lines and the 20 lines before them. A fz run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

After fb and fz: no pool left, zfs_recover 0, the dmesg check since N1 (only that warning expected), unload with zr's `scripts/zfs.sh -u`, `sudo -n rm -rf /var/tmp/task-1118-rov`, `ls -d /var/tmp/zhack_metaslab_append /var/tmp/zdb_space_map_overlap /var/tmp/tunable-RECOVER 2>&1` (none present), `sudo -n fstrim -av`, `df -h /var/tmp`. Logs stay in /var/log/task-1118-rov.

## Report

Write /var/log/task-1118-rov/report.md and give the same text as your final message:

1. Environment: kernel, Debian version, nproc, RAM, /proc/cmdline, git version; TREES-OK, the two SHA-OK, the name-status lines, the test's mode and the RECOVER line; build rc and warnings of zd and zr; srcversions and DEBUG mode; step 5's TREE-OK, SHA-OK, name-status lines, build-fb rc and quoted lines, and the srcversion checks.
2. Table: tag | build | code | run | selection | result per test (or "all N PASS") | duration | failing line or key note (the block's DVA for zhack_metaslab_append). Then one verdict line, e.g. "range-tree-recover-overlap: zhack_metaslab_append PASS debug 3/3, release 1/1; zhack, log_spacemap, zpool_initialize, zpool_import, removal groups PASS; master (release): zhack_metaslab_append FAIL 1/1 with N CKSUM errors on the file's block; zdb_space_map_overlap PASS debug 3/3, release 1/1, zdb group PASS, master (release) FAIL 1/1 with zdb -AA aborting".
3. Quotes: the zhack_metaslab_append and zdb_space_map_overlap lines of steps 3 and 4; the warnings in dmesg; fb's and fz's failing lines and master's warning; the dmesg checks.
4. Deviations, cleanup, then End.
