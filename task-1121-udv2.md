TASK-1121-UDV2 (from the coordinator session "main"): VM setup with a functional check, about 25 minutes, on THIS VM; it runs on each of the three test VMs. It installs the two OpenZFS udev rules the VMs still lack, `69-vdev.rules` and `90-zfs.rules`, with the helper `vdev_id`, system-wide and from the source the VMs build, checks them, records the clock, and proves that nothing else changed. The owner allows us to configure the VMs, with the duty to leave them clean and working. Run it at an idle point of this VM, never between the steps of another task, and never while another VM runs a performance measurement (it builds a tree). On VM 105, run it only once no host-rig task (HRIG2, HRA1 to HRA6, M29C) is pending: they boot 7.0.14-25-pve, and step 3 explains why that kernel matters here.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. No message you scheduled to yourself may be pending then.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-udv2`, `L=/var/log/task-1121-udv2`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

TASK-1117-UDV (17/11) installed only `/etc/udev/rules.d/60-zvol.rules` and `/lib/udev/zvol_id` (zvol links) on the three VMs. Still missing:

- `69-vdev.rules`: for every disk and partition it runs `/lib/udev/vdev_id -d <dev>`, which reads `/etc/zfs/vdev_id.conf` and gives the device a `/dev/disk/by-vdev/<alias>` link. `auto_replace_001_pos` and `auto_replace_002_pos` write that file and rely on the link. Without a `vdev_id.conf`, `vdev_id` prints `Error: Config file "/etc/zfs/vdev_id.conf" not found` and exits 1 on every disk or partition event, and udev imports nothing: upstream's behaviour wherever the rule is installed without a config file (systemd-udevd logs a failing import program at debug level only, so nothing is expected in the journal). It is not a finding.
- `90-zfs.rules`: `/dev/zfs` gets mode 0666 (the module creates it 0600), which tests that run `zfs` as an unprivileged user need (the `delegate` group, through `user_run`); and on any add or change event of a block or misc device that blkid identifies as ZFS (`ID_FS_TYPE` `zfs` or `zfs_member`), udev runs `/sbin/modprobe zfs`.

Source: upstream master `a1daebfec3f` ("ZTS: chattr_003_neg: accept uutils ln's wording for EPERM"), the base of what the VMs build now. The three `udev/rules.d/*.rules.in` templates are byte-identical in `c913da86156` (the source of 17/11's install), `zfs-2.4.4` and the 2.4.5 candidate; `vdev_id` is master's (2.4.x lacks its `sas_expander` topology, which no test uses). `configure` renders `@udevdir@` as `/lib/udev` on Debian 13 (`/lib/udev` exists, `/lib` being a link to `/usr/lib`), as 17/11's reports show (`PROGRAM=="/lib/udev/zvol_id $devnode"`). Step 1 builds the tree and takes the rendered files from it; their sha256 must be:

| File (in the built tree) | sha256 | Installed as |
| --- | --- | --- |
| `udev/rules.d/60-zvol.rules` | `08c4b59c3097c47989672e6c39be6949dcdd117c3b53c13900ee8d304e28ad8f` | already there since 17/11: it must equal this |
| `udev/rules.d/69-vdev.rules` | `f782f83399f26a714c73d314cda27b657ab69bfb58ac683b7b51630c816f4897` | `/etc/udev/rules.d/69-vdev.rules`, 0644 |
| `udev/rules.d/90-zfs.rules` | `70ebe772850d8b15a934c58c829d94da586750c8700416ee4c4293a0f511f0a0` | `/etc/udev/rules.d/90-zfs.rules`, 0644 (unless step 3 says otherwise) |
| `udev/vdev_id` | `e2c7d295f901d305414095cf80b22af9917a5cac06caba78357c342c4443d291` | `/lib/udev/vdev_id`, 0755 |

The rules go to `/etc/udev/rules.d`, as 17/11's did: no package owns that copy, and it does not depend on any task's tree. A reload changes nothing for devices that exist; the rules apply at each device's next event: at boot, on `udevadm trigger`, and at every `scripts/zfs.sh` load, which runs `udevadm trigger` on every device.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file. No package is installed or removed.
- Touch no block device of the VM and no pool this task did not create; never `zpool import` anything; never `zfs-tests.sh` here. The only devices this task creates are one loop device on a scratch file and one file-backed pool, both removed in the same step.
- The only system files this task may change: the three files of the table above (added), and `/etc/zfs/vdev_id.conf` for the duration of step 5 only, and only if it did not exist before. Anything else that changes is reported (step 7).
- Every build runs detached (`setsid nohup ... &`) and is polled; a build still going after 30 minutes is a hang: capture `ps -eo pid,stat,etime,comm | grep -E 'make|cc1|ld'` and `tail -n 30 $L/build.log`, stop and report. Every `udevadm settle` and `udevadm trigger --settle` is bounded (`timeout 60`); a timeout is a stop (quote `sudo -n journalctl -u systemd-udevd -n 40 --no-pager`).
- A module that will not unload, a panic or a hung task: capture `sudo -n dmesg | tail -n 150` into `$L/hang.txt`, stop and report; reboot only if the module cannot be unloaded (standing approval of 18/11), after writing `$L/report.md` and arranging your own resumption, as in your conventions.
- `sudo -n fstrim -av` after deleting the tree. No module loaded at the end.

## STEP 0: state, clock, and the "before" snapshot (about 3 minutes)

```sh
hostname; uname -r; uptime -s; nproc; free -m; df -h / /var/tmp
lsmod | grep -E '^(zfs|spl) '; ls -l /dev/zfs 2>&1; losetup -a; ls -d /var/tmp/task-1121-udv2 /var/log/task-1121-udv2 2>&1; pgrep -a -x zed || echo NO-ZED
ps -eo pid=,comm= | grep -wE 'make|cc1|ztest|zfs-tests.sh|zloop.sh|fio|hddsim'
udevadm --version; ls -l /etc/zfs 2>&1; ls -l /etc/zfs/zpool.cache 2>&1
```

Stop and report, changing nothing, if: a zfs or spl module is loaded or `/dev/zfs` exists; `losetup -a` is not empty; `$D` or `$L` exists; a build, test or measurement runs; less than 10 GiB free on /var/tmp; `/etc/zfs/zpool.cache` exists with a size above 0 (size 0: leave it, it is not this task's). Note `udevadm --version` (Debian 13: 257).

The clock (one VM read 42 minutes ahead after a hypervisor reboot on 18/11), recorded, not changed:

```sh
timedatectl; timedatectl show --property=NTP --property=NTPSynchronized 2>&1
systemctl is-active systemd-timesyncd chrony ntpsec 2>&1 | tr '\n' ' '; echo
date -u '+local %a, %d %b %Y %H:%M:%S GMT'; curl -sI --max-time 10 https://github.com | grep -i '^date:'
```

Expected `System clock synchronized: yes`, `NTP service: active`, and the local time within 2 seconds of the server's `date:` header. Anything else is a finding to report (do not enable or change anything).

Then the marker and the snapshot. Run this block as it is now, and again in step 7 with `t=after`:

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2; t=before
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && { [ -e $L/t0 ] || touch $L/t0; }
{
echo "== udev rules and helpers"
for f in /etc/udev/rules.d/* /usr/lib/udev/rules.d/*zfs* /usr/lib/udev/rules.d/*vdev* /usr/lib/udev/rules.d/*zvol* /usr/local/lib/udev/rules.d/* /run/udev/rules.d/* /usr/lib/udev/zvol_id /usr/lib/udev/vdev_id /etc/zfs/vdev_id.conf; do [ -e "$f" ] && echo "$(stat -c '%a %U' "$f") $(sha256sum < "$f" | cut -d' ' -f1) $f"; done
echo "== /dev"
ls -l /dev/root /dev/zfs 2>&1; find /dev/disk /dev/zvol -maxdepth 2 -printf '%p -> %l\n' 2>&1 | grep -v -e '/by-diskseq/' -e '/by-loop-' | sort
echo "== block devices"
lsblk -rno NAME,TYPE,FSTYPE,MOUNTPOINTS | grep -v -E '^(loop|zd|ublkb)'
for n in $(lsblk -rno NAME,TYPE | awk '($2 == "disk" || $2 == "part") && $1 !~ /^(loop|zd|ublkb)/ {print $1}'); do echo "-- $n"; udevadm info -q property -n /dev/$n | grep -v -E '^(USEC_INITIALIZED|DEVLINKS|TAGS|CURRENT_TAGS)=' | sort; udevadm info -q symlink -n /dev/$n | tr ' ' '\n' | grep -v by-diskseq | sort; done
echo "== modules, failed units, root"
lsmod | awk 'NR > 1 {print $1}' | sort | tr '\n' ' '; echo
systemctl --failed --no-legend --plain
findmnt -no SOURCE,FSTYPE /
} > $L/state-$t.txt 2>&1; wc -l < $L/state-$t.txt; sed -n '/^== udev rules/,/^== \/dev/p' $L/state-$t.txt
```

Expected (before): in the first section `/etc/udev/rules.d/60-zvol.rules` (0644, `08c4b59c...`), `/usr/lib/udev/zvol_id` (0755), and none of `69-vdev.rules`, `90-zfs.rules`, `vdev_id`, `vdev_id.conf`. A `69-vdev.rules`, `90-zfs.rules` or `vdev_id` already present anywhere is a stop: report its line and its first 5 lines, change nothing. Quote the `== /dev` section too (`/dev/root` absent and `/dev/zfs` absent expected).

## STEP 1: the source tree, built (about 8 minutes)

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2
mkdir $D/z && cd $D/z && git init -q . && git fetch -q --depth 1 https://github.com/openzfs/zfs a1daebfec3f5ca46ac261ef339efdf0dbeae997c && git checkout -q --detach FETCH_HEAD && echo "$(git rev-parse HEAD 'HEAD^{tree}' | tr '\n' ' ')$(git status --porcelain | wc -l)"
setsid nohup sh -c "cd $D/z && ./autogen.sh && ./configure --enable-debug && nice make -j$(nproc); echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Expected `a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92 0`. If the fetch fails, wait 1 minute and try once more, then fetch the same commit from https://github.com/tigerblue77/zfs instead (say so). Poll `tail -n 1 $L/build.log` until `build-rc=`; it must be `build-rc=0` (quote the duration and any `warning:` line outside autogen's known `AM_GNU_GETTEXT` one). Then:

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2
cd $D/z && grep -E '^udev(rule)?dir = ' Makefile
printf '%s\n' "08c4b59c3097c47989672e6c39be6949dcdd117c3b53c13900ee8d304e28ad8f  udev/rules.d/60-zvol.rules" "f782f83399f26a714c73d314cda27b657ab69bfb58ac683b7b51630c816f4897  udev/rules.d/69-vdev.rules" "70ebe772850d8b15a934c58c829d94da586750c8700416ee4c4293a0f511f0a0  udev/rules.d/90-zfs.rules" "e2c7d295f901d305414095cf80b22af9917a5cac06caba78357c342c4443d291  udev/vdev_id" | sha256sum -c
grep -n '/lib/udev' udev/rules.d/69-vdev.rules; cat udev/rules.d/90-zfs.rules
```

Expected `udevdir = /lib/udev`, `udevruledir = /lib/udev/rules.d`, four `OK` lines, the three `/lib/udev/vdev_id` lines of 69-vdev.rules (and its comment line), and 90-zfs.rules as quoted in "Why". Any difference: stop and report (do not install anything rendered differently).

## STEP 2: nobody else owns these paths

```sh
dpkg -S /etc/udev/rules.d/69-vdev.rules /etc/udev/rules.d/90-zfs.rules /lib/udev/vdev_id /usr/lib/udev/vdev_id /lib/udev/rules.d/69-vdev.rules /lib/udev/rules.d/90-zfs.rules 2>&1
sha256sum /etc/udev/rules.d/60-zvol.rules
```

Expected `no path found matching pattern` for each, and 17/11's `60-zvol.rules` with `08c4b59c...` (the same rendering as this tree's). A package that owns one of them: stop and report.

## STEP 3: would 90-zfs.rules load a ZFS module by itself?

`90-zfs.rules` runs `/sbin/modprobe zfs` when a ZFS-labelled block device appears or changes while no ZFS module is loaded (at boot, on any `udevadm trigger`). On a VM whose test disks carry ZFS labels (VM 105's exported `tank`, for one), that loads the installed kernel's own ZFS module if it has one, before any task: every task's "no zfs module loaded" check would then stop, and the in-tree module of a task would not load over it. The Proxmox kernel 7.0.14-25-pve that VM 105 boots for the host rig ships its own `zfs.ko`.

```sh
for k in /lib/modules/*; do k=${k##*/}; echo "$k zfs=$(/usr/sbin/modinfo -k $k -n zfs 2>&1) spl=$(/usr/sbin/modinfo -k $k -n spl 2>&1)"; done
ls /boot/vmlinuz-* 2>&1
lsblk -rno NAME,TYPE,FSTYPE,SIZE | awk '$3 ~ /zfs/'
```

Rule (fixed here): if `modinfo` finds no `zfs` module for any kernel listed (every line `zfs=modinfo: ERROR: Module zfs not found.`), install both rules. If it finds one for any kernel, install `69-vdev.rules` and `vdev_id` only, NOT `90-zfs.rules`, and report the module path(s) and the ZFS-labelled devices: main decides (that is not a stop for the rest of the task). Quote the three outputs either way.

## STEP 4: install and reload

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2; z=$D/z
sudo -n install -m 0755 $z/udev/vdev_id /lib/udev/vdev_id; echo rc=$?
sudo -n install -m 0644 $z/udev/rules.d/69-vdev.rules /etc/udev/rules.d/69-vdev.rules; echo rc=$?
sudo -n install -m 0644 $z/udev/rules.d/90-zfs.rules /etc/udev/rules.d/90-zfs.rules; echo rc=$?
```

