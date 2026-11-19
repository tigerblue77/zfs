TASK-1119-ZVH (from the coordinator session "main"): functional, about 2 h, debug build only, ONE reboot pre-approved at step 4. VM 106 (Debian 13, kernel 6.12).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1119-zvh`, `L=/var/log/task-1119-zvh`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test left behind: remove its files instead. If a result surprises you, stop and report it with the outputs.

## Why

Upstream PR openzfs/zfs#19847 (ooonea, "zvol: avoid write callbacks during minor replay") fixes a master bug: `zvol_os_create_minor()` opens the volume's ZIL with `zvol_get_data` for replay and closes it only when something was replayed, so when another hold keeps the objset cached, the next `zil_open()` trips `VERIFY0P(zilog->zl_get_data)` (debug builds) and the zvol taskq hangs. Fork CI hit it three times in `zvol_misc_volmode_race` on master-based heads. The PR passes NULL as the callback of that replay-only open and adds a deterministic test by Will Rouesnel, `zvol_misc_snapdev_held`. It has no review yet. This task gives it independent evidence: the PR's head passes, and master's `zvol_os.c` fails the PR's test.

What is tested: https://github.com/openzfs/zfs `refs/pull/19847/head` = `fda72f0e1da096355b308bf7d35927fbdc7aa15f` (tree `e3d82acb14fceb60239930af2f114b8cd5cac7fb`), three commits on master `c913da86156eba3243cc1ef515e98d69887654a7`: the fix `3d8a0c519957f38901e0b56558295d1a01e28467`, the test `faeecb8d73576cb3c7611a52a76dbca2626a11ad`, a test fix `fda72f0e1da0`. The fail-before (step 4) is the same tree with both `zvol_os.c` files taken from master (tree `4a330c5b00edba023439f3aa798fa91f2cb725c2`).

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM; never `zpool import -a`; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows this task's module only, `timeout 30 sudo -n $D/zd/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* 2>&1` shows nothing left by a previous test.
- Every build and ZTS run runs detached or in a background call under `timeout`, and is polled. A single test still going after 30 minutes, a group after 75 minutes, a build after 90 minutes is a hang: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang-<step>.txt, then stop and report. The only reboot allowed without asking main is the one of step 4.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`uname -r`, `uptime -s`, `free -m`, `df -h / /var/tmp` (at least 15 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs /etc/zfs/zpool.cache 2>&1` (neither may exist), `losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* 2>&1`. TQL's kept trees under /var/tmp stay untouched. Any difference: stop and report.

## STEP 1: fetch and check the tree

```sh
D=/var/tmp/task-1119-zvh; L=/var/log/task-1119-zvh
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
mkdir $D/zd && cd $D/zd && git init -q . && git fetch -q --depth 4 https://github.com/openzfs/zfs refs/pull/19847/head && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git rev-parse HEAD~2) $(git rev-parse HEAD~3) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "fda72f0e1da096355b308bf7d35927fbdc7aa15f faeecb8d73576cb3c7611a52a76dbca2626a11ad 3d8a0c519957f38901e0b56558295d1a01e28467 c913da86156eba3243cc1ef515e98d69887654a7 e3d82acb14fceb60239930af2f114b8cd5cac7fb 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
printf '%s\n' "c7bde44e60802298c2d51b99b720d93a84b55f1f7f198a03494c855dacf17dfb  module/os/linux/zfs/zvol_os.c" "28a0f923e72ed382782d21ddf18e5ab23883eb7ed20bf840054f7e2ee086d005  module/os/freebsd/zfs/zvol_os.c" "51e17c3c3d7b755deda047bce46b94b3ffae65559c2489b2b0c7e9f2445bfa4c  tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh" "64da5991d232d0cd543a54b838039b8ac10360871d660926e881a6d4af79c55f  tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_volmode_race.ksh" "5660fd1d84079d2710e09dbef44eba1a01fb2007a315de2d1c6876000b42ae14  tests/runfiles/common.run" "ae738c5459732911a5cedec25f5f95f0cf6850025c71c001453fe1ca1401b50d  tests/zfs-tests/tests/Makefile.am" > $L/sha.txt
sha256sum -c --quiet $L/sha.txt && echo SHA-OK
git diff --stat HEAD~3 HEAD | tail -1; ls -l tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_snapdev_held.ksh | awk '{print $1}'
```

