### Motivation and Context
Since #19373, an empty top-level vdev can be removed from any pool, but a vdev that still holds data can only be evacuated from a pool whose top-level vdevs all have the same sector size and none of which is raidz or dRAID. Removing such a vdev from another pool fails with:

```
cannot remove raidz1-1: invalid config; all top-level vdevs must have the same sector size and not be raidz.
```

That reads as if the pool were misconfigured, while what stops the removal is that this vdev is not empty. Users of `zpool rewrite` (#19419), who empty a vdev before removing it, will hit it whenever a snapshot still holds blocks on the vdev.

### Description
- `module/zfs/vdev_removal.c`: when the evacuation checks refuse the pool (`EINVAL`) and the vdev holds data, `spa_vdev_remove_top()` returns the new `ZFS_ERR_VDEV_NOT_EMPTY`.
- `lib/libzfs/libzfs_pool.c`: `zpool remove` then says how much is still allocated on the vdev: `cannot remove <vdev>: it still holds <size>; data can be evacuated only if all top-level vdevs have the same sector size and none is raidz or dRAID.`
- `include/sys/fs/zfs.h`, `contrib/pyzfs/libzfs_core/_constants.py`: the new error, appended to `zfs_errno_t`, which is not part of the checked library ABI.
- `remove_raidz_busy` expects the new message.

`EINVAL` keeps its old message for the other case that returns it, the last allocating vdev of the normal class.

### How Has This Been Tested?
Linux x86_64, GCC, debug build.

- A libzpool program (no kernel module) that removes vdevs holding data from pools with raidz vdevs, with and without `allocating=off`, gets `ZFS_ERR_VDEV_NOT_EMPTY` where it got `EINVAL`; with that expectation changed, its 34 scenarios (345 checks) pass.
- `make unit`, `make commitcheck`, and a build without `--enable-debug` with `-Werror` of the changed files pass.
- ZFS Test Suite on a Linux 6.12 VM, debug build, on a build that also carried the `zpool rewrite` series (#19419): `remove_empty_vdev`, `remove_raidz_empty`, `remove_raidz_busy` and `remove_raidz` pass. With a snapshot still holding blocks on an emptied special mirror, `zpool remove` exits 1 with `cannot remove mirror-1: it still holds 12.6M; data can be evacuated only if all top-level vdevs have the same sector size and none is raidz or dRAID.`
- The ZFS Test Suite in the CI of my fork: RESULT.

### Types of Changes
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [x] Code cleanup (non-breaking change which makes code smaller or more readable)
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