(The third line only if step 3 allowed it.) Then:

```sh
L=/var/log/task-1121-udv2
ls -l /lib/udev/vdev_id /etc/udev/rules.d/60-zvol.rules /etc/udev/rules.d/69-vdev.rules /etc/udev/rules.d/90-zfs.rules 2>&1; sha256sum /lib/udev/vdev_id /etc/udev/rules.d/*.rules
f=$(ls /etc/udev/rules.d/60-zvol.rules /etc/udev/rules.d/69-vdev.rules /etc/udev/rules.d/90-zfs.rules 2>/dev/null); sudo -n udevadm verify --no-style $f; echo verify-rc=$?; sudo -n udevadm verify $f 2>&1 | tail -n 20
date +%s > $L/t-reload; sudo -n udevadm control --reload; echo reload-rc=$?
sleep 2; sudo -n journalctl -u systemd-udevd --since "@$(cat $L/t-reload)" --no-pager
```

Expected: the rc lines 0; the files with the modes and sums of "Why"; `verify-rc=0` (syntax), then the same check with style notes, quoted whole (a style note is information: these are upstream's files, installed unchanged); reload-rc 0 and no journal line naming a rules file. A syntax error, a verify-rc above 0 or a reload error: undo (step 8's undo line), stop and report. If this `udevadm` has no `verify` command (systemd before 253), record it and go on: steps 5 and 6 exercise the rules.

