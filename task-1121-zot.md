TASK-1121-ZOT (from the coordinator session "main"): functional, about 50 min builds included on 6.12. A regression test for our fix "Linux: wait for zvol_open() before freeing a removed zvol" (M-41, text 49), built on our zinject named-point series (N-01, fork branch `zinject-delay-points-v2`): on Linux 5.10 and older, `zvol_open()` reads the `zvol_state_t` from the gendisk's `private_data` and then locks it with nothing holding it, and `del_gendisk()` does not wait for such an open, so removing the minor can free the `zvol_state_t` under it. (A) On this VM's 6.12 kernel, debug build with the fix: the new ZTS test `zvol_misc_remove_open` three times, `zinject_args` once, then the group `-T zvol_misc` once. (B) ONLY ON A KERNEL OF 5.10 OR OLDER: the same test with the fix, then the fail-before. This task does not run part B on 6.12: there the kernel itself closes the window, so the fail-before tree passes too and proves nothing; main runs part B through a 5.10 route of its own. Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-zot`, `L=/var/log/task-1121-zot`.

## Why

`zvol_open()` reads `zv` from `disk->private_data` and then takes `zv->zv_state_lock`, with nothing holding `zv` in between. `zvol_os_remove_minor()` clears `private_data` and calls `del_gendisk()`, then `zvol_remove_minors_impl()` frees `zv`: it relies on `del_gendisk()` to wait for an open that read `private_data` before it was cleared. On 5.14 and newer it does: `fops->open()` runs under `disk->open_mutex`, which `del_gendisk()` takes. On 5.10 and older the open runs under the whole disk's `bd_mutex`, which `del_gendisk()` never takes, so the open can lock a freed `zvol_state_t` (a fork CI run on AlmaLinux 8, 4.18, panicked this way: `list_del corruption` in `__mutex_lock()` from `zvol_open()`). The fix takes and drops the whole disk's `bd_mutex` after `del_gendisk()` where it exists (configure check `HAVE_BLKDEV_BD_MUTEX`, `no` on 6.12, where the fix is compiled out).

- fix (commit 5, `74f05aad57b`): `e256741ff14` of the fork branch `zvol-open-pin-state` (tested by TASK-1120-ZVO on 6.12), cherry-picked unchanged onto N-01.
- test (commit 6, `c37f6731575`): adds the zinject point `zvol_open:found` in `zvol_open()`, right after the `private_data` read and before `mutex_enter(&zv->zv_state_lock)` (a `zvol_state_t` has no pool at hand there, so the point passes no pool and a handler on any pool holds it), and `zvol_misc_remove_open` (Linux): it creates `testpool/removeopen` (64M, `volmode=dev`), waits for udev, `zinject -w zvol_open:found -s 120 testpool`, starts `dd` reading one block of the device in the background, held at the point, then `zfs set volmode=none` in the background, which removes the minor and returns only once the minor is removed and the `zvol_state_t` freed. Once the property reads `none`, `zfs set` must still be waiting 10 s later. The test then clears the handler: the `dd` open must fail (ENXIO), `zfs set` must return 0 and `/sys/block/zdN` must be gone; `volmode=dev` again must give a readable device.

Expected on 6.12 with the fix: PASS in about 20 s each time (10 s of it is the deliberate wait), as `del_gendisk()` waits for the held open. On 5.10 or older with the fix: the same, the fix's `bd_mutex` wait doing it. On 5.10 or older without the fix (part B): FAIL, and the released open then uses freed memory.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zvol-open-pin-state-test` (main pushes it before this task starts), head `c37f6731575b56c4a1677b0e08178fc24cd0428d`, tree `f47526dae8fd6c14da9bb6e8175715a7f5dd9051`: N-01's four commits (`4a98dfeb57b`, on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`, 20/11), then the fix, then the test. The fail-before tree (part B only) is N-01 plus the test commit only: tree `2027d8e21958a71544609fc97568a6260d8c9817`, whose `module/os/linux/zfs/zvol_os.c` is master's plus the point and its comment.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything by hand; never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls /sys/block | grep -E '^zd'` and `find /dev/zvol -type l 2>/dev/null` show nothing (an empty `/dev/zvol` directory is fine), `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_remove_open.* 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. A pool this task did not create, a non-empty cache file or a stale `/var/tmp/tunable-*` file is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. Capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|zvol|dd|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`), `sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg` and `sudo -n dmesg | tail -n 150` into $L/hang-<tag>.txt, then stop and report.
- No reboot in part A. A hang, an oops, a panic or a module that will not unload: capture as above, stop, report and wait for main.
- A finding that is not this bug: on master and on this tree, `zvol_os_create_minor()` can leave the ZIL open, and a later minor creation then fails `VERIFY0P(zilog->zl_get_data)` in `zil_open()` (M-30, upstream PR #19847, open and not in this tree; fork CI hit it in `zvol_misc_volmode_race`, and TASK-1119-ZVH reproduced it on master through `snapdev`). Its signature is `VERIFY0P(zilog->zl_get_data) failed` and `PANIC at zil.c:<line>:zil_open()` in dmesg, with `zil_open` and `zvol_os_create_minor` in the stack, often followed by `zfs set` stuck in `taskq_wait_outstanding` and a test failing with `Property changes still running`. It is handled as any panic (capture, stop, report), and reported as M-30, not against the fix.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r` (a 6.12 kernel expected; a kernel of 5.10 or older means main sent this task through its 5.10 route: say so, do part A only if the route says so, and part B as the route says), `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 10 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), `ls /sys/block | grep -E '^zd'` and `find /dev/zvol -type l 2>/dev/null` (nothing), `ls -ld /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_remove_open.* 2>&1` (none), `pgrep -a -x zed; pgrep -a -x lt-zed` (none; a running zed is a stop), `ls -l /etc/zfs/zpool.cache 2>&1`.

The cache file: absent or empty is expected. If it is above 0: quote its `ls -l` and `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E '^(testpool|/)' | sort -u`; if it names only ZTS pools (`testpool...`) with paths under `/var/tmp`, move it to `$L/zpool.cache.step0` in step 1 once $L exists, and say so; anything else: stop and report.

The zvol udev rule. `zvol_misc_remove_open` (and every `zvol_misc` test of step 3.3) reads `/dev/zvol/<pool>/<volume>`, which only the rule `60-zvol.rules` and its helper `zvol_id` create; the uninstalled tree installs neither. The records disagree: TASK-1117-ER34 found both missing on 17/11, then TASK-1117-UDV installed them by hand (around 18/11 00:08Z) on 105, 106 and 107 as regular files, `/etc/udev/rules.d/60-zvol.rules` and `/lib/udev/zvol_id`. Record which is true on this VM now:

```sh
ls /dev/zvol 2>&1; ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /etc/udev/rules.d/60-zvol.rules 2>&1; command -v zvol_id; ls -l /lib/udev/zvol_id /usr/lib/udev/zvol_id 2>&1; grep -h zvol_id /etc/udev/rules.d/60-zvol.rules /lib/udev/rules.d/60-zvol.rules 2>/dev/null
```

Quote all of it, and decide H:

- PRESENT: a `60-zvol.rules` exists (file or link) and the `zvol_id` its `PROGRAM==` names exists: H=no. Leave them alone: step 3 runs no `zfs-helpers.sh` and step 5 no `-r`; say "udev rule present, left alone". Expected on 105 to 107 since UDV: `/etc/udev/rules.d/60-zvol.rules` (461 bytes) and `/lib/udev/zvol_id` (22176 bytes; `/lib` is a link to `/usr/lib`, so `/usr/lib/udev/zvol_id` is the same file), the rule naming `/lib/udev/zvol_id`.
- ABSENT: no `60-zvol.rules` in any of the three directories and no `zvol_id`: H=yes. Step 3 installs the tree's helpers with `scripts/zfs-helpers.sh -i` before the first ZTS run, and step 5 removes them with `-r`, so that the VM ends as it began.
- Anything else (a rule without the `zvol_id` it names, or the reverse): stop and report, touch nothing.

`command -v zvol_id` prints nothing either way (`zvol_id` lives in `/lib/udev`, off `PATH`), and `ls /dev/zvol` shows `No such file or directory` or a directory with no volume link: both are recorded, neither decides.

## STEP 1: the tree

```sh
D=/var/tmp/task-1121-zot; L=/var/log/task-1121-zot
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $D/fix-dbg && cd $D/fix-dbg
git checkout -q a1daebfec3f5ca46ac261ef339efdf0dbeae997c
git fetch -q https://github.com/tigerblue77/zfs zvol-open-pin-state-test && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git rev-parse HEAD~2) $(git rev-parse HEAD~6) $(git status --porcelain | wc -l)"
#   c37f6731575b56c4a1677b0e08178fc24cd0428d f47526dae8fd6c14da9bb6e8175715a7f5dd9051 4a98dfeb57bddb64160b6c39f111e2e46b971ea6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git log --oneline -7
#   c37f6731575 ZTS: remove a zvol's minor while an open has not locked it
#   74f05aad57b Linux: wait for zvol_open() before freeing a removed zvol
#   4a98dfeb57b ZTS: free a dataset while its upgrade task returns
#   d074b306736 ZTS: leave a fresh vdev file after each upgrade test
#   af8df3d6ebe ZTS: replay a truncate or punch before a write made after it
#   352bd50e775 zinject: hold threads at named points in the code
#   a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM
sha256sum module/os/linux/zfs/zvol_os.c module/zfs/zio_inject.c config/kernel-blkdev.m4 tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh
#   543a74f0ce7086c14548bee56f869fac73d4bbb2e039ac039bfe737d189a019e  module/os/linux/zfs/zvol_os.c
#   2f65e78af7a09d492a70f50d8b71bcc65dfa53ab7858cb3720601518a192a2d9  module/zfs/zio_inject.c
#   14db830c60c3a4f42142cb5e09ed9d5c51ae161b0f65fc605fb8d0e2b14d6a65  config/kernel-blkdev.m4
#   75c325b6de6a884fbdbef761ca51666f1c1669a7f1bec31aba81834e0a5e9d9f  .../zvol_misc_remove_open.ksh
grep -c 'HAVE_BLKDEV_BD_MUTEX' module/os/linux/zfs/zvol_os.c config/kernel-blkdev.m4   # 2, 1
grep -c 'ZIO_INJECT_POINT_ZVOL_OPEN_FOUND' module/os/linux/zfs/zvol_os.c module/zfs/zio_inject.c include/sys/zio.h   # 1, 1, 1
grep -c 'zvol_misc_remove_open' tests/runfiles/linux.run tests/zfs-tests/tests/Makefile.am   # 1, 1
```

Stop and report if a hash, a count or a subject differs (the branch may have moved). If the fetch fails, wait 1 minute and try once more. Do not edit any file of the tree.

## STEP 2: build (about 20 minutes)

```sh
D=/var/tmp/task-1121-zot; L=/var/log/task-1121-zot
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-fix-dbg.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Quote the line `checking whether bdget_disk() and bd_mutex are available... no` from the log and `grep -c 'undef HAVE_BLKDEV_BD_MUTEX' $D/fix-dbg/zfs_config.h` (1): on 6.12 the fix is compiled out, the point is not. Note S1 = `modinfo -F srcversion $D/fix-dbg/module/zfs.ko`.

