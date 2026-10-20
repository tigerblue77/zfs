# Two ZTS reproductions, for disposable VM 107 only

Neither script has been run anywhere. They were checked with `ksh -n` and
kept to 80 columns, nothing more. Both expect a source tree built at
upstream master 0d4bb39c2c (or later) with its modules loaded via
`scripts/zfs.sh`. On master, both are expected to leave ZFS wedged, so
**reboot the VM after each failing run**, and run them one at a time.

| Script | Copy into | On master (bug) | After a fix |
| --- | --- | --- | --- |
| `import_rewind_indirect_mapping.ksh` | `tests/zfs-tests/tests/functional/cli_root/zpool_import/` | `zpool import -T` hangs, `dmesg` shows `PANIC at vdev_indirect_mapping.c:{345,350,360}:vdev_indirect_mapping_open()` (or `vdev_indirect_births.c:{105,111}`); FAIL | every import returns; PASS |
| `failmode_vdev_detach.ksh` | `tests/zfs-tests/tests/functional/failmode/` | `zpool status` of the other pool blocks; FAIL "holds spa_namespace_lock"; the VM needs a reboot | `zpool detach` returns non-zero within 60 s, pool SUSPENDED; PASS |

## Commands (VM 107, as root, from the built tree)

```sh
Z=<the VM's built source tree, upstream master>
SV=<path where this zts/ directory was copied to on the VM>
cd "$Z"
git log -1 --oneline             # record the exact build
cp "$SV/import_rewind_indirect_mapping.ksh" \
    tests/zfs-tests/tests/functional/cli_root/zpool_import/
cp "$SV/failmode_vdev_detach.ksh" tests/zfs-tests/tests/functional/failmode/
chmod +x tests/zfs-tests/tests/functional/cli_root/zpool_import/import_rewind_indirect_mapping.ksh \
    tests/zfs-tests/tests/functional/failmode/failmode_vdev_detach.ksh
./scripts/zfs.sh -r
cat /sys/module/spl/parameters/spl_panic_halt   # 0 keeps the box alive on VERIFY
dmesg -C

# 1. Rewind import across a removal (can take several minutes)
./scripts/zfs-tests.sh -v -t \
    "$Z/tests/zfs-tests/tests/functional/cli_root/zpool_import/import_rewind_indirect_mapping.ksh" \
    > /var/tmp/rewind.log 2>&1 || true
grep -E 'burst|txg [0-9]+:|txgs tried|PASS|FAIL|PANIC' /var/tmp/rewind.log
dmesg | grep -A25 'PANIC at vdev_indirect' | head -40
# reboot here if it FAILed, then reload with ./scripts/zfs.sh

# 2. Detach on a pool that suspends during the detach's own sync
./scripts/zfs-tests.sh -v -t \
    "$Z/tests/zfs-tests/tests/functional/failmode/failmode_vdev_detach.ksh" \
    > /var/tmp/detach.log 2>&1 || true
grep -E 'state:|returned|blocked|PASS|FAIL' /var/tmp/detach.log
cat /proc/*/stack 2>/dev/null | grep -c spa_vdev_config_exit || true
for p in $(pgrep -x zpool); do echo "== $p"; cat /proc/$p/stack; done
# reboot after a FAIL: zpool clear and zinject -c are blocked too
```

On a hang in test 2, the expected stacks are: `zpool detach` in
`txg_wait_synced <- spa_vdev_config_exit <- spa_vdev_exit <-
spa_vdev_detach`, and `zpool status` in `mutex_lock <- spa_open_common`.

## How to read the outcome of test 1

- **FAIL with a PANIC line**: reproduced. The import hangs holding
  `spa_namespace_lock`, because SPL's VERIFY parks the thread.
- **PASS, "failed cleanly" > 0**: no panic for those txgs. A clean failure
  can also come from something other than an unreadable mapping (for example
  the old txg's MOS was overwritten elsewhere), so read
  `zpool import`'s message in the log.
- **PASS, "no txg had an unreadable mapping"**: inconclusive. The freed
  blocks were not reused. Rerun it, or raise `BURSTS` and the churn loop.
- **log_unsupported**: the removal finished within the first 0.3 s burst.
  Raise the `randwritecomp` count.

zinject is not used in test 1. A handler holds `spa_inject_ref`
(module/zfs/zio_inject.c:1035), so `zpool export` returns EBUSY
(module/zfs/spa.c:8009), and no handler survives the export to act on the
reads done during the import. Overwriting the mapping block with `dd`
would be deterministic, but it was ruled out as hand corruption.

Test 2 uses the same zinject recipe as `failmode/failmode_vdev_state.ksh`
(b098c60ee6): `-e io -T write` and `-e nxio -T probe` on both sides, with
`zfs_txg_timeout` raised to 600 s.