## STEP 5: the rules on a scratch loop device, no ZFS module (about 2 minutes)

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2
truncate -s 64M $D/loop.img && LOOP=$(sudo -n losetup -f --show $D/loop.img) && echo $LOOP > $L/loop.txt && timeout 60 sudo -n udevadm settle; echo $LOOP rc=$?
LOOP=$(cat $L/loop.txt); sudo -n udevadm test --action=change /sys/class/block/${LOOP#/dev/} > $L/udevtest-loop.txt 2>&1; echo test-rc=$?; grep -n -E '69-vdev|90-zfs|vdev_id' $L/udevtest-loop.txt | head -n 20
```

Expected: a `/dev/loopN`; in the test output, lines naming `/etc/udev/rules.d/69-vdev.rules` that run `/lib/udev/vdev_id -d loopN` and report that it returned 1 (no `vdev_id.conf`), and lines of `90-zfs.rules` that reach `zfs_end` with nothing to run (the device is no ZFS member). `udevadm test` runs the rules' import programs (`vdev_id`, which only reads) and runs no `RUN` command; quote the lines whatever their exact wording.

Then the alias, end to end, only if `/etc/zfs` exists and `/etc/zfs/vdev_id.conf` does not (step 0's listing; otherwise skip it and say why):

```sh
L=/var/log/task-1121-udv2; LOOP=$(cat $L/loop.txt)
link=$(udevadm info -q symlink -n $LOOP | tr ' ' '\n' | grep -m 1 '^disk/by-diskseq/'); echo "link=[$link]"
test -n "$link" && test ! -e /etc/zfs/vdev_id.conf && echo "alias udvtest /dev/$link" | sudo -n tee /etc/zfs/vdev_id.conf && timeout 60 sudo -n udevadm trigger --action=change --settle $LOOP; echo trigger-rc=$?
udevadm info -q property -n $LOOP | grep -E '^ID_VDEV'; ls -l /dev/disk/by-vdev 2>&1
sudo -n rm -f /etc/zfs/vdev_id.conf && timeout 60 sudo -n udevadm trigger --action=change --settle $LOOP; echo trigger-rc=$?
udevadm info -q property -n $LOOP | grep -E '^ID_VDEV' || echo NO-ID_VDEV; ls -l /etc/zfs/vdev_id.conf 2>&1
sudo -n losetup -d $LOOP; rm -f $D/loop.img; losetup -a; ls /dev/disk/by-vdev 2>&1
```

Expected: a `disk/by-diskseq/<n>` link; `ID_VDEV=udvtest` and `ID_VDEV_PATH=disk/by-vdev/udvtest` imported by the rule; no `/dev/disk/by-vdev` link for it, by design (the rule links whole disks whose name ends in a letter, such as the `sdX` scsi_debug disks of `auto_replace_*`; `loopN` ends in a digit); after the removal `NO-ID_VDEV` and no `vdev_id.conf`; `losetup -a` empty and no `/dev/disk/by-vdev`. Anything else: report it as it is (and make sure `vdev_id.conf` is gone and the loop device detached before going on).

## STEP 6: the rules with a ZFS module loaded (about 2 minutes)

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2
cd $D/z && date +%s > $L/t-load && sudo -n ./scripts/zfs.sh; echo load-rc=$?; cat /sys/module/zfs/srcversion; sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1
stat -c '%a %U %G %n' /dev/zfs
truncate -s 256M $D/udv.img && sudo -n ./zpool create -o cachefile=none udv2pool $D/udv.img && sudo -n ./zfs create -V 64M udv2pool/vol && timeout 60 sudo -n udevadm settle; echo rc=$?
ls -l /dev/zvol/udv2pool/vol; readlink -f /dev/zvol/udv2pool/vol
sudo -n ./zpool destroy udv2pool; echo destroy-rc=$?; rm -f $D/udv.img; sudo -n ./scripts/zfs.sh -u; echo unload-rc=$?; lsmod | grep -E '^(zfs|spl) '; ls -l /dev/zfs 2>&1
sudo -n journalctl -u systemd-udevd --since "@$(cat $L/t-load)" --no-pager > $L/udevd-load.txt; grep -c 'vdev_id -d .*failed' $L/udevd-load.txt; grep -v -E 'vdev_id -d .*failed|^-- ' $L/udevd-load.txt | head -n 20
```

Expected: load-rc 0 and the "(DEBUG mode)" load line; `666 root root /dev/zfs` with `90-zfs.rules` installed (`600 root root /dev/zfs` where step 3 left it out: say so); the pool and the zvol created, the link `/dev/zvol/udv2pool/vol -> ../../zdN` and `/dev/zdN` (17/11's rule still works); destroy-rc and unload-rc 0, no module, no `/dev/zfs`. In the udevd journal since the load: the count of `vdev_id ... failed` lines (0 expected at udevd's default log level; any count is fine) and no other line, nothing about `modprobe` included (on a VM with ZFS-labelled disks and `90-zfs.rules`, the module is already loaded when the load's trigger runs): quote any other line.

## STEP 7: nothing else changed

Run step 0's snapshot block again with `t=after`, then:

```sh
L=/var/log/task-1121-udv2
diff $L/state-before.txt $L/state-after.txt; echo diff-rc=$?
sudo -n find /etc /usr/lib/udev /usr/local /usr/sbin /usr/bin -xdev -newer $L/t0 \( -type f -o -type l \) 2>/dev/null | sort
ls /var/tmp | grep -E '^(file-vdev|dev-|udv|zfs-tests)' ; losetup -a; zpool list 2>&1 | head -n 2
```

Expected: the diff shows only the new lines of the first section (`/etc/udev/rules.d/69-vdev.rules` and `/usr/lib/udev/vdev_id`, plus `/etc/udev/rules.d/90-zfs.rules` where installed); the `== /dev`, `== block devices` and `== modules` sections identical, save a module that step 5 or 6 loaded and that stays (`loop`, `zlib_deflate`, ...: name it); `find` lists exactly the installed files (`/etc/udev/rules.d/69-vdev.rules`, `/etc/udev/rules.d/90-zfs.rules`, `/usr/lib/udev/vdev_id`). Every other line of the diff or of `find` is reported with what it is (a file the system rewrites by itself, such as `/etc/resolv.conf` or `/etc/adjtime`, is named as such); none is undone without asking main. No leftover in /var/tmp, no loop device; `zpool` is not installed system-wide, so its "command not found" is expected.

## STEP 8: cleanup, conventions, undo

```sh
D=/var/tmp/task-1121-udv2; L=/var/log/task-1121-udv2
cd / && rm -rf $D && sudo -n fstrim -av && df -h /var/tmp
```

Append to `/var/log/VM-CONVENTIONS.md` (with `sudo -n tee -a`) a short section "Update 2026-11-21 (TASK-1121-UDV2)": what was installed (paths, sums, from `a1daebfec3f`), whether `90-zfs.rules` was left out and why, that `vdev_id` fails (silently) on every disk event while no `/etc/zfs/vdev_id.conf` exists, and the undo:

```sh
sudo -n rm -f /etc/udev/rules.d/69-vdev.rules /etc/udev/rules.d/90-zfs.rules /lib/udev/vdev_id && sudo -n udevadm control --reload
```

(17/11's `60-zvol.rules` and `zvol_id` stay. Devices keep what the rules gave them until their next event or the next boot: after an undo, `sudo -n udevadm trigger --action=change --subsystem-match=block` once with no ZFS module loaded, or a reboot, puts them back.) Do not run the undo now. Keep `$L`.

## Report

Write `$L/report.md` and give the same text as your final message (at most 60 lines):

1. VM, kernel, `udevadm --version`, free space; the clock: `timedatectl`'s synchronized and NTP lines, the time service, local against server time.
2. Step 1: HEAD and tree, build rc and duration, `udevdir`, the four sums OK. Step 2: owners (none expected).
3. Step 3: the module lines per kernel, the ZFS-labelled devices, and the decision (both rules, or `90-zfs.rules` left out and why).
4. Steps 4 to 6: install rcs, `ls -l` and sums, `udevadm verify` output, reload; the loop device's test lines, the `ID_VDEV` lines with the alias and after its removal; `/dev/zfs` mode; the zvol link; the count of `vdev_id` failure lines (0 expected) and any other udevd line.
5. Step 7: the diff (whole) and the `find` list, each unexpected line explained.
6. Cleanup, the conventions note, the undo command, then End.
