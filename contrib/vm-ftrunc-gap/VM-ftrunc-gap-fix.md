# VM: does the fix close the ftruncate/punch logging window?

Disposable VM only, same setup as `findings-a/ftrunc-gap/VM-ftrunc-gap.md`.
"Before" is already measured there (build A = master `0cdf531257` + the
delay patch: gap 10/10 in all three cases at 1000 ms). This adds build C =
master + the fix + the delay patch rebased onto the fix (same two module
parameters, same places; only the setattr hunk's context changed, since the
fix turns `zfs_freesp(..., FALSE)` into `TRUE`).

Files (sha256):

- `0001-Linux-log-size-changes-and-punches-before-dropping-t.patch` 5db16e02b8c81ce7ef46218a03d5f52924a5c730367626691de7ef41e642e00b
- `ftrunc-delay-on-fix.patch` de3a36415741b142bbea4cbdf5c6b9cd906048d25495e6b9140614e5f4c6b48c
- `ftrunc-gap.sh` b7a1f7ab1cab3695f888e00e13d1c2ebe6685d207cdfc7798d24a1c71c81f378
  (the reproducer, plus: with the fix a setattr logs its size as
  TX_TRUNCATE, which it now counts as the size record; unchanged on A)

```sh
set -u
Z=/var/tmp/ftrunc-gap; L=/var/log/ftrunc-gap
lsmod | grep -E '^(zfs|spl) ' && echo "STOP: a zfs module is loaded"
cp 0001-Linux-log-size-changes-and-punches-before-dropping-t.patch \
    ftrunc-delay-on-fix.patch ftrunc-gap.sh $Z/
sha256sum $Z/0001-*.patch $Z/ftrunc-delay-on-fix.patch $Z/ftrunc-gap.sh
git -C $Z/src worktree add --detach $Z/C 0cdf531257e977b2f4b31a5823e7a5907c49d5a2 \
    > $L/wt-C.log 2>&1
git -C $Z/C apply $Z/0001-*.patch && git -C $Z/C apply $Z/ftrunc-delay-on-fix.patch \
    && echo "C patched"
(cd $Z/C && nice -n 10 sh -c './autogen.sh && ./configure --enable-debug &&
    make -j8') > $L/build-C.log 2>&1; echo "build C rc=$?"
sudo -n $Z/C/scripts/zfs.sh > $L/load-C.log 2>&1 || echo "load C failed"
ls /sys/module/zfs/parameters/zfs_setattr_size_delay_ms \
    /sys/module/zfs/parameters/zfs_freesp_log_delay_ms || echo "STOP: not the patched module"
for c in setattr-buffered setattr-direct punch-buffered; do
	for d in 1000 0; do
		sudo -n env PATH="$Z/C:/usr/sbin:/sbin:/usr/bin:/bin" \
		    timeout -k 30 900 bash $Z/ftrunc-gap.sh $c $d 10 \
		    > $L/C-$c-$d.log 2>&1
		echo "C $c $d rc=$? $(grep -h '^RESULT\|SETUP ERROR' $L/C-$c-$d.log)"
	done
done
sudo -n journalctl -k --no-pager | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops|hung_task|blocked for more'
sudo -n $Z/C/scripts/zfs.sh -u > $L/unload-C.log 2>&1; echo "unload C rc=$?"
```

Expected from reading (not run):

| case, delay 1000 | log | write_returned_first | result |
| --- | --- | --- | --- |
| setattr-buffered, setattr-direct | `TW` (TX_TRUNCATE assigned under the lock, the write after it, then TX_SETATTR without ATTR_SIZE) | 1 (the sleep is now outside the lock) | `same=10` |
| punch-buffered | `TW` | 0 (the sleep is now inside the range lock, so the write waits) | `same=10` |

Delay 0 should stay `same=10`. Any `gap`, `other`, a `SETUP ERROR`, a hung
task or a kernel `VERIFY` is a finding: stop and report the round lines.
Also worth one run of upstream's `tests/functional/slog` and `fallocate`
ZTS groups on C (`zfs-tests.sh -T slog,fallocate`), which exercise
TX_TRUNCATE/TX_SETATTR replay.
