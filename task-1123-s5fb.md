TASK-1123-S5FB (from the coordinator session "main"): functional, about 45 min with its builds, at most 1 h 15, no reboot expected. It runs on VM 105, 106 or 107 (Debian 13, kernel 6.12), not next to a performance run. It covers step 5 of the SPA lock series ("Stop autotrim and rebuild threads before the namespace lock") on wrouesnel's zinject framework: its own head, and the same head without its fix, so that its test `vdev_change_thread_wait` is shown to fail without the fix. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-s5fb/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `U`).

## What is tested (fork https://github.com/tigerblue77/zfs)

| tree | branch | head | tree hash | parent |
|---|---|---|---|---|
| zd | spa-lock-detangle-3-wr | c763dacf340d05230c1e328cf31a9f7c9a225a6a | 77b83449960f87db6805296ac1da618a85090dfb | 5c40f9b9a86535259a8f1ab03d5a42e5f5659e5a (step 4) |
| zf | spa-lock-detangle-3-wr-fb | 1ba61a0d4b73e45cc9d30eb8edb743995d533a54 | a3fb5c5f8c2de43be59501be875c0e7795c3e139 | c763dacf340d05230c1e328cf31a9f7c9a225a6a (zd) |

- zd's parent chain reaches `189a309d464ca51c74d627fdbdd84b5992eade2d` (wrouesnel's framework) after 2 commits.
- zf is zd with `module/zfs/spa_misc.c` taken back to step 4: one file changed, `5 insertions(+), 14 deletions(-)`. Its points and test are zd's.
- The test `tests/zfs-tests/tests/functional/replacement/vdev_change_thread_wait.ksh` has mode `100755` and sha256 `8489946c995ef37ce44e57cdaa93d0eb2ef8a9d95017762616baac660147511d` in both trees.

## VM rules

These are the rules of TASK-1123-MSD (task-1123-msd.md on the same branch), with these additions:

- The test holds a thread at a zinject point and removes its handler by its ID in its cleanup. A run that leaves a handler listed, or a thread with `zio_inject_pause` in its stack, is a stop. Capture as MSD's hang rule says, change nothing, and report.
- On zf, `blocked for more than` lines are possible while the vdev change waits: quote them; they are not a stop.

## Step 0: state

As in MSD's step 0, with these names: `/var/tmp/task-1123-s5fb` and `/var/log/task-1123-s5fb` must both be absent.

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-s5fb; L=/var/log/task-1123-s5fb; U=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
for t in "zd spa-lock-detangle-3-wr 2" "zf spa-lock-detangle-3-wr-fb 3"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 5 $U $2 && git checkout -q FETCH_HEAD &&
  echo "$1 $(git rev-parse HEAD) $(git write-tree) $(git rev-parse HEAD^) $(git rev-parse HEAD~$3) $(git status --porcelain | wc -l)"
done | tee $L/trees.txt
for n in zd zf; do (cd $D/$n && p=tests/zfs-tests/tests/functional/replacement/vdev_change_thread_wait.ksh && echo "$n $(git ls-tree HEAD $p | cut -c1-6) $(sha256sum < $p | cut -c1-64)"); done
cd $D/zf && git diff --stat HEAD^ HEAD | tail -2
```

Expected:

```
zd c763dacf340d05230c1e328cf31a9f7c9a225a6a 77b83449960f87db6805296ac1da618a85090dfb 5c40f9b9a86535259a8f1ab03d5a42e5f5659e5a 189a309d464ca51c74d627fdbdd84b5992eade2d 0
zf 1ba61a0d4b73e45cc9d30eb8edb743995d533a54 a3fb5c5f8c2de43be59501be875c0e7795c3e139 c763dacf340d05230c1e328cf31a9f7c9a225a6a 189a309d464ca51c74d627fdbdd84b5992eade2d 0
```

Then two lines with `100755` and the sha256 above, and the diffstat ` module/zfs/spa_misc.c | 19 +++++--------------` / ` 1 file changed, 5 insertions(+), 14 deletions(-)`.

If the fetch fails, wait 1 minute and try once more. Any other difference is a stop.

## Step 2: the two debug builds, at once

As MSD's step 2, with this task's `D` and `L`. Also quote `grep -H 'injection points will be built' $L/build-z?.log`: both lines must end with `yes`.

## Step 3: zd, expected PASS

Load zd as MSD's step 3 says, with the mark `task-1123-s5fb: zd loaded`. Then:
1. `vdev_change_thread_wait`, 3 runs (tag `zd-thread`, `-t vdev_change_thread_wait`). Expected: PASS each time. Quote its two `the vdev change returned 0` notes.
2. The `replacement` group once (tag `replacement`, `-T replacement`). Expected: all PASS.

Then:
- no pool and no handler left;
- the kernel-line check since the mark;
- unload zd.

## Step 4: zf, expected FAIL

Load zf, with the mark `task-1123-s5fb: zf loaded`. Run `vdev_change_thread_wait` once (tag `zf-thread`).

Expected: FAIL, because the vdev change waits for the held thread while it holds `spa_namespace_lock`. Expect a `log_fail` line of the test, such as `... still running after 30s`, or `the vdev change still running 30s after ...`. Quote it, with the 20 lines before it.

After the run:
- the cleanup must have removed the handlers: `zinject` says `No handlers registered.`;
- no pool left: `timeout 30 sudo -n $D/zf/zpool list -H -o name`;
- quote the kernel-line check since the mark;
- unload zf; `lsmod` must be empty. If the unload fails, capture as for a hang, report, and do not reboot.

A run that passes, or fails elsewhere, is not the expected fail-before: quote it and say so.

Then: `sudo -n rm -rf /var/tmp/task-1123-s5fb`, `sudo -n fstrim -av`, `df -h /var/tmp`.

## Report

As MSD's report, for these trees and tags. One verdict line, for example: "step 5 (zd): vdev_change_thread_wait PASS 3/3, replacement all PASS; without the fix (zf): FAIL at '<line>'".
