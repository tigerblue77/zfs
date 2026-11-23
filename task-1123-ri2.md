TASK-1123-RI2 (from the coordinator session "main"): functional, about 45 min with its builds, at most 1 h 15, no reboot expected and none pre-approved. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It replaces item ri of TASK-1123-FBWR part B: the regression test of upstream PR 19705, ported to wrouesnel's zinject framework, did not fail cleanly without the fix there. It deadlocked on its own held read: the expansion finished, its thread waited for the read in `vdev_online()` holding `SCL_STATE` as a writer, and the test's progress poll (`zpool status -jp`) waited behind it, so the test never released the read. The test now reads the progress from the uberblocks on a disk (`zdb -lu`), runs every command that can wait for the pool while a thread is held in the background with a 30 s bound, releases the read even if the handler listing has not returned, and removes its handlers by their ID before its cleanup waits for anything. Here it runs 3 times with the fix (expected PASS, as in TASK-1122-WRS) and once without it (expected FAIL at its checksum check, within a bound, with nothing left held). Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-ri2/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`, `F`).

## What is tested (fork https://github.com/tigerblue77/zfs)

| tag | branch | head | tree hash | HEAD^ | n | build |
|---|---|---|---|---|---|---|
| rf | retro-19705-test-v2-wr | ebb5882dc419fb96402d8c67028d5a11d9944dc5 | 2cc3a6faee39f0e2e86c41f45b63557b5c4fce75 | 189a309d464ca51c74d627fdbdd84b5992eade2d | 1 | debug |
| rb | retro-19705-test-v2-wr-fb | a98c3cd4ab3842bf385cbc7f64873f8f0f181eba | 43f466aa0fe6700446e680f4977141a1e153d22a | ebb5882dc419fb96402d8c67028d5a11d9944dc5 (rf) | 2 | debug |

- `n` is the number of commits from the head down to `189a309d464` (wrouesnel's `zinject-points-uplift`, on upstream master `a1daebfec3f`).
- rf is one commit: the two points (`raidz_reflow_sync:committed`, `vdev_raidz_io_start:copied`) and the test. It replaces `0d7d8cdfe2e`, the head that TASK-1122-WRS and FBWR ran: only the test file (`144 insertions(+), 28 deletions(-)`) and the commit message differ.
- rb is rf plus one commit that reverts upstream's fix (`c9e0d4c8934`) in `module/zfs/spa.c`, byte for byte the revert of FBWR's ri tree (`7e919c93e3d`).
- The test `tests/zfs-tests/tests/functional/raidz/raidz_expand_inflight_read.ksh` has mode `100755` and sha256 `1ff8bb4491d291183a6e73bac3149e06edb6474b2812555eb7010da5915b60ba` in both trees.

## VM rules

These are the VM rules of TASK-1123-FBWR (task-1123-fbwr.md on the same branch), with `<tag>` rf or rb, and these changes:

- **No reboot is pre-approved.** A hang, a held thread, a pool or handler left, a task in D state, or a module that will not unload is a stop: capture as FBWR's hang rule says (into `$L/hang-<tag>.txt`), change nothing, and report.
- **Hangs:** builds as in FBWR. A single-test run still going 5 minutes after it started is a hang (the runner itself would only kill it at 10 minutes).
- **D state:** after each run, `ps -eo pid,stat,comm,wchan:32 | awk '$2 ~ /^D/'` must show no `zpool`, `zfs`, `zinject`, `dd`, `ksh`, `txg_sync` or `raidz_expand` task.
- **Kernel log:** after each step, the FBWR pattern since that step's mark. None expected, on rb included.
- No `sysctl` change is needed: quote `kernel.hung_task_panic` and `kernel.panic_on_oops` in step 0 only.

## Step 0: state

As FBWR's step 0, with these names: `/var/tmp/task-1123-ri2` and `/var/log/task-1123-ri2` must both be absent. Also quote `ls -d /var/tmp/raidz_expand_inflight_read* 2>&1` (absent expected).

If FBWR's ri run left anything (a module loaded, `testpool` imported, a handler, a task in D state, `/var/tmp/raidz_expand_inflight_read*`, `/var/tmp/tunable-*`), stop and report it: main decides how to clear it.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-ri2; L=/var/log/task-1123-ri2; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
echo "$(date -u +%FT%TZ) start $(hostname) $(uname -r)" >> $L/progress.md
for t in "rf retro-19705-test-v2-wr 1" "rb retro-19705-test-v2-wr-fb 2"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 4 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~$3) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
```

