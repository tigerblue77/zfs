<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
`zfs rewrite -W` (#19376) skips blocks whose allocated size would not change, so that a rewrite after a RAID-Z expansion can be run again without rewriting the same blocks twice. It also skips blocks that a rewrite would change in other ways. After `zfs set compression=zstd`, `checksum=sha256`, `copies=2`, `dedup=on` or a larger `special_small_blocks`, the blocks written before keep their allocated size, so `-W` skips them, although rewriting them is how the new value applies to them. The man page warns about compression, but in practice `-W` cannot be used once a property has changed, which is also when a rewrite is most often wanted. `-W` is in no release yet, so this changes what it skips before anyone depends on it.

### Description
A block is now skipped only when a rewrite would leave it as it is:

- Its checksum, compression algorithm, number of copies and dedup bit are those that `dmu_write_policy()` gives a write of the file now. The policy is computed once per range, and only with `-W`.
- Every DVA is in the allocation class that a write would choose, and has the allocated size that its physical size would get there in the txg of the rewrite (the check of #19376). The class choice of `spa_preferred_class()` is moved, unchanged, into `spa_class_select()`, which both call; `spa_preferred_class()` keeps the gang child and fallback handling. This also covers `special_small_blocks` raised or lowered, and a special vdev added after the data was written.
- Gang blocks, and blocks on an indirect vdev, on a vdev with `allocating=off`, or on a vdev being removed, are always rewritten, since a rewrite would move them.

Two limits, documented in `zfs-rewrite(8)`:

- A block stored uncompressed may be one that compression did not shrink: such a block is not rewritten only to be compressed, or `-W` would rewrite every incompressible block of a compressed dataset each time. Blocks written while compression was off are therefore not compressed by `-W`; a rewrite without it does that.
- The compression level is not recorded in the block pointer, so a change of the `zstd` or `gzip` level alone is not seen.

`bp_asize_is_current()` becomes `bp_rewrite_is_current()`, which takes the write properties, and the flag becomes `ZFS_REWRITE_SKIP_CURRENT` (same value, 0x8, same `-W` option). Neither was in a release. No library ABI change.

Related open work: once #19367 (rewrite of volumes) is merged, its `dmu_rewrite_skip()` gets the same check, so that `-W` behaves the same on volumes. The `-A` option proposed in #19419 (rewrite only the blocks on non-allocating vdevs) composes with this: `-W` never skips such a block.

### How Has This Been Tested?
Linux 6.12.112, debug build, in a QEMU (TCG) VM:

- New test `zfs_rewrite_skip_current`: a pool of one file vdev and a special file vdev, `compression=lz4`, with a compressible file Z and an incompressible file R (8 x 128K blocks each). `-W` first moves nothing. Then one property is changed at a time, and `-W` moves the blocks of the affected files, and only those, and a second `-W` moves nothing: `compression=zstd` and `compression=off` move Z only (R is stored uncompressed), `checksum=sha256`, `copies=2` and `dedup=on` move both, and `special_small_blocks=128K` moves both onto the special vdev. Contents and a scrub are clean.
- The same test fails on master at `compression=zstd`, where `-W` leaves Z in place.
- Existing tests in `cli_root/zfs_rewrite`, including `zfs_rewrite_skip_width` for the RAID-Z expansion case.

All pass, with no warning in the kernel log. (The directory's `cleanup.ksh` fails in this VM after every test, because its root file system is read-only.) A release (non-debug) build gives no warning in the changed files.

The change was also tested merged with the other open `zfs rewrite` PRs (#19367, #19370, #19375): all eight `cli_root/zfs_rewrite` tests and `raidz/raidz_expand_rewrite` pass, and on a volume, `-W` after `compression=zstd` moves every block and a second `-W` none.

The full ZTS on Debian 13 (x86_64), run with this change and with master on the same machine: no test fails with the change and passes on master, apart from three that pass when run again (`import_rewind_config_changed`, `reservation_016_pos`, and `zinject_args`, which passed in three of six reruns); none of them uses `zfs rewrite`. The zfs-qemu workflow also passes on this commit in my fork, on all its platforms, FreeBSD included.

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [x] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [x] Documentation (a change to man pages or other documentation)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [x] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [x] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