## STEP 3: part A, debug build with the fix, on 6.12

With H=yes only (step 0), the tree's helpers first:

```sh
D=/var/tmp/task-1121-zot; L=/var/log/task-1121-zot
sudo -n $D/fix-dbg/scripts/zfs-helpers.sh -iv > $L/helpers-i.log 2>&1; echo "rc=$?"; cat $L/helpers-i.log; sudo -n udevadm control --reload && echo UDEV-RELOADED
```

Expected rc 0, the configuration block (`udevdir:`, `udevruledir:` and the others), an `ln -s $D/fix-dbg/...` line for each of `zvol_id`, `60-zvol.rules`, `69-vdev.rules` and `90-zfs.rules` and for the other helpers, and UDEV-RELOADED. A `Source missing` line for `zvol_id` or `60-zvol.rules`: stop and report (the runs could not see `/dev/zvol`). A `Removing old symlink`, `Symlink exists` or `File exists` line means a helper from elsewhere was already there: quote it and report it (`-r` at step 5 does not restore a link that `-i` replaced). With H=no, skip this block and say so.

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0=`sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-zot; L=/var/log/task-1121-zot; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zvol_misc_remove_open > $L/zts-ad-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time, in about 20 s. For each run quote: the result line and duration; from the test's own log under the results directory the runner prints, the `NOTE:` line with the `zinject` table (handler, `testpool`, `zvol_open:found`, INJECT 1), the `NOTE: zfs set volmode=none waits for the held open` line, the `NOTE: open: rc ...` line (expected `rc 1` and ENXIO: `dd: failed to open '/dev/zdN': No such device or address`), and the `SUCCESS:` lines of `test 0 = 0`, `test 1 = 0` (a `log_mustnot`), `test -e /sys/block/zdN` (a `log_mustnot`) and the last `dd`; and, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection`, `list_del` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-t zinject_args` once (tag zi), after the pre-run checks: `cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t zinject_args > $L/zts-zi.log 2>&1 < /dev/null &`. Expected PASS (its list of points now holds `zvol_open:found`). Quote the result line and duration.
3. `-T zvol_misc` once (tag zm), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T zvol_misc > $L/zts-zm.log 2>&1 < /dev/null &`. Expected: the tests of `common.run` and `linux.run` tagged `zvol_misc` PASS, `zvol_misc_remove_open` among them. Quote every `Test:` line with result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known and whether it carries M-30's signature (VM rules).

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|general protection|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B, ONLY ON A KERNEL OF 5.10 OR OLDER (not run on 6.12)

