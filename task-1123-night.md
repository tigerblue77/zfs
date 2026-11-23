TASK-1123-NIGHT (from the coordinator session "main"): the night of Monday 23 to Tuesday 24 November 2026, 22:00Z to 07:30Z, for the three test VMs, and the evening before it. Night mode (the owner's rule): one VM works at a time, and each measured part runs alone on the host. Tonight VM 105 works the whole night in one window: the host rig's arms passes back to back (TASK-1121-HRA1, its second attempt, staged by day from 21:00Z, then HRA2 to HRA5), then TASK-1121-WBP2V2 if the start rule leaves room for it; TASK-1121-HRA6 is not run tonight. The HRA tasks use kit v2.2, which TASK-1123-HRAS ran on VM 105 on 23/11 (verdict PASS). VMs 106 and 107 finish their day queues by 21:50Z (section 3) and run nothing from 21:50Z to 07:30Z. Main may not be awake: VM 105 runs its window by itself, wakes itself, and never checks or contacts the other VMs. On Tuesday every VM waits for main's `DAY NOW` message (section 6).

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters.

**This file reuses TASK-1121-NIGHT** (`task-1121-night.md`, same branch `vm-tasks-1118`) for its mechanics, with these substitutions everywhere: `night-1121` -> `night-1123`, `NIGHT-1121` -> `NIGHT-1123`, `/var/tmp/tasks-1121-night` -> `/var/tmp/tasks-1123-night`, and its U time `10:05Z` -> `07:30Z`. From it apply: section 2 (rules for the whole night), 3.3 only (mask the four timers, logging `setup masked <timer>` in progress.md), section 4 (running your window: wake-ups, 4.3's start rule with the caps below and your HE, GO, turns, long steps, the hard end), section 5 (stops) and section 6 (restore 6.1 to 6.6, then U 6.7). Nothing else of it (its setup steps 3.1, 3.2, 3.4 to 3.6, its windows and its per-VM section do not apply). Its section 2's "outside your window you run nothing" holds from 21:50Z; before 21:50Z, section 3 of this file governs (the day queues, and on VM 105 the HRA1 staging).

## 1. The night (UTC)

| Window | VM | wake-up | items, in this order | HE |
|---|---|---|---|---|
| N1 | 105 | 22:00Z (and 22:04Z), or main's `START N1 NOW` | HRA1 (from its READY line if staged by day, 2.4; else from its step 0), HRA2, HRA3, HRA4, HRA5, then WBP2V2 (the rule below); each item: its steps up to its READY line, its measured part, its post (HRA: steps 9 and 10, with the reboot back to Debian; WBP2V2: step 7); then the restore | 07:25Z |
| U | all three | 07:30Z | unmask the timers (6.7 of TASK-1121-NIGHT, with the substitutions), nothing else; then section 6 | - |

Caps (minutes) for 4.3's start rule; prep is the steps up to the task's READY line, measured from GO to the end of its measured steps, post the rest of the task, its report included:

| item | task file (sha256 starts with) | prep | measured | post | prep / measured / post steps |
|---|---|---|---|---|---|
| HRA1 | task-1121-hra1.md (de8ee6b6ae5fa47c) | 40 (0 at its READY line) | 85 | 25 (with the reboot) | 0-6 / 7-8 / 9-10 |
| HRA2 | task-1121-hra2.md (54f9cc559242d29a) | 40 | 85 | 25 (with the reboot) | 0-6 / 7-8 / 9-10 |
| HRA3 | task-1121-hra3.md (a8140b305c42f458) | 40 | 85 | 25 (with the reboot) | 0-6 / 7-8 / 9-10 |
| HRA4 | task-1121-hra4.md (c27cbd4de497e01b) | 40 | 85 | 25 (with the reboot) | 0-6 / 7-8 / 9-10 |
| HRA5 | task-1121-hra5.md (b9d1a9ffa284aa31) | 40 | 85 | 25 (with the reboot) | 0-6 / 7-8 / 9-10 |
| WBP2V2 | task-1121-wbp2v2.md (821e8f32058217ca) | 45 | 90 | 15 | 0-4 / 5-6 / 7 |
| restore | TASK-1121-NIGHT 6.1 to 6.6 | - | - | 35 | - |

TASK-1121-HRA6 (`task-1121-hra6.md`, sha256 starts with 2ceb85f4f1f6831a) is published but is not an item tonight: after five passes it could not begin by 04:20Z.

**The start rule in N1** is 4.3 of TASK-1121-NIGHT with one change: an HRA item that the rule skips also skips the HRA items after it, but not WBP2V2, which gets its own check. With HE 07:25Z, an item's prep may begin at the latest at 04:20Z (HRA1 from step 0, HRA2 to HRA5, WBP2V2), and a measured part at the latest at 05:00Z (HRA, HRA1 at its READY line included) or 05:05Z (WBP2V2). Also skip the remaining HRA items, and go on with WBP2V2's check, if an HRA item stops on a disk check whose `err` or `rej` is above 0 (the emulated disks' store full: the next passes run the same arms).

**Each HRA item ends on Debian 6.12**, by day or in N1. If the start rule refuses an HRA item's measured part at its READY line, run its step 10 as on `ABORT` (nothing measured: teardown, the reboot back to Debian, its report), then the restore. If an HRA item stops after its 7.0 boot (step 3), handle the stop as the task says (capture, teardown, or its reboot to clear a suspended pool); then, if the VM is still on 7.0.14-25-pve with no zfs, spl or ublk_drv module loaded, do step 10's plain reboot to the default Debian entry (its second pre-approved reboot) before its report, so that the next item's step 0 finds 6.12. A stop of 5.3 of TASK-1121-NIGHT, or a module that teardown did not unload, gets no reboot of yours: report, and the restore's 6.1 as a report only.

**GO (deferred, given here):** `GO TASK-1121-HRA1`, `GO TASK-1121-HRA2`, `GO TASK-1121-HRA3`, `GO TASK-1121-HRA4`, `GO TASK-1121-HRA5`, `GO TASK-1121-WBP2V2`, each valid only inside N1 and when 4.3's start rule allows the measured part. None is valid by day: HRA1, staged at its READY line, waits for N1.

**Between items.** An HRA item ends with its step 10 back on Debian 6.12 and its final report; the next item begins at its own step 0, whose checks decide whether the state the previous one left allows it (HRA2 to HRA5 expect `/var/tmp/hra-trees` and the earlier passes' log directories; WBP2V2 expects no module, no null_blk device and no `/var/tmp/file-vdev*` or `/var/tmp/dev-*`). Where a task tells you to end your turn (its READY line, its final report), do it as 4.5 says: a one-shot scheduled task of your own session 2 minutes ahead (`NIGHT-1123 continue N1`), then 4.3.

**Reboots.** Each HRA task has two pre-approved reboots (into 7.0.14-25-pve at its step 3, back to the default Debian entry at its step 10). TASK-1123-HRAS took this route on VM 105 on 23/11 with the default pinned to 6.12.118 (`grub-reboot` to the 7.0.14-25-pve entry, back by a plain reboot). Before each, a one-shot scheduled task of your own session 5 minutes ahead (`NIGHT-1123 continue N1`), and `sync -f` of progress.md; after each, `uname -r` (the kernel the step expects), 4.2's checks (the timers stay masked), then the task goes on. The default entry is 6.12.118 (6.12.117 is kept); WBP2V2 builds for the running kernel, whose headers are installed on VM 105. WBP2V2 has no pre-approved reboot: a kernel line in it is 5.2 of TASK-1121-NIGHT.

A task not at the state its step 0 expects (a stop by day, a leftover): `<UTC> N1 <item> NOT READY (<state>)`, run nothing of it, and go on with the next item's check.

**After N1.** Once VM 105 has written its RESTORED line, nothing of this file runs before 07:30Z. Main may then send VM 106 or VM 107 its `DAY NOW` message early: it starts that VM's Tuesday queue at once, functional items only, alone on the host until 07:30Z; for that VM, the message ends this file's "run nothing" from the moment it arrives (its U at 07:30Z still unmasks its timers).

## 2. Setup, now (by day, between two steps of your current task; about 5 minutes)

2.1 Read this file in full, and the sections of TASK-1121-NIGHT it names. Then:
```
N=/var/log/night-1123; sudo -n mkdir -p $N && sudo -n chown "$(id -un)" $N && cp /var/tmp/tasks-1123-night/task-1123-night.md /var/tmp/tasks-1123-night/task-1121-night.md $N/ && sha256sum $N/task-1123-night.md $N/task-1121-night.md
echo "$(date -u +%FT%TZ) setup start $(hostname) $(uname -r)" | tee -a /var/log/night-1123/progress.md
```

2.2 Your wake-ups, one-shot scheduled tasks of your own session, each text followed by `(VM <your id>): read /var/log/night-1123/task-1123-night.md and progress.md, then act.`, their ids and times in `/var/log/night-1123/progress.md`: on every VM, `NIGHT-1123 mask` at 21:50:00Z (2.3) and `NIGHT-1123 U` at 07:30:00Z on 24/11; on VM 105 also `NIGHT-1123 stage HRA1` at 21:00:00Z (2.4), and `NIGHT-1123 wake-up N1` at 22:00:00Z and 22:04:00Z.

2.3 At 21:50Z (your `NIGHT-1123 mask` wake-up), with nothing of yours running: 3.3 of TASK-1121-NIGHT (mask the timers that exist, after waiting for any apt/dpkg as it says), with `night-1123`. A VM at a READY line stays as its task left it: masking timers does not touch it.

2.4 VM 105 only, HRA1 staged by day. At 21:00Z (your `NIGHT-1123 stage HRA1` wake-up), or earlier if main's message says `STAGE HRA1 NOW`, with no task of yours running: run TASK-1121-HRA1's steps 0 to 6 (functional) and stop at its READY line. Its step 0 checks that the VM is clean (booted on 6.12, no zfs, spl, ublk_drv or null_blk module, its first attempt's directory and the trees as expected). If a day task of yours is still running at 21:00Z, begin when it ends, but never after 21:10Z (its prep cap, 40 minutes, ends by the 21:50Z idle time); if you cannot begin by 21:10Z, do not begin, and N1 runs HRA1 from step 0. From its READY line on, this file's replacements apply to it (TASK-1121-NIGHT section 2 (a) to (d)): your night wake-ups stay pending, and its GO is N1's.

2.5 Reply with one line: the ids and times of your wake-ups, and on VM 105 the state of HRA1 (staging at 21:00Z, READY, or not begun and why). Then go on with your day queue.

## 3. Until 21:50Z, then your window

**By day, until 21:50Z.** Each VM finishes its day queue (the tasks main has sent it, and any it sends later) under this start rule: a functional task begins only if now + its cap is at or before your deadline, 21:50Z on VMs 106 and 107, 21:00Z on VM 105 (its HRA1 staging, 2.4); a task's cap is its own "at most", else 1.5 times its "about". A task the rule refuses does not begin: write `<UTC> DAY <task> SKIPPED (start rule: now <time>, cap <min> min, deadline <time>)` in progress.md, say so in one line, and begin no other day task until main writes (on VM 105 the HRA1 staging still begins as 2.4 says). A task still running at 21:45Z although the rule let it begin: finish only the step in progress if it is not a build or a test run; otherwise stop it the way the task handles a hang (capture, then teardown), write `stopped at the idle period`, and report. Each task's own rules apply in full (its reboots: only the ones it pre-approves).

**From 21:50Z.** VM 105: as section 4 of TASK-1121-NIGHT, with section 1's start rule. VMs 106 and 107 have no window tonight: from 21:50Z to U they run nothing (no build, test, fetch, apt, fstrim, reboot, module load or unload); a wake-up gets one line ("no window tonight"). After VM 105's restore (6.1 to 6.6) and before 07:30Z: one line per wake-up ("outside my window"), nothing else. Its final report (6.6) ends with `NIGHT-1123 VM 105 RESTORED <UTC>`.

Restore notes for VM 105: 6.1's expected state is the Debian 6.12.118 kernel; `/var/tmp/hra-trees` kept (HRA6 does not run tonight; say its size); `/var/log/task-1121-hra1-1122` (HRA1's first attempt), `/var/log/task-1123-hras` and every `/var/log/task-1121-hra?` kept; nothing of WBP2V2 left in `/var/tmp`. 6.3 (apt) as written; 6.4 is skipped (the two udev rules are installed on VM 105 since 22/11).

## 4. Stops and messages

As section 5 of TASK-1121-NIGHT, and its section 2 for main's messages (`HOLD`, `ABORT <task>`, `START N1 NOW`, `RESTORE NOW`). A stopped item is over; the next item begins with its own step 0 (4.3), except as section 1 says for a full store, and after section 1's "Each HRA item ends on Debian 6.12". 5.4 (a second stop ends the window's items) applies. A kernel line: capture as 5.2 says, then reboot only as it allows (an HRA item's reboot to clear a suspended pool is its own, pre-approved).

## 5. Report

VM 105's restore report (6.6) is its report for the night: per item its begin, READY, GO, MEASURED and end times, the verdict or result line of its report and the report path; what the start rule skipped (with its line), what stopped and why; the reboots with their UTC times.

## 6. Tuesday 24/11, by day (from 07:30Z)

After your U line (07:30Z), wait for main's `DAY NOW` message: it gives your Tuesday queue (task files, their sha256 and caps) and the day's rules. Until it arrives, run nothing of your own; a wake-up gets one line ("waiting for DAY NOW"). A VM that main has already started on its Tuesday queue after VM 105's restore (section 1, "After N1") goes on with that queue.

All task files are on https://github.com/tigerblue77/zfs branch `vm-tasks-1118` (fetch its current head; check each sha256 before reading; a mismatch is a stop).
