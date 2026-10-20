### Motivation and Context
Depends on #DETACH_PR, whose first commit this branch carries.

With #DETACH_PR, a pool left by a crash during `zpool detach`, or the detach that completes a `zpool replace`, imports once its devices have changed names, as `/dev/sdX` names do across a reboot. It imports DEGRADED, though: the detached device shows as UNAVAIL under its old path. Its data is intact and the pool on disk still mirrors onto it; in place, the same state imports healthy, and moving the devices back to their old paths brings it back.

Two crash points leave this:
- right after `spa_vdev_detach()` erased the labels of the detached device with `vdev_label_init(VDEV_LABEL_REMOVE)`, before any label of the detach txg is written. The pool on disk is still the mirror, and every label of the remaining device describes it.
- right before the uberblocks of the detach txg. Labels 0 and 2 of the remaining device already describe it alone as the top-level vdev, under the guid of the mirror.

`VDEV_LABEL_REMOVE` zeroes nothing: it writes labels with txg 0 that still carry the pool and vdev guids. `vdev_validate()` accepts them, which is why the in-place import finds the device. The import scan skips them: `zpool_read_label()` does not count a label with txg 0, so the scan never records where the device is. At the first point, the scanned config names the leaf, from the labels of the remaining device, with its old path, and `fix_paths()` finds no device for it. At the second point the scanned config, built from the newest label of the remaining device, does not name the leaf at all, so `vdev_copy_path_relaxed()` has no path to give it and it keeps the one recorded in the MOS config.

I found this with the same crash points as #DETACH_PR, by stopping a userspace (libzpool) detach in the debugger at each label and uberblock write, killing it, and importing the devices from another directory. I found no existing report.

### Description
Two commits.

1. libzutil: use devices whose labels have txg 0 as last-resort paths.
   - `lib/libzutil/zutil_import.c`: the label checks that the aio and non-aio readers duplicated move into `label_unpack()`, which also notes the pool and vdev guids of the first label with txg 0 that names a pool. `zpool_read_label_txg0()` returns them; `zpool_read_label()` is a wrapper that returns what it did before, so `zdb`, `zpool labelclear` and the other callers see no change.
   - `lib/libzutil/os/{linux,freebsd}/zutil_import_os.c`: `zpool_open_func()` keeps those guids for a device that has no valid label. Both platforms have the same structure there and change the same way.
   - `zpool_find_import_impl()` puts such devices on a list of their own, after the same exclusive open test that prunes multipath components. They are never added as pool members, so the list of pools to import does not change. `fix_paths()` uses them as a last resort, after the existing lookups, and only for a leaf of the same pool.
2. Pass the kernel the paths of leaves the import config does not name.
   - `get_configs()` adds `ZPOOL_CONFIG_LEAF_PATHS` ("leaf_paths") to a config when the scan found devices of the pool, with valid labels or with labels of txg 0, whose guid its vdev tree does not name: an nvlist of paths keyed by vdev guid in decimal.
   - `spa_ld_trusted_config()`, when `vdev_copy_path_strict()` fails, calls `vdev_copy_leaf_paths()` after `vdev_copy_path_relaxed()`. A concrete leaf of the MOS config that the untrusted config does not name, by guid or by the former guid from #DETACH_PR, takes the path listed for its guid. The device still has to pass `vdev_validate()`.
   - Compatibility: the key is only read during an import, and the config is regenerated from the MOS config right after, so it is never stored on disk or in the cache file. `zpool import` passes it with the tryimport only, since the config the tryimport returns already has the path. An older kernel ignores the key; a kernel given no key does exactly what it did.
   - `best_name()` now picks the best of several names when given no path to prefer, instead of the first. `fix_paths()` only does so for leaves without a path, such as distributed spares, for which no device is found anyway.