Expected TREE-OK, SHA-OK, `5 files changed, 94 insertions(+), 4 deletions(-)` and `-rwxr-xr-x`. If the fetch fails, wait 1 minute and try once more; any other difference: stop and report as is. Do not edit any file.

## STEP 2: debug build (about 10 minutes)

`cd $D/zd && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=$?` into $L/build-zd.log, detached and polled. It must end `build-rc=0`; quote every `warning:` line outside objtool notes.

## STEP 3: the PR's head (all expected PASS)

`sudo -n $D/zd/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal `modinfo -F srcversion $D/zd/module/zfs.ko`) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`.

A run, one at a time, after the pre-run checks: `cd $D/zd && sudo -n -u zts ./scripts/zfs-tests.sh -vKO <sel> > $L/zts-<tag>-<n>.log 2>&1 < /dev/null`. Per run quote every `Test:` line with result and duration, and the Results Summary; on a FAIL or KILLED copy that test's log from the run's result directory to $L/ and quote the failing lines with the 20 before them.

1. `-t zvol_misc_snapdev_held`, 3 runs (tag sh). Expected PASS each.
2. `-t zvol_misc_volmode_race`, 10 runs (tag vr). Expected PASS each.
3. `-T zvol_misc` once (tag zm), then `-T slog` once (tag sl). Expected all PASS; a FAIL is quoted and reported as is, without rerun, saying whether tests/test-runner/bin/zts-report.py.in lists it as known.

After them: no pool left; `sudo -n dmesg | tail -n +$((N0+1)) | grep -cE 'zl_get_data|PANIC|VERIFY'` must print 0, and every line matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it. Unload with `sudo -n $D/zd/scripts/zfs.sh -u`.

## STEP 4: master's zvol_os.c, fail-before (one reboot pre-approved)

```sh
cd /var/tmp/task-1119-zvh/zd && git checkout c913da86156eba3243cc1ef515e98d69887654a7 -- module/os/linux/zfs/zvol_os.c module/os/freebsd/zfs/zvol_os.c && test "$(git write-tree)" = 4a330c5b00edba023439f3aa798fa91f2cb725c2 && echo TREE-OK && sha256sum module/os/linux/zfs/zvol_os.c
```

Expected TREE-OK and `231d506fb25035cfd936f54ba8d44756c9988b9a7eec74b0c8923c5030f65a29`. Rebuild in place (`nice make -j4`, detached, `build-rc=0`), check that the new `zfs.ko` srcversion differs from step 3's, load it (srcversion equal to the tree's, DEBUG mode), note N1.

Run `-t zvol_misc_snapdev_held` once (tag fb) under `timeout 1500`. Expected: the test FAILs or is KILLED, and the kernel log has `VERIFY0P(zilog->zl_get_data) failed` and `PANIC at zil.c:<line>:zil_open()` with `zil_open` and `zvol_os_create_minor` in the stack. Quote those lines, the stack, and the test's failing lines. Capture $L/hang-fb.txt as in the rules.

If no such panic appears: run `-t zvol_misc_snapdev_held` twice more, then `-t zvol_misc_volmode_race` up to 10 times, stopping at the first `zl_get_data` panic; report the counts.

After the panic (and only then), the module cannot be unloaded: save everything to $L, then reboot (`sudo -n systemctl reboot`), schedule your own resumption, and after the boot, before loading anything: `lsmod | grep -E '^(zfs|spl) '` empty, `ls /etc/zfs/zpool.cache 2>&1` absent (if present, move it to $L/zpool.cache.fb), remove the test's leftover files under /var/tmp named in the test's log (never import them), `sudo -n rm -rf $D`, `sudo -n fstrim -av`.

If no panic happened, unload normally and do the same cleanup without rebooting.

## Report

Write $L/report.md and give the same text as your final message:
1. Environment; TREE-OK, SHA-OK, diffstat, mode; build rc and warnings; srcversions; step 4's TREE-OK, sha and srcversions.
2. Table: tag | code | run | selection | result | duration | key note. Then one verdict line, e.g. "#19847 fda72f0e1: zvol_misc_snapdev_held PASS 3/3, zvol_misc_volmode_race PASS 10/10, zvol_misc and slog groups PASS, no zl_get_data panic; master's zvol_os.c: zvol_misc_snapdev_held FAIL 1/1 with the zl_get_data panic in zil_open < zvol_os_create_minor".
3. Quotes: the fb panic lines and stack, the dmesg checks.
4. Deviations, cleanup, then End.
