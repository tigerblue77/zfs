TASK-1123-ZVWRFB (from the coordinator session "main"): functional, about 1 h 50, at most 2 h 45, no reboot of this VM. M-41 on wrouesnel's zinject framework. It runs our regression test `zvol_misc_remove_open`, as ported to that framework, on Linux 5.10, the only kernels where its fix acts: three times on the fork branch `zvol-open-pin-state-test-wr` (the fix and the test: expected PASS), then once on its fail-before tree `zvol-open-pin-state-test-wr-fb` (the test without the fix: expected FAIL, then an oops in the disposable guest). TASK-1122-WRS ran the branch three times on 6.12 (PASS), but there `del_gendisk()` waits for the open with or without the fix, so that run did not exercise the fix, and TASK-1123-FBWR left this fail-before out for the same reason. The route is ZV510C's, reused by ZV510D and ZV510E: a disposable Debian 11 guest (Linux 5.10.0-46, `slub_debug=P`) under QEMU/KVM inside this VM; this VM keeps its kernel, its GRUB and its file systems, and is not rebooted. VM 106 is preferred, after ZV510D; VM 107 works as well (step 0 checks `vmx`). This task never runs beside ZV510D or ZV510E on one VM: all three use the same port, the same QEMU name and the same guest names. Other VMs may run functional tasks at the same time: this task measures nothing, but its guest keeps 6 vCPUs busy for about 25 minutes of builds, so not beside a measurement. Nothing is published on GitHub, and no credential file is touched except the guest's throwaway key (below). Report: `/var/log/task-1123-zvwrfb/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The commands and the scripts of step 1 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. On this VM (the host): `D=/var/tmp/task-1123-zvwrfb` (trees, image, guest disk, guest key; deleted at the end) and `L=/var/log/task-1123-zvwrfb` (logs, scripts, serial logs; kept). In the guest, ZV510B's names stay, as in ZV510C, ZV510D and ZV510E, whose guest-side scripts this task reuses byte for byte: trees under `/var/tmp/zv`, logs and scripts under `/var/log/zv510b`, and `zv510b` as the QEMU name and the guest's host name. Every command for the guest goes through `$L/g.sh '<command>'` (ssh as `debian`, bounded by `GT` seconds, default 120); its standard input reaches the guest.

Never run anything outside the steps of this task. If a result surprises you, stop and report it with the outputs.

## Plan and durations

| step | what | about |
| --- | --- | --- |
| 0 | state of this VM, records | 5 min |
| 1 | two trees, their checks, four scripts | 6 min |
| 2 | QEMU on this VM | 5 min |
| 3 | image, disk, key, seed | 5 min |
| 4 | B1: first boot, packages (ZV510C's and ZTS's), user `zts`, copies, GRUB | 17 min |
| 5 | B1: both trees built in full, in one wave | 25 min |
| 6 | B2: `wr`, the test 3 times, unload | 15 min |
| 7 | B3: `fb`, the test once, the oops, its capture | 15 min |
| 8 | evidence on this VM | 3 min |
| 9 | restore | 15 min |

## Why

M-41. On Linux 5.10 and older, `zvol_open()` runs under the whole disk's `bd_mutex`, which `del_gendisk()` does not take. `zvol_open()` reads `disk->private_data` and only then takes `zv->zv_state_lock`: between the two nothing holds `zv`, so a removal of the minor (`zvol_os_remove_minor()`) can free it, and the open then locks a freed `zvol_state_t`. The fix, `5439f31735b` "Linux: wait for zvol_open() before freeing a removed zvol" (the same patch as `e256741ff14`, which ZV510C ran under `race.sh`), applies where configure finds `bdget_disk()` and `bd_mutex` (`HAVE_BLKDEV_BD_MUTEX`; on 5.10, yes): `zvol_os_remove_minor()` looks the disk's `block_device` up with `bdget_disk()` before `del_gendisk()`, then takes and drops its `bd_mutex` after it, before `zv` is freed, so the removal waits for an open in progress. Newer kernels compile it out; from 5.14, `del_gendisk()` takes the `open_mutex` the open runs under.

The test, `tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh` (Linux only, in `linux.run`): it creates `testpool/removeopen` (64M, `volmode=dev`) and waits for udev; it holds a `dd` open of its device at the injection point `zvol_open:found` (`zinject -n zvol_open:found -A pause`), which sits right after the `private_data` read and before `mutex_enter(&zv->zv_state_lock)`; it sets `volmode=none` in the background, which removes the minor and returns once `zv` is freed, and requires `zfs set` to be still waiting 10 s after the property reads `none`; then it releases the open (`zinject -c <id>`), which must fail with ENXIO, `zfs set` must return 0, and `volmode=dev` again must give a readable device. The point has no pool at hand, so it passes a NULL spa; `684a8aefc5e` ("zinject: let a point with no pool at hand act for any pool") lets a handler on any pool act there, and both trees carry that change. In this framework a `-A pause` hold has no time bound: only `zinject -c` releases it (the test does, and so does its clean-up with `zinject -c all`).

Expected on `wr` (B2): PASS in about 20 s each time (10 s of it the deliberate wait), as ZV510C's z1 to z3 on the earlier version of the branch (`zvol-open-pin-state-test`, our own point framework); only the `zinject` table now has wrouesnel's columns. Expected on `fb` (B3): the test FAILs in its step 3 within seconds, `zfs set` having returned while the open was held: `NOTE: zfs set: rc 0, ` then `The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned`. Its clean-up's `zinject -c all` then releases the `dd` open, which locks the freed, poisoned `zvol_state_t`: a `general protection fault, probably for non-canonical address 0x6b6b6b6b6b6b6b..` in `__mutex_lock()` (or a helper of it) from `zvol_open()`, Comm `dd`. ZV510C saw exactly that on the earlier version (its fail-before tree `2027d8e2`: the same FAIL line, then the GPF). Neither depends on timing: the point opens the window on purpose.

What would contradict this: `wr` failing with the line above (the fix does not hold the removal), or `fb` passing (the removal waited without the fix). Either way, report as is, with no rerun.

The restart leak is not this test's subject. Neither tree has the change of the separate pull request `zvol-open-retry-before-restart` (ZV510E) or ZV510D's `fix2`, so on 5.10 a first open that `zvol_open()` restarts (`-ERESTARTSYS`, when `spa_namespace_lock` is busy) while another opener wins can leak one zfs module reference. In ZV510C the earlier version of this branch left no reference after its ZTS runs (clean unload). A use count above its value before a `wr` run is recorded and captured (REF-CAPTURE), is that leak unless REF-CAPTURE shows something else holding the module, and does not change the test's verdict.

