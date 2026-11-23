TASK-1123-ZV510ER (from the coordinator session "main"): functional, about 1 h 50 (2 h 05 with the optional step 7), at most 2 h 30, no reboot of this VM. The release variant of TASK-1124-ZV510E, for the pull request "Linux: retry a contended zvol_open() before restarting it" (fork branch `zvol-open-retry-before-restart`): its regression test `zvol_misc_open_contended` three times on the branch built WITHOUT `--enable-debug` (expected PASS: the zfs module's use count unchanged, and a clean unload after each run), then `-T zvol_misc` once on the same build. ZV510E ran the same on debug builds only. Optional, if time remains under the cap: the release run of the M-41 fix branch `zvol-open-pin-state-test-wr` (built with `--enable-inject-points`), its test `zvol_misc_remove_open` three times, in the same guest. The route is ZV510C's, reused by ZV510D, ZV510E and ZVWRFB: a disposable Debian 11 guest (Linux 5.10.0-46, `slub_debug=P`) under QEMU/KVM inside this VM; this VM keeps its kernel, its GRUB and its file systems, and is not rebooted. Any of the three test VMs (Debian 13) whose CPU shows `vmx`; planned on VM 107 (32 CPUs and 32 GiB since its reconfiguration of 23/11: step 0 checks that `vmx` is still there). This task never runs beside ZV510D, ZV510E or ZVWRFB on one VM: all of them use the same port, the same QEMU name and the same guest names. Other VMs may run functional tasks at the same time: this task measures nothing, but its guest keeps 6 vCPUs busy for about 25 minutes of builds, so not beside a measurement.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The commands and the scripts of step 1 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. On this VM (the host): `D=/var/tmp/task-1123-zv510er` (trees, image, guest disk, guest key; deleted at the end) and `L=/var/log/task-1123-zv510er` (logs, scripts, serial logs; kept). In the guest, ZV510B's names stay, as in ZV510C, ZV510D, ZV510E and ZVWRFB, whose guest-side scripts this task reuses: trees under `/var/tmp/zv`, logs and scripts under `/var/log/zv510b`, and `zv510b` as the QEMU name and the guest's host name. Every command for the guest goes through `$L/g.sh '<command>'` (ssh as `debian`, bounded by `GT` seconds, default 120); its standard input reaches the guest.

Never run anything outside the steps of this task. If a result surprises you, stop and report it with the outputs.

## Plan and durations

| step | what | about |
| --- | --- | --- |
| 0 | state of this VM, records | 5 min |
| 1 | two trees (`f`, and `wr` for step 7), their checks, five scripts | 8 min |
| 2 | QEMU on this VM | 5 min |
| 3 | image, disk, key, seed | 5 min |
| 4 | B1: first boot, packages (ZV510C's and ZTS's), user `zts`, copies, GRUB | 17 min |
| 5 | B1: both trees built in full without `--enable-debug`, in one wave | 25 min |
| 6 | B2: `f`, the test 3 times with an unload and a reload after each, then `-T zvol_misc`, unload | 25 min |
| 7 | optional, B3: `wr`, the test 3 times, unload | 15 min |
| 8 | evidence on this VM | 3 min |
| 9 | restore | 15 min |

Note the time (UTC) at which step 0 starts: the decision of step 7 counts from it.

## Why

ZV510E (VM 107, 23/11) ran the branch (`f`) and master with only the test added (`m`) on Linux 5.10.0-46, with debug builds: `zvol_misc_open_contended` PASS 3/3 on `f` (use count 1 before and after each run, a clean unload each time), FAIL 3/3 on `m` (`The zfs module's use count rose from 1 to 4`, unload refused, nothing but the three leaked references holding the module), and `-T zvol_misc` on `f` PASS 17/17 in 00:03:21. Its guest build script, ZV510B's `gbuild.sh`, hard-codes `--enable-debug`. This task builds `f` with `gbuildr.sh`, the same script without `--enable-debug` (it passes any further configure option on, which step 5 uses for `wr`), and repeats ZV510E's branch part (its step 6) on that release module. Master's part (ZV510E's step 7, three boots) is not repeated: the leak is the kernel's (references `__blkdev_get()` takes and keeps), and master's `zvol_open()` returns `-ERESTARTSYS` the same way in either build.

What a release run adds: neither the change nor the leak involves an assertion, so a release module is expected to behave exactly as the debug one; but the pull request's VM evidence comes from debug modules only, and a release build is what users run. So the configure checks, the module parameter and the result lines expected here are ZV510E's, and only the build type differs: `config.log` says `DEBUG_ZFS='_without_debug'`, configure says `checking whether assertion support will be enabled... no`, and the guest's load line has no `(DEBUG mode)`. A srcversion hashes the module's sources only, so `f`'s is expected to equal the one ZV510E's debug build of the same commit reported (recorded, not a stop).

The leak. From Linux 4.16 to 5.10, `__blkdev_get()` (`fs/block_dev.c`) takes a reference on the gendisk and on its driver's module (zfs) on each pass of an open, and keeps them only for the first opener of the device: `if (!first_open) put_disk_and_module(disk);`. It sets `first_open` on a pass that finds `bd_openers` at 0, and does not clear it when `fops->open()` returns `-ERESTARTSYS` and it goes back to `restart:`. If another task opens the device before the restarted pass takes the disk's `bd_mutex`, that pass finds the device open, `zvol_open()` succeeds (the zvol is open, so it needs no namespace lock), and its references are never put: one zfs module reference per occurrence, held until the machine stops. On 5.12 and older, master's `zvol_open()` returns `-ERESTARTSYS` at once when a first open cannot take `spa_namespace_lock` (configure: `whether blkdev_get_by_path() handles ERESTARTSYS... yes`). Linux 5.11 reworked `__blkdev_get()`: there every open holds its own references.

The branch's change, in `module/os/linux/zfs/zvol_os.c`: `zvol_open()` waits for the lock for up to `zvol_open_timeout_ms` (1000 ms, in 10 ms steps) on every kernel, as it already did on 5.13 and newer, and only then returns `-ERESTARTSYS`. While it waits it holds `bd_mutex`, so the other openers of the device wait in `__blkdev_get()` before they reach the first-open path, instead of restarting next to it. `zvol_open_timeout_ms` becomes a module parameter on 5.10 too (on master it exists only from 5.13). The branch also documents the parameter in `zfs.4`.

The test, `zvol_misc_open_contended` (Linux only; it skips with `log_unsupported` on 5.11 and newer). It creates a zvol (`testpool/openvol`, 16M) and a second pool `testpool1` on a file in its work directory under `/var/tmp`, sets `zvol_open_timeout_ms` to 60000 where the parameter exists, reads the zfs module's use count (`/sys/module/zfs/refcnt`, the lowest of four reads), delays every write to `testpool1` by one second (`zinject -D 1000:64 -T write`), and runs `zpool reguid testpool1`, which holds `spa_namespace_lock` while it waits for a txg sync (at least four seconds with the delay). One second in, four processes open the zvol and each keeps it open for three seconds. One second later it checks that `zpool reguid` is still running, that the four openers have started and that none has opened the zvol. Then every open must succeed, and the use count, read again for up to 30 s, must be back to its value before the opens. The value before is not 0: the group's setup creates `testpool`, whose mounted root file system holds one reference, so it is about 1. Expected on `f`: the first opener waits in `zvol_open()` holding `bd_mutex` until the lock is free, the three others wait for `bd_mutex`, nobody restarts; PASS in about 20 s, the count unchanged, a use count of 0 after the run, and a clean unload.

`-T zvol_misc` once on `f`: 13 tests (8 from `common.run`, 5 from `linux.run`, the new one included) and the two groups' setup and cleanup, 17 results; ZV510E: 17 PASS in 00:03:21 on the debug build. On 5.10 this tree lacks the M-41 fix (a separate branch, `zvol-open-pin-state`): `zvol_misc_volmode_race`, or any test that removes a minor while udev opens it, can hit M-41's use-after-free in `zvol_open()` (a `general protection fault` at a `0x6b6b...` address in `__mutex_lock()` from `zvol_open()`, a `list_del corruption`, or a SLUB `Poison overwritten`). The window is narrow, and master has it too: such a finding is reported as M-41, not against this branch.

What would contradict the expected results: `f` failing with a rise of the use count (the change does not avoid the leak), or with `<k> of 4 opens completed while the lock was held` (the wait does not hold the openers), or any result that differs from ZV510E's debug run. Report as is, with no rerun.

A finding that is not this bug: on every tree here, `zvol_os_create_minor()` can leave the ZIL open, and a later minor creation then fails `VERIFY0P(zilog->zl_get_data)` in `zil_open()` (M-30, upstream PR 19847, in neither tree). Its signature is `VERIFY0P(zilog->zl_get_data) failed` and `PANIC at zil.c:<line>:zil_open()`, with `zil_open` and `zvol_os_create_minor` in the stack; on a release module `VERIFY` still fires. In ZTS it can show as `Property changes still running` with `zfs` stuck in `taskq_wait_outstanding`. It is handled as any finding (CAPTURE), and reported as M-30.

Step 7, optional. `wr` is the M-41 fix ported to wrouesnel's zinject framework (fork branch `zvol-open-pin-state-test-wr`): on Linux 5.10 and older, `zvol_os_remove_minor()` looks the disk's `block_device` up with `bdget_disk()` before `del_gendisk()`, then takes and drops its `bd_mutex` after it, so a removal waits for a `zvol_open()` that has read `disk->private_data` but not yet locked `zv`. Its test `zvol_misc_remove_open` holds such an open at the injection point `zvol_open:found` (`zinject -n zvol_open:found -A pause`) and requires `zfs set volmode=none` to wait for it. TASK-1123-ZVWRFB ran it in this same guest with a debug build: PASS 3/3 (and, without the fix, a FAIL followed by a GPF in `zvol_open()`). The framework (`189a309d464`, "zinject: build injection points only into debug builds") puts the points into a build without `--enable-debug` only when configure gets `--enable-inject-points`: a plain release build would report the test UNSUPPORTED. So `wr` is built with `--enable-inject-points` and without `--enable-debug`, as the release arms of TASK-1123-SLD5B-WR and TASK-1123-FBWR were. On 6.12 the fix is compiled out (`del_gendisk()` waits for the open there), so only a kernel such as this 5.10 shows the fix at work in a release build. Expected as ZVWRFB's w1 to w3: PASS in about 20 s each, the removal waiting for the held open, the open then failing with ENXIO, the use count unchanged, a clean unload. Its tree is fetched, checked and built with `f` (steps 1 and 5); it runs in a third boot, B3, only if step 7's conditions hold. A problem with `wr` before step 7 (its fetch, a check, its build) drops step 7 and nothing else.

