# VM: does zpool_error_scrub_import_paused fail without the fix and pass with it?

Disposable Debian 13 VM only: modules are loaded and unloaded, ZTS creates
and destroys pools, file vdevs and loop devices. Post nothing, push nothing.
Run every block as root, in order, in one bash shell. ZTS runs as the
non-root user `zts` (created in step 2 if it does not exist). Builds and
tests run under `nice -n 10 ionice -c3`, with `make -j8`.

Two trees, both `--enable-debug`:

| Tree | Commit | Content |
| --- | --- | --- |
| B | `0b70bdad7b` (full hash checked in step 2) | fork branch `errorscrub-pause-state`: commit 1 (pause time cleared at error scrub setup, test `zpool_error_scrub_cancel_paused`) and commit 2 (`spa_scan_stat_init()` on the no-scan-record path of `dsl_scan_init()`, test `zpool_error_scrub_import_paused`) |
| A | `1a956006ab`, B's parent | commit 1 only, plus the new test file and its one line in `tests/zfs-tests/tests/Makefile.am` and `tests/runfiles/common.run`, taken from B |

So A and B differ only by the 7 lines of `module/zfs/dsl_scan.c`.

## 1. Preconditions

```sh
set -u
[ "$(id -u)" = 0 ] || { echo "STOP: run as root"; exit 1; }
E=/var/tmp/esps; L=/var/tmp/esps-logs; D=/var/tmp/esps-files
TEST=tests/functional/cli_root/zpool_scrub/zpool_error_scrub_import_paused.ksh
TNAME=$(basename "$TEST" .ksh)
uname -r; nproc; df -h /var/tmp | tail -1
lsmod | grep -E '^(zfs|spl) ' && echo "STOP: a zfs module is loaded"
dpkg -l | grep -E '^ii +(zfsutils|zfs-dkms|libzfs|libzpool)' && echo "STOP: distribution ZFS packages are installed"
ls -d "/lib/modules/$(uname -r)/build" || echo "STOP: no kernel headers for the running kernel"
for c in ksh fio git autoconf automake libtool gcc make sudo truncate; do
	command -v $c > /dev/null || echo "MISSING: $c"
done
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|gcc|fio|bench' | grep -v pgrep
mkdir -p "$L" && install -d -m 1777 "$D"
```

STOP on any `STOP` or `MISSING` line, or if another job is running: report it.

## 2. Test user, clone, trees

```sh
id zts > /dev/null 2>&1 || { useradd -m -s /bin/bash zts && echo "zts created"; }
echo "sudo as zts: $(runuser -u zts -- sudo -n id -un)"          # must be root
install -d -o zts -g zts "$E"
runuser -u zts -- git clone https://github.com/tigerblue77/zfs "$E/src" > "$L/clone.log" 2>&1
B=$(runuser -u zts -- git -C "$E/src" rev-parse origin/errorscrub-pause-state)
A=$(runuser -u zts -- git -C "$E/src" rev-parse "$B^")
echo "B=$B A=$A"            # B starts 0b70bdad7b, A starts 1a956006ab
runuser -u zts -- git -C "$E/src" worktree add --detach "$E/B" "$B" > "$L/wt-B.log" 2>&1
runuser -u zts -- git -C "$E/src" worktree add --detach "$E/A" "$A" > "$L/wt-A.log" 2>&1
runuser -u zts -- git -C "$E/A" checkout "$B" -- "tests/zfs-tests/$TEST" \
    tests/zfs-tests/tests/Makefile.am tests/runfiles/common.run
runuser -u zts -- git -C "$E/A" diff --stat "$B"     # only module/zfs/dsl_scan.c
ls -l "$E/A/tests/zfs-tests/$TEST"                   # must be executable
```

STOP if a hash does not start as shown, or if the last `diff --stat` lists
anything but `module/zfs/dsl_scan.c`.

## 3. Build both trees

```sh
for t in B A; do
	runuser -u zts -- nice -n 10 ionice -c3 sh -c \
	    "cd $E/$t && ./autogen.sh && ./configure --enable-debug && make -j8" \
	    > "$L/build-$t.log" 2>&1
	echo "build $t rc=$?"
	echo "$t built srcversion: $(modinfo -F srcversion "$E/$t/module/zfs.ko")"
done
```

The two srcversions must differ. A failed build: STOP and report
`grep -m5 -n 'error:' $L/build-<t>.log`.

## 4. B: the new test, then the whole zpool_scrub group

