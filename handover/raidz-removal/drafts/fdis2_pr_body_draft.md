### Motivation and Context
Closes #19439.

A feature enabled by mistake, by `zpool upgrade` or by `zpool create` without `-d` or a compatibility file, can only be taken back by creating the pool again, even while nothing uses it, and `zpool status` keeps reporting features enabled outside the pool's `compatibility` property.

This follows @amotin's comment on #19439: no per-feature flag. A review of every activation path found features that are checked at one time and activated in an arbitrary later txg: a dataset property selecting a checksum or `recordsize`, `longname`, a clone or rewrite in progress, the user and project accounting upgrade. Waiting for a txg to sync, as suggested there, does not cover those. So instead of an asynchronous "disabling" state, whatever activates a disabled feature enables it again first.

### Description
- `zpool set feature@<name>=disabled` on an existing pool disables an enabled feature whose reference count is 0 and on which no enabled feature depends, directly or through other dependencies. A sync task checks this in syncing context and removes what `feature_enable_sync()` wrote: the entry in `features_for_read` or `features_for_write`, the description, and the entry in `feature_enabled_txg` with its reference. The feature then reads as never enabled, so the on-disk format does not change. Features that become active when enabled, and `enabled_txg`, never drop to 0 and are refused with `EBUSY` like any active feature.
- `spa_feature_incr()` of a feature that is not enabled now enables it, with its dependencies, instead of failing a VERIFY, and records it in the pool history like an explicit enable. It also runs on `dp_sync_taskq` threads, so a new `spa_feat_enable_lock` serializes these enables with each other and with the disable sync task.
- Four `ASSERT(spa_feature_is_enabled(...))` right before a feature is referenced no longer hold and are removed: `spa_activate_allocation_classes()`, `vdev_indirect_sync_obsolete()`, and the two block clone ZIL replay functions. For each, the reference count tracks all of the feature's on-disk state, so a feature with that state is never disabled; the paths only meet a disabled feature when they are about to create that state, and the increment then enables it.
- libzfs passes `ZPOOL_FEATURE_DISABLE` as the value of the feature property instead of refusing `disabled` on an existing pool, and explains `EBUSY`. A kernel without this change refuses the value with `EINVAL`. `zpool import -o` and `zpool split -o` still refuse `feature@<name>=disabled`.
- `zpool-features(7)` and `zpoolprops(7)` describe when a feature can be disabled and that a later use enables it again.
- `cli_root/zpool_set/zpool_set_features` checked that `async_destroy`, `large_dnode` and the others it lists cannot be disabled; it now checks that features active since creation cannot be disabled.
- No libzfs ABI change.

### How Has This Been Tested?
Linux x86_64, userspace only (libzpool, file vdevs), debug build. A probe calls `spa_feature_disable()` and `spa_prop_set()` in nine scenarios, all passing: disabling and enabling again across export and import and in the history; `raidz_expansion`, `draid` and its dependent `draid_failure_domains`; features active since creation (`lz4_compress`, `spacemap_v2`, `enabled_txg`, `hole_birth`, `embedded_data`, `head_errlog`) refused with `EBUSY`; a transitive dependency refused on a pool with `encryption` and without `bookmark_v2`, as errata #4 pools have; `EROFS` on a read-only import; six datasets with `checksum=sha512` written in one txg after `sha512` was disabled, which enable it again with a reference count of 6; and four `dp_sync_taskq` threads referencing the same disabled feature at once, which panics in `feature_enable_sync()` (`zap_add()` returning `EEXIST`) without the new lock and passes 25 rounds out of 25 with it. `zdb -bcc` finds no leaks on any of the pools.

`make unit`, `cstyle`, `mancheck`, `testscheck`, `spdxcheck`, `vcscheck` and `commitcheck` pass, and so do GCC and Clang builds without `--enable-debug`, with `-Werror`, of the changed files that build in userspace. `zfs_vnops.c` and `zvol.c` were only compiled by the CI. The CI of my fork: RESULT.

Not run: the ZFS Test Suite on my machines, the new and changed tests included.

### Types of Changes
- [ ] Bug fix (non-breaking change which fixes an issue)
- [x] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [x] Documentation (a change to man pages or other documentation)

### Checklist
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [x] I have updated the documentation accordingly.
- [ ] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