Expected:

```
rf ebb5882dc419fb96402d8c67028d5a11d9944dc5 2cc3a6faee39f0e2e86c41f45b63557b5c4fce75 189a309d464ca51c74d627fdbdd84b5992eade2d 189a309d464ca51c74d627fdbdd84b5992eade2d 0
rb a98c3cd4ab3842bf385cbc7f64873f8f0f181eba 43f466aa0fe6700446e680f4977141a1e153d22a ebb5882dc419fb96402d8c67028d5a11d9944dc5 189a309d464ca51c74d627fdbdd84b5992eade2d 0
```

Then:

```sh
D=/var/tmp/task-1123-ri2; F=tests/zfs-tests/tests/functional/raidz/raidz_expand_inflight_read.ksh
for t in "rf 1" "rb 2"; do set -- $t; (cd $D/$1 &&
  echo "$1 $(git ls-tree HEAD $F | cut -c1-6) $(sha256sum < $F | cut -c1-64) $(sha256sum < module/zfs/spa.c | cut -c1-64)$(git diff --stat HEAD~$2 HEAD | tail -1)" &&
  echo "$1 $(grep -c -F 'vre->vre_rangelock, synced_offset' module/zfs/spa.c) $(grep -c -e '"raidz_reflow_sync:committed"' -e '"vdev_raidz_io_start:copied"' module/zfs/vdev_raidz.c)"); done
cd $D/rb && git diff --stat HEAD^ HEAD | tail -1
```

Expected:

```
rf 100755 1ff8bb4491d291183a6e73bac3149e06edb6474b2812555eb7010da5915b60ba 6884528a1b85744188e96ae653c56c13cd7b79a2ade703b116c2d2805e56a823 4 files changed, 375 insertions(+), 1 deletion(-)
rf 1 2
rb 100755 1ff8bb4491d291183a6e73bac3149e06edb6474b2812555eb7010da5915b60ba 993579efc3dcfe69e319fae619cfa4d2e45cf6aec1a764feb4e9402707f05986 5 files changed, 376 insertions(+), 19 deletions(-)
rb 0 2
 1 file changed, 1 insertion(+), 18 deletions(-)
```

rb's `spa.c` sha256 is the one FBWR's step 1 checked for ri. If a fetch fails, wait 1 minute and try once more. Any other difference is a stop: report the outputs as they are. Edit no file.

## Step 2: the two debug builds, at once (about 15 to 25 minutes)

```sh
D=/var/tmp/task-1123-ri2; L=/var/log/task-1123-ri2
for t in rf rb; do
  setsid nohup sh -c "date -u +%T; cd $D/$t && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$t.log 2>&1 < /dev/null &
done
```

Poll `grep -H build-rc= $L/build-r?.log` until there are two lines; each must read `build-rc=0`. Then quote:
- both durations, and every `warning:` line outside objtool notes (none expected);
- `grep -H -E 'assertion support will be enabled|injection points will be built' $L/build-r?.log`: four lines ending in `yes`;
- `strings $D/<tag>/module/zfs.ko | grep -c -x -E 'raidz_reflow_sync:committed|vdev_raidz_io_start:copied'`: 2 for each tree;
- both srcversions (`modinfo -F srcversion $D/<tag>/module/zfs.ko`), which must differ.

## Step 3: rf, the fix, 3 runs, expected PASS

Load rf as FBWR's "Load (every item)" says, with the mark `task-1123-ri2: rf loaded` ("(DEBUG mode)" expected). Then 3 runs, one at a time, each after FBWR's pre-run checks:

```sh
cd /var/tmp/task-1123-ri2/rf
setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t raidz_expand_inflight_read > /var/log/task-1123-ri2/zts-rf-<k>.log 2>&1 < /dev/null &
```

Expected for each run: PASS, in about 30 s (WRS: under a minute). Quote, from the merged log (`R=$(ls -td /var/tmp/test_results/*/ | head -1); find "$R" -name merged`):
- the `Test:` lines with result and duration, and the Results Summary;
- the two notes: `reflow offset <old>, next <new>, block <b> at <xoff> size <xsize>`, then `reflow offset synced on disk: <X> (past the block: <T>)`. With the fix, `X` is expected close to `<new>` and below `T`: the uberblock of the held sync's txg, which records the progress up to about `<new>`, reaches the disk before spa_sync() waits for the read, and the reflow cannot go further until that txg ends. A finding, not a stop, if `X` is `end` or at or above `T` (then the test would have released the read early);
- with their timestamps, the lines `SUCCESS: bounded zinject -c <id>` (two: the sync, then about 20 s later the read) and `SUCCESS: bounded eval zpool status -p ...`;
- every line with `FAIL`, `failed`, `ERROR` or `still running`.

