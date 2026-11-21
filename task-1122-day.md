TASK-1122-DAY (from the coordinator session "main"): the schedule of the three test VMs for the day of Sunday 22 November 2026, from 09:00Z to 21:50Z. Main may not be awake when it starts, so each VM runs its own queue, wakes itself, and never checks the other VMs. The night file (task-1121-night.md) still governs your night window and the U step at 10:05Z; this file governs everything else on 22/11 by day.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat. Every note for main is the end of a turn in your own chat.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

## 1. The owner's new schedule rule (21/11 21:2xZ)

Day mode, with the three VMs working in parallel on functional tasks, runs until 22:00Z (23:00 for the owner). Night mode (one VM at a time, measurements alone on the host) runs from 22:00Z until 07:30Z on weekdays and until 09:00Z on Saturdays and Sundays. 22/11 is a Sunday: the day starts at 09:00Z.

Consequence for VM 105's night window S3 tonight: AR3's measured part must end by 09:00Z. In the night file's start rule (4.3) for AR3's measured part only, read HE as 09:00Z + post + 35 (that is, the measured part may begin only if now + measured ≤ 09:00Z). Everything after it (AR3's post steps, its END reboot, the restore) is functional and may go on past 09:00Z up to the night file's HE.

## 2. The day (UTC)

| VM | from | queue, in this order | must be idle |
|---|---|---|---|
| 106 | 09:00Z | morning: SPM, then TSN. Afternoon, from 13:05Z: SAT, ZV510C | 11:45Z-13:05Z, and from 21:50Z |
| 107 | 09:00Z | morning: ROP, then CLR. Afternoon, from 13:05Z: L2D, ZLT, L2R, ZOT | 11:45Z-13:05Z, and from 21:50Z |
| 105 | after your U step (10:05Z) | HRIG3 (its prep up to READY; its measured part at 11:45Z, see 4.4); then, from the end of HRIG3 (Debian booted again): B24, then PRB | from 21:50Z |

The window 11:45Z-13:05Z belongs to VM 105's measured part of HRIG3, which runs alone on the host: VMs 106 and 107 run nothing then (no build, test, fio, git fetch, apt, fstrim, reboot, module load or unload). Waiting idle is allowed.

Task files, all on https://github.com/tigerblue77/zfs branch `vm-tasks-1118` (fetch the commit main's message names, check each sha256 before reading it; a mismatch is a stop):

| task | file | sha256 | caps used by the start rule |
|---|---|---|---|
| SAT | task-1121-sat.md | 345afaa093c084408ba1cc5149cb9b8af060a35ac7f1b42aa49c07e2b410db66 | 205 min |
| L2D | task-1121-l2d.md | ff0fc3e4edd752862a8829d0a027465f99d7ff76181f00a68d2ee870a92d78f7 | 160 min |
| ZV510C | task-1121-zv510c.md | cf7b27b90b1e394e498cb02cc0457a5b08bbd7a9848a863f2dba7e76d31f13aa | 285 min |
| ROP | task-1121-rop.md | 8ae8a9a59ddb0abc0afd978c704400dcda6d31ed78f901b5d8d15129a2f1765a | 115 min |
| PRB | task-1121-prb.md | 3e210e4ea6e16ecdb46090bb8d0b074bb20506f1634d647d2d87dfd9432c8a87 | 135 min |
| SPM | task-1121-spm.md | fd050f8c103c7f47e849b55ee4b6148045212d954c242c1fdd346b7e8cdc7597 | 150 min |
| ZLT | task-1121-zlt.md | 87f6f10722758338a79f72940e36542de690e64bdd185fb88fb70a745bc6e0f6 | 115 min |
| ZOT | task-1121-zot.md | f75a3c387d3d0bd0faf0a2a9dc5bf2c909dd615b2e62ad99aa056158d6bb397f | 75 min |
| CLR | task-1121-clr.md | 68ee5537088e228bcfbd81ec06841d44db9fcad297925f533c1c44e773f95db4 | 70 min |
| L2R | task-1121-l2r.md | 61f9f1debc8279dd91d33cf7ef0a44fcc18bdbaa6e3571a7d3d7f670c1f04473 | 115 min |
| TSN | task-1121-tsn.md | e377945d1868c43cd5c5cf71f72418da5df4afa42633828d7b0e33193ab7cd25 | 45 min |
| B24 | task-1121-b24.md | e51cdb925a384091b7eb9a790968aaa13333bbc1824fab8d1fb86e66ab7f1984 | 120 min |
| HRIG3 | task-1122-hrig3.md | 049e61f11d88704710b00b033657020f74ad4c0a241b382d76575c448814cc63 | its own caps |

A cap is the task's own "at most" where it states one, otherwise one and a half times its "about".

## 3. Rules for the day

- **The start rule.** A task may begin only if now + its cap ≤ the start of your next "must be idle" period (11:45Z in the morning, 21:50Z in the afternoon). If it may not, it does not begin: write `<UTC> DAY <task> SKIPPED (start rule: now <time>, cap <min> min, deadline <time>)` in progress.md and try the next task of the same part of the day; a task skipped in the morning is not moved to the afternoon.
- **The idle periods are hard.** If a task is still running at 11:40Z (106, 107) or at 21:45Z (all three) although the start rule let it begin, finish only the step in progress if it is not a build or a test run; otherwise stop it the way the task handles a hang (capture, then teardown), write `stopped at the idle period`, and report. Then wait for the period's end (11:45Z-13:05Z) or stop for the day (21:50Z).
- **Each task's own rules apply in full** (its VM rules, its stops, its reboots: only the ones it pre-approves). A stop ends that task: report it, then go on with the next task of your queue only if the stop left the VM in the state the next task's step 0 accepts; otherwise wait for main.
- **"Stay idle until main writes" in a task file** is replaced today by: after the final report, go on with your queue (4).
- **You never check or contact the other VMs.** A message from main wins over this file: `HOLD` (start nothing new; finish the running step only as far as its task allows, tear down as the task says, wait), `ABORT <task>` (that task's abort path), or new instructions.
- **progress.md:** keep using `/var/log/night-1121/progress.md`, one line per event, `<UTC> DAY <task> <event>`, `sync -f` after each.
- The timers the night masked stay masked until your U step at 10:05Z unmasks them, as the night file says. Running day tasks before 10:05Z with them masked is fine.

## 4. Turns and wake-ups

4.1 Now (when main's message gives you this file): read it in full, then create one-shot scheduled tasks of your own session (the kind you use to resume after a reboot), text `DAY-1122 wake-up (VM <yours>)`: VMs 106 and 107 at 2026-11-22T09:00:00Z and 09:04:00Z, and at 13:05:00Z and 13:09:00Z; VM 105 nothing new (its 10:12Z HRIG3 wake-up stays). Write their ids in progress.md, reply with one line listing them, and end your turn.

4.2 At a wake-up: go on from where progress.md stands. A wake-up that arrives while you already work gets one line. Before 09:00Z (106, 107) or inside 11:45Z-13:05Z (106, 107), or after 21:50Z: one line ("not in my day period"), nothing else.

4.3 Where a task tells you to end your turn (its final report, or a READY line), first create a one-shot scheduled task of your own session 2 minutes ahead, text `DAY-1122 continue (VM <yours>)`, then end the turn as the task says. Before a build or any step expected to run more than 10 minutes in background mode, create a one-shot scheduled task at its expected end plus 15 minutes.

4.4 VM 105, HRIG3: run it from your 10:12Z wake-up up to its READY line, as main's earlier message said, once your U step is done. **Main's GO for its measured part is given here, deferred:** at its READY line, if it is 11:45Z or later and not later than 11:50Z, write `<UTC> DAY HRIG3 GO (day schedule)` in progress.md and begin the measured part; if READY comes before 11:45Z, wait idle until 11:45Z (create a wake-up at 11:45:00Z and end your turn as the READY line says), then begin; if READY comes after 11:50Z, do not begin: wait for main's plain `GO TASK-1122-HRIG3`. Then the task's post steps, its reboot back into Debian, its final report, then B24 and PRB under the start rule.

4.5 At 21:50Z every VM is idle for the night that main's next night file will describe. If main has sent no night file by then, stay idle, run nothing, and keep your night's masked timers as the U step left them.

## 5. Report

After 21:50Z, or when your queue is done, one message: a table `task | start | end | verdict line | stops or skips`, the ids of the wake-ups still pending, and the state left on the VM (modules, pools, trees, /etc/zfs/zpool.cache).