On 6.12, write "Part B: not run (kernel 6.12: `del_gendisk()` waits for the open there, so the fail-before tree would pass)" and go to step 5.

Main runs this part through its 5.10 route (a separate task gives the kernel, its headers and compiler, the boot with `slub_debug=P` on that kernel's command line only, `kernel.panic_on_oops=0` for the run, the reboots and the restore); the route's rules win where they differ from this task's, the zvol udev rule included (TASK-1121-ZV510C installs it in its guest with the tree's `zfs-helpers.sh -i`). On that kernel:

1. Trees: `fix-dbg` as in step 1, and the fail-before tree:

   ```sh
   D=/var/tmp/task-1121-zot
   git clone -q $D/fix-dbg $D/fb-dbg && cd $D/fb-dbg
   git checkout -q 4a98dfeb57bddb64160b6c39f111e2e46b971ea6
   git -c user.name=t -c user.email=t@t cherry-pick c37f6731575b56c4a1677b0e08178fc24cd0428d
   git rev-parse HEAD^{tree}          # 2027d8e21958a71544609fc97568a6260d8c9817
   sha256sum module/os/linux/zfs/zvol_os.c   # 225a28ee132dd7a79ea566a93104ca61e86cff62774d647e1213d70d71159eb1
   grep -c 'HAVE_BLKDEV_BD_MUTEX' module/os/linux/zfs/zvol_os.c config/kernel-blkdev.m4   # 0, 0
   git diff a1daebfec3f5ca46ac261ef339efdf0dbeae997c -- module/os/linux/zfs/zvol_os.c | grep -c '^+[^+]'   # 5 (the point and its comment)
   ```

   Both built with `--enable-debug` against the running kernel's headers. In the `fix-dbg` build log, `checking whether bdget_disk() and bd_mutex are available... yes` and `grep -c 'define HAVE_BLKDEV_BD_MUTEX 1' zfs_config.h` = 1 (the fix compiled in); `nm module/zfs.ko | grep -cE ' U (bdget_disk|bdput)$'` = 2. A `no` there is a stop: the run would not test the fix.
