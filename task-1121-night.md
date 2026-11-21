TASK-1121-NIGHT (from the coordinator session "main"): the schedule of the three test VMs for the night of 21 to 22 November 2026, from 19:00Z, and the restore after it. Main cannot wake itself at night, so each VM runs its own window by itself, at fixed UTC times, on wake-ups it schedules in its own session, with the GO of its measured parts given in advance by main's message. One VM works at a time. Nothing is published on GitHub, no credential files. Logs: `/var/log/night-1121/`.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat in the morning, or earlier if it can. Every note for main is the end of a turn in your own chat.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: each block below sets what it needs.

## 1. The night (UTC)

Main's message names your window. Default assignment:

| window | VM | wake-up | whole content fits if begun by | hard end (HE) | items, in this order |
|---|---|---|---|---|---|
| S1 | 107 | 19:00Z | 19:10Z | 02:25Z | X3, X4, V10P, restore |
| S2 | 106 | 02:30Z | 02:40Z (03:10Z if TQL3 is staged) | 05:30Z | TQL3, restore |
| S3 | 105 | 05:35Z | 05:45Z | 10:00Z | HRIG2's measured part and step 10, only if they have not run by day; AR3; restore |
| U | all three | 10:05Z | - | - | unmask the timers (6.7), nothing else |

"Begun by" is the latest time at which the window's first step can begin with every item still fitting; a later start drops items from the end (4.3). The windows do not overlap and no VM works past its HE: that is the only coordination, because no VM can see another.

