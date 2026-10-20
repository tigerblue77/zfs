### Motivation and Context
A crash or power loss during `zpool detach` can leave a pool that does not import once its devices have changed names, as `/dev/sdX` names do across a reboot. `zfs_dbgmsg` shows:

```
vdev_copy_path: vdev type mismatch: file != mirror
spa_load(..., config untrusted): vdev_copy_path_strict failed, falling back to vdev_copy_path_relaxed
spa_load(..., config trusted): vdev tree has 1 missing top-level vdevs.
```

and the import fails with `ENXIO`. It imports again once the devices are back under their old names.

When a detach leaves a mirror, replacing or spare vdev with one child, `vdev_remove_parent()` puts that child in its place as the top-level vdev and gives it the guid of the parent, so that the top-level vdev keeps its guid. The txg that does so writes labels 0 and 2 of the device before its uberblocks. A crash in between leaves a device whose newest labels describe it as the top-level vdev under the guid of the mirror, while labels 1 and 3, the uberblocks and the MOS config still describe the mirror and the device as its child, under its own guid.

The scan builds the config of each top-level vdev from its newest label. Once the import has the MOS config, `vdev_copy_path_relaxed()` looks each leaf of it up in the scanned config by guid, to give it the path the device was found at. The old guid of the remaining device is not there, so the leaf keeps the path recorded in the MOS config. The labels of the detached device are erased before the txg, so the scan does not find that one either, and no child of the mirror opens. The same happens in the detach that completes a `zpool replace`.

In place, the same state imports, but DEGRADED: `vdev_validate()` reads the newest label of the remaining device and rejects it for its new guid, while it accepts the detached one, whose erased labels carry txg 0.

`zpool split` has the same window, and handles it in part: the label of the device left in the original pool carries its former guid as `ZPOOL_CONFIG_ORIG_GUID`, which `vdev_validate()` accepts in place of its guid. A detach does not write it, and nothing uses it to find the paths, so a split interrupted at that point fails the same way once the devices are renamed.

I found this while looking for crash windows in label updates, by stopping a userspace (libzpool) detach in the debugger at the uberblock writes of its txg, killing it, and importing the devices from another directory. I found no existing report.

### Description
- `module/zfs/vdev_label.c`: `vdev_config_generate()` writes `ZPOOL_CONFIG_ORIG_GUID` for every leaf that took the guid of its parent, not only for one being split, and only in labels: nothing reads it from the MOS config, and leaving it out keeps it from being read back into the vdev tree once the change has synced.
- `module/zfs/vdev.c`: `vdev_alloc()` reads the key from a config being loaded, and `vdev_copy_path_search()` looks a leaf of the MOS config up by it when its guid is not in the scanned config. `vdev_splitting`, which only gated the key, goes.
- The key is not new on disk: `zpool split` has written it in labels for years, and every reader since accepts it in `vdev_validate()` and ignores it elsewhere. No feature flag is needed, and code without this change imports the states this change writes at least as well as before: in place it even imports them healthy.
- I considered erasing the labels of the detached device only once the detach has synced, as is done for log devices. It does not help on its own: the scan keeps the newest label of each top-level vdev, which leaves the detached device out of the config even when its labels are intact. I checked it by putting back the labels of the detached device from a copy taken right before they were erased: the import still fails with `ENXIO`.
- New test `cli_root/zpool_detach/zpool_detach_002_pos`: it exports a mirror on file vdevs and copies the devices, imports the pool, detaches one side and exports it, then copies back labels 1 and 3 and the uberblock rings of the remaining device with `dd`. This is what a crash right before the uberblocks of the detach txg leaves. It then renames the devices and moves them to another directory. The pool must import from there, with the remaining device online, and scrub without errors.

### How Has This Been Tested?
Linux x86_64, GCC, debug build, userspace only (libzpool, file vdevs). "Renamed": every device copied to another directory under another name, and the directory recorded in the pool hidden.

States killed in `gdb` before each label and uberblock write of the operation, from its start to the end of the export; every pool the scan finds imported by guid, in place and renamed, and the pool opened from the cache file. In each column, the same code writes the state and imports it:

| state | master | this change |
| --- | --- | --- |
| `zpool detach`, mirror and a single disk, before the uberblocks of the detach txg: renamed | fails (`ENXIO`) | imports, DEGRADED: the detached disk is missing |
| same, in place, and from the cache file | imports DEGRADED | imports healthy |
| `zpool replace`, before the uberblocks of the detach txg that ends it: renamed | fails (`ENXIO`) | imports, DEGRADED: the replaced disk is missing |
| same, in place, and from the cache file | imports DEGRADED | imports healthy |
| the 10 other points of the detach, the 23 others of the replace, the 18 of `zpool attach` | | same as master at every point |
| built with `dd` as in the new test, 3 runs: renamed | fails (`ENXIO`) | imports DEGRADED, scrubs without errors |
| same, in place | imports DEGRADED | imports healthy, scrubs without errors |

Across versions: master's code imports the states this change writes healthy in place, as `vdev_validate()` already accepts the former guid, and still fails renamed. This change imports the states master's code writes exactly as master does, at every point of the three operations: their labels have no former guid. So it does not repair a pool left by older code; moving the devices back to their old paths still does.

Killed before the first label of the detach txg, the pool imports DEGRADED renamed, with and without this change: the labels of the detached disk are already erased, so the scan does not find it. Erasing them only once the txg has synced should bring that disk back there (not tested), but not at the uberblock, as said above.

`zpool split` (two two-way mirrors), killed before each of its 18 label and uberblock writes, every pool found imported by guid, in place and renamed: same as master at every point but one. At the uberblock of the split txg of the original pool, renamed, the original pool fails on master (`ENXIO`) and imports with this change, without the two disks split off. That holds for states written by master's code too, since a split already writes the former guid in labels.

`make unit`, `make commitcheck`, `cstyle`, `shellcheck`, `checkbashisms`, `mancheck`, `testscheck`, `vcscheck`, a 4-minute ztest run, and a build of `vdev.c` and `vdev_label.c` without `--enable-debug` with `-Werror` pass. A second ztest run stopped with "failed to find config for pool 'ztest'", which #ZTEST_PR fixes: it happens at random on a host where the zfs module is not loaded. The CI of my fork: RESULT.

On a Linux x86_64 VM with the kernel module (debug build, Debian 13, kernel 6.12.111): `zpool_detach` passes with this change, the new test included, which fails on master ("no such pool or dataset"), and so does `replacement/detach`. In `zpool_import`, the 3 tests that fail on master fail with this change too, for lack of an NFS server and of zvol links on that VM.

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