2. With the fix (`fix-dbg` loaded, srcversion and "(DEBUG mode)" quoted): `-t zvol_misc_remove_open` three times, as in step 3.1. Expected PASS each time, the fix's `bd_mutex` wait now holding the removal (this, not part A, is the run that exercises the fix). Then `-t zvol_misc_volmode_race` once (the test that hit the panic in CI; expected PASS, no dmesg finding; a finding with M-30's signature is reported as M-30, not against the fix). Unload.
3. Fail-before (`fb-dbg` loaded, srcversion and "(DEBUG mode)" quoted; `cat /proc/cmdline` must show `slub_debug=P`, without which the use-after-free of the next point is most likely silent): start `dmesg --follow` into `$L/oops-follow.txt` detached, then `-t zvol_misc_remove_open` ONCE, detached, with `timeout 900`. Expected:
   - the test FAILs in its step 3, within a few seconds: `NOTE: zfs set: rc 0,` then, on its own line, `The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned` (`log_fail` prints its message with no `ERROR:` prefix; the removal did not wait for the held open, and freed the `zvol_state_t`);
   - its cleanup runs `zinject -c all`, and the released `dd` open locks the freed, poisoned `zvol_state_t`: expected `general protection fault, probably for non-canonical address 0x6b6b6b6b6b6b6b..` with `RIP:` in `__mutex_lock` or a helper of it (`mutex_spin_on_owner`, `mutex_can_spin_on_owner`), `zvol_open` in the Call Trace below it (then `__blkdev_get`, `blkdev_get`, `blkdev_open`, `do_dentry_open`), Comm `dd`. A `list_del corruption`, a SLUB `Poison overwritten` or any other oops in `zvol_open()` is as much a confirmation; no finding at all is reported as such (the test's ERROR line stays the verdict);
   - `NOTE: open: rc` is not printed (the test failed before); the cleanup's `wait_file` of the open should still see the `dd` subshell end (rc 139 if `dd` died in the oops; if not, the cleanup prints the stacks of `dd` and `zfs` after 60 s, quote them); the runner is expected to return with FAIL; the dead open keeps the old disk's `bd_mutex`, which nothing else needs, so the pool cleanup is expected to complete (if it hangs instead, capture it as a hang and follow the route). Quote the oops (GPF or BUG line, RIP, Comm, the first 15 frames), the test's `ERROR:` line and the cleanup lines, `ps` of any D task with its kernel stack, `zpool list` after it, and whether the runner returned.
   - The kernel is tainted afterwards: reboot as the route says.

## STEP 5: end

Unload if loaded (`sudo -n $D/fix-dbg/scripts/zfs.sh -u`, quote rc; `lsmod | grep -E '^(zfs|spl) '` empty). The pre-run checks once more (all clean expected). `/etc/zfs/zpool.cache` above 0: quote its `ls -l` and the `strings` check of step 0; if it names only `testpool...` with paths under `/var/tmp`, move it to `$L/zpool.cache.end`; otherwise report it and leave it. With H=yes, remove the helpers before the tree goes (their links point into it):

```sh
D=/var/tmp/task-1121-zot; L=/var/log/task-1121-zot
sudo -n $D/fix-dbg/scripts/zfs-helpers.sh -rv > $L/helpers-r.log 2>&1; echo "rc=$?"; cat $L/helpers-r.log; sudo -n udevadm control --reload && echo UDEV-RELOADED
```

Expected rc 0, an `rm` line for each `ln -s` line of `helpers-i.log`, no `Expected symlink` line, and UDEV-RELOADED. Then, with H=yes or H=no, the udev command of step 0 once more: its output must be the same as at step 0 (ABSENT again with H=yes, the untouched rule and `zvol_id` with H=no). Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`.

## Report

Write $L/report.md and give the same text as your final message. The zvol udev rule: the step 0 output, H (present and left alone, or installed with `-i` and removed with `-r`) and the step 5 comparison. Part A: the configure line and the zfs_config.h count, S1 and the load line; a table (run, result, duration, zinject table INJECT 1 yes/no, "waits for the held open" seen yes/no, open rc and its message), then "no VERIFY/ASSERT/PANIC/BUG/Oops/GPF/list_del/hung task" or what was seen (an M-30 finding named as such); the `zinject_args` result; the group result (Results Summary, every non-PASS with its lines). Part B: "not run (kernel 6.12)" or, through the route, the configure lines, the with-fix table, the fail-before failure line (no `ERROR:` prefix), the oops (GPF or BUG line, RIP, Comm, first 15 frames), the cleanup lines, the D tasks, whether the runner returned. One-line verdict: "ZOT PASS 3/3 debug on 6.12, zinject_args PASS, -T zvol_misc PASS; part B not run (6.12)" or what differs.
