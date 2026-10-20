### Motivation and Context
A crash or power loss during `zpool reguid` can leave a pool that no longer imports by any means: `zpool import`, `zpool import -F`, `zpool import -FX` and the open from the cache file at boot all fail, and `zfs_dbgmsg` shows, for every leaf:

```
vdev_validate: vdev label pool_guid doesn't match config (<new guid> != <old guid>)
spa_load(..., config trusted): FAILED: cannot open vdev tree after invalidating some vdevs
```

The txg that writes the new guid writes labels 0 and 2 of every device before its uberblocks. A crash in between leaves the new guid in those labels, and the old one in labels 1 and 3, in the uberblocks and in the MOS config. Once the import has reloaded the pool with the MOS config, `vdev_validate()` compares the old guid with the label it reads, and it reads the newest one.

It is meant to read the newest label within the last synced txg, and to read the newest of all only during an extreme rewind or when the config lock is not held as writer, for callers that race with `spa_sync()` (OpenZFS 9187, #7529). Since #11470 ("Parallelize vdev_validate", OpenZFS 2.1), the children of a vdev are validated in taskq threads, which never hold the lock: `spa_config_held()` is false there, so the bound has not applied to any leaf since.

When only some devices got the new labels before the crash, `zpool import` lists two pools with the same name, one per guid, and neither imports.

I found this while looking for crash windows in label updates, by stopping a userspace (libzpool) `spa_change_guid()` in the debugger at the uberblock writes of its txg and killing it. I found no existing report.

### Description
- `module/zfs/vdev.c`: `vdev_validate()` chooses the txg in the thread that calls it, which holds the lock, and hands it to the children through the new `vdev_validate_txg`. A child validated in its own thread uses that txg. The calls that do not hold the lock as writer still read the newest label, as before.
- Labels are ordered by txg only within one pool. When the label within the bound names another pool, as the label a device split off into a new pool keeps from the pool it came from, or when no label is within the bound, as after a rewind to before the labels of a vdev were last written, `vdev_validate()` checks the newest label, as it does today. Without the first case, a crash in `zpool split` right before the second uberblock of the new pool leaves the new pool unimportable, while it imports on master: its own label is newer than its uberblock, and its txgs restart below those of the pool it came from. Without the second, a rewind past the last label update of one top-level vdev fails: `vdev_uberblock_load()` already requires a label within the bound, but only on the vdev holding the uberblock.
- New test `cli_root/zpool_reguid/zpool_reguid_003_pos`: it exports a pool on file vdevs and copies the devices, imports the pool, changes its guid and exports it, then copies back labels 1 and 3 and every uberblock ring with `dd`. This is what a crash right before the uberblocks of the reguid txg leaves. The pool must then import, healthy, with its old guid, and scrub without errors.

### How Has This Been Tested?
Linux x86_64, GCC, debug build, userspace only (libzpool, file vdevs, a pool of a two-way mirror and a single disk). States obtained by killing the reguid in `gdb` at a breakpoint in its txg, or built with `dd` as the new test does:

| state | master | this change |
| --- | --- | --- |
| killed before the uberblocks: scan import, `-F`, `-FX`, cache file | fails (`ENXIO`) | imports, old guid |
| killed before labels 1 and 3, after the uberblocks | imports, new guid | same |
| built with `dd` as in the new test | fails (`ENXIO`) | imports, old guid |
| new labels on the mirror only: import by name | `more than one matching pool` | same |
| new labels on the mirror only: import of either guid | fails (`ENXIO`) | imports, old guid; the next scan finds one pool |
| every label of one top-level vdev newer than every uberblock | imports | imports (fails without the newest-label fallback) |
| killed before each of the 6 label and uberblock writes from the reguid txg to the end of the export | fails before the uberblocks of the reguid txg, imports at the 5 others | imports at all 6 |
| killed before each of the 18 label and uberblock writes of `zpool split` (two two-way mirrors), every pool found imported by guid | every pool found imports | same at every point (without the other-pool fallback, the new pool fails at one) |
| killed before each of the 105 label and uberblock writes of `zpool attach`, `detach`, `replace`, `add`, `online -e` and a device removal, to the end of the export | every pool imports; at the uberblock of a detach txg, and of the detach that ends a replace, it imports DEGRADED: the remaining disk's newest label already describes the detach | same at every point, except those two, where the pool imports healthy |

The last row stands for a rewind past a label update. On master, the same state with every label of every vdev newer than every uberblock fails earlier, in `vdev_uberblock_load()` (`label config unavailable`), and still does.

A crash-consistency fuzzer (random pool operations, the worker killed at a random time, then a scan import, a scrub and `zdb -b`), 100 runs on master and twice 100 runs on this change: 4 pools could not be imported on master, 4 and 7 with this change. The crash is timed, so the failing runs differ. Each of the 11 states left with this change fails the same way with master's code. Ten import once the fixes for the other crash bugs the fuzzer found are applied (#REWIND_PR, #SCAN_PR); the last is a log removal interrupted after the labels were erased, which #LOG_PR prevents rather than repairs. No run involved a reguid: the fuzzer does not run one.

`make unit`, `make commitcheck`, `cstyle`, `shellcheck`, `mancheck`, `testscheck`, two 4-minute ztest runs, and a build of `vdev.c` without `--enable-debug` with `-Werror` pass. ztest ran with #ZTEST_PR applied: without it, on a host where the zfs module is not loaded, it stops at random with "failed to find config for pool 'ztest'", as it did twice here. The CI of my fork: RESULT.

On a Linux x86_64 VM with the kernel module (debug build, Debian 13, kernel 6.12.111): `zpool_reguid` passes with this change, the new test included, which fails on master with "cannot open vdev tree after invalidating some vdevs". In `zpool_import`, the same 4 tests fail with and without this change: `zpool_import_012_pos`, `zpool_import_errata3` and `zpool_import_errata4`, for lack of an NFS server and of zvol links on that VM, and `import_rewind_config_changed`, whose rewind past an attach found the MOS of the old txg overwritten, as its disclaimer allows.

Not run: FreeBSD (left to the CI).

### Types of Changes
- [x] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Checklist
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [ ] I have updated the documentation accordingly.
- [ ] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