```sh
"$E/B/scripts/zfs.sh" > "$L/load-B.log" 2>&1; echo "load B rc=$?"
echo "B loaded srcversion: $(cat /sys/module/zfs/srcversion)"
journalctl -k --no-pager | wc -l > "$L/kmsg-start-B"
runuser -u zts -- nice -n 10 timeout -k 60 1500 "$E/B/scripts/zfs-tests.sh" -d "$D" -t "$TEST" \
    > "$L/B-single.log" 2>&1; echo "B single rc=$?"
grep -E "$TNAME.*\[(PASS|FAIL|SKIP|KILLED)\]" "$L/B-single.log"
RB=$(awk '/^Log directory/ {print $3}' "$L/B-single.log")
grep -hnE 'ERROR:|SUCCESS: zpool (scrub|import)' $(find "$RB" -path "*$TNAME*" -name merged)
runuser -u zts -- nice -n 10 timeout -k 60 3600 "$E/B/scripts/zfs-tests.sh" -d "$D" -T zpool_scrub \
    > "$L/B-group.log" 2>&1; echo "B group rc=$?"
sed -n '/^Results Summary/,/^Log directory/p' "$L/B-group.log"
grep -E '\[(FAIL|KILLED|SKIP)\]' "$L/B-group.log"
journalctl -k --no-pager | tail -n +"$(( $(cat "$L/kmsg-start-B") + 1 ))" \
    | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|blocked for more'
"$E/B/scripts/zfs.sh" -u > "$L/unload-B.log" 2>&1; echo "unload B rc=$?"
lsmod | grep -E '^(zfs|spl) ' && echo "STOP: B modules still loaded"
```

The loaded srcversion must equal B's built one. For a `[FAIL]` or
`[KILLED]` test of the group, report its `ERROR:` lines:
`grep -hn 'ERROR:' $(find "$(awk '/^Log directory/ {print $3}' $L/B-group.log)" -path '*<name>*' -name merged)`.

## 5. A: the new test

```sh
"$E/A/scripts/zfs.sh" > "$L/load-A.log" 2>&1; echo "load A rc=$?"
echo "A loaded srcversion: $(cat /sys/module/zfs/srcversion)"
journalctl -k --no-pager | wc -l > "$L/kmsg-start-A"
runuser -u zts -- nice -n 10 timeout -k 60 1500 "$E/A/scripts/zfs-tests.sh" -d "$D" -t "$TEST" \
    > "$L/A-single.log" 2>&1; echo "A single rc=$?"
grep -E "$TNAME.*\[(PASS|FAIL|SKIP|KILLED)\]" "$L/A-single.log"
RA=$(awk '/^Log directory/ {print $3}' "$L/A-single.log")
grep -hnE 'ERROR:|SUCCESS: zpool (scrub|import)' $(find "$RA" -path "*$TNAME*" -name merged)
grep -hn -A3 'zpool status' $(find "$RA" -path "*$TNAME*" -name merged) | grep -E 'scrub:' | tail -3
journalctl -k --no-pager | tail -n +"$(( $(cat "$L/kmsg-start-A") + 1 ))" \
    | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|blocked for more'
"$E/A/scripts/zfs.sh" -u > "$L/unload-A.log" 2>&1; echo "unload A rc=$?"
```

If a `zpool` process is left in `D` state, run nothing else by hand: report
`ps -eo pid,stat,etimes,wchan:32,args | awk 'NR == 1 || $2 ~ /^D/'` and
`cat /proc/<pid>/stack`, then reboot.

## 6. Clean up

```sh
lsmod | grep -qE '^zfs ' && "$E/A/scripts/zfs.sh" -u
losetup -a | grep "$D" | cut -d: -f1 | xargs -r -n1 losetup -d
rm -rf "$E" "$D"
df -h /var/tmp | tail -1
```

Keep `$L`. Remove the `zts` user only if step 2 created it.

## Expected (from reading, not from a kernel run)

| Run | Result | Shown by |
| --- | --- | --- |
| B, new test | PASS | `SUCCESS: zpool import -d ... testpool2`, then no `ERROR:` |
| B, `-T zpool_scrub` | every test PASS (22 in `common.run`, the two new ones included) | `Results Summary` |
| A, new test | FAIL after the second import | `ERROR: is_pool_error_scrubbing testpool2 true unexpectedly exited 0`; `zpool status` shows `error scrub in progress since` |

Anything else is a finding: a FAIL on B, a PASS on A, a FAIL on A at
another line, a loaded srcversion that does not match its tree, a kernel
`VERIFY`/`PANIC`, or a hang.

## Report

Under 40 lines: for each tree, build rc and built/loaded srcversion; the
test result line and the grep output of steps 4 and 5; for B, the group's
`Results Summary` and any non-PASS test with its `ERROR:` lines; kernel log
counts; any deviation from this document.