Caps, in minutes, used by the start rule (each task's own maximum, split): prep = the task's steps up to its READY line, measured = from GO to the end of its measured steps, post = the rest of the task, its report included.

| item | task file (sha256 starts with) | prep | measured | post | prep / measured / post steps |
|---|---|---|---|---|---|
| X3 | task-1120-x3.md (53fad4dec7c6a8f6) | 45 | 90 | 15 | 0-4 / 5 / 6 |
| X4 | task-1120-x4.md (9d671348412e196a) | 45 | 90 | 15 | 0-4 / 5 / 6-7 |
| V10P | task-1120-v10p.md (4f9dd2d59470cee7) | 40 | 90 | 15 | 0-4 / 5-6 / 7 |
| TQL3 | task-1121-tql3.md (9d1858fee4e6603d) | 30 | 90 | 15 | 0-4 / 5-6 / 7 |
| HRIG2 | task-1121-hrig2.md (23e55484fc947cc7) | 55 | 75 | 20 | 0-6 / 7-9 / 10 |
| AR3 | task-1121-ar3.md (27db6a8f49aff169) | 70 | 115 | 30 | 0-5 / 6 / 7-END |
| restore | this file, 6.1 to 6.6 | - | - | 35 | - |

## 2. Rules for the whole night

- **One VM works at a time.** Inside your window you run its items. Outside it you run nothing: no build, test, fio, git fetch, apt, fstrim, trim or reboot, and no module load or unload. Waiting at a task's READY line (modules loaded, a pool imported, nothing running) counts as idle. Before 19:00Z the setup and the staging of section 3 may run while other VMs work, but never while a measured part runs anywhere: main's message says when.
- **UTC only.** Read every time with `date -u`; this VM's local zone is not UTC.
- **You never check or contact the other VMs.**
- **A GO in main's night message is deferred:** it starts nothing outside your window, and nothing before the start rule allows it.
- **A message from main wins over this file:** `HOLD` (start nothing new; finish the running step or pass only as far as its task allows, tear down as the task says, wait), `ABORT <task id>` (that task's abort path), `START <window> NOW` (main has checked the other VMs: treat now as your window's wake-up; its HE does not move), `RESTORE NOW` (your restore now, then 6.7). By day, a plain message from main with a GO for a task is an ordinary GO.
- **What this file replaces tonight, and only tonight**, in the task files: (a) "wait for main's GO" (4.4); (b) "no scheduled resumption of yours may be pending" (your night wake-ups stay pending); (c) "stay idle until main writes" after a READY line or a final report (4.5); (d) "wait for main (no reboot)" after a kernel line (5.2). Everything else in each task applies as written.
- **progress.md:** `/var/log/night-1121/progress.md` gets one line per event, `<UTC> <window> <item> <event>`, appended before and after every step (`echo ... | tee -a`, then `sync -f` on the file). After a restart it is how you find where you stand.

## 3. Setup (by day, when main's message asks for it; about 10 minutes plus the staging)

3.1 Files:
```
N=/var/log/night-1121; sudo -n mkdir -p $N && sudo -n chown "$(id -un)" $N && cp /var/tmp/tasks-1121-night/task-1121-night.md $N/ && sha256sum $N/task-1121-night.md
echo "$(date -u +%FT%TZ) setup start $(hostname) $(uname -r)" | tee -a /var/log/night-1121/progress.md
```

3.2 Clock (4.2 repeats the measure):
```
date -u; timedatectl show -p Timezone -p NTP -p NTPSynchronized; systemctl is-active systemd-timesyncd chrony 2>&1 | tr '\n' ' '; echo
h=$(curl -sI --max-time 10 https://github.com | tr -d '\r' | sed -n 's/^[Dd]ate: //p'); echo "http_date=$h offset_s=$(( $(date -u +%s) - $(date -u -d "$h" +%s) ))"
```
Expected: `NTP=yes`, `NTPSynchronized=yes`, `offset_s` between -5 and 5. If `NTP=no`: `sudo -n timedatectl set-ntp true`, then wait up to 3 minutes for the synchronization (`for i in $(seq 18); do [ "$(timedatectl show -p NTPSynchronized --value)" = yes ] && break; sleep 10; done; timedatectl show -p NTPSynchronized`) and measure again. If `offset_s` is still beyond ±10: `sudo -n date -u -s "@$(date -u -d "$h" +%s)"` once, measure again, and say so. If github.com does not answer, use https://deb.debian.org. Report every line.

3.3 Background timers. Debian's apt timers fire at night (apt-daily-upgrade around 05:00Z to 06:00Z, apt-daily at a random time) and would load the host during another VM's measurement, or your own:
```
systemctl list-timers --all --no-pager | head -25
for t in apt-daily.timer apt-daily-upgrade.timer man-db.timer e2scrub_all.timer; do printf '%s %s %s\n' $t "$(systemctl is-enabled $t 2>&1)" "$(systemctl is-active $t 2>&1)"; done
pgrep -a -f 'unattended-upgr|apt.systemd.daily|apt-get|dpkg' || echo no-apt
```
If an apt or dpkg process runs, wait until the last command prints `no-apt` (check once a minute, at most 30 minutes). Then mask the timers that exist:
```
N=/var/log/night-1121; for t in apt-daily.timer apt-daily-upgrade.timer man-db.timer e2scrub_all.timer; do systemctl cat $t > /dev/null 2>&1 && sudo -n systemctl mask --now $t && echo "$(date -u +%FT%TZ) setup masked $t" | tee -a $N/progress.md; done
```
A masked timer stays masked across a reboot; 6.7 unmasks exactly the ones progress.md lists.

3.4 Wake-up test. Create a one-shot scheduled task of your own session (the kind you use to resume after a reboot), 2 minutes ahead, with the text `NIGHT-1121 wake-up test`. Write its id in progress.md and end your turn with one line. When it arrives, write `wake-up test OK <UTC>` in progress.md and go on with 3.5.

3.5 Your wake-ups: one-shot scheduled tasks of your own session at your window's wake-up time and 4 minutes after it, and at 10:05Z and 10:09Z for U (section 1), each with the text `NIGHT-1121 wake-up <window> (VM <your VM id>): read /var/log/night-1121/task-1121-night.md and progress.md, then act.` Write each id and time in progress.md.

3.6 Setup table, in your chat: VM, hostname, `uname -r`, clock (NTP, synchronized, `offset_s`, any fix), timers masked, wake-up test, wake-up ids and times, staged task and its state.

3.7 Staging, only if main's message asks for it: the task's steps up to its READY line, begun no later than 18:55Z minus its prep cap (section 1), so that they end by 18:55Z. At the READY line, end your turn as the task says, its READY line last. If you cannot begin in time, do not begin: your window does it (4.3).

## 4. Running your window

4.1 On any wake-up of yours, first `date -u` and read progress.md (a message from main is handled as section 2 says).
- Before your window: if neither of its wake-ups is pending any more, create one at its time; one line; end your turn.
- After its HE and before 10:05Z: one line ("outside my window"), nothing else.
- Inside the window: go on from where progress.md stands. A wake-up that arrives while you already work on the window gets one line.

4.2 At the start of the window: `<UTC> <window> START` in progress.md; 3.2's measure (if `offset_s` is beyond ±10 seconds, take the HTTP time for every decision of the night and say so); `systemctl is-enabled` of the timers 3.3 masked (each `masked`; if one is not, mask it again as in 3.3); the state your staged task left at its READY line, checked as the task checks it before its measured part.

4.3 The start rule, with the caps of section 1 and your HE; the restore's 35 minutes are always kept:
- an item's prep may begin only if now + prep + measured + post + 35 ≤ HE (no prep for a task already at its READY line);
- a measured part may begin only if now + measured + post + 35 ≤ HE;
- if not, this item and every later one but the restore are skipped: write `<UTC> <window> <item> SKIPPED (start rule: now <time>, needs <minutes> min, HE <HE>)` and go to the restore.
Items run in the order of section 1. An item whose task stopped (section 5) is over; the next item begins with its own step 0, whose checks decide whether the state the stop left allows it.

4.4 The GO. Main's message gave, in advance, the GO of each measured part of your window (its `GO TASK-...` lines). When the start rule allows the measured part, write `<UTC> <window> <item> GO (night schedule)` in progress.md and in your chat, then go on with the task's measured steps. That time is the GO time the task's time rule counts from. The task's quiet check runs as it says.

4.5 Turns. Where a task tells you to end your turn (its READY line, its final report), end it so, after creating a one-shot scheduled task of your own session 2 minutes ahead (text `NIGHT-1121 continue <window>`); when it arrives, go on with 4.3. A final report stays the full last message of its turn.

4.6 Long steps. Before a build, a chain of passes or any step expected to run more than 10 minutes in background mode, create a one-shot scheduled task of your own session at its expected end plus 15 minutes, never later than HE minus 5 minutes (text `NIGHT-1121 check <window> <item>`), and delete it when the step ends in the same turn. If it fires because your session was restarted: read progress.md and the task's driver log. A step that died is not resumed and a measured pass that was cut is not retried: tear down as the task says, report, and go on with 4.3.

4.7 The hard end. At HE nothing of this window runs (no build, test, pass, fio, sampler, apt). A task step still running at HE minus 35 minutes has passed its cap: stop it the way its task handles a hang (capture, then teardown), write "stopped at the hard end", and go to the restore.

## 5. Stops

5.1 Each task's stop conditions apply as written: stop, capture what it asks, report; never retry a measured pass; tear down where the task says.

5.2 A kernel line (`VERIFY`, `PANIC`, `BUG:`, `Oops`, `Call Trace`, a hung task): capture as the task says, plus `ps -eLo pid,tid,stat,comm,wchan:32 | grep ' D'`, the kernel stack of each D task (`sudo -n cat /proc/<pid>/stack`), `cat /proc/pressure/cpu /proc/pressure/io`, and two `head -1 /proc/stat` 60 seconds apart. Then, tonight only (2(d)): reboot (the owner's standing approval) if your window still has an item the start rule allows, or if the two `/proc/stat` lines show this VM busy above 5 % of its CPU time: `sync -f` your logs and progress.md, create a one-shot scheduled task of your own session 5 minutes ahead, reboot through a transient unit as in your earlier tasks, at the latest at HE minus 45 minutes. After the boot: `uname -r` (the kernel the next item needs), 4.2's checks, then 4.3. Otherwise leave the VM as it is for main, say so, and run the restore's 6.1 as a report only.

5.3 A pool, device or process the task did not create, or anything destructive or ambiguous: stop, report, touch nothing. The window's later items are skipped; the restore runs 6.1 as a report only.

5.4 A second stop in the same window ends its items: go to the restore.

## 6. Restore (the last item of your window; `RESTORE NOW` from main starts it at once)

6.1 State, read only:
```
date -u; uname -r; uptime -s; lsmod | grep -E '^(zfs|spl|null_blk|brd|ublk_drv|dm_delay) '; ls /dev/zfs /dev/ublk* /dev/ram0 2>&1; losetup -a; sudo -n dmsetup ls; ls /sys/kernel/config/nullb 2>&1; ps -C hddsim,fio,ztest -o pid=,comm=; ls -ld /var/tmp/task-* /var/tmp/x14.* 2>&1; ls -l /etc/zfs/zpool.cache 2>&1; df -h / /var/tmp
```
Expected: the Debian 6.12 kernel; no module, device or process listed; no tree of tonight's tasks (their logs stay under /var/log). A tree of one of tonight's tasks that a stop left: `sudo -n rm -rf` it and say which. Anything else (another task's tree, a module, a pool, a cache file above 0 bytes): report it and leave it. On VM 105, if HRIG2 has not run: its 7.0.14-25-pve boot, its kept tree and its logs stay as they are, since the task is still pending; then skip 6.3 and 6.4.

6.2 The night's task checkout: `sudo -n rm -rf /var/tmp/tasks-1121-night` (your copy of this file stays in /var/log/night-1121).

6.3 Up to date, without a reboot, the timers still masked:
```
N=/var/log/night-1121; sudo -n apt-get -o DPkg::Lock::Timeout=300 update > $N/apt-update.log 2>&1; echo rc=$?; apt list --upgradable 2>/dev/null | tail -n +2 | tee $N/upgradable.txt; sudo -n apt-get -s upgrade | grep -E '^(Inst|Remv)|upgraded,'
```
With no `Remv` line and at least one `Inst`: `sudo -n env DEBIAN_FRONTEND=noninteractive apt-get -y -o DPkg::Lock::Timeout=300 upgrade > $N/apt-upgrade.log 2>&1; echo rc=$?; tail -n 3 $N/apt-upgrade.log`. What `upgrade` keeps back (a new kernel package, for one) is reported, not installed: main decides by day. No reboot.

6.4 The ZFS udev rules 69-vdev.rules and 90-zfs.rules (60-zvol.rules has been installed since 18/11). They are installed now, after the night's measurements, so that those ran in the same setup as the earlier runs they are compared with:
```
ls -l /etc/udev/rules.d/ /usr/lib/udev/rules.d/ 2>&1 | grep -E 'zvol|vdev|zfs'; U=$(sed -n 's#.*PROGRAM=="\([^"]*\)/zvol_id .*#\1#p' /etc/udev/rules.d/60-zvol.rules | head -n 1); echo "U=$U"; ls -l "$U/zvol_id"; ls /lib/modules/*/zfs/zfs.ko* /lib/modules/*/*/zfs/zfs.ko* 2>/dev/null
```
Skip the rest of 6.4 (report, change nothing) if a 69-vdev.rules or 90-zfs.rules already exists in either directory, `U` is empty or `$U/zvol_id` is missing. Otherwise:
```
N=/var/log/night-1121; U=$(sed -n 's#.*PROGRAM=="\([^"]*\)/zvol_id .*#\1#p' /etc/udev/rules.d/60-zvol.rules | head -n 1); d=$(mktemp -d /var/tmp/x14.XXXXXX) && git -C "$d" init -q && git -C "$d" fetch -q --depth 1 https://github.com/openzfs/zfs master && git -C "$d" checkout -q FETCH_HEAD -- udev && git -C "$d" rev-parse FETCH_HEAD && sed "s#@udevdir@#$U#g" "$d/udev/rules.d/69-vdev.rules.in" > $N/69-vdev.rules && grep -v 'RUN+="/sbin/modprobe zfs"' "$d/udev/rules.d/90-zfs.rules.in" > $N/90-zfs.rules && grep -c '@' $N/69-vdev.rules $N/90-zfs.rules; diff "$d/udev/rules.d/90-zfs.rules.in" $N/90-zfs.rules; sudo -n install -m 0644 $N/69-vdev.rules /etc/udev/rules.d/69-vdev.rules && sudo -n install -m 0644 $N/90-zfs.rules /etc/udev/rules.d/90-zfs.rules && sudo -n install -m 0755 "$d/udev/vdev_id" "$U/vdev_id" && sudo -n udevadm control --reload; echo rc=$?; sudo -n udevadm verify /etc/udev/rules.d/69-vdev.rules /etc/udev/rules.d/90-zfs.rules; echo verify_rc=$?; ls -l /etc/udev/rules.d/ "$U/vdev_id"; rm -rf "$d"
```
Expected: the master commit; `grep -c` prints `:0` for both files; the diff shows exactly the two removed `RUN+="/sbin/modprobe zfs"` lines; `rc=0`; `udevadm verify` reports no error (if the command does not exist, say so). The two modprobe lines are left out because every task here loads the module it built itself: on a kernel that ships its own zfs module (the first block lists any; the Proxmox 7.0 kernel VM 105 boots for the host-rig tasks may), they would load that module whenever a disk with a ZFS label appears. No `udevadm trigger`: the rules apply from the next device event.

6.5 `sudo -n fstrim -av; df -h / /var/tmp`.

6.6 Final report, as your last message: per item of your window its begin, GO and end times, the verdict or result line of its report and the report path; what was skipped or stopped, and why; reboots; the clock at setup and at the window start; timers masked; apt (the `upgrade` summary, what was kept back); udev (master commit, files, verify); 6.1's state; fstrim; and, as the very last line, `NIGHT-1121 VM <id> RESTORED <UTC>, timers unmask at 10:05Z`. Write `<UTC> <window> END` in progress.md first.

6.7 U, at 10:05Z (or right after the restore on `RESTORE NOW`): unmask and start exactly the timers progress.md lists as masked, then one line in your chat.
```
N=/var/log/night-1121; for t in $(sed -n 's/.* setup masked //p' $N/progress.md); do sudo -n systemctl unmask $t && sudo -n systemctl start $t && echo "$(date -u +%FT%TZ) U restored $t" | tee -a $N/progress.md; done; systemctl list-timers --all --no-pager | head -12
```
apt-daily-upgrade may start at once (it missed its morning run): let it run. Delete every scheduled task of yours whose text starts with `NIGHT-1121` and is still pending, and end with the line `NIGHT-1121 VM <id> DONE <UTC>`.

## 7. Per VM

- **VM 107 (S1).** X3 is staged by day if main's message asks for it. X3's step 6 removes `/var/tmp/task-1120-x3` and keeps `/var/log/task-1120-x3` whole: X4 reads it, and X4's step 0 checks both. X4 and V10P are not staged by day: S1 runs their steps 0 to 4.
- **VM 106 (S2).** TQL3 is staged by day if main's message asks for it. TQL2 is not run tonight.
- **VM 105 (S3).** HRIG2's measured part runs by day on a plain GO from main when it can. S3 runs HRIG2's measured part only if HRIG2 is at its READY line at the window start: its prep does not fit S3. HRIG2's step 10 reboots into Debian inside S3, as the task says: resume by your own arrangement, then 4.2's checks (the timers stay masked), HRIG2's report, then the next item. AR3 is not staged: S3 runs it from its step 0, and the start rule leaves it out when HRIG2 ran in S3. AR3's step 4 boots 7.0.14-25-pve and its END boots back into Debian, both inside S3: before each, a one-shot scheduled task of your own session 5 minutes ahead; after each, 4.2's checks (the timers stay masked), then the task goes on. AR3's END keeps its four trees under `~/work` (`z-P3`, `z-P3C2`, `z-P3C3`, `z-P3A`): the restore leaves them.