I considered erasing the labels of the detached device only once the detach has synced. That covers the first crash point for pools written by new code only, and not the second one on its own (#DETACH_PR explains why). Reading the erased labels at import covers both, for pools written by any version.

Tests:
- New `cli_root/zpool_detach/zpool_detach_003_pos`: exports a mirror on file vdevs and copies the devices, imports the pool, detaches one side and exports it. It then copies back every label and uberblock ring of the remaining device with `dd`, which leaves the state of the first crash point, renames the devices and moves them to another directory. The pool must import from there healthy, with both devices online, and scrub without errors. It fails without the first commit (the detached device is UNAVAIL).
- `zpool_detach_002_pos` from #DETACH_PR builds the state of the second crash point the same way. It now also expects the pool to be healthy, with the detached device online. That fails with the first commit alone and passes with the second.

### How Has This Been Tested?
Linux x86_64, GCC, debug build, userspace only (libzpool, file vdevs). "Renamed": every device copied to another directory under another name, and the directory recorded in the pool hidden.

States killed in `gdb` before each label and uberblock write of the operation, from its start to the end of the export; every pool the scan finds imported by guid, in place and renamed, both directly (as `zdb -e` does, without a tryimport) and through a tryimport whose config is then imported (as `zpool import` does), and the pool opened from the cache file. In each column, the same code writes the state and imports it. #DETACH_PR is the base:

| state | #DETACH_PR | this change | this change and #VTXG_PR |
| --- | --- | --- | --- |
| `zpool detach`, mirror and a single disk, before the first label of the detach txg: renamed | DEGRADED: detached disk missing | healthy | healthy |
| same, before the uberblocks of the detach txg: renamed | DEGRADED: detached disk missing | healthy | healthy |
| both points in place, and from the cache file | healthy | healthy | healthy |
| `zpool replace`, the same two points of the detach txg that ends it | as for the detach | healthy | healthy |
| the 10 other points of the detach, the 22 others of the replace | healthy | healthy | healthy |
| the 18 points of `zpool attach` | healthy (earlier run, direct import only) | healthy | healthy |

Direct import and tryimport gave the same result at every point. Opening from the cache file fails with `ENOENT` once the export has removed the pool from it, in every column alike.

States written by code without #DETACH_PR, whose labels carry no former guid, at the two points of the detach txg of `zpool detach` and `zpool replace` (the other points are as above):

| state | #DETACH_PR (earlier run, direct import only) | this change | this change and #VTXG_PR |
| --- | --- | --- | --- |
| before the first label: renamed | DEGRADED: detached disk missing | healthy | healthy |
| before the uberblocks: renamed | fails (`ENXIO`) | DEGRADED: remaining disk missing | DEGRADED: remaining disk missing |
| before the uberblocks: in place, and from the cache file | DEGRADED | DEGRADED | healthy |

At the uberblock point, the newest labels of the remaining disk name it only by the guid of the mirror, so nothing gives the path of the leaf the MOS config knows it as; moving the devices back to their old paths still brings it back. The detached disk is found there now.

The steps of the two tests run with libzpool, as above:

| test | #DETACH_PR | first commit | both commits |
| --- | --- | --- | --- |
| `zpool_detach_003_pos` state, renamed | DEGRADED: detached disk missing | healthy, scrub clean | healthy, scrub clean |
| `zpool_detach_002_pos` state, renamed | DEGRADED: detached disk missing | DEGRADED: detached disk missing | healthy, scrub clean |
| both, in place | healthy | not run | healthy |

Comparing labels with `zdb -lu`, the state built with `dd` for `zpool_detach_003_pos` has the shape of the state killed before the first label of the detach txg: the remaining device has four identical labels describing the mirror, and the detached device has four labels with txg 0 naming the pool, the mirror and its own guid, and no valid uberblock. The labels of the remaining device say exported rather than active.

A crash-consistency fuzzer (random pool operations, the worker killed at a random time, then a scan import with the devices in place or moved, a scrub and `zdb -b`), on a tree that also carries #DETACH_PR, #VTXG_PR and my other pending import fixes: 300 runs, no failure. Without this change, the same 300 seeds gave one failure, a crash during the detach that completes a `zpool replace`, with the devices moved: the pool imports DEGRADED, the replaced disk missing. Master fails the same way on that image, and this change imports it healthy. So do the three images of that kind an earlier campaign had left, which import DEGRADED without this change.

Each commit builds on its own with `-Werror`. `make unit`, `make commitcheck` (both commits), `cstyle`, `shellcheck`, `checkbashisms`, `mancheck`, `testscheck`, `vcscheck`, `spdxcheck`, and a build of `vdev.c`, `spa.c`, `zutil_import.c` and the Linux `zutil_import_os.c` without debug with `-Werror` pass. The CI of my fork: RESULT.

Not run: the ZFS Test Suite with the kernel module, FreeBSD (the FreeBSD `zutil_import_os.c` was not compiled here; left to the CI), `make checkabi` (no ABI change is intended: the new function is internal to libzutil).

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