What is tested (fork https://github.com/tigerblue77/zfs, both fetched by full SHA):

- `f`: branch `zvol-open-retry-before-restart`, head `2a073ee4564e123ac15a9ed8767a324b8c070649`, tree `478a233cdd61d23b72d91cabc72bb659db4a0844`, one commit on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (tree `0b23c141b417a8542546e1f7a32662bc82bc0f92`). It changes `module/os/linux/zfs/zvol_os.c` and `man/man4/zfs.4`, and adds the test (`tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_open_contended.ksh`, `tests/runfiles/linux.run`, `tests/zfs-tests/tests/Makefile.am`). Built in full: `gbuildr.sh f all 3`, that is `./configure --with-config=all` with no `--enable-debug`.
- `wr` (step 7 only): branch `zvol-open-pin-state-test-wr`, head `3e42136b8d9448d56eca1155a0a78d6328196595`, tree `73f4f8b0178dae5f7624126ff0327468267dec79`; HEAD^ `684a8aefc5e341247b103a892ae6e054eb0ab473` (a point with no pool at hand acts for any pool), HEAD~2 `5439f31735b1e8714b83c14f687098e51d048c8e` (the fix), HEAD~3 `189a309d464ca51c74d627fdbdd84b5992eade2d` (wrouesnel's `zinject-points-uplift`, 16 commits on master `a1daebfec3f`). Built in full: `gbuildr.sh wr all 3 --enable-inject-points`.

Each tree runs its own `zfs-tests.sh`, as in ZV510C.

The guest is ZV510C's, ZV510D's, ZV510E's and ZVWRFB's:

- **Image.** Debian 11 `generic` cloud image 20260821-2577: `https://cdimage.debian.org/images/cloud/bullseye/20260821-2577/debian-11-generic-amd64-20260821-2577.qcow2`, 365428736 bytes, kernel `linux-image-5.10.0-46-amd64` 5.10.262-1, cloud-init 20.4.1, python3 3.9.2, user `debian` with passwordless sudo.
- **Headers.** `linux-headers-5.10.0-46-amd64` from bullseye-security. As in ZV510D and ZV510E, step 4 points the guest's apt at archive.debian.org from the start.
- **QEMU.** `-accel kvm -cpu host -machine q35 -smp 6 -m 4096`, a 24 GiB qcow2 overlay on the read-only image, user-mode network with only `127.0.0.1:2251` forwarded to the guest's ssh, a cloud-init NoCloud seed ISO with a throwaway ssh key, and the serial console in `$L/serial-<boot>.log`.
- **Boots.** Two or three: B1 provisions and builds, B2 runs `f`, B3 (optional) runs `wr`. B2 and B3 each boot fresh with `slub_debug=P` (the GRUB drop-in made in B1 stays).
- **ZTS, as in ZV510C.** User `zts` with passwordless sudo; each tree's own `scripts/zfs-tests.sh -vKO` as `zts`: one test with `-t` and `timeout 900` (`gzts.sh`, ZV510C's), the group with `-T` and `timeout 1800` (`gztag.sh`, ZV510E's); three 4 GiB sparse file vdevs and loop devices under the guest's /var/tmp; results under the guest's /var/tmp/test_results. The zvol udev rules (the `/dev/zvol/<pool>/<volume>` links the tests need) come from `f`'s `scripts/zfs-helpers.sh -i`, at the start of B2; they stay for B3 (`wr`'s `zvol_id`, rules and helper scripts are `f`'s byte for byte).
- **Results.** The guest's `/var/log/zv510b` and `/var/tmp/test_results` are copied by tar over ssh into `$L/guest-<boot>/`. The serial logs hold every kernel message, even if the guest dies.

What this task changes on the VM, all temporarily, under the owner's standing rule (anything a test needs, provided the VM is left clean, up to date and working): the package `qemu-system-x86` from trixie and its dependencies, installed with `--no-install-recommends` (ZV510C added 20 packages on VM 106); `qemu-utils` and `genisoimage` only if step 0 finds them absent; `kvm_intel` loaded only if step 0 finds it not loaded; files under $D (deleted at step 9) and under $L (kept). No apt source, no GRUB file, no kernel and no file under /etc or /boot is touched, and the VM is not rebooted. Step 9 undoes all of it. The ZTS packages, the user `zts`, its sudoers file and the udev rules exist only in the guest.

The one key this task creates is the guest's throwaway ssh key pair, `$D/key` and `$D/key.pub`, made in step 3 with no passphrase, as in ZV510C to ZVWRFB: it opens only the disposable guest, through a port bound to 127.0.0.1, is never displayed or copied anywhere else, and is deleted with $D at step 9. No other credential file is created, displayed or copied.

**STOP-RESTORE.** If any part of this plan proves infeasible, do not improvise: no other image, compiler, kernel or accelerator, no edit of a tree or a script, no `[trusted=yes]`, no forced dpkg option. Record the outputs, stop the guest (GUEST-OFF), go to step 9 and report. Infeasible means any of these:

- apt would remove or replace an installed package on this VM;
- the image cannot be fetched, or its checksum differs twice;
- QEMU does not start, or KVM does not work;
- the guest does not boot, gets no network, or cannot install its packages;
- `f` does not configure or build in the guest, or a configure result or a count of step 5 for `f` differs from the expected one (its release checks included);
- a check of step 1 for `f` differs (TREE-F-OK, FILES-F-OK, `f`'s counts), SCRIPTS-OK fails, or GUEST-TREES-OK fails;
- B2 (or B3) boots without `slub_debug=P`;
- the modules do not load, or a load line says `(DEBUG mode)`.

The same kinds of failure for `wr` only (step 1, step 5) drop step 7: record them and go on with `f`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file, except the guest key described above.
- On this VM: no ZFS module is loaded, no pool is created or imported, no loop device is set up, no block device is touched; QEMU uses files under $D only. In the guest: the only pools are those ZTS and its tests create (`testpool...`; `zvol_misc_open_contended` adds `testpool1` on a file in its work directory), on its file vdevs and loop devices under /var/tmp; never `zpool import`; never `zfs-tests.sh -x`; no ZTS outside steps 6 and 7.
- The changes above are the only ones on this VM. In the guest the steps may change anything (it is deleted at the end), but nothing is run there outside the steps.
- No reboot of this VM is pre-approved. Stopping the guest by killing QEMU (GUEST-OFF from the TERM, then `-KILL`) is part of the plan. If this VM reboots by itself, record `uptime -s`, `journalctl --list-boots | tail -n 3` and `journalctl -k -b -1 --no-pager | grep -iE 'kvm|BUG|Oops|panic' | head -n 40`, then go to step 9.
- Every guest command goes through `$L/g.sh` with a `GT` bound. Every build and every ZTS run runs detached in the guest (`setsid nohup ... &`) and is polled. QEMU daemonises. A hang is any of these:
  - no ssh 5 minutes after a launch (8 minutes for B1);
  - cloud-init not done after 10 minutes;
  - a guest apt install still going after 20 minutes;
  - a build still going after 90 minutes;
  - a one-test ZTS run with no `zts-rc=` line 17 minutes after its start (`gzts.sh`'s `timeout 900` ends it at 15);
  - the `-T zvol_misc` run with no `zts-rc=` line 32 minutes after its start (`gztag.sh`'s `timeout 1800` ends it at 30);
  - a load or an unload (`zfs.sh`, `zfs.sh -u`) still going after 3 minutes (`GT=180` ends the ssh; the guest may still be at it: check with `lsmod`);
  - QEMU still there 2 minutes after a poweroff.
- **LOAD <tree>**, the module of a tree (`f`, or `wr` in step 7):

  ```sh
  L=/var/log/task-1123-zv510er
  GT=180 $L/g.sh 't=<tree>; sudo -n /var/tmp/zv/$t/scripts/zfs.sh; echo "rc=$?"; cat /sys/module/zfs/srcversion; sudo -n dmesg | grep "ZFS: Loaded module" | tail -n 1; ls -l /dev/zfs; cat /sys/module/zfs/parameters/zvol_open_timeout_ms 2>&1; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; sudo -n timeout 30 /var/tmp/zv/$t/zinject'
  ```

  Expected for `f`: rc 0, a srcversion equal to SF, the line `ZFS: Loaded module v2.4.99-1, ZFS pool version 5000, ZFS filesystem version 5` WITHOUT `(DEBUG mode)`, `/dev/zfs` (`crw-rw-rw-`), `1000`, `refcnt=0` and `No handlers registered.` For `wr`: rc 0, SW, the same load line without `(DEBUG mode)`, `/dev/zfs`, `cat: /sys/module/zfs/parameters/zvol_open_timeout_ms: No such file or directory` (`wr` lacks the change of `f`, and master compiles the parameter out on 5.10), `refcnt=0` and `No handlers registered.` A load line with `(DEBUG mode)` is a debug module where a release one was built: UNLOAD it, COPY-OUT, GUEST-OFF, STOP-RESTORE. A module that does not load: quote the error and `sudo -n dmesg | tail -n 30`, GUEST-OFF, STOP-RESTORE.
- **UNLOAD <tree>**, always alone in its `g.sh` call (never chain a poweroff after it: ZV510C lost its unload error that way):

  ```sh
  L=/var/log/task-1123-zv510er
  GT=180 $L/g.sh 't=<tree>; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; sudo -n /var/tmp/zv/$t/scripts/zfs.sh -u; echo "rc=$?"; lsmod | grep -cE "^(zfs|spl) "'
  ```

  `zfs.sh -u` exits 0 even when `rmmod` fails (the status of its unload function is not its exit status): an unload is judged by the module count after it and by the `rmmod` line, never by its rc. Clean: `refcnt=0`, nothing between it and `rc=0`, and `0`. Refused: `rmmod: ERROR: Module zfs is in use`, `rc=0` all the same, and `2`.
- **Pre-ZTS checks**, before every ZTS run (`<tree>` is the boot's tree):

  ```sh
  L=/var/log/task-1123-zv510er
  $L/g.sh 't=<tree>; lsmod | grep -E "^zfs "; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; cat /sys/module/zfs/srcversion; sudo -n timeout 30 /var/tmp/zv/$t/zpool list -H -o name; sudo -n timeout 30 /var/tmp/zv/$t/zinject; ls /sys/block | grep -E "^zd"; find /dev/zvol -type l 2> /dev/null; ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_open_contended.* /var/tmp/zvol_misc_remove_open.* 2> /dev/null | wc -l; sudo -n losetup -a; test ! -s /etc/zfs/zpool.cache && echo CACHE-OK; pgrep -a -x zed; pgrep -a -x lt-zed; df -h / | tail -n 1'
  ```

  Expected: one zfs line (its last number is the use count), `refcnt=0` (in step 7: the use count after the previous run of this boot, see there), this boot's srcversion (SF or SW), no pool, `No handlers registered.`, no zd device, `0`, no loop device, CACHE-OK, no zed, and at least 10G available on `/`. A leftover `/dev/zvol` link (ZOT and ZV510C saw a broken `/dev/zvol/testpool/testvol` after a first ZTS run) is recorded with `ls -l`, and is not a stop. Any other leftover (a pool, a handler, a zd device, a file vdev, a work directory, a tunable file, a loop device, a cache file, zed, `refcnt` above what is expected): quote it, with `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E "^(testpool|/)" | sort -u` for a cache file, run nothing more in this boot, copy the logs out (COPY-OUT), GUEST-OFF, and go to the next step of the plan. A handler left behind in step 7 means a `dd` may still be held at the point: say so. Your driver must stop at such a check: ZV510C's printed one and ran on.
- **Serial watch.** The pattern is `P='list_del|BUG|Oops|Poison|general protection|KASAN|use-after-free|VERIFY|ASSERT|PANIC|Kernel panic|blocked for more than'`. Before each run, note `N=$(sudo -n grep -cE "$P" $L/serial-<boot>.log)`. While it runs, every 30 s, check `sudo -n grep -cE "$P" $L/serial-<boot>.log` and `pgrep -f '^qemu-system-x86_64 -name zv510b'`. A count above N, or QEMU gone, is a finding: CAPTURE. (ZTS's `-K` writes `ZTS run <test>` lines to the kernel log; they do not match P.) The serial logs are root's: read them with `sudo -n`.
- **CAPTURE <tag>** (a finding, or a hang as defined above). Wait 60 s, then `sudo -n grep -nE "$P" $L/serial-<boot>.log | head -n 20 > $L/finding-<tag>.txt` (`hang-<tag>.txt` for a hang). If `GT=30 $L/g.sh true` answers, run the command below in the guest with `<tag>` and `<tree>` filled in (it dumps the blocked tasks to the kernel log first, then saves dmesg, threads, stacks, the handlers and the ZFS debug log), then COPY-OUT. If the guest does not answer, wait until 150 s after the finding, so that the guest's hung-task reports reach the serial log, and go on. From then on, run no zfs, zpool, zinject or `zfs.sh` command in that boot, and stop it with GUEST-OFF from the TERM. Then quote the first finding line and the 40 lines after it, from the guest's `dmesg-<tag>.txt` (or from the serial log if the guest did not answer), and say which process it names, whether `zvol_open`, `__mutex_lock`, `__mutex_remove_waiter`, `zvol_os_remove_minor` or `zvol_remove_minors_impl` appear in its trace, how long after the run's start T0 it came, and whether it is M-41's or M-30's signature (see "Why").

  ```sh
  L=/var/log/task-1123-zv510er
  GT=120 $L/g.sh 'GL=/var/log/zv510b; t=<tag>; z=<tree>; echo w | sudo -n tee /proc/sysrq-trigger > /dev/null; ps -eLo pid,tid,stat,comm,wchan:32 | grep -E " D|zvol|zfs|zpool|zinject|systemd-udevd|txg|dd|ksh" > $GL/ps-$t.txt; for i in $(ps -eLo tid=,stat= | awk "\$2 ~ /^D/ { print \$1 }"); do echo "== $i"; sudo -n cat /proc/$i/stack; done > $GL/stacks-$t.txt 2>&1; sudo -n timeout 10 /var/tmp/zv/$z/zinject > $GL/zinject-$t.txt 2>&1; sudo -n dmesg > $GL/dmesg-$t.txt; sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg > $GL/dbgmsg-$t.txt 2>&1; cat $GL/refcnt.txt'
  ```

- **REF-CAPTURE <tag>** (an unload that is refused, or a use count above what is expected). Do not power off first: run this in the guest, with `<tag>` and `<tree>` filled in, and quote its output:

  ```sh
  L=/var/log/task-1123-zv510er
  GT=120 $L/g.sh 'GL=/var/log/zv510b; t=<tag>; z=/var/tmp/zv/<tree>/zpool; { echo "refcnt=$(cat /sys/module/zfs/refcnt)"; echo "holders: $(ls /sys/module/zfs/holders)"; lsmod | grep -E "^(zfs|spl) "; echo "zd: $(ls /sys/block | grep -E "^zd" | tr "\n" " ")"; sudo -n losetup -a; sudo -n timeout 30 $z list 2>&1; grep -cE " (zfs|zvol) " /proc/self/mountinfo; sudo -n find /proc/[0-9]*/fd -maxdepth 1 \( -lname "/dev/zd*" -o -lname "/dev/zfs" \) -printf "%p -> %l\n" 2> /dev/null; ps -eo pid,stat,comm,wchan:32 | awk "\$2 ~ /^D/"; pgrep -a -x zed; sudo -n dmesg | tail -n 40; } > $GL/refs-$t.txt 2>&1; cat $GL/refs-$t.txt'
  ```

  Then the step says what follows; the boot ends with COPY-OUT and GUEST-OFF (a poweroff is fine: the module stays loaded, nothing else is stuck). A held module with no holder, no zd device, no pool, no mount, no open `/dev/zd*` or `/dev/zfs`, no D task and no zed is the restart leak of "Why".
- **COPY-OUT** (into this boot's `guest-<boot>`, e.g. `guest-b2`):

  ```sh
  L=/var/log/task-1123-zv510er; B=<boot>
  mkdir -p $L/guest-$B && GT=120 $L/g.sh 'tar -C /var/log -cf - zv510b' | tar -C $L/guest-$B -xf - && ls $L/guest-$B/zv510b
  GT=120 $L/g.sh 'sudo -n tar -C /var/tmp -cf - test_results' | tar -C $L/guest-$B -xf - && ls $L/guest-$B/test_results
  ```

  (In B1, before any ZTS run, only the first line; the second then reports that `test_results` does not exist.)
- **GUEST-OFF.** Run `$L/g.sh 'sudo -n systemctl poweroff'`, alone; a 255 status as the connection closes is normal. Poll `pgrep -f '^qemu-system-x86_64 -name zv510b'` every 10 s for 2 minutes. If QEMU is still there, run `sudo -n pkill -TERM -f '^qemu-system-x86_64 -name zv510b'`, wait 30 s more, then do the same with `-KILL`. After a finding or a hang, skip the poweroff and start at the TERM.
- `sudo -n fstrim -av` after deleting $D.

## STEP 0: state (read-only, about 5 minutes)

```sh
L=/var/log/task-1123-zv510er
date -u +%FT%TZ; hostname; uname -r; uptime -s; nproc; free -m; df -h / /var/tmp
lsmod | grep -E '^(zfs|spl) '; ls -l /dev/zfs 2>&1; sudo -n losetup -a; ls -d /var/tmp/task-1123-zv510er $L /var/tmp/task-1124-zv510e /var/log/task-1124-zv510e /var/tmp/task-1123-zvwrfb /var/log/task-1123-zvwrfb /var/tmp/task-1122-zv510d /var/log/task-1122-zv510d /var/tmp/task-1121-zv510c /var/log/task-1121-zv510c 2>&1
pgrep -a -x zed; pgrep -a -x lt-zed; pgrep -af qemu-system; git --version; python3 --version; curl --version | head -n 1
grep -m1 'model name' /proc/cpuinfo; echo "vmx=$(grep -cw vmx /proc/cpuinfo) svm=$(grep -cw svm /proc/cpuinfo)"
ls -l /dev/kvm 2>&1; lsmod | grep -E '^kvm'; cat /sys/module/kvm_intel/parameters/nested 2>&1; getent group kvm
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' qemu-system-x86 qemu-utils genisoimage 2>&1; command -v ssh ssh-keygen
dpkg-query -W -f='${binary:Package}\n' | grep -cE '^(pve-|proxmox-)'
ss -ltn | grep -c ':2251 '; env | grep -ciE '^(https?|ftp|no|all)_proxy='; apt-config dump | grep -ci proxy
sudo -n dmesg | grep -ciE 'kvm|vmx'
```

Expected:

- the start time (note it: step 7's decision counts from it);
- a 6.12 kernel (`6.12.118+deb13-cloud-amd64` on 106 and 107 since 23/11; record it: it must be the same at the end); nproc 8 on 105 and 106, 32 on 107;
- at least 5500 MiB available and 20 GiB free on /var/tmp;
- no zfs module, no /dev/zfs, no loop device, $D and $L absent, no zed, no qemu process; git present;
- `/var/tmp/task-1124-zv510e`, `/var/tmp/task-1123-zvwrfb`, `/var/tmp/task-1122-zv510d` and `/var/tmp/task-1121-zv510c` absent (those tasks restored, or never ran here); their `/var/log` directories present on the VM that ran them (on 107: ZV510E's and ZVWRFB's): record them, they are not touched;
- the CPU line of earlier reports, with `vmx` above 0 (each CPU has a `flags` and a `vmx flags` line, so on 107 about 64) and `svm=0`;
- `/dev/kvm` present, with `kvm_intel` and `kvm` loaded; record `nested` and the kvm group;
- `qemu-system-x86` not installed; record the versions of `qemu-utils` and `genisoimage` (an absent one joins the install of step 2);
- 0 Proxmox packages; the port count 0; both proxy counts 0.

Stop and report (nothing has been changed yet) if:

- `vmx=0`: KVM cannot run here (this task has no TCG variant); on 107 this means its reconfiguration dropped the CPU's virtualisation flags: report it for main;
- a zfs module, a pool, zed or a qemu process exists;
- one of the four `/var/tmp/task-...` directories above exists (that task is running here or did not restore);
- `qemu-system-x86` is installed (an earlier task did not restore);
- the port is taken;
- there is less memory or disk than above;
- a proxy is configured (the guest would not have it).

Then the records for step 9:

```sh
L=/var/log/task-1123-zv510er
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' | sort > $L/pkgs.step0; wc -l < $L/pkgs.step0; sudo -n dpkg --audit
getent group | cut -d: -f1 | sort > $L/groups.step0; getent passwd | cut -d: -f1 | sort > $L/users.step0; lsmod | awk 'NR > 1 { print $1 }' | sort > $L/modules.step0
sudo -n sha256sum /boot/grub/grub.cfg /etc/default/grub > $L/boot.step0; cat /proc/cmdline > $L/cmdline.step0; uptime -s > $L/uptime.step0
sysctl kernel.panic_on_oops kernel.panic kernel.softlockup_panic kernel.hung_task_panic | tee $L/sysctl.step0; ls -l /etc/zfs/zpool.cache 2>&1
ls /etc/apt/sources.list.d/ /etc/apt/preferences.d/; command -v dkms && sudo -n dkms status
systemctl is-active unattended-upgrades.service apt-daily.timer apt-daily-upgrade.timer
sudo -n apt-get -o DPkg::Lock::Timeout=300 update > $L/apt-update-0.log 2>&1; echo "rc=$?"; grep -E '^(W|E|Err):' $L/apt-update-0.log
apt list --upgradable 2>/dev/null | tail -n +2 | tee $L/upgradable.step0
```

Stop and report if `dpkg --audit` prints anything or `apt-get update` fails. Record the rest, including whether a zpool.cache exists (this task does not touch it).

## STEP 1: trees and scripts, on this VM (about 8 minutes)

One fetch per tree, by full SHA: depth 2 for `f` (its parent is `a1daebfec3f`), depth 4 for `wr` (its HEAD~3 is `189a309d464`).

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D && sudo -n chown "$(id -un):" $D && chmod 755 $D && mkdir $D/src $D/img $D/seed
mkdir $D/src/f && git -C $D/src/f init -q . && git -C $D/src/f fetch -q --depth 2 $F 2a073ee4564e123ac15a9ed8767a324b8c070649 && git -C $D/src/f checkout -q FETCH_HEAD && echo FETCH-OK-f
mkdir $D/src/wr && git -C $D/src/wr init -q . && git -C $D/src/wr fetch -q --depth 4 $F 3e42136b8d9448d56eca1155a0a78d6328196595 && git -C $D/src/wr checkout -q FETCH_HEAD && echo FETCH-OK-wr
printf '%s\n' "f 2a073ee4564e123ac15a9ed8767a324b8c070649 478a233cdd61d23b72d91cabc72bb659db4a0844 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92" > $L/tree-f.expected
printf '%s\n' "wr 3e42136b8d9448d56eca1155a0a78d6328196595 73f4f8b0178dae5f7624126ff0327468267dec79 684a8aefc5e341247b103a892ae6e054eb0ab473 5439f31735b1e8714b83c14f687098e51d048c8e 189a309d464ca51c74d627fdbdd84b5992eade2d" > $L/tree-wr.expected
echo "f $(git -C $D/src/f rev-parse HEAD) $(git -C $D/src/f rev-parse 'HEAD^{tree}') $(git -C $D/src/f rev-parse HEAD~1) $(git -C $D/src/f rev-parse 'HEAD~1^{tree}')" | diff $L/tree-f.expected - && echo TREE-F-OK
echo "wr $(git -C $D/src/wr rev-parse HEAD) $(git -C $D/src/wr rev-parse 'HEAD^{tree}') $(git -C $D/src/wr rev-parse HEAD~1) $(git -C $D/src/wr rev-parse HEAD~2) $(git -C $D/src/wr rev-parse HEAD~3)" | diff $L/tree-wr.expected - && echo TREE-WR-OK
git -C $D/src/f log --oneline --abbrev=11 -2; git -C $D/src/wr log --oneline --abbrev=11 -4; for t in f wr; do git -C $D/src/$t status --porcelain | wc -l; done; git -C $D/src/f diff --stat=80 HEAD~1 HEAD
```

Expected:

- FETCH-OK-f, FETCH-OK-wr, TREE-F-OK and TREE-WR-OK;
- `f`'s log, top first: `2a073ee4564 Linux: retry a contended zvol_open() before restarting it` and `a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM`;
- `wr`'s log, top first: `3e42136b8d9 ZTS: remove a zvol's minor while an open has not locked it`, `684a8aefc5e zinject: let a point with no pool at hand act for any pool`, `5439f31735b Linux: wait for zvol_open() before freeing a removed zvol`, `189a309d464 zinject: build injection points only into debug builds`;
- `0` and `0` (both clean);
- what `f` adds to master:

  ```
   man/man4/zfs.4                                     |   7 +
   module/os/linux/zfs/zvol_os.c                      |  28 ++--
   tests/runfiles/linux.run                           |   4 +-
   tests/zfs-tests/tests/Makefile.am                  |   1 +
   .../zvol/zvol_misc/zvol_misc_open_contended.ksh    | 175 +++++++++++++++++++++
   5 files changed, 197 insertions(+), 18 deletions(-)
  ```

Then the files and the counts:

```sh
D=/var/tmp/task-1123-zv510er
cd $D/src && printf '%s\n' "4dd13f257f0cbb4b089354c3faf9edcffabcb0b3371f30852f72570b603a2959  f/module/os/linux/zfs/zvol_os.c" "131908f2f6cfce2a53951cccce9444c01ee4fb1ec494a5ca6061bd3e5feb6126  f/man/man4/zfs.4" "89a3feffa561e1c158f6c9b7a487bfd3137138af401719fc96a4dfda86246c5e  f/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_open_contended.ksh" "6a87c6b6a88d3ed2307daeb289f6b94922c5af89ed795b8e8b00f9c2de5d4f5c  f/tests/runfiles/linux.run" "c29af780e5c0ec130cfd1f257e73aae09f2ee1d03c0ca0b0595d5e9014d531ea  f/tests/zfs-tests/tests/Makefile.am" "a2a35a52e887dacd6b1e2eb6aeca90267557cbb4e38795872f0ef347fc06fc6d  f/config/kernel-blkdev.m4" | sha256sum -c && echo FILES-F-OK
cd $D/src && printf '%s\n' "f2f57fc2e9661e850f2af61455c29a4c024d2233b443d9b200e1de82c3c3625c  wr/module/os/linux/zfs/zvol_os.c" "14db830c60c3a4f42142cb5e09ed9d5c51ae161b0f65fc605fb8d0e2b14d6a65  wr/config/kernel-blkdev.m4" "0a9b63bb56c944837e8587df58bfabaf6493636d2ca64a49a8220f4fd62d8ac6  wr/module/zfs/zio_inject.c" "f0f80d2ff128a4bd794733b3c6c01033dd4cd9963ce9806188804d4e48f7b7e7  wr/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh" "3ff8fc09db72e44fbeec3acbabff0285fd5c43c7c01985c068077115e2382b85  wr/tests/zfs-tests/include/libtest.shlib" | sha256sum -c && echo FILES-WR-OK
cd $D/src && for t in f wr; do z=$t/module/os/linux/zfs/zvol_os.c; echo "$t $(grep -c HAVE_BLKDEV_BD_MUTEX $z) $(grep -c HAVE_BLKDEV_GET_ERESTARTSYS $z) $(grep -c '^module_param(zvol_open_timeout_ms' $z) $(grep -c '"zvol_open:found"' $z) $(grep -c bdget_disk $z) $(grep -c zvol_open_timeout_ms $t/man/man4/zfs.4) $(grep -c 'AC_ARG_ENABLE(\[inject-points\]' $t/config/zfs-build.m4) $(grep -c zvol_misc_open_contended $t/tests/runfiles/linux.run) $(grep -c zvol_misc_remove_open $t/tests/runfiles/linux.run)"; done
cd $D/src && ls -l f/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_open_contended.ksh wr/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh | cut -c1-10
```

Expected:

- six `OK` lines and FILES-F-OK, five `OK` lines and FILES-WR-OK (the `f` files are ZV510E's, the `wr` files ZVWRFB's);
- the counts `<tree> <BD_MUTEX> <GET_ERESTARTSYS> <module_param> <the point> <bdget_disk> <zfs.4> <--enable-inject-points> <open_contended in linux.run> <remove_open in linux.run>`: `f 0 1 1 0 0 1 0 1 0` and `wr 2 4 1 1 2 0 1 0 1`. `f` keeps one `HAVE_BLKDEV_GET_ERESTARTSYS` conditional, around a `schedule()`, and documents the parameter; `wr` has master's four, the fix's two `HAVE_BLKDEV_BD_MUTEX` blocks around `bdget_disk()`, the point, and configure's `--enable-inject-points`;
- `-rwxr-xr-x` twice (both tests executable).

If a fetch fails, wait 1 minute and try once more. A difference for `f`: go to step 9 and report as is (only $D and $L exist yet). A difference for `wr` only (its fetch, TREE-WR-OK, FILES-WR-OK, its counts or its log): quote it, `rm -rf $D/src/wr`, write WR-DROPPED and the reason in your notes, and go on with `f` alone (step 7 will not run). Then the expected lines of the trees kept:

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er
cat $L/tree-f.expected > $L/tree.expected; test -d $D/src/wr && cat $L/tree-wr.expected >> $L/tree.expected; cat $L/tree.expected; ls $D/src
```

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF`, in this order, to $L/g.sh, $L/qemu.sh, $L/gbuildr.sh, $L/gzts.sh and $L/gztag.sh. `g.sh` and `qemu.sh` run on this VM: they are ZV510E's with this task's $D and $L and its id in their first comment. The other three run in the guest. `gbuildr.sh` is new: ZV510B's `gbuild.sh` (`./configure --enable-debug --with-config=<all|kernel>`) without `--enable-debug`, and with the arguments after the third passed on to configure (step 5 gives `wr` its `--enable-inject-points`). `gzts.sh` and `gztag.sh` are ZV510E's byte for byte (same sums). Then:

```sh
L=/var/log/task-1123-zv510er
chmod 755 $L/g.sh
printf '%s\n' "fca543675286eec4266f0e3b5cb0c0187a88d56879713ea093d248f62e27fc91  $L/g.sh" "71cd5ed2d0efc4e39d5a66d4d7657cdad21383bc937b9de5fda54ccf418d4d04  $L/qemu.sh" "30c9f51d3cfa09bf05afc4287a8ab3a37320110cfd5fab6e58ade2bd8a57a79d  $L/gbuildr.sh" "5deb9b8402f01d22070ec0a17f55bcceec2398fbde4b9fc1092155f9c15f2955  $L/gzts.sh" "31cdf1e2274c735015fd31cdda5db39de4c8bf8e1e4475f3ac75343a6a15ee44  $L/gztag.sh" | sha256sum -c && for f in g.sh qemu.sh gbuildr.sh gzts.sh gztag.sh; do sh -n $L/$f || echo "SYNTAX $f"; done && echo SCRIPTS-OK
grep -c -- '--enable-debug' $L/gbuildr.sh
```

Expected five `OK` lines, SCRIPTS-OK, and `1` (the only mention of `--enable-debug` in `gbuildr.sh` is its comment). If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

```sh
#!/bin/sh
# TASK-1123-ZV510ER: run a command in the guest as debian: g.sh '<command>'
# Bounded by $GT seconds (default 120). Standard input goes to the guest.
D=/var/tmp/task-1123-zv510er
exec timeout "${GT:-120}" ssh -F none -i $D/key -p 2251 \
    -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=10 \
    -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=$D/known_hosts \
    -o LogLevel=ERROR debian@127.0.0.1 "$@"
```

```sh
#!/bin/sh
# TASK-1123-ZV510ER guest launcher, run as root: qemu.sh <boot tag> <kvm|tcg>
D=/var/tmp/task-1123-zv510er
L=/var/log/task-1123-zv510er
case $2 in
kvm)
    acc="-accel kvm -cpu host"
    ;;
tcg)
    acc="-accel tcg,thread=multi -cpu max"
    ;;
*)
    echo "usage: qemu.sh <boot tag> <kvm|tcg>" >&2
    exit 2
    ;;
esac
if pgrep -f '^qemu-system-x86_64 -name zv510b' > /dev/null; then
    echo "a zv510b guest is already running" >&2
    exit 1
fi
if [ -e $L/serial-$1.log ]; then
    echo "$L/serial-$1.log exists" >&2
    exit 1
fi
rm -f $D/qemu.pid
exec qemu-system-x86_64 -name zv510b $acc -machine q35 -smp 6 -m 4096 \
    -display none -monitor none -no-reboot -daemonize -pidfile $D/qemu.pid \
    -serial file:$L/serial-$1.log \
    -drive file=$D/guest.qcow2,if=virtio,format=qcow2 \
    -drive file=$D/seed.iso,if=virtio,format=raw,readonly=on \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2251-:22 \
    -device virtio-net-pci,netdev=n0
```

```sh
#!/bin/sh
# TASK-1123-ZV510ER guest release build, as debian:
# gbuildr.sh <tree> <all|kernel> <jobs> [<configure option> ...]
# ZV510B's gbuild.sh without --enable-debug.
t=$1
c=$2
j=$3
shift 3
cd /var/tmp/zv/$t || exit 1
./autogen.sh && ./configure --with-config=$c "$@" && nice make -j$j
echo "build-rc=$?"
```

```sh
#!/bin/sh
# TASK-1121-ZV510C guest ZTS run, as debian: gzts.sh <tree> <test>
# The tree's own zfs-tests.sh as user zts, as TASK-1121-ZOT runs it.
cd /var/tmp/zv/$1 || exit 1
timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t $2
echo "zts-rc=$?"
```

```sh
#!/bin/sh
# TASK-1124-ZV510E guest ZTS tag run, as debian: gztag.sh <tree> <tag>
# The tree's own zfs-tests.sh as user zts, as gzts.sh runs one test.
cd /var/tmp/zv/$1 || exit 1
timeout 1800 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T $2
echo "zts-rc=$?"
```

## STEP 2: QEMU on this VM (about 5 minutes)

As ZV510E's step 2, with this task's $L. Use as `<missing>` the packages among `qemu-utils` and `genisoimage` that step 0 found absent (usually none). Do not name an installed one: apt would upgrade it.

```sh
L=/var/log/task-1123-zv510er
apt-cache policy qemu-system-x86 | head -n 4
apt-get -s --no-install-recommends install qemu-system-x86 <missing> > $L/apt-sim.txt 2>&1; echo "rc=$?"
grep -E '^Inst' $L/apt-sim.txt; grep -E '^Remv|^Inst [^ ]+ \[' $L/apt-sim.txt; grep -E 'upgraded,' $L/apt-sim.txt
```

Expected `0 upgraded, <n> newly installed, 0 to remove`, with n about 20 (at most 40; ZV510C: 20 on VM 106); quote every `Inst` line; no `Remv` line and no `Inst <pkg> [<old version>]` line; every package from trixie (main, -updates or -security). A `Remv`, a replaced version, or more than 40 packages: STOP-RESTORE. Otherwise install:

```sh
L=/var/log/task-1123-zv510er
sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y --no-install-recommends -o DPkg::Lock::Timeout=300 install qemu-system-x86 <missing> > $L/apt-install.log 2>&1; echo "rc=$?"
grep -E '^(W|E):|[Ww]arning|[Ee]rror' $L/apt-install.log
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' | sort > $L/pkgs.installed; sudo -n dpkg --audit
awk 'NR == FNR { old[$1] = $2; next } !($1 in old) { print $1 }' $L/pkgs.step0 $L/pkgs.installed > $L/added-packages.txt; cat $L/added-packages.txt
awk 'NR == FNR { old[$1] = $2; next } ($1 in old) && old[$1] != $2 { print }' $L/pkgs.step0 $L/pkgs.installed
qemu-system-x86_64 --version | head -n 1; qemu-img --version | head -n 1; genisoimage --version 2>&1 | head -n 1
```

Expected rc 0; `added-packages.txt` the same set as the simulation, every one `ii`; the second awk prints nothing (a line there, from unattended-upgrades for instance, is reported, and its package is not in the purge list); `QEMU emulator version 10.0.x (Debian 1:10.0.x+ds-...)` and `qemu-img` of the same series.

KVM:

```sh
L=/var/log/task-1123-zv510er
test -e /dev/kvm || { sudo -n modprobe kvm_intel && touch $L/kvm-loaded-by-task; }; ls -l /dev/kvm; lsmod | grep -E '^kvm'
```

Expected `/dev/kvm` with `kvm_intel` and `kvm` loaded. A modprobe error: STOP-RESTORE.

## STEP 3: image, disk, key and seed (about 5 minutes)

ZV510C's step 3, with this task's $D and $L:

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er; B=https://cdimage.debian.org/images/cloud/bullseye/20260821-2577; I=debian-11-generic-amd64-20260821-2577.qcow2
curl -fsSL --retry 3 --max-time 120 -o $D/img/SHA512SUMS $B/SHA512SUMS; echo "rc=$?"
curl -fsSL --retry 3 --max-time 1800 -o $D/img/$I $B/$I; echo "rc=$?"; ls -l $D/img
printf '%s\n' "9f8d0417cdc9caa7509c50e71af97750e6e71a6990142b13d16f692f87c3562a  $D/img/SHA512SUMS" | sha256sum -c
printf '%s\n' "9f2b86f7f3f58c3ad418dcbe4aadd4a1f37159100f31a94203d22416f8a57f715e974248914b45fd6c636e1e7afe0f8c853aac3ec1dd85e27f401ed5b99b9c19  $I" > $L/image.sha512
grep -Fx -f $L/image.sha512 $D/img/SHA512SUMS > /dev/null && echo SUMS-LINE-OK; cd $D/img && sha512sum -c $L/image.sha512 && echo IMAGE-OK
```

Expected: both rc 0, the image at 365428736 bytes, `SHA512SUMS: OK`, SUMS-LINE-OK, `debian-11-generic-amd64-20260821-2577.qcow2: OK` and IMAGE-OK. A failed download or check: delete the image and download it once more. A second failure, or SHA512SUMS not OK: STOP-RESTORE.

```sh
D=/var/tmp/task-1123-zv510er; I=/var/tmp/task-1123-zv510er/img/debian-11-generic-amd64-20260821-2577.qcow2
chmod 444 $I; qemu-img info $I | grep -E '^(file format|virtual size|backing file)|corrupt'
qemu-img create -q -f qcow2 -F qcow2 -b $I $D/guest.qcow2 24G && qemu-img info --backing-chain $D/guest.qcow2 | grep -E '^(image|file format|virtual size|backing file):'
ssh-keygen -q -t ed25519 -N '' -C zv510b -f $D/key && ls -l $D/key $D/key.pub
printf '%s\n' '#cloud-config' 'ssh_authorized_keys:' "  - $(cat $D/key.pub)" 'ssh_pwauth: false' > $D/seed/user-data
printf '%s\n' 'instance-id: zv510b' 'local-hostname: zv510b' > $D/seed/meta-data
genisoimage -quiet -output $D/seed.iso -volid cidata -joliet -rock $D/seed/user-data $D/seed/meta-data && ls -l $D/seed.iso
```

Expected: the image `qcow2`, `virtual size: 3 GiB (3221225472 bytes)`, `corrupt: false`, no `backing file`; the overlay 24 GiB on the image; `key` at mode `-rw-------` (never display it) and `key.pub`; a seed ISO of a few hundred KiB.

## STEP 4: B1, the guest's first boot (about 17 minutes)

```sh
L=/var/log/task-1123-zv510er
sudo -n sh $L/qemu.sh b1 kvm > $L/qemu-b1.log 2>&1; echo "rc=$?"; cat $L/qemu-b1.log; pgrep -af '^qemu-system-x86_64 -name zv510b'
```

Expected rc 0, an empty log and one QEMU process. Any error (`Could not access KVM kernel module`, `failed to initialize kvm`, ...): STOP-RESTORE. Then poll every 10 s, for at most 8 minutes, `GT=20 $L/g.sh true && echo GUEST-UP` and `sudo -n grep -c 'Linux version 5.10.0-46-amd64' $L/serial-b1.log`. No GUEST-UP in time: quote `sudo -n tail -n 80 $L/serial-b1.log`, GUEST-OFF, STOP-RESTORE.

```sh
L=/var/log/task-1123-zv510er
GT=660 $L/g.sh 'cloud-init status --wait > /dev/null; cloud-init status --long | head -n 3'
$L/g.sh 'uname -r; cat /proc/cmdline; nproc; free -m | head -n 2; df -h / | tail -n 1; cat /etc/debian_version; systemd-detect-virt; grep -m1 "model name" /proc/cpuinfo; cat /sys/kernel/slab/kmalloc-1k/poison; cat /proc/sys/kernel/printk; sudo -n true && echo SUDO-OK'
$L/g.sh 'sudo -n install -d -o debian -g debian /var/log/zv510b /var/tmp/zv && sudo -n systemctl disable --now apt-daily.timer apt-daily-upgrade.timer 2> /dev/null; systemctl is-active apt-daily.timer apt-daily-upgrade.timer'
```

Expected, as in ZV510C:

- `status: done`, with a `detail` naming `DataSourceNoCloud` and the seed device;
- `5.10.0-46-amd64`;
- the command line `BOOT_IMAGE=/boot/vmlinuz-5.10.0-46-amd64 root=PARTUUID=d4879f80-3868-4600-ae3f-5407b53c3ea9 ro console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200 consoleblank=0`;
- `6`, about 3900 MiB, and / grown to about 24G;
- `11.x`, `kvm`, the host's CPU model, poison `0`, `7 4 1 7` and SUDO-OK;
- `inactive inactive`: the guest's apt timers stay out of the way.

cloud-init `error` with a working ssh is recorded, not a stop.

The guest's packages. As in ZV510D and ZV510E, the list goes to the archive first, which carries the same suites (`/sbin` tools are called by their path below, since `debian`'s PATH lacks them):

```sh
L=/var/log/task-1123-zv510er
printf '%s\n' 'deb http://archive.debian.org/debian bullseye main' 'deb http://archive.debian.org/debian bullseye-updates main' 'deb http://archive.debian.org/debian-security bullseye-security main' | $L/g.sh 'sudo -n tee /etc/apt/sources.list > /dev/null; sudo -n rm -f /etc/apt/sources.list.d/*.list; cat /etc/apt/sources.list'
GT=600 $L/g.sh 'sudo -n apt-get -o DPkg::Lock::Timeout=300 update > /var/log/zv510b/apt-update.log 2>&1; echo "rc=$?"; grep -E "^(W|E|Err):" /var/log/zv510b/apt-update.log'
```

Expected the three lines, rc 0, and no `Err:` or `E:`. A signature error or a failure: GUEST-OFF, STOP-RESTORE.

```sh
L=/var/log/task-1123-zv510er
GT=1200 $L/g.sh 'sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y --no-install-recommends -o DPkg::Lock::Timeout=300 install autoconf automake build-essential libtool libtool-bin pkg-config uuid-dev zlib1g-dev libblkid-dev libudev-dev libssl-dev libtirpc-dev libaio-dev libelf-dev git linux-headers-$(uname -r) > /var/log/zv510b/apt-install.log 2>&1; echo "rc=$?"; grep -E "upgraded,|^(W|E):" /var/log/zv510b/apt-install.log'
$L/g.sh 'gcc --version | head -n 1; ls -d /lib/modules/$(uname -r)/build /usr/src/linux-headers-5.10.0-46-amd64 /usr/src/linux-headers-5.10.0-46-common; dpkg-query -W -f="\${Package} \${Version}\n" linux-image-$(uname -r) linux-headers-$(uname -r) linux-kbuild-5.10 linux-compiler-gcc-10-x86 gcc-10 git; grep -E "^CONFIG_(SLUB|SLUB_DEBUG|DEBUG_LIST|BUG_ON_DATA_CORRUPTION|DETECT_HUNG_TASK)=" /boot/config-$(uname -r)'
```

Expected:

- rc 0, about 64 packages newly installed; a few `upgraded` is fine in the guest;
- `gcc (Debian 10.2.1-6) 10.2.1 20210110`, and the three directories;
- `linux-image-5.10.0-46-amd64`, `linux-headers-5.10.0-46-amd64`, `linux-kbuild-5.10` and `linux-compiler-gcc-10-x86`, all at `5.10.262-1`; `gcc-10 10.2.1-6`; git 2.30;
- `CONFIG_SLUB_DEBUG=y`, `CONFIG_SLUB=y`, `CONFIG_DEBUG_LIST=y`, `CONFIG_BUG_ON_DATA_CORRUPTION=y` and `CONFIG_DETECT_HUNG_TASK=y`.

A failure or a missing header package: GUEST-OFF, STOP-RESTORE.

ZTS's packages and its user, before the builds (`make` links the tools ZTS finds into each tree's `tests/zfs-tests/bin` as `debian`, and `zts` cannot add a link there later):

```sh
L=/var/log/task-1123-zv510er
GT=1200 $L/g.sh 'sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y --no-install-recommends -o DPkg::Lock::Timeout=300 install ksh fio bc jq acl attr xxhash pamtester rsync parted lsscsi python3 > /var/log/zv510b/apt-install-zts.log 2>&1; echo "rc=$?"; grep -E "upgraded,|^(W|E):" /var/log/zv510b/apt-install-zts.log'
$L/g.sh 'dpkg-query -W -f="\${Package} \${Version}\n" ksh fio xxhash jq python3; command -v ksh fio xxh128sum bc jq pamtester rsync parted lsscsi getfacl getfattr python3; readlink -f /bin/ksh; python3 --version'
$L/g.sh 'sudo -n useradd -m -s /bin/bash zts && echo "zts ALL=(ALL) NOPASSWD: ALL" | sudo -n tee /etc/sudoers.d/zts > /dev/null && sudo -n chmod 440 /etc/sudoers.d/zts && sudo -n visudo -c > /dev/null && echo SUDOERS-OK; id zts; sudo -n -u zts sudo -n id -un'
```

Expected rc 0 (about 34 packages, none upgraded or removed); the versions of ZV510C (`ksh 2020.0.0+really93u+20120801-9`, `fio 3.25-2`, `xxhash 0.8.0-2`, `jq 1.6-2.1+deb11u...`, `python3 3.9.2-3`); a path for each of the twelve commands; `/bin/ksh93` (or `/usr/bin/ksh93`); `Python 3.9.2`; SUDOERS-OK, the `zts` id line and `root`. A failure: GUEST-OFF, STOP-RESTORE.

Trees and scripts into the guest (`wr` only if step 1 kept it):

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er
tar -C $D/src -cf - $(ls $D/src) | GT=900 $L/g.sh 'tar -C /var/tmp/zv -xf - && echo COPIED'
$L/g.sh 'cd /var/tmp/zv; echo "f $(git -C f rev-parse HEAD) $(git -C f rev-parse "HEAD^{tree}") $(git -C f rev-parse HEAD~1) $(git -C f rev-parse "HEAD~1^{tree}")"; if test -d wr; then echo "wr $(git -C wr rev-parse HEAD) $(git -C wr rev-parse "HEAD^{tree}") $(git -C wr rev-parse HEAD~1) $(git -C wr rev-parse HEAD~2) $(git -C wr rev-parse HEAD~3)"; fi' | diff $L/tree.expected - && echo GUEST-TREES-OK
for f in gbuildr.sh gzts.sh gztag.sh; do $L/g.sh "cat > /var/log/zv510b/$f" < $L/$f || echo "COPY-FAILED $f"; done
$L/g.sh 'cd /var/log/zv510b && sha256sum gbuildr.sh gzts.sh gztag.sh'
```

Expected COPIED, GUEST-TREES-OK, and the three sums of step 1. Then the guest's boot command line, for B2 and B3:

```sh
L=/var/log/task-1123-zv510er
printf '%s\n' 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT slub_debug=P"' | $L/g.sh 'sudo -n tee /etc/default/grub.d/99-zv510b.cfg'
GT=300 $L/g.sh 'sudo -n update-grub > /var/log/zv510b/update-grub.log 2>&1; echo "rc=$?"; grep -m1 -E "^[[:space:]]*linux[[:space:]]" /boot/grub/grub.cfg'
```

Expected the drop-in line echoed (with `$GRUB_CMDLINE_LINUX_DEFAULT` literal), rc 0, and a `linux /boot/vmlinuz-5.10.0-46-amd64 root=PARTUUID=... ro console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200 consoleblank=0  slub_debug=P` line (one or two spaces before `slub_debug`). Anything else: GUEST-OFF, STOP-RESTORE. The drop-in stays: every later boot gets `slub_debug=P`.

## STEP 5: release builds in the guest, in B1 (about 25 minutes)

One wave: both trees in full without `--enable-debug`, `wr` with `--enable-inject-points` (without it a non-debug build of that framework has no injection points, and its test reports UNSUPPORTED), 3 jobs each on the guest's 6 vCPUs and 4 GiB, as ZV510E's wave:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'setsid nohup sh /var/log/zv510b/gbuildr.sh f all 3 > /var/log/zv510b/build-f.log 2>&1 < /dev/null & setsid nohup sh /var/log/zv510b/gbuildr.sh wr all 3 --enable-inject-points > /var/log/zv510b/build-wr.log 2>&1 < /dev/null & echo STARTED'
```

If step 1 dropped `wr`, start `f` alone with 6 jobs: `$L/g.sh 'setsid nohup sh /var/log/zv510b/gbuildr.sh f all 6 > /var/log/zv510b/build-f.log 2>&1 < /dev/null & echo STARTED'`.

Poll every 2 to 5 minutes with `$L/g.sh 'grep -H build-rc= /var/log/zv510b/build-*.log; uptime'`, and once, halfway, `free -m | head -n 2` on this VM. Each build must end `build-rc=0`. Then:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'cd /var/tmp/zv; K=$(ls -d f wr 2> /dev/null); for t in $K; do echo "== $t"; grep -E "^checking kernel (source|build) directory|^checking kernel source version|bdget_disk\(\) and bd_mutex|blkdev_get_by_path\(\) handles ERESTARTSYS|assertion support will be enabled|injection points will be built" /var/log/zv510b/build-$t.log; grep -E "^DEBUG_ZFS=" $t/config.log; done; for t in $K; do echo "$t $(grep -c "define HAVE_BLKDEV_BD_MUTEX 1" $t/zfs_config.h) $(grep -c "define HAVE_BLKDEV_GET_ERESTARTSYS 1" $t/zfs_config.h) $(/sbin/modinfo -F parm $t/module/zfs.ko | grep -c "^zvol_open_timeout_ms:") $(nm $t/module/zfs.ko | grep -cE " U bdget_disk\$") $(nm $t/module/zfs.ko | grep -cE " T zio_handle_func_injection\$") $(grep -c -a "zvol_open:found" $t/module/zfs.ko) $(grep -c "define ZFS_DEBUG 1" $t/zfs_config.h) $(grep -c -a "DEBUG mode" $t/module/zfs.ko)"; done; grep "warning:" /var/log/zv510b/build-*.log | grep -v "objtool: can.t decode instruction"; for t in $K; do echo "$t $(/sbin/modinfo -F vermagic $t/module/zfs.ko) | $(readelf -p .comment $t/module/zfs.ko | grep -m1 -o "GCC: .*") | $(/sbin/modinfo -F srcversion $t/module/zfs.ko)"; done; for t in $K; do ls -d $t/zpool $t/zfs $t/zinject $t/udev/zvol_id $t/tests/test-runner/bin/test-runner.py $t/tests/zfs-tests/bin/ksh; echo "$t links $(ls $t/tests/zfs-tests/bin | wc -l)"; done; df -h / | tail -n 1; du -sh $K'
```

Expected:

- for `f`, in log order: `checking kernel source directory... /usr/src/linux-headers-5.10.0-46-common`, `checking kernel build directory... /usr/src/linux-headers-5.10.0-46-amd64`, `checking kernel source version... 5.10.0-46-amd64`, `checking whether blkdev_get_by_path() handles ERESTARTSYS... yes` and, after the kernel checks, `checking whether assertion support will be enabled... no`; no `bdget_disk() and bd_mutex` line (that check is the M-41 fix's) and no `injection points` line (`f` has master's configure); then `DEBUG_ZFS='_without_debug'`;
- for `wr`, the same four kernel lines with `checking whether bdget_disk() and bd_mutex are available... yes` among them, then `checking whether assertion support will be enabled... no` and `checking whether zinject injection points will be built... yes`; then `DEBUG_ZFS='_without_debug'`;
- the counts `<tree> <BD_MUTEX> <GET_ERESTARTSYS> <zvol_open_timeout_ms parameter> <bdget_disk import> <zio_handle_func_injection> <the point in zfs.ko> <ZFS_DEBUG defined> <"DEBUG mode" in zfs.ko>`: `f 0 1 1 0 0 0 0 0` and `wr 1 1 0 1 1 <n> 0 0`, n any number above 0 (lines of `zfs.ko` holding the point's name). Both define `HAVE_BLKDEV_GET_ERESTARTSYS` (5.10 restarts opens); only `f`'s module has the parameter; only `wr`'s imports `bdget_disk` and carries the injection framework and the point; neither defines `ZFS_DEBUG` or carries the "(DEBUG mode)" string: both are release builds;
- no `warning:` line beyond objtool's `can't decode instruction` notes in `module/icp/asm-x86_64` objects (filtered out: 5.10's objtool predates those instructions); quote any other (a release build has no `-Werror`, so a warning does not stop it);
- for each tree, vermagic `5.10.0-46-amd64 SMP mod_unload modversions`, `GCC: (Debian 10.2.1-6) 10.2.1 20210110`, and its srcversion: note SF (`f`'s; expected equal to the SF of ZV510E's debug build of the same commit, recorded either way) and SW (`wr`'s);
- the six paths of each tree, without error; more than 200 links in each `tests/zfs-tests/bin` (232 on the VMs; a few fewer here, the tools left out of the guest);
- at least 10G available on `/` (about 19G expected); note the sizes.

For `f`: a failed configure or build (quote the last 40 lines of its log and the failing check of its `config.log`), `ERESTARTSYS ... no`, `assertion support ... yes`, `_with_debug`, a count other than the expected one, a compiler other than gcc 10.2.1, or `ksh` missing from its `tests/zfs-tests/bin` (user `zts` could not run that tree's ZTS): GUEST-OFF, STOP-RESTORE. For `wr`, the same (or `injection points will be built... no`, or a point count of 0): quote it and drop step 7 (WR-DROPPED), nothing else.

COPY-OUT with `B=b1` (its first line only), then GUEST-OFF (with the poweroff).

## STEP 6: B2, the branch `f` on 5.10, release (about 25 minutes)

```sh
L=/var/log/task-1123-zv510er
sudo -n sh $L/qemu.sh b2 kvm > $L/qemu-b2.log 2>&1; echo "rc=$?"; cat $L/qemu-b2.log
```

Poll for GUEST-UP as in step 4 (at most 5 minutes), then:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'uname -r; cat /proc/cmdline; cat /sys/kernel/slab/kmalloc-1k/poison /sys/kernel/slab/kmalloc-2k/poison; sudo -n sysctl kernel.panic_on_oops kernel.panic kernel.softlockup_panic kernel.hung_task_panic kernel.hung_task_timeout_secs; systemctl is-system-running; systemctl --failed --no-legend; lsmod | grep -cE "^(zfs|spl) "'
```

Expected:

- `5.10.0-46-amd64`, with a command line ending ` slub_debug=P`;
- poison `1` and `1`;
- `panic_on_oops = 0`, `panic = 0`, `softlockup_panic = 0`, `hung_task_panic = 0` and `hung_task_timeout_secs = 120`: an oops leaves the guest up for the capture, and a hung task is reported after 120 s;
- `running` (or `degraded`: record the failed units), and `0` modules.

`panic_on_oops = 1`: set it to 0 with `sudo -n sysctl -w` and say so. Without `slub_debug=P`, or poison `0`: GUEST-OFF, STOP-RESTORE.

Then the zvol udev rules from `f` (they stay in the guest for B3), and `f`'s module:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'id zts; sudo -n -u zts sudo -n id -un; pgrep -a -x zed; pgrep -a -x lt-zed; sudo -n /var/tmp/zv/f/scripts/zfs-helpers.sh -iv > /var/log/zv510b/helpers.log 2>&1; echo "rc=$?"; cat /var/log/zv510b/helpers.log; sudo -n udevadm control --reload && echo UDEV-RELOADED'
```

Expected the `zts` id line, `root`, no zed; rc 0; the configuration block with `udevdir:` and `udevruledir:`; an `ln -s /var/tmp/zv/f/...` line for each of `zvol_id`, `60-zvol.rules`, `69-vdev.rules` and `90-zfs.rules`, and for the other helpers; UDEV-RELOADED. A `Source missing` line for another helper is recorded. One for these four, or a `File exists` line for one of them, means no `/dev/zvol` links: COPY-OUT with `B=b2`, GUEST-OFF, and go to step 8 without step 7.

LOAD f (expected values in the VM rules: no `(DEBUG mode)`). Note N0 = `$L/g.sh 'sudo -n dmesg | wc -l'`.

Then three runs of the test, `f1`, `f2` and `f3`, one after the other. For each: the pre-ZTS checks with `t=f`, the serial count N for `serial-b2.log`, then the run:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'cut -d" " -f1 /proc/uptime; setsid nohup sh /var/log/zv510b/gzts.sh f zvol_misc_open_contended > /var/log/zv510b/zts-f1.log 2>&1 < /dev/null & echo STARTED'
```

(`zts-f2.log` and `zts-f3.log` for the next two.) The first number printed is the run's start T0, in seconds since the guest's boot: the clock of its dmesg and serial timestamps. Note it. Poll every 30 s with `$L/g.sh 'grep -E "] Test: |zts-rc=" /var/log/zv510b/zts-f1.log'` until `zts-rc=` appears (a run takes about a minute with ZTS's set-up and clean-up), with the serial watch. Then the result of the run:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 't=f1; x=zvol_misc_open_contended; f=/var/log/zv510b/zts-$t.log; grep -E "] Test: |zts-rc=|Missing util|Permission denied" $f; d=$(sed -n "s/^Log directory:[[:space:]]*//p" $f); echo "dir=$d"; o=$(find "$d" -path "*/$x*/stdout"); grep -nE "^(NOTE|ERROR|ASSERTION|SUCCESS: (test|zinject|save_tunable|set_tunable32))|use count|reguid|openers|opens |leaks nothing|Only Linux" "$o"; cat /sys/module/zfs/parameters/zvol_open_timeout_ms 2>&1; sudo -n dmesg | tail -n +<N+1> | grep -E "VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|list_del|blocked for more than"; sudo -n dmesg | wc -l; sudo -n udevadm settle -t 30; echo "$t refcnt-after=$(cat /sys/module/zfs/refcnt)" | tee -a /var/log/zv510b/refcnt.txt'
```

with `<N>` the dmesg count after the previous run (N0 for `f1`; the second-last number printed is the next run's N; after an unload and a reload the count only grows, so it stays valid).

Expected for each of `f1` to `f3`, as ZV510E's on the debug build:

- `[<date and time>] Test: /var/tmp/zv/f/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_open_contended (run as root) [00:1x] [PASS]` (or `[00:2x]`; the runner prefixes each result line with a timestamp, and the group's `setup` and `cleanup` lines come with it, both PASS), and `zts-rc=0`;
- from the test's `stdout`, in this order: `ASSERTION: Opening a ZVOL while spa_namespace_lock is held leaks nothing`; `SUCCESS: save_tunable VOL_OPEN_TIMEOUT_MS`; `SUCCESS: set_tunable32 VOL_OPEN_TIMEOUT_MS 60000`; `NOTE: zfs module use count before the opens: <N>` (about 1); `SUCCESS: zinject -d /var/tmp/zvol_misc_open_contended.<suffix>/vdev -D 1000:64 -T write testpool1`; `SUCCESS: test 0 = 0` (`zpool reguid` returned 0); `NOTE: zfs module use count after the opens: <N>` (the same number); the pass line `Opening a ZVOL while spa_namespace_lock is held leaks nothing` (`log_pass` prints it as is); `NOTE: Performing local cleanup via log_onexit (cleanup)`; `SUCCESS: zinject -c all`;
- `1000` (the test's clean-up restored the parameter);
- from dmesg, no line;
- `f<k> refcnt-after=0`;
- in the runner's own output, ZTS's `Missing util(s):` list and a few `ln: ... Permission denied` lines for `fsck`, `newfs`, `compress` and `uncompress` (user `zts` cannot replace links that `make` made as `debian`), as in ZV510C: expected and harmless; record them.

After each of `f1`, `f2` and `f3`, PASS or FAIL: UNLOAD f (a clean unload expected), then LOAD f (the same expected values as the first load). That gives each run a fresh module and each a clean unload.

A FAIL (or KILLED): quote the last 40 lines of its runner log and of the test's `stdout`; `log_fail` prints its reason as is, with no prefix: one of `zpool reguid ended before the opens could be checked`, `Only <k> of 4 openers started`, `<k> of 4 opens completed while the lock was held`, `zpool reguid or an opener still runs after 120s` (with the stack of each `zpool` before it), `<k> of 4 opens failed`, `Only <k> of 4 opens completed`, or `The zfs module's use count rose from <N> to <M>`. Then UNLOAD f and LOAD f as after a PASS, and the next run. After a second FAIL in a row: UNLOAD f only, no further run in this boot (no `fz`), and the end of this step.

A `refcnt-after` above 0 (with a FAIL or not): REF-CAPTURE <tag> (with `<tree>` `f`) in place of the UNLOAD, no further run in this boot (the module can no longer be reloaded), and the end of this step. An UNLOAD that is refused: REF-CAPTURE <tag>-unload, then the same. A finding in the serial watch or in dmesg, or a hang: CAPTURE <tag> (with `<tree>` `f`), quote it as the VM rules say, then step 7 if its conditions hold, else step 8 (CAPTURE has copied the logs out and stopped the guest). That is a finding against the branch, unless it is M-41's or M-30's signature.

Then `fz`, the group once, after the pre-ZTS checks (with `t=f`) and with the serial watch:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'cut -d" " -f1 /proc/uptime; setsid nohup sh /var/log/zv510b/gztag.sh f zvol_misc > /var/log/zv510b/zts-fz.log 2>&1 < /dev/null & echo STARTED'
```

Note T0. Poll every 30 s with `$L/g.sh 'grep -cE "] Test: " /var/log/zv510b/zts-fz.log; grep -E "\[(FAIL|KILLED)\]|zts-rc=" /var/log/zv510b/zts-fz.log'` until `zts-rc=` appears. Then:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 't=fz; f=/var/log/zv510b/zts-$t.log; grep -cE "] Test: .*\[PASS\]" $f; grep -E "] Test: " $f | grep -vE "\[PASS\]"; grep -E "] Test: .*/zvol_misc_(open_contended|volmode_race) " $f; sed -n "/^Results Summary/,/^Log directory/p" $f; grep -E "zts-rc=|Missing util" $f; d=$(sed -n "s/^Log directory:[[:space:]]*//p" $f); o=$(find "$d" -path "*/zvol_misc_open_contended*/stdout"); grep -nE "use count|^SUCCESS: set_tunable32" "$o"; cat /sys/module/zfs/parameters/zvol_open_timeout_ms; sudo -n dmesg | tail -n +<N+1> | grep -E "VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|list_del|blocked for more than"; sudo -n dmesg | wc -l; sudo -n udevadm settle -t 30; echo "$t refcnt-after=$(cat /sys/module/zfs/refcnt)" | tee -a /var/log/zv510b/refcnt.txt'
```

Expected:

- `17`, then no line (every result PASS: 13 tests and the setup and cleanup of the two groups);
- the two named lines, both `[PASS]`: `zvol_misc_open_contended` in about 20 s (its fourth run on `f`), `zvol_misc_volmode_race` in about 30 to 40 s;
- `Results Summary` with one line, `PASS` and `17`, then `Running Time:` about 00:03:00 to 00:08:00 (ZV510E: 00:03:21 on the debug build), `Percent passed:` `100.0%` and the `Log directory:` line (the runner separates these fields with tabs);
- `zts-rc=0` and the `Missing util(s):` line;
- the set_tunable32 line and the two use count NOTEs with the same number;
- `1000`; from dmesg, no line; `fz refcnt-after=0`.

A SKIP is recorded with its reason (from that test's `stdout`: `log_unsupported` prints it as is). A FAIL (or KILLED) is quoted with the 20 lines before it in the runner log and the last 40 lines of its `stdout`, and reported as is, without rerun, saying whether `f/tests/test-runner/bin/zts-report.py.in` lists it as known and whether it is M-30's signature. A `refcnt-after` above 0: REF-CAPTURE fz (with `<tree>` `f`), and say which tests ran (a lock held for over a second can still restart an open on the branch, and leak: the pull request says so; this would show it happening in the group). A finding or a hang: CAPTURE fz, quote it as the VM rules say, COPY-OUT with `B=b2`, GUEST-OFF from the TERM, then step 7 if its conditions hold, else step 8.

End of this step (after `fz`, or wherever a rule above sent you here):

- the dmesg check since N0: `$L/g.sh 'sudo -n dmesg | tail -n +<N0+1> | grep -B10 -E "list_del|VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|blocked for more than"'` (none expected);
- UNLOAD f if the module is loaded and this boot has had no REF-CAPTURE, which is the case after `fz` (a clean unload expected; refused: REF-CAPTURE fz-unload);
- `$L/g.sh 'sudo -n dmesg > /var/log/zv510b/dmesg-f.txt; cat /var/log/zv510b/refcnt.txt'`;
- COPY-OUT with `B=b2`, then GUEST-OFF;
- `date -u +%FT%TZ`: the time this step ended.

## STEP 7, optional: B3, `wr` on 5.10, release with injection points (about 15 minutes)

Run this step only if all of these hold, and otherwise go to step 8 and say which one failed:

- step 1 kept `wr` and step 5 built it with the expected counts (no WR-DROPPED);
- step 6 reached its end with the udev rules installed (not the "no `/dev/zvol` links" exit);
- the time at the end of step 6 is at most 1 h 55 after the start of step 0 (this step takes about 15 minutes, steps 8 and 9 about 20: the cap is 2 h 30).

A fresh boot, whatever happened in B2:

```sh
L=/var/log/task-1123-zv510er
sudo -n sh $L/qemu.sh b3 kvm > $L/qemu-b3.log 2>&1; echo "rc=$?"; cat $L/qemu-b3.log
```

GUEST-UP (at most 5 minutes) and the checks of step 6, with the same expected values. Then, before the module loads, ZTS's own leftovers go (after a clean B2 there are none; a B2 that ended in a capture leaves its file vdevs, work directory, tunable files and cache file, and `zfs-tests.sh` refuses to start over an existing file vdev). Nothing is imported at this point:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'pgrep -a -x zed; ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_open_contended.* /var/tmp/zvol_misc_remove_open.* /etc/zfs/zpool.cache 2> /dev/null; sudo -n rm -rf /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_open_contended.* /var/tmp/zvol_misc_remove_open.*; if test -e /etc/zfs/zpool.cache; then sudo -n mv /etc/zfs/zpool.cache /var/log/zv510b/zpool.cache.b2; fi; echo CLEANED; ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules 2> /dev/null'
```

Expected no zed, no path listed before CLEANED (any listed path is quoted: it is what B2 left), then the rules file of step 6 (a link to `/var/tmp/zv/f/...`). LOAD wr (expected values in the VM rules: SW, no `(DEBUG mode)`, no `zvol_open_timeout_ms`, `No handlers registered.`). Note N0 = `$L/g.sh 'sudo -n dmesg | wc -l'`.

Then three runs of the test, `w1`, `w2` and `w3`, one after the other, all in this boot and with this one module load (as ZVWRFB's w1 to w3). For each: the pre-ZTS checks with `t=wr`, the serial count N for `serial-b3.log`, then the run:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 'cut -d" " -f1 /proc/uptime; setsid nohup sh /var/log/zv510b/gzts.sh wr zvol_misc_remove_open > /var/log/zv510b/zts-w1.log 2>&1 < /dev/null & echo STARTED'
```

(`zts-w2.log` and `zts-w3.log` for the next two.) Note T0. Poll every 30 s with `$L/g.sh 'grep -E "] Test: |zts-rc=" /var/log/zv510b/zts-w1.log'` until `zts-rc=` appears (one to two minutes), with the serial watch. Then the result of the run:

```sh
L=/var/log/task-1123-zv510er
$L/g.sh 't=w1; z=wr; x=zvol_misc_remove_open; f=/var/log/zv510b/zts-$t.log; grep -E "] Test: |zts-rc=|Missing util|Permission denied" $f; d=$(sed -n "s/^Log directory:[[:space:]]*//p" $f); echo "dir=$d"; o=$(find "$d" -path "*/$x*/stdout"); grep -nE "^(NOTE|ERROR|ASSERTION|SUCCESS: (test|dd|zinject))|zvol_open:found|was removed while|waits for an open that found it" "$o"; sudo -n timeout 30 /var/tmp/zv/$z/zinject; sudo -n dmesg | tail -n +<N+1> | grep -E "VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|list_del|blocked for more than"; sudo -n dmesg | wc -l; sudo -n udevadm settle -t 30; echo "$t refcnt-after=$(cat /sys/module/zfs/refcnt)" | tee -a /var/log/zv510b/refcnt.txt'
```

with `<N>` the dmesg count after the previous run (N0 for `w1`; the second-last number printed is the next run's N).

Expected for each of `w1` to `w3`, as ZVWRFB's on the debug build:

- `[<date and time>] Test: /var/tmp/zv/wr/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open (run as root) [00:1x] [PASS]` (or `[00:2x]`), the group's `setup` and `cleanup` lines PASS, and `zts-rc=0`;
- from the test's `stdout`, in this order: `ASSERTION: Removing the minor of a volume waits for an open that found it`; `SUCCESS: zinject -c <n>` (`verify_inject_points` registered a probe handler and removed it: the release module has the points); `SUCCESS: test -b /dev/zdN` and `SUCCESS: test -d /sys/block/zdN`; the `NOTE:` line that starts the `zinject` table, with wrouesnel's header `ID  POOL  POINT  ACTION  MATCH  INJECT  FROM  AT` (columns padded), a dashed line, then its one row: `<id>  testpool  zvol_open:found  pause  1  1  -  -` (a count above 1 in INJECT, the sixth column, is recorded, not a failure: the open may have retried in `zvol_open()` and passed the point again); `SUCCESS: test none = none`; `NOTE: zfs set volmode=none waits for the held open`; `SUCCESS: zinject -c <id>`; `NOTE: open: rc 1, dd: failed to open '/dev/zdN': No such device or address` (the open fails with ENXIO); `SUCCESS: test 0 = 0`, `SUCCESS: test 1 = 0 exited 1` and `SUCCESS: test -e /sys/block/zdN exited 1`, and the last `dd`'s `SUCCESS:` line; the pass line `Removing the minor of a volume waits for an open that found it`;
- `No handlers registered.` (the test cleared its handler);
- from dmesg, no line;
- `w<k> refcnt-after=0`;
- the `Missing util(s):` list and the `Permission denied` lines, as in step 6.

A FAIL (or KILLED): quote the last 40 lines of its runner log and of the test's `stdout`, then go on to the next run (after its pre-ZTS checks); two FAILs in a row: no further run in this boot. A FAIL that printed `The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned` contradicts "Why" (the fix did not hold the removal in the release build): say so. An UNSUPPORTED result means the module has no injection points (`verify_inject_points`): quote the test's `stdout`, no further run in this boot, and say so (step 5's counts said otherwise: report both).

`wr` lacks `f`'s change, so a first open restarted while `spa_namespace_lock` is busy can leak one reference here (ZVWRFB saw none). A `refcnt-after` above that run's `refcnt` before (with a PASS or not): REF-CAPTURE <tag> (with `<tree>` `wr`). If it shows only that restart leak (a held module with nothing holding it), go on with the next run: the next pre-ZTS check then expects this new count. Anything else holding the module is a finding to quote: run nothing more in this boot, COPY-OUT, GUEST-OFF, and step 8. A finding in the serial watch or in dmesg, or a hang: CAPTURE <tag> (with `<tree>` `wr`), quote it as the VM rules say, then step 8. That is a finding against the M-41 branch, unless it is M-30's signature.

End of this step (after `w3`, or wherever a rule above sent you here):

- the dmesg check since N0: `$L/g.sh 'sudo -n dmesg | tail -n +<N0+1> | grep -B10 -E "list_del|VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|blocked for more than"'` (none expected);
- UNLOAD wr if the module is loaded (a clean unload expected; refused: REF-CAPTURE w-unload, which is the expected outcome only if a run's `refcnt-after` rose);
- `$L/g.sh 'sudo -n dmesg > /var/log/zv510b/dmesg-wr.txt; cat /var/log/zv510b/refcnt.txt'`;
- COPY-OUT with `B=b3`, then GUEST-OFF.

## STEP 8: evidence on this VM (about 3 minutes)

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er; P='list_del|BUG|Oops|Poison|general protection|KASAN|use-after-free|VERIFY|ASSERT|PANIC|Kernel panic|blocked for more than'
pgrep -af qemu-system; uptime -s | diff $L/uptime.step0 - && echo NO-VM-REBOOT
sudo -n dmesg | grep -ciE 'kvm|vmx'; sudo -n dmesg | grep -iE 'kvm|vmx' | tail -n 10
for b in b1 b2 b3; do if sudo -n test -e $L/serial-$b.log; then echo "$b $(sudo -n grep -m1 -o 'Linux version [^ ]*' $L/serial-$b.log) | $(sudo -n grep -m1 -o 'Command line: .*' $L/serial-$b.log) | findings=$(sudo -n grep -cE "$P" $L/serial-$b.log)"; else echo "$b not booted"; fi; done
sort -u $L/guest-b*/zv510b/refcnt.txt; ls $L/guest-b*/zv510b/refs-*.txt 2> /dev/null
for t in f1 f2 f3 w1 w2 w3; do f=$(ls $L/guest-b*/zv510b/zts-$t.log 2> /dev/null | tail -n 1); if [ -n "$f" ]; then echo "$t $(grep -E '] Test: .*/zvol_misc_(open_contended|remove_open) ' $f) $(grep -o 'zts-rc=[0-9]*' $f)"; else echo "$t no log"; fi; done
f=$(ls $L/guest-b*/zv510b/zts-fz.log 2> /dev/null | tail -n 1); if [ -n "$f" ]; then echo "fz $(grep -cE '] Test: .*\[PASS\]' $f) PASS of $(grep -cE '] Test: ' $f) $(grep -o 'zts-rc=[0-9]*' $f)"; else echo "fz no log"; fi
du -h $D/guest.qcow2; df -h /var/tmp
```

Expected: no QEMU process and NO-VM-REBOOT; the kvm lines as at step 0 (quote any new error); the boots on `5.10.0-46-amd64`, B1 without `slub_debug=P` and B2 (and B3) with it; findings 0 in every boot (except where a capture was made); `refcnt-after=0` for f1, f2, f3, fz (and w1 to w3); the test lines (f PASS, and w PASS if step 7 ran; `no log` for w1 to w3 if it did not) and `fz 17 PASS of 17 zts-rc=0`; the overlay at about 5 GiB.

## STEP 9: restore (every STOP-RESTORE ends here)

As ZV510E's step 9, with this task's $D and $L. Make sure the guest is stopped first: if `pgrep -f '^qemu-system-x86_64 -name zv510b'` prints anything, do GUEST-OFF from the TERM.

```sh
D=/var/tmp/task-1123-zv510er; L=/var/log/task-1123-zv510er
test -s $L/added-packages.txt && sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::Lock::Timeout=300 purge $(cat $L/added-packages.txt) > $L/apt-purge.log 2>&1; echo "rc=$?"
grep -E '^(W|E):|[Ww]arning|[Ee]rror' $L/apt-purge.log
test -e $L/kvm-loaded-by-task && sudo -n modprobe -r kvm_intel kvm; lsmod | awk 'NR > 1 { print $1 }' | sort | diff $L/modules.step0 -
sudo -n rm -rf $D; sudo -n fstrim -av; df -h / /var/tmp
```

```sh
L=/var/log/task-1123-zv510er
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' | sort > $L/pkgs.end; diff $L/pkgs.step0 $L/pkgs.end; sudo -n dpkg --audit
getent group | cut -d: -f1 | sort | diff $L/groups.step0 -; getent passwd | cut -d: -f1 | sort | diff $L/users.step0 -
sudo -n sha256sum /boot/grub/grub.cfg /etc/default/grub | diff $L/boot.step0 -; diff $L/cmdline.step0 /proc/cmdline
ls -d /var/tmp/task-1123-zv510er /usr/share/qemu /usr/share/seabios 2>&1; ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: purge rc 0 (or `rc=1` from `test` if step 2 installed nothing); the module list as at step 0 (a module that QEMU or KVM loaded on demand may appear: list it, and leave it unless this task loaded it); $D gone, and the space back near step 0's figures; no package, group, user, boot-file or command-line difference (a version changed by unattended-upgrades during the task is the only acceptable package line: say so; a new group or user left by a QEMU package is reported and left for main); every path `No such file or directory` (`/etc/qemu` belongs to `qemu-guest-agent` on VM 106 and stays); the zpool.cache as at step 0.

Up to date: `sudo -n apt-get -o DPkg::Lock::Timeout=300 update > $L/apt-update-2.log 2>&1; echo "rc=$?"`, then `apt list --upgradable 2>/dev/null | tail -n +2`, compared with $L/upgradable.step0. If anything new is listed: `sudo -n apt-get -s upgrade | grep -E '^(Inst|Remv)|upgraded,'`; with no `Remv`, apply it with `sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::Lock::Timeout=300 upgrade > $L/apt-upgrade.log 2>&1` and quote its summary line; a new kernel ABI kept back is listed and left to main (it would change the boot kernel of later tasks). Then `sudo -n apt-get autoclean > /dev/null`.

Working: `uname -r` and `uptime -s` as at step 0, `systemctl is-system-running` and `systemctl --failed --no-legend` as at step 0, `lsmod | grep -E '^(zfs|spl) '` empty, and `sysctl kernel.panic_on_oops kernel.panic kernel.softlockup_panic kernel.hung_task_panic | diff $L/sysctl.step0 -`. Logs, scripts, serial logs and the guest logs stay in $L. `date -u +%FT%TZ`: the end time.

## Report

Write $L/report.md and give the same text as your final message:

1. **This VM at step 0:** start time, kernel, CPU with vmx and nproc, `/dev/kvm` and the kvm modules, memory, disk, `qemu-utils` and `genisoimage` before the task, the sysctls, zpool.cache, the directories of ZV510C, ZV510D, ZV510E and ZVWRFB. **Packages added:** every one with version and suite, and the QEMU version. **Setup:** FETCH-OK-f and -wr, TREE-F-OK and TREE-WR-OK; the two logs; `f`'s diffstat; FILES-F-OK and FILES-WR-OK; the counts and the modes; WR-DROPPED and why, if so; SCRIPTS-OK and the `--enable-debug` count of `gbuildr.sh`; the image checks; the overlay; the seed.
2. **Guest B1:** launch rc, time to GUEST-UP, cloud-init, kernel, command line, virt, CPU, `printk`; the apt sources, update rc, packages installed and upgraded (ZV510C's list and ZTS's); the ZTS versions, SUDOERS-OK, `zts`'s id; gcc, the header packages, the kernel config lines; COPIED, GUEST-TREES-OK, the three sums; the update-grub line. **Builds:** for each tree, rc and duration, the configure lines (the assertion and injection point lines included), `DEBUG_ZFS`, the counts line, warnings, vermagic, the GCC comment, SF and SW, the link counts, free space, tree sizes.
3. **Boots B2 (and B3):** launch rc, time to GUEST-UP, command line, poison, sysctls, failed units; every LOAD (rc, srcversion, load line, `/dev/zfs`, `zvol_open_timeout_ms` 1000 or absent, `refcnt`, handlers); for B2 the `zfs-helpers.sh` output; for B3 the leftovers before CLEANED and the rules file.
4. **Runs:** the table `tag | boot | tree | srcversion | result | duration | zts-rc | use count before -> after (the test's NOTEs) | failure line | zvol_open_timeout_ms after | dmesg or serial findings | refcnt-after | unload (refcnt, rmmod line, module count)` for f1, f2 and f3. Then `fz`: the PASS count, every result that is not PASS with its lines, the `zvol_misc_open_contended` and `zvol_misc_volmode_race` lines, the Results Summary, Running Time, zts-rc, the use count NOTEs, `refcnt-after` and the unload. Then, if step 7 ran, the table `tag | boot | tree | srcversion | result | duration | zts-rc | zinject row (MATCH, INJECT) | "waits for the held open" | open rc and message | failure line | handlers after | dmesg or serial findings | refcnt before -> after` for w1, w2 and w3, and the UNLOAD of B3; if it did not run, which condition stopped it. The `Missing util(s)` list.
5. **Unloads:** every UNLOAD (refcnt line, any `rmmod` line, rc, module count), and every REF-CAPTURE output in full.
6. **The question of this task:** on the release build of the branch, the use count before and after each run and the clean unloads, compared with ZV510E's debug results; whether this fits "Why" (the same results without `--enable-debug`) or contradicts it, and how. If step 7 ran: on `wr`'s release build, whether `zfs set volmode=none` waited for the held open in each run, with a clean unload. **Findings:** every CAPTURE, quoted as the VM rules say, M-41, M-30 or other; or "no VERIFY/ASSERT/PANIC/BUG/Oops/GPF/Poison/list_del/hung task in f1 to f3 and fz (and w1 to w3)".
7. **This VM after the guest:** no QEMU, NO-VM-REBOOT, its kvm dmesg lines, the serial summary of step 8, the overlay's size.
8. **Restore:** purge, module list, $D removed, the comparisons, upgrades applied or kept back, final state; the start and end times and the total.
9. **Verdict:** two or three lines, e.g. "zvol_misc_open_contended on 5.10.0-46 (Debian 11 guest, nested KVM, slub_debug=P, release build: DEBUG_ZFS='_without_debug', no (DEBUG mode)): branch 2a073ee4564 PASS 3/3, use count unchanged (1 to 1), clean unload after each run", "-T zvol_misc on 2a073ee4564, release: PASS 17/17 in <time>, use count 0 after, clean unload; no finding" and "zvol_misc_remove_open on 5.10.0-46, release with --enable-inject-points: 3e42136b8d9 PASS 3/3 (removal waited for the held open, open ENXIO, use count 0, clean unload)" or "step 7 not run: <why>". Then "VM never rebooted, packages as before", or say what differs. Then the deviations, and End.
