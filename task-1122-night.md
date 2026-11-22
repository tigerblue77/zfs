TASK-1122-NIGHT (from the coordinator session "main"): the night of Sunday 22 to Monday 23 November 2026, 22:00Z to 07:30Z, for the three test VMs. Night mode (the owner's rule): one VM works at a time, and each measured part runs alone on the host. Main may not be awake: each VM runs its own window, wakes itself, and never checks or contacts the other VMs. Then Monday's day queue (section 6).

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters.

**This file reuses TASK-1121-NIGHT** (`task-1121-night.md`, same branch `vm-tasks-1118`; you read it on 21/11) for its mechanics, with these substitutions everywhere: `night-1121` -> `night-1122`, `NIGHT-1121` -> `NIGHT-1122`, `/var/tmp/tasks-1121-night` -> `/var/tmp/tasks-1122-night`, and its U time `10:05Z` -> `07:30Z`. From it apply: section 2 (rules for the whole night), 3.3 only (mask the four timers, logging `setup masked <timer>` in progress.md), section 4 (running your window: wake-ups, 4.3's start rule with the caps below and your HE, GO, turns, long steps, the hard end), section 5 (stops) and section 6 (restore 6.1 to 6.6, then U 6.7). Nothing else of it (its setup steps 3.1, 3.2, 3.4 to 3.6, its windows and its per-VM section do not apply).

## 1. The night (UTC)

| Window | VM | wake-up | items, in this order | HE |
|---|---|---|---|---|
| N1 | 107 | 21:58Z (and 22:02Z) | X5's measured part (step 5, READY since 22/11 17:10Z), its post (step 6, report), restore | 00:25Z |
| N2 | 105 | 00:25Z (and 00:29Z) | HRA1's measured part (steps 7 and 8; READY expected by 19:50Z), its post (steps 9 and 10, the reboot back to Debian, report), restore | 02:50Z |
| N3 | 106 | 02:50Z (and 02:54Z) | F27's measured part (READY expected by 19:00Z), its post and report, restore | 05:15Z |
| U | all three | 07:30Z | unmask the timers (6.7 of TASK-1121-NIGHT, with the substitutions), nothing else | - |

Caps (minutes) for 4.3's start rule (prep is 0: all three tasks are at their READY line):

| item | task file (sha256 prefix) | prep | measured | post |
|---|---|---|---|---|
| X5 | task-1122-x5.md (7a53d768934469b6) | 0 | 90 | 15 |
| HRA1 | task-1121-hra1.md (03320be53c759989) | 0 | 85 | 25 (with the reboot) |
| F27 | task-1122-f27.md (77cdef9c8ec7cce3) | 0 | 90 | 15 |

**GO (deferred, given here):** `GO TASK-1122-X5`, `GO TASK-1121-HRA1`, `GO TASK-1122-F27`, each valid only inside its own window and when 4.3's start rule allows the measured part.

A task not at its READY line when its window starts (a restart, a stop by day): write `<UTC> <window> <item> NOT READY (<state>)`, run nothing of it, and go to the restore.

## 2. Setup, now (by day, within the day file's rules; about 5 minutes)

2.1 Read this file in full; `mkdir -p /var/log/night-1122`; copy this file there.
2.2 Create your two night wake-ups (one-shot scheduled tasks of your own session, text `NIGHT-1122 wake-up <window>`) at your window's times of section 1, and one for U at 07:30:00Z (text `NIGHT-1122 U`). Write their ids in `/var/log/night-1122/progress.md`.
2.3 At 21:50Z (the day file's idle time; create a one-shot at 21:50:00Z, text `NIGHT-1122 mask`), with nothing of yours running: 3.3 of TASK-1121-NIGHT (mask the timers that exist, after waiting for any apt/dpkg as it says), with `night-1122`. A VM at a READY line stays as its task left it: masking timers does not touch it.
2.4 Reply with one line: the ids and times of your wake-ups, and the state of your staged task (READY, or what it is doing and when it expects READY).

## 3. Your window

As section 4 of TASK-1121-NIGHT. Before your window you run nothing (a READY rig stays as it is). After your restore (6.1 to 6.6) and before 07:30Z: one line per wake-up ("outside my window"), nothing else. Your final report (6.6) ends with `NIGHT-1122 VM <id> RESTORED <UTC>`.

Restore notes: 6.1's expected state on VM 105 after HRA1 is the Debian 6.12 kernel with HRA1's kept trees under `/var/tmp/hra-trees` (they serve the next passes: leave them, say their size); 6.3 (apt) and 6.4 (the two udev rules, skipped if they exist) as written.

## 4. Stops and messages

As section 5 of TASK-1121-NIGHT, and its section 2 for main's messages (`HOLD`, `ABORT <task>`, `START <window> NOW`, `RESTORE NOW`). A kernel line tonight: capture as 5.2 says, then reboot only if the task pre-approves one; otherwise wait for main.

## 5. Report

Your restore's final report (6.6) is your report for the night.

## 6. Monday 23/11, by day (07:30Z to 21:50Z)

After your U line (07:30Z), each VM runs its queue in this order, under the rules of `task-1122-day.md` section 3 (the start rule against 21:50Z; caps = the task's own "at most", else 1.5 x its "about"; each task's own rules in full; "stay idle until main writes" in a task file is replaced by: go on with your queue). No "must be idle" period by day on Monday except from 21:50Z. Create a one-shot `DAY-1123 continue (VM <yours>)` wake-up 2 minutes ahead wherever a task tells you to end your turn, and one at the expected end + 15 min before any step longer than 10 minutes, as day file 4.3 says.

| VM | queue | caps (min) |
|---|---|---|
| 105 | SLD6B (`task-1122-sld6b.md`, sha256 6198e08867883c4cc0acb86a529d13be52033cfc2664f8cea06060987db61b3e) | 540 |
| 106 | SLD5B (`task-1122-sld5b.md`, sha256 ea11b3705a6d5bd06a0216425d9166f6c676921a371e859fd790c0028b2a2ffe) | 540 |
| 107 | L2D2 (`task-1122-l2d2.md`, sha256 52fcb917b5e34a2ad3273c9d4ba962c7be783b0a0ceed9c33464d272cb8489e6), then IRD v2 (`task-1122-ird-v2.md`, sha256 f557b1859d41ad5f4aae56ea757834242126ced46f7dddb96d627f196e145cda) | 180, 315 |

All task files are on https://github.com/tigerblue77/zfs branch `vm-tasks-1118` (fetch its current head; check each sha256 before reading; a mismatch is a stop). After your queue: one report message, as day file section 5 says, then stay idle: main will send the next night's file.