After the 3 runs: no pool, no handler, no held thread, no task in D state, the kernel-line check since the mark, then unload rf (`$D/rf/scripts/zfs.sh -u`; `lsmod` empty).

A FAIL or an UNSUPPORTED here is a finding: copy its merged log to `$L/merged-rf-<k>.txt`, quote the failing line with the 20 lines before it, and go on to step 4 only if nothing is left held, imported or in D state.

## Step 4: rb, without the fix, 1 run, expected FAIL

Load rb as above, with the mark `task-1123-ri2: rb loaded`, write `rb run started` in `$L/progress.md`, then one run after the pre-run checks, with `-D` so that the test's output reaches the log as it runs:

```sh
cd /var/tmp/task-1123-ri2/rb
setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKOD -t raidz_expand_inflight_read > /var/log/task-1123-ri2/zts-rb.log 2>&1 < /dev/null &
```

Poll every 10 s. Expected:
- the note `reflow offset synced on disk: end (past the block: <T>)` (the expansion finished while the read was held), or a number at or above `T`;
- the failure `the read found other data at the old location of the block (<N> checksum errors)`, with `N` above 0 (TASK-1122-R19705 saw 42: 7 on each of the 6 old disks), printed after the `zpool status -v` of the pool;
- the test's FAIL within about 1 minute of its start, and at most about 2 min 40 s if every 30 s bound is used up (the runner's `Test:` duration);
- then its cleanup: `removed all registered handlers` (from `zinject -c all`, run only once nothing is blocked) and `SUCCESS: zpool destroy -f testpool`. No `commands are still blocked: the pool is left` note.

Quote, from `$L/zts-rb.log` and the merged log (copy it to `$L/merged-rb.txt`):
- the failing line with the 20 lines before it;
- the `Test:` line and the Results Summary;
- the two notes above, and with their timestamps `NOTE: reflow offset synced on disk`, `SUCCESS: bounded zinject -c <readid>` and the failing line. A gap of 30 s or more between the first two means the handler listing waited for the pool and the test released the read anyway: say so;
- every line with `still running`, `blocked` or `ERROR`;
- if present in the merged log's dbgmsg dump: the `zinject: handler <id> paused at vdev_raidz_io_start:copied` and `... resumed at ...` lines.

Any other `log_fail` line, a PASS, or an UNSUPPORTED is not the expected fail-before: quote it and say so. The lines that would mean the bounds caught something are `... still running after 30s`, `zinject still running 30s after the release`, `the read did not return in 30s` and `the listing of the zinject handlers did not return in 30s while the read was held`.

Two minutes after the run ends, check and quote:
- no pool: `timeout 30 sudo -n $D/rb/zpool list -H -o name` prints nothing;
- no handler: the first line of `timeout 30 sudo -n $D/rb/zinject` is `No handlers registered.`;
- no held thread (FBWR's check) and no task in D state;
- the kernel-line check since the mark (none expected);
- `ls -d /var/tmp/testdir /var/tmp/raidz_expand_inflight_read* /var/tmp/tunable-* 2>&1`: none.

Then unload rb; `lsmod` must be empty.

If anything is still blocked (a task in D state, a handler, the pool, the run still going at 5 minutes): capture into `$L/hang-rb.txt` as FBWR's hang rule says, plus `sudo -n tail -n 100 /proc/spl/kstat/zfs/dbgmsg`, change nothing, do not reboot, and report.

## The end

- No module is loaded; `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK`.
- `sudo -n rm -rf /var/tmp/task-1123-ri2` (keep it if a stop needs it, and say so), `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

Write `/var/log/task-1123-ri2/report.md` and give the same text as your final message:

1. **Environment:** VM, kernel, nproc, RAM, free space, the step 0 sysctls, `trees.txt` and the step 1 lines, each build's rc and duration with its configure lines and warnings, the point counts, and each srcversion with its load line.
2. **Table:** tag | run | result | duration | `X` and `T` of the synced-offset note | failing line (or "-"). Then one verdict line, for example: "rf PASS 3/3 (X close to new, below T); rb FAIL at the checksum check, 42 checksum errors, in 0:25, offset note 'end', nothing left held".
3. **Quotes:** as listed in steps 3 and 4, and the checks after each step.
4. **Deviations, cleanup,** then `End`.