A finding that is not this bug: on both trees, `zvol_os_create_minor()` can leave the ZIL open, and a later minor creation then fails `VERIFY0P(zilog->zl_get_data)` in `zil_open()` (M-30, upstream PR 19847, in neither tree). Its signature is `VERIFY0P(zilog->zl_get_data) failed` and `PANIC at zil.c:<line>:zil_open()`, with `zil_open` and `zvol_os_create_minor` in the stack. It is handled as any finding (CAPTURE), and reported as M-30.

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parents |
| --- | --- | --- | --- | --- |
| `wr` | `zvol-open-pin-state-test-wr` | `3e42136b8d9448d56eca1155a0a78d6328196595` | `73f4f8b0178dae5f7624126ff0327468267dec79` | HEAD^ `684a8aefc5e341247b103a892ae6e054eb0ab473` (the NULL-spa point), HEAD~2 `5439f31735b1e8714b83c14f687098e51d048c8e` (the fix), HEAD~3 `189a309d464ca51c74d627fdbdd84b5992eade2d` |
| `fb` | `zvol-open-pin-state-test-wr-fb` | `4072f6297815daf43ecbf058cea31725311ec509` | `b363dc7da2b9400eb82c894b289b71956d581fd6` | HEAD^ `46fa05849da56c9e8f2939011b3901cf4f845b33` (`684a8aefc5e`'s change), HEAD~2 `189a309d464ca51c74d627fdbdd84b5992eade2d` |

`189a309d464` is wrouesnel's `zinject-points-uplift` (16 commits on upstream master `a1daebfec3f`). `fb`'s tree is exactly `wr`'s with the fix `5439f31735b` reverted: the two trees differ in `config/kernel-blkdev.m4` and `module/os/linux/zfs/zvol_os.c` only. Both are built in full with `--enable-debug` (which builds the injection points in), and each runs its own `zfs-tests.sh`, as in ZV510C.

The test file, the same in both trees: mode `100755`, sha256 `f0f80d2ff128a4bd794733b3c6c01033dd4cd9963ce9806188804d4e48f7b7e7`.

| file | `wr` sha256 | `fb` sha256 |
| --- | --- | --- |
| `module/os/linux/zfs/zvol_os.c` | `f2f57fc2e9661e850f2af61455c29a4c024d2233b443d9b200e1de82c3c3625c` | `de241dd5bc40dac835afa8d289c9f674ce3b03f0adc926a4bce459cf1c1d427f` |
| `config/kernel-blkdev.m4` | `14db830c60c3a4f42142cb5e09ed9d5c51ae161b0f65fc605fb8d0e2b14d6a65` (ZV510D's `zt2`) | `a2a35a52e887dacd6b1e2eb6aeca90267557cbb4e38795872f0ef347fc06fc6d` (master's, ZV510E's) |
| `module/zfs/zio_inject.c` | `0a9b63bb56c944837e8587df58bfabaf6493636d2ca64a49a8220f4fd62d8ac6` | the same |
| `tests/zfs-tests/include/libtest.shlib` | `3ff8fc09db72e44fbeec3acbabff0285fd5c43c7c01985c068077115e2382b85` | the same |

The guest is ZV510C's, ZV510D's and ZV510E's:

- **Image.** Debian 11 `generic` cloud image 20260821-2577: `https://cdimage.debian.org/images/cloud/bullseye/20260821-2577/debian-11-generic-amd64-20260821-2577.qcow2`, 365428736 bytes, kernel `linux-image-5.10.0-46-amd64` 5.10.262-1, cloud-init 20.4.1, python3 3.9.2, user `debian` with passwordless sudo.
- **Headers.** `linux-headers-5.10.0-46-amd64` from bullseye-security. As in ZV510D and ZV510E, step 4 points the guest's apt at archive.debian.org from the start.
- **QEMU.** `-accel kvm -cpu host -machine q35 -smp 6 -m 4096`, a 24 GiB qcow2 overlay on the read-only image, user-mode network with only `127.0.0.1:2251` forwarded to the guest's ssh, a cloud-init NoCloud seed ISO with a throwaway ssh key, and the serial console in `$L/serial-<boot>.log`.
- **Boots.** Three: B1 provisions and builds, B2 runs `wr`, B3 runs `fb`. B2 and B3 each boot fresh with `slub_debug=P` (the GRUB drop-in made in B1 stays). B3 is the last: its kernel is tainted by the expected oops, and nothing runs on it after the capture.
- **ZTS, as in ZV510C.** User `zts` with passwordless sudo; each tree's own `scripts/zfs-tests.sh -vKO -t <test>` as `zts` with `timeout 900` (`gzts.sh`, ZV510C's); three 4 GiB sparse file vdevs and loop devices under the guest's /var/tmp; results under the guest's /var/tmp/test_results. The zvol udev rules (the `/dev/zvol/<pool>/<volume>` links the test needs) come from `wr`'s `scripts/zfs-helpers.sh -i`, at the start of B2; they stay for B3 (`fb`'s `zvol_id` and rules are `wr`'s byte for byte).
- **Results.** The guest's `/var/log/zv510b` and `/var/tmp/test_results` are copied by tar over ssh into `$L/guest-<boot>/`. The serial logs hold every kernel message, even if the guest dies.

What this task changes on the VM, all temporarily, under the owner's standing rule (anything a test needs, provided the VM is left clean, up to date and working): the package `qemu-system-x86` from trixie and its dependencies, installed with `--no-install-recommends` (ZV510C added 20 packages on VM 106); `qemu-utils` and `genisoimage` only if step 0 finds them absent; `kvm_intel` loaded only if step 0 finds it not loaded; files under $D (deleted at step 9) and under $L (kept). No apt source, no GRUB file, no kernel and no file under /etc or /boot is touched, and the VM is not rebooted. Step 9 undoes all of it. The ZTS packages, the user `zts`, its sudoers file and the udev rules exist only in the guest.

The one key this task creates is the guest's throwaway ssh key pair, `$D/key` and `$D/key.pub`, made in step 3 with no passphrase, as in ZV510C, ZV510D and ZV510E: it opens only the disposable guest, through a port bound to 127.0.0.1, is never displayed or copied anywhere else, and is deleted with $D at step 9. No other credential file is created, displayed or copied.

**STOP-RESTORE.** If any part of this plan proves infeasible, do not improvise: no other image, compiler, kernel or accelerator, no edit of a tree or a script, no `[trusted=yes]`, no forced dpkg option. Record the outputs, stop the guest (GUEST-OFF), go to step 9 and report. Infeasible means any of these:

- apt would remove or replace an installed package on this VM;
- the image cannot be fetched, or its checksum differs twice;
- QEMU does not start, or KVM does not work;
- the guest does not boot, gets no network, or cannot install its packages;
- a tree does not configure or build in the guest, or a configure result or a count of step 5 differs from the expected one;
- a check of step 1 differs (TREES-OK, the `diff -rq` lines, FILES-OK, the counts, SCRIPTS-OK), or GUEST-TREES-OK fails;
- B2 or B3 boots without `slub_debug=P`;
- the modules do not load.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file, except the guest key described above.
- On this VM: no ZFS module is loaded, no pool is created or imported, no loop device is set up, no block device is touched; QEMU uses files under $D only. In the guest: the only pools are those ZTS and its test create (`testpool...`), on its file vdevs and loop devices under /var/tmp; never `zpool import`; never `zfs-tests.sh -x`; no ZTS outside steps 6 and 7.
- The changes above are the only ones on this VM. In the guest the steps may change anything (it is deleted at the end), but nothing is run there outside the steps.
- **Reboots.** This VM is not rebooted by this task, and no reboot of it is pre-approved: the oops that `fb` is expected to cause stays inside the disposable guest. What is pre-approved, for B3 only, is to stop that guest by killing QEMU (GUEST-OFF from the TERM, then `-KILL`), since its kernel is tainted and the dead open keeps a disk's `bd_mutex`. If QEMU is still there 60 s after `-KILL`, record `ps -o pid,stat,wchan:32,args -p <pid>`, `sudo -n cat /proc/<pid>/stack` and `sudo -n dmesg | tail -n 60`, then go to step 8 and report: main decides about this VM. If this VM reboots by itself, record `uptime -s`, `journalctl --list-boots | tail -n 3` and `journalctl -k -b -1 --no-pager | grep -iE 'kvm|BUG|Oops|panic' | head -n 40`, then go to step 9.
- Every guest command goes through `$L/g.sh` with a `GT` bound. Every build and every ZTS run runs detached in the guest (`setsid nohup ... &`) and is polled. QEMU daemonises. A hang is any of these:
  - no ssh 5 minutes after a launch (8 minutes for B1);
  - cloud-init not done after 10 minutes;
  - a guest apt install still going after 20 minutes;
  - a build still going after 90 minutes;
  - a ZTS run with no `zts-rc=` line 17 minutes after its start (`gzts.sh`'s `timeout 900` ends it at 15);
  - a load or an unload (`zfs.sh`, `zfs.sh -u`) still going after 3 minutes (`GT=180` ends the ssh; the guest may still be at it: check with `lsmod`);
  - QEMU still there 2 minutes after a poweroff.
- **LOAD <tree>**, the module of a tree (`wr` or `fb`):

  ```sh
  L=/var/log/task-1123-zvwrfb
  GT=180 $L/g.sh 't=<tree>; sudo -n /var/tmp/zv/$t/scripts/zfs.sh; echo "rc=$?"; cat /sys/module/zfs/srcversion; sudo -n dmesg | grep "ZFS: Loaded module" | tail -n 1; ls -l /dev/zfs; cat /sys/module/zfs/parameters/zvol_open_timeout_ms 2>&1; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; sudo -n timeout 30 /var/tmp/zv/$t/zinject'
  ```

  Expected: rc 0, the tree's srcversion (SW for `wr`, SB for `fb`), `(DEBUG mode)`, `/dev/zfs` (`crw-rw-rw-`), `cat: /sys/module/zfs/parameters/zvol_open_timeout_ms: No such file or directory` (neither tree has the parameter on 5.10: it is compiled out where `HAVE_BLKDEV_GET_ERESTARTSYS` is defined), `refcnt=0` and `No handlers registered.` A module that does not load: quote the error and `sudo -n dmesg | tail -n 30`, GUEST-OFF, STOP-RESTORE.
- **UNLOAD <tree>**, always alone in its `g.sh` call (never chain a poweroff after it: ZV510C lost its unload error that way):

  ```sh
  L=/var/log/task-1123-zvwrfb
  GT=180 $L/g.sh 't=<tree>; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; sudo -n /var/tmp/zv/$t/scripts/zfs.sh -u; echo "rc=$?"; lsmod | grep -cE "^(zfs|spl) "'
  ```

  `zfs.sh -u` exits 0 even when `rmmod` fails (the status of its unload function is not its exit status, as ZV510E notes): an unload is judged by the module count after it and by the `rmmod` line, never by its rc. Clean: `refcnt=0`, nothing between it and `rc=0`, and `0`. Refused: `rmmod: ERROR: Module zfs is in use`, `rc=0` all the same, and `2`.
- **Pre-ZTS checks**, before every ZTS run (`<tree>` is the boot's tree):

  ```sh
  L=/var/log/task-1123-zvwrfb
  $L/g.sh 't=<tree>; lsmod | grep -E "^zfs "; echo "refcnt=$(cat /sys/module/zfs/refcnt)"; cat /sys/module/zfs/srcversion; sudo -n timeout 30 /var/tmp/zv/$t/zpool list -H -o name; sudo -n timeout 30 /var/tmp/zv/$t/zinject; ls /sys/block | grep -E "^zd"; find /dev/zvol -type l 2> /dev/null; ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_remove_open.* 2> /dev/null | wc -l; sudo -n losetup -a; test ! -s /etc/zfs/zpool.cache && echo CACHE-OK; pgrep -a -x zed; pgrep -a -x lt-zed; df -h / | tail -n 1'
  ```

  Expected: one zfs line (its last number is the use count), `refcnt=` the use count after the previous run of this boot (0 for the first run, and 0 after every run unless one leaked: see step 6), this boot's srcversion, no pool, `No handlers registered.`, no zd device, `0`, no loop device, CACHE-OK, no zed, and at least 10G available on `/`. A leftover `/dev/zvol` link (ZOT and ZV510C saw a broken `/dev/zvol/testpool/testvol` after a first ZTS run) is recorded with `ls -l`, and is not a stop. Any other leftover (a pool, a handler, a zd device, a file vdev, a work directory, a tunable file, a loop device, a cache file, zed): quote it, with `sudo -n strings -n 6 /etc/zfs/zpool.cache | grep -E "^(testpool|/)" | sort -u` for a cache file, run nothing more in this boot, COPY-OUT, GUEST-OFF, and go to the next step of the plan. A handler left behind means a `dd` may still be held at the point: say so. Your driver must stop at such a check: ZV510C's printed one and ran on.
- **Serial watch.** The pattern is `P='list_del|BUG|Oops|Poison|general protection|KASAN|use-after-free|VERIFY|ASSERT|PANIC|Kernel panic|blocked for more than'`. Before each run, note `N=$(sudo -n grep -cE "$P" $L/serial-<boot>.log)`. While it runs, every 30 s, check `sudo -n grep -cE "$P" $L/serial-<boot>.log` and `pgrep -f '^qemu-system-x86_64 -name zv510b'`. A count above N, or QEMU gone, is a finding: CAPTURE (step 7 has its own rule for its expected oops). ZTS's `-K` writes `ZTS run <test>` lines to the kernel log; they do not match P. The serial logs are root's: read them with `sudo -n`.
- **CAPTURE <tag>** (a finding, or a hang as defined above). Wait 60 s, then `sudo -n grep -nE "$P" $L/serial-<boot>.log | head -n 20 > $L/finding-<tag>.txt` (`hang-<tag>.txt` for a hang). If `GT=30 $L/g.sh true` answers, run the command below in the guest with `<tag>` filled in (it dumps the blocked tasks to the kernel log first, then saves dmesg, threads, stacks, the handlers and the ZFS debug log), then COPY-OUT. If the guest does not answer, wait until 150 s after the finding, so that the guest's hung-task reports reach the serial log, and go on. From then on, run no zfs, zpool, zinject or `zfs.sh` command in that boot, and stop it with GUEST-OFF from the TERM. Then quote the first finding line and the 40 lines after it, from the guest's `dmesg-<tag>.txt` (or from the serial log if the guest did not answer), and say which process it names, whether `zvol_open`, `__mutex_lock`, `__mutex_remove_waiter`, `zvol_os_remove_minor` or `zvol_remove_minors_impl` appear in its trace, how long after the run's start T0 it came, and whether it is M-41's or M-30's signature (see "Why").

  ```sh
  L=/var/log/task-1123-zvwrfb
  GT=120 $L/g.sh 'GL=/var/log/zv510b; t=<tag>; echo w | sudo -n tee /proc/sysrq-trigger > /dev/null; ps -eLo pid,tid,stat,comm,wchan:32 | grep -E " D|zvol|zfs|zpool|zinject|systemd-udevd|txg|dd|ksh" > $GL/ps-$t.txt; for i in $(ps -eLo tid=,stat= | awk "\$2 ~ /^D/ { print \$1 }"); do echo "== $i"; sudo -n cat /proc/$i/stack; done > $GL/stacks-$t.txt 2>&1; sudo -n timeout 10 /var/tmp/zv/wr/zinject > $GL/zinject-$t.txt 2>&1; sudo -n dmesg > $GL/dmesg-$t.txt; sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg > $GL/dbgmsg-$t.txt 2>&1; cat $GL/refcnt.txt'
  ```

  (`wr`'s `zinject` serves both boots: the user space of the two trees is the same.)
- **REF-CAPTURE <tag>** (an unload that is refused, or a use count that rose in a run). Do not power off first: run this in the guest, with `<tag>` and `<tree>` filled in, and quote its output:

  ```sh
  L=/var/log/task-1123-zvwrfb
  GT=120 $L/g.sh 'GL=/var/log/zv510b; t=<tag>; z=/var/tmp/zv/<tree>/zpool; { echo "refcnt=$(cat /sys/module/zfs/refcnt)"; echo "holders: $(ls /sys/module/zfs/holders)"; lsmod | grep -E "^(zfs|spl) "; echo "zd: $(ls /sys/block | grep -E "^zd" | tr "\n" " ")"; sudo -n losetup -a; sudo -n timeout 30 $z list 2>&1; grep -cE " (zfs|zvol) " /proc/self/mountinfo; sudo -n find /proc/[0-9]*/fd -maxdepth 1 \( -lname "/dev/zd*" -o -lname "/dev/zfs" \) -printf "%p -> %l\n" 2> /dev/null; ps -eo pid,stat,comm,wchan:32 | awk "\$2 ~ /^D/"; pgrep -a -x zed; sudo -n dmesg | tail -n 40; } > $GL/refs-$t.txt 2>&1; cat $GL/refs-$t.txt'
  ```

  A held module with no holder, no zd device, no pool, no mount, no open `/dev/zd*` or `/dev/zfs`, no D task and no zed is the restart leak of "Why". Anything else holding the module is a finding to quote: run nothing more in this boot, COPY-OUT, GUEST-OFF, and go to the next step of the plan.
- **COPY-OUT** (into this boot's `guest-<boot>`, e.g. `guest-b2`):

  ```sh
  L=/var/log/task-1123-zvwrfb; B=<boot>
  mkdir -p $L/guest-$B && GT=120 $L/g.sh 'tar -C /var/log -cf - zv510b' | tar -C $L/guest-$B -xf - && ls $L/guest-$B/zv510b
  GT=120 $L/g.sh 'sudo -n tar -C /var/tmp -cf - test_results' | tar -C $L/guest-$B -xf - && ls $L/guest-$B/test_results
  ```

  (In B1, before any ZTS run, only the first line.)
- **GUEST-OFF.** Run `$L/g.sh 'sudo -n systemctl poweroff'`, alone; a 255 status as the connection closes is normal. Poll `pgrep -f '^qemu-system-x86_64 -name zv510b'` every 10 s for 2 minutes. If QEMU is still there, run `sudo -n pkill -TERM -f '^qemu-system-x86_64 -name zv510b'`, wait 30 s more, then do the same with `-KILL`. After a finding or a hang, and always in B3, skip the poweroff and start at the TERM.
- `sudo -n fstrim -av` after deleting $D.

## STEP 0: state (read-only, about 5 minutes)

```sh
L=/var/log/task-1123-zvwrfb
hostname; uname -r; uptime -s; nproc; free -m; df -h / /var/tmp
lsmod | grep -E '^(zfs|spl) '; ls -l /dev/zfs 2>&1; sudo -n losetup -a; ls -d /var/tmp/task-1123-zvwrfb $L /var/tmp/task-1122-zv510d /var/tmp/task-1124-zv510e /var/tmp/task-1121-zv510c /var/log/task-1122-zv510d /var/log/task-1124-zv510e /var/log/task-1121-zv510c 2>&1
pgrep -a -x zed; pgrep -a -x lt-zed; pgrep -af qemu-system; git --version; python3 --version; curl --version | head -n 1
grep -m1 'model name' /proc/cpuinfo; echo "vmx=$(grep -cw vmx /proc/cpuinfo) svm=$(grep -cw svm /proc/cpuinfo)"
ls -l /dev/kvm 2>&1; lsmod | grep -E '^kvm'; cat /sys/module/kvm_intel/parameters/nested 2>&1; getent group kvm
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' qemu-system-x86 qemu-utils genisoimage 2>&1; command -v ssh ssh-keygen
dpkg-query -W -f='${binary:Package}\n' | grep -cE '^(pve-|proxmox-)'
ss -ltn | grep -c ':2251 '; env | grep -ciE '^(https?|ftp|no|all)_proxy='; apt-config dump | grep -ci proxy
sudo -n dmesg | grep -ciE 'kvm|vmx'
```

Expected:

- a 6.12 kernel (`6.12.118+deb13-cloud-amd64` on 106 and 107 since 23/11; record it: it must be the same at the end);
- at least 5500 MiB available and 20 GiB free on /var/tmp;
- no zfs module, no /dev/zfs, no loop device, $D and $L absent, no zed, no qemu process; git present;
- `/var/tmp/task-1122-zv510d`, `/var/tmp/task-1124-zv510e` and `/var/tmp/task-1121-zv510c` absent (those tasks restored, or never ran here); their `/var/log` directories present on the VM that ran them: record them, they are not touched;
- the CPU line of earlier reports, with `vmx` above 0 (ZV510C saw `vmx=16` on VM 106: each CPU has a `flags` and a `vmx flags` line) and `svm=0`;
- `/dev/kvm` present, with `kvm_intel` and `kvm` loaded; record `nested` and the kvm group;
- `qemu-system-x86` not installed; record the versions of `qemu-utils` and `genisoimage` (an absent one joins the install of step 2);
- 0 Proxmox packages; the port count 0; both proxy counts 0.

Stop and report (nothing has been changed yet) if:

- `vmx=0`: KVM cannot run here (this task has no TCG variant);
- a zfs module, a pool, zed or a qemu process exists;
- `/var/tmp/task-1122-zv510d`, `/var/tmp/task-1124-zv510e` or `/var/tmp/task-1121-zv510c` exists (that task is running here or did not restore);
- `qemu-system-x86` is installed (an earlier task did not restore);
- the port is taken;
- there is less memory or disk than above;
- a proxy is configured (the guest would not have it).

Then the records for step 9:

```sh
L=/var/log/task-1123-zvwrfb
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

## STEP 1: trees and scripts, on this VM (about 6 minutes)

One fetch per branch, depth 4 (`wr`'s HEAD~3 and `fb`'s HEAD~2 are `189a309d464`). Neither branch may have moved: TREES-OK checks it.

```sh
D=/var/tmp/task-1123-zvwrfb; L=/var/log/task-1123-zvwrfb
sudo -n mkdir -p $D && sudo -n chown "$(id -un):" $D && chmod 755 $D && mkdir $D/src $D/img $D/seed
for t in "wr zvol-open-pin-state-test-wr" "fb zvol-open-pin-state-test-wr-fb"; do set -- $t; mkdir $D/src/$1 && git -C $D/src/$1 init -q . && git -C $D/src/$1 fetch -q --depth 4 https://github.com/tigerblue77/zfs $2 && git -C $D/src/$1 checkout -q FETCH_HEAD || echo "FETCH-FAILED $1"; done
{ echo "wr $(git -C $D/src/wr rev-parse HEAD) $(git -C $D/src/wr rev-parse HEAD^{tree}) $(git -C $D/src/wr rev-parse HEAD~1) $(git -C $D/src/wr rev-parse HEAD~2) $(git -C $D/src/wr rev-parse HEAD~3)"; echo "fb $(git -C $D/src/fb rev-parse HEAD) $(git -C $D/src/fb rev-parse HEAD^{tree}) $(git -C $D/src/fb rev-parse HEAD~1) $(git -C $D/src/fb rev-parse HEAD~2)"; } > $L/tree.txt
printf '%s\n' "wr 3e42136b8d9448d56eca1155a0a78d6328196595 73f4f8b0178dae5f7624126ff0327468267dec79 684a8aefc5e341247b103a892ae6e054eb0ab473 5439f31735b1e8714b83c14f687098e51d048c8e 189a309d464ca51c74d627fdbdd84b5992eade2d" "fb 4072f6297815daf43ecbf058cea31725311ec509 b363dc7da2b9400eb82c894b289b71956d581fd6 46fa05849da56c9e8f2939011b3901cf4f845b33 189a309d464ca51c74d627fdbdd84b5992eade2d" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREES-OK
git -C $D/src/wr log --oneline --abbrev=11 -4; git -C $D/src/fb log --oneline --abbrev=11 -3; for t in wr fb; do git -C $D/src/$t status --porcelain | wc -l; done
cd $D/src && diff -rq -x .git wr fb
```

Expected:

- TREES-OK (the two lines of `$L/tree.expected`);
- `wr`'s log, top first: `3e42136b8d9 ZTS: remove a zvol's minor while an open has not locked it`, `684a8aefc5e zinject: let a point with no pool at hand act for any pool`, `5439f31735b Linux: wait for zvol_open() before freeing a removed zvol`, `189a309d464 zinject: build injection points only into debug builds`;
- `fb`'s log: `4072f629781 VM only: zvol_misc_remove_open without the zvol_open() wait fix`, `46fa05849da VM only: let a point with no pool at hand act for any pool`, `189a309d464 zinject: build injection points only into debug builds`;
- `0` and `0` (both clean);
- exactly two lines from `diff -rq`, in this order:

  ```
  Files wr/config/kernel-blkdev.m4 and fb/config/kernel-blkdev.m4 differ
  Files wr/module/os/linux/zfs/zvol_os.c and fb/module/os/linux/zfs/zvol_os.c differ
  ```

Then the files and the counts:

```sh
D=/var/tmp/task-1123-zvwrfb
cd $D/src && printf '%s\n' "f2f57fc2e9661e850f2af61455c29a4c024d2233b443d9b200e1de82c3c3625c  wr/module/os/linux/zfs/zvol_os.c" "de241dd5bc40dac835afa8d289c9f674ce3b03f0adc926a4bce459cf1c1d427f  fb/module/os/linux/zfs/zvol_os.c" "14db830c60c3a4f42142cb5e09ed9d5c51ae161b0f65fc605fb8d0e2b14d6a65  wr/config/kernel-blkdev.m4" "a2a35a52e887dacd6b1e2eb6aeca90267557cbb4e38795872f0ef347fc06fc6d  fb/config/kernel-blkdev.m4" "0a9b63bb56c944837e8587df58bfabaf6493636d2ca64a49a8220f4fd62d8ac6  wr/module/zfs/zio_inject.c" "0a9b63bb56c944837e8587df58bfabaf6493636d2ca64a49a8220f4fd62d8ac6  fb/module/zfs/zio_inject.c" "f0f80d2ff128a4bd794733b3c6c01033dd4cd9963ce9806188804d4e48f7b7e7  wr/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh" "f0f80d2ff128a4bd794733b3c6c01033dd4cd9963ce9806188804d4e48f7b7e7  fb/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh" "3ff8fc09db72e44fbeec3acbabff0285fd5c43c7c01985c068077115e2382b85  wr/tests/zfs-tests/include/libtest.shlib" "3ff8fc09db72e44fbeec3acbabff0285fd5c43c7c01985c068077115e2382b85  fb/tests/zfs-tests/include/libtest.shlib" | sha256sum -c && echo FILES-OK
cd $D/src && for t in wr fb; do z=$t/module/os/linux/zfs/zvol_os.c; echo "$t $(grep -c HAVE_BLKDEV_BD_MUTEX $z) $(grep -c HAVE_BLKDEV_GET_ERESTARTSYS $z) $(grep -c '^module_param(zvol_open_timeout_ms' $z) $(grep -c '"zvol_open:found"' $z) $(grep -c bdget_disk $z) $(grep -c 'spa != NULL &&$' $t/module/zfs/zio_inject.c) $(git -C $t ls-tree HEAD tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open.ksh | cut -c1-6)"; done
```

Expected:

- ten `OK` lines and FILES-OK;
- the counts `<tree> <BD_MUTEX> <GET_ERESTARTSYS> <module_param> <the point> <bdget_disk> <NULL-spa line> <test mode>`: `wr 2 4 1 1 2 1 100755` and `fb 0 4 1 1 0 1 100755`. Only `wr` has the fix (its two `HAVE_BLKDEV_BD_MUTEX` blocks around `bdget_disk()`); both have the point and the NULL-spa match.

If a fetch fails, wait 1 minute and try once more. Any other difference: go to step 9 and report as is (only $D and $L exist yet).

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF`, in this order, to $L/g.sh, $L/qemu.sh, $L/gbuild.sh and $L/gzts.sh. `g.sh` and `qemu.sh` run on this VM: they are ZV510E's with this task's $D and $L and its id in their first comment. The other two run in the guest and are ZV510C's, ZV510D's and ZV510E's byte for byte (same sums). Then:

```sh
L=/var/log/task-1123-zvwrfb
chmod 755 $L/g.sh
printf '%s\n' "b117f2090932f895a95f960160f1f2e02a00a0d2424993f0f216e1da11721d5d  $L/g.sh" "5e337afbf2fc782b4d0e653b479ab556f4a43cd71f0bdbf8fc025cdf5d2ca008  $L/qemu.sh" "7a62162affa98c148a368f8c2d1be2501668eb169176e87e0dcb6598370811a5  $L/gbuild.sh" "5deb9b8402f01d22070ec0a17f55bcceec2398fbde4b9fc1092155f9c15f2955  $L/gzts.sh" | sha256sum -c && for f in g.sh qemu.sh gbuild.sh gzts.sh; do sh -n $L/$f || echo "SYNTAX $f"; done && echo SCRIPTS-OK
```

Expected four `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

```sh
#!/bin/sh
# TASK-1123-ZVWRFB: run a command in the guest as debian: g.sh '<command>'
# Bounded by $GT seconds (default 120). Standard input goes to the guest.
D=/var/tmp/task-1123-zvwrfb
exec timeout "${GT:-120}" ssh -F none -i $D/key -p 2251 \
    -o BatchMode=yes -o IdentitiesOnly=yes -o ConnectTimeout=10 \
    -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=$D/known_hosts \
    -o LogLevel=ERROR debian@127.0.0.1 "$@"
```

```sh
#!/bin/sh
# TASK-1123-ZVWRFB guest launcher, run as root: qemu.sh <boot tag> <kvm|tcg>
D=/var/tmp/task-1123-zvwrfb
L=/var/log/task-1123-zvwrfb
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
# TASK-1121-ZV510B guest build, as debian: gbuild.sh <tree> <all|kernel> <jobs>
cd /var/tmp/zv/$1 || exit 1
./autogen.sh && ./configure --enable-debug --with-config=$2 && nice make -j$3
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

## STEP 2: QEMU on this VM (about 5 minutes)

As ZV510E's step 2, with this task's $L. Use as `<missing>` the packages among `qemu-utils` and `genisoimage` that step 0 found absent (usually none). Do not name an installed one: apt would upgrade it.

```sh
L=/var/log/task-1123-zvwrfb
apt-cache policy qemu-system-x86 | head -n 4
apt-get -s --no-install-recommends install qemu-system-x86 <missing> > $L/apt-sim.txt 2>&1; echo "rc=$?"
grep -E '^Inst' $L/apt-sim.txt; grep -E '^Remv|^Inst [^ ]+ \[' $L/apt-sim.txt; grep -E 'upgraded,' $L/apt-sim.txt
```

Expected `0 upgraded, <n> newly installed, 0 to remove`, with n about 20 (at most 40; ZV510C: 20 on VM 106); quote every `Inst` line; no `Remv` line and no `Inst <pkg> [<old version>]` line; every package from trixie (main, -updates or -security). A `Remv`, a replaced version, or more than 40 packages: STOP-RESTORE. Otherwise install:

```sh
L=/var/log/task-1123-zvwrfb
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
L=/var/log/task-1123-zvwrfb
test -e /dev/kvm || { sudo -n modprobe kvm_intel && touch $L/kvm-loaded-by-task; }; ls -l /dev/kvm; lsmod | grep -E '^kvm'
```

Expected `/dev/kvm` with `kvm_intel` and `kvm` loaded. A modprobe error: STOP-RESTORE.

## STEP 3: image, disk, key and seed (about 5 minutes)

ZV510C's step 3, with this task's $D and $L:

```sh
D=/var/tmp/task-1123-zvwrfb; L=/var/log/task-1123-zvwrfb; B=https://cdimage.debian.org/images/cloud/bullseye/20260821-2577; I=debian-11-generic-amd64-20260821-2577.qcow2
curl -fsSL --retry 3 --max-time 120 -o $D/img/SHA512SUMS $B/SHA512SUMS; echo "rc=$?"
curl -fsSL --retry 3 --max-time 1800 -o $D/img/$I $B/$I; echo "rc=$?"; ls -l $D/img
printf '%s\n' "9f8d0417cdc9caa7509c50e71af97750e6e71a6990142b13d16f692f87c3562a  $D/img/SHA512SUMS" | sha256sum -c
printf '%s\n' "9f2b86f7f3f58c3ad418dcbe4aadd4a1f37159100f31a94203d22416f8a57f715e974248914b45fd6c636e1e7afe0f8c853aac3ec1dd85e27f401ed5b99b9c19  $I" > $L/image.sha512
grep -Fx -f $L/image.sha512 $D/img/SHA512SUMS > /dev/null && echo SUMS-LINE-OK; cd $D/img && sha512sum -c $L/image.sha512 && echo IMAGE-OK
```

Expected: both rc 0, the image at 365428736 bytes, `SHA512SUMS: OK`, SUMS-LINE-OK, `debian-11-generic-amd64-20260821-2577.qcow2: OK` and IMAGE-OK. A failed download or check: delete the image and download it once more. A second failure, or SHA512SUMS not OK: STOP-RESTORE.

```sh
D=/var/tmp/task-1123-zvwrfb; I=/var/tmp/task-1123-zvwrfb/img/debian-11-generic-amd64-20260821-2577.qcow2
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
L=/var/log/task-1123-zvwrfb
sudo -n sh $L/qemu.sh b1 kvm > $L/qemu-b1.log 2>&1; echo "rc=$?"; cat $L/qemu-b1.log; pgrep -af '^qemu-system-x86_64 -name zv510b'
```

Expected rc 0, an empty log and one QEMU process. Any error (`Could not access KVM kernel module`, `failed to initialize kvm`, ...): STOP-RESTORE. Then poll every 10 s, for at most 8 minutes, `GT=20 $L/g.sh true && echo GUEST-UP` and `sudo -n grep -c 'Linux version 5.10.0-46-amd64' $L/serial-b1.log`. No GUEST-UP in time: quote `sudo -n tail -n 80 $L/serial-b1.log`, GUEST-OFF, STOP-RESTORE.

```sh
L=/var/log/task-1123-zvwrfb
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
L=/var/log/task-1123-zvwrfb
printf '%s\n' 'deb http://archive.debian.org/debian bullseye main' 'deb http://archive.debian.org/debian bullseye-updates main' 'deb http://archive.debian.org/debian-security bullseye-security main' | $L/g.sh 'sudo -n tee /etc/apt/sources.list > /dev/null; sudo -n rm -f /etc/apt/sources.list.d/*.list; cat /etc/apt/sources.list'
GT=600 $L/g.sh 'sudo -n apt-get -o DPkg::Lock::Timeout=300 update > /var/log/zv510b/apt-update.log 2>&1; echo "rc=$?"; grep -E "^(W|E|Err):" /var/log/zv510b/apt-update.log'
```

Expected the three lines, rc 0, and no `Err:` or `E:`. A signature error or a failure: GUEST-OFF, STOP-RESTORE.

```sh
L=/var/log/task-1123-zvwrfb
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
L=/var/log/task-1123-zvwrfb
GT=1200 $L/g.sh 'sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y --no-install-recommends -o DPkg::Lock::Timeout=300 install ksh fio bc jq acl attr xxhash pamtester rsync parted lsscsi python3 > /var/log/zv510b/apt-install-zts.log 2>&1; echo "rc=$?"; grep -E "upgraded,|^(W|E):" /var/log/zv510b/apt-install-zts.log'
$L/g.sh 'dpkg-query -W -f="\${Package} \${Version}\n" ksh fio xxhash jq python3; command -v ksh fio xxh128sum bc jq pamtester rsync parted lsscsi getfacl getfattr python3; readlink -f /bin/ksh; python3 --version'
$L/g.sh 'sudo -n useradd -m -s /bin/bash zts && echo "zts ALL=(ALL) NOPASSWD: ALL" | sudo -n tee /etc/sudoers.d/zts > /dev/null && sudo -n chmod 440 /etc/sudoers.d/zts && sudo -n visudo -c > /dev/null && echo SUDOERS-OK; id zts; sudo -n -u zts sudo -n id -un'
```

Expected rc 0 (about 34 packages, none upgraded or removed); the versions of ZV510C (`ksh 2020.0.0+really93u+20120801-9`, `fio 3.25-2`, `xxhash 0.8.0-2`, `jq 1.6-2.1+deb11u...`, `python3 3.9.2-3`); a path for each of the twelve commands; `/bin/ksh93` (or `/usr/bin/ksh93`); `Python 3.9.2`; SUDOERS-OK, the `zts` id line and `root`. A failure: GUEST-OFF, STOP-RESTORE.

Trees and scripts into the guest:

```sh
D=/var/tmp/task-1123-zvwrfb; L=/var/log/task-1123-zvwrfb
tar -C $D/src -cf - wr fb | GT=900 $L/g.sh 'tar -C /var/tmp/zv -xf - && echo COPIED'
$L/g.sh 'cd /var/tmp/zv; echo "wr $(git -C wr rev-parse HEAD) $(git -C wr rev-parse "HEAD^{tree}") $(git -C wr rev-parse HEAD~1) $(git -C wr rev-parse HEAD~2) $(git -C wr rev-parse HEAD~3)"; echo "fb $(git -C fb rev-parse HEAD) $(git -C fb rev-parse "HEAD^{tree}") $(git -C fb rev-parse HEAD~1) $(git -C fb rev-parse HEAD~2)"' | diff $L/tree.expected - && echo GUEST-TREES-OK
for f in gbuild.sh gzts.sh; do $L/g.sh "cat > /var/log/zv510b/$f" < $L/$f || echo "COPY-FAILED $f"; done
$L/g.sh 'cd /var/log/zv510b && sha256sum gbuild.sh gzts.sh'
```

Expected COPIED, GUEST-TREES-OK, and the two sums of step 1. Then the guest's boot command line, for B2 and B3:

```sh
L=/var/log/task-1123-zvwrfb
printf '%s\n' 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT slub_debug=P"' | $L/g.sh 'sudo -n tee /etc/default/grub.d/99-zv510b.cfg'
GT=300 $L/g.sh 'sudo -n update-grub > /var/log/zv510b/update-grub.log 2>&1; echo "rc=$?"; grep -m1 -E "^[[:space:]]*linux[[:space:]]" /boot/grub/grub.cfg'
```

Expected the drop-in line echoed (with `$GRUB_CMDLINE_LINUX_DEFAULT` literal), rc 0, and a `linux /boot/vmlinuz-5.10.0-46-amd64 root=PARTUUID=... ro console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200 consoleblank=0  slub_debug=P` line (one or two spaces before `slub_debug`). Anything else: GUEST-OFF, STOP-RESTORE. The drop-in stays: every later boot gets `slub_debug=P`.

## STEP 5: builds in the guest, in B1 (about 25 minutes)

One wave: both trees in full with `--enable-debug` (`gbuild.sh <tree> all`; on this framework a debug build has the injection points, which the test needs: without them it reports UNSUPPORTED), 3 jobs each on the guest's 6 vCPUs and 4 GiB, as ZV510E:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'setsid nohup sh /var/log/zv510b/gbuild.sh wr all 3 > /var/log/zv510b/build-wr.log 2>&1 < /dev/null & setsid nohup sh /var/log/zv510b/gbuild.sh fb all 3 > /var/log/zv510b/build-fb.log 2>&1 < /dev/null & echo STARTED'
```

Poll every 2 to 5 minutes with `$L/g.sh 'grep -H build-rc= /var/log/zv510b/build-*.log; uptime'`, and once, halfway, `free -m | head -n 2` on this VM. Each build must end `build-rc=0`. Then:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'cd /var/tmp/zv; for t in wr fb; do grep -E "^checking kernel (source|build) directory|^checking kernel source version|bdget_disk\(\) and bd_mutex|blkdev_get_by_path\(\) handles ERESTARTSYS|injection points will be built" /var/log/zv510b/build-$t.log; done; for t in wr fb; do echo "$t $(grep -c "define HAVE_BLKDEV_BD_MUTEX 1" $t/zfs_config.h) $(grep -c "define HAVE_BLKDEV_GET_ERESTARTSYS 1" $t/zfs_config.h) $(/sbin/modinfo -F parm $t/module/zfs.ko | grep -c "^zvol_open_timeout_ms:") $(nm $t/module/zfs.ko | grep -cE " U bdget_disk\$") $(nm $t/module/zfs.ko | grep -cE " T zio_handle_func_injection\$") $(grep -c -a "zvol_open:found" $t/module/zfs.ko)"; done; grep "warning:" /var/log/zv510b/build-*.log | grep -v "objtool: can.t decode instruction"; /sbin/modinfo -F vermagic wr/module/zfs.ko fb/module/zfs.ko; for t in wr fb; do readelf -p .comment $t/module/zfs.ko | grep -m1 GCC; done; /sbin/modinfo -F srcversion wr/module/zfs.ko fb/module/zfs.ko; ls -d wr/zpool wr/zfs wr/zinject wr/udev/zvol_id wr/tests/test-runner/bin/test-runner.py wr/tests/zfs-tests/bin/ksh fb/zpool fb/zfs fb/zinject fb/udev/zvol_id fb/tests/test-runner/bin/test-runner.py fb/tests/zfs-tests/bin/ksh; ls wr/tests/zfs-tests/bin | wc -l; ls fb/tests/zfs-tests/bin | wc -l; df -h / | tail -n 1; du -sh wr fb'
```

Expected:

- for each tree, in log order: `checking kernel source directory... /usr/src/linux-headers-5.10.0-46-common`, `checking kernel build directory... /usr/src/linux-headers-5.10.0-46-amd64`, `checking kernel source version... 5.10.0-46-amd64` and `checking whether blkdev_get_by_path() handles ERESTARTSYS... yes`; for `wr` then `checking whether bdget_disk() and bd_mutex are available... yes`, for `fb` no such line (`fb` has master's `kernel-blkdev.m4`); then, for both, `checking whether zinject injection points will be built... yes`;
- the counts `<tree> <BD_MUTEX> <GET_ERESTARTSYS> <zvol_open_timeout_ms parameter> <bdget_disk import> <zio_handle_func_injection> <the point in zfs.ko>`: `wr 1 1 0 1 1 1` and `fb 0 1 0 0 1 1` (the last number counts the lines of `zfs.ko` that hold the point's name: any number above 0 is right). Both define `HAVE_BLKDEV_GET_ERESTARTSYS` (5.10 restarts opens), neither has the `zvol_open_timeout_ms` parameter, both modules carry the injection framework and the point. The fix is what tells the two modules apart: only `wr` imports `bdget_disk`. Any other count: STOP-RESTORE;
- no `warning:` line beyond objtool's `can't decode instruction` notes in `module/icp/asm-x86_64` objects (filtered out: 5.10's objtool predates those instructions); quote any other;
- vermagic `5.10.0-46-amd64 SMP mod_unload modversions` twice, and `GCC: (Debian 10.2.1-6) 10.2.1 20210110` twice;
- note SW (`wr`'s srcversion) and SB (`fb`'s): they must differ (`zvol_os.c` differs);
- the twelve paths, without error; more than 200 links in each `tests/zfs-tests/bin` (232 on the VMs; a few fewer here, the tools left out of the guest);
- at least 10G available on `/` (about 19G expected); note the two sizes.

A failed configure or build: quote the last 40 lines of its log and the failing check of its `config.log`, then GUEST-OFF, STOP-RESTORE. `ERESTARTSYS ... no`, `injection points will be built... no`, or a compiler other than gcc 10.2.1 in a module: STOP-RESTORE. `ksh` missing from a `tests/zfs-tests/bin`: STOP-RESTORE (user `zts` could not run that tree's ZTS).

COPY-OUT with `B=b1` (its first line only), then GUEST-OFF (with the poweroff).

## STEP 6: B2, the branch `wr` on 5.10 (about 15 minutes)

```sh
L=/var/log/task-1123-zvwrfb
sudo -n sh $L/qemu.sh b2 kvm > $L/qemu-b2.log 2>&1; echo "rc=$?"; cat $L/qemu-b2.log
```

Poll for GUEST-UP as in step 4 (at most 5 minutes), then:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'uname -r; cat /proc/cmdline; cat /sys/kernel/slab/kmalloc-1k/poison /sys/kernel/slab/kmalloc-2k/poison; sudo -n sysctl kernel.panic_on_oops kernel.panic kernel.softlockup_panic kernel.hung_task_panic kernel.hung_task_timeout_secs; systemctl is-system-running; systemctl --failed --no-legend; lsmod | grep -cE "^(zfs|spl) "'
```

Expected:

- `5.10.0-46-amd64`, with a command line ending ` slub_debug=P`;
- poison `1` and `1`;
- `panic_on_oops = 0`, `panic = 0`, `softlockup_panic = 0`, `hung_task_panic = 0` and `hung_task_timeout_secs = 120`: an oops leaves the guest up for the capture, and a hung task is reported after 120 s;
- `running` (or `degraded`: record the failed units), and `0` modules.

`panic_on_oops = 1`: set it to 0 with `sudo -n sysctl -w` and say so. Without `slub_debug=P`, or poison `0`: GUEST-OFF, STOP-RESTORE.

Then the zvol udev rules from `wr` (they stay in the guest for B3):

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'id zts; sudo -n -u zts sudo -n id -un; pgrep -a -x zed; pgrep -a -x lt-zed; sudo -n /var/tmp/zv/wr/scripts/zfs-helpers.sh -iv > /var/log/zv510b/helpers.log 2>&1; echo "rc=$?"; cat /var/log/zv510b/helpers.log; sudo -n udevadm control --reload && echo UDEV-RELOADED'
```

Expected the `zts` id line, `root`, no zed; rc 0; the configuration block with `udevdir:` and `udevruledir:`; an `ln -s /var/tmp/zv/wr/...` line for each of `zvol_id`, `60-zvol.rules`, `69-vdev.rules` and `90-zfs.rules`, and for the other helpers; UDEV-RELOADED. A `Source missing` line for another helper is recorded. One for these four, or a `File exists` line for one of them, means no `/dev/zvol` links: COPY-OUT with `B=b2`, GUEST-OFF, and go to step 8 without step 7.

LOAD wr (expected values in the VM rules). Note N0 = `$L/g.sh 'sudo -n dmesg | wc -l'`.

Then three runs of the test, `w1`, `w2` and `w3`, one after the other, all in this boot and with this one module load (as ZV510C's z1 to z3). For each: the pre-ZTS checks with `t=wr`, the serial count N for `serial-b2.log`, then the run:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'cut -d" " -f1 /proc/uptime; setsid nohup sh /var/log/zv510b/gzts.sh wr zvol_misc_remove_open > /var/log/zv510b/zts-w1.log 2>&1 < /dev/null & echo STARTED'
```

(`zts-w2.log` and `zts-w3.log` for the next two.) The first number printed is the run's start T0, in seconds since the guest's boot: the clock of its dmesg and serial timestamps. Note it. Poll every 30 s with `$L/g.sh 'grep -E "] Test: |zts-rc=" /var/log/zv510b/zts-w1.log'` until `zts-rc=` appears (a run takes one to two minutes with ZTS's set-up and clean-up), with the serial watch. Then the result of the run (`z` is the tree):

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 't=w1; z=wr; x=zvol_misc_remove_open; f=/var/log/zv510b/zts-$t.log; grep -E "] Test: |zts-rc=|Missing util|Permission denied" $f; d=$(sed -n "s/^Log directory:[[:space:]]*//p" $f); echo "dir=$d"; o=$(find "$d" -path "*/$x*/stdout"); grep -nE "^(NOTE|ERROR|ASSERTION|SUCCESS: (test|dd|zinject))|zvol_open:found|was removed while|waits for an open that found it" "$o"; sudo -n timeout 30 /var/tmp/zv/$z/zinject; sudo -n dmesg | tail -n +<N+1> | grep -E "VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|list_del|blocked for more than"; sudo -n dmesg | wc -l; sudo -n udevadm settle -t 30; echo "$t refcnt-after=$(cat /sys/module/zfs/refcnt)" | tee -a /var/log/zv510b/refcnt.txt'
```

with `<N>` the dmesg count after the previous run (N0 for `w1`; the second-last number printed is the next run's N).

Expected for each of `w1` to `w3`:

- `[<date and time>] Test: /var/tmp/zv/wr/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open (run as root) [00:1x] [PASS]` (or `[00:2x]`; the runner prefixes each result line with a timestamp, and the group's `setup` and `cleanup` lines come with it, both PASS), and `zts-rc=0`;
- from the test's `stdout`, in this order: `ASSERTION: Removing the minor of a volume waits for an open that found it`; `SUCCESS: zinject -c <n>` (`verify_inject_points` registered a probe handler and removed it); `SUCCESS: test -b /dev/zdN` and `SUCCESS: test -d /sys/block/zdN`; the `NOTE:` line that starts the `zinject` table, with wrouesnel's header `ID  POOL  POINT  ACTION  MATCH  INJECT  FROM  AT` (columns padded), a dashed line, then its one row: `<id>  testpool  zvol_open:found  pause  1  1  -  -` (INJECT is now the sixth column, not the last; a count above 1 is recorded, not a failure: the open may have retried in `zvol_open()` and passed the point again); `SUCCESS: test none = none`; `NOTE: zfs set volmode=none waits for the held open`; `SUCCESS: zinject -c <id>`; `NOTE: open: rc 1, dd: failed to open '/dev/zdN': No such device or address` (the open fails with ENXIO); `SUCCESS: test 0 = 0`, `SUCCESS: test 1 = 0 exited 1` and `SUCCESS: test -e /sys/block/zdN exited 1` (two `log_mustnot`), and the last `dd`'s `SUCCESS:` line; the pass line `Removing the minor of a volume waits for an open that found it` (`log_pass` prints it as is);
- `No handlers registered.` (the test cleared its handler);
- from dmesg, no line;
- `w<k> refcnt-after=0`;
- in the runner's own output, ZTS's `Missing util(s):` list and a few `ln: ... Permission denied` lines for `fsck`, `newfs`, `compress` and `uncompress` (user `zts` cannot replace links that `make` made as `debian`), as in ZV510C: expected and harmless; record them.

A FAIL (or KILLED): quote the last 40 lines of its runner log and of the test's `stdout`, then go on to the next run (after its pre-ZTS checks); two FAILs in a row: no further run in this boot. A FAIL that printed `The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned` is the contradiction of "Why": say so. A `UNSUPPORTED` result means the module has no injection points (`verify_inject_points`): quote the test's `stdout`, no further run in this boot, and say so.

A `refcnt-after` above that run's `refcnt` before (with a PASS or not): REF-CAPTURE <tag> (with `<tree>` `wr`). If it shows only the restart leak (see the VM rules), go on with the next run: the next pre-ZTS check then expects this new count. Otherwise follow REF-CAPTURE's rule. A finding in the serial watch or in dmesg, or a hang: CAPTURE <tag>, quote it as the VM rules say, then step 7 (CAPTURE has copied the logs out and stopped the guest). That is a finding against the branch, unless it is M-30's signature.

End of this step (after `w3`, or wherever a rule above sent you here):

- the dmesg check since N0: `$L/g.sh 'sudo -n dmesg | tail -n +<N0+1> | grep -B10 -E "list_del|VERIFY|ASSERT|PANIC|BUG|Oops|Poison|general protection|blocked for more than"'` (none expected);
- UNLOAD wr if the module is loaded (a clean unload expected; refused: REF-CAPTURE w-unload, which is the expected outcome only if a run's `refcnt-after` rose);
- `$L/g.sh 'sudo -n dmesg > /var/log/zv510b/dmesg-wr.txt; cat /var/log/zv510b/refcnt.txt'`;
- COPY-OUT with `B=b2`, then GUEST-OFF.

Step 7 runs unless every run of this step failed before printing its `zinject` table (an environment fault, such as no `/dev/zvol` link or a point that holds nothing, which B3 would only repeat): then go to step 8 and say so.

## STEP 7: B3, the fail-before `fb` on 5.10 (about 15 minutes)

A fresh boot, whatever happened in B2:

```sh
L=/var/log/task-1123-zvwrfb
sudo -n sh $L/qemu.sh b3 kvm > $L/qemu-b3.log 2>&1; echo "rc=$?"; cat $L/qemu-b3.log
```

GUEST-UP (at most 5 minutes) and the checks of step 6, with the same expected values: `slub_debug=P` is required here, without which the use-after-free is most likely silent. Then, before the module loads, ZTS's own leftovers go (after a clean B2 there are none; a B2 that ended in a capture leaves its file vdevs, work directory, tunable files and cache file, and `zfs-tests.sh` refuses to start over an existing file vdev). Nothing is imported at this point:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'pgrep -a -x zed; ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_remove_open.* /etc/zfs/zpool.cache 2> /dev/null; sudo -n rm -rf /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/zvol_misc_remove_open.*; if test -e /etc/zfs/zpool.cache; then sudo -n mv /etc/zfs/zpool.cache /var/log/zv510b/zpool.cache.b2; fi; echo CLEANED; ls -l /lib/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules 2> /dev/null'
```

Expected no zed, no path listed before CLEANED (any listed path is quoted: it is what B2 left), then the rules file of step 6 (a link to `/var/tmp/zv/wr/...`). Then LOAD fb (expected values in the VM rules: SB, and no `zvol_open_timeout_ms`), and the kernel log follower:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'sudo -n setsid nohup stdbuf -oL dmesg --follow > /var/log/zv510b/oops-follow.txt 2>&1 < /dev/null & sleep 2; wc -l < /var/log/zv510b/oops-follow.txt; sudo -n dmesg | wc -l'
```

Expected two line counts of the same order (`dmesg --follow` is running, line-buffered; the second count is N3).

The pre-ZTS checks with `t=fb`, the serial count N for `serial-b3.log`, then the run, once:

```sh
L=/var/log/task-1123-zvwrfb
$L/g.sh 'cut -d" " -f1 /proc/uptime; setsid nohup sh /var/log/zv510b/gzts.sh fb zvol_misc_remove_open > /var/log/zv510b/zts-fb.log 2>&1 < /dev/null & echo STARTED'
```

Note T0. Poll every 30 s with `GT=30 $L/g.sh 'grep -E "] Test: |zts-rc=" /var/log/zv510b/zts-fb.log'`, the serial count and QEMU. Here the serial count rising above N is the expected result: note the time of the first new line, do not CAPTURE yet, and keep polling until `zts-rc=` appears or 17 minutes have passed since T0 (then it is a hang). QEMU gone: CAPTURE fb at once (from the serial log only).

Then the capture of this step (in place of CAPTURE, with its rules):

1. wait 60 s, then `sudo -n grep -nE "$P" $L/serial-b3.log | head -n 20 > $L/finding-fb.txt` (`hang-fb.txt` for a hang);
2. CAPTURE's guest command with `t=fb`;
3. the first of the two zfs commands allowed after the oops (both bounded by `timeout`): `GT=60 $L/g.sh 'sudo -n timeout 30 /var/tmp/zv/fb/zpool list; echo "rc=$?"; echo "refcnt=$(cat /sys/module/zfs/refcnt)"'`;
4. the second: the result command of step 6 with `t=fb`, `z=fb` and `<N>` = N3;
5. COPY-OUT with `B=b3`;
6. GUEST-OFF from the TERM (pre-approved here: the kernel is tainted, and the dead open keeps the removed disk's `bd_mutex`; no poweroff).

If the guest does not answer at 2, wait until 150 s after the first new serial line, skip 2 to 5, and quote from `serial-b3.log` only.

Expected:

- the runner returns: `[<date and time>] Test: /var/tmp/zv/fb/tests/zfs-tests/tests/functional/zvol/zvol_misc/zvol_misc_remove_open (run as root) [00:xx] [FAIL]` and a `zts-rc=` other than 0;
- the test fails in its step 3, within a few seconds of the hold: after the `zinject` table (as on `wr`, INJECT 1) and `SUCCESS: test none = none`, `NOTE: zfs set: rc 0, ` and on the next line `The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned` (`log_fail` prints it as is, with no prefix). `NOTE: zfs set volmode=none waits for the held open` and `NOTE: open: rc` are not printed (the test failed before them);
- then ZTS's test-fail callbacks (`zfs_dbgmsg`, `zfs_dmesg`), and `NOTE: Performing local cleanup via log_onexit (cleanup)`: its `zinject -c all` releases the `dd` open, which locks the freed, poisoned `zvol_state_t`;
- on the serial log, right after the release: `general protection fault, probably for non-canonical address 0x6b6b6b6b6b6b6b..`, with `RIP:` in `__mutex_lock` or a helper of it (`mutex_spin_on_owner`, `mutex_can_spin_on_owner`), `zvol_open` in the Call Trace below it (then `__blkdev_get`, `blkdev_get`, `blkdev_open`, `do_dentry_open`), and `Comm: dd`. A `list_del corruption` / `kernel BUG at lib/list_debug.c`, a SLUB `Poison overwritten` or any other oops in `zvol_open()` confirms it as well. No oops at all is reported as such: the test's FAIL stays the verdict;
- the clean-up goes on: `dd` died in the oops, so its subshell ends and the clean-up prints no stacks (if it does, after 60 s, quote the stacks of `dd` and `zfs` it prints); the dead open keeps only the removed disk's `bd_mutex` and its references, which nothing else needs, so the pool's clean-up is expected to complete and the runner to return. If it hangs instead, it is a hang: capture it as above;
- `zpool list`: `no pools available` (rc 0) once the clean-up has completed; `refcnt=` 1 or so (the dead open's reference: the module cannot be unloaded, and is not tried);
- from the result command: `No handlers registered.` (the clean-up cleared the handler; a handler still listed means the release did not happen: say so), the dmesg lines of the oops, and a `refcnt-after` equal to the `refcnt` of item 3.

Quote: the `Test:` line and `zts-rc=`, or that the runner did not return; the test's `zfs set: rc` NOTE and the failure line, and the clean-up lines after it; the oops (the GPF or BUG line, `RIP:`, `Comm:`, and the first 15 frames of its Call Trace) from `serial-b3.log`, with its time minus T0; any D task from `ps-fb.txt` with its stack from `stacks-fb.txt`; `zinject-fb.txt`; the `zinject: handler <id> paused at zvol_open:found` and `... resumed at zvol_open:found` lines of `dbgmsg-fb.txt`, if they are among its last 60 lines; the `zpool list` output and the `refcnt`; the line count of `oops-follow.txt`. A PASS here (the removal waited without the fix) is the contradiction of "Why": report it with the test's `stdout`.

B3 is the guest's last boot: nothing runs on its tainted kernel after this step.

## STEP 8: evidence on this VM (about 3 minutes)

```sh
D=/var/tmp/task-1123-zvwrfb; L=/var/log/task-1123-zvwrfb; P='list_del|BUG|Oops|Poison|general protection|KASAN|use-after-free|VERIFY|ASSERT|PANIC|Kernel panic|blocked for more than'
pgrep -af qemu-system; uptime -s | diff $L/uptime.step0 - && echo NO-VM-REBOOT
sudo -n dmesg | grep -ciE 'kvm|vmx'; sudo -n dmesg | grep -iE 'kvm|vmx' | tail -n 10
for b in b1 b2 b3; do echo "$b $(sudo -n grep -m1 -o 'Linux version [^ ]*' $L/serial-$b.log) | $(sudo -n grep -m1 -o 'Command line: .*' $L/serial-$b.log) | findings=$(sudo -n grep -cE "$P" $L/serial-$b.log)"; done
sort -u $L/guest-b*/zv510b/refcnt.txt; ls $L/guest-b*/zv510b/refs-*.txt 2> /dev/null
for t in w1 w2 w3 fb; do f=$(ls $L/guest-b*/zv510b/zts-$t.log 2> /dev/null | tail -n 1); if [ -n "$f" ]; then echo "$t $(grep -E '] Test: .*/zvol_misc_remove_open ' $f) $(grep -o 'zts-rc=[0-9]*' $f)"; else echo "$t no log"; fi; done
du -h $D/guest.qcow2; df -h /var/tmp
```

Expected: no QEMU process and NO-VM-REBOOT; the kvm lines as at step 0 (quote any new error); the three boots on `5.10.0-46-amd64`, B1 without `slub_debug=P` and B2 and B3 with it; findings 0 in B1 and B2 (except where a capture was made) and above 0 in B3 (its expected oops); `refcnt-after=0` for w1 to w3, and the dead open's count for fb; the four test lines (w PASS, fb FAIL); the overlay at about 5 GiB.

## STEP 9: restore (every STOP-RESTORE ends here)

As ZV510E's step 9, with this task's $D and $L. Make sure the guest is stopped first: if `pgrep -f '^qemu-system-x86_64 -name zv510b'` prints anything, do GUEST-OFF from the TERM.

```sh
D=/var/tmp/task-1123-zvwrfb; L=/var/log/task-1123-zvwrfb
test -s $L/added-packages.txt && sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::Lock::Timeout=300 purge $(cat $L/added-packages.txt) > $L/apt-purge.log 2>&1; echo "rc=$?"
grep -E '^(W|E):|[Ww]arning|[Ee]rror' $L/apt-purge.log
test -e $L/kvm-loaded-by-task && sudo -n modprobe -r kvm_intel kvm; lsmod | awk 'NR > 1 { print $1 }' | sort | diff $L/modules.step0 -
sudo -n rm -rf $D; sudo -n fstrim -av; df -h / /var/tmp
```

```sh
L=/var/log/task-1123-zvwrfb
dpkg-query -W -f='${binary:Package} ${Version} ${db:Status-Abbrev}\n' | sort > $L/pkgs.end; diff $L/pkgs.step0 $L/pkgs.end; sudo -n dpkg --audit
getent group | cut -d: -f1 | sort | diff $L/groups.step0 -; getent passwd | cut -d: -f1 | sort | diff $L/users.step0 -
sudo -n sha256sum /boot/grub/grub.cfg /etc/default/grub | diff $L/boot.step0 -; diff $L/cmdline.step0 /proc/cmdline
ls -d /var/tmp/task-1123-zvwrfb /usr/share/qemu /usr/share/seabios 2>&1; ls -l /etc/zfs/zpool.cache 2>&1
```

Expected: purge rc 0 (or `rc=1` from `test` if step 2 installed nothing); the module list as at step 0 (a module that QEMU or KVM loaded on demand may appear: list it, and leave it unless this task loaded it); $D gone, and the space back near step 0's figures; no package, group, user, boot-file or command-line difference (a version changed by unattended-upgrades during the task is the only acceptable package line: say so; a new group or user left by a QEMU package is reported and left for main); every path `No such file or directory` (`/etc/qemu` belongs to `qemu-guest-agent` on VM 106 and stays); the zpool.cache as at step 0.

Up to date: `sudo -n apt-get -o DPkg::Lock::Timeout=300 update > $L/apt-update-2.log 2>&1; echo "rc=$?"`, then `apt list --upgradable 2>/dev/null | tail -n +2`, compared with $L/upgradable.step0. If anything new is listed: `sudo -n apt-get -s upgrade | grep -E '^(Inst|Remv)|upgraded,'`; with no `Remv`, apply it with `sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::Lock::Timeout=300 upgrade > $L/apt-upgrade.log 2>&1` and quote its summary line; a new kernel ABI kept back is listed and left to main (it would change the boot kernel of later tasks). Then `sudo -n apt-get autoclean > /dev/null`.

Working: `uname -r` and `uptime -s` as at step 0, `systemctl is-system-running` and `systemctl --failed --no-legend` as at step 0, `lsmod | grep -E '^(zfs|spl) '` empty, and `sysctl kernel.panic_on_oops kernel.panic kernel.softlockup_panic kernel.hung_task_panic | diff $L/sysctl.step0 -`. Logs, scripts, serial logs and the guest logs stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. **This VM at step 0:** kernel, CPU with vmx and nproc, `/dev/kvm` and the kvm modules, memory, disk, `qemu-utils` and `genisoimage` before the task, the sysctls, zpool.cache, the directories of ZV510C, ZV510D and ZV510E. **Packages added:** every one with version and suite, and the QEMU version. **Setup:** TREES-OK (both lines); the two logs; the `diff -rq` lines; FILES-OK; the counts; SCRIPTS-OK; the image checks; the overlay; the seed.
2. **Guest B1:** launch rc, time to GUEST-UP, cloud-init, kernel, command line, virt, CPU, `printk`; the apt sources, update rc, packages installed and upgraded (ZV510C's list and ZTS's); the ZTS versions, SUDOERS-OK, `zts`'s id; gcc, the header packages, the kernel config lines; COPIED, GUEST-TREES-OK, the two sums; the update-grub line. **Builds:** for each tree, rc and duration, the configure lines, the counts line, warnings, vermagic, the GCC comments, SW and SB, the link counts, free space, tree sizes.
3. **Boots B2 and B3:** launch rc, time to GUEST-UP, command line, poison, sysctls, failed units; each LOAD (rc, srcversion, load line, `/dev/zfs`, `zvol_open_timeout_ms` absent, `refcnt`, handlers); for B2 the `zfs-helpers.sh` output; for B3 the leftovers before CLEANED, the rules file and the `dmesg --follow` start.
4. **Runs:** the table `tag | boot | tree | srcversion | result | duration | zts-rc | zinject row (MATCH, INJECT) | "waits for the held open" | open rc and message | failure line | handlers after | dmesg or serial findings | refcnt before -> after` for w1, w2, w3 and fb, and the `Missing util(s)` list. Then for fb: the oops as step 7 says (GPF or BUG line, RIP, Comm, first 15 frames, delay from T0), the clean-up lines, the D tasks, `zinject-fb.txt`, `zpool list` and `refcnt`, and whether the runner returned.
5. **Unloads:** the UNLOAD of B2 (refcnt line, any `rmmod` line, rc, module count), and every REF-CAPTURE output in full.
6. **The question of this task:** on `wr`, whether `zfs set volmode=none` waited for the held open in each run (the fix holding the removal on 5.10), with a clean unload; on `fb`, whether it returned while the open was held, and the oops that followed the release. Whether this fits "Why" or contradicts it, and how. **Findings:** every CAPTURE, quoted as the VM rules say, M-41 (expected in fb only), M-30 or other; or "no VERIFY/ASSERT/PANIC/BUG/Oops/GPF/Poison/list_del/hung task in w1 to w3".
7. **This VM after the guest:** no QEMU, NO-VM-REBOOT, its kvm dmesg lines, the serial summary of step 8, the overlay's size.
8. **Restore:** purge, module list, $D removed, the comparisons, upgrades applied or kept back, final state.
9. **Verdict:** one line, e.g. "zvol_misc_remove_open (zinject framework port) on 5.10.0-46 (Debian 11 guest, nested KVM, slub_debug=P, debug builds): branch 3e42136b8d9 PASS 3/3 (removal waited for the held open, open ENXIO, use count 0, clean unload); fail-before 4072f629781 FAIL ('The minor of testpool/removeopen was removed while an open held at zvol_open:found had not returned'), then GPF in zvol_open (dd) on the release; VM never rebooted, packages as before". Then the deviations, and End.
